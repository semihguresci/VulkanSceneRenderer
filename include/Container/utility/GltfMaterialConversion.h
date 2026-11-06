#pragma once

#include <Container/geometry/GltfMaterialAuthoring.h>
#include <Container/geometry/GltfMaterialExport.h>
#include <Container/utility/Material.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace container::material {

// Order matches ImportedMaterialTextureBindings; source identity is retained
// independently of resource deduplication, image names and sampler sharing.
using GltfMaterialTextureSlot = container::geometry::gltf::GltfMaterialTextureSlot;
// SG's packed specular/glossiness texture has color RGB and linear alpha.
// Both logical consumers share its Color resource; alpha is not sRGB decoded.
// MR roughness and the other scalar/normal slots retain their Data resources.
// The bool overload describes a known packed SG source binding. For resident
// materials use the Material overload: an SG extension can instead retain an
// independent legacy Roughness Data binding without a packed SG texture.
[[nodiscard]] bool GltfMaterialTextureUsesColorResource(
    GltfMaterialTextureSlot slot, bool specularGlossinessWorkflow) noexcept;
[[nodiscard]] bool GltfMaterialTextureUsesColorResource(
    GltfMaterialTextureSlot slot, const Material& material) noexcept;
enum class GltfMaterialConversionPolicy : uint8_t {
  StrictGltf,
  // Explicit compatibility with the old importer's omitted-metallic heuristic.
  // Never promotes texture-backed zero factors or black colors to one.
  LegacyImportCompatibility
};
struct GltfMaterialConversionContext {
  size_t textureCount{};
  std::span<const std::string> extensionsUsed;
  std::span<const uint32_t> colorResources, dataResources;
  size_t runtimeResourceCount{};
  bool enableExperimentalMaterials{};
};
struct GltfMaterialConversionLimits {
  uint64_t maxSourceTextures{1'000'000};
  uint64_t maxRuntimeResources{1'000'000};
  container::geometry::gltf::GltfMaterialAuthoringLimits validation{};
};
struct ConvertedGltfMaterial {
  Material value{};
  std::array<int32_t, size_t(GltfMaterialTextureSlot::Count)> sourceTextures = [] {
    std::array<int32_t, size_t(GltfMaterialTextureSlot::Count)> result;
    result.fill(-1);
    return result;
  }();
  container::geometry::gltf::ImportedMaterialTextureBindings runtimeBindings{};
};

// Validates and converts ONE detached source record. Context tables are borrowed
// and source-indexed; only referenced roles are read. Missing uploaded roles
// reject explicitly. No model/document/image/buffer/resource table is copied.
// Output starts fresh, so removed extensions/classification/bindings reset.
// Typed legacy opacity/specular/refraction/height factors and texture bindings
// are retained; legacy texture parameters may select UV0/1 and channel0..4,
// with explicit offsetX/offsetY/scaleX/scaleY/rotation numeric components.
[[nodiscard]] ConvertedGltfMaterial ConvertGltfMaterial(
    const tinygltf::Material& evaluated, GltfMaterialConversionContext context,
    GltfMaterialConversionPolicy policy = GltfMaterialConversionPolicy::StrictGltf,
    const GltfMaterialConversionLimits& limits = {});

} // namespace container::material
