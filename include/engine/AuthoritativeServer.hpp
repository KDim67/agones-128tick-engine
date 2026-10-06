#pragma once

#include "Common.hpp"
#include "Protocol.hpp"
#include "NetServer.hpp"
#include "WorldState.hpp"
#include "HistoryBuffer.hpp"
#include "LagCompensation.hpp"
#include "Metrics.hpp"
#include <cstdint>

namespace engine {

// One authoritative step: ingest, simulate, rewind, record, replicate
class AuthoritativeServer {
public:
    explicit AuthoritativeServer(Metrics& metrics) noexcept : metrics_(metrics) {}

    AuthoritativeServer(const AuthoritativeServer&) = delete;
    AuthoritativeServer& operator=(const AuthoritativeServer&) = delete;

    bool Initialize(uint16_t bind_port) noexcept { return net_server_.Initialize(bind_port); }
    void Shutdown() noexcept { net_server_.Shutdown(); }

    void Tick(uint32_t current_tick, double dt) noexcept;

    [[nodiscard]] NetServer& Net() noexcept { return net_server_; }
    [[nodiscard]] const NetServer& Net() const noexcept { return net_server_; }
    [[nodiscard]] const WorldState& World() const noexcept { return world_state_; }
    [[nodiscard]] const LagCompensationManager& LagCompensation() const noexcept { return lag_comp_; }

private:
    void ProcessSession(ClientSession& session, uint32_t current_tick, double dt) noexcept;
    void ResolveShot(uint32_t shooter_slot, const ClientInputPayload& input, uint32_t current_tick,
                     uint32_t oldest_tick) noexcept;
    void BroadcastSnapshot(uint32_t current_tick) noexcept;

    Metrics& metrics_;
    NetServer net_server_{};
    WorldState world_state_{};
    HistoryBuffer history_buffer_{};
    LagCompensationManager lag_comp_{};
    ServerSnapshotPayload snapshot_{};
};

} // namespace engine
