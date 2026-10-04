#include "engine/Protocol.hpp"
#include "engine/InputReorderBuffer.hpp"
#include "Check.hpp"

#include <array>
#include <cstdio>

namespace {

static_assert(engine::kMaxControlPayloadBytes >= sizeof(engine::HitNotificationPayload));
static_assert(engine::kMaxInputBatchBytes == 1 + engine::kInputRedundancy * sizeof(engine::ClientInputPayload));

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
        const size_t bytes = engine::EncodeInputBatch(inputs.data(), count, wire.data(), wire.size());
        CHECK(bytes == engine::InputBatchBytes(count));

        std::array<engine::ClientInputPayload, engine::kInputRedundancy> decoded{};
        CHECK(engine::DecodeInputBatch(wire.data(), bytes, decoded) == count);
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

    const size_t bytes = engine::EncodeInputBatch(inputs.data(), engine::kInputRedundancy, wire.data(), wire.size());
    CHECK(engine::DecodeInputBatch(wire.data(), 0, decoded) == 0);
    CHECK(engine::DecodeInputBatch(wire.data(), bytes - 1, decoded) == 0);
    CHECK(engine::DecodeInputBatch(wire.data(), bytes + 1, decoded) == 0);

    wire[0] = 0;
    CHECK(engine::DecodeInputBatch(wire.data(), engine::InputBatchBytes(0), decoded) == 0);
    wire[0] = static_cast<uint8_t>(engine::kInputRedundancy + 1);
    CHECK(engine::DecodeInputBatch(wire.data(), engine::InputBatchBytes(engine::kInputRedundancy + 1), decoded) == 0);

    CHECK(engine::EncodeInputBatch(inputs.data(), 0, wire.data(), wire.size()) == 0);
    CHECK(engine::EncodeInputBatch(inputs.data(), engine::kInputRedundancy + 1, wire.data(), wire.size()) == 0);
    CHECK(engine::EncodeInputBatch(inputs.data(), engine::kInputRedundancy, wire.data(),
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

    std::printf("\nAll Protocol Verification Checks PASSED (100%%).\n");
    return 0;
}
