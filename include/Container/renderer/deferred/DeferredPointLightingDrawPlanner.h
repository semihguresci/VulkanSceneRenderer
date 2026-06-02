#pragma once

#include "Container/renderer/deferred/DeferredRasterLighting.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <span>

namespace container::renderer {

enum class DeferredPointLightingStencilPipeline : uint32_t {
  PointLight = 0,
  PointLightStencilDebug = 1,
};

struct DeferredPointLightingDrawInputs {
  DeferredPointLightingState state{};
  bool debugVisualizePointLightStencil{false};
  uint32_t framebufferWidth{0};
  uint32_t framebufferHeight{0};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
  uint32_t contactVisibilityEnabled{0};
  uint32_t localShadowEnabled{0};
  float bounceIntensity{1.0f};
  std::span<const container::gpu::PointLightData> pointLights{};
  uint32_t lightVolumeIndexCount{0};
};

struct DeferredPointLightingStencilRoute {
  container::gpu::PointLightData light{};
};

struct DeferredPointLightingDrawPlan {
  DeferredPointLightingPath path{DeferredPointLightingPath::None};
  container::gpu::TiledLightingPushConstants tiledPushConstants{};
  DeferredPointLightingStencilPipeline stencilPipeline{
      DeferredPointLightingStencilPipeline::PointLight};
  uint32_t contactVisibilityEnabled{0};
  uint32_t localShadowEnabled{0};
  float bounceIntensity{1.0f};
  uint32_t lightVolumeIndexCount{0};
  std::array<DeferredPointLightingStencilRoute,
             container::gpu::kMaxDeferredPointLights>
      stencilRoutes{};
  uint32_t stencilRouteCount{0};
};

[[nodiscard]] DeferredPointLightingDrawPlan
buildDeferredPointLightingDrawPlan(
    const DeferredPointLightingDrawInputs &inputs);

} // namespace container::renderer
