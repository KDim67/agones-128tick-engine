#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include "Math.hpp"
#include "WorldState.hpp"
#include "HistoryBuffer.hpp"
#include <array>

namespace engine {

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

class LagCompensationManager {
public:
    constexpr LagCompensationManager() noexcept = default;

    HitResult ProcessShot(WorldState& world, const HistoryBuffer& history,
                          uint32_t shooter_slot, uint32_t target_tick,
                          float sub_tick_fraction = 0.0f,
                          float max_range = 100.0f) noexcept;

    [[nodiscard]] const LagCompensationTelemetry& GetTelemetry() const noexcept { return telemetry_; }

private:
    std::array<PlayerEntity, kMaxPlayers> backup_players_{};
    LagCompensationTelemetry telemetry_{};
};

} // namespace engine
