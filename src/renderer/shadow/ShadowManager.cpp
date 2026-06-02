#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace container::renderer {

using container::gpu::kShadowCascadeCount;
using container::gpu::kLocalShadowAreaLayerCount;
using container::gpu::kLocalShadowMapResolution;
using container::gpu::kLocalShadowPointFaceCount;
using container::gpu::kLocalShadowSpotLayerCount;
using container::gpu::kLocalShadowTypeArea;
using container::gpu::kLocalShadowTypePoint;
using container::gpu::kLocalShadowTypeSpot;
using container::gpu::kMaxAreaLights;
using container::gpu::kMaxShadowedLocalLightLayers;
using container::gpu::kShadowMapResolution;
using container::gpu::AreaLightData;
using container::gpu::LocalShadowData;
using container::gpu::LocalShadowLayerData;
using container::gpu::PointLightData;
using container::gpu::ShadowCullData;
using container::gpu::ShadowData;

namespace {

constexpr float kLocalShadowNearPlane = 0.05f;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;
constexpr float kLocalShadowNormalBiasMinTexels = 0.35f;
constexpr float kLocalShadowNormalBiasMaxTexels = 1.0f;
constexpr float kLocalShadowSoftFilterRadiusMultiplier = 28.0f;
constexpr float kLocalShadowPointSourceRadiusFraction = 0.025f;
constexpr float kDirectionalCascadeGuardTexels = 32.0f;

[[nodiscard]] bool hasFiniteLocalShadowRange(float range) {
  return std::isfinite(range) && range > 0.0f;
}

[[nodiscard]] float clampLocalShadowRange(float range) {
  return std::max(range, kLocalShadowNearPlane + 0.01f);
}

[[nodiscard]] glm::vec3 normalizeOr(const glm::vec3& value,
                                    const glm::vec3& fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * glm::inversesqrt(len2);
}

[[nodiscard]] glm::vec3 upForDirection(const glm::vec3& direction) {
  return std::abs(glm::dot(direction, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.98f
             ? glm::vec3(0.0f, 0.0f, 1.0f)
             : glm::vec3(0.0f, 1.0f, 0.0f);
}

[[nodiscard]] bool finiteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool validBounds(const glm::vec3& minBounds,
                               const glm::vec3& maxBounds) {
  return finiteVec3(minBounds) && finiteVec3(maxBounds) &&
         minBounds.x <= maxBounds.x && minBounds.y <= maxBounds.y &&
         minBounds.z <= maxBounds.z;
}

[[nodiscard]] bool validCasterSceneBounds(
    const ShadowCasterSceneBounds* bounds) {
  return bounds != nullptr && validBounds(bounds->minBounds, bounds->maxBounds);
}

[[nodiscard]] std::array<glm::vec3, 8> boundsCorners(
    const ShadowCasterSceneBounds& bounds) {
  return {{
      {bounds.minBounds.x, bounds.minBounds.y, bounds.minBounds.z},
      {bounds.maxBounds.x, bounds.minBounds.y, bounds.minBounds.z},
      {bounds.minBounds.x, bounds.maxBounds.y, bounds.minBounds.z},
      {bounds.maxBounds.x, bounds.maxBounds.y, bounds.minBounds.z},
      {bounds.minBounds.x, bounds.minBounds.y, bounds.maxBounds.z},
      {bounds.maxBounds.x, bounds.minBounds.y, bounds.maxBounds.z},
      {bounds.minBounds.x, bounds.maxBounds.y, bounds.maxBounds.z},
      {bounds.maxBounds.x, bounds.maxBounds.y, bounds.maxBounds.z},
  }};
}

[[nodiscard]] uint32_t metadataBaseLayer(const PointLightData& light) {
  const float encoded = light.coneOuterCosType.z;
  if (!std::isfinite(encoded) || encoded < 1.0f) {
    return kMaxShadowedLocalLightLayers;
  }
  return static_cast<uint32_t>(encoded + 0.5f) - 1u;
}

[[nodiscard]] uint32_t metadataLayerCount(const PointLightData& light) {
  const float encoded = light.coneOuterCosType.w;
  if (!std::isfinite(encoded) || encoded < 1.0f) {
    return 0u;
  }
  return static_cast<uint32_t>(encoded + 0.5f);
}

[[nodiscard]] std::array<glm::vec3, kLocalShadowPointFaceCount>
pointShadowDirections() {
  return {{{1.0f, 0.0f, 0.0f},
           {-1.0f, 0.0f, 0.0f},
           {0.0f, 1.0f, 0.0f},
           {0.0f, -1.0f, 0.0f},
           {0.0f, 0.0f, 1.0f},
           {0.0f, 0.0f, -1.0f}}};
}

[[nodiscard]] std::array<glm::vec3, kLocalShadowPointFaceCount>
pointShadowUps() {
  return {{{0.0f, -1.0f, 0.0f},
           {0.0f, -1.0f, 0.0f},
           {0.0f, 0.0f, 1.0f},
           {0.0f, 0.0f, -1.0f},
           {0.0f, -1.0f, 0.0f},
           {0.0f, -1.0f, 0.0f}}};
}

void setPackedAreaRef(LocalShadowData& data, uint32_t areaIndex,
                      uint32_t encodedLayer) {
  if (areaIndex >= kMaxAreaLights) {
    return;
  }
  glm::uvec4& pack = data.areaLightRefs[areaIndex / 4u];
  pack[areaIndex % 4u] = encodedLayer;
}

}  // namespace

ShadowManager::ShadowManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&              pipelineManager)
    : device_(std::move(device)),
      allocationManager_(allocationManager),
      pipelineManager_(pipelineManager) {}

ShadowManager::~ShadowManager() {
  destroy();
}

// ---------------------------------------------------------------------------
void ShadowManager::createResources(VkFormat depthFormat,
                                    uint32_t descriptorSetCount) {
  depthFormat_ = depthFormat;
  VkDevice dev = device_->device();

  // ---- Shadow atlas image (2D array, kShadowCascadeCount layers) ----------
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = depthFormat_;
    ii.extent      = {kShadowMapResolution, kShadowMapResolution, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = kShadowCascadeCount;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &shadowAtlasImage_,
                       &shadowAtlasAllocation_, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow atlas image");
  }

  // ---- Full array view (for shader sampling) ------------------------------
  {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = shadowAtlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format   = depthFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowCascadeCount};
    if (vkCreateImageView(dev, &vi, nullptr, &shadowAtlasArrayView_) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow atlas array view");
  }

  // ---- Per-cascade single-layer views (for framebuffer attachment) ---------
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image    = shadowAtlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = depthFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &cascadeViews_[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow cascade view");
  }

  // ---- Local light shadow atlas -------------------------------------------
  {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType   = VK_IMAGE_TYPE_2D;
    ii.format      = depthFormat_;
    ii.extent      = {kLocalShadowMapResolution, kLocalShadowMapResolution, 1};
    ii.mipLevels   = 1;
    ii.arrayLayers = kMaxShadowedLocalLightLayers;
    ii.samples     = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling      = VK_IMAGE_TILING_OPTIMAL;
    ii.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocationManager_.memoryManager()->allocator(),
                       &ii, &ai, &localShadowAtlasImage_,
                       &localShadowAtlasAllocation_, nullptr) != VK_SUCCESS)
      throw std::runtime_error("failed to create local shadow atlas image");
  }
  {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = localShadowAtlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = depthFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                           kMaxShadowedLocalLightLayers};
    if (vkCreateImageView(dev, &vi, nullptr,
                          &localShadowAtlasArrayView_) != VK_SUCCESS)
      throw std::runtime_error("failed to create local shadow atlas view");
  }
  for (uint32_t i = 0; i < kMaxShadowedLocalLightLayers; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = localShadowAtlasImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = depthFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
    if (vkCreateImageView(dev, &vi, nullptr,
                          &localShadowLayerViews_[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create local shadow layer view");
  }

  // ---- Comparison sampler for PCF -----------------------------------------
  {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    // Reverse-Z shadow maps clear to far depth (0.0) and use GREATER compare.
    // PCF taps that fall just outside a cascade should therefore read far/lit,
    // not near/shadowed.
    si.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    si.compareEnable = VK_TRUE;
    si.compareOp     = VK_COMPARE_OP_GREATER;
    si.minLod       = 0.0f;
    si.maxLod       = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &shadowSampler_) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow sampler");
  }

  // ---- Descriptor set layout (shadow UBO only — binding 0) ----------------
  {
    const VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    descriptorSetLayout_ =
        pipelineManager_.createDescriptorSetLayout({binding}, {0});
  }

  recreatePerFrameResources(descriptorSetCount);
}

void ShadowManager::recreatePerFrameResources(uint32_t descriptorSetCount) {
  const uint32_t setCount = std::max<uint32_t>(1u, descriptorSetCount);
  VkDevice dev = device_->device();

  for (auto& shadowUbo : shadowUbos_) {
    if (shadowUbo.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(shadowUbo);
    }
  }
  for (auto& shadowCullUbo : shadowCullUbos_) {
    if (shadowCullUbo.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(shadowCullUbo);
    }
  }
  for (auto& localShadowUbo : localShadowUbos_) {
    if (localShadowUbo.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(localShadowUbo);
    }
  }
  shadowUbos_.assign(setCount, {});
  shadowCullUbos_.assign(setCount, {});
  localShadowUbos_.assign(setCount, {});
  for (auto& shadowUbo : shadowUbos_) {
    shadowUbo = allocationManager_.createBuffer(
        sizeof(ShadowData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
  for (auto& shadowCullUbo : shadowCullUbos_) {
    shadowCullUbo = allocationManager_.createBuffer(
        sizeof(ShadowCullData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
  for (auto& localShadowUbo : localShadowUbos_) {
    localShadowUbo = allocationManager_.createBuffer(
        sizeof(LocalShadowData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  if (descriptorPool_ != VK_NULL_HANDLE) {
    pipelineManager_.destroyDescriptorPool(descriptorPool_);
  }
  descriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount * 2u}}, setCount * 2u, 0);

  descriptorSets_.assign(setCount, VK_NULL_HANDLE);
  localDescriptorSets_.assign(setCount, VK_NULL_HANDLE);
  std::vector<VkDescriptorSetLayout> layouts(setCount, descriptorSetLayout_);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool     = descriptorPool_;
  ai.descriptorSetCount = setCount;
  ai.pSetLayouts        = layouts.data();
  if (vkAllocateDescriptorSets(dev, &ai, descriptorSets_.data()) != VK_SUCCESS)
    throw std::runtime_error("failed to allocate shadow descriptor sets");
  if (vkAllocateDescriptorSets(dev, &ai, localDescriptorSets_.data()) !=
      VK_SUCCESS)
    throw std::runtime_error("failed to allocate local shadow descriptor sets");

  for (size_t i = 0; i < descriptorSets_.size(); ++i) {
    VkDescriptorBufferInfo bufInfo{shadowUbos_[i].buffer, 0, sizeof(ShadowData)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = descriptorSets_[i];
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo     = &bufInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
  }
  for (size_t i = 0; i < localDescriptorSets_.size(); ++i) {
    VkDescriptorBufferInfo bufInfo{localShadowUbos_[i].buffer, 0,
                                   sizeof(LocalShadowData)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = localDescriptorSets_[i];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
  }
}

// ---------------------------------------------------------------------------
void ShadowManager::createFramebuffers(VkRenderPass shadowRenderPass) {
  destroyFramebuffers();
  VkDevice dev = device_->device();
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass      = shadowRenderPass;
    fbi.attachmentCount = 1;
    fbi.pAttachments    = &cascadeViews_[i];
    fbi.width           = kShadowMapResolution;
    fbi.height          = kShadowMapResolution;
    fbi.layers          = 1;
    if (vkCreateFramebuffer(dev, &fbi, nullptr, &framebuffers_[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create shadow framebuffer");
  }
  for (uint32_t i = 0; i < kMaxShadowedLocalLightLayers; ++i) {
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = shadowRenderPass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &localShadowLayerViews_[i];
    fbi.width = kLocalShadowMapResolution;
    fbi.height = kLocalShadowMapResolution;
    fbi.layers = 1;
    if (vkCreateFramebuffer(dev, &fbi, nullptr,
                            &localShadowFramebuffers_[i]) != VK_SUCCESS)
      throw std::runtime_error("failed to create local shadow framebuffer");
  }
}

// ---------------------------------------------------------------------------
void ShadowManager::destroyFramebuffers() {
  VkDevice dev = device_->device();
  for (auto& fb : framebuffers_) {
    if (fb != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(dev, fb, nullptr);
      fb = VK_NULL_HANDLE;
    }
  }
  for (auto& fb : localShadowFramebuffers_) {
    if (fb != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(dev, fb, nullptr);
      fb = VK_NULL_HANDLE;
    }
  }
}

// ---------------------------------------------------------------------------
void ShadowManager::destroy() {
  VkDevice dev = device_->device();
  if (!dev) return;

  destroyFramebuffers();

  if (shadowSampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(dev, shadowSampler_, nullptr);
    shadowSampler_ = VK_NULL_HANDLE;
  }

  for (auto& v : cascadeViews_) {
    if (v != VK_NULL_HANDLE) {
      vkDestroyImageView(dev, v, nullptr);
      v = VK_NULL_HANDLE;
    }
  }
  for (auto& v : localShadowLayerViews_) {
    if (v != VK_NULL_HANDLE) {
      vkDestroyImageView(dev, v, nullptr);
      v = VK_NULL_HANDLE;
    }
  }
  if (shadowAtlasArrayView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(dev, shadowAtlasArrayView_, nullptr);
    shadowAtlasArrayView_ = VK_NULL_HANDLE;
  }
  if (localShadowAtlasArrayView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(dev, localShadowAtlasArrayView_, nullptr);
    localShadowAtlasArrayView_ = VK_NULL_HANDLE;
  }
  if (shadowAtlasImage_ != VK_NULL_HANDLE && shadowAtlasAllocation_ != nullptr) {
    vmaDestroyImage(allocationManager_.memoryManager()->allocator(),
                    shadowAtlasImage_, shadowAtlasAllocation_);
    shadowAtlasImage_      = VK_NULL_HANDLE;
    shadowAtlasAllocation_ = nullptr;
  }
  if (localShadowAtlasImage_ != VK_NULL_HANDLE &&
      localShadowAtlasAllocation_ != nullptr) {
    vmaDestroyImage(allocationManager_.memoryManager()->allocator(),
                    localShadowAtlasImage_, localShadowAtlasAllocation_);
    localShadowAtlasImage_ = VK_NULL_HANDLE;
    localShadowAtlasAllocation_ = nullptr;
  }
  for (auto& shadowUbo : shadowUbos_) {
    if (shadowUbo.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(shadowUbo);
    }
  }
  shadowUbos_.clear();
  for (auto& shadowCullUbo : shadowCullUbos_) {
    if (shadowCullUbo.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(shadowCullUbo);
    }
  }
  shadowCullUbos_.clear();
  for (auto& localShadowUbo : localShadowUbos_) {
    if (localShadowUbo.buffer != VK_NULL_HANDLE) {
      allocationManager_.destroyBuffer(localShadowUbo);
    }
  }
  localShadowUbos_.clear();

  if (descriptorPool_ != VK_NULL_HANDLE) {
    pipelineManager_.destroyDescriptorPool(descriptorPool_);
    descriptorPool_ = VK_NULL_HANDLE;
  }
  if (descriptorSetLayout_ != VK_NULL_HANDLE) {
    pipelineManager_.destroyDescriptorSetLayout(descriptorSetLayout_);
    descriptorSetLayout_ = VK_NULL_HANDLE;
  }
  descriptorSets_.clear();
  localDescriptorSets_.clear();
}

// ---------------------------------------------------------------------------
void ShadowManager::computeCascadeSplits(float nearPlane, float farPlane) {
  const float ratio = farPlane / nearPlane;
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    const float p = static_cast<float>(i + 1) /
                    static_cast<float>(kShadowCascadeCount);
    const float logSplit  = nearPlane * std::pow(ratio, p);
    const float linSplit  = nearPlane + (farPlane - nearPlane) * p;
    cascadeSplits_[i] = kCascadeLambda * logSplit +
                        (1.0f - kCascadeLambda) * linSplit;
  }
}

// ---------------------------------------------------------------------------
ShadowManager::CascadeViewProjData ShadowManager::computeCascadeViewProj(
    uint32_t cascadeIndex,
    const glm::vec3& lightDirection,
    const glm::mat4& cameraView,
    const glm::mat4& cameraProj,
    float cameraNear,
    float cameraFar,
    const ShadowCasterSceneBounds* casterSceneBounds) const {
  const float cascadeNear =
      (cascadeIndex == 0) ? cameraNear : cascadeSplits_[cascadeIndex - 1];
  const float cascadeFar = cascadeSplits_[cascadeIndex];

  constexpr std::array<glm::vec2, 4> ndcXY = {{
      {-1.0f, -1.0f},
      { 1.0f, -1.0f},
      { 1.0f,  1.0f},
      {-1.0f,  1.0f},
  }};

  const glm::mat4 invVP = glm::inverse(cameraProj * cameraView);

  std::array<glm::vec3, 4> nearCorners{};
  std::array<glm::vec3, 4> farCorners{};
  for (size_t i = 0; i < ndcXY.size(); ++i) {
    glm::vec4 nearCorner = invVP * glm::vec4(ndcXY[i], 1.0f, 1.0f);
    glm::vec4 farCorner  = invVP * glm::vec4(ndcXY[i], 0.0f, 1.0f);
    nearCorners[i] = glm::vec3(nearCorner) / nearCorner.w;
    farCorners[i]  = glm::vec3(farCorner) / farCorner.w;
  }

  const float cameraDepthRange = std::max(cameraFar - cameraNear, 1e-4f);
  const float tNear =
      std::clamp((cascadeNear - cameraNear) / cameraDepthRange, 0.0f, 1.0f);
  const float tFar =
      std::clamp((cascadeFar - cameraNear) / cameraDepthRange, 0.0f, 1.0f);

  std::array<glm::vec3, 8> sliceCorners{};
  for (size_t i = 0; i < nearCorners.size(); ++i) {
    const glm::vec3 ray = farCorners[i] - nearCorners[i];
    sliceCorners[i]     = nearCorners[i] + ray * tNear;
    sliceCorners[i + 4] = nearCorners[i] + ray * tFar;
  }

  glm::vec3 center(0.0f);
  for (const auto& corner : sliceCorners) center += corner;
  center /= static_cast<float>(sliceCorners.size());

  float cascadeRadius = 0.0f;
  for (const auto& corner : sliceCorners)
    cascadeRadius = std::max(cascadeRadius, glm::length(corner - center));
  cascadeRadius = std::max(cascadeRadius, 1.0f);

  glm::vec3 lightDir = lightDirection;
  if (glm::dot(lightDir, lightDir) < 1e-6f) lightDir = glm::vec3(-0.45f, -1.0f, -0.3f);
  lightDir = glm::normalize(lightDir);

  const glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.99f)
      ? glm::vec3(0, 0, 1)
      : glm::vec3(0, 1, 0);
  const float projectionRadius = expandShadowCascadeRadiusForFilterGuard(
      cascadeRadius, kShadowMapResolution, kDirectionalCascadeGuardTexels);
  const float cascadeExtent = projectionRadius * 2.0f;
  const float texelSize =
      cascadeExtent / static_cast<float>(kShadowMapResolution);

  auto makeSnappedLightView = [&](float lightDistance) {
    const glm::mat4 unsnappedLightView = container::math::lookAt(
        -lightDir * lightDistance, glm::vec3(0.0f), up);
    const glm::vec3 lightSpaceCenter =
        glm::vec3(unsnappedLightView * glm::vec4(center, 1.0f));

    glm::vec2 snappedLightSpaceCenter{lightSpaceCenter.x, lightSpaceCenter.y};
    if (texelSize > 1e-6f) {
      snappedLightSpaceCenter.x =
          std::floor(snappedLightSpaceCenter.x / texelSize) * texelSize;
      snappedLightSpaceCenter.y =
          std::floor(snappedLightSpaceCenter.y / texelSize) * texelSize;
    }

    const glm::mat4 invUnsnappedLightView = glm::inverse(unsnappedLightView);
    const glm::vec3 snappedCenter = glm::vec3(
        invUnsnappedLightView *
        glm::vec4(snappedLightSpaceCenter, lightSpaceCenter.z, 1.0f));
    return container::math::lookAt(snappedCenter - lightDir * lightDistance,
                                   snappedCenter, up);
  };

  auto receiverBoundsForView = [&](const glm::mat4& lightView) {
    ShadowCasterSceneBounds bounds{};
    bounds.minBounds = {-projectionRadius, -projectionRadius,
                        std::numeric_limits<float>::max()};
    bounds.maxBounds = {projectionRadius, projectionRadius,
                        -std::numeric_limits<float>::max()};
    for (const auto& corner : sliceCorners) {
      const glm::vec3 lightSpaceCorner =
          glm::vec3(lightView * glm::vec4(corner, 1.0f));
      bounds.minBounds.z = std::min(bounds.minBounds.z, lightSpaceCorner.z);
      bounds.maxBounds.z = std::max(bounds.maxBounds.z, lightSpaceCorner.z);
    }
    return bounds;
  };

  auto casterBoundsForView = [&](const glm::mat4& lightView,
                                 ShadowCasterSceneBounds& outBounds) {
    if (!validCasterSceneBounds(casterSceneBounds)) {
      return false;
    }
    outBounds.minBounds = {-projectionRadius, -projectionRadius,
                           std::numeric_limits<float>::max()};
    outBounds.maxBounds = {projectionRadius, projectionRadius,
                           -std::numeric_limits<float>::max()};
    for (const auto& corner : boundsCorners(*casterSceneBounds)) {
      const glm::vec3 lightSpaceCorner =
          glm::vec3(lightView * glm::vec4(corner, 1.0f));
      outBounds.minBounds.z = std::min(outBounds.minBounds.z,
                                       lightSpaceCorner.z);
      outBounds.maxBounds.z = std::max(outBounds.maxBounds.z,
                                       lightSpaceCorner.z);
    }
    return std::isfinite(outBounds.minBounds.z) &&
           std::isfinite(outBounds.maxBounds.z);
  };

  auto depthPlanForView = [&](const glm::mat4& lightView,
                              float lightDistance) {
    const ShadowCasterSceneBounds receiverBounds =
        receiverBoundsForView(lightView);
    ShadowCasterSceneBounds casterBounds{};
    const bool hasFiniteCasterBounds =
        casterBoundsForView(lightView, casterBounds);

    ShadowCascadeDepthPlanInputs inputs{};
    inputs.receiverMinBounds = receiverBounds.minBounds;
    inputs.receiverMaxBounds = receiverBounds.maxBounds;
    inputs.casterMinBounds = casterBounds.minBounds;
    inputs.casterMaxBounds = casterBounds.maxBounds;
    inputs.hasFiniteCasterBounds = hasFiniteCasterBounds;
    inputs.texelSize = texelSize;
    inputs.lightDistance = lightDistance;
    inputs.fallbackCasterDepth = projectionRadius * 2.0f;
    return buildShadowCascadeDepthPlan(inputs);
  };

  float lightDistance = cascadeRadius * 2.0f;
  glm::mat4 lightView = makeSnappedLightView(lightDistance);
  ShadowCascadeDepthPlan depthPlan = depthPlanForView(lightView, lightDistance);
  constexpr uint32_t kMaxLightDistanceRebuilds = 8u;
  for (uint32_t rebuild = 0u;
       rebuild < kMaxLightDistanceRebuilds &&
       depthPlan.lightDistanceIncrease > 1.0e-4f;
       ++rebuild) {
    lightDistance = depthPlan.lightDistance;
    lightView = makeSnappedLightView(lightDistance);
    depthPlan = depthPlanForView(lightView, lightDistance);
  }
  if (depthPlan.lightDistanceIncrease > 1.0e-4f) {
    lightDistance = depthPlan.lightDistance;
    lightView = makeSnappedLightView(lightDistance);
    depthPlan = depthPlanForView(lightView, lightDistance);
  }

  const glm::mat4 lightProj = container::math::orthoRH_ReverseZ(
      -projectionRadius, projectionRadius, -projectionRadius, projectionRadius,
      depthPlan.nearPlane, depthPlan.farPlane);

  CascadeViewProjData result{};
  result.viewProj = lightProj * lightView;
  result.cullBounds.lightView = lightView;
  result.cullBounds.receiverMinBounds = depthPlan.receiverMinBounds;
  result.cullBounds.receiverMaxBounds = depthPlan.receiverMaxBounds;
  result.cullBounds.casterMinBounds = depthPlan.casterMinBounds;
  result.cullBounds.casterMaxBounds = depthPlan.casterMaxBounds;
  result.texelSize = texelSize;
  result.worldRadius = projectionRadius;
  result.depthRange = depthPlan.depthRange;
  return result;
}

// ---------------------------------------------------------------------------
void ShadowManager::update(const container::scene::BaseCamera* camera,
                           float aspectRatio,
                           const glm::vec3& lightDirection,
                           const container::gpu::ShadowSettings& shadowSettings,
                           const ShadowCasterSceneBounds* casterSceneBounds,
                           uint32_t imageIndex) {
  if (!camera) return;

  float nearPlane = 0.1f;
  float farPlane = 100.0f;
  if (const auto* perspCam =
          dynamic_cast<const container::scene::PerspectiveCamera*>(camera)) {
    nearPlane = perspCam->nearPlane();
    farPlane = perspCam->farPlane();
  } else if (const auto* orthoCam =
                 dynamic_cast<const container::scene::OrthographicCamera*>(
                     camera)) {
    nearPlane = orthoCam->nearPlane();
    farPlane = orthoCam->farPlane();
  }
  nearPlane = std::max(nearPlane, 1.0e-4f);
  farPlane = std::max(farPlane, nearPlane + 1.0e-3f);

  computeCascadeSplits(nearPlane, farPlane);

  const glm::mat4 cameraView = camera->viewMatrix();
  glm::mat4 cameraProj = camera->projectionMatrix(aspectRatio);

  const float normalBiasMinTexels =
      std::max(shadowSettings.normalBiasMinTexels, 0.0f);
  const float normalBiasMaxTexels =
      std::max(shadowSettings.normalBiasMaxTexels, normalBiasMinTexels);
  const float maxDepthBias = std::max(shadowSettings.maxDepthBias, 0.0f);
  const float baseFilterRadiusTexels =
      std::max(shadowSettings.filterRadiusTexels, 0.25f);
  const float pcssLightRadiusDegrees = std::clamp(
      shadowSettings.directionalPcssLightRadiusDegrees, 0.0f, 10.0f);
  const float pcssTanLightRadius =
      std::tan(pcssLightRadiusDegrees * kPi / 180.0f);
  const float pcssBlockerSearchRadiusTexels = std::max(
      shadowSettings.directionalPcssBlockerSearchRadiusTexels,
      baseFilterRadiusTexels);
  const float pcssMaxFilterRadiusTexels = std::max(
      shadowSettings.directionalPcssMaxFilterRadiusTexels,
      baseFilterRadiusTexels);
  const float directionalContactMaxDistance = std::clamp(
      shadowSettings.directionalContactMaxDistance, 0.0f, 0.75f);
  const float directionalContactThickness = std::min(
      std::clamp(shadowSettings.directionalContactThickness, 0.0f, 0.25f),
      directionalContactMaxDistance);
  const float directionalContactFadeDistance =
      directionalContactMaxDistance > 0.0f
          ? std::clamp(shadowSettings.directionalContactFadeDistance,
                       directionalContactThickness,
                       directionalContactMaxDistance)
          : 0.0f;
  shadowData_.biasSettings = glm::vec4(
      normalBiasMinTexels,
      normalBiasMaxTexels,
      std::max(shadowSettings.slopeBiasScale, 0.0f),
      std::max(shadowSettings.receiverPlaneBiasScale, 0.0f));
  shadowData_.filterSettings = glm::vec4(
      baseFilterRadiusTexels,
      std::clamp(shadowSettings.cascadeBlendFraction, 0.0f, 0.45f),
      std::max(shadowSettings.constantDepthBias, 0.0f),
      maxDepthBias);
  shadowData_.softShadowSettings = glm::vec4(
      shadowSettings.directionalPcssEnabled ? 1.0f : 0.0f,
      pcssTanLightRadius,
      pcssBlockerSearchRadiusTexels,
      pcssMaxFilterRadiusTexels);
  shadowData_.contactShadowSettings = glm::vec4(
      shadowSettings.directionalContactVisibility ? 1.0f : 0.0f,
      directionalContactMaxDistance,
      directionalContactThickness,
      directionalContactFadeDistance);

  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    const CascadeViewProjData cascadeData =
        computeCascadeViewProj(i, lightDirection, cameraView, cameraProj,
                               nearPlane, farPlane, casterSceneBounds);
    shadowData_.cascades[i].viewProj = cascadeData.viewProj;
    shadowData_.cascades[i].splitDepth = cascadeSplits_[i];
    shadowData_.cascades[i].texelSize = cascadeData.texelSize;
    shadowData_.cascades[i].worldRadius = cascadeData.worldRadius;
    shadowData_.cascades[i].depthRange = cascadeData.depthRange;

    shadowCullData_.cascades[i].viewProj = cascadeData.viewProj;
    shadowCullData_.cascades[i].lightView = cascadeData.cullBounds.lightView;
    shadowCullData_.cascades[i].receiverMinBounds =
        glm::vec4(cascadeData.cullBounds.receiverMinBounds, 0.0f);
    shadowCullData_.cascades[i].receiverMaxBounds =
        glm::vec4(cascadeData.cullBounds.receiverMaxBounds, 0.0f);
    shadowCullData_.cascades[i].casterMinBounds =
        glm::vec4(cascadeData.cullBounds.casterMinBounds, 0.0f);
    shadowCullData_.cascades[i].casterMaxBounds =
        glm::vec4(cascadeData.cullBounds.casterMaxBounds, 0.0f);

    cascadeCullBounds_[i] = cascadeData.cullBounds;
  }

  if (imageIndex < shadowUbos_.size()) {
    uploadMappedBuffer(shadowUbos_[imageIndex], &shadowData_,
                       sizeof(ShadowData));
  }
  if (imageIndex < shadowCullUbos_.size()) {
    uploadMappedBuffer(shadowCullUbos_[imageIndex], &shadowCullData_,
                       sizeof(ShadowCullData));
  }
}

void ShadowManager::uploadMappedBuffer(
    const container::gpu::AllocatedBuffer& buffer,
    const void* data,
    VkDeviceSize size) const {
  if (buffer.buffer == VK_NULL_HANDLE) return;
  void* mapped = buffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager_.memoryManager()->allocator(),
                     buffer.allocation, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map shadow buffer for writing");
    }
    mappedHere = true;
  }

  std::memcpy(mapped, data, static_cast<size_t>(size));
  if (vmaFlushAllocation(allocationManager_.memoryManager()->allocator(),
                         buffer.allocation, 0, size) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(allocationManager_.memoryManager()->allocator(),
                     buffer.allocation);
    }
    throw std::runtime_error("failed to flush shadow buffer after writing");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager_.memoryManager()->allocator(),
                   buffer.allocation);
  }
}

void ShadowManager::updateLocalShadows(
    std::span<const PointLightData> pointLights,
    std::span<const AreaLightData> areaLights,
    const container::gpu::ShadowSettings& shadowSettings,
    uint32_t localShadowLayerBudget,
    uint32_t imageIndex) {
  localShadowData_ = {};
  localShadowData_.counts = glm::uvec4(0u, kMaxShadowedLocalLightLayers,
                                       kLocalShadowMapResolution, 0u);
  const uint32_t activeLayerBudget =
      std::min(localShadowLayerBudget, kMaxShadowedLocalLightLayers);
  if (activeLayerBudget == 0u) {
    if (imageIndex < localShadowUbos_.size()) {
      uploadMappedBuffer(localShadowUbos_[imageIndex], &localShadowData_,
                         sizeof(LocalShadowData));
    }
    return;
  }
  const float localNormalBiasMinTexels =
      std::clamp(shadowSettings.normalBiasMinTexels, 0.0f,
                 kLocalShadowNormalBiasMinTexels);
  const float localNormalBiasMaxTexels =
      std::clamp(shadowSettings.normalBiasMaxTexels,
                 localNormalBiasMinTexels,
                 kLocalShadowNormalBiasMaxTexels);
  localShadowData_.biasSettings = glm::vec4(
      localNormalBiasMinTexels,
      localNormalBiasMaxTexels,
      std::max(shadowSettings.slopeBiasScale, 0.0f),
      std::max(shadowSettings.receiverPlaneBiasScale, 0.0f));
  const float baseFilterRadiusTexels =
      std::max(shadowSettings.filterRadiusTexels, 0.25f);
  const float maxSoftFilterRadiusTexels =
      std::max(baseFilterRadiusTexels,
               baseFilterRadiusTexels *
                   kLocalShadowSoftFilterRadiusMultiplier);
  localShadowData_.filterSettings = glm::vec4(
      baseFilterRadiusTexels,
      maxSoftFilterRadiusTexels,
      std::max(shadowSettings.constantDepthBias, 0.0f),
      std::max(shadowSettings.maxDepthBias, 0.0f));

  uint32_t usedLayerCount = 0u;
  const auto writeLayer =
      [&](uint32_t layerIndex, uint32_t lightIndex, uint32_t faceIndex,
          uint32_t sourceLayerCount, const glm::vec3& position,
          float range, const glm::vec3& direction, float lightType,
          float texelSize, float depthRange, float outerCos,
          float sourceRadius,
          const glm::mat4& viewProj) {
        if (layerIndex >= activeLayerBudget) {
          return;
        }
        LocalShadowLayerData layer{};
        layer.viewProj = viewProj;
        layer.positionRange = glm::vec4(position, range);
        layer.directionType = glm::vec4(direction, lightType);
        layer.meta = glm::uvec4(lightIndex, faceIndex, sourceLayerCount, 1u);
        layer.params = glm::vec4(texelSize, depthRange, outerCos,
                                 std::max(sourceRadius, 0.0f));
        localShadowData_.layers[layerIndex] = layer;
        usedLayerCount = std::max(usedLayerCount, layerIndex + 1u);
      };

  const auto pointDirs = pointShadowDirections();
  const auto pointUps = pointShadowUps();
  for (uint32_t lightIndex = 0u;
       lightIndex < pointLights.size() &&
       lightIndex < container::gpu::kMaxClusteredLights;
       ++lightIndex) {
    const PointLightData& light = pointLights[lightIndex];
    const uint32_t baseLayer = metadataBaseLayer(light);
    const uint32_t layerCount = metadataLayerCount(light);
    if (baseLayer >= activeLayerBudget || layerCount == 0u ||
        baseLayer + layerCount > activeLayerBudget) {
      continue;
    }

    const bool isSpot = light.coneOuterCosType.y >= 0.5f;
    const glm::vec3 position = glm::vec3(light.positionRadius);
    if (!hasFiniteLocalShadowRange(light.positionRadius.w)) {
      continue;
    }
    const float range = clampLocalShadowRange(light.positionRadius.w);
    const float nearPlane = std::min(kLocalShadowNearPlane, range * 0.25f);
    const float farPlane = std::max(range, nearPlane + 0.01f);
    if (isSpot) {
      const glm::vec3 direction =
          normalizeOr(glm::vec3(light.directionInnerCos),
                      glm::vec3(0.0f, 0.0f, -1.0f));
      const float outerCos =
          std::clamp(light.coneOuterCosType.x, -0.98f, 0.999f);
      const float fov = std::clamp(2.0f * std::acos(outerCos),
                                   kPi / 36.0f, kPi * 0.95f);
      const glm::mat4 view =
          container::math::lookAt(position, position + direction,
                                  upForDirection(direction));
      const glm::mat4 proj =
          container::math::perspectiveRH_ReverseZ(fov, 1.0f, nearPlane,
                                                  farPlane);
      const float texelSize =
          2.0f * std::tan(fov * 0.5f) * farPlane /
          static_cast<float>(kLocalShadowMapResolution);
      const float sourceRadius =
          std::max(range * kLocalShadowPointSourceRadiusFraction,
                   texelSize * 2.0f);
      writeLayer(baseLayer, lightIndex, 0u, kLocalShadowSpotLayerCount,
                 position, range, direction, kLocalShadowTypeSpot,
                 texelSize, farPlane - nearPlane, outerCos, sourceRadius,
                 proj * view);
      continue;
    }

    const glm::mat4 proj =
        container::math::perspectiveRH_ReverseZ(kHalfPi, 1.0f, nearPlane,
                                                farPlane);
    const float texelSize =
        farPlane / static_cast<float>(kLocalShadowMapResolution);
    const float sourceRadius =
        std::max(range * kLocalShadowPointSourceRadiusFraction,
                 texelSize * 2.0f);
    for (uint32_t faceIndex = 0u;
         faceIndex < kLocalShadowPointFaceCount && faceIndex < layerCount;
         ++faceIndex) {
      const glm::vec3 direction = pointDirs[faceIndex];
      const glm::mat4 view =
          container::math::lookAt(position, position + direction,
                                  pointUps[faceIndex]);
      writeLayer(baseLayer + faceIndex, lightIndex, faceIndex,
                 kLocalShadowPointFaceCount, position, range, direction,
                 kLocalShadowTypePoint, texelSize, farPlane - nearPlane,
                 0.0f, sourceRadius, proj * view);
    }
  }

  uint32_t nextLayer = usedLayerCount;
  const uint32_t areaCount =
      std::min<uint32_t>(static_cast<uint32_t>(areaLights.size()),
                         kMaxAreaLights);
  for (uint32_t areaIndex = 0u; areaIndex < areaCount; ++areaIndex) {
    if (nextLayer + kLocalShadowAreaLayerCount > activeLayerBudget) {
      break;
    }
    const AreaLightData& light = areaLights[areaIndex];
    if (light.colorIntensity.a <= 0.0f) {
      continue;
    }

    const glm::vec3 position = glm::vec3(light.positionRange);
    const glm::vec3 direction =
        normalizeOr(glm::vec3(light.directionType),
                    glm::vec3(0.0f, 0.0f, -1.0f));
    if (!hasFiniteLocalShadowRange(light.positionRange.w)) {
      continue;
    }
    const float range = clampLocalShadowRange(light.positionRange.w);
    const float nearPlane = std::min(kLocalShadowNearPlane, range * 0.25f);
    const float farPlane = std::max(range, nearPlane + 0.01f);
    const float maxHalfSize = std::max(
        std::max(std::abs(light.tangentHalfSize.w),
                 std::abs(light.bitangentHalfSize.w)),
        0.05f);
    const float halfExtent = std::max(maxHalfSize * 2.0f, farPlane * 0.5f);
    const glm::mat4 view =
        container::math::lookAt(position, position + direction,
                                upForDirection(direction));
    const glm::mat4 proj =
        container::math::orthoRH_ReverseZ(-halfExtent, halfExtent,
                                          -halfExtent, halfExtent, nearPlane,
                                          farPlane);
    writeLayer(nextLayer, areaIndex, 0u, kLocalShadowAreaLayerCount, position,
               range, direction, kLocalShadowTypeArea,
               (halfExtent * 2.0f) /
                   static_cast<float>(kLocalShadowMapResolution),
               farPlane - nearPlane, 0.0f, maxHalfSize, proj * view);
    setPackedAreaRef(localShadowData_, areaIndex, nextLayer + 1u);
    ++nextLayer;
  }

  localShadowData_.counts.x = usedLayerCount = std::max(usedLayerCount, nextLayer);
  localShadowData_.counts.w = usedLayerCount > 0u ? 1u : 0u;

  if (imageIndex < localShadowUbos_.size()) {
    uploadMappedBuffer(localShadowUbos_[imageIndex], &localShadowData_,
                       sizeof(LocalShadowData));
  }
}

bool ShadowManager::cascadeIntersectsSphere(
    uint32_t cascadeIndex,
    const glm::vec4& boundingSphere) const {
  if (cascadeIndex >= kShadowCascadeCount) return false;

  const auto& cascadeBounds = cascadeCullBounds_[cascadeIndex];
  const glm::vec3 sphereCenter = glm::vec3(boundingSphere);
  const float     sphereRadius = std::max(boundingSphere.w, 0.0f);
  const glm::vec3 lightSpaceCenter = glm::vec3(
      cascadeBounds.lightView * glm::vec4(sphereCenter, 1.0f));

  const glm::vec3 sphereMin = lightSpaceCenter - glm::vec3(sphereRadius);
  const glm::vec3 sphereMax = lightSpaceCenter + glm::vec3(sphereRadius);

  return sphereMax.x >= cascadeBounds.casterMinBounds.x &&
         sphereMin.x <= cascadeBounds.casterMaxBounds.x &&
         sphereMax.y >= cascadeBounds.casterMinBounds.y &&
         sphereMin.y <= cascadeBounds.casterMaxBounds.y &&
         sphereMax.z >= cascadeBounds.casterMinBounds.z &&
         sphereMin.z <= cascadeBounds.casterMaxBounds.z;
}

}  // namespace container::renderer
