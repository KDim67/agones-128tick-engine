#include "engine/Common.hpp"
#include "engine/TickLoop.hpp"
#include "engine/AuthoritativeServer.hpp"
#include "engine/NetMemoryPool.hpp"
#include "engine/Metrics.hpp"
#include "engine/MetricsServer.hpp"
#include "engine/AgonesBridge.hpp"

#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct ServerContext {
    engine::TickLoop tick_loop{};
    engine::Metrics metrics{};
    engine::AuthoritativeServer server{metrics};
    engine::MetricsServer metrics_server{};
    engine::AgonesBridge agones{};
};

ServerContext* g_context = nullptr;

constexpr uint32_t kMaxEmptyGraceSec = 86400;

void SignalHandler(int signal) {
    if (g_context && (signal == SIGINT || signal == SIGTERM)) {
        g_context->tick_loop.RequestStop();
    }
}

// 128Hz authoritative tick callback
void AuthoritativeTick(uint64_t tick_index, double dt, void* user_data) {
    const auto tick_start = engine::TickLoop::Clock::now();
    auto* ctx = static_cast<ServerContext*>(user_data);
    const auto current_tick = static_cast<uint32_t>(tick_index);

    ctx->server.Tick(current_tick, dt);

    const auto tick_end = engine::TickLoop::Clock::now();
    const int64_t duration_us = std::chrono::duration_cast<std::chrono::microseconds>(tick_end - tick_start).count();
    const int64_t jitter_us = ctx->tick_loop.GetTelemetry().last_jitter_ns / 1000;
    constexpr int64_t kTickBudgetUs = engine::kTickDurationNs.count() / 1000;
    const bool overrun = (duration_us > kTickBudgetUs);

    const auto& net_server = ctx->server.Net();
    const auto active_players = static_cast<uint32_t>(net_server.GetActivePlayerCount());
    ctx->metrics.RecordTick(duration_us, jitter_us, overrun);
    ctx->metrics.SetActivePlayers(active_players);
    ctx->agones.PublishTick(tick_index + 1, active_players);

    // 1Hz Agones state sync and telemetry summary
    if (tick_index > 0 && (tick_index % engine::kTickRate) == 0) {
        ctx->metrics.SetGameServerState(ctx->agones.GetState());
        const auto& net = net_server.GetTelemetry();
        ctx->metrics.SetInputCounters(net.inputs_accepted, net.inputs_missed, net.inputs_redundant,
                                      net.inputs_overflow, net.packets_malformed);
        ctx->metrics.SetInputBacklogPeak(ctx->server.Net().TakeInputBacklogPeak());
        const engine::NetMemoryStats pool = engine::NetMemoryPool::Instance().Stats();
        ctx->metrics.SetTransportHealth(pool.peak_bytes, pool.failures, net.backlog_disconnects);
        if (ctx->agones.IsShutdownRequested()) {
            ctx->tick_loop.RequestStop();
        }

        const auto& telem = ctx->tick_loop.GetTelemetry();
        const auto& lag_telem = ctx->server.LagCompensation().GetTelemetry();

        std::printf("[Tick %7u] TPS: %5.1f | Exec: %4lld us (Avg: %4lld us, Peak: %4lld us) | Jitter: %+4lld us | Shots: %llu (Hits: %llu [H:%llu B:%llu L:%llu], Occl: %llu, Rej: %llu) | Clients: %zu\n",
                    current_tick,
                    static_cast<double>(telem.current_tps),
                    static_cast<long long>(telem.last_execution_time_ns / 1000),
                    static_cast<long long>(telem.avg_execution_time_ns / 1000),
                    static_cast<long long>(telem.peak_execution_time_ns / 1000),
                    static_cast<long long>(telem.last_jitter_ns / 1000),
                    static_cast<unsigned long long>(lag_telem.total_shots),
                    static_cast<unsigned long long>(lag_telem.registered_hits),
                    static_cast<unsigned long long>(lag_telem.headshots),
                    static_cast<unsigned long long>(lag_telem.bodyshots),
                    static_cast<unsigned long long>(lag_telem.legshots),
                    static_cast<unsigned long long>(lag_telem.occluded_shots),
                    static_cast<unsigned long long>(lag_telem.rejected_rewinds),
                    net_server.GetActivePlayerCount());
        std::fflush(stdout);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    uint16_t bind_port = 7777;
    uint16_t metrics_port = 9090;
    uint64_t benchmark_seconds = 0;
    bool agones_enabled = false;
    engine::AgonesConfig agones_config{};

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            bind_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--metrics-port") == 0 && i + 1 < argc) {
            metrics_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc) {
            benchmark_seconds = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--agones") == 0) {
            agones_enabled = true;
        } else if (std::strcmp(argv[i], "--empty-grace") == 0 && i + 1 < argc) {
            const char* arg = argv[++i];
            const char* arg_end = arg + std::strlen(arg);
            uint32_t seconds = 0;
            const auto [parsed_end, ec] = std::from_chars(arg, arg_end, seconds);
            if (ec != std::errc{} || parsed_end != arg_end || seconds < 1 || seconds > kMaxEmptyGraceSec) {
                std::fprintf(stderr, "Fatal: --empty-grace expects whole seconds in [1, %u], got '%s'\n", kMaxEmptyGraceSec, arg);
                return 1;
            }
            agones_config.empty_grace = std::chrono::seconds{seconds};
        }
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
    std::printf(" Prometheus Exporter: http://0.0.0.0:%u/metrics\n", metrics_port);
    std::printf(" Agones SDK: %s\n", agones_enabled ? "enabled" : "disabled");
    if (benchmark_seconds > 0) {
        std::printf(" Benchmark Mode: Running for %llu seconds (%llu ticks)...\n",
                    static_cast<unsigned long long>(benchmark_seconds),
                    static_cast<unsigned long long>(benchmark_seconds * engine::kTickRate));
    }
    std::printf("====================================================================\n");

    if (!context.server.Initialize(bind_port)) {
        std::fprintf(stderr, "Fatal: Failed to bind ENet UDP server on port %u\n", bind_port);
        return 1;
    }

    if (!context.metrics_server.Start(metrics_port, context.metrics)) {
        std::fprintf(stderr, "Warning: Failed to bind Prometheus metrics server on port %u\n", metrics_port);
    }

    if (agones_enabled && !context.agones.Start(agones_config)) {
        std::fprintf(stderr, "Fatal: Failed to start Agones SDK control plane\n");
        context.metrics_server.Stop();
        context.server.Shutdown();
        return 1;
    }

    // Host and peer tables exist; from here every ENet allocation is served by the preallocated pool
    engine::NetMemoryPool::Instance().Seal();

    const uint64_t max_ticks = benchmark_seconds * engine::kTickRate;
    context.tick_loop.Run(AuthoritativeTick, &context, max_ticks);

    // Agones Shutdown() before UDP teardown
    context.agones.Stop();

    const auto& telem = context.tick_loop.GetTelemetry();
    const auto& lag_telem = context.server.LagCompensation().GetTelemetry();
    const engine::NetMemoryStats pool = engine::NetMemoryPool::Instance().Stats();

    std::printf("\n====================================================================\n");
    std::printf(" Server Execution Report\n");
    std::printf(" Total Ticks Processed: %llu\n", static_cast<unsigned long long>(telem.total_ticks));
    std::printf(" Final TPS: %.2f\n", static_cast<double>(telem.current_tps));
    std::printf(" Peak Tick Exec Time: %lld us\n", static_cast<long long>(telem.peak_execution_time_ns / 1000));
    std::printf(" Peak Tick Jitter: %lld us\n", static_cast<long long>(telem.peak_jitter_ns / 1000));
    std::printf(" Total Tick Overruns: %llu\n", static_cast<unsigned long long>(telem.overruns));
    std::printf(" Total Ballistic Shots: %llu\n", static_cast<unsigned long long>(lag_telem.total_shots));
    std::printf(" Confirmed Hits: %llu (Head: %llu, Body: %llu, Leg: %llu)\n",
                static_cast<unsigned long long>(lag_telem.registered_hits),
                static_cast<unsigned long long>(lag_telem.headshots),
                static_cast<unsigned long long>(lag_telem.bodyshots),
                static_cast<unsigned long long>(lag_telem.legshots));
    std::printf(" Occluded Shots (Wall Hits): %llu\n", static_cast<unsigned long long>(lag_telem.occluded_shots));
    std::printf(" Rejected Rewinds: %llu\n", static_cast<unsigned long long>(lag_telem.rejected_rewinds));
    std::printf(" Net Pool Peak: %zu of %zu KiB (Failures: %llu, Setup Heap Blocks: %llu)\n",
                pool.peak_bytes / 1024, engine::kNetPoolArenaBytes / 1024,
                static_cast<unsigned long long>(pool.failures),
                static_cast<unsigned long long>(pool.heap_allocations));
    std::printf("====================================================================\n");

    context.metrics_server.Stop();
    context.server.Shutdown();
    std::printf("Server terminated cleanly.\n");

    return 0;
}
