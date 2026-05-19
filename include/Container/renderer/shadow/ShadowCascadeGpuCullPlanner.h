#pragma once

#include "Container/common/CommonVulkan.h"

#include <cstdint>

namespace container::renderer {

struct ShadowCascadeGpuCullPlanInputs {
  bool gpuShadowCullEnabled{false};
  bool shadowCullPassActive{false};
  bool shadowCullManagerReady{false};
  bool shadowCullDispatchReady{false};
  bool sceneSingleSidedDrawsAvailable{false};
  bool sourceDrawCommandsAllSingleInstance{true};
  bool cascadeIndexInRange{false};
  VkBuffer indirectDrawBuffer{VK_NULL_HANDLE};
  VkBuffer drawCountBuffer{VK_NULL_HANDLE};
  uint32_t maxDrawCount{0u};
};

struct ShadowCascadeGpuCullPlan {
  bool useGpuCull{false};
};

struct ShadowGpuCullSourceUploadPlanInputs {
  bool shadowAtlasVisible{false};
  bool gpuShadowCullEnabled{false};
  bool shadowCullManagerReady{false};
  bool sourceDrawCommandsPresent{false};
  bool sourceDrawCommandsAllSingleInstance{true};
  uint32_t sourceDrawCount{0u};
};

struct ShadowGpuCullSourceUploadPlan {
  bool uploadSourceDrawCommands{false};
  uint32_t requiredDrawCapacity{0u};
};

[[nodiscard]] ShadowCascadeGpuCullPlan
buildShadowCascadeGpuCullPlan(const ShadowCascadeGpuCullPlanInputs &inputs);

[[nodiscard]] ShadowGpuCullSourceUploadPlan
buildShadowGpuCullSourceUploadPlan(
    const ShadowGpuCullSourceUploadPlanInputs &inputs);

} // namespace container::renderer
