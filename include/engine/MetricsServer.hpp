#pragma once

#include "Common.hpp"
#include "Metrics.hpp"
#include <atomic>
#include <thread>
#include <cstdint>

namespace engine {

// Non-blocking HTTP metrics endpoint serving Prometheus scrapes on an isolated thread
class MetricsServer {
public:
    MetricsServer() noexcept;
    ~MetricsServer();

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    bool Start(uint16_t port, const Metrics& metrics) noexcept;
    void Stop() noexcept;

    [[nodiscard]] bool IsRunning() const noexcept { return is_running_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }

private:
    void RunServer() noexcept;

    const Metrics* metrics_{nullptr};
    std::atomic<bool> is_running_{false};
    uint16_t port_{9090};
    std::thread worker_thread_{};

#if defined(_WIN32)
    uintptr_t listen_socket_{~static_cast<uintptr_t>(0)};
#else
    int listen_socket_{-1};
#endif
};

} // namespace engine
