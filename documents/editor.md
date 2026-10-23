# Editor

## Launcher

`ogrethrone` without arguments shows the project launcher: create a project, import
an existing project folder, or click a known one to open it in the editor.
On Windows, **Browse...** beside the import folder or new project's parent folder
opens the native folder picker. Selecting a folder fills the path; cancelling keeps it unchanged.
The editor bundles Noto Sans JP for Japanese project names, paths and text input, with the
common Simplified Chinese characters it lacks from Noto Sans SC (for Chinese asset names);
no system font installation is needed. Their license is in `media/fonts/OFL.txt`.
`ogrethrone <folder>` opens that project directly. The known projects are kept in
`%APPDATA%/OgreThrone/projects.lton` (`$XDG_CONFIG_HOME/ogrethrone/` elsewhere);
removing one from the list leaves its folder alone. File > Close Project returns
to the launcher.

## Projects and assets

A project is a folder containing `project.throne.lton`:

```text
<project>/
    project.throne.lton   # name and optional mainLevel
    main.lh               # the game's entry point (generated, optional)
    lhat-host.json        # written by the editor for the L^ language server
    assets/               # levels, scenes, prefabs, mixers, scripts, models, textures, sounds
    baked/                # GI bakes, by scene ID
    .throne/              # the editor's cache (asset index); not for version control
```

Files under `assets/` can be arranged freely. Each has a persistent ID: levels, scenes,
prefabs and mixers hold theirs as a top-level `id`, every other file in a `<name>.meta`
beside it, created when the project is opened. Files refer to each other by these IDs, so
moving or renaming one keeps every reference to it; move a file's `.meta` along when moving
it outside the editor. A reference may also be written by hand as a path relative to
`assets/`, which becomes an ID the next time the file is saved. `ogrethrone --resave-assets
<project>` gives every file its ID and rewrites every reference by ID. Paths starting with
`@ogre/` refer to the pinned OGRE sample media. Scripts are read from `assets/` (main.lh from
the project's root) and cannot read files outside it. [`samples/basic`](../samples/basic) is a sample project; the VS Code
launch configurations open it or show the launcher.

[`samples/showcase`](../samples/showcase) ports the OGRE Forward3D, PBS Materials,
Area Lights and ParticleFX2 scenes to editable Throne levels, and adds one comparing
subsurface scattering and translucency. Open the project and
double-click a level
in the **Browser** (`levels/`). **Lighting** switches Forward3D/Clustered, cluster debug
and Approx/LTC area-light previews. Light and PBS Material components are editable
in Inspector and persist with scenes and prefab overrides. The particle level
includes a fountain, rings, lit/unlit smoke and sphere particles, with edit
preview, Play/Pause/Stop and transparency comparison in **Particles**. A Particle
System component stores its template and optional script file. See the sample's
[README](../samples/showcase/README.md) for controls and source attribution.

The editor's dockable panels are Scene, Systems, Inspector, Viewport, Browser and Console
(the layout is kept in `editor.ini` beside the project list).
Scene and Systems scroll sideways when their rows are wider than the panel; the
Inspector wraps its text instead. Dragging the gap between the Inspector's fields and
their labels sets the width of the label column (kept per user in `editor.lton`).

The **Browser** shows `assets/`: its folders as a tree on the left, and the chosen folder's
files on the right, as a grid of tiles (image files show a thumbnail; the slider sizes the
tiles) or as a list, with a breadcrumb and a name filter. Files are shown without their
extension (everything from the first dot, such as `.scene.lton`), which the icon and the
tooltip tell; renaming keeps it, and names made in the Browser cannot hold a dot. Double-clicking a file opens it: a
level replaces the open one, a scene or prefab is edited, a model opens alone, a mixer opens
in the Mixer panel, and anything else (a script, a sound) opens in the operating system's
application for it. Right-clicking a file renames (F2), duplicates (with a new ID), deletes
(Delete; after confirming, references to it show as missing), copies its path or shows it in
Explorer; right-clicking the empty space makes a new folder, level, scene, prefab, mixer or
script. **Refresh** (Ctrl+R) scans `assets/` again for files added or changed outside the
editor; files and folders dropped from Explorer onto the Browser are copied into its folder
and get their IDs at once. Dragging files onto a folder (in either pane) moves them, keeping every reference to
them. Dragging a prefab or model onto the hierarchy or the viewport places it (a scene too, at
the Level step); Add Scene and Add Instance place the scene or prefab selected in the Browser.

## Scenes, prefabs and levels

The data is split
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
  and its own systems. A node of a scene or prefab can be an *instance* of a
  prefab: it becomes the prefab's root, giving it a name and transform, and
  stores only its overrides of the prefab's properties and system field values.
  Prefabs nest; a prefab that contains itself is reported. A **variant** is a
  prefab file with no nodes, only a `base` prefab and overrides of it.
- A **Behaviour component** holds object-oriented script instances, each with its
  own data and methods. **BehaviourSystems** query and call their phase methods.
  In Inspector it uses the same collapsible section and close button as other
  components. Add it through **Add Component → Behaviour**, then use **Add Script**
  inside the section to attach more scripts.
  Custom systems can also query components or use node references; scenes and
  levels can both hold them. Systems have no transform or scene-hierarchy entry.
- A **level** (`*.level.lton`) holds scene instances and its own systems. The
  same scene can be placed more than once; each instance has its own transform,
  under which all of the scene's nodes lie. A level holds no nodes, no prefab
  instances and no overrides of its own: editing a scene's content at the Level
  step writes to that scene's file, so every instance of it shows the change.
- A **Camera** component keeps a viewpoint: the node's position and orientation (it
  looks along its local -Z, +Y up), with a field of view, near/far clip distances,
  depth of field (a focus distance and an f-stop; 0 keeps everything in focus) and
  **Anti-Aliasing**: **TAA** (the default), **SMAA** or **None**. Alpha hashing and the
  noise of SSAO, SSR and depth of field average out only under TAA; orthographic views
  use SMAA instead. Its Inspector has **Align View to Camera** (the scene view takes its
  position, direction and field of view; the view has no roll) and **Align Camera to
  View** (the node's Transform and field of view take the view's; one undo step). The
  scene view keeps its own clip distances, depth of field and anti-aliasing, edited in
  the level's Inspector. Levels keep no view of their own; `--capture` looks through the
  level's first Camera node, with all of its settings.

Levels, scenes and prefabs are named after their files (`main.scene.lton` shows
as `main [Scene]`; a scene placed twice adds its instance ID). The editor drills
down: at the Level step the hierarchy panel is named **Level** and lists, under a row for
the level itself, the scene instances (inside a scene or prefab it is named **Scene**).
Selecting the level's row shows its Inspector: the scene view's camera (position, rotation
about X then Y, field of view and clip distances), edited there as by navigating; it is not
saved. Selecting a scene instance shows its transform in the Inspector and the gizmo,
and double-clicking one (or a scene file in the Browser) edits that
scene, and double-clicking an instance node (or a prefab file) edits
that prefab. The breadcrumb over the viewport returns to an outer step. Nodes and
systems that come from an instance are drawn in blue; editing them writes an
override, shown in orange with a Revert button. With a node inside an instance
selected, Add Node and Add Instance add
the new node as an override of that instance, also shown in orange; Delete on a
node inside an instance records its removal, which the instance's Inspector
lists with a Restore button. Systems work the same way: + System with an
instance's system or a node inside it selected adds one to that instance, and
Delete on an instance's system records its removal. Add Instance places the prefab selected
in the Browser; a new prefab is made with the Browser's **New Prefab**.
A node's Inspector lists the system fields that refer to it (Referenced by);
clicking one selects that system.
Each override has an Apply button beside its Revert (a property, a component, a
script setting or field of an attachment, a system field, a removal, or a node, system
or attachment the instance added):
it writes that override into the prefab, saving it at once, and removes it from the
instance. The instance's Inspector has Apply All and Revert All for all of them.
When the prefab no longer has what an override names (a node, a system, or a
component), the override is listed in grey under the instance with a Remove button
(and Remove All). These overrides are never removed automatically, so a prefab that is
broken for a while (a branch switch, an undone deletion) does not lose them. The
overrides of an instance whose prefab cannot be read are kept, but not listed.
Unpack in the Inspector replaces an instance with plain nodes holding what it
shows, nested instances included, and copies its systems into the file.
Dragging a node onto a Browser folder saves it there as
`<name>.prefab.lton` and leaves an instance in its place; dragging an
instance makes a variant of its prefab. Dropping a node on an existing prefab
file asks before replacing that prefab with it; dropping an instance on its own
prefab applies the instance's overrides to the prefab. Variants are shown in violet.
Ctrl+S saves the level, its scenes and every opened prefab.

## Navigation and command-line checks

Right-drag looks around in place; hold the right button and use WASD to fly
forward/backward and strafe left/right (initially 5 scene units per second).
Q descends and E ascends along the world's Y axis while flying.
While holding the right button, the wheel adjusts fly speed instead of dollying;
hold Left Shift to double the current speed. Alt+middle-drag
orbits the focus point. A middle-click focuses the nearest rendered surface without
moving the camera; clicking empty space leaves the focus unchanged. Middle-drag
pans, Ctrl+Alt+middle-drag vertically dollies (up moves closer, down moves away),
the wheel dollies outside fly mode, left-click selects a node
and `F` frames it. The selected node shows an ImGuizmo gizmo: W, E and R (or the
viewport toolbar) switch between move, rotate and scale, and Local switches between
the node's own axes and the world's; one drag is one undo step. Ctrl+Z/Ctrl+Y undo/redo, Ctrl+D duplicates a node (with copies of
the systems that refer to it, referring to the copy), Delete
removes the selected node or system, Ctrl+S saves the level and its scenes and
Ctrl+O opens a level. New and Open are undoable; closing the window or the project with
unsaved changes asks to save or discard them (on Windows this also covers the
window's close button). Colibri widgets are not wired yet.
`ogrethrone --smoke` creates a throwaway project, runs the editor on it for three
frames and exits, without touching the project list.
With `BUILD_TESTING=ON`, `ogrethrone --smoke-navigation` replays mouse and keyboard
input against a temporary copy of `samples/physics`, checking surface focus,
empty-space clicks, orbit, looking in place, WASD movement, panning, dolly,
wheel-adjusted fly speed and Left Shift boost.
On Windows it also sends a native Close request during Play and verifies that
rendering and simulation do not run another frame before shutdown. Use
`ogrethrone --smoke-close` for the same checks with the real audio output backend
instead of the null driver. Shutdown stages are recorded in `OgreThrone.log`;
the final OGRE marker is also written to standard error.
**View → Statistics** toggles a viewport overlay showing averaged editor FPS and
frame time, scene node count and active physics body count. Its **− / +** button
switches between the full panel and a single FPS row. Dragging its left or bottom edge
(or the corner between them) resizes it; the size is kept in `editor.lton`. Expanded statistics also
show CPU wall times for UI/scripts, physics, scene copying, render synchronization,
render/present and waiting for simulation. With frame delay, simulation and rendering
overlap; render/present includes GPU/driver/display waits and is not a GPU timestamp. The panel is enabled
by default.

`ogrethrone --profile-physics [project] [output.csv]` profiles a temporary copy of
`samples/physics` by default. It advances at a fixed 1/60 s without the main-loop
sleep, disables VSync for the test, writes per-frame wall times, and reports the
average at 300–329 physics bodies. The default output is `out/physics-profile.csv`.
Use matching Debug/Release builds and compare measured milliseconds; the test's
fixed simulation delta makes the displayed FPS unsuitable for this comparison.
It uses OGRE's native window and message pump. HlmsUnlit shader templates are read
from the pinned `third_party/ogre-next/Samples/Media`, so the executable currently
needs the source tree.
