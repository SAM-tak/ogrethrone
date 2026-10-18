#pragma once

#include "core/Math.h"
#include "runtime/TaskPool.h"
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
    /// Forces Box3D task callbacks to run inline for deterministic debugging.
    void setSerial(bool serial) { serial_ = serial; }
    /// Returns the falling box's world position.
    Vec3 position() const;

private:
    TaskPool tasks_;
    bool serial_ = false;
    b3WorldId world_;
    b3BodyId box_;
};
}
