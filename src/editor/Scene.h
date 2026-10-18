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
/// A scene node's ID: a positive 64-bit Snowflake ID (see newNodeId()), so IDs
/// from different files do not meet (TDDja/06 3). 0 means no node; files may
/// hold any positive ID.
using NodeId = std::uint64_t;

/// Node IDs from a scene or prefab's own node down through prefab instances,
/// outermost first (TDDja/06 3). Each ID is looked for anywhere below the node
/// before it, so a path stays valid when a subtree is moved into a new prefab.
/// The instances along it tell apart the copies of a prefab placed more than
/// once. An instance node and its prefab's root are one node, which either's
/// ID names.
using NodePath = std::vector<NodeId>;

/// A reference to a scene node (TDDja/05 4, 06 3.1).
struct NodeRef {
    /// ID of the level's scene instance, or 0 for the scene (or prefab) holding the reference.
    std::uint32_t scene = 0;
    /// The node's ID in that scene; 0 means no node.
    NodeId node = 0;
    /// Path below `node` when it is a prefab instance; empty for the node itself.
    NodePath inner;
    bool operator==(const NodeRef& other) const {
        return scene == other.scene && node == other.node && inner == other.inner;
    }
};

/// A value written for one behaviour field, overriding the script's default.
using FieldValue = std::variant<double, bool, std::string, NodeRef>;

/// One script attachment and its saved field values; also used for custom Component data.
struct Behaviour {
    /// Project-relative path of a module^ publishing `Behaviour = def^{ ... }`.
    std::string script;
    /// Field name to value; fields not listed keep the script's default.
    std::map<std::string, FieldValue> fields;
    /// Execution stage for a node attachment.
    std::string runner = "Update";
    /// Runner attachments may publish System instead of Behaviour.
    bool system = false;
};

/// Direct Behaviour/System attachments grouped into the named execution stage (TDDja/07).
/// A runner has no transform; it reaches scene nodes only through field values.
struct Runner {
    /// Unique nonzero ID within the file that holds the runner.
    std::uint32_t id = 0;
    /// Display name; 1 to 255 bytes.
    std::string name;
    std::vector<Behaviour> behaviours;
};

/// Property values an instance gives one node of its prefab (TDDja/06 4); unset ones
/// keep the prefab's. Component properties apply only where the node has the component.
struct NodeOverride {
    /// The node, relative to the instance; empty for the instance node itself (the root).
    NodePath path;
    std::optional<std::string> name;
    /// Transform properties.
    std::optional<Vec3> position, rotation, scale;
    /// BoxPrimitive properties.
    std::optional<std::array<float, 3>> colour;
    /// Replacement attachment list, including field edits, additions and removals.
    std::optional<std::vector<Behaviour>> behaviours;
    /// Field overrides for existing script components; never adds or removes a component.
    std::map<std::string, std::map<std::string, FieldValue>> scriptFields;
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

struct AddedNode;

/// A runner an instance's overrides remove: its ID in the instance it comes from.
struct RemovedRunner {
    /// That instance, relative to the overriding one; empty for the instance itself.
    NodePath instance;
    std::uint32_t runner = 0;
};

/// What one instance changes in what it instantiates (TDDja/06 4).
struct Overrides {
    std::vector<NodeOverride> nodes;
    std::vector<FieldOverride> runners;
    /// Nodes the instance adds, parents before children.
    std::vector<AddedNode> added;
    /// Nodes the instance removes, each with all below it; relative to the
    /// instance and never empty (the instance node itself stays).
    std::vector<NodePath> removed;
    /// Runners the instance adds, with IDs from newRunnerId().
    std::vector<Runner> addedRunners;
    std::vector<RemovedRunner> removedRunners;

    /// Whether nothing is overridden.
    bool empty() const;
    /// Returns the override for @p path, adding an empty one when there is none.
    NodeOverride& node(const NodePath& path);
    /// Returns the field override for that behaviour, adding an empty one when there is none.
    FieldOverride& fieldsOf(const NodePath& instance, std::uint32_t runner, std::size_t behaviour);
};

/// The Transform component (TDDja/07 3): placement relative to the nearest ancestor
/// with a Transform; a node without one passes its parent's placement on unchanged.
/// Also a scene instance's placement in its level.
struct Transform {
    Vec3 position;
    /// Euler angles in degrees, applied about X, then Y, then Z.
    Vec3 rotation;
    /// Per-axis scale; each component is at least 0.001.
    Vec3 scale{1, 1, 1};
};

/// The BoxPrimitive component (TDDja/07 3): a unit cube drawn at the node.
struct BoxPrimitive {
    /// Linear RGB colour with components in [0, 1].
    std::array<float, 3> colour{0.32f, 0.62f, 0.82f};
};

/// One node of a scene hierarchy (TDDja/07 3): an ID, a name and a parent, and the
/// components it has. A node naming a prefab is a prefab instance and becomes the
/// prefab's root (TDDja/06 3): it gives the root its name and Transform, everything
/// else comes from the prefab, and it has no children of its own.
struct SceneNode {
    /// Unique nonzero ID; see NodeId.
    NodeId id = 0;
    /// Display name; 1 to 255 bytes.
    std::string name;
    /// ID of the parent node, or 0 for the scene root.
    NodeId parent = 0;
    std::optional<Transform> transform;
    /// Unused by an instance, whose root's comes from the prefab or its overrides.
    std::optional<BoxPrimitive> box;
    /// Scripts attached to this node, in attachment order.
    std::vector<Behaviour> behaviours;
    /// Script-defined component types, keyed by their query name. Each attachment publishes Component.
    std::map<std::string, Behaviour> scriptComponents;
    /// Project-relative path of the prefab this node instantiates; empty for a plain node.
    std::string prefab;
    /// What this instance changes in the prefab.
    Overrides overrides;
};

/// A node an instance adds to what it instantiates (TDDja/06 4). Its path is the
/// instance's followed by its ID, wherever its parent is.
struct AddedNode {
    /// The parent, relative to the instance; empty for the instance node itself.
    NodePath parent;
    /// The node; its `parent` is unused. It may be an instance itself.
    SceneNode node;
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
    /// For a variant, the project-relative path of the prefab it is based on.
    std::string base;
    /// For a variant, what it changes in its base.
    Overrides overrides;

    /// Returns the node with @p id, or nullptr.
    SceneNode* findNode(NodeId id);
    /// @copydoc findNode
    const SceneNode* findNode(NodeId id) const;
    /// Appends a cube node (a Transform half a unit up and a BoxPrimitive) named @p name
    /// under @p parent, with a new ID, and returns it.
    SceneNode& addNode(const std::string& name, NodeId parent = 0);
    /// Removes the node and all its descendants.
    void removeNode(NodeId id);
    /// Whether @p id is @p ancestor or lies below it.
    bool isWithin(NodeId id, NodeId ancestor) const;
    /// Moves @p id (with its subtree) under @p parent, just before its child
    /// @p before, or as the last child when @p before is 0.
    /// @return false when that would make a node its own ancestor, or @p before
    ///         is @p id or not a child of @p parent.
    bool reparent(NodeId id, NodeId parent, NodeId before = 0);

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

/// Returns a new NodeId: a Snowflake ID (core/Ids.h) counted from 2026-01-01 UTC. Not thread safe.
NodeId newNodeId();
/// Returns a random runner ID from 0x80000000 up, for runners an instance adds:
/// a file numbers its own runners from 1, so the two do not meet.
std::uint32_t newRunnerId();
/// Writes @p overrides as an LTON table; shared by prefab instances and scene instances.
LhatValue writeOverrides(lton::Writer& out, const Overrides& overrides);
/// Reads what writeOverrides wrote (nil^ reads as no overrides). @throws lton::Invalid on malformed data.
Overrides readOverrides(LhatValue value);
/// Writes node or runner attachments, including their execution stage.
LhatValue writeBehaviours(lton::Writer& out, const std::vector<Behaviour>& behaviours);
/// Reads attachments; nil means an empty list.
std::vector<Behaviour> readBehaviours(LhatValue value);
/// Writes @p runners as an LTON sequence; shared by prefabs, scenes and levels.
LhatValue writeRunners(lton::Writer& out, const std::vector<Runner>& runners);
/// Reads what writeRunners wrote. @throws lton::Invalid on malformed data.
std::vector<Runner> readRunners(LhatValue value, std::uint32_t& nextId);
}
