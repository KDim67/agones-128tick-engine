#include "engine/WorldState.hpp"
#include <cmath>
#include <algorithm>

namespace engine {

WorldState::WorldState() noexcept {
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        players_[i].slot_id = static_cast<uint32_t>(i);
        players_[i].is_active = false;
        players_[i].flags = EntityFlag_None;
        players_[i].health = 100;
    }
}

void WorldState::SpawnPlayer(uint32_t slot_id, const Vec3& spawn_position) noexcept {
    if (slot_id >= kMaxPlayers) {
        return;
    }
    auto& p = players_[slot_id];
    p.slot_id = slot_id;
    p.is_active = true;
    p.flags = EntityFlag_Alive | EntityFlag_Grounded;
    p.health = 100;
    p.position = spawn_position;
    p.velocity = Vec3{0.0f, 0.0f, 0.0f};
    p.view_pitch = 0.0f;
    p.view_yaw = 0.0f;
    p.last_applied_input_seq = 0;
    p.respawn_tick = 0;
    ++p.spawn_id;
}

void WorldState::ProcessRespawns(uint32_t current_tick) noexcept {
    for (auto& p : players_) {
        if (!p.is_active || p.IsAlive()) {
            continue;
        }

        if (p.respawn_tick == 0) {
            p.respawn_tick = current_tick + kRespawnDelayTicks;
            continue;
        }

        if (current_tick >= p.respawn_tick) {
            // Input sequence and view survive so client acks stay monotonic
            p.flags = EntityFlag_Alive | EntityFlag_Grounded;
            p.health = 100;
            p.position = SpawnPoint(p.slot_id);
            p.velocity = Vec3{0.0f, 0.0f, 0.0f};
            p.respawn_tick = 0;
            ++p.spawn_id;
        }
    }
}

void WorldState::DespawnPlayer(uint32_t slot_id) noexcept {
    if (slot_id < kMaxPlayers) {
        players_[slot_id].is_active = false;
        players_[slot_id].flags = EntityFlag_None;
    }
}

PlayerEntity* WorldState::GetPlayer(uint32_t slot_id) noexcept {
    if (slot_id < kMaxPlayers && players_[slot_id].is_active) {
        return &players_[slot_id];
    }
    return nullptr;
}

const PlayerEntity* WorldState::GetPlayer(uint32_t slot_id) const noexcept {
    if (slot_id < kMaxPlayers && players_[slot_id].is_active) {
        return &players_[slot_id];
    }
    return nullptr;
}

bool WorldState::RaycastWorld(const Ray& ray, float max_dist, float& out_hit_dist) const noexcept {
    float closest_dist = max_dist;
    bool hit_any = false;

    for (const auto& obstacle : kArenaObstacles) {
        float d = 0.0f;
        if (obstacle.IntersectsRay(ray, closest_dist, d)) {
            closest_dist = d;
            hit_any = true;
        }
    }

    if (hit_any) {
        out_hit_dist = closest_dist;
        return true;
    }
    return false;
}

void WorldState::IntegratePlayerInput(uint32_t slot_id, const ClientInputPayload& input, double dt) noexcept {
    if (slot_id >= kMaxPlayers) {
        return;
    }

    auto& p = players_[slot_id];
    if (!p.is_active) {
        return;
    }

    // Consumed inputs are acked while dead so client reconciliation never stalls
    p.last_applied_input_seq = input.sequence;
    if (!p.IsAlive()) {
        return;
    }

    p.view_pitch = input.view_pitch;
    p.view_yaw = input.view_yaw;

    if (input.input_flags & InputFlag_Crouch) {
        p.flags |= EntityFlag_Crouching;
    } else {
        p.flags &= ~EntityFlag_Crouching;
    }

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float yaw_rad = p.view_yaw * kDegToRad;
    const Vec3 forward{std::sin(yaw_rad), 0.0f, std::cos(yaw_rad)};
    const Vec3 right{forward.z, 0.0f, -forward.x};

    Vec3 wish_dir{0.0f, 0.0f, 0.0f};
    if (input.input_flags & InputFlag_Forward)   wish_dir += forward;
    if (input.input_flags & InputFlag_Backward)  wish_dir -= forward;
    if (input.input_flags & InputFlag_MoveRight) wish_dir += right;
    if (input.input_flags & InputFlag_MoveLeft)  wish_dir -= right;
    wish_dir = wish_dir.Normalized();

    const float target_max_speed = p.IsCrouching() ? (kMaxGroundSpeed * 0.45f) : kMaxGroundSpeed;
    const float dt_f = static_cast<float>(dt);

    if (p.IsGrounded()) {
        // Apply ground friction
        const float horiz_speed = std::sqrt(p.velocity.x * p.velocity.x + p.velocity.z * p.velocity.z);
        if (horiz_speed > 1e-4f) {
            const float friction_drop = horiz_speed * kGroundFriction * dt_f;
            const float scale = std::max(0.0f, horiz_speed - friction_drop) / horiz_speed;
            p.velocity.x *= scale;
            p.velocity.z *= scale;
        }

        // Accelerate in wish direction
        const float current_speed = Vec3::Dot(p.velocity, wish_dir);
        const float add_speed = target_max_speed - current_speed;
        if (add_speed > 0.0f) {
            const float accel_speed = std::min(add_speed, kGroundAcceleration * dt_f * target_max_speed);
            p.velocity += wish_dir * accel_speed;
        }

        // Jump
        if (input.input_flags & InputFlag_Jump) {
            p.velocity.y = kJumpVelocity;
            p.flags &= ~EntityFlag_Grounded;
        }
    } else {
        // Air strafe
        const float current_air_speed = Vec3::Dot(p.velocity, wish_dir);
        const float add_air_speed = kMaxAirSpeed - current_air_speed;
        if (add_air_speed > 0.0f) {
            const float air_accel_speed = std::min(add_air_speed, kAirAcceleration * dt_f * target_max_speed);
            p.velocity += wish_dir * air_accel_speed;
        }

        // Gravity
        p.velocity.y -= kGravity * dt_f;
    }

    p.position += p.velocity * dt_f;

    // Ground plane collision
    if (p.position.y <= kArenaBounds.min.y) {
        p.position.y = kArenaBounds.min.y;
        p.velocity.y = 0.0f;
        p.flags |= EntityFlag_Grounded;
    }

    // Arena boundary clamping
    const float radius = kStandingHalfExtents.x;
    p.position.x = std::clamp(p.position.x, kArenaBounds.min.x + radius, kArenaBounds.max.x - radius);
    p.position.z = std::clamp(p.position.z, kArenaBounds.min.z + radius, kArenaBounds.max.z - radius);

    // Obstacle AABB resolution
    for (const auto& obs : kArenaObstacles) {
        if (p.position.x > (obs.min.x - radius) && p.position.x < (obs.max.x + radius) &&
            p.position.z > (obs.min.z - radius) && p.position.z < (obs.max.z + radius) &&
            p.position.y < obs.max.y) {

            const float dx1 = p.position.x - (obs.min.x - radius);
            const float dx2 = (obs.max.x + radius) - p.position.x;
            const float dz1 = p.position.z - (obs.min.z - radius);
            const float dz2 = (obs.max.z + radius) - p.position.z;

            const float min_d = std::min({dx1, dx2, dz1, dz2});
            if (min_d == dx1) p.position.x = obs.min.x - radius;
            else if (min_d == dx2) p.position.x = obs.max.x + radius;
            else if (min_d == dz1) p.position.z = obs.min.z - radius;
            else p.position.z = obs.max.z + radius;
        }
    }
}

} // namespace engine
