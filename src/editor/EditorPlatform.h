#pragma once
#include <functional>
#include <filesystem>
#include <optional>

namespace Ogre { class Window; }
namespace throne {
/// Feeds native window input and display size to Dear ImGui.
///
/// On Windows this initializes the ImGui Win32 backend and subclasses the OGRE
/// window so its messages reach ImGui and closing the window becomes a request
/// (see takeCloseRequest()), and gives the window a title bar that follows
/// Windows' light or dark app mode. IME results are read as UTF-16 before the
/// ANSI OGRE window can convert them; native composition/candidate UI is retained.
/// While the window is dragged
/// or resized, Windows runs its own message loop; modalFrame is then called about
/// 60 times a second so the window keeps being drawn. Elsewhere only the display
/// size is set.
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
    /// Opens an owned native folder picker, starting at @p initial when it exists.
    /// Returns no path on cancellation; throws std::runtime_error on failure.
    /// On Windows, the calling thread must have an initialized COM STA.
    std::optional<std::filesystem::path> pickFolder(const std::filesystem::path& initial) const;
    /// Opens @p file in the operating system's application for it. Windows only for now.
    void openFile(const std::filesystem::path& file) const;
    /// Shows @p path selected in the operating system's file manager. Windows only for now.
    void showInFolder(const std::filesystem::path& path) const;
    /// Runs one frame of the application while a move or resize blocks its main loop.
    std::function<void()> modalFrame;
private:
#ifdef _WIN32
    friend struct WindowHook;
#endif
    Ogre::Window& window_;
    void* handle_ = nullptr;
    bool closeRequested_ = false;
};
}
