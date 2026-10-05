#pragma once

#include <cstdint>
#include <chrono>
#include <array>
#include <cmath>
#include <algorithm>
#include <string_view>

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace engine {

// 128Hz tick interval: 7.8125ms budget
constexpr uint32_t kTickRate = 128;
constexpr std::chrono::nanoseconds kTickDurationNs{1'000'000'000 / kTickRate};
constexpr double kFixedDeltaTimeSec = 1.0 / static_cast<double>(kTickRate);

// Hybrid sleep-spin threshold
constexpr std::chrono::nanoseconds kSleepThresholdNs{1'500'000};
constexpr uint32_t kMaxCatchupTicks = 4;

constexpr uint8_t kProtocolVersion = 2;
constexpr size_t kMaxPlayers = 10;
// Channel 0 unsequenced state, channel 1 reliable control
constexpr size_t kNetChannelCount = 2;
constexpr size_t kMaxInputQueuePerClient = 64;
// Bucket depth spans the input window so a held uplink drains; the per-tick cap bounds the catch-up burst
constexpr uint32_t kInputBurstTokens = static_cast<uint32_t>(kMaxInputQueuePerClient);
constexpr uint32_t kMaxInputsPerTick = 4;
constexpr uint32_t kInputHoleWaitTicks = 8;
constexpr size_t kInputRedundancy = 4;
constexpr uint32_t kMaxInterpSpanTicks = 8;
constexpr uint32_t kMaxInterpDelayTicks = 24;
// Rewind reach beyond RTT plus interpolation: hole wait and queueing slack, not backtracking
constexpr uint32_t kRewindToleranceTicks = 16;
constexpr uint32_t kRespawnDelayTicks = 2 * kTickRate;
constexpr uint32_t kClientTimeoutMs = 5000;
constexpr size_t kMaxMtuPayload = 1200;

// Low-overhead CPU pause intrinsic
inline void CpuPause() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Elevate thread scheduling priority
inline bool SetThreadRealtimePriority() noexcept {
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#elif defined(__linux__)
    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
    return false;
#endif
}

// Clamp view angles to prevent NaNs and camera inversion
inline bool ValidateAndClampViewAngles(float& pitch, float& yaw) noexcept {
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }
    pitch = std::clamp(pitch, -89.0f, 89.0f);
    yaw = std::fmod(yaw, 360.0f);
    if (yaw < 0.0f) {
        yaw += 360.0f;
    }
    // Tiny negative yaw rounds up to 360; fold it so a second pass is a no-op
    if (yaw >= 360.0f) {
        yaw = 0.0f;
    }
    return true;
}

} // namespace engine
