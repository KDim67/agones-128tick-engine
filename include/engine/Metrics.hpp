#pragma once

#include "Common.hpp"
#include "GameServerLifecycle.hpp"
#include <atomic>
#include <array>
#include <cstdint>
#include <string_view>

namespace engine {

// Linear and power-of-two histogram bounds up to and beyond 7,812.5us budget
constexpr size_t kHistogramBucketCount = 11;
constexpr std::array<int64_t, kHistogramBucketCount> kHistogramBuckets = {
    50, 100, 250, 500, 1000, 2500, 5000, 7000, 7812, 10000, 15000
};

// Lock-free atomic Prometheus metrics repository
class Metrics {
public:
    Metrics() noexcept;

    // Record single tick duration and jitter on hot path
    void RecordTick(int64_t duration_us, int64_t jitter_us, bool overrun) noexcept;

    // Record combat actions and lag compensation outcomes
    void RecordCombat(bool shot, bool hit, bool occluded, bool rejected) noexcept;

    // Update current connected client gauge
    void SetActivePlayers(uint32_t count) noexcept;

    // Mirror Agones GameServer lifecycle state
    void SetGameServerState(GameServerState state) noexcept;

    // Serialize all registered metrics into Prometheus text format
    size_t Serialize(char* out_buffer, size_t max_bytes) const noexcept;

    [[nodiscard]] uint64_t GetTotalTicks() const noexcept { return total_ticks_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t GetOverruns() const noexcept { return tick_overruns_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> total_ticks_{0};
    std::atomic<uint64_t> tick_overruns_{0};
    std::atomic<uint32_t> active_players_{0};
    std::atomic<uint8_t> gameserver_state_{0};

    std::atomic<int64_t> last_tick_duration_us_{0};
    std::atomic<int64_t> last_tick_jitter_us_{0};

    std::array<std::atomic<uint64_t>, kHistogramBucketCount> histogram_buckets_{};
    std::atomic<uint64_t> histogram_inf_bucket_{0};
    std::atomic<int64_t> histogram_sum_us_{0};
    std::atomic<uint64_t> histogram_count_{0};

    std::atomic<uint64_t> total_shots_{0};
    std::atomic<uint64_t> confirmed_hits_{0};
    std::atomic<uint64_t> occluded_shots_{0};
    std::atomic<uint64_t> rejected_rewinds_{0};
};

} // namespace engine
