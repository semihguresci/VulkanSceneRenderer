#pragma once

#include <Container/utility/SceneGraph.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace container::scene {

struct SceneNodeAuthoringLimits {
  size_t maxNameBytes{4096};
  // Bounds the graph-indexed scratch storage as well as the visited nodes.
  uint64_t maxVisitedNodes{262144};
  // Child references plus active-view root entries; checked before caching.
  uint64_t maxVisitedEdges{524288};
};

// The caller supplies live binding identities, never names or guessed node
// shapes. Bodies/joint endpoints alone do not forbid a metadata rename or an
// empty leaf child; product-owned internal nodes cannot receive editor children.
struct SceneNodeAuthoringRestrictions {
  std::span<const uint32_t> protectedNodes{};
  std::span<const uint32_t> physicsProductNodes{};
  std::span<const uint32_t> physicsBodyNodes{};
  std::span<const uint32_t> jointEndpointNodes{};
  // Caller-resolved graph identities required by retained skin consumers
  // (joints/skeletons). No source/graph index guessing is performed here.
  std::span<const uint32_t> skinDependencyNodes{};
};

enum class SceneNodeAuthoringPurpose { Rename, CreateEmptyChild, DeleteSubtree, DuplicateSubtree };

// New user labels: nonblank, bounded UTF-8, without C0/C1 controls or line
// separators. Does not normalize the text. Undo may restore an imported old
// label verbatim without applying this new-input policy a second time.
// Every rejection in this helper uses contextual std::invalid_argument.
void ValidateSceneNodeName(std::string_view name,
                          const SceneNodeAuthoringLimits& limits = {});

// Resolve an imported synthetic renderable through its immutable mesh owner,
// independent of its current parent/name. Source-unmapped procedural/aggregate
// primitives remain editable themselves. Rename rejects tagged structural
// wrappers; CreateEmptyChild accepts the tagged editor root as context while
// rejecting import interaction boundaries. This chooses the parent of a NEW
// leaf, and does not wrap/reparent the existing selection. Requires the selected
// target to belong to the active hierarchy, except that the editor root remains
// an attachment context for an empty scene. Visibility is not a restriction.
[[nodiscard]] uint32_t ResolveSceneNodeAuthoringTarget(
    const SceneGraph& graph, uint32_t selected,
    SceneNodeAuthoringPurpose purpose,
    const SceneNodeAuthoringRestrictions& restrictions = {},
    const SceneNodeAuthoringLimits& limits = {});

struct SceneNodeAuthoringSubtree {
  // Stable parent-before-child preorder, preserving each child list's order.
  std::vector<uint32_t> nodes;
  bool containsProtectedNodes{};
  bool containsPhysicsProductNodes{};
  bool containsPhysicsBodies{};
  bool containsJointEndpoints{};
  bool containsSkinDependencies{};
};

// Inspect the exact root supplied; no owner redirection and no mutation.
// Iterative O(graph nodes + supplied metadata + visited edges), with bounded
// scratch space. Invalid children, repeated/cyclic edges and inconsistent child
// parent links reject. Constraint flags report caller metadata, not permission
// to duplicate/delete bodies, joints, or protected source/catalog roots.
[[nodiscard]] SceneNodeAuthoringSubtree CollectSceneNodeAuthoringSubtree(
    const SceneGraph& graph, uint32_t root,
    const SceneNodeAuthoringRestrictions& restrictions = {},
    const SceneNodeAuthoringLimits& limits = {});

// Resolve the editable source owner, collect its entire stable-slot subtree,
// and reject structural/protected/product/body/joint/skin dependencies before
// any history gesture or graph mutation. Caller metadata must include retained
// consumers in every scene, including currently inactive source content.
[[nodiscard]] SceneNodeAuthoringSubtree ValidateSceneNodeRetirement(
    const SceneGraph& graph, uint32_t selected,
    const SceneNodeAuthoringRestrictions& restrictions = {},
    const SceneNodeAuthoringLimits& limits = {});

} // namespace container::scene
