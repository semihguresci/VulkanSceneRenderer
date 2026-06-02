# VulkanSceneRenderer

VulkanSceneRenderer is a C++23 Vulkan renderer for real-time scene rendering,
glTF, BIM, and USD content, physically based materials, shadows,
GPU culling, and debug visualization.

The CMake project and build targets use `VulkanSceneRenderer`. The public
include root remains `include/Container`, and the source namespace remains
`container::`, to avoid a broad source-level API rename.

## Gallery

VulkanSceneRenderer supports high-detail scene rendering, physically based
materials, live renderer telemetry, and debug-oriented render target
visualization.

![Sponza hallway rendered with textured banners, stone surfaces, and scene lighting](docs/images/readme/sponza-hallway.jpg)

| Debug visualization | Material and scene rendering |
| --- | --- |
| ![Render target debug views showing the shaded scene, color attachments, normals, depth, wireframe, and telemetry panes](docs/images/readme/debug-render-targets.png) | ![glTF camera model shown across debug render targets including shaded, normal, depth, wireframe, and blurred views](docs/images/readme/gltf-camera-debug-views.png) |
| ![Aviator mask model rendered with textured PBR materials against an environment map](docs/images/readme/aviator-mask-pbr.png) | ![PBR material grid comparing specular factors, specular textures, color factors, color textures, and high color factor values](docs/images/readme/pbr-material-grid.png) |
| ![Side-by-side comparison of metallic roughness and KHR materials pbrSpecularGlossiness rendering on bottle models](docs/images/readme/pbr-extension-comparison.png) | ![Textured stylized car model rendered on a red cloth surface with scene controls visible](docs/images/readme/textured-car-scene.png) |

## Quick Start

Windows release:

```powershell
cmake --preset windows-release
cmake --build out/build/windows-release --target VulkanSceneRenderer --config Release
```

Run tests:

```powershell
ctest --test-dir out/build/windows-release --output-on-failure
```

Run with a BIM sidecar model:

```powershell
$bim = "models\buildingSMART-IFC5-development\examples\Hello Wall\hello-wall.ifcx"
.\out\build\windows-release\VulkanSceneRenderer.exe --bim-model $bim
```

The sidecar path also accepts USD, USDA, USDC, and USDZ mesh files through the
TinyUSDZ-backed importer:

```powershell
$usd = "models\my_ascii_mesh.usda"
.\out\build\windows-release\VulkanSceneRenderer.exe --bim-model $usd
```

Download the OpenUSD sample models with CMake:

```powershell
cmake --build out/build/windows-release --target download_usd_models --config Release
```

## Documentation

- [Project overview](docs/project-overview.md) - features, repository layout,
  and asset organization.
- [Architecture](docs/architecture.md) - runtime ownership, frame flow, and
  subsystem boundaries.
- [Build and test](docs/build-and-test.md) - requirements, presets, build
  commands, helper scripts, and known test status.
- [Development guide](docs/development-guide.md) - renderer conventions,
  shader/C++ layout contracts, and commenting guidance.
- [MSAA](docs/msaa.md) - deferred raster multisampling configuration,
  render-pass resolves, and render graph boundaries.
- [Renderer telemetry](docs/renderer-telemetry.md) - live frame timing,
  GPU query backends, render graph metrics, and validation commands.
- [Coordinate conventions](docs/coordinate-conventions.md) - source of truth
  for coordinate systems, reverse-Z depth, viewports, culling, and matrix rules.
- [Lighting system plan](docs/lighting-system-improvement-plan.md) - lighting,
  shadows, tiled culling, GTAO, GPU-driven rendering, and bloom rationale.
- [Refactoring plan](docs/refactoring-plan.md) - ownership boundaries,
  dependency cleanup, and render graph direction.
