#include "engine/Metrics.hpp"
#include <cstdio>
#include <cstring>

namespace engine {

Metrics::Metrics() noexcept {
    for (size_t i = 0; i < kHistogramBucketCount; ++i) {
        tick_duration_.buckets[i].store(0, std::memory_order_relaxed);
        tick_lateness_.buckets[i].store(0, std::memory_order_relaxed);
    }
}

void Metrics::RecordTick(int64_t duration_us, int64_t jitter_us, bool overrun) noexcept {
    total_ticks_.fetch_add(1, std::memory_order_relaxed);
    if (overrun) {
        tick_overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    last_tick_duration_us_.store(duration_us, std::memory_order_relaxed);
    last_tick_jitter_us_.store(jitter_us, std::memory_order_relaxed);

    tick_duration_.Observe(duration_us);

    // Early starts clamp to zero lateness
    tick_lateness_.Observe(jitter_us > 0 ? jitter_us : 0);
}

void Metrics::RecordCombat(bool shot, bool hit, bool occluded, bool rejected) noexcept {
    if (shot)     total_shots_.fetch_add(1, std::memory_order_relaxed);
    if (hit)      confirmed_hits_.fetch_add(1, std::memory_order_relaxed);
    if (occluded) occluded_shots_.fetch_add(1, std::memory_order_relaxed);
    if (rejected) rejected_rewinds_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::RecordThrottledShot() noexcept {
    throttled_shots_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::SetActivePlayers(uint32_t count) noexcept {
    active_players_.store(count, std::memory_order_relaxed);
}

void Metrics::SetGameServerState(GameServerState state) noexcept {
    gameserver_state_.store(static_cast<uint8_t>(state), std::memory_order_relaxed);
}

void Metrics::SetInputBacklogPeak(uint32_t depth) noexcept {
    input_backlog_peak_.store(depth, std::memory_order_relaxed);
}

void Metrics::SetTransportHealth(size_t pool_peak_bytes, uint64_t pool_failures, uint64_t backlog_disconnects) noexcept {
    net_pool_peak_bytes_.store(pool_peak_bytes, std::memory_order_relaxed);
    net_pool_failures_.store(pool_failures, std::memory_order_relaxed);
    backlog_disconnects_.store(backlog_disconnects, std::memory_order_relaxed);
}

void Metrics::SetInputCounters(uint64_t accepted, uint64_t missed, uint64_t redundant,
                               uint64_t overflow, uint64_t malformed) noexcept {
    inputs_accepted_.store(accepted, std::memory_order_relaxed);
    inputs_missed_.store(missed, std::memory_order_relaxed);
    inputs_redundant_.store(redundant, std::memory_order_relaxed);
    inputs_overflow_.store(overflow, std::memory_order_relaxed);
    packets_malformed_.store(malformed, std::memory_order_relaxed);
}

size_t Metrics::Serialize(char* out_buffer, size_t max_bytes) const noexcept {
    if (!out_buffer || max_bytes == 0) {
        return 0;
    }

    int written = 0;
    int offset = 0;

    // Literal lines go through "%s" so no caller string is ever parsed as a format
    auto append = [&](const char* format, auto... args) {
        if (offset < static_cast<int>(max_bytes)) {
            const size_t room = max_bytes - static_cast<size_t>(offset);
            if constexpr (sizeof...(args) == 0) {
                written = std::snprintf(out_buffer + offset, room, "%s", format);
            } else {
                written = std::snprintf(out_buffer + offset, room, format, args...);
            }
            if (written > 0) {
                offset += written;
            }
        }
    };

    auto append_histogram = [&](const char* name, const char* help, const LatencyHistogram& histogram) {
        append("# HELP %s %s\n", name, help);
        append("# TYPE %s histogram\n", name);
        for (size_t i = 0; i < kHistogramBucketCount; ++i) {
            append("%s_bucket{le=\"%lld\"} %llu\n", name,
                   static_cast<long long>(kHistogramBuckets[i]),
                   static_cast<unsigned long long>(histogram.buckets[i].load(std::memory_order_relaxed)));
        }
        append("%s_bucket{le=\"+Inf\"} %llu\n", name,
               static_cast<unsigned long long>(histogram.inf_bucket.load(std::memory_order_relaxed)));
        append("%s_sum %lld\n", name,
               static_cast<long long>(histogram.sum_us.load(std::memory_order_relaxed)));
        append("%s_count %llu\n", name,
               static_cast<unsigned long long>(histogram.count.load(std::memory_order_relaxed)));
    };

    append_histogram("game_tick_duration_us", "128Hz tick execution duration in microseconds", tick_duration_);
    append_histogram("game_tick_lateness_us", "Tick start lateness behind its scheduled deadline in microseconds", tick_lateness_);

    append("# HELP game_active_players Currently connected players\n");
    append("# TYPE game_active_players gauge\n");
    append("game_active_players %u\n", active_players_.load(std::memory_order_relaxed));

    // Server-side clock so rate math is immune to scrape latency
    append("# HELP game_uptime_seconds Seconds since the metrics registry was created\n");
    append("# TYPE game_uptime_seconds gauge\n");
    append("game_uptime_seconds %.6f\n",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count());

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

    append("# HELP game_throttled_shots Fire inputs refused by the weapon cooldown\n");
    append("# TYPE game_throttled_shots counter\n");
    append("game_throttled_shots %llu\n",
           static_cast<unsigned long long>(throttled_shots_.load(std::memory_order_relaxed)));

    append("# HELP game_client_inputs Client inputs by ingest outcome\n");
    append("# TYPE game_client_inputs counter\n");
    append("game_client_inputs{outcome=\"accepted\"} %llu\n",
           static_cast<unsigned long long>(inputs_accepted_.load(std::memory_order_relaxed)));
    append("game_client_inputs{outcome=\"missed\"} %llu\n",
           static_cast<unsigned long long>(inputs_missed_.load(std::memory_order_relaxed)));
    append("game_client_inputs{outcome=\"redundant\"} %llu\n",
           static_cast<unsigned long long>(inputs_redundant_.load(std::memory_order_relaxed)));
    append("game_client_inputs{outcome=\"overflow\"} %llu\n",
           static_cast<unsigned long long>(inputs_overflow_.load(std::memory_order_relaxed)));

    append("# HELP game_input_backlog_peak Deepest per-session input reorder backlog over the last second, in ticks\n");
    append("# TYPE game_input_backlog_peak gauge\n");
    append("game_input_backlog_peak %u\n", input_backlog_peak_.load(std::memory_order_relaxed));

    append("# HELP game_malformed_packets Packets rejected for version, framing or field validation\n");
    append("# TYPE game_malformed_packets counter\n");
    append("game_malformed_packets %llu\n",
           static_cast<unsigned long long>(packets_malformed_.load(std::memory_order_relaxed)));

    append("# HELP game_net_pool_peak_bytes High-water mark of the preallocated ENet block pool\n");
    append("# TYPE game_net_pool_peak_bytes gauge\n");
    append("game_net_pool_peak_bytes %llu\n",
           static_cast<unsigned long long>(net_pool_peak_bytes_.load(std::memory_order_relaxed)));

    append("# HELP game_net_pool_failures ENet allocations the pool refused; each one drops a packet\n");
    append("# TYPE game_net_pool_failures counter\n");
    append("game_net_pool_failures %llu\n",
           static_cast<unsigned long long>(net_pool_failures_.load(std::memory_order_relaxed)));

    append("# HELP game_reliable_backlog_disconnects Peers cut off for leaving too many reliable packets unacked\n");
    append("# TYPE game_reliable_backlog_disconnects counter\n");
    append("game_reliable_backlog_disconnects %llu\n",
           static_cast<unsigned long long>(backlog_disconnects_.load(std::memory_order_relaxed)));

    // Stateset: one series per state, active at 1
    append("# HELP game_agones_state Agones GameServer lifecycle state reported by the SDK sidecar\n");
    append("# TYPE game_agones_state gauge\n");
    const auto current_state = static_cast<size_t>(gameserver_state_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < kGameServerStateCount; ++i) {
        append("game_agones_state{state=\"%.*s\"} %d\n",
               static_cast<int>(kGameServerStateNames[i].size()),
               kGameServerStateNames[i].data(),
               i == current_state ? 1 : 0);
    }

    return static_cast<size_t>(std::min(offset, static_cast<int>(max_bytes - 1)));
}

} // namespace engine
