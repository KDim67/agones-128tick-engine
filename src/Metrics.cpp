#include "engine/Metrics.hpp"
#include <cstdio>
#include <cstring>

namespace engine {

Metrics::Metrics() noexcept {
    for (size_t i = 0; i < kHistogramBucketCount; ++i) {
        histogram_buckets_[i].store(0, std::memory_order_relaxed);
    }
}

void Metrics::RecordTick(int64_t duration_us, int64_t jitter_us, bool overrun) noexcept {
    total_ticks_.fetch_add(1, std::memory_order_relaxed);
    if (overrun) {
        tick_overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    last_tick_duration_us_.store(duration_us, std::memory_order_relaxed);
    last_tick_jitter_us_.store(jitter_us, std::memory_order_relaxed);

    histogram_sum_us_.fetch_add(duration_us, std::memory_order_relaxed);
    histogram_count_.fetch_add(1, std::memory_order_relaxed);

    // Cumulative histogram bucket distribution
    for (size_t i = 0; i < kHistogramBucketCount; ++i) {
        if (duration_us <= kHistogramBuckets[i]) {
            histogram_buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
    histogram_inf_bucket_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::RecordCombat(bool shot, bool hit, bool occluded, bool rejected) noexcept {
    if (shot)     total_shots_.fetch_add(1, std::memory_order_relaxed);
    if (hit)      confirmed_hits_.fetch_add(1, std::memory_order_relaxed);
    if (occluded) occluded_shots_.fetch_add(1, std::memory_order_relaxed);
    if (rejected) rejected_rewinds_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::SetActivePlayers(uint32_t count) noexcept {
    active_players_.store(count, std::memory_order_relaxed);
}

size_t Metrics::Serialize(char* out_buffer, size_t max_bytes) const noexcept {
    if (!out_buffer || max_bytes == 0) {
        return 0;
    }

    int written = 0;
    int offset = 0;

    auto append = [&](const char* format, auto... args) {
        if (offset < static_cast<int>(max_bytes)) {
            written = std::snprintf(out_buffer + offset, max_bytes - static_cast<size_t>(offset), format, args...);
            if (written > 0) {
                offset += written;
            }
        }
    };

    append("# HELP game_tick_duration_us 128Hz tick execution duration in microseconds\n");
    append("# TYPE game_tick_duration_us histogram\n");
    for (size_t i = 0; i < kHistogramBucketCount; ++i) {
        append("game_tick_duration_us_bucket{le=\"%lld\"} %llu\n",
               static_cast<long long>(kHistogramBuckets[i]),
               static_cast<unsigned long long>(histogram_buckets_[i].load(std::memory_order_relaxed)));
    }
    append("game_tick_duration_us_bucket{le=\"+Inf\"} %llu\n",
           static_cast<unsigned long long>(histogram_inf_bucket_.load(std::memory_order_relaxed)));
    append("game_tick_duration_us_sum %lld\n",
           static_cast<long long>(histogram_sum_us_.load(std::memory_order_relaxed)));
    append("game_tick_duration_us_count %llu\n",
           static_cast<unsigned long long>(histogram_count_.load(std::memory_order_relaxed)));

    append("# HELP game_active_players Currently connected players\n");
    append("# TYPE game_active_players gauge\n");
    append("game_active_players %u\n", active_players_.load(std::memory_order_relaxed));

    append("# HELP game_total_ticks Monotonically increasing tick counter\n");
    append("# TYPE game_total_ticks counter\n");
    append("game_total_ticks %llu\n",
           static_cast<unsigned long long>(total_ticks_.load(std::memory_order_relaxed)));

    append("# HELP game_tick_overruns Total ticks exceeding 7812.5us budget\n");
    append("# TYPE game_tick_overruns counter\n");
    append("game_tick_overruns %llu\n",
           static_cast<unsigned long long>(tick_overruns_.load(std::memory_order_relaxed)));

    append("# HELP game_tick_jitter_us Absolute tick start jitter relative to scheduled deadline\n");
    append("# TYPE game_tick_jitter_us gauge\n");
    append("game_tick_jitter_us %lld\n",
           static_cast<long long>(last_tick_jitter_us_.load(std::memory_order_relaxed)));

    append("# HELP game_ballistic_shots Total firing commands evaluated\n");
    append("# TYPE game_ballistic_shots counter\n");
    append("game_ballistic_shots %llu\n",
           static_cast<unsigned long long>(total_shots_.load(std::memory_order_relaxed)));

    append("# HELP game_confirmed_hits Total confirmed raycast hits\n");
    append("# TYPE game_confirmed_hits counter\n");
    append("game_confirmed_hits %llu\n",
           static_cast<unsigned long long>(confirmed_hits_.load(std::memory_order_relaxed)));

    append("# HELP game_occluded_shots Ballistic shots occluded by world geometry\n");
    append("# TYPE game_occluded_shots counter\n");
    append("game_occluded_shots %llu\n",
           static_cast<unsigned long long>(occluded_shots_.load(std::memory_order_relaxed)));

    append("# HELP game_rejected_rewinds Rewind requests outside 1-second retention window\n");
    append("# TYPE game_rejected_rewinds counter\n");
    append("game_rejected_rewinds %llu\n",
           static_cast<unsigned long long>(rejected_rewinds_.load(std::memory_order_relaxed)));

    return static_cast<size_t>(std::min(offset, static_cast<int>(max_bytes - 1)));
}

} // namespace engine
