#pragma once

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::scene {

struct SceneNode {
  glm::mat4 localTransform{1.0f};
  glm::mat4 worldTransform{1.0f};
  uint32_t parent = std::numeric_limits<uint32_t>::max();
  uint32_t materialIndex = 0;
  uint32_t primitiveIndex = std::numeric_limits<uint32_t>::max();
  // Imported group identity; synthetic primitive children and authored nodes
  // use -1. Animation and export share this association with the source asset.
  int32_t sourceNodeIndex = -1;
  // Editor's outer transform anchor; independent of its user-editable label.
  bool editorSceneRoot = false;
  // Original imported mesh group that owns a skin/morph instance. This stays
  // stable when its synthetic primitive child is reparented by the editor.
  uint32_t meshOwnerGraphNode = std::numeric_limits<uint32_t>::max();
  bool renderable = false;
  bool visible = true;
  std::string name{};
  std::vector<uint32_t> children{};
  // Primary asset primitive identity; independent of graph parenting and names.
  int32_t sourceMeshIndex = -1;
  int32_t sourcePrimitiveIndex = -1;
  int32_t sourceInstanceIndex = -1;
  // Local source flags inherit independently through the graph. Synthetic
  // primitive children retain true and inherit their owning source group.
  bool selectable = true;
  bool hoverable = true;
  // Import/editor transform wrappers are not authored interaction targets.
  bool interactionRootBoundary = false;
  // Stable editor deletion. The slot and complete forest topology remain
  // addressable for history/source bindings, but never contribute live content.
  bool retired = false;
  bool operator==(const SceneNode&) const = default;
};

struct SceneRetirementLimits {
  uint64_t maxNodes{262144};
  uint64_t maxEdges{524288};
  uint64_t maxPayloadBytes{64ull * 1024 * 1024};
};

struct SceneNodeLocalPose {
  uint32_t node;
  glm::mat4 localTransform{1.0f};
};

struct ScenePlacementLimits {
  uint64_t maxNodes{262144};
  uint64_t maxEdges{524288};
  uint64_t maxPayloadBytes{64ull * 1024 * 1024};
};

class SceneGraph {
  // The material authoring helper publishes a proved ID-only batch without
  // copying topology or invalidating cached world/active-membership data.
  friend struct MaterialBindingGraphAccess;
 public:
  static constexpr uint32_t kInvalidNode =
      std::numeric_limits<uint32_t>::max();

  // Retains geometry references and stable node indices while an appended
  // editor operation is undone. Tail payload/topology and the exact optional
  // before/after active views are saved; retained payloads remain live.
  struct DetachedTail {
    uint32_t firstNode{0};
    std::vector<SceneNode> nodes{};

   private:
    friend class SceneGraph;
    struct Position {
      uint32_t node;
      size_t position;
    };
    struct ChildPosition {
      uint32_t parent;
      uint32_t child;
      size_t position;
    };
    uint64_t graphIdentity{0};
    // Original slot lifetimes are private authority. An equal freshly appended
    // branch must not impersonate a detached tail or its retained parents.
    std::vector<uint64_t> nodeIncarnations{};
    std::vector<uint64_t> retainedIncarnations{};
    uint64_t retainedTopologyFingerprint{0};
    std::vector<Position> roots{};
    std::vector<Position> renderables{};
    std::vector<ChildPosition> externalChildren{};
    std::optional<std::vector<uint32_t>> detachedActiveRoots{};
    std::optional<std::vector<uint32_t>> restoredActiveRoots{};
  };

  // Opaque, single-use publication proof. All allocation and validation happen
  // in prepareAppend; moving or destroying the proof needs no complete State
  // definition in callers. Successful publication retains old storage here
  // until the consumed proof is destroyed.
  class PreparedAppend {
   public:
    PreparedAppend() noexcept;
    ~PreparedAppend();
    PreparedAppend(PreparedAppend&&) noexcept;
    PreparedAppend& operator=(PreparedAppend&&) noexcept;
    PreparedAppend(const PreparedAppend&) = delete;
    PreparedAppend& operator=(const PreparedAppend&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] const SceneGraph* candidateGraph() const noexcept;

   private:
    friend class SceneGraph;
    struct State;
    std::unique_ptr<State> state_;
  };

  // Detached undo/redo preparation owns every potentially throwing copy,
  // topology/world evaluation and active-membership allocation. The candidate
  // is available while staging the matching renderer generation. A successful
  // detach publication yields its receipt exactly once, without allocating.
  class PreparedTailChange {
   public:
    PreparedTailChange() noexcept;
    ~PreparedTailChange();
    PreparedTailChange(PreparedTailChange&&) noexcept;
    PreparedTailChange& operator=(PreparedTailChange&&) noexcept;
    PreparedTailChange(const PreparedTailChange&) = delete;
    PreparedTailChange& operator=(const PreparedTailChange&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] const SceneGraph* candidateGraph() const noexcept;
    [[nodiscard]] std::optional<DetachedTail> takeDetachedTail() noexcept;

   private:
    friend class SceneGraph;
    struct State;
    std::unique_ptr<State> state_;
  };

  // One exact parent/local/sibling placement and active-view publication.
  // The detached candidate is available to stage matching external metadata.
  class PreparedPlacement {
   public:
    PreparedPlacement() noexcept;
    ~PreparedPlacement();
    PreparedPlacement(PreparedPlacement&&) noexcept;
    PreparedPlacement& operator=(PreparedPlacement&&) noexcept;
    PreparedPlacement(const PreparedPlacement&) = delete;
    PreparedPlacement& operator=(const PreparedPlacement&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] const SceneGraph* candidateGraph() const noexcept;

   private:
    friend class SceneGraph;
    struct State;
    std::unique_ptr<State> state_;
  };

  // Exact stable-slot retirement delta, including already-retired descendants.
  // The caller owns physics/skin/product validation and the matching paused
  // animation/source/lighting snapshots. This receipt never erases source IDs.
  class SubtreeRetirement {
   public:
    SubtreeRetirement() noexcept;
    ~SubtreeRetirement();
    SubtreeRetirement(SubtreeRetirement&&) noexcept;
    SubtreeRetirement& operator=(SubtreeRetirement&&) noexcept;
    SubtreeRetirement(const SubtreeRetirement&) = delete;
    SubtreeRetirement& operator=(const SubtreeRetirement&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] uint32_t rootNode() const noexcept;
    [[nodiscard]] std::span<const uint32_t> nodes() const noexcept;

   private:
    friend class SceneGraph;
    struct State;
    std::unique_ptr<State> state_;
  };

  class PreparedRetirement {
   public:
    PreparedRetirement() noexcept;
    ~PreparedRetirement();
    PreparedRetirement(PreparedRetirement&&) noexcept;
    PreparedRetirement& operator=(PreparedRetirement&&) noexcept;
    PreparedRetirement(const PreparedRetirement&) = delete;
    PreparedRetirement& operator=(const PreparedRetirement&) = delete;
    explicit operator bool() const noexcept;
    [[nodiscard]] const SceneGraph* candidateGraph() const noexcept;
    // Initial successful publication yields its history receipt once.
    [[nodiscard]] std::optional<SubtreeRetirement> takeRetirement() noexcept;

   private:
    friend class SceneGraph;
    struct State;
    std::unique_ptr<State> state_;
  };

  SceneGraph();
  SceneGraph(const SceneGraph &other);
  SceneGraph &operator=(const SceneGraph &other);
  SceneGraph(SceneGraph &&other) noexcept;
  SceneGraph &operator=(SceneGraph &&other) noexcept;

  uint32_t createNode(const glm::mat4 &localTransform, uint32_t materialIndex,
                      bool renderable = false,
                      uint32_t primitiveIndex = kInvalidNode,
                      std::string name = {});
  bool setParent(uint32_t child, std::optional<uint32_t> parentIndex);
  [[nodiscard]] bool canSetParent(uint32_t child,
                                  std::optional<uint32_t> parentIndex) const;
  bool setParentPreserveWorldTransform(
      uint32_t child, std::optional<uint32_t> parentIndex);
  [[nodiscard]] std::optional<size_t> siblingIndex(uint32_t node) const;
  // Reorder within the existing parent (or root list), preserving node IDs.
  bool setSiblingIndex(uint32_t node, size_t index);
  // Fully stage the exact placement, forest, worlds and optional view. Null,
  // empty, repeated and retired active-root entries retain their existing view
  // meaning; the moved node and any destination parent must be live. Invalid
  // requests return an empty proof. Storage is bounded before copying; allocation
  // failures propagate without changing live state. Publication is single-use,
  // allocation-free and refuses stale or foreign proofs while retaining identity.
  [[nodiscard]] PreparedPlacement preparePlacement(uint32_t node,
      std::optional<uint32_t> parent, const glm::mat4& local, size_t siblingIndex,
      const std::optional<std::vector<uint32_t>>& targetActiveRoots,
      const ScenePlacementLimits& limits = {}) const;
  [[nodiscard]] bool canPublishPlacement(const PreparedPlacement&) const noexcept;
  bool publishPlacement(PreparedPlacement&) noexcept;
  void setLocalTransform(uint32_t nodeIndex, const glm::mat4& localTransform);
  void setRenderable(uint32_t nodeIndex, bool renderable);
  void setVisible(uint32_t nodeIndex, bool visible);
  void setSelectable(uint32_t nodeIndex, bool selectable);
  void setHoverable(uint32_t nodeIndex, bool hoverable);
  void setName(uint32_t nodeIndex, std::string name);
  void setMaterialIndex(uint32_t nodeIndex, uint32_t materialIndex);
  void updateWorldTransforms();
  // Optional active-scene view over the authoring graph. Source nodes, edits
  // and geometry identities survive scene switches; providers see only active
  // renderables. Ancestors of selected roots remain available for hierarchy UI.
  void setActiveRoots(std::span<const uint32_t> roots);
  void clearActiveRoots();
  [[nodiscard]] const std::optional<std::vector<uint32_t>>& activeRoots() const noexcept {
    return activeRoots_;
  }
  [[nodiscard]] bool isNodeActive(uint32_t nodeIndex) const;
  [[nodiscard]] bool isNodeInActiveHierarchy(uint32_t nodeIndex) const;
  [[nodiscard]] bool isNodeRetired(uint32_t nodeIndex) const noexcept;

  // Retirement retains every slot, child/root/renderable ordering and the exact
  // optional active view. Preparation validates the full forest in linear time,
  // enforces bounded detached storage and stages evaluated worlds/membership.
  // Tagged structural wrappers and live external mesh-owner references reject.
  // Physics bodies/products, joint/skin dependencies require caller validation.
  [[nodiscard]] PreparedRetirement prepareRetireSubtree(uint32_t root,
      const SceneRetirementLimits& limits = {}) const;
  // Replay uses the same graph/slot topology and exact expected retirement mask.
  // Optional finite local poses restore captured animation state off-line; the
  // caller publishes the corresponding source/player/light snapshot together.
  // Replay restores the deletion-time pose and leaves playback paused, rather
  // than claiming to preserve a later explicitly resumed animation time.
  [[nodiscard]] PreparedRetirement prepareRetirementChange(
      const SubtreeRetirement& receipt, bool retired,
      std::span<const SceneNodeLocalPose> capturedPose = {},
      const SceneRetirementLimits& limits = {}) const;
  bool publishRetirement(PreparedRetirement& prepared) noexcept;

  // Detach only appended nodes. Retained descendants, malformed topology and
  // empty/invalid ranges are refused without changing the graph. Restoration
  // requires the same graph and unchanged retained topology; transforms and
  // visibility may change while detached. Neither operation changes identity.
  [[nodiscard]] std::optional<DetachedTail> detachTail(uint32_t firstNode);
  bool restoreTail(const DetachedTail &tail);
  // The default detach view removes retired tail roots. The explicit overload
  // restores an exact pre-append view, including an absent all-active view or
  // an editor root refined into context during merge. Retained membership must
  // remain identical except for such nonrenderable, source-unmapped context.
  // Invalid topology/view requests return an empty proof; allocation failures
  // propagate before changing live state. Restoration permits retained payload
  // edits but requires the captured retained topology and detached view.
  // An optional pose restores the target history side's evaluated locals,
  // including manual graph-only edits. A nonempty pose must cover every
  // candidate slot in dense order (node == index), with finite matrices.
  // Validation and world recomputation complete before any live publication.
  [[nodiscard]] PreparedTailChange prepareDetachTail(uint32_t firstNode) const;
  [[nodiscard]] PreparedTailChange prepareDetachTail(uint32_t firstNode,
      const std::optional<std::vector<uint32_t>>& undoActiveRoots,
      std::span<const SceneNodeLocalPose> capturedPose = {}) const;
  [[nodiscard]] PreparedTailChange prepareRestoreTail(const DetachedTail& tail,
      std::span<const SceneNodeLocalPose> capturedPose = {}) const;
  bool publishTailChange(PreparedTailChange& prepared) noexcept;

  // Candidate must preserve every retained payload, root/renderable order and
  // active view, allowing only tail nodes and tail children on old parents.
  // An active nonrenderable/source-unmapped editor root may be replaced by its
  // ORIGINAL children in order (stable deduplicated). This narrow refinement
  // preserves old source/renderable membership while excluding new inactive
  // canonical content appended below that context-only editor root.
  // Preparation checks the complete forest in linear time. Publication swaps
  // staged storage without allocating, preserves this graph's identity and
  // refuses stale/foreign/consumed proofs. Staleness follows the mutation API's
  // revision contract; legacy raw getNode payload writes are not tracked.
  [[nodiscard]] PreparedAppend prepareAppend(SceneGraph candidate) const;
  bool publishAppend(PreparedAppend& prepared) noexcept;
  // Linear detached bulk assembly. Tail parents/children/mesh owners already
  // use final global IDs. New-parent children are complete; children attached
  // to old parents are appended in tail order. Clone the retained graph once,
  // reserve each destination once, preserve the private renderable/root order,
  // and validate the complete forest without per-node ancestry searches.
  // Does not alter worlds, active view or live state. Publish via prepareAppend.
  [[nodiscard]] SceneGraph makeAppendCandidate(std::vector<SceneNode> tail) const;
  // Append one authored empty node at nodeCount(), with identity local transform
  // and no source/geometry binding. A parent must belong to the active hierarchy
  // (active content or context), except a tagged, nonrenderable/source-unmapped
  // editor root without an import boundary may anchor an otherwise empty view.
  // A context child or a new scene root is added to
  // the exact explicit active view without activating its existing siblings;
  // an absent all-active view stays absent. Shared authoring name validation
  // applies before copying the graph. Retained payloads/world caches remain
  // exact; the new world is evaluated from its ancestors' local transforms.
  // Prepare/publish with prepareAppend, and undo with prepareDetachTail using
  // the captured old activeRoots() view. UI attachment policy is caller-owned.
  [[nodiscard]] SceneGraph makeEmptyNodeCandidate(
      std::optional<uint32_t> parent, std::string name = "Empty") const;

  [[nodiscard]] const std::vector<uint32_t>& rootNodes() const {
    return roots_;
  }
  [[nodiscard]] const std::vector<uint32_t>& renderableNodes() const {
    refreshActiveMembership();
    // Preserve the original all-active table when no filtering is needed.
    // Retired slots still require the cached filtered view, even without an
    // explicit scene selection. No additional scan or persistent flag needed.
    if (!activeRoots_ && activeRenderables_.size() == renderableNodes_.size())
      return renderableNodes_;
    return activeRenderables_;
  }
  [[nodiscard]] const SceneNode *getNode(uint32_t nodeIndex) const;
  // Mutable access supports legacy payload edits. Use the mutation functions
  // above for topology, labels, transforms, renderability and local flags so
  // revision-keyed consumers observe changes.
  [[nodiscard]] SceneNode *getNode(uint32_t nodeIndex);
  [[nodiscard]] size_t nodeCount() const { return nodes_.size(); }
  [[nodiscard]] uint64_t revision() const { return revision_; }
  // Identity changes when a fresh graph replaces one at the same address.
  // Hierarchy consumers can ignore transform and visibility-only changes.
  [[nodiscard]] uint64_t identity() const { return identity_; }
  // Original slot lifetime, stable through prepared placement/retirement and
  // restoration of the exact original detached tail. Copy/move graph identity
  // still separates foreign graphs. Raw SceneNode payload writes cannot forge
  // this witness. Zero means that the slot does not exist.
  [[nodiscard]] uint64_t nodeIncarnation(uint32_t node) const noexcept {
    return node < nodeIncarnations_.size() ? nodeIncarnations_[node] : 0;
  }
  [[nodiscard]] uint64_t hierarchyRevision() const {
    return hierarchyRevision_;
  }
  [[nodiscard]] bool isNodeEffectivelyVisible(uint32_t nodeIndex) const;
  // Resolve the nearest geometric hit through inherited opt-outs. The parent
  // of the highest blocking ancestor is the target, or kInvalidNode when no
  // authored/active parent remains. This does not trace through geometry and
  // does not combine selectability/hoverability with visibility.
  [[nodiscard]] uint32_t nearestSelectableAncestor(uint32_t nodeIndex) const;
  [[nodiscard]] uint32_t nearestHoverableAncestor(uint32_t nodeIndex) const;
  [[nodiscard]] bool isNodeEffectivelySelectable(uint32_t nodeIndex) const;
  [[nodiscard]] bool isNodeEffectivelyHoverable(uint32_t nodeIndex) const;

private:
  [[nodiscard]] bool isDescendant(uint32_t ancestor,
                                  uint32_t candidate) const;
  void updateWorldRecursive(uint32_t nodeIndex,
                            const glm::mat4& parentTransform);
  void registerRenderable(uint32_t nodeIndex);
  void unregisterRenderable(uint32_t nodeIndex);
  void refreshActiveMembership() const;
  [[nodiscard]] uint32_t nearestInteractionAncestor(
      uint32_t nodeIndex, bool SceneNode::*flag) const;

  std::vector<SceneNode> nodes_{};
  std::vector<uint64_t> nodeIncarnations_{};
  std::vector<uint32_t> roots_{};
  std::vector<uint32_t> renderableNodes_{};
  uint64_t revision_{0};
  uint64_t identity_{0};
  uint64_t hierarchyRevision_{0};
  std::optional<std::vector<uint32_t>> activeRoots_;
  mutable uint64_t activeMembershipRevision_{std::numeric_limits<uint64_t>::max()};
  mutable std::vector<uint8_t> activeMembership_;
  mutable std::vector<uint32_t> activeRenderables_;
};

}  // namespace container::scene
