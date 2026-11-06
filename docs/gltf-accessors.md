# glTF accessor import

The geometry importer, skin/morph decoder and animation sampler share
`CheckedAccessor` in `include/Container/geometry/GltfAccessor.h`. It reads the
authored buffers without rewriting accessors, changing component types, or
materializing dense copies of sparse arrays. Node transforms, inverse-bind
dequantization and texture transforms retain their source semantics.

## Sparse and checked reads

Dense accessors may have sparse replacements. Accessors without a buffer view
start from zero, whether or not sparse overrides are present. Sparse indices
use exact unsigned 8/16/32-bit reads, must increase
strictly, and must remain inside the accessor count. Both sparse byte offsets
are honored. Sparse views must omit a target and byte stride.

The reader checks buffer/view indices, count, component type, normalization,
offsets, alignment, strides and byte ranges before reading. Arithmetic checks
use subtraction/division before count multiplication. Vertex attribute
elements require four-byte alignment; sparse values use their encoded element
layout. Matrix columns follow glTF column-major order with the required
four-byte padding for small integer matrices. Final trailing column padding
may be absent. Index and joint reads never pass through floating-point values.

The shared view has no count-sized scratch allocation. Geometry, morph deltas,
joint influences and inverse binds stream into the final runtime arrays.
Finite `LoadOptions::maxInputBytes` preflight accounts for encoded input,
declared buffers, converted geometry, retained deformation data and independent
binary64 animation arrays per channel. Animation additionally bounds the total
decoded input/output track arrays to 128 MiB across the library, including
shared samplers expanded by multiple channels. A small sparse payload therefore
cannot evade the decoded memory limits.

Invalid finite/range conversion fails with an accessor label and element/
component location. The existing static importer deliberately repairs invalid
FLOAT normals and tangents; only those frame reads opt into that exception.
Positions, UVs, colors, deformation, animation and DOUBLE conversion remain
strict.

## Quantized geometry

New quantized layouts require `KHR_mesh_quantization` in `extensionsRequired`.
The importer supports signed/unsigned 8/16-bit POSITION values, normalized or
unnormalized, and corresponding signed/unsigned TEXCOORD_0/1 layouts. Normalized
signed BYTE/SHORT normals and tangents retain the existing compatibility path.
Normalized integers use glTF's specified conversion, including clamping the
most-negative signed integer to −1. Raw integer UVs remain raw numeric UVs;
their authored texture transforms perform any dequantization.

Position morph deltas support signed BYTE/SHORT normalized or raw data. Normal
and tangent morph deltas support signed normalized BYTE/SHORT data. These
formats use the same owning node/skin transform as the base geometry. UV/color
morph targets are explicitly rejected because this deformation runtime does
not represent those channels. Quantization remains a partial capability while
that renderer scope is incomplete. No vertex-layout change is needed for
quantization or sparse data.

## Experimental binary64

`KHR_accessor_float64` is a
[draft extension](https://github.com/KhronosGroup/glTF/pull/2397).
The asset opts in by listing it in `extensionsRequired`. The checked reader
also requires its explicit `allowFloat64` option; importer consumers enable
that option only from the required declaration. Undeclared DOUBLE accessors
and normalized DOUBLE data are rejected.

The draft's component type 5130 is accepted where this renderer already
supports FLOAT: mesh attributes, supported morph deltas, weights, inverse-bind
matrices and animation inputs/outputs. Geometry/deformation use checked
binary32 conversion, rejecting nonfinite values, overflow and nonzero values
that would underflow to zero. Ordinary representable rounding remains possible;
the capability diagnostic warns about precision loss. Source bytes and
accessors stay binary64 for saving and inspection.

Animation retains binary64 key times, values, derivatives and interpolation.
FLOAT time bounds are compared at FLOAT precision; DOUBLE bounds retain their
authored double precision. Sampled values are checked before publication to
float-backed runtime properties, including cubic overshoot. Raw cubic tangents
may be wider or smaller than binary32 because their product with a key interval
can produce a representable result. Offline sampling preserves those values;
runtime publication reports unsafe results.

## Validation

`gltf_accessor_tests` exercises exact uint32 boundaries, dense and zero-base
sparse replacements with nonzero offsets, signed normalization, padded small
matrices with absent final padding, malformed ranges/strides/indices, vertex
alignment, binary64 opt-in/range limits, and the FLOAT frame-repair exception.
Public-loader fixtures check source immutability, RGBA color import, quantized
geometry and authored transforms, quantized morph plus binary64 skin data, and
separate sparse geometry/animation memory-budget witnesses.

On 2026-10-25 the isolated MSVC build passed all 119 combined accessor, loader,
deformation, deformation-loader, deformation-frame and animation/runtime
cases. The accessor suite contains 14 focused cases; animation/runtime contains
37. Evidence is in `out/accessor-check/results.xml` and `run.log`. Only existing
alignment/test narrowing warnings were emitted; no shared application build or
shader output was changed by that isolated check.

Animation tests additionally verify binary64 time/interpolation precision,
wide cubic derivatives over tiny intervals, and rejection of unsafe sampled
properties before source/runtime publication. Existing loader/deformation/
animation suites remain regression checks for the shared decoder migration.

The independent opt-in native harness is
`tests/validation/gltf_accessor_runtime_regression.py`. It compares sparse,
quantized and binary64 assets against separately decoded FLOAT oracles in
both raster techniques, with a combined morph/skin fixture and source-save
round trip. Native results require a coordinated application build; CPU tests
alone do not establish renderer parity.

The final accessor/vertex-alpha checkpoint passes 650 cases across 26
executables, including real Vulkan execution with zero skips, and 236 native
captures on executable SHA-256
`cdc23f9806e19c06a4894b306d7d40ee259a7ccb27adf31346f5871df1d90d64`.
Accessor comparisons (40 captures/30 checks) are pixel exact across both raster
techniques and save/reload. Vertex-alpha comparisons (72 captures/50 checks)
cover FLOAT/U8/U16, MASK/BLEND/OPAQUE, transmission, deformation and raster/ray
shadows, with active DDGI tracing and maximum positive mean error 0.01205/255.
The remaining 124 captures rerun the deformation/velocity/shadow matrix after
the vertex ABI change. All native logs are free of Vulkan/synchronization
errors. GPU picking readback remains outside this native harness.
See `out/feature-parity/accessor-validation-checkpoint.json` for report paths,
hashes and the exact test counts.

Format rules follow the
[glTF accessor specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#accessors)
and
[`KHR_mesh_quantization`](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_mesh_quantization/README.md).
