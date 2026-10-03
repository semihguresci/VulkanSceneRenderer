#include "Container/renderer/bim/BimManager.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/geometry/Model.h"
#include "Container/geometry/UsdLoader.h"
#include "Container/renderer/bim/BimDrawFilterState.h"
#include "Container/renderer/bim/BimDrawCompactionPlanner.h"
#include "Container/renderer/bim/BimMetadataCatalog.h"
#include "Container/renderer/bim/BimMetadataIndex.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/Material.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace container::renderer {
namespace {

using Json = nlohmann::json;

constexpr uint32_t kBimVisibilityFilterType = 1u << 0u;
constexpr uint32_t kBimVisibilityFilterStorey = 1u << 1u;
constexpr uint32_t kBimVisibilityFilterMaterial = 1u << 2u;
constexpr uint32_t kBimVisibilityFilterDiscipline = 1u << 3u;
constexpr uint32_t kBimVisibilityFilterPhase = 1u << 4u;
constexpr uint32_t kBimVisibilityFilterFireRating = 1u << 5u;
constexpr uint32_t kBimVisibilityFilterLoadBearing = 1u << 6u;
constexpr uint32_t kBimVisibilityFilterStatus = 1u << 7u;
constexpr uint32_t kBimVisibilityFilterDrawBudget = 1u << 8u;
constexpr uint32_t kBimVisibilityFilterIsolateSelection = 1u << 9u;
constexpr uint32_t kBimVisibilityFilterHideSelection = 1u << 10u;

size_t drawCompactionSlotIndex(BimDrawCompactionSlot slot) {
  return std::min(static_cast<size_t>(slot), kBimDrawCompactionSlotCount - 1u);
}

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string normalizedElementType(std::string_view value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1u]))) {
    --end;
  }
  if (begin == end) {
    return "Unknown";
  }
  return std::string(value.substr(begin, end - begin));
}

glm::mat4 importScaleTransform(float scale) {
  return glm::scale(glm::mat4(1.0f), glm::vec3(sanitizeImportScale(scale)));
}

BimModelUnitMetadata bimModelUnitMetadata(
    const container::geometry::dotbim::ModelUnitMetadata &source) {
  BimModelUnitMetadata metadata{};
  metadata.hasSourceUnits = source.hasSourceUnits;
  metadata.sourceUnits = source.sourceUnits;
  metadata.hasMetersPerUnit = source.hasMetersPerUnit;
  metadata.metersPerUnit = source.metersPerUnit;
  metadata.hasImportScale = source.hasImportScale;
  metadata.importScale = source.importScale;
  metadata.hasEffectiveImportScale = source.hasEffectiveImportScale;
  metadata.effectiveImportScale = source.effectiveImportScale;
  return metadata;
}

BimModelGeoreferenceMetadata bimModelGeoreferenceMetadata(
    const container::geometry::dotbim::ModelGeoreferenceMetadata &source) {
  BimModelGeoreferenceMetadata metadata{};
  metadata.hasSourceUpAxis = source.hasSourceUpAxis;
  metadata.sourceUpAxis = source.sourceUpAxis;
  metadata.hasCoordinateOffset = source.hasCoordinateOffset;
  metadata.coordinateOffset = source.coordinateOffset;
  metadata.coordinateOffsetSource = source.coordinateOffsetSource;
  metadata.crsName = source.crsName;
  metadata.crsAuthority = source.crsAuthority;
  metadata.crsCode = source.crsCode;
  metadata.mapConversionName = source.mapConversionName;
  return metadata;
}

BimElementProperty
bimElementProperty(const container::geometry::dotbim::ElementProperty &source) {
  return BimElementProperty{
      .set = source.set,
      .name = source.name,
      .value = source.value,
      .category = source.category,
  };
}

std::vector<BimElementProperty> bimElementProperties(
    const std::vector<container::geometry::dotbim::ElementProperty> &source) {
  std::vector<BimElementProperty> properties;
  properties.reserve(source.size());
  for (const auto &property : source) {
    if (!property.name.empty() && !property.value.empty()) {
      properties.push_back(bimElementProperty(property));
    }
  }
  return properties;
}

BimModelUnitMetadata fallbackUnitMetadata(float importScale) {
  const float sanitizedImportScale = sanitizeImportScale(importScale);
  BimModelUnitMetadata metadata{};
  metadata.hasImportScale = true;
  metadata.importScale = sanitizedImportScale;
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale = sanitizedImportScale;
  return metadata;
}

bool transformFlipsWinding(const glm::mat4 &transform) {
  const glm::vec3 x(transform[0]);
  const glm::vec3 y(transform[1]);
  const glm::vec3 z(transform[2]);
  const float determinant = x.x * (y.y * z.z - y.z * z.y) -
                            y.x * (x.y * z.z - x.z * z.y) +
                            z.x * (x.y * y.z - x.z * y.y);
  return determinant < 0.0f;
}

bool intersectRaySphere(const glm::vec3 &origin, const glm::vec3 &direction,
                        const glm::vec3 &center, float radius,
                        float &outDistance) {
  if (radius <= 0.0f) {
    return false;
  }

  const glm::vec3 toCenter = center - origin;
  const float projected = glm::dot(toCenter, direction);
  const float centerDistance2 = glm::dot(toCenter, toCenter);
  const float radius2 = radius * radius;
  const float closestDistance2 = centerDistance2 - projected * projected;
  if (closestDistance2 > radius2) {
    return false;
  }

  const float halfChord = std::sqrt(std::max(0.0f, radius2 - closestDistance2));
  const float t0 = projected - halfChord;
  const float t1 = projected + halfChord;
  if (t1 < 0.0f) {
    return false;
  }

  outDistance = t0 >= 0.0f ? t0 : 0.0f;
  return true;
}

enum class TriangleCullMode {
  None,
  Back,
  Front,
};

struct PickRay {
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
  bool valid{false};
};

PickRay makePickRay(const container::gpu::CameraData &cameraData,
                    VkExtent2D viewportExtent, double cursorX, double cursorY) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u) {
    return {};
  }

  const float ndcX = static_cast<float>(
      (cursorX / static_cast<double>(viewportExtent.width)) * 2.0 - 1.0);
  const float ndcY = static_cast<float>(
      1.0 - (cursorY / static_cast<double>(viewportExtent.height)) * 2.0);

  const glm::vec4 nearClip{ndcX, ndcY, 1.0f, 1.0f};
  const glm::vec4 farClip{ndcX, ndcY, 0.0f, 1.0f};
  glm::vec4 nearWorld = cameraData.inverseViewProj * nearClip;
  glm::vec4 farWorld = cameraData.inverseViewProj * farClip;
  if (nearWorld.w == 0.0f || farWorld.w == 0.0f) {
    return {};
  }
  nearWorld /= nearWorld.w;
  farWorld /= farWorld.w;

  PickRay ray{};
  ray.origin = glm::vec3(nearWorld);
  const glm::vec3 farPoint = glm::vec3(farWorld);
  ray.direction = farPoint - ray.origin;
  const float rayLength = glm::length(ray.direction);
  if (rayLength <= 0.0001f) {
    return {};
  }
  ray.direction /= rayLength;
  ray.valid = true;
  return ray;
}

bool intersectRayTriangle(const glm::vec3 &origin, const glm::vec3 &direction,
                          const glm::vec3 &v0, const glm::vec3 &v1,
                          const glm::vec3 &v2, TriangleCullMode cullMode,
                          float &outDistance) {
  constexpr float kEpsilon = 0.0000001f;
  const glm::vec3 edge1 = v1 - v0;
  const glm::vec3 edge2 = v2 - v0;
  const glm::vec3 pvec = glm::cross(direction, edge2);
  const float determinant = glm::dot(edge1, pvec);
  if ((cullMode == TriangleCullMode::Back && determinant <= kEpsilon) ||
      (cullMode == TriangleCullMode::Front && determinant >= -kEpsilon) ||
      (cullMode == TriangleCullMode::None &&
       std::abs(determinant) <= kEpsilon)) {
    return false;
  }

  const float inverseDeterminant = 1.0f / determinant;
  const glm::vec3 tvec = origin - v0;
  const float u = glm::dot(tvec, pvec) * inverseDeterminant;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const glm::vec3 qvec = glm::cross(tvec, edge1);
  const float v = glm::dot(direction, qvec) * inverseDeterminant;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float distance = glm::dot(edge2, qvec) * inverseDeterminant;
  if (distance < 0.0f) {
    return false;
  }

  outDistance = distance;
  return true;
}

float projectDepth(const container::gpu::CameraData &cameraData,
                   const glm::vec3 &worldPosition) {
  const glm::vec4 clip = cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (clip.w == 0.0f) {
    return 0.0f;
  }
  return std::clamp(clip.z / clip.w, 0.0f, 1.0f);
}

bool sectionPlaneClips(const glm::vec3 &worldPosition, bool enabled,
                       const glm::vec4 &plane) {
  return enabled && glm::dot(glm::vec3(plane), worldPosition) + plane.w < 0.0f;
}

uint32_t accumulateSectionCapCrossing(uint32_t sectionCapCrossings,
                                      const glm::vec3 &worldPosition,
                                      const glm::vec4 &plane) {
  const float signedDistance =
      glm::dot(glm::vec3(plane), worldPosition) + plane.w;
  return sectionCapCrossings | (signedDistance >= 0.0f ? 1u : 2u);
}

bool intersectRaySectionPlane(const PickRay &ray, const glm::vec4 &plane,
                              float &outDistance) {
  constexpr float kEpsilon = 1.0e-6f;
  const glm::vec3 normal{plane};
  const float denominator = glm::dot(normal, ray.direction);
  if (std::abs(denominator) <= kEpsilon) {
    return false;
  }
  const float distance =
      -(glm::dot(normal, ray.origin) + plane.w) / denominator;
  if (distance < 0.0f || !std::isfinite(distance)) {
    return false;
  }
  outDistance = distance;
  return true;
}

bool insideSectionCapBounds(const glm::vec3 &worldPosition,
                            const BimElementBounds &bounds) {
  if (!bounds.valid) {
    return false;
  }
  const float largestExtent =
      std::max({bounds.size.x, bounds.size.y, bounds.size.z, 1.0f});
  const float tolerance = std::max(0.01f, largestExtent * 0.002f);
  return worldPosition.x >= bounds.min.x - tolerance &&
         worldPosition.x <= bounds.max.x + tolerance &&
         worldPosition.y >= bounds.min.y - tolerance &&
         worldPosition.y <= bounds.max.y + tolerance &&
         worldPosition.z >= bounds.min.z - tolerance &&
         worldPosition.z <= bounds.max.z + tolerance;
}

bool sameSectionCapBuildOptions(const BimSectionCapBuildOptions &lhs,
                                const BimSectionCapBuildOptions &rhs) {
  constexpr float kEpsilon = 1.0e-5f;
  if (glm::length(lhs.sectionPlane - rhs.sectionPlane) > kEpsilon ||
      lhs.clipPlaneCount != rhs.clipPlaneCount ||
      glm::length(lhs.fillColor - rhs.fillColor) > kEpsilon ||
      std::abs(lhs.fillOpacity - rhs.fillOpacity) > kEpsilon ||
      glm::length(lhs.hatchColor - rhs.hatchColor) > kEpsilon ||
      std::abs(lhs.hatchSpacing - rhs.hatchSpacing) > kEpsilon ||
      std::abs(lhs.hatchAngleRadians - rhs.hatchAngleRadians) > kEpsilon ||
      std::abs(lhs.capOffset - rhs.capOffset) > kEpsilon ||
      lhs.crossHatch != rhs.crossHatch ||
      lhs.invertedBoxClip != rhs.invertedBoxClip ||
      lhs.sectionMarkersEnabled != rhs.sectionMarkersEnabled ||
      glm::length(lhs.sectionMarkerColor - rhs.sectionMarkerColor) >
          kEpsilon ||
      std::abs(lhs.sectionMarkerLineWidth - rhs.sectionMarkerLineWidth) >
          kEpsilon ||
      lhs.materialStyles.size() != rhs.materialStyles.size()) {
    return false;
  }

  const uint32_t clipPlaneCount = std::min<uint32_t>(
      lhs.clipPlaneCount, static_cast<uint32_t>(lhs.clipPlanes.size()));
  for (uint32_t planeIndex = 0; planeIndex < clipPlaneCount; ++planeIndex) {
    if (glm::length(lhs.clipPlanes[planeIndex] - rhs.clipPlanes[planeIndex]) >
        kEpsilon) {
      return false;
    }
  }
  for (size_t styleIndex = 0; styleIndex < lhs.materialStyles.size();
       ++styleIndex) {
    const BimSectionCapMaterialStyle &lhsStyle =
        lhs.materialStyles[styleIndex];
    const BimSectionCapMaterialStyle &rhsStyle =
        rhs.materialStyles[styleIndex];
    if (lhsStyle.materialIndex != rhsStyle.materialIndex ||
        glm::length(lhsStyle.fillColor - rhsStyle.fillColor) > kEpsilon ||
        std::abs(lhsStyle.fillOpacity - rhsStyle.fillOpacity) > kEpsilon ||
        std::abs(lhsStyle.hatchSpacing - rhsStyle.hatchSpacing) > kEpsilon ||
        std::abs(lhsStyle.hatchAngleRadians - rhsStyle.hatchAngleRadians) >
            kEpsilon ||
        glm::length(lhsStyle.hatchColor - rhsStyle.hatchColor) > kEpsilon) {
      return false;
    }
  }
  return true;
}

container::gpu::ObjectData
makeObjectData(const glm::mat4 &transform, uint32_t materialIndex,
               bool doubleSided, glm::vec3 boundsCenter, float boundsRadius) {
  container::gpu::ObjectData object{};
  object.model = transform;
  const glm::mat3 normalMatrix =
      glm::transpose(glm::inverse(glm::mat3(transform)));
  object.normalMatrix0 = glm::vec4(normalMatrix[0], 0.0f);
  object.normalMatrix1 = glm::vec4(normalMatrix[1], 0.0f);
  object.normalMatrix2 = glm::vec4(normalMatrix[2], 0.0f);
  object.objectInfo.x = materialIndex;
  object.objectInfo.y =
      doubleSided ? container::gpu::kObjectFlagDoubleSided : 0u;
  object.objectInfo.z = container::gpu::kPickIdBimMask;

  const glm::vec3 worldCenter =
      glm::vec3(transform * glm::vec4(boundsCenter, 1.0f));
  const float scaleMax = std::max({glm::length(glm::vec3(transform[0])),
                                   glm::length(glm::vec3(transform[1])),
                                   glm::length(glm::vec3(transform[2]))});
  object.boundingSphere = glm::vec4(worldCenter, boundsRadius * scaleMax);
  return object;
}

uint32_t packColor(glm::vec4 color) {
  auto pack = [](float component) {
    return static_cast<uint32_t>(
        std::clamp(std::lround(component * 255.0f), 0l, 255l));
  };
  return pack(color.r) | (pack(color.g) << 8u) | (pack(color.b) << 16u) |
         (pack(color.a) << 24u);
}

bool isTransparentColor(const glm::vec4 &color) {
  return std::isfinite(color.a) && color.a < 0.999f;
}

constexpr uint32_t kInvalidMaterialIndex = std::numeric_limits<uint32_t>::max();

bool isValidMaterialIndex(uint32_t index,
                          const container::geometry::dotbim::Model &model) {
  return index != kInvalidMaterialIndex && index < model.materials.size();
}

bool sameVisibilityFilterSettings(const BimVisibilityFilterPushConstants &lhs,
                                  const BimVisibilityFilterPushConstants &rhs) {
  return lhs.objectCount == rhs.objectCount && lhs.flags == rhs.flags &&
         lhs.drawBudgetMaxObjects == rhs.drawBudgetMaxObjects &&
         lhs.selectedObjectIndex == rhs.selectedObjectIndex &&
         lhs.typeId == rhs.typeId && lhs.storeyId == rhs.storeyId &&
         lhs.materialId == rhs.materialId &&
         lhs.disciplineId == rhs.disciplineId && lhs.phaseId == rhs.phaseId &&
         lhs.fireRatingId == rhs.fireRatingId &&
         lhs.loadBearingId == rhs.loadBearingId &&
         lhs.statusId == rhs.statusId &&
         lhs.selectedProductId == rhs.selectedProductId;
}

bool visibilityFilterHasActiveMask(
    const BimVisibilityFilterPushConstants &settings) {
  return settings.flags != 0u;
}

BimGeometryKind
bimGeometryKind(container::geometry::dotbim::GeometryKind kind) {
  switch (kind) {
  case container::geometry::dotbim::GeometryKind::Points:
    return BimGeometryKind::Points;
  case container::geometry::dotbim::GeometryKind::Curves:
    return BimGeometryKind::Curves;
  case container::geometry::dotbim::GeometryKind::Mesh:
  default:
    return BimGeometryKind::Mesh;
  }
}

enum class DrawBucket : uint8_t {
  OpaqueSingleSided,
  OpaqueWindingFlipped,
  OpaqueDoubleSided,
  TransparentSingleSided,
  TransparentWindingFlipped,
  TransparentDoubleSided,
};

struct PendingDraw {
  container::gpu::ObjectData object{};
  BimElementMetadata metadata{};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t nativeFirstIndex{0};
  uint32_t nativeIndexCount{0};
  DrawBucket bucket{DrawBucket::OpaqueSingleSided};
};

uint8_t bucketSortKey(DrawBucket bucket) {
  return static_cast<uint8_t>(bucket);
}

uint8_t geometryKindSortKey(BimGeometryKind kind) {
  return static_cast<uint8_t>(kind);
}

void appendDrawCommand(std::vector<DrawCommand> &commands, uint32_t objectIndex,
                       uint32_t firstIndex, uint32_t indexCount,
                       bool allowMerge) {
  if (indexCount == 0u) {
    return;
  }
  if (allowMerge && !commands.empty()) {
    DrawCommand &last = commands.back();
    const uint32_t lastInstanceCount = std::max(last.instanceCount, 1u);
    if (last.firstIndex == firstIndex && last.indexCount == indexCount &&
        last.objectIndex + lastInstanceCount == objectIndex &&
        lastInstanceCount < std::numeric_limits<uint32_t>::max()) {
      last.instanceCount = lastInstanceCount + 1u;
      return;
    }
  }

  DrawCommand command{};
  command.objectIndex = objectIndex;
  command.firstIndex = firstIndex;
  command.indexCount = indexCount;
  command.instanceCount = 1u;
  commands.push_back(command);
}

void appendDrawCommandToGeometryLists(BimGeometryDrawLists &lists,
                                      DrawBucket bucket, uint32_t objectIndex,
                                      uint32_t firstIndex, uint32_t indexCount,
                                      bool allowMerge) {
  auto appendBucketCommand = [&](std::vector<DrawCommand> &commands) {
    appendDrawCommand(commands, objectIndex, firstIndex, indexCount,
                      allowMerge);
  };
  auto appendAggregateCommand = [&](std::vector<DrawCommand> &commands) {
    appendDrawCommand(commands, objectIndex, firstIndex, indexCount, false);
  };

  switch (bucket) {
  case DrawBucket::OpaqueSingleSided:
    appendAggregateCommand(lists.opaqueDrawCommands);
    appendBucketCommand(lists.opaqueSingleSidedDrawCommands);
    break;
  case DrawBucket::OpaqueWindingFlipped:
    appendAggregateCommand(lists.opaqueDrawCommands);
    appendBucketCommand(lists.opaqueWindingFlippedDrawCommands);
    break;
  case DrawBucket::OpaqueDoubleSided:
    appendAggregateCommand(lists.opaqueDrawCommands);
    appendBucketCommand(lists.opaqueDoubleSidedDrawCommands);
    break;
  case DrawBucket::TransparentSingleSided:
    appendAggregateCommand(lists.transparentDrawCommands);
    appendBucketCommand(lists.transparentSingleSidedDrawCommands);
    break;
  case DrawBucket::TransparentWindingFlipped:
    appendAggregateCommand(lists.transparentDrawCommands);
    appendBucketCommand(lists.transparentWindingFlippedDrawCommands);
    break;
  case DrawBucket::TransparentDoubleSided:
    appendAggregateCommand(lists.transparentDrawCommands);
    appendBucketCommand(lists.transparentDoubleSidedDrawCommands);
    break;
  }
}

bool hasDrawCommands(const BimGeometryDrawLists &lists) {
  return !lists.opaqueDrawCommands.empty() ||
         !lists.opaqueSingleSidedDrawCommands.empty() ||
         !lists.opaqueWindingFlippedDrawCommands.empty() ||
         !lists.opaqueDoubleSidedDrawCommands.empty() ||
         !lists.transparentDrawCommands.empty() ||
         !lists.transparentSingleSidedDrawCommands.empty() ||
         !lists.transparentWindingFlippedDrawCommands.empty() ||
         !lists.transparentDoubleSidedDrawCommands.empty();
}

bool drawCommandCoversObject(const DrawCommand &command, uint32_t objectIndex) {
  const uint32_t instanceCount = std::max(command.instanceCount, 1u);
  return objectIndex >= command.objectIndex &&
         objectIndex - command.objectIndex < instanceCount;
}

bool drawCommandListCoversObject(const std::vector<DrawCommand> &commands,
                                 uint32_t objectIndex) {
  return std::any_of(commands.begin(), commands.end(),
                     [&](const DrawCommand &command) {
                       return drawCommandCoversObject(command, objectIndex);
                     });
}

bool geometryDrawListsCoverObject(const BimGeometryDrawLists &lists,
                                  uint32_t objectIndex) {
  return drawCommandListCoversObject(lists.opaqueDrawCommands, objectIndex) ||
         drawCommandListCoversObject(lists.transparentDrawCommands,
                                     objectIndex);
}

std::optional<uint32_t> tryLoadSceneTexture(
    container::scene::SceneManager &sceneManager,
    const container::geometry::dotbim::MaterialTextureAsset &texture,
    bool isSrgb) {
  if (texture.empty()) {
    return std::nullopt;
  }

  if (!texture.encodedBytes.empty()) {
    try {
      std::string textureName = texture.name;
      if (textureName.empty() && !texture.path.empty()) {
        textureName =
            container::util::pathToUtf8(texture.path.lexically_normal());
      }
      return sceneManager.loadMaterialTextureFromBytes(
          textureName, texture.encodedBytes, isSrgb, texture.samplerIndex);
    } catch (...) {
      if (texture.path.empty()) {
        return std::nullopt;
      }
    }
  }

  const std::filesystem::path normalized = texture.path.lexically_normal();
  std::error_code existsError;
  if (!std::filesystem::exists(normalized, existsError)) {
    return std::nullopt;
  }
  try {
    return sceneManager.loadMaterialTexture(normalized, isSrgb,
                                            texture.samplerIndex);
  } catch (...) {
    return std::nullopt;
  }
}

void assignTextureIndex(
    container::scene::SceneManager &sceneManager,
    const container::geometry::dotbim::MaterialTextureAsset &texture,
    bool isSrgb, uint32_t &textureIndex) {
  if (const auto loaded = tryLoadSceneTexture(sceneManager, texture, isSrgb)) {
    textureIndex = *loaded;
  }
}

container::material::Material
makeSceneMaterial(const container::geometry::dotbim::Material &source,
                  container::scene::SceneManager &sceneManager) {
  container::material::Material material = source.pbr;
  material.baseColor =
      glm::clamp(material.baseColor, glm::vec4(0.0f), glm::vec4(1.0f));
  material.opacityFactor = std::clamp(material.opacityFactor, 0.0f, 1.0f);
  material.metallicFactor = std::clamp(material.metallicFactor, 0.0f, 1.0f);
  material.roughnessFactor = std::clamp(material.roughnessFactor, 0.0f, 1.0f);
  material.alphaCutoff = std::clamp(material.alphaCutoff, 0.0f, 1.0f);
  material.normalTextureScale = std::isfinite(material.normalTextureScale)
                                    ? material.normalTextureScale
                                    : 1.0f;
  material.occlusionStrength =
      std::clamp(material.occlusionStrength, 0.0f, 1.0f);

  assignTextureIndex(sceneManager, source.texturePaths.baseColor, true,
                     material.baseColorTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.emissive, true,
                     material.emissiveTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.normal, false,
                     material.normalTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.occlusion, false,
                     material.occlusionTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.metallicRoughness, false,
                     material.metallicRoughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.roughness, false,
                     material.roughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.metalness, false,
                     material.metalnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.specular, false,
                     material.specularTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.specularColor, true,
                     material.specularColorTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.opacity, false,
                     material.opacityTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.transmission, false,
                     material.transmissionTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.clearcoat, false,
                     material.clearcoatTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.clearcoatRoughness,
                     false, material.clearcoatRoughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.clearcoatNormal, false,
                     material.clearcoatNormalTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.sheenColor, true,
                     material.sheenColorTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.sheenRoughness, false,
                     material.sheenRoughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.iridescence, false,
                     material.iridescenceTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.iridescenceThickness,
                     false, material.iridescenceThicknessTextureIndex);
  return material;
}

bool hasRenderableSourceGeometry(
    const container::geometry::dotbim::Model &model) {
  return !model.vertices.empty() && !model.indices.empty() &&
         !model.meshRanges.empty() && !model.elements.empty();
}

size_t meshletClusterCountForRange(uint32_t indexCount) {
  constexpr uint32_t kMeshletTriangleBudget = 64u;
  const uint32_t triangleCount = indexCount / 3u;
  return triangleCount == 0u
             ? 0u
             : static_cast<size_t>(
                   (triangleCount + kMeshletTriangleBudget - 1u) /
                   kMeshletTriangleBudget);
}

size_t
meshletClusterCountForModel(const container::geometry::dotbim::Model &model) {
  if (!model.meshletClusters.empty()) {
    return model.meshletClusters.size();
  }
  std::unordered_set<uint32_t> meshGeometryIds;
  meshGeometryIds.reserve(model.elements.size());
  for (const auto &element : model.elements) {
    if (element.geometryKind ==
        container::geometry::dotbim::GeometryKind::Mesh) {
      meshGeometryIds.insert(element.meshId);
    }
  }
  size_t clusterCount = 0;
  for (const auto &range : model.meshRanges) {
    if (!meshGeometryIds.empty() && !meshGeometryIds.contains(range.meshId)) {
      continue;
    }
    clusterCount += meshletClusterCountForRange(range.indexCount);
  }
  return clusterCount;
}

uint32_t saturatingUint32(size_t value) {
  return value > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(value);
}

std::unordered_set<uint32_t>
meshGeometryIdsForModel(const container::geometry::dotbim::Model &model) {
  std::unordered_set<uint32_t> meshGeometryIds;
  meshGeometryIds.reserve(model.elements.size());
  for (const auto &element : model.elements) {
    if (element.geometryKind ==
        container::geometry::dotbim::GeometryKind::Mesh) {
      meshGeometryIds.insert(element.meshId);
    }
  }
  return meshGeometryIds;
}

std::vector<BimMeshletClusterMetadata> buildMeshletClusterMetadataForModel(
    const container::geometry::dotbim::Model &model) {
  std::vector<BimMeshletClusterMetadata> clusters;
  if (!model.meshletClusters.empty()) {
    clusters.reserve(model.meshletClusters.size());
    for (const auto &source : model.meshletClusters) {
      clusters.push_back(BimMeshletClusterMetadata{
          .meshId = source.meshId,
          .firstIndex = source.firstIndex,
          .indexCount = source.indexCount,
          .triangleCount = source.triangleCount,
          .lodLevel = source.lodLevel,
          .boundsCenter = source.boundsCenter,
          .boundsRadius = source.boundsRadius,
          .estimated = false,
      });
    }
  } else {
    const std::unordered_set<uint32_t> meshGeometryIds =
        meshGeometryIdsForModel(model);
    clusters.reserve(meshletClusterCountForModel(model));
    constexpr uint32_t kMeshletTriangleBudget = 64u;
    constexpr uint32_t kMeshletIndexBudget = kMeshletTriangleBudget * 3u;
    for (const auto &range : model.meshRanges) {
      if (!meshGeometryIds.empty() && !meshGeometryIds.contains(range.meshId)) {
        continue;
      }
      const uint32_t endIndex = range.firstIndex + range.indexCount;
      for (uint32_t firstIndex = range.firstIndex; firstIndex < endIndex;
           firstIndex += kMeshletIndexBudget) {
        const uint32_t remaining = endIndex - firstIndex;
        const uint32_t indexCount =
            std::min(kMeshletIndexBudget, remaining) / 3u * 3u;
        if (indexCount == 0u) {
          continue;
        }
        clusters.push_back(BimMeshletClusterMetadata{
            .meshId = range.meshId,
            .firstIndex = firstIndex,
            .indexCount = indexCount,
            .triangleCount = indexCount / 3u,
            .lodLevel = 0u,
            .boundsCenter = range.boundsCenter,
            .boundsRadius = range.boundsRadius,
            .estimated = true,
        });
      }
    }
  }

  std::ranges::stable_sort(clusters, [](const BimMeshletClusterMetadata &lhs,
                                        const BimMeshletClusterMetadata &rhs) {
    if (lhs.meshId != rhs.meshId) {
      return lhs.meshId < rhs.meshId;
    }
    if (lhs.lodLevel != rhs.lodLevel) {
      return lhs.lodLevel < rhs.lodLevel;
    }
    return lhs.firstIndex < rhs.firstIndex;
  });
  return clusters;
}

struct MeshletClusterSpan {
  uint32_t firstCluster{0};
  uint32_t clusterCount{0};
  uint32_t maxLodLevel{0};
  uint32_t triangleCount{0};
  bool estimatedClusters{false};
};

std::unordered_map<uint32_t, MeshletClusterSpan> meshletClusterSpansByMeshId(
    std::span<const BimMeshletClusterMetadata> clusters) {
  std::unordered_map<uint32_t, MeshletClusterSpan> spans;
  spans.reserve(clusters.size());
  for (size_t i = 0; i < clusters.size(); ++i) {
    const BimMeshletClusterMetadata &cluster = clusters[i];
    MeshletClusterSpan &span = spans[cluster.meshId];
    if (span.clusterCount == 0u) {
      span.firstCluster = saturatingUint32(i);
    }
    span.clusterCount =
        saturatingUint32(static_cast<size_t>(span.clusterCount) + 1u);
    span.maxLodLevel = std::max(span.maxLodLevel, cluster.lodLevel);
    span.triangleCount = saturatingUint32(
        static_cast<size_t>(span.triangleCount) + cluster.triangleCount);
    span.estimatedClusters = span.estimatedClusters || cluster.estimated;
  }
  return spans;
}

std::string
optimizedModelCacheKey(const std::filesystem::path &path,
                       const container::geometry::dotbim::Model &model,
                       const BimOptimizedModelMetadata &metadata) {
  std::string key = container::util::pathToUtf8(path);
  key += "|v=" + std::to_string(model.vertices.size());
  key += "|i=" + std::to_string(model.indices.size());
  key += "|o=" + std::to_string(model.elements.size());
  key += "|c=" + std::to_string(metadata.meshletClusterCount);
  key += "|slod=" + std::to_string(metadata.maxLodLevel);
  key += metadata.hasSourceMeshletClusters ? "|src" : "|estimated";
  return key;
}

std::string cacheFileStemForKey(std::string_view key) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : key) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 1099511628211ull;
  }

  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string stem(16u, '0');
  for (size_t i = 0; i < stem.size(); ++i) {
    stem[stem.size() - i - 1u] = kHexDigits[hash & 0xfu];
    hash >>= 4u;
  }
  return stem;
}

std::filesystem::path
optimizedModelMetadataCachePath(std::string_view cacheKey) {
  return container::util::executableDirectory() / "cache" /
         "bim_optimized_metadata" / (cacheFileStemForKey(cacheKey) + ".json");
}

bool optimizedModelMetadataCacheMatches(
    const Json &document, const BimOptimizedModelMetadata &metadata) {
  return document.value("schemaVersion", 0) == 1 &&
         document.value("cacheKey", std::string{}) == metadata.cacheKey &&
         document.value("meshObjectCount", size_t{0}) ==
             metadata.meshObjectCount &&
         document.value("meshletClusterCount", size_t{0}) ==
             metadata.meshletClusterCount &&
         document.value("sourceMeshletClusterCount", size_t{0}) ==
             metadata.sourceMeshletClusterCount &&
         document.value("estimatedMeshletClusterCount", size_t{0}) ==
             metadata.estimatedMeshletClusterCount &&
         document.value("objectClusterReferenceCount", size_t{0}) ==
             metadata.objectClusterReferenceCount &&
         document.value("maxLodLevel", uint32_t{0}) == metadata.maxLodLevel;
}

enum class OptimizedModelMetadataCacheProbe {
  Missing,
  Hit,
  Stale,
};

OptimizedModelMetadataCacheProbe
probeOptimizedModelMetadataCache(const BimOptimizedModelMetadata &metadata) {
  if (!metadata.cacheable || metadata.cachePath.empty()) {
    return OptimizedModelMetadataCacheProbe::Missing;
  }
  std::ifstream input(container::util::pathFromUtf8(metadata.cachePath));
  if (!input) {
    return OptimizedModelMetadataCacheProbe::Missing;
  }
  try {
    const Json document = Json::parse(input, nullptr, true, true);
    return optimizedModelMetadataCacheMatches(document, metadata)
               ? OptimizedModelMetadataCacheProbe::Hit
               : OptimizedModelMetadataCacheProbe::Stale;
  } catch (...) {
    return OptimizedModelMetadataCacheProbe::Stale;
  }
}

bool writeOptimizedModelMetadataCache(
    const BimOptimizedModelMetadata &metadata,
    std::span<const BimObjectLodStreamingMetadata> objectLodMetadata) {
  if (!metadata.cacheable || metadata.cachePath.empty()) {
    return false;
  }
  const std::filesystem::path cachePath =
      container::util::pathFromUtf8(metadata.cachePath);
  std::error_code error;
  std::filesystem::create_directories(cachePath.parent_path(), error);
  if (error) {
    return false;
  }

  size_t residentCandidateObjects = 0;
  size_t residentCandidateClusters = 0;
  for (const BimObjectLodStreamingMetadata &lod : objectLodMetadata) {
    if (lod.geometryKind != BimGeometryKind::Mesh || lod.clusterCount == 0u) {
      continue;
    }
    ++residentCandidateObjects;
    residentCandidateClusters += lod.clusterCount;
  }

  Json document = {
      {"schemaVersion", 1},
      {"cacheKey", metadata.cacheKey},
      {"meshObjectCount", metadata.meshObjectCount},
      {"meshletClusterCount", metadata.meshletClusterCount},
      {"sourceMeshletClusterCount", metadata.sourceMeshletClusterCount},
      {"estimatedMeshletClusterCount", metadata.estimatedMeshletClusterCount},
      {"objectClusterReferenceCount", metadata.objectClusterReferenceCount},
      {"maxLodLevel", metadata.maxLodLevel},
      {"hasSourceMeshletClusters", metadata.hasSourceMeshletClusters},
      {"residentCandidateObjects", residentCandidateObjects},
      {"residentCandidateClusters", residentCandidateClusters},
  };

  std::ofstream output(cachePath, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << document.dump(2);
  return static_cast<bool>(output);
}

void refreshOptimizedModelMetadataCache(
    BimOptimizedModelMetadata &metadata,
    std::span<const BimObjectLodStreamingMetadata> objectLodMetadata) {
  metadata.cacheHit = false;
  metadata.cacheStale = false;
  metadata.cacheWriteAttempted = false;
  metadata.cacheWriteSucceeded = false;
  metadata.cacheStatus = metadata.cacheable ? "miss" : "disabled";
  if (!metadata.cacheable || metadata.cachePath.empty()) {
    return;
  }

  const OptimizedModelMetadataCacheProbe probe =
      probeOptimizedModelMetadataCache(metadata);
  if (probe == OptimizedModelMetadataCacheProbe::Hit) {
    metadata.cacheHit = true;
    metadata.cacheStatus = "hit";
    return;
  }

  metadata.cacheStale = probe == OptimizedModelMetadataCacheProbe::Stale;
  metadata.cacheWriteAttempted = true;
  metadata.cacheWriteSucceeded =
      writeOptimizedModelMetadataCache(metadata, objectLodMetadata);
  if (metadata.cacheWriteSucceeded) {
    metadata.cacheStatus =
        metadata.cacheStale ? "stale refreshed" : "miss written";
  } else {
    metadata.cacheStatus =
        metadata.cacheStale ? "stale write failed" : "miss write failed";
  }
}

BimOptimizedModelMetadata buildOptimizedModelMetadata(
    const std::filesystem::path &path,
    const container::geometry::dotbim::Model &model,
    std::span<const BimMeshletClusterMetadata> clusters) {
  BimOptimizedModelMetadata metadata{};
  metadata.cacheable = !path.empty() && !model.vertices.empty() &&
                       !model.indices.empty() && !clusters.empty();
  metadata.hasSourceMeshletClusters = !model.meshletClusters.empty();
  metadata.meshletClusterCount = clusters.size();
  for (const BimMeshletClusterMetadata &cluster : clusters) {
    if (cluster.estimated) {
      ++metadata.estimatedMeshletClusterCount;
    } else {
      ++metadata.sourceMeshletClusterCount;
    }
    metadata.maxLodLevel = std::max(metadata.maxLodLevel, cluster.lodLevel);
  }
  for (const auto &element : model.elements) {
    if (element.geometryKind ==
        container::geometry::dotbim::GeometryKind::Mesh) {
      ++metadata.meshObjectCount;
    }
  }
  metadata.cacheKey = optimizedModelCacheKey(path, model, metadata);
  if (metadata.cacheable) {
    metadata.cachePath = container::util::pathToUtf8(
        optimizedModelMetadataCachePath(metadata.cacheKey));
  }
  return metadata;
}

std::string modelLoadErrorPrefix(std::string_view format,
                                 const std::filesystem::path &path) {
  return std::string(format) + " file has no supported BIM geometry: " +
         container::util::pathToUtf8(path);
}

std::optional<std::filesystem::path>
preparedIfcSidecarPath(const std::filesystem::path &path) {
  if (lowerAscii(path.extension().string()) != ".ifc") {
    return std::nullopt;
  }

  std::filesystem::path sidecar = path;
  sidecar.replace_extension(".ifcx");
  std::error_code error;
  if (std::filesystem::is_regular_file(sidecar, error)) {
    return sidecar;
  }
  return std::nullopt;
}

struct BimFloorPlanBuildResult {
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};

  [[nodiscard]] bool valid() const { return indexCount >= 2u; }
};

struct FloorPlanPointKey {
  int64_t x{0};
  int64_t y{0};
  int64_t z{0};

  [[nodiscard]] bool operator==(const FloorPlanPointKey &other) const {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct FloorPlanPointKeyHash {
  [[nodiscard]] size_t operator()(const FloorPlanPointKey &key) const {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
      hash ^= value;
      hash *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(key.x));
    mix(static_cast<uint64_t>(key.y));
    mix(static_cast<uint64_t>(key.z));
    return static_cast<size_t>(hash);
  }
};

struct FloorPlanEdgeKey {
  uint32_t a{0};
  uint32_t b{0};

  [[nodiscard]] bool operator==(const FloorPlanEdgeKey &other) const {
    return a == other.a && b == other.b;
  }
};

struct FloorPlanEdgeKeyHash {
  [[nodiscard]] size_t operator()(const FloorPlanEdgeKey &key) const {
    return (static_cast<size_t>(key.a) << 32u) ^ static_cast<size_t>(key.b);
  }
};

[[nodiscard]] bool isFiniteVec3(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool isFiniteMat4(const glm::mat4 &value) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(value[column][row])) {
        return false;
      }
    }
  }
  return true;
}

void hashCombine(size_t &seed, size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
}

[[nodiscard]] size_t quantizedFloatHash(float value) {
  return std::hash<int>{}(static_cast<int>(std::round(value * 1000.0f)));
}

[[nodiscard]] size_t coordinationMarkerGeometrySignature(
    const BimCoordinationOverlayResult &overlay, float markerRadius,
    uint64_t objectDataRevision) {
  size_t signature = std::hash<size_t>{}(overlay.markers.size());
  hashCombine(signature, std::hash<uint64_t>{}(objectDataRevision));
  hashCombine(signature, quantizedFloatHash(markerRadius));
  for (const BimCoordinationOverlayMarker &marker : overlay.markers) {
    hashCombine(signature,
                std::hash<uint32_t>{}(static_cast<uint32_t>(marker.kind)));
    hashCombine(signature, quantizedFloatHash(marker.position.x));
    hashCombine(signature, quantizedFloatHash(marker.position.y));
    hashCombine(signature, quantizedFloatHash(marker.position.z));
    hashCombine(signature, quantizedFloatHash(marker.color.x));
    hashCombine(signature, quantizedFloatHash(marker.color.y));
    hashCombine(signature, quantizedFloatHash(marker.color.z));
    hashCombine(signature, std::hash<uint32_t>{}(marker.primaryObjectIndex));
    hashCombine(signature, std::hash<uint32_t>{}(marker.secondaryObjectIndex));
    hashCombine(signature, std::hash<std::string>{}(marker.label));
    hashCombine(signature, std::hash<std::string>{}(marker.ifcGuid));
    hashCombine(signature, std::hash<std::string>{}(marker.sourceId));
  }
  return signature;
}

[[nodiscard]] size_t sectionPlaneVisualGeometrySignature(
    const glm::vec4 &plane, float size, const glm::vec3 &color,
    uint64_t objectDataRevision) {
  size_t signature = 0u;
  hashCombine(signature, std::hash<uint64_t>{}(objectDataRevision));
  hashCombine(signature, quantizedFloatHash(plane.x));
  hashCombine(signature, quantizedFloatHash(plane.y));
  hashCombine(signature, quantizedFloatHash(plane.z));
  hashCombine(signature, quantizedFloatHash(plane.w));
  hashCombine(signature, quantizedFloatHash(size));
  hashCombine(signature, quantizedFloatHash(color.x));
  hashCombine(signature, quantizedFloatHash(color.y));
  hashCombine(signature, quantizedFloatHash(color.z));
  return signature;
}

void includeBoundsPoint(const glm::vec3 &point, glm::vec3 &boundsMin,
                        glm::vec3 &boundsMax, bool &hasBounds) {
  if (!isFiniteVec3(point)) {
    return;
  }
  if (!hasBounds) {
    boundsMin = point;
    boundsMax = point;
    hasBounds = true;
    return;
  }
  boundsMin = glm::min(boundsMin, point);
  boundsMax = glm::max(boundsMax, point);
}

void includeBoundsBox(const BimElementBounds &bounds, glm::vec3 &boundsMin,
                      glm::vec3 &boundsMax, bool &hasBounds) {
  if (!bounds.valid || !isFiniteVec3(bounds.min) || !isFiniteVec3(bounds.max)) {
    return;
  }
  includeBoundsPoint(bounds.min, boundsMin, boundsMax, hasBounds);
  includeBoundsPoint(bounds.max, boundsMin, boundsMax, hasBounds);
}

[[nodiscard]] bool
stringContainsAny(std::string_view value,
                  std::initializer_list<std::string_view> needles) {
  for (std::string_view needle : needles) {
    if (value.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool elementContributesToFloorPlan(
    const container::geometry::dotbim::Element &element) {
  if (element.geometryKind != container::geometry::dotbim::GeometryKind::Mesh) {
    return false;
  }
  std::string label = element.type;
  label += ' ';
  label += element.objectType;
  label += ' ';
  label += element.displayName;
  label = lowerAscii(std::move(label));
  if (stringContainsAny(label, {"roof", "covering", "terrain", "site"})) {
    return false;
  }
  return true;
}

BimFloorPlanBuildResult appendFloorPlanOverlayGeometry(
    const container::geometry::dotbim::Model &model,
    std::vector<container::geometry::Vertex> &vertices,
    std::vector<uint32_t> &indices, bool preserveElevation) {
  constexpr double kQuantizationScale = 1000.0;
  constexpr float kVerticalFaceMaxUp = 0.55f;
  constexpr float kMinSegmentLength2 = 0.000001f;
  constexpr float kMinTriangleArea2 = 0.00000001f;

  BimFloorPlanBuildResult result{};
  result.firstIndex = static_cast<uint32_t>(
      std::min<size_t>(indices.size(), std::numeric_limits<uint32_t>::max()));
  const size_t firstVertex = vertices.size();
  const size_t firstIndex = indices.size();

  std::unordered_map<uint32_t, container::geometry::dotbim::MeshRange>
      rangesByMeshId;
  rangesByMeshId.reserve(model.meshRanges.size());
  for (const auto &range : model.meshRanges) {
    rangesByMeshId.emplace(range.meshId, range);
  }

  auto floorPlanStoreyLabel =
      [](const container::geometry::dotbim::Element &element) {
        return !element.storeyName.empty() ? element.storeyName
                                           : element.storeyId;
      };
  auto computeElementBaseElevation =
      [&](const container::geometry::dotbim::Element &element,
          const container::geometry::dotbim::MeshRange &range)
      -> std::optional<float> {
    if (range.firstIndex >= model.indices.size()) {
      return std::nullopt;
    }
    const size_t begin = range.firstIndex;
    const size_t end = std::min(model.indices.size(),
                                begin + static_cast<size_t>(range.indexCount));
    float minY = std::numeric_limits<float>::max();
    bool found = false;
    for (size_t index = begin; index < end; ++index) {
      const uint32_t vertexIndex = model.indices[index];
      if (vertexIndex >= model.vertices.size()) {
        continue;
      }
      const glm::vec3 worldPosition =
          glm::vec3(element.transform *
                    glm::vec4(model.vertices[vertexIndex].position, 1.0f));
      if (!isFiniteVec3(worldPosition)) {
        continue;
      }
      minY = std::min(minY, worldPosition.y);
      found = true;
    }
    if (!found) {
      return std::nullopt;
    }
    return minY;
  };

  std::vector<std::optional<float>> elementBaseElevations;
  std::unordered_map<std::string, float> storeyBaseElevations;
  if (preserveElevation) {
    elementBaseElevations.resize(model.elements.size());
    storeyBaseElevations.reserve(model.elements.size());
    for (size_t elementIndex = 0; elementIndex < model.elements.size();
         ++elementIndex) {
      const auto &element = model.elements[elementIndex];
      if (!elementContributesToFloorPlan(element)) {
        continue;
      }
      const auto rangeIt = rangesByMeshId.find(element.meshId);
      if (rangeIt == rangesByMeshId.end() || rangeIt->second.indexCount == 0u) {
        continue;
      }
      const std::optional<float> baseElevation =
          computeElementBaseElevation(element, rangeIt->second);
      if (!baseElevation.has_value()) {
        continue;
      }
      elementBaseElevations[elementIndex] = baseElevation;
      const std::string storeyLabel = floorPlanStoreyLabel(element);
      if (storeyLabel.empty()) {
        continue;
      }
      const auto [storeyIt, inserted] =
          storeyBaseElevations.try_emplace(storeyLabel, *baseElevation);
      if (!inserted) {
        storeyIt->second = std::min(storeyIt->second, *baseElevation);
      }
    }
  }

  std::unordered_map<FloorPlanPointKey, uint32_t, FloorPlanPointKeyHash>
      pointIndices;
  std::unordered_set<FloorPlanEdgeKey, FloorPlanEdgeKeyHash> edges;
  pointIndices.reserve(model.vertices.size());
  edges.reserve(model.indices.size() / 2u);

  glm::vec3 boundsMin(std::numeric_limits<float>::max());
  glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
  auto addProjectedPoint = [&](const glm::vec3 &worldPosition,
                               float elevation) -> uint32_t {
    const double projectedY =
        preserveElevation ? static_cast<double>(elevation) : 0.0;
    const FloorPlanPointKey key{
        static_cast<int64_t>(std::llround(static_cast<double>(worldPosition.x) *
                                          kQuantizationScale)),
        static_cast<int64_t>(std::llround(projectedY * kQuantizationScale)),
        static_cast<int64_t>(std::llround(static_cast<double>(worldPosition.z) *
                                          kQuantizationScale))};
    const auto existing = pointIndices.find(key);
    if (existing != pointIndices.end()) {
      return existing->second;
    }

    container::geometry::Vertex vertex{};
    vertex.position = {
        static_cast<float>(static_cast<double>(key.x) / kQuantizationScale),
        static_cast<float>(static_cast<double>(key.y) / kQuantizationScale),
        static_cast<float>(static_cast<double>(key.z) / kQuantizationScale)};
    vertex.normal = {0.0f, 1.0f, 0.0f};
    vertex.color = {1.0f, 1.0f, 1.0f};
    const uint32_t vertexIndex = static_cast<uint32_t>(std::min<size_t>(
        vertices.size(), std::numeric_limits<uint32_t>::max()));
    vertices.push_back(vertex);
    pointIndices.emplace(key, vertexIndex);
    boundsMin = glm::min(boundsMin, vertex.position);
    boundsMax = glm::max(boundsMax, vertex.position);
    return vertexIndex;
  };

  auto addProjectedSegment = [&](const glm::vec3 &a, const glm::vec3 &b,
                                 float elevation) {
    const glm::vec2 delta{a.x - b.x, a.z - b.z};
    if (glm::dot(delta, delta) <= kMinSegmentLength2) {
      return;
    }
    uint32_t ia = addProjectedPoint(a, elevation);
    uint32_t ib = addProjectedPoint(b, elevation);
    if (ia == ib) {
      return;
    }
    if (ia > ib) {
      std::swap(ia, ib);
    }
    const FloorPlanEdgeKey edge{ia, ib};
    if (!edges.insert(edge).second) {
      return;
    }
    indices.push_back(ia);
    indices.push_back(ib);
  };

  for (size_t elementIndex = 0; elementIndex < model.elements.size();
       ++elementIndex) {
    const auto &element = model.elements[elementIndex];
    if (!elementContributesToFloorPlan(element)) {
      continue;
    }
    const auto rangeIt = rangesByMeshId.find(element.meshId);
    if (rangeIt == rangesByMeshId.end() || rangeIt->second.indexCount < 3u ||
        rangeIt->second.firstIndex >= model.indices.size()) {
      continue;
    }

    const auto &range = rangeIt->second;
    float overlayElevation = 0.0f;
    if (preserveElevation) {
      const std::string storeyLabel = floorPlanStoreyLabel(element);
      const auto storeyIt = storeyBaseElevations.find(storeyLabel);
      if (storeyIt != storeyBaseElevations.end()) {
        overlayElevation = storeyIt->second;
      } else if (elementIndex < elementBaseElevations.size() &&
                 elementBaseElevations[elementIndex].has_value()) {
        overlayElevation = *elementBaseElevations[elementIndex];
      }
    }
    const size_t begin = range.firstIndex;
    const size_t end = std::min(model.indices.size(),
                                begin + static_cast<size_t>(range.indexCount));
    const float edgeVerticalTolerance =
        std::max(0.03f, range.boundsRadius * 0.0025f);
    for (size_t index = begin; index + 2u < end; index += 3u) {
      const uint32_t i0 = model.indices[index];
      const uint32_t i1 = model.indices[index + 1u];
      const uint32_t i2 = model.indices[index + 2u];
      if (i0 >= model.vertices.size() || i1 >= model.vertices.size() ||
          i2 >= model.vertices.size()) {
        continue;
      }

      const glm::vec3 v0 = glm::vec3(
          element.transform * glm::vec4(model.vertices[i0].position, 1.0f));
      const glm::vec3 v1 = glm::vec3(
          element.transform * glm::vec4(model.vertices[i1].position, 1.0f));
      const glm::vec3 v2 = glm::vec3(
          element.transform * glm::vec4(model.vertices[i2].position, 1.0f));
      if (!isFiniteVec3(v0) || !isFiniteVec3(v1) || !isFiniteVec3(v2)) {
        continue;
      }

      const glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
      const float normalLength2 = glm::dot(normal, normal);
      if (normalLength2 <= kMinTriangleArea2) {
        continue;
      }
      const glm::vec3 unitNormal = normal * (1.0f / std::sqrt(normalLength2));
      if (std::abs(unitNormal.y) > kVerticalFaceMaxUp) {
        continue;
      }

      auto addHorizontalEdge = [&](const glm::vec3 &a, const glm::vec3 &b) {
        if (std::abs(a.y - b.y) > edgeVerticalTolerance) {
          return;
        }
        addProjectedSegment(a, b, overlayElevation);
      };
      addHorizontalEdge(v0, v1);
      addHorizontalEdge(v1, v2);
      addHorizontalEdge(v2, v0);
    }
  }

  result.indexCount = static_cast<uint32_t>(std::min<size_t>(
      indices.size() - firstIndex, std::numeric_limits<uint32_t>::max()));
  if (!result.valid()) {
    vertices.resize(firstVertex);
    indices.resize(firstIndex);
    result.indexCount = 0u;
    return result;
  }

  result.boundsCenter = (boundsMin + boundsMax) * 0.5f;
  const glm::vec3 extent = boundsMax - result.boundsCenter;
  result.boundsRadius = std::max(glm::length(extent), 0.001f);
  return result;
}

} // namespace

namespace detail {

[[nodiscard]] size_t bimCoordinationMarkerGeometrySignatureForTesting(
    const BimCoordinationOverlayResult &overlay, float markerRadius,
    uint64_t objectDataRevision) {
  return coordinationMarkerGeometrySignature(overlay, markerRadius,
                                             objectDataRevision);
}

[[nodiscard]] size_t bimSectionPlaneVisualGeometrySignatureForTesting(
    const glm::vec4 &plane, float size, const glm::vec3 &color,
    uint64_t objectDataRevision) {
  return sectionPlaneVisualGeometrySignature(plane, size, color,
                                             objectDataRevision);
}

} // namespace detail

BimManager::BimManager(std::shared_ptr<container::gpu::VulkanDevice> device,
                       container::gpu::AllocationManager &allocationManager,
                       container::gpu::PipelineManager &pipelineManager)
    : device_(std::move(device)), allocationManager_(allocationManager),
      pipelineManager_(pipelineManager),
      metadataCatalog_(std::make_unique<BimMetadataCatalog>()),
      metadataIndex_(std::make_unique<BimMetadataIndex>()),
      drawFilterState_(std::make_unique<BimDrawFilterState>()) {}

BimManager::~BimManager() {
  clear();
  destroyDrawCompactionComputeResources();
  destroyVisibilityFilterComputeResources();
  destroyMeshletResidencyComputeResources();
}

void BimManager::clear() {
  if (vertexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(vertexBuffer_);
  }
  if (indexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(indexBuffer_);
  }
  if (objectBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(objectBuffer_);
  }
  clearSectionClipCapGeometry();
  clearCoordinationMarkerGeometry();
  clearSectionPlaneVisualGeometry();
  objectBufferCapacity_ = 0;
  vertexSlice_ = {};
  indexSlice_ = {};
  vertices_.clear();
  indices_.clear();
  objectData_.clear();
  elementMetadata_.clear();
  relationshipGraph_.clear();
  objectDrawCommands_.clear();
  objectDrawCommandOffsets_.clear();
  objectDrawCommandCounts_.clear();
  metadataIndex_->clear();
  metadataCatalog_->clear();
  drawFilterState_->clear();
  opaqueDrawCommands_.clear();
  opaqueSingleSidedDrawCommands_.clear();
  opaqueWindingFlippedDrawCommands_.clear();
  opaqueDoubleSidedDrawCommands_.clear();
  transparentDrawCommands_.clear();
  transparentSingleSidedDrawCommands_.clear();
  transparentWindingFlippedDrawCommands_.clear();
  transparentDoubleSidedDrawCommands_.clear();
  pointDrawLists_.clear();
  curveDrawLists_.clear();
  nativePointDrawLists_.clear();
  nativeCurveDrawLists_.clear();
  meshletClusterCount_ = 0;
  meshletClusters_.clear();
  objectLodMetadata_.clear();
  optimizedModelMetadata_ = {};
  destroyMeshletResidencyBuffers();
  destroyVisibilityFilterBuffers();
  destroyDrawCompactionBuffers();
  floorPlanGround_ = {};
  floorPlanSourceElevation_ = {};
  semanticColorMode_ = BimSemanticColorMode::Off;
  semanticColorIdsDirty_ = true;
  modelPath_.clear();
  ++objectDataRevision_;
}

bool BimManager::hasScene() const {
  return vertexSlice_.buffer != VK_NULL_HANDLE &&
         indexSlice_.buffer != VK_NULL_HANDLE &&
         objectBuffer_.buffer != VK_NULL_HANDLE && !objectData_.empty() &&
         (!opaqueDrawCommands_.empty() ||
          !opaqueSingleSidedDrawCommands_.empty() ||
          !opaqueWindingFlippedDrawCommands_.empty() ||
          !opaqueDoubleSidedDrawCommands_.empty() ||
          !transparentDrawCommands_.empty() ||
          !transparentSingleSidedDrawCommands_.empty() ||
          !transparentWindingFlippedDrawCommands_.empty() ||
          !transparentDoubleSidedDrawCommands_.empty() ||
          hasDrawCommands(pointDrawLists_) ||
          hasDrawCommands(curveDrawLists_) ||
          hasDrawCommands(nativePointDrawLists_) ||
          hasDrawCommands(nativeCurveDrawLists_));
}

VkDeviceSize BimManager::objectBufferSize() const {
  return static_cast<VkDeviceSize>(sizeof(container::gpu::ObjectData) *
                                   objectBufferCapacity_);
}

void BimManager::clearSectionClipCapGeometry() {
  if (sectionClipCapVertexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(sectionClipCapVertexBuffer_);
  }
  if (sectionClipCapIndexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(sectionClipCapIndexBuffer_);
  }
  sectionClipCapDrawData_ = {};
  sectionClipCapVertices_.clear();
  sectionClipCapIndices_.clear();
  sectionClipCapBuildOptionsValid_ = false;
}

void BimManager::clearCoordinationMarkerGeometry() {
  if (coordinationMarkerVertexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(coordinationMarkerVertexBuffer_);
  }
  if (coordinationMarkerIndexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(coordinationMarkerIndexBuffer_);
  }
  coordinationMarkerDrawData_ = {};
  coordinationMarkerVertices_.clear();
  coordinationMarkerIndices_.clear();
  coordinationMarkerGeometrySignature_ = 0u;
}

void BimManager::clearSectionPlaneVisualGeometry() {
  if (sectionPlaneVisualVertexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(sectionPlaneVisualVertexBuffer_);
  }
  if (sectionPlaneVisualIndexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(sectionPlaneVisualIndexBuffer_);
  }
  sectionPlaneVisualDrawData_ = {};
  sectionPlaneVisualVertices_.clear();
  sectionPlaneVisualIndices_.clear();
  sectionPlaneVisualGeometrySignature_ = 0u;
}

bool BimManager::rebuildCoordinationMarkerGeometry(
    const BimCoordinationOverlayResult &overlay, float markerRadius) {
  if (overlay.markers.empty() || objectData_.empty()) {
    clearCoordinationMarkerGeometry();
    return false;
  }

  const size_t signature =
      coordinationMarkerGeometrySignature(overlay, markerRadius,
                                          objectDataRevision_);
  if (coordinationMarkerGeometrySignature_ == signature &&
      coordinationMarkerDrawData_.valid()) {
    return true;
  }

  clearCoordinationMarkerGeometry();

  const auto primitives =
      buildBimCoordinationOverlayMarkerWirePrimitives(overlay.markers,
                                                      markerRadius);
  auto markerObjectIndex =
      [this](const BimCoordinationOverlayMarkerWirePrimitive &primitive) {
        if (primitive.primaryObjectIndex < objectData_.size()) {
          return primitive.primaryObjectIndex;
        }
        if (primitive.secondaryObjectIndex < objectData_.size()) {
          return primitive.secondaryObjectIndex;
        }
        return 0u;
      };
  auto transformToObjectLocal = [this](uint32_t objectIndex,
                                       const glm::vec3 &worldPosition) {
    if (objectIndex >= objectData_.size()) {
      return std::optional<glm::vec3>{};
    }
    const glm::mat4 inverseModel = glm::inverse(objectData_[objectIndex].model);
    if (!isFiniteMat4(inverseModel)) {
      return std::optional<glm::vec3>{};
    }
    const glm::vec3 localPosition =
        glm::vec3(inverseModel * glm::vec4(worldPosition, 1.0f));
    if (!isFiniteVec3(localPosition)) {
      return std::optional<glm::vec3>{};
    }
    return std::optional<glm::vec3>{localPosition};
  };

  for (const BimCoordinationOverlayMarkerWirePrimitive &primitive :
       primitives) {
    const uint32_t objectIndex = markerObjectIndex(primitive);
    const uint32_t baseVertex = static_cast<uint32_t>(std::min<size_t>(
        coordinationMarkerVertices_.size(),
        std::numeric_limits<uint32_t>::max()));
    const uint32_t baseIndex = static_cast<uint32_t>(std::min<size_t>(
        coordinationMarkerIndices_.size(),
        std::numeric_limits<uint32_t>::max()));

    bool primitiveValid = !primitive.positions.empty() &&
                          !primitive.indices.empty();
    for (const glm::vec3 &worldPosition : primitive.positions) {
      const std::optional<glm::vec3> localPosition =
          transformToObjectLocal(objectIndex, worldPosition);
      if (!localPosition) {
        primitiveValid = false;
        break;
      }
      container::geometry::Vertex vertex{};
      vertex.position = *localPosition;
      vertex.color = primitive.color;
      coordinationMarkerVertices_.push_back(vertex);
    }
    if (!primitiveValid) {
      coordinationMarkerVertices_.resize(baseVertex);
      continue;
    }

    for (uint32_t index : primitive.indices) {
      coordinationMarkerIndices_.push_back(baseVertex + index);
    }
    DrawCommand command{.objectIndex = objectIndex,
                        .firstIndex = baseIndex,
                        .indexCount = static_cast<uint32_t>(
                            coordinationMarkerIndices_.size() - baseIndex),
                        .instanceCount = 1u};
    if (primitive.kind == BimCoordinationOverlayKind::Clash) {
      coordinationMarkerDrawData_.clashDrawCommands.push_back(command);
    } else if (primitive.kind == BimCoordinationOverlayKind::IssuePin) {
      coordinationMarkerDrawData_.issuePinDrawCommands.push_back(command);
    }
  }

  if (coordinationMarkerVertices_.empty() ||
      coordinationMarkerIndices_.empty()) {
    clearCoordinationMarkerGeometry();
    return false;
  }

  const VkDeviceSize vertexBufferSize = static_cast<VkDeviceSize>(
      sizeof(container::geometry::Vertex) * coordinationMarkerVertices_.size());
  const VkDeviceSize indexBufferSize = static_cast<VkDeviceSize>(
      sizeof(uint32_t) * coordinationMarkerIndices_.size());
  coordinationMarkerVertexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(coordinationMarkerVertices_.data()),
       static_cast<size_t>(vertexBufferSize)},
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  coordinationMarkerIndexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(coordinationMarkerIndices_.data()),
       static_cast<size_t>(indexBufferSize)},
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  coordinationMarkerDrawData_.vertexSlice = container::gpu::BufferSlice{
      coordinationMarkerVertexBuffer_.buffer, 0, vertexBufferSize};
  coordinationMarkerDrawData_.indexSlice = container::gpu::BufferSlice{
      coordinationMarkerIndexBuffer_.buffer, 0, indexBufferSize};
  coordinationMarkerDrawData_.indexType = VK_INDEX_TYPE_UINT32;
  coordinationMarkerGeometrySignature_ = signature;
  return coordinationMarkerDrawData_.valid();
}

bool BimManager::rebuildSectionPlaneVisualGeometry(glm::vec4 plane, float size,
                                                   glm::vec3 color) {
  if (objectData_.empty()) {
    clearSectionPlaneVisualGeometry();
    return false;
  }

  glm::vec3 normal{plane.x, plane.y, plane.z};
  const float normalLength = glm::length(normal);
  if (!std::isfinite(normalLength) || normalLength <= 0.0001f ||
      !std::isfinite(plane.w)) {
    clearSectionPlaneVisualGeometry();
    return false;
  }
  normal /= normalLength;
  plane = glm::vec4(normal, plane.w / normalLength);
  size = std::clamp(size, 0.1f, 100000.0f);
  if (!isFiniteVec3(color)) {
    color = {0.12f, 0.62f, 1.0f};
  }

  const size_t signature =
      sectionPlaneVisualGeometrySignature(plane, size, color,
                                          objectDataRevision_);
  if (sectionPlaneVisualGeometrySignature_ == signature &&
      sectionPlaneVisualDrawData_.valid()) {
    return true;
  }

  clearSectionPlaneVisualGeometry();

  const glm::vec3 center = -normal * plane.w;
  const glm::vec3 helper =
      std::abs(normal.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                : glm::vec3{1.0f, 0.0f, 0.0f};
  const glm::vec3 tangent = glm::normalize(glm::cross(helper, normal));
  const glm::vec3 bitangent = glm::normalize(glm::cross(normal, tangent));
  if (!isFiniteVec3(center) || !isFiniteVec3(tangent) ||
      !isFiniteVec3(bitangent)) {
    clearSectionPlaneVisualGeometry();
    return false;
  }

  const uint32_t objectIndex = 0u;
  const glm::mat4 inverseModel = glm::inverse(objectData_[objectIndex].model);
  if (!isFiniteMat4(inverseModel)) {
    clearSectionPlaneVisualGeometry();
    return false;
  }

  constexpr uint32_t kGridSegments = 12u;
  sectionPlaneVisualVertices_.reserve(
      static_cast<size_t>(kGridSegments + 1u) *
      static_cast<size_t>(kGridSegments + 1u));
  sectionPlaneVisualIndices_.reserve(static_cast<size_t>(kGridSegments) *
                                     static_cast<size_t>(kGridSegments) * 6u);
  const float halfSize = size * 0.5f;
  for (uint32_t y = 0u; y <= kGridSegments; ++y) {
    const float v = static_cast<float>(y) / static_cast<float>(kGridSegments);
    for (uint32_t x = 0u; x <= kGridSegments; ++x) {
      const float u = static_cast<float>(x) / static_cast<float>(kGridSegments);
      const glm::vec3 worldPosition =
          center + tangent * ((u * 2.0f - 1.0f) * halfSize) +
          bitangent * ((v * 2.0f - 1.0f) * halfSize);
      const glm::vec3 localPosition =
          glm::vec3(inverseModel * glm::vec4(worldPosition, 1.0f));
      if (!isFiniteVec3(localPosition)) {
        clearSectionPlaneVisualGeometry();
        return false;
      }
      container::geometry::Vertex vertex{};
      vertex.position = localPosition;
      vertex.normal = normal;
      vertex.color = color;
      sectionPlaneVisualVertices_.push_back(vertex);
    }
  }

  const uint32_t rowStride = kGridSegments + 1u;
  for (uint32_t y = 0u; y < kGridSegments; ++y) {
    for (uint32_t x = 0u; x < kGridSegments; ++x) {
      const uint32_t i0 = y * rowStride + x;
      const uint32_t i1 = i0 + 1u;
      const uint32_t i2 = i0 + rowStride;
      const uint32_t i3 = i2 + 1u;
      sectionPlaneVisualIndices_.push_back(i0);
      sectionPlaneVisualIndices_.push_back(i2);
      sectionPlaneVisualIndices_.push_back(i1);
      sectionPlaneVisualIndices_.push_back(i1);
      sectionPlaneVisualIndices_.push_back(i2);
      sectionPlaneVisualIndices_.push_back(i3);
    }
  }

  const VkDeviceSize vertexBufferSize = static_cast<VkDeviceSize>(
      sizeof(container::geometry::Vertex) * sectionPlaneVisualVertices_.size());
  const VkDeviceSize indexBufferSize = static_cast<VkDeviceSize>(
      sizeof(uint32_t) * sectionPlaneVisualIndices_.size());
  sectionPlaneVisualVertexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(sectionPlaneVisualVertices_.data()),
       static_cast<size_t>(vertexBufferSize)},
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  sectionPlaneVisualIndexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(sectionPlaneVisualIndices_.data()),
       static_cast<size_t>(indexBufferSize)},
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  sectionPlaneVisualDrawData_.vertexSlice = container::gpu::BufferSlice{
      sectionPlaneVisualVertexBuffer_.buffer, 0, vertexBufferSize};
  sectionPlaneVisualDrawData_.indexSlice = container::gpu::BufferSlice{
      sectionPlaneVisualIndexBuffer_.buffer, 0, indexBufferSize};
  sectionPlaneVisualDrawData_.indexType = VK_INDEX_TYPE_UINT32;
  sectionPlaneVisualDrawData_.drawCommands.push_back(
      DrawCommand{.objectIndex = objectIndex,
                  .firstIndex = 0u,
                  .indexCount = static_cast<uint32_t>(
                      std::min<size_t>(sectionPlaneVisualIndices_.size(),
                                       std::numeric_limits<uint32_t>::max())),
                  .instanceCount = 1u});
  sectionPlaneVisualGeometrySignature_ = signature;
  return sectionPlaneVisualDrawData_.valid();
}

bool BimManager::rebuildSectionClipCapGeometry(
    const BimSectionCapBuildOptions &options) {
  if (sectionClipCapBuildOptionsValid_ &&
      sameSectionCapBuildOptions(sectionClipCapBuildOptions_, options) &&
      sectionClipCapDrawData_.valid()) {
    return true;
  }
  clearSectionClipCapGeometry();
  if (vertices_.empty() || indices_.empty() || objectData_.empty() ||
      objectDrawCommands_.empty()) {
    return false;
  }

  auto appendGeneratedMesh = [this](const BimSectionCapGeneratedMesh &mesh,
                                    uint32_t objectIndex) {
    const uint32_t baseVertex = static_cast<uint32_t>(std::min<size_t>(
        sectionClipCapVertices_.size(), std::numeric_limits<uint32_t>::max()));
    const uint32_t baseIndex = static_cast<uint32_t>(std::min<size_t>(
        sectionClipCapIndices_.size(), std::numeric_limits<uint32_t>::max()));
    sectionClipCapVertices_.insert(sectionClipCapVertices_.end(),
                                   mesh.vertices.begin(), mesh.vertices.end());
    sectionClipCapIndices_.reserve(sectionClipCapIndices_.size() +
                                   mesh.indices.size());
    for (uint32_t index : mesh.indices) {
      sectionClipCapIndices_.push_back(baseVertex + index);
    }
    auto appendCommands = [baseIndex](const std::vector<DrawCommand> &source,
                                      std::vector<DrawCommand> &target) {
      target.reserve(target.size() + source.size());
      for (DrawCommand command : source) {
        command.firstIndex += baseIndex;
        target.push_back(command);
      }
    };
    appendCommands(mesh.fillDrawCommands,
                   sectionClipCapDrawData_.fillDrawCommands);
    appendCommands(mesh.hatchDrawCommands,
                   sectionClipCapDrawData_.hatchDrawCommands);
    sectionClipCapDrawData_.fillDrawStyles.insert(
        sectionClipCapDrawData_.fillDrawStyles.end(),
        mesh.fillDrawStyles.begin(), mesh.fillDrawStyles.end());
    sectionClipCapDrawData_.hatchDrawStyles.insert(
        sectionClipCapDrawData_.hatchDrawStyles.end(),
        mesh.hatchDrawStyles.begin(), mesh.hatchDrawStyles.end());
    sectionClipCapDrawData_.sectionMarkerLines.insert(
        sectionClipCapDrawData_.sectionMarkerLines.end(),
        mesh.sectionMarkerLines.begin(), mesh.sectionMarkerLines.end());
    auto appendSectionMarkerGeometry =
        [this, objectIndex](const BimSectionMarkerLine &marker) {
          if (objectIndex >= objectData_.size() || !isFiniteVec3(marker.a) ||
              !isFiniteVec3(marker.b) || !isFiniteVec3(marker.color)) {
            return;
          }

          const glm::vec3 line = marker.b - marker.a;
          const float lineLength = glm::length(line);
          if (!std::isfinite(lineLength) || lineLength <= 0.0001f) {
            return;
          }

          const uint32_t baseVertex = static_cast<uint32_t>(std::min<size_t>(
              sectionClipCapVertices_.size(),
              std::numeric_limits<uint32_t>::max()));
          const uint32_t firstIndex = static_cast<uint32_t>(std::min<size_t>(
              sectionClipCapIndices_.size(),
              std::numeric_limits<uint32_t>::max()));

          auto pushVertex = [&](const glm::vec3 &position) {
            container::geometry::Vertex vertex{};
            vertex.position = position;
            vertex.color = marker.color;
            sectionClipCapVertices_.push_back(vertex);
            return static_cast<uint32_t>(
                sectionClipCapVertices_.size() - 1u);
          };
          auto pushSegment = [&](const glm::vec3 &a, const glm::vec3 &b) {
            const uint32_t ia = pushVertex(a);
            const uint32_t ib = pushVertex(b);
            sectionClipCapIndices_.push_back(ia);
            sectionClipCapIndices_.push_back(ib);
          };

          pushSegment(marker.a, marker.b);

          const glm::vec3 direction = line / lineLength;
          const glm::vec3 reference =
              std::abs(direction.y) < 0.9f ? glm::vec3{0.0f, 1.0f, 0.0f}
                                           : glm::vec3{1.0f, 0.0f, 0.0f};
          glm::vec3 side = glm::cross(direction, reference);
          const float sideLength = glm::length(side);
          if (std::isfinite(sideLength) && sideLength > 0.0001f) {
            side /= sideLength;
          } else {
            side = {0.0f, 0.0f, 1.0f};
          }
          const float arrowLength =
              std::clamp(lineLength * 0.08f, 0.05f, 1.0f);
          const float arrowWidth = arrowLength * 0.45f;
          auto pushArrow = [&](const glm::vec3 &tip,
                               const glm::vec3 &inwardDirection) {
            const glm::vec3 wingCenter = tip + inwardDirection * arrowLength;
            pushSegment(tip, wingCenter + side * arrowWidth);
            pushSegment(tip, wingCenter - side * arrowWidth);
          };
          if (marker.startArrow) {
            pushArrow(marker.a, direction);
          }
          if (marker.endArrow) {
            pushArrow(marker.b, -direction);
          }

          const uint32_t indexCount = static_cast<uint32_t>(
              sectionClipCapIndices_.size() - firstIndex);
          if (indexCount == 0u) {
            sectionClipCapVertices_.resize(baseVertex);
            return;
          }

          sectionClipCapDrawData_.hatchDrawCommands.push_back(DrawCommand{
              .objectIndex = objectIndex,
              .firstIndex = firstIndex,
              .indexCount = indexCount,
              .instanceCount = 1u,
          });
          sectionClipCapDrawData_.hatchDrawStyles.push_back(
              BimSectionCapDrawStyle{
                  .objectIndex = objectIndex,
                  .materialIndex = kInvalidBimSectionCapMaterialIndex,
                  .fillColor = marker.color,
                  .fillOpacity = 1.0f,
                  .hatchSpacing = 0.0f,
                  .hatchAngleRadians = 0.0f,
                  .hatchColor = marker.color,
                  .lineWidth = marker.lineWidth,
              });
        };
    for (const BimSectionMarkerLine &marker : mesh.sectionMarkerLines) {
      appendSectionMarkerGeometry(marker);
    }
  };

  BimSectionCapBuilder sectionCapBuilder;
  std::vector<BimSectionCapTriangle> triangles;
  for (uint32_t objectIndex = 0u; objectIndex < elementMetadata_.size();
       ++objectIndex) {
    const BimElementMetadata &metadata = elementMetadata_[objectIndex];
    if (metadata.geometryKind != BimGeometryKind::Mesh ||
        objectIndex >= objectData_.size() ||
        objectIndex >= objectDrawCommandOffsets_.size() ||
        objectIndex >= objectDrawCommandCounts_.size()) {
      continue;
    }

    const glm::mat4 localPlaneTransform =
        glm::transpose(objectData_[objectIndex].model);
    const glm::vec4 localPlane = localPlaneTransform * options.sectionPlane;
    BimSectionCapBuildOptions localOptions = options;
    localOptions.sectionPlane = normalizedSectionCapPlane(localPlane);
    localOptions.clipPlaneCount = std::min<uint32_t>(
        options.clipPlaneCount,
        static_cast<uint32_t>(localOptions.clipPlanes.size()));
    for (uint32_t planeIndex = 0; planeIndex < localOptions.clipPlaneCount;
         ++planeIndex) {
      localOptions.clipPlanes[planeIndex] = normalizedSectionCapPlane(
          localPlaneTransform * options.clipPlanes[planeIndex]);
    }

    triangles.clear();
    const uint32_t commandOffset = objectDrawCommandOffsets_[objectIndex];
    const uint32_t commandCount = objectDrawCommandCounts_[objectIndex];
    const uint32_t commandEnd =
        std::min<uint32_t>(static_cast<uint32_t>(objectDrawCommands_.size()),
                           commandOffset + commandCount);
    for (uint32_t commandIndex = commandOffset; commandIndex < commandEnd;
         ++commandIndex) {
      const DrawCommand &command = objectDrawCommands_[commandIndex];
      const uint32_t indexEnd =
          std::min<uint32_t>(static_cast<uint32_t>(indices_.size()),
                             command.firstIndex + command.indexCount);
      for (uint32_t index = command.firstIndex; index + 2u < indexEnd;
           index += 3u) {
        const uint32_t i0 = indices_[index];
        const uint32_t i1 = indices_[index + 1u];
        const uint32_t i2 = indices_[index + 2u];
        if (i0 >= vertices_.size() || i1 >= vertices_.size() ||
            i2 >= vertices_.size()) {
          continue;
        }
        triangles.push_back(BimSectionCapTriangle{
            .objectIndex = objectIndex,
            .materialIndex = metadata.materialIndex,
            .p0 = vertices_[i0].position,
            .p1 = vertices_[i1].position,
            .p2 = vertices_[i2].position,
        });
      }
    }

    const BimSectionCapGeneratedMesh generated =
        sectionCapBuilder.build(triangles, localOptions);
    if (generated.valid()) {
      appendGeneratedMesh(generated, objectIndex);
    }
  }

  if (sectionClipCapVertices_.empty() || sectionClipCapIndices_.empty()) {
    clearSectionClipCapGeometry();
    return false;
  }

  const VkDeviceSize vertexBufferSize = static_cast<VkDeviceSize>(
      sizeof(container::geometry::Vertex) * sectionClipCapVertices_.size());
  const VkDeviceSize indexBufferSize = static_cast<VkDeviceSize>(
      sizeof(uint32_t) * sectionClipCapIndices_.size());
  sectionClipCapVertexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(sectionClipCapVertices_.data()),
       static_cast<size_t>(vertexBufferSize)},
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  sectionClipCapIndexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(sectionClipCapIndices_.data()),
       static_cast<size_t>(indexBufferSize)},
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  sectionClipCapDrawData_.vertexSlice = container::gpu::BufferSlice{
      sectionClipCapVertexBuffer_.buffer, 0, vertexBufferSize};
  sectionClipCapDrawData_.indexSlice = container::gpu::BufferSlice{
      sectionClipCapIndexBuffer_.buffer, 0, indexBufferSize};
  sectionClipCapBuildOptions_ = options;
  sectionClipCapBuildOptionsValid_ = sectionClipCapDrawData_.valid();
  return sectionClipCapBuildOptionsValid_;
}

const BimElementMetadata *
BimManager::metadataForObject(uint32_t objectIndex) const {
  if (objectIndex >= elementMetadata_.size()) {
    return nullptr;
  }
  return &elementMetadata_[objectIndex];
}

BimCoordinationOverlayResult BimManager::buildCoordinationOverlay(
    const BimCoordinationOverlayBuildOptions &options,
    std::span<const BimCoordinationOverlayClashPair> clashPairs,
    std::span<const BimCoordinationOverlayIssuePin> issuePins) const {
  std::vector<BimCoordinationOverlayElement> elements;
  elements.reserve(elementMetadata_.size());
  for (const BimElementMetadata &metadata : elementMetadata_) {
    BimCoordinationOverlayBounds bounds{};
    if (metadata.bounds.valid) {
      bounds = {.min = metadata.bounds.min,
                .max = metadata.bounds.max,
                .valid = true};
    }
    elements.push_back({.objectIndex = metadata.objectIndex,
                        .ifcClass = metadata.type,
                        .type = metadata.objectType,
                        .name = metadata.displayName,
                        .guid = metadata.guid,
                        .bounds = bounds});
  }

  auto firstValidObjectIndex = [this](std::span<const uint32_t> objectIndices) {
    for (uint32_t objectIndex : objectIndices) {
      if (objectIndex < objectData_.size()) {
        return objectIndex;
      }
    }
    return kInvalidBimCoordinationOverlayObjectIndex;
  };
  auto resolveIssuePinObjectIndex =
      [&](const BimCoordinationOverlayIssuePin &pin) {
        if (pin.primaryObjectIndex < objectData_.size()) {
          return pin.primaryObjectIndex;
        }
        if (!pin.ifcGuid.empty()) {
          const uint32_t objectIndex =
              firstValidObjectIndex(objectIndicesForGuid(pin.ifcGuid));
          if (objectIndex != kInvalidBimCoordinationOverlayObjectIndex) {
            return objectIndex;
          }
        }
        if (!pin.sourceId.empty()) {
          const uint32_t objectIndex =
              firstValidObjectIndex(objectIndicesForSourceId(pin.sourceId));
          if (objectIndex != kInvalidBimCoordinationOverlayObjectIndex) {
            return objectIndex;
          }
        }
        return pin.primaryObjectIndex;
      };
  std::vector<BimCoordinationOverlayIssuePin> resolvedIssuePins;
  resolvedIssuePins.reserve(issuePins.size());
  for (const BimCoordinationOverlayIssuePin &pin : issuePins) {
    BimCoordinationOverlayIssuePin resolved = pin;
    if (resolved.primaryObjectIndex >= objectData_.size()) {
      resolved.primaryObjectIndex = resolveIssuePinObjectIndex(pin);
    }
    resolvedIssuePins.push_back(std::move(resolved));
  }

  return buildBimCoordinationOverlay({.elements = elements,
                                      .clashPairs = clashPairs,
                                      .issuePins = resolvedIssuePins,
                                      .options = options});
}

std::span<const uint32_t>
BimManager::objectIndicesForGuid(std::string_view guid) const {
  return metadataIndex_->objectIndicesForGuid(guid);
}

std::span<const uint32_t>
BimManager::objectIndicesForSourceId(std::string_view sourceId) const {
  return metadataIndex_->objectIndicesForSourceId(sourceId);
}

BimElementBounds
BimManager::elementBoundsForObject(uint32_t objectIndex) const {
  if (const BimElementMetadata *metadata = metadataForObject(objectIndex);
      metadata != nullptr) {
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    bool hasBounds = false;

    auto includeIndexedBounds = [&](std::span<const uint32_t> objectIndices) {
      for (uint32_t candidateObjectIndex : objectIndices) {
        if (const BimElementMetadata *candidate =
                metadataForObject(candidateObjectIndex)) {
          includeBoundsBox(candidate->bounds, boundsMin, boundsMax, hasBounds);
        }
      }
    };

    if (!metadata->guid.empty()) {
      includeIndexedBounds(objectIndicesForGuid(metadata->guid));
    } else if (!metadata->sourceId.empty()) {
      includeIndexedBounds(objectIndicesForSourceId(metadata->sourceId));
    } else {
      includeBoundsBox(metadata->bounds, boundsMin, boundsMax, hasBounds);
    }
    if (hasBounds) {
      BimElementBounds bounds{};
      bounds.valid = true;
      bounds.min = boundsMin;
      bounds.max = boundsMax;
      bounds.center = (boundsMin + boundsMax) * 0.5f;
      bounds.size = boundsMax - boundsMin;
      bounds.radius = glm::length(bounds.size) * 0.5f;
      bounds.floorElevation = boundsMin.y;
      return bounds;
    }
  }

  BimElementBounds bounds{};
  std::vector<DrawCommand> commands;
  collectDrawCommandsForObject(objectIndex, commands);

  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
  bool hasBounds = false;
  for (const DrawCommand &command : commands) {
    if (command.firstIndex >= indices_.size()) {
      continue;
    }
    const size_t firstIndex = command.firstIndex;
    const size_t indexCount = std::min(static_cast<size_t>(command.indexCount),
                                       indices_.size() - firstIndex);
    const size_t endIndex = firstIndex + indexCount;
    const uint32_t instanceCount = std::max(command.instanceCount, 1u);
    for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
         ++instanceOffset) {
      if (command.objectIndex >
          std::numeric_limits<uint32_t>::max() - instanceOffset) {
        break;
      }
      const uint32_t commandObjectIndex = command.objectIndex + instanceOffset;
      if (commandObjectIndex >= objectData_.size()) {
        continue;
      }
      const glm::mat4 &model = objectData_[commandObjectIndex].model;
      for (size_t index = firstIndex; index < endIndex; ++index) {
        const uint32_t vertexIndex = indices_[index];
        if (vertexIndex >= vertices_.size()) {
          continue;
        }
        const glm::vec3 worldPosition =
            glm::vec3(model * glm::vec4(vertices_[vertexIndex].position, 1.0f));
        includeBoundsPoint(worldPosition, boundsMin, boundsMax, hasBounds);
      }
    }
  }

  if (!hasBounds && objectIndex < objectData_.size()) {
    const glm::vec4 sphere = objectData_[objectIndex].boundingSphere;
    const glm::vec3 center{sphere.x, sphere.y, sphere.z};
    const float radius = std::max(sphere.w, 0.0f);
    if (isFiniteVec3(center) && std::isfinite(radius)) {
      includeBoundsPoint(center - glm::vec3(radius), boundsMin, boundsMax,
                         hasBounds);
      includeBoundsPoint(center + glm::vec3(radius), boundsMin, boundsMax,
                         hasBounds);
    }
  }

  if (!hasBounds) {
    return bounds;
  }
  bounds.valid = true;
  bounds.min = boundsMin;
  bounds.max = boundsMax;
  bounds.center = (boundsMin + boundsMax) * 0.5f;
  bounds.size = boundsMax - boundsMin;
  bounds.radius = glm::length(bounds.size) * 0.5f;
  bounds.floorElevation = boundsMin.y;
  return bounds;
}

const std::vector<std::string> &BimManager::elementTypes() const {
  return metadataCatalog_->types();
}

const std::vector<std::string> &BimManager::elementStoreys() const {
  return metadataCatalog_->storeys();
}

const std::vector<std::string> &BimManager::elementMaterials() const {
  return metadataCatalog_->materials();
}

const std::vector<std::string> &BimManager::elementDisciplines() const {
  return metadataCatalog_->disciplines();
}

const std::vector<std::string> &BimManager::elementPhases() const {
  return metadataCatalog_->phases();
}

const std::vector<std::string> &BimManager::elementFireRatings() const {
  return metadataCatalog_->fireRatings();
}

const std::vector<std::string> &BimManager::elementLoadBearingValues() const {
  return metadataCatalog_->loadBearingValues();
}

const std::vector<std::string> &BimManager::elementStatuses() const {
  return metadataCatalog_->statuses();
}

const std::vector<BimStoreyRange> &BimManager::elementStoreyRanges() const {
  return metadataCatalog_->storeyRanges();
}

const BimModelUnitMetadata &BimManager::modelUnitMetadata() const {
  return metadataCatalog_->modelUnitMetadata();
}

const BimModelGeoreferenceMetadata &
BimManager::modelGeoreferenceMetadata() const {
  return metadataCatalog_->modelGeoreferenceMetadata();
}

std::vector<BimDrawingExportLine> BimManager::floorPlanDrawingExportLines(
    bool sourceElevation, glm::vec3 color, float lineWidthMm) const {
  const BimFloorPlanOverlayData &overlay =
      sourceElevation ? floorPlanSourceElevation_ : floorPlanGround_;
  if (!overlay.valid() || overlay.firstIndex >= indices_.size()) {
    return {};
  }

  const size_t firstIndex = overlay.firstIndex;
  const size_t endIndex = std::min(
      indices_.size(), firstIndex + static_cast<size_t>(overlay.indexCount));
  std::vector<BimDrawingExportLine> lines;
  lines.reserve((endIndex - firstIndex) / 2u);

  const char *layer = sourceElevation ? "floor-plan-source-elevation"
                                      : "floor-plan-ground";
  for (size_t index = firstIndex; index + 1u < endIndex; index += 2u) {
    const uint32_t ia = indices_[index];
    const uint32_t ib = indices_[index + 1u];
    if (ia >= vertices_.size() || ib >= vertices_.size()) {
      continue;
    }
    const glm::vec3 a = vertices_[ia].position;
    const glm::vec3 b = vertices_[ib].position;
    if (!isFiniteVec3(a) || !isFiniteVec3(b)) {
      continue;
    }
    lines.push_back(BimDrawingExportLine{.a = a,
                                         .b = b,
                                         .color = color,
                                         .lineWidthMm = lineWidthMm,
                                         .layer = layer});
  }
  return lines;
}

bool BimManager::setSemanticColorMode(BimSemanticColorMode mode) {
  if (mode == semanticColorMode_ && !semanticColorIdsDirty_) {
    return false;
  }
  semanticColorMode_ = mode;
  bool objectDataChanged = false;
  for (const BimElementMetadata &metadata : elementMetadata_) {
    if (metadata.objectIndex >= objectData_.size()) {
      continue;
    }
    const uint32_t semanticId =
        metadataCatalog_->semanticIdForMetadata(metadata, mode);
    container::gpu::ObjectData &object = objectData_[metadata.objectIndex];
    if (object.objectInfo.w == semanticId) {
      continue;
    }
    object.objectInfo.w = semanticId;
    objectDataChanged = true;
  }
  if (objectDataChanged) {
    uploadObjects();
  }
  semanticColorIdsDirty_ = false;
  return objectDataChanged;
}

BimSceneStats BimManager::sceneStats() const {
  size_t meshObjectCount = 0;
  size_t pointObjectCount = 0;
  size_t curveObjectCount = 0;
  for (const BimElementMetadata &metadata : elementMetadata_) {
    switch (metadata.geometryKind) {
    case BimGeometryKind::Points:
      ++pointObjectCount;
      break;
    case BimGeometryKind::Curves:
      ++curveObjectCount;
      break;
    case BimGeometryKind::Mesh:
    default:
      ++meshObjectCount;
      break;
    }
  }

  size_t gpuCompactionInputDrawCount = 0u;
  size_t gpuCompactionOutputCapacity = 0u;
  VkDeviceSize gpuCompactionBufferBytes = 0u;
  bool gpuCompactionReady = false;
  bool gpuCompactionDispatchPending = false;
  for (const BimDrawCompactionSlotResources &slot : drawCompactionSlots_) {
    gpuCompactionInputDrawCount += slot.stats.inputDrawCount;
    gpuCompactionOutputCapacity += slot.stats.outputCapacity;
    gpuCompactionBufferBytes += slot.stats.inputBufferBytes +
                                slot.stats.outputBufferBytes +
                                slot.stats.countBufferBytes;
    gpuCompactionReady = gpuCompactionReady || slot.stats.computeReady;
    gpuCompactionDispatchPending =
        gpuCompactionDispatchPending || slot.stats.dispatchPending;
  }

  return BimSceneStats{
      .objectCount = elementMetadata_.size(),
      .meshObjectCount = meshObjectCount,
      .pointObjectCount = pointObjectCount,
      .curveObjectCount = curveObjectCount,
      .opaqueDrawCount = opaqueDrawCommands_.size(),
      .transparentDrawCount = transparentDrawCommands_.size(),
      .pointOpaqueDrawCount = pointDrawLists_.opaqueDrawCommands.size(),
      .pointTransparentDrawCount =
          pointDrawLists_.transparentDrawCommands.size(),
      .curveOpaqueDrawCount = curveDrawLists_.opaqueDrawCommands.size(),
      .curveTransparentDrawCount =
          curveDrawLists_.transparentDrawCommands.size(),
      .nativePointOpaqueDrawCount =
          nativePointDrawLists_.opaqueDrawCommands.size(),
      .nativePointTransparentDrawCount =
          nativePointDrawLists_.transparentDrawCommands.size(),
      .nativeCurveOpaqueDrawCount =
          nativeCurveDrawLists_.opaqueDrawCommands.size(),
      .nativeCurveTransparentDrawCount =
          nativeCurveDrawLists_.transparentDrawCommands.size(),
      .meshletClusterCount = meshletClusterCount_,
      .meshletSourceClusterCount =
          optimizedModelMetadata_.sourceMeshletClusterCount,
      .meshletEstimatedClusterCount =
          optimizedModelMetadata_.estimatedMeshletClusterCount,
      .meshletObjectReferenceCount =
          optimizedModelMetadata_.objectClusterReferenceCount,
      .meshletGpuResidentObjectCount =
          meshletResidencyStats_.residentObjectCount,
      .meshletGpuResidentClusterCount =
          meshletResidencyStats_.residentClusterCount,
      .meshletGpuBufferBytes = meshletResidencyStats_.clusterBufferBytes +
                               meshletResidencyStats_.objectLodBufferBytes +
                               meshletResidencyStats_.residencyBufferBytes,
      .meshletGpuComputeReady = meshletResidencyStats_.computeReady,
      .meshletGpuDispatchPending = meshletResidencyStats_.dispatchPending,
      .meshletMaxLodLevel = optimizedModelMetadata_.maxLodLevel,
      .gpuCompactionInputDrawCount = gpuCompactionInputDrawCount,
      .gpuCompactionOutputCapacity = gpuCompactionOutputCapacity,
      .gpuCompactionBufferBytes = gpuCompactionBufferBytes,
      .gpuCompactionReady = gpuCompactionReady,
      .gpuCompactionDispatchPending = gpuCompactionDispatchPending,
      .optimizedModelMetadataCacheable = optimizedModelMetadata_.cacheable,
      .optimizedModelMetadataCacheHit = optimizedModelMetadata_.cacheHit,
      .optimizedModelMetadataCacheStale = optimizedModelMetadata_.cacheStale,
      .optimizedModelMetadataCacheWriteSucceeded =
          optimizedModelMetadata_.cacheWriteSucceeded,
      .floorPlanDrawCount = floorPlanGround_.drawCommands.size() +
                            floorPlanSourceElevation_.drawCommands.size(),
      .uniqueTypeCount = metadataCatalog_->types().size(),
      .uniqueStoreyCount = metadataCatalog_->storeys().size(),
      .uniqueMaterialCount = metadataCatalog_->materials().size(),
      .uniqueDisciplineCount = metadataCatalog_->disciplines().size(),
      .uniquePhaseCount = metadataCatalog_->phases().size(),
      .uniqueFireRatingCount = metadataCatalog_->fireRatings().size(),
      .uniqueLoadBearingCount = metadataCatalog_->loadBearingValues().size(),
      .uniqueStatusCount = metadataCatalog_->statuses().size(),
  };
}

std::vector<container::scene::SceneProviderTriangleBatch>
BimManager::sceneProviderTriangleBatches() const {
  std::vector<container::scene::SceneProviderTriangleBatch> batches;
  batches.reserve(opaqueDrawCommands_.size() + transparentDrawCommands_.size());

  auto toSceneProviderMaterialIndex = [](uint32_t materialIndex) {
    if (materialIndex >
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
      return int32_t{-1};
    }
    return static_cast<int32_t>(materialIndex);
  };

  auto appendBatches = [&](const std::vector<DrawCommand> &commands,
                           bool transparent) {
    for (const DrawCommand &command : commands) {
      if (command.indexCount == 0u) {
        continue;
      }

      const BimElementMetadata *metadata =
          metadataForObject(command.objectIndex);
      if (metadata != nullptr &&
          metadata->geometryKind != BimGeometryKind::Mesh) {
        continue;
      }

      uint32_t materialIndex = std::numeric_limits<uint32_t>::max();
      bool doubleSided = false;
      bool batchTransparent = transparent;
      if (metadata != nullptr) {
        materialIndex = metadata->materialIndex;
        doubleSided = metadata->doubleSided;
        batchTransparent = batchTransparent || metadata->transparent;
      } else if (command.objectIndex < objectData_.size()) {
        const container::gpu::ObjectData &object =
            objectData_[command.objectIndex];
        materialIndex = object.objectInfo.x;
        doubleSided =
            (object.objectInfo.y & container::gpu::kObjectFlagDoubleSided) !=
            0u;
      }

      batches.push_back(container::scene::SceneProviderTriangleBatch{
          .firstIndex = command.firstIndex,
          .indexCount = command.indexCount,
          .materialIndex = toSceneProviderMaterialIndex(materialIndex),
          .doubleSided = doubleSided,
          .transparent = batchTransparent,
          .instanceCount = std::max(command.instanceCount, 1u),
      });
    }
  };

  appendBatches(opaqueDrawCommands_, false);
  appendBatches(transparentDrawCommands_, true);
  return batches;
}

BimDrawBudgetLodStats
BimManager::drawBudgetLodStats(const BimDrawFilter &filter) const {
  BimDrawBudgetLodStats stats{};
  stats.enabled = filter.drawBudgetEnabled;
  stats.maxObjects = filter.drawBudgetMaxObjects;

  const BimDrawFilterStateInputs inputs = drawFilterStateInputs();
  for (uint32_t objectIndex = 0u; objectIndex < elementMetadata_.size();
       ++objectIndex) {
    const BimElementMetadata &metadata = elementMetadata_[objectIndex];
    if (!drawFilterState_->objectMatchesFilter(objectIndex, filter, inputs)) {
      continue;
    }
    ++stats.visibleObjectCount;
    if (metadata.geometryKind != BimGeometryKind::Mesh) {
      continue;
    }
    ++stats.visibleMeshObjectCount;
    if (objectIndex >= objectLodMetadata_.size()) {
      continue;
    }
    const BimObjectLodStreamingMetadata &lod = objectLodMetadata_[objectIndex];
    stats.visibleMeshletClusterReferences += lod.clusterCount;
    stats.visibleMaxLodLevel =
        std::max(stats.visibleMaxLodLevel, lod.maxLodLevel);
  }

  return stats;
}

BimDrawFilterStateInputs BimManager::drawFilterStateInputs() const {
  return BimDrawFilterStateInputs{
      .revision = objectDataRevision_,
      .objectCount = objectData_.size(),
      .metadata = {elementMetadata_.data(), elementMetadata_.size()},
      .phaseOrder = metadataCatalog_->phases(),
      .opaqueDrawCommands = &opaqueDrawCommands_,
      .opaqueSingleSidedDrawCommands = &opaqueSingleSidedDrawCommands_,
      .opaqueWindingFlippedDrawCommands = &opaqueWindingFlippedDrawCommands_,
      .opaqueDoubleSidedDrawCommands = &opaqueDoubleSidedDrawCommands_,
      .transparentDrawCommands = &transparentDrawCommands_,
      .transparentSingleSidedDrawCommands =
          &transparentSingleSidedDrawCommands_,
      .transparentWindingFlippedDrawCommands =
          &transparentWindingFlippedDrawCommands_,
      .transparentDoubleSidedDrawCommands =
          &transparentDoubleSidedDrawCommands_,
      .pointDrawLists = &pointDrawLists_,
      .curveDrawLists = &curveDrawLists_,
      .nativePointDrawLists = &nativePointDrawLists_,
      .nativeCurveDrawLists = &nativeCurveDrawLists_,
  };
}

bool BimManager::objectMatchesFilter(uint32_t objectIndex,
                                     const BimDrawFilter &filter) const {
  return drawFilterState_->objectMatchesFilter(objectIndex, filter,
                                               drawFilterStateInputs());
}

const BimDrawLists &BimManager::filteredDrawLists(const BimDrawFilter &filter) {
  return drawFilterState_->filteredDrawLists(filter, drawFilterStateInputs());
}

BimPickHit
BimManager::pickRenderableObject(const container::gpu::CameraData &cameraData,
                                 VkExtent2D viewportExtent, double cursorX,
                                 double cursorY, bool sectionPlaneEnabled,
                                 glm::vec4 sectionPlane) const {
  return pickRenderableObjectForDraws(cameraData, viewportExtent, cursorX,
                                      cursorY, true, true, sectionPlaneEnabled,
                                      sectionPlane);
}

BimPickHit BimManager::pickTransparentRenderableObject(
    const container::gpu::CameraData &cameraData, VkExtent2D viewportExtent,
    double cursorX, double cursorY, bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableObjectForDraws(cameraData, viewportExtent, cursorX,
                                      cursorY, false, true, sectionPlaneEnabled,
                                      sectionPlane);
}

BimPickHit BimManager::pickRenderableObjectForDraws(
    const container::gpu::CameraData &cameraData, VkExtent2D viewportExtent,
    double cursorX, double cursorY, bool includeOpaque, bool includeTransparent,
    bool sectionPlaneEnabled, glm::vec4 sectionPlane) const {
  if (objectData_.empty() || vertices_.empty() || indices_.empty()) {
    return {};
  }

  const PickRay ray = makePickRay(cameraData, viewportExtent, cursorX, cursorY);
  if (!ray.valid) {
    return {};
  }

  BimPickHit nearest{};
  auto testDrawCommands = [&](const std::vector<DrawCommand> &commands,
                              TriangleCullMode cullMode) {
    for (const DrawCommand &command : commands) {
      if (command.firstIndex >= indices_.size() || command.indexCount < 3u) {
        continue;
      }
      const size_t firstIndex = command.firstIndex;
      const size_t indexCount =
          std::min(static_cast<size_t>(command.indexCount),
                   indices_.size() - firstIndex);
      const size_t endIndex = firstIndex + indexCount;
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);

      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        if (objectIndex >= objectData_.size()) {
          continue;
        }

        const glm::vec4 sphere = objectData_[objectIndex].boundingSphere;
        float sphereDistance = 0.0f;
        if (sphere.w > 0.0f &&
            (!intersectRaySphere(ray.origin, ray.direction, glm::vec3(sphere),
                                 sphere.w, sphereDistance) ||
             sphereDistance > nearest.distance)) {
          continue;
        }

        const glm::mat4 &model = objectData_[objectIndex].model;
        const BimElementMetadata *metadata = metadataForObject(objectIndex);
        float sectionCapHitDistance = 0.0f;
        glm::vec3 sectionCapHitPosition{0.0f};
        bool sectionCapCandidate = false;
        bool sectionCapCrossesObject = false;
        if (sectionPlaneEnabled && metadata != nullptr &&
            intersectRaySectionPlane(ray, sectionPlane,
                                     sectionCapHitDistance) &&
            sectionCapHitDistance < nearest.distance) {
          sectionCapHitPosition =
              ray.origin + ray.direction * sectionCapHitDistance;
          sectionCapCandidate =
              insideSectionCapBounds(sectionCapHitPosition, metadata->bounds);
        }

        for (size_t index = firstIndex; index + 2u < endIndex; index += 3u) {
          const uint32_t i0 = indices_[index];
          const uint32_t i1 = indices_[index + 1u];
          const uint32_t i2 = indices_[index + 2u];
          if (i0 >= vertices_.size() || i1 >= vertices_.size() ||
              i2 >= vertices_.size()) {
            continue;
          }

          const glm::vec3 v0 =
              glm::vec3(model * glm::vec4(vertices_[i0].position, 1.0f));
          const glm::vec3 v1 =
              glm::vec3(model * glm::vec4(vertices_[i1].position, 1.0f));
          const glm::vec3 v2 =
              glm::vec3(model * glm::vec4(vertices_[i2].position, 1.0f));
          if (sectionCapCandidate && !sectionCapCrossesObject) {
            uint32_t sectionCapCrossings = 0u;
            sectionCapCrossings = accumulateSectionCapCrossing(
                sectionCapCrossings, v0, sectionPlane);
            sectionCapCrossings = accumulateSectionCapCrossing(
                sectionCapCrossings, v1, sectionPlane);
            sectionCapCrossings = accumulateSectionCapCrossing(
                sectionCapCrossings, v2, sectionPlane);
            sectionCapCrossesObject = (sectionCapCrossings & 1u) != 0u &&
                                      (sectionCapCrossings & 2u) != 0u;
          }
          float hitDistance = 0.0f;
          if (intersectRayTriangle(ray.origin, ray.direction, v0, v1, v2,
                                   cullMode, hitDistance) &&
              hitDistance < nearest.distance) {
            const glm::vec3 hitPosition =
                ray.origin + ray.direction * hitDistance;
            if (sectionPlaneClips(hitPosition, sectionPlaneEnabled,
                                  sectionPlane)) {
              continue;
            }
            nearest.objectIndex = objectIndex;
            nearest.distance = hitDistance;
            nearest.depth = projectDepth(cameraData, hitPosition);
            nearest.worldPosition = hitPosition;
            nearest.hasWorldPosition = true;
            nearest.hit = true;
          }
        }
        if (sectionCapCandidate && sectionCapCrossesObject &&
            sectionCapHitDistance < nearest.distance) {
          nearest.objectIndex = objectIndex;
          nearest.distance = sectionCapHitDistance;
          nearest.depth = projectDepth(cameraData, sectionCapHitPosition);
          nearest.worldPosition = sectionCapHitPosition;
          nearest.hasWorldPosition = true;
          nearest.hit = true;
        }
      }
    }
  };

  auto testDrawListSet =
      [&](const std::vector<DrawCommand> &opaque,
          const std::vector<DrawCommand> &opaqueSingleSided,
          const std::vector<DrawCommand> &opaqueWindingFlipped,
          const std::vector<DrawCommand> &opaqueDoubleSided,
          const std::vector<DrawCommand> &transparent,
          const std::vector<DrawCommand> &transparentSingleSided,
          const std::vector<DrawCommand> &transparentWindingFlipped,
          const std::vector<DrawCommand> &transparentDoubleSided) {
        const bool hasSplitDrawCommands =
            !opaqueSingleSided.empty() || !transparentSingleSided.empty() ||
            !opaqueWindingFlipped.empty() ||
            !transparentWindingFlipped.empty() || !opaqueDoubleSided.empty() ||
            !transparentDoubleSided.empty();
        if (hasSplitDrawCommands) {
          if (includeOpaque) {
            testDrawCommands(opaqueSingleSided, TriangleCullMode::Back);
            testDrawCommands(opaqueWindingFlipped, TriangleCullMode::Front);
            testDrawCommands(opaqueDoubleSided, TriangleCullMode::None);
          }
          if (includeTransparent) {
            testDrawCommands(transparentSingleSided, TriangleCullMode::Back);
            testDrawCommands(transparentWindingFlipped,
                             TriangleCullMode::Front);
            testDrawCommands(transparentDoubleSided, TriangleCullMode::None);
          }
          return;
        }
        if (includeOpaque) {
          testDrawCommands(opaque, TriangleCullMode::None);
        }
        if (includeTransparent) {
          testDrawCommands(transparent, TriangleCullMode::None);
        }
      };
  auto testGeometryDrawLists = [&](const BimGeometryDrawLists &lists) {
    testDrawListSet(
        lists.opaqueDrawCommands, lists.opaqueSingleSidedDrawCommands,
        lists.opaqueWindingFlippedDrawCommands,
        lists.opaqueDoubleSidedDrawCommands, lists.transparentDrawCommands,
        lists.transparentSingleSidedDrawCommands,
        lists.transparentWindingFlippedDrawCommands,
        lists.transparentDoubleSidedDrawCommands);
  };

  testDrawListSet(opaqueDrawCommands_, opaqueSingleSidedDrawCommands_,
                  opaqueWindingFlippedDrawCommands_,
                  opaqueDoubleSidedDrawCommands_, transparentDrawCommands_,
                  transparentSingleSidedDrawCommands_,
                  transparentWindingFlippedDrawCommands_,
                  transparentDoubleSidedDrawCommands_);
  testGeometryDrawLists(pointDrawLists_);
  testGeometryDrawLists(curveDrawLists_);

  std::vector<DrawCommand> pointCurvePickingCommands;
  pointCurvePickingCommands.reserve(elementMetadata_.size());
  for (const BimElementMetadata &metadata : elementMetadata_) {
    if (metadata.geometryKind != BimGeometryKind::Points &&
        metadata.geometryKind != BimGeometryKind::Curves) {
      continue;
    }
    if ((metadata.transparent && !includeTransparent) ||
        (!metadata.transparent && !includeOpaque)) {
      continue;
    }
    if (metadata.objectIndex >= objectDrawCommandOffsets_.size() ||
        metadata.objectIndex >= objectDrawCommandCounts_.size()) {
      continue;
    }
    const uint32_t offset = objectDrawCommandOffsets_[metadata.objectIndex];
    const uint32_t count = objectDrawCommandCounts_[metadata.objectIndex];
    if (offset > objectDrawCommands_.size() ||
        count > objectDrawCommands_.size() - offset) {
      continue;
    }
    pointCurvePickingCommands.insert(
        pointCurvePickingCommands.end(), objectDrawCommands_.begin() + offset,
        objectDrawCommands_.begin() + offset + count);
  }
  testDrawCommands(pointCurvePickingCommands, TriangleCullMode::None);

  return nearest;
}

void BimManager::collectDrawCommandsForObject(
    uint32_t objectIndex, std::vector<DrawCommand> &outCommands) const {
  outCommands.clear();
  if (objectIndex == std::numeric_limits<uint32_t>::max()) {
    return;
  }
  const BimElementMetadata *selectedMetadata = metadataForObject(objectIndex);
  if (selectedMetadata == nullptr) {
    return;
  }

  auto nativePointCurveObjectHasNativeDraw =
      [&](const BimElementMetadata &metadata) {
        switch (metadata.geometryKind) {
        case BimGeometryKind::Points:
          return geometryDrawListsCoverObject(nativePointDrawLists_,
                                              metadata.objectIndex);
        case BimGeometryKind::Curves:
          return geometryDrawListsCoverObject(nativeCurveDrawLists_,
                                              metadata.objectIndex);
        case BimGeometryKind::Mesh:
        default:
          return false;
        }
      };

  auto appendCommandsForObject = [&](uint32_t candidateObjectIndex) {
    const BimElementMetadata *candidateMetadata =
        metadataForObject(candidateObjectIndex);
    if (candidateMetadata != nullptr &&
        nativePointCurveObjectHasNativeDraw(*candidateMetadata)) {
      return;
    }
    if (candidateObjectIndex >= objectDrawCommandOffsets_.size() ||
        candidateObjectIndex >= objectDrawCommandCounts_.size()) {
      return;
    }
    const uint32_t offset = objectDrawCommandOffsets_[candidateObjectIndex];
    const uint32_t count = objectDrawCommandCounts_[candidateObjectIndex];
    if (offset > objectDrawCommands_.size() ||
        count > objectDrawCommands_.size() - offset) {
      return;
    }
    for (uint32_t i = 0u; i < count; ++i) {
      outCommands.push_back(objectDrawCommands_[offset + i]);
    }
  };

  auto appendIndexedCommands = [&](std::span<const uint32_t> objectIndices) {
    for (uint32_t candidateObjectIndex : objectIndices) {
      if (metadataForObject(candidateObjectIndex) != nullptr) {
        appendCommandsForObject(candidateObjectIndex);
      }
    }
  };

  if (!selectedMetadata->guid.empty()) {
    appendIndexedCommands(objectIndicesForGuid(selectedMetadata->guid));
  } else if (!selectedMetadata->sourceId.empty()) {
    appendIndexedCommands(objectIndicesForSourceId(selectedMetadata->sourceId));
  } else {
    appendCommandsForObject(objectIndex);
  }
}

void BimManager::collectNativePointDrawCommandsForObject(
    uint32_t objectIndex, std::vector<DrawCommand> &outCommands) const {
  collectNativeDrawCommandsForObject(objectIndex, BimGeometryKind::Points,
                                     nativePointDrawLists_, outCommands);
}

void BimManager::collectNativeCurveDrawCommandsForObject(
    uint32_t objectIndex, std::vector<DrawCommand> &outCommands) const {
  collectNativeDrawCommandsForObject(objectIndex, BimGeometryKind::Curves,
                                     nativeCurveDrawLists_, outCommands);
}

void BimManager::collectNativeDrawCommandsForObject(
    uint32_t objectIndex, BimGeometryKind geometryKind,
    const BimGeometryDrawLists &sourceLists,
    std::vector<DrawCommand> &outCommands) const {
  outCommands.clear();
  if (objectIndex == std::numeric_limits<uint32_t>::max()) {
    return;
  }
  const BimElementMetadata *selectedMetadata = metadataForObject(objectIndex);
  if (selectedMetadata == nullptr ||
      selectedMetadata->geometryKind != geometryKind) {
    return;
  }

  auto appendCommandsForObject = [&](uint32_t candidateObjectIndex) {
    const BimElementMetadata *candidateMetadata =
        metadataForObject(candidateObjectIndex);
    if (candidateMetadata == nullptr ||
        candidateMetadata->geometryKind != geometryKind) {
      return;
    }

    const auto appendMatchingCommands =
        [&](const std::vector<DrawCommand> &commands) {
          for (const DrawCommand &command : commands) {
            if (!drawCommandCoversObject(command, candidateObjectIndex)) {
              continue;
            }
            DrawCommand selectedCommand = command;
            selectedCommand.objectIndex = candidateObjectIndex;
            selectedCommand.instanceCount = 1u;
            outCommands.push_back(selectedCommand);
          }
        };
    appendMatchingCommands(sourceLists.opaqueDrawCommands);
    appendMatchingCommands(sourceLists.transparentDrawCommands);
  };

  auto appendIndexedCommands = [&](std::span<const uint32_t> objectIndices) {
    for (uint32_t candidateObjectIndex : objectIndices) {
      appendCommandsForObject(candidateObjectIndex);
    }
  };

  if (!selectedMetadata->guid.empty()) {
    appendIndexedCommands(objectIndicesForGuid(selectedMetadata->guid));
  } else if (!selectedMetadata->sourceId.empty()) {
    appendIndexedCommands(objectIndicesForSourceId(selectedMetadata->sourceId));
  } else {
    appendCommandsForObject(objectIndex);
  }
}

std::filesystem::path
BimManager::resolveModelPath(const std::string &path) const {
  std::filesystem::path resolved = container::util::pathFromUtf8(path);
  if (!resolved.is_relative() || std::filesystem::exists(resolved)) {
    return resolved;
  }

  const std::filesystem::path exeRelative =
      container::util::executableDirectory() / resolved;
  if (std::filesystem::exists(exeRelative)) {
    return exeRelative;
  }
  return resolved;
}

void BimManager::loadModel(const std::string &path, float importScale,
                           container::scene::SceneManager &sceneManager) {
  clear();
  if (path.empty()) {
    return;
  }

  const std::filesystem::path resolvedPath = resolveModelPath(path);
  const std::string extension = lowerAscii(resolvedPath.extension().string());
  if (extension == ".bim") {
    loadDotBim(resolvedPath, importScale, sceneManager);
  } else if (extension == ".ifc") {
    loadIfcWithPreparedSidecarFallback(resolvedPath, importScale,
                                       sceneManager);
  } else if (extension == ".ifcx") {
    loadPreparedModel(
        container::geometry::ifcx::LoadFromFile(resolvedPath, importScale),
        resolvedPath, "IFCX", sceneManager);
  } else if (extension == ".usd" || extension == ".usda" ||
             extension == ".usdc" || extension == ".usdz") {
    loadPreparedModel(
        container::geometry::usd::LoadFromFile(resolvedPath, importScale),
        resolvedPath, "USD", sceneManager);
  } else if (extension == ".gltf" || extension == ".glb") {
    loadGltfFallback(resolvedPath, importScale, sceneManager);
  } else {
    throw std::runtime_error(
        "unsupported BIM model format '" + extension +
        "'; supported BIM sources are .bim, .ifc, .ifcx, .usd, .usda, "
        ".usdc, .usdz, .gltf, and .glb");
  }
  modelPath_ = path;
}

void BimManager::loadDotBim(const std::filesystem::path &path,
                            float importScale,
                            container::scene::SceneManager &sceneManager) {
  const auto model =
      container::geometry::dotbim::LoadFromFile(path, importScale);
  loadPreparedModel(model, path, "dotbim", sceneManager);
}

void BimManager::loadIfc(const std::filesystem::path &path, float importScale,
                         container::scene::SceneManager &sceneManager) {
  const auto model = container::geometry::ifc::LoadFromFile(path, importScale);
  loadPreparedModel(model, path, "IFC", sceneManager);
}

void BimManager::loadIfcWithPreparedSidecarFallback(
    const std::filesystem::path &path, float importScale,
    container::scene::SceneManager &sceneManager) {
  try {
    loadIfc(path, importScale, sceneManager);
    return;
  } catch (const std::exception &) {
    const std::optional<std::filesystem::path> sidecar =
        preparedIfcSidecarPath(path);
    if (!sidecar) {
      throw;
    }
    clear();
    loadPreparedModel(container::geometry::ifcx::LoadFromFile(*sidecar,
                                                              importScale),
                      *sidecar, "IFCX", sceneManager);
  }
}

void BimManager::loadPreparedModel(
    const container::geometry::dotbim::Model &model,
    const std::filesystem::path &path, std::string_view format,
    container::scene::SceneManager &sceneManager) {
  if (!hasRenderableSourceGeometry(model)) {
    throw std::runtime_error(modelLoadErrorPrefix(format, path));
  }

  metadataCatalog_->setModelUnitMetadata(
      bimModelUnitMetadata(model.unitMetadata));
  metadataCatalog_->setModelGeoreferenceMetadata(
      bimModelGeoreferenceMetadata(model.georeferenceMetadata));
  meshletClusters_ = buildMeshletClusterMetadataForModel(model);
  meshletClusterCount_ = meshletClusters_.size();
  optimizedModelMetadata_ =
      buildOptimizedModelMetadata(path, model, meshletClusters_);
  std::vector<container::geometry::Vertex> uploadVertices = model.vertices;
  std::vector<uint32_t> uploadIndices = model.indices;
  const BimFloorPlanBuildResult floorPlanGround =
      appendFloorPlanOverlayGeometry(model, uploadVertices, uploadIndices,
                                     false);
  floorPlanGround_.firstIndex = floorPlanGround.firstIndex;
  floorPlanGround_.indexCount = floorPlanGround.indexCount;
  floorPlanGround_.boundsCenter = floorPlanGround.boundsCenter;
  floorPlanGround_.boundsRadius = floorPlanGround.boundsRadius;
  const BimFloorPlanBuildResult floorPlanSourceElevation =
      appendFloorPlanOverlayGeometry(model, uploadVertices, uploadIndices,
                                     true);
  floorPlanSourceElevation_.firstIndex = floorPlanSourceElevation.firstIndex;
  floorPlanSourceElevation_.indexCount = floorPlanSourceElevation.indexCount;
  floorPlanSourceElevation_.boundsCenter =
      floorPlanSourceElevation.boundsCenter;
  floorPlanSourceElevation_.boundsRadius =
      floorPlanSourceElevation.boundsRadius;

  uploadGeometry(uploadVertices, uploadIndices);
  buildDrawDataFromModel(model, sceneManager);
  relationshipGraph_.build(elementMetadata_, model.relationships);
  uploadMeshletResidencyBuffers();
  if (!hasScene()) {
    clear();
    throw std::runtime_error(modelLoadErrorPrefix(format, path));
  }
}

void BimManager::buildDrawDataFromModel(
    const container::geometry::dotbim::Model &model,
    container::scene::SceneManager &sceneManager) {
  std::unordered_map<uint32_t, uint32_t> colorMaterialCache;
  colorMaterialCache.reserve(model.elements.size());
  std::vector<uint32_t> sourceMaterialCache(model.materials.size(),
                                            kInvalidMaterialIndex);
  std::unordered_map<uint32_t, MeshRange> rangesByMeshId;
  rangesByMeshId.reserve(model.meshRanges.size());
  for (const auto &sourceRange : model.meshRanges) {
    rangesByMeshId.emplace(sourceRange.meshId,
                           MeshRange{sourceRange.meshId, sourceRange.firstIndex,
                                     sourceRange.indexCount,
                                     sourceRange.boundsCenter,
                                     sourceRange.boundsRadius});
  }
  std::unordered_map<uint32_t, MeshRange> nativePointRangesByMeshId;
  nativePointRangesByMeshId.reserve(model.nativePointRanges.size());
  for (const auto &sourceRange : model.nativePointRanges) {
    nativePointRangesByMeshId.emplace(
        sourceRange.meshId,
        MeshRange{sourceRange.meshId, sourceRange.firstIndex,
                  sourceRange.indexCount, sourceRange.boundsCenter,
                  sourceRange.boundsRadius});
  }
  std::unordered_map<uint32_t, MeshRange> nativeCurveRangesByMeshId;
  nativeCurveRangesByMeshId.reserve(model.nativeCurveRanges.size());
  for (const auto &sourceRange : model.nativeCurveRanges) {
    nativeCurveRangesByMeshId.emplace(
        sourceRange.meshId,
        MeshRange{sourceRange.meshId, sourceRange.firstIndex,
                  sourceRange.indexCount, sourceRange.boundsCenter,
                  sourceRange.boundsRadius});
  }
  const std::unordered_map<uint32_t, MeshletClusterSpan> clusterSpansByMeshId =
      meshletClusterSpansByMeshId(meshletClusters_);

  std::vector<PendingDraw> opaquePendingDraws;
  std::vector<PendingDraw> transparentPendingDraws;
  opaquePendingDraws.reserve(model.elements.size());
  transparentPendingDraws.reserve(model.elements.size());
  elementMetadata_.clear();
  objectLodMetadata_.clear();
  metadataIndex_->clear();
  metadataCatalog_->clearLabels();
  metadataIndex_->reserve(model.elements.size());
  metadataCatalog_->reserve(model.elements.size());
  std::unordered_map<std::string, uint32_t> productIdentityIds;
  productIdentityIds.reserve(model.elements.size());

  auto computeWorldBounds = [&](const MeshRange &range,
                                const glm::mat4 &transform) {
    BimElementBounds bounds{};
    if (range.firstIndex >= model.indices.size()) {
      return bounds;
    }

    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    bool hasBounds = false;
    const size_t firstIndex = range.firstIndex;
    const size_t indexCount = std::min(static_cast<size_t>(range.indexCount),
                                       model.indices.size() - firstIndex);
    const size_t endIndex = firstIndex + indexCount;
    for (size_t index = firstIndex; index < endIndex; ++index) {
      const uint32_t vertexIndex = model.indices[index];
      if (vertexIndex >= model.vertices.size()) {
        continue;
      }
      const glm::vec3 worldPosition = glm::vec3(
          transform * glm::vec4(model.vertices[vertexIndex].position, 1.0f));
      includeBoundsPoint(worldPosition, boundsMin, boundsMax, hasBounds);
    }

    if (!hasBounds) {
      return bounds;
    }
    bounds.valid = true;
    bounds.min = boundsMin;
    bounds.max = boundsMax;
    bounds.center = (boundsMin + boundsMax) * 0.5f;
    bounds.size = boundsMax - boundsMin;
    bounds.radius = glm::length(bounds.size) * 0.5f;
    bounds.floorElevation = boundsMin.y;
    return bounds;
  };

  for (size_t elementIndex = 0; elementIndex < model.elements.size();
       ++elementIndex) {
    const auto &element = model.elements[elementIndex];
    const auto rangeIt = rangesByMeshId.find(element.meshId);
    if (rangeIt == rangesByMeshId.end() || rangeIt->second.indexCount == 0u) {
      continue;
    }

    glm::vec4 color =
        glm::clamp(element.color, glm::vec4(0.0f), glm::vec4(1.0f));
    uint32_t materialIndex = kInvalidMaterialIndex;
    bool transparent = isTransparentColor(color);
    bool materialDoubleSided = false;
    if (isValidMaterialIndex(element.materialIndex, model)) {
      materialIndex = sourceMaterialCache[element.materialIndex];
      if (materialIndex == kInvalidMaterialIndex) {
        const auto &sourceMaterial = model.materials[element.materialIndex];
        materialIndex = sceneManager.createMaterial(
            makeSceneMaterial(sourceMaterial, sceneManager));
        sourceMaterialCache[element.materialIndex] = materialIndex;
      }
      const auto properties =
          sceneManager.materialRenderProperties(materialIndex);
      transparent = transparent || properties.transparent;
      materialDoubleSided = properties.doubleSided;
    } else {
      const auto alphaMode = transparent
                                 ? container::material::AlphaMode::Blend
                                 : container::material::AlphaMode::Opaque;
      const uint32_t colorKey = packColor(color);
      auto materialIt = colorMaterialCache.find(colorKey);
      if (materialIt == colorMaterialCache.end()) {
        const uint32_t newMaterialIndex =
            sceneManager.createSolidMaterial(color, false, alphaMode);
        materialIt =
            colorMaterialCache.emplace(colorKey, newMaterialIndex).first;
      }
      materialIndex = materialIt->second;
    }

    const auto &range = rangeIt->second;
    const BimElementBounds elementBounds =
        computeWorldBounds(range, element.transform);
    const bool doubleSided = element.doubleSided || materialDoubleSided;
    const bool windingFlipped =
        !doubleSided && transformFlipsWinding(element.transform);
    const std::string elementType = normalizedElementType(element.type);
    const uint32_t semanticTypeId = metadataCatalog_->registerType(elementType);
    BimElementMetadata metadataLabelProbe{};
    metadataLabelProbe.storeyName = element.storeyName;
    metadataLabelProbe.storeyId = element.storeyId;
    metadataLabelProbe.materialName = element.materialName;
    metadataLabelProbe.materialCategory = element.materialCategory;
    metadataCatalog_->registerStorey(metadataLabelProbe, elementBounds);
    metadataCatalog_->registerMaterial(metadataLabelProbe);
    metadataCatalog_->registerDiscipline(element.discipline);
    metadataCatalog_->registerPhase(element.phase);
    metadataCatalog_->registerFireRating(element.fireRating);
    metadataCatalog_->registerLoadBearing(element.loadBearing);
    metadataCatalog_->registerStatus(element.status);

    PendingDraw pending{};
    pending.object =
        makeObjectData(element.transform, materialIndex, doubleSided,
                       range.boundsCenter, range.boundsRadius);
    pending.object.objectInfo.w = semanticTypeId + 1u;
    pending.metadata = BimElementMetadata{
        .sourceElementIndex = static_cast<uint32_t>(std::min<size_t>(
            elementIndex, std::numeric_limits<uint32_t>::max())),
        .meshId = element.meshId,
        .sourceMaterialIndex = element.materialIndex,
        .materialIndex = materialIndex,
        .semanticTypeId = semanticTypeId,
        .sourceColor = color,
        .guid = element.guid,
        .type = elementType,
        .displayName = element.displayName,
        .objectType = element.objectType,
        .storeyName = element.storeyName,
        .storeyId = element.storeyId,
        .materialName = element.materialName,
        .materialCategory = element.materialCategory,
        .discipline = element.discipline,
        .phase = element.phase,
        .fireRating = element.fireRating,
        .loadBearing = element.loadBearing,
        .status = element.status,
        .sourceId = element.sourceId,
        .properties = bimElementProperties(element.properties),
        .transparent = transparent,
        .doubleSided = doubleSided,
        .bounds = elementBounds,
        .geometryKind = bimGeometryKind(element.geometryKind),
    };
    pending.firstIndex = range.firstIndex;
    pending.indexCount = range.indexCount;
    if (pending.metadata.geometryKind == BimGeometryKind::Points) {
      if (const auto nativeRangeIt =
              nativePointRangesByMeshId.find(element.meshId);
          nativeRangeIt != nativePointRangesByMeshId.end()) {
        pending.nativeFirstIndex = nativeRangeIt->second.firstIndex;
        pending.nativeIndexCount = nativeRangeIt->second.indexCount;
      }
    } else if (pending.metadata.geometryKind == BimGeometryKind::Curves) {
      if (const auto nativeRangeIt =
              nativeCurveRangesByMeshId.find(element.meshId);
          nativeRangeIt != nativeCurveRangesByMeshId.end()) {
        pending.nativeFirstIndex = nativeRangeIt->second.firstIndex;
        pending.nativeIndexCount = nativeRangeIt->second.indexCount;
      }
    }
    if (transparent) {
      if (doubleSided) {
        pending.bucket = DrawBucket::TransparentDoubleSided;
      } else if (windingFlipped) {
        pending.bucket = DrawBucket::TransparentWindingFlipped;
      } else {
        pending.bucket = DrawBucket::TransparentSingleSided;
      }
      transparentPendingDraws.push_back(pending);
    } else {
      if (doubleSided) {
        pending.bucket = DrawBucket::OpaqueDoubleSided;
      } else if (windingFlipped) {
        pending.bucket = DrawBucket::OpaqueWindingFlipped;
      } else {
        pending.bucket = DrawBucket::OpaqueSingleSided;
      }
      opaquePendingDraws.push_back(pending);
    }
  }

  if (opaquePendingDraws.empty() && transparentPendingDraws.empty()) {
    return;
  }

  metadataCatalog_->sortStoreyRanges();

  std::ranges::sort(
      opaquePendingDraws, [](const PendingDraw &lhs, const PendingDraw &rhs) {
        if (lhs.bucket != rhs.bucket) {
          return bucketSortKey(lhs.bucket) < bucketSortKey(rhs.bucket);
        }
        if (lhs.metadata.geometryKind != rhs.metadata.geometryKind) {
          return geometryKindSortKey(lhs.metadata.geometryKind) <
                 geometryKindSortKey(rhs.metadata.geometryKind);
        }
        if (lhs.firstIndex != rhs.firstIndex) {
          return lhs.firstIndex < rhs.firstIndex;
        }
        return lhs.indexCount < rhs.indexCount;
      });
  std::ranges::sort(transparentPendingDraws, [](const PendingDraw &lhs,
                                                const PendingDraw &rhs) {
    if (lhs.bucket != rhs.bucket) {
      return bucketSortKey(lhs.bucket) < bucketSortKey(rhs.bucket);
    }
    if (lhs.metadata.geometryKind != rhs.metadata.geometryKind) {
      return geometryKindSortKey(lhs.metadata.geometryKind) <
             geometryKindSortKey(rhs.metadata.geometryKind);
    }
    if (lhs.firstIndex != rhs.firstIndex) {
      return lhs.firstIndex < rhs.firstIndex;
    }
    return lhs.indexCount < rhs.indexCount;
  });

  auto countPendingKind = [](const std::vector<PendingDraw> &draws,
                             BimGeometryKind kind) {
    return static_cast<size_t>(
        std::ranges::count_if(draws, [kind](const PendingDraw &pending) {
          return pending.metadata.geometryKind == kind;
        }));
  };
  const size_t opaqueMeshDrawCount =
      countPendingKind(opaquePendingDraws, BimGeometryKind::Mesh);
  const size_t transparentMeshDrawCount =
      countPendingKind(transparentPendingDraws, BimGeometryKind::Mesh);
  const size_t opaquePointDrawCount =
      countPendingKind(opaquePendingDraws, BimGeometryKind::Points);
  const size_t transparentPointDrawCount =
      countPendingKind(transparentPendingDraws, BimGeometryKind::Points);
  const size_t opaqueCurveDrawCount =
      countPendingKind(opaquePendingDraws, BimGeometryKind::Curves);
  const size_t transparentCurveDrawCount =
      countPendingKind(transparentPendingDraws, BimGeometryKind::Curves);

  const size_t totalDraws =
      opaquePendingDraws.size() + transparentPendingDraws.size();
  const size_t floorPlanDrawCount =
      (floorPlanGround_.indexCount >= 2u ? 1u : 0u) +
      (floorPlanSourceElevation_.indexCount >= 2u ? 1u : 0u);
  objectData_.reserve(totalDraws + floorPlanDrawCount);
  objectDrawCommands_.reserve(totalDraws + floorPlanDrawCount);
  objectDrawCommandOffsets_.reserve(totalDraws + floorPlanDrawCount);
  objectDrawCommandCounts_.reserve(totalDraws + floorPlanDrawCount);
  elementMetadata_.reserve(totalDraws);
  objectLodMetadata_.reserve(totalDraws);
  opaqueDrawCommands_.reserve(opaqueMeshDrawCount);
  opaqueSingleSidedDrawCommands_.reserve(opaqueMeshDrawCount);
  opaqueWindingFlippedDrawCommands_.reserve(opaqueMeshDrawCount);
  opaqueDoubleSidedDrawCommands_.reserve(opaqueMeshDrawCount);
  transparentDrawCommands_.reserve(transparentMeshDrawCount);
  transparentSingleSidedDrawCommands_.reserve(transparentMeshDrawCount);
  transparentWindingFlippedDrawCommands_.reserve(transparentMeshDrawCount);
  transparentDoubleSidedDrawCommands_.reserve(transparentMeshDrawCount);
  pointDrawLists_.reserve(opaquePointDrawCount, transparentPointDrawCount);
  curveDrawLists_.reserve(opaqueCurveDrawCount, transparentCurveDrawCount);
  nativePointDrawLists_.reserve(opaquePointDrawCount,
                                transparentPointDrawCount);
  nativeCurveDrawLists_.reserve(opaqueCurveDrawCount,
                                transparentCurveDrawCount);

  auto appendPendingDraw = [this, &clusterSpansByMeshId, &productIdentityIds](
                               const PendingDraw &pending, bool allowMerge) {
    const uint32_t objectIndex = static_cast<uint32_t>(objectData_.size());
    objectData_.push_back(pending.object);
    objectDrawCommandOffsets_.push_back(
        static_cast<uint32_t>(objectDrawCommands_.size()));
    objectDrawCommands_.push_back(DrawCommand{
        .objectIndex = objectIndex,
        .firstIndex = pending.firstIndex,
        .indexCount = pending.indexCount,
        .instanceCount = 1u,
    });
    objectDrawCommandCounts_.push_back(1u);
    BimElementMetadata metadata = pending.metadata;
    metadata.objectIndex = objectIndex;
    std::string productKey;
    if (!metadata.guid.empty()) {
      productKey = "g:" + metadata.guid;
    } else if (!metadata.sourceId.empty()) {
      productKey = "s:" + metadata.sourceId;
    } else {
      productKey = "o:" + std::to_string(objectIndex);
    }
    auto [productIt, insertedProduct] = productIdentityIds.try_emplace(
        productKey, static_cast<uint32_t>(std::min<size_t>(
                        productIdentityIds.size() + 1u,
                        std::numeric_limits<uint32_t>::max())));
    (void)insertedProduct;
    metadata.productIdentityId = productIt->second;
    elementMetadata_.push_back(std::move(metadata));
    metadataIndex_->index(elementMetadata_.back());
    BimObjectLodStreamingMetadata lodMetadata{};
    lodMetadata.objectIndex = objectIndex;
    lodMetadata.sourceElementIndex = elementMetadata_.back().sourceElementIndex;
    lodMetadata.meshId = elementMetadata_.back().meshId;
    lodMetadata.geometryKind = elementMetadata_.back().geometryKind;
    if (elementMetadata_.back().geometryKind == BimGeometryKind::Mesh) {
      if (const auto spanIt =
              clusterSpansByMeshId.find(elementMetadata_.back().meshId);
          spanIt != clusterSpansByMeshId.end()) {
        lodMetadata.firstCluster = spanIt->second.firstCluster;
        lodMetadata.clusterCount = spanIt->second.clusterCount;
        lodMetadata.maxLodLevel = spanIt->second.maxLodLevel;
        lodMetadata.triangleCount = spanIt->second.triangleCount;
        lodMetadata.estimatedClusters = spanIt->second.estimatedClusters;
      }
    }
    objectLodMetadata_.push_back(lodMetadata);

    auto appendBucketCommand = [&](std::vector<DrawCommand> &commands) {
      appendDrawCommand(commands, objectIndex, pending.firstIndex,
                        pending.indexCount, allowMerge);
    };
    auto appendAggregateCommand = [&](std::vector<DrawCommand> &commands) {
      appendDrawCommand(commands, objectIndex, pending.firstIndex,
                        pending.indexCount, false);
    };

    switch (pending.metadata.geometryKind) {
    case BimGeometryKind::Points:
      if (pending.nativeIndexCount > 0u) {
        appendDrawCommandToGeometryLists(nativePointDrawLists_, pending.bucket,
                                         objectIndex, pending.nativeFirstIndex,
                                         pending.nativeIndexCount, allowMerge);
      } else {
        appendDrawCommandToGeometryLists(pointDrawLists_, pending.bucket,
                                         objectIndex, pending.firstIndex,
                                         pending.indexCount, allowMerge);
      }
      break;
    case BimGeometryKind::Curves:
      if (pending.nativeIndexCount > 0u) {
        appendDrawCommandToGeometryLists(nativeCurveDrawLists_, pending.bucket,
                                         objectIndex, pending.nativeFirstIndex,
                                         pending.nativeIndexCount, allowMerge);
      } else {
        appendDrawCommandToGeometryLists(curveDrawLists_, pending.bucket,
                                         objectIndex, pending.firstIndex,
                                         pending.indexCount, allowMerge);
      }
      break;
    case BimGeometryKind::Mesh:
    default:
      switch (pending.bucket) {
      case DrawBucket::OpaqueSingleSided:
        appendAggregateCommand(opaqueDrawCommands_);
        appendBucketCommand(opaqueSingleSidedDrawCommands_);
        break;
      case DrawBucket::OpaqueWindingFlipped:
        appendAggregateCommand(opaqueDrawCommands_);
        appendBucketCommand(opaqueWindingFlippedDrawCommands_);
        break;
      case DrawBucket::OpaqueDoubleSided:
        appendAggregateCommand(opaqueDrawCommands_);
        appendBucketCommand(opaqueDoubleSidedDrawCommands_);
        break;
      case DrawBucket::TransparentSingleSided:
        appendAggregateCommand(transparentDrawCommands_);
        appendBucketCommand(transparentSingleSidedDrawCommands_);
        break;
      case DrawBucket::TransparentWindingFlipped:
        appendAggregateCommand(transparentDrawCommands_);
        appendBucketCommand(transparentWindingFlippedDrawCommands_);
        break;
      case DrawBucket::TransparentDoubleSided:
        appendAggregateCommand(transparentDrawCommands_);
        appendBucketCommand(transparentDoubleSidedDrawCommands_);
        break;
      }
      break;
    }
  };

  for (const PendingDraw &pending : opaquePendingDraws) {
    appendPendingDraw(pending, true);
  }
  for (const PendingDraw &pending : transparentPendingDraws) {
    appendPendingDraw(pending, false);
  }
  optimizedModelMetadata_.objectClusterReferenceCount = 0u;
  for (const BimObjectLodStreamingMetadata &lod : objectLodMetadata_) {
    if (lod.geometryKind == BimGeometryKind::Mesh) {
      optimizedModelMetadata_.objectClusterReferenceCount += lod.clusterCount;
      optimizedModelMetadata_.maxLodLevel =
          std::max(optimizedModelMetadata_.maxLodLevel, lod.maxLodLevel);
    }
  }
  refreshOptimizedModelMetadataCache(optimizedModelMetadata_,
                                     objectLodMetadata_);

  auto appendFloorPlanOverlay =
      [this, &sceneManager](BimFloorPlanOverlayData &overlay) {
        if (overlay.indexCount < 2u) {
          return;
        }
        overlay.objectIndex = static_cast<uint32_t>(objectData_.size());
        container::gpu::ObjectData floorPlanObject =
            makeObjectData(glm::mat4(1.0f), sceneManager.defaultMaterialIndex(),
                           true, overlay.boundsCenter, overlay.boundsRadius);
        floorPlanObject.objectInfo.y = container::gpu::kObjectFlagDoubleSided;
        floorPlanObject.objectInfo.w = 0u;
        objectData_.push_back(floorPlanObject);
        DrawCommand floorPlanDrawCommand{
            .objectIndex = overlay.objectIndex,
            .firstIndex = overlay.firstIndex,
            .indexCount = overlay.indexCount,
            .instanceCount = 1u,
        };
        objectDrawCommandOffsets_.push_back(
            static_cast<uint32_t>(objectDrawCommands_.size()));
        objectDrawCommands_.push_back(floorPlanDrawCommand);
        objectDrawCommandCounts_.push_back(1u);
        overlay.drawCommands.push_back(floorPlanDrawCommand);
      };
  appendFloorPlanOverlay(floorPlanGround_);
  appendFloorPlanOverlay(floorPlanSourceElevation_);

  if (objectData_.empty()) {
    return;
  }
  sceneManager.uploadMaterialResources();
  uploadObjects();
  uploadVisibilityFilterBuffers();
}

void BimManager::loadGltfFallback(
    const std::filesystem::path &path, float importScale,
    container::scene::SceneManager &sceneManager) {
  const auto model = container::geometry::Model::LoadFromGltf(
      container::util::pathToUtf8(path));
  if (model.vertices().empty() || model.indices().empty() ||
      model.primitiveRanges().empty()) {
    throw std::runtime_error(modelLoadErrorPrefix("glTF", path));
  }
  metadataCatalog_->setModelUnitMetadata(fallbackUnitMetadata(importScale));
  uploadGeometry(model.vertices(), model.indices());

  const glm::mat4 transform = importScaleTransform(importScale);
  const uint32_t materialIndex = sceneManager.defaultMaterialIndex();
  container::gpu::ObjectData object =
      makeObjectData(transform, materialIndex, false, glm::vec3(0.0f), 0.0f);
  object.objectInfo.w = 1u;
  objectData_.push_back(object);
  objectDrawCommandOffsets_.push_back(
      static_cast<uint32_t>(objectDrawCommands_.size()));
  metadataCatalog_->clearLabels();
  const uint32_t semanticTypeId = metadataCatalog_->registerType("glTF");
  elementMetadata_.push_back(BimElementMetadata{
      .objectIndex = 0u,
      .sourceElementIndex = 0u,
      .meshId = 0u,
      .materialIndex = materialIndex,
      .semanticTypeId = semanticTypeId,
      .productIdentityId = 1u,
      .type = "glTF",
      .sourceId = container::util::pathToUtf8(path.lexically_normal()),
  });
  metadataIndex_->index(elementMetadata_.back());

  opaqueDrawCommands_.reserve(model.primitiveRanges().size());
  opaqueSingleSidedDrawCommands_.reserve(model.primitiveRanges().size());
  opaqueDoubleSidedDrawCommands_.reserve(model.primitiveRanges().size());
  uint32_t objectDrawCommandCount = 0u;
  for (const auto &primitive : model.primitiveRanges()) {
    if (primitive.indexCount == 0u) {
      continue;
    }

    DrawCommand command{};
    command.objectIndex = 0u;
    command.firstIndex = primitive.firstIndex;
    command.indexCount = primitive.indexCount;
    objectDrawCommands_.push_back(command);
    ++objectDrawCommandCount;
    opaqueDrawCommands_.push_back(command);
    if (primitive.disableBackfaceCulling) {
      opaqueDoubleSidedDrawCommands_.push_back(command);
    } else if (transformFlipsWinding(transform)) {
      opaqueWindingFlippedDrawCommands_.push_back(command);
    } else {
      opaqueSingleSidedDrawCommands_.push_back(command);
    }
  }
  objectDrawCommandCounts_.push_back(objectDrawCommandCount);

  relationshipGraph_.build(elementMetadata_);
  uploadObjects();
  uploadVisibilityFilterBuffers();
  if (!hasScene()) {
    clear();
    throw std::runtime_error(modelLoadErrorPrefix("glTF", path));
  }
}

void BimManager::uploadGeometry(
    std::span<const container::geometry::Vertex> vertices,
    std::span<const uint32_t> indices) {
  vertices_.clear();
  indices_.clear();
  if (vertices.empty() || indices.empty()) {
    return;
  }

  vertices_.assign(vertices.begin(), vertices.end());
  indices_.assign(indices.begin(), indices.end());

  const VkDeviceSize vertexBufferSize = static_cast<VkDeviceSize>(
      sizeof(container::geometry::Vertex) * vertices.size());
  const VkDeviceSize indexBufferSize =
      static_cast<VkDeviceSize>(sizeof(uint32_t) * indices.size());

  vertexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(vertices.data()),
       static_cast<size_t>(vertexBufferSize)},
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  indexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(indices.data()),
       static_cast<size_t>(indexBufferSize)},
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  vertexSlice_ = {vertexBuffer_.buffer, 0, vertexBufferSize};
  indexSlice_ = {indexBuffer_.buffer, 0, indexBufferSize};
}

void BimManager::destroyMeshletResidencyBuffers() {
  if (meshletClusterBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(meshletClusterBuffer_);
  }
  if (meshletObjectLodBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(meshletObjectLodBuffer_);
  }
  if (meshletResidencyBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(meshletResidencyBuffer_);
  }
  meshletResidencyStats_ = {};
  meshletResidencyCameraBuffer_ = VK_NULL_HANDLE;
  meshletResidencyCameraBufferSize_ = 0;
  meshletResidencyObjectBuffer_ = VK_NULL_HANDLE;
  meshletResidencyObjectBufferSize_ = 0;
  meshletResidencyDescriptorsDirty_ = true;
  meshletResidencyDispatchPending_ = false;
}

void BimManager::destroyMeshletResidencyComputeResources() {
  pipelineManager_.destroyPipeline(meshletResidencyPipeline_);
  pipelineManager_.destroyPipelineLayout(meshletResidencyPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(meshletResidencyDescriptorPool_);
  pipelineManager_.destroyDescriptorSetLayout(meshletResidencySetLayout_);
  meshletResidencyDescriptorSet_ = VK_NULL_HANDLE;
  meshletResidencyDescriptorsDirty_ = false;
  meshletResidencyDispatchPending_ = false;
}

void BimManager::destroyVisibilityFilterBuffers() {
  if (visibilityFilterMetadataBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(visibilityFilterMetadataBuffer_);
  }
  if (visibilityMaskBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(visibilityMaskBuffer_);
  }
  visibilityFilterMetadata_.clear();
  visibilityFilterStats_ = {};
  visibilityFilterStats_.computeReady =
      visibilityFilterPipeline_ != VK_NULL_HANDLE &&
      visibilityFilterDescriptorSet_ != VK_NULL_HANDLE;
  visibilityFilterSettings_ = {};
  visibilityFilterDescriptorsDirty_ = true;
  visibilityFilterDispatchPending_ = false;
  visibilityFilterMaskCurrent_ = false;
  visibilityFilterMaskAllVisible_ = false;
}

void BimManager::destroyVisibilityFilterComputeResources() {
  pipelineManager_.destroyPipeline(visibilityFilterPipeline_);
  pipelineManager_.destroyPipelineLayout(visibilityFilterPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(visibilityFilterDescriptorPool_);
  pipelineManager_.destroyDescriptorSetLayout(visibilityFilterSetLayout_);
  visibilityFilterDescriptorSet_ = VK_NULL_HANDLE;
  visibilityFilterDescriptorsDirty_ = false;
  visibilityFilterDispatchPending_ = false;
  visibilityFilterStats_.computeReady = false;
  visibilityFilterStats_.dispatchPending = false;
}

void BimManager::createVisibilityFilterResources(
    const std::filesystem::path &shaderDir) {
  if (visibilityFilterPipeline_ != VK_NULL_HANDLE) {
    return;
  }
  if (visibilityFilterSetLayout_ != VK_NULL_HANDLE ||
      visibilityFilterDescriptorPool_ != VK_NULL_HANDLE ||
      visibilityFilterPipelineLayout_ != VK_NULL_HANDLE) {
    destroyVisibilityFilterComputeResources();
  }
  if (!device_) {
    throw std::runtime_error(
        "BimManager::createVisibilityFilterResources requires a device");
  }

  const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
  }};
  const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0u);
  visibilityFilterSetLayout_ = pipelineManager_.createDescriptorSetLayout(
      {bindings.begin(), bindings.end()}, flags);
  visibilityFilterDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}}, 1, 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = visibilityFilterDescriptorPool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &visibilityFilterSetLayout_;
  if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
                               &visibilityFilterDescriptorSet_) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate BIM visibility filter set");
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(BimVisibilityFilterPushConstants);
  visibilityFilterPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {visibilityFilterSetLayout_}, {pcRange});

  const std::filesystem::path compPath =
      shaderDir / "spv_shaders" / "bim_visibility_filter.comp.spv";
  if (!std::filesystem::exists(compPath)) {
    visibilityFilterStats_.computeReady = false;
    visibilityFilterDescriptorsDirty_ = true;
    return;
  }

  const auto spvData = container::util::readFile(compPath);
  VkShaderModule compModule =
      container::gpu::createShaderModule(device_->device(), spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName = "computeMain";

  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = stage;
  pipelineInfo.layout = visibilityFilterPipelineLayout_;
  visibilityFilterPipeline_ = pipelineManager_.createComputePipeline(
      pipelineInfo, "bim_visibility_filter");

  destroyOwnedShaderModule(device_->device(), compModule, nullptr);
  visibilityFilterStats_.computeReady =
      visibilityFilterPipeline_ != VK_NULL_HANDLE &&
      visibilityFilterDescriptorSet_ != VK_NULL_HANDLE;
  visibilityFilterDescriptorsDirty_ = true;
  writeVisibilityFilterDescriptorSet();
}

void BimManager::writeVisibilityFilterDescriptorSet() {
  if (visibilityFilterDescriptorSet_ == VK_NULL_HANDLE ||
      visibilityFilterMetadataBuffer_.buffer == VK_NULL_HANDLE ||
      visibilityMaskBuffer_.buffer == VK_NULL_HANDLE ||
      visibilityFilterStats_.metadataBufferBytes == 0u ||
      visibilityFilterStats_.visibilityMaskBufferBytes == 0u) {
    visibilityFilterDescriptorsDirty_ = true;
    visibilityFilterDispatchPending_ = false;
    visibilityFilterStats_.dispatchPending = false;
    return;
  }

  const std::array<VkDescriptorBufferInfo, 2> bufferInfos{{
      {visibilityFilterMetadataBuffer_.buffer, 0,
       visibilityFilterStats_.metadataBufferBytes},
      {visibilityMaskBuffer_.buffer, 0,
       visibilityFilterStats_.visibilityMaskBufferBytes},
  }};
  std::array<VkWriteDescriptorSet, 2> writes{};
  for (uint32_t binding = 0; binding < writes.size(); ++binding) {
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = visibilityFilterDescriptorSet_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[binding].descriptorCount = 1;
    writes[binding].pBufferInfo = &bufferInfos[binding];
  }
  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
  visibilityFilterDescriptorsDirty_ = false;
  visibilityFilterStats_.computeReady =
      visibilityFilterPipeline_ != VK_NULL_HANDLE;
  visibilityFilterDispatchPending_ =
      visibilityFilterStats_.computeReady &&
      visibilityFilterStats_.objectCount > 0u &&
      !visibilityFilterMaskCurrent_ &&
      (visibilityFilterHasActiveMask(visibilityFilterSettings_) ||
       !visibilityFilterMaskAllVisible_);
  visibilityFilterStats_.dispatchPending = visibilityFilterDispatchPending_;
}

void BimManager::destroyDrawCompactionBuffers(
    BimDrawCompactionSlotResources &slot) {
  if (slot.inputBuffer.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(slot.inputBuffer);
  }
  if (slot.indirectBuffer.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(slot.indirectBuffer);
  }
  if (slot.countBuffer.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(slot.countBuffer);
  }
  slot.uploadScratch.clear();
  slot.stats = {};
  slot.stats.computeReady =
      drawCompactionSupported() && drawCompactionPipeline_ != VK_NULL_HANDLE;
  slot.inputSourceData = nullptr;
  slot.inputSourceSize = 0u;
  slot.inputSourceRevision = 0u;
  slot.descriptorsDirty = true;
  slot.dispatchPending = false;
}

void BimManager::destroyDrawCompactionBuffers() {
  for (BimDrawCompactionSlotResources &slot : drawCompactionSlots_) {
    destroyDrawCompactionBuffers(slot);
  }
}

void BimManager::invalidateDrawCompactionOutputs(bool requestDispatch) {
  for (BimDrawCompactionSlotResources &slot : drawCompactionSlots_) {
    slot.stats.drawsValid = false;
    const bool canDispatch = requestDispatch && slot.stats.computeReady &&
                             slot.stats.inputDrawCount > 0u &&
                             slot.stats.outputCapacity > 0u;
    slot.dispatchPending = canDispatch;
    slot.stats.dispatchPending = canDispatch;
  }
}

void BimManager::destroyDrawCompactionComputeResources() {
  pipelineManager_.destroyPipeline(drawCompactionPipeline_);
  pipelineManager_.destroyPipelineLayout(drawCompactionPipelineLayout_);
  pipelineManager_.destroyDescriptorPool(drawCompactionDescriptorPool_);
  pipelineManager_.destroyDescriptorSetLayout(drawCompactionSetLayout_);
  drawCompactionDescriptorSets_.fill(VK_NULL_HANDLE);
  for (BimDrawCompactionSlotResources &slot : drawCompactionSlots_) {
    slot.descriptorsDirty = false;
    slot.dispatchPending = false;
    slot.stats.computeReady = false;
    slot.stats.dispatchPending = false;
    slot.stats.drawsValid = false;
  }
}

void BimManager::createDrawCompactionResources(
    const std::filesystem::path &shaderDir) {
  if (drawCompactionPipeline_ != VK_NULL_HANDLE) {
    return;
  }
  if (drawCompactionSetLayout_ != VK_NULL_HANDLE ||
      drawCompactionDescriptorPool_ != VK_NULL_HANDLE ||
      drawCompactionPipelineLayout_ != VK_NULL_HANDLE) {
    destroyDrawCompactionComputeResources();
  }
  if (!device_) {
    throw std::runtime_error(
        "BimManager::createDrawCompactionResources requires a device");
  }

  const std::array<VkDescriptorSetLayoutBinding, 5> bindings{{
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
  }};
  const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0u);
  drawCompactionSetLayout_ = pipelineManager_.createDescriptorSetLayout(
      {bindings.begin(), bindings.end()}, flags);

  drawCompactionDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        5u * static_cast<uint32_t>(kBimDrawCompactionSlotCount)}},
      static_cast<uint32_t>(kBimDrawCompactionSlotCount), 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = drawCompactionDescriptorPool_;
  allocInfo.descriptorSetCount =
      static_cast<uint32_t>(drawCompactionDescriptorSets_.size());
  std::array<VkDescriptorSetLayout, kBimDrawCompactionSlotCount> setLayouts{};
  setLayouts.fill(drawCompactionSetLayout_);
  allocInfo.pSetLayouts = setLayouts.data();
  if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
                               drawCompactionDescriptorSets_.data()) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate BIM draw compaction sets");
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(BimDrawCompactionPushConstants);
  drawCompactionPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {drawCompactionSetLayout_}, {pcRange});

  const std::filesystem::path compPath =
      shaderDir / "spv_shaders" / "bim_draw_compact.comp.spv";
  if (!std::filesystem::exists(compPath)) {
    for (BimDrawCompactionSlotResources &slot : drawCompactionSlots_) {
      slot.stats.computeReady = false;
      slot.descriptorsDirty = true;
    }
    return;
  }

  const auto spvData = container::util::readFile(compPath);
  VkShaderModule compModule =
      container::gpu::createShaderModule(device_->device(), spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName = "computeMain";

  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = stage;
  pipelineInfo.layout = drawCompactionPipelineLayout_;
  drawCompactionPipeline_ =
      pipelineManager_.createComputePipeline(pipelineInfo, "bim_draw_compact");

  destroyOwnedShaderModule(device_->device(), compModule, nullptr);
  for (size_t index = 0; index < drawCompactionSlots_.size(); ++index) {
    BimDrawCompactionSlotResources &slot = drawCompactionSlots_[index];
    slot.stats.computeReady =
        drawCompactionSupported() &&
        drawCompactionPipeline_ != VK_NULL_HANDLE &&
        drawCompactionDescriptorSets_[index] != VK_NULL_HANDLE;
    slot.descriptorsDirty = true;
    writeDrawCompactionDescriptorSet(static_cast<BimDrawCompactionSlot>(index));
  }
}

void BimManager::createMeshletResidencyResources(
    const std::filesystem::path &shaderDir) {
  if (meshletResidencyPipeline_ != VK_NULL_HANDLE) {
    createVisibilityFilterResources(shaderDir);
    createDrawCompactionResources(shaderDir);
    return;
  }
  if (meshletResidencySetLayout_ != VK_NULL_HANDLE ||
      meshletResidencyDescriptorPool_ != VK_NULL_HANDLE ||
      meshletResidencyPipelineLayout_ != VK_NULL_HANDLE) {
    destroyMeshletResidencyComputeResources();
  }
  if (!device_) {
    throw std::runtime_error(
        "BimManager::createMeshletResidencyResources requires a device");
  }

  const std::array<VkDescriptorSetLayoutBinding, 5> bindings{{
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
  }};
  const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0u);
  meshletResidencySetLayout_ = pipelineManager_.createDescriptorSetLayout(
      {bindings.begin(), bindings.end()}, flags);

  meshletResidencyDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}},
      1, 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = meshletResidencyDescriptorPool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &meshletResidencySetLayout_;
  if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
                               &meshletResidencyDescriptorSet_) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate BIM meshlet residency set");
  }

  VkPushConstantRange pcRange{};
  pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcRange.offset = 0;
  pcRange.size = sizeof(BimMeshletResidencyPushConstants);
  meshletResidencyPipelineLayout_ = pipelineManager_.createPipelineLayout(
      {meshletResidencySetLayout_}, {pcRange});

  const std::filesystem::path compPath =
      shaderDir / "spv_shaders" / "bim_meshlet_residency.comp.spv";
  if (!std::filesystem::exists(compPath)) {
    meshletResidencyStats_.computeReady = false;
    meshletResidencyDescriptorsDirty_ = true;
    createVisibilityFilterResources(shaderDir);
    createDrawCompactionResources(shaderDir);
    return;
  }

  const auto spvData = container::util::readFile(compPath);
  VkShaderModule compModule =
      container::gpu::createShaderModule(device_->device(), spvData);

  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = compModule;
  stage.pName = "computeMain";

  VkComputePipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipelineInfo.stage = stage;
  pipelineInfo.layout = meshletResidencyPipelineLayout_;
  meshletResidencyPipeline_ = pipelineManager_.createComputePipeline(
      pipelineInfo, "bim_meshlet_residency");

  destroyOwnedShaderModule(device_->device(), compModule, nullptr);
  meshletResidencyStats_.computeReady =
      meshletResidencyPipeline_ != VK_NULL_HANDLE &&
      meshletResidencyDescriptorSet_ != VK_NULL_HANDLE;
  if (meshletResidencyStats_.gpuResident) {
    meshletResidencyDescriptorsDirty_ = true;
    writeMeshletResidencyDescriptorSet();
  }
  createVisibilityFilterResources(shaderDir);
  createDrawCompactionResources(shaderDir);
}

void BimManager::updateMeshletResidencySettings(
    const BimMeshletResidencySettings &settings) {
  meshletResidencySettings_ = settings;
  meshletResidencySettings_.screenErrorPixels =
      std::clamp(meshletResidencySettings_.screenErrorPixels, 0.25f, 64.0f);
  meshletResidencySettings_.viewportHeightPixels =
      std::max(meshletResidencySettings_.viewportHeightPixels, 1.0f);
  meshletResidencyDispatchPending_ = meshletResidencyStats_.gpuResident;
  meshletResidencyStats_.dispatchPending = meshletResidencyDispatchPending_;
}

void BimManager::updateVisibilityFilterSettings(const BimDrawFilter &filter) {
  BimVisibilityFilterPushConstants settings{};
  settings.objectCount = static_cast<uint32_t>(
      std::min<size_t>(visibilityFilterStats_.objectCount,
                       std::numeric_limits<uint32_t>::max()));
  settings.selectedObjectIndex = filter.selectedObjectIndex;
  settings.drawBudgetMaxObjects = filter.drawBudgetMaxObjects;

  auto setLabelFilter = [&](bool enabled, const std::string &label,
                            BimMetadataSemanticCategory category, uint32_t flag,
                            uint32_t &targetId) {
    if (!enabled || label.empty()) {
      return;
    }
    settings.flags |= flag;
    targetId = metadataCatalog_->semanticIdForCategory(category, label);
  };

  setLabelFilter(filter.typeFilterEnabled, filter.type,
                 BimMetadataSemanticCategory::Type, kBimVisibilityFilterType,
                 settings.typeId);
  setLabelFilter(filter.storeyFilterEnabled, filter.storey,
                 BimMetadataSemanticCategory::Storey,
                 kBimVisibilityFilterStorey, settings.storeyId);
  setLabelFilter(filter.materialFilterEnabled, filter.material,
                 BimMetadataSemanticCategory::Material,
                 kBimVisibilityFilterMaterial, settings.materialId);
  setLabelFilter(filter.disciplineFilterEnabled, filter.discipline,
                 BimMetadataSemanticCategory::Discipline,
                 kBimVisibilityFilterDiscipline, settings.disciplineId);
  setLabelFilter(filter.phaseFilterEnabled, filter.phase,
                 BimMetadataSemanticCategory::Phase, kBimVisibilityFilterPhase,
                 settings.phaseId);
  setLabelFilter(filter.fireRatingFilterEnabled, filter.fireRating,
                 BimMetadataSemanticCategory::FireRating,
                 kBimVisibilityFilterFireRating, settings.fireRatingId);
  setLabelFilter(filter.loadBearingFilterEnabled, filter.loadBearing,
                 BimMetadataSemanticCategory::LoadBearing,
                 kBimVisibilityFilterLoadBearing, settings.loadBearingId);
  setLabelFilter(filter.statusFilterEnabled, filter.status,
                 BimMetadataSemanticCategory::Status,
                 kBimVisibilityFilterStatus, settings.statusId);

  if (filter.drawBudgetEnabled && filter.drawBudgetMaxObjects > 0u) {
    settings.flags |= kBimVisibilityFilterDrawBudget;
  }
  if (filter.isolateSelection &&
      filter.selectedObjectIndex != std::numeric_limits<uint32_t>::max()) {
    settings.flags |= kBimVisibilityFilterIsolateSelection;
  }
  if (filter.hideSelection &&
      filter.selectedObjectIndex != std::numeric_limits<uint32_t>::max()) {
    settings.flags |= kBimVisibilityFilterHideSelection;
  }
  if (const BimElementMetadata *selected =
          metadataForObject(filter.selectedObjectIndex)) {
    settings.selectedProductId = selected->productIdentityId;
  }

  const bool settingsChanged =
      !sameVisibilityFilterSettings(visibilityFilterSettings_, settings);
  const bool activeMask = visibilityFilterHasActiveMask(settings);
  visibilityFilterSettings_ = settings;

  if (!visibilityFilterStats_.gpuResident ||
      visibilityFilterStats_.objectCount == 0u) {
    visibilityFilterMaskCurrent_ = false;
    visibilityFilterMaskAllVisible_ = false;
    visibilityFilterDispatchPending_ = false;
    visibilityFilterStats_.dispatchPending = false;
    invalidateDrawCompactionOutputs(false);
    return;
  }

  // The upload path seeds an all-visible mask, so inactive filters can reuse it
  // instead of redispatching a full object-count compute pass every frame.
  if (!activeMask && visibilityFilterMaskAllVisible_) {
    visibilityFilterMaskCurrent_ = true;
    visibilityFilterDispatchPending_ = false;
  } else if (!settingsChanged && visibilityFilterMaskCurrent_) {
    visibilityFilterDispatchPending_ = false;
  } else if (visibilityFilterStats_.computeReady &&
             visibilityFilterDescriptorSet_ != VK_NULL_HANDLE) {
    visibilityFilterMaskCurrent_ = false;
    visibilityFilterDispatchPending_ = true;
  } else {
    visibilityFilterMaskCurrent_ = false;
    visibilityFilterDispatchPending_ = false;
  }

  visibilityFilterStats_.dispatchPending = visibilityFilterDispatchPending_;
  if (settingsChanged || visibilityFilterDispatchPending_) {
    invalidateDrawCompactionOutputs(visibilityMaskReadyForDrawCompaction());
  }
}

bool BimManager::visibilityMaskReadyForDrawCompaction() const {
  if (!visibilityFilterStats_.gpuResident ||
      visibilityFilterStats_.objectCount == 0u ||
      visibilityMaskBuffer_.buffer == VK_NULL_HANDLE ||
      visibilityFilterStats_.visibilityMaskBufferBytes == 0u) {
    return false;
  }
  if (!visibilityFilterHasActiveMask(visibilityFilterSettings_)) {
    return visibilityFilterMaskAllVisible_;
  }
  // Active filters require a completed compute pass. A resident all-visible
  // mask alone is not enough, otherwise missing filter shaders would bypass
  // CPU-filtered BIM draw lists.
  return visibilityFilterStats_.computeReady &&
         !visibilityFilterDescriptorsDirty_ &&
         !visibilityFilterDispatchPending_ && visibilityFilterMaskCurrent_;
}

bool BimManager::drawCompactionSupported() const {
  return device_ != nullptr &&
         device_->enabledFeatures().drawIndirectFirstInstance == VK_TRUE &&
         device_->enabledFeatures().multiDrawIndirect == VK_TRUE &&
         device_->enabledVulkan12Features().drawIndirectCount == VK_TRUE;
}

void BimManager::writeMeshletResidencyDescriptorSet(
    VkBuffer cameraBuffer, VkDeviceSize cameraBufferSize, VkBuffer objectBuffer,
    VkDeviceSize objectBufferSize) {
  if (cameraBuffer != VK_NULL_HANDLE && cameraBufferSize > 0u) {
    meshletResidencyCameraBuffer_ = cameraBuffer;
    meshletResidencyCameraBufferSize_ = cameraBufferSize;
  }
  if (objectBuffer != VK_NULL_HANDLE && objectBufferSize > 0u) {
    meshletResidencyObjectBuffer_ = objectBuffer;
    meshletResidencyObjectBufferSize_ = objectBufferSize;
  }

  if (meshletResidencyDescriptorSet_ == VK_NULL_HANDLE ||
      meshletClusterBuffer_.buffer == VK_NULL_HANDLE ||
      meshletObjectLodBuffer_.buffer == VK_NULL_HANDLE ||
      meshletResidencyBuffer_.buffer == VK_NULL_HANDLE ||
      meshletResidencyCameraBuffer_ == VK_NULL_HANDLE ||
      meshletResidencyObjectBuffer_ == VK_NULL_HANDLE ||
      meshletResidencyStats_.clusterBufferBytes == 0u ||
      meshletResidencyStats_.objectLodBufferBytes == 0u ||
      meshletResidencyStats_.residencyBufferBytes == 0u ||
      meshletResidencyCameraBufferSize_ == 0u ||
      meshletResidencyObjectBufferSize_ == 0u) {
    meshletResidencyDescriptorsDirty_ = true;
    meshletResidencyDispatchPending_ = false;
    meshletResidencyStats_.dispatchPending = false;
    return;
  }

  const std::array<VkDescriptorBufferInfo, 5> bufferInfos{{
      {meshletClusterBuffer_.buffer, 0,
       meshletResidencyStats_.clusterBufferBytes},
      {meshletObjectLodBuffer_.buffer, 0,
       meshletResidencyStats_.objectLodBufferBytes},
      {meshletResidencyBuffer_.buffer, 0,
       meshletResidencyStats_.residencyBufferBytes},
      {meshletResidencyCameraBuffer_, 0, meshletResidencyCameraBufferSize_},
      {meshletResidencyObjectBuffer_, 0, meshletResidencyObjectBufferSize_},
  }};
  std::array<VkWriteDescriptorSet, 5> writes{};
  for (uint32_t binding = 0; binding < writes.size(); ++binding) {
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = meshletResidencyDescriptorSet_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorType = binding == 3u
                                         ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[binding].descriptorCount = 1;
    writes[binding].pBufferInfo = &bufferInfos[binding];
  }
  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
  meshletResidencyDescriptorsDirty_ = false;
  meshletResidencyStats_.computeReady =
      meshletResidencyPipeline_ != VK_NULL_HANDLE;
  meshletResidencyDispatchPending_ = meshletResidencyStats_.computeReady;
  meshletResidencyStats_.dispatchPending = meshletResidencyDispatchPending_;
}

void BimManager::recordVisibilityFilterUpdate(VkCommandBuffer cmd) {
  if (cmd == VK_NULL_HANDLE || visibilityFilterPipeline_ == VK_NULL_HANDLE ||
      visibilityFilterPipelineLayout_ == VK_NULL_HANDLE ||
      visibilityFilterDescriptorSet_ == VK_NULL_HANDLE ||
      visibilityFilterStats_.objectCount == 0u) {
    return;
  }
  if (visibilityFilterDescriptorsDirty_) {
    writeVisibilityFilterDescriptorSet();
  }
  if (visibilityFilterDescriptorsDirty_ || !visibilityFilterDispatchPending_) {
    return;
  }

  BimVisibilityFilterPushConstants pushConstants = visibilityFilterSettings_;
  pushConstants.objectCount = static_cast<uint32_t>(
      std::min<size_t>(visibilityFilterStats_.objectCount,
                       std::numeric_limits<uint32_t>::max()));

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    visibilityFilterPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          visibilityFilterPipelineLayout_, 0, 1,
                          &visibilityFilterDescriptorSet_, 0, nullptr);
  vkCmdPushConstants(cmd, visibilityFilterPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(BimVisibilityFilterPushConstants), &pushConstants);
  const uint32_t groupCount = (pushConstants.objectCount + 63u) / 64u;
  vkCmdDispatch(cmd, groupCount, 1, 1);

  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                       nullptr, 0, nullptr);

  visibilityFilterDispatchPending_ = false;
  visibilityFilterStats_.dispatchPending = false;
  visibilityFilterMaskCurrent_ = true;
  visibilityFilterMaskAllVisible_ =
      !visibilityFilterHasActiveMask(pushConstants);
  invalidateDrawCompactionOutputs(true);
}

void BimManager::recordMeshletResidencyUpdate(VkCommandBuffer cmd,
                                              VkBuffer cameraBuffer,
                                              VkDeviceSize cameraBufferSize,
                                              VkBuffer objectBuffer,
                                              VkDeviceSize objectBufferSize) {
  if (cmd == VK_NULL_HANDLE || meshletResidencyPipeline_ == VK_NULL_HANDLE ||
      meshletResidencyPipelineLayout_ == VK_NULL_HANDLE ||
      meshletResidencyDescriptorSet_ == VK_NULL_HANDLE ||
      meshletResidencyStats_.objectCount == 0u) {
    return;
  }
  if (meshletResidencySettings_.pauseStreaming) {
    meshletResidencyStats_.dispatchPending = meshletResidencyDispatchPending_;
    return;
  }
  if (meshletResidencyDescriptorsDirty_ ||
      meshletResidencyCameraBuffer_ != cameraBuffer ||
      meshletResidencyCameraBufferSize_ != cameraBufferSize ||
      meshletResidencyObjectBuffer_ != objectBuffer ||
      meshletResidencyObjectBufferSize_ != objectBufferSize) {
    writeMeshletResidencyDescriptorSet(cameraBuffer, cameraBufferSize,
                                       objectBuffer, objectBufferSize);
  }
  if (meshletResidencyDescriptorsDirty_ || !meshletResidencyDispatchPending_) {
    return;
  }

  BimMeshletResidencyPushConstants pushConstants{};
  pushConstants.objectCount = static_cast<uint32_t>(
      std::min<size_t>(meshletResidencyStats_.objectCount,
                       std::numeric_limits<uint32_t>::max()));
  pushConstants.forceResident =
      meshletResidencySettings_.forceResident ? 1u : 0u;
  pushConstants.drawBudgetMaxObjects =
      meshletResidencySettings_.drawBudgetEnabled
          ? meshletResidencySettings_.drawBudgetMaxObjects
          : 0u;
  pushConstants.selectedObjectIndex =
      meshletResidencySettings_.selectedObjectIndex;
  pushConstants.lodBias = meshletResidencySettings_.lodBias;
  pushConstants.screenErrorPixels = meshletResidencySettings_.screenErrorPixels;
  pushConstants.viewportHeightPixels =
      meshletResidencySettings_.viewportHeightPixels;
  pushConstants.flags =
      (meshletResidencySettings_.autoLod ? 1u : 0u) |
      (meshletResidencySettings_.drawBudgetEnabled ? 2u : 0u) |
      (meshletResidencySettings_.keepSelectedResident ? 4u : 0u) |
      (meshletResidencySettings_.pauseStreaming ? 8u : 0u);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    meshletResidencyPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          meshletResidencyPipelineLayout_, 0, 1,
                          &meshletResidencyDescriptorSet_, 0, nullptr);
  vkCmdPushConstants(cmd, meshletResidencyPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(BimMeshletResidencyPushConstants), &pushConstants);
  const uint32_t groupCount = (pushConstants.objectCount + 63u) / 64u;
  vkCmdDispatch(cmd, groupCount, 1, 1);

  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);

  meshletResidencyDispatchPending_ =
      meshletResidencySettings_.autoLod ||
      meshletResidencySettings_.drawBudgetEnabled;
  meshletResidencyStats_.dispatchPending = meshletResidencyDispatchPending_;
  invalidateDrawCompactionOutputs(visibilityMaskReadyForDrawCompaction());
}

void BimManager::ensureDrawCompactionCapacity(BimDrawCompactionSlot slotId,
                                              size_t inputDrawCount,
                                              size_t outputCapacity) {
  if (inputDrawCount == 0u || outputCapacity == 0u) {
    return;
  }
  BimDrawCompactionSlotResources &slot =
      drawCompactionSlots_[drawCompactionSlotIndex(slotId)];
  const size_t clampedInputCount =
      std::min(inputDrawCount,
               static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
  const size_t clampedOutputCapacity =
      std::min(outputCapacity,
               static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
  const size_t requiredInputCapacity = std::max<size_t>(64u, clampedInputCount);
  const size_t requiredOutputCapacity =
      std::max<size_t>(64u, clampedOutputCapacity);
  const size_t currentInputCapacity =
      slot.stats.inputBufferBytes /
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand);
  const size_t currentOutputCapacity =
      slot.stats.outputBufferBytes /
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand);
  if (slot.inputBuffer.buffer != VK_NULL_HANDLE &&
      slot.indirectBuffer.buffer != VK_NULL_HANDLE &&
      slot.countBuffer.buffer != VK_NULL_HANDLE &&
      currentInputCapacity >= requiredInputCapacity &&
      currentOutputCapacity >= requiredOutputCapacity) {
    return;
  }

  destroyDrawCompactionBuffers(slot);

  slot.inputBuffer = allocationManager_.createBuffer(
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand) *
          requiredInputCapacity,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  slot.indirectBuffer = allocationManager_.createBuffer(
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand) *
          requiredOutputCapacity,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
  slot.countBuffer = allocationManager_.createBuffer(
      sizeof(uint32_t),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  slot.stats.gpuResident = slot.inputBuffer.buffer != VK_NULL_HANDLE &&
                           slot.indirectBuffer.buffer != VK_NULL_HANDLE &&
                           slot.countBuffer.buffer != VK_NULL_HANDLE;
  slot.stats.inputBufferBytes = static_cast<VkDeviceSize>(
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand) *
      requiredInputCapacity);
  slot.stats.outputBufferBytes = static_cast<VkDeviceSize>(
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand) *
      requiredOutputCapacity);
  slot.stats.countBufferBytes = sizeof(uint32_t);
  slot.stats.outputCapacity = requiredOutputCapacity;
  slot.stats.computeReady =
      drawCompactionSupported() &&
      drawCompactionPipeline_ != VK_NULL_HANDLE &&
      drawCompactionDescriptorSets_[drawCompactionSlotIndex(slotId)] !=
          VK_NULL_HANDLE;
  slot.stats.drawsValid = false;
  slot.descriptorsDirty = true;
  writeDrawCompactionDescriptorSet(slotId);
}

void BimManager::writeDrawCompactionDescriptorSet(
    BimDrawCompactionSlot slotId) {
  const size_t slotIndex = drawCompactionSlotIndex(slotId);
  BimDrawCompactionSlotResources &slot = drawCompactionSlots_[slotIndex];
  if (drawCompactionDescriptorSets_[slotIndex] == VK_NULL_HANDLE ||
      slot.inputBuffer.buffer == VK_NULL_HANDLE ||
      meshletResidencyBuffer_.buffer == VK_NULL_HANDLE ||
      slot.indirectBuffer.buffer == VK_NULL_HANDLE ||
      slot.countBuffer.buffer == VK_NULL_HANDLE ||
      visibilityMaskBuffer_.buffer == VK_NULL_HANDLE ||
      slot.stats.inputBufferBytes == 0u ||
      meshletResidencyStats_.residencyBufferBytes == 0u ||
      slot.stats.outputBufferBytes == 0u || slot.stats.countBufferBytes == 0u ||
      visibilityFilterStats_.visibilityMaskBufferBytes == 0u) {
    slot.descriptorsDirty = true;
    slot.dispatchPending = false;
    slot.stats.dispatchPending = false;
    return;
  }

  const std::array<VkDescriptorBufferInfo, 5> bufferInfos{{
      {slot.inputBuffer.buffer, 0, slot.stats.inputBufferBytes},
      {meshletResidencyBuffer_.buffer, 0,
       meshletResidencyStats_.residencyBufferBytes},
      {slot.indirectBuffer.buffer, 0, slot.stats.outputBufferBytes},
      {slot.countBuffer.buffer, 0, slot.stats.countBufferBytes},
      {visibilityMaskBuffer_.buffer, 0,
       visibilityFilterStats_.visibilityMaskBufferBytes},
  }};
  std::array<VkWriteDescriptorSet, 5> writes{};
  for (uint32_t binding = 0; binding < writes.size(); ++binding) {
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = drawCompactionDescriptorSets_[slotIndex];
    writes[binding].dstBinding = binding;
    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[binding].descriptorCount = 1;
    writes[binding].pBufferInfo = &bufferInfos[binding];
  }
  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
  slot.descriptorsDirty = false;
  slot.stats.computeReady =
      drawCompactionSupported() && drawCompactionPipeline_ != VK_NULL_HANDLE;
  slot.dispatchPending =
      slot.stats.computeReady && slot.stats.inputDrawCount > 0u &&
      !slot.stats.drawsValid && visibilityMaskReadyForDrawCompaction();
  slot.stats.dispatchPending = slot.dispatchPending;
}

void BimManager::prepareDrawCompaction(
    BimDrawCompactionSlot slotId, const std::vector<DrawCommand> &commands) {
  BimDrawCompactionSlotResources &slot =
      drawCompactionSlots_[drawCompactionSlotIndex(slotId)];
  if (!drawCompactionSupported() || commands.empty() ||
      meshletResidencyBuffer_.buffer == VK_NULL_HANDLE ||
      meshletResidencyStats_.objectCount == 0u) {
    slot.stats.drawsValid = false;
    slot.stats.inputDrawCount = 0u;
    slot.stats.computeReady = false;
    slot.inputSourceData = nullptr;
    slot.inputSourceSize = 0u;
    slot.inputSourceRevision = 0u;
    slot.dispatchPending = false;
    slot.stats.dispatchPending = false;
    return;
  }

  const bool sourceUnchanged =
      slot.inputSourceData == commands.data() &&
      slot.inputSourceSize == commands.size() &&
      slot.inputSourceRevision == objectDataRevision_ &&
      slot.stats.inputDrawCount == commands.size() &&
      slot.inputBuffer.buffer != VK_NULL_HANDLE &&
      slot.stats.inputBufferBytes >=
          sizeof(container::gpu::GpuDrawIndexedIndirectCommand) *
              commands.size();
  if (sourceUnchanged) {
    if (slot.descriptorsDirty) {
      writeDrawCompactionDescriptorSet(slotId);
    }
    // Static draw streams keep their uploaded GPU input. Residency or filter
    // changes invalidate only the compacted output, which is cheap to rebuild.
    if (!slot.stats.drawsValid && visibilityMaskReadyForDrawCompaction()) {
      slot.dispatchPending = slot.stats.computeReady &&
                             slot.stats.inputDrawCount > 0u &&
                             slot.stats.outputCapacity > 0u;
      slot.stats.dispatchPending = slot.dispatchPending;
    }
    return;
  }

  slot.stats.drawsValid = false;
  slot.stats.inputDrawCount = 0u;
  slot.dispatchPending = false;
  slot.stats.dispatchPending = false;

  const size_t outputCapacity = bimDrawCompactionOutputCapacity(commands);
  if (outputCapacity == 0u) {
    return;
  }

  ensureDrawCompactionCapacity(slotId, commands.size(), outputCapacity);
  if (!slot.stats.gpuResident || slot.inputBuffer.buffer == VK_NULL_HANDLE) {
    return;
  }

  slot.inputSourceData = commands.data();
  slot.inputSourceSize = commands.size();
  slot.inputSourceRevision = objectDataRevision_;
  const uint32_t inputDrawCount = static_cast<uint32_t>(std::min<size_t>(
      commands.size(),
      slot.stats.inputBufferBytes /
          sizeof(container::gpu::GpuDrawIndexedIndirectCommand)));
  slot.uploadScratch.resize(inputDrawCount);
  for (uint32_t index = 0; index < inputDrawCount; ++index) {
    const DrawCommand &command = commands[index];
    container::gpu::GpuDrawIndexedIndirectCommand gpuCommand{};
    gpuCommand.indexCount = command.indexCount;
    gpuCommand.instanceCount = std::max(command.instanceCount, 1u);
    gpuCommand.firstIndex = command.firstIndex;
    gpuCommand.vertexOffset = 0;
    gpuCommand.firstInstance = command.objectIndex;
    slot.uploadScratch[index] = gpuCommand;
  }

  SceneController::writeToBuffer(
      allocationManager_, slot.inputBuffer, slot.uploadScratch.data(),
      sizeof(container::gpu::GpuDrawIndexedIndirectCommand) * inputDrawCount);

  slot.stats.inputDrawCount = inputDrawCount;
  slot.stats.outputCapacity =
      std::max(slot.stats.outputCapacity, outputCapacity);
  if (slot.descriptorsDirty) {
    writeDrawCompactionDescriptorSet(slotId);
  }
  slot.dispatchPending = slot.stats.computeReady && !slot.descriptorsDirty &&
                         visibilityMaskReadyForDrawCompaction();
  slot.stats.dispatchPending = slot.dispatchPending;
}

void BimManager::prepareDrawCompaction(
    const std::vector<DrawCommand> &commands) {
  prepareDrawCompaction(BimDrawCompactionSlot::OpaqueSingleSided, commands);
}

void BimManager::recordDrawCompactionUpdate(VkCommandBuffer cmd) {
  if (!drawCompactionSupported() || cmd == VK_NULL_HANDLE ||
      drawCompactionPipeline_ == VK_NULL_HANDLE ||
      drawCompactionPipelineLayout_ == VK_NULL_HANDLE ||
      meshletResidencyStats_.objectCount == 0u ||
      !visibilityMaskReadyForDrawCompaction()) {
    return;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    drawCompactionPipeline_);
  for (size_t index = 0; index < drawCompactionSlots_.size(); ++index) {
    BimDrawCompactionSlotResources &slot = drawCompactionSlots_[index];
    const BimDrawCompactionSlot slotId =
        static_cast<BimDrawCompactionSlot>(index);
    if (slot.stats.inputDrawCount == 0u || slot.stats.outputCapacity == 0u ||
        drawCompactionDescriptorSets_[index] == VK_NULL_HANDLE) {
      continue;
    }
    if (slot.descriptorsDirty) {
      writeDrawCompactionDescriptorSet(slotId);
    }
    if (slot.descriptorsDirty || !slot.dispatchPending) {
      continue;
    }

    vkCmdFillBuffer(cmd, slot.countBuffer.buffer, 0, sizeof(uint32_t), 0);

    VkMemoryBarrier fillBarrier{};
    fillBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &fillBarrier, 0, nullptr, 0, nullptr);

    BimDrawCompactionPushConstants pushConstants{};
    pushConstants.inputDrawCount = static_cast<uint32_t>(std::min<size_t>(
        slot.stats.inputDrawCount, std::numeric_limits<uint32_t>::max()));
    pushConstants.outputCapacity = static_cast<uint32_t>(std::min<size_t>(
        slot.stats.outputCapacity, std::numeric_limits<uint32_t>::max()));
    pushConstants.objectCount = static_cast<uint32_t>(
        std::min<size_t>(meshletResidencyStats_.objectCount,
                         std::numeric_limits<uint32_t>::max()));
    pushConstants.flags = 0u;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            drawCompactionPipelineLayout_, 0, 1,
                            &drawCompactionDescriptorSets_[index], 0, nullptr);
    vkCmdPushConstants(cmd, drawCompactionPipelineLayout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(BimDrawCompactionPushConstants), &pushConstants);
    const uint32_t groupCount = (pushConstants.inputDrawCount + 63u) / 64u;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    VkMemoryBarrier drawBarrier{};
    drawBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    drawBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    drawBarrier.dstAccessMask =
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &drawBarrier, 0, nullptr, 0, nullptr);

    slot.dispatchPending = false;
    slot.stats.dispatchPending = false;
    slot.stats.drawsValid = true;
  }
}

const BimDrawCompactionStats &
BimManager::drawCompactionStats(BimDrawCompactionSlot slot) const {
  return drawCompactionSlots_[drawCompactionSlotIndex(slot)].stats;
}

bool BimManager::drawCompactionReady(BimDrawCompactionSlot slotId) const {
  const size_t slotIndex = drawCompactionSlotIndex(slotId);
  const BimDrawCompactionSlotResources &slot = drawCompactionSlots_[slotIndex];
  return drawCompactionSupported() && visibilityMaskReadyForDrawCompaction() &&
         slot.stats.drawsValid &&
         slot.indirectBuffer.buffer != VK_NULL_HANDLE &&
         slot.countBuffer.buffer != VK_NULL_HANDLE;
}

void BimManager::drawCompacted(BimDrawCompactionSlot slotId,
                               VkCommandBuffer cmd) const {
  const size_t slotIndex = drawCompactionSlotIndex(slotId);
  const BimDrawCompactionSlotResources &slot = drawCompactionSlots_[slotIndex];
  if (cmd == VK_NULL_HANDLE || !drawCompactionReady(slotId)) {
    return;
  }
  const uint32_t maxDrawCount = static_cast<uint32_t>(std::min<size_t>(
      slot.stats.outputCapacity, std::numeric_limits<uint32_t>::max()));
  if (maxDrawCount == 0u) {
    return;
  }
  vkCmdDrawIndexedIndirectCount(
      cmd, slot.indirectBuffer.buffer, 0, slot.countBuffer.buffer, 0,
      maxDrawCount, sizeof(container::gpu::GpuDrawIndexedIndirectCommand));
}

void BimManager::drawCompactedOpaqueSingleSided(VkCommandBuffer cmd) const {
  drawCompacted(BimDrawCompactionSlot::OpaqueSingleSided, cmd);
}

void BimManager::uploadVisibilityFilterBuffers() {
  destroyVisibilityFilterBuffers();
  if (elementMetadata_.empty()) {
    return;
  }

  visibilityFilterMetadata_.reserve(elementMetadata_.size());
  std::vector<uint32_t> visibilityMask;
  visibilityMask.reserve(elementMetadata_.size());
  for (const BimElementMetadata &metadata : elementMetadata_) {
    visibilityFilterMetadata_.push_back(
        metadataCatalog_->visibilityGpuMetadata(metadata));
    visibilityMask.push_back(1u);
  }

  auto asBytes = [](const auto &values) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(values.data()),
        sizeof(typename std::decay_t<decltype(values)>::value_type) *
            values.size());
  };

  visibilityFilterMetadataBuffer_ = allocationManager_.uploadBuffer(
      asBytes(visibilityFilterMetadata_), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  visibilityMaskBuffer_ = allocationManager_.uploadBuffer(
      asBytes(visibilityMask),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

  visibilityFilterStats_.gpuResident =
      visibilityFilterMetadataBuffer_.buffer != VK_NULL_HANDLE &&
      visibilityMaskBuffer_.buffer != VK_NULL_HANDLE;
  visibilityFilterStats_.objectCount = visibilityFilterMetadata_.size();
  visibilityFilterStats_.metadataBufferBytes =
      static_cast<VkDeviceSize>(sizeof(BimVisibilityGpuObjectMetadata) *
                                visibilityFilterMetadata_.size());
  visibilityFilterStats_.visibilityMaskBufferBytes =
      static_cast<VkDeviceSize>(sizeof(uint32_t) * visibilityMask.size());
  visibilityFilterStats_.computeReady =
      visibilityFilterPipeline_ != VK_NULL_HANDLE &&
      visibilityFilterDescriptorSet_ != VK_NULL_HANDLE;
  visibilityFilterDescriptorsDirty_ = true;
  visibilityFilterMaskAllVisible_ = visibilityFilterStats_.gpuResident;
  visibilityFilterMaskCurrent_ =
      visibilityFilterMaskAllVisible_ &&
      !visibilityFilterHasActiveMask(visibilityFilterSettings_);
  visibilityFilterDispatchPending_ =
      visibilityFilterStats_.gpuResident &&
      visibilityFilterHasActiveMask(visibilityFilterSettings_);
  visibilityFilterStats_.dispatchPending = visibilityFilterDispatchPending_;
  writeVisibilityFilterDescriptorSet();

  for (BimDrawCompactionSlotResources &slot : drawCompactionSlots_) {
    slot.descriptorsDirty = true;
  }
}

void BimManager::uploadMeshletResidencyBuffers() {
  destroyMeshletResidencyBuffers();
  if (meshletClusters_.empty() || objectLodMetadata_.empty()) {
    return;
  }

  std::vector<BimMeshletGpuCluster> gpuClusters;
  gpuClusters.reserve(meshletClusters_.size());
  for (const BimMeshletClusterMetadata &cluster : meshletClusters_) {
    BimMeshletGpuCluster gpuCluster{};
    gpuCluster.drawRange = {
        cluster.meshId,
        cluster.firstIndex,
        cluster.indexCount,
        cluster.triangleCount,
    };
    gpuCluster.lodInfo = {
        cluster.lodLevel,
        cluster.estimated ? 1u : 0u,
        0u,
        0u,
    };
    gpuCluster.boundsCenterRadius =
        glm::vec4(cluster.boundsCenter, cluster.boundsRadius);
    gpuClusters.push_back(gpuCluster);
  }

  std::vector<BimMeshletGpuObjectLod> gpuObjectLods;
  std::vector<BimMeshletResidencyEntry> residencyEntries;
  gpuObjectLods.reserve(objectLodMetadata_.size());
  residencyEntries.reserve(objectLodMetadata_.size());

  size_t residentObjectCount = 0;
  size_t residentClusterCount = 0;
  for (const BimObjectLodStreamingMetadata &lod : objectLodMetadata_) {
    BimMeshletGpuObjectLod gpuObject{};
    gpuObject.objectInfo = {
        lod.objectIndex,
        lod.sourceElementIndex,
        lod.meshId,
        static_cast<uint32_t>(lod.geometryKind),
    };
    gpuObject.clusterInfo = {
        lod.firstCluster,
        lod.clusterCount,
        lod.maxLodLevel,
        lod.estimatedClusters ? 1u : 0u,
    };
    gpuObjectLods.push_back(gpuObject);

    BimMeshletResidencyEntry residency{};
    residency.flags = lod.clusterCount > 0u ? 1u : 0u;
    residency.selectedLod = 0u;
    residency.firstCluster = lod.firstCluster;
    residency.clusterCount = lod.clusterCount;
    residencyEntries.push_back(residency);

    if (lod.clusterCount > 0u) {
      ++residentObjectCount;
      residentClusterCount += lod.clusterCount;
    }
  }

  auto asBytes = [](const auto &values) {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(values.data()),
        sizeof(typename std::decay_t<decltype(values)>::value_type) *
            values.size());
  };

  meshletClusterBuffer_ = allocationManager_.uploadBuffer(
      asBytes(gpuClusters), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  meshletObjectLodBuffer_ = allocationManager_.uploadBuffer(
      asBytes(gpuObjectLods), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  meshletResidencyBuffer_ = allocationManager_.uploadBuffer(
      asBytes(residencyEntries),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

  meshletResidencyStats_.gpuResident =
      meshletClusterBuffer_.buffer != VK_NULL_HANDLE &&
      meshletObjectLodBuffer_.buffer != VK_NULL_HANDLE &&
      meshletResidencyBuffer_.buffer != VK_NULL_HANDLE;
  meshletResidencyStats_.objectCount = objectLodMetadata_.size();
  meshletResidencyStats_.clusterCount = meshletClusters_.size();
  meshletResidencyStats_.residentObjectCount = residentObjectCount;
  meshletResidencyStats_.residentClusterCount = residentClusterCount;
  meshletResidencyStats_.clusterBufferBytes = static_cast<VkDeviceSize>(
      sizeof(BimMeshletGpuCluster) * gpuClusters.size());
  meshletResidencyStats_.objectLodBufferBytes = static_cast<VkDeviceSize>(
      sizeof(BimMeshletGpuObjectLod) * gpuObjectLods.size());
  meshletResidencyStats_.residencyBufferBytes = static_cast<VkDeviceSize>(
      sizeof(BimMeshletResidencyEntry) * residencyEntries.size());
  meshletResidencyStats_.computeReady =
      meshletResidencyPipeline_ != VK_NULL_HANDLE &&
      meshletResidencyDescriptorSet_ != VK_NULL_HANDLE;
  meshletResidencyDescriptorsDirty_ = true;
  writeMeshletResidencyDescriptorSet();
}

void BimManager::uploadObjects() {
  const bool recreated = SceneController::ensureObjectBufferCapacity(
      allocationManager_, objectBuffer_, objectBufferCapacity_,
      objectData_.size());
  (void)recreated;
  if (objectBuffer_.buffer == VK_NULL_HANDLE || objectData_.empty()) {
    return;
  }

  SceneController::writeToBuffer(
      allocationManager_, objectBuffer_, objectData_.data(),
      sizeof(container::gpu::ObjectData) * objectData_.size());
  ++objectDataRevision_;
}

} // namespace container::renderer
