#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace throne {
/// Presentation settings for a standalone game, independent of editor preferences.
struct GameRendering {
    bool vsync = true;
};

/// Per-user editor preferences, stored beside the project list, never in a project.
struct EditorPreferences {
    bool vsync = true;
    /// Width in pixels of the Inspector's label column, set by the handle over it.
    float inspectorLabelWidth = 140;
    /// Size of the viewport's statistics panel, in pixels at the 13-pixel font, set by dragging
    /// its left and bottom edges; a height of 0 fits its rows.
    float statisticsWidth = 240, statisticsHeight = 0;
    /// Missing or damaged preferences use the defaults; an empty path disables persistence.
    static EditorPreferences load(const std::filesystem::path& file);
    /// Saves preferences atomically; an empty path disables persistence.
    void save(const std::filesystem::path& file) const;
};

/// A project folder (TDDja/03): `project.throne.lton` with `levels/`, `scripts/`
/// and `lhat-host.json` beside it. Paths inside a project are relative to its root.
struct Project {
    /// Name of the marker file that makes a folder a project.
    static constexpr const char* markerName = "project.throne.lton";

    /// Absolute path of the project folder.
    std::filesystem::path root;
    /// Display name.
    std::string name;
    /// Project-relative path of the level opened with the project; may be empty.
    std::string mainLevel;
    /// Defaults used by the game's output window, not the editor's window.
    GameRendering rendering;

    /// Creates `<parent>/<name>/` with the standard layout, a default `main.lh`, an
    /// example scene and a main level that loads it.
    /// @throws std::runtime_error when the name is unusable or the folder exists and is not empty.
    static Project create(const std::filesystem::path& parent, const std::string& name);
    /// Opens the project at @p path, which may be the folder or its marker file.
    /// @throws std::runtime_error when there is no valid marker.
    static Project open(const std::filesystem::path& path);
    /// Writes the marker file.
    void save() const;
    /// Resolves a project-relative path.
    std::filesystem::path resolve(std::string_view relative) const;
};

/// The user's list of known projects, most recently opened first (TDDja/03 4.2).
struct ProjectList {
    /// One known project folder.
    struct Entry {
        std::filesystem::path root;
        /// UNIX time in seconds.
        std::int64_t lastOpened = 0;
    };
    std::vector<Entry> entries;

    /// `%APPDATA%/OgreThrone/projects.lton`, or `$XDG_CONFIG_HOME/ogrethrone/projects.lton`.
    static std::filesystem::path defaultFile();
    /// Reads @p file; a missing or unreadable file gives an empty list.
    static ProjectList load(const std::filesystem::path& file);
    /// Writes @p file, creating its folder.
    void save(const std::filesystem::path& file) const;
    /// Moves @p root to the front with the current time, adding it when new.
    void touch(const std::filesystem::path& root);
    /// Forgets @p root; the folder itself is left alone.
    void remove(const std::filesystem::path& root);
};
}
