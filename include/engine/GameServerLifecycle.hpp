#pragma once

#include "Common.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace engine {

// Agones GameServer status.state as reported by the sidecar watch stream
enum class GameServerState : uint8_t {
    Unknown = 0,
    Scheduled,
    RequestReady,
    Ready,
    Reserved,
    Allocated,
    Shutdown,
    Unhealthy,
    Error,
    Count
};

constexpr size_t kGameServerStateCount = static_cast<size_t>(GameServerState::Count);

constexpr std::array<std::string_view, kGameServerStateCount> kGameServerStateNames = {
    "Unknown", "Scheduled", "RequestReady", "Ready", "Reserved",
    "Allocated", "Shutdown", "Unhealthy", "Error"
};

[[nodiscard]] constexpr std::string_view GameServerStateName(GameServerState state) noexcept {
    const auto index = static_cast<size_t>(state);
    return index < kGameServerStateCount ? kGameServerStateNames[index] : kGameServerStateNames[0];
}

// Unmapped transitional states (PortAllocation, Creating, Starting) collapse to Unknown
[[nodiscard]] constexpr GameServerState ParseGameServerState(std::string_view name) noexcept {
    for (size_t i = 0; i < kGameServerStateCount; ++i) {
        if (kGameServerStateNames[i] == name) {
            return static_cast<GameServerState>(i);
        }
    }
    return GameServerState::Unknown;
}

enum class LifecycleAction : uint8_t {
    None = 0,
    Allocate,
    Shutdown
};

// Self-allocates occupied Ready servers, recycles Allocated servers left empty past the grace window
class SessionLifecyclePolicy {
public:
    using Clock = std::chrono::steady_clock;

    SessionLifecyclePolicy(std::chrono::milliseconds empty_grace, std::chrono::milliseconds retry_interval) noexcept
        : empty_grace_(empty_grace), retry_interval_(retry_interval) {}

    [[nodiscard]] LifecycleAction Evaluate(GameServerState state, uint32_t active_players, Clock::time_point now) noexcept {
        if (state == GameServerState::Shutdown) {
            return LifecycleAction::None;
        }

        // Shutdown is terminal; reissue until the sidecar confirms the transition
        if (shutdown_pending_) {
            return IssueIfDue(now) ? LifecycleAction::Shutdown : LifecycleAction::None;
        }

        // Direct-connect players on a Ready server would be lost to a fleet scale-down
        if (state == GameServerState::Ready && active_players > 0) {
            empty_tracking_ = false;
            return IssueIfDue(now) ? LifecycleAction::Allocate : LifecycleAction::None;
        }

        if (state != GameServerState::Allocated || active_players > 0) {
            empty_tracking_ = false;
            return LifecycleAction::None;
        }

        if (!empty_tracking_) {
            empty_tracking_ = true;
            empty_since_ = now;
            return LifecycleAction::None;
        }

        if (now - empty_since_ < empty_grace_) {
            return LifecycleAction::None;
        }

        shutdown_pending_ = true;
        issued_ = true;
        last_issue_ = now;
        return LifecycleAction::Shutdown;
    }

    [[nodiscard]] bool IsShutdownPending() const noexcept { return shutdown_pending_; }

private:
    [[nodiscard]] bool IssueIfDue(Clock::time_point now) noexcept {
        if (issued_ && (now - last_issue_) < retry_interval_) {
            return false;
        }
        issued_ = true;
        last_issue_ = now;
        return true;
    }

    std::chrono::milliseconds empty_grace_;
    std::chrono::milliseconds retry_interval_;
    Clock::time_point empty_since_{};
    Clock::time_point last_issue_{};
    bool empty_tracking_{false};
    bool shutdown_pending_{false};
    bool issued_{false};
};

} // namespace engine
