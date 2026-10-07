#pragma once

#include "Common.hpp"
#include "GameServerLifecycle.hpp"
#include <atomic>
#include <chrono>
#include <array>
#include <cstdint>
#include <string_view>

namespace engine {

// Linear and power-of-two histogram bounds up to and beyond 7,812.5us budget
constexpr size_t kHistogramBucketCount = 11;
constexpr std::array<int64_t, kHistogramBucketCount> kHistogramBuckets = {
    50, 100, 250, 500, 1000, 2500, 5000, 7000, 7812, 10000, 15000
};

// Cumulative Prometheus histogram over kHistogramBuckets
struct LatencyHistogram {
    std::array<std::atomic<uint64_t>, kHistogramBucketCount> buckets{};
    std::atomic<uint64_t> inf_bucket{0};
    std::atomic<int64_t> sum_us{0};
    std::atomic<uint64_t> count{0};

    void Observe(int64_t value_us) noexcept {
        sum_us.fetch_add(value_us, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < kHistogramBucketCount; ++i) {
            if (value_us <= kHistogramBuckets[i]) {
                buckets[i].fetch_add(1, std::memory_order_relaxed);
            }
        }
        inf_bucket.fetch_add(1, std::memory_order_relaxed);
    }
};

// Lock-free atomic Prometheus metrics repository
class Metrics {
public:
    Metrics() noexcept;

    // Record single tick duration and jitter on hot path
    void RecordTick(int64_t duration_us, int64_t jitter_us, bool overrun) noexcept;

    // Record combat actions and lag compensation outcomes
    void RecordCombat(bool shot, bool hit, bool occluded, bool rejected) noexcept;

    // Fire input refused by the weapon cooldown
    void RecordThrottledShot() noexcept;

    // Update current connected client gauge
    void SetActivePlayers(uint32_t count) noexcept;

    // Mirror Agones GameServer lifecycle state
    void SetGameServerState(GameServerState state) noexcept;

    // Mirror cumulative network ingest counters
    void SetInputCounters(uint64_t accepted, uint64_t missed, uint64_t redundant,
                          uint64_t overflow, uint64_t malformed) noexcept;

    // Deepest per-session input backlog over the last export interval
    void SetInputBacklogPeak(uint32_t depth) noexcept;

    // Mirror the ENet block pool high-water mark and refused allocations
    void SetNetPool(size_t peak_bytes, uint64_t failures) noexcept;

    // Serialize all registered metrics into Prometheus text format
    size_t Serialize(char* out_buffer, size_t max_bytes) const noexcept;

    [[nodiscard]] uint64_t GetTotalTicks() const noexcept { return total_ticks_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t GetOverruns() const noexcept { return tick_overruns_.load(std::memory_order_relaxed); }

private:
    std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
    std::atomic<uint64_t> total_ticks_{0};
    std::atomic<uint64_t> tick_overruns_{0};
    std::atomic<uint32_t> active_players_{0};
    std::atomic<uint8_t> gameserver_state_{0};

    std::atomic<int64_t> last_tick_duration_us_{0};
    std::atomic<int64_t> last_tick_jitter_us_{0};

    LatencyHistogram tick_duration_{};
    LatencyHistogram tick_lateness_{};

    std::atomic<uint64_t> total_shots_{0};
    std::atomic<uint64_t> confirmed_hits_{0};
    std::atomic<uint64_t> occluded_shots_{0};
    std::atomic<uint64_t> rejected_rewinds_{0};
    std::atomic<uint64_t> throttled_shots_{0};

    std::atomic<uint64_t> inputs_accepted_{0};
    std::atomic<uint64_t> inputs_missed_{0};
    std::atomic<uint64_t> inputs_redundant_{0};
    std::atomic<uint64_t> inputs_overflow_{0};
    std::atomic<uint64_t> packets_malformed_{0};
    std::atomic<uint32_t> input_backlog_peak_{0};
    std::atomic<uint64_t> net_pool_peak_bytes_{0};
    std::atomic<uint64_t> net_pool_failures_{0};
};

} // namespace engine
