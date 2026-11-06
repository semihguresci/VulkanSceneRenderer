#pragma once

#include "Container/asl/PoseReplay.h"
#include <array>
#include <limits>
#include <optional>
#include <utility>

struct GLFWwindow;
namespace container::scene { class SceneGraph; class SceneManager; }
namespace container::asl {

// CPU observation playback, rendered through existing instanced scene meshes.
// No skinning, physical reconstruction, calibrated depth or training is implied.
class PoseReplayScene {
public:
  PoseReplayScene(PoseReplay replay, scene::SceneGraph &graph, scene::SceneManager &manager,
                  double startMs, double speed, bool loop, bool handKinematics = false,
                  double handAspectRatio = 1.0, double handAngularVelocity = 12.0,
                  const std::filesystem::path &handReport = {});
  void tick(double seconds, GLFWwindow *window);
private:
  void apply();
  PoseReplay replay_;
  std::optional<HandPoseReplay> handReplay_;
  ReplayClock clock_;
  scene::SceneGraph &graph_;
  uint64_t graphIdentity_{};
  std::array<uint32_t, kLandmarkCount> nodes_{};
  struct Bone { size_t from, to; uint32_t node; };
  std::vector<Bone> bones_;
  size_t displayedFrame_{std::numeric_limits<size_t>::max()};
  bool loop_{true}, pauseKey_{false}, restartKey_{false}, compareKey_{false}, showFitted_{true};
};
} // namespace container::asl
