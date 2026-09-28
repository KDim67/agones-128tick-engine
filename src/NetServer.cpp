#include "engine/NetServer.hpp"
#include <cstring>
#include <cstdio>

namespace engine {

NetServer::NetServer() noexcept {
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        sessions_[i].slot_id = static_cast<uint32_t>(i);
        sessions_[i].is_active = false;
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
        if (session.is_active && session.peer) {
            enet_peer_disconnect_now(session.peer, 0);
            session.is_active = false;
            session.peer = nullptr;
        }
    }

    enet_host_destroy(host_);
    host_ = nullptr;
    enet_deinitialize();
}

void NetServer::PollEvents() noexcept {
    if (!host_) {
        return;
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

void NetServer::HandleConnect(ENetPeer* peer) noexcept {
    for (auto& session : sessions_) {
        if (!session.is_active) {
            session.is_active = true;
            session.peer = peer;
            session.input_queue.count = 0;
            session.input_queue.read_idx = 0;
            session.input_queue.write_idx = 0;
            peer->data = &session;
            ++active_players_;
            return;
        }
    }
    // Reject connection if slot pool is exhausted
    enet_peer_reset(peer);
}

void NetServer::HandleDisconnect(ENetPeer* peer) noexcept {
    if (peer->data) {
        auto* session = static_cast<ClientSession*>(peer->data);
        session->is_active = false;
        session->peer = nullptr;
        peer->data = nullptr;
        if (active_players_ > 0) {
            --active_players_;
        }
    }
}

void NetServer::HandlePacket(ENetPeer* peer, const uint8_t* data, size_t length) noexcept {
    if (length < sizeof(PacketHeader)) {
        return;
    }

    PacketHeader header;
    std::memcpy(&header, data, sizeof(PacketHeader));

    auto* session = static_cast<ClientSession*>(peer->data);
    if (!session || !session->is_active) {
        return;
    }

    if (static_cast<PacketType>(header.type) == PacketType::ClientInput) {
        if (header.payload_bytes != sizeof(ClientInputPayload) || length < (sizeof(PacketHeader) + sizeof(ClientInputPayload))) {
            return;
        }

        ClientInputPayload input;
        std::memcpy(&input, data + sizeof(PacketHeader), sizeof(ClientInputPayload));

        // Reject stale out-of-order sequence frames
        if (input.sequence > session->last_input_sequence) {
            session->last_input_sequence = input.sequence;
            session->last_ack_tick = input.ack_tick;
            session->input_queue.Push(input);
        }
    }
}

void NetServer::BroadcastSnapshot(const ServerSnapshotPayload& snapshot) noexcept {
    if (!host_ || active_players_ == 0) {
        return;
    }

    PacketHeader header{
        .type = static_cast<uint8_t>(PacketType::ServerSnapshot),
        .protocol_version = 1,
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
        if (session.is_active && session.peer) {
            enet_peer_send(session.peer, 0, packet);
        }
    }

    enet_host_flush(host_);
}

} // namespace engine
