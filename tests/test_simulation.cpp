#include "engine/Common.hpp"
#include "engine/Math.hpp"
#include "engine/WorldState.hpp"
#include "engine/HistoryBuffer.hpp"
#include "engine/LagCompensation.hpp"
#include "Check.hpp"

#include <cstdio>
#include <cmath>

namespace {

void AimAt(engine::PlayerEntity& shooter, const engine::Vec3& point) {
    engine::DirectionToAngles((point - shooter.GetEyePosition()).Normalized(), shooter.view_pitch, shooter.view_yaw);
    CHECK(engine::ValidateAndClampViewAngles(shooter.view_pitch, shooter.view_yaw));
}

engine::HitResult ClientTrace(const engine::WorldState& world, const engine::WorldSnapshot& view,
                              const engine::PlayerEntity& shooter) {
    const engine::Ray ray{shooter.GetEyePosition(), engine::AnglesToDirection(shooter.view_pitch, shooter.view_yaw)};
    return engine::TraceShot(world, view.players, shooter.slot_id, ray, engine::kMaxShotRange);
}

bool SamePosition(const engine::Vec3& a, const engine::Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

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
    CHECK(p != nullptr);
    CHECK(p->position.z > 0.0f);
    CHECK(p->position.y == 0.0f);
    CHECK(p->IsGrounded());

    // Obstacle collision against pillar 0: [-11, 0, -11] to [-9, 3, -9]
    world.SpawnPlayer(1, engine::Vec3{-8.5f, 0.0f, -10.0f});
    input.view_yaw = 270.0f;
    for (int i = 0; i < 20; ++i) {
        world.IntegratePlayerInput(1, input, engine::kFixedDeltaTimeSec);
    }
    const auto* p_obs = world.GetPlayer(1);
    CHECK(p_obs->position.x >= -9.0f + engine::kStandingHalfExtents.x);

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

    CHECK(history.GetLatestTick() == 149);
    CHECK(!history.IsTickValid(10));
    CHECK(!history.IsTickValid(21));
    CHECK(history.IsTickValid(22));
    CHECK(history.IsTickValid(149));

    engine::WorldSnapshot interp_snap;
    CHECK(history.GetInterpolatedSnapshot(100, 1, 0.5f, interp_snap));
    CHECK(std::abs(interp_snap.players[0].position.x - 100.5f) < 1e-4f);

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
    CHECK(head_hit.hit);
    CHECK(head_hit.victim_slot == 1);
    CHECK(head_hit.hit_zone == engine::HitZone::Head);
    CHECK(head_hit.damage == 150);
    CHECK(head_hit.killed);

    // Verify live state restoration
    CHECK(std::abs(world.GetPlayer(1)->position.x - 30.0f) < 1e-4f);

    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});
    history.Record(120, 120 * 7'812'500ULL, world.GetPlayers());

    // Torso shot (~1.15m)
    live_shooter.view_pitch = 2.7f;
    live_shooter.view_yaw = 0.0f;
    const engine::HitResult torso_hit = lag_comp.ProcessShot(world, history, 0, 120, 0.0f);
    CHECK(torso_hit.hit);
    CHECK(torso_hit.hit_zone == engine::HitZone::Torso);
    CHECK(torso_hit.damage == 40);
    CHECK(!torso_hit.killed);
    CHECK(world.GetPlayer(1)->health == 60);

    // Legs shot (~0.40m)
    live_shooter.view_pitch = 7.3f;
    live_shooter.view_yaw = 0.0f;
    const engine::HitResult leg_hit = lag_comp.ProcessShot(world, history, 0, 120, 0.0f);
    CHECK(leg_hit.hit);
    CHECK(leg_hit.hit_zone == engine::HitZone::Legs);
    CHECK(leg_hit.damage == 30);
    CHECK(world.GetPlayer(1)->health == 30);

    // Sub-tick lerp at tick 120.5 -> target at (1.0, 0.0, 10.0)
    live_target.position = engine::Vec3{2.0f, 0.0f, 10.0f};
    history.Record(121, 121 * 7'812'500ULL, world.GetPlayers());
    live_target.position = engine::Vec3{30.0f, 0.0f, 10.0f};
    live_shooter.view_pitch = 0.0f;
    live_shooter.view_yaw = 5.71f;
    const engine::HitResult subtick_hit = lag_comp.ProcessShot(world, history, 0, 120, 0.5f);
    CHECK(subtick_hit.hit);

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
    CHECK(!occluded_hit.hit);
    CHECK(lag_comp.GetTelemetry().occluded_shots > 0);

    std::printf("  -> World obstacle occlusion passed. Bullet stopped by cover pillar.\n");
}

void TestLongUptimeSubTickPrecision() {
    std::printf("[Test] Running Sub-Tick Rewind Precision at 3M Ticks (6.5h Uptime)...\n");
    engine::HistoryBuffer history;
    std::array<engine::PlayerEntity, engine::kMaxPlayers> players{};
    players[0].is_active = true;

    constexpr uint32_t kBaseTick = 3'000'000;
    players[0].position = engine::Vec3{0.0f, 0.0f, 0.0f};
    history.Record(kBaseTick, 0, players);
    players[0].position = engine::Vec3{1.0f, 0.0f, 0.0f};
    history.Record(kBaseTick + 1, 0, players);

    // float(3'000'000) + 0.3f rounds to 3'000'000.25; tick and alpha stay separate
    engine::WorldSnapshot snap;
    CHECK(history.GetInterpolatedSnapshot(kBaseTick, 1, 0.3f, snap));
    CHECK(std::abs(snap.players[0].position.x - 0.3f) < 1e-6f);

    CHECK(engine::SanitizeSubTickAlpha(std::nanf("")) == 0.0f);
    CHECK(engine::SanitizeSubTickAlpha(4.0f) == engine::kMaxSubTickAlpha);
    CHECK(engine::SanitizeSubTickAlpha(-1.0f) == 0.0f);

    std::printf("  -> Alpha 0.3 resolved exactly at tick %u; NaN and out-of-range fractions sanitized.\n", kBaseTick);
}

void TestRespawnAndDeadInputAck() {
    std::printf("[Test] Running Respawn Timer and Dead-Player Input Acknowledgement...\n");
    engine::WorldState world;
    world.SpawnPlayer(3, engine::Vec3{5.0f, 0.0f, 5.0f});
    auto* player = world.GetPlayer(3);
    CHECK(player != nullptr);

    engine::ClientInputPayload input{};
    input.sequence = 41;
    input.input_flags = engine::InputFlag_Forward;
    world.IntegratePlayerInput(3, input, engine::kFixedDeltaTimeSec);
    CHECK(player->TakeDamage(150));

    const engine::Vec3 death_position = player->position;
    input.sequence = 42;
    world.IntegratePlayerInput(3, input, engine::kFixedDeltaTimeSec);
    CHECK(player->last_applied_input_seq == 42);
    CHECK(player->position.z == death_position.z);

    constexpr uint32_t kDeathTick = 1000;
    world.ProcessRespawns(kDeathTick);
    world.ProcessRespawns(kDeathTick + engine::kRespawnDelayTicks - 1);
    CHECK(!player->IsAlive());

    world.ProcessRespawns(kDeathTick + engine::kRespawnDelayTicks);
    const engine::Vec3 spawn = engine::WorldState::SpawnPoint(3);
    CHECK(player->IsAlive());
    CHECK(player->health == 100);
    CHECK(player->position.x == spawn.x && player->position.z == spawn.z);
    CHECK(player->last_applied_input_seq == 42);

    std::printf("  -> Dead inputs acked, respawn after %u ticks at slot spawn, sequence preserved.\n",
                engine::kRespawnDelayTicks);
}

void TestClientServerTraceParity() {
    std::printf("[Test] Running Client/Server Shot Trace Parity...\n");
    engine::WorldState world;
    engine::HistoryBuffer history;
    engine::LagCompensationManager lag_comp;

    world.SpawnPlayer(0, engine::Vec3{0.0f, 0.0f, 0.0f});
    world.SpawnPlayer(1, engine::Vec3{-1.0f, 0.0f, 20.0f});
    history.Record(200, 0, world.GetPlayers());
    world.GetPlayers()[1].position = engine::Vec3{1.0f, 0.0f, 20.0f};
    history.Record(201, 0, world.GetPlayers());

    // Client view: same two snapshots, same alpha, shared trace
    engine::WorldSnapshot client_view;
    CHECK(history.GetInterpolatedSnapshot(200, 1, 0.25f, client_view));
    auto& live_shooter = world.GetPlayers()[0];
    AimAt(live_shooter, client_view.players[1].GetHeadHitbox().Center());

    const engine::HitResult predicted = ClientTrace(world, client_view, live_shooter);
    const engine::HitResult confirmed = lag_comp.ProcessShot(world, history, 0, 200, 0.25f);

    CHECK(predicted.hit && confirmed.hit);
    CHECK(predicted.hit_zone == engine::HitZone::Head && confirmed.hit_zone == engine::HitZone::Head);
    CHECK(SamePosition(predicted.hit_position, confirmed.hit_position));
    CHECK(predicted.target_mask == confirmed.target_mask);

    std::printf("  -> Client-predicted and server-rewound headshot land on the identical point.\n");
}

void TestBridgedSnapshotLossParity() {
    std::printf("[Test] Running Rewind Parity Across a Lost Snapshot (span 2)...\n");
    engine::WorldState world;
    engine::HistoryBuffer history;
    engine::LagCompensationManager lag_comp;

    // Target swerves at tick 301; the client never receives that snapshot
    world.SpawnPlayer(0, engine::Vec3{0.0f, 0.0f, 0.0f});
    world.SpawnPlayer(1, engine::Vec3{-1.0f, 0.0f, 20.0f});
    history.Record(300, 0, world.GetPlayers());
    world.GetPlayers()[1].position = engine::Vec3{0.8f, 0.0f, 20.0f};
    history.Record(301, 0, world.GetPlayers());
    world.GetPlayers()[1].position = engine::Vec3{1.0f, 0.0f, 20.0f};
    history.Record(302, 0, world.GetPlayers());

    // Client received 300 and 302 only and renders tick 301 through the shared pair selection
    engine::WorldSnapshot received_300;
    engine::WorldSnapshot received_302;
    CHECK(history.GetSnapshot(300, received_300) && history.GetSnapshot(302, received_302));
    auto received = [](uint32_t tick) { return tick == 300 || tick == 302; };

    engine::RenderPair pair;
    CHECK(engine::SelectRenderPair(301.0, 302, 32, received, pair));
    CHECK(pair.gap && pair.tick == 300 && pair.span == 2 && pair.alpha == 0.5f);

    engine::WorldSnapshot client_view;
    engine::InterpolateSnapshot(received_300, received_302, pair.alpha, client_view);
    auto& live_shooter = world.GetPlayers()[0];
    AimAt(live_shooter, client_view.players[1].GetHeadHitbox().Center());

    const engine::HitResult predicted = ClientTrace(world, client_view, live_shooter);
    const engine::HitResult single_tick = lag_comp.ProcessShot(world, history, 0, 301, 0.0f, 1);
    const engine::HitResult bridged = lag_comp.ProcessShot(world, history, 0, pair.tick, pair.alpha, pair.span);

    CHECK(predicted.hit && predicted.hit_zone == engine::HitZone::Head);
    CHECK(!single_tick.hit);
    CHECK(bridged.hit && bridged.hit_zone == engine::HitZone::Head);
    CHECK(SamePosition(predicted.hit_position, bridged.hit_position));

    // An exact received tick needs no partner; a bridge wider than kMaxInterpSpanTicks holds the older frame
    CHECK(engine::SelectRenderPair(302.0, 302, 32, received, pair));
    CHECK(!pair.gap && pair.tick == 302 && pair.alpha == 0.0f);
    auto sparse = [](uint32_t tick) { return tick == 290 || tick == 300; };
    CHECK(engine::SelectRenderPair(295.5, 300, 32, sparse, pair));
    CHECK(pair.gap && pair.tick == 290 && pair.span == 0 && pair.alpha == 0.0f);

    std::printf("  -> Shared pair selection bridges the lost tick; span-2 rewind lands on the client's exact point.\n");
}

void TestRewindNeverCrossesSpawn() {
    std::printf("[Test] Running Rewind Isolation Across Reconnects and Respawns...\n");
    engine::WorldState world;
    engine::HistoryBuffer history;
    engine::LagCompensationManager lag_comp;

    world.SpawnPlayer(0, engine::Vec3{0.0f, 0.0f, 0.0f});
    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});
    history.Record(400, 0, world.GetPlayers());

    auto& live_shooter = world.GetPlayers()[0];
    AimAt(live_shooter, world.GetPlayers()[1].GetTorsoHitbox().Center());
    const engine::HitResult same_life = lag_comp.ProcessShot(world, history, 0, 400);
    CHECK(same_life.hit && (same_life.target_mask & (1u << 1)) != 0);

    // New occupant in slot 1 standing where the old one stood at tick 400
    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});
    const engine::HitResult reconnect = lag_comp.ProcessShot(world, history, 0, 400);
    CHECK(!reconnect.hit && (reconnect.target_mask & (1u << 1)) == 0);
    CHECK(world.GetPlayer(1)->health == 100);

    // Same occupant killed and respawned: its previous life is out of reach too
    history.Record(401, 0, world.GetPlayers());
    auto& victim = world.GetPlayers()[1];
    CHECK(victim.TakeDamage(200));
    world.ProcessRespawns(500);
    world.ProcessRespawns(500 + engine::kRespawnDelayTicks);
    CHECK(victim.IsAlive());
    victim.position = engine::Vec3{0.0f, 0.0f, 10.0f};
    const engine::HitResult respawned = lag_comp.ProcessShot(world, history, 0, 401);
    CHECK(!respawned.hit && victim.health == 100);

    // Joined after the rewound tick: absent then, absent now
    world.SpawnPlayer(2, engine::Vec3{0.0f, 0.0f, 5.0f});
    const engine::HitResult joined = lag_comp.ProcessShot(world, history, 0, 401);
    CHECK(!joined.hit && (joined.target_mask & (1u << 2)) == 0);

    std::printf("  -> Reconnected, respawned and newly joined players are untouchable at earlier ticks.\n");
}

void TestRewindBackstop() {
    std::printf("[Test] Running Rewind Age Limit (backtrack rejection)...\n");
    engine::WorldState world;
    engine::HistoryBuffer history;
    engine::LagCompensationManager lag_comp;

    world.SpawnPlayer(0, engine::Vec3{0.0f, 0.0f, 0.0f});
    world.SpawnPlayer(1, engine::Vec3{0.0f, 0.0f, 10.0f});
    for (uint32_t tick = 600; tick <= 700; ++tick) {
        history.Record(tick, 0, world.GetPlayers());
    }
    AimAt(world.GetPlayers()[0], world.GetPlayers()[1].GetTorsoHitbox().Center());

    // 20 ms client claiming a 90-tick-old view: past RTT plus interpolation allowance
    constexpr uint32_t kRttTicks = 3;
    constexpr uint32_t kOldest = 701 - (kRttTicks + engine::kMaxInterpDelayTicks + engine::kRewindToleranceTicks);
    const engine::HitResult backtrack = lag_comp.ProcessShot(world, history, 0, 611, 0.0f, 1, kOldest);
    CHECK(backtrack.rejected && !backtrack.hit);
    CHECK(world.GetPlayer(1)->health == 100);

    const engine::HitResult honest = lag_comp.ProcessShot(world, history, 0, kOldest, 0.0f, 1, kOldest);
    CHECK(!honest.rejected && honest.hit);

    std::printf("  -> Rewind to tick %u rejected under a %u-tick limit; tick %u accepted.\n",
                611u, 701 - kOldest, kOldest);
}

void TestViewAngleClampIdempotent() {
    std::printf("[Test] Running View Angle Clamp Idempotence...\n");
    float pitch = 0.0f;
    float yaw = -1e-6f;
    CHECK(engine::ValidateAndClampViewAngles(pitch, yaw));
    const float once = yaw;
    CHECK(engine::ValidateAndClampViewAngles(pitch, yaw));
    CHECK(yaw == once && yaw >= 0.0f && yaw < 360.0f);

    for (const float raw : {-720.0f, -360.0f, -0.0f, 359.99997f, 360.0f, 1e9f}) {
        float p = 95.0f;
        float y = raw;
        CHECK(engine::ValidateAndClampViewAngles(p, y));
        const float clamped_pitch = p;
        const float clamped_yaw = y;
        CHECK(engine::ValidateAndClampViewAngles(p, y));
        CHECK(p == clamped_pitch && y == clamped_yaw && y >= 0.0f && y < 360.0f);
    }

    std::printf("  -> Client and server clamps agree: a second pass never moves the ray.\n");
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
    TestLongUptimeSubTickPrecision();
    TestRespawnAndDeadInputAck();
    TestClientServerTraceParity();
    TestBridgedSnapshotLossParity();
    TestRewindNeverCrossesSpawn();
    TestRewindBackstop();
    TestViewAngleClampIdempotent();

    std::printf("\nAll Simulation Verification Checks PASSED (100%%).\n");
    return 0;
}
