#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "Container/utility/SwapChainManager.h"

namespace container::gpu {

SwapChainManager::SwapChainManager(GLFWwindow* window,
                                   VkPhysicalDevice physicalDevice,
                                   VkDevice device, VkSurfaceKHR surface)
    : window_(window),
      physicalDevice_(physicalDevice),
      device_(device),
      surface_(surface) {}

SwapChainManager::~SwapChainManager() { cleanup(); }

void SwapChainManager::initialize() {
  waitForNonZeroFramebufferExtent();
  createSwapChain();
  createImageViews();
}

void SwapChainManager::recreate(RenderingPassHandle renderPass) {
  waitForNonZeroFramebufferExtent();

  const VkSwapchainKHR oldSwapchain = swapChain_;
  createSwapChain(oldSwapchain);

  destroyFramebuffers();
  destroyImageViews();
  if (oldSwapchain != VK_NULL_HANDLE) {
    destroyOwnedSwapchainKHR(device_, oldSwapchain, nullptr);
  }

  createImageViews();
  createFramebuffers(renderPass);
}

void SwapChainManager::cleanup() {
  destroyFramebuffers();
  destroyImageViews();

  if (swapChain_ != VK_NULL_HANDLE) {
    destroyOwnedSwapchainKHR(device_, swapChain_, nullptr);
    swapChain_ = VK_NULL_HANDLE;
  }
  swapChainImages_.clear();
}

void SwapChainManager::destroyFramebuffers() {
  for (RenderingTargetHandle framebuffer : swapChainFramebuffers_) {
    destroyRenderingTarget(device_, framebuffer, nullptr);
  }
  swapChainFramebuffers_.clear();
}

void SwapChainManager::destroyImageViews() {
  for (VkImageView imageView : swapChainImageViews_) {
    destroyVulkanImageView(device_, imageView, nullptr);
  }
  swapChainImageViews_.clear();
}

void SwapChainManager::waitForNonZeroFramebufferExtent() const {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  while (!glfwWindowShouldClose(window_) && (width == 0 || height == 0)) {
    glfwWaitEvents();
    glfwGetFramebufferSize(window_, &width, &height);
  }
}

void SwapChainManager::createSwapChain(VkSwapchainKHR oldSwapchain) {
  SwapChainSupportDetails swapChainSupport =
      QuerySwapChainSupport(physicalDevice_, surface_);

  VkSurfaceFormatKHR surfaceFormat =
      chooseSwapSurfaceFormat(swapChainSupport.formats);
  VkPresentModeKHR presentMode =
      chooseSwapPresentMode(swapChainSupport.presentModes);
  VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);
  if (extent.width == 0 || extent.height == 0) {
    throw std::runtime_error("cannot create a zero-sized swap chain");
  }

  uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
  if (swapChainSupport.capabilities.maxImageCount > 0 &&
      imageCount > swapChainSupport.capabilities.maxImageCount) {
    imageCount = swapChainSupport.capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surface_;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = extent;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  supportsTransferSrc_ =
      (swapChainSupport.capabilities.supportedUsageFlags &
       VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  if (supportsTransferSrc_) {
    createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }

  QueueFamilyIndices indices = FindQueueFamilies(physicalDevice_, surface_);
  uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(),
                                   indices.presentFamily.value()};

  if (indices.graphicsFamily != indices.presentFamily) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
  createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = oldSwapchain;

  VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
  VkResult res =
      createOwnedSwapchainKHR(device_, &createInfo, nullptr, &newSwapchain);

  if (res != VK_SUCCESS) {
    throw std::runtime_error("failed to create swap chain!");
  }

  uint32_t swapImageCount = 0;
  vkGetSwapchainImagesKHR(device_, newSwapchain, &swapImageCount, nullptr);

  std::vector<VkImage> newImages(swapImageCount);
  vkGetSwapchainImagesKHR(device_, newSwapchain, &swapImageCount,
                          newImages.data());

  swapChain_ = newSwapchain;
  swapChainImages_ = std::move(newImages);
  swapChainImageFormat_ = surfaceFormat.format;
  swapChainExtent_ = extent;
}

void SwapChainManager::createImageViews() {
  swapChainImageViews_.resize(swapChainImages_.size());

  for (size_t i = 0; i < swapChainImages_.size(); ++i) {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = swapChainImages_[i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = swapChainImageFormat_;

    createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = 1;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    VkResult res = createVulkanImageView(device_, &createInfo, nullptr,
                                     &swapChainImageViews_[i]);

    if (res != VK_SUCCESS) {
      throw std::runtime_error("failed to create image views!");
    }
  }
}

void SwapChainManager::createFramebuffers(RenderingPassHandle renderPass) {
  swapChainFramebuffers_.resize(swapChainImageViews_.size());

  for (size_t i = 0; i < swapChainImageViews_.size(); ++i) {
    VkImageView attachments[] = {swapChainImageViews_[i]};

    RenderingTargetCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = swapChainExtent_.width;
    framebufferInfo.height = swapChainExtent_.height;
    framebufferInfo.layers = 1;

    VkResult res = createRenderingTarget(device_, &framebufferInfo, nullptr,
                                       &swapChainFramebuffers_[i]);

    if (res != VK_SUCCESS) {
      throw std::runtime_error("failed to create framebuffer!");
    }
  }
}

VkResult SwapChainManager::present(VkQueue presentQueue, uint32_t imageIndex,
                                   VkSemaphore waitSemaphore) const {
  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &waitSemaphore;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &swapChain_;
  presentInfo.pImageIndices = &imageIndex;

  return vkQueuePresentKHR(presentQueue, &presentInfo);
}

SwapChainSupportDetails SwapChainManager::QuerySwapChainSupport(
    VkPhysicalDevice device, VkSurfaceKHR surface) {
  SwapChainSupportDetails details{};

  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface,
                                            &details.capabilities);

  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

  if (formatCount != 0) {
    details.formats.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
                                         details.formats.data());
  }

  uint32_t presentModeCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
                                            nullptr);

  if (presentModeCount != 0) {
    details.presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(
        device, surface, &presentModeCount, details.presentModes.data());
  }

  return details;
}

QueueFamilyIndices SwapChainManager::FindQueueFamilies(VkPhysicalDevice device,
                                                       VkSurfaceKHR surface) {
  QueueFamilyIndices indices{};

  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                           queueFamilies.data());

  for (uint32_t i = 0; i < queueFamilyCount; ++i) {
    if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      indices.graphicsFamily = i;
    }

    VkBool32 presentSupport = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

    if (presentSupport) {
      indices.presentFamily = i;
    }

    if (indices.isComplete()) {
      break;
    }
  }

  return indices;
}

VkSurfaceFormatKHR SwapChainManager::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats) {
  for (const auto& availableFormat : availableFormats) {
    if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
        availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      return availableFormat;
    }
  }

  return availableFormats[0];
}

VkPresentModeKHR SwapChainManager::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& availablePresentModes) {
  for (const auto& availablePresentMode : availablePresentModes) {
    if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
      return availablePresentMode;
    }
  }

  return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D SwapChainManager::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& capabilities) {
  if (capabilities.currentExtent.width !=
      std::numeric_limits<uint32_t>::max()) {
    return capabilities.currentExtent;
  }

  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window_, &width, &height);

  VkExtent2D actualExtent{static_cast<uint32_t>(width),
                          static_cast<uint32_t>(height)};

  actualExtent.width =
      std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                 capabilities.maxImageExtent.width);

  actualExtent.height =
      std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                 capabilities.maxImageExtent.height);

  return actualExtent;
}

}  // namespace container::gpu
