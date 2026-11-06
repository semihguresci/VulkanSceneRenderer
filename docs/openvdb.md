# OpenVDB

OpenVDB support covers scalar `.vdb` surfaces, standard point previews, a bounded
GPU density preview, and CPU voxel/SDF/point processing and smoke stepping.
It is optional and disabled by default.
The renderer retains its Vulkan 1.4 requirements; OpenVDB does not require CUDA.
The [feature completion ledger](openvdb-feature-status.md) tracks delivered,
partial and pending capability families. Full fluid simulation and spatial
multiple scattering remain open work.

## Build

Use the normal CMake preset with OpenVDB enabled:

```powershell
cmake --preset windows-release -DENABLE_OPENVDB=ON
cmake --build out/build/windows-release --config Release --target VulkanSceneRenderer openvdb_cpu_validation
ctest --test-dir out/build/windows-release --output-on-failure -R "^openvdb_" -LE requires-vulkan
```

CMake selects the `openvdb` vcpkg manifest feature before dependency
installation. With a separately installed OpenVDB, set `CMAKE_PREFIX_PATH` or
`OpenVDB_ROOT` to its installation prefix. Package-config installations and the
upstream `FindOpenVDB.cmake` installation layout are supported. An enabled
build fails configuration if the library is missing; a disabled build has
actionable runtime diagnostics for explicit VDB requests.

The Linux presets accept the same `-DENABLE_OPENVDB=ON` option. The
[OpenVDB build documentation](https://www.openvdb.org/documentation/doxygen/build.html)
and [vcpkg port](https://github.com/microsoft/vcpkg/tree/master/ports/openvdb)
describe the upstream dependencies. Native sparse rendering uses the portable
NanoVDB headers with Vulkan; it does not select the vcpkg CUDA feature. With
OpenVDB enabled, `ENABLE_NANOVDB` defaults to ON. CMake uses installed headers
or downloads pinned OpenVDB 12.0.1 headers. Set `NANOVDB_INCLUDE_DIR` to the
directory containing `nanovdb/PNanoVDB.h`, or disable the sparse path with
`-DENABLE_NANOVDB=OFF`. Dense volumes and the CPU tools remain available.

Portable native-file tools use the separate `ENABLE_NANOVDB_IO` option. It
defaults to the initial `ENABLE_OPENVDB` setting and can be enabled independently:

```powershell
cmake --preset windows-release -DENABLE_OPENVDB=OFF -DENABLE_NANOVDB=OFF -DENABLE_NANOVDB_IO=ON
```

This route needs NanoVDB headers and the normal application dependencies.
It does not require OpenVDB, TBB or CUDA. Available zlib and Blosc providers
enable ZIP/BLOSC; `ENABLE_NANOVDB_ZIP=OFF` and `ENABLE_NANOVDB_BLOSC=OFF` produce
a NONE-codec-only build. Runtime capability discovery reports each provider
independently. Conversion to/from `.vdb` additionally requires OpenVDB.

## Native NanoVDB file tools

```powershell
.\OpenVdbTool.exe --nano-capabilities
.\OpenVdbTool.exe --inspect-nano volume.nvdb
.\OpenVdbTool.exe --validate-nano volume.nvdb --require-full-checksum
.\OpenVdbTool.exe --to-nano density.vdb density.nvdb --codec zip --allow-float-map-rounding
.\OpenVdbTool.exe --from-nano density.nvdb restored.vdb
.\OpenVdbTool.exe --copy-nano volume.nvdb selected.nvdb --grid index:0 --raw
.\OpenVdbTool.exe --checksum-nano volume.nvdb rewritten.nvdb --checksum full
.\OpenVdbTool.exe --stats-nano volume.nvdb statistics.nvdb --native-statistics all
```

The API is in `OpenVdbNanoInterchange.h`. It accepts little-endian ABI32.6/32.7
raw groups and segmented/multisegment files with ordinary fixed-size,
breadth-first 5/4/3 Float, Double, Int32, Int64, Boolean, Mask, Vec3f and Vec3d
trees. Empty selection includes every grid. Duplicate native names require
`index:ordinal`; `name:literal` escapes a name that resembles an index selector.
VDB conversion uses the existing `descriptor:exact name` selectors. Unsupported
types remain visible in inspection and fail explicit conversion instead of
producing a partial archive. Point/index, quantized/half, UInt, Vec4, packed,
nonlinear and non-breadth-first representations remain outside this contract.

Catalog inspection checks encoded ranges and names before decompression. It
does not establish payload validity. Reading/validation independently checks
offsets, node ownership, masks, counts, finite stored values, map conditioning,
blind-data ranges and long-name termination before upstream tree/CRC traversal.
Compressed catalog inspection reports an unknown checksum mode as JSON `null`;
the mode becomes known after reading the native payload. Full, header-only and
disabled CRC32 are explicit. CRC32 provides corruption detection, and cannot
authenticate a file. Explicit checksum repair accepts an old mismatch only
after structural validation. It preserves raw/segmented form and per-grid
codecs, recomputes CRC, and leaves the source unchanged.

The full-mode flag alone does not establish complete coverage. The upstream
blocked convention leaves a tail smaller than 4 KiB unchecked; historic
ABI32.6 node-based checksums omit blind data. Validated reports expose
`checksumCoversAllData`, and `--require-full-checksum` requires both full mode
and complete coverage after the magic/checksum fields. Inspection reports
unknown coverage as JSON `null`. Both legitimate ABI32.6.0 tail conventions
are checked explicitly. Legacy output uses its official blocked-tail
convention; an empty tail becomes header-only because that header already
covers the entire buffer. Use `--raw` when handing legacy payloads back to
older SDKs that do not recognize the modern segmented-file magic.

Native copying retains tree, map and blind-payload bytes. Conversion preserves
stored inactive values/states, exact integers and signed zero. A versioned
`container:openvdb_metadata_v1` Unknown blind-data extension retains grid/file
metadata and vector-frame tags. Unregistered metadata types retain their raw
type tags and bytes. Loader-generated VDB statistics are recalculated on output.
Unknown third-party blind data, conflicting original file metadata and vector
frames without a preserved VDB extension require `--allow-metadata-loss`; every
omission/default is named. External vectors default to invariant local-space
only with that opt-in. Clearing a preserved half-storage flag to write exact
native values also requires the opt-in. Reverse output uses full-precision ZIP
without active-mask pruning.

The original Double affine coefficients remain exact, including small shear.
NanoVDB also stores derived Float map caches; any rounding there requires
`--allow-float-map-rounding`. Entirely inactive internal branches are recovered
from their checked parent ownership during VDB reconstruction. Reading and
copying an upstream native file retain its original bytes.

Statistics recomputation supports bounds, min/max, or full native caches. It
weights active tiles by their logical voxel counts and excludes inactive values
and background. Scalar integer extrema retain exact native types. Vector
extrema compare squared magnitudes; upstream vector averages and deviations
describe **squared magnitudes**, with native cache precision. Bool/Mask grids
have occupancy bounds and no numeric summary. Empty grids expose no extrema
or moments. Unsafe moment accumulation and vector squared-magnitude ranges
fail before publication; bounds-only processing remains available. Active
regions ending at `INT32_MAX` require recentering to avoid the upstream
exclusive-bound calculation. Private raw intermediate passes retain values,
activity, maps and blind bytes, and share the aggregate work/workspace budget.
Intermediate encoded bytes use the native-byte cap; user inputs/outputs use
the encoded-file cap.

All outputs must be new files. Native filesystem snapshots/publication support
Unicode filenames and parent directories. Limits apply to encoded/native bytes,
combined workspace, active voxels, stored leaf slots, nodes including roots,
work, grids, name bytes and aggregate preserved metadata. Use the dedicated
`--max-input-bytes`, `--max-native-bytes`, `--max-workspace-bytes`,
`--max-active-voxels`, `--max-stored-voxels`, `--max-nodes`, `--max-nano-work`,
`--max-grids`, `--max-name-bytes` and `--max-metadata-bytes` options. These are
conservative logical reserves; provider internals, worker stacks and process
RSS are outside their scope.

Seven authoring-panel operations expose catalog inspection, validation,
copy/re-encoding, both conversion directions, checksum rewriting and statistics. They use
the existing asynchronous immutable job snapshots and display the capability
and loss diagnostics. Native outputs can be converted to VDB before using the
existing preview controls. The primitive authoring operation below extends these tools. Device authoring
and wider format and validity-report coverage remain outstanding.

The coordinated native-file/statistics batch passes 61 enabled CPU, CLI, UI,
configuration, shader and runtime-package groups, plus 56 disabled groups.
The full build includes 28 interchange and 12 statistics cases. Independent
native-only builds pass 22 interchange and 11 statistics cases with providers
enabled or disabled; their seven CLI groups include one conversion-only skip.
The rebuilt application also passes all 20 transport/image/upload GPU cases,
without GPU skips or VUIDs. Artifacts are in
`out/openvdb-validation/nano-integrity-{cpu,disabled,gpu}.log` and
`out/openvdb-review/nano-standalone/`.

Ten raw/segmented fixtures generated with the official OpenVDB 11.0.0 NanoVDB
headers validate as ABI32.6.0. Ten raw copy/statistics outputs independently
read back with that SDK. Provenance, archive digest and results are retained
in `out/openvdb-review/nano-legacy-sdk/`. This check caught the legacy blocked
checksum convention that the current upstream compatibility path misses.
The legacy headers are validation inputs and are not linked or shipped with
the application.

## Native NanoVDB primitive authoring

`WriteNanoVdbPrimitive` in `OpenVdbNanoPrimitives.h` and `--nano-primitive`
create a CPU sphere, axis-aligned box or WORLD-Y torus as a Float/Double narrow
band or fog grid. Native I/O alone is required; OpenVDB, TBB and CUDA are
optional for this route. The full native-I/O-only Tool has been built and
validated independently of OpenVDB and TBB.

```powershell
OpenVdbTool --nano-primitive sphere sphere.nvdb --nano-storage double
OpenVdbTool --nano-primitive box fog.nvdb --nano-class fog --box-dimensions 2 1 3
OpenVdbTool --nano-primitive torus torus.nvdb --major-radius 1.5 --minor-radius .5 --raw
OpenVdbTool --nano-primitive sphere fine.nvdb --voxel-size .1 --allow-float-map-rounding
```

Defaults are radius1, full box dimensions2/2/2, torus major1/minor.25,
voxel size.125, half width3 voxels, center/origin0, Float level set and all
native statistics. Shape-specific options reject when used with another shape,
even if their values equal defaults. `--center X Y Z` places the primitive in
WORLD units; `--origin X Y Z` sets the uniform grid map origin. Positive voxel
size and dimensions/radii must be in[1e-6,1e6], band width in[1,64], and sphere
radius/box half dimensions/torus minor radius must resolve at least1.5 voxels.
Torus major radius must exceed minor radius. General rotation/shear is outside
this authoring API.

Native Double geometry construction precedes Float/Double serialization, so
Float storage does not prematurely narrow shape parameters. Every published
stored value, inactive tile and background participates in the Float rounding
audit; nonzero-to-zero and nonfinite conversion reject. SDF values use WORLD
distance and symmetric inactive band continuation. Fog is the native interior
linear ramp `min(1,-phi/(halfWidth*voxelSize))` with zero exterior, including
absent root tiles; it is not a mass-conservative rasterization.

The Double map remains exact. `--allow-float-map-rounding` explicitly permits
rounded native Float forward/inverse caches only when the stored-coefficient
discrepancy over the candidate domain is at most1e-4 voxel. The report separates
that discrepancy from Float VALUE rounding and from subsequent Float arithmetic.
Default map coefficients are exactly representable.

`--grid-name`, `--native-statistics bounds|minmax|all`, `--codec none|zip|blosc`,
`--checksum disabled|header|full` and `--raw` expose native output policies.
Raw output requires codec none. The native-file byte/work/node/voxel/name/grid/
metadata limits listed above apply jointly to construction, native buffers,
validation, private raw staging and final checked writing. CLI/UI report memory
and formatting work are deducted before the API. Reports include candidate and
node bounds, Float errors, native/encoded sizes, CRC coverage and truncation.
The pipeline uses one TBB worker when configured; the SDK std-thread fallback
may parallelize. Logical caps exclude provider internals and worker stacks.

The authoring panel provides the same source-free operation and immutable
settings snapshots. It produces a new `.nvdb` file without an automatic model
load; use the native conversion route and its metadata policies before VDB
surface preview. Conversion additionally requires OpenVDB. Arbitrary VDB
metadata extensions, quantized/index/point authoring and CUDA/device tools
remain separate work.

## Surface import

```powershell
.\VulkanSceneRenderer.exe --model C:\Volumes\surface.vdb
```

`.vdb` follows the same auxiliary scene route as USD and IFC. The scene model
menu discovers files placed in `models/OpenVDB` when OpenVDB support is built.
Each supported grid becomes a selectable object with grid metadata, bounds,
materials and triangle geometry, so the existing raster, shadow, picking and
scene inspection paths can display it.

FloatGrid and DoubleGrid surfaces are supported. The default isosurface is zero for
level sets and 0.5 for fog and other scalar fields. Grid transforms and a
positive import scale are baked into positions. Standard PointDataGrid inputs
also load as physical-radius octahedral mesh markers, colored by signed velocity
direction. Zero velocity is neutral gray. These markers use the normal mesh
rendering, selection and material paths; global point-style controls do not
change their size. `ImportOptions::pointPreview` selects uniform, signed-direction
or magnitude colors and explicit point/geometry budgets. Positions and radii
use source world units multiplied by import scale; velocity color units remain
source world units per second. The strict P/v/pscale layout below is required.
All stored records, including records in inactive voxels, are displayed.
Empty point grids produce diagnostics. Vector and integer grids are skipped;
a file without displayable scalar or standard point geometry fails clearly.
Set `ImportOptions::includePoints=false` for scalar-only import. OBJ surface
export always uses that setting and never exports point markers as surfaces.

Vector previews are an explicit model import mode:

```powershell
.\VulkanSceneRenderer.exe --model wind.vdb --vdb-model-grid velocity --vdb-vector-mode glyphs --vdb-vector-bounds -1 -1 -1 1 1 1 --vdb-vector-seeds 4 4 4 --vdb-vector-seconds 0.1 --vdb-preview-radius 0.01
.\VulkanSceneRenderer.exe --model smoke.vdb --vdb-model-grid velocity --vdb-vector-mode streamlines --vdb-vector-domain fluid --vdb-vector-step 0.05 --vdb-vector-steps 128
```

`ImportOptions::vectorPreview` selects only one Vec3S/Vec3D field. World-space
collocated fields support conditioned affine shear/reflection. Explicitly tagged
native MAC uses negative-face component sampling followed by the grid Jacobian,
so displayed vectors have world units. Bounds, seed positions, glyph/tube radius
and lengths use source world units before positive import scale. A nonzero
background or empty active extent requires explicit bounds. The seed product is
at most 4096. Glyph displacement is velocity times display seconds; a length cap
reports shortening. Streamlines use normalized world-vector RK4 and a
Jacobian-limited arc-length step; they describe the current field, not evolving
particle trajectories. Optional same-file fluid masks clip walls and exclude
stencil leakage. Zero/stagnant values, clipping, return-to-seed cycles and step
exhaustion are reported in object metadata. Work, topology, geometry and Float
display-precision limits fail explicitly. These model paths render ordinary mesh
arrows/tubes through existing raster, picking and shadow facilities.

`--vdb-preview-color uniform|direction|magnitude`, `--vdb-preview-tint R G B`
and `--vdb-preview-speed` configure point/vector colors. `--vdb-vector-length`,
`--vdb-preview-max-triangles` and `--vdb-preview-max-work` set explicit preview
limits. **Field preview controls** in OpenVDB tools expose the same settings.
Clicking **Load field preview** snapshots them into asynchronous model loading.
Same-source reloads and failed-load rollback retain the active import settings.

Scalar/vector slices and typed native points have separate explicit import modes:

```powershell
.\VulkanSceneRenderer.exe --model smoke.vdb --vdb-model-grid density --vdb-slice --vdb-slice-origin 0 0 0 --vdb-slice-extent 2 2 --vdb-slice-samples 64 64 --vdb-slice-range 0 1 --vdb-slice-opacity 0.5
.\VulkanSceneRenderer.exe --model ids.vdb --vdb-model-grid id --vdb-slice --vdb-slice-integer-range 9007199254740992 9007199254740993
.\VulkanSceneRenderer.exe --model particles.vdb --vdb-model-grid particles --vdb-native-points --vdb-native-radius-attribute width --vdb-native-color scalar --vdb-native-color-attribute id --vdb-native-any-group selected
```

`--vdb-slice-u` and `--vdb-slice-v` set independently normalized source-world
plane axes. Extents are full side lengths; the explicit sample rectangle includes
its edges and may extend into background values. Float/Double scalar slices use
`--vdb-slice-value scalar`; world-vector or explicitly tagged native MAC slices
use `x|y|z|magnitude`. Scalar SDF values remain signed distances. The color map
is `gray|blue-red|diverging`, with an explicit finite range and reported clipping.
The two-sided unlit mesh has one material opacity, with no source-grid edits.

Bool/Mask/Int32/Int64 UNKNOWN scalar slices use nearest native cell centers,
with half ties toward the greater index. Int32/Int64 default to the exact
observed sample range; constant automatic ranges use the middle color.
Bool/Mask default to [0,1], and Mask samples its native activity bit. Optional
`--vdb-slice-integer-range MIN MAX` supplies strictly increasing signed 64-bit
bounds. Integer subtraction precedes floating color normalization, so adjacent
Int64 endpoints above 2^53 do not first collapse to the same Double value.
Raw extrema remain exact text. Floating range controls must remain at their
defaults for integer scalars; exact integer ranges reject on floating/vector
sources. The CLI rejects combining floating/integer range flags before GPU
startup. Collocated world Vec3I UNKNOWN fields support nearest-sampled vector
components/magnitude with the existing floating color range.

Collocated world vectors retain all five native vector semantic tags: invariant,
covariant, normalized covariant, relative contravariant and absolute
contravariant. Slice values are their stored world components; the sampler
does not apply the grid Jacobian, add the map translation or normalize them.
The returned model properties record the original tag and this sampling rule.
Index-space vectors require the separate explicit MAC route where supported.

Floating and native MAC slice sampling remain unchanged. Shared encoded,
scanned/file metadata and full typed leaf-buffer limits precede paging;
Unicode inputs use owned snapshots. Eighteen native feature cases and the
disabled availability case pass, including affine, reflected and sheared maps
for all five world-vector tags. Exact integer range controls have rebuilt
application and eight-capture proof recorded below. The additional semantic
properties preserve the existing sampling calculation.

Native point previews accept registered fixed codecs and group selections,
including inactive-voxel records. Choose a positive named scalar radius or
`--vdb-native-radius`, then an optional `--vdb-native-radius-scale`. Colors are
`uniform|scalar|direction|string`; nonuniform modes require a named attribute.
Scalar auto-ranging distinguishes adjacent Int64 values, vector colors describe
stored component direction, and string colors use a stable 12-color palette
with possible category collisions. `--vdb-native-opacity` sets material alpha.
No attribute name is assumed automatically. Unsupported selected strides/codecs
and insufficient Float geometry precision fail explicitly.

Vector, slice and native-point modes are mutually exclusive. Mode-specific flags
require their explicit mode. Shared `--vdb-preview-max-triangles`,
`--vdb-preview-max-voxels` and `--vdb-preview-max-bytes` bound geometry/source
workspaces; slice sampling/work and native point/schema/group/string limits have
separate controls. The authoring panel offers all five preview kinds and snapshots
their settings before asynchronous loading. See `OpenVdbSlicePreview.h` and
`OpenVdbNativePointPreview.h` for the precise map, codec and budget contracts.

The C++ API in `OpenVdbLoader.h` also exposes grid selection, an explicit
isovalue, adaptivity, and input-memory/voxel/output-triangle budgets. Defaults are
512 MiB encoded input and recorded/loaded grid memory, 8 million active or stored
leaf voxels per grid, and 2 million triangles in total. Input checks precede
extraction. OpenVDB creates its temporary meshing buffers
before the output-triangle check, so the triangle budget is not a strict peak
memory bound. Surface import and density rendering are separate workflows.
Link `VulkanSceneRenderer_openvdb` for the import and density-loading APIs;
its public headers do not expose OpenVDB types.

For duplicate display names, select an exact file descriptor with
`descriptor:<name>` (for example, `--vdb-grid 'descriptor:density[0]'` or
`--input-grid 'descriptor:density[1]'`). Use the exact descriptor shown in the
imported grid's metadata; OpenVDB may append bracketed indices to duplicate
names. Plain duplicate display names are rejected as ambiguous.

## Density volumes

```powershell
.\VulkanSceneRenderer.exe --vdb-volume C:\Volumes\smoke.vdb --vdb-grid density --vdb-density 2 --vdb-resolution 128
```

The default density path reads FloatGrid or DoubleGrid data and resamples its
world bounds into bounded 3D textures. Up to four media share one ray marcher,
which combines extinction, scattering and emission before compositing into HDR
scene color and clips rays against opaque scene depth and active scene section
planes/boxes. Directional and environment lighting, scattering tint, phase
anisotropy and emission are supported. `--vdb-density` controls extinction and
`--vdb-resolution` sets the longest texture axis, from 16 to 256. The default
resolution is 128. A 256 cubed R32 texture occupies 64 MiB before staging and
other resources; an optional temperature channel doubles that texture storage.
The source `.vdb` remains sparse and is not modified.
With only `--vdb-volume`, the viewer frames the volume and omits the default
scene. Supply `--model` or `--bim-model` to view geometry with the medium.
Explicit camera options override automatic framing. `--vdb-volume-scale`
controls the volume's positive world scale independently of mesh import.
Input defaults cap the source at 32 Mi active voxels and 512 MiB on disk and
in recorded/loaded grid memory; stored leaf topology is also bounded. Density
and an optional temperature grid share these source budgets.

The Scene Controls panel exposes a loaded volume's visibility, density,
scattering tint, anisotropy, multiple scattering, emission and ray-step quality. The same controls
are available on the command line:

```powershell
.\VulkanSceneRenderer.exe --vdb-volume smoke.vdb --vdb-scattering 0.7 0.8 1 --vdb-anisotropy 0.4 --vdb-step-scale 0.5
.\VulkanSceneRenderer.exe --vdb-volume smoke.vdb --vdb-multiple-scattering 1 --vdb-scattering-orders 4
.\VulkanSceneRenderer.exe --vdb-volume fire.vdb --vdb-grid density --vdb-temperature-grid temperature --vdb-temperature-scale 1000 --vdb-emission-scale 2
```

Scattering tint is an RGB albedo in [0,1]. Anisotropy ranges from -0.95 to 0.95;
positive values favor forward scattering. Emission defaults to zero and uses
`--vdb-emission R G B` as its color multiplier. Without a temperature grid this
is a fixed color. An explicit scalar temperature grid supplies thermal color;
`--vdb-temperature-scale` converts simulation values into Kelvin. Temperature
and density may use different affine transforms. A step scale below 1 improves
detail at greater GPU cost; values from 0.25 to 4 are accepted.

Multiple scattering is an optional local light-path return approximation.
`--vdb-multiple-scattering` sets its strength from 0 to 1; the default 0 preserves
single scattering. `--vdb-scattering-orders` adds 1 to 4 isotropic orders, with 2
as the default. It uses each existing light sample's combined-medium
transmittance `T` and the mixture's RGB albedo `a`. The return proxy is
`q = a * (1 - T)`, and the added gain is `strength * (q + ... + q^N)`.
Since `0 <= a <= 1`, the local path factor `T * (1 + q + ... + q^N)` is at most
one in each channel. This bound applies to the closure's light-path gain.
The approximation adds no density or shadow rays, keeps the direct anisotropic
term, and leaves ambient lighting and emission unchanged. It cannot simulate
spatial diffusion between regions, light routing around occluders or indirect
illumination from fire emission. It supports directional, point, spot and area
lights, including their existing geometry-shadow visibility.

The preview also uses the first 64 scene point/spot lights, including their
range and spot cones. Directional and local lights use combined density
self-shadowing across all media. Opaque geometry casts shadows into the medium
using the scene's cascaded and local raster shadow maps, including when ray
queries are disabled. `--no-vdb-shadows` disables geometry shadow reception;
medium self-extinction remains active. Temperature color is a
white-balanced three-band Planck approximation with independent emission
brightness, rather than a radiometrically calibrated fire simulation.

The first eight scene rectangle/disk area lights also illuminate the medium.
Four deterministic samples per emitter account for its area, orientation,
range, radiance and the medium's phase function. Each sample includes combined
density self-extinction. Geometry visibility uses the scene's available area
shadow-map origins, so it is a bounded approximation of a continuous soft
shadow. Area emitters use the same units and shape controls as surface lighting;
they do not require separate VDB light settings.

The volume pass depth-sorts the renderer's existing transparent fragment lists
and integrates the medium between surface events. Alpha glass in front of,
inside or behind fog therefore receives the appropriate camera-ray attenuation.
The surface model retains its existing environment-based transmission and
refraction preview; the volume pass follows the straight camera ray. It retains
the existing limit of 64 transparent fragments per pixel and global node-pool
capacity. Pixels composed by the volume pass consume their fragment list so
later temporal/post passes cannot blend the same surfaces twice. Overview
diagnostic panels retain the original lists. Transparent-layer processing adds
at most 64 boundaries to the existing primary march budget of 768 dense steps
or 4,096 native steps; it preserves the primary sample positions.
The C++ `record()` call takes a `transparentOitActive` argument. Set it only
when the current frame's clear and resolve passes are active; both built-in
techniques supply that state. Standalone callers default to retaining the
original transparency lists.

Use a scalar fog/density grid for smoke, fog or clouds. Signed-distance grids
must first be converted with `OpenVdbTool --fog`. Use the surface importer for
selection and mesh-based shadow/physics workflows.
The compute stage uses eight storage buffers, two storage images, six uniform
buffers, eleven sampled images and one sampler. Incompatible devices produce an
explicit capability diagnostic.
TAA rejects history for pixels with a visible volume contribution or consumed
transparent fragments. The preview does not supply volume motion vectors or a single surface depth;
uncovered geometry and background retain normal temporal accumulation.

### Overlapping media and native sparse rendering

Repeat `--vdb-volume` to load up to four files. Their native world transforms
position each medium; the global volume controls apply to all sources. The C++
renderer also exposes per-medium setting overrides. Overlapping densities are
added inside one integration step, so changing file order does not change the
compositing order.

```powershell
.\VulkanSceneRenderer.exe --vdb-volume smoke_a.vdb --vdb-volume smoke_b.vdb
.\VulkanSceneRenderer.exe --vdb-volume detailed_smoke.vdb --vdb-sparse
```

`--vdb-sparse` converts the selected scalar trees into NanoVDB Float32 buffers
and traverses them directly in the compute shader. It preserves the native
voxel lattice and independent density/temperature transforms; `--vdb-resolution`
only controls the dense preview. Empty constant tiles are skipped conservatively,
including an interpolation halo. Stored leaves and positive inactive values
are never treated as empty. Native stepping uses the physical voxel size with
a 4096-iteration bound; dense previews retain their 768-step bound. Extremely
long or dense rays can still exhaust the iteration budget.

Converted values are clamped to [0,1e6], and native buffers must fit both the
source byte budget and the device's storage-buffer range. The GPU performs
checked buffer reads. Conversion allocates temporary buffers before the final
serialized-size check, so these limits do not form a strict peak-memory sandbox.

### Animated sequences

```powershell
.\VulkanSceneRenderer.exe --vdb-sequence C:\Volumes\smoke_frames --vdb-fps 24 --vdb-no-loop
.\VulkanSceneRenderer.exe --vdb-sequence C:\Volumes\fire_frames --vdb-sparse --vdb-temperature-grid temperature --vdb-emission-scale 2
```

Directory discovery selects only regular `.vdb` files in that directory and
sorts numeric filename runs naturally (`smoke2` precedes `smoke10`). Keep SDF
construction inputs outside the frame directory. At most 10,000 frames are
discovered. The sequence occupies the first medium slot; up to three static
`--vdb-volume` sources can accompany it.

Playback supports FPS, `--vdb-speed`, `--vdb-paused`, `--vdb-start-frame` and
looping/hold behavior. The Scene Controls panel provides pause, loop, speed and
frame scrubbing. `--vdb-cache-mib` controls the bounded CPU LRU (512 MiB by
default, at most two retained frames); conservative pre-eviction can retain
fewer frames for large or native sources. Each frame must keep the selected
grid names and channel presence, but its bounds, transform and dimensions may
change. Invalid frames fail with their index and path.

The renderer retires graphics readers before publishing the animated medium's
resources, retains its pipelines, and preserves static media. The default path
loads and uploads synchronously and can stall on large frames. Automatic
camera framing uses the initial frame and static sources. Use an explicit
camera for moving sequences, or the C++ cache's streaming `scanUnionBounds()`
when preparing a conservative view. No full sequence is loaded into memory.

Pass `--vdb-async` to load subsequent frames on a CPU worker, prefetch the
next frame, and stage GPU replacements without waiting for each upload. The
initial frame loads synchronously for startup and camera
framing. While a requested frame is pending, the viewer keeps rendering the
last uploaded frame and holds the playback clock; camera and UI input remain
active. The volume panel shows the requested and displayed frame indices.
Scrubbing supersedes older requests, and a failed requested frame reports its
index and path. Synchronous loading remains the default for deterministic
captures. When captures explicitly use `--vdb-async`, each fixed timestep waits
for its requested frame with a 30-second deadline, and capture telemetry records
both requested and displayed indices. Unpaced render ticks do not stand in for
the worker's wall-clock loading time.

The worker bounds cached, ready, caller-held and in-flight frame payloads
together. CPU leases count against the byte budget until the caller releases
them, and exhausted budgets wait for a lease instead of allocating unbounded
frames. Source-grid/conversion scratch allocations remain subject to the
loader's separate limits. One pending GPU candidate owns its staging bytes,
images/native buffers, commands and completion fence. A superseded candidate is
discarded after its transfer completes. Density and temperature travel together;
static media keep their resources while only the animated slot is replaced.

GPU submission and readiness polling do not wait for the queue or device to
become idle. Publication updates descriptors after graphics readers retire.
Validation, allocation and the CPU staging copy still run on the application
thread, and transfers share the graphics queue with rendering. This removes
explicit upload waits without promising independent copy/render execution.
Startup and the synchronous C++ replacement APIs retain their blocking behavior.
Shutdown joins an active CPU load and waits for an outstanding upload before
releasing its resources.

C++ callers can move a warmed `OpenVdbVolumeSequence` into
`OpenVdbAsyncVolumeSequence` without copying its voxel arrays. Request and poll
from the application thread; the worker owns all subsequent loading. For
example, with an application-provided `uploadVolume` function:

```cpp
using namespace container::geometry;
auto warmed = std::make_unique<OpenVdbVolumeSequence>(
    manifest, volumeOptions, cacheOptions);
uploadVolume(warmed->frame(0));
OpenVdbAsyncVolumeSequence sequence(std::move(warmed));

const auto requestId = sequence.request(targetFrame);
// On subsequent render iterations, poll without blocking:
if (auto frame = sequence.tryTakeFrame()) {
    if (!frame->error.empty()) throw std::runtime_error(frame->error);
    uploadVolume(*frame->volume);
    const auto displayedFrame = frame->frameIndex;
    frame.reset(); // Release the CPU lease after upload.
    sequence.prefetch((displayedFrame + 1) % sequence.manifest().frames.size());
}
```

`request()` supersedes an older target and deduplicates repeated pending
requests. Each completion includes its frame index and request ID; a failed
load has an explicit error instead of a volume. `status()` exposes loading,
memory-wait, resident-byte and prefetch-error information. A failed prefetch
is latched to avoid retrying every render iteration and becomes an explicit
completion if that frame is requested. Keep shared frame leases only while
needed, and schedule the next prefetch after releasing the uploaded frame.

`OpenVdbVolumeRenderer::beginReplaceVolume()` copies a frame into owned staging
and returns false if one replacement is already pending. The source CPU lease
can be released after a successful begin. `volumeReplacementReady()` polls its
fence; `publishVolumeReplacement()` swaps the medium only when ready. The caller
must retire every graphics reader before publication because all frame
descriptor sets reference the media. `discardVolumeReplacement()` drops a ready
candidate without changing the displayed medium. These operations keep the
pipelines and the renderer's private upload command pool across swapchain/MSAA
resource changes.

## Voxel and SDF processing

`OpenVdbTool` runs on the CPU without opening a Vulkan window. It creates new
files and refuses to overwrite existing outputs. Run `OpenVdbTool --help` for
all budgets, grid selection and generation options.

```powershell
# Produce two signed-distance fields with matching voxel transforms.
.\OpenVdbTool.exe --sphere sphere.vdb --radius 1 --voxel-size 0.05
.\OpenVdbTool.exe --sphere offset.vdb --radius 0.7 --center 0.7 0 0 --voxel-size 0.05

# SDF booleans preserve a sparse level-set representation.
.\OpenVdbTool.exe --union sphere.vdb offset.vdb union.vdb
.\OpenVdbTool.exe --intersection sphere.vdb offset.vdb intersection.vdb
.\OpenVdbTool.exe --difference sphere.vdb offset.vdb difference.vdb

# Convert a closed imported mesh into a signed-distance field.
.\OpenVdbTool.exe --mesh closed_model.usda model_sdf.vdb --voxel-size 0.05
.\OpenVdbTool.exe --mesh closed_model.glb gltf_sdf.vdb --voxel-size 0.05

# Rebuild mismatched SDFs onto the same world transform and band width.
.\OpenVdbTool.exe --resample sphere.vdb sphere_matched.vdb --voxel-size 0.1 --half-width 3
.\OpenVdbTool.exe --sample sphere_matched.vdb 0 0 0

# Edit a FloatGrid level set using its existing world transform and band.
.\OpenVdbTool.exe --offset sphere.vdb expanded.vdb --distance 0.2
.\OpenVdbTool.exe --offset sphere.vdb contracted.vdb --distance -0.2
.\OpenVdbTool.exe --smooth sphere.vdb smooth.vdb --filter gaussian --width 1 --iterations 2

# Convert an SDF into [0,1] fog density and render it.
.\OpenVdbTool.exe --fog union.vdb cloud.vdb --grid-name density
.\VulkanSceneRenderer.exe --vdb-volume cloud.vdb --camera-position 0 0 5 --camera-target 0 0 0
```

Mesh voxelization reuses the `.bim`, `.ifc`, `.ifcx`, USD-family and glTF/GLB importers
and applies element transforms. glTF uses static instances in the active scene,
including nested parent transforms; texture images are not decoded. Bake skins
and morph targets before voxelization. Incomplete imports and non-mesh objects
are rejected so a successful output cannot silently omit them. Meshes must be
closed triangle surfaces; open/nonmanifold meshes cannot define a reliable inside.
The C++ processing API also accepts a
shared `dotbim::Model` directly and exposes nearest-voxel world-space scalar
sampling for SDF/occupancy queries.
Link `VulkanSceneRenderer_openvdb_processing` when using these processing APIs.

Open surfaces use the separate unsigned conversion route:

```powershell
.\OpenVdbTool.exe --mesh-distance open_model.glb distance.vdb --voxel-size 0.05 --half-width 3 --triangle-id-name closest
.\OpenVdbTool.exe --inspect distance.vdb
```

`WriteMeshDistance` accepts Double world positions/triangle indices or a scene
model, and `VoxelizeModelDistance` loads the scene formats above. The Float
distance grid has `GRID_UNKNOWN`, nonnegative world distances and a positive
band background. It is not a signed level set. An optional native Int32 grid
shares its exact active topology, with zero-based source triangle IDs and -1
background. Model metadata records `[elementIndex, meshId, rangeTriangleIndex]`
provenance. Exact distance ties prefer the lower ID; canonically ordered corners
make permutation duplicates compute the same distance. Other near ties retain
OpenVDB floating-point arithmetic. No invalid triangles are silently omitted.
`--no-triangle-ids` omits the companion. Generation recenters Double coordinates
before meshing and restores the world map. The glTF wrapper rejects instance
transform narrowing exceeding 1e-4 voxel; recenter/bake the asset or use the
Double-position API when the scene's Float transform cannot retain placement.

`WritePlatonicLevelSet` and `--platonic` add tetrahedra, cubes, octahedra,
dodecahedra and icosahedra:

```powershell
.\OpenVdbTool.exe --platonic cube.vdb --faces 6 --scale 1 --center 0 0 0 --voxel-size 0.05 --half-width 3
.\OpenVdbTool.exe --platonic ico.vdb --faces 20 --scale 1 --voxel-size 0.05
```

Face counts are 4, 6, 8, 12 or 20. Scale is the cube side length; the other
solids use OpenVDB's approximately unit-circumradius canonical meshes. Distances,
center and voxel size use world units. The supported voxel range is
[1.5e-5,1e6], scale [1e-6,1e6], with at least five voxels per scale and a
3..64-voxel band. Conservative padded tree/workspace limits precede generation.

`MeasureLevelSet` / `--measure` report area, enclosed volume, average mean
curvature, Gaussian curvature integral and a numerical Euler estimate:

```powershell
.\OpenVdbTool.exe --measure sphere.vdb --input-grid surface
```

This accepts Float/Double signed-distance level sets with a symmetric 3..64
voxel inactive band, uniform orthogonal affine voxels and a resolved zero
crossing. Active tiles and invalid/incomplete distance bands fail. Integer
recentering and Double voxel-unit integration avoid remote-index cancellation.
Area/volume/mean curvature use world squared/cubed/inverse-length units. Euler
is a resolution-dependent curvature integral divided by `2*pi`, with raw value,
rounded estimate, rounding error and a refinement diagnostic. It is not an exact
topology test, and genus is not inferred for disconnected surfaces. Source bytes
remain unchanged; padded integration-box and workspace caps precede allocation.

Boolean inputs must be FloatGrid level sets with identical transforms and
matching band widths. Resample both mismatched inputs before combining them.
`--resample` rebuilds a level set from its zero surface at the requested world
voxel size and half width, or uses trilinear interpolation for fog. OpenVDB's
level-set rebuild can remove interior bubbles; it is not a topology-preserving
file conversion. `--sample` prints a nearest-voxel world-space scalar for queries.
Processing uses explicit file-size, triangle and conservative voxel-bounds
budgets; extremely sparse but widely separated content can exceed the bounds
budget. Invalid parameters fail before output is written.

`--offset` uses world-distance units: a positive `--distance` dilates the solid,
and a negative value erodes it. `--smooth` selects `gaussian`, `mean` or `median`;
`--width` is a kernel radius in source voxels and `--iterations` repeats the
operation. These edits accept FloatGrid level sets with uniform affine voxels,
source voxel sizes from 1e-6 to 1e6 world units and band half widths from 3 to 64
voxels. They preserve the source transform, band width and custom metadata, and use
`--grid-name` for the output name. They do not use the generation/resampling
`--voxel-size` and `--half-width` options.

Offsets are limited to 256 source voxels; nonzero offsets below 0.001 voxel are
rejected rather than silently ignored. Smoothing radius is 1 to 4 and iteration
count is 1 to 32. Topology, coordinate and conservative workspace checks run
before each offset step or smoothing iteration. Sparse but widely separated
regions can therefore exceed the bounds budget. Filtering can change features
and topology; erosion can remove a solid entirely.

### Particle surfaces and fog transport

`OpenVdbParticles.h` exposes `WriteParticlesLevelSet()` for a borrowed span of
world-space spheres and `WriteParticlesLevelSetFile()` for JSON input. The JSON
root is an array of objects with exactly `position: [x,y,z]` and `radius: r`.
Every value must be finite. Unknown or duplicate fields, incomplete records and
empty arrays fail before rasterization. Individual JSON tokens are limited to
128 encoded bytes; use scientific notation for unusually long numbers.

```powershell
'[{"position":[0,0,0],"radius":0.6},{"position":[0.7,0,0],"radius":0.5}]' | Set-Content -Encoding utf8 particles.json
.\OpenVdbTool.exe --particles particles.json particles.vdb --voxel-size 0.1 --max-particles 100000
.\OpenVdbTool.exe --smooth particles.vdb particles_smooth.vdb --filter gaussian
.\OpenVdbTool.exe --fog particles_smooth.vdb particles_fog.vdb --grid-name density

# Velocity components are world units per second, independent of grid spacing.
.\OpenVdbTool.exe --velocity flow.vdb --vector 1 0 0 --grid-name velocity
.\OpenVdbTool.exe --advect particles_fog.vdb flow.vdb moved.vdb --time 1 --steps 2 --input-grid density --velocity-grid velocity --grid-name density
.\VulkanSceneRenderer.exe --vdb-volume moved.vdb --vdb-grid density
```

Particle positions, radii and output signed distances use world units. The
result is the union of all spheres on an origin-aligned FloatGrid lattice.
Generation voxel sizes span 1.5e-5 to 1e6 world units, band half widths are greater than 1
and at most 64 voxels, and every radius must span 1.5 to 100 source voxels.
The wrapper reports particles outside these upstream cutoffs rather than
silently omitting them. The default particle-count cap is one million;
conservative voxel and workspace budgets can permit fewer particles.
Single-worker rasterization avoids per-worker tree copies. JSON uses bounded
SAX parsing with encoded-token and decoded-particle reserves. Input, aggregate
coordinate/voxel bounds, prospective tree topology and workspace are checked
before the rasterizer runs.

`OpenVdbAdvection.h` exposes `WriteConstantVelocity()` and `WriteAdvectedFog()`.
Advection accepts FloatGrid fog with uniform affine voxels, finite nonnegative
density and a zero background, plus a collocated Vec3SGrid or Vec3DGrid of
world-space invariant or contravariant-relative velocities.
Density is limited to 1e6, and nonzero inactive density values are rejected.
The velocity grid may use an independent affine transform. Index-space and
staggered velocities are rejected. Output keeps the fog's transform and custom
metadata; `--grid-name` selects its name. Generation `--voxel-size` and
`--half-width` settings do not resample an advection input.
Constant-velocity generation uses voxel sizes from 1.5e-5 to 1e6 world units;
existing advection inputs support voxel-axis sizes from 1e-6 to 1e6.
Each velocity component is finite and bounded to a magnitude of 1e12 world
units per second. Large world offsets that cannot retain sub-voxel integration
precision fail with a recentering diagnostic.
Nontrivial fog advection also rejects maps which the upstream transport's own
transform copy changes, including some small scales at distant origins. The
diagnostic requests recentering before transport; zero-time/zero-speed copies
preserve the original coefficients.

`--time` is signed elapsed time in seconds, from -3600 to 3600; `--steps` is
1 to 64 substeps. Total travel is limited to 256 source voxels and each substep
to 16; an excessive step reports how to split the operation. Source grids,
coordinate expansion and conservative workspace are checked before transport.
Zero time or zero velocity writes an unchanged density field. The bounded
semi-Lagrangian operation transports existing fog; it can diffuse density and
does not solve fluid pressure, forces or incompressibility. Nontrivial transport
normalizes by peak density before the upstream operation and restores density
units afterward. Upstream pruning is therefore relative, approximately 1e-5
of the peak, instead of erasing every fog value below an absolute threshold.

### Sampled particle trails and interior masks

`OpenVdbParticleGeometry.h` adds `WriteParticleGeometry` and the strict-file
`WriteParticleGeometryFile`. The versioned JSON schema is separate from the
older sphere-array input above:

```json
{"version":1,"particles":[{"position":[0,0,0],"radius":4,"velocity":[4,0,0]}]}
```

```powershell
.\OpenVdbTool.exe --particle-geometry particles.json trails.vdb --particle-output both --voxel-size 1 --particle-origin 0 0 0 --min-radius-voxels 2
```

`--particle-output sdf|mask|both` selects Float WORLD-distance SDF, native
MaskGrid, or both. Position, radius and velocity use WORLD units; radius cutoffs
use target voxels. Optional velocity defaults to zero. Signed `--trail-time`
(`--time-step` alias) and dimensionless `--trail-spacing` control native sampled
spheres opposite displacement. Radius tapers toward `--min-radius-voxels`; the
last sample need not reach the endpoint. The example stamps centers 0, -2 and
-3.5 along X with radii 4, 3 and 2.25. It does not create a continuous capsule.

The mask marks voxel centers strictly inside those spheres, including interiors;
SDF narrow-band activity has a different meaning. Its native class is UNKNOWN,
not LEVEL_SET. `--sdf-grid-name` (`--grid-name`) and `--mask-grid-name` name the
requested outputs. SDF-only controls reject in mask-only mode and conversely.
`--particle-origin` fixes lattice phase; absent it, the reported head/tail-box
midpoint defines the origin. Exact stored h and translation survive at
h1e-6 through h1e6. Radius cutoffs require 1.5<=min<=max<=1024; spacing spans
.25 through 4 and SDF half width spans 3 through 64 voxels. Out-of-policy
particles fail instead of being silently omitted.

Native zero-squared-distance centers can incorrectly saturate to the negative
band when radius is smaller than the band. The adapter repairs those candidates
against the complete sampled sphere union before Float conversion. The report
includes deduplicated candidate count, candidate-times-stamp work bound and
repair count, plus Float rounding/error metrics. Pair work, full combined swept
box, all simultaneous trees, JSON/SAX buffers, native partitions and writer
scratch share the explicit particle/instance/voxel/node/work/byte/metadata caps.
These are logical limits, not an RSS sandbox. Unsafe weak displacement or map
precision, malformed JSON/tokens, inexact integer conversion and numerical
underflow fail before publication. Existing destinations and source bytes are
preserved; Unicode paths use checked ASCII private SDK scratch and exclusive
native-path publication.

The main build passes 14 native feature cases and ten actual CLI cases. The
rebuilt application has 101 authoring choices and all 92 panel cases pass, within
the full available 107-group enabled and 103-group OFF milestone.
`InspectFile` and active-region `valueType` both call the Mask's value type
`bool`; `RegionValueType::Mask` distinguishes its native tree. Independent
occupancy tests check that enum and strict center/boundary counts. Exact hashes,
XML and logs are recorded in
`out/openvdb-validation/particle-fluid-fog-milestone.json`.
Per-grid artifact actions retain submitted selectors after draft changes;
native record-separator names now use exact SDK human-descriptor spelling and
pass actual native inspection/preview actions. Source-ID/attribute ownership,
continuous trajectories, cell-intersection masks and generic PointData input
remain separate variants.

### General passive field transport

`WriteAdvectedField` in `OpenVdbFieldAdvection.h` extends passive transport to
ordinary Float/Double scalar channels and collocated world Vec3S/Vec3D channels.
It retains signed/nonzero backgrounds and inactive values, and exposes six
schemes without the older fog route's relative-pruning behavior:

```powershell
.\OpenVdbTool.exe --advect-field density.vdb velocity.vdb transported.vdb --advection-scheme rk4 --time .1 --steps 2
.\OpenVdbTool.exe --advect-field density.vdb velocity.vdb corrected.vdb --advection-scheme bfecc --advection-limiter clamp --advection-mask selected.vdb
.\OpenVdbTool.exe --advect-field channels.vdb projected.vdb transported.vdb --velocity-grid velocity --velocity-layout mac-index --advection-scheme midpoint
```

Schemes are `semi`, `midpoint` (default), `rk3`, `rk4`, `maccormack` and `bfecc`.
MacCormack/BFECC use Euler forward/reverse correction. Their `none`, `clamp`
(default) and `revert` limiters operate independently per component against the
original departure cell's eight values; specifying a limiter for another
scheme rejects. Unrestricted correction can overshoot. Fog starts nonnegative;
if unrestricted correction creates a negative value, the output explicitly
becomes UNKNOWN class. Round-trip corrections and extrema reports are
heuristics, not error, convergence or conservation certificates.

Source axes are uniform orthogonal affine in [1e-6,1e6]. Collocated velocity
uses world units/second on its own conditioned affine map. `--velocity-layout
auto|world|mac-index` validates the selected layout. Native MAC input requires
STAGGERED/local/contravariant-relative tags and the saved negative-face,
voxels-per-second metadata. Components reconstruct at the corresponding
half-cell offsets before multiplication by the velocity Jacobian. Inactive
velocity values/background participate in sampling and speed bounds.

Transport uses Double trilinear sampling and RK tracing in source index units,
adapting the installed schemes while avoiding native Float positions, map
simplification, approximate-background pruning and vector lexicographic
limiters. Activity follows nonzero interpolation/correction contributions;
active zero/background samples remain represented. `--advection-mask` uses
the activity of any of the nine nonpoint native types on an exactly matching
source map. Its payload is ignored. This is a final write selector, not a
collision barrier: intermediate correction halos remain complete, and every
unselected original value/activity bit survives.

Signed time is bounded to 3600 seconds, with 1..64 equal substeps, at most 256
source voxels total travel and `--advection-cfl` (default 4, maximum 16) per
step. Active tiles require `--advection-voxelize-tiles` for nontrivial motion;
zero time/speed, empty activity or an empty effective mask writes an exact
copy. Trace-rounding envelopes are reported and capped at 1e-4 source voxel;
wholly unresolved constant motion and nonzero arithmetic range loss reject.
Full multi-pass halos, coordinates, source/velocity/mask buffers, output trees,
metadata and work share preflight caps. Exact source maps/metadata survive
exclusive ZIP publication. Seventeen native feature cases cover these routes.
Time-varying velocity inputs and broader layout/transport policies remain open;
this operation does not add pressure, forces or incompressibility.

### Connected surfaces, OBJ export and inspection

`OpenVdbSegmentation.h` exposes `WriteSegmentedLevelSet()`. It writes all
components to one new VDB file and returns a processing result for each grid.
Components are ordered by descending active voxel count, then index-space
bounds and stored content, and named with the output base plus `_000`, `_001`,
and so on. The source transform and custom metadata are retained.

```powershell
.\OpenVdbTool.exe --segment particles.vdb pieces.vdb --grid-name piece --max-components 64
.\OpenVdbTool.exe --inspect pieces.vdb
.\OpenVdbTool.exe --inspect pieces.vdb --stats --input-grid piece_000
.\OpenVdbTool.exe --export-mesh pieces.vdb pieces.obj
.\OpenVdbTool.exe --export-mesh pieces.vdb first_piece.obj --input-grid piece_000 --iso-value 0 --adaptivity 0.1
```

Segmentation uses six-connected zero-crossing surfaces rather than physical
solid-object labels. Nested cavities can produce upstream components with a
negative exterior; these are rejected with isolation/rebuild guidance to
preserve signed-distance semantics and bounded storage.
Inputs must be FloatGrid level sets with uniform affine voxels from 1e-6 to
1e6 world units, band half widths from 3 to 64 voxels, finite stored values
inside that band, and no active tiles. Empty grids and grids without a zero
crossing fail. `--max-components` accepts 1 to 1024 and defaults to 64. A
bounded zero-mask pass checks this cap before upstream segmentation. Bounds,
tree topology, mask traversal, per-component metadata copies and workspace estimates are checked
before each stage; outputs share the voxel and memory budgets. These estimates
include upstream graph reservations, but are not a hard process-memory limit.
Generation settings do not resample the source. Use `--input-grid` to select a
specific level set; otherwise the first eligible FloatGrid level set is used.
The output grid-name base is limited to 256 bytes.

`OpenVdbMeshExport.h` exposes `WriteObjSurface()`, using the existing scalar
surface importer. It exports FloatGrid or DoubleGrid isosurfaces in world
coordinates as OBJ positions, normals and indexed triangle faces (`v//vn`).
Level sets default to isovalue zero and other scalar grids to 0.5; `--iso-value`
and `--adaptivity` override these choices. Reflected transforms retain outward
winding. Each nonempty grid has a distinct sanitized group name. Extraction
retains the importer's Float32 position precision. Input/topology and triangle
limits also apply; the triangle cap is checked after extraction, so it is not
a strict peak-memory bound for the mesher. Unsupported or incomplete selected
grids fail the export. Empty grids alongside valid surfaces produce explicit
diagnostics and OBJ comments; an entirely empty selection fails. Output paths
must be new, and failed writes remove their partial output.

`OpenVdbInspect.h` exposes `InspectFile()`. `--inspect` prints JSON with
`schemaVersion: 1`, encoded `fileBytes` and a `grids` array. Each grid reports
its exact descriptor and display names, value type, grid class, vector semantics,
voxel sizes, transform information and typed metadata strings. The optional
`recorded` counts, memory sizes and bounds are file hints rather than recomputed
facts; missing or invalid hints are `null`. Safe affine transforms include a
row-major 4x4 `indexToWorld` matrix for column vectors. Nonlinear or invalid
affine maps have no matrix or world envelope and carry a diagnostic;
non-finite voxel sizes fail inspection.
World bounds enclose occupied cells, transforming all eight corners of the
index bounds expanded by half a voxel.
Inspection reads native metadata/transform records without the SDK's
metadata-grid copy, retaining distant scales and sub-tolerance shears exactly.
Affine voxel sizes come from the linear coefficients. Encoded grid/block/end
ranges and nonnegative descriptor counts are checked before seeking, shared
tree instances retain their own metadata/maps, and duplicate ordering matches
SDK file iteration. Statistics verify the archive UUID before loading trees.
Sixteen native inspection cases and the shared 61-enabled/56-disabled batch
pass; evidence is `out/openvdb-validation/exact-inspection-{cpu,disabled}.log`.

Default inspection reads metadata without voxel trees. `--stats` explicitly
loads one selected FloatGrid or DoubleGrid at a time and adds its background,
active voxel count, minimum, maximum, sum, mean, population standard deviation,
RMS, actual tree memory and active bounds.
Active tiles contribute their full voxel multiplicity without expansion;
inactive values do not contribute to sums or extrema. Empty grids have count
zero, sum zero and `null` extrema/moments/bounds. Stored values and background must be
finite, and the sum and moments must fit finite doubles. A nonzero moment which
rounds to zero is reported as an error. Fixed exact binary accumulators retain
weighted sums and squares until final rounding, avoiding cancelled intermediate
overflow and squared underflow. Point attribute statistics share this accumulator.
Unsupported grid
types carry a statistics-unavailable reason, or fail if explicitly selected
for statistics. Exact `descriptor:` selectors resolve duplicate display names.

```powershell
.\OpenVdbTool.exe --inspect density.vdb --stats --histogram-bins 64
.\OpenVdbTool.exe --inspect density.vdb --stats --histogram-bins 16 --histogram-range 0 1
```

Histograms support 1..4096 bins and require `--stats`. Automatic ranges use
active extrema; a constant range puts all values in the first bin. Explicit
finite increasing ranges report `belowRange` and `aboveRange` separately.
Interior bins are half-open; the maximum belongs to the last bin. Bin boundaries
use exact binary rational comparisons, including adjacent or extreme Double
bounds, and active tiles contribute their full counts without expansion.
Empty histograms retain zero bin counts, with null automatic bounds. Bin storage
shares the metadata/report byte cap. The panel supplies histogram count and
optional range controls alongside scalar statistics.
The completed batch passes 61 enabled groups in 124.90 seconds and 56 disabled
groups in 16.60 seconds. Twenty-three native inspection cases, eight real CLI
groups and 28 panel tests include these workflows. The shared point-statistics
suite retains thirteen passing feature cases. An independent exact-rational/
1800-digit oracle matches 2,700 histogram and weighted-moment cases. Evidence
is `out/openvdb-validation/inspection-statistics-{cpu,disabled}.log` and
`out/openvdb-review/exact-statistics/results.json`.

Inspection defaults to at most 256 scanned descriptors and 1 MiB of aggregate
metadata/report storage; `--max-grids` accepts up to 4096 and
`--max-metadata-bytes` changes the string budget. Encoded input and loaded
statistics also honor `--max-input-bytes` and `--max-voxels`. Statistics require
valid recorded count/memory hints before loading and check actual topology
afterward. Rewrite files without these hints using normal OpenVDB statistics
metadata before requesting loaded statistics. Streaming VDB files without
random-access grid offsets are rejected before opening to avoid eager tree
loads. Formats before version 216, newer unsupported formats, and colliding
human-readable descriptor names are rejected with rewrite/rename guidance.
Ordinary duplicate display names remain supported through exact descriptor
selectors. Upstream metadata decoding precedes the metadata cap, so inspection is
not a strict heap sandbox for hostile files. Invalid UTF-8 in source strings is
replaced when the CLI emits JSON.

### Native file authoring

`OpenVdbFileAuthoring.h` and `OpenVdbFileAuthoringSpec.h` preserve and combine
native grids without numerical field conversion. The version 1 JSON route is
available as `--author-file SPEC.json OUTPUT.vdb` and as an authoring-panel job.
Input paths resolve relative to the original spec directory.

```json
{"version":1,"inputs":[{"path":"fog.vdb","select":["density"],"edits":[{"selector":"density","newName":"smoke","metadata":[{"name":"artist","operation":"set","type":"string","value":"example"}]}]}],"compression":"zip"}
```

```powershell
.\OpenVdbTool.exe --author-file author.json combined.vdb --max-grids 256 --max-metadata-bytes 1048576
.\OpenVdbTool.exe --capabilities
```

Each of up to 16 inputs supports `select`, `remove` and per-grid `edits`.
Empty selection means all grids; exact `descriptor:` selectors disambiguate
duplicate display names. Edits may rename a grid and set/remove custom metadata.
Root `metadata` edits file metadata after merging; `metadataConflict` is
`reject` (default), `preferFirst` or `preferLast`. Metadata types are `bool`,
`int32`, `int64`, `float`, `double`, `string`, `vec3s` and `vec3d` (the latter
two mean three binary32/binary64 components). Int64 tokens remain exact.
Structural and recorded-statistic metadata keys cannot be edited.

Float, Double, Bool, Mask, Int32, Int64, Vec3I, Vec3S, Vec3D and PointData grids
retain topology, activity, transforms, class/vector tags and existing registered
metadata. Point attributes retain their registered codecs, strides, dictionaries,
groups and hidden flags. Transient/streaming attributes and unsupported tree
types fail explicitly. Removing all grids writes a valid zero-grid VDB.

`compression` accepts `zip` (default), `blosc` or `none`; unavailable compressors
fail. Defaults preserve native value bits, including NaN/Inf and inactive signed
zero. `activeMaskCompression` and `saveFloatAsHalf` default to false; either
opt-in reports potential precision loss. Final names must be nonempty and
unique. Unknown/duplicate JSON fields, nonfinite new metadata and numeric range
loss fail before output publication. Existing outputs and inputs remain intact.

External `--max-spec-bytes`, `--max-spec-depth`, `--max-grids`,
`--max-metadata-bytes`, `--max-edits`, `--max-points`, `--max-attributes` and
`--max-groups` limits cannot be raised by JSON. Encoded snapshots, parser/model
storage, loaded native trees/attributes and writer workspace share
`--max-input-bytes`; active voxels and stored leaf slots share `--max-voxels`.
Upstream metadata and legacy point decoding can allocate before estimates are
available; these logical caps do not constitute a hostile-file RSS sandbox.
Seekable supported-version VDB files are accepted. Stream conversion and
asynchronous file-library output remain separate pending workflows.

### PointDataGrid records

`OpenVdbPoints.h` creates and reads real PointDataGrid files using the standard
`P`, `v` and `pscale` attributes. JSON uses a root array of records with a required
`position`, optional `velocity` and optional positive `radius`. Velocity defaults
to zero and radius to one; values use world units. Unknown/duplicate fields,
nonfinite values and raw JSON tokens longer than 128 bytes fail.

```powershell
'[{"position":[0.1,0.2,0.3],"velocity":[1,0,0],"radius":0.2},{"position":[1,0,0]}]' | Set-Content -Encoding utf8 points.json
.\OpenVdbTool.exe --points points.json points.vdb --voxel-size 0.1 --grid-name points --max-points 100000
.\OpenVdbTool.exe --inspect points.vdb --input-grid points
.\OpenVdbTool.exe --export-points points.vdb points_roundtrip.json --input-grid points --max-points 100000
```

`P` stores Float32 offsets within each voxel, while `v` uses Vec3d and `pscale`
uses double, all with NullCodec and constant stride one. Local position offsets
introduce voxel-relative quantization, normally below 1e-6 voxel plus world-double
rounding. Generation accepts voxel sizes from 1.5e-5 to 1e6 world units and empty
arrays; the default point cap is one million. Point radii need only be positive;
the sphere-rasterization cutoffs described above apply to `--particles`.

Readback includes points stored in inactive voxels and accepts finite,
nonsingular affine transforms with source axes from 1e-6 to 1e6 world units and
sufficient conditioning and world-coordinate precision. It transforms local P
into world positions; velocity and radius remain world-unit payloads. Missing/incompatible
attributes, extra attributes and groups fail explicitly. Export sorts records
lexicographically by position, velocity and radius; it does not preserve insertion
order or export custom grid metadata to JSON. Seekable files and valid recorded
count/memory metadata are required before loading. Point counts, topology,
encoded data and attribute workspace are bounded. OpenVDB delayed loading must
be supported and enabled; `OPENVDB_DISABLE_DELAYED_LOAD` causes point readback to
fail. The reader requests a private file copy; upstream can fall back to the
source file if copying fails. It checks the loaded-size estimates of paged
attributes before creating attribute handles. Legacy nonpaged encodings
can allocate inside the upstream reader before these checks, so these budgets
are not a strict process-memory limit for hostile files. Metadata inspection
remains available; `--stats` provides scalar statistics, not point-attribute
statistics.

### Native typed point attributes and groups

`OpenVdbPointAttributes.h` edits native columns and group bits without unpacking
or repartitioning positions. Add/Set/Rename/Drop support bool, int32/int64,
Float/Double, Vec3f/Vec3d and native strings. Unrelated registered codecs,
descriptor/grid metadata, hidden flags, offsets and active/inactive point masks
are preserved. Position `P` is protected. Variable-stride/opaque columns may be
preserved but cannot be populated through the typed interface. Group removal
retains unused backing storage; point deletion is a separate workflow.

The bounded JSON adapter uses a version 1 edit document:

```json
{"version":1,"attributes":[{"op":"add","name":"id","type":"int64","values":[9007199254740993]}],"groups":[{"op":"add","name":"selected","values":[true]}]}
```

One value broadcasts; otherwise length must equal native point count. Rename
uses `op`, `name`, `new_name`; Drop uses `op`, `name`. Attributes run sequentially
before groups. Unknown/duplicate keys, implicit integer conversions, invalid
UTF-8 and nonfinite/underflowed numbers fail. Strings can contain escaped NUL.
Exact int64 output avoids floating-point conversion. Readback includes
`traversal:leaf_origin_xyz_then_local_index`, including inactive points; this
differs from standard `--export-points` world-position sorting. Readback documents
are not edit batches. Invalid native string bytes fail JSON export explicitly;
native editing itself preserves those bytes.

```powershell
.\OpenVdbTool.exe --edit-point-attributes points.vdb edits.json edited.vdb --input-grid points --grid-name native
.\OpenVdbTool.exe --point-schema edited.vdb schema.json
.\OpenVdbTool.exe --point-attribute edited.vdb id ids.json
.\OpenVdbTool.exe --point-group edited.vdb selected membership.json
```

CLI/UI expose point, attribute/group/edit, string and shared workspace limits.
Parser snapshots and native edits share the same memory budget. Outputs must
be new; failed exports remove owned partial files. Transient payloads,
heterogeneous descriptors and unsupported materialization fail before mutation.
Empty native trees have no leaf descriptor, so nonempty edit batches require
a stored leaf.

### Native point selection and statistics

`OpenVdbPointSelection.h` counts, keeps or deletes points using native groups.
Repeat `--any-group`, `--all-group` and `--exclude-group` to express
`(any list empty OR any match) AND all match AND no excluded match`. Every named
group must exist; an empty predicate selects every point, including points in
inactive voxels.

```powershell
.\OpenVdbTool.exe --count-points edited.vdb --any-group selected
.\OpenVdbTool.exe --point-statistics edited.vdb id --all-group selected
.\OpenVdbTool.exe --select-points edited.vdb selected.vdb --any-group selected
.\OpenVdbTool.exe --delete-points edited.vdb remaining.vdb --exclude-group protected
```

Filtering preserves native traversal, registered attributes/codecs, fixed
strides, groups, transforms, metadata and value bits. Dynamic-stride compaction
is rejected. Removing every point produces a valid empty native tree; a bounded,
versioned metadata archive retains its descriptor and leaf schema for inspection.
Repopulating that archive is a separate, pending operation.

Statistics accept the seven scalar/vector numeric column types with NullCodec
and stride one, excluding the special `P` column. Extrema retain their exact
native type, including int64 values beyond Double integer precision. Means and
population standard deviations use bounded exact accumulators before Double
rounding. Empty selections have no extrema or moments; a nonzero result below
Double range fails explicitly. CLI JSON and four authoring choices expose the
same selection, point, attribute and shared workspace limits.

The same group predicate can generate a native voxel mask:

```powershell
.\OpenVdbTool.exe --point-mask edited.vdb occupied.vdb --any-group selected --point-counts --point-count-name selected_count --grid-name occupied
```

`OpenVdbPointMask.h` marks each selected record's original stored voxel without
decoding/repartitioning positions or interpreting radius. The source transform,
including a nonlinear map, is retained. Output contains a MaskGrid and optionally
an Int64Grid of exact selected counts, both with identical active topology and
zero background. Empty selections and validated empty-schema archives yield
empty grids. Custom grid metadata survives, while point schemas and regenerated
structural/archive fields do not transfer to scalar grids. Output grids share
the voxel/byte budgets; `--point-count-name` requires `--point-counts`.

### Attribute-preserving native point movement

`OpenVdbPointMotion.h` exposes `WriteMovedPoints()` for deforming native point
positions while preserving the rest of a registered PointDataGrid. The tool
command is `--move-native-points INPUT.vdb OUTPUT.vdb`. It supports custom
attributes/groups and registered fixed P codecs; the standard-layout
`--advect-points` workflow has a separate velocity-integration contract.

```powershell
.\OpenVdbTool.exe --move-native-points source.vdb moved.vdb `
  --input-grid points --grid-name points `
  --point-world-transform 1 0 0 10 0 1 0 0 0 0 1 0 0 0 0 1
```

Both matrix options take sixteen row-major entries multiplying COLUMN vectors.
Translation is in entries 3, 7 and 11; the last row must be `0 0 0 1`.
`--point-world-transform` is the world deformation and defaults to identity.
It may be singular, so collapsing multiple points to one position is valid.
Only P is deformed. Velocity `v`, radius `pscale`, other attribute storage,
group membership, native codecs, hidden flags and descriptor metadata are
copied without vector/radius/orientation transformations. Record order may
change. An internal temporary reference verifies each original record and all
non-P payload bytes before publication; no ID column is required or added.

Group filters select deformation, never deletion:

```powershell
.\OpenVdbTool.exe --move-native-points source.vdb selected.vdb `
  --point-world-transform 1 0 0 10 0 1 0 0 0 0 1 0 0 0 0 1 `
  --any-group moving --any-group secondary --all-group enabled `
  --exclude-group locked
```

ANY groups form a union, ALL groups an intersection, and excluded groups remove
records from deformation. Empty filters select every point, including points
stored in inactive voxels. All named groups must exist. Every source record
survives; unselected world positions are preserved within the reported target
codec tolerance. Duplicate names within one CLI group clause are rejected.

Without a target option the exact source map is retained. The optional
`--point-target-transform` changes the destination partition map for all
records, including unselected points. Its axes must be uniform and orthogonal,
with length 1e-6..1e6 world units; rotations and reflections are supported.
World positions are deformed first, then repartitioned into this map.

```powershell
.\OpenVdbTool.exe --move-native-points source.vdb repartitioned.vdb `
  --point-target-transform 0.5 0 0 1 0 -0.5 0 2 0 0 0.5 3 0 0 0 1 `
  --point-position-error 0.0001
```

`--point-position-error` is the maximum component position error in target
voxels, in (0, 0.25], default 1e-4. The existing P codec is retained and trial
encoded/decoded before native movement. Coarse fixed-point codecs may require a
larger explicit tolerance. An exact no-op retains its original P bytes, skips
trial re-encoding and reports zero error. The report includes maximum voxel and
world position errors, plus the maximum unselected position error.

Activity is stored per voxel rather than per record. If active and inactive
records would share a destination voxel, the default rejects the operation.
`--allow-point-activity-merge` explicitly selects OR activity; all records in
such a mixed voxel become active, and `inactiveRecordsActivated` reports the
affected originally inactive records. Occupied inactive voxels and their
records otherwise remain inactive. `outputVoxelCount` counts occupied
destination cells including inactive cells, rather than point count or active
voxel count. Empty/native archived-schema no-ops preserve their schema archive.

Common point count, attribute/group/string, voxel and byte limits apply, along
with `--max-point-motion-work`, `--max-point-motion-grids` and
`--max-point-motion-metadata`. Defaults are 200000000 work items, 256 scanned
grids and 1048576 aggregate metadata bytes. The metadata limit includes source
catalog and predicted output descriptors. Full source arrays, planning maps,
expanded uniform-stride arrays, native target trees/move maps and writer
storage share the byte cap. The generation flags `--voxel-size`,
`--half-width`, `--max-triangles` and unrelated edit/velocity options are
rejected by this command.

The JSON report contains typed input/selected/output record counts, active
record counts, activity merges, occupied voxel count, work bound, error metrics,
no-op/archive flags, schema counts and diagnostics. It does not export arbitrary
native names or expose the temporary original-reference column. Use the native
point schema/column/group exports for data readback; their documented
leaf-origin/local-index order can change after repartitioning. Stable user ID
attributes survive exactly and can join records across files.

Source maps must be affine, conditioned and retain source-voxel world precision.
Coordinates must fit native Int32 destination margins and the P codec error
limit. Nonlinear maps, unsupported precision, inconsistent per-leaf persistent
flags/strides and dynamic-stride movement reject. Exact no-op copies may retain
dynamic layouts. Registered fixed-stride arbitrary non-P codec storage,
including signed zeros and NaN payload bits, is verified without decoding it
into the public eight-type column API. Transient/streaming arrays reject rather
than being omitted. Tiny aligned target maps are supported; the SDK can reject
tiny rotated targets through its native affine determinant limit.

Files are privately snapshotted through Unicode-safe paths and require delayed
loading. Source bytes remain unchanged. New full-precision seekable ZIP outputs
publish exclusively with owned-partial cleanup. Logical payload/work estimates
do not provide a process-RSS sandbox for eager metadata or legacy nonpaged SDK
layouts. Source/file/descriptor metadata is preserved except the output name,
regenerated point count and native writer statistics.

The authoring operation **Move native points** exposes the same world/target
3x4 matrix rows, groups, position-error tolerance, activity-OR opt-in and limits.
The final affine row stays fixed. Submission captures nested options by value;
subsequent edits affect the next job. The result is a file workflow and does
not automatically load a model.

### Attribute-preserving native point advection

`--advect-native-points POINTS.vdb VELOCITY.vdb OUTPUT.vdb` moves selected
native PointData records through a static velocity field and preserves their
registered fixed-stride attributes, codecs, groups and metadata. This is the
arbitrary native-layout counterpart of the standard-layout `--advect-points`
workflow. Only position `P` changes: stored velocity, radius, ID, orientation
and other columns are not transformed implicitly. Native record order may
change after the final repartition.

For example:

```powershell
OpenVdbTool --advect-native-points particles.vdb flow.vdb moved.vdb `
  --point-integration rk4 --time-step 0.5 --steps 8 `
  --velocity-layout world --velocity-grid velocity `
  --any-group moving --grid-name particles
```

Time is a signed total interval in world seconds; `steps` divides it into equal
RK2 midpoint or classical RK4 substeps. The velocity grid may have an
independent affine map. Samples include background and inactive stored values;
field activity does not clip motion. Group predicates select which existing
records move, including records in inactive point voxels. They do not delete
unselected records.

Already-WORLD UNKNOWN Vec3S/Vec3D grids accept all five recognized native vector
tags. Their stored XYZ components are read as physical world distance per
second. A covariant-normalize tag does not trigger normalization, and an
absolute tag does not add the grid translation. The `native-mac` route instead
requires an explicitly tagged local negative-face MAC field in voxels/second:
each component is sampled at its own half-face and the map Jacobian converts
the result to world velocity. Auto chooses from the validated grid class.

`--max-point-step-travel` and `--max-point-travel` are L2 distances in the
VELOCITY grid's index frame, not the PointData grid's voxel frame. The workflow
checks both a conservative envelope and actual traced travel. Defaults are16
per substep and256 over the complete interval. Stronger limits, the native
work cap, and shared byte/voxel/metadata/point limits can reject a job before
publication. These are conservative logical resource bounds, not a guarantee
about allocator overhead or process RSS.

An optional `--point-target-transform` contains16 row-major COLUMN-vector
matrix entries, with translation at3,7,11 and final row0,0,0,1. It must describe
a supported uniform orthogonal target map. A target change can require a final
repartition even when integration is skipped. The `integrationSkipped` report
field therefore differs from whole-operation `noOp`.

The target position-codec tolerance is expressed in target voxels through
`--point-position-error`. Its reported error is relative to computed RK
endpoints, not an estimate of error against the exact ODE solution. RK
convergence, incompressibility, mass conservation and collision avoidance are
not certified. Finite range, conditioned-coordinate, interpolation-underflow
and unresolved-displacement checks can reject otherwise representable input.

Mixed active/inactive records arriving in one destination voxel reject by
default. `--allow-point-activity-merge` explicitly selects voxel-activity OR;
the report counts originally inactive records activated by that decision.
Dynamic-stride layouts support exact no-op copies only. Nonlinear maps,
time-varying velocity, constraints, replication and automatic ID generation
remain separate capabilities.

Output publication is exclusive, seekable and full-precision ZIP. Existing
outputs are protected, source files are immutable, and owned temporary or
partial files are cleaned on failure. Runtime availability is checked before
options or paths when OpenVDB is disabled. CLI/UI report capacity and bounded
formatting work share the native job limits. Arbitrary native names use
bounded text-or-null/byte-prefix report fields rather than lossy JSON text.

The Count93 milestone validates20 native-advection cases, all13 unchanged
affine-Motion cases, the two native OFF availability checks and the actual
CLI/UI routes. Its independent NativeIO-on/OpenVDB-off Tool passes47 native
primitive/interchange/statistics cases, nine primitive CLI checks and six
existing Nano CLI checks, with the appropriate availability/conversion skips.

The subsequent shared legacy fixes preserve tiny PointData transforms while
reading metadata and publishing edited points. Direct P readback and native
world-neighbor queries now operate on the original h1e-6 output without
re-encoding it. The constant-velocity writer publishes through checked ASCII
scratch storage and a native-path exclusive stream, supporting Unicode output
filenames and parent folders. If neither system temporary storage nor the
working directory supplies a writable ASCII scratch path, it fails explicitly.

Four direct real-Tool regression groups (twelve commands) pass on the original
retained tiny fixture and independent native-array output oracles. The
strengthened advection CLI suite passes eight cases plus one availability skip
in15.010s, using direct tiny-map readback and the actual legacy Unicode velocity
writer. The rebuilt authoring binary passes64 panel cases, including native
advection, primitives and the new read-only query controls. Historical failed
commands and their temporary test-only workarounds remain in
`out/openvdb-review/native-point-advection/dependent-route-gaps.md`; current
proof is under `out/openvdb-review/legacy-route-fixes/` and
`out/openvdb-validation/region-ray-final-*`.

### Native field sampling into point attributes

`OpenVdbPointFieldSampling.h` samples a scalar/vector field at selected native
points while retaining P, offsets, activity, transforms, groups, descriptor
metadata and untouched registered attributes/codecs:

```powershell
.\OpenVdbTool.exe --sample-point-attribute particles.vdb density.vdb sampled.vdb --attribute-name density_sample --point-interpolation linear --field-grid density
.\OpenVdbTool.exe --sample-point-attribute particles.vdb ids.vdb sampled_ids.vdb --attribute-name id_sample --point-interpolation nearest --any-group selected
```

The field has its own affine transform. Sampling includes inactive point records,
inactive field values and background values. Bool/Int32/Int64 fields support exact
nearest sampling. Float/Double and class-UNKNOWN world Vec3S/D support nearest,
linear and quadratic voxel-center sampling. Existing world vector components are
sampled without renormalizing their vector tag. Explicit negative-face native MAC
supports nearest/linear sampling with component offsets and Jacobian conversion
to world vectors. Quadratic interpolation can overshoot; unsupported exponent
ranges fail instead of silently erasing tiny terms.

`--attribute-operation add` requires a missing column and gives unselected points
zero defaults. `set` requires the exact output native type, NullCodec and stride
one, preserving every unselected value bit. P and group storage cannot be targets.
Floating output defaults to Double/Vec3D; `--point-field-precision source` retains
Float/Vec3S source precision with explicit overflow/nonzero-to-zero checks.
Integer types retain their exact native representation. Empty trees/archived
empty schemas require a separate repopulation operation.

Group and native point limits are shared with the other point commands. The
`--max-point-field-work`, `--max-point-field-grids` and
`--max-point-field-metadata` controls bound catalogs, validation and sampling.
Point/field topology and encoded/decoded/clone/writer memory use aggregate
processing limits. Output is a new ZIP VDB; all paged point payloads are
materialized within the reserved budget before cloning and serialization.

### Dense arrays and multiresolution grids

`OpenVdbDenseInterchange.h` imports and exports native Float/Double/Vec3S/Vec3D
regions. Export requires absolute integer index origin and positive dimensions.
The versioned little-endian `CVDBARR` format records x-fastest values, explicit
activity bytes, typed background, affine map, grid name/class and vector tags.
It retains finite native bits, including inactive negative zero and subnormals.

```powershell
.\OpenVdbTool.exe --dense-export fog.vdb region.cvarr --dense-origin -16 -8 -4 --dense-dimensions 32 16 8
.\OpenVdbTool.exe --dense-import region.cvarr restored.vdb
.\OpenVdbTool.exe --multires fog.vdb levels.vdb --levels 3
.\OpenVdbTool.exe --multires fog.vdb coarse.vdb --levels 3 --level 2
.\OpenVdbTool.exe --multires-info fog.vdb --levels 3
.\OpenVdbTool.exe --multires-sample fog.vdb 0.25 0.5 1 1.5 --levels 3
```

Cropping omits outside data and custom metadata values. Diagnostics retain
omitted key names, the outside active count and whether stored nonbackground
values exist outside the region. A level-set class tag does not make a cropped
array a valid global signed-distance field. Import keeps the stored name/map;
generation and rename flags are rejected. Dense arrays, native trees, writer
workspace and encoded data share the processing limits.

`OpenVdbMultiResolution.h` builds one to eight retained full-weighting levels
using the upstream 27-sample low-pass kernel. Level L scales source index spacing
by 2^L while retaining world translation. World vector magnitudes are unchanged.
Ordinary scalar fields, nonnegative zero-background fog and supported collocated
world vectors are accepted; signed-distance, MAC, position and normal semantics
are rejected. Filtering is neither mass conservative nor Double exact. Affine
reflection/shear, fractional-level world sampling and all-level/single-level
output are supported. `--max-multires-work` and shared processing limits cover
tile expansion, remote stored leaves, retained levels and scratch. The panel
provides five corresponding array/level operations.

### Native point rasterization

`OpenVdbPointRasterization.h` converts standard P/v/pscale point grids directly
into a sphere-union SDF or fixed-kernel scalar/vector splats:

```powershell
.\OpenVdbTool.exe --points-sdf points.vdb point_surface.vdb --input-grid points --voxel-size 0.1
.\OpenVdbTool.exe --splat-points-density points.vdb point_mass.vdb --voxel-size 0.1 --point-mass 2 --grid-name rho
.\OpenVdbTool.exe --splat-points-velocity points.vdb point_flow.vdb --voxel-size 0.1 --splat-density-name rho
.\OpenVdbTool.exe --splat-points-radius points.vdb point_size.vdb --voxel-size 0.1 --splat-density-name rho
```

The SDF uses physical radii spanning 1.5..100 target voxels. Splats use eight
voxel-center trilinear weights independent of radius. Double density integrates
to total point mass; velocity and radius are normalized weighted means with a
companion density grid. Velocity is a world-space Vec3D grid. Source metadata
is preserved, input records include inactive point voxels, and exact contributing
cells/nodes are preflighted against aggregate output limits. Positive contributions
lost to numeric range fail rather than silently losing mass. Generic attributes
and frustum rasterization remain pending. Source and existing output files are
preserved. The authoring panel offers the same four workflows.

### Point scattering, trajectories and attribute sampling

`OpenVdbPointProcessing.h` exposes density-weighted `ScatterPoints()`,
`WriteAdvectedPoints()` and `WriteSampledPoints()` for standard P/v/pscale
records. All three keep custom grid metadata and write a new PointDataGrid.

```powershell
.\OpenVdbTool.exe --scatter particles_fog.vdb scattered.vdb --input-grid density --point-count 10000 --seed 42 --point-radius 0.05 --voxel-size 0.1
.\OpenVdbTool.exe --advect-points scattered.vdb flow.vdb moved_points.vdb --time 0.25 --steps 2 --integration rk4 --velocity-grid velocity --keep-velocity
.\OpenVdbTool.exe --sample-point-velocity moved_points.vdb flow.vdb sampled_points.vdb --field-grid velocity
.\OpenVdbTool.exe --sample-point-radius sampled_points.vdb particles_fog.vdb sized_points.vdb --field-grid density
```

Scattering accepts Float/Double fog with finite nonnegative densities, zero
background and zero inactive values. Active tiles are sampled without dense
expansion; cell probability is proportional to density times cell volume.
Within a selected cell, positions are uniform in index space, then transformed
to world space. A documented 53-bit mapping of `mt19937_64` makes seeds
repeatable across STL implementations. Zero count produces an empty point
grid. Density ranges whose mass is lost during normalization, CDF accumulation
or random-number resolution are rejected with partition/rescaling guidance.

Point trajectories use midpoint RK2 or classical RK4 with signed time and
1..64 substeps. Independent affine world-space Vec3S/Vec3D velocity fields are
sampled in Double; all already-world vector tags are accepted. Final velocity
is sampled into `v` unless `--keep-velocity` preserves it. Radius sampling
accepts Float/Double fields and requires every sampled radius to be positive.
Stable trilinear interpolation handles opposite-sign finite Double endpoints.
Sampling retains positions and the other standard attribute.

Processing repartitions records into origin-aligned `--voxel-size` cells; it
does not retain the source point transform or insertion order. Descriptor
metadata, hidden/transient flags, inactive point-voxel selections, custom
attributes and groups are rejected rather than silently discarded. Coordinate,
world/index precision, point, topology, encoded-input, metadata and conservative
workspace limits apply before publication. Point motion is limited to 256
source point voxels total and 16 per substep. Unresolvable motion or source-grid
fractions fail with recentering guidance. These are static-field point workflows;
they do not simulate particle forces or particle-fluid coupling.

### Ordinary affine field resampling

This route is separate from `--resample`, which rebuilds or resamples level-set/fog data under its existing contract. `--resample-field` samples an ordinary native field on an explicit affine target map and optionally deforms its world domain.

```text
--resample-field INPUT.vdb OUTPUT.vdb
--field-target-transform M00 ... M33
--field-world-transform M00 ... M33
--field-interpolation nearest|linear|quadratic
--field-activity nearest|any
--field-vector-values semantic|raw
--max-field-resampling-work N
--max-field-resampling-grids N
--max-field-resampling-metadata N
--grid/--input-grid NAME
--grid-name NAME
--max-voxels N
--max-input-bytes N
```

Both matrices contain 16 row-major COLUMN-vector entries, with translation at entries 3/7/11 and final row `0 0 0 1`. Target omission preserves the exact source map. World deformation defaults to identity. Both matrices must be invertible, affine and sufficiently conditioned; target/deformation scale and index-precision limits are enforced by the native API.

The default output name is `field`. Generation controls such as voxel size, band half width and triangle count are incompatible with this command. Choose the target matrix to set output spacing and orientation. Changing only the target samples the same world field and preserves its vector components. World deformation changes the sampled domain; semantic vector handling is explicit below.

Nearest preserves native Float, Double, Bool, Mask, Int32, Int64, Vec3I, Vec3S and Vec3D values with independent activity. Ties select the greater integer index. Real Float/Double scalar and vector fields also support linear and quadratic interpolation; linear is the default. Values include inactive stored samples and background. Activity defaults to any mathematically nonzero stencil contributor; `nearest` instead takes source activity from the nearest cell. Bool inactive true/active false and Mask activity remain distinct.

Accepted classes are ordinary UNKNOWN fields and nonnegative scalar FOG fields. Signed-distance LEVEL_SET, staggered/native MAC, local vectors, nonlinear maps and unsupported native types reject. Interpolation does not repair distance values, antialias, conserve mass, or certify a fog integral. Negative quadratic fog overshoot rejects.

With a nonidentity world deformation `A,t`, semantic WORLD vector handling is:

| Native tag | Stored output value |
| --- | --- |
| Invariant | Unchanged components |
| ContravariantRelative | `A*v` |
| ContravariantAbsolute | `A*p+t` |
| Covariant | `inverse-transpose(A)*v` |
| CovariantNormalize | Normalized covariant result; zero remains zero |

Infinite background receives the same transformation. Identity deformation preserves original values, including nonunit normalized-tag vectors. `--field-vector-values raw` preserves all raw world components explicitly while retaining their tags. Vec3I semantic deformation is limited to signed axis permutations with power-of-two scales and exact integral Int32 results. Normalized Vec3I requires exact zero/axis results; no hidden rounding onto an integer/axis occurs.

Identity map/deformation preserves native topology and every value/activity bit. Other operations sample a conservative kernel-padded target box covering every active stored item and bitwise-nonbackground inactive value, including signed zero/subnormals. Every target cell in that box is stored. Widely separated sparse support may exceed conservative domain/leaf budgets. Float rounding is reported. Overflow, nonzero underflow, unrepresentable interpolation fractions and detectable quadratic dynamic-range loss reject before publication.

Example, resample at half spacing with a reflection in Z, without deforming vector values:

```sh
OpenVdbTool --resample-field source.vdb sampled.vdb --input-grid field \
  --field-target-transform .5 0 0 0 0 .5 0 0 0 0 -.5 0 0 0 0 1 \
  --field-interpolation linear --field-activity any --grid-name sampled
```

Example, deform a world field and its semantic vector values into a translated reflected/scaled target:

```sh
OpenVdbTool --resample-field vectors.vdb moved.vdb \
  --field-world-transform 2 0 0 5 0 -1 0 6 0 0 .5 7 0 0 0 1 \
  --field-target-transform 2 0 0 5 0 -1 0 6 0 0 .5 7 0 0 0 1 \
  --field-vector-values semantic
```

The command emits schema-version-1 typed JSON with native type/class/name, active and source/output leaf counts, sampled/inactive cell counts, work upper bound, Float rounding count/error, exact-copy flag, inclusive source/target bounds and diagnostics. Caller UTF-8 is validated and bounded before VDB publication, and report storage shares the byte budget with native work. Disabled builds report availability before converting paths or building reports.

Native input uses immutable private snapshots and native Unicode filesystem paths. Delayed loading is mandatory. Aggregate file/catalog metadata, typed full-buffer memory, stored root/internal/leaf traversal, joint output leaf/domain/native workspace and writer buffers are preflighted. Custom grid/file metadata and exact target coefficients survive regenerated structural/provenance fields. Source files stay unchanged; new ZIP outputs are exclusive and partial output cleanup is owned. Limits are logical payload/work estimates, not a hostile-file heap or RSS guarantee.

The 87th authoring operation is **Resample ordinary field**. It exposes the same interpolation/activity/vector policies, two COLUMN 3x4 matrix editors with a fixed affine final row, optional target map, shared byte/voxel limits and native work/catalog/metadata caps. It does not automatically load an output as a model. Submission copies all nested options; subsequent draft edits apply only to the next explicit job.

### Generic native point-attribute transfer

`WriteTransferredPointAttribute` in `OpenVdbPointTransfer.h` deposits a selected
numeric point column into an ordinary scalar or collocated vector field. Each
point contributes to eight cell centers with CIC/hat weights. The companion
grid always stores Double weight sums or mass per world voxel volume.

```powershell
.\OpenVdbTool.exe --transfer-point-attribute points.vdb temperature mean.vdb --transfer-mode mean --grid-name temperature
.\OpenVdbTool.exe --transfer-point-attribute points.vdb signal weighted.vdb --transfer-mode sum --transfer-weight-attribute mass --transfer-companion density --any-group selected
.\OpenVdbTool.exe --transfer-point-attribute points.vdb signal unit-map.vdb --transfer-target-transform 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1
```

The default target spacing is 0.1. `--transfer-target-transform` takes sixteen
row-major COLUMN-vector coefficients, translation at 3, 7 and 11 and final row
`0 0 0 1`. Uniform orthogonal axes in [1e-6,1e6] support rotations/reflections;
the target map controls deposition coordinates. The source positions stay
unchanged. Tiny rotated affine maps can exceed the SDK determinant limit.

Chosen Bool/Int32/Int64/Float/Double/Vec3I/Vec3S/Vec3D columns require registered
numeric codecs and stride one. P, strings, group backing and selected
dynamic/strided columns reject. Unrelated columns remain untouched. Stored
vector components receive no implicit Jacobian, translation, normalization or
unit conversion. All selected values must be finite; selected masses must be
strictly positive. Groups select records for deposition, including inactive
point records, using the same ANY/ALL/exclude predicates as native selection.

`--transfer-mode mean|sum` defaults to mean. Sum is `sum(value*mass*kernel)`;
mean divides that sum by `sum(mass*kernel)`. `--transfer-mass` sets a constant
positive mass, default 1; it is mutually exclusive with
`--transfer-weight-attribute`, even when the supplied constant is 1.
`--transfer-storage double|float` defaults to Double. The companion remains
Double, with `--transfer-companion weight|density` defaulting to weight.
Its default name is the field name plus `_weight` or `_density`; override it
with `--transfer-companion-name`. Both grids have identical positive-weight
activity, zero inactive backgrounds and UNKNOWN class. Cancelled or zero-valued
field cells remain active. Empty selections produce empty grids.

Int64 values that lose bits when converted to Double reject by default.
`--transfer-integers allow-rounded` opts into counted conversion; `exact` is
the default. Float output checks range and reports rounding. Nonzero field,
weight or density outputs that round to zero reject. Exact dyadic weights and
bounded accumulators retain tiny support and large intermediate cancellation
before final native rounding. Canonical mass equality is checked before output
rounding; the reported companion mass-error bound describes the stored result.
Density divides by the reported rounded absolute determinant of the actual
target axes. This is a fixed collocated transfer kernel; it supplies no
arbitrary-kernel, MAC or frustum rasterization contract.

The JSON report gives input/selected/inactive counts, contributions, native
output grids, component count, extrema availability, source/deposited mass,
voxel volume, rounding counts/errors, work and bounded diagnostics. Output
field names default to `field`. `--grid` and `--input-grid` are selector aliases.
Common point/attribute/group/string and voxel/byte caps apply, together with
`--max-point-transfer-work` (default 2000000000, maximum 10000000000),
`--max-point-transfer-grids` (default 256, maximum 4096) and
`--max-point-transfer-metadata` (default 1048576 bytes).

Full source arrays before paging, encoded snapshots, joint output/proxy native
nodes, bounded exact scratch, metadata and writer storage share the caps.
The CLI also reserves its report/options workspace before publication and
subtracts that reserve from the native byte allowance. Delayed loading is
required. These are logical workspace/work limits, rather than a process-RSS
sandbox for eager metadata or legacy SDK allocation. Unicode paths use private
snapshots; new seekable ZIP output publishes exclusively, with owned-partial
cleanup. Source bytes and existing outputs remain unchanged.

The authoring panel exposes the column, groups, reduction, precision, mass
source, companion, target matrix and budgets. Switching between constant and
attribute weights clears incompatible settings. Submitted options are copied
by value; field and native-point previews remain explicit actions.

### Native point indices, particle queries and rebucketed masks

`OpenVdbPointNeighbors.h` exposes `QueryPointNeighbors` using native
PointIndexGrid/ParticleAtlas structures and `WriteIndexedPointMask` for explicit
output-map occupancy:

```powershell
.\OpenVdbTool.exe --point-neighbors points.vdb --neighbor-query 0 0 0 .5 --neighbor-buckets
.\OpenVdbTool.exe --point-neighbors points.vdb --neighbor-mode particles --neighbor-radius-attribute pscale --neighbor-query 0 0 0 0
.\OpenVdbTool.exe --indexed-point-mask points.vdb cells.vdb --indexed-voxel-size .1 --indexed-origin 0 0 0 --indexed-counts --grid-name point_cells
```

Center queries include rounded-Double Euclidean distance `<= query radius`.
Particle queries include `distance <= query radius + particle radius`; radius
zero supports coincident-center or point-in-particle tests. Hits sort by distance
then original ordinal, without self/duplicate suppression. References retain
the P03 leaf-origin/native-index traversal, including selected inactive points;
selection never renumbers them. Registered fixed P codecs decode without
repartitioning. Group predicates use the existing any/all/exclude controls.
Particle radii come from a positive scalar attribute or `--neighbor-radius`,
bounded to 1e6 world units; dynamic/P/group storage rejects. Radius/atlas
controls apply only to particle mode.

`--neighbor-index-voxel-size` sets the base world scale in [1e-6,1e6]. Positions
are recentered at the first selected world point and normalized before native
construction. Final distances/radius tests use original world values. Reported
level scales, `indexOriginWorld` and optional eight-cell-wide bucket origins
make partitions interpretable; bucket references remain in original ordinal
order. Adaptive atlas subdivision obeys `--neighbor-atlas-levels` (default 16,
maximum 32), not a requested exact level count. Native approximate-uniform-radius
filters do not determine final inclusion.

The indexed mask rebuckets point centers onto the explicit axis-aligned world
map as a native MaskGrid. It differs from `--point-mask`, which marks original
stored point voxels.
Particle radii do not expand mask coverage. Optional exact Int64 counts include
inactive-point records, defaulting to the output mask name plus `_count`;
`--indexed-count-name` requires `--indexed-counts`. Custom grid/file metadata
survives, with point/archive structural keys removed from mask outputs.

Point/attribute, index-coordinate, atlas, query-box, hit/bucket, metadata, work
and ZIP-writer limits share preflight budgets. `--max-neighbor-queries`,
`--max-neighbors` and `--max-point-index-work` bound reports and computation;
catalog/metadata and common voxel/byte limits also apply. Empty selections
produce empty hits; bounded empty-schema archives are validated. Sources are
unchanged and mask output is exclusive. Twelve native feature cases cover
stable references, inclusive boundaries, groups, radii, scales, buckets and
mask counts. Reusable index lifecycles and broader partition/filter policies
remain open; raw PointPartitioner sort controls are not exposed.

### Ordinary scalar and vector filtering

`OpenVdbFieldFilter.h` filters ordinary Float/Double fields and supported
collocated world vectors. This operation has its own CLI and panel route:

```powershell
.\OpenVdbTool.exe --filter-field fog.vdb blurred.vdb --field-filter gaussian --width 1 --iterations 2
.\OpenVdbTool.exe --filter-field velocity.vdb shifted.vdb --field-filter offset --field-offset 1 -2 0.5
.\OpenVdbTool.exe --filter-field fog.vdb masked.vdb --field-filter mean --filter-mask alpha.vdb --filter-mask-grid alpha
```

Kernels use index voxel units and write active cells only. Stored inactive
values and background participate as neighbors and retain their original bits.
Active tiles remain unchanged unless `--voxelize-tiles` explicitly expands them.
Mean/Gaussian are componentwise; Gaussian is the native four-box approximation.
Vector median ranks whole vectors lexicographically by X, Y and Z. Upstream
Float reciprocal/alpha weights can round Double results. Offsets use stored
component units once per iteration; scalar Y/Z offsets must be zero. Fog must
remain nonnegative. Signed-distance tracking and MAC operators have separate
routes and are rejected here.

Float/Double masks require exactly matching affine coefficients. Activity is
ignored: all stored values and background define alpha. `--mask-min` and
`--mask-max` normalize the range, followed by SmoothStep; `--invert-mask`
reverses the weights. Separable filters blend at each axis pass. Interior
weights lost in Float precision fail explicitly. Source and mask are unchanged.
`--max-filter-work`, shared voxel and input/workspace limits cover stored
topology, optional tile expansion, masks, clones, buffers and output. Output is
exclusive ZIP VDB, retaining metadata and inactive signed zero.

### Active topology masks and morphology

`OpenVdbTopology.h` extracts occupancy from active states of the nine supported
native scalar/vector/mask types. Active zero, false or nonfinite values are
occupied; inactive nonzero values and negative SDF interiors are unoccupied.
PointData grids use the separate selected-point mask route above.

```powershell
.\OpenVdbTool.exe --topology-mask field.vdb active.vdb
.\OpenVdbTool.exe --topology-mask field.vdb expanded.vdb --topology-operation dilate --topology-iterations 2 --topology-neighbors 26 --topology-tiles preserve
.\OpenVdbTool.exe --topology-boolean a.vdb b.vdb difference.vdb --topology-boolean-operation difference --grid-a density --grid-b density
```

Output is a MaskGrid with Boolean values equal to activity, including compact
tiles. Morphology offers `dilate|erode|open|close`, one to 32 index-lattice steps
per phase and 6/18/26 neighbors. It does not imply a world-distance SDF offset.
`preserve` keeps compact unchanged tile interiors, `expand` voxelizes active
tiles, and `ignore` edits only leaf voxels while tiles still act as erosion
neighbors. Consequently ignore-tiles open/close need not obey whole-set
morphological identities for tiled inputs.

Boolean `union|intersection|difference` combines active sets with exact affine
map matching and no resampling; source value types/classes may differ. Source A
custom metadata survives with regenerated mask semantics/statistics. Finite
nonsingular affine maps are retained exactly. Local candidate leaf/node sets
avoid a giant global box around disconnected remote regions. Prospective tree,
scratch, metadata and writer budgets are conservative; `--max-topology-work`
bounds traversal/expansion. The panel provides separate morphology and Boolean
operations. Native value/activity edits, box clip/fill, exact pruning and
background replacement are described below; mask clipping and signed-interior
repair remain outstanding in the completion ledger.

### Native value and activity editing

`WriteValueTopology` in `OpenVdbTopology.h` edits native Float, Double, Bool,
Int32, Int64, Vec3I, Vec3S and Vec3D grids. It retains value/activity bits outside
the declared edit, the exact affine transform, vector frames and file/grid
metadata. MaskGrid has no independent stored leaf values; use the occupancy
tools above. PointDataGrid uses the point editing tools.

```powershell
.\OpenVdbTool.exe --edit-topology field.vdb active.vdb --edit-operation activate-value --native-value -0
.\OpenVdbTool.exe --edit-topology ids.vdb inactive.vdb --edit-operation deactivate-value --native-value 9223372036854775807
.\OpenVdbTool.exe --edit-topology vector.vdb active-vector.vdb --edit-operation activate-value --native-vector 1.125 -2 3
.\OpenVdbTool.exe --edit-topology field.vdb compact.vdb --edit-operation prune-exact
.\OpenVdbTool.exe --edit-topology field.vdb filled.vdb --edit-operation fill-box --native-value 4.125 --index-box -2 -1 0 0 1 0 --fill-inactive
.\OpenVdbTool.exe --edit-topology field.vdb clipped.vdb --edit-operation clip-box --index-box -8 -8 -8 7 7 7
.\OpenVdbTool.exe --edit-topology field.vdb background.vdb --edit-operation replace-background --native-value 7
```

Activation/deactivation matches stored native values bit for bit, including
signed zero. It leaves the infinite implicit background inactive. Default
internal tiles are stored values, so activating them can exceed the active cap
even for a source with few leaves. Integers parse directly in their native type;
Bool requires `true` or `false`. Finite decimal real values round once to the
native Float/Double type. Use one scalar or exactly three vector components.
Stored values and background must also be finite, including inactive values.

Exact pruning collapses only children with identical native bits and activity.
It retains signed background tiles and mixed activity; no tolerance or numeric
median is used. Fill replaces values/activity in an inclusive **index** box.
Clip retains values/activity inside that box and resets the outside to inactive
background. It reconstructs sparse regions without upstream numeric pruning.
The conservative dense node/leaf estimate covers even inactive fills. The last
Int32 root block is rejected (`maximum <= INT32_MAX-4096`) to avoid upstream
sparse-fill loop overflow. Recenter those boxes before processing.

Background replacement changes the infinite background and inactive stored
values bit-identical to the old background. Opposite signs, nearby unequal
values and active values survive. This differs from upstream approximate,
sign-paired background replacement and does not repair SDF signs/band widths.
Only exact pruning accepts SDF/staggered classes. Other edits require ordinary
or fog fields and output class UNKNOWN; no field-quality guarantee is inferred.

The authoring panel exposes all six edits with immutable job snapshots.
`--grid`, `--grid-name`, `--max-voxels`, `--max-input-bytes` and
`--max-topology-work` apply. Values/boxes/fill activity are accepted only where
used. Shared input, clone, expansion and writer budgets are checked before
growth; outputs must be new files. Seekable supported-version inputs with
recorded statistics are required. Output uses full-precision ZIP without
active-mask value loss. Private snapshots support Unicode input/output parents;
the upstream narrow API still needs a usable temporary-directory path.
Closed-field background normalization and signed flood fill are described
below. Surface closure repair and broader world/frustum clip domains remain
separate outstanding workflows.

`WriteMaskedValues` in `OpenVdbMaskedValues.h` adds native masked activation,
deactivation, activity intersection and inside/outside clipping:

```powershell
.\OpenVdbTool.exe --edit-topology-mask field.vdb mask.vdb activated.vdb --mask-operation activate
.\OpenVdbTool.exe --edit-topology-mask field.vdb mask.vdb inactive.vdb --mask-operation deactivate
.\OpenVdbTool.exe --edit-topology-mask field.vdb mask.vdb selected.vdb --mask-operation intersect-activity
.\OpenVdbTool.exe --edit-topology-mask field.vdb mask.vdb inside.vdb --mask-operation clip-inside --grid field --mask-grid mask
.\OpenVdbTool.exe --edit-topology-mask field.vdb mask.vdb outside.vdb --mask-operation clip-outside
```

Source A supports the same eight finite native value types. The mask supports
those types and MaskGrid; only its activity selects cells, including active
false, zero or NaN values. Inactive nonzero values and negative SDF interiors
do not select cells. Affine maps must match exactly; masks are not resampled.
Activate uses A OR mask activity, deactivate A AND NOT mask, and intersection
A AND mask. All three preserve every logical source value, including inactive
values in branches which native topology intersection/difference might delete.
Clipping retains source values/activity on the chosen side and replaces the
other side with inactive source background. It performs no numeric pruning.

Both inputs share byte/active/stored-slot/work caps. Projected output node sizes
use the source value type: a compact mask node does not stand in for the much
larger Vec3D node it can require. Source A metadata and vector frames survive;
mask metadata is not merged. Class becomes UNKNOWN and statistics/provenance
are regenerated. The authoring panel supplies the second mask path/grid, all
five modes and a shared work limit. Other output, availability and snapshot
rules match the native edit workflow above.

The combined batch passes 61 enabled CPU/configuration/CLI/UI/shader/package
groups in 123.89 seconds and 56 disabled groups in 26.11 seconds. The topology
suite has 31 feature cases plus three availability cases; eleven CLI groups
and 26 panel cases cover the application routes and all 70 operation choices.
All 20 live transport/image/upload GPU cases pass in 5.48 seconds, without GPU
skips or VUIDs. The rebuilt application includes both edit workflows. Logs are
`out/openvdb-validation/masked-values-{cpu,disabled,gpu}.log`,
`masked-values-main-cli.log` and `masked-values-ui.log` in that directory.

### SDF background normalization and signed flood fill

`WriteSdfBackground` in `OpenVdbSdfBackground.h` edits native Float/Double
LEVEL_SET backgrounds and inactive narrow-band signs:

```powershell
.\OpenVdbTool.exe --sdf-background surface.vdb widths.vdb --sdf-mode background --outside-width 1 --inside-width 1.25
.\OpenVdbTool.exe --sdf-background surface.vdb filled.vdb --sdf-mode flood-fill
.\OpenVdbTool.exe --sdf-background surface.vdb selected.vdb --sdf-mode flood-fill --grid surface --max-topology-work 200000000
```

Widths are positive world-distance magnitudes. Outside defaults to the source
background; inside defaults to the requested outside width. The widths round
once to the native scalar type, must remain finite and positive, and must
contain every existing active value on their respective side. The operation
preserves every active native value and activity bit. Changing these widths
does not expand activity or construct a wider distance band.

`background` sets stored inactive negative values to minus the inside width,
other inactive values to the outside width, and the infinite background to the
outside width. Negative zero is outside. `flood-fill` first propagates signs
with OpenVDB's signed flood fill, then normalizes inactive root and child
values. It assumes a closed signed field. Inactive-only nodes retain their
sign seeds; arbitrary corrupted interiors, open surfaces and missing surfaces
need separate repair or reconstruction.

Inputs require finite stored/background values, positive background, uniform
orthogonal affine voxel axes in [1e-6,1e6] and no active tiles. Root scanline
growth is reserved against shared byte/work limits before filling, and root
child-coordinate differences must fit signed Int32. Native metadata and exact
affine coefficients survive, SDF class remains LEVEL_SET, half saving is
disabled and file statistics/provenance are regenerated. Exclusive ZIP output
and Unicode snapshot/publication rules match native value editing above.

The authoring panel supplies both modes, optional outside/asymmetric inside
widths and the work cap. The focused native suite has seven SDF cases plus a
cross-workflow exact-transform regression. Dense and NanoVDB conversion tests
also retain distant scales and sub-tolerance shears exactly. Transform copies
avoid upstream map simplification; matrix factories retain stock map records.
These guarantees preserve coefficients, while sampling still requires enough
world-coordinate precision for the requested operation.

### Native SDF filtering and tracking

`WriteSdfFiltered` in `OpenVdbSdfFilters.h` retains native Float/Double distances
and exposes thirteen operations:

```powershell
.\OpenVdbTool.exe --sdf-filter surface.vdb smooth.vdb --sdf-filter-operation mean-curvature
.\OpenVdbTool.exe --sdf-filter surface.vdb fillet.vdb --sdf-filter-operation fillet --sdf-iterations 2
.\OpenVdbTool.exe --sdf-filter surface.vdb expanded.vdb --sdf-filter-operation offset --sdf-distance .05
.\OpenVdbTool.exe --sdf-filter surface.vdb tracked.vdb --sdf-filter-operation track --sdf-spatial hjweno5 --sdf-temporal rk3
.\OpenVdbTool.exe --sdf-filter surface.vdb wider.vdb --sdf-filter-operation resize-band --sdf-band-width 6
.\OpenVdbTool.exe --sdf-filter surface.vdb selected.vdb --sdf-filter-operation normalize --sdf-normalization-mask activity.vdb
```

The modes are `gaussian`, `mean`, `median`, `mean-curvature`, `laplacian`,
`fillet`, `offset`, `normalize`, `track`, `prune`, `resize-band`, `dilate-band`
and `erode-band`. Positive offset expands outward; distance is per iteration
in world units. Kernel radius is 1..4, iterations 1..32, normalizations per
tracking pass 0..64 and band widths/layers 1..64 source voxels. Final band half
width must remain 1..64 voxels. Gaussian uses the SDK's four mean passes.

Applicable tracker operations expose first/second/third-order, WENO5 and
HJWENO5 spatial schemes, TVD RK1/RK2/RK3 time schemes and trimming of neither,
either or both sides. The CLI rejects flags that do not apply to its mode.
Partial trimming can leave out-of-band active distances. Pruning retains the
original world values, including inactive values and signed zero; it does not
round-trip distances through unit conditioning. Zero offset and zero-count
normalization retain exact source values.

`--sdf-mask FILE` with optional `--sdf-mask-grid`, `--sdf-alpha-range MIN MAX`
and `--sdf-invert-alpha` accepts a matching-map Float/Double alpha field for
the seven filter modes through offset. Stored values and background define
smooth-step alpha regardless of activity. Subsequent tracker normalization is
global, so an alpha-zero filter is not an exact-value preservation mask.

Normalization separately accepts `--sdf-normalization-mask FILE` and optional
`--sdf-normalization-grid`. Its Bool/Mask/Int32/Int64/Float/Double/Vec3I/Vec3S/
Vec3D mask selects by native activity alone, including active zero, false or
NaN. Selection intersects existing source active voxels. It neither grows nor
trims topology; every unselected world value and activity bit is retained
exactly. Empty selections and zero normalization count copy values exactly.
The JSON result reports `normalizationSelectedVoxels`, or null without this
mask. Alpha and normalization masks are separate modes and require exact
affine-map agreement with the source.

Sources require finite LEVEL_SET values, positive background, uniform
orthogonal affine axes of length [1e-6,1e6], and no active tiles. Working grids
use unit distances and recentered integer indices; nonzero distances that
underflow during conditioning or restoration are rejected. Full prospective
stored boxes, growth, nodes and auxiliary buffers are reserved before native
work against shared input/voxel/byte/work limits. `--max-sdf-work` defaults to
200 million work items. Output retains file/grid metadata, exact affine
coefficients, LEVEL_SET class and full native precision, with exclusive ZIP
publication and Unicode input/output parents.

Reports include half width, active/leaf/work counts and a near-interface
central-gradient mean error with its fully active stencil count. This is a
local quality indicator; it does not certify closed surfaces or globally valid
signed distances. The panel supplies the same controls and reports. The native
SDF and inspection follow-up has 46 passing feature cases, including all
spatial/time combinations, all nine activity-mask types, exact unselected
bits, small persisted voxel maps and Double intermediate-underflow failures.
The coordinated follow-up passes all 64 available enabled groups in 143.27
seconds, all 60 disabled groups in 19.67 seconds and twenty live GPU cases in
6.05 seconds without GPU skips or VUIDs. Eighteen topology/normalization command
groups and 33 panel cases pass. At that checkpoint, the configured enabled
suite had 65 groups and its Bitdefender-quarantined main topology executable
was an unresolved validation gap. No quarantined payload or security settings
were changed during that batch. Evidence is
`out/openvdb-validation/sweeping-spheres-*.log`.

### Closed-contour level-set rebuilding


`OpenVdbLevelSetRebuild.h` exposes `WriteRebuiltLevelSet()` and the tool exposes
`--rebuild-level-set` for rebuilding a bounded closed scalar contour into a new
signed-distance grid. The source may be Float or Double and tagged unknown,
level set or fog; its values need not already be distances. A chosen finite
isovalue defines the contour. The interior is the side opposite the source
background, which must differ from the isovalue. The output retains the source
native Float/Double storage type and stores signed distances in world units.

```powershell
.\OpenVdbTool.exe --rebuild-level-set source.vdb repaired.vdb `
  --input-grid surface --grid-name repaired `
  --rebuild-isovalue 0 `
  --rebuild-exterior-width 4 --rebuild-interior-width 2
```

The exterior and interior widths independently accept 1..64 **target voxels**.
They are rounded to native Float widths; the JSON report gives their effective
values. For example, exterior 4 and interior 2 at target spacing 0.25 produce an
exterior background of +1 and an interior clamp of -0.5 world units. The
generation flags `--voxel-size` and `--half-width` do not apply to this command.

By default the exact source native transform is retained. To change the target
map, pass sixteen entries to `--rebuild-target-transform` in row-major order,
using **column vectors**: translation is in entries 3, 7 and 11, and the final
row must be `0 0 0 1`. The three axes must be uniformly scaled and orthogonal;
rotations and reflections are allowed. This example changes the spacing to
0.125 and translates the target index origin to world (10, 0, 0):

```powershell
.\OpenVdbTool.exe --rebuild-level-set source.vdb finer.vdb `
  --rebuild-target-transform 0.125 0 0 10 0 0.125 0 0 0 0 0.125 0 0 0 0 1
```

Source and target voxel sizes are bounded to 1e-6..1e6 world units. Aligned
native maps at the small endpoint are supported through exact stock-map
serialization. The SDK can reject tiny rotated affine maps or conditioned
relative maps that it changes while copying; these fail before publication.
World/index conditioning avoids ordinary remote-origin Float narrowing, but
the native meshing stage still uses Float positions. A conservative coordinate
quantization check requires resolution within 1e-4 target voxel. This is a
coordinate representation guard, not a guarantee of that distance accuracy.

Rebuilding is approximate and can merge overlapping interior edges or close
internal bubbles. It is not topology preserving. Finite values, a bounded
closed contour, valid bounding faces and representable isovalue subtraction
are required; these checks do not certify manifoldness. Open planes, missing
crossings, nonfinite values, unsupported transforms and lost nonzero gradients
fail rather than producing a misleading level set. Double output storage does
not make the intermediate mesh geometry Double precision.

The report includes the source/output native type, effective bands, active
voxel counts, integer conditioning origins, conservative mesh/work upper
bounds and approximation diagnostics. It reports the requested target matrix
when supplied. Inspect the resulting file for its final transform and stored
metadata.

```powershell
.\OpenVdbTool.exe --rebuild-level-set source.vdb repaired.vdb `
  --max-rebuild-work 1000000000 --max-rebuild-grids 256 `
  --max-rebuild-metadata 1048576 `
  --max-voxels 16000000 --max-triangles 2000000 `
  --max-input-bytes 536870912
```

The limits cover dense source/output bounds, stored native traversal, worst-case
mesh/scratch trees, metadata and writer reserves. Sparse content spread across
a large domain can therefore exceed a cap. Grid catalog and file/grid metadata
are counted together. The loader requires recorded native memory/voxel
statistics and delayed loading, then checks full typed buffer requirements
before paging. A private source snapshot preserves the submitted bytes; source
and file custom metadata survive except regenerated native/provenance fields.
Output is a seekable ZIP VDB, published exclusively with cleanup on failure.
Unicode paths are supported. The limits are conservative logical payload/work
budgets, not a hard process-RSS or hostile-file heap sandbox; legacy metadata
decoding precedes its size validation. Existing outputs are never replaced.

In the authoring panel, choose **Rebuild closed contour to SDF**. Set the source
grid, isovalue, asymmetric target-voxel widths and optional target matrix. The
matrix editor shows its first three rows with translation in the final column;
the final affine row stays fixed. Rebuild work, catalog/metadata and common
workspace/voxel/triangle limits are available in the budget controls. Submitted
options are captured by value; later edits affect the next request.

### Volume sphere fitting

`FitVolumeSpheres` and `ExportVolumeSpheres` in `OpenVdbVolumeSpheres.h` expose
OpenVDB's approximate interior sphere fitting for native Float/Double SDFs and
[0,1] fog:

```powershell
.\OpenVdbTool.exe --volume-spheres surface.vdb spheres.json --sphere-max-count 50 --sphere-candidates 2048
.\OpenVdbTool.exe --volume-spheres fog.vdb spheres.json --iso .5 --sphere-min-radius .01 --sphere-max-radius .2
```

The exclusive UTF-8 version 1 JSON records the selected source name/type,
voxel size, isovalue, activity count, typed grid/file metadata summaries,
approximation diagnostic and `{position:[x,y,z],radius}` records in world
units. Unknown metadata is explicitly opaque; invalid UTF-8 is rejected.
The panel exports this JSON without treating it as a renderable volume.

Counts are bounded to 1..4096 spheres and 1..100000 scatter candidates. The
fixed native seed and radius-descending/xyz sort give repeatable output.
Overlapping spheres are opt-in. World radius limits apply after fitting:
maximum shrinks spheres and minimum discards them. An explicit minimum count
that cannot be met after filtering fails without creating output. The result
does not guarantee optimal packing, coverage or exact geometric containment.

Fog first rebuilds its selected isosurface to a distance field, avoiding
upstream fog scattering over active cells outside the selected interior.
SDFs require a symmetric narrow band of 3..64 voxels. Maps require uniform
orthogonal affine axes in [1e-6,1e6]. Unit distances and recentered integer
indices avoid native Float loss at remote world origins; unsuitable Float
object extents or unresolved world centers fail explicitly. Input bytes and
metadata are unchanged. Encoded sources, delayed/full native tree loading,
metadata, adaptive rebuilds, mesh/candidate workspace and both executed work
stages share preflight caps. Default `--max-sphere-work` is eight billion
conservative work items; voxel/byte limits and sphere catalog/metadata caps
also apply. The focused suite passes sixteen native feature cases and its
disabled availability case.

### Fast sweeping and field extension

`WriteFastSwept` and `WriteFastExtendedField` in `OpenVdbFastSweeping.h` expose
native fog/SDF conversion, reinitialization, distance-band dilation, mask
extension, topology-to-distance and scalar/vector field extension:

```powershell
.\OpenVdbTool.exe --fast-sweep fog.vdb distance.vdb --sweep-operation fog
.\OpenVdbTool.exe --fast-sweep surface.vdb distance.vdb --sweep-operation reinitialize --sweep-iterations 2
.\OpenVdbTool.exe --fast-sweep surface.vdb wider.vdb --sweep-operation dilate --sweep-dilation 2 --sweep-neighbors face-edge
.\OpenVdbTool.exe --fast-sweep surface.vdb extended.vdb --sweep-operation mask --sweep-mask activity.vdb
.\OpenVdbTool.exe --fast-sweep occupied.vdb surface.vdb --sweep-operation topology --topology-half-width 3
.\OpenVdbTool.exe --fast-extend surface.vdb field.vdb extended.vdb --sweep-side greater
```

Float/Double scalar distance operations retain native precision. Topology
conversion accepts activity from all nine nonpoint native grid types and
produces Float distances. Activity-mask values are ignored, including active
false/zero/NaN. Mask distance extension unions source and mask activity;
restriction masks for reinitialization, fog and field extension intersect
source activity while retaining its inactive stencil values. Dilation and
topology conversion reject restriction masks. Active fog/topology tiles need
explicit `--sweep-voxelize-tiles`; active SDF tiles are unsupported.

Field extension accepts Float with Float/Vec3S or Double with Double/Vec3D
collocated world fields. It writes distance named `--grid-name` (default
`surface`) plus an extension grid with `_extension` appended. Boundary samples
use native trilinear interpolation including inactive/background values.
`--sweep-side greater|less` retains original field values on the excluded side,
including boundary cells. The field's grid metadata is retained; field-file
metadata is not merged into the source archive. MAC, local/normalized-vector,
integer and mixed-precision extension are rejected by this workflow.

CLI fog isovalue defaults to .5 and SDF isovalue to zero. There are 1..32
eight-direction iterations, three dilation neighborhoods, one-sided growth
and topology width/closing/dilation/smoothing controls. The panel supplies two
operations for distance conversion and field extension. Source/field/mask
maps must match exactly with uniform orthogonal axes in [1e-6,1e6]. Unit-map
conditioning retains exact output coefficients; unresolved scalar contrast,
distance underflow, missing crossings and unreachable disconnected domains
fail before publication. Fixed iterations yield approximate upwind/PDE
distances without certifying convergence, closure or a uniform closed band.
Conservative full-domain/tree/buffer/work caps can reject remote sparse
layouts. Native paths, owned snapshots, delayed full-memory preflight,
exclusive ZIP output and failure cleanup are covered by seventeen passing
native feature cases and disabled-build availability.

### Region and cavity masks

`WriteRegions` in `OpenVdbRegions.h` writes true-valued Bool occupancy masks
with inactive false background. These operations distinguish active states,
signed interiors and positive enclosed voids:

```powershell
.\OpenVdbTool.exe --regions density.vdb components.vdb --region-operation components --grid-name occupied
.\OpenVdbTool.exe --regions surface.vdb interior.vdb --region-operation interior
.\OpenVdbTool.exe --regions surface.vdb enclosed.vdb --region-operation enclosed
.\OpenVdbTool.exe --regions surface.vdb cavities.vdb --region-operation cavities --region-isovalue 0
```

Components use face/six-connected active states across Float, Double, Bool,
Mask, Int32, Int64, Vec3I, Vec3S and Vec3D grids. Active false, zero and NaN
values are occupied; PointDataGrid uses the separate point-mask route. Active
tiles are voxelized only after domain/node/workspace checks. Components sort
by descending count and index bounds, with names such as `occupied_000`.
An empty source writes an explicitly diagnosed empty `_000` mask.

Signed operations require Float/Double LEVEL_SET grids with finite distances,
positive exterior background, symmetric inactive signed backgrounds and no
active tiles. `--region-isovalue` must be exactly representable in the native
type and strictly inside its background band. Interior is the strict
`value < isovalue` set, including inactive negative tiles and voxels. Enclosed
regions also include positive cells unreachable from the exterior; cavities
subtract the signed interior from that set. A bounded finite-domain
six-neighbor exterior flood replaces the SDK's unbounded convergence tracer.
Open face-connected positive passages remain exterior. This is voxel topology,
not a watertight-mesh certificate or physical solid-component segmentation.

`--max-region-components` accepts 1..1024, defaults to 64 and applies only to
components. Isovalue is accepted only for signed operations. Work defaults to
200 million via `--max-region-work`; `--max-region-grids` and
`--max-region-metadata` bound scanned descriptors and aggregate metadata.
The common voxel/byte limits include the full prospective domain, stored
leaves, queues, masks and per-component copies. Full boxes are deliberately
conservative for remote sparse inputs. Exact affine maps and custom grid/file
metadata survive; structural tags become unknown-class world masks. Outputs
are exclusive full-precision ZIP files. The native suite has sixteen feature
cases. Value-preserving component exports and fill-mask constraints remain
outside this mask-only workflow.

### Value-preserving active components

`WriteActiveValueComponents` and `--value-components` write one grid per
six-connected active component in index space. Bool, Mask, Int32, Int64, Float,
Double, Vec3I, Vec3S and Vec3D are supported. Active false, zero and nonfinite
values participate; inactive values do not. For every non-Mask output, the
entire source field's values, background and inactive continuations remain
unchanged. Other components become inactive while retaining their values. Mask
has activity alone. Source class/vector/world tags remain labels of the
unchanged field; the outputs do not certify separate solids, valid component
SDFs, or density/mass partitions.

```powershell
OpenVdbTool --value-components input.vdb components.vdb --grid density --component-prefix part --max-components 16
```

Components sort by their minimum lexicographic XYZ coordinate and receive
`part_000001`, `part_000002`, ... names. Empty activity writes one inactive source
copy named `part_000000`, with `activeComponentCount=0` and `emptySourceCopy=true`.
Exact native maps, custom grid/file metadata and stored value bits survive.
Names/statistics are regenerated and the five reserved boundary-projection
provenance keys are removed. No distance repair, interpolation, frame conversion
or value pruning occurs. Select an exact listed output grid before reusing the
result in another workflow.

The bounded adaptation uses a native label tree and explicit six-neighbor
traversal. Checked active-tile expansion and all component counts precede output
cloning. Source/expanded trees, label/queue/sort storage, all output clones and
writer scratch share byte, node, work and metadata caps. Full source trees can be
duplicated per component. The CLI exposes `--max-components` (1..64),
`--max-component-active-voxels`, `--max-voxels` (each source/expanded tree),
`--max-component-output-voxels` (aggregate output), `--max-component-nodes`,
`--max-component-work`, `--max-component-grids`, `--max-component-metadata`,
`--max-input-bytes` and `--max-output-bytes`. The bounded JSON report deducts its
own retained bytes and formatting work before the native call. Delayed loading
is required; native eager decoding still precedes post-decode checks. Exclusive
Unicode output uses the shared checked-ASCII scratch route. Existing inputs and
foreign destination files are preserved.

### Active-value region queries

`QueryActiveRegions` in `OpenVdbRegionQueries.h` reads native active values
inside inclusive Int32 INDEX boxes. It supports Float, Double, Bool, Mask,
Int32, Int64, Vec3I, Vec3S and Vec3D without expanding sparse tiles:

```powershell
.\OpenVdbTool.exe --query-active-regions field.vdb --grid density --region -8 -8 -8 8 8 8
.\OpenVdbTool.exe --query-active-regions field.vdb --region 0 0 0 4095 4095 4095 --counts-only
.\OpenVdbTool.exe --query-active-regions field.vdb --region 0 0 0 7 7 7 --max-region-records 16 --max-region-total-records 16
```

Activity determines membership: active false, zero, infinity and NaN remain
records; inactive continuations do not. Every record has its original and
clipped index boxes, native tree level, exact UInt64 voxel multiplicity and
typed value. Integer JSON values remain exact. Float/Double scalar and vector
components use authoritative fixed-width bit hex, preserving signed zero and
NaN payloads. Vector components remain raw regardless of vector tags.

Matched and omitted counts are complete even when returned records are
truncated. Native depth-first child-table order determines each prefix, and
earlier input queries consume the aggregate record quota first. Overlapping
queries count independently. An empty batch still validates and censuses the
source. `--counts-only` cannot be combined with explicit record quotas.

`--max-region-records` and `--max-region-total-records` accept zero and default
to 4096 and 65536. Separate caps cover stored leaf slots (`--max-voxels`),
nodes/root entries, exact source-active voxel count, work, scanned grids,
queries and metadata. Compact active tile volume is independent of allocated
leaf storage. The joint byte cap includes encoded snapshots, typed native
buffers before paging, metadata and report capacity.

Names and metadata preserve native bytes. JSON byte envelopes expose valid
UTF-8 text when complete, exact hex prefixes, full byte counts and explicit
truncation. `--region-report-bytes` controls each displayed prefix. Metadata
records include their native UInt32 size prefix; serialized transforms remain
opaque SDK records. A finite affine COLUMN matrix is also reported when
available. Nonlinear maps permit index queries without implying a valid world
ROI or inverse map. PointData/custom trees are rejected when selected.

### Native CPU ray queries

`QueryLevelSetRays` and `QueryActiveVolumeRays` in `OpenVdbRayQueries.h` accept
WORLD origins, nonzero directions and Euclidean world-distance intervals.
Directions are normalized; `0 <= MIN < MAX` is required. Repeat `--ray` for a
batch, or omit it to validate the source without tracing:

```powershell
.\OpenVdbTool.exe --ray-intersections surface.vdb --ray 0 0 3 0 0 -2 0 6
.\OpenVdbTool.exe --ray-intersections surface.vdb --ray 0 0 3 0 0 -1 0 6 --ray-refinements 8 --ray-normals off
.\OpenVdbTool.exe --ray-volume-spans fog.vdb --ray -3 .25 .25 1 0 0 0 32 --max-ray-spans 1000
```

SDF queries require finite Float/Double LEVEL_SET values, a positive symmetric
3..64-voxel inactive band and no active tiles. The isovalue must lie at least
two voxels inside both band limits. Uniform orthogonal affine maps support
spacing 1e-6..1e6, rotations and reflections; general affine/nonlinear maps
are not supported. Root recentering and Double unit-distance work grids avoid
premature Float coordinate narrowing.

Native hierarchical traversal uses checked signs and scaled chord refinement.
Results distinguish misses, sampled zeros and bracketed crossings. These are
approximate first hits: tangencies or multiple roots between samples may be
missed, and zero plateaus do not certify isolated surfaces. The sampled
residual is diagnostic. A normal is available only with all eight original
corners active and a nondegenerate gradient; missing coverage, degenerate
gradients, misses and disabled normals have separate statuses. Invalid fields
are `null`. Normals point toward increasing phi in WORLD coordinates.

Volume queries use activity across all nine supported field types, including
active false/zero/nonfinite values. They return **coarse full 8³ active-leaf
blocks and active tiles**. A single active cell in leaf zero can produce an
axis span `[-.5, 7.5]`; this is not an exact occupied-cell interval or density
integral. Inactive-only leaves are pruned, touching blocks merge, and the
centered-cell convention is explicit. Native grazing intervals at most
1e-9 index-distance units are omitted. All spans are returned or the query
fails; there is no truncated-success span list.

`--max-rays`, `--max-ray-grids`, `--max-ray-work`, `--max-ray-metadata`,
`--max-voxels` and `--max-input-bytes` jointly bound the source, working trees,
finite traversal estimates and report. `--max-ray-spans` applies only to
volume queries; `--ray-isovalue`, `--ray-refinements` (0..8) and
`--ray-normals on|off` apply only to SDF queries. Lowering the prospective span
cap also lowers the CLI's reserved report bytes and formatting work.

Both query families leave source bytes unchanged and write no model or file.
Mandatory delayed loading and logical preallocation limits do not constitute
a hostile-file allocator/RSS sandbox: eager native metadata/topology decoding
can precede estimates. CLI and authoring integration validation is recorded in
the feature ledger as each shared build completes.

### Nodal fog density and Beer ray integrals

`OpenVdbFogRayIntegrals.h` exposes read-only `IntegrateFogRays`. The Tool accepts
repeatable eight-number WORLD ray records:

```powershell
.\OpenVdbTool.exe --integrate-fog-rays density.vdb --grid density --ray -1 0 0 1 0 0 0 2
.\OpenVdbTool.exe --integrate-fog-rays density.vdb --ray -1 0 0 1 0 0 0 2 --extinction 2 --transmission-policy allow-opaque-underflow
```

Each ray is origin XYZ, direction XYZ, minimum distance and maximum distance.
Directions normalize; distances are Euclidean WORLD lengths. Native Float/Double
FOG or UNKNOWN scalar fields use nodal trilinear values at integer indices.
Every inactive value and the background participates independently of activity.
An activity-empty positive background is a constant medium, not vacuum. All
stored values must be finite and nonnegative; no normalization or density ceiling
is imposed. Uniform orthogonal native maps span h1e-6 through h1e6, including
representable rotations/reflections.

Exact rational cell events and independently retained barycentric complements
avoid epsilon holes, merged near-ties and prematurely rounded tiny products.
Positive Simpson sums integrate the fixed cell cubic; the represented cached
native inverse and normalized direction remain approximate. The full stored
non-background support includes inactive holes/tiles and a one-index nodal halo.
Background over outer intervals is added directly, avoiding cancellation. This
is a density/Beer query, not exact occupancy, scattering or a claim of parity
with the dense transport preview's different boundary convention.

The typed JSON includes complete native metadata text or byte hex, the original
frame, normalized directions, `densityIntegral`, `opticalDepth`, `transmittance`,
`absorbedFraction`, actual/prospective cell counts and transmission status. Thin
absorption uses `-expm1(-tau)` even when transmission rounds to one. Exact nonzero
I/tau outside the positive Double range rejects. Exponential underflow rejects
by default; the explicit opaque policy returns zero transmission with an
`opaque-underflow` status for finite tau. Extinction zero still validates and
computes the public density integral.

All ray/catalog/metadata/work/input/voxel caps and the two cell-segment caps are
exposed in help. Actual report and caller capacities are deducted from the same
native allowances. Sparse wide support can conservatively exceed prospective
cell-work bounds; native tiles are not expanded. No output is created, no partial
JSON is emitted on failure, and an empty ray batch still validates the source.
Checked ASCII private SDK scratch supports Unicode source and TEMP paths.
Twenty-one main native cases and nine actual CLI cases pass. The read-only
`Integrate fog rays` authoring operation snapshots source, selector, rays and
limits, reserves display/caller capacity before execution, and displays bounded
metadata byte prefixes plus the first 64 completed results. Every submitted ray
is computed or the whole operation fails; display limits never truncate native
queries. No output, model or restart action is offered, including after draft
changes or custom executors. Seven new panel cases bring the complete suite to
92 cases in the rebuilt 101-choice application. Current 107 enabled/103 OFF groups proof is
in `out/openvdb-validation/particle-fluid-fog-milestone.json`.

### CPU SDF surface-reference images

`RenderSurfaceReference` in `OpenVdbSurfaceReference.h` uses native perspective
or orthographic cameras, one checked SDF ray batch and native constant matte,
world-normal or two-sided headlight shading. Each pixel uses one deterministic
center ray. Geometry, depths, residuals and normal statuses retain Double
precision for Float and Double sources; native shader RGB is Float, promoted
with an explicit rounding audit. Headlight response is
`color * abs(dot(normal, rayDirection))`; world-normal shading is
`.5 * color * (normal + 1)`. These are surface diagnostics, without shadow rays,
fog, scattering, material grids or uncertainty estimates.

```powershell
OpenVdbTool --surface-reference sphere.vdb normal.exr --width 32 --height 32 --surface-shader normal --ray-max 10
OpenVdbTool --surface-reference sphere.vdb silhouette.exr --projection orthographic --ortho-height 3 --surface-shader matte --ray-normals off
```

`--eye X Y Z`, `--target X Y Z` and `--up X Y Z` configure the camera.
`--fov`/`--vertical-fov` require perspective projection;
`--ortho-height`/`--orthographic-height` require orthographic projection.
`--color`, `--background` and `--invalid-normal-color` each accept three [0,1]
components; the last option requires normal or headlight shading. Aliases share
duplicate checks. `--grid`/`--input-grid`, the SDF ray options and all shared
ray/source caps apply. `--max-pixels` adds an image cap and `--max-export-bytes`
adds an independent export cap. Surface reference defaults to 2B ray-work items
(ordinary ray queries retain 200M). The manually bounded JSON summary charges its
retained capacity and formatting/status-scan work before native rendering,
then formats all variable native fields before file publication. Pixel data
resides in the EXR.

Camera eye/target/up are WORLD vectors. Perspective vertical FOV is
0.1..150 degrees; orthographic height is a positive WORLD length. The public ray
minimum/maximum are radial Euclidean WORLD distances, independent of the native
camera's view-depth planes. Origin conditioning and an independent basis check
protect camera construction. Width/height are each 1..2048, with at most 262144
pixels and the separate ray-count cap (default 10000). The source/map/band and
approximate intersection limits from the ray-query section apply unchanged.

`WriteSurfaceReferenceExr` writes an exclusive Unicode-safe uncompressed linear
FLOAT EXR with eleven channels: `R`, `G`, `B`, `A`, `Z`, `N.X`, `N.Y`, `N.Z`,
`N.VALID`, `N.STATUS`, `HIT.KIND`. `A` is hit coverage, and `Z` is radial WORLD
depth. Miss depth and unavailable normals use zero placeholders; the status
channels determine validity. `N.STATUS` codes are 0=not requested, 1=no hit,
2=available, 3=missing active footprint, 4=degenerate gradient. `HIT.KIND` codes
are 0=miss, 1=sampled zero and 2=bracketed crossing. Requested misses must carry
no-hit status; disabled normals always carry not-requested status. A hit without
a usable normal remains a hit. Normal-dependent modes show the configurable
invalid-normal color (default magenta), rather than inventing a normal.

Film, ray/image capacities, camera/shader work and the retained native result
share the render's byte/work caps before native traversal. Export additionally
charges retained results, planar buffers, TinyEXR/header/metadata and encoded
bytes against the smaller of the requested export cap and the image's retained
cap. Caller-constructed images undergo the same status/counter/coverage checks.
Complete validation and serialization precede publication. Float overflow or
conversion of a nonzero value to zero rejects; other rounding is reported.
Both APIs report OpenVDB unavailability before options or paths in an OFF build.

### Approximate closest-surface queries

`QueryLevelSetSurface` in `OpenVdbSurfaceQueries.h` queries OpenVDB's extracted
surface point cloud. Returned positions and nonnegative world distances are
approximate, including queries outside the narrow band:

```powershell
.\OpenVdbTool.exe --surface-query surface.vdb --position 1.2 0 0 --position 3 0 0
.\OpenVdbTool.exe --surface-query surface.vdb --grid descriptor:surface
```

Repeated `--position X Y Z` values form a bounded query batch. An empty batch
returns validated metadata without building the surface index. JSON reports
`querySdfValid` and `geometryValid` independently. Signed-distance interpolation
requires all eight source corners active. Normal and curvature estimates are
evaluated at the returned approximate position and require the full native
19-point stencil at all eight interpolation corners and nonzero gradients.
Unavailable JSON fields are `null`. Normals point toward increasing distance;
an outward sphere has mean curvature `+1/r` and Gaussian curvature `+1/r²` in
inverse world units and inverse squared world units respectively.

Inputs are Float/Double LEVEL_SET grids with a symmetric 3..64-voxel inactive
band, no active tiles and a resolved crossing. Uniform orthogonal affine axes
must lie in [1e-6,1e6]. Unit-distance/index recentering preserves remote origins,
but native Float point coordinates must still resolve 1e-4 source voxels;
unresolved extents or distant queries fail explicitly. These are point-cloud
queries, not exact triangle or implicit-surface projections, and small grids
can have poorer native accuracy. No adaptive resampling is performed.

`--max-surface-queries` defaults to 10000 and allows up to one million.
`--max-surface-work`, `--max-surface-grids`, `--max-surface-metadata` and common
voxel/byte caps reserve source buffers, the surface index, query output and
worst-case comparison work. Typed metadata summaries mark unknown payloads
opaque. Source bytes/maps are unchanged. Eight native feature cases cover
planes, spheres, torus curvature, footprints, transforms, precision and limits.

### Surface-cell curvature, normals and closest-point fields

`WriteSurfaceFields` in `OpenVdbSurfaceFields.h` writes one derived field and
one true-valued Bool validity grid. Unlike `QueryLevelSetSurface`, these fields
are evaluated at the original source voxel centers. They do not query an
extracted point cloud or evaluate geometry at a projected position.

```powershell
.\OpenVdbTool.exe --sphere surface.vdb --radius 1 --voxel-size .125
.\OpenVdbTool.exe --surface-fields surface.vdb mean.vdb --surface-field-operation mean --grid-name mean_curvature --surface-validity-name mean_valid
.\OpenVdbTool.exe --surface-fields surface.vdb gaussian.vdb --surface-field-operation gaussian --grid-name gaussian_curvature
.\OpenVdbTool.exe --surface-fields surface.vdb normals.vdb --surface-field-operation normal --grid-name surface_normal
.\OpenVdbTool.exe --surface-fields surface.vdb closest.vdb --surface-field-operation closest-point --surface-field-double --grid-name closest_world
.\OpenVdbTool.exe --inspect mean.vdb --input-grid descriptor:mean_valid
```

`--surface-field-operation` is required. Mean curvature has inverse-world-length
units; Gaussian curvature has inverse-squared-world-length units. For an
outward sphere sampled at a cell of radius `rho`, the centered-difference
estimates approach `+1/rho` and `+1/rho^2`. They are evaluated at that cell,
rather than automatically at the sphere's zero surface.

Normals are unit increasing-phi world gradients and retain native
`VEC_COVARIANT_NORMALIZE`/world metadata. Closest-point-transform (CPT) values
are absolute world positions with `VEC_CONTRAVARIANT_ABSOLUTE`/world metadata.
Native CPT uses the local approximation `x - phi * gradient(phi)` with the
**unnormalized** gradient. It is not an exact triangle/implicit-surface nearest
point or a Newton solve; poor signed-distance quality changes the result.
All outputs have UNKNOWN class. Normal and position fields must not be
reinterpreted as velocity merely to fit a visualization route.

The validity grid is authoritative. Normals/CPT require active center and six
face neighbors; curvature additionally requires the twelve edge neighbors.
Cube corners and two-cell axial neighbors are not part of these footprints.
The unit-space gradient length must exceed `1e-7`. Invalid cells and both
backgrounds are inactive zero, including CPT's otherwise ambiguous zero
position. A valid zero-curvature cell remains active. JSON separates missing
footprints from degenerate gradients and reports both output grids' names,
types, active counts and voxel spacing. `maximumUnitGradientError` is `null`
when `gradientSampleCount` is zero; otherwise it reports maximum
`abs(length(gradient) - 1)` over complete footprints, including degenerate
ones. This is a local quality diagnostic, not a distance/closure certificate.

Inputs are Float/Double positive-exterior world/invariant LEVEL_SET grids,
finite inside a symmetric 3..64-voxel signed inactive band, with no active
tiles. Nonempty sources need a represented sign crossing. True empty
positive-background-only inputs yield diagnosed empty outputs; hidden negative
inactive topology without an active surface rejects. Uniform orthogonal affine
axes in `[1e-6,1e6]` support rotations/reflections. Original map coefficients
and custom grid/file metadata are retained, while structural tags/statistics
are regenerated.

Double unit-distance/index conditioning precedes native GridOperators and
CurvatureStencil evaluation. Curvatures are restored with `Hunit/h` and
`Kunit/h^2`. Float input normally writes Float/Vec3S; Double writes
Double/Vec3D. `--surface-field-double` promotes Float results. Meaningful
nonzero conditioning/storage range loss rejects. Absolute CPT spacing must
resolve `1e-4` source voxels; promote Float output or recenter remote sources
when inadequate. Derivative fields are translation independent and need no
representable absolute output positions.

`--grid` and `--input-grid` are aliases; supplying both is a duplicate. Use
`descriptor:<exact descriptor>` for unique native selection. `--grid-name`
defaults to `surface`; `--surface-validity-name` defaults to `surface_valid`.
Output names must be distinct and 1..256 bytes. Common `--max-voxels` and
`--max-input-bytes` apply. The additional limits are:

| Flag | Default | Range/meaning |
|---|---:|---|
| `--max-surface-field-work` | 200000000 | 1..10000000000, shared traversals/native work |
| `--max-surface-field-grids` | 256 | 1..4096, all scanned descriptors |
| `--max-surface-field-metadata` | 1048576 | Positive byte cap, file and scanned-grid metadata |

These bounds reserve the encoded snapshot, catalog, full typed buffers before
paging, conditioned/native/output/validity trees, conservative full boxes,
metadata replicas and writer storage. Prospective output leaf slots are shared
by both grids. Sparse distant regions can fail the conservative box cap.
Delayed loading must be enabled; `OPENVDB_DISABLE_DELAYED_LOAD` rejects.
Metadata/legacy decoding can allocate upstream before its checks, so these are
logical workspace limits rather than hostile-file process-RSS guarantees.
One worker is used. Unicode paths use private native snapshots; output is a
new full-precision ZIP VDB. Existing outputs and source bytes remain intact;
failure removes only owned partial output. Generation/filter options such as
voxel size, half-width and triangle limits are not silently accepted here.

### Fracture and rigid cutter instances

`WriteFracturedLevelSet` in `OpenVdbFracture.h` performs deterministic bounded
CSG cuts and writes residuals and fragments to one new VDB:

```powershell
.\OpenVdbTool.exe --fracture surface.vdb cutter.vdb fragments.vdb --grid-name piece
.\OpenVdbTool.exe --fracture surface.vdb cutter.vdb instances.vdb --fracture-instance 0 0 0 0 0 0 --fracture-instance .25 0 0 0 0 .1 --fracture-pivot 0 0 0
```

Each `--fracture-instance TX TY TZ RX RY RZ` specifies world displacement and
world X-then-Y-then-Z rotations in radians, each within [-2π,2π]. A following
`--fracture-pivot` sets that instance's world pivot; the default pivot is the
exact source affine origin. No instance list means one identity cut. Later
instances also cut earlier fragments by default; `--fracture-disjoint-cutters`
keeps those earlier fragments outside subsequent cuts. `--grid` and
`--cutter-grid` select the two input grids, including exact descriptor selectors.

Native Float/Double source and cutter must match type, exact affine
coefficients and backgrounds. They require finite signed narrow bands of
3..64 voxels, uniform orthogonal axes in [1e-6,1e6], invariant world scalar
semantics and no active tiles. Finite-grid positive-boundary/sign guards reject
malformed fields without certifying manifold closure. Integer index translations
use exact native nearest samples; other rigid instances use per-point Double
inverse mapping and trilinear samples. CSG/resampling gives approximate
distances, without automatic reinitialization or exact continuous-volume claims.

JSON records each piece's role, creating instance and negative-voxel count.
The partition `inputNegativeVoxelCount = outputNegativeVoxelCount +
cutBoundaryVoxelCount` is checked after each cut and in the final output.
Represented small fragments are retained rather than applying the SDK's
27-active-voxel discard heuristic. Default unsegmented output retains nested
cavities. `--fracture-segment` uses zero-surface segmentation and rejects
negative-exterior cavity components; it does not supply cavity-safe solid labels.

`--max-fracture-pieces` defaults to 64 and accepts 1..256 total pieces;
`--max-fracture-work` and common voxel/byte caps reserve instances, growth,
segmentation, metadata, output trees and writer workspace before cutting.
Source maps/custom grid/file metadata survive; cutter file metadata is not
merged. Native-path snapshots and exclusive ZIP publication preserve both
inputs and existing outputs. Extremely distant-origin/small-step SDK map-copy
combinations can reject before publication. The twelve native feature cases
include whole/disjoint cutters, rigid instances, small fragments, cavity
behavior, exact maps and source/output protection.

### Ordinary field compositing and value transforms

`OpenVdbFieldComposite.h` combines ordinary Float/Double/Vec3S/Vec3D grids with
the same exact native type, semantic tags and affine map coefficients. It does
not resample mismatched maps. SDF Boolean operations use the separate CSG route;
MAC/index vectors and normalized covariant vectors are rejected here.

```powershell
.\OpenVdbTool.exe --composite-fields a.vdb b.vdb sum.vdb --field-operation sum --grid-a density --grid-b density
.\OpenVdbTool.exe --composite-fields a.vdb b.vdb blend.vdb --field-operation blend --blend-weight 0.25 --field-activity intersection
.\OpenVdbTool.exe --transform-field field.vdb scaled.vdb --value-operation affine --value-scale 2 2 2 --value-offset 1 0 -1 --value-scope all
```

Binary operations are `sum|product|divide|min|max|blend|merge-active`. Arithmetic
applies to the entire logical field, including stored inactive values and the
infinite background. Division requires every denominator component, including
the background, to be nonzero. Activity is selected independently with
`union|intersection|left|right`; merge-active fixes union activity and copies
the preferred active operand, otherwise the other active operand, otherwise A.
`--merge-preference left|right` only applies to merge-active. Vector min/max can
use `--vector-extrema magnitude` to choose whole vectors by scaled norm rank;
componentwise comparison is the default. Blend endpoints copy native value bits.

Unary operations are `copy|affine|absolute|clamp`. The default scope `active`
preserves all inactive/background bits; `all` transforms them too. Scalar affine
parameters use X with Y/Z at their defaults. Clamp uses `--value-min X Y Z` and
`--value-max X Y Z`, rounding bounds inward to representable native values.
Unused operation-specific controls are rejected. Arithmetic rejects nonfinite
operated values, range loss and negative fog results; copy/merge retain payloads.
Source A metadata survives with regenerated output statistics. Topology is retained
without pruning or dense bounding-box expansion. `--max-field-work` and shared
input/voxel/byte limits cover both operands, output and writer workspaces.
Outputs are exclusive new ZIP VDB files.

### Scalar and vector calculus

`OpenVdbVectorProcessing.h` exposes `WriteDifferential()` and `SampleVector()`.
Gradient and Laplacian take Float/Double scalar grids; divergence, curl,
magnitude and normalization take collocated world-space Vec3S/Vec3D grids.
`--sample-vector` returns a trilinear world-position sample as a JSON array.

```powershell
.\OpenVdbTool.exe --gradient particles_fog.vdb gradient.vdb --input-grid density --grid-name gradient
.\OpenVdbTool.exe --curl gradient.vdb curl.vdb --input-grid gradient --grid-name curl
.\OpenVdbTool.exe --normalize gradient.vdb direction.vdb --input-grid gradient --grid-name direction
.\OpenVdbTool.exe --magnitude gradient.vdb slope.vdb --input-grid gradient --grid-name slope
.\OpenVdbTool.exe --sample-vector direction.vdb 0.4 0 0 --input-grid direction
```

Centered second-order derivatives use world units, including under bounded
affine rotations, reflections and shear. Output retains source precision,
transform and custom metadata, with unknown grid class. Vector components are
already world-space numbers; semantic tags describe later transform behavior.
All world-space vector tags, including covariant gradients, are accepted;
index-space and staggered vectors are rejected. Gradient/curl use OpenVDB's
covariant tag, and scalar outputs are invariant.

Operations evaluate the source active topology. Inactive values participate in
derivative stencils, and derivatives may change near active-domain boundaries.
Inactive output values become the operation's constant-background result.
Derivatives densify active tiles after preflight; magnitude and normalization
keep them sparse. Exactly zero vectors remain zero, while tiny nonzero vectors
retain their normalized direction. Input axes must span 1e-6 to 1e6 world units
with safe conditioning, and stored component magnitudes are limited to 1e12.
Sampling includes inactive values/background and uses the selected grid's own
transform. Source index coordinates must resolve increments of 1e-8 voxels in
Double; remote grid origins require recentering if they lose finer fractions.
Nearest scalar sampling accepts both Float and Double grids. Scalar/vector
queries ignore generation and output options. Input, expanded topology, metadata and serial workspace share the
processing budgets; generation/resampling options do not change the source grid.

### Scalar Poisson solve

`OpenVdbPoisson.h` exposes `WritePoissonSolution()`. `--poisson` solves the
discrete world-space equation `Laplacian(u) = rhs` on the input scalar grid's
active cells. Exterior cells have fixed zero Dirichlet values. It uses the
nearest-neighbor seven-point stencil, with the physical voxel spacing included.

```powershell
# Use existing density values as an example scalar right-hand side.
.\OpenVdbTool.exe --poisson particles_fog.vdb potential.vdb --input-grid density --grid-name potential --max-iterations 200 --tolerance 1e-5 --max-solver-voxels 100000
.\OpenVdbTool.exe --inspect potential.vdb --stats --input-grid potential
```

Float/Double inputs require uniform orthogonal affine voxels from 1e-6 to 1e6
world units; rotations and reflections are supported. Stored values and
background must be finite, but inactive/background values do not enter the
right-hand side. Active tiles are counted before bounded expansion. Empty or
zero RHS produces an exact zero solution. Output retains scalar precision,
transform and custom metadata, with unknown class and zero background/inactive
values. The source remains unchanged.

`--max-iterations` accepts 1 to 10,000, default 200; `--tolerance` is a relative
infinity-norm residual from 1e-12 to 1e-2, default 1e-6. `--max-solver-voxels`
cannot exceed one million. Recorded metadata, actual topology and conservative
matrix/preconditioner/vector/tree workspace limits apply before solving, so the
memory budget may permit fewer unknowns. The reported residual is independently
measured after rounding to the stored output type. Nonconvergence or an
unrepresentable/tolerance-violating result writes no output; very tight Float32
tolerances may require a DoubleGrid input or a looser tolerance. RHS dynamic
ranges that would erase a nonzero term during normalization are rejected;
reduce the range or solve disconnected regions separately. This scalar PDE
operation is a building block for fluid work; the separate projection workflow
below adds forces and incompressibility. Complete simulation stepping remains
pending in the completion ledger.

### Fixed-domain potential flow

`WritePotentialFlow` in `OpenVdbPotentialFlow.h` computes irrotational exterior
flow around a signed collider. It is a boundary-driven scalar-potential solve,
not pressure projection of an arbitrary input velocity or a fluid timestep:

```powershell
.\OpenVdbTool.exe --potential-flow-capabilities
.\OpenVdbTool.exe --potential-flow body.vdb flow.vdb --far-field 1 0 0 --collider-velocity 0 0 0 --exterior-width 6
.\OpenVdbTool.exe --potential-flow body.vdb named.vdb --velocity-name wind --potential-name phi --domain-name exterior --neumann-name wall --max-iterations 1000 --tolerance 1e-6
```

The fixed domain is an 18-neighbor dilation of the strict represented negative
interior minus that interior. `--exterior-width` is a topological thickness,
not a Euclidean distance, and must exceed `ceil(source band half width)+1`.
At collider Neumann cells the projected perturbation is
`(normal dot (colliderVelocity - farFieldVelocity)) * normal`. Other missing
faces use zero perturbation potential, providing a finite outer Dirichlet
boundary. Every connected domain component must contain an actual Dirichlet
face; pure-Neumann enclosed cavities reject. Spatial boundary-velocity grids,
compatible closed-component gauges and arbitrary caller domains remain open.

Inputs are Float/Double positive-exterior LEVEL_SET grids with finite distances,
a symmetric 3..64-voxel inactive band, no active tiles and a resolved crossing.
Uniform orthogonal affine axes in [1e-6,1e6], rotations/reflections and exact
original map coefficients are supported. Integer recentering and Double
unit-distance conditioning precede native operators. The world velocity
difference is formed before rotation/normalization. Large common velocities
whose stored Double total-flow spacing cannot resolve nonzero relative motion
at the requested tolerance reject; exact co-motion retains literal world flow
with zero perturbation.

One VDB contains world Vec3D `flow`, Double physical `potential` (length²/second),
Bool `flow_domain` and world Vec3D projected perturbation `neumann`. The four
name options in the example override these defaults. Outside/inactive flow is
the far-field velocity, while potential/Neumann backgrounds are zero. Native
boundary substitution drops the tangential potential derivative. The full
stored-potential seven-point mixed-boundary residual is checked independently;
all component/solver attempts share the iteration cap. Normal error is checked
in world units. Interior central divergence/curl are diagnostics only on
complete two-ring stencils, with availability/count fields and JSON `null`
when absent; they do not establish MAC incompressibility or infinite-domain
analytic convergence.

`--max-solver-voxels` defaults to 100000 (maximum one million),
`--max-iterations` to 1000 and `--tolerance` to 1e-6. `--max-flow-work`,
`--max-flow-grids`, `--max-flow-metadata` and common input/voxel limits reserve
the full interior/domain, decoded buffers, gradients, matrices/Krylov state,
metadata and writer storage before expansion/solving. Limits are conservative
for sparse layouts. Custom grid/file metadata and source bytes survive;
nonconvergence/precision failures leave no public output. Thirteen native
feature cases cover zero/co-motion, sphere flow, Galilean shifts, transforms,
remote indices, disconnected components, boundary rejection and budgets.
Capability discovery reports typed runtime availability, contracts and defaults.

### Native diagnostics and failure masks

`DiagnoseVdb` in `OpenVdbDiagnostics.h` returns a read-only typed report.
`WriteVdbDiagnosticMask` returns the same report and writes an exclusive Bool
union mask of finite located failures. These routes support nine native field
types and a separate PointData profile. Stored-values checks include inactive
values and background; optional component ranges are inclusive. The SDF profile
checks symmetric-band/class/map constraints with optional first-order or WENO
Godunov gradient diagnostics. The fog profile specifically checks normalized
density in [0,1]. PointData checks validate layout before handles and inspect P,
selected numeric columns and group-selected records, including inactive voxels.

```sh
OpenVdbTool --diagnose smoke.vdb --diagnostic-range 0 1000000
OpenVdbTool --diagnose surface.vdb --diagnostic-profile sdf \
  --diagnostic-gradient weno --diagnostic-footprint stored
OpenVdbTool --diagnostic-mask smoke.vdb failures.vdb \
  --diagnostic-profile fog --diagnostic-mask-name bad_density
OpenVdbTool --diagnose points.vdb --diagnostic-profile points \
  --diagnostic-attribute id --diagnostic-range 9007199254740992 9007199254740992
```

CLI controls include `--diagnostic-profile stored|sdf|fog|points`,
`--diagnostic-range MIN MAX`, field-only `--diagnostic-range-scope active|stored`,
SDF-only `--diagnostic-gradient none|first|weno`, unsquared
`--diagnostic-norm-range MIN MAX`, and `--diagnostic-footprint active|stored`.
`--diagnostic-tolerance` is an explicit absolute inactive/background tolerance
for SDF/fog. Repeat `--diagnostic-attribute` and the common point-group flags for
PointData; a point range requires chosen numeric columns. Common selectors,
byte/voxel caps, diagnostic work/catalog/metadata caps and
`--max-diagnostic-examples 0..128` apply. Zero examples disables examples.
Unrelated generation/point/gradient controls reject rather than being ignored.

Exit zero means a report was produced. Inspect `allRequestedPassed`, `complete`
and every check's Pass/Fail/Unavailable/NotApplicable status separately. Missing
or nonfinite derivative footprints are Unavailable and do not silently pass.
Native stored continuation is an explicit choice with separate counts. An empty
mask does not imply passing checks: background/schema/global failures have no
finite location, and unavailable footprints are not marked failures. Compact
failed tiles retain separate tile counts and semantic volume; point mask
locations use original stored voxels. No implicit repair, distance or closure
certificate is produced.

Range endpoints are parsed as finite Double values and printed. Plain int64
endpoints that Double cannot represent exactly reject. Integer source values
are compared exactly, so an Int64 value of 9007199254740993 fails the zero-width
interval at 9007199254740992. Native names/details use bounded UTF-8 text or null
plus exact byte-prefix hex, original length and truncation fields. Report
storage/serialization shares the byte/work caps before mask publication.
Delayed loading, full typed-buffer and metadata preflight, immutable Unicode
snapshots and exclusive output cleanup apply; these are logical limits rather
than a process-RSS sandbox. Input files remain unchanged.

### Closed-domain velocity projection

`OpenVdbFluidProjection.h` exposes `WriteProjectedVelocity()`. It adds a uniform
world-space acceleration, then solves a matching MAC pressure system on active
positive Float/Double or active true Bool domain cells. The domain must have a
zero background and finite nonnegative stored values. Uniform orthogonal
affine transforms, including rotation/reflection, are supported.

```powershell
.\OpenVdbTool.exe --project flow.vdb particles_fog.vdb projected.vdb --input-grid velocity --domain-grid density --time 0.041666666666666664 --acceleration 0 -9.81 0 --max-fluid-cells 100000 --tolerance 1e-6
.\OpenVdbTool.exe --inspect projected.vdb
# Its saved MAC velocity and domain can be used directly for another projection.
.\OpenVdbTool.exe --project projected.vdb projected.vdb projected_again.vdb --input-grid velocity --domain-grid fluid
```

Collocated input velocities are world-space Vec3S/Vec3D fields with independent
affine transforms. Native MAC input must exactly match the domain transform,
use `GRID_STAGGERED`, index-space contravariant-relative metadata, and negative
face storage: component `u_a(i)` is at `i - 0.5 e_a`. Output always uses this
local MAC representation in **voxels per second**; multiplying by the domain
Jacobian gives world velocity. Collocated component magnitudes are capped at
1e12, native MAC at 1e24. Successful MAC outputs obey the same cap and can be
projected again. Source samples must retain 1e-4 domain voxel precision.

Every exterior-domain normal face is zero. The six-neighbor Neumann graph
operator matches the face divergence and pressure correction. Each connected
component has its own pressure gauge and separately normalized solve, so a
weak disconnected flow is not lost beside a strong one. Physical pressure
assumes unit mass density and uses world-distance squared per second squared.
The full unpinned residual and stored MAC divergence are checked independently,
including gauge cells, after pressure is stored in Double.
The per-cell divergence limit is `tolerance * componentPeakDivergence +
8 * sum(ULP(six stored face values))`. This explicit storage-rounding allowance
handles nearly divergence-free circulation without an arbitrary cutoff for weak
flows. `divergenceRoundoffBound` reports the largest allowance and
`roundoffLimited` identifies results that need it; the pressure residual still
meets the requested relative tolerance independently.

The new VDB contains velocity, pressure and optionally the Bool domain. Output
names use `--velocity-name`, `--pressure-name` and `--domain-name`; `--no-domain`
omits the domain. JSON output reports cell/face/component counts, iterations,
residual and divergence before/after. Solver, topology and conservative memory
limits apply before allocation; nonconvergence/precision failures publish no
output. This workflow implements closed walls, uniform force and projection.
Open/free-surface boundaries and moving solids remain outstanding work.

### Pressure projection with explicit boundaries

`WriteBoundaryProjectedVelocity` and `WriteReprojectedBoundaryState` in
`OpenVdbBoundaryProjection.h` provide a fixed-domain, unit-density projection
separate from the closed smoke timestep. Direct inputs bind velocity, fluid and
optional solid/open/air/phi/wall/pressure grids independently. The fluid map is
finite uniform orthogonal with rotation/reflection; collocated world fields may
have independently conditioned affine maps. The solver checks compatible closed
wall flux, per-component gauges, full stored-pressure residuals and stored-face
divergence. All PCG attempts share the declared iteration/work limits.

Default output is seven grids: Double local MAC velocity, Double physical
pressure, Bool fluid, packed Vec3I face kinds, packed Vec3D effective fractions
and prescribed pressures, and local MAC wall normals. The packed channels are
invariant face tuples with dedicated schema/member metadata. Replay validates
all incident channels, activity, absent components, maps and member identifiers.
File metadata comes from the fluid source; new outputs are exclusive.


```
--project-boundaries VELOCITY.vdb FLUID.vdb OUTPUT.vdb
--reproject-boundaries STATE.vdb OUTPUT.vdb
--velocity-grid NAME    --fluid-grid NAME
--grid/--input-grid NAME                 Alias of --velocity-grid

# Direct only: every optional file has its own optional selector.
--boundary-solid FILE    --boundary-solid-grid NAME
--boundary-open FILE     --boundary-open-grid NAME
--boundary-air FILE      --boundary-air-grid NAME
--boundary-phi FILE      --boundary-phi-grid NAME
--boundary-wall FILE     --boundary-wall-grid NAME
--boundary-pressure FILE --boundary-pressure-grid NAME
--missing-boundary wall|open|air|surface  Default wall
--air-pressure center|surface           Requires boundary-air
--minimum-interface-fraction F           [1e-6,1], surface policy only
--wall-velocity X Y Z                    WORLD constant; excludes wall field
--ambient-pressure P                    Excludes pressure field

# Replay input selectors (canonical output names by default).
--face-kind-grid NAME --face-fraction-grid NAME
--face-pressure-grid NAME --wall-velocity-grid NAME

# Both commands.
--time-step SECONDS                      [1e-6,3600], positive
--acceleration X Y Z                     WORLD units/sec^2, <=1e12
--tolerance RELATIVE                     [1e-12,1e-2]
--iterations N                          [1,10000], all attempts per component
--max-fluid-cells N                      [1,1000000]
--velocity-name NAME --pressure-name NAME --fluid-name NAME
--face-kind-name NAME --face-fraction-name NAME
--face-pressure-name NAME --wall-velocity-name NAME
--no-boundary-state                      Write only velocity/pressure/fluid
--no-domain                              Requires no-boundary-state
--max-boundary-work N                    [1,10000000000]
--max-boundary-grids N                   [1,4096]
--max-boundary-metadata N
--max-voxels N --max-input-bytes N        Shared inputs/outputs/solver/report caps
```

Physical FACE pressure uses theta .5 (weight2), neighboring AIR CENTER pressure
theta1, and surface pressure the reported effective phi crossing fraction.
Clamping changes the effective pressure location. Cells/faces remain full-size;
there are no cut cells. Closed moving-wall flux must be compatible; it is never
repaired by subtracting a divergence mean. WORLD vector tags use raw physical
XYZ velocity components with no tag-driven transformation/normalization. Native
MAC fields and output use local negative XYZ faces in voxels/second.

Replay recomputes pressure and ignores the previous pressure grid. Nonuniform
prescribed pressure adds a FRESH impulse on EVERY call, even if the loaded
velocity is divergence free. Replay is not generally idempotent. Compare direct
and replay using the same loaded predictor. Geometric/constant BC controls are
rejected for replay; solve, dt, force and output controls remain applicable.

All output names are distinct1..256 UTF-8 bytes. Disabling an output rejects its
explicit name flag. Duplicate flags/aliases, irrelevant controls, invalid
operands and zero/overflowing counts fail. Literal `-h` paths require `./-h`.
Runtime disabled availability precedes path conversion and report reservation
after strict syntax validation. Reports use a bounded preallocated UTF-8 JSON
buffer and retain all numeric/Boolean native diagnostics as typed JSON. Native
diagnostic text is bounded to8 strings of1024 UTF-8 bytes, with truncation flags.


Canonical boundary membership survives explicit exact file copies and proven
identity resampling, unary Copy, no-op field advection and one-level passthrough
multiresolution routes. Derived values/topology invalidate the five exact
reserved boundary keys while retaining unrelated custom metadata. A new boundary
projection regenerates a consistent snapshot. An edited grid cannot retain a
claim that it belongs to the previous saved boundary table.

Pressure has world-distance-squared/second-squared units for unit density;
velocity and acceleration inputs use world units/second and world units/second
squared. This implementation does not advect geometry or a liquid interface,
provide cut-cell volumes, tangential no-slip, variable density, surface tension,
periodic/Robin boundaries or a complete open/free-surface timestep. Double affine
inversion/interpolation remain approximate; compensated velocity products retain
cancellation residuals without certifying arbitrary world-frame accuracy.

### Fixed-boundary scalar and velocity stepping

`OpenVdbBoundaryFluidStep.h` exposes `WriteBoundaryFluidStep`. It consumes six
explicitly selected canonical boundary members from the pressure workflow above,
required Float/Double fog density and optional ordinary Float/Double temperature.
The output contains fresh canonical seven-grid boundary state plus Double density
and temperature. A restart must bind all six boundary selectors and both scalar
selectors; a single-grid input cannot describe it. Old pressure values are
ignored and recomputed. Twenty-five main native feature cases, thirteen actual
CLI cases and authoring/restart checks pass within 107 enabled/103 OFF groups proof.
Actual CLI tests reproduced a near-rest successful output rejected by its next
call's stored-only initial divergence gate, because the endpoint had used the
larger predictor scale. The integrated API now exposes `initialVelocity` and
the CLI exposes `--initial-velocity stored|require-solenoidal`.
The default `TransportStored` measures and transports actual finite stored MAC
under the existing CFL/range/balance checks. `RequireSolenoidal` applies the
stored-only relative/six-face-ULP gate at both entry and every accepted endpoint.
Both report `initialDivergenceWithinTolerance`; arbitrary compressibility is not
called roundoff. There is no velocity floor, value erasure or cleanup projection.

```powershell
.\OpenVdbTool.exe --step-boundary-fluid boundary.vdb fog.vdb frame1.vdb --density-grid density --time-step 0.125 --initial-velocity stored
.\OpenVdbTool.exe --step-boundary-fluid frame1.vdb frame1.vdb frame2.vdb --velocity-grid descriptor:velocity --fluid-grid descriptor:fluid --face-kind-grid descriptor:boundary_kind --face-fraction-grid descriptor:boundary_fraction --face-pressure-grid descriptor:boundary_pressure --wall-velocity-grid descriptor:boundary_wall_velocity --density-grid descriptor:density --temperature frame1.vdb --temperature-grid descriptor:temperature --time-step 0.125 --initial-velocity stored
```

Use the JSON result's complete `restart` binding for custom output names; it
contains all six canonical selectors, both scalar selectors and the submitted
policy. Raw names and exact descriptor selectors are separate fields. Authoring
captures these roles before execution and retains them through draft switches.
Explicit restart restores all eight bindings and policy and clears the next
destination; density/velocity inspection pins the selected scalar/MAC grid.
The multi-grid result cannot become a generic single-grid restart or SDF model.

The geometry stays fixed, walls have zero normal velocity, and saved pressure
fractions describe ghost distances rather than evolving transport-cell volumes.
Scalar transport uses conservative first-order shared donor flux. Pressure inflow
uses constant reservoirs; outflow uses the adjacent fluid value. For tolerated
nonzero stored divergence, donor coefficients sum to `1-dt*div(U)`, so constants
need not remain exact. Constant-response drift and stored rounding balances are
reported without normalization or clamping. MAC momentum uses declared advective
face differences, incident-cell transverse averages and local fluid adjacency;
it does not claim momentum/energy conservation or tangential no-slip.

Transported scalars drive WORLD gravity and buoyancy, followed by one actual
canonical projection per attempted dt. Endpoint-CFL failure discards the whole
private trial and halves dt from unchanged input. Rejected work and solver
iterations remain charged. Nonuniform prescribed pressure adds a fresh impulse
each accepted dt; restart equivalence requires matching actual dt partitions.
Minimum-dt, convergence, range, time-progress or cap failure publishes no partial
time. The report includes the accepted dt sequence, separate density/temperature
balances, divergence and pressure residuals, attempts and all resource bounds.
Cell-centered speed samples are diagnostics; CFL uses separate face rates.

Uniform orthogonal maps span h1e-6 through h1e6, including represented rotations
and reflections. Exact bounded dyadic products retain tiny fluxes and cancellation
until final Double storage; detectable lost/spurious inverse coefficients reject.
All source catalogs, original/current/trial state, nine output trees, exact
scratch, every attempted projection and writer buffers share logical caps.
Boundary-file metadata and selected scalar-grid metadata survive; scalar-file
metadata is not merged and canonical membership is regenerated. Unicode paths,
exclusive output, source preservation, independent reservoir/closed-circulation
oracles, rejected-trial semantics, actual near-rest restart, strict entry/
endpoint failures and shared limits are covered by
`out/openvdb-validation/particle-fluid-fog-milestone.json`. Moving geometry,
evolving liquid surfaces, cut cells, viscosity/sources, vorticity and boundary
sequences remain open; the separate sequence proposal is ongoing staged work.

### Coupled closed-domain smoke stepping

`WriteFluidStep` and `--simulate` couple MAC velocity self-advection, masked
density/Kelvin transport, world gravity/spatial buoyancy and pressure projection.
Initialize a MAC field with `--project`, then step and restart from the saved
five-grid state:

```powershell
.\OpenVdbTool.exe --project wind.vdb fog.vdb initial.vdb --domain-grid density
.\OpenVdbTool.exe --simulate fog.vdb initial.vdb initial.vdb step.vdb --input-grid density --velocity-grid velocity --domain-grid fluid --time 0.041666666666666664 --steps 2 --gravity 0 -9.81 0
.\OpenVdbTool.exe --simulate step.vdb step.vdb step.vdb next.vdb --input-grid density --velocity-grid velocity --domain-grid fluid --temperature-input step.vdb --temperature-grid temperature
.\VulkanSceneRenderer.exe --vdb-volume step.vdb --vdb-grid density --vdb-temperature-grid temperature
```

Inputs must share the domain's uniform orthogonal transform. Velocity uses the
native MAC convention above. Float/Double density and optional temperature are
nonnegative, zero outside the fluid mask, and at most 1e6; omitted temperature
starts at ambient 293.15 Kelvin. Buoyancy is
`direction * (temperatureCoefficient * (T - ambient) - densityCoefficient * density)`.
The direction must be unit length. Adaptive steps bound component travel to
`--cfl` (at most 0.5 voxel), recheck speed/force each step and project after each
actual step. At most 64 steps are permitted and each must be at least 1e-6 seconds.

Transport uses RK2 backtracing, local wall-visibility checks and normalized fluid
samples; it cannot leak across a closed wall through a disconnected sampling
corner. It can diffuse density and does not conserve mass exactly. JSON reports
density mass and temperature-volume integrals before/after (the latter is not
thermal energy), clipped traces, actual substeps, speed, divergence, pressure
residual and storage-rounding diagnostics. Output contains Double density,
temperature, pressure, native MAC velocity and Bool domain. Source files and
existing destinations remain intact on failure. Optional `--kinematic-viscosity`
replaces each actual substep's projection with implicit viscosity followed by
projection. Repeated `--source` spheres apply scalar sources/sinks last; their
density/heat drives buoyancy on the next actual substep. The same options work
for saved sequences, with per-frame diffusion/source diagnostics in the manifest.
Default zero viscosity/empty sources retain the original stored-field behavior.
Vorticity, free surfaces, moving solids and point coupling remain open.

`WriteFluidSequence` and `--simulate-sequence` save repeated steps, using each
five-grid result as the next complete restart:

```powershell
.\OpenVdbTool.exe --simulate-sequence fog.vdb initial.vdb initial.vdb frames --input-grid density --velocity-grid velocity --domain-grid fluid --frames 24 --time 0.041666666666666664
.\VulkanSceneRenderer.exe --vdb-sequence frames --vdb-fps 24 --vdb-grid density
```

The output directory must be new. It contains `frame_0001.vdb` onward and
`sequence.json` with selectors, absolute simulated times and per-frame numeric
diagnostics. Each saved frame advances one timestep; the initial state is not
copied. Match playback FPS to the saved timestep. At most 256 frames and the
explicit aggregate encoded-byte cap are permitted; retained coordinator storage
is reserved alongside per-frame solver memory. Processing stages owned files
before creating the destination exclusively. Publication is rollback-safe for
ordinary errors, with no overwrite; it does not promise atomic visibility of
all files or crash durability. API callbacks report completed frames and permit
cancellation between timesteps. An in-progress library solve must finish first.

### Tracked level-set advection and morphing

`OpenVdbLevelSetTransport.h` exposes `WriteAdvectedLevelSet` and
`WriteMorphedLevelSet` with HJWENO5/TVD-RK2 advection and narrow-band tracking.
Sources are Float positive-exterior level sets with symmetric 3..64-voxel bands
and uniform orthogonal affine voxels. Advection samples collocated world-space
Vec3S/Vec3D velocity from an independent affine map. Native MAC requires a
separate conversion. Morph targets must match source transform and band.

```powershell
.\OpenVdbTool.exe --advect-sdf shape.vdb wind.vdb moved.vdb --time -0.2 --velocity-grid velocity --normalization-iterations 3
.\OpenVdbTool.exe --morph-sdf shape.vdb target.vdb relaxed.vdb --time 1 --target-grid surface --max-cfl-steps 512 --max-transport-work 500000000
```

Advection time is signed seconds. Morph time is nonnegative relaxation pseudo-time
in [0,8], not a linear interpolation fraction. Morphing cannot seed disconnected
target fronts without an existing source front. Unit-voxel conditioning retains
world transforms and distance units, including rotation/reflection and remote
indices. Travel, Float progress, CFL steps, work, prospective tracker topology
and workspace are bounded. PDE time is Float32: the requested Double interval
can round or leave an unresolved terminal remainder. Zero time writes an
unchanged field with the chosen output name; unresolvable nonzero motion fails.
Alternate schemes, masks, time-varying velocity and intermediate-frame authoring
remain open. Results report CFL and conservative leaf-slot/sample work counts.

### Scalar fluid sources and sinks

`OpenVdbFluidSources.h` operates on canonical five-grid restart states. Only true
fluid-cell centers inside inclusive world-space spheres are affected. Overlapping
rates are combined in canonical order, then exact local ODEs advance density
`D'=Jd-Kd*D` and temperature `T'=Jt+Kt*(ambient-T)`. Explicit clear/reset flags
apply before the rates. There is no velocity injection, domain expansion or
hidden mass clamp. Nonzero terms lost to numeric range and final scalars outside
[0,1e6] reject publication. MAC velocity, pressure, domain and custom metadata
are retained; scalar leaf growth is preflighted across the entire state.

```powershell
.\OpenVdbTool.exe --fluid-sources state.vdb emitted.vdb --time 0.1 --source 0 0 0 0.5 --density-rate 2 --heat-rate 10 --density-decay 0.2 --temperature-relaxation 1
```

Repeat `--source X Y Z R` to append up to 64 spheres. Subsequent rate/reset flags
edit the latest sphere and require one to exist. Density/heat rates use scalar
units per second; decay/relaxation use inverse seconds. The same state-name flags
as viscosity select literal grid names. The authoring panel provides sphere
add/remove controls and reports affected cells, mass and Kelvin-volume integrals.

### Implicit fluid viscosity

`OpenVdbFluidViscosity.h` and the **Fluid viscosity** authoring operation consume
a canonical five-grid smoke restart state:

```powershell
.\OpenVdbTool.exe --viscosity state.vdb viscous.vdb --time 0.04 --kinematic-viscosity 0.01 --tolerance 1e-6 --max-iterations 1000
```

`--density-name`, `--temperature-name`, `--velocity-name`, `--pressure-name` and
`--domain-name` select literal state names and retain them in the output. All
five grids must have the same uniform orthogonal affine transform. Density,
Kelvin, pressure and native negative-face MAC use Double; the domain is Bool.
The backward-Euler face solve uses a zero exterior face-lattice stencil, an
explicit stair-step no-slip approximation. It then applies the checked pressure
projection. Density, temperature, domain and custom metadata are retained.
Results report stored residuals, MAC energy before/after diffusion/projection,
divergence and rounding allowance. Zero viscosity validates and copies the five
selected grids without another solve. The coupled smoke timestep also accepts
optional viscosity at every actual substep. Free-slip, moving-solid and open boundaries remain
pending. Time, viscosity, solver and allocation limits are explicit in the API;
nonconvergence leaves no output.

### Spatial transport reference

`OpenVdbTransportReference.h` exposes a deterministic, bounded CPU reference
path tracer independent of OpenVDB/Vulkan build availability. `MakeTransportMedium`
copies a dense `OpenVdbVolume`; native payloads require an explicit dense load.
`TraceTransportReference` accepts rays, up to eight media, 32 directional/point
delta lights and 4096 two-sided black occluder triangles. It uses actual spatial
null-collision transport, RGB albedo, HG phase sampling, ratio-tracked shadows,
independent emission and a constant environment. Coefficients and source units,
cell-centered lattice order and sampling bounds are explicit in the public API.
Results include standard errors, work/event counts and truncated-path counts.

Increasing samples reduces Monte Carlo error; increasing scattering order is
independently necessary when truncated tails matter. The finite-order cap is
explicit bias. Roulette does not add that bias. Tests include absorption/emission,
overlapping media, phase direction, symmetry, uncertainty/convergence and indirect
light around an occluding wall. This API is a reference building block; the
interactive renderer still uses the local approximation described above.

`OpenVdbTransportGpuReference.h` supplies a separate synchronous Float32 Vulkan
compute reference with the same transport units. Link
`VulkanSceneRenderer_openvdb_transport_reference`, provide an existing device,
queue and memory manager, and serialize queue access. Its owned submission fence
is awaited before returning. It supports up to four media, 32 directional/point
lights, 4096 black triangles/rays and one million paths; no RTX, shaderInt64 or
shaderFloat64 is required. The immutable native-source factory uses the bounded
VDB loader. Input conversion, world precision, majorants and work are checked;
any failing path rejects the whole result. Small shadow weights use unbiased
roulette with the actual quantized survival probability. Finite scattering order
still introduces an explicitly reported truncated tail. Float32-subnormal
inputs are rejected. Native Double density above the loader's clamping range
can differ from dense resampling because clamping and interpolation order differ.
Temperature, area/spot lights, surface BSDFs, transparency interleaving and the
viewport adapter remain open. This reference does not change interactive rendering.

The isolated compute suite passes eleven enabled and eleven disabled-build
cases, covering analytic absorption/emission, CPU statistical agreement, phase,
occlusion/indirect transport, convergence, native slots, ownership and errors.
The dense tests remain available without OpenVDB/NanoVDB. To run the main target:

```powershell
cmake --build out/build/visual-studio --config Release --target openvdb_transport_gpu_reference_tests
$env:CONTAINER_RUN_OPENVDB_TRANSPORT_REFERENCE_GPU = '1'
ctest --test-dir out/build/visual-studio -C Release --output-on-failure -R '^openvdb_transport_gpu_reference_tests$'
```

### Physical image export

`OpenVdbTransportImage.h`, `OpenVdbTransportImageScene.h` and
`OpenVdbTransportRenderJob.h` provide CPU/GPU pinhole images and exclusive
scene-linear FLOAT OpenEXR output. The standalone `OpenVdbTransportRender` tool
and the panel's **Render physical volume image** job share the strict version 1
scene adapter. For example, save this beside an existing `fog.vdb`:

```json
{"schemaVersion":1,"camera":{"eye":[0,0,3],"target":[0,0,0],"up":[0,1,0],"verticalFovDegrees":45,"width":64,"height":64},"media":[{"input":"fog.vdb","gridName":"density","extinction":1,"albedo":[0.8,0.8,0.8]}],"lights":[{"type":"directional","vector":[1,1,1],"value":[1,1,1]}],"environment":[0.1,0.1,0.1]}
```

```powershell
.\OpenVdbTransportRender.exe --scene scene.json --output cpu.exr --samples 256 --order 8 --seed 17
.\OpenVdbTransportRender.exe --scene scene.json --output gpu.exr --backend gpu --samples 256 --order 8 --validation
```

The GPU tool finds its packaged `spv_shaders/openvdb_transport_reference.comp.spv`;
`--shader` can select another explicit compiled kernel. Its dedicated Vulkan
device/queue does not share live viewer resources. CPU requires dense sources;
`nativeSparse:true` chooses trusted native sources for GPU rendering. Media
also accept bounded `maxResolution`, `importScale`, `anisotropy` and RGB
`emission`. The adapter supports four sources, 32 point/directional lights and
4096 black triangles in `occluders`. Directional vectors point toward the
light; point vectors are world positions. Relative inputs resolve from the
original scene-spec directory. Unknown/duplicate fields and numeric range loss
fail. A CPU scene without VDB media works in OpenVDB-disabled builds.

Pixels are top-to-bottom, x-fastest center samples with no subpixel antialiasing.
Canonical 64-pixel RNG blocks make results independent of block traversal;
CPU/GPU noise differs. EXR channels are `R/G/B` and `SE.R/SE.G/SE.B`. Standard
error is unavailable at one sample and explicitly tagged. Double CPU estimates
remain Double in memory; Float export rejects overflow/nonzero underflow and
reports other rounding. Camera, seed, sample/order limits and work counters are
recorded in EXR attributes. No tone mapping or half conversion is applied.

Image limits are 2048 per axis, 262144 pixels, 8192 samples per pixel, 16 million
paths and one billion aggregate tracing/preparation work items. Scene loading,
retained images, native payloads, block scratch and serialization share byte
caps. `--max-bytes`, `--max-spec-bytes`, `--max-density-values` and `--work`
bound standalone jobs; the reusable API/panel expose the remaining external
caps. Scene JSON cannot raise them. Existing outputs remain intact on failure.
The displayed error estimates cover Monte Carlo noise, not the explicitly
reported finite-order tail. The physical image route retains the reference's
gray-extinction/HG/black-surface model; viewport integration, broader light and
surface BSDF coverage, temperature emission and transparent interleaving remain
open.

```powershell
cmake --build out/build/visual-studio --config Release --target openvdb_transport_image_gpu_tests
$env:CONTAINER_RUN_OPENVDB_TRANSPORT_IMAGE_GPU = '1'
ctest --test-dir out/build/visual-studio -C Release -R '^openvdb_transport_image_gpu_tests$' --output-on-failure
```

### Authoring in the viewer

Open **OpenVDB tools** in Scene Controls. The validated 78-choice batch covers CPU
generation, conversion, editing, point import/export, inspection, calculus,
Poisson, point processing, closed-domain projection, smoke stepping, Platonic
solids, sequences, surface measurements, unsigned mesh distances, native point
rasterization, native attribute/group editing and readback, tracked SDF advection/
morphing, fluid sources, implicit viscosity, ordinary masked field filters,
field compositing/value transforms, topology masks/morphology/Booleans,
native value/activity edits, mask clipping, SDF background/signed flood fill,
native SDF filters, tracking, activity-masked normalization, sphere fitting,
fast sweeping distance conversion and field extension, region/cavity masks,
approximate closest-surface/curvature queries and rigid cutter fracture,
selected point voxel masks and generic native-column field sampling,
native file authoring, physical image export and scalar/vector
sample workflows above; Field calculus contains the
six differential operators. Enter the applicable paths/grid selectors, choose a
new output path, adjust the parameters and processing limits, then select
**Run OpenVDB operation**.

Each submission takes an immutable settings snapshot and runs one background
CPU job. Rendering and UI polling continue; further submissions are disabled
until that job finishes. Results show completion/error messages, counts or
sample values. **Use output as input** prepares the next step and clears the
previous grid selector. For eligible
scalar/point outputs, **Load preview** explicitly loads the generated surface or
physical-radius markers into the scene. Smoke results offer **Use state as
restart**, preserving the grid names from the submitted snapshot. Density ray
marching uses the separate `--vdb-volume` route.
Smoke sequence jobs display completed-frame progress and offer **Cancel sequence**.
Cancellation waits for the current timestep and discards the staged sequence;
the next submission starts with fresh progress and cancellation state. Existing
API progress/stop callbacks are retained. Shutdown requests sequence cancellation
and joins the current solve. Other started library jobs must finish before shutdown.
File-producing operations require new paths and preserve existing outputs.

### Python interoperability

The stdlib-only `container_openvdb` package provides typed native dense buffers,
point columns/groups, selection/statistics and file authoring through discovered
versioned tool protocols. Python 3.10 or newer is required. It preserves exact
int64 values, exposes buffer-compatible arrays and reports runtime availability
before file operations. See [the Python bridge guide](openvdb-python.md) for
installation, data examples, limits and native traversal rules.

## Packaging

An OpenVDB-enabled renderer build also builds `OpenVdbTool`. Runtime DLL staging
includes the library and imported shared dependencies. The Windows packaging
script includes the tool, this guide, installed dependency notices, and the
`openvdb_enabled`/`nanovdb_enabled` manifest flags. NanoVDB-enabled packages also
include `THIRD_PARTY_LICENSES/NanoVDB-Apache-2.0.txt`; configure
`NANOVDB_LICENSE_FILE` if an externally supplied header installation has no
discoverable upstream license. If OpenVDB uses a separate vcpkg installation,
pass `-OpenVdbInstalledDirectory` alongside the normal package arguments.

## Validation

The current particle/fluid/fog milestone rebuilds the application with 101
authoring choices and passes all 107 available enabled CPU/configuration/CLI/UI/
shader/package groups in 378.40 seconds and all 103 OFF groups in 52.71 seconds.
All 92 panel cases pass in 13.754 seconds. This includes the integrated particle
descriptor repair, boundary inverse guard, explicit boundary-fluid initial-
velocity policy/restart binding and read-only fog authoring. Exact source and
binary hashes, logs and XML are in
`out/openvdb-validation/particle-fluid-fog-milestone.json`.
The inaccessible exact topology executable is the single excluded enabled CPU
group; there is no renamed/relocated substitute. Linux execution and fresh GPU/
display proof remain open. Broader feature gaps stay explicit in the completion
ledger; ongoing weighted statistics/sequence stages are outside this checkpoint.


The 26 October region/query/fracture batch passes all 68 available enabled
CPU/configuration/CLI/UI/shader/package groups in 182.68 seconds and all 64
disabled groups in 24.14 seconds. The configured enabled suite has 69 groups;
this available-group run excludes its topology executable and does not establish
a refreshed full-suite link/build pass. The twenty selected
transport/image/upload GPU cases pass in 4.98 seconds without GPU skips or
Vulkan validation errors. That batch validates 78 panel choices and passes 36
cases in 7.097 seconds. The new real region/query/fracture harness runs five
methods: four pass and the OFF-only method skips in the enabled run. Native
fixtures cover sixteen region, eight surface-query and twelve fracture cases,
with their disabled availability routes.
Evidence is `out/openvdb-validation/regions-surface-fracture-{cpu-available,disabled,gpu,ui,main-cli}.log`
and `regions-surface-fracture-ui.xml`.

The user-allowed existing topology executable subsequently passes CTest in
3.72 seconds (3.75 seconds real time). Its 01:32:13 timestamp predates the
current implementation's 01:48:54 timestamp, so this historical runtime result
does not validate the current topology source. Refreshed write-open/link still
fails with Access Denied/LNK1104; the fresh-binary validation gap remains.
No security settings or bypass were used. Evidence is
`out/openvdb-validation/topology-allowed-tests.log` and
`topology-allowed-build.log`; the earlier quarantine notes describe their
historical checkpoints.

```powershell
python tests/validation/openvdb_regions_surface_fracture_cli_tests.py --tool out/build/visual-studio/Release/OpenVdbTool.exe --enabled 1 -v
```

The subsequent field-advection/potential-flow/point-index and discrete-slice
batch passes all 74 available enabled groups in 187.49 seconds and all 70 OFF
groups in 28.20 seconds. The rebuilt application has 82 authoring choices;
all forty panel cases pass in 7.727 seconds. Seventeen/thirteen/twelve native
feature cases cover the three new processing modules, and thirteen cover
slices, each with disabled availability proof. The configured enabled suite
has 75 groups; the current topology executable remains excluded because
Windows still denies its refreshed link. Its older restored binary's runtime
pass does not establish current-source validation.

Twenty selected live transport/image/upload GPU cases pass in 5.94 seconds.
Eight additional hidden renderer captures pass in 30.486 seconds: adjacent
Int64 IDs above 2^53, Bool stored values, Mask activity and Vec3I components
match independently authored Float reference images pixel-for-pixel. Adjacent
IDs produce five distinct ordered colors; Bool inactive-true/active-false and
Mask activity stay distinct. Native inputs remain unchanged, with no Vulkan
validation errors or synchronization hazards. The opt-in native fixture
helper also passes ON/OFF, Unicode-path and exclusive-output checks.

Evidence is `out/openvdb-validation/advection-flow-index-slice-{cpu-available,disabled,gpu}.log`,
`advection-flow-index-slice-{authoring,slice_preview,field_advection,potential_flow,point_neighbors}.xml`
and `discrete-preview-runtime/run-20261026T074742Z-47860-325e4f96/results.json`.
The earlier forty-panel 6.913-second and initial OFF 32.07-second results are
historical checkpoints.

Focused real-tool CLI checks pass eight field-advection methods plus one
expected OFF-only skip in 9.030 seconds, eight potential-flow methods plus one
expected skip in 7.943 seconds, and six point-index methods plus one expected
skip in 4.664 seconds. Exact-final potential-flow OFF checks pass four methods
with five enabled-only skips in 2.525 seconds; point-index OFF checks pass two
with five expected skips in 1.226 seconds. Capability availability matches the
linked runtime in both builds. These focused checks do not establish a complete
enabled application rebuild. Logs are
`out/openvdb-validation/advection-flow-index-{field,points,potential}-cli.log`
and `advection-flow-index-{potential,points}-cli-disabled-final.log`; native artifacts
are under `out/openvdb-review/{field-advection,potential-flow,point-neighbors}/`.

The next closed-contour rebuild milestone passes all 76 available enabled
CPU/configuration/CLI/UI/shader/package groups in 182.04 seconds and all 72
OFF groups in 29.36 seconds. The application is rebuilt with 83 authoring
choices; all 41 panel cases pass in 7.055 seconds. Twelve native rebuild feature
cases pass with the availability check skipped in the enabled build, and the
OFF availability case passes. Its CLI route is included in the coordinated
run. The enabled configuration contains 77 groups; the fresh topology link
remains blocked as described above. Earlier GPU/capture evidence belongs to
the preceding 82-choice milestone; this rebuild milestone changes CPU authoring.
Evidence is `out/openvdb-validation/level-set-rebuild-{build,cpu-available,disabled}.log`
and `level-set-rebuild-{authoring,native}.xml`.

The surface-field/native-point-motion/generic-transfer milestone is integrated
into the rebuilt 86-choice application. Its 21/13/16 native feature cases and
disabled availability checks pass. Focused real CLI checks pass 10/7/7 enabled
cases, respectively, with the expected OFF-only skips, in 9.557/15.144/8.926
seconds. All 44 panel cases pass in 7.829 seconds. All 82 available enabled
CPU/configuration/CLI/UI/shader/package groups pass in 233.95 seconds, and all
78 OFF groups pass in 34.28 seconds. The enabled configuration includes one
additional topology group whose current executable cannot be freshly linked
while Windows denies replacing it.

A subsequent slice correction preserves nearest-cell selection immediately
below half-cell ties for Int64, Bool, Mask and Vec3I. The nineteen native slice
feature cases pass in 1.816 seconds, with OFF availability also passing.
The application was rebuilt with that correction; all twenty GPU
upload/transport/image cases pass in 5.27 seconds and all eight rendered
discrete-preview captures pass without validation errors. The broad 82-group
run precedes this correction; these targeted native/GPU/application checks
validate the correction. Evidence is under `out/openvdb-validation/`:
`surface-motion-transfer-{cpu-available,disabled,authoring-focused}.log`,
`surface-motion-{surface,point}-cli.log`, `surface-motion-transfer-cli.log`,
`slice-midpoint-enabled.{log,xml}`, `slice-midpoint-app-retry.log`,
`surface-motion-transfer-gpu.log`, and
`discrete-preview-runtime/run-20261026T192213Z-53600-5c04a22d/results.json`.

The ordinary-field-resampling milestone passes all 84 available enabled
CPU/configuration/CLI/UI/shader/package groups in 258.77 seconds and all 80 OFF
groups in 40.97 seconds. The application is rebuilt with 87 authoring choices;
all 46 panel cases pass in 8.454 seconds. Eighteen integrated native resampling
feature cases pass in 1.972 seconds, with the enabled availability case skipped;
OFF availability passes. Six real CLI cases pass with one OFF-only skip in
10.433 seconds. The broad run also includes the corrected nineteen-case slice
suite. One additional configured topology group remains excluded because the
restored executable runs but Windows still denies replacing it during linking.
No GPU/rendering change is claimed for this CPU authoring milestone; the twenty
GPU cases and eight rendered captures above belong to the preceding rebuilt
slice-correction milestone. Evidence is under `out/openvdb-validation/`:
`field-resampling-{build,cpu-available,disabled,native,cli,authoring-focused}.log`
and the corresponding native/authoring XML files. Remaining V05 variants stay
open in the ledger.

The diagnostics/boundary milestone passes all 89 available enabled
CPU/configuration/CLI/UI/shader/package groups in 279.35 seconds and all 85 OFF
groups in 44.33 seconds. The application is rebuilt with 91 authoring choices;
all 52 panel cases pass in 9.035 seconds. Integrated diagnostics has 26 native
feature cases and 12 actual CLI cases; boundary projection/replay has 23 native
feature cases and nine actual CLI cases. Eight integrated provenance cases
verify derived-grid invalidation and exact-copy exceptions for the five reserved
boundary keys. These enabled native suites skip only their OFF availability
case. Actual disabled CLI proofs pass three diagnostics and two boundary cases.
The boundary test was freshly rebuilt after its portable environment-guard fix.
Fresh previews from the rebuilt application pass eight captures in 46.0 seconds
with zero Vulkan validation errors, exact independent Float-reference parity,
and visibly ordered adjacent Int64 colors. These are new rendered checks; the
twenty GPU unit cases above remain evidence from the earlier slice milestone.
One additional configured topology group still lacks a fresh link because
Windows denies replacing the restored executable. Linux validation remains
pending: the reviewed nine-module ON/OFF container harness is prepared but UNRUN
because the Docker engine is unavailable. Evidence is under
`out/openvdb-validation/diagnostics-boundaries-*`, including the milestone JSON,
native/authoring XML and `diagnostics-boundaries-discrete-runtime/`
`run-20261026T204836Z-24460-0da920f0/results.json`. Broader diagnostics, boundaries,
simulation and backend requirements remain open in the ledger.

The native-point/Nano-primitive milestone passes all 93 available enabled
CPU/configuration/CLI/UI/shader/package groups in 301.88 seconds and all 89 OFF
groups in 47.18 seconds. The application is rebuilt with 93 authoring choices;
all 57 panel cases pass in 10.117 seconds. Twenty native-advection feature cases
and all thirteen unchanged affine-Motion cases pass; their enabled availability
cases skip. All fourteen primitive tests pass, including its availability check.
The actual main Tool passes eight advection CLI cases and nine primitive CLI
cases, with one OFF-only skip in each. Actual OFF CLI proof passes two and three
cases respectively. The advection fixture/readback-only repair is documented;
it does not fix the two adjacent legacy Unicode/tiny-map routes.
An independently built full native-I/O-only Tool has nativeIo=true and
openVdbConversion/openvdbAvailable=false, with no OpenVDB/TBB DLL dependency.
It passes 47 native cases, nine primitive CLI cases and six existing Nano CLI
cases, plus the native-advection unavailable route. Exact sources, binary hashes,
logs/XML and the 91-to-92-to-93 authoring chain are recorded under
`out/openvdb-validation/native-point-nano-*` and
`out/openvdb-review/native-io-full-tool/`. Rendering evidence belongs to the
preceding 91-choice milestone; these additions are CPU authoring workflows.
The topology fresh-link and Linux runtime gaps above remain unresolved, and
the broader point/Nano families remain partial.

The active-region/ray-query milestone passes all 97 available enabled
CPU/configuration/CLI/UI/shader/package groups in 332.32 seconds and all 93 OFF
groups in 57.90 seconds. The application is rebuilt with 96 authoring choices;
all 64 panel cases pass. Eleven native active-region cases and eighteen native
ray-query cases pass, plus seven and ten actual CLI feature cases respectively;
their enabled availability cases skip. The tracked independent SDK fixture
producer supplies all nine native value types, exact integer/nonfinite payloads,
binary metadata, weak Double crossings and coarse active-leaf/tile spans. Region
reports retain exact typed values and full counts under bounded record quotas.
SDF ray hits use approximate native HDDA/chord refinement; volume spans cover
coarse active leaves/tiles rather than exact voxel occupancy or fog integrals.
Both read-only report routes suppress stale output/model/restart actions.

This milestone also repairs the two legacy routes left open by the preceding
93-choice checkpoint. Exact tiny PointData maps and encoded offsets now reach
direct attribute and native neighbor commands without unit-map re-encoding.
Legacy velocity output uses checked ASCII SDK scratch followed by exclusive
Unicode publication, including Unicode parent paths. Four direct regression
groups execute twelve real Tool commands against the original tiny-map fixture;
the strengthened tracked native-advection CLI passes eight feature cases plus
one OFF-only skip. Six legacy native suites pass 95 feature cases; source and
foreign output bytes are preserved. Hosts without a writable ASCII TEMP or
working-directory scratch route still receive an explicit unsupported-path
diagnostic. The fresh topology link remains blocked by Windows after the user's
allow action, and Linux runtime validation remains unrun. No new GPU capture
proof is claimed. Exact sources/binaries, logs and native/authoring XML are
recorded in `out/openvdb-validation/region-ray-milestone.json`,
`region-ray-final-*` and `out/openvdb-review/legacy-route-fixes/`.

The value-component/surface-reference milestone passes all 101 available enabled
CPU/configuration/CLI/UI/shader/package groups in 341.86 seconds and all 97 OFF
groups in 29.59 seconds. The application is rebuilt with 98 authoring choices;
all 71 panel cases pass in 11.487 seconds. Eight native component cases and
sixteen native surface-reference cases pass, plus eight and eleven actual CLI
feature cases respectively; enabled availability cases skip. The component CLI
independently reads all nine value types, inactive/nonselected finite payloads,
exact maps and binary metadata. Its corrected canonical fixture checks both
pressure's three membership keys and boundary fractions' full five keys rather
than assuming clamp diagnostics exist on pressure.

Component outputs retain the whole source field's values while selecting one
connected activity component, without asserting separate physical solids. The
CPU reference uses native perspective/orthographic center rays, matte/normal/
headlight response and eleven linear FLOAT EXR channels, with independent
standard-library pixel/header/camera/depth/status readback. A real default32
sphere probe found that the inherited 200M ray-work cap was insufficient; the
surface-specific default is now 2B, ordinary ray-query defaults are unchanged,
and the real CLI regression uses no image/work overrides. EXR results cannot
become VDB model/restart inputs, including custom executors and later draft
changes. All variable report data is prepared before publication. Source and
foreign output bytes are preserved; native/OFF/Unicode/cap failure paths pass.
Exact source/binary hashes, XML and logs are recorded under
`out/openvdb-validation/value-surface-*`, especially `value-surface-milestone.json`.
The topology fresh-link and Linux runtime gaps remain open. This milestone adds
CPU image proof; it does not claim fresh Vulkan captures or close the remaining
families in the ledger.


The topology/sampling batch on 25 October passes 57 enabled
CPU/configuration/CLI/UI/shader groups in 59.98 seconds, 53 disabled groups in
21.58 seconds and the clean-PATH Windows package runtime check in 2.58 seconds.
It includes ordinary field
compositing/value transforms, topology masks/morphology/Booleans, selected point
voxel masks, generic native point-column sampling, native attribute previews and
sampled scalar/vector slices. Dense interchange explicitly tests Unicode input
filenames through native-path snapshots. Native topology has 15 feature cases;
native sampling has eleven, including paged attribute preservation and aggregate
source/field limits. The preceding 54-group batch passed in 57.51 seconds.

The extended preview application check passes ten captures and thirteen commands
in 38.302 seconds with zero Vulkan validation errors. Named point radii change
projected area by the expected direction; adjacent Int64 and string attributes
change colors; group selection removes the center marker; half opacity reduces
the image signal. Slices match exactly between forward/deferred paths, while
point differences remain below the recorded image tolerance. Commands, images
and metrics are retained in
`out/openvdb-validation/extended-preview-runtime/run-20261025T181804Z-10556-36cf2d70/`.

```powershell
$env:CONTAINER_RUN_OPENVDB_EXTENDED_PREVIEW_GPU = '1'
python tests/validation/openvdb_extended_preview_runtime_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --tool out/build/visual-studio/Release/OpenVdbTool.exe --output-dir out/openvdb-validation/extended-preview-runtime
```

The main physical transport/image GPU suites pass eleven/five cases in
3.28 seconds, and eleven CPU/GPU image-tool cases pass independent EXR readback.
Shader staging repairs a stale production copy without relinking the tool.
These results establish the offline transport contract above; the remaining
viewport requirements stay open in the completion ledger.

The CPU suites create small temporary VDB fixtures rather than downloading
large smoke datasets. They cover surface geometry, transforms, density
and temperature resampling, native sparse sampling, grid selection, input
budgets, SDF generation/rebuilding, transformed glTF/GLB mesh conversion,
boolean signs and fog conversion. Editing cases check world-space offset signs,
all three smoothing filters, metadata preservation, relative voxel uniformity,
numeric limits and workspace bounds. Sequence tests cover natural ordering,
playback controls, bounded dense/native caching and explicit frame failures.
Async tests check warmed-array ownership without a copy, latest-request
delivery, byte/frame reservations while callers hold leases, wakeup after
lease release, bounded prefetching, latched prefetch errors and explicit
foreground retry. Shutdown tests cover idle workers, memory waits and leases
that outlive the worker; invalid budgets and failed loaders are also tested
when OpenVDB is disabled.
Dependency checks cover disabled builds, package configs, upstream module
discovery, NanoVDB flags/header discovery without a CUDA download, and
actionable errors when an enabled dependency is absent.

Local validation on 24 October 2026 used OpenVDB 12.0.1, MSVC 14.51,
and an NVIDIA GeForce RTX 2080 SUPER with Vulkan 1.4.341:

- The OpenVDB/NanoVDB-enabled Release renderer and all four CPU test targets
  build successfully: surface import, processing, volume loading and sequences.
- All nine enabled `openvdb_*` CPU/configuration/CLI CTest groups pass in
  14.19 seconds, including six settings cases and clean-PATH Windows packaging.
  The sequence group,
  including async loading and shutdown tests, passes in 1.01 seconds. The
  shared glTF loader regression suite also passes. Availability tests for the
  opposite build mode skip.
- An isolated native build passes all 20 density/temperature and native volume
  cases. An isolated disabled build compiles the actual import, processing,
  volume and sequence sources plus the tool; all six smoke/CPU/CLI groups pass
  with the async implementation and SDF editing APIs in 2.02 seconds. Its generic
  sequence cases include invalid
  ownership/budgets, explicit failed completions and 64 idle shutdown races.
- Earlier Debug surface validation passes with the matching Debug OpenVDB/CRT
  dependencies; test output folders are separate per configuration.
- All 98 GPU captures pass in 426.940 seconds across 110 subprocess commands,
  including deferred and forward 4x MSAA, surface import, clipping,
  orthographic views, selective TAA, emission,
  point/spot/area lighting, overlapping volumes, synchronous and asynchronous
  sequence playback, and raster geometry shadows. No Vulkan validation or
  synchronization errors were found.
- All 55 captures shared with the previous default-settings run are pixel
  identical: maximum RGB channel error is zero. The optional scattering and
  transparency changes preserve these existing results.
- All four Vulkan upload lifecycle cases pass in 24.95 seconds. A timeline
  semaphore holds transfers pending while the tests verify nonblocking polling,
  busy/early-publication rejection, owned source bytes, ready-candidate discard,
  independent command-pool lifetime and destruction during an unfinished upload.
  No Vulkan validation or synchronization errors were found.
- All three headless GPU scattering-property cases pass in 1.05 seconds,
  exercising the production helper across 160 RGB albedo/transmittance/order
  queries, strength response and per-medium shadow visibility. Synchronization
  validation reports no errors.
- Reversing two overlapping volumes produces identical pixels. Rendering a
  volume twice matches doubled extinction exactly, and four overlapping media
  match two media at doubled density. Paused, zero-speed, start-frame and
  advance/hold sequence captures match their static references exactly.
- Rectangle and disk emitters illuminate fog, and higher area-light radiance
  increases illumination. Backfacing emitters match the disabled reference
  exactly; two equal emitters match one emitter at doubled radiance exactly.
  The area-light geometry-shadow comparison changes 2,538 pixels while the
  opaque central 12x12 patch has zero mean RGB error. This validates the
  bounded area-shadow approximation described above.
- Async dense pause, start-frame, advance/hold and temperature captures match
  their static references exactly. Replacing the animated medium preserves two
  other static media exactly. Resize, acquire/present recovery, MSAA changes and
  a forward/deferred technique round trip also match the static reference.
- Advancing dense/native async telemetry reports requested/displayed frame 2,
  two GPU uploads submitted and two published, zero discarded, and no pending
  CPU load or GPU upload. Paused startup submits no replacement. Native
  advance/hold differs from the dense reference by at most one RGB channel
  level; native async temperature differs by at most two. A corrupt second
  frame exits with an error identifying frame 1 and `smoke002.vdb`.
- Native/dense comparisons differ by at most two RGB channel levels across
  density, temperature emission, forward 4x MSAA and four-volume captures. The
  off-camera caster visibly shadows fog while the opaque central 12x12 patch
  stays unchanged. The reactive diagnostic remains black over sky/occluder
  and white over visible fog.
- Multiple-scattering zero strength, vacuum, zero albedo and emission-only
  comparisons are exact. Additional scattering changes 5,258 cloud pixels;
  increasing strength/order changes 5,863. Directional, point and area lights
  respond while an away-facing spot cone remains unchanged. Opaque depth,
  media order and mixture/doubled-density equivalence remain intact; the
  opaque central patch has zero RGB error. Native/dense multiple scattering
  differs by at most two channel levels, or one with forward 4x MSAA.
- Transparent planes before, inside and behind fog produce different depth
  compositions in both deferred and forward 4x MSAA: the two placement
  comparisons change 6,107 and 6,997 pixels. Reversing a three-plane stack is
  exact, vacuum differs from the existing resolve by at most one channel level,
  and an alpha-one front plane occludes the medium exactly. Glass reactivity
  with 1x TAA is white over transparent fragments and black over sky. Resize, MSAA,
  technique changes and acquire recovery preserve the reference exactly;
  native/dense glass depth and stack comparisons differ by at most one level.

Additional validation on 25 October 2026 covers particle rasterization and fog
advection with the same local toolchain:

- The Release tool and new test targets build successfully. All 12 enabled
  CPU/configuration/CLI CTest groups pass in 17.05 seconds. Cases include strict
  particle JSON/radius/count limits, signed and diagonal transport, independent
  velocity transforms, active/inactive tile and voxel promotion against a
  varying-density reference, subnormal density, large-world precision guards,
  metadata preservation and output/workspace protection.
- The isolated disabled build compiles the new modules and tool; all nine
  smoke/CPU/CLI groups pass in 3.39 seconds with actionable availability errors.
- All six transport GPU captures pass in 23.673 seconds across 12 subprocess
  commands, with no Vulkan validation or synchronization errors. Generated
  particles produce a visible surface and fog. Positive world-X advection moves
  the fog's image centroid by (+40.532817, -0.171546) pixels and matches the
  separately translated particle/fog reference pixel for pixel. Native/dense
  advected fog differs by at most one RGB channel level.

Later validation on 25 October 2026 covers segmentation, OBJ export and
inspection:

- The Release tool and all three new test targets build successfully. All 16
  enabled CPU/configuration/CLI CTest groups pass in 21.55 seconds; all 13
  disabled smoke/CPU/CLI groups pass in 3.81 seconds.
- All three interchange GPU captures pass in 12.073 seconds across seven
  subprocess commands, with no Vulkan validation or synchronization errors.
  Original and segmented VDB surfaces are pixel identical, with maximum RGB
  error zero; each changes 3,118 pixels against the empty reference.
- CPU readback of the exported OBJ finds two groups, 26,856 vertices, 26,856
  normals and 8,952 triangles. Inspection confirms the two named VDB components.

The subsequent point/calculus/solver/authoring CPU batch on 25 October 2026
passes all 24 enabled CTest groups in 32.82 seconds and all 21 disabled groups in
6.03 seconds. It includes real point JSON round-trip, seeded scattering,
RK2/RK4 trajectories, field-to-point attributes, Double scalar/vector queries,
six calculus operations, scalar Poisson and closed-domain projection. Eighteen
projection cases include irregular sphere masks and native MAC reuse; eight
authoring tests exercise real point/projection execution and explicit
Run/Load/reuse controls. The recorded logs are
`point-flow-final-cpu.log` and `point-flow-final-disabled.log` under
`out/openvdb-validation`. Subsequent preview, simulation and transport-reference
work has separate validation and does not inherit these results.

Raw commands, logs, images and metrics are retained locally under
`out/openvdb-validation`. The 24 October CPU/configuration/CLI run is recorded in
`scattering-glass-cpu.log`. The runtime regression's passing run is
`scattering-glass-runtime/run-20261024T164655Z-48740-e00afecd/results.json`
within that directory. The previous-default comparison is recorded in
`scattering-glass-default-parity.json`. The upload lifecycle and headless
scattering-property results are in
`out/build/visual-studio/test_results/openvdb_volume_upload_gpu_tests-Release.xml`
and `out/build/visual-studio/test_results/openvdb_multiple_scattering_gpu_tests-Release.xml`.
The 25 October enabled and disabled results are in
`particles-advection-final-cpu.log` and `particles-advection-disabled.log`;
transport images and metrics are in
`particles-advection-runtime/run-20261025T092950Z-13516-379cdfa7/results.json`.
The later interchange enabled/disabled runs are in `interchange-final-cpu.log`
and `interchange-disabled.log`; its inspection, OBJ counts, captures and metrics
are in `interchange-runtime/run-20261025T113841Z-1260-338db58a/results.json`.

To repeat the opt-in GPU check (Python Pillow required):

```powershell
$env:CONTAINER_RUN_OPENVDB_REGRESSION = '1'
python tests/validation/openvdb_runtime_regression.py --exe out/build/windows-release/VulkanSceneRenderer.exe --tool out/build/windows-release/OpenVdbTool.exe --output-dir out/openvdb-runtime --surface-smoke --native-sparse
```

Omit `--native-sparse` when NanoVDB headers were unavailable at build time.
The harness generates local fog, sequence, occluder and light fixtures. It
compares visibility, opaque depth, clipping, selective TAA, emission and
point/spot/area lighting, then checks overlap order/density equivalence,
sequence controls, async completion telemetry/errors, raster geometry shadows
and optional native/dense parity. Staged upload checks include temperature,
retained static media and descriptor recreation across resize/MSAA/technique
changes. Multiple-scattering cases check zero-strength/vacuum/albedo/emission
invariants, light response, overlap equivalence and native parity. Glass cases
check depth placement, stack order, alpha-one occlusion, selective reactivity
and resource recovery. Every capture checks validation logs; the full run with
both optional flags contains 98 captures and 110 subprocess commands.
Result JSON retains commands, PNGs, logs and image-difference metrics.
CTest registers it when both OpenVDB and windowed tests are enabled.

The separate transport check creates particles, converts their SDF to fog,
advects it through constant world-space velocity, and compares the rendered
result with a translated reference. With native parity enabled it contains six
captures and 12 subprocess commands. It also retains commands, images, logs and
metrics under a unique run directory and checks every validation log.

```powershell
$env:CONTAINER_RUN_OPENVDB_TRANSPORT_GPU = '1'
python tests/validation/openvdb_transport_runtime_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --tool out/build/visual-studio/Release/OpenVdbTool.exe --output-dir out/openvdb-validation/particles-advection-runtime --native-sparse
```

Omit `--native-sparse` for a build without NanoVDB. CTest registers this check as
`openvdb_transport_runtime_regression` when OpenVDB and windowed tests are enabled.

The interchange check generates two disconnected surfaces, segments and
inspects their VDB grids, reads the exported OBJ records on the CPU, and compares
the original and segmented VDB renders. It contains three captures and seven
subprocess commands, retaining commands, logs and metrics in a unique directory.

```powershell
$env:CONTAINER_RUN_OPENVDB_INTERCHANGE_GPU = '1'
python tests/validation/openvdb_interchange_runtime_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --tool out/build/visual-studio/Release/OpenVdbTool.exe --output-dir out/openvdb-validation/interchange-runtime
```

CTest registers `openvdb_interchange_runtime_regression` when OpenVDB and
windowed tests are enabled.

The staged-upload API also has opt-in Vulkan lifecycle tests. They hold a
transfer pending behind a host-signaled timeline semaphore, then check early
polling/publication, busy rejection, source ownership, discard, command-pool
independence and shutdown. A watchdog releases the gate if a regression blocks.

```powershell
cmake --build out/build/windows-release --config Release --target openvdb_volume_upload_gpu_tests
$env:CONTAINER_RUN_OPENVDB_UPLOAD_GPU = '1'
ctest --test-dir out/build/windows-release --output-on-failure -R '^openvdb_volume_upload_gpu_tests$'
```

These tests require a Vulkan display/runtime and synchronization validation;
they skip without the opt-in environment variable.

The multiple-scattering helper has separate headless GPU property tests. They
execute the production Slang helper across albedo/transmittance endpoints and
check finite, nonnegative, bounded gain, strength/order response and per-medium
shadow visibility. They require a Vulkan 1.4 compute device and synchronization
validation, and do not open a window. Settings sanitization tests run on the CPU
even when OpenVDB is disabled.

```powershell
cmake --build out/build/windows-release --config Release --target openvdb_volume_settings_tests openvdb_multiple_scattering_gpu_tests
$env:CONTAINER_RUN_OPENVDB_SCATTERING_GPU = '1'
ctest --test-dir out/build/windows-release --output-on-failure -R '^openvdb_(volume_settings|multiple_scattering_gpu)_tests$'
```

The discrete display regression can be run independently after building its
native test fixture helper:

```powershell
cmake --build out/build/visual-studio --config Release --target OpenVdbDiscretePreviewFixture
$env:CONTAINER_RUN_OPENVDB_DISCRETE_PREVIEW_GPU = "1"
python tests/validation/openvdb_discrete_preview_runtime_regression.py --exe out/build/visual-studio/Release/VulkanSceneRenderer.exe --generator out/build/visual-studio/Release/OpenVdbDiscretePreviewFixture.exe --output-dir out/openvdb-validation/discrete-preview-runtime
```

With `ENABLE_WINDOWED_TESTS=ON`, CTest registers this as
`openvdb_discrete_preview_runtime_regression`. It is serial, opt-in, and retains
its native fixtures, commands, logs, PNGs and result JSON in a unique directory.
