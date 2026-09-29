#include "engine/Common.hpp"
#include "engine/Protocol.hpp"
#include "engine/TickLoop.hpp"
#include "engine/NetServer.hpp"
#include "engine/WorldState.hpp"
#include "engine/HistoryBuffer.hpp"
#include "engine/LagCompensation.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct ServerContext {
    engine::TickLoop tick_loop{};
    engine::NetServer net_server{};
    engine::WorldState world_state{};
    engine::HistoryBuffer history_buffer{};
    engine::LagCompensationManager lag_comp{};
    engine::ServerSnapshotPayload current_snapshot{};
};

ServerContext* g_context = nullptr;

void SignalHandler(int signal) {
    if (g_context && (signal == SIGINT || signal == SIGTERM)) {
        g_context->tick_loop.RequestStop();
    }
}

// 128Hz authoritative tick callback
void AuthoritativeTick(uint64_t tick_index, double dt, void* user_data) {
    auto* ctx = static_cast<ServerContext*>(user_data);
    const auto current_tick = static_cast<uint32_t>(tick_index);

    ctx->net_server.PollEvents(current_tick);

    if (tick_index > 0 && (tick_index % engine::kTickRate) == 0) {
        ctx->net_server.CheckTimeouts();
    }

    auto& sessions = ctx->net_server.GetSessions();

    for (size_t i = 0; i < engine::kMaxPlayers; ++i) {
        auto& session = sessions[i];

        if (session.state == engine::SessionState::Connected) {
            if (!ctx->world_state.GetPlayer(session.slot_id)) {
                const float spawn_x = static_cast<float>(session.slot_id * 4) - 16.0f;
                ctx->world_state.SpawnPlayer(session.slot_id, engine::Vec3{spawn_x, 0.0f, 0.0f});
            }

            engine::ClientInputPayload input;
            while (session.input_queue.Pop(input)) {
                ctx->world_state.IntegratePlayerInput(session.slot_id, input, dt);

                if (input.input_flags & engine::InputFlag_Fire) {
                    const auto hit = ctx->lag_comp.ProcessShot(
                        ctx->world_state, ctx->history_buffer,
                        session.slot_id, input.ack_tick, input.sub_tick_fraction);

                    if (hit.hit) {
                        const auto* victim = ctx->world_state.GetPlayer(hit.victim_slot);
                        const uint8_t victim_hp = victim ? static_cast<uint8_t>(victim->health) : 0;

                        engine::HitNotificationPayload notif{
                            .shooter_slot = session.slot_id,
                            .victim_slot = hit.victim_slot,
                            .server_tick = current_tick,
                            .hit_x = hit.hit_position.x,
                            .hit_y = hit.hit_position.y,
                            .hit_z = hit.hit_position.z,
                            .distance = hit.hit_distance,
                            .damage = hit.damage,
                            .hit_zone = static_cast<uint8_t>(hit.hit_zone),
                            .victim_health = victim_hp
                        };
                        ctx->net_server.SendPacket(session.slot_id, engine::PacketType::HitNotification, &notif, sizeof(notif), true);
                        ctx->net_server.SendPacket(hit.victim_slot, engine::PacketType::HitNotification, &notif, sizeof(notif), true);
                    }
                }
            }
        } else if (session.state == engine::SessionState::Disconnected) {
            ctx->world_state.DespawnPlayer(session.slot_id);
        }
    }

    const uint64_t now_ns = static_cast<uint64_t>(tick_index * engine::kTickDurationNs.count());
    ctx->history_buffer.Record(current_tick, now_ns, ctx->world_state.GetPlayers());

    ctx->current_snapshot.server_tick = current_tick;
    uint8_t entity_count = 0;

    for (const auto& p : ctx->world_state.GetPlayers()) {
        if (p.is_active) {
            auto& ent = ctx->current_snapshot.entities[entity_count++];
            ent.entity_id = p.slot_id;
            ent.pos_x = p.position.x;
            ent.pos_y = p.position.y;
            ent.pos_z = p.position.z;
            ent.vel_x = p.velocity.x;
            ent.vel_y = p.velocity.y;
            ent.vel_z = p.velocity.z;
            ent.pitch = p.view_pitch;
            ent.yaw = p.view_yaw;
            ent.flags = p.flags;
            ent.health = static_cast<uint8_t>(p.health);
            ent.reserved[0] = 0;
            ent.reserved[1] = 0;
            ent.reserved[2] = 0;
        }
    }

    ctx->current_snapshot.entity_count = entity_count;
    ctx->net_server.BroadcastSnapshot(ctx->current_snapshot);

    // Periodic 1Hz telemetry summary
    if (tick_index > 0 && (tick_index % engine::kTickRate) == 0) {
        const auto& telem = ctx->tick_loop.GetTelemetry();
        const auto& lag_telem = ctx->lag_comp.GetTelemetry();

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
                    ctx->net_server.GetActivePlayerCount());
        std::fflush(stdout);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    uint16_t bind_port = 7777;
    uint64_t benchmark_seconds = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            bind_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc) {
            benchmark_seconds = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10));
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
    if (benchmark_seconds > 0) {
        std::printf(" Benchmark Mode: Running for %llu seconds (%llu ticks)...\n",
                    static_cast<unsigned long long>(benchmark_seconds),
                    static_cast<unsigned long long>(benchmark_seconds * engine::kTickRate));
    }
    std::printf("====================================================================\n");

    if (!context.net_server.Initialize(bind_port)) {
        std::fprintf(stderr, "Fatal: Failed to bind ENet UDP server on port %u\n", bind_port);
        return 1;
    }

    const uint64_t max_ticks = benchmark_seconds * engine::kTickRate;
    context.tick_loop.Run(AuthoritativeTick, &context, max_ticks);

    const auto& telem = context.tick_loop.GetTelemetry();
    const auto& lag_telem = context.lag_comp.GetTelemetry();

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
    std::printf("====================================================================\n");

    context.net_server.Shutdown();
    std::printf("Server terminated cleanly.\n");

    return 0;
}
