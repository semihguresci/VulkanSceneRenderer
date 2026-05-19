#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"
#include "Container/utility/VulkanMemoryManager.h"

namespace container::renderer {

class GpuCullManager;

struct SceneOpaqueDrawGeometryBinding {
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct SceneOpaqueDrawPipelineHandles {
  VkPipeline primary{VK_NULL_HANDLE};
  VkPipeline frontCull{VK_NULL_HANDLE};
  VkPipeline noCull{VK_NULL_HANDLE};
};

struct SceneOpaqueDrawRecordInputs {
  const SceneOpaqueDrawPlan *plan{nullptr};
  SceneOpaqueDrawGeometryBinding geometry{};
  SceneOpaqueDrawPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  uint32_t imageIndex{0};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  const GpuCullManager *gpuCullManager{nullptr};
};

[[nodiscard]] bool recordSceneOpaqueDrawCommands(
    VkCommandBuffer cmd, const SceneOpaqueDrawRecordInputs &inputs);

} // namespace container::renderer
