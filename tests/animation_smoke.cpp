// Skeletal animation on the simulation side (TDDja/07 5.3), without OGRE: a three-joint arm
// built from raw data, sampled by AnimationWorld at known times.
#include "runtime/AnimationWorld.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
/// Within ozz's key quantization.
bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

/// root -> elbow -> hand, each one unit above its parent; "bend" turns the root from no
/// rotation to 90 degrees about Z over one second.
std::shared_ptr<const throne::SkeletonAsset> arm() {
    using namespace ozz::animation::offline;
    RawSkeleton raw;
    raw.roots.resize(1);
    auto& root = raw.roots[0];
    root.name = "root";
    root.transform = ozz::math::Transform::identity();
    root.children.resize(1);
    auto& elbow = root.children[0];
    elbow.name = "elbow";
    elbow.transform = ozz::math::Transform::identity();
    elbow.transform.translation = ozz::math::Float3(0, 1, 0);
    elbow.children.resize(1);
    auto& hand = elbow.children[0];
    hand.name = "hand";
    hand.transform = ozz::math::Transform::identity();
    hand.transform.translation = ozz::math::Float3(0, 1, 0);
    throne::RawClip bend;
    bend.name = "bend";
    bend.duration = 1;
    auto& track = bend.tracks["root"];
    track.rotations.push_back({0, ozz::math::Quaternion::identity()});
    track.rotations.push_back({1, ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3(0, 0, 1), 1.5707963f)});
    return throne::buildSkeleton(raw, {bend});
}

throne::LevelExpansion level() {
    throne::SceneNode node;
    node.id = 1;
    node.name = "Arm";
    node.model.emplace();
    node.model->path = "models/arm.fbx";
    node.animator.emplace();
    node.animator->clip = "bend";
    throne::LevelExpansion expanded;
    expanded.instances.push_back({1, {}, {}});
    expanded.instances[0].expansion.nodes.push_back({{1}, throne::Expansion::npos, node});
    return expanded;
}
}

int main() {
    try {
        throne::AnimationLibrary library;
        const auto asset = arm();
        require(asset->clips.size() == 1 && near(asset->duration("bend"), 1) && asset->joint("hand") >= 0, "Arm asset");
        library.put("models/arm.fbx", asset);
        throne::AnimationWorld world(library);
        auto expanded = level();
        float m[16];

        // Half way: the hand is two units out along the root's 45 degree turn.
        world.step(expanded, 0.5f);
        require(near(expanded.instances[0].expansion.nodes[0].node.animator->time, 0.5f), "Time not advanced");
        require(world.jointMatrix(0, 0, "hand", m), "No hand matrix");
        require(near(m[12], -2 * std::sin(0.7853982f)) && near(m[13], 2 * std::cos(0.7853982f)), "Hand pose wrong at 0.5 s");

        // Looping wraps 1.25 s round to 0.25 s; a held clip stops at its end.
        world.step(expanded, 0.75f);
        require(near(expanded.instances[0].expansion.nodes[0].node.animator->time, 0.25f), "Loop did not wrap");
        expanded.instances[0].expansion.nodes[0].node.animator->loop = false;
        world.step(expanded, 5);
        require(near(expanded.instances[0].expansion.nodes[0].node.animator->time, 1), "Held clip ran past its end");
        world.jointMatrix(0, 0, "hand", m);
        require(near(m[12], -2) && near(m[13], 0), "Hand pose wrong at the end");

        // The renderer keeps the published pose while the simulation moves on.
        world.publish();
        const auto* pose = world.published(0, 0);
        require(pose && pose->joints.size() == 3, "Pose not published");
        const int root = asset->joint("root");
        const float published = pose->joints[std::size_t(root)].rotation[2];
        require(near(published, std::sin(0.7853982f)), "Published root rotation wrong");
        auto& animator = *expanded.instances[0].expansion.nodes[0].node.animator;
        animator.time = 0;
        world.step(expanded, 0);
        require(near(world.published(0, 0)->joints[std::size_t(root)].rotation[2], published), "Published pose changed before publish");
        world.publish();
        require(near(world.published(0, 0)->joints[std::size_t(root)].rotation[2], 0), "Publish did not take the new pose");

        // An unknown clip shows the rest pose; a node losing its Animator loses its pose.
        animator.clip = "missing";
        world.step(expanded, 1);
        world.jointMatrix(0, 0, "hand", m);
        require(near(m[12], 0) && near(m[13], 2) && near(animator.time, 0), "Rest pose wrong");
        expanded.instances[0].expansion.nodes[0].node.animator.reset();
        world.step(expanded, 0);
        require(!world.jointMatrix(0, 0, "hand", m), "Pose kept without an Animator");
        std::puts("animation smoke passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "animation smoke failed: %s\n", error.what());
        return 1;
    }
}
