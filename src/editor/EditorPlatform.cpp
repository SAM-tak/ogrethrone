#include "editor/EditorPlatform.h"
#include <OgreWindow.h>
#include <imgui.h>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
/// Forwards input to ImGui and turns WM_CLOSE into a request flag (@p closeRequested).
LRESULT CALLBACK editorWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR closeRequested) {
    if (message == WM_CLOSE) {
        *reinterpret_cast<bool*>(closeRequested) = true;
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) return 1;
    return DefSubclassProc(hwnd, message, wparam, lparam);
}
}
#endif

namespace throne {
EditorPlatform::EditorPlatform(Ogre::Window& window) : window_(window) {
#ifdef _WIN32
    window.getCustomAttribute("WINDOW", &handle_);
    if (!ImGui_ImplWin32_Init(handle_)) throw std::runtime_error("ImGui Win32 initialization failed");
    if (!SetWindowSubclass(static_cast<HWND>(handle_), editorWindowProc, 1,
                           reinterpret_cast<DWORD_PTR>(&closeRequested_))) {
        ImGui_ImplWin32_Shutdown();
        throw std::runtime_error("Editor window input hook failed");
    }
#endif
}
EditorPlatform::~EditorPlatform() {
#ifdef _WIN32
    RemoveWindowSubclass(static_cast<HWND>(handle_), editorWindowProc, 1);
    ImGui_ImplWin32_Shutdown();
#endif
}
bool EditorPlatform::takeCloseRequest() { return std::exchange(closeRequested_, false); }
void EditorPlatform::newFrame() {
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(window_.getWidth()),
                                      static_cast<float>(window_.getHeight()));
#endif
}
}
