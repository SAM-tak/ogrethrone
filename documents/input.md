# Input devices

OS windows are created by OGRE. OIS is built statically from the pinned
[`SAM-tak/OIS` `for-throne` branch](https://github.com/SAM-tak/OIS/tree/for-throne),
with its demos disabled and Windows DirectInput/XInput support enabled.
The editor opens attached joysticks/gamepads at startup, captures them before UI
and simulation each frame, and releases them before the native window is destroyed.
No controller is required to start the editor; the detected count and device names
are recorded in `OgreThrone.log`.

`OisInput` in `src/input/` provides `joystickCount()` and read-only `joystick(index)`
access. Include `<OIS.h>` to inspect a device's `getJoyStickState()` (buttons, axes,
POVs and sliders). These are raw OIS device states; action bindings, script APIs,
automatic hotplug discovery and controller navigation of the editor are not yet
implemented. Connect devices before launching. DirectInput devices use foreground,
nonexclusive access. Editor keyboard/mouse and Unicode/IME text input continue
through the native ImGui platform backend.

OIS uses native input backends, without SDL. Linux's native backend requires X11
development packages; CPU-only headless builds do not build OIS. OIS retains its
license in `third_party/ois/LICENSE.md`.
