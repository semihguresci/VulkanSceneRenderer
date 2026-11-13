#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <vector>

#include "Container/common/CommonGLFW.h"
#include "Container/common/VulkanTypes.h"

namespace container::gpu {

class SwapChainManager {
 public:
  SwapChainManager(GLFWwindow* window, VkPhysicalDevice physicalDevice,
                   VkDevice device, VkSurfaceKHR surface,
                   QueueFamilyIndices queueFamilies,
                   bool presentFencesEnabled = false,
                   size_t framesInFlight = 0);
  ~SwapChainManager();

  SwapChainManager(const SwapChainManager&) = delete;
  SwapChainManager& operator=(const SwapChainManager&) = delete;
  SwapChainManager(SwapChainManager&&) = delete;
  SwapChainManager& operator=(SwapChainManager&&) = delete;

  void initialize();
  bool recreate(std::optional<VkFormat> requestedFormat = std::nullopt);
  void cleanup();
  void drainAcquires();
  void createFramebuffers(RenderingPassHandle renderPass);
  void destroyFramebuffers();

  [[nodiscard]] VkResult acquire(uint64_t timeout, size_t frameIndex,
                                 VkSemaphore imageAvailable, uint32_t& imageIndex);
  [[nodiscard]] VkResult present(VkQueue presentQueue, uint32_t imageIndex);
  [[nodiscard]] VkSemaphore renderFinishedForImage(size_t imageIndex) const {
    return active_.images.at(imageIndex).renderFinished;
  }

  [[nodiscard]] VkSwapchainKHR swapChain() const { return active_.handle; }
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
  struct PresentationImage {
    VkSemaphore renderFinished{VK_NULL_HANDLE};
    VkFence presentFence{VK_NULL_HANDLE};
    bool fencePending{false};
    bool presented{false};
  };

  struct Generation {
    VkSwapchainKHR handle{VK_NULL_HANDLE};
    uint64_t id{0};
    std::vector<PresentationImage> images;
    bool enqueuedPresent{false};
    bool outOfDate{false};
  };

  struct AcquireSlot {
    VkFence fence{VK_NULL_HANDLE};
    uint64_t generation{0};  // Nonzero while a successful acquire is pending.
    bool provesPresentation{false};
  };

  void createSwapChain(std::optional<VkFormat> requestedFormat);
  void createPresentationResources();
  void destroyGeneration(Generation& generation);
  void clearRenderTargets();
  VkResult finishAcquire(AcquireSlot& slot, bool wait);
  void collectRetiredGenerations();
  void createImageViews();
  void destroyImageViews();
  bool waitForNonZeroFramebufferExtent() const;

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

  const bool presentFencesEnabled_;
  Generation active_;
  std::list<Generation> retired_;
  std::vector<AcquireSlot> acquires_;
  uint64_t generationCounter_{0};
  uint64_t latestReacquiredGeneration_{0};
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
