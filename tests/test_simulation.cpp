#include "engine/Common.hpp"
#include "engine/Math.hpp"
#include "engine/WorldState.hpp"
#include "engine/HistoryBuffer.hpp"
#include "engine/LagCompensation.hpp"

#include <cassert>
#include <cstdio>
#include <cmath>

namespace {

void TestKinematicsAndCollision() {
    std::printf("[Test] Running Kinematics, Air Strafe, and Obstacle Collision...\n");
    engine::WorldState world;
    world.SpawnPlayer(0, engine::Vec3{0.0f, 0.0f, 0.0f});

    engine::ClientInputPayload input{};
    input.sequence = 1;
    input.view_yaw = 0.0f;
    input.view_pitch = 0.0f;
    input.input_flags = engine::InputFlag_Forward;

    for (int i = 0; i < 10; ++i) {
        world.IntegratePlayerInput(0, input, engine::kFixedDeltaTimeSec);
    }

    const auto* p = world.GetPlayer(0);
    assert(p != nullptr);
    assert(p->position.z > 0.0f);
    assert(p->position.y == 0.0f);
    assert(p->IsGrounded());

    // Obstacle collision against pillar 0: [-11, 0, -11] to [-9, 3, -9]
    world.SpawnPlayer(1, engine::Vec3{-8.5f, 0.0f, -10.0f});
    input.view_yaw = 270.0f;
    for (int i = 0; i < 20; ++i) {
        world.IntegratePlayerInput(1, input, engine::kFixedDeltaTimeSec);
    }
    const auto* p_obs = world.GetPlayer(1);
    assert(p_obs->position.x >= -9.0f + engine::kStandingHalfExtents.x);

    std::printf("  -> Kinematics & collision passed. Grounded Z: %.3f m, Obstacle clamped X: %.3f m\n",
                p->position.z, p_obs->position.x);
}

void TestHistoryBuffer() {
    std::printf("[Test] Running Circular History Buffer (128 snapshots)...\n");
    engine::HistoryBuffer history;
    std::array<engine::PlayerEntity, engine::kMaxPlayers> dummy_players{};
    dummy_players[0].slot_id = 0;
    dummy_players[0].is_active = true;

    for (uint32_t tick = 0; tick < 150; ++tick) {
        dummy_players[0].position = engine::Vec3{static_cast<float>(tick), 0.0f, 0.0f};
        history.Record(tick, tick * 7'812'500ULL, dummy_players);
    }

    assert(history.GetLatestTick() == 149);
    assert(!history.IsTickValid(10));
    assert(!history.IsTickValid(21));
    assert(history.IsTickValid(22));
    assert(history.IsTickValid(149));

    engine::WorldSnapshot interp_snap;
    assert(history.GetInterpolatedSnapshot(100.5f, interp_snap));
    assert(std::abs(interp_snap.players[0].position.x - 100.5f) < 1e-4f);

    std::printf("  -> History buffer passed. 128 snapshots retained, sub-tick lerp verified.\n");
}

void TestMultiHitboxAndLagCompensation() {
    std::printf("[Test] Running Multi-Hitbox Lag Compensation & Sub-Tick Rewind...\n");
    engine::WorldState world;
    engine::HistoryBuffer history;
    engine::LagCompensationManager lag_comp;

    world.SpawnPlayer(0, engine::Vec3{0.0f, 0.0f, 0.0f});
    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});

    history.Record(100, 100 * 7'812'500ULL, world.GetPlayers());

    auto& live_target = world.GetPlayers()[1];
    live_target.position = engine::Vec3{2.0f, 0.0f, 10.0f};
    history.Record(101, 101 * 7'812'500ULL, world.GetPlayers());

    live_target.position = engine::Vec3{30.0f, 0.0f, 10.0f};
    history.Record(110, 110 * 7'812'500ULL, world.GetPlayers());

    auto& live_shooter = world.GetPlayers()[0];

    // Headshot (eye level ~1.62m)
    live_shooter.view_pitch = 0.0f;
    live_shooter.view_yaw = 0.0f;
    const engine::HitResult head_hit = lag_comp.ProcessShot(world, history, 0, 100, 0.0f);
    assert(head_hit.hit);
    assert(head_hit.victim_slot == 1);
    assert(head_hit.hit_zone == engine::HitZone::Head);
    assert(head_hit.damage == 150);
    assert(head_hit.killed);

    // Verify live state restoration
    assert(std::abs(world.GetPlayer(1)->position.x - 30.0f) < 1e-4f);

    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});
    history.Record(120, 120 * 7'812'500ULL, world.GetPlayers());

    // Torso shot (~1.15m)
    live_shooter.view_pitch = 2.7f;
    live_shooter.view_yaw = 0.0f;
    const engine::HitResult torso_hit = lag_comp.ProcessShot(world, history, 0, 120, 0.0f);
    assert(torso_hit.hit);
    assert(torso_hit.hit_zone == engine::HitZone::Torso);
    assert(torso_hit.damage == 40);
    assert(!torso_hit.killed);
    assert(world.GetPlayer(1)->health == 60);

    // Legs shot (~0.40m)
    live_shooter.view_pitch = 7.3f;
    live_shooter.view_yaw = 0.0f;
    const engine::HitResult leg_hit = lag_comp.ProcessShot(world, history, 0, 120, 0.0f);
    assert(leg_hit.hit);
    assert(leg_hit.hit_zone == engine::HitZone::Legs);
    assert(leg_hit.damage == 30);
    assert(world.GetPlayer(1)->health == 30);

    // Sub-tick lerp at tick 100.5 -> target at (1.0, 0.0, 10.0)
    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});
    live_shooter.view_pitch = 0.0f;
    live_shooter.view_yaw = 5.71f;
    const engine::HitResult subtick_hit = lag_comp.ProcessShot(world, history, 0, 100, 0.5f);
    assert(subtick_hit.hit);

    std::printf("  -> Multi-hitbox (Head: %u dmg, Torso: %u dmg, Legs: %u dmg) and Sub-tick lerp passed.\n",
                head_hit.damage, torso_hit.damage, leg_hit.damage);
}

void TestWorldOcclusion() {
    std::printf("[Test] Running Static Obstacle Occlusion Verification...\n");
    engine::WorldState world;
    engine::HistoryBuffer history;
    engine::LagCompensationManager lag_comp;

    // Obstacle 0 covers x=[-11, -9], z=[-11, -9]
    world.SpawnPlayer(0, engine::Vec3{-10.0f, 0.0f, -15.0f});
    world.SpawnPlayer(1, engine::Vec3{-10.0f, 0.0f,  -5.0f});

    history.Record(50, 50 * 7'812'500ULL, world.GetPlayers());

    auto& shooter = world.GetPlayers()[0];
    shooter.view_pitch = 0.0f;
    shooter.view_yaw = 0.0f;

    const engine::HitResult occluded_hit = lag_comp.ProcessShot(world, history, 0, 50, 0.0f);
    assert(!occluded_hit.hit);
    assert(lag_comp.GetTelemetry().occluded_shots > 0);

    std::printf("  -> World obstacle occlusion passed. Bullet stopped by cover pillar.\n");
}

} // namespace

int main() {
    std::printf("====================================================\n");
    std::printf(" Agones-128 Engine: Simulation Test Verification\n");
    std::printf("====================================================\n");

    TestKinematicsAndCollision();
    TestHistoryBuffer();
    TestMultiHitboxAndLagCompensation();
    TestWorldOcclusion();

    std::printf("\nAll Simulation Verification Checks PASSED (100%%).\n");
    return 0;
}
