#include "Container/utility/SceneEditHistory.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using container::scene::SceneGraph;
using History = container::util::SceneEditHistory;

glm::mat4 translated(float x, float y = 0.0f, float z = 0.0f) {
  return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
}

glm::mat4 sheared() {
  glm::mat4 matrix = translated(1.25f, -2.0f, 3.5f);
  matrix[0][0] = 1.75f;
  matrix[1][0] = 0.375f;
  matrix[2][1] = -0.625f;
  matrix[2][2] = 0.75f;
  return matrix;
}

void expectNear(const glm::mat4 &actual, const glm::mat4 &expected) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      EXPECT_NEAR(actual[column][row], expected[column][row], 1e-5f);
}

TEST(SceneEditHistory, NoOpAndInvalidEditsDoNotCreateHistoryOrNotify) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1.0f), 0);
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  const auto revision = graph.revision();

  EXPECT_FALSE(history.setTransform(node, glm::mat4(1.0f)));
  EXPECT_FALSE(history.setVisible(node, true));
  EXPECT_FALSE(history.reparent(node, std::nullopt));
  EXPECT_FALSE(history.setTransform(99, translated(2.0f)));
  EXPECT_FALSE(history.setVisible(99, false));
  EXPECT_FALSE(history.setSelectable(node, true));
  EXPECT_FALSE(history.setHoverable(node, true));
  EXPECT_FALSE(history.setSelectable(99, false));
  EXPECT_FALSE(history.setHoverable(99, false));
  EXPECT_FALSE(history.reparent(99, node));
  auto invalid = glm::mat4(1.0f);
  invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(history.setTransform(node, invalid));
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(notifications, 0);
  EXPECT_EQ(history.size(), 0u);
}

TEST(SceneEditHistory, RenameRoundTripsImportedEmptyNameAndPreservesSourcePayload) {
  SceneGraph graph;
  const auto parent = graph.createNode(translated(2), 0);
  const auto node = graph.createNode(sheared(), 9, false, SceneGraph::kInvalidNode, "");
  ASSERT_TRUE(graph.setParent(node, parent));
  graph.getNode(node)->sourceNodeIndex = 7;
  graph.setSelectable(node, false);
  graph.setHoverable(node, false);
  const auto child = graph.createNode(translated(3), 4, true, 8, "Primitive child");
  ASSERT_TRUE(graph.setParent(child, node));
  graph.getNode(child)->meshOwnerGraphNode = node;
  graph.getNode(child)->sourceMeshIndex = 3;
  graph.getNode(child)->sourcePrimitiveIndex = 1;
  graph.getNode(child)->sourceInstanceIndex = 2;
  graph.updateWorldTransforms();
  const auto before = *graph.getNode(node), childBefore = *graph.getNode(child);
  int notifications = 0;
  History history(graph, {[](auto, const auto&) { throw std::runtime_error("unexpected physics transform check"); },
      [](auto, auto, const auto&) { throw std::runtime_error("unexpected physics reparent check"); },
      [&] { ++notifications; }});
  const std::string name = "  Renam\xC3\xA9 \xF0\x9F\x8C\x8D  ";
  auto after = before; after.name = name;
  ASSERT_TRUE(history.setName(node, name));
  EXPECT_EQ(history.undoLabel(), "Rename"); EXPECT_EQ(history.size(), 1u);
  EXPECT_EQ(*graph.getNode(node), after); EXPECT_EQ(*graph.getNode(child), childBefore);
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(*graph.getNode(node), before); EXPECT_EQ(*graph.getNode(child), childBefore);
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(*graph.getNode(node), after); EXPECT_EQ(*graph.getNode(child), childBefore);
  }
  EXPECT_EQ(notifications, 7);
}

TEST(SceneEditHistory, RenameSeparatesTransformGesturesAndRepeatedNameEdits) {
  SceneGraph graph;
  const auto node = graph.createNode(sheared(), 0, false, SceneGraph::kInvalidNode, "Imported");
  History history(graph);
  ASSERT_TRUE(history.setTransform(node, translated(1), 42));
  ASSERT_TRUE(history.setTransform(node, translated(2), 42));
  ASSERT_TRUE(history.setName(node, "First rename"));
  ASSERT_TRUE(history.setTransform(node, translated(3), 42));
  ASSERT_TRUE(history.setName(node, "Second rename"));
  EXPECT_EQ(history.size(), 4u);
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->name, "First rename");
  EXPECT_EQ(graph.getNode(node)->localTransform, translated(3));
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->localTransform, translated(2));
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->name, "Imported");
  EXPECT_EQ(graph.getNode(node)->localTransform, translated(2));
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->localTransform, sheared());
  ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.getNode(node)->localTransform, translated(2));
  ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.getNode(node)->name, "First rename");
  ASSERT_TRUE(history.redo()); ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(node)->name, "Second rename");
  EXPECT_EQ(graph.getNode(node)->localTransform, translated(3));
}

TEST(SceneEditHistory, InvalidRenameAndNoOpsPreserveRedoAndTheCurrentTransformGesture) {
  SceneGraph graph;
  const auto node = graph.createNode(sheared(), 0, false, SceneGraph::kInvalidNode, "Original");
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.setTransform(node, translated(1), 19));
  const std::array malformed{std::string{}, std::string(" \t\n"), std::string("bad\0name", 8),
      std::string("bad\x01name", 8), std::string("\xC0\xAF"), std::string(4097, 'x')};
  const auto expectInvalidUnchanged = [&] {
    for (const auto& name : malformed) {
      const auto revision = graph.revision();
      const auto count = notifications;
      const auto cursor = history.undoCount(), redo = history.redoCount();
      EXPECT_THROW(history.setName(node, name), std::invalid_argument);
      EXPECT_EQ(graph.getNode(node)->name, "Original"); EXPECT_EQ(graph.revision(), revision);
      EXPECT_EQ(notifications, count); EXPECT_EQ(history.undoCount(), cursor); EXPECT_EQ(history.redoCount(), redo);
    }
  };
  expectInvalidUnchanged();
  EXPECT_FALSE(history.setName(node, "Original"));
  EXPECT_FALSE(history.setName(999, "Missing node"));
  ASSERT_TRUE(history.setTransform(node, translated(2), 19));
  EXPECT_EQ(history.size(), 1u); // Rejected metadata did not split the gesture.
  ASSERT_TRUE(history.undo()); EXPECT_EQ(history.redoCount(), 1u);
  expectInvalidUnchanged();
  ASSERT_TRUE(history.setName(node, "New branch"));
  EXPECT_FALSE(history.canRedo()); EXPECT_EQ(history.size(), 1u);
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->name, "Original");
  EXPECT_EQ(graph.getNode(node)->localTransform, sheared());
}

TEST(SceneEditHistory, RenameCommandsCannotReplayIntoAReplacementGraphAtTheSameIndex) {
  SceneGraph graph;
  const auto node = graph.createNode(translated(1), 0, false, SceneGraph::kInvalidNode, "Old source");
  History history(graph);
  ASSERT_TRUE(history.setName(node, "Old renamed")); ASSERT_TRUE(history.undo());
  SceneGraph replacement;
  ASSERT_EQ(replacement.createNode(sheared(), 0, false, SceneGraph::kInvalidNode, "New source"), node);
  graph = std::move(replacement);
  EXPECT_FALSE(history.redo()); EXPECT_EQ(history.size(), 0u);
  EXPECT_EQ(graph.getNode(node)->name, "New source"); EXPECT_EQ(graph.getNode(node)->localTransform, sheared());
  ASSERT_TRUE(history.setName(node, "New renamed"));
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->name, "New source");
}

TEST(SceneEditHistory, MissingRenameNodeLeavesPlaybackCursorAndNotificationsUnchanged) {
  SceneGraph graph;
  graph.createNode(translated(1), 0);
  const auto node = graph.createNode(sheared(), 0, false, SceneGraph::kInvalidNode, "Imported tail");
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.setName(node, "Renamed tail"));
  const auto tail = graph.detachTail(node); ASSERT_TRUE(tail);
  const auto detachedRevision = graph.revision();
  EXPECT_FALSE(history.undo()); EXPECT_EQ(history.undoCount(), 1u); EXPECT_EQ(history.redoCount(), 0u);
  EXPECT_EQ(notifications, 1); EXPECT_EQ(graph.revision(), detachedRevision);
  ASSERT_TRUE(graph.restoreTail(*tail));
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->name, "Imported tail");
  const auto beforeRedo = graph.detachTail(node); ASSERT_TRUE(beforeRedo);
  EXPECT_FALSE(history.redo()); EXPECT_EQ(history.redoCount(), 1u); EXPECT_EQ(notifications, 2);
  ASSERT_TRUE(graph.restoreTail(*beforeRedo));
  ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.getNode(node)->name, "Renamed tail");
}

TEST(SceneEditHistory, RenameRefreshExceptionRetainsTheCompletedNameAndHistoryCursor) {
  SceneGraph graph;
  const auto node = graph.createNode(sheared(), 0);
  bool fail = true;
  History history(graph, {{}, {}, [&] { if (fail) throw std::runtime_error("refresh failed"); }});
  EXPECT_THROW(history.setName(node, "Committed rename"), std::runtime_error);
  EXPECT_EQ(graph.getNode(node)->name, "Committed rename"); EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_THROW(history.undo(), std::runtime_error);
  EXPECT_TRUE(graph.getNode(node)->name.empty()); EXPECT_EQ(history.redoCount(), 1u);
  fail = false;
  ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.getNode(node)->name, "Committed rename");
  EXPECT_EQ(graph.getNode(node)->localTransform, sheared());
}

TEST(SceneEditHistory, InteractionFlagsUndoIndependentlyWithoutChangingVisibility) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4(1), 0);
  const auto child = graph.createNode(glm::mat4(1), 0, true);
  ASSERT_TRUE(graph.setParent(child, parent));
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.setSelectable(parent, false));
  ASSERT_TRUE(history.setHoverable(child, false));
  EXPECT_FALSE(graph.isNodeEffectivelySelectable(child));
  EXPECT_FALSE(graph.isNodeEffectivelyHoverable(child));
  EXPECT_TRUE(graph.isNodeEffectivelyVisible(child));
  EXPECT_TRUE(graph.getNode(child)->selectable);
  EXPECT_TRUE(graph.getNode(parent)->hoverable);
  EXPECT_EQ(history.undoLabel(), "Hoverability");
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.isNodeEffectivelyHoverable(child));
  EXPECT_FALSE(graph.isNodeEffectivelySelectable(child));
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.isNodeEffectivelySelectable(child));
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.redo());
  EXPECT_FALSE(graph.isNodeEffectivelySelectable(child));
  EXPECT_FALSE(graph.isNodeEffectivelyHoverable(child));
  EXPECT_EQ(notifications, 6);
}

TEST(SceneEditHistory, GroupedNodeFlagsAreOneUndoStepAndRestoreAllLocals) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1), 0);
  History history(graph);
  EXPECT_FALSE(history.setNodeFlags(node, true, true, true));
  EXPECT_FALSE(history.setNodeFlags(999, false, false, false));
  ASSERT_TRUE(history.setNodeFlags(node, false, false, false));
  EXPECT_EQ(history.size(), 1u);
  EXPECT_FALSE(graph.getNode(node)->visible);
  EXPECT_FALSE(graph.getNode(node)->selectable);
  EXPECT_FALSE(graph.getNode(node)->hoverable);
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.getNode(node)->visible);
  EXPECT_TRUE(graph.getNode(node)->selectable);
  EXPECT_TRUE(graph.getNode(node)->hoverable);
  ASSERT_TRUE(history.redo());
  EXPECT_FALSE(graph.getNode(node)->visible);
  EXPECT_FALSE(graph.getNode(node)->selectable);
  EXPECT_FALSE(graph.getNode(node)->hoverable);
}

TEST(SceneEditHistory, TransformPlaybackRetainsExactImportedShear) {
  SceneGraph graph;
  const glm::mat4 before = sheared();
  const glm::mat4 after = translated(4.0f) * before;
  const auto node = graph.createNode(before, 0, true);
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});

  ASSERT_TRUE(history.setTransform(node, after));
  EXPECT_EQ(graph.getNode(node)->localTransform, after);
  EXPECT_EQ(graph.getNode(node)->worldTransform, after);
  EXPECT_EQ(history.undoLabel(), "Transform");
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(graph.getNode(node)->localTransform, before);
    EXPECT_EQ(graph.getNode(node)->worldTransform, before);
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(graph.getNode(node)->localTransform, after);
  }
  EXPECT_EQ(notifications, 7);
}

TEST(SceneEditHistory, VisibilityPreservesChildFlagsAndInheritedVisibility) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4(1.0f), 0);
  const auto child = graph.createNode(glm::mat4(1.0f), 0, true);
  ASSERT_TRUE(graph.setParent(child, parent));
  History history(graph);

  ASSERT_TRUE(history.setVisible(parent, false));
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(child));
  EXPECT_TRUE(graph.getNode(child)->visible);
  ASSERT_TRUE(history.setVisible(child, false));
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.getNode(child)->visible);
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(child));
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.isNodeEffectivelyVisible(child));
  ASSERT_TRUE(history.redo());
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(child));
}

TEST(SceneEditHistory, ReparentPreservesWorldAndRestoresExactStoredPlacements) {
  SceneGraph graph;
  auto firstLocal =
      glm::rotate(translated(3.0f, 4.0f), 0.4f, glm::vec3(0.0f, 1.0f, 0.0f));
  firstLocal = glm::scale(firstLocal, glm::vec3(2.0f, 1.0f, 0.5f));
  const auto first = graph.createNode(firstLocal, 0);
  const auto second = graph.createNode(
      glm::rotate(translated(-3.0f, 2.0f), -0.2f, glm::vec3(0.0f, 0.0f, 1.0f)),
      0);
  const glm::mat4 originalLocal = sheared();
  const auto child = graph.createNode(originalLocal, 0, true);
  ASSERT_TRUE(graph.setParent(child, first));
  graph.updateWorldTransforms();
  const glm::mat4 originalWorld = graph.getNode(child)->worldTransform;
  History history(graph);

  ASSERT_TRUE(history.reparent(child, second));
  const glm::mat4 reparentedLocal = graph.getNode(child)->localTransform;
  EXPECT_EQ(graph.getNode(child)->parent, second);
  expectNear(graph.getNode(child)->worldTransform, originalWorld);
  EXPECT_EQ(history.undoLabel(), "Reparent");
  for (int cycle = 0; cycle < 3; ++cycle) {
    ASSERT_TRUE(history.undo());
    EXPECT_EQ(graph.getNode(child)->parent, first);
    EXPECT_EQ(graph.getNode(child)->localTransform, originalLocal);
    EXPECT_EQ(graph.getNode(child)->worldTransform, originalWorld);
    ASSERT_TRUE(history.redo());
    EXPECT_EQ(graph.getNode(child)->parent, second);
    EXPECT_EQ(graph.getNode(child)->localTransform, reparentedLocal);
    expectNear(graph.getNode(child)->worldTransform, originalWorld);
  }
}

TEST(SceneEditHistory, ReparentToRootUsesCurrentLocalsWithStaleWorldCaches) {
  SceneGraph graph;
  const auto parent = graph.createNode(translated(2.0f), 0);
  const auto child = graph.createNode(sheared(), 0);
  ASSERT_TRUE(graph.setParent(child, parent));
  graph.updateWorldTransforms();
  graph.setLocalTransform(parent, translated(7.0f));
  const auto expectedWorld = translated(7.0f) * sheared();
  History history(graph);

  ASSERT_TRUE(history.reparent(child, std::nullopt));
  EXPECT_EQ(graph.getNode(child)->parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.getNode(child)->localTransform, expectedWorld);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(child)->parent, parent);
  EXPECT_EQ(graph.getNode(child)->localTransform, sheared());
}

TEST(SceneEditHistory, InvalidReparentNeverInvokesValidationOrChangesGraph) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4(1.0f), 0);
  const auto child = graph.createNode(sheared(), 0);
  const auto singular = graph.createNode(
      glm::scale(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 1.0f)), 0);
  ASSERT_TRUE(graph.setParent(child, parent));
  int validations = 0;
  History history(graph,
                  {{}, [&](auto, auto, const auto &) { ++validations; }, {}});
  const auto revision = graph.revision();
  const auto beforeLocal = graph.getNode(child)->localTransform;
  const auto beforeWorld = graph.getNode(child)->worldTransform;

  EXPECT_FALSE(history.reparent(parent, child));
  EXPECT_FALSE(history.reparent(child, child));
  EXPECT_FALSE(history.reparent(child, 99));
  EXPECT_FALSE(history.reparent(child, singular));
  EXPECT_EQ(validations, 0);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(child)->parent, parent);
  EXPECT_EQ(graph.getNode(child)->localTransform, beforeLocal);
  EXPECT_EQ(graph.getNode(child)->worldTransform, beforeWorld);
  EXPECT_EQ(history.size(), 0u);
}

TEST(SceneEditHistory, TransformValidationRejectsInitialAndPlaybackAtomically) {
  SceneGraph graph;
  const auto node = graph.createNode(sheared(), 0);
  bool reject = true;
  int notifications = 0;
  glm::mat4 proposed(1.0f);
  History history(graph, {[&](auto target, const auto &transform) {
                            EXPECT_EQ(target, node);
                            proposed = transform;
                            if (reject)
                              throw std::runtime_error("physics rejected");
                          },
                          {},
                          [&] { ++notifications; }});
  const auto beforeRevision = graph.revision();
  const auto after = translated(5.0f) * sheared();

  EXPECT_THROW(history.setTransform(node, after), std::runtime_error);
  EXPECT_EQ(proposed, after);
  EXPECT_EQ(graph.getNode(node)->localTransform, sheared());
  EXPECT_EQ(graph.revision(), beforeRevision);
  EXPECT_EQ(history.size(), 0u);
  EXPECT_EQ(notifications, 0);
  reject = false;
  ASSERT_TRUE(history.setTransform(node, after));
  reject = true;
  const auto appliedRevision = graph.revision();
  EXPECT_THROW(history.undo(), std::runtime_error);
  EXPECT_EQ(proposed, sheared());
  EXPECT_EQ(graph.getNode(node)->localTransform, after);
  EXPECT_EQ(graph.revision(), appliedRevision);
  EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_EQ(notifications, 1);
  reject = false;
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(node)->localTransform, sheared());
}

TEST(SceneEditHistory, ReparentValidationSeesExactProposalBeforeAnyMutation) {
  SceneGraph graph;
  const auto first = graph.createNode(translated(3.0f), 0);
  const auto second = graph.createNode(translated(8.0f), 0);
  const auto child = graph.createNode(sheared(), 0);
  ASSERT_TRUE(graph.setParent(child, first));
  const auto originalWorldCache = graph.getNode(child)->worldTransform;
  bool reject = true;
  glm::mat4 proposal(1.0f);
  std::optional<uint32_t> target;
  History history(graph, {{},
                          [&](auto node, auto parent, const auto &local) {
                            EXPECT_EQ(node, child);
                            target = parent;
                            proposal = local;
                            if (reject)
                              throw std::runtime_error("physics rejected");
                          },
                          {}});
  const auto revision = graph.revision();

  EXPECT_THROW(history.reparent(child, second), std::runtime_error);
  EXPECT_EQ(target, second);
  EXPECT_EQ(proposal, translated(-5.0f) * sheared());
  EXPECT_EQ(graph.getNode(child)->parent, first);
  EXPECT_EQ(graph.getNode(child)->localTransform, sheared());
  EXPECT_EQ(graph.getNode(child)->worldTransform, originalWorldCache);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(history.size(), 0u);
  reject = false;
  ASSERT_TRUE(history.reparent(child, second));
  const auto after = graph.getNode(child)->localTransform;
  reject = true;
  EXPECT_THROW(history.undo(), std::runtime_error);
  EXPECT_EQ(target, first);
  EXPECT_EQ(proposal, sheared());
  EXPECT_EQ(graph.getNode(child)->parent, second);
  EXPECT_EQ(graph.getNode(child)->localTransform, after);
  EXPECT_EQ(history.undoCount(), 1u);
  reject = false;
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(child)->parent, first);
  EXPECT_EQ(graph.getNode(child)->localTransform, sheared());
}

TEST(SceneEditHistory, SceneReplacementClearsHistoryBeforeQueriesOrPlayback) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1.0f), 0);
  History history(graph);
  ASSERT_TRUE(history.setTransform(node, translated(3.0f)));
  ASSERT_TRUE(history.undo());
  SceneGraph replacement;
  replacement.createNode(sheared(), 0);
  graph = std::move(replacement);

  EXPECT_FALSE(history.canRedo());
  EXPECT_FALSE(history.canUndo());
  EXPECT_EQ(history.size(), 0u);
  EXPECT_TRUE(history.undoLabel().empty());
  EXPECT_FALSE(history.redo());
  EXPECT_EQ(graph.getNode(0)->localTransform, sheared());
  ASSERT_TRUE(history.setVisible(0, false));
  graph = SceneGraph{};
  EXPECT_TRUE(history.sync());
  EXPECT_FALSE(history.sync());
  EXPECT_EQ(history.size(), 0u);
}

TEST(SceneEditHistory, MixedCommandsUndoAndRedoInOrder) {
  SceneGraph graph;
  const auto parent = graph.createNode(translated(10.0f), 0);
  const auto child = graph.createNode(sheared(), 0, true);
  History history(graph);
  const auto transformed = translated(3.0f) * sheared();
  ASSERT_TRUE(history.setTransform(child, transformed));
  ASSERT_TRUE(history.setVisible(child, false));
  ASSERT_TRUE(history.reparent(child, parent));
  const auto reparented = graph.getNode(child)->localTransform;
  EXPECT_EQ(history.size(), 3u);

  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(child)->parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.getNode(child)->localTransform, transformed);
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.getNode(child)->visible);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(child)->localTransform, sheared());
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(child)->parent, parent);
  EXPECT_EQ(graph.getNode(child)->localTransform, reparented);
  EXPECT_FALSE(graph.getNode(child)->visible);
}

TEST(SceneEditHistory, GesturesMergePerNodeUntilFinished) {
  SceneGraph graph;
  const auto first = graph.createNode(sheared(), 0);
  const auto second = graph.createNode(glm::mat4(1.0f), 0);
  History history(graph);
  ASSERT_TRUE(history.setTransform(first, translated(1.0f), 42));
  ASSERT_TRUE(history.setTransform(first, translated(2.0f), 42));
  EXPECT_EQ(history.size(), 1u);
  history.finishGesture();
  ASSERT_TRUE(history.setTransform(first, translated(3.0f), 42));
  ASSERT_TRUE(history.setTransform(second, translated(4.0f), 42));
  EXPECT_EQ(history.size(), 3u);
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(second)->localTransform, glm::mat4(1.0f));
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(first)->localTransform, translated(2.0f));
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(first)->localTransform, sheared());
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(first)->localTransform, translated(2.0f));
}

TEST(SceneEditHistory,
     RecordAppliedUsesCommonRefreshForBothPlaybackDirections) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1.0f), 0);
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  graph.setLocalTransform(node, translated(5.0f));
  ASSERT_TRUE(
      history.recordApplied({"External edit",
                             [&] {
                               graph.setLocalTransform(node, glm::mat4(1.0f));
                               return true;
                             },
                             [&] {
                               graph.setLocalTransform(node, translated(5.0f));
                               return true;
                             }}));
  EXPECT_EQ(graph.getNode(node)->worldTransform, translated(5.0f));
  EXPECT_EQ(history.undoLabel(), "External edit");
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(node)->worldTransform, glm::mat4(1.0f));
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(node)->worldTransform, translated(5.0f));
  EXPECT_EQ(notifications, 3);
}

TEST(SceneEditHistory, RefreshExceptionLeavesCompletedEditAndCursorConsistent) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1.0f), 0);
  bool rejectRefresh = true;
  History history(graph, {{}, {}, [&] {
                            if (rejectRefresh)
                              throw std::runtime_error("refresh failed");
                          }});
  EXPECT_THROW(history.setTransform(node, translated(3.0f)),
               std::runtime_error);
  EXPECT_EQ(graph.getNode(node)->localTransform, translated(3.0f));
  EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_THROW(history.undo(), std::runtime_error);
  EXPECT_EQ(graph.getNode(node)->localTransform, glm::mat4(1.0f));
  EXPECT_EQ(history.redoCount(), 1u);
  rejectRefresh = false;
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(node)->localTransform, translated(3.0f));
}
TEST(SceneEditHistory, ReparentRestoresRootOrder) {
  SceneGraph graph;
  const auto a = graph.createNode(glm::mat4(1), 0);
  const auto b = graph.createNode(glm::mat4(1), 0);
  const auto c = graph.createNode(glm::mat4(1), 0);
  const auto roots = graph.rootNodes();
  History history(graph);
  ASSERT_TRUE(history.reparent(b, a));
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.rootNodes(), roots);
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.rootNodes(), (std::vector<uint32_t>{a, c}));
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.rootNodes(), roots);
}

TEST(SceneEditHistory, InterleavedReparentAndPrimitiveRedoPreservesTopology) {
  SceneGraph graph;
  const auto p = graph.createNode(glm::mat4(1), 0);
  const auto q = graph.createNode(glm::mat4(1), 0);
  const auto x = graph.createNode(glm::mat4(1), 0);
  const auto y = graph.createNode(glm::mat4(1), 0);
  ASSERT_TRUE(graph.setParent(x, p));
  ASSERT_TRUE(graph.setParent(y, q));
  History history(graph);
  ASSERT_TRUE(history.reparent(x, q));
  const auto first = static_cast<uint32_t>(graph.nodeCount());
  const auto primitive = graph.createNode(glm::mat4(1), 0, true, 1);
  ASSERT_TRUE(graph.setParent(primitive, p));
  std::optional<SceneGraph::DetachedTail> detached;
  ASSERT_TRUE(history.recordApplied(
      {"Add primitive",
       [&] {
         detached = graph.detachTail(first);
         return detached.has_value();
       },
       [&] { return detached && graph.restoreTail(*detached); }}));
  ASSERT_TRUE(history.reparent(y, p));
  ASSERT_TRUE(history.undo());
  EXPECT_EQ(graph.getNode(q)->children, (std::vector<uint32_t>{y, x}));
  ASSERT_TRUE(history.undo());
  ASSERT_TRUE(history.undo());
  ASSERT_TRUE(history.redo());
  ASSERT_TRUE(history.redo());
  EXPECT_NE(graph.getNode(primitive), nullptr);
  ASSERT_TRUE(history.redo());
  EXPECT_EQ(graph.getNode(q)->children, (std::vector<uint32_t>{x}));
  EXPECT_EQ(graph.getNode(p)->children, (std::vector<uint32_t>{primitive, y}));
}

TEST(SceneEditHistory, PreparedAppendPreservesOldVisibilityUndoAndRedoOnTheLiveGraph) {
  SceneGraph graph;
  const auto root = graph.createNode(translated(10), 0);
  const auto retained = graph.createNode(translated(2), 0, true, 1);
  ASSERT_TRUE(graph.setParent(retained, root));
  History history(graph);
  ASSERT_TRUE(history.setVisible(root, false));
  const auto identity = graph.identity();
  SceneGraph candidate(graph);
  const auto imported = candidate.createNode(translated(3), 0, true, 2);
  ASSERT_TRUE(candidate.setParent(imported, root));
  candidate.updateWorldTransforms();
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_FALSE(history.sync());
  EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(imported));
  ASSERT_TRUE(history.undo());
  EXPECT_TRUE(graph.isNodeEffectivelyVisible(retained));
  EXPECT_TRUE(graph.isNodeEffectivelyVisible(imported));
  EXPECT_EQ(graph.nodeCount(), 3u);
  EXPECT_EQ(graph.getNode(root)->children, (std::vector<uint32_t>{retained, imported}));
  ASSERT_TRUE(history.redo());
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(imported));
  EXPECT_EQ(graph.getNode(imported)->worldTransform, translated(13));
}
TEST(SceneEditHistory, PublishedCommandsSkipRefreshAfterCompletePreparedGraphPublication) {
  SceneGraph graph;
  const auto root = graph.createNode(translated(2), 0);
  History history(graph, {{}, {}, [] { throw std::runtime_error("unexpected post-publication refresh"); }});
  std::optional<SceneGraph::DetachedTail> tail;
  // Both callback captures exist before the initial graph generation changes.
  container::util::EditHistory::Command command{"Merge",
    [&] {
      auto change = graph.prepareDetachTail(1);
      if (!graph.publishTailChange(change)) return false;
      tail = change.takeDetachedTail(); return tail.has_value();
    },
    [&] {
      if (!tail) return false;
      auto change = graph.prepareRestoreTail(*tail);
      return graph.publishTailChange(change);
    }};
  SceneGraph candidate(graph);
  const auto child = candidate.createNode(translated(3), 0, true, 7);
  ASSERT_TRUE(candidate.setParent(child, root)); candidate.updateWorldTransforms();
  auto initial = graph.prepareAppend(std::move(candidate)); ASSERT_TRUE(graph.publishAppend(initial));
  EXPECT_NO_THROW(EXPECT_TRUE(history.recordAppliedPublished(std::move(command))));
  EXPECT_EQ(graph.getNode(child)->worldTransform, translated(5));
  EXPECT_NO_THROW(EXPECT_TRUE(history.undo())); EXPECT_EQ(graph.nodeCount(), 1u);
  EXPECT_NO_THROW(EXPECT_TRUE(history.redo())); EXPECT_EQ(graph.nodeCount(), 2u);
  EXPECT_EQ(graph.getNode(child)->worldTransform, translated(5));
  EXPECT_EQ(history.undoCount(), 1u);
}

TEST(SceneEditHistory, PublishedPlaybackFailuresKeepCursorStateAndDoNotNotify) {
  SceneGraph graph; const auto node = graph.createNode(translated(1), 0);
  int notifications = 0, state = 1, failure = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.recordAppliedPublished({"Generation",
    [&] {
      if (failure == 1) return false;
      if (failure == 2) throw std::runtime_error("undo stage failure");
      state = 0; return true;
    },
    [&] {
      if (failure == 1) return false;
      if (failure == 2) throw std::runtime_error("redo stage failure");
      state = 1; return true;
    }}));
  // A fully published generation owns its evaluated cache; this sentinel also
  // proves SceneEditHistory does not perform another world traversal.
  graph.getNode(node)->worldTransform = translated(41);
  failure = 1; EXPECT_FALSE(history.undo()); EXPECT_EQ(history.undoCount(), 1u);
  failure = 2; EXPECT_THROW(history.undo(), std::runtime_error); EXPECT_EQ(history.undoCount(), 1u);
  EXPECT_EQ(state, 1); EXPECT_EQ(graph.getNode(node)->worldTransform, translated(41));
  failure = 0; ASSERT_TRUE(history.undo()); EXPECT_EQ(state, 0);
  failure = 1; EXPECT_FALSE(history.redo()); EXPECT_EQ(history.redoCount(), 1u);
  failure = 2; EXPECT_THROW(history.redo(), std::runtime_error); EXPECT_EQ(history.redoCount(), 1u);
  failure = 0; ASSERT_TRUE(history.redo()); EXPECT_EQ(state, 1);
  EXPECT_EQ(notifications, 0); EXPECT_EQ(graph.getNode(node)->worldTransform, translated(41));
}

TEST(SceneEditHistory, MixedOrdinaryAndPublishedCommandsNotifyOnlyOrdinaryChanges) {
  SceneGraph graph; const auto node = graph.createNode(translated(1), 0);
  int notifications = 0, generation = 1;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.setVisible(node, false)); EXPECT_EQ(notifications, 1);
  ASSERT_TRUE(history.recordAppliedPublished({"Generation",
    [&] { generation = 0; return true; }, [&] { generation = 1; return true; }}));
  EXPECT_EQ(notifications, 1);
  ASSERT_TRUE(history.undo()); EXPECT_EQ(generation, 0); EXPECT_EQ(notifications, 1);
  ASSERT_TRUE(history.undo()); EXPECT_TRUE(graph.getNode(node)->visible); EXPECT_EQ(notifications, 2);
  ASSERT_TRUE(history.redo()); EXPECT_FALSE(graph.getNode(node)->visible); EXPECT_EQ(notifications, 3);
  ASSERT_TRUE(history.redo()); EXPECT_EQ(generation, 1); EXPECT_EQ(notifications, 3);
  ASSERT_TRUE(history.setVisible(node, true)); EXPECT_EQ(notifications, 4);
  ASSERT_TRUE(history.undo()); EXPECT_EQ(notifications, 5);
  ASSERT_TRUE(history.undo()); EXPECT_EQ(generation, 0); EXPECT_EQ(notifications, 5);
}

TEST(SceneEditHistory, PublishedPolicyHandlesDisabledHistoryAndCannotCoalesceWithOrdinaryCommands) {
  SceneGraph graph; int notifications = 0;
  History disabled(graph, {{}, {}, [&] { ++notifications; }}, 0);
  EXPECT_FALSE(disabled.recordAppliedPublished({"Generation", [] { return true; }, [] { return true; }}));
  EXPECT_EQ(notifications, 0); EXPECT_FALSE(disabled.undo());
  container::util::EditHistory raw;
  EXPECT_FALSE(raw.undoIsSelfContainedPublication()); EXPECT_FALSE(raw.redoIsSelfContainedPublication());
  ASSERT_TRUE(raw.record({"Ordinary", [] { return true; }, [] { return true; }}, 17));
  ASSERT_TRUE(raw.record({"Published", [] { return true; }, [] { return true; }, true}, 17));
  EXPECT_EQ(raw.size(), 2u); EXPECT_TRUE(raw.undoIsSelfContainedPublication());
  ASSERT_TRUE(raw.undo()); EXPECT_FALSE(raw.undoIsSelfContainedPublication());
  EXPECT_TRUE(raw.redoIsSelfContainedPublication());
  ASSERT_TRUE(raw.undo()); EXPECT_FALSE(raw.undoIsSelfContainedPublication());
  EXPECT_FALSE(raw.redoIsSelfContainedPublication());
}

TEST(SceneEditHistory, RetiredSlotsRejectNewEditsAndStalePlaybackPreservesCursorAndCallbacks) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1), 0, true, 0, "original");
  const auto parent = graph.createNode(glm::mat4(1), 0, false);
  int notifications = 0, transforms = 0;
  History history(graph, {[&](auto, const auto&) { ++transforms; }, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.setName(node, "edited"));
  auto prepared = graph.prepareRetireSubtree(node); ASSERT_TRUE(graph.publishRetirement(prepared));
  auto receipt = prepared.takeRetirement(); ASSERT_TRUE(receipt);
  const auto before = *graph.getNode(node); const auto revision = graph.revision();
  EXPECT_FALSE(history.setName(node, "new")); EXPECT_FALSE(history.setTransform(node, translated(4)));
  EXPECT_FALSE(history.setVisible(node, false)); EXPECT_FALSE(history.setSelectable(node, false));
  EXPECT_FALSE(history.setHoverable(node, false)); EXPECT_FALSE(history.setNodeFlags(node, false, false, false));
  EXPECT_FALSE(history.reparent(node, parent)); EXPECT_FALSE(history.undo());
  EXPECT_EQ(history.undoCount(), 1u); EXPECT_EQ(history.redoCount(), 0u);
  EXPECT_EQ(notifications, 1); EXPECT_EQ(transforms, 0);
  EXPECT_EQ(*graph.getNode(node), before); EXPECT_EQ(graph.revision(), revision);
  auto restore = graph.prepareRetirementChange(*receipt, false); ASSERT_TRUE(graph.publishRetirement(restore));
  ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.getNode(node)->name, "original");
  ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.getNode(node)->name, "edited");
}

TEST(SceneEditHistory, PublishedRetirementCommandKeepsOldHistoryAndStableIdentity) {
  SceneGraph graph; const auto node = graph.createNode(glm::mat4(1), 0, true, 0, "old");
  int notifications = 0;
  History history(graph, {{}, {}, [&] { ++notifications; }});
  ASSERT_TRUE(history.setVisible(node, false));
  const auto identity = graph.identity();
  auto prepared = graph.prepareRetireSubtree(node); ASSERT_TRUE(graph.publishRetirement(prepared));
  auto receipt = std::make_shared<SceneGraph::SubtreeRetirement>(std::move(*prepared.takeRetirement()));
  ASSERT_TRUE(history.recordAppliedPublished({"Delete subtree",
      [&] { auto change = graph.prepareRetirementChange(*receipt, false); return graph.publishRetirement(change); },
      [&] { auto change = graph.prepareRetirementChange(*receipt, true); return graph.publishRetirement(change); }}));
  EXPECT_EQ(notifications, 1); EXPECT_TRUE(graph.isNodeRetired(node));
  ASSERT_TRUE(history.undo()); EXPECT_FALSE(graph.isNodeRetired(node));
  EXPECT_FALSE(graph.getNode(node)->visible); EXPECT_EQ(notifications, 1);
  ASSERT_TRUE(history.undo()); EXPECT_TRUE(graph.getNode(node)->visible);
  ASSERT_TRUE(history.redo()); EXPECT_FALSE(graph.getNode(node)->visible);
  ASSERT_TRUE(history.redo()); EXPECT_TRUE(graph.isNodeRetired(node));
  EXPECT_EQ(notifications, 3); EXPECT_EQ(graph.identity(), identity);
}

// A second owner has exact metadata beyond the pure graph. Its actions stage
// both payloads before publication, as the frontend's scene-root receipt does.
struct PreparedReparentWitness {
  enum class Hook { Full, Empty, Half, Throw };
  SceneGraph graph;
  uint32_t editor{}, parent{}, extra{}, child{};
  std::vector<uint32_t> metadata;
  std::unique_ptr<History> history;
  Hook hook{Hook::Full};
  bool rejectValidation{}, rejectRefresh{};
  int rejectAction{}, preparations{}, validations{}, publications{}, notifications{};
  std::optional<History::Placement> before, after;
  PreparedReparentWitness() {
    editor = graph.createNode(glm::mat4(1), 0, false, SceneGraph::kInvalidNode, "Editor");
    graph.getNode(editor)->editorSceneRoot = true;
    parent = graph.createNode(translated(3), 0, false, SceneGraph::kInvalidNode, "Source parent");
    extra = graph.createNode(glm::mat4(1), 0, false, SceneGraph::kInvalidNode, "Earlier explicit root");
    child = graph.createNode(sheared(), 0, false, SceneGraph::kInvalidNode, "Inherited empty");
    graph.setParent(parent, editor); graph.setParent(extra, editor); graph.setParent(child, parent);
    graph.setActiveRoots(std::array{parent, extra}); graph.updateWorldTransforms(); metadata = {extra};
    History::Callbacks callbacks;
    callbacks.beforeReparent = [this](auto, auto, const auto&) {
      ++validations;
      if (rejectValidation) throw std::runtime_error("placement validation rejected");
    };
    callbacks.onChanged = [this] {
      ++notifications;
      if (rejectRefresh) throw std::runtime_error("refresh failed after cursor commit");
    };
    callbacks.prepareReparent = [this](auto node, const auto& oldPlacement, const auto& newPlacement) -> History::ReparentActions {
      ++preparations; before = oldPlacement; after = newPlacement;
      if (hook == Hook::Throw) throw std::runtime_error("metadata preparation rejected");
      if (hook == Hook::Empty) return {};
      if (hook == Hook::Half) return {[this] { ++publications; return true; }, {}};
      const auto oldMetadata = metadata;
      auto newMetadata = oldMetadata; newMetadata.push_back(node);
      const auto oldView = graph.activeRoots(); auto newView = oldView;
      if (newView) newView->push_back(node);
      const auto publish = [this, node](const History::Placement& placement,
          const auto& expected, const auto& desired, const auto& view) {
        if (rejectAction == 1) return false;
        if (rejectAction == 2) throw std::runtime_error("metadata staging failed");
        if (metadata != expected) return false;
        auto stagedMetadata = desired;
        auto proof = graph.preparePlacement(node, placement.parent, placement.local, placement.siblingIndex, view);
        if (!proof || !graph.publishPlacement(proof)) return false;
        metadata.swap(stagedMetadata); ++publications;
        return true;
      };
      return {
          [=] { return publish(oldPlacement, newMetadata, oldMetadata, oldView); },
          [=] { return publish(newPlacement, oldMetadata, newMetadata, newView); }};
    };
    history = std::make_unique<History>(graph, std::move(callbacks));
  }
};

TEST(SceneEditHistory, PreparedReparentRestoresOwnedMetadataBeforeUndoingInheritedEmptyCreation) {
  PreparedReparentWitness f;
  const auto oldView = f.graph.activeRoots();
  const auto oldMetadata = f.metadata;
  const auto originalLocal = f.graph.getNode(f.child)->localTransform;
  const auto originalWorld = f.graph.getNode(f.child)->worldTransform;
  const auto identity = f.graph.identity();
  std::optional<SceneGraph::DetachedTail> tail;
  ASSERT_TRUE(f.history->recordApplied({"Create inherited empty",
      [&] {
        // The strict creation receipt never owned an explicit entry for this
        // child. A reparent command must remove its own entry before this undo.
        if (f.metadata != oldMetadata) return false;
        auto proof = f.graph.prepareDetachTail(f.child, oldView);
        if (!f.graph.publishTailChange(proof)) return false;
        tail = proof.takeDetachedTail(); return tail.has_value();
      },
      [&] { return tail && f.graph.restoreTail(*tail); }}));
  ASSERT_TRUE(f.history->reparent(f.child, f.editor));
  EXPECT_EQ(f.metadata, (std::vector<uint32_t>{f.extra, f.child}));
  EXPECT_EQ(f.graph.activeRoots(), (std::optional(std::vector<uint32_t>{f.parent, f.extra, f.child})));
  ASSERT_TRUE(f.before); ASSERT_TRUE(f.after);
  EXPECT_EQ(f.before->parent, std::optional(f.parent)); EXPECT_EQ(f.before->local, originalLocal);
  EXPECT_EQ(f.after->parent, std::optional(f.editor)); EXPECT_EQ(f.after->local, originalWorld);
  ASSERT_TRUE(f.history->undo()); EXPECT_EQ(f.metadata, oldMetadata); EXPECT_EQ(f.graph.activeRoots(), oldView);
  EXPECT_EQ(f.graph.getNode(f.child)->localTransform, originalLocal);
  ASSERT_TRUE(f.history->undo()); EXPECT_EQ(f.graph.nodeCount(), f.child); EXPECT_EQ(f.metadata, oldMetadata);
  ASSERT_TRUE(f.history->redo()); ASSERT_TRUE(f.history->redo());
  EXPECT_EQ(f.metadata, (std::vector<uint32_t>{f.extra, f.child}));
  EXPECT_EQ(f.graph.getNode(f.child)->worldTransform, originalWorld); EXPECT_EQ(f.graph.identity(), identity);
  EXPECT_EQ(f.preparations, 1); EXPECT_EQ(f.validations, 3); EXPECT_EQ(f.publications, 3);
  EXPECT_EQ(f.history->undoCount(), 2u);
}

TEST(SceneEditHistory, ReparentHookPreparationAndActionFailuresPreserveRedoAndEmptyHookFallsBack) {
  PreparedReparentWitness f;
  ASSERT_TRUE(f.history->setTransform(f.child, translated(8), 42)); ASSERT_TRUE(f.history->undo());
  const auto node = *f.graph.getNode(f.child); const auto view = f.graph.activeRoots();
  const auto metadata = f.metadata, roots = f.graph.rootNodes(); const auto revision = f.graph.revision();
  f.hook = PreparedReparentWitness::Hook::Throw;
  EXPECT_THROW(f.history->reparent(f.child, f.editor), std::runtime_error);
  f.hook = PreparedReparentWitness::Hook::Half;
  EXPECT_THROW(f.history->reparent(f.child, f.editor), std::invalid_argument);
  f.hook = PreparedReparentWitness::Hook::Full; f.rejectAction = 1;
  EXPECT_FALSE(f.history->reparent(f.child, f.editor));
  f.rejectAction = 2; EXPECT_THROW(f.history->reparent(f.child, f.editor), std::runtime_error);
  EXPECT_EQ(*f.graph.getNode(f.child), node); EXPECT_EQ(f.graph.activeRoots(), view); EXPECT_EQ(f.metadata, metadata);
  EXPECT_EQ(f.graph.rootNodes(), roots); EXPECT_EQ(f.graph.revision(), revision);
  EXPECT_EQ(f.history->undoCount(), 0u); EXPECT_EQ(f.history->redoCount(), 1u); EXPECT_EQ(f.publications, 0);
  f.hook = PreparedReparentWitness::Hook::Empty; f.rejectAction = 0;
  ASSERT_TRUE(f.history->reparent(f.child, f.editor));
  EXPECT_EQ(f.graph.getNode(f.child)->parent, f.editor); EXPECT_EQ(f.metadata, metadata);
  EXPECT_EQ(f.graph.activeRoots(), view); EXPECT_EQ(f.publications, 0); // Built-in, manager-free placement.
  ASSERT_TRUE(f.history->undo()); EXPECT_EQ(*f.graph.getNode(f.child), node);
}

TEST(SceneEditHistory, PreparedReparentValidatesBeforeEveryActionAndFailedReplayKeepsCursorAndSidecar) {
  PreparedReparentWitness f; f.rejectValidation = true;
  EXPECT_THROW(f.history->reparent(f.child, f.editor), std::runtime_error);
  EXPECT_EQ(f.publications, 0); EXPECT_EQ(f.history->size(), 0u);
  f.rejectValidation = false; ASSERT_TRUE(f.history->reparent(f.child, f.editor));
  const auto applied = *f.graph.getNode(f.child); const auto metadata = f.metadata;
  f.rejectValidation = true; EXPECT_THROW(f.history->undo(), std::runtime_error);
  f.rejectValidation = false; f.rejectAction = 1; EXPECT_FALSE(f.history->undo());
  f.rejectAction = 2; EXPECT_THROW(f.history->undo(), std::runtime_error);
  EXPECT_EQ(f.publications, 1); EXPECT_EQ(*f.graph.getNode(f.child), applied); EXPECT_EQ(f.metadata, metadata);
  EXPECT_EQ(f.history->undoCount(), 1u); EXPECT_EQ(f.history->redoCount(), 0u);
  f.rejectAction = 0; ASSERT_TRUE(f.history->undo());
  f.metadata.push_back(f.editor); const auto edited = f.metadata; const auto before = *f.graph.getNode(f.child);
  EXPECT_FALSE(f.history->redo()); EXPECT_EQ(f.metadata, edited); EXPECT_EQ(*f.graph.getNode(f.child), before);
  EXPECT_EQ(f.history->redoCount(), 1u); f.metadata.pop_back();
  auto retire = f.graph.prepareRetireSubtree(f.child); ASSERT_TRUE(f.graph.publishRetirement(retire));
  auto receipt = retire.takeRetirement(); ASSERT_TRUE(receipt); const auto validations = f.validations;
  EXPECT_FALSE(f.history->redo()); EXPECT_EQ(f.validations, validations); EXPECT_EQ(f.publications, 2);
  auto restore = f.graph.prepareRetirementChange(*receipt, false); ASSERT_TRUE(f.graph.publishRetirement(restore));
  ASSERT_TRUE(f.history->redo()); EXPECT_EQ(f.publications, 3);
}

TEST(SceneEditHistory, PreparedReparentRefreshFailureKeepsCommittedGraphMetadataAndHistoryCursor) {
  PreparedReparentWitness f; f.rejectRefresh = true;
  EXPECT_THROW(f.history->reparent(f.child, f.editor), std::runtime_error);
  EXPECT_EQ(f.graph.getNode(f.child)->parent, f.editor); EXPECT_EQ(f.metadata, (std::vector<uint32_t>{f.extra, f.child}));
  EXPECT_EQ(f.history->undoCount(), 1u); EXPECT_EQ(f.publications, 1);
  EXPECT_THROW(f.history->undo(), std::runtime_error);
  EXPECT_EQ(f.graph.getNode(f.child)->parent, f.parent); EXPECT_EQ(f.metadata, std::vector<uint32_t>{f.extra});
  EXPECT_EQ(f.history->redoCount(), 1u); EXPECT_EQ(f.publications, 2);
  f.rejectRefresh = false; ASSERT_TRUE(f.history->redo());
  EXPECT_EQ(f.graph.getNode(f.child)->parent, f.editor); EXPECT_EQ(f.publications, 3);
}

TEST(SceneEditHistory, PreparedReparentDoesNotInvokeActionAfterValidatorReplacesGraph) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::mat4(1), 0), child = graph.createNode(sheared(), 0);
  int actions = 0;
  History::Callbacks callbacks;
  callbacks.beforeReparent = [&](auto, auto, const auto&) { graph = SceneGraph{}; graph.createNode(glm::mat4(1), 0); };
  callbacks.prepareReparent = [&](auto, const auto&, const auto&) {
    return History::ReparentActions{[&] { ++actions; return true; }, [&] { ++actions; return true; }};
  };
  History history(graph, std::move(callbacks));
  EXPECT_FALSE(history.reparent(child, parent)); EXPECT_EQ(actions, 0); EXPECT_EQ(history.size(), 0u);
}

} // namespace
