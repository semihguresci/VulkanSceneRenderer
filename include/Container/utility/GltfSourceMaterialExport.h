#pragma once

#include <Container/utility/GltfMaterialConversion.h>

namespace container::material {

struct GltfSourceMaterialExportLimits {
  GltfMaterialConversionLimits conversion{};
  uint64_t maxOwnedBytes{64ull*1024*1024};
};

// Lossless export of a source-backed SG material. E supplies the saved pose and
// metadata; the owner's explicit archive->E texture-ID map supplies identities.
// Every E texture must have exactly one archive identity. No GPU handle is
// reversed. Both forward conversions must reproduce the complete runtime value.
// Unknown normative metadata rejects when source IDs require remapping; extras
// remain opaque annotations. Only selected records and compact ID scratch copy.
[[nodiscard]] tinygltf::Material ExportSourceBackedGltfMaterial(
    const Material& runtime,const tinygltf::Material& authored,const tinygltf::Material& evaluated,
    GltfMaterialConversionContext archiveContext,GltfMaterialConversionContext evaluatedContext,
    std::span<const int32_t> archiveTextureToEvaluated,
    const GltfSourceMaterialExportLimits& limits={});

} // namespace container::material
