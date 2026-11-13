#include "Container/utility/VulkanInstance.h"
#include "Container/renderer/pathtracing/ReferenceDlssReconstructor.h"
#include "Container/renderer/temporal/DlssUpscaler.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::gpu {
namespace {

bool hasExtension(const std::vector<const char*>& extensions,
                  std::string_view name) {
  return std::ranges::any_of(extensions, [name](const char* extension) {
    return extension != nullptr && name == extension;
  });
}

void addSurfaceMaintenance1Extensions(std::vector<const char*>& enabled) {
  if (!hasExtension(enabled, VK_KHR_SURFACE_EXTENSION_NAME)) return;

  uint32_t count = 0;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
    return;
  std::vector<VkExtensionProperties> available(count);
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()) != VK_SUCCESS)
    return;
  available.resize(count);

  const auto supported = [&available, &enabled](std::string_view name) {
    return hasExtension(enabled, name) ||
           std::ranges::any_of(available, [name](const auto& extension) {
             return name == extension.extensionName;
           });
  };
  if (!supported(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) return;

  const auto enable = [&enabled](const char* name) {
    if (!hasExtension(enabled, name)) enabled.push_back(name);
  };
  // The device may expose only one spelling. Enable both available instance
  // prerequisites so device creation can choose a matching extension.
  const auto enableMaintenance = [&supported, &enable](const char* name) {
    if (supported(name)) {
      enable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
      enable(name);
    }
  };
#ifdef VK_KHR_surface_maintenance1
  enableMaintenance(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
  enableMaintenance(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
}

std::string addDlssInstanceExtensions(std::vector<const char*>& enabled,
                                      const std::vector<std::string>& required) {
  if (required.empty()) return {};

  uint32_t count = 0;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS)
    return "Could not enumerate Vulkan instance extensions for DLSS";
  std::vector<VkExtensionProperties> available(count);
  if (vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()) != VK_SUCCESS)
    return "Could not enumerate Vulkan instance extensions for DLSS";

  for (const auto& name : required) {
    if (!hasExtension(enabled, name) &&
        std::ranges::none_of(available, [&name](const VkExtensionProperties& extension) {
          return name == extension.extensionName;
        })) {
      return "DLSS requires Vulkan instance extension " + name;
    }
  }
  for (const auto& name : required) {
    if (!hasExtension(enabled, name)) enabled.push_back(name.c_str());
  }
  return {};
}

}  // namespace

VulkanInstance::VulkanInstance(const InstanceCreateInfo& createInfo) {
  context_ = std::make_unique<vk::raii::Context>();
  uint32_t loaderVersion = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS ||
      loaderVersion < createInfo.apiVersion)
    throw std::runtime_error("Vulkan 1.4 loader required; update the Vulkan runtime");
  auto layers = createInfo.additionalLayers;
  if (createInfo.enableValidationLayers)
    layers.insert(layers.end(), createInfo.validationLayers.begin(), createInfo.validationLayers.end());
  if (!layers.empty() && !checkValidationLayerSupport(layers))
    throw std::runtime_error("Requested Vulkan instance layer unavailable; check validation installation or GFXReconstruct layer path");

  auto extensions = createInfo.requiredExtensions;
  if (createInfo.enableSurfaceMaintenance1)
    addSurfaceMaintenance1Extensions(extensions);
#ifdef VK_KHR_surface_maintenance1
  khrSurfaceMaintenance1Enabled_ =
      hasExtension(extensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
#endif
  extSurfaceMaintenance1Enabled_ =
      hasExtension(extensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
  renderer::DlssExtensionRequirements dlssExtensions;
  renderer::DlssExtensionRequirements dlssRrExtensions;
  if (createInfo.enableDlssUpscaling) {
    dlssExtensions = renderer::DlssUpscaler::queryInstanceExtensions(util::executableDirectory());
    dlssUpscalingStatus_ = dlssExtensions.status.empty()
        ? addDlssInstanceExtensions(extensions, dlssExtensions.names) : std::move(dlssExtensions.status);
  }
  if (createInfo.enableDlssRayReconstruction) {
    dlssRrExtensions = renderer::ReferenceDlssReconstructor::queryInstanceExtensions(util::executableDirectory());
    dlssRayReconstructionStatus_ = dlssRrExtensions.status.empty()
        ? addDlssInstanceExtensions(extensions, dlssRrExtensions.names) : std::move(dlssRrExtensions.status);
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = createInfo.applicationName.c_str();
  appInfo.applicationVersion = createInfo.applicationVersion;
  appInfo.pEngineName = createInfo.engineName.c_str();
  appInfo.engineVersion = createInfo.engineVersion;
  appInfo.apiVersion = createInfo.apiVersion;

  const uint32_t enabledLayerCount = static_cast<uint32_t>(layers.size());
  const char* const* enabledLayerNames = layers.empty() ? nullptr : layers.data();

  VkInstanceCreateInfo instanceCreateInfo{};
  instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceCreateInfo.pApplicationInfo = &appInfo;
  instanceCreateInfo.pNext = createInfo.next;
  instanceCreateInfo.enabledLayerCount = enabledLayerCount;
  instanceCreateInfo.ppEnabledLayerNames = enabledLayerNames;
  instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  instanceCreateInfo.ppEnabledExtensionNames = extensions.data();

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
      instance_{std::exchange(other.instance_, VK_NULL_HANDLE)},
      dlssUpscalingStatus_(std::move(other.dlssUpscalingStatus_)),
      dlssRayReconstructionStatus_(std::move(other.dlssRayReconstructionStatus_)),
      khrSurfaceMaintenance1Enabled_(std::exchange(other.khrSurfaceMaintenance1Enabled_, false)),
      extSurfaceMaintenance1Enabled_(std::exchange(other.extSurfaceMaintenance1Enabled_, false)) {}

VulkanInstance& VulkanInstance::operator=(VulkanInstance&& other) noexcept {
  if (this != &other) {
    if (instance_ != VK_NULL_HANDLE) {
      ownedInstance_.clear();
    }
    instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
    ownedInstance_ = std::move(other.ownedInstance_);
    context_ = std::move(other.context_);
    dlssUpscalingStatus_ = std::move(other.dlssUpscalingStatus_);
    dlssRayReconstructionStatus_ = std::move(other.dlssRayReconstructionStatus_);
    khrSurfaceMaintenance1Enabled_ = std::exchange(other.khrSurfaceMaintenance1Enabled_, false);
    extSurfaceMaintenance1Enabled_ = std::exchange(other.extSurfaceMaintenance1Enabled_, false);
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
