#include "Container/renderer/effects/OitManager.h"

#include <array>

namespace container::renderer {

VkBufferMemoryBarrier2 makeOitCounterReadBarrier(VkBuffer counterBuffer) {
  // The retired frame's mapped counter is also read by the host. Include the
  // transfer clear, which is the last write when no fragments reach the pool.
  return {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_CLEAR_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                       VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = counterBuffer,
      .size = VK_WHOLE_SIZE};
}

OitManager::OitManager(std::shared_ptr<container::gpu::VulkanDevice> device)
    : device_(std::move(device)) {}

void OitManager::clearResources(VkCommandBuffer cmd,
                                const OitFrameResources& resources,
                                uint32_t invalidNodeIndex) const {
  if (cmd == VK_NULL_HANDLE ||
      resources.headPointerImage == VK_NULL_HANDLE ||
      resources.counterBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier headClear{};
  headClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  headClear.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headClear.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headClear.image               = resources.headPointerImage;
  headClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  headClear.srcAccessMask       = 0;
  headClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

  VkBufferMemoryBarrier counterClear{};
  counterClear.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  counterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterClear.buffer              = resources.counterBuffer;
  counterClear.offset              = 0;
  counterClear.size                = sizeof(uint32_t);
  counterClear.srcAccessMask       = 0;
  counterClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                       1, &counterClear, 1, &headClear);

  VkClearColorValue clearColor{};
  clearColor.uint32[0] = invalidNodeIndex;
  const VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, resources.headPointerImage,
                       VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);
  vkCmdFillBuffer(cmd, resources.counterBuffer, 0, sizeof(uint32_t), 0u);

  VkImageMemoryBarrier headReady = headClear;
  headReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  headReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  VkBufferMemoryBarrier counterReady = counterClear;
  counterReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  counterReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                       1, &counterReady, 1, &headReady);
}

void OitManager::prepareResolve(VkCommandBuffer cmd,
                                const OitFrameResources& resources) const {
  if (cmd == VK_NULL_HANDLE ||
      resources.headPointerImage == VK_NULL_HANDLE ||
      resources.nodeBuffer == VK_NULL_HANDLE ||
      resources.counterBuffer == VK_NULL_HANDLE) {
    return;
  }

  constexpr VkPipelineStageFlags2 fragment =
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags2 shaderWrite = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  constexpr VkAccessFlags2 shaderReadWrite =
      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | shaderWrite;
  const VkImageMemoryBarrier2 headBarrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = fragment,
      .srcAccessMask = shaderWrite,
      .dstStageMask = fragment,
      .dstAccessMask = shaderReadWrite,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = resources.headPointerImage,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  const std::array<VkBufferMemoryBarrier2, 2> bufferBarriers{{
      {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
       .srcStageMask = fragment,
       .srcAccessMask = shaderWrite,
       .dstStageMask = fragment,
       .dstAccessMask = shaderReadWrite,
       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
       .buffer = resources.nodeBuffer,
       .size = VK_WHOLE_SIZE},
      makeOitCounterReadBarrier(resources.counterBuffer)}};
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size()),
      .pBufferMemoryBarriers = bufferBarriers.data(),
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &headBarrier};
  vkCmdPipelineBarrier2(cmd, &dependency);
}

}  // namespace container::renderer
