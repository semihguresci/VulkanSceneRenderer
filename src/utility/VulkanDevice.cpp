#include <Container/utility/VulkanDevice.h>
#include "Container/utility/AsyncComputeQueue.h"
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

// vk_oidn 0.2.0's image-view backend is optional. Clone the application's
// core feature chain only if the selected device can supply its full contract.
struct ReferenceOidnFeatures {
  VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
  VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};

  bool enable(VkPhysicalDevice device, std::span<const VkExtensionProperties> extensions,
              const void* requested, VkPhysicalDeviceFeatures& base) {
    const auto* head = static_cast<const VkBaseInStructure*>(requested);
    if (!head || head->sType != v14.sType || !head->pNext || head->pNext->sType != v13.sType ||
        !head->pNext->pNext || head->pNext->pNext->sType != v12.sType ||
        !head->pNext->pNext->pNext || head->pNext->pNext->pNext->sType != v11.sType ||
        !hasExtension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) return false;

    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &v14;
    v14.pNext = &v13;
    v13.pNext = &v12;
    v12.pNext = &v11;
    if (hasExtension(extensions, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME))
      v11.pNext = &cooperative;
    vkGetPhysicalDeviceFeatures2(device, &supported);
    if (!supported.features.shaderInt64 || !supported.features.shaderInt16 ||
        !supported.features.shaderStorageImageReadWithoutFormat ||
        !supported.features.shaderStorageImageWriteWithoutFormat || !v11.storageBuffer16BitAccess ||
        !v12.bufferDeviceAddress || !v12.shaderFloat16 || !v12.vulkanMemoryModel ||
        !v12.vulkanMemoryModelDeviceScope || !v13.maintenance4 ||
        !v13.subgroupSizeControl || !v13.computeFullSubgroups || !v14.pushDescriptor) return false;

    v14 = *reinterpret_cast<const VkPhysicalDeviceVulkan14Features*>(head);
    v13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(head->pNext);
    v12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(head->pNext->pNext);
    v11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(head->pNext->pNext->pNext);
    v14.pNext = &v13;
    v13.pNext = &v12;
    v12.pNext = &v11;
    if (cooperative.cooperativeMatrix) {
      cooperative.cooperativeMatrixRobustBufferAccess = VK_FALSE;
      cooperative.pNext = v11.pNext;
      v11.pNext = &cooperative;
    }
    base.shaderInt64 = base.shaderInt16 = VK_TRUE;
    base.shaderStorageImageReadWithoutFormat = base.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    v11.storageBuffer16BitAccess = VK_TRUE;
    v12.shaderFloat16 = v12.vulkanMemoryModel = v12.vulkanMemoryModelDeviceScope = VK_TRUE;
    v13.subgroupSizeControl = v13.computeFullSubgroups = VK_TRUE;
    v14.pushDescriptor = VK_TRUE;
    return true;
  }
};

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
                                   supportedFeatures.hostQueryReset) &&
         requestedFeatureSupported(requestedFeatures->timelineSemaphore,
                                   supportedFeatures.timelineSemaphore);
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

  uint32_t familyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(familyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
  const auto asyncIndex = selectAsyncComputeQueueIndex(
      families, queueFamilyIndices_.graphicsFamily.value());
  const std::array queuePriorities{1.0f, 0.5f};
  for (uint32_t queueFamily : uniqueQueueFamilies) {
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamily;
    queueCreateInfo.queueCount = asyncIndex &&
        queueFamily == queueFamilyIndices_.graphicsFamily.value() ? 2u : 1u;
    queueCreateInfo.pQueuePriorities = queuePriorities.data();
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
  rayQuerySupport_.accelerationStructureExtension = hasExtension(
      availableExtensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
  rayQuerySupport_.rayQueryExtension =
      hasExtension(availableExtensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
  rayQuerySupport_.deferredHostOperationsExtension = hasExtension(
      availableExtensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  rayQuerySupport_.bufferDeviceAddressEnabled =
      enabledVulkan12Features_.bufferDeviceAddress == VK_TRUE;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceOpacityMicromapFeaturesEXT opacityMicromapFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
  // Only query extension structs advertised by the selected device.
  VkPhysicalDeviceFeatures2 rayFeatures{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  if (rayQuerySupport_.accelerationStructureExtension) {
    accelerationFeatures.pNext = rayFeatures.pNext;
    rayFeatures.pNext = &accelerationFeatures;
  }
  if (rayQuerySupport_.rayQueryExtension) {
    rayQueryFeatures.pNext = rayFeatures.pNext;
    rayFeatures.pNext = &rayQueryFeatures;
  }
  if (createInfo_.enableOpacityMicromaps &&
      hasExtension(availableExtensions, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME)) {
    opacityMicromapFeatures.pNext = rayFeatures.pNext;
    rayFeatures.pNext = &opacityMicromapFeatures;
  }
  vkGetPhysicalDeviceFeatures2(physicalDevice_, &rayFeatures);
  rayQuerySupport_.accelerationStructureFeature =
      accelerationFeatures.accelerationStructure == VK_TRUE;
  rayQuerySupport_.rayQueryFeature = rayQueryFeatures.rayQuery == VK_TRUE;
  rayQueriesEnabled_ =
      createInfo_.enableRayQueries && rayQuerySupport_.supported();
  opacityMicromapsEnabled_ = rayQueriesEnabled_ && opacityMicromapFeatures.micromap == VK_TRUE;
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
  if (rayQueriesEnabled_) {
    addEnabledExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    addEnabledExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    addEnabledExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  }
  if (opacityMicromapsEnabled_)
    addEnabledExtension(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);

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

  ReferenceOidnFeatures oidn;
  const void* coreFeatureChain = createInfo_.next;
  if (createInfo_.enableReferenceOidn && rayQueriesEnabled_ &&
      oidn.enable(physicalDevice_, availableExtensions, coreFeatureChain, enabledFeatures_)) {
    referenceOidnEnabled_ = true;
    referenceOidnCooperativeMatrix_ = oidn.cooperative.cooperativeMatrix == VK_TRUE;
    addEnabledExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (referenceOidnCooperativeMatrix_)
      addEnabledExtension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    coreFeatureChain = &oidn.v14;
    enabledVulkan12Features_ = oidn.v12;
    enabledVulkan12Features_.pNext = nullptr;
  }

  VkPhysicalDevicePerformanceQueryFeaturesKHR performanceQueryFeatures{};
  performanceQueryFeatures.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR;
  performanceQueryFeatures.performanceCounterQueryPools =
      enablePerformanceQuery ? VK_TRUE : VK_FALSE;
  performanceQueryFeatures.pNext = const_cast<void*>(coreFeatureChain);

  // Enable only the base features used by the shared backend. Do not copy the
  // queried structs: that would opt into capture/replay or host-build features.
  VkPhysicalDeviceAccelerationStructureFeaturesKHR enabledAcceleration{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  VkPhysicalDeviceRayQueryFeaturesKHR enabledRayQuery{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  VkPhysicalDeviceOpacityMicromapFeaturesEXT enabledOpacityMicromap{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
  const void *featureChain =
      enablePerformanceQuery ? &performanceQueryFeatures : coreFeatureChain;
  if (rayQueriesEnabled_) {
    enabledAcceleration.accelerationStructure = VK_TRUE;
    enabledAcceleration.pNext = const_cast<void *>(featureChain);
    enabledRayQuery.rayQuery = VK_TRUE;
    enabledRayQuery.pNext = &enabledAcceleration;
    featureChain = &enabledRayQuery;
  }
  if (opacityMicromapsEnabled_) {
    enabledOpacityMicromap.micromap = VK_TRUE;
    enabledOpacityMicromap.pNext = const_cast<void*>(featureChain);
    featureChain = &enabledOpacityMicromap;
  }

  VkDeviceCreateInfo deviceCreateInfo{};
  deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceCreateInfo.queueCreateInfoCount =
      static_cast<uint32_t>(queueCreateInfos.size());
  deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
  deviceCreateInfo.pEnabledFeatures = &enabledFeatures_;
  deviceCreateInfo.pNext = featureChain;
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
  if (asyncIndex)
    vkGetDeviceQueue(device_, queueFamilyIndices_.graphicsFamily.value(),
                     *asyncIndex, &asyncComputeQueue_);
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

