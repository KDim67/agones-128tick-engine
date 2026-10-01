#include "engine/LagCompensation.hpp"
#include <chrono>
#include <algorithm>

namespace engine {

HitResult LagCompensationManager::ProcessShot(WorldState& world, const HistoryBuffer& history,
                                              uint32_t shooter_slot, uint32_t target_tick,
                                              float sub_tick_fraction, float max_range) noexcept {
    const auto start_time = std::chrono::steady_clock::now();
    HitResult result{};

    auto* shooter = world.GetPlayer(shooter_slot);
    if (!shooter || !shooter->IsAlive()) {
        return result;
    }

    if (!history.IsTickValid(target_tick)) {
        result.rejected = true;
        ++telemetry_.rejected_rewinds;
        return result;
    }

    // Interpolate continuous target tick
    const float continuous_tick = static_cast<float>(target_tick) + std::clamp(sub_tick_fraction, 0.0f, 0.999f);
    WorldSnapshot historical_snapshot;
    if (!history.GetInterpolatedSnapshot(continuous_tick, historical_snapshot)) {
        result.rejected = true;
        ++telemetry_.rejected_rewinds;
        return result;
    }

    result.fired = true;
    ++telemetry_.total_shots;

    const Vec3 eye_pos = shooter->GetEyePosition();
    const Vec3 view_dir = AnglesToDirection(shooter->view_pitch, shooter->view_yaw);
    const Ray shot_ray{eye_pos, view_dir};

    // World obstacle occlusion check
    float world_hit_dist = max_range;
    const bool world_occluded = world.RaycastWorld(shot_ray, max_range, world_hit_dist);
    float closest_hit_dist = world_occluded ? world_hit_dist : max_range;

    auto& live_players = world.GetPlayers();

    // Rewind opponent transforms
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        backup_players_[i] = live_players[i];

        if (live_players[i].slot_id != shooter_slot && live_players[i].is_active) {
            const auto& historical_entity = historical_snapshot.players[i];
            if (historical_entity.is_active) {
                live_players[i].position = historical_entity.position;
                live_players[i].flags = historical_entity.flags;
            }
        }
    }

    for (const auto& target : live_players) {
        if (target.slot_id == shooter_slot || !target.is_active || !target.IsAlive()) {
            continue;
        }

        float broad_dist = 0.0f;
        if (!target.GetBoundingBox().IntersectsRay(shot_ray, closest_hit_dist, broad_dist)) {
            continue;
        }

        // Head (150 dmg)
        float head_dist = 0.0f;
        if (target.GetHeadHitbox().IntersectsRay(shot_ray, closest_hit_dist, head_dist)) {
            closest_hit_dist = head_dist;
            result.hit = true;
            result.victim_slot = target.slot_id;
            result.hit_distance = head_dist;
            result.hit_position = shot_ray.origin + shot_ray.direction * head_dist;
            result.hit_zone = HitZone::Head;
            result.damage = 150;
        }

        // Torso (40 dmg)
        float torso_dist = 0.0f;
        if (target.GetTorsoHitbox().IntersectsRay(shot_ray, closest_hit_dist, torso_dist)) {
            closest_hit_dist = torso_dist;
            result.hit = true;
            result.victim_slot = target.slot_id;
            result.hit_distance = torso_dist;
            result.hit_position = shot_ray.origin + shot_ray.direction * torso_dist;
            result.hit_zone = HitZone::Torso;
            result.damage = 40;
        }

        // Legs (30 dmg)
        float legs_dist = 0.0f;
        if (target.GetLegsHitbox().IntersectsRay(shot_ray, closest_hit_dist, legs_dist)) {
            closest_hit_dist = legs_dist;
            result.hit = true;
            result.victim_slot = target.slot_id;
            result.hit_distance = legs_dist;
            result.hit_position = shot_ray.origin + shot_ray.direction * legs_dist;
            result.hit_zone = HitZone::Legs;
            result.damage = 30;
        }
    }

    // Restore live transforms
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        live_players[i] = backup_players_[i];
    }

    if (result.hit) {
        ++telemetry_.registered_hits;
        if (result.hit_zone == HitZone::Head)       ++telemetry_.headshots;
        else if (result.hit_zone == HitZone::Torso) ++telemetry_.bodyshots;
        else if (result.hit_zone == HitZone::Legs)  ++telemetry_.legshots;

        auto* live_victim = world.GetPlayer(result.victim_slot);
        if (live_victim) {
            result.killed = live_victim->TakeDamage(result.damage);
        }
    } else if (world_occluded) {
        result.occluded = true;
        ++telemetry_.occluded_shots;
    }

    const auto end_time = std::chrono::steady_clock::now();
    telemetry_.last_rewind_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    return result;
}

} // namespace engine
