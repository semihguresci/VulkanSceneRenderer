#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/forward/ForwardRasterTechnique.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using container::renderer::DeferredRasterFrameGraphContext;
using container::renderer::DeferredRasterFrameGraphServices;
using container::renderer::ForwardRasterTechnique;
using container::renderer::FrameRecorder;
using container::renderer::FrameRecordParams;
using container::renderer::FrameResourceRegistry;
using container::renderer::PipelineRecipeKind;
using container::renderer::PipelineRegistry;
using container::renderer::RenderGraph;
using container::renderer::RenderGraphDebugModel;
using container::renderer::RenderPassId;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;
using container::renderer::RenderSystemContext;
using container::renderer::RenderTechniqueId;
using container::renderer::TechniquePipelineKey;
using container::renderer::TechniqueResourceKey;

bool hasPass(const RenderGraphDebugModel &model, std::string_view passName) {
  return std::ranges::any_of(model.passes, [passName](const auto &pass) {
    return pass.passName == passName;
  });
}

std::filesystem::path repoPath(const std::filesystem::path &relativePath) {
  return std::filesystem::path(CONTAINER_SOURCE_DIR) / relativePath;
}

std::string readRepoTextFile(const std::filesystem::path &relativePath) {
  const std::filesystem::path path = repoPath(relativePath);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

bool resourceListContains(const std::vector<RenderResourceId> &resources,
                          RenderResourceId resource) {
  return std::ranges::find(resources, resource) != resources.end();
}

size_t executionPosition(const RenderGraph &graph, RenderPassId id) {
  const auto order = graph.executionPassIds();
  const auto it = std::ranges::find(order, id);
  if (it == order.end()) {
    return order.size();
  }
  return static_cast<size_t>(std::distance(order.begin(), it));
}

std::string_view lightingResourceAccessBlock(std::string_view source) {
  const std::string_view marker =
      "graph.setPassResourceAccess(\n      RenderPassId::Lighting";
  const size_t begin = source.find(marker);
  if (begin == std::string_view::npos) {
    return {};
  }
  const size_t end = source.find("graph.setPassResourceTransitions", begin);
  if (end == std::string_view::npos) {
    return source.substr(begin);
  }
  return source.substr(begin, end - begin);
}

} // namespace

TEST(ForwardRasterTechniqueTests, PublishesForwardContracts) {
  FrameResourceRegistry frameResources;
  PipelineRegistry pipelines;
  ForwardRasterTechnique technique;
  RenderSystemContext context{
      .frameResources = &frameResources,
      .pipelines = &pipelines,
  };

  technique.registerTechniqueContracts(context);

  EXPECT_TRUE(frameResources.contains(
      TechniqueResourceKey{RenderTechniqueId::ForwardRaster, "scene-color"}));
  EXPECT_TRUE(frameResources.contains(
      TechniqueResourceKey{RenderTechniqueId::ForwardRaster, "depth-stencil"}));
  EXPECT_TRUE(frameResources.contains(TechniqueResourceKey{
      RenderTechniqueId::ForwardRaster, "lighting-framebuffer"}));

  EXPECT_TRUE(pipelines.contains(TechniquePipelineKey{
      RenderTechniqueId::ForwardRaster, "forward-opaque"}));
  EXPECT_TRUE(pipelines.contains(TechniquePipelineKey{
      RenderTechniqueId::ForwardRaster, "forward-transparent"}));
  EXPECT_TRUE(pipelines.contains(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "post-process"}));

  EXPECT_FALSE(frameResources.contains(TechniqueResourceKey{
      RenderTechniqueId::ForwardRaster, "gbuffer-framebuffer"}));
  EXPECT_FALSE(frameResources.contains(TechniqueResourceKey{
      RenderTechniqueId::ForwardRaster, "g-buffer-sampler"}));
  EXPECT_FALSE(pipelines.contains(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "gbuffer"}));
}

TEST(ForwardRasterTechniqueTests, PublishesForwardRecipesBackedByShaderSource) {
  FrameResourceRegistry frameResources;
  PipelineRegistry pipelines;
  ForwardRasterTechnique technique;
  RenderSystemContext context{
      .frameResources = &frameResources,
      .pipelines = &pipelines,
  };

  technique.registerTechniqueContracts(context);

  const std::array<std::string_view, 17> expectedRecipeNames = {
      "depth-prepass",
      "depth-prepass-front-cull",
      "depth-prepass-no-cull",
      "bim-depth-prepass",
      "bim-depth-prepass-front-cull",
      "bim-depth-prepass-no-cull",
      "forward-opaque",
      "forward-transparent",
      "forward-transparent-front-cull",
      "forward-transparent-no-cull",
      "post-process",
      "light-gizmo",
      "light-gizmo-coverage",
      "transform-gizmo",
      "transform-gizmo-solid",
      "transform-gizmo-overlay",
      "transform-gizmo-solid-overlay",
  };
  for (std::string_view recipeName : expectedRecipeNames) {
    EXPECT_NE(pipelines.find(TechniquePipelineKey{
                  RenderTechniqueId::ForwardRaster, std::string(recipeName)}),
              nullptr)
        << recipeName;
  }

  const auto *opaque = pipelines.find(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "forward-opaque"});
  ASSERT_NE(opaque, nullptr);
  EXPECT_EQ(opaque->layoutName, "transparent");

  const auto *transparent = pipelines.find(TechniquePipelineKey{
      RenderTechniqueId::ForwardRaster, "forward-transparent"});
  ASSERT_NE(transparent, nullptr);
  EXPECT_EQ(transparent->layoutName, "transparent");

  const auto *postProcess = pipelines.find(
      TechniquePipelineKey{RenderTechniqueId::ForwardRaster, "post-process"});
  ASSERT_NE(postProcess, nullptr);
  EXPECT_EQ(postProcess->kind, PipelineRecipeKind::Graphics);
  EXPECT_EQ(postProcess->shaderStages.size(), 2u);
  EXPECT_EQ(postProcess->layoutName, "post-process");

  const std::filesystem::path shaderPath =
      repoPath("shaders/forward_opaque.slang");
  ASSERT_TRUE(std::filesystem::exists(shaderPath));

  const std::string shader = readRepoTextFile("shaders/forward_opaque.slang");
  EXPECT_TRUE(contains(shader, "kObjectFlagTransparentOnly"));
  EXPECT_FALSE(contains(shader, "[earlydepthstencil]"));
  EXPECT_TRUE(contains(shader, "#include \"alpha_mask_common.slang\""));
  EXPECT_TRUE(contains(
      shader, "AlphaMaskPass(baseAlpha, vertIn.alphaCutoff, vertIn.pos)"));
  EXPECT_FALSE(contains(shader, "baseAlpha < vertIn.alphaCutoff"));
  const auto transparentOnlyGuard =
      shader.find("if ((vertIn.flags & kObjectFlagTransparentOnly) != 0u)");
  ASSERT_NE(transparentOnlyGuard, std::string::npos);
  const auto transparentOnlyDiscard =
      shader.find("discard;", transparentOnlyGuard);
  ASSERT_NE(transparentOnlyDiscard, std::string::npos);
  const auto materialWork =
      shader.find("GpuMaterial material", transparentOnlyGuard);
  ASSERT_NE(materialWork, std::string::npos);
  EXPECT_LT(transparentOnlyDiscard, materialWork);
  EXPECT_TRUE(contains(shader, "return float4(lighting, 1.0);"));
  EXPECT_FALSE(contains(shader, "oit_common.slang"));
  EXPECT_FALSE(contains(shader, "TransparentNode"));
}

TEST(ForwardRasterTechniqueTests,
     AvailabilityRequiresFrameRecorderAndSharedRasterServices) {
  ForwardRasterTechnique technique;

  const auto missingContextAvailability =
      technique.availability(RenderSystemContext{});
  EXPECT_FALSE(missingContextAvailability.available);
  EXPECT_TRUE(contains(missingContextAvailability.reason, "frame recorder"));
  EXPECT_TRUE(contains(missingContextAvailability.reason,
                       "shared raster services"));

  FrameRecorder recorder;
  RenderSystemContext missingSharedServicesContext{.frameRecorder = &recorder};
  const auto missingSharedServicesAvailability =
      technique.availability(missingSharedServicesContext);
  EXPECT_FALSE(missingSharedServicesAvailability.available);
  EXPECT_TRUE(contains(missingSharedServicesAvailability.reason,
                       "shared raster services"));

  DeferredRasterFrameGraphContext deferredContext(
      DeferredRasterFrameGraphServices{.graph = &recorder.graph()});
  RenderSystemContext completeContext{
      .frameRecorder = &recorder,
      .deferredRaster = &deferredContext,
  };
  EXPECT_TRUE(technique.availability(completeContext).available);
}

TEST(ForwardRasterTechniqueTests, PublishesForwardDebugPanel) {
  ForwardRasterTechnique technique;

  const auto model = technique.debugModel();

  EXPECT_EQ(model.techniqueName, "forward-raster");
  EXPECT_EQ(model.displayName, "Forward rendering");
  ASSERT_EQ(model.panels.size(), 1u);
  EXPECT_EQ(model.panels.front().id, "forward-frame");
  EXPECT_EQ(model.panels.front().title, "Forward Frame");

  const auto hasControl = [&model](std::string_view id) {
    return std::ranges::any_of(model.panels.front().controls,
                               [id](const auto &control) {
                                 return control.id == id;
                               });
  };
  EXPECT_TRUE(hasControl("render-graph"));
  EXPECT_TRUE(hasControl("depth-prepass"));
  EXPECT_TRUE(hasControl("forward-lighting"));
  EXPECT_TRUE(hasControl("transparent-oit"));
}

TEST(ForwardRasterTechniqueTests, BuildsForwardGraphWithoutGBufferOnlyPasses) {
  FrameResourceRegistry frameResources;
  PipelineRegistry pipelines;
  FrameRecorder recorder;
  DeferredRasterFrameGraphContext deferredContext(
      DeferredRasterFrameGraphServices{.graph = &recorder.graph()});
  ForwardRasterTechnique technique;
  RenderSystemContext context{
      .frameRecorder = &recorder,
      .deferredRaster = &deferredContext,
      .frameResources = &frameResources,
      .pipelines = &pipelines,
  };

  technique.buildFrameGraph(context);

  const RenderGraphDebugModel model = recorder.graph().debugModel();
  EXPECT_TRUE(hasPass(model, "DepthPrepass"));
  EXPECT_TRUE(hasPass(model, "Lighting"));
  EXPECT_TRUE(hasPass(model, "PostProcess"));
  EXPECT_TRUE(hasPass(model, "ShadowCascade0"));
  EXPECT_TRUE(hasPass(model, "ShadowCullCascade0"));
  EXPECT_TRUE(hasPass(model, "LocalShadowDepth"));
  EXPECT_TRUE(hasPass(model, "DepthToReadOnly"));
  EXPECT_TRUE(hasPass(model, "TransformGizmos"));
  EXPECT_TRUE(hasPass(model, "ExposureAdaptation"));
  EXPECT_TRUE(hasPass(model, "Bloom"));

  const RenderGraph &graph = recorder.graph();
  const auto *localShadow = graph.findPass(RenderPassId::LocalShadowDepth);
  ASSERT_NE(localShadow, nullptr);
  EXPECT_TRUE(resourceListContains(localShadow->reads,
                                   RenderResourceId::LocalShadowData));
  EXPECT_TRUE(resourceListContains(localShadow->writes,
                                   RenderResourceId::LocalShadowAtlas));

  const auto *lighting = graph.findPass(RenderPassId::Lighting);
  ASSERT_NE(lighting, nullptr);
  EXPECT_TRUE(
      resourceListContains(lighting->reads, RenderResourceId::SceneDepth));
  EXPECT_FALSE(
      resourceListContains(lighting->reads, RenderResourceId::GBufferNormal));
  EXPECT_TRUE(resourceListContains(lighting->optionalReads,
                                   RenderResourceId::LocalShadowAtlas));
  EXPECT_TRUE(
      resourceListContains(lighting->writes, RenderResourceId::OitStorage));

  const auto *exposure = graph.findPass(RenderPassId::ExposureAdaptation);
  ASSERT_NE(exposure, nullptr);
  EXPECT_TRUE(
      resourceListContains(exposure->reads, RenderResourceId::SceneColor));
  EXPECT_TRUE(
      resourceListContains(exposure->writes, RenderResourceId::ExposureState));

  const auto *bloom = graph.findPass(RenderPassId::Bloom);
  ASSERT_NE(bloom, nullptr);
  EXPECT_TRUE(resourceListContains(bloom->reads, RenderResourceId::SceneColor));
  EXPECT_TRUE(
      resourceListContains(bloom->writes, RenderResourceId::BloomTexture));

  const auto *postProcess = graph.findPass(RenderPassId::PostProcess);
  ASSERT_NE(postProcess, nullptr);
  EXPECT_TRUE(resourceListContains(postProcess->optionalReads,
                                   RenderResourceId::BloomTexture));
  EXPECT_TRUE(resourceListContains(postProcess->optionalReads,
                                   RenderResourceId::ExposureState));

  EXPECT_LT(executionPosition(graph, RenderPassId::LocalShadowDepth),
            executionPosition(graph, RenderPassId::DepthToReadOnly));
  EXPECT_LT(executionPosition(graph, RenderPassId::BimDepthPrepass),
            executionPosition(graph, RenderPassId::ShadowCullCascade0));
  EXPECT_LT(executionPosition(graph, RenderPassId::ShadowCullCascade0),
            executionPosition(graph, RenderPassId::ShadowCascade0));
  EXPECT_LT(executionPosition(graph, RenderPassId::DepthToReadOnly),
            executionPosition(graph, RenderPassId::Lighting));
  EXPECT_LT(executionPosition(graph, RenderPassId::Lighting),
            executionPosition(graph, RenderPassId::TransformGizmos));
  EXPECT_LT(executionPosition(graph, RenderPassId::TransformGizmos),
            executionPosition(graph, RenderPassId::ExposureAdaptation));
  EXPECT_LT(executionPosition(graph, RenderPassId::ExposureAdaptation),
            executionPosition(graph, RenderPassId::OitResolve));
  EXPECT_LT(executionPosition(graph, RenderPassId::OitResolve),
            executionPosition(graph, RenderPassId::Bloom));
  EXPECT_LT(executionPosition(graph, RenderPassId::Bloom),
            executionPosition(graph, RenderPassId::PostProcess));

  EXPECT_FALSE(hasPass(model, "GBuffer"));
  EXPECT_FALSE(hasPass(model, "BimGBuffer"));
  EXPECT_FALSE(hasPass(model, "TileCull"));
  EXPECT_FALSE(hasPass(model, "GTAO"));
}

TEST(ForwardRasterTechniqueTests,
     LightingPassUsesForwardRecorderReadinessForEmptyFrame) {
  FrameResourceRegistry frameResources;
  PipelineRegistry pipelines;
  FrameRecorder recorder;
  ForwardRasterTechnique technique;
  RenderSystemContext context{
      .frameRecorder = &recorder,
      .frameResources = &frameResources,
      .pipelines = &pipelines,
  };

  technique.buildFrameGraph(context);

  const auto *lightingPass = recorder.graph().findPass(RenderPassId::Lighting);
  ASSERT_NE(lightingPass, nullptr);
  ASSERT_TRUE(lightingPass->readiness);

  const auto readiness = lightingPass->readiness(FrameRecordParams{});
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ForwardRasterTechniqueTests,
     SourceWiresLightingToForwardRecorderAndExcludesDeferredDependencies) {
  const std::string source =
      readRepoTextFile("src/renderer/forward/ForwardRasterTechnique.cpp");
  const std::string deferredContext = readRepoTextFile(
      "src/renderer/deferred/DeferredRasterFrameGraphContext.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(source, "ForwardRasterLightingPassRecorder.h"));
  EXPECT_TRUE(contains(source, "recordForwardRasterDepthPrepass"));
  EXPECT_TRUE(contains(source, "recordForwardRasterBimDepthPrepass"));
  EXPECT_TRUE(contains(source, "addForwardShadowCullPass"));
  EXPECT_TRUE(contains(source, "buildShadowCullPassPlan"));
  EXPECT_TRUE(contains(source, "recordShadowCullPassCommands"));
  EXPECT_TRUE(contains(source, "recordForwardShadowPass"));
  EXPECT_TRUE(contains(source, "canRecordForwardShadowPass"));
  EXPECT_TRUE(contains(source, "recordForwardRasterLocalShadowPass"));
  EXPECT_TRUE(contains(source, "recordForwardRasterDepthReadOnlyTransition"));
  EXPECT_TRUE(
      contains(source, "recordForwardRasterLightingPassCommands(cmd, p)"));
  EXPECT_TRUE(contains(source, "checkForwardRasterLightingPassReadiness(p)"));
  EXPECT_TRUE(contains(source, "recordForwardRasterExposureAdaptation"));
  EXPECT_TRUE(contains(source, "recordForwardRasterBloom"));
  EXPECT_TRUE(contains(source, "recordForwardRasterPostProcessPass"));
  EXPECT_TRUE(contains(source, "recordDeferredPostProcessPassCommands"));
  EXPECT_TRUE(contains(source, ".displayMode = sharedContext.displayMode()"));
  EXPECT_FALSE(contains(
      source, ".displayMode = container::ui::GBufferViewMode::Lit"));
  EXPECT_TRUE(contains(source, "recordForwardRasterLightGizmoOverlay"));
  EXPECT_TRUE(contains(source, "recordForwardRasterTransformGizmoOverlay"));
  EXPECT_TRUE(contains(source, "buildDeferredRasterSceneColorReadBarrierPlan"));
  EXPECT_TRUE(
      contains(source, "recordDeferredRasterSceneColorReadBarrierCommands"));
  EXPECT_TRUE(contains(deferredContext, "forwardShadowCascadeFramePassContext"));
  EXPECT_TRUE(contains(deferredContext,
                       "p.runtime.activeTechnique == "
                       "RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(rendererFrontend, "const bool shadowAtlasVisible"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "activeTechnique == RenderTechniqueId::ForwardRaster"));
  EXPECT_TRUE(contains(rendererFrontend, "displayModeRecordsShadowAtlas"));
  EXPECT_TRUE(contains(source, "recordForwardRasterOitClear"));
  EXPECT_TRUE(contains(source, "recordForwardRasterOitResolvePreparation"));
  EXPECT_TRUE(contains(source, "recordDeferredTransparentOitClearCommands"));
  EXPECT_TRUE(contains(
      source, "recordDeferredTransparentOitResolvePreparationCommands"));
  EXPECT_TRUE(contains(source, "RenderResourceId::SwapchainImage"));
  EXPECT_TRUE(contains(source, "RenderResourceId::OitStorage"));
  EXPECT_TRUE(contains(source, "RenderResourceId::SceneColor"));
  const std::string_view lightingAccess = lightingResourceAccessBlock(source);
  ASSERT_FALSE(lightingAccess.empty());
  EXPECT_TRUE(contains(lightingAccess, "RenderResourceId::SceneDepth"));
  EXPECT_FALSE(contains(lightingAccess, "RenderResourceId::GBufferNormal"));
  EXPECT_TRUE(
      contains(lightingAccess,
               "{RenderResourceId::SceneColor, RenderResourceId::OitStorage}"));
  EXPECT_FALSE(
      contains(lightingAccess,
               "{RenderResourceId::SceneColor, RenderResourceId::SceneDepth}"));

  EXPECT_FALSE(contains(source, "DeferredRasterResourceBridge"));
  EXPECT_FALSE(contains(source, "DeferredRasterPipelineBridge"));
  EXPECT_FALSE(contains(source, "RenderPassId::GBuffer"));
  EXPECT_FALSE(contains(source, "RenderPassId::BimGBuffer"));
  EXPECT_FALSE(contains(source, "RenderPassId::TileCull"));
  EXPECT_FALSE(contains(source, "RenderPassId::GTAO"));
  EXPECT_FALSE(contains(source, "ForwardOpaqueFrontCull"));
  EXPECT_FALSE(contains(source, "ForwardOpaqueNoCull"));
  EXPECT_FALSE(contains(source, "g-buffer-sampler"));
}

TEST(ForwardRasterTechniqueTests,
     IfcPathsStayOnForwardBimPathWithPreparedSidecarFallback) {
  const std::string mainSource = readRepoTextFile("main.cpp");
  const std::string bimManager =
      readRepoTextFile("src/renderer/bim/BimManager.cpp");
  const std::string forwardSource =
      readRepoTextFile("src/renderer/forward/ForwardRasterTechnique.cpp");
  const std::string forwardLighting =
      readRepoTextFile("src/renderer/forward/"
                       "ForwardRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(contains(mainSource, "extension == \".ifc\""));
  EXPECT_TRUE(contains(mainSource, "config.bimModelPath = config.modelPath"));
  EXPECT_TRUE(contains(mainSource, "config.modelPath.clear()"));

  EXPECT_TRUE(contains(bimManager, "loadIfcWithPreparedSidecarFallback"));
  EXPECT_TRUE(contains(bimManager, "preparedIfcSidecarPath"));
  EXPECT_TRUE(contains(bimManager, "replace_extension(\".ifcx\")"));
  EXPECT_TRUE(contains(bimManager, "container::geometry::ifcx::LoadFromFile"));

  EXPECT_TRUE(contains(forwardSource, "recordForwardRasterBimDepthPrepass"));
  EXPECT_TRUE(contains(forwardLighting,
                       "BimSurfacePassKind::OpaqueLighting"));
  EXPECT_TRUE(contains(forwardLighting,
                       "BimSurfacePassKind::TransparentLighting"));
  EXPECT_FALSE(contains(forwardSource, "RenderPassId::BimGBuffer"));
}
