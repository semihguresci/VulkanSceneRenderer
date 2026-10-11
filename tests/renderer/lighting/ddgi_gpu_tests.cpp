#include "Container/common/CommonGLFW.h"
#include "Container/renderer/lighting/DdgiProbeVolume.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/utility/VulkanMemoryManager.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <glm/packing.hpp>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::renderer;
using namespace container::gpu;
constexpr uint32_t raysPerProbe = 4096;
constexpr uint32_t interior = 2, tile = interior + 2;

// Mirror the production UBO, while testing the actual production update
// kernel rather than a second implementation of its integration algorithm.
using GpuData = DdgiGpuData;
static_assert(sizeof(GpuData) == 400);

struct HostBuffer {
  VulkanMemoryManager &memory;
  AllocatedBuffer value;
  size_t size;
  HostBuffer(VulkanMemoryManager &m, size_t bytes, VkBufferUsageFlags usage)
      : memory(m),
        value(m.createBuffer(std::max<size_t>(bytes, 16), usage,
                             VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT)),
        size(std::max<size_t>(bytes, 16)) {}
  ~HostBuffer() { memory.destroyBuffer(value); }
  void upload(const void *source, size_t bytes) {
    if (bytes > size || !value.allocation_info.pMappedData)
      throw std::runtime_error("invalid DDGI test upload");
    if (bytes)
      std::memcpy(value.allocation_info.pMappedData, source, bytes);
    if (vmaFlushAllocation(memory.allocator(), value.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI test upload flush failed");
  }
  void invalidate() {
    if (vmaInvalidateAllocation(memory.allocator(), value.allocation, 0,
                                VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI test readback invalidate failed");
  }
};

struct TestImage {
  VulkanMemoryManager &memory;
  VkImage image{};
  VmaAllocation allocation{};
  vk::raii::ImageView view{nullptr};
  TestImage(const vk::raii::Device &device, VulkanMemoryManager &m,
            uint32_t width, uint32_t height, VkFormat format)
      : memory(m) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(m.allocator(), &info, &alloc, &image, &allocation,
                       nullptr) != VK_SUCCESS)
      throw std::runtime_error("DDGI test image allocation failed");
    try {
      vk::ImageViewCreateInfo create;
      create.image = image;
      create.viewType = vk::ImageViewType::e2D;
      create.format = static_cast<vk::Format>(format);
      create.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
      view = vk::raii::ImageView(device, create);
    } catch (...) {
      vmaDestroyImage(m.allocator(), image, allocation);
      throw;
    }
  }
  ~TestImage() {
    view.clear();
    vmaDestroyImage(memory.allocator(), image, allocation);
  }
};

class DdgiGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<DdgiGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto *enabled = std::getenv("CONTAINER_RUN_GPU_DDGI");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_DDGI=1 for Vulkan DDGI contracts";
    std::ifstream shader(CONTAINER_DDGI_UPDATE_SPIRV,
                         std::ios::binary | std::ios::ate);
    ASSERT_TRUE(shader) << "Production DDGI update SPIR-V is missing";
    const auto bytes = static_cast<size_t>(shader.tellg());
    ASSERT_GT(bytes, 20u);
    ASSERT_EQ(bytes % 4, 0u);
    code.resize(bytes / 4);
    shader.seekg(0);
    shader.read(reinterpret_cast<char *>(code.data()), bytes);
    ASSERT_TRUE(shader);
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(64, 64, "DDGI update contract", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count = 0;
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
    deviceInfo.optionalFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
    deviceInfo.optionalFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
    deviceInfo.optionalFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    deviceInfo.next = &v13;
    device = std::make_unique<VulkanDevice>(*instance, handle, deviceInfo);
    // OpCapability (17): optional typed/untyped image capabilities depend on
    // Slang's emitted image format; test only the features this module needs.
    const auto &features = device->enabledFeatures();
    for (size_t offset = 5; offset < code.size();) {
      const auto words = code[offset] >> 16;
      ASSERT_GT(words, 0u);
      ASSERT_LE(words, code.size() - offset);
      if ((code[offset] & 0xffffu) == 17u && words == 2u) {
        const auto capability = code[offset + 1];
        if ((capability == 49u &&
             !features.shaderStorageImageExtendedFormats) ||
            (capability == 55u &&
             !features.shaderStorageImageReadWithoutFormat) ||
            (capability == 56u &&
             !features.shaderStorageImageWriteWithoutFormat))
          GTEST_SKIP() << "Device lacks DDGI image capability " << capability;
      }
      offset += words;
    }
    for (auto format :
         {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32_SFLOAT}) {
      VkFormatProperties properties{};
      vkGetPhysicalDeviceFormatProperties(device->physicalDevice(), format,
                                          &properties);
      constexpr auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      if ((properties.optimalTilingFeatures & required) != required)
        GTEST_SKIP() << "Device lacks DDGI atlas format " << format;
    }
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
    vk::SubmitInfo info;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &handle;
    vk::raii::Fence fence(device->raii(), vk::FenceCreateInfo());
    vk::raii::Queue queue(
        device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(info, *fence);
    if (device->raii().waitForFences(*fence, true, UINT64_MAX) !=
        vk::Result::eSuccess)
      throw std::runtime_error("DDGI test fence wait failed");
  }
  void barrier(vk::PipelineStageFlags2 sourceStage,
               vk::AccessFlags2 sourceAccess, vk::PipelineStageFlags2 destStage,
               vk::AccessFlags2 destAccess) {
    vk::MemoryBarrier2 memoryBarrier(sourceStage, sourceAccess, destStage,
                                     destAccess);
    vk::DependencyInfo dependency;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memoryBarrier;
    command.pipelineBarrier2(dependency);
  }

  struct Harness {
    DdgiGpu &owner;
    uint32_t probeCount, width, height;
    bool initialized = false;
    HostBuffer uniform, states, rays, updates, irradianceReadback,
        visibilityReadback;
    TestImage previousIrradiance, previousVisibility, irradiance, visibility;
    vk::raii::DescriptorSetLayout layout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool pool{nullptr};
    std::vector<vk::raii::DescriptorSet> sets;
    std::vector<uint16_t> irradiancePixels;
    std::vector<glm::vec2> visibilityPixels;
    std::vector<DdgiProbeState> resultStates;
    glm::vec4 previousColor{0};
    Harness(DdgiGpu &o, uint32_t count, glm::vec4 previous = glm::vec4(0))
        : owner(o), probeCount(count), width(tile * count), height(tile),
          uniform(*o.memory, sizeof(GpuData),
                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
          states(*o.memory, count * sizeof(DdgiProbeState),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          rays(*o.memory, count * raysPerProbe * sizeof(DdgiRaySample),
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          updates(*o.memory, count * sizeof(DdgiProbeUpdate),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          irradianceReadback(*o.memory, size_t(width) * height * 8,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT),
          visibilityReadback(*o.memory, size_t(width) * height * 8,
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT),
          previousIrradiance(o.device->raii(), *o.memory, width, height,
                             VK_FORMAT_R16G16B16A16_SFLOAT),
          previousVisibility(o.device->raii(), *o.memory, width, height,
                             VK_FORMAT_R32G32_SFLOAT),
          irradiance(o.device->raii(), *o.memory, width, height,
                     VK_FORMAT_R16G16B16A16_SFLOAT),
          visibility(o.device->raii(), *o.memory, width, height,
                     VK_FORMAT_R32G32_SFLOAT),
          previousColor(previous) {
      std::vector<DdgiProbeState> zero(count);
      states.upload(zero.data(), zero.size() * sizeof(zero.front()));
      const std::array<uint32_t, 8> indices{0, 1, 2, 4, 5, 6, 7, 8};
      std::array<vk::DescriptorSetLayoutBinding, 8> bindings;
      for (size_t i = 0; i < indices.size(); ++i) {
        const auto binding = indices[i];
        const auto type = binding == 0  ? vk::DescriptorType::eUniformBuffer
                          : binding < 3 ? vk::DescriptorType::eSampledImage
                          : binding > 6 ? vk::DescriptorType::eStorageImage
                                        : vk::DescriptorType::eStorageBuffer;
        bindings[i] = {binding, type, 1, vk::ShaderStageFlagBits::eCompute};
      }
      const auto &device = owner.device->raii();
      layout = vk::raii::DescriptorSetLayout(
          device, vk::DescriptorSetLayoutCreateInfo({}, bindings));
      pipelineLayout = vk::raii::PipelineLayout(
          device, vk::PipelineLayoutCreateInfo({}, *layout));
      const std::array sizes{
          vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1),
          vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 2),
          vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3),
          vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 2)};
      pool = vk::raii::DescriptorPool(
          device,
          vk::DescriptorPoolCreateInfo(
              vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
      sets = device.allocateDescriptorSets(
          vk::DescriptorSetAllocateInfo(*pool, *layout));
      const std::array<const HostBuffer *, 4> buffers{&uniform, &states, &rays,
                                                      &updates};
      const std::array<uint32_t, 4> bufferBindings{0, 4, 5, 6};
      for (uint32_t i = 0; i < buffers.size(); ++i) {
        vk::DescriptorBufferInfo info(buffers[i]->value.buffer, 0,
                                      buffers[i]->size);
        device.updateDescriptorSets(
            vk::WriteDescriptorSet(*sets.front(), bufferBindings[i], 0, 1,
                                   i == 0 ? vk::DescriptorType::eUniformBuffer
                                          : vk::DescriptorType::eStorageBuffer,
                                   nullptr, &info),
            {});
      }
      const std::array<const TestImage *, 4> images{
          &previousIrradiance, &previousVisibility, &irradiance, &visibility};
      const std::array<uint32_t, 4> imageBindings{1, 2, 7, 8};
      for (uint32_t i = 0; i < images.size(); ++i) {
        vk::DescriptorImageInfo info({}, *images[i]->view,
                                     vk::ImageLayout::eGeneral);
        device.updateDescriptorSets(
            vk::WriteDescriptorSet(*sets.front(), imageBindings[i], 0, 1,
                                   i < 2 ? vk::DescriptorType::eSampledImage
                                         : vk::DescriptorType::eStorageImage,
                                   &info),
            {});
      }
      vk::raii::ShaderModule module(device,
                                    vk::ShaderModuleCreateInfo({}, owner.code));
      vk::PipelineShaderStageCreateInfo stage(
          {}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain");
      pipeline = vk::raii::Pipeline(
          device, nullptr,
          vk::ComputePipelineCreateInfo({}, stage, *pipelineLayout));
    }
    GpuData data(uint32_t updateCount, uint32_t frame = 7) const {
      GpuData d;
      d.spacingViewBias = {1, 1, 1, 0};
      d.counts = {probeCount, 1, 1, probeCount};
      d.irradianceLayout =
          d.visibilityLayout = {interior, tile, probeCount, width};
      d.extents = {height, height, updateCount, raysPerProbe};
      d.trace = {20, .95f, 50, 3};
      d.frame = {frame, 1, 0, 0};
      return d;
    }
    void run(const GpuData &d, std::span<const DdgiRaySample> samples,
             std::span<const DdgiProbeUpdate> records) {
      uniform.upload(&d, sizeof(d));
      rays.upload(samples.data(), samples.size_bytes());
      updates.upload(records.data(), records.size_bytes());
      owner.begin();
      if (!initialized) {
        for (const auto *image : {&previousIrradiance, &previousVisibility,
                                  &irradiance, &visibility}) {
          vk::ImageMemoryBarrier2 b;
          b.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
          b.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
          b.oldLayout = vk::ImageLayout::eUndefined;
          b.newLayout = vk::ImageLayout::eGeneral;
          b.srcQueueFamilyIndex = b.dstQueueFamilyIndex =
              VK_QUEUE_FAMILY_IGNORED;
          b.image = image->image;
          b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
          vk::DependencyInfo dependency;
          dependency.imageMemoryBarrierCount = 1;
          dependency.pImageMemoryBarriers = &b;
          owner.command.pipelineBarrier2(dependency);
        }
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor,
                                              0, 1, 0, 1);
        owner.command.clearColorImage(previousIrradiance.image,
                                      vk::ImageLayout::eGeneral,
                                      vk::ClearColorValue(std::array<float, 4>{
                                          previousColor.x, previousColor.y,
                                          previousColor.z, previousColor.w}),
                                      range);
        owner.command.clearColorImage(
            previousVisibility.image, vk::ImageLayout::eGeneral,
            vk::ClearColorValue(std::array<float, 4>{3, 9, 0, 0}), range);
        owner.command.clearColorImage(
            irradiance.image, vk::ImageLayout::eGeneral,
            vk::ClearColorValue(std::array<float, 4>{7, 8, 9, 0}), range);
        owner.command.clearColorImage(
            visibility.image, vk::ImageLayout::eGeneral,
            vk::ClearColorValue(std::array<float, 4>{4, 16, 0, 0}), range);
        initialized = true;
      }
      owner.barrier(
          vk::PipelineStageFlagBits2::eAllCommands |
              vk::PipelineStageFlagBits2::eHost,
          vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite |
              vk::AccessFlagBits2::eHostWrite,
          vk::PipelineStageFlagBits2::eComputeShader |
              vk::PipelineStageFlagBits2::eTransfer,
          vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
              vk::AccessFlagBits2::eTransferRead |
              vk::AccessFlagBits2::eTransferWrite);
      owner.command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
      owner.command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                       *pipelineLayout, 0, *sets.front(), {});
      // Dispatch one guarded group for pause, proving zero extents preserves
      // resources even when a caller accidentally issues an empty-budget pass.
      owner.command.dispatch(std::max(d.extents.z, 1u), 1, 1);
      owner.barrier(vk::PipelineStageFlagBits2::eComputeShader |
                        vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eShaderWrite |
                        vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eTransfer |
                        vk::PipelineStageFlagBits2::eHost,
                    vk::AccessFlagBits2::eTransferRead |
                        vk::AccessFlagBits2::eHostRead);
      vk::BufferImageCopy copy;
      copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
      copy.imageExtent = vk::Extent3D(width, height, 1);
      owner.command.copyImageToBuffer(irradiance.image,
                                      vk::ImageLayout::eGeneral,
                                      irradianceReadback.value.buffer, copy);
      owner.command.copyImageToBuffer(visibility.image,
                                      vk::ImageLayout::eGeneral,
                                      visibilityReadback.value.buffer, copy);
      owner.barrier(vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferWrite,
                    vk::PipelineStageFlagBits2::eHost,
                    vk::AccessFlagBits2::eHostRead);
      owner.submit();
      states.invalidate();
      irradianceReadback.invalidate();
      visibilityReadback.invalidate();
      resultStates.resize(probeCount);
      irradiancePixels.resize(size_t(width) * height * 4);
      visibilityPixels.resize(size_t(width) * height);
      std::memcpy(resultStates.data(), states.value.allocation_info.pMappedData,
                  resultStates.size() * sizeof(DdgiProbeState));
      std::memcpy(irradiancePixels.data(),
                  irradianceReadback.value.allocation_info.pMappedData,
                  irradiancePixels.size() * sizeof(uint16_t));
      std::memcpy(visibilityPixels.data(),
                  visibilityReadback.value.allocation_info.pMappedData,
                  visibilityPixels.size() * sizeof(glm::vec2));
    }
    glm::vec4 irradianceAt(uint32_t probe, uint32_t x, uint32_t y) const {
      const auto offset = (size_t(y) * width + probe * tile + x) * 4;
      const auto first = uint32_t(irradiancePixels[offset]) |
                         uint32_t(irradiancePixels[offset + 1]) << 16;
      const auto second = uint32_t(irradiancePixels[offset + 2]) |
                          uint32_t(irradiancePixels[offset + 3]) << 16;
      return {glm::unpackHalf2x16(first), glm::unpackHalf2x16(second)};
    }
    glm::vec2 visibilityAt(uint32_t probe, uint32_t x, uint32_t y) const {
      return visibilityPixels[size_t(y) * width + probe * tile + x];
    }
  };

  static std::vector<DdgiRaySample> constantRays(uint32_t probes, glm::vec3 li,
                                                 float distance = 2,
                                                 float status = 0) {
    std::vector<DdgiRaySample> result(size_t(probes) * raysPerProbe);
    for (uint32_t probe = 0; probe < probes; ++probe)
      for (uint32_t ray = 0; ray < raysPerProbe; ++ray) {
        const double y = 1.0 - 2.0 * (double(ray) + .5) / raysPerProbe;
        const double radius = std::sqrt(std::max(1.0 - y * y, 0.0));
        const double angle = double(ray) * 2.399963229728653;
        result[size_t(probe) * raysPerProbe + ray] = {
            {li.x, li.y, li.z, distance},
            {float(radius * std::cos(angle)), float(y),
             float(radius * std::sin(angle)), status}};
      }
    return result;
  }
  GLFWwindow *window{};
  bool glfwStarted{};
  std::atomic_uint validationErrors{0};
  std::vector<uint32_t> code;
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr};
  vk::raii::SurfaceKHR surface{nullptr};
  std::unique_ptr<VulkanDevice> device;
  std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool pool{nullptr};
  vk::raii::CommandBuffer command{nullptr};
};

TEST_F(DdgiGpu,
       ConstantSphericalRadianceIntegratesToPiLiAndPreservesOtherProbes) {
  Harness h(*this, 2);
  const glm::vec3 li(.25f, .5f, 1.f);
  const auto rays = constantRays(2, li);
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe}};
  h.run(h.data(1), rays, updates);
  EXPECT_EQ(h.resultStates[0].classification, DdgiProbeClassification::Active);
  EXPECT_EQ(h.resultStates[0].validRayCount, raysPerProbe);
  EXPECT_EQ(h.resultStates[0].lastUpdatedFrame, 7u);
  EXPECT_EQ(h.resultStates[0].flags, 0u);
  EXPECT_EQ(h.resultStates[1].classification,
            DdgiProbeClassification::Uninitialized);
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) {
      const auto e = h.irradianceAt(0, x, y);
      for (uint32_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(e[channel], std::numbers::pi_v<float> * li[channel], .015f)
            << "texel " << x << "," << y << " channel " << channel;
      EXPECT_FLOAT_EQ(e.w, 1.f);
      const auto moments = h.visibilityAt(0, x, y);
      EXPECT_NEAR(moments.x, 2.f, 1e-4f);
      EXPECT_NEAR(moments.y, 4.f, 1e-4f);
      EXPECT_EQ(h.irradianceAt(1, x, y), glm::vec4(7, 8, 9, 0));
      EXPECT_EQ(h.visibilityAt(1, x, y), glm::vec2(4, 16));
    }
}

TEST_F(DdgiGpu, PackedVolumesUseTheirOwnCellSizeForRelocationBounds) {
  Harness h(*this,2);
  DdgiSettings settings;
  settings.volume={{0,0,0},{2,2,2},{1,1,1}};
  settings.additionalVolumes.push_back({{1,0,0},{.5f,.5f,.5f},{1,1,1}});
  auto data=h.data(2); data.intensityBias.z=2;
  data.volumes=DdgiVolumeSet(settings).gpuVolumes(settings);
  auto rays=constantRays(2,glm::vec3(0),1,-1);
  for(auto &ray:rays) ray.directionAndReserved={1,0,0,-1};
  const std::array updates{DdgiProbeUpdate{0,0,raysPerProbe},
      DdgiProbeUpdate{1,raysPerProbe,raysPerProbe}};
  h.run(data,rays,updates);
  EXPECT_NEAR(h.resultStates[0].relocation[0],.9f,1e-5f);
  EXPECT_NEAR(h.resultStates[1].relocation[0],.225f,1e-5f);
  for(const auto &state:h.resultStates) {
    EXPECT_EQ(state.classification,DdgiProbeClassification::Inactive);
    EXPECT_EQ(state.validRayCount,0u);
  }
}

TEST_F(DdgiGpu, HysteresisBlendsExistingActiveHistoryAndDistanceMoments) {
  Harness h(*this, 1, glm::vec4(6, 4, 2, 1));
  DdgiProbeState initial;
  initial.classification = DdgiProbeClassification::Active;
  initial.validRayCount = raysPerProbe;
  h.states.upload(&initial, sizeof(initial));
  const auto rays = constantRays(1, glm::vec3(.5f));
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe}};
  auto data = h.data(1);
  data.frame.z = 1;
  data.trace.y = .5f;
  h.run(data, rays, updates);
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) {
      const auto e = h.irradianceAt(0, x, y);
      for (uint32_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(
            e[channel],
            .5f * (std::numbers::pi_v<float> * .5f + h.previousColor[channel]),
            .015f);
      const auto moments = h.visibilityAt(0, x, y);
      EXPECT_NEAR(moments.x, 2.5f, 1e-4f);
      EXPECT_NEAR(moments.y, 6.5f, 1e-4f);
    }
}

TEST_F(DdgiGpu,
       ContentEpochRefreshesOnlySelectedProbeWithoutBlendingStaleHistory) {
  Harness h(*this, 2, glm::vec4(6, 4, 2, 1));
  std::array<DdgiProbeState, 2> initial;
  for (auto &state : initial) {
    state.classification = DdgiProbeClassification::Active;
    state.validRayCount = raysPerProbe;
    state.lastUpdatedFrame = 3;
    state.reserved = 1; // The atlas contains the previous content epoch.
  }
  h.states.upload(initial.data(), sizeof(initial));
  const glm::vec3 li(.25f, .5f, 1.f);
  const auto rays = constantRays(2, li);
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe, 2}};
  auto data = h.data(1);
  data.frame.z = 1; // Global history is valid; only this probe is stale.
  data.trace.y = .95f;
  h.run(data, rays, updates);
  EXPECT_EQ(h.resultStates[0].classification, DdgiProbeClassification::Active);
  EXPECT_EQ(h.resultStates[0].validRayCount, raysPerProbe);
  EXPECT_EQ(h.resultStates[0].lastUpdatedFrame, 7u);
  EXPECT_EQ(h.resultStates[0].reserved, 2u);
  EXPECT_EQ(
      std::memcmp(&h.resultStates[1], &initial[1], sizeof(DdgiProbeState)), 0);
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) {
      const auto e = h.irradianceAt(0, x, y);
      for (uint32_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(e[channel], std::numbers::pi_v<float> * li[channel], .015f)
            << "fresh epoch texel " << x << "," << y << " channel " << channel;
      const auto moments = h.visibilityAt(0, x, y);
      EXPECT_NEAR(moments.x, 2.f, 1e-4f);
      EXPECT_NEAR(moments.y, 4.f, 1e-4f);
      EXPECT_EQ(h.irradianceAt(1, x, y), glm::vec4(7, 8, 9, 0));
      EXPECT_EQ(h.visibilityAt(1, x, y), glm::vec2(4, 16));
    }
}

TEST_F(DdgiGpu,
       PartialInvalidRaySetsKeepGeneratedDirectionDenominator) {
  Harness h(*this, 1);
  const glm::vec3 li(.25f, .5f, 1.f);
  auto rays = constantRays(1, li);
  // Alternating Fibonacci directions retain uniform spherical coverage. The
  // invalid half contributes zero to E, while the PDF denominator remains the
  // complete generated set; renormalizing survivors would double its energy.
  for (uint32_t ray = 0; ray < raysPerProbe; ray += 2)
    rays[ray].radianceAndDistance[0] =
        std::numeric_limits<float>::quiet_NaN();
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe}};
  h.run(h.data(1), rays, updates);
  EXPECT_EQ(h.resultStates[0].classification, DdgiProbeClassification::Active);
  EXPECT_EQ(h.resultStates[0].validRayCount, raysPerProbe / 2);
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) {
      const auto e = h.irradianceAt(0, x, y);
      for (uint32_t channel = 0; channel < 3; ++channel)
        EXPECT_NEAR(e[channel], .5f * std::numbers::pi_v<float> * li[channel], .015f)
            << "partial ray texel " << x << "," << y << " channel " << channel;
      EXPECT_FLOAT_EQ(e.w, 1.f);
      const auto moments = h.visibilityAt(0, x, y);
      EXPECT_NEAR(moments.x, 2.f, 1e-4f);
      EXPECT_NEAR(moments.y, 4.f, 1e-4f);
    }
}

TEST_F(DdgiGpu, FiniteHdrRadianceSaturatesHalfAtlasAndPublishesClampFlag) {
  Harness h(*this, 1);
  const auto rays = constantRays(1,
      glm::vec3(std::numeric_limits<float>::max()));
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe}};
  h.run(h.data(1), rays, updates);
  EXPECT_EQ(h.resultStates[0].classification, DdgiProbeClassification::Active);
  EXPECT_EQ(h.resultStates[0].validRayCount, raysPerProbe);
  EXPECT_NE(h.resultStates[0].flags & 4u, 0u);
  for (uint32_t y = 0; y < tile; ++y)
    for (uint32_t x = 0; x < tile; ++x) {
      const auto e = h.irradianceAt(0, x, y);
      for (uint32_t channel = 0; channel < 3; ++channel) {
        EXPECT_TRUE(std::isfinite(e[channel]));
        EXPECT_FLOAT_EQ(e[channel], 65504.f);
      }
      EXPECT_FLOAT_EQ(e.w, 1.f);
      const auto moments = h.visibilityAt(0, x, y);
      EXPECT_NEAR(moments.x, 2.f, 1e-4f);
      EXPECT_NEAR(moments.y, 4.f, 1e-4f);
    }
}

TEST_F(DdgiGpu,
       ClassificationRejectsBackfacesAndNonfiniteRaysButRetainsOccluders) {
  Harness h(*this, 3);
  auto rays = constantRays(3, glm::vec3(4), 2, -1);
  for (uint32_t ray = 0; ray < raysPerProbe; ++ray) {
    rays[raysPerProbe + ray].directionAndReserved[3] = 2;
    rays[2 * raysPerProbe + ray].radianceAndDistance[0] =
        std::numeric_limits<float>::quiet_NaN();
  }
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe},
                           DdgiProbeUpdate{1, raysPerProbe, raysPerProbe},
                           DdgiProbeUpdate{2, 2 * raysPerProbe, raysPerProbe}};
  h.run(h.data(3), rays, updates);
  EXPECT_EQ(h.resultStates[0].classification,
            DdgiProbeClassification::Inactive);
  EXPECT_EQ(h.resultStates[0].validRayCount, 0u);
  EXPECT_NE(h.resultStates[0].flags & 1u, 0u);
  for (float offset : h.resultStates[0].relocation)
    EXPECT_LE(std::abs(offset), .45001f);
  EXPECT_EQ(h.resultStates[1].classification, DdgiProbeClassification::Active);
  EXPECT_EQ(h.resultStates[1].validRayCount, raysPerProbe);
  EXPECT_NE(h.resultStates[1].flags & 2u, 0u);
  EXPECT_EQ(h.resultStates[2].classification,
            DdgiProbeClassification::Inactive);
  EXPECT_EQ(h.resultStates[2].validRayCount, 0u);
  for (uint32_t probe = 0; probe < 3; ++probe)
    for (uint32_t y = 0; y < tile; ++y)
      for (uint32_t x = 0; x < tile; ++x) {
        const auto e = h.irradianceAt(probe, x, y);
        EXPECT_EQ(glm::vec3(e), glm::vec3(0));
        const auto moments = h.visibilityAt(probe, x, y);
        EXPECT_NEAR(moments.x, probe == 2 ? 20.f : 2.f, 1e-4f);
        EXPECT_NEAR(moments.y, probe == 2 ? 400.f : 4.f, 1e-4f);
      }
}

TEST_F(DdgiGpu, PausedUpdatePreservesSubmittedAtlasAndProbeStateBitwise) {
  Harness h(*this, 1);
  const auto rays = constantRays(1, glm::vec3(.5f));
  const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe}};
  h.run(h.data(1), rays, updates);
  const auto beforeIrradiance = h.irradiancePixels;
  const auto beforeVisibility = h.visibilityPixels;
  const auto beforeStates = h.resultStates;
  h.run(h.data(0, 99), constantRays(1, glm::vec3(100)), {});
  EXPECT_EQ(h.irradiancePixels, beforeIrradiance);
  ASSERT_EQ(h.visibilityPixels.size(), beforeVisibility.size());
  EXPECT_EQ(std::memcmp(h.visibilityPixels.data(), beforeVisibility.data(),
                        beforeVisibility.size() * sizeof(glm::vec2)),
            0);
  ASSERT_EQ(h.resultStates.size(), beforeStates.size());
  EXPECT_EQ(std::memcmp(h.resultStates.data(), beforeStates.data(),
                        beforeStates.size() * sizeof(DdgiProbeState)),
            0);
}

TEST_F(DdgiGpu, RelocationRemainsScaleRelativeAndInvalidUntilRetraced) {
  for (const float spacing : {1.f, 5e-7f, 1e-15f}) {
    SCOPED_TRACE(spacing);
    Harness h(*this, 1);
    auto data = h.data(1);
    data.spacingViewBias = glm::vec4(glm::vec3(spacing), 0.f);
    data.trace.x = 20.f * spacing;
    const auto enclosed = constantRays(1, glm::vec3(4), .04f * spacing, -1.f);
    const std::array updates{DdgiProbeUpdate{0, 0, raysPerProbe}};
    h.run(data, enclosed, updates);

    // The nearest backface is .04 cells away and the clearance is .02.
    // Its escape is feasible at every scale, including cells whose complete
    // relocation allowance is smaller than the old fixed world-unit epsilon.
    const auto relocated = h.resultStates[0].relocation;
    EXPECT_EQ(h.resultStates[0].classification,
              DdgiProbeClassification::Inactive);
    EXPECT_EQ(h.resultStates[0].validRayCount, 0u);
    EXPECT_NE(h.resultStates[0].flags & 1u, 0u);
    for (uint32_t axis = 0; axis < 3; ++axis) {
      EXPECT_NEAR(relocated[axis] / spacing,
                  .06f * enclosed.front().directionAndReserved[axis], 1e-6f);
      EXPECT_LE(std::abs(relocated[axis] / spacing), .45001f);
    }
    for (uint32_t y = 0; y < tile; ++y)
      for (uint32_t x = 0; x < tile; ++x)
        EXPECT_EQ(h.irradianceAt(0, x, y), glm::vec4(0));

    // A complete ray set from the new origin can publish the moved probe.
    // Misses require no further relocation and provide known finite radiance.
    data.frame.x += 1;
    data.frame.z = 1;
    h.run(data, constantRays(1, glm::vec3(.5f), data.trace.x), updates);
    EXPECT_EQ(h.resultStates[0].classification,
              DdgiProbeClassification::Active);
    EXPECT_EQ(h.resultStates[0].validRayCount, raysPerProbe);
    EXPECT_EQ(h.resultStates[0].lastUpdatedFrame, data.frame.x);
    EXPECT_EQ(h.resultStates[0].relocation, relocated);
    EXPECT_EQ(h.resultStates[0].flags, 0u);
    for (uint32_t y = 0; y < tile; ++y)
      for (uint32_t x = 0; x < tile; ++x) {
        const auto e = h.irradianceAt(0, x, y);
        for (uint32_t channel = 0; channel < 3; ++channel)
          EXPECT_NEAR(e[channel], std::numbers::pi_v<float> * .5f, .015f);
        EXPECT_FLOAT_EQ(e.w, 1.f);
      }
  }
}
} // namespace
