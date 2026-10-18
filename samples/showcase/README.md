# OGRE lighting and material showcases

Four editable Throne levels port the scene layouts and lighting from the pinned
OGRE-Next `Forward3D`, `PbsMaterials`, `AreaApproxLights` and `ParticleFX2` samples.

Open this folder from the project launcher, or run from the repository root:

```powershell
out/build/windows-clang-ninja/bin/Release/ogrethrone.exe samples/showcase
```

The project opens **Area Lights**. Click a file in **Browser > Levels** to switch:

- `area-lights.level.lton`: 64 spheres varying roughness and Fresnel, 16 animated
  objects, and warm/cool rectangular lights. The large light uses the original
  hollow-frame mask pattern. Use **Lighting > Area Lights: Approx (textured)** or
  **Area Lights: LTC** to compare their shading and rectangular highlights.
- `pbs-materials.level.lton`: the 8 × 8 roughness/Fresnel sphere palette, reflection
  cubemap, rock/marble surfaces, and two coloured spotlights.
- `forward3d.level.lton`: 128 coloured point lights over a large ground plane,
  64 pillars, 16 animated cubes, and two spotlights. Use **Lighting > Forward3D /
  Forward Clustered** and **Show Clusters** to compare light assignment. Both
  methods aim to produce the same lighting; their difference is culling cost.
- `particlefx2.level.lton`: the purple fountain, rising aureola rings, PBS smoke,
  rotating Unlit smoke and PBS sphere particles. A marble platform and marker
  reproduce the original ten-second motion. The opening view frames the larger
  particle layout; fly closer to inspect individual effects.

F5 starts/stops the rotation and particle-platform behaviours. Pause suspends or
resumes Play. These editor shortcuts replace the standalone samples' function-key controls.
Viewport navigation remains available while paused.

Particles animate in edit mode by default. **Particles > Preview in Edit Mode**
toggles this preview; **Restart** clears existing particles and starts emission
again. Play starts fresh particle pools, Pause freezes emission, positions and
lifetimes, and Stop clears the Play particles and restores the authored platform
poses. Edit preview resumes after Stop when enabled.

**Particles > Alpha Blending / Alpha Hashing** compares conventional transparency
with the sample's order-independent alpha hashing. The viewport is single-sampled,
so the standalone demo's additional MSAA alpha-to-coverage mode is not offered.
Statistics includes system counts and an approximate particle count (SIMD slots).

A **Particle System** component selects a template by name and has an Enabled flag.
An empty Particle Script uses the five built-in showcase templates; a nonempty
project-relative `.particle2` path loads definitions and associated materials and
textures from that directory. Template names must be unique across loaded files.
Editing a template/enable flag restarts the particle pools. Components round-trip
through scenes and prefab overrides; `throne.query("ParticleSystem")` finds their
nodes, and `node.particleSystem()` returns the component handle.

The built-in definitions in `media/particles/Showcase.particle2` preserve the OGRE
sample values but flatten smoke inheritance so Scaler, Rotator and ColourImage
remain separate affectors. Leading `+` signs on colour-fader rates are omitted
for the pinned charconv parser; their numeric values are unchanged. Original
textures/materials are reused from the
pinned OGRE media. The ring image uses luminance plus alpha; its RG channels are
swizzled to grayscale RGB and alpha for the STBI decoder. The standalone sample's static marble billboard is represented
by a vertical plane; the actual particle effects use OGRE ParticleFX2 billboards.

Select a node in Scene and edit its **Light** or **PBS Material** in Inspector.
Both components support save/load, undo and prefab property overrides. Light
types are Directional, Point, Spot, Area Approx and Area LTC. PBS properties
include roughness, Fresnel F0 or metallic workflow, emission and texture maps.
`throne.query("Light")` and `throne.query("PbsMaterial")` also find them in scripts.
The Lighting menu's area model override is a temporary comparison; use the Light
component's Type field to save a model change. Forward method and cluster debug
selection are saved with the level. Ambient colours and the default sun can also
be edited in Lighting. Initial view, sky, grid and environment are level data.

The ports use Throne's built-in box/sphere/plane geometry instead of the OGRE
sample meshes. Their arrangements, light parameters and palette values are
authored in ordinary `.scene.lton` files; pillars and point positions use a fixed
random seed. The sample applications' transparency/BRDF cycling and procedural
light-count controls are not part of these scene files. The marble surface uses
the original image as a tiled diffuse map. Area lights use regular Forward and
do not generate area-light shadow maps. The authored directional light can use
the editor's existing cascaded shadows.

`@ogre/` texture paths refer to `third_party/ogre-next/Samples/Media`, already
required for shader templates. They reuse the original rock/marble textures and
`SaintPetersBasilica.dds` without duplicating them in this project. Ordinary paths
are project-relative. Approx masks currently use a 256 × 256 R8 DDS with nine mip
levels, matching the renderer's shared area-mask pool. LTC lookup tables are
loaded from OGRE's Common resources.

Run automated data and Vulkan checks from the repository root:

```powershell
ctest --preset windows-release -R showcase_data_smoke
out/build/windows-clang-ninja/bin/Release/ogrethrone.exe --smoke-showcases
```

The Vulkan check opens all four levels, validates light counts, switches culling
and area-light models, and checks Play/Pause/Stop against viewport captures in
`out/showcase-captures/`. Directional shadows are disabled for pose comparisons
because cascade fitting can change edge samples. Particle checks also inspect
live CPU positions/lifetimes while paused, isolate each of the five effects to
verify visible pixels and transparent ring corners, compare blending modes, and exercise
preview toggling, Stop cleanup and level re-entry. See [NOTICE.md](NOTICE.md) for
source and asset attribution.
The particle checks also capture the actual ImGui viewport presentation with
shadows enabled and compare its scene pixels with the render target in both
blending modes. This catches accidental reuse of particle alpha when displaying
the already composited scene image.
