#pragma once

#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <cmath>
#include <algorithm>

namespace engine {

// 3D vector primitive with deterministic float arithmetic
struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    constexpr Vec3() noexcept = default;
    constexpr Vec3(float x_, float y_, float z_) noexcept : x(x_), y(y_), z(z_) {}

    constexpr Vec3 operator+(const Vec3& rhs) const noexcept { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
    constexpr Vec3 operator-(const Vec3& rhs) const noexcept { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
    constexpr Vec3 operator*(float scalar) const noexcept { return {x * scalar, y * scalar, z * scalar}; }

    Vec3& operator+=(const Vec3& rhs) noexcept {
        x += rhs.x; y += rhs.y; z += rhs.z;
        return *this;
    }

    Vec3& operator-=(const Vec3& rhs) noexcept {
        x -= rhs.x; y -= rhs.y; z -= rhs.z;
        return *this;
    }

    Vec3& operator*=(float scalar) noexcept {
        x *= scalar; y *= scalar; z *= scalar;
        return *this;
    }

    [[nodiscard]] float LengthSq() const noexcept { return x * x + y * y + z * z; }
    [[nodiscard]] float Length() const noexcept { return std::sqrt(LengthSq()); }

    [[nodiscard]] Vec3 Normalized() const noexcept {
        const float len = Length();
        if (len > 1e-6f) {
            const float inv = 1.0f / len;
            return {x * inv, y * inv, z * inv};
        }
        return {0.0f, 0.0f, 0.0f};
    }

    [[nodiscard]] static constexpr float Dot(const Vec3& a, const Vec3& b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    [[nodiscard]] static constexpr Vec3 Lerp(const Vec3& a, const Vec3& b, float t) noexcept {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }
};

// Spatial ray for ballistic hit testing
struct Ray {
    Vec3 origin;
    Vec3 direction; // Normalized directional vector
};

// Axis-aligned bounding box representation for player hitboxes
struct AABB {
    Vec3 min;
    Vec3 max;

    [[nodiscard]] static AABB FromCenterAndExtents(const Vec3& center, const Vec3& half_extents) noexcept {
        return {center - half_extents, center + half_extents};
    }

    // Branchless slab intersection test returning hit boolean and entry distance
    [[nodiscard]] bool IntersectsRay(const Ray& ray, float max_dist, float& out_hit_dist) const noexcept {
        float tmin = 0.0f;
        float tmax = max_dist;

        // X axis slab
        if (std::abs(ray.direction.x) > 1e-6f) {
            const float inv_d = 1.0f / ray.direction.x;
            float t1 = (min.x - ray.origin.x) * inv_d;
            float t2 = (max.x - ray.origin.x) * inv_d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        } else if (ray.origin.x < min.x || ray.origin.x > max.x) {
            return false;
        }

        // Y axis slab
        if (std::abs(ray.direction.y) > 1e-6f) {
            const float inv_d = 1.0f / ray.direction.y;
            float t1 = (min.y - ray.origin.y) * inv_d;
            float t2 = (max.y - ray.origin.y) * inv_d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        } else if (ray.origin.y < min.y || ray.origin.y > max.y) {
            return false;
        }

        // Z axis slab
        if (std::abs(ray.direction.z) > 1e-6f) {
            const float inv_d = 1.0f / ray.direction.z;
            float t1 = (min.z - ray.origin.z) * inv_d;
            float t2 = (max.z - ray.origin.z) * inv_d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        } else if (ray.origin.z < min.z || ray.origin.z > max.z) {
            return false;
        }

        out_hit_dist = tmin;
        return true;
    }
};

// Convert spherical Euler angles in degrees to normalized Cartesian direction
inline Vec3 AnglesToDirection(float pitch_deg, float yaw_deg) noexcept {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float pitch_rad = pitch_deg * kDegToRad;
    const float yaw_rad = yaw_deg * kDegToRad;

    const float cos_pitch = std::cos(pitch_rad);
    return {
        std::sin(yaw_rad) * cos_pitch,
        -std::sin(pitch_rad),
        std::cos(yaw_rad) * cos_pitch
    };
}

} // namespace engine
