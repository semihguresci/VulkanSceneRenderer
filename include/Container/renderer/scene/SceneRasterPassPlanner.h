#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"
#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"
#include "Container/renderer/scene/SceneRasterPassRecorder.h"

namespace container::renderer {

struct SceneRasterPassPipelineInputs {
  VkPipeline primary{VK_NULL_HANDLE};
  VkPipeline frontCull{VK_NULL_HANDLE};
  VkPipeline noCull{VK_NULL_HANDLE};
};

struct SceneRasterPassPlanInputs {
  SceneRasterPassKind kind{SceneRasterPassKind::DepthPrepass};
  bool gpuIndirectAvailable{false};
  bool occludedGpuIndirectAvailable{false};
  bool preferOccludedGpuIndirect{false};
  SceneOpaqueDrawLists draws{};
  SceneRasterPassPipelineInputs pipelines{};
};

struct SceneRasterPassPlan {
  SceneRasterPassClearValues clearValues{};
  SceneOpaqueDrawPlan drawPlan{};
  SceneOpaqueDrawPipelineHandles pipelines{};
};

[[nodiscard]] SceneRasterPassPlan
buildSceneRasterPassPlan(const SceneRasterPassPlanInputs &inputs);

} // namespace container::renderer
