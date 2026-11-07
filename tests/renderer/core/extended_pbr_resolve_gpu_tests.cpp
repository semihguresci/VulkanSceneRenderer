#include <Container/common/CommonGLFW.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanMemoryManager.h>

#include <gtest/gtest.h>
#include <glm/vec4.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef CONTAINER_EXTENDED_PBR_SHADER_DIR
#define CONTAINER_EXTENDED_PBR_SHADER_DIR "."
#endif

namespace {
using namespace container::gpu;
constexpr VkFormat kStorageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkSampleCountFlagBits kSamples = VK_SAMPLE_COUNT_4_BIT;
constexpr double kHalfTolerance = 5e-4;
constexpr double kDecodedTolerance = 2e-3;

struct Draw {
  glm::vec4 colorAndFactor;
  VkSampleMask mask;
};
struct Case {
  std::string name;
  std::vector<Draw> draws;
};
struct alignas(16) ResolveResult {
  glm::vec4 storage{}, decodedColor{}, reweighted{};
};
static_assert(sizeof(ResolveResult) == 48);

std::vector<Case> cases() {
  return {
      {"uncovered-all-samples", {}},
      {"ordinary-material-all-samples", {{{1, 1, 1, 0}, 0xf}}},
      {"half-red-with-uncovered-samples", {{{1, 0, 0, 1}, 0x3}}},
      {"half-red-with-ordinary-neighbor", {{{1, 0, 0, 1}, 0x3}, {{1, 1, 1, 0}, 0xc}}},
      {"ordinary-last-owner-overwrites-half", {{{1, 0, 0, 1}, 0xf}, {{1, 1, 1, 0}, 0x3}}},
      {"mixed-colors-and-factors", {{{1, 0, 0, 1}, 0x3}, {{0, 1, 0, 0.5f}, 0xc}}},
      {"black-with-full-diffuse-factor", {{{0, 0, 0, 1}, 0xf}}},
      {"nonblack-with-zero-factor", {{{0.2f, 0.7f, 0.9f, 0}, 0xf}}},
      {"quarter-coverage-with-quarter-factor", {{{0.5f, 0.25f, 1, 0.25f}, 0x1}}},
      {"mixed-colored-and-black-transmission", {{{1, 0, 0, 0.25f}, 0x3}, {{0, 0, 0, 1}, 0xc}}},
      {"nonbinary-half-float-rounding", {{{0.2f, 0.7f, 0.9f, 0.3f}, 0x1}}},
  };
}

std::array<double, 4> expectedStorage(const Case& test) {
  // Track sample ownership in draw order, independently of the production
  // encoder. Overwrites are replacements, not additive coverage contributions.
  std::array<std::array<double, 4>, 4> samples{};
  for (const auto& draw : test.draws) {
    for (uint32_t sample = 0; sample < 4; ++sample) {
      if ((draw.mask & (1u << sample)) == 0) continue;
      const double factor = draw.colorAndFactor.w;
      for (int channel = 0; channel < 3; ++channel)
        samples[sample][channel] = double(draw.colorAndFactor[channel]) * factor;
      samples[sample][3] = factor;
    }
  }
  std::array<double, 4> result{};
  for (const auto& sample : samples)
    for (int channel = 0; channel < 4; ++channel) result[channel] += sample[channel] / 4.0;
  return result;
}

class Attachment {
 public:
  Attachment(VulkanMemoryManager& memory, VkDevice device, VkExtent2D extent,
              VkSampleCountFlagBits samples, VkImageUsageFlags usage,
              VkFormat format = kStorageFormat,
              VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT)
      : memory_(memory), device_(device) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(memory_.allocator(), &info, &allocation, &image_, &allocation_, nullptr) != VK_SUCCESS)
      throw std::runtime_error("Could not allocate extended-PBR resolve attachment");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspects, 0, 1, 0, 1};
    // Register with the same metadata/transition path as production attachments.
    if (createVulkanImageView(device_, &view, nullptr, &view_) != VK_SUCCESS) {
      vmaDestroyImage(memory_.allocator(), image_, allocation_);
      throw std::runtime_error("Could not create extended-PBR resolve image view");
    }
  }
  ~Attachment() {
    destroyVulkanImageView(device_, view_, nullptr);
    vmaDestroyImage(memory_.allocator(), image_, allocation_);
  }
  Attachment(const Attachment&) = delete;
  Attachment& operator=(const Attachment&) = delete;
  [[nodiscard]] VkImage image() const { return image_; }
  [[nodiscard]] VkImageView view() const { return view_; }

 private:
  VulkanMemoryManager& memory_;
  VkDevice device_{};
  VkImage image_{};
  VmaAllocation allocation_{};
  VkImageView view_{};
};

class Readback {
 public:
  Readback(VulkanMemoryManager& memory, size_t bytes,
           VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) : memory_(memory) {
    buffer_ = memory_.createBuffer(bytes, usage,
        VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
  ~Readback() { memory_.destroyBuffer(buffer_); }
  Readback(const Readback&) = delete;
  Readback& operator=(const Readback&) = delete;
  [[nodiscard]] const AllocatedBuffer& buffer() const { return buffer_; }

 private:
  VulkanMemoryManager& memory_;
  AllocatedBuffer buffer_{};
};

struct RenderingScopeResources {
  VkDevice device{};
  RenderingPassHandle pass{};
  RenderingTargetHandle target{};
  ~RenderingScopeResources() {
    if (target) destroyRenderingTarget(device, target, nullptr);
    if (pass) destroyRenderingPass(device, pass, nullptr);
  }
};

class ExtendedPbrResolveGpu : public ::testing::Test {
 protected:
  static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
      ++static_cast<ExtendedPbrResolveGpu*>(user)->validationErrors_;
      std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
  }

  void SetUp() override {
    const char* enabled = std::getenv("CONTAINER_RUN_GPU_EXTENDED_PBR");
    if (!enabled || std::strcmp(enabled, "1") != 0)
      GTEST_SKIP() << "Set CONTAINER_RUN_GPU_EXTENDED_PBR=1 for actual four-sample resolve checks";
    const char* override = std::getenv("CONTAINER_EXTENDED_PBR_SHADER_DIR");
    shaderDirectory_ = std::filesystem::path(override ? override : CONTAINER_EXTENDED_PBR_SHADER_DIR) / "spv_shaders";
    for (const auto* stage : {"vert", "frag", "comp"})
      ASSERT_TRUE(std::filesystem::is_regular_file(shaderPath(stage))) << shaderPath(stage);
    ASSERT_EQ(glfwInit(), GLFW_TRUE);
    glfwStarted_ = true;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(32, 32, "Extended PBR four-sample resolve contract", nullptr, nullptr);
    ASSERT_NE(window_, nullptr);
    uint32_t count{};
    const auto* extensions = glfwGetRequiredInstanceExtensions(&count);
    ASSERT_NE(extensions, nullptr);
    InstanceCreateInfo create;
    create.enableValidationLayers = true;
    create.validationLayers = {"VK_LAYER_KHRONOS_validation"};
    create.requiredExtensions.assign(extensions, extensions + count);
    create.requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const auto synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &synchronization;
    create.next = &validation;
    instance_ = std::make_unique<VulkanInstance>(create);
    vk::DebugUtilsMessengerCreateInfoEXT debug;
    debug.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    debug.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
        vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debug.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(callback);
    debug.pUserData = this;
    messenger_ = vk::raii::DebugUtilsMessengerEXT(instance_->raii(), debug);
    VkSurfaceKHR handle{};
    ASSERT_EQ(glfwCreateWindowSurface(instance_->instance(), window_, nullptr, &handle), VK_SUCCESS);
    surface_ = vk::raii::SurfaceKHR(instance_->raii(), handle);
    DeviceCreateInfo info;
    info.requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    v11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.pNext = &v11;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    v13.pNext = &v12;
    // Device suitability checks support, while next is the actual enable chain.
    // The production raster/transition helpers need both Vulkan 1.3 features;
    // Slang's shared vertex compilation also declares DrawParameters.
    info.next = &v13;
    device_ = std::make_shared<VulkanDevice>(*instance_, handle, info);
    for (const auto samples : {VK_SAMPLE_COUNT_1_BIT, kSamples}) {
      const auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
          (samples == VK_SAMPLE_COUNT_1_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u);
      VkImageFormatProperties support{};
      ASSERT_EQ(vkGetPhysicalDeviceImageFormatProperties(device_->physicalDevice(), kStorageFormat,
          VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &support), VK_SUCCESS);
      if ((support.sampleCounts & samples) == 0)
        GTEST_SKIP() << "RGBA16F attachment does not support requested sample count " << samples;
    }
    memory_ = std::make_unique<VulkanMemoryManager>(instance_->instance(), device_->physicalDevice(), device_->device());
    pipelines_ = std::make_unique<PipelineManager>(device_->device());
    pool_ = vk::raii::CommandPool(device_->raii(), vk::CommandPoolCreateInfo({},
        device_->queueFamilyIndices().graphicsFamily.value()));
    auto commands = device_->raii().allocateCommandBuffers(
        vk::CommandBufferAllocateInfo(*pool_, vk::CommandBufferLevel::ePrimary, 1));
    command_ = std::move(commands.front());
  }

  void TearDown() override {
    if (device_) device_->raii().waitIdle();
    command_.clear();
    pool_.clear();
    pipelines_.reset();
    memory_.reset();
    device_.reset();
    surface_.clear();
    messenger_.clear();
    instance_.reset();
    if (window_) glfwDestroyWindow(window_);
    if (glfwStarted_) glfwTerminate();
    EXPECT_EQ(validationErrors_.load(), 0u) << "Vulkan validation and synchronization errors";
  }

  std::filesystem::path shaderPath(const char* stage) const {
    return shaderDirectory_ / (std::string("extended_pbr_resolve_contract.") + stage + ".spv");
  }

  bool supportsDepthFormat(VkFormat format) const {
    VkImageFormatProperties support{};
    return vkGetPhysicalDeviceImageFormatProperties(device_->physicalDevice(), format,
        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        0, &support) == VK_SUCCESS && (support.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
  }

  void verifyReadOnlyDepthStores(VkFormat format, VkAttachmentStoreOp depthStore,
                                VkAttachmentStoreOp stencilStore) {
    const bool stencil = format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT |
        (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    constexpr VkExtent2D extent{1, 1};
    Attachment depth(*memory_, device_->device(), extent, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        format, aspects);
    Readback readback(*memory_, 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto makeScope = [&](VkImageLayout initial, VkImageLayout inside,
                               VkImageLayout final, VkAttachmentLoadOp load,
                               VkAttachmentStoreOp store, VkAttachmentStoreOp stencilOp) {
      auto resources = std::make_unique<RenderingScopeResources>();
      resources->device = device_->device();
      VkAttachmentDescription attachment{};
      attachment.format = format;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = attachment.stencilLoadOp = load;
      attachment.storeOp = store;
      attachment.stencilStoreOp = stencilOp;
      attachment.initialLayout = initial;
      attachment.finalLayout = final;
      const VkAttachmentReference reference{0, inside};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.pDepthStencilAttachment = &reference;
      VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      pass.attachmentCount = pass.subpassCount = 1;
      pass.pAttachments = &attachment;
      pass.pSubpasses = &subpass;
      if (createRenderingPass(device_->device(), &pass, nullptr, &resources->pass) != VK_SUCCESS)
        throw std::runtime_error("Could not create depth store regression scope");
      const VkImageView view = depth.view();
      RenderingTargetCreateInfo target;
      target.renderPass = resources->pass;
      target.attachmentCount = 1;
      target.pAttachments = &view;
      target.width = target.height = 1;
      if (createRenderingTarget(device_->device(), &target, nullptr, &resources->target) != VK_SUCCESS)
        throw std::runtime_error("Could not create depth store regression target");
      return resources;
    };
    const auto initialize = makeScope(VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_STORE);
    const auto readOnly = makeScope(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
        depthStore, stencilStore);
    const auto prepareCopy = makeScope(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_STORE);
    pool_.reset();
    command_.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    const VkClearValue clear{.depthStencil = {0.625f, 77}};
    for (const auto* resources : {initialize.get(), readOnly.get(), prepareCopy.get()}) {
      RenderingBeginInfo begin;
      begin.renderPass = resources->pass;
      begin.framebuffer = resources->target;
      begin.renderArea = {{0, 0}, extent};
      begin.clearValueCount = 1;
      begin.pClearValues = &clear;
      beginDynamicRendering(*command_, &begin, VK_SUBPASS_CONTENTS_INLINE);
      // No draw is necessary: STORE and DONT_CARE themselves write at
      // vkCmdEndRendering, even with a read-only attachment layout.
      endDynamicRendering(*command_);
    }
    std::array<VkBufferImageCopy, 2> copies{};
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copies[0].imageExtent = {1, 1, 1};
    copies[1].bufferOffset = 4;
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    copies[1].imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(*command_, depth.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer().buffer, stencil ? 2u : 1u, copies.data());
    const vk::MemoryBarrier host(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead);
    command_.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            {}, host, {}, {});
    command_.end();
    const vk::CommandBuffer raw = *command_;
    vk::SubmitInfo submission;
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &raw;
    vk::raii::Queue queue(device_->raii(), device_->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submission, nullptr);
    queue.waitIdle();
    ASSERT_EQ(vmaInvalidateAllocation(memory_->allocator(), readback.buffer().allocation,
                                     0, VK_WHOLE_SIZE), VK_SUCCESS);
    const auto* data = static_cast<const std::byte*>(readback.buffer().allocation_info.pMappedData);
    ASSERT_NE(data, nullptr);
    if (depthStore != VK_ATTACHMENT_STORE_OP_DONT_CARE) {
      float actual{};
      std::memcpy(&actual, data, sizeof(actual));
      EXPECT_EQ(actual, 0.625f);
    }
    if (stencil && stencilStore != VK_ATTACHMENT_STORE_OP_DONT_CARE)
      EXPECT_EQ(std::to_integer<uint32_t>(data[4]), 77u);
    EXPECT_EQ(validationErrors_.load(), 0u);
  }

  std::vector<ResolveResult> resolve(const std::vector<Case>& tests) {
    const VkExtent2D extent{static_cast<uint32_t>(tests.size()), 1};
    Attachment multisampled(*memory_, device_->device(), extent, kSamples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    Attachment resolved(*memory_, device_->device(), extent, VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    Readback readback(*memory_, tests.size() * sizeof(ResolveResult));
    RenderingScopeResources rendering{device_->device()};
    std::array<VkAttachmentDescription, 2> attachments{};
    for (auto& attachment : attachments) {
      attachment.format = kStorageFormat;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    attachments[0].samples = kSamples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference resolveColor{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pResolveAttachments = &resolveColor;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    passInfo.pAttachments = attachments.data();
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    if (createRenderingPass(device_->device(), &passInfo, nullptr, &rendering.pass) != VK_SUCCESS)
      throw std::runtime_error("Could not create production dynamic-rendering resolve description");
    const std::array views{multisampled.view(), resolved.view()};
    RenderingTargetCreateInfo target;
    target.renderPass = rendering.pass;
    target.attachmentCount = static_cast<uint32_t>(views.size());
    target.pAttachments = views.data();
    target.width = extent.width;
    target.height = extent.height;
    target.layers = 1;
    if (createRenderingTarget(device_->device(), &target, nullptr, &rendering.target) != VK_SUCCESS)
      throw std::runtime_error("Could not create resolve rendering target");

    const auto vertexCode = container::util::readFile(shaderPath("vert"));
    const auto fragmentCode = container::util::readFile(shaderPath("frag"));
    const auto computeCode = container::util::readFile(shaderPath("comp"));
    vk::raii::ShaderModule vertex(device_->raii(), vk::ShaderModuleCreateInfo({}, vertexCode.size(),
        reinterpret_cast<const uint32_t*>(vertexCode.data())));
    vk::raii::ShaderModule fragment(device_->raii(), vk::ShaderModuleCreateInfo({}, fragmentCode.size(),
        reinterpret_cast<const uint32_t*>(fragmentCode.data())));
    vk::raii::ShaderModule compute(device_->raii(), vk::ShaderModuleCreateInfo({}, computeCode.size(),
        reinterpret_cast<const uint32_t*>(computeCode.data())));
    const VkPushConstantRange pushRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4)};
    const VkPipelineLayout rasterLayout = pipelines_->createPipelineLayout({}, {pushRange});
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = *vertex;
    stages[0].pName = "vertMain";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = *fragment;
    stages[1].pName = "fragMain";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = kSamples;
    VkPipelineColorBlendAttachmentState colorBlend{};
    colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &colorBlend;
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    RenderingGraphicsPipelineCreateInfo graphics{};
    graphics.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    graphics.stageCount = static_cast<uint32_t>(stages.size());
    graphics.pStages = stages.data();
    graphics.pVertexInputState = &vertexInput;
    graphics.pInputAssemblyState = &assembly;
    graphics.pViewportState = &viewport;
    graphics.pRasterizationState = &raster;
    graphics.pMultisampleState = &samples;
    graphics.pColorBlendState = &blending;
    graphics.pDynamicState = &dynamic;
    graphics.layout = rasterLayout;
    graphics.renderPass = rendering.pass;
    std::set<VkSampleMask> masks;
    for (const auto& test : tests)
      for (const auto& draw : test.draws) masks.insert(draw.mask);
    std::map<VkSampleMask, VkPipeline> rasterPipelines;
    for (const auto mask : masks) {
      samples.pSampleMask = &mask;
      rasterPipelines[mask] = pipelines_->createGraphicsPipeline(graphics,
          "extended_pbr_resolve_mask_" + std::to_string(mask));
    }

    const std::array bindings{
        vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute),
        vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute)};
    vk::raii::DescriptorSetLayout decodeLayout(device_->raii(), vk::DescriptorSetLayoutCreateInfo({}, bindings));
    const std::array poolSizes{vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, 1),
                               vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, 1)};
    vk::raii::DescriptorPool descriptors(device_->raii(), vk::DescriptorPoolCreateInfo(
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, poolSizes));
    const std::array rawLayouts{*decodeLayout};
    auto sets = device_->raii().allocateDescriptorSets(vk::DescriptorSetAllocateInfo(*descriptors, rawLayouts));
    const vk::DescriptorImageInfo image({}, resolved.view(), vk::ImageLayout::eShaderReadOnlyOptimal);
    const vk::DescriptorBufferInfo buffer(readback.buffer().buffer, 0, VK_WHOLE_SIZE);
    const std::array writes{
        vk::WriteDescriptorSet(*sets[0], 0, 0, 1, vk::DescriptorType::eSampledImage, &image),
        vk::WriteDescriptorSet(*sets[0], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buffer)};
    device_->raii().updateDescriptorSets(writes, {});
    vk::raii::PipelineLayout computeLayout(device_->raii(), vk::PipelineLayoutCreateInfo({}, rawLayouts));
    vk::raii::Pipeline decodePipeline(device_->raii(), nullptr, vk::ComputePipelineCreateInfo({},
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eCompute, *compute, "computeMain"),
        *computeLayout));

    pool_.reset();
    command_.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    const VkClearValue clear{};
    RenderingBeginInfo begin;
    begin.renderPass = rendering.pass;
    begin.framebuffer = rendering.target;
    begin.renderArea = {{0, 0}, extent};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    beginDynamicRendering(*command_, &begin, VK_SUBPASS_CONTENTS_INLINE);
    // Match the scene negative-height viewport; coverage comes only from the
    // fixed sample mask, without assumptions about device sample positions.
    const VkViewport sceneViewport{0, 1, float(extent.width), -1, 0, 1};
    vkCmdSetViewport(*command_, 0, 1, &sceneViewport);
    for (uint32_t index = 0; index < tests.size(); ++index) {
      const VkRect2D scissor{{static_cast<int32_t>(index), 0}, {1, 1}};
      vkCmdSetScissor(*command_, 0, 1, &scissor);
      for (const auto& draw : tests[index].draws) {
        vkCmdBindPipeline(*command_, VK_PIPELINE_BIND_POINT_GRAPHICS, rasterPipelines.at(draw.mask));
        vkCmdPushConstants(*command_, rasterLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(glm::vec4), &draw.colorAndFactor);
        vkCmdDraw(*command_, 3, 1, 0, 0);
      }
    }
    // The production end transition orders actual color resolve writes before
    // the compute shader's sampled-image reads; no test-only image barrier.
    endDynamicRendering(*command_);
    command_.bindPipeline(vk::PipelineBindPoint::eCompute, *decodePipeline);
    const std::array rawSets{*sets[0]};
    command_.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *computeLayout, 0, rawSets, {});
    command_.dispatch(static_cast<uint32_t>((tests.size() + 31) / 32), 1, 1);
    const vk::MemoryBarrier host(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
    command_.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                             {}, host, {}, {});
    command_.end();
    const vk::CommandBuffer raw = *command_;
    vk::SubmitInfo submission;
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &raw;
    vk::raii::Queue queue(device_->raii(), device_->queueFamilyIndices().graphicsFamily.value(), 0);
    queue.submit(submission, nullptr);
    queue.waitIdle();
    if (vmaInvalidateAllocation(memory_->allocator(), readback.buffer().allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS)
      throw std::runtime_error("Could not invalidate resolved extended-PBR readback");
    if (!readback.buffer().allocation_info.pMappedData)
      throw std::runtime_error("Resolved extended-PBR readback is not mapped");
    std::vector<ResolveResult> output(tests.size());
    std::memcpy(output.data(), readback.buffer().allocation_info.pMappedData,
                output.size() * sizeof(ResolveResult));
    return output;
  }

  std::filesystem::path shaderDirectory_;
  bool glfwStarted_{};
  GLFWwindow* window_{};
  std::unique_ptr<VulkanInstance> instance_;
  vk::raii::DebugUtilsMessengerEXT messenger_{nullptr};
  vk::raii::SurfaceKHR surface_{nullptr};
  std::shared_ptr<VulkanDevice> device_;
  std::unique_ptr<VulkanMemoryManager> memory_;
  std::unique_ptr<PipelineManager> pipelines_;
  vk::raii::CommandPool pool_{nullptr};
  vk::raii::CommandBuffer command_{nullptr};
  std::atomic<uint32_t> validationErrors_{};
};

TEST_F(ExtendedPbrResolveGpu, ActualFourSampleResolvePreservesTransportEnergyAndLastSampleOwners) {
  const auto tests = cases();
  const auto output = resolve(tests);
  ASSERT_EQ(output.size(), tests.size());
  for (size_t index = 0; index < tests.size(); ++index) {
    SCOPED_TRACE(tests[index].name);
    const auto expected = expectedStorage(tests[index]);
    const auto& actual = output[index];
    for (int channel = 0; channel < 4; ++channel) {
      ASSERT_TRUE(std::isfinite(actual.storage[channel]));
      EXPECT_NEAR(double(actual.storage[channel]), expected[channel], kHalfTolerance);
    }
    for (int channel = 0; channel < 3; ++channel) {
      const double decoded = expected[3] > 0 ? expected[channel] / expected[3] : 0.0;
      ASSERT_TRUE(std::isfinite(actual.decodedColor[channel]));
      ASSERT_TRUE(std::isfinite(actual.reweighted[channel]));
      EXPECT_NEAR(double(actual.decodedColor[channel]), decoded, kDecodedTolerance);
      EXPECT_NEAR(double(actual.reweighted[channel]), expected[channel], kHalfTolerance);
      // This directly detects the original coverage-squared dark-edge bug:
      // half red/d=1 must transport 0.5 red, rather than 0.25 red.
      if (tests[index].name == "half-red-with-uncovered-samples") {
        EXPECT_NEAR(actual.reweighted.x, 0.5f, kHalfTolerance);
        EXPECT_GT(actual.reweighted.x, 0.4f);
      }
      if (expected[3] == 0.0) {
        EXPECT_EQ(actual.storage[channel], 0.0f);
        EXPECT_EQ(actual.decodedColor[channel], 0.0f);
        EXPECT_EQ(actual.reweighted[channel], 0.0f);
      }
    }
    EXPECT_NEAR(double(actual.decodedColor.w), expected[3], kHalfTolerance);
    EXPECT_EQ(actual.reweighted.w, expected[3] > 0.0 ? 1.0f : 0.0f);
  }
}

TEST_F(ExtendedPbrResolveGpu, ReadOnlyDepthStoreOrdersImplicitWritesBeforeLayoutTransition) {
  if (!supportsDepthFormat(VK_FORMAT_D32_SFLOAT)) GTEST_SKIP() << "D32 depth transfer unsupported";
  verifyReadOnlyDepthStores(VK_FORMAT_D32_SFLOAT, VK_ATTACHMENT_STORE_OP_STORE,
                           VK_ATTACHMENT_STORE_OP_NONE);
}

TEST_F(ExtendedPbrResolveGpu, ReadOnlyDepthStencilStoreAndDiscardOrderImplicitWrites) {
  if (!supportsDepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT))
    GTEST_SKIP() << "D32/S8 depth/stencil transfer unsupported";
  for (const auto operations : {
           std::pair{VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_NONE},
           std::pair{VK_ATTACHMENT_STORE_OP_NONE, VK_ATTACHMENT_STORE_OP_STORE},
           std::pair{VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_STORE},
           std::pair{VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE},
           std::pair{VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_STORE_OP_DONT_CARE},
           std::pair{VK_ATTACHMENT_STORE_OP_NONE, VK_ATTACHMENT_STORE_OP_NONE}}) {
    SCOPED_TRACE(::testing::Message() << "depth=" << operations.first << " stencil=" << operations.second);
    verifyReadOnlyDepthStores(VK_FORMAT_D32_SFLOAT_S8_UINT, operations.first, operations.second);
  }
}

}  // namespace
