#pragma once

#include "core/Math.h"
#include "editor/Lton.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace throne {
/// Node IDs from a scene or prefab's own node down through prefab instances,
/// outermost first (TDDja/06 3). IDs are unique only within one file, so a
/// node inside nested instances needs the whole path. An instance node and its
/// prefab's root are one node, named by the instance's ID alone; the root's own
/// ID after it is accepted too, as the prefab itself names its root.
using NodePath = std::vector<std::uint32_t>;

/// A reference to a scene node (TDDja/05 4, 06 3.1).
struct NodeRef {
    /// ID of the level's scene instance, or 0 for the scene (or prefab) holding the reference.
    std::uint32_t scene = 0;
    /// The node's ID in that scene; 0 means no node.
    std::uint32_t node = 0;
    /// Path below `node` when it is a prefab instance; empty for the node itself.
    NodePath inner;
    bool operator==(const NodeRef& other) const {
        return scene == other.scene && node == other.node && inner == other.inner;
    }
};

/// A value written for one behaviour field, overriding the script's default.
using FieldValue = std::variant<double, bool, std::string, NodeRef>;

/// One script attached to a runner, with the field values this runner gives it.
struct Behaviour {
    /// Project-relative path of a module^ publishing `Behaviour = def^{ ... }`.
    std::string script;
    /// Field name to value; fields not listed keep the script's default.
    std::map<std::string, FieldValue> fields;
};

/// Something that runs behaviours, independent of the scene hierarchy (TDDja/05).
/// A runner has no transform; it reaches scene nodes only through field values.
struct Runner {
    /// Unique nonzero ID within the file that holds the runner.
    std::uint32_t id = 0;
    /// Display name; 1 to 255 bytes.
    std::string name;
    std::vector<Behaviour> behaviours;
};

/// Property values an instance gives one node of its prefab (TDDja/06 4); unset ones keep the prefab's.
struct NodeOverride {
    /// The node, relative to the instance; empty for the instance node itself (the root).
    NodePath path;
    std::optional<std::string> name;
    std::optional<Vec3> position, rotation, scale;
    std::optional<std::array<float, 3>> colour;
};

/// Field values an instance gives one behaviour of a runner in its prefab.
struct FieldOverride {
    /// The prefab instance the runner comes from, relative to the instance; empty for
    /// the instance's own prefab.
    NodePath instance;
    std::uint32_t runner = 0;
    std::size_t behaviour = 0;
    std::map<std::string, FieldValue> fields;
};

/// What one instance changes in what it instantiates (TDDja/06 4).
struct Overrides {
    std::vector<NodeOverride> nodes;
    std::vector<FieldOverride> runners;

    /// Whether nothing is overridden.
    bool empty() const { return nodes.empty() && runners.empty(); }
    /// Returns the override for @p path, adding an empty one when there is none.
    NodeOverride& node(const NodePath& path);
    /// Returns the field override for that behaviour, adding an empty one when there is none.
    FieldOverride& fieldsOf(const NodePath& instance, std::uint32_t runner, std::size_t behaviour);
};

/// One drawable node of a scene hierarchy. Currently every node is a unit cube.
/// A node naming a prefab is a prefab instance and becomes the prefab's root
/// (TDDja/06 3): it gives the root its name and transform, everything else comes
/// from the prefab, and it has no children of its own.
struct SceneNode {
    /// Unique nonzero ID within its scene.
    std::uint32_t id = 0;
    /// Display name; 1 to 255 bytes.
    std::string name;
    /// ID of the parent node, or 0 for the scene root.
    std::uint32_t parent = 0;
    /// Position relative to the parent.
    Vec3 position;
    /// Euler angles in degrees relative to the parent, applied about X, then Y, then Z.
    Vec3 rotation;
    /// Per-axis scale relative to the parent; each component is at least 0.001.
    Vec3 scale{1, 1, 1};
    /// Linear RGB colour with components in [0, 1]; unused by an instance, whose
    /// colour comes from the prefab or its overrides.
    std::array<float, 3> colour{0.32f, 0.62f, 0.82f};
    /// Project-relative path of the prefab this node instantiates; empty for a plain node.
    std::string prefab;
    /// What this instance changes in the prefab.
    Overrides overrides;
};

/// The runners of one file (a prefab, scene or level), in run order.
struct RunnerSet {
    std::vector<Runner> runners;
    std::uint32_t nextRunnerId = 1;

    /// Returns the runner with @p id, or nullptr.
    Runner* findRunner(std::uint32_t id);
    /// Appends a runner without behaviours and returns it.
    Runner& addRunner(const std::string& name);
    /// Removes the runner.
    void removeRunner(std::uint32_t id);
};

/// Nodes and runners: what prefabs and scenes share (TDDja/05 2, 3). A prefab
/// file has exactly one root node, or is a variant (TDDja/06 3.2): no nodes, only
/// a base prefab and overrides of it.
struct Prefab : RunnerSet {
    /// Nodes with every parent before its children; siblings keep this order.
    std::vector<SceneNode> nodes;
    std::uint32_t nextNodeId = 1;
    /// For a variant, the project-relative path of the prefab it is based on.
    std::string base;
    /// For a variant, what it changes in its base.
    Overrides overrides;

    /// Returns the node with @p id, or nullptr.
    SceneNode* findNode(std::uint32_t id);
    /// @copydoc findNode
    const SceneNode* findNode(std::uint32_t id) const;
    /// Appends a cube node named @p name under @p parent and returns it.
    SceneNode& addNode(const std::string& name, std::uint32_t parent = 0);
    /// Removes the node and all its descendants.
    void removeNode(std::uint32_t id);
    /// Whether @p id is @p ancestor or lies below it.
    bool isWithin(std::uint32_t id, std::uint32_t ancestor) const;
    /// Moves @p id (with its subtree) under @p parent, as the last child.
    /// @return false when that would make a node its own ancestor.
    bool reparent(std::uint32_t id, std::uint32_t parent);

    /// Whether this is a variant of another prefab.
    bool variant() const { return !base.empty(); }
    /// Reads `ogrethrone-prefab` LTON.
    static Prefab load(const std::filesystem::path& path);
    /// Writes `ogrethrone-prefab` LTON.
    void save(const std::filesystem::path& path) const;

protected:
    /// Reads the shared fields from a parsed file.
    void read(const LhatTable& root);
    /// Writes the shared fields into @p root.
    void write(lton::Writer& out, LhatValue root) const;
};

/// A prefab extended with scene-wide data (TDDja/05 2: the only difference).
struct Scene : Prefab {
    // Scene-wide data (global lighting, environment; TDDja/04 4) belongs here.

    /// Reads `ogrethrone-scene` LTON.
    static Scene load(const std::filesystem::path& path);
    /// Writes `ogrethrone-scene` LTON.
    void save(const std::filesystem::path& path) const;
    /// Returns a scene with two cubes, the first named SpinningCube.
    static Scene example();
};

/// Writes @p overrides as an LTON table; shared by prefab instances and scene instances.
LhatValue writeOverrides(lton::Writer& out, const Overrides& overrides);
/// Reads what writeOverrides wrote (nil^ reads as no overrides). @throws lton::Invalid on malformed data.
Overrides readOverrides(LhatValue value);
/// Writes @p runners as an LTON sequence; shared by prefabs, scenes and levels.
LhatValue writeRunners(lton::Writer& out, const std::vector<Runner>& runners);
/// Reads what writeRunners wrote. @throws lton::Invalid on malformed data.
std::vector<Runner> readRunners(LhatValue value, std::uint32_t& nextId);
}
