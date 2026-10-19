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
to switch. Each level uses filmic tone mapping and the files' 34° field of view.
`exterior`, `interior` and `interior-wine` open at their file's camera;
`plaza.level.lton` is the exterior seen from ORCA's first exterior preview render
(the corner of Le Petit Coin with the scooter), at +1.3 EV.

`exterior.level.lton` places `scenes/exterior.scene.lton`:

- **Bistro Exterior**: a Model component reading `BistroExterior.fbx` (1296 meshes,
  132 metal-rough materials, 2.8 M triangles). The file's DirectX normal maps and
  ORM "Specular" maps (occlusion, roughness, metalness) are used as authored.
- **Sun**: the file's directional light (its direction and warm colour).

The level's environment is the scene's own `san_giuseppe_bridge_4k.hdr`: it is drawn
as the sky and lights every surface (spherical harmonics for diffuse light, a
GGX-prefiltered cubemap for reflections).

`interior.level.lton` places `BistroInterior.fbx` with point lights at the file's
four ceiling lights (which cast shadows) and at the wall lights and lanterns. `interior-wine.level.lton`
places `BistroInterior_Wine.fbx`, the larger interior with filled wine glasses; the
file has no lights, so point lights sit at its ceiling lights, wall lights and
lanterns. Clear glass refracts; wine and bottles are alpha blended. The path tracer that ORCA's renders use
lights these rooms from their emissive surfaces; point lights stand in for that.
Their exposure is -1.5 EV.

Every level bakes global illumination (two bounces) over the area around its view;
bake it once with **Lighting → Bake Global Illumination**, or from the repository root:

```powershell
out/build/windows-clang-ninja/bin/Release/ogrethrone.exe --bake-gi samples/bistro out/bistro.ppm levels/plaza.level.lton
```

The results are written to `levels/*.gi/` (about 50 MB each) and ignored by Git.
The sun uses power 150 at 6500 K, and the exterior levels scale the environment by
20, so the lighting ratio between sun, sky and shade follows ORCA's preview renders.

Reading a file takes several seconds on first open.

Amazon Lumberyard Bistro, Open Research Content Archive (ORCA), by Amazon
Lumberyard (2017), is licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/); see `Bistro_v5_2/LICENSE.txt`.
Its environment map comes from <https://hdrihaven.com> (Poly Haven).
