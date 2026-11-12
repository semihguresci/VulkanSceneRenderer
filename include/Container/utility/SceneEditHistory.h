#pragma once

#include "Container/utility/EditHistory.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneNodeAuthoringPolicy.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat3x3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace container::util {

// Graph edits retain exact matrices, including imported shear. This object and
// its graph must outlive playback. All entry points synchronize graph identity
// so replacing a scene at the same address discards its previous commands.
class SceneEditHistory {
public:
  using NodeIndex = std::uint32_t;
  using Parent = std::optional<NodeIndex>;
  using MergeId = EditHistory::MergeId;

  struct Placement {
    Parent parent;
    glm::mat4 local;
    size_t siblingIndex;
  };
  struct ReparentActions {
    std::function<bool()> undo, redo;
  };
  using NodeFlagChanges = std::array<std::optional<bool>, 3>; // Visible, Selectable, Hoverable
  struct NodeFlagActions {
    std::function<void()> undo, redo;
  };
  struct MeshSelection {
    NodeIndex node;
    bool anotherTargetSelected;
  };

  struct Callbacks {
    // Validation runs before graph mutation, including during undo/redo.
    std::function<void(NodeIndex, const glm::mat4 &)> beforeTransform;
    std::function<void(NodeIndex, Parent, const glm::mat4 &)> beforeReparent;
    // Called after the edit and history cursor commit. A refresh exception can
    // propagate, but the completed edit remains available in history.
    std::function<void()> onChanged;
    // Read-only preparation constructs both graph/external-metadata actions
    // before mutation. Each action is invoked only after the ordinary placement
    // and validation guards and must publish completely or leave state unchanged.
    // Empty actions use the built-in graph path; providing only one is invalid.
    std::function<ReparentActions(NodeIndex, const Placement&, const Placement&)> prepareReparent;
    std::function<void(NodeIndex)> beforeNodeMetadata;
    // Prepare any matching source-property changes before applying the graph edit.
    // The returned actions publish atomically before the local graph setters.
    std::function<NodeFlagActions(NodeIndex, const NodeFlagChanges&)> prepareNodeFlags;
    std::function<MeshSelection()> meshSelection;
    // Selection publication must not throw. The normal scene refresh follows.
    std::function<void(NodeIndex)> setMeshSelection;
  };

  explicit SceneEditHistory(scene::SceneGraph &graph, Callbacks callbacks = {},
                            std::size_t capacity = 128)
      : graph_(graph), callbacks_(std::move(callbacks)), history_(capacity),
        graphIdentity_(graph.identity()) {}

  // Rename is a standalone metadata command. Validate only the new user entry;
  // imported labels (including empty ones) must round-trip exactly on undo.
  bool setName(NodeIndex node, std::string name) {
    sync();
    const auto* current = graph_.getNode(node);
    if (!current || current->retired) return false;
    scene::ValidateSceneNodeName(name);
    if (current->name == name) return false;
    const auto incarnation = graph_.nodeIncarnation(node);
    const std::string before = current->name;
    EditHistory::Command command{
        "Rename",
        [this, node, incarnation, before] { return applyName(node, incarnation, before); },
        [this, node, incarnation, name] { return applyName(node, incarnation, name); }};
    if (!command.redo()) return false;
    finishGesture();
    history_.record(std::move(command));
    changed();
    return true;
  }

  bool setTransform(NodeIndex node, const glm::mat4 &transform,
                    std::optional<MergeId> mergeId = std::nullopt) {
    sync();
    const auto *current = graph_.getNode(node);
    if (!current || current->retired || current->localTransform == transform || !finite(transform))
      return false;
    const auto incarnation = graph_.nodeIncarnation(node);
    const auto* start = transformGestureStart(node, mergeId);
    const glm::mat4 before = start ? *start : current->localTransform;
    EditHistory::Command command{
        "Transform",
        [this, node, incarnation, before] { return applyTransform(node, incarnation, before); },
        [this, node, incarnation, transform] { return applyTransform(node, incarnation, transform); }};
    if (!command.redo())
      return false;
    if (!start) finishGesture();
    if (mergeId) {
      transformGesture_ = TransformGesture{node, incarnation, *mergeId, before,
                                            transform, std::move(command)};
    } else {
      history_.record(std::move(command));
    }
    changed();
    return true;
  }

  // A live preview publishes normally while leaving the journal untouched.
  // Use the exact starting matrix as the basis of subsequent widget updates.
  [[nodiscard]] const glm::mat4* transformGestureStart(
      NodeIndex node, std::optional<MergeId> id) const noexcept {
    if (!transformGesture_ || transformGesture_->id != id ||
        transformGesture_->node != node ||
        !matchesNode(node, transformGesture_->incarnation)) return nullptr;
    return &transformGesture_->before;
  }

  bool cancelTransformGesture() {
    sync();
    if (!transformGesture_) return false;
    const auto& gesture = *transformGesture_;
    const bool restored = gesture.before != gesture.after && gesture.command.undo();
    transformGesture_.reset();
    if (restored) changed();
    return restored;
  }

  bool setVisible(NodeIndex node, bool visible) {
    return setFlags(node, {visible, std::nullopt, std::nullopt}, "Visibility");
  }

  bool setSelectable(NodeIndex node, bool selectable) {
    return setFlags(node, {std::nullopt, selectable, std::nullopt}, "Selectability");
  }

  bool setHoverable(NodeIndex node, bool hoverable) {
    return setFlags(node, {std::nullopt, std::nullopt, hoverable}, "Hoverability");
  }

  bool setNodeFlags(NodeIndex node, bool visible, bool selectable, bool hoverable) {
    return setFlags(node, {visible, selectable, hoverable}, "Node flags");
  }

  // Preserve the current world matrix when choosing a new parent. Playback
  // restores the stored local matrix directly, without another inverse or TRS
  // decomposition, so repeated undo/redo cannot accumulate reparenting drift.
  bool reparent(NodeIndex node, Parent parent) {
    sync();
    const auto *current = graph_.getNode(node);
    if (!current || !graph_.canSetParent(node, parent) ||
        current->parent == parent.value_or(scene::SceneGraph::kInvalidNode))
      return false;
    const Placement before{parentOf(*current), current->localTransform,
                           *graph_.siblingIndex(node)};
    const auto world = worldTransform(node);
    const auto parentWorld = parent ? worldTransform(*parent)
                                    : std::optional<glm::mat4>(glm::mat4(1.0f));
    if (!world || !parentWorld || !invertible(*parentWorld))
      return false;
    const Placement after{parent, glm::inverse(*parentWorld) * *world,
                          parent ? graph_.getNode(*parent)->children.size()
                                 : graph_.rootNodes().size()};
    if (!finite(after.local))
      return false;
    const ReparentTargets targets{node, graph_.nodeIncarnation(node), before.parent, after.parent,
        before.parent ? graph_.nodeIncarnation(*before.parent) : 0,
        after.parent ? graph_.nodeIncarnation(*after.parent) : 0};
    auto actions = callbacks_.prepareReparent ? callbacks_.prepareReparent(node, before, after) : ReparentActions{};
    if (bool(actions.undo) != bool(actions.redo))
      throw std::invalid_argument("Prepared reparent requires both undo and redo actions");
    EditHistory::Command command;
    command.label = "Reparent";
    if (actions.undo) {
      command.undo = [this, targets, before, action = std::move(actions.undo)] {
        return applyPreparedPlacement(targets, before, action);
      };
      command.redo = [this, targets, after, action = std::move(actions.redo)] {
        return applyPreparedPlacement(targets, after, action);
      };
    } else {
      command.undo = [this, targets, before] { return applyPlacement(targets, before); };
      command.redo = [this, targets, after] { return applyPlacement(targets, after); };
    }
    trackMeshSelection(command);
    if (!command.redo())
      return false;
    finishGesture();
    history_.record(std::move(command));
    changed();
    return true;
  }

  // For append/remove commands whose initial graph edit was already applied.
  // Their callbacks only perform the edit; successful playback invokes the same
  // world-transform and renderer refresh path as the built-in commands.
  bool recordApplied(EditHistory::Command command,
                     std::optional<MergeId> mergeId = std::nullopt) {
    sync();
    if (transformGesture_) finishGesture();
    const bool published = command.selfContainedPublication;
    const bool recorded = history_.record(std::move(command), mergeId);
    if (!published) changed();
    return recorded;
  }

  // Construct all command captures before publishing a generation. The caller
  // and both playback callbacks stage and publish the complete graph/renderer
  // state themselves; recording and successful playback perform no redundant
  // world evaluation or onChanged callback after that publication.
  bool recordAppliedPublished(EditHistory::Command command) {
    command.selfContainedPublication = true;
    return recordApplied(std::move(command));
  }

  bool undo() {
    finishGesture();
    const bool published = history_.undoIsSelfContainedPublication();
    if (!history_.undo())
      return false;
    if (!published) changed();
    return true;
  }

  bool redo() {
    if (transformGesture_) finishGesture();
    else sync();
    const bool published = history_.redoIsSelfContainedPublication();
    if (!history_.redo())
      return false;
    if (!published) changed();
    return true;
  }

  [[nodiscard]] bool canUndo() {
    sync();
    return history_.canUndo();
  }
  [[nodiscard]] bool canRedo() {
    sync();
    return history_.canRedo();
  }
  [[nodiscard]] std::string_view undoLabel() {
    sync();
    return history_.undoLabel();
  }
  [[nodiscard]] std::string_view redoLabel() {
    sync();
    return history_.redoLabel();
  }
  [[nodiscard]] std::size_t size() {
    sync();
    return history_.size();
  }
  [[nodiscard]] std::size_t undoCount() {
    sync();
    return history_.undoCount();
  }
  [[nodiscard]] std::size_t redoCount() {
    sync();
    return history_.redoCount();
  }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return history_.capacity();
  }
  [[nodiscard]] bool transformGestureActive() const noexcept {
    return transformGesture_.has_value() && graphIdentity_ == graph_.identity();
  }

  void finishGesture() noexcept {
    sync();
    history_.finishGesture();
    if (!transformGesture_) return;
    auto gesture = std::move(*transformGesture_);
    transformGesture_.reset();
    if (gesture.before != gesture.after && matchesNode(gesture.node, gesture.incarnation))
      history_.record(std::move(gesture.command));
  }

  void clear() noexcept {
    history_.clear();
    graphIdentity_ = graph_.identity();
    transformGesture_.reset();
  }

  // Returns true when an externally replaced graph invalidated the history.
  bool sync() noexcept {
    if (graphIdentity_ == graph_.identity())
      return false;
    clear();
    return true;
  }

private:
  struct TransformGesture {
    NodeIndex node;
    std::uint64_t incarnation;
    MergeId id;
    glm::mat4 before, after;
    EditHistory::Command command;
  };

  static Parent parentOf(const scene::SceneNode &node) {
    return node.parent == scene::SceneGraph::kInvalidNode ? std::nullopt
                                                          : Parent(node.parent);
  }

  static bool finite(const glm::mat4 &matrix) {
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 4; ++row)
        if (!std::isfinite(matrix[column][row]))
          return false;
    return true;
  }

  // Match SceneGraph's preserve-world rejection policy for singular parents.
  static bool invertible(const glm::mat4 &matrix) {
    if (!finite(matrix))
      return false;
    float maxColumnLength = 0.0f;
    for (int column = 0; column < 3; ++column)
      maxColumnLength =
          std::max(maxColumnLength, glm::length(glm::vec3(matrix[column])));
    const float determinant = glm::determinant(glm::mat3(matrix));
    const float threshold = std::numeric_limits<float>::epsilon() *
                            maxColumnLength * maxColumnLength *
                            maxColumnLength * 64.0f;
    return maxColumnLength > 0.0f && std::isfinite(determinant) &&
           std::abs(determinant) > threshold;
  }

  // Evaluate from local ancestry, including stale world caches, without
  // mutating even cached graph state before a validation callback can reject.
  std::optional<glm::mat4> worldTransform(NodeIndex node) const {
    std::vector<NodeIndex> ancestry;
    while (node != scene::SceneGraph::kInvalidNode) {
      const auto *current = graph_.getNode(node);
      if (!current || current->retired || ancestry.size() == graph_.nodeCount())
        return std::nullopt;
      ancestry.push_back(node);
      node = current->parent;
    }
    glm::mat4 world(1.0f);
    for (auto ancestor = ancestry.rbegin(); ancestor != ancestry.rend();
         ++ancestor)
      world *= graph_.getNode(*ancestor)->localTransform;
    return finite(world) ? std::optional<glm::mat4>(world) : std::nullopt;
  }

  [[nodiscard]] bool matchesNode(NodeIndex node, std::uint64_t incarnation) const noexcept {
    return graphIdentity_ == graph_.identity() && incarnation &&
           graph_.nodeIncarnation(node) == incarnation && !graph_.isNodeRetired(node);
  }

  bool applyTransform(NodeIndex node, std::uint64_t incarnation, const glm::mat4 &transform) {
    if (!matchesNode(node, incarnation))
      return false;
    if (callbacks_.beforeTransform)
      callbacks_.beforeTransform(node, transform);
    if (!matchesNode(node, incarnation))
      return false;
    graph_.setLocalTransform(node, transform);
    return true;
  }

  bool applyName(NodeIndex node, std::uint64_t incarnation, const std::string& name) {
    if (!matchesNode(node, incarnation)) return false;
    if (callbacks_.beforeNodeMetadata) callbacks_.beforeNodeMetadata(node);
    if (!matchesNode(node, incarnation)) return false;
    // SceneGraph takes the copy before replacing the current string. No new
    // name validation applies here: undo restores exact imported metadata.
    graph_.setName(node, name);
    return true;
  }

  bool applyFlags(NodeIndex node, std::uint64_t incarnation, const NodeFlagChanges& flags,
                  const std::function<void()>& applySource) {
    if (!matchesNode(node, incarnation)) return false;
    if (callbacks_.beforeNodeMetadata) callbacks_.beforeNodeMetadata(node);
    if (!matchesNode(node, incarnation)) return false;
    if (applySource) applySource();
    if (flags[0]) graph_.setVisible(node, *flags[0]);
    if (flags[1]) graph_.setSelectable(node, *flags[1]);
    if (flags[2]) graph_.setHoverable(node, *flags[2]);
    return true;
  }

  bool setFlags(NodeIndex node, const NodeFlagChanges& requested, const char* label) {
    sync();
    const auto* current = graph_.getNode(node);
    if (!current || current->retired) return false;
    const std::array local{current->visible, current->selectable, current->hoverable};
    NodeFlagChanges before, after;
    for (size_t i = 0; i < requested.size(); ++i) {
      if (!requested[i] || *requested[i] == local[i]) continue;
      before[i] = local[i];
      after[i] = requested[i];
    }
    if (before == after) return false;
    const auto incarnation = graph_.nodeIncarnation(node);
    auto source = callbacks_.prepareNodeFlags ? callbacks_.prepareNodeFlags(node, after) : NodeFlagActions{};
    if (bool(source.undo) != bool(source.redo))
      throw std::logic_error("Incomplete source node flag history actions");
    EditHistory::Command command{
        label,
        [this, node, incarnation, before, apply = std::move(source.undo)] {
          return applyFlags(node, incarnation, before, apply);
        },
        [this, node, incarnation, after, apply = std::move(source.redo)] {
          return applyFlags(node, incarnation, after, apply);
        }};
    if (after[1]) trackMeshSelection(command);
    if (!command.redo()) return false;
    finishGesture();
    history_.record(std::move(command));
    changed();
    return true;
  }

  struct DisplacedMeshSelection {
    NodeIndex node{scene::SceneGraph::kInvalidNode};
    uint64_t incarnation{};
  };

  void trackMeshSelection(EditHistory::Command& command) {
    if (!callbacks_.meshSelection || !callbacks_.setMeshSelection) return;
    auto displaced = std::make_shared<DisplacedMeshSelection>();
    const auto wrap = [this, displaced](std::function<bool()> apply) {
      return [this, displaced, apply = std::move(apply)] {
        constexpr auto none = scene::SceneGraph::kInvalidNode;
        const auto current = callbacks_.meshSelection();
        const bool wasSelectable = graph_.isNodeEffectivelySelectable(current.node);
        const auto incarnation = graph_.nodeIncarnation(current.node);
        if (current.node == none && displaced->node != none)
          (void)graph_.isNodeActive(displaced->node);
        if (!apply()) return false;
        const auto previous = std::exchange(*displaced, {});
        // Capture only the selection this direction of the edit invalidates.
        // Inverse playback can recover it while the selection remains vacant.
        if (wasSelectable && !graph_.isNodeEffectivelySelectable(current.node)) {
          *displaced = {current.node, incarnation};
          callbacks_.setMeshSelection(none);
        } else if (current.node == none && !current.anotherTargetSelected && previous.node != none &&
                   graph_.nodeIncarnation(previous.node) == previous.incarnation &&
                   graph_.isNodeEffectivelySelectable(previous.node)) {
          callbacks_.setMeshSelection(previous.node);
        }
        return true;
      };
    };
    command.undo = wrap(std::move(command.undo));
    command.redo = wrap(std::move(command.redo));
  }

  struct ReparentTargets {
    NodeIndex node;
    std::uint64_t incarnation;
    Parent oldParent, newParent;
    std::uint64_t oldParentIncarnation, newParentIncarnation;
  };

  [[nodiscard]] bool matchesReparentTargets(const ReparentTargets& targets) const noexcept {
    return matchesNode(targets.node, targets.incarnation) &&
        (!targets.oldParent || matchesNode(*targets.oldParent, targets.oldParentIncarnation)) &&
        (!targets.newParent || matchesNode(*targets.newParent, targets.newParentIncarnation));
  }

  bool applyPlacement(const ReparentTargets& targets, const Placement &placement) {
    const auto node = targets.node;
    if (!matchesReparentTargets(targets) || !graph_.canSetParent(node, placement.parent))
      return false;
    const auto targetParent =
        placement.parent.value_or(scene::SceneGraph::kInvalidNode);
    const auto targetSize =
        placement.parent ? graph_.getNode(*placement.parent)->children.size()
                         : graph_.rootNodes().size();
    const bool changingParent = graph_.getNode(node)->parent != targetParent;
    if (placement.siblingIndex >= targetSize + (changingParent ? 1u : 0u))
      return false;
    if (callbacks_.beforeReparent)
      callbacks_.beforeReparent(node, placement.parent, placement.local);
    if (!matchesReparentTargets(targets) ||
        !graph_.canSetParent(node, placement.parent))
      return false;
    if (callbacks_.meshSelection && callbacks_.setMeshSelection) {
      // A prepared placement also has its new active membership ready for
      // selection recovery, so playback needs no allocation after publication.
      auto prepared = graph_.preparePlacement(node, placement.parent, placement.local,
          placement.siblingIndex, graph_.activeRoots());
      return prepared && graph_.publishPlacement(prepared);
    }
    if (graph_.getNode(node)->parent !=
            placement.parent.value_or(scene::SceneGraph::kInvalidNode) &&
        !graph_.setParent(node, placement.parent))
      return false;
    graph_.setLocalTransform(node, placement.local);
    graph_.setSiblingIndex(node, placement.siblingIndex);
    return true;
  }

  bool applyPreparedPlacement(const ReparentTargets& targets, const Placement& placement,
                              const std::function<bool()>& action) {
    const auto node = targets.node;
    const auto valid = [&] {
      if (!matchesReparentTargets(targets) || !finite(placement.local) ||
          !graph_.canSetParent(node, placement.parent)) return false;
      const auto target = placement.parent.value_or(scene::SceneGraph::kInvalidNode);
      const auto count = placement.parent ? graph_.getNode(*placement.parent)->children.size()
                                          : graph_.rootNodes().size();
      return placement.siblingIndex < count + (graph_.getNode(node)->parent != target ? 1u : 0u);
    };
    if (!valid()) return false;
    if (callbacks_.beforeReparent)
      callbacks_.beforeReparent(node, placement.parent, placement.local);
    if (!valid()) return false;
    return action();
  }

  void changed() {
    graph_.updateWorldTransforms();
    if (callbacks_.onChanged)
      callbacks_.onChanged();
  }

  scene::SceneGraph &graph_;
  Callbacks callbacks_;
  EditHistory history_;
  std::uint64_t graphIdentity_;
  std::optional<TransformGesture> transformGesture_;
};

} // namespace container::util
