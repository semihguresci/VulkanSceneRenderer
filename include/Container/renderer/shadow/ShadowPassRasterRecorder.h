#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/shadow/ShadowPassRasterPlanner.h"

#include <functional>

namespace container::renderer {

using ShadowPassRasterRecordBody = std::function<void(VkCommandBuffer)>;

struct ShadowPassRasterRecordInputs {
  const ShadowPassRasterPlan *plan{nullptr};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  ShadowPassRasterRecordBody recordBody{};
};

[[nodiscard]] bool recordShadowPassRasterCommands(
    VkCommandBuffer cmd, const ShadowPassRasterRecordInputs &inputs);

} // namespace container::renderer
