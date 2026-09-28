#pragma once

#include "Common.hpp"
#include <atomic>
#include <chrono>

namespace engine {

// Real-time runtime telemetry metrics
struct TickTelemetry {
    uint64_t total_ticks{0};
    uint64_t overruns{0};
    int64_t last_execution_time_ns{0};
    int64_t peak_execution_time_ns{0};
    int64_t last_jitter_ns{0};
    float current_tps{0.0f};
};

// Fixed-step 128Hz authoritative loop with hybrid sleep-spin synchronizer
class TickLoop {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using TickFn = void (*)(uint64_t tick_index, double dt, void* user_data);

    TickLoop() noexcept;
    ~TickLoop();

    TickLoop(const TickLoop&) = delete;
    TickLoop& operator=(const TickLoop&) = delete;

    void Run(TickFn tick_fn, void* user_data) noexcept;
    void RequestStop() noexcept;

    [[nodiscard]] const TickTelemetry& GetTelemetry() const noexcept { return telemetry_; }
    [[nodiscard]] bool IsRunning() const noexcept { return is_running_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> is_running_{false};
    TickTelemetry telemetry_{};
    TimePoint last_stats_time_{};
    uint64_t stats_tick_anchor_{0};
};

} // namespace engine
