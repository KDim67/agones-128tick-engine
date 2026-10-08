#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include "InputReorderBuffer.hpp"
#include "SnapshotCodec.hpp"
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
    // Newest snapshot tick the client holds; the next snapshot is a delta against it
    uint32_t snapshot_ack{kNoBaseline};
    bool spawn_pending{false};
};

// Cumulative ingest counters, read by the tick thread for metrics export
struct NetTelemetry {
    uint64_t inputs_accepted{0};
    uint64_t inputs_missed{0};
    uint64_t inputs_redundant{0};
    uint64_t inputs_overflow{0};
    uint64_t packets_malformed{0};
    uint64_t backlog_disconnects{0};
};

// Zero-allocation non-blocking UDP authoritative transport
class NetServer {
public:
    NetServer() noexcept;
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Null bind_host listens on every interface
    bool Initialize(uint16_t bind_port, const char* bind_host = nullptr) noexcept;
    void Shutdown() noexcept;

    // Actual UDP port, resolved when bound to port 0
    [[nodiscard]] uint16_t BoundPort() const noexcept;

    // Non-blocking packet poll executed at the start of each tick
    void PollEvents(uint32_t current_tick) noexcept;

    // Check inactive sessions and purge timed-out peers
    void CheckTimeouts() noexcept;

    // Queues one client's snapshot unsequenced; Flush() sends the tick's batch in one pass
    void SendSnapshot(uint32_t slot_id, const uint8_t* payload, size_t payload_bytes) noexcept;
    void Flush() noexcept;

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

    // Peers mid-handshake or mid-close live outside the session slots, so a departing player never blocks a joining one
    static constexpr size_t kSpareNetPeers = 4;
    // Unacked reliable packets one peer may hold in the pool; honest peers sit in single digits
    static constexpr uint32_t kMaxReliableBacklog = 64;

private:
    void HandlePacket(ENetPeer* peer, const uint8_t* data, size_t length) noexcept;
    void HandleConnect(ENetPeer* peer) noexcept;
    void HandleDisconnect(ENetPeer* peer) noexcept;

    ENetHost* host_{nullptr};
    size_t active_players_{0};
    uint32_t current_tick_{0};
    std::array<ClientSession, kMaxPlayers> sessions_{};
    std::array<uint8_t, sizeof(PacketHeader) + kMaxSnapshotBytes> snapshot_buffer_{};
    std::array<uint8_t, sizeof(PacketHeader) + kMaxControlPayloadBytes> control_buffer_{};
    std::array<ClientInputPayload, kInputRedundancy> input_batch_{};
    NetTelemetry telemetry_{};
    std::chrono::steady_clock::time_point last_poll_{};
    std::chrono::nanoseconds credit_carry_{0};
    uint32_t input_backlog_peak_{0};
    std::array<uint32_t, kMaxPlayers + kSpareNetPeers> reliable_in_flight_{};
};

} // namespace engine
