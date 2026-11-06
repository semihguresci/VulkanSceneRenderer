// scene_graph_tests.cpp
//
// CPU-only unit tests for the SceneGraph and SceneNode types.
// No GPU, no window, and no Vulkan device are required.

#include <gtest/gtest.h>

#include "Container/utility/SceneGraph.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
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

TEST(SceneGraph, DeepInactiveHierarchyUpdatesWithoutRecursiveTraversal) {
  SceneGraph graph;
  constexpr uint32_t depth = 10000;
  const auto step = glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, 0));
  for (uint32_t i = 0; i < depth; ++i) graph.createNode(step, 0);
  // Attach from the leaf upward so constructing the test does not repeatedly
  // walk an increasingly long ancestor chain in the cycle guard.
  for (uint32_t i = depth - 1; i > 0; --i) ASSERT_TRUE(graph.setParent(i, i - 1));
  const auto active = graph.createNode(glm::mat4(1.0f), 3, true, 7);
  graph.setActiveRoots(std::array{active});
  graph.setVisible(0, false);
  const auto rootTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10, 2, 3)) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(2, 3, 4));
  graph.setLocalTransform(0, rootTransform);
  const auto revision = graph.revision();
  const auto hierarchyRevision = graph.hierarchyRevision();
  const auto identity = graph.identity();
  graph.updateWorldTransforms();

  for (const auto index : {0u, depth / 2, depth - 1}) {
    const auto expected = rootTransform * glm::translate(
        glm::mat4(1.0f), glm::vec3(static_cast<float>(index), 0, 0));
    EXPECT_EQ(graph.getNode(index)->worldTransform, expected) << index;
  }
  EXPECT_FALSE(graph.isNodeActive(depth - 1));
  EXPECT_FALSE(graph.getNode(0)->visible);
  EXPECT_EQ(graph.getNode(depth - 1)->localTransform, step);
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{active});
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.hierarchyRevision(), hierarchyRevision);
  EXPECT_EQ(graph.identity(), identity);
}

TEST(SceneGraph, WorldUpdatesRefreshEditedAndReparentedBranchesInParentOrder) {
  SceneGraph graph;
  const auto leafLocal = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 4));
  const auto branchLocal = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 0));
  const auto siblingLocal = glm::translate(glm::mat4(1.0f), glm::vec3(0, 3, 0));
  const auto firstLocal = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)) *
                          glm::scale(glm::mat4(1.0f), glm::vec3(2, 3, 4));
  const auto secondLocal = glm::translate(glm::mat4(1.0f), glm::vec3(-5, 6, 7)) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(-2, 4, 3));
  // Children precede their parents in storage, so node-index iteration alone
  // cannot propagate the current parent transform.
  const auto leaf = graph.createNode(leafLocal, 11, true, 9);
  const auto branch = graph.createNode(branchLocal, 0);
  const auto first = graph.createNode(firstLocal, 0);
  const auto second = graph.createNode(secondLocal, 0);
  const auto sibling = graph.createNode(siblingLocal, 12, true, 10);
  ASSERT_TRUE(graph.setParent(leaf, branch));
  ASSERT_TRUE(graph.setParent(branch, first));
  ASSERT_TRUE(graph.setParent(sibling, first));
  graph.setVisible(branch, false);
  graph.updateWorldTransforms();
  EXPECT_EQ(graph.getNode(leaf)->worldTransform, firstLocal * branchLocal * leafLocal);

  const auto edited = glm::translate(glm::mat4(1.0f), glm::vec3(3, -2, 1)) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(4, 2, -3));
  graph.setLocalTransform(first, edited);
  graph.updateWorldTransforms();
  EXPECT_EQ(graph.getNode(leaf)->worldTransform, edited * branchLocal * leafLocal);
  EXPECT_EQ(graph.getNode(sibling)->worldTransform, edited * siblingLocal);

  ASSERT_TRUE(graph.setParent(branch, second));
  graph.setActiveRoots(std::array{second});
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{leaf});
  const auto revision = graph.revision();
  const auto hierarchyRevision = graph.hierarchyRevision();
  graph.updateWorldTransforms();
  EXPECT_EQ(graph.getNode(leaf)->worldTransform, secondLocal * branchLocal * leafLocal);
  EXPECT_EQ(graph.getNode(sibling)->worldTransform, edited * siblingLocal);
  EXPECT_EQ(graph.getNode(branch)->localTransform, branchLocal);
  EXPECT_FALSE(graph.getNode(branch)->visible);
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(leaf));
  EXPECT_EQ(graph.getNode(leaf)->materialIndex, 11u);
  EXPECT_EQ(graph.getNode(leaf)->primitiveIndex, 9u);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.hierarchyRevision(), hierarchyRevision);
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

struct AppendSnapshot {
  uint64_t identity, revision, hierarchyRevision;
  std::vector<SceneNode> nodes;
  std::vector<uint32_t> roots, renderables;
  std::vector<bool> active, context;
};

AppendSnapshot appendSnapshot(const SceneGraph& graph) {
  AppendSnapshot result{graph.identity(), graph.revision(), graph.hierarchyRevision(), {},
      graph.rootNodes(), graph.renderableNodes(), {}, {}};
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    result.nodes.push_back(*graph.getNode(id));
    result.active.push_back(graph.isNodeActive(id));
    result.context.push_back(graph.isNodeInActiveHierarchy(id));
  }
  return result;
}

void expectAppendUnchanged(const SceneGraph& graph, const AppendSnapshot& before) {
  EXPECT_EQ(graph.identity(), before.identity);
  EXPECT_EQ(graph.revision(), before.revision);
  EXPECT_EQ(graph.hierarchyRevision(), before.hierarchyRevision);
  ASSERT_EQ(graph.nodeCount(), before.nodes.size());
  EXPECT_EQ(graph.rootNodes(), before.roots);
  EXPECT_EQ(graph.renderableNodes(), before.renderables);
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    EXPECT_EQ(*graph.getNode(id), before.nodes[id]) << id;
    EXPECT_EQ(graph.isNodeActive(id), before.active[id]) << id;
    EXPECT_EQ(graph.isNodeInActiveHierarchy(id), before.context[id]) << id;
  }
}

static_assert(!std::is_copy_constructible_v<SceneGraph::PreparedAppend>);
static_assert(!std::is_copy_assignable_v<SceneGraph::PreparedAppend>);
static_assert(std::is_nothrow_move_constructible_v<SceneGraph::PreparedAppend>);
static_assert(std::is_nothrow_move_assignable_v<SceneGraph::PreparedAppend>);
static_assert(std::is_nothrow_destructible_v<SceneGraph::PreparedAppend>);
static_assert(noexcept(std::declval<SceneGraph&>().publishAppend(std::declval<SceneGraph::PreparedAppend&>())));

TEST(SceneGraph, PreparedAppendPublishesTailAndAdoptedChildrenWithoutReplacingIdentity) {
  SceneGraph graph;
  const auto root = graph.createNode(glm::translate(glm::mat4(1), glm::vec3(10, 0, 0)), 4, false, 9, "Retained");
  graph.getNode(root)->sourceNodeIndex = 17;
  graph.setVisible(root, false);
  graph.updateWorldTransforms();
  const auto before = appendSnapshot(graph);
  SceneGraph candidate(graph);
  const auto tail = candidate.createNode(glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)), 6, true, 3, "New");
  candidate.getNode(tail)->sourceMeshIndex = 1;
  candidate.getNode(tail)->sourcePrimitiveIndex = 2;
  candidate.getNode(tail)->sourceInstanceIndex = 0;
  candidate.getNode(tail)->meshOwnerGraphNode = root;
  ASSERT_TRUE(candidate.setParent(tail, root));
  candidate.updateWorldTransforms();
  auto prepared = graph.prepareAppend(std::move(candidate));
  expectAppendUnchanged(graph, before);
  SceneGraph::PreparedAppend moved(std::move(prepared));
  EXPECT_FALSE(graph.publishAppend(prepared));
  ASSERT_TRUE(graph.publishAppend(moved));
  EXPECT_EQ(graph.identity(), before.identity);
  EXPECT_EQ(graph.revision(), before.revision + 1);
  EXPECT_EQ(graph.hierarchyRevision(), before.hierarchyRevision + 1);
  EXPECT_EQ(graph.nodeCount(), 2u);
  auto retained = before.nodes[0]; retained.children.push_back(tail);
  EXPECT_EQ(*graph.getNode(root), retained);
  EXPECT_EQ(graph.getNode(tail)->worldTransform, glm::translate(glm::mat4(1), glm::vec3(12, 0, 0)));
  EXPECT_EQ(graph.getNode(tail)->meshOwnerGraphNode, root);
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{tail});
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(tail));
  const auto published = appendSnapshot(graph);
  EXPECT_FALSE(graph.publishAppend(moved));
  SceneGraph::PreparedAppend empty;
  EXPECT_FALSE(graph.publishAppend(empty));
  expectAppendUnchanged(graph, published);
}

TEST(SceneGraph, PreparedAppendRejectsChangedRetainedPayloadAcrossAllIdentityAndFlagFields) {
  SceneGraph graph;
  const auto retained = graph.createNode(glm::mat4(1), 0);
  const auto before = appendSnapshot(graph);
  for (int field = 0; field < 16; ++field) {
    SCOPED_TRACE(field);
    SceneGraph candidate(graph);
    const auto tail = candidate.createNode(glm::mat4(1), 0);
    auto& node = *candidate.getNode(retained);
    switch (field) {
      case 0: node.localTransform[3][0] = 1; break;
      case 1: node.worldTransform[3][1] = 1; break;
      case 2: node.materialIndex = 3; break;
      case 3: node.primitiveIndex = 0; break;
      case 4: node.sourceNodeIndex = 12; break;
      case 5: node.editorSceneRoot = true; break;
      case 6: node.meshOwnerGraphNode = tail; break;
      case 7: node.renderable = true; break;
      case 8: node.visible = false; break;
      case 9: node.name = "changed"; break;
      case 10: node.sourceMeshIndex = node.sourcePrimitiveIndex = 0; break;
      case 11: node.sourceInstanceIndex = 0; break;
      case 12: node.selectable = false; break;
      case 13: node.hoverable = false; break;
      case 14: node.interactionRootBoundary = true; break;
      case 15: node.parent = tail; break;
    }
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument);
    expectAppendUnchanged(graph, before);
  }
}

TEST(SceneGraph, PreparedAppendRejectsRetainedReparentingSiblingAndRenderableReordering) {
  SceneGraph graph;
  const auto root = graph.createNode(glm::mat4(1), 0);
  const auto a = graph.createNode(glm::mat4(1), 0, true);
  const auto b = graph.createNode(glm::mat4(1), 0, true);
  ASSERT_TRUE(graph.setParent(a, root));
  ASSERT_TRUE(graph.setParent(b, root));
  const auto before = appendSnapshot(graph);
  for (int change = 0; change < 3; ++change) {
    SceneGraph candidate(graph);
    const auto tail = candidate.createNode(glm::mat4(1), 0, true);
    if (change == 0) ASSERT_TRUE(candidate.setParent(a, tail));
    if (change == 1) ASSERT_TRUE(candidate.setSiblingIndex(a, 1));
    if (change == 2) { candidate.setRenderable(a, false); candidate.setRenderable(a, true); }
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument);
    expectAppendUnchanged(graph, before);
  }
  SceneGraph roots;
  roots.createNode(glm::mat4(1), 0); roots.createNode(glm::mat4(1), 0);
  SceneGraph reordered(roots);
  reordered.createNode(glm::mat4(1), 0);
  ASSERT_TRUE(reordered.setSiblingIndex(1, 0));
  EXPECT_THROW((void)roots.prepareAppend(std::move(reordered)), std::invalid_argument);
}

TEST(SceneGraph, PreparedAppendRefusesStaleEditsReplacementAndForeignOwnerWithoutMutation) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1), 0);
  SceneGraph candidate(graph); candidate.createNode(glm::mat4(1), 0);
  auto prepared = graph.prepareAppend(std::move(candidate));
  SceneGraph foreign(graph);
  const auto foreignBefore = appendSnapshot(foreign);
  EXPECT_FALSE(foreign.publishAppend(prepared));
  expectAppendUnchanged(foreign, foreignBefore);
  graph.setVisible(0, false);
  const auto edited = appendSnapshot(graph);
  EXPECT_FALSE(graph.publishAppend(prepared));
  expectAppendUnchanged(graph, edited);

  SceneGraph next(graph); next.createNode(glm::mat4(1), 0);
  auto replacedProof = graph.prepareAppend(std::move(next));
  SceneGraph replacement(graph);
  graph = std::move(replacement);
  const auto replaced = appendSnapshot(graph);
  EXPECT_FALSE(graph.publishAppend(replacedProof));
  expectAppendUnchanged(graph, replaced);
}

TEST(SceneGraph, PreparedAppendRejectsDisconnectedCyclesDuplicateAndOverflowingIndicesBoundedly) {
  SceneGraph graph;
  const auto root = graph.createNode(glm::mat4(1), 0);
  const auto before = appendSnapshot(graph);
  for (int invalid = 0; invalid < 12; ++invalid) {
    SCOPED_TRACE(invalid);
    SceneGraph candidate(graph);
    const auto a = candidate.createNode(glm::mat4(1), 0, true);
    const auto b = candidate.createNode(glm::mat4(1), 0);
    auto& roots = const_cast<std::vector<uint32_t>&>(candidate.rootNodes());
    auto& renderables = const_cast<std::vector<uint32_t>&>(candidate.renderableNodes());
    switch (invalid) {
      case 0:
        candidate.getNode(root)->children = {a, a}; candidate.getNode(a)->parent = root;
        std::erase(roots, a); break;
      case 1:
        candidate.getNode(a)->parent = b; candidate.getNode(a)->children = {b};
        candidate.getNode(b)->parent = a; candidate.getNode(b)->children = {a};
        roots.resize(1); break;
      case 2: candidate.getNode(root)->children = {SceneGraph::kInvalidNode}; break;
      case 3: candidate.getNode(a)->parent = SceneGraph::kInvalidNode - 1; break;
      case 4: roots.push_back(SceneGraph::kInvalidNode); break;
      case 5: roots.push_back(a); break;
      case 6: renderables.push_back(a); break;
      case 7: renderables.push_back(SceneGraph::kInvalidNode); break;
      case 8: candidate.getNode(b)->renderable = true; break;
      case 9: candidate.getNode(a)->renderable = false; break;
      case 10: candidate.getNode(b)->children = {root}; break;
      case 11: candidate.getNode(a)->parent = b; break;
    }
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument);
    expectAppendUnchanged(graph, before);
  }
}

TEST(SceneGraph, PreparedAppendValidatesNewFiniteTransformsAndOwnerSourceReferences) {
  SceneGraph graph;
  graph.createNode(glm::scale(glm::mat4(1), glm::vec3(0, 1, -2)), 17);
  graph.setVisible(0, false); // Existing finite singular engine edits remain valid.
  const auto before = appendSnapshot(graph);
  for (int invalid = 0; invalid < 11; ++invalid) {
    SceneGraph candidate(graph);
    const auto tail = candidate.createNode(glm::mat4(1), 0);
    auto& node = *candidate.getNode(tail);
    switch (invalid) {
      case 0: node.localTransform[0][0] = std::numeric_limits<float>::infinity(); break;
      case 1: node.worldTransform[0][0] = std::numeric_limits<float>::quiet_NaN(); break;
      case 2: node.meshOwnerGraphNode = 999; break;
      case 3: node.meshOwnerGraphNode = tail; break;
      case 4: node.sourceNodeIndex = -2; break;
      case 5: node.sourceMeshIndex = -2; break;
      case 6: node.sourcePrimitiveIndex = -2; break;
      case 7: node.sourceInstanceIndex = -2; break;
      case 8: node.sourceMeshIndex = 0; break;
      case 9: node.sourcePrimitiveIndex = 0; break;
      case 10: node.sourceInstanceIndex = 0; break;
    }
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument) << invalid;
    expectAppendUnchanged(graph, before);
  }
  SceneGraph valid(graph); valid.createNode(glm::mat4(1), 0);
  auto prepared = graph.prepareAppend(std::move(valid));
  EXPECT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.getNode(0)->localTransform, before.nodes[0].localTransform);
}

TEST(SceneGraph, PreparedAppendPreservesExplicitActiveViewAndPublishesPreparedMembership) {
  SceneGraph graph;
  const auto active = graph.createNode(glm::mat4(1), 0, true);
  const auto inactive = graph.createNode(glm::mat4(1), 0, true);
  graph.setActiveRoots(std::array{active});
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{active});
  SceneGraph candidate(graph);
  const auto tail = candidate.createNode(glm::mat4(1), 0, true);
  candidate.setActiveRoots(std::array{active, tail});
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.renderableNodes(), (std::vector<uint32_t>{active, tail}));
  EXPECT_TRUE(graph.isNodeActive(active));
  EXPECT_TRUE(graph.isNodeActive(tail));
  EXPECT_FALSE(graph.isNodeActive(inactive));
}

TEST(SceneGraph, PreparedAppendCanIntroduceAnActiveViewWithoutActivatingCanonicalIncomingRoots) {
  SceneGraph graph;
  const auto root = graph.createNode(glm::mat4(1), 0);
  const auto oldMesh = graph.createNode(glm::mat4(1), 0, true);
  ASSERT_TRUE(graph.setParent(oldMesh, root));
  SceneGraph candidate(graph);
  const auto canonical = candidate.createNode(glm::mat4(1), 0, true);
  const auto placed = candidate.createNode(glm::mat4(1), 0);
  const auto newMesh = candidate.createNode(glm::mat4(1), 0, true);
  ASSERT_TRUE(candidate.setParent(placed, root));
  ASSERT_TRUE(candidate.setParent(newMesh, placed));
  candidate.setActiveRoots(std::array{root});
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.rootNodes(), (std::vector<uint32_t>{root, canonical}));
  EXPECT_EQ(graph.renderableNodes(), (std::vector<uint32_t>{oldMesh, newMesh}));
  EXPECT_FALSE(graph.isNodeActive(canonical));
  EXPECT_TRUE(graph.isNodeActive(placed));
}

TEST(SceneGraph, PreparedAppendRejectsDroppingOrExpandingRetainedActiveSelectionAndInvalidRoots) {
  SceneGraph graph;
  const auto active = graph.createNode(glm::mat4(1), 0);
  const auto inactive = graph.createNode(glm::mat4(1), 0);
  graph.setActiveRoots(std::array{active});
  for (int invalid = 0; invalid < 3; ++invalid) {
    SceneGraph candidate(graph); const auto tail = candidate.createNode(glm::mat4(1), 0);
    if (invalid == 0) candidate.clearActiveRoots();
    if (invalid == 1) candidate.setActiveRoots(std::array{active, inactive, tail});
    if (invalid == 2) candidate.setActiveRoots(std::array{tail, active});
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument);
  }
  SceneGraph allActive;
  allActive.createNode(glm::mat4(1), 0); allActive.createNode(glm::mat4(1), 0);
  SceneGraph dropsRoot(allActive); dropsRoot.createNode(glm::mat4(1), 0);
  dropsRoot.setActiveRoots(std::array{uint32_t(0)});
  EXPECT_THROW((void)allActive.prepareAppend(std::move(dropsRoot)), std::invalid_argument);
  SceneGraph dangling(allActive);
  dangling.createNode(glm::mat4(1), 0);
  const auto removed = dangling.createNode(glm::mat4(1), 0);
  dangling.setActiveRoots(std::array{uint32_t(0), uint32_t(1), removed});
  ASSERT_TRUE(dangling.detachTail(removed));
  // Tail retirement now removes retired roots from the active view atomically.
  EXPECT_EQ(*dangling.activeRoots(), (std::vector<uint32_t>{0, 1}));
  EXPECT_NO_THROW((void)allActive.prepareAppend(std::move(dangling)));
}

TEST(SceneGraph, PreparedAppendRejectsAnEmptyTailWithoutMutatingTheOwner) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1), 0);
  const auto before = appendSnapshot(graph);
  EXPECT_THROW((void)graph.prepareAppend(SceneGraph(graph)), std::invalid_argument);
  EXPECT_THROW((void)graph.prepareAppend(SceneGraph{}), std::invalid_argument);
  expectAppendUnchanged(graph, before);
}

TEST(SceneGraph, PreparedAppendValidatesWideForestsWithoutQuadraticChildSearches) {
  SceneGraph graph;
  const auto root = graph.createNode(glm::mat4(1), 0);
  SceneGraph candidate(graph);
  constexpr uint32_t width = 16384;
  for (uint32_t i = 0; i < width; ++i) candidate.createNode(glm::mat4(1), 0, i == width - 1);
  // Build the offline candidate directly so test construction itself avoids
  // repeated root-list erasure in the ordinary one-node reparent operation.
  auto& children = candidate.getNode(root)->children;
  children.reserve(width);
  for (uint32_t id = 1; id <= width; ++id) {
    children.push_back(id); candidate.getNode(id)->parent = root;
  }
  const_cast<std::vector<uint32_t>&>(candidate.rootNodes()).resize(1);
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.getNode(root)->children.size(), width);
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{width});
}

TEST(SceneGraph, PreparedAppendValidatesDeepForestsWithoutRecursionOrAncestorQuadraticWork) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1), 0);
  SceneGraph candidate(graph);
  constexpr uint32_t depth = 10000;
  const auto step = glm::translate(glm::mat4(1), glm::vec3(1, 0, 0));
  for (uint32_t i = 1; i <= depth; ++i) candidate.createNode(step, 0, i == depth);
  for (uint32_t id = 1; id <= depth; ++id) {
    candidate.getNode(id)->parent = id - 1;
    candidate.getNode(id - 1)->children = {id};
  }
  const_cast<std::vector<uint32_t>&>(candidate.rootNodes()).resize(1);
  candidate.updateWorldTransforms();
  candidate.setActiveRoots(std::array{depth}); // Prefix must retain the old root instead.
  EXPECT_THROW((void)graph.prepareAppend(candidate), std::invalid_argument);
  candidate.setActiveRoots(std::array{uint32_t(0)});
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.nodeCount(), depth + 1);
  EXPECT_EQ(graph.getNode(depth)->worldTransform, glm::translate(glm::mat4(1), glm::vec3(float(depth), 0, 0)));
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{depth});
}

TEST(SceneGraph, PreparedAppendRefinesEditorContextIntoOriginalChildrenOnly) {
  for (const bool explicitView : {false, true}) {
    SCOPED_TRACE(explicitView);
    SceneGraph graph;
    const auto editor = graph.createNode(glm::mat4(1), 0);
    graph.getNode(editor)->editorSceneRoot = true;
    const auto source = graph.createNode(glm::mat4(1), 0);
    graph.getNode(source)->sourceNodeIndex = 0;
    const auto draw = graph.createNode(glm::mat4(1), 3, true, 0);
    const auto procedural = graph.createNode(glm::mat4(1), 4, true, 1);
    const auto inactive = graph.createNode(glm::mat4(1), 0);
    graph.getNode(inactive)->sourceNodeIndex = 1;
    const auto inactiveDraw = graph.createNode(glm::mat4(1), 5, true, 2);
    ASSERT_TRUE(graph.setParent(source, editor)); ASSERT_TRUE(graph.setParent(draw, source));
    ASSERT_TRUE(graph.setParent(procedural, editor)); ASSERT_TRUE(graph.setParent(inactiveDraw, inactive));
    if (explicitView) graph.setActiveRoots(std::array{editor, source, procedural, editor});
    graph.updateWorldTransforms();
    std::vector<bool> oldMembership;
    for (uint32_t id = 0; id < graph.nodeCount(); ++id) oldMembership.push_back(graph.isNodeActive(id));
    const auto before = appendSnapshot(graph);
    SceneGraph candidate(graph);
    const auto wrapper = candidate.createNode(glm::mat4(1), 0);
    const auto canonical = candidate.createNode(glm::mat4(1), 6, true, 3);
    const auto placed = candidate.createNode(glm::mat4(1), 6, true, 3);
    ASSERT_TRUE(candidate.setParent(wrapper, editor));
    ASSERT_TRUE(candidate.setParent(canonical, wrapper)); ASSERT_TRUE(candidate.setParent(placed, wrapper));
    std::vector<uint32_t> roots{source, procedural};
    if (!explicitView) roots.push_back(inactive);
    roots.push_back(placed); candidate.setActiveRoots(roots);
    auto proof = graph.prepareAppend(std::move(candidate));
    ASSERT_TRUE(graph.publishAppend(proof));
    EXPECT_EQ(graph.identity(), before.identity);
    EXPECT_FALSE(graph.isNodeActive(editor)); EXPECT_TRUE(graph.isNodeInActiveHierarchy(editor));
    for (uint32_t id = 1; id < oldMembership.size(); ++id)
      EXPECT_EQ(graph.isNodeActive(id), oldMembership[id]) << id;
    EXPECT_EQ(graph.getNode(source)->sourceNodeIndex, 0);
    EXPECT_FALSE(graph.isNodeActive(canonical)); EXPECT_TRUE(graph.isNodeActive(placed));
    EXPECT_EQ(*graph.activeRoots(), roots);
    EXPECT_EQ(graph.renderableNodes().size(), explicitView ? 3u : 4u);
    EXPECT_EQ(graph.isNodeActive(inactiveDraw), !explicitView);
  }
}

TEST(SceneGraph, PreparedAppendRejectsArbitraryOldRootChangesDuringEditorRefinement) {
  SceneGraph graph;
  const auto editor = graph.createNode(glm::mat4(1), 0);
  graph.getNode(editor)->editorSceneRoot = true;
  const auto first = graph.createNode(glm::mat4(1), 0, true);
  const auto second = graph.createNode(glm::mat4(1), 0, true);
  const auto inactive = graph.createNode(glm::mat4(1), 0, true);
  ASSERT_TRUE(graph.setParent(first, editor)); ASSERT_TRUE(graph.setParent(second, editor));
  graph.setActiveRoots(std::array{editor});
  const auto before = appendSnapshot(graph);
  for (const auto& oldRoots : std::vector<std::vector<uint32_t>>{
           {first}, {second, first}, {first, second, inactive}, {first, first, second}, {editor, second}}) {
    SceneGraph candidate(graph);
    const auto added = candidate.createNode(glm::mat4(1), 0, true);
    auto roots = oldRoots; roots.push_back(added); candidate.setActiveRoots(roots);
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument);
    expectAppendUnchanged(graph, before);
  }
}

TEST(SceneGraph, PreparedAppendRefinementCannotDropSourceBoundOrRenderableEditorRoots) {
  for (const bool renderable : {false, true}) {
    SceneGraph graph;
    const auto editor = graph.createNode(glm::mat4(1), 0, renderable);
    graph.getNode(editor)->editorSceneRoot = true;
    if (!renderable) graph.getNode(editor)->sourceNodeIndex = 0;
    const auto child = graph.createNode(glm::mat4(1), 0, true);
    ASSERT_TRUE(graph.setParent(child, editor)); graph.setActiveRoots(std::array{editor});
    SceneGraph candidate(graph);
    const auto added = candidate.createNode(glm::mat4(1), 0, true);
    candidate.setActiveRoots(std::array{child, added});
    EXPECT_THROW((void)graph.prepareAppend(std::move(candidate)), std::invalid_argument);
  }
}

TEST(SceneGraph, BulkAppendRetainsPrivateRenderableOrderAndMovesGlobalTopology) {
  SceneGraph graph;
  const auto first = graph.createNode(glm::mat4(1), 0, false);
  const auto second = graph.createNode(glm::mat4(1), 0, true);
  graph.setRenderable(first, true); // Render order intentionally differs from IDs.
  const auto before = appendSnapshot(graph);
  std::vector<SceneNode> tail(3);
  tail[0].parent = first; tail[0].children = {3, 4};
  tail[1].parent = 2; tail[1].renderable = true; tail[1].meshOwnerGraphNode = 2;
  tail[2].parent = 2; tail[2].renderable = true; tail[2].meshOwnerGraphNode = 2;
  auto candidate = graph.makeAppendCandidate(std::move(tail));
  expectAppendUnchanged(graph, before);
  EXPECT_EQ(candidate.rootNodes(), (std::vector<uint32_t>{first, second}));
  EXPECT_EQ(candidate.getNode(first)->children, (std::vector<uint32_t>{2}));
  EXPECT_EQ(candidate.renderableNodes(), (std::vector<uint32_t>{second, first, 3, 4}));
  auto proof = graph.prepareAppend(std::move(candidate)); ASSERT_TRUE(graph.publishAppend(proof));
  EXPECT_EQ(graph.renderableNodes(), (std::vector<uint32_t>{second, first, 3, 4}));
}

TEST(SceneGraph, InvalidBulkAppendCannotMutateRetainedState) {
  SceneGraph graph; graph.createNode(glm::mat4(1), 0);
  const auto before = appendSnapshot(graph);
  for (int mode = 0; mode < 5; ++mode) {
    std::vector<SceneNode> tail(2); tail[0].parent = 0; tail[0].children = {2}; tail[1].parent = 1;
    if (mode == 0) tail[0].children = {0};
    if (mode == 1) tail[0].children = {2, 2};
    if (mode == 2) tail[1].parent = 3;
    if (mode == 3) tail[1].meshOwnerGraphNode = 3;
    if (mode == 4) tail[1].worldTransform[0][0] = std::numeric_limits<float>::infinity();
    EXPECT_THROW((void)graph.makeAppendCandidate(std::move(tail)), std::invalid_argument);
    expectAppendUnchanged(graph, before);
  }
}

}  // namespace
