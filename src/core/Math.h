#pragma once

namespace throne {
/// Engine-neutral 3D vector.
///
/// Library types (OGRE, Box3D, Steam Audio) are converted at their adapter
/// boundaries so core code never depends on a specific library.
struct Vec3 {
    float x = 0, y = 0, z = 0;

    /// Returns the components as a contiguous `float[3]` (e.g. for ImGui widgets).
    float* data() { return &x; }
    /// @copydoc data()
    const float* data() const { return &x; }
};
static_assert(sizeof(Vec3) == 3 * sizeof(float), "Vec3 must be tightly packed for data()");

/// Component-wise sum.
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
/// Component-wise difference.
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
/// Scales every component by @p s.
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
/// Exact component-wise equality (no epsilon).
inline bool operator==(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
/// Negation of operator==().
inline bool operator!=(Vec3 a, Vec3 b) { return !(a == b); }
}
