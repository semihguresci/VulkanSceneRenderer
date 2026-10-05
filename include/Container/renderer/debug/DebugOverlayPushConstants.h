#pragma once

#include "Container/common/CommonMath.h"

#include <cstddef>
#include <cstdint>

namespace container::renderer {

struct WireframePushConstants {
  alignas(4) uint32_t objectIndex{0};
  alignas(4) uint32_t sectionPlaneEnabled{0};
  alignas(4) uint32_t useObjectColor{0};
  alignas(4) uint32_t padding1{0};
  alignas(16) glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 colorIntensity{0.0f, 1.0f, 0.0f, 1.0f};
  alignas(4) float lineWidth{1.0f};
  alignas(4) float padding7{0.0f};
  alignas(4) float padding8{0.0f};
  alignas(4) float padding9{0.0f};
};
static_assert(offsetof(WireframePushConstants, objectIndex) == 0);
static_assert(offsetof(WireframePushConstants, sectionPlaneEnabled) == 4);
static_assert(offsetof(WireframePushConstants, useObjectColor) == 8);
static_assert(offsetof(WireframePushConstants, padding1) == 12);
static_assert(offsetof(WireframePushConstants, sectionPlane) == 16);
static_assert(offsetof(WireframePushConstants, colorIntensity) == 32);
static_assert(offsetof(WireframePushConstants, lineWidth) == 48);
static_assert(offsetof(WireframePushConstants, padding7) == 52);
static_assert(offsetof(WireframePushConstants, padding8) == 56);
static_assert(offsetof(WireframePushConstants, padding9) == 60);
static_assert(sizeof(WireframePushConstants) == 64);

struct NormalValidationPushConstants {
  alignas(4) uint32_t objectIndex{0};
  alignas(4) uint32_t showFaceFill{1};
  alignas(4) float faceAlpha{1.0f};
  alignas(4) uint32_t faceClassificationFlags{0};
  alignas(4) uint32_t sectionPlaneEnabled{0};
  alignas(4) uint32_t padding0{0};
  alignas(4) uint32_t padding1{0};
  alignas(4) uint32_t padding2{0};
  alignas(16) glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(offsetof(NormalValidationPushConstants, objectIndex) == 0);
static_assert(offsetof(NormalValidationPushConstants, showFaceFill) == 4);
static_assert(offsetof(NormalValidationPushConstants, faceAlpha) == 8);
static_assert(offsetof(NormalValidationPushConstants,
                       faceClassificationFlags) == 12);
static_assert(offsetof(NormalValidationPushConstants, sectionPlaneEnabled) ==
              16);
static_assert(offsetof(NormalValidationPushConstants, sectionPlane) == 32);
static_assert(sizeof(NormalValidationPushConstants) == 48);

inline constexpr uint32_t kNormalValidationInvertFaceClassification =
    1u << 0u;
inline constexpr uint32_t kNormalValidationBothSidesValid = 1u << 1u;

struct SurfaceNormalPushConstants {
  alignas(4) uint32_t objectIndex{0};
  alignas(4) float lineLength{0.16f};
  alignas(4) float lineOffset{0.002f};
  alignas(4) uint32_t sectionPlaneEnabled{0};
  alignas(16) glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(offsetof(SurfaceNormalPushConstants, objectIndex) == 0);
static_assert(offsetof(SurfaceNormalPushConstants, lineLength) == 4);
static_assert(offsetof(SurfaceNormalPushConstants, lineOffset) == 8);
static_assert(offsetof(SurfaceNormalPushConstants, sectionPlaneEnabled) == 12);
static_assert(offsetof(SurfaceNormalPushConstants, sectionPlane) == 16);
static_assert(sizeof(SurfaceNormalPushConstants) == 32);

struct TransformGizmoPushConstants {
  alignas(16) glm::vec4 originScale{0.0f, 0.0f, 0.0f, 1.0f};
  alignas(16) glm::vec4 axisX{1.0f, 0.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 axisY{0.0f, 1.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 axisZ{0.0f, 0.0f, 1.0f, 0.0f};
  alignas(4) uint32_t tool{0};
  alignas(4) uint32_t activeAxis{0};
  alignas(4) uint32_t transformSpace{0};
  alignas(4) uint32_t padding0{0};
};
static_assert(offsetof(TransformGizmoPushConstants, originScale) == 0);
static_assert(offsetof(TransformGizmoPushConstants, axisX) == 16);
static_assert(offsetof(TransformGizmoPushConstants, axisY) == 32);
static_assert(offsetof(TransformGizmoPushConstants, axisZ) == 48);
static_assert(offsetof(TransformGizmoPushConstants, tool) == 64);
static_assert(offsetof(TransformGizmoPushConstants, activeAxis) == 68);
static_assert(offsetof(TransformGizmoPushConstants, transformSpace) == 72);
static_assert(offsetof(TransformGizmoPushConstants, padding0) == 76);
static_assert(sizeof(TransformGizmoPushConstants) == 80);

} // namespace container::renderer
