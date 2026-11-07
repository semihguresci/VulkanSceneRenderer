#pragma once

#include <tiny_gltf.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace container::geometry::gltf {

enum class GltfCameraProjection : uint8_t { Perspective, Orthographic };
enum class GltfCameraProperty : uint8_t {
  PerspectiveYfov, PerspectiveAspectRatio, PerspectiveNear, PerspectiveFar,
  OrthographicXmag, OrthographicYmag, OrthographicNear, OrthographicFar, Count
};
[[nodiscard]] std::string_view GltfCameraPropertyPath(GltfCameraProperty) noexcept;
[[nodiscard]] std::optional<GltfCameraProperty> ParseGltfCameraProperty(std::string_view) noexcept;
// monostate means OMIT, and is accepted only for perspective aspectRatio/zfar.
// An absent edit leaves that field unchanged. Neither viewport aspect nor the
// finite shadow/culling fallback for an infinite camera is authored implicitly.
using GltfCameraNumericValue = std::variant<std::monostate, double>;
struct GltfCameraNumericEdit { GltfCameraProperty property; GltfCameraNumericValue value; };
struct GltfCameraProjectionPatch {
  std::optional<GltfCameraProjection> type;
  std::vector<GltfCameraNumericEdit> values;
};
struct GltfCameraAuthoringLimits {
  uint64_t maxOwnedBytes{16ull * 1024 * 1024};
  uint64_t maxMetadataValues{262144};
  uint32_t maxMetadataDepth{64};
  uint64_t maxCameraRecords{1'000'000}, maxNodes{1'000'000};
  uint64_t maxAnimationClips{65536}, maxAnimationChannels{1'000'000};
  uint64_t maxPointerBytes{4096}, maxPatchOperations{8};
};
struct GltfCameraAuthoringSourceView {
  std::span<const tinygltf::Camera> cameras;
  std::span<const tinygltf::Node> nodes;
  std::span<const tinygltf::Animation> animations;
};
[[nodiscard]] GltfCameraAuthoringSourceView GltfCameraSourceView(const tinygltf::Model&) noexcept;
struct GltfCameraAnimationBinding {
  size_t clip{}, channel{};
  GltfCameraProperty property{};
  bool operator==(const GltfCameraAnimationBinding&) const = default;
};
struct GltfCameraProjectionOwnership {
  uint16_t properties{};
  bool type{};
};
class GltfCameraAuthoringReceipt;
struct GltfCameraAuthoringAccess;
struct GltfCameraProjectionPlan {
  // Caller resolves the two source identities explicitly; equal numeric IDs
  // are never used as evidence that archive/evaluated definitions correspond.
  int32_t authoredCamera{-1}, evaluatedCamera{-1};
  tinygltf::Camera authoredBefore, authoredAfter, evaluatedBefore, evaluatedAfter;
  // All source nodes referencing the definition, including inactive scenes.
  // A projection edit affects all these uses, but changes no node pose.
  std::vector<uint32_t> authoredNodes, evaluatedNodes;
  std::vector<GltfCameraAnimationBinding> authoredAnimationBindings, evaluatedAnimationBindings;
  std::vector<GltfCameraProperty> changedProperties;
  bool changedType{}, changed{};
  uint64_t peakOwnedBytes{};
 private:
  std::shared_ptr<const GltfCameraAuthoringReceipt> receipt_;
  friend struct GltfCameraAuthoringAccess;
};

// Pure selected-record preparation: no model/binary/graph copy, mutation,
// projection convention change, I/O or GPU work. Opaque camera metadata and raw
// JSON caches survive exactly, including the dormant projection block.
// Unanimated edits apply independently to authored/evaluated records;
// untouched values remain distinct. Animated scalar edits change authored rest
// values while preserving the current evaluated sample. The player transaction
// restores the new defaults and resamples the selected clip before publication.
// Animated optional leaves must remain present. Animated type switches reject.
// Type switches require explicit yfov/near or all four orthographic fields,
// even when the dormant block was valid. Optional perspective aspect/far keep
// their dormant values unless explicitly edited/omitted. Edits for the other
// projection type reject; inactive fields are never silently edited.
[[nodiscard]] GltfCameraProjectionPlan PlanGltfCameraProjectionEdit(
    GltfCameraAuthoringSourceView authored, GltfCameraAuthoringSourceView evaluated,
    int32_t authoredCamera, int32_t evaluatedCamera,
    const GltfCameraProjectionPatch&, const GltfCameraAuthoringLimits& limits = {});

// Exact public-plan receipt and immediate-before records, shared-node IDs and
// relevant raw animation targets. Unrelated camera/pose/animation changes need
// not invalidate a projection-only receipt. All walks use the captured finite
// limits; this predicate allocates nothing, including on stale input.
[[nodiscard]] bool GltfCameraProjectionEditMatches(
    const GltfCameraProjectionPlan&, GltfCameraAuthoringSourceView authored,
    GltfCameraAuthoringSourceView evaluated) noexcept;
void ValidateGltfCameraProjectionEditBefore(
    const GltfCameraProjectionPlan&, GltfCameraAuthoringSourceView authored,
    GltfCameraAuthoringSourceView evaluated);

// Selected-record helpers; equality includes raw JSON, opaque Value types and
// floating-point bits. Equality inputs must already have bounded metadata.
[[nodiscard]] uint64_t GltfCameraRecordStorageBytes(const tinygltf::Camera&,
    const GltfCameraAuthoringLimits& limits = {});
[[nodiscard]] bool GltfCameraRecordsEqual(const tinygltf::Camera&, const tinygltf::Camera&) noexcept;
[[nodiscard]] bool GltfCameraProjectionValid(const tinygltf::Camera&) noexcept;
// Checks the immutable public-plan receipt before exposing its owned fields.
[[nodiscard]] GltfCameraProjectionOwnership GltfCameraProjectionOwnedFields(const GltfCameraProjectionPlan&);

enum class GltfCameraEditSide : uint8_t { Before, After };
// A fresh immediate-before receipt for Undo/Redo. Only the original requested
// projection leaves/type are restored; current opaque metadata, unowned lens
// fields and evaluated animation of other fields survive. Owned current leaves
// must exactly match the opposite trusted endpoint. Context IDs/shared-node
// uses/raw camera targets remain exact. The immutable original command is kept
// once, without chaining previews. Animated evaluated leaves may advance with
// the current clip; their authored endpoints and curve targets remain exact.
[[nodiscard]] GltfCameraProjectionPlan RebaseGltfCameraProjectionEdit(
    const GltfCameraProjectionPlan&, GltfCameraEditSide,
    GltfCameraAuthoringSourceView currentAuthored, GltfCameraAuthoringSourceView currentEvaluated,
    const GltfCameraAuthoringLimits& limits = {});

} // namespace container::geometry::gltf
