#pragma once

#include "core/Math.h"
#include <box3d/box3d.h>

namespace throne {
/// Bootstrap Box3D world: a static ground plane and one dynamic box falling onto it.
class PhysicsWorld {
public:
    /// Creates the world, ground and box.
    PhysicsWorld();
    /// Destroys the world and all its bodies.
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /// Advances the simulation by a fixed 1/60 s step with 4 sub-steps.
    void step();
    /// Returns the falling box's world position.
    Vec3 position() const;

private:
    b3WorldId world_;
    b3BodyId box_;
};
}
