#pragma once

#include "core/AssetRef.h"
#include "core/Math.h"
#include "editor/Lton.h"
#include <array>
#include <string>

namespace throne {
/// Where and how densely a scene bakes its global illumination (TDDja/09 9, 12, 13).
struct GiSettings {
    /// Whether the scene uses baked global illumination.
    bool enabled = false;
    /// The box, in the scene's coordinates, the Local probe volume covers.
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
    /// Whether the Adaptive Irradiance Field also covers empty space far from any surface, with
    /// the coarsest cells.
    bool fillEmptySpaces = false;
    /// Whether the Adaptive Irradiance Field covers the box above (Unity's Local mode) instead of
    /// the bounds of the scene's nodes (Global mode, the default).
    bool localProbeVolume = false;
    /// Whether diffuse probes bake with one coarser map for the sun's shadow instead of the
    /// viewport's cascades (local lights keep theirs): faster, slightly less exact.
    bool fastBakeShadows = true;
    /// Equality of every field; a bake records them all (GlobalIllumination's gi.lton).
    bool operator==(const GiSettings& other) const {
        return enabled == other.enabled && lower == other.lower && upper == other.upper &&
               reflectionProbes == other.reflectionProbes && bounces == other.bounces && probeSpacing == other.probeSpacing &&
               probeOffset == other.probeOffset && minObjectVolume == other.minObjectVolume &&
               fillEmptySpaces == other.fillEmptySpaces && localProbeVolume == other.localProbeVolume &&
               fastBakeShadows == other.fastBakeShadows;
    }
    /// Negation of equality.
    bool operator!=(const GiSettings& other) const { return !(*this == other); }
};

/// A scene's environment and baked global illumination (TDDja/13). A level is shown in the
/// environment of its first scene.
struct SceneLighting {
    /// The editor's default directional light, and OGRE's procedural sky.
    bool defaultSun = true, sky = true;
    /// Filmic (ACES) tone mapping of the HDR scene instead of clamping it; exposure is in stops.
    bool toneMapping = false;
    float exposure = 0;
    /// Hemispherical ambient light, used without an environment map.
    Vec3 ambientUpper{.22f, .25f, .30f}, ambientLower{.06f, .07f, .09f};
    Vec3 ambientDirection{0, 1, 0};
    /// Optional project-relative equirectangular HDR file lighting the scene and drawn as its
    /// sky; environmentScale multiplies its radiance (sky, reflections and ambient light),
    /// tinted by environmentTemperature like a light's (K; 0 leaves its colour as it is).
    AssetRef environment;
    float environmentScale = 1, environmentTemperature = 0;
    /// Baked global illumination; the bake lives in the `<scene>.gi` folder beside the scene.
    GiSettings gi;
};

/// Reads @p table as writeLighting() writes it; missing fields keep their defaults.
SceneLighting readLighting(const LhatTable& table);
/// Writes @p lighting into a new table of @p out.
LhatValue writeLighting(lton::Writer& out, const SceneLighting& lighting);
}
