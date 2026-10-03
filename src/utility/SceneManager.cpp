#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/Mesh.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/VulkanDevice.h"

#include <glm/gtc/quaternion.hpp>

namespace container::scene {

namespace {

glm::mat4 nodeLocalTransform(const tinygltf::Node& node) {
  if (node.matrix.size() == 16) {
    glm::mat4 transform(1.0f);
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        transform[column][row] =
            static_cast<float>(node.matrix[column * 4 + row]);
      }
    }
    return transform;
  }

  glm::mat4 transform(1.0f);

  if (node.translation.size() == 3) {
    transform = glm::translate(
        transform,
        glm::vec3(static_cast<float>(node.translation[0]),
                  static_cast<float>(node.translation[1]),
                  static_cast<float>(node.translation[2])));
  }

  if (node.rotation.size() == 4) {
    const glm::quat rotation(
        static_cast<float>(node.rotation[3]),
        static_cast<float>(node.rotation[0]),
        static_cast<float>(node.rotation[1]),
        static_cast<float>(node.rotation[2]));
    transform *= glm::mat4_cast(rotation);
  }

  if (node.scale.size() == 3) {
    transform = glm::scale(
        transform,
        glm::vec3(static_cast<float>(node.scale[0]),
                  static_cast<float>(node.scale[1]),
                  static_cast<float>(node.scale[2])));
  }

  return transform;
}

bool isDefaultSceneRequest(std::string_view path) {
  return path == container::app::kDefaultSceneModelToken;
}

bool isProceduralSphereEntry(std::string_view entry) {
  return entry == "__procedural_uv_sphere__";
}

bool hasMaterialTexture(uint32_t textureIndex) {
  return textureIndex != std::numeric_limits<uint32_t>::max();
}

void writeHostVisibleBuffer(container::gpu::AllocationManager& allocationManager,
                            const container::gpu::AllocatedBuffer& buffer,
                            const void* data,
                            size_t size) {
  if (buffer.buffer == VK_NULL_HANDLE || buffer.allocation == nullptr ||
      data == nullptr || size == 0u) {
    return;
  }

  void* mapped = buffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map scene uniform buffer");
    }
    mappedHere = true;
  }

  std::memcpy(mapped, data, size);
  if (vmaFlushAllocation(allocationManager.memoryManager()->allocator(),
                         buffer.allocation, 0,
                         static_cast<VkDeviceSize>(size)) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation);
    }
    throw std::runtime_error("failed to flush scene uniform buffer");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                   buffer.allocation);
  }
}

bool isForwardTransparentMaterial(
    const container::material::Material& material) {
  return material.alphaMode == container::material::AlphaMode::Blend ||
         material.transmissionFactor > 0.001f ||
         hasMaterialTexture(material.transmissionTextureIndex);
}

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

glm::mat4 importScaleTransform(float scale) {
  return glm::scale(glm::mat4(1.0f),
                    glm::vec3(sanitizeImportScale(scale)));
}

std::vector<int> activeSceneRootNodes(const tinygltf::Model& model) {
  if (!model.scenes.empty()) {
    int sceneIndex = model.defaultScene;
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(model.scenes.size())) {
      sceneIndex = 0;
    }
    return model.scenes[sceneIndex].nodes;
  }

  std::vector<bool> hasParent(model.nodes.size(), false);
  for (const auto& node : model.nodes) {
    for (int childIndex : node.children) {
      if (childIndex >= 0 &&
          childIndex < static_cast<int>(hasParent.size())) {
        hasParent[childIndex] = true;
      }
    }
  }

  std::vector<int> rootNodes;
  rootNodes.reserve(model.nodes.size());
  for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex) {
    if (!hasParent[nodeIndex]) {
      rootNodes.push_back(static_cast<int>(nodeIndex));
    }
  }
  return rootNodes;
}

glm::vec3 pointLightColorOrDefault(const tinygltf::Light& light) {
  glm::vec3 color(1.0f);
  if (light.color.size() >= 3) {
    color = glm::vec3(static_cast<float>(light.color[0]),
                      static_cast<float>(light.color[1]),
                      static_cast<float>(light.color[2]));
  }

  for (int component = 0; component < 3; ++component) {
    if (!std::isfinite(color[component]) || color[component] < 0.0f) {
      color[component] = 1.0f;
    }
  }
  return color;
}

float pointLightIntensityOrDefault(const tinygltf::Light& light) {
  if (!std::isfinite(light.intensity) || light.intensity < 0.0) {
    return 1.0f;
  }
  return static_cast<float>(light.intensity);
}

glm::vec3 normalizeOr(const glm::vec3& value, const glm::vec3& fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

glm::vec3 gltfDirectionalLightDirection(const glm::mat4& sceneLocalTransform) {
  return normalizeOr(
      glm::vec3(sceneLocalTransform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)),
      glm::vec3(0.0f, 0.0f, -1.0f));
}

float gltfPointLightRange(const tinygltf::Light& light, float importScale) {
  constexpr float kMinExplicitRange = 0.05f;
  if (std::isfinite(light.range) && light.range > 0.0) {
    return std::max(static_cast<float>(light.range) *
                        sanitizeImportScale(importScale),
                    kMinExplicitRange);
  }

  return container::gpu::kUnboundedPointLightRange;
}

std::pair<float, float> gltfSpotConeCosines(const tinygltf::Light& light) {
  constexpr float kMaxSpotConeAngle = 1.57079632679f;
  constexpr float kDefaultOuterConeAngle = 0.7853981634f;

  auto sanitizeAngle = [=](double value, float fallback) {
    if (!std::isfinite(value)) {
      return fallback;
    }
    return std::clamp(static_cast<float>(value), 0.0f, kMaxSpotConeAngle);
  };

  const float outerAngle =
      sanitizeAngle(light.spot.outerConeAngle, kDefaultOuterConeAngle);
  const float innerAngle =
      std::min(sanitizeAngle(light.spot.innerConeAngle, 0.0f), outerAngle);
  return {std::cos(innerAngle), std::cos(outerAngle)};
}

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

std::optional<float> areaLightTypeFromString(std::string_view typeName) {
  const std::string type = lowerAscii(std::string(typeName));
  if (type == "rect" || type == "rectangle" || type == "rectangular" ||
      type == "quad") {
    return container::gpu::kAreaLightTypeRectangle;
  }
  if (type == "disk" || type == "disc" || type == "circle") {
    return container::gpu::kAreaLightTypeDisk;
  }
  return std::nullopt;
}

std::optional<float> readAreaNumber(
    const tinygltf::Value* object,
    std::initializer_list<const char*> valueNames) {
  if (object == nullptr || !object->IsObject()) {
    return std::nullopt;
  }

  for (const char* valueName : valueNames) {
    if (!object->Has(valueName)) {
      continue;
    }

    const tinygltf::Value& value = object->Get(valueName);
    if (!value.IsNumber()) {
      continue;
    }

    const float result = static_cast<float>(value.GetNumberAsDouble());
    if (std::isfinite(result)) {
      return result;
    }
  }
  return std::nullopt;
}

std::optional<std::string> readAreaString(
    const tinygltf::Value* object,
    std::initializer_list<const char*> valueNames) {
  if (object == nullptr || !object->IsObject()) {
    return std::nullopt;
  }

  for (const char* valueName : valueNames) {
    if (!object->Has(valueName)) {
      continue;
    }

    const tinygltf::Value& value = object->Get(valueName);
    if (value.IsString()) {
      return value.Get<std::string>();
    }
  }
  return std::nullopt;
}

const tinygltf::Value* valueMember(const tinygltf::Value& object,
                                   const char* valueName) {
  if (!object.IsObject() || !object.Has(valueName)) {
    return nullptr;
  }

  return &object.Get(valueName);
}

const tinygltf::Value* objectMember(const tinygltf::Value& object,
                                    const char* valueName) {
  const tinygltf::Value* value = valueMember(object, valueName);
  if (value == nullptr) {
    return nullptr;
  }
  return value->IsObject() ? value : nullptr;
}

std::optional<float> areaLightTypeFromMetadata(
    const tinygltf::Value* metadata) {
  const std::optional<std::string> shape =
      readAreaString(metadata, {"shape", "type", "kind"});
  if (!shape) {
    return std::nullopt;
  }
  return areaLightTypeFromString(*shape);
}

bool metadataLooksLikeAreaLight(const tinygltf::Value* metadata) {
  if (metadata == nullptr || !metadata->IsObject()) {
    return false;
  }

  if (areaLightTypeFromMetadata(metadata)) {
    return true;
  }

  return readAreaNumber(metadata, {"width", "height", "radius", "diameter",
                                   "size", "sizeX", "sizeY", "size_x",
                                   "size_y"})
      .has_value();
}

static constexpr std::array<const char*, 4> kKnownAreaLightExtensions = {
    "KHR_lights_area",
    "EXT_lights_area",
    "WEBGI_lights_area",
    "WEBGI_area_light",
};

const tinygltf::Value* findAreaLightMetadata(const tinygltf::Light& light) {
  for (const char* extensionName : kKnownAreaLightExtensions) {
    const auto it = light.extensions.find(extensionName);
    if (it != light.extensions.end() && it->second.IsObject()) {
      return &it->second;
    }
  }

  for (const auto& [extensionName, extensionValue] : light.extensions) {
    (void)extensionName;
    if (metadataLooksLikeAreaLight(&extensionValue)) {
      return &extensionValue;
    }
  }

  if (const tinygltf::Value* nested = objectMember(light.extras, "areaLight")) {
    return nested;
  }
  if (metadataLooksLikeAreaLight(&light.extras)) {
    return &light.extras;
  }
  return nullptr;
}

float positiveOr(float value, float fallback) {
  return std::isfinite(value) && value > 0.0f ? value : fallback;
}

float readPositiveAreaNumber(const tinygltf::Value* metadata,
                             std::initializer_list<const char*> valueNames,
                             float fallback) {
  const std::optional<float> value = readAreaNumber(metadata, valueNames);
  return value ? positiveOr(*value, fallback) : fallback;
}

float gltfAreaLightRange(const tinygltf::Light& light,
                         const tinygltf::Value* metadata,
                         float importScale) {
  constexpr float kMinExplicitRange = 0.05f;
  const std::optional<float> metadataRange =
      readAreaNumber(metadata, {"range", "distance", "maxDistance"});
  if (metadataRange && *metadataRange > 0.0f) {
    return std::max(*metadataRange * sanitizeImportScale(importScale),
                    kMinExplicitRange);
  }
  return gltfPointLightRange(light, importScale);
}

struct GltfAreaLightSpec {
  float type{container::gpu::kAreaLightTypeRectangle};
  float halfWidth{0.5f};
  float halfHeight{0.5f};
  float range{container::gpu::kUnboundedPointLightRange};
};

std::optional<GltfAreaLightSpec> gltfAreaLightSpec(
    const tinygltf::Light& light,
    float importScale) {
  const tinygltf::Value* metadata = findAreaLightMetadata(light);
  std::optional<float> type = areaLightTypeFromString(light.type);
  if (!type) {
    type = areaLightTypeFromMetadata(metadata);
  }
  if (!type) {
    return std::nullopt;
  }

  constexpr float kDefaultAreaSize = 1.0f;
  constexpr float kMinAreaExtent = 0.001f;
  const float scale = sanitizeImportScale(importScale);

  GltfAreaLightSpec spec{};
  spec.type = *type;
  spec.range = gltfAreaLightRange(light, metadata, importScale);

  if (spec.type == container::gpu::kAreaLightTypeDisk) {
    const std::optional<float> radius =
        readAreaNumber(metadata, {"radius"});
    const float sourceRadius =
        radius ? positiveOr(*radius, kDefaultAreaSize * 0.5f)
               : readPositiveAreaNumber(metadata, {"diameter", "size", "width"},
                                        kDefaultAreaSize) *
                     0.5f;
    const float scaledRadius = std::max(sourceRadius * scale, kMinAreaExtent);
    spec.halfWidth = scaledRadius;
    spec.halfHeight = scaledRadius;
  } else {
    const float sourceWidth =
        readPositiveAreaNumber(metadata, {"width", "sizeX", "size_x", "size"},
                               kDefaultAreaSize);
    const float sourceHeight =
        readPositiveAreaNumber(metadata, {"height", "sizeY", "size_y"},
                               sourceWidth);
    spec.halfWidth = std::max(sourceWidth * scale * 0.5f, kMinAreaExtent);
    spec.halfHeight = std::max(sourceHeight * scale * 0.5f, kMinAreaExtent);
  }

  return spec;
}

glm::vec3 gltfAreaLightTangent(const glm::mat4& sceneLocalTransform) {
  return normalizeOr(
      glm::vec3(sceneLocalTransform * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)),
      glm::vec3(1.0f, 0.0f, 0.0f));
}

glm::vec3 gltfAreaLightBitangent(const glm::mat4& sceneLocalTransform) {
  return normalizeOr(
      glm::vec3(sceneLocalTransform * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f)),
      glm::vec3(0.0f, 1.0f, 0.0f));
}

std::optional<container::gpu::AreaLightData> makeGltfAreaLightData(
    const tinygltf::Light& lightDefinition,
    const glm::mat4& sceneLocalTransform,
    float importScale) {
  const std::optional<GltfAreaLightSpec> areaSpec =
      gltfAreaLightSpec(lightDefinition, importScale);
  if (!areaSpec) {
    return std::nullopt;
  }

  const glm::vec3 sceneLocalPosition =
      glm::vec3(sceneLocalTransform *
                glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

  container::gpu::AreaLightData areaLight{};
  areaLight.positionRange = glm::vec4(sceneLocalPosition, areaSpec->range);
  areaLight.colorIntensity =
      glm::vec4(pointLightColorOrDefault(lightDefinition),
                pointLightIntensityOrDefault(lightDefinition));
  areaLight.directionType =
      glm::vec4(gltfDirectionalLightDirection(sceneLocalTransform),
                areaSpec->type);
  areaLight.tangentHalfSize =
      glm::vec4(gltfAreaLightTangent(sceneLocalTransform),
                areaSpec->halfWidth);
  areaLight.bitangentHalfSize =
      glm::vec4(gltfAreaLightBitangent(sceneLocalTransform),
                areaSpec->halfHeight);
  return areaLight;
}

std::vector<double> readAreaColor(const tinygltf::Value& object) {
  if (!object.IsObject() || !object.Has("color")) {
    return {};
  }

  const tinygltf::Value& color = object.Get("color");
  if (!color.IsArray() || color.ArrayLen() < 3) {
    return {};
  }

  std::vector<double> result(3, 1.0);
  for (size_t i = 0; i < result.size(); ++i) {
    const tinygltf::Value& component = color.Get(i);
    if (!component.IsNumber()) {
      return {};
    }
    result[i] = component.GetNumberAsDouble();
  }
  return result;
}

tinygltf::Light areaLightDefinitionFromValue(const tinygltf::Value& value) {
  tinygltf::Light light{};
  light.type =
      readAreaString(&value, {"type", "shape", "kind"}).value_or("rectangle");
  light.color = readAreaColor(value);
  if (const std::optional<float> intensity =
          readAreaNumber(&value, {"intensity", "power"})) {
    light.intensity = *intensity;
  }
  if (const std::optional<float> range =
          readAreaNumber(&value, {"range", "distance", "maxDistance"})) {
    light.range = *range;
  }
  light.extras = value;
  return light;
}

std::optional<int> readAreaLightIndex(const tinygltf::Value& nodeExtension) {
  const std::optional<float> index = readAreaNumber(&nodeExtension, {"light"});
  if (!index || *index < 0.0f) {
    return std::nullopt;
  }
  return static_cast<int>(*index);
}

const tinygltf::Value* findModelAreaLightDefinition(
    const tinygltf::Model& model,
    const char* extensionName,
    int lightIndex) {
  if (lightIndex < 0) {
    return nullptr;
  }

  const auto extensionIt = model.extensions.find(extensionName);
  if (extensionIt == model.extensions.end() || !extensionIt->second.IsObject()) {
    return nullptr;
  }

  const tinygltf::Value* lights = valueMember(extensionIt->second, "lights");
  if (lights == nullptr || !lights->IsArray() ||
      lightIndex >= static_cast<int>(lights->ArrayLen())) {
    return nullptr;
  }

  const tinygltf::Value& lightDefinition =
      lights->Get(static_cast<size_t>(lightIndex));
  return lightDefinition.IsObject() ? &lightDefinition : nullptr;
}

glm::mat4 makeDefaultSceneTransform(const glm::vec3& translation, float uniformScale) {
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation);
  return glm::scale(transform, glm::vec3(uniformScale));
}

glm::mat4 composeNodeWorldTransform(const tinygltf::Model& model, int nodeIndex) {
  glm::mat4 worldTransform(1.0f);
  if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
    return worldTransform;
  }

  std::vector<int> parentByNode(model.nodes.size(), -1);
  for (size_t parentIndex = 0; parentIndex < model.nodes.size(); ++parentIndex) {
    for (int childIndex : model.nodes[parentIndex].children) {
      if (childIndex >= 0 && childIndex < static_cast<int>(model.nodes.size())) {
        parentByNode[childIndex] = static_cast<int>(parentIndex);
      }
    }
  }

  std::vector<int> chain;
  for (int current = nodeIndex; current >= 0; current = parentByNode[current]) {
    chain.push_back(current);
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    worldTransform *= nodeLocalTransform(model.nodes[*it]);
  }
  return worldTransform;
}

container::geometry::Mesh transformMesh(const container::geometry::Mesh& mesh,
                                        const glm::mat4& transform,
                                        int32_t materialIndex) {
  std::vector<container::geometry::Vertex> vertices = mesh.vertices();
  std::vector<uint32_t> indices = mesh.indices();
  const glm::mat3 modelMatrix = glm::mat3(transform);
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(modelMatrix));
  const bool windingFlipped = glm::determinant(modelMatrix) < 0.0f;

  for (auto& vertex : vertices) {
    vertex.position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));

    glm::vec3 worldNormal = normalMatrix * vertex.normal;
    if (glm::dot(worldNormal, worldNormal) > 1e-8f) {
      vertex.normal = glm::normalize(worldNormal);
    }

    glm::vec3 worldTangent = modelMatrix * glm::vec3(vertex.tangent);
    worldTangent -= vertex.normal * glm::dot(vertex.normal, worldTangent);
    if (glm::dot(worldTangent, worldTangent) > 1e-8f) {
      vertex.tangent =
          glm::vec4(glm::normalize(worldTangent),
                    windingFlipped ? -vertex.tangent.w : vertex.tangent.w);
    }
  }

  if (windingFlipped) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      std::swap(indices[i + 1], indices[i + 2]);
    }
  }

  return container::geometry::Mesh(std::move(vertices), std::move(indices),
                                   materialIndex, mesh.disableBackfaceCulling());
}

void appendModelMeshes(const container::geometry::Model& model,
                       const glm::mat4& transform,
                       int32_t materialIndex,
                       std::vector<container::geometry::Mesh>& mergedMeshes) {
  for (const auto& mesh : model.meshes()) {
    const int32_t resolvedMaterialIndex =
        materialIndex >= 0 ? materialIndex : mesh.materialIndex();
    mergedMeshes.push_back(transformMesh(mesh, transform, resolvedMaterialIndex));
  }
}

container::gpu::GpuTextureTransform makeGpuTextureTransform(
    const container::material::TextureTransform& transform) {
  const float cosRotation = std::cos(transform.rotation);
  const float sinRotation = std::sin(transform.rotation);

  container::gpu::GpuTextureTransform gpuTransform{};
  gpuTransform.row0 =
      glm::vec4(transform.scale.x * cosRotation,
                -transform.scale.y * sinRotation,
                transform.offset.x,
                static_cast<float>(transform.texCoord));
  gpuTransform.row1 =
      glm::vec4(transform.scale.x * sinRotation,
                transform.scale.y * cosRotation,
                transform.offset.y,
                static_cast<float>(transform.channel));
  return gpuTransform;
}

VkSamplerAddressMode materialSamplerAddressMode(uint32_t wrapMode) {
  switch (wrapMode) {
    case container::gpu::kMaterialSamplerWrapClampToEdge:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case container::gpu::kMaterialSamplerWrapMirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case container::gpu::kMaterialSamplerWrapRepeat:
    default:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

uint32_t sanitizeMaterialSamplerIndex(uint32_t samplerIndex) {
  return std::min(samplerIndex,
                  container::gpu::kMaterialSamplerDescriptorCapacity - 1u);
}

std::string textureCacheKey(std::string_view textureName,
                            uint32_t samplerIndex) {
  std::string key(textureName);
  key += "#sampler=";
  key += std::to_string(sanitizeMaterialSamplerIndex(samplerIndex));
  return key;
}

uint64_t hashBytes(std::span<const std::byte> bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (std::byte byte : bytes) {
    hash ^= static_cast<uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

container::gpu::GpuMaterial makeGpuMaterial(
    const container::material::Material& material) {
  container::gpu::GpuMaterial gpuMaterial{};
  gpuMaterial.color = material.baseColor;
  gpuMaterial.emissiveColor = material.emissiveColor;
  gpuMaterial.emissiveStrength = material.emissiveStrength;
  gpuMaterial.metallicRoughness =
      glm::vec2(material.metallicFactor, material.roughnessFactor);
  gpuMaterial.alphaCutoff = material.alphaCutoff;
  gpuMaterial.normalTextureScale = material.normalTextureScale;
  gpuMaterial.occlusionStrength = material.occlusionStrength;
  gpuMaterial.baseColorTextureIndex = material.baseColorTextureIndex;
  gpuMaterial.normalTextureIndex = material.normalTextureIndex;
  gpuMaterial.occlusionTextureIndex = material.occlusionTextureIndex;
  gpuMaterial.emissiveTextureIndex = material.emissiveTextureIndex;
  gpuMaterial.metallicRoughnessTextureIndex =
      material.metallicRoughnessTextureIndex;
  gpuMaterial.roughnessTextureIndex = material.roughnessTextureIndex;
  gpuMaterial.metalnessTextureIndex = material.metalnessTextureIndex;
  gpuMaterial.specularTextureIndex = material.specularTextureIndex;
  gpuMaterial.heightTextureIndex = material.heightTextureIndex;
  gpuMaterial.opacityTextureIndex = material.opacityTextureIndex;
  gpuMaterial.transmissionTextureIndex = material.transmissionTextureIndex;
  gpuMaterial.specularColorTextureIndex = material.specularColorTextureIndex;
  gpuMaterial.clearcoatTextureIndex = material.clearcoatTextureIndex;
  gpuMaterial.clearcoatRoughnessTextureIndex =
      material.clearcoatRoughnessTextureIndex;
  gpuMaterial.clearcoatNormalTextureIndex =
      material.clearcoatNormalTextureIndex;
  gpuMaterial.thicknessTextureIndex = material.thicknessTextureIndex;
  gpuMaterial.sheenColorTextureIndex = material.sheenColorTextureIndex;
  gpuMaterial.sheenRoughnessTextureIndex =
      material.sheenRoughnessTextureIndex;
  gpuMaterial.iridescenceTextureIndex = material.iridescenceTextureIndex;
  gpuMaterial.iridescenceThicknessTextureIndex =
      material.iridescenceThicknessTextureIndex;
  gpuMaterial.opacityFactor = material.opacityFactor;
  gpuMaterial.specularFactor = material.specularFactor;
  gpuMaterial.heightScale = material.heightScale;
  gpuMaterial.heightOffset = material.heightOffset;
  gpuMaterial.transmissionFactor = material.transmissionFactor;
  gpuMaterial.ior = material.ior;
  gpuMaterial.dispersion = material.dispersion;
  gpuMaterial.clearcoatFactor = material.clearcoatFactor;
  gpuMaterial.clearcoatRoughnessFactor = material.clearcoatRoughnessFactor;
  gpuMaterial.clearcoatNormalTextureScale =
      material.clearcoatNormalTextureScale;
  gpuMaterial.thicknessFactor = material.thicknessFactor;
  gpuMaterial.attenuationDistance = material.attenuationDistance;
  gpuMaterial.sheenRoughnessFactor = material.sheenRoughnessFactor;
  gpuMaterial.iridescenceFactor = material.iridescenceFactor;
  gpuMaterial.iridescenceIor = material.iridescenceIor;
  gpuMaterial.iridescenceThicknessMinimum =
      material.iridescenceThicknessMinimum;
  gpuMaterial.iridescenceThicknessMaximum =
      material.iridescenceThicknessMaximum;
  gpuMaterial.specularColorFactor =
      glm::vec4(material.specularColorFactor, 0.0f);
  gpuMaterial.attenuationColor =
      glm::vec4(material.attenuationColor, 0.0f);
  gpuMaterial.sheenColorFactor =
      glm::vec4(material.sheenColorFactor, 0.0f);
  gpuMaterial.baseColorTextureTransform =
      makeGpuTextureTransform(material.baseColorTextureTransform);
  gpuMaterial.normalTextureTransform =
      makeGpuTextureTransform(material.normalTextureTransform);
  gpuMaterial.occlusionTextureTransform =
      makeGpuTextureTransform(material.occlusionTextureTransform);
  gpuMaterial.emissiveTextureTransform =
      makeGpuTextureTransform(material.emissiveTextureTransform);
  gpuMaterial.metallicRoughnessTextureTransform =
      makeGpuTextureTransform(material.metallicRoughnessTextureTransform);
  gpuMaterial.roughnessTextureTransform =
      makeGpuTextureTransform(material.roughnessTextureTransform);
  gpuMaterial.metalnessTextureTransform =
      makeGpuTextureTransform(material.metalnessTextureTransform);
  gpuMaterial.specularTextureTransform =
      makeGpuTextureTransform(material.specularTextureTransform);
  gpuMaterial.heightTextureTransform =
      makeGpuTextureTransform(material.heightTextureTransform);
  gpuMaterial.opacityTextureTransform =
      makeGpuTextureTransform(material.opacityTextureTransform);
  gpuMaterial.transmissionTextureTransform =
      makeGpuTextureTransform(material.transmissionTextureTransform);
  gpuMaterial.specularColorTextureTransform =
      makeGpuTextureTransform(material.specularColorTextureTransform);
  gpuMaterial.clearcoatTextureTransform =
      makeGpuTextureTransform(material.clearcoatTextureTransform);
  gpuMaterial.clearcoatRoughnessTextureTransform =
      makeGpuTextureTransform(material.clearcoatRoughnessTextureTransform);
  gpuMaterial.clearcoatNormalTextureTransform =
      makeGpuTextureTransform(material.clearcoatNormalTextureTransform);
  gpuMaterial.thicknessTextureTransform =
      makeGpuTextureTransform(material.thicknessTextureTransform);
  gpuMaterial.sheenColorTextureTransform =
      makeGpuTextureTransform(material.sheenColorTextureTransform);
  gpuMaterial.sheenRoughnessTextureTransform =
      makeGpuTextureTransform(material.sheenRoughnessTextureTransform);
  gpuMaterial.iridescenceTextureTransform =
      makeGpuTextureTransform(material.iridescenceTextureTransform);
  gpuMaterial.iridescenceThicknessTextureTransform =
      makeGpuTextureTransform(material.iridescenceThicknessTextureTransform);

  if (material.alphaMode == container::material::AlphaMode::Mask) {
    gpuMaterial.flags |= container::gpu::kObjectFlagAlphaMask;
  }
  if (material.alphaMode == container::material::AlphaMode::Blend) {
    gpuMaterial.flags |= container::gpu::kObjectFlagAlphaBlend;
  }
  if (material.doubleSided) {
    gpuMaterial.flags |= container::gpu::kObjectFlagDoubleSided;
  }
  if (material.specularGlossinessWorkflow) {
    gpuMaterial.flags |= container::gpu::kObjectFlagSpecularGlossiness;
  }
  if (material.unlit) {
    gpuMaterial.flags |= container::gpu::kObjectFlagUnlit;
  }

  return gpuMaterial;
}

}  // namespace

SceneManager::SceneManager(
    container::gpu::AllocationManager& allocationManager,
    container::gpu::PipelineManager& pipelineManager,
    std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper,
    const container::app::AppConfig& config)
    : allocationManager_(&allocationManager),
      pipelineManager_(&pipelineManager),
      deviceWrapper_(std::move(deviceWrapper)),
      config_(config) {}

SceneManager::~SceneManager() {
  resetLoadedAssets();
  if (sceneClipStateBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_->destroyBuffer(sceneClipStateBuffer_);
  }

  descriptorSets_.clear();
  descriptorPool_ = VK_NULL_HANDLE;
  descriptorSetLayout_ = VK_NULL_HANDLE;

  VkDevice device = deviceWrapper_->device();
  for (VkSampler& sampler : materialSamplers_) {
    if (sampler != VK_NULL_HANDLE) {
      destroyOwnedSampler(device, sampler, nullptr);
      sampler = VK_NULL_HANDLE;
    }
  }
  materialSamplers_.clear();
  baseColorSampler_ = VK_NULL_HANDLE;
}

uint32_t SceneManager::queryTextureDescriptorCapacity() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(), &properties);

  constexpr uint32_t kReservedSampledImageDescriptors = 64;
  const uint32_t sampledImageLimit =
      std::min(properties.limits.maxPerStageDescriptorSampledImages,
               properties.limits.maxDescriptorSetSampledImages);
  const uint32_t samplerLimit =
      std::min(properties.limits.maxPerStageDescriptorSamplers,
               properties.limits.maxDescriptorSetSamplers);
  if (sampledImageLimit <
      container::gpu::kMaterialTextureDescriptorCapacity +
          kReservedSampledImageDescriptors) {
    throw std::runtime_error(
        "Device does not support the renderer material texture descriptor "
        "capacity");
  }
  if (samplerLimit < container::gpu::kMaterialSamplerDescriptorCapacity) {
    throw std::runtime_error(
        "Device does not support the renderer material sampler descriptor "
        "capacity");
  }

  return container::gpu::kMaterialTextureDescriptorCapacity;
}

uint32_t SceneManager::resolveLoadedMaterialIndex(int32_t materialIndex) const {
  if (materialIndex < 0 ||
      materialIndex >= static_cast<int32_t>(gltfModel_.materials.size())) {
    return defaultMaterialIndex_;
  }

  return gltfMaterialBaseIndex_ + static_cast<uint32_t>(materialIndex);
}

uint32_t SceneManager::diagnosticMaterialIndex() const {
  return resolveGpuMaterialIndex(diagnosticMaterialIndex_);
}

uint32_t SceneManager::resolveGpuMaterialIndex(uint32_t materialIndex) const {
  if (materialManager_.getMaterial(materialIndex) != nullptr) {
    return materialIndex;
  }
  if (materialManager_.getMaterial(defaultMaterialIndex_) != nullptr) {
    return defaultMaterialIndex_;
  }
  return 0;
}

MaterialRenderProperties SceneManager::materialRenderProperties(
    uint32_t materialIndex) const {
  MaterialRenderProperties properties{};
  properties.gpuMaterialIndex = resolveGpuMaterialIndex(materialIndex);

  const auto* material =
      materialManager_.getMaterial(properties.gpuMaterialIndex);
  if (material == nullptr) {
    return properties;
  }

  properties.transparent = isForwardTransparentMaterial(*material);
  properties.alphaMasked =
      material->alphaMode == container::material::AlphaMode::Mask;
  properties.doubleSided = material->doubleSided;
  properties.specularGlossiness = material->specularGlossinessWorkflow;
  properties.unlit = material->unlit;
  properties.heightScale = material->heightScale;
  return properties;
}

void SceneManager::initialize(const std::string& initialModelPath,
                              float importScale,
                              uint32_t descriptorSetCount) {
  textureDescriptorCapacity_ = queryTextureDescriptorCapacity();
  createDescriptorSetLayout();
  createSampler();
  loadMaterialXMaterial();

  config_.modelPath = initialModelPath;
  config_.importScale = sanitizeImportScale(importScale);
  loadGltfAssets();
  uploadMaterialBuffer();
  uploadTextureMetadataBuffer();
  createSceneClipStateBuffer();
  allocateDescriptorSets(descriptorSetCount);
}

bool SceneManager::isDefaultTestSceneActive() const {
  return isDefaultSceneRequest(config_.modelPath);
}

void SceneManager::populateSceneGraph(SceneGraph& sceneGraph) const {
  sceneGraph = SceneGraph{};

  if (primitiveRanges().empty()) {
    return;
  }

  if (gltfModel_.nodes.empty()) {
    const uint32_t rootNode = sceneGraph.createNode(
        importScaleTransform(config_.importScale), defaultMaterialIndex_,
        false, SceneGraph::kInvalidNode, "Imported Scene");
    for (size_t primitiveIndex = 0; primitiveIndex < primitiveRanges().size();
         ++primitiveIndex) {
      const auto& primitive = primitiveRanges()[primitiveIndex];
      const uint32_t primitiveNode = sceneGraph.createNode(
          glm::mat4(1.0f), resolveLoadedMaterialIndex(primitive.materialIndex),
          true, static_cast<uint32_t>(primitiveIndex),
          "Primitive " + std::to_string(primitiveIndex));
      sceneGraph.setParent(primitiveNode, rootNode);
    }
    sceneGraph.updateWorldTransforms();
    return;
  }

  std::vector<uint32_t> meshPrimitiveBase(gltfModel_.meshes.size(), 0);
  uint32_t primitiveOffset = 0;
  for (size_t meshIndex = 0; meshIndex < gltfModel_.meshes.size(); ++meshIndex) {
    meshPrimitiveBase[meshIndex] = primitiveOffset;
    primitiveOffset +=
        static_cast<uint32_t>(gltfModel_.meshes[meshIndex].primitives.size());
  }

  std::optional<uint32_t> importRootNode;
  if (sanitizeImportScale(config_.importScale) != 1.0f) {
    importRootNode = sceneGraph.createNode(
        importScaleTransform(config_.importScale), defaultMaterialIndex_, false,
        SceneGraph::kInvalidNode, "Import Scale");
  }

  std::function<void(int, std::optional<uint32_t>)> appendNode =
      [&](int gltfNodeIndex, std::optional<uint32_t> parentNode) {
        if (gltfNodeIndex < 0 ||
            gltfNodeIndex >= static_cast<int>(gltfModel_.nodes.size())) {
          return;
        }

        const auto& gltfNode = gltfModel_.nodes[gltfNodeIndex];
        const std::string nodeName =
            gltfNode.name.empty()
                ? "glTF Node " + std::to_string(gltfNodeIndex)
                : gltfNode.name;
        const uint32_t graphNode = sceneGraph.createNode(
            nodeLocalTransform(gltfNode), defaultMaterialIndex_, false,
            SceneGraph::kInvalidNode, nodeName);
        sceneGraph.setParent(graphNode, parentNode);

        if (gltfNode.mesh >= 0 &&
            gltfNode.mesh < static_cast<int>(gltfModel_.meshes.size())) {
          const auto& mesh = gltfModel_.meshes[gltfNode.mesh];
          const uint32_t basePrimitiveIndex = meshPrimitiveBase[gltfNode.mesh];

          for (size_t primitiveOffsetInMesh = 0;
               primitiveOffsetInMesh < mesh.primitives.size();
               ++primitiveOffsetInMesh) {
            const uint32_t primitiveIndex =
                basePrimitiveIndex + static_cast<uint32_t>(primitiveOffsetInMesh);
            if (primitiveIndex >= primitiveRanges().size()) {
              continue;
            }

            const auto& primitive = primitiveRanges()[primitiveIndex];
            const uint32_t primitiveNode = sceneGraph.createNode(
                glm::mat4(1.0f),
                resolveLoadedMaterialIndex(primitive.materialIndex), true,
                primitiveIndex,
                "Primitive " + std::to_string(primitiveIndex));
            sceneGraph.setParent(primitiveNode, graphNode);
          }
        }

        for (int childIndex : gltfNode.children) {
          appendNode(childIndex, graphNode);
        }
      };

  if (!gltfModel_.scenes.empty()) {
    int sceneIndex = gltfModel_.defaultScene;
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(gltfModel_.scenes.size())) {
      sceneIndex = 0;
    }

    for (int rootNodeIndex : gltfModel_.scenes[sceneIndex].nodes) {
      appendNode(rootNodeIndex, importRootNode);
    }
  } else {
    std::vector<bool> hasParent(gltfModel_.nodes.size(), false);
    for (const auto& node : gltfModel_.nodes) {
      for (int childIndex : node.children) {
        if (childIndex >= 0 &&
            childIndex < static_cast<int>(hasParent.size())) {
          hasParent[childIndex] = true;
        }
      }
    }

    for (size_t nodeIndex = 0; nodeIndex < gltfModel_.nodes.size(); ++nodeIndex) {
      if (!hasParent[nodeIndex]) {
        appendNode(static_cast<int>(nodeIndex), importRootNode);
      }
    }
  }

  sceneGraph.updateWorldTransforms();
}

uint32_t SceneManager::appendRuntimeMesh(container::geometry::Mesh mesh) {
  if (mesh.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  std::vector<container::geometry::Mesh> meshes = model_.meshes();
  const uint32_t primitiveIndex = static_cast<uint32_t>(meshes.size());
  meshes.push_back(std::move(mesh));
  model_ = container::geometry::Model::FromMeshes(std::move(meshes));
  vertices_ = model_.vertices();
  indices_ = model_.indices();
  updateModelBounds();
  indexType_ = VK_INDEX_TYPE_UINT32;
  return primitiveIndex;
}

bool SceneManager::reloadModel(
    const std::string& path,
    float importScale,
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  const std::string previousModelPath = config_.modelPath;
  const float previousImportScale = config_.importScale;
  auto resetForLoad = [this]() {
    resetLoadedAssets();
    loadMaterialXMaterial();
    vertices_.clear();
    indices_.clear();
    gltfModel_ = tinygltf::Model{};
    model_ = container::geometry::Model{};
  };

  resetForLoad();
  config_.modelPath = path;
  config_.importScale = sanitizeImportScale(importScale);

  try {
    loadGltfAssets();
    uploadMaterialBuffer();
    uploadTextureMetadataBuffer();
    updateDescriptorSets(cameraBuffers, objectBuffer);
    return true;
  } catch (...) {
    try {
      resetForLoad();
      config_.modelPath = previousModelPath;
      config_.importScale = previousImportScale;
      loadGltfAssets();
      uploadMaterialBuffer();
      uploadTextureMetadataBuffer();
      updateDescriptorSets(cameraBuffers, objectBuffer);
    } catch (const std::exception& e) {
      std::println(stderr, "failed to restore previous model '{}': {}",
                   previousModelPath, e.what());
      resetForLoad();
      uploadMaterialBuffer();
      uploadTextureMetadataBuffer();
    }
    return false;
  }
}

void SceneManager::updateDescriptorSets(
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  updateDescriptorSets(
      cameraBuffers,
      std::span<const container::gpu::AllocatedBuffer>(&objectBuffer, 1u));
}

void SceneManager::updateDescriptorSets(
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    std::span<const container::gpu::AllocatedBuffer> objectBuffers) {
  if (cameraBuffers.empty() || objectBuffers.empty() || descriptorSets_.empty())
    return;

  if (descriptorSets_.size() != cameraBuffers.size() ||
      auxiliaryDescriptorSets_.size() != cameraBuffers.size()) {
    allocateDescriptorSets(static_cast<uint32_t>(cameraBuffers.size()));
  }

  const size_t descriptorCount = std::min(descriptorSets_.size(), cameraBuffers.size());
  for (size_t i = 0; i < descriptorCount; ++i) {
    const auto& objectBuffer = objectBuffers[std::min(
        i, objectBuffers.size() - 1u)];
    writeDescriptorSetContents(descriptorSets_[i], cameraBuffers[i], objectBuffer);
  }
}

void SceneManager::updateAuxiliaryDescriptorSets(
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  updateAuxiliaryDescriptorSets(
      cameraBuffers,
      std::span<const container::gpu::AllocatedBuffer>(&objectBuffer, 1u));
}

void SceneManager::updateAuxiliaryDescriptorSets(
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    std::span<const container::gpu::AllocatedBuffer> objectBuffers) {
  if (cameraBuffers.empty() || objectBuffers.empty()) return;

  if (descriptorSets_.size() != cameraBuffers.size() ||
      auxiliaryDescriptorSets_.size() != cameraBuffers.size()) {
    allocateDescriptorSets(static_cast<uint32_t>(cameraBuffers.size()));
  }

  const size_t descriptorCount =
      std::min(auxiliaryDescriptorSets_.size(), cameraBuffers.size());
  for (size_t i = 0; i < descriptorCount; ++i) {
    const auto& objectBuffer = objectBuffers[std::min(
        i, objectBuffers.size() - 1u)];
    writeDescriptorSetContents(auxiliaryDescriptorSets_[i], cameraBuffers[i],
                               objectBuffer);
  }
}

void SceneManager::updateSceneClipState(
    const container::gpu::SceneClipState& clipState) {
  if (std::memcmp(&sceneClipState_, &clipState,
                  sizeof(container::gpu::SceneClipState)) == 0) {
    return;
  }
  sceneClipState_ = clipState;
  writeSceneClipStateBuffer();
}

/* ---------- Material resolution ---------- */

uint32_t SceneManager::createSolidMaterial(const glm::vec4& baseColor,
                                           bool doubleSided,
                                           container::material::AlphaMode alphaMode) {
  container::material::Material material{};
  material.baseColor = baseColor;
  material.metallicFactor = 0.0f;
  material.roughnessFactor = 0.82f;
  material.doubleSided = doubleSided;
  material.alphaMode = alphaMode;
  return materialManager_.createMaterial(material);
}

uint32_t SceneManager::createMaterial(
    const container::material::Material& material) {
  return materialManager_.createMaterial(material);
}

uint32_t SceneManager::loadMaterialTexture(
    const std::filesystem::path& texturePath,
    bool isSrgb,
    uint32_t samplerIndex) {
  const std::filesystem::path normalized = texturePath.lexically_normal();
  const std::string textureName = container::util::pathToUtf8(normalized);
  const std::string key = textureCacheKey(textureName, samplerIndex);
  if (const auto existing = textureManager_.findTextureIndex(key)) {
    return *existing;
  }

  auto resource = allocationManager_->createTextureFromFile(
      textureName, isSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                          : VK_FORMAT_R8G8B8A8_UNORM);
  resource.name = key;
  resource.samplerIndex = sanitizeMaterialSamplerIndex(samplerIndex);
  return textureManager_.registerTexture(resource);
}

uint32_t SceneManager::loadMaterialTextureFromBytes(
    const std::string& textureName,
    std::span<const std::byte> encodedBytes,
    bool isSrgb,
    uint32_t samplerIndex) {
  const std::string normalizedName =
      textureName.empty() ? std::string("embedded-usd-texture") : textureName;
  std::string cacheName = normalizedName;
  cacheName += "#bytes=";
  cacheName += std::to_string(encodedBytes.size());
  cacheName += ":";
  cacheName += std::to_string(hashBytes(encodedBytes));
  const std::string key = textureCacheKey(cacheName, samplerIndex);
  if (const auto existing = textureManager_.findTextureIndex(key)) {
    return *existing;
  }

  auto resource = allocationManager_->createTextureFromEncodedBytes(
      normalizedName, encodedBytes,
      isSrgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
  resource.name = key;
  resource.samplerIndex = sanitizeMaterialSamplerIndex(samplerIndex);
  return textureManager_.registerTexture(resource);
}

void SceneManager::uploadMaterialResources() {
  uploadMaterialBuffer();
  uploadTextureMetadataBuffer();
}

glm::vec4 SceneManager::resolveMaterialColor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->baseColor;
  }
  return materialBaseColor_;
}

glm::vec4 SceneManager::resolveMaterialEmissive(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->emissiveColor, 1.0f);
  }
  return glm::vec4(0.0f);
}

glm::vec2 SceneManager::resolveMaterialMetallicRoughnessFactors(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return {m->metallicFactor, m->roughnessFactor};
  }
  return {0.0f, 1.0f};
}

uint32_t SceneManager::resolveMaterialTextureIndex(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->baseColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialNormalTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->normalTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

float SceneManager::resolveMaterialNormalTextureScale(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->normalTextureScale;
  }
  return 1.0f;
}

uint32_t SceneManager::resolveMaterialOcclusionTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->occlusionTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

float SceneManager::resolveMaterialOcclusionStrength(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->occlusionStrength;
  }
  return 1.0f;
}

uint32_t SceneManager::resolveMaterialEmissiveTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->emissiveTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialMetallicRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->metallicRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->roughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialMetalnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->metalnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSpecularTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSpecularColorTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialHeightTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->heightTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialOpacityTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->opacityTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialTransmissionTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->transmissionTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialClearcoatTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialClearcoatRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialClearcoatNormalTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatNormalTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialThicknessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->thicknessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSheenColorTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->sheenColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSheenRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->sheenRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialIridescenceTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialIridescenceThicknessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceThicknessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

float SceneManager::resolveMaterialOpacityFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->opacityFactor;
  }
  return 1.0f;
}

float SceneManager::resolveMaterialSpecularFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularFactor;
  }
  return 1.0f;
}

glm::vec4 SceneManager::resolveMaterialSpecularColorFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->specularColorFactor, 0.0f);
  }
  return glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
}

float SceneManager::resolveMaterialHeightScale(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->heightScale;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialHeightOffset(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->heightOffset;
  }
  return -0.5f;
}

float SceneManager::resolveMaterialTransmissionFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->transmissionFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialEmissiveStrength(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->emissiveStrength;
  }
  return 1.0f;
}

float SceneManager::resolveMaterialIor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->ior;
  }
  return 1.5f;
}

float SceneManager::resolveMaterialDispersion(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->dispersion;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialClearcoatFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialClearcoatRoughnessFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatRoughnessFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialClearcoatNormalTextureScale(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatNormalTextureScale;
  }
  return 1.0f;
}

float SceneManager::resolveMaterialThicknessFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->thicknessFactor;
  }
  return 0.0f;
}

glm::vec4 SceneManager::resolveMaterialAttenuationColor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->attenuationColor, 0.0f);
  }
  return glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
}

float SceneManager::resolveMaterialAttenuationDistance(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->attenuationDistance;
  }
  return std::numeric_limits<float>::infinity();
}

glm::vec4 SceneManager::resolveMaterialSheenColorFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->sheenColorFactor, 0.0f);
  }
  return glm::vec4(0.0f);
}

float SceneManager::resolveMaterialSheenRoughnessFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->sheenRoughnessFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialIridescenceFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialIridescenceIor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceIor;
  }
  return 1.3f;
}

float SceneManager::resolveMaterialIridescenceThicknessMinimum(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceThicknessMinimum;
  }
  return 100.0f;
}

float SceneManager::resolveMaterialIridescenceThicknessMaximum(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceThicknessMaximum;
  }
  return 400.0f;
}

float SceneManager::resolveMaterialAlphaCutoff(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaCutoff;
  }
  return 0.5f;
}

bool SceneManager::isMaterialTransparent(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return isForwardTransparentMaterial(*m);
  }
  return false;
}

bool SceneManager::isMaterialAlphaMasked(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaMode == container::material::AlphaMode::Mask;
  }
  return false;
}

bool SceneManager::isMaterialDoubleSided(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->doubleSided;
  }
  return false;
}

bool SceneManager::usesMaterialSpecularGlossiness(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularGlossinessWorkflow;
  }
  return false;
}

bool SceneManager::isMaterialUnlit(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->unlit;
  }
  return false;
}

/* ---------- Vulkan setup ---------- */

void SceneManager::createDescriptorSetLayout() {
  std::array<VkDescriptorSetLayoutBinding, 7> bindings{};

  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER,
                 container::gpu::kMaterialSamplerDescriptorCapacity,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[5] = {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureDescriptorCapacity_,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[6] = {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  std::array<VkDescriptorBindingFlags, 7> bindingFlags{
      0, 0, 0, 0, 0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
      0};

  descriptorSetLayout_ = pipelineManager_->createDescriptorSetLayout(
      std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                bindings.end()),
      std::vector<VkDescriptorBindingFlags>(bindingFlags.begin(),
                                           bindingFlags.end()),
      0);
}

void SceneManager::createSampler() {
  if (!materialSamplers_.empty()) {
    return;
  }

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(), &properties);

  materialSamplers_.resize(container::gpu::kMaterialSamplerDescriptorCapacity,
                           VK_NULL_HANDLE);
  for (uint32_t wrapT = 0;
       wrapT < container::gpu::kMaterialSamplerWrapModeCount; ++wrapT) {
    for (uint32_t wrapS = 0;
         wrapS < container::gpu::kMaterialSamplerWrapModeCount; ++wrapS) {
      const uint32_t samplerIndex =
          wrapS + wrapT * container::gpu::kMaterialSamplerWrapModeCount;

      VkSamplerCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      info.magFilter = VK_FILTER_LINEAR;
      info.minFilter = VK_FILTER_LINEAR;
      info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
      info.addressModeU = materialSamplerAddressMode(wrapS);
      info.addressModeV = materialSamplerAddressMode(wrapT);
      info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
      info.minLod = 0.0f;
      info.maxLod = VK_LOD_CLAMP_NONE;
      info.anisotropyEnable = VK_TRUE;
      info.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
      info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

      if (createOwnedSampler(deviceWrapper_->device(), &info, nullptr,
                          &materialSamplers_[samplerIndex]) != VK_SUCCESS) {
        throw std::runtime_error("failed to create scene texture sampler");
      }
    }
  }

  baseColorSampler_ = materialSamplers_.front();
}

/* ---------- Assets ---------- */

void SceneManager::loadMaterialXMaterial() {
  container::material::Material material{};

  try {
    auto doc = materialXBridge_.loadDocument("materials/base.mtlx");
    material.baseColor = materialXBridge_.extractBaseColor(doc);
    material.opacityFactor =
        materialXBridge_.extractFloatInput(doc, "opacity", material.opacityFactor);
    material.specularFactor =
        materialXBridge_.extractFloatInput(doc, "specular", material.specularFactor);
    material.heightScale =
        materialXBridge_.extractFloatInput(doc, "height", material.heightScale);
    material.transmissionFactor = materialXBridge_.extractFloatInput(
        doc, "refraction",
        materialXBridge_.extractFloatInput(
            doc, "transmission", material.transmissionFactor));
  } catch (const std::exception& e) {
    std::println(stderr, "MaterialX load failed: {}", e.what());
    material.baseColor = glm::vec4(1.0f);
  }

  material.emissiveColor = glm::vec3(0.0f);
  material.metallicFactor = 0.0f;
  material.roughnessFactor = 1.0f;

  materialBaseColor_ = material.baseColor;

  if (defaultMaterialIndex_ == std::numeric_limits<uint32_t>::max()) {
    defaultMaterialIndex_ = materialManager_.createMaterial(material);
  } else {
    materialManager_.updateMaterial(defaultMaterialIndex_, material);
  }

  container::material::Material diagnosticMaterial{};
  diagnosticMaterial.baseColor = glm::vec4(1.0f);
  diagnosticMaterial.metallicFactor = 0.0f;
  diagnosticMaterial.roughnessFactor = 0.5f;
  if (diagnosticMaterialIndex_ == std::numeric_limits<uint32_t>::max()) {
    diagnosticMaterialIndex_ = materialManager_.createMaterial(diagnosticMaterial);
  } else {
    materialManager_.updateMaterial(diagnosticMaterialIndex_, diagnosticMaterial);
  }
}

void SceneManager::loadGltfAssets() {
  model_ = container::geometry::Model{};
  gltfModel_ = tinygltf::Model{};
  authoredPointLights_.clear();
  authoredDirectionalLights_.clear();
  authoredAreaLights_.clear();

   if (isDefaultSceneRequest(config_.modelPath)) {
    loadDefaultTestSceneAssets();
    vertices_ = model_.vertices();
    indices_ = model_.indices();
    updateModelBounds();
    indexType_ = VK_INDEX_TYPE_UINT32;
    return;
  }

  if (!config_.modelPath.empty()) {
    try {
      std::filesystem::path resolvedPath =
          container::util::pathFromUtf8(config_.modelPath);
      if (resolvedPath.is_relative() && !std::filesystem::exists(resolvedPath)) {
        resolvedPath = resolveSceneAssetPath(config_.modelPath);
      }
      auto result = container::geometry::gltf::LoadModelWithSource(
          container::util::pathToUtf8(resolvedPath));
      gltfModel_ = std::move(result.gltfModel);
      model_ = std::move(result.model);

      const auto baseDir = resolvedPath.parent_path();

      auto imageToTexture = materialXBridge_.loadTexturesForGltf(
          gltfModel_, baseDir, textureManager_,
          [this](const std::string& path, bool isSrgb) {
            return allocationManager_->createTextureFromFile(
                path, isSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                             : VK_FORMAT_R8G8B8A8_UNORM);
          });

      const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
      gltfMaterialBaseIndex_ =
          static_cast<uint32_t>(materialManager_.materialCount());
      materialXBridge_.loadMaterialsForGltf(
          gltfModel_, imageToTexture, materialManager_, defaultMaterialIndex_);
      defaultMaterialIndex_ = fallbackMaterialIndex;
    } catch (const std::exception& e) {
      std::println(stderr, "glTF load failed: {}", e.what());
      throw;
    }
  }

  vertices_ = model_.vertices();
  indices_ = model_.indices();
  updateModelBounds();
  collectAuthoredPunctualLights();
  indexType_ = VK_INDEX_TYPE_UINT32;
}

void SceneManager::loadDefaultTestSceneAssets() {
  std::vector<container::geometry::Mesh> mergedMeshes;
  mergedMeshes.reserve(container::app::kDefaultSceneModelRelativePaths.size());

  const std::array<glm::mat4, 3> transforms = {{
      makeDefaultSceneTransform(glm::vec3(-2.4f, 0.0f, 0.0f), 1.3f),
      makeDefaultSceneTransform(glm::vec3(0.0f, 0.0f, 0.0f), 1.0f),
      makeDefaultSceneTransform(glm::vec3(2.4f, 0.0f, 0.0f), 1.25f),
  }};

  container::material::Material sphereMaterial{};
  sphereMaterial.baseColor = glm::vec4(0.90f, 0.92f, 1.0f, 1.0f);
  sphereMaterial.metallicFactor = 0.0f;
  sphereMaterial.roughnessFactor = 0.45f;
  const int32_t proceduralSphereMaterialIndex = static_cast<int32_t>(
      materialManager_.createMaterial(sphereMaterial));

  for (size_t i = 0; i < container::app::kDefaultSceneModelRelativePaths.size(); ++i) {
    const auto entry = container::app::kDefaultSceneModelRelativePaths[i];
    if (isProceduralSphereEntry(entry)) {
      appendModelMeshes(container::geometry::Model::MakeSphere(), transforms[i],
                        proceduralSphereMaterialIndex, mergedMeshes);
      continue;
    }

    const std::filesystem::path assetPath =
        resolveSceneAssetPath(entry);
    appendSceneAsset(assetPath, transforms[i], mergedMeshes);
  }

  if (mergedMeshes.empty()) {
    throw std::runtime_error("failed to build default test scene from triangle/cube/sphere assets");
  }

  model_ = container::geometry::Model::FromMeshes(std::move(mergedMeshes));
  gltfModel_ = tinygltf::Model{};
}

void SceneManager::appendSceneAsset(
    const std::filesystem::path& assetPath,
    const glm::mat4& transform,
    std::vector<container::geometry::Mesh>& mergedMeshes) {
  auto result = container::geometry::gltf::LoadModelWithSource(
      container::util::pathToUtf8(assetPath));

  const auto imageToTexture = materialXBridge_.loadTexturesForGltf(
      result.gltfModel, assetPath.parent_path(), textureManager_,
      [this](const std::string& path, bool isSrgb) {
        return allocationManager_->createTextureFromFile(
            path, isSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                         : VK_FORMAT_R8G8B8A8_UNORM);
      });

  const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
  const uint32_t materialBaseIndex =
      static_cast<uint32_t>(materialManager_.materialCount());
  materialXBridge_.loadMaterialsForGltf(
      result.gltfModel, imageToTexture, materialManager_, defaultMaterialIndex_);
  defaultMaterialIndex_ = fallbackMaterialIndex;

  std::vector<uint32_t> primitiveBaseByMesh(result.gltfModel.meshes.size(), 0);
  uint32_t primitiveBase = 0;
  for (size_t meshIndex = 0; meshIndex < result.gltfModel.meshes.size(); ++meshIndex) {
    primitiveBaseByMesh[meshIndex] = primitiveBase;
    primitiveBase +=
        static_cast<uint32_t>(result.gltfModel.meshes[meshIndex].primitives.size());
  }

  uint32_t meshCursor = 0;
  for (size_t nodeIndex = 0; nodeIndex < result.gltfModel.nodes.size(); ++nodeIndex) {
    const auto& node = result.gltfModel.nodes[nodeIndex];
    if (node.mesh < 0 ||
        node.mesh >= static_cast<int>(result.gltfModel.meshes.size())) {
      continue;
    }

    const glm::mat4 nodeTransform =
        transform * composeNodeWorldTransform(result.gltfModel, static_cast<int>(nodeIndex));
    const auto& meshDef = result.gltfModel.meshes[node.mesh];
    for (size_t primitiveIndex = 0; primitiveIndex < meshDef.primitives.size(); ++primitiveIndex) {
      if (meshCursor >= result.model.meshes().size()) {
        break;
      }
      const auto& mesh = result.model.meshes()[meshCursor++];
      const uint32_t primitiveGlobalIndex =
          primitiveBaseByMesh[node.mesh] + static_cast<uint32_t>(primitiveIndex);
      int32_t sourceMaterialIndex = mesh.materialIndex();
      if (primitiveGlobalIndex < result.model.primitiveRanges().size()) {
        sourceMaterialIndex = result.model.primitiveRanges()[primitiveGlobalIndex].materialIndex;
      }
      const int32_t materialIndex = sourceMaterialIndex >= 0
          ? static_cast<int32_t>(materialBaseIndex) + sourceMaterialIndex
          : -1;
      mergedMeshes.push_back(transformMesh(mesh, nodeTransform, materialIndex));
    }
  }

  while (meshCursor < result.model.meshes().size()) {
    const auto& mesh = result.model.meshes()[meshCursor++];
    const int32_t materialIndex = mesh.materialIndex() >= 0
        ? static_cast<int32_t>(materialBaseIndex) + mesh.materialIndex()
        : -1;
    mergedMeshes.push_back(transformMesh(mesh, transform, materialIndex));
  }
}

void SceneManager::collectAuthoredPunctualLights() {
  authoredPointLights_.clear();
  authoredDirectionalLights_.clear();
  authoredAreaLights_.clear();
  if (gltfModel_.nodes.empty()) {
    return;
  }

  std::vector<bool> visited(gltfModel_.nodes.size(), false);
  std::function<void(int, const glm::mat4&)> traverseNode =
      [&](int nodeIndex, const glm::mat4& parentTransform) {
        if (nodeIndex < 0 ||
            nodeIndex >= static_cast<int>(gltfModel_.nodes.size())) {
          return;
        }
        if (visited[static_cast<size_t>(nodeIndex)]) {
          return;
        }
        visited[static_cast<size_t>(nodeIndex)] = true;

        const tinygltf::Node& node =
            gltfModel_.nodes[static_cast<size_t>(nodeIndex)];
        const glm::mat4 sceneLocalTransform =
            parentTransform * nodeLocalTransform(node);

        if (node.light >= 0 &&
            node.light < static_cast<int>(gltfModel_.lights.size())) {
          const tinygltf::Light& lightDefinition =
              gltfModel_.lights[static_cast<size_t>(node.light)];
          if (const std::optional<container::gpu::AreaLightData> areaLight =
                  makeGltfAreaLightData(lightDefinition, sceneLocalTransform,
                                        config_.importScale)) {
            authoredAreaLights_.push_back(*areaLight);
          } else if (lightDefinition.type == "point") {
            const glm::vec3 sceneLocalPosition =
                glm::vec3(sceneLocalTransform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

            container::gpu::PointLightData pointLight{};
            pointLight.positionRadius =
                glm::vec4(sceneLocalPosition,
                          gltfPointLightRange(lightDefinition,
                                              config_.importScale));
            pointLight.colorIntensity =
                glm::vec4(pointLightColorOrDefault(lightDefinition),
                          pointLightIntensityOrDefault(lightDefinition));
            authoredPointLights_.push_back(pointLight);
          } else if (lightDefinition.type == "directional") {
            AuthoredDirectionalLight directionalLight{};
            directionalLight.direction =
                glm::vec4(gltfDirectionalLightDirection(sceneLocalTransform),
                          0.0f);
            directionalLight.colorIntensity =
                glm::vec4(pointLightColorOrDefault(lightDefinition),
                          pointLightIntensityOrDefault(lightDefinition));
            authoredDirectionalLights_.push_back(directionalLight);
          } else if (lightDefinition.type == "spot") {
            const glm::vec3 sceneLocalPosition =
                glm::vec3(sceneLocalTransform *
                          glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            const glm::vec3 sceneLocalDirection =
                gltfDirectionalLightDirection(sceneLocalTransform);
            const auto [innerCos, outerCos] =
                gltfSpotConeCosines(lightDefinition);

            container::gpu::PointLightData spotLight{};
            spotLight.positionRadius =
                glm::vec4(sceneLocalPosition,
                          gltfPointLightRange(lightDefinition,
                                              config_.importScale));
            spotLight.colorIntensity =
                glm::vec4(pointLightColorOrDefault(lightDefinition),
                          pointLightIntensityOrDefault(lightDefinition));
            spotLight.directionInnerCos =
                glm::vec4(sceneLocalDirection, innerCos);
            spotLight.coneOuterCosType =
                glm::vec4(outerCos, container::gpu::kLightTypeSpot, 0.0f, 0.0f);
            authoredPointLights_.push_back(spotLight);
          }
        }

        for (const char* extensionName : kKnownAreaLightExtensions) {
          const auto extensionIt = node.extensions.find(extensionName);
          if (extensionIt == node.extensions.end()) {
            continue;
          }

          const std::optional<int> areaLightIndex =
              readAreaLightIndex(extensionIt->second);
          if (!areaLightIndex) {
            continue;
          }

          const tinygltf::Value* areaLightValue =
              findModelAreaLightDefinition(gltfModel_, extensionName,
                                           *areaLightIndex);
          if (areaLightValue == nullptr) {
            continue;
          }

          const tinygltf::Light areaDefinition =
              areaLightDefinitionFromValue(*areaLightValue);
          if (const std::optional<container::gpu::AreaLightData> areaLight =
                  makeGltfAreaLightData(areaDefinition, sceneLocalTransform,
                                        config_.importScale)) {
            authoredAreaLights_.push_back(*areaLight);
          }
        }

        for (int childIndex : node.children) {
          traverseNode(childIndex, sceneLocalTransform);
        }
      };

  const glm::mat4 rootTransform = importScaleTransform(config_.importScale);
  for (int rootNodeIndex : activeSceneRootNodes(gltfModel_)) {
    traverseNode(rootNodeIndex, rootTransform);
  }
}

void SceneManager::uploadMaterialBuffer() {
  gpuMaterials_.clear();
  const size_t materialCount = materialManager_.materialCount();
  gpuMaterials_.reserve(std::max<size_t>(1, materialCount));
  for (uint32_t i = 0; i < materialCount; ++i) {
    if (const auto* material = materialManager_.getMaterial(i)) {
      gpuMaterials_.push_back(makeGpuMaterial(*material));
    }
  }
  if (gpuMaterials_.empty()) {
    gpuMaterials_.push_back(container::gpu::GpuMaterial{});
  }

  const size_t requiredCount = gpuMaterials_.size();
  const VkDeviceSize requiredSize =
      static_cast<VkDeviceSize>(sizeof(container::gpu::GpuMaterial) *
                                requiredCount);

  if (materialBuffer_.buffer == VK_NULL_HANDLE ||
      materialBufferCapacity_ < requiredCount) {
    if (materialBuffer_.buffer != VK_NULL_HANDLE) {
      allocationManager_->destroyBuffer(materialBuffer_);
      materialBufferCapacity_ = 0;
    }
    materialBuffer_ = allocationManager_->createBuffer(
        requiredSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    materialBufferCapacity_ = requiredCount;
  }

  void* mapped = materialBuffer_.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager_->memoryManager()->allocator(),
                     materialBuffer_.allocation, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map material buffer for writing");
    }
    mappedHere = true;
  }

  std::memcpy(mapped, gpuMaterials_.data(),
              static_cast<size_t>(requiredSize));
  if (vmaFlushAllocation(allocationManager_->memoryManager()->allocator(),
                         materialBuffer_.allocation, 0,
                         requiredSize) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(allocationManager_->memoryManager()->allocator(),
                     materialBuffer_.allocation);
    }
    throw std::runtime_error("failed to flush material buffer after writing");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager_->memoryManager()->allocator(),
                   materialBuffer_.allocation);
  }
}

void SceneManager::uploadTextureMetadataBuffer() {
  textureMetadata_.clear();
  const size_t textureCount = textureManager_.textureCount();
  textureMetadata_.reserve(std::max<size_t>(1, textureCount));
  for (uint32_t i = 0; i < textureCount; ++i) {
    container::gpu::GpuTextureMetadata metadata{};
    if (const auto* texture = textureManager_.getTexture(i)) {
      metadata.samplerIndex = std::min(
          texture->samplerIndex,
          container::gpu::kMaterialSamplerDescriptorCapacity - 1u);
    }
    textureMetadata_.push_back(metadata);
  }
  if (textureMetadata_.empty()) {
    textureMetadata_.push_back(container::gpu::GpuTextureMetadata{});
  }

  const size_t requiredCount = textureMetadata_.size();
  const VkDeviceSize requiredSize =
      static_cast<VkDeviceSize>(sizeof(container::gpu::GpuTextureMetadata) *
                                requiredCount);

  if (textureMetadataBuffer_.buffer == VK_NULL_HANDLE ||
      textureMetadataBufferCapacity_ < requiredCount) {
    if (textureMetadataBuffer_.buffer != VK_NULL_HANDLE) {
      allocationManager_->destroyBuffer(textureMetadataBuffer_);
      textureMetadataBufferCapacity_ = 0;
    }
    textureMetadataBuffer_ = allocationManager_->createBuffer(
        requiredSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    textureMetadataBufferCapacity_ = requiredCount;
  }

  void* mapped = textureMetadataBuffer_.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager_->memoryManager()->allocator(),
                     textureMetadataBuffer_.allocation, &mapped) !=
        VK_SUCCESS) {
      throw std::runtime_error(
          "failed to map texture metadata buffer for writing");
    }
    mappedHere = true;
  }

  std::memcpy(mapped, textureMetadata_.data(),
              static_cast<size_t>(requiredSize));
  if (vmaFlushAllocation(allocationManager_->memoryManager()->allocator(),
                         textureMetadataBuffer_.allocation, 0,
                         requiredSize) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(allocationManager_->memoryManager()->allocator(),
                     textureMetadataBuffer_.allocation);
    }
    throw std::runtime_error(
        "failed to flush texture metadata buffer after writing");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager_->memoryManager()->allocator(),
                   textureMetadataBuffer_.allocation);
  }
}

void SceneManager::createSceneClipStateBuffer() {
  if (sceneClipStateBuffer_.buffer != VK_NULL_HANDLE) {
    return;
  }

  sceneClipStateBuffer_ = allocationManager_->createBuffer(
      sizeof(container::gpu::SceneClipState),
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  writeSceneClipStateBuffer();
}

void SceneManager::writeSceneClipStateBuffer() {
  if (sceneClipStateBuffer_.buffer == VK_NULL_HANDLE) {
    createSceneClipStateBuffer();
    return;
  }
  writeHostVisibleBuffer(*allocationManager_, sceneClipStateBuffer_,
                         &sceneClipState_, sizeof(sceneClipState_));
}

std::filesystem::path SceneManager::resolveSceneAssetPath(
    std::string_view relativePath) const {
  const std::filesystem::path requestedPath =
      container::util::pathFromUtf8(relativePath);
  const std::filesystem::path exeRelative =
      container::util::executableDirectory() / requestedPath;
  if (std::filesystem::exists(exeRelative)) {
    return exeRelative;
  }

  const std::filesystem::path workspaceRelative = requestedPath;
  if (std::filesystem::exists(workspaceRelative)) {
    return workspaceRelative;
  }

  return exeRelative;
}

void SceneManager::updateModelBounds() {
  modelBounds_ = ModelBounds{};
  if (vertices_.empty()) {
    return;
  }

  auto forEachRenderedPoint =
      [this](const std::function<void(const glm::vec3&)>& visit) {
        const glm::mat4 rootTransform =
            importScaleTransform(config_.importScale);

        auto emitPrimitive = [&](uint32_t primitiveIndex,
                                 const glm::mat4& transform) {
          if (primitiveIndex >= model_.primitiveRanges().size()) {
            return;
          }

          const auto& primitive = model_.primitiveRanges()[primitiveIndex];
          const uint32_t endIndex = primitive.firstIndex + primitive.indexCount;
          for (uint32_t i = primitive.firstIndex;
               i < endIndex && i < indices_.size(); ++i) {
            const uint32_t vertexIndex = indices_[i];
            if (vertexIndex >= vertices_.size()) {
              continue;
            }
            visit(glm::vec3(transform *
                            glm::vec4(vertices_[vertexIndex].position, 1.0f)));
          }
        };

        if (!gltfModel_.nodes.empty() && !gltfModel_.meshes.empty()) {
          std::vector<uint32_t> meshPrimitiveBase(gltfModel_.meshes.size(), 0);
          uint32_t primitiveOffset = 0;
          for (size_t meshIndex = 0; meshIndex < gltfModel_.meshes.size();
               ++meshIndex) {
            meshPrimitiveBase[meshIndex] = primitiveOffset;
            primitiveOffset += static_cast<uint32_t>(
                gltfModel_.meshes[meshIndex].primitives.size());
          }

          std::function<void(int, const glm::mat4&)> traverseNode =
              [&](int gltfNodeIndex, const glm::mat4& parentTransform) {
                if (gltfNodeIndex < 0 ||
                    gltfNodeIndex >= static_cast<int>(gltfModel_.nodes.size())) {
                  return;
                }

                const auto& node = gltfModel_.nodes[gltfNodeIndex];
                const glm::mat4 worldTransform =
                    parentTransform * nodeLocalTransform(node);

                if (node.mesh >= 0 &&
                    node.mesh < static_cast<int>(gltfModel_.meshes.size())) {
                  const auto& mesh = gltfModel_.meshes[node.mesh];
                  const uint32_t basePrimitiveIndex =
                      meshPrimitiveBase[node.mesh];
                  for (size_t primitiveInMesh = 0;
                       primitiveInMesh < mesh.primitives.size();
                       ++primitiveInMesh) {
                    emitPrimitive(
                        basePrimitiveIndex +
                            static_cast<uint32_t>(primitiveInMesh),
                        worldTransform);
                  }
                }

                for (int childIndex : node.children) {
                  traverseNode(childIndex, worldTransform);
                }
              };

          if (!gltfModel_.scenes.empty()) {
            int sceneIndex = gltfModel_.defaultScene;
            if (sceneIndex < 0 ||
                sceneIndex >= static_cast<int>(gltfModel_.scenes.size())) {
              sceneIndex = 0;
            }

            for (int rootNodeIndex : gltfModel_.scenes[sceneIndex].nodes) {
              traverseNode(rootNodeIndex, rootTransform);
            }
            return;
          }

          std::vector<bool> hasParent(gltfModel_.nodes.size(), false);
          for (const auto& node : gltfModel_.nodes) {
            for (int childIndex : node.children) {
              if (childIndex >= 0 &&
                  childIndex < static_cast<int>(hasParent.size())) {
                hasParent[childIndex] = true;
              }
            }
          }

          for (size_t nodeIndex = 0; nodeIndex < gltfModel_.nodes.size();
               ++nodeIndex) {
            if (!hasParent[nodeIndex]) {
              traverseNode(static_cast<int>(nodeIndex), rootTransform);
            }
          }
          return;
        }

        for (const auto& vertex : vertices_) {
          visit(glm::vec3(rootTransform * glm::vec4(vertex.position, 1.0f)));
        }
      };

  bool hasPoint = false;
  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
  forEachRenderedPoint([&](const glm::vec3& point) {
    hasPoint = true;
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  });

  if (!hasPoint) {
    return;
  }

  const glm::vec3 center = 0.5f * (minBounds + maxBounds);
  float radius = 0.0f;
  forEachRenderedPoint([&](const glm::vec3& point) {
    radius = std::max(radius, glm::length(point - center));
  });

  modelBounds_.min = minBounds;
  modelBounds_.max = maxBounds;
  modelBounds_.center = center;
  modelBounds_.size = maxBounds - minBounds;
  modelBounds_.radius = radius;
  modelBounds_.valid = true;
}

/* ---------- Descriptor sets ---------- */

void SceneManager::allocateDescriptorSets(uint32_t descriptorSetCount) {
  const uint32_t textureDescriptorCount =
      std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager_.textureCount()));
  const uint32_t frameSetCount = std::max(1u, descriptorSetCount);
  const uint32_t setCount = frameSetCount * 2u;

  if (descriptorPool_ != VK_NULL_HANDLE) {
    pipelineManager_->destroyDescriptorPool(descriptorPool_);
  }
  descriptorSets_.clear();
  auxiliaryDescriptorSets_.clear();

  if (textureDescriptorCount > textureDescriptorCapacity_) {
    throw std::runtime_error("Model requires more sampled-image descriptors than "
                             "the device supports");
  }

  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * 2u},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 3u},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
       textureDescriptorCapacity_ * setCount},
      {VK_DESCRIPTOR_TYPE_SAMPLER,
       container::gpu::kMaterialSamplerDescriptorCapacity * setCount},
  };

  descriptorPool_ = pipelineManager_->createDescriptorPool(poolSizes, setCount, 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = setCount;

  std::vector<VkDescriptorSetLayout> layouts(setCount, descriptorSetLayout_);
  allocInfo.pSetLayouts = layouts.data();

  // Binding 5 has a fixed device-sized capacity. Variable descriptor counts
  // would require it to be the highest binding; camera data lives at binding 6.

  std::vector<VkDescriptorSet> allocatedSets(setCount, VK_NULL_HANDLE);
  if (vkAllocateDescriptorSets(deviceWrapper_->device(), &allocInfo,
                               allocatedSets.data()) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate scene descriptor sets");
  }

  descriptorSets_.assign(allocatedSets.begin(),
                         allocatedSets.begin() + frameSetCount);
  auxiliaryDescriptorSets_.assign(allocatedSets.begin() + frameSetCount,
                                  allocatedSets.end());
}

void SceneManager::writeDescriptorSetContents(
    VkDescriptorSet descriptorSet,
    const container::gpu::AllocatedBuffer& cameraBuffer,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  if (descriptorSet == VK_NULL_HANDLE) return;
  if (materialBuffer_.buffer == VK_NULL_HANDLE) {
    uploadMaterialBuffer();
  }
  if (textureMetadataBuffer_.buffer == VK_NULL_HANDLE) {
    uploadTextureMetadataBuffer();
  }
  if (sceneClipStateBuffer_.buffer == VK_NULL_HANDLE) {
    createSceneClipStateBuffer();
  }

  VkDescriptorBufferInfo cameraInfo{cameraBuffer.buffer, 0, sizeof(container::gpu::CameraData)};
  VkDescriptorBufferInfo objectInfo{
      objectBuffer.buffer, 0, objectBuffer.allocation_info.size};
  VkDescriptorBufferInfo materialInfo{
      materialBuffer_.buffer, 0,
      static_cast<VkDeviceSize>(
          sizeof(container::gpu::GpuMaterial) *
          std::max<size_t>(1, gpuMaterials_.size()))};
  VkDescriptorBufferInfo textureMetadataInfo{
      textureMetadataBuffer_.buffer, 0,
      static_cast<VkDeviceSize>(
          sizeof(container::gpu::GpuTextureMetadata) *
          std::max<size_t>(1, textureMetadata_.size()))};
  VkDescriptorBufferInfo sceneClipInfo{
      sceneClipStateBuffer_.buffer, 0, sizeof(container::gpu::SceneClipState)};

  std::vector<VkWriteDescriptorSet> writes;

  VkWriteDescriptorSet cameraWrite{};
  cameraWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  cameraWrite.dstSet = descriptorSet;
  cameraWrite.dstBinding = 0;
  cameraWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  cameraWrite.descriptorCount = 1;
  cameraWrite.pBufferInfo = &cameraInfo;
  writes.push_back(cameraWrite);

  VkWriteDescriptorSet objectWrite{};
  objectWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  objectWrite.dstSet = descriptorSet;
  objectWrite.dstBinding = 1;
  objectWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  objectWrite.descriptorCount = 1;
  objectWrite.pBufferInfo = &objectInfo;
  writes.push_back(objectWrite);

  VkWriteDescriptorSet materialWrite{};
  materialWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  materialWrite.dstSet = descriptorSet;
  materialWrite.dstBinding = 3;
  materialWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  materialWrite.descriptorCount = 1;
  materialWrite.pBufferInfo = &materialInfo;
  writes.push_back(materialWrite);

  VkWriteDescriptorSet textureMetadataWrite{};
  textureMetadataWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  textureMetadataWrite.dstSet = descriptorSet;
  textureMetadataWrite.dstBinding = 4;
  textureMetadataWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  textureMetadataWrite.descriptorCount = 1;
  textureMetadataWrite.pBufferInfo = &textureMetadataInfo;
  writes.push_back(textureMetadataWrite);

  VkWriteDescriptorSet sceneClipWrite{};
  sceneClipWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  sceneClipWrite.dstSet = descriptorSet;
  sceneClipWrite.dstBinding = 6;
  sceneClipWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  sceneClipWrite.descriptorCount = 1;
  sceneClipWrite.pBufferInfo = &sceneClipInfo;
  writes.push_back(sceneClipWrite);

  std::vector<VkDescriptorImageInfo> imageInfos;
  const size_t texCount = textureManager_.textureCount();
  if (texCount > textureDescriptorCapacity_) {
    throw std::runtime_error("Model requires more sampled-image descriptors than "
                             "the device supports");
  }

  for (size_t i = 0; i < texCount; ++i) {
    const auto* tex = textureManager_.getTexture(static_cast<uint32_t>(i));
    if (!tex) continue;
    imageInfos.push_back({VK_NULL_HANDLE, tex->imageView,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
  }

  if (!imageInfos.empty()) {
    VkWriteDescriptorSet imageWrite{};
    imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    imageWrite.dstSet = descriptorSet;
    imageWrite.dstBinding = 5;
    imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    imageWrite.descriptorCount = static_cast<uint32_t>(imageInfos.size());
    imageWrite.pImageInfo = imageInfos.data();
    writes.push_back(imageWrite);
  }

  std::array<VkDescriptorImageInfo,
             container::gpu::kMaterialSamplerDescriptorCapacity>
      samplerInfos{};
  for (uint32_t i = 0; i < container::gpu::kMaterialSamplerDescriptorCapacity;
       ++i) {
    samplerInfos[i].sampler =
        i < materialSamplers_.size() ? materialSamplers_[i] : baseColorSampler_;
    samplerInfos[i].imageView = VK_NULL_HANDLE;
    samplerInfos[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  VkWriteDescriptorSet samplerWrite{};
  samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  samplerWrite.dstSet = descriptorSet;
  samplerWrite.dstBinding = 2;
  samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  samplerWrite.descriptorCount =
      container::gpu::kMaterialSamplerDescriptorCapacity;
  samplerWrite.pImageInfo = samplerInfos.data();
  writes.push_back(samplerWrite);

  vkUpdateDescriptorSets(deviceWrapper_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

void SceneManager::resetLoadedAssets() {
  if (materialBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_->destroyBuffer(materialBuffer_);
  }
  if (textureMetadataBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_->destroyBuffer(textureMetadataBuffer_);
  }
  materialBufferCapacity_ = 0;
  textureMetadataBufferCapacity_ = 0;
  gpuMaterials_.clear();
  textureMetadata_.clear();
  allocationManager_->resetTextureAllocations();
  materialManager_ = container::material::MaterialManager{};
  textureManager_ = container::material::TextureManager{};
  defaultMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  diagnosticMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  gltfMaterialBaseIndex_ = 0;
  materialBaseColor_ = glm::vec4(1.0f);
  modelBounds_ = ModelBounds{};
  authoredPointLights_.clear();
  authoredDirectionalLights_.clear();
  authoredAreaLights_.clear();
}

}  // namespace container::scene
