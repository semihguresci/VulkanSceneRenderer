#include "Container/renderer/temporal/FsrUpscaler.h"
#include "Container/utility/VulkanInstance.h"
#include "Container/utility/VulkanMemoryManager.h"
#include "Container/utility/Platform.h"

#include <glm/gtc/packing.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::renderer;
using namespace container::gpu;

constexpr FsrUpscalerSettings settings{{64, 48}, {128, 96}, true};

struct Image {
  FsrImage resource;
  VmaAllocation allocation{};
};

class FsrVulkan : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* owner) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) {
      ++static_cast<FsrVulkan*>(owner)->errors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char* enabled = std::getenv("CONTAINER_RUN_GPU_FSR_UPSCALER");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_FSR_UPSCALER=1 to run the pinned Vulkan SDK";
    ASSERT_TRUE(FsrUpscaler::builtWithSdk());
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug{};
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                            vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);

    for (auto& candidate : instance->raii().enumeratePhysicalDevices()) {
      const auto queues = candidate.getQueueFamilyProperties();
      for (uint32_t index = 0; index < queues.size(); ++index) {
        constexpr auto flags = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
        if ((queues[index].queueFlags & flags) == flags && queues[index].timestampValidBits) {
          physical = std::move(candidate);
          family = index;
          break;
        }
      }
      if (*physical) break;
    }
    ASSERT_TRUE(bool(*physical));
    const auto extensions = physical.enumerateDeviceExtensionProperties();
    if (std::none_of(extensions.begin(), extensions.end(), [](const auto& extension) {
          return std::strcmp(extension.extensionName, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) == 0;
        }))
      GTEST_SKIP() << "FSR SDK 2.4.0 requires VK_KHR_compute_shader_derivatives";
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    v11.pNext = &derivatives;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v11};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &v12};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v13};
    vkGetPhysicalDeviceFeatures2(*physical, &features);
    if (!derivatives.computeDerivativeGroupLinear)
      GTEST_SKIP() << "FSR SDK 2.4.0 requires computeDerivativeGroupLinear";
    derivatives.computeDerivativeGroupQuads = VK_FALSE;
    ASSERT_TRUE(features.features.shaderStorageImageExtendedFormats);
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queueInfo({}, family, 1, &priority);
    const std::array enabledExtensions{VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME};
    const vk::DeviceCreateInfo deviceInfo({}, 1, &queueInfo, 0, nullptr,
        static_cast<uint32_t>(enabledExtensions.size()), enabledExtensions.data(), nullptr, &features);
    device = vk::raii::Device(physical, deviceInfo);
    queue = vk::raii::Queue(device, family, 0);
    memory = std::make_unique<VulkanMemoryManager>(instance->instance(), *physical, *device);
    pool = vk::raii::CommandPool(device, vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family));
    auto allocated = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo(*pool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(allocated.front());
    fence = vk::raii::Fence(device, vk::FenceCreateInfo{});
    timestamps = vk::raii::QueryPool(device, vk::QueryPoolCreateInfo({}, vk::QueryType::eTimestamp, 2));
    upscaler = std::make_unique<FsrUpscaler>(*physical, *device, container::util::executableDirectory());
    ASSERT_TRUE(upscaler->prepare(settings)) << upscaler->status();
    if (physical.getProperties().vendorID == 0x10de) {
      ASSERT_EQ(upscaler->status(), "FSR 3.1.6 is ready (SDK 2.4.0)");
      ASSERT_EQ(upscaler->outputFormat(), VK_FORMAT_R32G32B32A32_SFLOAT);
    }
  }

  void TearDown() override {
    if (*device) device.waitIdle();
    upscaler.reset();
    if (memory) {
      for (const auto& image : images)
        vmaDestroyImage(memory->allocator(), image.resource.image, image.allocation);
      memory->destroyBuffer(readback);
    }
    EXPECT_EQ(errors, 0u);
  }

  FsrImage image(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = usage;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    Image created{{VK_NULL_HANDLE, format, extent}, nullptr};
    const auto result = vmaCreateImage(memory->allocator(), &info, &allocation,
        &created.resource.image, &created.allocation, nullptr);
    EXPECT_EQ(result, VK_SUCCESS);
    if (result == VK_SUCCESS) images.push_back(created);
    return created.resource;
  }

  void barrier(FsrImage image, VkImageLayout before, VkImageLayout after,
               VkPipelineStageFlags2 source, VkAccessFlags2 sourceAccess,
               VkPipelineStageFlags2 destination, VkAccessFlags2 destinationAccess) {
    const VkImageMemoryBarrier2 item{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
        source, sourceAccess, destination, destinationAccess, before, after,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image.image, range};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &item;
    vkCmdPipelineBarrier2(*command, &dependency);
  }

  void clear(FsrImage image, VkClearColorValue value) {
    barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(*command, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  }

  void submit() {
    command.end();
    const VkCommandBuffer cmd = *command;
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &cmd;
    ASSERT_EQ(vkQueueSubmit(*queue, 1, &submission, *fence), VK_SUCCESS);
    upscaler->commit();
    const VkFence ready = *fence;
    ASSERT_EQ(vkWaitForFences(*device, 1, &ready, VK_TRUE, UINT64_MAX), VK_SUCCESS);
    ASSERT_EQ(vkResetFences(*device, 1, &ready), VK_SUCCESS);
  }

  static constexpr VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::PhysicalDevice physical{nullptr};
  vk::raii::Device device{nullptr};
  vk::raii::Queue queue{nullptr};
  std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  vk::raii::Fence fence{nullptr};
  vk::raii::QueryPool timestamps{nullptr};
  std::unique_ptr<FsrUpscaler> upscaler;
  std::vector<Image> images;
  AllocatedBuffer readback{};
  uint32_t family{};
  uint32_t errors{};
};

TEST(FsrUpscalerAvailability, InvalidDeviceLeavesBackendInactive) {
  FsrUpscaler upscaler(VK_NULL_HANDLE, VK_NULL_HANDLE, {});
  EXPECT_EQ(upscaler.outputFormat(), VK_FORMAT_R16G16B16A16_SFLOAT);
  EXPECT_FALSE(upscaler.prepare(settings));
  EXPECT_FALSE(upscaler.available());
  EXPECT_FALSE(upscaler.nextJitterPixels().has_value());
  EXPECT_EQ(upscaler.record({}), FsrDispatchResult::Unavailable);
  EXPECT_FALSE(upscaler.status().empty());
}

#ifdef _WIN32
class IsolatedFsrRuntime {
public:
  IsolatedFsrRuntime() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = container::util::executableDirectory() / ("fsr-runtime-test-" + std::to_string(stamp));
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Could not create isolated FSR runtime");
  }
  ~IsolatedFsrRuntime() {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(container::util::executableDirectory(), error);
    if (error) { ADD_FAILURE() << "Cannot resolve FSR test root: " << error.message(); return; }
    const auto target = std::filesystem::weakly_canonical(directory, error);
    if (error || target.parent_path() != root || !target.filename().string().starts_with("fsr-runtime-test-")) {
      ADD_FAILURE() << "Refusing to clean an invalid isolated FSR runtime path";
      return;
    }
    std::filesystem::remove_all(target, error);
    EXPECT_FALSE(error) << "Could not clean isolated FSR runtime: " << error.message();
  }
  std::filesystem::path containing(std::string_view name, std::initializer_list<const char*> modules) const {
    const auto path = directory / name;
    std::filesystem::create_directory(path);
    for (const auto module : modules)
      std::filesystem::copy_file(container::util::executableDirectory() / module, path / module);
    return path;
  }
private:
  std::filesystem::path directory;
};

TEST_F(FsrVulkan, RequiresBothRuntimeModulesAndReleasesFailedCandidateLeases) {
  upscaler.reset();
  ASSERT_EQ(GetModuleHandleW(L"amd_fidelityfx_loader_vk.dll"), nullptr);
  ASSERT_EQ(GetModuleHandleW(L"amd_fidelityfx_upscaler_vk.dll"), nullptr);
  IsolatedFsrRuntime runtime;
  const auto missingLoader = runtime.containing("missing-loader", {"amd_fidelityfx_upscaler_vk.dll"});
  const auto missingEffect = runtime.containing("missing-upscaler", {"amd_fidelityfx_loader_vk.dll"});
  for (const auto& path : {missingLoader, missingEffect}) {
    FsrUpscaler candidate(*physical, *device, path);
    EXPECT_FALSE(candidate.prepare(settings));
    EXPECT_FALSE(candidate.available());
    EXPECT_FALSE(candidate.nextJitterPixels());
    EXPECT_EQ(candidate.record({}), FsrDispatchResult::Unavailable);
  }
  ASSERT_EQ(GetModuleHandleW(L"amd_fidelityfx_loader_vk.dll"), nullptr);
  ASSERT_EQ(GetModuleHandleW(L"amd_fidelityfx_upscaler_vk.dll"), nullptr);
  const auto complete = runtime.containing("complete", {"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll"});
  {
    FsrUpscaler candidate(*physical, *device, complete);
    ASSERT_TRUE(candidate.prepare(settings)) << candidate.status();
    EXPECT_TRUE(candidate.available());
    EXPECT_TRUE(candidate.nextJitterPixels());
  }
  EXPECT_EQ(GetModuleHandleW(L"amd_fidelityfx_loader_vk.dll"), nullptr);
  EXPECT_EQ(GetModuleHandleW(L"amd_fidelityfx_upscaler_vk.dll"), nullptr);
}

TEST_F(FsrVulkan, RejectsDifferentDirectoryModulesWithoutDisturbingLiveBackend) {
  const auto before = upscaler->nextJitterPixels();
  const std::string status(upscaler->status());
  IsolatedFsrRuntime runtime;
  const auto other = runtime.containing("other-directory", {"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll"});
  {
    FsrUpscaler candidate(*physical, *device, other);
    EXPECT_FALSE(candidate.prepare(settings));
    EXPECT_FALSE(candidate.available());
  }
  EXPECT_TRUE(upscaler->available());
  EXPECT_EQ(upscaler->nextJitterPixels(), before);
  EXPECT_EQ(upscaler->status(), status);
}
#endif

TEST_F(FsrVulkan, UpscalesHdrAndRecreatesAnAbandonedRecording) {
  FsrUpscaler missing(*physical, *device, container::util::executableDirectory() / "no-fsr-sdk");
  EXPECT_FALSE(missing.prepare(settings));
  EXPECT_FALSE(missing.available());

  constexpr auto inputs = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  FsrUpscalerFrame frame{};
  frame.commands = *command;
  frame.color = image(VK_FORMAT_R16G16B16A16_SFLOAT, settings.renderExtent, inputs);
  frame.depth = image(VK_FORMAT_R32_SFLOAT, settings.renderExtent, inputs);
  frame.motion = image(VK_FORMAT_R16G16_SFLOAT, settings.renderExtent, inputs);
  frame.reactive = image(VK_FORMAT_R8_UNORM, settings.renderExtent, inputs);
  frame.transparency = image(VK_FORMAT_R8_UNORM, settings.renderExtent, inputs);
  const auto outputFormat = upscaler->outputFormat();
  ASSERT_TRUE(outputFormat == VK_FORMAT_R16G16B16A16_SFLOAT || outputFormat == VK_FORMAT_R32G32B32A32_SFLOAT);
  const VkDeviceSize outputBytes = VkDeviceSize(settings.displayExtent.width) * settings.displayExtent.height *
      (outputFormat == VK_FORMAT_R32G32B32A32_SFLOAT ? 16u : 8u);
  frame.output = image(outputFormat, settings.displayExtent,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  ASSERT_FALSE(::testing::Test::HasFailure());
  readback = memory->createBuffer(outputBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  ASSERT_NE(readback.allocation_info.pMappedData, nullptr);
  frame.cameraNear = 0.1f;
  frame.verticalFovRadians = 1.0f;
  frame.frameMilliseconds = 16.67f;
  command.begin(vk::CommandBufferBeginInfo{});
  clear(frame.color, {{0.25f, 0.5f, 2.0f, 1.0f}});
  clear(frame.depth, {{0.5f, 0, 0, 0}});
  clear(frame.motion, {});
  clear(frame.reactive, {});
  clear(frame.transparency, {});
  barrier(frame.output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  submit();
  ASSERT_FALSE(::testing::Test::HasFailure());

  std::array<uint64_t, 2> ticks{};
  const auto firstJitter = upscaler->nextJitterPixels();
  ASSERT_TRUE(firstJitter.has_value());
  for (int i = 0; i != 7; ++i) {
    command.reset();
    command.begin(vk::CommandBufferBeginInfo{});
    const auto jitter = upscaler->nextJitterPixels();
    ASSERT_TRUE(jitter.has_value());
    if (i == 1) EXPECT_NE(jitter, firstJitter);
    frame.jitterPixels = *jitter;
    auto bad = frame;
    --bad.output.extent.width;
    EXPECT_EQ(upscaler->record(bad), FsrDispatchResult::InvalidInput);
    bad = frame;
    bad.output.format = outputFormat == VK_FORMAT_R32G32B32A32_SFLOAT ?
        VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
    EXPECT_EQ(upscaler->record(bad), FsrDispatchResult::InvalidInput);
    vkCmdResetQueryPool(*command, *timestamps, 0, 2);
    vkCmdWriteTimestamp2(*command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, *timestamps, 0);
    ASSERT_EQ(upscaler->record(frame), FsrDispatchResult::Recorded) << upscaler->status();
    EXPECT_EQ(upscaler->nextJitterPixels(), jitter);
    vkCmdWriteTimestamp2(*command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, *timestamps, 1);
    barrier(frame.output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferImageCopy region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        {0, 0, 0}, {settings.displayExtent.width, settings.displayExtent.height, 1}};
    vkCmdCopyImageToBuffer(*command, frame.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    barrier(frame.output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(*command, &dependency);
    submit();
    ASSERT_FALSE(::testing::Test::HasFailure());
    ASSERT_EQ(vmaInvalidateAllocation(memory->allocator(), readback.allocation, 0, outputBytes), VK_SUCCESS);
    const auto component = [&](uint32_t index) {
      if (outputFormat == VK_FORMAT_R32G32B32A32_SFLOAT)
        return static_cast<const float*>(readback.allocation_info.pMappedData)[index];
      return glm::unpackHalf1x16(static_cast<const uint16_t*>(readback.allocation_info.pMappedData)[index]);
    };
    for (uint32_t pixel = 0; pixel < settings.displayExtent.width * settings.displayExtent.height; ++pixel) {
      ASSERT_NEAR(component(4 * pixel), 0.25f, 0.02f);
      ASSERT_NEAR(component(4 * pixel + 1), 0.5f, 0.02f);
      ASSERT_NEAR(component(4 * pixel + 2), 2.0f, 0.04f);
    }
    ASSERT_EQ(vkGetQueryPoolResults(*device, *timestamps, 0, 2, sizeof(ticks), ticks.data(),
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT), VK_SUCCESS);
    EXPECT_GT(ticks[1], ticks[0]);
  }
  std::printf("Vulkan %.*s, 64x48 to 128x96, last dispatch %.3f ms\n",
      static_cast<int>(upscaler->status().size()), upscaler->status().data(),
      (ticks[1] - ticks[0]) * physical.getProperties().limits.timestampPeriod / 1e6);
  command.reset();
  command.begin(vk::CommandBufferBeginInfo{});
  ASSERT_EQ(upscaler->record(frame), FsrDispatchResult::Recorded);
  upscaler->discard();
  command.reset();
  EXPECT_FALSE(upscaler->available());
  ASSERT_TRUE(upscaler->prepare(settings)) << upscaler->status();
  EXPECT_EQ(upscaler->outputFormat(), outputFormat);
  EXPECT_EQ(upscaler->nextJitterPixels(), firstJitter);
}

} // namespace
