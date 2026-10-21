#pragma once

#include "editor/AdaptiveIrradianceField.h"
#include "editor/Scene.h"
#include "runtime/AnimationLibrary.h"
#include <OgreMesh2.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Ogre {
class HlmsPbs;
class HlmsPbsDatablock;
class Item;
class Root;
class SceneManager;
class SceneNode;
class TextureGpu;
}

namespace throne {
/// The HlmsPbs custom pixel-shader pieces of a surface's @p translucency and subsurface
/// scattering width (in metres), empty when it has neither. Translucency is added to every light
/// the surface receives (directional, point, spot and Forward+): the pieces rename OGRE's BRDF
/// before it is declared and wrap it under its own name, so every light loop calls the wrapper.
/// Shadows and attenuation still multiply the result, so a thick surface's own shadow hides it;
/// thin ones (leaves, paper) lie within the shadow bias. Area lights do not use BRDF and add none.
/// The width is defined as THRONE_SUBSURFACE_WIDTH, which the depth prepass writes for the
/// viewport's subsurface scattering (media/pbs/MotionVectors).
std::string surfacePiece(const Translucency& translucency, float subsurfaceWidth);

/// Draws model files (FBX, read with ufbx) through OGRE for the Model component
/// (TDDja/09 8). Each file is read once into shared meshes and PBS materials;
/// placing it creates one Item per mesh instance under the given node.
///
/// Files are converted to metres, +Y up and right-handed. Materials follow the
/// metallic workflow: a base colour map (its alpha is opacity), a normal map, an
/// ORM map packed as glTF does (occlusion, roughness, metalness in R, G, B) and an
/// emissive map. A material with an FBX transparency colour is a clear dielectric: it
/// refracts the opaque pass and keeps its reflections, in render queue 200. One the
/// component gives absorption to is a volume instead, multiplying what is behind it by its
/// Beer-Lambert transmittance last in the opaque pass (queue 99), so glass in front of a
/// liquid refracts it. Lights and cameras in the file are left out.
///
/// Reading a file is split so the editor can keep drawing: parse() does the slow part
/// without OGRE, on any thread, and add() makes the OGRE objects on the main thread.
class ModelLibrary {
public:
    /// Receives problems reading a file.
    using Log = std::function<void(const std::string&)>;

    /// The OGRE objects of one placed model, and its bounds in the placing node's space.
    struct Placed {
        Ogre::SceneNode* root = nullptr;
        std::vector<Ogre::Item*> items;
        Vec3 lower, upper;
    };

    /// What parse() reads from a file: plain data, made without OGRE.
    struct Parsed {
        /// One material, with the texture files it uses (empty when it has none).
        struct Material {
            std::string name;
            std::filesystem::path diffuse, normal, orm, emissive;
            Vec3 colour{1, 1, 1};
            /// The emissive map's scale (ModelComponent::emission).
            float emission = 1;
            float opacity = 1;
            bool alphaTested = false, blended = false;
            /// A cut-out that is alpha-hashed instead of alpha-tested (ModelComponent::alphaHashing).
            bool alphaHashed = false;
            /// The custom pixel-shader pieces (ORM channels, absorption) it needs.
            std::string pieces;
            /// The render queue its items draw in (see Part::renderQueue); 99 when it absorbs.
            std::uint8_t renderQueue = 0;
        };
        /// One submesh: interleaved position, normal, tangent and uv (12 floats a vertex).
        struct Submesh {
            std::vector<float> vertices;
            std::vector<std::uint32_t> indices;
            std::size_t material = 0;
        };
        struct Mesh {
            std::vector<Submesh> submeshes;
            Vec3 lower, upper;
        };
        struct Part {
            std::size_t mesh = 0;
            Vec3 position, scale{1, 1, 1};
            float rotation[4] = {0, 0, 0, 1};
        };
        std::vector<Material> materials;
        std::vector<Mesh> meshes;
        std::vector<Part> parts;
        Vec3 lower, upper;
        /// Where the file's surfaces are, in its space: the centres of the surfaceCell-sized
        /// cubes its triangles touch (sampled at most surfaceCell apart), each with the volume
        /// of the largest object touching it (a connected piece of a placed mesh, as files
        /// often merge many objects into one mesh), for placing GI probes near geometry
        /// (TDDja/12 3).
        std::vector<SurfacePoint> surface;
        /// Problems found while reading, logged by add().
        std::vector<std::string> messages;
        /// An OGRE mesh file (.mesh, with its .skeleton), which add() reads through OGRE
        /// instead; the rest is then empty.
        std::filesystem::path ogreMesh;
    };

    /// @param scene Where placed models are created; must outlive the library.
    ModelLibrary(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log);
    /// Destroys the meshes and materials of every file read.
    ~ModelLibrary();
    ModelLibrary(const ModelLibrary&) = delete;
    ModelLibrary& operator=(const ModelLibrary&) = delete;

    /// The name a component's model is read under: its file, normal maps and material changes.
    static std::string key(const ModelComponent& model, const std::filesystem::path& file);
    /// Reads @p file as @p model asks; safe on a worker thread, as it touches no OGRE state.
    static Parsed parse(const std::filesystem::path& file, const ModelComponent& model);
    /// Makes the OGRE meshes and materials of @p parsed under @p key, logging its messages.
    void add(const std::string& key, const Parsed& parsed);
    /// Whether add() has made the model read under @p key.
    bool has(const std::string& key) const { return models_.count(key) != 0; }
    /// Places the model added under @p key below @p parent; empty when it has no meshes.
    /// Its items start hidden; reveal() shows them.
    Placed place(const std::string& key, Ogre::SceneNode& parent);
    /// Destroys what place() created; leaves @p placed empty.
    void remove(Placed& placed);
    /// Shows hidden placed items: every one whose materials have been shown already, and
    /// those bringing up to @p materials new ones. A material's first drawing compiles its
    /// shaders, so showing a few per frame spreads that work and keeps frames short.
    void reveal(unsigned materials);
    /// Placed items still hidden, and placed items hidden since all were last shown.
    std::size_t hidden() const { return hidden_.size(); }
    std::size_t hiddenTotal() const { return hiddenTotal_; }
    /// Sets the cubemap every model material reflects, or none.
    void setReflection(Ogre::TextureGpu* cubemap);
    /// The size of the cubes Parsed::surface is made of, in metres.
    static constexpr float surfaceCell = 0.5f;
    /// Parsed::surface of the model added under @p key; empty when there is none.
    const std::vector<SurfacePoint>& surface(const std::string& key) const;
    /// The skeleton and clips of the model added under @p key, or null when it has none. A
    /// skinned model is placed as one item, whose SkeletonInstance poses it.
    std::shared_ptr<const SkeletonAsset> skeleton(const std::string& key) const;

private:
    /// One submesh of a mesh instance of a file (so one material): its mesh and its
    /// placement in the file.
    struct Part {
        Ogre::MeshPtr mesh;
        /// The render queue its item draws in: 0 for opaque materials, 99 for absorbing
        /// volumes (last in the opaque pass), 200 for refracting ones (the refraction pass).
        /// Only opaque parts cast shadows.
        Ogre::uint8 renderQueue = 0;
        Vec3 position, scale{1, 1, 1};
        /// Rotation as a quaternion (x, y, z, w).
        float rotation[4] = {0, 0, 0, 1};
    };
    /// What one file was read into.
    struct Model {
        std::vector<Part> parts;
        Vec3 lower, upper;
        std::vector<Ogre::MeshPtr> meshes;
        std::vector<Ogre::HlmsPbsDatablock*> materials;
        std::vector<SurfacePoint> surface;
        std::shared_ptr<const SkeletonAsset> skeleton;
    };
    /// Reads an OGRE .mesh (and its skeleton) for add().
    void addOgreMesh(const std::string& key, const std::filesystem::path& file);

    Ogre::Root& root_;
    Ogre::SceneManager& scene_;
    Ogre::HlmsPbs& pbs_;
    Log log_;
    /// Placed items not shown yet, how many were hidden since none were, and the materials shown.
    std::vector<Ogre::Item*> hidden_;
    std::size_t hiddenTotal_ = 0;
    std::set<const void*> shownMaterials_;
    /// Models made, by key().
    std::map<std::string, Model> models_;
    Ogre::TextureGpu* reflection_ = nullptr;
};
}
