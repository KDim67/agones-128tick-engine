#include "engine/NetMemoryPool.hpp"
#include "engine/Common.hpp"
#include "engine/Protocol.hpp"
#include "engine/SnapshotCodec.hpp"
#include <enet/enet.h>
#include <cstdlib>
#include <new>

namespace engine {

// Every steady-state ENet allocation fits a class; only host setup is oversize
static_assert(sizeof(ENetPacket) <= kNetPoolClasses[0].block_bytes);
static_assert(sizeof(ENetAcknowledgement) <= kNetPoolClasses[1].block_bytes);
static_assert(sizeof(ENetOutgoingCommand) <= kNetPoolClasses[1].block_bytes);
static_assert(sizeof(ENetIncomingCommand) <= kNetPoolClasses[1].block_bytes);
static_assert(kNetChannelCount * sizeof(ENetChannel) <= kNetPoolClasses[2].block_bytes);
static_assert(sizeof(PacketHeader) + kMaxSnapshotBytes <= kNetPoolClasses[2].block_bytes);
static_assert(kMaxMtuPayload <= kNetPoolClasses.back().block_bytes);
static_assert(alignof(std::max_align_t) <= kNetPoolClasses[0].block_bytes);

namespace {

void* ENET_CALLBACK PoolAllocate(size_t bytes) {
    return NetMemoryPool::Instance().Allocate(bytes);
}

void ENET_CALLBACK PoolFree(void* block) {
    NetMemoryPool::Instance().Free(block);
}

// Exhaustion drops the packet; ENet's default handler aborts the process
void ENET_CALLBACK PoolExhausted() {
}

} // namespace

NetMemoryPool& NetMemoryPool::Instance() noexcept {
    static NetMemoryPool pool;
    return pool;
}

NetMemoryPool::NetMemoryPool() noexcept {
    size_t offset = 0;
    for (size_t c = 0; c < kNetPoolClasses.size(); ++c) {
        const auto& size_class = kNetPoolClasses[c];
        class_offsets_[c] = offset;

        // Threaded back to front so blocks are handed out in address order
        FreeBlock* head = nullptr;
        for (size_t i = size_class.blocks; i-- > 0;) {
            head = new (arena_.data() + offset + i * size_class.block_bytes) FreeBlock{head};
        }
        free_lists_[c] = head;
        offset += size_class.block_bytes * size_class.blocks;
    }
}

bool NetMemoryPool::InstallEnetCallbacks() noexcept {
    ENetCallbacks callbacks{};
    callbacks.malloc = &PoolAllocate;
    callbacks.free = &PoolFree;
    callbacks.no_memory = &PoolExhausted;
    return enet_initialize_with_callbacks(ENET_VERSION, &callbacks) == 0;
}

void NetMemoryPool::Seal() noexcept {
    Lock();
    sealed_ = true;
    Unlock();
}

void* NetMemoryPool::Allocate(size_t bytes) noexcept {
    Lock();

    // Smallest fitting class first; an exhausted class spills into the next larger one
    for (size_t c = 0; c < kNetPoolClasses.size(); ++c) {
        FreeBlock* block = free_lists_[c];
        if (bytes > kNetPoolClasses[c].block_bytes || !block) {
            continue;
        }
        free_lists_[c] = block->next;
        ++stats_.pool_allocations;
        stats_.in_use_bytes += kNetPoolClasses[c].block_bytes;
        stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.in_use_bytes);
        Unlock();
        return block;
    }

    // Host and peer tables are allocated once during setup; after Seal() the heap is off limits
    void* memory = sealed_ ? nullptr : std::malloc(bytes);
    if (memory) {
        ++stats_.heap_allocations;
    } else {
        ++stats_.failures;
    }
    Unlock();
    return memory;
}

void NetMemoryPool::Free(void* block) noexcept {
    if (!block) {
        return;
    }

    const auto address = reinterpret_cast<uintptr_t>(block);
    const auto base = reinterpret_cast<uintptr_t>(arena_.data());
    if (address < base || address >= base + arena_.size()) {
        std::free(block);
        return;
    }

    const auto offset = static_cast<size_t>(address - base);
    size_t c = kNetPoolClasses.size() - 1;
    while (offset < class_offsets_[c]) {
        --c;
    }

    Lock();
    free_lists_[c] = new (block) FreeBlock{free_lists_[c]};
    stats_.in_use_bytes -= kNetPoolClasses[c].block_bytes;
    Unlock();
}

NetMemoryStats NetMemoryPool::Stats() const noexcept {
    Lock();
    const NetMemoryStats snapshot = stats_;
    Unlock();
    return snapshot;
}

void NetMemoryPool::Lock() const noexcept {
    while (lock_.test_and_set(std::memory_order_acquire)) {
        CpuPause();
    }
}

void NetMemoryPool::Unlock() const noexcept {
    lock_.clear(std::memory_order_release);
}

} // namespace engine
