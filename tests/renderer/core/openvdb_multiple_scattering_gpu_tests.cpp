#include "Container/utility/FileLoader.h"
#include "Container/utility/VulkanInstance.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::gpu;

struct alignas(16) Medium {
  std::array<float, 4> temperatureControls{1, 1, 1, 0};
  std::array<uint32_t, 4> flags{1, 2, 0, 0};
};
struct alignas(16) Query {
  std::array<float, 4> albedoTransmittance{};
  std::array<uint32_t, 4> controls{};
};
struct alignas(16) Result {
  std::array<float, 4> gainEnabled{};
  std::array<float, 4> source{};
};
static_assert(sizeof(Medium) == 32 && sizeof(Query) == 32 && sizeof(Result) == 32);

struct HostBuffer {
  VulkanMemoryManager& memory;
  AllocatedBuffer value;
  HostBuffer(VulkanMemoryManager& owner, size_t size)
      : memory(owner), value(owner.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT)) {}
  ~HostBuffer() { memory.destroyBuffer(value); }
  void upload(const void* data, size_t size) {
    std::memcpy(value.allocation_info.pMappedData, data, size);
    if (vmaFlushAllocation(memory.allocator(), value.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Failed to flush scattering test inputs");
  }
};

// No surface, swapchain, render targets or renderer fixture is needed for the
// production numerical helper. Validation includes the shader-to-host barrier.
class OpenVdbMultipleScatteringGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<OpenVdbMultipleScatteringGpu*>(user)->validationErrors_;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    InstanceCreateInfo info;
    info.enableValidationLayers = true;
    info.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    info.requiredExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    info.next = &validation;
    instance_ = std::make_unique<VulkanInstance>(info);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger_ = vk::raii::DebugUtilsMessengerEXT(instance_->raii(), debug);
    for (auto& candidate : instance_->raii().enumeratePhysicalDevices()) {
      if (candidate.getProperties().apiVersion < VK_API_VERSION_1_4) continue;
      const auto families = candidate.getQueueFamilyProperties();
      for (uint32_t index = 0; index < families.size(); ++index) {
        if (families[index].queueCount && (families[index].queueFlags & vk::QueueFlagBits::eCompute)) {
          family_ = index;
          physical_ = std::move(candidate);
          break;
        }
      }
      if (*physical_) break;
    }
    ASSERT_TRUE(*physical_) << "A Vulkan 1.4 compute device is required";
    const float priority = 1;
    vk::DeviceQueueCreateInfo queue({}, family_, 1, &priority);
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE; // Production VMA uses address-capable allocations.
    vk::DeviceCreateInfo create({}, queue);
    create.pNext = &v12;
    device_ = vk::raii::Device(physical_, create);
    queue_ = device_.getQueue(family_, 0);
    memory_ = std::make_unique<VulkanMemoryManager>(instance_->instance(), *physical_, *device_);
    pool_ = vk::raii::CommandPool(device_, vk::CommandPoolCreateInfo(
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family_));
    auto commands = device_.allocateCommandBuffers(vk::CommandBufferAllocateInfo(
        *pool_, vk::CommandBufferLevel::ePrimary, 1));
    command_ = std::move(commands.front());
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute)};
    layout_ = vk::raii::DescriptorSetLayout(device_, vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const std::array layouts{*layout_};
    pipelineLayout_ = vk::raii::PipelineLayout(device_, vk::PipelineLayoutCreateInfo({}, layouts));
    const auto bytes = container::util::readFile(CONTAINER_OPENVDB_SCATTERING_SPIRV);
    ASSERT_FALSE(bytes.empty());
    ASSERT_EQ(bytes.size() % sizeof(uint32_t), 0u);
    std::vector<uint32_t> code(bytes.size() / sizeof(uint32_t));
    std::memcpy(code.data(), bytes.data(), bytes.size());
    vk::raii::ShaderModule shader(device_, vk::ShaderModuleCreateInfo({}, bytes.size(), code.data()));
    vk::PipelineShaderStageCreateInfo stage({}, vk::ShaderStageFlagBits::eCompute, *shader, "computeMain");
    pipeline_ = vk::raii::Pipeline(device_, nullptr, vk::ComputePipelineCreateInfo({}, stage, *pipelineLayout_));
  }
  void TearDown() override {
    if (*device_) device_.waitIdle();
    pipeline_.clear();
    pipelineLayout_.clear();
    layout_.clear();
    command_.clear();
    pool_.clear();
    memory_.reset();
    queue_.clear();
    device_.clear();
    physical_.clear();
    messenger_.clear();
    instance_.reset();
    EXPECT_EQ(validationErrors_.load(), 0u);
  }
  std::vector<Result> run(const std::array<Medium, 4>& media, std::span<const Query> queries) {
    HostBuffer control(*memory_, sizeof(media));
    HostBuffer input(*memory_, queries.size_bytes());
    HostBuffer output(*memory_, queries.size() * sizeof(Result));
    control.upload(media.data(), sizeof(media));
    input.upload(queries.data(), queries.size_bytes());
    const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3)};
    vk::raii::DescriptorPool descriptors(device_, vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    const std::array layouts{*layout_};
    auto sets = device_.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors, layouts));
    const std::array buffers{
        vk::DescriptorBufferInfo(control.value.buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(input.value.buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(output.value.buffer, 0, VK_WHOLE_SIZE)};
    std::array<vk::WriteDescriptorSet, 3> writes;
    for (uint32_t index = 0; index < writes.size(); ++index) {
      writes[index].dstSet = *sets.front();
      writes[index].dstBinding = index;
      writes[index].descriptorCount = 1;
      writes[index].descriptorType = vk::DescriptorType::eStorageBuffer;
      writes[index].pBufferInfo = &buffers[index];
    }
    device_.updateDescriptorSets(writes, {});
    command_.reset();
    command_.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    command_.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline_);
    const std::array bound{*sets.front()};
    command_.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout_, 0, bound, {});
    command_.dispatch(static_cast<uint32_t>((queries.size() + 63) / 64), 1, 1);
    const vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
    command_.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                             {}, host, {}, {});
    command_.end();
    vk::raii::Fence fence(device_, vk::FenceCreateInfo{});
    const std::array commands{*command_};
    vk::SubmitInfo submit({}, {}, commands);
    queue_.submit(submit, *fence);
    if (device_.waitForFences(*fence, true, 10'000'000'000ull) != vk::Result::eSuccess)
      throw std::runtime_error("Timed out reading production multiple-scattering helper");
    if (vmaInvalidateAllocation(memory_->allocator(), output.value.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Failed to invalidate scattering test results");
    std::vector<Result> result(queries.size());
    std::memcpy(result.data(), output.value.allocation_info.pMappedData, result.size() * sizeof(Result));
    return result;
  }
  static std::vector<Query> grid() {
    const std::array albedos{std::array{0.0f, 0.0f, 0.0f}, std::array{1.0f, 1.0f, 1.0f},
                             std::array{0.0f, 0.5f, 1.0f}, std::array{0.1f, 0.95f, 0.25f}};
    const std::array transmittances{0.0f, std::numeric_limits<float>::min(), 1e-7f, 0.001f,
        0.01f, 0.25f, 0.5f, 0.9f, std::nextafter(1.0f, 0.0f), 1.0f};
    std::vector<Query> queries;
    for (const auto& albedo : albedos)
      for (const float transmittance : transmittances)
        for (uint32_t index = 0; index < 4; ++index)
          queries.push_back({{albedo[0], albedo[1], albedo[2], transmittance}, {index, 1, 0, 0}});
    return queries;
  }
  std::atomic_uint validationErrors_{};
  std::unique_ptr<VulkanInstance> instance_;
  vk::raii::DebugUtilsMessengerEXT messenger_{nullptr};
  vk::raii::PhysicalDevice physical_{nullptr};
  vk::raii::Device device_{nullptr};
  vk::raii::Queue queue_{nullptr};
  uint32_t family_{};
  std::unique_ptr<VulkanMemoryManager> memory_;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer command_{nullptr};
  vk::raii::DescriptorSetLayout layout_{nullptr};
  vk::raii::PipelineLayout pipelineLayout_{nullptr};
  vk::raii::Pipeline pipeline_{nullptr};
};

TEST_F(OpenVdbMultipleScatteringGpu, ActualGainIsFiniteAndConservativeAtPhysicalEndpoints) {
  std::array<Medium, 4> media;
  for (uint32_t index = 0; index < media.size(); ++index) media[index].flags[1] = index + 1;
  const auto queries = grid();
  const auto result = run(media, queries);
  ASSERT_EQ(result.size(), 160u);
  for (size_t index = 0; index < queries.size(); ++index) {
    SCOPED_TRACE(index);
    const float transmittance = queries[index].albedoTransmittance[3];
    for (size_t channel = 0; channel < 3; ++channel) {
      const float gain = result[index].gainEnabled[channel];
      EXPECT_TRUE(std::isfinite(gain));
      EXPECT_GE(gain, 0);
      EXPECT_LE(gain, 4);
      EXPECT_LE(transmittance * (1.0f + gain), 1.0f + 2e-6f);
      if (transmittance == 1 || queries[index].albedoTransmittance[channel] == 0)
        EXPECT_FLOAT_EQ(gain, 0);
      else EXPECT_GT(gain, 0);
    }
  }
}

TEST_F(OpenVdbMultipleScatteringGpu, MoreOrdersAndStrengthNeverReduceTheReturnedGain) {
  std::array<Medium, 4> media;
  for (uint32_t index = 0; index < media.size(); ++index) media[index].flags[1] = index + 1;
  const auto queries = grid();
  const auto orders = run(media, queries);
  for (size_t base = 0; base < orders.size(); base += 4)
    for (size_t index = 1; index < 4; ++index)
      for (size_t channel = 0; channel < 3; ++channel)
        EXPECT_GE(orders[base + index].gainEnabled[channel], orders[base + index - 1].gainEnabled[channel]);
  const std::array strengths{0.0f, 0.25f, 0.5f, 1.0f};
  for (uint32_t index = 0; index < media.size(); ++index) {
    media[index].flags[1] = 4;
    media[index].temperatureControls[2] = strengths[index];
  }
  const auto strength = run(media, queries);
  for (size_t base = 0; base < strength.size(); base += 4) {
    for (size_t channel = 0; channel < 3; ++channel) {
      EXPECT_FLOAT_EQ(strength[base].gainEnabled[channel], 0);
      for (size_t index = 1; index < 4; ++index)
        EXPECT_GE(strength[base + index].gainEnabled[channel], strength[base + index - 1].gainEnabled[channel]);
    }
  }
}

TEST_F(OpenVdbMultipleScatteringGpu, PerMediumStrengthAndShadowReceptionGateTheActualSource) {
  std::array<Medium, 4> media;
  media[0].flags[0] |= 16;
  media[2].temperatureControls[2] = 0;
  media[3].temperatureControls[2] = 0.5f;
  std::vector<Query> queries;
  for (uint32_t medium = 0; medium < 4; ++medium)
    for (uint32_t visible = 0; visible < 2; ++visible)
      queries.push_back({{0.25f, 0.5f, 1, 0.4f}, {medium, visible, 0, 0}});
  const auto result = run(media, queries);
  for (size_t index = 0; index < result.size(); ++index) {
    const bool shadowed = queries[index].controls[0] == 0 && queries[index].controls[1] == 0;
    for (size_t channel = 0; channel < 3; ++channel) {
      EXPECT_TRUE(std::isfinite(result[index].source[channel]));
      const float expected = shadowed ? 0 : float(channel + 1) * result[index].gainEnabled[channel];
      EXPECT_NEAR(result[index].source[channel], expected, 2e-6f);
    }
  }
  for (auto& medium : media) medium.temperatureControls[2] = 0;
  for (const auto& value : run(media, queries)) {
    EXPECT_FLOAT_EQ(value.gainEnabled[3], 0);
    for (size_t channel = 0; channel < 3; ++channel) {
      EXPECT_FLOAT_EQ(value.gainEnabled[channel], 0);
      EXPECT_FLOAT_EQ(value.source[channel], 0);
    }
  }
}
} // namespace

int main(int argc, char** argv) {
  const char* enabled = std::getenv("CONTAINER_RUN_OPENVDB_SCATTERING_GPU");
  if (!enabled || std::strcmp(enabled, "1") != 0) {
    std::puts("SKIP: set CONTAINER_RUN_OPENVDB_SCATTERING_GPU=1 for production helper GPU tests");
    return 77;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
