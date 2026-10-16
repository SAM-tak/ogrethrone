#pragma once

#include "editor/EditorPlatform.h"
#include "editor/Level.h"
#include "editor/Project.h"
#include "script/ScriptRuntime.h"
#include <OgreVector3.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
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
    /// OGRE objects that display one scene node.
    struct RenderObject {
        std::size_t scene;
        std::uint32_t id;
        Ogre::SceneNode* node;
        Ogre::ManualObject* mesh;
        Ogre::HlmsUnlitDatablock* material;
    };
    /// What the inspector shows: a scene node, or a runner of a scene or of the level.
    struct Selection {
        enum class Kind { None, Node, Runner } kind = Kind::None;
        /// Index into World::scenes, or -1 for a level runner.
        int scene = -1;
        std::uint32_t id = 0;
    };
    /// A script's editable fields, refreshed when the file changes.
    struct CachedFields {
        std::filesystem::file_time_type stamp;
        std::vector<FieldInfo> fields;
        std::string error;
        bool loaded = false;
    };

    /// Recreates all render objects from the world (after load, undo or structural edits).
    void rebuildScene();
    /// Destroys every render object and its material.
    void destroyRenderObjects();
    /// Copies node transforms and colours to the render objects and the selection outline.
    void syncTransforms();
    /// Records the current world for undo before an edit.
    void checkpoint();
    /// Records @p before for undo when an edit that started from it has finished.
    void commit(World before);
    /// Restores the previous world state, if any.
    void undo();
    /// Reapplies the most recently undone state, if any.
    void redo();
    /// The selected scene node, or nullptr.
    SceneNode* selectedNode();
    /// The selected runner, or nullptr.
    Runner* selectedRunner();
    /// The runners of a scene (index) or of the level (-1).
    RunnerSet& runnersOf(int scene);
    /// Adds a cube under the selected node, or at the root of the first scene, and selects it.
    void addNode();
    /// Adds a runner to a scene (index) or the level (-1) and selects it.
    void addRunner(int scene);
    /// Loads the scene at newScenePath_ into the level, creating it when the file is absent.
    void addScene();
    /// Copies the selected node (without children) beside itself and selects the copy.
    void duplicate();
    /// Deletes the selected node (with its children) or runner.
    void erase();
    /// Returns the fields of @p script, reading it again when the file has changed.
    const CachedFields& fieldsOf(const std::string& script);
    /// Replaces the world with the level at levelPath_ and its scenes; undoable.
    /// @return false on failure, with the reason in fileError_.
    bool open();
    /// Writes the level to levelPath_ and every loaded scene to its path.
    /// @return false on failure, with the reason in fileError_.
    bool save();
    /// Handles keyboard shortcuts while no text field has focus.
    void shortcuts();
    /// Draws the main menu bar.
    void menu();
    /// Draws the modal open/save dialog requested through showFiles_.
    void fileDialog();
    /// Enters Play mode: runs the scripts on a copy of the document (TDDja/02 3).
    /// Rejected scripts are reported in the log and Play is not entered.
    void play();
    /// Leaves Play mode and restores the document as it was before play().
    void stop();
    /// Performs @p action now, or asks first when the document has unsaved changes.
    void requestLeave(Leave action);
    /// Performs @p action without asking.
    void leave(Leave action);
    /// Draws the Save / Discard / Cancel prompt requested through confirmClose_.
    void closeDialog();
    /// Draws the Scene panel: each loaded scene's node hierarchy, with drag and drop reparenting.
    void scenePanel();
    /// Draws the children of @p parent in scene @p scene as a tree.
    void nodeTree(std::size_t scene, std::uint32_t parent);
    /// Draws the Runners panel: the level's runners and each scene's.
    void runnersPanel();
    /// Draws the property panel for the selected node or runner.
    void inspector();
    /// Draws the node properties.
    void nodeInspector(SceneNode& node);
    /// Draws the runner's name and its behaviours with their fields.
    void runnerInspector(Runner& runner, int scene);
    /// Draws the rendered scene and handles camera and picking input.
    void viewport();
    /// Draws the bottom panel listing the project's levels and log messages.
    void browser();
    /// Applies the orbit parameters and projection mode to the camera.
    void updateCamera();
    /// Centres the orbit camera on the selected node.
    void focusSelection();
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
    World world_;
    World editStart_;
    std::vector<World> undo_, redo_;
    /// Non-null while playing; runs on world_, with the edited state kept in playBackup_.
    std::unique_ptr<ScriptRuntime> runtime_;
    World playBackup_;
    Selection selected_;
    std::map<std::string, CachedFields> fieldCache_;
    char newScenePath_[256] = "scenes/new.scene.lton";
    Ogre::Vector3 target_{0, 0.5f, 0};
    float yaw_ = 0.65f, pitch_ = 0.45f, distance_ = 9.0f;
    unsigned viewportWidth_ = 0, viewportHeight_ = 0;
    bool showGrid_ = true, orthographic_ = false, close_ = false;
    bool dirty_ = false, showFiles_ = false, saving_ = true, confirmClose_ = false;
    /// Project-relative path of the open level.
    char levelPath_[512] = "";
    std::string fileError_;
    std::vector<std::string> messages_;
};
}
