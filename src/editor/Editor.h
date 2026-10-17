#pragma once

#include "editor/EditorPlatform.h"
#include "editor/Expansion.h"
#include "editor/Project.h"
#include "script/ScriptRuntime.h"
#include <OgreVector3.h>
#include <imgui.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
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
class HlmsUnlit;
class HlmsUnlitDatablock;
}

namespace throne {
/// Dear ImGui level editor and project launcher drawn over the OGRE window.
///
/// The editor works on a World: the level's runners plus the scenes it loads.
/// Panels: Scene (node hierarchies), Runners, Inspector, Viewport and Browser.
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

    /// Builds the UI and updates the scene; call once before each Root::renderOneFrame().
    void frame(float deltaSeconds);
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
        Ogre::HlmsUnlitDatablock* material;
    };
    /// Everything the editor edits; one undo step is a copy of it.
    struct Document {
        World world;
        /// Prefabs opened for editing (TDDja/06 6), by path; saved with the level.
        std::map<std::string, Prefab> prefabs;
    };
    /// One step of the drill-down (TDDja/06 6).
    struct Context {
        enum class Kind { Level, Scene, Prefab } kind;
        /// The scene or prefab file; empty for the level.
        std::string path;
    };
    /// What the inspector shows: a node of view_, or a runner.
    struct Selection {
        enum class Kind { None, Node, Runner } kind = Kind::None;
        /// Index into view_.instances; -1 for a runner of the level itself.
        int instance = -1;
        /// The node's path; for a runner, the prefab instance it came from.
        NodePath path;
        /// The runner's ID.
        std::uint32_t runner = 0;
    };
    /// Where an edit of one node is written: the node itself (it belongs to the
    /// file being edited), or a property override of the instance that contains it.
    struct NodeTarget {
        SceneNode* node = nullptr;
        Overrides* overrides = nullptr;
        NodePath relative;
    };
    /// Where an edit of one runner is written: the runner itself, or field overrides.
    struct RunnerTarget {
        Runner* runner = nullptr;
        Overrides* overrides = nullptr;
        NodePath instance;
    };
    /// A script's editable fields, refreshed when the file changes.
    struct CachedFields {
        std::filesystem::file_time_type stamp;
        std::vector<FieldInfo> fields;
        std::string error;
        bool loaded = false;
    };

    /// The scene or prefab of the current context, or nullptr at the level.
    Prefab* content();
    /// Expands what the current context shows into view_, and rebuilds the render
    /// objects when the node structure changed. Skipped while playing.
    void refreshView();
    /// Enters the drill-down step @p context at the start of the next frame.
    void enter(Context context);
    /// Where an edit of node @p path of view instance @p instance goes.
    NodeTarget targetOf(int instance, const NodePath& path);
    /// Where an edit of the selected runner goes.
    RunnerTarget runnerTarget();
    /// The selected node of view_, or nullptr.
    const ExpandedNode* selectedNode();
    /// The selected runner of view_ (or of the level), or nullptr.
    const Runner* selectedRunner();
    /// Writes @p value into the selected node through targetOf(); @p changed says which properties.
    void writeNode(const SceneNode& value, unsigned changed);
    /// Recreates all render objects from view_.
    void rebuildScene();
    /// Destroys every render object and its material.
    void destroyRenderObjects();
    /// Copies node transforms and colours to the render objects and the selection outline.
    void syncTransforms();
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
    void addNode();
    /// Adds an instance of the prefab at newPrefabPath_ to the edited scene or prefab,
    /// creating an empty prefab when the file is absent.
    void addInstance();
    /// Adds a runner to the level (at the Level step) or to the edited scene or prefab.
    void addRunner();
    /// Adds an instance of the scene at newScenePath_ to the level, creating the file when absent.
    void addScene();
    /// Removes scene instance @p id from the level.
    void removeScene(std::uint32_t id);
    /// Copies the selected node (without children) beside itself and selects the copy.
    void duplicate();
    /// Deletes the selected node (with its children) or runner, when the edited file owns it.
    void erase();
    /// Returns the fields of @p script, reading it again when the file has changed.
    const CachedFields& fieldsOf(const std::string& script);
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
    /// Draws the modal open/save dialog requested through showFiles_.
    void fileDialog();
    /// Enters Play mode on the level's expansion (TDDja/02 3, 06 6); scripts change
    /// only view_, never the document. Rejected scripts are reported in the log.
    void play();
    /// Leaves Play mode; the next refreshView() shows the document again.
    void stop();
    /// Performs @p action now, or asks first when the document has unsaved changes.
    void requestLeave(Leave action);
    /// Performs @p action without asking.
    void leave(Leave action);
    /// Draws the Save / Discard / Cancel prompt requested through confirmClose_.
    void closeDialog();
    /// Draws the breadcrumb of the drill-down over the viewport.
    void breadcrumb();
    /// Draws the Scene panel: the node hierarchies of view_, with drag and drop
    /// reparenting of the edited file's own nodes.
    void scenePanel();
    /// Draws the children of node @p parent (an index, or npos) of view instance @p instance.
    void nodeTree(std::size_t instance, std::size_t parent);
    /// Draws the Runners panel: the level's runners and the runners of view_.
    void runnersPanel();
    /// Draws the property panel for the selected node or runner.
    void inspector();
    /// Draws the selected node's properties, marking and reverting overridden ones.
    void nodeInspector(const ExpandedNode& node);
    /// Draws the selected runner's behaviours and their fields.
    void runnerInspector(const Runner& runner);
    /// Draws the rendered scene and handles camera and picking input.
    void viewport();
    /// Draws the bottom panel listing the project's levels and log messages.
    void browser();
    /// Applies the orbit parameters and projection mode to the camera.
    void updateCamera();
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
    EditorPlatform platform_;
    std::unique_ptr<Ogre::ImguiManager> gui_;
    std::unique_ptr<Ogre::CompositorPassImguiProvider> guiPass_;
    Ogre::TextureGpu* texture_ = nullptr;
    Ogre::CompositorWorkspace* viewportWorkspace_ = nullptr;
    Ogre::CompositorWorkspace* windowWorkspace_ = nullptr;
    Ogre::ManualObject* grid_ = nullptr;
    Ogre::SceneNode* gridNode_ = nullptr;
    Ogre::ManualObject* outline_ = nullptr;
    Ogre::SceneNode* outlineNode_ = nullptr;
    std::vector<RenderObject> renderObjects_;
    std::optional<Project> project_;
    ProjectList projects_;
    std::filesystem::path projectListFile_;
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
    /// The drill-down to switch to at the start of the next frame.
    std::optional<std::vector<Context>> navigate_;
    Library library_;
    /// What the current step shows, expanded; the runtime moves its nodes while playing.
    LevelExpansion view_;
    /// view_'s node structure (instance, path, parent) when the render objects were built.
    std::vector<std::tuple<std::size_t, NodePath, std::size_t>> viewShape_;
    /// Non-null while playing.
    std::unique_ptr<ScriptRuntime> runtime_;
    Selection selected_;
    std::map<std::string, CachedFields> fieldCache_;
    char newScenePath_[256] = "scenes/new.scene.lton";
    char newPrefabPath_[256] = "prefabs/new.prefab.lton";
    Ogre::Vector3 target_{0, 0.5f, 0};
    float yaw_ = 0.65f, pitch_ = 0.45f, distance_ = 9.0f;
    unsigned viewportWidth_ = 0, viewportHeight_ = 0;
    /// ImGuizmo::OPERATION of the gizmo (translate initially).
    int gizmoOperation_ = 7;
    bool gizmoLocal_ = true, gizmoOver_ = false, gizmoUsing_ = false;
    bool showGrid_ = true, orthographic_ = false, close_ = false;
    bool dirty_ = false, showFiles_ = false, saving_ = true, confirmClose_ = false;
    /// Project-relative path of the open level.
    char levelPath_[512] = "";
    std::string fileError_;
    std::vector<std::string> messages_;
};
}
