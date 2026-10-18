# Box3D playground

Open this folder as an Ogre Throne project and press **F5** to play.
The blue sphere bounces, the orange capsule tumbles, and the yellow box falls onto
the purple platform. The green ground is a finite PlaneCollider.
The **Box Spawn Point** at (-2, 8, 0) runs `scripts/spawn-boxes.lh`, creating a
0.6-unit dynamic box every 0.1 seconds for as long as Play runs. The boxes start
with downward velocity so each clears the spawn point. They are never pooled or
removed during Play; this sample deliberately accumulates physics/rendering load.
Select the spawn point before Play to edit its exported `interval` field.
Press F5 again to stop and restore all starting positions.
Stopping also removes every spawned box.
Use the menu-bar **Pause / Resume** button or the keyboard **Pause** key to freeze
and resume the same session. Spawning and physics stop while camera navigation
remains available. F5 stops and restores the scene even while paused.

**View → Statistics** toggles the viewport overlay (shown by default): averaged
editor FPS/frame time, rendered scene node count, and active physics body count.
Use **− / +** to minimize/expand it. Expanded rows split physics, rendering,
editor/scripts and synchronization time. Physics and rendering run concurrently.
`ogrethrone --profile-physics` records an uncapped, fixed-delta run through
300–329 bodies to `out/physics-profile.csv` without modifying this project.

Select any body to inspect its Primitive, Collider and RigidBody. Collider-only
objects are static. Use **Edit → Add Primitive** and **Inspector → Add Component**
to build additional bodies. Mass is derived from collider density; restitution
controls bouncing. PlaneCollider supports Static/Kinematic motion only.

The default renderer is Forward Clustered with PBS materials and a directional sun.
The procedural blue sky follows the camera; **View → Skybox** toggles it.
The ground and platform receive dynamic three-cascade PSSM shadows filtered with
3×3 PCF. Use **View → Shadows** to compare the same lighting with shadow casting
enabled or disabled. Moving bodies and their shadows use the same rendered pose.
