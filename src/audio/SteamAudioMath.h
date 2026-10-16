#pragma once

#include "core/Math.h"
#include <phonon.h>

namespace throne {
/// Converts to a Steam Audio vector.
inline IPLVector3 toIPL(Vec3 v) { return {v.x, v.y, v.z}; }
}
