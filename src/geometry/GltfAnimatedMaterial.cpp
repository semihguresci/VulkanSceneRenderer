#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/utility/Material.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace container::geometry::gltf {
namespace {
const tinygltf::Value* extension(const tinygltf::Material& source, const char* name) {
  const auto found = source.extensions.find(name);
  return found != source.extensions.end() && found->second.IsObject() ? &found->second : nullptr;
}
float scalar(const tinygltf::Value& object, const char* key, float fallback) {
  return object.Has(key) && object.Get(key).IsNumber() ? static_cast<float>(object.Get(key).GetNumberAsDouble()) : fallback;
}
glm::vec3 vector(const tinygltf::Value& object, const char* key, glm::vec3 fallback) {
  if (!object.Has(key) || !object.Get(key).IsArray() || object.Get(key).ArrayLen() != 3) return fallback;
  const auto& value = object.Get(key);
  for (int i = 0; i < 3; ++i) {
    if (!value.Get(static_cast<size_t>(i)).IsNumber()) return fallback;
    fallback[i] = static_cast<float>(value.Get(static_cast<size_t>(i)).GetNumberAsDouble());
  }
  return fallback;
}
void transform(const tinygltf::Value* value, container::material::TextureTransform& destination) {
  destination.offset = glm::vec2(0);
  destination.scale = glm::vec2(1);
  destination.rotation = 0;
  if (!value || !value->IsObject()) return;
  for (auto [key, result] : {std::pair{"offset", &destination.offset}, std::pair{"scale", &destination.scale}}) {
    if (!value->Has(key) || !value->Get(key).IsArray() || value->Get(key).ArrayLen() != 2) continue;
    for (size_t i = 0; i < 2; ++i)
      if (value->Get(key).Get(i).IsNumber()) (*result)[static_cast<int>(i)] = static_cast<float>(value->Get(key).Get(i).GetNumberAsDouble());
  }
  destination.rotation = scalar(*value,"rotation",0);
}
template<class TextureInfo>
void textureTransform(const TextureInfo& info, container::material::TextureTransform& destination) {
  if (info.index < 0) return;
  const auto found = info.extensions.find("KHR_texture_transform");
  transform(found == info.extensions.end() ? nullptr : &found->second,destination);
}
const tinygltf::Value* textureInfo(const tinygltf::Value* extensionObject, const char* texture) {
  if (!extensionObject || !extensionObject->Has(texture)) return nullptr;
  const auto& value = extensionObject->Get(texture);
  return value.IsObject() && value.Has("index") && value.Get("index").IsInt() && value.Get("index").Get<int>() >= 0 ? &value : nullptr;
}
void textureTransform(const tinygltf::Value* info, container::material::TextureTransform& destination) {
  if (!info) return;
  const tinygltf::Value* value = nullptr;
  if (info->Has("extensions") && info->Get("extensions").IsObject() && info->Get("extensions").Has("KHR_texture_transform"))
    value = &info->Get("extensions").Get("KHR_texture_transform");
  transform(value,destination);
}
void specularGlossinessFactors(const tinygltf::Value& object, container::material::Material& material) {
  glm::vec4 diffuse(1);
  if (object.Has("diffuseFactor") && object.Get("diffuseFactor").IsArray() && object.Get("diffuseFactor").ArrayLen() == 4)
    for (size_t i = 0; i < 4; ++i) if (object.Get("diffuseFactor").Get(i).IsNumber()) diffuse[static_cast<int>(i)] = static_cast<float>(object.Get("diffuseFactor").Get(i).GetNumberAsDouble());
  const auto specular = vector(object,"specularFactor",glm::vec3(1));
  // Match the importer's existing specular/glossiness approximation. Updating
  // another channel in this workflow must not reset it to the core MR defaults.
  const auto d = glm::clamp(glm::vec3(diffuse),0.0f,1.0f);
  const auto s = glm::clamp(specular,0.0f,1.0f);
  const auto brightness = [](glm::vec3 color) { return std::sqrt(glm::dot(color*color,glm::vec3(.299f,.587f,.114f))); };
  const float strength = 1 - std::clamp(std::max({s.r,s.g,s.b}),0.0f,1.0f);
  float metallic = 0;
  if (const float specularBrightness = brightness(s); specularBrightness >= .04f) {
    const float b = brightness(d)*strength/.96f + specularBrightness - .08f;
    metallic = std::clamp((-b + std::sqrt(std::max(b*b - .16f*(.04f-specularBrightness),0.0f)))/.08f,0.0f,1.0f);
  }
  const auto fromDiffuse = d*strength/std::max(.96f*(1-metallic),1e-6f);
  const auto fromSpecular = (s-glm::vec3(.04f*(1-metallic)))/std::max(metallic,1e-6f);
  material.baseColor = glm::vec4(glm::clamp(glm::mix(fromDiffuse,fromSpecular,metallic*metallic),0.0f,1.0f),std::clamp(diffuse.a,0.0f,1.0f));
  material.metallicFactor = metallic;
  material.roughnessFactor = std::clamp(scalar(object,"glossinessFactor",1),0.0f,1.0f);
  const float maximum = std::max({specular.r,specular.g,specular.b});
  material.specularFactor = std::clamp(maximum/.04f,0.0f,1.0f);
  material.specularColorFactor = maximum > 1e-6f ? specular/maximum : glm::vec3(1);
}
} // namespace

void UpdateAnimatedMaterialProperties(const tinygltf::Material& source,
                                      container::material::Material& material) {
  if (source.pbrMetallicRoughness.baseColorFactor.size() == 4)
    for (int i = 0; i < 4; ++i) material.baseColor[i] = static_cast<float>(source.pbrMetallicRoughness.baseColorFactor[static_cast<size_t>(i)]);
  if (source.emissiveFactor.size() == 3)
    for (int i = 0; i < 3; ++i) material.emissiveColor[i] = static_cast<float>(source.emissiveFactor[static_cast<size_t>(i)]);
  material.metallicFactor = static_cast<float>(source.pbrMetallicRoughness.metallicFactor);
  material.roughnessFactor = static_cast<float>(source.pbrMetallicRoughness.roughnessFactor);
  material.alphaCutoff = static_cast<float>(source.alphaCutoff);
  material.normalTextureScale = static_cast<float>(source.normalTexture.scale);
  material.occlusionStrength = static_cast<float>(source.occlusionTexture.strength);
  if (const auto* e = extension(source,"KHR_materials_emissive_strength")) material.emissiveStrength = scalar(*e,"emissiveStrength",1);
  if (const auto* e = extension(source,"KHR_materials_ior")) material.ior = scalar(*e,"ior",1.5f);
  if (const auto* e = extension(source,"KHR_materials_dispersion")) material.dispersion = scalar(*e,"dispersion",0);
  if (const auto* e = extension(source,"KHR_materials_transmission")) material.transmissionFactor = scalar(*e,"transmissionFactor",0);
  if (const auto* e = extension(source,"KHR_materials_volume")) {
    material.thicknessFactor = scalar(*e,"thicknessFactor",0);
    material.attenuationDistance = scalar(*e,"attenuationDistance",std::numeric_limits<float>::infinity());
    material.attenuationColor = vector(*e,"attenuationColor",glm::vec3(1));
  }
  if (const auto* e = extension(source,"KHR_materials_clearcoat")) {
    material.clearcoatFactor = scalar(*e,"clearcoatFactor",0);
    material.clearcoatRoughnessFactor = scalar(*e,"clearcoatRoughnessFactor",0);
    const auto* normal = textureInfo(e,"clearcoatNormalTexture");
    material.clearcoatNormalTextureScale = normal ? scalar(*normal,"scale",1) : 1;
  }
  if (const auto* e = extension(source,"KHR_materials_specular")) {
    material.specularFactor = scalar(*e,"specularFactor",1);
    material.specularColorFactor = vector(*e,"specularColorFactor",glm::vec3(1));
  }
  if (const auto* e = extension(source,"KHR_materials_sheen")) {
    material.sheenColorFactor = vector(*e,"sheenColorFactor",glm::vec3(0));
    material.sheenRoughnessFactor = scalar(*e,"sheenRoughnessFactor",0);
  }
  if (const auto* e = extension(source,"KHR_materials_iridescence")) {
    material.iridescenceFactor = scalar(*e,"iridescenceFactor",0);
    material.iridescenceIor = scalar(*e,"iridescenceIor",1.3f);
    material.iridescenceThicknessMinimum = scalar(*e,"iridescenceThicknessMinimum",100);
    material.iridescenceThicknessMaximum = scalar(*e,"iridescenceThicknessMaximum",400);
  }
  if (const auto* e = extension(source,"KHR_materials_anisotropy")) {
    material.anisotropyStrength = scalar(*e,"anisotropyStrength",0);
    material.anisotropyRotation = scalar(*e,"anisotropyRotation",0);
  }
  if (const auto* e = extension(source,"KHR_materials_diffuse_transmission")) {
    material.diffuseTransmissionFactor = scalar(*e,"diffuseTransmissionFactor",0);
    material.diffuseTransmissionColorFactor = vector(*e,"diffuseTransmissionColorFactor",glm::vec3(1));
  }
  textureTransform(source.pbrMetallicRoughness.baseColorTexture,material.baseColorTextureTransform);
  textureTransform(source.normalTexture,material.normalTextureTransform);
  textureTransform(source.occlusionTexture,material.occlusionTextureTransform);
  textureTransform(source.emissiveTexture,material.emissiveTextureTransform);
  textureTransform(source.pbrMetallicRoughness.metallicRoughnessTexture,material.metallicRoughnessTextureTransform);
  if (!extension(source,"KHR_materials_pbrSpecularGlossiness")) {
    textureTransform(source.pbrMetallicRoughness.metallicRoughnessTexture,material.roughnessTextureTransform);
    textureTransform(source.pbrMetallicRoughness.metallicRoughnessTexture,material.metalnessTextureTransform);
  }
  struct Slot { const char* extension; const char* texture; container::material::TextureTransform container::material::Material::* member; };
  for (const auto& slot : {
    Slot{"KHR_materials_specular","specularTexture",&container::material::Material::specularTextureTransform},
    Slot{"KHR_materials_specular","specularColorTexture",&container::material::Material::specularColorTextureTransform},
    Slot{"KHR_materials_transmission","transmissionTexture",&container::material::Material::transmissionTextureTransform},
    Slot{"KHR_materials_clearcoat","clearcoatTexture",&container::material::Material::clearcoatTextureTransform},
    Slot{"KHR_materials_clearcoat","clearcoatRoughnessTexture",&container::material::Material::clearcoatRoughnessTextureTransform},
    Slot{"KHR_materials_clearcoat","clearcoatNormalTexture",&container::material::Material::clearcoatNormalTextureTransform},
    Slot{"KHR_materials_volume","thicknessTexture",&container::material::Material::thicknessTextureTransform},
    Slot{"KHR_materials_sheen","sheenColorTexture",&container::material::Material::sheenColorTextureTransform},
    Slot{"KHR_materials_sheen","sheenRoughnessTexture",&container::material::Material::sheenRoughnessTextureTransform},
    Slot{"KHR_materials_iridescence","iridescenceTexture",&container::material::Material::iridescenceTextureTransform},
    Slot{"KHR_materials_iridescence","iridescenceThicknessTexture",&container::material::Material::iridescenceThicknessTextureTransform},
    Slot{"KHR_materials_anisotropy","anisotropyTexture",&container::material::Material::anisotropyTextureTransform},
    Slot{"KHR_materials_diffuse_transmission","diffuseTransmissionTexture",&container::material::Material::diffuseTransmissionTextureTransform},
    Slot{"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture",&container::material::Material::diffuseTransmissionColorTextureTransform}})
    textureTransform(textureInfo(extension(source,slot.extension),slot.texture),material.*slot.member);
  if (const auto* e = extension(source,"KHR_materials_pbrSpecularGlossiness")) {
    specularGlossinessFactors(*e,material);
    textureTransform(textureInfo(e,"diffuseTexture"),material.baseColorTextureTransform);
    const auto* packed = textureInfo(e,"specularGlossinessTexture");
    textureTransform(packed,material.roughnessTextureTransform);
    textureTransform(packed,material.specularColorTextureTransform);
  }
}
} // namespace container::geometry::gltf
