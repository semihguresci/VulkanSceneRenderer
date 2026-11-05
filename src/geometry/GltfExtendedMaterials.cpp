#include <Container/geometry/GltfExtendedMaterials.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::geometry::gltf {
namespace {
using Json = nlohmann::json;
constexpr const char* kAnisotropy = "KHR_materials_anisotropy";
constexpr const char* kDiffuse = "KHR_materials_diffuse_transmission";
constexpr const char* kTransform = "KHR_texture_transform";

[[noreturn]] void fail(std::string_view context, std::string_view reason) {
  throw std::runtime_error("glTF extended materials " + std::string(context) + ": " + std::string(reason));
}

// A shallow read-only adapter shares validation between original JSON and the
// decoded model. No recursive conversion or copy of opaque metadata is needed.
struct View {
  const Json* json{};
  const tinygltf::Value* value{};
  explicit View(const Json& source) : json(&source) {}
  explicit View(const tinygltf::Value& source) : value(&source) {}
  bool object() const { return json ? json->is_object() : value->IsObject(); }
  bool array() const { return json ? json->is_array() : value->IsArray(); }
  bool number() const { return json ? json->is_number() : value->IsNumber(); }
  bool integer() const { return json ? json->is_number_integer() : value->IsInt(); }
  size_t size() const { return json ? json->size() : value->Get<tinygltf::Value::Array>().size(); }
  double real() const { return json ? json->get<double>() : value->GetNumberAsDouble(); }
  View at(size_t index) const {
    return json ? View(json->at(index)) : View(value->Get<tinygltf::Value::Array>().at(index));
  }
  std::optional<View> member(const char* name) const {
    if (json) {
      const auto found = json->find(name);
      if (found != json->end()) return View(*found);
    } else if (value->Has(name)) return View(value->Get(name));
    return std::nullopt;
  }
  uint64_t nonnegative(std::string_view context) const {
    if (!integer()) fail(context, "must be an exact nonnegative integer index");
    if (json) {
      if (!json->is_number_unsigned() && json->get<int64_t>() < 0)
        fail(context, "must be an exact nonnegative integer index");
      return json->get<uint64_t>();
    }
    const auto result = value->Get<int>();
    if (result < 0) fail(context, "must be an exact nonnegative integer index");
    return static_cast<uint64_t>(result);
  }
};

struct Declarations { bool anisotropy{}, diffuse{}, transform{}; };

GltfExtendedMaterialSummary checkedSummary(size_t count, const GltfExtendedMaterialLimits& limits) {
  if (!limits.maxMaterials || limits.maxMaterials > INT_MAX || !limits.maxRetainedBytes ||
      limits.maxRetainedBytes == UINT64_MAX)
    fail("limits", "material and retained-byte limits must be finite and positive within int identity range");
  if (count > limits.maxMaterials) fail("materials", "material count exceeds its limit");
  if (count > std::min<uint64_t>(limits.maxRetainedBytes, SIZE_MAX) / sizeof(GltfExtendedMaterial))
    fail("materials", "local material storage exceeds its retained-byte budget");
  return {uint64_t(count), 0, 0, 0, uint64_t(count) * sizeof(GltfExtendedMaterial)};
}

float scalar(View value, const std::string& context, bool unit = false) {
  if (!value.number()) fail(context, "must be a number");
  const double number = value.real();
  if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
    fail(context, "must be finite and within the binary32 range");
  if (unit && (number < 0 || number > 1)) fail(context, "must be in [0, 1]");
  return static_cast<float>(number);
}

template <glm::length_t Count>
void vector(View value, glm::vec<Count, float>& result, const std::string& context, bool unit = false) {
  if (!value.array() || value.size() != Count) fail(context, "has the wrong numeric vector shape");
  for (glm::length_t i = 0; i < Count; ++i)
    result[i] = scalar(value.at(size_t(i)), context + "[" + std::to_string(i) + "]", unit);
}

uint32_t uvSet(View value, const std::string& context) {
  const auto index = value.nonnegative(context);
  if (index > INT_MAX) fail(context, "UV index exceeds the int identity range");
  return static_cast<uint32_t>(index);
}

void textureInfo(View info, GltfExtendedTextureInfo& result, size_t textureCount,
                 const Declarations& declared, const std::string& context) {
  if (!info.object()) fail(context, "textureInfo must be an object");
  const auto index = info.member("index");
  if (!index) fail(context + ".index", "textureInfo requires an index");
  const auto source = index->nonnegative(context + ".index");
  if (source > INT_MAX || source >= textureCount) fail(context + ".index", "references a missing texture or exceeds int range");
  result.sourceTexture = static_cast<int>(source);
  if (const auto coord = info.member("texCoord"))
    result.sourceTexCoord = uvSet(*coord, context + ".texCoord");
  result.transform.texCoord = result.sourceTexCoord;
  if (const auto extensions = info.member("extensions")) {
    if (!extensions->object()) fail(context + ".extensions", "must be an object");
    if (const auto transform = extensions->member(kTransform)) {
      const auto label = context + ".extensions." + kTransform;
      if (!declared.transform) fail(label, "extension must be declared in extensionsUsed");
      if (!transform->object()) fail(label, "must be an object");
      result.hasTransform = true;
      if (const auto offset = transform->member("offset")) vector(*offset, result.transform.offset, label + ".offset");
      if (const auto scale = transform->member("scale")) vector(*scale, result.transform.scale, label + ".scale");
      if (const auto rotation = transform->member("rotation")) result.transform.rotation = scalar(*rotation, label + ".rotation");
      if (const auto coord = transform->member("texCoord")) result.transform.texCoord = uvSet(*coord, label + ".texCoord");
    }
  }
  if (result.transform.texCoord > 1)
    fail(context + ".texCoord", "effective UV sets above 1 are unsupported by current material consumers");
}

template <typename FindExtension>
GltfExtendedMaterial parseMaterial(FindExtension find, size_t materialIndex, size_t textureCount,
                                   const Declarations& declared) {
  GltfExtendedMaterial result;
  const auto base = "material[" + std::to_string(materialIndex) + "].extensions.";
  for (const auto& [name, present] : std::array{
           std::pair{kAnisotropy, declared.anisotropy}, std::pair{kDiffuse, declared.diffuse}}) {
    const auto extension = find(name);
    if (!extension) continue;
    const auto context = base + name;
    if (!present) fail(context, "extension must be declared in extensionsUsed");
    if (!extension->object()) fail(context, "must be an object");
    if (find("KHR_materials_unlit") || find("KHR_materials_pbrSpecularGlossiness"))
      fail(context, "excludes KHR_materials_unlit and KHR_materials_pbrSpecularGlossiness");
    if (std::string_view(name) == kAnisotropy) {
      result.authoredExtensions |= static_cast<uint8_t>(GltfExtendedMaterialExtension::Anisotropy);
      if (const auto strength = extension->member("anisotropyStrength")) result.anisotropyStrength = scalar(*strength, context + ".anisotropyStrength", true);
      if (const auto rotation = extension->member("anisotropyRotation")) result.anisotropyRotation = scalar(*rotation, context + ".anisotropyRotation");
      if (const auto info = extension->member("anisotropyTexture")) textureInfo(*info, result.anisotropyTexture, textureCount, declared, context + ".anisotropyTexture");
    } else {
      result.authoredExtensions |= static_cast<uint8_t>(GltfExtendedMaterialExtension::DiffuseTransmission);
      if (const auto factor = extension->member("diffuseTransmissionFactor")) result.diffuseTransmissionFactor = scalar(*factor, context + ".diffuseTransmissionFactor", true);
      if (const auto color = extension->member("diffuseTransmissionColorFactor")) vector(*color, result.diffuseTransmissionColorFactor, context + ".diffuseTransmissionColorFactor", true);
      if (const auto info = extension->member("diffuseTransmissionTexture")) textureInfo(*info, result.diffuseTransmissionTexture, textureCount, declared, context + ".diffuseTransmissionTexture");
      if (const auto info = extension->member("diffuseTransmissionColorTexture")) textureInfo(*info, result.diffuseTransmissionColorTexture, textureCount, declared, context + ".diffuseTransmissionColorTexture");
    }
  }
  return result;
}

void accumulate(GltfExtendedMaterialSummary& summary, const GltfExtendedMaterial& material) {
  summary.anisotropyMaterials += material.hasExtension(GltfExtendedMaterialExtension::Anisotropy);
  summary.diffuseTransmissionMaterials += material.hasExtension(GltfExtendedMaterialExtension::DiffuseTransmission);
  summary.textureInfos += (material.anisotropyTexture.sourceTexture >= 0) +
      (material.diffuseTransmissionTexture.sourceTexture >= 0) + (material.diffuseTransmissionColorTexture.sourceTexture >= 0);
}

} // namespace

GltfExtendedMaterialSummary ValidateGltfExtendedMaterialsJson(
    const Json& source, const GltfExtendedMaterialLimits& limits) {
  if (!source.is_object()) fail("asset", "source must be an object");
  const auto materials = source.find("materials"), textures = source.find("textures");
  if (materials != source.end() && !materials->is_array()) fail("materials", "must be an array");
  if (textures != source.end() && !textures->is_array()) fail("textures", "must be an array");
  auto summary = checkedSummary(materials == source.end() ? 0 : materials->size(), limits);
  const auto textureCount = textures == source.end() ? 0 : textures->size();
  if (textureCount > INT_MAX) fail("textures", "texture table exceeds int identity range");
  Declarations declared;
  if (const auto used = source.find("extensionsUsed"); used != source.end()) {
    if (!used->is_array()) fail("extensionsUsed", "must be an array");
    for (const auto& value : *used) {
      if (!value.is_string()) fail("extensionsUsed", "extension names must be strings");
      const auto& name = value.get_ref<const std::string&>();
      declared.anisotropy |= name == kAnisotropy;
      declared.diffuse |= name == kDiffuse;
      declared.transform |= name == kTransform;
    }
  }
  for (size_t i = 0; i < summary.materials; ++i) {
    const auto& material = materials->at(i);
    const auto label = "material[" + std::to_string(i) + "]";
    if (!material.is_object()) fail(label, "must be an object");
    const auto extensions = material.find("extensions");
    if (extensions == material.end()) continue;
    if (!extensions->is_object()) fail(label + ".extensions", "must be an object");
    const auto find = [&](const char* name) -> std::optional<View> {
      const auto found = extensions->find(name);
      return found == extensions->end() ? std::nullopt : std::optional<View>(View(*found));
    };
    accumulate(summary, parseMaterial(find, i, textureCount, declared));
  }
  return summary;
}

GltfExtendedMaterialData ParseGltfExtendedMaterials(
    const tinygltf::Model& source, const GltfExtendedMaterialLimits& limits) {
  GltfExtendedMaterialData result;
  result.summary = checkedSummary(source.materials.size(), limits);
  if (source.textures.size() > INT_MAX) fail("textures", "texture table exceeds int identity range");
  const auto listed = [&](const char* name) { return std::ranges::find(source.extensionsUsed, name) != source.extensionsUsed.end(); };
  const Declarations declared{listed(kAnisotropy), listed(kDiffuse), listed(kTransform)};
  result.materials.reserve(source.materials.size());
  for (size_t i = 0; i < source.materials.size(); ++i) {
    const auto& material = source.materials[i];
    const auto find = [&](const char* name) -> std::optional<View> {
      const auto found = material.extensions.find(name);
      return found == material.extensions.end() ? std::nullopt : std::optional<View>(View(found->second));
    };
    result.materials.push_back(parseMaterial(find, i, source.textures.size(), declared));
    accumulate(result.summary, result.materials.back());
  }
  return result;
}

} // namespace container::geometry::gltf
