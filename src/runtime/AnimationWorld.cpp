#include "runtime/AnimationWorld.h"

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace throne {
namespace {
using Key = std::pair<std::size_t, std::size_t>;
}

struct AnimationWorld::State {
    /// One posed node.
    struct Slot {
        std::string path;
        std::shared_ptr<const SkeletonAsset> asset;
        std::unique_ptr<ozz::animation::SamplingJob::Context> context;
        std::vector<ozz::math::SoaTransform> locals;
        std::vector<ozz::math::Float4x4> models;
        bool seen = false;
    };
    const AnimationLibrary& library;
    std::map<Key, Slot> slots;
    std::map<Key, Pose> published;

    explicit State(const AnimationLibrary& library) : library(library) {}
};

AnimationWorld::AnimationWorld(const AnimationLibrary& library) : state_(std::make_unique<State>(library)) {}
AnimationWorld::~AnimationWorld() = default;

void AnimationWorld::step(LevelExpansion& expanded, float dt) {
    auto& s = *state_;
    for (auto& [key, slot] : s.slots) slot.seen = false;
    for (std::size_t i = 0; i < expanded.instances.size(); ++i) {
        auto& nodes = expanded.instances[i].expansion.nodes;
        for (std::size_t n = 0; n < nodes.size(); ++n) {
            auto& node = nodes[n].node;
            if (!node.animator || !node.model || node.model->path.empty()) continue;
            auto& slot = s.slots[{i, n}];
            if (!slot.asset || slot.path != node.model->path) {
                slot.path = node.model->path;
                slot.asset = s.library.get(slot.path);
                if (!slot.asset) continue;
                const auto& skeleton = *slot.asset->skeleton;
                slot.context = std::make_unique<ozz::animation::SamplingJob::Context>(skeleton.num_joints());
                slot.locals.resize(std::size_t(skeleton.num_soa_joints()));
                slot.models.resize(std::size_t(skeleton.num_joints()));
            }
            slot.seen = true;
            auto& animator = *node.animator;
            const auto* clip = slot.asset->find(animator.clip);
            if (clip && animator.playing && dt != 0) {
                const float length = clip->duration();
                animator.time += dt * animator.speed;
                animator.time = animator.loop ? animator.time - std::floor(animator.time / length) * length
                                              : std::clamp(animator.time, 0.0f, length);
            }
            const auto& skeleton = *slot.asset->skeleton;
            if (clip) {
                ozz::animation::SamplingJob sampling;
                sampling.animation = clip;
                sampling.context = slot.context.get();
                sampling.ratio = std::clamp(animator.time / clip->duration(), 0.0f, 1.0f);
                sampling.output = ozz::make_span(slot.locals);
                if (!sampling.Run()) throw std::runtime_error("Animation sampling failed: " + animator.clip);
            } else {
                // No (known) clip: the rest pose.
                const auto rest = skeleton.joint_rest_poses();
                std::copy(rest.begin(), rest.end(), slot.locals.begin());
            }
            ozz::animation::LocalToModelJob toModel;
            toModel.skeleton = &skeleton;
            toModel.input = ozz::make_span(slot.locals);
            toModel.output = ozz::make_span(slot.models);
            if (!toModel.Run()) throw std::runtime_error("Animation pose failed: " + animator.clip);
        }
    }
    for (auto it = s.slots.begin(); it != s.slots.end();) it = it->second.seen ? std::next(it) : s.slots.erase(it);
}

void AnimationWorld::publish() {
    auto& s = *state_;
    s.published.clear();
    for (const auto& [key, slot] : s.slots) {
        if (!slot.asset) continue;
        auto& pose = s.published[key];
        pose.asset = slot.asset;
        pose.joints.resize(std::size_t(slot.asset->skeleton->num_joints()));
        // Four joints a SoA transform.
        for (std::size_t j = 0; j < pose.joints.size(); j += 4) {
            const auto& soa = slot.locals[j / 4];
            float values[10][4];
            const ozz::math::SimdFloat4 lanes[10] = {soa.translation.x, soa.translation.y, soa.translation.z,
                                                     soa.rotation.x,    soa.rotation.y,    soa.rotation.z,
                                                     soa.rotation.w,    soa.scale.x,       soa.scale.y,
                                                     soa.scale.z};
            for (int v = 0; v < 10; ++v) ozz::math::StorePtrU(lanes[v], values[v]);
            for (std::size_t lane = 0; lane < 4 && j + lane < pose.joints.size(); ++lane) {
                auto& joint = pose.joints[j + lane];
                for (int c = 0; c < 3; ++c) joint.position[c] = values[c][lane], joint.scale[c] = values[7 + c][lane];
                for (int c = 0; c < 4; ++c) joint.rotation[c] = values[3 + c][lane];
            }
        }
    }
}

const AnimationWorld::Pose* AnimationWorld::published(std::size_t instance, std::size_t node) const {
    const auto found = state_->published.find({instance, node});
    return found != state_->published.end() ? &found->second : nullptr;
}

bool AnimationWorld::jointMatrix(std::size_t instance, std::size_t node, std::string_view name, float matrix[16]) const {
    const auto found = state_->slots.find({instance, node});
    if (found == state_->slots.end() || !found->second.asset) return false;
    const int joint = found->second.asset->joint(name);
    if (joint < 0 || std::size_t(joint) >= found->second.models.size()) return false;
    const auto& m = found->second.models[std::size_t(joint)];
    for (int c = 0; c < 4; ++c) ozz::math::StorePtrU(m.cols[c], matrix + 4 * c);
    return true;
}

void AnimationWorld::clear() {
    state_->slots.clear();
    state_->published.clear();
}
}
