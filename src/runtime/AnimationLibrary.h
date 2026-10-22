#pragma once

#include "core/AssetRef.h"
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/base/memory/unique_ptr.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace ozz::animation {
class Animation;
class Skeleton;
}

namespace throne {
/// One model's skeleton and clips for ozz-animation (TDDja/07 5.3). Built once and then only
/// read, so the simulation and the renderer share it across threads.
struct SkeletonAsset {
    struct Clip {
        std::string name;
        ozz::unique_ptr<ozz::animation::Animation> animation;
    };
    /// The joints in ozz's order (depth-first), which importers' bone orders need not match.
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    std::vector<Clip> clips;

    SkeletonAsset();
    ~SkeletonAsset();
    /// The clip named @p name, or nullptr.
    const ozz::animation::Animation* find(std::string_view name) const;
    /// The length in seconds of the clip named @p name; 0 when there is none.
    float duration(std::string_view name) const;
    /// The index of the joint named @p name, or -1.
    int joint(std::string_view name) const;
};

/// One clip as an importer reads it: keys in seconds of each joint, by joint name. Joints
/// without a track keep their rest pose.
struct RawClip {
    std::string name;
    float duration = 0;
    std::map<std::string, ozz::animation::offline::RawAnimation::JointTrack> tracks;
};

/// Builds the runtime skeleton and clips; throws std::runtime_error when the data is invalid
/// (empty or too large skeletons, keys outside the duration or out of order).
std::shared_ptr<const SkeletonAsset> buildSkeleton(const ozz::animation::offline::RawSkeleton& skeleton,
                                                   const std::vector<RawClip>& clips);

/// The skeletons of the models read so far, by the Model component's file. The editor adds
/// them on the main thread as models finish reading; the simulation looks them up from its
/// own thread.
class AnimationLibrary {
public:
    /// Adds or replaces the skeleton of the model @p model.
    void put(const AssetRef& model, std::shared_ptr<const SkeletonAsset> asset);
    /// The skeleton of the model @p model, or null; safe from any thread.
    std::shared_ptr<const SkeletonAsset> get(const AssetRef& model) const;
    /// get() without sharing ownership, for the main thread (which alone calls put()).
    const SkeletonAsset* find(const AssetRef& model) const { return get(model).get(); }
    /// Forgets every skeleton (a project closing).
    void clear();

private:
    mutable std::mutex mutex_;
    std::map<AssetRef, std::shared_ptr<const SkeletonAsset>> assets_;
};
}
