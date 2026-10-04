#include "Container/renderer/forward/ForwardRasterTechnique.h"
#include "Container/renderer/temporal/TemporalManager.h"

#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/deferred/DeferredRasterBimSurfacePassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterDepthReadOnlyTransitionRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/deferred/DeferredRasterFrustumCullPassPlanner.h"
#include "Container/renderer/deferred/DeferredRasterFrustumCullPassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterPostProcess.h"
#include "Container/renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.h"
#include "Container/renderer/deferred/DeferredRasterScenePassRecorder.h"
#include "Container/renderer/deferred/DeferredRasterTransformGizmo.h"
#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"
#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/effects/ExposureManager.h"
#include "Container/renderer/forward/ForwardRasterLightingPassRecorder.h"
#include "Container/renderer/forward/ForwardRasterPipelineBridge.h"
#include "Container/renderer/forward/ForwardRasterResourceBridge.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/SceneViewport.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowCullPassPlanner.h"
#include "Container/renderer/shadow/ShadowCullPassRecorder.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/shadow/ShadowPassRecorder.h"
#include "Container/renderer/shadow/ShadowPipelineBridge.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneData.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace container::renderer {

using container::gpu::kLocalShadowMapResolution;
using container::gpu::kMaxShadowedLocalLightLayers;
using container::gpu::kShadowCascadeCount;

namespace {

constexpr RenderTechniqueId kForwardRasterTechnique =
    RenderTechniqueId::ForwardRaster;

void registerForwardRasterFrameResources(FrameResourceRegistry &registry) {
  registry.clearTechnique(kForwardRasterTechnique);

  registry.registerExternal(kForwardRasterTechnique, "swapchain");
  registry.registerExternal(kForwardRasterTechnique, "scene-geometry");
  registry.registerExternal(kForwardRasterTechnique, "bim-geometry");
  registry.registerExternal(kForwardRasterTechnique, "shadow-atlas");
  registry.registerExternal(kForwardRasterTechnique, "local-shadow-atlas");
  registry.registerSampler(kForwardRasterTechnique, "depth-cull-sampler",
                           {}, FrameResourceLifetime::Imported);

  registry.registerBuffer(
      kForwardRasterTechnique, "camera-buffer",
      FrameBufferDesc{.size = sizeof(container::gpu::CameraData),
                      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
      FrameResourceLifetime::Imported);
  registry.registerBuffer(
      kForwardRasterTechnique, "scene-object-buffer",
      FrameBufferDesc{.size = 0, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::Imported);

  for (const char *descriptorName :
       {"scene-descriptor-set", "bim-scene-descriptor-set",
        "light-descriptor-set", "shadow-descriptor-set",
        "local-shadow-descriptor-set"}) {
    registry.registerDescriptorSet(kForwardRasterTechnique, descriptorName,
                                   FrameResourceLifetime::Imported);
  }
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "frame-lighting-descriptor-set");
  registry.registerDescriptorSet(kForwardRasterTechnique,
                                 "post-process-descriptor-set");
  registry.registerDescriptorSet(kForwardRasterTechnique, "oit-descriptor-set");

  registry.registerImage(
      kForwardRasterTechnique, "depth-stencil",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "depth-sampling-view",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "scene-color",
      FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "pick-id",
      FrameImageDesc{.format = VK_FORMAT_R32_UINT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "pick-depth",
      FrameImageDesc{.format = VK_FORMAT_UNDEFINED,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
  registry.registerImage(
      kForwardRasterTechnique, "oit-head-pointers",
      FrameImageDesc{.format = VK_FORMAT_R32_UINT,
                     .extent = {0, 0, 1},
                     .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT});

  registry.registerBuffer(
      kForwardRasterTechnique, "oit-node-buffer",
      FrameBufferDesc{.size = 0, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerBuffer(
      kForwardRasterTechnique, "oit-counter-buffer",
      FrameBufferDesc{.size = sizeof(uint32_t),
                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT},
      FrameResourceLifetime::PerFrame);
  registry.registerBuffer(
      kForwardRasterTechnique, "oit-metadata-buffer",
      FrameBufferDesc{.size = sizeof(OitMetadata),
                      .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
      FrameResourceLifetime::PerFrame);

  registry.registerFramebuffer(kForwardRasterTechnique,
                               "depth-prepass-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "bim-depth-prepass-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "transparent-pick-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kForwardRasterTechnique, "lighting-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "transparent-lighting-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 2u});
  registry.registerFramebuffer(kForwardRasterTechnique,
                               "transform-gizmo-framebuffer",
                               FrameFramebufferDesc{.attachmentCount = 1u});
}

void registerForwardRasterPipelineRecipes(PipelineRegistry &registry) {
  registry.clearTechnique(kForwardRasterTechnique);

  const auto registerGraphicsRecipe =
      [&registry](std::string_view name,
                  std::initializer_list<std::string_view> shaderStages,
                  std::string_view layoutName) {
        PipelineRecipe recipe{.key = {kForwardRasterTechnique,
                                      std::string(name)},
                              .kind = PipelineRecipeKind::Graphics,
                              .layoutName = std::string(layoutName)};
        recipe.shaderStages.reserve(shaderStages.size());
        for (std::string_view shaderStage : shaderStages) {
          recipe.shaderStages.emplace_back(shaderStage);
        }
        registry.registerRecipe(std::move(recipe));
      };
  const auto registerDepthRecipe = [&registerGraphicsRecipe](
                                       std::string_view name) {
    registerGraphicsRecipe(name,
                           {"spv_shaders/depth_prepass.vert.spv",
                            "spv_shaders/depth_prepass.frag.spv"},
                           "scene");
  };
  const auto registerTransparentRecipe = [&registerGraphicsRecipe](
                                            std::string_view name) {
    registerGraphicsRecipe(name,
                           {"spv_shaders/forward_transparent.vert.spv",
                            "spv_shaders/forward_transparent.frag.spv"},
                           "transparent");
  };
  const auto registerTransformGizmoRecipe = [&registerGraphicsRecipe](
                                               std::string_view name) {
    registerGraphicsRecipe(name,
                           {"spv_shaders/transform_gizmo.vert.spv",
                            "spv_shaders/transform_gizmo.frag.spv"},
                           "transform-gizmo");
  };

  registerDepthRecipe("depth-prepass");
  registerDepthRecipe("depth-prepass-front-cull");
  registerDepthRecipe("depth-prepass-no-cull");
  registerDepthRecipe("bim-depth-prepass");
  registerDepthRecipe("bim-depth-prepass-front-cull");
  registerDepthRecipe("bim-depth-prepass-no-cull");
  registerGraphicsRecipe("forward-opaque",
                         {"spv_shaders/forward_opaque.vert.spv",
                          "spv_shaders/forward_opaque.frag.spv"},
                         "transparent");
  registerTransparentRecipe("forward-transparent");
  registerTransparentRecipe("forward-transparent-front-cull");
  registerTransparentRecipe("forward-transparent-no-cull");
  registerGraphicsRecipe("post-process",
                         {"spv_shaders/post_process.vert.spv",
                          "spv_shaders/post_process.frag.spv"},
                         "post-process");
  registerGraphicsRecipe("light-gizmo",
                         {"spv_shaders/light_gizmo.vert.spv",
                          "spv_shaders/light_gizmo.frag.spv"},
                         "light-gizmo");
  registerGraphicsRecipe("light-gizmo-coverage",
                         {"spv_shaders/light_gizmo.vert.spv",
                          "spv_shaders/light_gizmo.frag.spv"},
                         "light-gizmo");
  registerTransformGizmoRecipe("transform-gizmo");
  registerTransformGizmoRecipe("transform-gizmo-solid");
  registerTransformGizmoRecipe("transform-gizmo-overlay");
  registerTransformGizmoRecipe("transform-gizmo-solid-overlay");
}

void addForwardShellPass(RenderGraph &graph, RenderPassId id,
                         std::initializer_list<RenderPassId> dependencies) {
  graph.addPass(id, dependencies,
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  (void)cmd;
                  (void)p;
                });
  graph.setPassResourceAccess(id, {}, {}, {});
  graph.setPassResourceTransitions(id, {});
}

[[nodiscard]] VkPipeline chooseForwardRasterPipeline(VkPipeline preferred,
                                                     VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

[[nodiscard]] SceneOpaqueDrawLists
forwardRasterSceneOpaqueDrawLists(const FrameDrawLists &draws) {
  return {.aggregate = draws.opaqueDrawCommands,
          .singleSided = draws.opaqueSingleSidedDrawCommands,
          .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = draws.opaqueDoubleSidedDrawCommands};
}

[[nodiscard]] BimSurfaceDrawLists
forwardRasterSurfaceDrawLists(const FrameDrawLists &draws) {
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

[[nodiscard]] BimSurfaceFramePassDrawSources
forwardRasterBimSurfaceDrawSources(const FrameBimResources &bim) {
  return {.mesh = forwardRasterSurfaceDrawLists(bim.draws),
          .pointPlaceholders = forwardRasterSurfaceDrawLists(bim.pointDraws),
          .curvePlaceholders = forwardRasterSurfaceDrawLists(bim.curveDraws),
          .opaqueMeshDrawsUseGpuVisibility =
              bim.opaqueMeshDrawsUseGpuVisibility,
          .transparentMeshDrawsUseGpuVisibility =
              bim.transparentMeshDrawsUseGpuVisibility};
}

[[nodiscard]] BimSurfaceFrameBinding forwardRasterBimSurfaceFrameBinding(
    const FrameBimResources &bim,
    std::span<const VkDescriptorSet> descriptorSets) {
  return buildBimSurfaceFrameBinding(
      {.draws = forwardRasterBimSurfaceDrawSources(bim),
       .vertexSlice = bim.scene.vertexSlice,
       .indexSlice = bim.scene.indexSlice,
       .indexType = bim.scene.indexType,
       .descriptorSets = descriptorSets,
       .semanticColorMode = bim.semanticColorMode});
}

[[nodiscard]] SceneRasterPassPipelineInputs
forwardRasterDepthPrepassPipelines(const FrameRecordParams &p) {
  const VkPipeline depthPipeline =
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::DepthPrepass);
  return {.primary = depthPipeline,
          .frontCull = chooseForwardRasterPipeline(
              forwardRasterPipelineHandle(
                  p, ForwardRasterPipelineId::DepthPrepassFrontCull),
              depthPipeline),
          .noCull = chooseForwardRasterPipeline(
              forwardRasterPipelineHandle(
                  p, ForwardRasterPipelineId::DepthPrepassNoCull),
              depthPipeline)};
}

[[nodiscard]] BimSurfaceRasterPassPipelines
forwardRasterBimDepthPrepassPipelines(const FrameRecordParams &p) {
  const VkPipeline depthPipeline =
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::BimDepthPrepass);
  return {.singleSided = depthPipeline,
          .windingFlipped = chooseForwardRasterPipeline(
              forwardRasterPipelineHandle(
                  p, ForwardRasterPipelineId::BimDepthPrepassFrontCull),
              depthPipeline),
          .doubleSided = chooseForwardRasterPipeline(
              forwardRasterPipelineHandle(
                  p, ForwardRasterPipelineId::BimDepthPrepassNoCull),
              depthPipeline)};
}

[[nodiscard]] bool
forwardRasterSceneDepthGeometryReady(const FrameRecordParams &p) {
  return forwardRasterDescriptorSetReady(p,
                                         ForwardRasterDescriptorSetId::Scene) &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] RenderPassReadiness
forwardRasterDepthPrepassReadiness(const FrameRecordParams &p) {
  const bool sceneOpaqueDraws = hasOpaqueDrawCommands(p.draws);
  const bool bimOpaqueDraws = hasBimOpaqueDrawCommands(p.bim);
  const bool transparentDraws = hasTransparentDrawCommands(p);
  if (!sceneOpaqueDraws && !bimOpaqueDraws && !transparentDraws) {
    return renderPassNotNeeded();
  }
  if (!forwardRasterRenderPassReady(p,
                                    ForwardRasterFramebufferId::DepthPrepass) ||
      !forwardRasterFramebufferReady(
          p, ForwardRasterFramebufferId::DepthPrepass)) {
    return renderPassMissingResource(RenderResourceId::SceneDepth);
  }
  if (p.pushConstants.bindless == nullptr) {
    return renderPassMissingResource(RenderResourceId::SceneGeometry);
  }
  if (!sceneOpaqueDraws) {
    return renderPassReady();
  }
  if (!forwardRasterSceneDepthGeometryReady(p) ||
      !forwardRasterPipelineReady(p, ForwardRasterPipelineId::DepthPrepass) ||
      !forwardRasterPipelineLayoutReady(p,
                                        ForwardRasterPipelineLayoutId::Scene)) {
    return renderPassMissingResource(RenderResourceId::SceneGeometry);
  }
  return renderPassReady();
}

void recordForwardRasterDepthPrepass(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  static_cast<void>(recordDeferredRasterScenePassCommands(
      cmd, {.kind = SceneRasterPassKind::DepthPrepass,
            .renderPass = forwardRasterRenderPass(
                p, ForwardRasterFramebufferId::DepthPrepass),
            .framebuffer = forwardRasterFramebuffer(
                p, ForwardRasterFramebufferId::DepthPrepass),
            .extent = sharedContext.swapchainExtent(),
            .draws = forwardRasterSceneOpaqueDrawLists(p.draws),
            .geometry = {.descriptorSet = forwardRasterDescriptorSet(
                             p, ForwardRasterDescriptorSetId::Scene),
                         .vertexSlice = p.scene.vertexSlice,
                         .indexSlice = p.scene.indexSlice,
                         .indexType = p.scene.indexType},
            .pipelines = forwardRasterDepthPrepassPipelines(p),
            .pipelineLayout = forwardRasterPipelineLayout(
                p, ForwardRasterPipelineLayoutId::Scene),
            .pushConstants = p.pushConstants.bindless,
            .imageIndex = p.runtime.imageIndex,
            .gpuCullManager = p.services.gpuCullManager,
            .frustumCullActive = p.services.gpuCullManager != nullptr &&
                p.services.gpuCullManager->frustumDrawsValid(p.runtime.imageIndex),
            .debugOverlay = sharedContext.debugOverlay()}));
}

[[nodiscard]] RenderPassReadiness
forwardRasterBimDepthPrepassReadiness(const FrameRecordParams &p) {
  if (!hasBimOpaqueDrawCommands(p.bim)) {
    return renderPassNotNeeded();
  }
  if (!forwardRasterRenderPassReady(
          p, ForwardRasterFramebufferId::BimDepthPrepass) ||
      !forwardRasterFramebufferReady(
          p, ForwardRasterFramebufferId::BimDepthPrepass)) {
    return renderPassMissingResource(RenderResourceId::SceneDepth);
  }
  if (p.pushConstants.bindless == nullptr ||
      !forwardRasterDescriptorSetReady(
          p, ForwardRasterDescriptorSetId::BimScene) ||
      p.bim.scene.vertexSlice.buffer == VK_NULL_HANDLE ||
      p.bim.scene.indexSlice.buffer == VK_NULL_HANDLE ||
      !forwardRasterPipelineReady(p,
                                  ForwardRasterPipelineId::BimDepthPrepass) ||
      !forwardRasterPipelineLayoutReady(p,
                                        ForwardRasterPipelineLayoutId::Scene)) {
    return renderPassMissingResource(RenderResourceId::BimGeometry);
  }
  return renderPassReady();
}

void recordForwardRasterBimDepthPrepass(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  const std::array<VkDescriptorSet, 1> descriptorSets = {
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::BimScene)};
  static_cast<void>(recordDeferredRasterBimSurfacePassCommands(
      cmd,
      {.kind = BimSurfacePassKind::DepthPrepass,
       .passReady = true,
       .renderPass = forwardRasterRenderPass(
           p, ForwardRasterFramebufferId::BimDepthPrepass),
       .framebuffer = forwardRasterFramebuffer(
           p, ForwardRasterFramebufferId::BimDepthPrepass),
       .extent = sharedContext.swapchainExtent(),
       .binding = forwardRasterBimSurfaceFrameBinding(p.bim, descriptorSets),
       .pipelines = forwardRasterBimDepthPrepassPipelines(p),
       .pipelineLayout =
           forwardRasterPipelineLayout(p, ForwardRasterPipelineLayoutId::Scene),
       .pushConstants = p.pushConstants.bindless,
       .debugOverlay = sharedContext.debugOverlay(),
       .bimManager = p.services.bimManager}));
}

[[nodiscard]] ShadowPassGeometryBinding
forwardRasterShadowGeometryBinding(VkDescriptorSet descriptorSet,
                                   const FrameSceneGeometry &scene) {
  return {.sceneDescriptorSet = descriptorSet,
          .vertexSlice = scene.vertexSlice,
          .indexSlice = scene.indexSlice,
          .indexType = scene.indexType};
}

[[nodiscard]] bool
forwardRasterLocalShadowSceneReady(const FrameRecordParams &p) {
  return forwardRasterDescriptorSetReady(p,
                                         ForwardRasterDescriptorSetId::Scene) &&
         p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasOpaqueDrawCommands(p.draws);
}

[[nodiscard]] bool
forwardRasterLocalShadowBimReady(const FrameRecordParams &p) {
  return forwardRasterDescriptorSetReady(
             p, ForwardRasterDescriptorSetId::BimScene) &&
         p.bim.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.bim.scene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasBimOpaqueDrawCommands(p.bim);
}

[[nodiscard]] ShadowPassDrawLists
forwardRasterLocalShadowSceneDraws(const FrameDrawLists &draws) {
  return {.singleSided = hasDrawCommands(draws.opaqueSingleSidedDrawCommands)
                             ? draws.opaqueSingleSidedDrawCommands
                             : draws.opaqueDrawCommands,
          .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = draws.opaqueDoubleSidedDrawCommands};
}

[[nodiscard]] ShadowPassDrawLists
forwardRasterLocalShadowBimDraws(const FrameBimResources &bim) {
  return {.singleSided =
              hasDrawCommands(bim.draws.opaqueSingleSidedDrawCommands)
                  ? bim.draws.opaqueSingleSidedDrawCommands
                  : bim.draws.opaqueDrawCommands,
          .windingFlipped = bim.draws.opaqueWindingFlippedDrawCommands,
          .doubleSided = bim.draws.opaqueDoubleSidedDrawCommands};
}

[[nodiscard]] bool
forwardRasterCanRecordLocalShadowPass(const FrameRecordParams &p) {
  return p.shadows.renderPass != VK_NULL_HANDLE &&
         p.shadows.localShadowFramebuffers != nullptr &&
         forwardRasterDescriptorSetReady(
             p, ForwardRasterDescriptorSetId::LocalShadow) &&
         p.shadows.localShadowLayerCount > 0u &&
         shadowPipelineReady(p, ShadowPipelineId::LocalDepth) &&
         shadowPipelineLayoutReady(p, ShadowPipelineLayoutId::Shadow) &&
         (forwardRasterLocalShadowSceneReady(p) ||
          forwardRasterLocalShadowBimReady(p));
}

[[nodiscard]] RenderPassReadiness
forwardRasterLocalShadowReadiness(const FrameRecordParams &p) {
  if (p.shadows.localShadowLayerCount == 0u) {
    return renderPassNotNeeded();
  }
  if (!forwardRasterCanRecordLocalShadowPass(p)) {
    return renderPassMissingResource(RenderResourceId::LocalShadowAtlas);
  }
  return renderPassReady();
}

void recordForwardRasterLocalShadowPass(VkCommandBuffer cmd,
                                        const FrameRecordParams &p) {
  if (!forwardRasterCanRecordLocalShadowPass(p)) {
    return;
  }

  const uint32_t layerCount =
      std::min(p.shadows.localShadowLayerCount, kMaxShadowedLocalLightLayers);
  const VkPipeline primaryPipeline =
      shadowPipelineHandle(p, ShadowPipelineId::LocalDepth);
  const VkPipeline frontCullPipeline = chooseForwardRasterPipeline(
      shadowPipelineHandle(p, ShadowPipelineId::LocalDepthFrontCull),
      primaryPipeline);
  const VkPipeline noCullPipeline = chooseForwardRasterPipeline(
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
         .raster = {.shadowAtlasVisible = true,
                    .shadowPassRecordable = true,
                    .extent = localShadowExtent},
         .renderPass = p.shadows.renderPass,
         .framebuffer = framebuffer,
         .extent = localShadowExtent,
         .scene = forwardRasterShadowGeometryBinding(
             forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Scene),
             p.scene),
         .bim = forwardRasterShadowGeometryBinding(
             forwardRasterDescriptorSet(p,
                                        ForwardRasterDescriptorSetId::BimScene),
             p.bim.scene),
         .shadowDescriptorSet = forwardRasterDescriptorSet(
             p, ForwardRasterDescriptorSetId::LocalShadow),
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
             .sceneGeometryReady = forwardRasterLocalShadowSceneReady(p),
             .bimGeometryReady = forwardRasterLocalShadowBimReady(p),
             .sceneGpuCullActive = false,
             .bimGpuFilteredMeshActive =
                 p.bim.opaqueMeshDrawsUseGpuVisibility &&
                 hasOpaqueDrawCommands(p.bim.draws),
             .sceneDraws = forwardRasterLocalShadowSceneDraws(p.draws),
             .bimDraws = forwardRasterLocalShadowBimDraws(p.bim)}}));
  }
}

[[nodiscard]] RenderPassReadiness
forwardRasterDepthReadOnlyReadiness(const FrameRecordParams &p) {
  if (!hasOpaqueDrawCommands(p.draws) && !hasBimOpaqueDrawCommands(p.bim) &&
      !hasTransparentDrawCommands(p)) {
    return renderPassNotNeeded();
  }
  return forwardRasterImageReady(p, ForwardRasterImageId::DepthStencil)
             ? renderPassReady()
             : renderPassMissingResource(RenderResourceId::SceneDepth);
}

void recordForwardRasterDepthReadOnlyTransition(VkCommandBuffer cmd,
                                                const FrameRecordParams &p) {
  const DeferredRasterDepthReadOnlyTransitionPlan transitionPlan =
      buildDeferredRasterDepthReadOnlyTransitionPlan(
          {.depthStencilImage =
               forwardRasterImage(p, ForwardRasterImageId::DepthStencil),
           .shadowAtlasImage = p.shadows.shadowManager != nullptr
                                   ? p.shadows.shadowManager->shadowAtlasImage()
                                   : VK_NULL_HANDLE,
           .shadowAtlasVisible = true,
           .shadowCascadeCount = kShadowCascadeCount,
           .localShadowAtlasImage =
               p.shadows.shadowManager != nullptr
                   ? p.shadows.shadowManager->localShadowAtlasImage()
                   : VK_NULL_HANDLE,
           .localShadowAtlasVisible = forwardRasterCanRecordLocalShadowPass(p),
           .localShadowLayerCount = std::min(p.shadows.localShadowLayerCount,
                                             kMaxShadowedLocalLightLayers)});
  static_cast<void>(
      recordDeferredRasterDepthReadOnlyTransitionCommands(cmd, transitionPlan));
}

[[nodiscard]] bool allForwardRasterDrawCommandsSingleInstance(
    const std::vector<DrawCommand> *commands) {
  if (commands == nullptr) {
    return false;
  }
  return std::ranges::all_of(*commands, [](const DrawCommand &command) {
    return command.instanceCount <= 1u;
  });
}

[[nodiscard]] std::array<VkDescriptorSet, 2>
forwardRasterPostProcessDescriptorSets(const FrameRecordParams &p) {
  return {
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::PostProcess),
      forwardRasterDescriptorSet(p, ForwardRasterDescriptorSetId::Oit)};
}

[[nodiscard]] bool forwardRasterHasTransparentWork(const FrameRecordParams &p) {
  return hasTransparentDrawCommands(p) || hasBimTransparentGeometry(p);
}

[[nodiscard]] DeferredTransparentOitFrameResourceInputs
forwardRasterOitInputs(const FrameRecordParams &p,
                       const OitManager *oitManager) {
  DeferredTransparentOitFrameResourceInputs inputs{};
  inputs.oitManager = oitManager;
  inputs.resources = {
      .headPointerImage =
          forwardRasterImage(p, ForwardRasterImageId::OitHeadPointers),
      .nodeBuffer = forwardRasterBuffer(p, ForwardRasterBufferId::OitNode),
      .counterBuffer =
          forwardRasterBuffer(p, ForwardRasterBufferId::OitCounter)};
  return inputs;
}

[[nodiscard]] bool forwardRasterOitResourcesReady(
    const DeferredTransparentOitFrameResourceInputs &inputs) {
  return inputs.oitManager != nullptr &&
         inputs.resources.headPointerImage != VK_NULL_HANDLE &&
         inputs.resources.nodeBuffer != VK_NULL_HANDLE &&
         inputs.resources.counterBuffer != VK_NULL_HANDLE;
}

[[nodiscard]] RenderPassReadiness forwardRasterOitReadiness(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  if (!forwardRasterHasTransparentWork(p)) {
    return renderPassNotNeeded();
  }
  if (!forwardRasterOitResourcesReady(
          forwardRasterOitInputs(p, sharedContext.oitManager()))) {
    return renderPassMissingResource(RenderResourceId::OitStorage);
  }
  return renderPassReady();
}

bool recordForwardRasterOitClear(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  if (!forwardRasterHasTransparentWork(p)) {
    return false;
  }
  return recordDeferredTransparentOitClearCommands(
      cmd, forwardRasterOitInputs(p, sharedContext.oitManager()));
}

bool recordForwardRasterOitResolvePreparation(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  if (!forwardRasterHasTransparentWork(p)) {
    return false;
  }
  return recordDeferredTransparentOitResolvePreparationCommands(
      cmd, forwardRasterOitInputs(p, sharedContext.oitManager()));
}

[[nodiscard]] bool forwardRasterSceneColorReady(const FrameRecordParams &p) {
  return forwardRasterImageReady(p, ForwardRasterImageId::SceneColor) &&
         forwardRasterImageViewReady(p, ForwardRasterImageId::SceneColor);
}

void recordForwardRasterSceneColorReadBarrier(VkCommandBuffer cmd,
                                              const FrameRecordParams &p) {
  const DeferredRasterSceneColorReadBarrierPlan sceneColorReadPlan =
      buildDeferredRasterSceneColorReadBarrierPlan(
          {.sceneColorImage =
               forwardRasterImage(p, ForwardRasterImageId::SceneColor)});
  static_cast<void>(recordDeferredRasterSceneColorReadBarrierCommands(
      cmd, sceneColorReadPlan));
}

[[nodiscard]] RenderPassReadiness forwardRasterExposureAdaptationReadiness(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  ExposureManager *exposureManager = sharedContext.exposureManager();
  if (exposureManager == nullptr || !exposureManager->isReady()) {
    return renderPassNotNeeded();
  }
  if (sanitizeExposureSettings(p.postProcess.exposureSettings).mode !=
      container::gpu::kExposureModeAuto) {
    return renderPassNotNeeded();
  }
  if (!forwardRasterSceneColorReady(p)) {
    return renderPassMissingResource(RenderResourceId::SceneColor);
  }
  return renderPassReady();
}

void recordForwardRasterExposureAdaptation(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  ExposureManager *exposureManager = sharedContext.exposureManager();
  if (exposureManager == nullptr || !exposureManager->isReady() ||
      !forwardRasterSceneColorReady(p)) {
    return;
  }
  const container::gpu::ExposureSettings exposureSettings =
      sanitizeExposureSettings(p.postProcess.exposureSettings);
  if (exposureSettings.mode != container::gpu::kExposureModeAuto) {
    return;
  }

  recordForwardRasterSceneColorReadBarrier(cmd, p);
  const auto extent = sharedContext.swapchainExtent();
  exposureManager->dispatch(
      p.runtime.imageIndex, cmd,
      temporalSceneColorView(
          p, forwardRasterImageView(p, ForwardRasterImageId::SceneColor)), extent.width,
      extent.height, exposureSettings,
      temporalSceneColorLayout(p));
}

[[nodiscard]] RenderPassReadiness forwardRasterBloomReadiness(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  BloomManager *bloomManager = sharedContext.bloomManager();
  if (bloomManager == nullptr || !bloomManager->isReady() ||
      !bloomManager->enabled()) {
    return renderPassNotNeeded();
  }
  if (!forwardRasterSceneColorReady(p)) {
    return renderPassMissingResource(RenderResourceId::SceneColor);
  }
  return renderPassReady();
}

void recordForwardRasterBloom(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  BloomManager *bloomManager = sharedContext.bloomManager();
  if (bloomManager == nullptr || !bloomManager->isReady() ||
      !bloomManager->enabled() || !forwardRasterSceneColorReady(p)) {
    return;
  }

  recordForwardRasterSceneColorReadBarrier(cmd, p);
  const auto extent = sharedContext.swapchainExtent();
  bloomManager->dispatch(
      cmd,
      temporalSceneColorView(
          p, forwardRasterImageView(p, ForwardRasterImageId::SceneColor)),
      extent.width, extent.height, temporalSceneColorLayout(p));
}

[[nodiscard]] bool forwardRasterLightGizmoOverlayReady(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  const LightingManager *lightingManager = sharedContext.lightingManager();
  const container::ui::GuiManager *guiManager = sharedContext.guiManager();
  if (lightingManager == nullptr || guiManager == nullptr ||
      !guiManager->showLightGizmos()) {
    return false;
  }
  return lightingManager->lightGizmoIconsReady() &&
         lightingManager->lightGizmoIconDescriptorSet() != VK_NULL_HANDLE &&
         forwardRasterDescriptorSetReady(
             p, ForwardRasterDescriptorSetId::FrameLighting) &&
         forwardRasterPipelineReady(p, ForwardRasterPipelineId::LightGizmo) &&
         forwardRasterPipelineReady(
             p, ForwardRasterPipelineId::LightGizmoCoverage) &&
         forwardRasterPipelineLayoutReady(
             p, ForwardRasterPipelineLayoutId::LightGizmo);
}

void recordForwardRasterLightGizmoOverlay(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext, VkExtent2D extent) {
  const LightingManager *lightingManager = sharedContext.lightingManager();
  if (cmd == VK_NULL_HANDLE ||
      !forwardRasterLightGizmoOverlayReady(p, sharedContext) ||
      lightingManager == nullptr) {
    return;
  }

  const std::array<VkDescriptorSet, 2> descriptorSets = {
      forwardRasterDescriptorSet(p,
                                 ForwardRasterDescriptorSetId::FrameLighting),
      lightingManager->lightGizmoIconDescriptorSet()};
  recordSceneViewportAndScissor(cmd, extent);
  lightingManager->drawLightGizmos(
      cmd, descriptorSets,
      forwardRasterPipelineHandle(p, ForwardRasterPipelineId::LightGizmo),
      forwardRasterPipelineHandle(p,
                                  ForwardRasterPipelineId::LightGizmoCoverage),
      forwardRasterPipelineLayout(p, ForwardRasterPipelineLayoutId::LightGizmo),
      sharedContext.camera());
}

[[nodiscard]] DeferredTransformGizmoDrawInputs
forwardRasterTransformGizmoDrawInputs(VkCommandBuffer cmd,
                                      const FrameRecordParams &p,
                                      VkExtent2D extent, VkPipeline pipeline,
                                      VkPipeline solidPipeline) {
  return {.commandBuffer = cmd,
          .extent = extent,
          .gizmo = p.transformGizmo,
          .wideLinesSupported = p.debug.wireframeWideLinesSupported,
          .lightingDescriptorSet = forwardRasterDescriptorSet(
              p, ForwardRasterDescriptorSetId::FrameLighting),
          .pipelineLayout = forwardRasterPipelineLayout(
              p, ForwardRasterPipelineLayoutId::TransformGizmo),
          .pipeline = pipeline,
          .solidPipeline = solidPipeline,
          .pushConstants = p.pushConstants.transformGizmo};
}

void recordForwardRasterTransformGizmoOverlay(VkCommandBuffer cmd,
                                              const FrameRecordParams &p,
                                              VkExtent2D extent) {
  recordDeferredTransformGizmoOverlay(forwardRasterTransformGizmoDrawInputs(
      cmd, p, extent,
      forwardRasterPipelineHandle(
          p, ForwardRasterPipelineId::TransformGizmoOverlay),
      forwardRasterPipelineHandle(
          p, ForwardRasterPipelineId::TransformGizmoSolidOverlay)));
}

[[nodiscard]] RenderPassReadiness forwardRasterPostProcessReadiness(
    const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  const VkExtent2D extent = sharedContext.swapchainExtent();
  const auto descriptorSets = forwardRasterPostProcessDescriptorSets(p);
  const bool descriptorSetsReady =
      std::ranges::all_of(descriptorSets, [](VkDescriptorSet descriptorSet) {
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
      !forwardRasterPipelineReady(p, ForwardRasterPipelineId::PostProcess) ||
      !forwardRasterPipelineLayoutReady(
          p, ForwardRasterPipelineLayoutId::PostProcess)) {
    return renderPassMissingResource(RenderResourceId::SwapchainImage);
  }

  return renderPassReady();
}

void recordForwardRasterPostProcessPass(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const DeferredRasterFrameGraphContext &sharedContext) {
  const VkExtent2D extent = sharedContext.swapchainExtent();
  const auto postProcessSets = forwardRasterPostProcessDescriptorSets(p);
  const container::gpu::ExposureSettings exposureSettings =
      sanitizeExposureSettings(p.postProcess.exposureSettings);
  BloomManager *bloomManager = sharedContext.bloomManager();
  ExposureManager *exposureManager = sharedContext.exposureManager();
  const LightingManager *lightingManager = sharedContext.lightingManager();
  static_cast<void>(recordDeferredPostProcessPassCommands(
      {.commandBuffer = cmd,
       .renderPass = p.postProcess.renderPass,
       .swapChainFramebuffers = p.swapchain.swapChainFramebuffers,
       .imageIndex = p.runtime.imageIndex,
       .extent = extent,
       .pipeline =
           forwardRasterPipelineHandle(p, ForwardRasterPipelineId::PostProcess),
       .pipelineLayout = forwardRasterPipelineLayout(
           p, ForwardRasterPipelineLayoutId::PostProcess),
       .descriptorSets = postProcessSets,
       .frameInputs =
           {.displayMode = sharedContext.displayMode(),
            .bloomPassActive = sharedContext.isPassActive(RenderPassId::Bloom),
            .bloomReady = bloomManager != nullptr && bloomManager->isReady(),
            .bloomEnabled = bloomManager != nullptr && bloomManager->enabled(),
            .bloomIntensity =
                bloomManager != nullptr ? bloomManager->intensity() : 0.0f,
            .exposureSettings = exposureSettings,
            .resolvedExposure =
                exposureManager != nullptr
                    ? exposureManager->resolvedExposure(exposureSettings)
                    : resolvePostProcessExposure(exposureSettings),
            .cameraNear = p.camera.nearPlane,
            .cameraFar = p.camera.farPlane,
            .shadowData = p.shadows.shadowData,
            .tileCullPassActive = false,
            .tiledLightingReady = false,
            .pointLightCount =
                lightingManager != nullptr
                    ? static_cast<uint32_t>(
                          lightingManager->pointLightsSsbo().size())
                    : 0u,
            .transparentOitActive =
                sharedContext.isPassActive(RenderPassId::OitResolve)},
       .recordAfterFullscreenDraw = [&sharedContext,
                                     &p](VkCommandBuffer passCmd) {
         recordForwardRasterLightGizmoOverlay(passCmd, p, sharedContext,
                                              sharedContext.swapchainExtent());
         recordForwardRasterTransformGizmoOverlay(
             passCmd, p, sharedContext.swapchainExtent());
         sharedContext.renderGui(passCmd);
       }}));
}

void addForwardShadowCascadePass(
    RenderGraph &graph, DeferredRasterFrameGraphContext &sharedContext,
    RenderPassId id, uint32_t cascadeIndex,
    std::initializer_list<RenderPassId> dependencies) {
  graph.addPass(id, dependencies,
                [&sharedContext, cascadeIndex](VkCommandBuffer cmd,
                                               const FrameRecordParams &p) {
                  sharedContext.recordForwardShadowPass(cmd, p, cascadeIndex);
                });
  graph.setPassReadiness(
      id, [&sharedContext, cascadeIndex](const FrameRecordParams &p) {
        return sharedContext.canRecordForwardShadowPass(p, cascadeIndex)
                   ? renderPassReady()
                   : renderPassMissingResource(RenderResourceId::ShadowAtlas);
      });
}

void addForwardShadowCullPass(
    RenderGraph &graph, RenderPassId id, uint32_t cascadeIndex,
    std::initializer_list<RenderPassId> dependencies) {
  auto buildPlan = [cascadeIndex](const FrameRecordParams &p) {
    const uint32_t sourceDrawCount =
        p.draws.opaqueSingleSidedDrawCommands != nullptr
            ? static_cast<uint32_t>(
                  p.draws.opaqueSingleSidedDrawCommands->size())
            : 0u;
    return buildShadowCullPassPlan(
        {.shadowAtlasVisible = true,
         .gpuShadowCullEnabled = p.shadows.useGpuShadowCull,
         .shadowCullManagerReady = p.shadows.shadowCullManager != nullptr &&
                                   p.shadows.shadowCullManager->isReady(),
         .sceneSingleSidedDrawsAvailable =
             hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands),
         .sourceDrawCommandsAllSingleInstance =
             allForwardRasterDrawCommandsSingleInstance(
                 p.draws.opaqueSingleSidedDrawCommands),
         .cameraBufferReady =
             forwardRasterBufferReady(p, ForwardRasterBufferId::Camera),
         .cascadeIndexInRange = cascadeIndex < kShadowCascadeCount,
         .sourceDrawCount = sourceDrawCount});
  };

  graph.addPass(id, dependencies,
                [cascadeIndex, buildPlan](VkCommandBuffer cmd,
                                          const FrameRecordParams &p) {
                  static_cast<void>(recordShadowCullPassCommands(
                      cmd, {.shadowCullManager = p.shadows.shadowCullManager,
                            .plan = buildPlan(p),
                            .imageIndex = p.runtime.imageIndex,
                            .cascadeIndex = cascadeIndex}));
                });
  graph.setPassReadiness(id, [buildPlan](const FrameRecordParams &p) {
    return buildPlan(p).readiness;
  });
}

} // namespace

std::string_view ForwardRasterTechnique::name() const {
  return renderTechniqueName(id());
}

std::string_view ForwardRasterTechnique::displayName() const {
  return renderTechniqueDisplayName(id());
}

RenderTechniqueAvailability
ForwardRasterTechnique::availability(const RenderSystemContext &context) const {
  if (context.frameRecorder == nullptr && context.deferredRaster == nullptr) {
    return RenderTechniqueAvailability::unavailable(
        "forward rendering requires frame recorder and shared raster services");
  }
  if (context.frameRecorder == nullptr) {
    return RenderTechniqueAvailability::unavailable(
        "forward rendering requires frame recorder");
  }
  if (context.deferredRaster == nullptr) {
    return RenderTechniqueAvailability::unavailable(
        "forward rendering requires shared raster services");
  }
  return RenderTechniqueAvailability::availableNow();
}

TechniqueDebugModel ForwardRasterTechnique::debugModel() const {
  TechniqueDebugModel model{};
  model.techniqueName = std::string(name());
  model.displayName = std::string(displayName());
  model.displayModes = {
      {.id = "lit",
       .label = "Lit",
       .value = static_cast<uint32_t>(container::ui::GBufferViewMode::Lit)},
      {.id = "depth",
       .label = "Depth",
       .value = static_cast<uint32_t>(container::ui::GBufferViewMode::Depth)},
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
      {.id = "shadow-cascades",
       .label = "Shadow Cascades",
       .value = static_cast<uint32_t>(
           container::ui::GBufferViewMode::ShadowCascades)},
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
      .id = "forward-frame",
      .title = "Forward Frame",
      .controls = {
          TechniqueDebugControl{.id = "render-graph",
                                .label = "Render graph",
                                .kind = TechniqueDebugControlKind::Action},
          TechniqueDebugControl{.id = "depth-prepass",
                                .label = "Depth prepass",
                                .kind = TechniqueDebugControlKind::Action},
          TechniqueDebugControl{.id = "forward-lighting",
                                .label = "Forward lighting",
                                .kind = TechniqueDebugControlKind::Action},
          TechniqueDebugControl{.id = "transparent-oit",
                                .label = "Transparent OIT",
                                .kind = TechniqueDebugControlKind::Action},
      }});
  return model;
}

void ForwardRasterTechnique::registerTechniqueContracts(
    RenderSystemContext &context) {
  if (context.frameResources != nullptr) {
    registerForwardRasterFrameResources(*context.frameResources);
  }
  if (context.pipelines != nullptr) {
    registerForwardRasterPipelineRecipes(*context.pipelines);
  }
}

void ForwardRasterTechnique::buildFrameGraph(RenderSystemContext &context) {
  registerTechniqueContracts(context);

  if (context.frameRecorder == nullptr) {
    return;
  }

  RenderGraph &graph = context.frameRecorder->graph();
  graph.clear();

  graph.addPass(RenderPassId::FrustumCull, {},
                [](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const auto* camera = forwardRasterBufferBinding(p, ForwardRasterBufferId::Camera);
    const auto* draws = p.draws.opaqueSingleSidedDrawCommands;
    auto* culling = p.services.gpuCullManager;
    const auto plan = buildDeferredRasterFrustumCullPassPlan({
        .gpuCullManagerReady = culling != nullptr && culling->isReady(),
        .sceneSingleSidedDrawsAvailable = draws != nullptr && !draws->empty(),
        .cameraBufferReady = camera != nullptr && camera->buffer != VK_NULL_HANDLE,
        .objectBufferReady = p.scene.objectBuffer != VK_NULL_HANDLE && p.scene.objectBufferSize > 0u,
        .debugFreezeCulling = p.debug.debugFreezeCulling,
        .cullingFrozen = culling != nullptr && culling->cullingFrozen(),
        .sourceDrawCount = draws != nullptr ? static_cast<uint32_t>(draws->size()) : 0u});
    if (!plan.active || camera == nullptr) return;
    static_cast<void>(recordDeferredRasterFrustumCullPassCommands(cmd, {
        .gpuCullManager = culling, .plan = plan, .drawCommands = draws,
        .imageIndex = p.runtime.imageIndex, .cameraBuffer = camera->buffer,
        .cameraBufferSize = camera->size, .objectBuffer = p.scene.objectBuffer,
        .objectBufferSize = p.scene.objectBufferSize,
        .drawSourceRevision = p.scene.objectDataRevision}));
  });
  graph.setPassResourceAccess(RenderPassId::FrustumCull,
      {RenderResourceId::SceneGeometry, RenderResourceId::CameraBuffer, RenderResourceId::ObjectBuffer},
      {}, {RenderResourceId::FrustumCullDraws, RenderResourceId::CullStats});

  if (context.deferredRaster != nullptr) {
    graph.addPass(RenderPassId::DepthPrepass, {RenderPassId::FrustumCull},
                  [sharedContext = context.deferredRaster](
                      VkCommandBuffer cmd, const FrameRecordParams &p) {
                    recordForwardRasterDepthPrepass(cmd, p, *sharedContext);
                  });
    graph.setPassReadiness(RenderPassId::DepthPrepass,
                           [](const FrameRecordParams &p) {
                             return forwardRasterDepthPrepassReadiness(p);
                           });
    graph.addPass(RenderPassId::BimDepthPrepass, {RenderPassId::DepthPrepass},
                  [sharedContext = context.deferredRaster](
                      VkCommandBuffer cmd, const FrameRecordParams &p) {
                    recordForwardRasterBimDepthPrepass(cmd, p, *sharedContext);
                  });
    graph.setPassReadiness(RenderPassId::BimDepthPrepass,
                           [](const FrameRecordParams &p) {
                             return forwardRasterBimDepthPrepassReadiness(p);
                           });
  } else {
    graph.addPass(RenderPassId::DepthPrepass, {RenderPassId::FrustumCull},
                  [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                    (void)cmd;
                    (void)p;
                  });
    graph.setPassReadiness(
        RenderPassId::DepthPrepass,
        [](const FrameRecordParams &) { return renderPassNotNeeded(); });
    graph.addPass(RenderPassId::BimDepthPrepass, {RenderPassId::DepthPrepass},
                  [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                    (void)cmd;
                    (void)p;
                  });
    graph.setPassReadiness(
        RenderPassId::BimDepthPrepass,
        [](const FrameRecordParams &) { return renderPassNotNeeded(); });
  }
  const auto forwardShadowCullPassIds = shadowCullPassIds();
  if (context.deferredRaster != nullptr) {
    addForwardShadowCullPass(graph, forwardShadowCullPassIds[0], 0u,
                             {RenderPassId::BimDepthPrepass});
    addForwardShadowCullPass(graph, forwardShadowCullPassIds[1], 1u,
                             {RenderPassId::BimDepthPrepass});
    addForwardShadowCullPass(graph, forwardShadowCullPassIds[2], 2u,
                             {RenderPassId::BimDepthPrepass});
    addForwardShadowCullPass(graph, forwardShadowCullPassIds[3], 3u,
                             {RenderPassId::BimDepthPrepass});
    addForwardShadowCascadePass(
        graph, *context.deferredRaster, RenderPassId::ShadowCascade0, 0u,
        {RenderPassId::BimDepthPrepass, forwardShadowCullPassIds[0]});
    addForwardShadowCascadePass(
        graph, *context.deferredRaster, RenderPassId::ShadowCascade1, 1u,
        {RenderPassId::ShadowCascade0, forwardShadowCullPassIds[1]});
    addForwardShadowCascadePass(
        graph, *context.deferredRaster, RenderPassId::ShadowCascade2, 2u,
        {RenderPassId::ShadowCascade1, forwardShadowCullPassIds[2]});
    addForwardShadowCascadePass(
        graph, *context.deferredRaster, RenderPassId::ShadowCascade3, 3u,
        {RenderPassId::ShadowCascade2, forwardShadowCullPassIds[3]});
  } else {
    addForwardShellPass(graph, RenderPassId::ShadowCascade0,
                        {RenderPassId::BimDepthPrepass});
    addForwardShellPass(graph, RenderPassId::ShadowCascade1,
                        {RenderPassId::ShadowCascade0});
    addForwardShellPass(graph, RenderPassId::ShadowCascade2,
                        {RenderPassId::ShadowCascade1});
    addForwardShellPass(graph, RenderPassId::ShadowCascade3,
                        {RenderPassId::ShadowCascade2});
  }
  graph.addPass(RenderPassId::LocalShadowDepth, {RenderPassId::ShadowCascade3},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  recordForwardRasterLocalShadowPass(cmd, p);
                });
  graph.setPassReadiness(RenderPassId::LocalShadowDepth,
                         [](const FrameRecordParams &p) {
                           return forwardRasterLocalShadowReadiness(p);
                         });

  graph.addPass(RenderPassId::DepthToReadOnly, {RenderPassId::LocalShadowDepth},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  recordForwardRasterDepthReadOnlyTransition(cmd, p);
                });
  graph.setPassReadiness(RenderPassId::DepthToReadOnly,
                         [](const FrameRecordParams &p) {
                           return forwardRasterDepthReadOnlyReadiness(p);
                         });
  graph.addPass(RenderPassId::HiZGenerate, {RenderPassId::DepthToReadOnly},
                [](VkCommandBuffer cmd, const FrameRecordParams& p) {
    auto* culling = p.services.gpuCullManager;
    const auto depth = forwardRasterImageView(p, ForwardRasterImageId::DepthSamplingView);
    const auto sampler = p.sampler(kForwardRasterTechnique, "depth-cull-sampler");
    if (culling == nullptr || !culling->isReady() || depth == VK_NULL_HANDLE ||
        sampler == VK_NULL_HANDLE || !culling->frustumDrawsValid(p.runtime.imageIndex)) return;
    const auto* binding = forwardRasterImageBinding(p, ForwardRasterImageId::DepthSamplingView);
    const auto extent = binding->extent;
    culling->ensureHiZImage(p.runtime.imageIndex, extent.width, extent.height);
    culling->dispatchHiZGenerate(cmd, p.runtime.imageIndex, depth, sampler, extent.width, extent.height);
  });
  graph.setPassResourceAccess(RenderPassId::HiZGenerate,
      {RenderResourceId::SceneDepth}, {}, {RenderResourceId::HiZPyramid});
  graph.addPass(RenderPassId::OcclusionCull, {RenderPassId::HiZGenerate},
                [](VkCommandBuffer cmd, const FrameRecordParams& p) {
    auto* culling = p.services.gpuCullManager;
    const auto* camera = forwardRasterBufferBinding(p, ForwardRasterBufferId::Camera);
    const auto* draws = p.draws.opaqueSingleSidedDrawCommands;
    if (culling == nullptr || !culling->hizGeneratedThisFrame(p.runtime.imageIndex) ||
        !culling->canRecordOcclusionCull(p.runtime.imageIndex) || camera == nullptr ||
        draws == nullptr || draws->empty()) return;
    culling->dispatchOcclusionCull(cmd, p.runtime.imageIndex, camera->buffer,
        camera->size, static_cast<uint32_t>(draws->size()));
  });
  graph.setPassResourceAccess(RenderPassId::OcclusionCull,
      {RenderResourceId::HiZPyramid, RenderResourceId::FrustumCullDraws,
       RenderResourceId::CameraBuffer, RenderResourceId::ObjectBuffer},
      {}, {RenderResourceId::OcclusionCullDraws, RenderResourceId::CullStats});
  graph.addPass(RenderPassId::CullStatsReadback, {RenderPassId::OcclusionCull},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  if (auto *culling = p.services.gpuCullManager;
                      culling != nullptr && culling->isReady()) {
                    culling->scheduleStatsReadback(cmd, p.runtime.imageIndex);
                  }
                });
  graph.setPassReadiness(RenderPassId::CullStatsReadback,
                        [](const FrameRecordParams &p) {
                          return p.services.gpuCullManager != nullptr &&
                                         p.services.gpuCullManager->isReady()
                                     ? renderPassReady()
                                     : renderPassNotNeeded();
                        });
  if (context.deferredRaster != nullptr) {
    graph.addPass(RenderPassId::OitClear, {RenderPassId::CullStatsReadback},
                  [sharedContext = context.deferredRaster](
                      VkCommandBuffer cmd, const FrameRecordParams &p) {
                    static_cast<void>(
                        recordForwardRasterOitClear(cmd, p, *sharedContext));
                  });
    graph.setPassReadiness(
        RenderPassId::OitClear,
        [sharedContext = context.deferredRaster](const FrameRecordParams &p) {
          return forwardRasterOitReadiness(p, *sharedContext);
        });
    graph.setPassResourceAccess(RenderPassId::OitClear, {}, {},
                                {RenderResourceId::OitStorage});
    graph.setPassResourceTransitions(RenderPassId::OitClear, {});
  } else {
    addForwardShellPass(graph, RenderPassId::OitClear,
                        {RenderPassId::CullStatsReadback});
  }
  graph.addPass(RenderPassId::Lighting, {RenderPassId::OitClear},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  (void)recordForwardRasterLightingPassCommands(cmd, p);
                });
  graph.setPassReadiness(RenderPassId::Lighting,
                         [](const FrameRecordParams &p) {
                           return checkForwardRasterLightingPassReadiness(p);
                         });
  graph.setPassResourceAccess(
      RenderPassId::Lighting,
      {RenderResourceId::SceneGeometry, RenderResourceId::BimGeometry,
       RenderResourceId::CameraBuffer, RenderResourceId::ObjectBuffer,
       RenderResourceId::BimObjectBuffer, RenderResourceId::LightingData,
       RenderResourceId::EnvironmentMaps, RenderResourceId::SceneDepth},
      {RenderResourceId::ShadowAtlas, RenderResourceId::LocalShadowAtlas,
       RenderResourceId::OitStorage, RenderResourceId::FrustumCullDraws,
       RenderResourceId::OcclusionCullDraws},
      {RenderResourceId::SceneColor, RenderResourceId::OitStorage});
  graph.setPassResourceTransitions(RenderPassId::Lighting, {});
  RenderGraphBuilder temporalGraph(graph);
  registerTemporalPasses(temporalGraph);

  graph.addPass(RenderPassId::TransformGizmos, {RenderPassId::TemporalResolve},
                [](VkCommandBuffer cmd, const FrameRecordParams &p) {
                  (void)cmd;
                  (void)p;
                });
  graph.setPassReadiness(
      RenderPassId::TransformGizmos,
      [](const FrameRecordParams &) { return renderPassNotNeeded(); });

  if (context.deferredRaster != nullptr) {
    graph.addPass(
        RenderPassId::ExposureAdaptation, {RenderPassId::TransformGizmos},
        [sharedContext = context.deferredRaster](VkCommandBuffer cmd,
                                                 const FrameRecordParams &p) {
          recordForwardRasterExposureAdaptation(cmd, p, *sharedContext);
        });
    graph.setPassReadiness(
        RenderPassId::ExposureAdaptation,
        [sharedContext = context.deferredRaster](const FrameRecordParams &p) {
          return forwardRasterExposureAdaptationReadiness(p, *sharedContext);
        });
  } else {
    addForwardShellPass(graph, RenderPassId::ExposureAdaptation,
                        {RenderPassId::TransformGizmos});
  }
  if (context.deferredRaster != nullptr) {
    graph.addPass(RenderPassId::OitResolve, {RenderPassId::ExposureAdaptation},
                  [sharedContext = context.deferredRaster](
                      VkCommandBuffer cmd, const FrameRecordParams &p) {
                    static_cast<void>(recordForwardRasterOitResolvePreparation(
                        cmd, p, *sharedContext));
                  });
    graph.setPassReadiness(
        RenderPassId::OitResolve,
        [sharedContext = context.deferredRaster](const FrameRecordParams &p) {
          return forwardRasterOitReadiness(p, *sharedContext);
        });
    graph.setPassResourceAccess(RenderPassId::OitResolve,
                                {RenderResourceId::OitStorage}, {}, {});
    graph.setPassResourceTransitions(RenderPassId::OitResolve, {});
  } else {
    addForwardShellPass(graph, RenderPassId::OitResolve,
                        {RenderPassId::ExposureAdaptation});
  }
  if (context.deferredRaster != nullptr) {
    graph.addPass(RenderPassId::Bloom, {RenderPassId::OitResolve},
                  [sharedContext = context.deferredRaster](
                      VkCommandBuffer cmd, const FrameRecordParams &p) {
                    recordForwardRasterBloom(cmd, p, *sharedContext);
                  });
    graph.setPassReadiness(
        RenderPassId::Bloom,
        [sharedContext = context.deferredRaster](const FrameRecordParams &p) {
          return forwardRasterBloomReadiness(p, *sharedContext);
        });
  } else {
    addForwardShellPass(graph, RenderPassId::Bloom, {RenderPassId::OitResolve});
  }
  if (context.deferredRaster != nullptr) {
    graph.addPass(RenderPassId::PostProcess, {RenderPassId::Bloom},
                  [sharedContext = context.deferredRaster](
                      VkCommandBuffer cmd, const FrameRecordParams &p) {
                    recordForwardRasterPostProcessPass(cmd, p, *sharedContext);
                  });
    graph.setPassReadiness(
        RenderPassId::PostProcess,
        [sharedContext = context.deferredRaster](const FrameRecordParams &p) {
          return forwardRasterPostProcessReadiness(p, *sharedContext);
        });
    graph.setPassResourceAccess(
        RenderPassId::PostProcess,
        {RenderResourceId::SceneColor, RenderResourceId::CameraBuffer,
         RenderResourceId::SceneDepth},
        {RenderResourceId::TemporalColor, RenderResourceId::OitStorage, RenderResourceId::BloomTexture,
         RenderResourceId::ExposureState, RenderResourceId::ShadowAtlas},
        {RenderResourceId::SwapchainImage});
    graph.setPassResourceTransitions(
        RenderPassId::PostProcess,
        {{RenderResourceId::SwapchainImage, RenderResourceState::Present,
          RenderResourceState::ColorAttachment}});
  } else {
    addForwardShellPass(graph, RenderPassId::PostProcess,
                        {RenderPassId::Bloom});
  }
  graph.compile();
}

} // namespace container::renderer
