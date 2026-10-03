#include "Container/common/DynamicRendering.h"
#include "Container/common/VulkanObjects.h"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace {
std::mutex metadataMutex;
std::unordered_map<VkImageView, VkImageViewCreateInfo> imageViews;
struct ActiveRendering {
  RenderingPassHandle pass;
  RenderingTargetHandle target;
};
std::unordered_map<VkCommandBuffer, ActiveRendering> activeRendering;

bool hasStencil(VkFormat format) {
  return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
         format == VK_FORMAT_D24_UNORM_S8_UINT ||
         format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_S8_UINT;
}

struct LayoutAccess {
  VkPipelineStageFlags2 stages;
  VkAccessFlags2 access;
};
LayoutAccess layoutAccess(VkImageLayout layout) {
  switch (layout) {
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
  case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
  case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
  default:
    // Discarding contents does not discard dependencies on previous users.
    return {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
  }
}

void finishPassDescription(container::gpu::RenderingPass &pass) {
  for (const auto &color : pass.colors) {
    pass.colorFormats.push_back(
        color.attachment == VK_ATTACHMENT_UNUSED
            ? VK_FORMAT_UNDEFINED
            : pass.attachments.at(color.attachment).format);
    if (color.attachment != VK_ATTACHMENT_UNUSED)
      pass.samples = pass.attachments.at(color.attachment).samples;
  }
  if (pass.depth.attachment != VK_ATTACHMENT_UNUSED) {
    const auto &depth = pass.attachments.at(pass.depth.attachment);
    pass.depthFormat = depth.format;
    pass.stencilFormat =
        hasStencil(depth.format) ? depth.format : VK_FORMAT_UNDEFINED;
    pass.samples = depth.samples;
  }
}

VkImageLayout attachmentLayout(const container::gpu::RenderingPass &pass,
                               uint32_t index) {
  for (const auto &color : pass.colors)
    if (color.attachment == index)
      return color.layout;
  for (const auto &resolve : pass.resolves)
    if (resolve.attachment == index)
      return resolve.layout;
  if (pass.depth.attachment == index)
    return pass.depth.layout;
  if (pass.depthResolve.attachment == index)
    return pass.depthResolve.layout;
  return pass.attachments[index].finalLayout;
}

void transitionAttachments(VkCommandBuffer cmd, const ActiveRendering &active,
                           bool begin) {
  std::vector<VkImageMemoryBarrier2> barriers;
  for (uint32_t index = 0; index < active.pass->attachments.size(); ++index) {
    const auto view = active.target->attachments.at(index);
    const auto &description = imageViews.at(view);
    const auto &attachment = active.pass->attachments[index];
    const VkImageLayout inside = attachmentLayout(*active.pass, index);
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.oldLayout = begin ? attachment.initialLayout : inside;
    barrier.newLayout = begin ? inside : attachment.finalLayout;
    const auto source = layoutAccess(barrier.oldLayout);
    const auto destination = layoutAccess(barrier.newLayout);
    barrier.srcStageMask = source.stages;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destination.stages;
    barrier.dstAccessMask = destination.access;
    if (index == active.pass->depthResolve.attachment) {
      // Dynamic-rendering depth/stencil resolves execute at color output,
      // including when the destination image has a depth/stencil format.
      barrier.srcStageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      barrier.srcAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      barrier.dstStageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      barrier.dstAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = description.image;
    barrier.subresourceRange = description.subresourceRange;
    if (hasStencil(description.format) &&
        (barrier.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT))
      barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    barriers.push_back(barrier);
  }
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
  dependency.pImageMemoryBarriers = barriers.data();
  vkCmdPipelineBarrier2(cmd, &dependency);
}

VkRenderingAttachmentInfo renderingAttachment(const RenderingBeginInfo &begin,
                                              VkAttachmentReference reference,
                                              bool stencil = false) {
  VkRenderingAttachmentInfo result{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (reference.attachment == VK_ATTACHMENT_UNUSED)
    return result;
  const auto &attachment =
      begin.renderPass->attachments.at(reference.attachment);
  result.imageView = begin.framebuffer->attachments.at(reference.attachment);
  result.imageLayout = reference.layout;
  result.loadOp = stencil ? attachment.stencilLoadOp : attachment.loadOp;
  result.storeOp = stencil ? attachment.stencilStoreOp : attachment.storeOp;
  if (reference.attachment < begin.clearValueCount)
    result.clearValue = begin.pClearValues[reference.attachment];
  return result;
}
} // namespace

VkPipelineRenderingCreateInfo
container::gpu::RenderingPass::pipelineInfo() const {
  VkPipelineRenderingCreateInfo info{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  info.viewMask = viewMask;
  info.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
  info.pColorAttachmentFormats = colorFormats.data();
  info.depthAttachmentFormat = depthFormat;
  info.stencilAttachmentFormat = stencilFormat;
  return info;
}

VkCommandBufferInheritanceRenderingInfo
container::gpu::RenderingPass::inheritanceInfo() const {
  VkCommandBufferInheritanceRenderingInfo info{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
  info.viewMask = viewMask;
  info.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
  info.pColorAttachmentFormats = colorFormats.data();
  info.depthAttachmentFormat = depthFormat;
  info.stencilAttachmentFormat = stencilFormat;
  info.rasterizationSamples = samples;
  return info;
}

VkResult createRenderingPass(VkDevice, const VkRenderPassCreateInfo *info,
                             const VkAllocationCallbacks *,
                             RenderingPassHandle *result) {
  if (!info || info->subpassCount != 1u)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto pass = std::make_unique<container::gpu::RenderingPass>();
  pass->attachments.assign(info->pAttachments,
                           info->pAttachments + info->attachmentCount);
  const auto &subpass = info->pSubpasses[0];
  if (subpass.colorAttachmentCount) {
    pass->colors.assign(subpass.pColorAttachments,
                        subpass.pColorAttachments +
                            subpass.colorAttachmentCount);
    if (subpass.pResolveAttachments)
      pass->resolves.assign(subpass.pResolveAttachments,
                            subpass.pResolveAttachments +
                                subpass.colorAttachmentCount);
  }
  if (subpass.pDepthStencilAttachment)
    pass->depth = *subpass.pDepthStencilAttachment;
  finishPassDescription(*pass);
  *result = pass.release();
  return VK_SUCCESS;
}

VkResult createRenderingPass2(VkDevice, const VkRenderPassCreateInfo2 *info,
                              const VkAllocationCallbacks *,
                              RenderingPassHandle *result) {
  if (!info || info->subpassCount != 1u)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto pass = std::make_unique<container::gpu::RenderingPass>();
  for (uint32_t i = 0; i < info->attachmentCount; ++i) {
    const auto &a = info->pAttachments[i];
    pass->attachments.push_back({a.flags, a.format, a.samples, a.loadOp,
                                 a.storeOp, a.stencilLoadOp, a.stencilStoreOp,
                                 a.initialLayout, a.finalLayout});
  }
  const auto &subpass = info->pSubpasses[0];
  pass->viewMask = subpass.viewMask;
  for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i) {
    const auto &color = subpass.pColorAttachments[i];
    pass->colors.push_back({color.attachment, color.layout});
    if (subpass.pResolveAttachments) {
      const auto &resolve = subpass.pResolveAttachments[i];
      pass->resolves.push_back({resolve.attachment, resolve.layout});
    }
  }
  if (subpass.pDepthStencilAttachment)
    pass->depth = {subpass.pDepthStencilAttachment->attachment,
                   subpass.pDepthStencilAttachment->layout};
  for (auto *next = static_cast<const VkBaseInStructure *>(subpass.pNext); next;
       next = next->pNext) {
    if (next->sType ==
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE) {
      const auto &resolve =
          *reinterpret_cast<const VkSubpassDescriptionDepthStencilResolve *>(
              next);
      pass->depthResolveMode = resolve.depthResolveMode;
      pass->stencilResolveMode = resolve.stencilResolveMode;
      if (resolve.pDepthStencilResolveAttachment)
        pass->depthResolve = {
            resolve.pDepthStencilResolveAttachment->attachment,
            resolve.pDepthStencilResolveAttachment->layout};
    }
  }
  finishPassDescription(*pass);
  *result = pass.release();
  return VK_SUCCESS;
}

void destroyRenderingPass(VkDevice, RenderingPassHandle pass,
                          const VkAllocationCallbacks *) {
  delete pass;
}

VkResult createRenderingTarget(VkDevice, const RenderingTargetCreateInfo *info,
                               const VkAllocationCallbacks *,
                               RenderingTargetHandle *result) {
  if (!info || !info->renderPass ||
      info->attachmentCount != info->renderPass->attachments.size())
    return VK_ERROR_INITIALIZATION_FAILED;
  auto target = std::make_unique<container::gpu::RenderingTarget>();
  target->attachments.assign(info->pAttachments,
                             info->pAttachments + info->attachmentCount);
  target->width = info->width;
  target->height = info->height;
  target->layers = info->layers;
  *result = target.release();
  return VK_SUCCESS;
}
void destroyRenderingTarget(VkDevice, RenderingTargetHandle target,
                            const VkAllocationCallbacks *) {
  delete target;
}

void beginDynamicRendering(VkCommandBuffer cmd, const RenderingBeginInfo *begin,
                           VkSubpassContents contents) {
  std::lock_guard lock(metadataMutex);
  ActiveRendering active{begin->renderPass, begin->framebuffer};
  if (activeRendering.contains(cmd))
    throw std::logic_error("nested dynamic rendering scope");
  transitionAttachments(cmd, active, true);
  std::vector<VkRenderingAttachmentInfo> colors;
  for (size_t i = 0; i < active.pass->colors.size(); ++i) {
    auto color = renderingAttachment(*begin, active.pass->colors[i]);
    if (i < active.pass->resolves.size() &&
        active.pass->resolves[i].attachment != VK_ATTACHMENT_UNUSED) {
      const auto resolve = active.pass->resolves[i];
      color.resolveMode = active.pass->colorFormats[i] == VK_FORMAT_R32_UINT
                              ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                              : VK_RESOLVE_MODE_AVERAGE_BIT;
      color.resolveImageView =
          active.target->attachments.at(resolve.attachment);
      color.resolveImageLayout = resolve.layout;
    }
    colors.push_back(color);
  }
  auto depth = renderingAttachment(*begin, active.pass->depth);
  auto stencil = renderingAttachment(*begin, active.pass->depth, true);
  if (active.pass->depthResolve.attachment != VK_ATTACHMENT_UNUSED) {
    depth.resolveMode = active.pass->depthResolveMode;
    stencil.resolveMode = active.pass->stencilResolveMode;
    depth.resolveImageView = stencil.resolveImageView =
        active.target->attachments.at(active.pass->depthResolve.attachment);
    depth.resolveImageLayout = stencil.resolveImageLayout =
        active.pass->depthResolve.layout;
  }
  VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.flags = contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                   ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
                   : 0u;
  info.renderArea = begin->renderArea;
  info.layerCount = active.target->layers;
  info.viewMask = active.pass->viewMask;
  info.colorAttachmentCount = static_cast<uint32_t>(colors.size());
  info.pColorAttachments = colors.data();
  info.pDepthAttachment =
      active.pass->depthFormat != VK_FORMAT_UNDEFINED ? &depth : nullptr;
  info.pStencilAttachment =
      active.pass->stencilFormat != VK_FORMAT_UNDEFINED ? &stencil : nullptr;
  vkCmdBeginRendering(cmd, &info);
  activeRendering.emplace(cmd, active);
}

void endDynamicRendering(VkCommandBuffer cmd) {
  std::lock_guard lock(metadataMutex);
  vkCmdEndRendering(cmd);
  transitionAttachments(cmd, activeRendering.at(cmd), false);
  activeRendering.erase(cmd);
}

VkResult createVulkanImageView(VkDevice device,
                               const VkImageViewCreateInfo *info,
                               const VkAllocationCallbacks *allocator,
                               VkImageView *result) {
  const auto status = createOwnedImageView(device, info, allocator, result);
  if (status == VK_SUCCESS) {
    std::lock_guard lock(metadataMutex);
    imageViews.emplace(*result, *info);
  }
  return status;
}
void destroyVulkanImageView(VkDevice device, VkImageView view,
                            const VkAllocationCallbacks *allocator) {
  std::lock_guard lock(metadataMutex);
  imageViews.erase(view);
  destroyOwnedImageView(device, view, allocator);
}
