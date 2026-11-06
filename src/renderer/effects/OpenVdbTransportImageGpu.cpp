#include "Container/renderer/effects/OpenVdbTransportImageGpu.h"
#include "../../geometry/OpenVdbTransportImageInternal.h"
#include "Container/utility/VulkanInstance.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <atomic>
#include <cstdio>

namespace container::renderer {
geometry::vdb::TransportImageResult RenderTransportImageGpu(
    OpenVdbTransportGpuReference &reference, const geometry::vdb::TransportScene &scene,
    const geometry::vdb::TransportPinholeCamera &camera,
    const geometry::vdb::TransportImageOptions &options,
    const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> &sources,
    uint32_t maxWorkPerPath) {
  using namespace geometry::vdb::image_detail;
  uint64_t extra = 0, words = 0;
  for (const auto &source : sources)
    if (source) {
      const auto &v = source->volume();
      words = add(words, add(v.sparseDensity.size(), v.sparseTemperature.size()));
      extra = add(extra, add(sizeof(v), add(v.gridName.size(), v.temperatureGridName.size())));
      extra = add(extra,
                  mul(add(v.sparseDensity.size(), v.sparseTemperature.size()), sizeof(uint32_t)));
      extra = add(extra, mul(add(v.density.size(), v.temperature.size()), sizeof(float)));
    }
  return render(scene, camera, options, extra, words, true, [&](const auto &rays, const auto &o) {
    TransportGpuReferenceOptions gpu;
    gpu.transport = o;
    gpu.maxWorkPerPath = maxWorkPerPath;
    return reference.trace(scene, rays, gpu, sources);
  });
}
struct TransportImageGpuSession::Impl {
  std::atomic_uint64_t errors{};
  std::unique_ptr<gpu::VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::PhysicalDevice physical{nullptr};
  vk::raii::Device device{nullptr};
  vk::raii::Queue queue{nullptr};
  std::unique_ptr<gpu::VulkanMemoryManager> memory;
  std::unique_ptr<OpenVdbTransportGpuReference> reference;
  static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                   void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<Impl *>(user)->errors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  Impl(const std::filesystem::path &shader, bool validate) {
    gpu::InstanceCreateInfo info;
    info.enableValidationLayers = validate;
    if (validate) {
      info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
      info.requiredExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    }
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = &sync;
    if (validate)
      info.next = &features;
    instance = std::make_unique<gpu::VulkanInstance>(info);
    if (validate) {
      vk::DebugUtilsMessengerCreateInfoEXT debug;
      debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
      debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                          vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
      debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validation);
      debug.pUserData = this;
      messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    }
    uint32_t family = 0;
    for (auto &candidate : instance->raii().enumeratePhysicalDevices()) {
      if (candidate.getProperties().apiVersion < VK_API_VERSION_1_4)
        continue;
      const auto supported =
          candidate.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
      if (!supported.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress)
        continue;
      const auto families = candidate.getQueueFamilyProperties();
      for (uint32_t i = 0; i < families.size(); ++i)
        if (families[i].queueCount && (families[i].queueFlags & vk::QueueFlagBits::eCompute)) {
          family = i;
          physical = std::move(candidate);
          break;
        }
      if (*physical)
        break;
    }
    if (!*physical)
      throw std::runtime_error(
          "transport image GPU requires Vulkan1.4 compute and bufferDeviceAddress");
    const float priority = 1;
    vk::DeviceQueueCreateInfo q({}, family, 1, &priority);
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    vk::DeviceCreateInfo create({}, q);
    create.pNext = &v12;
    device = vk::raii::Device(physical, create);
    queue = device.getQueue(family, 0);
    memory = std::make_unique<gpu::VulkanMemoryManager>(instance->instance(), *physical, *device);
    reference = std::make_unique<OpenVdbTransportGpuReference>(
        TransportGpuDevice{*physical, *device, *queue, family, memory.get()}, shader);
  }
  ~Impl() {
    reference.reset();
    memory.reset();
  }
};
TransportImageGpuSession::TransportImageGpuSession(const std::filesystem::path &spirv,
                                                   bool validation)
    : impl_(std::make_unique<Impl>(spirv, validation)) {}
TransportImageGpuSession::~TransportImageGpuSession() = default;
geometry::vdb::TransportImageResult TransportImageGpuSession::render(
    const geometry::vdb::TransportScene &scene, const geometry::vdb::TransportPinholeCamera &camera,
    const geometry::vdb::TransportImageOptions &options,
    const std::array<std::shared_ptr<const TransportGpuNativeSource>, 4> &sources,
    uint32_t maxWorkPerPath) {
  const auto before = impl_->errors.load();
  auto result =
      RenderTransportImageGpu(*impl_->reference, scene, camera, options, sources, maxWorkPerPath);
  if (impl_->errors.load() != before)
    throw std::runtime_error("transport image Vulkan validation failed; image invalidated");
  return result;
}
} // namespace container::renderer
