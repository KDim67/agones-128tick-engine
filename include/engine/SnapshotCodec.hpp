#pragma once

#include "Common.hpp"
#include "WorldState.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace engine {

// Wire precision of replicated state: 1/512 m positions, 1/256 m/s velocities, 2^16 steps per turn
constexpr float kPositionUnitsPerMetre = 512.0f;
constexpr float kVelocityUnitsPerMetre = 256.0f;
constexpr float kAngleUnitsPerDegree = 65536.0f / 360.0f;

constexpr uint32_t kNoBaseline = ~0u;
constexpr uint8_t kNoSelfSlot = 0xFF;
constexpr size_t kSnapshotFrameHistory = 64;

struct QuantizedEntity {
    std::array<int16_t, 3> position{};
    std::array<int16_t, 3> velocity{};
    int16_t pitch{0};
    uint16_t yaw{0};
    uint8_t flags{0};
    uint8_t health{0};

    [[nodiscard]] bool operator==(const QuantizedEntity&) const noexcept = default;
};

// Every replicated entity in one tick, exactly as every client decodes it
struct ReplicatedFrame {
    uint32_t tick{kNoBaseline};
    uint16_t present{0};
    std::array<QuantizedEntity, kMaxPlayers> entities{};
};

// The receiving client's own entity at full precision; prediction replays from it
struct SelfState {
    Vec3 position{};
    Vec3 velocity{};
    uint32_t last_input_seq{0};
    uint8_t flags{0};
    uint8_t health{0};
};

// Per-entity changed-field mask
constexpr uint32_t kFieldPosition = 1u << 0;
constexpr uint32_t kFieldVelocity = 1u << 1;
constexpr uint32_t kFieldView = 1u << 2;
constexpr uint32_t kFieldFlags = 1u << 3;
constexpr uint32_t kFieldHealth = 1u << 4;
constexpr uint32_t kFieldAll = 0x1Fu;

// [tick u32][baseline u32][present u16][self slot u8], then self state, then one field mask plus changed fields per other entity
constexpr size_t kSnapshotHeaderBytes = 11;
constexpr size_t kSelfStateBytes = 30;
constexpr size_t kMaxEntityDeltaBytes = 19;
constexpr size_t kMaxSnapshotBytes = kSnapshotHeaderBytes + kSelfStateBytes + (kMaxPlayers - 1) * kMaxEntityDeltaBytes;

static_assert(kMaxPlayers <= 16, "present mask is 16 bits");

[[nodiscard]] inline int16_t QuantizeSigned(float value, float units_per_unit) noexcept {
    const float scaled = std::isfinite(value) ? std::clamp(value * units_per_unit, -32767.0f, 32767.0f) : 0.0f;
    return static_cast<int16_t>(std::lround(scaled));
}

[[nodiscard]] inline uint16_t QuantizeYaw(float yaw_degrees) noexcept {
    const float scaled = std::isfinite(yaw_degrees) ? yaw_degrees * kAngleUnitsPerDegree : 0.0f;
    return static_cast<uint16_t>(std::lround(scaled) & 0xFFFF);
}

[[nodiscard]] inline QuantizedEntity QuantizeEntity(const PlayerEntity& player) noexcept {
    QuantizedEntity q;
    q.position = {QuantizeSigned(player.position.x, kPositionUnitsPerMetre),
                  QuantizeSigned(player.position.y, kPositionUnitsPerMetre),
                  QuantizeSigned(player.position.z, kPositionUnitsPerMetre)};
    q.velocity = {QuantizeSigned(player.velocity.x, kVelocityUnitsPerMetre),
                  QuantizeSigned(player.velocity.y, kVelocityUnitsPerMetre),
                  QuantizeSigned(player.velocity.z, kVelocityUnitsPerMetre)};
    q.pitch = QuantizeSigned(player.view_pitch, kAngleUnitsPerDegree);
    q.yaw = QuantizeYaw(player.view_yaw);
    q.flags = static_cast<uint8_t>(player.flags & 0xFFu);
    q.health = static_cast<uint8_t>(std::clamp<int16_t>(player.health, 0, 255));
    return q;
}

// Positions and velocities land exactly on the grid, so requantizing a decoded value is lossless
inline void ApplyQuantized(const QuantizedEntity& q, PlayerEntity& player) noexcept {
    player.position = {static_cast<float>(q.position[0]) / kPositionUnitsPerMetre,
                       static_cast<float>(q.position[1]) / kPositionUnitsPerMetre,
                       static_cast<float>(q.position[2]) / kPositionUnitsPerMetre};
    player.velocity = {static_cast<float>(q.velocity[0]) / kVelocityUnitsPerMetre,
                       static_cast<float>(q.velocity[1]) / kVelocityUnitsPerMetre,
                       static_cast<float>(q.velocity[2]) / kVelocityUnitsPerMetre};
    player.view_pitch = static_cast<float>(q.pitch) / kAngleUnitsPerDegree;
    player.view_yaw = static_cast<float>(q.yaw) / kAngleUnitsPerDegree;
    player.flags = q.flags;
    player.health = static_cast<int16_t>(q.health);
}

class ByteWriter {
public:
    ByteWriter(uint8_t* out, size_t capacity) noexcept : out_(out), capacity_(capacity) {}

    template <typename T>
    void Put(const T& value) noexcept {
        if (!ok_ || capacity_ - offset_ < sizeof(T)) {
            ok_ = false;
            return;
        }
        std::memcpy(out_ + offset_, &value, sizeof(T));
        offset_ += sizeof(T);
    }

    [[nodiscard]] size_t Finish() const noexcept { return ok_ ? offset_ : 0; }

private:
    uint8_t* out_;
    size_t capacity_;
    size_t offset_{0};
    bool ok_{true};
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t bytes) noexcept : data_(data), bytes_(bytes) {}

    template <typename T>
    [[nodiscard]] bool Get(T& value) noexcept {
        if (bytes_ - offset_ < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, data_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    [[nodiscard]] bool Exhausted() const noexcept { return offset_ == bytes_; }

private:
    const uint8_t* data_;
    size_t bytes_;
    size_t offset_{0};
};

struct SnapshotHeader {
    uint32_t tick{0};
    uint32_t baseline_tick{kNoBaseline};
    uint16_t present{0};
    uint8_t self_slot{kNoSelfSlot};
};

[[nodiscard]] inline bool ReadSnapshotHeader(const uint8_t* data, size_t bytes, SnapshotHeader& header) noexcept {
    ByteReader reader(data, bytes);
    return reader.Get(header.tick) && reader.Get(header.baseline_tick) && reader.Get(header.present) &&
           reader.Get(header.self_slot);
}

// Bytes written, 0 if out cannot hold them. Entities absent from the baseline are sent whole
[[nodiscard]] inline size_t EncodeSnapshot(const ReplicatedFrame& current, const ReplicatedFrame* baseline, uint8_t self_slot,
                                           const SelfState& self, uint8_t* out, size_t capacity) noexcept {
    const bool has_self = self_slot < kMaxPlayers && (current.present & (1u << self_slot)) != 0;

    ByteWriter writer(out, capacity);
    writer.Put(current.tick);
    writer.Put(baseline ? baseline->tick : kNoBaseline);
    writer.Put(current.present);
    writer.Put(has_self ? self_slot : kNoSelfSlot);

    if (has_self) {
        writer.Put(self.position.x);
        writer.Put(self.position.y);
        writer.Put(self.position.z);
        writer.Put(self.velocity.x);
        writer.Put(self.velocity.y);
        writer.Put(self.velocity.z);
        writer.Put(self.last_input_seq);
        writer.Put(self.flags);
        writer.Put(self.health);
    }

    for (uint32_t slot = 0; slot < kMaxPlayers; ++slot) {
        const uint32_t bit = 1u << slot;
        if ((current.present & bit) == 0 || (has_self && slot == self_slot)) {
            continue;
        }

        const QuantizedEntity& now = current.entities[slot];
        uint32_t changed = kFieldAll;
        if (baseline && (baseline->present & bit) != 0) {
            const QuantizedEntity& was = baseline->entities[slot];
            changed = (now.position != was.position ? kFieldPosition : 0u) |
                      (now.velocity != was.velocity ? kFieldVelocity : 0u) |
                      (now.pitch != was.pitch || now.yaw != was.yaw ? kFieldView : 0u) |
                      (now.flags != was.flags ? kFieldFlags : 0u) |
                      (now.health != was.health ? kFieldHealth : 0u);
        }

        const auto mask = static_cast<uint8_t>(changed);
        writer.Put(mask);
        if (mask & kFieldPosition) writer.Put(now.position);
        if (mask & kFieldVelocity) writer.Put(now.velocity);
        if (mask & kFieldView) {
            writer.Put(now.pitch);
            writer.Put(now.yaw);
        }
        if (mask & kFieldFlags) writer.Put(now.flags);
        if (mask & kFieldHealth) writer.Put(now.health);
    }
    return writer.Finish();
}

// False on any framing error or a baseline the packet does not name; every byte must be consumed
[[nodiscard]] inline bool DecodeSnapshot(const uint8_t* data, size_t bytes, const ReplicatedFrame* baseline,
                                         ReplicatedFrame& frame, SelfState& self, uint8_t& self_slot) noexcept {
    ByteReader reader(data, bytes);
    SnapshotHeader header;
    if (!reader.Get(header.tick) || !reader.Get(header.baseline_tick) || !reader.Get(header.present) ||
        !reader.Get(header.self_slot)) {
        return false;
    }
    if ((header.present >> kMaxPlayers) != 0) {
        return false;
    }
    if (header.baseline_tick == kNoBaseline) {
        baseline = nullptr;
    } else if (!baseline || baseline->tick != header.baseline_tick) {
        return false;
    }

    const bool has_self = header.self_slot != kNoSelfSlot;
    if (has_self && (header.self_slot >= kMaxPlayers || (header.present & (1u << header.self_slot)) == 0)) {
        return false;
    }

    frame.tick = header.tick;
    frame.present = header.present;
    self_slot = header.self_slot;
    if (has_self) {
        if (!reader.Get(self.position.x) || !reader.Get(self.position.y) || !reader.Get(self.position.z) ||
            !reader.Get(self.velocity.x) || !reader.Get(self.velocity.y) || !reader.Get(self.velocity.z) ||
            !reader.Get(self.last_input_seq) || !reader.Get(self.flags) || !reader.Get(self.health)) {
            return false;
        }
    }

    for (uint32_t slot = 0; slot < kMaxPlayers; ++slot) {
        const uint32_t bit = 1u << slot;
        if ((header.present & bit) == 0 || (has_self && slot == header.self_slot)) {
            continue;
        }

        uint8_t mask = 0;
        if (!reader.Get(mask) || (mask & ~kFieldAll) != 0) {
            return false;
        }

        QuantizedEntity entity{};
        if (baseline && (baseline->present & bit) != 0) {
            entity = baseline->entities[slot];
        } else if (mask != kFieldAll) {
            return false;
        }

        if ((mask & kFieldPosition) && !reader.Get(entity.position)) return false;
        if ((mask & kFieldVelocity) && !reader.Get(entity.velocity)) return false;
        if ((mask & kFieldView) && (!reader.Get(entity.pitch) || !reader.Get(entity.yaw))) return false;
        if ((mask & kFieldFlags) && !reader.Get(entity.flags)) return false;
        if ((mask & kFieldHealth) && !reader.Get(entity.health)) return false;
        frame.entities[slot] = entity;
    }
    return reader.Exhausted();
}

} // namespace engine
