# LTC area lighting

[Epic #40](https://github.com/semihguresci/VulkanSceneRenderer/issues/40) adds
linearly transformed cosine (LTC) integration for rectangular and disk emitters.
Forward opaque, deferred and forward transparent lighting use the same helper.
LTC integrates unoccluded material response; raster or ray-query visibility is
applied once afterwards. The default is LTC when its lookup tables are valid.

## Controls

Choose **Area light integration** in Lighting Settings, or use:

```powershell
.\VulkanSceneRenderer.exe --display-mode lit --area-lighting ltc
.\VulkanSceneRenderer.exe --display-mode lit --area-lighting sampled --area-light-samples 25
```

`--area-light-samples` accepts 9, 25 or 64. The 25-sample mode retains the
existing Gauss-Legendre baseline. These samples also serve conservative fallback
and residual terms; they are independent of `--area-shadow-quality` and
`--ray-shadow-samples`. Changing the integration mode or quadrature quality
resets TAA colour history. Capture events accept `areaLighting` and
`areaLightSamples` for deterministic comparisons and reset checks.

**Emitter geometry at authored size**, or `--area-emitter-debug` with the UI
enabled, shows rectangle/disk outlines and their emitting normals in the light
overlay. Normal editor overlays and light gizmos must be enabled. The display
uses the emitter's world position, orientation and half-size, without the
ordinary scene-relative coverage scaling.

## Tables and resources

The fitter adapts the parameterization and optimization from
[selfshadow/ltc_code](https://github.com/selfshadow/ltc_code), pinned at
`31e5e96b54f98f33098f8503003119ba2231a1c6`, under its permissive license with
the required notice and paper citation. See `materials/ltc/LICENSE.txt` and
[the reproducible fitter](https://github.com/semihguresci/VulkanSceneRenderer/blob/main/tools/ltc_fit/README.md). Reference:
*Real-Time Polygonal-Light Shading with Linearly Transformed Cosines*, Eric
Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt, ACM SIGGRAPH 2016
([project and paper](https://eheitzresearch.wordpress.com/415-2/)).

The tables fit this renderer's existing GGX/Schlick BRDF, including its minimum
roughness and denominator clamps. The original Smith-GGX tables would change
low-roughness lighting and are not used. The two 64×64 linear RGBA float32
textures require 131,072 bytes of pixel payload; telemetry reports actual VMA
allocation bytes. The offline fitter is not a runtime dependency.

Each file has a 16-byte little-endian `CLTC` version-1 header with width/height,
followed by row-major RGBA float32 texels. X is perceptual roughness; Y is
`sqrt(1 - NdotV)`. Linear clamp-to-edge sampling uses a half-texel inset at the
table edges. Matrix channels pack inverse transform elements in GLM column/row
order `[0][0], [0][2], [2][0], [2][2]`, normalized by `[1][1]`.
Amplitude R stores the fitted unit-F amplitude: target hemispherical energy
divided by the cosine mass retained by the physical receiver clip. G stores
its correspondingly normalized Schlick-Pow5 moment; B stores the
cosine-weighted diffuse Fresnel moment, and A is zero. The shader reconstructs
the matrix explicitly to preserve the engine's column-major convention.

Vulkan-Hpp RAII owns image views, sampler and upload commands; VMA owns image
storage. Tables are immutable across scene reload and resize. Per-image light
descriptors retain valid bindings even when tables are unavailable. Missing,
malformed, nonfinite or invalid tables select sampled lighting and report the
reason in Lighting Settings and capture telemetry. Shader-read barriers follow
startup uploads; normal rendering adds no queue idle or per-frame uploads.

## Integration and limits

Rectangles use cosine polygon integration. Disks start with 32 circular sectors
and subdivide each sector according to a bound on arc-to-chord deviation relative
to the nearest receiver distance in both physical and fitted LTC space. The bound
is 0.002 in each space; sectors support up to 512 subdivisions. Ordinary distant
disks retain the 32-sided path, whose far-field inscribed-area deficit is about
0.641%. Near-rim disks refine their boundary while keeping the cosine and
constant-F0 integration analytic. Sources that exceed the finite subdivision
budget preserve the configured sampled response. Both clip
against the physical receiver horizon and the
transformed LTC horizon, handle authored emitter sidedness, and guard degenerate
edges. Near-antiparallel edges use an angle/cross-length evaluation to avoid
losing their contribution when the dot product rounds to −1.

Material F0/base colour, metallic response, clearcoat attenuation and lobe, and
the existing sheen response follow the shared layered-lighting conventions.
The G-buffer preserves authored base roughness, albedo and emission instead of
altering them to approximate layers that deferred lighting evaluates separately.
Iridescence shares tint and thickness conventions with forward lighting:
maximum thickness without a thickness texture, with tint applied to the mixed
dielectric/conductor reflectance before its final clamp. Two RGBA16F G-buffer
attachments preserve sampled clearcoat factor/roughness, sheen colour/roughness,
and iridescence factor/thickness, including texture-coordinate transforms.
Directional, point, tiled and area lighting consume these values. Deferred
environment lighting evaluates clearcoat and sheen separately, matching the
forward layer ordering. Core PBR texture sampling remains supported.

The two RGBA16F layer targets add 16 bytes per pixel per swapchain image at
1× samples. At MSAA sample count S, their multisample targets plus resolves add
`16 × width × height × (S + 1)` bytes per image, before allocation alignment.
At 960×540 this is 7.91 MiB per image at 1× or 39.55 MiB at 4×. This metadata
also adds G-buffer bandwidth; the Lighting-pass benchmarks below do not include
that pass's cost.

The renderer's NDF denominator floor produces a flattened low-roughness lobe
that a single LTC cannot fit accurately. Base or active clearcoat roughness
below 0.28 retains sampled lighting; a smooth transition from 0.28 to 0.34 avoids
a hard seam in roughness textures. When the geometry and Fresnel guards accept
full LTC, the shader skips sampled GGX evaluation.
Fresnel approximations are bounded over the emitter domain. A source-plane cone
and the physical receiver hemisphere complement the enclosing-sphere cone;
that sphere constraint relaxes smoothly within 1–1.1 times its radius. The specular
hemisphere moment is clamped to that directional interval; a relative lobe
bound or a diffuse-dominance bound controls its use. Lighting fully uses the
fitted Fresnel contribution within 2% lighting uncertainty and blends it into
sampled Schlick-Pow5 residual integration between 2% and 4%; the blend weight caps the
fitted moment's contribution to the same 2% budget. The constant-F0 GGX lobe,
diffuse cosine term and sheen remain analytic in these Fresnel fallback domains.
Diffuse cases outside their bound retain a sampled Fresnel residual as well.
This avoids the bright quadrature spokes that full-BRDF sampling can produce
around a broad, close emitter. Geometry and low-roughness fallback policies
still preserve the complete configured sampled response.
Cosine integration is skipped when the material has no diffuse or sheen energy.
Artistic bounce remains a separate sampled residual and is skipped at zero
bounce intensity.

The integrated solid angle already contains source area, source cosine and
inverse-square attenuation. Authored intensity remains emitted scene-linear
radiance; it is not multiplied by emitter area again. Finite range uses a
centre fade only when a conservative emitter-distance bound limits fade
variation to 1%, with a smooth transition from 0.5%; otherwise it selects sampled
quadrature. The legacy distance clamp within 0.1 world units, thin-surface
transmission and singular grazing views retain sampled integration. The near
transition spans distances 0.1–0.12 and the view transition spans `NdotV`
0.01–0.02. Transparent surfaces without those terms can use LTC with the
normal scalar raster visibility policy.

Extremely thin, distant or foreshortened emitters can lose float32 precision
through cancellation of polygon edge contributions. A conservative projected
angular scale uses the harmonic half-size, source-facing cosine and centre
distance. Values at or below `1e-4` retain the full sampled response, with a
smooth transition to LTC at `2e-4`. The independent tests cover both boundaries
and an intense 0.01-by-400-unit rectangle over 4,000 units from the receiver;
its full sampled fallback also preserves the existing ray-shadow MSAA checks.

LTC approximates the angular BRDF distribution; it is not ground-truth
transport. A finite disk polygon, table interpolation and a fitted single lobe
have measurable error. Numerical comparisons use the renderer's exact BRDF and
light conventions and report errors separately from the sampled baseline.
Existing shadow approximations retain their own documented limitations.

## Packaging and validation

The build stages `materials/ltc` beside the executable and the Windows package
requires both tables and their license notice. Files are validated before GPU
allocation. CPU cases cover headers, dimensions, length, channel order,
nonfinite values and invalid matrix/amplitude semantics. Emitter-overlay tests
check authored dimensions, orientation and disk radius; capture parser tests
check mode/sample changes and invalid inputs.

The package includes this guide and records SHA-256 hashes for its executable,
runtime DLLs, compiled shaders and LTC assets in `build-info.json`. The package
smoke test verifies those extracted files against the archive and its checksum,
then launches from an unrelated directory with SDK/build paths and Vulkan
overrides removed. It exercises both raster paths in LTC and sampled modes,
plus missing/corrupt table fallback in independent runtime copies. The original
extraction remains intact. Normal package runs disable optional validation;
the separate lighting regression enables synchronization validation.

```powershell
python -B tests/validation/ltc_package_smoke.py --runtime out/extracted/VulkanSceneRenderer-VERSION-windows-x64 --archive out/packages/VulkanSceneRenderer-VERSION-windows-x64.zip --output out/review-40/package-smoke
```

Runtime regression results and device-specific lighting timings are recorded
under `out/review-40`. Quality checks reconstruct scene-linear radiance from
unclipped PNG captures by inverting the known tone-map and exposure, accounting
for 8-bit quantization uncertainty. They compare that estimate against
converged independent area integration, with fixed cameras and material units.

Run from a Visual Studio Developer Console after building the Release app and
the focused test targets. The Python harness requires NumPy and Pillow; CTest
skips these optional cases if they are unavailable.

```powershell
ctest --test-dir out/build/visual-studio -C Release -R '^(ltc_lut_data_tests|submitted_upload_wait_tests|ltc_area_light_numeric_oracle|temporal_capture_tests|deferred_light_gizmo_planner_tests|rendering_convention_tests)$' --output-on-failure
$env:CONTAINER_RUN_GPU_LTC = '1'
python -B tests/validation/ltc_area_light_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --output out/review-40/runtime --case all
python -B tests/validation/material_layer_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --output out/review-40/material-layers
```

The GPU matrix checks rectangle/disk near-field, horizon, grazing, sidedness,
mirrored transforms, roughness, layers and transparency. It also checks
forward/deferred parity, raster/hard-ray/soft-ray visibility, missing/corrupt
assets in independent runtimes, and integration/quality changes with TAA enabled.
The repeatable benchmark measures Lighting and G-buffer cost at 1, 8, 32 and
128 emitters.
The material-layer regression isolates environment-only and direct-only
lighting, checks metallic iridescence and sampled extension textures, and
compares UV-set/transform selection and MSAA/resize transitions in both pipelines.
The upload-wait tests inject allocation errors and device loss, including a
second wait failure, to verify that submitted owners remain alive until the
queue is drained or the process stops without unwinding pending resources.
The spatial regression additionally checks a close disk's outer annulus at
9, 25 and 64 samples in both pipelines, including quadrature spokes and the
gaps between them. It reports centre-normalized and local errors separately.
Run that group with `--case spatial`. For a diffuse-material cost comparison,
use `--case benchmark` with
`--benchmark-material dielectric --benchmark-lights 32`.

### Repeatable disk profiling

Use a Release renderer and enable GPU tests with `CONTAINER_RUN_GPU_LTC=1`.
Use `--case correctness` or the opt-in GPU CTest target for the accuracy
suite alone. The benchmark commands assume a Ninja Release build at `$build`.
The benchmark first records independent timing processes with validation off,
then captures both modes with Khronos synchronization validation and checks
them against the same numerical reference and thresholds as the quality
suite. Configure `VK_LAYER_PATH` if the SDK's validation layer is outside the
Vulkan loader's normal search path.

```powershell
$env:CONTAINER_RUN_GPU_LTC = '1'
python -B tests/validation/ltc_area_light_regression.py `
  --case benchmark --exe "$build/VulkanSceneRenderer.exe" --output out/ltc-baseline `
  --benchmark-repeats 5 --benchmark-warmup 48 --benchmark-frames 8 `
  --benchmark-stride 4 --benchmark-order alternate --benchmark-seed 71
```

The default matrix uses both metallic and dielectric receivers for coarse
disks and rectangle controls at 1, 8, 32 and 128 coincident emitters. The
aggregate emitter radiance stays constant as light count changes. Additional
single-emitter cases exercise adaptive near-field and horizon clipping,
guarded material residual, and low-roughness sampled fallback. Use
`--benchmark-domain` to expand selected cases to every requested light
count. The older single-run numbers below use their original validation and
fixture settings.

`benchmark-results.json` records the actual order, executable and shader
hashes, LUT hashes, GPU/driver identity, capture settings, fixture hashes,
and raw retired GPU recordings. Each process reports its median and linearly
interpolated P95 for Lighting and G-buffer, plus the sum of the available
timed passes. The sum is descriptive because pass coverage can vary. The
renderer currently supplies pass timestamps. A full-frame timestamp is
reported when available. Samples use telemetry-only frames to
avoid image readback during the timing window. The recorded frame is the
observation frame and the recording serial identifies the previous GPU draw.
The report also records the capture path's serialized submission policy and
frames-in-flight setting.

Uncertainty is a descriptive 95% percentile bootstrap of the median of
independent process medians. The mode comparison resamples paired LTC /
sampled ratios. A comparison between builds resamples each build's process medians
independently. Three repeats are the minimum for a decision and five or more
are useful when results sit close to a threshold. Re-run with more repeats
if the interval is wide or crosses the threshold.

For #70, repeat the same command against the new Release executable and add
`--benchmark-baseline out/ltc-baseline/benchmark-results.json`. Select a
baseline for the same GPU, driver, Vulkan version and fixture settings.
Both builds must also use the same LTC table binaries.
The default 5% allowed increase applies to absolute LTC Lighting medians.
`--benchmark-threshold-percent` changes it. The comparison exits 1 when the
entire 95% interval exceeds the limit, 2 when the interval straddles it or
there are too few repeats, and 0 when the entire interval is within it.

For path diagnostics, install OpenCV with OpenEXR support, build the optional
`ltc_profile_shaders` target from
the same build and pass `--benchmark-profile-shaders "$build/ltc_profile_shaders"`.
The runner stages an isolated shader set and checks its paired hashes. The
diagnostic shader preserves RGB and writes overlapping per-pixel path flags
to the final scene-linear EXR. The report counts receiving pixels that took
each branch at least once across emitters or BRDF terms. A separate matched
LTC process pair measures the overhead before the EXR capture. The baseline
timings and regression gate use the ordinary shaders.

#### RTX 2080 SUPER measurements

The November 12, 2026 Release run measured three independent process pairs
per fixture with 48 warm-up frames and eight timing samples per process.
The ratios below use paired Lighting medians. Values above 1 indicate longer
LTC Lighting times. The brackets show the descriptive 95% intervals.

| Fixture | Emitters | Metallic LTC / sampled | Dielectric LTC / sampled |
| --- | ---: | ---: | ---: |
| Coarse disk | 1 | 1.088 [1.003, 1.213] | 1.177 [1.105, 1.179] |
| Coarse disk | 128 | 0.786 [0.785, 0.788] | 0.790 [0.782, 0.794] |
| Rectangle | 128 | 0.405 [0.403, 0.406] | 0.418 [0.415, 0.421] |
| Adaptive near field | 1 | 1.532 [1.522, 1.676] | 2.117 [2.088, 2.471] |
| Adaptive horizon | 1 | 1.276 [1.259, 1.476] | 2.115 [1.819, 2.149] |
| Material residual | 1 | 1.529 [1.430, 1.629] | 1.644 [1.508, 1.656] |
| Full sampled fallback | 1 | 1.025 [1.017, 1.034] | 1.012 [0.859, 1.166] |

The coarse disk and rectangle controls used their respective analytic paths
across all receiving pixels. The near and horizon cases subdivided the disk,
with horizon clipping on every receiving pixel in the tilted horizon case.
The material-residual case used coarse disk geometry and sampled residual
work at every receiver pixel. The fallback case used sampled integration
at every receiver pixel. At 128 emitters the diagnostic shader increased
the coarse disk Lighting median by 2.1–3.5% across the two materials in the
separate process pairs.

The full 24-fixture report includes 1,152 baseline timing samples, 48 matched
validation-enabled correctness captures, and the diagnostic path counts and
overhead estimates. The same-build repeat of the single metallic coarse disk
stayed within the default 5% regression threshold. Its change interval was
−6.76% to +3.22%. [Repeated benchmark results](../tests/visual-regression/ltc/windows-nvidia/repeated-benchmark-results.json)

### Disk integration optimization

The coarse disk path still integrates the same 32 ordered edges. When a
conservative height bound places the whole disk above both the receiver and
fitted horizons, it transforms and normalizes each vertex once and shares it
between adjacent edges. Disks that fail either height test go through the
existing ordered clipping path. Adaptive integration still uses the physical
and fitted arc-to-chord bounds with a relative tolerance of 0.002. Each
power-of-four increase above that tolerance adds two to the exponent of its
positive FP32 bit pattern. The shader uses that spacing to compute the first
valid subdivision level directly. The 512-subdivision limit and sampled
fallback still apply.

The `legacy-disk` benchmark domain reproduces the original metallic receiver
with roughness 0.6, disk radius 0.4, emitter distance 1.5, manual exposure 0.2
and 25 integration samples. The matched before/after run uses Release builds,
validation off for timing, 48 warm-up frames and eight timing samples per
process at a stride of four. Run five independent process pairs per fixture
with `--benchmark-domain legacy-disk rectangle --benchmark-material metallic
--benchmark-repeats 5 --benchmark-seed 70`. The rectangle controls use the
same receiver, light counts and capture policy. The report also records
G-buffer timing, the sum of timed passes, GPU driver and Vulkan versions,
binary hashes, and synchronization-validated reference captures.

The matched Release runs on the RTX 2080 SUPER used raw Vulkan driver version
`2559967232`. Lighting times are medians in milliseconds. The last column
compares the two LTC builds and includes the 95% bootstrap interval.

| Fixture | Emitters | Sampled after | LTC before | LTC after | LTC change [95% interval] |
| --- | ---: | ---: | ---: | ---: | ---: |
| Legacy disk | 1 | 1.740 | 1.382 | 1.227 | −11.21% [−29.70%, +4.95%] |
| Legacy disk | 8 | 5.535 | 5.224 | 4.213 | −19.34% [−19.64%, −19.07%] |
| Legacy disk | 32 | 20.737 | 18.918 | 14.876 | −21.36% [−23.09%, −20.49%] |
| Legacy disk | 128 | 81.718 | 73.951 | 57.694 | −21.98% [−22.39%, −20.98%] |
| Rectangle | 1 | 1.479 | 1.650 | 1.472 | −10.80% [−25.70%, +4.09%] |
| Rectangle | 8 | 5.520 | 3.017 | 2.996 | −0.68% [−1.64%, +0.04%] |
| Rectangle | 32 | 20.487 | 8.722 | 8.691 | −0.35% [−2.59%, +2.90%] |
| Rectangle | 128 | 80.821 | 32.786 | 32.632 | −0.47% [−1.34%, +1.08%] |

LTC was faster than sampled across the paired timing intervals for all four
optimized disk counts. The measured
build-to-build improvements at 8, 32 and 128 disks exceeded run-to-run
uncertainty. The one-disk interval and all rectangle intervals include zero.
At 128 disks, G-buffer stayed at 0.180 ms and the LTC timed-pass sum fell
from 74.506 to 58.247 ms. The renderer supplied pass timestamps only.
The matched pre-change build already had lower LTC medians than sampled under
this protocol. The historical 7–12% overhead was measured with validation on
and an earlier driver. The baseline path flags for the legacy disks showed
coarse work on all receiver pixels, adaptive work on 10.87%, subdivision on
4.48% and clipping on 0.33%. These counts overlap.

Shader-only experiments on the RTX 2080 SUPER kept the executable and fixture
fixed and interleaved three processes per shader variant. At 128 legacy disks,
the conservative coarse path reduced Lighting from 73.73 to 58.97 ms.
Specializing the edge-angle case as well took 88.40 ms. The direct subdivision
calculation alone improved the adaptive near and horizon fixtures by 4–5%.
Caching fitted endpoints and combining the clipping work each increased
Lighting time in those fixtures. The clipping variant's deferred SPIR-V was
15% smaller. Nsight could not collect hardware counters on this system, so
driver-level register use, occupancy and instruction throughput are unmeasured.

Four single-emitter dielectric fixtures checked the remaining disk paths.
The near-field fixture improved from 3.378 to 3.181 ms, and the material
residual fixture improved from 2.269 to 2.120 ms. Both still took more Lighting
time than sampled integration, as did the tilted horizon fixture. The path
flags show sampled residual work on every receiver pixel in the residual
fixture. The horizon and full sampled fallback medians also decreased, with
build-to-build intervals that crossed the 5% regression limit. The fallback
fixture's paired LTC-to-sampled ratio was 1.019 [0.925, 1.198].

The existing thresholds passed across 46 CPU oracle cases, 15 CPU disk-rim
cases, two angular-continuity checks and 232 GPU correctness records from
216 fresh Vulkan synchronization-validated captures. The independent
material-layer suite passed 67 records from 52 validation-clean captures.
The largest measured GPU disk-rim relative error was 0.10%. The maximum
close-disk annulus errors were 0.23% center-normalized and 0.47% locally
normalized.
[Disk optimization results](../tests/visual-regression/ltc/windows-nvidia/disk-optimization-results.json)

### Historical validation and timings

The October 8, 2026 validation run on an RTX 2080 SUPER passed 136 quality
comparisons and 34 absolute forward/deferred comparisons. The largest measured
full-LTC error against converged integration was 3.54%, within the fixed 6%
fixture threshold. The configured sampled equation and guarded LTC equation
matched captures within 0.41% and 0.45%, respectively. The parity comparisons
had no excess error after allowing for PNG precision. These values describe
the tested material/emitter domains and nine receiver points per quality
capture, rather than a bound for arbitrary scenes. Quality errors subtract the
quantization allowance and normalize by the largest reference channel among
those points; they are not per-pixel relative HDR error bounds. The full-LTC
reference and equation checks retain their unchanged 6% and 2.5% thresholds,
respectively.

The separate 73-point close-disk annulus passed all 9/25/64-sample comparisons
in both pipelines. Its worst measured LTC error was 0.70% normalized to centre
radiance, or 1.73% normalized to each point's reference. Policy-equation error
was below 0.24%, and all six spatial parity comparisons had no excess error.
The configured sampled baseline's centre-normalized errors were 141.76%,
94.69% and 38.42% at 9, 25 and 64 samples, respectively. This case motivates
keeping the analytic cosine and constant-F0 lobe during Fresnel fallback;
increasing coarse full-BRDF sample count alone did not resolve its spokes.

All 68 LTC quality images were generated with the final Fresnel-residual and
projected-angular conditioning shader. The two new thin-far explicit sampled
images were also fresh. The 66 explicit sampled-baseline images from the prior
shader version were reused and rechecked against the current equations and
telemetry.
Parity, spatial quality, performance and remaining runtime checks used fresh
captures. The harness defaults to generating the complete matrix; reuse
requires an explicit approved result manifest. Agreement at the central test
points alone is insufficient to reuse an image after a lighting change.

On the RTX 2080 SUPER, Release Lighting-pass medians at 960×540 were:

| Emitters | Rectangle sampled → LTC (ms) | Speedup | Disk sampled → LTC (ms) | Speedup |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1.193 → 0.777 | 1.53× | 0.997 → 1.210 | 0.82× |
| 8 | 4.579 → 2.307 | 1.98× | 4.670 → 4.258 | 1.10× |
| 32 | 17.500 → 7.817 | 2.24× | 17.876 → 16.059 | 1.11× |
| 128 | 68.647 → 29.209 | 2.35× | 69.367 → 62.342 | 1.11× |

These synthetic fixtures use metallic material at roughness 0.6, 25 configured
samples, validation enabled, zero bounce and no active shadows. Each value is
the median of eight GPU timestamps at frames 49–77. They measure the Lighting
pass, not total frame time. The single-disk fixture was 21.38% slower with LTC;
LTC does not improve every tested workload. Dense disk polygon integration has
a much smaller cost advantage than rectangle integration on this device. Other
material/view domains can trigger sampled residuals and incur different costs.
The two LUT image allocations total 131,072 bytes on this device.

With a dielectric material at the same roughness and 32 emitters, rectangle
Lighting cost was 17.469 → 7.802 ms (2.24×), and disk cost was
17.849 → 16.305 ms (1.09×). This separate capture uses the same resolution,
timestamp sampling, validation and disabled shadow/bounce settings.

These measurements and capture provenance are recorded in
`out/review-40/final-runtime-summary.json`; the separate dielectric timings are
in `out/review-40/dielectric-benchmark/results.json`.

The October 8 matrix passed all 220 primary records and 18 spatial records on
the final shader.
The shadow matrix's worst visibility-profile MAE was 0.00124 and P95 difference
0.00564; hard-ray comparisons had zero difference. Missing and corrupted LUT
runtimes selected sampled integration and matched its equation within 0.25%.
Their valid dummy image allocations totalled 1,024 bytes on this device.
Mode/sample changes reset TAA history, and the largest switched-image MAE was
0.000156. The original frame-57 capture retained the epoch while holding the
settings without a capture event; it did not test an explicit repeated-setting
event. Captures reported no Vulkan validation or synchronization hazards.

A focused October 9 rerun passed 16 runtime-switch checks in both pipelines.
It exercised mode-only and sample-only edits, followed by explicit mode-only,
sample-only and combined unchanged-setting events. Real edits reset GPU history
age to approximately 1; unchanged events retained epoch 8 and continued history
age to approximately 3, 5 and 9. The largest TAA comparison MAE was 0.000156,
with zero P95 difference. All six capture logs were validation-clean. These
results are recorded in `out/review-40/fixes/switch/results.json`.

The separate final ray-shadow MSAA run passed all 57 records with no validation
or synchronization hazards, retaining the existing thresholds. Hard receivers
had zero incorrectly lit or blocked pixels, and close soft-area receivers had
zero incorrectly lit pixels with a worst shadowed-interior visibility P95 of
0.00916.
Filtered tilted-receiver MAE at 1×, 2×, 4× and 8× MSAA was 0.01466, 0.01507,
0.01555 and 0.01589, respectively, below the unchanged 0.035 limit and at least
20% lower than unfiltered error. Moving-camera warm-history MAE was at most
0.01599 versus 0.04093 for the cold frame. See
`out/review-40/msaa/summary.json` and its validation-clean result records.

The October 9 review fixes passed a fresh Visual Studio Release build and all
nine focused CTest suites. The numeric oracle covered 46 quality cases and
15 disk-rim/transformed-horizon cases, including rejection when the subdivision
budget cannot establish the geometry bound. Upload wait fault-injection tests
covered successful completion, recoverable wait errors, device loss and failure
to establish completion before submitted owners can unwind. Capture parser
tests reject explicit empty and invalid lighting modes while allowing omission.

The final disk rerun passed 30 launches and 38 records against a fixed
executable/source/SPIR-V snapshot. Both raster paths passed the radius-10 and
radius-100 rim cases at polygon vertices and edge midpoints. Their largest
reference error after PNG quantization allowance was 0.0974%; selected LTC
quality cases were at most 0.8495%. All four near/horizon parity comparisons
had zero excess error, and all captures were validation-clean. These remain
fixture measurements rather than general HDR error bounds. See
`out/review-40/fix-disk-gpu-final/results.json` and its hash manifests.

With the same RTX 2080 SUPER benchmark settings described above, the reviewed
build measured these Lighting-pass medians:

| Emitters | Rectangle sampled → LTC (ms) | Speedup | Disk sampled → LTC (ms) | Speedup |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.9802 → 0.8446 | 1.16× | 1.0348 → 1.1383 | 0.91× |
| 32 | 16.5948 → 7.6569 | 2.17× | 16.8700 → 18.6925 | 0.90× |

A separate coarse kernel reduced dense disk LTC cost by 30.5% relative to the
first adaptive implementation, while retaining the same geometry acceptance
bound. LTC is still 10.8% slower than sampled integration in this 32-disk
fixture and 10.0% slower with one disk. Disk accuracy and rectangle speedups
therefore do not imply a performance win for every area-light workload.

The final material regression passed all 52 launches and 67 records, isolating
clearcoat/sheen environment lighting, metallic iridescence, six extension
textures, authored UV sets/transforms, and punctual-light texture response.
Forward/deferred parity had a largest mean absolute difference of 1.135 bytes
and P95 of 2 bytes, below the unchanged 3/7-byte thresholds. The smallest
authored layer response was 1.667 bytes, above the 0.75-byte response threshold.

Both raster paths also retained material layers through 1× → 4× MSAA, resize
to 400×300, and combined restoration to 1×/320×240. Their maximum mean-channel
drift was 0.00433 bytes, and the restored image matched the baseline. This test
exposed an uninitialized OIT occupancy counter during resource recreation;
initializing and flushing it before use prevents spurious enormous pool growth.
All 52 logs were free of Vulkan validation and synchronization hazards. The
executable and all recorded source/runtime shader hashes remained unchanged
through the run and matched the final disk executable. See
`out/review-40/fix-material-layers-final/summary.json`, `results.json`, and
`build-hashes.json`.

The subsequent October 9 completion matrix supersedes the older full-matrix
evidence: all 232 launches, 238 primary records and 18 spatial records passed,
producing 386 fresh PNGs without quality-image reuse. Start/end executable,
source, SPIR-V and LTC asset hashes matched. All 232 logs were free of validation
and synchronization hazards. The 140 quality records covered both integration
modes; geometrically eligible LTC cases had at most 3.542% reference error after
quantization allowance, below the unchanged 6% gate. All 36 raster parity
comparisons had zero excess error. The close-disk annulus improved to 0.227%
centre-normalized and 0.471% local reference error. Shadow visibility MAE/P95
remained at most 0.00124/0.00564, hard-ray differences were zero, missing/corrupt
asset fallback passed, and the largest TAA mode-switch MAE was 0.000156.

The full 960×540 Lighting-pass benchmark sweep measured:

| Emitters | Rectangle sampled → LTC (ms) | Speedup | Disk sampled → LTC (ms) | Speedup |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.973 → 0.821 | 1.18× | 0.995 → 1.066 | 0.93× |
| 8 | 4.388 → 2.338 | 1.88× | 4.456 → 4.982 | 0.89× |
| 32 | 16.628 → 7.728 | 2.15× | 16.933 → 18.624 | 0.91× |
| 128 | 64.559 → 29.004 | 2.23× | 66.669 → 71.999 | 0.93× |

These retain the metallic/roughness, exposure, visibility and timestamp
settings described above. Disk integration is 7–12% slower in this sweep;
performance remains device/workload dependent. The complete result records,
numeric checks, hash manifests, material-layer checks and eight untouched
comparison images are saved in
[the repository validation evidence](../tests/visual-regression/ltc/windows-nvidia/README.md).

Follow-up work is tracked in [#70: disk integration optimization](https://github.com/semihguresci/VulkanSceneRenderer/issues/70)
and [#71: profiling and performance regression checks](https://github.com/semihguresci/VulkanSceneRenderer/issues/71).

The clean package checkpoint is `d7eb6a25ae1170a405d9315e12a3980eb4fa17fd`.
Its Visual Studio Release rebuild passed all ten focused CPU suites, including
authored emitter-overlay geometry. The rebuilt executable's revision stamp
changed its hash; all recorded renderer sources and runtime SPIR-V files
remained identical to the full matrix snapshot. The extracted validation ZIP
passed eight LTC/sample/fallback launches and seven TAA, BIM/USD, MSAA and ray
shadow launches from unrelated working directories with Windows-only PATH and
SDK/Vulkan overrides removed. Its 78 shaders, eight DLLs and four LTC files
matched all 91 payload hashes before and after testing. The original extraction
was preserved. Package launches disable validation; the full matrix above
provides synchronization-validation coverage. This historical package predates
build identity in screenshot telemetry. Its provenance uses the clean build
log, source checkpoint, archive checksum and payload hashes.

This local test archive is `VulkanSceneRenderer-v0.1.0-ltc-validation-windows-x64.zip`
(77,108,924 bytes), SHA-256
`1a82936f4bb6a072d70f573cde1713b15d0fc444f7cdfc11656fbb939b84e422`.
Package/CPU result records and the source/SPIR-V equivalence check are saved
with the repository evidence. The archive is a local validation artifact;
GitHub publication and merge remain separate delivery steps.
