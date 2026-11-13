#include "Container/renderer/deferred/DeferredRasterLighting.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::DeferredLightingFrameInputs;
using container::renderer::DeferredLightingDisplayMode;
using container::renderer::DeferredLightingWireframeMode;
using container::renderer::DeferredPointLightingPath;
using container::renderer::buildDeferredLightingFrameState;

[[nodiscard]] DeferredLightingFrameInputs litInputs() {
  DeferredLightingFrameInputs inputs{};
  inputs.displayMode = DeferredLightingDisplayMode::Lit;
  inputs.guiAvailable = true;
  inputs.wireframeSupported = true;
  inputs.wireframeWideLinesSupported = true;
  inputs.pipelines.directionalLight = true;
  inputs.pipelines.wireframeDepth = true;
  inputs.pipelines.wireframeNoDepth = true;
  return inputs;
}

TEST(DeferredRasterLightingTests, WireframeFullOverridesLitPathWhenSupported) {
  auto inputs = litInputs();
  inputs.wireframeSettings.enabled = true;
  inputs.wireframeSettings.mode = DeferredLightingWireframeMode::Full;
  inputs.pointLightCount = 4u;
  inputs.pipelines.pointLight = true;
  inputs.pipelines.stencilVolume = true;
  inputs.transparentDrawCommandsAvailable = true;

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_TRUE(state.wireframeEnabled);
  EXPECT_TRUE(state.wireframeFullMode);
  EXPECT_FALSE(state.directionalLightingEnabled);
  EXPECT_EQ(state.pointLighting.path, DeferredPointLightingPath::None);
  EXPECT_FALSE(state.transparentOitEnabled);
  EXPECT_FLOAT_EQ(state.wireframeIntensity, 1.0f);
}

TEST(DeferredRasterLightingTests,
     ObjectSpaceNormalsSuppressesDeferredLightAccumulation) {
  auto inputs = litInputs();
  inputs.displayMode = DeferredLightingDisplayMode::ObjectSpaceNormals;
  inputs.pipelines.objectNormalDebug = true;
  inputs.pointLightCount = 3u;
  inputs.pipelines.pointLight = true;
  inputs.pipelines.stencilVolume = true;

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_TRUE(state.objectSpaceNormalsEnabled);
  EXPECT_FALSE(state.directionalLightingEnabled);
  EXPECT_EQ(state.pointLighting.path, DeferredPointLightingPath::None);
}

TEST(DeferredRasterLightingTests, DirectionalOnlySuppressesPointLighting) {
  auto inputs = litInputs();
  inputs.debugDirectionalOnly = true;
  inputs.pointLightCount = 3u;
  inputs.pipelines.pointLight = true;
  inputs.pipelines.stencilVolume = true;

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_TRUE(state.directionalLightingEnabled);
  EXPECT_EQ(state.pointLighting.path, DeferredPointLightingPath::None);
}

TEST(DeferredRasterLightingTests,
     TiledPointLightingRequiresTileCullReadinessDepthDescriptorPipelineAndLights) {
  auto inputs = litInputs();
  inputs.pointLightCount = 2u;
  inputs.tileCullPassActive = true;
  inputs.tiledLightingReady = true;
  inputs.depthSamplingReady = true;
  inputs.tiledDescriptorSetReady = true;
  inputs.pipelines.tiledPointLight = true;

  EXPECT_EQ(buildDeferredLightingFrameState(inputs).pointLighting.path,
            DeferredPointLightingPath::Tiled);

  inputs.tiledDescriptorSetReady = false;
  EXPECT_NE(buildDeferredLightingFrameState(inputs).pointLighting.path,
            DeferredPointLightingPath::Tiled);
  inputs.tiledDescriptorSetReady = true;
  inputs.depthSamplingReady = false;
  EXPECT_NE(buildDeferredLightingFrameState(inputs).pointLighting.path,
            DeferredPointLightingPath::Tiled);
  inputs.depthSamplingReady = true;
  inputs.pointLightCount = 0u;
  EXPECT_NE(buildDeferredLightingFrameState(inputs).pointLighting.path,
            DeferredPointLightingPath::Tiled);
}

TEST(DeferredRasterLightingTests,
     StencilPointLightingClampsToMaxDeferredPointLights) {
  auto inputs = litInputs();
  inputs.pointLightCount = container::gpu::kMaxDeferredPointLights + 12u;
  inputs.pipelines.pointLight = true;
  inputs.pipelines.stencilVolume = true;

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_EQ(state.pointLighting.path, DeferredPointLightingPath::Stencil);
  EXPECT_EQ(state.pointLighting.stencilLightCount,
            container::gpu::kMaxDeferredPointLights);
}

TEST(DeferredRasterLightingTests,
     ClusterCapacityFallbackUsesRegularPointShaderWithoutStencilVolume) {
  auto inputs = litInputs();
  inputs.clusteredCapacityLimited = true;
  inputs.depthSamplingReady = true;
  inputs.pointLightCount = container::gpu::kMaxDeferredPointLights + 2u;
  inputs.pipelines.pointLight = true;
  inputs.debugVisualizePointLightStencil = true;

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_EQ(state.pointLighting.path, DeferredPointLightingPath::FullscreenFallback);
  EXPECT_EQ(state.pointLighting.stencilLightCount,
            container::gpu::kMaxDeferredPointLights);
  EXPECT_TRUE(state.directionalLightingEnabled);
  inputs.pointLightCount = 0;
  EXPECT_EQ(buildDeferredLightingFrameState(inputs).pointLighting.path,
            DeferredPointLightingPath::None);
}

TEST(DeferredRasterLightingTests, TransparentOitDisabledByWireframeFullMode) {
  auto inputs = litInputs();
  inputs.transparentDrawCommandsAvailable = true;

  EXPECT_TRUE(buildDeferredLightingFrameState(inputs).transparentOitEnabled);

  inputs.wireframeSettings.enabled = true;
  inputs.wireframeSettings.mode = DeferredLightingWireframeMode::Full;

  EXPECT_FALSE(buildDeferredLightingFrameState(inputs).transparentOitEnabled);
}

TEST(DeferredRasterLightingTests,
     SurfaceNormalLinesFollowNormalValidationOrSurfaceNormalView) {
  auto inputs = litInputs();
  inputs.pipelines.surfaceNormalLine = true;

  EXPECT_FALSE(
      buildDeferredLightingFrameState(inputs).surfaceNormalLinesEnabled);

  inputs.normalValidationSettings.enabled = true;
  inputs.pipelines.normalValidation = true;
  EXPECT_TRUE(
      buildDeferredLightingFrameState(inputs).surfaceNormalLinesEnabled);

  inputs.normalValidationSettings.enabled = false;
  inputs.displayMode = DeferredLightingDisplayMode::SurfaceNormals;
  EXPECT_TRUE(
      buildDeferredLightingFrameState(inputs).surfaceNormalLinesEnabled);
}

TEST(DeferredRasterLightingTests, OverlayStyleValuesAreClamped) {
  auto inputs = litInputs();
  inputs.wireframeSettings.enabled = true;
  inputs.wireframeSettings.mode = DeferredLightingWireframeMode::Overlay;
  inputs.wireframeSettings.overlayIntensity = 2.0f;
  inputs.wireframeSettings.lineWidth = 0.0f;
  inputs.normalValidationSettings.lineWidth = 0.0f;

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_TRUE(state.wireframeOverlayMode);
  EXPECT_FLOAT_EQ(state.wireframeSettings.overlayIntensity, 1.0f);
  EXPECT_FLOAT_EQ(state.wireframeSettings.lineWidth, 1.0f);
  EXPECT_FLOAT_EQ(state.normalValidationSettings.lineWidth, 1.0f);
  EXPECT_FLOAT_EQ(state.surfaceNormalLineWidth, 1.0f);
}

TEST(DeferredRasterLightingTests,
     BimTechnicalElevationForcesDepthTestedHiddenLineOverlay) {
  auto inputs = litInputs();
  inputs.guiAvailable = false;
  inputs.wireframeSettings.enabled = false;
  inputs.bimTechnicalElevation.enabled = true;
  inputs.bimTechnicalElevation.hiddenLineOverlay = true;
  inputs.bimTechnicalElevation.lineWidth = 2.25f;
  inputs.bimTechnicalElevation.overlayIntensity = 0.9f;
  inputs.bimTechnicalElevation.lineColor = {0.02f, 0.03f, 0.04f};

  const auto state = buildDeferredLightingFrameState(inputs);

  EXPECT_TRUE(state.bimTechnicalElevationEnabled);
  EXPECT_TRUE(state.wireframeEnabled);
  EXPECT_FALSE(state.wireframeFullMode);
  EXPECT_TRUE(state.wireframeOverlayMode);
  EXPECT_TRUE(state.wireframeSettings.depthTest);
  EXPECT_EQ(state.wireframeSettings.mode, DeferredLightingWireframeMode::Overlay);
  EXPECT_FLOAT_EQ(state.wireframeSettings.lineWidth, 2.25f);
  EXPECT_FLOAT_EQ(state.wireframeSettings.overlayIntensity, 0.9f);
  EXPECT_FLOAT_EQ(state.wireframeSettings.color.x, 0.02f);
  EXPECT_FLOAT_EQ(state.wireframeSettings.color.y, 0.03f);
  EXPECT_FLOAT_EQ(state.wireframeSettings.color.z, 0.04f);
  EXPECT_FLOAT_EQ(state.wireframeIntensity, 0.9f);

  inputs.bimTechnicalElevation.depthTestLines = false;
  EXPECT_FALSE(buildDeferredLightingFrameState(inputs)
                   .bimTechnicalElevationEnabled);
}

TEST(DeferredRasterLightingTests, BimTechnicalLinesUseTheRequestedDepthVariant) {
  auto inputs = litInputs();
  inputs.guiAvailable = false;
  inputs.bimTechnicalElevation.enabled = true;
  inputs.bimTechnicalElevation.hiddenLineOverlay = false;

  for (bool depthTest : {false, true}) {
    inputs.bimTechnicalElevation.depthTestLines = depthTest;
    inputs.pipelines.wireframeDepth = depthTest;
    inputs.pipelines.wireframeNoDepth = !depthTest;
    const auto state = buildDeferredLightingFrameState(inputs);
    EXPECT_TRUE(state.bimTechnicalElevationEnabled);
    EXPECT_TRUE(state.wireframeOverlayMode);
    EXPECT_FALSE(state.wireframeFullMode);
    EXPECT_EQ(state.wireframeSettings.depthTest, depthTest);
  }

  inputs.pipelines.wireframeDepth = false;
  EXPECT_FALSE(buildDeferredLightingFrameState(inputs).bimTechnicalElevationEnabled);
}

} // namespace
