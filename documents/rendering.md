# Rendering

## Editor and project settings

The editor defaults to **VSync on**. The statistics overlay's **VSync** checkbox
controls the editor window, including Play, and remains available when statistics
are minimized. It is saved per user in `editor.lton` beside the project list.
The launcher and an inactive/hidden Viewport tab always use VSync; they also skip
the scene workspace, so unseen scenes do not keep issuing draw calls. Returning
to the Viewport restores the checkbox's preference. Synchronization uses Vulkan
FIFO presentation; turning it off allows immediate presentation when supported.

Select **OGRE** in the Systems panel to inspect rendering settings. **Game
rendering (project) → VSync** is a separate project-wide setting, saved in
`project.throne.lton` as `rendering = { vsync = true^ }`. It supports Save and Undo
and never overrides editor Play's checkbox. Standalone game execution is not yet
implemented; this setting records its presentation preference. A level's rendering
(Forward Clustered and its depth range, Show Clusters) is in the level's Inspector,
and the environment and global illumination are in each scene's (below).

A **PBS Material** component chooses how its surface uses alpha (the diffuse map's alpha):
**Opaque**, **Alpha Test** (cut out below a threshold) or **Alpha Hash** (each pixel kept
with the probability of the alpha times **Opacity**). Hashed surfaces still write depth,
need no sorting and cast matching shadows; the hash changes every frame while temporal
anti-aliasing is on, so TAA averages it into soft edges. Large see-through areas keep some
grain, so hashing suits cut-outs better than glass.
**Translucency** (a colour; black, the default, turns it off) adds the light passing through a
thin surface from behind it, such as leaves, paper, cloth or wax: each directional, point and
spot light adds its colour times **Scale** times how directly the viewer looks into the light,
bent by the surface normal (**Distortion**) and focused by **Power**. Shadows still hide it, so
a thick object's own shadow removes it; thin surfaces lie within the shadow bias. Area lights
add none. **Subsurface Width (m)** (0, the default, turns it off) lets light spread under the
surface (skin, wax, marble): after the opaque surfaces are lit, the viewport blurs the lighting
of those that have it across and then down the screen by a skin-like profile (red spreads
furthest), over the given distance at the surface, without crossing silhouettes or depth
gaps; temporal anti-aliasing averages its sampling noise. Specular highlights are blurred with
the rest. **View → Subsurface Scattering** (on by default) turns the blur off. A Model's
material changes have both settings for named opaque materials.
The other engine stages (Box3DStep, Animation, the Sync barriers) are selectable too;
their Inspectors currently show only the stage name, reserving the same place for
future settings.

## Default rendering

The viewport uses OGRE-Next **Forward Clustered** and physically based (HlmsPbs)
lighting. Its default skybox is an OGRE procedural blue sky with a sun aligned to
the directional light; **View → Skybox** toggles it. It renders at background depth
and follows the camera's view without introducing scene geometry or colliders.
Fog is disabled and the existing direct/ambient light settings are preserved.

The default directional sun casts **three-cascade PSSM/CSM shadows**, with
3×3 PCF filtering, blended/faded splits, and the first two cascades stabilized.
The 2048×3072 depth atlas packs a 2048² near cascade and two 1024² cascades; shadow
range is 100 world units. The clustered grid is 16×8×24, with 64 local lights per
cell and a 0.1–100 depth range. Point and spot Light components with **Cast
Shadows** share four further 1024² maps in the same atlas (point lights through
dual-paraboloid maps); OGRE gives them to the nearest such lights.

Primitive colours are diffuse albedo. Materials use dielectric Fresnel 0.04 and
roughness 0.65, with hemispherical ambient fill. Proper outward normals and triangle
winding provide direct lighting and shadow reception. Planes are lit on both sides.
Grid lines, selection outlines and ImGui retain their unlit appearance and do not
cast shadows. **View → Shadows** toggles shadow casting while retaining direct and
ambient lighting. These are viewport defaults; scene light authoring and persistent
quality controls are not implemented yet.

## Models and environment maps

A **Model** component draws a project-relative FBX file, read with the pinned
[ufbx](https://github.com/ufbx/ufbx) (`third_party/ufbx`) and converted to metres,
+Y up and right-handed. Each file is read once; its meshes and PBS materials are
shared by every node placing it. Materials use the metal-rough workflow: a base
colour map (alpha is opacity and enables alpha testing for DXT5 DDS maps and PNG maps with alpha, or alpha hashing
with **Alpha-Hashed Cut-Outs**: soft edges on foliage once TAA averages them), a normal map
(**DirectX normal maps** selects the green direction), an ORM map packed as glTF
does (occlusion, roughness, metalness in R, G, B) and an emissive map, all repeating
across their UVs with anisotropic filtering. FBX files carry no emissive strength;
**Emission** sets the radiance of an emissive map's full value (1 by default, which is
dim beside a strong sun). A material with an FBX transparency
colour is a clear dielectric (glass, water): it refracts the scene behind it and keeps
its reflections. A Model's **material changes** give named materials of its file a
Beer-Lambert **absorption** (per metre, RGB) and **thickness**: such a volume (wine,
beer, a tinted bottle) multiplies what is behind it by its transmittance, and glass in
front of it shows it. A material change's **Diffuse Map** (project-relative) replaces the
material's base colour map, for files that name their texture only in the material's name.
A **Hidden** material change leaves that material's surfaces out of the model, for helpers
the file draws (such as the physics proxies mmd_tools exports from Blender). For an MMD model
imported with mmd_tools, `blender -b model.blend --python tools/mmd-to-fbx.py -- out.fbx`
exports its armature and meshes with their base textures (Blender's FBX exporter otherwise
writes none of mmd_tools' textures) and without the physics proxies; enable **Alpha-Hashed
Cut-Outs** for its overlay materials. Only opaque materials cast shadows. Lights and
cameras in the file are not placed. A node with only a Model is picked and outlined
by the model's bounds.

Double-clicking a model file (`.fbx`, `.mesh`) in the Browser, double-clicking a node with a Model,
or **Open Model** in its Inspector opens the model alone as a drill-down step, in the
default lighting (opened from a node, with that node's Model settings). Its
hierarchy shows the file as read: its nodes (each mesh naming the materials it uses, and
skeleton joints marked [Joint]) and a **Materials** group. Selecting one shows its
placement in the model, mesh size, surface type and textures in the Inspector, read-only;
the viewport outlines the selected node's meshes, shows a greyed-out gizmo at it, and `F`
frames them. Clicking a mesh in the viewport selects its node (by the nearest bounds the
click meets) and reveals it in the hierarchy.

An FBX file's skinned meshes keep their skin: up to four bones a vertex, bound where the
file binds them, with a skeleton of the bones and the nodes above them, and one clip per
animation stack (baked by ufbx, named as the stack). An unskinned mesh under a bone (hair,
a helmet) moves rigidly with it. A Model can also name an OGRE `.mesh` (with
the `.skeleton` beside it), such as `@ogre/models/Stickman.mesh` from the pinned sample media;
its material scripts are not read, so it is drawn in plain grey. An **Animator** component
plays one of its skeleton's clips: **Clip**, **Time** (seconds), **Speed**, **Loop** and **Playing**. Poses are sampled on
the simulation side with [ozz-animation](https://github.com/guillaumeblanc/ozz-animation):
the Animation stage advances each playing Animator's time by its speed (wrapping, or
holding at the clip's end) and samples its pose; SyncSimulation hands the poses to the
renderer, which writes them to the model's OGRE bones, and OGRE derives the bone matrices
on its worker threads and skins the mesh on the GPU. Outside Play the poses follow the
Animators' times as edited, so dragging **Time** scrubs a clip. When a skinned model is read,
the Console lists its joint count and clips (Blender's `Armature|Action` stacks are named by
the action). [`samples/characters`](../samples/characters) places six animated characters
(CC0, downloaded separately; see its [README](../samples/characters/README.md)).

A scene keeps its environment and global illumination (`lighting = { ... }` in the scene
file): the default sun, the procedural sky, the ambient light, an HDR environment, filmic tone
mapping and exposure, and its bake. They are edited in the scene's Inspector: select the
scene's row inside it, or a scene instance at the Level step (**Lighting → Scene Lighting...**
selects the one shown). A level is shown in the environment and GI of its first scene; inside
a scene, in that scene's.

**Environment (HDR)** names a project-relative equirectangular Radiance
`.hdr` file for the scene, and **Environment Scale** scales the light it gives.
The editor converts it to a 512² cubemap, drawn as the sky and reflected by every
PBS material without its own reflection map, and to spherical harmonics for diffuse
ambient light. OGRE's IBL compute pass prefilters the cubemap's mipmaps for GGX
roughness. Radiance above 16 (a sun in the image) is clamped; use a directional
Light component for the sun, which the sky's sun then follows.

Global illumination is set in the scene's Inspector. A bake has parallax-corrected reflection
probes on a grid over the probes' volume for reflections, and **Adaptive Irradiance Field** for
diffuse light: an octree of irradiance probes that is finest (**Finest Spacing**, along X, Y
and Z) in cells holding surfaces and coarser, by halves, away from them. The probes lie on a
grid through **Probe Offset** (with a Y offset of 0 and a Y spacing of 2 m, layers sit on the
ground and 2 m above it). Neighbouring cells share the probes at their corners; a probe lying
on the edge or face of a coarser cell is interpolated from it instead of rendered, so levels
meet without seams, except for the corners of cells holding a surface, which are always
rendered. By default
(**Probe Volume: Global**) the probes cover the bounds of the scene's nodes; **Local** uses the
box **Box Lower**–**Box Upper**, in the scene's coordinates (editable only then; **Fit to
Scene** fits it to the scene but its floor). Surfaces of objects smaller than **Min Object Volume** do not refine the cells, and
**Fill Empty Spaces** also covers space far from any surface. **Fast Bake Shadows** (on by
default) bakes the diffuse probes with one 2048² map for the sun instead of the viewport's
three cascades (local lights keep their shadows): about a fifth faster, with hardly any
visible difference. The Inspector shows the cells,
probes and cubemap renders a bake needs.

**Bake** (or **Lighting → Bake Global Illumination**) bakes the scene shown, where its
instance is placed, rendering the probes from everything drawn (other scenes too), once per
**Bounces** (each pass lit by the previous one), a few probes per frame under a progress
window (it waits for files being read). It saves the result, in the scene's coordinates, in
`baked/gi/<scene ID>/`, so it follows the scene when it moves, with the time it took (shown in the Inspector beside
the bake's name); opening a level reads its first scene's bake back without rendering it
again, and the bake moves with its scene instance. For now only the first scene's GI is shown
and baked. `ogrethrone --bake-gi <folder> [file] [level]` bakes from the command line, then
captures like `--capture`. Bakes kept beside scenes (`<scene>.gi/`) are no longer read; move
them to `baked/gi/<scene ID>/` or bake again.
While a bake is in use it replaces the ambient light, and the environment map is drawn only
as the sky. The bake is used while Global Illumination is enabled and its files exist, with
the settings it was made with; after the settings change, the Inspector warns until it is
baked again. Beyond the probes the bake fades out over a finest cell, and the environment's
spherical harmonics light the scene (and stand in for reflections) there. **View → GI
Probes** draws the probes' volume (white), the baked reflection probes' areas (blue) and the Adaptive Irradiance Field's probes as crosses, coloured by the finest level around them from
yellow to red, interpolated ones in grey; **Show Surface Points** adds the surface points
that refine the cells.

Long work runs while the editor keeps drawing: opening a level reads its models, HDR
environment and GI bake on worker threads, then shows the models a couple of
materials per frame (each compiles its shaders when first drawn). A **Working**
window lists what is running, with a progress bar when the share done is known and
a spinner otherwise.

A Light component's **Temperature (K)** tints its colour like a black body (6500 K
is white, lower is warmer; 0 leaves the colour as it is). **Environment Scale**
multiplies the environment's radiance, so the sky, reflections and ambient light
follow it; **Environment Temperature (K)** tints it the same way as a light.

Reflection probes see the scene from where they stand, so under an awning or indoors
they would reflect light that does not reach the surface. While a bake is in use, a
probe's reflection is dimmed by the ratio of the Adaptive Irradiance Field's light at the
surface to the diffuse light the probe itself saw (it is never brightened).

The viewport first draws lit opaque geometry (render queue 10) in a depth prepass, which
also writes a G-buffer of view-space normals, sun shadow/roughness and motion vectors
(each pixel's change in UV since the previous frame, from the camera's and each object's
motion, skinned meshes' from their previous bone matrices; the sky is left at zero) for screen-space effects. It then renders the scene in half float: queue 10 again, reusing the prepass
(its depth read-only, normal maps and sun shadows not sampled again), then the sky (11),
unlit lines and emitters (12), particles (15) and absorbing volumes (99); then, after
copying that image, queue 200 and above, where refractive materials sample the first image
and its depth. It then exposes the result.
**Filmic Tone Mapping** maps it through the ACES filmic curve instead
of clamping and **Exposure (EV)** scales it in stops; both are the scene's. Tone mapping writes
sRGB-encoded colours; the later targets, the viewport texture and the window are
not sRGB, so ImGui shows the viewport as rendered.

**TAA** (the view's or camera's **Anti-Aliasing**, by default) jitters the perspective camera by
sub-pixel offsets of the Halton (2, 3) sequence over eight frames and blends each frame
into the history of earlier ones, reprojected by the motion vectors (the sky by the
camera's motion); the history is clipped to the colours around each pixel so that moving
objects leave no trails. It makes alpha-hashed transparency usable and steadies thin
geometry; orthographic views get none. **View → Motion Blur (Play)** (on by default)
smears each pixel along its motion over a 180° shutter while the game runs (up to 32
pixels): a moving object blurs over the background, while a static foreground stays
sharp over a moving background. Navigating the editor view never blurs.
**View → Ambient Occlusion** (on by default) darkens the indirect light (ambient, global
illumination and reflections, not direct light) where nearby surfaces, within half a
metre, cover the hemisphere around a surface: screen-space ambient occlusion from the
depth prepass, smoothed across a depth-aware 5×5 filter. **View → Screen-Space
Reflections** (on by default) traces each glossy surface's reflection through the depth
buffer (up to 20 m) and shows what it meets as the previous frame drew it, blended over the
reflection probes by confidence: it fades out on rough surfaces, near the screen's edges
and for reflections towards the camera, which the screen cannot show. **Depth of field**
follows the view's focus distance and f-stop (the level's Inspector; `--capture` takes
the Camera's): a thin lens on a 35 mm full-frame sensor of the view's field of
view, gathering each pixel's neighbours whose circle of confusion reaches it (at most 16
pixels across) without letting the background bleed over a sharp foreground. Transparent
surfaces write no depth, so they take the focus of what lies behind them. The noise of
these screen-space passes changes each frame so that temporal anti-aliasing averages it
away. The viewport then
applies subtle HDR bloom before
exposure/tone mapping, then, when the view's anti-aliasing is SMAA, OGRE's SMAA 1x High
preset to smooth edges. Editor UI is
composed afterwards. `--capture` waits 16 frames for the history to settle.
Bloom uses a soft brightness threshold of 1.5 (knee 0.5), a quarter-resolution
Gaussian blur, and strength 0.08. These defaults are in
`media/postprocess/Bloom.material` and `Tonemap.material`; SMAA reuses the shaders
and lookup textures bundled with OGRE rather than copying them.

[`samples/bistro`](../samples/bistro) reproduces the Amazon Lumberyard Bistro (exterior,
interior and the wine-glass interior) from NVIDIA ORCA (CC BY 4.0), with a level for
each of ORCA's five preview views, each seen through a Camera node. The asset is downloaded separately; see the
sample's [README](../samples/bistro/README.md).

`ogrethrone --capture <folder> [file] [level]` opens a project and the given
project-relative level (else its main level), waits for its textures, looks through the
level's first Camera node (with nothing selected), saves the viewport as a PPM image of
the colours shown (default `out/capture.ppm`) and exits. OGRE
assertions are written to standard error instead of a dialog.
