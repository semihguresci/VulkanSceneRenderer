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
constexpr uint32_t interior = 2, tile = interior + 2;

// Mirror the production UBO, while testing the actual production lookup
// function rather than a second implementation of its interpolation math.
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
      throw std::runtime_error("invalid DDGI lookup test upload");
    if (bytes)
      std::memcpy(value.allocation_info.pMappedData, source, bytes);
    if (vmaFlushAllocation(memory.allocator(), value.allocation, 0,
                           VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI lookup test upload flush failed");
  }
  void invalidate() {
    if (vmaInvalidateAllocation(memory.allocator(), value.allocation, 0,
                                VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("DDGI lookup test readback invalidate failed");
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
      throw std::runtime_error("DDGI lookup test image allocation failed");
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

class DdgiLookupGpu : public ::testing::Test {
protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<DdgiLookupGpu *>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }
  void SetUp() override {
    const auto *enabled = std::getenv("CONTAINER_RUN_GPU_DDGI");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_DDGI=1 for Vulkan DDGI contracts";
    std::ifstream shader(CONTAINER_DDGI_LOOKUP_SPIRV,
                         std::ios::binary | std::ios::ate);
    ASSERT_TRUE(shader) << "Production DDGI lookup SPIR-V is missing";
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
    window = glfwCreateWindow(64, 64, "DDGI lookup contract", nullptr, nullptr);
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
      constexpr auto required =
          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
          VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
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
      throw std::runtime_error("DDGI lookup test fence wait failed");
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

  struct alignas(16) Query {
    glm::vec4 world{0}, geometricNormal{0, 1, 0, 0}, shadingNormal{0, 1, 0, 0},
        viewDirection{0, 1, 0, 0};
  };
  static_assert(sizeof(Query) == 64);

  struct Harness {
    DdgiLookupGpu &owner;
    uint32_t probeCount, width, height;
    HostBuffer uniform, states, positions, queries, results, irradianceUpload,
        visibilityUpload;
    TestImage irradiance, visibility;
    vk::raii::Sampler sampler{nullptr};
    vk::raii::DescriptorSetLayout layout{nullptr};
    vk::raii::PipelineLayout pipelineLayout{nullptr};
    vk::raii::Pipeline pipeline{nullptr};
    vk::raii::DescriptorPool pool{nullptr};
    std::vector<vk::raii::DescriptorSet> sets;
    bool initialized = false;

    Harness(DdgiLookupGpu &o, uint32_t count)
        : owner(o), probeCount(count), width(tile * count), height(tile),
          uniform(*o.memory, sizeof(GpuData),
                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
          states(*o.memory, count * sizeof(DdgiProbeState),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          positions(*o.memory, count * sizeof(glm::vec4),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          queries(*o.memory, 16 * sizeof(Query),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          results(*o.memory, 16 * sizeof(glm::vec4),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          irradianceUpload(*o.memory, size_t(width) * height * 8,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
          visibilityUpload(*o.memory, size_t(width) * height * 8,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
          irradiance(o.device->raii(), *o.memory, width, height,
                     VK_FORMAT_R16G16B16A16_SFLOAT),
          visibility(o.device->raii(), *o.memory, width, height,
                     VK_FORMAT_R32G32_SFLOAT) {
      const auto &device = owner.device->raii();
      vk::SamplerCreateInfo samplerInfo;
      samplerInfo.magFilter = samplerInfo.minFilter = vk::Filter::eLinear;
      samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
      samplerInfo.addressModeU = samplerInfo.addressModeV =
          samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
      sampler = vk::raii::Sampler(device, samplerInfo);
      const std::array<uint32_t, 8> indices{0, 1, 2, 3, 4, 11, 12, 13};
      std::array<vk::DescriptorSetLayoutBinding, 8> bindings;
      for (size_t i = 0; i < indices.size(); ++i) {
        const auto binding = indices[i];
        const auto type = binding == 0   ? vk::DescriptorType::eUniformBuffer
                          : binding < 3  ? vk::DescriptorType::eSampledImage
                          : binding == 3 ? vk::DescriptorType::eSampler
                                         : vk::DescriptorType::eStorageBuffer;
        bindings[i] = {binding, type, 1, vk::ShaderStageFlagBits::eCompute};
      }
      layout = vk::raii::DescriptorSetLayout(
          device, vk::DescriptorSetLayoutCreateInfo({}, bindings));
      pipelineLayout = vk::raii::PipelineLayout(
          device, vk::PipelineLayoutCreateInfo({}, *layout));
      const std::array sizes{
          vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, 1),
          vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 2),
          vk::DescriptorPoolSize(vk::DescriptorType::eSampler, 1),
          vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 4)};
      pool = vk::raii::DescriptorPool(
          device,
          vk::DescriptorPoolCreateInfo(
              vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
      sets = device.allocateDescriptorSets(
          vk::DescriptorSetAllocateInfo(*pool, *layout));
      const std::array<const HostBuffer *, 5> buffers{
          &uniform, &states, &positions, &queries, &results};
      const std::array<uint32_t, 5> bufferBindings{0, 4, 11, 12, 13};
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
      const std::array<const TestImage *, 2> images{&irradiance, &visibility};
      for (uint32_t i = 0; i < images.size(); ++i) {
        vk::DescriptorImageInfo info({}, *images[i]->view,
                                     vk::ImageLayout::eShaderReadOnlyOptimal);
        device.updateDescriptorSets(
            vk::WriteDescriptorSet(*sets.front(), i + 1, 0, 1,
                                   vk::DescriptorType::eSampledImage, &info),
            {});
      }
      vk::DescriptorImageInfo samplerDescriptor(*sampler, {}, {});
      device.updateDescriptorSets(
          vk::WriteDescriptorSet(*sets.front(), 3, 0, 1,
                                 vk::DescriptorType::eSampler,
                                 &samplerDescriptor),
          {});
      vk::raii::ShaderModule module(device,
                                    vk::ShaderModuleCreateInfo({}, owner.code));
      vk::PipelineShaderStageCreateInfo stage(
          {}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain");
      pipeline = vk::raii::Pipeline(
          device, nullptr,
          vk::ComputePipelineCreateInfo({}, stage, *pipelineLayout));
    }

    GpuData data(float scale = 1.f) const {
      GpuData d;
      d.spacingViewBias = {scale, scale, scale, 0};
      d.counts = {probeCount, 1, 1, probeCount};
      d.irradianceLayout =
          d.visibilityLayout = {interior, tile, probeCount, width};
      d.extents = {height, height, 0, 16};
      d.trace = {20 * scale, 0, 50, 3};
      d.frame = {1, 1, 1, 0};
      return d;
    }

    std::vector<glm::vec4>
    run(const GpuData &d, std::span<const Query> inputQueries,
        std::span<const glm::vec3> colors, std::span<const glm::vec2> moments,
        std::span<const DdgiProbeState> inputStates = {}) {
      if (inputQueries.empty() || inputQueries.size() > 16 ||
          colors.size() != probeCount || moments.size() != probeCount)
        throw std::runtime_error("Invalid DDGI lookup test inputs");
      uniform.upload(&d, sizeof(d));
      queries.upload(inputQueries.data(), inputQueries.size_bytes());
      std::vector<DdgiProbeState> defaultStates(probeCount);
      for (auto &state : defaultStates) {
        state.classification = DdgiProbeClassification::Active;
        state.validRayCount = 16;
      }
      if (inputStates.empty())
        inputStates = defaultStates;
      if (inputStates.size() != probeCount)
        throw std::runtime_error("Invalid DDGI lookup test state count");
      states.upload(inputStates.data(), inputStates.size_bytes());
      std::vector<glm::vec4> basePositions(probeCount);
      for (uint32_t probe = 0; probe < probeCount; ++probe)
        basePositions[probe] = {float(probe) * d.spacingViewBias.x, 0, 0, 0};
      if (d.intensityBias.z > 0.f) {
        for (uint32_t v = 0; v < uint32_t(d.intensityBias.z); ++v) {
          const auto &gpu = d.volumes[v];
          const DdgiProbeVolume volume({glm::vec3(gpu.originBlend),
              glm::vec3(gpu.spacingNormalBias), glm::uvec3(gpu.countsOffset)});
          for (uint32_t i = 0; i < volume.probeCount(); ++i)
            basePositions.at(gpu.countsOffset.w + i) = glm::vec4(volume.probePosition(i),0);
        }
      }
      positions.upload(basePositions.data(),
                       basePositions.size() * sizeof(basePositions.front()));
      std::vector<uint16_t> irradiancePixels(size_t(width) * height * 4);
      std::vector<glm::vec2> visibilityPixels(size_t(width) * height);
      for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          const auto probe = x / tile;
          const auto pixel = size_t(y) * width + x;
          for (uint32_t channel = 0; channel < 3; ++channel)
            irradiancePixels[pixel * 4 + channel] =
                glm::packHalf1x16(colors[probe][channel]);
          irradiancePixels[pixel * 4 + 3] = glm::packHalf1x16(1.f);
          visibilityPixels[pixel] = moments[probe];
        }
      irradianceUpload.upload(irradiancePixels.data(),
                              irradiancePixels.size() * sizeof(uint16_t));
      visibilityUpload.upload(visibilityPixels.data(),
                              visibilityPixels.size() * sizeof(glm::vec2));
      owner.begin();
      owner.barrier(vk::PipelineStageFlagBits2::eHost,
                    vk::AccessFlagBits2::eHostWrite,
                    vk::PipelineStageFlagBits2::eTransfer |
                        vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eTransferRead |
                        vk::AccessFlagBits2::eShaderRead |
                        vk::AccessFlagBits2::eShaderWrite);
      for (const auto *image : {&irradiance, &visibility}) {
        vk::ImageMemoryBarrier2 b;
        b.srcStageMask = initialized
                             ? vk::PipelineStageFlagBits2::eComputeShader
                             : vk::PipelineStageFlagBits2::eNone;
        b.srcAccessMask = initialized ? vk::AccessFlagBits2::eShaderRead
                                      : vk::AccessFlagBits2::eNone;
        b.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        b.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
        b.oldLayout = initialized ? vk::ImageLayout::eShaderReadOnlyOptimal
                                  : vk::ImageLayout::eUndefined;
        b.newLayout = vk::ImageLayout::eTransferDstOptimal;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image->image;
        b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::DependencyInfo dependency;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        owner.command.pipelineBarrier2(dependency);
      }
      vk::BufferImageCopy copy;
      copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
      copy.imageExtent = vk::Extent3D(width, height, 1);
      owner.command.copyBufferToImage(
          irradianceUpload.value.buffer, irradiance.image,
          vk::ImageLayout::eTransferDstOptimal, copy);
      owner.command.copyBufferToImage(
          visibilityUpload.value.buffer, visibility.image,
          vk::ImageLayout::eTransferDstOptimal, copy);
      for (const auto *image : {&irradiance, &visibility}) {
        vk::ImageMemoryBarrier2 b;
        b.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        b.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        b.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        b.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
        b.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        b.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image->image;
        b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::DependencyInfo dependency;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        owner.command.pipelineBarrier2(dependency);
      }
      owner.command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
      owner.command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                       *pipelineLayout, 0, *sets.front(), {});
      owner.command.dispatch(static_cast<uint32_t>(inputQueries.size()), 1, 1);
      owner.barrier(vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite,
                    vk::PipelineStageFlagBits2::eHost,
                    vk::AccessFlagBits2::eHostRead);
      owner.submit();
      initialized = true;
      results.invalidate();
      std::vector<glm::vec4> output(inputQueries.size());
      std::memcpy(output.data(), results.value.allocation_info.pMappedData,
                  output.size() * sizeof(output.front()));
      return output;
    }
  };

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

TEST_F(DdgiLookupGpu, FullyVisibleConstantFieldPreservesPhysicalIrradiance) {
  Harness h(*this, 2);
  const std::array colors{glm::vec3(3, 5, 7), glm::vec3(3, 5, 7)};
  for (const float scale : {1.f, 1e-8f}) {
    const std::array moments{glm::vec2(2 * scale, 4 * scale * scale),
                             glm::vec2(2 * scale, 4 * scale * scale)};
    std::array<Query, 3> query;
    query[0].world.x = 0;
    query[1].world.x = .37f * scale;
    query[2].world.x = scale;
    const auto result = h.run(h.data(scale), query, colors, moments);
    for (const auto &value : result) {
      EXPECT_NEAR(value.x, 3, 2e-5f);
      EXPECT_NEAR(value.y, 5, 2e-5f);
      EXPECT_NEAR(value.z, 7, 2e-5f);
      EXPECT_FLOAT_EQ(value.w, 1);
    }
  }
}

TEST_F(DdgiLookupGpu, NestedVolumeLookupUsesPackedOffsetsAndSmoothFineEdges) {
  Harness h(*this, 8);
  DdgiSettings settings;
  settings.volume = {{-2,0,0},{2,2,2},{3,1,1}};
  settings.additionalVolumes.push_back({{-1,0,0},{.5f,.5f,.5f},{5,1,1}});
  auto data = h.data();
  data.intensityBias.z = 2;
  data.volumes = DdgiVolumeSet(settings).gpuVolumes(settings);
  // Biases zero to isolate selection from receiver geometry conventions.
  for (auto &v : data.volumes) { v.spacingNormalBias.w=0; v.bias={0,0,0,0}; }
  const std::array colors{glm::vec3(2,0,0),glm::vec3(2,0,0),glm::vec3(2,0,0),
      glm::vec3(0,0,6),glm::vec3(0,0,6),glm::vec3(0,0,6),glm::vec3(0,0,6),glm::vec3(0,0,6)};
  std::array<glm::vec2,8> moments; moments.fill({20,400});
  std::array<Query,4> queries;
  queries[0].world.x=-1;
  queries[1].world.x=-.75f;
  queries[2].world.x=0;
  queries[3].world.x=1.5f;
  const auto result = h.run(data,queries,colors,moments);
  EXPECT_NEAR(result[0].x,2,1e-4f); EXPECT_NEAR(result[0].z,0,1e-4f);
  EXPECT_NEAR(result[1].x,1,1e-4f); EXPECT_NEAR(result[1].z,3,1e-4f);
  EXPECT_NEAR(result[2].x,0,1e-4f); EXPECT_NEAR(result[2].z,6,1e-4f);
  EXPECT_NEAR(result[3].x,2,1e-4f); EXPECT_NEAR(result[3].z,0,1e-4f);
  for (const auto &r:result) EXPECT_FLOAT_EQ(r.w,1.f);
}

TEST_F(DdgiLookupGpu, MissingFineSupportFallsBackCoarseButValidBlackDoesNot) {
  Harness h(*this,8);
  DdgiSettings settings;
  settings.volume = {{-2,0,0},{2,2,2},{3,1,1}};
  settings.additionalVolumes.push_back({{-1,0,0},{.5f,.5f,.5f},{5,1,1}});
  auto data=h.data(); data.intensityBias.z=2;
  data.volumes=DdgiVolumeSet(settings).gpuVolumes(settings);
  for(auto &v:data.volumes) {v.spacingNormalBias.w=0;v.bias={0,0,0,0};}
  std::array<glm::vec3,8> colors{};
  std::fill_n(colors.begin(),3,glm::vec3(2,4,6));
  std::array<glm::vec2,8> moments; moments.fill({20,400});
  std::array<Query,1> queries;
  std::array<DdgiProbeState,8> states{};
  for(auto &s:states) {s.classification=DdgiProbeClassification::Active;s.validRayCount=16;}
  const auto black=h.run(data,queries,colors,moments,states);
  EXPECT_EQ(black[0],glm::vec4(0,0,0,1));
  for(uint32_t i=3;i<8;++i) states[i].classification=DdgiProbeClassification::Uninitialized;
  const auto fallback=h.run(data,queries,colors,moments,states);
  EXPECT_NEAR(fallback[0].x,2,1e-4f); EXPECT_NEAR(fallback[0].y,4,1e-4f);
  EXPECT_NEAR(fallback[0].z,6,1e-4f); EXPECT_FLOAT_EQ(fallback[0].w,1.f);
}

TEST_F(DdgiLookupGpu, FacingRejectionIsInvariantUnderWorldUnitScaling) {
  Harness h(*this, 2);
  const std::array colors{glm::vec3(9, 0, 0), glm::vec3(0, 0, 6)};
  for (const float scale : {1.f, 1e-8f}) {
    const std::array moments{glm::vec2(2 * scale, 4 * scale * scale),
                             glm::vec2(2 * scale, 4 * scale * scale)};
    Query query;
    query.world.x = .25f * scale;
    query.geometricNormal = query.shadingNormal = {1, 0, 0, 0};
    const auto result =
        h.run(h.data(scale), std::span(&query, 1), colors, moments);
    EXPECT_FLOAT_EQ(result.front().x, 0);
    EXPECT_FLOAT_EQ(result.front().y, 0);
    EXPECT_NEAR(result.front().z, 6, 2e-5f);
    EXPECT_FLOAT_EQ(result.front().w, 1);
  }
}

TEST_F(DdgiLookupGpu, MomentVisibilityRatioIsInvariantUnderWorldUnitScaling) {
  Harness h(*this, 2);
  const std::array colors{glm::vec3(9, 18, 0), glm::vec3(0)};
  for (const float scale : {1.f, 1e-8f}) {
    // First probe's mean=.25, variance=.0625, receiver distance=.5:
    // Chebyshev=.5 and the configured visibility cube=.125. The other
    // probe is fully visible black, so normalized interpolation gives E/9.
    const std::array moments{glm::vec2(.25f * scale, .125f * scale * scale),
                             glm::vec2(2 * scale, 4 * scale * scale)};
    Query query;
    query.world.x = .5f * scale;
    const auto result =
        h.run(h.data(scale), std::span(&query, 1), colors, moments);
    EXPECT_NEAR(result.front().x, 1, 2e-5f);
    EXPECT_NEAR(result.front().y, 2, 2e-5f);
    EXPECT_FLOAT_EQ(result.front().z, 0);
    EXPECT_FLOAT_EQ(result.front().w, 1);
  }
}

TEST_F(DdgiLookupGpu, CompleteOcclusionIsValidBlackAndDoesNotRestoreFallback) {
  Harness h(*this, 2);
  const std::array colors{glm::vec3(9, 18, 27), glm::vec3(6, 12, 18)};
  for (const float scale : {1.f, 1e-8f}) {
    const std::array moments{glm::vec2(.25f * scale, .0625f * scale * scale),
                             glm::vec2(.25f * scale, .0625f * scale * scale)};
    Query query;
    query.world.x = .5f * scale;
    const auto result =
        h.run(h.data(scale), std::span(&query, 1), colors, moments);
    EXPECT_EQ(result.front(), glm::vec4(0, 0, 0, 1));
  }
}

TEST_F(DdgiLookupGpu, VisibleProbeWinsOverCompletelyBlockedBrightProbe) {
  Harness h(*this, 2);
  const std::array colors{glm::vec3(600), glm::vec3(2, 3, 5)};
  const std::array moments{glm::vec2(.25f, .0625f), glm::vec2(2, 4)};
  Query query;
  query.world.x = .5f;
  const auto result = h.run(h.data(), std::span(&query, 1), colors, moments);
  EXPECT_NEAR(result.front().x, 2, 2e-5f);
  EXPECT_NEAR(result.front().y, 3, 2e-5f);
  EXPECT_NEAR(result.front().z, 5, 2e-5f);
  EXPECT_FLOAT_EQ(result.front().w, 1);
}

TEST_F(DdgiLookupGpu, InvalidMissingAndOutsideFieldsPreserveFallbackCoverage) {
  Harness h(*this, 2);
  const std::array colors{glm::vec3(2), glm::vec3(3)};
  const std::array moments{glm::vec2(2, 4), glm::vec2(2, 4)};
  Query query;
  query.world.x = .5f;
  const auto oneQuery = std::span(&query, 1);
  for (const auto classification : {DdgiProbeClassification::Uninitialized,
                                    DdgiProbeClassification::Inactive}) {
    std::array<DdgiProbeState, 2> states;
    for (auto &state : states) {
      state.classification = classification;
      state.validRayCount = 16;
    }
    EXPECT_EQ(h.run(h.data(), oneQuery, colors, moments, states).front(),
              glm::vec4(0));
  }
  auto d = h.data();
  d.frame.y = 0;
  EXPECT_EQ(h.run(d, oneQuery, colors, moments).front(), glm::vec4(0));
  d = h.data();
  d.frame.z = 0;
  EXPECT_EQ(h.run(d, oneQuery, colors, moments).front(), glm::vec4(0));
  query.world.x = -1;
  EXPECT_EQ(h.run(h.data(), oneQuery, colors, moments).front(), glm::vec4(0));
  query.world.x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_EQ(h.run(h.data(), oneQuery, colors, moments).front(), glm::vec4(0));
  query.world.x = .5f;
  const std::array invalidMoments{
      glm::vec2(std::numeric_limits<float>::quiet_NaN(), 4),
      glm::vec2(std::numeric_limits<float>::quiet_NaN(), 4)};
  EXPECT_EQ(h.run(h.data(), oneQuery, colors, invalidMoments).front(),
            glm::vec4(0));
}

} // namespace
