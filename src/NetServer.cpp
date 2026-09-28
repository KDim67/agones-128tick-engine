#include "engine/NetServer.hpp"
#include <cstring>
#include <cstdio>

namespace engine {

NetServer::NetServer() noexcept {
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        sessions_[i].slot_id = static_cast<uint32_t>(i);
        sessions_[i].state = SessionState::Disconnected;
        sessions_[i].peer = nullptr;
    }
}

NetServer::~NetServer() {
    Shutdown();
}

bool NetServer::Initialize(uint16_t bind_port) noexcept {
    if (enet_initialize() != 0) {
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = bind_port;

    // Allocate non-blocking host: 2 channels (0: unsequenced inputs/snapshots, 1: reliable control)
    host_ = enet_host_create(&address, kMaxPlayers, 2, 0, 0);
    return host_ != nullptr;
}

void NetServer::Shutdown() noexcept {
    if (!host_) {
        return;
    }

    for (auto& session : sessions_) {
        if (session.state != SessionState::Disconnected && session.peer) {
            enet_peer_disconnect_now(session.peer, 0);
            session.state = SessionState::Disconnected;
            session.peer = nullptr;
        }
    }

    enet_host_destroy(host_);
    host_ = nullptr;
    enet_deinitialize();
}

void NetServer::PollEvents(uint32_t current_tick) noexcept {
    if (!host_) {
        return;
    }

    current_tick_ = current_tick;

    // Reset per-tick ingress rate limiter for connected sessions
    for (auto& session : sessions_) {
        session.inputs_this_tick = 0;
    }

    ENetEvent event;
    // Drain inbound OS socket buffers without blocking
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                HandleConnect(event.peer);
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                HandlePacket(event.peer, event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                HandleDisconnect(event.peer);
                break;

            case ENET_EVENT_TYPE_NONE:
            default:
                break;
        }
    }
}

void NetServer::CheckTimeouts() noexcept {
    const auto now = std::chrono::steady_clock::now();

    for (auto& session : sessions_) {
        if (session.state == SessionState::Connected && session.peer) {
            const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - session.last_activity).count();
            if (idle_ms >= static_cast<int64_t>(kClientTimeoutMs)) {
                enet_peer_disconnect_later(session.peer, 0);
                HandleDisconnect(session.peer);
            }
        }
    }
}

void NetServer::HandleConnect(ENetPeer* peer) noexcept {
    for (auto& session : sessions_) {
        if (session.state == SessionState::Disconnected) {
            session.state = SessionState::Connecting;
            session.peer = peer;
            session.input_queue.count = 0;
            session.input_queue.read_idx = 0;
            session.input_queue.write_idx = 0;
            session.last_activity = std::chrono::steady_clock::now();
            session.last_input_sequence = 0;
            session.last_ack_tick = 0;
            peer->data = &session;
            return;
        }
    }
    // Reject connection if slot pool is exhausted
    enet_peer_reset(peer);
}

void NetServer::HandleDisconnect(ENetPeer* peer) noexcept {
    if (peer->data) {
        auto* session = static_cast<ClientSession*>(peer->data);
        if (session->state == SessionState::Connected && active_players_ > 0) {
            --active_players_;
        }
        session->state = SessionState::Disconnected;
        session->peer = nullptr;
        peer->data = nullptr;
    }
}

void NetServer::HandlePacket(ENetPeer* peer, const uint8_t* data, size_t length) noexcept {
    if (length < sizeof(PacketHeader)) {
        return;
    }

    PacketHeader header;
    std::memcpy(&header, data, sizeof(PacketHeader));

    if (header.protocol_version != kProtocolVersion) {
        return;
    }

    auto* session = static_cast<ClientSession*>(peer->data);
    if (!session || session->state == SessionState::Disconnected) {
        return;
    }

    const auto packet_type = static_cast<PacketType>(header.type);

    if (packet_type == PacketType::ConnectRequest) {
        if (header.payload_bytes != sizeof(ConnectRequestPayload) || length < (sizeof(PacketHeader) + sizeof(ConnectRequestPayload))) {
            return;
        }

        ConnectRequestPayload request;
        std::memcpy(&request, data + sizeof(PacketHeader), sizeof(ConnectRequestPayload));

        if (session->state == SessionState::Connecting) {
            session->state = SessionState::Connected;
            session->last_activity = std::chrono::steady_clock::now();
            ++active_players_;

            // Acknowledge connection with assigned slot and initial tick anchor
            ConnectAckPayload ack{
                .assigned_slot = session->slot_id,
                .server_tick = current_tick_,
                .tick_rate = static_cast<uint16_t>(kTickRate)
            };
            SendPacket(session->slot_id, PacketType::ConnectAck, &ack, sizeof(ConnectAckPayload), true);
        }
        return;
    }

    if (packet_type == PacketType::ClientInput) {
        if (session->state != SessionState::Connected) {
            return;
        }

        if (header.payload_bytes != sizeof(ClientInputPayload) || length < (sizeof(PacketHeader) + sizeof(ClientInputPayload))) {
            return;
        }

        // Enforce ingress rate limit per tick
        if (session->inputs_this_tick >= kMaxInputsPerTick) {
            return;
        }

        ClientInputPayload input;
        std::memcpy(&input, data + sizeof(PacketHeader), sizeof(ClientInputPayload));

        // Sanitize view angles against exploitation
        if (!ValidateAndClampViewAngles(input.view_pitch, input.view_yaw)) {
            return;
        }

        // Reject stale out-of-order sequence frames
        if (input.sequence > session->last_input_sequence) {
            session->last_input_sequence = input.sequence;
            session->last_ack_tick = input.ack_tick;
            session->last_activity = std::chrono::steady_clock::now();
            ++session->inputs_this_tick;
            session->input_queue.Push(input);
        }
        return;
    }

    if (packet_type == PacketType::Disconnect) {
        HandleDisconnect(peer);
    }
}

void NetServer::SendPacket(uint32_t slot_id, PacketType type, const void* payload, size_t payload_bytes, bool reliable) noexcept {
    if (slot_id >= kMaxPlayers || !host_) {
        return;
    }

    auto& session = sessions_[slot_id];
    if (session.state == SessionState::Disconnected || !session.peer) {
        return;
    }

    PacketHeader header{
        .type = static_cast<uint8_t>(type),
        .protocol_version = kProtocolVersion,
        .payload_bytes = static_cast<uint16_t>(payload_bytes)
    };

    const size_t total_size = sizeof(PacketHeader) + payload_bytes;
    if (total_size > control_buffer_.size()) {
        return;
    }

    std::memcpy(control_buffer_.data(), &header, sizeof(PacketHeader));
    if (payload && payload_bytes > 0) {
        std::memcpy(control_buffer_.data() + sizeof(PacketHeader), payload, payload_bytes);
    }

    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    const enet_uint8 channel = reliable ? 1 : 0;

    ENetPacket* packet = enet_packet_create(control_buffer_.data(), total_size, flags);
    if (packet) {
        enet_peer_send(session.peer, channel, packet);
        enet_host_flush(host_);
    }
}

void NetServer::BroadcastSnapshot(const ServerSnapshotPayload& snapshot) noexcept {
    if (!host_ || active_players_ == 0) {
        return;
    }

    PacketHeader header{
        .type = static_cast<uint8_t>(PacketType::ServerSnapshot),
        .protocol_version = kProtocolVersion,
        .payload_bytes = sizeof(ServerSnapshotPayload)
    };

    std::memcpy(broadcast_buffer_.data(), &header, sizeof(PacketHeader));
    std::memcpy(broadcast_buffer_.data() + sizeof(PacketHeader), &snapshot, sizeof(ServerSnapshotPayload));

    const size_t total_size = sizeof(PacketHeader) + sizeof(ServerSnapshotPayload);

    // Unsequenced fire-and-forget payload for authoritative snapshot
    ENetPacket* packet = enet_packet_create(broadcast_buffer_.data(), total_size, ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet) {
        return;
    }

    for (const auto& session : sessions_) {
        if (session.state == SessionState::Connected && session.peer) {
            enet_peer_send(session.peer, 0, packet);
        }
    }

    enet_host_flush(host_);
}

} // namespace engine
