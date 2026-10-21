#include "runtime/FbxSkeleton.h"

#include <ufbx.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace throne {
int FbxSkeleton::joint(const ufbx_node* node) const {
    const auto found = indices_.find(node);
    return found != indices_.end() ? found->second : -1;
}

FbxSkeleton readFbxSkeleton(const ufbx_scene& scene) {
    FbxSkeleton out;
    // The bind pose of every bone a cluster binds; the joints are those bones and their ancestors.
    std::map<const ufbx_node*, ufbx_matrix> bind;
    for (std::size_t i = 0; i < scene.skin_clusters.count; ++i) {
        const auto* cluster = scene.skin_clusters.data[i];
        if (!cluster->bone_node) continue;
        bind.try_emplace(cluster->bone_node, cluster->bind_to_world);
        for (const auto* node = cluster->bone_node; node && !out.indices_.count(node); node = node->parent)
            out.indices_[node] = 0;
    }
    if (out.indices_.empty()) return out;
    // Parents first: by depth, then in the file's order.
    for (std::size_t i = 0; i < scene.nodes.count; ++i)
        if (out.indices_.count(scene.nodes.data[i])) out.nodes.push_back(scene.nodes.data[i]);
    std::stable_sort(out.nodes.begin(), out.nodes.end(),
                     [](const ufbx_node* a, const ufbx_node* b) { return a->node_depth < b->node_depth; });
    std::map<std::string, int> used;
    auto& world = out.world;
    world.resize(out.nodes.size());
    for (std::size_t j = 0; j < out.nodes.size(); ++j) {
        const auto* node = out.nodes[j];
        out.indices_[node] = int(j);
        out.parents.push_back(node->parent ? out.joint(node->parent) : -1);
        std::string name = node->is_root ? std::string("Root") : std::string(node->name.data, node->name.length);
        if (name.empty()) name = "Joint";
        if (const int seen = used[name]++) name += "#" + std::to_string(seen);
        out.names.push_back(name);
        const auto found = bind.find(node);
        world[j] = found != bind.end() ? found->second : node->node_to_world;
        const auto parent = out.parents.back();
        auto local = world[j];
        if (parent >= 0) {
            const auto toParent = ufbx_matrix_invert(&world[std::size_t(parent)]);
            local = ufbx_matrix_mul(&toParent, &world[j]);
        }
        const auto t = ufbx_matrix_to_transform(&local);
        out.rest.push_back({float(t.translation.x), float(t.translation.y), float(t.translation.z), float(t.rotation.x),
                            float(t.rotation.y), float(t.rotation.z), float(t.rotation.w), float(t.scale.x), float(t.scale.y),
                            float(t.scale.z)});
    }

    using namespace ozz::animation::offline;
    RawSkeleton raw;
    std::vector<std::vector<int>> children(out.nodes.size());
    std::vector<int> roots;
    for (std::size_t j = 0; j < out.nodes.size(); ++j)
        (out.parents[j] < 0 ? roots : children[std::size_t(out.parents[j])]).push_back(int(j));
    const auto fill = [&](auto& self, RawSkeleton::Joint& joint, int j) -> void {
        const auto& r = out.rest[std::size_t(j)];
        joint.name = out.names[std::size_t(j)].c_str();
        joint.transform.translation = ozz::math::Float3(r[0], r[1], r[2]);
        joint.transform.rotation = ozz::math::Quaternion(r[3], r[4], r[5], r[6]);
        joint.transform.scale = ozz::math::Float3(r[7], r[8], r[9]);
        const auto& below = children[std::size_t(j)];
        joint.children.resize(below.size());
        for (std::size_t c = 0; c < below.size(); ++c) self(self, joint.children[c], below[c]);
    };
    raw.roots.resize(roots.size());
    for (std::size_t r = 0; r < roots.size(); ++r) fill(fill, raw.roots[r], roots[r]);

    std::vector<RawClip> clips;
    for (std::size_t s = 0; s < scene.anim_stacks.count; ++s) {
        const auto* stack = scene.anim_stacks.data[s];
        ufbx_bake_opts options = {};
        options.trim_start_time = true;
        ufbx_error error;
        auto* baked = ufbx_bake_anim(&scene, stack->anim, &options, &error);
        if (!baked) continue;
        RawClip clip;
        clip.name = std::string(stack->name.data, stack->name.length);
        // Blender names stacks "Armature|Action": the action names the clip, unless repeated.
        if (const auto bar = clip.name.rfind('|'); bar != std::string::npos) {
            const auto action = clip.name.substr(bar + 1);
            bool repeated = false;
            for (std::size_t o = 0; o < scene.anim_stacks.count; ++o) {
                const std::string other(scene.anim_stacks.data[o]->name.data, scene.anim_stacks.data[o]->name.length);
                repeated |= o != s && other.size() > action.size() && other.compare(other.size() - action.size() - 1, std::string::npos, "|" + action) == 0;
            }
            if (!repeated && !action.empty()) clip.name = action;
        }
        if (clip.name.empty()) clip.name = "Clip " + std::to_string(s);
        clip.duration = float(std::max(baked->playback_duration, baked->key_time_max));
        if (!(clip.duration > 0)) clip.duration = 1.0f / 30;
        for (const auto& node : baked->nodes) {
            const int j = out.joint(scene.nodes.data[node.typed_id]);
            if (j < 0) continue;
            auto& track = clip.tracks[out.names[std::size_t(j)]];
            const auto time = [&](double t) { return std::clamp(float(t), 0.0f, clip.duration); };
            float last = -1;
            for (const auto& key : node.translation_keys)
                if (const float t = time(key.time); t > last)
                    track.translations.push_back({last = t, ozz::math::Float3(float(key.value.x), float(key.value.y), float(key.value.z))});
            last = -1;
            for (const auto& key : node.rotation_keys)
                if (const float t = time(key.time); t > last)
                    track.rotations.push_back({last = t, ozz::math::Quaternion(float(key.value.x), float(key.value.y),
                                                                               float(key.value.z), float(key.value.w))});
            last = -1;
            for (const auto& key : node.scale_keys)
                if (const float t = time(key.time); t > last)
                    track.scales.push_back({last = t, ozz::math::Float3(float(key.value.x), float(key.value.y), float(key.value.z))});
        }
        ufbx_free_baked_anim(baked);
        clips.push_back(std::move(clip));
    }
    out.asset = buildSkeleton(raw, clips);
    return out;
}
}
