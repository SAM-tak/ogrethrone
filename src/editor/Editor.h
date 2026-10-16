#pragma once

#include "editor/EditorPlatform.h"
#include "editor/Level.h"
#include "editor/Project.h"
#include <OgreVector3.h>
#include <filesystem>
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
class ScriptRuntime;

/// Dear ImGui level editor and project launcher drawn over the OGRE window.
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
    /// OGRE objects that display one LevelObject; same order as Level::objects.
    struct RenderObject {
        Ogre::SceneNode* node;
        Ogre::ManualObject* mesh;
        Ogre::HlmsUnlitDatablock* material;
    };
    /// Recreates all render objects from the document (after load, undo or structural edits).
    void rebuildScene();
    /// Destroys every render object and its material.
    void destroyRenderObjects();
    /// Copies document transforms and colours to the render objects and selection outline.
    void syncTransforms();
    /// Records the current document for undo before an edit.
    void checkpoint();
    /// Records @p before for undo when an edit that started from it has finished.
    void commit(Level before);
    /// Restores the previous document state, if any.
    void undo();
    /// Reapplies the most recently undone state, if any.
    void redo();
    /// Adds a default cube and selects it.
    void addCube();
    /// Copies the selected object with a small offset and selects the copy.
    void duplicate();
    /// Deletes the selected object.
    void erase();
    /// Replaces the document with the file at levelPath_; undoable.
    /// @return false on failure, with the reason in fileError_.
    bool open();
    /// Writes the document to levelPath_.
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
    /// Draws the object list panel.
    void hierarchy();
    /// Draws the property panel for the selected object.
    void inspector();
    /// Draws the rendered scene and handles camera and picking input.
    void viewport();
    /// Draws the bottom panel listing the project's levels and log messages.
    void browser();
    /// Applies the orbit parameters and projection mode to the camera.
    void updateCamera();
    /// Centres the orbit camera on the selected object.
    void focusSelection();
    /// Selects the frontmost object under normalized viewport coordinates, or clears selection.
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
    std::vector<Known> known_;
    enum class Form { None, New, Import } form_ = Form::None;
    char newName_[101] = "", newParent_[512] = "", importPath_[512] = "";
    std::string launcherError_;
    Leave leaving_ = Leave::Exit;
    Level document_;
    Level editStart_;
    std::vector<Level> undo_, redo_;
    /// Non-null while playing; runs on document_, with the edited state kept in playBackup_.
    std::unique_ptr<ScriptRuntime> runtime_;
    Level playBackup_;
    std::uint32_t selected_ = 0;
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
