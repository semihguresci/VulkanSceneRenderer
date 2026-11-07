# Vulkan DLSS reconstruction

## Raster Super Resolution

The optional Windows x64 backend uses NVIDIA DLSS SDK 310.9.16. The build
checks the SDK headers, MSVC loader libraries, signed release DLL, and
license against the pin. An enabled build stages `nvngx_dlss.dll` beside
the renderer. The Windows package requires that DLL and the SDK notices.
[Pinned NVIDIA release](https://github.com/NVIDIA/DLSS/releases/tag/v310.9.16)

DLSS 4.5 Super Resolution supports GeForce RTX 20-series GPUs and later.
Its newer M/L models carry a higher performance cost on RTX 20/30-series
cards, which lack native FP8 acceleration.
[NVIDIA compatibility and model guidance](https://www.nvidia.com/en-us/geforce/forums/geforce-graphics-cards/5/580689/dlss-45-super-resolution-faq-updated-jan-14/)
This integration leaves the AI model preset to NGX. The Quality, Balanced,
and Performance controls select reconstruction quality and input dimensions.

```powershell
cmake -S . -B build/dlss -DENABLE_DLSS_UPSCALING=ON -DENABLE_DLSS_RAY_RECONSTRUCTION=ON -DCONTAINER_DLSS_SDK_ROOT=F:/Projects/Container/build/codex-gltf-validation/dlss-sdk-310.9.16
cmake --build build/dlss --target VulkanSceneRenderer
```

Use `--dlss quality`, `--dlss balanced`, or `--dlss performance`, or select
the mode under **Raster reconstruction** in the temporal controls. NGX
chooses the render dimensions for the current display extent and preset.
`--dlss native` selects the regular raster path. FSR and DLSS use the
same selection. Capture sequences can switch with a `dlss` or `fsr`
event, with one selection per event.

Super Resolution runs in perspective forward and deferred raster views at
MSAA 1x. Orthographic and multisampled raster views use native rendering.
An absent SDK, missing runtime DLL, unsuitable adapter, or failed feature
initialization leaves the scene at native resolution and reports the reason.
Instance and device creation query the required Vulkan extension sets for the
feature and keep the renderer's normal adapter selection.
[NGX Vulkan API](https://github.com/NVIDIA/DLSS/blob/v310.9.16/include/nvsdk_ngx_vk.h)

### Frame inputs and history

DLSS imports scene-linear RGBA16F, single-sample reverse-Z depth converted to
R32F, and two-component unjittered motion. The scene renderer uses
current-to-previous motion in upper-left UV coordinates. The wrapper
multiplies motion by the render dimensions through NGX's scale parameters.
The camera's Halton jitter is passed in render pixels with +X right and +Y
down. NGX runs automatic exposure on unscaled HDR with pre-exposure 1.
Exposure, bloom, tone mapping, and ImGui run on the display grid afterward.
[NVIDIA programming guide](https://github.com/NVIDIA/DLSS/blob/v310.9.16/doc/DLSS_Programming_Guide_Release.pdf)

The OIT pass resolves the current sorted transparent layers into premultiplied
color and opacity. The renderer reconstructs opaque HDR with DLSS, then samples
the transparent layer on the display grid with the current jitter offset.
This keeps translucent coverage out of opaque temporal history. The
transparent layer retains render-resolution shading and bilinear coverage.
The existing FSR and TAA transparency paths retain their composition rules.

Mode and resolution changes retire queued GPU work before replacing the NGX
feature and its images. Scene replacement, camera cuts, and explicit resets
clear reconstruction history. Capture telemetry records the active backend,
requested mode, render and display extents, optional NGX allocation, engine
image allocation, and the GPU time for the temporal resolve.

`tests/validation/dlss_upscaling_runtime_regression.py` compares native and
DLSS output during camera and glass motion, mode switches, and resize.
`--expect-unavailable` checks pixel-identical native fallback.

The integrated 310.9.16 run, built with Slang 2026.21, passed 29
synchronization-validation-clean captures across five processes and both
raster techniques on the RTX 2080 SUPER. Deferred Quality reconstructed
427×240 input to 640×360 in a median 1.058816 ms. Forward Performance
reconstructed 640×360 input to 1280×720 in a median 1.690080 ms and reported
59,083,942 vendor bytes. Engine temporal image allocations were 24,133,632
bytes in deferred Quality and 64,143,360 bytes in forward Performance.
Median totals across the measured GPU passes were 0.875040 ms native and
1.578112 ms Quality in deferred, and 1.417600 ms native and 2.352352 ms
Performance in forward. These timings describe the regression fixtures.

The suite also checked moving glass, camera cuts, reload, resize, MSAA
fallback, and switching between DLSS and active FSR 3.1.6. Frozen runtime,
shader, helper, and layer hashes matched before and after the run. Evidence
is in `build/codex-gltf-validation/latest-dlss-sr-integrated/results.json` and
`build/codex-gltf-validation/latest-dlss-sr-integrated-inputs.json`.
The accepted SR renderer SHA-256 is
`a250881bfdfd0832d11fcf6a7838926e3bc3a59ef6e9c2a098837763ccf0cf63`.

A separate SDK-disabled build without the NVIDIA DLL passed 18 captures.
All nine requested-DLSS frames matched native output pixel for pixel.
DLSS, hybrid reflections, fog, and a display-space depth readback also
passed together at a 240×180 render extent and 480×360 display extent.
Evidence is in `out/feature-parity/dlss-sdk-off/results.json` and
`out/feature-parity/dlss-fog-reflection-depth/results.json`.

## Reference Ray Reconstruction

Enable `ENABLE_DLSS_RAY_RECONSTRUCTION` with the same pinned SDK root. This
option stages `nvngx_dlssd.dll` and can be enabled independently of raster
Super Resolution. Select `--path-dlss quality`, `--path-dlss balanced`, or
`--path-dlss performance` in the reference path tracer. The same options
appear in **Reference display denoising** in the editor. Use `--path-dlss off`
to disable reconstruction. A capture sequence can select the mode with a
`referenceDlss` event.

Two dispatches use independent image grids and sample streams. The reference
path tracer accumulates its running mean and variance at the display extent.
A second dispatch traces one noisy scene-linear HDR sample per pixel at the
NGX-selected input extent each frame. NGX reconstructs the display from that
sample and its guides. Scene-linear EXR captures continue to read the raw
full-resolution mean, including when Ray Reconstruction drives the viewport.
Both dispatches contribute to frame time. The composition preserves the raw
renderer’s diagnostic colors.
The viewport falls back to raw accumulation if the SDK or runtime is
unavailable, the renderer flags the whole scene as unsupported, a thin-lens
aperture is active, or the shadow matte is active.

The guide dispatch sends RGBA32F HDR color, R32F positive linear camera
depth, RG32F motion, RGBA16F diffuse and specular albedo, RGBA16F signed world
shading normals with linear roughness in alpha, and R32F specular hit
distance. Motion is the unjittered current-to-previous displacement in render
pixels, with positive X right and positive Y down. It uses the current and
previous camera projections and the previous triangle and instance transform
at the hit barycentrics. Sky motion uses a direction at infinity. NGX receives
the projected image jitter, which has the opposite sign to the sample-point
offset, and unscaled HDR at unit pre-exposure. Sky depth is the maximum finite
float. A specular ray that hits the sky uses a hit-distance sentinel of 65,504.
Pixels without a sampled specular reflection use a distance of zero.
[Pinned NVIDIA integration guide](https://github.com/NVIDIA/DLSS/blob/v310.9.16/doc/DLSS-RR%20Integration%20Guide.pdf)

The specular distance follows the sampled glossy reflection from its primary
surface. A smooth straight-through sheet can use the surface seen through
it, with its albedo tinted by the sheet. Curved refraction and rough
transmission keep the primary interface's normal, depth, and motion. The noisy
input carries the actual reflected and transmitted radiance in both cases.

Compatible camera motion and transforms or deformation with matching
topology reuse history. A camera translation over five world units, a
change of more than 60 degrees in the camera's viewing axis, or a projection
change starts a new history. Viewport resize, backend or quality switches,
scene replacement, topology changes, and explicit resets also clear history.

The 310.9.16 integration, built with Slang 2026.21, passed 39 captures across
seven processes on the RTX 2080 SUPER with synchronization validation
1.4.363.1 enabled. Quality reconstructed 427×240 to 640×360. Against a
128-sample Cornell-box display reference, normalized RGB RMSE was 0.167962 for
one native sample and 0.0299094 for the first reconstructed frame. At frame
eight it was 0.094755 for the native display capped at four samples and
0.0235967 for reconstruction with eight one-sample history frames. NGX
evaluation took a median 3.010752 ms and reported 32,439,452 bytes. The renderer
allocated 10,109,504 additional bytes for guides, output, and uniforms.
The raw EXR matched both in decoded float bits and encoded bytes.

The moving emissive marker stayed within 0.41614 output pixels of its native
centroid through camera motion, object motion, a cut, and resize. The
measured thin-glass tint matched the native display. At 704×400, the three
presets used 469×267, 408×232, and 352×200 input grids. Their measured NGX
dispatches took 3.28032, 3.000704, and 2.437568 ms respectively.
These timings describe the regression fixtures.

The NGX-specific Vulkan resolver adds memory dependencies around two private
initialization clears in the first RR evaluation. It matches the observed
single-image transition and clear parameters, makes preceding writes visible
to the clear, and publishes the clear's writes to subsequent work. The signed
NVIDIA DLLs remain unchanged. Nine CPU mock tests verify the matching guards,
original argument forwarding, and isolation by feature, evaluation, thread,
and command buffer. The full native regression above passed with strict
validation and unchanged runtime, shader, helper, and layer hashes.

Evidence is in
`build/codex-gltf-validation/dlss-latest-rr-synchronized-full/results.json` and
`build/codex-gltf-validation/dlss-latest-rr-synchronized-full-inputs.json`.
The accepted renderer SHA-256 is
`432e749a58284e1f1ff601909b1c083c2f2c0cfb03d6884b3ea99ea01d997871`.

Run `tests/validation/reference_dlss_runtime_regression.py` against the
SDK-enabled renderer to reproduce the viewport and raw EXR comparisons.
Use `--expect-unavailable` with the SDK-disabled build.
