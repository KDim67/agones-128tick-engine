#include "engine/LagCompensation.hpp"
#include <chrono>

namespace engine {

HitResult TraceShot(const WorldState& world, const std::array<PlayerEntity, kMaxPlayers>& targets,
                    uint32_t shooter_slot, const Ray& ray, float max_range) noexcept {
    HitResult result{};

    // World obstacle occlusion check
    float world_hit_dist = max_range;
    const bool world_occluded = world.RaycastWorld(ray, max_range, world_hit_dist);
    float closest_hit_dist = world_occluded ? world_hit_dist : max_range;

    for (const auto& target : targets) {
        if (target.slot_id == shooter_slot || !target.is_active || !target.IsAlive()) {
            continue;
        }
        result.target_mask = static_cast<uint16_t>(result.target_mask | (1u << target.slot_id));

        float broad_dist = 0.0f;
        if (!target.GetBoundingBox().IntersectsRay(ray, closest_hit_dist, broad_dist)) {
            continue;
        }

        // Head (150 dmg)
        float head_dist = 0.0f;
        if (target.GetHeadHitbox().IntersectsRay(ray, closest_hit_dist, head_dist)) {
            closest_hit_dist = head_dist;
            result.hit = true;
            result.victim_slot = target.slot_id;
            result.hit_distance = head_dist;
            result.hit_position = ray.origin + ray.direction * head_dist;
            result.hit_zone = HitZone::Head;
            result.damage = 150;
        }

        // Torso (40 dmg)
        float torso_dist = 0.0f;
        if (target.GetTorsoHitbox().IntersectsRay(ray, closest_hit_dist, torso_dist)) {
            closest_hit_dist = torso_dist;
            result.hit = true;
            result.victim_slot = target.slot_id;
            result.hit_distance = torso_dist;
            result.hit_position = ray.origin + ray.direction * torso_dist;
            result.hit_zone = HitZone::Torso;
            result.damage = 40;
        }

        // Legs (30 dmg)
        float legs_dist = 0.0f;
        if (target.GetLegsHitbox().IntersectsRay(ray, closest_hit_dist, legs_dist)) {
            closest_hit_dist = legs_dist;
            result.hit = true;
            result.victim_slot = target.slot_id;
            result.hit_distance = legs_dist;
            result.hit_position = ray.origin + ray.direction * legs_dist;
            result.hit_zone = HitZone::Legs;
            result.damage = 30;
        }
    }

    result.occluded = !result.hit && world_occluded;
    return result;
}

HitResult LagCompensationManager::ProcessShot(WorldState& world, const HistoryBuffer& history,
                                              uint32_t shooter_slot, uint32_t target_tick,
                                              float sub_tick_fraction, uint32_t interp_span_ticks,
                                              uint32_t oldest_tick, float max_range) noexcept {
    const auto start_time = std::chrono::steady_clock::now();

    auto* shooter = world.GetPlayer(shooter_slot);
    if (!shooter || !shooter->IsAlive()) {
        return HitResult{};
    }

    if (target_tick < oldest_tick || interp_span_ticks > kMaxInterpSpanTicks ||
        !history.GetInterpolatedSnapshot(target_tick, interp_span_ticks, SanitizeSubTickAlpha(sub_tick_fraction), historical_snapshot_)) {
        HitResult rejected{};
        rejected.rejected = true;
        ++telemetry_.rejected_rewinds;
        return rejected;
    }

    // Rewind opponent transforms into scratch; live world untouched
    const auto& live_players = world.GetPlayers();
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        const auto& live = live_players[i];
        const auto& historical = historical_snapshot_.players[i];
        rewound_players_[i] = live;
        if (live.slot_id == shooter_slot) {
            continue;
        }

        // Joins, reconnects and respawns after the rewound tick were not there to be hit
        if (historical.is_active && historical.spawn_id == live.spawn_id) {
            rewound_players_[i].position = historical.position;
            rewound_players_[i].flags = historical.flags;
        } else {
            rewound_players_[i].is_active = false;
        }
    }

    const Ray shot_ray{shooter->GetEyePosition(), AnglesToDirection(shooter->view_pitch, shooter->view_yaw)};
    HitResult result = TraceShot(world, rewound_players_, shooter_slot, shot_ray, max_range);
    result.fired = true;
    ++telemetry_.total_shots;

    if (result.hit) {
        ++telemetry_.registered_hits;
        if (result.hit_zone == HitZone::Head)       ++telemetry_.headshots;
        else if (result.hit_zone == HitZone::Torso) ++telemetry_.bodyshots;
        else if (result.hit_zone == HitZone::Legs)  ++telemetry_.legshots;

        auto* live_victim = world.GetPlayer(result.victim_slot);
        if (live_victim) {
            result.killed = live_victim->TakeDamage(result.damage);
        }
    } else if (result.occluded) {
        ++telemetry_.occluded_shots;
    }

    const auto end_time = std::chrono::steady_clock::now();
    telemetry_.last_rewind_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    return result;
}

} // namespace engine
