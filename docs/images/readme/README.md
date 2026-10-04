# README gallery

These are screenshots captured directly by VulkanSceneRenderer on 2026-10-04,
using the Visual Studio Release build and source matching commit
`65b58c1514281105a3d9a6733712ff4d33ae9ea0`. Captures used an NVIDIA GeForce
RTX 2080 SUPER, Vulkan 1.4.325, deferred raster rendering, TAA, and 1× MSAA.
All images are 1600 × 900. Native PNG captures were converted to JPEG at quality
93 with no cropping, retouching, or generated image content.

## Models and credits

| Images | Source and attribution |
| --- | --- |
| `sponza-hallway.jpg` | [Sponza](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza), Crytek. See the model's [license notice](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/Sponza/LICENSE.md). |
| `flight-helmet-pbr.jpg`, `debug-render-targets.jpg` | [Flight Helmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/FlightHelmet), Gary Hsu, CC0-1.0. |
| `toy-car-pbr.jpg` | [Toy Car](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar), Guido Odendahl; extensions and scene composition by Eric Chadwick, CC0-1.0. |
| `usd-teapot.jpg`, `usd-suzanne.jpg` | `teapot-pbr.usdc` and `suzanne-pbr.usda` from the [TinyUSDZ model fixtures](https://github.com/lighttransport/tinyusdz/tree/v0.9.3/models). Suzanne is Blender's monkey mesh. See the [TinyUSDZ license](https://github.com/lighttransport/tinyusdz/blob/v0.9.3/LICENSE). |
| `bim-tekla-house.jpg` | [Tekla House](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House), from buildingSMART's IFC 5 development examples. |
| `bim-acca-building.jpg` | [ACCA Building](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/ACCA%20Building), from buildingSMART's IFC 5 development examples. |
| `geometric-shadows.jpg` | This project's MIT-licensed temporal geometry fixture, with a point light added. The self-contained [gallery scene](scenes/shadow-blockers.gltf) is included. |
| `lamp-shadows.jpg` | [Lights Punctual Lamp](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/LightsPunctualLamp), DGG, [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/). Gallery changes add a floor and key light and reduce the authored fill lights; see the preparation script below. |

The environment is Poly Haven's
[Citrus Orchard Road (Pure Sky)](https://polyhaven.com/a/citrus_orchard_road_puresky),
CC0. See [third-party notices](../../third-party-notices.md).
The IFCX buildings are preliminary educational and testing examples; consult
the [buildingSMART repository's usage notes](https://github.com/buildingSMART/IFC5-development/blob/1a63082ada967c683cfacee2005f8f749c8e1b79/README.md).
External model binaries and textures are not redistributed in this directory.

## Capture settings

[capture-settings.json](capture-settings.json) records each input model,
camera, exposure, lighting override, and capture frame. Placeholder roots
identify downloaded sample collections: `<glTF-Sample-Assets>`,
`<tinyusdz>`, and `<IFC5-development>`. The glTF captures use the `glTF`
variants with external textures. USD and IFCX captures use their native files.

Run from the executable's directory so the packaged shaders and HDR environment
resolve. Replace the paths below with absolute paths on your machine:

```powershell
.\VulkanSceneRenderer.exe --model "C:\Models\scene.gltf" `
  --render-technique deferred-raster --taa --msaa 1 --display-mode lit `
  --width 1600 --height 900 --warmup-frames 8 --capture-frame 48 `
  --fixed-dt 0.016666667 --no-ui --no-bloom `
  --screenshot "C:\Captures\scene.png"
```

Append the scene's overrides from the JSON. The diagnostic image uses
`--display-mode overview`. For glTF captures, the longer capture frame allows
time to clear the initial selection with Escape and click an empty background
to clear the hover highlight. Keep the pointer off meshes until capture
completes. USD and IFCX captures use frame 48 and `--hidden`.
The app's screenshot command captures the framebuffer without window chrome
or the system pointer.

Sponza uses a steep directional sun angle `(0.35, -1, -0.08)` so light reaches
the floor through the courtyard opening. Directional intensity 8 and environment
intensity 0.08 make the sunlit strip and shaded arcades distinct; exposure is
0.22. Its geometry and materials are unchanged from the source asset.

The two dedicated shadow images use different scenes and point-light shadow maps.
The blocker scene contains an orange cube and thin white occluders. The lamp
scene adds a receiving floor so its cast shadow is visible; the original asset
has no floor. Cast shadows in these examples are produced at runtime.

To prepare the lamp variant, first download the complete `glTF` folder of
Lights Punctual Lamp, then run from the repository root:

```powershell
python docs/images/readme/prepare_lamp_scene.py "C:\Models\LightsPunctualLamp\LightsPunctualLamp.gltf"
```

The script writes `LightsPunctualLamp-gallery.gltf` beside the input, preserving
relative texture and buffer references. Its added floor comes from this
repository's temporal fixture. It adds a point light at `(1.8, 3.8, 2)` with
intensity 120 and range 12, and scales the existing light intensities by 0.02.
Use the `lamp-shadows` camera and lighting overrides in the capture settings.
