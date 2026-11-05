#include "Container/common/CommonGLFW.h"
#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/effects/ExposureManager.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/VulkanDevice.h"
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
using namespace container::gpu;
using namespace container::renderer;

class ManagedComputeOwnerGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<ManagedComputeOwnerGpu*>(user)->errors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled = std::getenv("CONTAINER_RUN_GPU_MANAGED_COMPUTE");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_MANAGED_COMPUTE=1";
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    started = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Managed compute owners", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count{};
    const auto extensions = glfwGetRequiredInstanceExtensions(&count);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + count);
    info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(callback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR raw{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &raw), VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), raw);
    DeviceCreateInfo create;
    create.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    create.enabledFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    create.next = &v13;
    device = std::make_shared<VulkanDevice>(*instance, raw, create);
    pool = vk::raii::CommandPool(device->raii(), vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        device->queueFamilyIndices().graphicsFamily.value()));
    allocation = std::make_unique<AllocationManager>();
    allocation->initialize(instance->instance(), device->physicalDevice(), device->device(),
                           device->graphicsQueue(), *pool, {});
    pipelines = std::make_unique<PipelineManager>(device->device());
    auto commands = device->raii().allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(*pool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());
    scratch = root / ("managed-owner-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch / "spv_shaders");
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    command.clear();
    pipelines.reset();
    allocation.reset();
    pool.clear();
    device.reset();
    surface.clear();
    messenger.clear();
    instance.reset();
    if (window) glfwDestroyWindow(window);
    if (started) glfwTerminate();
    EXPECT_EQ(errors.load(), 0u);
  }
  void replacement(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
      std::ifstream input(root / "spv_shaders" / name, std::ios::binary | std::ios::ate);
      ASSERT_TRUE(input);
      const auto bytes = input.tellg();
      ASSERT_GE(bytes, std::streampos(20));
      ASSERT_EQ(static_cast<std::streamoff>(bytes) % 4, 0);
      std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
      input.seekg(0);
      input.read(reinterpret_cast<char*>(words.data()), bytes);
      ASSERT_TRUE(input);
      ASSERT_EQ(words[0], 0x07230203u);
      // Change only generator metadata. The real owner's descriptor/push
      // interface and executable instructions stay identical.
      words[2] ^= 1u;
      std::ofstream output(scratch / "spv_shaders" / name, std::ios::binary);
      output.write(reinterpret_cast<const char*>(words.data()), bytes);
      ASSERT_TRUE(output);
    }
  }
  void submit() {
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo info;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &handle;
    vk::raii::Queue queue(device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(info, nullptr);
    queue.waitIdle();
    command.reset();
  }
  std::atomic_uint errors{};
  bool started{};
  GLFWwindow* window{};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  std::unique_ptr<AllocationManager> allocation;
  std::unique_ptr<PipelineManager> pipelines;
  const std::filesystem::path root{CONTAINER_MANAGED_OWNER_SHADER_ROOT};
  std::filesystem::path scratch;
};

TEST_F(ManagedComputeOwnerGpu, BloomRetirementAndRecreationRegisterOnlyTheLiveSlots) {
  BloomManager bloom(device, *allocation, *pipelines, *pool);
  bloom.createResources(root);
  bloom.createTextures(32, 32);
  ASSERT_TRUE(bloom.isReady());
  replacement({"bloom_downsample.comp.spv", "bloom_upsample.comp.spv"});
  {
    auto abandoned = pipelines->prepareComputeReload(scratch);
    EXPECT_EQ(abandoned.size(), 2u);
    EXPECT_TRUE(abandoned.changesShader("bloom_downsample.comp.spv"));
    EXPECT_TRUE(abandoned.changesShader("bloom_upsample.comp.spv"));
  }
  auto reload = pipelines->prepareComputeReload(scratch);
  ASSERT_EQ(reload.size(), 2u);
  pipelines->publishComputeReload(reload);
  // Dispose of the old executable before retiring its borrowed layouts.
  reload = {};
  EXPECT_EQ(pipelines->prepareComputeReload(scratch).size(), 0u);
  bloom.destroy();
  EXPECT_FALSE(bloom.isReady());
  EXPECT_EQ(pipelines->prepareComputeReload(root).size(), 0u);
  bloom.createResources(root);
  bloom.createTextures(64, 32);
  ASSERT_TRUE(bloom.isReady());
  // Recreation uses the last published directory, rather than stale startup
  // bytes. Both reverse replacements must therefore be registered.
  auto reverse = pipelines->prepareComputeReload(root);
  EXPECT_EQ(reverse.size(), 2u);
  pipelines->publishComputeReload(reverse);
}

TEST_F(ManagedComputeOwnerGpu, ExposureDispatchRetainsHistogramAndAdaptationAcrossPublication) {
  ExposureManager exposure(device, *allocation, *pipelines);
  exposure.createResources(root);
  ASSERT_TRUE(exposure.isReady());
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  imageInfo.extent = {32, 32, 1};
  imageInfo.mipLevels = imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo create{};
  create.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VkImage image{};
  VmaAllocation memory{};
  ASSERT_EQ(vmaCreateImage(allocation->memoryManager()->allocator(), &imageInfo, &create,
                          &image, &memory, nullptr), VK_SUCCESS);
  struct ImageCleanup {
    VmaAllocator allocator; VkImage image; VmaAllocation memory;
    ~ImageCleanup() { vmaDestroyImage(allocator, image, memory); }
  } imageCleanup{allocation->memoryManager()->allocator(), image, memory};
  vk::ImageViewCreateInfo viewInfo({}, image, vk::ImageViewType::e2D,
      vk::Format::eR32G32B32A32Sfloat, {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
  vk::raii::ImageView view(device->raii(), viewInfo);
  command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(*command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  const VkClearColorValue color{{4.0f, 4.0f, 4.0f, 1.0f}};
  vkCmdClearColorImage(*command, image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &barrier.subresourceRange);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(*command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  submit();
  ExposureSettings settings;
  settings.mode = kExposureModeAuto;
  const auto dispatch = [&] {
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    exposure.dispatch(0, *command, *view, 32, 32, settings);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(*command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &host, 0, nullptr, 0, nullptr);
    submit();
    exposure.collectReadback(0, settings);
    EXPECT_TRUE(exposure.hasExposureDebugState());
    // Histogram bin quantization brackets the constant four-nit input.
    EXPECT_GT(exposure.averageLuminance(), 3.0f);
    EXPECT_LT(exposure.averageLuminance(), 5.0f);
    EXPECT_NEAR(exposure.resolvedExposure(settings),
                settings.targetLuminance / exposure.averageLuminance(), 1e-5f);
    return exposure.resolvedExposure(settings);
  };
  const auto initial = dispatch();
  replacement({"exposure_histogram.comp.spv", "exposure_adapt.comp.spv"});
  auto reload = pipelines->prepareComputeReload(scratch);
  ASSERT_EQ(reload.size(), 2u);
  EXPECT_FLOAT_EQ(dispatch(), initial);
  pipelines->publishComputeReload(reload);
  EXPECT_FLOAT_EQ(dispatch(), initial);
  reload = {};
  exposure.destroy();
  EXPECT_EQ(pipelines->prepareComputeReload(root).size(), 0u);
}

TEST_F(ManagedComputeOwnerGpu, EnvironmentRegistersAllSixBrdfIblAndGtaoExecutables) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  environment.createGtaoResources(root, 32, 32);
  ASSERT_TRUE(environment.isReady());
  ASSERT_TRUE(environment.isGtaoReady());
  replacement({"brdf_lut.comp.spv", "equirect_to_cubemap.comp.spv",
      "irradiance_convolution.comp.spv", "prefilter_specular.comp.spv",
      "gtao.comp.spv", "gtao_blur.comp.spv"});
  auto reload = pipelines->prepareComputeReload(scratch);
  ASSERT_EQ(reload.size(), 6u);
  EXPECT_TRUE(reload.changesShader("brdf_lut.comp.spv"));
  EXPECT_TRUE(reload.changesShader("equirect_to_cubemap.comp.spv"));
  EXPECT_TRUE(reload.changesShader("irradiance_convolution.comp.spv"));
  EXPECT_TRUE(reload.changesShader("prefilter_specular.comp.spv"));
  EXPECT_TRUE(reload.changesShader("gtao.comp.spv"));
  EXPECT_TRUE(reload.changesShader("gtao_blur.comp.spv"));
  pipelines->publishComputeReload(reload);
  reload = {};
  EXPECT_EQ(pipelines->prepareComputeReload(scratch).size(), 0u);
  environment.destroy();
  EXPECT_EQ(pipelines->prepareComputeReload(root).size(), 0u);
}
} // namespace
