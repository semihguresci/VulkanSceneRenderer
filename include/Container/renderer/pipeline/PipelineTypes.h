#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"

#include <memory>
#include <vector>

namespace container::renderer {

// All Vulkan pipeline layouts created by GraphicsPipelineBuilder.
struct PipelineLayouts {
  std::shared_ptr<const PipelineRegistry> layoutRegistry{};

  VkPipelineLayout scene{VK_NULL_HANDLE};
  VkPipelineLayout transparent{VK_NULL_HANDLE};
  VkPipelineLayout lighting{VK_NULL_HANDLE};
  VkPipelineLayout lightGizmo{VK_NULL_HANDLE};
  VkPipelineLayout tiledLighting{VK_NULL_HANDLE};
  VkPipelineLayout shadow{VK_NULL_HANDLE};
  VkPipelineLayout postProcess{VK_NULL_HANDLE};
  VkPipelineLayout wireframe{VK_NULL_HANDLE};
  VkPipelineLayout normalValidation{VK_NULL_HANDLE};
  VkPipelineLayout surfaceNormal{VK_NULL_HANDLE};
  VkPipelineLayout transformGizmo{VK_NULL_HANDLE};
  std::vector<RegisteredPipelineLayout> extraLayouts{};
};

[[nodiscard]] std::shared_ptr<const PipelineRegistry>
buildGraphicsPipelineLayoutRegistry(const PipelineLayouts &layouts);

// All Vulkan pipelines created by GraphicsPipelineBuilder.
struct GraphicsPipelines {
  std::shared_ptr<const PipelineRegistry> handleRegistry{};
  std::shared_ptr<const PipelineRegistry> layoutRegistry{};

  // Depth/G-buffer/shadow pipelines are split by culling mode so double-sided
  // materials can disable culling, while mirrored transforms can invert the
  // cull side after glTF triangle winding has been repaired at load time.
  VkPipeline depthPrepass{VK_NULL_HANDLE};
  VkPipeline depthPrepassFrontCull{VK_NULL_HANDLE};
  VkPipeline depthPrepassNoCull{VK_NULL_HANDLE};
  VkPipeline bimDepthPrepass{VK_NULL_HANDLE};
  VkPipeline bimDepthPrepassFrontCull{VK_NULL_HANDLE};
  VkPipeline bimDepthPrepassNoCull{VK_NULL_HANDLE};
  VkPipeline gBuffer{VK_NULL_HANDLE};
  VkPipeline gBufferFrontCull{VK_NULL_HANDLE};
  VkPipeline gBufferNoCull{VK_NULL_HANDLE};
  VkPipeline bimGBuffer{VK_NULL_HANDLE};
  VkPipeline bimGBufferFrontCull{VK_NULL_HANDLE};
  VkPipeline bimGBufferNoCull{VK_NULL_HANDLE};
  VkPipeline shadowDepth{VK_NULL_HANDLE};
  VkPipeline shadowDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline shadowDepthNoCull{VK_NULL_HANDLE};
  VkPipeline localShadowDepth{VK_NULL_HANDLE};
  VkPipeline localShadowDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline localShadowDepthNoCull{VK_NULL_HANDLE};
  VkPipeline directionalLight{VK_NULL_HANDLE};
  VkPipeline stencilVolume{VK_NULL_HANDLE};
  VkPipeline pointLight{VK_NULL_HANDLE};
  VkPipeline pointLightStencilDebug{VK_NULL_HANDLE};
  VkPipeline tiledPointLight{VK_NULL_HANDLE};
  VkPipeline transparent{VK_NULL_HANDLE};
  VkPipeline transparentFrontCull{VK_NULL_HANDLE};
  VkPipeline transparentNoCull{VK_NULL_HANDLE};
  VkPipeline transparentPick{VK_NULL_HANDLE};
  VkPipeline transparentPickFrontCull{VK_NULL_HANDLE};
  VkPipeline transparentPickNoCull{VK_NULL_HANDLE};
  VkPipeline postProcess{VK_NULL_HANDLE};
  VkPipeline geometryDebug{VK_NULL_HANDLE};
  VkPipeline normalValidation{VK_NULL_HANDLE};
  VkPipeline normalValidationFrontCull{VK_NULL_HANDLE};
  VkPipeline normalValidationNoCull{VK_NULL_HANDLE};
  VkPipeline wireframeDepth{VK_NULL_HANDLE};
  VkPipeline wireframeDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepth{VK_NULL_HANDLE};
  VkPipeline wireframeNoDepthFrontCull{VK_NULL_HANDLE};
  VkPipeline selectionMask{VK_NULL_HANDLE};
  VkPipeline selectionOutline{VK_NULL_HANDLE};
  VkPipeline bimFloorPlanDepth{VK_NULL_HANDLE};
  VkPipeline bimFloorPlanNoDepth{VK_NULL_HANDLE};
  VkPipeline bimPointCloudDepth{VK_NULL_HANDLE};
  VkPipeline bimPointCloudNoDepth{VK_NULL_HANDLE};
  VkPipeline bimCurveDepth{VK_NULL_HANDLE};
  VkPipeline bimCurveNoDepth{VK_NULL_HANDLE};
  VkPipeline bimSectionClipCapFill{VK_NULL_HANDLE};
  VkPipeline bimSectionClipCapHatch{VK_NULL_HANDLE};
  VkPipeline surfaceNormalLine{VK_NULL_HANDLE};
  VkPipeline objectNormalDebug{VK_NULL_HANDLE};
  VkPipeline objectNormalDebugFrontCull{VK_NULL_HANDLE};
  VkPipeline objectNormalDebugNoCull{VK_NULL_HANDLE};
  VkPipeline lightGizmo{VK_NULL_HANDLE};
  VkPipeline lightGizmoCoverage{VK_NULL_HANDLE};
  VkPipeline lightGizmoPick{VK_NULL_HANDLE};
  VkPipeline transformGizmo{VK_NULL_HANDLE};
  VkPipeline transformGizmoSolid{VK_NULL_HANDLE};
  VkPipeline transformGizmoOverlay{VK_NULL_HANDLE};
  VkPipeline transformGizmoSolidOverlay{VK_NULL_HANDLE};
  std::vector<RegisteredPipelineHandle> extraHandles{};
};

[[nodiscard]] std::shared_ptr<const PipelineRegistry>
buildGraphicsPipelineHandleRegistry(const GraphicsPipelines &pipelines);

// Input descriptor-set layouts required to build the pipeline layouts.
struct PipelineDescriptorLayouts {
  VkDescriptorSetLayout scene{VK_NULL_HANDLE};
  VkDescriptorSetLayout lighting{VK_NULL_HANDLE};
  VkDescriptorSetLayout light{VK_NULL_HANDLE};
  VkDescriptorSetLayout lightGizmo{VK_NULL_HANDLE};
  VkDescriptorSetLayout tiled{VK_NULL_HANDLE};
  VkDescriptorSetLayout shadow{VK_NULL_HANDLE};
  VkDescriptorSetLayout postProcess{VK_NULL_HANDLE};
  VkDescriptorSetLayout oit{VK_NULL_HANDLE};
};

// Input render passes required to create the pipelines.
struct PipelineRenderPasses {
  RenderingPassHandle depthPrepass{VK_NULL_HANDLE};
  RenderingPassHandle bimDepthPrepass{VK_NULL_HANDLE};
  RenderingPassHandle gBuffer{VK_NULL_HANDLE};
  RenderingPassHandle bimGBuffer{VK_NULL_HANDLE};
  RenderingPassHandle transparentPick{VK_NULL_HANDLE};
  RenderingPassHandle shadow{VK_NULL_HANDLE};
  RenderingPassHandle lighting{VK_NULL_HANDLE};
  RenderingPassHandle transformGizmos{VK_NULL_HANDLE};
  RenderingPassHandle postProcess{VK_NULL_HANDLE};
  RenderingPassHandle forwardLighting{VK_NULL_HANDLE};
};

// All outputs returned by a single build() call.
struct PipelineBuildResult {
  PipelineLayouts layouts;
  GraphicsPipelines pipelines;
};

} // namespace container::renderer
