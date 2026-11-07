#pragma once

#include <tiny_gltf.h>
#include <Container/geometry/GltfMaterialAuthoring.h>
#include <Container/geometry/GltfCameraAuthoring.h>
#include <Container/geometry/GltfCameraPoseAuthoring.h>
#include <Container/geometry/GltfNodeFlags.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace container::geometry::gltf {

struct GltfSubtreeClonePlan;
struct GltfProjectedSubtreeClonePlan;
class GltfProjectedCloneOwnershipProof;

enum class AnimationInterpolation { Step, Linear, CubicSpline };
enum class AnimationTargetKind {
  NodeTranslation, NodeRotation, NodeScale, NodeWeights,
  Material, Light, Camera, NodeFlag
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
  // When captured for reset, retain whether a Boolean flag leaf was authored.
  std::optional<bool> nodeFlagAuthored{};
};
struct AnimationPose { std::vector<AnimatedValue> values; };

struct AnimatedNodeFlagChange {
  size_t node{};
  uint8_t mask{};
};

struct AnimationDirtySet {
  std::vector<size_t> nodes;
  std::vector<size_t> weights; // Node indices, never mesh indices.
  std::vector<size_t> materials;
  std::vector<size_t> lights;
  std::vector<size_t> cameras;
  std::vector<AnimatedNodeFlagChange> nodeFlags;
  [[nodiscard]] bool empty() const;
  void merge(const AnimationDirtySet& other);
};

// Invalid supported channels throw invalid_argument with clip/channel context.
// Unknown pointer properties are diagnosed and omitted; this CPU API does not
// imply a corresponding renderer feature. Overlapping targets are rejected.
AnimationLibrary ParseAnimations(const tinygltf::Model& model);
AnimationPose SampleAnimation(const AnimationClip& clip, double timeSeconds);
// Shared typed Object Model access for behavior graphs. Targets outside
// the animation pointer table return nullopt, with malformed paths diagnosed.
std::optional<AnimationTarget> ResolveAnimationPointer(const tinygltf::Model&,
    const std::string& pointer);
std::vector<double> ReadAnimationTarget(const tinygltf::Model&, const AnimationTarget&);
AnimatedValue CaptureAnimationTarget(const tinygltf::Model&, const AnimationTarget&);
bool ReadAnimatedNodeFlag(const tinygltf::Node&, GltfNodeFlag);
// Publication is atomic: every sampled numeric value is validated before any
// write, and target objects are restored if a later property operation fails.
// Only touched source objects are staged; asset buffers/accessors are not copied.
AnimationDirtySet ApplyAnimationPose(tinygltf::Model& model,
                                     const AnimationPose& pose);
AnimationDirtySet RestoreAnimationRestPose(tinygltf::Model& model,
                                           const AnimationLibrary& library);

struct GltfMaterialAnimationEditLimits {
  uint64_t maxOwnedBytes{256ull * 1024 * 1024};
  uint64_t maxDecodedBytes{128ull * 1024 * 1024};
  uint64_t maxMetadataValues{1'000'000};
  uint64_t maxChannels{1'000'000};
  uint64_t maxClips{65536};
  uint32_t maxMetadataDepth{64};
  GltfMaterialAuthoringLimits authoring{};
};

class AnimationPlayer;
struct GltfCameraAnimationEditLimits {
  uint64_t maxOwnedBytes{64ull * 1024 * 1024};
  uint64_t maxDecodedBytes{32ull * 1024 * 1024};
  uint64_t maxMetadataValues{1'000'000};
  uint64_t maxChannels{1'000'000}, maxClips{65536};
  uint32_t maxMetadataDepth{64};
  GltfCameraAuthoringLimits authoring{};
};
class PreparedCameraRestEdit {
public:
  PreparedCameraRestEdit() noexcept;
  ~PreparedCameraRestEdit();
  PreparedCameraRestEdit(PreparedCameraRestEdit&&) noexcept;
  PreparedCameraRestEdit& operator=(PreparedCameraRestEdit&&) noexcept;
  PreparedCameraRestEdit(const PreparedCameraRestEdit&) = delete;
  PreparedCameraRestEdit& operator=(const PreparedCameraRestEdit&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] const tinygltf::Camera& evaluatedCamera() const;
private:
  struct State;
  std::unique_ptr<State> state_;
  friend class AnimationPlayer;
};
struct GltfCameraNodeAnimationEditLimits {
  uint64_t maxOwnedBytes{64ull * 1024 * 1024};
  uint64_t maxDecodedBytes{32ull * 1024 * 1024};
  uint64_t maxMetadataValues{1'000'000};
  uint64_t maxChannels{1'000'000}, maxClips{65536};
  uint32_t maxMetadataDepth{64};
  GltfCameraPoseAuthoringLimits authoring{};
};
class PreparedCameraNodeRestEdit {
public:
  PreparedCameraNodeRestEdit() noexcept;
  ~PreparedCameraNodeRestEdit();
  PreparedCameraNodeRestEdit(PreparedCameraNodeRestEdit&&) noexcept;
  PreparedCameraNodeRestEdit& operator=(PreparedCameraNodeRestEdit&&) noexcept;
  PreparedCameraNodeRestEdit(const PreparedCameraNodeRestEdit&) = delete;
  PreparedCameraNodeRestEdit& operator=(const PreparedCameraNodeRestEdit&) = delete;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  [[nodiscard]] const tinygltf::Node& evaluatedNode() const;
private:
  struct State;
  std::unique_ptr<State> state_;
  friend class AnimationPlayer;
};
// Prepared strict decoder/source proof for the final material publication
// boundary. Owns raw animation metadata, selected encoded accessor ranges and
// exact decoded curves/diagnostics; never owns a Model, whole geometry buffer,
// image payload or player/rest snapshot. Copying the witness shares its proof.
class MaterialAnimationCurveWitness {
public:
  MaterialAnimationCurveWitness() noexcept = default;
  explicit operator bool() const noexcept;
  [[nodiscard]] uint64_t ownedBytes() const noexcept;
  // No decoding, allocation or exceptions, including stale/malformed inputs.
  // Player rests/counts and playback controls are deliberately outside this
  // curve proof. The outer publication must check current player identity and
  // clocks/rests, and call this for BOTH live and prospective players if any.
  [[nodiscard]] bool matches(const tinygltf::Model& currentSource,
      const AnimationPlayer&) const noexcept;
private:
  struct State;
  std::shared_ptr<const State> state_;
  friend class AnimationPlayer;
};

class AnimationPlayer {
public:
  explicit AnimationPlayer(const tinygltf::Model& model);
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
  // Prepare an append-only source extension without changing either source or
  // player. Existing source identities/clips must retain their order; existing
  // authored rest snapshots and all playback controls survive the extension.
  // Incoming rest snapshots come from appendedSource, so incoming objects must
  // still contain their authored values. No clip is sampled or auto-selected.
  [[nodiscard]] AnimationPlayer withExtendedSource(
      const tinygltf::Model& appendedSource) const;
  // Prepare exactly the sibling clone declared by the detached planner. The
  // current source witnesses unchanged raw clip/sampler/channel metadata; old
  // decoded tracks must still match this player. The only accepted channel
  // suffixes are ordered node-target copies with the declared appended IDs.
  // Old authored rest and playback controls survive, and each clone receives
  // its original node's authored numeric rest, independently of current pose.
  // Neither source is sampled or mutated. Camera selection remains caller-owned.
  [[nodiscard]] AnimationPlayer withClonedSource(
      const tinygltf::Model& currentSource,
      const tinygltf::Model& appendedSource,
      const GltfSubtreeClonePlan& clone) const;
  // Graph-derived copies require the opaque ownership witness and explicit
  // mesh/weight masks. Visual nodes without copied primitives omit weights;
  // nonvisual binding owners retain them. A graph-only copy may append no
  // source nodes. The evaluated output is taken from the validated plan.
  [[nodiscard]] AnimationPlayer withClonedSource(
      const tinygltf::Model& currentSource,
      const GltfProjectedSubtreeClonePlan& projected,
      const GltfProjectedCloneOwnershipProof& ownership) const;
  // Prepare only the authored material rests. The immutable planner receipt,
  // exact evaluated before record and exact previous authored rest must match.
  // No resource/model copy, clip selection, sampling or clock reset occurs.
  // Duplicate/append adds one untracked rest; removing that tail requires a
  // separate resource-history authority. An unavailable current raw material
  // pointer cannot become valid by aliasing the prospective appended ID.
  [[nodiscard]] AnimationPlayer withMaterialEdits(
      const tinygltf::Model& currentSource, const GltfMaterialAuthoringPlan& plan,
      const GltfMaterialAnimationEditLimits& limits = {}) const;
  // Prepare removal of exactly the last authored material rest. Current source
  // counts and decoded curves must still match this player, and no decoded or
  // raw material pointer (including unsupported properties) may reference the
  // removed ID or an unavailable ID. Current prefix rests, diagnostics and all
  // playback controls are copied without sampling or source mutation. Opaque
  // historical raw metadata is not retained by the player; current raw input
  // is validated, not compared to a historical byte snapshot. This helper alone
  // grants no material/resource deletion or history ownership authority.
  [[nodiscard]] AnimationPlayer withoutLastMaterialRest(
      const tinygltf::Model& currentSource,
      const tinygltf::Material& expectedAuthoredRest,
      const GltfMaterialAnimationEditLimits& limits = {}) const;
  // Replace one untracked rest without renumbering surviving materials. Used
  // by stable-slot retirement/restoration after the scene owner proves that
  // the material is unused. Every decoded/raw material pointer to this ID,
  // including unsupported suffixes, rejects; other rests, curves and playback
  // controls remain current. authoredSource supplies the independent archive
  // texture/declaration domain. This helper grants no deletion authority.
  [[nodiscard]] AnimationPlayer withUntrackedMaterialRest(
      const tinygltf::Model& currentSource,
      GltfMaterialAuthoringSourceView authoredSource, size_t material,
      const tinygltf::Material& expectedAuthoredRest,
      const tinygltf::Material& replacementAuthoredRest,
      const GltfMaterialAnimationEditLimits& limits = {}) const;
  // Bounded prepublication reparse against this captured library's exact
  // decoded curves/diagnostics. Material-rest count can differ by a prepared
  // tail append/removal. Allocates validation scratch, never mutates or samples.
  void validateMaterialEditCurves(const tinygltf::Model& currentSource,
      const GltfMaterialAnimationEditLimits& limits = {}) const;
  // Fallible bounded preparation using the existing strict parser/curve
  // compatibility check. All copies and selected byte gathering precede final
  // publication; matches() subsequently rechecks exact source/layout/curves
  // without a second decoder or allocation. Unrelated source payloads are not
  // copied. In particular strided input captures elements, not stride gaps.
  [[nodiscard]] MaterialAnimationCurveWitness prepareMaterialEditCurveWitness(
      const tinygltf::Model& currentSource,
      const GltfMaterialAnimationEditLimits& limits = {}) const;
  // Selected-camera rest transaction. Only the selected rest, decoded camera
  // tracks and their exact raw samplers/accessor byte ranges are retained.
  // Other rests/clips/curves and every playback control remain current. The
  // token also owns the candidate camera with edited defaults and today's
  // selected tracks applied to those leaves. Unowned evaluated fields survive.
  [[nodiscard]] PreparedCameraRestEdit prepareCameraRestEdit(
      const GltfCameraProjectionPlan&, GltfCameraAuthoringSourceView authoredSource,
      const tinygltf::Model& evaluatedSource, const GltfCameraAnimationEditLimits& limits = {}) const;
  [[nodiscard]] bool cameraRestEditMatches(const PreparedCameraRestEdit&,
      GltfCameraAuthoringSourceView authoredSource, const tinygltf::Model& evaluatedSource) const noexcept;
  // All guards run before a preconstructed three-move-assignment exchange.
  // Call before the enclosing source A/E row swaps. Failed/consumed tokens do
  // nothing; old selected rest remains token-owned until release outside commit.
  [[nodiscard]] bool exchangeCameraRestEdit(PreparedCameraRestEdit&,
      GltfCameraAuthoringSourceView authoredSource, const tinygltf::Model& evaluatedSource) noexcept;
  // Pose-only selected-node rest transaction for write-from-view. Stores only
  // the selected rest's matrix/TRS arrays; current morph weights, metadata,
  // associations, other rest rows and the decoded library remain untouched.
  // Owned TRS defaults are sampled at today's selected clip/time into the
  // detached evaluatedNode candidate. Untouched evaluated fields and weights
  // survive. Selected pose/weight curves retain exact decoded/raw proofs.
  [[nodiscard]] PreparedCameraNodeRestEdit prepareCameraNodeRestEdit(
      const GltfCameraPosePlan&, GltfCameraPoseSourceView authoredSource,
      const tinygltf::Model& evaluatedSource, const GltfCameraNodeAnimationEditLimits& limits = {}) const;
  [[nodiscard]] bool cameraNodeRestEditMatches(const PreparedCameraNodeRestEdit&,
      GltfCameraPoseSourceView authoredSource, const tinygltf::Model& evaluatedSource) const noexcept;
  // Call before enclosing A/E pose-array swaps, after all outer graph/view
  // guards. Only preallocated vector swaps run after the final exact proof.
  [[nodiscard]] bool exchangeCameraNodeRestEdit(PreparedCameraNodeRestEdit&,
      GltfCameraPoseSourceView authoredSource, const tinygltf::Model& evaluatedSource) noexcept;
  // Restore only this material's animation-targeted leaves/components from
  // prepared authored rest, then sample its selected clip at the current
  // clock. Missing active tracks use rest even after clearing/changing clips.
  // Untracked evaluated leaves/resources and untracked duplicates retain their
  // supplied appearance. The source/player remain unchanged.
  [[nodiscard]] tinygltf::Material evaluateMaterialRestEdit(
      size_t materialId, const tinygltf::Material& preparedRecord,
      const GltfMaterialAnimationEditLimits& limits = {}) const;
  // Source data and playback time/selection/playing state commit together.
  // Failed sampling/publication leaves the previous successful state intact.
  AnimationDirtySet selectClip(tinygltf::Model& model, size_t clip);
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
  uint64_t cameraRestRevision_{};
  uint64_t cameraNodeRestRevision_{};
};

} // namespace container::geometry::gltf
