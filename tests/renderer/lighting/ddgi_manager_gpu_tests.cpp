#include "Container/common/CommonGLFW.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/debug/DdgiDebugRenderer.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::renderer;
using namespace container::gpu;

struct UploadBuffer {
  VulkanMemoryManager &memory;
  AllocatedBuffer value;
  size_t size;
  UploadBuffer(VulkanMemoryManager &m, size_t bytes, VkBufferUsageFlags usage)
      : memory(m), value(m.createBuffer(
                       bytes, usage, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                       VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT)),
        size(bytes) {}
  ~UploadBuffer() { memory.destroyBuffer(value); }
  void write(const void *source, size_t bytes) {
    if (bytes > size || !value.allocation_info.pMappedData)
      throw std::runtime_error("invalid DDGI manager test upload");
    std::memcpy(value.allocation_info.pMappedData, source, bytes);
    if (vmaFlushAllocation(memory.allocator(), value.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI manager test flush failed");
  }
};

// Exercise the production owner and both production kernels with a real
// ray-query scene. Geometry is beyond maxDistance, so every probe samples the
// initialized environment. Live edits run across two retired image slots.
class DdgiManagerGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<DdgiManagerGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char *enabled = std::getenv("CONTAINER_RUN_GPU_DDGI");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP()
          << "Set CONTAINER_RUN_GPU_DDGI=1 for live DDGI manager tests";
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Live DDGI settings", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count{};
    const auto *extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + count);
    info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback =
        reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(
            validationCallback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR handle{};
    ASSERT_EQ(
        glfwCreateWindowSurface(instance->instance(), window, nullptr, &handle),
        VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), handle);
    DeviceCreateInfo deviceInfo;
    deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    deviceInfo.enableRayQueries = true;
    deviceInfo.enabledFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan11Features v11{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    v11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.separateDepthStencilLayouts = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    v12.pNext = &v11;
    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.dynamicRendering = VK_TRUE;
    v13.shaderDemoteToHelperInvocation = VK_TRUE;
    v13.pNext = &v12;
    deviceInfo.next = &v13;
    device = std::make_shared<VulkanDevice>(*instance, handle, deviceInfo);
    if (!device->rayQueriesEnabled())
      GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
    memory = std::make_unique<VulkanMemoryManager>(
        instance->instance(), device->physicalDevice(), device->device());
    pool = vk::raii::CommandPool(
        device->raii(),
        vk::CommandPoolCreateInfo(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            device->queueFamilyIndices().graphicsFamily.value()));
    auto commands =
        device->raii().allocateCommandBuffers(vk::CommandBufferAllocateInfo(
            *pool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());
    createImages();
    createSceneDescriptors();
    rayScene = std::make_unique<RayShadowManager>(device, *memory);
    rayScene->createPipelines(CONTAINER_DDGI_SHADER_ROOT, *sceneLayout,
                              *lightLayout);
    rayScene->prepare({32, 32}, 2);
    vertices.resize(3);
    vertices[0].position = {-1, -1, 100};
    vertices[1].position = {1, -1, 100};
    vertices[2].position = {0, 1, 100};
    for (auto &vertex : vertices) {
      vertex.normal = {0, 0, 1};
      vertex.tangent = {1, 0, 0, 1};
      vertex.color = {1, 1, 1};
    }
    indices = {0, 1, 2};
    objects.resize(1);
    draws = {{0, 0, 3}};
    config.volume = {{0, 0, 0}, {1, 1, 1}, {4, 1, 1}};
    config.maxDistance = 10;
    config.resources.irradianceInteriorTexels = 2;
    config.resources.visibilityInteriorTexels = 2;
    config.resources.updates = {2, 128, 256};
    manager = std::make_unique<DdgiManager>(device, *memory, *rayScene, config);
    if (!manager->supported())
      GTEST_SKIP() << manager->status();
    manager->createPipelines(CONTAINER_DDGI_SHADER_ROOT, *sceneLayout,
                             *lightLayout);
    manager->prepare(2, *environmentView, *sampler);
  }

  void createImages() {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = 6;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &alloc, &environment,
                             &environmentAllocation, nullptr),
              VK_SUCCESS);
    vk::ImageViewCreateInfo view;
    view.image = environment;
    view.format = vk::Format::eR8G8B8A8Unorm;
    view.viewType = vk::ImageViewType::eCube;
    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6};
    environmentView = vk::raii::ImageView(device->raii(), view);
    view.viewType = vk::ImageViewType::e2D;
    view.subresourceRange.layerCount = 1;
    textureView = vk::raii::ImageView(device->raii(), view);
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter = samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.addressModeU = samplerInfo.addressModeV =
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler = vk::raii::Sampler(device->raii(), samplerInfo);
    command.begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::ImageMemoryBarrier2 barrier;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = environment;
    barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6};
    vk::DependencyInfo dependency;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    command.pipelineBarrier2(dependency);
    command.clearColorImage(
        environment, vk::ImageLayout::eTransferDstOptimal,
        vk::ClearColorValue(std::array<float, 4>{.25f, .5f, 1.f, 1.f}),
        barrier.subresourceRange);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    command.pipelineBarrier2(dependency);
    submit();
    info.flags = 0;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {32, 32, 1};
    info.arrayLayers = 1;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &alloc, &depth,
                             &depthAllocation, nullptr),
              VK_SUCCESS);
    view.image = depth;
    view.format = vk::Format::eD32Sfloat;
    view.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    depthView = vk::raii::ImageView(device->raii(), view);
    frame.depthStencil = {depth, depthAllocation, *depthView,
                          VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT};
    frame.depthSamplingView = *depthView;
  }

  void createSceneDescriptors() {
    const std::array sceneBindings{
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampler, 9,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(5, vk::DescriptorType::eSampledImage,
                                       4096,
                                       vk::ShaderStageFlagBits::eCompute)};
    sceneLayout = vk::raii::DescriptorSetLayout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, sceneBindings));
    const std::array lightBindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eUniformBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute)};
    lightLayout = vk::raii::DescriptorSetLayout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, lightBindings));
    const std::array sizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 9),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 4096),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 4),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1)};
    descriptors = vk::raii::DescriptorPool(
        device->raii(),
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 2, sizes));
    const std::array layouts{*sceneLayout, *lightLayout};
    sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptors, layouts));
    const GpuMaterial material;
    const GpuTextureMetadata metadata;
    const PointLightData point;
    const AreaLightData area;
    auto makeBuffer = [&](const auto &data, bool uniform = false) {
      auto buffer = std::make_unique<UploadBuffer>(
          *memory, sizeof(data),
          uniform ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                  : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      buffer->write(&data, sizeof(data));
      return buffer;
    };
    uploads.push_back(makeBuffer(material));
    uploads.push_back(makeBuffer(metadata));
    uploads.push_back(makeBuffer(lighting, true));
    uploads.push_back(makeBuffer(point));
    uploads.push_back(makeBuffer(area));
    for (uint32_t i = 0; i < uploads.size(); ++i) {
      vk::DescriptorBufferInfo buffer(uploads[i]->value.buffer, 0,
                                      uploads[i]->size);
      device->raii().updateDescriptorSets(
          vk::WriteDescriptorSet(*sets[i < 2 ? 0 : 1], i < 2 ? i + 3 : i - 2, 0,
                                 1,
                                 i == 2 ? vk::DescriptorType::eUniformBuffer
                                        : vk::DescriptorType::eStorageBuffer,
                                 nullptr, &buffer),
          {});
    }
    std::vector<vk::DescriptorImageInfo> images(
        4096, vk::DescriptorImageInfo({}, *textureView,
                                      vk::ImageLayout::eShaderReadOnlyOptimal));
    device->raii().updateDescriptorSets(
        vk::WriteDescriptorSet(*sets[0], 5, 0, uint32_t(images.size()),
                               vk::DescriptorType::eSampledImage,
                               images.data()),
        {});
    const std::array<vk::DescriptorImageInfo, 9> samplers{
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {}),
        vk::DescriptorImageInfo(*sampler, {}, {})};
    device->raii().updateDescriptorSets(
        vk::WriteDescriptorSet(*sets[0], 2, 0, uint32_t(samplers.size()),
                               vk::DescriptorType::eSampler, samplers.data()),
        {});
  }

  void submit(bool wait = true, bool ddgiFrame = false) {
    command.end();
    const VkCommandBufferSubmitInfo bufferInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, *command, 0};
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    info.commandBufferInfoCount = 1;
    info.pCommandBufferInfos = &bufferInfo;
    std::optional<VkSemaphoreSubmitInfo> readWait, frameSignal;
    if (manager) {
      readWait = manager->graphicsWait();
      if (ddgiFrame) frameSignal = manager->graphicsSignal();
    }
    if (readWait) {
      info.waitSemaphoreInfoCount = 1;
      info.pWaitSemaphoreInfos = &*readWait;
    }
    if (frameSignal) {
      info.signalSemaphoreInfoCount = 1;
      info.pSignalSemaphoreInfos = &*frameSignal;
    }
    vk::raii::Queue queue(
        device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit2(reinterpret_cast<const vk::SubmitInfo2 &>(info), nullptr);
    if (wait)
      queue.waitIdle();
  }

  void run(uint32_t image, bool wait = true, const DdgiSchedulingHints &hints = {}) {
    manager->retireImage(image);
    rayScene->requestSurfaceScene(manager->wantsScene(),
                                  manager->settings().surfaceBudgetBytes);
    const std::array providers{
        RaySceneProviderSource{{"mesh"}, 1, vertices, indices, objects, draws}};
    rayScene->update(image, camera, frame, *sets[0], *sets[1], providers,
                     lighting, {}, {}, 0, {}, {},
                     [](uint32_t) { return uint64_t(1); });
    manager->update(image, *sets[0], *sets[1], lighting, {}, {}, 0, {}, {},
                    *environmentView, *sampler, true, hints);
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    rayScene->recordBuild(*command, image);
    manager->recordTrace(*command, image);
    manager->recordUpdate(*command, image);
    submit(wait, true);
    rayScene->commit();
    manager->commit();
    manager->submitAsync(image);
  }

  void enable() {
    config.enabled = true;
    manager->applySettings(config);
  }

  void verifyPausedSceneHistoryReset(bool asynchronous) {
    config.asyncCompute = asynchronous;
    config.volume.origin.z = -2.f;
    config.debugProbeRadius = .25f;
    enable();
    if (asynchronous && !manager->asyncActive())
      GTEST_SKIP() << "A compatible second compute queue is unavailable";
    camera.viewProj = container::math::orthoRH_ReverseZ(
        -1.f, 5.f, -1.f, 1.f, .1f, 10.f);
    camera.inverseViewProj = glm::inverse(camera.viewProj);
    ASSERT_NO_THROW(run(0));
    ASSERT_NO_THROW(manager->synchronizeAsync());
    ASSERT_NO_THROW(run(1));
    ASSERT_NO_THROW(manager->synchronizeAsync());
    ASSERT_EQ(manager->refreshRemainingProbes(), 0u);
    config.paused = true;
    ASSERT_NO_THROW(manager->applySettings(config));
    ASSERT_NO_THROW(run(0));
    createDebugResources();
    const auto before = debugPixels(0.f, 4);
    ASSERT_GT(before[16 * 32 + 10][1], 190u);
    const auto bytes = manager->allocatedBytes();
    const auto readSet = manager->readDescriptors(0);
    const auto submitted = manager->asyncSubmittedUpdates();
    const auto resets = manager->historyResets();

    ASSERT_NO_THROW(manager->resetSceneHistory());
    EXPECT_EQ(manager->historyResets(), resets + 1);
    ASSERT_NO_THROW(run(0));
    EXPECT_EQ(manager->lastBatch().probeCount, 0u);
    EXPECT_EQ(manager->frameNumber(), 0u);
    EXPECT_EQ(manager->schedulingCursor(), 0u);
    EXPECT_EQ(manager->refreshRemainingProbes(), 4u);
    EXPECT_EQ(manager->allocatedBytes(), bytes);
    EXPECT_EQ(manager->readDescriptors(0), readSet);
    EXPECT_EQ(manager->asyncSubmittedUpdates(), submitted);
    EXPECT_NE(manager->status().find("no probe history"), std::string::npos);
    const auto cleared = debugPixels(0.f, 4);
    const auto uninitialized = cleared[16 * 32 + 10];
    EXPECT_GT(uninitialized[0], 45u);
    EXPECT_LT(uninitialized[0], 70u);
    EXPECT_GT(uninitialized[1], 50u);
    EXPECT_LT(uninitialized[1], 80u);
    EXPECT_GT(uninitialized[2], 60u);
    EXPECT_LT(uninitialized[2], 90u);

    config.paused = false;
    ASSERT_NO_THROW(manager->applySettings(config));
    ASSERT_NO_THROW(run(1));
    // Reset must also retire a newly submitted writer, without resetting live
    // semaphore counters or letting it publish old irradiance after the reset.
    const auto nextSubmitted = manager->asyncSubmittedUpdates();
    ASSERT_NO_THROW(manager->resetSceneHistory());
    if (asynchronous) EXPECT_GT(nextSubmitted, submitted);
    EXPECT_EQ(manager->asyncPublishedUpdates(), nextSubmitted);
    config.paused = true;
    ASSERT_NO_THROW(manager->applySettings(config));
    ASSERT_NO_THROW(run(0));
    EXPECT_EQ(manager->frameNumber(), 0u);
    EXPECT_LT(debugPixels(0.f, 4)[16 * 32 + 10][1], 80u);
    config.paused = false;
    ASSERT_NO_THROW(manager->applySettings(config));
    ASSERT_NO_THROW(run(0));
    ASSERT_NO_THROW(manager->synchronizeAsync());
    ASSERT_NO_THROW(run(1));
    ASSERT_NO_THROW(manager->synchronizeAsync());
    ASSERT_NO_THROW(run(0));
    EXPECT_EQ(manager->refreshRemainingProbes(), 0u);
    EXPECT_GT(debugPixels(0.f, 4)[16 * 32 + 10][1], 190u);
  }

  void createDebugResources() {
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(6, vk::DescriptorType::eSampledImage, 1,
                                       vk::ShaderStageFlagBits::eFragment),
        vk::DescriptorSetLayoutBinding(10, vk::DescriptorType::eUniformBuffer,
                                       1,
                                       vk::ShaderStageFlagBits::eVertex |
                                           vk::ShaderStageFlagBits::eFragment)};
    debugLayout = vk::raii::DescriptorSetLayout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const std::array sizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 1),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1)};
    debugPool = vk::raii::DescriptorPool(
        device->raii(),
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    debugSets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*debugPool, *debugLayout));
    debugCamera = std::make_unique<UploadBuffer>(
        *memory, sizeof(CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const vk::DescriptorBufferInfo buffer(debugCamera->value.buffer, 0,
                                          debugCamera->size);
    const vk::DescriptorImageInfo image(
        {}, *depthView, vk::ImageLayout::eShaderReadOnlyOptimal);
    const std::array writes{
        vk::WriteDescriptorSet(*debugSets[0], 6, 0, 1,
                               vk::DescriptorType::eSampledImage, &image),
        vk::WriteDescriptorSet(*debugSets[0], 10, 0, 1,
                               vk::DescriptorType::eUniformBuffer, nullptr,
                               &buffer)};
    device->raii().updateDescriptorSets(writes, {});
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {32, 32, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &alloc, &debugColor,
                             &debugColorAllocation, nullptr),
              VK_SUCCESS);
    vk::ImageViewCreateInfo view;
    view.image = debugColor;
    view.viewType = vk::ImageViewType::e2D;
    view.format = vk::Format::eR8G8B8A8Unorm;
    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    debugColorView = vk::raii::ImageView(device->raii(), view);
    debugReadback =
        memory->createBuffer(32 * 32 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT);
    debugRenderer = std::make_unique<DdgiDebugRenderer>(
        device, CONTAINER_DDGI_SHADER_ROOT, *debugLayout,
        manager->descriptorLayout(), VK_FORMAT_R8G8B8A8_UNORM);
  }

  std::vector<std::array<uint8_t, 4>> debugPixels(float sceneDepth = 0,
                                               uint32_t probeCount = 1,
                                               DdgiDebugView mode = DdgiDebugView::Classification) {
    debugCamera->write(&camera, sizeof(camera));
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::ImageMemoryBarrier2 depthBarrier;
    depthBarrier.srcStageMask =
        debugInitialized ? vk::PipelineStageFlagBits2::eFragmentShader
                         : vk::PipelineStageFlagBits2::eNone;
    depthBarrier.srcAccessMask = debugInitialized
                                     ? vk::AccessFlagBits2::eShaderRead
                                     : vk::AccessFlagBits2::eNone;
    depthBarrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    depthBarrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    depthBarrier.oldLayout = debugInitialized
                                 ? vk::ImageLayout::eShaderReadOnlyOptimal
                                 : vk::ImageLayout::eUndefined;
    depthBarrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    depthBarrier.srcQueueFamilyIndex = depthBarrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = depth;
    depthBarrier.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0,
                                     1};
    vk::DependencyInfo dependency;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &depthBarrier;
    command.pipelineBarrier2(dependency);
    command.clearDepthStencilImage(depth, vk::ImageLayout::eTransferDstOptimal,
                                   vk::ClearDepthStencilValue(sceneDepth, 0),
                                   depthBarrier.subresourceRange);
    depthBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    depthBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    depthBarrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    depthBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    depthBarrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    depthBarrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    command.pipelineBarrier2(dependency);
    vk::ImageMemoryBarrier2 colorBarrier;
    colorBarrier.srcStageMask = debugInitialized
                                    ? vk::PipelineStageFlagBits2::eTransfer
                                    : vk::PipelineStageFlagBits2::eNone;
    colorBarrier.srcAccessMask = debugInitialized
                                     ? vk::AccessFlagBits2::eTransferRead
                                     : vk::AccessFlagBits2::eNone;
    colorBarrier.dstStageMask =
        vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorBarrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    colorBarrier.oldLayout = debugInitialized
                                 ? vk::ImageLayout::eTransferSrcOptimal
                                 : vk::ImageLayout::eUndefined;
    colorBarrier.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorBarrier.srcQueueFamilyIndex = colorBarrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    colorBarrier.image = debugColor;
    colorBarrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                     1};
    dependency.pImageMemoryBarriers = &colorBarrier;
    command.pipelineBarrier2(dependency);
    vk::RenderingAttachmentInfo attachment;
    attachment.imageView = *debugColorView;
    attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    attachment.loadOp = vk::AttachmentLoadOp::eClear;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.clearValue =
        vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 1}));
    vk::RenderingInfo rendering;
    rendering.renderArea = vk::Rect2D({0, 0}, {32, 32});
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    command.beginRendering(rendering);
    command.setViewport(0, vk::Viewport(0, 32, 32, -32, 0, 1));
    command.setScissor(0, vk::Rect2D({0, 0}, {32, 32}));
    debugRenderer->record(*command, *debugSets[0], manager->readDescriptors(0),
                          probeCount, mode,
                          config.debugProbeRadius);
    command.endRendering();
    colorBarrier.srcStageMask =
        vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    colorBarrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    colorBarrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    colorBarrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    colorBarrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorBarrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    command.pipelineBarrier2(dependency);
    vk::BufferImageCopy copy;
    copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.imageExtent = vk::Extent3D(32, 32, 1);
    command.copyImageToBuffer(debugColor, vk::ImageLayout::eTransferSrcOptimal,
                              debugReadback.buffer, copy);
    vk::MemoryBarrier2 hostBarrier;
    hostBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    hostBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    hostBarrier.dstStageMask = vk::PipelineStageFlagBits2::eHost;
    hostBarrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
    dependency.imageMemoryBarrierCount = 0;
    dependency.pImageMemoryBarriers = nullptr;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &hostBarrier;
    command.pipelineBarrier2(dependency);
    submit();
    debugInitialized = true;
    if (vmaInvalidateAllocation(memory->allocator(), debugReadback.allocation,
                                0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("debug color readback invalidate failed");
    std::vector<std::array<uint8_t, 4>> pixels(32 * 32);
    std::memcpy(pixels.data(), debugReadback.allocation_info.pMappedData,
                pixels.size() * sizeof(pixels.front()));
    return pixels;
  }

  void TearDown() override {
    if (device)
      device->raii().waitIdle();
    command.clear();
    debugRenderer.reset();
    debugSets.clear();
    debugPool.clear();
    debugLayout.clear();
    debugCamera.reset();
    debugColorView.clear();
    if (debugColor)
      vmaDestroyImage(memory->allocator(), debugColor, debugColorAllocation);
    if (debugReadback.buffer)
      memory->destroyBuffer(debugReadback);
    manager.reset();
    rayScene.reset();
    sets.clear();
    descriptors.clear();
    uploads.clear();
    sceneLayout.clear();
    lightLayout.clear();
    sampler.clear();
    environmentView.clear();
    textureView.clear();
    depthView.clear();
    if (environment)
      vmaDestroyImage(memory->allocator(), environment, environmentAllocation);
    if (depth)
      vmaDestroyImage(memory->allocator(), depth, depthAllocation);
    pool.clear();
    memory.reset();
    device.reset();
    surface.clear();
    messenger.clear();
    instance.reset();
    if (window)
      glfwDestroyWindow(window);
    if (glfwStarted)
      glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }

  GLFWwindow *window{};
  bool glfwStarted{};
  std::atomic_uint validationErrors{};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  vk::raii::DescriptorSetLayout sceneLayout{nullptr}, lightLayout{nullptr};
  vk::raii::DescriptorPool descriptors{nullptr};
  std::vector<vk::raii::DescriptorSet> sets;
  std::vector<std::unique_ptr<UploadBuffer>> uploads;
  VkImage environment{}, depth{};
  VmaAllocation environmentAllocation{}, depthAllocation{};
  vk::raii::ImageView environmentView{nullptr}, textureView{nullptr},
      depthView{nullptr};
  vk::raii::Sampler sampler{nullptr};
  vk::raii::DescriptorSetLayout debugLayout{nullptr};
  vk::raii::DescriptorPool debugPool{nullptr};
  std::vector<vk::raii::DescriptorSet> debugSets;
  std::unique_ptr<UploadBuffer> debugCamera;
  VkImage debugColor{};
  VmaAllocation debugColorAllocation{};
  vk::raii::ImageView debugColorView{nullptr};
  AllocatedBuffer debugReadback;
  std::unique_ptr<DdgiDebugRenderer> debugRenderer;
  bool debugInitialized{};
  FrameResources frame;
  CameraData camera;
  LightingData lighting;
  DdgiSettings config;
  std::unique_ptr<RayShadowManager> rayScene;
  std::unique_ptr<DdgiManager> manager;
  std::vector<container::geometry::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<ObjectData> objects;
  std::vector<DrawCommand> draws;
};

TEST_F(DdgiManagerGpu, EnableAfterDisabledPreparationProducesSubmittedHistory) {
  EXPECT_FALSE(manager->wantsScene());
  const auto disabledBytes = manager->allocatedBytes();
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->frameNumber(), 0u);
  ASSERT_NO_THROW(enable());
  EXPECT_TRUE(manager->wantsScene());
  EXPECT_GT(manager->allocatedBytes(), disabledBytes);
  ASSERT_NO_THROW(run(1));
  EXPECT_TRUE(manager->active());
  EXPECT_EQ(manager->frameNumber(), 1u);
  EXPECT_EQ(manager->lastBatch().firstProbe, 0u);
  EXPECT_EQ(manager->lastBatch().probeCount, 2u);
  EXPECT_EQ(manager->lastBatch().nextProbe, 2u);
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->frameNumber(), 2u);
  EXPECT_EQ(manager->lastBatch().firstProbe, 2u);
  EXPECT_EQ(manager->refreshRemainingProbes(), 0u);
}

TEST_F(DdgiManagerGpu, AdditionalVolumesShareCapacityAndRefreshAllPackedProbes) {
  config.additionalVolumes.push_back({{-.5f,0,0},{.5f,.5f,.5f},{4,1,1}});
  enable();
  const auto bytes=manager->allocatedBytes();
  for(uint32_t frame=0;frame<8 && (frame==0 || manager->refreshRemainingProbes()!=0);++frame) {
    ASSERT_NO_THROW(run(frame%2));
    EXPECT_EQ(manager->lastBatch().volumeProbeCount,8u);
    EXPECT_EQ(manager->lastBatch().probeCount,2u);
    EXPECT_EQ(manager->lastBatch().rayCount,256u);
    EXPECT_EQ(manager->allocatedBytes(),bytes);
  }
  EXPECT_TRUE(manager->active());
  EXPECT_EQ(manager->refreshRemainingProbes(),0u);
  config.volumeBlendCells=2.f;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->allocatedBytes(),bytes);
  EXPECT_EQ(manager->settings().additionalVolumes.size(),1u);
  config.additionalVolumes.clear();
  ASSERT_NO_THROW(manager->applySettings(config));
  EXPECT_EQ(manager->frameNumber(),0u);
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().volumeProbeCount,4u);
}

TEST_F(DdgiManagerGpu, AsyncWriterDoesNotBlockGraphicsOrPublishUnfinishedHistory) {
  config.asyncCompute = true;
  enable();
  if (!device->asyncComputeQueue()) {
    EXPECT_FALSE(manager->asyncActive());
    ASSERT_NO_THROW(run(0));
    EXPECT_EQ(manager->frameNumber(), 1u);
    return;
  }
  ASSERT_TRUE(manager->asyncActive());
  vk::SemaphoreTypeCreateInfo type(vk::SemaphoreType::eTimeline, 0);
  vk::SemaphoreCreateInfo info;
  info.pNext = &type;
  vk::raii::Semaphore gate(device->raii(), info);
  vk::raii::Queue compute(device->raii(),
      device->queueFamilyIndices().graphicsFamily.value(), 1);
  struct ReleaseGate {
    const vk::raii::Device &device;
    const vk::raii::Semaphore &gate;
    const vk::raii::Queue &queue;
    ~ReleaseGate() noexcept {
      try {
        if (gate.getCounterValue() < 1)
          device.signalSemaphore(vk::SemaphoreSignalInfo(*gate, 1));
        queue.waitIdle();
      } catch (const std::exception &) {}
    }
  } releaseGate{device->raii(), gate, compute};
  const vk::SemaphoreSubmitInfo wait(*gate, 1,
                                     vk::PipelineStageFlagBits2::eAllCommands);
  vk::SubmitInfo2 gated;
  gated.waitSemaphoreInfoCount = 1;
  gated.pWaitSemaphoreInfos = &wait;
  compute.submit2(gated, nullptr);
  // Graphics 0 supplies the AS/input snapshot; its writer remains behind a
  // host-controlled gate. Graphics 1 must still complete using old history.
  EXPECT_NO_THROW(run(0));
  EXPECT_EQ(manager->frameNumber(), 1u);
  EXPECT_FALSE(manager->graphicsWait());
  lighting.environmentIntensity = .75f;
  EXPECT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().probeCount, 0u);
  EXPECT_EQ(manager->frameNumber(), 1u);
  EXPECT_FALSE(manager->graphicsWait());
  device->raii().signalSemaphore(vk::SemaphoreSignalInfo(*gate, 1));
  ASSERT_NO_THROW(manager->retireImage(0));
  ASSERT_TRUE(manager->graphicsWait());
  EXPECT_EQ(manager->graphicsWait()->value, 1u);
  EXPECT_EQ(manager->refreshRemainingProbes(), 4u); // New lighting epoch.
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->frameNumber(), 2u);
  ASSERT_NO_THROW(manager->synchronizeAsync());
}

TEST_F(DdgiManagerGpu, AsyncPauseRetainsPublishedSnapshotAndResumeCursor) {
  config.asyncCompute = true;
  enable();
  ASSERT_NO_THROW(run(0));
  ASSERT_NO_THROW(manager->synchronizeAsync());
  const auto bytes = manager->allocatedBytes();
  const auto cursor = manager->schedulingCursor();
  const auto epoch = manager->contentEpoch();
  config.paused = true;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(1));
  EXPECT_TRUE(manager->active());
  EXPECT_EQ(manager->lastBatch().probeCount, 0u);
  EXPECT_EQ(manager->frameNumber(), 1u);
  EXPECT_EQ(manager->schedulingCursor(), cursor);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  config.paused = false;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->lastBatch().firstProbe, cursor);
  ASSERT_NO_THROW(manager->synchronizeAsync());
  EXPECT_EQ(manager->refreshRemainingProbes(), 0u);
  config.asyncCompute = false;
  ASSERT_NO_THROW(manager->applySettings(config));
  EXPECT_FALSE(manager->asyncActive());
  EXPECT_EQ(manager->frameNumber(), 0u);
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->frameNumber(), 1u);
}

TEST_F(DdgiManagerGpu, PausedSceneReplacementClearsHistoryWithoutReallocation) {
  verifyPausedSceneHistoryReset(false);
}

TEST_F(DdgiManagerGpu, AsyncSceneReplacementRetiresWriterAndClearsPausedHistory) {
  verifyPausedSceneHistoryReset(true);
}

TEST_F(DdgiManagerGpu, ScrollingRetainsPhysicalSamplesAndClearsExposedNodesWhilePaused) {
  config.cameraFollow = true;
  config.volume.origin.z = -2.f;
  config.debugProbeRadius = .25f;
  enable();
  camera.viewProj = container::math::orthoRH_ReverseZ(-1.f, 5.f, -1.f, 1.f, .1f, 10.f);
  camera.inverseViewProj = glm::inverse(camera.viewProj);
  DdgiSchedulingHints hints;
  hints.cameraPosition = glm::vec3(1.5f, 0, -2);
  ASSERT_NO_THROW(run(0, true, hints));
  ASSERT_NO_THROW(run(1, true, hints));
  ASSERT_EQ(manager->refreshRemainingProbes(), 0u);
  const auto bytes = manager->allocatedBytes();
  const auto epoch = manager->contentEpoch();
  const auto resets = manager->historyResets();
  const auto readSet = manager->readDescriptors(0);
  config.paused = true;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0, true, hints));
  createDebugResources();
  const auto beforeMove = debugPixels(0.f, 4, DdgiDebugView::Irradiance);
  hints.cameraPosition = glm::vec3(2.5f, 0, -2);
  ASSERT_NO_THROW(run(0, true, hints));
  EXPECT_EQ(manager->scrollCount(), 1u);
  EXPECT_EQ(manager->currentPrimaryVolume().origin, glm::vec3(1, 0, -2));
  EXPECT_EQ(manager->settings().volume.origin, glm::vec3(0, 0, -2));
  EXPECT_EQ(manager->lastScrollPreservedProbes(), 3u);
  EXPECT_EQ(manager->refreshRemainingProbes(), 1u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_EQ(manager->readDescriptors(0), readSet);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->historyResets(), resets);
  // Render the production state lookup. Remapped physical probe x=1 retains
  // its valid green state; the newly exposed x=4 node is uninitialized gray.
  const auto pixels = debugPixels(0.f, 4);
  const auto retained = pixels[16 * 32 + 10];
  const auto exposed = pixels[16 * 32 + 26];
  EXPECT_LT(retained[0], 40u);
  EXPECT_GT(retained[1], 190u);
  EXPECT_GT(retained[2], 40u);
  EXPECT_GT(exposed[0], 45u);
  EXPECT_LT(exposed[0], 70u);
  EXPECT_GT(exposed[1], 50u);
  EXPECT_LT(exposed[1], 80u);
  EXPECT_GT(exposed[2], 60u);
  EXPECT_LT(exposed[2], 90u);
  const auto afterMove = debugPixels(0.f, 4, DdgiDebugView::Irradiance);
  EXPECT_GT(beforeMove[16 * 32 + 10][2], 30u);
  for (uint32_t channel = 0; channel < 3; ++channel)
    EXPECT_NEAR(afterMove[16 * 32 + 10][channel],
                beforeMove[16 * 32 + 10][channel], 1);
  config.paused = false;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(1, true, hints));
  EXPECT_EQ(manager->currentPrimaryVolume().origin, glm::vec3(1, 0, -2));
  EXPECT_EQ(manager->refreshRemainingProbes(), 0u);
  config.paused = true;
  ASSERT_NO_THROW(manager->applySettings(config));
  hints.cameraPosition = glm::vec3(50.5f, 0, -2);
  ASSERT_NO_THROW(run(0, true, hints));
  EXPECT_EQ(manager->lastScrollPreservedProbes(), 0u);
  EXPECT_EQ(manager->refreshRemainingProbes(), 4u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
}

TEST_F(DdgiManagerGpu, AsyncScrollingKeepsStaticDetailVolumesAndRetiresOldWriter) {
  config.cameraFollow = config.asyncCompute = true;
  config.additionalVolumes.push_back({{-2, 0, 0}, {.5f, 1, 1}, {4, 1, 1}});
  enable();
  DdgiSchedulingHints hints;
  hints.cameraPosition = glm::vec3(1.5f, 0, 0);
  for (uint32_t frame = 0; frame < 8; ++frame) {
    ASSERT_NO_THROW(run(frame % 2, true, hints));
    ASSERT_NO_THROW(manager->synchronizeAsync());
  }
  ASSERT_EQ(manager->refreshRemainingProbes(), 0u);
  const auto bytes = manager->allocatedBytes();
  const auto epoch = manager->contentEpoch();
  hints.cameraPosition = glm::vec3(2.5f, 0, 0);
  ASSERT_NO_THROW(run(0, true, hints));
  ASSERT_NO_THROW(manager->synchronizeAsync());
  EXPECT_EQ(manager->lastScrollPreservedProbes(), 3u);
  EXPECT_EQ(manager->settings().additionalVolumes.front().origin, glm::vec3(-2, 0, 0));
  EXPECT_EQ(manager->lastBatch().volumeProbeCount, 8u);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_LE(manager->refreshRemainingProbes(), 1u);
}

TEST_F(DdgiManagerGpu, UnrepresentableCameraScrollKeepsWorkingGridAndHistory) {
  config.cameraFollow = true;
  enable();
  DdgiSchedulingHints hints;
  hints.cameraPosition = glm::vec3(1.5f, 0, 0);
  ASSERT_NO_THROW(run(0, true, hints));
  ASSERT_NO_THROW(run(1, true, hints));
  const auto previous = manager->currentPrimaryVolume();
  const auto epoch = manager->contentEpoch();
  const auto bytes = manager->allocatedBytes();
  hints.cameraPosition = glm::vec3(1e8f, 0, 0);
  ASSERT_NO_THROW(run(0, true, hints));
  EXPECT_TRUE(manager->active());
  EXPECT_EQ(manager->currentPrimaryVolume(), previous);
  EXPECT_EQ(manager->scrollCount(), 0u);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->refreshRemainingProbes(), 0u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_NE(manager->status().find("camera-follow held:"), std::string::npos);
  hints.cameraPosition = glm::vec3(2.5f, 0, 0);
  ASSERT_NO_THROW(run(1, true, hints));
  EXPECT_EQ(manager->scrollCount(), 1u);
  EXPECT_EQ(manager->status().find("camera-follow held:"), std::string::npos);
}

TEST_F(DdgiManagerGpu, InvalidLightSnapshotCountFallsBackAndRecovers) {
  enable();
  ASSERT_NO_THROW(run(0));
  const auto epoch = manager->contentEpoch();
  const auto frameNumber = manager->frameNumber();
  lighting.pointLightCount = 1; // The fixture deliberately supplies no points.
  ASSERT_NO_THROW(run(1));
  EXPECT_FALSE(manager->active());
  EXPECT_EQ(manager->lastBatch().probeCount, 0u);
  EXPECT_EQ(manager->frameNumber(), frameNumber);
  EXPECT_NE(manager->status().find("light counts exceed snapshot arrays"), std::string::npos);
  lighting.pointLightCount = 0;
  ASSERT_NO_THROW(run(0));
  EXPECT_TRUE(manager->active());
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->frameNumber(), frameNumber + 1);
}

TEST_F(DdgiManagerGpu, CameraPriorityNeverCompletesEpochWithDuplicateUpdates) {
  config.volume.counts={8,1,1};
  enable();
  DdgiSchedulingHints hints;
  hints.cameraPosition=glm::vec3(7,0,0);
  hints.cameraForward={-1,0,0};
  std::array<bool,8> visited{};
  const uint32_t maximumFrames=16;
  uint32_t epoch=0;
  for(uint32_t frame=0;frame<maximumFrames;++frame) {
    ASSERT_NO_THROW(run(frame%2,true,hints));
    if(frame==0) epoch=manager->contentEpoch();
    EXPECT_EQ(manager->contentEpoch(),epoch);
    for(const auto &u:manager->lastUpdates()) visited.at(u.probeIndex)=true;
    const uint32_t remaining=uint32_t(std::count(visited.begin(),visited.end(),false));
    EXPECT_EQ(manager->refreshRemainingProbes(),remaining);
    if(remaining==0) break;
  }
  EXPECT_EQ(manager->refreshRemainingProbes(),0u);
  const auto resets=manager->historyResets();
  hints.cameraPosition=glm::vec3(0,0,0);
  hints.cameraForward={1,0,0};
  ASSERT_NO_THROW(run(0,true,hints));
  EXPECT_EQ(manager->contentEpoch(),epoch);
  EXPECT_EQ(manager->historyResets(),resets);
}

TEST_F(DdgiManagerGpu,
       PauseAndReducedBudgetsPreserveResourcesAndRoundRobinCursor) {
  enable();
  ASSERT_NO_THROW(run(0));
  const auto bytes = manager->allocatedBytes();
  const auto set = manager->readDescriptors(0);
  const auto epoch = manager->contentEpoch();
  const auto resets = manager->historyResets();
  config.paused = true;
  ASSERT_NO_THROW(manager->applySettings(config));
  for (uint32_t image : {1u, 0u}) {
    ASSERT_NO_THROW(run(image));
    EXPECT_TRUE(manager->active());
    EXPECT_EQ(manager->lastBatch().probeCount, 0u);
    EXPECT_EQ(manager->frameNumber(), 1u);
    EXPECT_EQ(manager->schedulingCursor(), 2u);
    EXPECT_EQ(manager->allocatedBytes(), bytes);
    EXPECT_EQ(manager->readDescriptors(0), set);
    EXPECT_EQ(manager->contentEpoch(), epoch);
    EXPECT_EQ(manager->historyResets(), resets);
  }
  config.resources.updates.maxProbesPerFrame = 1;
  config.resources.updates.maxRaysPerFrame = 128;
  config.paused = false;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().firstProbe, 2u);
  EXPECT_EQ(manager->lastBatch().probeCount, 1u);
  EXPECT_EQ(manager->lastBatch().nextProbe, 3u);
  EXPECT_EQ(manager->schedulingCursor(), 3u);
  EXPECT_EQ(manager->frameNumber(), 2u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_EQ(manager->readDescriptors(0), set);
  config.resources.updates.maxRaysPerFrame = 0;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->lastBatch().probeCount, 0u);
  EXPECT_EQ(manager->frameNumber(), 2u);
  EXPECT_EQ(manager->schedulingCursor(), 3u);
  config.resources.updates.maxRaysPerFrame = 128;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().firstProbe, 3u);
  EXPECT_EQ(manager->lastBatch().nextProbe, 0u);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->historyResets(), resets);
  config.enabled = false;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0));
  EXPECT_FALSE(manager->active());
  EXPECT_EQ(manager->frameNumber(), 3u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_EQ(manager->readDescriptors(0), set);
  config.enabled = true;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(1));
  EXPECT_TRUE(manager->active());
  EXPECT_EQ(manager->lastBatch().firstProbe, 0u);
  EXPECT_EQ(manager->frameNumber(), 4u);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->historyResets(), resets);
}

TEST_F(DdgiManagerGpu, LiveTransportRetuningRefreshesContentWithoutRebuilding) {
  enable();
  ASSERT_NO_THROW(run(0));
  ASSERT_NO_THROW(run(1));
  ASSERT_EQ(manager->refreshRemainingProbes(), 0u);
  const auto bytes = manager->allocatedBytes();
  const auto set = manager->readDescriptors(0);
  const auto epoch = manager->contentEpoch();
  const auto resets = manager->historyResets();
  config.intensity = 2;
  config.debugView = DdgiDebugView::Classification;
  config.debugProbeRadius = .08f;
  config.hysteresis = .5f;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->lastBatch().firstProbe, 0u);
  EXPECT_EQ(manager->frameNumber(), 3u);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->historyResets(), resets);
  EXPECT_EQ(manager->refreshRemainingProbes(), 0u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_EQ(manager->readDescriptors(0), set);
  // Lookup bias participates in previous-bounce transport as well as reads.
  config.viewBias = .02f;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().firstProbe, 2u);
  EXPECT_EQ(manager->contentEpoch(), epoch + 1);
  EXPECT_EQ(manager->historyResets(), resets + 1);
  EXPECT_EQ(manager->refreshRemainingProbes(), 2u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  config.normalBias = .05f;
  ASSERT_NO_THROW(manager->applySettings(config));
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->contentEpoch(), epoch + 2);
  EXPECT_EQ(manager->historyResets(), resets + 2);
  EXPECT_EQ(manager->lastBatch().firstProbe, 0u);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
}

TEST_F(DdgiManagerGpu,
       CapacityGrowthAndLatticeChangeRetirePendingGpuResources) {
  enable();
  // Leave a production trace/update submission outstanding. applySettings must
  // retire it before freeing its descriptor pool, buffers or ping-pong images.
  ASSERT_NO_THROW(run(0, false));
  config.resources.updates.maxProbesPerFrame = 4;
  config.resources.updates.maxRaysPerFrame = 512;
  ASSERT_NO_THROW(manager->applySettings(config));
  EXPECT_EQ(manager->frameNumber(), 0u);
  EXPECT_EQ(manager->contentEpoch(), 0u);
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().firstProbe, 0u);
  EXPECT_EQ(manager->lastBatch().probeCount, 4u);
  EXPECT_EQ(manager->frameNumber(), 1u);
  ASSERT_NO_THROW(run(0, false));
  config.volume.origin = {-1, 0, 0};
  config.volume.counts.x = 8;
  config.resources.updates.raysPerProbe = 256;
  config.resources.updates.maxRaysPerFrame = 1024;
  ASSERT_NO_THROW(manager->applySettings(config));
  EXPECT_EQ(manager->frameNumber(), 0u);
  EXPECT_EQ(manager->contentEpoch(), 0u);
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().volumeProbeCount, 8u);
  EXPECT_EQ(manager->lastBatch().raysPerProbe, 256u);
  EXPECT_EQ(manager->lastBatch().firstProbe, 0u);
  EXPECT_EQ(manager->lastBatch().nextProbe, 4u);
  ASSERT_NO_THROW(run(0));
  EXPECT_EQ(manager->lastBatch().firstProbe, 4u);
  EXPECT_EQ(manager->lastBatch().nextProbe, 0u);
  EXPECT_EQ(manager->frameNumber(), 2u);
}

TEST_F(DdgiManagerGpu,
       InvalidLiveEditLeavesSubmittedHistoryAndConfigurationUsable) {
  enable();
  ASSERT_NO_THROW(run(0));
  const auto bytes = manager->allocatedBytes();
  const auto set = manager->readDescriptors(0);
  const auto epoch = manager->contentEpoch();
  auto invalid = config;
  invalid.volume.spacing.y = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(manager->applySettings(invalid), std::invalid_argument);
  invalid = config;
  invalid.resources.updates.raysPerProbe = 0;
  EXPECT_THROW(manager->applySettings(invalid), std::invalid_argument);
  invalid = config;
  invalid.payloadBudgetBytes = 1;
  EXPECT_THROW(manager->applySettings(invalid), std::invalid_argument);
  EXPECT_EQ(manager->allocatedBytes(), bytes);
  EXPECT_EQ(manager->readDescriptors(0), set);
  EXPECT_EQ(manager->frameNumber(), 1u);
  EXPECT_EQ(manager->contentEpoch(), epoch);
  EXPECT_EQ(manager->settings().volume.spacing, config.volume.spacing);
  EXPECT_EQ(manager->settings().resources.updates.raysPerProbe,
            config.resources.updates.raysPerProbe);
  EXPECT_EQ(manager->settings().payloadBudgetBytes, config.payloadBudgetBytes);
  ASSERT_NO_THROW(run(1));
  EXPECT_EQ(manager->lastBatch().firstProbe, 2u);
  EXPECT_EQ(manager->frameNumber(), 2u);
}

TEST_F(DdgiManagerGpu, NearPlaneCrossingDebugSphereRemainsVisibleAndOccluded) {
  // The eye is outside this sphere, but its center-plane quad is before the
  // near plane. The far hemisphere remains visible and must be rasterized.
  config.volume = {{0, 0, -.09f}, {1, 1, 1}, {1, 1, 1}};
  config.debugProbeRadius = .06f;
  enable();
  camera.viewProj = container::math::perspectiveRH_ReverseZ(glm::radians(60.f),
                                                            1.f, .1f, 20.f);
  camera.inverseViewProj = glm::inverse(camera.viewProj);
  ASSERT_NO_THROW(run(0));
  ASSERT_TRUE(manager->active());
  createDebugResources();
  auto pixels = debugPixels();
  const auto center = pixels[16 * 32 + 16];
  EXPECT_LT(center[0], 30u);
  EXPECT_GT(center[1], 200u);
  EXPECT_GT(center[2], 50u);
  // Nearer opaque depth must still hide the analytic sphere intersection.
  pixels = debugPixels(1.f);
  for (const auto &pixel : pixels) {
    EXPECT_EQ(pixel[0], 0u);
    EXPECT_EQ(pixel[1], 0u);
    EXPECT_EQ(pixel[2], 0u);
  }
}

TEST_F(DdgiManagerGpu, OffAxisOrthographicDebugSphereCoversFullSilhouette) {
  config.volume = {{.7f, 0, -1.f}, {1, 1, 1}, {1, 1, 1}};
  config.debugProbeRadius = .22f;
  enable();
  camera.viewProj = container::math::orthoRH_ReverseZ(-1, 1, -1, 1, .1f, 20.f);
  camera.inverseViewProj = glm::inverse(camera.viewProj);
  ASSERT_NO_THROW(run(0));
  createDebugResources();
  const auto pixels = debugPixels();
  // Pixel center x=30.5 is inside the projected circle, close to its right
  // edge. An eye-facing quad underbounds an off-axis orthographic sphere here.
  for (uint32_t x : {27u, 30u}) {
    const auto pixel = pixels[16 * 32 + x];
    EXPECT_LT(pixel[0], 30u) << "x=" << x;
    EXPECT_GT(pixel[1], 200u) << "x=" << x;
    EXPECT_GT(pixel[2], 50u) << "x=" << x;
  }
  EXPECT_EQ(pixels[16 * 32 + 23][1], 0u);
}
} // namespace
