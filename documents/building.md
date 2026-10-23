# Building and dependencies

## Windows details

Visual Studio's LLVM directory is added to PATH by the standard Windows presets.

The executable embeds a manifest (visual styles, per-monitor DPI awareness, UTF-8)
and the application icon `media/ogrethrone.ico`; after editing the
`media/ogrethrone<size>.png` images, run `tools/make-icon.ps1` to repack it. The
title bar follows Windows' light or dark app mode.

`VULKAN_SDK` can also locate the SDK. A terminal opened before SDK installation
may need restarting, or use the explicit CMake option above.
The pinned glslang submodule is built from source for each configuration, using
the same MSVC runtime as OGRE. SDK shader compiler libraries are not linked.
MSVC remains available for compatibility checks in a separate build directory:

```powershell
cmake --preset windows-msvc -DOGRE_VULKAN_SDK=C:/VulkanSDK/<installed-version>
cmake --build --preset windows-msvc-debug --parallel 4
ctest --preset windows-msvc-debug
```

Use `windows-release` / `windows-msvc-release` to build and test Release, and
`windows-msvc-headless` for CPU checks with MSVC. The MSVC presets use VS 2026;
with VS 2022, override the generator with `-G "Visual Studio 17 2022"` at initial
configuration. Clang and MSVC use independent build trees; old `out/build/windows`
and `out/build/windows-headless` directories are not reused.
The VS 2026 generator requires a CMake version that supports that generator;
the Clang/Ninja presets do not require the ClangCL MSBuild integration component.

The smoke check initializes an L^ VM, simulates a box falling onto a ground plane,
and compares all 180 positions against a second identical run. This verifies
same-build repeatability; it does not establish cross-platform determinism.

Verified locally with Clang 22.1.3 (MSVC ABI), Visual Studio 2026's Windows toolchain,
Vulkan SDK 1.4.363.0 and an NVIDIA GeForce GTX 1660 Ti: Debug and Release builds,
all three CTest checks, and three-frame Vulkan runs passed. Both graphics runs
compiled the ImGui vertex and fragment shaders successfully; Release also passed
the audio device playback check. The separate MSVC headless preset passed all
three CTest checks. CI is configured to check both Windows compilers.

## VS Code

Open the repository root as a folder. The shared `.vscode` configuration recommends
CMake Tools and C/C++, uses CMake Presets for build flags and IntelliSense, and
lets CMake Tools initialize the Visual Studio developer environment on Windows.
Select the `windows` configure preset and a build preset, then
select `ogrethrone` with **CMake: Set Debug Target** (or the launch-target picker).
`Ctrl+Shift+B` builds the active preset; **Tasks: Run Test Task** runs CTest.
The **Regenerate samples/basic/lhat-host.json** task builds the editor and rewrites
the sample's host API file with `--dump-host-api`.
The supplied Windows F5 configuration builds and launches the selected CMake
target using the active build preset, with the Visual Studio debugger. To switch,
open **Ctrl+Shift+P → CMake: Select Build Preset** and choose `windows-debug`
(unoptimized debugging), `windows-release` (optimized), or `windows-relwithdebinfo`
(optimized with debug symbols). Then press F5. If the configure preset has not
been selected yet, use **CMake: Select Configure Preset → windows** first.
F5 uses CMake Tools' `cmake.buildBeforeRun` incremental build check; the launch
configuration does not add a second build task. With no source changes, it should
report no work rather than recompile the editor.

This engine currently has no C# component. In the Extensions view, select
**Disable (Workspace)** for C# Dev Kit (`ms-dotnettools.csdevkit`) and C#
(`ms-dotnettools.csharp`), then reload the window if prompted. This prevents .NET
project discovery from treating old C# examples bundled with zlib as engine
projects. The `unwantedRecommendations` list documents this choice; it does not
disable already installed extensions. Other workspaces retain their C# support.

Do not convert the bundled `DotZLib.csproj` or restore its NuGet packages to build
this engine. It is outside the CMake target graph. Configure from the root rather
than opening third-party CMake projects independently.

The parent sets the minimum dependency policy version to 3.10. This avoids CMake's
deprecated pre-3.10 compatibility mode without suppressing deprecation warnings
globally or modifying the bundled ICU sources.

## Linux

Use the `linux` or `linux-headless` configure/build presets with Ninja and a C17 /
C++17 compiler. Graphics additionally requires Vulkan headers/loader and
X11/XCB development packages. Linux builds have not yet been verified.

## Dependency policy

- OGRE-Next is pinned to a 4.0 development commit. Vulkan is the only enabled
  graphics backend; threaded shader/PSO compilation is enabled with mode `2`.
- OGRE's ImGui integration builds the pinned ImGui sources. Do not build a second
  ImGui copy. `ImTextureID` is configured as `void*` for the OGRE renderer.
  ImGui is the `v1.91.6-docking` tag, so editor panels can dock; 1.91.5's Win32
  backend crashed when the cursor was over another Dear ImGui application's window.
- ImGuizmo (`third_party/imguizmo`) is pinned to `22a6c90`, the last commit before
  it followed ImGui 1.92.8's `AddPolyline`/`AddRect` argument changes, and is
  compiled into the editor against the same ImGui. Its `SetRect` miscomputes the
  bottom edge in this commit; only bounds manipulation, which the editor does not
  use, depends on it.
- The pinned OGRE build scripts re-add the disabled FreeImage codec via their
  unity-build exclusion list. The parent CMake adapter removes that source from
  the target without editing the codec sources.
- The OGRE fork includes clang-cl compatibility fixes: native SIMD vector
  operators must not be redeclared, and the existing Clang workaround for arrays
  of restricted pointers also applies to clang-cl. `-Wall` reaches the Clang
  frontend instead of selecting clang-cl's MSVC `/Wall` behavior. Clang uses
  `/fp:precise` because OGRE relies on infinity for unbounded objects. Windows
  module-handle sentinels are explicitly initialized, and resource compilation
  does not receive PIC or C/C++ warning options.
- Colibri builds its bundled FreeType, HarfBuzz, ICU and zlib dependencies.
- glslang is pinned and built with SPIR-V output enabled. SPIRV-Tools optimization
  is disabled; it is not bundled by this bootstrap.
- Box3D keeps its default deterministic math settings; do not enable fast-math.
- ozz-animation (`third_party/ozz-animation`) is pinned to the 0.17.0 release. Only its
  runtime and offline libraries are built (no tools, importers, samples or tests), with its
  warnings off; skeletons and clips are built from imported data when a model is read, not
  from `.ozz` files.
- MSVC dependencies use the static runtime consistently (`/MT`, `/MTd`).
- Initially assets are unpacked; OGRE ZIP, FreeImage and dependency demos/tools
  are disabled. RapidJSON is pinned for Colibri skins and OGRE materials.
  PBR, Unlit and particle plugins remain
  available. Asset packaging/import tooling is a separate milestone.
- No shader warm-up policy or disk cache persistence is implemented by this bootstrap.
- Apple support is not configured yet. Vulkan-through-portability support must
  be verified separately rather than assumed from the presence of a Vulkan backend.

For L^ development, use the sibling working tree explicitly:

```powershell
cmake --preset windows -DTHRONE_LHAT_SOURCE_DIR=C:/Users/Owner/source/repos/lhat
```

The default is the pinned `third_party/lhat`, so clean clones are reproducible.
When updating dependencies, check out the intended commit inside each submodule,
build and test, then commit the new submodule pointers in the parent repository.
Keep local upstream modifications in explicit commits/forks; do not hide patches
inside build scripts. Each dependency retains its own license.
