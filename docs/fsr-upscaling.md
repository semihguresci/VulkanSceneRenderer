# Vulkan FSR 3.1.4 backend

The optional Windows x64 backend uses the signed Vulkan library from AMD
FidelityFX SDK 1.1.4 at commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.
`FsrUpscaler` records super-resolution work in the viewer's graphics command
buffer. Forward and deferred raster share the same temporal input conversion
and dispatch. [Pinned AMD release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v1.1.4)

Configure an existing build with the extracted SDK root. The renderer build
checks the signed DLL, four public headers and MIT license against the exact
commit. The standalone test and enabled application stage the DLL beside the
executable. The Windows package includes that DLL and the SDK license. Building
without the SDK produces the native fallback and requires neither the signed
DLL nor AMD's sample and shader build tools. [SDK license](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/LICENSE.txt)

```powershell
cmake -S . -B out/build/feature-parity -DENABLE_FSR_UPSCALING=ON -DCONTAINER_FSR_SDK_ROOT=F:/Libraries/FidelityFX-SDK-v1.1.4
cmake --build out/build/feature-parity --target fsr_upscaler_gpu_tests
$env:CONTAINER_RUN_GPU_FSR_UPSCALER = '1'
ctest --test-dir out/build/feature-parity -R '^fsr_upscaler_gpu_tests$' --output-on-failure
```

Launch with `--fsr quality`, `--fsr balanced`, or `--fsr performance` to
render at approximately 67%, 59%, or 50% of the display size on each axis.
`--fsr native` selects the normal resolution. ImGui uses the display extent.
FSR shares the single-sample perspective raster path. MSAA, orthographic and
reference rendering use native resolution. The FSR velocity and reactive mask
are available in `--display-mode taa-velocity` and `taa-reactive`.

## Frame inputs and submission

The wrapper takes one-layer, one-mip, single-sample Vulkan images. It imports
render-resolution linear HDR color (`RGBA16F`), jittered reverse-Z depth
(`R32F`), unjittered motion (`RG16F` or `RG32F`) and optional separate masks
(`R8_UNORM`). The caller transitions those images to
`SHADER_READ_ONLY_OPTIMAL` and the presentation-resolution `RGBA16F` storage
output to `GENERAL`. The caller owns producer and consumer barriers and keeps
all dispatches ordered on one graphics queue. The AMD backend owns its internal
history, views, and descriptors. The native GPU test exercises the driver with
graphics and compute support, extended storage image formats, Vulkan 1.1
16-bit storage, Vulkan 1.2 float16 and Vulkan 1.3 subgroup controls when
available. The SDK queries supported physical-device features itself, so an
enabled integration must supply compatible logical-device features. [Pinned Vulkan backend](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/backends/vk/ffx_vk.cpp)

The pinned SDK allocates luma history as `RGBA16F`, but its Vulkan shader
declares the storage image as `RGBA8`. The SDK's Vulkan resolver corrects the
SPIR-V image format in memory after matching one of the two pinned shader
variants. The signed DLL stays intact. If neither variant is recognized,
FSR initialization fails and the viewer uses native resolution.
[AMD issue #161](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/issues/161)

The projected jitter is in render pixels with +X right and +Y down. Use
`nextJitterPixels()` when producing both the jittered projection and the
dispatch. The existing velocity shader already measures `previousUv - currentUv`
in unjittered upper-left UVs. The dispatch uses positive render width and
height scales, with no additional Y sign change. Dispatch receives milliseconds,
physical camera near and far distances, perspective vertical FOV in radians,
and linear HDR with pre-exposure 1. The SDK runs internal exposure estimation
and returns linear HDR to the viewer's later exposure and tone mapping.
[AMD integration manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)

The first recorded frame resets history. Explicit camera or scene cuts also
set `reset`. `commit()` advances jitter only after a successful submit.
Discarding an already recorded frame invalidates the AMD context because the
SDK rotated its resources while recording. Recreate the context once work is
retired, and destroy it before destroying imported images. An ordinary resize,
quality change, or finite/infinite projection change uses the same retirement
boundary. The synthetic fixture checks repeated HDR dispatches, invalid input,
missing runtime fallback, and reconstruction of an abandoned dispatch.

## Resolution and composition

Depth, G-buffer, raster HDR, OIT, temporal inputs, culling tiles and scene-space
effects use the render extent. Swapchain framebuffers, display post-process,
ImGui and screenshots use the presentation extent. The camera projection uses
the presentation aspect, while its jitter and scene sampling use render pixels.

Both raster techniques share the `TemporalManager` velocity and HDR compose
boundary. FSR reuses that velocity and OIT composition, then converts the depth
and motion and writes separate reactive and composition masks. The shader uses
strong reactivity for new surfaces, unsupported motion, OIT coverage and
emissive shading. Exposure, bloom and post-processing consume the upscaled
linear HDR. Engine allocation telemetry covers its own velocity, conversion and
presentation images. The AMD runtime owns and manages its internal history.

`tests/validation/fsr_upscaling_runtime_regression.py` runs the native capture
comparison. Use `--suite full` to include forward transparency, temporal debug
inputs and UI capture. Its `--expect-unavailable` mode checks pixel-exact native
fallback when the SDK or its DLL is unavailable.

The RTX 2080 SUPER run produced 39 Vulkan-validation-clean captures. It covered
quality switches, scene reload, resize, camera cuts and recovery from the MSAA
fallback. In the 640×360 deferred cohort, median measured pass time was 0.888 ms
at native resolution and 0.711 ms with Quality. Reconstruction accounted for
0.186 ms and the engine's temporal images allocated 21,479,424 bytes. The
maximum mean absolute RGB difference from the native frames was 1.942/255.
At 1280×720, the forward transparency cohort measured 1.756 ms native and
1.134 ms in Performance mode, with 0.387 ms in reconstruction and 58,245,120
engine temporal image bytes. Measurements were taken with validation enabled.
Capture settings and per-frame diagnostics are in
`out/feature-parity/fsr-viewer-format-clean/results.json`.
