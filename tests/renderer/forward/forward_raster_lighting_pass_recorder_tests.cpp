#include "Container/renderer/forward/ForwardRasterLightingPassRecorder.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/DrawCommand.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using container::renderer::DrawCommand;
using container::renderer::FrameDescriptorBinding;
using container::renderer::FrameFramebufferBinding;
using container::renderer::FrameRecordParams;
using container::renderer::FrameResourceRegistry;
using container::renderer::PipelineRegistry;
using container::renderer::RegisteredPipelineHandle;
using container::renderer::RegisteredPipelineLayout;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;
using container::renderer::RenderTechniqueId;
using container::renderer::TechniquePipelineKey;
using container::renderer::checkForwardRasterLightingPassReadiness;
using container::renderer::recordForwardRasterLightingPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::filesystem::path repoPath(const std::filesystem::path& relativePath) {
  return std::filesystem::path(CONTAINER_SOURCE_DIR) / relativePath;
}

std::string readRepoTextFile(const std::filesystem::path& relativePath) {
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

void bindDescriptor(FrameResourceRegistry& resources, std::string name,
                    uintptr_t value) {
  resources.bindDescriptorSet(
      RenderTechniqueId::ForwardRaster, std::move(name), 0u,
      FrameDescriptorBinding{
          .descriptorSet = fakeHandle<VkDescriptorSet>(value)});
}

void bindForwardLightingFramebuffer(FrameResourceRegistry& resources) {
  resources.bindFramebuffer(
      RenderTechniqueId::ForwardRaster, "lighting-framebuffer", 0u,
      FrameFramebufferBinding{
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x1001),
          .renderPass = fakeHandle<RenderingPassHandle>(0x1002),
          .extent = {1280u, 720u},
          .attachmentCount = 2u});
  resources.bindFramebuffer(
      RenderTechniqueId::ForwardRaster, "transparent-lighting-framebuffer", 0u,
      FrameFramebufferBinding{
          .framebuffer = fakeHandle<RenderingTargetHandle>(0x1003),
          .renderPass = fakeHandle<RenderingPassHandle>(0x1004),
          .extent = {1280u, 720u},
          .attachmentCount = 2u});
}

void registerForwardPipeline(PipelineRegistry& pipelines, std::string name,
                             uintptr_t value) {
  pipelines.registerHandle(
      RegisteredPipelineHandle{.key = {RenderTechniqueId::ForwardRaster,
                                       std::move(name)},
                               .pipeline = fakeHandle<VkPipeline>(value)});
}

void bindForwardPipelines(PipelineRegistry& pipelines) {
  registerForwardPipeline(pipelines, "forward-opaque", 0x2001);
  registerForwardPipeline(pipelines, "forward-transparent", 0x2002);
  registerForwardPipeline(pipelines, "forward-transparent-front-cull", 0x2003);
  registerForwardPipeline(pipelines, "forward-transparent-no-cull", 0x2004);
  pipelines.registerLayout(
      RegisteredPipelineLayout{.key = {RenderTechniqueId::ForwardRaster,
                                       "transparent"},
                               .layout = fakeHandle<VkPipelineLayout>(0x3001)});
}

void bindForwardTransparentOnlyPipelines(PipelineRegistry& pipelines) {
  registerForwardPipeline(pipelines, "forward-transparent", 0x2002);
  registerForwardPipeline(pipelines, "forward-transparent-front-cull", 0x2003);
  registerForwardPipeline(pipelines, "forward-transparent-no-cull", 0x2004);
  pipelines.registerLayout(
      RegisteredPipelineLayout{.key = {RenderTechniqueId::ForwardRaster,
                                       "transparent"},
                               .layout = fakeHandle<VkPipelineLayout>(0x3001)});
}

void bindForwardOpaqueOnlyPipelines(PipelineRegistry& pipelines) {
  registerForwardPipeline(pipelines, "forward-opaque", 0x2001);
  pipelines.registerLayout(
      RegisteredPipelineLayout{.key = {RenderTechniqueId::ForwardRaster,
                                       "transparent"},
                               .layout = fakeHandle<VkPipelineLayout>(0x3001)});
}

void bindForwardDescriptorSets(FrameResourceRegistry& resources) {
  bindDescriptor(resources, "scene-descriptor-set", 0x4001);
  bindDescriptor(resources, "bim-scene-descriptor-set", 0x4002);
  bindDescriptor(resources, "light-descriptor-set", 0x4003);
  bindDescriptor(resources, "shadow-descriptor-set", 0x4004);
  bindDescriptor(resources, "local-shadow-descriptor-set", 0x4005);
  bindDescriptor(resources, "frame-lighting-descriptor-set", 0x4006);
  bindDescriptor(resources, "oit-descriptor-set", 0x4007);
}

void bindForwardDescriptorSetsWithoutOit(FrameResourceRegistry& resources) {
  bindDescriptor(resources, "scene-descriptor-set", 0x4001);
  bindDescriptor(resources, "bim-scene-descriptor-set", 0x4002);
  bindDescriptor(resources, "light-descriptor-set", 0x4003);
  bindDescriptor(resources, "shadow-descriptor-set", 0x4004);
  bindDescriptor(resources, "local-shadow-descriptor-set", 0x4005);
  bindDescriptor(resources, "frame-lighting-descriptor-set", 0x4006);
}

void makeSceneGeometryReady(FrameRecordParams& params) {
  params.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x5001);
  params.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x5002);
}

void makeBimGeometryReady(FrameRecordParams& params) {
  params.bim.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x5101);
  params.bim.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x5102);
}

void makeForwardBindingsReady(FrameRecordParams& params,
                              FrameResourceRegistry& resources,
                              PipelineRegistry& pipelines) {
  bindForwardLightingFramebuffer(resources);
  bindForwardDescriptorSets(resources);
  bindForwardPipelines(pipelines);
  params.registries.resourceBindings = &resources;
  params.registries.pipelineHandles = &pipelines;
  params.registries.pipelineLayouts = &pipelines;
}

}  // namespace

TEST(ForwardRasterLightingPassRecorderTests, NullCommandBufferReturnsFalse) {
  const FrameRecordParams params{};

  EXPECT_FALSE(recordForwardRasterLightingPassCommands(VK_NULL_HANDLE, params));
}

TEST(ForwardRasterLightingPassRecorderTests,
     ReadinessMissingLightingFramebufferReturnsMissingSceneColor) {
  std::vector<DrawCommand> opaqueDraws(1u);
  FrameRecordParams params{};
  params.draws.opaqueDrawCommands = &opaqueDraws;

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(readiness.blockingResource, RenderResourceId::SceneColor);
}

TEST(ForwardRasterLightingPassRecorderTests,
     ReadinessMissingTransparencyTargetReturnsMissingSceneColor) {
  std::vector<DrawCommand> opaqueDraws(1u);
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  container::gpu::BindlessPushConstants bindless{};
  FrameRecordParams params{};
  params.draws.opaqueDrawCommands = &opaqueDraws;
  params.pushConstants.bindless = &bindless;
  makeSceneGeometryReady(params);
  makeForwardBindingsReady(params, resources, pipelines);
  resources.bindFramebuffer(
      RenderTechniqueId::ForwardRaster, "transparent-lighting-framebuffer", 0u,
      FrameFramebufferBinding{});

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(readiness.blockingResource, RenderResourceId::SceneColor);
}

TEST(ForwardRasterLightingPassRecorderTests,
     ReadinessMissingSceneGeometryReturnsMissingSceneGeometry) {
  std::vector<DrawCommand> opaqueDraws(1u);
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  container::gpu::BindlessPushConstants bindless{};
  FrameRecordParams params{};
  params.draws.opaqueDrawCommands = &opaqueDraws;
  params.pushConstants.bindless = &bindless;
  makeForwardBindingsReady(params, resources, pipelines);

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(readiness.blockingResource, RenderResourceId::SceneGeometry);
}

TEST(ForwardRasterLightingPassRecorderTests, ReadinessNoDrawsReturnsNotNeeded) {
  FrameRecordParams params{};

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(ForwardRasterLightingPassRecorderTests,
     NativeOnlyCurvesInitializeLightingAndRequireTheirPipeline) {
  std::vector<DrawCommand> curves(1u);
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  container::gpu::BindlessPushConstants bindless{};
  container::renderer::WireframePushConstants wireframe{};
  FrameRecordParams params{};
  params.pushConstants.bindless = &bindless;
  params.pushConstants.wireframe = &wireframe;
  params.bim.primitivePasses.curves.enabled = true;
  params.bim.nativeCurveDraws.opaqueDrawCommands = &curves;
  makeBimGeometryReady(params);
  makeForwardBindingsReady(params, resources, pipelines);
  EXPECT_TRUE(
      container::renderer::hasForwardRasterNativePrimitiveDraws(params));
  EXPECT_FALSE(checkForwardRasterLightingPassReadiness(params).ready);
  registerForwardPipeline(pipelines, "bim-curve-depth", 0x2101);
  pipelines.registerLayout(RegisteredPipelineLayout{
      .key = {RenderTechniqueId::ForwardRaster, "wireframe"},
      .layout = fakeHandle<VkPipelineLayout>(0x3101)});
  EXPECT_TRUE(checkForwardRasterLightingPassReadiness(params).ready);
  params.bim.primitivePasses.curves.depthTest = false;
  EXPECT_FALSE(checkForwardRasterLightingPassReadiness(params).ready);
  registerForwardPipeline(pipelines, "bim-curve-no-depth", 0x2102);
  EXPECT_TRUE(checkForwardRasterLightingPassReadiness(params).ready);
  params.pushConstants.wireframe = nullptr;
  EXPECT_EQ(checkForwardRasterLightingPassReadiness(params).blockingResource,
            RenderResourceId::BimGeometry);
  params.bim.primitivePasses.curves.enabled = false;
  EXPECT_FALSE(
      container::renderer::hasForwardRasterNativePrimitiveDraws(params));
  EXPECT_EQ(checkForwardRasterLightingPassReadiness(params).skipReason,
            RenderPassSkipReason::NotNeeded);
}

TEST(ForwardRasterLightingPassRecorderTests,
     ReadinessWithOpaqueDrawsAndForwardBindingsReadyReturnsReady) {
  std::vector<DrawCommand> opaqueDraws(1u);
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  container::gpu::BindlessPushConstants bindless{};
  FrameRecordParams params{};
  params.draws.opaqueDrawCommands = &opaqueDraws;
  params.pushConstants.bindless = &bindless;
  makeSceneGeometryReady(params);
  makeForwardBindingsReady(params, resources, pipelines);

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_TRUE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::None);
}

TEST(ForwardRasterLightingPassRecorderTests,
     ReadinessWithTransparentOnlyDrawsDoesNotRequireOpaquePipeline) {
  std::vector<DrawCommand> transparentDraws(1u);
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  container::gpu::BindlessPushConstants bindless{};
  FrameRecordParams params{};
  params.draws.transparentDrawCommands = &transparentDraws;
  params.pushConstants.bindless = &bindless;
  makeSceneGeometryReady(params);
  bindForwardLightingFramebuffer(resources);
  bindForwardDescriptorSets(resources);
  bindForwardTransparentOnlyPipelines(pipelines);
  params.registries.resourceBindings = &resources;
  params.registries.pipelineHandles = &pipelines;
  params.registries.pipelineLayouts = &pipelines;

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_TRUE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::None);
}

TEST(ForwardRasterLightingPassRecorderTests,
     ReadinessWithOpaqueBimOnlyDrawsDoesNotRequireOitDescriptorSet) {
  std::vector<DrawCommand> opaqueBimDraws(1u);
  FrameResourceRegistry resources;
  PipelineRegistry pipelines;
  container::gpu::BindlessPushConstants bindless{};
  FrameRecordParams params{};
  params.bim.draws.opaqueDrawCommands = &opaqueBimDraws;
  params.pushConstants.bindless = &bindless;
  makeBimGeometryReady(params);
  bindForwardLightingFramebuffer(resources);
  bindForwardDescriptorSetsWithoutOit(resources);
  bindForwardOpaqueOnlyPipelines(pipelines);
  params.registries.resourceBindings = &resources;
  params.registries.pipelineHandles = &pipelines;
  params.registries.pipelineLayouts = &pipelines;

  const auto readiness = checkForwardRasterLightingPassReadiness(params);

  EXPECT_TRUE(readiness.ready);
  EXPECT_EQ(readiness.skipReason, RenderPassSkipReason::None);
}

TEST(ForwardRasterLightingPassRecorderTests, SourceUsesForwardBridgesAndHelpers) {
  const std::string source = readRepoTextFile(
      "src/renderer/forward/ForwardRasterLightingPassRecorder.cpp");

  EXPECT_TRUE(contains(source, "ForwardRasterResourceBridge.h"));
  EXPECT_TRUE(contains(source, "ForwardRasterPipelineBridge.h"));
  EXPECT_TRUE(contains(source, "recordRenderPassBeginCommands"));
  EXPECT_TRUE(contains(source, "recordRenderPassEndCommands"));
  EXPECT_TRUE(contains(source, "recordSceneOpaqueDrawCommands"));
  EXPECT_TRUE(contains(source, "forwardOpaqueDescriptorSets"));
  EXPECT_TRUE(contains(source, "BimSurfacePassKind::OpaqueLighting"));
  EXPECT_FALSE(contains(source, "BimSurfacePassKind::DepthPrepass,"));
  EXPECT_TRUE(contains(source,
                      "basePushConstants.semanticColorMode = "
                      "p.bim.semanticColorMode"));
}

TEST(ForwardRasterLightingPassRecorderTests,
     SourceExcludesDeferredAndGBufferOnlyDependencies) {
  const std::string source = readRepoTextFile(
      "src/renderer/forward/ForwardRasterLightingPassRecorder.cpp");

  EXPECT_FALSE(contains(source, "DeferredRasterResourceBridge"));
  EXPECT_FALSE(contains(source, "DeferredRasterPipelineBridge"));
  EXPECT_FALSE(contains(source, "GBuffer"));
  EXPECT_FALSE(contains(source, "TileCull"));
  EXPECT_FALSE(contains(source, "GTAO"));
  EXPECT_FALSE(contains(source, "p.postProcess.renderPass"));
  EXPECT_FALSE(contains(source, "ForwardOpaqueFrontCull"));
  EXPECT_FALSE(contains(source, "ForwardOpaqueNoCull"));
  EXPECT_FALSE(contains(source, "g-buffer-sampler"));
}
