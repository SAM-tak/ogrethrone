#pragma once

#include "core/Math.h"
#include "editor/Lton.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace throne {
/// A reference to a scene node: the scene's project-relative path and the node's ID.
/// An empty scene path means the scene that holds the reference (TDDja/05 4).
struct NodeRef {
    std::string scene;
    std::uint32_t node = 0;
    bool operator==(const NodeRef& other) const { return scene == other.scene && node == other.node; }
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

/// One drawable node of a scene hierarchy. Currently every node is a unit cube.
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
    /// Linear RGB colour with components in [0, 1].
    std::array<float, 3> colour{0.32f, 0.62f, 0.82f};
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

/// Nodes and runners: what prefabs and scenes share (TDDja/05 2, 3).
struct Prefab : RunnerSet {
    /// Nodes with every parent before its children; siblings keep this order.
    std::vector<SceneNode> nodes;
    std::uint32_t nextNodeId = 1;

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

/// Writes @p runners as an LTON sequence; shared by prefabs, scenes and levels.
LhatValue writeRunners(lton::Writer& out, const std::vector<Runner>& runners);
/// Reads what writeRunners wrote. @throws lton::Invalid on malformed data.
std::vector<Runner> readRunners(LhatValue value, std::uint32_t& nextId);
}
