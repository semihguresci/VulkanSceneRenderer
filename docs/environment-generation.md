# Vulkan environment generation

`EnvironmentManager` owns the unfiltered radiance cubemap, diffuse irradiance,
specular mip chain, BRDF integration LUT and GTAO resources. The radiance view
supplies the reference path tracer without sampling its specular approximation.
The currently published application loads its default HDR through the same API.
Viewport controls, presentation uniforms, solar scene lighting and the full
reload controller remain separate publication dependencies.

## Loading and publication

`loadHdrEnvironment(path)` decodes and generates a replacement synchronously;
call it only when no request is pending.
`requestHdrEnvironment(path)` starts CPU decoding on a worker; that worker has
no Vulkan access. Call `pollEnvironment(wait, beforePublish)` on the render
thread to submit generation and publish a completed replacement. Normal polling
may need several frames. A true `wait` waits for both preparation stages.

The retirement callback runs after generation completes, while the old images
are still active. Retire every reader there. Successful publication replaces the
images; refresh all consuming descriptors before their next submission. Decode,
generation and retirement-callback failures preserve the active images. A pending
load rejects another request. Resource destruction also waits for its submitted
generation and joins its CPU worker. These APIs require serialized render-thread
ownership and callers that outlive their resources and callbacks.

OpenEXR and Radiance HDR paths support Unicode and case-insensitive extensions.
Encoded and decoded data have finite budgets; OpenEXR dimensions, channels and
data-window arithmetic are checked before image allocation. Components must be
finite and fit the half-float cubemap. Values above one are preserved. These
budgets do not bound private codec scratch or driver allocations.

`TextureImageData` supplies the CPU Radiance decoder and the complete existing
typed PNG/DDS/KTX/WebP API. Its optional codecs and notices are configured in
`TextureCodecDependencies.cmake`; see [typed texture data](texture-image-data.md).
GPU texture upload and glTF texture authoring remain later publication steps.

## Sky resources and revision identity

`loadSkyEnvironment(settings)` and `requestSkyEnvironment(settings)` generate
plain, gradient or atmosphere cubemaps. Gradients use a normalized up direction
and independent horizon-to-pole curves. Atmosphere generation supports geometric
or location/date/time-derived sun angles, density scales and the existing
multiple-scattering LUT. The manager exposes the matching atmosphere/sun data;
scene lighting and the visible sun disk require their consuming renderer paths.
Reference transport still reports atmosphere/solar scenes as unsupported until
that transport is implemented. Generating a sky is not proof of that feature.

`environmentRevision()` changes on successful radiance publication, creation and
destruction, independently of Vulkan handle reuse. BRDF-only rebakes retain that
radiometric revision. Temporal consumers include it in their content key; the
local reference frontend does so. A separate publication revision guards staged
rebakes against any intervening environment/BRDF publication or destruction.

## Shader rebaking and command lifetime

`prepareShaderBake(directory, brdf, environment)` builds detached replacement
textures from the candidate shaders. Retained encoded HDR bytes permit rebaking
after the original file is removed. Procedural skies retain their full settings.
Check `canPublishShaderBake()` immediately before publication, retire readers,
publish, refresh descriptors, then release the candidate holding the old images.
Abandonment keeps the live generation unchanged.

BRDF staging uses `AllocationManager::executeStagedCommands`. Recording failure
frees the unsubmitted command. Submitted commands retire before a failed progress
callback can unwind resource ownership. Upload guards reject allocator cleanup
and geometry/texture reset during that operation. Reentrant texture retirement
is guarded independently. Future typed texture uploads use the same primitives.

The production-owner GPU tests sample all six cubemap axes, compare plain and
gradient results to independent colors, check HDR values above one and finite
atmosphere output, exercise asynchronous failure/publication and retained-source
rebaking, and verify radiance versus BRDF revision behavior. They also dispatch
and read back staged work across recording/progress failures with synchronization
validation. CPU decoder tests cover exact precision, authored mip data, native
blocks, optional codecs and invalid inputs; their disabled-codec profile records
the two optional KTX skips separately.

The environment publication gate passes 234 cases in 11 selected executables
(seven Vulkan), including 26 decoder cases and eight production-owner cases.
The codec-disabled CPU profile separately passes 23 cases with two expected KTX
skips. All 96 SPIR-V modules validate; only the equirectangular generator changes
relative to the preceding producer, plus the new six-axis test probe. The broader
source-convention suite retains the same eight parent failures.

Ninety publication-profile native captures/logs include four prior-executable
baselines. Eighty compatibility images match the preceding checkpoint exactly;
active bloom and original ACCA raster comparisons also match in both techniques.
The full local editor separately passes 690 cases in 20 executables (ten Vulkan),
50 sky/HDR transition captures in 12 clean process logs, and one original ACCA
path-traced replay at 128 samples with zero diagnostics. Its 105 engine shaders
are unchanged. The original IFCX is read in place. Viewport, scene-lighting and
reload-controller publication remains pending. Frozen evidence is recorded in
`out/feature-parity/environment-publication-acceptance/results.json`.
