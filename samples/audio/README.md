# Scene audio sample

Open this folder as a project, then press F5. The visible sphere moves left and
right while looping a Kenney footstep. The scene Listener is at (0, 1, 0), facing -Z.
During editor Play, Level Inspector > Scene View Audio Listener is enabled by
default: the visible Scene View camera is the highest-priority listener. Fly or
orbit the camera to listen from that position. Turn this option off to use scene
Listener priorities. The option is not saved with the level; a hidden Scene View
also falls back to scene listeners.
A wall between the source and Listener muffles the footstep as it crosses behind
it; the floor, enclosing walls and ceiling also reflect sound. The moving source
has Reflections / reverb enabled at a wet gain of 0.7. A Kenney UI cue plays once on its own bus
and bypasses geometry as a 2D sound. Pause preserves playback positions;
Stop releases all voices. Editing the scene does not autoplay sounds.

Select the level to see its Mixer reference. In Browser > Mixers, open
`mixers/main.mixer.lton`. Adjust SFX, UI, Music or Master gain/mute during Play.
Mixer edits participate in Save and Undo/Redo. Missing output bus IDs use Master
and report a warning. Bus IDs survive renaming and reparenting.

The fourth node contains a 12-second stereo streaming fixture, initially stopped.
Use Console during Play to control it (import `throne` first):

```lhat
import^ throne
let^ node = throne.find("Streaming Stereo")
if^ node != nil^ {
    let^ sound = node.audioSource()
    if^ sound != nil^ { sound.play() }
}
# Use sound.pause(), sound.play() or sound.stop() in the same guarded form.
```

Each AudioSource owns at most one voice. `play()` resumes a paused voice, starts
a stopped/finished voice from the beginning, and leaves an already playing voice
alone. Commands apply at the next SoLoud fixed-step stage. `playing()` reports
the last applied state. `setVolume`, `setLoop`, `setBus`, `setEnabled`, and the
Listener's `setPriority`/`setEnabled` are available to Behaviour scripts. Typed
`node.component(throne.AudioSource)` and component queries are supported too.

3D clips must be preloaded mono; 2D clips can be preloaded or streamed mono/stereo.
Streaming uses a bounded producer buffer so disk reads and decoder rewinds never
run in the audio callback. If that buffer underruns, the callback inserts silence.
The highest-priority enabled Listener wins; equal priorities use scene/hierarchy
order. With no Listener, 3D playback advances silently while 2D playback continues.

## Assets and checks

`audio/kenney` includes one original file and its CC0 license from each of Kenney's
Impact Sounds 1.0 and Interface Sounds 1.0. The first audio filename in lexical
order was selected in each archive and verified non-silent. Impact's original is
stereo; its separate mono PCM16 derivative is used by the 3D source. Each pack's
README records origin, download URL, retrieval date, conversion and SHA-256.
No Kenney branding is bundled. `audio/generated` contains project-created CC0
noise and stereo signals for deterministic/streaming checks. Regenerate these
with `python tools/generate-audio-signals.py` from the repository root.

Run `ogrethrone --smoke-audio` to check the editor panels, save, Undo/Redo and
Play lifecycle in a throwaway copy of this project.

Run `throne_audio_world_smoke` for device-independent integration checks, or
`throne_audio_world_smoke --device` for a short real-output moving-source check
including pause/resume. Source and Mixer data are checked without network access.

## Occlusion and transmission

Select Acoustic Wall or Floor in the Inspector to edit Acoustic Geometry before
Play. Shape `Render Geometry` uses the node's primitives or FBX rest geometry.
`Box Proxy` supplies an invisible box of the chosen dimensions. Participation is
explicit and independent of Collider components. Coefficients run from 0 to 1:
transmission controls how much of each frequency band crosses each surface.
Absorption and scattering affect the simulated reflections and reverb.

A moving wall's transform is updated without rebuilding its local mesh. Disabling
or removing Acoustic Geometry opens the direct path. FBX geometry follows rigid
node movement; skeletal deformation and OGRE .mesh acoustic imports are not
supported. Use a Box proxy where appropriate.

Audio Source > Occlusion bypasses both occlusion and transmission when disabled.
With Occlusion enabled and Transmission disabled, a fully blocked direct path is
silent on the direct path; disable Reflections too to isolate this check. Enable Transmission to hear the material's frequency-dependent filtering.
2D sources continue to bypass acoustic simulation.

During Play, Console can alter the wall's material using the typed component API:

```lhat
import^ throne
let^ wall = throne.find("Acoustic Wall")
if^ wall != nil^ {
    let^ geometry = wall.acousticGeometry()
    if^ geometry != nil^ { geometry.setTransmission(throne.vec3(0.8, 0.5, 0.2)) }
}
```

`throne_scene_acoustics_smoke` verifies raycast results and PCM frequency changes,
moving/scaled/rotated walls, FBX import, materials, serialization and script APIs.

## Reflections and reverb

Select Moving Footstep > Audio Source to enable/disable Reflections / reverb and
change Reflection volume before Play. These settings also support prefab
serialization, Undo/Redo and the script methods `setReflections` and
`setReflectionVolume`. Source volume and its SFX bus affect direct and wet sound;
Reflection volume affects wet sound alone. Disable Transmission while leaving
Reflections on to hear sound arriving around the dividing wall.

Change the walls' absorption toward 1 to suppress reflections, or lower it to
hear a longer tail. Scattering controls diffuse reflection. Reflections run on a
separate worker at up to 10 Hz, so changes become audible after a completed job;
startup initially plays direct sound alone. Listener rotation updates the binaural
decode at every fixed step. Pause freezes both paths; Stop clears them.

The initial quality budget is 16 sources, 4096 rays, 16 bounces, first-order
Ambisonics and a one-second convolution IR. Long hall tails, baking, Doppler,
OneShot and separate reflection sends remain future work. Other sources opt in
explicitly with `reflections = true^`; the default is false.

`throne_reflections_smoke` checks actual wet PCM behind an opaque wall, decay
after the source ends, absorption, IR ownership after simulator destruction,
concurrent IR publication, script controls, Mixer mute and pause/listener handling.
