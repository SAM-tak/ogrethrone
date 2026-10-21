#include "editor/Expansion.h"
#include <algorithm>
#include <optional>
#include <set>

namespace throne {
namespace {
/// Overrides waiting to be applied at @p prefix, innermost first.
struct Pending {
    NodePath prefix;
    Overrides overrides;
    /// Whether they are an instance's own (not a variant's layer inside the instance).
    bool own;
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
    /// Appends the nodes @p overrides adds to the instance at @p prefix; @p own as in Pending.
    void add(const Overrides& overrides, const NodePath& prefix, bool own);
};

void Expander::add(const Overrides& overrides, const NodePath& prefix, bool own) {
    for (const auto& entry : overrides.added) {
        const auto where = join(prefix, entry.parent);
        const auto parent = out.find(where);
        if (parent == Expansion::npos && !where.empty()) {
            out.errors.push_back(entry.node.name + ": the node it was added to is gone");
            if (own && !out.unsure(where)) {
                Overrides piece;
                piece.added.push_back(entry);
                out.unused.push_back({prefix, std::move(piece), entry.node.name + ", added under a node no longer there"});
            }
            continue;
        }
        out.nodes[place(entry.node, join(prefix, {entry.node.id}), parent)].added = true;
    }
    for (const auto& system : overrides.addedSystems) out.systems.push_back({prefix, system, true});
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
    add(node.overrides, path, true);
    // Pushed after the prefab's own instances and variant layers, so those apply first.
    pending.push_back({path, node.overrides, true});
    return at;
}

std::size_t Expander::prefab(const std::string& file, const SceneNode* placed, const NodePath& prefix,
                             std::size_t parent) {
    // What cannot be expanded stays in the hierarchy as a plain node, with an error.
    const auto broken = [&](const std::string& error) {
        out.errors.push_back((placed ? placed->name : file) + ": " + error);
        out.broken.push_back(prefix);
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
        add(content->overrides, prefix, false);  // Still on the stack: an added node may not instantiate the variant.
        stack.pop_back();
        pending.push_back({prefix, content->overrides, false});
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
    for (const auto& system : content->systems) out.systems.push_back({prefix, system});
    return at;
}

/// The names of the properties @p layer sets, separated by commas.
std::string names(const NodeOverride& layer) {
    std::string result;
    forEachProperty([&](const char* name, const auto& value) {
        if (value) result += (result.empty() ? "" : ", ") + std::string(name);
    }, layer);
    return result;
}

/// Applies one override list to an expansion; the nodes it removes join @p gone.
void apply(Expansion& out, const Pending& pending, std::set<std::size_t>& gone) {
    const auto& prefix = pending.prefix;
    const auto& overrides = pending.overrides;
    // Records a part that changes nothing, unless what it names may come back.
    const auto unused = [&](const NodePath& where, Overrides piece, std::string label) {
        if (pending.own && !out.unsure(where)) out.unused.push_back({prefix, std::move(piece), std::move(label)});
    };
    // The expanded path of the instance at @p where (empty: the content), or nullopt.
    const auto instanceAt = [&](const NodePath& where) -> std::optional<NodePath> {
        if (where.empty()) return where;
        const auto at = out.find(where);
        if (at == Expansion::npos) return std::nullopt;
        return out.nodes[at].path;
    };
    for (const auto& path : overrides.removed) {
        const auto where = join(prefix, path);
        if (const auto at = out.find(where); at != Expansion::npos) {
            gone.insert(at);
            out.removed[where] = out.nodes[at].node.name;
        } else {
            Overrides piece;
            piece.removed.push_back(path);
            unused(where, std::move(piece), "Removal of a node no longer there");
        }
    }
    for (const auto& entry : overrides.nodes) {
        const auto where = join(prefix, entry.path);
        const auto at = out.find(where);
        Overrides piece;
        if (at == Expansion::npos) {
            piece.nodes.push_back(entry);
            unused(where, std::move(piece), names(entry) + " of a node no longer there");
            continue;
        }
        auto& node = out.nodes[at].node;
        piece.nodes.push_back(applyOverride(node, entry));
        if (!piece.nodes.back().empty())
            unused(where, std::move(piece), names(piece.nodes.back()) + " of " + node.name + ", which lacks the component");
    }
    for (const auto& entry : overrides.removedSystems) {
        const auto where = join(prefix, entry.instance);
        bool found = false;
        if (const auto instance = instanceAt(where)) {
            auto& systems = out.systems;
            for (auto it = systems.begin(); it != systems.end();) {
                if (it->instance != *instance || it->system.id != entry.system) {
                    ++it;
                    continue;
                }
                out.removedSystems[{where, entry.system}] = it->system.name;
                it = systems.erase(it);
                found = true;
            }
        }
        if (found) continue;
        Overrides piece;
        piece.removedSystems.push_back(entry);
        unused(where, std::move(piece), "Removal of a system no longer there");
    }
    for (const auto& entry : overrides.systems) {
        // The instance is found like a node, so it may have moved deeper since.
        const auto where = join(prefix, entry.instance);
        bool found = false;
        if (const auto instance = instanceAt(where)) {
            for (auto& expanded : out.systems) {
                if (expanded.instance != *instance || expanded.system.id != entry.system) continue;
                if (entry.behaviour >= expanded.system.behaviour.size()) continue;
                for (const auto& [name, value] : entry.fields) expanded.system.behaviour[entry.behaviour].data[name] = value;
                found = true;
            }
        }
        if (found || entry.fields.empty()) continue;
        std::string fields;
        for (const auto& [name, value] : entry.fields) fields += (fields.empty() ? "" : ", ") + name;
        Overrides piece;
        piece.systems.push_back(entry);
        unused(where, std::move(piece), fields + " of a system no longer there");
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

bool Expansion::unsure(const NodePath& path) const {
    return std::any_of(broken.begin(), broken.end(), [&](const NodePath& instance) {
        return instance.size() <= path.size() && std::equal(instance.begin(), instance.end(), path.begin());
    });
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
        expander.add(content.overrides, {}, true);
        expander.pending.push_back({{}, content.overrides, true});
    } else {
        std::map<NodeId, std::size_t> index;
        for (const auto& node : content.nodes)
            index[node.id] = expander.place(node, {node.id}, node.parent ? index.at(node.parent) : Expansion::npos);
        for (const auto& system : content.systems) out.systems.push_back({{}, system});
    }
    std::set<std::size_t> gone;
    for (const auto& entry : expander.pending) apply(out, entry, gone);
    if (gone.empty()) return out;
    // Removed nodes go with everything below them, and the systems of instances among them.
    std::vector<std::size_t> kept(out.nodes.size(), Expansion::npos);
    std::size_t count = 0;
    for (std::size_t i = 0; i < out.nodes.size(); ++i) {
        const auto parent = out.nodes[i].parent;
        if (!gone.count(i) && (parent == Expansion::npos || kept[parent] != Expansion::npos)) kept[i] = count++;
    }
    std::vector<ExpandedSystem> systems;
    for (auto& system : out.systems) {
        const auto at = system.instance.empty() ? Expansion::npos : out.find(system.instance);
        if (system.instance.empty() || (at != Expansion::npos && kept[at] != Expansion::npos)) systems.push_back(std::move(system));
    }
    std::vector<ExpandedNode> nodes;
    for (std::size_t i = 0; i < out.nodes.size(); ++i) {
        if (kept[i] == Expansion::npos) continue;
        auto& node = out.nodes[i];
        if (node.parent != Expansion::npos) node.parent = kept[node.parent];
        nodes.push_back(std::move(node));
    }
    out.nodes = std::move(nodes);
    out.systems = std::move(systems);
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
