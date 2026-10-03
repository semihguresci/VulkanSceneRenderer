#include "Container/renderer/platform/VulkanContextInitializer.h"

#include "Container/app/AppConfig.h"
#include "Container/common/CommonGLFW.h"
#include "Container/common/CommonVulkan.h"
#include "Container/utility/DebugMessengerExt.h"
#include "Container/utility/Logger.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanInstance.h"

#include <stdexcept>
#include <vector>

namespace container::renderer {

using container::gpu::CreateDebugUtilsMessengerEXT;
using container::gpu::debugCallback;
using container::gpu::DestroyDebugUtilsMessengerEXT;

VulkanContextInitializer::VulkanContextInitializer(const container::app::AppConfig& config)
    : config_(config) {
}

VulkanContextResult VulkanContextInitializer::initialize(
    const std::vector<const char*>& requiredWindowExtensions,
    GLFWwindow* nativeWindow) const {
  VulkanContextResult result{};

  // --- Instance ---
  {
    container::gpu::InstanceCreateInfo ci{};
    ci.applicationName        = "Vulkan Container";
    ci.engineName             = "Vulkan Container Engine";
    ci.apiVersion             = VK_API_VERSION_1_4;
    ci.enableValidationLayers  = config_.enableValidationLayers;
    ci.validationLayers       = config_.validationLayers;
    ci.requiredExtensions     = requiredWindowExtensions;
    if (config_.enableValidationLayers) {
      ci.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkDebugUtilsMessengerCreateInfoEXT debugCI{};
    if (config_.enableValidationLayers) {
      debugCI.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      debugCI.messageSeverity =
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      debugCI.messageType =
          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debugCI.pfnUserCallback = debugCallback;
      ci.next = &debugCI;
    }

    result.instanceWrapper =
        std::make_shared<container::gpu::VulkanInstance>(ci);
    result.instance = result.instanceWrapper->instance();
  }

  // --- Debug messenger ---
  if (config_.enableValidationLayers) {
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;

    result.ownedDebugMessenger = vk::raii::DebugUtilsMessengerEXT(
        result.instanceWrapper->raii(),
        reinterpret_cast<const vk::DebugUtilsMessengerCreateInfoEXT&>(ci));
    result.debugMessenger = static_cast<VkDebugUtilsMessengerEXT>(*result.ownedDebugMessenger);
  }

  // --- Surface ---
  if (glfwCreateWindowSurface(result.instance, nativeWindow, nullptr,
                              &result.surface) != VK_SUCCESS) {
    throw std::runtime_error("failed to create window surface!");
  }
  result.ownedSurface = vk::raii::SurfaceKHR(result.instanceWrapper->raii(), result.surface);

  // --- Device ---
  {
    container::gpu::DeviceCreateInfo ci{};
    ci.requiredExtensions     = config_.deviceExtensions;
    ci.optionalExtensions     = config_.optionalDeviceExtensions;
    ci.validationLayers       = config_.validationLayers;
    ci.enableValidationLayers  = config_.enableValidationLayers;
    ci.enabledFeatures.samplerAnisotropy       = VK_TRUE;
    ci.enabledFeatures.fragmentStoresAndAtomics = VK_TRUE;
    ci.enabledFeatures.geometryShader          = VK_TRUE;
    ci.enabledFeatures.drawIndirectFirstInstance = VK_TRUE;
    ci.enabledFeatures.multiDrawIndirect = VK_TRUE;
    ci.optionalFeatures.fillModeNonSolid       = VK_TRUE;
    ci.optionalFeatures.wideLines              = VK_TRUE;

    VkPhysicalDeviceVulkan11Features vulkan11Features{};
    vulkan11Features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11Features.shaderDrawParameters = VK_TRUE;

    VkPhysicalDeviceVulkan12Features vulkan12Features{};
    vulkan12Features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.descriptorIndexing = VK_TRUE;
    vulkan12Features.runtimeDescriptorArray = VK_TRUE;
    vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
    vulkan12Features.descriptorBindingVariableDescriptorCount = VK_TRUE;
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    vulkan12Features.bufferDeviceAddress = VK_TRUE;
    vulkan12Features.drawIndirectCount = VK_TRUE;
    vulkan12Features.hostQueryReset = VK_TRUE;

    VkPhysicalDeviceVulkan13Features vulkan13Features{};
    vulkan13Features.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    vulkan13Features.dynamicRendering = VK_TRUE;
    vulkan13Features.synchronization2 = VK_TRUE;
    vulkan13Features.maintenance4 = VK_TRUE;
    vulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceVulkan14Features vulkan14Features{};
    vulkan14Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    vulkan14Features.maintenance5 = VK_TRUE;
    vulkan14Features.maintenance6 = VK_TRUE;
    vulkan14Features.pNext = &vulkan13Features;

    vulkan13Features.pNext = &vulkan12Features;
    vulkan12Features.pNext = &vulkan11Features;
    ci.next = &vulkan14Features;

    result.deviceWrapper = std::make_shared<container::gpu::VulkanDevice>(
        *result.instanceWrapper, result.surface, ci);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(result.deviceWrapper->physicalDevice(), &properties);
    container::log::ContainerLogger::instance().renderer()->info(
        "Vulkan {}.{}.{} on {}: dynamic rendering, synchronization2, GPU indirect counts, Slang SPIR-V 1.6",
        VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
        VK_API_VERSION_PATCH(properties.apiVersion), properties.deviceName);

    const auto& enabled                 = result.deviceWrapper->enabledFeatures();
    result.wireframeRasterModeSupported = enabled.fillModeNonSolid == VK_TRUE;
    result.wireframeWideLinesSupported  = result.wireframeRasterModeSupported
                                              ? (enabled.wideLines == VK_TRUE)
                                              : true;
    result.wireframeSupported           = true;

    if (!result.wireframeRasterModeSupported) {
      container::log::ContainerLogger::instance().renderer()->warn(
          "Wireframe polygon mode unsupported; using shader-based wireframe fallback");
    }
  }

  return result;
}

VkFormat VulkanContextInitializer::findSupportedFormat(
    VkPhysicalDevice physicalDevice,
    std::initializer_list<VkFormat> candidates,
    VkImageTiling tiling,
    VkFormatFeatureFlags features) {
  for (VkFormat fmt : candidates) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, fmt, &props);
    const VkFormatFeatureFlags tilingFeatures =
        tiling == VK_IMAGE_TILING_LINEAR ? props.linearTilingFeatures
                                         : props.optimalTilingFeatures;
    if ((tilingFeatures & features) == features) {
      return fmt;
    }
  }
  throw std::runtime_error("failed to find a supported Vulkan image format");
}

VkFormat VulkanContextInitializer::findDepthStencilFormat(
    VkPhysicalDevice physicalDevice) {
  return findSupportedFormat(
      physicalDevice,
      {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
      VK_IMAGE_TILING_OPTIMAL,
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
}

}  // namespace container::renderer
