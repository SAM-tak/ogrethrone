# Ogre Throne

A C++ game engine using OGRE-Next 4.0 development sources for Vulkan graphics,
Box3D for physics, L^ for scripting, Dear ImGui for editor UI, Colibri for
in-game UI, and SoLoud + Steam Audio for sound. The name is currently a working name.

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
static CRT. Visual Studio's LLVM directory is added to PATH by the preset.
A Vulkan-capable GPU and driver are required to run graphics.

```powershell
cmake --preset windows -DOGRE_VULKAN_SDK=C:/VulkanSDK/<installed-version>
cmake --build --preset windows-debug --parallel 8
ctest --preset windows-debug
./out/build/windows-clang-ninja/bin/Debug/ogrethrone.exe
```

The executable embeds a manifest (visual styles, per-monitor DPI awareness, UTF-8)
and the application icon `media/ogrethrone.ico`; after editing the
`media/ogrethrone<size>.png` images, run `tools/make-icon.ps1` to repack it. The
title bar follows Windows' light or dark app mode.

`VULKAN_SDK` can also locate the SDK. A terminal opened before SDK installation
may need restarting, or use the explicit CMake option above.
The pinned glslang submodule is built from source for each configuration, using
the same MSVC runtime as OGRE. SDK shader compiler libraries are not linked.

`ogrethrone` without arguments shows the project launcher: create a project, import
an existing project folder, or click a known one to open it in the editor.
`ogrethrone <folder>` opens that project directly. The known projects are kept in
`%APPDATA%/OgreThrone/projects.lton` (`$XDG_CONFIG_HOME/ogrethrone/` elsewhere);
removing one from the list leaves its folder alone. File > Close Project returns
to the launcher.

A project is a folder containing `project.throne.lton`:

```text
<project>/
    project.throne.lton   # name and optional mainLevel
    main.lh               # the game's entry point (generated, optional)
    lhat-host.json        # written by the editor for the L^ language server
    levels/               # *.level.lton
    scenes/               # *.scene.lton
    prefabs/              # *.prefab.lton
    scripts/              # *.lh
```

Every path inside a project, including those stored in levels and scenes, is
relative to the project folder. [`samples/basic`](samples/basic) is a sample project; the VS Code
launch configurations open it or show the launcher.

The editor's dockable panels are Scene, Runners, Inspector, Viewport, Browser and Console
(the layout is kept in `editor.ini` beside the project list). The data is split
in three kinds of file, all LTON (L^ Table Object Notation, read by L^ as
effect-free data):

- A **scene** (`*.scene.lton`) is a hierarchy of nodes. A node has an ID, a name
  and a parent, and components, each optional: a **Transform** (local position,
  rotation in degrees applied about X, then Y, then Z, and scale; a node without
  one passes its parent's placement on) and a **Box Primitive** (a unit cube and
  its colour). The Inspector shows one section per component, where a node of the
  edited file can add or remove them. Drag nodes in the Scene panel to reparent
  them, or drop one on the top or bottom edge of another to place it before or
  after it; sibling order is kept. A scene is a prefab (`*.prefab.lton`) extended with scene-wide data.
- A **prefab** (`*.prefab.lton`) is a reusable node hierarchy with a single root
  and its own runners. A node of a scene or prefab can be an *instance* of a
  prefab: it becomes the prefab's root, giving it a name and transform, and
  stores only its overrides of the prefab's properties and runner field values.
  Prefabs nest; a prefab that contains itself is reported. A **variant** is a
  prefab file with no nodes, only a `base` prefab and overrides of it.
- A **runner** runs behaviours. It is not part of the scene hierarchy and has no
  transform; it reaches nodes only through its behaviours' fields. Scenes and
  levels can both hold runners.
- A **level** (`*.level.lton`) holds scene instances and its own runners. The
  same scene can be placed more than once; each instance has its own transform,
  under which all of the scene's nodes lie. A level holds no nodes, no prefab
  instances and no overrides of its own: editing a scene's content at the Level
  step writes to that scene's file, so every instance of it shows the change.

Levels, scenes and prefabs are named after their files (`main.scene.lton` shows
as `main [Scene]`; a scene placed twice adds its instance ID). The editor drills
down: at the Level step the Scene panel lists the scene instances; selecting one shows its transform in the Inspector and the gizmo,
and double-clicking one (or a file in the Browser's Scenes tab) edits that
scene, and double-clicking an instance node (or a file in the Prefabs tab) edits
that prefab. The breadcrumb over the viewport returns to an outer step. Nodes and
runners that come from an instance are drawn in blue; editing them writes an
override, shown in orange with a Revert button. With a node inside an instance
selected, Add Node and Add Instance add
the new node as an override of that instance, also shown in orange; Delete on a
node inside an instance records its removal, which the instance's Inspector
lists with a Restore button. Runners work the same way: + Runner with an
instance's runner or a node inside it selected adds one to that instance, and
Delete on an instance's runner records its removal. Inside a scene or prefab, Add
Instance places the prefab named next to it, creating one with a root node when
the file does not exist yet; double-click the new instance node to fill the prefab.
A node's Inspector lists the runner fields that refer to it (Referenced by);
clicking one selects that runner.
Apply in the Inspector writes all of an instance's overrides into its prefab.
Unpack in the Inspector replaces an instance with plain nodes holding what it
shows, nested instances included, and copies its runners into the file.
Dragging a node onto the Browser's Prefabs tab saves it as
`prefabs/<name>.prefab.lton` and leaves an instance in its place; dragging an
instance makes a variant of its prefab. Dropping a node on an existing prefab
file asks before replacing that prefab with it; dropping an instance on its own
prefab applies the instance's overrides to the prefab. Variants are shown in violet.
Ctrl+S saves the level, its scenes and every opened prefab.

Right-drag orbits, middle-drag pans, the wheel zooms, left-click selects a node
and `F` frames it. The selected node shows an ImGuizmo gizmo: W, E and R (or the
viewport toolbar) switch between move, rotate and scale, and Local switches between
the node's own axes and the world's; one drag is one undo step. Ctrl+Z/Ctrl+Y undo/redo, Ctrl+D duplicates a node (with copies of
the runners that refer to it, referring to the copy), Delete
removes the selected node or runner, Ctrl+S saves the level and its scenes and
Ctrl+O opens a level. New and Open are undoable; closing the window or the project with
unsaved changes asks to save or discard them (on Windows this also covers the
window's close button). Colibri widgets and model import are not wired yet.
`ogrethrone --smoke` creates a throwaway project, runs the editor on it for three
frames and exits, without touching the project list.
It uses OGRE's native window and message pump. HlmsUnlit shader templates are read
from the pinned `third_party/ogre-next/Samples/Media`, so the executable currently
needs the source tree.

CPU-only checks require neither the Vulkan SDK nor a graphics/audio device:

```powershell
cmake --preset windows-headless
cmake --build --preset windows-headless --parallel 8
ctest --preset windows-headless
```

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
Select the `windows` configure preset and `windows-debug` build preset, then
select `ogrethrone` with **CMake: Set Debug Target** (or the launch-target picker).
`Ctrl+Shift+B` builds the active preset; **Tasks: Run Test Task** runs CTest.
The supplied Windows F5 configuration launches the selected CMake target with
the Visual Studio debugger. Choose a Debug build for debugging.

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

## Scripting

Scripts are written in L^ and run only in the editor's Play mode (F5 or the Play
menu item). Play runs the open level: its runners and its scenes' runners, but not
`main.lh`. Playing the whole game from `main.lh` will be a separate command. Play runs on a copy of the level and its scenes; Stop discards it, so
script changes never reach the edited data. Editing is disabled while playing.
Each Play checks and compiles every script anew, so edited scripts take effect
on the next Play. Rejected scripts, run-time errors, and frames over the loop
budget (treated as endless loops) are written to the Console and stop Play.

Paths are relative to the project folder. `main.lh` at the top of the project is the
game's entry point and organizer (which levels to play, in what order); new
projects get a placeholder one. It is a plain script (no `module^`); an `await^`
at its top level makes it a coroutine that the whole-game Play will resume once
per frame until it ends. Level playback from it (`throne.playLevel`) is still
being designed (TDDja/02 4.1). A runner holds any number
of behaviours. Each behaviour names a `module^` that publishes a definition called
`Behaviour`, and gets its own instance per runner:

```lhat
module^ samples.spin
import^ throne

public^let^Behaviour = def^{
    self^{
        target : throne.SceneNode|nil^ = nil^,
        degreesPerSecond = 90,
    },
    start = p^self^, runner:throne.Runner { throne.log("hello " .. runner.name()) },
    update = p^self^, runner:throne.Runner, dt:number^ {
        if^ self^.target != nil^ {
            let^ t = self^.target.transform()
            if^ t != nil^ {
                var^ r = t.rotation()
                r.y := r.y + self^.degreesPerSecond * dt
                t.setRotation(r)
            }
        }
    },
}
```

The `self^{ ... }` fields are what the Inspector shows for that behaviour: number,
bool and string fields are edited directly, and `throne.SceneNode` fields pick a
node: a level runner from any scene instance, a scene or prefab runner from its
own hierarchy (each prefab instance gets its own copy of the prefab's runners,
pointing into that instance). A runner stores only the values that differ from the
script's defaults (Reset removes one); a value whose field is gone or has another
type is skipped with a warning in the log.

`start` and `update` are optional; when present their signatures are checked
before Play starts. Order per frame: each scene's runners, then the level's
runners; every `start` runs once before the first `update`.

The `throne` module provides:

- `throne.Vec3`: a value type with `x`/`y`/`z`, `+`, `-`, `*` and `/` by a number,
  `dot`, `cross`, `length` and `normalized`; made with `throne.vec3(x, y, z)`. The
  axes match OGRE, Box3D and Steam Audio: right-handed, +Y up, -Z forward. Like
  any L^ host value it cannot be stored in a table; keep one with `throne.Vec3.Box^`.
- `throne.SceneNode`: `id()`, `name()`, `valid()`, and its components:
  `transform()` (a `throne.Transform`, or nil^ when the node has none) and `box()`
  (a `throne.BoxPrimitive`, or nil^). Using a handle whose node is gone panics;
  check `valid()` first.
- `throne.Transform`: `position()`/`rotation()`/`scale()` and the matching
  `setPosition(v)` etc., relative to the parent; rotation is in degrees, applied
  about X, then Y, then Z, and scale is kept at 0.001 or more.
- `throne.BoxPrimitive`: `colour()` and `setColour(v)`, kept in [0, 1].
- A component handle whose node or component is gone panics.
- `throne.Runner`: `name()`; passed to `start` and `update`.
- `throne.find(name)` (the first node with that name, scene by scene),
  `throne.log(text)` and `throne.time()` (seconds since Play).

`std.math` is the only standard module scripts can import.

The Console panel shows the log, and its bottom line is an L^ prompt. While
playing, lines run on the playing scripts' machine, so `throne.find("Cube 2")`
reaches the running scene; otherwise they run in the console's own session on
an empty world. Names a line binds stay for the next, an unfinished input
continues on the next line (Esc drops it), Up and Down recall earlier lines,
and refused inputs and run errors are reported without stopping Play.

The L^ language server (VS Code extension) reads the `lhat-host.json` nearest to a
script to know the `throne` module. The editor rewrites a project's copy whenever
the project is created or opened and the bindings have changed. To write one
without opening a window:

```powershell
./out/build/windows-clang-ninja/bin/Debug/ogrethrone.exe --dump-host-api [file]
```

The file defaults to `lhat-host.json` in the working directory.

In [`samples/basic`](samples/basic), the scene's SpinningCube runner gives
[`spin.lh`](samples/basic/scripts/spin.lh) the SpinningCube node as its `target`.
The design is recorded in [TDDja/02-scripting.md](TDDja/02-scripting.md),
[TDDja/05-runner.md](TDDja/05-runner.md) and [TDDja/06-prefab.md](TDDja/06-prefab.md).

## Audio

SoLoud is pinned as a submodule. The parent builds its mixer, WAV/Ogg Vorbis/MP3
(also FLAC) decoders, Wav/WavStream sources, miniaudio device backend, and null
backend. Synthesizers, OpenAL, SDL and optional module players are not built.

CMake downloads the official Steam Audio **4.8.1 C SDK** at first configuration
(about 181 MB), verifies the pinned SHA-256, and caches the archive and extracted
SDK in `.fetchcontent-cache/`, shared by all presets. Each build tree keeps its own
generator-specific FetchContent sub-build. Both directories are ignored.
This uses Valve's prebuilt library, not a local build of the Steam Audio sources.
For offline configuration or a shared SDK cache, point to the extracted directory
containing `include/phonon.h` and `lib/`:

```powershell
cmake --preset windows -DTHRONE_STEAM_AUDIO_ROOT=C:/SDKs/steamaudio
```

The adapter maps Windows x64, Linux x64, and macOS SDK libraries. Only Windows
has been verified. `phonon.dll` (or the platform equivalent) and audio license
notices are copied next to each audio executable. CPU processing uses the default
Steam Audio scene backend; optional GPU/OpenCL acceleration is not configured.

`AudioSystem` initializes Steam Audio and one stereo 48 kHz SoLoud output device.
The editor initializes this system on startup; `--smoke` uses null audio output.
There must be only one device-backed AudioSystem at a time because the upstream
miniaudio backend has global device state. Keep the system alive longer than its
sound sources. Standard SoLoud Wav/WavStream sources handle stereo BGM and UI audio.

`SpatialSound` preloads a mono clip, applies Steam Audio direct-path effects and
HRTF, and supplies stereo PCM to SoLoud. Direction is fixed in listener coordinates;
direct parameters can be supplied from Steam Audio simulation results. Each voice
has independent effect state, handles arbitrary requested buffer lengths, and
drains its HRTF tail. Looping repeats from the start. Play it through `play()`,
not `play3d()`, to avoid duplicate panning/attenuation. Keep the source alive while
playing; destroying it stops its voices.

```cpp
throne::AudioSystem audio;
throne::SpatialSound sound(audio, "footstep.wav", {1, 0, -1});
auto voice = audio.mixer().play(sound);
// Keep audio and sound alive until playback finishes.
```

Automated tests use null output and run with the normal CTest commands:

- `audio_smoke`: WAV/Ogg/MP3 decoding, streaming, spatial playback, independent
  ear signals, direct-path attenuation, finite output, and voice completion.
- `acoustics_smoke`: a room and wall mesh, simulated occlusion/material
  transmission through the production bridge, and nonzero reflected PCM from
  Steam Audio's convolution effect.

Verified on Windows: Debug/headless and Release audio tests passed, including
matching spatial PCM for 31- and 777-frame read requests. Device initialization
and playback completion passed in Debug and Release. The editor's three-frame
Vulkan smoke run also passed in both configurations after adding audio. The
headless preset successfully fetched and verified the SDK without a root override.

An optional device check plays a short, quiet spatialized tone:

```powershell
./out/build/windows-clang-ninja/bin/Debug/throne_audio_smoke.exe --device
```

This is an audio bootstrap. Moving-source/listener updates, OGRE/Box3D acoustic
mesh export, acoustic material authoring, reflection buses in the runtime mixer,
background simulation jobs and baking tools are not implemented yet. Reflection
simulation/rendering is currently exercised by the diagnostic test only.

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
- MSVC dependencies use the static runtime consistently (`/MT`, `/MTd`).
- Initially assets are unpacked; OGRE ZIP, FreeImage and dependency demos/tools
  are disabled. RapidJSON is pinned for Colibri skins and OGRE materials.
  PBR, Unlit and particle plugins remain
  available. Asset packaging/import tooling is a separate milestone.
- No TAA, Alpha Hashing configuration, shader warm-up policy, or disk cache
  persistence is implemented by this bootstrap.
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
