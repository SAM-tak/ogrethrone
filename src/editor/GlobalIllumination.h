#pragma once

#include "editor/AdaptiveIrradianceField.h"
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
class ParallaxCorrectedCubemapAuto;
class Root;
class SceneManager;
class TextureGpu;
}

namespace throne {
/// Baked global illumination of a level: parallax-corrected cubemap probes, placed over a grid
/// and fitted to the surrounding geometry, give reflections; the Adaptive Irradiance Field (TDDja/12), an
/// octree of irradiance probes finer near surfaces, give diffuse light. Both render the level
/// as it is drawn when baked, so they hold one bounce of its lighting.
///
/// A bake is written to a folder in the project and read back when the level opens,
/// so it is never rendered again until it is rebaked.
class GlobalIllumination {
public:
    /// Receives progress and problems.
    using Log = std::function<void(const std::string&)>;

    /// @param scene Must outlive this object; its workspaces and probes are created there.
    /// Installs the HlmsListener through which @p pbs reads the diffuse light.
    GlobalIllumination(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log);
    /// Unbinds and destroys the bake and any bake in progress, and removes the listener.
    ~GlobalIllumination();
    GlobalIllumination(const GlobalIllumination&) = delete;
    GlobalIllumination& operator=(const GlobalIllumination&) = delete;

    /// Starts baking @p settings with the diffuse probes of @p octree: prepares the reflection
    /// probes, which continueBake() then renders from the scene as drawn, and then the diffuse
    /// probes. When it is done the bake replaces the one in use; as the scene is drawn with that
    /// one, baking again adds one more bounce. @p camera only seeds the reflection probes'
    /// tracking; it is not moved.
    void beginBake(const GiSettings& settings, Ogre::Camera& camera, ProbeOctree octree);
    /// Renders more probes of the bake begun last, for about @p budget: reflection probes (at
    /// least one) or, once they are all done, diffuse probes, whose images are read back and
    /// projected a few frames later. Call it once a frame.
    void continueBake(std::chrono::milliseconds budget);
    /// Which probes the bake is rendering and how many are done, for a progress display.
    std::string bakeStatus() const;
    /// Whether a bake has begun and not yet finished.
    bool baking() const;
    /// The share of the current bake's probes already done, from 0 to 1.
    float bakeProgress() const;
    /// Writes the bake into @p folder, creating it, with the @p seconds it took (all passes).
    void save(const std::filesystem::path& folder, double seconds);

    /// The diffuse light of a bake: the octree's tables, as ProbeOctree describes them, packed
    /// into images the shader reads (TDDja/12 3), and where the octree lies.
    struct Diffuse {
        Vec3 lower, spacing{1, 1, 1};
        std::array<std::uint32_t, 3> top{};
        /// The top cells' entries (3D), the nodes, the leaves' corners and the probes (2D).
        std::shared_ptr<Ogre::Image2> topNodes, nodes, cells, probes;
    };
    /// A bake as read from its folder, before any OGRE object exists.
    struct Stored {
        /// One reflection probe's placement.
        struct Probe {
            Vec3 camera, areaCentre, areaHalfSize, innerRegion, shapeCentre, shapeHalfSize;
        };
        GiSettings settings;
        /// How long the bake took, in seconds; 0 when it was not recorded.
        double seconds = 0;
        std::vector<Probe> probes;
        std::shared_ptr<Ogre::Image2> reflections;
        Diffuse diffuse;
        /// Why there is nothing to use, when there is a reason worth logging.
        std::string message;
    };
    /// Reads the bake save() wrote into @p folder, with the settings it was made with; safe on
    /// a worker thread. Holds no images when the folder has none.
    static Stored read(const std::filesystem::path& folder);
    /// Uses a bake read(), replacing the one in use; answers false (after logging why) when
    /// it holds none.
    bool use(const Stored& stored);
    /// Stops using and destroys the bake, and abandons any bake in progress.
    void clear();

    /// Whether a bake is in use.
    bool active() const { return reflections_ != nullptr; }
    /// The settings of the bake in use.
    const GiSettings& settings() const { return settings_; }
    /// How long the bake in use took, in seconds; 0 when not known.
    double bakeSeconds() const { return bakeSeconds_; }
    /// Turns the debug display (View > GI Probes) on or off; the caller draws it.
    void setDebugVisible(bool visible) { debug_ = visible; }
    /// Whether the debug display is on.
    bool debugVisible() const { return debug_; }
    /// The bake's reflection probes' areas, as centres and half sizes.
    std::vector<std::pair<Vec3, Vec3>> reflectionAreas() const;
    /// Changes whenever the bake in use does.
    unsigned version() const { return version_; }

private:
    class Listener;
    class ProbeRenderer;

    /// Creates a reflection-probe system sized for @p probes probes, unbound.
    std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> createReflections(unsigned probes);
    /// Replaces the bake in use with these, made for @p settings.
    void use(const GiSettings& settings, std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections,
             const Diffuse& diffuse);
    /// Destroys the textures the diffuse light is read from.
    void destroyDiffuseTextures();

    Ogre::Root& root_;
    Ogre::SceneManager& scene_;
    Ogre::HlmsPbs& pbs_;
    Log log_;
    GiSettings settings_;
    double bakeSeconds_ = 0;
    std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections_;
    Diffuse diffuse_;
    std::array<Ogre::TextureGpu*, 4> diffuseTextures_{};
    std::unique_ptr<Listener> listener_;
    bool debug_ = false;
    unsigned version_ = 0;
    /// A bake being rendered: its reflection probes, then its diffuse probes.
    struct Pending {
        GiSettings settings;
        std::unique_ptr<Ogre::ParallaxCorrectedCubemapAuto> reflections;
        unsigned reflectionProbesDone = 0;
        ProbeOctree octree;
        /// The probes to render (the octree's not hanging ones), how many have been sent to
        /// render and how many have come back.
        std::vector<std::uint32_t> toRender;
        std::size_t sent = 0, collected = 0;
        /// Per probe: R, G and B as (a, b), the light along a normal n being a + b.n.
        std::vector<std::array<float, 12>> values;
        std::unique_ptr<ProbeRenderer> renderer;
        /// When rendering the diffuse probes began, for the log.
        std::chrono::steady_clock::time_point diffuseStart;
    } pending_;
    /// Places and renders the next reflection probe of the bake in progress.
    void renderNextReflectionProbe();
};
}
