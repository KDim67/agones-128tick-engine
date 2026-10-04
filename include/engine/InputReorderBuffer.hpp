#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace engine {

enum class InputInsert : uint8_t {
    Accepted,
    Redundant
};

// Sequences that will never be simulated: missed never arrived, overflow arrived but fell out of the window
struct InputLoss {
    uint32_t missed{0};
    uint32_t overflow{0};
};

// Sequence-indexed jitter buffer: arrivals land in any order, simulation consumes strictly in order
class InputReorderBuffer {
public:
    void Reset() noexcept {
        *this = InputReorderBuffer{};
    }

    // An input past the window advances it instead of wedging; skipped sequences are charged as loss
    [[nodiscard]] InputInsert Insert(const ClientInputPayload& input, InputLoss& loss) noexcept {
        if (input.sequence < next_sequence_) {
            return InputInsert::Redundant;
        }
        if (input.sequence - next_sequence_ >= kMaxInputQueuePerClient) {
            const uint32_t new_next = input.sequence - static_cast<uint32_t>(kMaxInputQueuePerClient) + 1;
            const uint32_t skipped = new_next - next_sequence_;
            const auto scan = std::min<uint32_t>(skipped, static_cast<uint32_t>(kMaxInputQueuePerClient));
            uint32_t buffered = 0;
            for (uint32_t i = 0; i < scan; ++i) {
                auto& slot = slots_[(next_sequence_ + i) % kMaxInputQueuePerClient];
                if (slot.sequence == next_sequence_ + i) {
                    ++buffered;
                }
                slot.sequence = 0;
            }
            loss.overflow += buffered;
            loss.missed += skipped - buffered;
            next_sequence_ = new_next;
            hole_pending_ = false;
        }

        auto& slot = slots_[input.sequence % kMaxInputQueuePerClient];
        if (slot.sequence == input.sequence) {
            return InputInsert::Redundant;
        }
        slot = input;
        if (input.sequence > highest_sequence_) {
            highest_sequence_ = input.sequence;
        }
        return InputInsert::Accepted;
    }

    // Once per tick; credit is wall-clock ticks so a stalled server cannot ratchet the backlog
    void Refill(uint32_t credit) noexcept {
        tokens_ = std::min(kInputBurstTokens, tokens_ + std::min(credit, kInputBurstTokens));
        tick_budget_ = kMaxInputsPerTick;
    }

    // Next in-order input; a hole waits kInputHoleWaitTicks for a late or redundant copy, then the whole gap is skipped
    [[nodiscard]] bool Pop(uint32_t current_tick, ClientInputPayload& out, InputLoss& loss) noexcept {
        while (tokens_ > 0 && tick_budget_ > 0) {
            auto& slot = slots_[next_sequence_ % kMaxInputQueuePerClient];
            if (slot.sequence == next_sequence_) {
                out = slot;
                slot.sequence = 0;
                ++next_sequence_;
                --tokens_;
                --tick_budget_;
                hole_pending_ = false;
                return true;
            }

            if (highest_sequence_ < next_sequence_) {
                return false;
            }
            if (!hole_pending_) {
                hole_pending_ = true;
                hole_since_tick_ = current_tick;
                return false;
            }
            if (current_tick - hole_since_tick_ < kInputHoleWaitTicks) {
                return false;
            }

            // One deadline per loss event, not one per missing input
            while (slots_[next_sequence_ % kMaxInputQueuePerClient].sequence != next_sequence_) {
                ++loss.missed;
                ++next_sequence_;
            }
            hole_pending_ = false;
        }
        return false;
    }

    [[nodiscard]] uint32_t NextSequence() const noexcept { return next_sequence_; }
    [[nodiscard]] uint32_t Depth() const noexcept {
        return highest_sequence_ >= next_sequence_ ? highest_sequence_ - next_sequence_ + 1 : 0;
    }
    [[nodiscard]] uint32_t Tokens() const noexcept { return tokens_; }

private:
    std::array<ClientInputPayload, kMaxInputQueuePerClient> slots_{};
    uint32_t next_sequence_{1};
    uint32_t highest_sequence_{0};
    uint32_t hole_since_tick_{0};
    uint32_t tokens_{kInputBurstTokens};
    uint32_t tick_budget_{kMaxInputsPerTick};
    bool hole_pending_{false};
};

} // namespace engine
