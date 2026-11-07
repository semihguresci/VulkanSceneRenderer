#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/vec3.hpp>
#include <nlohmann/json_fwd.hpp>
#include "Container/asl/HandKinematics.h"

namespace container::asl {

inline constexpr size_t kLandmarkCount = 553;
inline constexpr size_t kMaxFrames = 2048;
inline constexpr uint64_t kMaxReplayBytes = 64 * 1024 * 1024;

struct Observation {
  glm::vec3 xyz{};
  bool valid{false};
};
struct PoseFrame {
  uint64_t timestampMs{};
  std::array<Observation, kLandmarkCount> points{};
};

// Model-relative Z is retained for interchange. Display intentionally uses
// image XY: independent hand/face/body Z origins do not define a common rig.
[[nodiscard]] glm::vec3 imagePlanePosition(const glm::vec3 &point);

class PoseReplay {
public:
  explicit PoseReplay(const nlohmann::json &document);
  [[nodiscard]] static PoseReplay load(const std::filesystem::path &path);
  [[nodiscard]] const PoseFrame &frameAt(double elapsedMs, bool loop = true) const;
  [[nodiscard]] double durationMs() const;
  [[nodiscard]] size_t frameIndexAt(double elapsedMs, bool loop = true) const;
  [[nodiscard]] const std::vector<PoseFrame> &frames() const { return frames_; }
  [[nodiscard]] const std::string &sampleId() const { return sampleId_; }
  [[nodiscard]] const std::string &label() const { return label_; }

private:
  std::vector<PoseFrame> frames_;
  std::string sampleId_, label_;
};

// Preview defaults are explicit experimental bounds, not measured physiology.
[[nodiscard]] HandFitOptions replayHandFitOptions();

// A derived, deterministic view: calibration and temporal fits visit every
// original frame once, in source timestamp order. Seeking/looping only samples
// the result; it cannot leak playback clock changes into the solver history.
class HandPoseReplay {
public:
  explicit HandPoseReplay(const PoseReplay &replay, double imageAspectRatio = 1.0,
                         HandFitOptions options = replayHandFitOptions());
  [[nodiscard]] const std::vector<PoseFrame> &frames() const { return frames_; }
  [[nodiscard]] const std::vector<std::array<HandFit, 2>> &fits() const { return fits_; }
  [[nodiscard]] const std::array<HandCalibration, 2> &calibrations() const { return calibrations_; }
  [[nodiscard]] nlohmann::json fitDocument() const;
private:
  double aspectRatio_{};
  HandFitOptions options_;
  std::array<HandCalibration, 2> calibrations_{};
  std::vector<std::array<HandFit, 2>> fits_;
  std::vector<PoseFrame> frames_;
};

// The fixed-step capture and interactive paths share this deterministic clock.
class ReplayClock {
public:
  void advance(double seconds);
  void restart() { elapsedMs_ = 0; }
  void seek(double milliseconds);
  void togglePause() { paused_ = !paused_; }
  void setSpeed(double speed);
  [[nodiscard]] double elapsedMs() const { return elapsedMs_; }
  [[nodiscard]] bool paused() const { return paused_; }
private:
  double elapsedMs_{0}, speed_{1};
  bool paused_{false};
};

} // namespace container::asl
