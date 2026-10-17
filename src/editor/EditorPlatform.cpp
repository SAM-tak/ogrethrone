#include "editor/EditorPlatform.h"
#include <OgreWindow.h>
#include <imgui.h>
#include <cwchar>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
/// Gives @p hwnd a dark title bar when Windows' app mode is dark.
void followTheme(HWND hwnd) {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    const BOOL dark = light == 0;
    // DWMWA_USE_IMMERSIVE_DARK_MODE; older SDKs lack the name.
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
}
}

namespace throne {
/// The window procedure hook of EditorPlatform.
struct WindowHook {
    /// Timer that drives modalFrame during a move or resize.
    static constexpr UINT_PTR timer = 1;

    /// Forwards input to ImGui, turns WM_CLOSE into a request flag and keeps
    /// drawing during moves and resizes. @p data is the EditorPlatform.
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto& platform = *reinterpret_cast<EditorPlatform*>(data);
        switch (message) {
        case WM_CLOSE:
            platform.closeRequested_ = true;
            return 0;
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd, timer, 16, nullptr);
            break;
        case WM_EXITSIZEMOVE:
            KillTimer(hwnd, timer);
            break;
        case WM_SETTINGCHANGE:
            if (lparam && std::wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0)
                followTheme(hwnd);
            break;
        case WM_TIMER:
            if (wparam != timer) break;
            if (platform.modalFrame) platform.modalFrame();
            return 0;
        }
        if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) return 1;
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
};
}
#endif

namespace throne {
EditorPlatform::EditorPlatform(Ogre::Window& window) : window_(window) {
#ifdef _WIN32
    window.getCustomAttribute("WINDOW", &handle_);
    if (!ImGui_ImplWin32_Init(handle_)) throw std::runtime_error("ImGui Win32 initialization failed");
    if (!SetWindowSubclass(static_cast<HWND>(handle_), WindowHook::proc, 1, reinterpret_cast<DWORD_PTR>(this))) {
        ImGui_ImplWin32_Shutdown();
        throw std::runtime_error("Editor window input hook failed");
    }
    followTheme(static_cast<HWND>(handle_));
    // OGRE registers its window class with the stock icon; use the executable's own.
    const auto instance = GetModuleHandleW(nullptr);
    for (const auto [kind, metric] : {std::pair{ICON_SMALL, SM_CXSMICON}, std::pair{ICON_BIG, SM_CXICON}}) {
        const auto size = GetSystemMetrics(metric);
        if (const auto icon = LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_SHARED))
            SendMessageW(static_cast<HWND>(handle_), WM_SETICON, kind, reinterpret_cast<LPARAM>(icon));
    }
#endif
}
EditorPlatform::~EditorPlatform() {
#ifdef _WIN32
    KillTimer(static_cast<HWND>(handle_), WindowHook::timer);
    RemoveWindowSubclass(static_cast<HWND>(handle_), WindowHook::proc, 1);
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
