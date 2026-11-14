#include "Container/renderer/lighting/LightingManager.h"
#include "Container/app/RendererLightingSettings.h"
#include "Container/ecs/World.h"
#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"
#include "Container/renderer/deferred/DeferredLightGizmoRecorder.h"
#include "Container/renderer/lighting/LightGizmoIconAtlas.h"
#include "Container/renderer/lighting/LtcLutResources.h"
#include "Container/renderer/lighting/LocalShadowLayerAllocator.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/Logger.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <glm/gtc/matrix_transform.hpp>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace container::renderer {

using container::gpu::AreaLightData;
using container::gpu::kClusterDepthSlices;
using container::gpu::kMaxAreaLights;
using container::gpu::kMaxClusteredLights;
using container::gpu::kMaxDeferredPointLights;
using container::gpu::kMaxLightsPerTile;
using container::gpu::kMaxShadowedLocalLightLayers;
using container::gpu::kTileSize;
using container::gpu::LightCullingStats;
using container::gpu::LightingData;
using container::gpu::LightingSettings;
using container::app::RendererLightingSetting;
using container::app::clampRendererLightingValue;
using container::gpu::PointLightData;
using container::gpu::TileCullPushConstants;
using container::gpu::TileLightGrid;

struct LightEditIdentityState {
  struct Row {
    EditableLightId id;uint64_t birth{},nodeBirth{};uint32_t graphNode{container::scene::SceneGraph::kInvalidNode};
    int32_t sourceNode{-1},definition{-1};std::string extension;
  };
  uint64_t asset{},graph{},scene{},generator{},bytes{};
  std::vector<Row> rows;
  std::vector<Row> inactiveImportedRows;
};
struct LightEditAddState {
  uint64_t owner{},asset{},graph{},scene{},birth{},pointPrefixBirth{},areaPrefixBirth{},generator{};
  size_t points{},areas{};EditableLightType type{};
  EditableLightEntity added;
  uint64_t beforeSelectionBirth{};
  PointLightData point;AreaLightData area;
  std::optional<EditableLightEntity> beforeDirectional;
  std::optional<LightEditIdentityState::Row> beforeDirectionalIdentity;
};
uint64_t SceneLightEdits::identityStorageBytes() const noexcept {return identities?identities->bytes:0;}

namespace {

uint64_t lightIdentity() {
  static std::atomic<uint64_t> next{1};auto value=next.load(std::memory_order_relaxed);
  for(;;) {if(value==UINT64_MAX) throw std::overflow_error("Light editor identities exhausted");
    if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed)) return value;}
}

bool poseFloat(float a,float b) noexcept {return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);}
template<glm::length_t N> bool poseVector(const glm::vec<N,float,glm::defaultp>& a,const glm::vec<N,float,glm::defaultp>& b) noexcept {
  for(glm::length_t i=0;i<N;++i) if(!poseFloat(a[i],b[i])) return false;return true;
}
bool poseMatrix(const glm::mat4& a,const glm::mat4& b) noexcept {
  for(int c=0;c<4;++c) if(!poseVector(a[c],b[c])) return false;return true;
}
bool posePayload(const PointLightData& a,const PointLightData& b) noexcept {
  return poseVector(a.positionRadius,b.positionRadius) && poseVector(a.colorIntensity,b.colorIntensity) &&
      poseVector(a.directionInnerCos,b.directionInnerCos) && poseVector(a.coneOuterCosType,b.coneOuterCosType) &&
      poseVector(a.iesTangent,b.iesTangent) && a.iesProfileAddress==b.iesProfileAddress;
}
bool posePayload(const AreaLightData& a,const AreaLightData& b) noexcept {
  return poseVector(a.positionRange,b.positionRange) && poseVector(a.colorIntensity,b.colorIntensity) && poseVector(a.directionType,b.directionType) &&
      poseVector(a.tangentHalfSize,b.tangentHalfSize) && poseVector(a.bitangentHalfSize,b.bitangentHalfSize);
}
bool posePayload(const container::scene::AuthoredDirectionalLight& a,const container::scene::AuthoredDirectionalLight& b) noexcept {
  return poseVector(a.direction,b.direction) && poseVector(a.colorIntensity,b.colorIntensity);
}
bool posePayload(const container::scene::AuthoredLightBinding& a,const container::scene::AuthoredLightBinding& b) noexcept {
  return a.sourceNode==b.sourceNode && a.extension==b.extension;
}
template<class T> bool posePayload(const std::optional<T>& a,const std::optional<T>& b) noexcept {
  return bool(a)==bool(b) && (!a || posePayload(*a,*b));
}
template<class A,class B> bool poseArray(const A& a,const B& b) noexcept {
  if(a.size()!=b.size()) return false;for(size_t i=0;i<a.size();++i) if(!posePayload(a[i],b[i])) return false;return true;
}
bool poseSettings(const LightingSettings& a,const LightingSettings& b) noexcept {
  return a.preset==b.preset && poseFloat(a.density,b.density) && poseFloat(a.radiusScale,b.radiusScale) &&
      poseFloat(a.intensityScale,b.intensityScale) && poseFloat(a.directionalIntensity,b.directionalIntensity) &&
      poseFloat(a.environmentIntensity,b.environmentIntensity) && poseFloat(a.bounceIntensity,b.bounceIntensity) &&
      a.localShadowPointBudget==b.localShadowPointBudget && a.localShadowLayerBudget==b.localShadowLayerBudget &&
      a.areaLightingMode==b.areaLightingMode && a.areaLightSampleCount==b.areaLightSampleCount &&
      a.areaEmitterDebug==b.areaEmitterDebug && a.environment==b.environment;
}
bool poseEditable(const std::optional<EditableLightEntity>& a,const std::optional<EditableLightEntity>& b) noexcept {
  if(bool(a)!=bool(b)) return false;if(!a) return true;
  return a->id.type==b->id.type && a->id.source==b->id.source && a->id.index==b->id.index && a->type==b->type && a->source==b->source &&
      a->label==b->label && poseVector(a->position,b->position) && poseVector(a->direction,b->direction) &&
      poseVector(a->tangent,b->tangent) && poseVector(a->bitangent,b->bitangent) && poseVector(a->color,b->color) &&
      poseFloat(a->intensity,b->intensity) && poseFloat(a->range,b->range) && poseFloat(a->innerConeDegrees,b->innerConeDegrees) &&
      poseFloat(a->outerConeDegrees,b->outerConeDegrees) && poseVector(a->areaHalfSize,b->areaHalfSize) &&
      poseFloat(a->areaShape,b->areaShape) && a->iesProfileAddress==b->iesProfileAddress &&
      a->selected==b->selected && a->editable==b->editable;
}
bool poseData(const LightingData& a,const LightingData& b) noexcept {
  return poseVector(a.directionalDirection,b.directionalDirection) && poseVector(a.directionalColorIntensity,b.directionalColorIntensity) &&
      a.pointLightCount==b.pointLightCount && a.shadowEnabled==b.shadowEnabled && a.gtaoEnabled==b.gtaoEnabled &&
      a.tileCullEnabled==b.tileCullEnabled && a.prefilteredMipCount==b.prefilteredMipCount &&
      poseFloat(a.environmentIntensity,b.environmentIntensity) && a.areaLightCount==b.areaLightCount &&
      a.localShadowEnabled==b.localShadowEnabled && poseFloat(a.bounceIntensity,b.bounceIntensity) &&
      a.localContactVisibility==b.localContactVisibility && a.areaLightingMode==b.areaLightingMode &&
      a.areaLightSampleCount==b.areaLightSampleCount &&
      poseVector(a.environmentOrientation,b.environmentOrientation) &&
      poseVector(a.environmentBackground,b.environmentBackground) &&
      poseVector(a.environmentSunDirection,b.environmentSunDirection) &&
      poseVector(a.environmentSunRadiance,b.environmentSunRadiance) &&
      poseVector(a.aerialGeometry,b.aerialGeometry) &&
      poseVector(a.aerialDensity,b.aerialDensity) && a.atmosphereLut == b.atmosphereLut;
}
bool poseBounds(const container::scene::ModelBounds& a,const container::scene::ModelBounds& b) noexcept {
  return a.valid==b.valid && poseVector(a.min,b.min) && poseVector(a.max,b.max) && poseVector(a.center,b.center) &&
      poseVector(a.size,b.size) && poseFloat(a.radius,b.radius);
}
bool poseLights(const container::scene::SceneManager& manager,const container::scene::AuthoredSceneLights& lights) noexcept {
  return poseArray(manager.authoredPointLights(),lights.points) && poseArray(manager.authoredDirectionalLights(),lights.directionals) &&
      poseArray(manager.authoredAreaLights(),lights.areas) && poseArray(manager.authoredPointLightBindings(),lights.pointBindings) &&
      poseArray(manager.authoredDirectionalLightBindings(),lights.directionalBindings) && poseArray(manager.authoredAreaLightBindings(),lights.areaBindings);
}
struct PoseLightBudget {
  const LightingGraphPoseGenerationLimits& limits;uint64_t cpu{},gpu{};
  void add(uint64_t count,uint64_t stride=1) {
    if(cpu>limits.maxPreparedCpuBytes || (stride && count>(limits.maxPreparedCpuBytes-cpu)/stride))
      throw std::invalid_argument("Graph pose lighting CPU preparation budget exceeded");cpu+=count*stride;
  }
  void device(uint64_t count,uint64_t stride) {
    if(gpu>limits.maxPreparedGpuBytes || (stride && count>(limits.maxPreparedGpuBytes-gpu)/stride))
      throw std::invalid_argument("Graph pose lighting GPU buffer budget exceeded");gpu+=count*stride;
  }
  template<class A> void array(const A& values) {add(values.size(),sizeof(typename A::value_type));}
  template<class A> void bindings(const A& values) {
    array(values);for(const auto& binding:values) {if(binding.extension.size()>4096) throw std::invalid_argument("Graph pose light binding metadata exceeds its limit");add(binding.extension.size()+1);}
  }
  void edits(const SceneLightEdits& e) {
    add(sizeof(e));array(e.generatedPointOverrides);array(e.importedPointOverrides);array(e.generatedAreaOverrides);array(e.importedAreaOverrides);
    array(e.manualPointLights);array(e.manualAreaLights);
    if(e.directionalOverride) {if(e.directionalOverride->label.size()>4096) throw std::invalid_argument("Graph pose directional edit label exceeds its limit");add(e.directionalOverride->label.size()+1);}
    if(e.identities) add(e.identities->bytes);
  }
  template<class P,class D,class A,class PB,class DB,class AB> void lights(const P& points,const D& directions,const A& areas,
      const PB& pointBindings,const DB& directionalBindings,const AB& areaBindings) {
    if(points.size()>1'000'000 || directions.size()>1'000'000 || areas.size()>1'000'000 ||
        points.size()!=pointBindings.size() || directions.size()!=directionalBindings.size() || areas.size()!=areaBindings.size())
      throw std::invalid_argument("Graph pose authored light/binding table exceeds its fixed capacity or shape");
    add(sizeof(container::scene::AuthoredSceneLights));array(points);array(directions);array(areas);
    bindings(pointBindings);bindings(directionalBindings);bindings(areaBindings);
  }
};

constexpr uint32_t kTimingQueryCount = 4u;
constexpr uint32_t kTimingQueryFrameSlots = 8u;
constexpr uint32_t kClusterCullStartQuery = 0u;
constexpr uint32_t kClusterCullEndQuery = 1u;
constexpr uint32_t kClusteredLightingStartQuery = 2u;
constexpr uint32_t kClusteredLightingEndQuery = 3u;
void syncLightingPointCount(LightingData &lightingData,
                            const std::vector<PointLightData> &allPointLights) {
  lightingData.pointLightCount = static_cast<uint32_t>(
      std::min<size_t>(allPointLights.size(), kMaxClusteredLights));
}

void syncLightingAreaCount(LightingData &lightingData,
                           const std::vector<AreaLightData> &allAreaLights) {
  lightingData.areaLightCount = static_cast<uint32_t>(
      std::min<size_t>(allAreaLights.size(), kMaxAreaLights));
}

float transformDistanceScale(const glm::mat4 &transform) {
  const float scale = std::max({
      glm::length(glm::vec3(transform[0])),
      glm::length(glm::vec3(transform[1])),
      glm::length(glm::vec3(transform[2])),
  });
  return std::isfinite(scale) && scale > 1.0e-6f ? scale : 1.0f;
}

glm::vec3 normalizeOr(const glm::vec3 &value, const glm::vec3 &fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

bool generatedLightingSettingsDiffer(const LightingSettings &lhs,
                                     const LightingSettings &rhs) {
  return lhs.preset != rhs.preset || lhs.density != rhs.density ||
         lhs.radiusScale != rhs.radiusScale ||
         lhs.intensityScale != rhs.intensityScale ||
         lhs.directionalIntensity != rhs.directionalIntensity ||
         lhs.environmentIntensity != rhs.environmentIntensity ||
         lhs.bounceIntensity != rhs.bounceIntensity;
}

EditableLightId invalidEditableLightId() { return {}; }

glm::vec3 rotateVector(const glm::vec3 &value, const glm::vec3 &axis,
                       float degrees) {
  const glm::vec3 rotationAxis = normalizeOr(axis, glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 rotation =
      glm::rotate(glm::mat4(1.0f), glm::radians(degrees), rotationAxis);
  return normalizeOr(glm::vec3(rotation * glm::vec4(value, 0.0f)),
                     normalizeOr(value, glm::vec3(0.0f, -1.0f, 0.0f)));
}

void writeTiledDescriptors(VkDevice device,
    VkDescriptorSet tileCullSet1_, VkDescriptorSet tileCullSet2_, VkDescriptorSet tiledDescriptorSet_,
    const container::gpu::AllocatedBuffer& lightSsbo_,
    const container::gpu::AllocatedBuffer& tileGridSsbo_,
    const container::gpu::AllocatedBuffer& lightIndexListSsbo_,
    const container::gpu::AllocatedBuffer& lightStatsBuffer_,
    const ClusteredLightCapacity& capacity) noexcept {
  if (!capacity.available() || tileCullSet1_ == VK_NULL_HANDLE ||
      tileCullSet2_ == VK_NULL_HANDLE ||
      tiledDescriptorSet_ == VK_NULL_HANDLE ||
      lightSsbo_.buffer == VK_NULL_HANDLE ||
      tileGridSsbo_.buffer == VK_NULL_HANDLE ||
      lightIndexListSsbo_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.buffer == VK_NULL_HANDLE) {
    return;
  }

  VkDescriptorBufferInfo lightInfo{
      lightSsbo_.buffer, 0, sizeof(PointLightData) * kMaxClusteredLights};
  VkDescriptorBufferInfo tileGridInfo{tileGridSsbo_.buffer, 0,
                                      capacity.tileGridBytes};
  VkDescriptorBufferInfo indexListInfo{lightIndexListSsbo_.buffer, 0,
                                       capacity.lightIndexListBytes};
  VkDescriptorBufferInfo statsInfo{lightStatsBuffer_.buffer, 0,
                                   sizeof(uint32_t) * 4};

  std::array<VkWriteDescriptorSet, 7> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = tileCullSet1_;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &lightInfo;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = tileCullSet2_;
  writes[1].dstBinding = 0;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &tileGridInfo;

  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = tileCullSet2_;
  writes[2].dstBinding = 1;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &indexListInfo;

  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = tileCullSet2_;
  writes[3].dstBinding = 2;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo = &statsInfo;

  writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[4].dstSet = tiledDescriptorSet_;
  writes[4].dstBinding = 0;
  writes[4].descriptorCount = 1;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[4].pBufferInfo = &lightInfo;

  writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[5].dstSet = tiledDescriptorSet_;
  writes[5].dstBinding = 1;
  writes[5].descriptorCount = 1;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[5].pBufferInfo = &tileGridInfo;

  writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[6].dstSet = tiledDescriptorSet_;
  writes[6].dstBinding = 2;
  writes[6].descriptorCount = 1;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[6].pBufferInfo = &indexListInfo;

  vkUpdateDescriptorSets(device,
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

VkDescriptorPool createFrameDescriptorPool(VkDevice device,
    std::span<const VkDescriptorPoolSize> sizes, uint32_t setCount) {
  VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = setCount;
  info.poolSizeCount = static_cast<uint32_t>(sizes.size());
  info.pPoolSizes = sizes.data();
  VkDescriptorPool pool{};
  if (createOwnedDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS)
    throw std::runtime_error("Could not allocate lighting frame descriptor pool");
  return pool;
}

void allocateFrameDescriptorSets(VkDevice device, VkDescriptorPool pool,
    std::span<const VkDescriptorSetLayout> layouts, std::span<VkDescriptorSet> sets) {
  VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  info.descriptorPool = pool;
  info.descriptorSetCount = static_cast<uint32_t>(layouts.size());
  info.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(device, &info, sets.data()) != VK_SUCCESS)
    throw std::runtime_error("Could not allocate lighting frame descriptor sets");
}

std::array<VkDescriptorPoolSize, 4> lightDescriptorPoolSizes(uint32_t imageCount) {
  return {{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount * 2},
           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount * 2},
           {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount * 3},
           {VK_DESCRIPTOR_TYPE_SAMPLER, imageCount}}};
}

void writeLightDescriptorSet(VkDevice device, VkDescriptorSet set,
    VkBuffer uniform, VkBuffer point, VkBuffer area,
    VkImageView matrixView, VkImageView amplitudeView, VkSampler sampler) noexcept {
  const std::array<VkDescriptorBufferInfo, 3> buffers{{
      {uniform, 0, sizeof(LightingData)},
      {point, 0, sizeof(PointLightData) * kMaxClusteredLights},
      {area, 0, sizeof(AreaLightData) * kMaxAreaLights}}};
  const std::array<VkDescriptorImageInfo, 3> images{{
      {VK_NULL_HANDLE, matrixView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, amplitudeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}}};
  std::array<VkWriteDescriptorSet, 6> writes{};
  for (uint32_t index = 0; index < writes.size(); ++index) {
    auto& write = writes[index];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.descriptorCount = 1;
    if (index < buffers.size()) {
      write.dstBinding = index;
      write.descriptorType = index == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &buffers[index];
    } else {
      write.dstBinding = index + 2;
      write.descriptorType = index == 5 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                         : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &images[index - buffers.size()];
    }
  }
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void writeRayShadowBindings(VkDevice device, VkDescriptorSet set,
    VkBuffer settings, VkDeviceSize size, VkImageView visibility) noexcept {
  const VkDescriptorBufferInfo buffer{settings, 0, size};
  const VkDescriptorImageInfo image{VK_NULL_HANDLE, visibility, VK_IMAGE_LAYOUT_GENERAL};
  std::array<VkWriteDescriptorSet, 2> writes{};
  for (auto& write : writes) {
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.descriptorCount = 1;
  }
  writes[0].dstBinding = 3;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[0].pBufferInfo = &buffer;
  writes[1].dstBinding = 4;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[1].pImageInfo = &image;
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

} // namespace

ClusteredLightCapacity planClusteredLightCapacity(
    VkExtent2D extent, VkDeviceSize storageRangeLimit,
    const std::array<uint32_t, 3> &dispatchLimits) noexcept {
  ClusteredLightCapacity result{};
  result.storageRangeLimit = storageRangeLimit;
  const auto tileCount = [](uint32_t pixels) {
    return static_cast<uint32_t>(std::max<uint64_t>(
        1, (uint64_t(pixels) + kTileSize - 1u) / kTileSize));
  };
  result.tileCountX = tileCount(extent.width);
  result.tileCountY = tileCount(extent.height);
  result.clusterCount = uint64_t(result.tileCountX) * result.tileCountY *
                        kClusterDepthSlices;
  if (result.clusterCount > UINT32_MAX) {
    result.status = ClusteredLightCapacityStatus::ShaderIndexLimitExceeded;
    return result;
  }

  result.tileGridBytes = result.clusterCount * sizeof(TileLightGrid);
  const uint64_t indexCount = result.clusterCount * kMaxLightsPerTile;
  result.lightIndexListBytes = indexCount * sizeof(uint32_t);
  // The shaders and storage-buffer addressing use 32-bit indices and offsets.
  constexpr uint64_t kShaderAddressSpaceBytes = uint64_t(UINT32_MAX) + 1u;
  if (result.tileGridBytes > kShaderAddressSpaceBytes ||
      result.lightIndexListBytes > kShaderAddressSpaceBytes) {
    result.status = ClusteredLightCapacityStatus::ShaderIndexLimitExceeded;
  } else if (result.tileGridBytes > storageRangeLimit ||
             result.lightIndexListBytes > storageRangeLimit) {
    result.status = ClusteredLightCapacityStatus::StorageRangeExceeded;
  } else if (result.tileCountX > dispatchLimits[0] ||
             result.tileCountY > dispatchLimits[1] ||
             kClusterDepthSlices > dispatchLimits[2]) {
    result.status = ClusteredLightCapacityStatus::DispatchLimitExceeded;
  } else {
    result.status = ClusteredLightCapacityStatus::Available;
  }
  return result;
}

std::string_view clusteredLightCapacityStatusName(
    ClusteredLightCapacityStatus status) noexcept {
  switch (status) {
  case ClusteredLightCapacityStatus::NotInitialized:
    return "notInitialized";
  case ClusteredLightCapacityStatus::Available:
    return "available";
  case ClusteredLightCapacityStatus::StorageRangeExceeded:
    return "storageRangeExceeded";
  case ClusteredLightCapacityStatus::DispatchLimitExceeded:
    return "dispatchLimitExceeded";
  case ClusteredLightCapacityStatus::ShaderIndexLimitExceeded:
    return "shaderIndexLimitExceeded";
  }
  return "unknown";
}

LightingManager::LightingManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager &allocationManager,
    container::gpu::PipelineManager &pipelineManager,
    container::scene::SceneManager *sceneManager,
    container::scene::SceneGraph &sceneGraph, container::ecs::World &world)
    : LightingManager(std::move(device),allocationManager,pipelineManager,sceneManager,std::as_const(sceneGraph),world,DetachedGraphTag{}) {}

LightingManager::LightingManager(std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager& allocationManager,container::gpu::PipelineManager& pipelineManager,
    container::scene::SceneManager* sceneManager,const container::scene::SceneGraph& sceneGraph,
    container::ecs::World& world,DetachedGraphTag)
    : device_(std::move(device)), allocationManager_(allocationManager),
      pipelineManager_(pipelineManager), sceneManager_(sceneManager),
      sceneGraph_(sceneGraph), world_(world),lightEditOwnerIdentity_(lightIdentity()) {}

struct LightingManager::PreparedGeneration::State {
  std::unique_ptr<LightingManager> payload;
  const LightingManager* owner{};
  VkDescriptorSetLayout lightLayout{}, cull0Layout{}, cull1Layout{}, cull2Layout{}, tiledLayout{};
  bool graphPose{},lightOnly{},consumed{};
  uint64_t cpuBytes{},gpuBytes{},epoch{},graphIdentity{},graphRevision{},cameraRevision{},sourceEpoch{},cameraAsset{};
  uint32_t root{container::scene::SceneGraph::kInvalidNode};size_t graphNodes{};
  glm::mat4 beforeRootLocal{1},beforeRootWorld{1},afterRootLocal{1},afterRootWorld{1};
  container::scene::AuthoredSceneLights beforeLights,afterLights;
  container::scene::ModelBounds beforeBounds,afterBounds;
  SceneLightEdits beforeEdits;
  LightingSettings beforeSettings;
  LightingData beforeData;
  EditableLightId beforeSelected;
  std::weak_ptr<const uint8_t> bindingOwner;
};

struct LightingManager::PreparedRayShadowBindings::State {
  std::shared_ptr<container::gpu::VulkanDevice> device;
  std::weak_ptr<const uint8_t> ownerIdentity;
  uint64_t epoch{};
  VkDescriptorSetLayout layout{};
  VkDescriptorPool sourcePool{}, pool{};
  std::vector<VkDescriptorSet> sets;
  std::vector<std::array<VkBuffer, 3>> lightBuffers;
  VkImageView matrixView{}, amplitudeView{};
  VkSampler sampler{};
  VkDeviceSize maxUniformRange{};
  std::vector<RayShadowBinding> rayBindings;
  ~State() {
    // Only raw-owned, detached pools enter this state, including abandoned
    // stages. The shared device makes destruction safe after owner teardown.
    if (pool != VK_NULL_HANDLE)
      destroyOwnedDescriptorPool(device->device(), pool, nullptr);
  }
};

LightingManager::PreparedRayShadowBindings::PreparedRayShadowBindings() noexcept = default;
LightingManager::PreparedRayShadowBindings::~PreparedRayShadowBindings() = default;
LightingManager::PreparedRayShadowBindings::PreparedRayShadowBindings(PreparedRayShadowBindings&&) noexcept = default;
LightingManager::PreparedRayShadowBindings& LightingManager::PreparedRayShadowBindings::operator=(PreparedRayShadowBindings&&) noexcept = default;
LightingManager::PreparedRayShadowBindings::operator bool() const noexcept {
  return state_ && state_->pool != VK_NULL_HANDLE;
}
VkDescriptorSet LightingManager::PreparedRayShadowBindings::lightDescriptorSet(uint32_t image) const noexcept {
  return *this && image < state_->sets.size() ? state_->sets[image] : VK_NULL_HANDLE;
}
void LightingManager::PreparedRayShadowBindings::updateRayShadowDescriptors(uint32_t image,
    VkBuffer settings, VkDeviceSize size, VkImageView visibility) {
  if (!*this || state_->ownerIdentity.expired() || image >= state_->sets.size() ||
      settings == VK_NULL_HANDLE || size == 0 || size > state_->maxUniformRange || visibility == VK_NULL_HANDLE)
    throw std::invalid_argument("Invalid staged ray-shadow light descriptor binding");
  writeRayShadowBindings(state_->device->device(), state_->sets[image], settings, size, visibility);
  state_->rayBindings[image] = {settings, size, visibility};
}

LightingManager::PreparedRayShadowBindings LightingManager::prepareRayShadowBindings(uint32_t imageCount) const {
  if (imageCount == 0 || imageCount > 64 || imageCount != lightingBuffers_.size() ||
      imageCount != lightDescriptorSets_.size() || imageCount != rayShadowBindings_.size() ||
      !device_ || device_->device() == VK_NULL_HANDLE || lightDescriptorSetLayout_ == VK_NULL_HANDLE ||
      lightDescriptorPool_ == VK_NULL_HANDLE || pointLightBuffers_.size() != imageCount ||
      areaLightBuffers_.size() != imageCount || !ltcLuts_ ||
      descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max())
    throw std::invalid_argument("Ray-shadow staging requires matching initialized lighting frames");
  for (uint32_t image = 0; image < imageCount; ++image)
    if (!lightingBuffers_[image].buffer || !pointLightBuffers_[image].buffer ||
        !areaLightBuffers_[image].buffer || !lightDescriptorSets_[image])
      throw std::invalid_argument("Ray-shadow staging requires complete lighting buffers and sets");
  if (ltcLuts_->matrixView() == VK_NULL_HANDLE || ltcLuts_->amplitudeView() == VK_NULL_HANDLE ||
      ltcLuts_->sampler() == VK_NULL_HANDLE)
    throw std::invalid_argument("Ray-shadow staging requires resident LTC bindings");

  PreparedRayShadowBindings result;
  result.state_ = std::make_unique<PreparedRayShadowBindings::State>();
  auto& state = *result.state_;
  state.device = device_;
  state.ownerIdentity = descriptorBindingIdentity_;
  state.epoch = descriptorBindingEpoch_;
  state.layout = lightDescriptorSetLayout_;
  state.sourcePool = lightDescriptorPool_;
  state.matrixView = ltcLuts_->matrixView();
  state.amplitudeView = ltcLuts_->amplitudeView();
  state.sampler = ltcLuts_->sampler();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device_->physicalDevice(), &properties);
  state.maxUniformRange = properties.limits.maxUniformBufferRange;
  state.sets.resize(imageCount, VK_NULL_HANDLE);
  state.lightBuffers.reserve(imageCount);
  state.rayBindings = rayShadowBindings_;
  state.pool = createFrameDescriptorPool(device_->device(), lightDescriptorPoolSizes(imageCount), imageCount);
  const std::vector<VkDescriptorSetLayout> layouts(imageCount, state.layout);
  allocateFrameDescriptorSets(device_->device(), state.pool, layouts, state.sets);

  for (uint32_t image = 0; image < imageCount; ++image) {
    state.lightBuffers.push_back({lightingBuffers_[image].buffer, pointLightBuffers_[image].buffer,
                                  areaLightBuffers_[image].buffer});
    const auto& buffers = state.lightBuffers.back();
    writeLightDescriptorSet(device_->device(), state.sets[image], buffers[0], buffers[1], buffers[2],
                            state.matrixView, state.amplitudeView, state.sampler);
    const auto& ray = state.rayBindings[image];
    if (ray.settings != VK_NULL_HANDLE && ray.settingsSize != 0 && ray.settingsSize <= state.maxUniformRange &&
        ray.visibility != VK_NULL_HANDLE)
      writeRayShadowBindings(device_->device(), state.sets[image], ray.settings, ray.settingsSize, ray.visibility);
  }
  return result;
}

bool LightingManager::canExchangeRayShadowBindings(const PreparedRayShadowBindings& value) const noexcept {
  if (!value || value.state_->ownerIdentity.lock() != descriptorBindingIdentity_ ||
      value.state_->epoch != descriptorBindingEpoch_ ||
      descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max()) return false;
  const auto& state = *value.state_;
  if (state.device != device_ || state.layout != lightDescriptorSetLayout_ || state.sourcePool != lightDescriptorPool_ ||
      !ltcLuts_ || state.lightBuffers.size() != state.sets.size() ||
      state.matrixView != ltcLuts_->matrixView() || state.amplitudeView != ltcLuts_->amplitudeView() ||
      state.sampler != ltcLuts_->sampler() || state.sets.size() != lightingBuffers_.size() ||
      state.sets.size() != lightDescriptorSets_.size() || state.sets.size() != rayShadowBindings_.size() ||
      state.sets.size() != pointLightBuffers_.size() || state.sets.size() != areaLightBuffers_.size()) return false;
  for (size_t image = 0; image < state.sets.size(); ++image) {
    const auto& ray = state.rayBindings[image];
    const auto& buffers = state.lightBuffers[image];
    if (buffers[0] != lightingBuffers_[image].buffer || buffers[1] != pointLightBuffers_[image].buffer ||
        buffers[2] != areaLightBuffers_[image].buffer || state.sets[image] == VK_NULL_HANDLE ||
        ray.settings == VK_NULL_HANDLE || ray.settingsSize == 0 || ray.settingsSize > state.maxUniformRange ||
        ray.visibility == VK_NULL_HANDLE) return false;
  }
  return true;
}

bool LightingManager::exchangeRayShadowBindings(PreparedRayShadowBindings& value) noexcept {
  if (!canExchangeRayShadowBindings(value)) return false;
  auto& state = *value.state_;
  // GPU readers must already be retired. No allocation or descriptor write is
  // permitted in this final publication phase. Retire the old pool before
  // making the replacement sets live.
  destroyLightDescriptorPool();
  lightDescriptorPool_ = std::exchange(state.pool, VK_NULL_HANDLE);
  lightDescriptorSets_.swap(state.sets);
  rayShadowBindings_.swap(state.rayBindings);
  return true;
}

LightingManager::PreparedGeneration::PreparedGeneration() noexcept = default;
LightingManager::PreparedGeneration::~PreparedGeneration() = default;
LightingManager::PreparedGeneration::PreparedGeneration(PreparedGeneration&&) noexcept = default;
LightingManager::PreparedGeneration& LightingManager::PreparedGeneration::operator=(PreparedGeneration&&) noexcept = default;
LightingManager::PreparedGeneration::operator bool() const noexcept {
  return state_ && state_->payload && (!(state_->graphPose||state_->lightOnly) || !state_->consumed);
}
const LightingData& LightingManager::PreparedGeneration::lightingData() const {
  if (!*this) throw std::logic_error("Empty lighting generation");
  return state_->payload->lightingData_;
}
const std::vector<PointLightData>& LightingManager::PreparedGeneration::pointLightsSsbo() const {
  if (!*this) throw std::logic_error("Empty lighting generation");
  return state_->payload->pointLightsSsbo_;
}
const std::vector<AreaLightData>& LightingManager::PreparedGeneration::areaLightsSsbo() const {
  if (!*this) throw std::logic_error("Empty lighting generation");
  return state_->payload->areaLightsSsbo_;
}
std::span<const container::gpu::AllocatedBuffer>
LightingManager::PreparedGeneration::lightingBuffers() const noexcept {
  return *this ? std::span<const container::gpu::AllocatedBuffer>(state_->payload->lightingBuffers_)
               : std::span<const container::gpu::AllocatedBuffer>{};
}
const container::gpu::AllocatedBuffer& LightingManager::PreparedGeneration::pointLightBuffer(uint32_t image) const {
  if (!*this) throw std::logic_error("Empty lighting generation");
  return state_->payload->pointLightBuffer(image);
}
const container::gpu::AllocatedBuffer& LightingManager::PreparedGeneration::areaLightBuffer(uint32_t image) const {
  if (!*this) throw std::logic_error("Empty lighting generation");
  return state_->payload->areaLightBuffer(image);
}
std::span<const container::gpu::AllocatedBuffer>
LightingManager::PreparedGeneration::pointLightBuffers() const noexcept {
  return *this ? state_->payload->pointLightBuffers() : std::span<const container::gpu::AllocatedBuffer>{};
}
std::span<const container::gpu::AllocatedBuffer>
LightingManager::PreparedGeneration::areaLightBuffers() const noexcept {
  return *this ? state_->payload->areaLightBuffers() : std::span<const container::gpu::AllocatedBuffer>{};
}
VkDescriptorSet LightingManager::PreparedGeneration::lightDescriptorSet(uint32_t image) const noexcept {
  return *this ? state_->payload->lightDescriptorSet(image) : VK_NULL_HANDLE;
}
VkDescriptorSet LightingManager::PreparedGeneration::tiledDescriptorSet(uint32_t image) const noexcept {
  return *this ? state_->payload->tiledDescriptorSet(image) : VK_NULL_HANDLE;
}
uint64_t LightingManager::PreparedGeneration::ownedBytes() const noexcept {return state_?state_->cpuBytes:0;}
uint64_t LightingManager::PreparedGeneration::ownedGpuBytes() const noexcept {return state_?state_->gpuBytes:0;}
void LightingManager::PreparedGeneration::updateRayShadowDescriptors(uint32_t image,
    VkBuffer settings, VkDeviceSize size, VkImageView visibility) {
  if (!*this) throw std::logic_error("Empty lighting generation");
  state_->payload->updateRayShadowDescriptors(image, settings, size, visibility);
}

LightingManager::PreparedGeneration LightingManager::prepareGeneration(
    container::scene::SceneManager& sceneManager, container::scene::SceneGraph& graph,
    container::ecs::World& world, uint32_t rootNode, const SceneLightEdits& edits,
    uint32_t imageCount) const {
  return prepareGenerationImpl(sceneManager,graph,world,rootNode,edits,imageCount,nullptr,nullptr,nullptr);
}
LightingManager::PreparedGeneration LightingManager::prepareGraphPoseGeneration(
    container::scene::SceneManager& sceneManager,const container::scene::SceneGraph& graph,
    container::ecs::World& world,uint32_t rootNode,const container::scene::AuthoredSceneLights& lights,
    const container::scene::ModelBounds& bounds,const SceneLightEdits& edits,uint32_t images,
    const LightingGraphPoseGenerationLimits& limits) const {
  return prepareGenerationImpl(sceneManager,graph,world,rootNode,edits,images,&lights,&bounds,&limits);
}
LightingManager::PreparedGeneration LightingManager::prepareGenerationImpl(
    container::scene::SceneManager& sceneManager,const container::scene::SceneGraph& graph,
    container::ecs::World& world,uint32_t rootNode,const SceneLightEdits& edits,uint32_t imageCount,
    const container::scene::AuthoredSceneLights* candidateLights,const container::scene::ModelBounds* candidateBounds,
    const LightingGraphPoseGenerationLimits* poseLimits,bool lightOnly) const {
  const auto reject = [](const char* message) {
    throw std::invalid_argument(std::string("Lighting generation: ") + message);
  };
  const bool graphPose=poseLimits!=nullptr&&!lightOnly;
  if (lightOnly?(&graph!=&sceneGraph_ || &world!=&world_ || &sceneManager!=sceneManager_):
      (&graph == &sceneGraph_ || &world == &world_ || (graphPose?&sceneManager!=sceneManager_:&sceneManager==sceneManager_)))
    reject(graphPose?"graph pose requires this live source plus detached graph and world":"preparation requires detached scene, graph and world inputs");
  if (rootNode != container::scene::SceneGraph::kInvalidNode && !graph.getNode(rootNode))
    reject("scene anchor root is invalid");
  if (!imageCount || imageCount > 64) reject("image count must be between 1 and 64");
  const auto within = [](size_t source, size_t manual, size_t bound) {
    return source <= bound && manual <= bound - source;
  };
  const auto sourcePoints=candidateLights?candidateLights->points.size():sceneManager.authoredPointLights().size();
  const auto sourceAreas=candidateLights?candidateLights->areas.size():sceneManager.authoredAreaLights().size();
  const auto points=lightOnly?std::min<size_t>(sourcePoints,kMaxClusteredLights):sourcePoints;
  const auto areas=lightOnly?std::min<size_t>(sourceAreas,kMaxAreaLights):sourceAreas;
  if (!within(points, edits.manualPointLights.size(), kMaxClusteredLights) ||
      !within(areas, edits.manualAreaLights.size(), kMaxAreaLights) ||
      edits.generatedPointOverrides.size() > kMaxClusteredLights ||
      edits.importedPointOverrides.size() > kMaxClusteredLights ||
      edits.generatedAreaOverrides.size() > kMaxAreaLights ||
      edits.importedAreaOverrides.size() > kMaxAreaLights)
    reject("authored, manual or override light counts exceed fixed GPU capacities");
  if (!device_ || device_->device() == VK_NULL_HANDLE || !allocationManager_.memoryManager() ||
      lightDescriptorSetLayout_ == VK_NULL_HANDLE || !ltcLuts_)
    reject("live light descriptor resources have not been initialized");
  const bool tiled = tileCullPipeline_ != VK_NULL_HANDLE;
  if (tiled && (tileCullSet0Layout_ == VK_NULL_HANDLE || tileCullSet1Layout_ == VK_NULL_HANDLE ||
      tileCullSet2Layout_ == VK_NULL_HANDLE || tiledDescriptorSetLayout_ == VK_NULL_HANDLE ||
      (!tiledCapacityLimited() && !tiledScratchResourcesReady())))
    reject("live tiled resources are incomplete");

  uint64_t cpuBytes{},gpuBytes{};
  if(poseLimits) {
    if(!candidateLights || !candidateBounds || !poseLimits->maxPreparedCpuBytes || poseLimits->maxPreparedCpuBytes==UINT64_MAX ||
        !poseLimits->maxPreparedGpuBytes || poseLimits->maxPreparedGpuBytes==UINT64_MAX || !poseLimits->maxGraphNodes ||
        poseLimits->maxGraphNodes>=UINT32_MAX || graph.nodeCount()>poseLimits->maxGraphNodes || sceneGraph_.nodeCount()!=graph.nodeCount() ||
        sceneGraph_.revision()==UINT64_MAX || sceneManager.cameraRevision()==UINT64_MAX || descriptorBindingEpoch_==UINT64_MAX ||
        rootNode!=rootNode_ || imageCount!=lightingBuffers_.size() || imageCount!=lightDescriptorSets_.size())
      reject("graph pose source, root, frame or finite capacities are invalid");
    const auto* beforeRoot=sceneGraph_.getNode(rootNode);const auto* afterRoot=graph.getNode(rootNode);
    if(rootNode!=container::scene::SceneGraph::kInvalidNode && (!beforeRoot || beforeRoot->retired || !afterRoot || afterRoot->retired))
      reject("graph pose root is missing or retired");
    const auto finiteBounds=[](const container::scene::ModelBounds& b) {
      for(const auto* v:{&b.min,&b.max,&b.center,&b.size}) for(int i=0;i<3;++i) if(!std::isfinite((*v)[i])) return false;
      return std::isfinite(b.radius) && b.radius>=0;
    };
    if(!finiteBounds(sceneManager.modelBounds()) || !finiteBounds(*candidateBounds)) reject("graph pose bounds are nonfinite");
    if(edits.generationSettings && !poseSettings(*edits.generationSettings,lightingSettings_))
      reject("graph pose cannot change current light generation settings");
    PoseLightBudget budget{*poseLimits};budget.add(sizeof(PreparedGeneration::State)+sizeof(LightingManager)+4096);
    budget.lights(sceneManager.authoredPointLights(),sceneManager.authoredDirectionalLights(),sceneManager.authoredAreaLights(),
        sceneManager.authoredPointLightBindings(),sceneManager.authoredDirectionalLightBindings(),sceneManager.authoredAreaLightBindings());
    budget.lights(candidateLights->points,candidateLights->directionals,candidateLights->areas,
        candidateLights->pointBindings,candidateLights->directionalBindings,candidateLights->areaBindings);
    if(directionalOverride_ && directionalOverride_->label.size()>4096) reject("live directional edit label exceeds its limit");
    budget.add(sceneLightEditsStorageBytes());budget.edits(edits);budget.edits(edits);
    // Fixed renderer capacities bound vector growth, editable labels and the
    // detached World's replacement point-light storage, including coexistence.
    budget.add(kMaxClusteredLights,2*sizeof(PointLightData)+2*sizeof(EditableLightEntity)+768);
    budget.add(kMaxAreaLights,2*sizeof(AreaLightData)+2*sizeof(EditableLightEntity)+256);
    budget.add(imageCount, 3 * sizeof(container::gpu::AllocatedBuffer) +
        5 * sizeof(VkDescriptorSet) + sizeof(RayShadowBinding) + 64);
    budget.device(imageCount, sizeof(LightingData) +
        kMaxClusteredLights * sizeof(PointLightData) + kMaxAreaLights * sizeof(AreaLightData));
    cpuBytes=budget.cpu;gpuBytes=budget.gpu;
  }

  PreparedGeneration result; result.state_ = std::make_unique<PreparedGeneration::State>();
  auto& state = *result.state_;
  state.owner = this; state.lightLayout = lightDescriptorSetLayout_;
  state.cull0Layout = tileCullSet0Layout_; state.cull1Layout = tileCullSet1Layout_;
  state.cull2Layout = tileCullSet2Layout_; state.tiledLayout = tiledDescriptorSetLayout_;
  state.graphPose=graphPose;state.lightOnly=lightOnly;state.cpuBytes=cpuBytes;state.gpuBytes=gpuBytes;
  if(graphPose) {
    state.bindingOwner=descriptorBindingIdentity_;state.epoch=descriptorBindingEpoch_;
    state.graphIdentity=sceneGraph_.identity();state.graphRevision=sceneGraph_.revision();state.graphNodes=sceneGraph_.nodeCount();
    state.cameraRevision=sceneManager.cameraRevision();state.sourceEpoch=sceneManager.generationEpoch();state.cameraAsset=sceneManager.cameraAssetIdentity();
    state.root=rootNode;state.beforeLights=sceneManager.publishedAuthoredSceneLights();state.afterLights=*candidateLights;
    state.beforeBounds=sceneManager.modelBounds();state.afterBounds=*candidateBounds;
    state.beforeEdits=snapshotSceneLightEdits();state.beforeSettings=lightingSettings_;state.beforeData=lightingData_;
    state.beforeSelected=selectedEditableLight_;
    if(const auto* root=sceneGraph_.getNode(rootNode)) {state.beforeRootLocal=root->localTransform;state.beforeRootWorld=root->worldTransform;}
    if(const auto* root=graph.getNode(rootNode)) {state.afterRootLocal=root->localTransform;state.afterRootWorld=root->worldTransform;}
  }
  // A private const-graph constructor keeps the old public constructor/API
  // intact and avoids a throwaway graph copy or a const_cast of caller data.
  state.payload.reset(new LightingManager(device_,allocationManager_,pipelineManager_,&sceneManager,graph,world,DetachedGraphTag{}));
  auto& next = *state.payload;
  next.suppressWorldPublication_=lightOnly;
  next.lightingSettings_ = lightingSettings_;
  next.restoreSceneLightEdits(edits);
  next.selectedEditableLight_ = selectedEditableLight_;
  next.rootNode_ = rootNode;
  // CPU evaluation touches only the supplied detached world. It preserves its
  // renderables/camera and replaces its point-light entities before publication.
  next.authoredLightsOverride_=candidateLights;next.boundsOverride_=candidateBounds;
  try {
    next.updateLightingDataForActiveCamera();
    if(graphPose) {
      const auto anchor=next.computeSceneLightingAnchor();
      if(!std::isfinite(anchor.center.x) || !std::isfinite(anchor.center.y) || !std::isfinite(anchor.center.z) ||
          !std::isfinite(anchor.localRadius) || !std::isfinite(anchor.worldRadius)) reject("candidate graph lighting anchor is nonfinite");
    }
  } catch(...) {next.authoredLightsOverride_=nullptr;next.boundsOverride_=nullptr;throw;}
  next.authoredLightsOverride_=nullptr;next.boundsOverride_=nullptr;
  next.lightingData_.areaLightingMode = lightingSettings_.areaLightingMode && ltcReady() ? 1u : 0u;
  next.lightingData_.shadowEnabled = lightingData_.shadowEnabled;
  next.lightingData_.gtaoEnabled = lightingData_.gtaoEnabled;
  next.lightingData_.tileCullEnabled = lightingData_.tileCullEnabled;
  next.lightingData_.prefilteredMipCount = lightingData_.prefilteredMipCount;
  next.lightingData_.localShadowEnabled = lightingData_.localShadowEnabled;
  next.lightingData_.localContactVisibility = lightingData_.localContactVisibility;
  const auto finite4 = [](const glm::vec4& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z) && std::isfinite(value.w);
  };
  if (!finite4(next.lightingData_.directionalDirection) ||
      !finite4(next.lightingData_.directionalColorIntensity) ||
      !std::isfinite(next.lightingData_.environmentIntensity) ||
      !std::isfinite(next.lightingData_.bounceIntensity)) reject("evaluated lighting is nonfinite");
  for (const auto& light : next.pointLightsSsbo_)
    if (!finite4(light.positionRadius) || !finite4(light.colorIntensity) ||
        !finite4(light.directionInnerCos) || !finite4(light.coneOuterCosType) || !finite4(light.iesTangent) ||
        (light.iesTangent.w != 0.0f) != (light.iesProfileAddress != 0))
      reject("evaluated point lighting is nonfinite");
  for (const auto& light : next.areaLightsSsbo_)
    if (!finite4(light.positionRange) || !finite4(light.colorIntensity) ||
        !finite4(light.directionType) || !finite4(light.tangentHalfSize) || !finite4(light.bitangentHalfSize))
      reject("evaluated area lighting is nonfinite");

  createLightingFrameResources(next, imageCount, true);
  next.uploadLightingData();
  if (tiled) {
    createTiledFrameDescriptors(next, imageCount);
    writeTiledResourceDescriptors(next);
  }
  return result;
}

bool LightingManager::tiledCapacityLimited() const noexcept {
  return clusteredLightCapacity_.status != ClusteredLightCapacityStatus::NotInitialized &&
         !clusteredLightCapacity_.available();
}

bool LightingManager::tiledScratchResourcesReady() const noexcept {
  const auto sufficient=[](const container::gpu::AllocatedBuffer& buffer,VkDeviceSize bytes) noexcept {
    return bytes && buffer.buffer!=VK_NULL_HANDLE && buffer.allocation && buffer.allocation_info.size>=bytes;
  };
  return clusteredLightCapacity_.available() &&
      sufficient(tileGridSsbo_, clusteredLightCapacity_.tileGridBytes) &&
      sufficient(lightIndexListSsbo_, clusteredLightCapacity_.lightIndexListBytes) &&
      // GPU stats contain four counters; submitted count and timings in the
      // public CPU LightCullingStats are derived separately during collection.
      sufficient(lightStatsBuffer_,4*sizeof(uint32_t));
}

bool LightingManager::generationResourcesReady(const PreparedGeneration& value) const noexcept {
  if (!value || value.state_->owner != this ||
      descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max()) return false;
  const auto& state = *value.state_; const auto& next = *state.payload;
  const auto imageCount = lightingBuffers_.size();
  return imageCount != 0 && next.lightDescriptorPool_ != VK_NULL_HANDLE &&
      next.lightingBuffers_.size() == imageCount &&
      next.pointLightBuffers_.size() == imageCount && next.areaLightBuffers_.size() == imageCount &&
      state.lightLayout == lightDescriptorSetLayout_ && state.cull0Layout == tileCullSet0Layout_ &&
      state.cull1Layout == tileCullSet1Layout_ && state.cull2Layout == tileCullSet2Layout_ &&
      state.tiledLayout == tiledDescriptorSetLayout_ &&
      next.lightDescriptorSets_.size() == imageCount &&
      (next.tileCullDescriptorPool_ != VK_NULL_HANDLE) == (tileCullPipeline_ != VK_NULL_HANDLE) &&
      (tileCullPipeline_ == VK_NULL_HANDLE || (next.tiledSets_.size() == imageCount &&
          (tiledCapacityLimited() || tiledScratchResourcesReady())));
}

bool LightingManager::graphPoseGenerationMatches(const PreparedGeneration& value,bool prospective) const noexcept {
  if(!value || !value.state_->graphPose || !sceneManager_) return false;
  const auto& s=*value.state_;
  if(s.consumed || s.bindingOwner.lock()!=descriptorBindingIdentity_ || s.epoch!=descriptorBindingEpoch_ ||
      s.beforeEdits.identities!=lightEditIdentities_ ||
      authoredLightsOverride_ || boundsOverride_ || s.payload->authoredLightsOverride_ || s.payload->boundsOverride_ ||
      rootNode_!=s.root || sceneGraph_.identity()!=s.graphIdentity || sceneGraph_.nodeCount()!=s.graphNodes ||
      sceneGraph_.revision()!=(prospective?s.graphRevision+1:s.graphRevision) ||
      sceneManager_->cameraRevision()!=(prospective?s.cameraRevision+1:s.cameraRevision) ||
      sceneManager_->generationEpoch()!=s.sourceEpoch || sceneManager_->cameraAssetIdentity()!=s.cameraAsset ||
      !poseSettings(lightingSettings_,s.beforeSettings) || !poseData(lightingData_,s.beforeData) ||
      selectedEditableLight_.type!=s.beforeSelected.type || selectedEditableLight_.source!=s.beforeSelected.source || selectedEditableLight_.index!=s.beforeSelected.index ||
      !poseEditable(directionalOverride_,s.beforeEdits.directionalOverride) ||
      !poseArray(generatedPointOverrides_,s.beforeEdits.generatedPointOverrides) ||
      !poseArray(importedPointOverrides_,s.beforeEdits.importedPointOverrides) ||
      !poseArray(generatedAreaOverrides_,s.beforeEdits.generatedAreaOverrides) ||
      !poseArray(importedAreaOverrides_,s.beforeEdits.importedAreaOverrides) ||
      !poseArray(manualPointLights_,s.beforeEdits.manualPointLights) || !poseArray(manualAreaLights_,s.beforeEdits.manualAreaLights) ||
      !poseLights(*sceneManager_,prospective?s.afterLights:s.beforeLights) ||
      !poseBounds(sceneManager_->modelBounds(),prospective?s.afterBounds:s.beforeBounds)) return false;
  const auto* root=sceneGraph_.getNode(s.root);
  return s.root==container::scene::SceneGraph::kInvalidNode || (root && !root->retired &&
      poseMatrix(root->localTransform,prospective?s.afterRootLocal:s.beforeRootLocal) &&
      poseMatrix(root->worldTransform,prospective?s.afterRootWorld:s.beforeRootWorld));
}

bool LightingManager::canExchangeGeneration(const PreparedGeneration& value) const noexcept {
  return generationResourcesReady(value) && !value.state_->lightOnly && (!value.state_->graphPose || graphPoseGenerationMatches(value,false));
}

bool LightingManager::exchangeGeneration(PreparedGeneration& value) noexcept {
  if(value.state_&&value.state_->lightOnly) return false;
  if (!generationResourcesReady(value) || (value.state_->graphPose?!graphPoseGenerationMatches(value,true):!canExchangeGeneration(value))) return false;
  exchangeGenerationPayload(value);return true;
}
void LightingManager::exchangeGenerationPayload(PreparedGeneration& value) noexcept {
  auto& next = *value.state_->payload;
  // Retained history may span a viewport resize. Rebind only detached sets to
  // today's shared scratch buffers before swapping them live. This helper is
  // raw Vulkan void calls over fixed stack arrays: no allocation or exception.
  // A pose stage forbids a scratch/descriptor epoch change, so its detached
  // sets already reference these exact buffers. Keep its final boundary CPU
  // only. Legacy reversible generations still rebind after viewport resize.
  if (tileCullPipeline_ != VK_NULL_HANDLE && !value.state_->graphPose && !value.state_->lightOnly)
    writeTiledResourceDescriptors(next);
  using std::swap;
  static_assert(std::is_nothrow_swappable_v<LightingData>);
  static_assert(std::is_nothrow_swappable_v<LightingSettings>);
  static_assert(std::is_nothrow_swappable_v<decltype(directionalOverride_)>);
  swap(lightingData_, next.lightingData_); swap(lightingSettings_, next.lightingSettings_);
  swap(rootNode_, next.rootNode_); swap(lastStats_, next.lastStats_);
  if (tiledCapacityLimited())
    lastStats_ = {};
  pointLightsSsbo_.swap(next.pointLightsSsbo_); areaLightsSsbo_.swap(next.areaLightsSsbo_);
  editableLights_.swap(next.editableLights_); swap(selectedEditableLight_, next.selectedEditableLight_);
  directionalOverride_.swap(next.directionalOverride_);
  generatedPointOverrides_.swap(next.generatedPointOverrides_); importedPointOverrides_.swap(next.importedPointOverrides_);
  generatedAreaOverrides_.swap(next.generatedAreaOverrides_); importedAreaOverrides_.swap(next.importedAreaOverrides_);
  manualPointLights_.swap(next.manualPointLights_); manualAreaLights_.swap(next.manualAreaLights_);
  lightEditIdentities_.swap(next.lightEditIdentities_);
  swap(generatedLightRevision_,next.generatedLightRevision_);
  swapFrameResources(next);
  lastDispatchClusterCount_ = 0; timingQueriesWritten_ = false;
  invalidateRayShadowBindingPreparations();
  if(value.state_->graphPose||value.state_->lightOnly) value.state_->consumed=true;
  if(!value.state_->lightOnly&&lightEditRevision_!=UINT64_MAX) ++lightEditRevision_;
}

void LightingManager::swapFrameResources(LightingManager& target) noexcept {
  lightingBuffers_.swap(target.lightingBuffers_);
  pointLightBuffers_.swap(target.pointLightBuffers_);
  areaLightBuffers_.swap(target.areaLightBuffers_);
  lightDescriptorSets_.swap(target.lightDescriptorSets_);
  rayShadowBindings_.swap(target.rayShadowBindings_);
  tiledSets_.swap(target.tiledSets_);
  std::swap(lightDescriptorPool_, target.lightDescriptorPool_);
  std::swap(tileCullDescriptorPool_, target.tileCullDescriptorPool_);
}

LightingManager::~LightingManager() {
  descriptorBindingIdentity_.reset();
  destroyLightDescriptorPool();
  destroyTileCullDescriptorPool();
  if (lightGizmoIconSampler_ != VK_NULL_HANDLE && device_ &&
      device_->device() != VK_NULL_HANDLE) {
    destroyOwnedSampler(device_->device(), lightGizmoIconSampler_, nullptr);
    lightGizmoIconSampler_ = VK_NULL_HANDLE;
  }
  if (timestampQueryPool_ != VK_NULL_HANDLE && device_ &&
      device_->device() != VK_NULL_HANDLE) {
    destroyOwnedQueryPool(device_->device(), timestampQueryPool_, nullptr);
    timestampQueryPool_ = VK_NULL_HANDLE;
  }
  allocationManager_.destroyBuffer(lightStatsBuffer_);
  allocationManager_.destroyBuffer(lightIndexListSsbo_);
  allocationManager_.destroyBuffer(tileGridSsbo_);
  for (auto* buffers : {&lightingBuffers_, &pointLightBuffers_, &areaLightBuffers_}) {
    for (auto& buffer : *buffers) allocationManager_.destroyBuffer(buffer);
  }
}

void LightingManager::destroyLightDescriptorPool() noexcept {
  if (lightDescriptorPool_ == VK_NULL_HANDLE) return;
  invalidateRayShadowBindingPreparations();
  destroyOwnedDescriptorPool(device_->device(), std::exchange(lightDescriptorPool_, VK_NULL_HANDLE), nullptr);
}

void LightingManager::invalidateRayShadowBindingPreparations() noexcept {
  // Saturate rather than wrap: once exhausted, no token may be admitted.
  if (descriptorBindingEpoch_ != std::numeric_limits<uint64_t>::max())
    ++descriptorBindingEpoch_;
}

void LightingManager::destroyTileCullDescriptorPool() noexcept {
  if (tileCullDescriptorPool_ != VK_NULL_HANDLE)
    destroyOwnedDescriptorPool(device_->device(), std::exchange(tileCullDescriptorPool_, VK_NULL_HANDLE), nullptr);
}

void LightingManager::createDescriptorResources(uint32_t descriptorSetCount) {
  if (descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Lighting descriptor binding epoch exhausted");
  const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
  if (setCount > std::numeric_limits<uint32_t>::max() / 7u)
    throw std::length_error("Lighting descriptor count is too large");

  if (!ltcLuts_) {
    auto luts = std::make_unique<LtcLutResources>(device_, *allocationManager_.memoryManager());
    luts->load(container::util::executableDirectory());
    ltcLuts_ = std::move(luts);
  }

  if (lightDescriptorSetLayout_ == VK_NULL_HANDLE) {
    const std::array<VkDescriptorSetLayoutBinding, 8> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {7, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
    }};
    lightDescriptorSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, std::vector<VkDescriptorBindingFlags>(bindings.size(), 0));
  }

  LightingManager next(device_, allocationManager_, pipelineManager_, sceneManager_, sceneGraph_,
                       world_, DetachedGraphTag{});
  createLightingFrameResources(next, setCount, false);
  next.uploadLightingData(*this);
  if (tileCullSet0Layout_ != VK_NULL_HANDLE) {
    createTiledFrameDescriptors(next, setCount);
    writeTiledResourceDescriptors(next);
  }
  createLightGizmoIconDescriptorResources();
  invalidateRayShadowBindingPreparations();
  swapFrameResources(next);
}

void LightingManager::createLightingFrameResources(LightingManager& target, uint32_t imageCount,
                                                    bool inheritRayBindings) const {
  constexpr auto host = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
  target.lightingBuffers_.resize(imageCount);
  target.pointLightBuffers_.resize(imageCount);
  target.areaLightBuffers_.resize(imageCount);
  target.lightDescriptorSets_.resize(imageCount);
  target.rayShadowBindings_.resize(imageCount);
  target.lightDescriptorSetLayout_ = lightDescriptorSetLayout_;

  const auto device = device_->device();
  target.lightDescriptorPool_ = createFrameDescriptorPool(device, lightDescriptorPoolSizes(imageCount), imageCount);
  const std::vector<VkDescriptorSetLayout> layouts(imageCount, lightDescriptorSetLayout_);
  allocateFrameDescriptorSets(device, target.lightDescriptorPool_, layouts, target.lightDescriptorSets_);
  for (uint32_t image = 0; image < imageCount; ++image) {
    auto& uniform = target.lightingBuffers_[image];
    auto& point = target.pointLightBuffers_[image];
    auto& area = target.areaLightBuffers_[image];
    uniform = allocationManager_.createBuffer(sizeof(LightingData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                               VMA_MEMORY_USAGE_AUTO, host);
    point = allocationManager_.createBuffer(sizeof(PointLightData) * kMaxClusteredLights,
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, host);
    area = allocationManager_.createBuffer(sizeof(AreaLightData) * kMaxAreaLights,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO, host);
    writeLightDescriptorSet(device, target.lightDescriptorSets_[image], uniform.buffer, point.buffer, area.buffer,
                            ltcLuts_->matrixView(), ltcLuts_->amplitudeView(), ltcLuts_->sampler());
    if (inheritRayBindings && image < rayShadowBindings_.size()) {
      const auto& ray = rayShadowBindings_[image];
      if (ray.settings && ray.visibility && ray.settingsSize)
        target.updateRayShadowDescriptors(image, ray.settings, ray.settingsSize, ray.visibility);
    }
  }
}

void LightingManager::createTiledFrameDescriptors(LightingManager& target, uint32_t imageCount) const {
  constexpr uint32_t setsPerImage = 4;
  const std::array<VkDescriptorPoolSize, 3> sizes{{
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount * 7}}};
  const std::array layouts{tileCullSet0Layout_, tileCullSet1Layout_, tileCullSet2Layout_, tiledDescriptorSetLayout_};
  const auto device = device_->device();
  target.tiledSets_.resize(imageCount);
  target.tileCullDescriptorPool_ = createFrameDescriptorPool(device, sizes, imageCount * setsPerImage);
  for (auto& sets : target.tiledSets_)
    allocateFrameDescriptorSets(device, target.tileCullDescriptorPool_, layouts, sets);
}

bool LightingManager::ltcReady() const {
  return ltcLuts_ && ltcLuts_->ready();
}
const std::string &LightingManager::ltcStatus() const {
  static const std::string uninitialized = "LTC resources not initialized";
  return ltcLuts_ ? ltcLuts_->status() : uninitialized;
}
uint64_t LightingManager::ltcMemoryBytes() const {
  return ltcLuts_ ? ltcLuts_->allocatedBytes() : 0;
}

void LightingManager::updateRayShadowDescriptors(uint32_t imageIndex,
                                                 VkBuffer settings,
                                                 VkDeviceSize settingsSize,
                                                 VkImageView visibility) {
  if (imageIndex >= lightDescriptorSets_.size() || imageIndex >= rayShadowBindings_.size() ||
      settings == VK_NULL_HANDLE || !settingsSize || visibility == VK_NULL_HANDLE)
    throw std::invalid_argument("Invalid ray-shadow light descriptor binding");
  const auto& old = rayShadowBindings_[imageIndex];
  if (old.settings == settings && old.settingsSize == settingsSize && old.visibility == visibility) return;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device_->physicalDevice(), &properties);
  if (settingsSize > properties.limits.maxUniformBufferRange)
    throw std::invalid_argument("Ray-shadow settings exceed uniform descriptor range");
  if (descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Lighting descriptor binding epoch exhausted");
  invalidateRayShadowBindingPreparations();
  writeRayShadowBindings(device_->device(), lightDescriptorSet(imageIndex), settings, settingsSize, visibility);
  rayShadowBindings_[imageIndex] = {settings, settingsSize, visibility};
}

void LightingManager::createLightGizmoIconDescriptorResources() {
  if (lightGizmoIconDescriptorSetLayout_ == VK_NULL_HANDLE) {
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
    }};
    lightGizmoIconDescriptorSetLayout_ =
        pipelineManager_.createDescriptorSetLayout(
            {bindings.begin(), bindings.end()}, {0, 0});
  }

  if (lightGizmoIconDescriptorPool_ == VK_NULL_HANDLE) {
    lightGizmoIconDescriptorPool_ = pipelineManager_.createDescriptorPool(
        {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
         {VK_DESCRIPTOR_TYPE_SAMPLER, 1}},
        1, 0);
  }

  if (lightGizmoIconDescriptorSet_ == VK_NULL_HANDLE) {
    VkDescriptorSetAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = lightGizmoIconDescriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &lightGizmoIconDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
                                 &lightGizmoIconDescriptorSet_) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to allocate light gizmo icon descriptor");
    }
  }
}

void LightingManager::loadLightGizmoIconResources(
    const std::filesystem::path &assetRoot) {
  if (lightGizmoIconsReady_) {
    return;
  }

  createLightGizmoIconDescriptorResources();
  const std::vector<std::byte> atlasPixels =
      loadLightGizmoIconAtlasRgba(assetRoot, kLightGizmoIconSize);
  lightGizmoIconAtlas_ = allocationManager_.createTexture2DArrayFromRgbaPixels(
      "light-gizmo-icons", atlasPixels, kLightGizmoIconSize,
      kLightGizmoIconSize, kLightGizmoIconLayerCount, VK_FORMAT_R8G8B8A8_UNORM,
      container::gpu::TextureAllocationLifetime::Persistent);

  if (lightGizmoIconSampler_ == VK_NULL_HANDLE) {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (createOwnedSampler(device_->device(), &samplerInfo, nullptr,
                        &lightGizmoIconSampler_) != VK_SUCCESS) {
      throw std::runtime_error("failed to create light gizmo icon sampler");
    }
  }

  writeLightGizmoIconDescriptor();
  lightGizmoIconsReady_ = true;
}

void LightingManager::writeLightGizmoIconDescriptor() const {
  if (lightGizmoIconDescriptorSet_ == VK_NULL_HANDLE ||
      lightGizmoIconAtlas_.imageView == VK_NULL_HANDLE ||
      lightGizmoIconSampler_ == VK_NULL_HANDLE) {
    return;
  }

  VkDescriptorImageInfo imageInfo{};
  imageInfo.imageView = lightGizmoIconAtlas_.imageView;
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkDescriptorImageInfo samplerInfo{};
  samplerInfo.sampler = lightGizmoIconSampler_;

  std::array<VkWriteDescriptorSet, 2> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = lightGizmoIconDescriptorSet_;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &imageInfo;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = lightGizmoIconDescriptorSet_;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[1].pImageInfo = &samplerInfo;

  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

void LightingManager::createLightVolumeGeometry() {
  lightVolumeIndexCount_ = 36;
}

SceneLightingAnchor LightingManager::computeSceneLightingAnchor() const {
  SceneLightingAnchor anchor{};
  if (!sceneManager_)
    return anchor;

  const auto &bounds = boundsOverride_?*boundsOverride_:sceneManager_->modelBounds();
  if (const auto *root = sceneGraph_.getNode(rootNode_)) {
    anchor.sceneTransform = root->worldTransform;
  }

  const glm::vec3 localCenter = bounds.valid ? bounds.center : glm::vec3(0.0f);
  anchor.center =
      glm::vec3(anchor.sceneTransform * glm::vec4(localCenter, 1.0f));

  const float sceneScale =
      std::max({glm::length(glm::vec3(anchor.sceneTransform[0])),
                glm::length(glm::vec3(anchor.sceneTransform[1])),
                glm::length(glm::vec3(anchor.sceneTransform[2])), 1.0f});
  anchor.localRadius = std::max(bounds.valid ? bounds.radius : 10.0f, 1.0f);
  anchor.worldRadius = anchor.localRadius * sceneScale;
  return anchor;
}

glm::vec3 LightingManager::directionalLightPosition() const {
  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  return anchor.center - glm::vec3(lightingData_.directionalDirection) *
                             (anchor.worldRadius * 1.15f);
}

void LightingManager::uploadLightingData() const {
  uploadLightingData(*this);
}

void LightingManager::uploadLightingData(const LightingManager& source) const {
  for (uint32_t image = 0; image < lightingBuffers_.size(); ++image)
    uploadLightingData(image, source);
}

void LightingManager::uploadLightingData(uint32_t imageIndex) const {
  uploadLightingData(imageIndex, *this);
}

void LightingManager::uploadLightingData(uint32_t imageIndex, const LightingManager& source) const {
  if (imageIndex >= lightingBuffers_.size()) return;
  SceneController::writeToBuffer(allocationManager_, lightingBuffers_[imageIndex],
                                 &source.lightingData_, sizeof(LightingData));
  const auto points = std::min<size_t>(source.pointLightsSsbo_.size(), kMaxClusteredLights);
  const auto areas = std::min<size_t>(source.areaLightsSsbo_.size(), kMaxAreaLights);
  if (points)
    SceneController::writeToBuffer(allocationManager_, pointLightBuffers_[imageIndex],
                                   source.pointLightsSsbo_.data(), sizeof(PointLightData) * points);
  if (areas)
    SceneController::writeToBuffer(allocationManager_, areaLightBuffers_[imageIndex],
                                   source.areaLightsSsbo_.data(), sizeof(AreaLightData) * areas);
}

void LightingManager::setLightingSettings(const LightingSettings &settings) {
  const bool generatorSettingsChanged =
      generatedLightingSettingsDiffer(lightingSettings_, settings);
  if(generatorSettingsChanged&&generatedLightRevision_==UINT64_MAX)
    throw std::overflow_error("Generated light lifetime exhausted");
  lightingSettings_.preset = std::min(settings.preset, 3u);
  lightingSettings_.density = std::clamp(settings.density, 0.1f, 16.0f);
  lightingSettings_.radiusScale = std::clamp(settings.radiusScale, 0.05f, 8.0f);
  lightingSettings_.intensityScale =
      std::clamp(settings.intensityScale, 0.0f, 16.0f);
  lightingSettings_.directionalIntensity =
      clampRendererLightingValue(RendererLightingSetting::DirectionalIntensity, settings.directionalIntensity);
  lightingSettings_.environmentIntensity =
      clampRendererLightingValue(RendererLightingSetting::EnvironmentIntensity, settings.environmentIntensity);
  const auto bounded = [](float value, float min, float max) {
    return std::isfinite(value) ? std::clamp(value, min, max) : 0.0f;
  };
  lightingSettings_.environment = settings.environment;
  auto& environment = lightingSettings_.environment;
  environment.yawDegrees = clampRendererLightingValue(RendererLightingSetting::EnvironmentYaw, environment.yawDegrees);
  environment.backgroundBlur = bounded(environment.backgroundBlur, 0.0f, 1.0f);
  for (float& channel : environment.backgroundColor)
    channel = bounded(channel, 0.0f, 1.0f);
  lightingSettings_.bounceIntensity =
      clampRendererLightingValue(RendererLightingSetting::BounceIntensity, settings.bounceIntensity);
  lightingSettings_.localShadowPointBudget =
      std::min(settings.localShadowPointBudget,
               kMaxLocalShadowOmniPointBudget);
  lightingSettings_.localShadowLayerBudget =
      std::min(settings.localShadowLayerBudget, kMaxShadowedLocalLightLayers);
  lightingSettings_.areaLightingMode = std::min(settings.areaLightingMode, 1u);
  lightingSettings_.areaLightSampleCount = settings.areaLightSampleCount <= 9u
                                             ? 9u
                                             : settings.areaLightSampleCount <= 25u
                                                   ? 25u
                                                   : 64u;
  lightingSettings_.areaEmitterDebug = settings.areaEmitterDebug;
  if (generatorSettingsChanged) {
    ++generatedLightRevision_;
    generatedPointOverrides_.clear();
    generatedAreaOverrides_.clear();
    if (directionalOverride_ &&
        directionalOverride_->source == EditableLightSource::Generated) {
      directionalOverride_.reset();
    }
  }
}

SceneLightEdits LightingManager::snapshotSceneLightEdits() const {
  synchronizeLightEditIdentities();
  return {.generationSettings = lightingSettings_,
          .directionalOverride = directionalOverride_,
          .generatedPointOverrides = generatedPointOverrides_,
          .importedPointOverrides = importedPointOverrides_,
          .generatedAreaOverrides = generatedAreaOverrides_,
          .importedAreaOverrides = importedAreaOverrides_,
          .manualPointLights = manualPointLights_,
          .manualAreaLights = manualAreaLights_,.identities=lightEditIdentities_};
}

uint64_t LightingManager::sceneLightEditsStorageBytes() const noexcept {
  uint64_t bytes = sizeof(SceneLightEdits);
  const auto charge = [&bytes](uint64_t count, uint64_t stride) {
    const auto maximum = std::numeric_limits<uint64_t>::max();
    if (count > (maximum - bytes) / stride) bytes = maximum;
    else bytes += count * stride;
  };
  const auto vectorBytes = [&charge](const auto& values) {
    using Value = typename std::decay_t<decltype(values)>::value_type;
    charge(values.size(), sizeof(Value));
  };
  vectorBytes(generatedPointOverrides_);
  vectorBytes(importedPointOverrides_);
  vectorBytes(generatedAreaOverrides_);
  vectorBytes(importedAreaOverrides_);
  vectorBytes(manualPointLights_);
  vectorBytes(manualAreaLights_);
  if (directionalOverride_) {
    charge(directionalOverride_->label.size(), 1);
    charge(1, 1);
  }
  if(lightEditIdentities_) charge(lightEditIdentities_->bytes,1);
  return bytes;
}

void LightingManager::restoreSceneLightEdits(const SceneLightEdits &edits) {
  // Copy before committing so allocation failure leaves all live edits intact.
  auto restored = edits;
  if(restored.generationSettings && generatedLightingSettingsDiffer(*restored.generationSettings,lightingSettings_)) {
    restored.generatedPointOverrides.clear(); restored.generatedAreaOverrides.clear();
    if(restored.directionalOverride && restored.directionalOverride->source==EditableLightSource::Generated)
      restored.directionalOverride.reset();
  }
  directionalOverride_.swap(restored.directionalOverride);
  generatedPointOverrides_.swap(restored.generatedPointOverrides);
  importedPointOverrides_.swap(restored.importedPointOverrides);
  generatedAreaOverrides_.swap(restored.generatedAreaOverrides);
  importedAreaOverrides_.swap(restored.importedAreaOverrides);
  manualPointLights_.swap(restored.manualPointLights);
  manualAreaLights_.swap(restored.manualAreaLights);
  lightEditIdentities_=restored.identities;
  if(lightEditIdentities_) generatedLightRevision_=lightEditIdentities_->generator;
  selectedEditableLight_ = invalidEditableLightId();
  for (auto &light : editableLights_) light.selected = false;
  if(lightEditRevision_!=UINT64_MAX) ++lightEditRevision_;
}

void LightingManager::clearSceneLightOverrides() {
  if(lightEditRevision_!=UINT64_MAX) ++lightEditRevision_;
  importedPointOverrides_.clear();
  importedAreaOverrides_.clear();
  generatedPointOverrides_.clear();
  generatedAreaOverrides_.clear();
  if (directionalOverride_ &&
      directionalOverride_->source != EditableLightSource::Manual) {
    directionalOverride_.reset();
  }
  selectedEditableLight_ = invalidEditableLightId();
  updateLightingData();
}

void LightingManager::collectStats() {
  if (!isTiledLightingReady() || lastDispatchClusterCount_ == 0) {
    return;
  }

  VmaAllocationInfo allocInfo{};
  vmaGetAllocationInfo(allocationManager_.memoryManager()->allocator(),
                       lightStatsBuffer_.allocation, &allocInfo);
  if (allocInfo.pMappedData == nullptr)
    return;

  vmaInvalidateAllocation(allocationManager_.memoryManager()->allocator(),
                          lightStatsBuffer_.allocation, 0,
                          sizeof(uint32_t) * 4);
  const auto *data = static_cast<const uint32_t *>(allocInfo.pMappedData);
  lastStats_.maxLightsPerCluster = data[0];
  lastStats_.droppedLightReferences = data[1];
  lastStats_.activeClusters = data[2];
  lastStats_.submittedLights = data[3];
  lastStats_.totalClusters = lastDispatchClusterCount_;

  if (timestampQueryPool_ != VK_NULL_HANDLE && timingQueriesWritten_) {
    std::array<uint64_t, kTimingQueryCount * 2u> queryResults{};
    const VkResult queryResult = vkGetQueryPoolResults(
        device_->device(), timestampQueryPool_, lastTimingQueryBase_,
        kTimingQueryCount, sizeof(queryResults), queryResults.data(),
        sizeof(uint64_t) * 2u,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    const auto queryAvailable = [&](uint32_t queryIndex) {
      return queryResults[queryIndex * 2u + 1u] != 0u;
    };
    const auto elapsedMs = [&](uint32_t startQuery, uint32_t endQuery) {
      if (queryResult != VK_SUCCESS || !queryAvailable(startQuery) ||
          !queryAvailable(endQuery)) {
        return 0.0f;
      }
      const uint64_t start = queryResults[startQuery * 2u];
      const uint64_t end = queryResults[endQuery * 2u];
      if (end <= start)
        return 0.0f;
      return static_cast<float>(static_cast<double>(end - start) *
                                static_cast<double>(timestampPeriodNs_) *
                                1.0e-6);
    };
    const float clusterCullMs =
        elapsedMs(kClusterCullStartQuery, kClusterCullEndQuery);
    const float clusteredLightingMs =
        elapsedMs(kClusteredLightingStartQuery, kClusteredLightingEndQuery);
    if (clusterCullMs > 0.0f)
      lastStats_.clusterCullMs = clusterCullMs;
    if (clusteredLightingMs > 0.0f)
      lastStats_.clusteredLightingMs = clusteredLightingMs;
  }
}

bool LightingManager::applyAuthoredDirectionalLight(
    const SceneLightingAnchor &anchor) {
  const auto lights=authoredLightsOverride_?std::span<const container::scene::AuthoredDirectionalLight>(authoredLightsOverride_->directionals):
      sceneManager_?std::span<const container::scene::AuthoredDirectionalLight>(sceneManager_->authoredDirectionalLights()):
      std::span<const container::scene::AuthoredDirectionalLight>{};
  if (lights.empty()) {
    return false;
  }

  const auto &authoredLight =
      lights.front();
  const glm::vec3 fallbackDirection =
      normalizeOr(glm::vec3(lightingData_.directionalDirection),
                  glm::vec3(0.0f, 0.0f, 1.0f));
  const glm::vec3 worldDirection = normalizeOr(
      glm::vec3(anchor.sceneTransform *
                glm::vec4(glm::vec3(authoredLight.direction), 0.0f)),
      fallbackDirection);
  lightingData_.directionalDirection = glm::vec4(worldDirection, 0.0f);
  lightingData_.directionalColorIntensity = authoredLight.colorIntensity;
  return true;
}

void LightingManager::appendAuthoredPointLights(
    const SceneLightingAnchor &anchor) {
  if (!sceneManager_) {
    return;
  }

  const auto lights=authoredLightsOverride_?std::span<const PointLightData>(authoredLightsOverride_->points):
      std::span<const PointLightData>(sceneManager_->authoredPointLights());
  for (const PointLightData &authoredLight : lights) {
    if (pointLightsSsbo_.size() >= kMaxClusteredLights) {
      return;
    }

    PointLightData light = authoredLight;
    light.positionRadius = glm::vec4(
        glm::vec3(anchor.sceneTransform *
                  glm::vec4(glm::vec3(authoredLight.positionRadius), 1.0f)),
        authoredLight.positionRadius.w);
    if (authoredLight.coneOuterCosType.y >= 0.5f || authoredLight.iesProfileAddress) {
      const glm::vec3 fallbackDirection =
          normalizeOr(glm::vec3(authoredLight.directionInnerCos),
                      glm::vec3(0.0f, 0.0f, -1.0f));
      const glm::vec3 worldDirection = normalizeOr(
          glm::vec3(
              anchor.sceneTransform *
              glm::vec4(glm::vec3(authoredLight.directionInnerCos), 0.0f)),
          fallbackDirection);
      light.directionInnerCos =
          glm::vec4(worldDirection, authoredLight.directionInnerCos.w);
      if (authoredLight.iesProfileAddress) {
        const glm::vec3 tangent = glm::vec3(anchor.sceneTransform * glm::vec4(glm::vec3(authoredLight.iesTangent), 0.0f));
        const glm::vec3 axis = std::abs(worldDirection.x) < 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 fallback = glm::normalize(axis - worldDirection * glm::dot(axis, worldDirection));
        light.iesTangent = glm::vec4(normalizeOr(tangent - worldDirection * glm::dot(tangent, worldDirection), fallback), 1.0f);
      }
    }
    pointLightsSsbo_.push_back(light);
  }
}

void LightingManager::appendAuthoredAreaLights(
    const SceneLightingAnchor &anchor) {
  if (!sceneManager_) {
    return;
  }

  const auto lights=authoredLightsOverride_?std::span<const AreaLightData>(authoredLightsOverride_->areas):
      std::span<const AreaLightData>(sceneManager_->authoredAreaLights());
  for (const AreaLightData &authoredLight : lights) {
    if (areaLightsSsbo_.size() >= kMaxAreaLights) {
      return;
    }

    AreaLightData light = authoredLight;
    light.positionRange = glm::vec4(
        glm::vec3(anchor.sceneTransform *
                  glm::vec4(glm::vec3(authoredLight.positionRange), 1.0f)),
        authoredLight.positionRange.w);

    const glm::vec3 fallbackDirection = normalizeOr(
        glm::vec3(authoredLight.directionType), glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::vec3 worldDirection = normalizeOr(
        glm::vec3(anchor.sceneTransform *
                  glm::vec4(glm::vec3(authoredLight.directionType), 0.0f)),
        fallbackDirection);
    light.directionType =
        glm::vec4(worldDirection, authoredLight.directionType.w);

    const glm::vec3 fallbackTangent = normalizeOr(
        glm::vec3(authoredLight.tangentHalfSize), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::vec3 worldTangent = normalizeOr(
        glm::vec3(anchor.sceneTransform *
                  glm::vec4(glm::vec3(authoredLight.tangentHalfSize), 0.0f)),
        fallbackTangent);
    light.tangentHalfSize =
        glm::vec4(worldTangent, authoredLight.tangentHalfSize.w);

    const glm::vec3 fallbackBitangent =
        normalizeOr(glm::vec3(authoredLight.bitangentHalfSize),
                    glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 worldBitangent = normalizeOr(
        glm::vec3(anchor.sceneTransform *
                  glm::vec4(glm::vec3(authoredLight.bitangentHalfSize), 0.0f)),
        fallbackBitangent);
    light.bitangentHalfSize =
        glm::vec4(worldBitangent, authoredLight.bitangentHalfSize.w);
    areaLightsSsbo_.push_back(light);
  }
}

void LightingManager::assignLocalShadowLayerMetadata() {
  (void)container::renderer::assignLocalShadowLayerMetadata(
      pointLightsSsbo_,
      {.omniPointBudget = lightingSettings_.localShadowPointBudget,
       .layerBudget = lightingSettings_.localShadowLayerBudget});
}

void LightingManager::publishPointLights() {
  assignLocalShadowLayerMetadata();
  if(!suppressWorldPublication_) world_.replacePointLights(pointLightsSsbo_);
  // ECS storage traversal is not the authored light-list order. Preserve this
  // canonical order for editor IDs, overrides, shadow indices and scene save;
  // publishing the same records to ECS must not permute those identities.
  syncLightingPointCount(lightingData_, pointLightsSsbo_);
}

void LightingManager::publishAreaLights() {
  syncLightingAreaCount(lightingData_, areaLightsSsbo_);
}

std::optional<EditableLightEntity>
LightingManager::selectedEditableLight() const {
  for (const EditableLightEntity &light : editableLights_) {
    if (light.id == selectedEditableLight_) {
      return light;
    }
  }
  return std::nullopt;
}

void LightingManager::selectEditableLight(EditableLightId id) {
  selectedEditableLight_ = id;
  for (EditableLightEntity &light : editableLights_) {
    light.selected = light.id == selectedEditableLight_;
  }
}

bool LightingManager::updateEditableLight(const EditableLightEntity &entity) {
  if (!isValidEditableLightId(entity.id)||lightEditRevision_==UINT64_MAX||
      (entity.id.source!=EditableLightSource::Imported&&entity.id.source!=EditableLightSource::Generated&&entity.id.source!=EditableLightSource::Manual)) {
    return false;
  }

  EditableLightEntity edited = entity;
  edited.type = entity.id.type;
  edited.source = entity.id.source;
  edited.selected = true;

  if (edited.type == EditableLightType::Directional) {
    std::optional<EditableLightEntity> staged(std::move(edited));directionalOverride_.swap(staged);
    selectedEditableLight_=entity.id;++lightEditRevision_;
    return true;
  }

  if (edited.type == EditableLightType::Point ||
      edited.type == EditableLightType::Spot) {
    const PointLightData data = pointLightDataFromEditable(edited);
    if (edited.source == EditableLightSource::Manual) {
      if (edited.id.index >= manualPointLights_.size()) {
        return false;
      }
      manualPointLights_[edited.id.index] = data;
      selectedEditableLight_=edited.id;++lightEditRevision_;
      return true;
    }

    auto &overrides = edited.source == EditableLightSource::Imported
                          ? importedPointOverrides_
                          : generatedPointOverrides_;
    if (overrides.size() <= edited.id.index) {
      overrides.resize(static_cast<size_t>(edited.id.index) + 1u);
    }
    overrides[edited.id.index] = data;
    selectedEditableLight_=edited.id;++lightEditRevision_;
    return true;
  }

  if (edited.type == EditableLightType::Area) {
    const AreaLightData data = areaLightDataFromEditable(edited);
    if (edited.source == EditableLightSource::Manual) {
      if (edited.id.index >= manualAreaLights_.size()) {
        return false;
      }
      manualAreaLights_[edited.id.index] = data;
      selectedEditableLight_=edited.id;++lightEditRevision_;
      return true;
    }

    auto &overrides = edited.source == EditableLightSource::Imported
                          ? importedAreaOverrides_
                          : generatedAreaOverrides_;
    if (overrides.size() <= edited.id.index) {
      overrides.resize(static_cast<size_t>(edited.id.index) + 1u);
    }
    overrides[edited.id.index] = data;
    selectedEditableLight_=edited.id;++lightEditRevision_;
    return true;
  }

  return false;
}

EditableLightId
LightingManager::addManualEditableLight(EditableLightType type) {
  if(lightEditRevision_==UINT64_MAX) throw std::overflow_error("Light editor revision exhausted");
  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const glm::vec3 center = anchor.center;
  const float radius = std::max(anchor.worldRadius, 1.0f);

  if (type == EditableLightType::Directional) {
    EditableLightEntity manual = editableDirectionalLight(
        EditableLightSource::Manual, directionalLightPosition(), lightingData_,
        true);
    manual.id = {.type = EditableLightType::Directional,
                 .source = EditableLightSource::Manual,
                 .index = 0u};
    manual.source = EditableLightSource::Manual;
    manual.label = "Manual Directional";
    directionalOverride_ = manual;
    selectedEditableLight_ = manual.id;
    ++lightEditRevision_;
    return manual.id;
  }

  if (type == EditableLightType::Area) {
    AreaLightData light{};
    light.positionRange = glm::vec4(
        center + glm::vec3(0.0f, radius * 0.35f, 0.0f), radius * 1.5f);
    light.colorIntensity = glm::vec4(1.0f, 0.92f, 0.82f, 8.0f);
    light.directionType =
        glm::vec4(0.0f, -1.0f, 0.0f, container::gpu::kAreaLightTypeRectangle);
    light.tangentHalfSize = glm::vec4(1.0f, 0.0f, 0.0f, radius * 0.12f);
    light.bitangentHalfSize = glm::vec4(0.0f, 0.0f, 1.0f, radius * 0.12f);
    const uint32_t index = static_cast<uint32_t>(manualAreaLights_.size());
    manualAreaLights_.push_back(light);
    selectedEditableLight_ = {.type = EditableLightType::Area,
                              .source = EditableLightSource::Manual,
                              .index = index};
    ++lightEditRevision_;
    return selectedEditableLight_;
  }

  PointLightData light{};
  light.positionRadius =
      glm::vec4(center + glm::vec3(0.0f, radius * 0.25f, 0.0f), radius * 1.25f);
  light.colorIntensity = glm::vec4(1.0f, 0.9f, 0.78f, 6.0f);
  light.directionInnerCos = glm::vec4(0.0f, -1.0f, 0.0f, 1.0f);
  light.coneOuterCosType =
      glm::vec4(0.0f, container::gpu::kLightTypePoint, 0.0f, 0.0f);
  if (type == EditableLightType::Spot) {
    light.directionInnerCos =
        glm::vec4(0.0f, -1.0f, 0.0f, std::cos(glm::radians(18.0f)));
    light.coneOuterCosType =
        glm::vec4(std::cos(glm::radians(32.0f)), container::gpu::kLightTypeSpot,
                  0.0f, 0.0f);
  }
  const uint32_t index = static_cast<uint32_t>(manualPointLights_.size());
  manualPointLights_.push_back(light);
  selectedEditableLight_ = {
      .type = type, .source = EditableLightSource::Manual, .index = index};
  ++lightEditRevision_;
  return selectedEditableLight_;
}

bool LightingManager::translateSelectedEditableLight(const glm::vec3 &delta) {
  auto selected = selectedEditableLight();
  if (!selected || selected->type == EditableLightType::Directional) {
    return false;
  }
  selected->position += delta;
  return updateEditableLight(*selected);
}

bool LightingManager::rotateSelectedEditableLight(const glm::vec3 &axis,
                                                  float degrees) {
  auto selected = selectedEditableLight();
  if (!selected || selected->type == EditableLightType::Point) {
    return false;
  }
  selected->direction = rotateVector(selected->direction, axis, degrees);
  if (selected->type == EditableLightType::Area) {
    selected->tangent = rotateVector(selected->tangent, axis, degrees);
    selected->bitangent = rotateVector(selected->bitangent, axis, degrees);
  }
  return updateEditableLight(*selected);
}

bool LightingManager::scaleSelectedEditableLight(float factor) {
  auto selected = selectedEditableLight();
  if (!selected || selected->type == EditableLightType::Directional ||
      !std::isfinite(factor) || factor <= 0.0f) {
    return false;
  }
  const float clampedFactor = std::clamp(factor, 0.05f, 20.0f);
  selected->range = std::max(selected->range * clampedFactor, 0.001f);
  if (selected->type == EditableLightType::Area) {
    selected->areaHalfSize =
        glm::max(selected->areaHalfSize * clampedFactor, glm::vec2(0.001f));
  }
  return updateEditableLight(*selected);
}

void LightingManager::applyEditableLightOverrides(
    EditableLightSource directionalSource, EditableLightSource pointSource,
    size_t sourcePointCount, EditableLightSource areaSource,
    size_t sourceAreaCount) {
  if (directionalOverride_ &&
      (directionalOverride_->source == EditableLightSource::Manual ||
       directionalOverride_->source == directionalSource)) {
    lightingData_ =
        directionalLightDataFromEditable(*directionalOverride_, lightingData_);
  }

  auto applyPointOverrides =
      [&](const std::vector<std::optional<PointLightData>> &overrides) {
        const size_t count = std::min(
            {sourcePointCount, pointLightsSsbo_.size(), overrides.size()});
        for (size_t i = 0; i < count; ++i) {
          if (!overrides[i]) continue;
          auto& light = pointLightsSsbo_[i];
          const auto& edited = *overrides[i];
          if (light.iesProfileAddress) {
            if (edited.iesProfileAddress == light.iesProfileAddress) light.colorIntensity = edited.colorIntensity;
          } else if (!edited.iesProfileAddress) {
            light = edited;
          }
        }
      };
  applyPointOverrides(pointSource == EditableLightSource::Imported
                          ? importedPointOverrides_
                          : generatedPointOverrides_);

  auto applyAreaOverrides =
      [&](const std::vector<std::optional<AreaLightData>> &overrides) {
        const size_t count = std::min(
            {sourceAreaCount, areaLightsSsbo_.size(), overrides.size()});
        for (size_t i = 0; i < count; ++i) {
          if (overrides[i]) {
            areaLightsSsbo_[i] = *overrides[i];
          }
        }
      };
  applyAreaOverrides(areaSource == EditableLightSource::Imported
                         ? importedAreaOverrides_
                         : generatedAreaOverrides_);
}

void LightingManager::appendManualEditableLights(const SceneLightingAnchor &) {
  for (const PointLightData &light : manualPointLights_) {
    if (pointLightsSsbo_.size() >= kMaxClusteredLights) {
      break;
    }
    pointLightsSsbo_.push_back(light);
  }
  for (const AreaLightData &light : manualAreaLights_) {
    if (areaLightsSsbo_.size() >= kMaxAreaLights) {
      break;
    }
    areaLightsSsbo_.push_back(light);
  }
}

void LightingManager::rebuildEditableLights(
    EditableLightSource directionalSource, EditableLightSource pointSource,
    size_t sourcePointCount, EditableLightSource areaSource,
    size_t sourceAreaCount) {
  editableLights_.clear();
  editableLights_.reserve(1u + sourcePointCount + sourceAreaCount +
                          manualPointLights_.size() + manualAreaLights_.size());

  auto appendEditable = [&](EditableLightEntity light) {
    light.selected = light.id == selectedEditableLight_;
    editableLights_.push_back(std::move(light));
  };

  appendEditable(editableDirectionalLight(
      directionalOverride_ &&
              directionalOverride_->source == EditableLightSource::Manual
          ? EditableLightSource::Manual
          : directionalSource,
      directionalLightPosition(), lightingData_,
      selectedEditableLight_.type == EditableLightType::Directional));

  for (size_t i = 0; i < sourcePointCount && i < pointLightsSsbo_.size(); ++i) {
    appendEditable(editablePointLight(pointSource, static_cast<uint32_t>(i),
                                      pointLightsSsbo_[i], false));
  }
  for (size_t i = 0; i < sourceAreaCount && i < areaLightsSsbo_.size(); ++i) {
    appendEditable(editableAreaLight(areaSource, static_cast<uint32_t>(i),
                                     areaLightsSsbo_[i], false));
  }
  for (uint32_t i = 0u; i < manualPointLights_.size(); ++i) {
    appendEditable(editablePointLight(EditableLightSource::Manual, i,
                                      manualPointLights_[i], false));
  }
  for (uint32_t i = 0u; i < manualAreaLights_.size(); ++i) {
    appendEditable(editableAreaLight(EditableLightSource::Manual, i,
                                     manualAreaLights_[i], false));
  }

  const bool selectionStillExists = std::ranges::any_of(
      editableLights_, [&](const EditableLightEntity &light) {
        return light.id == selectedEditableLight_;
      });
  if (!selectionStillExists) {
    selectedEditableLight_ = invalidEditableLightId();
    for (EditableLightEntity &light : editableLights_) {
      light.selected = false;
    }
  }
}

void LightingManager::updateLightingData() {
  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const glm::mat4 &sceneTransform = anchor.sceneTransform;

  lightingData_ = {};
  lightingData_.environmentIntensity = lightingSettings_.environmentIntensity;
  const auto& environment = lightingSettings_.environment;
  const float yaw = glm::radians(environment.yawDegrees);
  lightingData_.environmentOrientation = {std::cos(yaw), std::sin(yaw),
                                           environment.backgroundBlur, 0.0f};
  lightingData_.environmentBackground = {environment.backgroundColor[0],
      environment.backgroundColor[1], environment.backgroundColor[2],
      environment.solidBackground ? 1.0f : 0.0f};
  lightingData_.bounceIntensity = lightingSettings_.bounceIntensity;
  lightingData_.areaLightingMode =
      lightingSettings_.areaLightingMode != 0u && ltcReady() ? 1u : 0u;
  lightingData_.areaLightSampleCount = lightingSettings_.areaLightSampleCount;
  const glm::vec3 baseDir = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.3f));
  lightingData_.directionalDirection = glm::vec4(
      glm::normalize(glm::vec3(sceneTransform * glm::vec4(baseDir, 0.0f))),
      0.0f);

  lightingData_.directionalColorIntensity =
      glm::vec4(1.0f, 0.96f, 0.9f, lightingSettings_.directionalIntensity);
  const bool authoredDirectionalApplied = applyAuthoredDirectionalLight(anchor);
  const EditableLightSource directionalSource =
      authoredDirectionalApplied ? EditableLightSource::Imported
                                 : EditableLightSource::Generated;

  pointLightsSsbo_.clear();
  pointLightsSsbo_.reserve(std::min<uint32_t>(kMaxClusteredLights, 256u));
  appendAuthoredPointLights(anchor);
  areaLightsSsbo_.clear();
  areaLightsSsbo_.reserve(std::min<uint32_t>(kMaxAreaLights, 64u));
  appendAuthoredAreaLights(anchor);

  const size_t sourcePointCount = pointLightsSsbo_.size();
  const size_t sourceAreaCount = areaLightsSsbo_.size();
  const EditableLightSource pointSource =
      sourcePointCount > 0u ? EditableLightSource::Imported
                            : EditableLightSource::Generated;
  const EditableLightSource areaSource =
      sourceAreaCount > 0u ? EditableLightSource::Imported
                           : EditableLightSource::Generated;
  applyEditableLightOverrides(directionalSource, pointSource, sourcePointCount,
                              areaSource, sourceAreaCount);
  appendManualEditableLights(anchor);
  publishPointLights();
  publishAreaLights();
  rebuildEditableLights(directionalSource, pointSource, sourcePointCount,
                        areaSource, sourceAreaCount);
}

void LightingManager::updateLightingData(
    const container::scene::BaseCamera *) {
  updateLightingData();
}

void LightingManager::updateLightingDataForActiveCamera() {
  updateLightingData(nullptr);
}

void LightingManager::drawLightGizmos(
    VkCommandBuffer commandBuffer,
    const std::array<VkDescriptorSet, 2> &lightingDescriptorSets,
    VkPipeline lightGizmoPipeline, VkPipeline lightGizmoCoveragePipeline,
    VkPipelineLayout lightingPipelineLayout,
    const container::scene::BaseCamera *camera) const {
  if (lightGizmoPipeline == VK_NULL_HANDLE ||
      lightGizmoCoveragePipeline == VK_NULL_HANDLE ||
      lightingPipelineLayout == VK_NULL_HANDLE || !lightGizmoIconsReady_ ||
      lightGizmoIconDescriptorSet_ == VK_NULL_HANDLE) {
    return;
  }

  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const auto *activeCamera = world_.activeCamera();
  const glm::vec3 cameraPos =
      activeCamera ? glm::vec3(activeCamera->data.cameraWorldPosition)
                   : (camera ? camera->position() : anchor.center);

  const DeferredLightGizmoPlan gizmoPlan = buildDeferredLightGizmoPlan(
      {.sceneCenter = anchor.center,
       .sceneWorldRadius = anchor.worldRadius,
       .cameraPosition = cameraPos,
       .directionalDirection = glm::vec3(lightingData_.directionalDirection),
       .directionalColor = glm::vec3(lightingData_.directionalColorIntensity),
       .editableLights = std::span<const EditableLightEntity>(
           editableLights_.data(), editableLights_.size()),
       .areaEmitterDebug = lightingSettings_.areaEmitterDebug});

  static_cast<void>(recordDeferredLightGizmoCommands(
      commandBuffer,
      {.pipeline = lightGizmoPipeline,
       .coveragePipeline = lightGizmoCoveragePipeline,
       .pipelineLayout = lightingPipelineLayout,
       .descriptorSets = {lightingDescriptorSets[0],
                          lightGizmoIconDescriptorSet_},
       .pushConstants = std::span<const LightPushConstants>(
           gizmoPlan.pushConstants.data(), gizmoPlan.pushConstantCount),
       .coveragePushConstants = std::span<const LightPushConstants>(
           gizmoPlan.coveragePushConstants.data(),
           gizmoPlan.coveragePushConstantCount)}));
}

void LightingManager::drawLightGizmoPickIds(
    VkCommandBuffer commandBuffer,
    const std::array<VkDescriptorSet, 2> &lightingDescriptorSets,
    VkPipeline lightGizmoPickPipeline, VkPipelineLayout lightingPipelineLayout,
    const container::scene::BaseCamera *camera) const {
  if (lightGizmoPickPipeline == VK_NULL_HANDLE ||
      lightingPipelineLayout == VK_NULL_HANDLE || !lightGizmoIconsReady_ ||
      lightGizmoIconDescriptorSet_ == VK_NULL_HANDLE ||
      editableLights_.empty()) {
    return;
  }

  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const auto *activeCamera = world_.activeCamera();
  const glm::vec3 cameraPos =
      activeCamera ? glm::vec3(activeCamera->data.cameraWorldPosition)
                   : (camera ? camera->position() : anchor.center);

  const DeferredLightGizmoPlan gizmoPlan = buildDeferredLightGizmoPlan(
      {.sceneCenter = anchor.center,
       .sceneWorldRadius = anchor.worldRadius,
       .cameraPosition = cameraPos,
       .directionalDirection = glm::vec3(lightingData_.directionalDirection),
       .directionalColor = glm::vec3(lightingData_.directionalColorIntensity),
       .editableLights = std::span<const EditableLightEntity>(
           editableLights_.data(), editableLights_.size())});

  static_cast<void>(recordDeferredLightGizmoCommands(
      commandBuffer,
      {.pipeline = lightGizmoPickPipeline,
       .pipelineLayout = lightingPipelineLayout,
       .descriptorSets = {lightingDescriptorSets[0],
                          lightGizmoIconDescriptorSet_},
       .pushConstants = std::span<const LightPushConstants>(
           gizmoPlan.pushConstants.data(), gizmoPlan.pushConstantCount)}));
}

// ---------------------------------------------------------------------------
// Tiled light culling
// ---------------------------------------------------------------------------

void LightingManager::allocateClusterBuffers(VkExtent2D extent) {
  const auto capacity = planClusteredLightCapacity(
      extent, clusteredStorageRangeLimit_, clusteredDispatchLimits_);
  if (capacity.clusterCount > UINT32_MAX)
    throw std::length_error("Tiled lighting extent exceeds its cluster address space");

  const bool wasLimited = tiledCapacityLimited();
  const auto previousStatus = clusteredLightCapacity_.status;
  if (capacity.available() &&
      capacity.clusterCount == clusteredLightCapacity_.clusterCount &&
      tiledScratchResourcesReady()) {
    clusteredLightCapacity_ = capacity;
    return;
  }
  if (!capacity.available() && wasLimited && capacity.status == previousStatus) {
    clusteredLightCapacity_ = capacity;
    return;
  }

  // Pose generations borrow these exact scratch allocations in their staged
  // tiled sets. Invalidate before destruction, including failed replacement,
  // while a same-grid resize above remains a genuine binding no-op. Legacy
  // reversible generations deliberately rebind today's scratch at exchange.
  if (descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Lighting descriptor binding epoch exhausted");
  invalidateRayShadowBindingPreparations();
  allocationManager_.destroyBuffer(lightIndexListSsbo_);
  allocationManager_.destroyBuffer(tileGridSsbo_);
  clusteredLightCapacity_ = capacity;
  lastStats_ = {};
  lastDispatchClusterCount_ = 0;
  timingQueriesWritten_ = false;
  if (!capacity.available()) {
    container::log::ContainerLogger::instance().renderer()->warn(
        "Clustered lighting disabled at {}x{} ({}): grid {} bytes, light indices "
        "{} bytes, maxStorageBufferRange {} bytes. Deferred uses up to {} fullscreen "
        "point lights. Forward uses its regular point light list.",
        extent.width, extent.height, clusteredLightCapacityStatusName(capacity.status),
        capacity.tileGridBytes, capacity.lightIndexListBytes,
        capacity.storageRangeLimit, kMaxDeferredPointLights);
    return;
  }

  const auto allocationCheckpoint=[&](uint8_t point) {
    if(scratchAllocationFailureForTest_==point) {
      scratchAllocationFailureForTest_=0;
      throw std::bad_alloc();
    }
  };
  allocationCheckpoint(1);
  tileGridSsbo_ = allocationManager_.createBuffer(
      capacity.tileGridBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  allocationCheckpoint(2);
  lightIndexListSsbo_ = allocationManager_.createBuffer(
      capacity.lightIndexListBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
}

void LightingManager::writeTiledResourceDescriptors(const LightingManager& target) const noexcept {
  for (size_t image = 0; image < target.tiledSets_.size(); ++image) {
    const auto& sets = target.tiledSets_[image];
    writeTiledDescriptors(device_->device(), sets[1], sets[2], sets[3], target.pointLightBuffers_[image],
        tileGridSsbo_, lightIndexListSsbo_, lightStatsBuffer_, clusteredLightCapacity_);
  }
}

void LightingManager::resizeTiledResources(VkExtent2D extent) {
  if (pointLightBuffers_.empty() || lightStatsBuffer_.buffer == VK_NULL_HANDLE) return;

  const uint64_t previousBindingEpoch = descriptorBindingEpoch_;
  allocateClusterBuffers(extent);
  if (descriptorBindingEpoch_ != previousBindingEpoch) {
    writeTiledResourceDescriptors(*this);
  }
}

void LightingManager::createTiledResources(
    const std::filesystem::path &shaderDir, VkExtent2D initialExtent) {
  if (lightingBuffers_.empty())
    throw std::logic_error("Initialize lighting descriptor resources before tiled lighting");
  if (descriptorBindingEpoch_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Lighting descriptor binding epoch exhausted");
  invalidateRayShadowBindingPreparations();
  loadLightGizmoIconResources(shaderDir / "materials" / "gizmos" / "lights");

  lightStatsBuffer_ = allocationManager_.createBuffer(
      sizeof(uint32_t) * 4,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  const std::array<uint32_t, 4> zeroStats{};
  SceneController::writeToBuffer(allocationManager_, lightStatsBuffer_,
                                 zeroStats.data(),
                                 sizeof(uint32_t) * zeroStats.size());

  VkPhysicalDeviceProperties deviceProperties{};
  vkGetPhysicalDeviceProperties(device_->physicalDevice(), &deviceProperties);
  clusteredStorageRangeLimit_ = deviceProperties.limits.maxStorageBufferRange;
  std::copy_n(deviceProperties.limits.maxComputeWorkGroupCount,
              clusteredDispatchLimits_.size(), clusteredDispatchLimits_.begin());
  if (timestampQueryPool_ == VK_NULL_HANDLE) {
    timestampPeriodNs_ = deviceProperties.limits.timestampPeriod;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device_->physicalDevice(),
                                             &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        device_->physicalDevice(), &queueFamilyCount, queueFamilies.data());

    const auto queueIndices = device_->queueFamilyIndices();
    if (queueIndices.graphicsFamily.has_value() &&
        queueIndices.graphicsFamily.value() < queueFamilies.size() &&
        queueFamilies[queueIndices.graphicsFamily.value()].timestampValidBits >
            0) {
      VkQueryPoolCreateInfo queryPoolInfo{
          VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
      queryPoolInfo.queryCount = kTimingQueryFrameSlots * kTimingQueryCount;
      timestampQueriesSupported_ =
          createOwnedQueryPool(device_->device(), &queryPoolInfo, nullptr,
                            &timestampQueryPool_) == VK_SUCCESS;
      if (!timestampQueriesSupported_) {
        timestampQueryPool_ = VK_NULL_HANDLE;
      }
    }
  }

  // ---- Descriptor set layouts -----------------------------------------------
  // Set 0: camera UBO + depth texture. The compute shader uses texel Load for
  // exact G-buffer depth, so no sampler descriptor is needed.
  {
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tileCullSet0Layout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }
  // Set 1: light SSBO
  {
    const std::array<VkDescriptorSetLayoutBinding, 1> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tileCullSet1Layout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }
  // Set 2: cluster grid + light index list + stats SSBOs
  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
         nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tileCullSet2Layout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  // Tiled lighting fragment shader descriptor set (set 1 for the lighting
  // pipeline): 3 SSBO bindings: light SSBO, tile grid, light index list.
  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tiledDescriptorSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  LightingManager next(device_, allocationManager_, pipelineManager_, sceneManager_, sceneGraph_,
                       world_, DetachedGraphTag{});
  createTiledFrameDescriptors(next, static_cast<uint32_t>(lightingBuffers_.size()));

  // ---- Compute pipeline -----------------------------------------------------
  {
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size = sizeof(TileCullPushConstants);

    tileCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
        {tileCullSet0Layout_, tileCullSet1Layout_, tileCullSet2Layout_},
        {pcRange});

    std::filesystem::path compPath =
        shaderDir / "spv_shaders" / "tile_light_cull.comp.spv";
    tileCullPipeline_ = pipelineManager_.createComputePipeline(
        compPath, tileCullPipelineLayout_, "tile_light_cull");
  }

  resizeTiledResources(initialExtent);
  std::swap(tileCullDescriptorPool_, next.tileCullDescriptorPool_);
  tiledSets_.swap(next.tiledSets_);
  writeTiledResourceDescriptors(*this);
}

void LightingManager::dispatchTileCull(VkCommandBuffer cmd, uint32_t imageIndex,
                                       VkExtent2D screenExtent,
                                       VkBuffer cameraBuffer,
                                       VkDeviceSize cameraBufferSize,
                                       VkImageView depthView, float cameraNear,
                                       float cameraFar) const {
  if (!isTiledLightingReady() || imageIndex >= tiledSets_.size())
    return;

  const auto& sets = tiledSets_[imageIndex];
  const auto dispatch = planClusteredLightCapacity(
      screenExtent, clusteredStorageRangeLimit_, clusteredDispatchLimits_);
  if (!dispatch.available() ||
      dispatch.clusterCount > clusteredLightCapacity_.clusterCount) {
    lastDispatchClusterCount_ = 0;
    return;
  }
  const uint32_t totalLights = std::min(
      static_cast<uint32_t>(pointLightsSsbo_.size()), kMaxClusteredLights);
  lastDispatchClusterCount_ = static_cast<uint32_t>(dispatch.clusterCount);

  vkCmdFillBuffer(cmd, lightStatsBuffer_.buffer, 0, sizeof(uint32_t) * 4, 0u);
  if (totalLights == 0u) {
    // No point lights: keep the downstream tiled-lighting descriptor valid but
    // make every cluster empty without launching one workgroup per cluster.
    vkCmdFillBuffer(cmd, tileGridSsbo_.buffer, 0,
                    dispatch.tileGridBytes, 0u);

    VkMemoryBarrier clearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &clearBarrier, 0, nullptr, 0, nullptr);
    return;
  }

  VkMemoryBarrier statsClearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  statsClearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  statsClearBarrier.dstAccessMask =
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                       &statsClearBarrier, 0, nullptr, 0, nullptr);

  // Update this image's set 0 with its camera and depth resources.
  {
    VkDescriptorBufferInfo camInfo{cameraBuffer, 0, cameraBufferSize};
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView = depthView;
    depthInfo.imageLayout =
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> w{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = sets[0];
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &camInfo;

    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = sets[0];
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &depthInfo;

    vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(w.size()),
                           w.data(), 0, nullptr);
  }

  TileCullPushConstants pc{};
  pc.tileCountX = dispatch.tileCountX;
  pc.tileCountY = dispatch.tileCountY;
  pc.depthSliceCount = kClusterDepthSlices;
  pc.totalLights = totalLights;
  pc.cameraNear = cameraNear;
  pc.cameraFar = cameraFar;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullPipeline_);

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullPipelineLayout_, 0,
                          3, sets.data(), 0, nullptr);

  vkCmdPushConstants(cmd, tileCullPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(TileCullPushConstants), &pc);

  vkCmdDispatch(cmd, dispatch.tileCountX, dispatch.tileCountY, kClusterDepthSlices);

  // Pipeline barrier: compute writes → fragment reads on the tile SSBOs.
  VkMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void LightingManager::resetGpuTimers(VkCommandBuffer,
                                     uint32_t frameSlot) const {
  if (!timestampQueriesSupported_ || timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  const uint32_t slot = frameSlot % kTimingQueryFrameSlots;
  lastTimingQueryBase_ = slot * kTimingQueryCount;
  timingQueriesWritten_ = false;
  vkResetQueryPool(device_->device(), timestampQueryPool_, lastTimingQueryBase_,
                   kTimingQueryCount);
}

void LightingManager::beginClusterCullTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ || timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusterCullStartQuery);
}

void LightingManager::endClusterCullTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ || timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusterCullEndQuery);
  timingQueriesWritten_ = true;
}

void LightingManager::beginClusteredLightingTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ || timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusteredLightingStartQuery);
}

void LightingManager::endClusteredLightingTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ || timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusteredLightingEndQuery);
  timingQueriesWritten_ = true;
}

namespace {
bool editRowKey(const LightEditIdentityState::Row& a,const LightEditIdentityState::Row& b) noexcept {
  return a.id.type==b.id.type&&a.id.source==b.id.source&&a.nodeBirth==b.nodeBirth&&a.sourceNode==b.sourceNode&&
      a.definition==b.definition&&a.extension==b.extension&&
      (a.id.source==EditableLightSource::Imported||a.id.index==b.id.index);
}
auto importedEditRowKey(const LightEditIdentityState::Row& row) noexcept {
  return std::tie(row.nodeBirth,row.sourceNode,row.definition,row.extension,row.id.type);
}
bool editEntity(const EditableLightEntity& a,const EditableLightEntity& b) noexcept {
  return a.id==b.id&&a.type==b.type&&a.source==b.source&&LightEditValuesEqual(LightEditValuesFrom(a),LightEditValuesFrom(b));
}
bool importedLightSourceMatches(const LightEditIdentityState::Row& row,
    const container::scene::SceneGraph& graph,const tinygltf::Model& model) noexcept {
  const auto* graphNode=graph.getNode(row.graphNode);
  if(!graphNode||graphNode->retired||graphNode->sourceNodeIndex!=row.sourceNode||
      graph.nodeIncarnation(row.graphNode)!=row.nodeBirth||row.sourceNode<0||size_t(row.sourceNode)>=model.nodes.size())
    return false;
  const auto& node=model.nodes[size_t(row.sourceNode)];
  if(row.extension.empty()) return node.light==row.definition;
  const auto ext=node.extensions.find(row.extension);
  return ext!=node.extensions.end()&&ext->second.IsObject()&&ext->second.Get("light").IsInt()&&
      ext->second.Get("light").Get<int>()==row.definition;
}
}
void LightingManager::synchronizeLightEditIdentities() const {
  if(editableLights_.size()>kMaxClusteredLights+kMaxAreaLights+1) throw std::length_error("Editable light identities exceed their capacity");
  const auto asset=sceneManager_?sceneManager_->materialAssetIdentity():lightEditOwnerIdentity_;
  const auto scene=sceneManager_?sceneManager_->selectedScene():0;
  uint64_t work{};
  std::vector<uint32_t> sourceNodes;
  const auto makeRow=[&](const EditableLightEntity& light,const LightEditIdentityState::Row* previous=nullptr) {
    LightEditIdentityState::Row row;row.id=light.id;
    if(light.source==EditableLightSource::Imported) {
      if(!sceneManager_) throw std::invalid_argument("Imported light has no current scene owner");
      const auto bindings=light.type==EditableLightType::Directional?sceneManager_->authoredDirectionalLightBindings():
          light.type==EditableLightType::Area?sceneManager_->authoredAreaLightBindings():sceneManager_->authoredPointLightBindings();
      const auto index=light.type==EditableLightType::Directional?0u:light.id.index;
      if(index>=bindings.size()||bindings[index].extension.size()>4096) throw std::invalid_argument("Imported light binding is unavailable");
      row.sourceNode=bindings[index].sourceNode;row.extension=bindings[index].extension;
      const auto& model=std::as_const(*sceneManager_).animationSource();
      if(row.sourceNode<0||size_t(row.sourceNode)>=model.nodes.size()||sceneGraph_.nodeCount()>1'000'000)
        throw std::invalid_argument("Imported light source identity is unavailable");
      if(previous&&previous->sourceNode==row.sourceNode&&previous->extension==row.extension) {
        const auto* node=sceneGraph_.getNode(previous->graphNode);
        if(node&&!node->retired&&node->sourceNodeIndex==row.sourceNode) {
          row.nodeBirth=sceneGraph_.nodeIncarnation(previous->graphNode);row.graphNode=previous->graphNode;
        }
      }
      if(!row.nodeBirth) {
        if(model.nodes.size()>1'000'000) throw std::length_error("Source light identity table exceeds its finite capacity");
        if(sourceNodes.empty()) {
          sourceNodes.assign(model.nodes.size(),container::scene::SceneGraph::kInvalidNode);
          for(uint32_t i=0;i<sceneGraph_.nodeCount();++i) {
            if(++work>64ull*1024*1024) throw std::length_error("Light identity lookup exceeds its work limit");
            const auto* node=sceneGraph_.getNode(i);
            if(node&&!node->retired&&node->sourceNodeIndex>=0&&size_t(node->sourceNodeIndex)<sourceNodes.size()&&
                sourceNodes[size_t(node->sourceNodeIndex)]==container::scene::SceneGraph::kInvalidNode) sourceNodes[size_t(node->sourceNodeIndex)]=i;
          }
        }
        row.graphNode=sourceNodes[size_t(row.sourceNode)];row.nodeBirth=sceneGraph_.nodeIncarnation(row.graphNode);
      }
      const auto& node=model.nodes[size_t(row.sourceNode)];row.definition=node.light;
      if(!row.extension.empty()) {
        const auto ext=node.extensions.find(row.extension);
        if(ext==node.extensions.end()||!ext->second.IsObject()||!ext->second.Get("light").IsInt())
          throw std::invalid_argument("Imported area light association is unavailable");
        row.definition=ext->second.Get("light").Get<int>();
      }
      if(!row.nodeBirth||row.definition<0) throw std::invalid_argument("Imported light source has no original lifetime");
    }
    return row;
  };
  const auto old=lightEditIdentities_;
  const bool sameOwner=old&&old->asset==asset&&old->graph==sceneGraph_.identity()&&old->scene==scene;
  const auto canRetainImported = [&](const LightEditIdentityState::Row& row) {
    return sceneManager_&&row.id.source==EditableLightSource::Imported&&
        importedLightSourceMatches(row,sceneGraph_,std::as_const(*sceneManager_).animationSource())&&
        sceneGraph_.isNodeActive(row.graphNode);
  };
  bool unchanged=sameOwner&&old->generator==generatedLightRevision_&&old->rows.size()==editableLights_.size();
  if(unchanged&&!lightEditIdentitiesValid()) for(size_t i=0;i<editableLights_.size();++i) {
    const auto row=makeRow(editableLights_[i],&old->rows[i]);if(row.id!=old->rows[i].id||!editRowKey(row,old->rows[i])) {unchanged=false;break;}
  }
  if(unchanged&&std::ranges::all_of(old->inactiveImportedRows,canRetainImported)) return;
  auto next=std::make_shared<LightEditIdentityState>();next->asset=asset;next->graph=sceneGraph_.identity();next->scene=scene;next->generator=generatedLightRevision_;
  next->bytes=sizeof(LightEditIdentityState)+256+editableLights_.size()*(sizeof(LightEditIdentityState::Row)+64);
  next->rows.reserve(editableLights_.size());
  for(size_t rowIndex=0;rowIndex<editableLights_.size();++rowIndex) {
    const auto& light=editableLights_[rowIndex];
    const LightEditIdentityState::Row* prior{};
    if(sameOwner&&rowIndex<old->rows.size()&&old->rows[rowIndex].id==light.id) prior=&old->rows[rowIndex];
    if(sameOwner&&!prior) for(const auto& previous:old->rows) {
      if(++work>64ull*1024*1024) throw std::length_error("Light identity matching exceeds its work limit");
      if(previous.id==light.id) {prior=&previous;break;}
    }
    auto row=makeRow(light,prior);
    if(sameOwner&&(light.source!=EditableLightSource::Generated||old->generator==generatedLightRevision_)&&prior&&editRowKey(row,*prior)) row.birth=prior->birth;
    if(!row.birth&&sameOwner&&(light.source!=EditableLightSource::Generated||old->generator==generatedLightRevision_))
      for(const auto& previous:old->rows) {
        if(++work>64ull*1024*1024) throw std::length_error("Light identity matching exceeds its work limit");
        if(editRowKey(row,previous)) {row.birth=previous.birth;break;}
      }
    if(!row.birth&&sameOwner&&light.source==EditableLightSource::Imported) {
      const auto key=importedEditRowKey(row);
      const auto previous=std::ranges::lower_bound(old->inactiveImportedRows,key,{},importedEditRowKey);
      if(previous!=old->inactiveImportedRows.end()&&importedEditRowKey(*previous)==key)
        row.birth=previous->birth;
    }
    if(!row.birth) row.birth=lightIdentity();next->bytes+=row.extension.size()+1;next->rows.push_back(std::move(row));
  }
  if(sameOwner) {
    // Visibility can remove an imported light from the packed runtime table
    // while its source remains available to earlier history commands.
    std::vector<uint64_t> activeImportedBirths;
    for(const auto& row:next->rows)
      if(row.id.source==EditableLightSource::Imported) activeImportedBirths.push_back(row.birth);
    std::ranges::sort(activeImportedBirths);
    const auto retainInactive = [&](const LightEditIdentityState::Row& row) {
      if(!canRetainImported(row)||std::ranges::binary_search(activeImportedBirths,row.birth)) return;
      next->inactiveImportedRows.push_back(row);
      next->bytes+=row.extension.size()+1;
    };
    for(const auto& row:old->inactiveImportedRows) retainInactive(row);
    for(const auto& row:old->rows) retainInactive(row);
    std::ranges::sort(next->inactiveImportedRows,{},importedEditRowKey);
    next->bytes+=next->inactiveImportedRows.capacity()*(sizeof(LightEditIdentityState::Row)+64);
  }
  lightEditIdentities_=std::move(next);
}
bool LightingManager::lightEditIdentitiesValid() const noexcept {
  const auto& set=lightEditIdentities_;if(!set||set->rows.size()!=editableLights_.size()||!sceneManager_) return false;
  const auto& model=std::as_const(*sceneManager_).animationSource();
  for(size_t i=0;i<set->rows.size();++i) {
    const auto& row=set->rows[i];if(row.id!=editableLights_[i].id) return false;
    if(row.id.source!=EditableLightSource::Imported) continue;
    if(!importedLightSourceMatches(row,sceneGraph_,model)) return false;
    const auto bindings=row.id.type==EditableLightType::Directional?sceneManager_->authoredDirectionalLightBindings():
        row.id.type==EditableLightType::Area?sceneManager_->authoredAreaLightBindings():sceneManager_->authoredPointLightBindings();
    const auto index=row.id.type==EditableLightType::Directional?0u:row.id.index;
    if(index>=bindings.size()||bindings[index].sourceNode!=row.sourceNode||bindings[index].extension!=row.extension) return false;
  }
  return true;
}
std::optional<LightEditTarget> LightingManager::currentLightEditTarget(EditableLightId id) const noexcept {
  const auto& set=lightEditIdentities_;if(!set||lightEditRevision_==UINT64_MAX) return {};
  const auto asset=sceneManager_?sceneManager_->materialAssetIdentity():lightEditOwnerIdentity_;
  const auto scene=sceneManager_?sceneManager_->selectedScene():0;
  if(set->asset!=asset||set->graph!=sceneGraph_.identity()||set->scene!=scene||set->generator!=generatedLightRevision_) return {};
  for(const auto& row:set->rows) if(row.id==id)
    return LightEditTarget{lightEditOwnerIdentity_,asset,sceneGraph_.identity(),scene,row.birth,lightEditRevision_,id};
  return {};
}
LightEditSnapshot LightingManager::lightEditSnapshot(EditableLightId id) const {
  LightEditSnapshot result;synchronizeLightEditIdentities();
  const auto row=std::ranges::find_if(editableLights_,[&](const auto& value){return value.id==id;});
  const auto target=currentLightEditTarget(id);
  if(row==editableLights_.end()||!target) {result.reason="The selected light is no longer available";return result;}
  // Bound both the visible row copy and the selected directional override that
  // preparation would otherwise copy next. Rebuilding a visible sun uses a
  // canonical short label even when a retained legacy override has a huge one.
  if(row->label.size()>4096 || (id.type==EditableLightType::Directional && directionalOverride_ &&
      directionalOverride_->label.size()>4096)) {result.reason="Label too long";return result;}
  result.target=*target;result.light=*row;result.available=true;return result;
}
std::optional<EditableLightEntity> LightingManager::lightOverride(EditableLightId id) const {
  if(id.type==EditableLightType::Directional) return directionalOverride_;
  if(id.source==EditableLightSource::Manual) {
    if(id.type==EditableLightType::Area) {
      if(id.index>=manualAreaLights_.size()) throw std::invalid_argument("Manual area light is unavailable");
      return editableAreaLight(id.source,id.index,manualAreaLights_[id.index],true);
    }
    if(id.index>=manualPointLights_.size()) throw std::invalid_argument("Manual point light is unavailable");
    return editablePointLight(id.source,id.index,manualPointLights_[id.index],true);
  }
  if(id.type==EditableLightType::Area) {
    const auto& rows=id.source==EditableLightSource::Imported?importedAreaOverrides_:generatedAreaOverrides_;
    if(id.index<rows.size()&&rows[id.index]) return editableAreaLight(id.source,id.index,*rows[id.index],true);
  } else {
    const auto& rows=id.source==EditableLightSource::Imported?importedPointOverrides_:generatedPointOverrides_;
    if(id.index<rows.size()&&rows[id.index]) return editablePointLight(id.source,id.index,*rows[id.index],true);
  }
  return {};
}
std::optional<LightEditValues> LightingManager::lightOverrideValues(EditableLightId id) const noexcept {
  if(id.type==EditableLightType::Directional) return directionalOverride_?std::optional(LightEditValuesFrom(*directionalOverride_)):std::nullopt;
  LightEditValues v;
  if(id.type==EditableLightType::Area) {
    const AreaLightData* data{};
    if(id.source==EditableLightSource::Manual) {if(id.index<manualAreaLights_.size()) data=&manualAreaLights_[id.index];}
    else {const auto& rows=id.source==EditableLightSource::Imported?importedAreaOverrides_:generatedAreaOverrides_;
      if(id.index<rows.size()&&rows[id.index]) data=&*rows[id.index];}
    if(!data) return {};
    v.position=glm::vec3(data->positionRange);v.range=std::max(data->positionRange.w,0.f);
    v.color=glm::max(glm::vec3(data->colorIntensity),glm::vec3(0));v.intensity=std::max(data->colorIntensity.w,0.f);
    v.direction=normalizeOr(glm::vec3(data->directionType),glm::vec3(0,-1,0));
    const auto up=std::abs(v.direction.y)<.95f?glm::vec3(0,1,0):glm::vec3(1,0,0);
    v.tangent=normalizeOr(glm::vec3(data->tangentHalfSize),normalizeOr(glm::cross(up,v.direction),glm::vec3(1,0,0)));
    v.bitangent=normalizeOr(glm::vec3(data->bitangentHalfSize),normalizeOr(glm::cross(v.direction,v.tangent),glm::vec3(0,0,1)));
    v.areaHalfSize={std::max(data->tangentHalfSize.w,.001f),std::max(data->bitangentHalfSize.w,.001f)};
    v.areaShape=data->directionType.w>=container::gpu::kAreaLightTypeDisk-.5f?container::gpu::kAreaLightTypeDisk:container::gpu::kAreaLightTypeRectangle;
  } else {
    const PointLightData* data{};
    if(id.source==EditableLightSource::Manual) {if(id.index<manualPointLights_.size()) data=&manualPointLights_[id.index];}
    else {const auto& rows=id.source==EditableLightSource::Imported?importedPointOverrides_:generatedPointOverrides_;
      if(id.index<rows.size()&&rows[id.index]) data=&*rows[id.index];}
    if(!data) return {};
    v.position=glm::vec3(data->positionRadius);v.range=std::max(data->positionRadius.w,0.f);
    v.color=glm::max(glm::vec3(data->colorIntensity),glm::vec3(0));v.intensity=std::max(data->colorIntensity.w,0.f);
    v.direction=normalizeOr(glm::vec3(data->directionInnerCos),glm::vec3(0,-1,0));
    if(id.type==EditableLightType::Spot) {v.innerConeDegrees=glm::degrees(std::acos(std::clamp(data->directionInnerCos.w,-1.f,1.f)));
      v.outerConeDegrees=glm::degrees(std::acos(std::clamp(data->coneOuterCosType.x,-1.f,1.f)));}
  }
  return v;
}
bool LightingManager::lightEditCommandMatches(const LightEditCommand& command) const noexcept {
  if(!command||!command.published()||!command.applied()||!lightEditIdentitiesValid()) return false;
  for(const auto& row:lightEditIdentities_->rows) if(row.birth==command.target().incarnation) {
    const auto target=currentLightEditTarget(row.id);
    return target&&LightEditCommandPreviewMatches(command,*target,lightOverrideValues(row.id));
  }
  return false;
}
LightEditCanonicalPayload LightingManager::lightRawOverride(EditableLightId id) const noexcept {
  LightEditCanonicalPayload result;
  if(id.type==EditableLightType::Directional) return result;
  if(id.type==EditableLightType::Area) {
    if(id.source==EditableLightSource::Manual) {if(id.index<manualAreaLights_.size()) {result.kind=LightEditCanonicalPayload::Kind::Area;result.area=manualAreaLights_[id.index];}}
    else {const auto& rows=id.source==EditableLightSource::Imported?importedAreaOverrides_:generatedAreaOverrides_;
      if(id.index<rows.size()&&rows[id.index]) {result.kind=LightEditCanonicalPayload::Kind::Area;result.area=*rows[id.index];}}
  } else {
    if(id.source==EditableLightSource::Manual) {if(id.index<manualPointLights_.size()) {result.kind=LightEditCanonicalPayload::Kind::Point;result.point=manualPointLights_[id.index];}}
    else {const auto& rows=id.source==EditableLightSource::Imported?importedPointOverrides_:generatedPointOverrides_;
      if(id.index<rows.size()&&rows[id.index]) {result.kind=LightEditCanonicalPayload::Kind::Point;result.point=*rows[id.index];}}
  }
  return result;
}
EditableLightEntity LightingManager::lightBase(EditableLightId id,LightEditCanonicalPayload* raw) const {
  if(id.source==EditableLightSource::Manual) {if(raw) *raw=lightRawOverride(id);return *lightOverride(id);}
  LightingManager base(device_,allocationManager_,pipelineManager_,sceneManager_,sceneGraph_,world_,DetachedGraphTag{});
  base.lightingSettings_=lightingSettings_;base.rootNode_=rootNode_;base.suppressWorldPublication_=true;
  base.updateLightingData();
  if(raw&&id.type==EditableLightType::Area&&id.index<base.areaLightsSsbo_.size()) {raw->kind=LightEditCanonicalPayload::Kind::Area;raw->area=base.areaLightsSsbo_[id.index];}
  else if(raw&&id.type!=EditableLightType::Directional&&id.index<base.pointLightsSsbo_.size()) {raw->kind=LightEditCanonicalPayload::Kind::Point;raw->point=base.pointLightsSsbo_[id.index];}
  const auto row=std::ranges::find_if(base.editableLights_,[&](const auto& light){return light.id==id;});
  if(row==base.editableLights_.end()) throw std::invalid_argument("The original source light is no longer active");return *row;
}
struct LightingManager::PreparedLightEdit::State {
  const LightingManager* owner{};uint64_t revision{},epoch{},graph{},graphRevision{},asset{},scene{},sourceEpoch{},geometry{},material{},camera{};
  std::shared_ptr<const LightEditIdentityState> identities;
  LightEditTarget target;PreparedLightEditPlan plan;PreparedGeneration generation;
  container::ecs::World::PreparedLightValues world;
  std::unique_ptr<container::ecs::World> beforeWorld,nextWorld;
  std::shared_ptr<const LightEditAddState> addition;
  SceneLightEdits before,after;container::scene::AuthoredSceneLights source;
  container::scene::ModelBounds bounds;LightingSettings settings;LightingData data;
  glm::mat4 rootLocal{1},rootWorld{1};uint32_t root{};
  EditableLightEntity displayed,base;std::optional<EditableLightEntity> originalOverride;
  EditableLightId selectedBefore;
  std::optional<tinygltf::Light> rawLight;
  std::optional<tinygltf::Value> rawArea;
  int32_t rawDefinition{-1};std::string rawExtension;
  container::geometry::gltf::GltfCameraPoseAuthoringLimits metadataLimits;
  uint64_t bytes{},gpuBytes{};bool consumed{},structuralWorld{};
};
LightingManager::PreparedLightEdit::PreparedLightEdit() noexcept=default;
LightingManager::PreparedLightEdit::~PreparedLightEdit()=default;
LightingManager::PreparedLightEdit::PreparedLightEdit(PreparedLightEdit&&) noexcept=default;
LightingManager::PreparedLightEdit& LightingManager::PreparedLightEdit::operator=(PreparedLightEdit&&) noexcept=default;
LightingManager::PreparedLightEdit::operator bool() const noexcept {return state_&&!state_->consumed;}
bool LightingManager::PreparedLightEdit::changed() const noexcept {return state_&&state_->plan.changed();}
uint64_t LightingManager::PreparedLightEdit::ownedBytes() const noexcept {return state_?state_->bytes:0;}
uint64_t LightingManager::PreparedLightEdit::ownedGpuBytes() const noexcept {return state_?state_->gpuBytes:0;}
LightingManager::LightEditCommand LightingManager::PreparedLightEdit::command() const noexcept {return state_?state_->plan.command():LightEditCommand{};}
LightEditTarget LightingManager::PreparedLightEdit::target() const noexcept {return state_?state_->target:LightEditTarget{};}
const SceneLightEdits& LightingManager::PreparedLightEdit::edits() const {if(!*this) throw std::logic_error("Empty light edit");return state_->after;}
const SceneLightEdits& LightingManager::PreparedLightEdit::beforeEdits() const {if(!*this) throw std::logic_error("Empty light edit");return state_->before;}
const EditableLightEntity& LightingManager::PreparedLightEdit::selectedLight() const {if(!*this) throw std::logic_error("Empty light edit");return state_->plan.selectedAfter();}
const LightingData& LightingManager::PreparedLightEdit::lightingData() const {if(!*this) throw std::logic_error("Empty light edit");return state_->generation.lightingData();}
const std::vector<PointLightData>& LightingManager::PreparedLightEdit::pointLightsSsbo() const {if(!*this) throw std::logic_error("Empty light edit");return state_->generation.pointLightsSsbo();}
const std::vector<AreaLightData>& LightingManager::PreparedLightEdit::areaLightsSsbo() const {if(!*this) throw std::logic_error("Empty light edit");return state_->generation.areaLightsSsbo();}
std::span<const container::gpu::AllocatedBuffer> LightingManager::PreparedLightEdit::lightingBuffers() const noexcept {return *this?state_->generation.lightingBuffers():std::span<const container::gpu::AllocatedBuffer>{};}
const container::gpu::AllocatedBuffer& LightingManager::PreparedLightEdit::pointLightBuffer(uint32_t image) const {
  if (!*this) throw std::logic_error("Empty light edit");
  return state_->generation.pointLightBuffer(image);
}
const container::gpu::AllocatedBuffer& LightingManager::PreparedLightEdit::areaLightBuffer(uint32_t image) const {
  if (!*this) throw std::logic_error("Empty light edit");
  return state_->generation.areaLightBuffer(image);
}
std::span<const container::gpu::AllocatedBuffer>
LightingManager::PreparedLightEdit::pointLightBuffers() const noexcept {
  return *this ? state_->generation.pointLightBuffers() : std::span<const container::gpu::AllocatedBuffer>{};
}
std::span<const container::gpu::AllocatedBuffer>
LightingManager::PreparedLightEdit::areaLightBuffers() const noexcept {
  return *this ? state_->generation.areaLightBuffers() : std::span<const container::gpu::AllocatedBuffer>{};
}
VkDescriptorSet LightingManager::PreparedLightEdit::lightDescriptorSet(uint32_t i) const noexcept {return *this?state_->generation.lightDescriptorSet(i):VK_NULL_HANDLE;}
VkDescriptorSet LightingManager::PreparedLightEdit::tiledDescriptorSet(uint32_t image) const noexcept {
  return *this ? state_->generation.tiledDescriptorSet(image) : VK_NULL_HANDLE;
}
void LightingManager::PreparedLightEdit::updateRayShadowDescriptors(uint32_t i,VkBuffer b,VkDeviceSize n,VkImageView v) {if(!*this) throw std::logic_error("Empty light edit");state_->generation.updateRayShadowDescriptors(i,b,n,v);}
LightingManager::PreparedLightEdit LightingManager::prepareLightEdit(const LightEditTarget& target,const LightEditPatch& patch,
    const LightEditCommand* gesture,const LightEditLimits& limits) const {
  return prepareLightEditImpl(target,&patch,gesture,false,true,limits);
}
LightingManager::PreparedLightEdit LightingManager::prepareLightEditReplay(const LightEditCommand& command,bool applied,const LightEditLimits& limits) const {
  if(const auto addition=LightEditAddition(command)) return prepareLightAddImpl(addition->type,&command,applied,limits);
  synchronizeLightEditIdentities();
  for(const auto& row:lightEditIdentities_->rows) if(row.birth==command.target().incarnation) {
    const auto target=currentLightEditTarget(row.id);if(target) return prepareLightEditImpl(*target,nullptr,&command,true,applied,limits);
  }
  throw std::invalid_argument("The original light incarnation no longer exists");
}
LightingManager::PreparedLightEdit LightingManager::prepareLightEditImpl(const LightEditTarget& target,const LightEditPatch* patch,
    const LightEditCommand* original,bool replay,bool applied,const LightEditLimits& limits) const {
  if(!sceneManager_||!limits.maxPreparedCpuBytes||limits.maxPreparedCpuBytes==UINT64_MAX||!limits.maxPreparedGpuBytes||
      limits.maxPreparedGpuBytes==UINT64_MAX||lightEditRevision_==UINT64_MAX) throw std::invalid_argument("Light edit needs a current source and finite budgets");
  if(limits.maxPreparedCpuBytes<sizeof(PreparedLightEdit::State)+4096+
      editableLights_.size()*(sizeof(LightEditIdentityState::Row)+64)) throw std::invalid_argument("Light edit identity admission exceeds its CPU budget");
  const auto snapshot=lightEditSnapshot(target.id);if(!snapshot.available||snapshot.target!=target) throw std::invalid_argument("The selected light request is stale");
  if (patch && snapshot.light.iesProfileAddress &&
      (patch->fields & (LightEditFieldBit(LightEditField::Position) | LightEditFieldBit(LightEditField::Range))))
    throw std::invalid_argument("Edit the IES fixture transform through its source node. Its range is unbounded.");
  LightingGraphPoseGenerationLimits capacity;capacity.maxPreparedCpuBytes=limits.maxPreparedCpuBytes;capacity.maxPreparedGpuBytes=limits.maxPreparedGpuBytes;
  PoseLightBudget budget{capacity};budget.add(sizeof(PreparedLightEdit::State)+4096);
  budget.add(pointLightsSsbo_.size(),sizeof(PointLightData)+sizeof(EditableLightEntity)+256);
  budget.add(areaLightsSsbo_.size(),sizeof(AreaLightData)+sizeof(EditableLightEntity)+256);budget.add(4096);
  budget.add(sceneLightEditsStorageBytes(),3);budget.lights(sceneManager_->authoredPointLights(),sceneManager_->authoredDirectionalLights(),sceneManager_->authoredAreaLights(),
      sceneManager_->authoredPointLightBindings(),sceneManager_->authoredDirectionalLightBindings(),sceneManager_->authoredAreaLightBindings());
  auto result=PreparedLightEdit{};result.state_=std::make_unique<PreparedLightEdit::State>();auto& s=*result.state_;
  s.owner=this;s.revision=lightEditRevision_;s.epoch=descriptorBindingEpoch_;s.graph=sceneGraph_.identity();s.graphRevision=sceneGraph_.revision();
  s.selectedBefore=selectedEditableLight_;
  s.asset=sceneManager_->materialAssetIdentity();s.scene=sceneManager_->selectedScene();s.sourceEpoch=sceneManager_->generationEpoch();
  s.geometry=sceneManager_->geometryRevision();s.material=sceneManager_->materialRevision();s.camera=sceneManager_->cameraRevision();
  LightEditCanonicalPayload raw;
  s.identities=lightEditIdentities_;s.target=target;s.displayed=snapshot.light;s.base=lightBase(target.id,&raw);s.originalOverride=lightOverride(target.id);
  if(s.originalOverride&&target.id.type!=EditableLightType::Directional) raw=lightRawOverride(target.id);
  s.plan=replay?RebaseLightEdit(target,s.displayed,s.base,s.originalOverride,*original,applied,limits,&raw):
      PlanLightEdit(target,s.displayed,s.base,s.originalOverride,*patch,original,limits,&raw);
  budget.add(s.plan.ownedBytes());s.before=snapshotSceneLightEdits();s.after=s.before;s.source=sceneManager_->publishedAuthoredSceneLights();
  s.bounds=sceneManager_->modelBounds();s.settings=lightingSettings_;s.data=lightingData_;s.root=rootNode_;
  if(target.id.source==EditableLightSource::Imported) {
    const auto row=std::ranges::find_if(s.identities->rows,[&](const auto& r){return r.birth==target.incarnation;});
    if(row==s.identities->rows.end()) throw std::invalid_argument("Light source identity changed");
    const auto& model=std::as_const(*sceneManager_).animationSource();s.rawDefinition=row->definition;s.rawExtension=row->extension;
    s.metadataLimits.maxOwnedBytes=limits.maxPreparedCpuBytes-budget.cpu;
    if(row->extension.empty()) {
      if(size_t(row->definition)>=model.lights.size()) throw std::invalid_argument("Light source definition is unavailable");
      budget.add(container::geometry::gltf::GltfCameraPoseLightStorageBytes(model.lights[size_t(row->definition)],s.metadataLimits));
      s.rawLight.emplace();*s.rawLight=model.lights[size_t(row->definition)];
    } else {
      const auto ext=model.extensions.find(row->extension);
      if(ext==model.extensions.end()||!ext->second.IsObject()||!ext->second.Get("lights").IsArray()||
          size_t(row->definition)>=ext->second.Get("lights").ArrayLen()) throw std::invalid_argument("Area light source definition is unavailable");
      const auto& raw=ext->second.Get("lights").Get(size_t(row->definition));
      budget.add(container::geometry::gltf::GltfAuthoringValueStorageBytes(raw,s.metadataLimits));s.rawArea.emplace(raw);
    }
  }
  if(const auto* root=sceneGraph_.getNode(rootNode_)) {s.rootLocal=root->localTransform;s.rootWorld=root->worldTransform;}
  const auto& next=s.plan.overrideAfter();const auto id=target.id;
  if(!s.plan.changed()) {s.bytes=budget.cpu;if(!canExchangeLightEdit(result)) throw std::invalid_argument("No-op light preparation became stale");return result;}
  if(id.type==EditableLightType::Directional) s.after.directionalOverride=next;
  else if(id.type==EditableLightType::Area) {
    if(id.source==EditableLightSource::Manual) s.after.manualAreaLights.at(id.index)=s.plan.canonicalAfter().area;
    else {auto& rows=id.source==EditableLightSource::Imported?s.after.importedAreaOverrides:s.after.generatedAreaOverrides;
      if(rows.size()<=id.index) rows.resize(size_t(id.index)+1);rows[id.index]=next?std::optional(s.plan.canonicalAfter().area):std::nullopt;}
  } else {
    if(id.source==EditableLightSource::Manual) s.after.manualPointLights.at(id.index)=s.plan.canonicalAfter().point;
    else {auto& rows=id.source==EditableLightSource::Imported?s.after.importedPointOverrides:s.after.generatedPointOverrides;
      if(rows.size()<=id.index) rows.resize(size_t(id.index)+1);rows[id.index]=next?std::optional(s.plan.canonicalAfter().point):std::nullopt;}
  }
  auto generationCapacity=capacity;generationCapacity.maxPreparedCpuBytes=limits.maxPreparedCpuBytes-budget.cpu;
  s.generation=prepareGenerationImpl(*sceneManager_,sceneGraph_,world_,rootNode_,s.after,uint32_t(lightingBuffers_.size()),&s.source,&s.bounds,&generationCapacity,true);
  s.generation.state_->payload->selectEditableLight(target.id);
  budget.add(s.generation.ownedBytes());container::ecs::World::LightValueLimits worldLimits;
  worldLimits.maxOwnedBytes=limits.maxPreparedCpuBytes-budget.cpu;worldLimits.maxWork=limits.maxWork;
  s.world=world_.prepareLightValues(pointLightsSsbo_,s.generation.pointLightsSsbo(),worldLimits);budget.add(s.world.ownedBytes());
  s.bytes=budget.cpu;s.gpuBytes=s.generation.ownedGpuBytes();
  if(!canExchangeLightEdit(result)) throw std::invalid_argument("Light source or selected override changed during preparation");return result;
}
LightingManager::PreparedLightEdit LightingManager::prepareLightAdd(EditableLightType type,const LightEditLimits& limits) const {
  return prepareLightAddImpl(type,nullptr,true,limits);
}
LightingManager::PreparedLightEdit LightingManager::prepareLightAddImpl(EditableLightType type,const LightEditCommand* original,
    bool applied,const LightEditLimits& limits) const {
  if(!sceneManager_||!limits.maxPreparedCpuBytes||limits.maxPreparedCpuBytes==UINT64_MAX||!limits.maxPreparedGpuBytes||
      limits.maxPreparedGpuBytes==UINT64_MAX||lightEditRevision_==UINT64_MAX||
      (type!=EditableLightType::Directional&&type!=EditableLightType::Point&&type!=EditableLightType::Spot&&type!=EditableLightType::Area))
    throw std::invalid_argument("Manual light creation needs current ownership and finite limits");
  if(limits.maxPreparedCpuBytes<sizeof(PreparedLightEdit::State)+4096+
      editableLights_.size()*(sizeof(LightEditIdentityState::Row)+64)) throw std::invalid_argument("Manual light identity admission exceeds its CPU budget");
  synchronizeLightEditIdentities();
  LightingGraphPoseGenerationLimits capacity;capacity.maxPreparedCpuBytes=limits.maxPreparedCpuBytes;capacity.maxPreparedGpuBytes=limits.maxPreparedGpuBytes;
  PoseLightBudget budget{capacity};budget.add(sizeof(PreparedLightEdit::State)+sizeof(LightEditAddState)+8192);
  budget.add(pointLightsSsbo_.size()+1,3*(sizeof(PointLightData)+sizeof(EditableLightEntity)+256));
  budget.add(areaLightsSsbo_.size()+1,3*(sizeof(AreaLightData)+sizeof(EditableLightEntity)+256));budget.add(8192);
  budget.add(sceneLightEditsStorageBytes(),4);budget.lights(sceneManager_->authoredPointLights(),sceneManager_->authoredDirectionalLights(),sceneManager_->authoredAreaLights(),
      sceneManager_->authoredPointLightBindings(),sceneManager_->authoredDirectionalLightBindings(),sceneManager_->authoredAreaLightBindings());
  const bool structuralWorld=type==EditableLightType::Point||type==EditableLightType::Spot;
  if(structuralWorld) {
    const auto worldBytes=world_.stateStorageBytes(limits.maxPreparedCpuBytes-budget.cpu,limits.maxWork);
    budget.add(worldBytes,2);
  }
  // Rare structural staging may add one entity and component sparse pages;
  // routine property/gizmo edits never enter this clone path.
  if(structuralWorld) budget.add(2ull*1024*1024);
  budget.add(pointLightsSsbo_.size()+1,4*sizeof(PointLightData)+128);
  PreparedLightEdit result;result.state_=std::make_unique<PreparedLightEdit::State>();auto& s=*result.state_;
  s.structuralWorld=structuralWorld;
  s.owner=this;s.revision=lightEditRevision_;s.epoch=descriptorBindingEpoch_;s.graph=sceneGraph_.identity();s.graphRevision=sceneGraph_.revision();
  s.selectedBefore=selectedEditableLight_;
  s.asset=sceneManager_->materialAssetIdentity();s.scene=sceneManager_->selectedScene();s.sourceEpoch=sceneManager_->generationEpoch();
  s.geometry=sceneManager_->geometryRevision();s.material=sceneManager_->materialRevision();s.camera=sceneManager_->cameraRevision();
  s.identities=lightEditIdentities_;s.before=snapshotSceneLightEdits();s.after=s.before;
  s.source=sceneManager_->publishedAuthoredSceneLights();s.bounds=sceneManager_->modelBounds();s.settings=lightingSettings_;s.data=lightingData_;s.root=rootNode_;
  if(const auto* root=sceneGraph_.getNode(rootNode_)) {s.rootLocal=root->localTransform;s.rootWorld=root->worldTransform;}
  const auto birth=[&](EditableLightType rowType,size_t index) -> uint64_t {
    for(const auto& row:s.identities->rows) if(row.id.source==EditableLightSource::Manual&&row.id.index==index&&
        ((rowType==EditableLightType::Area)==(row.id.type==EditableLightType::Area))&&row.id.type!=EditableLightType::Directional) return row.birth;
    return 0;
  };
  std::shared_ptr<const LightEditAddState> authority;
  if(original) {
    authority=LightEditAddition(*original);
    if(!authority||authority->owner!=lightEditOwnerIdentity_||authority->asset!=s.asset||authority->graph!=s.graph||authority->scene!=s.scene||authority->type!=type)
      throw std::invalid_argument("Manual light creation belongs to another source generation");
    const size_t expectedPoints=authority->points+(!applied&&(type==EditableLightType::Point||type==EditableLightType::Spot)?1u:0u);
    const size_t expectedAreas=authority->areas+(!applied&&type==EditableLightType::Area?1u:0u);
    if(manualPointLights_.size()!=expectedPoints||manualAreaLights_.size()!=expectedAreas||
        (authority->points&&birth(EditableLightType::Point,authority->points-1)!=authority->pointPrefixBirth)||
        (authority->areas&&birth(EditableLightType::Area,authority->areas-1)!=authority->areaPrefixBirth))
      throw std::invalid_argument("Original manual light creation no longer owns its exact tail prefix");
    if(!applied) {
      const auto target=currentLightEditTarget(authority->added.id);
      if(!target||target->incarnation!=authority->birth||
          (type==EditableLightType::Area&&!posePayload(manualAreaLights_.back(),authority->area))||
          ((type==EditableLightType::Point||type==EditableLightType::Spot)&&!posePayload(manualPointLights_.back(),authority->point))||
          (type==EditableLightType::Directional&&!poseEditable(directionalOverride_,std::optional(authority->added))))
        throw std::invalid_argument("The originally created manual light has changed; undo its edits first");
    } else if(type==EditableLightType::Directional) {
      if(!poseEditable(directionalOverride_,authority->beforeDirectional)) throw std::invalid_argument("The replaced directional override changed");
      if(authority->beforeDirectional&&authority->beforeDirectionalIdentity) {
        const auto old=std::ranges::find_if(s.identities->rows,[&](const auto& row){return row.id.type==EditableLightType::Directional;});
        if(old==s.identities->rows.end()||old->birth!=authority->beforeDirectionalIdentity->birth||
            (old->id.source==EditableLightSource::Generated&&authority->generator!=generatedLightRevision_))
          throw std::invalid_argument("The original directional source lifetime changed");
      }
    }
  } else {
    auto created=std::make_shared<LightEditAddState>();created->owner=lightEditOwnerIdentity_;created->asset=s.asset;created->graph=s.graph;created->scene=s.scene;
    created->type=type;created->points=manualPointLights_.size();created->areas=manualAreaLights_.size();created->birth=lightIdentity();created->generator=generatedLightRevision_;
    if(created->points) {created->pointPrefixBirth=birth(EditableLightType::Point,created->points-1);if(!created->pointPrefixBirth) throw std::invalid_argument("Manual prefix exceeds the active renderer capacity");}
    if(created->areas) {created->areaPrefixBirth=birth(EditableLightType::Area,created->areas-1);if(!created->areaPrefixBirth) throw std::invalid_argument("Manual area prefix exceeds the active renderer capacity");}
    if(const auto target=currentLightEditTarget(selectedEditableLight_)) created->beforeSelectionBirth=target->incarnation;
    created->beforeDirectional=directionalOverride_;
    if(type==EditableLightType::Directional) for(const auto& row:s.identities->rows)
      if(row.id.type==EditableLightType::Directional) {created->beforeDirectionalIdentity=row;break;}
    LightingManager defaults(device_,allocationManager_,pipelineManager_,sceneManager_,sceneGraph_,world_,DetachedGraphTag{});
    defaults.lightingSettings_=lightingSettings_;defaults.rootNode_=rootNode_;defaults.suppressWorldPublication_=true;defaults.restoreSceneLightEdits(s.before);
    defaults.lightingData_=lightingData_;
    const auto id=defaults.addManualEditableLight(type);defaults.updateLightingData();defaults.selectEditableLight(id);
    const auto selected=defaults.selectedEditableLight();if(!selected) throw std::invalid_argument("The new light exceeds the active renderer capacity");
    created->added=*selected;
    if(type==EditableLightType::Area) created->area=defaults.manualAreaLights_.at(id.index);
    else if(type!=EditableLightType::Directional) created->point=defaults.manualPointLights_.at(id.index);
    authority=std::move(created);
  }
  s.addition=authority;s.target={lightEditOwnerIdentity_,s.asset,s.graph,s.scene,authority->birth,s.revision,authority->added.id};
  if(type==EditableLightType::Directional) s.after.directionalOverride=applied?std::optional(authority->added):authority->beforeDirectional;
  else if(type==EditableLightType::Area) {if(applied) s.after.manualAreaLights.push_back(authority->area);else s.after.manualAreaLights.pop_back();}
  else {if(applied) s.after.manualPointLights.push_back(authority->point);else s.after.manualPointLights.pop_back();}
  LightingManager prospective(device_,allocationManager_,pipelineManager_,sceneManager_,sceneGraph_,world_,DetachedGraphTag{});
  prospective.lightingSettings_=lightingSettings_;prospective.rootNode_=rootNode_;prospective.suppressWorldPublication_=true;
  prospective.restoreSceneLightEdits(s.after);prospective.updateLightingData();prospective.synchronizeLightEditIdentities();
  budget.add(prospective.lightEditIdentities_->bytes);
  auto identities=std::make_shared<LightEditIdentityState>(*prospective.lightEditIdentities_);
  EditableLightEntity selected;
  if(applied) {
    auto row=std::ranges::find_if(identities->rows,[&](const auto& r){return r.id==authority->added.id;});
    if(row==identities->rows.end()) throw std::invalid_argument("Created manual light is not renderer-visible");row->birth=authority->birth;
    prospective.selectEditableLight(authority->added.id);selected=*prospective.selectedEditableLight();
  } else {
    if(type==EditableLightType::Directional&&authority->beforeDirectionalIdentity) {
      auto row=std::ranges::find_if(identities->rows,[](const auto& r){return r.id.type==EditableLightType::Directional;});
      const bool sameSource=row!=identities->rows.end()&&editRowKey(*row,*authority->beforeDirectionalIdentity)&&
          (row->id.source!=EditableLightSource::Generated||authority->generator==generatedLightRevision_);
      if(authority->beforeDirectional&&!sameSource)
        throw std::invalid_argument("Original directional binding cannot be restored to another light");
      // Absence reveals today's source. Reuse the old birth only while its
      // exact binding/regeneration capability survives; never revive an older
      // generated incarnation merely because a manual sun masked it.
      if(sameSource) row->birth=authority->beforeDirectionalIdentity->birth;
    }
    for(const auto& row:identities->rows) if(row.birth==authority->beforeSelectionBirth) {
      prospective.selectEditableLight(row.id);if(const auto value=prospective.selectedEditableLight()) selected=*value;break;
    }
  }
  s.after.identities=std::move(identities);
  std::optional<EditableLightEntity> beforeConcept,afterConcept;
  if(type==EditableLightType::Directional) {beforeConcept=s.before.directionalOverride;afterConcept=s.after.directionalOverride;}
  else {if(!applied) beforeConcept=authority->added;if(applied) afterConcept=authority->added;}
  s.plan=PlanLightEditStructure(s.target,beforeConcept,afterConcept,selected,original,applied,authority,limits);
  budget.add(s.plan.ownedBytes());s.displayed=selected;s.displayed.id=s.target.id;s.displayed.type=type;s.displayed.source=EditableLightSource::Manual;
  s.base=s.displayed;s.originalOverride=beforeConcept;
  container::ecs::World::LightValueLimits worldLimits;worldLimits.maxOwnedBytes=limits.maxPreparedCpuBytes-budget.cpu;worldLimits.maxWork=limits.maxWork;
  s.world=world_.prepareLightValues(pointLightsSsbo_,pointLightsSsbo_,worldLimits);budget.add(s.world.ownedBytes());
  if(structuralWorld) {s.beforeWorld=world_.cloneStateWithoutContext();s.nextWorld=world_.cloneStateWithoutContext();}
  auto generationCapacity=capacity;generationCapacity.maxPreparedCpuBytes=limits.maxPreparedCpuBytes-budget.cpu;
  s.generation=prepareGenerationImpl(*sceneManager_,sceneGraph_,world_,rootNode_,s.after,uint32_t(lightingBuffers_.size()),&s.source,&s.bounds,&generationCapacity,true);
  budget.add(s.generation.ownedBytes());
  if(structuralWorld) s.nextWorld->replacePointLights(s.generation.pointLightsSsbo());
  else if(!poseArray(pointLightsSsbo_,s.generation.pointLightsSsbo())) throw std::invalid_argument("Area/sun creation changed unrelated point lighting");
  s.generation.state_->payload->selectEditableLight(selected.id);
  s.bytes=budget.cpu;s.gpuBytes=s.generation.ownedGpuBytes();
  if(!canExchangeLightEdit(result)) throw std::invalid_argument("Manual light creation became stale during preparation");return result;
}
bool LightingManager::canExchangeLightEdit(const PreparedLightEdit& prepared) const noexcept {
  if(!prepared) return false;const auto& s=*prepared.state_;const auto target=currentLightEditTarget(s.target.id);
  if(s.owner!=this||s.revision!=lightEditRevision_||lightEditRevision_==UINT64_MAX||s.identities!=lightEditIdentities_||
      s.selectedBefore!=selectedEditableLight_||
      (!s.addition&&(!target||*target!=s.target))||
      s.epoch!=descriptorBindingEpoch_||s.graph!=sceneGraph_.identity()||s.graphRevision!=sceneGraph_.revision()||!sceneManager_||
      s.asset!=sceneManager_->materialAssetIdentity()||s.scene!=sceneManager_->selectedScene()||s.sourceEpoch!=sceneManager_->generationEpoch()||
      s.geometry!=sceneManager_->geometryRevision()||s.material!=sceneManager_->materialRevision()||s.camera!=sceneManager_->cameraRevision()||
      s.root!=rootNode_||!poseSettings(s.settings,lightingSettings_)||!poseData(s.data,lightingData_)||!poseLights(*sceneManager_,s.source)||
      !poseBounds(s.bounds,sceneManager_->modelBounds())||!poseEditable(s.before.directionalOverride,directionalOverride_)||
      !poseArray(s.before.generatedPointOverrides,generatedPointOverrides_)||!poseArray(s.before.importedPointOverrides,importedPointOverrides_)||
      !poseArray(s.before.generatedAreaOverrides,generatedAreaOverrides_)||!poseArray(s.before.importedAreaOverrides,importedAreaOverrides_)||
      !poseArray(s.before.manualPointLights,manualPointLights_)||!poseArray(s.before.manualAreaLights,manualAreaLights_)||
      !lightEditIdentitiesValid()||!LightEditPlanMatches(s.plan,s.target,s.displayed,s.base,s.originalOverride)||
      (s.plan.changed()&&(!generationResourcesReady(s.generation)||!world_.canExchangeLightValues(s.world)))||
      (s.structuralWorld&&(!s.beforeWorld||!s.nextWorld||!world_.stateValuesMatch(*s.beforeWorld)))) return false;
  const auto& model=std::as_const(*sceneManager_).animationSource();
  if(s.rawLight&&(s.rawDefinition<0||size_t(s.rawDefinition)>=model.lights.size()||
      !container::geometry::gltf::GltfCameraPoseLightRecordsMatch(model.lights[size_t(s.rawDefinition)],*s.rawLight,s.metadataLimits))) return false;
  if(s.rawArea) {
    const auto ext=model.extensions.find(s.rawExtension);
    if(ext==model.extensions.end()||!ext->second.IsObject()||!ext->second.Get("lights").IsArray()||s.rawDefinition<0||
        size_t(s.rawDefinition)>=ext->second.Get("lights").ArrayLen()||
        !container::geometry::gltf::GltfCameraPoseValuesMatch(ext->second.Get("lights").Get(size_t(s.rawDefinition)),*s.rawArea,s.metadataLimits)) return false;
  }
  const auto row=std::ranges::find_if(editableLights_,[&](const auto& light){return light.id==s.target.id;});
  if(!s.addition&&(row==editableLights_.end()||!editEntity(*row,s.displayed))) return false;
  const auto* root=sceneGraph_.getNode(s.root);
  return s.root==container::scene::SceneGraph::kInvalidNode||(root&&poseMatrix(root->localTransform,s.rootLocal)&&poseMatrix(root->worldTransform,s.rootWorld));
}
bool LightingManager::exchangeLightEdit(PreparedLightEdit& prepared) noexcept {
  if(!canExchangeLightEdit(prepared)) return false;auto& s=*prepared.state_;
  if(s.plan.changed()) {
    if(s.structuralWorld) world_.swapStatePreservingContext(*s.nextWorld);
    else if(!world_.exchangeLightValues(s.world)) return false;
    exchangeGenerationPayload(s.generation);++lightEditRevision_;
  }
  if(!PublishLightEditPlan(s.plan,lightEditRevision_)) std::terminate();s.consumed=true;return true;
}

} // namespace container::renderer
