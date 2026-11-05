#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace container::ui {
struct GltfAnimationClipChoice { std::string name; double duration{}; };
struct GltfAnimationCameraChoice { std::string name; size_t sourceNode{}; };
struct GltfAnimationControls {
  std::vector<GltfAnimationClipChoice> clips;
  std::vector<GltfAnimationCameraChoice> cameras;
  int selectedClip{-1};
  int cameraNode{-1};
  double time{};
  double speed{1};
  bool playing{};
  bool loop{true};
  std::string error;
  std::vector<std::string> diagnostics;
  std::optional<int> requestedClip;
  std::optional<int> requestedCameraNode;
  std::optional<double> requestedTime;
  std::optional<double> requestedSpeed;
  std::optional<bool> requestedPlaying;
  std::optional<bool> requestedLoop;
};
void DrawGltfAnimationControls(GltfAnimationControls& controls);
} // namespace container::ui
