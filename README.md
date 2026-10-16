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

Requirements: Visual Studio 2026 with Desktop development with C++, CMake 3.25+,
and the LunarG Vulkan SDK (headers and loader library). The existing preset uses
VS 2026; a VS 2022 user can inherit it in `CMakeUserPresets.json` and override the
generator. A Vulkan-capable GPU and driver are required to run graphics.

```powershell
cmake --preset windows -DOGRE_VULKAN_SDK=C:/VulkanSDK/<installed-version>
cmake --build --preset windows-debug --parallel 8
ctest --preset windows-debug
./out/build/windows/bin/Debug/ogrethrone.exe
```

`VULKAN_SDK` can also locate the SDK. A terminal opened before SDK installation
may need restarting, or use the explicit CMake option above.
The pinned glslang submodule is built from source for each configuration, using
the same MSVC runtime as OGRE. SDK shader compiler libraries are not linked.

`ogrethrone --smoke` opens a Vulkan window, renders three frames, and exits.
This initial executable clears a window and initializes physics and UI objects;
model rendering, ImGui drawing/input and Colibri widgets are not wired yet.
It currently uses OGRE's native window and message pump.

CPU-only checks require neither the Vulkan SDK nor a graphics/audio device:

```powershell
cmake --preset windows-headless
cmake --build --preset windows-headless --parallel 8
ctest --preset windows-headless
```

The smoke check initializes an L^ VM, simulates a box falling onto a ground plane,
and compares all 180 positions against a second identical run. This verifies
same-build repeatability; it does not establish cross-platform determinism.

Verified locally on Windows with Visual Studio 2026, Vulkan SDK 1.4.363.0 and an
NVIDIA GeForce GTX 1660 Ti: Debug and Release builds, the CPU smoke test in both
configurations, and three-frame Vulkan runs all passed. Both graphics runs also
compiled the ImGui vertex and fragment shaders successfully.

## Audio

SoLoud is pinned as a submodule. The parent builds its mixer, WAV/Ogg Vorbis/MP3
(also FLAC) decoders, Wav/WavStream sources, miniaudio device backend, and null
backend. Synthesizers, OpenAL, SDL and optional module players are not built.

CMake downloads the official Steam Audio **4.8.1 C SDK** at first configuration
(about 181 MB), verifies the pinned SHA-256, and caches it locally under
`FETCHCONTENT_BASE_DIR`. Both the build tree and `.fetchcontent-cache/` are ignored.
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
./out/build/windows/bin/Debug/throne_audio_smoke.exe --device
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
- The pinned OGRE build scripts re-add the disabled FreeImage codec via their
  unity-build exclusion list. The parent CMake adapter removes that source from
  the target; upstream source files are unchanged.
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
