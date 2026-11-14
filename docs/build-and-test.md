# Build and Test

Temporal anti-aliasing is opt-in with `--taa --msaa 1` for both raster techniques.
Build `temporal_convention_tests` and `temporal_capture_tests`; run CTest with
`-R "^temporal_"`. The metric CPU test requires Python Pillow and NumPy and gives
an explicit skip if they are unavailable. GPU tests stay opt-in:

```powershell
$env:CONTAINER_RUN_TAA_REGRESSION = '1'
python tests/validation/temporal_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --suite all
```

The script enables synchronization validation and writes selected PNGs,
effective settings/timing/allocation sidecars and result JSON under
`out/taa-regression`. Use `--suite quality`, `stress`, or `performance` to narrow
the run, and `--analyze` to recompute metrics from existing captures.
Run `--suite mutations --slangc C:/Tools/slang/bin/slangc.exe` after
the quality suite to verify detection of deliberately inverted velocity, stale
history addressing and skipped invalidation. This temporarily replaces runtime
SPIR-V, restores it in `finally`, and must run without concurrent builds/captures.
See
[temporal contracts and capture controls](temporal-rendering.md) and
[measured validation](taa-validation.md).

## Requirements

- CMake 3.23 or newer.
- Ninja on `PATH`, or an explicit `CMAKE_MAKE_PROGRAM`.
- A C++23 compiler.
- Vulkan 1.4 SDK, including `glslangValidator`, `slangc`, and validation layers.
- vcpkg with manifest dependencies from `vcpkg.json`.

Optional [OpenVDB import, volume rendering and voxel/SDF tools](openvdb.md)
are enabled with `-DENABLE_OPENVDB=ON`. CMake automatically selects the vcpkg
`openvdb` feature. Run the `openvdb_*` CPU and configuration tests after building
their targets.

Set `VCPKG_ROOT` before configuring. Keep machine-local overrides in an
untracked `CMakeUserPresets.json`.

Use the latest stable dependency releases, with pinned versions, hashes and
notices updated together after the relevant build and runtime checks pass.
An installed SDK or a reused cache can still select an older tool; this policy
does not mean every dependency in the current build has already been updated.
The scoped vcpkg overlays pin the [selected library releases](library-versions.md)
without changing the registry baseline for unrelated ports. GLM remains
header-only with the project's
existing matrix, radians and depth definitions. CMake verifies the selected
headers and stages their notices from their actual package prefixes. For a
standalone upstream install, also copy its unmodified `copying.txt` to
`<prefix>/share/glm/copyright` or `LICENSE.txt` to
`<prefix>/share/vulkan-memory-allocator/copyright`, respectively.
Select a separately installed Slang compiler explicitly when configuring:

```powershell
cmake --preset windows-release "-DSLANGC_EXECUTABLE=C:/Tools/slang/bin/slangc.exe"
```

CMake logs the selected compiler and retains explicit or cached selections.
Without a selection, it searches `VULKAN_SDK` first, then normal program paths
including `PATH`. Choose a different compiler with `-DSLANGC_EXECUTABLE=...`,
or clear that cache entry with `-U SLANGC_EXECUTABLE` to repeat discovery.

For a portable Windows x64 package, optionally select LunarG's signed Vulkan
Runtime 1.4.363.0 component (also used by SDK 1.4.363.1):

```powershell
cmake --preset windows-release "-DCONTAINER_VULKAN_LOADER_ROOT=C:/Tools/VulkanRT-1.4.363.0/x64-portable"
```

That directory must contain the official `vulkan-1.dll` and
`VulkanRT-License.txt`. CMake checks both exact hashes and stages the loader and
notice beside each runtime; packaging checks them again and records the version
and hash. Nothing is downloaded or installed by this option. Without it, ordinary
development builds retain their existing OS or dependency-provided loader.

The optional portable SDK core selection is independent of that runtime loader:

```powershell
cmake --preset windows-release "-DCONTAINER_VULKAN_SDK_CORE_ROOT=C:/Tools/VulkanSDK-1.4.363.1-core"
```

This Windows x64 subset contains the official SDK's `Include/vulkan`,
`Include/vk_video` and `Lib/vulkan-1.lib`, plus the pinned header and loader
notices under `licenses/Vulkan-Headers-LICENSE.md` and
`licenses/Vulkan-Loader-LICENSE.txt`. CMake checks all 64 files, selects those
include/library paths and watches them for changes that require a new hash
check. Packaging retains both notices and records SDK 1.4.363.1/header 1.4.363.
The option does not install the SDK or change the physical device's API/features.

Slang 2026.21 emits storage-buffer nonuniform indexing in the shared bindless
material shaders as well as sampled-image nonuniform indexing. Device selection
checks both features before creating the device, and enables both explicitly.
Shader validation must therefore include a native run against the selected
device; standalone SPIR-V validation does not check enabled device features.

On Windows, use a Visual Studio Developer Command Prompt so MSVC, Windows SDK,
and Ninja are discoverable.

## Build

Windows release:

```powershell
cmake --preset windows-release
cmake --build out/build/windows-release --target VulkanSceneRenderer --config Release
```

Windows debug:

```powershell
cmake --preset windows-debug
cmake --build out/build/windows-debug --target VulkanSceneRenderer --config Debug
```

### Visual Studio

Open the repository folder in Visual Studio with the Desktop development with
C++ workload installed, and select `windows-debug` or `windows-release` from
the CMake configuration selector. Select `VulkanSceneRenderer.exe` as the
startup target. Remove an old `VulkanContainer.exe` startup entry from
`.vs/launch.vs.json` if an existing workspace still has that name.

To generate a native Visual Studio 2026 solution instead of using the Ninja
presets, use CMake 4.2 or newer in a Visual Studio Developer Command Prompt:

```powershell
cmake -S . -B out/build/visual-studio -G "Visual Studio 18 2026" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build out/build/visual-studio --config Release --target VulkanSceneRenderer
```

The executable and runtime assets are staged in
`out/build/visual-studio/Release`. Debug uses its own `Debug` directory.
Multi-configuration builds stage assets on every build, including when only
shaders change. The TinyUSDZ MSVC compatibility header is guarded for C++ so
Visual Studio can also compile the dependency's C sources.

If CMake reuses a stale compiler path, refresh the configure cache:

```powershell
cmake --fresh --preset windows-release
```

Linux presets are also provided:

```sh
cmake --preset linux-release
cmake --build out/build/linux-release --target VulkanSceneRenderer
```

The build compiles Slang shaders and copies or generates runtime assets through
the `shaders`, `copy_materials`, `copy_hdr`, and `generate_models` targets.
`ENABLE_SAMPLE_MODEL_DOWNLOAD` controls the pinned glTF Sample Models archive
download used for gallery scenes such as Sponza. The default Cornell box scene
is included locally. `ENABLE_BIM_SAMPLE_MODEL_DOWNLOAD`
controls the buildingSMART IFC5-development and community sample fetches used by BIM importer tests
and manual IFC/IFCX validation. The BIM fetch checks the repository `main` ref
at build time and refreshes the local archive when that ref changes.
Community IFC samples come from
`buildingsmart-community/Community-Sample-Test-Files`; the previous
`buildingSMART/Sample-Test-Files` URL now redirects to certification datasets.
Archive extraction matches the resolved commit rather than a repository-name
prefix, so repository renames do not break the build.
`ENABLE_USD_SAMPLE_MODEL_DOWNLOAD` downloads the OpenUSD Kitchen Set and
PointInstancedMedCity archives into `models/OpenUSD-Sample-Assets` for USD
loader validation and follow-up work.
The glTF archive is large; if a slow connection still times out, increase
`GLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS` while configuring. Stalled
transfers are controlled separately by
`GLTF_SAMPLE_MODELS_INACTIVITY_TIMEOUT_SECONDS`.

To download or refresh only the USD samples through CMake:

```powershell
cmake --build out/build/windows-release --target download_usd_models --config Release
```

Run the renderer with a BIM sidecar model:

```powershell
$bim = "models\buildingSMART-IFC5-development\examples\Hello Wall\hello-wall.ifcx"
.\out\build\windows-release\VulkanSceneRenderer.exe --bim-model $bim
```

Use `--bim-import-scale` when a BIM source needs a unit or scene scale override.
The same sidecar route accepts `.usd`, `.usda`, `.usdc`, and `.usdz` mesh files.
`ENABLE_TINYUSDZ_USD_LOADER` controls the TinyUSDZ-backed importer; when it is
disabled, the fallback loader only handles lightweight ASCII USD/USDA meshes and
stored text-root USDZ packages.

## Windows Release Package

After a Release build, create a portable ZIP and SHA-256 checksum file:

```powershell
.\cmake\package_windows.ps1 -Version v0.1.0-preview.1 `
  -RuntimeDirectory out/build/visual-studio/Release `
  -VcpkgInstalledDirectory out/build/visual-studio/vcpkg_installed `
  -TinyUsdzSourceDirectory out/build/visual-studio/_deps/tinyusdz-src
```

For a Ninja preset, use `out/build/windows-release` as the runtime directory
and its corresponding `vcpkg_installed` and `_deps/tinyusdz-src` paths.
If dependencies were reused from another build, pass those actual paths.
Omit `-TinyUsdzSourceDirectory` when the build cache records
`ENABLE_TINYUSDZ_USD_LOADER=OFF`. An enabled build requires that source
directory's upstream and bundled notices. Use `-BuildDirectory` when the
runtime has been staged separately from its CMake cache.
The script requires an unused staging directory, includes dependency notices,
and writes the archive, `SHA256SUMS.txt`, and a build manifest to `out/packages`.
It deliberately includes local samples rather than the large downloaded model
collections. The Microsoft C++ runtime is installed separately through the
official x64 redistributable link in the included quick start guide.

Before publishing, extract the ZIP into a fresh directory and run both the
default deferred renderer and `--render-technique forward-raster --msaa 4`.
Also launch the executable from a different working directory to verify that
bundled materials and other runtime assets resolve beside the executable.
Run the capture with `--validation` on a development machine to check Vulkan
errors. Attach the ZIP and `SHA256SUMS.txt` to a GitHub release pointing to the
same commit recorded in `build-info.json`. Mark preview builds as prereleases.

## Tests

See [GFXReconstruct debugging](gfxreconstruct.md) for optional API capture,
replay, mapped-memory troubleshooting, and the opt-in GPU replay smoke test.

The frame-resource initialization regression checks startup, resize, and a
live 1× → 4× → 1× MSAA change with transparency and GPU picking in both raster
techniques. Run it with the Khronos validation layer available on a device
that supports 4× MSAA.

```powershell
$env:CONTAINER_RUN_GPU_FRAME_INITIALIZATION = '1'
ctest --test-dir out/build/windows-release -C Release -R '^frame_resource_initialization_gpu_regression$' --output-on-failure
```

To count the initialization submits and waits, run the same Forward fixture
under the LunarG API dump layer. Pass the directories that contain the
Khronos validation and LunarG API dump manifests.

```powershell
python tests/validation/frame_resource_initialization_api_trace.py `
  --renderer out/build/windows-release/VulkanSceneRenderer.exe `
  --output out/frame-resource-initialization-trace `
  --validation-layer "$env:VULKAN_SDK/Bin" --api-dump-layer "$env:VULKAN_SDK/Bin" --batched
```

Omit `--batched` when tracing a build that predates the batch. The report
counts the initialization command buffers and their waits separately from
rendering, upload, and swapchain retirement work.

### Graphics frame overlap

The Release benchmark compares the default graphics policy and
`--serialize-graphics` using the same executable, scene, camera, and frame
slot count. It measures both raster techniques at one, two, and three slots.
Provide the Cornell Box glTF and Tekla House IFCX fixtures from your build's
runtime assets. You can pass either fixture to run only its cases.

```powershell
$renderer = 'out/build/windows-release/VulkanSceneRenderer.exe'
$models = 'out/build/windows-release/models'
python tests/validation/graphics_concurrency_benchmark.py `
  --renderer $renderer `
  --gltf "$models/validation/cornell_box_local_light.gltf" `
  --ifcx "$models/buildingSMART-IFC5-development/examples/Tekla House/TeklaHouse.ifcx" `
  --output out/graphics-concurrency
```

Use Python 3.11 or newer. Add `--validation-manifest` with the path to
`VkLayer_khronos_validation.json` for a separate synchronization validation run.
The report includes the selected policy, pending frame counts, CPU phase
statistics, submission-matched GPU timings and collection latency, and VMA
allocations on device-local heaps. The whole-graphics timestamp span includes
GPU waits within the command buffer. Presentation has its own CPU phase.
The final screenshot writes the telemetry and is excluded from the steady sample.

CPU tests are enabled by default through `ENABLE_TESTS`. Window/Vulkan tests are
opt-in with `ENABLE_WINDOWED_TESTS=ON`.

The Windows swapchain retirement regression traces resize, minimize/restore,
and exit on a device with swapchain maintenance1. It also forces the base-WSI
path on the same device and verifies that older swapchains and their present
semaphores are reclaimed after the appropriate Vulkan fence signals.

```powershell
$env:CONTAINER_RUN_GPU_SWAPCHAIN_RETIREMENT = '1'
$env:CONTAINER_SWAPCHAIN_RETIREMENT_VALIDATION_LAYER_DIR = "$env:VULKAN_SDK/Bin"
$env:CONTAINER_SWAPCHAIN_RETIREMENT_API_DUMP_LAYER_DIR = "$env:VULKAN_SDK/Bin"
ctest --test-dir out/build/windows-release -C Release -R '^swapchain_retirement_gpu_regression$' --output-on-failure
```

The raster MSAA menu and `--msaa` use the sample counts supported by the
renderer’s shared color, integer pick ID, and depth/stencil attachments.
Requests use the highest supported count at or below the requested value.
Packed depth/stencil resolves prefer reverse-Z maximum depth and use a
common mode when the device requires depth and stencil to resolve together.
The opt-in capability test runs Forward and Deferred on the physical device,
then uses Khronos Profiles to require paired resolves and to limit integer
attachments to 1×. It checks the selected counts and modes at startup and
after live changes, as well as transparent object picking.

```powershell
$env:CONTAINER_RUN_GPU_MSAA_CAPABILITIES = '1'
$env:CONTAINER_MSAA_VALIDATION_LAYER = "$env:VULKAN_SDK/Bin"
$env:CONTAINER_MSAA_PROFILES_LAYER = "$env:VULKAN_SDK/Bin"
ctest --test-dir out/build/windows-release -C Release -R '^msaa_attachment_capabilities_gpu_regression$' --output-on-failure
```

Build and run the suite:

```powershell
cmake --build out/build/windows-release --config Release
ctest --test-dir out/build/windows-release --output-on-failure
```

Known Windows release status:

- `glm_tests`, `ecs_tests`, `scene_graph_tests`, `rendering_convention_tests`,
  `dotbim_loader_tests`, `ifc_tessellated_loader_tests`, and
  `ifcx_loader_tests`, `usd_loader_tests`, and `render_graph_tests` pass in the
  current Windows release build.
- Window/Vulkan tests require a working Vulkan runtime and display environment.

The helper script configures, builds, and optionally runs CTest:

```powershell
python rebuild.py --preset windows-release --run-tests
```
