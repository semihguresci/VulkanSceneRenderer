#include "Container/renderer/pipeline/PreparedGraphicsPipelines.h"
#include <Container/renderer/scene/PbrAuxiliaryPass.h>

#include <Container/geometry/Vertex.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/FileLoader.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/ShaderModule.h>
#include <Container/utility/VulkanDevice.h>

#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace container::renderer {
namespace {
constexpr VkFormat kAuxiliaryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Frame slots are renderer resources, rather than source asset data. Bound
// accidental caller-sized metadata before allocating any Vulkan resources.
constexpr size_t kMaxFrameSlots = 64;

[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("PBR auxiliary pass: ") + message);
}

gpu::VulkanMemoryManager& requireMemory(gpu::AllocationManager& allocator) {
  auto* memory = allocator.memoryManager();
  if (!memory || !memory->allocator()) fail("allocator is not initialized");
  return *memory;
}

bool hasDepth(VkFormat format) {
  return format == VK_FORMAT_D16_UNORM ||
         format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
         format == VK_FORMAT_D32_SFLOAT ||
         format == VK_FORMAT_D16_UNORM_S8_UINT ||
         format == VK_FORMAT_D24_UNORM_S8_UINT ||
         format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

void requireImageSupport(VkPhysicalDevice physicalDevice, VkFormat format,
                         VkImageUsageFlags usage,
                         VkSampleCountFlagBits samples, VkExtent2D extent) {
  VkImageFormatProperties properties{};
  if (vkGetPhysicalDeviceImageFormatProperties(
          physicalDevice, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
          usage, 0, &properties) != VK_SUCCESS ||
      (properties.sampleCounts & samples) == 0 ||
      extent.width > properties.maxExtent.width ||
      extent.height > properties.maxExtent.height) {
    fail("requested attachment format, extent, or sample count is unsupported");
  }
}

struct ShaderModules {
  VkDevice device{VK_NULL_HANDLE};
  std::array<VkShaderModule, 2> modules{};
  ~ShaderModules() {
    for (const auto module : modules) {
      if (module) destroyOwnedShaderModule(device, module, nullptr);
    }
  }
};

VkAttachmentDescription colorAttachment(VkSampleCountFlagBits samples,
                                          bool resolve) {
  VkAttachmentDescription attachment{};
  attachment.format = kAuxiliaryFormat;
  attachment.samples = samples;
  attachment.loadOp = resolve ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                              : VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachment.finalLayout = samples == VK_SAMPLE_COUNT_1_BIT
      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
      : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  return attachment;
}
}  // namespace

struct PbrAuxiliaryPass::Impl {
  struct Attachment {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VmaAllocation allocation{};
  };
  struct Frame {
    std::array<Attachment, 2> outputs{};
    std::array<Attachment, 2> multisampled{};
    RenderingTargetHandle target{};
  };

  std::shared_ptr<gpu::VulkanDevice> device;
  gpu::VulkanMemoryManager& memory;
  gpu::PipelineManager& pipelines;
  std::vector<Frame> frames{};
  RenderingPassHandle pass{};
  VkPipelineLayout layout{VK_NULL_HANDLE};
  std::array<VkPipeline, 3> cullPipelines{};
  VkSampleCountFlagBits pipelineSamples{VK_SAMPLE_COUNT_1_BIT};
  VkExtent2D size{};
  VkCommandBuffer activeCommand{VK_NULL_HANDLE};

  Impl(std::shared_ptr<gpu::VulkanDevice> deviceIn,
        gpu::VulkanMemoryManager& memoryIn, gpu::PipelineManager& pipelinesIn)
      : device(std::move(deviceIn)), memory(memoryIn), pipelines(pipelinesIn) {
    if (!device || device->device() == VK_NULL_HANDLE) fail("device is null");
  }

  ~Impl() {
    // The caller retires GPU readers and closes rendering before destruction.
    for (auto& pipeline : cullPipelines) pipelines.destroyPipeline(pipeline);
    pipelines.destroyPipelineLayout(layout);
    for (auto& frame : frames) {
      if (frame.target) {
        destroyRenderingTarget(device->device(), frame.target, nullptr);
      }
      for (auto& attachment : frame.outputs) destroyAttachment(attachment);
      for (auto& attachment : frame.multisampled) destroyAttachment(attachment);
    }
    if (pass) destroyRenderingPass(device->device(), pass, nullptr);
  }

  void destroyAttachment(Attachment& attachment) {
    if (attachment.view) {
      destroyVulkanImageView(device->device(), attachment.view, nullptr);
    }
    if (attachment.image) {
      vmaDestroyImage(memory.allocator(), attachment.image,
                      attachment.allocation);
    }
    attachment = {};
  }

  void createAttachment(Attachment& attachment,
                         VkSampleCountFlagBits samples) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kAuxiliaryFormat;
    imageInfo.extent = {size.width, size.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = samples;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (samples == VK_SAMPLE_COUNT_1_BIT) {
      imageInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(memory.allocator(), &imageInfo, &allocationInfo,
                       &attachment.image, &attachment.allocation, nullptr) !=
        VK_SUCCESS) {
      fail("failed to allocate auxiliary color image");
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = attachment.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kAuxiliaryFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Register views with DynamicRendering's transition metadata, just like
    // FrameResourceManager attachments; raw vkCreateImageView bypasses it.
    if (createVulkanImageView(device->device(), &viewInfo, nullptr,
                              &attachment.view) != VK_SUCCESS) {
      fail("failed to create auxiliary color image view");
    }
  }

  void createPass(VkFormat depthFormat, VkSampleCountFlagBits samples) {
    const bool multisampled = samples != VK_SAMPLE_COUNT_1_BIT;
    std::array<VkAttachmentDescription, 5> attachments{};
    attachments[0] = colorAttachment(samples, false);
    attachments[1] = colorAttachment(samples, false);
    auto& depth = attachments[2];
    depth.format = depthFormat;
    depth.samples = samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Preserve stencil too when the scene uses a combined depth/stencil image.
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[3] = colorAttachment(VK_SAMPLE_COUNT_1_BIT, true);
    attachments[4] = colorAttachment(VK_SAMPLE_COUNT_1_BIT, true);
    const std::array<VkAttachmentReference, 2> colors{{
        {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    const std::array<VkAttachmentReference, 2> resolves{{
        {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}}};
    const VkAttachmentReference depthReference{
        2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    subpass.pColorAttachments = colors.data();
    subpass.pResolveAttachments = multisampled ? resolves.data() : nullptr;
    subpass.pDepthStencilAttachment = &depthReference;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = multisampled ? 5u : 3u;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    // DynamicRendering emits synchronization2 barriers from these exact
    // attachment layouts, including prior sampled reads before color reuse.
    if (createRenderingPass(device->device(), &info, nullptr, &pass) !=
        VK_SUCCESS) {
      fail("failed to create auxiliary rendering description");
    }
  }

  PreparedGraphicsPipelines preparePipelines(const std::filesystem::path& shaderDir) {
    PreparedGraphicsPipelines candidate(pipelines);
    if (!layout || !pass)
      return candidate;
    ShaderModules modules{device->device()};
    modules.modules[0] = gpu::createShaderModule(device->device(), util::readFile(
        shaderDir / "spv_shaders/extended_material_aux.vert.spv"));
    modules.modules[1] = gpu::createShaderModule(device->device(), util::readFile(
        shaderDir / "spv_shaders/extended_material_aux.frag.spv"));
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (size_t index = 0; index < stages.size(); ++index) {
      stages[index].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[index].stage = index == 0 ? VK_SHADER_STAGE_VERTEX_BIT
                                        : VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[index].module = modules.modules[index];
      stages[index].pName = index == 0 ? "vertMain" : "fragMain";
    }
    const auto binding = geometry::Vertex::bindingDescription();
    const auto attributes = geometry::Vertex::attributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Parent sets the scene negative-height viewport. Keep the same glTF CCW
    // convention as the G-buffer; there is no projection or winding Y-flip.
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisampling{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = pipelineSamples;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    // Reverse-Z replay matches the G-buffer's tolerant prepass comparison.
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    std::array<VkPipelineColorBlendAttachmentState, 2> blendAttachments{};
    for (auto& attachment : blendAttachments) {
      attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
          VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
          VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();
    const std::array<VkDynamicState, 2> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    RenderingGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisampling;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = pass;
    const std::array<VkCullModeFlags, 3> culls{
        VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE};
    const std::array<const char*, 3> names{
        "pbr_auxiliary_back_cull", "pbr_auxiliary_front_cull",
        "pbr_auxiliary_no_cull"};
    for (size_t index = 0; index < culls.size(); ++index) {
      raster.cullMode = culls[index];
      candidate.handles[index] = pipelines.createGraphicsPipeline(info, names[index]);
    }
    return candidate;
  }

  void create(const std::filesystem::path& shaderDir,
               VkDescriptorSetLayout sceneLayout, VkFormat depthFormat,
               VkSampleCountFlagBits samples,
               std::span<const VkImageView> depthViews, VkExtent2D extent) {
    if (!sceneLayout) fail("scene descriptor layout is null");
    if (!hasDepth(depthFormat)) fail("depth format has no depth aspect");
    if (depthViews.empty() || depthViews.size() > kMaxFrameSlots) {
      fail("frame-slot count must be between one and 64");
    }
    for (const auto view : depthViews) {
      if (!view) fail("depth attachment view is null");
    }
    const auto sampleBits = static_cast<uint32_t>(samples);
    if (sampleBits == 0 || sampleBits > VK_SAMPLE_COUNT_64_BIT ||
        (sampleBits & (sampleBits - 1u)) != 0) {
      fail("sample count must be one supported Vulkan sample-count bit");
    }
    if (extent.width == 0 || extent.height == 0) fail("extent is empty");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device->physicalDevice(), &properties);
    if (extent.width > properties.limits.maxFramebufferWidth ||
        extent.height > properties.limits.maxFramebufferHeight ||
        properties.limits.maxColorAttachments < 2) {
      fail("framebuffer limits do not cover the requested auxiliary outputs");
    }
    if ((properties.limits.framebufferColorSampleCounts &
         properties.limits.framebufferDepthSampleCounts & sampleBits) == 0) {
      fail("framebuffer limits do not support the requested sample count");
    }
    const auto physical = device->physicalDevice();
    requireImageSupport(physical, kAuxiliaryFormat,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_SAMPLE_COUNT_1_BIT, extent);
    requireImageSupport(physical, depthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        samples, extent);
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
      requireImageSupport(physical, kAuxiliaryFormat,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, samples, extent);
    }
    size = extent;
    createPass(depthFormat, samples);
    const VkPushConstantRange pushConstants{
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(gpu::BindlessPushConstants)};
    layout = pipelines.createPipelineLayout({sceneLayout}, {pushConstants});
    pipelineSamples = samples;
    auto candidate = preparePipelines(shaderDir);
    cullPipelines.swap(candidate.handles);
    frames.resize(depthViews.size());
    for (size_t index = 0; index < frames.size(); ++index) {
      auto& frame = frames[index];
      for (auto& output : frame.outputs) {
        createAttachment(output, VK_SAMPLE_COUNT_1_BIT);
      }
      const bool multisampled = samples != VK_SAMPLE_COUNT_1_BIT;
      if (multisampled) {
        for (auto& color : frame.multisampled) createAttachment(color, samples);
      }
      const std::array<VkImageView, 5> views{
          multisampled ? frame.multisampled[0].view : frame.outputs[0].view,
          multisampled ? frame.multisampled[1].view : frame.outputs[1].view,
          depthViews[index], frame.outputs[0].view, frame.outputs[1].view};
      RenderingTargetCreateInfo targetInfo{};
      targetInfo.renderPass = pass;
      targetInfo.attachmentCount = multisampled ? 5u : 3u;
      targetInfo.pAttachments = views.data();
      targetInfo.width = size.width;
      targetInfo.height = size.height;
      if (createRenderingTarget(device->device(), &targetInfo, nullptr,
                                 &frame.target) != VK_SUCCESS) {
        fail("failed to create auxiliary rendering target");
      }
    }
  }
};

PbrAuxiliaryPass::PbrAuxiliaryPass(
    std::shared_ptr<gpu::VulkanDevice> device, gpu::AllocationManager& allocator,
    gpu::PipelineManager& pipelines)
    : impl_(std::make_unique<Impl>(std::move(device), requireMemory(allocator),
                                   pipelines)) {}

PbrAuxiliaryPass::~PbrAuxiliaryPass() = default;

void PbrAuxiliaryPass::createResources(
    const std::filesystem::path& shaderDir,
    VkDescriptorSetLayout sceneDescriptorLayout, VkFormat depthFormat,
    VkSampleCountFlagBits msaa, std::span<const VkImageView> depthViews,
    VkExtent2D extent) {
  if (impl_->activeCommand) fail("cannot recreate inside a rendering scope");
  // Keep the old pass intact when any allocation, module, or pipeline fails.
  auto replacement = std::make_unique<Impl>(impl_->device, impl_->memory,
                                            impl_->pipelines);
  replacement->create(shaderDir, sceneDescriptorLayout, depthFormat, msaa,
                       depthViews, extent);
  impl_.swap(replacement);
}

void PbrAuxiliaryPass::begin(VkCommandBuffer command, uint32_t frameIndex) {
  if (!ready()) fail("resources are not ready");
  if (!command) fail("command buffer is null");
  if (impl_->activeCommand) fail("a rendering scope is already active");
  const auto& frame = impl_->frames.at(frameIndex);
  const std::array<VkClearValue, 2> clears{};
  RenderingBeginInfo info{};
  info.renderPass = impl_->pass;
  info.framebuffer = frame.target;
  info.renderArea = {{0, 0}, impl_->size};
  info.clearValueCount = static_cast<uint32_t>(clears.size());
  info.pClearValues = clears.data();
  beginDynamicRendering(command, &info, VK_SUBPASS_CONTENTS_INLINE);
  impl_->activeCommand = command;
}

void PbrAuxiliaryPass::end(VkCommandBuffer command) {
  if (!command || command != impl_->activeCommand) {
    fail("end does not match the active command buffer");
  }
  endDynamicRendering(command);
  impl_->activeCommand = VK_NULL_HANDLE;
}

PreparedGraphicsPipelines PbrAuxiliaryPass::prepareShaderReload(const std::filesystem::path& root) {
  return impl_->preparePipelines(root);
}

void PbrAuxiliaryPass::publishShaderReload(PreparedGraphicsPipelines& candidate) noexcept {
  if (candidate.handles[0])
    impl_->cullPipelines.swap(candidate.handles);
}

VkPipelineLayout PbrAuxiliaryPass::pipelineLayout() const { return impl_->layout; }
VkPipeline PbrAuxiliaryPass::backCullPipeline() const {
  return impl_->cullPipelines[0];
}
VkPipeline PbrAuxiliaryPass::frontCullPipeline() const {
  return impl_->cullPipelines[1];
}
VkPipeline PbrAuxiliaryPass::noCullPipeline() const {
  return impl_->cullPipelines[2];
}
VkImageView PbrAuxiliaryPass::anisotropyView(uint32_t frameIndex) const {
  return impl_->frames.at(frameIndex).outputs[0].view;
}
VkImageView PbrAuxiliaryPass::diffuseTransmissionView(uint32_t frameIndex) const {
  return impl_->frames.at(frameIndex).outputs[1].view;
}
bool PbrAuxiliaryPass::ready() const {
  return impl_->pass && impl_->layout && impl_->cullPipelines[0] &&
         impl_->cullPipelines[1] && impl_->cullPipelines[2] &&
         !impl_->frames.empty();
}
size_t PbrAuxiliaryPass::frameCount() const { return impl_->frames.size(); }
VkExtent2D PbrAuxiliaryPass::extent() const { return impl_->size; }

}  // namespace container::renderer
