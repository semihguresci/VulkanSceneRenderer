#include "Container/renderer/scene/SceneController.h"
#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::ObjectData;

namespace {

bool transformFlipsWinding(const glm::mat4& transform) {
  const glm::vec3 x(transform[0]);
  const glm::vec3 y(transform[1]);
  const glm::vec3 z(transform[2]);
  const float determinant =
      x.x * (y.y * z.z - y.z * z.y) -
      y.x * (x.y * z.z - x.z * z.y) +
      z.x * (x.y * y.z - x.z * y.y);
  return determinant < 0.0f;
}

void expandBounds(glm::vec3 &boundsMin, glm::vec3 &boundsMax,
                  const glm::vec3 &point) {
  boundsMin = glm::min(boundsMin, point);
  boundsMax = glm::max(boundsMax, point);
}

bool intersectRaySphere(const glm::vec3& origin, const glm::vec3& direction,
                        const glm::vec3& center, float radius,
                        float& outDistance) {
  if (radius <= 0.0f) {
    return false;
  }

  const glm::vec3 toCenter = center - origin;
  const float projected = glm::dot(toCenter, direction);
  const float centerDistance2 = glm::dot(toCenter, toCenter);
  const float radius2 = radius * radius;
  const float closestDistance2 =
      centerDistance2 - projected * projected;
  if (closestDistance2 > radius2) {
    return false;
  }

  const float halfChord =
      std::sqrt(std::max(0.0f, radius2 - closestDistance2));
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

PickRay makePickRay(const container::gpu::CameraData& cameraData,
                    VkExtent2D viewportExtent,
                    double cursorX,
                    double cursorY) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u) {
    return {};
  }

  const float ndcX =
      static_cast<float>((cursorX / static_cast<double>(viewportExtent.width)) *
                             2.0 -
                         1.0);
  const float ndcY =
      static_cast<float>(1.0 -
                         (cursorY / static_cast<double>(viewportExtent.height)) *
                             2.0);

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

bool intersectRayTriangle(const glm::vec3& origin,
                          const glm::vec3& direction,
                          const glm::vec3& v0,
                          const glm::vec3& v1,
                          const glm::vec3& v2,
                          TriangleCullMode cullMode,
                          float& outDistance) {
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

float projectDepth(const container::gpu::CameraData& cameraData,
                   const glm::vec3& worldPosition) {
  const glm::vec4 clip =
      cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (clip.w == 0.0f) {
    return 0.0f;
  }
  return std::clamp(clip.z / clip.w, 0.0f, 1.0f);
}

bool sectionPlaneClips(const glm::vec3& worldPosition, bool enabled,
                       const glm::vec4& plane) {
  return enabled && glm::dot(glm::vec3(plane), worldPosition) + plane.w < 0.0f;
}

}  // namespace

SceneController::SceneController(
    std::shared_ptr<container::gpu::VulkanDevice>  device,
    container::gpu::AllocationManager&             allocationManager,
    container::gpu::PipelineManager&             pipelineManager,
    container::scene::SceneGraph&                     sceneGraph,
    container::scene::SceneManager&                   sceneManager,
    container::ui::GuiManager*                        guiManager,
    const container::app::AppConfig&                           config)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , pipelineManager_(pipelineManager)
    , sceneGraph_(sceneGraph)
    , sceneManager_(sceneManager)
    , guiManager_(guiManager)
    , config_(config)
    , world_(std::make_unique<container::ecs::World>()) {
}

SceneController::~SceneController() = default;

container::ecs::World& SceneController::world() { return *world_; }
const container::ecs::World& SceneController::world() const { return *world_; }

void SceneController::rebuildPrimitiveBoundsCache() {
  const auto& primitiveRanges = sceneManager_.primitiveRanges();
  const auto& verts = sceneManager_.vertices();
  const auto& indices = sceneManager_.indices();

  primitiveBounds_.assign(primitiveRanges.size(), {});
  for (size_t primitiveIndex = 0; primitiveIndex < primitiveRanges.size();
       ++primitiveIndex) {
    const auto& primitive = primitiveRanges[primitiveIndex];
    const size_t begin = primitive.firstIndex;
    if (begin >= indices.size() || primitive.indexCount == 0u) {
      continue;
    }
    const size_t end =
        std::min(indices.size(), begin + static_cast<size_t>(primitive.indexCount));

    bool hasVertex = false;
    glm::vec3 localMin(std::numeric_limits<float>::max());
    glm::vec3 localMax(std::numeric_limits<float>::lowest());
    for (size_t i = begin; i < end; ++i) {
      const uint32_t vertexIndex = indices[i];
      if (vertexIndex >= verts.size()) {
        continue;
      }
      const glm::vec3& pos = verts[vertexIndex].position;
      localMin = glm::min(localMin, pos);
      localMax = glm::max(localMax, pos);
      hasVertex = true;
    }
    if (!hasVertex) {
      continue;
    }

    const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
    float localRadius = 0.0f;
    for (size_t i = begin; i < end; ++i) {
      const uint32_t vertexIndex = indices[i];
      if (vertexIndex >= verts.size()) {
        continue;
      }
      localRadius = std::max(
          localRadius, glm::length(verts[vertexIndex].position - localCenter));
    }
    primitiveBounds_[primitiveIndex] = {localCenter, localRadius, true};
  }
}

void SceneController::invalidateObjectDataCache() {
  objectDataCacheValid_ = false;
  cachedSceneGraphRevision_ = std::numeric_limits<uint64_t>::max();
  objectBufferUploadRevisions_.clear();
}

void SceneController::refreshObjectDataCache(bool showDiagCube) {
  const uint64_t sceneGraphRevision = sceneGraph_.revision();
  if (objectDataCacheValid_ &&
      cachedSceneGraphRevision_ == sceneGraphRevision &&
      cachedShowDiagCube_ == showDiagCube) {
    return;
  }

  syncObjectDataFromSceneGraph(showDiagCube);
}

// ---------------------------------------------------------------------------
// Static buffer helpers
// ---------------------------------------------------------------------------

void SceneController::writeToBuffer(
    container::gpu::AllocationManager&      allocationManager,
    const container::gpu::AllocatedBuffer&  buffer,
    const void*                              data,
    size_t                                   size) {
  void* mapped = buffer.allocation_info.pMappedData;
  bool  mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map buffer for writing");
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
    throw std::runtime_error("failed to flush buffer after writing");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                   buffer.allocation);
  }
}

bool SceneController::ensureObjectBufferCapacity(
    container::gpu::AllocationManager&  allocationManager,
    container::gpu::AllocatedBuffer&    objectBuffer,
    size_t&                              objectBufferCapacity,
    size_t                               requiredObjectCount) {
  const size_t required = std::max<size_t>(1, requiredObjectCount);
  if (objectBuffer.buffer != VK_NULL_HANDLE &&
      objectBufferCapacity >= required) {
    return false;
  }

  if (objectBuffer.buffer != VK_NULL_HANDLE) {
    allocationManager.destroyBuffer(objectBuffer);
    objectBufferCapacity = 0;
  }

  objectBuffer = allocationManager.createBuffer(
      sizeof(ObjectData) * required, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  objectBufferCapacity = required;
  return true;
}

// ---------------------------------------------------------------------------
// Geometry buffers
// ---------------------------------------------------------------------------

void SceneController::createGeometryBuffers() {
  const auto& sceneVertices = sceneManager_.vertices();
  const auto& sceneIndices  = sceneManager_.indices();
  const container::geometry::Mesh cube = makeScenePrimitiveCube();

  std::vector<container::geometry::Vertex> mergedVertices;
  mergedVertices.reserve(sceneVertices.size() + cube.vertices().size());
  mergedVertices.insert(mergedVertices.end(), sceneVertices.begin(),
                        sceneVertices.end());
  const uint32_t diagVertexBase =
      static_cast<uint32_t>(mergedVertices.size());
  mergedVertices.insert(mergedVertices.end(), cube.vertices().begin(),
                        cube.vertices().end());

  std::vector<uint32_t> mergedIndices;
  mergedIndices.reserve(sceneIndices.size() + cube.indices().size());
  mergedIndices.insert(mergedIndices.end(), sceneIndices.begin(),
                       sceneIndices.end());
  const VkDeviceSize diagIndexByteOffset =
      static_cast<VkDeviceSize>(mergedIndices.size()) * sizeof(uint32_t);
  for (const uint32_t index : cube.indices()) {
    mergedIndices.push_back(index + diagVertexBase);
  }

  vertexSlice_ = allocationManager_.uploadVertices(
      std::span<const container::geometry::Vertex>(mergedVertices));
  indexSlice_ = allocationManager_.uploadIndices(
      std::span<const uint32_t>(mergedIndices));

  diagCubeVertexSlice_ = vertexSlice_;
  diagCubeIndexSlice_  = indexSlice_;
  diagCubeIndexSlice_.offset = indexSlice_.offset + diagIndexByteOffset;
  diagCubeIndexSlice_.size =
      static_cast<VkDeviceSize>(cube.indices().size()) * sizeof(uint32_t);
  diagCubeIndexCount_ = static_cast<uint32_t>(cube.indices().size());
  rebuildPrimitiveBoundsCache();
  invalidateObjectDataCache();
}

void SceneController::createSceneBuffers(
    const container::gpu::AllocatedBuffer& cameraBuffer,
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity) {
  const VkBuffer previousObjectBuffer = objectBuffer.buffer;
  const bool bufferRecreated =
      ensureObjectBufferCapacity(allocationManager_, objectBuffer,
                                 objectBufferCapacity,
                                 sceneGraph_.renderableNodes().size());
  if (bufferRecreated) {
    objectBufferUploadRevisions_.erase(previousObjectBuffer);
    objectBufferUploadRevisions_.erase(objectBuffer.buffer);
  }
  refreshObjectDataCache(false);
  // Upload is deferred to updateObjectBuffer; descriptor sets updated by
  // caller.
}

// ---------------------------------------------------------------------------
// syncObjectDataFromSceneGraph
// ---------------------------------------------------------------------------

void SceneController::syncObjectDataFromSceneGraph(bool showDiagCube) {
  sceneGraph_.updateWorldTransforms();

  // Sync ECS registry from the scene graph so views are up-to-date.
  world_->syncFromSceneGraph(sceneGraph_);

  objectData_.clear();
  objectNodeIndices_.clear();
  opaqueDrawCommands_.clear();
  transparentDrawCommands_.clear();
  opaqueSingleSidedDrawCommands_.clear();
  opaqueWindingFlippedDrawCommands_.clear();
  opaqueDoubleSidedDrawCommands_.clear();
  transparentSingleSidedDrawCommands_.clear();
  transparentWindingFlippedDrawCommands_.clear();
  transparentDoubleSidedDrawCommands_.clear();
  const uint32_t renderableCount = world_->renderableCount();
  objectData_.reserve(renderableCount);
  objectNodeIndices_.reserve(renderableCount);
  opaqueDrawCommands_.reserve(renderableCount);
  transparentDrawCommands_.reserve(renderableCount);
  opaqueSingleSidedDrawCommands_.reserve(renderableCount);
  opaqueWindingFlippedDrawCommands_.reserve(renderableCount);
  opaqueDoubleSidedDrawCommands_.reserve(renderableCount);
  transparentSingleSidedDrawCommands_.reserve(renderableCount);
  transparentWindingFlippedDrawCommands_.reserve(renderableCount);
  transparentDoubleSidedDrawCommands_.reserve(renderableCount);

  world_->forEachRenderableWithNode(
      [this](const container::ecs::TransformComponent& transform,
             const container::ecs::MeshComponent&      mesh,
             const container::ecs::MaterialComponent&   material,
             const container::ecs::SceneNodeRef&        nodeRef) {
        if (mesh.primitiveIndex == std::numeric_limits<uint32_t>::max() ||
            mesh.primitiveIndex >= sceneManager_.primitiveRanges().size()) {
          return;
        }

        const auto& primitive =
            sceneManager_.primitiveRanges()[mesh.primitiveIndex];
        const auto materialProperties =
            sceneManager_.materialRenderProperties(material.materialIndex);
        const uint32_t materialIndex = materialProperties.gpuMaterialIndex;

        ObjectData object{};
        object.model = transform.worldTransform;
        object.objectInfo.x = materialIndex;
        object.temporalInfo.z = nodeRef.nodeIndex;
        object.temporalInfo.w = mesh.primitiveIndex;
        {
          const glm::mat3 model3  = glm::mat3(transform.worldTransform);
          const glm::mat3 normal3 = glm::transpose(glm::inverse(model3));
          object.normalMatrix0 = glm::vec4(normal3[0], 0.0f);
          object.normalMatrix1 = glm::vec4(normal3[1], 0.0f);
          object.normalMatrix2 = glm::vec4(normal3[2], 0.0f);
        }
        const bool materialDoubleSided = materialProperties.doubleSided;
        const bool materialTransparent = materialProperties.transparent;
        const bool rasterDoubleSided =
            materialDoubleSided || primitive.disableBackfaceCulling;
        const bool windingFlipped =
            transformFlipsWinding(transform.worldTransform);
        object.objectInfo.y =
            rasterDoubleSided ? container::gpu::kObjectFlagDoubleSided : 0u;

        if (mesh.primitiveIndex < primitiveBounds_.size()) {
          const auto& bounds = primitiveBounds_[mesh.primitiveIndex];
          if (bounds.valid) {
            const glm::vec3 worldCenter = glm::vec3(
                transform.worldTransform * glm::vec4(bounds.center, 1.0f));
            const float scaleMax = std::max({
                glm::length(glm::vec3(transform.worldTransform[0])),
                glm::length(glm::vec3(transform.worldTransform[1])),
                glm::length(glm::vec3(transform.worldTransform[2]))});
            const float heightInflation =
                std::abs(materialProperties.heightScale) * scaleMax;
            object.boundingSphere =
                glm::vec4(worldCenter, bounds.radius * scaleMax + heightInflation);
          }
        }

        const uint32_t objectIndex =
            static_cast<uint32_t>(objectData_.size());
        objectData_.push_back(object);
        objectNodeIndices_.push_back(nodeRef.nodeIndex);

        DrawCommand drawCommand{};
        drawCommand.objectIndex = objectIndex;
        drawCommand.firstIndex  = primitive.firstIndex;
        drawCommand.indexCount  = primitive.indexCount;

        if (materialTransparent) {
          transparentDrawCommands_.push_back(drawCommand);
          if (rasterDoubleSided) {
            transparentDoubleSidedDrawCommands_.push_back(drawCommand);
          } else if (windingFlipped) {
            transparentWindingFlippedDrawCommands_.push_back(drawCommand);
          } else {
            transparentSingleSidedDrawCommands_.push_back(drawCommand);
          }
        } else {
          opaqueDrawCommands_.push_back(drawCommand);
          if (rasterDoubleSided) {
            opaqueDoubleSidedDrawCommands_.push_back(drawCommand);
          } else if (windingFlipped) {
            opaqueWindingFlippedDrawCommands_.push_back(drawCommand);
          } else {
            opaqueSingleSidedDrawCommands_.push_back(drawCommand);
          }
        }
      });

  // Diagnostic cube: appended last so its objectIndex is always known.
  diagCubeObjectIndex_ = std::numeric_limits<uint32_t>::max();
  if (showDiagCube && diagCubeVertexSlice_.buffer != VK_NULL_HANDLE) {
    ObjectData cubeObject{};
    glm::vec3  diagCenter{0.0f};
    float      diagScale = 0.5f;
    const auto& bounds = sceneManager_.modelBounds();
    if (bounds.valid) {
      diagCenter = bounds.center +
                   glm::vec3(bounds.radius * 1.75f, 0.0f, 0.0f);
      diagScale = std::max(0.25f, bounds.radius * 0.45f);
    }
    const glm::mat4 cubeModel =
        glm::translate(glm::mat4(1.0f), diagCenter) *
        glm::scale(glm::mat4(1.0f), glm::vec3(diagScale));
    cubeObject.model = cubeModel;
    const glm::mat3 cubeNormal =
        glm::transpose(glm::inverse(glm::mat3(cubeModel)));
    cubeObject.normalMatrix0 = glm::vec4(cubeNormal[0], 0.0f);
    cubeObject.normalMatrix1 = glm::vec4(cubeNormal[1], 0.0f);
    cubeObject.normalMatrix2 = glm::vec4(cubeNormal[2], 0.0f);
    cubeObject.objectInfo.x = sceneManager_.diagnosticMaterialIndex();
    cubeObject.temporalInfo.z = container::scene::SceneGraph::kInvalidNode;
    cubeObject.temporalInfo.w = std::numeric_limits<uint32_t>::max();
    diagCubeObjectIndex_ = static_cast<uint32_t>(objectData_.size());
    objectData_.push_back(cubeObject);
    objectNodeIndices_.push_back(container::scene::SceneGraph::kInvalidNode);
  }

  cachedSceneGraphRevision_ = sceneGraph_.revision();
  cachedShowDiagCube_ = showDiagCube;
  objectDataCacheValid_ = true;
  ++objectDataRevision_;
  ++objectBufferUploadRevision_;
}

// ---------------------------------------------------------------------------
// updateObjectBuffer
// ---------------------------------------------------------------------------

bool SceneController::updateObjectBuffer(
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity,
    const container::gpu::AllocatedBuffer& cameraBuffer) {
  const VkBuffer previousObjectBuffer = objectBuffer.buffer;
  const bool showDiagCube =
      guiManager_ && guiManager_->showNormalDiagCube() &&
      diagCubeVertexSlice_.buffer != VK_NULL_HANDLE;
  refreshObjectDataCache(showDiagCube);

  const bool bufferRecreated = ensureObjectBufferCapacity(
      allocationManager_, objectBuffer, objectBufferCapacity,
      objectData_.size());
  if (bufferRecreated) {
    objectBufferUploadRevisions_.erase(previousObjectBuffer);
    objectBufferUploadRevisions_.erase(objectBuffer.buffer);
  }
  if (objectBuffer.buffer == VK_NULL_HANDLE || objectData_.empty())
    return bufferRecreated;

  const auto uploadRecord =
      objectBufferUploadRevisions_.find(objectBuffer.buffer);
  const uint64_t uploadedRevision =
      uploadRecord == objectBufferUploadRevisions_.end()
          ? std::numeric_limits<uint64_t>::max()
          : uploadRecord->second;
  if (uploadedRevision != objectBufferUploadRevision_) {
    writeToBuffer(allocationManager_, objectBuffer, objectData_.data(),
                  sizeof(ObjectData) * objectData_.size());
    objectBufferUploadRevisions_[objectBuffer.buffer] =
        objectBufferUploadRevision_;
  }
  return bufferRecreated;
}

uint32_t SceneController::pickRenderableNode(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableNodeHit(cameraData, viewportExtent, cursorX, cursorY,
                               sectionPlaneEnabled, sectionPlane)
      .nodeIndex;
}

SceneNodePickHit SceneController::pickRenderableNodeHit(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableNodeHitForDraws(cameraData, viewportExtent, cursorX,
                                       cursorY, true, true,
                                       sectionPlaneEnabled, sectionPlane);
}

SceneNodePickHit SceneController::pickTransparentRenderableNodeHit(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableNodeHitForDraws(cameraData, viewportExtent, cursorX,
                                       cursorY, false, true,
                                       sectionPlaneEnabled, sectionPlane);
}

SceneNodePickHit SceneController::pickRenderableNodeHitForDraws(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool includeOpaque,
    bool includeTransparent,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  if (objectData_.empty()) {
    return {};
  }

  const PickRay ray = makePickRay(cameraData, viewportExtent, cursorX, cursorY);
  if (!ray.valid) {
    return {};
  }

  const auto& vertices = sceneManager_.vertices();
  const auto& indices = sceneManager_.indices();
  SceneNodePickHit nearest{};
  auto testDrawCommands = [&](const std::vector<DrawCommand>& commands,
                              TriangleCullMode cullMode) {
    for (const DrawCommand& command : commands) {
      if (command.firstIndex >= indices.size() || command.indexCount < 3u) {
        continue;
      }
      const size_t firstIndex = command.firstIndex;
      const size_t indexCount =
          std::min(static_cast<size_t>(command.indexCount),
                   indices.size() - firstIndex);
      const size_t endIndex = firstIndex + indexCount;
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);

      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        if (objectIndex >= objectData_.size() ||
            objectIndex >= objectNodeIndices_.size()) {
          continue;
        }
        const uint32_t nodeIndex = objectNodeIndices_[objectIndex];
        if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
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

        const glm::mat4& model = objectData_[objectIndex].model;
        for (size_t index = firstIndex; index + 2u < endIndex; index += 3u) {
          const uint32_t i0 = indices[index];
          const uint32_t i1 = indices[index + 1u];
          const uint32_t i2 = indices[index + 2u];
          if (i0 >= vertices.size() || i1 >= vertices.size() ||
              i2 >= vertices.size()) {
            continue;
          }

          const glm::vec3 v0 =
              glm::vec3(model * glm::vec4(vertices[i0].position, 1.0f));
          const glm::vec3 v1 =
              glm::vec3(model * glm::vec4(vertices[i1].position, 1.0f));
          const glm::vec3 v2 =
              glm::vec3(model * glm::vec4(vertices[i2].position, 1.0f));
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
            nearest.nodeIndex = nodeIndex;
            nearest.distance = hitDistance;
            nearest.depth = projectDepth(cameraData, hitPosition);
            nearest.worldPosition = hitPosition;
            nearest.hasWorldPosition = true;
            nearest.hit = true;
          }
        }
      }
    }
  };

  const bool hasSplitDrawCommands =
      !opaqueSingleSidedDrawCommands_.empty() ||
      !transparentSingleSidedDrawCommands_.empty() ||
      !opaqueWindingFlippedDrawCommands_.empty() ||
      !transparentWindingFlippedDrawCommands_.empty() ||
      !opaqueDoubleSidedDrawCommands_.empty() ||
      !transparentDoubleSidedDrawCommands_.empty();
  if (hasSplitDrawCommands) {
    if (includeOpaque) {
      testDrawCommands(opaqueSingleSidedDrawCommands_, TriangleCullMode::Back);
    }
    if (includeTransparent) {
      testDrawCommands(transparentSingleSidedDrawCommands_,
                       TriangleCullMode::Back);
    }
    if (includeOpaque) {
      testDrawCommands(opaqueWindingFlippedDrawCommands_,
                       TriangleCullMode::Front);
    }
    if (includeTransparent) {
      testDrawCommands(transparentWindingFlippedDrawCommands_,
                       TriangleCullMode::Front);
    }
    if (includeOpaque) {
      testDrawCommands(opaqueDoubleSidedDrawCommands_, TriangleCullMode::None);
    }
    if (includeTransparent) {
      testDrawCommands(transparentDoubleSidedDrawCommands_,
                       TriangleCullMode::None);
    }
  } else {
    if (includeOpaque) {
      testDrawCommands(opaqueDrawCommands_, TriangleCullMode::None);
    }
    if (includeTransparent) {
      testDrawCommands(transparentDrawCommands_, TriangleCullMode::None);
    }
  }

  return nearest;
}

uint32_t SceneController::nodeIndexForObject(uint32_t objectIndex) const {
  if (objectIndex >= objectNodeIndices_.size()) {
    return container::scene::SceneGraph::kInvalidNode;
  }
  return objectNodeIndices_[objectIndex];
}

SceneNodeWorldBounds SceneController::nodeWorldBounds(uint32_t nodeIndex) const {
  if (nodeIndex == container::scene::SceneGraph::kInvalidNode ||
      objectData_.empty()) {
    return {};
  }

  std::unordered_set<uint32_t> nodeSubtree;
  std::vector<uint32_t> stack{nodeIndex};
  while (!stack.empty()) {
    const uint32_t current = stack.back();
    stack.pop_back();
    if (!nodeSubtree.insert(current).second) {
      continue;
    }
    if (const auto *node = sceneGraph_.getNode(current)) {
      for (const uint32_t child : node->children) {
        stack.push_back(child);
      }
    }
  }
  if (nodeSubtree.empty()) {
    return {};
  }

  glm::vec3 boundsMin{std::numeric_limits<float>::max()};
  glm::vec3 boundsMax{std::numeric_limits<float>::lowest()};
  bool hasBounds = false;
  for (uint32_t objectIndex = 0u;
       objectIndex < objectData_.size() && objectIndex < objectNodeIndices_.size();
       ++objectIndex) {
    if (!nodeSubtree.contains(objectNodeIndices_[objectIndex])) {
      continue;
    }

    const glm::vec4 sphere = objectData_[objectIndex].boundingSphere;
    const glm::vec3 center{sphere};
    const float radius = std::max(sphere.w, 0.0f);
    if (radius > 0.0f && std::isfinite(radius)) {
      expandBounds(boundsMin, boundsMax, center - glm::vec3(radius));
      expandBounds(boundsMin, boundsMax, center + glm::vec3(radius));
      hasBounds = true;
      continue;
    }

    const glm::vec3 origin{objectData_[objectIndex].model[3]};
    expandBounds(boundsMin, boundsMax, origin);
    hasBounds = true;
  }

  if (!hasBounds) {
    return {};
  }

  SceneNodeWorldBounds bounds{};
  bounds.valid = true;
  bounds.min = boundsMin;
  bounds.max = boundsMax;
  bounds.center = (boundsMin + boundsMax) * 0.5f;
  bounds.size = boundsMax - boundsMin;
  bounds.radius = std::max(0.25f, glm::length(bounds.size) * 0.5f);
  return bounds;
}

void SceneController::collectDrawCommandsForNode(
    uint32_t nodeIndex, std::vector<DrawCommand>& outCommands) const {
  outCommands.clear();
  if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
    return;
  }

  auto appendMatching = [&](const std::vector<DrawCommand>& commands) {
    for (const DrawCommand& command : commands) {
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);
      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        if (objectIndex < objectNodeIndices_.size() &&
            objectNodeIndices_[objectIndex] == nodeIndex) {
          DrawCommand selected = command;
          selected.objectIndex = objectIndex;
          selected.instanceCount = 1u;
          outCommands.push_back(selected);
        }
      }
    }
  };

  appendMatching(opaqueDrawCommands_);
  appendMatching(transparentDrawCommands_);
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

void SceneController::buildSceneGraph(uint32_t& outRootNode,
                                      uint32_t& outSelectedMeshNode,
                                      uint32_t& outCubeNode) {
  sceneManager_.populateSceneGraph(sceneGraph_);
  outRootNode = container::scene::SceneGraph::kInvalidNode;

  const uint32_t existingNodeCount =
      static_cast<uint32_t>(sceneGraph_.nodeCount());
  if (existingNodeCount > 0) {
    outRootNode = sceneGraph_.createNode(
        glm::mat4(1.0f), sceneManager_.defaultMaterialIndex(), false,
        container::scene::SceneGraph::kInvalidNode, "Scene Root");
    for (uint32_t i = 0; i < existingNodeCount; ++i) {
      const auto* node = sceneGraph_.getNode(i);
      if (node != nullptr &&
          node->parent == container::scene::SceneGraph::kInvalidNode) {
        sceneGraph_.setParent(i, outRootNode);
      }
    }
    sceneGraph_.updateWorldTransforms();
  }

  const auto& renderable = sceneGraph_.renderableNodes();
  if (renderable.empty()) {
    outSelectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  } else if (std::find(renderable.begin(), renderable.end(),
                       outSelectedMeshNode) == renderable.end()) {
    outSelectedMeshNode = renderable.front();
  }
  outCubeNode = container::scene::SceneGraph::kInvalidNode;
  invalidateObjectDataCache();
}

void SceneController::addSceneObject(
    const glm::mat4&                         transform,
    uint32_t                                 rootNode,
    container::gpu::AllocatedBuffer&        objectBuffer,
    size_t&                                  objectBufferCapacity,
    const container::gpu::AllocatedBuffer&  cameraBuffer) {
  if (sceneGraph_.renderableNodes().size() >= config_.maxSceneObjects) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Reached maximum scene object capacity");
    }
    return;
  }
  const uint32_t node = sceneGraph_.createNode(
      transform, sceneManager_.defaultMaterialIndex(), true);
  sceneGraph_.setParent(node, rootNode);
  updateObjectBuffer(objectBuffer, objectBufferCapacity, cameraBuffer);
  if (guiManager_) {
    guiManager_->setStatusMessage("Added object to scene");
  }
}

uint32_t SceneController::addScenePrimitive(ScenePrimitiveKind kind,
                                            const glm::mat4& transform,
                                            uint32_t& rootNode) {
  if (sceneGraph_.renderableNodes().size() >= config_.maxSceneObjects) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Reached maximum scene object capacity");
    }
    return container::scene::SceneGraph::kInvalidNode;
  }

  const int32_t materialIndex =
      sceneManager_.defaultMaterialIndex() ==
              std::numeric_limits<uint32_t>::max()
          ? -1
          : static_cast<int32_t>(sceneManager_.defaultMaterialIndex());
  const uint32_t primitiveIndex =
      sceneManager_.appendRuntimeMesh(makeScenePrimitive(kind, materialIndex));
  if (primitiveIndex == std::numeric_limits<uint32_t>::max()) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Failed to add primitive");
    }
    return container::scene::SceneGraph::kInvalidNode;
  }

  if (rootNode == container::scene::SceneGraph::kInvalidNode) {
    rootNode = sceneGraph_.createNode(glm::mat4(1.0f),
                                      sceneManager_.defaultMaterialIndex(),
                                      false);
  }

  createGeometryBuffers();
  const uint32_t node = sceneGraph_.createNode(
      transform, sceneManager_.defaultMaterialIndex(), true, primitiveIndex,
      std::string(scenePrimitiveKindLabel(kind)));
  sceneGraph_.setParent(node, rootNode);
  sceneGraph_.updateWorldTransforms();
  invalidateObjectDataCache();

  if (guiManager_) {
    guiManager_->setStatusMessage("Added " +
                                  std::string(scenePrimitiveKindLabel(kind)));
  }
  return node;
}

bool SceneController::reloadSceneModel(
    const std::string&                      path,
    float                                   importScale,
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity,
    const container::gpu::AllocatedBuffer& cameraBuffer,
    VkIndexType&                            outIndexType,
    uint32_t&                               outRootNode,
    uint32_t&                               outSelectedMeshNode,
    uint32_t&                               outCubeNode) {
  vkDeviceWaitIdle(device_->device());
  const std::array<container::gpu::AllocatedBuffer, 1> cameraBuffers = {
      cameraBuffer};
  const bool result =
      sceneManager_.reloadModel(path, importScale, cameraBuffers, objectBuffer);
  outIndexType = sceneManager_.indexType();
  buildSceneGraph(outRootNode, outSelectedMeshNode, outCubeNode);
  ensureObjectBufferCapacity(allocationManager_, objectBuffer,
                             objectBufferCapacity,
                             sceneGraph_.renderableNodes().size());
  createGeometryBuffers();
  if (guiManager_) {
    guiManager_->setStatusMessage(
        result ? "Loaded model: " + path : "Failed to load model: " + path);
  }
  return result;
}

}  // namespace container::renderer
