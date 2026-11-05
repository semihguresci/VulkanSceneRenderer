# glTF image sources and typed textures

Scene import retains original encoded image bytes and selects each texture's
source before GPU upload. Enabled BasisU sources take precedence over DDS,
WebP and the ordinary core fallback. A required unavailable decoder rejects the
asset; an optional unavailable alternate uses its valid core fallback. Selected
missing images, invalid source indices and malformed buffer views produce an
image-specific error before scene replacement.

The reader handles external files, data URIs and GLB buffer views. New container
MIME types reach the image decoder without corrupting base64 `+` characters.
Compressed image views wait for checked meshopt normalization. Encoded reads,
decoded geometry and retained/decoded image copies share finite input budgets;
texture preparation additionally bounds the aggregate decoded image output.

`LoadOptions::retainEncodedImages` selects the retained-byte contract. Ordinary
source-only callers keep the existing decoded PNG/JPEG contract by default;
`decodeImages=false` leaves image payloads empty. The scene renderer opts into
retention so authored mips, HDR values and 16-bit precision reach the typed
decoder and Vulkan uploader unchanged.

Texture preparation determines color and data roles from authored material
slots, including unused variant materials. One glTF texture can have separate
color and data resources. Sampler policies can share the same uploaded image;
content identity includes role, format, component mapping and every authored
mip. Material color slots resolve their original source texture IDs directly,
including the packed specular/glossiness alpha alias. Data-resource deduplication
cannot redirect a color slot to another source texture.

Color conversion happens once. Color PNG16 is converted to linear floating
point without eight-bit truncation; normal/scalar data retains its linear
interpretation. Authored zero factors and black colors remain zero/black when
a texture is present. Sampling uses the [dense sampler scene
consumer](material-sampler-publication.md).

Replacement prepares image roles and validates decoding before retiring live
assets. Initial import, replacement and default-scene appends use the same typed
producer. Invalid retained images and invalid sampler indices reject without
changing the live scene. Later interactive resource transactions and portable
scene-authoring publication remain under #108.

The default extension catalog describes the complete editor's capabilities.
An active scene consumer can restrict that catalog through
`LoadOptions::unavailableRuntimeExtensions`. Required restrictions fail before
external resource reads; optional restrictions produce fallback diagnostics.
The published static scene consumer currently restricts the separately developed
node interaction, instancing, animation-pointer, material-variant, anisotropy and
diffuse-transmission runtime integrations. Source metadata, CPU evaluators and
the full local editor retain their existing capabilities. These renderer/editor
integrations remain tracked; source parsing does not complete their issues.

Optional meshoptimizer and Draco decoding uses pinned dependencies and retains
their MIT and Apache-2.0 notices. Draco runs in a bounded worker with checked
protocol, process-memory/time limits and exact integer attribute transport.
Building the viewer builds its required worker. Windows packages include that
worker, its runtime dependencies and both enabled decoder notices; missing
required payloads reject packaging.

Run `gltf_image_sources_tests`, `gltf_texture_resources_tests`,
`gltf_capabilities_tests`, `materialx_integration_tests` and the compression/
accessor/deformation reference suites for CPU verification. The real Vulkan
scene sampler tests include malformed retained-image replacement. Native
`gltf_texture_formats_runtime_regression.py` compares generated formats against
independent PNG/float references and deliberately dropped/truncated controls,
including authored mips, PNG16, HDR, BasisU and WebP alpha through ray/DDGI
traversal. Save/reload options require the full editor's separate export path.
`gltf_texture_role_runtime_regression.py` renders one texture shared by emission
and normal slots. Analytic linear emission and independent floating-point normal
references check both roles; deliberately wrong gamma/linear controls must
produce a visible difference.
