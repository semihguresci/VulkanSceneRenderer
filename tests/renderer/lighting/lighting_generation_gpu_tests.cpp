#include <Container/common/CommonGLFW.h>
#include <Container/app/AppConfig.h>
#include <Container/ecs/World.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/VulkanDevice.h>

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using namespace container::gpu;
using namespace container::renderer;
using container::scene::SceneGraph;
using LightingBytes = std::array<std::byte, sizeof(LightingData)>;
using PointBytes = std::array<std::byte, sizeof(PointLightData)>;
using AreaBytes = std::array<std::byte, sizeof(AreaLightData)>;

static_assert(!std::is_copy_constructible_v<LightingManager::PreparedGeneration>);
static_assert(std::is_nothrow_move_constructible_v<LightingManager::PreparedGeneration>);
static_assert(std::is_nothrow_move_assignable_v<LightingManager::PreparedGeneration>);
static_assert(noexcept(std::declval<LightingManager&>().exchangeGeneration(
    std::declval<LightingManager::PreparedGeneration&>())));
static_assert(!std::is_copy_constructible_v<LightingManager::PreparedRayShadowBindings>);
static_assert(std::is_nothrow_move_constructible_v<LightingManager::PreparedRayShadowBindings>);
static_assert(std::is_nothrow_move_assignable_v<LightingManager::PreparedRayShadowBindings>);
static_assert(noexcept(std::declval<LightingManager&>().exchangeRayShadowBindings(
    std::declval<LightingManager::PreparedRayShadowBindings&>())));

class LightingGenerationGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<LightingGenerationGpu*>(user)->errors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto* enabled = std::getenv("CONTAINER_RUN_GPU_LIGHTING");
    if (!enabled || std::strcmp(enabled, "1") != 0) GTEST_SKIP() << "Set CONTAINER_RUN_GPU_LIGHTING=1";
    ASSERT_EQ(glfwInit(), GLFW_TRUE); started = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Staged lighting generations", nullptr, nullptr); ASSERT_NE(window, nullptr);
    uint32_t count{}; const auto* extensions = glfwGetRequiredInstanceExtensions(&count); ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info; info.enableValidationLayers = true; info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + count); info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1; validation.pEnabledValidationFeatures = &sync; info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(callback); debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR handle{}; ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &handle), VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), handle);
    DeviceCreateInfo deviceInfo; deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE; v13.pNext = &v12; deviceInfo.next = &v13;
    device = std::make_shared<VulkanDevice>(*instance, handle, deviceInfo);
    commands = vk::raii::CommandPool(device->raii(), vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer, device->queueFamilyIndices().graphicsFamily.value()));
    allocations = std::make_unique<AllocationManager>();
    allocations->initialize(instance->instance(), device->physicalDevice(), device->device(), device->graphicsQueue(), *commands, {});
    pipelines = std::make_unique<PipelineManager>(device->device());
    container::app::AppConfig config;
    scene = std::make_unique<container::scene::SceneManager>(*allocations, *pipelines, device, config);
    detachedScene = std::make_unique<container::scene::SceneManager>(*allocations, *pipelines, device, config);
    root = graph.createNode(glm::mat4(1), 0); graph.getNode(root)->editorSceneRoot = true;
    detachedGraph = graph;
    (void)world.setActiveCamera({});
    lighting = std::make_unique<LightingManager>(device, *allocations, *pipelines, scene.get(), graph, world);
    lighting->setRootNode(root); lighting->createDescriptorResources(2);
    lighting->createTiledResources(CONTAINER_LIGHTING_SHADER_ROOT, {32, 32});
    lighting->updateLightingData(); lighting->uploadLightingData();
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    lighting.reset(); detachedScene.reset(); scene.reset(); pipelines.reset(); allocations.reset(); commands.clear();
    device.reset(); surface.clear(); messenger.clear(); instance.reset();
    if (window) glfwDestroyWindow(window); if (started) glfwTerminate();
    EXPECT_EQ(errors.load(), 0u);
  }
  uint32_t allocationCount() const {
    VmaTotalStatistics stats{}; vmaCalculateStatistics(allocations->memoryManager()->allocator(), &stats);
    return stats.total.statistics.allocationCount;
  }
  template<class T> T read(const AllocatedBuffer& buffer) const {
    T value{};
    if (!buffer.allocation || !buffer.allocation_info.pMappedData) throw std::runtime_error("Missing staged host allocation");
    if (vmaInvalidateAllocation(allocations->memoryManager()->allocator(), buffer.allocation, 0, sizeof(T)) != VK_SUCCESS)
      throw std::runtime_error("Could not invalidate staged lighting allocation");
    std::memcpy(&value, buffer.allocation_info.pMappedData, sizeof(T)); return value;
  }
  SceneLightEdits edits() const {
    auto result = lighting->snapshotSceneLightEdits();
    PointLightData point; point.positionRadius = {3, 2, 1, 4}; point.colorIntensity = {.25f, .5f, 1, 7};
    AreaLightData area; area.positionRange = {-2, 1, 3, 5}; area.colorIntensity = {1, .125f, .5f, 9};
    result.manualPointLights = {point}; result.manualAreaLights = {area};
    EditableLightEntity sun; sun.type = EditableLightType::Directional; sun.source = EditableLightSource::Manual;
    sun.id = {sun.type, sun.source, 0}; sun.direction = {0, -1, 0}; sun.color = {.5f, .25f, 1}; sun.intensity = 3;
    result.directionalOverride = sun;
    return result;
  }
  struct RayResources {
    AllocationManager* allocations{};
    std::array<AllocatedBuffer, 2> settings{};
    container::material::TextureResource visibility;
    ~RayResources() {
      for (auto& buffer : settings) allocations->destroyBuffer(buffer);
    }
  };
  std::unique_ptr<RayResources> rayResources() const {
    auto result = std::make_unique<RayResources>(); result->allocations = allocations.get();
    for (auto& buffer : result->settings) {
      buffer = allocations->createBuffer(64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
    const std::array<std::byte, 4> pixel{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    result->visibility = allocations->createTextureFromRgbaPixels(
        "staged-ray-visibility", pixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    return result;
  }
  void bindRayFrames(LightingManager::PreparedRayShadowBindings& stage, const RayResources& ray) const {
    for (uint32_t image = 0; image < ray.settings.size(); ++image)
      stage.updateRayShadowDescriptors(image, ray.settings[image].buffer, 64, ray.visibility.imageView);
  }
  void bindLiveRayFrames(const RayResources& ray) const {
    for (uint32_t image = 0; image < ray.settings.size(); ++image)
      lighting->updateRayShadowDescriptors(image, ray.settings[image].buffer, 64, ray.visibility.imageView);
  }
  bool started{}; GLFWwindow* window{}; std::atomic<uint32_t> errors{};
  std::unique_ptr<VulkanInstance> instance; vk::raii::DebugUtilsMessengerEXT messenger{nullptr}; vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device; vk::raii::CommandPool commands{nullptr};
  std::unique_ptr<AllocationManager> allocations; std::unique_ptr<PipelineManager> pipelines;
  SceneGraph graph, detachedGraph; container::ecs::World world; uint32_t root{};
  std::unique_ptr<container::scene::SceneManager> scene, detachedScene;
  std::unique_ptr<LightingManager> lighting;
};

TEST_F(LightingGenerationGpu, PreparationIsIsolatedAndPublicationReusesOwnedGenerationsAcrossResize) {
  auto settings = lighting->lightingSettings(); settings.environmentIntensity = .375f; settings.bounceIntensity = .25f;
  lighting->setLightingSettings(settings); lighting->updateLightingData();
  lighting->lightingData().gtaoEnabled = 1; lighting->lightingData().prefilteredMipCount = 7;
  lighting->uploadLightingData();
  auto nextWorld = world.cloneState();
  const auto oldPoint = lighting->pointLightBuffer().buffer, oldArea = lighting->areaLightBuffer().buffer;
  const auto oldSet = lighting->lightDescriptorSet(0), oldTiledSet = lighting->tiledDescriptorSet();
  const auto grid = lighting->tileGridBuffer(); const auto ltcBytes = lighting->ltcMemoryBytes();
  const auto oldLayout = lighting->lightDescriptorSetLayout(); const auto iconSet = lighting->lightGizmoIconDescriptorSet();
  const auto oldUniform = read<LightingData>(lighting->lightingBuffer(0));
  const auto count = allocationCount(); const auto desired = edits();
  auto next = lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, desired, 2);
  ASSERT_TRUE(next); ASSERT_TRUE(lighting->canExchangeGeneration(next));
  EXPECT_EQ(allocationCount(), count + 3 * next.lightingBuffers().size()); // UBO and two SSBOs per image
  EXPECT_EQ(world.pointLightCount(), 0u); EXPECT_EQ(nextWorld->pointLightCount(), 1u); EXPECT_TRUE(nextWorld->hasActiveCamera());
  EXPECT_EQ(lighting->pointLightsSsbo().size(), 0u); EXPECT_EQ(lighting->lightDescriptorSet(0), oldSet);
  EXPECT_EQ(lighting->pointLightBuffer().buffer, oldPoint); EXPECT_EQ(lighting->tileGridBuffer(), grid);
  ASSERT_EQ(next.pointLightsSsbo().size(), 1u); ASSERT_EQ(next.areaLightsSsbo().size(), 1u);
  EXPECT_EQ(next.pointLightsSsbo()[0].positionRadius, desired.manualPointLights[0].positionRadius);
  EXPECT_EQ(next.pointLightsSsbo()[0].colorIntensity, desired.manualPointLights[0].colorIntensity);
  EXPECT_FLOAT_EQ(next.pointLightsSsbo()[0].coneOuterCosType.z, 1); EXPECT_FLOAT_EQ(next.pointLightsSsbo()[0].coneOuterCosType.w, 6);
  EXPECT_EQ(next.areaLightsSsbo()[0].colorIntensity, desired.manualAreaLights[0].colorIntensity);
  EXPECT_EQ(read<PointLightData>(next.pointLightBuffer(1)).positionRadius, glm::vec4(3, 2, 1, 4));
  EXPECT_EQ(read<AreaLightData>(next.areaLightBuffer(1)).positionRange, glm::vec4(-2, 1, 3, 5));
  const auto stagedUniform = read<LightingData>(next.lightingBuffers()[1]);
  EXPECT_EQ(stagedUniform.pointLightCount, 1u); EXPECT_EQ(stagedUniform.areaLightCount, 1u);
  EXPECT_FLOAT_EQ(stagedUniform.environmentIntensity, .375f); EXPECT_FLOAT_EQ(stagedUniform.bounceIntensity, .25f);
  EXPECT_EQ(stagedUniform.gtaoEnabled, 1u); EXPECT_EQ(stagedUniform.prefilteredMipCount, 7u);
  EXPECT_EQ(stagedUniform.directionalColorIntensity, glm::vec4(.5f, .25f, 1, 3));
  EXPECT_EQ(stagedUniform.areaLightingMode, lighting->ltcReady() ? 1u : 0u);
  EXPECT_NE(next.pointLightBuffer().buffer, oldPoint); EXPECT_NE(next.areaLightBuffer().buffer, oldArea);
  EXPECT_NE(next.lightDescriptorSet(0), oldSet); EXPECT_NE(next.tiledDescriptorSet(), oldTiledSet);
  const auto freshPoint = next.pointLightBuffer().buffer;
  const auto freshSet = next.lightDescriptorSet(0);
  LightingManager::PreparedGeneration moved(std::move(next)); EXPECT_FALSE(next);
  ASSERT_TRUE(lighting->exchangeGeneration(moved));
  EXPECT_EQ(lighting->pointLightBuffer().buffer, freshPoint); EXPECT_EQ(lighting->lightDescriptorSet(0), freshSet);
  EXPECT_EQ(moved.pointLightBuffer().buffer, oldPoint); EXPECT_EQ(moved.lightDescriptorSet(0), oldSet);
  EXPECT_EQ(read<LightingData>(moved.lightingBuffers()[0]).pointLightCount, oldUniform.pointLightCount);
  EXPECT_EQ(lighting->lightDescriptorSetLayout(), oldLayout); EXPECT_EQ(lighting->lightGizmoIconDescriptorSet(), iconSet);
  EXPECT_EQ(lighting->ltcMemoryBytes(), ltcBytes);
  lighting->resizeTiledResources({96, 64});
  ASSERT_TRUE(lighting->exchangeGeneration(moved)); EXPECT_EQ(lighting->pointLightBuffer().buffer, oldPoint);
  ASSERT_TRUE(lighting->exchangeGeneration(moved)); EXPECT_EQ(lighting->pointLightBuffer().buffer, freshPoint);
  EXPECT_EQ(read<LightingData>(lighting->lightingBuffer(1)).pointLightCount, 1u);
  EXPECT_EQ(lighting->lightDescriptorSet(0), freshSet); EXPECT_EQ(allocationCount(), count + 3 * lighting->lightingBuffers().size());
  nextWorld->clearPointLights();
  lighting->updateLightingData();
  EXPECT_EQ(world.pointLightCount(), 1u); EXPECT_EQ(nextWorld->pointLightCount(), 0u);
  const auto retainedEdits = lighting->snapshotSceneLightEdits();
  ASSERT_TRUE(retainedEdits.directionalOverride); EXPECT_EQ(retainedEdits.directionalOverride->source, EditableLightSource::Manual);
  EXPECT_EQ(retainedEdits.manualPointLights.size(), 1u); EXPECT_EQ(retainedEdits.manualAreaLights.size(), 1u);
}

TEST_F(LightingGenerationGpu, InvalidAndAbandonedStagesCannotMutateLiveBuffersOrLeakAllocations) {
  const auto count = allocationCount();
  const auto oldSet = lighting->lightDescriptorSet(0);
  const auto oldBuffer = lighting->lightingBuffer(0).buffer;
  auto desired = edits(); auto nextWorld = world.cloneState();
  desired.manualPointLights.resize(kMaxClusteredLights + 1);
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, desired, 2), std::invalid_argument);
  EXPECT_EQ(nextWorld->pointLightCount(), 0u); EXPECT_EQ(allocationCount(), count);
  desired = edits(); desired.manualAreaLights.resize(kMaxAreaLights + 1);
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, desired, 2), std::invalid_argument);
  desired = edits();
  EXPECT_THROW((void)lighting->prepareGeneration(*scene, detachedGraph, *nextWorld, root, desired, 2), std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, graph, *nextWorld, root, desired, 2), std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, detachedGraph, world, root, desired, 2), std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, desired, 0), std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, 123456, desired, 2), std::invalid_argument);
  desired.manualPointLights[0].colorIntensity.x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, desired, 2), std::invalid_argument);
  EXPECT_EQ(allocationCount(), count); desired = edits();
  {
    auto abandoned = lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, desired, 2);
    EXPECT_EQ(allocationCount(), count + 3 * lighting->lightingBuffers().size());
    LightingManager foreign(device, *allocations, *pipelines, scene.get(), graph, world);
    EXPECT_FALSE(foreign.canExchangeGeneration(abandoned)); EXPECT_FALSE(foreign.exchangeGeneration(abandoned));
    EXPECT_EQ(lighting->lightDescriptorSet(0), oldSet); EXPECT_EQ(lighting->lightingBuffer(0).buffer, oldBuffer);
  }
  EXPECT_EQ(allocationCount(), count); EXPECT_EQ(lighting->lightDescriptorSet(0), oldSet);
  EXPECT_EQ(lighting->lightingBuffer(0).buffer, oldBuffer); EXPECT_EQ(world.pointLightCount(), 0u);
}

TEST_F(LightingGenerationGpu, RayBindingPreparationAndDiscardLeaveLiveResourcesUntouched) {
  auto ray = rayResources();
  const auto count = allocationCount();
  const auto pool = lighting->lightDescriptorPool();
  const auto set0 = lighting->lightDescriptorSet(0), set1 = lighting->lightDescriptorSet(1);
  const auto ubo0 = lighting->lightingBuffer(0).buffer, ubo1 = lighting->lightingBuffer(1).buffer;
  const auto bytes = read<LightingBytes>(lighting->lightingBuffer(0));
  EXPECT_THROW((void)lighting->prepareRayShadowBindings(0), std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareRayShadowBindings(1), std::invalid_argument);
  EXPECT_THROW((void)lighting->prepareRayShadowBindings(65), std::invalid_argument);
  {
    auto stage = lighting->prepareRayShadowBindings(2);
    ASSERT_TRUE(stage);
    EXPECT_NE(stage.lightDescriptorSet(0), set0); EXPECT_NE(stage.lightDescriptorSet(1), set1);
    EXPECT_EQ(stage.lightDescriptorSet(2), VK_NULL_HANDLE);
    EXPECT_FALSE(lighting->canExchangeRayShadowBindings(stage)); // no ray rows yet
    stage.updateRayShadowDescriptors(0, ray->settings[0].buffer, 64, ray->visibility.imageView);
    EXPECT_FALSE(lighting->canExchangeRayShadowBindings(stage)); // every frame is required
    stage.updateRayShadowDescriptors(1, ray->settings[1].buffer, 64, ray->visibility.imageView);
    ASSERT_TRUE(lighting->canExchangeRayShadowBindings(stage));
    LightingManager::PreparedRayShadowBindings moved(std::move(stage));
    EXPECT_FALSE(stage); EXPECT_EQ(stage.lightDescriptorSet(0), VK_NULL_HANDLE);
    ASSERT_TRUE(lighting->canExchangeRayShadowBindings(moved));
    EXPECT_EQ(allocationCount(), count); // no UBO/SSBO/image allocation by the stage
    EXPECT_EQ(lighting->lightDescriptorPool(), pool);
    EXPECT_EQ(lighting->lightDescriptorSet(0), set0); EXPECT_EQ(lighting->lightDescriptorSet(1), set1);
  }
  EXPECT_EQ(allocationCount(), count);
  EXPECT_EQ(lighting->lightDescriptorPool(), pool);
  EXPECT_EQ(lighting->lightingBuffer(0).buffer, ubo0); EXPECT_EQ(lighting->lightingBuffer(1).buffer, ubo1);
  EXPECT_EQ(read<LightingBytes>(lighting->lightingBuffer(0)), bytes);
  EXPECT_EQ(world.pointLightCount(), 0u);
}

TEST_F(LightingGenerationGpu, RayBindingExchangeReplacesEverySetWithoutChangingLightOrWorldPayloads) {
  lighting->restoreSceneLightEdits(edits()); lighting->updateLightingData(); lighting->uploadLightingData();
  auto ray = rayResources();
  const auto count = allocationCount();
  const auto point = lighting->pointLightBuffer().buffer, area = lighting->areaLightBuffer().buffer;
  const auto ubo0 = lighting->lightingBuffer(0).buffer, ubo1 = lighting->lightingBuffer(1).buffer;
  const auto grid = lighting->tileGridBuffer(); const auto tiled = lighting->tiledDescriptorSet();
  const auto ltc = lighting->ltcMemoryBytes(); const auto icon = lighting->lightGizmoIconDescriptorSet();
  const auto oldPool = lighting->lightDescriptorPool(); const auto layout = lighting->lightDescriptorSetLayout();
  const auto data0 = read<LightingBytes>(lighting->lightingBuffer(0));
  const auto data1 = read<LightingBytes>(lighting->lightingBuffer(1));
  const auto pointData = read<PointBytes>(lighting->pointLightBuffer());
  const auto areaData = read<AreaBytes>(lighting->areaLightBuffer());
  const auto worldPointCount = world.pointLightCount();
  auto stage = lighting->prepareRayShadowBindings(2); bindRayFrames(stage, *ray);
  const auto fresh0 = stage.lightDescriptorSet(0), fresh1 = stage.lightDescriptorSet(1);
  ASSERT_TRUE(lighting->exchangeRayShadowBindings(stage));
  EXPECT_FALSE(stage); EXPECT_FALSE(lighting->canExchangeRayShadowBindings(stage));
  EXPECT_FALSE(lighting->exchangeRayShadowBindings(stage));
  EXPECT_EQ(lighting->lightDescriptorSet(0), fresh0); EXPECT_EQ(lighting->lightDescriptorSet(1), fresh1);
  EXPECT_NE(lighting->lightDescriptorPool(), oldPool); EXPECT_EQ(lighting->lightDescriptorSetLayout(), layout);
  EXPECT_EQ(lighting->lightingBuffer(0).buffer, ubo0); EXPECT_EQ(lighting->lightingBuffer(1).buffer, ubo1);
  EXPECT_EQ(lighting->pointLightBuffer().buffer, point); EXPECT_EQ(lighting->areaLightBuffer().buffer, area);
  EXPECT_EQ(lighting->tileGridBuffer(), grid); EXPECT_EQ(lighting->tiledDescriptorSet(), tiled);
  EXPECT_EQ(lighting->ltcMemoryBytes(), ltc); EXPECT_EQ(lighting->lightGizmoIconDescriptorSet(), icon);
  EXPECT_EQ(read<LightingBytes>(lighting->lightingBuffer(0)), data0);
  EXPECT_EQ(read<LightingBytes>(lighting->lightingBuffer(1)), data1);
  EXPECT_EQ(read<PointBytes>(lighting->pointLightBuffer()), pointData);
  EXPECT_EQ(read<AreaBytes>(lighting->areaLightBuffer()), areaData);
  EXPECT_EQ(world.pointLightCount(), worldPointCount); EXPECT_TRUE(world.hasActiveCamera());
  const auto retained = lighting->snapshotSceneLightEdits();
  ASSERT_TRUE(retained.directionalOverride); EXPECT_EQ(retained.manualPointLights.size(), 1u);
  EXPECT_EQ(retained.manualAreaLights.size(), 1u); EXPECT_EQ(allocationCount(), count);
  // Live CPU uploads still target the original buffers, independent of the
  // descriptor-only generation. A fresh stage copies both published ray rows.
  lighting->lightingData().prefilteredMipCount = 11; lighting->uploadLightingData();
  EXPECT_EQ(read<LightingData>(lighting->lightingBuffer(1)).prefilteredMipCount, 11u);
  auto inherited = lighting->prepareRayShadowBindings(2);
  EXPECT_TRUE(lighting->canExchangeRayShadowBindings(inherited));
}

TEST_F(LightingGenerationGpu, RayBindingStagesRejectForeignIncompleteAndStalePublicationAtomically) {
  auto ray = rayResources(); bindLiveRayFrames(*ray);
  auto stage = lighting->prepareRayShadowBindings(2);
  const auto livePool = lighting->lightDescriptorPool(); const auto liveSet = lighting->lightDescriptorSet(0);
  ASSERT_TRUE(lighting->canExchangeRayShadowBindings(stage));
  EXPECT_THROW(stage.updateRayShadowDescriptors(2, ray->settings[0].buffer, 64, ray->visibility.imageView), std::invalid_argument);
  EXPECT_THROW(stage.updateRayShadowDescriptors(0, VK_NULL_HANDLE, 64, ray->visibility.imageView), std::invalid_argument);
  EXPECT_THROW(stage.updateRayShadowDescriptors(0, ray->settings[0].buffer, 0, ray->visibility.imageView), std::invalid_argument);
  EXPECT_THROW(stage.updateRayShadowDescriptors(0, ray->settings[0].buffer, VK_WHOLE_SIZE, ray->visibility.imageView), std::invalid_argument);
  EXPECT_THROW(stage.updateRayShadowDescriptors(0, ray->settings[0].buffer, 64, VK_NULL_HANDLE), std::invalid_argument);
  EXPECT_THROW(lighting->updateRayShadowDescriptors(2, ray->settings[0].buffer, 64, ray->visibility.imageView), std::invalid_argument);
  EXPECT_THROW(lighting->updateRayShadowDescriptors(0, ray->settings[0].buffer, VK_WHOLE_SIZE, ray->visibility.imageView), std::invalid_argument);
  // Invalid requests and repeating an identical live binding do not stale it.
  bindLiveRayFrames(*ray); ASSERT_TRUE(lighting->canExchangeRayShadowBindings(stage));
  LightingManager foreign(device, *allocations, *pipelines, scene.get(), graph, world);
  EXPECT_FALSE(foreign.canExchangeRayShadowBindings(stage)); EXPECT_FALSE(foreign.exchangeRayShadowBindings(stage));
  lighting->updateRayShadowDescriptors(0, ray->settings[1].buffer, 64, ray->visibility.imageView);
  EXPECT_FALSE(lighting->canExchangeRayShadowBindings(stage)); EXPECT_FALSE(lighting->exchangeRayShadowBindings(stage));
  EXPECT_EQ(lighting->lightDescriptorPool(), livePool); EXPECT_EQ(lighting->lightDescriptorSet(0), liveSet);
  // Restoring the old handles cannot revive a stale epoch (ABA).
  lighting->updateRayShadowDescriptors(0, ray->settings[0].buffer, 64, ray->visibility.imageView);
  EXPECT_FALSE(lighting->canExchangeRayShadowBindings(stage));
  auto first = lighting->prepareRayShadowBindings(2), second = lighting->prepareRayShadowBindings(2);
  ASSERT_TRUE(lighting->exchangeRayShadowBindings(first));
  const auto publishedPool = lighting->lightDescriptorPool();
  EXPECT_FALSE(lighting->exchangeRayShadowBindings(second)); EXPECT_EQ(lighting->lightDescriptorPool(), publishedPool);
  auto recreated = lighting->prepareRayShadowBindings(2);
  lighting->createDescriptorResources(2);
  const auto recreatedPool = lighting->lightDescriptorPool();
  EXPECT_FALSE(lighting->exchangeRayShadowBindings(recreated));
  EXPECT_EQ(lighting->lightDescriptorPool(), recreatedPool);
}

TEST_F(LightingGenerationGpu, FullGenerationExchangesInvalidateRayStagesAndRemainReversible) {
  auto ray = rayResources(); bindLiveRayFrames(*ray);
  auto nextWorld = world.cloneState();
  auto full = lighting->prepareGeneration(*detachedScene, detachedGraph, *nextWorld, root, edits(), 2);
  const auto oldPoint = lighting->pointLightBuffer().buffer;
  const auto newPoint = full.pointLightBuffer().buffer;
  auto oldStage = lighting->prepareRayShadowBindings(2);
  auto replacement = lighting->prepareRayShadowBindings(2);
  // Descriptor-only exchange does not invalidate a complete owned generation.
  ASSERT_TRUE(lighting->exchangeRayShadowBindings(replacement));
  ASSERT_TRUE(lighting->canExchangeGeneration(full));
  ASSERT_TRUE(lighting->exchangeGeneration(full));
  EXPECT_EQ(lighting->pointLightBuffer().buffer, newPoint);
  EXPECT_FALSE(lighting->exchangeRayShadowBindings(oldStage));
  auto current = lighting->prepareRayShadowBindings(2);
  ASSERT_TRUE(lighting->canExchangeRayShadowBindings(current));
  ASSERT_TRUE(lighting->exchangeGeneration(full));
  EXPECT_EQ(lighting->pointLightBuffer().buffer, oldPoint);
  EXPECT_FALSE(lighting->exchangeRayShadowBindings(current));
  // Returning to exactly the same generation still invalidates earlier stages.
  ASSERT_TRUE(lighting->exchangeGeneration(full));
  EXPECT_EQ(lighting->pointLightBuffer().buffer, newPoint);
  EXPECT_FALSE(lighting->canExchangeRayShadowBindings(current));
  auto fresh = lighting->prepareRayShadowBindings(2);
  ASSERT_TRUE(lighting->exchangeRayShadowBindings(fresh));
  EXPECT_EQ(lighting->pointLightBuffer().buffer, newPoint);
  ASSERT_TRUE(lighting->exchangeGeneration(full));
  EXPECT_EQ(lighting->pointLightBuffer().buffer, oldPoint);
}

TEST_F(LightingGenerationGpu, AbandonedRayStageCanOutliveItsOwnerButCannotBindToAReusedAddress) {
  auto ray = rayResources();
  std::optional<LightingManager> owner;
  owner.emplace(device, *allocations, *pipelines, scene.get(), graph, world);
  owner->createDescriptorResources(2); owner->createTiledResources(CONTAINER_LIGHTING_SHADER_ROOT, {32, 32});
  auto stage = owner->prepareRayShadowBindings(2); bindRayFrames(stage, *ray);
  const auto* address = &*owner;
  ASSERT_TRUE(owner->canExchangeRayShadowBindings(stage));
  owner.reset();
  EXPECT_THROW(stage.updateRayShadowDescriptors(0, ray->settings[0].buffer, 64, ray->visibility.imageView), std::invalid_argument);
  owner.emplace(device, *allocations, *pipelines, scene.get(), graph, world);
  owner->createDescriptorResources(2); owner->createTiledResources(CONTAINER_LIGHTING_SHADER_ROOT, {32, 32});
  EXPECT_EQ(&*owner, address);
  EXPECT_FALSE(owner->canExchangeRayShadowBindings(stage)); EXPECT_FALSE(owner->exchangeRayShadowBindings(stage));
  owner.reset(); // the detached pool remains safely owned until stage teardown
}
} // namespace
