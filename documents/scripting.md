# Scripting

Scripts are written in L^ and run only in the editor's Play mode (F5 or the Play
menu item). Play runs the open level: its systems and its scenes' systems, but not
`main.lh`. Playing the whole game from `main.lh` will be a separate command. Play runs on a copy of the level and its scenes; Stop discards it, so
script changes never reach the edited data. Editing is disabled while playing.
Each Play checks and compiles every script anew, so edited scripts take effect
on the next Play. Rejected scripts, run-time errors, and frames over the loop
budget (treated as endless loops) are written to the Console and stop Play.
**F5** toggles Play/Stop. During Play, the **Pause** menu-bar button or keyboard
**Pause** key suspends scripts and physics, including spawning and simulation time.
The button becomes **Resume**; clicking it or pressing Pause again continues the
same session. Rendering, statistics and viewport navigation remain active. Stop
also works while paused, and a new Play session always starts unpaused.

`main.lh` at the top of the project is the
game's entry point and organizer (which levels to play, in what order); new
projects get a placeholder one. It is a plain script (no `module^`); an `await^`
at its top level makes it a coroutine that the whole-game Play will resume once
per frame until it ends. Level playback from it (`throne.playLevel`) is still
being designed (TDDja/02 4.1).

The Behaviour component at `components.behaviour` holds a list of script references
and per-instance `data`, with optional enabled and priority overrides. Each script
instance owns both its data and methods; even repeated attachments of the same
script have independent state. Each attachment gets a fresh
instance for each scene/prefab instance. A script publishes `Behaviour = def^{ ... }`:

```lhat
module^ samples.spin
import^ throne
public^let^ Behaviour = def^{
    self^{ @export degreesPerSecond = 90 },
    update = p^self^, node:throne.SceneNode, dt:number^ {
        let^ t = node.transform()
        if^ t != nil^ {
            t.setRotation(t.rotation() + throne.vec3(0, self^.degreesPerSecond * dt, 0))
        }
    },
}
```

The scene representation is:

```lton
components = {
    behaviour = {
        { script = "scripts/spin.lh", data = { degreesPerSecond = 45 } },
        { script = "scripts/pop.lh", data = { interval = 1.5, height = 0.5, duration = 0.4 } },
    },
},
```

The Inspector edits exported members in `data`; the runtime writes them into the
same instance that owns the callbacks. A prefab instance overrides the prefab's
attachments one by one, found by their `id` (an attachment without one gets its place in
the list, from 1), under `overrides.nodes[].behaviour`:

```lton
behaviour = {
    changed = { { id = 1, data = { degreesPerSecond = 1360 }, enabled = false^ } },
    added = { { script = "scripts/pop.lh", id = 7484712563498500123, data = {} } },
    removed = { 2 },
},
```

`order` (attachment IDs), written when an instance's attachments are dragged into another
order, puts them in that order; attachments it does not list follow in their own. In the
Inspector, dragging an attachment's header onto another's moves it there, in the edited
file's own nodes and in instances alike.
`changed` sets fields and settings (`script`, `enabled`, the priorities; a priority of
`false^` returns to the script's default) over the prefab's, so the prefab's other fields
still reach the instance.

`start(node)`, `update(node, dt)`, `lateupdate(node, dt)` and
`onendofframe(node, dt)` are optional. The three BehaviourSystems query enabled
attachments implementing their corresponding method; an instance shares its state
across all phases. `start` runs once before the first callback (start-only
attachments start in Update). Direct system attachments remain available for
orchestration; their callbacks receive `throne.System` instead.

Order is ascending finite real priority, with scene-instance, depth-first hierarchy
and attachment order breaking ties. A script declares defaults in `self^{ priority
= 0, updatePriority = ..., lateupdatePriority = ..., onendofframePriority = ... }`.
Each attachment can override the common priority or individual phases. Precedence
is attachment phase, attachment common, script phase, script common, then zero.
The Inspector edits these overrides; Systems shows the resolved execution order.
Queries are cached; adding/removing attachments or changing enabled/priority takes
effect at the next frame boundary. Sorting is done when configuration changes.

Callbacks accept ordinary functions and procedures. Their result is ignored unless
it is a coroutine, in which case the system drives it immediately and retains its
suspended invocation. `yield^ nil^` waits for the next visit of the originating
system; `yield^ target` waits for a script `throne.System`. A later stage can resume
in the same UpdateLoop pass; the current or an earlier stage resumes in the next pass
(EndOfFrameBehaviourSystem and the stages after it: the next frame). Each method
is independent and never overlaps itself. Disabling freezes pending invocations;
removing an attachment or stopping Play disposes them, running `finally^`.

```lhat
update = p^self^, node:throne.SceneNode, dt:number^ {
    let^ end = throne.system("EndOfFrameBehaviourSystem")
    if^ end != nil^ { yield^ end }
    // Continue after render/physics synchronization.
},
```

The Inspector edits exported number, bool, string and node-reference fields. Other
Behaviour fields remain private. Reset removes an override; missing or mistyped
fields are ignored with a warning. In a prefab instance, each field or setting changed on
an attachment of the prefab is an override, coloured, with its own Revert and Apply; the
instance can also add attachments and remove the prefab's.

The level owns the execution schedule. The Systems panel shows engine stages and
user stages together, expands their attachments in execution order, adds user stages,
and moves them among the simulation stages. There is only a fixed update: UpdateLoop
runs once per elapsed 1/60 s (catching up at most a quarter second per frame), each
pass with `dt` = 1/60; EndOfFrameBehaviourSystem and the stages after it run once per
frame with the time the passes advanced. The level's `serialExecution` chooses between
two frames:

```text
serialExecution = false^ (default)       serialExecution = true^
SyncSimulation                           FetchInput
Parallel                                 UpdateLoop x n
    Rendering (main thread)                  UpdateBehaviourSystem
        PreRenderEventSystem                 LateUpdateBehaviourSystem
        OGRE                                 Animation
        PostRenderEventSystem                Box3DStep
    Simulation (worker thread)           EndOfFrameBehaviourSystem
        FetchInput                       SyncSimulation
        UpdateLoop x n                   PreRenderEventSystem
            UpdateBehaviourSystem        OGRE
            LateUpdateBehaviourSystem    PostRenderEventSystem
            Animation                    SyncRender
            Box3DStep
        EndOfFrameBehaviourSystem
SyncRender
```

SyncSimulation copies the simulated scene to the render snapshot; SyncRender joins the
two columns and ends the frame. Without the flag, rendering draws the previous frame's
snapshot while simulation computes this one, so script and physics changes appear one
frame later; with it, they appear in the same frame and nothing overlaps. Scripts and
the editor UI never run at the same time, and OGRE stays on the main thread.
Box3DStep advances Box3D by one fixed step with a bounded native task pool, taking the
scene's edits as input and writing poses back, so the next stage already sees them.
Animation advances the Animator components and samples their skeletal poses.

A **Render Event** component (`components.renderEvent`, same list shape as `behaviour`)
holds scripts that run on the render thread: PreRenderEventSystem calls their
`prerender(node, dt)` before OGRE draws and PostRenderEventSystem their
`postrender(node, dt)` after it, with the frame's elapsed time as `dt`. A script
publishes `RenderEvent = def^{ ... }` instead of `Behaviour`; exported fields, prefab
overrides and the Inspector work as for Behaviour, without priority overrides. These
scripts run on a separate L^ machine against the render snapshot (with frame delay,
the previous frame's scene), so they can read nodes and components but not change
them: setters and `spawnBox` panic. They do not run while paused, and their state is
not shared with Behaviour instances.

A Behaviour script attached directly to a system receives the system in its
optional callbacks. It can iterate
`throne.query("Transform", "BoxPrimitive")`, which returns nodes having every named
component. User-defined native components and systems are planned for C/C++;
scripts cannot define Component or System types. Scripts use Behaviour.

The `throne` module provides:

- `throne.Vec3`: a value type with `x`/`y`/`z`, `+`, `-`, `*` and `/` by a number,
  `dot`, `cross`, `length` and `normalized`; made with `throne.vec3(x, y, z)`. The
  axes match OGRE, Box3D and Steam Audio: right-handed, +Y up, -Z forward. Like
  any L^ host value it cannot be stored in a table; keep one with `throne.Vec3.Box^`.
- `throne.SceneNode`: `id()`, `name()`, `valid()`, and its components:
  `transform()` (a `throne.Transform`, or nil^ when the node has none) and `box()`
  (a `throne.BoxPrimitive`, or nil^). Using a handle whose node is gone panics;
  check `valid()` first.
- `node.spawnBox(size)` creates a dynamic box child at the receiver's local origin and
  returns its SceneNode handle. Size is a positive finite Vec3 applied as local
  scale; the child inherits the parent's placement. Its Transform, BoxPrimitive,
  BoxCollider and RigidBody are available immediately. Runtime nodes are appended
  without invalidating existing handles; their bodies and meshes join the next
  physics/render synchronization. They disappear on Stop and are not saved.
- `throne.Transform`: `position()`/`rotation()`/`scale()` and the matching
  `setPosition(v)` etc., relative to the parent; rotation is in degrees, applied
  about X, then Y, then Z, and scale is kept at 0.001 or more.
- `throne.BoxPrimitive`: `colour()` and `setColour(v)`, kept in [0, 1].
- A component handle whose node or component is gone panics.
- `throne.System`: `name()`; passed to direct system attachments and Systems.
- `throne.system(name)`: a script system handle (a user stage or a
  BehaviourSystem), or nil. Native engine stages cannot be coroutine wait targets.
- `throne.currentSystem()`: the current callback/resume context, or nil outside
  system execution.
- `node.setBehaviourPriority(index, numberOrNil)` and
  `setBehaviourPhasePriority(index, system, numberOrNil)`: change common/phase
  overrides. Nil restores inheritance; indexes are zero-based.
- `node.setBehaviourEnabled(index, enabled)`, `addBehaviour(script)` (returns its
  index), and `removeBehaviour(index)`: change the next frame's Behaviour registry.
- `node.component(type)`: a handle of the requested component type, or nil when
  absent. For example, `node.component(throne.Transform)` returns
  `throne.Transform|nil^`; after a nil check its native methods are available.
  The host signature is `f^self^, type^ -> ARG0.T0|nil^;` (`self^` is not counted
  in argument numbering). Non-component types are rejected at runtime.
- `throne.query(names...)`: a snapshot list in scene-instance/hierarchy order;
  no names matches all nodes, an unknown name matches none. Built-in names are
  `Transform`, `BoxPrimitive` and the other built-in render/physics components.
- `node.animator()`: the node's `throne.Animator`, or nil. `clip()`/`setClip(name)` (which
  plays from the start), `time()`/`setTime(t)`, `speed()`/`setSpeed(s)`, `playing()`,
  `play()` and `stop()`; `jointPosition(name)` is where a joint of the model's skeleton is,
  relative to the node, as the Animation stage last posed it (nil when there is none).
- `throne.find(name)` (the first node with that name, scene by scene),
  `throne.log(text)` and `throne.time()` (seconds since Play).

`print(...)` (also `L^.print`) writes its arguments, separated by spaces, as one
line of the log, as the L^ command line does.

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

In [`samples/basic`](../samples/basic), [`spin.lh`](../samples/basic/assets/scripts/spin.lh)
is attached to SpinningCube, Cube 2 and the rotor prefab's Hub. SpinningCube also
has [`pop.lh`](../samples/basic/assets/scripts/pop.lh), hopping upward every 1.5 seconds
while continuing to spin. The hop lasts 0.4 seconds and reaches 0.5 units above
its starting position; `interval`, `duration` and `height` are editable in the Inspector.
The fast variant overrides its Hub attachment's speed with 360 degrees per second, which the
scene's instance raises to 1360 (enough to show motion blur in Play).
All use BehaviourSystems; no separate Spin Component or Spin System is needed.
The design is recorded in [TDDja/07-component.md](../TDDja/07-component.md).
