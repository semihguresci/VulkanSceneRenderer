#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

#include "Container/utility/SwapChainManager.h"
#include "Container/utility/QueueFamilySelection.h"

namespace container::gpu {

SwapChainManager::SwapChainManager(GLFWwindow* window,
                                   VkPhysicalDevice physicalDevice,
                                   VkDevice device, VkSurfaceKHR surface,
                                   QueueFamilyIndices queueFamilies)
    : window_(window),
      physicalDevice_(physicalDevice),
      device_(device),
      surface_(surface),
      queueFamilies_(queueFamilies) {}

SwapChainManager::~SwapChainManager() { cleanup(); }

void SwapChainManager::initialize() {
  recreate();
}

void SwapChainManager::recreate(std::optional<VkFormat> requestedFormat) {
  waitForNonZeroFramebufferExtent();
  createSwapChain(requestedFormat);
}

void SwapChainManager::cleanup() {
  destroyFramebuffers();
  destroyImageViews();

  if (swapChain_ != VK_NULL_HANDLE) {
    destroyOwnedSwapchainKHR(device_, swapChain_, nullptr);
    swapChain_ = VK_NULL_HANDLE;
  }
  swapChainImages_.clear();
  surfaceFormat_ = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  swapChainExtent_ = {};
  supportsTransferSrc_ = false;
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

void SwapChainManager::createSwapChain(std::optional<VkFormat> requestedFormat) {
  const SwapChainSupportDetails swapChainSupport =
      QuerySwapChainSupport(physicalDevice_, surface_);
  if (swapChainSupport.formats.empty() || swapChainSupport.presentModes.empty())
    throw std::runtime_error("failed to query swap chain surface support");

  VkSurfaceFormatKHR surfaceFormat =
      chooseSwapSurfaceFormat(swapChainSupport.formats, requestedFormat);
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
  constexpr auto captureUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImageFormatProperties captureProperties{};
  const bool supportsTransferSrc =
      (swapChainSupport.capabilities.supportedUsageFlags &
       VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 &&
      vkGetPhysicalDeviceImageFormatProperties(
          physicalDevice_, surfaceFormat.format, VK_IMAGE_TYPE_2D,
          VK_IMAGE_TILING_OPTIMAL, captureUsage, 0, &captureProperties) == VK_SUCCESS &&
      extent.width <= captureProperties.maxExtent.width &&
      extent.height <= captureProperties.maxExtent.height;
  if (supportsTransferSrc)
    createInfo.imageUsage = captureUsage;

  const uint32_t queueFamilyIndices[] = {queueFamilies_.graphicsFamily.value(),
                                         queueFamilies_.presentFamily.value()};

  if (queueFamilies_.graphicsFamily != queueFamilies_.presentFamily) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }

  createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
  createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createInfo.presentMode = presentMode;
  // PNG readback and scene-linear fragment writes need every pixel, including
  // when a capture is requested after startup or the window is obscured.
  createInfo.clipped = VK_FALSE;
  createInfo.oldSwapchain = swapChain_;

  VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
  VkResult res;
  try {
    res = createOwnedSwapchainKHR(device_, &createInfo, nullptr, &newSwapchain);
  } catch (...) {
    cleanup();
    throw;
  }
  // Vulkan retires oldSwapchain when creation is attempted, even on failure.
  cleanup();
  if (res != VK_SUCCESS)
    throw std::runtime_error("failed to create swap chain");

  swapChain_ = newSwapchain;
  surfaceFormat_ = surfaceFormat;
  swapChainExtent_ = extent;
  supportsTransferSrc_ = supportsTransferSrc;
  try {
    uint32_t swapImageCount = 0;
    if (vkGetSwapchainImagesKHR(device_, swapChain_, &swapImageCount, nullptr) !=
            VK_SUCCESS || swapImageCount == 0)
      throw std::runtime_error("failed to query swap chain image count");
    swapChainImages_.resize(swapImageCount);
    if (vkGetSwapchainImagesKHR(device_, swapChain_, &swapImageCount,
                               swapChainImages_.data()) != VK_SUCCESS)
      throw std::runtime_error("failed to query swap chain images");
    swapChainImages_.resize(swapImageCount);
    createImageViews();
  } catch (...) {
    cleanup();
    throw;
  }
  if (requestedFormat)
    preferredFormat_ = requestedFormat;
}

void SwapChainManager::createImageViews() {
  swapChainImageViews_.resize(swapChainImages_.size());

  for (size_t i = 0; i < swapChainImages_.size(); ++i) {
    VkImageViewCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createInfo.image = swapChainImages_[i];
    createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createInfo.format = imageFormat();

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
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface,
                                               &details.capabilities) != VK_SUCCESS)
    return {};

  const auto queryList = [&](auto query, auto& values) {
    VkResult result;
    do {
      uint32_t count = 0;
      if (query(device, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
        values.clear();
        return;
      }
      values.resize(count);
      result = query(device, surface, &count, values.data());
      values.resize(result == VK_SUCCESS ? count : 0);
    } while (result == VK_INCOMPLETE);
  };
  queryList(vkGetPhysicalDeviceSurfaceFormatsKHR, details.formats);
  queryList(vkGetPhysicalDeviceSurfacePresentModesKHR, details.presentModes);

  return details;
}

QueueFamilyIndices SwapChainManager::FindQueueFamilies(VkPhysicalDevice device,
                                                       VkSurfaceKHR surface) {
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount,
                                           queueFamilies.data());

  std::vector<QueueFamilySupport> families;
  families.reserve(queueFamilyCount);
  for (uint32_t i = 0; i < queueFamilyCount; ++i) {
    const auto& family = queueFamilies[i];
    VkBool32 presentSupport = VK_FALSE;
    const VkResult result =
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
    families.push_back({family.queueFlags, family.queueCount,
                        result == VK_SUCCESS && presentSupport == VK_TRUE});
  }

  return selectQueueFamilies(families);
}

VkSurfaceFormatKHR SwapChainManager::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats,
    std::optional<VkFormat> requestedFormat) const {
  const auto isSrgbSpace = [](const VkSurfaceFormatKHR& format) {
    return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  };
  const auto findSrgb = [&](VkFormat format) {
    return std::ranges::find_if(availableFormats, [&](const auto& available) {
      return available.format == format && isSrgbSpace(available);
    });
  };
  if (requestedFormat || preferredFormat_) {
    const auto match = findSrgb(requestedFormat ? *requestedFormat : *preferredFormat_);
    if (match != availableFormats.end())
      return *match;
    if (requestedFormat)
      throw std::runtime_error("requested sRGB swap chain format is unavailable");
  }

  constexpr std::array preferredSdrFormats{
      VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
  for (const VkFormat format : preferredSdrFormats) {
    const auto match = findSrgb(format);
    if (match != availableFormats.end())
      return *match;
  }
  const auto sdr = std::ranges::find_if(availableFormats, isSrgbSpace);
  if (sdr != availableFormats.end())
    return *sdr;
  const auto linear = std::ranges::find_if(availableFormats, [](const auto& format) {
    return format.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT &&
           format.format == VK_FORMAT_R16G16B16A16_SFLOAT;
  });
  if (linear != availableFormats.end())
    return *linear;
  throw std::runtime_error("surface has no supported sRGB color space");
}

bool SwapChainManager::needsShaderSrgbEncoding() const {
  if (surfaceFormat_.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    return false;
  switch (imageFormat()) {
  case VK_FORMAT_R8_SRGB:
  case VK_FORMAT_R8G8_SRGB:
  case VK_FORMAT_R8G8B8_SRGB:
  case VK_FORMAT_B8G8R8_SRGB:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    return false;
  default:
    return true;
  }
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
