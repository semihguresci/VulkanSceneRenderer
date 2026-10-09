#include "Container/renderer/deferred/DeferredRasterTechnique.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/renderer/temporal/TemporalManager.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/deferred/DeferredRasterBimSurfacePassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/deferred/DeferredRasterFrustumCullPassPlanner.h"
#include "Container/renderer/deferred/DeferredRasterFrustumCullPassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterHiZDepthTransitionRecorder.h"
#include "Container/renderer/deferred/DeferredRasterHiZPassPlanner.h"
#include "Container/renderer/deferred/DeferredRasterLightingPassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterPipelineBridge.h"
#include "Container/renderer/deferred/DeferredRasterPostProcess.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.h"
#include "Container/renderer/deferred/DeferredRasterScenePassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterTileCullPlanner.h"
#include "Container/renderer/deferred/DeferredRasterTileCullRecorder.h"
#include "Container/renderer/deferred/DeferredRasterTransformGizmo.h"
#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"
#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/effects/ExposureManager.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/picking/TransparentPickRasterPassRecorder.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/SceneViewport.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowCullPassPlanner.h"
#include "Container/renderer/shadow/ShadowCullPassRecorder.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/shadow/ShadowPassRecorder.h"
#include "Container/renderer/shadow/ShadowPipelineBridge.h"
#include "Container/renderer/shadow/ShadowResourceBridge.h"
#include "Container/utility/GuiManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace container::renderer {

using container::gpu::kLocalShadowMapResolution;
using container::gpu::kMaxShadowedLocalLightLayers;
using container::gpu::kShadowCascadeCount;

namespace {

constexpr RenderTechniqueId kDeferredRasterTechnique =
    RenderTechniqueId::DeferredRaster;

void registerDeferredRasterFrameResources(FrameResourceRegistry &registry) {
  registry.clearTechnique(kDeferredRasterTechnique);

  registry.registerExternal(kDeferredRasterTechnique, "swapchain");
  registry.registerExternal(kDeferredRasterTechnique, "scene-geometry");
  registry.registerExternal(kDeferredRasterTechnique, "bim-geometry");
  registry.registerExternal(kDeferredRasterTechnique, "shadow-atlas");
  registry.registerExternal(kDeferredRasterTechnique, "local-shadow-atlas");

  registry.registerBuffer(
      kDeferredRasterTechnique, "camera-buffer",
      FrameBufferDesc{.size = sizeof(container::gpu::CameraData),
                      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
      FrameResourceLifetime::Imported);
  registry.registerBuffer(
      kDeferredRasterTechnique, "scene-object-buffer",
      FrameBufferDesc{.size = 0, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "scene-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "bim-scene-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "light-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "tiled-lighting-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "shadow-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "local-shadow-descriptor-set",
                                 FrameResourceLifetime::Imported);
  registry.registerSampler(kDeferredRasterTechnique, "g-buffer-sampler",
                           FrameSamplerDesc{}, FrameResourceLifetime::Imported);

  registry.registerImage(
      kDeferredRasterTechnique, "depth-stencil",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "depth-sampling-view",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "scene-color",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "albedo",
      FrameImageDesc{.format = VK_FORMAT_R8G8B8A8_UNORM,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "normal",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "material",
      FrameImageDesc{.format = VK_FORMAT_R32G32B32A32_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "emissive",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "specular",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "material-layers",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "material-sheen",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "pick-id",
      FrameImageDesc{.format = VK_FORMAT_R32_UINT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "pick-depth",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kDeferredRasterTechnique, "oit-head-pointers",
      FrameImageDesc{.format = VK_FORMAT_R32_UINT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT});

  registry.registerBuffer(
      kDeferredRasterTechnique, "oit-node-buffer",
      FrameBufferDesc{.size = 0, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerBuffer(
      kDeferredRasterTechnique, "oit-counter-buffer",
      FrameBufferDesc{.size = sizeof(uint32_t),
                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerBuffer(
      kDeferredRasterTechnique, "oit-metadata-buffer",
      FrameBufferDesc{.size = sizeof(OitMetadata),
                      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "frame-lighting-descriptor-set");
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "post-process-descriptor-set");
  registry.registerDescriptorSet(kDeferredRasterTechnique,
                                 "oit-descriptor-set");

  registry.registerFramebuffer(kDeferredRasterTechnique,
                               "depth-prepass-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
  registry.registerFramebuffer(kDeferredRasterTechnique,
                               "bim-depth-prepass-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
  registry.registerFramebuffer(kDeferredRasterTechnique, "gbuffer-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 9u});
  registry.registerFramebuffer(kDeferredRasterTechnique,
                               "bim-gbuffer-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 9u});
  registry.registerFramebuffer(kDeferredRasterTechnique,
                               "transparent-pick-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kDeferredRasterTechnique, "lighting-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kDeferredRasterTechnique,
                               "transform-gizmo-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
}

void registerDeferredRasterPipelineRecipes(PipelineRegistry &registry) {
  registry.clearTechnique(kDeferredRasterTechnique);

  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "depth-prepass"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/depth_prepass.vert.spv",
                                      "spv_shaders/depth_prepass.frag.spv"},
                     .layoutName = "scene"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "gbuffer"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/gbuffer.vert.spv",
                                      "spv_shaders/gbuffer.frag.spv"},
                     .layoutName = "scene"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "bim-depth-prepass"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/depth_prepass.vert.spv",
                                      "spv_shaders/depth_prepass.frag.spv"},
                     .layoutName = "scene"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "bim-gbuffer"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/gbuffer.vert.spv",
                                      "spv_shaders/gbuffer.frag.spv"},
                     .layoutName = "scene"});
  registry.registerRecipe(PipelineRecipe{
      .key = {kDeferredRasterTechnique, "transparent"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/forward_transparent.vert.spv",
                       "spv_shaders/forward_transparent.frag.spv"},
      .layoutName = "transparent"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "transparent-pick"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/transparent_pick.vert.spv",
                                      "spv_shaders/transparent_pick.frag.spv"},
                     .layoutName = "scene"});
  registry.registerRecipe(PipelineRecipe{
      .key = {kDeferredRasterTechnique, "lighting"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/deferred_directional.vert.spv",
                       "spv_shaders/deferred_directional.frag.spv"},
      .layoutName = "lighting"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "shadow-depth"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/shadow_depth.vert.spv",
                                      "spv_shaders/shadow_depth.frag.spv"},
                     .layoutName = "shadow"});
  registry.registerRecipe(PipelineRecipe{
      .key = {kDeferredRasterTechnique, "local-shadow-depth"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"spv_shaders/local_shadow_depth.vert.spv",
                       "spv_shaders/local_shadow_depth.frag.spv"},
      .layoutName = "shadow"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "post-process"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/post_process.vert.spv",
                                      "spv_shaders/post_process.frag.spv"},
                     .layoutName = "post-process"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "transform-gizmo"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/transform_gizmo.vert.spv",
                                      "spv_shaders/transform_gizmo.frag.spv"},
                     .layoutName = "transform-gizmo"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "light-gizmo"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/light_gizmo.vert.spv",
                                      "spv_shaders/light_gizmo.frag.spv"},
                     .layoutName = "light-gizmo"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "light-gizmo-coverage"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/light_gizmo.vert.spv",
                                      "spv_shaders/light_gizmo.frag.spv"},
                     .layoutName = "light-gizmo"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "light-gizmo-pick"},
                     .kind = PipelineRecipeKind::Graphics,
                     .shaderStages = {"spv_shaders/light_gizmo_pick.vert.spv",
                                      "spv_shaders/light_gizmo_pick.frag.spv"},
                     .layoutName = "light-gizmo"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "frustum-cull"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/frustum_cull.comp.spv"},
                     .layoutName = "gpu-cull"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "occlusion-cull"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/occlusion_cull.comp.spv"},
                     .layoutName = "gpu-cull"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "hi-z-generate"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/hiz_generate.comp.spv"},
                     .layoutName = "gpu-cull"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "tile-cull"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/tile_light_cull.comp.spv"},
                     .layoutName = "tiled-lighting"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "gtao"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/gtao.comp.spv",
                                      "spv_shaders/gtao_blur.comp.spv"},
                     .layoutName = "ambient-occlusion"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "bloom"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/bloom_downsample.comp.spv",
                                      "spv_shaders/bloom_upsample.comp.spv"},
                     .layoutName = "bloom"});
  registry.registerRecipe(
      PipelineRecipe{.key = {kDeferredRasterTechnique, "exposure-adaptation"},
                     .kind = PipelineRecipeKind::Compute,
                     .shaderStages = {"spv_shaders/exposure_histogram.comp.spv",
                                      "spv_shaders/exposure_adapt.comp.spv"},
                     .layoutName = "exposure"});
}

BimSurfaceDrawLists
deferredRasterSurfaceDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimSurfaceFramePassDrawSources
deferredRasterBimSurfaceDrawSources(const FrameBimResources &bim) {
  return {.mesh = deferredRasterSurfaceDrawLists(bim.draws),
          .pointPlaceholders = deferredRasterSurfaceDrawLists(bim.pointDraws),
          .curvePlaceholders = deferredRasterSurfaceDrawLists(bim.curveDraws),
          .opaqueMeshDrawsUseGpuVisibility =
              bim.opaqueMeshDrawsUseGpuVisibility,
          .transparentMeshDrawsUseGpuVisibility =
              bim.transparentMeshDrawsUseGpuVisibility};
}

SceneOpaqueDrawLists
deferredRasterSceneOpaqueDrawLists(const FrameDrawLists &draws) {
  return {.aggregate = draws.opaqueDrawCommands,
          .singleSided = draws.opaqueSingleSidedDrawCommands,
          .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = draws.opaqueDoubleSidedDrawCommands};
}

VkDescriptorSet deferredRasterSceneDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::Scene);
}

VkDescriptorSet
deferredRasterBimSceneDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(p,
                                     DeferredRasterDescriptorSetId::BimScene);
}

VkDescriptorSet deferredRasterLightDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::Light);
}

VkDescriptorSet
deferredRasterTiledLightingDescriptorSet(const FrameRecordParams &p) {
  return deferredRasterDescriptorSet(
      p, DeferredRasterDescriptorSetId::TiledLighting);
}

VkBuffer deferredRasterCameraBuffer(const FrameRecordParams &p) {
  return deferredRasterBuffer(p, DeferredRasterBufferId::Camera);
}

VkDeviceSize deferredRasterCameraBufferSize(const FrameRecordParams &p) {
  return deferredRasterBufferSize(p, DeferredRasterBufferId::Camera);
}

bool deferredRasterCameraBufferReady(const FrameRecordParams &p) {
  return deferredRasterCameraBuffer(p) != VK_NULL_HANDLE &&
         deferredRasterCameraBufferSize(p) > 0u;
}

VkSampler deferredRasterGBufferSampler(const FrameRecordParams &p) {
  return deferredRasterSampler(p, DeferredRasterSamplerId::GBuffer);
}

bool deferredRasterGBufferSamplerReady(const FrameRecordParams &p) {
  return deferredRasterGBufferSampler(p) != VK_NULL_HANDLE;
}

std::array<VkDescriptorSet, 2>
deferredRasterPostProcessDescriptorSets(const FrameRecordParams &p) {
  return {deferredRasterDescriptorSet(
              p, DeferredRasterDescriptorSetId::PostProcess),
          deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::Oit)};
}

RenderPassReadiness deferredRasterPostProcessReadiness(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &deferred) {
  const VkExtent2D extent = deferred.swapchainExtent();
  const auto descriptorSets = deferredRasterPostProcessDescriptorSets(p);
  const bool descriptorSetsReady =
      std::all_of(descriptorSets.begin(), descriptorSets.end(),
                  [](VkDescriptorSet descriptorSet) {
                    return descriptorSet != VK_NULL_HANDLE;
                  });
  const bool framebufferReady =
      p.swapchain.swapChainFramebuffers != nullptr &&
      p.runtime.imageIndex < p.swapchain.swapChainFramebuffers->size() &&
      (*p.swapchain.swapChainFramebuffers)[p.runtime.imageIndex] !=
          VK_NULL_HANDLE;

  if (extent.width == 0u || extent.height == 0u ||
      p.postProcess.renderPass == VK_NULL_HANDLE || !framebufferReady ||
      !descriptorSetsReady ||
      !deferredRasterPipelineReady(p, DeferredRasterPipelineId::PostProcess) ||
      !deferredRasterPipelineLayoutReady(
          p, DeferredRasterPipelineLayoutId::PostProcess)) {
    return renderPassMissingResource(RenderResourceId::SwapchainImage);
  }

  return renderPassReady();
}

SceneOpaqueDrawGeometryBinding
deferredRasterSceneOpaqueGeometry(const FrameRecordParams &p) {
  return {.descriptorSet = deferredRasterSceneDescriptorSet(p),
          .vertexSlice = p.scene.vertexSlice,
          .indexSlice = p.scene.indexSlice,
          .indexType = p.scene.indexType};
}

SceneDiagnosticCubeGeometry deferredRasterSceneDiagnosticCubeGeometry(
    const SceneController *sceneController) {
  if (sceneController == nullptr) {
    return {};
  }
  return {.vertexSlice = sceneController->diagCubeVertexSlice(),
          .indexSlice = sceneController->diagCubeIndexSlice(),
          .indexType = VK_INDEX_TYPE_UINT32,
          .indexCount = sceneController->diagCubeIndexCount()};
}

RenderingPassHandle deferredRasterSceneRenderPass(const FrameRecordParams &p,
                                           SceneRasterPassKind kind) {
  switch (kind) {
  case SceneRasterPassKind::DepthPrepass:
    return deferredRasterRenderPass(p,
                                    DeferredRasterFramebufferId::DepthPrepass);
  case SceneRasterPassKind::GBuffer:
    return deferredRasterRenderPass(p, DeferredRasterFramebufferId::GBuffer);
  }
  return VK_NULL_HANDLE;
}

RenderingTargetHandle deferredRasterSceneFramebuffer(const FrameRecordParams &p,
                                             SceneRasterPassKind kind) {
  switch (kind) {
  case SceneRasterPassKind::DepthPrepass:
    return deferredRasterFramebuffer(p,
                                     DeferredRasterFramebufferId::DepthPrepass);
  case SceneRasterPassKind::GBuffer:
    return deferredRasterFramebuffer(p, DeferredRasterFramebufferId::GBuffer);
  }
  return VK_NULL_HANDLE;
}

SceneRasterPassPipelineInputs
deferredRasterScenePipelineInputs(const FrameRecordParams &p,
                                  SceneRasterPassKind kind) {
  switch (kind) {
  case SceneRasterPassKind::DepthPrepass:
    return {.primary = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::DepthPrepass),
            .frontCull = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::DepthPrepassFrontCull),
            .noCull = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::DepthPrepassNoCull)};
  case SceneRasterPassKind::GBuffer:
    return {.primary = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::GBuffer),
            .frontCull = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::GBufferFrontCull),
            .noCull = deferredRasterPipelineHandle(
                p, DeferredRasterPipelineId::GBufferNoCull)};
  }
  return {};
}

VkPipeline deferredRasterScenePrimaryPipeline(const FrameRecordParams &p,
                                              SceneRasterPassKind kind) {
  switch (kind) {
  case SceneRasterPassKind::DepthPrepass:
    return deferredRasterPipelineHandle(p,
                                        DeferredRasterPipelineId::DepthPrepass);
  case SceneRasterPassKind::GBuffer:
    return deferredRasterPipelineHandle(p, DeferredRasterPipelineId::GBuffer);
  }
  return VK_NULL_HANDLE;
}

SceneDiagnosticCubeRecordInputs deferredRasterSceneDiagnosticCubeInputs(
    const FrameRecordParams &p, SceneRasterPassKind kind,
    const SceneController *sceneController) {
  container::gpu::BindlessPushConstants pushConstants{};
  if (p.pushConstants.bindless != nullptr) {
    pushConstants = *p.pushConstants.bindless;
  }
  return {.geometry =
              deferredRasterSceneDiagnosticCubeGeometry(sceneController),
          .pipeline = deferredRasterScenePrimaryPipeline(p, kind),
          .pipelineLayout = deferredRasterPipelineLayout(
              p, DeferredRasterPipelineLayoutId::Scene),
          .descriptorSet = deferredRasterSceneDescriptorSet(p),
          .objectIndex = p.scene.diagCubeObjectIndex,
          .pushConstants = pushConstants};
}

SceneTransparentDrawLists
deferredRasterSceneTransparentDrawLists(const FrameDrawLists &draws) {
  return {
      .aggregate = draws.transparentDrawCommands,
      .singleSided = draws.transparentSingleSidedDrawCommands,
      .windingFlipped = draws.transparentWindingFlippedDrawCommands,
      .doubleSided = draws.transparentDoubleSidedDrawCommands,
  };
}

bool deferredRasterSceneTransparentPickReady(const FrameRecordParams &p) {
  return deferredRasterSceneDescriptorSet(p) != VK_NULL_HANDLE &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         (hasOpaqueDrawCommands(p.draws) || hasTransparentDrawCommands(p.draws));
}

RenderPassReadiness
deferredRasterSceneRasterReadiness(const FrameRecordParams &p,
                                   SceneRasterPassKind kind) {
  const RenderResourceId attachmentResource =
      kind == SceneRasterPassKind::DepthPrepass
          ? RenderResourceId::SceneDepth
          : RenderResourceId::GBufferAlbedo;
  if (deferredRasterFramebuffer(p, kind == SceneRasterPassKind::DepthPrepass
                                       ? DeferredRasterFramebufferId::DepthPrepass
                                       : DeferredRasterFramebufferId::GBuffer) ==
          VK_NULL_HANDLE ||
      deferredRasterRenderPass(p, kind == SceneRasterPassKind::DepthPrepass
                                      ? DeferredRasterFramebufferId::DepthPrepass
                                      : DeferredRasterFramebufferId::GBuffer) ==
          VK_NULL_HANDLE) {
    return renderPassMissingResource(attachmentResource);
  }
  if (!hasOpaqueDrawCommands(p.draws)) {
    // Clear the shared depth/G-buffer even when the only scene is a sidecar.
    // BIM LOAD attachments and sky/post-process still require this producer.
    return renderPassReady();
  }
  if (deferredRasterSceneDescriptorSet(p) == VK_NULL_HANDLE ||
      p.scene.vertexSlice.buffer == VK_NULL_HANDLE ||
      p.scene.indexSlice.buffer == VK_NULL_HANDLE ||
      deferredRasterPipelineLayoutReady(
          p, DeferredRasterPipelineLayoutId::Scene) == false ||
      !deferredRasterPipelineReady(
          p, kind == SceneRasterPassKind::DepthPrepass
                 ? DeferredRasterPipelineId::DepthPrepass
                 : DeferredRasterPipelineId::GBuffer)) {
    return renderPassMissingResource(RenderResourceId::SceneGeometry);
  }
  return renderPassReady();
}

bool deferredRasterLightGizmoPickReady(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &deferred) {
  const LightingManager *lightingManager = deferred.lightingManager();
  if (lightingManager == nullptr || lightingManager->editableLights().empty()) {
    return false;
  }
  const container::ui::GuiManager *guiManager = deferred.guiManager();
  if (guiManager == nullptr || !guiManager->showLightGizmos()) {
    return false;
  }
  return lightingManager->lightGizmoIconsReady() &&
         lightingManager->lightGizmoIconDescriptorSet() != VK_NULL_HANDLE &&
         deferredRasterDescriptorSet(p,
                                     DeferredRasterDescriptorSetId::FrameLighting) !=
             VK_NULL_HANDLE &&
         deferredRasterPipelineReady(
             p, DeferredRasterPipelineId::LightGizmoPick) &&
         deferredRasterPipelineLayoutReady(
             p, DeferredRasterPipelineLayoutId::LightGizmo);
}

TransparentPickFramePassRecordInputs deferredRasterTransparentPickInputs(
    const FrameRecordParams &p, VkExtent2D extent,
    const DeferredRasterFrameGraphContext &deferred) {
  const LightingManager *lightingManager = deferred.lightingManager();
  const bool lightGizmoPickReady =
      deferredRasterLightGizmoPickReady(p, deferred);
  const std::array<VkDescriptorSet, 2> lightGizmoDescriptorSets = {
      deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::FrameLighting),
      lightingManager ? lightingManager->lightGizmoIconDescriptorSet()
                      : VK_NULL_HANDLE};
  const VkPipeline lightGizmoPickPipeline =
      deferredRasterPipelineHandle(p, DeferredRasterPipelineId::LightGizmoPick);
  const VkPipelineLayout lightGizmoPipelineLayout =
      deferredRasterPipelineLayout(p,
                                   DeferredRasterPipelineLayoutId::LightGizmo);
  std::function<void(VkCommandBuffer)> recordLightGizmoPick{};
  if (lightGizmoPickReady) {
    recordLightGizmoPick = [lightingManager, lightGizmoDescriptorSets,
                            lightGizmoPickPipeline, lightGizmoPipelineLayout,
                            camera =
                                deferred.camera()](VkCommandBuffer passCmd) {
      lightingManager->drawLightGizmoPickIds(passCmd, lightGizmoDescriptorSets,
                                             lightGizmoPickPipeline,
                                             lightGizmoPipelineLayout, camera);
    };
  }
  return {
      .scenePassReady = deferredRasterSceneTransparentPickReady(p),
      .bimPassReady =
          hasBimTransparentGeometry(p) ||
          (deferredRasterBimSceneDescriptorSet(p) != VK_NULL_HANDLE &&
           p.bim.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
           p.bim.scene.indexSlice.buffer != VK_NULL_HANDLE &&
           hasBimOpaqueDrawCommands(p.bim)),
      .renderPass = deferredRasterRenderPass(
          p, DeferredRasterFramebufferId::TransparentPick),
      .framebuffer = deferredRasterFramebuffer(
          p, DeferredRasterFramebufferId::TransparentPick),
      .extent = extent,
      .sourceDepthStencilImage =
          deferredRasterImage(p, DeferredRasterImageId::DepthStencil),
      .pickDepthImage =
          deferredRasterImage(p, DeferredRasterImageId::PickDepth),
      .pickIdImage = deferredRasterImage(p, DeferredRasterImageId::PickId),
      .sceneOpaqueDraws = deferredRasterSceneOpaqueDrawLists(p.draws),
      .sceneDraws = deferredRasterSceneTransparentDrawLists(p.draws),
      .bimOpaqueDraws = deferredRasterBimSurfaceDrawSources(p.bim),
      .bimDraws = deferredRasterBimSurfaceDrawSources(p.bim),
      .scene = {.descriptorSet = deferredRasterSceneDescriptorSet(p),
                .vertexSlice = p.scene.vertexSlice,
                .indexSlice = p.scene.indexSlice,
                .indexType = p.scene.indexType},
      .bim = {.descriptorSet = deferredRasterBimSceneDescriptorSet(p),
              .vertexSlice = p.bim.scene.vertexSlice,
              .indexSlice = p.bim.scene.indexSlice,
              .indexType = p.bim.scene.indexType},
      .pipelines = {.primary = deferredRasterPipelineHandle(
                        p, DeferredRasterPipelineId::TransparentPick),
                    .frontCull = deferredRasterPipelineHandle(
                        p, DeferredRasterPipelineId::TransparentPickFrontCull),
                    .noCull = deferredRasterPipelineHandle(
                        p, DeferredRasterPipelineId::TransparentPickNoCull)},
      .pipelineLayout = deferredRasterPipelineLayout(
          p, DeferredRasterPipelineLayoutId::Scene),
      .pushConstants = p.pushConstants.bindless,
      .bimSemanticColorMode = p.bim.semanticColorMode,
      .debugOverlay = deferred.debugOverlay(),
      .bimManager = p.services.bimManager,
      .extraPassWorkActive = lightGizmoPickReady,
      .recordAfterGeometry = std::move(recordLightGizmoPick)};
}

bool deferredRasterLightGizmoOverlayReady(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &deferred) {
  const LightingManager *lightingManager = deferred.lightingManager();
  if (lightingManager == nullptr) {
    return false;
  }
  const container::ui::GuiManager *guiManager = deferred.guiManager();
  if (guiManager == nullptr || !guiManager->showLightGizmos()) {
    return false;
  }
  return lightingManager->lightGizmoIconsReady() &&
         lightingManager->lightGizmoIconDescriptorSet() != VK_NULL_HANDLE &&
         deferredRasterDescriptorSet(p,
                                     DeferredRasterDescriptorSetId::FrameLighting) !=
             VK_NULL_HANDLE &&
         deferredRasterPipelineReady(p, DeferredRasterPipelineId::LightGizmo) &&
         deferredRasterPipelineReady(p,
                                     DeferredRasterPipelineId::LightGizmoCoverage) &&
         deferredRasterPipelineLayoutReady(
             p, DeferredRasterPipelineLayoutId::LightGizmo);
}

void recordDeferredRasterLightGizmoOverlay(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &deferred, VkExtent2D extent) {
  const LightingManager *lightingManager = deferred.lightingManager();
  if (cmd == VK_NULL_HANDLE ||
      !deferredRasterLightGizmoOverlayReady(p, deferred) ||
      lightingManager == nullptr) {
    return;
  }

  const std::array<VkDescriptorSet, 2> descriptorSets = {
      deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::FrameLighting),
      lightingManager->lightGizmoIconDescriptorSet()};
  recordSceneViewportAndScissor(cmd, extent);
  lightingManager->drawLightGizmos(
      cmd, descriptorSets,
      deferredRasterPipelineHandle(p, DeferredRasterPipelineId::LightGizmo),
      deferredRasterPipelineHandle(p,
                                   DeferredRasterPipelineId::LightGizmoCoverage),
      deferredRasterPipelineLayout(p, DeferredRasterPipelineLayoutId::LightGizmo),
      deferred.camera());
}

DeferredRasterScenePassRecordInputs
deferredRasterScenePassInputs(const FrameRecordParams &p,
                              SceneRasterPassKind kind,
                              const DeferredRasterFrameGraphContext &deferred) {
  return {.kind = kind,
          .renderPass = deferredRasterSceneRenderPass(p, kind),
          .framebuffer = deferredRasterSceneFramebuffer(p, kind),
          .extent = deferred.swapchainExtent(),
          .draws = deferredRasterSceneOpaqueDrawLists(p.draws),
          .geometry = deferredRasterSceneOpaqueGeometry(p),
          .pipelines = deferredRasterScenePipelineInputs(p, kind),
          .pipelineLayout = deferredRasterPipelineLayout(
              p, DeferredRasterPipelineLayoutId::Scene),
          .pushConstants = p.pushConstants.bindless,
          .imageIndex = p.runtime.imageIndex,
          .diagnosticCube = deferredRasterSceneDiagnosticCubeInputs(
              p, kind, deferred.sceneController()),
          .gpuCullManager = deferred.gpuCullManager(),
          .frustumCullActive = deferred.isPassActive(RenderPassId::FrustumCull),
          .occlusionCullActive =
              deferred.isPassActive(RenderPassId::OcclusionCull),
          .debugOverlay = deferred.debugOverlay()};
}

VkPipeline chooseDeferredRasterPipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

bool allDrawCommandsSingleInstance(const std::vector<DrawCommand> *commands) {
  if (commands == nullptr) {
    return false;
  }
  return std::ranges::all_of(*commands, [](const DrawCommand &command) {
    return command.instanceCount <= 1u;
  });
}

ShadowPassGeometryBinding
deferredRasterShadowGeometryBinding(VkDescriptorSet descriptorSet,
                                    const FrameSceneGeometry &scene) {
  return {.sceneDescriptorSet = descriptorSet,
          .vertexSlice = scene.vertexSlice,
          .indexSlice = scene.indexSlice,
          .indexType = scene.indexType};
}

bool deferredRasterLocalShadowSceneReady(const FrameRecordParams &p) {
  return shadowDescriptorSet(p, ShadowDescriptorSetId::Scene) !=
             VK_NULL_HANDLE &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasOpaqueDrawCommands(p.draws);
}

bool deferredRasterLocalShadowBimReady(const FrameRecordParams &p) {
  return shadowDescriptorSet(p, ShadowDescriptorSetId::BimScene) !=
             VK_NULL_HANDLE &&
         p.bim.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.bim.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasBimOpaqueDrawCommands(p.bim);
}

ShadowPassDrawLists
deferredRasterLocalShadowSceneDraws(const FrameDrawLists &draws) {
  return {.singleSided = hasDrawCommands(draws.opaqueSingleSidedDrawCommands)
                             ? draws.opaqueSingleSidedDrawCommands
                             : draws.opaqueDrawCommands,
          .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = draws.opaqueDoubleSidedDrawCommands};
}

ShadowPassDrawLists
deferredRasterLocalShadowBimDraws(const FrameBimResources &bim) {
  return {.singleSided =
              hasDrawCommands(bim.draws.opaqueSingleSidedDrawCommands)
                  ? bim.draws.opaqueSingleSidedDrawCommands
                  : bim.draws.opaqueDrawCommands,
          .windingFlipped = bim.draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = bim.draws.opaqueDoubleSidedDrawCommands};
}

bool deferredRasterCanRecordLocalShadowPass(const FrameRecordParams &p) {
  return p.shadows.renderPass != VK_NULL_HANDLE &&
         p.shadows.localShadowFramebuffers != nullptr &&
         shadowDescriptorSet(p, ShadowDescriptorSetId::LocalShadow) !=
             VK_NULL_HANDLE &&
         p.shadows.localShadowLayerCount > 0u &&
         shadowPipelineReady(p, ShadowPipelineId::LocalDepth) &&
         shadowPipelineLayoutReady(p, ShadowPipelineLayoutId::Shadow) &&
         (deferredRasterLocalShadowSceneReady(p) ||
          deferredRasterLocalShadowBimReady(p));
}

void recordDeferredRasterLocalShadowPass(VkCommandBuffer cmd,
                                         const FrameRecordParams &p,
                                         bool localShadowAtlasVisible) {
  if (!deferredRasterCanRecordLocalShadowPass(p)) {
    return;
  }

  const uint32_t layerCount =
      std::min(p.shadows.localShadowLayerCount, kMaxShadowedLocalLightLayers);
  if (layerCount == 0u) {
    return;
  }

  const VkPipeline primaryPipeline =
      shadowPipelineHandle(p, ShadowPipelineId::LocalDepth);
  const VkPipeline frontCullPipeline = chooseDeferredRasterPipeline(
      shadowPipelineHandle(p, ShadowPipelineId::LocalDepthFrontCull),
      primaryPipeline);
  const VkPipeline noCullPipeline = chooseDeferredRasterPipeline(
      shadowPipelineHandle(p, ShadowPipelineId::LocalDepthNoCull),
      primaryPipeline);
  const VkExtent2D localShadowExtent{kLocalShadowMapResolution,
                                     kLocalShadowMapResolution};

  for (uint32_t layerIndex = 0u; layerIndex < layerCount; ++layerIndex) {
    const RenderingTargetHandle framebuffer =
        p.shadows.localShadowFramebuffers[layerIndex];
    if (framebuffer == VK_NULL_HANDLE) {
      continue;
    }

    container::gpu::ShadowPushConstants pushConstants{};
    pushConstants.cascadeIndex = layerIndex;
    if (p.pushConstants.bindless != nullptr) {
      pushConstants.sectionPlaneEnabled =
          p.pushConstants.bindless->sectionPlaneEnabled;
      pushConstants.sectionPlane = p.pushConstants.bindless->sectionPlane;
    }

    static_cast<void>(recordShadowCascadePassCommands(
        cmd,
        {.cascadePassActive = true,
         .raster = {.shadowAtlasVisible = localShadowAtlasVisible,
                    .shadowPassRecordable = true,
                    .extent = localShadowExtent},
         .renderPass = p.shadows.renderPass,
         .framebuffer = framebuffer,
         .extent = localShadowExtent,
         .scene = deferredRasterShadowGeometryBinding(
             shadowDescriptorSet(p, ShadowDescriptorSetId::Scene), p.scene),
         .bim = deferredRasterShadowGeometryBinding(
             shadowDescriptorSet(p, ShadowDescriptorSetId::BimScene),
             p.bim.scene),
         .shadowDescriptorSet =
             shadowDescriptorSet(p, ShadowDescriptorSetId::LocalShadow),
         .pipelines = {.primary = primaryPipeline,
                       .frontCull = frontCullPipeline,
                       .noCull = noCullPipeline},
         .pipelineLayout =
             shadowPipelineLayout(p, ShadowPipelineLayoutId::Shadow),
         .pushConstants = pushConstants,
         .rasterConstantBias = p.shadows.shadowSettings.rasterConstantBias,
         .rasterSlopeBias = p.shadows.shadowSettings.rasterSlopeBias,
         .bimManager = p.services.bimManager,
         .drawInputs = {
             .sceneGeometryReady = deferredRasterLocalShadowSceneReady(p),
             .bimGeometryReady = deferredRasterLocalShadowBimReady(p),
             .sceneGpuCullActive = false,
             .bimGpuFilteredMeshActive =
                 p.bim.opaqueMeshDrawsUseGpuVisibility &&
                 hasOpaqueDrawCommands(p.bim.draws),
             .sceneDraws = deferredRasterLocalShadowSceneDraws(p.draws),
             .bimDraws = deferredRasterLocalShadowBimDraws(p.bim)}}));
  }
}

bool deferredRasterBimSurfacePassReady(const FrameRecordParams &p,
                                       BimSurfacePassKind kind) {
  switch (kind) {
  case BimSurfacePassKind::DepthPrepass:
    return deferredRasterRenderPass(
               p, DeferredRasterFramebufferId::BimDepthPrepass) !=
               VK_NULL_HANDLE &&
           deferredRasterFramebuffer(
               p, DeferredRasterFramebufferId::BimDepthPrepass) !=
               VK_NULL_HANDLE;
  case BimSurfacePassKind::GBuffer:
    return deferredRasterRenderPass(
               p, DeferredRasterFramebufferId::BimGBuffer) != VK_NULL_HANDLE &&
           deferredRasterFramebuffer(
               p, DeferredRasterFramebufferId::BimGBuffer) != VK_NULL_HANDLE;
  case BimSurfacePassKind::TransparentPick:
  case BimSurfacePassKind::OpaqueLighting:
  case BimSurfacePassKind::TransparentLighting:
    return false;
  }
  return false;
}

bool deferredRasterBimProviderRasterBatchReady(const FrameRecordParams &p) {
  for (const RasterDrawBatchDesc &batch : p.sceneExtraction.rasterBatches) {
    if (batch.providerKind == container::scene::SceneProviderKind::Bim &&
        batch.opaqueBatchCount > 0) {
      return true;
    }
  }
  return false;
}

bool deferredRasterBimProviderExtractionPresent(const FrameRecordParams &p) {
  for (const container::scene::SceneProviderSnapshot &provider :
       p.sceneExtraction.snapshots) {
    if (provider.kind == container::scene::SceneProviderKind::Bim) {
      return true;
    }
  }
  return false;
}

RenderingPassHandle deferredRasterBimSurfaceRenderPass(const FrameRecordParams &p,
                                                BimSurfacePassKind kind) {
  switch (kind) {
  case BimSurfacePassKind::DepthPrepass:
    return deferredRasterRenderPass(
        p, DeferredRasterFramebufferId::BimDepthPrepass);
  case BimSurfacePassKind::GBuffer:
    return deferredRasterRenderPass(p, DeferredRasterFramebufferId::BimGBuffer);
  case BimSurfacePassKind::TransparentPick:
  case BimSurfacePassKind::OpaqueLighting:
  case BimSurfacePassKind::TransparentLighting:
    return VK_NULL_HANDLE;
  }
  return VK_NULL_HANDLE;
}

RenderingTargetHandle deferredRasterBimSurfaceFramebuffer(const FrameRecordParams &p,
                                                  BimSurfacePassKind kind) {
  switch (kind) {
  case BimSurfacePassKind::DepthPrepass:
    return deferredRasterFramebuffer(
        p, DeferredRasterFramebufferId::BimDepthPrepass);
  case BimSurfacePassKind::GBuffer:
    return deferredRasterFramebuffer(p,
                                     DeferredRasterFramebufferId::BimGBuffer);
  case BimSurfacePassKind::TransparentPick:
  case BimSurfacePassKind::OpaqueLighting:
  case BimSurfacePassKind::TransparentLighting:
    return VK_NULL_HANDLE;
  }
  return VK_NULL_HANDLE;
}

BimSurfaceRasterPassPipelines
deferredRasterBimSurfacePipelines(const FrameRecordParams &p,
                                  BimSurfacePassKind kind) {
  switch (kind) {
  case BimSurfacePassKind::DepthPrepass: {
    const VkPipeline depthPipeline = chooseDeferredRasterPipeline(
        deferredRasterPipelineHandle(p,
                                     DeferredRasterPipelineId::BimDepthPrepass),
        deferredRasterPipelineHandle(p,
                                     DeferredRasterPipelineId::DepthPrepass));
    const VkPipeline frontCullPipeline = chooseDeferredRasterPipeline(
        deferredRasterPipelineHandle(
            p, DeferredRasterPipelineId::BimDepthPrepassFrontCull),
        depthPipeline);
    const VkPipeline noCullPipeline = chooseDeferredRasterPipeline(
        deferredRasterPipelineHandle(
            p, DeferredRasterPipelineId::BimDepthPrepassNoCull),
        depthPipeline);
    return {.singleSided = depthPipeline,
            .windingFlipped = frontCullPipeline,
            .doubleSided = noCullPipeline};
  }
  case BimSurfacePassKind::GBuffer: {
    const VkPipeline gBufferPipeline = chooseDeferredRasterPipeline(
        deferredRasterPipelineHandle(p, DeferredRasterPipelineId::BimGBuffer),
        deferredRasterPipelineHandle(p, DeferredRasterPipelineId::GBuffer));
    const VkPipeline frontCullPipeline = chooseDeferredRasterPipeline(
        deferredRasterPipelineHandle(
            p, DeferredRasterPipelineId::BimGBufferFrontCull),
        gBufferPipeline);
    const VkPipeline noCullPipeline = chooseDeferredRasterPipeline(
        deferredRasterPipelineHandle(
            p, DeferredRasterPipelineId::BimGBufferNoCull),
        gBufferPipeline);
    return {.singleSided = gBufferPipeline,
            .windingFlipped = frontCullPipeline,
            .doubleSided = noCullPipeline};
  }
  case BimSurfacePassKind::TransparentPick:
  case BimSurfacePassKind::OpaqueLighting:
  case BimSurfacePassKind::TransparentLighting:
    return {};
  }
  return {};
}

BimSurfaceFrameBinding deferredRasterBimSurfaceFrameBinding(
    const FrameBimResources &bim,
    std::span<const VkDescriptorSet> descriptorSets) {
  return buildBimSurfaceFrameBinding(
      {.draws = deferredRasterBimSurfaceDrawSources(bim),
       .vertexSlice = bim.scene.vertexSlice,
       .indexSlice = bim.scene.indexSlice,
       .indexType = bim.scene.indexType,
       .descriptorSets = descriptorSets,
       .semanticColorMode = bim.semanticColorMode});
}

DeferredRasterBimSurfacePassRecordInputs deferredRasterBimSurfacePassInputs(
    const FrameRecordParams &p, BimSurfacePassKind kind,
    const DeferredRasterFrameGraphContext &deferred,
    std::span<const VkDescriptorSet> descriptorSets) {
  return {.kind = kind,
          .passReady = deferredRasterBimSurfacePassReady(p, kind),
          .renderPass = deferredRasterBimSurfaceRenderPass(p, kind),
          .framebuffer = deferredRasterBimSurfaceFramebuffer(p, kind),
          .extent = deferred.swapchainExtent(),
          .binding =
              deferredRasterBimSurfaceFrameBinding(p.bim, descriptorSets),
          .pipelines = deferredRasterBimSurfacePipelines(p, kind),
          .pipelineLayout = deferredRasterPipelineLayout(
              p, DeferredRasterPipelineLayoutId::Scene),
          .pushConstants = p.pushConstants.bindless,
          .debugOverlay = deferred.debugOverlay(),
          .bimManager = p.services.bimManager};
}

DeferredTransformGizmoDrawInputs deferredRasterTransformGizmoDrawInputs(
    VkCommandBuffer cmd, const FrameRecordParams &p, VkExtent2D extent,
    VkPipeline pipeline, VkPipeline solidPipeline) {
  return {.commandBuffer = cmd,
          .extent = extent,
          .gizmo = p.transformGizmo,
          .wideLinesSupported = p.debug.wireframeWideLinesSupported,
          .lightingDescriptorSet = deferredRasterDescriptorSet(
              p, DeferredRasterDescriptorSetId::FrameLighting),
          .pipelineLayout = deferredRasterPipelineLayout(
              p, DeferredRasterPipelineLayoutId::TransformGizmo),
          .pipeline = pipeline,
          .solidPipeline = solidPipeline,
          .pushConstants = p.pushConstants.transformGizmo};
}

void recordDeferredRasterTransformGizmoFramePass(VkCommandBuffer cmd,
                                                 const FrameRecordParams &p,
                                                 VkExtent2D extent) {
  recordDeferredTransformGizmoPass(
      {.renderPass = deferredRasterRenderPass(
           p, DeferredRasterFramebufferId::TransformGizmo),
       .framebuffer = deferredRasterFramebuffer(
           p, DeferredRasterFramebufferId::TransformGizmo),
       .draw = deferredRasterTransformGizmoDrawInputs(
           cmd, p, extent,
           deferredRasterPipelineHandle(
               p, DeferredRasterPipelineId::TransformGizmo),
           deferredRasterPipelineHandle(
               p, DeferredRasterPipelineId::TransformGizmoSolid))});
}

void recordDeferredRasterTransformGizmoOverlay(VkCommandBuffer cmd,
                                               const FrameRecordParams &p,
                                               VkExtent2D extent) {
  recordDeferredTransformGizmoOverlay(deferredRasterTransformGizmoDrawInputs(
      cmd, p, extent,
      deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::TransformGizmoOverlay),
      deferredRasterPipelineHandle(
          p, DeferredRasterPipelineId::TransformGizmoSolidOverlay)));
}

} // namespace

std::string_view DeferredRasterTechnique::name() const {
  return renderTechniqueName(id());
}

std::string_view DeferredRasterTechnique::displayName() const {
  return renderTechniqueDisplayName(id());
}

RenderTechniqueAvailability DeferredRasterTechnique::availability(
    const RenderSystemContext &context) const {
  if (context.frameRecorder == nullptr || context.deferredRaster == nullptr) {
    return RenderTechniqueAvailability::unavailable(
        "deferred raster requires frame recorder and deferred services");
  }
  return RenderTechniqueAvailability::availableNow();
}

TechniqueDebugModel DeferredRasterTechnique::debugModel() const {
  TechniqueDebugModel model{};
  model.techniqueName = std::string(name());
  model.displayName = std::string(displayName());
  model.displayModes = {
      {.id = "lit",
       .label = "Lit",
       .value = static_cast<uint32_t>(container::ui::GBufferViewMode::Lit)},
      {.id = "albedo",
       .label = "Albedo",
       .value = static_cast<uint32_t>(container::ui::GBufferViewMode::Albedo)},
      {.id = "normals",
       .label = "Normals",
       .value = static_cast<uint32_t>(container::ui::GBufferViewMode::Normals)},
      {.id = "material",
       .label = "Material",
       .value =
           static_cast<uint32_t>(container::ui::GBufferViewMode::Material)},
      {.id = "depth",
       .label = "Depth",
       .value = static_cast<uint32_t>(container::ui::GBufferViewMode::Depth)},
      {.id = "emissive",
       .label = "Emissive",
       .value =
           static_cast<uint32_t>(container::ui::GBufferViewMode::Emissive)},
      {.id = "transparency",
       .label = "Transparency",
       .value =
           static_cast<uint32_t>(container::ui::GBufferViewMode::Transparency)},
      {.id = "revealage",
       .label = "Revealage",
       .value =
           static_cast<uint32_t>(container::ui::GBufferViewMode::Revealage)},
      {.id = "overview",
       .label = "Overview",
       .value =
           static_cast<uint32_t>(container::ui::GBufferViewMode::Overview)},
      {.id = "surface-normals",
       .label = "Surface Normals",
       .value = static_cast<uint32_t>(
           container::ui::GBufferViewMode::SurfaceNormals)},
      {.id = "object-space-normals",
       .label = "Object Normals",
       .value = static_cast<uint32_t>(
           container::ui::GBufferViewMode::ObjectSpaceNormals)},
      {.id = "shadow-cascades",
       .label = "Shadow Cascades",
       .value = static_cast<uint32_t>(
           container::ui::GBufferViewMode::ShadowCascades)},
      {.id = "tile-light-heat-map",
       .label = "Tile Light Heat Map",
       .value = static_cast<uint32_t>(
           container::ui::GBufferViewMode::TileLightHeatMap)},
      {.id = "shadow-texel-density",
       .label = "Shadow Texel Density",
       .value = static_cast<uint32_t>(
           container::ui::GBufferViewMode::ShadowTexelDensity)},
  };
  model.displayModes.insert(
      model.displayModes.end(),
      {
          {.id = "taa-velocity",
           .label = "TAA Velocity (magenta = invalid)",
           .value = 100},
          {.id = "taa-age", .label = "TAA History Age", .value = 101},
          {.id = "taa-rejection",
           .label = "TAA Rejection Reason",
           .value = 102},
          {.id = "taa-blend", .label = "TAA History Blend", .value = 103},
          {.id = "taa-reactive", .label = "TAA Reactive Mask", .value = 104},
      });
  model.panels.push_back(TechniqueDebugPanel{
      .id = "deferred-frame",
      .title = "Deferred Frame",
      .controls = {
          TechniqueDebugControl{.id = "render-graph",
                                .label = "Render graph",
                                .kind = TechniqueDebugControlKind::Action},
          TechniqueDebugControl{
              .id = "gbuffer-view",
              .label = "G-buffer view",
              .kind = TechniqueDebugControlKind::Enum,
              .options =
                  {
                      {.id = "lit", .label = "Lit"},
                      {.id = "overview", .label = "Overview"},
                      {.id = "transparency", .label = "Transparency"},
                  }},
          TechniqueDebugControl{.id = "wireframe-overlay",
                                .label = "Wireframe overlay",
                                .kind = TechniqueDebugControlKind::Toggle},
      }});
  return model;
}

void DeferredRasterTechnique::registerTechniqueContracts(
    RenderSystemContext &context) {
  if (context.frameResources != nullptr) {
    registerDeferredRasterFrameResources(*context.frameResources);
  }
  if (context.pipelines != nullptr) {
    registerDeferredRasterPipelineRecipes(*context.pipelines);
  }
}

void DeferredRasterTechnique::buildFrameGraph(RenderSystemContext &context) {
  registerTechniqueContracts(context);

  if (context.frameRecorder == nullptr || context.deferredRaster == nullptr) {
    return;
  }

  DeferredRasterFrameGraphContext *deferred = context.deferredRaster;
  const DeferredTransparentOitFramePassRecorder transparentOit =
      deferred->transparentOitFramePassRecorder();
  RenderGraphBuilder graph = deferred->graphBuilder();
  graph.clear();

  // CPU registration order is the frame schedule. Later passes rely on the
  // image layouts and indirect-count buffers produced by earlier nodes.
  graph.addPass(
      RenderPassId::FrustumCull,
      [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
        const uint32_t sourceDrawCount =
            p.draws.opaqueSingleSidedDrawCommands != nullptr
                ? static_cast<uint32_t>(
                      p.draws.opaqueSingleSidedDrawCommands->size())
                : 0u;
        const DeferredRasterFrustumCullPassPlan frustumCullPlan =
            buildDeferredRasterFrustumCullPassPlan(
                {.gpuCullManagerReady = deferred->gpuCullManager() != nullptr &&
                                        deferred->gpuCullManager()->isReady(),
                 .sceneSingleSidedDrawsAvailable =
                     hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands),
                 .cameraBufferReady = deferredRasterCameraBufferReady(p),
                 .objectBufferReady = p.scene.objectBuffer != VK_NULL_HANDLE &&
                                      p.scene.objectBufferSize > 0u,
                 .debugFreezeCulling = p.debug.debugFreezeCulling,
                 .cullingFrozen = deferred->gpuCullManager() != nullptr &&
                                  deferred->gpuCullManager()->cullingFrozen(),
                 .sourceDrawCount = sourceDrawCount});
        static_cast<void>(recordDeferredRasterFrustumCullPassCommands(
            cmd, {.gpuCullManager = deferred->gpuCullManager(),
                  .plan = frustumCullPlan,
                  .drawCommands = p.draws.opaqueSingleSidedDrawCommands,
                  .imageIndex = p.runtime.imageIndex,
                  .cameraBuffer = deferredRasterCameraBuffer(p),
                  .cameraBufferSize = deferredRasterCameraBufferSize(p),
                  .objectBuffer = p.scene.objectBuffer,
                  .objectBufferSize = p.scene.objectBufferSize,
                  .drawSourceRevision = p.scene.objectDataRevision}));
      });

  graph.addPass(
      RenderPassId::DepthPrepass,
      [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
        static_cast<void>(recordDeferredRasterScenePassCommands(
            cmd, deferredRasterScenePassInputs(
                     p, SceneRasterPassKind::DepthPrepass, *deferred)));
      });

  graph.addPass(RenderPassId::BimDepthPrepass,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
                      deferredRasterBimSceneDescriptorSet(p)};
                  static_cast<void>(recordDeferredRasterBimSurfacePassCommands(
                      cmd, deferredRasterBimSurfacePassInputs(
                               p, BimSurfacePassKind::DepthPrepass, *deferred,
                               bimDescriptorSets)));
                });

  graph.addPass(RenderPassId::HiZGenerate, [deferred](
                                               VkCommandBuffer cmd,
                                               const FrameRecordParams &p) {
    const VkImage depthStencilImage =
        deferredRasterImage(p, DeferredRasterImageId::DepthStencil);
    const VkImageView depthSamplingView =
        deferredRasterImageView(p, DeferredRasterImageId::DepthSamplingView);
    const bool frameReady = depthStencilImage != VK_NULL_HANDLE ||
                            depthSamplingView != VK_NULL_HANDLE;
    const DeferredRasterHiZPassPlan hizPassPlan =
        buildDeferredRasterHiZPassPlan(
            {.gpuCullManagerReady = deferred->gpuCullManager() != nullptr &&
                                    deferred->gpuCullManager()->isReady(),
             .frameReady = frameReady,
             .depthSamplingViewReady = depthSamplingView != VK_NULL_HANDLE,
             .depthSamplerReady = deferredRasterGBufferSamplerReady(p),
             .depthStencilImageReady = depthStencilImage != VK_NULL_HANDLE});
    if (!hizPassPlan.active)
      return;

    const auto extent = deferred->swapchainExtent();
    deferred->gpuCullManager()->ensureHiZImage(p.runtime.imageIndex,
                                               extent.width, extent.height);
    const DeferredRasterHiZDepthTransitionPlan hizDepthTransitionPlan =
        buildDeferredRasterHiZDepthTransitionPlan(
            {.depthStencilImage = depthStencilImage});
    if (!hizDepthTransitionPlan.active)
      return;
    static_cast<void>(recordDeferredRasterHiZDepthToSamplingTransitionCommands(
        cmd, hizDepthTransitionPlan));

    deferred->gpuCullManager()->dispatchHiZGenerate(
        cmd, p.runtime.imageIndex, depthSamplingView,
        deferredRasterGBufferSampler(p), extent.width, extent.height);

    static_cast<void>(
        recordDeferredRasterHiZDepthToAttachmentTransitionCommands(
            cmd, hizDepthTransitionPlan));
  });

  graph.addPass(RenderPassId::OcclusionCull, [deferred](
                                                 VkCommandBuffer cmd,
                                                 const FrameRecordParams &p) {
    if (deferred->gpuCullManager() &&
        deferred->gpuCullManager()->occlusionCullResourcesReady(
            p.runtime.imageIndex) &&
        p.draws.opaqueSingleSidedDrawCommands &&
        !p.draws.opaqueSingleSidedDrawCommands->empty()) {
      deferred->gpuCullManager()->dispatchOcclusionCull(
          cmd, p.runtime.imageIndex, deferredRasterCameraBuffer(p),
          deferredRasterCameraBufferSize(p),
          static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph.addPass(RenderPassId::CullStatsReadback,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (deferred->gpuCullManager() &&
                      deferred->gpuCullManager()->isReady()) {
                    deferred->gpuCullManager()->scheduleStatsReadback(cmd, p.runtime.imageIndex);
                  }
                });

  graph.addPass(RenderPassId::GBuffer,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  static_cast<void>(recordDeferredRasterScenePassCommands(
                      cmd, deferredRasterScenePassInputs(
                               p, SceneRasterPassKind::GBuffer, *deferred)));
                });

  graph.addPass(RenderPassId::BimGBuffer,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
                      deferredRasterBimSceneDescriptorSet(p)};
                  static_cast<void>(recordDeferredRasterBimSurfacePassCommands(
                      cmd, deferredRasterBimSurfacePassInputs(
                               p, BimSurfacePassKind::GBuffer, *deferred,
                               bimDescriptorSets)));
                });

  graph.addPass(RenderPassId::TransparentPick,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  static_cast<void>(recordTransparentPickFramePassCommands(
                      cmd, deferredRasterTransparentPickInputs(
                               p, deferred->swapchainExtent(), *deferred)));
                });

  graph.addPass(
      RenderPassId::OitClear,
      [transparentOit](VkCommandBuffer cmd, const FrameRecordParams &p) {
        static_cast<void>(transparentOit.recordClear(cmd, p));
      });

  const auto shadowCullIds = shadowCullPassIds();
  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    graph.addPass(shadowCullIds[i], [deferred, i](VkCommandBuffer cmd,
                                                  const FrameRecordParams &p) {
      const uint32_t sourceDrawCount =
          p.draws.opaqueSingleSidedDrawCommands != nullptr
              ? static_cast<uint32_t>(
                    p.draws.opaqueSingleSidedDrawCommands->size())
              : 0u;
      const ShadowCullPassPlan shadowCullPlan = buildShadowCullPassPlan(
          {.shadowAtlasVisible =
               displayModeRecordsShadowAtlas(deferred->displayMode()),
           .gpuShadowCullEnabled = p.shadows.useGpuShadowCull,
           .shadowCullManagerReady = p.shadows.shadowCullManager != nullptr &&
                                     p.shadows.shadowCullManager->isReady(),
           .sceneSingleSidedDrawsAvailable =
               hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands),
           .sourceDrawCommandsAllSingleInstance =
               allDrawCommandsSingleInstance(
                   p.draws.opaqueSingleSidedDrawCommands),
           .cameraBufferReady = deferredRasterCameraBufferReady(p),
           .cascadeIndexInRange = i < kShadowCascadeCount,
           .sourceDrawCount = sourceDrawCount});
      static_cast<void>(recordShadowCullPassCommands(
          cmd, {.shadowCullManager = p.shadows.shadowCullManager,
                .plan = shadowCullPlan,
                .imageIndex = p.runtime.imageIndex,
                .cascadeIndex = i}));
    });

    graph.addPass(shadowPassIds[i], [deferred, i](VkCommandBuffer cmd,
                                                  const FrameRecordParams &p) {
      deferred->recordShadowPass(cmd, p, i);
    });
  }

  graph.addPass(RenderPassId::LocalShadowDepth,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  recordDeferredRasterLocalShadowPass(
                      cmd, p,
                      displayModeRecordsShadowAtlas(deferred->displayMode()));
                });

  // Transition depth from attachment-writable to read-only for the compute
  // passes (TileCull, GTAO) that sample depth.  The lighting render pass
  // initialLayout also expects DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
  graph.addPass(RenderPassId::DepthToReadOnly, [deferred](
                                                   VkCommandBuffer cmd,
                                                   const FrameRecordParams &p) {
    const DeferredRasterDepthReadOnlyTransitionPlan transitionPlan =
        buildDeferredRasterDepthReadOnlyTransitionPlan(
            {.depthStencilImage =
                 deferredRasterImage(p, DeferredRasterImageId::DepthStencil),
             .shadowAtlasImage =
                 p.shadows.shadowManager != nullptr
                     ? p.shadows.shadowManager->shadowAtlasImage()
                     : VK_NULL_HANDLE,
             .shadowAtlasVisible =
                 displayModeRecordsShadowAtlas(deferred->displayMode()),
             .shadowCascadeCount = kShadowCascadeCount,
             .localShadowAtlasImage =
                 p.shadows.shadowManager != nullptr
                     ? p.shadows.shadowManager->localShadowAtlasImage()
                     : VK_NULL_HANDLE,
             .localShadowAtlasVisible =
                 displayModeRecordsShadowAtlas(deferred->displayMode()) &&
                 deferredRasterCanRecordLocalShadowPass(p),
             .localShadowLayerCount = std::min(p.shadows.localShadowLayerCount,
                                               kMaxShadowedLocalLightLayers)});
    static_cast<void>(recordDeferredRasterDepthReadOnlyTransitionCommands(
        cmd, transitionPlan));
  });

  graph.addPass(RenderPassId::TileCull, [deferred](VkCommandBuffer cmd,
                                                   const FrameRecordParams &p) {
    const VkImageView depthSamplingView =
        deferredRasterImageView(p, DeferredRasterImageId::DepthSamplingView);
    const bool frameAvailable = depthSamplingView != VK_NULL_HANDLE;
    const DeferredRasterTileCullPlan tileCullPlan =
        buildDeferredRasterTileCullPlan(
            {.tileCullDisplayMode =
                 displayModeRecordsTileCull(deferred->displayMode()),
             .tiledLightingReady =
                 deferred->lightingManager() != nullptr &&
                 deferred->lightingManager()->isTiledLightingReady(),
             .frameAvailable = frameAvailable,
             .depthSamplingView = depthSamplingView,
             .cameraBuffer = deferredRasterCameraBuffer(p),
             .cameraBufferSize = deferredRasterCameraBufferSize(p),
             .screenExtent = deferred->swapchainExtent(),
             .cameraNear = p.camera.nearPlane,
             .cameraFar = p.camera.farPlane});
    static_cast<void>(recordDeferredRasterTileCullCommands(
        cmd, {.lightingManager = deferred->lightingManager(),
              .plan = tileCullPlan}));
  });

  graph.addPass(RenderPassId::GTAO, [deferred](VkCommandBuffer cmd,
                                               const FrameRecordParams &p) {
    if (!displayModeRecordsGtao(deferred->displayMode()))
      return;
    const VkImageView depthSamplingView =
        deferredRasterImageView(p, DeferredRasterImageId::DepthSamplingView);
    const VkImageView normalView =
        deferredRasterImageView(p, DeferredRasterImageId::Normal);
    if (deferred->environmentManager() &&
        deferred->environmentManager()->isGtaoReady() &&
        depthSamplingView != VK_NULL_HANDLE && normalView != VK_NULL_HANDLE) {
      const auto extent = deferred->swapchainExtent();
      deferred->environmentManager()->dispatchGtao(
          cmd, extent.width, extent.height, deferredRasterCameraBuffer(p),
          deferredRasterCameraBufferSize(p), depthSamplingView,
          deferredRasterGBufferSampler(p), normalView,
          deferredRasterGBufferSampler(p));
      deferred->environmentManager()->dispatchGtaoBlur(
          cmd, depthSamplingView, deferredRasterGBufferSampler(p),
          p.camera.nearPlane, p.camera.farPlane, p.camera.orthographic);
    }
  });

  graph.addPass(RenderPassId::Lighting, [deferred](VkCommandBuffer cmd,
                                                   const FrameRecordParams &p) {
    const std::array<VkDescriptorSet, 2> lightingSets = {
        deferredRasterDescriptorSet(
            p, DeferredRasterDescriptorSetId::FrameLighting),
        deferredRasterLightDescriptorSet(p)};
    const std::array<VkDescriptorSet, 4> transparentSets = {
        deferredRasterSceneDescriptorSet(p),
        deferredRasterLightDescriptorSet(p),
        deferredRasterDescriptorSet(p, DeferredRasterDescriptorSetId::Oit),
        deferredRasterDescriptorSet(
            p, DeferredRasterDescriptorSetId::FrameLighting)};
    deferred->lightingPassRecorder().record(cmd, p,
                                            deferredRasterSceneDescriptorSet(p),
                                            lightingSets, transparentSets);
  });

  registerTemporalPasses(graph);
  registerRayShadowPasses(graph);

  graph.addPass(RenderPassId::TransformGizmos,
                [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (p.services.temporalManager &&
                      p.services.temporalManager->active())
                    return;
                  recordDeferredRasterTransformGizmoFramePass(
                      cmd, p, deferred->swapchainExtent());
                });

  graph.addPass(
      RenderPassId::ExposureAdaptation,
      [deferred](VkCommandBuffer cmd, const FrameRecordParams &p) {
        if (!displayModeRecordsExposureAdaptation(deferred->displayMode()))
          return;
        if (!deferred->exposureManager() ||
            !deferred->exposureManager()->isReady())
          return;
        const VkImage sceneColorImage =
            deferredRasterImage(p, DeferredRasterImageId::SceneColor);
        const VkImageView sceneColorView = temporalSceneColorView(
            p, deferredRasterImageView(p, DeferredRasterImageId::SceneColor));
        if (sceneColorView == VK_NULL_HANDLE ||
            sceneColorImage == VK_NULL_HANDLE)
          return;

        const container::gpu::ExposureSettings exposureSettings =
            sanitizeExposureSettings(p.postProcess.exposureSettings);
        if (exposureSettings.mode != container::gpu::kExposureModeAuto)
          return;

        const DeferredRasterSceneColorReadBarrierPlan sceneColorReadPlan =
            buildDeferredRasterSceneColorReadBarrierPlan(
                {.sceneColorImage = sceneColorImage});
        static_cast<void>(recordDeferredRasterSceneColorReadBarrierCommands(
            cmd, sceneColorReadPlan));

        const auto extent = deferred->swapchainExtent();
        deferred->exposureManager()->dispatch(p.runtime.imageIndex, cmd,
                                              sceneColorView, extent.width,
                                              extent.height,
                                              exposureSettings, temporalSceneColorLayout(p));
      });

  graph.addPass(
      RenderPassId::OitResolve,
      [transparentOit](VkCommandBuffer cmd, const FrameRecordParams &p) {
        static_cast<void>(transparentOit.recordResolvePreparation(cmd, p));
      });

  graph.addPass(RenderPassId::Bloom, [deferred](VkCommandBuffer cmd,
                                                const FrameRecordParams &p) {
    if (!displayModeRecordsBloom(deferred->displayMode()))
      return;
    if (!deferred->bloomManager() || !deferred->bloomManager()->isReady())
      return;
    if (!deferred->bloomManager()->enabled())
      return;
    const VkImage sceneColorImage =
        deferredRasterImage(p, DeferredRasterImageId::SceneColor);
    const VkImageView sceneColorView = temporalSceneColorView(
        p, deferredRasterImageView(p, DeferredRasterImageId::SceneColor));
    if (sceneColorView == VK_NULL_HANDLE || sceneColorImage == VK_NULL_HANDLE)
      return;

    const DeferredRasterSceneColorReadBarrierPlan sceneColorReadPlan =
        buildDeferredRasterSceneColorReadBarrierPlan(
            {.sceneColorImage = sceneColorImage});
    static_cast<void>(recordDeferredRasterSceneColorReadBarrierCommands(
        cmd, sceneColorReadPlan));

    const auto extent = deferred->swapchainExtent();
    deferred->bloomManager()->dispatch(cmd, sceneColorView, extent.width,
                                       extent.height,
                                       temporalSceneColorLayout(p));
  });

  graph.addPass(RenderPassId::PostProcess, [deferred](VkCommandBuffer cmd,
                                                      const FrameRecordParams &p) {
    const auto ppSets = deferredRasterPostProcessDescriptorSets(p);
    const auto extent = deferred->swapchainExtent();
    const container::gpu::ExposureSettings exposureSettings =
        sanitizeExposureSettings(p.postProcess.exposureSettings);
    BloomManager *bloomManager = deferred->bloomManager();
    const LightingManager *lightingManager = deferred->lightingManager();
    const ExposureManager *exposureManager = deferred->exposureManager();
    static_cast<void>(recordDeferredPostProcessPassCommands(
        {.commandBuffer = cmd,
         .renderPass = p.postProcess.renderPass,
         .swapChainFramebuffers = p.swapchain.swapChainFramebuffers,
         .imageIndex = p.runtime.imageIndex,
         .extent = extent,
         .pipeline = deferredRasterPipelineHandle(
             p, DeferredRasterPipelineId::PostProcess),
         .pipelineLayout = deferredRasterPipelineLayout(
             p, DeferredRasterPipelineLayoutId::PostProcess),
         .descriptorSets = ppSets,
         .frameInputs =
             {.displayMode = deferred->displayMode(),
              .bloomPassActive = deferred->isPassActive(RenderPassId::Bloom),
              .bloomReady = bloomManager && bloomManager->isReady(),
              .bloomEnabled = bloomManager && bloomManager->enabled(),
              .bloomIntensity = bloomManager ? bloomManager->intensity() : 0.0f,
              .exposureSettings = exposureSettings,
              .resolvedExposure =
                  exposureManager
                      ? exposureManager->resolvedExposure(exposureSettings)
                      : resolvePostProcessExposure(exposureSettings),
              .cameraNear = p.camera.nearPlane,
              .cameraFar = p.camera.farPlane,
              .shadowData = p.shadows.shadowData,
              .tileCullPassActive =
                  deferred->isPassActive(RenderPassId::TileCull),
              .tiledLightingReady =
                  lightingManager && lightingManager->isTiledLightingReady(),
              .pointLightCount =
                  lightingManager
                      ? static_cast<uint32_t>(
                            lightingManager->pointLightsSsbo().size())
                      : 0u,
              .transparentOitActive =
                  deferred->isPassActive(RenderPassId::OitResolve)},
         .recordAfterFullscreenDraw = [deferred, &p](VkCommandBuffer passCmd) {
           recordDeferredRasterLightGizmoOverlay(
               passCmd, p, *deferred, deferred->swapchainExtent());
           recordDeferredRasterTransformGizmoOverlay(
               passCmd, p, deferred->swapchainExtent());
           deferred->renderGui(passCmd);
         }}));
  });

  graph.setPassReadiness(
      RenderPassId::FrustumCull, [deferred](const FrameRecordParams &p) {
        const uint32_t sourceDrawCount =
            p.draws.opaqueSingleSidedDrawCommands != nullptr
                ? static_cast<uint32_t>(
                      p.draws.opaqueSingleSidedDrawCommands->size())
                : 0u;
        return buildDeferredRasterFrustumCullPassPlan(
                   {.gpuCullManagerReady =
                        deferred->gpuCullManager() != nullptr &&
                        deferred->gpuCullManager()->isReady(),
                    .sceneSingleSidedDrawsAvailable =
                        hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands),
                    .cameraBufferReady = deferredRasterCameraBufferReady(p),
                    .objectBufferReady =
                        p.scene.objectBuffer != VK_NULL_HANDLE &&
                        p.scene.objectBufferSize > 0u,
                    .debugFreezeCulling = p.debug.debugFreezeCulling,
                    .cullingFrozen =
                        deferred->gpuCullManager() != nullptr &&
                        deferred->gpuCullManager()->cullingFrozen(),
                    .sourceDrawCount = sourceDrawCount})
            .readiness;
      });

  graph.setPassReadiness(
      RenderPassId::DepthPrepass, [](const FrameRecordParams &p) {
        return deferredRasterSceneRasterReadiness(
            p, SceneRasterPassKind::DepthPrepass);
      });

  graph.setPassReadiness(
      RenderPassId::BimDepthPrepass, [](const FrameRecordParams &p) {
        if (deferredRasterFramebuffer(
                p, DeferredRasterFramebufferId::BimDepthPrepass) ==
                VK_NULL_HANDLE ||
            deferredRasterRenderPass(
                p, DeferredRasterFramebufferId::BimDepthPrepass) ==
                VK_NULL_HANDLE) {
          return renderPassMissingResource(RenderResourceId::SceneDepth);
        }
        const bool bimOpaqueDrawsReady = hasBimOpaqueDrawCommands(p.bim);
        const bool bimProviderExtractionPresent =
            deferredRasterBimProviderExtractionPresent(p);
        const bool bimProviderRasterBatchReady =
            deferredRasterBimProviderRasterBatchReady(p);
        if (!bimOpaqueDrawsReady ||
            (bimProviderExtractionPresent && !bimProviderRasterBatchReady)) {
          return renderPassNotNeeded();
        }
        if (deferredRasterBimSceneDescriptorSet(p) == VK_NULL_HANDLE ||
            p.bim.scene.vertexSlice.buffer == VK_NULL_HANDLE ||
            p.bim.scene.indexSlice.buffer == VK_NULL_HANDLE) {
          return renderPassMissingResource(RenderResourceId::BimGeometry);
        }
        return renderPassReady();
      });

  graph.setPassReadiness(
      RenderPassId::HiZGenerate, [deferred](const FrameRecordParams &p) {
        const VkImage depthStencilImage =
            deferredRasterImage(p, DeferredRasterImageId::DepthStencil);
        const VkImageView depthSamplingView = deferredRasterImageView(
            p, DeferredRasterImageId::DepthSamplingView);
        const bool frameReady = depthStencilImage != VK_NULL_HANDLE ||
                                depthSamplingView != VK_NULL_HANDLE;
        return buildDeferredRasterHiZPassPlan(
                   {.gpuCullManagerReady =
                        deferred->gpuCullManager() != nullptr &&
                        deferred->gpuCullManager()->isReady(),
                    .frameReady = frameReady,
                    .depthSamplingViewReady =
                        depthSamplingView != VK_NULL_HANDLE,
                    .depthSamplerReady = deferredRasterGBufferSamplerReady(p),
                    .depthStencilImageReady =
                        depthStencilImage != VK_NULL_HANDLE})
            .readiness;
      });

  graph.setPassReadiness(
      RenderPassId::OcclusionCull, [deferred](const FrameRecordParams &p) {
        if (!deferred->gpuCullManager() ||
            !deferred->gpuCullManager()->occlusionCullResourcesReady(
                p.runtime.imageIndex) ||
            !hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands)) {
          return renderPassNotNeeded();
        }
        return renderPassReady();
      });

  graph.setPassReadiness(RenderPassId::CullStatsReadback,
                         [deferred](const FrameRecordParams &) {
                           return (deferred->gpuCullManager() &&
                                   deferred->gpuCullManager()->isReady())
                                      ? renderPassReady()
                                      : renderPassNotNeeded();
                         });

  graph.setPassReadiness(
      RenderPassId::GBuffer, [](const FrameRecordParams &p) {
        return deferredRasterSceneRasterReadiness(p,
                                                 SceneRasterPassKind::GBuffer);
      });

  graph.setPassReadiness(
      RenderPassId::BimGBuffer, [](const FrameRecordParams &p) {
        if (deferredRasterFramebuffer(
                p, DeferredRasterFramebufferId::BimGBuffer) == VK_NULL_HANDLE ||
            deferredRasterRenderPass(
                p, DeferredRasterFramebufferId::BimGBuffer) == VK_NULL_HANDLE) {
          return renderPassMissingResource(RenderResourceId::GBufferAlbedo);
        }
        const bool bimOpaqueDrawsReady = hasBimOpaqueDrawCommands(p.bim);
        const bool bimProviderExtractionPresent =
            deferredRasterBimProviderExtractionPresent(p);
        const bool bimProviderRasterBatchReady =
            deferredRasterBimProviderRasterBatchReady(p);
        if (!bimOpaqueDrawsReady ||
            (bimProviderExtractionPresent && !bimProviderRasterBatchReady)) {
          return renderPassNotNeeded();
        }
        if (deferredRasterBimSceneDescriptorSet(p) == VK_NULL_HANDLE ||
            p.bim.scene.vertexSlice.buffer == VK_NULL_HANDLE ||
            p.bim.scene.indexSlice.buffer == VK_NULL_HANDLE) {
          return renderPassMissingResource(RenderResourceId::BimGeometry);
        }
        return renderPassReady();
      });

  graph.setPassReadiness(
      RenderPassId::TransparentPick, [deferred](const FrameRecordParams &p) {
        if (deferredRasterFramebuffer(
                p, DeferredRasterFramebufferId::TransparentPick) ==
                VK_NULL_HANDLE ||
            !deferredRasterImageReady(p, DeferredRasterImageId::DepthStencil) ||
            !deferredRasterImageReady(p, DeferredRasterImageId::PickDepth) ||
            !deferredRasterImageReady(p, DeferredRasterImageId::PickId) ||
            deferredRasterRenderPass(
                p, DeferredRasterFramebufferId::TransparentPick) ==
                VK_NULL_HANDLE) {
          return renderPassMissingResource(RenderResourceId::PickId);
        }
        const bool lightGizmoPickReady =
            deferredRasterLightGizmoPickReady(p, *deferred);
        if (!hasOpaqueDrawCommands(p.draws) && !hasBimOpaqueDrawCommands(p.bim) &&
            !hasTransparentDrawCommands(p) && !lightGizmoPickReady) {
          return renderPassNotNeeded();
        }
        const bool sceneDrawable =
            deferredRasterSceneDescriptorSet(p) != VK_NULL_HANDLE &&
            p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
            p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
            (hasOpaqueDrawCommands(p.draws) ||
             hasTransparentDrawCommands(p.draws));
        if (!lightGizmoPickReady && !sceneDrawable &&
            !hasBimOpaqueDrawCommands(p.bim) &&
            !hasBimTransparentGeometry(p)) {
          return renderPassMissingResource(RenderResourceId::SceneGeometry);
        }
        return renderPassReady();
      });

  graph.setPassReadiness(RenderPassId::OitClear,
                         [transparentOit](const FrameRecordParams &p) {
                           return transparentOit.readiness(p);
                         });

  for (uint32_t i = 0; i < shadowCullIds.size(); ++i) {
    graph.setPassReadiness(shadowCullIds[i], [deferred,
                                              i](const FrameRecordParams &p) {
      const uint32_t sourceDrawCount =
          p.draws.opaqueSingleSidedDrawCommands != nullptr
              ? static_cast<uint32_t>(
                    p.draws.opaqueSingleSidedDrawCommands->size())
              : 0u;
      return buildShadowCullPassPlan(
                 {.shadowAtlasVisible =
                      displayModeRecordsShadowAtlas(deferred->displayMode()),
                  .gpuShadowCullEnabled = p.shadows.useGpuShadowCull,
                  .shadowCullManagerReady =
                      p.shadows.shadowCullManager != nullptr &&
                      p.shadows.shadowCullManager->isReady(),
                  .sceneSingleSidedDrawsAvailable =
                      hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands),
                  .sourceDrawCommandsAllSingleInstance =
                      allDrawCommandsSingleInstance(
                          p.draws.opaqueSingleSidedDrawCommands),
                  .cameraBufferReady = deferredRasterCameraBufferReady(p),
                  .cascadeIndexInRange = i < kShadowCascadeCount,
                  .sourceDrawCount = sourceDrawCount})
          .readiness;
    });
  }

  for (uint32_t i = 0; i < shadowPassIds.size(); ++i) {
    graph.setPassReadiness(
        shadowPassIds[i], [deferred, i](const FrameRecordParams &p) {
          if (!displayModeRecordsShadowAtlas(deferred->displayMode())) {
            return renderPassNotNeeded();
          }
          if (!deferred->canRecordShadowPass(p, i)) {
            return renderPassMissingResource(RenderResourceId::ShadowAtlas);
          }
          return renderPassReady();
        });
  }

  graph.setPassReadiness(
      RenderPassId::LocalShadowDepth, [deferred](const FrameRecordParams &p) {
        if (!displayModeRecordsShadowAtlas(deferred->displayMode())) {
          return renderPassNotNeeded();
        }
        if (p.shadows.localShadowLayerCount == 0u) {
          return renderPassNotNeeded();
        }
        if (!deferredRasterCanRecordLocalShadowPass(p)) {
          return renderPassMissingResource(RenderResourceId::LocalShadowAtlas);
        }
        return renderPassReady();
      });

  graph.setPassReadiness(RenderPassId::TileCull,
                         [deferred](const FrameRecordParams &p) {
        const VkImageView depthSamplingView = deferredRasterImageView(
            p, DeferredRasterImageId::DepthSamplingView);
        const bool frameAvailable = depthSamplingView != VK_NULL_HANDLE;
        return buildDeferredRasterTileCullPlan(
                   {.tileCullDisplayMode =
                        displayModeRecordsTileCull(deferred->displayMode()),
                    .tiledLightingReady =
                        deferred->lightingManager() != nullptr &&
                        deferred->lightingManager()->isTiledLightingReady(),
                    .frameAvailable = frameAvailable,
                    .depthSamplingView = depthSamplingView,
                    .cameraBuffer = deferredRasterCameraBuffer(p),
                    .cameraBufferSize = deferredRasterCameraBufferSize(p),
                    .screenExtent = deferred->swapchainExtent(),
                    .cameraNear = p.camera.nearPlane,
                    .cameraFar = p.camera.farPlane})
            .readiness;
      });

  graph.setPassReadiness(
      RenderPassId::GTAO, [deferred](const FrameRecordParams &p) {
        if (!displayModeRecordsGtao(deferred->displayMode())) {
          return renderPassNotNeeded();
        }
        if (!deferred->environmentManager() ||
            !deferred->environmentManager()->isGtaoReady() ||
            !deferred->environmentManager()->isAoEnabled()) {
          return renderPassNotNeeded();
        }
        if (!deferredRasterImageViewReady(
                p, DeferredRasterImageId::DepthSamplingView) ||
            !deferredRasterImageViewReady(p, DeferredRasterImageId::Normal) ||
            !deferredRasterCameraBufferReady(p) ||
            !deferredRasterGBufferSamplerReady(p)) {
          return renderPassMissingResource(RenderResourceId::SceneDepth);
        }
        return renderPassReady();
      });

  graph.setPassReadiness(
      RenderPassId::TransformGizmos,
      [](const FrameRecordParams &) { return renderPassNotNeeded(); });

  graph.setPassReadiness(
      RenderPassId::ExposureAdaptation, [deferred](const FrameRecordParams &p) {
        if (!displayModeRecordsExposureAdaptation(deferred->displayMode())) {
          return renderPassNotNeeded();
        }
        if (!deferred->exposureManager() ||
            !deferred->exposureManager()->isReady()) {
          return renderPassNotNeeded();
        }
        if (sanitizeExposureSettings(p.postProcess.exposureSettings).mode !=
            container::gpu::kExposureModeAuto) {
          return renderPassNotNeeded();
        }
        if (!deferredRasterImageViewReady(p,
                                          DeferredRasterImageId::SceneColor) ||
            !deferredRasterImageReady(p, DeferredRasterImageId::SceneColor)) {
          return renderPassMissingResource(RenderResourceId::SceneColor);
        }
        return renderPassReady();
      });

  graph.setPassReadiness(RenderPassId::OitResolve,
                         [transparentOit](const FrameRecordParams &p) {
                           return transparentOit.readiness(p);
                         });

  graph.setPassReadiness(RenderPassId::Bloom, [deferred](
                                                  const FrameRecordParams &p) {
    if (!displayModeRecordsBloom(deferred->displayMode()) ||
        !deferred->bloomManager() || !deferred->bloomManager()->isReady() ||
        !deferred->bloomManager()->enabled()) {
      return renderPassNotNeeded();
    }
    if (!deferredRasterImageViewReady(p, DeferredRasterImageId::SceneColor) ||
        !deferredRasterImageReady(p, DeferredRasterImageId::SceneColor)) {
      return renderPassMissingResource(RenderResourceId::SceneColor);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::PostProcess,
                         [deferred](const FrameRecordParams &p) {
    return deferredRasterPostProcessReadiness(p, *deferred);
  });

  graph.compile();
}

} // namespace container::renderer
