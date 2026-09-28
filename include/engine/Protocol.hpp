#pragma once

#include "Common.hpp"
#include <type_traits>

namespace engine {

#pragma pack(push, 1)

// Packet opcode identification
enum class PacketType : uint8_t {
    ConnectRequest = 1,
    ConnectAck = 2,
    ClientInput = 3,
    ServerSnapshot = 4,
    Disconnect = 5
};

// Wire header prepended to all network frames
struct PacketHeader {
    uint8_t type;
    uint8_t protocol_version;
    uint16_t payload_bytes;
};

// Client initial connection request payload
struct ConnectRequestPayload {
    uint32_t protocol_version;
    uint64_t client_auth_token;
};

// Server handshake acknowledgement payload
struct ConnectAckPayload {
    uint32_t assigned_slot;
    uint32_t server_tick;
    uint16_t tick_rate;
};

// Graceful session disconnect payload
struct DisconnectPayload {
    uint32_t slot_id;
    uint32_t reason_code;
};

// Input movement flags bitmask
enum InputFlags : uint16_t {
    InputFlag_None       = 0,
    InputFlag_Forward    = 1 << 0,
    InputFlag_Backward   = 1 << 1,
    InputFlag_MoveLeft   = 1 << 2,
    InputFlag_MoveRight  = 1 << 3,
    InputFlag_Jump       = 1 << 4,
    InputFlag_Crouch     = 1 << 5,
    InputFlag_Fire       = 1 << 6
};

// Client command frame
struct ClientInputPayload {
    uint32_t sequence;
    uint32_t ack_tick;
    float view_pitch;
    float view_yaw;
    uint16_t input_flags;
    uint16_t client_timestamp_ms;
};

// Entity spatial state in snapshot
struct EntitySnapshot {
    uint32_t entity_id;
    float pos_x;
    float pos_y;
    float pos_z;
    float vel_x;
    float vel_y;
    float vel_z;
    float yaw;
    float pitch;
    uint32_t flags;
};

// Authoritative world snapshot broadcast
struct ServerSnapshotPayload {
    uint32_t server_tick;
    uint32_t last_client_seq;
    uint8_t entity_count;
    std::array<EntitySnapshot, kMaxPlayers> entities;
};

#pragma pack(pop)

// Enforce standard layout and binary wire consistency
static_assert(std::is_standard_layout_v<PacketHeader>);
static_assert(sizeof(PacketHeader) == 4);

static_assert(std::is_standard_layout_v<ConnectRequestPayload>);
static_assert(sizeof(ConnectRequestPayload) == 12);

static_assert(std::is_standard_layout_v<ConnectAckPayload>);
static_assert(sizeof(ConnectAckPayload) == 10);

static_assert(std::is_standard_layout_v<DisconnectPayload>);
static_assert(sizeof(DisconnectPayload) == 8);

static_assert(std::is_standard_layout_v<ClientInputPayload>);
static_assert(sizeof(ClientInputPayload) == 20);

static_assert(std::is_standard_layout_v<EntitySnapshot>);
static_assert(sizeof(EntitySnapshot) == 40);

static_assert(std::is_standard_layout_v<ServerSnapshotPayload>);
static_assert(sizeof(ServerSnapshotPayload) == 9 + (kMaxPlayers * sizeof(EntitySnapshot)));

} // namespace engine
