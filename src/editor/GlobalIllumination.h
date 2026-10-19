#pragma once

#include "editor/Level.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Ogre {
class Camera;
class Image2;
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
    /// Unbinds and destroys the bake and any bake in progress.
    ~GlobalIllumination();
    GlobalIllumination(const GlobalIllumination&) = delete;
    GlobalIllumination& operator=(const GlobalIllumination&) = delete;

    /// Starts baking @p settings: prepares its reflection probes and irradiance field, which
    /// continueBake() then renders from the scene as drawn. When it is done the bake
    /// replaces the one in use; as the scene is drawn with that one, baking again adds one
    /// more bounce. @p camera only seeds the probes' tracking; it is not moved.
    void beginBake(const GiSettings& settings, Ogre::Camera& camera);
    /// Renders more probes of the bake begun last, for about @p budget: reflection probes
    /// (at least one) or, once they are all done, irradiance probes. Call it once a frame.
    void continueBake(std::chrono::milliseconds budget);
    /// Which probes the bake is rendering and how many are done, for a progress display.
    std::string bakeStatus() const;
    /// Whether a bake has begun and not yet finished.
    bool baking() const { return pending_.field != nullptr; }
    /// The share of the current bake's probes already rendered, from 0 to 1.
    float bakeProgress() const;
    /// Writes the bake into @p folder, creating it.
    void save(const std::filesystem::path& folder) const;
    /// A bake as read from its folder, before any OGRE object exists.
    struct Stored {
        /// One reflection probe's placement.
        struct Probe {
            Vec3 camera, areaCentre, areaHalfSize, innerRegion, shapeCentre, shapeHalfSize;
        };
        GiSettings settings;
        std::vector<Probe> probes;
        std::shared_ptr<Ogre::Image2> reflections, irradiance, depth;
        /// Why there is nothing to use, when there is a reason worth logging.
        std::string message;
    };
    /// Reads the bake save() wrote into @p folder for @p settings; safe on a worker thread.
    /// Holds no images when the folder has none or one made for other settings.
    static Stored read(const std::filesystem::path& folder, const GiSettings& settings);
    /// Uses a bake read(), replacing the one in use; answers false (after logging why) when
    /// it holds none.
    bool use(const Stored& stored);
    /// Stops using and destroys the bake, and abandons any bake in progress.
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
    /// A bake being rendered, with the probes of each kind it has rendered.
    struct Pending {
        GiSettings settings;
        std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections;
        std::unique_ptr<Ogre::IrradianceField> field;
        unsigned reflectionProbesDone = 0, fieldProbesDone = 0;
        /// How many irradiance probes the next field update renders.
        unsigned fieldBatch = 1;
    } pending_;
    /// Places and renders the next reflection probe of the bake in progress.
    void renderNextReflectionProbe();
};
}
