#include <Container/utility/GltfMaterialConversion.h>
#include <Container/geometry/GltfExtendedMaterials.h>

#include <Container/geometry/GltfAnimationRuntime.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::material {
namespace {
using Slot = GltfMaterialTextureSlot;
using Value = tinygltf::Value;
constexpr auto invalid = std::numeric_limits<uint32_t>::max();
constexpr size_t slotCount = size_t(Slot::Count);
constexpr std::array<uint32_t Material::*, slotCount> members{
    &Material::baseColorTextureIndex, &Material::normalTextureIndex,
    &Material::occlusionTextureIndex, &Material::emissiveTextureIndex,
    &Material::metallicRoughnessTextureIndex, &Material::roughnessTextureIndex,
    &Material::metalnessTextureIndex, &Material::specularTextureIndex,
    &Material::heightTextureIndex, &Material::opacityTextureIndex,
    &Material::transmissionTextureIndex, &Material::specularColorTextureIndex,
    &Material::clearcoatTextureIndex, &Material::clearcoatRoughnessTextureIndex,
    &Material::clearcoatNormalTextureIndex, &Material::thicknessTextureIndex,
    &Material::sheenColorTextureIndex, &Material::sheenRoughnessTextureIndex,
    &Material::iridescenceTextureIndex, &Material::iridescenceThicknessTextureIndex,
    &Material::anisotropyTextureIndex, &Material::diffuseTransmissionTextureIndex,
    &Material::diffuseTransmissionColorTextureIndex};
constexpr std::array<TextureTransform Material::*, slotCount> transforms{
    &Material::baseColorTextureTransform, &Material::normalTextureTransform,
    &Material::occlusionTextureTransform, &Material::emissiveTextureTransform,
    &Material::metallicRoughnessTextureTransform, &Material::roughnessTextureTransform,
    &Material::metalnessTextureTransform, &Material::specularTextureTransform,
    &Material::heightTextureTransform, &Material::opacityTextureTransform,
    &Material::transmissionTextureTransform, &Material::specularColorTextureTransform,
    &Material::clearcoatTextureTransform, &Material::clearcoatRoughnessTextureTransform,
    &Material::clearcoatNormalTextureTransform, &Material::thicknessTextureTransform,
    &Material::sheenColorTextureTransform, &Material::sheenRoughnessTextureTransform,
    &Material::iridescenceTextureTransform, &Material::iridescenceThicknessTextureTransform,
    &Material::anisotropyTextureTransform, &Material::diffuseTransmissionTextureTransform,
    &Material::diffuseTransmissionColorTextureTransform};
[[noreturn]] void fail(std::string_view reason) {
  throw std::invalid_argument("glTF material conversion: " + std::string(reason));
}
float finiteFloat(double value, std::string_view name) {
  if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
    fail(std::string(name) + " must be finite and representable as binary32");
  return static_cast<float>(value);
}
int exactInteger(double value, int maximum, std::string_view name) {
  if (!std::isfinite(value) || value < 0 || value > maximum || std::trunc(value) != value)
    fail(std::string(name) + " must be an exact nonnegative integer in range");
  return static_cast<int>(value);
}
const Value* extension(const tinygltf::Material& source, const char* name) {
  const auto found = source.extensions.find(name);
  return found == source.extensions.end() ? nullptr : &found->second;
}
void applyTransform(TextureTransform& target, int coord, const tinygltf::ExtensionMap& extensions) {
  target.texCoord = uint32_t(coord);
  const auto found = extensions.find("KHR_texture_transform");
  if (found == extensions.end()) return;
  const auto& source = found->second;
  if (source.Has("texCoord")) target.texCoord = uint32_t(source.Get("texCoord").Get<int>());
  if (source.Has("rotation")) target.rotation = float(source.Get("rotation").GetNumberAsDouble());
  for (const auto [name, member] : {std::pair{"offset", &TextureTransform::offset}, std::pair{"scale", &TextureTransform::scale}})
    if (source.Has(name)) for (size_t i = 0; i < 2; ++i)
      (target.*member)[int(i)] = float(source.Get(name).Get(i).GetNumberAsDouble());
}
TextureTransform objectTransform(const Value& info, uint32_t channel = 4) {
  if (info.Has("channel")) fail("nonstandard texture-info channel is unsupported; use a typed legacy texture parameter");
  TextureTransform result;
  result.channel = channel;
  const int coord = info.Has("texCoord") ? info.Get("texCoord").Get<int>() : 0;
  if (info.Has("extensions")) applyTransform(result, coord, info.Get("extensions").Get<Value::Object>());
  else result.texCoord = uint32_t(coord);
  return result;
}
struct Builder {
  const GltfMaterialConversionContext& context;
  ConvertedGltfMaterial result;
  void bind(Slot slot, int index, TextureTransform transform = {},bool packedSpecularGlossiness=false) {
    const auto n = size_t(slot);
    if (index == -1) return;
    if (index < 0 || size_t(index) >= context.textureCount) fail("source texture reference is out of range");
    const bool color=GltfMaterialTextureUsesColorResource(slot,packedSpecularGlossiness);
    const auto role = color ? context.colorResources : context.dataResources;
    const auto resource = role[size_t(index)];
    if (resource == invalid) fail(color ? "referenced color texture role has not been uploaded" : "referenced data texture role has not been uploaded");
    if (resource >= context.runtimeResourceCount) fail("referenced runtime texture resource is out of range");
    result.sourceTextures[n] = index;
    result.value.*members[n] = resource;
    result.value.*transforms[n] = transform;
  }
  template<class Info> void core(Slot slot, const Info& info) {
    if (info.index < 0) return;
    TextureTransform transform;
    applyTransform(transform, info.texCoord, info.extensions);
    bind(slot, info.index, transform);
  }
  void object(Slot slot, const Value* parent, const char* name, uint32_t channel = 4,bool packedSpecularGlossiness=false) {
    if (!parent || !parent->Has(name)) return;
    const auto& info = parent->Get(name);
    bind(slot, info.Get("index").Get<int>(), objectTransform(info, channel),packedSpecularGlossiness);
  }
  void legacy(Slot slot, const tinygltf::ParameterMap& values, const char* name) {
    const auto found = values.find(name);
    if (found == values.end()) return;
    const auto& object = found->second.json_double_value;
    const auto index = object.find("index");
    if (index == object.end()) fail(std::string(name) + " has no typed texture index");
    const int source = exactInteger(index->second, INT_MAX, name);
    if (size_t(source) >= context.textureCount) fail(std::string(name) + " references a missing source texture");
    TextureTransform transform;
    if (const auto coord = object.find("texCoord"); coord != object.end()) transform.texCoord = uint32_t(exactInteger(coord->second, 1, "legacy texture UV"));
    if (const auto channel = object.find("channel"); channel != object.end()) transform.channel = uint32_t(exactInteger(channel->second, 4, "legacy texture channel"));
    for(const auto [name,target]:{std::pair{"offsetX",&transform.offset.x},std::pair{"offsetY",&transform.offset.y},
        std::pair{"scaleX",&transform.scale.x},std::pair{"scaleY",&transform.scale.y},std::pair{"rotation",&transform.rotation}})
      if(const auto entry=object.find(name);entry!=object.end()) *target=finiteFloat(entry->second,name);
    // Validate even a lower-precedence fallback before ignoring its binding.
    if (result.sourceTextures[size_t(slot)] < 0) bind(slot, source, transform);
  }
};
void legacyFactor(const tinygltf::ParameterMap& values, const char* name, float& target, bool unit) {
  const auto found = values.find(name);
  if (found == values.end()) return;
  if (!found->second.has_number_value) fail(std::string(name) + " must be a typed numeric legacy factor");
  const auto value = finiteFloat(found->second.number_value, name);
  if (unit && (value < 0 || value > 1)) fail(std::string(name) + " must be in [0,1]");
  target = value;
}
} // namespace

bool GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot slot,bool specularGlossinessWorkflow) noexcept {
  return slot==GltfMaterialTextureSlot::BaseColor || slot==GltfMaterialTextureSlot::Emissive ||
      slot==GltfMaterialTextureSlot::SpecularColor || slot==GltfMaterialTextureSlot::SheenColor ||
      slot==GltfMaterialTextureSlot::DiffuseTransmissionColor ||
      (specularGlossinessWorkflow && slot==GltfMaterialTextureSlot::Roughness);
}
bool GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot slot,const Material& material) noexcept {
  const bool packed=material.specularGlossinessWorkflow && material.roughnessTextureIndex!=invalid &&
      material.roughnessTextureIndex==material.specularColorTextureIndex;
  return GltfMaterialTextureUsesColorResource(slot,packed);
}

ConvertedGltfMaterial ConvertGltfMaterial(const tinygltf::Material& source,
    GltfMaterialConversionContext context, GltfMaterialConversionPolicy policy,
    const GltfMaterialConversionLimits& limits) {
  if (!limits.maxSourceTextures || limits.maxSourceTextures == UINT64_MAX || !limits.maxRuntimeResources || limits.maxRuntimeResources == UINT64_MAX ||
      context.textureCount > limits.maxSourceTextures || context.textureCount > INT_MAX ||
      context.runtimeResourceCount > limits.maxRuntimeResources || context.runtimeResourceCount > UINT32_MAX ||
      context.colorResources.size() != context.textureCount || context.dataResources.size() != context.textureCount)
    fail("resource table shape or finite capacity limit is invalid");
  if (policy != GltfMaterialConversionPolicy::StrictGltf && policy != GltfMaterialConversionPolicy::LegacyImportCompatibility)
    fail("unknown conversion policy");
  auto validation = limits.validation;
  if (validation.maxOutputBytes == UINT64_MAX || validation.maxOutputBytes <= sizeof(ConvertedGltfMaterial)) fail("conversion output exceeds its finite byte budget");
  validation.maxOutputBytes -= sizeof(ConvertedGltfMaterial);
  geometry::gltf::ValidateGltfMaterialRecord(source, {1, context.textureCount, context.extensionsUsed, {}}, validation);

  Builder builder{context};
  auto& value = builder.result.value;
  geometry::gltf::UpdateAnimatedMaterialProperties(source, value);
  if (context.enableExperimentalMaterials) {
    const auto layer = geometry::gltf::ParseGltfExperimentalMaterial(source, context.extensionsUsed, context.textureCount);
    value.experimental = geometry::gltf::BindGltfExperimentalMaterial(layer, [&](int texture, bool color) {
      if (texture < 0) return invalid;
      const auto resource = (color ? context.colorResources : context.dataResources)[size_t(texture)];
      if (resource == invalid || resource >= context.runtimeResourceCount)
        fail("experimental material texture role is not available");
      return resource;
    });
  }
  // The numeric updater preserves runtime UV/channel choices by contract.
  // Rebuild all such choices from source here, including absent-slot defaults.
  for (size_t i = 0; i < slotCount; ++i) value.*transforms[i] = TextureTransform{};
  value.diffuseTransmissionTextureTransform.channel = 3;
  value.doubleSided = source.doubleSided;
  value.alphaMode = source.alphaMode == "MASK" ? AlphaMode::Mask : source.alphaMode == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
  value.unlit = extension(source, "KHR_materials_unlit") != nullptr;
  const auto* sg = extension(source, "KHR_materials_pbrSpecularGlossiness");
  value.specularGlossinessWorkflow = sg != nullptr;
  builder.core(Slot::Normal, source.normalTexture);
  builder.core(Slot::Occlusion, source.occlusionTexture);
  builder.core(Slot::Emissive, source.emissiveTexture);
  if (sg) {
    builder.object(Slot::BaseColor, sg, "diffuseTexture");
    builder.object(Slot::Roughness, sg, "specularGlossinessTexture",4,true);
    builder.object(Slot::SpecularColor, sg, "specularGlossinessTexture");
  } else {
    builder.core(Slot::BaseColor, source.pbrMetallicRoughness.baseColorTexture);
    builder.core(Slot::MetallicRoughness, source.pbrMetallicRoughness.metallicRoughnessTexture);
  }
  struct ExtensionSlot { Slot slot; const char* extension; const char* texture; uint32_t channel{4}; };
  constexpr std::array slots{
      ExtensionSlot{Slot::Specular, "KHR_materials_specular", "specularTexture"},
      ExtensionSlot{Slot::SpecularColor, "KHR_materials_specular", "specularColorTexture"},
      ExtensionSlot{Slot::Transmission, "KHR_materials_transmission", "transmissionTexture"},
      ExtensionSlot{Slot::Clearcoat, "KHR_materials_clearcoat", "clearcoatTexture"},
      ExtensionSlot{Slot::ClearcoatRoughness, "KHR_materials_clearcoat", "clearcoatRoughnessTexture"},
      ExtensionSlot{Slot::ClearcoatNormal, "KHR_materials_clearcoat", "clearcoatNormalTexture"},
      ExtensionSlot{Slot::Thickness, "KHR_materials_volume", "thicknessTexture"},
      ExtensionSlot{Slot::SheenColor, "KHR_materials_sheen", "sheenColorTexture"},
      ExtensionSlot{Slot::SheenRoughness, "KHR_materials_sheen", "sheenRoughnessTexture"},
      ExtensionSlot{Slot::Iridescence, "KHR_materials_iridescence", "iridescenceTexture"},
      ExtensionSlot{Slot::IridescenceThickness, "KHR_materials_iridescence", "iridescenceThicknessTexture"},
      ExtensionSlot{Slot::Anisotropy, "KHR_materials_anisotropy", "anisotropyTexture"},
      ExtensionSlot{Slot::DiffuseTransmission, "KHR_materials_diffuse_transmission", "diffuseTransmissionTexture", 3},
      ExtensionSlot{Slot::DiffuseTransmissionColor, "KHR_materials_diffuse_transmission", "diffuseTransmissionColorTexture"}};
  for (const auto& slot : slots) builder.object(slot.slot, extension(source, slot.extension), slot.texture, slot.channel);
  struct LegacySlot { Slot slot; const char* name; };
  constexpr std::array legacySlots{
      LegacySlot{Slot::Roughness, "roughnessTexture"}, LegacySlot{Slot::Metalness, "metalnessTexture"},
      LegacySlot{Slot::Specular, "specularTexture"}, LegacySlot{Slot::Height, "heightTexture"},
      LegacySlot{Slot::Height, "displacementTexture"}, LegacySlot{Slot::Opacity, "opacityTexture"},
      LegacySlot{Slot::Transmission, "refractionTexture"}, LegacySlot{Slot::Transmission, "transmissionTexture"}};
  for (const auto& slot : legacySlots) for (const auto* map : {&source.values, &source.additionalValues})
    builder.legacy(slot.slot, *map, slot.name);
  for (const auto* map : {&source.values, &source.additionalValues}) {
    legacyFactor(*map, "opacity", value.opacityFactor, true);
    // Physical source objects and the SG approximation remain authoritative.
    // Validate legacy fallbacks even when those higher-priority values win.
    auto specular = value.specularFactor;
    auto transmission = value.transmissionFactor;
    legacyFactor(*map, "specular", specular, true);
    legacyFactor(*map, "refraction", transmission, true);
    if (!sg && !extension(source, "KHR_materials_specular")) value.specularFactor = specular;
    if (!sg && !extension(source, "KHR_materials_transmission")) value.transmissionFactor = transmission;
    legacyFactor(*map, "heightScale", value.heightScale, false);
    legacyFactor(*map, "heightOffset", value.heightOffset, false);
  }
  if (value.alphaMode == AlphaMode::Opaque && (value.opacityFactor < .999f || value.opacityTextureIndex != invalid))
    value.alphaMode = AlphaMode::Blend;
  if (!sg && policy == GltfMaterialConversionPolicy::LegacyImportCompatibility) {
    const auto authored = [&](const auto& map) {const auto found = map.find("metallicFactor"); return found != map.end() && found->second.has_number_value;};
    if (!authored(source.values) && !authored(source.additionalValues) &&
        std::abs(source.pbrMetallicRoughness.metallicFactor - 1.) <= 1e-6 &&
        value.metallicRoughnessTextureIndex == invalid && value.metalnessTextureIndex == invalid)
      value.metallicFactor = 0;
  }
  for (size_t i = 0; i < slotCount; ++i) builder.result.runtimeBindings.runtimeIndices[i] = value.*members[i];
  return builder.result;
}
} // namespace container::material
