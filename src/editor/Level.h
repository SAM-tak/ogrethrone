#pragma once

#include "editor/Scene.h"
#include <filesystem>
#include <string>
#include <vector>

namespace throne {
/// The game-side data of one level (TDDja/04, 05): the scenes it starts with and
/// the runners that belong to the level itself. Independent of OGRE and the
/// simulation world; "scene" is left to OGRE's rendering terms.
struct Level : RunnerSet {
    /// Project-relative paths of the scenes loaded when the level starts.
    std::vector<std::string> scenes;

    /// Writes the level as `ogrethrone-level` version 2 LTON (L^ Table Object Notation).
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

/// A scene loaded into a level, with its project-relative path.
struct LoadedScene {
    std::string path;
    Scene scene;
};

/// A level with the scenes it has loaded: what the editor edits and Play runs on.
struct World {
    Level level;
    std::vector<LoadedScene> scenes;

    /// Returns the loaded scene at @p path, or nullptr.
    LoadedScene* findScene(const std::string& path) {
        for (auto& loaded : scenes)
            if (loaded.path == path) return &loaded;
        return nullptr;
    }
};
}
