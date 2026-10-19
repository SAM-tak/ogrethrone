#pragma once

#include "core/Math.h"
#include <OgreMesh2.h>
#include <filesystem>
#include <functional>
#include <map>
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
/// Draws model files (FBX, read with ufbx) through OGRE for the Model component
/// (TDDja/09 8). Each file is read once into shared meshes and PBS materials;
/// placing it creates one Item per mesh instance under the given node.
///
/// Files are converted to metres, +Y up and right-handed. Materials follow the
/// metallic workflow: a base colour map (its alpha is opacity), a normal map, an
/// ORM map packed as glTF does (occlusion, roughness, metalness in R, G, B) and an
/// emissive map. A material with an FBX transparency colour is blended and keeps its
/// reflections; a nearly clear one (glass) also refracts, so its items are drawn in
/// render queue 200, after the scene they sample. Lights and cameras in the file are left out.
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

    /// @param scene Where placed models are created; must outlive the library.
    ModelLibrary(Ogre::Root& root, Ogre::SceneManager& scene, Ogre::HlmsPbs& pbs, Log log);
    /// Destroys the meshes and materials of every file read.
    ~ModelLibrary();
    ModelLibrary(const ModelLibrary&) = delete;
    ModelLibrary& operator=(const ModelLibrary&) = delete;

    /// Places the model in @p file under @p parent, reading the file on first use.
    /// @p directXNormals says which way the file's normal maps point green. Answers
    /// an empty Placed, after logging why, when the file cannot be read.
    Placed place(const std::filesystem::path& file, bool directXNormals, Ogre::SceneNode& parent);
    /// Destroys what place() created; leaves @p placed empty.
    void remove(Placed& placed);
    /// Sets the cubemap every model material reflects, or none.
    void setReflection(Ogre::TextureGpu* cubemap);

private:
    /// One mesh instance of a file: its mesh and its placement in the file.
    struct Part {
        Ogre::MeshPtr mesh;
        /// False when every material of the mesh is blended (glass casts no shadow).
        bool castShadows = true;
        /// True when any material of the mesh refracts; it is drawn in the refraction pass.
        bool refractive = false;
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
    };

    /// Reads @p file into a Model; answers false after logging when it cannot.
    bool load(const std::filesystem::path& file, bool directXNormals, Model& out);

    Ogre::Root& root_;
    Ogre::SceneManager& scene_;
    Ogre::HlmsPbs& pbs_;
    Log log_;
    /// Files read, by path and normal-map convention.
    std::map<std::string, Model> models_;
    Ogre::TextureGpu* reflection_ = nullptr;
};
}
