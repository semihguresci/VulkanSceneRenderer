#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/shadow/ShadowPassRasterPlanner.h"
#include "Container/renderer/shadow/ShadowPassDrawPlanner.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>

namespace container::renderer {

class BimManager;

struct ShadowPassGeometryBinding {
  VkDescriptorSet sceneDescriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct ShadowPassPipelineHandles {
  VkPipeline primary{VK_NULL_HANDLE};
  VkPipeline frontCull{VK_NULL_HANDLE};
  VkPipeline noCull{VK_NULL_HANDLE};
};

struct ShadowPassGpuIndirectBuffers {
  VkBuffer drawBuffer{VK_NULL_HANDLE};
  VkBuffer countBuffer{VK_NULL_HANDLE};
  uint32_t maxDrawCount{0};
};

struct ShadowPassRecordInputs {
  const ShadowPassDrawPlan *plan{nullptr};
  VkExtent2D extent{};
  ShadowPassGeometryBinding scene{};
  ShadowPassGeometryBinding bim{};
  VkDescriptorSet shadowDescriptorSet{VK_NULL_HANDLE};
  ShadowPassPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::ShadowPushConstants pushConstants{};
  float rasterConstantBias{0.0f};
  float rasterSlopeBias{0.0f};
  ShadowPassGpuIndirectBuffers sceneGpuIndirect{};
  const BimManager *bimManager{nullptr};
};

struct ShadowCascadePassRecordInputs {
  bool cascadePassActive{false};
  ShadowPassRasterPlanInputs raster{};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  ShadowPassGeometryBinding scene{};
  ShadowPassGeometryBinding bim{};
  VkDescriptorSet shadowDescriptorSet{VK_NULL_HANDLE};
  ShadowPassPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::ShadowPushConstants pushConstants{};
  float rasterConstantBias{0.0f};
  float rasterSlopeBias{0.0f};
  ShadowPassGpuIndirectBuffers sceneGpuIndirect{};
  const BimManager *bimManager{nullptr};
  ShadowPassDrawInputs drawInputs{};
};

struct ShadowCascadeSecondaryPassRecordInputs {
  bool secondaryCommandBuffersEnabled{false};
  std::array<ShadowCascadePassRecordInputs,
             container::gpu::kShadowCascadeCount>
      cascades{};
};

[[nodiscard]] bool recordShadowPassCommands(
    VkCommandBuffer cmd, const ShadowPassRecordInputs &inputs);

[[nodiscard]] bool recordShadowCascadePassCommands(
    VkCommandBuffer cmd, const ShadowCascadePassRecordInputs &inputs);

void recordShadowCascadeSecondaryPassCommands(
    const ShadowCascadeSecondaryPassRecordInputs &inputs);

} // namespace container::renderer
