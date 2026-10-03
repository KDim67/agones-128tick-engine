#pragma once

#include "Common.hpp"
#include "GameServerLifecycle.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace engine {

struct AgonesConfig {
    std::chrono::milliseconds health_interval{2000};
    std::chrono::milliseconds empty_grace{60000};
};

// Agones sidecar control plane; all gRPC traffic runs on dedicated control and watch threads
class AgonesBridge {
public:
    AgonesBridge() noexcept;
    ~AgonesBridge();

    AgonesBridge(const AgonesBridge&) = delete;
    AgonesBridge& operator=(const AgonesBridge&) = delete;

    bool Start(const AgonesConfig& config) noexcept;

    // Sends Shutdown() if not already issued, cancels the watch stream and joins
    void Stop() noexcept;

    // Tick-thread liveness and occupancy; relaxed stores only
    void PublishTick(uint64_t completed_ticks, uint32_t active_players) noexcept {
        heartbeat_tick_.store(completed_ticks, std::memory_order_relaxed);
        active_players_.store(active_players, std::memory_order_relaxed);
    }

    [[nodiscard]] GameServerState GetState() const noexcept {
        return static_cast<GameServerState>(state_.load(std::memory_order_relaxed));
    }

    // Set once the sidecar has accepted Shutdown()
    [[nodiscard]] bool IsShutdownRequested() const noexcept {
        return shutdown_requested_.load(std::memory_order_acquire);
    }

private:
    struct Impl;

    std::atomic<uint64_t> heartbeat_tick_{0};
    std::atomic<uint32_t> active_players_{0};
    std::atomic<uint8_t> state_{0};
    std::atomic<bool> shutdown_requested_{false};
    std::unique_ptr<Impl> impl_;
};

} // namespace engine
