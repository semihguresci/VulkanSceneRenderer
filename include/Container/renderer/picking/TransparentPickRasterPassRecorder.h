#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"
#include "Container/renderer/picking/TransparentPickPassRecorder.h"
#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"

#include <functional>

namespace container::renderer {

struct TransparentPickRasterPassRecordInputs {
  bool active{false};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  TransparentPickPassRecordInputs pass{};
  bool extraPassWorkActive{false};
  std::function<void(VkCommandBuffer)> recordAfterGeometry{};
};

struct TransparentPickFramePassRecordInputs {
  bool scenePassReady{false};
  bool bimPassReady{false};
  RenderingPassHandle renderPass{VK_NULL_HANDLE};
  RenderingTargetHandle framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  VkImage sourceDepthStencilImage{VK_NULL_HANDLE};
  VkImage pickDepthImage{VK_NULL_HANDLE};
  VkImage pickIdImage{VK_NULL_HANDLE};
  SceneOpaqueDrawLists sceneOpaqueDraws{};
  SceneTransparentDrawLists sceneDraws{};
  BimSurfaceFramePassDrawSources bimOpaqueDraws{};
  BimSurfaceFramePassDrawSources bimDraws{};
  TransparentPickPassGeometryBinding scene{};
  TransparentPickPassGeometryBinding bim{};
  TransparentPickPassPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  const container::gpu::BindlessPushConstants *pushConstants{nullptr};
  uint32_t bimSemanticColorMode{0};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
  bool extraPassWorkActive{false};
  std::function<void(VkCommandBuffer)> recordAfterGeometry{};
};

[[nodiscard]] bool recordTransparentPickRasterPassCommands(
    VkCommandBuffer cmd, const TransparentPickRasterPassRecordInputs &inputs);

[[nodiscard]] bool recordTransparentPickFramePassCommands(
    VkCommandBuffer cmd, const TransparentPickFramePassRecordInputs &inputs);

} // namespace container::renderer
