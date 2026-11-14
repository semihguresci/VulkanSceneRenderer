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

} // namespace

DeferredRasterDepthReadOnlyTransitionPlan
buildDeferredRasterDepthReadOnlyTransitionPlan(
    const DeferredRasterDepthReadOnlyTransitionInputs &inputs) {
  if (inputs.depthStencilImage == VK_NULL_HANDLE) {
    return {};
  }

  // Shadow atlases enter the sampled layout at creation and at the end of
  // each shadow pass. Diagnostic modes that skip the pass leave them alone.
  DeferredRasterDepthReadOnlyTransitionPlan plan{.stepCount = 1};
  plan.steps[0] = {
      .srcStageMask = kDepthStages,
      .dstStageMask = kDepthReadStages,
      .barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                  .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  .oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  .newLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                  .image = inputs.depthStencilImage,
                  .subresourceRange = {kDepthStencilAspects, 0, 1, 0, 1}}};
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
