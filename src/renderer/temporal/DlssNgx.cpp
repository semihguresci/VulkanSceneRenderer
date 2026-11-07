#include "DlssNgx.h"

#if CONTAINER_HAS_DLSS_SDK
#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace container::renderer::ngx {

std::mutex mutex;

namespace {
constexpr char projectId[] = "fe8aa4de-ad01-43f8-841b-7192ac6401de";
constexpr char engineVersion[] = "1.0";
constexpr auto engineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;

void checkVk(VkResult result, std::string_view operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::format("{} failed (Vulkan {})", operation, static_cast<int>(result)));
}

struct Location {
  explicit Location(Feature feature, const std::filesystem::path& runtimeDirectory) {
    if (runtimeDirectory.empty()) throw std::runtime_error("DLSS runtime directory is unset");
    runtime = std::filesystem::absolute(runtimeDirectory).lexically_normal();
    const auto* dll = feature == Feature::RayReconstruction ? "nvngx_dlssd.dll" : "nvngx_dlss.dll";
    if (!std::filesystem::is_regular_file(runtime / dll))
      throw std::runtime_error(std::format("The pinned {} is unavailable beside the executable", dll));
    const auto data = std::filesystem::temp_directory_path() / "VulkanSceneRenderer" / "ngx";
    std::filesystem::create_directories(data);
    runtimePath = runtime.wstring();
    dataPath = data.wstring();
    searchPath = runtimePath.c_str();
    common.PathListInfo = {&searchPath, 1};
  }

  [[nodiscard]] NVSDK_NGX_FeatureDiscoveryInfo discovery(Feature feature) const {
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = feature == Feature::RayReconstruction
        ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc = {projectId, engineType, engineVersion};
    info.ApplicationDataPath = dataPath.c_str();
    info.FeatureInfo = &common;
    return info;
  }

  std::filesystem::path runtime;
  std::wstring runtimePath, dataPath;
  const wchar_t* searchPath{};
  NVSDK_NGX_FeatureCommonInfo common{};
};

struct SharedSession {
  VkDevice device{};
  std::unique_ptr<Location> location;
  uint32_t users{};
};
SharedSession shared;

template<class Query>
DlssExtensionRequirements discoverExtensions(Feature feature, const std::filesystem::path& runtimeDirectory,
    Query query) {
  try {
    const Location location(feature, runtimeDirectory);
    const auto discovery = location.discovery(feature);
    uint32_t count{};
    VkExtensionProperties* extensions{};
    std::scoped_lock lock(mutex);
    check(query(&discovery, &count, &extensions), "NGX Vulkan extension discovery");
    if (count && !extensions) throw std::runtime_error("NGX returned no Vulkan extension properties");
    DlssExtensionRequirements result;
    result.names.reserve(count);
    for (uint32_t i = 0; i < count; ++i) result.names.emplace_back(extensions[i].extensionName);
    return result;
  } catch (const std::exception& error) {
    return {.status = error.what()};
  }
}
} // namespace

void check(NVSDK_NGX_Result result, std::string_view operation) {
  if (NVSDK_NGX_SUCCEED(result)) return;
  const std::wstring_view description = GetNGXResultAsString(result);
  std::string readable(description.size(), '\0');
  std::ranges::transform(description, readable.begin(), [](wchar_t c) { return static_cast<char>(c); });
  throw std::runtime_error(std::format("{} failed ({}, {:#x})", operation, readable,
      static_cast<unsigned int>(result)));
}

bool nvidiaAdapter(VkPhysicalDevice physicalDevice) {
  if (!physicalDevice) return false;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(physicalDevice, &properties);
  return properties.vendorID == 0x10de;
}

NVSDK_NGX_PerfQuality_Value quality(UpscaleMode mode) {
  switch (mode) {
    case UpscaleMode::Native: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case UpscaleMode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case UpscaleMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case UpscaleMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
  }
  throw std::invalid_argument("Invalid NVIDIA DLSS mode");
}

DlssExtensionRequirements queryInstanceExtensions(Feature feature, const std::filesystem::path& runtimeDirectory) {
  return discoverExtensions(feature, runtimeDirectory, NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements);
}

DlssExtensionRequirements queryDeviceExtensions(Feature feature, VkInstance instance, VkPhysicalDevice physicalDevice,
    const std::filesystem::path& runtimeDirectory) {
  if (!instance || !nvidiaAdapter(physicalDevice))
    return {.status = "DLSS requires a supported NVIDIA Vulkan adapter"};
  return discoverExtensions(feature, runtimeDirectory, [=](const auto* info, auto* count, auto** extensions) {
    return NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physicalDevice, info, count, extensions);
  });
}

bool imageMatches(const DlssImage& resource, VkFormat format, VkExtent2D extent) noexcept {
  return resource.image && resource.view && resource.format == format && sameExtent(resource.extent, extent);
}

NVSDK_NGX_Resource_VK image(const DlssImage& resource, bool storage) {
  return NVSDK_NGX_Create_ImageView_Resource_VK(resource.view, resource.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      resource.format, resource.extent.width, resource.extent.height, storage);
}

Session::~Session() {
  if (!device_) return;
  std::scoped_lock lock(mutex);
  if (parameters_) NVSDK_NGX_VULKAN_DestroyParameters(parameters_);
  if (--shared.users == 0) {
    NVSDK_NGX_VULKAN_Shutdown1(device_);
    shared = {};
  }
}

void Session::initialize(Feature feature, VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
    const std::filesystem::path& runtimeDirectory) {
  auto location = std::make_unique<Location>(feature, runtimeDirectory);
  std::scoped_lock lock(mutex);
  if (shared.users) {
    if (shared.device != device || shared.location->runtime != location->runtime)
      throw std::runtime_error("Another Vulkan device or runtime directory owns the NVIDIA NGX session");
  } else {
    check(NVSDK_NGX_VULKAN_Init_with_ProjectID(projectId, engineType, engineVersion, location->dataPath.c_str(),
        instance, physicalDevice, device, rr_clear_sync::instanceProcAddr, rr_clear_sync::deviceProcAddr, &location->common),
        "NVIDIA NGX Vulkan initialization");
    shared.device = device;
    shared.location = std::move(location);
  }
  device_ = device;
  ++shared.users;
  check(NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters_), "DLSS capability query");
  if (!parameters_) throw std::runtime_error("NGX returned no DLSS capability parameters");
  parameters_->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
}

FeatureCommands::~FeatureCommands() {
  if (fence) vkDestroyFence(device, fence, nullptr);
  if (pool) vkDestroyCommandPool(device, pool, nullptr);
}

VkCommandBuffer FeatureCommands::begin(uint32_t queueFamily) {
  const VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
      VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queueFamily};
  checkVk(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "DLSS command pool creation");
  const VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
      pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{};
  checkVk(vkAllocateCommandBuffers(device, &allocation, &command), "DLSS command buffer allocation");
  const VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  checkVk(vkBeginCommandBuffer(command, &beginInfo), "DLSS feature command buffer begin");
  return command;
}

void FeatureCommands::submit(VkQueue queue, VkCommandBuffer command) {
  checkVk(vkEndCommandBuffer(command), "DLSS feature command buffer end");
  const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  checkVk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "DLSS feature fence creation");
  const VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command};
  checkVk(vkQueueSubmit(queue, 1, &submitInfo, fence), "DLSS feature initialization submission");
  const auto waited = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) vkQueueWaitIdle(queue);
  checkVk(waited, "DLSS feature initialization wait");
}

} // namespace container::renderer::ngx
#endif
