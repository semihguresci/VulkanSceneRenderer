#pragma once

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/renderer/shadow/ShadowCascadeDepthPlanner.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::scene {
class BaseCamera;
}

namespace container::renderer {

struct ShadowCascadeCullBounds {
  glm::mat4 lightView{1.0f};
  glm::vec3 receiverMinBounds{0.0f};
  float     padding0{0.0f};
  glm::vec3 receiverMaxBounds{0.0f};
  float     padding1{0.0f};
  glm::vec3 casterMinBounds{0.0f};
  float     padding2{0.0f};
  glm::vec3 casterMaxBounds{0.0f};
  float     padding3{0.0f};
};

class ShadowManager {
 public:
  ShadowManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&              pipelineManager);

  ~ShadowManager();
  ShadowManager(const ShadowManager&) = delete;
  ShadowManager& operator=(const ShadowManager&) = delete;

  // Create shadow atlas image, per-layer views, sampler, UBO, descriptor
  // resources.  Must be called once after construction.
  void createResources(VkFormat depthFormat, uint32_t descriptorSetCount);
  void recreatePerFrameResources(uint32_t descriptorSetCount);

  // Destroy all GPU resources.
  void destroy();

  // Recompute cascade splits and view-projection matrices, upload to UBO.
  void update(const container::scene::BaseCamera* camera,
              float aspectRatio,
              const glm::vec3& lightDirection,
              const container::gpu::ShadowSettings& shadowSettings,
              const ShadowCasterSceneBounds* casterSceneBounds,
              uint32_t imageIndex);
  void updateLocalShadows(
      std::span<const container::gpu::PointLightData> pointLights,
      std::span<const container::gpu::AreaLightData> areaLights,
      const container::gpu::ShadowSettings& shadowSettings,
      uint32_t localShadowLayerBudget,
      uint32_t imageIndex);

  // ---- Accessors -----------------------------------------------------------

  [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const {
    return descriptorSetLayout_;
  }
  [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t imageIndex) const {
    return imageIndex < descriptorSets_.size() ? descriptorSets_[imageIndex]
                                               : VK_NULL_HANDLE;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer& shadowUbo(uint32_t imageIndex) const {
    return shadowUbos_[imageIndex];
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer& shadowCullUbo(uint32_t imageIndex) const {
    return shadowCullUbos_[imageIndex];
  }
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> shadowUbos() const {
    return shadowUbos_;
  }
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> shadowCullUbos() const {
    return shadowCullUbos_;
  }
  [[nodiscard]] const container::gpu::ShadowData& shadowData() const {
    return shadowData_;
  }
  [[nodiscard]] const container::gpu::LocalShadowData& localShadowData() const {
    return localShadowData_;
  }
  [[nodiscard]] const container::gpu::ShadowCullData& shadowCullData() const {
    return shadowCullData_;
  }
  [[nodiscard]] const std::array<ShadowCascadeCullBounds,
                                 container::gpu::kShadowCascadeCount>&
      cascadeCullBounds() const {
    return cascadeCullBounds_;
  }
  [[nodiscard]] VkImageView shadowAtlasArrayView() const {
    return shadowAtlasArrayView_;
  }
  [[nodiscard]] VkImageView cascadeView(uint32_t cascadeIndex) const {
    return cascadeViews_[cascadeIndex];
  }
  [[nodiscard]] VkSampler shadowSampler() const { return shadowSampler_; }
  [[nodiscard]] VkFormat  depthFormat()   const { return depthFormat_; }
  [[nodiscard]] VkImage   shadowAtlasImage() const { return shadowAtlasImage_; }
  [[nodiscard]] VkImage localShadowAtlasImage() const {
    return localShadowAtlasImage_;
  }
  [[nodiscard]] uint64_t localShadowAtlasAllocatedBytes() const;
  [[nodiscard]] VkImageView localShadowAtlasArrayView() const {
    return localShadowAtlasArrayView_;
  }
  [[nodiscard]] VkImageView localShadowLayerView(uint32_t layerIndex) const {
    return layerIndex < localShadowLayerViews_.size()
               ? localShadowLayerViews_[layerIndex]
               : VK_NULL_HANDLE;
  }
  [[nodiscard]] RenderingTargetHandle localShadowFramebuffer(uint32_t layerIndex) const {
    return layerIndex < localShadowFramebuffers_.size()
               ? localShadowFramebuffers_[layerIndex]
               : VK_NULL_HANDLE;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer& localShadowUbo(
      uint32_t imageIndex) const {
    static const container::gpu::AllocatedBuffer kEmptyBuffer{};
    return imageIndex < localShadowUbos_.size() ? localShadowUbos_[imageIndex]
                                                : kEmptyBuffer;
  }
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer>
      localShadowUbos() const {
    return localShadowUbos_;
  }
  [[nodiscard]] VkDescriptorSet localShadowDescriptorSet(
      uint32_t imageIndex) const {
    return imageIndex < localDescriptorSets_.size()
               ? localDescriptorSets_[imageIndex]
               : VK_NULL_HANDLE;
  }
  [[nodiscard]] uint32_t localShadowLayerCount() const {
    return std::min(localShadowData_.counts.x,
                    container::gpu::kMaxShadowedLocalLightLayers);
  }
  [[nodiscard]] bool hasLocalShadowLayers() const {
    return localShadowLayerCount() > 0u;
  }

  [[nodiscard]] const std::array<RenderingTargetHandle, container::gpu::kShadowCascadeCount>&
      framebuffers() const { return framebuffers_; }
  [[nodiscard]] const std::array<RenderingTargetHandle,
                                 container::gpu::kMaxShadowedLocalLightLayers>&
      localShadowFramebuffers() const {
    return localShadowFramebuffers_;
  }

  void createFramebuffers(RenderingPassHandle shadowRenderPass);
  void destroyFramebuffers();

  [[nodiscard]] bool cascadeIntersectsSphere(
      uint32_t cascadeIndex,
      const glm::vec4& boundingSphere) const;

 private:
  struct CascadeViewProjData {
    glm::mat4               viewProj{1.0f};
    ShadowCascadeCullBounds cullBounds{};
    float                   texelSize{0.0f};
    float                   worldRadius{0.0f};
    float                   depthRange{0.0f};
  };

  void computeCascadeSplits(float nearPlane, float farPlane);
  CascadeViewProjData computeCascadeViewProj(
      uint32_t cascadeIndex,
      const glm::vec3& lightDirection,
      const glm::mat4& cameraView,
      const glm::mat4& cameraProj,
      float cameraNear,
      float cameraFar,
      const ShadowCasterSceneBounds* casterSceneBounds) const;
  void uploadMappedBuffer(const container::gpu::AllocatedBuffer& buffer,
                          const void* data,
                          VkDeviceSize size) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;

  VkFormat      depthFormat_{VK_FORMAT_UNDEFINED};
  VkImage       shadowAtlasImage_{VK_NULL_HANDLE};
  VmaAllocation shadowAtlasAllocation_{nullptr};
  VkImageView   shadowAtlasArrayView_{VK_NULL_HANDLE};
  std::array<VkImageView, container::gpu::kShadowCascadeCount>
      cascadeViews_{};
  std::array<RenderingTargetHandle, container::gpu::kShadowCascadeCount>
      framebuffers_{};
  VkSampler     shadowSampler_{VK_NULL_HANDLE};

  VkImage       localShadowAtlasImage_{VK_NULL_HANDLE};
  VmaAllocation localShadowAtlasAllocation_{nullptr};
  VkImageView   localShadowAtlasArrayView_{VK_NULL_HANDLE};
  std::array<VkImageView, container::gpu::kMaxShadowedLocalLightLayers>
      localShadowLayerViews_{};
  std::array<RenderingTargetHandle, container::gpu::kMaxShadowedLocalLightLayers>
      localShadowFramebuffers_{};

  std::vector<container::gpu::AllocatedBuffer> shadowUbos_{};
  std::vector<container::gpu::AllocatedBuffer> shadowCullUbos_{};
  std::vector<container::gpu::AllocatedBuffer> localShadowUbos_{};
  container::gpu::ShadowData      shadowData_{};
  container::gpu::LocalShadowData localShadowData_{};
  container::gpu::ShadowCullData  shadowCullData_{};
  std::array<float, container::gpu::kShadowCascadeCount> cascadeSplits_{};
  std::array<ShadowCascadeCullBounds, container::gpu::kShadowCascadeCount>
      cascadeCullBounds_{};

  VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool      descriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> descriptorSets_{};
  std::vector<VkDescriptorSet> localDescriptorSets_{};

  static constexpr float kCascadeLambda = 0.75f;
};

}  // namespace container::renderer
