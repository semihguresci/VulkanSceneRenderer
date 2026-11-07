# glTF image source and retention contracts

`SelectTextureImageSources` is the shared metadata-only selector for the loader
and material import. It prefers enabled `KHR_texture_basisu`, then
`MSFT_texture_dds`, then enabled `EXT_texture_webp`, then the core image.
The draft DDS extension uses retained DDS containers and the existing supported
2D formats, mip preservation and device-aware fallback diagnostics. Its source
index, MIME and DDS magic are checked before decoding or cache reuse, and export
remaps its image index. Unavailable optional alternatives require
a valid core reference. Required unavailable codecs, malformed alternate objects,
and negative, fractional or out-of-range image indices fail before resource I/O.
The returned selection records whether strict BasisU validation applies; ordinary
KTX sources do not inherit that extension's additional DFD restrictions.

`LoadOptions::retainEncodedImages` defaults to false. The default ordinary PNG/JPEG
contract remains decoded RGBA, including 16-bit PNG channels. When enabled, image
bytes remain exact encoded containers with `image.as_is=true` and an identified
MIME type. Selected BasisU/DDS/WebP and recognized DDS/KTX/WebP containers retain
encoded bytes automatically because the legacy TinyGLTF decoder cannot represent
their authored formats or mip chains. Unsupported unselected alternatives remain
undecoded in the default mode. `decodeImages=false` always returns empty image
payloads and performs no image CPU decoding, including when retention is requested.

Base64 data URIs for the new MIME types are normalized only in the parser's
detached JSON, because TinyGLTF has a fixed MIME allowlist. Their payload bytes,
including `+`, reach the bounded image callback unchanged. Non-base64 image data
URIs receive an explicit diagnostic instead of being treated as filenames.

Manual texture imports archive recognized containers exactly. TGA/PSD/PNM-style
ordinary images without a recognized portable MIME signature instead retain
decoded RGBA pixels at their original 8/16-bit precision. Scene export emits PNG
for those pixels. The 16-bit PNG writer keeps channel order, alpha and adjacent
sample values, uses big-endian PNG samples and bounded stored DEFLATE blocks,
and requires no optional codec library. GLB and ASCII reload tests cover one
through four source channels and data spanning multiple DEFLATE blocks.

Image buffer views are validated before TinyGLTF can form a pointer to their
contents. Meshopt-compressed image views use private one-byte parser URI reads;
their original references are restored and bytes are extracted only after
detached decompression. No synthetic resource is written to disk. Selected missing
external images fail instead of silently becoming an absent material map.

Encoded image staging and decoded RGBA allocations count against the loader's
aggregate input budget. Ordinary image decoding also has a 512 MiB aggregate
decoded-image limit, a 256 MiB per-image decoded limit, a 16,384 dimension limit,
and a 256 MiB per-image encoded
limit. These data limits are independent of codec scratch memory or GPU memory.

Runtime material preflight calls `DecodeSelectedTextureImage` with a color/data
role and the physical device's allowed format profile before replacing the live
scene. Retained containers use the typed codec pipeline. Legacy decoded images
remain accepted: 16-bit data becomes RGBA16 UNORM; 16-bit color becomes linear
RGBA32 float; neither falls back to eight bits. An unsupported precision-preserving
format is an explicit error. Selected BasisU sources enable strict extension
DFD, metadata and format checks. Preflight may also request those checks for a
core alias of the same image; that strict request cannot be bypassed by decoded
legacy pixels.

`RegenerateGltfBindTangents` supports detached material-variant preparation. Its
per-primitive UV selectors are already resolved by the caller, including texture
transform overrides. It validates all tables first, then uses the importer's
existing MikkTSpace path only where authored normals or tangents were absent.
Authored complete frames and appended procedural meshes remain unchanged.
