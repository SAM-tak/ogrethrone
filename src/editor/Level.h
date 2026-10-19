#pragma once

#include "editor/Scene.h"
#include "runtime/FramePipeline.h"
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

/// Persisted level lighting and the initial editor camera for authored showcases.
struct RenderingSettings {
    bool defaultSun = true, sky = true, grid = true, clustered = true, debugClusters = false;
    /// Filmic (ACES) tone mapping of the HDR scene instead of clamping it; exposure is in stops.
    bool toneMapping = false;
    float exposure = 0;
    Vec3 ambientUpper{.22f,.25f,.30f}, ambientLower{.06f,.07f,.09f};
    Vec3 ambientDirection{0,1,0};
    float minDistance = .1f, maxDistance = 100;
    /// Optional project-relative equirectangular HDR file lighting the level and drawn as
    /// its sky; environmentScale scales the light it gives.
    std::string environment;
    float environmentScale = 1;
    /// When set, the level itself is captured at probePosition and its irradiance
    /// replaces the environment's diffuse light, adding one bounce off nearby surfaces.
    bool lightProbe = false;
    Vec3 probePosition{0,1.5f,0};
    /// Optional opening view: orbit target, radians and distance.
    bool openingCamera = false;
    Vec3 target{0,.5f,0};
    float yaw = .65f, pitch = .45f, distance = 9;
    /// Vertical field of view of the viewport camera, in degrees.
    float fieldOfView = 45;
};

/// The game-side data of one level (TDDja/04, 05, 06): the scene instances it
/// starts with and the runners that belong to the level itself. A level holds
/// no scene nodes of its own. Independent of OGRE and the simulation world.
struct Level : RunnerSet {
    RenderingSettings rendering;
    /// Scene instances loaded when the level starts, in order.
    std::vector<SceneInstance> scenes;
    std::uint32_t nextSceneId = 1;
    /// One level-wide execution schedule; engine barriers are fixed.
    std::vector<ExecutionStage> stages = defaultStages();
    /// Debug mode preserves snapshots and barriers while running group members in order.
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
