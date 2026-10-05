#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderGraph.h"

namespace container::renderer {

[[nodiscard]] bool
hasForwardRasterNativePrimitiveDraws(const FrameRecordParams &p);

[[nodiscard]] RenderPassReadiness
checkForwardRasterLightingPassReadiness(const FrameRecordParams& p);

[[nodiscard]] bool
recordForwardRasterLightingPassCommands(VkCommandBuffer commandBuffer,
                                        const FrameRecordParams& p);

}  // namespace container::renderer
