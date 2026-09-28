#pragma once

#include <cstdint>
#include <chrono>
#include <array>
#include <string_view>

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

// Capacity limits for zero-allocation runtime
constexpr size_t kMaxPlayers = 10;
constexpr size_t kMaxInputQueuePerClient = 64;
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

} // namespace engine
