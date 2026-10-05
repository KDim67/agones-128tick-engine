#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace engine {

struct NetPoolClass {
    size_t block_bytes;
    size_t blocks;
};

// Packet headers, protocol commands, peer channels, snapshots, MTU-sized inbound payloads
constexpr std::array<NetPoolClass, 5> kNetPoolClasses = {{
    {64, 2048}, {128, 4096}, {256, 256}, {512, 256}, {2048, 128}
}};

constexpr size_t kNetPoolArenaBytes = [] {
    size_t total = 0;
    for (const auto& size_class : kNetPoolClasses) {
        total += size_class.block_bytes * size_class.blocks;
    }
    return total;
}();

struct NetMemoryStats {
    uint64_t pool_allocations{0};
    uint64_t heap_allocations{0};
    uint64_t failures{0};
    size_t in_use_bytes{0};
    size_t peak_bytes{0};
};

// Fixed-block arena behind ENet's allocator hooks; once sealed nothing reaches the system heap
class NetMemoryPool {
public:
    [[nodiscard]] static NetMemoryPool& Instance() noexcept;

    NetMemoryPool(const NetMemoryPool&) = delete;
    NetMemoryPool& operator=(const NetMemoryPool&) = delete;

    // Routes every ENet allocation through this pool, then initializes ENet
    [[nodiscard]] bool InstallEnetCallbacks() noexcept;

    // Setup over: requests no class can serve now fail instead of reaching the heap
    void Seal() noexcept;

    // nullptr on exhaustion; ENet treats it as a dropped packet
    [[nodiscard]] void* Allocate(size_t bytes) noexcept;
    void Free(void* block) noexcept;

    [[nodiscard]] NetMemoryStats Stats() const noexcept;

private:
    struct FreeBlock {
        FreeBlock* next;
    };

    NetMemoryPool() noexcept;

    void Lock() const noexcept;
    void Unlock() const noexcept;

    alignas(64) std::array<std::byte, kNetPoolArenaBytes> arena_;
    std::array<FreeBlock*, kNetPoolClasses.size()> free_lists_{};
    std::array<size_t, kNetPoolClasses.size()> class_offsets_{};
    NetMemoryStats stats_{};
    bool sealed_{false};
    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

} // namespace engine
