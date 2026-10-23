#include "runtime/AnimationLibrary.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <stdexcept>

namespace throne {
namespace {
/// Adds keys to @p keys, interpolated by @p lerp, so that none lies more than @p gap seconds
/// from the next or from the clip's ends. ozz's AnimationBuilder adds a key wherever two keys
/// of a track lie more than 65535 keys apart in its time-sorted list, restarting its whole pass
/// each time: minutes for a long clip whose constant tracks only have a key at each end.
template <typename Key, typename Lerp>
void densify(ozz::vector<Key>& keys, float gap, float duration, const Lerp& lerp) {
    if (keys.front().time > 0) keys.insert(keys.begin(), {0, keys.front().value});
    if (keys.back().time < duration) keys.push_back({duration, keys.back().value});
    ozz::vector<Key> dense;
    dense.reserve(keys.size());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        if (k) {
            const auto &a = keys[k - 1], &b = keys[k];
            const int steps = int((b.time - a.time) / gap);
            for (int s = 1; s <= steps; ++s) {
                const float f = float(s) / float(steps + 1);
                dense.push_back({a.time + (b.time - a.time) * f, lerp(a.value, b.value, f)});
            }
        }
        dense.push_back(keys[k]);
    }
    keys = std::move(dense);
}
}

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
        // Each kind of key is sorted on its own; about half of ozz's limit lies between a track's keys.
        std::size_t counts[3] = {};
        for (const auto& track : animation.tracks)
            counts[0] += track.translations.size(), counts[1] += track.rotations.size(), counts[2] += track.scales.size();
        const auto gap = [&](std::size_t count) { return clip.duration * 32768.0f / float(count); };
        const auto shortest = [](const ozz::math::Quaternion& a, const ozz::math::Quaternion& b, float f) {
            const bool flip = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0;
            return ozz::math::NLerp(a, flip ? -b : b, f);
        };
        const auto lerp = [](const ozz::math::Float3& a, const ozz::math::Float3& b, float f) { return ozz::math::Lerp(a, b, f); };
        for (auto& track : animation.tracks) {
            densify(track.translations, gap(counts[0]), clip.duration, lerp);
            densify(track.rotations, gap(counts[1]), clip.duration, shortest);
            densify(track.scales, gap(counts[2]), clip.duration, lerp);
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
