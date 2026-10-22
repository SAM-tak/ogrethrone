#include "runtime/AnimationLibrary.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <stdexcept>

namespace throne {
SkeletonAsset::SkeletonAsset() = default;
SkeletonAsset::~SkeletonAsset() = default;

const ozz::animation::Animation* SkeletonAsset::find(std::string_view name) const {
    for (const auto& clip : clips)
        if (clip.name == name) return clip.animation.get();
    return nullptr;
}

float SkeletonAsset::duration(std::string_view name) const {
    const auto* animation = find(name);
    return animation ? animation->duration() : 0.0f;
}

int SkeletonAsset::joint(std::string_view name) const {
    const auto names = skeleton->joint_names();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (name == names[i]) return int(i);
    return -1;
}

std::shared_ptr<const SkeletonAsset> buildSkeleton(const ozz::animation::offline::RawSkeleton& raw,
                                                   const std::vector<RawClip>& clips) {
    auto asset = std::make_shared<SkeletonAsset>();
    if (!raw.Validate() || raw.num_joints() == 0) throw std::runtime_error("Invalid skeleton");
    asset->skeleton = ozz::animation::offline::SkeletonBuilder()(raw);
    if (!asset->skeleton) throw std::runtime_error("Invalid skeleton");
    // Tracks follow the runtime skeleton's joint order.
    const auto names = asset->skeleton->joint_names();
    for (const auto& clip : clips) {
        ozz::animation::offline::RawAnimation animation;
        animation.name = clip.name.c_str();
        animation.duration = clip.duration;
        animation.tracks.resize(names.size());
        for (std::size_t i = 0; i < names.size(); ++i)
            if (const auto track = clip.tracks.find(names[i]); track != clip.tracks.end()) animation.tracks[i] = track->second;
        // A joint without keys holds its rest pose.
        const auto rest = asset->skeleton->joint_rest_poses();
        for (std::size_t i = 0; i < names.size(); ++i) {
            auto& track = animation.tracks[i];
            const auto& soa = rest[i / 4];
            const int lane = int(i % 4);
            float x[4], y[4], z[4], w[4];
            if (track.translations.empty()) {
                ozz::math::StorePtrU(soa.translation.x, x), ozz::math::StorePtrU(soa.translation.y, y);
                ozz::math::StorePtrU(soa.translation.z, z);
                track.translations.push_back({0, ozz::math::Float3(x[lane], y[lane], z[lane])});
            }
            if (track.rotations.empty()) {
                ozz::math::StorePtrU(soa.rotation.x, x), ozz::math::StorePtrU(soa.rotation.y, y);
                ozz::math::StorePtrU(soa.rotation.z, z), ozz::math::StorePtrU(soa.rotation.w, w);
                track.rotations.push_back({0, ozz::math::Quaternion(x[lane], y[lane], z[lane], w[lane])});
            }
            if (track.scales.empty()) {
                ozz::math::StorePtrU(soa.scale.x, x), ozz::math::StorePtrU(soa.scale.y, y);
                ozz::math::StorePtrU(soa.scale.z, z);
                track.scales.push_back({0, ozz::math::Float3(x[lane], y[lane], z[lane])});
            }
        }
        if (!animation.Validate()) throw std::runtime_error("Invalid animation clip: " + clip.name);
        auto built = ozz::animation::offline::AnimationBuilder()(animation);
        if (!built) throw std::runtime_error("Invalid animation clip: " + clip.name);
        asset->clips.push_back({clip.name, std::move(built)});
    }
    return asset;
}

void AnimationLibrary::put(const AssetRef& model, std::shared_ptr<const SkeletonAsset> asset) {
    std::lock_guard lock(mutex_);
    assets_[model] = std::move(asset);
}

std::shared_ptr<const SkeletonAsset> AnimationLibrary::get(const AssetRef& model) const {
    std::lock_guard lock(mutex_);
    const auto found = assets_.find(model);
    return found != assets_.end() ? found->second : nullptr;
}

void AnimationLibrary::clear() {
    std::lock_guard lock(mutex_);
    assets_.clear();
}
}
