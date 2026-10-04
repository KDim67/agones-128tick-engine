#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include "Math.hpp"
#include <array>

namespace engine {

enum EntityFlags : uint32_t {
    EntityFlag_None       = 0,
    EntityFlag_Alive      = 1 << 0,
    EntityFlag_Grounded   = 1 << 1,
    EntityFlag_Crouching  = 1 << 2,
    EntityFlag_Firing     = 1 << 3
};

constexpr Vec3 kStandingHalfExtents{0.35f, 0.90f, 0.35f};
constexpr Vec3 kCrouchingHalfExtents{0.35f, 0.60f, 0.35f};
constexpr float kStandingEyeHeight = 1.62f;
constexpr float kCrouchingEyeHeight = 1.05f;

constexpr float kMaxGroundSpeed = 7.5f;
constexpr float kMaxAirSpeed = 2.5f;
constexpr float kGroundAcceleration = 50.0f;
constexpr float kAirAcceleration = 12.0f;
constexpr float kGroundFriction = 10.0f;
constexpr float kGravity = 22.0f;
constexpr float kJumpVelocity = 6.5f;
constexpr AABB kArenaBounds{Vec3{-50.0f, 0.0f, -50.0f}, Vec3{50.0f, 30.0f, 50.0f}};

constexpr size_t kObstacleCount = 4;
constexpr std::array<AABB, kObstacleCount> kArenaObstacles = {{
    {Vec3{-11.0f, 0.0f, -11.0f}, Vec3{-9.0f, 3.0f, -9.0f}},
    {Vec3{  9.0f, 0.0f, -11.0f}, Vec3{ 11.0f, 3.0f, -9.0f}},
    {Vec3{-11.0f, 0.0f,   9.0f}, Vec3{-9.0f, 3.0f,  11.0f}},
    {Vec3{  9.0f, 0.0f,   9.0f}, Vec3{ 11.0f, 3.0f,  11.0f}}
}};

struct PlayerEntity {
    uint32_t slot_id{0};
    bool is_active{false};
    uint32_t flags{EntityFlag_None};
    int16_t health{100};
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float view_pitch{0.0f};
    float view_yaw{0.0f};
    uint32_t last_applied_input_seq{0};
    uint32_t respawn_tick{0};
    // Bumped on every spawn and respawn; a rewind never reaches across it
    uint32_t spawn_id{0};

    [[nodiscard]] bool IsAlive() const noexcept { return (flags & EntityFlag_Alive) != 0 && health > 0; }
    [[nodiscard]] bool IsGrounded() const noexcept { return (flags & EntityFlag_Grounded) != 0; }
    [[nodiscard]] bool IsCrouching() const noexcept { return (flags & EntityFlag_Crouching) != 0; }

    // Player bounding box
    [[nodiscard]] AABB GetBoundingBox() const noexcept {
        const Vec3 extents = IsCrouching() ? kCrouchingHalfExtents : kStandingHalfExtents;
        const Vec3 center{position.x, position.y + extents.y, position.z};
        return AABB::FromCenterAndExtents(center, extents);
    }

    [[nodiscard]] AABB GetHeadHitbox() const noexcept {
        const float eye_y = IsCrouching() ? kCrouchingEyeHeight : kStandingEyeHeight;
        return AABB::FromCenterAndExtents({position.x, position.y + eye_y, position.z}, {0.14f, 0.14f, 0.14f});
    }

    [[nodiscard]] AABB GetTorsoHitbox() const noexcept {
        const float center_y = IsCrouching() ? 0.68f : 1.15f;
        const float half_h = IsCrouching() ? 0.22f : 0.32f;
        return AABB::FromCenterAndExtents({position.x, position.y + center_y, position.z}, {0.28f, half_h, 0.20f});
    }

    [[nodiscard]] AABB GetLegsHitbox() const noexcept {
        const float center_y = IsCrouching() ? 0.24f : 0.40f;
        const float half_h = IsCrouching() ? 0.24f : 0.40f;
        return AABB::FromCenterAndExtents({position.x, position.y + center_y, position.z}, {0.26f, half_h, 0.20f});
    }

    [[nodiscard]] Vec3 GetEyePosition() const noexcept {
        const float eye_offset = IsCrouching() ? kCrouchingEyeHeight : kStandingEyeHeight;
        return {position.x, position.y + eye_offset, position.z};
    }

    bool TakeDamage(uint16_t damage) noexcept {
        health = static_cast<int16_t>(std::max(0, health - static_cast<int>(damage)));
        if (health == 0) {
            flags &= ~EntityFlag_Alive;
            velocity = Vec3{0.0f, 0.0f, 0.0f};
            return true;
        }
        return false;
    }
};

class WorldState {
public:
    WorldState() noexcept;

    void SpawnPlayer(uint32_t slot_id, const Vec3& spawn_position) noexcept;
    void DespawnPlayer(uint32_t slot_id) noexcept;

    // Dead players respawn at their slot spawn point kRespawnDelayTicks after death
    void ProcessRespawns(uint32_t current_tick) noexcept;

    [[nodiscard]] static constexpr Vec3 SpawnPoint(uint32_t slot_id) noexcept {
        return {static_cast<float>(slot_id * 4) - 16.0f, 0.0f, 0.0f};
    }

    void IntegratePlayerInput(uint32_t slot_id, const ClientInputPayload& input, double dt) noexcept;
    [[nodiscard]] bool RaycastWorld(const Ray& ray, float max_dist, float& out_hit_dist) const noexcept;

    [[nodiscard]] std::array<PlayerEntity, kMaxPlayers>& GetPlayers() noexcept { return players_; }
    [[nodiscard]] const std::array<PlayerEntity, kMaxPlayers>& GetPlayers() const noexcept { return players_; }
    [[nodiscard]] PlayerEntity* GetPlayer(uint32_t slot_id) noexcept;
    [[nodiscard]] const PlayerEntity* GetPlayer(uint32_t slot_id) const noexcept;

private:
    std::array<PlayerEntity, kMaxPlayers> players_{};
};

} // namespace engine
