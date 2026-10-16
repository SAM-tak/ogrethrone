#pragma once

#include "core/Math.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace throne {
/// One editable object in a scene. Currently every object is a unit cube.
struct SceneObject {
    /// Unique nonzero ID within its document.
    std::uint32_t id = 0;
    /// Display name; 1 to 255 bytes of UTF-8.
    std::string name;
    /// World position.
    Vec3 position{0, 0.5f, 0};
    /// Euler angles in degrees, applied about X, then Y, then Z.
    Vec3 rotation;
    /// Per-axis scale; each component is at least 0.001.
    Vec3 scale{1, 1, 1};
    /// Linear RGB colour with components in [0, 1].
    std::array<float, 3> colour{0.32f, 0.62f, 0.82f};
    /// Project-relative path of the L^ module^ that drives this object in Play
    /// mode (TDDja/02 4.2); empty for none.
    std::string script;
};

/// The editor's scene data, independent of OGRE and the simulation world.
struct SceneDocument {
    /// Objects in hierarchy order.
    std::vector<SceneObject> objects;
    /// ID assigned to the next added object.
    std::uint32_t nextId = 1;

    /// Appends a cube with default transform and a generated name.
    /// @throws std::runtime_error when the ID space is exhausted.
    SceneObject& addCube();
    /// Returns the object with @p id, or nullptr.
    SceneObject* find(std::uint32_t id);
    /// Writes the document as `ogrethrone-scene` version 1 LTON (L^ Table Object Notation).
    ///
    /// The file is written to a temporary sibling and then replaced, so a failed
    /// save leaves any existing file intact.
    /// @throws std::runtime_error on non-finite values or I/O failure.
    void save(const std::filesystem::path& path) const;
    /// Reads and validates a document written by save().
    ///
    /// The text is evaluated by L^ as the body of an f^, so it may contain
    /// expressions but cannot call anything with effects.
    /// @throws std::runtime_error when the file is missing, malformed or out of range.
    static SceneDocument load(const std::filesystem::path& path);
    /// Returns a small two-cube scene used when the editor starts.
    static SceneDocument example();
};
}
