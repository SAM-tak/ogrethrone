#pragma once

#include "core/Math.h"
#include "audio/AudioComponents.h"
#include "editor/Lton.h"
#include "editor/SceneLighting.h"
#include "runtime/FramePipeline.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace throne {
/// A scene node's ID: a positive 64-bit Snowflake ID (see newNodeId()), so IDs
/// from different files do not meet (TDDja/06 3). 0 means no node; files may
/// hold any positive ID.
using NodeId = std::uint64_t;
/// Generates an identity for a node or script attachment.
NodeId newNodeId();

/// Node IDs from a scene or prefab's own node down through prefab instances,
/// outermost first (TDDja/06 3). Each ID is looked for anywhere below the node
/// before it, so a path stays valid when a subtree is moved into a new prefab.
/// The instances along it tell apart the copies of a prefab placed more than
/// once. An instance node and its prefab's root are one node, which either's
/// ID names.
using NodePath = std::vector<NodeId>;

/// A reference to a scene node (TDDja/05 4, 06 3.1).
struct NodeRef {
    /// ID of the level's scene instance, or 0 for the scene (or prefab) holding the reference.
    std::uint32_t scene = 0;
    /// The node's ID in that scene; 0 means no node.
    NodeId node = 0;
    /// Path below `node` when it is a prefab instance; empty for the node itself.
    NodePath inner;
    bool operator==(const NodeRef& other) const {
        return scene == other.scene && node == other.node && inner == other.inner;
    }
};

/// A value written for one behaviour field, overriding the script's default.
using FieldValue = std::variant<double, bool, std::string, NodeRef>;

/// One object-oriented script attachment: a script reference and its instance data.
/// Also one RenderEvent attachment, whose script publishes `RenderEvent` instead.
struct Behaviour {
    /// Project-relative path of a module^ publishing `Behaviour = def^{ ... }`.
    std::string script;
    /// Saved instance data; omitted fields retain the script's defaults.
    std::map<std::string, FieldValue> data;
    /// Attachment identity, retained across field changes and prefab overrides.
    NodeId id = newNodeId();
    bool enabled = true;
    /// Instance overrides; missing values inherit the script's defaults.
    std::optional<double> priority;
    std::array<std::optional<double>,3> phasePriorities;
};

/// Script defaults for ordering the same instance in different execution contexts.
struct BehaviourDefaults {
    double priority = 0;
    std::array<std::optional<double>,3> phasePriorities;
};
/// Resolves instance/phase overrides before script/phase defaults.
inline double effectivePriority(const Behaviour& b,const BehaviourDefaults& defaults,int phase) {
    if(b.phasePriorities[phase]) return *b.phasePriorities[phase];
    if(b.priority) return *b.priority;
    return defaults.phasePriorities[phase].value_or(defaults.priority);
}

/// Direct Behaviour/System attachments grouped into the named execution stage (TDDja/07).
/// A system has no transform; it reaches scene nodes only through field values.
struct System {
    /// Unique nonzero ID within the file that holds the system.
    std::uint32_t id = 0;
    /// Display name; 1 to 255 bytes.
    std::string name;
    std::vector<Behaviour> behaviour;
};

/// A sphere drawn around the node's origin.
struct SpherePrimitive {
    std::array<float, 3> colour{0.32f, 0.62f, 0.82f};
    float radius = 0.5f;
};
/// A Y-axis capsule; height includes both hemispheres.
struct CapsulePrimitive {
    std::array<float, 3> colour{0.86f, 0.53f, 0.25f};
    float radius = 0.5f, height = 2.0f;
};
/// A finite, double-sided visual rectangle on the local XZ plane.
struct PlanePrimitive {
    std::array<float, 3> colour{0.35f, 0.45f, 0.35f};
    float width = 10, depth = 10;
};
/// How a surface uses its alpha (the diffuse map's alpha, times opacity for hashing).
enum class AlphaMode {
    /// Alpha is ignored.
    Opaque,
    /// Cut out where alpha is below a threshold (foliage, fences): hard edges.
    Test,
    /// Each pixel kept with the probability of its alpha, so temporal anti-aliasing averages
    /// it into soft edges or see-through surfaces that still write depth, sort themselves and
    /// cast matching shadows.
    Hash,
};

/// Light passing through a thin surface from behind (leaves, paper, cloth, wax), added to each
/// light's contribution as Barre-Brisebois and Bouchard's (Frostbite, GDC 2011) approximation:
/// colour * scale * saturate(dot(V, -(L + N * distortion)))^power.
struct Translucency {
    /// The transmitted light's tint; black (the default) turns translucency off.
    Vec3 colour;
    /// How far the surface normal bends the light passing through ([0, 1]).
    float distortion = .2f;
    /// How tightly the light is focused around the direction away from it.
    float power = 4;
    float scale = 1;
    /// Whether it adds any light.
    bool enabled() const { return colour.x > 0 || colour.y > 0 || colour.z > 0; }
    /// Equality of every field.
    bool operator==(const Translucency& other) const {
        return colour == other.colour && distortion == other.distortion && power == other.power && scale == other.scale;
    }
    /// Negation of equality.
    bool operator!=(const Translucency& other) const { return !(*this == other); }
};

/// Authored PBS surface settings; primitive colour supplies the diffuse/base colour.
struct PbsMaterial {
    float roughness = 0.65f;
    std::array<float,3> fresnel{0.04f,0.04f,0.04f};
    /// A negative value selects the specular/Fresnel workflow, otherwise metallic [0,1].
    float metalness = -1;
    std::array<float,3> emissive{0,0,0};
    /// Project-relative textures; @ogre/ refers to the pinned OGRE sample media.
    std::string diffuseMap, normalMap, roughnessMap, specularMap, reflectionMap;
    float uvScale = 1;
    AlphaMode alpha = AlphaMode::Opaque;
    /// The alpha below which Test cuts the surface out.
    float alphaThreshold = 0.5f;
    /// The alpha of the whole surface, for Hash (multiplied by the diffuse map's).
    float opacity = 1;
    Translucency translucency;
    /// How far light spreads under the surface, in metres, blurring its lighting on screen by a
    /// skin-like profile (red furthest); 0 (the default) turns subsurface scattering off.
    float subsurfaceWidth = 0;
};
/// Supported OGRE light models. Rectangular lights use regular Forward.
enum class LightType { Directional, Point, Spot, AreaApprox, AreaLtc };
/// A light attached to the node's transform; direction is in node-local space.
struct LightComponent {
    LightType type = LightType::Point;
    std::array<float,3> colour{1,1,1};
    float power = 3.14159265f, radius = 10, threshold = 0.01f;
    /// Colour temperature in kelvin that tints colour like a black body (6500 K is
    /// white, lower is warmer); 0 leaves colour as it is.
    float temperature = 0;
    Vec3 direction{0,0,-1};
    float innerAngle = 30, outerAngle = 45;
    float width = 1, height = 1;
    bool enabled = true, twoSided = false, showEmitter = true;
    /// Whether a point or spot light takes one of the viewport's local shadow maps;
    /// directional lights always cast shadows.
    bool castShadows = false;
    /// A project-relative mask texture, used by AreaApprox only.
    std::string mask;
};

/// What a Model component changes in one of its file's materials (TDDja/09 8).
struct ModelMaterial {
    /// The material's name in the file.
    std::string name;
    /// Beer-Lambert absorption coefficients per metre (red, green, blue); a blended material
    /// with any is a volume (a liquid, tinted glass) that multiplies what is seen through it
    /// by exp(-absorption * thickness * cos(view angle)).
    Vec3 absorption;
    /// Distance through the volume, in metres, along the surface normal.
    float thickness = .05f;
    /// Light the material passes through from behind (foliage).
    Translucency translucency;
    /// Subsurface scattering width in metres (PbsMaterial::subsurfaceWidth).
    float subsurfaceWidth = 0;
    /// A project-relative base colour map replacing the file's (files often name their texture
    /// only in the material's name); empty keeps the file's.
    std::string diffuseMap;
    /// Equality, to tell when the drawn model must be rebuilt.
    bool operator==(const ModelMaterial& other) const {
        return name == other.name && absorption == other.absorption && thickness == other.thickness &&
               translucency == other.translucency && subsurfaceWidth == other.subsurfaceWidth && diffuseMap == other.diffuseMap;
    }
};

/// A model file (FBX) drawn at the node: its meshes with their own hierarchy and
/// PBS materials (TDDja/09 8). Lights and cameras in the file are not imported.
struct ModelComponent {
    /// Project-relative path of the model file.
    std::string path;
    /// Whether the file's normal maps use DirectX's convention (green pointing down).
    bool directXNormals = true;
    /// Radiance of its emissive maps' full value. Files carry no strength (lamps are 1),
    /// which is dim beside a sun.
    float emission = 1;
    /// Whether cut-out materials (diffuse maps with alpha) are alpha-hashed instead of
    /// alpha-tested: soft edges once temporal anti-aliasing averages them.
    bool alphaHashing = false;
    /// Changes to the file's materials, by material name.
    std::vector<ModelMaterial> materials;
    /// Equality, to tell when the drawn model must be rebuilt.
    bool operator==(const ModelComponent& other) const {
        return path == other.path && directXNormals == other.directXNormals && emission == other.emission &&
               alphaHashing == other.alphaHashing && materials == other.materials;
    }
    /// Negation of equality.
    bool operator!=(const ModelComponent& other) const { return !(*this == other); }
};

/// An instance of an OGRE ParticleFX2 template attached to this node's transform.
struct ParticleSystemComponent {
    std::string definition = "Examples/PurpleFountain";
    /// Optional project-relative .particle2 file; empty uses the built-in OGRE examples.
    std::string script;
    bool enabled = true;
    /// Whether renderer resources can be reused for this authored instance.
    bool operator==(const ParticleSystemComponent& other) const {
        return definition==other.definition && script==other.script && enabled==other.enabled;
    }
    /// Negation of equality.
    bool operator!=(const ParticleSystemComponent& other) const { return !(*this==other); }
};

/// A viewpoint (TDDja/07): the node's position and orientation, looking along its local -Z
/// with +Y up, through a perspective projection. The editor's view can be set from it, or
/// written into it.
/// How a camera's picture is anti-aliased.
enum class AntiAliasing {
    /// Temporal anti-aliasing: jittered frames blended into a reprojected history. Alpha hashing
    /// and the screen-space effects' noise rely on it to average out. Orthographic views get SMAA.
    Temporal,
    /// SMAA 1x on the tone-mapped picture: no history, so noise stays as it is.
    Smaa,
    /// None, for comparison and debugging.
    None,
};

/// Plays a skeletal clip of the node's Model (TDDja/07 5.3): the Animation stage advances the
/// time and samples the pose on the simulation side; the renderer poses the model's bones.
struct AnimatorComponent {
    /// The clip's name in the model's skeleton; empty shows the rest pose.
    std::string clip;
    /// Seconds into the clip; the Animation stage advances it by speed while playing.
    float time = 0;
    float speed = 1;
    /// Whether the clip repeats; otherwise it holds its last pose.
    bool loop = true, playing = true;
    /// Equality of every field.
    bool operator==(const AnimatorComponent& other) const {
        return clip == other.clip && time == other.time && speed == other.speed && loop == other.loop && playing == other.playing;
    }
};

struct CameraComponent {
    /// Vertical field of view, in degrees.
    float fieldOfView = 45;
    /// Near and far clip distances, in metres.
    float nearClip = .1f, farClip = 1000;
    /// Depth of field of a 35 mm full-frame lens of this field of view: the distance in focus,
    /// in metres, and the f-number (0: everything in focus).
    float focusDistance = 10, fStop = 0;
    AntiAliasing antiAliasing = AntiAliasing::Temporal;
    /// Equality of every field.
    bool operator==(const CameraComponent& other) const {
        return fieldOfView == other.fieldOfView && nearClip == other.nearClip && farClip == other.farClip &&
               focusDistance == other.focusDistance && fStop == other.fStop && antiAliasing == other.antiAliasing;
    }
    /// Negation of equality.
    bool operator!=(const CameraComponent& other) const { return !(*this == other); }
};

/// Material and local offset shared by collision shapes. Density determines mass.
struct Collider {
    Vec3 centre;
    float density = 1, friction = 0.6f, restitution = 0.2f;
    bool sensor = false;
};
/// An axis-aligned box in body-local space, scaled with the node.
struct BoxCollider : Collider { Vec3 size{1, 1, 1}; };
/// A sphere, scaled uniformly by the largest node scale component.
struct SphereCollider : Collider { float radius = 0.5f; };
/// A Y-axis capsule; radial scale uses max(X,Z), height scale uses Y.
struct CapsuleCollider : Collider { float radius = 0.5f, height = 2; };
/// A finite, upward-facing triangle mesh. Only static and kinematic bodies may own it.
struct PlaneCollider : Collider { float width = 10, depth = 10; };
/// Box3D's three motion modes. Colliders without a RigidBody create a static body.
enum class BodyType { Static, Kinematic, Dynamic };
/// Simulation state and settings. Angular velocity is expressed in degrees/second.
struct RigidBody {
    BodyType type = BodyType::Dynamic;
    Vec3 linearVelocity, angularVelocity;
    float linearDamping = 0, angularDamping = 0.05f, gravityScale = 1;
    bool bullet = false;
};

/// What an instance changes in one attachment of its prefab, found by the attachment's
/// ID (TDDja/07 4). Unset settings and fields keep the prefab's.
struct AttachmentOverride {
    NodeId id = 0;
    std::optional<std::string> script;
    std::optional<bool> enabled;
    /// Priority overrides; an empty inner value returns to the script's default.
    std::optional<std::optional<double>> priority;
    std::array<std::optional<std::optional<double>>, 3> phasePriorities;
    /// Field values set over the attachment's data.
    std::map<std::string, FieldValue> data;

    /// Whether nothing is set.
    bool empty() const;
    /// Writes what is set into @p behaviour.
    void applyTo(Behaviour& behaviour) const;
};

/// What an instance changes in one attachment list (Behaviour or Render Event) of a node
/// of its prefab. has_value() and reset() are those of std::optional, so code over all
/// properties can treat it as one.
struct AttachmentsOverride {
    std::vector<AttachmentOverride> changed;
    /// Attachments the instance adds after the prefab's.
    std::vector<Behaviour> added;
    /// IDs of the prefab's attachments the instance removes.
    std::vector<NodeId> removed;
    /// The attachments' order, by ID, when the instance moves them; empty keeps the prefab's.
    /// Those it does not list (added to the prefab later) follow, in their own order.
    std::vector<NodeId> order;

    bool has_value() const { return !changed.empty() || !added.empty() || !removed.empty() || !order.empty(); }
    void reset() { *this = {}; }
    /// The override of attachment @p id, added empty when there is none.
    AttachmentOverride& change(NodeId id);
    /// Adds @p from on top of this: its settings and fields win, its additions and removals join.
    void merge(const AttachmentsOverride& from);
    /// Removes what @p piece sets, as Overrides::subtract().
    void subtract(const AttachmentsOverride& piece);
    /// Applies this to @p list: removals, changes, then additions. Returns the removals
    /// and changes whose attachment @p list does not have.
    AttachmentsOverride apply(std::vector<Behaviour>& list) const;
};

/// Property values an instance gives one node of its prefab (TDDja/06 4); unset ones
/// keep the prefab's. Component properties apply only where the node has the component.
struct NodeOverride {
    /// The node, relative to the instance; empty for the instance node itself (the root).
    NodePath path;
    std::optional<std::string> name;
    /// Transform properties.
    std::optional<Vec3> position, rotation, scale;
    /// BoxPrimitive properties.
    std::optional<std::array<float, 3>> colour;
    /// Overrides of existing render/physics components.
    std::optional<SpherePrimitive> sphere;
    std::optional<CapsulePrimitive> capsule;
    std::optional<PlanePrimitive> plane;
    std::optional<PbsMaterial> material;
    std::optional<LightComponent> light;
    std::optional<ParticleSystemComponent> particles;
    std::optional<ModelComponent> model;
    std::optional<CameraComponent> camera;
    std::optional<AnimatorComponent> animator;
    std::optional<AudioSourceComponent> audioSource;
    std::optional<AudioListenerComponent> audioListener;
    std::optional<AcousticGeometryComponent> acousticGeometry;
    std::optional<BoxCollider> boxCollider;
    std::optional<SphereCollider> sphereCollider;
    std::optional<CapsuleCollider> capsuleCollider;
    std::optional<PlaneCollider> planeCollider;
    std::optional<RigidBody> rigidBody;

    /// Changes to the attachment lists, by attachment.
    AttachmentsOverride behaviour, renderEvent;

    /// Whether no property is set.
    bool empty() const;
};

/// Field values an instance gives one behaviour of a system in its prefab.
struct FieldOverride {
    /// The prefab instance the system comes from, relative to the instance; empty for
    /// the instance's own prefab.
    NodePath instance;
    std::uint32_t system = 0;
    std::size_t behaviour = 0;
    std::map<std::string, FieldValue> fields;
};

struct AddedNode;

/// A system an instance's overrides remove: its ID in the instance it comes from.
struct RemovedSystem {
    /// That instance, relative to the overriding one; empty for the instance itself.
    NodePath instance;
    std::uint32_t system = 0;
};

/// What one instance changes in what it instantiates (TDDja/06 4).
struct Overrides {
    std::vector<NodeOverride> nodes;
    std::vector<FieldOverride> systems;
    /// Nodes the instance adds, parents before children.
    std::vector<AddedNode> added;
    /// Nodes the instance removes, each with all below it; relative to the
    /// instance and never empty (the instance node itself stays).
    std::vector<NodePath> removed;
    /// Systems the instance adds, with IDs from newSystemId().
    std::vector<System> addedSystems;
    std::vector<RemovedSystem> removedSystems;

    /// Whether nothing is overridden.
    bool empty() const;
    /// Returns the override for @p path, adding an empty one when there is none.
    NodeOverride& node(const NodePath& path);
    /// Returns the field override for that behaviour, adding an empty one when there is none.
    FieldOverride& fieldsOf(const NodePath& instance, std::uint32_t system, std::size_t behaviour);
    /// Removes what @p piece overrides: its properties, field values, and added or
    /// removed nodes and systems (matched by path and ID). Emptied entries go too.
    void subtract(const Overrides& piece);
};

/// The Transform component (TDDja/07 3): placement relative to the nearest ancestor
/// with a Transform; a node without one passes its parent's placement on unchanged.
/// Also a scene instance's placement in its level.
struct Transform {
    Vec3 position;
    /// Euler angles in degrees, applied about X, then Y, then Z.
    Vec3 rotation;
    /// Per-axis scale; each component is at least 0.001.
    Vec3 scale{1, 1, 1};
};

/// The BoxPrimitive component (TDDja/07 3): a unit cube drawn at the node.
struct BoxPrimitive {
    /// Linear RGB colour with components in [0, 1].
    std::array<float, 3> colour{0.32f, 0.62f, 0.82f};
};

/// One node of a scene hierarchy (TDDja/07 3): an ID, a name and a parent, and the
/// components it has. A node naming a prefab is a prefab instance and becomes the
/// prefab's root (TDDja/06 3): it gives the root its name and Transform, everything
/// else comes from the prefab, and it has no children of its own.
struct SceneNode {
    /// Unique nonzero ID; see NodeId.
    NodeId id = 0;
    /// Display name; 1 to 255 bytes.
    std::string name;
    /// ID of the parent node, or 0 for the scene root.
    NodeId parent = 0;
    std::optional<Transform> transform;
    /// Unused by an instance, whose root's comes from the prefab or its overrides.
    std::optional<BoxPrimitive> box;
    /// Render and physics components; collision shapes on a node share its body.
    std::optional<SpherePrimitive> sphere;
    std::optional<CapsulePrimitive> capsule;
    std::optional<PlanePrimitive> plane;
    std::optional<PbsMaterial> material;
    std::optional<LightComponent> light;
    std::optional<ParticleSystemComponent> particles;
    std::optional<ModelComponent> model;
    std::optional<CameraComponent> camera;
    std::optional<AnimatorComponent> animator;
    std::optional<AudioSourceComponent> audioSource;
    std::optional<AudioListenerComponent> audioListener;
    std::optional<AcousticGeometryComponent> acousticGeometry;
    std::optional<BoxCollider> boxCollider;
    std::optional<SphereCollider> sphereCollider;
    std::optional<CapsuleCollider> capsuleCollider;
    std::optional<PlaneCollider> planeCollider;
    std::optional<RigidBody> rigidBody;

    /// The Behaviour component: independent script/data instances, in attachment order.
    std::vector<Behaviour> behaviour;
    /// The RenderEvent component: script instances run on the render thread before and
    /// after OGRE draws, reading the render snapshot only.
    std::vector<Behaviour> renderEvent;
    /// Project-relative path of the prefab this node instantiates; empty for a plain node.
    std::string prefab;
    /// What this instance changes in the prefab.
    Overrides overrides;
};

/// Calls @p visit(name, each.member...) for every component an override replaces whole
/// (all but Transform and BoxPrimitive), on SceneNodes and NodeOverrides alike.
template <class F, class... T> void forEachComponent(F&& visit, T&... each) {
    visit("Particle System", each.particles...);
    visit("Model", each.model...);
    visit("Camera", each.camera...);
    visit("Animator", each.animator...);
    visit("Audio Source", each.audioSource...);
    visit("Audio Listener", each.audioListener...);
    visit("Acoustic Geometry", each.acousticGeometry...);
    visit("PBS Material", each.material...);
    visit("Light", each.light...);
    visit("Sphere Primitive", each.sphere...);
    visit("Capsule Primitive", each.capsule...);
    visit("Plane Primitive", each.plane...);
    visit("Box Collider", each.boxCollider...);
    visit("Sphere Collider", each.sphereCollider...);
    visit("Capsule Collider", each.capsuleCollider...);
    visit("Plane Collider", each.planeCollider...);
    visit("RigidBody", each.rigidBody...);
}

/// Calls @p visit(name, each.member...) for every property of NodeOverrides.
template <class F, class... T> void forEachProperty(F&& visit, T&... each) {
    visit("Name", each.name...);
    visit("Position", each.position...);
    visit("Rotation", each.rotation...);
    visit("Scale", each.scale...);
    visit("Colour", each.colour...);
    visit("Behaviour", each.behaviour...);
    visit("Render Event", each.renderEvent...);
    forEachComponent(visit, each...);
}

/// Sets in @p kept what @p set sets, as one property of forEachProperty().
template <class T> void overlay(std::optional<T>& kept, const std::optional<T>& set) {
    if (set) kept = set;
}
inline void overlay(AttachmentsOverride& kept, const AttachmentsOverride& set) { kept.merge(set); }

/// Unsets in @p kept what @p gone sets, as one property of forEachProperty().
template <class T> void unset(std::optional<T>& kept, const std::optional<T>& gone) {
    if (gone) kept.reset();
}
inline void unset(AttachmentsOverride& kept, const AttachmentsOverride& gone) { kept.subtract(gone); }

/// Writes the properties @p layer sets into @p node. Component properties go only where
/// the node has the component; those that do not are returned, at @p layer's path.
NodeOverride applyOverride(SceneNode& node, const NodeOverride& layer);

/// A node an instance adds to what it instantiates (TDDja/06 4). Its path is the
/// instance's followed by its ID, wherever its parent is.
struct AddedNode {
    /// The parent, relative to the instance; empty for the instance node itself.
    NodePath parent;
    /// The node; its `parent` is unused. It may be an instance itself.
    SceneNode node;
};

/// The systems of one file (a prefab, scene or level), in run order.
struct SystemSet {
    std::vector<System> systems;
    std::uint32_t nextSystemId = 1;

    /// Returns the system with @p id, or nullptr.
    System* findSystem(std::uint32_t id);
    /// Appends a system without behaviour and returns it.
    System& addSystem(const std::string& name);
    /// Removes the system.
    void removeSystem(std::uint32_t id);
};

/// Nodes and systems: what prefabs and scenes share (TDDja/05 2, 3). A prefab
/// file has exactly one root node, or is a variant (TDDja/06 3.2): no nodes, only
/// a base prefab and overrides of it.
struct Prefab : SystemSet {
    /// Nodes with every parent before its children; siblings keep this order.
    std::vector<SceneNode> nodes;
    /// For a variant, the project-relative path of the prefab it is based on.
    std::string base;
    /// For a variant, what it changes in its base.
    Overrides overrides;

    /// Returns the node with @p id, or nullptr.
    SceneNode* findNode(NodeId id);
    /// @copydoc findNode
    const SceneNode* findNode(NodeId id) const;
    /// Appends a cube node (a Transform half a unit up and a BoxPrimitive) named @p name
    /// under @p parent, with a new ID, and returns it.
    SceneNode& addNode(const std::string& name, NodeId parent = 0);
    /// Removes the node and all its descendants.
    void removeNode(NodeId id);
    /// Whether @p id is @p ancestor or lies below it.
    bool isWithin(NodeId id, NodeId ancestor) const;
    /// Moves @p id (with its subtree) under @p parent, just before its child
    /// @p before, or as the last child when @p before is 0.
    /// @return false when that would make a node its own ancestor, or @p before
    ///         is @p id or not a child of @p parent.
    bool reparent(NodeId id, NodeId parent, NodeId before = 0);

    /// Whether this is a variant of another prefab.
    bool variant() const { return !base.empty(); }
    /// Reads `ogrethrone-prefab` LTON.
    static Prefab load(const std::filesystem::path& path);
    /// Writes `ogrethrone-prefab` LTON.
    void save(const std::filesystem::path& path) const;

protected:
    /// Reads the shared fields from a parsed file.
    void read(const LhatTable& root);
    /// Writes the shared fields into @p root.
    void write(lton::Writer& out, LhatValue root) const;
};

/// A prefab extended with scene-wide data (TDDja/05 2: the only difference).
struct Scene : Prefab {
    /// The scene's environment and baked global illumination (TDDja/13).
    SceneLighting lighting;

    /// Reads `ogrethrone-scene` LTON.
    static Scene load(const std::filesystem::path& path);
    /// Writes `ogrethrone-scene` LTON.
    void save(const std::filesystem::path& path) const;
    /// Returns a scene with two cubes, the first named SpinningCube.
    static Scene example();
};

/// Returns a new NodeId: a Snowflake ID (core/Ids.h) counted from 2026-01-01 UTC. Not thread safe.
NodeId newNodeId();
/// Returns a random system ID from 0x80000000 up, for systems an instance adds:
/// a file numbers its own systems from 1, so the two do not meet.
std::uint32_t newSystemId();
/// Writes @p overrides as an LTON table; shared by prefab instances and scene instances.
LhatValue writeOverrides(lton::Writer& out, const Overrides& overrides);
/// Reads what writeOverrides wrote (nil^ reads as no overrides). @throws lton::Invalid on malformed data.
Overrides readOverrides(LhatValue value);
/// Writes node or system attachments, including their execution stage.
LhatValue writeBehaviourList(lton::Writer& out, const std::vector<Behaviour>& behaviour);
/// Reads attachments; nil means an empty list. One without an ID gets its place in the list (from 1).
std::vector<Behaviour> readBehaviourList(LhatValue value);
/// Writes an instance's changes to an attachment list.
LhatValue writeAttachmentsOverride(lton::Writer& out, const AttachmentsOverride& overrides);
/// Reads what writeAttachmentsOverride wrote; nil means no changes.
AttachmentsOverride readAttachmentsOverride(LhatValue value);
/// Writes @p systems as an LTON sequence; shared by prefabs, scenes and levels.
LhatValue writeSystems(lton::Writer& out, const std::vector<System>& systems);
/// Reads what writeSystems wrote. @throws lton::Invalid on malformed data.
std::vector<System> readSystems(LhatValue value, std::uint32_t& nextId);
}
