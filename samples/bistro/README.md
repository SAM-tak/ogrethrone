# Amazon Lumberyard Bistro

The Amazon Lumberyard Bistro scenes, as distributed by the NVIDIA Open Research
Content Archive (ORCA), drawn through Model components.

The asset is not part of this repository. Download **Bistro** (v5.2) from
<https://developer.nvidia.com/orca/amazon-lumberyard-bistro> and extract it so that
this folder contains `Bistro_v5_2/` with `BistroExterior.fbx`, `BistroInterior.fbx`,
`BistroInterior_Wine.fbx`, the `Textures/` folder and `san_giuseppe_bridge_4k.hdr`. The folder is ignored by Git.

Open this folder from the project launcher, or run from the repository root:

```powershell
out/build/windows-clang-ninja/bin/Release/ogrethrone.exe samples/bistro
```

The project opens `levels/exterior.level.lton`; click a file in **Browser > Levels**
to switch. Each level uses filmic tone mapping. Each level places a scene of its own,
`scenes/<level>-view.scene.lton`, holding one Camera node, **View**: select it and press
**Align View to Camera** to see the level as framed (`--capture` does so itself).
`exterior` and `interior` are framed at their file's camera. Five levels are seen from
ORCA's preview renders, framed by eye (the renders' cameras are not published):

- `plaza`: the corner of Le Petit Coin with the scooter (exterior preview 1).
- `terrace`: the table under the awning with the bottle and ashtray (exterior preview 2).
- `interior-wine`: the room and the bar (interior preview 1).
- `wine-table`: a laid table with wine glasses (interior preview 2).
- `wine-counter`: the end of the counter with the cooler and cookie jars (interior preview 3).

`exterior.level.lton` places `scenes/exterior.scene.lton`:

- **Bistro Exterior**: a Model component reading `BistroExterior.fbx` (1296 meshes,
  132 metal-rough materials, 2.8 M triangles). The file's DirectX normal maps and
  ORM "Specular" maps (occlusion, roughness, metalness) are used as authored.
- **Sun**: a directional light, power 90 at 4300 K. Its direction (from the
  south-west, 45° high) is chosen so the plaza and terrace shadows fall as in ORCA's
  renders; the FBX's own sun is steeper.

The level's environment is the scene's own `san_giuseppe_bridge_4k.hdr`: it is drawn
as the sky and lights every surface (spherical harmonics for diffuse light, a
GGX-prefiltered cubemap for reflections).

`interior.level.lton` places `BistroInterior.fbx` with point lights at the file's
four ceiling lights (which cast shadows) and at the wall lights and lanterns. `interior-wine.level.lton`
places `BistroInterior_Wine.fbx`, the larger interior with filled wine glasses; the
file has no lights, so point lights sit at its ceiling lights, wall lights and
lanterns. Clear glass refracts; red and white wine, beer and the wine bottles absorb
light with the coefficients of ORCA's `BistroInterior_Wine.pyscene`. The path tracer that ORCA's renders use
lights these rooms from their emissive surfaces; point lights stand in for that.
Their exposure is -2.9 EV.

Each scene bakes its global illumination (two bounces); the exterior levels share
`scenes/exterior.scene.lton`, and the wine-glass levels `scenes/interior-wine.scene.lton`, with
their environment, exposure and bake. Bake once with **Lighting → Bake Global Illumination**,
or from the repository root:

```powershell
out/build/windows-clang-ninja/bin/Release/ogrethrone.exe --bake-gi samples/bistro out/bistro.ppm levels/plaza.level.lton
```

The results are written to `scenes/*.gi/` (about 50 MB each) and ignored by Git.
The exterior levels scale the environment by 28 and tint it to 5000 K (exposure
-2.5 EV, the terrace -2.7 EV), and the exterior's lamps and signs glow with emission
30, so the balance of sun, sky, shade and lamps follows ORCA's preview renders. Bakes
made before October 20, 2026 do not match these settings; bake them again.

Opening a level reads its files in the background and then shows the models a few
materials at a time; the **Working** window shows the progress. In a Debug build this
takes about 30 seconds the first time.

Amazon Lumberyard Bistro, Open Research Content Archive (ORCA), by Amazon
Lumberyard (2017), is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); see `Bistro_v5_2/LICENSE.txt`.
Its environment map comes from <https://hdrihaven.com> (Poly Haven).
