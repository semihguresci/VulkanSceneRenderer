#pragma once

#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/lighting/LightPushConstants.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace container::renderer {

inline constexpr uint32_t kMaxDeferredLightGizmos = 256u;
inline constexpr uint32_t kLightGizmoCoverageVertexCount = 192u;
inline constexpr uint32_t kLightGizmoCoveragePoint = 1u;
inline constexpr uint32_t kLightGizmoCoverageSpot = 2u;
inline constexpr uint32_t kLightGizmoCoverageArea = 3u;
inline constexpr uint32_t kLightGizmoCoverageDirectional = 4u;
inline constexpr float kLightGizmoMinIconExtent = 0.07f;
inline constexpr float kLightGizmoMaxIconExtent = 1.1f;
inline constexpr float kLightGizmoCoverageSceneRadiusScale = 0.14f;
inline constexpr float kLightGizmoDirectionalCoverageSceneRadiusScale = 0.16f;
inline constexpr uint32_t kEditableLightPickTypeShift = 28u;
inline constexpr uint32_t kEditableLightPickSourceShift = 26u;
inline constexpr uint32_t kEditableLightPickIndexMask = 0x03ffffffu;

struct DeferredLightGizmoPlanInputs {
  glm::vec3 sceneCenter{0.0f, 0.0f, 0.0f};
  float sceneWorldRadius{1.0f};
  glm::vec3 cameraPosition{0.0f, 0.0f, 0.0f};
  glm::vec3 directionalDirection{0.0f, -1.0f, 0.0f};
  glm::vec3 directionalColor{1.0f, 1.0f, 1.0f};
  std::span<const EditableLightEntity> editableLights{};
  std::span<const container::gpu::PointLightData> pointLights{};
  bool areaEmitterDebug{false};
};

struct DeferredLightGizmoPlan {
  std::array<LightPushConstants, kMaxDeferredLightGizmos + 1u> pushConstants{};
  std::array<LightPushConstants, kMaxDeferredLightGizmos + 1u>
      coveragePushConstants{};
  uint32_t pushConstantCount{0u};
  uint32_t coveragePushConstantCount{0u};
};

[[nodiscard]] DeferredLightGizmoPlan
buildDeferredLightGizmoPlan(const DeferredLightGizmoPlanInputs &inputs);

[[nodiscard]] uint32_t encodeEditableLightPickId(EditableLightId id);
[[nodiscard]] std::optional<EditableLightId>
decodeEditableLightPickId(uint32_t pickId);

} // namespace container::renderer
