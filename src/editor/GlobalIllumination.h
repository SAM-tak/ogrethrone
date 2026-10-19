#pragma once

#include "editor/Level.h"
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace Ogre {
class Camera;
class HlmsPbs;
class IrradianceField;
class ParallaxCorrectedCubemapAuto;
class Root;
class SceneManager;
}

namespace throne {
/// Baked global illumination of a level: parallax-corrected cubemap probes, placed
/// over a grid and fitted to the surrounding geometry, give reflections; a raster
/// irradiance field (DDGI-style probes with depth) gives diffuse light. Both render
/// the level as it is drawn when baked, so they hold one bounce of its lighting.
///
/// A bake is written to a folder in the project and read back when the level opens,
/// so it is never rendered again until it is rebaked.
class GlobalIllumination {
public:
    /// Receives progress and problems.
    using Log = std::function<void(const std::string&)>;

    /// @param scene Must outlive this object; its workspaces and probes are created there.
    GlobalIllumination(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log);
    /// Unbinds and destroys the bake.
    ~GlobalIllumination();
    GlobalIllumination(const GlobalIllumination&) = delete;
    GlobalIllumination& operator=(const GlobalIllumination&) = delete;

    /// Renders all probes of @p settings from the scene as it is drawn now, then uses them
    /// instead of any earlier bake. The scene is drawn with the bake in use, so baking
    /// again adds one more bounce. @p camera only seeds the probes' tracking; it is not moved.
    void bake(const GiSettings& settings, Ogre::Camera& camera);
    /// Writes the bake into @p folder, creating it.
    void save(const std::filesystem::path& folder) const;
    /// Reads a bake that save() wrote for the same settings and uses it. Answers false,
    /// using nothing, when the folder holds none or one made for other settings.
    bool load(const std::filesystem::path& folder, const GiSettings& settings);
    /// Stops using and destroys the bake.
    void clear();

    /// Whether a bake is in use.
    bool active() const { return field_ || reflections_; }
    /// The settings of the bake in use.
    const GiSettings& settings() const { return settings_; }

private:
    /// Creates a reflection-probe system sized for @p probes probes, unbound.
    std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> createReflections(unsigned probes);
    /// Creates an irradiance field for @p settings, unbound and not yet rendered.
    std::unique_ptr<Ogre::IrradianceField> createField(const GiSettings& settings);
    /// Replaces the bake in use with these, made for @p settings.
    void use(const GiSettings& settings, std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections,
             std::unique_ptr<Ogre::IrradianceField> field);

    Ogre::Root& root_;
    Ogre::SceneManager& scene_;
    Ogre::HlmsPbs& pbs_;
    Log log_;
    GiSettings settings_;
    std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections_;
    std::unique_ptr<Ogre::IrradianceField> field_;
};
}
