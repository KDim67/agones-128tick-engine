#include "engine/AuthoritativeServer.hpp"
#include "engine/Metrics.hpp"
#include "engine/NetMemoryPool.hpp"
#include "engine/Protocol.hpp"
#include "Check.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> g_tracking{false};
std::atomic<uint64_t> g_tick_allocations{0};

void* CountedAllocate(std::size_t bytes) noexcept {
    if (g_tracking.load(std::memory_order_relaxed)) {
        g_tick_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(bytes == 0 ? 1 : bytes);
}

void* CountedAllocateAligned(std::size_t bytes, std::align_val_t alignment) noexcept {
    if (g_tracking.load(std::memory_order_relaxed)) {
        g_tick_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    const auto align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((bytes == 0 ? 1 : bytes) + align - 1) / align * align;
#if defined(_MSC_VER)
    return _aligned_malloc(rounded, align);
#else
    return std::aligned_alloc(align, rounded);
#endif
}

void FreeAligned(void* block) noexcept {
#if defined(_MSC_VER)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

} // namespace

// Every global allocation form is counted while a server tick runs
void* operator new(std::size_t bytes) {
    if (void* block = CountedAllocate(bytes)) {
        return block;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {
    if (void* block = CountedAllocate(bytes)) {
        return block;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if (void* block = CountedAllocateAligned(bytes, alignment)) {
        return block;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    if (void* block = CountedAllocateAligned(bytes, alignment)) {
        return block;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { return CountedAllocate(bytes); }
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { return CountedAllocate(bytes); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return CountedAllocateAligned(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return CountedAllocateAligned(bytes, alignment);
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }
void operator delete(void* block, const std::nothrow_t&) noexcept { std::free(block); }
void operator delete[](void* block, const std::nothrow_t&) noexcept { std::free(block); }
void operator delete(void* block, std::align_val_t) noexcept { FreeAligned(block); }
void operator delete[](void* block, std::align_val_t) noexcept { FreeAligned(block); }
void operator delete(void* block, std::size_t, std::align_val_t) noexcept { FreeAligned(block); }
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept { FreeAligned(block); }
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(block); }
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(block); }

namespace {

constexpr uint32_t kJoinTimeoutTicks = 256;
constexpr uint32_t kSoakTicks = 512;
constexpr uint32_t kLeaveTick = 96;
constexpr uint32_t kRejoinTick = 112;
constexpr uint32_t kFirePeriodTicks = 32;
constexpr size_t kLeavingClient = 3;
constexpr uint32_t kDeafTick = 200;
// Slot 0 fires every tick to exercise the cooldown; slot 9 stops acking to exercise the reliable backlog cap
constexpr uint32_t kSpammerSlot = 0;
constexpr uint32_t kDeafSlot = static_cast<uint32_t>(engine::kMaxPlayers) - 1;

// Minimal in-process player: stands at its slot spawn facing +x, so every shot meets the next slot's head
struct TestClient {
    ENetHost* host{nullptr};
    ENetPeer* peer{nullptr};
    uint32_t slot{static_cast<uint32_t>(engine::kMaxPlayers)};
    uint32_t sequence{0};
    uint32_t last_snapshot_tick{0};
    bool joined{false};
    bool deaf{false};
    uint64_t shot_results{0};
    uint64_t throttled_results{0};
    uint64_t hits_taken{0};
    std::array<engine::ClientInputPayload, engine::kInputRedundancy> recent{};

    bool CreateHost() {
        host = enet_host_create(nullptr, 1, engine::kNetChannelCount, 0, 0);
        return host != nullptr;
    }

    bool Connect(const ENetAddress& server) {
        peer = enet_host_connect(host, &server, engine::kNetChannelCount, 0);
        return peer != nullptr;
    }

    void Destroy() {
        if (host) {
            enet_host_destroy(host);
        }
        host = nullptr;
        peer = nullptr;
    }

    void Send(engine::PacketType type, const void* payload, size_t bytes, bool reliable) {
        std::array<uint8_t, sizeof(engine::PacketHeader) + engine::kMaxInputBatchBytes> wire{};
        const engine::PacketHeader header{
            .type = static_cast<uint8_t>(type),
            .protocol_version = engine::kProtocolVersion,
            .payload_bytes = static_cast<uint16_t>(bytes)
        };
        std::memcpy(wire.data(), &header, sizeof(header));
        std::memcpy(wire.data() + sizeof(header), payload, bytes);
        ENetPacket* packet = enet_packet_create(wire.data(), sizeof(header) + bytes,
                                                reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
        CHECK(packet != nullptr);
        if (enet_peer_send(peer, reliable ? 1 : 0, packet) != 0) {
            enet_packet_destroy(packet);
        }
    }

    void Receive(const uint8_t* data, size_t length) {
        engine::PacketHeader header;
        CHECK(length >= sizeof(header));
        std::memcpy(&header, data, sizeof(header));
        const uint8_t* payload = data + sizeof(header);

        switch (static_cast<engine::PacketType>(header.type)) {
            case engine::PacketType::ConnectAck: {
                engine::ConnectAckPayload ack;
                std::memcpy(&ack, payload, sizeof(ack));
                slot = ack.assigned_slot;
                joined = true;
                break;
            }
            case engine::PacketType::ServerSnapshot: {
                uint32_t tick = 0;
                std::memcpy(&tick, payload, sizeof(tick));
                last_snapshot_tick = std::max(last_snapshot_tick, tick);
                break;
            }
            case engine::PacketType::ShotResult: {
                engine::ShotResultPayload result;
                std::memcpy(&result, payload, sizeof(result));
                ++shot_results;
                throttled_results += result.status == static_cast<uint8_t>(engine::ShotStatus::Throttled) ? 1u : 0u;
                break;
            }
            case engine::PacketType::HitNotification:
                ++hits_taken;
                break;
            default:
                break;
        }
    }

    // A deaf client still sends but never reads, so it never acks
    void Service() {
        ENetEvent event;
        while (host && !deaf && enet_host_service(host, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                const engine::ConnectRequestPayload request{.protocol_version = engine::kProtocolVersion, .client_auth_token = 0};
                Send(engine::PacketType::ConnectRequest, &request, sizeof(request), true);
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                Receive(event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                joined = false;
                peer = nullptr;
            }
        }
    }

    void SendInput(uint32_t tick) {
        if (!joined) {
            return;
        }
        engine::ClientInputPayload input{};
        input.sequence = ++sequence;
        input.ack_tick = last_snapshot_tick;
        input.interp_span_ticks = 1;
        input.view_yaw = 90.0f;
        const bool spamming = deaf || slot == kSpammerSlot;
        input.input_flags = (spamming || (tick + slot * 3) % kFirePeriodTicks == 0) ? engine::InputFlag_Fire
                                                                                     : engine::InputFlag_None;

        std::rotate(recent.begin(), recent.begin() + 1, recent.end());
        recent.back() = input;
        const size_t count = std::min<size_t>(sequence, engine::kInputRedundancy);

        std::array<uint8_t, engine::kMaxInputBatchBytes> batch{};
        const size_t bytes = engine::EncodeInputBatch(recent.data() + (recent.size() - count), count, batch.data(), batch.size());
        CHECK(bytes > 0);
        Send(engine::PacketType::ClientInput, batch.data(), bytes, false);
        enet_host_flush(host);
    }

    void Leave() {
        const engine::DisconnectPayload goodbye{.slot_id = slot, .reason_code = 0};
        Send(engine::PacketType::Disconnect, &goodbye, sizeof(goodbye), true);
        enet_peer_disconnect_later(peer, 0);
        enet_host_flush(host);
        joined = false;
    }
};

void TestSteadyStateTickIsAllocationFree() {
    std::printf("[Test] Running Live Tick Allocation Audit (10 ENet clients, kills, respawns, rejoin, fire spam, deaf client)...\n");
    engine::Metrics metrics;
    engine::AuthoritativeServer server{metrics};
    CHECK(server.Initialize(0, "127.0.0.1"));

    ENetAddress address{};
    CHECK(enet_address_set_host_ip(&address, "127.0.0.1") == 0);
    address.port = server.Net().BoundPort();
    CHECK(address.port != 0);

    // Host tables are setup-time allocations; the spare host rejoins into the slot the leaver frees
    std::array<TestClient, engine::kMaxPlayers + 1> clients{};
    for (auto& client : clients) {
        CHECK(client.CreateHost());
    }
    for (size_t i = 0; i < engine::kMaxPlayers; ++i) {
        CHECK(clients[i].Connect(address));
    }

    auto& pool = engine::NetMemoryPool::Instance();
    pool.Seal();
    const engine::NetMemoryStats sealed = pool.Stats();

    uint32_t tick = 1;
    auto next_tick = std::chrono::steady_clock::now();
    auto run_tick = [&] {
        for (auto& client : clients) {
            client.Service();
            client.SendInput(tick);
        }
        g_tracking.store(true, std::memory_order_relaxed);
        server.Tick(tick, engine::kFixedDeltaTimeSec);
        g_tracking.store(false, std::memory_order_relaxed);
        ++tick;
        next_tick += engine::kTickDurationNs;
        std::this_thread::sleep_until(next_tick);
    };

    while (tick < kJoinTimeoutTicks &&
           !std::all_of(clients.begin(), clients.begin() + engine::kMaxPlayers, [](const TestClient& c) { return c.joined; })) {
        run_tick();
    }
    CHECK(server.Net().GetActivePlayerCount() == engine::kMaxPlayers);

    const uint32_t soak_start = tick;
    while (tick - soak_start < kSoakTicks) {
        if (tick - soak_start == kLeaveTick) {
            clients[kLeavingClient].Leave();
        }
        if (tick - soak_start == kDeafTick) {
            for (auto& client : clients) {
                client.deaf = client.joined && client.slot == kDeafSlot;
            }
        }
        if (tick - soak_start == kRejoinTick) {
            CHECK(clients.back().Connect(address));
        }
        run_tick();
    }

    const engine::NetMemoryStats soaked = pool.Stats();
    const auto& combat = server.LagCompensation().GetTelemetry();
    const auto& players = server.World().GetPlayers();
    CHECK(g_tick_allocations.load() == 0);
    // ENet traffic must have flowed through the pool, or the heap audit above proves nothing
    CHECK(soaked.pool_allocations > sealed.pool_allocations);
    CHECK(soaked.failures == 0 && soaked.heap_allocations == sealed.heap_allocations);
    CHECK(combat.total_shots > 0 && combat.registered_hits > 0);
    CHECK(clients.back().joined && clients.back().slot == kLeavingClient);
    CHECK(players[1].spawn_id >= 2);
    CHECK(server.Net().GetTelemetry().packets_malformed == 0);
    for (const auto& client : clients) {
        CHECK(client.shot_results > 0);
    }

    // Spammer fires every tick but lands at most one round per cooldown; the deaf client is cut off at the backlog cap
    const auto spammer = std::find_if(clients.begin(), clients.end(), [](const TestClient& c) { return c.slot == kSpammerSlot; });
    CHECK(spammer != clients.end() && spammer->throttled_results > 0);
    CHECK(spammer->shot_results - spammer->throttled_results <= (tick - 1) / engine::kFireIntervalTicks + 1);
    CHECK(server.Net().GetTelemetry().backlog_disconnects == 1);
    CHECK(server.Net().GetActivePlayerCount() == engine::kMaxPlayers - 1);
    CHECK(server.Net().GetSessions()[kDeafSlot].state == engine::SessionState::Disconnected);

    for (auto& client : clients) {
        client.Destroy();
    }
    server.Shutdown();
    CHECK(pool.Stats().in_use_bytes == 0);

    std::printf("  -> %u ticks, %llu shots, %llu hits, %llu spam shots throttled, 1 deaf client cut off: 0 C++ heap allocations "
                "in the tick, pool peak %zu of %zu KiB, %llu setup heap blocks, all blocks returned.\n",
                tick - 1, static_cast<unsigned long long>(combat.total_shots),
                static_cast<unsigned long long>(combat.registered_hits),
                static_cast<unsigned long long>(spammer->throttled_results), soaked.peak_bytes / 1024,
                engine::kNetPoolArenaBytes / 1024, static_cast<unsigned long long>(soaked.heap_allocations));
}

void TestPoolClassesSpillAndSeal() {
    std::printf("[Test] Running Pool Size Classes, Spill and Sealed Oversize...\n");
    auto& pool = engine::NetMemoryPool::Instance();
    const engine::NetMemoryStats before = pool.Stats();
    CHECK(before.in_use_bytes == 0);

    // Smallest class drained completely; the next request spills into the 128-byte class
    constexpr auto kSmall = engine::kNetPoolClasses[0];
    std::array<void*, kSmall.blocks> small{};
    for (auto& block : small) {
        block = pool.Allocate(1);
        CHECK(block != nullptr);
    }
    CHECK(pool.Stats().in_use_bytes == kSmall.blocks * kSmall.block_bytes);
    void* spilled = pool.Allocate(kSmall.block_bytes);
    CHECK(spilled != nullptr);
    CHECK(pool.Stats().in_use_bytes == kSmall.blocks * kSmall.block_bytes + engine::kNetPoolClasses[1].block_bytes);

    // Sealed: oversize never reaches the heap
    CHECK(pool.Allocate(engine::kNetPoolClasses.back().block_bytes + 1) == nullptr);
    CHECK(pool.Stats().failures == before.failures + 1);
    CHECK(pool.Stats().heap_allocations == before.heap_allocations);

    pool.Free(spilled);
    for (void* block : small) {
        pool.Free(block);
    }
    CHECK(pool.Stats().in_use_bytes == 0);

    std::printf("  -> %zu-byte class exhausted and spilled, sealed oversize refused, every block returned.\n",
                kSmall.block_bytes);
}

} // namespace

int main() {
    std::printf("====================================================\n");
    std::printf(" Agones-128 Engine: Allocation Verification\n");
    std::printf("====================================================\n");

    TestSteadyStateTickIsAllocationFree();
    TestPoolClassesSpillAndSeal();

    std::printf("\nAll Allocation Verification Checks PASSED (100%%).\n");
    return 0;
}
