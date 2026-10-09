#include "Container/renderer/lighting/LightingManager.h"
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
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <span>
#include <stdexcept>
#include <vector>

namespace container::renderer {

using container::gpu::AreaLightData;
using container::gpu::kClusterDepthSlices;
using container::gpu::kMaxAreaLights;
using container::gpu::kMaxClusteredLights;
using container::gpu::kMaxLightsPerTile;
using container::gpu::kMaxShadowedLocalLightLayers;
using container::gpu::kTileSize;
using container::gpu::LightCullingStats;
using container::gpu::LightingData;
using container::gpu::LightingSettings;
using container::gpu::PointLightData;
using container::gpu::TileCullPushConstants;
using container::gpu::TileLightGrid;

namespace {

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

} // namespace

LightingManager::LightingManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager &allocationManager,
    container::gpu::PipelineManager &pipelineManager,
    container::scene::SceneManager *sceneManager,
    container::scene::SceneGraph &sceneGraph, container::ecs::World &world)
    : device_(std::move(device)), allocationManager_(allocationManager),
      pipelineManager_(pipelineManager), sceneManager_(sceneManager),
      sceneGraph_(sceneGraph), world_(world) {}

LightingManager::~LightingManager() {
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
  allocationManager_.destroyBuffer(areaLightSsbo_);
  allocationManager_.destroyBuffer(lightSsbo_);
  for (auto &lightingBuffer : lightingBuffers_) {
    allocationManager_.destroyBuffer(lightingBuffer);
  }
  lightingBuffers_.clear();
}

void LightingManager::createDescriptorResources(uint32_t descriptorSetCount) {
  const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);

  if (!ltcLuts_) {
    ltcLuts_ = std::make_unique<LtcLutResources>(
        device_, *allocationManager_.memoryManager());
    ltcLuts_->load(container::util::executableDirectory());
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

  for (auto &buffer : lightingBuffers_) {
    if (buffer.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(buffer);
    }
  }
  lightingBuffers_.assign(setCount, {});
  for (auto &buffer : lightingBuffers_) {
    buffer = allocationManager_.createBuffer(
        sizeof(container::gpu::LightingData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  if (lightDescriptorPool_ != VK_NULL_HANDLE) {
    pipelineManager_.destroyDescriptorPool(lightDescriptorPool_);
  }
  lightDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * 2u},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setCount * 3u},
       {VK_DESCRIPTOR_TYPE_SAMPLER, setCount},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount * 2u}},
      setCount, 0);

  lightDescriptorSets_.assign(lightingBuffers_.size(), VK_NULL_HANDLE);
  std::vector<VkDescriptorSetLayout> layouts(lightingBuffers_.size(),
                                             lightDescriptorSetLayout_);
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = lightDescriptorPool_;
  allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
  allocInfo.pSetLayouts = layouts.data();
  if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
                               lightDescriptorSets_.data()) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate light descriptor sets");
  }

  for (size_t i = 0; i < lightingBuffers_.size(); ++i) {
    VkDescriptorBufferInfo bufInfo{lightingBuffers_[i].buffer, 0,
                                   sizeof(container::gpu::LightingData)};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = lightDescriptorSets_[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
  }
  writeLightDescriptorStorageBuffers();
  writeLtcDescriptors();
  createLightGizmoIconDescriptorResources();
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

void LightingManager::writeLtcDescriptors() const {
  if (!ltcLuts_)
    return;
  const std::array info{
      vk::DescriptorImageInfo({}, ltcLuts_->matrixView(),
                               vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::DescriptorImageInfo({}, ltcLuts_->amplitudeView(),
                               vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::DescriptorImageInfo(ltcLuts_->sampler(), {}, {})};
  std::vector<vk::WriteDescriptorSet> writes;
  writes.reserve(lightDescriptorSets_.size() * info.size());
  for (const auto set : lightDescriptorSets_)
    for (uint32_t i = 0; i < info.size(); ++i)
      writes.emplace_back(set, 5u + i, 0, 1,
                          i < 2 ? vk::DescriptorType::eSampledImage
                                : vk::DescriptorType::eSampler,
                          &info[i]);
  device_->raii().updateDescriptorSets(writes, {});
}

void LightingManager::writeLightDescriptorStorageBuffers() const {
  if (lightDescriptorSets_.empty()) {
    return;
  }

  VkDescriptorBufferInfo lightInfo{
      lightSsbo_.buffer, 0, sizeof(PointLightData) * kMaxClusteredLights};
  VkDescriptorBufferInfo areaLightInfo{areaLightSsbo_.buffer, 0,
                                       sizeof(AreaLightData) * kMaxAreaLights};
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(lightDescriptorSets_.size() * 2u);
  for (VkDescriptorSet descriptorSet : lightDescriptorSets_) {
    if (lightSsbo_.buffer != VK_NULL_HANDLE) {
      VkWriteDescriptorSet &write = writes.emplace_back();
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = 1;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &lightInfo;
    }
    if (areaLightSsbo_.buffer != VK_NULL_HANDLE) {
      VkWriteDescriptorSet &write = writes.emplace_back();
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptorSet;
      write.dstBinding = 2;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &areaLightInfo;
    }
  }

  if (writes.empty()) {
    return;
  }
  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

void LightingManager::updateRayShadowDescriptors(uint32_t imageIndex,
                                                 VkBuffer settings,
                                                 VkDeviceSize settingsSize,
                                                 VkImageView visibility) {
  vk::DescriptorBufferInfo buffer(settings, 0, settingsSize);
  vk::DescriptorImageInfo image({}, visibility, vk::ImageLayout::eGeneral);
  const std::array writes{
      vk::WriteDescriptorSet(lightDescriptorSet(imageIndex), 3, 0, 1,
                             vk::DescriptorType::eUniformBuffer, nullptr,
                             &buffer),
      vk::WriteDescriptorSet(lightDescriptorSet(imageIndex), 4, 0, 1,
                             vk::DescriptorType::eSampledImage, &image)};
  device_->raii().updateDescriptorSets(writes, {});
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

  const auto &bounds = sceneManager_->modelBounds();
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
  if (lightingBuffers_.empty())
    return;
  for (const auto &lightingBuffer : lightingBuffers_) {
    if (lightingBuffer.buffer != VK_NULL_HANDLE) {
      SceneController::writeToBuffer(allocationManager_, lightingBuffer,
                                     &lightingData_,
                                     sizeof(container::gpu::LightingData));
    }
  }
  uploadLightSsbo();
  uploadAreaLightSsbo();
}

void LightingManager::uploadLightingData(uint32_t imageIndex) const {
  if (imageIndex >= lightingBuffers_.size())
    return;
  const auto &lightingBuffer = lightingBuffers_[imageIndex];
  if (lightingBuffer.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, lightingBuffer,
                                   &lightingData_,
                                   sizeof(container::gpu::LightingData));
  }
  uploadLightSsbo();
  uploadAreaLightSsbo();
}

void LightingManager::setLightingSettings(const LightingSettings &settings) {
  const bool generatorSettingsChanged =
      generatedLightingSettingsDiffer(lightingSettings_, settings);
  lightingSettings_.preset = std::min(settings.preset, 3u);
  lightingSettings_.density = std::clamp(settings.density, 0.1f, 16.0f);
  lightingSettings_.radiusScale = std::clamp(settings.radiusScale, 0.05f, 8.0f);
  lightingSettings_.intensityScale =
      std::clamp(settings.intensityScale, 0.0f, 16.0f);
  lightingSettings_.directionalIntensity =
      std::clamp(settings.directionalIntensity, 0.0f, 16.0f);
  lightingSettings_.environmentIntensity =
      std::clamp(settings.environmentIntensity, 0.0f, 16.0f);
  lightingSettings_.bounceIntensity =
      std::clamp(settings.bounceIntensity, 0.0f, 2.0f);
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
    generatedPointOverrides_.clear();
    generatedAreaOverrides_.clear();
    if (directionalOverride_ &&
        directionalOverride_->source == EditableLightSource::Generated) {
      directionalOverride_.reset();
    }
  }
}

void LightingManager::collectStats() {
  if (lightStatsBuffer_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.allocation == nullptr) {
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
  if (!sceneManager_ || sceneManager_->authoredDirectionalLights().empty()) {
    return false;
  }

  const auto &authoredLight =
      sceneManager_->authoredDirectionalLights().front();
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

  for (const PointLightData &authoredLight :
       sceneManager_->authoredPointLights()) {
    if (pointLightsSsbo_.size() >= kMaxClusteredLights) {
      return;
    }

    PointLightData light = authoredLight;
    light.positionRadius = glm::vec4(
        glm::vec3(anchor.sceneTransform *
                  glm::vec4(glm::vec3(authoredLight.positionRadius), 1.0f)),
        authoredLight.positionRadius.w);
    if (authoredLight.coneOuterCosType.y >= 0.5f) {
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
    }
    pointLightsSsbo_.push_back(light);
  }
}

void LightingManager::appendAuthoredAreaLights(
    const SceneLightingAnchor &anchor) {
  if (!sceneManager_) {
    return;
  }

  for (const AreaLightData &authoredLight :
       sceneManager_->authoredAreaLights()) {
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
  world_.replacePointLights(pointLightsSsbo_);
  rebuildPointLightSsboFromEcs();
  assignLocalShadowLayerMetadata();
  syncLightingPointCount(lightingData_, pointLightsSsbo_);
}

void LightingManager::publishAreaLights() {
  syncLightingAreaCount(lightingData_, areaLightsSsbo_);
}

void LightingManager::rebuildPointLightSsboFromEcs() {
  pointLightsSsbo_.clear();
  pointLightsSsbo_.reserve(
      std::min<uint32_t>(world_.pointLightCount(), kMaxClusteredLights));
  world_.forEachPointLight([&](const container::ecs::LightComponent &light) {
    if (pointLightsSsbo_.size() >= kMaxClusteredLights)
      return;
    pointLightsSsbo_.push_back(light.data);
  });
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
  if (!isValidEditableLightId(entity.id)) {
    return false;
  }

  EditableLightEntity edited = entity;
  edited.type = entity.id.type;
  edited.source = entity.id.source;
  edited.selected = true;
  selectedEditableLight_ = edited.id;

  if (edited.type == EditableLightType::Directional) {
    directionalOverride_ = edited;
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
      return true;
    }

    auto &overrides = edited.source == EditableLightSource::Imported
                          ? importedPointOverrides_
                          : generatedPointOverrides_;
    if (overrides.size() <= edited.id.index) {
      overrides.resize(static_cast<size_t>(edited.id.index) + 1u);
    }
    overrides[edited.id.index] = data;
    return true;
  }

  if (edited.type == EditableLightType::Area) {
    const AreaLightData data = areaLightDataFromEditable(edited);
    if (edited.source == EditableLightSource::Manual) {
      if (edited.id.index >= manualAreaLights_.size()) {
        return false;
      }
      manualAreaLights_[edited.id.index] = data;
      return true;
    }

    auto &overrides = edited.source == EditableLightSource::Imported
                          ? importedAreaOverrides_
                          : generatedAreaOverrides_;
    if (overrides.size() <= edited.id.index) {
      overrides.resize(static_cast<size_t>(edited.id.index) + 1u);
    }
    overrides[edited.id.index] = data;
    return true;
  }

  return false;
}

EditableLightId
LightingManager::addManualEditableLight(EditableLightType type) {
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
          if (overrides[i]) {
            pointLightsSsbo_[i] = *overrides[i];
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
  const uint32_t tileCountX =
      std::max(1u, (extent.width + kTileSize - 1u) / kTileSize);
  const uint32_t tileCountY =
      std::max(1u, (extent.height + kTileSize - 1u) / kTileSize);
  const uint32_t requiredTileCount = tileCountX * tileCountY;
  const uint32_t requiredClusterCount = requiredTileCount * kClusterDepthSlices;

  if (requiredClusterCount == maxClusterCount_ &&
      tileGridSsbo_.buffer != VK_NULL_HANDLE &&
      lightIndexListSsbo_.buffer != VK_NULL_HANDLE) {
    return;
  }

  allocationManager_.destroyBuffer(lightIndexListSsbo_);
  allocationManager_.destroyBuffer(tileGridSsbo_);

  maxTileCount_ = requiredTileCount;
  maxClusterCount_ = requiredClusterCount;

  const VkDeviceSize tileGridSize = sizeof(TileLightGrid) * maxClusterCount_;
  tileGridSsbo_ = allocationManager_.createBuffer(
      tileGridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  const VkDeviceSize lightIndexListSize =
      sizeof(uint32_t) * maxClusterCount_ * kMaxLightsPerTile;
  lightIndexListSsbo_ = allocationManager_.createBuffer(
      lightIndexListSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
}

void LightingManager::writeTiledResourceDescriptors() const {
  if (tileCullSet1_ == VK_NULL_HANDLE || tileCullSet2_ == VK_NULL_HANDLE ||
      tiledDescriptorSet_ == VK_NULL_HANDLE ||
      lightSsbo_.buffer == VK_NULL_HANDLE ||
      tileGridSsbo_.buffer == VK_NULL_HANDLE ||
      lightIndexListSsbo_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.buffer == VK_NULL_HANDLE || maxClusterCount_ == 0) {
    return;
  }

  VkDescriptorBufferInfo lightInfo{
      lightSsbo_.buffer, 0, sizeof(PointLightData) * kMaxClusteredLights};
  VkDescriptorBufferInfo tileGridInfo{tileGridSsbo_.buffer, 0,
                                      sizeof(TileLightGrid) * maxClusterCount_};
  VkDescriptorBufferInfo indexListInfo{lightIndexListSsbo_.buffer, 0,
                                       sizeof(uint32_t) * maxClusterCount_ *
                                           kMaxLightsPerTile};
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

  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

void LightingManager::resizeTiledResources(VkExtent2D extent) {
  if (lightSsbo_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.buffer == VK_NULL_HANDLE) {
    return;
  }

  const uint32_t previousClusterCount = maxClusterCount_;
  allocateClusterBuffers(extent);
  if (maxClusterCount_ != previousClusterCount) {
    writeTiledResourceDescriptors();
  }
}

void LightingManager::createTiledResources(
    const std::filesystem::path &shaderDir, VkExtent2D initialExtent) {
  loadLightGizmoIconResources(shaderDir / "materials" / "gizmos" / "lights");

  // ---- Allocate SSBOs -------------------------------------------------------
  const VkDeviceSize lightSsboSize =
      sizeof(PointLightData) * kMaxClusteredLights;
  lightSsbo_ = allocationManager_.createBuffer(
      lightSsboSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  const VkDeviceSize areaLightSsboSize = sizeof(AreaLightData) * kMaxAreaLights;
  areaLightSsbo_ = allocationManager_.createBuffer(
      areaLightSsboSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

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

  if (timestampQueryPool_ == VK_NULL_HANDLE) {
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(device_->physicalDevice(), &deviceProperties);
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

  // ---- Descriptor pools & sets ----------------------------------------------
  tileCullDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}},
      4, 0);

  auto allocSet = [&](VkDescriptorSetLayout layout) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = tileCullDescriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(device_->device(), &ai, &set) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate tiled descriptor set");
    return set;
  };

  tileCullSet0_ = allocSet(tileCullSet0Layout_);
  tileCullSet1_ = allocSet(tileCullSet1Layout_);
  tileCullSet2_ = allocSet(tileCullSet2Layout_);
  tiledDescriptorSet_ = allocSet(tiledDescriptorSetLayout_);

  resizeTiledResources(initialExtent);
  writeLightDescriptorStorageBuffers();

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
    const auto spvData = container::util::readFile(compPath);
    VkShaderModule compModule =
        container::gpu::createShaderModule(device_->device(), spvData);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = compModule;
    stage.pName = "computeMain";

    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = tileCullPipelineLayout_;

    tileCullPipeline_ =
        pipelineManager_.createComputePipeline(ci, "tile_light_cull");

    destroyOwnedShaderModule(device_->device(), compModule, nullptr);
  }
}

void LightingManager::uploadLightSsbo() const {
  if (lightSsbo_.buffer == VK_NULL_HANDLE)
    return;
  const uint32_t count = std::min(
      static_cast<uint32_t>(pointLightsSsbo_.size()), kMaxClusteredLights);
  if (count == 0)
    return;
  SceneController::writeToBuffer(allocationManager_, lightSsbo_,
                                 pointLightsSsbo_.data(),
                                 sizeof(PointLightData) * count);
}

void LightingManager::uploadAreaLightSsbo() const {
  if (areaLightSsbo_.buffer == VK_NULL_HANDLE)
    return;
  const uint32_t count =
      std::min(static_cast<uint32_t>(areaLightsSsbo_.size()), kMaxAreaLights);
  if (count == 0)
    return;
  SceneController::writeToBuffer(allocationManager_, areaLightSsbo_,
                                 areaLightsSsbo_.data(),
                                 sizeof(AreaLightData) * count);
}

void LightingManager::dispatchTileCull(VkCommandBuffer cmd,
                                       VkExtent2D screenExtent,
                                       VkBuffer cameraBuffer,
                                       VkDeviceSize cameraBufferSize,
                                       VkImageView depthView, float cameraNear,
                                       float cameraFar) const {
  if (!isTiledLightingReady())
    return;

  const uint32_t tileCountX =
      std::max(1u, (screenExtent.width + kTileSize - 1u) / kTileSize);
  const uint32_t tileCountY =
      std::max(1u, (screenExtent.height + kTileSize - 1u) / kTileSize);
  const uint32_t totalLights = std::min(
      static_cast<uint32_t>(pointLightsSsbo_.size()), kMaxClusteredLights);
  lastDispatchClusterCount_ = tileCountX * tileCountY * kClusterDepthSlices;
  if (lastDispatchClusterCount_ > maxClusterCount_ ||
      tileGridSsbo_.buffer == VK_NULL_HANDLE ||
      lightIndexListSsbo_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.buffer == VK_NULL_HANDLE) {
    lastDispatchClusterCount_ = 0;
    return;
  }

  vkCmdFillBuffer(cmd, lightStatsBuffer_.buffer, 0, sizeof(uint32_t) * 4, 0u);
  if (totalLights == 0u) {
    // No point lights: keep the downstream tiled-lighting descriptor valid but
    // make every cluster empty without launching one workgroup per cluster.
    vkCmdFillBuffer(cmd, tileGridSsbo_.buffer, 0,
                    sizeof(TileLightGrid) * lastDispatchClusterCount_, 0u);

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

  // Update tileCullSet0_ with the current frame's camera + depth resources.
  {
    VkDescriptorBufferInfo camInfo{cameraBuffer, 0, cameraBufferSize};
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView = depthView;
    depthInfo.imageLayout =
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> w{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = tileCullSet0_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &camInfo;

    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = tileCullSet0_;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &depthInfo;

    vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(w.size()),
                           w.data(), 0, nullptr);
  }

  TileCullPushConstants pc{};
  pc.tileCountX = tileCountX;
  pc.tileCountY = tileCountY;
  pc.depthSliceCount = kClusterDepthSlices;
  pc.totalLights = totalLights;
  pc.cameraNear = cameraNear;
  pc.cameraFar = cameraFar;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullPipeline_);

  std::array<VkDescriptorSet, 3> sets = {tileCullSet0_, tileCullSet1_,
                                         tileCullSet2_};
  vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullPipelineLayout_, 0,
      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

  vkCmdPushConstants(cmd, tileCullPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(TileCullPushConstants), &pc);

  vkCmdDispatch(cmd, tileCountX, tileCountY, kClusterDepthSlices);

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

} // namespace container::renderer
