#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/renderer/pipeline/PreparedGraphicsPipelines.h"
#include "Container/renderer/temporal/TemporalVelocityVertexInput.h"
#include "Container/geometry/Vertex.h"
#include "Container/renderer/bim/BimSurfacePassRecorder.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/effects/OpenVdbVolumeRenderer.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"
#include "Container/renderer/scene/SceneViewport.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace container::renderer {
namespace {
struct ResolveConstants {
  uint32_t width, height, historyValid, orthographic;
  float historyWeight, absoluteTolerance, relativeTolerance, varianceGamma;
  float nearPlane, farPlane, previousNear, previousFar;
};
static_assert(sizeof(ResolveConstants) == 48);

constexpr std::array<VkDescriptorSetLayoutBinding, 8> composeBindings{{
    {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    {7, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
}};

struct TransparencyConstants {
  uint32_t width, height;
  float jitterX, jitterY;
};
static_assert(sizeof(TransparencyConstants) == 16);

void dependency(VkCommandBuffer cmd, VkPipelineStageFlags2 destination,
                VkAccessFlags2 access) {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask =
      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
  barrier.dstStageMask = destination;
  barrier.dstAccessMask = access;
  VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  info.memoryBarrierCount = 1;
  info.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &info);
}

void initializeImage(VkCommandBuffer cmd, VkImage image) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  info.imageMemoryBarrierCount = 1;
  info.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &info);
}

bool needed(const FrameRecordParams &frame) {
  const auto *temporal = frame.services.temporalManager;
  if (frame.runtime.activeTechnique == RenderTechniqueId::PathTracing || !temporal || !temporal->active())
    return false;
  const auto display = frame.debug.displayMode;
  if (temporal->upscalingActive())
    return !frame.camera.orthographic && (display == 0 || display == 100 || display == 104);
  return display == 0 || display == 8 || (display >= 100 && display <= 104);
}
} // namespace

TemporalManager::TemporalManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager &allocator,
    container::gpu::PipelineManager &pipelines)
    : device_(std::move(device)), allocator_(allocator), pipelines_(pipelines) {
}

TemporalManager::~TemporalManager() {
  destroyImages();
  destroyDlssCompositionPipelines();
  const VkDevice device = device_->device();
  for (auto pipeline : velocityPipelines_)
    if (pipeline)
      destroyOwnedPipeline(device, pipeline, nullptr);
  pipelines_.destroyPipeline(resolvePipeline_);
  pipelines_.destroyPipeline(composePipeline_);
  pipelines_.destroyPipeline(upscaleInputsPipeline_);
  pipelines_.destroyPipelineLayout(velocityLayout_);
  pipelines_.destroyPipelineLayout(resolveLayout_);
  pipelines_.destroyPipelineLayout(composeLayout_);
  pipelines_.destroyPipelineLayout(upscaleInputsLayout_);
  pipelines_.destroyDescriptorSetLayout(resolveSetLayout_);
  pipelines_.destroyDescriptorSetLayout(composeSetLayout_);
  pipelines_.destroyDescriptorSetLayout(upscaleInputsSetLayout_);
  if (sampler_)
    destroyOwnedSampler(device, sampler_, nullptr);
}

void TemporalManager::createDlssCompositionPipelines(const std::filesystem::path& root) {
  if (dlssComposePipeline_ && dlssTransparencyPipeline_) return;
  std::vector<VkDescriptorSetLayoutBinding> bindings(composeBindings.begin(), composeBindings.end());
  bindings.push_back({8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
  const std::vector<VkDescriptorSetLayoutBinding> layerBindings{
      {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  try {
    dlssComposeSetLayout_ = pipelines_.createDescriptorSetLayout(bindings,
        std::vector<VkDescriptorBindingFlags>(bindings.size()));
    dlssTransparencySetLayout_ = pipelines_.createDescriptorSetLayout(layerBindings, {0, 0, 0});
    dlssComposeLayout_ = pipelines_.createPipelineLayout({dlssComposeSetLayout_},
        {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}});
    dlssTransparencyLayout_ = pipelines_.createPipelineLayout({dlssTransparencySetLayout_},
        {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TransparencyConstants)}});
    dlssComposePipeline_ = pipelines_.createComputePipeline(root / "spv_shaders/dlss_compose.comp.spv",
        dlssComposeLayout_, "dlss.compose");
    dlssTransparencyPipeline_ = pipelines_.createComputePipeline(root / "spv_shaders/dlss_transparency.comp.spv",
        dlssTransparencyLayout_, "dlss.transparency");
  } catch (...) {
    destroyDlssCompositionPipelines();
    throw;
  }
}

void TemporalManager::destroyDlssCompositionPipelines() {
  pipelines_.destroyPipeline(dlssComposePipeline_);
  pipelines_.destroyPipeline(dlssTransparencyPipeline_);
  pipelines_.destroyPipelineLayout(dlssComposeLayout_);
  pipelines_.destroyPipelineLayout(dlssTransparencyLayout_);
  pipelines_.destroyDescriptorSetLayout(dlssComposeSetLayout_);
  pipelines_.destroyDescriptorSetLayout(dlssTransparencySetLayout_);
}

void TemporalManager::createUpscaleInputPipeline(const std::filesystem::path& root) {
  if (upscaleInputsPipeline_) return;
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for (uint32_t binding = 0; binding < 8; ++binding)
    bindings.push_back({binding, binding < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
  upscaleInputsSetLayout_ = pipelines_.createDescriptorSetLayout(bindings,
      std::vector<VkDescriptorBindingFlags>(bindings.size(), 0));
  try {
    upscaleInputsLayout_ = pipelines_.createPipelineLayout({upscaleInputsSetLayout_},
        {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t)}});
    upscaleInputsPipeline_ = pipelines_.createComputePipeline(root / "spv_shaders/fsr_inputs.comp.spv",
        upscaleInputsLayout_, "fsr.inputs");
  } catch (...) {
    pipelines_.destroyPipelineLayout(upscaleInputsLayout_);
    pipelines_.destroyDescriptorSetLayout(upscaleInputsSetLayout_);
    upscaleInputsLayout_ = {};
    upscaleInputsSetLayout_ = {};
    throw;
  }
}

bool TemporalManager::prepareFsr(const std::filesystem::path& root,
                                 const FsrUpscalerSettings& settings) {
  if (!FsrUpscaler::builtWithSdk() || !device_->fsrUpscalingEnabled()) {
    upscalingStatus_ = FsrUpscaler::builtWithSdk() ? "GPU lacks the required Vulkan FSR features"
                                            : "Built without the Vulkan FSR SDK";
    return false;
  }
  if (!inputs_.empty() || dlss_) {
    upscalingStatus_ = "Retire temporal frame resources before preparing FSR";
    return false;
  }
  try {
    auto candidate = std::make_unique<FsrUpscaler>(device_->physicalDevice(), device_->device(), root);
    if (!candidate->prepare(settings)) {
      upscalingStatus_ = candidate->status();
      return false;
    }
    createUpscaleInputPipeline(root);
    upscalingStatus_ = candidate->status();
    upscaleRenderExtent_ = settings.renderExtent;
    upscaleDisplayExtent_ = settings.displayExtent;
    fsr_ = std::move(candidate);
    return true;
  } catch (const std::exception& error) {
    upscalingStatus_ = error.what();
    return false;
  }
}

std::optional<VkExtent2D> TemporalManager::prepareDlss(const std::filesystem::path& root,
                                                      UpscaleMode mode, VkExtent2D display) {
  if (!DlssUpscaler::builtWithSdk()) {
    upscalingStatus_ = "Built without the Vulkan DLSS SDK";
    return std::nullopt;
  }
  if (!device_->dlssUpscalingEnabled()) {
    upscalingStatus_ = device_->dlssUpscalingStatus();
    return std::nullopt;
  }
  if (!inputs_.empty() || fsr_) {
    upscalingStatus_ = "Retire temporal frame resources before preparing DLSS";
    return std::nullopt;
  }
  if (!device_->enabledFeatures().shaderStorageImageExtendedFormats) {
    upscalingStatus_ = "GPU lacks the storage image formats required by DLSS inputs";
    return std::nullopt;
  }
  try {
    auto candidate = std::make_unique<DlssUpscaler>(device_->instance(), device_->physicalDevice(),
        device_->device(), device_->graphicsQueue(), device_->queueFamilyIndices().graphicsFamily.value(), root);
    if (!candidate->prepare(mode, display)) {
      upscalingStatus_ = candidate->status();
      return std::nullopt;
    }
    createUpscaleInputPipeline(root);
    createDlssCompositionPipelines(root);
    upscalingStatus_ = candidate->status();
    upscaleRenderExtent_ = candidate->renderExtent();
    upscaleDisplayExtent_ = display;
    dlss_ = std::move(candidate);
    return upscaleRenderExtent_;
  } catch (const std::exception& error) {
    upscalingStatus_ = error.what();
    return std::nullopt;
  }
}

void TemporalManager::prepareCamera(container::gpu::CameraData& camera, glm::uvec2 extent,
                                    const container::temporal::Settings& settings,
                                    const glm::mat4& projection, float frameMilliseconds) {
  std::optional<glm::vec2> jitterOverride;
  if (fsrActive() && settings.enabled) {
    const auto jitter = fsr_->nextJitterPixels();
    if (!jitter) throw std::runtime_error("Could not query the active Vulkan FSR jitter");
    upscaleJitter_ = *jitter;
    fsrVerticalFov_ = 2.0f * std::atan(1.0f / std::abs(projection[1][1]));
    jitterOverride = glm::vec2(upscaleJitter_[0], upscaleJitter_[1]);
  }
  state_.prepareCamera(camera, extent, settings, projection, jitterOverride);
  upscaleFrameMilliseconds_ = std::isfinite(frameMilliseconds) && frameMilliseconds > 0.0f
      ? frameMilliseconds : 1000.0f / 60.0f;
  if (dlssActive() && settings.enabled)
    upscaleJitter_ = {camera.jitterUv.x * extent.x, camera.jitterUv.y * extent.y};
}

void TemporalManager::createPipelines(const std::filesystem::path &root,
                                      VkDescriptorSetLayout sceneLayout,
                                      VkFormat depthFormat) {
  if (resolvePipeline_)
    return;
  const VkDevice device = device_->device();
  std::vector<VkDescriptorSetLayoutBinding> bindings;
  for (uint32_t binding = 0; binding < 13; ++binding) {
    const auto type = binding == 0   ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                      : binding == 1 ? VK_DESCRIPTOR_TYPE_SAMPLER
                      : binding < 9  ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings.push_back(
        {binding, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
  }
  resolveSetLayout_ = pipelines_.createDescriptorSetLayout(
      bindings, std::vector<VkDescriptorBindingFlags>(bindings.size(), 0));
  bindings.assign(composeBindings.begin(), composeBindings.end());
  composeSetLayout_ = pipelines_.createDescriptorSetLayout(
      bindings, std::vector<VkDescriptorBindingFlags>(bindings.size(), 0));
  resolveLayout_ = pipelines_.createPipelineLayout(
      {resolveSetLayout_},
      {{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolveConstants)}});
  composeLayout_ = pipelines_.createPipelineLayout(
      {composeSetLayout_}, {{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16}});
  velocityLayout_ = pipelines_.createPipelineLayout(
      {sceneLayout},
      {{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(container::gpu::BindlessPushConstants)}});
  const auto shaders = root / "spv_shaders";
  resolvePipeline_ = pipelines_.createComputePipeline(
      shaders / "temporal_resolve.comp.spv", resolveLayout_, "taa.resolve");
  composePipeline_ = pipelines_.createComputePipeline(
      shaders / "temporal_compose.comp.spv", composeLayout_, "taa.compose");
  VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = samplerInfo.addressModeV =
      samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (createOwnedSampler(device, &samplerInfo, nullptr, &sampler_) !=
      VK_SUCCESS)
    throw std::runtime_error("TAA sampler creation failed");

  velocityDepthFormat_ = depthFormat;
  auto candidate = prepareVelocityReload(root);
  publishVelocityReload(candidate);
}

PreparedGraphicsPipelines TemporalManager::prepareVelocityReload(const std::filesystem::path& root) {
  PreparedGraphicsPipelines candidate(pipelines_);
  if (!velocityLayout_)
    return candidate;
  const VkDevice device = device_->device();
  auto module = [&](const char *name) {
    return container::gpu::createShaderModule(
        device, container::util::readFile(root / "spv_shaders" / name));
  };
  VkShaderModule vertex = module("temporal_velocity.vert.spv");
  VkShaderModule fragment{};
  try {
    fragment = module("temporal_velocity.frag.spv");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (auto &stage : stages)
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "vertMain";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "fragMain";
    const auto vertexBindings = temporalVelocityVertexBindings();
    const auto attributes = temporalVelocityVertexAttributes();
    VkPipelineVertexInputStateCreateInfo input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount =
        static_cast<uint32_t>(vertexBindings.size());
    input.pVertexBindingDescriptions = vertexBindings.data();
    input.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo samples{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachments{};
    for (auto &attachment : blendAttachments)
      attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();
    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates.data();
    const std::array<VkFormat, 3> formats = {
        VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R8_UNORM};
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 3;
    rendering.pColorAttachmentFormats = formats.data();
    rendering.depthAttachmentFormat = velocityDepthFormat_;
    VkGraphicsPipelineCreateInfo info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = velocityLayout_;
    const std::array<VkCullModeFlags, 3> culls = {
        VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE};
    for (uint32_t i = 0; i < 3; ++i) {
      raster.cullMode = culls[i];
      if (createOwnedGraphicsPipelines(
              device, pipelines_.getOrCreatePipelineCache("taa.velocity"), 1,
              &info, nullptr, &candidate.handles[i]) != VK_SUCCESS)
        throw std::runtime_error("TAA velocity pipeline creation failed");
    }
  } catch (...) {
    if (fragment)
      destroyOwnedShaderModule(device, fragment, nullptr);
    destroyOwnedShaderModule(device, vertex, nullptr);
    throw;
  }
  destroyOwnedShaderModule(device, fragment, nullptr);
  destroyOwnedShaderModule(device, vertex, nullptr);
  return candidate;
}

void TemporalManager::publishVelocityReload(PreparedGraphicsPipelines& candidate) noexcept {
  if (candidate.handles[0])
    velocityPipelines_.swap(candidate.handles);
}

AttachmentImage TemporalManager::image(VkFormat format,
                                       VkImageUsageFlags usage,
                                       VkExtent2D extent) {
  VkFormatProperties features{};
  vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), format,
                                      &features);
  if ((features.optimalTilingFeatures &
       (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) !=
      (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
       VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
    throw std::runtime_error(
        "Temporal image format lacks sampled/storage support");
  AttachmentImage result{};
  result.format = format;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {extent.width, extent.height, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo allocation{};
  allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_.memoryManager()->allocator(), &info,
                     &allocation, &result.image, &result.allocation,
                     nullptr) != VK_SUCCESS)
    throw std::runtime_error("Temporal image allocation failed");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = result.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (createVulkanImageView(device_->device(), &view, nullptr, &result.view) !=
      VK_SUCCESS) {
    vmaDestroyImage(allocator_.memoryManager()->allocator(), result.image,
                    result.allocation);
    throw std::runtime_error("Temporal image view creation failed");
  }
  return result;
}

void TemporalManager::destroyImage(AttachmentImage &attachment) {
  if (attachment.view)
    destroyVulkanImageView(device_->device(), attachment.view, nullptr);
  if (attachment.image)
    vmaDestroyImage(allocator_.memoryManager()->allocator(), attachment.image,
                    attachment.allocation);
  attachment = {};
}

void TemporalManager::destroyImages() {
  // Retire the vendor feature before releasing its imported frame images.
  fsr_.reset();
  dlss_.reset();
  upscalingStatus_ = "native resolution";
  destroyFrameImages();
}

void TemporalManager::destroyFrameImages() {
  if (pool_) {
    destroyOwnedDescriptorPool(device_->device(), pool_, nullptr);
    pool_ = {};
  }
  for (auto& input : inputs_) {
    destroyImage(input.upscaleDepth);
    destroyImage(input.upscaleMotion);
    destroyImage(input.upscaleReactive);
    destroyImage(input.upscaleTransparency);
    destroyImage(input.upscaleOutput);
    destroyImage(input.dlssOitLayer);
  }
  inputs_.clear();
  for (auto &history : histories_) {
    destroyImage(history.color);
    destroyImage(history.depth);
    destroyImage(history.identity);
    history.initialized = false;
  }
  resolveRecorded_ = false;
  state_.reset("history resources recreated");
}

void TemporalManager::prepare(FrameResourceManager &frames, VkExtent2D extent,
                              uint32_t imageCount) {
  const bool useUpscaler = upscalingActive();
  const bool useDlss = dlssActive();
  const VkFormat upscaleOutputFormat = fsrActive() ? fsr_->outputFormat() : VK_FORMAT_R16G16B16A16_SFLOAT;
  if ((!settings_.enabled && !useUpscaler) || !extent.width || !extent.height || !imageCount)
    return;
  frames.ensureTemporalAttachments();
  if (inputs_.size() == imageCount && extent_.width == extent.width &&
      extent_.height == extent.height &&
      (!useUpscaler || inputs_[0].upscaleOutput.format == upscaleOutputFormat) &&
      inputs_[0].frame.temporalMotion.image ==
          frames.frame(0)->temporalMotion.image)
    return;
  if (useUpscaler && (!inputs_.empty() || extent.width != upscaleRenderExtent_.width ||
      extent.height != upscaleRenderExtent_.height))
    throw std::logic_error("The upscaler requires retired frame resources at its configured render extent");
  destroyFrameImages();
  extent_ = extent;
  try {
    const auto usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!useUpscaler) {
      for (auto &history : histories_) {
        history.color = image(VK_FORMAT_R16G16B16A16_SFLOAT, usage, extent);
        history.depth = image(VK_FORMAT_R32G32_SFLOAT, usage, extent);
        history.identity = image(VK_FORMAT_R32_UINT, usage, extent);
      }
    }
    const std::array<VkDescriptorPoolSize, 5> sizes = {
        {{VK_DESCRIPTOR_TYPE_SAMPLER, imageCount * (useDlss ? 2u : useUpscaler ? 1u : 3u)},
         {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imageCount * (useDlss ? 6u : useUpscaler ? 5u : 16u)},
         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, imageCount * (useDlss ? 9u : useUpscaler ? 7u : 10u)},
         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, imageCount * (useUpscaler ? 1u : 3u)},
         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, imageCount * 2}}};
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    const uint32_t setCount = useUpscaler && !useDlss ? 2u : 3u;
    poolInfo.maxSets = imageCount * setCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (createOwnedDescriptorPool(device_->device(), &poolInfo, nullptr,
                                  &pool_) != VK_SUCCESS)
      throw std::runtime_error("Temporal descriptor pool creation failed");
    inputs_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
      auto &input = inputs_[i];
      input.frame = *frames.frame(i);
      const std::array<VkDescriptorSetLayout, 3> layouts = {
          useDlss ? dlssComposeSetLayout_ : composeSetLayout_,
          useUpscaler ? upscaleInputsSetLayout_ : resolveSetLayout_,
          useDlss ? dlssTransparencySetLayout_ : resolveSetLayout_};
      VkDescriptorSetAllocateInfo info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      info.descriptorPool = pool_;
      info.descriptorSetCount = setCount;
      info.pSetLayouts = layouts.data();
      std::array<VkDescriptorSet, 3> sets{};
      if (vkAllocateDescriptorSets(device_->device(), &info, sets.data()) !=
          VK_SUCCESS)
        throw std::runtime_error("Temporal descriptor allocation failed");
      input.compose = sets[0];
      if (useUpscaler) {
        input.upscaleInputs = sets[1];
        input.upscaleDepth = image(VK_FORMAT_R32_SFLOAT, usage, extent);
        input.upscaleMotion = image(VK_FORMAT_R16G16_SFLOAT, usage, extent);
        input.upscaleReactive = image(VK_FORMAT_R8_UNORM, usage, extent);
        input.upscaleTransparency = image(VK_FORMAT_R8_UNORM, usage, extent);
        input.upscaleOutput = image(upscaleOutputFormat,
            usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, upscaleDisplayExtent_);
        if (useDlss) {
          input.dlssTransparency = sets[2];
          input.dlssOitLayer = image(VK_FORMAT_R16G16B16A16_SFLOAT, usage, extent);
        }
      } else {
        input.resolve = {sets[1], sets[2]};
      }
    }
  } catch (...) {
    destroyImages();
    throw;
  }
}

void TemporalManager::updateDescriptors(
    uint32_t imageIndex, const FrameResources &frame,
    const container::gpu::AllocatedBuffer &camera, bool resolveThisFrame) {
  // Override only the current image's post-process descriptors, after its
  // fence.
  const std::array<VkDescriptorImageInfo, 3> postImages = {
      {{{},
        (active() && resolveThisFrame) ? outputView(imageIndex) : frame.sceneColor.view,
        (active() && resolveThisFrame)
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
       {{},
        (active() && resolveThisFrame) ? frame.temporalMotion.view
                                       : frame.normal.view,
        (active() && resolveThisFrame)
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
       {{},
        (active() && resolveThisFrame) ? frame.temporalDiagnostics.view
                                       : frame.emissive.view,
        (active() && resolveThisFrame)
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
  std::array<VkWriteDescriptorSet, 3> postWrites{};
  const std::array<uint32_t, 3> postBindings = {1, 14, 15};
  for (uint32_t i = 0; i < 3; ++i) {
    auto &write = postWrites[i];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = frame.postProcessDescriptorSet;
    write.dstBinding = postBindings[i];
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &postImages[i];
  }
  vkUpdateDescriptorSets(device_->device(), 3, postWrites.data(), 0, nullptr);
  if (!active() || imageIndex >= inputs_.size())
    return;
  auto &input = inputs_[imageIndex];
  input.frame = frame;
  VkDescriptorBufferInfo cameraInfo{camera.buffer, 0,
                                    sizeof(container::gpu::CameraData)};
  for (uint32_t slot = 0; !upscalingActive() && slot < 2; ++slot) {
    const auto &old = histories_[slot ^ 1u];
    const auto &output = histories_[slot];
    const std::array<VkImageView, 11> views = {frame.temporalComposite.view,
                                               frame.temporalMotion.view,
                                               frame.temporalIdentity.view,
                                               frame.depthSamplingView,
                                               old.color.view,
                                               old.depth.view,
                                               old.identity.view,
                                               output.color.view,
                                               output.depth.view,
                                               output.identity.view,
                                               frame.temporalDiagnostics.view};
    std::array<VkDescriptorImageInfo, 12> images{};
    images[0].sampler = sampler_;
    std::array<VkWriteDescriptorSet, 13> writes{};
    for (uint32_t binding = 0; binding < 13; ++binding) {
      auto &write = writes[binding];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = input.resolve[slot];
      write.dstBinding = binding;
      write.descriptorCount = 1;
      if (binding == 0) {
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &cameraInfo;
      } else if (binding == 1) {
        write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        write.pImageInfo = &images[0];
      } else {
        auto &info = images[binding - 1];
        info.imageView = views[binding - 2];
        info.imageLayout = binding == 5
                               ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_GENERAL;
        write.descriptorType = binding < 9 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &info;
      }
    }
    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
  }
  const std::array<VkDescriptorImageInfo, 6> images = {
      {{sampler_, {}, {}},
       {{}, frame.sceneColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
       {{}, frame.temporalReactive.view, VK_IMAGE_LAYOUT_GENERAL},
       {{}, frame.temporalComposite.view, VK_IMAGE_LAYOUT_GENERAL},
       {{}, frame.oitHeadPointers.view, VK_IMAGE_LAYOUT_GENERAL},
       {{}, input.dlssOitLayer.view, VK_IMAGE_LAYOUT_GENERAL}}};
  const std::array<VkDescriptorBufferInfo, 3> buffers = {
      {{frame.oitNodeBuffer.buffer, 0, VK_WHOLE_SIZE},
       {frame.oitCounterBuffer.buffer, 0, sizeof(uint32_t)},
       {frame.oitMetadataBuffer.buffer, 0, sizeof(OitMetadata)}}};
  std::array<VkWriteDescriptorSet, 9> writes{};
  const uint32_t composeWrites = dlssActive() ? 9u : 8u;
  for (uint32_t binding = 0; binding < composeWrites; ++binding) {
    auto &write = writes[binding];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = input.compose;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    if (binding < 5 || binding == 8) {
      write.pImageInfo = &images[binding == 8 ? 5u : binding];
      write.descriptorType = binding == 0  ? VK_DESCRIPTOR_TYPE_SAMPLER
                             : binding < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    } else {
      write.pBufferInfo = &buffers[binding - 5];
      write.descriptorType = binding == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
  }
  vkUpdateDescriptorSets(device_->device(), composeWrites, writes.data(), 0, nullptr);
  if (upscalingActive()) updateUpscaleDescriptors(input);
  if (dlssActive()) updateDlssTransparencyDescriptors(input);
}

void TemporalManager::updateDlssTransparencyDescriptors(const Input& input) {
  const std::array<VkDescriptorImageInfo, 3> images{{
      {sampler_, {}, {}},
      {{}, input.dlssOitLayer.view, VK_IMAGE_LAYOUT_GENERAL},
      {{}, input.upscaleOutput.view, VK_IMAGE_LAYOUT_GENERAL},
  }};
  const std::array types{VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  std::array<VkWriteDescriptorSet, images.size()> writes{};
  for (uint32_t binding = 0; binding < writes.size(); ++binding)
    writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        input.dlssTransparency, binding, 0, 1, types[binding], &images[binding]};
  vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void TemporalManager::updateUpscaleDescriptors(const Input& input) {
  const auto& frame = input.frame;
  const std::array views{frame.depthSamplingView, frame.temporalMotion.view,
      frame.temporalComposite.view, input.upscaleDepth.view, input.upscaleMotion.view,
      input.upscaleReactive.view, input.upscaleTransparency.view, frame.temporalDiagnostics.view};
  std::array<VkDescriptorImageInfo, views.size()> images{};
  std::array<VkWriteDescriptorSet, views.size()> writes{};
  for (uint32_t binding = 0; binding < views.size(); ++binding) {
    images[binding] = {{}, views[binding], binding == 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                      : VK_IMAGE_LAYOUT_GENERAL};
    auto& write = writes[binding];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = input.upscaleInputs;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = binding < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &images[binding];
  }
  vkUpdateDescriptorSets(device_->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void TemporalManager::recordVelocity(VkCommandBuffer cmd,
                                     const FrameRecordParams &p) {
  if (!needed(p))
    return;
  auto &input = inputs_.at(p.runtime.imageIndex);
  const auto &f = input.frame;
  if (!input.initialized) {
    for (auto attachment :
         {f.temporalMotion, f.temporalIdentity, f.temporalReactive,
          f.temporalComposite, f.temporalDiagnostics})
      initializeImage(cmd, attachment.image);
    if (upscalingActive()) {
      for (const auto& attachment : {input.upscaleDepth, input.upscaleMotion, input.upscaleReactive,
                                      input.upscaleTransparency, input.upscaleOutput})
        initializeImage(cmd, attachment.image);
    }
    if (dlssActive())
      initializeImage(cmd, input.dlssOitLayer.image);
    input.initialized = true;
  }
  dependency(cmd,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
  std::array<VkRenderingAttachmentInfo, 3> colors{};
  const std::array<VkImageView, 3> views = {
      f.temporalMotion.view, f.temporalIdentity.view, f.temporalReactive.view};
  for (uint32_t i = 0; i < 3; ++i) {
    colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colors[i].imageView = views[i];
    colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = f.depthStencil.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
  VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea.extent = extent_;
  info.layerCount = 1;
  info.colorAttachmentCount = 3;
  info.pColorAttachments = colors.data();
  info.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &info);
  recordSceneViewportAndScissor(cmd, extent_);
  container::gpu::BindlessPushConstants push =
      p.pushConstants.bindless ? *p.pushConstants.bindless
                               : container::gpu::BindlessPushConstants{};
  auto *cull = p.services.gpuCullManager;
  const bool indirect =
      cull && cull->isReady() && cull->frustumDrawsValid(p.runtime.imageIndex);
  const bool occluded = cull && cull->isReady() &&
                        cull->occlusionDrawsValid(p.runtime.imageIndex);
  auto plan = buildSceneOpaqueDrawPlan(
      {.gpuIndirectAvailable = indirect,
       .occludedGpuIndirectAvailable = occluded,
       .preferOccludedGpuIndirect = true,
       .draws = {p.draws.opaqueDrawCommands,
                 p.draws.opaqueSingleSidedDrawCommands,
                 p.draws.opaqueWindingFlippedDrawCommands,
                 p.draws.opaqueDoubleSidedDrawCommands}});
  const DebugOverlayRenderer drawHelper;
  const SceneOpaqueDrawPipelineHandles pipelines{
      velocityPipelines_[0], velocityPipelines_[1], velocityPipelines_[2]};
  (void)recordTemporalPreviousVertexBinding(
      cmd, p.scene.vertexSlice, p.scene.previousVertexSlice);
  (void)recordSceneOpaqueDrawCommands(
      cmd,
      {.plan = &plan,
       .geometry = {p.descriptorSet(p.runtime.activeTechnique,
                                    "scene-descriptor-set"),
                    p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType},
       .pipelines = pipelines,
       .pipelineLayout = velocityLayout_,
       .pushConstants = push,
       .imageIndex = p.runtime.imageIndex,
       .debugOverlay = &drawHelper,
       .gpuCullManager = cull});
  const VkDescriptorSet bimSet =
      p.descriptorSet(p.runtime.activeTechnique, "bim-scene-descriptor-set");
  if (bimSet && p.bim.scene.vertexSlice.buffer &&
      p.bim.scene.indexSlice.buffer) {
    BimSurfacePassInputs inputs{};
    inputs.passReady = inputs.geometryReady = inputs.descriptorSetReady =
        inputs.bindlessPushConstantsReady = inputs.basePipelineReady = true;
    const auto drawSources = bimSurfaceDrawListSet(p.bim);
    inputs.sourceCount = 3;
    for (uint32_t source = 0; source < 3; ++source) {
      const auto &sourceDraws = *drawSources[source];
      inputs.sources[source].source =
          static_cast<BimSurfacePassSourceKind>(source);
      inputs.sources[source].draws = {
          sourceDraws.opaqueDrawCommands,
          sourceDraws.opaqueSingleSidedDrawCommands,
          sourceDraws.opaqueWindingFlippedDrawCommands,
          sourceDraws.opaqueDoubleSidedDrawCommands};
    }
    inputs.sources[0].gpuCompactionEligible =
        p.bim.opaqueMeshDrawsUseGpuVisibility;
    inputs.sources[0].gpuVisibilityOwnsCpuFallback =
        p.bim.opaqueMeshDrawsUseGpuVisibility;
    const auto bimPlan = buildBimSurfacePassPlan(inputs);
    const std::array<VkDescriptorSet, 1> sets = {bimSet};
    // BIM has rigid object history and its own vertex index space. Never leave
    // primary-scene deformation history bound while drawing BIM triangles.
    (void)recordTemporalPreviousVertexBinding(cmd, p.bim.scene.vertexSlice);
    (void)recordBimSurfacePassCommands(
        cmd, {.plan = &bimPlan,
              .geometry = {sets, p.bim.scene.vertexSlice,
                           p.bim.scene.indexSlice, p.bim.scene.indexType},
              .singleSidedPipeline = velocityPipelines_[0],
              .windingFlippedPipeline = velocityPipelines_[1],
              .doubleSidedPipeline = velocityPipelines_[2],
              .pipelineLayout = velocityLayout_,
              .pushConstants = push,
              .debugOverlay = &drawHelper,
              .bimManager = p.services.bimManager});
  }
  vkCmdEndRendering(cmd);
  dependency(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

void TemporalManager::recordResolve(VkCommandBuffer cmd,
                                    const FrameRecordParams &p) {
  if (!needed(p))
    return;
  // Editor overlays are rendered into the HDR scene only on display frames.
  // A raw capture and a display frame cannot share that accumulated color.
  recordedSceneLinear_ = p.screenshot.sceneLinear;
  const auto hasDraws = [](const FrameDrawLists &draws) {
    return hasOpaqueDrawCommands(draws) || hasTransparentDrawCommands(draws);
  };
  recordedDisplayOverlays_ = !recordedSceneLinear_ &&
      (p.debug.temporalForceReactive || p.bim.floorPlan.enabled ||
       p.bim.coordinationMarkers.issueMarkersEnabled ||
       p.bim.coordinationMarkers.clashMarkersEnabled ||
       p.bim.sectionPlaneVisual.enabled || p.bim.sectionClipCapGeometry.valid() ||
       (p.bim.technicalElevation.enabled && p.bim.technicalElevation.depthTestLines) ||
       hasDrawCommands(p.draws.hoveredDrawCommands) ||
       hasDrawCommands(p.bim.draws.hoveredDrawCommands) ||
       (p.bim.pointCurveStyle.points.enabled && hasDraws(p.bim.pointDraws)) ||
       (p.bim.pointCurveStyle.curves.enabled && hasDraws(p.bim.curveDraws)));
  const bool historyValid =
      state_.valid() && historySceneLinear_ == recordedSceneLinear_ &&
      historyDisplayOverlays_ == recordedDisplayOverlays_;
  if (!upscalingActive()) {
    for (auto& history : histories_) {
      if (!history.initialized) {
        for (auto attachment : {history.color, history.depth, history.identity}) {
          initializeImage(cmd, attachment.image);
          const VkClearColorValue clear{};
          const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          vkCmdClearColorImage(cmd, attachment.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        }
        history.initialized = true;
      }
    }
  }
  dependency(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  const auto &input = inputs_.at(p.runtime.imageIndex);
  // The OIT producer's storage writes and previous consumers are ordered even
  // if the graph's optional OIT preparation node is inactive.
  const bool transparent =
      hasTransparentDrawCommands(p) ||
      (p.bim.nativePointDraws.transparentDrawCommands &&
       !p.bim.nativePointDraws.transparentDrawCommands->empty()) ||
      (p.bim.nativeCurveDraws.transparentDrawCommands &&
       !p.bim.nativeCurveDraws.transparentDrawCommands->empty());
  // HDR editor overlays and native primitives do not expose matching surface
  // velocity. Their conservative fallback cannot borrow an opaque surface.
  const bool fallback = recordedDisplayOverlays_ ||
                        hasOpaqueDrawCommands(p.bim.nativePointDraws) ||
                        hasOpaqueDrawCommands(p.bim.nativeCurveDraws);
  // The volume pass writes exact visible coverage into spare HDR alpha. Merge
  // it here, after velocity has cleared/rasterized the surface reactive mask.
  // The flag mirrors the producer so ordinary surface alpha is never consumed.
  const bool volumeCoverage = p.services.openVdbVolumeRenderer &&
                             p.services.openVdbVolumeRenderer->records(p);
  const std::array<uint32_t, 4> compose = {
      extent_.width, extent_.height, transparent ? 1u : 0u,
      (fallback ? 1u : 0u) | (volumeCoverage ? 2u : 0u)};
  const auto composeLayout = dlssActive() ? dlssComposeLayout_ : composeLayout_;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dlssActive() ? dlssComposePipeline_ : composePipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composeLayout,
                          0, 1, &input.compose, 0, nullptr);
  vkCmdPushConstants(cmd, composeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(compose), compose.data());
  vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
  dependency(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
             VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
  if (upscalingActive()) {
    recordUpscale(cmd, p, input, transparent, historyValid);
    resolveRecorded_ = true;
    return;
  }
  const ResolveConstants constants{extent_.width,
                                   extent_.height,
                                   historyValid ? 1u : 0u,
                                   p.camera.orthographic ? 1u : 0u,
                                   settings_.historyWeight,
                                   settings_.depthAbsoluteTolerance,
                                   settings_.depthRelativeTolerance,
                                   settings_.varianceGamma,
                                   p.camera.nearPlane,
                                   p.camera.farPlane,
                                   p.camera.nearPlane,
                                   p.camera.farPlane};
  const VkDescriptorSet set = input.resolve[writeSlot()];
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolvePipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resolveLayout_,
                          0, 1, &set, 0, nullptr);
  vkCmdPushConstants(cmd, resolveLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(constants), &constants);
  vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
  dependency(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_MEMORY_READ_BIT);
  resolveRecorded_ = true;
}

void TemporalManager::recordUpscale(VkCommandBuffer cmd, const FrameRecordParams& p, const Input& input,
                                    bool hasTransparency, bool historyValid) {
  const std::array constants{extent_.width, extent_.height};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscaleInputsPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, upscaleInputsLayout_, 0, 1,
      &input.upscaleInputs, 0, nullptr);
  vkCmdPushConstants(cmd, upscaleInputsLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants.data());
  vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);

  const std::array attachments{input.frame.temporalComposite.image, input.upscaleDepth.image,
      input.upscaleMotion.image, input.upscaleReactive.image, input.upscaleTransparency.image};
  std::array<VkImageMemoryBarrier2, attachments.size()> barriers{};
  for (uint32_t i = 0; i < attachments.size(); ++i) {
    barriers[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, nullptr,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, attachments[i],
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  }
  VkDependencyInfo inputsReady{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  inputsReady.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
  inputsReady.pImageMemoryBarriers = barriers.data();
  vkCmdPipelineBarrier2(cmd, &inputsReady);

  if (dlssActive()) {
    const auto dlssImage = [this](const AttachmentImage& image) -> DlssImage {
      return {image.image, image.view, image.format, extent_};
    };
    const auto result = dlss_->record({.commands = cmd,
        .color = dlssImage(input.frame.temporalComposite), .depth = dlssImage(input.upscaleDepth),
        .motion = dlssImage(input.upscaleMotion),
        .output = {input.upscaleOutput.image, input.upscaleOutput.view, input.upscaleOutput.format, upscaleDisplayExtent_},
        .jitterPixels = upscaleJitter_, .reset = !historyValid, .frameMilliseconds = upscaleFrameMilliseconds_});
    if (result != DlssDispatchResult::Recorded)
      throw std::runtime_error("Could not record the active Vulkan DLSS frame: " + std::string(dlss_->status()));
    if (hasTransparency) {
      dependency(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
      const TransparencyConstants layer{upscaleDisplayExtent_.width, upscaleDisplayExtent_.height,
          upscaleJitter_[0] / extent_.width, upscaleJitter_[1] / extent_.height};
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dlssTransparencyPipeline_);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, dlssTransparencyLayout_, 0, 1,
          &input.dlssTransparency, 0, nullptr);
      vkCmdPushConstants(cmd, dlssTransparencyLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(layer), &layer);
      vkCmdDispatch(cmd, (layer.width + 7u) / 8u, (layer.height + 7u) / 8u, 1);
    }
  } else {
    const auto fsrImage = [this](const AttachmentImage& image) -> FsrImage {
      return {image.image, image.format, extent_};
    };
    const auto result = fsr_->record({.commands = cmd,
        .color = fsrImage(input.frame.temporalComposite), .depth = fsrImage(input.upscaleDepth),
        .motion = fsrImage(input.upscaleMotion), .reactive = fsrImage(input.upscaleReactive),
        .transparency = fsrImage(input.upscaleTransparency),
        .output = {input.upscaleOutput.image, input.upscaleOutput.format, upscaleDisplayExtent_},
        .jitterPixels = upscaleJitter_, .cameraNear = p.camera.nearPlane, .cameraFar = p.camera.farPlane,
        .verticalFovRadians = fsrVerticalFov_, .frameMilliseconds = upscaleFrameMilliseconds_,
        .reset = !historyValid});
    if (result != FsrDispatchResult::Recorded)
      throw std::runtime_error("Could not record the active Vulkan FSR frame: " + std::string(fsr_->status()));
  }

  // Restore the frame-local resources for the next use of this image. The
  // SDK leaves the presentation HDR image in GENERAL for post-processing.
  for (auto& barrier : barriers) {
    std::swap(barrier.oldLayout, barrier.newLayout);
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  }
  vkCmdPipelineBarrier2(cmd, &inputsReady);
  dependency(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
}

void TemporalManager::commit() {
  if (fsrActive() && resolveRecorded_) fsr_->commit();
  if (dlssActive() && resolveRecorded_) dlss_->commit();
  if (resolveRecorded_) {
    historySceneLinear_ = recordedSceneLinear_;
    historyDisplayOverlays_ = recordedDisplayOverlays_;
  }
  if ((!settings_.enabled && !upscalingActive()) || resolveRecorded_)
    state_.commit();
  else
    state_.reset("temporal pass skipped");
  resolveRecorded_ = false;
}

VkImageView TemporalManager::outputView(uint32_t image) const {
  if (upscalingActive()) return image < inputs_.size() ? inputs_[image].upscaleOutput.view : VK_NULL_HANDLE;
  return histories_[writeSlot()].color.view;
}

VkImage TemporalManager::outputImage(uint32_t image) const {
  if (upscalingActive()) return image < inputs_.size() ? inputs_[image].upscaleOutput.image : VK_NULL_HANDLE;
  return histories_[writeSlot()].color.image;
}

VkImageView TemporalManager::diagnosticView(uint32_t index) const {
  return index < inputs_.size() ? inputs_[index].frame.temporalDiagnostics.view
                                : VK_NULL_HANDLE;
}

uint64_t TemporalManager::memoryBytes() const {
  if (upscalingActive()) {
    const uint64_t render = uint64_t(extent_.width) * extent_.height;
    const uint64_t display = uint64_t(upscaleDisplayExtent_.width) * upscaleDisplayExtent_.height;
    const uint64_t outputPixelBytes = fsrActive() && fsr_->outputFormat() == VK_FORMAT_R32G32B32A32_SFLOAT ? 16u : 8u;
    return ((dlssActive() ? 55u : 47u) * render + outputPixelBytes * display) * inputs_.size();
  }
  return !inputs_.empty() ? uint64_t(extent_.width) * extent_.height *
                                (40u + 37u * inputs_.size())
                          : 0u;
}

uint64_t TemporalManager::allocatedBytes() const {
  uint64_t bytes = 0;
  const auto add = [&](const AttachmentImage &attachment) {
    if (attachment.allocation) {
      VmaAllocationInfo info{};
      vmaGetAllocationInfo(allocator_.memoryManager()->allocator(),
                           attachment.allocation, &info);
      bytes += info.size;
    }
  };
  for (const auto &history : histories_) {
    add(history.color);
    add(history.depth);
    add(history.identity);
  }
  for (const auto &input : inputs_) {
    add(input.frame.temporalMotion);
    add(input.frame.temporalIdentity);
    add(input.frame.temporalReactive);
    add(input.frame.temporalComposite);
    add(input.frame.temporalDiagnostics);
    add(input.upscaleDepth);
    add(input.upscaleMotion);
    add(input.upscaleReactive);
    add(input.upscaleTransparency);
    add(input.upscaleOutput);
    add(input.dlssOitLayer);
  }
  return bytes;
}

void registerTemporalPasses(RenderGraphBuilder& graph, RenderPassId predecessor) {
  graph.addPass(RenderPassId::TemporalVelocity, {predecessor},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (p.services.temporalManager)
                    p.services.temporalManager->recordVelocity(cmd, p);
                });
  graph.setPassReadiness(
      RenderPassId::TemporalVelocity, [](const FrameRecordParams &p) {
        return needed(p) ? renderPassReady() : renderPassNotNeeded();
      });
  graph.setPassResourceAccess(
      RenderPassId::TemporalVelocity,
      {RenderResourceId::CameraBuffer, RenderResourceId::SceneDepth},
      {RenderResourceId::SceneGeometry, RenderResourceId::ObjectBuffer,
       RenderResourceId::BimGeometry, RenderResourceId::BimObjectBuffer,
       RenderResourceId::FrustumCullDraws,
       RenderResourceId::OcclusionCullDraws},
      {RenderResourceId::TemporalMotion});
  graph.addPass(RenderPassId::TemporalResolve, {RenderPassId::TemporalVelocity},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (p.services.temporalManager)
                    p.services.temporalManager->recordResolve(cmd, p);
                });
  graph.setPassReadiness(
      RenderPassId::TemporalResolve, [](const FrameRecordParams &p) {
        return needed(p) ? renderPassReady() : renderPassNotNeeded();
      });
  graph.setPassResourceAccess(
      RenderPassId::TemporalResolve,
      {RenderResourceId::SceneColor, RenderResourceId::TemporalMotion,
       RenderResourceId::SceneDepth},
      {RenderResourceId::OitStorage}, {RenderResourceId::TemporalColor});
}

VkImageLayout temporalSceneColorLayout(const FrameRecordParams &frame) {
  return needed(frame) ? VK_IMAGE_LAYOUT_GENERAL
                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageView temporalSceneColorView(const FrameRecordParams &frame,
                                   VkImageView nativeView) {
  return needed(frame) ? frame.services.temporalManager->outputView(frame.runtime.imageIndex)
                       : nativeView;
}
} // namespace container::renderer
