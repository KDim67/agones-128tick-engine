#include "engine/NetServer.hpp"
#include "engine/NetMemoryPool.hpp"
#include <cstring>

namespace engine {

namespace {

// Peers mid-handshake or mid-close live outside the session slots, so a departing player never blocks a joining one
constexpr size_t kSpareNetPeers = 4;

} // namespace

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

bool NetServer::Initialize(uint16_t bind_port, const char* bind_host) noexcept {
    if (!NetMemoryPool::Instance().InstallEnetCallbacks()) {
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = bind_port;
    if (bind_host && enet_address_set_host_ip(&address, bind_host) != 0) {
        enet_deinitialize();
        return false;
    }

    host_ = enet_host_create(&address, kMaxPlayers + kSpareNetPeers, kNetChannelCount, 0, 0);
    if (!host_) {
        enet_deinitialize();
        return false;
    }

    // Defaults allow 32 MB reassembly per peer; every legitimate packet fits one datagram and one pool block
    host_->maximumPacketSize = kMaxMtuPayload;
    host_->maximumWaitingData = kMaxMtuPayload * kMaxInputQueuePerClient;
    return true;
}

uint16_t NetServer::BoundPort() const noexcept {
    ENetAddress address{};
    if (!host_ || enet_socket_get_address(host_->socket, &address) != 0) {
        return 0;
    }
    return address.port;
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

    // Wall-clock input credit; a stalled server owes clients the ticks it skipped
    const auto now = std::chrono::steady_clock::now();
    uint32_t credit = 1;
    if (last_poll_ != std::chrono::steady_clock::time_point{}) {
        constexpr auto kMaxCarry = kTickDurationNs * static_cast<int64_t>(kInputBurstTokens);
        credit_carry_ = std::min(credit_carry_ + (now - last_poll_), kMaxCarry);
        credit = static_cast<uint32_t>(credit_carry_ / kTickDurationNs);
        credit_carry_ -= kTickDurationNs * static_cast<int64_t>(credit);
    }
    last_poll_ = now;

    for (auto& session : sessions_) {
        if (session.state == SessionState::Connected) {
            session.inputs.Refill(credit);
        }
    }

    ENetEvent event;
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

    // Stalled handshakes time out too, so a silent peer cannot squat a slot
    for (auto& session : sessions_) {
        if (session.state != SessionState::Disconnected && session.peer) {
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
            session.inputs.Reset();
            session.spawn_pending = false;
            session.last_activity = std::chrono::steady_clock::now();
            peer->data = &session;
            return;
        }
    }
    // Full: tell the client now instead of leaving it to time out
    enet_peer_disconnect_now(peer, 0);
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
        ++telemetry_.packets_malformed;
        return;
    }

    PacketHeader header;
    std::memcpy(&header, data, sizeof(PacketHeader));

    if (header.protocol_version != kProtocolVersion) {
        ++telemetry_.packets_malformed;
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
            session->spawn_pending = true;
            session->last_activity = std::chrono::steady_clock::now();
            ++active_players_;

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

        const size_t payload_bytes = length - sizeof(PacketHeader);
        const size_t count = (header.payload_bytes == payload_bytes)
            ? DecodeInputBatch(data + sizeof(PacketHeader), payload_bytes, input_batch_)
            : 0;
        if (count == 0) {
            ++telemetry_.packets_malformed;
            return;
        }

        // Any arrival order; copies of buffered or consumed inputs are redundancy, not loss
        for (size_t k = 0; k < count; ++k) {
            ClientInputPayload& input = input_batch_[k];
            if (!std::isfinite(input.sub_tick_fraction) || input.interp_span_ticks > kMaxInterpSpanTicks ||
                !ValidateAndClampViewAngles(input.view_pitch, input.view_yaw)) {
                ++telemetry_.packets_malformed;
                return;
            }

            InputLoss loss{};
            if (session->inputs.Insert(input, loss) == InputInsert::Accepted) {
                ++telemetry_.inputs_accepted;
                session->last_activity = std::chrono::steady_clock::now();
            } else {
                ++telemetry_.inputs_redundant;
            }
            telemetry_.inputs_missed += loss.missed;
            telemetry_.inputs_overflow += loss.overflow;
        }
        return;
    }

    // The client is already closing; a graceful close here would cross its own and hold the peer until timeout
    if (packet_type == PacketType::Disconnect) {
        HandleDisconnect(peer);
        enet_peer_disconnect_now(peer, 0);
    }
}

bool NetServer::PopInput(uint32_t slot_id, ClientInputPayload& out_input) noexcept {
    if (slot_id >= kMaxPlayers || sessions_[slot_id].state != SessionState::Connected) {
        return false;
    }

    auto& inputs = sessions_[slot_id].inputs;
    input_backlog_peak_ = std::max(input_backlog_peak_, inputs.Depth());

    InputLoss loss{};
    const bool popped = inputs.Pop(current_tick_, out_input, loss);
    telemetry_.inputs_missed += loss.missed;
    return popped;
}

bool NetServer::TakeSpawnRequest(uint32_t slot_id) noexcept {
    if (slot_id >= kMaxPlayers || !sessions_[slot_id].spawn_pending) {
        return false;
    }
    sessions_[slot_id].spawn_pending = false;
    return true;
}

uint32_t NetServer::RoundTripTicks(uint32_t slot_id) const noexcept {
    if (slot_id >= kMaxPlayers || !sessions_[slot_id].peer) {
        return 0;
    }
    const ENetPeer* peer = sessions_[slot_id].peer;
    const uint64_t rtt_ms = static_cast<uint64_t>(peer->roundTripTime) + 4ull * peer->roundTripTimeVariance;
    return static_cast<uint32_t>(std::min<uint64_t>((rtt_ms * kTickRate + 999) / 1000, UINT32_MAX));
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
