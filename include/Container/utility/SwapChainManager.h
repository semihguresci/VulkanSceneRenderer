#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "Container/common/CommonGLFW.h"
#include "Container/common/VulkanTypes.h"

namespace container::gpu {

class SwapChainManager {
 public:
  SwapChainManager(GLFWwindow* window, VkPhysicalDevice physicalDevice,
                   VkDevice device, VkSurfaceKHR surface,
                   QueueFamilyIndices queueFamilies);
  ~SwapChainManager();

  SwapChainManager(const SwapChainManager&) = delete;
  SwapChainManager& operator=(const SwapChainManager&) = delete;
  SwapChainManager(SwapChainManager&&) = delete;
  SwapChainManager& operator=(SwapChainManager&&) = delete;

  void initialize();
  void recreate(std::optional<VkFormat> requestedFormat = std::nullopt);
  void cleanup();
  void createFramebuffers(RenderingPassHandle renderPass);
  void destroyFramebuffers();

  [[nodiscard]] VkResult present(VkQueue presentQueue, uint32_t imageIndex,
                                 VkSemaphore waitSemaphore) const;

  [[nodiscard]] VkSwapchainKHR swapChain() const { return swapChain_; }
  [[nodiscard]] const std::vector<VkImageView>& imageViews() const {
    return swapChainImageViews_;
  }
  [[nodiscard]] size_t imageCount() const { return swapChainImages_.size(); }
  [[nodiscard]] VkImage image(size_t imageIndex) const {
    return swapChainImages_.at(imageIndex);
  }
  [[nodiscard]] const std::vector<RenderingTargetHandle>& framebuffers() const {
    return swapChainFramebuffers_;
  }
  [[nodiscard]] VkFormat imageFormat() const { return surfaceFormat_.format; }
  [[nodiscard]] VkSurfaceFormatKHR surfaceFormat() const { return surfaceFormat_; }
  [[nodiscard]] VkExtent2D extent() const { return swapChainExtent_; }
  [[nodiscard]] bool supportsTransferSrc() const {
    return supportsTransferSrc_;
  }
  [[nodiscard]] bool needsShaderSrgbEncoding() const;

  static SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device,
                                                       VkSurfaceKHR surface);

  static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device,
                                              VkSurfaceKHR surface);

 private:
  void createSwapChain(std::optional<VkFormat> requestedFormat);
  void createImageViews();
  void destroyImageViews();
  void waitForNonZeroFramebufferExtent() const;

  VkSurfaceFormatKHR chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR>& availableFormats,
      std::optional<VkFormat> requestedFormat) const;

  VkPresentModeKHR chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR>& availablePresentModes);

  VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

  GLFWwindow* window_{nullptr};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  const QueueFamilyIndices queueFamilies_;

  VkSwapchainKHR swapChain_{VK_NULL_HANDLE};
  std::vector<VkImage> swapChainImages_;
  VkSurfaceFormatKHR surfaceFormat_{VK_FORMAT_UNDEFINED,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  std::optional<VkFormat> preferredFormat_;
  VkExtent2D swapChainExtent_{};
  std::vector<VkImageView> swapChainImageViews_;
  std::vector<RenderingTargetHandle> swapChainFramebuffers_;
  bool supportsTransferSrc_{false};
};

}  // namespace container::gpu
