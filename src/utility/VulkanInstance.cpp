#include "Container/utility/VulkanInstance.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace container::gpu {

VulkanInstance::VulkanInstance(const InstanceCreateInfo& createInfo) {
  context_ = std::make_unique<vk::raii::Context>();
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
      loaderVersion < createInfo.apiVersion)
    throw std::runtime_error("Vulkan 1.4 loader required; update the Vulkan runtime");
  if (createInfo.enableValidationLayers &&
      !checkValidationLayerSupport(createInfo.validationLayers)) {
    throw std::runtime_error("validation layers requested, but not available!");
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = createInfo.applicationName.c_str();
  appInfo.applicationVersion = createInfo.applicationVersion;
  appInfo.pEngineName = createInfo.engineName.c_str();
  appInfo.engineVersion = createInfo.engineVersion;
  appInfo.apiVersion = createInfo.apiVersion;

  const uint32_t enabledLayerCount =
      createInfo.enableValidationLayers
          ? static_cast<uint32_t>(createInfo.validationLayers.size())
          : 0u;

  const char* const* enabledLayerNames =
      createInfo.enableValidationLayers ? createInfo.validationLayers.data()
                                        : nullptr;

  VkInstanceCreateInfo instanceCreateInfo{};
  instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceCreateInfo.pApplicationInfo = &appInfo;
  instanceCreateInfo.pNext = createInfo.next;
  instanceCreateInfo.enabledLayerCount = enabledLayerCount;
  instanceCreateInfo.ppEnabledLayerNames = enabledLayerNames;
  instanceCreateInfo.enabledExtensionCount =
      static_cast<uint32_t>(createInfo.requiredExtensions.size());
  instanceCreateInfo.ppEnabledExtensionNames =
      createInfo.requiredExtensions.data();

  ownedInstance_ = vk::raii::Instance(*context_,
      reinterpret_cast<const vk::InstanceCreateInfo&>(instanceCreateInfo));
  instance_ = static_cast<VkInstance>(*ownedInstance_);
}

VulkanInstance::~VulkanInstance() {
  if (instance_ != VK_NULL_HANDLE) {
    ownedInstance_.clear();
    instance_ = VK_NULL_HANDLE;
  }
}

VulkanInstance::VulkanInstance(VulkanInstance&& other) noexcept
    : context_(std::move(other.context_)), ownedInstance_(std::move(other.ownedInstance_)),
      instance_{std::exchange(other.instance_, VK_NULL_HANDLE)} {}

VulkanInstance& VulkanInstance::operator=(VulkanInstance&& other) noexcept {
  if (this != &other) {
    if (instance_ != VK_NULL_HANDLE) {
      ownedInstance_.clear();
    }
    instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
    ownedInstance_ = std::move(other.ownedInstance_);
    context_ = std::move(other.context_);
  }
  return *this;
}

bool VulkanInstance::checkValidationLayerSupport(
    const std::vector<const char*>& validationLayers) {
  uint32_t layerCount = 0;
  VkResult res = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
  if (res != VK_SUCCESS || layerCount == 0) {
    return validationLayers.empty();  // no layers available
  }

  std::vector<VkLayerProperties> availableLayers(layerCount);
  res = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
  if (res != VK_SUCCESS) {
    return false;
  }

  for (const char* layerName : validationLayers) {
    bool layerFound = false;
    for (const auto& layerProperties : availableLayers) {
      // layerName in VkLayerProperties is a null-terminated C string
      if (std::string(layerName) == std::string(layerProperties.layerName)) {
        layerFound = true;
        break;
      }
    }
    if (!layerFound) {
      return false;
    }
  }

  return true;
}

}  // namespace container::gpu
