#include "editor/Scene.h"
#include <algorithm>
#include <limits>
#include <random>
#include <set>

namespace throne {
namespace {
constexpr std::string_view prefabFormat = "ogrethrone-prefab", sceneFormat = "ogrethrone-scene";
constexpr LhatInteger prefabVersion = 2, sceneVersion = 1;

/// Reads a name: 1 to 255 bytes without NUL.
std::string name(LhatValue value) {
    std::string result(lton::string(value));
    if (result.empty() || result.size() > 255 || result.find('\0') != std::string::npos) lton::invalid();
    return result;
}

/// Reads a node ID: a positive integer.
NodeId nodeId(LhatValue value) {
    const auto id = lton::integer(value);
    if (id <= 0) lton::invalid();
    return static_cast<NodeId>(id);
}

/// Reads an ID: a positive integer below UINT32_MAX.
std::uint32_t id(LhatValue value) {
    const auto id = lton::integer(value);
    if (id <= 0 || id >= std::numeric_limits<std::uint32_t>::max()) lton::invalid();
    return static_cast<std::uint32_t>(id);
}

/// Whether @p id is @p ancestor or lies below it, looking parents up in @p nodes.
bool within(const std::vector<SceneNode>& nodes, NodeId id, NodeId ancestor) {
    while (id != 0) {
        if (id == ancestor) return true;
        const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const SceneNode& node) { return node.id == id; });
        if (it == nodes.end()) return false;
        id = it->parent;
    }
    return false;
}

/// Writes a path as a sequence of IDs.
LhatValue writePath(lton::Writer& out, const NodePath& path) {
    const auto list = out.table();
    for (const auto step : path) out.push(list, lhat_integer(static_cast<LhatInteger>(step)));
    return list;
}

/// Reads what writePath wrote; nil^ reads as an empty path.
NodePath readPath(LhatValue value) {
    NodePath path;
    if (lhat_is_nil(value)) return path;
    const auto& list = lton::table(value);
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) path.push_back(nodeId(lton::at(list, i)));
    return path;
}

/// Writes one field value: a number, bool, string, or `{ scene = ..., node = ..., inner = {...} }`.
LhatValue writeField(lton::Writer& out, const FieldValue& value) {
    if (const auto* number = std::get_if<double>(&value)) return lhat_real(*number);
    if (const auto* flag = std::get_if<bool>(&value)) return lhat_bool(*flag);
    if (const auto* text = std::get_if<std::string>(&value)) return out.string(*text);
    const auto& ref = std::get<NodeRef>(value);
    const auto table = out.table();
    if (ref.scene) out.set(table, "scene", lhat_integer(ref.scene));
    out.set(table, "node", lhat_integer(static_cast<LhatInteger>(ref.node)));
    if (!ref.inner.empty()) out.set(table, "inner", writePath(out, ref.inner));
    return table;
}

/// Reads what writeField wrote.
FieldValue readField(LhatValue value) {
    if (lhat_is_bool(value)) return lhat_as_bool(value);
    if (lhat_is_integer(value) || lhat_is_real(value)) return lton::number(value);
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) return std::string(lton::string(value));
    const auto& table = lton::table(value);
    NodeRef ref;
    if (const auto scene = lton::field(table, "scene"); !lhat_is_nil(scene)) ref.scene = id(scene);
    ref.node = nodeId(lton::field(table, "node"));
    ref.inner = readPath(lton::field(table, "inner"));
    return ref;
}

/// Writes a field map as a table keyed by field name.
LhatValue writeFields(lton::Writer& out, const std::map<std::string, FieldValue>& fields) {
    const auto table = out.table();
    for (const auto& [key, value] : fields) out.set(table, key, writeField(out, value));
    return table;
}

/// Reads what writeFields wrote; nil^ reads as no fields.
std::map<std::string, FieldValue> readFields(LhatValue value) {
    std::map<std::string, FieldValue> fields;
    if (lhat_is_nil(value)) return fields;
    LhatWalkCursor walk{&lton::table(value), 0, 0, LHAT_WALK_PAIR};
    LhatValue key, field;
    while (lhat_table_walk(&walk, &key, &field)) fields[std::string(lton::string(key))] = readField(field);
    return fields;
}

/// Reads one node table (its `parent` too); an instance's overrides come along.
SceneNode readNode(const LhatTable& entry) {
    SceneNode node;
    node.id = nodeId(lton::field(entry, "id"));
    node.name = name(lton::field(entry, "name"));
    const auto parent = lton::field(entry, "parent");
    node.parent = lton::integer(parent) == 0 ? 0 : nodeId(parent);
    lton::vector(lton::field(entry, "position"), node.position.data());
    lton::vector(lton::field(entry, "rotation"), node.rotation.data());
    lton::vector(lton::field(entry, "scale"), node.scale.data());
    lton::vector(lton::field(entry, "colour"), node.colour.data());
    for (unsigned axis = 0; axis < 3; ++axis)
        if (node.scale.data()[axis] < 0.001f || node.colour[axis] < 0 || node.colour[axis] > 1) lton::invalid();
    if (const auto prefab = lton::field(entry, "prefab"); !lhat_is_nil(prefab)) {
        node.prefab = lton::relativePath(prefab);
        node.overrides = readOverrides(lton::field(entry, "overrides"));
    }
    return node;
}

/// Writes what readNode reads.
LhatValue writeNode(lton::Writer& out, const SceneNode& node) {
    const auto entry = out.table();
    out.set(entry, "id", lhat_integer(static_cast<LhatInteger>(node.id)));
    out.set(entry, "name", out.string(node.name));
    out.set(entry, "parent", lhat_integer(static_cast<LhatInteger>(node.parent)));
    out.set(entry, "position", out.vector(node.position.data()));
    out.set(entry, "rotation", out.vector(node.rotation.data()));
    out.set(entry, "scale", out.vector(node.scale.data()));
    out.set(entry, "colour", out.vector(node.colour.data()));
    if (!node.prefab.empty()) {
        out.set(entry, "prefab", out.string(node.prefab));
        if (!node.overrides.empty()) out.set(entry, "overrides", writeOverrides(out, node.overrides));
    }
    return entry;
}

/// Reads an optional three-component vector field into @p out.
template <typename T>
void readOptional(const LhatTable& table, std::string_view key, std::optional<T>& out) {
    if (const auto value = lton::field(table, key); !lhat_is_nil(value)) {
        out.emplace();
        lton::vector(value, out->data());
    }
}
}

bool Overrides::empty() const { return nodes.empty() && runners.empty() && added.empty() && removed.empty(); }

NodeOverride& Overrides::node(const NodePath& path) {
    for (auto& entry : nodes)
        if (entry.path == path) return entry;
    nodes.push_back({path});
    return nodes.back();
}

FieldOverride& Overrides::fieldsOf(const NodePath& instance, std::uint32_t runner, std::size_t behaviour) {
    for (auto& entry : runners)
        if (entry.instance == instance && entry.runner == runner && entry.behaviour == behaviour) return entry;
    runners.push_back({instance, runner, behaviour, {}});
    return runners.back();
}

LhatValue writeOverrides(lton::Writer& out, const Overrides& overrides) {
    const auto root = out.table();
    const auto nodes = out.table();
    for (const auto& entry : overrides.nodes) {
        const auto item = out.table();
        out.set(item, "path", writePath(out, entry.path));
        if (entry.name) out.set(item, "name", out.string(*entry.name));
        if (entry.position) out.set(item, "position", out.vector(entry.position->data()));
        if (entry.rotation) out.set(item, "rotation", out.vector(entry.rotation->data()));
        if (entry.scale) out.set(item, "scale", out.vector(entry.scale->data()));
        if (entry.colour) out.set(item, "colour", out.vector(entry.colour->data()));
        out.push(nodes, item);
    }
    out.set(root, "nodes", nodes);
    const auto runners = out.table();
    for (const auto& entry : overrides.runners) {
        const auto item = out.table();
        if (!entry.instance.empty()) out.set(item, "instance", writePath(out, entry.instance));
        out.set(item, "runner", lhat_integer(entry.runner));
        out.set(item, "behaviour", lhat_integer(static_cast<LhatInteger>(entry.behaviour)));
        out.set(item, "fields", writeFields(out, entry.fields));
        out.push(runners, item);
    }
    out.set(root, "runners", runners);
    if (!overrides.added.empty()) {
        const auto added = out.table();
        for (const auto& entry : overrides.added) {
            const auto item = out.table();
            out.set(item, "parent", writePath(out, entry.parent));
            out.set(item, "node", writeNode(out, entry.node));
            out.push(added, item);
        }
        out.set(root, "added", added);
    }
    if (!overrides.removed.empty()) {
        const auto removed = out.table();
        for (const auto& path : overrides.removed) out.push(removed, writePath(out, path));
        out.set(root, "removed", removed);
    }
    return root;
}

Overrides readOverrides(LhatValue value) {
    Overrides overrides;
    if (lhat_is_nil(value)) return overrides;
    const auto& root = lton::table(value);
    if (const auto nodes = lton::field(root, "nodes"); !lhat_is_nil(nodes)) {
        const auto& list = lton::table(nodes);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            NodeOverride entry;
            entry.path = readPath(lton::field(item, "path"));
            if (const auto text = lton::field(item, "name"); !lhat_is_nil(text)) entry.name = name(text);
            readOptional(item, "position", entry.position);
            readOptional(item, "rotation", entry.rotation);
            readOptional(item, "scale", entry.scale);
            readOptional(item, "colour", entry.colour);
            overrides.nodes.push_back(std::move(entry));
        }
    }
    if (const auto runners = lton::field(root, "runners"); !lhat_is_nil(runners)) {
        const auto& list = lton::table(runners);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            FieldOverride entry;
            entry.instance = readPath(lton::field(item, "instance"));
            entry.runner = id(lton::field(item, "runner"));
            const auto behaviour = lton::integer(lton::field(item, "behaviour"));
            if (behaviour < 0) lton::invalid();
            entry.behaviour = static_cast<std::size_t>(behaviour);
            entry.fields = readFields(lton::field(item, "fields"));
            overrides.runners.push_back(std::move(entry));
        }
    }
    if (const auto added = lton::field(root, "added"); !lhat_is_nil(added)) {
        const auto& list = lton::table(added);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            const auto& item = lton::table(lton::at(list, i));
            AddedNode entry{readPath(lton::field(item, "parent")), readNode(lton::table(lton::field(item, "node")))};
            entry.node.parent = 0;
            overrides.added.push_back(std::move(entry));
        }
    }
    if (const auto removed = lton::field(root, "removed"); !lhat_is_nil(removed)) {
        const auto& list = lton::table(removed);
        for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
            overrides.removed.push_back(readPath(lton::at(list, i)));
            if (overrides.removed.back().empty()) lton::invalid();
        }
    }
    return overrides;
}

NodeId newNodeId() {
    static std::mt19937_64 generator(std::random_device{}());
    // Never below 32 bits, so a generated ID cannot pass for a small counter value.
    NodeId id;
    do id = generator() >> 1;
    while (id < 0xFFFFFFFFu);
    return id;
}

SceneNode* Prefab::findNode(NodeId id) {
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const SceneNode& node) { return node.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}

const SceneNode* Prefab::findNode(NodeId id) const { return const_cast<Prefab*>(this)->findNode(id); }

Runner* RunnerSet::findRunner(std::uint32_t id) {
    const auto it = std::find_if(runners.begin(), runners.end(), [id](const Runner& runner) { return runner.id == id; });
    return it == runners.end() ? nullptr : &*it;
}

SceneNode& Prefab::addNode(const std::string& name, NodeId parent) {
    SceneNode node;
    do node.id = newNodeId();
    while (findNode(node.id));
    node.name = name;
    node.position = {0, 0.5f, 0};
    nodes.push_back(node);
    if (parent) reparent(node.id, parent);
    return *findNode(node.id);
}

Runner& RunnerSet::addRunner(const std::string& name) {
    if (nextRunnerId == std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Runner ID limit reached");
    runners.push_back({nextRunnerId++, name, {}});
    return runners.back();
}

void Prefab::removeNode(NodeId id) {
    const auto all = nodes;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const SceneNode& node) { return within(all, node.id, id); }),
                nodes.end());
}

void RunnerSet::removeRunner(std::uint32_t id) {
    runners.erase(std::remove_if(runners.begin(), runners.end(), [id](const Runner& runner) { return runner.id == id; }),
                  runners.end());
}

bool Prefab::isWithin(NodeId id, NodeId ancestor) const { return within(nodes, id, ancestor); }

bool Prefab::reparent(NodeId id, NodeId parent, NodeId before) {
    if (!findNode(id) || (parent && (!findNode(parent) || isWithin(parent, id)))) return false;
    if (before && (before == id || !findNode(before) || findNode(before)->parent != parent)) return false;
    // Keep parents before children: lift the subtree out, then put it before the
    // sibling, or after the new parent's last descendant (at the end for the root).
    std::vector<SceneNode> moved, kept;
    for (const auto& node : nodes) (isWithin(node.id, id) ? moved : kept).push_back(node);
    moved.front().parent = parent;
    auto at = kept.end();
    if (before) {
        at = std::find_if(kept.begin(), kept.end(), [before](const SceneNode& node) { return node.id == before; });
    } else if (parent) {
        for (auto it = kept.begin(); it != kept.end(); ++it)
            if (within(kept, it->id, parent)) at = it + 1;
    }
    kept.insert(at, moved.begin(), moved.end());
    nodes = std::move(kept);
    return true;
}

void Prefab::read(const LhatTable& root) {
    const auto& list = lton::table(lton::field(root, "nodes"));
    std::set<NodeId> seen;
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        auto node = readNode(lton::table(lton::at(list, i)));
        // A parent written earlier is what keeps the order and rules out cycles.
        if ((node.parent && !seen.count(node.parent)) || !seen.insert(node.id).second) lton::invalid();
        nodes.push_back(std::move(node));
    }
    runners = readRunners(lton::field(root, "runners"), nextRunnerId);
}

void Prefab::write(lton::Writer& out, LhatValue root) const {
    const auto list = out.table();
    for (const auto& node : nodes) out.push(list, writeNode(out, node));
    out.set(root, "nodes", list);
    out.set(root, "runners", writeRunners(out, runners));
}

Prefab Prefab::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(prefabFormat, prefabVersion);
    Prefab result;
    result.read(document.root());
    if (const auto base = lton::field(document.root(), "base"); !lhat_is_nil(base)) {
        result.base = lton::relativePath(base);
        result.overrides = readOverrides(lton::field(document.root(), "overrides"));
    }
    return result;
}

void Prefab::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(prefabFormat));
    out.set(root, "version", lhat_integer(prefabVersion));
    write(out, root);
    if (variant()) {
        out.set(root, "base", out.string(base));
        out.set(root, "overrides", writeOverrides(out, overrides));
    }
    out.save(root, path);
}

Scene Scene::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(sceneFormat, sceneVersion);
    Scene result;
    result.read(document.root());
    return result;
}

void Scene::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(sceneFormat));
    out.set(root, "version", lhat_integer(sceneVersion));
    write(out, root);
    out.save(root, path);
}

Scene Scene::example() {
    Scene scene;
    scene.addNode("SpinningCube");
    auto& second = scene.addNode("Cube 2");
    second.position = {2, 0.75f, -1};
    second.scale = {1, 1.5f, 1};
    second.colour = {0.86f, 0.53f, 0.25f};
    return scene;
}

LhatValue writeRunners(lton::Writer& out, const std::vector<Runner>& runners) {
    const auto list = out.table();
    for (const auto& runner : runners) {
        const auto entry = out.table();
        out.set(entry, "id", lhat_integer(runner.id));
        out.set(entry, "name", out.string(runner.name));
        const auto behaviours = out.table();
        for (const auto& behaviour : runner.behaviours) {
            const auto item = out.table();
            out.set(item, "script", out.string(behaviour.script));
            if (!behaviour.fields.empty()) out.set(item, "fields", writeFields(out, behaviour.fields));
            out.push(behaviours, item);
        }
        out.set(entry, "behaviours", behaviours);
        out.push(list, entry);
    }
    return list;
}

std::vector<Runner> readRunners(LhatValue value, std::uint32_t& nextId) {
    std::vector<Runner> result;
    if (lhat_is_nil(value)) return result;
    const auto& list = lton::table(value);
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        const auto& entry = lton::table(lton::at(list, i));
        Runner runner;
        runner.id = id(lton::field(entry, "id"));
        if (!seen.insert(runner.id).second) lton::invalid();
        runner.name = name(lton::field(entry, "name"));
        const auto& behaviours = lton::table(lton::field(entry, "behaviours"));
        for (std::size_t j = 0, n = lton::sequence(behaviours); j < n; ++j) {
            const auto& item = lton::table(lton::at(behaviours, j));
            Behaviour behaviour;
            behaviour.script = lton::relativePath(lton::field(item, "script"));
            behaviour.fields = readFields(lton::field(item, "fields"));
            runner.behaviours.push_back(std::move(behaviour));
        }
        nextId = std::max(nextId, runner.id + 1);
        result.push_back(std::move(runner));
    }
    return result;
}
}
