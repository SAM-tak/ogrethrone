#include "editor/Expansion.h"
#include <algorithm>

namespace throne {
namespace {
/// Overrides waiting to be applied at @p prefix, innermost first.
struct Pending {
    NodePath prefix;
    Overrides overrides;
};

/// Returns @p prefix followed by @p rest.
NodePath join(const NodePath& prefix, const NodePath& rest) {
    NodePath result = prefix;
    result.insert(result.end(), rest.begin(), rest.end());
    return result;
}

/// State of one expand() call.
struct Expander {
    Library& library;
    /// Prefabs being expanded, outermost first, for cycle checks.
    std::vector<std::string> stack;
    Expansion& out;
    std::vector<Pending> pending;

    /// Appends @p node at @p path; an instance becomes its prefab's root. Returns its index.
    std::size_t place(const SceneNode& node, const NodePath& path, std::size_t parent);
    /// Expands prefab @p file at @p prefix. Its root takes @p placed's name and
    /// transform, or keeps its own when @p placed is null. Returns the root's
    /// index, or npos when nothing could be placed.
    std::size_t prefab(const std::string& file, const SceneNode* placed, const NodePath& prefix, std::size_t parent);
};

std::size_t Expander::place(const SceneNode& node, const NodePath& path, std::size_t parent) {
    if (node.prefab.empty()) {
        out.nodes.push_back({path, parent, node});
        return out.nodes.size() - 1;
    }
    const auto at = prefab(node.prefab, &node, path, parent);
    auto& merged = out.nodes[at].node;
    merged.id = node.id;
    merged.parent = node.parent;
    merged.prefab = node.prefab;
    merged.overrides = node.overrides;
    // Pushed after the prefab's own instances and variant layers, so those apply first.
    pending.push_back({path, node.overrides});
    return at;
}

std::size_t Expander::prefab(const std::string& file, const SceneNode* placed, const NodePath& prefix,
                             std::size_t parent) {
    // What cannot be expanded stays in the hierarchy as a plain node, with an error.
    const auto broken = [&](const std::string& error) {
        out.errors.push_back((placed ? placed->name : file) + ": " + error);
        if (!placed) return Expansion::npos;
        out.nodes.push_back({prefix, parent, *placed});
        return out.nodes.size() - 1;
    };
    if (std::find(stack.begin(), stack.end(), file) != stack.end()) return broken(file + " instantiates itself");
    const Prefab* content;
    try {
        content = &library.prefab(file);
    } catch (const std::exception& error) {
        return broken(error.what());
    }
    if (content->variant()) {
        stack.push_back(file);
        const auto at = prefab(content->base, placed, prefix, parent);
        stack.pop_back();
        pending.push_back({prefix, content->overrides});
        return at;
    }
    const auto roots = std::count_if(content->nodes.begin(), content->nodes.end(),
                                     [](const SceneNode& node) { return node.parent == 0; });
    if (roots != 1) return broken(file + " must have exactly one root node");
    const auto& root = content->nodes.front();
    if (!root.prefab.empty()) return broken(file + ": its root cannot be an instance; make a variant instead");
    auto merged = root;
    if (placed) {
        merged.name = placed->name;
        merged.position = placed->position;
        merged.rotation = placed->rotation;
        merged.scale = placed->scale;
    }
    const auto at = out.nodes.size();
    out.nodes.push_back({prefix, parent, merged, root.id});
    stack.push_back(file);
    std::map<std::uint32_t, std::size_t> index{{root.id, at}};
    for (const auto& node : content->nodes)
        if (node.id != root.id) index[node.id] = place(node, join(prefix, {node.id}), index.at(node.parent));
    stack.pop_back();
    for (const auto& runner : content->runners) out.runners.push_back({prefix, runner});
    return at;
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
    const auto exact = [this](const NodePath& wanted) {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].path == wanted) return i;
        return npos;
    };
    // The empty path is a variant's root when the variant itself is expanded.
    auto at = exact({});
    NodePath canonical;
    for (const auto id : path) {
        canonical.push_back(id);
        if (const auto next = exact(canonical); next != npos) {
            at = next;
        } else if (at != npos && nodes[at].root == id) {
            canonical.pop_back();  // The merged root named by its own ID.
        } else {
            return npos;
        }
    }
    return at;
}

Expansion expand(const Prefab& content, Library& library, const std::string& self, const Overrides* outer) {
    Expansion out;
    Expander expander{library, {}, out, {}};
    if (!self.empty()) expander.stack.push_back(self);
    if (content.variant()) {
        expander.prefab(content.base, nullptr, {}, Expansion::npos);
        expander.pending.push_back({{}, content.overrides});
    } else {
        std::map<std::uint32_t, std::size_t> index;
        for (const auto& node : content.nodes)
            index[node.id] = expander.place(node, {node.id}, node.parent ? index.at(node.parent) : Expansion::npos);
        for (const auto& runner : content.runners) out.runners.push_back({{}, runner});
    }
    for (const auto& entry : expander.pending) apply(out, entry.prefix, entry.overrides);
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
            result.instances.push_back({instance.id, std::move(missing), instance.transform});
            continue;
        }
        result.instances.push_back(
            {instance.id, expand(scene->second, library, instance.scene, &instance.overrides), instance.transform});
    }
    return result;
}
}
