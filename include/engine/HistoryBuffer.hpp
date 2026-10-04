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

// Sub-tick alpha bounds shared by server rewind and client render
constexpr float kMaxSubTickAlpha = 0.999f;
constexpr float kSubTickSnapEpsilon = 1e-4f;

// Out-of-range or NaN client fractions collapse to the base tick
[[nodiscard]] inline float SanitizeSubTickAlpha(float alpha) noexcept {
    return std::isfinite(alpha) ? std::clamp(alpha, 0.0f, kMaxSubTickAlpha) : 0.0f;
}

// Positional lerp between two snapshots; flags and health follow the base snapshot
inline void InterpolateSnapshot(const WorldSnapshot& a, const WorldSnapshot& b, float alpha, WorldSnapshot& out) noexcept {
    out.tick = a.tick;
    out.timestamp_ns = a.timestamp_ns;

    for (size_t i = 0; i < kMaxPlayers; ++i) {
        const auto& pa = a.players[i];
        const auto& pb = b.players[i];

        out.players[i] = pa;
        if (pa.is_active && pb.is_active && pa.spawn_id == pb.spawn_id) {
            out.players[i].position = Vec3::Lerp(pa.position, pb.position, alpha);
            out.players[i].velocity = Vec3::Lerp(pa.velocity, pb.velocity, alpha);
        }
    }
}

// Rewind request for one render time: ack tick, span to the paired tick (0 holds the ack tick) and sub-tick weight
struct RenderPair {
    uint32_t tick{0};
    uint32_t span{1};
    float alpha{0.0f};
    bool gap{false};
};

// Pairs a fractional render tick with received snapshots, bridging lost ones with a span the server can replay
template <typename HasTick>
[[nodiscard]] bool SelectRenderPair(double render_tick, uint32_t newest_tick, uint32_t search_ticks,
                                    HasTick&& has_tick, RenderPair& out) noexcept {
    if (!(render_tick >= 0.0) || render_tick > static_cast<double>(newest_tick)) {
        return false;
    }

    const auto base = static_cast<uint32_t>(render_tick);
    out.tick = base;
    out.span = 1;
    out.alpha = SanitizeSubTickAlpha(static_cast<float>(render_tick - static_cast<double>(base)));
    out.gap = false;

    const bool have_base = has_tick(base);
    if (have_base && (out.alpha <= kSubTickSnapEpsilon || has_tick(base + 1))) {
        return true;
    }

    uint32_t lo = 0;
    bool have_lo = false;
    for (uint32_t back = 0; back < search_ticks && back <= base && !have_lo; ++back) {
        have_lo = has_tick(base - back);
        lo = base - back;
    }
    if (!have_lo) {
        return false;
    }

    uint32_t hi = 0;
    bool have_hi = false;
    for (uint32_t ahead = 1; ahead < search_ticks && base + ahead <= newest_tick && !have_hi; ++ahead) {
        have_hi = has_tick(base + ahead);
        hi = base + ahead;
    }

    out.gap = true;
    out.tick = lo;
    out.span = (have_hi && hi - lo <= kMaxInterpSpanTicks) ? hi - lo : 0;
    out.alpha = out.span == 0 ? 0.0f
        : SanitizeSubTickAlpha(static_cast<float>((render_tick - static_cast<double>(lo)) / static_cast<double>(out.span)));
    return true;
}

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
        const WorldSnapshot* snapshot = Find(target_tick);
        if (!snapshot) {
            return false;
        }
        out_snapshot = *snapshot;
        return true;
    }

    // Replays a RenderPair; integer tick plus separate alpha because a float tick loses sub-tick precision past 2^17
    [[nodiscard]] bool GetInterpolatedSnapshot(uint32_t tick, uint32_t span, float alpha, WorldSnapshot& out_snapshot) const noexcept {
        if (span == 0 || alpha <= kSubTickSnapEpsilon) {
            return GetSnapshot(tick, out_snapshot);
        }

        const WorldSnapshot* snap_a = Find(tick);
        const WorldSnapshot* snap_b = Find(tick + span);
        if (!snap_a || !snap_b) {
            return false;
        }

        InterpolateSnapshot(*snap_a, *snap_b, alpha, out_snapshot);
        return true;
    }

    [[nodiscard]] uint32_t GetLatestTick() const noexcept { return latest_tick_; }
    [[nodiscard]] size_t GetRecordedCount() const noexcept { return count_; }

private:
    [[nodiscard]] const WorldSnapshot* Find(uint32_t target_tick) const noexcept {
        return IsTickValid(target_tick) ? &ring_[target_tick % kCapacity] : nullptr;
    }

    std::array<WorldSnapshot, kCapacity> ring_{};
    uint32_t latest_tick_{0};
    size_t count_{0};
};

} // namespace engine
