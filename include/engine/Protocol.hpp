#pragma once

#include "Common.hpp"
#include <cstring>
#include <type_traits>

namespace engine {

#pragma pack(push, 1)

enum class PacketType : uint8_t {
    ConnectRequest = 1,
    ConnectAck = 2,
    ClientInput = 3,
    ServerSnapshot = 4,
    Disconnect = 5,
    HitNotification = 6,
    ShotResult = 7
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

// Rewind target lerp(ack_tick, ack_tick + interp_span_ticks, sub_tick_fraction) replays the client render pair
struct ClientInputPayload {
    uint32_t sequence;
    uint32_t ack_tick;
    float sub_tick_fraction;
    float view_pitch;
    float view_yaw;
    uint16_t input_flags;
    uint8_t interp_span_ticks;
    uint8_t reserved;
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
    uint32_t last_input_seq;
    uint8_t health;
    uint8_t reserved[3];
};

struct ServerSnapshotPayload {
    uint32_t server_tick;
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

enum class ShotStatus : uint8_t {
    Hit = 0,
    Miss = 1,
    Rejected = 2,
    ShooterDead = 3,
    Throttled = 4
};

// Authoritative verdict for every fire input; target_mask has a bit per slot that was hittable at the rewound tick
struct ShotResultPayload {
    uint32_t input_sequence;
    uint32_t server_tick;
    uint32_t victim_slot;
    float hit_x;
    float hit_y;
    float hit_z;
    uint16_t target_mask;
    uint8_t status;
    uint8_t hit_zone;
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
static_assert(sizeof(EntitySnapshot) == 48);

static_assert(std::is_standard_layout_v<ServerSnapshotPayload>);
static_assert(sizeof(ServerSnapshotPayload) == 5 + (kMaxPlayers * sizeof(EntitySnapshot)));

static_assert(std::is_standard_layout_v<HitNotificationPayload>);
static_assert(sizeof(HitNotificationPayload) == 32);

static_assert(std::is_standard_layout_v<ShotResultPayload>);
static_assert(sizeof(ShotResultPayload) == 28);
static_assert(kMaxPlayers <= 16);

// Redundant input batch: [count:u8][count x ClientInputPayload], oldest first
[[nodiscard]] constexpr size_t InputBatchBytes(size_t count) noexcept {
    return 1 + count * sizeof(ClientInputPayload);
}

constexpr size_t kMaxInputBatchBytes = InputBatchBytes(kInputRedundancy);
constexpr size_t kMaxControlPayloadBytes = std::max({sizeof(ConnectRequestPayload), sizeof(ConnectAckPayload),
                                                     sizeof(DisconnectPayload), sizeof(HitNotificationPayload),
                                                     sizeof(ShotResultPayload)});

static_assert(sizeof(PacketHeader) + sizeof(ServerSnapshotPayload) <= kMaxMtuPayload);
static_assert(sizeof(PacketHeader) + kMaxInputBatchBytes <= kMaxMtuPayload);

// Decoded input count, 0 if malformed
[[nodiscard]] inline size_t DecodeInputBatch(const uint8_t* payload, size_t bytes,
                                             std::array<ClientInputPayload, kInputRedundancy>& out) noexcept {
    if (bytes < 1) {
        return 0;
    }
    const size_t count = payload[0];
    if (count == 0 || count > kInputRedundancy || bytes != InputBatchBytes(count)) {
        return 0;
    }
    std::memcpy(out.data(), payload + 1, count * sizeof(ClientInputPayload));
    return count;
}

// Encoded byte count, 0 if count or capacity is invalid
[[nodiscard]] inline size_t EncodeInputBatch(const ClientInputPayload* inputs, size_t count,
                                             uint8_t* out, size_t capacity) noexcept {
    if (count == 0 || count > kInputRedundancy || capacity < InputBatchBytes(count)) {
        return 0;
    }
    out[0] = static_cast<uint8_t>(count);
    std::memcpy(out + 1, inputs, count * sizeof(ClientInputPayload));
    return InputBatchBytes(count);
}

} // namespace engine
