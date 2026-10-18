#pragma once

#include "core/Math.h"
#include <OgreVector3.h>

namespace throne {
/// Converts to an OGRE vector.
inline Ogre::Vector3 toOgre(Vec3 v) { return {v.x, v.y, v.z}; }
/// Converts an OGRE vector, narrowing to float when Ogre::Real is double.
inline Vec3 fromOgre(const Ogre::Vector3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
}
