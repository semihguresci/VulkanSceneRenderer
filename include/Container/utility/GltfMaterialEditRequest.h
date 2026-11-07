#pragma once

#include <Container/geometry/GltfMaterialAuthoring.h>
#include <Container/geometry/GltfMaterialTextureAuthoring.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace container::material {

enum class MaterialEditGesturePhase { Commit, Begin, Update, End, Cancel };

// Common capture/UI payload for one existing source material. The UI/backend
// supplies scene identity/generation witnesses separately; no runtime material
// offset or resource handle is inferred from this source ID.
struct GltfMaterialEditRequest {
  int32_t sourceMaterial{};
  container::geometry::gltf::GltfMaterialPatch patch;
  std::optional<uint64_t> gestureId;
  MaterialEditGesturePhase phase{MaterialEditGesturePhase::Commit};
};

// Texture editing is a discrete resource/material transaction. Public requests
// carry user intent and source catalog IDs, never GPU handles or owner proofs.
enum class MaterialTextureEditOperation : uint8_t {
  ImportFile, BindExisting, Clear, EditMapping, EditSampler
};
struct MaterialTextureSamplerDescription {
  int minFilter{-1}, magFilter{-1}, wrapS{10497}, wrapT{10497};
  bool operator==(const MaterialTextureSamplerDescription&) const = default;
};
struct GltfMaterialTextureEditRequest {
  int32_t sourceMaterial{};
  container::geometry::gltf::GltfMaterialTextureSlot slot{};
  MaterialTextureEditOperation operation{MaterialTextureEditOperation::Clear};
  std::optional<std::string> path;
  std::optional<uint32_t> sourceTexture;
  std::optional<MaterialTextureSamplerDescription> sampler;
  container::geometry::gltf::GltfMaterialTextureMappingPatch mapping;
  // UI always supplies the catalog revision. Capture may omit it and lets the
  // coordinator resolve the current owner before preparing any resources.
  std::optional<uint64_t> expectedResourceRevision;
};

inline constexpr std::array<std::string_view,23> kMaterialTextureSlotNames{
  "baseColor", "normal", "occlusion", "emissive", "metallicRoughness",
  "roughness", "metalness", "specular", "height", "opacity", "transmission",
  "specularColor", "clearcoat", "clearcoatRoughness", "clearcoatNormal",
  "thickness", "sheenColor", "sheenRoughness", "iridescence",
  "iridescenceThickness", "anisotropy", "diffuseTransmission", "diffuseTransmissionColor"};
static_assert(kMaterialTextureSlotNames.size()==size_t(container::geometry::gltf::GltfMaterialTextureSlot::Count));
[[nodiscard]] inline std::string_view MaterialTextureSlotName(
    container::geometry::gltf::GltfMaterialTextureSlot slot) {
  if(size_t(slot)>=kMaterialTextureSlotNames.size()) throw std::invalid_argument("Material texture slot is invalid");
  return kMaterialTextureSlotNames[size_t(slot)];
}
[[nodiscard]] inline std::optional<container::geometry::gltf::GltfMaterialTextureSlot>
FindMaterialTextureSlot(std::string_view name) noexcept {
  for(size_t i=0;i<kMaterialTextureSlotNames.size();++i)
    if(kMaterialTextureSlotNames[i]==name) return container::geometry::gltf::GltfMaterialTextureSlot(i);
  return {};
}
[[nodiscard]] inline bool MaterialTextureMappingEmpty(
    const container::geometry::gltf::GltfMaterialTextureMappingPatch& mapping) noexcept {
  return !mapping.texCoord && !mapping.channel && !mapping.offset && !mapping.scale && !mapping.rotation;
}
inline void ValidateMaterialTextureSampler(const MaterialTextureSamplerDescription& sampler) {
  const bool min=sampler.minFilter==-1 || sampler.minFilter==9728 || sampler.minFilter==9729 ||
      (sampler.minFilter>=9984 && sampler.minFilter<=9987);
  const bool mag=sampler.magFilter==-1 || sampler.magFilter==9728 || sampler.magFilter==9729;
  const auto wrap=[](int value) {return value==10497 || value==33071 || value==33648;};
  if(!min || !mag || !wrap(sampler.wrapS) || !wrap(sampler.wrapT))
    throw std::invalid_argument("Material sampler policy is invalid");
}
inline void ValidateMaterialTextureMapping(
    const container::geometry::gltf::GltfMaterialTextureMappingPatch& mapping) {
  if(mapping.texCoord) if(const auto* value=std::get_if<uint32_t>(&*mapping.texCoord);value && *value>1)
    throw std::invalid_argument("Material texture coordinates must use UV0 or UV1");
  if(mapping.channel) if(const auto* value=std::get_if<uint32_t>(&*mapping.channel);value && *value>4)
    throw std::invalid_argument("Material texture channel is invalid");
  const auto finite=[](double value) {
    if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())
      throw std::invalid_argument("Material texture mapping must be finite and representable in binary32");
  };
  if(mapping.rotation) if(const auto* value=std::get_if<double>(&*mapping.rotation)) finite(*value);
  for(const auto* item:{&mapping.offset,&mapping.scale})
    if(*item) if(const auto* value=std::get_if<std::array<double,2>>(&**item))
      for(double component:*value) finite(component);
}
inline void ValidateMaterialTextureEditRequest(const GltfMaterialTextureEditRequest& request) {
  if(request.sourceMaterial<0 || size_t(request.slot)>=kMaterialTextureSlotNames.size())
    throw std::invalid_argument("Choose an editable source material and texture slot first");
  if(request.expectedResourceRevision && !*request.expectedResourceRevision)
    throw std::invalid_argument("Material texture resource revision must be nonzero");
  if(request.path && (request.path->empty() || request.path->size()>65536 || request.path->find('\0')!=std::string::npos))
    throw std::invalid_argument("Material texture path must be a bounded nonempty path");
  if(request.sourceTexture && *request.sourceTexture>uint32_t(std::numeric_limits<int32_t>::max()))
    throw std::invalid_argument("Material source texture identity is invalid");
  if(request.sampler) ValidateMaterialTextureSampler(*request.sampler);
  ValidateMaterialTextureMapping(request.mapping);
  const bool mapping=!MaterialTextureMappingEmpty(request.mapping);
  switch(request.operation) {
  case MaterialTextureEditOperation::ImportFile:
    if(!request.path || request.sourceTexture) throw std::invalid_argument("Texture import requires only a file source");
    break;
  case MaterialTextureEditOperation::BindExisting:
    if(!request.sourceTexture || request.path) throw std::invalid_argument("Texture binding requires only an existing source identity");
    break;
  case MaterialTextureEditOperation::Clear:
    if(request.path || request.sourceTexture || request.sampler || mapping)
      throw std::invalid_argument("Clearing a texture slot cannot also edit its settings");
    break;
  case MaterialTextureEditOperation::EditMapping:
    if(!mapping || request.path || request.sourceTexture || request.sampler)
      throw std::invalid_argument("Texture mapping requires only mapping values");
    break;
  case MaterialTextureEditOperation::EditSampler:
    if(!request.sampler || request.path || request.sourceTexture || mapping)
      throw std::invalid_argument("Texture sampler editing requires only a sampler policy");
    break;
  default:throw std::invalid_argument("Material texture operation is invalid");
  }
}

} // namespace container::material
