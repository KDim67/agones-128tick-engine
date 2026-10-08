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
    RecordFrame(current_tick);
    ReplicateSnapshots(current_tick);
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

// Quantized once per tick; rewinds replay exactly the positions every client decodes
void AuthoritativeServer::RecordFrame(uint32_t current_tick) noexcept {
    ReplicatedFrame& frame = frames_[current_tick % kSnapshotFrameHistory];
    frame.tick = current_tick;
    frame.present = 0;

    const auto& players = world_state_.GetPlayers();
    for (size_t i = 0; i < kMaxPlayers; ++i) {
        replicated_players_[i] = players[i];
        if (!players[i].is_active) {
            continue;
        }
        frame.present = static_cast<uint16_t>(frame.present | (1u << i));
        frame.entities[i] = QuantizeEntity(players[i]);
        ApplyQuantized(frame.entities[i], replicated_players_[i]);
    }

    const uint64_t now_ns = static_cast<uint64_t>(current_tick) * static_cast<uint64_t>(kTickDurationNs.count());
    history_buffer_.Record(current_tick, now_ns, replicated_players_);
}

// Each client gets a delta against the newest frame it confirmed, plus its own state at full precision
void AuthoritativeServer::ReplicateSnapshots(uint32_t current_tick) noexcept {
    const ReplicatedFrame& current = frames_[current_tick % kSnapshotFrameHistory];

    for (const auto& session : net_server_.GetSessions()) {
        if (session.state != SessionState::Connected) {
            continue;
        }

        const ReplicatedFrame* baseline = nullptr;
        const uint32_t ack = session.snapshot_ack;
        if (ack != kNoBaseline && ack < current_tick && current_tick - ack < kSnapshotFrameHistory &&
            frames_[ack % kSnapshotFrameHistory].tick == ack) {
            baseline = &frames_[ack % kSnapshotFrameHistory];
        }

        SelfState self{};
        uint8_t self_slot = kNoSelfSlot;
        if (const PlayerEntity* player = world_state_.GetPlayer(session.slot_id)) {
            self.position = player->position;
            self.velocity = player->velocity;
            self.last_input_seq = player->last_applied_input_seq;
            self.flags = static_cast<uint8_t>(player->flags & 0xFFu);
            self.health = static_cast<uint8_t>(std::clamp<int16_t>(player->health, 0, 255));
            self_slot = static_cast<uint8_t>(session.slot_id);
        }

        const size_t bytes = EncodeSnapshot(current, baseline, self_slot, self, snapshot_scratch_.data(), snapshot_scratch_.size());
        if (bytes > 0) {
            net_server_.SendSnapshot(session.slot_id, snapshot_scratch_.data(), bytes, baseline != nullptr);
        }
    }
    net_server_.Flush();
}

} // namespace engine
