#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanDevice.h"

#include <memory>

namespace container::renderer {

struct RenderPasses {
  RenderingPassHandle depthPrepass{VK_NULL_HANDLE};
  RenderingPassHandle bimDepthPrepass{VK_NULL_HANDLE};
  RenderingPassHandle gBuffer{VK_NULL_HANDLE};
  RenderingPassHandle bimGBuffer{VK_NULL_HANDLE};
  RenderingPassHandle transparentPick{VK_NULL_HANDLE};
  RenderingPassHandle shadow{VK_NULL_HANDLE};
  RenderingPassHandle lighting{VK_NULL_HANDLE};
  RenderingPassHandle transformGizmos{VK_NULL_HANDLE};
  RenderingPassHandle postProcess{VK_NULL_HANDLE};
  RenderingPassHandle forwardLighting{VK_NULL_HANDLE};
  RenderingPassHandle forwardTransparent{VK_NULL_HANDLE};
};

class RenderPassManager {
 public:
  explicit RenderPassManager(std::shared_ptr<container::gpu::VulkanDevice> device);
  ~RenderPassManager();

  RenderPassManager(const RenderPassManager&) = delete;
  RenderPassManager& operator=(const RenderPassManager&) = delete;

  void create(VkFormat swapchainFormat,
              VkFormat depthStencilFormat,
              VkFormat sceneColorFormat,
              VkFormat albedoFormat,
              VkFormat normalFormat,
              VkFormat materialFormat,
              VkFormat emissiveFormat,
              VkFormat specularFormat,
              VkFormat pickIdFormat,
              VkSampleCountFlagBits msaaSamples);

  void destroy();

  [[nodiscard]] const RenderPasses& passes() const { return passes_; }

  [[nodiscard]] VkFormat findDepthStencilFormat() const;

 private:
  VkFormat findSupportedFormat(std::initializer_list<VkFormat> candidates,
                               VkImageTiling tiling,
                               VkFormatFeatureFlags features) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  RenderPasses passes_{};
};

}  // namespace container::renderer
