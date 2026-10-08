#include "engine/Protocol.hpp"
#include "engine/InputReorderBuffer.hpp"
#include "engine/SnapshotCodec.hpp"
#include "Check.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {

static_assert(engine::kMaxControlPayloadBytes >= sizeof(engine::HitNotificationPayload));
static_assert(engine::kMaxInputBatchBytes == 1 + sizeof(uint32_t) + engine::kInputRedundancy * sizeof(engine::ClientInputPayload));

std::array<engine::ClientInputPayload, engine::kInputRedundancy> MakeInputs() {
    std::array<engine::ClientInputPayload, engine::kInputRedundancy> inputs{};
    for (uint32_t i = 0; i < engine::kInputRedundancy; ++i) {
        inputs[i].sequence = 100 + i;
        inputs[i].ack_tick = 5000 + i;
        inputs[i].sub_tick_fraction = 0.25f * static_cast<float>(i);
        inputs[i].view_yaw = 90.0f;
        inputs[i].input_flags = static_cast<uint16_t>(engine::InputFlag_Forward | (i == 3 ? engine::InputFlag_Fire : 0));
    }
    return inputs;
}

void TestInputBatchRoundTrip() {
    std::printf("[Test] Running Redundant Input Batch Round Trip (1..%zu inputs)...\n", engine::kInputRedundancy);
    const auto inputs = MakeInputs();

    for (size_t count = 1; count <= engine::kInputRedundancy; ++count) {
        std::array<uint8_t, engine::kMaxInputBatchBytes> wire{};
        const uint32_t snapshot_ack = 9000 + static_cast<uint32_t>(count);
        const size_t bytes = engine::EncodeInputBatch(inputs.data(), count, snapshot_ack, wire.data(), wire.size());
        CHECK(bytes == engine::InputBatchBytes(count));

        std::array<engine::ClientInputPayload, engine::kInputRedundancy> decoded{};
        uint32_t decoded_ack = 0;
        CHECK(engine::DecodeInputBatch(wire.data(), bytes, decoded, decoded_ack) == count);
        CHECK(decoded_ack == snapshot_ack);
        for (size_t i = 0; i < count; ++i) {
            CHECK(decoded[i].sequence == inputs[i].sequence);
            CHECK(decoded[i].ack_tick == inputs[i].ack_tick);
            CHECK(decoded[i].sub_tick_fraction == inputs[i].sub_tick_fraction);
            CHECK(decoded[i].input_flags == inputs[i].input_flags);
        }
    }

    std::printf("  -> Batches of 1-%zu inputs encode to %zu-%zu bytes and decode losslessly.\n",
                engine::kInputRedundancy, engine::InputBatchBytes(1), engine::kMaxInputBatchBytes);
}

void TestMalformedBatchRejection() {
    std::printf("[Test] Running Malformed Input Batch Rejection...\n");
    const auto inputs = MakeInputs();
    std::array<uint8_t, engine::kMaxInputBatchBytes + 8> wire{};
    std::array<engine::ClientInputPayload, engine::kInputRedundancy> decoded{};
    uint32_t ack = 0;
    constexpr uint32_t kAck = engine::kNoBaseline;

    const size_t bytes = engine::EncodeInputBatch(inputs.data(), engine::kInputRedundancy, kAck, wire.data(), wire.size());
    CHECK(engine::DecodeInputBatch(wire.data(), 0, decoded, ack) == 0);
    CHECK(engine::DecodeInputBatch(wire.data(), bytes - 1, decoded, ack) == 0);
    CHECK(engine::DecodeInputBatch(wire.data(), bytes + 1, decoded, ack) == 0);

    wire[0] = 0;
    CHECK(engine::DecodeInputBatch(wire.data(), engine::InputBatchBytes(0), decoded, ack) == 0);
    wire[0] = static_cast<uint8_t>(engine::kInputRedundancy + 1);
    CHECK(engine::DecodeInputBatch(wire.data(), engine::InputBatchBytes(engine::kInputRedundancy + 1), decoded, ack) == 0);

    CHECK(engine::EncodeInputBatch(inputs.data(), 0, kAck, wire.data(), wire.size()) == 0);
    CHECK(engine::EncodeInputBatch(inputs.data(), engine::kInputRedundancy + 1, kAck, wire.data(), wire.size()) == 0);
    CHECK(engine::EncodeInputBatch(inputs.data(), engine::kInputRedundancy, kAck, wire.data(),
                                   engine::InputBatchBytes(engine::kInputRedundancy) - 1) == 0);

    std::printf("  -> Zero, oversized and length-mismatched batches rejected on both ends.\n");
}

engine::ClientInputPayload Input(uint32_t sequence) {
    engine::ClientInputPayload input{};
    input.sequence = sequence;
    return input;
}

void TestReorderBufferOrdering() {
    std::printf("[Test] Running Input Reorder Buffer Ordering, Redundancy and Window Advance...\n");
    engine::InputReorderBuffer buffer;
    engine::ClientInputPayload out{};
    engine::InputLoss loss{};

    // Jitter delivers 3 before 1 and 2; simulation still sees 1, 2, 3
    CHECK(buffer.Insert(Input(3), loss) == engine::InputInsert::Accepted);
    CHECK(buffer.Insert(Input(1), loss) == engine::InputInsert::Accepted);
    CHECK(buffer.Insert(Input(2), loss) == engine::InputInsert::Accepted);
    CHECK(buffer.Insert(Input(2), loss) == engine::InputInsert::Redundant);

    for (uint32_t expected = 1; expected <= 3; ++expected) {
        CHECK(buffer.Pop(0, out, loss) && out.sequence == expected);
    }
    CHECK(!buffer.Pop(0, out, loss));
    CHECK(buffer.Insert(Input(1), loss) == engine::InputInsert::Redundant);
    CHECK(loss.missed == 0 && loss.overflow == 0);

    // Client 64 inputs ahead: the window advances past the never-received input 4 instead of wedging
    CHECK(buffer.Insert(Input(4 + engine::kMaxInputQueuePerClient), loss) == engine::InputInsert::Accepted);
    CHECK(loss.missed == 1 && loss.overflow == 0 && buffer.NextSequence() == 5);

    // A buffered input pushed out by the advance is overflow, the hole beside it is missed
    CHECK(buffer.Insert(Input(6), loss) == engine::InputInsert::Accepted);
    CHECK(buffer.Insert(Input(6 + engine::kMaxInputQueuePerClient), loss) == engine::InputInsert::Accepted);
    CHECK(loss.missed == 2 && loss.overflow == 1 && buffer.NextSequence() == 7);

    buffer.Refill(1);
    CHECK(buffer.Insert(Input(7), loss) == engine::InputInsert::Accepted);
    CHECK(buffer.Pop(1, out, loss) && out.sequence == 7);

    std::printf("  -> Out-of-order arrivals consumed in sequence; window advance charges holes as missed, buffered as overflow.\n");
}

void TestReorderBufferHoles() {
    std::printf("[Test] Running Input Reorder Buffer Hole Wait, Late Fill and Skip...\n");
    engine::ClientInputPayload out{};
    engine::InputLoss loss{};

    // Late copy inside the wait window fills the hole with no loss
    engine::InputReorderBuffer late;
    CHECK(late.Insert(Input(1), loss) == engine::InputInsert::Accepted);
    CHECK(late.Insert(Input(3), loss) == engine::InputInsert::Accepted);
    CHECK(late.Pop(100, out, loss) && out.sequence == 1);
    CHECK(!late.Pop(100, out, loss));
    CHECK(late.Insert(Input(2), loss) == engine::InputInsert::Accepted);
    late.Refill(1);
    CHECK(late.Pop(103, out, loss) && out.sequence == 2);
    CHECK(late.Pop(103, out, loss) && out.sequence == 3);
    CHECK(loss.missed == 0);

    // Truly lost input is skipped exactly at the deadline
    engine::InputReorderBuffer lost;
    CHECK(lost.Insert(Input(1), loss) == engine::InputInsert::Accepted);
    CHECK(lost.Insert(Input(3), loss) == engine::InputInsert::Accepted);
    CHECK(lost.Pop(200, out, loss) && out.sequence == 1);
    CHECK(!lost.Pop(200, out, loss));
    lost.Refill(1);
    CHECK(!lost.Pop(200 + engine::kInputHoleWaitTicks - 1, out, loss));
    lost.Refill(1);
    CHECK(lost.Pop(200 + engine::kInputHoleWaitTicks, out, loss) && out.sequence == 3);
    CHECK(loss.missed == 1);

    std::printf("  -> Hole waits %u ticks for a late copy, then skips exactly one input.\n", engine::kInputHoleWaitTicks);
}

void TestReorderBufferTokenPacing() {
    std::printf("[Test] Running Input Token Bucket Pacing (speedhack and burst bound)...\n");
    engine::InputReorderBuffer buffer;
    engine::ClientInputPayload out{};
    engine::InputLoss loss{};

    // Flooding client: at most kMaxInputsPerTick per tick, at most the bucket plus elapsed ticks overall
    uint32_t inserted = 0;
    uint32_t consumed = 0;
    for (uint32_t tick = 0; tick < 200; ++tick) {
        buffer.Refill(1);
        while (inserted < consumed + static_cast<uint32_t>(engine::kMaxInputQueuePerClient)) {
            CHECK(buffer.Insert(Input(++inserted), loss) == engine::InputInsert::Accepted);
        }

        uint32_t this_tick = 0;
        while (buffer.Pop(tick, out, loss)) {
            ++this_tick;
        }
        consumed += this_tick;
        CHECK(this_tick <= engine::kMaxInputsPerTick);
        CHECK(consumed <= engine::kInputBurstTokens + tick + 1);
        if (tick >= 100) {
            CHECK(this_tick == 1);
        }
    }
    CHECK(loss.missed == 0 && loss.overflow == 0);

    std::printf("  -> At most %u inputs per tick; %u consumed over 200 ticks (bucket %u + 1 per tick), then exactly 1 per tick.\n",
                engine::kMaxInputsPerTick, consumed, engine::kInputBurstTokens);
}

void TestReorderBufferHeldUplinkDrains() {
    std::printf("[Test] Running Held Uplink Recovery (250 ms burst arrival)...\n");
    engine::InputReorderBuffer buffer;
    engine::ClientInputPayload out{};
    engine::InputLoss loss{};

    // One input per tick; the uplink holds 32 ticks then releases everything at once
    constexpr uint32_t kHoldStart = 40;
    constexpr uint32_t kHoldTicks = 32;
    uint32_t produced = 0;
    uint32_t delivered = 0;
    uint32_t drained_at = 0;
    for (uint32_t tick = 0; tick < 200; ++tick) {
        buffer.Refill(1);
        ++produced;
        const bool held = tick >= kHoldStart && tick < kHoldStart + kHoldTicks;
        while (!held && delivered < produced) {
            CHECK(buffer.Insert(Input(++delivered), loss) == engine::InputInsert::Accepted);
        }
        while (buffer.Pop(tick, out, loss)) {
        }
        if (tick >= kHoldStart + kHoldTicks && drained_at == 0 && buffer.Depth() == 0) {
            drained_at = tick;
        }
        if (drained_at != 0) {
            CHECK(buffer.Depth() == 0);
        }
    }
    const uint32_t drain_ticks = drained_at - (kHoldStart + kHoldTicks);
    CHECK(drained_at != 0 && drain_ticks <= kHoldTicks / (engine::kMaxInputsPerTick - 1) + 1);
    CHECK(loss.missed == 0 && loss.overflow == 0);

    std::printf("  -> %u-input burst drained in %u ticks at up to %u per tick; no permanent backlog.\n",
                kHoldTicks + 1, drain_ticks, engine::kMaxInputsPerTick);
}

void TestReorderBufferGapAndStallCredit() {
    std::printf("[Test] Running Whole-Gap Skip and Wall-Clock Stall Credit...\n");
    engine::ClientInputPayload out{};
    engine::InputLoss loss{};

    // Inputs 2-4 lost together cost one deadline, not three
    engine::InputReorderBuffer gap;
    CHECK(gap.Insert(Input(1), loss) == engine::InputInsert::Accepted);
    CHECK(gap.Insert(Input(5), loss) == engine::InputInsert::Accepted);
    CHECK(gap.Pop(10, out, loss) && out.sequence == 1);
    CHECK(!gap.Pop(10, out, loss));
    gap.Refill(1);
    CHECK(gap.Pop(10 + engine::kInputHoleWaitTicks, out, loss) && out.sequence == 5);
    CHECK(loss.missed == 3);

    // Bucket spent, then the server stalls 6 ticks: wall-clock credit drains the 7-input backlog at the per-tick cap
    engine::InputReorderBuffer stall;
    uint32_t sequence = 1;
    for (uint32_t tick = 0; stall.Tokens() > 0; ++tick) {
        stall.Refill(0);
        for (uint32_t i = 0; i < engine::kMaxInputsPerTick; ++i, ++sequence) {
            CHECK(stall.Insert(Input(sequence), loss) == engine::InputInsert::Accepted);
        }
        while (stall.Pop(tick, out, loss)) {
        }
    }
    CHECK(stall.Depth() == 0);

    for (uint32_t i = 0; i < 7; ++i, ++sequence) {
        CHECK(stall.Insert(Input(sequence), loss) == engine::InputInsert::Accepted);
    }
    stall.Refill(7);
    uint32_t drained = 0;
    while (stall.Pop(100, out, loss)) {
        ++drained;
    }
    CHECK(drained == engine::kMaxInputsPerTick);
    stall.Refill(1);
    while (stall.Pop(101, out, loss)) {
        ++drained;
    }
    CHECK(drained == 7 && stall.Depth() == 0);

    std::printf("  -> 3-input gap skipped on one %u-tick deadline; 6-tick stall backlog drained by its credit in 2 ticks.\n",
                engine::kInputHoleWaitTicks);
}

engine::PlayerEntity Player(uint32_t slot, float x, float z, float yaw) {
    engine::PlayerEntity player{};
    player.slot_id = slot;
    player.is_active = true;
    player.flags = engine::EntityFlag_Alive | engine::EntityFlag_Grounded;
    player.health = 100;
    player.position = {x, 0.0f, z};
    player.velocity = {7.5f, 0.0f, -2.25f};
    player.view_pitch = -12.5f;
    player.view_yaw = yaw;
    return player;
}

engine::ReplicatedFrame Frame(uint32_t tick, std::initializer_list<engine::PlayerEntity> players) {
    engine::ReplicatedFrame frame;
    frame.tick = tick;
    for (const auto& player : players) {
        frame.present = static_cast<uint16_t>(frame.present | (1u << player.slot_id));
        frame.entities[player.slot_id] = engine::QuantizeEntity(player);
    }
    return frame;
}

void TestQuantizationRoundTrip() {
    std::printf("[Test] Running Replicated State Quantization...\n");

    // Decoded values sit exactly on the grid: requantizing is lossless, so client renders and server rewinds agree bit for bit
    for (const float x : {-49.9f, -12.3456f, 0.0f, 0.001f, 33.33f, 49.99f}) {
        const engine::PlayerEntity player = Player(0, x, -0.5f * x, 123.456f);
        const engine::QuantizedEntity quantized = engine::QuantizeEntity(player);
        engine::PlayerEntity decoded{};
        engine::ApplyQuantized(quantized, decoded);
        CHECK(std::abs(decoded.position.x - x) <= 0.5f / engine::kPositionUnitsPerMetre);
        CHECK(std::abs(decoded.position.z + 0.5f * x) <= 0.5f / engine::kPositionUnitsPerMetre);
        CHECK(engine::QuantizeEntity(decoded) == quantized);
    }

    CHECK(engine::QuantizeYaw(180.0f) == 32768);
    CHECK(engine::QuantizeYaw(359.999f) == 0);
    CHECK(engine::QuantizeSigned(std::nanf(""), engine::kPositionUnitsPerMetre) == 0);
    CHECK(engine::QuantizeSigned(1e9f, engine::kPositionUnitsPerMetre) == 32767);

    std::printf("  -> Positions within %.2f mm, angles within %.4f deg; requantizing decoded state is exact.\n",
                500.0 / engine::kPositionUnitsPerMetre, 0.5 / engine::kAngleUnitsPerDegree);
}

void TestSnapshotDeltaEncoding() {
    std::printf("[Test] Running Delta Snapshot Encoding Against an Acked Baseline...\n");
    const engine::SelfState self{.position = {-16.0123f, 0.25f, 3.0f}, .velocity = {1.0f, 2.0f, 3.0f},
                                 .last_input_seq = 4242, .flags = 3, .health = 77};
    const engine::PlayerEntity owner = Player(0, -16.0f, 0.0f, 90.0f);
    engine::PlayerEntity mover = Player(1, -12.0f, 0.0f, 45.0f);
    const engine::PlayerEntity idle = Player(2, -8.0f, 0.0f, 270.0f);
    const engine::PlayerEntity leaver = Player(3, -4.0f, 0.0f, 10.0f);
    const engine::ReplicatedFrame baseline = Frame(100, {owner, mover, idle, leaver});

    mover.position.x += 0.0586f;
    mover.view_yaw = 47.5f;
    const engine::PlayerEntity joiner = Player(4, 0.0f, 0.0f, 0.0f);
    const engine::ReplicatedFrame current = Frame(116, {owner, mover, idle, joiner});

    // Header, own full-precision state, mover's position and view, idle's empty mask, joiner whole; the leaver costs nothing
    std::array<uint8_t, engine::kMaxSnapshotBytes> wire{};
    const size_t delta_bytes = engine::EncodeSnapshot(current, &baseline, 0, self, wire.data(), wire.size());
    CHECK(delta_bytes == engine::kSnapshotHeaderBytes + engine::kSelfStateBytes + 11 + 1 + engine::kMaxEntityDeltaBytes);

    engine::ReplicatedFrame decoded;
    engine::SelfState decoded_self{};
    uint8_t self_slot = engine::kNoSelfSlot;
    CHECK(engine::DecodeSnapshot(wire.data(), delta_bytes, &baseline, decoded, decoded_self, self_slot));
    CHECK(decoded.tick == 116 && decoded.present == current.present && self_slot == 0);
    for (const uint32_t slot : {1u, 2u, 4u}) {
        CHECK(decoded.entities[slot] == current.entities[slot]);
    }
    CHECK(decoded_self.position.x == self.position.x && decoded_self.velocity.z == self.velocity.z);
    CHECK(decoded_self.last_input_seq == 4242 && decoded_self.flags == 3 && decoded_self.health == 77);

    const size_t full_bytes = engine::EncodeSnapshot(current, nullptr, 0, self, wire.data(), wire.size());
    CHECK(full_bytes == engine::kSnapshotHeaderBytes + engine::kSelfStateBytes + 3 * engine::kMaxEntityDeltaBytes);

    // A full server with nothing to diff against is the bound every buffer is sized for
    engine::ReplicatedFrame crowded;
    crowded.tick = 200;
    for (uint32_t slot = 0; slot < engine::kMaxPlayers; ++slot) {
        crowded.present = static_cast<uint16_t>(crowded.present | (1u << slot));
        crowded.entities[slot] = engine::QuantizeEntity(Player(slot, static_cast<float>(slot), 1.0f, 2.0f));
    }
    CHECK(engine::EncodeSnapshot(crowded, nullptr, 0, self, wire.data(), wire.size()) == engine::kMaxSnapshotBytes);
    CHECK(engine::EncodeSnapshot(crowded, nullptr, 0, self, wire.data(), wire.size() - 1) == 0);

    std::printf("  -> %zu-byte delta vs %zu-byte full encode (legacy full snapshot: 489 bytes); worst case %zu bytes.\n",
                delta_bytes, full_bytes, engine::kMaxSnapshotBytes);
}

void TestSnapshotRejectsBadFraming() {
    std::printf("[Test] Running Delta Snapshot Framing Validation...\n");
    const engine::SelfState self{};
    engine::PlayerEntity mover = Player(1, 5.0f, 5.0f, 0.0f);
    const engine::ReplicatedFrame baseline = Frame(10, {Player(0, 0.0f, 0.0f, 0.0f), mover});
    mover.position.z += 1.0f;
    const engine::ReplicatedFrame current = Frame(11, {Player(0, 0.0f, 0.0f, 0.0f), mover});

    std::array<uint8_t, engine::kMaxSnapshotBytes + 1> wire{};
    const size_t bytes = engine::EncodeSnapshot(current, &baseline, 0, self, wire.data(), engine::kMaxSnapshotBytes);
    CHECK(bytes > 0);

    engine::ReplicatedFrame frame;
    engine::SelfState decoded_self{};
    uint8_t self_slot = 0;
    CHECK(!engine::DecodeSnapshot(wire.data(), bytes, nullptr, frame, decoded_self, self_slot));
    engine::ReplicatedFrame wrong_tick = baseline;
    wrong_tick.tick = 9;
    CHECK(!engine::DecodeSnapshot(wire.data(), bytes, &wrong_tick, frame, decoded_self, self_slot));
    CHECK(!engine::DecodeSnapshot(wire.data(), bytes - 1, &baseline, frame, decoded_self, self_slot));
    CHECK(!engine::DecodeSnapshot(wire.data(), bytes + 1, &baseline, frame, decoded_self, self_slot));

    // A partial mask needs the entity in the baseline it names
    engine::ReplicatedFrame missing_entity = baseline;
    missing_entity.present = 1u;
    CHECK(!engine::DecodeSnapshot(wire.data(), bytes, &missing_entity, frame, decoded_self, self_slot));

    std::array<uint8_t, engine::kMaxSnapshotBytes + 1> corrupt = wire;
    corrupt[engine::kSnapshotHeaderBytes + engine::kSelfStateBytes] = 0x20;
    CHECK(!engine::DecodeSnapshot(corrupt.data(), bytes, &baseline, frame, decoded_self, self_slot));
    corrupt = wire;
    corrupt[9] = static_cast<uint8_t>(corrupt[9] | 0x80);
    CHECK(!engine::DecodeSnapshot(corrupt.data(), bytes, &baseline, frame, decoded_self, self_slot));

    CHECK(engine::DecodeSnapshot(wire.data(), bytes, &baseline, frame, decoded_self, self_slot));
    std::printf("  -> Missing or wrong baselines, truncation, trailing bytes, unknown fields and slots all rejected.\n");
}

} // namespace

int main() {
    std::printf("====================================================\n");
    std::printf(" Agones-128 Engine: Protocol Test Verification\n");
    std::printf("====================================================\n");

    TestInputBatchRoundTrip();
    TestMalformedBatchRejection();
    TestReorderBufferOrdering();
    TestReorderBufferHoles();
    TestReorderBufferTokenPacing();
    TestReorderBufferHeldUplinkDrains();
    TestReorderBufferGapAndStallCredit();
    TestQuantizationRoundTrip();
    TestSnapshotDeltaEncoding();
    TestSnapshotRejectsBadFraming();

    std::printf("\nAll Protocol Verification Checks PASSED (100%%).\n");
    return 0;
}
