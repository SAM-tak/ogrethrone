#include "editor/Expansion.h"
#include <algorithm>

namespace throne {
namespace {
/// An instance node whose overrides wait to be applied, by index in Expansion::nodes.
struct Pending {
    NodePath prefix;
    std::size_t node;
};

/// Returns @p prefix followed by @p rest.
NodePath join(const NodePath& prefix, const NodePath& rest) {
    NodePath result = prefix;
    result.insert(result.end(), rest.begin(), rest.end());
    return result;
}

/// Appends @p content's nodes and runners under @p prefix, expanding nested instances.
void expandInto(const Prefab& content, const NodePath& prefix, std::size_t parent, Library& library,
                std::vector<std::string>& stack, Expansion& out, std::vector<Pending>& pending) {
    std::map<std::uint32_t, std::size_t> index;
    for (const auto& node : content.nodes) {
        const auto path = join(prefix, {node.id});
        const auto at = out.nodes.size();
        out.nodes.push_back({path, node.parent ? index.at(node.parent) : parent, node});
        index[node.id] = at;
        if (node.prefab.empty()) continue;
        if (std::find(stack.begin(), stack.end(), node.prefab) != stack.end()) {
            out.errors.push_back(node.name + ": " + node.prefab + " instantiates itself");
            continue;
        }
        try {
            const auto& prefab = library.prefab(node.prefab);
            stack.push_back(node.prefab);
            expandInto(prefab, path, at, library, stack, out, pending);
            stack.pop_back();
        } catch (const std::exception& error) {
            out.errors.push_back(node.name + ": " + error.what());
            continue;
        }
        // Pushed after its nested instances, so inner overrides apply first.
        pending.push_back({path, at});
    }
    for (const auto& runner : content.runners) out.runners.push_back({prefix, runner});
}

/// Applies one override list to an expansion.
void apply(Expansion& out, const NodePath& prefix, const Overrides& overrides) {
    for (const auto& entry : overrides.nodes) {
        const auto at = out.find(join(prefix, entry.path));
        if (at == Expansion::npos) continue;  // The prefab no longer has that node.
        auto& node = out.nodes[at].node;
        if (entry.name) node.name = *entry.name;
        if (entry.position) node.position = *entry.position;
        if (entry.rotation) node.rotation = *entry.rotation;
        if (entry.scale) node.scale = *entry.scale;
        if (entry.colour) node.colour = *entry.colour;
    }
    for (const auto& entry : overrides.runners) {
        const auto instance = join(prefix, entry.instance);
        for (auto& expanded : out.runners) {
            if (expanded.instance != instance || expanded.runner.id != entry.runner) continue;
            if (entry.behaviour >= expanded.runner.behaviours.size()) continue;
            for (const auto& [name, value] : entry.fields) expanded.runner.behaviours[entry.behaviour].fields[name] = value;
        }
    }
}
}

Library::Library(std::filesystem::path root) : root_(std::move(root)) {}

const Prefab& Library::prefab(const std::string& path) {
    auto& entry = entries_[path];
    if (entry.pinned) return entry.prefab;
    const auto file = root_ / std::filesystem::u8path(path);
    const auto stamp = std::filesystem::last_write_time(file);
    if (entry.stamp != stamp) {
        entry.prefab = Prefab::load(file);
        entry.stamp = stamp;
    }
    return entry.prefab;
}

void Library::put(const std::string& path, Prefab prefab) { entries_[path] = {std::move(prefab), {}, true}; }

void Library::forget(const std::string& path) { entries_.erase(path); }

std::size_t Expansion::find(const NodePath& path) const {
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].path == path) return i;
    return npos;
}

Expansion expand(const Prefab& content, Library& library, const std::string& self, const Overrides* outer) {
    Expansion out;
    std::vector<std::string> stack;
    if (!self.empty()) stack.push_back(self);
    std::vector<Pending> pending;
    expandInto(content, {}, Expansion::npos, library, stack, out, pending);
    // apply() changes node values but never an instance's overrides, so they can be read in place.
    for (const auto& entry : pending) apply(out, entry.prefix, out.nodes[entry.node].node.overrides);
    if (outer) apply(out, {}, *outer);
    return out;
}

Expansion* LevelExpansion::find(std::uint32_t id) {
    for (auto& instance : instances)
        if (instance.id == id) return &instance.expansion;
    return nullptr;
}

LevelExpansion expand(const World& world, Library& library) {
    LevelExpansion result;
    for (const auto& instance : world.level.scenes) {
        const auto scene = world.scenes.find(instance.scene);
        if (scene == world.scenes.end()) {
            Expansion missing;
            missing.errors.push_back(instance.scene + " is not loaded");
            result.instances.push_back({instance.id, std::move(missing)});
            continue;
        }
        result.instances.push_back({instance.id, expand(scene->second, library, instance.scene, &instance.overrides)});
    }
    return result;
}
}
