#include "Container/renderer/core/ScreenshotCaptureRecorder.h"

namespace container::renderer {

bool hasScreenshotCopyWork(const ScreenshotCopyInputs& inputs) {
  return inputs.commandBuffer != VK_NULL_HANDLE &&
         inputs.sourceImage != VK_NULL_HANDLE &&
         inputs.readbackBuffer != VK_NULL_HANDLE &&
         inputs.extent.width > 0 && inputs.extent.height > 0;
}

void recordScreenshotCaptureCopy(const ScreenshotCopyInputs& inputs) {
  if (!hasScreenshotCopyWork(inputs)) {
    return;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = inputs.sourceImage;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(inputs.commandBuffer,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageExtent = {inputs.extent.width, inputs.extent.height, 1};
  vkCmdCopyImageToBuffer(inputs.commandBuffer, inputs.sourceImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         inputs.readbackBuffer, 1, &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = inputs.readbackBuffer;
  hostRead.offset = 0;
  hostRead.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(inputs.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toPresent = toTransfer;
  toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toPresent.dstAccessMask = 0;
  toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(inputs.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toPresent);
}

void recordLinearCaptureHostBarrier(VkCommandBuffer commandBuffer, VkBuffer readbackBuffer) {
  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = readbackBuffer;
  hostRead.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
}

}  // namespace container::renderer
