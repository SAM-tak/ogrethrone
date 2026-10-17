#pragma once

#include "editor/Level.h"
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace throne {
/// Prefabs read on demand and kept until their file changes (TDDja/06 5).
/// A prefab being edited can be put in place of its file so instances show the edit.
class Library {
public:
    /// @param root Project directory prefab paths are relative to.
    explicit Library(std::filesystem::path root = {});

    /// Returns the prefab at @p path: the one put() there, or the file, read again when it changed.
    /// @throws std::runtime_error when the file is missing or malformed.
    const Prefab& prefab(const std::string& path);
    /// Uses @p prefab for @p path instead of the file until forget().
    void put(const std::string& path, Prefab prefab);
    /// Goes back to reading @p path from its file.
    void forget(const std::string& path);

private:
    struct Entry {
        Prefab prefab;
        std::filesystem::file_time_type stamp;
        bool pinned = false;
    };
    std::filesystem::path root_;
    std::map<std::string, Entry> entries_;
};

/// One node of an expanded hierarchy, with every override applied.
struct ExpandedNode {
    /// From the expanded content's own node down through prefab instances; a
    /// single ID is a node of the content itself.
    NodePath path;
    /// Index of the parent in Expansion::nodes, or Expansion::npos for a root.
    std::size_t parent;
    /// The resolved values. An instance node keeps its prefab and overrides.
    SceneNode node;
    /// For an instance node, the ID of the prefab root it became; otherwise 0.
    std::uint32_t root = 0;
};

/// One runner of an expanded hierarchy, with field overrides applied.
struct ExpandedRunner {
    /// The prefab instance node it came from; empty for the content's own runner.
    NodePath instance;
    Runner runner;
};

/// A scene or prefab with its prefab instances expanded (TDDja/06 5).
struct Expansion {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    /// Parents before children, siblings in order.
    std::vector<ExpandedNode> nodes;
    std::vector<ExpandedRunner> runners;
    /// Instances that could not be expanded (missing prefab, cycle); the rest is still expanded.
    std::vector<std::string> errors;

    /// Index of the node at @p path, or npos. A merged prefab root may also be
    /// named by its own ID after the instance's (see NodePath).
    std::size_t find(const NodePath& path) const;
};

/// Expands @p content; a variant expands its base, the base's root at the empty
/// path, with its overrides. Overrides apply inner instances first, then outer ones,
/// then @p outer (the overrides of whatever instantiates @p content, such as a
/// level's scene instance). @p self is the content's own path, for cycle checks.
Expansion expand(const Prefab& content, Library& library, const std::string& self = {},
                 const Overrides* outer = nullptr);

/// A level with the scene files its instances use: what the editor edits.
struct World {
    Level level;
    /// Scene files by project-relative path.
    std::map<std::string, Scene> scenes;
};

/// Every scene instance of a world, expanded: what the viewport shows and Play runs.
struct LevelExpansion {
    struct Instance {
        /// SceneInstance::id.
        std::uint32_t id;
        Expansion expansion;
    };
    std::vector<Instance> instances;

    /// The expansion of scene instance @p id, or nullptr.
    Expansion* find(std::uint32_t id);
};

/// Expands every scene instance of @p world; an instance whose scene is not loaded
/// expands to nothing, with an error.
LevelExpansion expand(const World& world, Library& library);
}
