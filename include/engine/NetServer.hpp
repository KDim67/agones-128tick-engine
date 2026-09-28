#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include <enet/enet.h>
#include <cstdint>
#include <array>
#include <chrono>

namespace engine {

// Client connection handshake and lifecycle states
enum class SessionState : uint8_t {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2
};

// Preallocated ring buffer for incoming client inputs
struct InputRingBuffer {
    std::array<ClientInputPayload, kMaxInputQueuePerClient> buffer{};
    uint32_t write_idx{0};
    uint32_t read_idx{0};
    uint32_t count{0};

    bool Push(const ClientInputPayload& input) noexcept {
        if (count >= kMaxInputQueuePerClient) {
            return false;
        }
        buffer[write_idx] = input;
        write_idx = (write_idx + 1) % kMaxInputQueuePerClient;
        ++count;
        return true;
    }

    bool Pop(ClientInputPayload& out_input) noexcept {
        if (count == 0) {
            return false;
        }
        out_input = buffer[read_idx];
        read_idx = (read_idx + 1) % kMaxInputQueuePerClient;
        --count;
        return true;
    }
};

// Client session metadata stored in static pool
struct ClientSession {
    SessionState state{SessionState::Disconnected};
    uint32_t slot_id{0};
    ENetPeer* peer{nullptr};
    uint32_t last_ack_tick{0};
    uint32_t last_input_sequence{0};
    uint32_t inputs_this_tick{0};
    std::chrono::steady_clock::time_point last_activity{};
    InputRingBuffer input_queue{};
};

// Zero-allocation non-blocking UDP authoritative transport
class NetServer {
public:
    NetServer() noexcept;
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool Initialize(uint16_t bind_port) noexcept;
    void Shutdown() noexcept;

    // Non-blocking packet poll executed at the start of each tick
    void PollEvents(uint32_t current_tick) noexcept;

    // Check inactive sessions and purge timed-out peers
    void CheckTimeouts() noexcept;

    // Broadcast snapshot to all active client sessions
    void BroadcastSnapshot(const ServerSnapshotPayload& snapshot) noexcept;

    // Direct message dispatch to single client session
    void SendPacket(uint32_t slot_id, PacketType type, const void* payload, size_t payload_bytes, bool reliable) noexcept;

    [[nodiscard]] std::array<ClientSession, kMaxPlayers>& GetSessions() noexcept { return sessions_; }
    [[nodiscard]] const std::array<ClientSession, kMaxPlayers>& GetSessions() const noexcept { return sessions_; }
    [[nodiscard]] size_t GetActivePlayerCount() const noexcept { return active_players_; }

private:
    void HandlePacket(ENetPeer* peer, const uint8_t* data, size_t length) noexcept;
    void HandleConnect(ENetPeer* peer) noexcept;
    void HandleDisconnect(ENetPeer* peer) noexcept;

    ENetHost* host_{nullptr};
    size_t active_players_{0};
    uint32_t current_tick_{0};
    std::array<ClientSession, kMaxPlayers> sessions_{};
    std::array<uint8_t, sizeof(PacketHeader) + sizeof(ServerSnapshotPayload)> broadcast_buffer_{};
    std::array<uint8_t, sizeof(PacketHeader) + sizeof(ConnectAckPayload)> control_buffer_{};
};

} // namespace engine
