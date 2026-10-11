#include "Container/common/CommonGLFW.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <tuple>

namespace {
using namespace container::renderer;
using namespace container::gpu;

enum class SurfaceFault { Normal, Tangent, Color, InverseTransform };

class RayShadowSurfaceGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<RayShadowSurfaceGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char *enabled = std::getenv("CONTAINER_RUN_GPU_RAY_QUERY");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP()
          << "Set CONTAINER_RUN_GPU_RAY_QUERY=1 for Vulkan manager tests";
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Optional ray surface fallback", nullptr,
                              nullptr);
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
    deviceInfo.enabledFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
    deviceInfo.enabledFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.separateDepthStencilLayouts = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
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

    // The production shadow pipeline requires the complete material and light
    // layouts even though this fixture records only AS builds, not dispatches.
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
    const std::array descriptorSizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 9),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 4096),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 4),
        vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1)};
    descriptorPool = vk::raii::DescriptorPool(
        device->raii(),
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 2,
            descriptorSizes));
    const std::array layouts{*sceneLayout, *lightLayout};
    sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptorPool, layouts));
    createDepth();
    manager = std::make_unique<RayShadowManager>(device, *memory);
    ASSERT_NO_THROW(manager->createPipelines(CONTAINER_RAY_SHADOW_SHADER_ROOT,
                                             *sceneLayout, *lightLayout));
    manager->prepare({32, 32}, 2);
    manager->requestSurfaceScene(true);
    vertices.resize(3);
    vertices[0].position = {-1, -1, 0};
    vertices[1].position = {1, -1, 0};
    vertices[2].position = {0, 1, 0};
    for (auto &vertex : vertices) {
      vertex.normal = {0, 0, 1};
      vertex.tangent = {1, 0, 0, 1};
      vertex.color = {1, 1, 1};
    }
    indices = {0, 1, 2};
    objects.resize(1);
    draws = {{0, 0, 3}};
  }

  void createDepth() {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {32, 32, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &alloc, &depth,
                             &depthAllocation, nullptr),
              VK_SUCCESS);
    vk::ImageViewCreateInfo view;
    view.image = depth;
    view.viewType = vk::ImageViewType::e2D;
    view.format = vk::Format::eD32Sfloat;
    view.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    depthView = vk::raii::ImageView(device->raii(), view);
    frame.depthStencil = {depth, depthAllocation, *depthView,
                          VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT};
    frame.depthSamplingView = *depthView;
  }

  void TearDown() override {
    if (device)
      device->raii().waitIdle();
    // Command references and descriptor sets retire before their resources.
    command.clear();
    manager.reset();
    sets.clear();
    descriptorPool.clear();
    sceneLayout.clear();
    lightLayout.clear();
    depthView.clear();
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

  void update(uint32_t image, bool emptyProviders = false) {
    const std::array providers{RaySceneProviderSource{
        {"mesh"}, revision, vertices, indices, objects, draws}};
    manager->update(image, camera, frame, *sets[0], *sets[1],
                    emptyProviders ? std::span<const RaySceneProviderSource>{}
                                   : std::span<const RaySceneProviderSource>(providers),
                    lighting, {}, {}, 0, {}, {},
                    [](uint32_t) { return uint64_t(1); });
  }

  void build(uint32_t image, bool emptyProviders = false) {
    update(image, emptyProviders);
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    manager->recordBuild(*command, image);
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &handle;
    vk::raii::Queue queue(
        device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, nullptr);
    queue.waitIdle();
  }

  void verifyUnloadAndReload(bool surfaces) {
    manager->settings().mode = RayShadowMode::Hard;
    manager->requestSurfaceScene(surfaces);
    ASSERT_NO_THROW(build(0));
    ASSERT_NO_THROW(build(1));
    ASSERT_TRUE(manager->active());
    ASSERT_NE(manager->buildStats(), nullptr);
    ASSERT_EQ(manager->buildStats()->instanceCount, 1u);
    EXPECT_EQ(bool(manager->surfaceBindings(1)), surfaces);
    const auto loadedGeneration = manager->sceneGeneration();
    const auto loadedBytes = manager->allocatedBytes();
    const auto savedDraws = draws;
    draws.clear();

    // Both slots retain the submitted generation. Retiring the first image
    // disables all new consumers immediately, retaining the other image's AS.
    ASSERT_NO_THROW(build(0));
    EXPECT_FALSE(manager->active());
    EXPECT_EQ(manager->sceneRevision(), 0u);
    EXPECT_EQ(manager->buildStats(), nullptr);
    EXPECT_FALSE(manager->surfaceBindings(0));
    EXPECT_FALSE(manager->surfaceBindings(1));
    EXPECT_TRUE(manager->surfaceUnsupportedReason().empty());
    EXPECT_EQ(manager->settings().mode, RayShadowMode::Hard);
    EXPECT_EQ(manager->sceneGeneration(), loadedGeneration);
    EXPECT_EQ(manager->allocatedBytes(), loadedBytes);
    ASSERT_NO_THROW(build(0, true));
    EXPECT_EQ(manager->allocatedBytes(), loadedBytes);

    // Only the last retired image can release the final submitted reference.
    ASSERT_NO_THROW(build(1, true));
    const auto emptyBytes = manager->allocatedBytes();
    EXPECT_LT(emptyBytes, loadedBytes);
    EXPECT_EQ(manager->buildStats(), nullptr);
    EXPECT_EQ(manager->sceneGeneration(), loadedGeneration);
    for (uint32_t cycle = 0; cycle < 3; ++cycle) {
      draws = savedDraws;
      ASSERT_NO_THROW(build(0));
      ASSERT_NO_THROW(build(1));
      EXPECT_TRUE(manager->active());
      EXPECT_NE(manager->sceneRevision(), 0u);
      ASSERT_NE(manager->buildStats(), nullptr);
      EXPECT_EQ(manager->buildStats()->instanceCount, 1u);
      EXPECT_EQ(bool(manager->surfaceBindings(1)), surfaces);
      EXPECT_EQ(manager->sceneGeneration(), loadedGeneration + cycle + 1);
      EXPECT_LE(manager->allocatedBytes(), loadedBytes);
      draws.clear();
      ASSERT_NO_THROW(build(0));
      ASSERT_NO_THROW(build(1, true));
      EXPECT_FALSE(manager->active());
      EXPECT_EQ(manager->sceneRevision(), 0u);
      EXPECT_EQ(manager->buildStats(), nullptr);
      EXPECT_EQ(manager->allocatedBytes(), emptyBytes);
    }
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
  vk::raii::DescriptorPool descriptorPool{nullptr};
  std::vector<vk::raii::DescriptorSet> sets;
  VkImage depth{};
  VmaAllocation depthAllocation{};
  vk::raii::ImageView depthView{nullptr};
  FrameResources frame;
  CameraData camera;
  LightingData lighting;
  std::unique_ptr<RayShadowManager> manager;
  std::vector<container::geometry::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<ObjectData> objects;
  std::vector<DrawCommand> draws;
  uint64_t revision{1};
};

class OptionalSurfaceFailure : public RayShadowSurfaceGpu,
                               public ::testing::WithParamInterface<
                                   std::tuple<RayShadowMode, SurfaceFault>> {};

TEST_P(OptionalSurfaceFailure, FallsBackCachesFailureAndRetriesRepairedScene) {
  const auto [mode, fault] = GetParam();
  manager->settings().mode = mode;
  ASSERT_NO_THROW(build(0));
  ASSERT_TRUE(manager->surfaceBindings(0));
  const auto initialGeneration = manager->sceneGeneration();
  const auto original = vertices[1];
  const float nan = std::numeric_limits<float>::quiet_NaN();
  switch (fault) {
  case SurfaceFault::Normal:
    vertices[1].normal.y = nan;
    ++revision;
    break;
  case SurfaceFault::Tangent:
    vertices[1].tangent.w = nan;
    ++revision;
    break;
  case SurfaceFault::Color:
    vertices[1].color.z = nan;
    ++revision;
    break;
  case SurfaceFault::InverseTransform:
    // Finite, affine and invertible for compact visibility; inverse-transpose
    // normal components exceed float range only for optional surface shading.
    objects[0].model = glm::scale(glm::mat4(1), glm::vec3(1e-39f, 1, 1));
    break;
  }
  ASSERT_NO_THROW(build(1));
  EXPECT_FALSE(manager->surfaceBindings(1));
  EXPECT_FALSE(manager->surfaceUnsupportedReason().empty());
  const auto failedGeneration = manager->sceneGeneration();
  if (mode == RayShadowMode::Raster) {
    EXPECT_FALSE(manager->active());
    EXPECT_EQ(manager->sceneRevision(), 0u);
    EXPECT_EQ(failedGeneration, initialGeneration);
  } else {
    EXPECT_TRUE(manager->active());
    EXPECT_NE(manager->sceneRevision(), 0u);
    ASSERT_NE(manager->buildStats(), nullptr);
    EXPECT_EQ(manager->buildStats()->instanceCount, 1u);
    EXPECT_EQ(failedGeneration, initialGeneration + 1);
  }
  const auto reason = manager->surfaceUnsupportedReason();
  // Retire both slots and revisit the same bad input: rejected surfaces must
  // neither thrash compact AS generations nor retry every raster frame.
  for (uint32_t image : {0u, 1u, 0u}) {
    ASSERT_NO_THROW(build(image));
    EXPECT_EQ(manager->sceneGeneration(), failedGeneration);
    EXPECT_EQ(manager->surfaceUnsupportedReason(), reason);
    EXPECT_FALSE(manager->surfaceBindings(image));
  }
  vertices[1] = original;
  objects[0].model = glm::mat4(1);
  if (fault != SurfaceFault::InverseTransform)
    ++revision;
  ASSERT_NO_THROW(build(1));
  EXPECT_TRUE(manager->surfaceBindings(1));
  EXPECT_TRUE(manager->surfaceUnsupportedReason().empty());
  EXPECT_EQ(manager->sceneGeneration(), failedGeneration + 1);
  EXPECT_EQ(manager->active(), mode != RayShadowMode::Raster);
}

INSTANTIATE_TEST_SUITE_P(
    RasterHardSoft, OptionalSurfaceFailure,
    ::testing::Combine(::testing::Values(RayShadowMode::Raster,
                                         RayShadowMode::Hard,
                                         RayShadowMode::Soft),
                       ::testing::Values(SurfaceFault::Normal,
                                         SurfaceFault::Tangent,
                                         SurfaceFault::Color,
                                         SurfaceFault::InverseTransform)));

TEST_F(RayShadowSurfaceGpu, MandatoryGeometryFailuresStillPropagate) {
  manager->settings().mode = RayShadowMode::Hard;
  ASSERT_NO_THROW(build(0));
  vertices[1].position.x = std::numeric_limits<float>::quiet_NaN();
  ++revision;
  EXPECT_THROW(update(1), std::invalid_argument);
  vertices[1].position.x = 1;
  indices[2] = 3;
  ++revision;
  EXPECT_THROW(update(1), std::invalid_argument);
  indices[2] = 2;
  ++revision;
  objects[0].model = glm::scale(glm::mat4(1), glm::vec3(0, 1, 1));
  EXPECT_THROW(update(1), std::invalid_argument);
}

TEST_F(RayShadowSurfaceGpu, EmptySceneRetiresCompactGenerationsAndReloads) {
  verifyUnloadAndReload(false);
}

TEST_F(RayShadowSurfaceGpu, EmptySceneRetiresSurfaceGenerationsAndReloads) {
  verifyUnloadAndReload(true);
}
} // namespace
