#pragma once

#include <Container/geometry/GltfSubtreeClone.h>
#include <memory>

namespace container::scene {
struct GraphCloneProjectionInput;
struct GraphCloneProjectionLimits;
struct GraphCloneProjectionPlan;
GraphCloneProjectionPlan BuildGraphCloneProjection(
    const GraphCloneProjectionInput&, const GraphCloneProjectionLimits&);
}

namespace container::geometry::gltf {

enum class GltfCloneNodeRole : uint8_t { Visual, MeshBindingOnly, JointTransformOnly };

struct GltfProjectedCloneNode {
  int32_t sourceNode{-1};
  // An original source ID included in this projection, or -1 for a new root.
  // Only appended source nodes receive these edges; old topology is unchanged.
  int32_t parentSourceNode{-1};
  GltfCloneNodeRole role{GltfCloneNodeRole::Visual};
  // Orthogonal to visibility: actual copied primitive-owner coverage decides
  // whether this node still needs a mesh/skin/weights/instancing association.
  bool retainMeshBinding{false};
  std::string name;
  bool visible{true}, selectable{true}, hoverable{true};
};

struct GltfSubtreeCloneProjection {
  // Stable desired appended-source order. Each old source may occur once.
  std::vector<GltfProjectedCloneNode> nodes;
  std::vector<size_t> affectedScenes;
  size_t destinationScene{};
};

struct GltfProjectedSubtreeClonePlan {
  GltfSubtreeClonePlan clone;
  // Indexed by ORIGINAL source ID; ignored when clone.nodes[id] == -1.
  std::vector<GltfCloneNodeRole> roles;
  std::vector<bool> retainMeshBindings, copyWeightTracks;
};

// Immutable graph-derived ownership witness. Only the actual graph projection
// builder can create it; a mutable source-role/mask table alone is insufficient
// to prove that no external primitive was pulled into the duplicate. This
// checks source identity/attachment projection, not graph staleness: callers
// must separately publish the prepared graph against its identity/revision.
class GltfProjectedCloneOwnershipProof {
public:
  GltfProjectedCloneOwnershipProof() = default;
  [[nodiscard]] explicit operator bool() const noexcept { return bool(state_); }
  [[nodiscard]] uint64_t retainedBytes() const noexcept;
  void validate(const tinygltf::Model& currentSource,
                const GltfProjectedSubtreeClonePlan& plan) const;
private:
  struct State;
  std::shared_ptr<const State> state_;
  GltfProjectedCloneOwnershipProof(const tinygltf::Model&,const GltfSubtreeCloneProjection&,
                                  uint64_t maxRetainedBytes);
  friend scene::GraphCloneProjectionPlan scene::BuildGraphCloneProjection(
      const scene::GraphCloneProjectionInput&,const scene::GraphCloneProjectionLimits&);
};

// Graph-driven source projection. Retained source nodes/scene-root prefixes,
// resource tables and raw clip prefixes remain exact. New nodes use the
// explicit projected forest, so graph-only intermediates/reparenting do not
// require rewriting old source topology or baking animation curves.
// Runtime graph publication is separate and remains authoritative for actual
// intermediate transforms/visual attachments. New source roots are registered
// only in affectedScenes. Visual nodes can drop an unused mesh binding while
// retaining their camera/light/flags. Binding-only nodes retain mesh animation
// but no camera/light, and transform-only nodes carry no visual attachments.
// No I/O or source mutation. The legacy canonical clone/player API is unchanged;
// projected playback requires a proof consuming the explicit copyWeightTracks
// mask, rather than inferring weight-channel coverage from contextOnly alone.
[[nodiscard]] GltfProjectedSubtreeClonePlan PlanGltfProjectedSubtreeClone(
    const tinygltf::Model& evaluatedSource, const tinygltf::Model& authoredArchive,
    const GltfSubtreeCloneProjection& projection,
    const GltfSubtreeCloneRestrictions& restrictions = {}, const GltfMergeLimits& limits = {});

} // namespace container::geometry::gltf
