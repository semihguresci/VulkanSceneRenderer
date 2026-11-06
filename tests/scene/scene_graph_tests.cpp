// scene_graph_tests.cpp
//
// CPU-only unit tests for the SceneGraph and SceneNode types.
// No GPU, no window, and no Vulkan device are required.

#include <gtest/gtest.h>

#include "Container/utility/SceneGraph.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

using container::scene::SceneGraph;
using container::scene::SceneNode;

// ============================================================================
// SceneNode defaults
// ============================================================================

TEST(SceneNode, DefaultsAreIdentityAndInvalid) {
  SceneNode node;
  EXPECT_EQ(node.localTransform, glm::mat4(1.0f));
  EXPECT_EQ(node.worldTransform, glm::mat4(1.0f));
  EXPECT_EQ(node.parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(node.materialIndex, 0u);
  EXPECT_EQ(node.primitiveIndex, SceneGraph::kInvalidNode);
  EXPECT_FALSE(node.renderable);
  EXPECT_TRUE(node.visible);
  EXPECT_TRUE(node.name.empty());
  EXPECT_TRUE(node.children.empty());
}

// ============================================================================
// SceneGraph — empty state
// ============================================================================

TEST(SceneGraph, DefaultConstructionIsEmpty) {
  SceneGraph graph;
  EXPECT_EQ(graph.nodeCount(), 0u);
  EXPECT_TRUE(graph.rootNodes().empty());
  EXPECT_TRUE(graph.renderableNodes().empty());
}

TEST(SceneGraph, GetNodeOnEmptyReturnsNull) {
  SceneGraph graph;
  EXPECT_EQ(graph.getNode(0), nullptr);
  EXPECT_EQ(graph.getNode(SceneGraph::kInvalidNode), nullptr);
}

// ============================================================================
// SceneGraph — createNode
// ============================================================================

TEST(SceneGraph, CreateNodeIncreasesCount) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0);
  EXPECT_EQ(graph.nodeCount(), 1u);

  graph.createNode(glm::mat4(1.0f), 0);
  EXPECT_EQ(graph.nodeCount(), 2u);
}

TEST(SceneGraph, MutationsAdvanceRevision) {
  SceneGraph graph;
  const uint64_t initialRevision = graph.revision();

  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  EXPECT_GT(graph.revision(), initialRevision);
  const uint64_t createRevision = graph.revision();

  const uint32_t child = graph.createNode(glm::mat4(1.0f), 0, false);
  graph.setParent(child, parent);
  EXPECT_GT(graph.revision(), createRevision);
  const uint64_t parentRevision = graph.revision();

  graph.setLocalTransform(child, glm::translate(glm::mat4(1.0f), glm::vec3(1.0f)));
  EXPECT_GT(graph.revision(), parentRevision);
  const uint64_t transformRevision = graph.revision();

  graph.setRenderable(child, true);
  EXPECT_GT(graph.revision(), transformRevision);
}

TEST(SceneGraph, NoOpMutationsDoNotAdvanceRevision) {
  SceneGraph graph;
  const uint32_t node = graph.createNode(glm::mat4(1.0f), 0, true, 0);
  const uint64_t revision = graph.revision();

  graph.setParent(node, std::nullopt);
  graph.setLocalTransform(node, glm::mat4(1.0f));
  graph.setRenderable(node, true);

  EXPECT_EQ(graph.revision(), revision);
}

TEST(SceneGraph, CreateNodeReturnsSequentialIndices) {
  SceneGraph graph;
  EXPECT_EQ(graph.createNode(glm::mat4(1.0f), 0), 0u);
  EXPECT_EQ(graph.createNode(glm::mat4(1.0f), 0), 1u);
  EXPECT_EQ(graph.createNode(glm::mat4(1.0f), 0), 2u);
}

TEST(SceneGraph, CreateNodePreservesTransformAndMaterial) {
  SceneGraph graph;
  const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(3, 4, 5));
  const uint32_t idx = graph.createNode(t, 7, false, 42, "Named node");

  const auto* node = graph.getNode(idx);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->localTransform, t);
  EXPECT_EQ(node->materialIndex, 7u);
  EXPECT_EQ(node->primitiveIndex, 42u);
  EXPECT_FALSE(node->renderable);
  EXPECT_EQ(node->name, "Named node");
}

TEST(SceneGraph, CreateRenderableNodeRegistersInList) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);
  EXPECT_EQ(graph.renderableNodes().size(), 1u);
}

TEST(SceneGraph, CreateNonRenderableNodeDoesNotRegister) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, false);
  EXPECT_TRUE(graph.renderableNodes().empty());
}

// ============================================================================
// SceneGraph — setRenderable
// ============================================================================

TEST(SceneGraph, SetRenderableTrueRegistersNode) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0, false);
  EXPECT_TRUE(graph.renderableNodes().empty());

  graph.setRenderable(idx, true);
  EXPECT_EQ(graph.renderableNodes().size(), 1u);
  EXPECT_EQ(graph.renderableNodes()[0], idx);
}

TEST(SceneGraph, SetRenderableFalseUnregistersNode) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0, true, 0);
  EXPECT_EQ(graph.renderableNodes().size(), 1u);

  graph.setRenderable(idx, false);
  EXPECT_TRUE(graph.renderableNodes().empty());
}

TEST(SceneGraph, SetRenderableTwiceDoesNotDuplicate) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0, true, 0);
  graph.setRenderable(idx, true);  // already renderable
  EXPECT_EQ(graph.renderableNodes().size(), 1u);
}

TEST(SceneGraph, VisibilityInheritsFromParents) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child = graph.createNode(glm::mat4(1.0f), 0, true, 0);
  graph.setParent(child, parent);

  EXPECT_TRUE(graph.isNodeEffectivelyVisible(parent));
  EXPECT_TRUE(graph.isNodeEffectivelyVisible(child));

  graph.setVisible(parent, false);

  EXPECT_FALSE(graph.getNode(parent)->visible);
  EXPECT_TRUE(graph.getNode(child)->visible);
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(parent));
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(child));
}

TEST(SceneGraph, VisibilityNoOpDoesNotAdvanceRevision) {
  SceneGraph graph;
  const uint32_t node = graph.createNode(glm::mat4(1.0f), 0);
  const uint64_t revision = graph.revision();

  graph.setVisible(node, true);

  EXPECT_EQ(graph.revision(), revision);
}

// ============================================================================
// SceneGraph — parent/child
// ============================================================================

TEST(SceneGraph, SetParentEstablishesRelationship) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child  = graph.createNode(glm::mat4(1.0f), 0);

  ASSERT_EQ(graph.rootNodes().size(), 2u);

  graph.setParent(child, parent);

  const auto* childNode  = graph.getNode(child);
  const auto* parentNode = graph.getNode(parent);
  ASSERT_NE(childNode, nullptr);
  ASSERT_NE(parentNode, nullptr);
  EXPECT_EQ(childNode->parent, parent);
  ASSERT_EQ(parentNode->children.size(), 1u);
  EXPECT_EQ(parentNode->children[0], child);
  ASSERT_EQ(graph.rootNodes().size(), 1u);
  EXPECT_EQ(graph.rootNodes()[0], parent);
}

TEST(SceneGraph, SetParentNulloptDetachesChild) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child  = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(child, parent);
  graph.setParent(child, std::nullopt);

  const auto* childNode  = graph.getNode(child);
  const auto* parentNode = graph.getNode(parent);
  ASSERT_NE(childNode, nullptr);
  ASSERT_NE(parentNode, nullptr);
  EXPECT_EQ(childNode->parent, SceneGraph::kInvalidNode);
  EXPECT_TRUE(parentNode->children.empty());
  EXPECT_NE(std::find(graph.rootNodes().begin(), graph.rootNodes().end(),
                      parent),
            graph.rootNodes().end());
  EXPECT_NE(std::find(graph.rootNodes().begin(), graph.rootNodes().end(),
                      child),
            graph.rootNodes().end());
}

TEST(SceneGraph, ReparentMovesChild) {
  SceneGraph graph;
  const uint32_t p1 = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t p2 = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t c  = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(c, p1);
  graph.setParent(c, p2);

  EXPECT_TRUE(graph.getNode(p1)->children.empty());
  ASSERT_EQ(graph.getNode(p2)->children.size(), 1u);
  EXPECT_EQ(graph.getNode(p2)->children[0], c);
  EXPECT_EQ(graph.getNode(c)->parent, p2);
}

TEST(SceneGraph, SetParentRejectsSelfParent) {
  SceneGraph graph;
  const uint32_t node = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(node, node);

  EXPECT_EQ(graph.getNode(node)->parent, SceneGraph::kInvalidNode);
  graph.updateWorldTransforms();
}

TEST(SceneGraph, SetParentRejectsDescendantParent) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t grandchild = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(child, parent);
  graph.setParent(grandchild, child);
  graph.setParent(parent, grandchild);

  EXPECT_EQ(graph.getNode(parent)->parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.getNode(child)->parent, parent);
  EXPECT_EQ(graph.getNode(grandchild)->parent, child);
  graph.updateWorldTransforms();
}

TEST(SceneGraph, SetParentRejectsInvalidParentWithoutDetaching) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(child, parent);
  graph.setParent(child, 999u);

  EXPECT_EQ(graph.getNode(child)->parent, parent);
  ASSERT_EQ(graph.getNode(parent)->children.size(), 1u);
  EXPECT_EQ(graph.getNode(parent)->children[0], child);
}

TEST(SceneGraph, ReparentPreservingWorldTransformKeepsNodeInPlace) {
  SceneGraph graph;
  const uint32_t firstParent = graph.createNode(
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), 0);
  const uint32_t secondParent = graph.createNode(
      glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)), 0);
  const uint32_t child = graph.createNode(
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)), 0);
  graph.setParent(child, firstParent);
  graph.updateWorldTransforms();
  const glm::mat4 originalWorld = graph.getNode(child)->worldTransform;

  EXPECT_TRUE(graph.setParentPreserveWorldTransform(child, secondParent));
  graph.updateWorldTransforms();

  const auto *childNode = graph.getNode(child);
  ASSERT_NE(childNode, nullptr);
  EXPECT_EQ(childNode->parent, secondParent);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(childNode->worldTransform[c][r], originalWorld[c][r],
                  1e-5f);
    }
  }
}

TEST(SceneGraph, ReparentPreservingWorldTransformRejectsSingularParent) {
  SceneGraph graph;
  const uint32_t child = graph.createNode(
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)), 0);
  const uint32_t singularParent =
      graph.createNode(glm::scale(glm::mat4(1.0f), glm::vec3(0.0f)), 0);
  graph.updateWorldTransforms();
  const uint64_t revision = graph.revision();
  const glm::mat4 originalLocal = graph.getNode(child)->localTransform;

  EXPECT_FALSE(
      graph.setParentPreserveWorldTransform(child, singularParent));

  EXPECT_EQ(graph.getNode(child)->parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.getNode(child)->localTransform, originalLocal);
  EXPECT_EQ(graph.revision(), revision);
}

// ============================================================================
// SceneGraph — transforms
// ============================================================================

TEST(SceneGraph, ReparentPreservingWorldTransformAcceptsTinyValidScale) {
  SceneGraph graph;
  const uint32_t child = graph.createNode(
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)), 0);
  const uint32_t tinyParent =
      graph.createNode(glm::scale(glm::mat4(1.0f), glm::vec3(0.001f)), 0);
  graph.updateWorldTransforms();
  const glm::mat4 originalWorld = graph.getNode(child)->worldTransform;

  EXPECT_TRUE(graph.setParentPreserveWorldTransform(child, tinyParent));
  graph.updateWorldTransforms();

  const auto *childNode = graph.getNode(child);
  ASSERT_NE(childNode, nullptr);
  EXPECT_EQ(childNode->parent, tinyParent);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      EXPECT_NEAR(childNode->worldTransform[c][r], originalWorld[c][r],
                  1e-4f);
    }
  }
}

TEST(SceneGraph, SetLocalTransformUpdatesNode) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0);
  const glm::mat4 t = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));

  graph.setLocalTransform(idx, t);
  EXPECT_EQ(graph.getNode(idx)->localTransform, t);
}

TEST(SceneGraph, UpdateWorldTransformsPropagatesParentTransform) {
  SceneGraph graph;
  const glm::mat4 parentT = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
  const glm::mat4 childT  = glm::translate(glm::mat4(1.0f), glm::vec3(0, 5, 0));

  const uint32_t parent = graph.createNode(parentT, 0);
  const uint32_t child  = graph.createNode(childT, 0);
  graph.setParent(child, parent);

  graph.updateWorldTransforms();

  // Parent world = parentT (no parent above it)
  EXPECT_EQ(graph.getNode(parent)->worldTransform, parentT);
  // Child world = parentT * childT
  const glm::mat4 expected = parentT * childT;
  const auto& actual = graph.getNode(child)->worldTransform;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      EXPECT_NEAR(actual[c][r], expected[c][r], 1e-5f)
          << "mismatch at [" << c << "][" << r << "]";
}

TEST(SceneGraph, RootNodeWorldTransformEqualsLocal) {
  SceneGraph graph;
  const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
  const uint32_t idx = graph.createNode(t, 0);

  graph.updateWorldTransforms();

  EXPECT_EQ(graph.getNode(idx)->worldTransform, t);
}

TEST(SceneGraph, ThreeLevelHierarchyPropagatesCorrectly) {
  SceneGraph graph;
  const glm::mat4 t1 = glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, 0));
  const glm::mat4 t2 = glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, 0));
  const glm::mat4 t3 = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 3));

  const uint32_t n1 = graph.createNode(t1, 0);
  const uint32_t n2 = graph.createNode(t2, 0);
  const uint32_t n3 = graph.createNode(t3, 0);
  graph.setParent(n2, n1);
  graph.setParent(n3, n2);

  graph.updateWorldTransforms();

  const glm::mat4 expected3 = t1 * t2 * t3;
  const auto& actual3 = graph.getNode(n3)->worldTransform;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      EXPECT_NEAR(actual3[c][r], expected3[c][r], 1e-5f);
}

// ============================================================================
// SceneGraph — getNode bounds
// ============================================================================

TEST(SceneGraph, GetNodeOutOfBoundsReturnsNull) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0);

  EXPECT_EQ(graph.getNode(1), nullptr);
  EXPECT_EQ(graph.getNode(999), nullptr);
}

TEST(SceneGraph, GetNodeConstAndMutableAgree) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 5);

  const auto& constGraph = graph;
  EXPECT_EQ(constGraph.getNode(idx)->materialIndex, 5u);
  EXPECT_EQ(graph.getNode(idx)->materialIndex, 5u);
}

TEST(SceneGraph, DetachedNodeEditRetainsLifetimeAndExactSiblingOrder) {
  SceneGraph graph;
  const auto left = graph.createNode(glm::mat4(1), 0);
  const auto right = graph.createNode(glm::mat4(1), 0);
  const auto first = graph.createNode(glm::mat4(1), 0);
  const auto moved = graph.createNode(glm::mat4(1), 0);
  const auto last = graph.createNode(glm::mat4(1), 0);
  ASSERT_TRUE(graph.setParent(first, left));
  ASSERT_TRUE(graph.setParent(moved, left));
  ASSERT_TRUE(graph.setParent(last, left));
  graph.updateWorldTransforms();
  const auto identity = graph.identity();
  const auto incarnation = graph.nodeIncarnation(moved);
  const auto revision = graph.revision();
  SceneGraph candidate(graph);
  ASSERT_FALSE(candidate.setParentAt(left, moved, 0));
  EXPECT_EQ(candidate.revision(), revision);
  ASSERT_TRUE(candidate.setParentAt(moved, right, 0));
  ASSERT_TRUE(candidate.setParentAt(moved, left, 1));
  EXPECT_EQ(candidate.getNode(left)->children, (std::vector{first, moved, last}));
  ASSERT_TRUE(candidate.setParentAt(moved, right, 0));
  candidate.updateWorldTransforms();
  graph.exchangeNodeEdit(candidate);
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_EQ(graph.nodeIncarnation(moved), incarnation);
  EXPECT_GT(graph.revision(), revision);
  EXPECT_EQ(graph.getNode(left)->children, (std::vector{first, last}));
  EXPECT_EQ(graph.siblingIndex(moved), 0u);
  EXPECT_EQ(candidate.getNode(left)->children, (std::vector{first, moved, last}));
}

TEST(SceneGraph, SceneLifetimeDistinguishesReusedNodeIndices) {
  SceneGraph graph;
  const auto node = graph.createNode(glm::mat4(1), 0);
  const auto identity = graph.identity();
  const auto incarnation = graph.nodeIncarnation(node);
  graph.createNode(glm::mat4(1), 0);
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_EQ(graph.nodeIncarnation(node), incarnation);

  SceneGraph snapshot = graph;
  EXPECT_NE(snapshot.identity(), identity);
  SceneGraph moved = std::move(graph);
  EXPECT_EQ(moved.identity(), identity);
  EXPECT_EQ(moved.nodeIncarnation(node), incarnation);
  EXPECT_NE(graph.identity(), moved.identity());
  moved = SceneGraph{};
  EXPECT_EQ(moved.createNode(glm::mat4(1), 0), node);
  EXPECT_NE(moved.identity(), identity);
}

TEST(SceneGraph, DetachedTailRestoresIncarnationAndRejectsReusedSlots) {
  SceneGraph graph;
  const auto root = graph.createNode(glm::mat4(1), 0);
  const auto leaf = graph.createNode(glm::mat4(1), 0, true, 1, "Cube");
  ASSERT_TRUE(graph.setParent(leaf, root));
  const auto saved = *graph.getNode(leaf);
  const auto incarnation = graph.nodeIncarnation(leaf);
  EXPECT_FALSE(graph.removeLastLeaf(root, graph.nodeIncarnation(root)));
  EXPECT_FALSE(graph.removeLastLeaf(leaf, 0));
  ASSERT_TRUE(graph.removeLastLeaf(leaf, incarnation));
  EXPECT_EQ(graph.nodeIncarnation(leaf), 0u);
  EXPECT_TRUE(graph.getNode(root)->children.empty());
  EXPECT_TRUE(graph.renderableNodes().empty());
  ASSERT_TRUE(graph.restoreLastLeaf(leaf, saved, 0));
  EXPECT_EQ(graph.nodeIncarnation(leaf), incarnation);
  EXPECT_EQ(graph.siblingIndex(leaf), 0u);
  EXPECT_EQ(graph.renderableNodes(), (std::vector{leaf}));
  EXPECT_FALSE(graph.restoreLastLeaf(leaf + 1, saved, 0));
  ASSERT_TRUE(graph.removeLastLeaf(leaf, incarnation));
  EXPECT_EQ(graph.createNode(glm::mat4(1), 0), leaf);
  EXPECT_NE(graph.nodeIncarnation(leaf), incarnation);
  EXPECT_FALSE(graph.removeLastLeaf(leaf, incarnation));
}

}  // namespace
