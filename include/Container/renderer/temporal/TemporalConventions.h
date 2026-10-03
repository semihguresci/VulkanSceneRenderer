#pragma once

#include <cmath>
#include <glm/glm.hpp>

// Pure math contracts mirrored in shaders/temporal_common.slang. These result
// types are CPU-local, not an upload ABI. See docs/temporal-rendering.md.
namespace container::temporal {

struct Jitter {
  glm::vec2 uv{0.0f};
  glm::vec2 ndc{0.0f};
  bool valid{false};
};

struct ProjectedPoint {
  glm::vec2 uv{0.0f};
  float reverseZDepth{0.0f};
  bool valid{false};
};

struct MotionVector {
  glm::vec2 uv{0.0f};
  float previousReverseZDepth{0.0f};
  bool valid{false};
};

struct ReprojectedAddress {
  glm::vec2 uv{0.0f};
  bool valid{false};
};

struct ExposureScale {
  float value{0.0f};
  bool valid{false};
};

[[nodiscard]] inline bool finite(glm::vec2 value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

// Scene viewport has negative height: NDC +Y maps toward UV -Y.
// These two low-level conversions require finite inputs.
[[nodiscard]] inline glm::vec2 sceneNdcToUv(glm::vec2 ndc) {
  return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f};
}

[[nodiscard]] inline glm::vec2 sceneUvToNdc(glm::vec2 uv) {
  return {uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
}

// Jitter means projected image displacement in render pixels: right/down +.
[[nodiscard]] inline Jitter makeJitter(glm::vec2 pixels, glm::uvec2 extent) {
  if (!finite(pixels) || extent.x == 0 || extent.y == 0) {
    return {};
  }
  const glm::vec2 uv = pixels / glm::vec2(extent);
  const glm::vec2 ndc{2.0f * uv.x, -2.0f * uv.y};
  if (!finite(uv) || !finite(ndc)) {
    return {};
  }
  return {uv, ndc, true};
}

// Off-screen XY is projectable. A caller must separately check image bounds.
// Depth zero is mathematically valid, but the resolve treats cleared sky as
// uncovered geometry. Every invalid result has a finite zero payload.
[[nodiscard]] inline ProjectedPoint projectSceneClip(glm::vec4 clip) {
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
      !std::isfinite(clip.z) || !std::isfinite(clip.w) || clip.w <= 0.0f) {
    return {};
  }
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  const glm::vec2 uv = sceneNdcToUv(glm::vec2(ndc));
  if (!finite(uv) || !std::isfinite(ndc.z) || ndc.z < 0.0f || ndc.z > 1.0f) {
    return {};
  }
  return {uv, ndc.z, true};
}

// Clips must be unjittered and represent the same surface point in each frame.
// previousSurfaceValid comes from stable identity / revision / frame validity.
[[nodiscard]] inline MotionVector
motionFromUnjitteredClips(glm::vec4 currentClip, glm::vec4 previousClip,
                          bool previousSurfaceValid) {
  if (!previousSurfaceValid) {
    return {};
  }
  const auto current = projectSceneClip(currentClip);
  const auto previous = projectSceneClip(previousClip);
  const glm::vec2 velocity = previous.uv - current.uv;
  if (!current.valid || !previous.valid || !finite(velocity)) {
    return {};
  }
  return {velocity, previous.reverseZDepth, true};
}

[[nodiscard]] inline bool insideImage(glm::vec2 uv) {
  return finite(uv) && uv.x >= 0.0f && uv.y >= 0.0f && uv.x < 1.0f &&
         uv.y < 1.0f;
}

// Supply previousGridJitterUv = 0 for resolved, unjittered color history;
// supply previous frame jitter for raw previous depth / raster attachments.
// Bounds failure rejects history; never clamp an off-screen address into it.
[[nodiscard]] inline ReprojectedAddress
reprojectToPreviousGrid(glm::vec2 currentRasterUv, const MotionVector &motion,
                        glm::vec2 currentJitterUv,
                        glm::vec2 previousGridJitterUv) {
  if (!motion.valid || !finite(currentRasterUv) || !finite(motion.uv) ||
      !finite(currentJitterUv) || !finite(previousGridJitterUv)) {
    return {};
  }
  const glm::vec2 uv =
      currentRasterUv + motion.uv - currentJitterUv + previousGridJitterUv;
  if (!insideImage(uv)) {
    return {};
  }
  return {uv, true};
}

// storedColor = sceneRadiance * preExposure. Convert old storage to new storage
// before neighborhood clipping or blending. Display exposure is unrelated.
[[nodiscard]] inline ExposureScale
historyExposureScale(float currentPreExposure, float previousPreExposure) {
  if (!std::isfinite(currentPreExposure) ||
      !std::isfinite(previousPreExposure) || currentPreExposure <= 0.0f ||
      previousPreExposure <= 0.0f) {
    return {};
  }
  const float value = currentPreExposure / previousPreExposure;
  if (!std::isfinite(value) || value <= 0.0f) {
    return {};
  }
  return {value, true};
}

} // namespace container::temporal
