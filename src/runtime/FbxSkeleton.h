#pragma once

#include "runtime/AnimationLibrary.h"
#include <ufbx.h>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace throne {
/// The skeleton of an FBX file's skinned meshes and its animation stacks (TDDja/07 5.4), read
/// with ufbx without OGRE: the bones the skins bind and all their ancestors up to the scene
/// root (which carries the axis and unit conversion), so every joint's transform is relative
/// to its parent node's, as the file animates it.
struct FbxSkeleton {
    /// The joints, parents first; a joint's index is its OGRE bone handle.
    std::vector<const ufbx_node*> nodes;
    /// Each joint's parent, or -1 for the scene root.
    std::vector<int> parents;
    /// Unique joint names (the file's, numbered when repeated), which match ozz's joints to
    /// OGRE's bones.
    std::vector<std::string> names;
    /// Each joint's bind pose, relative to its parent: position, rotation (x, y, z, w), scale.
    std::vector<std::array<float, 10>> rest;
    /// Each joint's bind pose in the scene.
    std::vector<ufbx_matrix> world;
    /// The ozz skeleton and one clip per animation stack; null when the file has no skin.
    std::shared_ptr<const SkeletonAsset> asset;

    /// The joint of @p node, or -1.
    int joint(const ufbx_node* node) const;

private:
    std::map<const ufbx_node*, int> indices_;
    friend FbxSkeleton readFbxSkeleton(const ufbx_scene& scene);
};

/// Reads @p scene's skeleton and clips; empty (no joints) when no mesh is skinned. Bones take
/// their bind pose where a skin cluster gives one, so a mesh's vertices in its bind pose
/// (cluster bind_to_world times geometry_to_bone) skin correctly; other joints take their
/// pose in the file. Clips are baked by ufbx, starting at zero.
/// @throws std::runtime_error when ozz rejects the data.
FbxSkeleton readFbxSkeleton(const ufbx_scene& scene);
}
