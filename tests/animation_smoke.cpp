// Skeletal animation on the simulation side (TDDja/07 5.3), without OGRE: a three-joint arm
// built from raw data, sampled by AnimationWorld at known times.
#include "runtime/AnimationWorld.h"
#include "runtime/FbxSkeleton.h"
#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <string>
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

throne::LevelExpansion level();

/// The largest difference between the 3x4 part of ozz matrix @p m and @p u.
float difference(const float m[16], const ufbx_matrix& u) {
    float most = 0;
    for (int c = 0; c < 4; ++c) {
        const ufbx_vec3& column = u.cols[c];
        most = std::max({most, std::fabs(m[4 * c] - float(column.x)), std::fabs(m[4 * c + 1] - float(column.y)),
                         std::fabs(m[4 * c + 2] - float(column.z))});
    }
    return most;
}

/// A skinned, animated FBX from ufbx's test data, read as the Model component reads files: the
/// sampled joints must follow ufbx's own evaluation of the clip, and the rest pose must be the
/// pose the skin was bound in.
void fbx(const std::string& data) {
    ufbx_load_opts options = {};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file((data + "/max7_skin_5000_binary.fbx").c_str(), &options, &error);
    require(scene, "Cannot read the skinned FBX");
    const auto skeleton = throne::readFbxSkeleton(*scene);
    require(skeleton.asset && !skeleton.asset->clips.empty() && scene->anim_stacks.count > 0, "FBX skeleton or clip missing");
    throne::AnimationLibrary library;
    library.put("skin.fbx", skeleton.asset);
    throne::AnimationWorld world(library);
    auto expanded = level();
    auto& node = expanded.instances[0].expansion.nodes[0].node;
    node.model->path = "skin.fbx";
    node.animator->clip.clear();
    float m[16];

    // Rest pose: each cluster's bone, times the cluster's geometry_to_bone, places the mesh
    // where every cluster agrees it was bound.
    world.step(expanded, 0);
    for (std::size_t i = 0; i < scene->skin_clusters.count; ++i) {
        const auto* cluster = scene->skin_clusters.data[i];
        const int joint = skeleton.joint(cluster->bone_node);
        require(joint >= 0 && world.jointMatrix(0, 0, skeleton.names[std::size_t(joint)], m), "Clustered bone not a joint");
        require(difference(m, cluster->bind_to_world) < 1e-3f, "Rest pose is not the bind pose");
    }

    // Part way through the first clip, every joint is where ufbx evaluates its node.
    const auto* stack = scene->anim_stacks.data[0];
    node.animator->clip = skeleton.asset->clips[0].name;
    node.animator->time = skeleton.asset->duration(node.animator->clip) * 0.37f;
    node.animator->playing = false;
    world.step(expanded, 0);
    ufbx_scene* posed = ufbx_evaluate_scene(scene, stack->anim, stack->time_begin + node.animator->time, nullptr, &error);
    require(posed, "ufbx could not evaluate the clip");
    float worst = 0, size = 0;
    for (std::size_t j = 0; j < skeleton.nodes.size(); ++j) {
        require(world.jointMatrix(0, 0, skeleton.names[j], m), "Joint not sampled");
        const auto& expected = posed->nodes.data[skeleton.nodes[j]->typed_id]->node_to_world;
        worst = std::max(worst, difference(m, expected));
        size = std::max({size, std::fabs(float(expected.cols[3].x)), std::fabs(float(expected.cols[3].y)),
                         std::fabs(float(expected.cols[3].z))});
    }
    ufbx_free_scene(posed);
    ufbx_free_scene(scene);
    // ozz quantizes keys and ufbx resamples curves at 30 per second: allow 1% of the extent.
    if (!(worst < 2e-3f + size * 1e-2f)) {
        std::fprintf(stderr, "joint error %g, extent %g\n", worst, size);
        throw std::runtime_error("Sampled joints do not follow the FBX clip");
    }
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

int main(int argc, char** argv) {
    try {
        if (argc > 1) fbx(argv[1]);
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
