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

/// Where and how densely a level bakes its global illumination (TDDja/09 9).
struct GiSettings {
    /// Whether the level uses baked global illumination.
    bool enabled = false;
    /// The world-space box the probes cover.
    Vec3 lower{-10, 0, -10}, upper{10, 5, 10};
    /// Parallax-corrected reflection probes along X, Y and Z.
    std::array<unsigned, 3> reflectionProbes{2, 1, 2};
    /// How many times the probes are rendered, each lit by the previous rendering:
    /// one bounce of light off the scene per rendering.
    unsigned bounces = 2;
    /// The finest spacing of the Adaptive Irradiance Field (TDDja/12), the diffuse light's probes,
    /// in metres along X, Y and Z.
    Vec3 probeSpacing{1, 1, 1};
    /// Where the probes' grid lies: probes are at this offset plus whole multiples of the
    /// spacing, in metres along X, Y and Z (layers at the floor, say).
    Vec3 probeOffset{0, 0, 0};
    /// Objects of smaller volume than this (cubic metres, of their bounds) do not place Adaptive
    /// Probes around them (Unity's Min Renderer Volume Size).
    float minObjectVolume = .1f;
    /// Whether the Adaptive Irradiance Field also covers empty space far from any surface, with the coarsest cells.
    bool fillEmptySpaces = false;
    /// Whether the Adaptive Irradiance Field covers the box above (Unity's Local mode) instead of the bounds
    /// of everything drawn (Global mode, the default).
    bool localProbeVolume = false;
    /// Whether diffuse probes bake with one coarser map for the sun's shadow instead of the
    /// viewport's cascades (local lights keep theirs): faster, slightly less exact.
    bool fastBakeShadows = true;
    /// Equality of every field; a bake records them all (GlobalIllumination's gi.lton).
    bool operator==(const GiSettings& other) const {
        return enabled == other.enabled && lower == other.lower && upper == other.upper &&
               reflectionProbes == other.reflectionProbes && bounces == other.bounces && probeSpacing == other.probeSpacing &&
               probeOffset == other.probeOffset &&
               minObjectVolume == other.minObjectVolume && fillEmptySpaces == other.fillEmptySpaces &&
               localProbeVolume == other.localProbeVolume && fastBakeShadows == other.fastBakeShadows;
    }
    /// Negation of equality.
    bool operator!=(const GiSettings& other) const { return !(*this == other); }
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
    /// its sky; environmentScale multiplies its radiance (sky, reflections and ambient light),
    /// tinted by environmentTemperature like a light's (K; 0 leaves its colour as it is).
    std::string environment;
    float environmentScale = 1, environmentTemperature = 0;
    /// Baked global illumination; the bake lives in the `<level>.gi` folder beside the level.
    GiSettings gi;
    /// Optional opening view: orbit target, radians and distance.
    bool openingCamera = false;
    Vec3 target{0,.5f,0};
    float yaw = .65f, pitch = .45f, distance = 9;
    /// Vertical field of view of the viewport camera, in degrees.
    float fieldOfView = 45;
};

/// The game-side data of one level (TDDja/04, 05, 06): the scene instances it
/// starts with and the systems that belong to the level itself. A level holds
/// no scene nodes of its own. Independent of OGRE and the simulation world.
struct Level : SystemSet {
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
