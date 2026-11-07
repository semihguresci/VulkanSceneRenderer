#pragma once

#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace container::geometry::gltf {

// Stable order shared by conversion, export, runtime provenance and the editor.
enum class GltfMaterialTextureSlot : uint8_t {
  BaseColor, Normal, Occlusion, Emissive, MetallicRoughness, Roughness,
  Metalness, Specular, Height, Opacity, Transmission, SpecularColor,
  Clearcoat, ClearcoatRoughness, ClearcoatNormal, Thickness, SheenColor,
  SheenRoughness, Iridescence, IridescenceThickness, Anisotropy,
  DiffuseTransmission, DiffuseTransmissionColor, Count
};
struct GltfMaterialTextureSlotInfo {
  GltfMaterialTextureSlot slot;
  std::string_view label, path, extension;
  bool srgb{};
  uint32_t channel{4};
};
[[nodiscard]] std::span<const GltfMaterialTextureSlotInfo> GltfMaterialTextureSlotRegistry() noexcept;
[[nodiscard]] const GltfMaterialTextureSlotInfo& GltfMaterialTextureSlotDescription(GltfMaterialTextureSlot);

// optional absent = untouched; monostate = reset/omit; T = explicit set.
using GltfMaterialTextureUIntValue = std::variant<std::monostate, uint32_t>;
using GltfMaterialTextureScalarValue = std::variant<std::monostate, double>;
using GltfMaterialTextureVectorValue = std::variant<std::monostate, std::array<double,2>>;
struct GltfMaterialTextureMappingPatch {
  std::optional<GltfMaterialTextureUIntValue> texCoord, channel;
  std::optional<GltfMaterialTextureVectorValue> offset, scale;
  std::optional<GltfMaterialTextureScalarValue> rotation;
};
enum class GltfMaterialTextureOperation : uint8_t { Bind, Clear, Mapping };
struct GltfMaterialTextureSourcePair {
  int32_t authoredTexture{-1}, evaluatedTexture{-1};
  bool operator==(const GltfMaterialTextureSourcePair&) const noexcept = default;
};
struct GltfMaterialTextureEdit {
  GltfMaterialTextureSlot slot{};
  GltfMaterialTextureOperation operation{GltfMaterialTextureOperation::Mapping};
  std::optional<GltfMaterialTextureSourcePair> binding;
  GltfMaterialTextureMappingPatch mapping;
};
struct GltfMaterialTextureBinding {
  int32_t index{-1};
  uint32_t texCoord{}, channel{4};
  std::array<double,2> offset{0.,0.}, scale{1.,1.};
  double rotation{};
  bool legacy{}, channelEditable{};
  std::optional<GltfMaterialTextureSlot> coupledSlot;
  // Borrowed static strings identifying the effective workflow source field.
  std::string_view path, extension;
};
// Reads only known selected-slot fields; the caller bounds resident metadata.
[[nodiscard]] GltfMaterialTextureBinding ReadGltfMaterialTextureBinding(
    const tinygltf::Material&, GltfMaterialTextureSlot);
void ValidateGltfMaterialTextureSourceReferences(const tinygltf::Material&, size_t textureCount);

// Internal pure planner operations, also useful to bounded inspectors. They
// touch selected material records only, never models/images/resources.
enum GltfMaterialTextureOwnership : uint8_t {
  TextureBinding = 1, TextureTexCoord = 2, TextureOffset = 4,
  TextureScale = 8, TextureRotation = 16, TextureChannel = 32,
  TextureContainer = 64
};
[[nodiscard]] uint8_t GltfMaterialTextureEditOwnership(const GltfMaterialTextureEdit&) noexcept;
void ValidateGltfMaterialTextureEdits(std::span<const GltfMaterialTextureEdit>,
    const tinygltf::Material& authored, const tinygltf::Material& evaluated,
    size_t authoredTextures, size_t evaluatedTextures);
void ApplyGltfMaterialTextureEdits(tinygltf::Material&,
    std::span<const GltfMaterialTextureEdit>, bool evaluated);
// Exact trusted endpoint leaves, preserving unrelated CURRENT evaluated fields.
void CopyGltfMaterialTextureEditLeaves(tinygltf::Material& target,
    const tinygltf::Material& trusted, GltfMaterialTextureSlot, uint8_t ownership);

} // namespace container::geometry::gltf
