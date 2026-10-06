
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/Material.h"
#include "Container/utility/TextureResource.h"
#include "Container/utility/TextureManager.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneData.h"
#include <MaterialXCore/Material.h>
#include <MaterialXCore/Util.h>
#include <MaterialXCore/Value.h>
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
MaterialX::Color3 parseColorOrDefault(const MaterialX::ValuePtr& value,
                                      const MaterialX::Color3& fallback) {
  if (!value) {
    return fallback;
  }

  if (value->isA<MaterialX::Color3>()) {
    return value->asA<MaterialX::Color3>();
  }

  if (value->isA<MaterialX::Color4>()) {
    auto asColor4 = value->asA<MaterialX::Color4>();
    return MaterialX::Color3(asColor4[0], asColor4[1], asColor4[2]);
  }

  auto maybeColor =
      MaterialX::fromValueString<MaterialX::Color3>(value->getValueString());
  if (maybeColor != MaterialX::Color3()) {
    return maybeColor;
  }

  return fallback;
}

std::string resolveImageFileInput(const MaterialX::InputPtr& input) {
  if (!input) {
    return {};
  }

  auto node = input->getConnectedNode();
  if (!node) {
    return {};
  }

  if (node->getCategory() == "image") {
    if (auto fileInput = node->getInput("file")) {
      return fileInput->getValueString();
    }
  }

  for (const auto& childInput : node->getInputs()) {
    auto fileName = resolveImageFileInput(childInput);
    if (!fileName.empty()) {
      return fileName;
    }
  }

  return {};
}

constexpr const char* kSpecularGlossinessExtension =
    "KHR_materials_pbrSpecularGlossiness";
constexpr const char* kTextureTransformExtension = "KHR_texture_transform";

const tinygltf::Value* findMaterialExtension(
    const tinygltf::Material& material, const char* extensionName) {
  const auto it = material.extensions.find(extensionName);
  if (it == material.extensions.end() || !it->second.IsObject()) {
    return nullptr;
  }
  return &it->second;
}

float readExtensionNumber(const tinygltf::Value* extension,
                          const char* valueName, float fallback) {
  if (extension == nullptr || !extension->Has(valueName)) {
    return fallback;
  }

  const auto& value = extension->Get(valueName);
  if (!value.IsNumber()) {
    return fallback;
  }

  return static_cast<float>(value.GetNumberAsDouble());
}

glm::vec3 readExtensionVec3(const tinygltf::Value* extension,
                            const char* valueName,
                            const glm::vec3& fallback) {
  if (extension == nullptr || !extension->Has(valueName)) {
    return fallback;
  }

  const auto& value = extension->Get(valueName);
  if (!value.IsArray() || value.ArrayLen() < 3) {
    return fallback;
  }

  glm::vec3 result{0.0f};
  for (size_t i = 0; i < 3; ++i) {
    const auto& component = value.Get(i);
    if (!component.IsNumber()) {
      return fallback;
    }
    result[static_cast<int>(i)] =
        static_cast<float>(component.GetNumberAsDouble());
  }
  return result;
}

glm::vec4 readExtensionVec4(const tinygltf::Value* extension,
                            const char* valueName,
                            const glm::vec4& fallback) {
  if (extension == nullptr || !extension->Has(valueName)) {
    return fallback;
  }

  const auto& value = extension->Get(valueName);
  if (!value.IsArray() || value.ArrayLen() < 4) {
    return fallback;
  }

  glm::vec4 result{0.0f};
  for (size_t i = 0; i < 4; ++i) {
    const auto& component = value.Get(i);
    if (!component.IsNumber()) {
      return fallback;
    }
    result[static_cast<int>(i)] =
        static_cast<float>(component.GetNumberAsDouble());
  }
  return result;
}

int readTextureIndexObject(const tinygltf::Value& textureObject) {
  if (!textureObject.IsObject() || !textureObject.Has("index")) {
    return -1;
  }

  const auto& indexValue = textureObject.Get("index");
  if (!indexValue.IsNumber()) {
    return -1;
  }

  return indexValue.GetNumberAsInt();
}

float readTextureScaleObject(const tinygltf::Value& textureObject,
                             float fallback) {
  if (!textureObject.IsObject() || !textureObject.Has("scale")) {
    return fallback;
  }

  const auto& scaleValue = textureObject.Get("scale");
  if (!scaleValue.IsNumber()) {
    return fallback;
  }

  return static_cast<float>(scaleValue.GetNumberAsDouble());
}

glm::vec2 readTextureTransformVec2(const tinygltf::Value* extension,
                                   const char* valueName,
                                   const glm::vec2& fallback) {
  if (extension == nullptr || !extension->Has(valueName)) {
    return fallback;
  }

  const auto& value = extension->Get(valueName);
  if (!value.IsArray() || value.ArrayLen() < 2) {
    return fallback;
  }

  const auto& x = value.Get(0);
  const auto& y = value.Get(1);
  if (!x.IsNumber() || !y.IsNumber()) {
    return fallback;
  }

  return {static_cast<float>(x.GetNumberAsDouble()),
          static_cast<float>(y.GetNumberAsDouble())};
}

uint32_t readTextureTransformTexCoord(const tinygltf::Value* extension,
                                      uint32_t fallback) {
  if (extension == nullptr || !extension->Has("texCoord")) {
    return fallback;
  }

  const auto& value = extension->Get("texCoord");
  if (!value.IsNumber()) {
    return fallback;
  }

  const int texCoord = value.GetNumberAsInt();
  return texCoord >= 0 ? static_cast<uint32_t>(texCoord) : fallback;
}

float readTextureTransformRotation(const tinygltf::Value* extension,
                                   float fallback) {
  if (extension == nullptr || !extension->Has("rotation")) {
    return fallback;
  }

  const auto& value = extension->Get("rotation");
  if (!value.IsNumber()) {
    return fallback;
  }

  return static_cast<float>(value.GetNumberAsDouble());
}

template <typename TextureInfo>
container::material::TextureTransform readTextureTransform(
    const TextureInfo& textureInfo) {
  container::material::TextureTransform transform{};
  transform.texCoord =
      textureInfo.texCoord >= 0 ? static_cast<uint32_t>(textureInfo.texCoord)
                                : 0u;

  const auto extensionIt =
      textureInfo.extensions.find(kTextureTransformExtension);
  const tinygltf::Value* extension =
      extensionIt != textureInfo.extensions.end() && extensionIt->second.IsObject()
          ? &extensionIt->second
          : nullptr;

  transform.offset = readTextureTransformVec2(
      extension, "offset", transform.offset);
  transform.rotation =
      readTextureTransformRotation(extension, transform.rotation);
  transform.scale = readTextureTransformVec2(
      extension, "scale", transform.scale);
  transform.texCoord =
      readTextureTransformTexCoord(extension, transform.texCoord);
  return transform;
}

container::material::TextureTransform readTextureTransformObject(
    const tinygltf::Value& textureObject) {
  container::material::TextureTransform transform{};
  if (!textureObject.IsObject()) {
    return transform;
  }

  if (textureObject.Has("texCoord")) {
    const auto& texCoordValue = textureObject.Get("texCoord");
    if (texCoordValue.IsNumber()) {
      const int texCoord = texCoordValue.GetNumberAsInt();
      transform.texCoord = texCoord >= 0 ? static_cast<uint32_t>(texCoord) : 0u;
    }
  }

  const tinygltf::Value* extension = nullptr;
  if (textureObject.Has("extensions")) {
    const auto& extensions = textureObject.Get("extensions");
    if (extensions.IsObject() && extensions.Has(kTextureTransformExtension)) {
      const auto& textureTransform = extensions.Get(kTextureTransformExtension);
      if (textureTransform.IsObject()) {
        extension = &textureTransform;
      }
    }
  }

  transform.offset = readTextureTransformVec2(
      extension, "offset", transform.offset);
  transform.rotation =
      readTextureTransformRotation(extension, transform.rotation);
  transform.scale = readTextureTransformVec2(
      extension, "scale", transform.scale);
  transform.texCoord =
      readTextureTransformTexCoord(extension, transform.texCoord);
  return transform;
}

const tinygltf::Value* findExtensionTextureObject(
    const tinygltf::Material& material, const char* extensionName,
    const char* textureName) {
  const auto* extension = findMaterialExtension(material, extensionName);
  if (extension == nullptr || !extension->Has(textureName)) {
    return nullptr;
  }

  const auto& textureObject = extension->Get(textureName);
  return textureObject.IsObject() ? &textureObject : nullptr;
}

int readExtensionTextureIndex(const tinygltf::Material& material,
                              const char* extensionName,
                              const char* textureName) {
  const tinygltf::Value* textureObject =
      findExtensionTextureObject(material, extensionName, textureName);
  if (textureObject == nullptr) {
    return -1;
  }

  return readTextureIndexObject(*textureObject);
}

int readLegacyTextureIndex(const tinygltf::ParameterMap& values,
                           const char* textureName) {
  const auto it = values.find(textureName);
  if (it == values.end()) {
    return -1;
  }
  return it->second.TextureIndex();
}

float readLegacyFactor(const tinygltf::ParameterMap& values,
                       const char* factorName, float fallback) {
  const auto it = values.find(factorName);
  if (it == values.end() || !it->second.has_number_value) {
    return fallback;
  }
  return static_cast<float>(it->second.Factor());
}

bool hasLegacyNumber(const tinygltf::ParameterMap& values,
                     const char* factorName) {
  const auto it = values.find(factorName);
  return it != values.end() && it->second.has_number_value;
}

bool hasAuthoredMaterialNumber(const tinygltf::Material& material,
                               const char* factorName) {
  return hasLegacyNumber(material.values, factorName) ||
         hasLegacyNumber(material.additionalValues, factorName);
}

bool hasAuthoredPbrFactor(const tinygltf::Material& material,
                          const char* factorName, double parsedValue,
                          double defaultValue) {
  return hasAuthoredMaterialNumber(material, factorName) ||
         std::abs(parsedValue - defaultValue) > 1e-6;
}

uint32_t resolveTextureIndex(const tinygltf::Model& model,
                             const std::vector<uint32_t>& textureToResource,
                             int textureIndex) {
  if (textureIndex < 0 ||
      static_cast<size_t>(textureIndex) >= model.textures.size()) {
    return std::numeric_limits<uint32_t>::max();
  }

  if (static_cast<size_t>(textureIndex) >= textureToResource.size()) {
    return std::numeric_limits<uint32_t>::max();
  }

  return textureToResource[static_cast<size_t>(textureIndex)];
}

uint32_t gltfWrapModeToMaterialSamplerAxis(int wrapMode) {
  switch (wrapMode) {
    case TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:
      return container::gpu::kMaterialSamplerWrapClampToEdge;
    case TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT:
      return container::gpu::kMaterialSamplerWrapMirroredRepeat;
    case TINYGLTF_TEXTURE_WRAP_REPEAT:
    default:
      return container::gpu::kMaterialSamplerWrapRepeat;
  }
}

uint32_t gltfTextureSamplerIndex(const tinygltf::Model& model,
                                 const tinygltf::Texture& texture) {
  uint32_t wrapS = container::gpu::kMaterialSamplerWrapRepeat;
  uint32_t wrapT = container::gpu::kMaterialSamplerWrapRepeat;
  if (texture.sampler >= 0 &&
      texture.sampler < static_cast<int>(model.samplers.size())) {
    const tinygltf::Sampler& sampler =
        model.samplers[static_cast<size_t>(texture.sampler)];
    wrapS = gltfWrapModeToMaterialSamplerAxis(sampler.wrapS);
    wrapT = gltfWrapModeToMaterialSamplerAxis(sampler.wrapT);
  }

  return wrapS + wrapT * container::gpu::kMaterialSamplerWrapModeCount;
}

float maxComponent(const glm::vec3& color) {
  return std::max({color.r, color.g, color.b});
}

float perceivedBrightness(const glm::vec3& color) {
  return std::sqrt(0.299f * color.r * color.r +
                   0.587f * color.g * color.g +
                   0.114f * color.b * color.b);
}

float solveMetallic(const glm::vec3& diffuse, const glm::vec3& specular,
                    float oneMinusSpecularStrength) {
  constexpr float kDielectricSpecular = 0.04f;
  constexpr float kEpsilon = 1e-6f;

  const float specularBrightness = perceivedBrightness(specular);
  if (specularBrightness < kDielectricSpecular) {
    return 0.0f;
  }

  const float diffuseBrightness = perceivedBrightness(diffuse);
  const float a = kDielectricSpecular;
  const float b = diffuseBrightness * oneMinusSpecularStrength /
                      (1.0f - kDielectricSpecular) +
                  specularBrightness - 2.0f * kDielectricSpecular;
  const float c = kDielectricSpecular - specularBrightness;
  const float discriminant = std::max(b * b - 4.0f * a * c, 0.0f);
  const float metallic = (-b + std::sqrt(discriminant)) /
                         std::max(2.0f * a, kEpsilon);
  return std::clamp(metallic, 0.0f, 1.0f);
}

struct MetallicRoughnessApproximation {
  glm::vec4 baseColor{1.0f};
  float metallic{0.0f};
  float roughness{1.0f};
};

MetallicRoughnessApproximation convertSpecularGlossinessToMetallicRoughness(
    const glm::vec4& diffuseFactor, const glm::vec3& specularFactor,
    float glossinessFactor) {
  constexpr float kDielectricSpecular = 0.04f;
  constexpr float kEpsilon = 1e-6f;

  const glm::vec3 diffuse = glm::clamp(glm::vec3(diffuseFactor), 0.0f, 1.0f);
  const glm::vec3 specular = glm::clamp(specularFactor, 0.0f, 1.0f);
  const float oneMinusSpecularStrength =
      1.0f - std::clamp(maxComponent(specular), 0.0f, 1.0f);
  const float metallic =
      solveMetallic(diffuse, specular, oneMinusSpecularStrength);

  const glm::vec3 baseColorFromDiffuse =
      diffuse * oneMinusSpecularStrength /
      std::max((1.0f - kDielectricSpecular) * (1.0f - metallic), kEpsilon);
  const glm::vec3 baseColorFromSpecular =
      (specular - glm::vec3(kDielectricSpecular * (1.0f - metallic))) /
      std::max(metallic, kEpsilon);
  const glm::vec3 baseColor = glm::clamp(
      glm::mix(baseColorFromDiffuse, baseColorFromSpecular,
               metallic * metallic),
      0.0f, 1.0f);

  return {glm::vec4(baseColor, std::clamp(diffuseFactor.a, 0.0f, 1.0f)),
          metallic, 1.0f - std::clamp(glossinessFactor, 0.0f, 1.0f)};
}
}  // namespace

namespace container::material {

SlangMaterialXBridge::SlangMaterialXBridge() = default;

MaterialX::DocumentPtr SlangMaterialXBridge::loadDocument(
    const std::string& filename) {
  auto document = MaterialX::createDocument();
  MaterialX::XmlReadOptions options;

  MaterialX::readFromXmlFile(document, filename, MaterialX::FileSearchPath(),
                             &options);
  document->importLibrary(MaterialX::createDocument());
  return document;
}

glm::vec4 SlangMaterialXBridge::extractBaseColor(
    const MaterialX::DocumentPtr& document) const {
  const MaterialX::Color3 defaultColor(1.0f, 1.0f, 1.0f);

  if (!document) {
    return glm::vec4(defaultColor[0], defaultColor[1], defaultColor[2], 1.0f);
  }

  // Look for standard surface shaders
  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput("base_color")) {
      auto baseColor = parseColorOrDefault(input->getValue(), defaultColor);
      return glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
    }
  }

  // Look for GLSL shaders
  auto glslShaders = document->getNodesOfType("glsl");
  for (const auto& shader : glslShaders) {
    std::vector<std::string> colorInputs = {"base_color", "baseColor",
                                            "diffuse_color"};
    for (const auto& inputName : colorInputs) {
      if (auto input = shader->getInput(inputName)) {
        auto baseColor = parseColorOrDefault(input->getValue(), defaultColor);
        return glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
      }
    }
  }

  return glm::vec4(defaultColor[0], defaultColor[1], defaultColor[2], 1.0f);
}

glm::vec3 SlangMaterialXBridge::extractColorInput(
    const MaterialX::DocumentPtr& document, const std::string& inputName,
    const glm::vec3& defaultValue) const {
  const MaterialX::Color3 defaultColor(defaultValue[0], defaultValue[1],
                                       defaultValue[2]);

  if (!document) {
    return defaultValue;
  }

  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput(inputName)) {
      auto color = parseColorOrDefault(input->getValue(), defaultColor);
      return glm::vec3(color[0], color[1], color[2]);
    }
  }

  return defaultValue;
}

float SlangMaterialXBridge::extractFloatInput(
    const MaterialX::DocumentPtr& document, const std::string& inputName,
    float defaultValue) const {
  if (!document) {
    return defaultValue;
  }

  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput(inputName)) {
      if (auto value = input->getValue()) {
        if (value->isA<float>()) {
          return value->asA<float>();
        }

        if (!value->getValueString().empty()) {
          return MaterialX::fromValueString<float>(value->getValueString());
        }
      }
    }
  }

  return defaultValue;
}

std::string SlangMaterialXBridge::extractTextureFileForInput(
    const MaterialX::DocumentPtr& document,
    const std::string& inputName) const {
  if (!document) {
    return {};
  }

  auto surfaceShaders = document->getNodesOfType("standard_surface");
  for (const auto& shader : surfaceShaders) {
    if (auto input = shader->getInput(inputName)) {
      const auto fileName = resolveImageFileInput(input);
      if (!fileName.empty()) {
        return fileName;
      }
    }
  }

  return {};
}

bool SlangMaterialXBridge::isBinaryGltf(const std::string& path) {
  const auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos || dotPos + 1 >= path.size()) return false;

  std::string ext = path.substr(dotPos + 1);
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == "glb";
}

MaterialX::Color3 SlangMaterialXBridge::toColor(
    const std::vector<double>& factor, const MaterialX::Color3& fallback) {
  if (factor.size() < 3) {
    return fallback;
  }

  float r = static_cast<float>(factor[0]);
  float g = static_cast<float>(factor[1]);
  float b = static_cast<float>(factor[2]);
  return MaterialX::Color3(r, g, b);
}

std::string SlangMaterialXBridge::resolveTextureUri(
    const tinygltf::Model& model, int textureIndex) {
  if (textureIndex < 0 ||
      textureIndex >= static_cast<int>(model.textures.size())) {
    return {};
  }

  const auto& texture = model.textures[textureIndex];
  if (texture.source >= 0 &&
      texture.source < static_cast<int>(model.images.size())) {
    const auto& image = model.images[texture.source];
    if (!image.uri.empty()) {
      return image.uri;
    }
    if (!image.name.empty()) {
      return image.name;
    }
    return "texture_" + std::to_string(texture.source);
  }

  return {};
}

MaterialX::DocumentPtr SlangMaterialXBridge::createDocumentFromGltfMaterial(
    const tinygltf::Model& model, const tinygltf::Material& gltfMaterial,
    const std::string& materialName) const {
  auto document = MaterialX::createDocument();
  const auto& pbr = gltfMaterial.pbrMetallicRoughness;
  const auto* specGlossExtension =
      findMaterialExtension(gltfMaterial, kSpecularGlossinessExtension);
  const bool useSpecularGlossiness = specGlossExtension != nullptr;
  const glm::vec4 specGlossDiffuseFactor = readExtensionVec4(
      specGlossExtension, "diffuseFactor", glm::vec4(1.0f));
  const glm::vec3 specGlossSpecularFactor = readExtensionVec3(
      specGlossExtension, "specularFactor", glm::vec3(1.0f));
  const float specGlossGlossinessFactor = readExtensionNumber(
      specGlossExtension, "glossinessFactor", 1.0f);
  const auto specGlossApproximation =
      convertSpecularGlossinessToMetallicRoughness(
          specGlossDiffuseFactor, specGlossSpecularFactor,
          specGlossGlossinessFactor);
  const MaterialX::Color3 defaultColor(1.0f, 1.0f, 1.0f);
  const MaterialX::Color3 blackColor(0.0f, 0.0f, 0.0f);

  std::string shaderName =
      materialName.empty() ? "gltf_shader" : materialName + "_shader";
  auto shader =
      document->addNode("standard_surface", shaderName, "surfaceshader");

  auto baseInput = shader->addInput("base", "float");
  baseInput->setValue(1.0f);

  auto baseColorInput = shader->addInput("base_color", "color3");
  if (useSpecularGlossiness) {
    baseColorInput->setValue(MaterialX::Color3(
        specGlossApproximation.baseColor.r, specGlossApproximation.baseColor.g,
        specGlossApproximation.baseColor.b));
  } else {
    baseColorInput->setValue(toColor(pbr.baseColorFactor, defaultColor));
  }

  auto opacityInput = shader->addInput("opacity", "float");
  const float opacityFactor =
      useSpecularGlossiness
          ? specGlossDiffuseFactor.a
          : (pbr.baseColorFactor.size() >= 4
                 ? static_cast<float>(pbr.baseColorFactor[3])
                 : 1.0f);
  opacityInput->setValue(opacityFactor);

  const auto baseColorTextureIndex =
      useSpecularGlossiness
          ? readTextureIndexObject(specGlossExtension->Get("diffuseTexture"))
          : pbr.baseColorTexture.index;
  if (baseColorTextureIndex >= 0) {
    auto imageNode = document->addNode(
        "image",
        (materialName.empty() ? "baseColor_tex"
                              : materialName + "_baseColor_tex"),
        "color3");
    auto fileInput = imageNode->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, baseColorTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }
    baseColorInput->setConnectedNode(imageNode);
  }

  auto metallicInput = shader->addInput("metalness", "float");
  metallicInput->setValue(useSpecularGlossiness
                              ? specGlossApproximation.metallic
                              : static_cast<float>(pbr.metallicFactor));

  auto roughnessInput = shader->addInput("specular_roughness", "float");
  roughnessInput->setValue(useSpecularGlossiness
                               ? specGlossApproximation.roughness
                               : static_cast<float>(pbr.roughnessFactor));

  auto specularInput = shader->addInput("specular", "float");
  specularInput->setValue(
      useSpecularGlossiness
          ? perceivedBrightness(specGlossSpecularFactor)
          : readExtensionNumber(
                findMaterialExtension(gltfMaterial, "KHR_materials_specular"),
                "specularFactor", 1.0f));

  auto transmissionInput = shader->addInput("transmission", "float");
  transmissionInput->setValue(readExtensionNumber(
      findMaterialExtension(gltfMaterial, "KHR_materials_transmission"),
      "transmissionFactor", 0.0f));

  const auto metallicRoughnessTextureIndex =
      useSpecularGlossiness ? -1 : pbr.metallicRoughnessTexture.index;
  if (metallicRoughnessTextureIndex >= 0) {
    auto imageNode = document->addNode(
        "image",
        (materialName.empty() ? "metalRough_tex"
                              : materialName + "_metalRough_tex"),
        "color3");
    auto fileInput = imageNode->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, metallicRoughnessTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto roughnessExtract = document->addNode(
        "extract",
        (materialName.empty() ? "roughness_extract"
                              : materialName + "_roughness_extract"),
        "float");
    roughnessExtract->addInput("in", "color3")->setConnectedNode(imageNode);
    roughnessExtract->addInput("index", "integer")->setValue(1);
    roughnessInput->setConnectedNode(roughnessExtract);

    auto metallicExtract = document->addNode(
        "extract",
        (materialName.empty() ? "metallic_extract"
                              : materialName + "_metallic_extract"),
        "float");
    metallicExtract->addInput("in", "color3")->setConnectedNode(imageNode);
    metallicExtract->addInput("index", "integer")->setValue(2);
    metallicInput->setConnectedNode(metallicExtract);
  }

  const auto normalTextureIndex = gltfMaterial.normalTexture.index;
  if (normalTextureIndex >= 0) {
    auto normalImage = document->addNode(
        "image",
        (materialName.empty() ? "normal_tex" : materialName + "_normal_tex"),
        "vector3");
    auto fileInput = normalImage->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, normalTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto normalMap = document->addNode(
        "normalmap",
        (materialName.empty() ? "normalmap" : materialName + "_normalmap"),
        "vector3");
    normalMap->addInput("in", "vector3")->setConnectedNode(normalImage);
    float normalScale = static_cast<float>(gltfMaterial.normalTexture.scale);
    normalMap->addInput("scale", "float")->setValue(normalScale);

    auto normalInput = shader->addInput("normal", "vector3");
    normalInput->setConnectedNode(normalMap);
  }

  const auto occlusionTextureIndex = gltfMaterial.occlusionTexture.index;
  if (occlusionTextureIndex >= 0) {
    auto occlusionImage = document->addNode(
        "image",
        (materialName.empty() ? "occlusion_tex"
                              : materialName + "_occlusion_tex"),
        "color3");
    auto fileInput = occlusionImage->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, occlusionTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto occlusionExtract = document->addNode(
        "extract",
        (materialName.empty() ? "occlusion_extract"
                              : materialName + "_occlusion_extract"),
        "float");
    occlusionExtract->addInput("in", "color3")
        ->setConnectedNode(occlusionImage);
    occlusionExtract->addInput("index", "integer")->setValue(0);

    auto occlusionInput = shader->addInput("occlusion", "float");
    float strength = static_cast<float>(gltfMaterial.occlusionTexture.strength);
    auto strengthMultiply = document->addNode(
        "multiply",
        (materialName.empty() ? "occlusion_strength"
                              : materialName + "_occlusion_strength"),
        "float");
    strengthMultiply->addInput("in1", "float")
        ->setConnectedNode(occlusionExtract);
    strengthMultiply->addInput("in2", "float")->setValue(strength);
    occlusionInput->setConnectedNode(strengthMultiply);
  }

  auto emissionColorInput = shader->addInput("emission_color", "color3");
  const auto emissiveColor = toColor(gltfMaterial.emissiveFactor, blackColor);
  emissionColorInput->setValue(emissiveColor);

  const auto emissiveTextureIndex = gltfMaterial.emissiveTexture.index;
  if (emissiveTextureIndex >= 0) {
    auto emissiveImage = document->addNode(
        "image",
        (materialName.empty() ? "emissive_tex"
                              : materialName + "_emissive_tex"),
        "color3");
    auto fileInput = emissiveImage->addInput("file", "filename");
    auto textureUri = resolveTextureUri(model, emissiveTextureIndex);
    if (!textureUri.empty()) {
      fileInput->setValueString(textureUri);
    }

    auto emissiveMultiply = document->addNode(
        "multiply",
        (materialName.empty() ? "emissive_mix"
                              : materialName + "_emissive_mix"),
        "color3");
    emissiveMultiply->addInput("in1", "color3")
        ->setConnectedNode(emissiveImage);
    emissiveMultiply->addInput("in2", "color3")->setValue(emissiveColor);
    emissionColorInput->setConnectedNode(emissiveMultiply);
  }

  auto shaderOutput = document->addOutput(
      materialName.empty() ? "out" : materialName + "_out", "surfaceshader");
  shaderOutput->setConnectedNode(shader);

  auto materialNode = document->addNode(
      "material", materialName.empty() ? "gltf_material" : materialName,
      "material");
  auto surfaceShaderInput =
      materialNode->addInput("surfaceshader", "surfaceshader");
  surfaceShaderInput->setConnectedNode(shader);

  return document;
}

std::vector<MaterialX::DocumentPtr> SlangMaterialXBridge::loadGltfMaterials(
    const std::string& filename) const {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err;
  std::string warn;

  bool loaded = false;
  if (isBinaryGltf(filename)) {
    loaded = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
  } else {
    loaded = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
  }

  if (!warn.empty()) {
    std::clog << "glTF warning: " << warn << std::endl;
  }
  if (!loaded) {
    throw std::runtime_error("Failed to load glTF file: " + err);
  }

  return loadGltfMaterials(model);
}

std::vector<MaterialX::DocumentPtr> SlangMaterialXBridge::loadGltfMaterials(
    const tinygltf::Model& model) const {
  std::vector<MaterialX::DocumentPtr> documents;
  documents.reserve(model.materials.size());

  for (size_t i = 0; i < model.materials.size(); ++i) {
    const auto& gltfMaterial = model.materials[i];
    std::string name = gltfMaterial.name.empty()
                           ? "material_" + std::to_string(i)
                           : gltfMaterial.name;
    documents.push_back(
        createDocumentFromGltfMaterial(model, gltfMaterial, name));
  }

  return documents;
}

std::vector<uint32_t> SlangMaterialXBridge::loadTexturesForGltf(
    const tinygltf::Model &model, const std::filesystem::path &baseDir,
    container::material::TextureManager &textureManager,
    const std::function<container::material::TextureResource(
        const std::string &, bool)> &textureLoader,
    const std::function<container::material::TextureResource(
        const std::string &, std::span<const std::byte>, uint32_t, uint32_t,
        bool)> &embeddedLoader) const {
  std::vector<uint32_t> textureToResource(
      model.textures.size(), std::numeric_limits<uint32_t>::max());

  // Classify each glTF image by how materials reference it. Color data
  // (base color, emissive) must be sRGB so sampling yields linear light;
  // data textures (normal, metallic-roughness, occlusion) must be UNORM or
  // the sRGB decode will corrupt their encoded values.
  std::vector<bool> imageIsSrgb(model.images.size(), true);
  std::vector<bool> imageClassified(model.images.size(), false);

  auto markImage = [&](int textureIndex, bool isSrgb) {
    if (textureIndex < 0 ||
        textureIndex >= static_cast<int>(model.textures.size())) {
      return;
    }
    const int source = model.textures[textureIndex].source;
    if (source < 0 || source >= static_cast<int>(model.images.size())) {
      return;
    }
    if (!imageClassified[source]) {
      imageIsSrgb[source] = isSrgb;
      imageClassified[source] = true;
    } else if (!isSrgb) {
      // If the same image is used as both color and data, prefer data/UNORM
      // because an sRGB normal map is visually broken while an "sRGB" albedo
      // sampled from a UNORM texture is only a minor tonal shift.
      imageIsSrgb[source] = false;
    }
  };

  for (const auto& mat : model.materials) {
    markImage(mat.pbrMetallicRoughness.baseColorTexture.index, true);
    markImage(mat.emissiveTexture.index, true);
    markImage(mat.normalTexture.index, false);
    markImage(mat.occlusionTexture.index, false);
    markImage(mat.pbrMetallicRoughness.metallicRoughnessTexture.index, false);
    markImage(readExtensionTextureIndex(
                  mat, kSpecularGlossinessExtension, "diffuseTexture"),
              true);
    markImage(readExtensionTextureIndex(
                  mat, kSpecularGlossinessExtension,
                  "specularGlossinessTexture"),
              true);

    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_specular", "specularTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_specular", "specularColorTexture"),
              true);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_transmission", "transmissionTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_volume", "thicknessTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_clearcoat", "clearcoatTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_clearcoat",
                  "clearcoatRoughnessTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_clearcoat", "clearcoatNormalTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_sheen", "sheenColorTexture"),
              true);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_sheen", "sheenRoughnessTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_iridescence", "iridescenceTexture"),
              false);
    markImage(readExtensionTextureIndex(
                  mat, "KHR_materials_iridescence",
                  "iridescenceThicknessTexture"),
              false);

    static constexpr std::array kLegacyPbrTextureNames = {
        "roughnessTexture", "metalnessTexture", "specularTexture",
        "heightTexture", "displacementTexture", "opacityTexture",
        "refractionTexture", "transmissionTexture"};
    for (const auto* textureName : kLegacyPbrTextureNames) {
      markImage(readLegacyTextureIndex(mat.values, textureName), false);
      markImage(readLegacyTextureIndex(mat.additionalValues, textureName),
                false);
    }
  }

  for (size_t i = 0; i < model.textures.size(); ++i) {
    const auto& texture = model.textures[i];
    if (texture.source < 0 ||
        texture.source >= static_cast<int>(model.images.size())) {
      continue;
    }

    const size_t imageIndex = static_cast<size_t>(texture.source);
    const auto& image = model.images[imageIndex];
    const bool embedded = image.uri.empty() || image.uri.starts_with("data:") ||
                          image.bufferView >= 0;
    if (embedded && !embeddedLoader)
      continue;

    const auto fullPath = container::util::pathToUtf8(
        (baseDir / container::util::pathFromUtf8(
                       embedded ? "embedded-image-" + std::to_string(imageIndex)
                                : image.uri))
            .lexically_normal());
    const uint32_t samplerIndex = gltfTextureSamplerIndex(model, texture);
    const std::string textureCacheKey = [&] {
      std::string key = (embedded ? "gltf-embedded:" : "file:") + fullPath;
      if (embedded) {
        // Multiple glTF models may share a directory and image index.
        // Identify decoded content too, so their embedded images cannot alias.
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char byte : image.image)
          hash = (hash ^ byte) * 1099511628211ull;
        key += "|pixels=" + std::to_string(hash) +
               "|size=" + std::to_string(image.width) + "x" +
               std::to_string(image.height) +
               "|format=" + std::to_string(image.bits) + ":" +
               std::to_string(image.component);
      }
      return key + "|sampler=" + std::to_string(samplerIndex) +
             "|srgb=" + std::to_string(imageIsSrgb[imageIndex]);
    }();

    if (const auto cachedIndex =
            textureManager.findTextureIndex(textureCacheKey)) {
      textureToResource[i] = *cachedIndex;
      continue;
    }

    try {
      container::material::TextureResource resource;
      if (embedded) {
        if (image.width <= 0 || image.height <= 0 || image.bits != 8 ||
            image.component < 1 || image.component > 4)
          throw std::runtime_error("unsupported decoded glTF image format");
        const uint64_t pixels = uint64_t(image.width) * uint64_t(image.height);
        if (pixels > std::numeric_limits<size_t>::max() / 4 ||
            pixels * uint64_t(image.component) > image.image.size())
          throw std::runtime_error("truncated decoded glTF image");
        std::vector<std::byte> rgba(size_t(pixels) * 4);
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
          const auto *source = image.image.data() + pixel * image.component;
          const bool grey = image.component <= 2;
          rgba[pixel * 4] = std::byte(source[0]);
          rgba[pixel * 4 + 1] = std::byte(grey ? source[0] : source[1]);
          rgba[pixel * 4 + 2] = std::byte(grey ? source[0] : source[2]);
          rgba[pixel * 4 + 3] = std::byte(image.component == 2   ? source[1]
                                          : image.component == 4 ? source[3]
                                                                 : 255);
        }
        resource =
            embeddedLoader(fullPath, rgba, uint32_t(image.width),
                           uint32_t(image.height), imageIsSrgb[imageIndex]);
      } else {
        resource = textureLoader(fullPath, imageIsSrgb[imageIndex]);
      }
      resource.name = textureCacheKey;
      resource.samplerIndex = samplerIndex;
      textureToResource[i] = textureManager.registerTexture(resource);
    } catch (const std::exception &exc) {
      std::println(stderr, "Texture load failed for {}: {}", fullPath, exc.what());
    }
  }

  return textureToResource;
}

void SlangMaterialXBridge::loadMaterialsForGltf(
    const tinygltf::Model& model, const std::vector<uint32_t>& textureToResource,
    container::material::MaterialManager& materialManager,
    uint32_t& defaultMaterialIndex) const {
  auto materialDocs = loadGltfMaterials(model);

  const auto resolveTextureIndexFromUri =
      [&](const std::string& uri) -> uint32_t {
    if (uri.empty()) {
      return std::numeric_limits<uint32_t>::max();
    }

    for (size_t i = 0; i < model.textures.size(); ++i) {
      const auto& texture = model.textures[i];
      if (texture.source < 0 ||
          texture.source >= static_cast<int>(model.images.size())) {
        continue;
      }

      const auto& image = model.images[static_cast<size_t>(texture.source)];
      const auto fallbackName = "texture_" + std::to_string(i);
      if (uri == image.uri || uri == image.name || uri == fallbackName) {
        if (i < textureToResource.size()) {
          return textureToResource[i];
        }
      }
    }

    return std::numeric_limits<uint32_t>::max();
  };

  const auto existingMaterials = materialManager.materialCount();
  for (size_t i = 0; i < model.materials.size(); ++i) {
    const auto& mat = model.materials[i];
    const auto* specGlossExtension =
        findMaterialExtension(mat, kSpecularGlossinessExtension);
    const bool hasAuthoredMetallicFactor = hasAuthoredPbrFactor(
        mat, "metallicFactor", mat.pbrMetallicRoughness.metallicFactor, 1.0);
    container::material::Material material{};
    if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
      material.baseColor = glm::vec4(
          static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[0]),
          static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[1]),
          static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[2]),
          static_cast<float>(mat.pbrMetallicRoughness.baseColorFactor[3]));
    }

    material.metallicFactor =
        static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
    material.roughnessFactor =
        static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
    material.alphaCutoff = static_cast<float>(mat.alphaCutoff);
    material.normalTextureScale =
        static_cast<float>(mat.normalTexture.scale);
    material.occlusionStrength =
        static_cast<float>(mat.occlusionTexture.strength);
    material.baseColorTextureTransform =
        readTextureTransform(mat.pbrMetallicRoughness.baseColorTexture);
    material.normalTextureTransform = readTextureTransform(mat.normalTexture);
    material.occlusionTextureTransform =
        readTextureTransform(mat.occlusionTexture);
    material.emissiveTextureTransform =
        readTextureTransform(mat.emissiveTexture);
    material.metallicRoughnessTextureTransform =
        readTextureTransform(
            mat.pbrMetallicRoughness.metallicRoughnessTexture);
    material.roughnessTextureTransform =
        material.metallicRoughnessTextureTransform;
    material.metalnessTextureTransform =
        material.metallicRoughnessTextureTransform;
    material.specularFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_specular"),
        "specularFactor", material.specularFactor);
    material.specularColorFactor = readExtensionVec3(
        findMaterialExtension(mat, "KHR_materials_specular"),
        "specularColorFactor", material.specularColorFactor);
    material.ior = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_ior"),
        "ior", material.ior);
    material.dispersion = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_dispersion"),
        "dispersion", material.dispersion);
    material.emissiveStrength = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_emissive_strength"),
        "emissiveStrength", material.emissiveStrength);
    material.transmissionFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_transmission"),
        "transmissionFactor", material.transmissionFactor);
    material.clearcoatFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_clearcoat"),
        "clearcoatFactor", material.clearcoatFactor);
    material.clearcoatRoughnessFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_clearcoat"),
        "clearcoatRoughnessFactor", material.clearcoatRoughnessFactor);
    if (const auto* clearcoatExtension =
            findMaterialExtension(mat, "KHR_materials_clearcoat");
        clearcoatExtension != nullptr &&
        clearcoatExtension->Has("clearcoatNormalTexture")) {
      material.clearcoatNormalTextureScale = readTextureScaleObject(
          clearcoatExtension->Get("clearcoatNormalTexture"),
          material.clearcoatNormalTextureScale);
    }
    material.thicknessFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_volume"),
        "thicknessFactor", material.thicknessFactor);
    material.attenuationColor = readExtensionVec3(
        findMaterialExtension(mat, "KHR_materials_volume"),
        "attenuationColor", material.attenuationColor);
    material.attenuationDistance = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_volume"),
        "attenuationDistance", material.attenuationDistance);
    material.sheenColorFactor = readExtensionVec3(
        findMaterialExtension(mat, "KHR_materials_sheen"),
        "sheenColorFactor", material.sheenColorFactor);
    material.sheenRoughnessFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_sheen"),
        "sheenRoughnessFactor", material.sheenRoughnessFactor);
    material.iridescenceFactor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_iridescence"),
        "iridescenceFactor", material.iridescenceFactor);
    material.iridescenceIor = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_iridescence"),
        "iridescenceIor", material.iridescenceIor);
    material.iridescenceThicknessMinimum = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_iridescence"),
        "iridescenceThicknessMinimum", material.iridescenceThicknessMinimum);
    material.iridescenceThicknessMaximum = readExtensionNumber(
        findMaterialExtension(mat, "KHR_materials_iridescence"),
        "iridescenceThicknessMaximum", material.iridescenceThicknessMaximum);
    material.unlit = findMaterialExtension(mat, "KHR_materials_unlit") != nullptr;
    material.opacityFactor =
        readLegacyFactor(mat.values, "opacity", material.opacityFactor);
    material.opacityFactor = readLegacyFactor(
        mat.additionalValues, "opacity", material.opacityFactor);
    material.specularFactor =
        readLegacyFactor(mat.values, "specular", material.specularFactor);
    material.specularFactor = readLegacyFactor(
        mat.additionalValues, "specular", material.specularFactor);
    material.heightScale =
        readLegacyFactor(mat.values, "heightScale", material.heightScale);
    material.heightScale = readLegacyFactor(
        mat.additionalValues, "heightScale", material.heightScale);
    material.transmissionFactor =
        readLegacyFactor(mat.values, "refraction", material.transmissionFactor);
    material.transmissionFactor = readLegacyFactor(
        mat.additionalValues, "refraction", material.transmissionFactor);
    material.doubleSided = mat.doubleSided;
    if (mat.alphaMode == "MASK") {
      material.alphaMode = AlphaMode::Mask;
    } else if (mat.alphaMode == "BLEND") {
      material.alphaMode = AlphaMode::Blend;
    } else {
      material.alphaMode = AlphaMode::Opaque;
    }
    if (mat.emissiveFactor.size() == 3) {
      material.emissiveColor =
          glm::vec3(static_cast<float>(mat.emissiveFactor[0]),
                    static_cast<float>(mat.emissiveFactor[1]),
                    static_cast<float>(mat.emissiveFactor[2]));
    }

    MaterialX::DocumentPtr materialDoc =
        i < materialDocs.size() ? materialDocs[i] : nullptr;

    if (materialDoc) {
      // NOTE: We intentionally do NOT overwrite material.baseColor from the
      // MaterialX document here. The glTF factor (already set above from
      // pbrMetallicRoughness.baseColorFactor) is the authoritative source for
      // the per-material tint. extractBaseColor() reads getValue() on the
      // MaterialX input, which returns nullptr once the input is connected to
      // an image node (as done in createDocumentFromGltfMaterial). In that case
      // parseColorOrDefault falls back to Color3() = (0,0,0), which would
      // stomp the real factor and produce pitch-black albedo in the G-buffer
      // even though textures are sampled correctly.
      material.emissiveColor = extractColorInput(materialDoc, "emission_color",
                                                 material.emissiveColor);
      material.metallicFactor =
          extractFloatInput(materialDoc, "metalness", material.metallicFactor);
      material.roughnessFactor = extractFloatInput(
          materialDoc, "specular_roughness", material.roughnessFactor);
      material.specularFactor =
          extractFloatInput(materialDoc, "specular", material.specularFactor);
      material.heightScale =
          extractFloatInput(materialDoc, "height", material.heightScale);
      material.transmissionFactor = extractFloatInput(
          materialDoc, "transmission", material.transmissionFactor);
      material.transmissionFactor = extractFloatInput(
          materialDoc, "refraction", material.transmissionFactor);

      auto applyTextureFromDoc = [&](const std::string& inputName,
                                     uint32_t& destination) {
        auto uri = extractTextureFileForInput(materialDoc, inputName);
        auto resolved = resolveTextureIndexFromUri(uri);
        if (resolved != std::numeric_limits<uint32_t>::max()) {
          destination = resolved;
        }
      };

      applyTextureFromDoc("base_color", material.baseColorTextureIndex);
      applyTextureFromDoc("normal", material.normalTextureIndex);
      applyTextureFromDoc("occlusion", material.occlusionTextureIndex);
      applyTextureFromDoc("emission_color", material.emissiveTextureIndex);
      applyTextureFromDoc("opacity", material.opacityTextureIndex);
      applyTextureFromDoc("specular", material.specularTextureIndex);
      applyTextureFromDoc("height", material.heightTextureIndex);
      applyTextureFromDoc("displacement", material.heightTextureIndex);
      applyTextureFromDoc("transmission", material.transmissionTextureIndex);
      applyTextureFromDoc("refraction", material.transmissionTextureIndex);

      auto metallicUri = extractTextureFileForInput(materialDoc, "metalness");
      auto roughnessUri =
          extractTextureFileForInput(materialDoc, "specular_roughness");
      auto metallicResolved = resolveTextureIndexFromUri(metallicUri);
      auto roughnessResolved = resolveTextureIndexFromUri(roughnessUri);
      const uint32_t invalidTexture = std::numeric_limits<uint32_t>::max();
      if (metallicResolved != invalidTexture &&
          roughnessResolved != invalidTexture &&
          metallicResolved == roughnessResolved) {
        material.metallicRoughnessTextureIndex = metallicResolved;
      } else {
        if (metallicResolved != invalidTexture) {
          material.metalnessTextureIndex = metallicResolved;
        }
        if (roughnessResolved != invalidTexture) {
          material.roughnessTextureIndex = roughnessResolved;
        }
      }
    }

    auto assignGltfTexture = [&](uint32_t& destination, int textureIndex) {
      const uint32_t resolved =
          resolveTextureIndex(model, textureToResource, textureIndex);
      if (resolved != std::numeric_limits<uint32_t>::max()) {
        destination = resolved;
      }
    };

    assignGltfTexture(material.baseColorTextureIndex,
                      mat.pbrMetallicRoughness.baseColorTexture.index);
    assignGltfTexture(
        material.metallicRoughnessTextureIndex,
        mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
    assignGltfTexture(material.normalTextureIndex, mat.normalTexture.index);
    assignGltfTexture(material.occlusionTextureIndex,
                      mat.occlusionTexture.index);
    assignGltfTexture(material.emissiveTextureIndex, mat.emissiveTexture.index);

    if (specGlossExtension != nullptr) {
      const glm::vec4 diffuseFactor = readExtensionVec4(
          specGlossExtension, "diffuseFactor", glm::vec4(1.0f));
      const glm::vec3 specularFactor = readExtensionVec3(
          specGlossExtension, "specularFactor", glm::vec3(1.0f));
      const float glossinessFactor = readExtensionNumber(
          specGlossExtension, "glossinessFactor", 1.0f);
      const auto approximation =
          convertSpecularGlossinessToMetallicRoughness(
              diffuseFactor, specularFactor, glossinessFactor);

      material.specularGlossinessWorkflow = true;
      material.baseColor = approximation.baseColor;
      material.metallicFactor = approximation.metallic;
      material.roughnessFactor =
          std::clamp(glossinessFactor, 0.0f, 1.0f);
      const float maxSpecular = maxComponent(specularFactor);
      material.specularFactor =
          std::clamp(maxSpecular / 0.04f, 0.0f, 1.0f);
      material.specularColorFactor =
          maxSpecular > 1e-6f ? specularFactor / maxSpecular : glm::vec3(1.0f);
      material.metallicRoughnessTextureIndex =
          std::numeric_limits<uint32_t>::max();
      material.metalnessTextureIndex = std::numeric_limits<uint32_t>::max();
      material.roughnessTextureIndex = std::numeric_limits<uint32_t>::max();
      material.specularTextureIndex = std::numeric_limits<uint32_t>::max();

      material.baseColorTextureIndex = resolveTextureIndex(
          model, textureToResource,
          readTextureIndexObject(specGlossExtension->Get("diffuseTexture")));
      if (specGlossExtension->Has("diffuseTexture")) {
        material.baseColorTextureTransform =
            readTextureTransformObject(specGlossExtension->Get("diffuseTexture"));
      }
      const uint32_t specularGlossinessTexture = resolveTextureIndex(
          model, textureToResource,
          readTextureIndexObject(
              specGlossExtension->Get("specularGlossinessTexture")));
      if (specularGlossinessTexture != std::numeric_limits<uint32_t>::max()) {
        material.roughnessTextureIndex = specularGlossinessTexture;
        material.specularColorTextureIndex = specularGlossinessTexture;
        if (specGlossExtension->Has("specularGlossinessTexture")) {
          const auto transform = readTextureTransformObject(
              specGlossExtension->Get("specularGlossinessTexture"));
          material.roughnessTextureTransform = transform;
          material.specularColorTextureTransform = transform;
        }
      }
    }

    auto assignOptionalTexture = [&](uint32_t& destination, int textureIndex) {
      if (destination != std::numeric_limits<uint32_t>::max()) {
        return;
      }
      const uint32_t resolved =
          resolveTextureIndex(model, textureToResource, textureIndex);
      if (resolved != std::numeric_limits<uint32_t>::max()) {
        destination = resolved;
      }
    };

    auto assignOptionalExtensionTexture =
        [&](uint32_t& destination,
            container::material::TextureTransform& destinationTransform,
            const char* extensionName, const char* textureName) {
          if (destination != std::numeric_limits<uint32_t>::max()) {
            return;
          }

          const tinygltf::Value* textureObject =
              findExtensionTextureObject(mat, extensionName, textureName);
          if (textureObject == nullptr) {
            return;
          }

          const uint32_t resolved =
              resolveTextureIndex(model, textureToResource,
                                  readTextureIndexObject(*textureObject));
          if (resolved != std::numeric_limits<uint32_t>::max()) {
            destination = resolved;
            destinationTransform = readTextureTransformObject(*textureObject);
          }
        };

    assignOptionalExtensionTexture(
        material.specularTextureIndex, material.specularTextureTransform,
        "KHR_materials_specular", "specularTexture");
    assignOptionalExtensionTexture(
        material.specularColorTextureIndex,
        material.specularColorTextureTransform, "KHR_materials_specular",
        "specularColorTexture");
    assignOptionalExtensionTexture(
        material.transmissionTextureIndex, material.transmissionTextureTransform,
        "KHR_materials_transmission", "transmissionTexture");
    assignOptionalExtensionTexture(
        material.clearcoatTextureIndex, material.clearcoatTextureTransform,
        "KHR_materials_clearcoat", "clearcoatTexture");
    assignOptionalExtensionTexture(
        material.clearcoatRoughnessTextureIndex,
        material.clearcoatRoughnessTextureTransform, "KHR_materials_clearcoat",
        "clearcoatRoughnessTexture");
    assignOptionalExtensionTexture(
        material.clearcoatNormalTextureIndex,
        material.clearcoatNormalTextureTransform, "KHR_materials_clearcoat",
        "clearcoatNormalTexture");
    assignOptionalExtensionTexture(
        material.thicknessTextureIndex, material.thicknessTextureTransform,
        "KHR_materials_volume", "thicknessTexture");
    assignOptionalExtensionTexture(
        material.sheenColorTextureIndex, material.sheenColorTextureTransform,
        "KHR_materials_sheen", "sheenColorTexture");
    assignOptionalExtensionTexture(
        material.sheenRoughnessTextureIndex,
        material.sheenRoughnessTextureTransform, "KHR_materials_sheen",
        "sheenRoughnessTexture");
    assignOptionalExtensionTexture(
        material.iridescenceTextureIndex, material.iridescenceTextureTransform,
        "KHR_materials_iridescence", "iridescenceTexture");
    assignOptionalExtensionTexture(
        material.iridescenceThicknessTextureIndex,
        material.iridescenceThicknessTextureTransform,
        "KHR_materials_iridescence", "iridescenceThicknessTexture");

    assignOptionalTexture(
        material.roughnessTextureIndex,
        readLegacyTextureIndex(mat.values, "roughnessTexture"));
    assignOptionalTexture(
        material.roughnessTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "roughnessTexture"));
    assignOptionalTexture(
        material.metalnessTextureIndex,
        readLegacyTextureIndex(mat.values, "metalnessTexture"));
    assignOptionalTexture(
        material.metalnessTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "metalnessTexture"));
    assignOptionalTexture(
        material.specularTextureIndex,
        readLegacyTextureIndex(mat.values, "specularTexture"));
    assignOptionalTexture(
        material.specularTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "specularTexture"));
    assignOptionalTexture(
        material.heightTextureIndex,
        readLegacyTextureIndex(mat.values, "heightTexture"));
    assignOptionalTexture(
        material.heightTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "heightTexture"));
    assignOptionalTexture(
        material.heightTextureIndex,
        readLegacyTextureIndex(mat.values, "displacementTexture"));
    assignOptionalTexture(
        material.heightTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "displacementTexture"));
    assignOptionalTexture(
        material.opacityTextureIndex,
        readLegacyTextureIndex(mat.values, "opacityTexture"));
    assignOptionalTexture(
        material.opacityTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "opacityTexture"));
    assignOptionalTexture(
        material.transmissionTextureIndex,
        readLegacyTextureIndex(mat.values, "refractionTexture"));
    assignOptionalTexture(
        material.transmissionTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "refractionTexture"));
    assignOptionalTexture(
        material.transmissionTextureIndex,
        readLegacyTextureIndex(mat.values, "transmissionTexture"));
    assignOptionalTexture(
        material.transmissionTextureIndex,
        readLegacyTextureIndex(mat.additionalValues, "transmissionTexture"));

    if (specGlossExtension == nullptr && !hasAuthoredMetallicFactor &&
        material.metallicRoughnessTextureIndex ==
            std::numeric_limits<uint32_t>::max() &&
        material.metalnessTextureIndex == std::numeric_limits<uint32_t>::max()) {
      material.metallicFactor = 0.0f;
    }

    if (material.alphaMode == AlphaMode::Opaque &&
        (material.opacityFactor < 0.999f ||
         material.opacityTextureIndex != std::numeric_limits<uint32_t>::max() ||
         material.transmissionFactor > 0.001f ||
         material.transmissionTextureIndex != std::numeric_limits<uint32_t>::max())) {
      material.alphaMode = AlphaMode::Blend;
    }

    materialManager.createMaterial(material);
  }

  if (!model.materials.empty()) {
    defaultMaterialIndex = static_cast<uint32_t>(existingMaterials);
  }
}

}  // namespace container::materialx
