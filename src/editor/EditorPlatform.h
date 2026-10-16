#pragma once

namespace Ogre { class Window; }
namespace throne {
/// Feeds native window input and display size to Dear ImGui.
///
/// On Windows this initializes the ImGui Win32 backend and subclasses the OGRE
/// window so its messages reach ImGui and closing the window becomes a request
/// (see takeCloseRequest()). Elsewhere only the display size is set.
/// Requires a current ImGui context for its whole lifetime.
class EditorPlatform {
public:
    /// Attaches to @p window, which must outlive this object.
    /// @throws std::runtime_error if the backend or input hook cannot be installed.
    explicit EditorPlatform(Ogre::Window& window);
    /// Removes the input hook and shuts the backend down.
    ~EditorPlatform();
    EditorPlatform(const EditorPlatform&) = delete;
    EditorPlatform& operator=(const EditorPlatform&) = delete;
    /// Updates ImGui's per-frame platform state; call before ImGui::NewFrame().
    void newFrame();
    /// Returns whether the user asked to close the window since the last call.
    bool takeCloseRequest();
private:
    Ogre::Window& window_;
    void* handle_ = nullptr;
    bool closeRequested_ = false;
};
}
