#include "BotClient.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace loadgen {

namespace {

constexpr float kMispredictionEpsilonM = 1e-4f;
constexpr float kMinInterpDelayTicks = 2.0f;
constexpr float kMaxInterpDelayTicks = static_cast<float>(engine::kMaxInterpDelayTicks);
constexpr float kJitterSmoothing = 0.05f;
constexpr float kJitterDelayGain = 3.0f;
constexpr uint32_t kRenderSearchTicks = 32;
constexpr auto kShotVerdictTimeout = std::chrono::seconds(3);

constexpr std::array<uint16_t, 8> kMovePatterns = {
    engine::InputFlag_Forward,
    engine::InputFlag_Forward | engine::InputFlag_MoveLeft,
    engine::InputFlag_Forward | engine::InputFlag_MoveRight,
    engine::InputFlag_MoveLeft,
    engine::InputFlag_MoveRight,
    engine::InputFlag_Backward | engine::InputFlag_MoveLeft,
    engine::InputFlag_Backward | engine::InputFlag_MoveRight,
    engine::InputFlag_None
};

[[nodiscard]] double ElapsedTicks(Clock::duration elapsed) noexcept {
    return std::chrono::duration<double>(elapsed).count() * static_cast<double>(engine::kTickRate);
}

[[nodiscard]] engine::Vec3 AimPoint(const engine::PlayerEntity& target, engine::HitZone zone) noexcept {
    return (zone == engine::HitZone::Torso ? target.GetTorsoHitbox() : target.GetHeadHitbox()).Center();
}

void AnglesTowards(const engine::Vec3& from, const engine::Vec3& to, float& pitch, float& yaw) noexcept {
    engine::DirectionToAngles((to - from).Normalized(), pitch, yaw);
}

[[nodiscard]] engine::PlayerEntity FromEntitySnapshot(const engine::EntitySnapshot& entity) noexcept {
    engine::PlayerEntity player{};
    player.slot_id = entity.entity_id;
    player.is_active = true;
    player.flags = entity.flags;
    player.health = static_cast<int16_t>(entity.health);
    player.position = {entity.pos_x, entity.pos_y, entity.pos_z};
    player.velocity = {entity.vel_x, entity.vel_y, entity.vel_z};
    player.view_pitch = entity.pitch;
    player.view_yaw = entity.yaw;
    player.last_applied_input_seq = entity.last_input_seq;
    return player;
}

} // namespace

BotClient::BotClient(const BotConfig& config) : config_(config), rng_(config.seed) {
    for (auto& snapshot : snapshots_) {
        snapshot.tick = kNoTick;
    }
    stats_.misprediction_error_m.reserve(config.sample_reserve);
    stats_.hit_error_m.reserve(config.sample_reserve);
}

BotClient::~BotClient() {
    Stop();
}

bool BotClient::Start(Clock::time_point now) noexcept {
    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) {
        return false;
    }

    peer_ = enet_host_connect(host_, &config_.server, 2, 0);
    if (!peer_) {
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }

    connect_started_ = now;
    state_ = ConnectionState::Connecting;
    return true;
}

void BotClient::Stop() noexcept {
    if (!host_) {
        return;
    }

    if (peer_ && state_ == ConnectionState::Playing) {
        const engine::DisconnectPayload goodbye{.slot_id = slot_, .reason_code = 0};
        SendControl(engine::PacketType::Disconnect, &goodbye, sizeof(goodbye));
        enet_host_flush(host_);
    }
    if (peer_) {
        enet_peer_disconnect_now(peer_, 0);
        peer_ = nullptr;
    }

    enet_host_destroy(host_);
    host_ = nullptr;
    state_ = ConnectionState::Closed;
}

void BotClient::BeginMeasurement() noexcept {
    const bool connected = stats_.connected;
    const bool dropped = stats_.dropped;
    const double connect_ms = stats_.connect_ms;

    // Keep the reserved sample capacity across the reset
    std::vector<float> misprediction_samples = std::move(stats_.misprediction_error_m);
    std::vector<float> hit_samples = std::move(stats_.hit_error_m);
    misprediction_samples.clear();
    hit_samples.clear();

    // A bot lost during warmup stays reported as dropped
    stats_ = BotStats{};
    stats_.connected = connected;
    stats_.dropped = dropped;
    stats_.connect_ms = connect_ms;
    stats_.misprediction_error_m = std::move(misprediction_samples);
    stats_.hit_error_m = std::move(hit_samples);
    measuring_ = true;
}

void BotClient::EndMeasurement() noexcept {
    measuring_ = false;
    if (peer_) {
        stats_.rtt_ms = peer_->roundTripTime;
        stats_.rtt_variance_ms = peer_->roundTripTimeVariance;
    }
}

void BotClient::Tick(Clock::time_point now) noexcept {
    if (!host_ || state_ == ConnectionState::Closed) {
        return;
    }

    Service(now);
    if (state_ == ConnectionState::Playing) {
        StepInput(now);
    }
    ResolveExpiredShots(now);

    if (host_) {
        enet_host_flush(host_);
    }
}

void BotClient::Service(Clock::time_point now) noexcept {
    ENetEvent event;
    while (host_ && enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                OnConnected();
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                HandlePacket(event.packet->data, event.packet->dataLength, now);
                enet_packet_destroy(event.packet);
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                stats_.dropped = true;
                peer_ = nullptr;
                state_ = ConnectionState::Closed;
                return;

            case ENET_EVENT_TYPE_NONE:
            default:
                break;
        }
    }
}

void BotClient::OnConnected() noexcept {
    const engine::ConnectRequestPayload request{
        .protocol_version = engine::kProtocolVersion,
        .client_auth_token = config_.seed
    };
    SendControl(engine::PacketType::ConnectRequest, &request, sizeof(request));
    state_ = ConnectionState::AwaitingAck;
}

void BotClient::HandlePacket(const uint8_t* data, size_t length, Clock::time_point now) noexcept {
    if (length < sizeof(engine::PacketHeader)) {
        return;
    }

    engine::PacketHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.protocol_version != engine::kProtocolVersion || header.payload_bytes != length - sizeof(header)) {
        return;
    }

    const uint8_t* payload = data + sizeof(header);
    switch (static_cast<engine::PacketType>(header.type)) {
        case engine::PacketType::ConnectAck: {
            if (state_ != ConnectionState::AwaitingAck || header.payload_bytes != sizeof(engine::ConnectAckPayload)) {
                return;
            }
            engine::ConnectAckPayload ack;
            std::memcpy(&ack, payload, sizeof(ack));
            if (ack.assigned_slot >= engine::kMaxPlayers) {
                return;
            }
            slot_ = ack.assigned_slot;
            stats_.connected = true;
            stats_.connect_ms = std::chrono::duration<double, std::milli>(now - connect_started_).count();
            state_ = ConnectionState::Playing;
            return;
        }

        case engine::PacketType::ServerSnapshot: {
            if (state_ != ConnectionState::Playing || header.payload_bytes != sizeof(engine::ServerSnapshotPayload)) {
                return;
            }
            engine::ServerSnapshotPayload snapshot;
            std::memcpy(&snapshot, payload, sizeof(snapshot));
            OnSnapshot(snapshot, now);
            return;
        }

        case engine::PacketType::HitNotification: {
            if (header.payload_bytes != sizeof(engine::HitNotificationPayload)) {
                return;
            }
            engine::HitNotificationPayload hit;
            std::memcpy(&hit, payload, sizeof(hit));
            OnHitNotification(hit);
            return;
        }

        case engine::PacketType::ShotResult: {
            if (header.payload_bytes != sizeof(engine::ShotResultPayload)) {
                return;
            }
            engine::ShotResultPayload result;
            std::memcpy(&result, payload, sizeof(result));
            OnShotResult(result);
            return;
        }

        default:
            return;
    }
}

void BotClient::OnSnapshot(const engine::ServerSnapshotPayload& payload, Clock::time_point now) noexcept {
    const uint32_t tick = payload.server_tick;
    if (newest_tick_ != kNoTick && tick + kSnapshotRing <= newest_tick_) {
        return;
    }

    auto& stored = snapshots_[tick % kSnapshotRing];
    if (stored.tick == tick) {
        return;
    }

    stored.tick = tick;
    for (uint32_t i = 0; i < engine::kMaxPlayers; ++i) {
        stored.players[i] = engine::PlayerEntity{};
        stored.players[i].slot_id = i;
    }
    const size_t entity_count = std::min<size_t>(payload.entity_count, engine::kMaxPlayers);
    for (size_t i = 0; i < entity_count; ++i) {
        const auto& entity = payload.entities[i];
        if (entity.entity_id < engine::kMaxPlayers) {
            stored.players[entity.entity_id] = FromEntitySnapshot(entity);
        }
    }

    if (measuring_) {
        ++stats_.snapshots_received;
    }

    if (newest_tick_ != kNoTick && tick < newest_tick_) {
        // Late arrival fills a gap counted when the newer tick landed
        if (measuring_) {
            ++stats_.snapshots_reordered;
            if (stats_.snapshot_ticks_missing > 0) {
                --stats_.snapshot_ticks_missing;
            }
        }
        return;
    }

    if (newest_tick_ != kNoTick) {
        const uint32_t step = tick - newest_tick_;
        if (measuring_) {
            stats_.snapshot_ticks_missing += step - 1;
        }

        // Interpolation delay tracks smoothed inter-arrival deviation
        const auto arrival_ticks = static_cast<float>(ElapsedTicks(now - newest_arrival_));
        const float deviation = std::abs(arrival_ticks - static_cast<float>(step));
        arrival_jitter_ticks_ += kJitterSmoothing * (deviation - arrival_jitter_ticks_);
        interp_delay_ticks_ = std::clamp(kMinInterpDelayTicks + kJitterDelayGain * arrival_jitter_ticks_,
                                         kMinInterpDelayTicks, kMaxInterpDelayTicks);
    }
    newest_tick_ = tick;
    newest_arrival_ = now;

    const engine::PlayerEntity& server_self = stored.players[slot_];
    if (server_self.is_active) {
        ReconcileSelf(server_self);
    }
}

void BotClient::ReconcileSelf(const engine::PlayerEntity& server_self) noexcept {
    auto& self = Self();
    const uint32_t ack = server_self.last_applied_input_seq;

    if (!server_self.IsAlive()) {
        if (predicting_ && measuring_) {
            ++stats_.deaths;
        }
        predicting_ = false;
        self = server_self;
        return;
    }

    // Spawn or respawn teleport: adopt authority, replay what it has not seen
    if (!predicting_) {
        self = server_self;
        sync_sequence_ = ack;
        last_checked_ack_ = ack;
        predicting_ = true;
        ReplayFrom(ack);
        return;
    }

    if (ack <= last_checked_ack_ || ack <= sync_sequence_ || sequence_ - ack >= kInputRing) {
        return;
    }
    last_checked_ack_ = ack;

    const float error = (server_self.position - predicted_positions_[ack % kInputRing]).Length();
    if (measuring_) {
        ++stats_.reconcile_checks;
    }
    if (error <= kMispredictionEpsilonM) {
        return;
    }

    if (measuring_) {
        ++stats_.mispredictions;
        stats_.misprediction_error_m.push_back(error);
    }
    self = server_self;
    ReplayFrom(ack);
}

void BotClient::ReplayFrom(uint32_t acked_sequence) noexcept {
    const uint32_t oldest_retained = sequence_ >= kInputRing ? sequence_ - static_cast<uint32_t>(kInputRing) + 1 : 1;
    for (uint32_t seq = std::max(acked_sequence + 1, oldest_retained); seq <= sequence_; ++seq) {
        local_world_.IntegratePlayerInput(slot_, input_ring_[seq % kInputRing], engine::kFixedDeltaTimeSec);
        predicted_positions_[seq % kInputRing] = Self().position;
    }
}

void BotClient::OnHitNotification(const engine::HitNotificationPayload& hit) noexcept {
    if (hit.victim_slot == slot_ && measuring_) {
        ++stats_.hits_taken;
    }
}

void BotClient::OnShotResult(const engine::ShotResultPayload& result) noexcept {
    auto& shot = shots_[result.input_sequence % kShotRing];
    if (shot.sequence != result.input_sequence || shot.resolved) {
        if (measuring_) {
            ++stats_.unexpected_results;
        }
        return;
    }
    shot.resolved = true;
    if (!shot.counted) {
        return;
    }

    // Trades: the shooter died first, or the predicted victim was no longer a target when the shot ran
    const auto status = static_cast<engine::ShotStatus>(result.status);
    if (status == engine::ShotStatus::Rejected) {
        ++stats_.shots_rejected;
        return;
    }
    const bool victim_was_target = (result.target_mask & (1u << shot.predicted.victim_slot)) != 0;
    if (status == engine::ShotStatus::ShooterDead || !victim_was_target) {
        ++stats_.shots_invalidated;
        return;
    }

    if (status == engine::ShotStatus::Hit && result.victim_slot == shot.predicted.victim_slot &&
        result.hit_zone == static_cast<uint8_t>(shot.predicted.hit_zone)) {
        ++stats_.shots_confirmed;
        const engine::Vec3 server_hit{result.hit_x, result.hit_y, result.hit_z};
        stats_.hit_error_m.push_back((server_hit - shot.predicted.hit_position).Length());
        return;
    }
    ++stats_.shots_mismatched;
}

const engine::WorldSnapshot* BotClient::FindSnapshot(uint32_t tick) const noexcept {
    const auto& snapshot = snapshots_[tick % kSnapshotRing];
    return snapshot.tick == tick ? &snapshot : nullptr;
}

bool BotClient::BuildRenderView(Clock::time_point now, RenderView& out) const noexcept {
    if (newest_tick_ == kNoTick) {
        return false;
    }

    const double since_newest = std::min(1.0, ElapsedTicks(now - newest_arrival_));
    const double render = static_cast<double>(newest_tick_) + since_newest - static_cast<double>(interp_delay_ticks_);
    const auto received = [this](uint32_t tick) { return FindSnapshot(tick) != nullptr; };
    if (!engine::SelectRenderPair(render, newest_tick_, kRenderSearchTicks, received, out.pair)) {
        return false;
    }

    // Same blend the server replays from (tick, span, alpha)
    const engine::WorldSnapshot* base = FindSnapshot(out.pair.tick);
    if (out.pair.span == 0 || out.pair.alpha <= engine::kSubTickSnapEpsilon) {
        out.snapshot = *base;
    } else {
        engine::InterpolateSnapshot(*base, *FindSnapshot(out.pair.tick + out.pair.span), out.pair.alpha, out.snapshot);
    }
    return true;
}

uint32_t BotClient::SelectTarget(const engine::WorldSnapshot& view, const engine::Vec3& eye) noexcept {
    if (target_slot_ < engine::kMaxPlayers && target_slot_ != slot_) {
        const auto& current = view.players[target_slot_];
        if (current.is_active && current.IsAlive()) {
            return target_slot_;
        }
    }

    target_slot_ = kNoTick;
    float best_distance_sq = 0.0f;
    for (const auto& candidate : view.players) {
        if (candidate.slot_id == slot_ || !candidate.is_active || !candidate.IsAlive()) {
            continue;
        }
        const float distance_sq = (candidate.position - eye).LengthSq();
        if (target_slot_ == kNoTick || distance_sq < best_distance_sq) {
            target_slot_ = candidate.slot_id;
            best_distance_sq = distance_sq;
        }
    }

    // Hitbox zone is fixed per engagement
    if (target_slot_ != kNoTick) {
        const bool head = config_.aim == AimMode::Head || (config_.aim == AimMode::Mixed && rng_() % 4 == 0);
        aim_zone_ = head ? engine::HitZone::Head : engine::HitZone::Torso;
    }
    return target_slot_;
}

void BotClient::StepInput(Clock::time_point now) noexcept {
    ++sequence_;

    engine::ClientInputPayload input{};
    input.sequence = sequence_;

    RenderView view;
    const bool have_view = BuildRenderView(now, view);
    if (have_view) {
        input.ack_tick = view.pair.tick;
        input.sub_tick_fraction = view.pair.alpha;
        input.interp_span_ticks = static_cast<uint8_t>(view.pair.span);
        if (measuring_) {
            stats_.interp_delay_tick_sum += static_cast<double>(interp_delay_ticks_);
            ++stats_.interp_delay_samples;
        }
    }

    // Re-roll a strafe pattern every 0.25-1s
    if (move_ticks_left_ == 0) {
        move_flags_ = kMovePatterns[rng_() % kMovePatterns.size()];
        if (rng_() % 10 == 0) {
            move_flags_ = static_cast<uint16_t>(move_flags_ | engine::InputFlag_Crouch);
        }
        move_ticks_left_ = 32 + static_cast<uint32_t>(rng_() % 96);
    }
    --move_ticks_left_;

    uint16_t flags = move_flags_;
    if (rng_() % 256 == 0) {
        flags = static_cast<uint16_t>(flags | engine::InputFlag_Jump);
    }
    input.input_flags = flags;

    auto& self = Self();
    const bool alive = predicting_ && self.IsAlive();
    uint32_t target = kNoTick;
    engine::Vec3 aim_point{};

    if (alive && have_view) {
        target = SelectTarget(view.snapshot, self.GetEyePosition());
        if (target != kNoTick) {
            aim_point = AimPoint(view.snapshot.players[target], aim_zone_);
            AnglesTowards(self.GetEyePosition(), aim_point, view_pitch_, view_yaw_);

            // Server traces from the post-move eye; refine the aim against a trial integration
            engine::WorldState trial = local_world_;
            engine::ClientInputPayload trial_input = input;
            trial_input.view_pitch = view_pitch_;
            trial_input.view_yaw = view_yaw_;
            engine::ValidateAndClampViewAngles(trial_input.view_pitch, trial_input.view_yaw);
            trial.IntegratePlayerInput(slot_, trial_input, engine::kFixedDeltaTimeSec);
            AnglesTowards(trial.GetPlayers()[slot_].GetEyePosition(), aim_point, view_pitch_, view_yaw_);
        }
    }
    if (target == kNoTick) {
        view_yaw_ += 0.5f;
    }

    input.view_pitch = view_pitch_;
    input.view_yaw = view_yaw_;
    engine::ValidateAndClampViewAngles(input.view_pitch, input.view_yaw);
    view_pitch_ = input.view_pitch;
    view_yaw_ = input.view_yaw;

    if (predicting_) {
        local_world_.IntegratePlayerInput(slot_, input, engine::kFixedDeltaTimeSec);
        predicted_positions_[sequence_ % kInputRing] = self.position;
    }

    // Fire only on a predicted hit so every shot carries a verifiable verdict
    if (alive && target != kNoTick && now >= next_fire_) {
        const engine::Ray ray{self.GetEyePosition(), engine::AnglesToDirection(input.view_pitch, input.view_yaw)};
        const engine::HitResult predicted = engine::TraceShot(local_world_, view.snapshot.players, slot_, ray, engine::kMaxShotRange);
        if (predicted.hit) {
            input.input_flags = static_cast<uint16_t>(input.input_flags | engine::InputFlag_Fire);

            auto& shot = shots_[sequence_ % kShotRing];
            shot = PendingShot{};
            shot.sequence = sequence_;
            shot.fired_at = now;
            shot.predicted = predicted;
            shot.counted = measuring_;
            shot.resolved = false;
            next_fire_ = now + config_.fire_interval;

            if (measuring_) {
                ++stats_.shots_fired;
                if (view.pair.gap) {
                    ++stats_.shots_gap_interpolated;
                }
            }
        }
    }

    input_ring_[sequence_ % kInputRing] = input;
    SendInputBatch();
}

void BotClient::SendInputBatch() noexcept {
    if (!peer_) {
        return;
    }

    const uint32_t count = std::min(config_.redundancy, sequence_);
    std::array<engine::ClientInputPayload, engine::kInputRedundancy> batch{};
    for (uint32_t i = 0; i < count; ++i) {
        batch[i] = input_ring_[(sequence_ - count + 1 + i) % kInputRing];
    }

    std::array<uint8_t, sizeof(engine::PacketHeader) + engine::kMaxInputBatchBytes> wire{};
    const size_t payload_bytes = engine::EncodeInputBatch(batch.data(), count, wire.data() + sizeof(engine::PacketHeader),
                                                          wire.size() - sizeof(engine::PacketHeader));
    if (payload_bytes == 0) {
        return;
    }

    const engine::PacketHeader header{
        .type = static_cast<uint8_t>(engine::PacketType::ClientInput),
        .protocol_version = engine::kProtocolVersion,
        .payload_bytes = static_cast<uint16_t>(payload_bytes)
    };
    std::memcpy(wire.data(), &header, sizeof(header));

    ENetPacket* packet = enet_packet_create(wire.data(), sizeof(header) + payload_bytes, ENET_PACKET_FLAG_UNSEQUENCED);
    if (!packet) {
        return;
    }
    if (enet_peer_send(peer_, 0, packet) != 0) {
        enet_packet_destroy(packet);
    }
}

void BotClient::SendControl(engine::PacketType type, const void* payload, size_t bytes) noexcept {
    if (!peer_ || bytes > engine::kMaxControlPayloadBytes) {
        return;
    }

    std::array<uint8_t, sizeof(engine::PacketHeader) + engine::kMaxControlPayloadBytes> wire{};
    const engine::PacketHeader header{
        .type = static_cast<uint8_t>(type),
        .protocol_version = engine::kProtocolVersion,
        .payload_bytes = static_cast<uint16_t>(bytes)
    };
    std::memcpy(wire.data(), &header, sizeof(header));
    std::memcpy(wire.data() + sizeof(header), payload, bytes);

    ENetPacket* packet = enet_packet_create(wire.data(), sizeof(header) + bytes, ENET_PACKET_FLAG_RELIABLE);
    if (packet && enet_peer_send(peer_, 1, packet) != 0) {
        enet_packet_destroy(packet);
    }
}

// Results ride the reliable channel; silence past the timeout means the input never reached the simulation
void BotClient::ResolveExpiredShots(Clock::time_point now) noexcept {
    for (auto& shot : shots_) {
        if (!shot.resolved && now - shot.fired_at >= kShotVerdictTimeout) {
            shot.resolved = true;
            if (shot.counted) {
                ++stats_.shots_unconfirmed;
            }
        }
    }
}

void BotClient::FinalizeShots() noexcept {
    ResolveExpiredShots(Clock::time_point::max());
}

} // namespace loadgen
