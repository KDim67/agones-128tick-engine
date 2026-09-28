#include "engine/Common.hpp"
#include "engine/Protocol.hpp"
#include "engine/TickLoop.hpp"
#include "engine/NetServer.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace {

struct ServerContext {
    engine::TickLoop tick_loop{};
    engine::NetServer net_server{};
    engine::ServerSnapshotPayload current_snapshot{};
};

// Global context pointer for signal handling
ServerContext* g_context = nullptr;

void SignalHandler(int signal) {
    if (g_context && (signal == SIGINT || signal == SIGTERM)) {
        g_context->tick_loop.RequestStop();
    }
}

// 128Hz authoritative tick callback
void AuthoritativeTick(uint64_t tick_index, [[maybe_unused]] double dt, void* user_data) {
    auto* ctx = static_cast<ServerContext*>(user_data);

    // Drain inbound UDP frames without allocation
    ctx->net_server.PollEvents();

    auto& sessions = ctx->net_server.GetSessions();
    uint8_t active_count = 0;

    // Process queued inputs for each connected player
    for (size_t i = 0; i < engine::kMaxPlayers; ++i) {
        auto& session = sessions[i];
        if (!session.is_active) {
            continue;
        }

        engine::ClientInputPayload input;
        while (session.input_queue.Pop(input)) {
            // Authoritative simulation step
        }

        auto& entity = ctx->current_snapshot.entities[active_count];
        entity.entity_id = session.slot_id;
        entity.flags = 1;
        ++active_count;
    }

    ctx->current_snapshot.server_tick = static_cast<uint32_t>(tick_index);
    ctx->current_snapshot.entity_count = active_count;

    // Broadcast authoritative world state to clients
    ctx->net_server.BroadcastSnapshot(ctx->current_snapshot);

    // Emit telemetry summary every 128 ticks (1 second)
    if (tick_index > 0 && (tick_index % engine::kTickRate) == 0) {
        const auto& telem = ctx->tick_loop.GetTelemetry();
        std::printf("[Tick %7llu] TPS: %5.1f | Exec: %5lld us | Peak: %5lld us | Jitter: %+5lld us | Overruns: %llu | Clients: %zu\n",
                    static_cast<unsigned long long>(tick_index),
                    static_cast<double>(telem.current_tps),
                    static_cast<long long>(telem.last_execution_time_ns / 1000),
                    static_cast<long long>(telem.peak_execution_time_ns / 1000),
                    static_cast<long long>(telem.last_jitter_ns / 1000),
                    static_cast<unsigned long long>(telem.overruns),
                    ctx->net_server.GetActivePlayerCount());
        std::fflush(stdout);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    uint16_t bind_port = 7777;
    if (argc > 1) {
        bind_port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    ServerContext context;
    g_context = &context;

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::printf("====================================================================\n");
    std::printf(" Agones-128: Authoritative 128Hz Dedicated Game Server Core\n");
    std::printf(" Target Tick Budget: %.4f ms (7,812.5 us) | Port: %u UDP\n",
                engine::kTickDurationNs.count() / 1'000'000.0,
                bind_port);
    std::printf("====================================================================\n");

    if (!context.net_server.Initialize(bind_port)) {
        std::fprintf(stderr, "Fatal: Failed to bind ENet UDP server on port %u\n", bind_port);
        return 1;
    }

    // Run authoritative loop
    context.tick_loop.Run(AuthoritativeTick, &context);

    std::printf("\nInitiating graceful server shutdown...\n");
    context.net_server.Shutdown();
    std::printf("Server terminated cleanly.\n");

    return 0;
}
