# README gallery

Six native 1920 × 1080 captures from VulkanSceneRenderer, dated 2026-11-07.
Two views each of Sponza, Tekla House and A Beautiful Game. Sponza and Tekla use
progressive path tracing at 512 samples per pixel, eight bounces and OptiX
display denoising. A Beautiful Game uses forward PBR with TAA, two rectangular
studio lights and its original 2048 × 2048 textures. Its solid glass produces
unsupported paths in the current reference tracer; the chess views use the
forward renderer.

These images describe the current source branch. The published v0.2.0 TAA
preview is an older prerelease, and local validation packages are not published
releases.

## Models and credits

The two glTF assets come from Khronos glTF Sample Assets at
[`8f492390ddc7a2ae221a1369ef560589cf8c70fb`](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb).
The captured source files are checked against that revision. Original model
geometry, materials and textures are retained. The prepared chess scene adds one
matte receiver plane, and the capture sequence adds two lights; the Tekla views add the
path tracer's analytic ground receiver. Camera, lighting and display grading
are recorded below. External model binaries, textures and HDR environments are
not redistributed in this gallery directory.

| Images | Source, attribution and license |
| --- | --- |
| `sponza-daylight.jpg`, `sponza-arcade.jpg` | [Sponza](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/Sponza), © 2016 Crytek. Improved Sponza model by Frank Meinl, based on Marko Dabrovic's original; PBR textures by Alexandre Pestana, with conversion details in the [upstream README](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/Sponza/README.md). The model and its textures use the [Cryengine Limited License Agreement](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/LICENSES/LicenseRef-CRYENGINE-Agreement.txt). The separate CC-BY-4.0 notice on its metadata does not replace that model license. |
| `a-beautiful-game.jpg`, `a-beautiful-game-detail.jpg` | [A Beautiful Game](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ABeautifulGame). Original by Moeen Sayed and Mujtaba Sayed for SideFX; original-model rights © 2020 ASWF/MaterialX Project. glTF conversion © 2022 Ed Mackey, with initial geometry conversion by Chris Rydalch and Jonathan Stone. Distributed under [CC-BY-4.0](https://creativecommons.org/licenses/by/4.0/); see the pinned [source credits](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ABeautifulGame/README.md) and [license](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/8f492390ddc7a2ae221a1369ef560589cf8c70fb/Models/ABeautifulGame/LICENSE.md). All 49 mesh instances and 1,500,224 instanced triangles are retained. |
| `bim-tekla-native-ifc.jpg`, `bim-tekla-detail.jpg` | [Tekla House](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House), from buildingSMART's IFC 5 development examples. Both views load the native `TeklaHouse.ifc` STEP source. |

The buildingSMART assets are preliminary examples for education and testing;
consult their [pinned usage notes](https://github.com/buildingSMART/IFC5-development/blob/1a63082ada967c683cfacee2005f8f749c8e1b79/README.md).
The gallery does not modify or redistribute these building models. Camera and
viewer lighting, analytic ground and display grading are renderer settings
rather than changes to their source files.

The complete supplied BIM folders were checked for companion geometry. Tekla's
IFC and IFCX files are alternative representations of the same structural
export, not separate design parts; the native IFC contains no external document
or library references. No companion building parts were omitted.
Tekla is a structural model, so the absence of furniture and architectural
finishes reflects the supplied design. The native import reports 10,042 of
10,042 represented products, with no skipped or incomplete representations.

## Environments

- [Aerodynamics Workshop](https://polyhaven.com/a/aerodynamics_workshop), by
  Oliksiy Yakovlyev, from Poly Haven, [CC0](https://polyhaven.com/license).
- [Citrus Orchard Road (Pure Sky)](https://polyhaven.com/a/citrus_orchard_road_puresky),
  photographed by Dimitrios Savva and processed by Jarod Guest, from Poly Haven,
  [CC0](https://polyhaven.com/license).

Sponza and BIM use Citrus Orchard Road; the chess views use Aerodynamics
Workshop at −35° yaw for fill and reflections, plus two rectangular softboxes
and a neutral matte receiver for the chess presentation. Per-image orientation,
intensity and lighting are recorded in [capture-settings.json](capture-settings.json).
See also the repository's
[third-party notices](../../third-party-notices.md).

## Capture and reproduction

[capture-settings.json](capture-settings.json) records each image's input,
camera, import scale, renderer, lights, AgX display grade, dimensions, frame or
sample count, GPU/driver, executable and shader/runtime fingerprints. The
captures use a local build with uncommitted source changes; a Git base commit
alone is not an exact build identifier. Per-image hashes identify the actual
executable, SPIR-V payload, inputs and output.
Native PNGs and their capture/telemetry sidecars are retained in ignored local
build storage; quality-93 JPEGs in this directory preserve the full frame.

Screenshots come from the renderer's framebuffer, without window chrome or the
system pointer. JPEG encoding changes pixel values but preserves the complete
composition: no cropping, resizing of the captured image, retouching, relighting
or generated image content. Source assets are not bundled with these images.

The [display and image export guide](../../display-and-image-export.md) describes
viewport PNGs, raw scene-linear EXR and capture sidecars. A native capture can
be reproduced from the executable's directory with the recorded per-image
settings. For example, the Sponza daylight view uses:

```powershell
.\VulkanSceneRenderer.exe --model "C:\Models\Sponza\glTF\Sponza.gltf" `
  --import-scale 1 --texture-memory-mb 1024 --width 1920 --height 1080 `
  --render-technique path-tracing --display-mode lit --path-spp 16 `
  --path-samples 512 --path-bounces 8 --path-seed 23 --path-firefly-clamp 0 `
  --path-optix --path-denoise-interval 512 --no-taa --no-ddgi --no-bloom `
  --msaa 1 --fsr native --dlss native --ray-shadows hard --screen-reflections off `
  --camera-position 8 1.8 0 --camera-target -8 2.8 0 --camera-fov 65 `
  --environment "C:\Environments\citrus_orchard_road_puresky_4k.exr" `
  --environment-intensity 4 --environment-yaw 0 `
  --directional-direction 0.35 -1 -0.08 --directional-color 1 0.98 0.94 `
  --directional-intensity 3 --tone-map agx --exposure 2 --vibrance 0.02 `
  --warmup-frames 0 --capture-frame 40 --fixed-dt 0.016666667 `
  --hidden --validation --no-ui --screenshot-viewport `
  --screenshot "C:\Captures\sponza-daylight.png"
```

Use `--model` for the building inputs too, to load them without the default
primary scene. The Tekla capture raises `--ddgi-surface-memory-mb` to 1024;
this existing setting also bounds reference path-tracer surface metadata.
The setting is a limit, not a reservation. Tekla uses import scale 1; the
IFC importer already applies declared length units and the up-axis conversion.
Its receiver is at Y = 5.255 m, below the native model's minimum Y = 5.275 m,
so the foundation geometry stays visible. The restrained Tekla display grade
reduces the source's engineering colour palette; its simplified materials
remain an architectural visualization.

The exact dual-view camera and studio setup sequences are bundled beside the
grading presets and referenced by `cliArgs` in the capture settings. Replace
the documented path placeholders with your local asset locations. The path
captures use frames 40 and 80; the chess captures use frames 128 and 256 after
TAA settles. Each second view has a separate camera-history epoch.

| Scene | Native grading preset | Camera and lighting sequence |
| --- | --- | --- |
| Sponza | [Daylight grade](grades/sponza-photographic-v2.json) | [Two courtyard views](grades/sponza-capture.json) |
| Tekla House | [Structural grade](grades/tekla-structural.json) | [Full and detail views](grades/tekla-capture.json) |
| A Beautiful Game | [Studio grade](grades/chess-photographic.json) | [Softboxes and two cameras](grades/chess-prepared-studio-capture.json) |

For the chess studio, copy
[ABeautifulGame-studio-prepared.gltf](scenes/ABeautifulGame-studio-prepared.gltf)
beside the pinned original `ABeautifulGame.gltf`, binary buffer and textures.
Load that prepared file; its external resource URIs are unchanged. This
CC-BY-4.0 scene variant appends one 100 m matte receiver at world Y = −0.001 m,
below the complete board. See its [attribution and change notice](scenes/ABeautifulGame-studio-NOTICE.txt).

The chess capture sequence disables interaction on imported roots, adds one
source-less editor helper and authors two area lights. The original 49
instances, 15 materials, transforms, draw bindings and 33 original 2K images
are checked independently. The additional receiver and its material are
recorded separately; no edited scene is saved by the renderer. Keep the complete
model resource folders. No command above downloads or overwrites an input file.

## Archived captures

The earlier gallery files remain in this directory as historical captures from
2026-10-04, source commit `65b58c1514281105a3d9a6733712ff4d33ae9ea0`, on an NVIDIA
GeForce RTX 2080 SUPER with Vulkan 1.4.325. Those images used deferred raster,
TAA, 1× MSAA and 1600 × 900 output, encoded as quality-93 JPEGs. They are not
evidence for the current branch's renderer or optional backends.

The archived [2026-10-04 settings](capture-settings-2026-10-04.json) describe
only that older set. Its files and source credits are retained below:

| Archived images | Source and attribution |
| --- | --- |
| `sponza-hallway.jpg` | Sponza, Crytek; the model license is the Cryengine Limited License Agreement described above. |
| `flight-helmet-pbr.jpg`, `debug-render-targets.jpg` | [Flight Helmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/FlightHelmet), Gary Hsu, CC0-1.0. |
| `toy-car-pbr.jpg` | [Toy Car](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar), Guido Odendahl; extensions and scene composition by Eric Chadwick, CC0-1.0. |
| `usd-teapot.jpg`, `usd-suzanne.jpg` | TinyUSDZ v0.9.3 [model fixtures](https://github.com/lighttransport/tinyusdz/tree/v0.9.3/models); Suzanne is Blender's monkey mesh. See the [TinyUSDZ license](https://github.com/lighttransport/tinyusdz/blob/v0.9.3/LICENSE). |
| `bim-tekla-house.jpg`, `bim-acca-building.jpg` | The pinned buildingSMART [Tekla House](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/Tekla%20House) and [ACCA Building](https://github.com/buildingSMART/IFC5-development/tree/1a63082ada967c683cfacee2005f8f749c8e1b79/examples/ACCA%20Building) examples, using IFCX sources. |
| `geometric-shadows.jpg` | This project's MIT-licensed [shadow-blocker fixture](scenes/shadow-blockers.gltf). |
| `lamp-shadows.jpg` | [Lights Punctual Lamp](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/LightsPunctualLamp), DGG, CC-BY-4.0. The historical variant adds a floor/key light and scales authored fill lights by 0.02; [prepare_lamp_scene.py](prepare_lamp_scene.py) records those changes. |
