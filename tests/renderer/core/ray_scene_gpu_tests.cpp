#include "Container/common/CommonGLFW.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>

namespace {
using namespace container::renderer;
using namespace container::gpu;

struct TestBuffer {
  VulkanMemoryManager &memory;
  AllocatedBuffer buffer;
  TestBuffer(VulkanMemoryManager &allocator, size_t size)
      : memory(allocator), buffer(memory.createBuffer(
                               size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT)) {}
  ~TestBuffer() { memory.destroyBuffer(buffer); }
};

struct Probe {
  glm::vec4 originAndMin;
  glm::vec4 directionAndMax;
  glm::uvec4 options;
};
static_assert(sizeof(Probe) == 48);

class RaySceneGpu : public ::testing::Test {
protected:
  virtual bool enableQueries() const { return true; }
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<RaySceneGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const char *enabled = std::getenv("CONTAINER_RUN_GPU_RAY_QUERY");
    if (enabled == nullptr || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP()
          << "Set CONTAINER_RUN_GPU_RAY_QUERY=1 for real Vulkan builds/queries";
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(64, 64, "Ray-query contract", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count = 0;
    const auto extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + count);
    info.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const VkValidationFeatureEnableEXT sync =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance = std::make_unique<VulkanInstance>(info);
    VkDebugUtilsMessengerCreateInfoEXT debug{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validationCallback;
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(
        instance->raii(),
        reinterpret_cast<const vk::DebugUtilsMessengerCreateInfoEXT &>(debug));
    VkSurfaceKHR surfaceHandle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr,
                                      &surfaceHandle),
              VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), surfaceHandle);
    DeviceCreateInfo deviceInfo;
    deviceInfo.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    deviceInfo.enableRayQueries = enableQueries();
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    deviceInfo.next = &v13;
    device =
        std::make_unique<VulkanDevice>(*instance, surfaceHandle, deviceInfo);
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
  }
  void TearDown() override {
    if (device)
      device->raii().waitIdle();
    command.clear();
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
  void begin() {
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
  }
  void submit() {
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
  std::vector<glm::uvec4> trace(const RaySceneGeneration &generation,
                                std::span<const Probe> probes) {
    TestBuffer inputs(*memory, probes.size_bytes());
    TestBuffer output(*memory, probes.size() * sizeof(glm::uvec4));
    std::memcpy(inputs.buffer.allocation_info.pMappedData, probes.data(),
                probes.size_bytes());
    if (vmaFlushAllocation(memory->allocator(), inputs.buffer.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("probe upload failed");
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{
        vk::DescriptorSetLayoutBinding(
            0, vk::DescriptorType::eAccelerationStructureKHR, 1,
            vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute)};
    vk::raii::DescriptorSetLayout layout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const vk::DescriptorSetLayout layoutHandle = *layout;
    vk::raii::PipelineLayout pipelineLayout(
        device->raii(), vk::PipelineLayoutCreateInfo({}, layoutHandle));
    std::array<vk::DescriptorPoolSize, 2> sizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR,
                               1),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 2)};
    vk::raii::DescriptorPool descriptorPool(
        device->raii(),
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    auto sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptorPool, layoutHandle));
    const vk::AccelerationStructureKHR tlas = generation.tlas();
    vk::WriteDescriptorSetAccelerationStructureKHR accelerationWrite(tlas);
    std::array<vk::DescriptorBufferInfo, 2> bufferInfo{
        vk::DescriptorBufferInfo(inputs.buffer.buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(output.buffer.buffer, 0, VK_WHOLE_SIZE)};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
      writes[i].dstSet = *sets.front();
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType =
          i == 0 ? vk::DescriptorType::eAccelerationStructureKHR
                 : vk::DescriptorType::eStorageBuffer;
      if (i == 0)
        writes[i].pNext = &accelerationWrite;
      else
        writes[i].pBufferInfo = &bufferInfo[i - 1];
    }
    device->raii().updateDescriptorSets(writes, {});
    std::ifstream file(CONTAINER_RAY_QUERY_CONTRACT_SPIRV,
                       std::ios::binary | std::ios::ate);
    if (!file)
      throw std::runtime_error("ray-query Slang probe is missing");
    const size_t bytes = static_cast<size_t>(file.tellg());
    if (bytes == 0 || bytes % 4)
      throw std::runtime_error("invalid Slang probe SPIR-V");
    std::vector<uint32_t> code(bytes / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char *>(code.data()), bytes);
    vk::raii::ShaderModule shader(device->raii(),
                                  vk::ShaderModuleCreateInfo({}, code));
    vk::PipelineShaderStageCreateInfo stage(
        {}, vk::ShaderStageFlagBits::eCompute, *shader, "computeMain");
    vk::raii::Pipeline pipeline(
        device->raii(), nullptr,
        vk::ComputePipelineCreateInfo({}, stage, *pipelineLayout));
    begin();
    command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout,
                               0, *sets.front(), {});
    command.dispatch(static_cast<uint32_t>(probes.size()), 1, 1);
    vk::MemoryBarrier2 barrier(vk::PipelineStageFlagBits2::eComputeShader,
                               vk::AccessFlagBits2::eShaderWrite,
                               vk::PipelineStageFlagBits2::eHost,
                               vk::AccessFlagBits2::eHostRead);
    vk::DependencyInfo dependency;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    command.pipelineBarrier2(dependency);
    submit();
    if (vmaInvalidateAllocation(memory->allocator(), output.buffer.allocation,
                                0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("probe readback failed");
    std::vector<glm::uvec4> result(probes.size());
    std::memcpy(result.data(), output.buffer.allocation_info.pMappedData,
                result.size() * sizeof(result[0]));
    return result;
  }
  GLFWwindow *window{};
  bool glfwStarted{};
  std::atomic_uint validationErrors{0};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::unique_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
};

class RaySceneRasterFallbackGpu : public RaySceneGpu {
  bool enableQueries() const override { return false; }
};
TEST_F(RaySceneRasterFallbackGpu, DisabledBackendDoesNotCreateRayResources) {
  ASSERT_FALSE(device->rayQueriesEnabled());
  RaySceneAcceleration backend(*device, *memory);
  EXPECT_FALSE(backend.supported());
  EXPECT_THROW((void)backend.recordBuild(command, {}), std::runtime_error);
}

TEST_F(RaySceneGpu,
       TracesSidedMirroredMaskedAndOffscreenGeometryAcrossGenerations) {
  if (!device->rayQueriesEnabled())
    GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
  std::vector<container::geometry::Vertex> vertices(3);
  vertices[0].position = {-1, -1, 0};
  vertices[1].position = {1, -1, 0};
  vertices[2].position = {0, 1, 0};
  auto alphaVertices = vertices;
  std::vector<uint32_t> indices{0, 1, 2};
  std::vector<RaySceneGeometry> geometry{
      {{"mesh"}, 1, 1, vertices, indices, true},
      {{"bim-usd"}, 1, 1, alphaVertices, indices, false}};
  std::vector<RaySceneInstance> instances;
  for (int i = 0; i < 5; ++i)
    instances.push_back({i == 4 ? 1u : 0u, uint32_t(100 + i),
                         glm::translate(glm::mat4(1), glm::vec3(i * 4, 0, 0)),
                         i == 2, uint8_t(i == 3 ? 2 : 0xff)});
  instances[1].transform *= glm::scale(glm::mat4(1), glm::vec3(-1, 2, 0.5f));
  instances.push_back(
      {0, 105, glm::translate(glm::mat4(1), glm::vec3(100, 0, 0))});
  RaySceneAcceleration backend(*device, *memory);
  begin();
  auto first = backend.recordBuild(command, {geometry, instances});
  submit();
  ASSERT_EQ(first->stats().blasBuilt, 2u);
  ASSERT_EQ(first->stats().instanceCount, 6u);
  ASSERT_GT(first->stats().accelerationBytes, 0u);
  const auto probe = [](float x, bool back = false, uint32_t mask = 0xff,
                        uint32_t accept = 0, float maximum = 4) {
    return Probe{{x, 0, back ? -2.f : 2.f, 0.001f},
                 {0, 0, back ? 1.f : -1.f, maximum},
                 {mask, accept, 0, 0}};
  };
  std::vector<Probe> probes{probe(0),
                            probe(0, true),
                            probe(4),
                            probe(8, true),
                            probe(12, false, 1),
                            probe(12, false, 2),
                            probe(16),
                            probe(16, false, 0xff, 1),
                            probe(100),
                            probe(0, false, 0xff, 0, 1)};
  const auto results = trace(*first, probes);
  const std::array<uint32_t, 10> expected{1, 0, 1, 1, 0, 1, 0, 1, 1, 0};
  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(results[i].x, expected[i]) << "probe " << i;
    if (expected[i])
      EXPECT_NEAR(std::bit_cast<float>(results[i].w), 2.f, 0.0001f)
          << "probe " << i;
  }
  EXPECT_EQ(results[0].y, 100u);
  EXPECT_EQ(results[2].y, 101u);
  EXPECT_EQ(results[6].z, 1u);
  EXPECT_EQ(results[7].z, 1u);
  instances[0].transform = glm::translate(glm::mat4(1), glm::vec3(2, 0, 0));
  begin();
  auto moved = backend.recordBuild(command, {geometry, instances}, first);
  submit();
  EXPECT_EQ(moved->stats().blasBuilt, 0u);
  EXPECT_EQ(moved->stats().blasReused, 2u);
  EXPECT_NE(first->tlas(), moved->tlas());
  const std::array<Probe, 2> motionProbes{probe(0), probe(2)};
  const auto movedResults = trace(*moved, motionProbes);
  EXPECT_EQ(movedResults[0].x, 0u);
  EXPECT_EQ(movedResults[1].x, 1u);
  // The preceding frame retains its original TLAS and remains usable.
  const auto oldResults = trace(*first, motionProbes);
  EXPECT_EQ(oldResults[0].x, 1u);
  EXPECT_EQ(oldResults[1].x, 0u);
  ++geometry[1].revision;
  for (auto &vertex : alphaVertices)
    vertex.position.z = -1.f;
  begin();
  auto revised = backend.recordBuild(command, {geometry, instances}, moved);
  submit();
  EXPECT_EQ(revised->stats().blasBuilt, 1u);
  EXPECT_EQ(revised->stats().blasReused, 1u);
  const std::array<Probe, 1> acceptedCutout{probe(16, false, 0xff, 1)};
  EXPECT_NEAR(std::bit_cast<float>(trace(*revised, acceptedCutout)[0].w), 3.f,
              0.0001f);
  EXPECT_NEAR(std::bit_cast<float>(trace(*moved, acceptedCutout)[0].w), 2.f,
              0.0001f);
  // Alpha classification changes are BLAS changes even with identical topology.
  geometry[1].opaque = true;
  begin();
  auto opaque = backend.recordBuild(command, {geometry, instances}, revised);
  submit();
  EXPECT_EQ(opaque->stats().blasBuilt, 1u);
  const std::array<Probe, 1> cutoutProbe{probe(16)};
  EXPECT_EQ(trace(*opaque, cutoutProbe)[0].x, 1u);
  begin();
  auto empty = backend.recordBuild(command, {}, opaque);
  submit();
  EXPECT_EQ(empty->stats().instanceCount, 0u);
  EXPECT_EQ(trace(*empty, cutoutProbe)[0].x, 0u);
  std::printf("Ray scene payload: AS=%llu, input=%llu, scratch=%llu bytes\n",
              static_cast<unsigned long long>(first->stats().accelerationBytes),
              static_cast<unsigned long long>(first->stats().inputBytes),
              static_cast<unsigned long long>(first->stats().scratchBytes));
}

TEST_F(RaySceneGpu, KeepsUnchangedProviderBlasAcrossProviderChanges) {
  if (!device->rayQueriesEnabled())
    GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
  std::vector<container::geometry::Vertex> bimVertices(3);
  bimVertices[0].position = {-1, -1, 0};
  bimVertices[1].position = {1, -1, 0};
  bimVertices[2].position = {0, 1, 0};
  const auto meshVertices = bimVertices;
  const std::vector<uint32_t> indices{0, 1, 2};
  std::vector<container::gpu::ObjectData> bimObjects(1), meshObjects(1);
  meshObjects[0].model =
      glm::translate(glm::mat4(1), glm::vec3(4, 0, 0));
  const std::vector<DrawCommand> draws{{0, 0, 3}};
  std::vector<RaySceneProviderSource> providers{
      {{"bim"}, 7, bimVertices, indices, bimObjects, draws}};
  RaySceneExtractionCache cache;
  RaySceneAcceleration backend(*device, *memory);
  auto build = [&](const std::shared_ptr<const RaySceneGeneration> &previous) {
    const auto snapshot = cache.update(providers);
    const auto views = snapshot.geometry->geometryViews();
    begin();
    auto generation = backend.recordBuild(
        command, {views, snapshot.instances.instances}, previous);
    submit();
    return generation;
  };

  const auto first = build({});
  EXPECT_EQ(first->stats().blasBuilt, 1u);
  providers.insert(providers.begin(),
                   {{"mesh"}, 1, meshVertices, indices, meshObjects, draws});
  const auto added = build(first);
  EXPECT_EQ(added->stats().blasBuilt, 1u);
  EXPECT_EQ(added->stats().blasReused, 1u);
  std::swap(providers[0], providers[1]);
  const auto reordered = build(added);
  EXPECT_EQ(reordered->stats().blasBuilt, 0u);
  EXPECT_EQ(reordered->stats().blasReused, 2u);

  // Replacement storage has the same provider ID, topology and revision, but
  // different positions. Reuse the BIM BLAS and rebuild only the mesh BLAS.
  auto replacement = meshVertices;
  for (auto &vertex : replacement)
    vertex.position.z = -1;
  providers[1].vertices = replacement;
  (void)cache.update(providers); // A second CPU update may precede submission.
  const auto replaced = build(reordered);
  EXPECT_EQ(replaced->stats().blasBuilt, 1u);
  EXPECT_EQ(replaced->stats().blasReused, 1u);
  const std::array<Probe, 2> probes{
      Probe{{0, 0, 2, 0.001f}, {0, 0, -1, 4}, {0xff, 1, 0, 0}},
      Probe{{4, 0, 2, 0.001f}, {0, 0, -1, 4}, {0xff, 1, 0, 0}}};
  const auto replacementHits = trace(*replaced, probes);
  ASSERT_EQ(replacementHits[0].x, 1u);
  ASSERT_EQ(replacementHits[1].x, 1u);
  EXPECT_NEAR(std::bit_cast<float>(replacementHits[0].w), 2.f, 0.0001f);
  EXPECT_NEAR(std::bit_cast<float>(replacementHits[1].w), 3.f, 0.0001f);
  // The retained generation still traces its original mesh storage.
  const auto originalHits = trace(*reordered, probes);
  EXPECT_NEAR(std::bit_cast<float>(originalHits[1].w), 2.f, 0.0001f);

  providers.pop_back();
  const auto removed = build(replaced);
  EXPECT_EQ(removed->stats().blasBuilt, 0u);
  EXPECT_EQ(removed->stats().blasReused, 1u);
  const auto removedHits = trace(*removed, probes);
  EXPECT_EQ(removedHits[0].x, 1u);
  EXPECT_EQ(removedHits[1].x, 0u);
}
} // namespace
