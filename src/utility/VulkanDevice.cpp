#include <Container/utility/VulkanDevice.h>
#include "Container/utility/SwapChainManager.h"  // FindQueueFamilies, QuerySwapChainSupport

#include <array>
#include <algorithm>
#include <cstring>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace container::gpu {
void registerRaiiDevice(const vk::raii::Device&);
void unregisterRaiiDevice(VkDevice);

namespace {

std::vector<VkExtensionProperties> enumerateDeviceExtensions(
    VkPhysicalDevice device) {
  uint32_t extensionCount = 0;
  VkResult res = vkEnumerateDeviceExtensionProperties(device, nullptr,
                                                      &extensionCount, nullptr);
  if (res != VK_SUCCESS) {
    return {};
  }

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  res = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                             availableExtensions.data());
  if (res != VK_SUCCESS) {
    return {};
  }
  return availableExtensions;
}

bool hasExtension(std::span<const VkExtensionProperties> extensions,
                  const char* name) {
  return std::ranges::any_of(extensions, [name](const auto& extension) {
    return std::strcmp(extension.extensionName, name) == 0;
  });
}

bool hasExtensionName(const std::vector<const char*>& extensions,
                      const char* name) {
  return std::ranges::any_of(extensions, [name](const char* extension) {
    return extension != nullptr && std::strcmp(extension, name) == 0;
  });
}

bool supportsPerformanceQueryFeature(VkPhysicalDevice device) {
  VkPhysicalDevicePerformanceQueryFeaturesKHR performanceFeatures{};
  performanceFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;

  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &performanceFeatures;
  vkGetPhysicalDeviceFeatures2(device, &features2);

  return performanceFeatures.performanceCounterQueryPools == VK_TRUE;
}

const VkPhysicalDeviceVulkan12Features* requestedVulkan12Features(
    const void* next) {
  const auto* feature = static_cast<const VkBaseInStructure*>(next);
  while (feature != nullptr) {
    if (feature->sType ==
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
      return reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(feature);
    }
    feature = feature->pNext;
  }
  return nullptr;
}

bool requestedFeatureSupported(VkBool32 requested, VkBool32 supported) {
  return requested != VK_TRUE || supported == VK_TRUE;
}

bool supportsRequestedVulkan12Features(
    VkPhysicalDevice device,
    const VkPhysicalDeviceVulkan12Features* requestedFeatures) {
  if (requestedFeatures == nullptr) {
    return true;
  }

  VkPhysicalDeviceVulkan12Features supportedFeatures{};
  supportedFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &supportedFeatures;
  vkGetPhysicalDeviceFeatures2(device, &features2);

  return requestedFeatureSupported(requestedFeatures->descriptorIndexing,
                                   supportedFeatures.descriptorIndexing) &&
         requestedFeatureSupported(
             requestedFeatures->runtimeDescriptorArray,
             supportedFeatures.runtimeDescriptorArray) &&
         requestedFeatureSupported(
             requestedFeatures->descriptorBindingPartiallyBound,
             supportedFeatures.descriptorBindingPartiallyBound) &&
         requestedFeatureSupported(
             requestedFeatures->descriptorBindingVariableDescriptorCount,
             supportedFeatures.descriptorBindingVariableDescriptorCount) &&
         requestedFeatureSupported(
             requestedFeatures->shaderSampledImageArrayNonUniformIndexing,
             supportedFeatures.shaderSampledImageArrayNonUniformIndexing) &&
         requestedFeatureSupported(requestedFeatures->bufferDeviceAddress,
                                   supportedFeatures.bufferDeviceAddress) &&
         requestedFeatureSupported(requestedFeatures->drawIndirectCount,
                                   supportedFeatures.drawIndirectCount) &&
         requestedFeatureSupported(requestedFeatures->hostQueryReset,
                                   supportedFeatures.hostQueryReset);
}

}  // namespace

VulkanDevice::VulkanDevice(const VulkanInstance& instance, VkSurfaceKHR surface,
                           const DeviceCreateInfo& createInfo)
    : instance_(instance.instance()), instanceOwner_(instance), surface_(surface), createInfo_(createInfo) {
  pickPhysicalDevice();
  createLogicalDevice();
  registerRaiiDevice(ownedDevice_);
}

VulkanDevice::~VulkanDevice() {
  if (device_ != VK_NULL_HANDLE) {
    // A lost device still needs its host-side RAII objects released.
    try { ownedDevice_.waitIdle(); } catch (const vk::SystemError&) {}
    unregisterRaiiDevice(device_);
    ownedDevice_.clear();
    device_ = VK_NULL_HANDLE;
  }
}

void VulkanDevice::pickPhysicalDevice() {
  uint32_t deviceCount = 0;
  VkResult res = vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
  if (res != VK_SUCCESS || deviceCount == 0) {
    throw std::runtime_error("failed to find GPUs with Vulkan support!");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  res = vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
  if (res != VK_SUCCESS) {
    throw std::runtime_error("vkEnumeratePhysicalDevices failed!");
  }

  for (VkPhysicalDevice dev : devices) {
    if (isDeviceSuitable(dev)) {
      physicalDevice_ = dev;
      break;
    }
  }

  if (physicalDevice_ == VK_NULL_HANDLE) {
    throw std::runtime_error("Vulkan 1.4 GPU with dynamic rendering, synchronization2, descriptor indexing, and indirect counts required");
  }
}

void VulkanDevice::createLogicalDevice() {
  queueFamilyIndices_ =
      SwapChainManager::FindQueueFamilies(physicalDevice_, surface_);

  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

  std::set<uint32_t> uniqueQueueFamilies = {
      queueFamilyIndices_.graphicsFamily.value(),
      queueFamilyIndices_.presentFamily.value()};

  float queuePriority = 1.0f;
  for (uint32_t queueFamily : uniqueQueueFamilies) {
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCreateInfo);
  }

  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(physicalDevice_, &supportedFeatures);

  enabledFeatures_ = createInfo_.enabledFeatures;

  constexpr size_t featureCount =
      sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
  static_assert(sizeof(VkPhysicalDeviceFeatures) == featureCount * sizeof(VkBool32),
                "VkPhysicalDeviceFeatures has unexpected padding; update merge logic");

  std::array<VkBool32, featureCount> enabledArr{};
  std::array<VkBool32, featureCount> optionalArr{};
  std::array<VkBool32, featureCount> supportedArr{};

  std::memcpy(enabledArr.data(),  &enabledFeatures_,            sizeof(VkPhysicalDeviceFeatures));
  std::memcpy(optionalArr.data(), &createInfo_.optionalFeatures, sizeof(VkPhysicalDeviceFeatures));
  std::memcpy(supportedArr.data(), &supportedFeatures,           sizeof(VkPhysicalDeviceFeatures));

  for (size_t i = 0; i < featureCount; ++i) {
    if (optionalArr[i] && supportedArr[i]) {
      enabledArr[i] = VK_TRUE;
    }
  }

  std::memcpy(&enabledFeatures_, enabledArr.data(), sizeof(VkPhysicalDeviceFeatures));
  enabledVulkan12Features_.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  if (const auto* requestedFeatures = requestedVulkan12Features(createInfo_.next)) {
    enabledVulkan12Features_ = *requestedFeatures;
    enabledVulkan12Features_.pNext = nullptr;
  }

  const std::vector<VkExtensionProperties> availableExtensions =
      enumerateDeviceExtensions(physicalDevice_);
  std::vector<const char*> enabledExtensions;
  enabledExtensions.reserve(createInfo_.requiredExtensions.size() +
                            createInfo_.optionalExtensions.size());
  const auto addEnabledExtension = [&enabledExtensions](const char* name) {
    if (name == nullptr || hasExtensionName(enabledExtensions, name)) {
      return;
    }
    enabledExtensions.push_back(name);
  };

  for (const char* requiredExtension : createInfo_.requiredExtensions) {
    addEnabledExtension(requiredExtension);
  }

  bool enablePerformanceQuery = false;
  for (const char* optionalExtension : createInfo_.optionalExtensions) {
    if (optionalExtension == nullptr) {
      continue;
    }
    if (!hasExtension(availableExtensions, optionalExtension)) {
      continue;
    }
    if (std::strcmp(optionalExtension,
                    VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME) == 0) {
      if (!supportsPerformanceQueryFeature(physicalDevice_)) {
        continue;
      }
      enablePerformanceQuery = true;
    }
    addEnabledExtension(optionalExtension);
  }

  VkPhysicalDevicePerformanceQueryFeaturesKHR performanceQueryFeatures{};
  performanceQueryFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;
  performanceQueryFeatures.performanceCounterQueryPools =
      enablePerformanceQuery ? VK_TRUE : VK_FALSE;
  performanceQueryFeatures.pNext = const_cast<void*>(createInfo_.next);

  VkDeviceCreateInfo deviceCreateInfo{};
  deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceCreateInfo.queueCreateInfoCount =
      static_cast<uint32_t>(queueCreateInfos.size());
  deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
  deviceCreateInfo.pEnabledFeatures = &enabledFeatures_;
  deviceCreateInfo.pNext =
      enablePerformanceQuery ? &performanceQueryFeatures : createInfo_.next;
  deviceCreateInfo.enabledExtensionCount =
      static_cast<uint32_t>(enabledExtensions.size());
  deviceCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

  if (createInfo_.enableValidationLayers) {
    deviceCreateInfo.enabledLayerCount =
        static_cast<uint32_t>(createInfo_.validationLayers.size());
    deviceCreateInfo.ppEnabledLayerNames = createInfo_.validationLayers.data();
  } else {
    deviceCreateInfo.enabledLayerCount = 0;
    deviceCreateInfo.ppEnabledLayerNames = nullptr;
  }

  const vk::raii::PhysicalDevice physical(instanceOwner_.raii(), physicalDevice_);
  ownedDevice_ = vk::raii::Device(physical,
      reinterpret_cast<const vk::DeviceCreateInfo&>(deviceCreateInfo));
  device_ = static_cast<VkDevice>(*ownedDevice_);

  vkGetDeviceQueue(device_, queueFamilyIndices_.graphicsFamily.value(), 0,
                   &graphicsQueue_);
  vkGetDeviceQueue(device_, queueFamilyIndices_.presentFamily.value(), 0,
                   &presentQueue_);
}

bool VulkanDevice::isDeviceSuitable(VkPhysicalDevice device) const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device, &properties);
  if (properties.apiVersion < VK_API_VERSION_1_4) return false;
  VkPhysicalDeviceVulkan13Features modernFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan14Features newestFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
  modernFeatures.pNext = &newestFeatures;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &modernFeatures;
  vkGetPhysicalDeviceFeatures2(device, &features);
  if (!modernFeatures.dynamicRendering || !modernFeatures.synchronization2 ||
      !modernFeatures.maintenance4 || !modernFeatures.shaderDemoteToHelperInvocation ||
      !newestFeatures.maintenance5 || !newestFeatures.maintenance6)
    return false;
  QueueFamilyIndices indices =
      SwapChainManager::FindQueueFamilies(device, surface_);

  bool extensionsSupported = checkDeviceExtensionSupport(device);

  bool swapChainAdequate = false;
  if (extensionsSupported) {
    SwapChainSupportDetails swapChainSupport =
        SwapChainManager::QuerySwapChainSupport(device, surface_);
    swapChainAdequate = !swapChainSupport.formats.empty() &&
                        !swapChainSupport.presentModes.empty();
  }

  return indices.isComplete() && extensionsSupported && swapChainAdequate &&
         supportsRequestedFeatures(device);
}

bool VulkanDevice::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
  const std::vector<VkExtensionProperties> availableExtensions =
      enumerateDeviceExtensions(device);
  if (availableExtensions.empty()) {
    return false;
  }

  std::set<std::string> requiredExtensions(
      createInfo_.requiredExtensions.begin(),
      createInfo_.requiredExtensions.end());

  for (const auto& extension : availableExtensions) {
    requiredExtensions.erase(extension.extensionName);
  }

  return requiredExtensions.empty();
}

bool VulkanDevice::supportsRequestedFeatures(VkPhysicalDevice device) const {
  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

  const auto* supported = reinterpret_cast<const VkBool32*>(&supportedFeatures);
  const auto* requested =
      reinterpret_cast<const VkBool32*>(&createInfo_.enabledFeatures);

  const size_t featureCount =
      sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);

  for (size_t i = 0; i < featureCount; ++i) {
    if (requested[i] && !supported[i]) {
      return false;
    }
  }

  return supportsRequestedVulkan12Features(
      device, requestedVulkan12Features(createInfo_.next));
}

}  // namespace container::gpu

