#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/scene/SceneDiagnosticCubeRecorder.h"
#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"

#include <array>
#include <cstdint>

namespace container::renderer {

enum class SceneRasterPassKind : uint32_t {
  DepthPrepass = 0,
  GBuffer = 1,
};

struct SceneRasterPassClearValues {
  std::array<VkClearValue, 8> values{};
  uint32_t count{0};
};

struct SceneRasterPassRecordInputs {
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  SceneRasterPassClearValues clearValues{};
  const SceneOpaqueDrawPlan *plan{nullptr};
  SceneOpaqueDrawGeometryBinding geometry{};
  SceneOpaqueDrawPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  uint32_t imageIndex{0};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  const GpuCullManager *gpuCullManager{nullptr};
  SceneDiagnosticCubeRecordInputs diagnosticCube{};
};

[[nodiscard]] SceneRasterPassClearValues
sceneRasterPassClearValues(SceneRasterPassKind kind);

[[nodiscard]] bool
recordSceneRasterPassCommands(VkCommandBuffer cmd,
                              const SceneRasterPassRecordInputs &inputs);

} // namespace container::renderer
