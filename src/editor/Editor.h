#pragma once
#include "input/OisInput.h"

#include "editor/EditorPlatform.h"
#include "editor/Expansion.h"
#include "editor/Environment.h"
#include "editor/AdaptiveIrradianceField.h"
#include "editor/GlobalIllumination.h"
#include "editor/PbsExtensions.h"
#include "editor/Model.h"
#include "editor/Tasks.h"
#include "editor/PrimitiveGeometry.h"
#include "editor/References.h"
#include "editor/Project.h"
#include "script/ScriptRuntime.h"
#include "runtime/PhysicsWorld.h"
#include <OgreMaterial.h>
#include <OgreVector3.h>
#include <imgui.h>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <string>
#include <vector>

namespace Ogre {
class Root;
class Window;
class SceneManager;
class Camera;
class SceneNode;
class ManualObject;
class TextureGpu;
class CompositorWorkspace;
class ImguiManager;
class CompositorPassImguiProvider;
class HlmsPbs;
class HlmsPbsDatablock;
class Light;
class AtmosphereNpr;
class ParticleSystem2;
class HlmsUnlit;
class HlmsUnlitDatablock;
}

namespace throne {
/// Drag-and-drop payload of a node a file holds itself: the view instance it is
/// shown in, and its ID.
struct DragNode {
    int instance;
    NodeId id;
};

/// Dear ImGui level editor and project launcher drawn over the OGRE window.
///
/// The editor works on a World: the level's systems plus the scenes it loads.
/// Panels: Scene (node hierarchies), Systems, Inspector, Viewport and Browser.
///
/// Without an open project the window shows the launcher (TDDja/03 4). With one,
/// the level is rendered into an offscreen texture shown in the viewport panel;
/// the window itself is cleared and covered by ImGui. Requires HlmsUnlit to be
/// registered and an ImGui context to exist for the editor's lifetime.
class Editor {
public:
    /// Installs the ImGui compositor pass, the viewport texture and both workspaces.
    /// @param root Initialized OGRE root with HlmsUnlit registered.
    /// @param window Window that receives the editor UI.
    /// @param scene Scene manager holding the edited objects.
    /// @param camera Camera used for the viewport.
    /// @param project Project folder to open at once; empty to show the launcher.
    /// @param projectList File holding the known projects; empty to keep the list in memory only.
    Editor(Ogre::Root& root, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera,
           const std::filesystem::path& project, std::filesystem::path projectList);
    /// Removes workspaces, scene objects and editor resources.
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    /// Builds the UI and runs a complete frame, including main-thread OGRE rendering.
    /// @param input Optional input replay hook, called after platform polling and before NewFrame.
    /// Returns false when the renderer requests shutdown.
    bool frame(float deltaSeconds, const std::function<void()>& input = {});
    /// Physical joystick/gamepad states captured before this frame's UI and simulation.
    const OisInput& input() const { return input_; }
    /// Wall-clock milliseconds for one completed frame. With frame delay, scripts and
    /// physics run alongside rendering; render/present includes driver/GPU/display waits,
    /// so these are not GPU timestamps.
    struct FrameTimings {
        double frameMs = 0, uiMs = 0, scriptMs = 0, snapshotMs = 0, physicsMs = 0;
        double renderSyncMs = 0, renderMs = 0, waitMs = 0;
        std::size_t nodes = 0, bodies = 0, particleSystems = 0, particles = 0;
    };
    /// The last completed frame, for profiling without FPS averaging or fixed-delta assumptions.
    const FrameTimings& frameTimings() const { return timings_; }
    /// Changes the user's viewport VSync preference; hidden views still force VSync on.
    void setEditorVSync(bool enabled);
    /// The user's preference, independent of the currently applied window setting.
    bool editorVSync() const { return preferences_.vsync; }
    /// Whether the scene workspace is enabled for this frame.
    bool sceneViewVisible() const;
    /// Selects OGRE's project and level rendering settings in the Inspector.
    void selectRenderer();
    /// Enables/disables shadow casting while preserving direct and ambient lighting.
    void setShadowsEnabled(bool enabled);
    /// Enables/disables temporal anti-aliasing (on by default); smoke checks that compare
    /// frames exactly turn it off.
    void setTemporalAntialiasing(bool enabled);
    /// Shows or hides the procedural sky without modifying scene lights or physics.
    void setSkyboxEnabled(bool enabled);
    /// Bakes the level's global illumination into its project folder once textures have streamed in.
    void requestGlobalIlluminationBake();
    /// Whether long work (reading models or environments, baking) is still running.
    bool busy() const { return tasks_.busy(); }
    /// Downloads the rendered viewport to a portable RGB image for visual smoke checks.
    void captureViewport(const std::filesystem::path& path);
    /// Opens a project-relative level; returns false if loading fails or Play is active.
    bool openLevel(const std::string& path);
    /// Sets the view to the first node with a Camera component, in scene-instance and
    /// hierarchy order, and clears the selection (no outline or gizmo in the picture);
    /// returns false when the level has none.
    bool viewFromFirstCamera();
    /// Selects the viewport's local-light culling algorithm.
    void setForwardClustered(bool enabled) { doc_.world.level.rendering.clustered=enabled; }
    /// Shows the Forward+ light-density debug overlay.
    void setClusterDebug(bool enabled) { doc_.world.level.rendering.debugClusters=enabled; }
    /// Preview override: -1 authored, 0 textured approximation, 1 LTC.
    void setAreaLightModel(int model) { areaLightModel_=model; }
    /// Enables live particle previews outside Play; changing this resets the preview.
    void setParticlePreview(bool enabled) { particlePreview_=enabled; }
    /// Preview blending for smoke: 0 alpha blending, 1 alpha hashing.
    void setParticleBlending(int mode) { particleBlending_=mode; }
    /// Whether a Play session currently owns the expanded world.
    bool playing() const { return runtime_ != nullptr; }
    /// Whether Play is suspended while viewport navigation and rendering remain active.
    bool paused() const { return runtime_ != nullptr && paused_; }
    /// True once the user chose to close (File > Exit or the window's close button),
    /// after resolving unsaved changes.
    bool shouldClose() const { return close_; }
    /// The window hook; set its modalFrame to keep drawing during moves and resizes.
    EditorPlatform& platform() { return platform_; }

private:
    /// What to do once unsaved changes are resolved.
    enum class Leave { Exit, CloseProject };
    /// A known project as the launcher lists it.
    struct Known {
        std::filesystem::path root;
        std::string name, lastOpened;
        bool found;
    };

    /// Opens the project at @p path, adds it to the list and loads its main level.
    /// Failures are shown in the launcher.
    void openProject(const std::filesystem::path& path);
    /// Returns to the launcher, discarding the open level.
    void closeProject();
    /// Writes the project's lhat-host.json when it differs from the current bindings (TDDja/03 3.4).
    void writeHostApi();
    /// Saves the project list and refreshes known_ from it.
    void projectsChanged();
    /// Draws the launcher: new, import, and the known projects.
    void launcher();
    /// OGRE objects that display one node of view_.
    struct RenderObject {
        std::size_t instance, index;
        Ogre::SceneNode* node;
        Ogre::ManualObject* mesh;
        Ogre::HlmsPbsDatablock* material;
        std::string materialName;
        PrimitiveShape shape;
        Vec3 lower, upper;
        float uvScale = 1;
        Ogre::Light* light = nullptr;
        Ogre::SceneNode* lightNode = nullptr;
        Ogre::SceneNode* emitterNode = nullptr;
        Ogre::ManualObject* emitter = nullptr;
        Ogre::HlmsUnlitDatablock* emitterMaterial = nullptr;
        std::optional<PbsMaterial> surface;
        PrimitiveShape emitterShape;
        Ogre::ParticleSystem2* particles = nullptr;
        /// The Model component last placed, and what placing it created.
        std::optional<ModelComponent> model;
        ModelLibrary::Placed placedModel;
        /// The library key the model is read under; empty when there is nothing to place.
        std::string modelKey;
        std::optional<ParticleSystemComponent> particleConfig;

    };
    /// Everything the editor edits; one undo step is a copy of it.
    struct Document {
        World world;
        /// Project-wide game output settings participate in Save and Undo with the document.
        GameRendering gameRendering;
        /// Prefabs opened for editing (TDDja/06 6), by path; saved with the level.
        std::map<std::string, Prefab> prefabs;
    };
    /// One step of the drill-down (TDDja/06 6).
    struct Context {
        enum class Kind { Level, Scene, Prefab } kind;
        /// The scene or prefab file; empty for the level.
        std::string path;
    };
    /// What the inspector shows: a node of view_, a system, or a scene instance of the level.
    struct Selection {
        enum class Kind { None, Node, System, Scene, EngineSystem, Level } kind = Kind::None;
        /// Index into view_.instances; -1 for a system of the level itself.
        int instance = -1;
        /// The node's path; for a system, the prefab instance it came from.
        NodePath path;
        /// The system's ID.
        std::uint32_t system = 0;
        /// Name of a built-in engine system, independent of authored System IDs.
        std::string engine;
    };
    /// Where an edit of one node is written: the node itself (it belongs to the
    /// file being edited), or a property override of the instance that contains it.
    struct NodeTarget {
        SceneNode* node = nullptr;
        Overrides* overrides = nullptr;
        NodePath relative;
        /// When @p node was added by an instance, the overrides holding it.
        Overrides* addedTo = nullptr;
    };
    /// Where an edit of one system is written: the system itself, or field overrides.
    struct SystemTarget {
        System* system = nullptr;
        Overrides* overrides = nullptr;
        NodePath instance;
        /// When @p system was added by an instance, the overrides holding it.
        Overrides* addedTo = nullptr;
    };
    /// A script's editable fields, refreshed when the file changes.
    struct CachedFields {
        std::filesystem::file_time_type stamp;
        std::vector<FieldInfo> fields;
        BehaviourInfo behaviour;
        std::string error;
        bool loaded = false;
    };

    /// The scene or prefab of the current context, or nullptr at the level.
    Prefab* content();
    /// The file whose nodes view instance @p instance shows: content(), or at the
    /// level that instance's scene; nullptr when there is none.
    Prefab* fileOf(int instance);
    /// Expands what the current context shows into view_, and rebuilds the render
    /// objects when the node structure changed. Skipped while playing.
    void refreshView();
    /// Enters the drill-down step @p context at the start of the next frame.
    void enter(Context context);
    /// Where an edit of node @p path of view instance @p instance goes.
    NodeTarget targetOf(int instance, const NodePath& path);
    /// targetOf() without looking into the nodes instances added.
    NodeTarget fileTarget(int instance, const NodePath& path);
    /// Where an edit of the selected system goes.
    SystemTarget systemTarget();
    /// The selected node of view_, or nullptr.
    const ExpandedNode* selectedNode();
    /// The selected system of view_ (or of the level), or nullptr.
    const System* selectedSystem();
    /// The selected scene instance of the level, or nullptr.
    SceneInstance* selectedScene();
    /// Writes @p value into the selected node through targetOf(); @p changed says which properties.
    void writeNode(const SceneNode& value, unsigned changed);
    /// Recreates all render objects from view_.
    void rebuildScene();
    /// Destroys every render object and its material.
    void destroyRenderObjects();
    /// Copies node transforms and colours to the render objects and the selection outline.
    void syncTransforms(const LevelExpansion* snapshot = nullptr);
    /// Applies persisted lighting settings, including the Forward+ method.
    void syncLighting(const LevelExpansion& shown);
    /// Offsets the camera by this frame's sub-pixel jitter and hands the temporal
    /// anti-aliasing and motion blur passes their parameters; the offset is removed after
    /// rendering.
    void jitterCamera();
    /// Moves the sky OGRE made into the queue after opaque geometry.
    void drawSkyAfterOpaque();
    /// Loads one project texture with a project-qualified cache key; masks share an R8 pool.
    Ogre::TextureGpu* projectTexture(const std::string& path, int kind, bool mask = false);
    /// Replaces the mipmaps of an environment cubemap with GGX-prefiltered radiance.
    void prefilter(Ogre::TextureGpu& cubemap);
    /// The cubemap surfaces without their own reflection map reflect: the environment's,
    /// unless baked reflection probes are in use.
    Ogre::TextureGpu* environmentReflection() const;
    /// The scene whose environment and global illumination the view shows (TDDja/13): the
    /// scene being edited, else the level's first scene; empty when there is none.
    std::string lightingScenePath() const;
    /// That scene's lighting, or the defaults when it is not loaded.
    const SceneLighting& lighting() const;
    /// The view instance whose scene's bake is shown: the first; -1 inside a prefab.
    int giInstance() const;
    /// How view instance @p instance is placed (its scene root's world transform).
    Ogre::Matrix4 instanceTransform(int instance) const;
    /// The project folder holding the baked global illumination of the scene shown.
    std::filesystem::path giFolder() const;
    /// Loads the standard ParticleFX2 resources and any project particle definitions.
    void loadParticleResources(const std::string& script);
    /// Reconciles particle instances after transforms; template/enable changes reset shared pools.
    void syncParticles(const LevelExpansion& shown);
    /// Clears all live particles and instance handles, including shared definition state.
    void clearParticles();
    /// Records the current world for undo before an edit.
    void checkpoint();
    /// Records @p before for undo when an edit that started from it has finished.
    void commit(Document before);
    /// Restores the previous world state, if any.
    void undo();
    /// Reapplies the most recently undone state, if any.
    void redo();
    /// Adds a cube to the edited scene or prefab, under the selected node when it is
    /// one of its own, and selects it.
    void addNode(PrimitiveKind kind = PrimitiveKind::Box);
    /// Adds an instance of the prefab at newPrefabPath_ to the edited scene or prefab,
    /// creating an empty prefab when the file is absent.
    void addInstance();
    /// Adds a system to the level (at the Level step) or to the edited scene or prefab.
    void addSystem();
    /// Adds an instance of the scene at newScenePath_ to the level, creating the file when absent.
    void addScene();
    /// Removes scene instance @p id from the level.
    void removeScene(std::uint32_t id);
    /// Adds @p node, with a new ID, where the selection says, and selects it: into the
    /// overrides of the instance holding the selected node (or of the selected scene
    /// instance, or of an edited variant), else into the edited file under the
    /// selected plain node or a prefab's root. Returns false, with a log line, when
    /// there is nowhere to add it.
    bool insert(SceneNode node);
    /// Whether node @p id is the root of the prefab being edited.
    bool isPrefabRoot(NodeId id);
    /// Whether the prefab at @p prefab is a variant; false when it cannot be read.
    bool isVariant(const std::string& prefab);
    /// Moves node @p id of the file view instance @p instance shows into a prefab saved at once, and leaves
    /// an instance of it in its place (TDDja/06 7). An instance becomes a variant of
    /// its prefab carrying its overrides. The prefab is @p file, replacing what it
    /// held, or a new file under prefabs/ when @p file is empty.
    void prefabify(int instance, NodeId id, std::string file = {});
    /// Handles node @p id (of view instance @p instance) dropped on prefab @p file: an instance of that prefab
    /// applies its overrides to it; any other node asks to replace it.
    void dropOnPrefab(int instance, NodeId id, const std::string& file);
    /// Writes the overrides of instance node @p id (of view instance @p instance) into its prefab, saved at once,
    /// and clears them.
    void applyOverrides(int instance, NodeId id);
    /// Replaces instance node @p id of the file view instance @p instance shows with plain nodes holding what
    /// it shows, nested instances included, and copies its systems into the file
    /// (TDDja/06 7). Nodes keep their IDs unless the file already uses one;
    /// references in the file and the level to a node given a new ID are rewritten.
    void unpack(int instance, NodeId id);
    /// Draws the confirmation requested through replacing_ before a prefab is replaced.
    void replaceDialog();
    /// Copies the selected node (without children) beside itself and selects the copy.
    void duplicate();
    /// Deletes the selected node (with its children) or system, when the edited file owns it.
    void erase();
    /// Returns the fields of @p script, reading it again when the file has changed.
    const CachedFields& fieldsOf(const std::string& script, const std::string& symbol);
    /// Replaces the document with the level at levelPath_ and its scenes; undoable.
    /// @return false on failure, with the reason in fileError_.
    bool open();
    /// Writes the level to levelPath_, every loaded scene and every opened prefab.
    /// @return false on failure, with the reason in fileError_.
    bool save();
    /// Handles keyboard shortcuts while no text field has focus.
    void shortcuts();
    /// Draws the main menu bar.
    void menu();
    /// Reads the open level's baked global illumination on a worker thread, then uses it.
    void readGlobalIllumination();
    /// The scene whose bake is in use (empty for none) and whether its GI was enabled, as last
    /// applied: the bake is read again when either changes.
    std::string giScene_;
    bool giEnabled_ = false;
    /// The scene whose environment was last applied; its sky setting applies when it changes.
    std::optional<std::string> lightingScene_;
    /// Shows newly placed model parts a few materials per frame, under a progress entry.
    void revealModels();
    /// Reads a model on a worker thread, then adds it to the library under @p key.
    void readModel(const ModelComponent& model, const std::string& key);
    /// Reads the project-relative environment @p path on a worker thread, then uploads it
    /// into environments_[@p key].
    void readEnvironmentFile(const std::string& path, const Ogre::Vector3& scale, const std::string& key);
    /// Draws the modal open/save dialog requested through showFiles_.
    void fileDialog();
    /// Enters Play mode on the level's expansion (TDDja/02 3, 06 6); scripts change
    /// only view_, never the document. Rejected scripts are reported in the log.
    void play();
    /// Leaves Play mode; the next refreshView() shows the document again.
    void stop();
    /// Suspends/resumes the current Play session; has no effect outside Play.
    void togglePause();
    /// Performs @p action now, or asks first when the document has unsaved changes.
    void requestLeave(Leave action);
    /// Performs @p action without asking.
    void leave(Leave action);
    /// Draws the Save / Discard / Cancel prompt requested through confirmClose_.
    void closeDialog();
    /// The name shown for view instance @p instance: its file's name and kind, and at
    /// the level the instance's ID when its scene is placed more than once.
    std::string instanceLabel(std::size_t instance);
    /// Draws the breadcrumb of the drill-down over the viewport.
    void breadcrumb();
    /// Draws the Scene panel: the node hierarchies of view_, with drag and drop
    /// reparenting of the edited file's own nodes.
    void scenePanel();
    /// Draws the children of node @p parent (an index, or npos) of view instance @p instance.
    void nodeTree(std::size_t instance, std::size_t parent);
    /// Draws the Systems panel: the level's systems and the systems of view_.
    void systemsPanel();
    /// Draws the property panel for the selected node or system.
    void inspector();
    /// Draws the selected node's properties, marking and reverting overridden ones.
    void nodeInspector(const ExpandedNode& node);
    /// Draws a scene instance's scene and transform.
    void sceneInspector(SceneInstance& instance);
    /// Draws the open level's properties: its rendering, and the view's camera, which is not saved.
    void levelInspector();
    /// Draws the environment and global illumination of the scene at @p path, written to its file.
    void lightingInspector(const std::string& path);
    /// Sets the view to node @p index of view instance @p instance and its Camera component:
    /// its position, its direction (without roll) and its projection.
    void alignViewToCamera(std::size_t instance, std::size_t index, const CameraComponent& camera);
    /// Writes the view into the selected node: its Transform's position and rotation (scale
    /// kept) and its Camera component's projection; one undo step.
    void alignCameraToView();
    /// Lists the nodes and systems @p overrides removes, named from @p expansion
    /// where the overrides' instance is at @p prefix, each with a Restore button.
    void removedList(Overrides& overrides, const Expansion& expansion, const NodePath& prefix);
    /// Draws the selected system's behaviour and their fields.
    void systemInspector(const System& system);
    /// Draws the rendered scene and handles camera and picking input.
    void viewport();
    /// Applies the editor's presentation policy and enables only visible scene rendering.
    void applyDisplaySettings();
    /// Draws OGRE's game output settings and the current level's rendering controls.
    void rendererInspector();
    /// Draws the Browser panel: the project's levels, scenes and prefabs.
    void browser();
    /// Draws the Console panel: the log, and an L^ prompt below it.
    void console();
    /// The runtime console lines run on: the playing one, or else one of the
    /// console's own on an empty world, made on first use. nullptr when that fails.
    ScriptRuntime* consoleRuntime();
    /// Runs @p line at the console and logs it with what it gave.
    void evaluate(const std::string& line);
    /// Applies the view parameters and projection mode to the camera.
    void updateCamera();
    /// Retargets the camera at the nearest rendered triangle, preserving its position.
    void focusAt(float x, float y);
    /// Centres the orbit camera on the selected node.
    void focusSelection();
    /// Draws the ImGuizmo transform gizmo for the selected node over the viewport
    /// rectangle and writes what it changes back to the node.
    void gizmo(ImVec2 origin, ImVec2 size);
    /// Selects the frontmost node under normalized viewport coordinates, or clears selection.
    void pick(float x, float y);
    /// Resizes the viewport texture and recreates the workspaces that reference it.
    void resizeViewport(unsigned width, unsigned height);
    /// Adds the viewport workspace, then the window workspace that samples it.
    void createWorkspaces();
    /// Removes both workspaces, if present.
    void destroyWorkspaces();
    /// Appends a line to the editor log panel.
    void log(const std::string& message);

    Ogre::Root& root_;
    Ogre::Window& window_;
    Ogre::SceneManager& scene_;
    Ogre::Camera& camera_;
    Ogre::HlmsUnlit& unlit_;
    Ogre::HlmsPbs& pbs_;
    Ogre::Light* sun_ = nullptr;
    std::unique_ptr<Ogre::AtmosphereNpr> sky_;
    /// Model files read for Model components, shared between the nodes placing them.
    std::unique_ptr<ModelLibrary> models_;
    bool skyboxEnabled_ = true;
    Ogre::SceneNode* sunNode_ = nullptr;
    bool shadowsEnabled_ = true;
    /// Temporal anti-aliasing: whether it is on, whether the history is unusable (new targets,
    /// a hidden view, TAA just turned on), the position in the jitter sequence, and the
    /// previous frame's jittered view-projection and jitter (in UV).
    bool temporalAntialiasing_ = true, taaReset_ = true;
    unsigned taaIndex_ = 0;
    Ogre::Matrix4 taaViewProj_ = Ogre::Matrix4::IDENTITY;
    Ogre::Vector2 taaJitter_ = Ogre::Vector2::ZERO;
    /// The temporal anti-aliasing pass material, given the reprojection each frame.
    Ogre::MaterialPtr taa_;
    /// The motion blur pass material, and whether it blurs while the game runs.
    Ogre::MaterialPtr motionBlur_;
    bool motionBlurInPlay_ = true;
    bool particleResourcesLoaded_ = false, particlePreview_ = true;
    int particleBlending_ = 1, appliedParticleBlending_ = -1;
    std::set<std::string> particleScripts_;
    std::size_t particleSystemCount_ = 0, particleCount_ = 0;
    int areaLightModel_ = -1;
    std::optional<bool> clustered_;
    float clusterMin_ = 0, clusterMax_ = 0;
    std::map<std::tuple<std::string,int,bool>,Ogre::TextureGpu*> projectTextures_;
    /// Environments read, by file; kept until shutdown because materials may refer to them.
    std::map<std::string,std::optional<Environment>> environments_;
    /// The level's environment file, and what it was read into (empty when it has none).
    std::string environmentFile_;
    const Environment* environment_ = nullptr;
    /// The viewport's exposure and tone-mapping pass material.
    Ogre::MaterialPtr tonemap_;
    /// The HlmsPbs listener: motion vectors, then the global illumination's.
    HlmsListenerChain pbsListeners_;
    MotionVectors motionVectors_;
    /// The level's baked global illumination, and whether a bake runs.
    std::unique_ptr<GlobalIllumination> gi_;
    bool baking_ = false, revealing_ = false;
    /// Long work kept running across frames, with its progress window.
    Tasks tasks_;
    /// Files being read on worker threads (a bake waits for them), and the model keys among them.
    unsigned reads_ = 0;
    std::set<std::string> modelsReading_;
    /// The reflection last given to model materials.
    Ogre::TextureGpu* modelReflection_ = nullptr;

    EditorPlatform platform_;
    OisInput input_;
    std::unique_ptr<Ogre::ImguiManager> gui_;
    std::unique_ptr<Ogre::CompositorPassImguiProvider> guiPass_;
    Ogre::TextureGpu* texture_ = nullptr;
    Ogre::CompositorWorkspace* viewportWorkspace_ = nullptr;
    Ogre::CompositorWorkspace* windowWorkspace_ = nullptr;
    Ogre::ManualObject* grid_ = nullptr;
    Ogre::SceneNode* gridNode_ = nullptr;
    Ogre::ManualObject* outline_ = nullptr;
    Ogre::SceneNode* outlineNode_ = nullptr;
    /// View > GI Probes: the GI box and the bake's reflection probe areas as wire boxes, and
    /// the Adaptive Irradiance Field's probes as crosses.
    Ogre::ManualObject* giBoxes_ = nullptr;
    Ogre::SceneNode* giBoxesNode_ = nullptr;
    /// GlobalIllumination::version() and layoutVersion_ when giBoxes_ was built.
    unsigned giBoxesVersion_ = ~0u, giBoxesLayout_ = ~0u;
    /// Whether giBoxes_ also shows the surface points the Adaptive Irradiance Field's probes are placed around.
    bool showSurface_ = false;
    /// The Adaptive Irradiance Field octree for the level's GI settings and the models placed, made when
    /// the debug display, the Inspector or a bake needs it (TDDja/12 3).
    ProbeOctree octree_;
    GiSettings layoutSettings_;
    std::size_t layoutModels_ = ~std::size_t(0);
    /// The volume octree_ covers: the GI box, or the bounds of everything drawn.
    Vec3 layoutLower_, layoutUpper_;
    unsigned layoutVersion_ = 0;
    /// Whether a placement for octree_ is running on a worker thread.
    bool placing_ = false;
    /// Brings octree_ up to date with the level's GI settings and placed models, on a worker
    /// thread: octree_ changes a few frames later.
    void updateProbeLayout();
    /// The bounds of the drawn nodes of view instance @p instance, in its scene's coordinates,
    /// into @p lower and @p upper; false when none.
    bool sceneBounds(Vec3& lower, Vec3& upper, int instance) const;
    /// Where the surfaces of the models placed in view instance @p instance are, in its scene's
    /// coordinates (ModelLibrary::surface()).
    std::vector<SurfacePoint> sceneSurface(int instance) const;
    std::vector<RenderObject> renderObjects_;
    /// One OGRE node per view_ instance, placing its scene; the scene's roots are its children.
    std::vector<Ogre::SceneNode*> sceneRoots_;
    /// Stable index lookup for existing nodes while runtime spawning appends children.
    std::vector<std::vector<Ogre::SceneNode*>> sceneNodes_;
    std::optional<Project> project_;
    ProjectList projects_;
    std::filesystem::path projectListFile_;
    std::filesystem::path preferencesFile_;
    EditorPreferences preferences_;
    bool viewportVisible_ = false;
    /// ImGui's settings file (the dock layout); kept alive because ImGui holds the pointer.
    std::string iniFile_;
    std::vector<Known> known_;
    enum class Form { None, New, Import } form_ = Form::None;
    char newName_[101] = "", newParent_[512] = "", importPath_[512] = "";
    std::string launcherError_;
    Leave leaving_ = Leave::Exit;
    Document doc_, editStart_;
    std::vector<Document> undo_, redo_;
    /// The drill-down; the first step is always the level.
    std::vector<Context> contexts_{{Context::Kind::Level, {}}};
    /// A node and the prefab it is to replace, waiting for confirmation (replaceDialog()).
    struct Replace {
        int instance;
        NodeId id;
        std::string file;
    };
    std::optional<Replace> replacing_, pendingReplace_;
    /// The drill-down to switch to at the start of the next frame.
    std::optional<std::vector<Context>> navigate_;
    Library library_;
    /// What the current step shows, expanded; the runtime moves its nodes while playing.
    LevelExpansion view_;
    /// Completed render snapshot from the preceding frame.
    std::optional<LevelExpansion> renderSnapshot_;
    std::unique_ptr<PhysicsWorld> physics_;
    /// Which system fields refer to each node of view_; rebuilt with it.
    References references_;
    /// view_'s node structure (instance, path, parent) when the render objects were built.
    std::vector<std::tuple<std::size_t, NodePath, std::size_t>> viewShape_;
    /// Non-null while playing.
    std::unique_ptr<ScriptRuntime> runtime_;
    /// The level's RenderEvent attachments while playing, run on *renderSnapshot_.
    std::unique_ptr<ScriptRuntime> renderEvents_;
    bool paused_ = false;
    /// What the console runs on while not playing: an empty world, and a runtime on it.
    World consoleWorld_;
    LevelExpansion consoleView_;
    std::unique_ptr<ScriptRuntime> consoleIdle_;
    /// The runtime the console's input being continued belongs to.
    ScriptRuntime* consoleMore_ = nullptr;
    char consoleInput_[1024] = "";
    /// Lines run at the console, oldest first, and the one shown while going through them.
    std::vector<std::string> consoleHistory_;
    std::size_t consoleRecall_ = 0;
    Selection selected_;
    std::map<std::string, CachedFields> fieldCache_;
    /// Drag-and-drop payload naming a node of the edited file by its ID.
    static constexpr const char* nodePayload = "THRONE_NODE";
    /// Text colours of nodes and systems that come from instances, of variants,
    /// and of overridden properties.
    static inline const ImVec4 instanceColour{0.55f, 0.75f, 1.0f, 1.0f}, variantColour{0.8f, 0.65f, 1.0f, 1.0f},
                               overriddenColour{1.0f, 0.75f, 0.3f, 1.0f};
    char newScenePath_[256] = "scenes/new.scene.lton";
    char newPrefabPath_[256] = "prefabs/new.prefab.lton";
    Ogre::Vector3 target_{0, 0.5f, 0};
    float yaw_ = 0.65f, pitch_ = 0.45f, distance_ = 9.0f;
    /// The view's projection: vertical field of view in degrees, and clip distances. Not saved;
    /// a Camera node keeps a view (alignViewToCamera(), alignCameraToView()).
    float viewFieldOfView_ = 45, viewNear_ = .1f, viewFar_ = 10000;
    /// Base fly speed in scene units per second, adjusted by the wheel during flight.
    float flySpeed_ = 5.0f;
    unsigned viewportWidth_ = 0, viewportHeight_ = 0;
    /// ImGuizmo::OPERATION of the gizmo (translate initially).
    int gizmoOperation_ = 7;
    bool gizmoLocal_ = true, gizmoOver_ = false, gizmoUsing_ = false;
    /// Navigation gesture captured by the viewport; disables the gizmo until release.
    enum class ViewDrag { None, Fly, Pan, Orbit, Dolly };
    ViewDrag viewDrag_ = ViewDrag::None;
    bool showStatistics_ = true;
    bool statisticsCollapsed_ = false;
    FrameTimings timings_, averageTimings_;
    bool showGrid_ = true, orthographic_ = false, close_ = false;
    bool dirty_ = false, showFiles_ = false, saving_ = true, confirmClose_ = false;
    /// Project-relative path of the open level.
    char levelPath_[512] = "";
    std::string fileError_;
    std::vector<std::string> messages_;
    /// Simulation and render event scripts may log at the same time; the console reads
    /// messages_ only between frames.
    std::mutex messagesMutex_;
};
}
