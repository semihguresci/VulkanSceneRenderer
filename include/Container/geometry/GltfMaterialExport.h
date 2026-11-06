#pragma once

#include <Container/utility/Material.h>
#include <tiny_gltf.h>
#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace container::geometry::gltf {

// Append resources from an independently imported asset. Returned materials
// reference the combined destination texture table; geometry is not appended.
std::vector<tinygltf::Material> appendExportResources(
    tinygltf::Model& destination, const tinygltf::Model& source,
    const std::filesystem::path& sourcePath);

// Captured once after import. Distinct source texture objects may share a GPU
// resource, so identity must be retained per material slot, not per resource.
struct ImportedMaterialTextureBindings {
  std::array<uint32_t, 23> runtimeIndices = [] {
    std::array<uint32_t, 23> indices;
    indices.fill(std::numeric_limits<uint32_t>::max());
    return indices;
  }();
};
ImportedMaterialTextureBindings snapshotMaterialTextureBindings(const material::Material& material);

// Append a detached core glTF texture with a scalar channel copied into its
// standard slot. The source texture and its other consumers remain intact.
int32_t appendRepackedScalarTexture(tinygltf::Model& model, int32_t sourceTexture,
                                   uint32_t fromChannel, uint32_t toChannel,
                                   uint64_t& remainingPixelBytes);

// Texture mapping is runtime resource index -> exported glTF texture index.
// Unknown optional material metadata is retained from the imported definition.
// With bindings captured at import, unchanged slots retain their imported,
// already-remapped texture indices even when GPU resources deduplicate.
// Custom roughness/metalness/height/opacity bindings require that explicit
// retained source proof. Their recognized legacy source fields are preserved
// by the project scene writer, without a generic runtime override extension.
tinygltf::Material exportMaterial(
    const material::Material& material, std::span<const int32_t> textureMapping,
    const tinygltf::Material* imported = nullptr,
    const ImportedMaterialTextureBindings* importedBindings = nullptr);

// Collects material and nested texture-info extension names into extensionsUsed
// after the final runtime material table has replaced the imported table.
void appendMaterialExtensionDeclarations(tinygltf::Model& model);

} // namespace container::geometry::gltf
