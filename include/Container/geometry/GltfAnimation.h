#pragma once

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace container::geometry::gltf {

enum class AnimationInterpolation { Step, Linear, CubicSpline };
enum class AnimationTargetKind {
  NodeTranslation, NodeRotation, NodeScale, NodeWeights,
  Material, Light, Camera
};

struct AnimationTarget {
  AnimationTargetKind kind{};
  size_t index{};
  // Canonical property path relative to the typed object; e.g.
  // pbrMetallicRoughness/baseColorFactor or perspective/yfov.
  std::string property;
  size_t components{};
  // An individual vector/weights component targeted by a JSON pointer.
  std::optional<size_t> element;
  bool operator==(const AnimationTarget&) const = default;
};

struct AnimationTrack {
  AnimationTarget target;
  AnimationInterpolation interpolation{};
  std::vector<double> times;
  // Authored values are never rewritten/hemisphere-flipped. Cubic layout is
  // [in tangent, value, out tangent] per key, including morph weight arrays.
  std::vector<double> values;
  size_t sourceChannel{};
  bool operator==(const AnimationTrack&) const = default;
};

struct AnimationClip {
  std::string name;
  double duration{}; // Timeline begins at zero even if the first key is later.
  std::vector<AnimationTrack> tracks;
  bool operator==(const AnimationClip&) const = default;
};

struct AnimationDiagnostic {
  size_t clip{};
  size_t channel{};
  std::string message;
};

struct AnimationLibrary {
  std::vector<AnimationClip> clips;
  std::vector<AnimationDiagnostic> diagnostics;
  std::vector<tinygltf::Node> restNodes;
  std::vector<tinygltf::Material> restMaterials;
  std::vector<tinygltf::Light> restLights;
  std::vector<tinygltf::Camera> restCameras;
};

struct AnimatedValue {
  AnimationTarget target;
  std::vector<double> value;
};
struct AnimationPose { std::vector<AnimatedValue> values; };

struct AnimationDirtySet {
  std::vector<size_t> nodes;
  std::vector<size_t> weights; // Node indices, never mesh indices.
  std::vector<size_t> materials;
  std::vector<size_t> lights;
  std::vector<size_t> cameras;
  [[nodiscard]] bool empty() const;
  void merge(const AnimationDirtySet& other);
};

// Invalid supported channels throw invalid_argument with clip/channel context.
// Unknown pointer properties are diagnosed and omitted; this CPU API does not
// imply a corresponding renderer feature. Overlapping targets are rejected.
AnimationLibrary ParseAnimations(const tinygltf::Model& model);
AnimationPose SampleAnimation(const AnimationClip& clip, double timeSeconds);
// Publication is atomic: every sampled numeric value is validated before any
// write, and target objects are restored if a later property operation fails.
// Only touched source objects are staged; asset buffers/accessors are not copied.
AnimationDirtySet ApplyAnimationPose(tinygltf::Model& model,
                                     const AnimationPose& pose);
AnimationDirtySet RestoreAnimationRestPose(tinygltf::Model& model,
                                           const AnimationLibrary& library);

class AnimationPlayer {
public:
  explicit AnimationPlayer(const tinygltf::Model& model);
  explicit AnimationPlayer(AnimationLibrary library) : library_(std::move(library)) {}
  [[nodiscard]] const AnimationLibrary& library() const { return library_; }
  [[nodiscard]] std::optional<size_t> selectedClip() const { return selected_; }
  [[nodiscard]] double time() const { return time_; }
  [[nodiscard]] double speed() const { return speed_; }
  [[nodiscard]] bool playing() const { return playing_; }
  [[nodiscard]] bool loop() const { return loop_; }
  void play() { playing_ = selected_.has_value(); }
  void pause() { playing_ = false; }
  void setLoop(bool loop) { loop_ = loop; }
  void setSpeed(double speed);
  // Source data and playback time/selection/playing state commit together.
  // Failed sampling/publication leaves the previous successful state intact.
  AnimationDirtySet selectClip(tinygltf::Model& model, size_t clip, double timeSeconds = 0);
  AnimationDirtySet clearClip(tinygltf::Model& model);
  AnimationDirtySet seek(tinygltf::Model& model, double timeSeconds);
  AnimationDirtySet advance(tinygltf::Model& model, double deltaSeconds);

private:
  AnimationLibrary library_;
  std::optional<size_t> selected_;
  double time_{};
  double speed_{1.0};
  bool playing_{};
  bool loop_{true};
};

} // namespace container::geometry::gltf
