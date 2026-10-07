#include "engine/AuthoritativeServer.hpp"

namespace engine {

void AuthoritativeServer::Tick(uint32_t current_tick, double dt) noexcept {
    net_server_.PollEvents(current_tick);

    if (current_tick > 0 && (current_tick % kTickRate) == 0) {
        net_server_.CheckTimeouts();
    }

    for (auto& session : net_server_.GetSessions()) {
        ProcessSession(session, current_tick, dt);
    }

    world_state_.ProcessRespawns(current_tick);

    const uint64_t now_ns = static_cast<uint64_t>(current_tick) * static_cast<uint64_t>(kTickDurationNs.count());
    history_buffer_.Record(current_tick, now_ns, world_state_.GetPlayers());

    BroadcastSnapshot(current_tick);
}

void AuthoritativeServer::ProcessSession(ClientSession& session, uint32_t current_tick, double dt) noexcept {
    if (session.state != SessionState::Connected) {
        // Handshaking slots carry no entity, so a departed occupant cannot linger in snapshots
        world_state_.DespawnPlayer(session.slot_id);
        return;
    }

    // Fresh entity per handshake; a slot reused within one poll must not inherit the previous occupant
    if (net_server_.TakeSpawnRequest(session.slot_id)) {
        world_state_.SpawnPlayer(session.slot_id, WorldState::SpawnPoint(session.slot_id));
    }

    // Rewinds older than RTT plus the interpolation allowance are backtracking, not lag
    const uint32_t max_rewind = net_server_.RoundTripTicks(session.slot_id) + kMaxInterpDelayTicks + kRewindToleranceTicks;
    const uint32_t oldest_tick = current_tick > max_rewind ? current_tick - max_rewind : 0;

    ClientInputPayload input;
    while (net_server_.PopInput(session.slot_id, input)) {
        world_state_.IntegratePlayerInput(session.slot_id, input, dt);
        if (input.input_flags & InputFlag_Fire) {
            ResolveShot(session.slot_id, input, current_tick, oldest_tick);
        }
    }
}

void AuthoritativeServer::ResolveShot(uint32_t shooter_slot, const ClientInputPayload& input, uint32_t current_tick,
                                      uint32_t oldest_tick) noexcept {
    // Too soon: the input still moved the player, but no round leaves the barrel
    const bool throttled = !world_state_.TryFire(shooter_slot, input.sequence);
    HitResult hit{};
    if (throttled) {
        metrics_.RecordThrottledShot();
    } else {
        hit = lag_comp_.ProcessShot(world_state_, history_buffer_, shooter_slot, input.ack_tick,
                                    input.sub_tick_fraction, input.interp_span_ticks, oldest_tick);
        metrics_.RecordCombat(hit.fired, hit.hit, hit.occluded, hit.rejected);
    }

    const ShotStatus status = throttled  ? ShotStatus::Throttled
        : hit.rejected ? ShotStatus::Rejected
        : !hit.fired   ? ShotStatus::ShooterDead
        : hit.hit      ? ShotStatus::Hit
                       : ShotStatus::Miss;
    const ShotResultPayload result{
        .input_sequence = input.sequence,
        .server_tick = current_tick,
        .victim_slot = hit.victim_slot,
        .hit_x = hit.hit_position.x,
        .hit_y = hit.hit_position.y,
        .hit_z = hit.hit_position.z,
        .target_mask = hit.target_mask,
        .status = static_cast<uint8_t>(status),
        .hit_zone = static_cast<uint8_t>(hit.hit_zone)
    };
    net_server_.SendPacket(shooter_slot, PacketType::ShotResult, &result, sizeof(result), true);

    if (!hit.hit) {
        return;
    }

    const auto* victim = world_state_.GetPlayer(hit.victim_slot);
    const HitNotificationPayload notification{
        .shooter_slot = shooter_slot,
        .victim_slot = hit.victim_slot,
        .server_tick = current_tick,
        .hit_x = hit.hit_position.x,
        .hit_y = hit.hit_position.y,
        .hit_z = hit.hit_position.z,
        .distance = hit.hit_distance,
        .damage = hit.damage,
        .hit_zone = static_cast<uint8_t>(hit.hit_zone),
        .victim_health = victim ? static_cast<uint8_t>(victim->health) : uint8_t{0}
    };
    net_server_.SendPacket(hit.victim_slot, PacketType::HitNotification, &notification, sizeof(notification), true);
}

void AuthoritativeServer::BroadcastSnapshot(uint32_t current_tick) noexcept {
    snapshot_.server_tick = current_tick;
    uint8_t entity_count = 0;

    for (const auto& p : world_state_.GetPlayers()) {
        if (!p.is_active) {
            continue;
        }
        auto& ent = snapshot_.entities[entity_count++];
        ent.entity_id = p.slot_id;
        ent.pos_x = p.position.x;
        ent.pos_y = p.position.y;
        ent.pos_z = p.position.z;
        ent.vel_x = p.velocity.x;
        ent.vel_y = p.velocity.y;
        ent.vel_z = p.velocity.z;
        ent.pitch = p.view_pitch;
        ent.yaw = p.view_yaw;
        ent.flags = p.flags;
        ent.last_input_seq = p.last_applied_input_seq;
        ent.health = static_cast<uint8_t>(p.health);
        ent.reserved[0] = 0;
        ent.reserved[1] = 0;
        ent.reserved[2] = 0;
    }

    snapshot_.entity_count = entity_count;
    net_server_.BroadcastSnapshot(snapshot_);
}

} // namespace engine
