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
#include <numbers>
#include <stb_image_write.h>

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
  using CubeSamples = std::array<std::array<float, 4>, 6>;
  static CubeSamples cubeAxes(float mip = 0.0f) {
    return {{{1, 0, 0, mip}, {-1, 0, 0, mip}, {0, 1, 0, mip},
             {0, -1, 0, mip}, {0, 0, 1, mip}, {0, 0, -1, mip}}};
  }
  CubeSamples sampleCube(const EnvironmentManager& environment, VkImageView cube,
                         const CubeSamples& queries) {
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}}};
    vk::raii::DescriptorSetLayout setLayout(device->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const vk::PushConstantRange range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(queries));
    vk::raii::PipelineLayout layout(device->raii(), vk::PipelineLayoutCreateInfo({}, *setLayout, range));
    auto pipeline = pipelines->createComputePipeline(CONTAINER_ENVIRONMENT_SAMPLE_SPIRV, *layout, "environment.sample");
    const std::array<vk::DescriptorPoolSize, 3> sizes{{
        {vk::DescriptorType::eSampledImage, 1}, {vk::DescriptorType::eSampler, 1},
        {vk::DescriptorType::eStorageBuffer, 1}}};
    vk::raii::DescriptorPool descriptors(device->raii(),
        vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    auto sets = device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors, *setLayout));
    CubeSamples values{};
    auto output = allocation->createBuffer(sizeof(values), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    struct BufferCleanup {
      AllocationManager* owner; AllocatedBuffer* buffer;
      ~BufferCleanup() { owner->destroyBuffer(*buffer); }
    } cleanup{allocation.get(), &output};
    const vk::DescriptorImageInfo image({}, cube, vk::ImageLayout::eShaderReadOnlyOptimal);
    const vk::DescriptorImageInfo sampler(environment.envSampler(), {}, {});
    const vk::DescriptorBufferInfo buffer(output.buffer, 0, sizeof(values));
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {*sets.front(), 0, 0, 1, vk::DescriptorType::eSampledImage, &image},
        {*sets.front(), 1, 0, 1, vk::DescriptorType::eSampler, &sampler},
        {*sets.front(), 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffer}}};
    device->raii().updateDescriptorSets(writes, {});
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    command.bindPipeline(vk::PipelineBindPoint::eCompute, VkPipeline(pipeline));
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, *sets.front(), {});
    vkCmdPushConstants(*command, *layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(queries), queries.data());
    command.dispatch(6, 1, 1);
    vk::MemoryBarrier2 host(vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                           vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
    vk::DependencyInfo dependency; dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &host;
    command.pipelineBarrier2(dependency);
    submit();
    EXPECT_EQ(vmaInvalidateAllocation(allocation->memoryManager()->allocator(), output.allocation, 0, sizeof(values)), VK_SUCCESS);
    std::memcpy(values.data(), output.allocation_info.pMappedData, sizeof(values));
    return values;
  }
  CubeSamples sampleEnvironment(const EnvironmentManager& environment) {
    return sampleCube(environment, environment.radianceView(), cubeAxes());
  }
  std::filesystem::path writeHdr(const std::filesystem::path& name, int width, int height,
                                 const float* pixels) {
    const auto path = scratch / name;
    std::vector<char> encoded;
    const auto append = [](void* context, void* data, int count) {
      auto& bytes = *static_cast<std::vector<char>*>(context);
      const auto* first = static_cast<const char*>(data);
      bytes.insert(bytes.end(), first, first + count);
    };
    EXPECT_NE(stbi_write_hdr_to_func(append, &encoded, width, height, 3, pixels), 0);
    std::ofstream stream(path, std::ios::binary); stream.write(encoded.data(), encoded.size());
    EXPECT_TRUE(stream);
    return path;
  }
  std::filesystem::path writeHdr() {
    const std::array<float, 6> pixels{4.0f, 0.5f, 0.25f, 4.0f, 0.5f, 0.25f};
    return writeHdr(std::filesystem::path(u8"environment-\u00e7.hdr"), 2, 1, pixels.data());
  }
  std::filesystem::path writeStripedHdr() {
    // Authored angular period0.703125degrees: sharp reflections should retain
    // alternating bright/dark detail instead of aliasing it into a flat field.
    constexpr int width = 2048, height = 1024;
    std::vector<float> pixels(width * height * 3);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const size_t offset = (size_t(y) * width + x) * 3;
      pixels[offset] = x % 4 < 2 ? 4.0f : 0.125f;
      pixels[offset + 1] = .5f;
      pixels[offset + 2] = .25f;
    }
    return writeHdr("specular-stripes.hdr", width, height, pixels.data());
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

TEST_F(ManagedComputeOwnerGpu, PlainGradientAndAtmosphereGenerateActualCubeRadiance) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  SkySettings sky;
  sky.mode = SkyMode::Plain;
  sky.horizon = {.125f, .25f, .5f};
  ASSERT_TRUE(environment.loadSkyEnvironment(sky));
  const auto plain = sampleEnvironment(environment);
  for (const auto& pixel : plain) for (size_t c = 0; c < 3; ++c)
    EXPECT_NEAR(pixel[c], sky.horizon[c], 0.001f);
  // A normalized prefilter must preserve uniform RGB energy at every
  // roughness, including the sharp first and fully rough final level.
  for (uint32_t mip = 0; mip < environment.prefilteredMipCount(); ++mip) {
    SCOPED_TRACE(mip);
    const auto filtered = sampleCube(environment, environment.prefilteredView(), cubeAxes(float(mip)));
    for (const auto& pixel : filtered) for (size_t c = 0; c < 3; ++c)
      EXPECT_NEAR(pixel[c], sky.horizon[c], .002f);
  }
  const auto revision = environment.environmentRevision();
  sky.mode = SkyMode::Gradient;
  sky.zenith = {.8f, .9f, 1.0f};
  sky.ground = {.05f, .1f, .2f};
  ASSERT_TRUE(environment.loadSkyEnvironment(sky));
  EXPECT_GT(environment.environmentRevision(), revision);
  const auto gradient = sampleEnvironment(environment);
  for (size_t c = 0; c < 3; ++c) {
    EXPECT_NEAR(gradient[0][c], sky.horizon[c], .005f);
    EXPECT_NEAR(gradient[2][c], sky.zenith[c], .005f);
    EXPECT_NEAR(gradient[3][c], sky.ground[c], .005f);
  }
  sky.mode = SkyMode::Atmosphere;
  sky.atmosphere.multipleScattering = true;
  sky.atmosphere.localSun = {.enabled=true, .latitudeDegrees=40, .longitudeDegrees=0,
      .year=2026, .month=6, .day=21, .hour=12};
  ASSERT_TRUE(environment.loadSkyEnvironment(sky));
  const auto atmosphere = sampleEnvironment(environment);
  for (const auto& pixel : atmosphere) for (size_t c = 0; c < 3; ++c) {
    EXPECT_TRUE(std::isfinite(pixel[c]));
    EXPECT_GE(pixel[c], 0.0f);
  }
  EXPECT_GT(atmosphere[2][2], atmosphere[2][0]);
  EXPECT_EQ(environment.activeSkyMode(), SkyMode::Atmosphere);
  const auto& sun = environment.activeSun().direction;
  EXPECT_NEAR(sun[0]*sun[0]+sun[1]*sun[1]+sun[2]*sun[2], 1.0f, 1e-5f);
  float scattering = 0;
  for (const auto& texel : environment.activeAtmosphereLut()) scattering += texel[0]+texel[1]+texel[2];
  EXPECT_GT(scattering, 0.0f);
}

TEST_F(ManagedComputeOwnerGpu, HdrLongitudeWrapsAndLatitudeClampsInActualCubeRadiance) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  constexpr int width = 8, height = 4;
  constexpr std::array<float, height> greenByRow{.875f, .5f, .5f, .125f};
  std::array<float, width * height * 3> pixels{};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto index = (y * width + x) * 3;
      pixels[index] = x < width / 2 ? 1.0f : 3.0f;
      pixels[index + 1] = greenByRow[y];
      pixels[index + 2] = .25f;
    }
  }
  ASSERT_TRUE(environment.loadHdrEnvironment(writeHdr("seam.hdr", width, height, pixels.data())));

  auto queries = cubeAxes();
  constexpr float offset = .125f;
  queries[4] = {-1, 0, offset, 0};
  queries[5] = {-1, 0, -offset, 0};
  const auto radiance = sampleCube(environment, environment.radianceView(), queries);

  // +X straddles the interior red transition. At -X both sides must blend
  // the endpoint columns over the same half-texel interval in longitude.
  const float redDelta = width * std::atan(offset) / std::numbers::pi_v<float>;
  EXPECT_NEAR(radiance[0][0], 2.0f, .08f);
  EXPECT_NEAR(radiance[1][0], 2.0f, .08f);
  EXPECT_NEAR(radiance[4][0], 2.0f + redDelta, .08f);
  EXPECT_NEAR(radiance[5][0], 2.0f - redDelta, .08f);
  EXPECT_NEAR(radiance[2][1], .875f, .005f);
  EXPECT_NEAR(radiance[3][1], .125f, .005f);
  for (const auto& pixel : radiance)
    EXPECT_NEAR(pixel[2], .25f, .005f);
}

TEST_F(ManagedComputeOwnerGpu, SmoothSpecularReflectionsResolveAuthoredAngularDetailAndRoughnessFiltersIt) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  ASSERT_TRUE(environment.loadHdrEnvironment(writeStripedHdr()));
  CubeSamples queries{};
  constexpr double pi = 3.14159265358979323846;
  for (size_t i = 0; i < queries.size(); ++i) {
    const double u = (1024.5 + double(i) * 2.0) / 2048.0;
    const double phi = (u - .5) * (2.0 * pi);
    queries[i] = {float(std::cos(phi)), 0.0f, float(std::sin(phi)), 0.0f};
  }
  const auto radiance = sampleCube(environment, environment.radianceView(), queries);
  const auto sharp = sampleCube(environment, environment.prefilteredView(), queries);
  for (size_t i = 0; i < queries.size(); ++i) {
    SCOPED_TRACE(i);
    // At zero roughness the specular lobe is the actual reflected direction,
    // not a diffuse average or an artificially softened source mip.
    for (size_t c = 0; c < 3; ++c)
      EXPECT_NEAR(sharp[i][c], radiance[i][c], .025f);
    EXPECT_NEAR(sharp[i][1], .5f, .002f);
    EXPECT_NEAR(sharp[i][2], .25f, .002f);
  }
  for (size_t i = 0; i < queries.size(); i += 2)
    EXPECT_GT(sharp[i][0] - sharp[i + 1][0], 1.5f) << i;
  for (auto& query : queries) query[3] = float(environment.prefilteredMipCount() - 1);
  const auto rough = sampleCube(environment, environment.prefilteredView(), queries);
  for (size_t i = 0; i < queries.size(); i += 2)
    EXPECT_LT(std::abs(rough[i][0] - rough[i + 1][0]), .1f) << i;
  for (const auto& pixel : rough) {
    EXPECT_NEAR(pixel[1], .5f, .002f);
    EXPECT_NEAR(pixel[2], .25f, .002f);
    for (const float value : pixel) EXPECT_TRUE(std::isfinite(value));
  }
}

TEST_F(ManagedComputeOwnerGpu, AsyncHdrAndFailedPublicationPreserveTheActiveGeneration) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  const auto previous = environment.radianceView();
  const auto revision = environment.environmentRevision();
  const auto path = writeHdr();
  ASSERT_TRUE(environment.requestHdrEnvironment(path));
  ASSERT_TRUE(environment.environmentLoading());
  EXPECT_EQ(environment.radianceView(), previous);
  EXPECT_EQ(environment.environmentRevision(), revision);
  SkySettings sky; sky.mode = SkyMode::Plain;
  EXPECT_FALSE(environment.requestSkyEnvironment(sky));
  uint32_t retired = 0;
  ASSERT_TRUE(environment.pollEnvironment(true, [&] {
    EXPECT_EQ(environment.radianceView(), previous);
    EXPECT_EQ(environment.environmentRevision(), revision);
    device->raii().waitIdle(); ++retired;
  }));
  EXPECT_EQ(retired, 1u);
  EXPECT_FALSE(environment.environmentLoading());
  EXPECT_EQ(environment.activeHdrPath(), path);
  EXPECT_EQ(environment.activeSkyMode(), SkyMode::Hdr);
  const auto hdr = sampleEnvironment(environment);
  for (const auto& pixel : hdr) {
    EXPECT_NEAR(pixel[0], 4.0f, .01f);
    EXPECT_NEAR(pixel[1], .5f, .002f);
    EXPECT_NEAR(pixel[2], .25f, .001f);
  }
  const auto active = environment.radianceView();
  const auto activeRevision = environment.environmentRevision();
  const auto invalid = scratch / "broken.hdr";
  { std::ofstream stream(invalid); stream << "#?RADIANCE\ntruncated"; }
  ASSERT_TRUE(environment.requestHdrEnvironment(invalid));
  EXPECT_FALSE(environment.pollEnvironment(true, {}));
  EXPECT_FALSE(environment.environmentLoading());
  EXPECT_EQ(environment.radianceView(), active);
  EXPECT_EQ(environment.environmentRevision(), activeRevision);
  ASSERT_TRUE(environment.requestSkyEnvironment(sky));
  EXPECT_FALSE(environment.pollEnvironment(true, [] { throw std::runtime_error("retirement failed"); }));
  EXPECT_FALSE(environment.environmentLoading());
  EXPECT_EQ(environment.radianceView(), active);
  EXPECT_EQ(environment.environmentRevision(), activeRevision);
  EXPECT_EQ(sampleEnvironment(environment), hdr);
}

TEST_F(ManagedComputeOwnerGpu, RetainedHdrBytesRebakeAfterFileRemovalAndStaleCandidatesAreRejected) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  const auto path = writeHdr();
  ASSERT_TRUE(environment.loadHdrEnvironment(path));
  const auto original = sampleEnvironment(environment);
  ASSERT_EQ(path.parent_path(), scratch);
  ASSERT_TRUE(std::filesystem::remove(path));
  auto bake = environment.prepareShaderBake(root, true, true);
  ASSERT_TRUE(bake);
  ASSERT_TRUE(environment.canPublishShaderBake(*bake));
  const auto revision = environment.environmentRevision();
  environment.publishShaderBake(*bake);
  EXPECT_GT(environment.environmentRevision(), revision);
  EXPECT_FALSE(environment.canPublishShaderBake(*bake));
  EXPECT_EQ(sampleEnvironment(environment), original);
  bake.reset();
  auto stale = environment.prepareShaderBake(root, false, true);
  ASSERT_TRUE(stale);
  SkySettings sky; sky.mode = SkyMode::Plain; sky.horizon = {.25f, .5f, .75f};
  ASSERT_TRUE(environment.loadSkyEnvironment(sky));
  EXPECT_FALSE(environment.canPublishShaderBake(*stale));
  const auto changed = sampleEnvironment(environment);
  stale.reset();
  EXPECT_EQ(sampleEnvironment(environment), changed);
}

TEST_F(ManagedComputeOwnerGpu, BrdfOnlyPublicationKeepsRadianceRevisionAndRejectsOlderCandidates) {
  EnvironmentManager environment(device, *allocation, *pipelines, *pool);
  environment.createResources(root);
  SkySettings sky; sky.mode = SkyMode::Plain;
  ASSERT_TRUE(environment.loadSkyEnvironment(sky));
  auto older = environment.prepareShaderBake(root, false, true);
  auto brdf = environment.prepareShaderBake(root, true, false);
  ASSERT_TRUE(older && brdf);
  ASSERT_TRUE(environment.canPublishShaderBake(*brdf));
  const auto revision = environment.environmentRevision();
  const auto radiance = environment.radianceView();
  environment.publishShaderBake(*brdf);
  EXPECT_EQ(environment.environmentRevision(), revision);
  EXPECT_EQ(environment.radianceView(), radiance);
  EXPECT_FALSE(environment.canPublishShaderBake(*older));
}

TEST_F(ManagedComputeOwnerGpu, StagedCommandExceptionsRetireGpuWorkAndBlockReentrantAllocatorReset) {
  auto buffer = allocation->createBuffer(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  struct Cleanup {
    AllocationManager* owner; AllocatedBuffer* buffer;
    ~Cleanup() { owner->destroyBuffer(*buffer); }
  } cleanup{allocation.get(), &buffer};
  uint32_t initial = 17;
  std::memcpy(buffer.allocation_info.pMappedData, &initial, sizeof(initial));
  ASSERT_EQ(vmaFlushAllocation(allocation->memoryManager()->allocator(), buffer.allocation, 0, 4), VK_SUCCESS);
  EXPECT_THROW(allocation->executeStagedCommands({}), std::invalid_argument);
  EXPECT_THROW(allocation->executeStagedCommands([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, buffer.buffer, 0, 4, 29);
    throw std::runtime_error("recording failed");
  }), std::runtime_error);
  uint32_t readback{};
  ASSERT_EQ(vmaInvalidateAllocation(allocation->memoryManager()->allocator(), buffer.allocation, 0, 4), VK_SUCCESS);
  std::memcpy(&readback, buffer.allocation_info.pMappedData, 4);
  EXPECT_EQ(readback, 17u); // The discarded recording was never submitted.
  uint32_t completed = 0;
  allocation->setLoadProgressCallback([&](bool finished) {
    if (!finished) return;
    ++completed;
    EXPECT_THROW(allocation->resetTextureAllocations(), std::logic_error);
    EXPECT_THROW(allocation->resetGeometryAllocations(), std::logic_error);
    EXPECT_THROW(allocation->cleanup(), std::logic_error);
    throw std::runtime_error("progress callback failed");
  });
  EXPECT_THROW(allocation->executeStagedCommands([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, buffer.buffer, 0, 4, 29);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &host, 0, nullptr, 0, nullptr);
  }), std::runtime_error);
  allocation->setLoadProgressCallback({});
  EXPECT_EQ(completed, 1u);
  ASSERT_EQ(vmaInvalidateAllocation(allocation->memoryManager()->allocator(), buffer.allocation, 0, 4), VK_SUCCESS);
  std::memcpy(&readback, buffer.allocation_info.pMappedData, 4);
  EXPECT_EQ(readback, 29u);
  EXPECT_NO_THROW(allocation->executeStagedCommands([](VkCommandBuffer) {}));
}
} // namespace
