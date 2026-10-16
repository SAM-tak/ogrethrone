#pragma once

#include "editor/EditorPlatform.h"
#include "editor/SceneDocument.h"
#include <OgreVector3.h>
#include <memory>
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
/// Dear ImGui scene editor drawn over the OGRE window.
///
/// The scene is rendered into an offscreen texture shown in the viewport panel;
/// the window itself is cleared and covered by ImGui. Requires HlmsUnlit to be
/// registered and an ImGui context to exist for the editor's lifetime.
class SceneEditor {
public:
    /// Installs the ImGui compositor pass, the viewport texture and both workspaces.
    /// @param root Initialized OGRE root with HlmsUnlit registered.
    /// @param window Window that receives the editor UI.
    /// @param scene Scene manager holding the edited objects.
    /// @param camera Camera used for the viewport.
    SceneEditor(Ogre::Root& root, Ogre::Window& window, Ogre::SceneManager& scene, Ogre::Camera& camera);
    /// Removes workspaces, scene objects and editor resources.
    ~SceneEditor();
    SceneEditor(const SceneEditor&) = delete;
    SceneEditor& operator=(const SceneEditor&) = delete;

    /// Builds the UI and updates the scene; call once before each Root::renderOneFrame().
    void frame(float deltaSeconds);
    /// True once File > Exit was chosen.
    bool shouldClose() const { return close_; }

private:
    /// OGRE objects that display one SceneObject; same order as SceneDocument::objects.
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
    void commit(SceneDocument before);
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
    /// Replaces the document with the file at scenePath_; undoable.
    /// @return false on failure, with the reason in fileError_.
    bool open();
    /// Writes the document to scenePath_.
    /// @return false on failure, with the reason in fileError_.
    bool save();
    /// Handles keyboard shortcuts while no text field has focus.
    void shortcuts();
    /// Draws the main menu bar.
    void menu();
    /// Draws the modal open/save dialog requested through showFiles_.
    void fileDialog();
    /// Draws the object list panel.
    void hierarchy();
    /// Draws the property panel for the selected object.
    void inspector();
    /// Draws the rendered scene and handles camera and picking input.
    void viewport();
    /// Draws the bottom panel listing scene files and log messages.
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
    SceneDocument document_ = SceneDocument::example();
    SceneDocument editStart_;
    std::vector<SceneDocument> undo_, redo_;
    std::uint32_t selected_ = 1;
    Ogre::Vector3 target_{0, 0.5f, 0};
    float yaw_ = 0.65f, pitch_ = 0.45f, distance_ = 9.0f;
    unsigned viewportWidth_ = 0, viewportHeight_ = 0;
    bool showGrid_ = true, orthographic_ = false, close_ = false;
    bool dirty_ = false, showFiles_ = false, saving_ = true;
    char scenePath_[512] = "scenes/Untitled.throne.json";
    std::string fileError_;
    std::vector<std::string> messages_;
};
}
