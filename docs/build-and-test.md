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
Run `--suite mutations --slangc C:/VulkanSDK/1.4.328.1/Bin/slangc.exe` after
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

Set `VCPKG_ROOT` before configuring. Keep machine-local overrides in an
untracked `CMakeUserPresets.json`.

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

CPU tests are enabled by default through `ENABLE_TESTS`. Window/Vulkan tests are
opt-in with `ENABLE_WINDOWED_TESTS=ON`.

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
