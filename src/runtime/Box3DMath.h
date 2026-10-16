#pragma once

#include "core/Math.h"
#include <box3d/math_functions.h>

namespace throne {
/// Converts to a Box3D world position.
///
/// b3Pos is double in BOX3D_DOUBLE_PRECISION builds and b3Vec3 otherwise.
inline b3Pos toB3Pos(Vec3 v) { return {v.x, v.y, v.z}; }
/// Converts a Box3D world position, narrowing to float in double-precision builds.
inline Vec3 fromB3Pos(const b3Pos& p) {
    return {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
}
}
