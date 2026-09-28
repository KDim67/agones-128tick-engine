#pragma once

#include <cstdint>
#include <chrono>
#include <array>
#include <cmath>
#include <algorithm>
#include <string_view>

#if defined(_WIN32)
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

// Tick rate configuration
constexpr uint32_t kTickRate = 128;
constexpr std::chrono::nanoseconds kTickDurationNs{1'000'000'000 / kTickRate}; // 7,812,500 ns
constexpr double kFixedDeltaTimeSec = 1.0 / static_cast<double>(kTickRate);

// Budget and jitter thresholds
constexpr std::chrono::nanoseconds kSleepThresholdNs{1'500'000}; // 1.5ms threshold for spinlock transition
constexpr uint32_t kMaxCatchupTicks = 4;

// Capacity and network protocol limits
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kMaxPlayers = 10;
constexpr size_t kMaxInputQueuePerClient = 64;
constexpr size_t kMaxInputsPerTick = 4;
constexpr uint32_t kClientTimeoutMs = 5000;
constexpr size_t kMaxMtuPayload = 1200;

// Low-overhead CPU yield intrinsic
inline void CpuPause() noexcept {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    // Fallback yield
#endif
}

// Elevate calling thread scheduling priority to prevent OS preemption
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

// Sanitize and clamp view angle inputs against exploits and invalid floats
inline bool ValidateAndClampViewAngles(float& pitch, float& yaw) noexcept {
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }
    pitch = std::clamp(pitch, -89.0f, 89.0f);
    yaw = std::fmod(yaw, 360.0f);
    if (yaw < 0.0f) {
        yaw += 360.0f;
    }
    return true;
}

} // namespace engine
