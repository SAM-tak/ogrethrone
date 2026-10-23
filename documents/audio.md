# Audio

Scene audio is integrated through `AudioSource` / `AudioListener` components and
`SteamAudio` then `SoLoud` stages after `Box3DStep` inside the fixed UpdateLoop.
The backend audio callback independently processes PCM and output. Play owns the
voices; Pause preserves positions and Stop releases them. See
[samples/audio](../samples/audio/README.md) for the Kenney CC0 demo, Mixer editor,
script APIs and headless/device checks, and [the audio design](../TDDja/14-audio.md).
The level's optional `mixer` selects a `*.mixer.lton` bus tree. The Browser
creates (New Mixer) and opens these assets; the Mixer panel edits gain, mute and routing with
Save and Undo/Redo. Unspecified mixers use Master alone. Spatial playback uses
mono preloads; 2D supports mono/stereo preloads and buffered streaming. Highest
Listener priority wins, with scene/hierarchy order breaking ties. In editor Play,
the visible Scene View camera takes precedence while Level Inspector > Scene View
Audio Listener is enabled (default); disabling it restores scene priorities. No Listener
silences 3D playback without stopping its clock. `AcousticGeometry` adds explicit
acoustic participation independently of colliders: use a node's primitives/FBX
rest geometry or an invisible Box proxy, with per-band absorption/transmission
and scattering. Steam Audio raycasts occlusion and transmission in the simulation
stage; direct-path results reach the audio callback as one coherent snapshot.
Moving geometry updates instance transforms; geometry/material edits rebuild only
the affected mesh. Source `occlusion`/`transmission` switches can bypass these
paths. Opt-in source `reflections` adds simulated early reflections and reverberation:
a dedicated worker traces immutable scene snapshots at up to 10 Hz; the callback
convolves the resulting one-second, first-order Ambisonics IR and decodes it using
the current Listener orientation. Reflections share the source bus and have a
separate `reflectionVolume`. The fixed budget covers 16 sources, 4096 rays and 16
bounces. Baking remains future work.

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
HRTF, and supplies stereo PCM to SoLoud. Direction starts in listener coordinates and can be updated during playback;
direct parameters can be supplied from Steam Audio simulation results. Each voice
has independent effect state, handles arbitrary requested buffer lengths, and
drains its HRTF and convolution tails. Imported mono clips are resampled to the
engine's 48 kHz rate outside the callback. Looping repeats from the start. Play it through `play()`,
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

Scene geometry/material authoring, moving sources/listeners and asynchronous
reflection simulation are integrated into the editor. `scene_acoustics_smoke`
checks direct propagation and `reflections_smoke` checks wet PCM, post-source
reverb tails, absorption, concurrent IR publication and lifecycle/routing.
The sample includes an enclosed room with the moving Kenney source's reflections
enabled. Independent reflection sends, longer IRs/quality controls, baking,
Doppler and OneShot are future work.
