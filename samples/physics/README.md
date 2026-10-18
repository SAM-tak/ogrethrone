# Box3D playground

Open this folder as an Ogre Throne project and press **F5** to play.
The blue sphere bounces, the orange capsule tumbles, and the yellow box falls onto
the purple platform. The green ground is a finite PlaneCollider.
Press F5 again to stop and restore all starting positions.

Select any body to inspect its Primitive, Collider and RigidBody. Collider-only
objects are static. Use **Edit → Add Primitive** and **Inspector → Add Component**
to build additional bodies. Mass is derived from collider density; restitution
controls bouncing. PlaneCollider supports Static/Kinematic motion only.
