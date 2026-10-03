#include "engine/TickLoop.hpp"
#include <thread>
#include <cmath>
#include <cstdio>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#endif

namespace engine {

TickLoop::TickLoop() noexcept {
#if defined(_WIN32)
    // 1ms timer resolution on Windows
    timeBeginPeriod(1);
#endif
}

TickLoop::~TickLoop() {
#if defined(_WIN32)
    // Restore default timer quantum
    timeEndPeriod(1);
#endif
}

void TickLoop::RequestStop() noexcept {
    // Sticky: a SIGTERM during startup must survive Run() entry
    stop_requested_.store(true, std::memory_order_relaxed);
}

void TickLoop::Run(TickFn tick_fn, void* user_data, uint64_t max_ticks) noexcept {
    // Realtime priority elevation; denied without CAP_SYS_NICE in containers
    if (!SetThreadRealtimePriority()) {
        std::fprintf(stderr, "Warning: Realtime scheduling denied, tick thread runs at default priority\n");
    }

    is_running_.store(true, std::memory_order_relaxed);

    TimePoint next_tick_target = Clock::now();
    last_stats_time_ = next_tick_target;
    stats_tick_anchor_ = 0;
    exec_accum_ns_ = 0;
    telemetry_.total_ticks = 0;
    telemetry_.current_tps = static_cast<float>(kTickRate);

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (max_ticks > 0 && telemetry_.total_ticks >= max_ticks) {
            break;
        }

        const TimePoint tick_start = Clock::now();

        // Tick jitter relative to scheduled deadline
        const int64_t jitter_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_start - next_tick_target).count();
        telemetry_.last_jitter_ns = jitter_ns;
        const int64_t abs_jitter = std::abs(jitter_ns);
        if (abs_jitter > telemetry_.peak_jitter_ns) {
            telemetry_.peak_jitter_ns = abs_jitter;
        }

        tick_fn(telemetry_.total_ticks, kFixedDeltaTimeSec, user_data);
        ++telemetry_.total_ticks;

        const TimePoint exec_end = Clock::now();
        const auto exec_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(exec_end - tick_start);
        telemetry_.last_execution_time_ns = exec_duration.count();
        exec_accum_ns_ += telemetry_.last_execution_time_ns;

        if (telemetry_.last_execution_time_ns > telemetry_.peak_execution_time_ns) {
            telemetry_.peak_execution_time_ns = telemetry_.last_execution_time_ns;
        }

        next_tick_target += kTickDurationNs;

        // Overrun recovery
        if (exec_end > next_tick_target) {
            ++telemetry_.overruns;

            // Clamp drift if stalled beyond catchup threshold
            const auto lag = std::chrono::duration_cast<std::chrono::nanoseconds>(exec_end - next_tick_target);
            if (lag > (kTickDurationNs * kMaxCatchupTicks)) {
                next_tick_target = exec_end + kTickDurationNs;
            }
        } else {
            auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(next_tick_target - Clock::now());
            if (remaining > kSleepThresholdNs) {
                std::this_thread::sleep_for(remaining - kSleepThresholdNs);
            }

            // Spin-wait final microsecond slice
            while (Clock::now() < next_tick_target) {
                CpuPause();
            }
        }

        // Rolling 1s execution telemetry
        const auto stats_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - last_stats_time_);
        if (stats_elapsed.count() >= 1000) {
            const uint64_t ticks_delta = telemetry_.total_ticks - stats_tick_anchor_;
            telemetry_.current_tps = static_cast<float>(ticks_delta * 1000) / static_cast<float>(stats_elapsed.count());
            if (ticks_delta > 0) {
                telemetry_.avg_execution_time_ns = exec_accum_ns_ / static_cast<int64_t>(ticks_delta);
            }
            exec_accum_ns_ = 0;
            last_stats_time_ = exec_end;
            stats_tick_anchor_ = telemetry_.total_ticks;
        }
    }

    is_running_.store(false, std::memory_order_relaxed);
}

} // namespace engine
