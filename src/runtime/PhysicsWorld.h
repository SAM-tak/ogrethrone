#pragma once

#include "editor/Expansion.h"
#include <memory>

namespace throne {
/// A Play session's Box3D world, built from expanded scene components.
/// step runs on the simulation column's thread, inside UpdateLoop.
class PhysicsWorld {
public:
    /// Creates an empty physics world.
    PhysicsWorld();
    /// Destroys all bodies, collision meshes and worker tasks.
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    /// Replaces the world with bodies/shapes from every scene instance.
    /// Throws for unsupported dynamic triangle meshes or invalid component values.
    void load(const LevelExpansion& expanded);
    /// Advances one fixedStep with four substeps: adds appended nodes, takes transform,
    /// velocity and setting edits as input, then publishes body poses and velocities
    /// into local node components.
    void step(LevelExpansion& expanded);
    /// Runs internal Box3D tasks inline when enabled.
    void setSerial(bool serial);
    /// Number of scene bodies, including static collider-only nodes.
    std::size_t bodyCount() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
