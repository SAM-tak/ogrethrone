#pragma once

#include "editor/Expansion.h"
#include <memory>

namespace throne {
/// A Play session's Box3D world, built from expanded scene components.
/// prepare/apply run on the main thread; advance runs only inside the engine group.
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
    /// Adds appended nodes and copies transforms/velocities/settings before the group.
    void prepare(const LevelExpansion& expanded);
    /// Advances by fixed 1/60 s steps with four substeps, retaining fractional time.
    void advance(float deltaSeconds);
    /// Publishes completed body poses and velocities into local node components.
    void apply(LevelExpansion& expanded);
    /// Runs internal Box3D tasks inline when enabled.
    void setSerial(bool serial);
    /// Number of scene bodies, including static collider-only nodes.
    std::size_t bodyCount() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
