#include "editor/EditorPlatform.h"
#include <OgreWindow.h>
#include <OgreLogManager.h>
#include <imgui.h>
#include <cwchar>
#include <stdexcept>
#include <utility>
#include <memory>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <shobjidl.h>
#include <wrl/client.h>
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
            Ogre::LogManager::getSingleton().logMessage("Shutdown: WM_CLOSE received");
            platform.closeRequested_ = true;
            return 0;
        case WM_IME_COMPOSITION:
            if (lparam & GCS_RESULTSTR) {
                // OGRE's ANSI window would otherwise convert committed text to code-page bytes.
                const auto context = ImmGetContext(hwnd);
                if (!context) break;
                const auto bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                std::wstring text(bytes > 0 ? static_cast<std::size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
                const auto copied = bytes > 0
                    ? ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), static_cast<DWORD>(bytes)) : bytes;
                ImmReleaseContext(hwnd, context);
                if (copied < 0) break;
                text.resize(static_cast<std::size_t>(copied) / sizeof(wchar_t));
                for (const auto character : text) ImGui::GetIO().AddInputCharacterUTF16(character);
                // Do not generate WM_IME_CHAR/WM_CHAR for text already delivered to ImGui.
                lparam &= ~(GCS_RESULTSTR | GCS_RESULTREADSTR | GCS_RESULTCLAUSE | GCS_RESULTREADCLAUSE);
                if (!lparam) return 0;
            }
            break;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            for (UINT i = 0, count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); i < count; ++i) {
                std::wstring name(DragQueryFileW(drop, i, nullptr, 0), L'\0');
                DragQueryFileW(drop, i, name.data(), UINT(name.size() + 1));
                platform.drop_.files.emplace_back(name);
            }
            POINT point{};
            DragQueryPoint(drop, &point);
            platform.drop_.x = float(point.x), platform.drop_.y = float(point.y);
            DragFinish(drop);
            return 0;
        }
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
    DragAcceptFiles(static_cast<HWND>(handle_), TRUE);
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
EditorPlatform::Drop EditorPlatform::takeDrop() { return std::exchange(drop_, {}); }
std::optional<std::filesystem::path> EditorPlatform::pickFolder(const std::filesystem::path& initial) const {
#ifdef _WIN32
    const auto check = [](HRESULT result) {
        if (FAILED(result)) throw std::runtime_error("Could not open the folder picker.");
    };
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)));
    DWORD options = 0;
    check(dialog->GetOptions(&options));
    check(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR));
    if (!initial.empty()) {
        Microsoft::WRL::ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dialog->SetFolder(folder.Get());
    }
    const auto result = dialog->Show(static_cast<HWND>(handle_));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    check(result);
    Microsoft::WRL::ComPtr<IShellItem> folder;
    check(dialog->GetResult(&folder));
    PWSTR name = nullptr;
    check(folder->GetDisplayName(SIGDN_FILESYSPATH, &name));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> ownedName(name, &CoTaskMemFree);
    return std::filesystem::path(ownedName.get());
#else
    throw std::runtime_error("The native folder picker is currently available on Windows only.");
#endif
}
void EditorPlatform::openFile(const std::filesystem::path& file) const {
#ifdef _WIN32
    ShellExecuteW(static_cast<HWND>(handle_), L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)file;
#endif
}
void EditorPlatform::showInFolder(const std::filesystem::path& path) const {
#ifdef _WIN32
    const auto arguments = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(static_cast<HWND>(handle_), L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
#else
    (void)path;
#endif
}
void EditorPlatform::newFrame() {
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(window_.getWidth()),
                                      static_cast<float>(window_.getHeight()));
#endif
}
}
