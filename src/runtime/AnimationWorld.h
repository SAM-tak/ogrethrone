#pragma once

#include "editor/Expansion.h"
#include "runtime/AnimationLibrary.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace throne {
/// The skeletal poses of a level's Animator components (TDDja/07 5.3): a pool the Animation
/// stage owns, as PhysicsWorld owns the Box3D bodies. step() runs on the simulation column
/// and samples each pose with ozz-animation; publish() hands them to the renderer at
/// SyncSimulation, so drawing never sees a pose being written.
class AnimationWorld {
public:
    /// One joint's local transform, relative to its parent joint.
    struct JointPose {
        float position[3];
        /// A unit quaternion, x, y, z, w.
        float rotation[4];
        float scale[3];
    };
    /// A published pose: the skeleton it is of and its joints in the skeleton's order.
    struct Pose {
        std::shared_ptr<const SkeletonAsset> asset;
        std::vector<JointPose> joints;
    };

    /// @param library Where skeletons are looked up by the Model component's path; it must
    ///                outlive the world.
    explicit AnimationWorld(const AnimationLibrary& library);
    ~AnimationWorld();
    AnimationWorld(const AnimationWorld&) = delete;
    AnimationWorld& operator=(const AnimationWorld&) = delete;

    /// Advances every playing Animator of a node with a Model by @p dt times its speed
    /// (wrapping or holding at the clip's end), then samples the nodes' poses. A @p dt of 0
    /// only samples, which is what the editor shows outside Play. Nodes whose model has no
    /// skeleton yet are left alone.
    void step(LevelExpansion& expanded, float dt);
    /// Copies the poses step() sampled for the renderer.
    void publish();
    /// The published pose of node @p node of scene instance @p instance, or nullptr.
    const Pose* published(std::size_t instance, std::size_t node) const;
    /// Writes the model-space transform (relative to the node) of the joint named @p joint of
    /// that node as step() last sampled it, column-major, into @p matrix; false when there is none.
    bool jointMatrix(std::size_t instance, std::size_t node, std::string_view joint, float matrix[16]) const;
    /// Forgets every pose.
    void clear();

private:
    struct State;
    std::unique_ptr<State> state_;
};
}
