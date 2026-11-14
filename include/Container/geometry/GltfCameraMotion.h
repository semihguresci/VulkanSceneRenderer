#pragma once

#include <tiny_gltf.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace container::geometry::gltf {

struct GltfCameraMotionPose {
  std::array<float, 3> translation{};
  std::array<float, 4> rotation{0, 0, 0, 1}; // glTF x, y, z, w
  bool operator==(const GltfCameraMotionPose&) const = default;
};

struct GltfCameraMotionKey {
  float time{};
  GltfCameraMotionPose pose;
  bool operator==(const GltfCameraMotionKey&) const = default;
};

struct GltfCameraMotionClip {
  uint32_t sourceNode{};
  std::string name;
  std::vector<GltfCameraMotionKey> keys;
  bool operator==(const GltfCameraMotionClip&) const = default;
};

enum class GltfCameraMotionKeyAction { Capture, Move, Delete };

[[nodiscard]] GltfCameraMotionClip EditGltfCameraMotionKey(
    const GltfCameraMotionClip&, GltfCameraMotionKeyAction, double time,
    std::optional<double> destinationTime = {}, std::optional<GltfCameraMotionPose> pose = {});

// Plain glTF extras identify editor-owned camera clips. A zero-key draft keeps
// its session index for undo but is omitted when the scene is exported.
[[nodiscard]] bool IsGltfCameraMotionDraft(const tinygltf::Animation&);
[[nodiscard]] std::optional<GltfCameraMotionClip> ReadGltfCameraMotion(const tinygltf::Model&, size_t clip);

class PreparedGltfCameraMotionEdit {
 public:
  PreparedGltfCameraMotionEdit() noexcept;
  ~PreparedGltfCameraMotionEdit();
  PreparedGltfCameraMotionEdit(PreparedGltfCameraMotionEdit&&) noexcept;
  PreparedGltfCameraMotionEdit& operator=(PreparedGltfCameraMotionEdit&&) noexcept;
  PreparedGltfCameraMotionEdit(const PreparedGltfCameraMotionEdit&) = delete;
  PreparedGltfCameraMotionEdit& operator=(const PreparedGltfCameraMotionEdit&) = delete;

 private:
  struct State;
  std::unique_ptr<State> state_;
  friend PreparedGltfCameraMotionEdit PrepareGltfCameraMotionEdit(const tinygltf::Model&, size_t,
      const std::optional<GltfCameraMotionClip>&, const tinygltf::Node*);
  friend void ReserveGltfCameraMotionEdit(tinygltf::Model&, const PreparedGltfCameraMotionEdit&);
  friend void PublishGltfCameraMotionEdit(tinygltf::Model&, PreparedGltfCameraMotionEdit&&) noexcept;
};

// A missing replacement removes only an editor-owned tail clip (undo create).
// Preparation copies only animation-side data. cameraRest, if supplied, is
// an equivalent prospective node in TRS form for a matrix-authored camera.
[[nodiscard]] PreparedGltfCameraMotionEdit PrepareGltfCameraMotionEdit(
    const tinygltf::Model&, size_t clip, const std::optional<GltfCameraMotionClip>&,
    const tinygltf::Node* cameraRest = nullptr);
// Reserve every affected model before publishing any of them. The caller
// holds exclusive access from preparation through publication. The prepared
// camera node pose can be assigned between reserve and publication.
void ReserveGltfCameraMotionEdit(tinygltf::Model&, const PreparedGltfCameraMotionEdit&);
void PublishGltfCameraMotionEdit(tinygltf::Model&, PreparedGltfCameraMotionEdit&&) noexcept;
void ApplyGltfCameraMotion(tinygltf::Model&, size_t clip, const std::optional<GltfCameraMotionClip>&);

} // namespace container::geometry::gltf
