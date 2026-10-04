#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include "Math.hpp"
#include "WorldState.hpp"
#include "HistoryBuffer.hpp"
#include <array>

namespace engine {

constexpr float kMaxShotRange = 100.0f;

struct HitResult {
    bool hit{false};
    bool occluded{false};
    bool rejected{false};
    bool fired{false};
    uint32_t victim_slot{0};
    float hit_distance{0.0f};
    Vec3 hit_position{0.0f, 0.0f, 0.0f};
    HitZone hit_zone{HitZone::None};
    uint16_t damage{0};
    bool killed{false};
    // Bit per slot that was a live, same-life target at the rewound tick
    uint16_t target_mask{0};
};

struct LagCompensationTelemetry {
    uint64_t total_shots{0};
    uint64_t registered_hits{0};
    uint64_t headshots{0};
    uint64_t bodyshots{0};
    uint64_t legshots{0};
    uint64_t occluded_shots{0};
    uint64_t rejected_rewinds{0};
    int64_t last_rewind_duration_ns{0};
};

// Closest hitbox along the ray, clipped by world geometry; no damage applied
[[nodiscard]] HitResult TraceShot(const WorldState& world, const std::array<PlayerEntity, kMaxPlayers>& targets,
                                  uint32_t shooter_slot, const Ray& ray, float max_range) noexcept;

class LagCompensationManager {
public:
    constexpr LagCompensationManager() noexcept = default;

    // Rewinds older than oldest_tick are rejected as backtracking
    HitResult ProcessShot(WorldState& world, const HistoryBuffer& history,
                          uint32_t shooter_slot, uint32_t target_tick,
                          float sub_tick_fraction = 0.0f,
                          uint32_t interp_span_ticks = 1,
                          uint32_t oldest_tick = 0,
                          float max_range = kMaxShotRange) noexcept;

    [[nodiscard]] const LagCompensationTelemetry& GetTelemetry() const noexcept { return telemetry_; }

private:
    WorldSnapshot historical_snapshot_{};
    std::array<PlayerEntity, kMaxPlayers> rewound_players_{};
    LagCompensationTelemetry telemetry_{};
};

} // namespace engine
