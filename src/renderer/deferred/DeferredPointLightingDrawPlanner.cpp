#include "Container/renderer/deferred/DeferredPointLightingDrawPlanner.h"

#include <algorithm>
#include <cmath>

namespace container::renderer {

namespace {

[[nodiscard]] uint32_t ceilDiv(uint32_t value, uint32_t divisor) {
  return (value + divisor - 1u) / divisor;
}

[[nodiscard]] uint32_t tileCount(uint32_t pixels) {
  return std::max(1u, ceilDiv(pixels, container::gpu::kTileSize));
}

} // namespace

DeferredPointLightingDrawPlan buildDeferredPointLightingDrawPlan(
    const DeferredPointLightingDrawInputs &inputs) {
  DeferredPointLightingDrawPlan plan{};
  plan.path = inputs.state.path;
  plan.contactVisibilityEnabled =
      inputs.contactVisibilityEnabled != 0u ? 1u : 0u;
  plan.localShadowEnabled = inputs.localShadowEnabled != 0u ? 1u : 0u;
  plan.bounceIntensity =
      std::clamp(std::isfinite(inputs.bounceIntensity) ? inputs.bounceIntensity
                                                       : 0.0f,
                 0.0f, 2.0f);
  plan.stencilPipeline =
      inputs.state.path == DeferredPointLightingPath::Stencil &&
              inputs.debugVisualizePointLightStencil
          ? DeferredPointLightingStencilPipeline::PointLightStencilDebug
          : DeferredPointLightingStencilPipeline::PointLight;

  if (inputs.state.path == DeferredPointLightingPath::Tiled) {
    plan.tiledPushConstants.tileCountX = tileCount(inputs.framebufferWidth);
    plan.tiledPushConstants.tileCountY = tileCount(inputs.framebufferHeight);
    plan.tiledPushConstants.depthSliceCount =
        container::gpu::kClusterDepthSlices;
    plan.tiledPushConstants.cameraNear = inputs.cameraNear;
    plan.tiledPushConstants.cameraFar = inputs.cameraFar;
    plan.tiledPushConstants.contactVisibilityEnabled =
        plan.contactVisibilityEnabled;
    plan.tiledPushConstants.localShadowEnabled = plan.localShadowEnabled;
    plan.tiledPushConstants.bounceIntensity = plan.bounceIntensity;
    return plan;
  }

  if (inputs.state.path != DeferredPointLightingPath::Stencil &&
      inputs.state.path != DeferredPointLightingPath::FullscreenFallback) {
    return plan;
  }

  if (inputs.state.path == DeferredPointLightingPath::Stencil)
    plan.lightVolumeIndexCount = inputs.lightVolumeIndexCount;
  const uint32_t requestedLightCount =
      std::min(inputs.state.stencilLightCount,
               container::gpu::kMaxDeferredPointLights);
  const uint32_t availableLightCount = static_cast<uint32_t>(
      std::min<size_t>(inputs.pointLights.size(),
                       container::gpu::kMaxDeferredPointLights));
  plan.stencilRouteCount = std::min(requestedLightCount, availableLightCount);

  for (uint32_t lightIndex = 0u; lightIndex < plan.stencilRouteCount;
       ++lightIndex) {
    plan.stencilRoutes[lightIndex] = {.light = inputs.pointLights[lightIndex]};
  }
  return plan;
}

} // namespace container::renderer
