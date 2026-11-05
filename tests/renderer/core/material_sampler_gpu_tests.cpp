#include <Container/common/CommonGLFW.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/MaterialSamplerKey.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanMemoryManager.h>

#include <gtest/gtest.h>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#ifndef CONTAINER_MATERIAL_SAMPLER_SHADER_DIR
#define CONTAINER_MATERIAL_SAMPLER_SHADER_DIR "."
#endif

namespace {
using namespace container::gpu;
namespace material = container::material;

const std::array<std::vector<glm::vec4>, 3> mipTexels = {{
    {{0,0,0,1}, {1,0,0,1}, {0,0,0,1}, {1,0,0,1},
     {0,1,0,1}, {1,1,0,1}, {0,1,0,1}, {1,1,0,1},
     {0,0,0,1}, {1,0,0,1}, {0,0,0,1}, {1,0,0,1},
     {0,1,0,1}, {1,1,0,1}, {0,1,0,1}, {1,1,0,1}},
    {{1,0,1,1}, {0,1,1,1}, {1,1,0,1}, {0,0,1,1}},
    {{64.0f/255,128.0f/255,192.0f/255,1}}
}};

int wrapIndex(int index, int size, int wrap) {
  if (wrap == 33071) return std::clamp(index, 0, size - 1);
  const int period = wrap == 33648 ? size * 2 : size;
  index = (index % period + period) % period;
  return index >= size ? period - 1 - index : index;
}

glm::vec4 sampleLevel(uint32_t level, glm::vec2 uv, bool linear,
                       const material::MaterialSamplerDescription& policy) {
  const int size = 4 >> level;
  const auto texel = [&](int x, int y) {
    return mipTexels[level][size_t(wrapIndex(y, size, policy.wrapT) * size +
                                    wrapIndex(x, size, policy.wrapS))];
  };
  if (!linear) return texel(int(std::floor(uv.x * size)), int(std::floor(uv.y * size)));
  const auto x = uv.x * size - .5f, y = uv.y * size - .5f;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const auto fx = x - ix, fy = y - iy;
  return (texel(ix, iy) * (1-fx) + texel(ix+1, iy) * fx) * (1-fy) +
         (texel(ix, iy+1) * (1-fx) + texel(ix+1, iy+1) * fx) * fy;
}

glm::vec4 expectedSample(material::MaterialSamplerKey key, glm::vec4 query, uint32_t levels) {
  const auto policy = material::DecodeMaterialSamplerKey(key);
  const glm::vec2 uv(query);
  if (query.z <= 0) return sampleLevel(0, uv, policy.magFilter == 9729, policy);
  const bool linear = policy.minFilter == 9729 || policy.minFilter == 9985 || policy.minFilter == 9987;
  if (policy.minFilter == 9728 || policy.minFilter == 9729)
    return sampleLevel(0, uv, linear, policy);
  const float lod = std::clamp(query.z, 0.0f, float(levels - 1));
  if (policy.minFilter == 9984 || policy.minFilter == 9985)
    return sampleLevel(uint32_t(std::floor(lod + .5f)), uv, linear, policy);
  const auto lower = uint32_t(std::floor(lod)), upper = uint32_t(std::ceil(lod));
  const auto weight = lod - lower;
  return sampleLevel(lower, uv, linear, policy) * (1-weight) + sampleLevel(upper, uv, linear, policy) * weight;
}

class MaterialSamplerGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<MaterialSamplerGpu*>(user)->validationErrors;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char* enabled = std::getenv("CONTAINER_RUN_GPU_MATERIAL_SAMPLERS");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_MATERIAL_SAMPLERS=1 for Vulkan filtering checks";
    const char* override = std::getenv("CONTAINER_MATERIAL_SAMPLER_SHADER_DIR");
    shaderPath = std::filesystem::path(override ? override : CONTAINER_MATERIAL_SAMPLER_SHADER_DIR) /
                 "spv_shaders" / "material_sampler_contract.comp.spv";
    ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath));
    ASSERT_EQ(glfwInit(), GLFW_TRUE); glfwStarted = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window = glfwCreateWindow(32, 32, "Material sampler numerical contract", nullptr, nullptr);
    ASSERT_NE(window, nullptr);
    uint32_t count = 0;
    const auto* extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo create;
    create.enableValidationLayers = true;
    create.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions, extensions + count);
    create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1; validation.pEnabledValidationFeatures = &sync;
    create.next = &validation;
    instance = std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(validationCallback);
    debug.pUserData = this;
    messenger = vk::raii::DebugUtilsMessengerEXT(instance->raii(), debug);
    VkSurfaceKHR surfaceHandle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance->instance(), window, nullptr, &surfaceHandle), VK_SUCCESS);
    surface = vk::raii::SurfaceKHR(instance->raii(), surfaceHandle);
    DeviceCreateInfo info;
    info.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    info.enabledFeatures.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    info.next = &v12;
    device = std::make_shared<VulkanDevice>(*instance, surfaceHandle, info);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &properties);
    capacity = material::MaterialSamplerDescriptorCapacity(properties.limits.maxPerStageDescriptorSamplers,
                                                            properties.limits.maxDescriptorSetSamplers);
    if (capacity < 3) GTEST_SKIP() << "Material sampler descriptor budget is too small for comparison";
    memory = std::make_unique<VulkanMemoryManager>(instance->instance(), device->physicalDevice(), device->device());
    pool = vk::raii::CommandPool(device->raii(), vk::CommandPoolCreateInfo({}, device->queueFamilyIndices().graphicsFamily.value()));
    auto commands = device->raii().allocateCommandBuffers(vk::CommandBufferAllocateInfo(*pool, vk::CommandBufferLevel::ePrimary, 1));
    command = std::move(commands.front());
    createImage();
  }

  void submit() {
    command.end();
    const vk::CommandBuffer raw = *command;
    vk::SubmitInfo info; info.commandBufferCount = 1; info.pCommandBuffers = &raw;
    vk::raii::Queue queue(device->raii(), device->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(info, nullptr); queue.waitIdle();
  }

  void createImage() {
    std::vector<unsigned char> bytes;
    std::array<VkBufferImageCopy, 3> copies{};
    for (uint32_t level = 0; level < 3; ++level) {
      copies[level].bufferOffset = bytes.size();
      copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
      copies[level].imageExtent = {4u >> level, 4u >> level, 1};
      for (auto texel : mipTexels[level])
        for (int channel = 0; channel < 4; ++channel) bytes.push_back(static_cast<unsigned char>(std::lround(texel[channel] * 255)));
    }
    StagingBuffer staging(*memory, bytes.size()); staging.upload(std::as_bytes(std::span(bytes)));
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {4, 4, 1}; info.mipLevels = 3; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocationInfo{}; allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ASSERT_EQ(vmaCreateImage(memory->allocator(), &info, &allocationInfo, &image, &imageMemory, nullptr), VK_SUCCESS);
    for (uint32_t levels : {3u, 1u}) {
      vk::ImageViewCreateInfo view({}, image, vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Unorm,
          {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, levels, 0, 1));
      views.emplace_back(device->raii(), view);
    }
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(*command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(*command, staging.buffer().buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(copies.size()), copies.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(*command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    submit();
  }

  void TearDown() override {
    if (device) device->raii().waitIdle();
    command.clear(); pool.clear(); views.clear();
    if (image) vmaDestroyImage(memory->allocator(), image, imageMemory);
    memory.reset(); device.reset(); surface.clear(); messenger.clear(); instance.reset();
    if (window) glfwDestroyWindow(window);
    if (glfwStarted) glfwTerminate();
    EXPECT_EQ(validationErrors.load(), 0u);
  }

  std::vector<glm::vec4> run(std::span<const material::MaterialSamplerKey> keys,
                            std::span<const glm::vec4> queries, uint32_t levels = 3) {
    const auto plan = material::PlanMaterialSamplerSlots(keys, capacity);
    std::vector<vk::raii::Sampler> samplers;
    for (auto key : plan.keys) {
      const auto info = material::MakeMaterialSamplerCreateInfo(key);
      samplers.emplace_back(device->raii(), *reinterpret_cast<const vk::SamplerCreateInfo*>(&info));
    }
    std::vector<GpuTextureMetadata> metadata;
    for (auto key : keys) metadata.push_back({plan.slot(key), static_cast<uint32_t>(plan.keys.size())});
    const auto hostBuffer = [&](const void* data, size_t size) {
      auto result = memory->createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
          VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
      if (data) std::memcpy(result.allocation_info.pMappedData, data, size);
      if (data && vmaFlushAllocation(memory->allocator(), result.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
        throw std::runtime_error("Could not flush sampler query buffer");
      return result;
    };
    auto queryBuffer = hostBuffer(queries.data(), queries.size_bytes());
    auto resultBuffer = hostBuffer(nullptr, queries.size_bytes());
    auto metadataBuffer = hostBuffer(metadata.data(), metadata.size() * sizeof(GpuTextureMetadata));
    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eSampler, capacity, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(5, vk::DescriptorType::eSampledImage, static_cast<uint32_t>(keys.size()), vk::ShaderStageFlagBits::eCompute)};
    vk::raii::DescriptorSetLayout layout(device->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 3),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampler, capacity),
        vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, static_cast<uint32_t>(keys.size()))};
    vk::raii::DescriptorPool descriptors(device->raii(), vk::DescriptorPoolCreateInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, sizes));
    const std::array rawLayouts{*layout};
    auto sets = device->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors, rawLayouts));
    const std::array buffers{vk::DescriptorBufferInfo(queryBuffer.buffer, 0, VK_WHOLE_SIZE),
        vk::DescriptorBufferInfo(resultBuffer.buffer, 0, VK_WHOLE_SIZE), vk::DescriptorBufferInfo(metadataBuffer.buffer, 0, VK_WHOLE_SIZE)};
    std::vector<vk::DescriptorImageInfo> samplerInfos(capacity);
    for (uint32_t i = 0; i < capacity; ++i) samplerInfos[i].sampler = *samplers[i < samplers.size() ? i : 0];
    std::vector<vk::DescriptorImageInfo> images(keys.size(), vk::DescriptorImageInfo({}, *views[levels == 1 ? 1 : 0], vk::ImageLayout::eShaderReadOnlyOptimal));
    const std::array writes{
        vk::WriteDescriptorSet(*sets[0], 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers[0]),
        vk::WriteDescriptorSet(*sets[0], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers[1]),
        vk::WriteDescriptorSet(*sets[0], 2, 0, capacity, vk::DescriptorType::eSampler, samplerInfos.data()),
        vk::WriteDescriptorSet(*sets[0], 4, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffers[2]),
        vk::WriteDescriptorSet(*sets[0], 5, 0, static_cast<uint32_t>(images.size()), vk::DescriptorType::eSampledImage, images.data())};
    device->raii().updateDescriptorSets(writes, {});
    vk::raii::PipelineLayout pipelineLayout(device->raii(), vk::PipelineLayoutCreateInfo({}, rawLayouts));
    const auto code = container::util::readFile(shaderPath);
    vk::raii::ShaderModule module(device->raii(), vk::ShaderModuleCreateInfo({}, code.size(), reinterpret_cast<const uint32_t*>(code.data())));
    vk::raii::Pipeline pipeline(device->raii(), nullptr, vk::ComputePipelineCreateInfo({},
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, *module, "computeMain"), *pipelineLayout));
    pool.reset();
    command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    const std::array rawSets{*sets[0]};
    command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipelineLayout, 0, rawSets, {});
    command.dispatch(static_cast<uint32_t>((queries.size() + 31) / 32), 1, 1);
    const vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
    command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost, {}, host, {}, {});
    submit();
    if (vmaInvalidateAllocation(memory->allocator(), resultBuffer.allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Could not invalidate sampler results");
    std::vector<glm::vec4> result(queries.size());
    std::memcpy(result.data(), resultBuffer.allocation_info.pMappedData, queries.size_bytes());
    // Descriptor sets cease referencing buffers before those buffers retire.
    pipeline.clear(); sets.clear(); descriptors.clear();
    memory->destroyBuffer(queryBuffer); memory->destroyBuffer(resultBuffer); memory->destroyBuffer(metadataBuffer);
    return result;
  }

  void compare(std::span<const material::MaterialSamplerKey> keys,
                std::span<const glm::vec4> queries, uint32_t levels = 3) {
    const auto actual = run(keys, queries, levels);
    for (size_t i = 0; i < actual.size(); ++i) {
      SCOPED_TRACE(i);
      const auto expected = expectedSample(keys[size_t(queries[i].w)], queries[i], levels);
      for (int c = 0; c < 4; ++c) EXPECT_NEAR(actual[i][c], expected[c], 2e-4f);
    }
  }

  std::filesystem::path shaderPath;
  bool glfwStarted{false}; GLFWwindow* window{nullptr};
  std::unique_ptr<VulkanInstance> instance;
  vk::raii::DebugUtilsMessengerEXT messenger{nullptr}; vk::raii::SurfaceKHR surface{nullptr};
  std::shared_ptr<VulkanDevice> device; std::unique_ptr<VulkanMemoryManager> memory;
  vk::raii::CommandPool pool{nullptr}; vk::raii::CommandBuffer command{nullptr};
  VkImage image{}; VmaAllocation imageMemory{}; std::vector<vk::raii::ImageView> views;
  uint32_t capacity{}; std::atomic<uint32_t> validationErrors{0};
};

TEST_F(MaterialSamplerGpu, AllMinMagModesSampleSharedImageThroughDenseNonuniformSlots) {
  for (int s : {10497, 33071, 33648})
    for (int t : {10497, 33071, 33648})
      for (int min : {9728, 9729, 9984, 9985, 9986, 9987}) {
        const std::array keys{material::NormalizeMaterialSamplerKey(min, 9728, s, t),
                              material::NormalizeMaterialSamplerKey(min, 9729, s, t)};
        std::vector<glm::vec4> queries;
        for (auto uv : {glm::vec2(.5f), glm::vec2(-.125f, 1.125f), glm::vec2(1.25f, -.25f)})
          for (float lod : {-1.0f, .25f, 1.25f, 2.0f})
            for (uint32_t index = 0; index < 2; ++index) queries.emplace_back(uv, lod, float(index));
        compare(keys, queries);
      }
}

TEST_F(MaterialSamplerGpu, WrapModesApplyPerAxisAtTexelAndLinearMipBoundaries) {
  for (int s : {10497, 33071, 33648})
    for (int t : {10497, 33071, 33648}) {
      const std::array keys{material::NormalizeMaterialSamplerKey(9984, 9728, s, t),
                            material::NormalizeMaterialSamplerKey(9987, 9729, s, t)};
      std::vector<glm::vec4> queries;
      for (auto uv : {glm::vec2(-.125f, 1.125f), glm::vec2(1.25f, -.25f), glm::vec2(.0f, 1.0f)})
        for (uint32_t index = 0; index < 2; ++index) queries.emplace_back(uv, .25f, float(index));
      compare(keys, queries);
    }
}

TEST_F(MaterialSamplerGpu, AOneLevelImageRetainsAuthoredMinificationFilter) {
  const std::array keys{material::NormalizeMaterialSamplerKey(9984, 9729),
                        material::NormalizeMaterialSamplerKey(9987, 9728)};
  const std::array queries{glm::vec4(.5f, .5f, 2.0f, 0), glm::vec4(.5f, .5f, 2.0f, 1)};
  compare(keys, queries, 1);
}

TEST_F(MaterialSamplerGpu, DensePoliciesBeyondTheLegacyWrapPrefixReachTheirOwnSampler) {
  std::vector<material::MaterialSamplerKey> keys;
  const auto count=std::min(capacity,12u);
  for(uint32_t i=0;i<count;++i) keys.push_back({i*9});
  std::vector<glm::vec4> queries;
  for(uint32_t i=0;i<count;++i)
    for(float lod:{-1.f,.25f,1.25f,2.f}) queries.emplace_back(.5f,.5f,lod,float(i));
  compare(keys,queries);
}

TEST_F(MaterialSamplerGpu, TexturePresenceKeepsZeroBlackAndHdrFactorsWithoutPromotion) {
  const std::array keys{material::MaterialSamplerKey{0}};
  const std::array queries{glm::vec4(0,0,0,-1),glm::vec4(0,0,0,-2),
      glm::vec4(.25f,.5f,2,-1),glm::vec4(-.5f,.5f,2,-1)};
  const auto values=run(keys,queries);ASSERT_EQ(values.size(),4u);
  EXPECT_EQ(values[0],glm::vec4(0));EXPECT_EQ(values[1],glm::vec4(0));
  EXPECT_EQ(values[2],glm::vec4(.25f,.25f,.5f,2));EXPECT_EQ(values[3],glm::vec4(0,0,.5f,2));
}

}  // namespace
