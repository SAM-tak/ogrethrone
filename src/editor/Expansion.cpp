#include "editor/Expansion.h"
#include <algorithm>
#include <set>

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
    /// Appends the nodes @p overrides adds to the instance at @p prefix.
    void add(const Overrides& overrides, const NodePath& prefix);
};

void Expander::add(const Overrides& overrides, const NodePath& prefix) {
    for (const auto& entry : overrides.added) {
        const auto where = join(prefix, entry.parent);
        const auto parent = out.find(where);
        if (parent == Expansion::npos && !where.empty()) {
            out.errors.push_back(entry.node.name + ": the node it was added to is gone");
            continue;
        }
        out.nodes[place(entry.node, join(prefix, {entry.node.id}), parent)].added = true;
    }
    for (const auto& runner : overrides.addedRunners) out.runners.push_back({prefix, runner, true});
}

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
    add(node.overrides, path);
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
        add(content->overrides, prefix);  // Still on the stack: an added node may not instantiate the variant.
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
    // The instance gives the root its name and Transform.
    if (placed) {
        merged.name = placed->name;
        if (placed->transform) merged.transform = placed->transform;
    }
    const auto at = out.nodes.size();
    out.nodes.push_back({prefix, parent, merged, root.id});
    stack.push_back(file);
    std::map<NodeId, std::size_t> index{{root.id, at}};
    for (const auto& node : content->nodes)
        if (node.id != root.id) index[node.id] = place(node, join(prefix, {node.id}), index.at(node.parent));
    stack.pop_back();
    for (const auto& runner : content->runners) out.runners.push_back({prefix, runner});
    return at;
}

/// Applies one override list to an expansion; the nodes it removes join @p gone.
void apply(Expansion& out, const NodePath& prefix, const Overrides& overrides, std::set<std::size_t>& gone) {
    for (const auto& path : overrides.removed) {
        const auto where = join(prefix, path);
        if (const auto at = out.find(where); at != Expansion::npos) {
            gone.insert(at);
            out.removed[where] = out.nodes[at].node.name;
        }
    }
    for (const auto& entry : overrides.nodes) {
        const auto at = out.find(join(prefix, entry.path));
        if (at == Expansion::npos) continue;  // The prefab no longer has that node.
        auto& node = out.nodes[at].node;
        applyExtraComponents(node, entry);
        if (entry.name) node.name = *entry.name;
        if (entry.behaviours) node.behaviours = *entry.behaviours;
        for (const auto& [name, fields] : entry.scriptFields)
            if (const auto component = node.scriptComponents.find(name); component != node.scriptComponents.end())
                for (const auto& [field, value] : fields) component->second.fields[field] = value;
        if (auto& transform = node.transform) {
            if (entry.position) transform->position = *entry.position;
            if (entry.rotation) transform->rotation = *entry.rotation;
            if (entry.scale) transform->scale = *entry.scale;
        }
        if (node.box && entry.colour) node.box->colour = *entry.colour;
    }
    for (const auto& entry : overrides.removedRunners) {
        const auto where = join(prefix, entry.instance);
        auto instance = where;
        if (!instance.empty()) {
            const auto at = out.find(instance);
            if (at == Expansion::npos) continue;
            instance = out.nodes[at].path;
        }
        auto& runners = out.runners;
        for (auto it = runners.begin(); it != runners.end();) {
            if (it->instance != instance || it->runner.id != entry.runner) {
                ++it;
                continue;
            }
            out.removedRunners[{where, entry.runner}] = it->runner.name;
            it = runners.erase(it);
        }
    }
    for (const auto& entry : overrides.runners) {
        // The instance is found like a node, so it may have moved deeper since.
        auto instance = join(prefix, entry.instance);
        if (!instance.empty()) {
            const auto at = out.find(instance);
            if (at == Expansion::npos) continue;
            instance = out.nodes[at].path;
        }
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
    // Whether node i lies below node at (npos: the top).
    const auto below = [this](std::size_t i, std::size_t at) {
        while ((i = nodes[i].parent) != npos)
            if (i == at) return true;
        return at == npos;
    };
    // The empty path is a variant's root when the variant itself is expanded.
    auto at = npos;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].path.empty()) at = i;
    for (const auto id : path) {
        if (at != npos && nodes[at].root == id) continue;  // The merged root named by its own ID.
        auto next = npos;
        for (std::size_t i = at == npos ? 0 : at + 1; i < nodes.size() && next == npos; ++i)
            if (nodes[i].node.id == id && below(i, at)) next = i;
        if (next == npos) return npos;
        at = next;
    }
    return at;
}

std::vector<std::size_t> Expansion::hierarchy() const {
    std::vector<std::vector<std::size_t>> children(nodes.size() + 1);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        children[nodes[i].parent == npos ? nodes.size() : nodes[i].parent].push_back(i);
    std::vector<std::size_t> result, stack;
    stack.assign(children.back().rbegin(), children.back().rend());
    while (!stack.empty()) {
        const auto i = stack.back();
        stack.pop_back();
        result.push_back(i);
        stack.insert(stack.end(), children[i].rbegin(), children[i].rend());
    }
    return result;
}

Expansion expand(const Prefab& content, Library& library, const std::string& self) {
    Expansion out;
    Expander expander{library, {}, out, {}};
    if (!self.empty()) expander.stack.push_back(self);
    if (content.variant()) {
        expander.prefab(content.base, nullptr, {}, Expansion::npos);
        expander.add(content.overrides, {});
        expander.pending.push_back({{}, content.overrides});
    } else {
        std::map<NodeId, std::size_t> index;
        for (const auto& node : content.nodes)
            index[node.id] = expander.place(node, {node.id}, node.parent ? index.at(node.parent) : Expansion::npos);
        for (const auto& runner : content.runners) out.runners.push_back({{}, runner});
    }
    std::set<std::size_t> gone;
    for (const auto& entry : expander.pending) apply(out, entry.prefix, entry.overrides, gone);
    if (gone.empty()) return out;
    // Removed nodes go with everything below them, and the runners of instances among them.
    std::vector<std::size_t> kept(out.nodes.size(), Expansion::npos);
    std::size_t count = 0;
    for (std::size_t i = 0; i < out.nodes.size(); ++i) {
        const auto parent = out.nodes[i].parent;
        if (!gone.count(i) && (parent == Expansion::npos || kept[parent] != Expansion::npos)) kept[i] = count++;
    }
    std::vector<ExpandedRunner> runners;
    for (auto& runner : out.runners) {
        const auto at = runner.instance.empty() ? Expansion::npos : out.find(runner.instance);
        if (runner.instance.empty() || (at != Expansion::npos && kept[at] != Expansion::npos)) runners.push_back(std::move(runner));
    }
    std::vector<ExpandedNode> nodes;
    for (std::size_t i = 0; i < out.nodes.size(); ++i) {
        if (kept[i] == Expansion::npos) continue;
        auto& node = out.nodes[i];
        if (node.parent != Expansion::npos) node.parent = kept[node.parent];
        nodes.push_back(std::move(node));
    }
    out.nodes = std::move(nodes);
    out.runners = std::move(runners);
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
            {instance.id, expand(scene->second, library, instance.scene), instance.transform});
    }
    return result;
}
}
