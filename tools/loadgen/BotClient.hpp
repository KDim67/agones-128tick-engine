#pragma once

#include "engine/Common.hpp"
#include "engine/HistoryBuffer.hpp"
#include "engine/LagCompensation.hpp"
#include "engine/Protocol.hpp"
#include "engine/SnapshotCodec.hpp"
#include "engine/WorldState.hpp"

#include <enet/enet.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace loadgen {

using Clock = std::chrono::steady_clock;

// Mixed: 1 in 4 targets engaged at the head, the rest at the torso
enum class AimMode : uint8_t { Head, Torso, Mixed };

[[nodiscard]] constexpr const char* AimModeName(AimMode mode) noexcept {
    return mode == AimMode::Head ? "head" : (mode == AimMode::Torso ? "torso" : "mixed");
}

struct BotConfig {
    ENetAddress server{};
    uint64_t seed{0};
    uint32_t redundancy{static_cast<uint32_t>(engine::kInputRedundancy)};
    std::chrono::milliseconds fire_interval{500};
    AimMode aim{AimMode::Mixed};
    size_t sample_reserve{0};
};

// Counters cover the measurement window only
struct BotStats {
    bool connected{false};
    bool dropped{false};
    double connect_ms{0.0};
    uint32_t rtt_ms{0};
    uint32_t rtt_variance_ms{0};

    uint64_t snapshots_received{0};
    uint64_t snapshots_reordered{0};
    uint64_t snapshot_ticks_missing{0};
    uint64_t snapshots_undecodable{0};

    uint64_t reconcile_checks{0};
    uint64_t mispredictions{0};
    std::vector<float> misprediction_error_m;

    uint64_t shots_fired{0};
    uint64_t shots_confirmed{0};
    uint64_t shots_mismatched{0};
    uint64_t shots_unconfirmed{0};
    uint64_t shots_rejected{0};
    uint64_t shots_throttled{0};
    uint64_t shots_invalidated{0};
    uint64_t shots_gap_interpolated{0};
    uint64_t unexpected_results{0};
    std::vector<float> hit_error_m;

    uint64_t hits_taken{0};
    uint64_t deaths{0};
    double interp_delay_tick_sum{0.0};
    uint64_t interp_delay_samples{0};
};

// One headless player: ENet transport, prediction, reconciliation, interpolation and shot verification
class BotClient {
public:
    explicit BotClient(const BotConfig& config);
    ~BotClient();

    BotClient(const BotClient&) = delete;
    BotClient& operator=(const BotClient&) = delete;

    bool Start(Clock::time_point now) noexcept;
    void Tick(Clock::time_point now) noexcept;
    void Stop() noexcept;

    void BeginMeasurement() noexcept;
    void EndMeasurement() noexcept;

    // Classifies counted shots still awaiting a server verdict
    void FinalizeShots() noexcept;

    [[nodiscard]] const BotStats& Stats() const noexcept { return stats_; }

private:
    enum class ConnectionState : uint8_t { Idle, Connecting, AwaitingAck, Playing, Closed };

    // Exactly the pair and weight sent as ack_tick, interp_span_ticks and sub_tick_fraction
    struct RenderView {
        engine::RenderPair pair{};
        engine::WorldSnapshot snapshot{};
    };

    struct PendingShot {
        uint32_t sequence{0};
        Clock::time_point fired_at{};
        engine::HitResult predicted{};
        bool counted{false};
        bool resolved{true};
    };

    static constexpr size_t kSnapshotRing = 64;
    static constexpr size_t kInputRing = 256;
    static constexpr size_t kShotRing = 512;
    static constexpr uint32_t kNoTick = ~0u;

    void Service(Clock::time_point now) noexcept;
    void HandlePacket(const uint8_t* data, size_t length, Clock::time_point now) noexcept;
    void OnConnected() noexcept;
    void OnSnapshot(const engine::ReplicatedFrame& frame, const engine::SelfState& self, uint8_t self_slot,
                    Clock::time_point now) noexcept;
    void OnHitNotification(const engine::HitNotificationPayload& hit) noexcept;
    void OnShotResult(const engine::ShotResultPayload& result) noexcept;
    void ReconcileSelf(const engine::PlayerEntity& server_self) noexcept;
    void ReplayFrom(uint32_t acked_sequence) noexcept;

    [[nodiscard]] bool BuildRenderView(Clock::time_point now, RenderView& out) const noexcept;
    [[nodiscard]] const engine::WorldSnapshot* FindSnapshot(uint32_t tick) const noexcept;
    [[nodiscard]] uint32_t SelectTarget(const engine::WorldSnapshot& view, const engine::Vec3& eye) noexcept;

    void StepInput(Clock::time_point now) noexcept;
    void SendInputBatch() noexcept;
    void SendControl(engine::PacketType type, const void* payload, size_t bytes) noexcept;
    void ResolveExpiredShots(Clock::time_point now) noexcept;

    [[nodiscard]] engine::PlayerEntity& Self() noexcept { return local_world_.GetPlayers()[slot_]; }

    BotConfig config_;
    BotStats stats_{};
    ConnectionState state_{ConnectionState::Idle};
    bool measuring_{false};

    ENetHost* host_{nullptr};
    ENetPeer* peer_{nullptr};
    Clock::time_point connect_started_{};

    uint32_t slot_{0};
    std::mt19937_64 rng_;

    std::array<engine::WorldSnapshot, kSnapshotRing> snapshots_{};
    // Decoded wire frames by tick: the baselines later deltas are applied to
    std::array<engine::ReplicatedFrame, kSnapshotRing> frames_{};
    uint32_t newest_tick_{kNoTick};
    Clock::time_point newest_arrival_{};
    float arrival_jitter_ticks_{0.0f};
    float interp_delay_ticks_{2.0f};

    engine::WorldState local_world_{};
    bool predicting_{false};
    uint32_t sync_sequence_{0};
    uint32_t last_checked_ack_{0};
    uint32_t sequence_{0};
    std::array<engine::ClientInputPayload, kInputRing> input_ring_{};
    std::array<engine::Vec3, kInputRing> predicted_positions_{};

    std::array<PendingShot, kShotRing> shots_{};
    Clock::time_point next_fire_{};
    uint32_t last_fire_sequence_{0};
    uint32_t target_slot_{kNoTick};
    engine::HitZone aim_zone_{engine::HitZone::Torso};

    uint16_t move_flags_{0};
    uint32_t move_ticks_left_{0};
    float view_pitch_{0.0f};
    float view_yaw_{0.0f};
};

} // namespace loadgen
