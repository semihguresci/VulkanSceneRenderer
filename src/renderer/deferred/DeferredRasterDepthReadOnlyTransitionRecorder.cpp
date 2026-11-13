#include "Container/renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.h"

namespace container::renderer {

namespace {

constexpr VkImageAspectFlags kDepthStencilAspects =
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
constexpr VkPipelineStageFlags kDepthStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkPipelineStageFlags kDepthReadStages =
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

VkImageMemoryBarrier makeDepthToReadOnlyBarrier(VkImage image,
                                                uint32_t layerCount,
                                                VkImageLayout oldLayout,
                                                VkImageLayout newLayout,
                                                VkAccessFlags srcAccess,
                                                VkAccessFlags dstAccess) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {kDepthStencilAspects, 0, 1, 0, layerCount};
  return barrier;
}

void appendStep(DeferredRasterDepthReadOnlyTransitionPlan &plan,
                DeferredRasterImageBarrierStep step) {
  if (plan.stepCount >= plan.steps.size()) {
    return;
  }
  plan.steps[plan.stepCount++] = step;
}

} // namespace

DeferredRasterDepthReadOnlyTransitionPlan
buildDeferredRasterDepthReadOnlyTransitionPlan(
    const DeferredRasterDepthReadOnlyTransitionInputs &inputs) {
  if (inputs.depthStencilImage == VK_NULL_HANDLE) {
    return {};
  }

  DeferredRasterDepthReadOnlyTransitionPlan plan{};
  appendStep(
      plan,
      {.srcStageMask = kDepthStages,
       .dstStageMask = kDepthReadStages,
       .barrier = makeDepthToReadOnlyBarrier(
           inputs.depthStencilImage, 1u,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
           VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           VK_ACCESS_SHADER_READ_BIT |
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)});

  if (!inputs.shadowAtlasVisible &&
      inputs.shadowAtlasImage != VK_NULL_HANDLE &&
      inputs.shadowCascadeCount > 0u) {
    appendStep(plan,
               {.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                .barrier = makeDepthToReadOnlyBarrier(
                    inputs.shadowAtlasImage, inputs.shadowCascadeCount,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                    VK_ACCESS_SHADER_READ_BIT)});
  }

  if (!inputs.localShadowAtlasVisible &&
      inputs.localShadowAtlasImage != VK_NULL_HANDLE &&
      inputs.localShadowLayerCount > 0u) {
    appendStep(plan,
               {.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                .barrier = makeDepthToReadOnlyBarrier(
                    inputs.localShadowAtlasImage, inputs.localShadowLayerCount,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                    VK_ACCESS_SHADER_READ_BIT)});
  }

  return plan;
}

bool recordDeferredRasterDepthReadOnlyTransitionCommands(
    VkCommandBuffer cmd,
    const DeferredRasterDepthReadOnlyTransitionPlan &plan) {
  if (plan.stepCount == 0u) {
    return false;
  }

  const uint32_t stepCount =
      plan.stepCount <= plan.steps.size()
          ? plan.stepCount
          : static_cast<uint32_t>(plan.steps.size());
  return recordDeferredRasterImageBarrierSteps(
      cmd, std::span<const DeferredRasterImageBarrierStep>(plan.steps.data(),
                                                           stepCount));
}

} // namespace container::renderer
