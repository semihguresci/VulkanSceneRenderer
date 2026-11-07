#include "Container/common/CommonGLFW.h"
#include "Container/geometry/GltfOpacityMicromaps.h"
#include "Container/renderer/raytracing/RaySceneAcceleration.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"
#include "Container/renderer/raytracing/RayInstanceTransformPass.h"
#include "Container/utility/PipelineManager.h"
#include <gtest/gtest.h>

#include <array>
#include <algorithm>
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
  virtual bool enableMicromaps() const { return false; }
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
    deviceInfo.enableOpacityMicromaps = enableMicromaps();
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
                                std::span<const Probe> probes,
                                std::span<const RayTriangleData> triangles = {},
                                double* elapsedNanoseconds = nullptr) {
    TestBuffer inputs(*memory, probes.size_bytes());
    TestBuffer output(*memory, probes.size() * sizeof(glm::uvec4));
    TestBuffer attributes(*memory, std::max(triangles.size_bytes(), sizeof(RayTriangleData)));
    if (!triangles.empty())
      std::memcpy(attributes.buffer.allocation_info.pMappedData, triangles.data(), triangles.size_bytes());
    if (vmaFlushAllocation(memory->allocator(), attributes.buffer.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("triangle metadata upload failed");
    std::memcpy(inputs.buffer.allocation_info.pMappedData, probes.data(),
                probes.size_bytes());
    if (vmaFlushAllocation(memory->allocator(), inputs.buffer.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("probe upload failed");
    std::array<vk::DescriptorSetLayoutBinding, 4> bindings{
        vk::DescriptorSetLayoutBinding(
            0, vk::DescriptorType::eAccelerationStructureKHR, 1,
            vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute)};
    vk::raii::DescriptorSetLayout layout(
        device->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const vk::DescriptorSetLayout layoutHandle = *layout;
    vk::raii::PipelineLayout pipelineLayout(
        device->raii(), vk::PipelineLayoutCreateInfo({}, layoutHandle));
    std::array<vk::DescriptorPoolSize, 2> sizes{
        vk::DescriptorPoolSize(vk::DescriptorType::eAccelerationStructureKHR,
                               1),
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3)};
    vk::raii::DescriptorPool descriptorPool(
        device->raii(),
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    auto sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptorPool, layoutHandle));
    const vk::AccelerationStructureKHR tlas = generation.tlas();
    vk::WriteDescriptorSetAccelerationStructureKHR accelerationWrite(tlas);
    std::array<vk::DescriptorBufferInfo, 3> bufferInfo{
        vk::DescriptorBufferInfo(inputs.buffer.buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(output.buffer.buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(attributes.buffer.buffer, 0, VK_WHOLE_SIZE)};
    std::array<vk::WriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
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
    vk::raii::QueryPool timing{nullptr};
    uint32_t timestampBits = 0;
    if (elapsedNanoseconds) {
      uint32_t count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(), &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(device->physicalDevice(), &count, families.data());
      timestampBits = families.at(device->queueFamilyIndices().graphicsFamily.value()).timestampValidBits;
      if (timestampBits)
        timing = vk::raii::QueryPool(device->raii(), vk::QueryPoolCreateInfo({}, vk::QueryType::eTimestamp, 2));
      *elapsedNanoseconds = 0;
    }
    begin();
    command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout,
                               0, *sets.front(), {});
    if (timestampBits) {
      command.resetQueryPool(*timing, 0, 2);
      command.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, *timing, 0);
    }
    command.dispatch(static_cast<uint32_t>(probes.size()), 1, 1);
    if (timestampBits)
      command.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, *timing, 1);
    vk::MemoryBarrier2 barrier(vk::PipelineStageFlagBits2::eComputeShader,
                               vk::AccessFlagBits2::eShaderWrite,
                               vk::PipelineStageFlagBits2::eHost,
                               vk::AccessFlagBits2::eHostRead);
    vk::DependencyInfo dependency;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    command.pipelineBarrier2(dependency);
    submit();
    if (timestampBits) {
      std::array<uint64_t, 2> timestamps{};
      if (vkGetQueryPoolResults(device->device(), *timing, 0, 2, sizeof(timestamps), timestamps.data(),
                                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        throw std::runtime_error("ray query timestamp readback failed");
      VkPhysicalDeviceProperties properties{};
      vkGetPhysicalDeviceProperties(device->physicalDevice(), &properties);
      const uint64_t mask = UINT64_MAX >> (64 - timestampBits);
      *elapsedNanoseconds = double((timestamps[1] - timestamps[0]) & mask) * properties.limits.timestampPeriod;
    }
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

class RaySceneMicromapGpu : public RaySceneGpu {
  bool enableMicromaps() const override { return true; }
};

struct MicromapFixture {
  using Attachment = container::geometry::gltf::GltfOpacityMicromapAttachment;
  using Map = container::geometry::gltf::GltfOpacityMicromap;
  std::array<container::geometry::Vertex, 12> vertices{};
  std::array<uint32_t, 12> indices{};
  std::array<RayTriangleData, 4> alpha{};
  std::shared_ptr<const Attachment> attachment;
  std::shared_ptr<const Attachment> specials;
  std::array<RaySceneGeometry, 2> geometry;
  std::array<RaySceneInstance, 2> instances;

  MicromapFixture() {
    for (uint32_t triangle = 0; triangle < 4; ++triangle) {
      const float x = float(triangle * 3);
      vertices[3 * triangle].position = {x - 1, -1, 0};
      vertices[3 * triangle + 1].position = {x + 1, -1, 0};
      vertices[3 * triangle + 2].position = {x, 1, 0};
      for (uint32_t corner = 0; corner < 3; ++corner)
        indices[3 * triangle + corner] = 3 * triangle + corner;
      alpha[triangle].vertexAlpha = glm::vec4(triangle == 1 || triangle == 2 ? 1.f : 0.f);
    }
    auto source = std::make_shared<Map>();
    source->data = {std::byte{0}, std::byte{1}, std::byte{2}};
    source->triangles = {{0, 0, VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT},
                         {1, 0, VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT},
                         {2, 0, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT}};
    source->usage = {{2, 0, VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT},
                     {1, 0, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT}};
    attachment = std::make_shared<Attachment>(Attachment{source, {0, 1, 2, UINT32_MAX - 3}, source->usage});
    specials = std::make_shared<Attachment>(Attachment{source,
        {UINT32_MAX, UINT32_MAX - 1, UINT32_MAX - 2, UINT32_MAX - 3}, {}});
    geometry = {RaySceneGeometry{{"micromap"}, 1, 1, vertices, indices, false},
                RaySceneGeometry{{"micromap"}, 2, 1, vertices, indices, false}};
    instances = {RaySceneInstance{0, 10},
                 RaySceneInstance{1, 11, glm::translate(glm::mat4(1), glm::vec3(0, 4, 0))}};
  }

  static std::vector<Probe> probes(uint32_t repeats = 1, bool forcedNonOpaque = false) {
    std::vector<Probe> result;
    result.reserve(repeats * 8);
    for (uint32_t repeat = 0; repeat < repeats; ++repeat)
      for (uint32_t copy = 0; copy < 2; ++copy)
        for (uint32_t triangle = 0; triangle < 4; ++triangle)
          result.push_back({{float(triangle * 3), float(copy * 4), 2, .001f}, {0, 0, -1, 4},
              {forcedNonOpaque ? 0x1ffu : 0xffu, std::bit_cast<uint32_t>(1.f), std::bit_cast<uint32_t>(.5f), 2}});
    return result;
  }
};

TEST_F(RaySceneGpu, DisabledMicromapsPreserveSampledAlphaAndOrdinaryReuse) {
  if (!device->rayQueriesEnabled()) GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
  ASSERT_FALSE(device->opacityMicromapsEnabled());
  MicromapFixture fixture;
  fixture.geometry[0].opacityMicromap = fixture.attachment;
  fixture.geometry[1].opacityMicromap = fixture.specials;
  RaySceneAcceleration backend(*device, *memory);
  begin();
  const auto fallback = backend.recordBuild(command, {fixture.geometry, fixture.instances});
  submit();
  ASSERT_EQ(fallback->stats().opacityMicromapCount, 0u);
  EXPECT_EQ(fallback->stats().opacityMicromapFallbackBlas, 2u);
  const auto hits = trace(*fallback, MicromapFixture::probes(), fixture.alpha);
  for (size_t probe = 0; probe < hits.size(); ++probe) {
    EXPECT_EQ(hits[probe].x, probe % 4 == 1 || probe % 4 == 2 ? 1u : 0u);
    EXPECT_EQ(hits[probe].z, 1u);
  }
  fixture.geometry[0].opacityMicromap.reset();
  begin();
  const auto reused = backend.recordBuild(command, {fixture.geometry, fixture.instances}, fallback);
  submit();
  EXPECT_EQ(reused->stats().blasReused, 2u);
}

TEST_F(RaySceneMicromapGpu, BuildsSharedMapWithCutoutParityAndRetainedGenerations) {
  if (!device->opacityMicromapsEnabled()) GTEST_SKIP() << "VK_EXT_opacity_micromap unavailable";
  MicromapFixture fixture;
  RaySceneAcceleration backend(*device, *memory);
  const auto build = [&](const std::shared_ptr<const RaySceneGeneration>& previous = {}) {
    begin();
    auto generation = backend.recordBuild(command, {fixture.geometry, fixture.instances}, previous);
    submit();
    return generation;
  };
  const auto baseline = build();
  fixture.geometry[0].opacityMicromap = fixture.attachment;
  fixture.geometry[1].opacityMicromap = fixture.specials;
  const auto mapped = build(baseline);
  ASSERT_EQ(mapped->stats().blasBuilt, 2u);
  ASSERT_EQ(mapped->stats().opacityMicromapAttachedBlas, 2u);
  EXPECT_EQ(mapped->stats().opacityMicromapCount, 1u);
  EXPECT_EQ(mapped->stats().opacityMicromapBuilt, 1u);
  EXPECT_GT(mapped->stats().opacityMicromapBytes, 0u);
  EXPECT_EQ(mapped->stats().opacityMicromapFallbackBlas, 0u);

  const auto probes = MicromapFixture::probes(512);
  double baselineNs = 0, mappedNs = 0;
  const auto ordinaryHits = trace(*baseline, probes, fixture.alpha, &baselineNs);
  const auto mappedHits = trace(*mapped, probes, fixture.alpha, &mappedNs);
  ASSERT_EQ(ordinaryHits.size(), mappedHits.size());
  for (size_t probe = 0; probe < mappedHits.size(); ++probe) {
    EXPECT_EQ(mappedHits[probe].x, ordinaryHits[probe].x) << "visibility probe " << probe;
    EXPECT_EQ(mappedHits[probe].y, ordinaryHits[probe].y) << "identity probe " << probe;
    EXPECT_EQ(mappedHits[probe].w, ordinaryHits[probe].w) << "distance probe " << probe;
    EXPECT_EQ(mappedHits[probe].z, probe % 4 >= 2 ? 1u : 0u) << "candidate probe " << probe;
  }
  const auto forced = trace(*mapped, MicromapFixture::probes(1, true), fixture.alpha);
  EXPECT_EQ(forced[0].z, 0u); // FORCE_NON_OPAQUE cannot restore known-transparent microtriangles.
  EXPECT_EQ(forced[1].z, 1u); // Known-opaque triangles still reach existing sampled-alpha shaders.
  EXPECT_EQ(forced[4].z, 0u);
  EXPECT_EQ(forced[5].z, 1u);

  const auto reused = build(mapped);
  EXPECT_EQ(reused->stats().blasReused, 2u);
  EXPECT_EQ(reused->stats().opacityMicromapBuilt, 0u);
  const auto total = [](const RaySceneBuildStats& stats) {
    return stats.accelerationAllocatedBytes + stats.inputAllocatedBytes + stats.scratchAllocatedBytes;
  };
  const std::array mappedOnly{mapped};
  EXPECT_EQ(RaySceneGeneration::retainedAllocatedBytes(mappedOnly), total(mapped->stats()));
  const std::array retained{mapped, reused};
  EXPECT_LT(RaySceneGeneration::retainedAllocatedBytes(retained), total(mapped->stats()) + total(reused->stats()));

  auto incompatible = std::make_shared<MicromapFixture::Attachment>(*fixture.attachment);
  incompatible->indices[0] = 8;
  fixture.geometry[0].opacityMicromap = std::move(incompatible);
  const auto rejected = build(reused);
  EXPECT_EQ(rejected->stats().opacityMicromapFallbackBlas, 1u);
  EXPECT_EQ(rejected->stats().blasBuilt, 1u);
  EXPECT_EQ(rejected->stats().blasReused, 1u);
  EXPECT_EQ(trace(*rejected, MicromapFixture::probes(), fixture.alpha)[0].z, 1u);
  EXPECT_EQ(trace(*mapped, MicromapFixture::probes(), fixture.alpha)[0].z, 0u);

  auto changed = std::make_shared<MicromapFixture::Attachment>(*fixture.attachment);
  changed->indices[0] = UINT32_MAX - 1;
  --changed->usage[0].count;
  fixture.geometry[0].opacityMicromap = std::move(changed);
  const auto reattached = build(mapped);
  EXPECT_EQ(reattached->stats().blasBuilt, 1u);
  EXPECT_EQ(reattached->stats().opacityMicromapBuilt, 0u);
  EXPECT_EQ(trace(*reattached, MicromapFixture::probes(), fixture.alpha)[0].x, 1u);
  std::printf("Opacity micromap fixture: %zu rays, sampled %.3f us, mapped %.3f us, map storage %llu bytes, allocation totals %llu -> %llu bytes\n",
      probes.size(), baselineNs / 1000., mappedNs / 1000.,
      static_cast<unsigned long long>(mapped->stats().opacityMicromapBytes),
      static_cast<unsigned long long>(total(baseline->stats())), static_cast<unsigned long long>(total(mapped->stats())));
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

TEST_F(RaySceneGpu, InterpolatedVertexAlphaMasksCandidatesAndOpaqueIgnoresAlpha) {
  if (!device->rayQueriesEnabled())
    GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
  std::vector<container::geometry::Vertex> vertices(3);
  vertices[0].position = {-1, -1, 0};
  vertices[1].position = {1, -1, 0};
  vertices[2].position = {-1, 1, 0};
  vertices[0].colorAlpha = 0;
  vertices[1].colorAlpha = 1;
  vertices[2].colorAlpha = .5f;
  const std::vector<uint32_t> indices{0, 1, 2};
  const std::vector<container::gpu::ObjectData> objects(1);
  const std::vector<DrawCommand> draws{{0, 0, 3}};
  const std::array providers{
      RaySceneProviderSource{{"rgba"}, 1, vertices, indices, objects, draws}};
  const auto extracted = extractRayScene(providers);
  const auto views = extracted.geometryViews();
  RaySceneAcceleration backend(*device, *memory);
  begin();
  const auto generation = backend.recordBuild(command, {views, extracted.instances});
  submit();
  const auto probe = [](float x, float y, uint32_t flags) {
    return Probe{{x, y, 2, .001f}, {0, 0, -1, 4},
        {0xff, std::bit_cast<uint32_t>(.8f), std::bit_cast<uint32_t>(.4f), flags + 1}};
  };
  // Barycentrics (1/4,1/4) -> alpha .375*.8=.3; (3/4,1/8)
  // -> .8125*.8=.65. OPAQUE must accept even the zero-alpha corner.
  const std::array probes{probe(-.5f, -.5f, 1), probe(.5f, -.75f, 1),
      probe(-.99f, -.99f, 0), probe(-.5f, -.5f, 2)};
  const auto results = trace(*generation, probes, extracted.triangles);
  const std::array<uint32_t, 4> expected{0, 1, 1, 0};
  ASSERT_EQ(results.size(), expected.size());
  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(results[i].z, 1u) << "candidate " << i;
    EXPECT_EQ(results[i].x, expected[i]) << "alpha probe " << i;
  }
}

TEST_F(RaySceneGpu, LocalMirroredGeometryFacingIsIndependentOfInstanceTransform) {
  if (!device->rayQueriesEnabled())
    GTEST_SKIP() << device->rayQuerySupport().unavailableReason();
  std::vector<container::geometry::Vertex> mirrored(3);
  mirrored[0].position = {1, -1, 0};
  mirrored[1].position = {-1, -1, 0};
  mirrored[2].position = {0, 1, 0};
  const std::vector<uint32_t> indices{0, 1, 2};
  const std::array geometries{
      RaySceneGeometry{{"locally-mirrored-skin"}, 1, 1, mirrored, indices, true}};
  std::vector<RaySceneInstance> instances;
  for (uint32_t i = 0; i < 4; ++i) {
    RaySceneInstance instance;
    instance.customIndex = i;
    instance.transform = glm::translate(glm::mat4(1), glm::vec3(i * 4, 0, 0));
    instance.flipFacing = i != 0;
    instance.doubleSided = i == 3;
    if (i == 2)
      instance.transform *= glm::scale(glm::mat4(1), glm::vec3(-1, 1, 1));
    instances.push_back(instance);
  }
  RaySceneAcceleration backend(*device, *memory);
  begin();
  auto generation = backend.recordBuild(command, {geometries, instances});
  submit();
  std::vector<Probe> probes;
  for (uint32_t instance = 0; instance < 4; ++instance)
    for (const bool back : {false, true})
      probes.push_back({{float(instance * 4), 0, back ? -2.f : 2.f, 0.001f},
                        {0, 0, back ? 1.f : -1.f, 4}, {0xff, 0, 0, 0}});
  const auto hits = trace(*generation, probes);
  // Uncorrected local mirror accepts the wrong side. Correcting it accepts
  // authored +Z, including under a model mirror; no-cull accepts both sides.
  const std::array<uint32_t, 8> expected{0, 1, 1, 0, 1, 0, 1, 1};
  ASSERT_EQ(hits.size(), expected.size());
  for (size_t i = 0; i < hits.size(); ++i)
    EXPECT_EQ(hits[i].x, expected[i]) << "probe " << i;
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
TEST_F(RaySceneGpu, GpuMatricesPublishImmutableTlasAndMatchingSurfaceTransforms) {
  ASSERT_TRUE(device->rayQueriesEnabled());
  PipelineManager pipelines(device->device());
  // Borrow the fixture's device; every publication is destroyed before TearDown.
  auto borrowed=std::shared_ptr<VulkanDevice>(device.get(),[](VulkanDevice*){});
  RayInstanceTransformPass producer(borrowed,*memory,pipelines);
  producer.createResources(CONTAINER_RAY_INSTANCE_SHADER_DIR);
  TestBuffer objects(*memory,sizeof(ObjectData)*2);
  TestBuffer firstSurfaces(*memory,sizeof(RaySurfaceInstanceData)*3);
  TestBuffer secondSurfaces(*memory,sizeof(RaySurfaceInstanceData)*3);
  std::array<ObjectData,2> gpuObjects{};
  gpuObjects[0].model=glm::translate(glm::mat4(1),glm::vec3(4,0,0));
  gpuObjects[1].model=glm::translate(glm::mat4(1),glm::vec3(-2,0,0))*
      glm::rotate(glm::mat4(1),.3f,glm::vec3(0,0,1))*glm::scale(glm::mat4(1),glm::vec3(-1,2,.5f));
  const auto upload=[&] {
    std::memcpy(objects.buffer.allocation_info.pMappedData,gpuObjects.data(),sizeof(gpuObjects));
    ASSERT_EQ(vmaFlushAllocation(memory->allocator(),objects.buffer.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  };
  upload();
  std::vector<container::geometry::Vertex> vertices(3);
  vertices[0].position={-1,-1,0};vertices[1].position={1,-1,0};vertices[2].position={0,1,0};
  const std::array<uint32_t,3> indices{0,1,2};
  const std::array<RaySceneGeometry,1> geometry{{{{"mesh"},1,1,vertices,indices,true}}};
  std::array<RaySceneInstance,3> instances{};
  for(uint32_t i=0;i<3;++i) {
    instances[i].geometryIndex=0;instances[i].customIndex=100+i;
    instances[i].doubleSided=true;
    // Deliberately different valid CPU mirrors prove the GPU stream is used.
    instances[i].transform=glm::translate(glm::mat4(1),glm::vec3(20+int(i)*4,0,0));
  }
  instances[0].gpuObjectIndex=1;
  instances[1].transform=glm::translate(glm::mat4(1),glm::vec3(-6,0,0));
  instances[2].gpuObjectIndex=0;instances[2].mask=2;instances[2].flipFacing=true;
  RaySceneGpuTransforms gpu{&producer,{objects.buffer.buffer,0,sizeof(gpuObjects)},2,
      {firstSurfaces.buffer.buffer,0,sizeof(RaySurfaceInstanceData)*3}};
  RaySceneAcceleration backend(*device,*memory);
  const auto build=[&](const std::shared_ptr<const RaySceneGeneration>& previous={}) {
    begin();auto generation=backend.recordBuild(command,{geometry,instances,&gpu},previous);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(*command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    submit();return generation;
  };
  const auto first=build();
  EXPECT_EQ(first->stats().gpuInstanceCount,2);
  EXPECT_EQ(first->stats().gpuTransformUploadBytes,3*24+48);
  const auto probe=[](float x,uint32_t mask=0xff) {return Probe{{x,0,2,.001f},{0,0,-1,4},{mask,0,0,0}};};
  const std::array probes{probe(-2),probe(-6),probe(4,1),probe(4,2),probe(20),probe(8,2)};
  const auto originalHits=trace(*first,probes);
  EXPECT_EQ(originalHits[0].x,1);EXPECT_EQ(originalHits[0].y,100);
  EXPECT_EQ(originalHits[1].x,1);EXPECT_EQ(originalHits[1].y,101);
  EXPECT_EQ(originalHits[2].x,0);EXPECT_EQ(originalHits[3].x,1);EXPECT_EQ(originalHits[3].y,102);
  EXPECT_EQ(originalHits[4].x,0);EXPECT_EQ(originalHits[5].x,0);
  ASSERT_EQ(vmaInvalidateAllocation(memory->allocator(),firstSurfaces.buffer.allocation,0,VK_WHOLE_SIZE),VK_SUCCESS);
  const auto* surface=static_cast<const RaySurfaceInstanceData*>(firstSurfaces.buffer.allocation_info.pMappedData);
  const std::array expected{gpuObjects[1].model,instances[1].transform,gpuObjects[0].model};
  for(size_t i=0;i<expected.size();++i) {
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) EXPECT_NEAR(surface[i].model[c][r],expected[i][c][r],2e-5f);
    const auto normal=glm::transpose(glm::inverse(glm::mat3(expected[i])));
    for(int c=0;c<3;++c) for(int r=0;r<3;++r) EXPECT_NEAR(surface[i].normalColumns[c][r],normal[c][r],2e-5f);
    EXPECT_EQ(surface[i].normalColumns[0].w,glm::determinant(glm::mat3(expected[i]))<0?-1.f:1.f);
  }
  gpuObjects[0].model[3].x=8;upload();
  gpu.surfaces.buffer=secondSurfaces.buffer.buffer;
  const auto second=build(first);
  EXPECT_EQ(second->stats().blasBuilt,0);EXPECT_EQ(second->stats().blasReused,1);
  const auto moved=trace(*second,probes);
  EXPECT_EQ(moved[3].x,0);EXPECT_EQ(moved[5].x,1);EXPECT_EQ(moved[5].y,102);
  const auto retained=trace(*first,probes);
  EXPECT_EQ(retained[3].x,1);EXPECT_EQ(retained[5].x,0);
  instances[0].gpuObjectIndex=2;
  EXPECT_THROW((void)backend.recordBuild(command,{geometry,instances,&gpu},first),std::invalid_argument);
  instances[0].gpuObjectIndex=1;gpu.surfaces.size=16;
  EXPECT_THROW((void)backend.recordBuild(command,{geometry,instances,&gpu},first),std::invalid_argument);
}
} // namespace
