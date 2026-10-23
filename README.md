# Ogre Throne

A C++ game engine using OGRE-Next 4.0 development sources for Vulkan graphics,
Box3D for physics, L^ for scripting, Dear ImGui for editor UI, Colibri for
in-game UI, OIS for device input, and SoLoud + Steam Audio for sound. The name is currently a working name.

![Screenshot](./images/bistro-exterior.jpg)

## Checkout

```sh
git clone --recurse-submodules <this-repository-url>
# For an existing checkout:
git submodule update --init --recursive
```

Source dependencies live in `third_party/` as submodules. The parent repository records
exact commits; ordinary builds never update them. Colibri's nested `sds_library`
submodule is required. The original sibling checkouts remain independent.

## Windows

Requirements: Visual Studio 2026 with Desktop development with C++ and C++ Clang
tools for Windows, CMake 3.25+, Ninja, and the LunarG Vulkan SDK (headers and loader
library). Open an **x64 Native Tools Command Prompt** or an x64 Developer PowerShell
before configuring or building with Clang. The standard Windows presets use
`clang-cl` and Ninja Multi-Config, retaining the MSVC ABI, standard library and
static CRT. A Vulkan-capable GPU and driver are required to run graphics.

```powershell
cmake --preset windows -DOGRE_VULKAN_SDK=C:/VulkanSDK/<installed-version>
cmake --build --preset windows-debug --parallel 8
ctest --preset windows-debug
./out/build/windows-clang-ninja/bin/Debug/ogrethrone.exe
```

CPU-only checks require neither the Vulkan SDK nor a graphics/audio device:

```powershell
cmake --preset windows-headless
cmake --build --preset windows-headless --parallel 8
ctest --preset windows-headless
```

MSVC compatibility checks use `windows-msvc` / `windows-msvc-headless` in a separate
build directory; `windows-release` / `windows-msvc-release` build Release. See
[documents/building.md](documents/building.md) for MSVC, Linux, VS Code, the Steam
Audio SDK download and the full dependency policy.

## Projects

`ogrethrone` without arguments shows the project launcher; `ogrethrone <folder>` opens
a project directly. A project is a folder containing `project.throne.lton`:

```text
<project>/
    project.throne.lton   # name and optional mainLevel
    main.lh               # the game's entry point (generated, optional)
    lhat-host.json        # written by the editor for the L^ language server
    assets/               # levels, scenes, prefabs, mixers, scripts, models, textures, sounds
    baked/                # GI bakes, by scene ID
    .throne/              # the editor's cache (asset index); not for version control
```

Data files are LTON (L^ Table Object Notation): levels (`*.level.lton`) hold scene
instances, scenes (`*.scene.lton`) and prefabs (`*.prefab.lton`) hold node hierarchies
with components. Every file under `assets/` has a persistent ID, and files refer to
each other by ID. Samples live in [`samples/`](samples).

## Documentation

- [Editor](documents/editor.md): launcher, panels, Browser, scenes/prefabs/levels,
  overrides, navigation, command-line checks.
- [Scripting](documents/scripting.md): Play mode, Behaviour and Render Event
  components, execution schedule, the `throne` module, Console, language server.
- [Rendering](documents/rendering.md): default rendering, materials, models and
  animation, environment maps, global illumination, post-processing, `--capture`.
- [Physics](documents/physics.md): primitives, colliders, rigid bodies, physics checks.
- [Audio](documents/audio.md): audio components, mixers, Steam Audio, tests.
- [Input devices](documents/input.md): OIS gamepads and joysticks.
- [Building and dependencies](documents/building.md): toolchains, VS Code, Linux,
  dependency policy.

## Dependency rules

- Dependencies are pinned submodules; update one by checking out the intended commit
  inside it, building and testing, then committing the new pointer.
- Keep local upstream modifications in explicit commits/forks, not in build scripts.
- Build only one ImGui (OGRE's ImGui component); ImGuizmo stays at `22a6c90` with
  ImGui `v1.91.6-docking`.
- Do not enable fast-math for Box3D or the simulation.
- MSVC dependencies use the static runtime (`/MT`, `/MTd`).
- Use `-DTHRONE_LHAT_SOURCE_DIR=<path>` to build against a sibling L^ checkout; the
  default is the pinned `third_party/lhat`.
