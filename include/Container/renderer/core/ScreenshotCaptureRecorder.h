#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

struct ScreenshotCopyInputs {
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  VkImage sourceImage{VK_NULL_HANDLE};
  VkBuffer readbackBuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
};

[[nodiscard]] bool hasScreenshotCopyWork(const ScreenshotCopyInputs& inputs);

void recordScreenshotCaptureCopy(const ScreenshotCopyInputs& inputs);
void recordLinearCaptureHostBarrier(VkCommandBuffer commandBuffer, VkBuffer readbackBuffer);

}  // namespace container::renderer
