#pragma once

#include "editor/Scene.h"
#include "runtime/FramePipeline.h"
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace throne {
/// One placement of a scene in a level (TDDja/06 3.1). A scene instance has no
/// transform; the same scene may be placed several times, each with its own overrides.
struct SceneInstance {
    /// Unique nonzero ID within the level; NodeRef::scene names it.
    std::uint32_t id = 0;
    /// Project-relative path of the scene.
    std::string scene;
    /// Where the scene is placed; every node of the scene lies below it.
    Transform transform;
};

/// How a level is drawn; its environment comes from its first scene (TDDja/13).
struct RenderingSettings {
    bool grid = true, clustered = true, debugClusters = false;
    /// The depth range Forward+ divides into cells.
    float minDistance = .1f, maxDistance = 100;
};

/// The game-side data of one level (TDDja/04, 05, 06): the scene instances it
/// starts with and the systems that belong to the level itself. A level holds
/// no scene nodes of its own. Independent of OGRE and the simulation world.
struct Level : SystemSet {
    RenderingSettings rendering;
    /// Project-relative *.mixer.lton; empty uses the built-in Master.
    std::string mixer;
    /// Scene instances loaded when the level starts, in order.
    std::vector<SceneInstance> scenes;
    std::uint32_t nextSceneId = 1;
    /// The simulation column of the level's schedule (TDDja/07); engine stages keep their order.
    std::vector<std::string> stages = defaultStages();
    /// No frame delay: simulation, then rendering of its result on one thread. Otherwise
    /// rendering draws the previous frame while simulation runs on a worker thread.
    bool serialExecution = false;

    /// Appends an instance of the scene at @p path and returns it.
    SceneInstance& addScene(const std::string& path);
    /// Returns the scene instance with @p id, or nullptr.
    SceneInstance* findScene(std::uint32_t id);

    /// Writes the level as `ogrethrone-level` version 3 LTON (L^ Table Object Notation).
    ///
    /// The file is written to a temporary sibling and then replaced, so a failed
    /// save leaves any existing file intact.
    /// @throws std::runtime_error on non-finite values or I/O failure.
    void save(const std::filesystem::path& path) const;
    /// Reads and validates a level written by save().
    ///
    /// The text is evaluated by L^ as the body of an f^, so it may contain
    /// expressions but cannot call anything with effects.
    /// @throws lton::Invalid when the file is missing, malformed or out of range.
    static Level load(const std::filesystem::path& path);
};
}
