# Vulkan FSR upscaling with AMD FSR SDK 2.4.0

The optional backend uses the signed Windows x64 Vulkan binaries from AMD FSR
SDK 2.4.0 at commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`. The SDK selects a
compatible provider and the engine reports its version after context creation.
The RTX 2080 SUPER audit exercised the bundled analytical FSR 3.1.6 provider.
AMD also lists ML providers for supported AMD hardware and drivers. The table
below records the versions of the installed DLLs separately from the provider.
[Pinned AMD release](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v2.4.0)
and [AMD hardware/provider guidance](https://gpuopen.com/amd-fsr-sdk/).

| Signed Windows x64 Vulkan runtime | PE file version |
| --- | --- |
| `amd_fidelityfx_loader_vk.dll` | 1.0.2.4555 |
| `amd_fidelityfx_upscaler_vk.dll` | 4.1.2.4555 |

Configure an existing build with the SDK root. The build checks both signed
Vulkan DLLs, the four public headers and license/notices against the pin. The
application and standalone GPU test stage `amd_fidelityfx_loader_vk.dll` and
`amd_fidelityfx_upscaler_vk.dll` together beside the executable. The loader
contains no effect code. The wrapper owns both module leases, rejects a
same-name module from another directory, and destroys the context before
unloading either module. Missing modules leave the scene at native resolution.
Building without the SDK requires neither AMD DLL nor AMD sample tools.

Both shipped Vulkan DLLs appear among the MIT exceptions in the pinned SDK
license. The Windows package includes `AMD-FSR-SDK-2.4.0-LICENSE.txt` and
`AMD-FSR-SDK-2.4.0-THIRD-PARTY-NOTICES.md` under `THIRD_PARTY_LICENSES/`. The
packager uses CMake's actual FSR availability and checks both DLL hashes.
[Pinned SDK license](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/docs/license.md).

```powershell
cmake -S . -B build/fsr -DENABLE_FSR_UPSCALING=ON -DCONTAINER_FSR_SDK_ROOT=F:/Libraries/FidelityFX-SDK-v2.4.0
cmake --build build/fsr --target fsr_upscaler_gpu_tests
$env:CONTAINER_RUN_GPU_FSR_UPSCALER = '1'
ctest --test-dir build/fsr -R '^fsr_upscaler_gpu_tests$' --output-on-failure
```

## SDK 2.4.0 compatibility matrix

| `--fsr` mode | Display-to-render ratio per axis | Render extent at 640 × 360 output | Temporal resolve |
| --- | --- | --- | --- |
| `native` | 1.0× | 640 × 360 | TAA when enabled |
| `quality` | 1.5× | 426 × 240 | FSR |
| `balanced` | 1.7× | 376 × 211 | FSR |
| `performance` | 2.0× | 320 × 180 | FSR |

The mode table describes a compatible device and viewport. The render extent
rounds each axis down. If either render dimension falls below two pixels,
the FSR context is rejected and rendering stays at native resolution. ImGui and
captures use the display extent. FSR owns jitter and temporal resolve while
active, including when TAA is selected. Native resolution can use TAA after
FSR is disabled or falls back, subject to the normal TAA requirements.

| Capability or request | Result | Audit coverage |
| --- | --- | --- |
| Windows x64 build with the pinned SDK, both DLLs and a compatible Vulkan device | Uses the SDK-selected provider. The engine applies its format adapter to analytical 3.1.6. | Analytical 3.1.6 on RTX 2080 SUPER. AMD ML and other driver providers are unverified. |
| `ENABLE_FSR_UPSCALING=OFF` or enabled with an empty SDK path | Builds without Vulkan FSR. An upscale request runs at native resolution. | Separate SDK-off build and native image comparison. |
| A supplied SDK root has a missing or mismatched pinned file | CMake rejects that configuration. | Pin checks in `cmake/FsrUpscalerDependencies.cmake`. |
| SDK-enabled build with a missing adjacent loader or upscaler | The runtime stays at native resolution and reports the unavailable DLL pair. | Staged runtime capture and standalone missing-module test. |
| Required Vulkan features or image formats absent, or the provider context cannot initialize | The runtime stays at native resolution and reports why FSR could not activate. | Source audit and standalone failure checks. The viewport device-feature rejection path still needs a hardware capture. |
| Perspective deferred or forward raster, MSAA 1×, at most four frames in flight | Eligible in Lit, `taa-velocity` and `taa-reactive` display modes. | Deferred mode transitions, forward Performance, velocity and reactive diagnostics, and UI capture. |
| MSAA above 1×, orthographic or reference rendering, another display mode, or more than four frames in flight | Retains the requested mode and renders at native resolution. | Live MSAA fallback and recovery. The other eligibility gates were inspected in `RendererFrontend::currentUpscaleResourceRequest`. |
| Linux or 32-bit Windows | Vulkan FSR SDK backend stays disabled. | Configure gate. Only Windows x64 is supported by the pinned binaries. |

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
history, views, and descriptors. The engine requires a graphics queue with
compute support, storage image read/write without format, extended storage
image formats, subgroup size control and full compute subgroups. If the device
advertises `shaderFloat16`, it must also support `storageBuffer16BitAccess`.
The SDK queries physical-device features and needs the corresponding features
enabled on the logical device. [Pinned Vulkan backend](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/backend/vk)

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

## Executable verification

Build `fsr_vulkan_image_adapter_tests` for CPU coverage of the pinned format
adapter. Run the GPU target with the opt-in environment variable shown above
to exercise module loading, context creation, HDR dispatch and dispatch
abandonment on Vulkan.

With Python 3.11 or newer, NumPy and Pillow, run the end-to-end capture fixture
from the repository root. Point `$manifest` to the
`VkLayer_khronos_validation.json` used by the installed Vulkan validation layer.
Give each run a new output directory.

```powershell
python -B tests/validation/fsr_upscaling_runtime_regression.py `
  --renderer build/fsr/VulkanSceneRenderer.exe --output build/fsr-verification/active `
  --suite full --expect-provider 3.1.6 --validation-manifest $manifest
python -B tests/validation/fsr_upscaling_runtime_regression.py `
  --renderer build/fsr-off/VulkanSceneRenderer.exe --output build/fsr-verification/sdk-off `
  --expect-fallback sdk-disabled --validation-manifest $manifest
python -B tests/validation/fsr_upscaling_runtime_regression.py `
  --renderer build/fsr-missing-dll/VulkanSceneRenderer.exe --output build/fsr-verification/missing-dll `
  --expect-fallback missing-runtime --validation-manifest $manifest
```

Build the second executable with `-DENABLE_FSR_UPSCALING=OFF`. For the third
run, stage the SDK-enabled executable with its runtime files in a separate
directory and omit `amd_fidelityfx_upscaler_vk.dll`. Keep the loader beside the
executable. The fallback tests compare PNG pixels against ordinary native
rendering and check the reported cause. The active suite checks the provider,
render and display extents, forward transparency, motion and camera cuts,
mode switches, reload, resize, MSAA recovery, velocity and reactive displays,
and UI capture. It also switches between standalone TAA and FSR, checking
resolve ownership, jitter and history. Each run records DLL and executable
identity, validation settings and the loaded validation library in
`results.json` alongside the per-frame telemetry and screenshots.

### Audit results

The SDK 2.4.0 build passed 18 CPU adapter tests and four wrapper tests, including
three Vulkan device tests. The RTX 2080 SUPER with NVIDIA driver 610.88 used
analytical 3.1.6 and passed 44 captures across nine processes. Both fallback
profiles passed 17 captures each. In each profile, all six frames requested
with Quality were pixel-identical to their native control frames. Vulkan
validation layer 1.4.363.1 reported no errors with synchronization validation
enabled. The maximum mean absolute RGB difference from native across the six
active Quality frames was 1.935 on the 0–255 scale.

| Fixture and mode | Median instrumented GPU pass total | Median resolve GPU time | Engine temporal images |
| --- | ---: | ---: | ---: |
| 640 × 360 deferred Native | 18.449 ms | — | 0 bytes |
| 640 × 360 deferred Quality | 6.338 ms | 0.188 ms | 27,377,664 bytes |
| 1280 × 720 forward Native | 102.267 ms | — | 0 bytes |
| 1280 × 720 forward Performance | 20.632 ms | 0.754 ms | 81,838,080 bytes |

These are fixture measurements with synchronization validation enabled and TAA
disabled for the four rows. The memory counts cover engine temporal images.
AMD-owned context allocations require separate measurement.

Frame 8 from `models/validation/taa_scene.gltf` at 640 × 360 uses the same
camera, lighting and scene state in each capture.

| Native at 640 × 360 | FSR Quality at 426 × 240, reconstructed to 640 × 360 |
| --- | --- |
| ![Native capture of the motion and transparency fixture](images/fsr/sdk-2.4.0-native.png) | ![FSR Quality capture of the same fixture](images/fsr/sdk-2.4.0-quality.png) |

## Remaining work

- Complete the resize, reload, viewport/UI and blocked-profile matrix in
  [#302](https://github.com/semihguresci/VulkanSceneRenderer/issues/302),
  including orthographic and reference rendering.
- Expand motion, transparency, shading-change, exposure and ghosting coverage
  in [#303](https://github.com/semihguresci/VulkanSceneRenderer/issues/303).
- Benchmark representative Sponza, glTF and BIM models and qualify the
  extracted Windows package on another machine in
  [#304](https://github.com/semihguresci/VulkanSceneRenderer/issues/304).
- Qualify AMD ML and other SDK-selected providers on supported devices.
  Verify the native fallback on a device missing the required features and
  account for AMD-owned context allocations separately from engine images.
- The CMake gate checks Windows and 64-bit pointer width. Native Windows
  ARM64 and ARM64EC builds need an explicit target architecture check before
  enabling the packaged x64 runtime.
- Native fallback covers initial FSR selection and context setup. Later
  allocation failures in the FSR frame resources and errors querying jitter
  or recording an active dispatch still propagate. Recovery needs to retire
  the failing context and recreate native frame resources.
