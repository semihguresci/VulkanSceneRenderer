#include "Container/renderer/core/RenderPassManager.h"

#include "Container/renderer/core/RendererMsaa.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace container::renderer {

namespace {

constexpr VkPipelineStageFlags kDepthTestStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

constexpr VkDependencyFlags kFramebufferLocalDependency =
    VK_DEPENDENCY_BY_REGION_BIT;

VkSubpassDependency MakeDependency(uint32_t srcSubpass,
                                   uint32_t dstSubpass,
                                   VkPipelineStageFlags srcStages,
                                   VkPipelineStageFlags dstStages,
                                   VkAccessFlags srcAccess,
                                   VkAccessFlags dstAccess,
                                   VkDependencyFlags flags = 0) {
  VkSubpassDependency dependency{};
  dependency.srcSubpass = srcSubpass;
  dependency.dstSubpass = dstSubpass;
  dependency.srcStageMask = srcStages;
  dependency.dstStageMask = dstStages;
  dependency.srcAccessMask = srcAccess;
  dependency.dstAccessMask = dstAccess;
  dependency.dependencyFlags = flags;
  return dependency;
}

VkAttachmentDescription2 MakeAttachmentDescription2(
    const VkAttachmentDescription& attachment) {
  VkAttachmentDescription2 result{
      VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
  result.flags = attachment.flags;
  result.format = attachment.format;
  result.samples = attachment.samples;
  result.loadOp = attachment.loadOp;
  result.storeOp = attachment.storeOp;
  result.stencilLoadOp = attachment.stencilLoadOp;
  result.stencilStoreOp = attachment.stencilStoreOp;
  result.initialLayout = attachment.initialLayout;
  result.finalLayout = attachment.finalLayout;
  return result;
}

VkAttachmentReference2 MakeAttachmentReference2(
    const VkAttachmentReference& reference) {
  VkAttachmentReference2 result{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
  result.attachment = reference.attachment;
  result.layout = reference.layout;
  return result;
}

VkSubpassDependency2 MakeDependency2(const VkSubpassDependency& dependency) {
  VkSubpassDependency2 result{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
  result.srcSubpass = dependency.srcSubpass;
  result.dstSubpass = dependency.dstSubpass;
  result.srcStageMask = dependency.srcStageMask;
  result.dstStageMask = dependency.dstStageMask;
  result.srcAccessMask = dependency.srcAccessMask;
  result.dstAccessMask = dependency.dstAccessMask;
  result.dependencyFlags = dependency.dependencyFlags;
  return result;
}

template <std::size_t N>
std::array<VkAttachmentDescription2, N> MakeAttachmentDescriptions2(
    const std::array<VkAttachmentDescription, N>& attachments) {
  std::array<VkAttachmentDescription2, N> result{};
  for (std::size_t i = 0; i < N; ++i) {
    result[i] = MakeAttachmentDescription2(attachments[i]);
  }
  return result;
}

template <std::size_t N>
std::array<VkAttachmentReference2, N> MakeAttachmentReferences2(
    const std::array<VkAttachmentReference, N>& references) {
  std::array<VkAttachmentReference2, N> result{};
  for (std::size_t i = 0; i < N; ++i) {
    result[i] = MakeAttachmentReference2(references[i]);
  }
  return result;
}

template <std::size_t N>
std::array<VkSubpassDependency2, N> MakeDependencies2(
    const std::array<VkSubpassDependency, N>& dependencies) {
  std::array<VkSubpassDependency2, N> result{};
  for (std::size_t i = 0; i < N; ++i) {
    result[i] = MakeDependency2(dependencies[i]);
  }
  return result;
}

}  // namespace

RenderPassManager::RenderPassManager(
    std::shared_ptr<container::gpu::VulkanDevice> device)
    : device_(std::move(device)) {}

RenderPassManager::~RenderPassManager() { destroy(); }

VkFormat RenderPassManager::findSupportedFormat(
    std::initializer_list<VkFormat> candidates, VkImageTiling tiling,
    VkFormatFeatureFlags features) const {
  for (VkFormat fmt : candidates) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device_->physicalDevice(), fmt, &props);
    const VkFormatFeatureFlags tilingFeatures =
        tiling == VK_IMAGE_TILING_LINEAR ? props.linearTilingFeatures
                                         : props.optimalTilingFeatures;
    if ((tilingFeatures & features) == features) return fmt;
  }
  throw std::runtime_error("failed to find a supported Vulkan image format");
}

VkFormat RenderPassManager::findDepthStencilFormat() const {
  return findSupportedFormat(
      {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
      VK_IMAGE_TILING_OPTIMAL,
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
          VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
}

void RenderPassManager::create(VkFormat swapchainFormat,
                               VkFormat depthStencilFormat,
                               VkFormat sceneColorFormat,
                               VkFormat albedoFormat,
                               VkFormat normalFormat,
                               VkFormat materialFormat,
                               VkFormat emissiveFormat,
                               VkFormat specularFormat,
                               VkFormat pickIdFormat,
                               VkFormat materialLayersFormat,
                               VkFormat materialSheenFormat,
                               VkSampleCountFlagBits msaaSamples) {
  VkDevice dev = device_->device();
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(device_->physicalDevice(), &properties);
  if (properties.limits.maxColorAttachments < 8u)
    throw std::runtime_error(
        "Deferred material layers require eight color attachments");
  const bool useMsaa = msaaSamples != VK_SAMPLE_COUNT_1_BIT;
  const VkResolveModeFlagBits depthResolveMode = preferredDepthResolveMode(
      queryRendererMsaaDeviceSupport(device_->physicalDevice())
          .depthResolveModes);

  // ---- Depth Prepass ----
  VkAttachmentDescription ds{};
  ds.format         = depthStencilFormat;
  ds.samples        = VK_SAMPLE_COUNT_1_BIT;
  ds.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  ds.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  ds.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  ds.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ds.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  ds.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference dsRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription depthSubpass{};
  depthSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
  depthSubpass.pDepthStencilAttachment = &dsRef;

  std::array<VkSubpassDependency, 2> depthDeps{};
  depthDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, kDepthTestStages,
      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);
  depthDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      kDepthTestStages, kDepthTestStages,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);

  VkRenderPassCreateInfo rpInfo{};
  rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpInfo.subpassCount    = 1;
  rpInfo.dependencyCount = static_cast<uint32_t>(depthDeps.size());
  rpInfo.pDependencies   = depthDeps.data();
  std::array<VkAttachmentDescription, 2> depthAttachments{};
  VkAttachmentReference depthResolveRef{
      1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescriptionDepthStencilResolve depthResolve{
      VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
  if (useMsaa) {
    depthAttachments[0] = ds;
    depthAttachments[0].samples = msaaSamples;
    depthAttachments[1] = ds;
    depthAttachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthResolve.depthResolveMode = depthResolveMode;
    depthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    auto depthAttachments2 = MakeAttachmentDescriptions2(depthAttachments);
    auto depthDeps2 = MakeDependencies2(depthDeps);
    const VkAttachmentReference2 dsRef2 = MakeAttachmentReference2(dsRef);
    const VkAttachmentReference2 depthResolveRef2 =
        MakeAttachmentReference2(depthResolveRef);
    depthResolve.pDepthStencilResolveAttachment = &depthResolveRef2;
    VkSubpassDescription2 depthSubpass2{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    depthSubpass2.pNext = &depthResolve;
    depthSubpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    depthSubpass2.pDepthStencilAttachment = &dsRef2;
    VkRenderPassCreateInfo2 rpInfo2{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpInfo2.attachmentCount =
        static_cast<uint32_t>(depthAttachments2.size());
    rpInfo2.pAttachments = depthAttachments2.data();
    rpInfo2.subpassCount = 1;
    rpInfo2.pSubpasses = &depthSubpass2;
    rpInfo2.dependencyCount = static_cast<uint32_t>(depthDeps2.size());
    rpInfo2.pDependencies = depthDeps2.data();
    if (createRenderingPass2(dev, &rpInfo2, nullptr,
                            &passes_.depthPrepass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create depth prepass render pass");
    }
  } else {
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &ds;
    rpInfo.pSubpasses = &depthSubpass;
    if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.depthPrepass) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to create depth prepass render pass");
    }
  }

  // ---- BIM Depth Prepass ----
  // BIM geometry extends the same depth buffer after the regular scene depth
  // prepass. Loading depth keeps existing scene occluders intact while allowing
  // BIM surfaces to participate in Hi-Z, GTAO, and deferred lighting.
  VkAttachmentDescription bimDs = ds;
  if (useMsaa) {
    bimDs.samples = msaaSamples;
  }
  bimDs.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  bimDs.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  bimDs.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription bimDepthSubpass = depthSubpass;
  bimDepthSubpass.pDepthStencilAttachment = &dsRef;
  std::array<VkSubpassDependency, 2> bimDepthDeps{};
  bimDepthDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      kDepthTestStages, kDepthTestStages,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);
  bimDepthDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      kDepthTestStages, kDepthTestStages,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);

  std::array<VkAttachmentDescription, 2> bimDepthAttachments{};
  VkSubpassDescriptionDepthStencilResolve bimDepthResolve{
      VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
  if (useMsaa) {
    bimDepthAttachments[0] = bimDs;
    bimDepthAttachments[1] = ds;
    bimDepthAttachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    bimDepthAttachments[1].initialLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    bimDepthAttachments[1].finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    bimDepthResolve.depthResolveMode = depthResolveMode;
    bimDepthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    auto bimDepthAttachments2 =
        MakeAttachmentDescriptions2(bimDepthAttachments);
    auto bimDepthDeps2 = MakeDependencies2(bimDepthDeps);
    const VkAttachmentReference2 dsRef2 = MakeAttachmentReference2(dsRef);
    const VkAttachmentReference2 depthResolveRef2 =
        MakeAttachmentReference2(depthResolveRef);
    bimDepthResolve.pDepthStencilResolveAttachment = &depthResolveRef2;
    VkSubpassDescription2 bimDepthSubpass2{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    bimDepthSubpass2.pNext = &bimDepthResolve;
    bimDepthSubpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    bimDepthSubpass2.pDepthStencilAttachment = &dsRef2;
    VkRenderPassCreateInfo2 rpInfo2{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpInfo2.attachmentCount =
        static_cast<uint32_t>(bimDepthAttachments2.size());
    rpInfo2.pAttachments = bimDepthAttachments2.data();
    rpInfo2.subpassCount = 1;
    rpInfo2.pSubpasses = &bimDepthSubpass2;
    rpInfo2.dependencyCount = static_cast<uint32_t>(bimDepthDeps2.size());
    rpInfo2.pDependencies = bimDepthDeps2.data();
    if (createRenderingPass2(dev, &rpInfo2, nullptr,
                            &passes_.bimDepthPrepass) != VK_SUCCESS) {
      throw std::runtime_error(
          "failed to create BIM depth prepass render pass");
    }
  } else {
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &bimDs;
    rpInfo.pSubpasses = &bimDepthSubpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(bimDepthDeps.size());
    rpInfo.pDependencies = bimDepthDeps.data();
    if (createRenderingPass(dev, &rpInfo, nullptr,
                           &passes_.bimDepthPrepass) != VK_SUCCESS) {
      throw std::runtime_error("failed to create BIM depth prepass render pass");
    }
  }

  // ---- GBuffer ----
  auto makeColor = [](VkFormat fmt, VkSampleCountFlagBits samples,
                      VkImageLayout finalLayout) {
    VkAttachmentDescription a{};
    a.format         = fmt;
    a.samples        = samples;
    a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout    = finalLayout;
    return a;
  };

  VkAttachmentDescription gbDs = ds;
  if (useMsaa) {
    gbDs.samples = msaaSamples;
  }
  gbDs.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
  gbDs.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  gbDs.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  gbDs.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  std::array<VkAttachmentDescription, 9> gbAttachments = {
      makeColor(albedoFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(normalFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(materialFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(emissiveFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(specularFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(pickIdFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(materialLayersFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(materialSheenFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      gbDs};
  std::array<VkAttachmentDescription, 17> gbMsaaAttachments = {{
      makeColor(albedoFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(normalFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(materialFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(emissiveFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(specularFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(pickIdFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(materialLayersFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeColor(materialSheenFormat, msaaSamples,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      gbDs,
      makeColor(albedoFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(normalFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(materialFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(emissiveFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(specularFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(materialLayersFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeColor(materialSheenFormat, VK_SAMPLE_COUNT_1_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      ds,
  }};
  for (uint32_t i = 9u; i <= 16u; ++i) {
    gbMsaaAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
  gbMsaaAttachments[16].initialLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  gbMsaaAttachments[16].finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  std::array<VkAttachmentReference, 8> colorRefs = {{
      {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {5, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {6, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {7, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
  }};
  VkAttachmentReference gbDsRef{8, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  std::array<VkAttachmentReference, 8> colorResolveRefs = {{
      {9, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {10, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {11, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {12, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {13, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED},
      {14, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      {15, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
  }};
  VkAttachmentReference gbDepthResolveRef{
      16, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescriptionDepthStencilResolve gbDepthResolve{
      VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};

  VkSubpassDescription gbSubpass{};
  gbSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  gbSubpass.colorAttachmentCount    = static_cast<uint32_t>(colorRefs.size());
  gbSubpass.pColorAttachments       = colorRefs.data();
  gbSubpass.pDepthStencilAttachment = &gbDsRef;

  std::array<VkSubpassDependency, 2> gbDeps{};
  // Entry: depth prepass wrote depth, then Hi-Z compute may have read it.
  // Occlusion culling may also have produced indirect commands for this pass.
  gbDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      kDepthTestStages | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  // Exit: GBuffer colors → shader read (lighting pass),
  //       depth → compute shader read (TileCull, GTAO) + attachment read (lighting).
  gbDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT);

  if (useMsaa) {
    auto gbAttachments2 = MakeAttachmentDescriptions2(gbMsaaAttachments);
    auto colorRefs2 = MakeAttachmentReferences2(colorRefs);
    auto colorResolveRefs2 = MakeAttachmentReferences2(colorResolveRefs);
    auto gbDeps2 = MakeDependencies2(gbDeps);
    const VkAttachmentReference2 gbDsRef2 =
        MakeAttachmentReference2(gbDsRef);
    const VkAttachmentReference2 gbDepthResolveRef2 =
        MakeAttachmentReference2(gbDepthResolveRef);
    gbDepthResolve.depthResolveMode = depthResolveMode;
    gbDepthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    gbDepthResolve.pDepthStencilResolveAttachment = &gbDepthResolveRef2;
    VkSubpassDescription2 gbSubpass2{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    gbSubpass2.pNext = &gbDepthResolve;
    gbSubpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    gbSubpass2.colorAttachmentCount =
        static_cast<uint32_t>(colorRefs2.size());
    gbSubpass2.pColorAttachments = colorRefs2.data();
    gbSubpass2.pResolveAttachments = colorResolveRefs2.data();
    gbSubpass2.pDepthStencilAttachment = &gbDsRef2;
    VkRenderPassCreateInfo2 rpInfo2{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpInfo2.attachmentCount = static_cast<uint32_t>(gbAttachments2.size());
    rpInfo2.pAttachments = gbAttachments2.data();
    rpInfo2.subpassCount = 1;
    rpInfo2.pSubpasses = &gbSubpass2;
    rpInfo2.dependencyCount = static_cast<uint32_t>(gbDeps2.size());
    rpInfo2.pDependencies = gbDeps2.data();
    if (createRenderingPass2(dev, &rpInfo2, nullptr, &passes_.gBuffer) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to create GBuffer render pass");
    }
  } else {
    rpInfo.attachmentCount = static_cast<uint32_t>(gbAttachments.size());
    rpInfo.pAttachments = gbAttachments.data();
    rpInfo.pSubpasses = &gbSubpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(gbDeps.size());
    rpInfo.pDependencies = gbDeps.data();
    if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.gBuffer) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to create GBuffer render pass");
    }
  }

  // ---- BIM GBuffer ----
  // This pass appends BIM geometry into the deferred attachments. Color
  // attachments are loaded from the regular G-buffer and stored back for the
  // lighting pass.
  auto makeLoadedColor = [](VkFormat fmt, VkSampleCountFlagBits samples,
                            VkImageLayout initialLayout,
                            VkImageLayout finalLayout) {
    VkAttachmentDescription a{};
    a.format         = fmt;
    a.samples        = samples;
    a.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout  = initialLayout;
    a.finalLayout    = finalLayout;
    return a;
  };

  VkAttachmentDescription bimGbDs = gbDs;
  bimGbDs.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  bimGbDs.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  bimGbDs.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  std::array<VkAttachmentDescription, 9> bimGbAttachments = {
      makeLoadedColor(albedoFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(normalFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(materialFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(emissiveFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(specularFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(pickIdFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(materialLayersFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(materialSheenFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      bimGbDs};
  std::array<VkAttachmentDescription, 17> bimGbMsaaAttachments = {{
      makeLoadedColor(albedoFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(normalFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(materialFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(emissiveFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(specularFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(pickIdFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(materialLayersFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      makeLoadedColor(materialSheenFormat, msaaSamples,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
      bimGbDs,
      makeLoadedColor(albedoFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(normalFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(materialFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(emissiveFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(specularFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(materialLayersFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      makeLoadedColor(materialSheenFormat, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      ds,
  }};
  for (uint32_t i = 9u; i <= 16u; ++i) {
    bimGbMsaaAttachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
  bimGbMsaaAttachments[16].initialLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  bimGbMsaaAttachments[16].finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  std::array<VkSubpassDependency, 2> bimGbDeps{};
  bimGbDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
  bimGbDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
      VK_ACCESS_SHADER_READ_BIT);

  if (useMsaa) {
    auto bimGbAttachments2 =
        MakeAttachmentDescriptions2(bimGbMsaaAttachments);
    auto colorRefs2 = MakeAttachmentReferences2(colorRefs);
    auto colorResolveRefs2 = MakeAttachmentReferences2(colorResolveRefs);
    auto bimGbDeps2 = MakeDependencies2(bimGbDeps);
    const VkAttachmentReference2 gbDsRef2 =
        MakeAttachmentReference2(gbDsRef);
    const VkAttachmentReference2 gbDepthResolveRef2 =
        MakeAttachmentReference2(gbDepthResolveRef);
    VkSubpassDescriptionDepthStencilResolve bimGbDepthResolve{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    bimGbDepthResolve.depthResolveMode = depthResolveMode;
    bimGbDepthResolve.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    bimGbDepthResolve.pDepthStencilResolveAttachment =
        &gbDepthResolveRef2;
    VkSubpassDescription2 bimGbSubpass2{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    bimGbSubpass2.pNext = &bimGbDepthResolve;
    bimGbSubpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    bimGbSubpass2.colorAttachmentCount =
        static_cast<uint32_t>(colorRefs2.size());
    bimGbSubpass2.pColorAttachments = colorRefs2.data();
    bimGbSubpass2.pResolveAttachments = colorResolveRefs2.data();
    bimGbSubpass2.pDepthStencilAttachment = &gbDsRef2;
    VkRenderPassCreateInfo2 rpInfo2{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpInfo2.attachmentCount =
        static_cast<uint32_t>(bimGbAttachments2.size());
    rpInfo2.pAttachments = bimGbAttachments2.data();
    rpInfo2.subpassCount = 1;
    rpInfo2.pSubpasses = &bimGbSubpass2;
    rpInfo2.dependencyCount = static_cast<uint32_t>(bimGbDeps2.size());
    rpInfo2.pDependencies = bimGbDeps2.data();
    if (createRenderingPass2(dev, &rpInfo2, nullptr,
                            &passes_.bimGBuffer) != VK_SUCCESS) {
      throw std::runtime_error("failed to create BIM GBuffer render pass");
    }
  } else {
    rpInfo.attachmentCount =
        static_cast<uint32_t>(bimGbAttachments.size());
    rpInfo.pAttachments = bimGbAttachments.data();
    rpInfo.pSubpasses = &gbSubpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(bimGbDeps.size());
    rpInfo.pDependencies = bimGbDeps.data();
    if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.bimGBuffer) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to create BIM GBuffer render pass");
    }
  }

  // ---- Transparent Picking ----
  // Transparent picking reuses the opaque pick ID color target and renders
  // transparent fragments against a copy of the opaque depth buffer. Depth
  // writes are enabled in the pipeline, so the final ID at each pixel belongs
  // to the nearest visible transparent fragment, if one exists.
  VkAttachmentDescription pickColor{};
  pickColor.format = pickIdFormat;
  pickColor.samples = VK_SAMPLE_COUNT_1_BIT;
  pickColor.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  pickColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  pickColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  pickColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  pickColor.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  pickColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentDescription pickDepth = ds;
  pickDepth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  pickDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  pickDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  pickDepth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  pickDepth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  pickDepth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  std::array<VkAttachmentDescription, 2> transparentPickAttachments = {
      pickColor, pickDepth};
  VkAttachmentReference pickColorRef{
      0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference pickDepthRef{
      1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription transparentPickSubpass{};
  transparentPickSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  transparentPickSubpass.colorAttachmentCount = 1;
  transparentPickSubpass.pColorAttachments = &pickColorRef;
  transparentPickSubpass.pDepthStencilAttachment = &pickDepthRef;

  std::array<VkSubpassDependency, 2> transparentPickDeps{};
  transparentPickDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);
  transparentPickDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      kFramebufferLocalDependency);

  rpInfo.attachmentCount =
      static_cast<uint32_t>(transparentPickAttachments.size());
  rpInfo.pAttachments = transparentPickAttachments.data();
  rpInfo.pSubpasses = &transparentPickSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(transparentPickDeps.size());
  rpInfo.pDependencies = transparentPickDeps.data();
  if (createRenderingPass(dev, &rpInfo, nullptr,
                         &passes_.transparentPick) != VK_SUCCESS) {
    throw std::runtime_error("failed to create transparent pick render pass");
  }

  // ---- Lighting ----
  VkAttachmentDescription lightColor{};
  lightColor.format         = sceneColorFormat;
  lightColor.samples        = VK_SAMPLE_COUNT_1_BIT;
  lightColor.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  lightColor.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  lightColor.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  lightColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  lightColor.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  lightColor.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentDescription lightDs = ds;
  lightDs.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
  // Stencil is cleared per-light-volume via vkCmdClearAttachments; no need to
  // preserve stale values from the GBuffer pass.
  lightDs.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  lightDs.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // After GBuffer, depth is transitioned to DEPTH_READ_ONLY_STENCIL_ATTACHMENT
  // for TileCull/GTAO compute reads; the lighting pass picks it up in that layout.
  lightDs.initialLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  lightDs.finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

  std::array<VkAttachmentDescription, 2> lightAttachments = {lightColor, lightDs};
  VkAttachmentReference lightColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference lightDsRef{1, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription lightSubpass{};
  lightSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  lightSubpass.colorAttachmentCount    = 1;
  lightSubpass.pColorAttachments       = &lightColorRef;
  lightSubpass.pDepthStencilAttachment = &lightDsRef;

  std::array<VkSubpassDependency, 2> lightDeps{};
  // Entry: preceding passes provide GBuffer textures, read-only depth,
  // tile-light grids, AO, and cleared OIT storage for transparent fragments.
  lightDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  lightDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | kDepthTestStages |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

  rpInfo.attachmentCount = static_cast<uint32_t>(lightAttachments.size());
  rpInfo.pAttachments    = lightAttachments.data();
  rpInfo.pSubpasses      = &lightSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(lightDeps.size());
  rpInfo.pDependencies   = lightDeps.data();
  if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.lighting) != VK_SUCCESS)
    throw std::runtime_error("failed to create lighting render pass");

  // Forward opaque shading must depth-test at the prepass sample locations.
  // Testing pixel-center depth against a MAX-resolved depth rejects sloped
  // surfaces. Resolve shaded HDR color instead; transparency then uses the
  // single-sample targets shared with OIT and post processing.
  auto forwardColor = lightColor;
  forwardColor.samples = msaaSamples;
  if (useMsaa) {
    forwardColor.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    forwardColor.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  auto forwardDepth = lightDs;
  forwardDepth.samples = msaaSamples;
  forwardDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  forwardDepth.initialLayout = useMsaa
      ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
      : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  forwardDepth.finalLayout =
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  auto forwardResolve = lightColor;
  forwardResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  const std::array forwardAttachments{forwardColor, forwardDepth,
                                       forwardResolve};
  const VkAttachmentReference forwardResolveRef{
      2u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  auto forwardSubpass = lightSubpass;
  forwardSubpass.pResolveAttachments = useMsaa ? &forwardResolveRef : nullptr;
  rpInfo.attachmentCount = useMsaa ? 3u : 2u;
  rpInfo.pAttachments = forwardAttachments.data();
  rpInfo.pSubpasses = &forwardSubpass;
  if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.forwardLighting) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create forward lighting pass");
  }

  auto forwardTransparentColor = lightColor;
  forwardTransparentColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  forwardTransparentColor.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  auto forwardTransparentDepth = lightDs;
  forwardTransparentDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  const std::array forwardTransparentAttachments{
      forwardTransparentColor, forwardTransparentDepth};
  rpInfo.attachmentCount = 2u;
  rpInfo.pAttachments = forwardTransparentAttachments.data();
  rpInfo.pSubpasses = &lightSubpass;
  if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.forwardTransparent) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create forward transparency pass");
  }

  // ---- Transform Gizmos ----
  // Renderer-native transform handles are composited after lighting into the
  // HDR scene color target and leave it shader-readable for exposure, bloom,
  // OIT resolve, and post-process passes.
  VkAttachmentDescription gizmoColor{};
  gizmoColor.format = sceneColorFormat;
  gizmoColor.samples = VK_SAMPLE_COUNT_1_BIT;
  gizmoColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  gizmoColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  gizmoColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  gizmoColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  gizmoColor.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  gizmoColor.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference gizmoColorRef{
      0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription gizmoSubpass{};
  gizmoSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  gizmoSubpass.colorAttachmentCount = 1;
  gizmoSubpass.pColorAttachments = &gizmoColorRef;

  std::array<VkSubpassDependency, 2> gizmoDeps{};
  gizmoDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      kFramebufferLocalDependency);
  gizmoDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT,
      kFramebufferLocalDependency);

  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments = &gizmoColor;
  rpInfo.subpassCount = 1;
  rpInfo.pSubpasses = &gizmoSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(gizmoDeps.size());
  rpInfo.pDependencies = gizmoDeps.data();
  if (createRenderingPass(dev, &rpInfo, nullptr,
                         &passes_.transformGizmos) != VK_SUCCESS) {
    throw std::runtime_error("failed to create transform gizmo render pass");
  }

  // ---- Shadow Depth ----
  // Depth-only render pass for shadow map cascades. Uses the same depth format
  // as the main scene but renders into a 2D array layer per cascade.
  VkAttachmentDescription shadowDs{};
  shadowDs.format         = depthStencilFormat;
  shadowDs.samples        = VK_SAMPLE_COUNT_1_BIT;
  shadowDs.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  shadowDs.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  shadowDs.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  shadowDs.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  shadowDs.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  shadowDs.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference shadowDsRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription shadowSubpass{};
  shadowSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
  shadowSubpass.pDepthStencilAttachment = &shadowDsRef;

  std::array<VkSubpassDependency, 2> shadowDeps{};
  shadowDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      kDepthTestStages | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
          VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  shadowDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      kDepthTestStages,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT);

  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments    = &shadowDs;
  rpInfo.subpassCount    = 1;
  rpInfo.pSubpasses      = &shadowSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(shadowDeps.size());
  rpInfo.pDependencies   = shadowDeps.data();
  if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.shadow) != VK_SUCCESS)
    throw std::runtime_error("failed to create shadow render pass");

  // ---- Post Process ----
  VkAttachmentDescription swapAttachment{};
  swapAttachment.format         = swapchainFormat;
  swapAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
  swapAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  swapAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  swapAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swapAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swapAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  swapAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference swapRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription postSubpass{};
  postSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
  postSubpass.colorAttachmentCount = 1;
  postSubpass.pColorAttachments    = &swapRef;

  std::array<VkSubpassDependency, 2> postDeps{};
  postDeps[0] = MakeDependency(
      VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
  postDeps[1] = MakeDependency(
      0, VK_SUBPASS_EXTERNAL,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);

  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments    = &swapAttachment;
  rpInfo.pSubpasses      = &postSubpass;
  rpInfo.dependencyCount = static_cast<uint32_t>(postDeps.size());
  rpInfo.pDependencies   = postDeps.data();
  if (createRenderingPass(dev, &rpInfo, nullptr, &passes_.postProcess) != VK_SUCCESS)
    throw std::runtime_error("failed to create post-process render pass");
}

void RenderPassManager::destroy() {
  VkDevice dev = device_->device();
  auto destroyPass = [&](RenderingPassHandle& rp) {
    if (rp != VK_NULL_HANDLE) {
      destroyRenderingPass(dev, rp, nullptr);
      rp = VK_NULL_HANDLE;
    }
  };
  destroyPass(passes_.depthPrepass);
  destroyPass(passes_.bimDepthPrepass);
  destroyPass(passes_.gBuffer);
  destroyPass(passes_.bimGBuffer);
  destroyPass(passes_.transparentPick);
  destroyPass(passes_.shadow);
  destroyPass(passes_.lighting);
  destroyPass(passes_.forwardLighting);
  destroyPass(passes_.forwardTransparent);
  destroyPass(passes_.transformGizmos);
  destroyPass(passes_.postProcess);
}

}  // namespace container::renderer
