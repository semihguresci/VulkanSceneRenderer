#include "Container/common/CommonGLFW.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::gpu;
struct alignas(16) Query {
  GpuMaterial material{};
  glm::vec4 transmissionAlphaMetallicRoughness{1, 1, 0, .05f};
  glm::vec4 tintIncidence{1, 1, 1, 1};
  glm::uvec4 controls{1, 1, 0, 0};
  glm::vec4 throughput{1, 1, 1, 0};
  glm::vec4 directionDistance{0, 0, -1, 0};
  glm::vec4 normalEntering{0, 0, 1, 1};
  glm::vec4 randomReserved{.25f, .5f, 0, 0};
  Query() { material.transmissionFactor = 1; }
};
static_assert(offsetof(Query, transmissionAlphaMetallicRoughness) == sizeof(GpuMaterial));
static_assert(sizeof(Query) == sizeof(GpuMaterial) + 112);
struct alignas(16) Result {
  glm::vec4 transmittanceSupported{};
  glm::vec4 throughputContinued{};
  glm::vec4 dielectricDirectionTir{};
  glm::vec4 absorptionSupported{};
};

class DdgiTransportGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<DdgiTransportGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto *enabled = std::getenv("CONTAINER_RUN_GPU_DDGI");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_DDGI=1 for DDGI transport contracts";
    std::ifstream shader(CONTAINER_DDGI_TRANSPORT_SPIRV,
                         std::ios::binary | std::ios::ate);
    ASSERT_TRUE(shader);
    const auto bytes = static_cast<size_t>(shader.tellg());
    ASSERT_GT(bytes, 20u);
    ASSERT_EQ(bytes % 4, 0u);
    std::vector<uint32_t> code(bytes / 4);
    shader.seekg(0);
    shader.read(reinterpret_cast<char *>(code.data()), bytes);
    ASSERT_TRUE(shader);
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "DDGI thin glass contract", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t extensionCount{};
    const auto *extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions.assign(extensions, extensions + extensionCount);
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
    VkSurfaceKHR surfaceHandle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &surfaceHandle),
              VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), surfaceHandle);
    DeviceCreateInfo create;
    create.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    create.next = &v13;
    device = std::make_unique<VulkanDevice>(*instance, surfaceHandle, create);
    memory = std::make_unique<VulkanMemoryManager>(instance->instance(),
        device->physicalDevice(), device->device());
    commandPool = vk::raii::CommandPool(device->raii(),
        vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            device->queueFamilyIndices().graphicsFamily.value()));
    auto commands = device->raii().allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(*commandPool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1,
                                       vk::ShaderStageFlagBits::eCompute)};
    layout = vk::raii::DescriptorSetLayout(device->raii(),
        vk::DescriptorSetLayoutCreateInfo({}, bindings));
    pipelineLayout = vk::raii::PipelineLayout(device->raii(),
        vk::PipelineLayoutCreateInfo({}, *layout));
    vk::raii::ShaderModule module(device->raii(), vk::ShaderModuleCreateInfo({}, code));
    pipeline = vk::raii::Pipeline(device->raii(), nullptr,
        vk::ComputePipelineCreateInfo({},
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute,
                                               *module, "computeMain"), *pipelineLayout));
  }
  void TearDown() override {
    if (device) device->raii().waitIdle();
    command.clear();
    pipeline.clear(); pipelineLayout.clear(); layout.clear(); commandPool.clear();
    memory.reset(); device.reset(); surface.clear(); messenger.clear(); instance.reset();
    if (window) glfwDestroyWindow(window);
    if (glfwStarted) glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }
  struct Buffer {
    VulkanMemoryManager &memory;
    AllocatedBuffer buffer;
    Buffer(VulkanMemoryManager &m, size_t bytes) : memory(m),
      buffer(m.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_HOST, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT)) {}
    ~Buffer() { memory.destroyBuffer(buffer); }
  };
  std::vector<Result> run(std::span<const Query> input) {
    if (input.empty()) throw std::invalid_argument("DDGI transport empty queries");
    Buffer queries(*memory, input.size_bytes()), output(*memory, input.size() * sizeof(Result));
    std::memcpy(queries.buffer.allocation_info.pMappedData, input.data(), input.size_bytes());
    if (vmaFlushAllocation(memory->allocator(), queries.buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI transport flush failed");
    const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer, 2);
    vk::raii::DescriptorPool descriptors(device->raii(), vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, size));
    auto sets = device->raii().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo(*descriptors, *layout));
    const std::array buffers{
      vk::DescriptorBufferInfo(queries.buffer.buffer, 0, input.size_bytes()),
      vk::DescriptorBufferInfo(output.buffer.buffer, 0, input.size() * sizeof(Result))};
    for (uint32_t i = 0; i < 2; ++i)
      device->raii().updateDescriptorSets(vk::WriteDescriptorSet(*sets.front(), i, 0, 1,
          vk::DescriptorType::eStorageBuffer, nullptr, &buffers[i]), {});
    command.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    vk::MemoryBarrier2 before(vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
        vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead |
                                                     vk::AccessFlagBits2::eShaderWrite);
    vk::DependencyInfo dependency;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &before;
    command.pipelineBarrier2(dependency);
    command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, *sets.front(), {});
    command.dispatch(static_cast<uint32_t>(input.size()), 1, 1);
    vk::MemoryBarrier2 after(vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderWrite, vk::PipelineStageFlagBits2::eHost,
        vk::AccessFlagBits2::eHostRead);
    dependency.pMemoryBarriers = &after;
    command.pipelineBarrier2(dependency);
    command.end();
    const vk::CommandBuffer handle = *command;
    vk::SubmitInfo submit;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &handle;
    vk::raii::Fence fence(device->raii(), vk::FenceCreateInfo());
    vk::raii::Queue queue(device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submit, *fence);
    if (device->raii().waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
      throw std::runtime_error("DDGI transport fence failed");
    if (vmaInvalidateAllocation(memory->allocator(), output.buffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI transport invalidate failed");
    std::vector<Result> result(input.size());
    std::memcpy(result.data(), output.buffer.allocation_info.pMappedData, result.size() * sizeof(Result));
    // Drop command references before descriptor/buffer destruction.
    command.reset();
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
  vk::raii::CommandPool commandPool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
  vk::raii::DescriptorSetLayout layout{nullptr};
  vk::raii::PipelineLayout pipelineLayout{nullptr};
  vk::raii::Pipeline pipeline{nullptr};
};

TEST_F(DdgiTransportGpu, OpaqueAlphaOneExplicitTransmissionPassesTintedFresnelEnergy) {
  std::array<Query, 2> input;
  input[0].tintIncidence = {.25f, .5f, 1, 1};
  input[1].tintIncidence = {.25f, .5f, 1, -1};
  const auto result = run(input);
  for (const auto &value : result) {
    EXPECT_FLOAT_EQ(value.transmittanceSupported.w, 1);
    EXPECT_NEAR(value.transmittanceSupported.x, .24f, 1e-6f);
    EXPECT_NEAR(value.transmittanceSupported.y, .48f, 1e-6f);
    EXPECT_NEAR(value.transmittanceSupported.z, .96f, 1e-6f);
    EXPECT_FLOAT_EQ(value.throughputContinued.w, 1);
  }
}

TEST_F(DdgiTransportGpu, DisabledOpaqueBlendedThickRoughMetalAndLayeredRemainUnsupported) {
  std::array<Query, 12> input;
  input[0].controls.x = 0;
  input[1].transmissionAlphaMetallicRoughness.x = 0;
  input[2].material.flags = 2; input[2].transmissionAlphaMetallicRoughness.y = .5f;
  input[3].material.thicknessFactor = .1f;
  input[4].material.thicknessTextureIndex = 0;
  input[5].transmissionAlphaMetallicRoughness.w = .2f;
  input[6].transmissionAlphaMetallicRoughness.z = .1f;
  input[7].material.normalTextureIndex = 0;
  input[8].material.heightTextureIndex = 0; input[8].material.heightScale = .1f;
  input[9].material.clearcoatFactor = 1;
  input[10].material.flags = 0x10;
  input[11].material.dispersion = .1f;
  for (const auto &value : run(input)) {
    EXPECT_EQ(value.transmittanceSupported, glm::vec4(0));
    EXPECT_FLOAT_EQ(value.throughputContinued.w, 0);
  }
}

TEST_F(DdgiTransportGpu, EightCrossingsPassButNinthAndNegligibleEnergyStop) {
  std::array<Query, 3> input;
  input[0].controls.y = 8;
  input[1].controls.y = 9;
  input[2].throughput = {1e-6f, 1e-6f, 1e-6f, 0};
  const auto result = run(input);
  EXPECT_FLOAT_EQ(result[0].throughputContinued.w, 1);
  EXPECT_NEAR(result[0].throughputContinued.x, std::pow(.96f, 8), 2e-6f);
  EXPECT_FLOAT_EQ(result[1].throughputContinued.w, 0);
  EXPECT_FLOAT_EQ(result[2].throughputContinued.w, 0);
}

TEST_F(DdgiTransportGpu, GrazingFresnelClosesTransmissionWithoutNan) {
  std::array<Query, 2> input;
  input[0].tintIncidence.w = 0;
  input[1].material.ior = 1;
  input[1].tintIncidence.w = 0;
  const auto result = run(input);
  EXPECT_EQ(result[0].transmittanceSupported, glm::vec4(0, 0, 0, 1));
  EXPECT_FLOAT_EQ(result[0].throughputContinued.w, 0);
  EXPECT_EQ(result[1].transmittanceSupported, glm::vec4(1));
}

TEST_F(DdgiTransportGpu, NonfiniteMaterialAndThroughputNeverContinue) {
  std::array<Query, 4> input;
  input[0].material.ior = std::numeric_limits<float>::quiet_NaN();
  input[1].tintIncidence.x = std::numeric_limits<float>::infinity();
  input[2].transmissionAlphaMetallicRoughness.x = std::numeric_limits<float>::quiet_NaN();
  input[3].throughput.z = std::numeric_limits<float>::infinity();
  const auto result = run(input);
  for (const auto &value : result) EXPECT_FLOAT_EQ(value.throughputContinued.w, 0);
}

Query dielectricQuery() {
  Query query;
  query.controls.w = 1;
  query.transmissionAlphaMetallicRoughness.w = 0;
  query.material.thicknessFactor = .2f;
  return query;
}

TEST_F(DdgiTransportGpu, SnellEntryAndExitRecoverOriginalDirectionAndRadianceScaling) {
  std::array<Query, 2> input{dielectricQuery(), dielectricQuery()};
  const auto incidence = glm::vec3(std::sqrt(.75f), 0, -.5f);
  const auto refracted = glm::vec3(std::sqrt(1.f / 3.f), 0, -std::sqrt(2.f / 3.f));
  input[0].directionDistance = {incidence, 0};
  input[1].directionDistance = {refracted, 0};
  input[1].normalEntering = {0, 0, -1, 0};
  const auto result = run(input);
  for (size_t axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(result[0].dielectricDirectionTir[axis], refracted[axis], 2e-6f);
    EXPECT_NEAR(result[1].dielectricDirectionTir[axis], incidence[axis], 2e-6f);
  }
  for (const auto &value : result) {
    EXPECT_FLOAT_EQ(value.transmittanceSupported.w, 1);
    EXPECT_FLOAT_EQ(value.dielectricDirectionTir.w, 0);
  }
  // Fresnel at 60 degrees is approximately .08919; entry/exit eta^2
  // radiance conversion cancels across a closed clear slab.
  EXPECT_NEAR(result[0].transmittanceSupported.x * result[1].transmittanceSupported.x,
              std::pow(1.f - .0891867f, 2), 3e-6f);
}

TEST_F(DdgiTransportGpu, InternalCriticalAngleReflectsInsteadOfLeakingIntoAir) {
  std::array<Query, 3> input{dielectricQuery(), dielectricQuery(), dielectricQuery()};
  input[0].normalEntering = {0, 0, -1, 0};
  input[0].directionDistance = {std::sqrt(.75f), 0, -.5f, 0};
  input[1].directionDistance = {0, 0, -1, 0};
  input[1].material.ior = 1;
  input[1].transmissionAlphaMetallicRoughness.w = .8f;
  input[2].directionDistance = {1, 0, 0, 0};
  input[2].material.ior = 1;
  input[2].transmissionAlphaMetallicRoughness.w = .8f;
  const auto result = run(input);
  EXPECT_FLOAT_EQ(result[0].transmittanceSupported.w, 1);
  EXPECT_NEAR(result[0].transmittanceSupported.x, 1, 1e-6f);
  EXPECT_NEAR(result[0].dielectricDirectionTir.x, std::sqrt(.75f), 1e-6f);
  EXPECT_NEAR(result[0].dielectricDirectionTir.z, .5f, 1e-6f);
  EXPECT_FLOAT_EQ(result[0].dielectricDirectionTir.w, 1);
  EXPECT_EQ(result[1].dielectricDirectionTir, glm::vec4(0, 0, -1, 0));
  EXPECT_EQ(result[1].transmittanceSupported, glm::vec4(1));
  EXPECT_EQ(result[2].dielectricDirectionTir, glm::vec4(1, 0, 0, 0));
  EXPECT_EQ(result[2].transmittanceSupported, glm::vec4(1));
}

TEST_F(DdgiTransportGpu, VolumeAbsorptionUsesTracedWorldDistanceAndExactBlackChannels) {
  std::array<Query, 5> input{dielectricQuery(), dielectricQuery(),
                           dielectricQuery(), dielectricQuery(), dielectricQuery()};
  for (auto &query : input) {
    query.material.attenuationColor = {.25f, .5f, 1, 0};
    query.material.attenuationDistance = 1;
  }
  input[0].directionDistance.w = 2;
  input[0].material.thicknessTextureIndex = 0; // intentionally ignored for ray tracing
  input[1].directionDistance.w = .5f;
  input[2].material.attenuationColor.x = 0;
  input[2].directionDistance.w = .001f;
  input[3].directionDistance.w = 10;
  input[3].material.attenuationDistance = std::numeric_limits<float>::infinity();
  input[4].directionDistance.w = 100;
  input[4].material.attenuationDistance = std::numeric_limits<float>::min();
  const auto result = run(input);
  EXPECT_NEAR(result[0].absorptionSupported.x, .0625f, 1e-6f);
  EXPECT_NEAR(result[0].absorptionSupported.y, .25f, 1e-6f);
  EXPECT_FLOAT_EQ(result[0].absorptionSupported.z, 1);
  EXPECT_FLOAT_EQ(result[0].absorptionSupported.w, 1);
  EXPECT_NEAR(result[1].absorptionSupported.x, .5f, 1e-6f);
  EXPECT_NEAR(result[1].absorptionSupported.y, std::sqrt(.5f), 1e-6f);
  EXPECT_FLOAT_EQ(result[2].absorptionSupported.x, 0);
  EXPECT_EQ(result[3].absorptionSupported, glm::vec4(1));
  EXPECT_EQ(result[4].absorptionSupported, glm::vec4(0, 0, 1, 1));
  for (const auto &value : result) EXPECT_FLOAT_EQ(value.transmittanceSupported.w, 1);
}

TEST_F(DdgiTransportGpu, RoughThinAndVolumeInterfacesSampleFiniteDifferentDirections) {
  std::array<Query, 128> input;
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = dielectricQuery();
    input[i].material.thicknessFactor = i < 64 ? 0 : .2f;
    input[i].transmissionAlphaMetallicRoughness.w = .65f;
    input[i].randomReserved = {(float(i % 64) + .5f) / 64.f,
                               std::fmod(float(i) * .61803398875f, 1.f), 0, 0};
  }
  const auto result = run(input);
  std::array<unsigned, 2> supported{};
  std::array<float, 2> lateralEnergy{};
  for (size_t i = 0; i < result.size(); ++i) {
    const auto &value = result[i];
    for (int axis = 0; axis < 4; ++axis) {
      EXPECT_TRUE(std::isfinite(value.dielectricDirectionTir[axis]));
      EXPECT_TRUE(std::isfinite(value.transmittanceSupported[axis]));
    }
    if (value.transmittanceSupported.w == 0) continue;
    ++supported[i / 64];
    EXPECT_LT(value.dielectricDirectionTir.z, 0);
    EXPECT_NEAR(glm::length(glm::vec3(value.dielectricDirectionTir)), 1, 2e-6f);
    lateralEnergy[i / 64] += value.dielectricDirectionTir.x * value.dielectricDirectionTir.x +
                             value.dielectricDirectionTir.y * value.dielectricDirectionTir.y;
  }
  for (size_t group = 0; group < 2; ++group) {
    EXPECT_GT(supported[group], 32u);
    EXPECT_GT(lateralEnergy[group], .1f);
  }
}

TEST_F(DdgiTransportGpu, InvalidDielectricAndAbsorptionInputsRemainBlocking) {
  std::array<Query, 10> input;
  for (auto &query : input) query = dielectricQuery();
  input[0].controls.x = 0;
  input[1].material.thicknessFactor = -.1f;
  input[2].material.attenuationDistance = 0;
  input[3].material.attenuationDistance = std::numeric_limits<float>::quiet_NaN();
  input[4].material.attenuationColor.x = -1;
  input[5].material.attenuationColor.x = std::numeric_limits<float>::infinity();
  input[6].randomReserved.x = 1;
  input[7].directionDistance.z = -2;
  input[8].transmissionAlphaMetallicRoughness.w = 1.1f;
  input[9].material.dispersion = .1f;
  for (const auto &value : run(input)) EXPECT_FLOAT_EQ(value.transmittanceSupported.w, 0);
}
} // namespace
