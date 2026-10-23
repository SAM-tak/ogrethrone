# Physics

Open `samples/physics` and press **F5**. The sphere bounces, the capsule tumbles, and
the box drops onto a static platform. Press F5 again to stop and restore the initial
scene. The simulation runs only in Play mode.

```powershell
./out/build/windows-clang-ninja/bin/Debug/ogrethrone.exe samples/physics
```

**Edit → Add Primitive** creates a Box, Sphere, Capsule or Plane with its matching
Collider. Add **RigidBody** in the Inspector to make it dynamic. Render geometry and
collision shapes are independent. A node has one render primitive, but can combine
several colliders on the same body. Child colliders are independent of a parent's body.

RigidBody exposes Static, Kinematic and Dynamic motion, linear/angular velocity,
damping, gravity scale and Bullet CCD. Collider-only nodes are static. Mass is derived
from collider density and shape volume, matching Box3D. Colliders also expose their
centre, friction, restitution and sensor flag. Sensor events are not exposed to scripts.

Sphere dimensions use a radius; Capsule dimensions use a radius and total height,
including both hemispheres, along local Y. Plane dimensions use width/depth on local XZ.
A PlaneCollider is a finite upward-facing triangle mesh for Static/Kinematic bodies;
it is not an infinite plane, and cannot be dynamic. Sphere colliders use the largest
scale axis; capsule colliders use max(X,Z) for radius and Y for height. Nonuniform scale
can therefore make visual spheres/capsules differ from their collision shapes.

The Inspector supports persistence, undo and prefab property overrides. New typed
getters are `sphere()`, `capsule()`, `plane()`, `boxCollider()`, `sphereCollider()`,
`capsuleCollider()`, `planeCollider()` and `rigidBody()`, all returning nil when absent.
All these types work with `throne.query`. RigidBody provides `type()`,
`linearVelocity()`/`setLinearVelocity(v)` and `angularVelocity()`/`setAngularVelocity(v)`.
Angular velocity is in degrees per second. Primitive colours are readable/writable;
shape dimensions and collider centres are readable. Body transform edits become
teleports at the next physics preparation step.

For a reproducible graphical check, `--smoke-physics [project] [capture-directory]`
captures the initial scene, then plays a temporary project copy until frame 240 at
a fixed 1/60 s, checking that Play remains active and Forward Clustered is selected. Initial shadow ON/OFF frames must differ;
after Stop, unshadowed rendering must exactly match the initial unshadowed frame,
and enabling shadows must affect the restored frame. This keeps PCF edge sampling
variation separate from exact geometry/lighting restoration. Viewport frames 2, 3,
60, 240, 244 and 245 are downloaded as PPM images. Defaults are
`samples/physics` and `out/physics-captures`; project files and the project list remain
untouched. `runtime_smoke` covers collisions, deterministic serial/parallel replay,
scene coordinate conversion, typed script access and component/prefab persistence.
