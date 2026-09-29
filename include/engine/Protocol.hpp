#pragma once

#include "Common.hpp"
#include <type_traits>

namespace engine {

#pragma pack(push, 1)

enum class PacketType : uint8_t {
    ConnectRequest = 1,
    ConnectAck = 2,
    ClientInput = 3,
    ServerSnapshot = 4,
    Disconnect = 5,
    HitNotification = 6
};

struct PacketHeader {
    uint8_t type;
    uint8_t protocol_version;
    uint16_t payload_bytes;
};

struct ConnectRequestPayload {
    uint32_t protocol_version;
    uint64_t client_auth_token;
};

struct ConnectAckPayload {
    uint32_t assigned_slot;
    uint32_t server_tick;
    uint16_t tick_rate;
};

struct DisconnectPayload {
    uint32_t slot_id;
    uint32_t reason_code;
};

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

enum class HitZone : uint8_t {
    None = 0,
    Head = 1,
    Torso = 2,
    Legs = 3
};

struct ClientInputPayload {
    uint32_t sequence;
    uint32_t ack_tick;
    float sub_tick_fraction;
    float view_pitch;
    float view_yaw;
    uint16_t input_flags;
    uint16_t client_timestamp_ms;
};

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
    uint8_t health;
    uint8_t reserved[3];
};

struct ServerSnapshotPayload {
    uint32_t server_tick;
    uint32_t last_client_seq;
    uint8_t entity_count;
    std::array<EntitySnapshot, kMaxPlayers> entities;
};

struct HitNotificationPayload {
    uint32_t shooter_slot;
    uint32_t victim_slot;
    uint32_t server_tick;
    float hit_x;
    float hit_y;
    float hit_z;
    float distance;
    uint16_t damage;
    uint8_t hit_zone;
    uint8_t victim_health;
};

#pragma pack(pop)

// Static ABI assertions
static_assert(std::is_standard_layout_v<PacketHeader>);
static_assert(sizeof(PacketHeader) == 4);

static_assert(std::is_standard_layout_v<ConnectRequestPayload>);
static_assert(sizeof(ConnectRequestPayload) == 12);

static_assert(std::is_standard_layout_v<ConnectAckPayload>);
static_assert(sizeof(ConnectAckPayload) == 10);

static_assert(std::is_standard_layout_v<DisconnectPayload>);
static_assert(sizeof(DisconnectPayload) == 8);

static_assert(std::is_standard_layout_v<ClientInputPayload>);
static_assert(sizeof(ClientInputPayload) == 24);

static_assert(std::is_standard_layout_v<EntitySnapshot>);
static_assert(sizeof(EntitySnapshot) == 44);

static_assert(std::is_standard_layout_v<ServerSnapshotPayload>);
static_assert(sizeof(ServerSnapshotPayload) == 9 + (kMaxPlayers * sizeof(EntitySnapshot)));

static_assert(std::is_standard_layout_v<HitNotificationPayload>);
static_assert(sizeof(HitNotificationPayload) == 32);

} // namespace engine
