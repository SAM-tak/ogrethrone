#include "editor/Scene.h"
#include <algorithm>
#include <limits>
#include <set>

namespace throne {
namespace {
constexpr std::string_view prefabFormat = "ogrethrone-prefab", sceneFormat = "ogrethrone-scene";
constexpr LhatInteger prefabVersion = 1, sceneVersion = 1;

/// Reads a name: 1 to 255 bytes without NUL.
std::string name(LhatValue value) {
    std::string result(lton::string(value));
    if (result.empty() || result.size() > 255 || result.find('\0') != std::string::npos) lton::invalid();
    return result;
}

/// Reads an ID: a positive integer below UINT32_MAX.
std::uint32_t id(LhatValue value) {
    const auto id = lton::integer(value);
    if (id <= 0 || id >= std::numeric_limits<std::uint32_t>::max()) lton::invalid();
    return static_cast<std::uint32_t>(id);
}

/// Whether @p id is @p ancestor or lies below it, looking parents up in @p nodes.
bool within(const std::vector<SceneNode>& nodes, std::uint32_t id, std::uint32_t ancestor) {
    while (id != 0) {
        if (id == ancestor) return true;
        const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const SceneNode& node) { return node.id == id; });
        if (it == nodes.end()) return false;
        id = it->parent;
    }
    return false;
}

/// Writes one field value: a number, bool, string, or `{ scene = ..., node = ... }`.
LhatValue writeField(lton::Writer& out, const FieldValue& value) {
    if (const auto* number = std::get_if<double>(&value)) return lhat_real(*number);
    if (const auto* flag = std::get_if<bool>(&value)) return lhat_bool(*flag);
    if (const auto* text = std::get_if<std::string>(&value)) return out.string(*text);
    const auto& ref = std::get<NodeRef>(value);
    const auto table = out.table();
    if (!ref.scene.empty()) out.set(table, "scene", out.string(ref.scene));
    out.set(table, "node", lhat_integer(ref.node));
    return table;
}

/// Reads what writeField wrote.
FieldValue readField(LhatValue value) {
    if (lhat_is_bool(value)) return lhat_as_bool(value);
    if (lhat_is_integer(value) || lhat_is_real(value)) return lton::number(value);
    if (lhat_is_object_kind(value, LHAT_OBJECT_STRING)) return std::string(lton::string(value));
    const auto& table = lton::table(value);
    NodeRef ref;
    if (const auto scene = lton::field(table, "scene"); !lhat_is_nil(scene)) ref.scene = lton::relativePath(scene);
    ref.node = id(lton::field(table, "node"));
    return ref;
}
}

SceneNode* Prefab::findNode(std::uint32_t id) {
    const auto it = std::find_if(nodes.begin(), nodes.end(), [id](const SceneNode& node) { return node.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}

const SceneNode* Prefab::findNode(std::uint32_t id) const { return const_cast<Prefab*>(this)->findNode(id); }

Runner* RunnerSet::findRunner(std::uint32_t id) {
    const auto it = std::find_if(runners.begin(), runners.end(), [id](const Runner& runner) { return runner.id == id; });
    return it == runners.end() ? nullptr : &*it;
}

SceneNode& Prefab::addNode(const std::string& name, std::uint32_t parent) {
    if (nextNodeId == std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("Node ID limit reached");
    SceneNode node;
    node.id = nextNodeId++;
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

void Prefab::removeNode(std::uint32_t id) {
    const auto all = nodes;
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const SceneNode& node) { return within(all, node.id, id); }),
                nodes.end());
}

void RunnerSet::removeRunner(std::uint32_t id) {
    runners.erase(std::remove_if(runners.begin(), runners.end(), [id](const Runner& runner) { return runner.id == id; }),
                  runners.end());
}

bool Prefab::isWithin(std::uint32_t id, std::uint32_t ancestor) const { return within(nodes, id, ancestor); }

bool Prefab::reparent(std::uint32_t id, std::uint32_t parent) {
    if (!findNode(id) || (parent && (!findNode(parent) || isWithin(parent, id)))) return false;
    // Keep parents before children: lift the subtree out, then put it after the
    // new parent's last descendant (or at the end for the root).
    std::vector<SceneNode> moved, kept;
    for (const auto& node : nodes) (isWithin(node.id, id) ? moved : kept).push_back(node);
    moved.front().parent = parent;
    auto at = kept.end();
    if (parent) {
        for (auto it = kept.begin(); it != kept.end(); ++it)
            if (within(kept, it->id, parent)) at = it + 1;
    }
    kept.insert(at, moved.begin(), moved.end());
    nodes = std::move(kept);
    return true;
}

void Prefab::read(const LhatTable& root) {
    const auto& list = lton::table(lton::field(root, "nodes"));
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0, count = lton::sequence(list); i < count; ++i) {
        const auto& entry = lton::table(lton::at(list, i));
        SceneNode node;
        node.id = id(lton::field(entry, "id"));
        node.name = name(lton::field(entry, "name"));
        node.parent = static_cast<std::uint32_t>(lton::integer(lton::field(entry, "parent")));
        // A parent written earlier is what keeps the order and rules out cycles.
        if ((node.parent && !seen.count(node.parent)) || !seen.insert(node.id).second) lton::invalid();
        lton::vector(lton::field(entry, "position"), node.position.data());
        lton::vector(lton::field(entry, "rotation"), node.rotation.data());
        lton::vector(lton::field(entry, "scale"), node.scale.data());
        lton::vector(lton::field(entry, "colour"), node.colour.data());
        for (unsigned axis = 0; axis < 3; ++axis)
            if (node.scale.data()[axis] < 0.001f || node.colour[axis] < 0 || node.colour[axis] > 1) lton::invalid();
        nextNodeId = std::max(nextNodeId, node.id + 1);
        nodes.push_back(std::move(node));
    }
    runners = readRunners(lton::field(root, "runners"), nextRunnerId);
}

void Prefab::write(lton::Writer& out, LhatValue root) const {
    const auto list = out.table();
    for (const auto& node : nodes) {
        const auto entry = out.table();
        out.set(entry, "id", lhat_integer(node.id));
        out.set(entry, "name", out.string(node.name));
        out.set(entry, "parent", lhat_integer(node.parent));
        out.set(entry, "position", out.vector(node.position.data()));
        out.set(entry, "rotation", out.vector(node.rotation.data()));
        out.set(entry, "scale", out.vector(node.scale.data()));
        out.set(entry, "colour", out.vector(node.colour.data()));
        out.push(list, entry);
    }
    out.set(root, "nodes", list);
    out.set(root, "runners", writeRunners(out, runners));
}

Prefab Prefab::load(const std::filesystem::path& path) {
    const lton::Document document(path);
    document.expect(prefabFormat, prefabVersion);
    Prefab result;
    result.read(document.root());
    return result;
}

void Prefab::save(const std::filesystem::path& path) const {
    lton::Writer out;
    const auto root = out.table();
    out.set(root, "format", out.string(prefabFormat));
    out.set(root, "version", lhat_integer(prefabVersion));
    write(out, root);
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
            if (!behaviour.fields.empty()) {
                const auto fields = out.table();
                for (const auto& [key, value] : behaviour.fields) out.set(fields, key, writeField(out, value));
                out.set(item, "fields", fields);
            }
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
            if (const auto fields = lton::field(item, "fields"); !lhat_is_nil(fields)) {
                LhatWalkCursor walk{&lton::table(fields), 0, 0, LHAT_WALK_PAIR};
                LhatValue key, field;
                while (lhat_table_walk(&walk, &key, &field))
                    behaviour.fields[std::string(lton::string(key))] = readField(field);
            }
            runner.behaviours.push_back(std::move(behaviour));
        }
        nextId = std::max(nextId, runner.id + 1);
        result.push_back(std::move(runner));
    }
    return result;
}
}
