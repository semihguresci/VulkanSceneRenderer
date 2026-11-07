# Compressed glTF geometry

Geometry compression is optional. The default build keeps both decoders disabled;
ordinary geometry and valid optional-extension fallback data continue to load.
A required compression extension fails with an actionable capability diagnostic
when its decoder is unavailable. Missing fallback bytes are never substituted
with geometry made from zeros.

| Extension | Enabled implementation | Scope |
| --- | --- | --- |
| `EXT_meshopt_compression` | meshoptimizer | Ratified extension; attribute version 0, index versions 0/1, all three modes and the four specified filters including `NONE` |
| `KHR_meshopt_compression` | meshoptimizer | **Release Candidate** extension; attribute/index versions 0/1, all three modes, and `NONE`, `OCTAHEDRAL`, `QUATERNION`, `EXPONENTIAL`, `COLOR` filters |
| `KHR_draco_mesh_compression` | Official Draco in a bounded subprocess | Triangle lists, mapped typed attributes and unsigned indices; triangle strips fail explicitly |

Meshopt decoding checks mode, filter, count, stride, bitstream version, encoded
range and destination size before allocating output. It supports every permitted
filter stride, including four- and eight-byte KHR color streams. KHR and EXT
extension objects cannot coexist on a single view or fallback-tagged buffer.
Separate views may share an untagged real destination buffer. Source fallback
tags and URI-less destination buffers must reference only matching compressed
views; a URI-less destination also requires that exact extension.

Draco decoding currently requires Windows. The client starts
`ContainerDracoWorker.exe` hidden and suspended, applies a Job Object with finite
process-memory, CPU and process-count limits, then resumes it. A wall timeout
terminates the job. The worker checks that it is bounded before reading a
request. Its versioned little-endian protocol checks lengths, counts, attributes,
scalar types and complete response size. See [Draco worker details](gltf-draco-worker.md).
The application and TinyGLTF do not link an in-process Draco decoder.

## Build and package

Enable either backend through CMake:

```text
-DENABLE_GLTF_MESHOPT=ON
-DENABLE_GLTF_DRACO=ON
```

The dependency module first looks for installed config packages. Meshoptimizer
must expose the stable version-1.1 color/filter APIs; Draco must be at least
1.5.9. To fetch missing dependencies explicitly, also set
`-DCONTAINER_FETCH_GLTF_COMPRESSION=ON`. Fetched archives are pinned and verified:

| Dependency | Version / commit | Archive SHA256 |
| --- | --- | --- |
| meshoptimizer | 1.3 / `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` | `0a26e578edbe4c392fdc7c41033d50cde2abbf8f598888b1abef6a6333418f49` |
| Draco | 1.5.9 / `554628821c3448319d7d4ae48db170b8c63e1aaf` | `7c83c9c8e96e264e97d91209dc5e9dd3fc999adf88704abe68065752196e3d36` |

Builds stage upstream notices in `third_party_licenses/meshoptimizer-MIT.txt` and
`third_party_licenses/Draco-Apache-2.0.txt`. A config package must provide its
upstream notice alongside its configuration, or its license file must be set
explicitly with `CONTAINER_GLTF_MESHOPT_LICENSE_FILE` /
`CONTAINER_GLTF_DRACO_LICENSE_FILE`. Missing notices stop configuration. A Draco
package must include the worker executable beside the renderer and any runtime
DLL dependencies; it must not enable TinyGLTF's eager Draco decoding.

## Allocation and ownership

`LoadOptions::compression.maxDecodedBytes` defaults to **256 MiB**. It is a
conservative aggregate reservation for appended decoded streams and Draco
response/conversion storage; encoded source buffers remain part of the separate
input allocation policy. Draco's default process-memory limit is 512 MiB and its
wall timeout is 30 seconds. These CPU load options can be made stricter by the
caller. Count/stride arithmetic is checked before allocation and all decoder
failures occur before a replacement scene can be published.

`InspectCompressedGltfSource` validates compression metadata before TinyGLTF
loads resource buffers. Its placeholder plan records logical destination sizes.
The loader supplies only one-byte private stand-ins to TinyGLTF and passes the
original plan to `DecodeCompressedGltf`; normalized decoded views replace every
reference to those stand-ins. No full zero-filled destination allocation is
needed. The planner reports decoded storage separately so input preflight can
charge declared source buffers and decoder output together.

`DecodeCompressedGltf` consumes a detached model. Passing an lvalue copies it;
moving a newly parsed model avoids another encoded-buffer copy. It never edits
caller-owned input on failure and publishes statistics only on success. Ordinary
models bypass compression metadata construction. Original encoded bytes remain
unchanged. Meshopt redirects existing views to appended decoded buffers; Draco
appends per-primitive accessors/views, retaining source numeric and sparse
metadata. Narrow Draco vertex elements receive four-byte padded strides without
altering their component values or normalization flags. Several semantics can
alias one Draco attribute ID when their component counts agree; the worker reads
that ID once and each semantic retains its own declared accessor metadata.

The decoded model uses ordinary checked accessors for static meshes, sparse
overrides, animation, skin joints/weights and morph targets. Draco preserves its
**decoded point order**. Encoders may reorder or deduplicate their input, so
ordinary attributes, fallback geometry and morph deltas supplied alongside Draco
must already match that decoded order and count. The loader cannot infer or
repair an exporter that supplies incompatible original-order arrays.

After successful normalization, consumed compression objects/declarations are
removed from the detached model. Portable scene exports therefore contain normal
decoded streams and do not require either decoder to reopen. Other source
extensions, extras, accessor formats and encoded-resource bytes are retained.

## Verification

Fixtures are generated with the pinned official encoders; no downloaded model
is used as a test oracle. CPU tests cover mode/version/stride combinations,
filters, invalid metadata and payloads, budgets, optional disabled-backend
fallback, placeholders, immutable source storage, sparse overrides, mapped
Draco attributes, padded normalized narrow attributes, and portable glTF/GLB
reload. The subprocess tests separately cover bounded launch, memory denial,
timeout, malformed/oversized responses and cleanup confinement. Native renderer
comparisons pass on the integrated decoder-enabled build: 675 cases across 29
executables, zero skips, and 50 native captures/45 checks across five scenes.
Both raster techniques match independent plain references and decoded glTF/GLB
reloads pixel exactly, including compressed animation/skin/morph and a lit
Draco normal-map/alpha witness. Zero Vulkan/synchronization errors occur.
All 93 executable/shader manifest entries stay unchanged. The combined report
preserves six excluded fixture-only aborted captures and explains their oracle
corrections. Evidence: `out/feature-parity/compression-validation-checkpoint.json`
and `out/feature-parity/compression-runtime-matrix.json`.

Primary references: [ratified EXT meshopt specification](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Vendor/EXT_meshopt_compression/README.md),
[KHR meshopt Release Candidate](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_meshopt_compression/README.md),
[Draco glTF specification](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_draco_mesh_compression/README.md),
[meshoptimizer 1.3 source](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4),
[Draco 1.5.9 source](https://github.com/google/draco/tree/554628821c3448319d7d4ae48db170b8c63e1aaf).
