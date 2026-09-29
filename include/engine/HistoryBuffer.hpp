#pragma once

#include "Common.hpp"
#include "WorldState.hpp"
#include <array>
#include <cstdint>

namespace engine {

struct WorldSnapshot {
    uint32_t tick{0};
    uint64_t timestamp_ns{0};
    std::array<PlayerEntity, kMaxPlayers> players{};
};

// 128-slot circular history buffer (1s at 128Hz)
class HistoryBuffer {
public:
    static constexpr size_t kCapacity = kTickRate;

    constexpr HistoryBuffer() noexcept = default;

    void Record(uint32_t tick, uint64_t timestamp_ns, const std::array<PlayerEntity, kMaxPlayers>& players) noexcept {
        const size_t index = tick % kCapacity;
        ring_[index].tick = tick;
        ring_[index].timestamp_ns = timestamp_ns;
        ring_[index].players = players;

        latest_tick_ = tick;
        if (count_ < kCapacity) {
            ++count_;
        }
    }

    [[nodiscard]] bool IsTickValid(uint32_t target_tick) const noexcept {
        if (count_ == 0 || target_tick > latest_tick_) {
            return false;
        }
        if ((latest_tick_ - target_tick) >= kCapacity) {
            return false;
        }
        const size_t index = target_tick % kCapacity;
        return ring_[index].tick == target_tick;
    }

    [[nodiscard]] bool GetSnapshot(uint32_t target_tick, WorldSnapshot& out_snapshot) const noexcept {
        if (!IsTickValid(target_tick)) {
            return false;
        }
        const size_t index = target_tick % kCapacity;
        if (ring_[index].tick != target_tick) {
            return false;
        }
        out_snapshot = ring_[index];
        return true;
    }

    // Sub-tick linear interpolation
    [[nodiscard]] bool GetInterpolatedSnapshot(float fractional_tick, WorldSnapshot& out_snapshot) const noexcept {
        const uint32_t tick_a = static_cast<uint32_t>(std::floor(fractional_tick));
        const uint32_t tick_b = tick_a + 1;
        const float alpha = fractional_tick - static_cast<float>(tick_a);

        if (alpha <= 1e-4f) {
            return GetSnapshot(tick_a, out_snapshot);
        }

        WorldSnapshot snap_a;
        WorldSnapshot snap_b;
        if (!GetSnapshot(tick_a, snap_a) || !GetSnapshot(tick_b, snap_b)) {
            return false;
        }

        out_snapshot.tick = tick_a;
        out_snapshot.timestamp_ns = snap_a.timestamp_ns;

        for (size_t i = 0; i < kMaxPlayers; ++i) {
            const auto& pa = snap_a.players[i];
            const auto& pb = snap_b.players[i];

            out_snapshot.players[i] = pa;
            if (pa.is_active && pb.is_active) {
                out_snapshot.players[i].position = Vec3::Lerp(pa.position, pb.position, alpha);
                out_snapshot.players[i].velocity = Vec3::Lerp(pa.velocity, pb.velocity, alpha);
            }
        }
        return true;
    }

    [[nodiscard]] uint32_t GetLatestTick() const noexcept { return latest_tick_; }
    [[nodiscard]] size_t GetRecordedCount() const noexcept { return count_; }

private:
    std::array<WorldSnapshot, kCapacity> ring_{};
    uint32_t latest_tick_{0};
    size_t count_{0};
};

} // namespace engine
