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
    const std::string before = current->name;
    EditHistory::Command command{
        "Rename",
        [this, node, before] { return applyName(node, before); },
        [this, node, name] { return applyName(node, name); }};
    if (!applyName(node, name)) return false;
    history_.record(std::move(command));
    transformMergeNode_.reset();
    changed();
    return true;
  }

  bool setTransform(NodeIndex node, const glm::mat4 &transform,
                    std::optional<MergeId> mergeId = std::nullopt) {
    sync();
    const auto *current = graph_.getNode(node);
    if (!current || current->retired || current->localTransform == transform || !finite(transform))
      return false;
    const glm::mat4 before = current->localTransform;
    EditHistory::Command command{
        "Transform",
        [this, node, before] { return applyTransform(node, before); },
        [this, node, transform] { return applyTransform(node, transform); }};
    if (!applyTransform(node, transform))
      return false;
    // Even an accidentally reused widget ID must not merge different nodes.
    if (!mergeId || transformMergeNode_ != node)
      history_.finishGesture();
    history_.record(std::move(command), mergeId);
    transformMergeNode_ =
        mergeId ? std::optional<NodeIndex>(node) : std::nullopt;
    changed();
    return true;
  }

  bool setVisible(NodeIndex node, bool visible) {
    sync();
    const auto *current = graph_.getNode(node);
    if (!current || current->retired || current->visible == visible)
      return false;
    const bool before = current->visible;
    EditHistory::Command command{
        "Visibility",
        [this, node, before] { return applyVisible(node, before); },
        [this, node, visible] { return applyVisible(node, visible); }};
    if (!applyVisible(node, visible))
      return false;
    history_.record(std::move(command));
    transformMergeNode_.reset();
    changed();
    return true;
  }

  bool setSelectable(NodeIndex node, bool selectable) {
    return setInteractionFlag(node, selectable, false);
  }

  bool setHoverable(NodeIndex node, bool hoverable) {
    return setInteractionFlag(node, hoverable, true);
  }

  bool setNodeFlags(NodeIndex node, bool visible, bool selectable, bool hoverable) {
    sync();
    const auto* current = graph_.getNode(node);
    if (!current || current->retired) return false;
    const std::array before{current->visible, current->selectable, current->hoverable};
    const std::array after{visible, selectable, hoverable};
    if (before == after) return false;
    EditHistory::Command command{
        "Node flags",
        [this, node, before] { return applyNodeFlags(node, before); },
        [this, node, after] { return applyNodeFlags(node, after); }};
    if (!applyNodeFlags(node, after)) return false;
    history_.record(std::move(command));
    transformMergeNode_.reset();
    changed();
    return true;
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
    auto actions = callbacks_.prepareReparent ? callbacks_.prepareReparent(node, before, after) : ReparentActions{};
    if (bool(actions.undo) != bool(actions.redo))
      throw std::invalid_argument("Prepared reparent requires both undo and redo actions");
    EditHistory::Command command;
    command.label = "Reparent";
    if (actions.undo) {
      command.undo = [this, node, before, action = std::move(actions.undo)] {
        return applyPreparedPlacement(node, before, action);
      };
      command.redo = [this, node, after, action = std::move(actions.redo)] {
        return applyPreparedPlacement(node, after, action);
      };
    } else {
      command.undo = [this, node, before] { return applyPlacement(node, before); };
      command.redo = [this, node, after] { return applyPlacement(node, after); };
    }
    if (!command.redo())
      return false;
    history_.record(std::move(command));
    transformMergeNode_.reset();
    changed();
    return true;
  }

  // For append/remove commands whose initial graph edit was already applied.
  // Their callbacks only perform the edit; successful playback invokes the same
  // world-transform and renderer refresh path as the built-in commands.
  bool recordApplied(EditHistory::Command command,
                     std::optional<MergeId> mergeId = std::nullopt) {
    sync();
    if (transformMergeNode_)
      history_.finishGesture();
    const bool published = command.selfContainedPublication;
    const bool recorded = history_.record(std::move(command), mergeId);
    transformMergeNode_.reset();
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
    sync();
    const bool published = history_.undoIsSelfContainedPublication();
    if (!history_.undo())
      return false;
    transformMergeNode_.reset();
    if (!published) changed();
    return true;
  }

  bool redo() {
    sync();
    const bool published = history_.redoIsSelfContainedPublication();
    if (!history_.redo())
      return false;
    transformMergeNode_.reset();
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

  void finishGesture() noexcept {
    sync();
    history_.finishGesture();
    transformMergeNode_.reset();
  }

  void clear() noexcept {
    history_.clear();
    graphIdentity_ = graph_.identity();
    transformMergeNode_.reset();
  }

  // Returns true when an externally replaced graph invalidated the history.
  bool sync() noexcept {
    if (graphIdentity_ == graph_.identity())
      return false;
    clear();
    return true;
  }

private:
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

  bool applyTransform(NodeIndex node, const glm::mat4 &transform) {
    if (!graph_.getNode(node) || graph_.isNodeRetired(node))
      return false;
    if (callbacks_.beforeTransform)
      callbacks_.beforeTransform(node, transform);
    if (graphIdentity_ != graph_.identity() || !graph_.getNode(node) || graph_.isNodeRetired(node))
      return false;
    graph_.setLocalTransform(node, transform);
    return true;
  }

  bool applyName(NodeIndex node, const std::string& name) {
    if (graphIdentity_ != graph_.identity() || !graph_.getNode(node) || graph_.isNodeRetired(node))
      return false;
    // SceneGraph takes the copy before replacing the current string. No new
    // name validation applies here: undo restores exact imported metadata.
    graph_.setName(node, name);
    return true;
  }

  bool applyVisible(NodeIndex node, bool visible) {
    if (!graph_.getNode(node) || graph_.isNodeRetired(node))
      return false;
    graph_.setVisible(node, visible);
    return true;
  }

  bool applyInteractionFlag(NodeIndex node, bool value, bool hover) {
    if (!graph_.getNode(node) || graph_.isNodeRetired(node)) return false;
    if (hover) graph_.setHoverable(node, value);
    else graph_.setSelectable(node, value);
    return true;
  }

  bool applyNodeFlags(NodeIndex node, const std::array<bool, 3>& flags) {
    if (!graph_.getNode(node) || graph_.isNodeRetired(node)) return false;
    graph_.setVisible(node, flags[0]);
    graph_.setSelectable(node, flags[1]);
    graph_.setHoverable(node, flags[2]);
    return true;
  }

  bool setInteractionFlag(NodeIndex node, bool value, bool hover) {
    sync();
    const auto* current = graph_.getNode(node);
    if (!current || current->retired) return false;
    const bool before = hover ? current->hoverable : current->selectable;
    if (before == value) return false;
    EditHistory::Command command{
        hover ? "Hoverability" : "Selectability",
        [this, node, before, hover] { return applyInteractionFlag(node, before, hover); },
        [this, node, value, hover] { return applyInteractionFlag(node, value, hover); }};
    if (!applyInteractionFlag(node, value, hover)) return false;
    history_.record(std::move(command));
    transformMergeNode_.reset();
    changed();
    return true;
  }

  bool applyPlacement(NodeIndex node, const Placement &placement) {
    if (!graph_.canSetParent(node, placement.parent))
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
    if (graphIdentity_ != graph_.identity() ||
        !graph_.canSetParent(node, placement.parent))
      return false;
    if (graph_.getNode(node)->parent !=
            placement.parent.value_or(scene::SceneGraph::kInvalidNode) &&
        !graph_.setParent(node, placement.parent))
      return false;
    graph_.setLocalTransform(node, placement.local);
    graph_.setSiblingIndex(node, placement.siblingIndex);
    return true;
  }

  bool applyPreparedPlacement(NodeIndex node, const Placement& placement,
                              const std::function<bool()>& action) {
    const auto valid = [&] {
      if (graphIdentity_ != graph_.identity() || !finite(placement.local) ||
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
  std::optional<NodeIndex> transformMergeNode_;
};

} // namespace container::util
