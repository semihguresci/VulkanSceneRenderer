#pragma once

#include "Container/renderer/core/RenderGraph.h"

#include <cstdint>

namespace container::renderer {

struct ShadowCullPassPlanInputs {
  bool shadowAtlasVisible{false};
  bool gpuShadowCullEnabled{false};
  bool shadowCullManagerReady{false};
  bool sceneSingleSidedDrawsAvailable{false};
  bool sourceDrawCommandsAllSingleInstance{true};
  bool cameraBufferReady{false};
  bool cascadeIndexInRange{false};
  uint32_t sourceDrawCount{0u};
};

struct ShadowCullPassPlan {
  bool active{false};
  RenderPassReadiness readiness{};
  uint32_t drawCount{0u};
};

[[nodiscard]] ShadowCullPassPlan
buildShadowCullPassPlan(const ShadowCullPassPlanInputs &inputs);

} // namespace container::renderer
