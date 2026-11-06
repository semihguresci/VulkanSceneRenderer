#pragma once

#include <Container/geometry/GltfCameraAuthoring.h>

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace container::geometry::gltf {

using GltfCameraPoseSourceView = GltfCameraAuthoringSourceView;
[[nodiscard]] GltfCameraPoseSourceView GltfCameraPoseView(const tinygltf::Model&) noexcept;

struct GltfCameraPoseGraphState {
  uint64_t identity{}, revision{}, nodeIncarnation{}, parentIncarnation{};
  uint32_t nodeCount{}, node{UINT32_MAX}, parent{UINT32_MAX};
  size_t siblingIndex{};
  glm::mat4 parentWorld{1.f}, local{1.f}, world{1.f};
  float viewportAspect{1.f};
};
struct GltfCameraPoseAuthoringLimits {
  uint64_t maxOwnedBytes{32ull * 1024 * 1024};
  uint64_t maxMetadataValues{262144};
  uint32_t maxMetadataDepth{64};
  uint64_t maxNodes{1'000'000}, maxCameraRecords{1'000'000}, maxGraphNodes{1'000'000};
  uint64_t maxAnimationClips{65536}, maxAnimationChannels{1'000'000}, maxPointerBytes{4096};
  double maxConditionNumber{1e8}, maxRelativeError{1e-5}, maxAngularError{1e-5}, maxPositionAbsoluteError{1e-4};
};
enum class GltfCameraNodeAnimationProperty : uint8_t { Weights };
struct GltfCameraNodeAnimationBinding {
  size_t clip{}, channel{};
  GltfCameraNodeAnimationProperty property{GltfCameraNodeAnimationProperty::Weights};
  bool operator==(const GltfCameraNodeAnimationBinding&) const = default;
};
class GltfCameraPoseReceipt;
struct GltfCameraPoseAccess;
struct GltfCameraPosePlan {
  int32_t authoredNode{-1}, evaluatedNode{-1}, authoredCamera{-1}, evaluatedCamera{-1};
  tinygltf::Node authoredBefore, authoredAfter, evaluatedBefore, evaluatedAfter;
  GltfCameraPoseGraphState graphBefore;
  glm::mat4 desiredView{1.f}, localAfter{1.f}, worldAfter{1.f}, viewAfter{1.f};
  std::vector<GltfCameraNodeAnimationBinding> authoredAnimationBindings, evaluatedAnimationBindings;
  bool changed{};
  uint64_t peakOwnedBytes{};
private:
  std::shared_ptr<const GltfCameraPoseReceipt> receipt_;
  friend struct GltfCameraPoseAccess;
};

// Pure pose-only preparation. Uses the actual current graph-parent world,
// preserves current world linear residual (scale/shear/reflection), and checks
// float publication through the existing camera evaluator. Lens definitions,
// node metadata/associations/weights and whole source/graph storage are not
// edited. Source node IDs are explicit independent A/E identities.
// Selected-node TRS/matrix animation in any clip rejects; weights and current
// animated ancestors are allowed. Graph/world provenance and incarnations must
// be resolved by the scene owner, not inferred from these numeric values.
[[nodiscard]] GltfCameraPosePlan PlanGltfCameraViewPose(
    GltfCameraPoseSourceView authored, GltfCameraPoseSourceView evaluated,
    int32_t authoredNode, int32_t evaluatedNode, const GltfCameraPoseGraphState& actualGraph,
    const glm::mat4& desiredUnjitteredView, const GltfCameraPoseAuthoringLimits& limits = {});

// Exact immutable public receipt, current source pose/metadata/associations,
// lens definitions, selected raw animation targets and actual graph witness.
// No allocation or decoding. Current selected weights are intentionally outside
// pose ownership. A consumer MUST exchange only matrix/TRS vectors, preserving
// current weights and other node fields; no full Node swap is authorized.
[[nodiscard]] bool GltfCameraPoseEditMatches(const GltfCameraPosePlan&,
    GltfCameraPoseSourceView authored, GltfCameraPoseSourceView evaluated,
    const GltfCameraPoseGraphState& currentGraph) noexcept;
void ValidateGltfCameraPoseEditBefore(const GltfCameraPosePlan&,
    GltfCameraPoseSourceView authored, GltfCameraPoseSourceView evaluated,
    const GltfCameraPoseGraphState& currentGraph);

// Restores the original owned matrix/TRS endpoint against today's parent world
// and other current node/lens/weight metadata. Graph identity, node and parent
// births, parent ID and sibling placement must remain original; revision and
// parent animation/world may advance. No camera/parent/timeline is rewound.
[[nodiscard]] GltfCameraPosePlan RebaseGltfCameraViewPose(const GltfCameraPosePlan&,
    GltfCameraEditSide, GltfCameraPoseSourceView authored, GltfCameraPoseSourceView evaluated,
    const GltfCameraPoseGraphState& currentGraph, const GltfCameraPoseAuthoringLimits& limits = {});

[[nodiscard]] uint64_t GltfCameraPoseNodeStorageBytes(const tinygltf::Node&,
    const GltfCameraPoseAuthoringLimits& limits = {});
// Exact optional representations and numeric bits. Inputs must already be
// bounded resident records. Pose equality intentionally ignores weights.
[[nodiscard]] bool GltfCameraNodePoseEqual(const tinygltf::Node&, const tinygltf::Node&) noexcept;
[[nodiscard]] bool GltfCameraPoseNodeRecordsEqual(const tinygltf::Node&, const tinygltf::Node&) noexcept;

// Bounded selected raw-light/extension witnesses for the enclosing graph-pose
// publication. Storage helpers do not copy, decode or claim schema authority.
[[nodiscard]] uint64_t GltfAuthoringValueStorageBytes(const tinygltf::Value&,
    const GltfCameraPoseAuthoringLimits& limits = {});
// Inputs must already be bounded; use ValuesMatch for untrusted current data.
[[nodiscard]] bool GltfAuthoringValuesEqual(const tinygltf::Value&, const tinygltf::Value&) noexcept;
[[nodiscard]] bool GltfCameraPoseValuesMatch(const tinygltf::Value&, const tinygltf::Value&,
    const GltfCameraPoseAuthoringLimits& limits = {}) noexcept;
[[nodiscard]] uint64_t GltfCameraPoseLightStorageBytes(const tinygltf::Light&,
    const GltfCameraPoseAuthoringLimits& limits = {});
[[nodiscard]] bool GltfCameraPoseLightRecordsMatch(const tinygltf::Light&, const tinygltf::Light&,
    const GltfCameraPoseAuthoringLimits& limits = {}) noexcept;

} // namespace container::geometry::gltf
