#pragma once

#include "Container/utility/SceneData.h"

#include <cstdint>

namespace container::renderer {

enum class DeferredLightingDisplayMode : uint32_t {
  Other = 0,
  Lit = 1,
  Transparency = 2,
  Revealage = 3,
  SurfaceNormals = 4,
  ObjectSpaceNormals = 5,
};

enum class DeferredLightingWireframeMode : uint32_t {
  Overlay = 0,
  Full = 1,
};

enum class DeferredPointLightingPath : uint32_t {
  None = 0,
  Tiled = 1,
  Stencil = 2,
  FullscreenFallback = 3,
};

struct DeferredLightingPipelineReadiness {
  bool directionalLight{false};
  bool wireframeDepth{false};
  bool wireframeNoDepth{false};
  bool objectNormalDebug{false};
  bool normalValidation{false};
  bool surfaceNormalLine{false};
  bool geometryDebug{false};
  bool tiledPointLight{false};
  bool pointLight{false};
  bool pointLightStencilDebug{false};
  bool stencilVolume{false};
};

struct DeferredLightingWireframeSettings {
  bool enabled{false};
  DeferredLightingWireframeMode mode{DeferredLightingWireframeMode::Overlay};
  bool depthTest{true};
  glm::vec3 color{0.0f, 1.0f, 0.0f};
  float lineWidth{1.0f};
  float overlayIntensity{0.85f};
};

struct DeferredLightingBimTechnicalElevationSettings {
  bool enabled{false};
  bool hiddenLineOverlay{true};
  bool depthTestLines{true};
  glm::vec3 lineColor{0.02f, 0.025f, 0.03f};
  float lineWidth{1.25f};
  float overlayIntensity{0.95f};
};

struct DeferredLightingFrameInputs {
  DeferredLightingDisplayMode displayMode{DeferredLightingDisplayMode::Other};
  bool guiAvailable{false};
  bool wireframeSupported{false};
  bool wireframeWideLinesSupported{false};
  DeferredLightingWireframeSettings wireframeSettings{};
  DeferredLightingBimTechnicalElevationSettings bimTechnicalElevation{};
  container::gpu::NormalValidationSettings normalValidationSettings{};
  bool geometryOverlayRequested{false};
  bool debugDirectionalOnly{false};
  bool debugVisualizePointLightStencil{false};
  bool tileCullPassActive{false};
  bool tiledLightingReady{false};
  bool clusteredCapacityLimited{false};
  bool depthSamplingReady{false};
  bool tiledDescriptorSetReady{false};
  bool transparentDrawCommandsAvailable{false};
  uint32_t pointLightCount{0};
  DeferredLightingPipelineReadiness pipelines{};
};

struct DeferredPointLightingState {
  DeferredPointLightingPath path{DeferredPointLightingPath::None};
  uint32_t stencilLightCount{0};
};

struct DeferredLightingFrameState {
  DeferredLightingDisplayMode displayMode{DeferredLightingDisplayMode::Other};
  DeferredLightingWireframeSettings wireframeSettings{};
  container::gpu::NormalValidationSettings normalValidationSettings{};
  bool wireframeEnabled{false};
  bool wireframeFullMode{false};
  bool wireframeOverlayMode{false};
  bool bimTechnicalElevationEnabled{false};
  bool objectSpaceNormalsEnabled{false};
  bool directionalLightingEnabled{false};
  bool normalValidationEnabled{false};
  bool surfaceNormalLinesEnabled{false};
  bool geometryOverlayEnabled{false};
  bool transparentOitEnabled{false};
  float wireframeIntensity{0.0f};
  float surfaceNormalLineWidth{1.0f};
  DeferredPointLightingState pointLighting{};
};

[[nodiscard]] DeferredLightingFrameState buildDeferredLightingFrameState(
    const DeferredLightingFrameInputs &inputs);

} // namespace container::renderer
