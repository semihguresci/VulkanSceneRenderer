# Vulkan FSR upscaling with AMD FSR SDK 2.4.0

The optional Windows x64 backend uses AMD FSR SDK 2.4.0 at commit
`7539ae8b6568ad29ed78a753404351daf3b5cca7`. The API selects the newest compatible
provider. On hardware without AMD ML-upscaler support, the bundled analytical
provider is FSR 3.1.6. This is the intended path on NVIDIA RTX 20-series hardware.
The wrapper queries and reports the actual provider version after creation.
The upscaler DLL's 4.1.2 file version does not establish ML support on NVIDIA.
[Pinned AMD release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0)
and [AMD hardware/provider guidance](https://gpuopen.com/amd-fsr-sdk/).

Configure an existing build with the SDK root. The build checks both signed
Vulkan DLLs, the four public headers and license/notices against the pin. The
application and standalone GPU test stage `amd_fidelityfx_loader_vk.dll` and
`amd_fidelityfx_upscaler_vk.dll` together beside the executable. The loader
contains no effect code. The wrapper owns both module leases, rejects a
same-name module from another directory, and destroys the context before
unloading either module. Missing modules leave the scene at native resolution.
Building without the SDK requires neither AMD DLL nor AMD sample tools.

The Windows package includes the full SDK redistribution license and third-party
notices. The license's MIT exceptions apply to the listed source files; its
default binary redistribution terms apply to the signed SDK distribution.
[Pinned SDK license](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/license.md).

```powershell
cmake -S . -B build/fsr -DENABLE_FSR_UPSCALING=ON -DCONTAINER_FSR_SDK_ROOT=F:/Libraries/FidelityFX-SDK-v2.4.0
cmake --build build/fsr --target fsr_upscaler_gpu_tests
$env:CONTAINER_RUN_GPU_FSR_UPSCALER = '1'
ctest --test-dir build/fsr -R '^fsr_upscaler_gpu_tests$' --output-on-failure
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
`SHADER_READ_ONLY_OPTIMAL` and the presentation-resolution HDR storage
output in `FsrUpscaler::outputFormat()` to `GENERAL`: RGBA32F for the analytical
3.1.6 provider and RGBA16F for the automatic ML/default profile. The caller owns
producer and consumer barriers and keeps
all dispatches ordered on one graphics queue. The AMD backend owns its internal
history, views, and descriptors. The native GPU test exercises the driver with
graphics and compute support, extended storage image formats, Vulkan 1.1
16-bit storage, Vulkan 1.2 float16 and Vulkan 1.3 subgroup controls when
available. The SDK queries supported physical-device features itself, so an
enabled integration must supply compatible logical-device features. [Pinned Vulkan backend](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/backend/vk)

SDK 2.4.0 analytical shaders also require the device extension
`VK_KHR_compute_shader_derivatives` and `computeDerivativeGroupLinear`.
The engine queries support and enables them before activating FSR; Vulkan 1.4
alone does not supply this extension.

The SDK 1.1.4 integration's exact luma-history SPIR-V rewrite is removed for
SDK 2.4.0. The wrapper passes the signed provider's shaders through unchanged.
The pinned analytical 3.1.6 provider declares Float32 scalar/vector shader images for
fifteen internal images while requesting UNORM8/FP16 host allocations. A private
Vulkan dispatch adapter creates these images and all their views as matching
R32/RG32/RGBA32 float formats. It covers the two luma images,
the shared farthest-depth/luma-instability intermediate, farthest-depth mip1,
SPD pyramid, private dilated velocity, two accumulation images, shading-change
and new-lock masks, two internal color histories, two luma histories, and the
dilated reactive masks. All have uninitialized data and
use default heaps; no packed CPU uploads, readbacks or image copies need
conversion. Their allocations follow Vulkan's actual memory requirements and
their texel storage increases by 2x to 4x. Valid mask values remain in [0,1]
through the SDK's saturation/threshold logic; the wider images remove UNORM8
rounding and FP16 rounding. This preserves continuous value meanings and is not
an exact output-equivalence claim; temporal threshold effects are covered by
the fixture acceptance below. The initialized sampled-only default R8 mask and SNORM LUT remain
unchanged. The adapter queries exact format usage,
dimensions, mip count and linear filtering support. Unsupported devices retain
native rendering. [Pinned resource descriptions and creation](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp#L524)

A passthrough probe first lets the SDK choose its automatic provider. Only the
exact pinned analytical 3.1.6 result is recreated with an enumerated explicit
provider ID and the adapter, then verified again. ML and driver providers keep
their unmodified contexts. Caller images are absent from the adapter's fixed
ownership table and keep their formats; the private velocity image's reimported
views are corrected by owned image handle. Context scopes and image destruction
remove ownership entries before module release.

The analytical provider's caller-owned HDR output is allocated directly as
RGBA32F before descriptor creation, without a conversion copy. Both raster
techniques' post-processing, exposure and bloom read it through sampled
Texture2D inputs; their own typed storage outputs retain their formats. DLSS
and the ML/default FSR profile continue to allocate RGBA16F output. Temporal
resource caching and logical payload accounting use the selected output format.

Unexpected internal image/view contracts latch a failure. An unexpected view
of an already promoted image receives a legal compatible placeholder so the
SDK can finish creating a destructible context. That candidate is then rejected
before submission; a mismatch while recording discards the commands. Actual
Vulkan failure codes are preserved. The SDK's public source has a preexisting
partial-create cleanup limitation: driver allocation failure can return before
publishing a context, without releasing every partial CPU/GPU allocation. The
adapter does not claim to repair that SDK error path. Failed destruction is
reported and keeps the runtime modules resident rather than pretending their
objects were released. Pre-context SDK memory estimates still describe narrow
formats; only actual context memory accounting includes the wider allocations.
New SDK numerical and native acceptance must pass strict Vulkan validation;
historical shader fingerprints are not applied to this runtime.

The projected jitter is in render pixels with +X right and +Y down. Use
`nextJitterPixels()` when producing both the jittered projection and the
dispatch. The existing velocity shader already measures `previousUv - currentUv`
in unjittered upper-left UVs. The dispatch uses positive render width and
height scales, with no additional Y sign change. Dispatch receives milliseconds,
physical camera near and far distances, perspective vertical FOV in radians,
and linear HDR with pre-exposure 1. The SDK runs internal exposure estimation
and returns linear HDR to the viewer's later exposure and tone mapping.
[AMD integration manual](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/)

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

SDK 2.4.0 passed the enabled build, all four strict Vulkan GPU cases and all
18 CPU adapter tests. The native RTX 2080 SUPER regression passed 39 captures
across eight processes using the actual analytical 3.1.6 provider. It covered
both raster techniques, camera motion and cuts, quality/backend switches,
scene reload, resize, recovery from MSAA fallback, forward transparency,
velocity/reactive diagnostics and UI capture. Validation layer 1.4.363.1 was
loaded with its duplicate-message cap raised to 1000; every process log was
clean, and frozen runtime, helper and layer hashes matched before and after.

In the 640×360 deferred fixture, median instrumented GPU pass totals were
0.868 ms at native resolution and 0.925 ms with Quality. The reconstruction
pass measured 0.248 ms; the engine's temporal images allocated 27,377,664 bytes.
The maximum mean absolute RGB difference from native frames was 1.938/255.
At 1280×720, the forward transparency fixture measured 1.434 ms native and
1.129 ms in Performance mode, with a 0.459 ms reconstruction pass and
81,838,080 engine temporal image bytes. These are validation-enabled fixture
measurements, not general performance guarantees or SDK internal-memory totals.

Capture settings and per-frame diagnostics are in
`build/codex-gltf-validation/latest-fsr-native-full/results.json`; the frozen
input receipt is `build/codex-gltf-validation/latest-fsr-native-full-inputs.json`.
The accepted renderer SHA-256 is
`a250881bfdfd0832d11fcf6a7838926e3bc3a59ef6e9c2a098837763ccf0cf63`.
