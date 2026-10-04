#pragma once

#include "Container/renderer/temporal/TemporalConventions.h"
#include "Container/utility/SceneData.h"

#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace container::temporal {

struct Settings {
  bool enabled{false};
  float historyWeight{0.9f};
  float depthAbsoluteTolerance{0.02f};
  float depthRelativeTolerance{0.01f};
  float varianceGamma{1.25f};
  uint32_t jitterSeed{0};
};

inline void validateSettings(const Settings &settings, uint32_t samples) {
  if (settings.enabled && samples != 1)
    throw std::invalid_argument(
        "TAA requires --msaa 1; disable TAA to use MSAA");
  if (!std::isfinite(settings.historyWeight) || settings.historyWeight < 0.0f ||
      settings.historyWeight > 0.98f ||
      !std::isfinite(settings.varianceGamma) || settings.varianceGamma < 0.5f ||
      settings.varianceGamma > 4.0f ||
      !std::isfinite(settings.depthAbsoluteTolerance) ||
      settings.depthAbsoluteTolerance < 0.0f ||
      settings.depthAbsoluteTolerance > 1000.0f ||
      !std::isfinite(settings.depthRelativeTolerance) ||
      settings.depthRelativeTolerance < 0.0f ||
      settings.depthRelativeTolerance > 1.0f)
    throw std::invalid_argument(
        "Invalid TAA settings: history weight [0,0.98], variance gamma "
        "[0.5,4], absolute depth tolerance [0,1000], relative depth tolerance "
        "[0,1]");
}

inline float radicalInverse(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float factor = 1.0f;
  while (index != 0) {
    factor /= static_cast<float>(base);
    result += factor * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

inline glm::vec2 jitterPixels(uint64_t frame, uint32_t seed) {
  const uint32_t index = static_cast<uint32_t>((frame + seed) % 32u) + 1u;
  // Center this finite 32-sample cycle, including every seeded rotation.
  return {radicalInverse(index, 2) - 0.5f + 0.01513671875f,
          radicalInverse(index, 3) - 0.5f + 0.0185185185185f};
}

struct SurfaceKey {
  uint64_t provider{0};
  uint64_t identity{0};
  bool operator==(const SurfaceKey &) const = default;
};

struct SurfaceKeyHash {
  size_t operator()(const SurfaceKey &key) const {
    return std::hash<uint64_t>{}(key.identity) ^
           (std::hash<uint64_t>{}(key.provider) << 1);
  }
};

// CPU state advances exclusively on successful scene submission. Pending data
// may be prepared/replaced repeatedly without consuming a history frame.
class State {
public:
  void reset(std::string reason) {
    ++epoch_;
    valid_ = false;
    sequenceFrame_ = 0;
    previous_.clear();
    pending_.clear();
    nextToken_ = 1; // Reuse only after the whole epoch is invalidated.
    resetReason_ = std::move(reason);
  }

  void prepareCamera(container::gpu::CameraData &camera, glm::uvec2 extent,
                     const Settings &settings,
                     const glm::mat4 &projection = glm::mat4(1.0f)) {
    if (extent != extent_ || settings.enabled != enabled_ ||
        settings.jitterSeed != seed_) {
      reset(extent != extent_ ? "render extent changed"
                              : "AA mode/jitter changed");
      extent_ = extent;
      enabled_ = settings.enabled;
      seed_ = settings.jitterSeed;
    }
    if (valid_ && projection != previousProjection_)
      reset("camera projection changed");
    if (valid_ &&
        (glm::dot(glm::vec3(camera.cameraForward),
                  glm::vec3(previousCamera_.cameraForward)) < 0.5f ||
         glm::distance(glm::vec3(camera.cameraWorldPosition),
                       glm::vec3(previousCamera_.cameraWorldPosition)) > 5.0f))
      reset("camera cut");
    pendingProjection_ = projection;
    camera.renderExtent = {float(extent.x), float(extent.y),
                           1.0f / float(std::max(extent.x, 1u)),
                           1.0f / float(std::max(extent.y, 1u))};
    camera.unjitteredViewProj = camera.viewProj;
    camera.previousViewProj =
        valid_ ? previousCamera_.unjitteredViewProj : camera.viewProj;
    camera.previousJitteredViewProj =
        valid_ ? previousCamera_.viewProj : camera.viewProj;
    camera.previousInverseViewProj =
        valid_ ? previousCamera_.inverseViewProj : camera.inverseViewProj;
    const Jitter jitter = makeJitter(
        enabled_ ? jitterPixels(sequenceFrame_, seed_) : glm::vec2(0), extent);
    const glm::vec2 previousJitter =
        valid_ ? glm::vec2(previousCamera_.jitterUv) : jitter.uv;
    camera.jitterUv = glm::vec4(jitter.uv, previousJitter);
    camera.temporalInfo = {valid_ && enabled_ ? 1u : 0u,
                           static_cast<uint32_t>(frameId_), epoch_,
                           enabled_ ? 1u : 0u};
    glm::mat4 translation(1.0f);
    translation[3][0] = jitter.ndc.x;
    translation[3][1] = jitter.ndc.y;
    camera.viewProj = translation * camera.viewProj;
    camera.inverseViewProj = glm::inverse(camera.viewProj);
    pendingCamera_ = camera;
    ++pendingGeneration_;
  }

  void prepareObject(container::gpu::ObjectData &object, SurfaceKey key,
                     uint64_t revision) {
    const auto found = previous_.find(key);
    const bool compatible = valid_ && found != previous_.end() &&
                            found->second.revision == revision;
    object.previousModel = compatible ? found->second.model : object.model;
    uint32_t token = compatible ? found->second.token : nextToken_++;
    if (token == 0 || nextToken_ == 0)
      throw std::overflow_error(
          "Temporal surface tokens exhausted; reset renderer before reuse");
    object.temporalInfo.x = token;
    object.temporalInfo.y = compatible ? 1u : 0u;
    pending_[key] = {object.model, revision, token, pendingGeneration_};
  }

  void commit() {
    previousCamera_ = pendingCamera_;
    previousProjection_ = pendingProjection_;
    std::erase_if(pending_, [&](const auto &entry) {
      return entry.second.preparedGeneration != pendingGeneration_;
    });
    // Retain both maps' node/bucket storage during ordinary frames. A large
    // static provider must not allocate one snapshot node per object per frame.
    previous_.swap(pending_);
    valid_ = true;
    ++frameId_;
    ++sequenceFrame_;
  }

  bool valid() const { return valid_; }
  uint64_t frameId() const { return frameId_; }
  uint32_t epoch() const { return epoch_; }
  const std::string &resetReason() const { return resetReason_; }

private:
  struct Surface {
    glm::mat4 model{1};
    uint64_t revision{0};
    uint32_t token{0};
    uint64_t preparedGeneration{0};
  };
  std::unordered_map<SurfaceKey, Surface, SurfaceKeyHash> previous_, pending_;
  container::gpu::CameraData previousCamera_{}, pendingCamera_{};
  glm::mat4 previousProjection_{1.0f}, pendingProjection_{1.0f};
  glm::uvec2 extent_{0};
  bool valid_{false}, enabled_{false};
  uint32_t epoch_{0}, seed_{0}, nextToken_{1};
  uint64_t frameId_{0}, sequenceFrame_{0};
  uint64_t pendingGeneration_{0};
  std::string resetReason_{"first frame"};
};

} // namespace container::temporal
