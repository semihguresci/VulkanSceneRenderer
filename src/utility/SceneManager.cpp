#include <algorithm>
#include <tuple>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <print>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/GltfAnimationRuntime.h"
#include "Container/geometry/GltfMaterialExport.h"
#include "Container/geometry/GltfMaterialTextureAuthoring.h"
#include "Container/geometry/GltfImageSources.h"
#include "Container/geometry/GltfMaterialExportCompaction.h"
#include "Container/geometry/GltfNodeFlags.h"
#include "Container/geometry/GltfNodeExportCompaction.h"
#include "Container/geometry/GltfSceneExport.h"
#include "Container/geometry/Mesh.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneNodeAuthoringPolicy.h"
#include "Container/utility/GltfSceneInstancing.h"
#include "Container/utility/GltfSourceMaterialExport.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SourceMaterialMapping.h"
#include "Container/utility/SourcePrimitiveMapping.h"
#include "Container/renderer/temporal/TemporalMaterialRevision.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"

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

container::geometry::GltfLoadResult prepareSceneGltfAsset(
    const std::filesystem::path& path,
    container::geometry::gltf::LoadOptions options) {
  options.retainEncodedImages = true;
  auto result = container::geometry::gltf::LoadModelWithSource(
      container::util::pathToUtf8(path), options);
  (void)container::geometry::gltf::ParseMaterialVariants(result.gltfModel);
  (void)BuildGltfSceneCatalog(result.gltfModel);
  return result;
}

container::geometry::GltfLoadResult prepareDefaultSceneGltfAsset(
    const std::filesystem::path& path) {
  container::geometry::gltf::LoadOptions options;
  // The diagnostic composition flattens meshes and materials. It does not
  // publish the source nodes, light bindings, animations or variant catalog.
  options.unavailableRuntimeExtensions = {
      "EXT_mesh_gpu_instancing", "KHR_animation_pointer", "KHR_materials_variants",
      "KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability",
      "KHR_lights_punctual", "KHR_lights_area", "EXT_lights_area", "WEBGI_lights_area",
      "KHR_xmp_json_ld",
  };
  return prepareSceneGltfAsset(path, std::move(options));
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
         material.transmissionFactor > 0.0f;
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

// CPU authoring state retains all scenes; only the selected view consumes the
// device's object table. Bound aggregate expansion independently of that view.
constexpr uint64_t kMaxAssetRenderableObjects = 262144;
constexpr uint64_t kMaxAssetGraphNodes = 524288;

size_t sourcePrimitiveCount(const tinygltf::Model& source) {
  size_t count = 0;
  for (const auto& mesh : source.meshes) {
    if (mesh.primitives.size() > size_t(std::numeric_limits<int32_t>::max()) - count)
      throw std::overflow_error("Source primitive table exceeds its identity range");
    count += mesh.primitives.size();
  }
  return count;
}

SceneGraph prepareCompleteHierarchy(const tinygltf::Model& source,
    const container::geometry::gltf::GltfInstancingData& instances,
    std::span<const uint32_t> materials,const glm::mat4& importTransform,
    uint64_t materialCount,uint64_t runtimePrimitiveCount,uint64_t activeObjectLimit,int requestedScene,
    std::span<const uint32_t> sourcePrimitiveToRuntime = {}) {
  const auto catalog=BuildGltfSceneCatalog(source);
  auto graph=BuildGltfSceneHierarchy(source,instances,materials,importTransform,
      {kMaxAssetRenderableObjects,kMaxAssetGraphNodes,runtimePrimitiveCount,materialCount},
      GltfHierarchyScope::CompleteAsset,sourcePrimitiveToRuntime);
  const auto selected=requestedScene>=0 ? size_t(requestedScene) : catalog.defaultScene;
  const auto activation=PlanGltfSceneActivation(catalog,graph,selected,activeObjectLimit);
  graph.setActiveRoots(activation.runtimeRoots);
  return graph;
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
  gpuMaterial.anisotropyDiffuseFactors = glm::vec4(material.anisotropyStrength,
      material.anisotropyRotation, material.diffuseTransmissionFactor, 0.0f);
  gpuMaterial.diffuseTransmissionColorFactor = glm::vec4(material.diffuseTransmissionColorFactor, 0.0f);
  gpuMaterial.anisotropyTextureIndex = material.anisotropyTextureIndex;
  gpuMaterial.diffuseTransmissionTextureIndex = material.diffuseTransmissionTextureIndex;
  gpuMaterial.diffuseTransmissionColorTextureIndex = material.diffuseTransmissionColorTextureIndex;
  gpuMaterial.anisotropyTextureTransform = makeGpuTextureTransform(material.anisotropyTextureTransform);
  gpuMaterial.diffuseTransmissionTextureTransform = makeGpuTextureTransform(material.diffuseTransmissionTextureTransform);
  gpuMaterial.diffuseTransmissionColorTextureTransform = makeGpuTextureTransform(material.diffuseTransmissionColorTextureTransform);

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
      config_(config) {
  // Scratch is not an exported glTF asset. Keep its empty payload canonical
  // before and after move-assignment exchanges, including resident-byte counts.
  modelExchangeScratch_.asset.version.clear();
}

SceneManager::~SceneManager() {
  if (descriptorPool_ != VK_NULL_HANDLE) {
    if (ownsDescriptorPool_) {
      vkDestroyDescriptorPool(deviceWrapper_->device(), descriptorPool_, nullptr);
      descriptorPool_ = VK_NULL_HANDLE;
    } else {
      pipelineManager_->destroyDescriptorPool(descriptorPool_);
    }
  }
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
  if (sampledImageLimit <
      container::gpu::kMaterialTextureDescriptorCapacity +
          kReservedSampledImageDescriptors) {
    throw std::runtime_error(
        "Device does not support the renderer material texture descriptor "
        "capacity");
  }
  return container::gpu::kMaterialTextureDescriptorCapacity;
}

uint32_t SceneManager::queryMaterialSamplerDescriptorCapacity() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(), &properties);
  const auto capacity = container::material::MaterialSamplerDescriptorCapacity(
      properties.limits.maxPerStageDescriptorSamplers, properties.limits.maxDescriptorSetSamplers);
  if (!capacity) throw std::runtime_error("Device has no material sampler budget after reserving pipeline resources");
  return capacity;
}

void SceneManager::validateMaterialSamplerPolicies(
    std::span<const container::material::MaterialSamplerKey> keys) const {
  (void)container::material::PlanTextureSamplerSlots(
      textureManager_,materialSamplerDescriptorCapacity_,keys);
}

void SceneManager::validateGltfSamplerPolicies(const tinygltf::Model& model) const {
  validateMaterialSamplerPolicies(container::material::CollectGltfMaterialSamplerKeys(model));
}

uint32_t SceneManager::resolveLoadedMaterialIndex(int32_t materialIndex) const {
  return ResolveSourceMaterialIndex(materialIndex, sourceMaterialToRuntime_,
      gltfModel_.materials.size(), materialManager_.materialCount(), defaultMaterialIndex_);
}

uint32_t SceneManager::resolvePrimitiveMaterialIndex(size_t sourcePrimitive,int32_t materialIndex) const {
  auto fallback=defaultMaterialIndex_;
  if(!sourcePrimitiveImplicitMaterials_.empty()) {
    if(sourcePrimitiveImplicitMaterials_.size()!=sourcePrimitiveToRuntime_.size() ||
        sourcePrimitive>=sourcePrimitiveImplicitMaterials_.size())
      throw std::runtime_error("Source primitive implicit material table is incomplete");
    fallback=sourcePrimitiveImplicitMaterials_[sourcePrimitive];
  }
  return ResolveSourceMaterialIndex(materialIndex,sourceMaterialToRuntime_,
      gltfModel_.materials.size(),materialManager_.materialCount(),fallback);
}

uint64_t SceneManager::temporalMaterialRevision(uint32_t gpuIndex) const {
  return gpuIndex < gpuMaterialTemporalRevisions_.size() ? gpuMaterialTemporalRevisions_[gpuIndex] : 0;
}

bool SceneManager::rayGeometryCompatible(uint32_t gpuIndex) const {
  if (gpuIndex >= gpuMaterials_.size())
    return false;
  const auto &material = gpuMaterials_[gpuIndex];
  // Raster vertices sample height maps at mip zero. Until displaced positions
  // are shared with AS builds, keep the complete frame on the raster route.
  return material.heightTextureIndex == UINT32_MAX ||
         std::abs(material.heightScale) <= 1e-7f;
}

uint32_t SceneManager::diagnosticMaterialIndex() const {
  return resolveGpuMaterialIndex(diagnosticMaterialIndex_);
}

uint32_t SceneManager::resolveGpuMaterialIndex(uint32_t materialIndex) const {
  if (materialIsActive(materialIndex)) {
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
  // Publication inventories include inert retired rows so their SSBO positions
  // remain stable. Authoring/binding entry points separately require active IDs.
  properties.gpuMaterialIndex = materialManager_.getMaterial(materialIndex)
      ? materialIndex : resolveGpuMaterialIndex(materialIndex);

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

container::gpu::GpuMaterial SceneManager::packMaterial(const container::material::Material& material) {
  return makeGpuMaterial(material);
}

void SceneManager::initialize(const std::string& initialModelPath,
                              float importScale,
                              uint32_t descriptorSetCount) {
  textureDescriptorCapacity_ = queryTextureDescriptorCapacity();
  materialSamplerDescriptorCapacity_ = queryMaterialSamplerDescriptorCapacity();
  createDescriptorSetLayout();
  createSampler();
  loadMaterialXMaterial();

  config_.modelPath = initialModelPath;
  config_.importScale = sanitizeImportScale(importScale);
  loadGltfAssets();
  resetCameraProjectionNamespace();
  resetMaterialLifecycle();
  uploadMaterialBuffer();
  uploadTextureMetadataBuffer();
  createSceneClipStateBuffer();
  allocateDescriptorSets(descriptorSetCount);
  finishTextureImageOwnershipPublication();
}

bool SceneManager::isDefaultTestSceneActive() const {
  return !mergedAssets_ && isDefaultSceneRequest(config_.modelPath);
}

void SceneManager::populateSceneGraph(SceneGraph& sceneGraph) {
  std::vector<uint32_t> materialIndices;
  const auto sourceCount = sourcePrimitiveCount(gltfModel_);
  ValidateSourcePrimitiveIds(sourcePrimitiveToRuntime_,sourceCount,primitiveRanges().size());
  if (sourceCount) {
    materialIndices.reserve(sourceCount);
    for (size_t source=0;source<sourcePrimitiveToRuntime_.size();++source)
      materialIndices.push_back(resolvePrimitiveMaterialIndex(source,
          primitiveRanges()[sourcePrimitiveToRuntime_[source]].materialIndex));
  } else {
    // The procedural/default aggregate stores runtime material IDs directly.
    materialIndices.reserve(primitiveRanges().size());
    for (const auto& primitive : primitiveRanges())
      materialIndices.push_back(resolveGpuMaterialIndex(primitive.materialIndex >= 0
          ? static_cast<uint32_t>(primitive.materialIndex) : defaultMaterialIndex_));
  }
  auto staged = prepareCompleteHierarchy(gltfModel_,gltfInstancing_,materialIndices,
      importScaleTransform(config_.importScale),materialManager_.materialCount(),
      model_.meshes().size(),config_.maxSceneObjects,int(selectedScene_),sourcePrimitiveToRuntime_);
  invalidateEditorSceneRootMembership();
  additionalSceneRoots_.assign(sceneCatalog_.entries.size(),{});
  sceneRootOrder_.clear();
  sceneRootsDirty_=false;
  sceneGraph = std::move(staged);
  refreshActiveScene(sceneGraph);
}

GltfSceneActivation SceneManager::planSceneSelection(const SceneGraph& graph,size_t selected) const {
  return planSceneSelection(graph,selected,config_.maxSceneObjects);
}

GltfSceneActivation SceneManager::planSceneSelection(
    const SceneGraph& graph,size_t selected,uint64_t maxObjects) const {
  if(selected>=additionalSceneRoots_.size())
    throw std::invalid_argument("glTF scene selection: requested scene index is invalid");
  auto result=PlanGltfSceneActivation(sceneCatalog_,graph,selected,maxObjects,
      additionalSceneRoots_[selected]);
  if(selected<sceneRootOrder_.size() && !sceneRootOrder_[selected].empty()) {
    std::vector<uint8_t> remaining(graph.nodeCount(),0);
    for(const auto root:result.runtimeRoots) remaining[root]=1;
    std::vector<uint32_t> ordered;
    ordered.reserve(result.runtimeRoots.size());
    for(const auto root:sceneRootOrder_[selected])
      if(root<remaining.size() && remaining[root]) {
        ordered.push_back(root); remaining[root]=0;
      }
    for(const auto root:result.runtimeRoots)
      if(remaining[root]) { ordered.push_back(root); remaining[root]=0; }
    result.runtimeRoots=std::move(ordered);
  }
  return result;
}

struct SceneManager::PreparedEditorSceneRootChange::State {
  const SceneManager* owner{};
  const SceneGraph* graph{};
  EditorSceneRootMembership receipt;
  uint64_t generation{}, revision{}, catalog{};
  size_t rootTableSize{}, orderTableSize{};
  std::vector<uint32_t> roots, order;
  bool hasOrder{}, dirty{}, present{}, consumed{};
};

SceneManager::PreparedEditorSceneRootChange::PreparedEditorSceneRootChange() noexcept = default;
SceneManager::PreparedEditorSceneRootChange::~PreparedEditorSceneRootChange() = default;
SceneManager::PreparedEditorSceneRootChange::PreparedEditorSceneRootChange(PreparedEditorSceneRootChange&&) noexcept = default;
SceneManager::PreparedEditorSceneRootChange& SceneManager::PreparedEditorSceneRootChange::operator=(PreparedEditorSceneRootChange&&) noexcept = default;
SceneManager::PreparedEditorSceneRootChange::operator bool() const noexcept {
  return state_ && state_->owner && !state_->consumed;
}
const SceneManager::EditorSceneRootMembership& SceneManager::PreparedEditorSceneRootChange::membership() const noexcept {
  static const EditorSceneRootMembership empty;
  return state_ ? state_->receipt : empty;
}
std::span<const uint32_t> SceneManager::PreparedEditorSceneRootChange::candidateRoots() const noexcept {
  return state_ ? std::span<const uint32_t>(state_->roots) : std::span<const uint32_t>{};
}

void SceneManager::invalidateEditorSceneRootMembership() noexcept {
  // Saturation permanently disables new preparations rather than allowing old
  // proofs to alias a wrapped counter. This also works during destruction.
  if (sceneRootMembershipRevision_ < UINT64_MAX) ++sceneRootMembershipRevision_;
  if (sceneRootCatalogIdentity_ < UINT64_MAX) ++sceneRootCatalogIdentity_;
  if (authoredMaterialRevision_ < UINT64_MAX) ++authoredMaterialRevision_;
}

SceneManager::PreparedEditorSceneRootChange SceneManager::prepareEditorSceneRootRegistration(
    const SceneGraph& base, const SceneGraph& candidate, uint32_t node) const {
  if (node != base.nodeCount() || candidate.nodeCount() != base.nodeCount()+1)
    throw std::invalid_argument("Editor scene membership requires exactly one new tail node");
  const auto* added = candidate.getNode(node);
  if (!added || added->renderable || added->sourceNodeIndex != -1 || added->sourceMeshIndex != -1 ||
      added->sourcePrimitiveIndex != -1 || added->sourceInstanceIndex != -1 || !added->children.empty() ||
      added->primitiveIndex != SceneGraph::kInvalidNode || added->meshOwnerGraphNode != SceneGraph::kInvalidNode)
    throw std::invalid_argument("Editor scene membership requires a source-unmapped empty leaf");
  if (!candidate.isNodeActive(node))
    throw std::invalid_argument("Editor scene membership candidate does not activate its new node");
  bool explicitRoot = true;
  if (added->parent != SceneGraph::kInvalidNode) {
    const auto* parent = base.getNode(added->parent);
    const bool emptyEditorContext = parent && parent->editorSceneRoot && !parent->interactionRootBoundary && !parent->renderable &&
        parent->sourceNodeIndex == -1 && base.activeRoots() && base.activeRoots()->empty();
    if (!parent || (!base.isNodeInActiveHierarchy(added->parent) && !emptyEditorContext))
      throw std::invalid_argument("Editor scene membership parent is outside the current hierarchy");
    explicitRoot = !base.isNodeActive(added->parent);
  }
  return prepareEditorSceneRootMembership(base,node,explicitRoot);
}

SceneManager::PreparedEditorSceneRootChange SceneManager::prepareEditorPrimitiveSceneRootRegistration(
    const SceneGraph& base,const SceneGraph& candidate,uint32_t node) const {
  constexpr SceneNodeAuthoringLimits bounds{};
  if(node!=base.nodeCount() || candidate.nodeCount()!=base.nodeCount()+1 ||
      candidate.nodeCount()>bounds.maxVisitedNodes)
    throw std::invalid_argument("Primitive scene membership requires exactly one bounded new tail node");
  const auto* added=candidate.getNode(node);
  if(!added || !added->renderable || added->retired || added->editorSceneRoot || added->interactionRootBoundary ||
      added->sourceNodeIndex!=-1 || added->sourceMeshIndex!=-1 || added->sourcePrimitiveIndex!=-1 ||
      added->sourceInstanceIndex!=-1 || added->meshOwnerGraphNode!=SceneGraph::kInvalidNode || !added->children.empty())
    throw std::invalid_argument("Primitive scene membership requires a source-unmapped renderable leaf");
  if(added->primitiveIndex>=model_.meshes().size() || model_.meshes()[added->primitiveIndex].empty() ||
      !materialIsActive(added->materialIndex))
    throw std::invalid_argument("Primitive scene membership references unavailable geometry or material");
  ValidateSceneNodeName(added->name,bounds);
  uint64_t references{};
  const auto charge=[&](uint64_t count) {
    if(count>bounds.maxVisitedEdges-references)
      throw std::invalid_argument("Primitive scene membership metadata exceeds its bound");
    references+=count;
  };
  for(uint32_t id=0;id<candidate.nodeCount();++id) charge(candidate.getNode(id)->children.size());
  if(base.activeRoots()) charge(base.activeRoots()->size());
  if(candidate.activeRoots()) charge(candidate.activeRoots()->size());
  for(const auto& roots:additionalSceneRoots_) charge(roots.size());
  if(selectedScene_<sceneRootOrder_.size()) charge(sceneRootOrder_[selectedScene_].size());
  for(const auto* matrix:{&added->localTransform,&added->worldTransform})
    for(int column=0;column<4;++column) for(int row=0;row<4;++row)
      if(!std::isfinite((*matrix)[column][row]))
        throw std::invalid_argument("Primitive scene membership transform is nonfinite");
  bool inherited=false;
  if(added->parent!=SceneGraph::kInvalidNode) {
    const auto parent=ResolveSceneNodeAuthoringTarget(base,added->parent,SceneNodeAuthoringPurpose::CreateEmptyChild,{},bounds);
    if(parent!=added->parent)
      throw std::invalid_argument("Primitive scene membership parent must be the authored target, not its synthetic primitive");
    const auto* context=base.getNode(parent);
    const bool emptyEditorContext=context->editorSceneRoot && !context->interactionRootBoundary && !context->renderable &&
        context->sourceNodeIndex==-1 && base.activeRoots() && base.activeRoots()->empty();
    if(!base.isNodeInActiveHierarchy(parent) && !emptyEditorContext)
      throw std::invalid_argument("Primitive scene membership parent is outside the current hierarchy");
    inherited=base.isNodeActive(parent);
  }
  auto expected=base.activeRoots();
  if(expected && !inherited) expected->push_back(node);
  if(candidate.activeRoots()!=expected || !candidate.isNodeActive(node))
    throw std::invalid_argument("Primitive scene membership candidate has an incorrect active view");
  if(candidate.renderableNodes().size()>config_.maxSceneObjects)
    throw std::invalid_argument("Primitive scene membership exceeds the active object capacity");
  // The additional entry is metadata ownership, not a demand for a redundant
  // active-view root. Owning it throughout the primitive's lifetime also keeps
  // later reparent/undo from manufacturing an unowned registration.
  return prepareEditorSceneRootMembership(base,node,true);
}

SceneManager::PreparedEditorSceneRootChange SceneManager::prepareEditorSceneRootMembership(
    const SceneGraph& base,uint32_t node,bool explicitRoot) const {
  if (selectedScene_ >= sceneCatalog_.entries.size() || additionalSceneRoots_.size() != sceneCatalog_.entries.size())
    throw std::invalid_argument("Editor scene membership catalog is not ready");
  // A retired old ID must never silently register a newly allocated node in a
  // different scene. New receipts remove their exact entry before retirement.
  for (const auto& roots : additionalSceneRoots_)
    if (std::ranges::find(roots, node) != roots.end())
      throw std::invalid_argument("Editor scene membership tail ID is already registered");
  EditorSceneRootMembership receipt;
  receipt.owner_ = this; receipt.ownerIdentity_ = sceneRootOwnerIdentity_;
  receipt.graphIdentity_ = base.identity(); receipt.catalogIdentity_ = sceneRootCatalogIdentity_;
  receipt.scene_ = selectedScene_; receipt.sceneCount_ = sceneCatalog_.entries.size();
  receipt.sourceNodeCount_ = sceneCatalog_.sourceNodeCount; receipt.node_ = node;
  receipt.insertionIndex_ = additionalSceneRoots_[selectedScene_].size(); receipt.explicitRoot_ = explicitRoot;
  return prepareEditorSceneRootChange(base, receipt, true);
}

SceneManager::PreparedEditorSceneRootChange SceneManager::prepareEditorSceneRootChange(
    const SceneGraph& graph, const EditorSceneRootMembership& receipt, bool present) const {
  if (!receipt || receipt.owner_ != this || receipt.ownerIdentity_ != sceneRootOwnerIdentity_ || receipt.graphIdentity_ != graph.identity() ||
      receipt.catalogIdentity_ != sceneRootCatalogIdentity_ || receipt.scene_ != selectedScene_ ||
      receipt.sceneCount_ != sceneCatalog_.entries.size() || receipt.sourceNodeCount_ != sceneCatalog_.sourceNodeCount ||
      additionalSceneRoots_.size() != sceneCatalog_.entries.size() || selectedScene_ >= additionalSceneRoots_.size() ||
      sceneRootMembershipRevision_ == UINT64_MAX || sceneRootCatalogIdentity_ == UINT64_MAX)
    throw std::invalid_argument("Editor scene membership receipt is stale or belongs to another owner/view");
  // These are pre-publication shape checks. Publication intentionally does not
  // repeat them after PreparedAppend/TailChange has replaced the live graph.
  if (present ? (graph.getNode(receipt.node_) || receipt.node_ != graph.nodeCount()) : !graph.getNode(receipt.node_))
    throw std::invalid_argument("Editor scene membership does not match the current tail state");
  PreparedEditorSceneRootChange prepared;
  prepared.state_ = std::make_unique<PreparedEditorSceneRootChange::State>();
  auto& state = *prepared.state_;
  state.owner = this; state.graph = &graph; state.receipt = receipt;
  state.generation = generationEpoch_; state.revision = sceneRootMembershipRevision_; state.catalog = sceneRootCatalogIdentity_;
  state.rootTableSize = additionalSceneRoots_.size(); state.orderTableSize = sceneRootOrder_.size();
  state.roots = additionalSceneRoots_[selectedScene_]; state.dirty = sceneRootsDirty_; state.present = present;
  state.hasOrder = selectedScene_ < sceneRootOrder_.size();
  if (state.hasOrder) state.order = sceneRootOrder_[selectedScene_];
  const auto found = std::ranges::find(state.roots, receipt.node_);
  if (receipt.explicitRoot_) {
    if (present) {
      if (found != state.roots.end() || receipt.insertionIndex_ > state.roots.size())
        throw std::invalid_argument("Editor scene membership cannot restore its exact root position");
      state.roots.insert(state.roots.begin()+receipt.insertionIndex_, receipt.node_);
    } else {
      if (found == state.roots.end() || std::count(state.roots.begin(), state.roots.end(), receipt.node_) != 1)
        throw std::invalid_argument("Editor scene membership no longer owns exactly one root entry");
      state.roots.erase(found);
    }
  } else if (found != state.roots.end()) {
    throw std::invalid_argument("Inherited editor node acquired an unrelated explicit root entry");
  }
  // Creation never owns an order hint. Drop its ID when retiring/restoring so
  // an old hint cannot reorder a later node that reuses this tail slot.
  std::erase(state.order, receipt.node_);
  return prepared;
}

bool SceneManager::canPublishEditorSceneRootChange(const PreparedEditorSceneRootChange& prepared) const noexcept {
  if (!prepared) return false;
  const auto& state = *prepared.state_; const auto& receipt = state.receipt;
  return state.owner == this && receipt.owner_ == this && receipt.ownerIdentity_ == sceneRootOwnerIdentity_ && state.graph &&
      state.graph->identity() == receipt.graphIdentity_ && state.generation == generationEpoch_ &&
      state.revision == sceneRootMembershipRevision_ && state.catalog == sceneRootCatalogIdentity_ &&
      state.revision != UINT64_MAX && state.catalog != UINT64_MAX && receipt.catalogIdentity_ == state.catalog &&
      receipt.scene_ == selectedScene_ && receipt.sceneCount_ == sceneCatalog_.entries.size() &&
      receipt.sourceNodeCount_ == sceneCatalog_.sourceNodeCount && state.rootTableSize == additionalSceneRoots_.size() &&
      receipt.scene_ < additionalSceneRoots_.size() && state.orderTableSize == sceneRootOrder_.size() &&
      state.hasOrder == (receipt.scene_ < sceneRootOrder_.size()) && state.dirty == sceneRootsDirty_ &&
      std::count(state.roots.begin(), state.roots.end(), receipt.node_) == int(state.present && receipt.explicitRoot_);
}

bool SceneManager::publishEditorSceneRootChange(PreparedEditorSceneRootChange& prepared) noexcept {
  if (!canPublishEditorSceneRootChange(prepared)) return false;
  auto& state = *prepared.state_;
  additionalSceneRoots_[state.receipt.scene_].swap(state.roots);
  if (state.hasOrder) sceneRootOrder_[state.receipt.scene_].swap(state.order);
  ++sceneRootMembershipRevision_;
  state.consumed = true;
  return true;
}

struct SceneManager::ReparentSceneRootMembership::State {
  const SceneManager* owner{};std::shared_ptr<const uint8_t> ownerIdentity;
  uint64_t graphIdentity{},catalogIdentity{};
  size_t scene{},sceneCount{},sourceCount{},nodeCount{},rootTableSize{},orderTableSize{};
  uint32_t node{};bool hasOrder{};
  std::array<std::vector<uint32_t>,2> roots,order;
  std::array<std::optional<std::vector<uint32_t>>,2> views;
};
struct SceneManager::PreparedReparentSceneRootChange::State {
  ReparentSceneRootMembership receipt;const SceneGraph* graph{};
  uint64_t generation{},revision{};bool dirty{},redo{},consumed{};
  std::vector<uint32_t> roots,order;
};
const std::optional<std::vector<uint32_t>>& SceneManager::ReparentSceneRootMembership::activeRoots(bool redo) const noexcept {
  static const std::optional<std::vector<uint32_t>> empty;
  return state_?state_->views[size_t(redo)]:empty;
}
SceneManager::PreparedReparentSceneRootChange::PreparedReparentSceneRootChange() noexcept=default;
SceneManager::PreparedReparentSceneRootChange::~PreparedReparentSceneRootChange()=default;
SceneManager::PreparedReparentSceneRootChange::PreparedReparentSceneRootChange(PreparedReparentSceneRootChange&&) noexcept=default;
SceneManager::PreparedReparentSceneRootChange& SceneManager::PreparedReparentSceneRootChange::operator=(PreparedReparentSceneRootChange&&) noexcept=default;
SceneManager::PreparedReparentSceneRootChange::operator bool() const noexcept {return state_ && !state_->consumed;}

namespace {
// Compute the same selected union/order as PlanGltfSceneActivation in linear
// forest order, avoiding one ancestry walk per possibly nested selected root.
std::vector<uint32_t> reparentRootView(const GltfSceneCatalog& catalog,size_t scene,const SceneGraph& graph,
    std::span<const uint32_t> additional,std::span<const uint32_t> order,uint32_t target,
    uint64_t maxObjects,bool& covered) {
  constexpr SceneNodeAuthoringLimits limits{};const auto count=graph.nodeCount();
  if(count>limits.maxVisitedNodes || catalog.sourceNodeCount>count || target>=count || scene>=catalog.entries.size())
    throw std::invalid_argument("Reparent scene membership graph/catalog exceeds its bound");
  uint64_t edges{};
  for(uint32_t id=0;id<count;++id) {
    const auto n=graph.getNode(id)->children.size();
    if(n>limits.maxVisitedEdges-edges) throw std::invalid_argument("Reparent scene membership edge budget exceeded");
    edges+=n;
  }
  if(graph.rootNodes().size()>limits.maxVisitedEdges || additional.size()>limits.maxVisitedEdges ||
      order.size()>limits.maxVisitedEdges || catalog.entries[scene].sourceRoots.size()>limits.maxVisitedEdges ||
      (graph.activeRoots() && graph.activeRoots()->size()>limits.maxVisitedEdges))
    throw std::invalid_argument("Reparent scene membership root metadata budget exceeded");
  std::vector<uint32_t> incoming(count),mapping(catalog.sourceNodeCount,SceneGraph::kInvalidNode),preorder;
  std::vector<uint8_t> visited(count),selected(count),active(count),top(count);preorder.reserve(count);
  for(uint32_t id=0;id<count;++id) {
    const auto& node=*graph.getNode(id);
    if(node.parent!=SceneGraph::kInvalidNode && node.parent>=count)
      throw std::invalid_argument("Reparent scene membership has an invalid parent");
    for(const auto child:node.children)
      if(child>=count || graph.getNode(child)->parent!=id || ++incoming[child]!=1)
        throw std::invalid_argument("Reparent scene membership has inconsistent child edges");
    if(node.sourceNodeIndex!=-1) {
      if(node.sourceNodeIndex<0 || size_t(node.sourceNodeIndex)>=mapping.size() || mapping[size_t(node.sourceNodeIndex)]!=SceneGraph::kInvalidNode)
        throw std::invalid_argument("Reparent scene membership source identity is invalid or repeated");
      mapping[size_t(node.sourceNodeIndex)]=id;
    }
  }
  auto pending=graph.rootNodes();
  for(const auto root:pending) if(root>=count || incoming[root] || graph.getNode(root)->parent!=SceneGraph::kInvalidNode)
    throw std::invalid_argument("Reparent scene membership has invalid global roots");
  while(!pending.empty()) {
    const auto id=pending.back();pending.pop_back();if(visited[id]++)
      throw std::invalid_argument("Reparent scene membership graph has a cycle or repeated root");
    preorder.push_back(id);const auto& children=graph.getNode(id)->children;
    pending.insert(pending.end(),children.begin(),children.end());
  }
  if(preorder.size()!=count || std::ranges::find(mapping,SceneGraph::kInvalidNode)!=mapping.end())
    throw std::invalid_argument("Reparent scene membership graph/source mapping is incomplete");
  std::vector<uint32_t> raw;
  const auto add=[&](uint32_t id) {
    if(id>=count || graph.isNodeRetired(id) || selected[id]) return;
    selected[id]=1;raw.push_back(id);
  };
  if(!catalog.authoredScenes && !catalog.sourceNodeCount) for(const auto root:graph.rootNodes()) add(root);
  else for(const auto source:catalog.entries[scene].sourceRoots) {
    if(source<0 || size_t(source)>=mapping.size()) throw std::invalid_argument("Reparent scene membership source root is invalid");
    add(mapping[size_t(source)]);
  }
  for(const auto root:additional) add(root); // Missing redo slots remain metadata, never live view roots.
  uint64_t objects{};
  for(const auto id:preorder) {
    const auto& node=*graph.getNode(id);const bool inherited=node.parent!=SceneGraph::kInvalidNode && active[node.parent];
    active[id]=!node.retired && (inherited || selected[id]);top[id]=selected[id] && !inherited;
    if(active[id] && node.renderable && ++objects>maxObjects)
      throw std::invalid_argument("Reparent scene membership exceeds the selected object capacity");
  }
  covered=active[target]!=0;
  std::vector<uint32_t> result;result.reserve(raw.size());
  for(const auto id:raw) if(top[id]) result.push_back(id);
  std::fill(selected.begin(),selected.end(),uint8_t{});for(const auto id:result) selected[id]=1;
  std::vector<uint32_t> ordered;ordered.reserve(result.size());
  for(const auto id:order) if(id<count && selected[id]) {ordered.push_back(id);selected[id]=0;}
  for(const auto id:result) if(selected[id]) {ordered.push_back(id);selected[id]=0;}
  return ordered;
}
}

SceneManager::ReparentSceneRootMembership SceneManager::planReparentSceneRoots(
    const SceneGraph& before,const SceneGraph& after,uint32_t node) const {
  constexpr SceneNodeAuthoringLimits limits{};
  if(before.nodeCount()!=after.nodeCount() || before.nodeCount()>limits.maxVisitedNodes ||
      !before.getNode(node) || before.isNodeRetired(node) || after.isNodeRetired(node) ||
      before.getNode(node)->editorSceneRoot || before.getNode(node)->interactionRootBoundary ||
      selectedScene_>=sceneCatalog_.entries.size() || additionalSceneRoots_.size()!=sceneCatalog_.entries.size() ||
      sceneRootMembershipRevision_==UINT64_MAX || sceneRootCatalogIdentity_==UINT64_MAX)
    throw std::invalid_argument("Reparent scene membership has an invalid owner/view/node");
  for(uint32_t id=0;id<before.nodeCount();++id) {
    const auto& a=*before.getNode(id);const auto& b=*after.getNode(id);
    if(a.sourceNodeIndex!=b.sourceNodeIndex || a.sourceMeshIndex!=b.sourceMeshIndex ||
        a.sourcePrimitiveIndex!=b.sourcePrimitiveIndex || a.sourceInstanceIndex!=b.sourceInstanceIndex ||
        a.meshOwnerGraphNode!=b.meshOwnerGraphNode || a.primitiveIndex!=b.primitiveIndex ||
        a.materialIndex!=b.materialIndex || a.renderable!=b.renderable || a.retired!=b.retired ||
        a.editorSceneRoot!=b.editorSceneRoot || a.interactionRootBoundary!=b.interactionRootBoundary ||
        a.name!=b.name || a.visible!=b.visible || a.selectable!=b.selectable || a.hoverable!=b.hoverable ||
        (id!=node && (a.parent!=b.parent || a.localTransform!=b.localTransform)))
      throw std::invalid_argument("Reparent scene membership prototype changed unrelated node payloads");
  }
  const bool hasOrder=selectedScene_<sceneRootOrder_.size();
  const std::span<const uint32_t> order=hasOrder?std::span<const uint32_t>(sceneRootOrder_[selectedScene_]):std::span<const uint32_t>{};
  bool wasActive{};const auto beforeView=reparentRootView(sceneCatalog_,selectedScene_,before,
      additionalSceneRoots_[selectedScene_],order,node,config_.maxSceneObjects,wasActive);
  if(before.activeRoots()) {
    // Stable deletion deliberately keeps retired IDs in the exact optional
    // view. They are inert, but their positions belong to the earlier command.
    std::vector<uint32_t> liveRoots;liveRoots.reserve(before.activeRoots()->size());
    for(const auto root:*before.activeRoots()) {
      if(!before.getNode(root)) throw std::invalid_argument("Reparent scene membership live view has a missing root");
      if(!before.isNodeRetired(root)) liveRoots.push_back(root);
    }
    if(liveRoots!=beforeView)
      throw std::invalid_argument("Reparent scene membership live view differs from its catalog");
  }
  auto state=std::make_shared<ReparentSceneRootMembership::State>();
  state->owner=this;state->ownerIdentity=sceneRootOwnerIdentity_;state->graphIdentity=before.identity();
  state->catalogIdentity=sceneRootCatalogIdentity_;state->scene=selectedScene_;state->sceneCount=sceneCatalog_.entries.size();
  state->sourceCount=sceneCatalog_.sourceNodeCount;state->nodeCount=before.nodeCount();state->node=node;
  state->rootTableSize=additionalSceneRoots_.size();state->orderTableSize=sceneRootOrder_.size();state->hasOrder=hasOrder;
  state->roots[0]=additionalSceneRoots_[selectedScene_];state->roots[1]=state->roots[0];
  if(hasOrder) state->order[0]=state->order[1]=sceneRootOrder_[selectedScene_];
  state->views[0]=before.activeRoots();
  bool afterCovered{};
  auto afterView=reparentRootView(sceneCatalog_,selectedScene_,after,state->roots[1],order,node,config_.maxSceneObjects,afterCovered);
  // Absent views activate the complete forest; the catalog still needs this
  // entry for a later explicit scene selection when moving outside its roots.
  if((wasActive || !before.activeRoots()) && !afterCovered) {
    if(state->roots[1].size()>=limits.maxVisitedEdges)
      throw std::invalid_argument("Reparent scene membership appended root exceeds its bound");
    state->roots[1].push_back(node);
    afterView=reparentRootView(sceneCatalog_,selectedScene_,after,state->roots[1],order,node,config_.maxSceneObjects,afterCovered);
  }
  if(before.activeRoots()) {
    const auto& oldRoots=*before.activeRoots();
    const auto inert=std::ranges::count_if(oldRoots,[&](uint32_t root) {return before.isNodeRetired(root);});
    if(uint64_t(inert)>limits.maxVisitedEdges-afterView.size())
      throw std::invalid_argument("Reparent scene membership retained view exceeds its bound");
    std::vector<uint32_t> exact;exact.reserve(afterView.size()+size_t(inert));size_t live{};
    for(size_t position=0;position<oldRoots.size();++position) if(before.isNodeRetired(oldRoots[position])) {
      while(exact.size()<position && live<afterView.size()) exact.push_back(afterView[live++]);
      exact.push_back(oldRoots[position]);
    }
    exact.insert(exact.end(),afterView.begin()+live,afterView.end());state->views[1]=std::move(exact);
  }
  ReparentSceneRootMembership receipt;receipt.state_=std::move(state);return receipt;
}

SceneManager::PreparedReparentSceneRootChange SceneManager::prepareReparentSceneRootChange(
    const SceneGraph& graph,const ReparentSceneRootMembership& receipt,bool redo) const {
  if(!receipt) throw std::invalid_argument("Reparent scene membership receipt is absent");
  const auto& original=*receipt.state_;const auto expected=size_t(!redo);
  if(original.owner!=this || original.ownerIdentity!=sceneRootOwnerIdentity_ || original.graphIdentity!=graph.identity() ||
      original.catalogIdentity!=sceneRootCatalogIdentity_ || original.scene!=selectedScene_ ||
      original.sceneCount!=sceneCatalog_.entries.size() || original.sourceCount!=sceneCatalog_.sourceNodeCount ||
      original.nodeCount!=graph.nodeCount() || !graph.getNode(original.node) || graph.isNodeRetired(original.node) ||
      original.rootTableSize!=additionalSceneRoots_.size() || original.orderTableSize!=sceneRootOrder_.size() ||
      selectedScene_>=additionalSceneRoots_.size() || original.hasOrder!=(selectedScene_<sceneRootOrder_.size()) ||
      additionalSceneRoots_[selectedScene_]!=original.roots[expected] ||
      (original.hasOrder && sceneRootOrder_[selectedScene_]!=original.order[expected]) ||
      graph.activeRoots()!=original.views[expected] || sceneRootMembershipRevision_==UINT64_MAX || sceneRootCatalogIdentity_==UINT64_MAX)
    throw std::invalid_argument("Reparent scene membership receipt does not match its opposite side");
  PreparedReparentSceneRootChange prepared;prepared.state_=std::make_unique<PreparedReparentSceneRootChange::State>();
  auto& state=*prepared.state_;state.receipt=receipt;state.graph=&graph;state.generation=generationEpoch_;
  state.revision=sceneRootMembershipRevision_;state.dirty=sceneRootsDirty_;state.redo=redo;
  state.roots=original.roots[size_t(redo)];state.order=original.order[size_t(redo)];return prepared;
}
bool SceneManager::canPublishReparentSceneRootChange(const PreparedReparentSceneRootChange& prepared) const noexcept {
  if(!prepared || !prepared.state_->receipt) return false;
  const auto& state=*prepared.state_;const auto& original=*state.receipt.state_;const auto expected=size_t(!state.redo);
  return original.owner==this && original.ownerIdentity==sceneRootOwnerIdentity_ && state.graph &&
      original.graphIdentity==state.graph->identity() && original.catalogIdentity==sceneRootCatalogIdentity_ &&
      state.generation==generationEpoch_ && state.revision==sceneRootMembershipRevision_ && state.revision!=UINT64_MAX &&
      original.scene==selectedScene_ && original.sceneCount==sceneCatalog_.entries.size() &&
      original.sourceCount==sceneCatalog_.sourceNodeCount && original.rootTableSize==additionalSceneRoots_.size() &&
      original.orderTableSize==sceneRootOrder_.size() && selectedScene_<additionalSceneRoots_.size() &&
      original.hasOrder==(selectedScene_<sceneRootOrder_.size()) && state.dirty==sceneRootsDirty_ &&
      additionalSceneRoots_[selectedScene_]==original.roots[expected] &&
      (!original.hasOrder || sceneRootOrder_[selectedScene_]==original.order[expected]);
}
bool SceneManager::publishReparentSceneRootChange(PreparedReparentSceneRootChange& prepared) noexcept {
  if(!canPublishReparentSceneRootChange(prepared)) return false;
  auto& state=*prepared.state_;additionalSceneRoots_[selectedScene_].swap(state.roots);
  if(state.receipt.state_->hasOrder) sceneRootOrder_[selectedScene_].swap(state.order);
  sceneRootsDirty_=false;++sceneRootMembershipRevision_;state.consumed=true;return true;
}

void SceneManager::applySceneSelection(SceneGraph& graph,const GltfSceneActivation& activation) {
  // Revalidate against current topology/capacity, before mutating either view.
  const auto checked=planSceneSelection(graph,activation.scene);
  graph.setActiveRoots(checked.runtimeRoots);
  if (sceneRootMembershipRevision_ < UINT64_MAX) ++sceneRootMembershipRevision_;
  selectedScene_=checked.scene;
  if(!gltfModel_.scenes.empty()) gltfModel_.defaultScene=int(selectedScene_);
  refreshActiveScene(graph);
}

void SceneManager::registerActiveSceneNode(SceneGraph& graph,uint32_t node) {
  if(!graph.getNode(node)) throw std::invalid_argument("New scene node is missing");
  const auto authored = planSceneSelection(graph, selectedScene_);
  std::vector<uint8_t> rootMask(graph.nodeCount(), 0);
  for (const auto root : authored.runtimeRoots) rootMask[root] = 1;
  size_t steps = 0;
  for (auto ancestor = node; ancestor != SceneGraph::kInvalidNode;) {
    if (ancestor >= rootMask.size() || steps++ >= graph.nodeCount())
      throw std::invalid_argument("New scene node ancestry is invalid");
    if (rootMask[ancestor]) return;
    ancestor = graph.getNode(ancestor)->parent;
  }
  auto& roots=additionalSceneRoots_.at(selectedScene_);
  roots.push_back(node);
  if (sceneRootMembershipRevision_ < UINT64_MAX) ++sceneRootMembershipRevision_;
  try { applySceneSelection(graph,planSceneSelection(graph,selectedScene_)); }
  catch(...) { roots.pop_back();throw; }
}

void SceneManager::preserveActiveSceneMembership(uint32_t node) {
  auto& roots=additionalSceneRoots_.at(selectedScene_);
  if(std::find(roots.begin(),roots.end(),node)==roots.end()) {
    roots.push_back(node); sceneRootsDirty_=true;
    if (sceneRootMembershipRevision_ < UINT64_MAX) ++sceneRootMembershipRevision_;
  }
}

void SceneManager::reconcileActiveSceneRoots(SceneGraph& graph) {
  if(!sceneRootsDirty_) return;
  applySceneSelection(graph,planSceneSelection(graph,selectedScene_));
  sceneRootsDirty_=false;
}

bool SceneManager::isAuthoredSceneRoot(const SceneGraph& graph,uint32_t node) const {
  const auto* runtime=graph.getNode(node);
  if(!runtime || graph.isNodeRetired(node) || runtime->sourceNodeIndex<0) return false;
  for(const auto& entry:sceneCatalog_.entries)
    if(std::find(entry.sourceRoots.begin(),entry.sourceRoots.end(),runtime->sourceNodeIndex)!=entry.sourceRoots.end())
      return true;
  return false;
}

void SceneManager::refreshActiveScene(const SceneGraph& graph) {
  updateModelBounds(&graph);
  collectAuthoredPunctualLights(&graph);
}

uint32_t SceneManager::appendRuntimeMesh(container::geometry::Mesh mesh) {
  ++geometryRevision_;
  if (mesh.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  auto mapping = MakeRuntimePrimitiveMapping(sourcePrimitiveCount(gltfModel_),model_.meshes().size(),
                                             runtimePrimitiveToSource_);
  if (mapping.sourceToRuntime != sourcePrimitiveToRuntime_)
    throw std::runtime_error("Source/runtime primitive maps disagree");
  if (model_.meshes().size() >= size_t(std::numeric_limits<uint32_t>::max()) - 1)
    throw std::overflow_error("Runtime primitive table exceeds its identity range");
  mapping.runtimeToSource.push_back(-1);
  std::vector<container::geometry::Mesh> meshes = model_.meshes();
  const uint32_t primitiveIndex = static_cast<uint32_t>(meshes.size());
  std::optional<container::geometry::Model> originalModel;
  if (variantOriginalModel_) {
    if (variantOriginalModel_->meshes().size()!=meshes.size())
      throw std::runtime_error("Variant source/runtime primitive tables disagree");
    auto original=variantOriginalModel_->meshes(); original.push_back(mesh);
    originalModel=container::geometry::Model::FromMeshes(std::move(original));
  }
  meshes.push_back(std::move(mesh));
  auto model = container::geometry::Model::FromMeshes(std::move(meshes));
  auto vertices = model.vertices();
  auto indices = model.indices();
  model_ = std::move(model);
  variantOriginalModel_ = std::move(originalModel);
  runtimePrimitiveToSource_ = std::move(mapping.runtimeToSource);
  vertices_ = std::move(vertices);
  indices_ = std::move(indices);
  // A new geometry slot has no instance yet. Keep the published graph bounds
  // until its caller commits the node and refreshes that graph's active view;
  // source-only traversal would resurrect hidden/retired authored instances.
  indexType_ = VK_INDEX_TYPE_UINT32;
  return primitiveIndex;
}

std::optional<uint32_t> SceneManager::retainedPrimitiveNormalTexCoord(uint32_t runtime) const noexcept {
  if(runtime>=runtimePrimitiveToSource_.size() || runtime>=model_.meshes().size()) return std::nullopt;
  const auto source=runtimePrimitiveToSource_[runtime];
  if(source<0 || size_t(source)>=sourcePrimitiveToRuntime_.size() || sourcePrimitiveToRuntime_[size_t(source)]!=runtime ||
      variantNormalTexCoords_.size()!=sourcePrimitiveToRuntime_.size() ||
      materialVariants_.primitives().size()!=sourcePrimitiveToRuntime_.size()) return std::nullopt;
  const auto& identity=materialVariants_.primitives()[size_t(source)];
  if(identity.sourceMesh>=gltfModel_.meshes.size() || identity.sourcePrimitive>=gltfModel_.meshes[identity.sourceMesh].primitives.size()) return std::nullopt;
  uint32_t uv=variantNormalTexCoords_[size_t(source)];
  if(deformation_) {
    if(deformation_->primitives.size()!=sourcePrimitiveToRuntime_.size()) return std::nullopt;
    const auto& primitive=deformation_->primitives[size_t(source)];
    if(primitive.sourceMesh!=identity.sourceMesh || primitive.sourcePrimitive!=identity.sourcePrimitive || primitive.normalTextureTexCoord!=uv)
      return std::nullopt;
    uv=primitive.normalTextureTexCoord;
  }
  if(uv>1) return std::nullopt;
  const std::string_view semantic=uv?"TEXCOORD_1":"TEXCOORD_0";
  for(const auto& [name,accessor]:gltfModel_.meshes[identity.sourceMesh].primitives[identity.sourcePrimitive].attributes) {
    if(std::string_view(name)!=semantic) continue;
    if(accessor<0 || size_t(accessor)>=gltfModel_.accessors.size()) return std::nullopt;
    const auto& attribute=gltfModel_.accessors[size_t(accessor)];
    if(attribute.type!=TINYGLTF_TYPE_VEC2 || attribute.count!=model_.meshes()[runtime].vertices().size()) return std::nullopt;
    return uv;
  }
  return std::nullopt;
}

container::geometry::GltfLoadResult SceneManager::prepareGltfModel(
    const std::string& path, uintmax_t maxInputBytes) {
  auto resolvedPath = container::util::pathFromUtf8(path);
  if (resolvedPath.is_relative() && !std::filesystem::exists(resolvedPath)) {
    resolvedPath = resolveSceneAssetPath(path);
  }
  container::geometry::gltf::LoadOptions options;
  options.maxInputBytes = maxInputBytes;
  return prepareSceneGltfAsset(resolvedPath, std::move(options));
}

bool SceneManager::reloadModel(
    const std::string& path,
    float importScale,
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer,
    container::geometry::GltfLoadResult* prepared) {
  const std::string previousModelPath = config_.modelPath;
  const float previousImportScale = config_.importScale;
  // Decode and check the incoming sampler budget before retiring live assets.
  // Rejected loads must leave runtime edits and auxiliary registrations intact.
  std::optional<container::geometry::GltfLoadResult> locallyPrepared;
  std::optional<container::material::PreparedGltfTextures> incomingTextures;
  try {
    std::vector<container::material::MaterialSamplerKey> incomingSamplerKeys;
    if (isDefaultSceneRequest(path)) {
      for (const auto entry : container::app::kDefaultSceneModelRelativePaths) {
        if (isProceduralSphereEntry(entry)) continue;
        const auto incoming = prepareDefaultSceneGltfAsset(resolveSceneAssetPath(entry));
        (void)container::material::PrepareGltfTextures(incoming.gltfModel,
            allocationManager_->textureDecodeOptions(false));
        const auto keys = container::material::CollectGltfMaterialSamplerKeys(incoming.gltfModel);
        incomingSamplerKeys.insert(incomingSamplerKeys.end(), keys.begin(), keys.end());
      }
    } else if (!path.empty()) {
      if (prepared == nullptr) {
        locallyPrepared = prepareGltfModel(path);
        prepared = &*locallyPrepared;
      }
      incomingSamplerKeys = container::material::CollectGltfMaterialSamplerKeys(prepared->gltfModel);
      (void)container::geometry::gltf::ParseMaterialVariants(prepared->gltfModel);
      const std::vector<uint32_t> preflightMaterials(prepared->model.primitiveRanges().size(),0);
      (void)prepareCompleteHierarchy(prepared->gltfModel,prepared->instancing,preflightMaterials,
          importScaleTransform(importScale),1,prepared->model.meshes().size(),config_.maxSceneObjects,config_.gltfSceneIndex);
      incomingTextures = container::material::PrepareGltfTextures(prepared->gltfModel,
          allocationManager_->textureDecodeOptions(false));
      if(container::material::CountPreparedGltfTextureResources(*incomingTextures)>textureDescriptorCapacity_)
        throw std::runtime_error("Incoming glTF textures exceed the scene image descriptor capacity");
    }
    // Replacement uses a fresh table (plus the default policy), rather than
    // charging incoming assets for policies belonging only to the old scene.
    (void)container::material::PlanMaterialSamplerSlots(
        incomingSamplerKeys, materialSamplerDescriptorCapacity_);
  } catch (const std::exception& error) {
    std::println(stderr, "glTF load failed: {}", error.what());
    return false;
  }
  auto resetForLoad = [this]() {
    resetLoadedAssets();
    loadMaterialXMaterial();
    vertices_.clear();
    indices_.clear();
    gltfModel_ = tinygltf::Model{};
    model_ = container::geometry::Model{};
  };

  try {
    resetForLoad();
    config_.modelPath = path;
    config_.importScale = sanitizeImportScale(importScale);
    loadGltfAssets(prepared, incomingTextures ? &*incomingTextures : nullptr);
    resetCameraProjectionNamespace();
    resetMaterialLifecycle();
    uploadMaterialBuffer();
    uploadTextureMetadataBuffer();
    updateDescriptorSets(cameraBuffers, objectBuffer);
    finishTextureImageOwnershipPublication();
    return true;
  } catch (...) {
    try {
      resetForLoad();
      config_.modelPath = previousModelPath;
      config_.importScale = previousImportScale;
      loadGltfAssets();
      resetCameraProjectionNamespace();
      resetMaterialLifecycle();
      uploadMaterialBuffer();
      uploadTextureMetadataBuffer();
      updateDescriptorSets(cameraBuffers, objectBuffer);
      finishTextureImageOwnershipPublication();
    } catch (const std::exception& e) {
      std::println(stderr, "failed to restore previous model '{}': {}",
                   previousModelPath, e.what());
      resetForLoad();
      resetCameraProjectionNamespace();
      resetMaterialLifecycle();
      uploadMaterialBuffer();
      uploadTextureMetadataBuffer();
      updateDescriptorSets(cameraBuffers, objectBuffer);
      finishTextureImageOwnershipPublication();
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
  return createMaterial(material);
}

void SceneManager::uploadMaterialResources() {
  uploadMaterialBuffer();
  uploadTextureMetadataBuffer();
  finishTextureImageOwnershipPublication();
}

void SceneManager::validateGltfTextureResourceTable(
    const container::material::GltfTextureResources& roles,size_t sourceTextureCount,
    size_t runtimeResourceCount) {
  if(roles.color.size()!=sourceTextureCount || roles.data.size()!=sourceTextureCount)
    throw std::runtime_error("glTF texture role table differs from the source texture identity space");
  for(const auto* role:{&roles.color,&roles.data}) for(const auto resource:*role)
    if(resource!=UINT32_MAX && resource>=runtimeResourceCount)
      throw std::runtime_error("glTF texture role references an unavailable runtime resource");
}

void SceneManager::rememberExportMaterials(
    const tinygltf::Model& source, const std::filesystem::path& sourcePath,
    const container::material::GltfTextureResources& textureToResource,
    std::span<const uint32_t> sourceMaterialToRuntime) {
  validateGltfTextureResourceTable(textureToResource,source.textures.size(),textureManager_.textureCount());
  ValidateSourceMaterialMapping(sourceMaterialToRuntime, source.materials.size(),
                               materialManager_.materialCount());
  validateMaterialResourceSource(source);
  const auto textureBase = static_cast<int32_t>(exportSource_.textures.size());
  const bool sameEvaluatedSource=&source==&gltfModel_;
  const auto evaluatedBase=sameEvaluatedSource?0u:uint32_t(gltfModel_.textures.size());
  if(source.textures.size()>1'000'000 || exportSource_.textures.size()>1'000'000-source.textures.size() ||
      (!sameEvaluatedSource && gltfModel_.textures.size()>1'000'000-source.textures.size()))
    throw std::length_error("Imported material source identities exceed their count limit");
  std::vector<tinygltf::Material> evaluatedDefinitions;
  if(!sameEvaluatedSource)
    evaluatedDefinitions=container::geometry::gltf::appendExportResources(gltfModel_,source,sourcePath);
  auto definitions = container::geometry::gltf::appendExportResources(
      exportSource_, source, sourcePath);
  archiveTextureResources_.color.resize(exportSource_.textures.size(),UINT32_MAX);
  archiveTextureResources_.data.resize(exportSource_.textures.size(),UINT32_MAX);
  archiveTextureToEvaluated_.resize(exportSource_.textures.size(),-1);
  archiveTextureIdentities_.resize(exportSource_.textures.size());
  gltfTextureResources_.color.resize(gltfModel_.textures.size(),UINT32_MAX);
  gltfTextureResources_.data.resize(gltfModel_.textures.size(),UINT32_MAX);
  for(size_t i=0;i<source.textures.size();++i) {
    const auto archive=size_t(textureBase)+i,evaluated=size_t(evaluatedBase)+i;
    archiveTextureResources_.color[archive]=textureToResource.color[i];
    archiveTextureResources_.data[archive]=textureToResource.data[i];
    gltfTextureResources_.color[evaluated]=textureToResource.color[i];
    gltfTextureResources_.data[evaluated]=textureToResource.data[i];
    archiveTextureToEvaluated_[archive]=int32_t(evaluated);
    archiveTextureIdentities_[archive]=std::make_shared<const uint8_t>(0);
  }
  exportMaterialSources_.resize(std::max(exportMaterialSources_.size(),
                                        materialManager_.materialCount()));
  exportMaterialTextureBindings_.resize(exportMaterialSources_.size());
  for (size_t i = 0; i < definitions.size(); ++i) {
    const uint32_t runtime = sourceMaterialToRuntime[i];
    exportMaterialSources_[runtime] = std::move(definitions[i]);
    const auto* material = materialManager_.getMaterial(runtime);
    if (!material) throw std::runtime_error("Cannot retain imported material texture bindings");
    exportMaterialTextureBindings_[runtime] =
        container::geometry::gltf::snapshotMaterialTextureBindings(*material);
    rememberRuntimeMaterialProvenance(runtime,*exportMaterialSources_[runtime],
        sameEvaluatedSource?source.materials[i]:evaluatedDefinitions[i]);
  }
  exportTextureIndices_.resize(textureManager_.textureCount(), -1);
  for (const auto* role : {&textureToResource.color, &textureToResource.data})
    for (size_t i = 0; i < role->size(); ++i)
      if ((*role)[i] < exportTextureIndices_.size())
        exportTextureIndices_[(*role)[i]] = textureBase + static_cast<int32_t>(i);
}

void SceneManager::saveScene(const SceneGraph& graph,
                             const std::filesystem::path& destination) const {
  auto output=buildSceneExport(graph);
  const auto retired=retiredSceneExportNodes(graph);
  if(!retired.empty()) output=container::geometry::gltf::CompactGltfExportNodes(output,retired).model;
  compactRetiredMaterialExport(output);
  container::geometry::gltf::writeExportModel(output, destination);
}

void SceneManager::compactRetiredMaterialExport(tinygltf::Model& detached) const {
  const auto retired=retiredMaterialIndices();
  if(retired.empty()) return;
  // buildSceneExport has already projected material references into runtime
  // IDs. Live slots stay stable; only this unpublished output becomes dense.
  const auto plan=container::geometry::gltf::PlanGltfMaterialExportCompaction(detached,retired);
  container::geometry::gltf::ApplyGltfMaterialExportCompaction(detached,plan);
}

std::vector<int32_t> SceneManager::retiredSceneExportNodes(const SceneGraph& graph) const {
  if(graph.nodeCount()>size_t(INT32_MAX))
    throw std::runtime_error("Scene save node identities exceed their integer range");
  std::vector<int32_t> retired;
  for(uint32_t id=0;id<graph.nodeCount();++id)
    if(graph.isNodeRetired(id)) retired.push_back(static_cast<int32_t>(id));
  return retired;
}

float SceneManager::importScale() const {
  return sanitizeImportScale(config_.importScale);
}

tinygltf::Model SceneManager::buildSceneExport(const SceneGraph& graph) const {
  auto source = exportSource_;
  auto phases = archiveResourcePhases_;
  auto textureMapping = exportTextureIndices_;
  std::map<std::tuple<int32_t, uint32_t, uint32_t>, uint32_t> repackedTextures;
  uint64_t repackPixelBudget = 256ull * 1024 * 1024;
  const auto sourceMaterials = ExportSourceMaterialMapping(sourceMaterialToRuntime_,
      gltfModel_.materials.size(), materialManager_.materialCount());
  std::vector<tinygltf::Material> materials(materialManager_.materialCount());
  std::vector<uint8_t> materialActive(materials.size(),1);
  for(uint32_t runtime=0;runtime<materials.size();++runtime)
    materialActive[runtime]=materialIsActive(runtime)?1:0;
  std::vector<int32_t> runtimeToSource(materials.size(),-1);
  for(size_t source=0;source<sourceMaterials.size();++source) {
    const auto runtime=sourceMaterials[source];
    if(runtime<0 || size_t(runtime)>=runtimeToSource.size())
      throw std::runtime_error("Scene save material source identity is unavailable");
    auto& inverse=runtimeToSource[size_t(runtime)];
    inverse=inverse==-1?int32_t(source):-2;
  }
  for (size_t i = 0; i < materials.size() && i < exportMaterialSources_.size(); ++i)
    if (materialIsActive(uint32_t(i)) && exportMaterialSources_[i]) materials[i] = *exportMaterialSources_[i];
  // Unused provider materials can contain authoring-format-specific inputs.
  // Only materials actually referenced by this primary scene need conversion.
  std::vector<bool> converted(materials.size(), false);
  const auto convert=[&](uint32_t materialIndex) {
    if (materialIndex >= materials.size())
      throw std::runtime_error("Scene save references an invalid material");
    if (!materialIsActive(materialIndex))
      throw std::runtime_error("Scene save references a retired material");
    if (converted[materialIndex]) return;
    const auto* material = materialManager_.getMaterial(materialIndex);
    if (!material) throw std::runtime_error("Scene save references an unavailable material");
    const auto* original = materialIndex < exportMaterialSources_.size() && exportMaterialSources_[materialIndex]
                               ? &*exportMaterialSources_[materialIndex] : nullptr;
    const auto* bindings = materialIndex < exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[materialIndex]
                               ? &*exportMaterialTextureBindings_[materialIndex] : nullptr;
    if(material->specularGlossinessWorkflow && runtimeToSource[materialIndex]==-2)
      throw std::runtime_error("Scene save SG material has ambiguous source identities");
    if(material->specularGlossinessWorkflow && runtimeToSource[materialIndex]>=0) {
      const auto source=size_t(runtimeToSource[materialIndex]);
      if(!hasRuntimeMaterialProvenance(materialIndex) || source>=exportSource_.materials.size() || source>=gltfModel_.materials.size())
        throw std::runtime_error("Scene save cannot prove the retained SG material source/resource identities");
      materials[materialIndex]=container::material::ExportSourceBackedGltfMaterial(*material,exportSource_.materials[source],gltfModel_.materials[source],
          {exportSource_.textures.size(),exportSource_.extensionsUsed,archiveTextureResources_.color,archiveTextureResources_.data,textureManager_.textureCount()},
          {gltfModel_.textures.size(),gltfModel_.extensionsUsed,gltfTextureResources_.color,gltfTextureResources_.data,textureManager_.textureCount()},
          archiveTextureToEvaluated_);
    } else {
      using namespace container::geometry::gltf;
      using Slot = GltfMaterialTextureSlot;
      using Material = container::material::Material;
      struct ScalarSlot {
        Slot slot;
        uint32_t Material::* texture;
        container::material::TextureTransform Material::* transform;
        uint32_t channel;
      };
      constexpr std::array scalarSlots{
          ScalarSlot{Slot::Specular, &Material::specularTextureIndex, &Material::specularTextureTransform, 3},
          ScalarSlot{Slot::Transmission, &Material::transmissionTextureIndex, &Material::transmissionTextureTransform, 0},
          ScalarSlot{Slot::Clearcoat, &Material::clearcoatTextureIndex, &Material::clearcoatTextureTransform, 0},
          ScalarSlot{Slot::ClearcoatRoughness, &Material::clearcoatRoughnessTextureIndex, &Material::clearcoatRoughnessTextureTransform, 1},
          ScalarSlot{Slot::Thickness, &Material::thicknessTextureIndex, &Material::thicknessTextureTransform, 1},
          ScalarSlot{Slot::SheenRoughness, &Material::sheenRoughnessTextureIndex, &Material::sheenRoughnessTextureTransform, 3},
          ScalarSlot{Slot::Iridescence, &Material::iridescenceTextureIndex, &Material::iridescenceTextureTransform, 0},
          ScalarSlot{Slot::IridescenceThickness, &Material::iridescenceThicknessTextureIndex, &Material::iridescenceThicknessTextureTransform, 1},
          ScalarSlot{Slot::DiffuseTransmission, &Material::diffuseTransmissionTextureIndex, &Material::diffuseTransmissionTextureTransform, 3},
      };
      std::optional<Material> detached;
      for (const auto& slot : scalarSlots) {
        const auto resource = material->*slot.texture;
        const auto channel = (material->*slot.transform).channel;
        if (resource == UINT32_MAX || channel > 3 || channel == slot.channel)
          continue;
        const auto binding = original ? ReadGltfMaterialTextureBinding(*original, slot.slot)
                                      : GltfMaterialTextureBinding{};
        if (binding.legacy)
          continue;
        const auto& data = archiveTextureResources_.data;
        const auto matches = [&](int32_t texture) {
          return texture >= 0 && size_t(texture) < data.size() && data[size_t(texture)] == resource;
        };
        int32_t texture = binding.index;
        if (!matches(texture)) {
          texture = resource < exportTextureIndices_.size() ? exportTextureIndices_[resource] : -1;
          if (!matches(texture))
            throw std::runtime_error("Scene save cannot prove the scalar texture's retained data source");
        }
        const auto key = std::tuple{texture, channel, slot.channel};
        auto packed = repackedTextures.find(key);
        if (packed == repackedTextures.end()) {
          const auto selected = SelectTextureImageSource(source, texture);
          const int sampler = source.textures[size_t(texture)].sampler;
          const auto active = [](const auto& mask, int index) {
            return index >= 0 && (mask.empty() || (size_t(index) < mask.size() && mask[size_t(index)]));
          };
          if (!active(phases.textures, texture) || !active(phases.images, selected.imageIndex) ||
              (sampler >= 0 && !active(phases.samplers, sampler)) || textureMapping.size() >= UINT32_MAX)
            throw std::runtime_error("Scene save cannot repack a retired or out-of-range scalar resource");
          const auto synthetic = uint32_t(textureMapping.size());
          textureMapping.push_back(appendRepackedScalarTexture(source, texture, channel, slot.channel, repackPixelBudget));
          packed = repackedTextures.emplace(key, synthetic).first;
        }
        if (!detached)
          detached.emplace(*material);
        (*detached).*slot.texture = packed->second;
        ((*detached).*slot.transform).channel = slot.channel;
      }
      materials[materialIndex] = exportMaterial(detached ? *detached : *material,
                                                textureMapping, original, bindings);
    }
    converted[materialIndex] = true;
  };
  for (uint32_t index=0;index<graph.nodeCount();++index)
    if(!graph.isNodeRetired(index) && graph.getNode(index)->renderable) convert(graph.getNode(index)->materialIndex);
  // Alternate materials can retain edits made before switching away.
  if (materialVariants_.hasVariants())
    for (const auto runtime : sourceMaterialToRuntime_)
      if(materialIsActive(runtime)) convert(runtime);
  std::vector<int32_t> sourceNodes;
  sourceNodes.reserve(graph.nodeCount());
  for (uint32_t i = 0; i < graph.nodeCount(); ++i)
    sourceNodes.push_back(graph.getNode(i)->sourceNodeIndex);
  // Keep archived binary payloads, but save the evaluated source pose. The
  // exporter uses it to separate current editor transforms from animated TRS.
  phases.resize(source.images.size(), source.textures.size(), source.samplers.size());
  source.nodes = gltfModel_.nodes;
  source.lights = gltfModel_.lights;
  source.cameras = gltfModel_.cameras;
  std::vector<std::vector<uint32_t>> roots;
  roots.reserve(sceneCatalog_.entries.size());
  for(size_t i=0;i<sceneCatalog_.entries.size();++i)
    roots.push_back(planSceneSelection(graph,i,kMaxAssetRenderableObjects).runtimeRoots);
  return container::geometry::gltf::buildExportModel(
      {graph, model_, &source, sourcePath_, sourceNodes, materials, sourceMaterials,
       &exportSource_,roots,int32_t(selectedScene_),runtimePrimitiveToSource_,
       {phases.images,phases.textures,phases.samplers,materialActive}});
}

MaterialVariantGraphEdit SceneManager::planMaterialVariant(const SceneGraph& graph,std::optional<size_t> variant) const {
  const auto mapping = ExportSourceMaterialMapping(sourceMaterialToRuntime_,
      gltfModel_.materials.size(), materialManager_.materialCount());
  const auto implicitDefault = ResolveSourceMaterialIndex(-1, sourceMaterialToRuntime_,
      gltfModel_.materials.size(), materialManager_.materialCount(), defaultMaterialIndex_);
  auto plan=container::geometry::gltf::planMaterialVariantSelection(materialVariants_,variant,mapping,int32_t(implicitDefault));
  for(size_t source=0;source<plan.assignments.size();++source)
    if(plan.assignments[source].sourceMaterial==-1)
      plan.assignments[source].runtimeMaterial=static_cast<int32_t>(resolvePrimitiveMaterialIndex(source,-1));
  auto edit=PlanMaterialVariantGraphEdit(graph,plan,selectedMaterialVariant_);
  std::erase_if(edit.assignments,[&](const auto& assignment) { return graph.isNodeRetired(assignment.node); });
  for (const auto& assignment:edit.assignments)
    if (!materialIsActive(assignment.after) || !materialIsActive(assignment.before))
      throw std::runtime_error("Material variant references an unavailable runtime material");
  return edit;
}
bool SceneManager::applyMaterialVariant(SceneGraph& graph,const MaterialVariantGraphEdit& edit,bool forward) {
  if (graph.identity()!=edit.graphIdentity) return false;
  for (const auto& assignment:edit.assignments)
    if (!graph.getNode(assignment.node) || graph.isNodeRetired(assignment.node) ||
        !materialIsActive(forward?assignment.after:assignment.before)) return false;
  auto texCoords=variantNormalTexCoords_;
  const auto sourceCount=sourcePrimitiveCount(gltfModel_);
  if (variantOriginalModel_) {
    ValidateSourcePrimitiveIds(sourcePrimitiveToRuntime_,sourceCount,model_.meshes().size());
    if (variantOriginalModel_->meshes().size()!=model_.meshes().size() || texCoords.size()!=sourceCount)
      throw std::runtime_error("Variant source/runtime primitive tables disagree");
  }
  std::map<std::pair<size_t,size_t>,size_t> primitiveIndices;
  for (size_t i=0;i<materialVariants_.primitives().size();++i) {
    const auto& primitive=materialVariants_.primitives()[i];
    primitiveIndices.emplace(std::pair{primitive.sourceMesh,primitive.sourcePrimitive},i);
  }
  for (const auto& assignment:edit.assignments) {
    if (!variantOriginalModel_) break;
    const auto& node=*graph.getNode(assignment.node);
    const auto found=primitiveIndices.find({size_t(node.sourceMeshIndex),size_t(node.sourcePrimitiveIndex)});
    if (found==primitiveIndices.end() || found->second>=sourcePrimitiveToRuntime_.size() ||
        sourcePrimitiveToRuntime_[found->second]!=node.primitiveIndex) return false;
    const auto index=found->second;
    const auto& source=gltfModel_.meshes[size_t(node.sourceMeshIndex)].primitives[size_t(node.sourcePrimitiveIndex)];
    if (source.attributes.contains("NORMAL") && source.attributes.contains("TANGENT")) continue;
    const auto& material=*materialManager_.getMaterial(forward?assignment.after:assignment.before);
    texCoords[index]=material.normalTextureIndex==std::numeric_limits<uint32_t>::max()?0:material.normalTextureTransform.texCoord;
  }
  std::optional<container::geometry::Model> regenerated;
  std::optional<container::geometry::gltf::DeformationData> deformation;
  std::vector<container::geometry::Vertex> regeneratedVertices;
  std::vector<uint32_t> regeneratedIndices;
  if (texCoords!=variantNormalTexCoords_) {
    // The tangent generator consumes source-flat geometry. Keep procedural
    // slots and the runtime ordering intact around that source-only operation.
    std::vector<container::geometry::Mesh> sourceMeshes;
    sourceMeshes.reserve(sourceCount);
    for (const auto runtime:sourcePrimitiveToRuntime_)
      sourceMeshes.push_back(variantOriginalModel_->meshes()[runtime]);
    const auto sourceGeometry=container::geometry::Model::FromMeshes(std::move(sourceMeshes));
    const auto sourceRegenerated=container::geometry::gltf::RegenerateGltfBindTangents(gltfModel_,sourceGeometry,texCoords);
    auto runtimeMeshes=variantOriginalModel_->meshes();
    for (size_t source=0;source<sourceCount;++source)
      runtimeMeshes[sourcePrimitiveToRuntime_[source]]=sourceRegenerated.meshes()[source];
    regenerated=container::geometry::Model::FromMeshes(std::move(runtimeMeshes));
    if (variantOriginalDeformation_) {
      deformation=*variantOriginalDeformation_;
      if (deformation->primitives.size()!=sourceCount)
        throw std::runtime_error("Variant deformation/source primitive tables disagree");
      for (size_t i=0;i<deformation->primitives.size();++i) {
        deformation->primitives[i].bindVertices=sourceRegenerated.meshes()[i].vertices();
        deformation->primitives[i].normalTextureTexCoord=texCoords[i];
      }
    }
    regeneratedVertices=regenerated->vertices(); regeneratedIndices=regenerated->indices();
  }
  if (!ApplyMaterialVariantGraphEdit(graph,edit,forward,selectedMaterialVariant_)) return false;
  if (regenerated) {
    model_=std::move(*regenerated); deformation_=std::move(deformation);
    vertices_=std::move(regeneratedVertices); indices_=std::move(regeneratedIndices);
    variantNormalTexCoords_=std::move(texCoords); ++geometryRevision_;
  }
  ++materialRevision_;
  return true;
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
                 materialSamplerDescriptorCapacity_,
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

  for (auto &binding : bindings)
    binding.stageFlags |= VK_SHADER_STAGE_COMPUTE_BIT;
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
  if (!materialSamplers_.empty()) return;
  materialSamplers_.resize(container::material::kMaterialSamplerKeyCount, VK_NULL_HANDLE);
  const auto info = container::material::MakeMaterialSamplerCreateInfo({0});
  if (createOwnedSampler(deviceWrapper_->device(), &info, nullptr, &materialSamplers_[0]) != VK_SUCCESS)
    throw std::runtime_error("failed to create default scene texture sampler");
  baseColorSampler_ = materialSamplers_.front();
}

TextureResourceLifecycleView SceneManager::textureResourceLifecycleView() const noexcept {
  return {{exportSource_.images,exportSource_.textures,exportSource_.samplers,exportSource_.bufferViews,exportSource_.buffers,
          archiveTextureResources_.color,archiveTextureResources_.data,true,
          archiveResourcePhases_.images,archiveResourcePhases_.textures,archiveResourcePhases_.samplers},
      {gltfModel_.images,gltfModel_.textures,gltfModel_.samplers,gltfModel_.bufferViews,gltfModel_.buffers,
          gltfTextureResources_.color,gltfTextureResources_.data,true,
          gltfResourcePhases_.images,gltfResourcePhases_.textures,gltfResourcePhases_.samplers},textureManager_,archiveTextureToEvaluated_,archiveTextureIdentities_};
}

bool SceneManager::sourceResourceActive(TextureResourceSelection selected) const noexcept {
  using namespace container::util;
  if(selected.domain==ResourceDomainKind::Runtime)
    return selected.kind==ResourceKind::RuntimeTexture && textureManager_.isTextureActive(selected.row);
  if(selected.domain!=ResourceDomainKind::Archive && selected.domain!=ResourceDomainKind::Evaluated) return false;
  const auto& model=selected.domain==ResourceDomainKind::Archive?exportSource_:gltfModel_;
  const auto& phases=selected.domain==ResourceDomainKind::Archive?archiveResourcePhases_:gltfResourcePhases_;
  const std::vector<uint8_t>* mask{};size_t count{};
  switch(selected.kind) {
    case ResourceKind::Image:mask=&phases.images;count=model.images.size();break;
    case ResourceKind::Texture:mask=&phases.textures;count=model.textures.size();break;
    case ResourceKind::Sampler:mask=&phases.samplers;count=model.samplers.size();break;
    default:return false;
  }
  return selected.row<count && (mask->empty() || (mask->size()==count && (*mask)[selected.row]==1));
}

void SceneManager::refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy policy,uint64_t maxBytes,
    const container::geometry::gltf::AnimationPlayer* player) const {
  auto view=textureResourceLifecycleView();
  if(textureResourceLifecycle_ && textureResourceLifecycle_->matches(view) && materialResourceDependenciesValid(player)) return;
  uint64_t phaseBytes{};
  std::optional<SourceResourcePhaseTable> nextA,nextE;
  const auto normalize=[&](const SourceResourcePhaseTable& current,const tinygltf::Model& source,
      std::optional<SourceResourcePhaseTable>& next) {
    const bool changed=(!current.images.empty() && current.images.size()!=source.images.size()) ||
        (!current.textures.empty() && current.textures.size()!=source.textures.size()) ||
        (!current.samplers.empty() && current.samplers.size()!=source.samplers.size());
    if(!changed) return;
    const auto count=source.images.size()+source.textures.size()+source.samplers.size();
    const auto previous=current.images.size()+current.textures.size()+current.samplers.size();
    if(count>1'000'000 || previous>1'000'000 || phaseBytes>maxBytes || previous>maxBytes-phaseBytes || count>maxBytes-phaseBytes-previous)
      throw std::length_error("Trusted source phase synchronization exceeds its CPU budget");
    phaseBytes+=previous+count;next=current;next->resize(source.images.size(),source.textures.size(),source.samplers.size());
  };
  normalize(archiveResourcePhases_,exportSource_,nextA);normalize(gltfResourcePhases_,gltfModel_,nextE);
  if(nextA) {view.archive.imageActive=nextA->images;view.archive.textureActive=nextA->textures;view.archive.samplerActive=nextA->samplers;}
  if(nextE) {view.evaluated.imageActive=nextE->images;view.evaluated.textureActive=nextE->textures;view.evaluated.samplerActive=nextE->samplers;}
  if(phaseBytes>=maxBytes) throw std::length_error("Texture resource synchronization has no remaining CPU budget");
  maxBytes-=phaseBytes;
  TextureResourceLifecycleLimits limits;limits.retirementPolicy=policy;limits.maxOwnedBytes=maxBytes;
  auto staged=textureResourceLifecycle_?ReconcileTextureResourceLifecycle(textureResourceLifecycle_,view,limits):
      PrepareTextureResourceLifecycle(textureResourceLifecycle_,view,limits);
  if(staged.ownedBytes()>=maxBytes) throw std::length_error("Texture resource synchronization exceeds its CPU budget");
  auto dependencies=stageCurrentMaterialResourceDependencies(staged,maxBytes-staged.ownedBytes(),{},player);
  const auto dependencyBytes=materialResourceDependencyMapBytes(dependencies,materialResourceDependencies_);
  if(dependencyBytes>maxBytes-staged.ownedBytes())
    throw std::length_error("Texture resource dependencies exceed their synchronization CPU budget");
  staged.seal();
  if(!materialResourceDependencyMapWillBeActive(dependencies,staged) ||
      !CanExchangeTextureResourceLifecycle(staged,textureResourceLifecycle_,view) ||
      !ExchangeTextureResourceLifecycle(staged,textureResourceLifecycle_,view))
    throw std::runtime_error("Texture resource lifecycle publication became stale");
  materialResourceDependencies_.swap(dependencies);
  using std::swap;
  if(nextA) swap(archiveResourcePhases_,*nextA);
  if(nextE) swap(gltfResourcePhases_,*nextE);
}

bool SceneManager::textureSamplingFallbackValid() const noexcept {
  if(!textureSamplingFallback_ || !textureSamplingFallback_->lease ||
      !textureSamplingFallback_->lease->groupId() || !allocationManager_) return false;
  const auto& resource=textureSamplingFallback_->resource;
  return resource.width==1 && resource.height==1 && resource.mipLevels==1 && resource.samplerIndex==0 &&
      resource.format==VK_FORMAT_R8G8B8A8_UNORM && allocationManager_->textureImageMatches(resource);
}

bool SceneManager::textureSamplingStateValid() const noexcept {
  return textureSamplingStateValidImpl(false);
}

bool SceneManager::textureSamplingStateValidForResources() const noexcept {
  return textureSamplingStateValidImpl(true);
}

bool SceneManager::textureSamplingStateValidImpl(bool allowAppendedPrefix) const noexcept {
  if(!allocationManager_ || !container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_) ||
      !textureSamplingFallbackValid() ||
      !textureDescriptorCapacity_ || textureManager_.textureCount()>textureDescriptorCapacity_ ||
      textureSamplingPlan_.images.size()>textureDescriptorCapacity_ ||
      !materialSamplerDescriptorCapacity_ || materialSamplerSlots_.keys.size()>materialSamplerDescriptorCapacity_ ||
      baseColorSampler_==VK_NULL_HANDLE ||
      !(allowAppendedPrefix?
          container::material::TextureSamplingPlanPrefixMatches(textureSamplingPlan_,textureManager_,
              textureSamplingFallback_->resource,*allocationManager_):
          container::material::TextureSamplingPlanMatches(textureSamplingPlan_,textureManager_,
              textureSamplingFallback_->resource,*allocationManager_)) ||
      textureSamplingPlan_.metadata.size()!=textureMetadata_.size() ||
      textureSamplingPlan_.samplerSlots.keys!=materialSamplerSlots_.keys ||
      textureSamplingPlan_.samplerSlots.slots!=materialSamplerSlots_.slots) return false;
  for(size_t i=0;i<textureMetadata_.size();++i)
    if(std::memcmp(&textureMetadata_[i],&textureSamplingPlan_.metadata[i],sizeof(textureMetadata_[i]))!=0) return false;
  for(const auto key:materialSamplerSlots_.keys)
    if(key.value>=materialSamplers_.size() || materialSamplers_[key.value]==VK_NULL_HANDLE) return false;
  return true;
}

void SceneManager::finishTextureImageOwnershipPublication() {
  if(!textureSamplingStateValid()) throw std::runtime_error("Scene image ownership cannot publish inconsistent descriptor resources");
  // No group-zero ownership changes occur while metadata, descriptors or any
  // other fallible first-publication resource is still being prepared. Keeping
  // these temporary pins alone never adopts reset-owned scene images.
  auto adoption=container::material::PrepareTextureImageOwnershipAdoption(
      textureImageOwnership_,textureManager_,*allocationManager_);
  if(!container::material::CanAdoptTextureImageOwnership(adoption,textureManager_,*allocationManager_))
    throw std::runtime_error("Scene image ownership adoption became stale");
  if(!container::material::AdoptTextureImageOwnership(adoption,textureManager_,*allocationManager_))
    throw std::runtime_error("Scene image ownership adoption was rejected");
  // The complete immutable owner was staged in this manager before adoption.
  // Its shared transfer is already complete; no fallible publication follows.
}

void SceneManager::ensureTextureSamplingFallback() {
  if(textureSamplingFallback_) {
    if(!textureSamplingFallbackValid()) throw std::runtime_error("Scene texture fallback allocation is stale or foreign");
    return;
  }
  // Private single-image batch ownership is independent of authored/runtime
  // resource tables. Copies of the shared holder keep actual descriptor readers
  // alive; raw matching handles never grant this ownership.
  auto fallback=std::make_shared<TextureSamplingFallback>();
  auto batch=allocationManager_->beginTextureAllocationBatch();
  constexpr std::array rgba{std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
  fallback->resource=allocationManager_->createTextureFromRgbaPixels(
      "__private_scene_texture_fallback",rgba,1,1,VK_FORMAT_R8G8B8A8_UNORM);
  fallback->resource.samplerIndex=0;
  fallback->lease=batch.commit();
  textureSamplingFallback_=std::move(fallback);
}

/* ---------- Assets ---------- */

void SceneManager::loadMaterialXMaterial() {
  container::material::Material material{};

  try {
    const auto materialPath = resolveSceneAssetPath("materials/base.mtlx");
    auto doc = materialXBridge_.loadDocument(
        container::util::pathToUtf8(materialPath));
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

void SceneManager::loadGltfAssets(container::geometry::GltfLoadResult* prepared,
    const container::material::PreparedGltfTextures* preparedTextures) {
  if(materialResourceRevision_==UINT64_MAX) throw std::overflow_error("Material resource revision exhausted");
  materialLifecycle_.reset();materialLifecycleDomain_={};materialLifecycleHandles_.clear();materialLifecycleNextSlot_=0;
  materialResourceDependencies_.clear();textureResourceLifecycle_.reset();archiveResourcePhases_={};gltfResourcePhases_={};
  invalidateEditorSceneRootMembership();
  model_ = container::geometry::Model{};
  sourcePrimitiveToRuntime_.clear(); runtimePrimitiveToSource_.clear();
  gltfModel_ = tinygltf::Model{};
  sourceMaterialToRuntime_.clear();
  sourcePrimitiveImplicitMaterials_.clear();
  gltfInstancing_ = {};
  sceneCatalog_={}; selectedScene_=0; additionalSceneRoots_.clear(); sceneRootOrder_.clear(); sceneRootsDirty_=false;
  mergedAssets_=false;
  deformation_.reset();
  materialVariants_={}; selectedMaterialVariant_.reset();
  variantOriginalModel_.reset(); variantOriginalDeformation_.reset(); variantNormalTexCoords_.clear();
  exportSource_ = tinygltf::Model{};
  gltfTextureResources_ = {};
  archiveTextureResources_={};archiveTextureToEvaluated_.clear();archiveTextureIdentities_.clear();
  materialTextureSourceRequests_.clear();materialResourceOwnerIdentity_=std::make_shared<const uint8_t>(0);
  materialResourceVacancy_.reset();
  ++materialResourceRevision_;
  runtimeMaterialProvenance_.clear();runtimeMaterialProvenance_.resize(materialManager_.materialCount());
  for(uint32_t id=0;id<materialManager_.materialCount();++id) {
    const auto bindings=container::geometry::gltf::snapshotMaterialTextureBindings(*materialManager_.getMaterial(id)).runtimeIndices;
    runtimeMaterialProvenance_[id].known=std::ranges::all_of(bindings,[](uint32_t texture){return texture==UINT32_MAX;});
  }
  sourcePath_.clear();
  exportMaterialSources_.clear();
  exportMaterialTextureBindings_.clear();
  exportTextureIndices_.clear();
  authoredPointLights_.clear();
  authoredDirectionalLights_.clear();
  authoredAreaLights_.clear();
  authoredPointBindings_.clear();
  authoredDirectionalBindings_.clear();
  authoredAreaBindings_.clear();

   if (isDefaultSceneRequest(config_.modelPath)) {
    loadDefaultTestSceneAssets();
    vertices_ = model_.vertices();
    indices_ = model_.indices();
    sceneCatalog_=BuildGltfSceneCatalog(gltfModel_);
    if(config_.gltfSceneIndex>0) throw std::invalid_argument("Requested glTF scene is unavailable");
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
      auto result = prepared ? std::move(*prepared)
                             : prepareGltfModel(container::util::pathToUtf8(resolvedPath));
      auto variants=container::geometry::gltf::ParseMaterialVariants(result.gltfModel);
      const std::vector<uint32_t> preflightMaterials(result.model.primitiveRanges().size(),0);
      auto catalog=BuildGltfSceneCatalog(result.gltfModel);
      (void)prepareCompleteHierarchy(result.gltfModel,result.instancing,preflightMaterials,
          importScaleTransform(config_.importScale),1,result.model.meshes().size(),config_.maxSceneObjects,config_.gltfSceneIndex);
      selectedScene_=config_.gltfSceneIndex>=0 ? size_t(config_.gltfSceneIndex) : catalog.defaultScene;
      sceneCatalog_=std::move(catalog);
      gltfModel_ = std::move(result.gltfModel);
      if(!gltfModel_.scenes.empty()) gltfModel_.defaultScene=int(selectedScene_);
      gltfInstancing_ = std::move(result.instancing);
      model_ = std::move(result.model);
      deformation_ = std::move(result.deformation);
      materialVariants_=std::move(variants);
      sourcePath_ = resolvedPath;
      exportSource_ = gltfModel_;
      exportSource_.buffers.clear();
      exportSource_.bufferViews.clear();
      exportSource_.images.clear();
      exportSource_.samplers.clear();
      exportSource_.textures.clear();

      validateGltfSamplerPolicies(gltfModel_);
      std::optional<container::material::PreparedGltfTextures> localTextures;
      if (!preparedTextures) {
        localTextures = container::material::PrepareGltfTextures(gltfModel_,
            allocationManager_->textureDecodeOptions(false));
        preparedTextures = &*localTextures;
      }
      if(preparedTextures->textureCount!=gltfModel_.textures.size())
        throw std::runtime_error("Prepared glTF texture identity table differs from the source");
      auto imageToTexture = container::material::UploadPreparedGltfTextures(
          *preparedTextures, textureManager_, [this](const auto& name, const auto& image) {
            return allocationManager_->createTextureFromImageData(name, image);
          });
      validateGltfTextureResourceTable(imageToTexture,gltfModel_.textures.size(),textureManager_.textureCount());

      const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
      const size_t firstRuntimeMaterial = materialManager_.materialCount();
      materialXBridge_.loadMaterialsForGltfWithTextureRoles(
          gltfModel_, imageToTexture, materialManager_, defaultMaterialIndex_);
      sourceMaterialToRuntime_ = MakeContiguousSourceMaterialMapping(firstRuntimeMaterial,
          gltfModel_.materials.size(), materialManager_.materialCount());
      rememberExportMaterials(gltfModel_, resolvedPath, imageToTexture,
                              sourceMaterialToRuntime_);
      gltfTextureResources_ = std::move(imageToTexture);
      defaultMaterialIndex_ = fallbackMaterialIndex;
      const bool needsImplicit=std::ranges::any_of(gltfModel_.meshes,[](const auto& mesh) {
        return std::ranges::any_of(mesh.primitives,[](const auto& primitive){return primitive.material<0;});
      });
      uint32_t implicitMaterial=fallbackMaterialIndex;
      if(needsImplicit) {
        container::material::Material implicit;
        implicit.metallicFactor=1.f;
        implicitMaterial=createMaterial(implicit);
      }
      if (materialVariants_.hasVariants()) {
        variantOriginalModel_=model_; variantOriginalDeformation_=deformation_;
      }
      // The loader built/repaired tangents with each original default's
      // effective Normal UV. Retain that policy even for static assets and
      // after their Normal binding is cleared or reassigned in the editor.
      variantNormalTexCoords_.reserve(materialVariants_.primitives().size());
      for (const auto& primitive:materialVariants_.primitives()) {
        const auto* material=primitive.defaultMaterial<0?nullptr:
            materialManager_.getMaterial(resolveLoadedMaterialIndex(primitive.defaultMaterial));
        variantNormalTexCoords_.push_back(material && material->normalTextureIndex!=std::numeric_limits<uint32_t>::max()
            ?material->normalTextureTransform.texCoord:0);
      }
      auto primitiveMapping = MakeSourcePrimitiveMapping(sourcePrimitiveCount(gltfModel_),model_.meshes().size());
      sourcePrimitiveToRuntime_ = std::move(primitiveMapping.sourceToRuntime);
      runtimePrimitiveToSource_ = std::move(primitiveMapping.runtimeToSource);
      if(needsImplicit) sourcePrimitiveImplicitMaterials_.assign(sourcePrimitiveToRuntime_.size(),implicitMaterial);
    } catch (const std::exception& e) {
      std::println(stderr, "glTF load failed: {}", e.what());
      throw;
    }
  }

  vertices_ = model_.vertices();
  indices_ = model_.indices();
  if(sceneCatalog_.entries.empty()) sceneCatalog_=BuildGltfSceneCatalog(gltfModel_);
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
      createMaterial(sphereMaterial));

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
  sourcePrimitiveToRuntime_.clear();
  runtimePrimitiveToSource_.assign(model_.meshes().size(),-1);
  // appendSceneAsset retained resource-only source tables. They deliberately
  // have no source material/node identities, but remain valid for later runtime
  // material creation with explicit texture slots.
}

void SceneManager::appendSceneAsset(
    const std::filesystem::path& assetPath,
    const glm::mat4& transform,
    std::vector<container::geometry::Mesh>& mergedMeshes) {
  auto result = prepareDefaultSceneGltfAsset(assetPath);

  validateGltfSamplerPolicies(result.gltfModel);
  const auto preparedTextures = container::material::PrepareGltfTextures(result.gltfModel,
      allocationManager_->textureDecodeOptions(false));
  const auto imageToTexture = container::material::UploadPreparedGltfTextures(
      preparedTextures, textureManager_, [this](const auto& name, const auto& image) {
        return allocationManager_->createTextureFromImageData(name, image);
      });

  const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
  const size_t firstRuntimeMaterial = materialManager_.materialCount();
  materialXBridge_.loadMaterialsForGltfWithTextureRoles(result.gltfModel, imageToTexture,
                                        materialManager_,
                                        defaultMaterialIndex_);
  const auto sourceMaterialToRuntime = MakeContiguousSourceMaterialMapping(firstRuntimeMaterial,
      result.gltfModel.materials.size(), materialManager_.materialCount());
  rememberExportMaterials(result.gltfModel, assetPath, imageToTexture,
                          sourceMaterialToRuntime);
  defaultMaterialIndex_ = fallbackMaterialIndex;

  uint32_t implicitMaterial=fallbackMaterialIndex;
  if(std::ranges::any_of(result.model.meshes(),[](const auto& mesh){return mesh.materialIndex()<0;})) {
    container::material::Material implicit;implicit.metallicFactor=1.f;implicitMaterial=createMaterial(implicit);
  }

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
          ? static_cast<int32_t>(ResolveSourceMaterialIndex(sourceMaterialIndex, sourceMaterialToRuntime,
              result.gltfModel.materials.size(), materialManager_.materialCount(), defaultMaterialIndex_))
          : static_cast<int32_t>(implicitMaterial);
      mergedMeshes.push_back(transformMesh(mesh, nodeTransform, materialIndex));
    }
  }

  while (meshCursor < result.model.meshes().size()) {
    const auto& mesh = result.model.meshes()[meshCursor++];
    const int32_t materialIndex = mesh.materialIndex() >= 0
        ? static_cast<int32_t>(ResolveSourceMaterialIndex(mesh.materialIndex(), sourceMaterialToRuntime,
            result.gltfModel.materials.size(), materialManager_.materialCount(), defaultMaterialIndex_))
        : static_cast<int32_t>(implicitMaterial);
    mergedMeshes.push_back(transformMesh(mesh, transform, materialIndex));
  }
}

void SceneManager::collectAuthoredPunctualLights(const SceneGraph* graph) {
  auto lights = evaluateAuthoredSceneLights(graph, selectedScene_);
  authoredPointLights_ = std::move(lights.points);
  authoredDirectionalLights_ = std::move(lights.directionals);
  authoredAreaLights_ = std::move(lights.areas);
  authoredPointBindings_ = std::move(lights.pointBindings);
  authoredDirectionalBindings_ = std::move(lights.directionalBindings);
  authoredAreaBindings_ = std::move(lights.areaBindings);
}

AuthoredSceneLights SceneManager::evaluateAuthoredSceneLights(
    const SceneGraph& graph, size_t scene, bool includeHidden) const {
  return evaluateAuthoredSceneLights(&graph, scene, includeHidden);
}

AuthoredSceneLights SceneManager::evaluateAuthoredSceneLights(
    const SceneGraph* graph, size_t scene, bool includeHidden) const {
  if ((!sceneCatalog_.entries.empty() && scene >= sceneCatalog_.entries.size()) ||
      (sceneCatalog_.entries.empty() && scene != 0))
    throw std::invalid_argument("Authored light scene index is out of range");
  AuthoredSceneLights result;
  if (gltfModel_.nodes.empty()) {
    return result;
  }
  const auto nodeFlags = graph ? container::geometry::gltf::GltfNodeFlagData{}
      : container::geometry::gltf::ParseGltfNodeFlags(gltfModel_);

  glm::mat4 inverseOuterRoot(1.0f);
  if (graph) {
    for (uint32_t index = 0; index < graph->nodeCount(); ++index) {
      const auto* runtimeNode = graph->getNode(index);
      if (!runtimeNode || graph->isNodeRetired(index)) continue;
      // LightingManager applies this editor anchor to authored light records.
      // Retain the separate Import Scale ancestor inside the authored record.
      if (runtimeNode->editorSceneRoot &&
          runtimeNode->parent == SceneGraph::kInvalidNode &&
          std::abs(glm::determinant(runtimeNode->worldTransform)) > 1e-10f)
        inverseOuterRoot = glm::inverse(runtimeNode->worldTransform);
    }
  }

  struct LightNode { int source; glm::mat4 transform; };
  std::vector<LightNode> lightNodes;
  if (graph) {
    // Explicit runtime membership includes editor reparenting/additional roots;
    // walking source JSON adjacency would leak moved lights into their old scene.
    const auto activation = planSceneSelection(*graph,scene,kMaxAssetRenderableObjects);
    struct RuntimeWork { uint32_t node; bool parentVisible; };
    std::vector<RuntimeWork> work;
    std::vector<bool> visited(graph->nodeCount(), false);
    for (auto root = activation.runtimeRoots.rbegin(); root != activation.runtimeRoots.rend(); ++root) {
      bool parentVisible = true;
      bool retiredAncestor = false;
      auto ancestor = graph->getNode(graph->getNode(*root)->parent);
      size_t depth = 0;
      while (ancestor && depth++ < graph->nodeCount()) {
        if(ancestor->retired) { retiredAncestor=true; break; }
        parentVisible = parentVisible && ancestor->visible;
        ancestor = graph->getNode(ancestor->parent);
      }
      if(retiredAncestor) continue;
      if (ancestor) parentVisible = false; // Fail closed on a corrupted parent cycle.
      work.push_back({*root, parentVisible});
    }
    while (!work.empty()) {
      const auto [id, parentVisible] = work.back(); work.pop_back();
      if (id >= visited.size() || visited[id]) continue;
      visited[id] = true;
      const auto& runtime = *graph->getNode(id);
      if(graph->isNodeRetired(id)) continue; // Retirement is independent of includeHidden.
      const bool visible = parentVisible && runtime.visible;
      if (!includeHidden && !visible) continue;
      if (runtime.sourceNodeIndex >= 0 && size_t(runtime.sourceNodeIndex) < gltfModel_.nodes.size())
        lightNodes.push_back({runtime.sourceNodeIndex, inverseOuterRoot * runtime.worldTransform});
      for (auto child = runtime.children.rbegin(); child != runtime.children.rend(); ++child)
        work.push_back({*child, visible});
    }
  } else {
    struct SourceWork { int node; glm::mat4 parentTransform; bool parentVisible; };
    std::vector<SourceWork> work;
    std::vector<bool> visited(gltfModel_.nodes.size(), false);
    const auto roots = sceneCatalog_.entries.empty()
        ? activeSceneRootNodes(gltfModel_) : sceneCatalog_.entries[scene].sourceRoots;
    for (auto root = roots.rbegin(); root != roots.rend(); ++root)
      work.push_back({*root, importScaleTransform(config_.importScale), true});
    while (!work.empty()) {
      const auto [id, parentTransform, parentVisible] = work.back(); work.pop_back();
      if (id < 0 || size_t(id) >= visited.size() || visited[size_t(id)]) continue;
      visited[size_t(id)] = true;
      const auto& node = gltfModel_.nodes[size_t(id)];
      const auto transform = parentTransform * nodeLocalTransform(node);
      const bool visible = parentVisible && nodeFlags.nodes[size_t(id)].visible;
      if (!includeHidden && !visible) continue;
      lightNodes.push_back({id, transform});
      for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
        work.push_back({*child, transform, visible});
    }
  }
  for (const auto& [nodeIndex, sceneLocalTransform] : lightNodes) {
        const auto& node = gltfModel_.nodes[size_t(nodeIndex)];

        if (node.light >= 0 &&
            node.light < static_cast<int>(gltfModel_.lights.size())) {
          const tinygltf::Light& lightDefinition =
              gltfModel_.lights[static_cast<size_t>(node.light)];
          if (const std::optional<container::gpu::AreaLightData> areaLight =
                  makeGltfAreaLightData(lightDefinition, sceneLocalTransform,
                                        config_.importScale)) {
            result.areas.push_back(*areaLight);
            result.areaBindings.push_back({nodeIndex, {}});
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
            result.points.push_back(pointLight);
            result.pointBindings.push_back({nodeIndex, {}});
          } else if (lightDefinition.type == "directional") {
            AuthoredDirectionalLight directionalLight{};
            directionalLight.direction =
                glm::vec4(gltfDirectionalLightDirection(sceneLocalTransform),
                          0.0f);
            directionalLight.colorIntensity =
                glm::vec4(pointLightColorOrDefault(lightDefinition),
                          pointLightIntensityOrDefault(lightDefinition));
            result.directionals.push_back(directionalLight);
            result.directionalBindings.push_back({nodeIndex, {}});
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
            result.points.push_back(spotLight);
            result.pointBindings.push_back({nodeIndex, {}});
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
            result.areas.push_back(*areaLight);
            result.areaBindings.push_back({nodeIndex, extensionName});
          }
        }

  }
  return result;
}

void SceneManager::publishAnimationChanges(
    const container::geometry::gltf::AnimationDirtySet& dirty,
    const SceneGraph& graph) {
  if(!dirty.cameras.empty() && cameraEvaluationRevision_==UINT64_MAX)
    throw std::overflow_error("Camera publication revision exhausted");
  bool materialChanged = false;
  // Validate the complete publication before mutating any runtime material.
  if (!dirty.materials.empty()) {
    ValidateSourceMaterialMapping(sourceMaterialToRuntime_, gltfModel_.materials.size(),
                                 materialManager_.materialCount());
    for (const size_t sourceIndex : dirty.materials)
      if (sourceIndex >= gltfModel_.materials.size())
        throw std::invalid_argument("animated material source index out of range");
  }
  for (size_t sourceIndex : dirty.materials) {
    const uint32_t runtimeIndex = resolveLoadedMaterialIndex(static_cast<int32_t>(sourceIndex));
    if (const auto* previous = materialManager_.getMaterial(runtimeIndex)) {
      auto material = *previous;
      container::geometry::gltf::UpdateAnimatedMaterialProperties(gltfModel_.materials[sourceIndex],material);
      materialManager_.updateMaterial(runtimeIndex,material);
      materialChanged = true;
    }
  }
  if (materialChanged) uploadMaterialBuffer();
  if (!dirty.nodes.empty() || !dirty.lights.empty()) collectAuthoredPunctualLights(&graph);
  if(!dirty.cameras.empty()) ++cameraEvaluationRevision_;
}

void SceneManager::uploadMaterialBuffer() {
  // Uploads publish imported or edited material classification as well as color.
  ++materialRevision_;
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
  hasExtendedPbrMaterials_ = std::any_of(gpuMaterials_.begin(), gpuMaterials_.end(),
      [](const auto& material) {
        return material.anisotropyDiffuseFactors.x > 0.0f ||
               material.anisotropyDiffuseFactors.z > 0.0f;
      });
  for (auto& material : gpuMaterials_) {
    // Every resolved material can share a pixel with a feature-bearing MSAA
    // sample. Publish the scene-wide auxiliary availability, so decoding an
    // ordinary material never suppresses its neighbour's resolved response.
    material.anisotropyDiffuseFactors.w = hasExtendedPbrMaterials_ ? 1.0f : 0.0f;
  }
  gpuMaterialTemporalRevisions_.clear();
  gpuMaterialTemporalRevisions_.reserve(gpuMaterials_.size());
  for (const auto& material : gpuMaterials_) gpuMaterialTemporalRevisions_.push_back(container::temporal::materialRevision(material));

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

void SceneManager::uploadTextureMetadataBuffer(uint64_t maxLogicalCpuBytes) {
  ensureTextureSamplingFallback();
  const std::array inherited{textureImageOwnership_};
  auto ownership=container::material::PrepareTextureImageOwnership(textureManager_,*allocationManager_,inherited);
  auto plan=container::material::MakeTextureSamplingPlan(textureManager_,textureSamplingFallback_->resource,
      *allocationManager_,textureDescriptorCapacity_,materialSamplerDescriptorCapacity_);
  for (const auto key : plan.samplerSlots.keys) {
    auto& sampler = materialSamplers_[key.value];
    if (sampler != VK_NULL_HANDLE) continue;
    const auto info = container::material::MakeMaterialSamplerCreateInfo(key);
    if (createOwnedSampler(deviceWrapper_->device(), &info, nullptr, &sampler) != VK_SUCCESS)
      throw std::runtime_error("failed to create authored scene texture sampler");
  }
  materialSamplerSlots_ = plan.samplerSlots;
  textureMetadata_ = plan.metadata;
  textureSamplingPlan_ = std::move(plan);

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
  // The old complete set remains in this local until after the transfer, so a
  // final pin release/free cannot occur within the shared-owner exchange.
    refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::BlockPreparations,maxLogicalCpuBytes);
    textureImageOwnership_.swap(ownership);
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
    std::string_view relativePath) {
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

void SceneManager::updateModelBounds(const SceneGraph* graph) {
  modelBounds_ = calculateModelBounds(graph);
}

ModelBounds SceneManager::calculateModelBounds(const SceneGraph* graph) const {
  ModelBounds bounds;
  if (vertices_.empty()) {
    return bounds;
  }

  auto forEachRenderedPoint =
      [this,graph](const std::function<void(const glm::vec3&)>& visit) {
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

        if(graph) {
          glm::mat4 inverseOuterRoot(1);
          for(const auto root:graph->rootNodes()) {
            const auto* node=graph->getNode(root);
            if(node && !graph->isNodeRetired(root) && node->editorSceneRoot && std::abs(glm::determinant(node->worldTransform))>1e-10f)
              inverseOuterRoot=glm::inverse(node->worldTransform);
          }
          for(const auto id:graph->renderableNodes()) {
            const auto& node=*graph->getNode(id);
            if(!graph->isNodeRetired(id) && graph->isNodeEffectivelyVisible(id))
              emitPrimitive(node.primitiveIndex,inverseOuterRoot*node.worldTransform);
          }
          return;
        }

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

                if(const auto visibility=node.extensions.find("KHR_node_visibility");
                    visibility!=node.extensions.end() && visibility->second.IsObject()) {
                  const auto& visible=visibility->second.Get("visible");
                  if(visible.IsBool() && !visible.Get<bool>()) return;
                }

                if (node.mesh >= 0 &&
                    node.mesh < static_cast<int>(gltfModel_.meshes.size())) {
                  const auto& mesh = gltfModel_.meshes[node.mesh];
                  const uint32_t basePrimitiveIndex =
                      meshPrimitiveBase[node.mesh];
                  for (size_t primitiveInMesh = 0;
                       primitiveInMesh < mesh.primitives.size();
                       ++primitiveInMesh) {
                    const auto sourcePrimitive=basePrimitiveIndex+static_cast<uint32_t>(primitiveInMesh);
                    if (sourcePrimitive>=sourcePrimitiveToRuntime_.size())
                      throw std::runtime_error("Model bounds references an unavailable source primitive");
                    const auto primitive=sourcePrimitiveToRuntime_[sourcePrimitive];
                    if(const auto* instances=container::geometry::gltf::FindNodeInstancing(
                        gltfInstancing_,uint32_t(gltfNodeIndex))) {
                      for(uint32_t instance=0;instance<instances->localMatrices.size();++instance)
                        emitPrimitive(primitive,container::geometry::gltf::ComposeInstanceWorld(*instances,instance,worldTransform));
                    } else emitPrimitive(primitive,worldTransform);
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
    return bounds;
  }

  const glm::vec3 center = 0.5f * (minBounds + maxBounds);
  float radius = 0.0f;
  forEachRenderedPoint([&](const glm::vec3& point) {
    radius = std::max(radius, glm::length(point - center));
  });

  bounds.min = minBounds;
  bounds.max = maxBounds;
  bounds.center = center;
  bounds.size = maxBounds - minBounds;
  bounds.radius = radius;
  bounds.valid = true;
  return bounds;
}

/* ---------- Descriptor sets ---------- */

void SceneManager::allocateDescriptorSets(uint32_t descriptorSetCount) {
  const uint32_t textureDescriptorCount =
      std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager_.textureCount()));
  const uint32_t frameSetCount = std::max(1u, descriptorSetCount);
  const uint32_t setCount = frameSetCount * 2u;

  if (descriptorPool_ != VK_NULL_HANDLE) {
    if(ownsDescriptorPool_) {
      vkDestroyDescriptorPool(deviceWrapper_->device(),descriptorPool_,nullptr);
      descriptorPool_=VK_NULL_HANDLE;
    } else pipelineManager_->destroyDescriptorPool(descriptorPool_);
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
        materialSamplerDescriptorCapacity_ * setCount},
  };

  if(ownsDescriptorPool_) {
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets=setCount;
    poolInfo.poolSizeCount=static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes=poolSizes.data();
    if(vkCreateDescriptorPool(deviceWrapper_->device(),&poolInfo,nullptr,&descriptorPool_)!=VK_SUCCESS)
      throw std::runtime_error("failed to create detached scene descriptor pool");
  } else descriptorPool_ = pipelineManager_->createDescriptorPool(poolSizes, setCount, 0);

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

  writeMaterialDescriptorSetContents(descriptorSet,cameraBuffer,objectBuffer,
      materialBuffer_,gpuMaterials_.size());
}

void SceneManager::writeMaterialDescriptorSetContents(
    VkDescriptorSet descriptorSet,const container::gpu::AllocatedBuffer& cameraBuffer,
    const container::gpu::AllocatedBuffer& objectBuffer,
    const container::gpu::AllocatedBuffer& materialBuffer,size_t materialCount) const {

  VkDescriptorBufferInfo cameraInfo{cameraBuffer.buffer, 0, sizeof(container::gpu::CameraData)};
  VkDescriptorBufferInfo objectInfo{
      // Memory requirements may be padded by the driver/capture layer. Bind
      // the logical VkBuffer, never its potentially larger VMA allocation.
      objectBuffer.buffer, 0, VK_WHOLE_SIZE};
  VkDescriptorBufferInfo materialInfo{
      materialBuffer.buffer, 0,
      static_cast<VkDeviceSize>(
          sizeof(container::gpu::GpuMaterial) *
          std::max<size_t>(1, materialCount))};
  VkDescriptorBufferInfo textureMetadataInfo{
      textureMetadataBuffer_.buffer, 0,
      static_cast<VkDeviceSize>(
          sizeof(container::gpu::GpuTextureMetadata) *
          std::max<size_t>(1, textureMetadata_.size()))};
  VkDescriptorBufferInfo sceneClipInfo{
      sceneClipStateBuffer_.buffer, 0, sizeof(container::gpu::SceneClipState)};

  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(8);

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

  if(!textureSamplingStateValid())
    throw std::runtime_error("Scene texture descriptor metadata is stale or inconsistent");
  const auto& imageInfos=textureSamplingPlan_.images;

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

  std::vector<VkDescriptorImageInfo> samplerInfos(materialSamplerDescriptorCapacity_);
  for (uint32_t i = 0; i < materialSamplerDescriptorCapacity_; ++i) {
    samplerInfos[i].sampler = i < materialSamplerSlots_.keys.size()
        ? materialSamplers_[materialSamplerSlots_.keys[i].value] : baseColorSampler_;
    samplerInfos[i].imageView = VK_NULL_HANDLE;
    samplerInfos[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  VkWriteDescriptorSet samplerWrite{};
  samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  samplerWrite.dstSet = descriptorSet;
  samplerWrite.dstBinding = 2;
  samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  samplerWrite.descriptorCount =
      materialSamplerDescriptorCapacity_;
  samplerWrite.pImageInfo = samplerInfos.data();
  writes.push_back(samplerWrite);

  vkUpdateDescriptorSets(deviceWrapper_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

void SceneManager::resetLoadedAssets() {
  if(cameraProjectionRevision_==UINT64_MAX) throw std::overflow_error("Camera publication revision exhausted");
  cameraProjectionNamespace_.reset();++cameraProjectionRevision_;
  if(materialResourceRevision_==UINT64_MAX) throw std::overflow_error("Material resource revision exhausted");
  invalidateEditorSceneRootMembership();
  materialVariants_={}; selectedMaterialVariant_.reset();
  variantOriginalModel_.reset(); variantOriginalDeformation_.reset(); variantNormalTexCoords_.clear();
  ++geometryRevision_;
  ++materialRevision_;
  ++materialResourceRevision_;
  if (materialBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_->destroyBuffer(materialBuffer_);
  }
  if (textureMetadataBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_->destroyBuffer(textureMetadataBuffer_);
  }
  materialBufferCapacity_ = 0;
  textureMetadataBufferCapacity_ = 0;
  gpuMaterials_.clear();
  hasExtendedPbrMaterials_ = false;
  textureMetadata_.clear();
  textureSamplingPlan_={};
  materialSamplerSlots_ = {};
  textureSamplingFallback_.reset();
  textureImageOwnership_.reset();
  materialResourceDependencies_.clear();textureResourceLifecycle_.reset();archiveResourcePhases_={};gltfResourcePhases_={};
  if(resetTextureAllocationsOnReset_) allocationManager_->resetTextureAllocations();
  runtimeMaterialProvenance_.clear();archiveTextureResources_={};archiveTextureToEvaluated_.clear();archiveTextureIdentities_.clear();
  materialTextureSourceRequests_.clear();materialResourceOwnerIdentity_=std::make_shared<const uint8_t>(0);
  materialResourceVacancy_.reset();
  materialManager_ = container::material::MaterialManager{};
  materialLifecycle_.reset();materialLifecycleDomain_={};materialLifecycleHandles_.clear();materialLifecycleNextSlot_=0;
  materialEntryOwners_.clear();materialTailVacancy_.reset();
  textureManager_ = container::material::TextureManager{};
  defaultMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  diagnosticMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  sourceMaterialToRuntime_.clear();
  sourcePrimitiveImplicitMaterials_.clear();
  sourcePrimitiveToRuntime_.clear(); runtimePrimitiveToSource_.clear();
  sceneRootOrder_.clear();
  mergedAssets_=false;
  deformation_.reset();
  exportSource_ = tinygltf::Model{};
  gltfTextureResources_ = {};
  sourcePath_.clear();
  exportMaterialSources_.clear();
  exportMaterialTextureBindings_.clear();
  exportTextureIndices_.clear();
  materialBaseColor_ = glm::vec4(1.0f);
  modelBounds_ = ModelBounds{};
  authoredPointLights_.clear();
  authoredDirectionalLights_.clear();
  authoredAreaLights_.clear();
  authoredPointBindings_.clear();
  authoredDirectionalBindings_.clear();
  authoredAreaBindings_.clear();
}

bool SceneManager::hasVolumeMaterials() const noexcept {
  return std::any_of(gpuMaterials_.begin(), gpuMaterials_.end(), [](const auto& material) {
    return material.thicknessFactor > 0.0f && material.transmissionFactor > 0.0f;
  });
}

}  // namespace container::scene
