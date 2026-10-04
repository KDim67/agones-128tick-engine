#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include "InputReorderBuffer.hpp"
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

// Client session metadata stored in static pool
struct ClientSession {
    SessionState state{SessionState::Disconnected};
    uint32_t slot_id{0};
    ENetPeer* peer{nullptr};
    std::chrono::steady_clock::time_point last_activity{};
    InputReorderBuffer inputs{};
    bool spawn_pending{false};
};

// Cumulative ingest counters, read by the tick thread for metrics export
struct NetTelemetry {
    uint64_t inputs_accepted{0};
    uint64_t inputs_missed{0};
    uint64_t inputs_redundant{0};
    uint64_t inputs_overflow{0};
    uint64_t packets_malformed{0};
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

    // Next in-order input for this tick, paced by the session token bucket
    [[nodiscard]] bool PopInput(uint32_t slot_id, ClientInputPayload& out_input) noexcept;

    // True once per handshake: the slot has a new occupant whose entity must be spawned fresh
    [[nodiscard]] bool TakeSpawnRequest(uint32_t slot_id) noexcept;

    // Smoothed RTT plus four deviations, in whole ticks
    [[nodiscard]] uint32_t RoundTripTicks(uint32_t slot_id) const noexcept;

    // Direct message dispatch to single client session
    void SendPacket(uint32_t slot_id, PacketType type, const void* payload, size_t payload_bytes, bool reliable) noexcept;

    [[nodiscard]] std::array<ClientSession, kMaxPlayers>& GetSessions() noexcept { return sessions_; }
    [[nodiscard]] const std::array<ClientSession, kMaxPlayers>& GetSessions() const noexcept { return sessions_; }
    [[nodiscard]] size_t GetActivePlayerCount() const noexcept { return active_players_; }
    [[nodiscard]] const NetTelemetry& GetTelemetry() const noexcept { return telemetry_; }

    // Deepest per-session input backlog since the previous call
    [[nodiscard]] uint32_t TakeInputBacklogPeak() noexcept {
        const uint32_t peak = input_backlog_peak_;
        input_backlog_peak_ = 0;
        return peak;
    }

private:
    void HandlePacket(ENetPeer* peer, const uint8_t* data, size_t length) noexcept;
    void HandleConnect(ENetPeer* peer) noexcept;
    void HandleDisconnect(ENetPeer* peer) noexcept;

    ENetHost* host_{nullptr};
    size_t active_players_{0};
    uint32_t current_tick_{0};
    std::array<ClientSession, kMaxPlayers> sessions_{};
    std::array<uint8_t, sizeof(PacketHeader) + sizeof(ServerSnapshotPayload)> broadcast_buffer_{};
    std::array<uint8_t, sizeof(PacketHeader) + kMaxControlPayloadBytes> control_buffer_{};
    std::array<ClientInputPayload, kInputRedundancy> input_batch_{};
    NetTelemetry telemetry_{};
    std::chrono::steady_clock::time_point last_poll_{};
    std::chrono::nanoseconds credit_carry_{0};
    uint32_t input_backlog_peak_{0};
};

} // namespace engine
