# Typed texture decoding

Publication scope: the CPU decoder and environment generation backend are
published together. Typed GPU upload, glTF image selection, texture authoring,
and their editor controls remain developed locally and need later publication.
The integration sections and historical checkpoint below describe that full
local implementation, rather than additional acceptance for this publication.

`TextureImageData.h` provides CPU decoding without creating Vulkan objects. The
caller supplies the material role through `TextureDecodeOptions::isSrgb`, finite
limits, and the formats its device can sample **and filter**. The returned
`VkFormat`, `VkComponentMapping`, and tightly packed mip bytes are one contract:
an uploader must retain all three.

The default limits are 256 MiB encoded bytes, 256 MiB final decoded mip bytes,
16384 pixels per dimension, and 32 levels. Dimensions, output byte arithmetic,
container section ranges, and mip counts are checked before output allocation.
These are data-size limits; STB, libktx, and WebP can allocate additional internal
scratch. This module does not claim an OS-enforced decoder memory/time ceiling.

An empty `allowedFormats` permits precision-preserving **uncompressed** results.
A native block format is returned only when explicitly present in a nonempty
profile. A nonempty profile also restricts every fallback. In particular,
RGBA32 float must be available for HDR or sRGB16; the decoder reports the missing
format rather than reducing these images to eight bits or half float.

## Precision, alpha, and material roles

| Source | Result |
| --- | --- |
| Ordinary 8-bit color/alpha | RGBA8 UNORM or SRGB, alpha unchanged |
| Linear 8-bit grayscale | R8 UNORM with `rrr1` component mapping |
| Linear 16-bit grayscale PNG | R16 UNORM with `rrr1`, original 16-bit values |
| Other linear 16-bit PNG | RGBA16 UNORM, original alpha precision |
| sRGB 16-bit PNG/container | Linear RGBA32 float, sRGB EOTF on RGB only |
| Radiance HDR | Linear RGBA32 float, finite values above one retained |
| Unsupported narrow/BGR/RGB uncompressed format | Precision-preserving RGBA expansion when the profile permits |

The material role selects the transfer function of UNORM image bytes, including
DDS/KTX's paired native UNORM/SRGB formats. Floating-point HDR is already linear.
There are no STB global gamma/vertical-flip changes. Premultiplied DDS/KTX alpha
is rejected because the material paths expect straight alpha. No authored mip
is synthesized, replaced, or discarded.

## Containers and native blocks

DDS handles a single 2D surface, legacy DXT1/3/5 and ATI1/2 (BC1–5), and a documented
set of DX10 UNORM/float/BC formats. KTX1 handles endian conversion, four-byte row
padding, typed channels, BC/ETC2/EAC/ASTC LDR blocks, and key/value metadata. KTX2
checks section overlap, alignment, DFD block dimensions, scalar channel/type
layout, level lengths, and the corresponding mip dimensions. Arrays, cubemaps,
volume/depth images, unknown scalar formats, unsupported transfer functions or
primaries, and unsupported layouts fail with a contextual diagnostic.

BC1–5 UNORM can decode to RGBA8, preserving cutout/continuous alpha and data
channels. BC6H/7, ETC/EAC, ASTC and signed BC formats can be retained when the
profile allows them; otherwise they report that a CPU fallback is unavailable.
There is no claim that libktx decompresses arbitrary native GPU blocks.

General KTX `KTXswizzle` supports `r`, `g`, `b`, `a`, `0`, and `1` for each output
component. Rightward/downward orientation is retained. Rightward/upward images
are row-flipped after uncompressed/BC decoding, or transcoded from Basis to
RGBA first. An upward native compressed image without a CPU decoder is rejected
with an actionable diagnostic. Other axis orientations are explicitly rejected.

`authoredMipChain` is true for DDS/KTX, even for a single level or an incomplete
pyramid. Upload the returned levels exactly and expose only those levels in the
view/sampler. Ordinary PNG/JPEG/HDR/WebP output has one un-authored level, allowing
the caller's existing mip-generation policy to apply separately.

## Optional codecs

`cmake/TextureCodecDependencies.cmake` creates `Dep_TextureCodecs`. Both codec
options default off; the base decoder and ordinary renderer remain usable.

| CMake option | Added capability |
| --- | --- |
| `ENABLE_TEXTURE_KTX` | ETC1S/BasisLZ, UASTC, Zstd/Zlib KTX2 through libktx |
| `ENABLE_TEXTURE_WEBP` | Static WebP RGBA through libwebp; animation rejected |
| `CONTAINER_FETCH_TEXTURE_CODECS` | Explicit opt-in to fetch missing pinned libraries |

Installed CMake packages are tried first. Package-provided aggregate licenses can
be supplied through `CONTAINER_TEXTURE_KTX_LICENSE_FILE` and
`CONTAINER_TEXTURE_WEBP_LICENSE_FILE`. Missing notices fail configuration.

The fetch fallback uses stable KTX-Software 4.4.2 at commit
`4d6fc70eaf62ad0558e63e8d97eb9766118327a6` (archive SHA256
`f75dc5b787189e1cf4473560448628e38eba945ebeed05cbf1d817ef895448fa`) and
libwebp 1.6.0 at commit `4fa21912338357f89e4fd51cf2368325b59e9bd9` (SHA256
`a1e18776b2bfdf59db4089cefb7453d3207bb6e77c24d649e0757e3a08fdca14`).
It links the read-only libraries, disables tools/tests/upload APIs, and stages
upstream licenses/notices under the build's `third_party_licenses`. KTX's
restricted Ericsson ETC software unpacker is disabled. Basis Universal, ASTC,
Zstd, DFD utilities, and uthash notices are retained. WebP's COPYING and PATENTS
are both staged. Dependency options stay local and do not change the parent
project's `ENABLE_TESTS` setting.

Basis targets are selected from the explicit profile: BC7/ASTC, then ETC2/BC3
for ETC1S, otherwise RGBA8. UASTC does not fall back to lossy ETC2/BC3
recompression. Every authored level is transcoded. `HasKtxTextureCodec()` and
`HasWebpTextureCodec()` expose compiled availability to image-source selection.

`strictKhrBasisu` applies only to an image selected by `KHR_texture_basisu`.
It requires ETC1S + BasisLZ or UASTC + none/Zstd; a single 2D image whose width
and height are divisible by four; identity swizzle; `rd` or absent orientation;
straight alpha; and the role-specific DFD transfer and primaries. Generic KTX
loads leave this flag false. Historical zero `bytesPlane[]` in supercompressed
KTX2 is accepted as required by the KTX specification.

## Verification and integration

`tests/utility/texture_image_data_tests.cpp` builds its PNG16, DDS, KTX1 and KTX2
fixtures from independently known values. Tests compare adjacent 16-bit samples,
the analytical sRGB EOTF, alpha interpolation in BC blocks, exact mip bytes,
endianness, swizzles, format-profile fallbacks, finite budgets, and malformed
ranges/metadata. The enabled fixture suite also decodes actual upstream ETC1S, UASTC,
UASTC+Zstd, and native-BC7 transcodes from the small licensed fixtures under
`tests/fixtures/texture-codecs`.

The renderer uses `AllocationManager::createTextureFromImageData` for complete
validated uploads. It checks enabled logical compression features and physical
sample/filter/transfer capabilities. Authored levels are copied exactly; only an
ordinary single-level image can generate mips through supported linear blits.
All raster and compute consumers receive transfer-to-shader barriers. Image views
retain component swizzles and expose only allocated levels.

`PrepareGltfTextures` prepares every used color/data role, including undrawn
variant materials, before live-scene retirement. Its default aggregate output
budget is 512 MiB; each image remains bounded separately. Equal content and
sampler policies reuse resources; different policies share the same uploaded
image. Color and data roles retain distinct format/transfer identities. Manual
file and byte imports use the same precision-preserving decoder. Source image
bytes are retained for portable export, independently of external-file lifetime.
`texture_upload_plan_tests`, `gltf_texture_resources_tests` and
`materialx_integration_tests` cover these additional contracts.
AVIF, animated/array/cube/volume textures, arbitrary color-gamut conversion,
and a general native-block decompressor are outside this module's scope.

Primary references:

- [KTX 1 specification](https://registry.khronos.org/KTX/specs/1.0/ktxspec.v1.html)
- [KTX 2 specification](https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html)
- [KHR_texture_basisu](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_basisu)
- [KTX-Software release and license](https://github.com/KhronosGroup/KTX-Software/tree/v4.4.2)
- [WebP decoding API](https://developers.google.com/speed/webp/docs/api)
- [libwebp 1.6.0 source and terms](https://chromium.googlesource.com/webm/libwebp/+/refs/tags/v1.6.0)
- [Microsoft DDS layout](https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide)

## Validated renderer checkpoint

The frozen build at SHA256
`ab019867be34735e8cce236442f2508b86b8eaddba42719d8f30b306ccf62cc2`
passes 794 cases across 36 executables with no failures/skips and real GPU
execution. Five typed-upload GPU tests verify exact authored R16/float/block
mip bytes and linear/sRGB generation. Texture-codec-OFF integration separately
passes 59 cases across five executables.

The selected native texture matrix passes 78 captures/56 checks across both
raster techniques. Independent PNG16/normal/HDR/container/mip/WebP/UASTC
references and portable reloads are pixel exact; ETC1S differs by at most one
display code. WebP MASK and BLEND have independent alpha/opaque-composition
oracles. The compressed-alpha ray/DDGI receiver witnesses are pixel exact,
trace 2,048 rays each and have zero Vulkan/synchronization errors. Original image
hashes survive GLB and ASCII export/reload after external-file deletion.

Reports and fixture-correction provenance are in
`out/feature-parity/texture-runtime-matrix.json` and
`out/feature-parity/texture-variant-validation-checkpoint.json`. A KTX2 data-URI
startup failure led to the parser normalization regression fix before this
checkpoint; a receiver-width correction strengthened the BLEND full-image
oracle without changing production code or the frozen executable.
