#include <gtest/gtest.h>

#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneEditHistory.h"

#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <array>
#include <stdexcept>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace {

using container::scene::SceneGraph;
using container::scene::SceneNode;

static_assert(!std::is_copy_constructible_v<SceneGraph::PreparedAppend>);
static_assert(std::is_nothrow_move_constructible_v<SceneGraph::PreparedAppend>);
static_assert(noexcept(std::declval<SceneGraph&>().publishAppend(
    std::declval<SceneGraph::PreparedAppend&>())));
static_assert(!std::is_copy_constructible_v<SceneGraph::PreparedTailChange>);
static_assert(std::is_nothrow_move_constructible_v<SceneGraph::PreparedTailChange>);
static_assert(std::is_nothrow_move_assignable_v<SceneGraph::PreparedTailChange>);
static_assert(noexcept(std::declval<SceneGraph&>().publishTailChange(
    std::declval<SceneGraph::PreparedTailChange&>())));
static_assert(noexcept(std::declval<SceneGraph::PreparedTailChange&>().takeDetachedTail()));
static_assert(!std::is_copy_constructible_v<SceneGraph::PreparedPlacement>);
static_assert(std::is_nothrow_move_constructible_v<SceneGraph::PreparedPlacement>);
static_assert(std::is_nothrow_move_assignable_v<SceneGraph::PreparedPlacement>);
static_assert(noexcept(std::declval<SceneGraph&>().publishPlacement(
    std::declval<SceneGraph::PreparedPlacement&>())));
static_assert(!std::is_copy_constructible_v<SceneGraph::SubtreeRetirement>);
static_assert(std::is_nothrow_move_constructible_v<SceneGraph::SubtreeRetirement>);
static_assert(!std::is_copy_constructible_v<SceneGraph::PreparedRetirement>);
static_assert(std::is_nothrow_move_constructible_v<SceneGraph::PreparedRetirement>);
static_assert(noexcept(std::declval<SceneGraph&>().publishRetirement(
    std::declval<SceneGraph::PreparedRetirement&>())));
static_assert(noexcept(std::declval<SceneGraph::PreparedRetirement&>().takeRetirement()));

std::vector<SceneNode> snapshot(const SceneGraph& graph) {
  std::vector<SceneNode> result;
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) result.push_back(*graph.getNode(id));
  return result;
}

uint32_t add(SceneGraph &graph, const char *name, bool renderable = true) {
  return graph.createNode(glm::mat4{1}, 7u, renderable, 42u, name);
}

TEST(SceneGraphEditTests, InvalidOrEmptyTailLeavesGraphUntouched) {
  SceneGraph graph;
  EXPECT_FALSE(graph.detachTail(0u));
  add(graph, "Retained");
  const auto revision = graph.revision();
  const auto hierarchy = graph.hierarchyRevision();
  EXPECT_FALSE(graph.detachTail(1u));
  EXPECT_FALSE(graph.detachTail(SceneGraph::kInvalidNode));
  EXPECT_EQ(graph.nodeCount(), 1u);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.hierarchyRevision(), hierarchy);
}

TEST(SceneGraphEditTests, EmptySceneRootAndPrimitiveRestoreWithStableIdentity) {
  SceneGraph graph;
  const auto identity = graph.identity();
  const auto root = add(graph, "Scene Root", false);
  const auto primitive = add(graph, "Cube");
  ASSERT_TRUE(graph.setParent(primitive, root));
  const auto transform = glm::translate(glm::mat4{1}, glm::vec3{2, 3, 4});
  graph.setLocalTransform(primitive, transform);
  graph.setVisible(primitive, false);
  const auto beforeRevision = graph.revision();
  const auto beforeHierarchy = graph.hierarchyRevision();

  auto tail = graph.detachTail(0u);
  ASSERT_TRUE(tail);
  EXPECT_EQ(tail->firstNode, 0u);
  ASSERT_EQ(tail->nodes.size(), 2u);
  EXPECT_EQ(graph.nodeCount(), 0u);
  EXPECT_TRUE(graph.rootNodes().empty());
  EXPECT_TRUE(graph.renderableNodes().empty());
  EXPECT_EQ(graph.getNode(primitive), nullptr);
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_GT(graph.revision(), beforeRevision);
  EXPECT_GT(graph.hierarchyRevision(), beforeHierarchy);

  ASSERT_TRUE(graph.restoreTail(*tail));
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_EQ(graph.nodeCount(), 2u);
  EXPECT_EQ(graph.rootNodes(), std::vector<uint32_t>{root});
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{primitive});
  const auto *restored = graph.getNode(primitive);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->name, "Cube");
  EXPECT_EQ(restored->materialIndex, 7u);
  EXPECT_EQ(restored->primitiveIndex, 42u);
  EXPECT_EQ(restored->localTransform, transform);
  EXPECT_EQ(restored->worldTransform, transform);
  EXPECT_FALSE(restored->visible);
  EXPECT_EQ(restored->parent, root);
  EXPECT_EQ(graph.getNode(root)->children, std::vector<uint32_t>{primitive});
  EXPECT_FALSE(graph.restoreTail(*tail));

  // Each undo captures a new snapshot, retaining the original node indices.
  auto second = graph.detachTail(0u);
  ASSERT_TRUE(second);
  ASSERT_TRUE(graph.restoreTail(*second));
  EXPECT_EQ(graph.getNode(primitive)->primitiveIndex, 42u);
  EXPECT_EQ(graph.identity(), identity);
}

TEST(SceneGraphEditTests, RestoresRootSiblingAndRenderableOrderExactly) {
  SceneGraph graph;
  const auto parent = add(graph, "Parent");
  const auto retainedSibling = add(graph, "Retained sibling");
  const auto retainedRoot = add(graph, "Retained root");
  const auto firstNode = static_cast<uint32_t>(graph.nodeCount());
  const auto first = add(graph, "First tail child");
  const auto second = add(graph, "Second tail child");
  const auto tailRoot = add(graph, "Tail root");
  ASSERT_TRUE(graph.setParent(first, parent));
  ASSERT_TRUE(graph.setParent(retainedSibling, parent));
  ASSERT_TRUE(graph.setParent(second, parent));
  // Root and renderable indices need not follow node creation order.
  ASSERT_TRUE(graph.setParent(retainedRoot, parent));
  ASSERT_TRUE(graph.setParent(retainedRoot, std::nullopt));
  graph.setRenderable(retainedSibling, false);
  graph.setRenderable(retainedSibling, true);
  const auto roots = graph.rootNodes();
  const auto children = graph.getNode(parent)->children;
  const auto renderables = graph.renderableNodes();
  ASSERT_EQ(roots, (std::vector<uint32_t>{parent, tailRoot, retainedRoot}));
  ASSERT_EQ(children, (std::vector<uint32_t>{first, retainedSibling, second}));

  auto tail = graph.detachTail(firstNode);
  ASSERT_TRUE(tail);
  EXPECT_EQ(graph.getNode(parent)->children,
            std::vector<uint32_t>{retainedSibling});
  EXPECT_EQ(graph.rootNodes(), (std::vector<uint32_t>{parent, retainedRoot}));
  ASSERT_TRUE(graph.restoreTail(*tail));
  EXPECT_EQ(graph.rootNodes(), roots);
  EXPECT_EQ(graph.getNode(parent)->children, children);
  EXPECT_EQ(graph.renderableNodes(), renderables);
}

TEST(SceneGraphEditTests, ReparentedTailUsesItsCurrentParentsAndPayload) {
  SceneGraph graph;
  const auto oldParent = add(graph, "Original parent", false);
  const auto newParent = add(graph, "New parent", false);
  const auto firstNode = static_cast<uint32_t>(graph.nodeCount());
  const auto first = add(graph, "First");
  const auto second = add(graph, "Second");
  ASSERT_TRUE(graph.setParent(first, oldParent));
  ASSERT_TRUE(graph.setParent(second, first));
  ASSERT_TRUE(graph.setParent(first, newParent));
  auto tail = graph.detachTail(firstNode);
  ASSERT_TRUE(tail);
  const auto parentTransform = glm::translate(glm::mat4{1}, glm::vec3{9, 8, 7});
  graph.setLocalTransform(newParent, parentTransform);
  graph.setVisible(oldParent, false);
  ASSERT_TRUE(graph.restoreTail(*tail));
  EXPECT_TRUE(graph.getNode(oldParent)->children.empty());
  EXPECT_EQ(graph.getNode(newParent)->children, std::vector<uint32_t>{first});
  EXPECT_EQ(graph.getNode(first)->parent, newParent);
  EXPECT_EQ(graph.getNode(second)->parent, first);
  EXPECT_EQ(graph.getNode(second)->worldTransform, parentTransform);
}

TEST(SceneGraphEditTests, RetainedDescendantRefusesDetachmentAtomically) {
  SceneGraph graph;
  const auto retained = add(graph, "Retained");
  const auto tailNode = add(graph, "Tail");
  ASSERT_TRUE(graph.setParent(retained, tailNode));
  const auto revision = graph.revision();
  const auto roots = graph.rootNodes();
  EXPECT_FALSE(graph.detachTail(tailNode));
  EXPECT_EQ(graph.nodeCount(), 2u);
  EXPECT_EQ(graph.rootNodes(), roots);
  EXPECT_EQ(graph.getNode(tailNode)->children, std::vector<uint32_t>{retained});
  EXPECT_EQ(graph.getNode(retained)->parent, tailNode);
  EXPECT_EQ(graph.revision(), revision);
}

TEST(SceneGraphEditTests, InvalidRestorationCannotPublishPartialNodes) {
  SceneGraph graph;
  const auto parent = add(graph, "Retained parent", false);
  const auto firstNode = add(graph, "Tail");
  ASSERT_TRUE(graph.setParent(firstNode, parent));
  const auto tailChild = add(graph, "Tail child");
  ASSERT_TRUE(graph.setParent(tailChild, firstNode));
  auto tail = graph.detachTail(firstNode);
  ASSERT_TRUE(tail);
  const auto revision = graph.revision();
  const auto hierarchy = graph.hierarchyRevision();
  const auto expectUnchanged = [&]() {
    EXPECT_EQ(graph.nodeCount(), firstNode);
    EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.hierarchyRevision(), hierarchy);
    EXPECT_TRUE(graph.getNode(parent)->children.empty());
  };

  auto invalidParent = *tail;
  invalidParent.nodes[0].parent = 1000u;
  EXPECT_FALSE(graph.restoreTail(invalidParent));
  expectUnchanged();

  auto invalidShape = *tail;
  invalidShape.nodes.pop_back();
  EXPECT_FALSE(graph.restoreTail(invalidShape));
  expectUnchanged();

  auto cycle = *tail;
  cycle.nodes[0].parent = tailChild;
  cycle.nodes[1].children = {firstNode};
  EXPECT_FALSE(graph.restoreTail(cycle));
  expectUnchanged();

  auto changedRenderable = *tail;
  changedRenderable.nodes[0].renderable = false;
  EXPECT_FALSE(graph.restoreTail(changedRenderable));
  expectUnchanged();
  EXPECT_TRUE(graph.restoreTail(*tail));
}

TEST(SceneGraphEditTests, ReplacementShapeAndTopologyInvalidateSnapshot) {
  SceneGraph graph;
  const auto parent = add(graph, "Parent", false);
  const auto otherParent = add(graph, "Other parent", false);
  const auto tailNode = add(graph, "Tail");
  ASSERT_TRUE(graph.setParent(tailNode, parent));
  auto tail = graph.detachTail(tailNode);
  ASSERT_TRUE(tail);
  SceneGraph replacement = graph;
  EXPECT_FALSE(replacement.restoreTail(*tail));
  EXPECT_EQ(replacement.nodeCount(), tailNode);

  add(graph, "Different appended node");
  EXPECT_FALSE(graph.restoreTail(*tail));
  auto differentTail = graph.detachTail(tailNode);
  ASSERT_TRUE(differentTail);
  // Removing a different appended tail restores exactly the retained shape.
  // An older snapshot is still safe in that state, despite newer revisions.
  ASSERT_TRUE(graph.setParent(otherParent, parent));
  EXPECT_FALSE(graph.restoreTail(*differentTail));
  EXPECT_EQ(graph.nodeCount(), tailNode);
}

TEST(SceneGraphEditTests, EarlierRetainedUndoRedoDoesNotInvalidatePrimitiveRedo) {
  SceneGraph graph;
  const auto parent = add(graph, "Parent", false);
  const auto retained = add(graph, "Retained child");
  ASSERT_TRUE(graph.setParent(retained, parent));
  const auto primitive = add(graph, "Primitive");
  ASSERT_TRUE(graph.setParent(primitive, parent));
  auto tail = graph.detachTail(primitive);
  ASSERT_TRUE(tail);
  // Undo and redo an older reparent operation while the primitive is absent.
  ASSERT_TRUE(graph.setParent(retained, std::nullopt));
  EXPECT_FALSE(graph.restoreTail(*tail));
  ASSERT_TRUE(graph.setParent(retained, parent));
  graph.setName(retained, "Renamed retained child");
  ASSERT_TRUE(graph.restoreTail(*tail));
  EXPECT_EQ(graph.getNode(parent)->children,
            (std::vector<uint32_t>{retained, primitive}));
  EXPECT_EQ(graph.getNode(retained)->name, "Renamed retained child");
}

TEST(SceneGraphEditTests, ActiveSceneViewsPreserveEditsAndExcludeContextAncestorsFromDraws) {
  SceneGraph graph;
  const auto context=add(graph,"Common root",true);
  const auto a=add(graph,"Scene A",false),b=add(graph,"Scene B",false);
  const auto meshA=add(graph,"A"),meshB=add(graph,"B");
  graph.setParent(a,context);graph.setParent(b,context);
  graph.setParent(meshA,a);graph.setParent(meshB,b);
  graph.setLocalTransform(b,glm::translate(glm::mat4(1),{3,0,0}));
  graph.updateWorldTransforms();
  const auto worldB=graph.getNode(meshB)->worldTransform;
  const auto identity=graph.identity();
  graph.setActiveRoots(std::array<uint32_t,1>{a});
  EXPECT_EQ(graph.renderableNodes(),(std::vector<uint32_t>{meshA}));
  EXPECT_TRUE(graph.isNodeInActiveHierarchy(context));EXPECT_FALSE(graph.isNodeActive(context));
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(context));
  EXPECT_FALSE(graph.isNodeActive(meshB));
  graph.setMaterialIndex(meshB,99);graph.setName(meshA,"Edited A");
  graph.setActiveRoots(std::array<uint32_t,1>{b});
  EXPECT_EQ(graph.renderableNodes(),(std::vector<uint32_t>{meshB}));
  EXPECT_EQ(graph.getNode(meshB)->materialIndex,99u);
  EXPECT_EQ(graph.getNode(meshB)->worldTransform,worldB);
  const auto revision=graph.revision();
  EXPECT_THROW(graph.setActiveRoots(std::array<uint32_t,1>{999}),std::invalid_argument);
  EXPECT_EQ(graph.revision(),revision);
  graph.setActiveRoots(std::array<uint32_t,1>{a});
  EXPECT_EQ(graph.getNode(meshA)->name,"Edited A");
  graph.setRenderable(meshA,false);EXPECT_TRUE(graph.renderableNodes().empty());
  graph.setRenderable(meshA,true);EXPECT_EQ(graph.renderableNodes().size(),1u);
  graph.setActiveRoots({});EXPECT_TRUE(graph.renderableNodes().empty());
  EXPECT_FALSE(graph.isNodeInActiveHierarchy(context));
  graph.clearActiveRoots();EXPECT_EQ(graph.renderableNodes().size(),3u);
  EXPECT_EQ(graph.identity(),identity);
}

TEST(SceneGraphEditTests, ActiveViewsSurviveCopyMoveAndTailUndoRedo) {
  SceneGraph graph;
  const auto a=add(graph,"A",false),b=add(graph,"B",true);
  graph.setActiveRoots(std::array<uint32_t,1>{a});
  const auto child=add(graph,"New primitive",true);graph.setParent(child,a);
  EXPECT_EQ(graph.renderableNodes(),(std::vector<uint32_t>{child}));
  const auto tail=graph.detachTail(child);ASSERT_TRUE(tail);
  EXPECT_TRUE(graph.renderableNodes().empty());
  ASSERT_TRUE(graph.restoreTail(*tail));
  EXPECT_EQ(graph.renderableNodes(),(std::vector<uint32_t>{child}));
  SceneGraph copy=graph;
  EXPECT_EQ(copy.renderableNodes(),(std::vector<uint32_t>{child}));
  SceneGraph moved=std::move(copy);
  EXPECT_EQ(moved.renderableNodes(),(std::vector<uint32_t>{child}));
  EXPECT_TRUE(copy.renderableNodes().empty());EXPECT_FALSE(moved.isNodeActive(b));
  moved.setActiveRoots(std::array<uint32_t,1>{b});
  EXPECT_EQ(moved.renderableNodes(),(std::vector<uint32_t>{b}));
  EXPECT_EQ(graph.renderableNodes(),(std::vector<uint32_t>{child}));
}

TEST(SceneGraphEditTests, InteractionFlagsRemainIndependentAndOnlyAdvancePayloadRevision) {
  SceneGraph graph;
  const auto parent=add(graph,"Parent",false),hit=add(graph,"Hit");
  graph.setParent(hit,parent);
  const auto hierarchy=graph.hierarchyRevision(),revision=graph.revision();
  EXPECT_TRUE(graph.getNode(hit)->selectable); EXPECT_TRUE(graph.getNode(hit)->hoverable);
  graph.setSelectable(hit,false); graph.setHoverable(hit,false);
  EXPECT_EQ(graph.revision(),revision+2); EXPECT_EQ(graph.hierarchyRevision(),hierarchy);
  EXPECT_TRUE(graph.getNode(hit)->visible);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),parent);
  EXPECT_EQ(graph.nearestHoverableAncestor(hit),parent);
  const auto changed=graph.revision();
  graph.setSelectable(hit,false); graph.setHoverable(hit,false);
  graph.setSelectable(SceneGraph::kInvalidNode,false); graph.setHoverable(999,false);
  EXPECT_EQ(graph.revision(),changed);
  graph.setSelectable(hit,true); graph.setVisible(hit,false);
  EXPECT_TRUE(graph.isNodeEffectivelySelectable(hit));
  EXPECT_FALSE(graph.isNodeEffectivelyHoverable(hit));
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(hit));
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),hit);
}

TEST(SceneGraphEditTests, InteractionRoutingUsesParentOfHighestOptOutIndependentlyForEachFlag) {
  SceneGraph graph;
  const auto root=add(graph,"Root",false),allowed=add(graph,"Allowed",false);
  const auto blocked=add(graph,"Blocked",false),leaf=add(graph,"Leaf",false),hit=add(graph,"Primitive");
  graph.setParent(allowed,root); graph.setParent(blocked,allowed);
  graph.setParent(leaf,blocked); graph.setParent(hit,leaf);
  graph.setSelectable(blocked,false); graph.setSelectable(leaf,false);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),allowed);
  EXPECT_FALSE(graph.isNodeEffectivelySelectable(hit));
  EXPECT_TRUE(graph.isNodeEffectivelySelectable(allowed));
  EXPECT_EQ(graph.nearestHoverableAncestor(hit),hit);
  graph.setHoverable(allowed,false); graph.setHoverable(leaf,false);
  EXPECT_EQ(graph.nearestHoverableAncestor(hit),root);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),allowed);
  graph.setSelectable(root,false);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),SceneGraph::kInvalidNode);
  EXPECT_FALSE(graph.isNodeEffectivelySelectable(root));
  graph.setSelectable(root,true); graph.setSelectable(blocked,true);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),blocked);
}

TEST(SceneGraphEditTests, BlockedSourceRootsNeverRedirectIntoSyntheticImportOrEditorRoots) {
  SceneGraph graph;
  const auto editor=add(graph,"Editor root",false),import=add(graph,"Renamed import",false);
  graph.getNode(editor)->editorSceneRoot=true;
  graph.getNode(import)->interactionRootBoundary=true;
  const auto source=add(graph,"Source root",false),hit=add(graph,"Primitive");
  graph.getNode(source)->sourceNodeIndex=0;
  graph.setParent(import,editor); graph.setParent(source,import); graph.setParent(hit,source);
  graph.setSelectable(source,false); graph.setHoverable(source,false);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.nearestHoverableAncestor(hit),SceneGraph::kInvalidNode);
  graph.setParent(source,editor);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),SceneGraph::kInvalidNode);
  const auto authored=add(graph,"Import Scale",false); // Names do not establish a boundary.
  graph.setParent(source,authored);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),authored);
  EXPECT_EQ(graph.nearestHoverableAncestor(hit),authored);
  graph.setSelectable(source,true); graph.setHoverable(source,true);
  graph.setParent(source,import);
  EXPECT_EQ(graph.nearestSelectableAncestor(hit),hit);
  EXPECT_EQ(graph.nearestHoverableAncestor(hit),hit);
}

TEST(SceneGraphEditTests, InteractionRoutingRejectsInactiveHitsAndContextOnlyRedirectTargets) {
  SceneGraph graph;
  const auto context=add(graph,"Context",false),a=add(graph,"A",false),b=add(graph,"B",false);
  const auto hitA=add(graph,"A primitive"),hitB=add(graph,"B primitive");
  graph.setParent(a,context); graph.setParent(b,context);
  graph.setParent(hitA,a); graph.setParent(hitB,b);
  graph.setActiveRoots(std::array<uint32_t,1>{a});
  EXPECT_EQ(graph.nearestSelectableAncestor(hitB),SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.nearestHoverableAncestor(hitB),SceneGraph::kInvalidNode);
  graph.setSelectable(a,false); graph.setHoverable(a,false);
  EXPECT_TRUE(graph.isNodeInActiveHierarchy(context)); EXPECT_FALSE(graph.isNodeActive(context));
  EXPECT_EQ(graph.nearestSelectableAncestor(hitA),SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.nearestHoverableAncestor(hitA),SceneGraph::kInvalidNode);
  graph.setSelectable(a,true); graph.setHoverable(a,true);
  graph.setSelectable(hitA,false); graph.setHoverable(hitA,false);
  EXPECT_EQ(graph.nearestSelectableAncestor(hitA),a);
  EXPECT_EQ(graph.nearestHoverableAncestor(hitA),a);
  graph.setActiveRoots(std::array<uint32_t,1>{b});
  EXPECT_EQ(graph.nearestSelectableAncestor(hitB),hitB);
  EXPECT_EQ(graph.nearestHoverableAncestor(hitA),SceneGraph::kInvalidNode);
}

TEST(SceneGraphEditTests, InteractionFlagsAndBoundariesSurviveCopyMoveAndTailUndoRedo) {
  SceneGraph graph;
  const auto retained=add(graph,"Retained",false);
  graph.getNode(retained)->interactionRootBoundary=true;
  const auto source=add(graph,"Source",false),hit=add(graph,"Hit");
  graph.setParent(source,retained); graph.setParent(hit,source);
  graph.getNode(source)->sourceNodeIndex=7;
  graph.setSelectable(source,false); graph.setHoverable(hit,false);
  SceneGraph copy=graph;
  EXPECT_FALSE(copy.getNode(source)->selectable); EXPECT_FALSE(copy.getNode(hit)->hoverable);
  EXPECT_TRUE(copy.getNode(retained)->interactionRootBoundary);
  SceneGraph moved=std::move(copy);
  EXPECT_EQ(moved.nearestSelectableAncestor(hit),SceneGraph::kInvalidNode);
  EXPECT_EQ(moved.nearestHoverableAncestor(hit),source);
  const auto tail=graph.detachTail(source); ASSERT_TRUE(tail);
  graph.setHoverable(retained,false); // Retained payload edits do not invalidate the tail.
  ASSERT_TRUE(graph.restoreTail(*tail));
  EXPECT_FALSE(graph.getNode(source)->selectable); EXPECT_TRUE(graph.getNode(source)->hoverable);
  EXPECT_TRUE(graph.getNode(hit)->selectable); EXPECT_FALSE(graph.getNode(hit)->hoverable);
  EXPECT_FALSE(graph.getNode(retained)->hoverable);
  EXPECT_TRUE(graph.getNode(retained)->interactionRootBoundary);
  EXPECT_EQ(graph.getNode(source)->sourceNodeIndex,7);
}

TEST(SceneGraphEditTests, InteractionRoutingIsBoundedForDeepOrMalformedLegacyParentChains) {
  SceneGraph graph;
  uint32_t parent=SceneGraph::kInvalidNode;
  uint32_t root=0,blocker=0;
  for (uint32_t i=0;i<4096;++i) {
    const auto node=add(graph,"Deep",false);
    if(parent!=SceneGraph::kInvalidNode) graph.setParent(node,parent);
    if(i==0) root=node;
    if(i==1) blocker=node;
    parent=node;
  }
  graph.setSelectable(blocker,false);
  EXPECT_EQ(graph.nearestSelectableAncestor(parent),root);
  EXPECT_EQ(graph.nearestHoverableAncestor(parent),parent);
  graph.getNode(root)->parent=parent;
  EXPECT_EQ(graph.nearestSelectableAncestor(parent),SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.nearestHoverableAncestor(parent),SceneGraph::kInvalidNode);
  graph.getNode(root)->parent=999999;
  EXPECT_EQ(graph.nearestSelectableAncestor(parent),SceneGraph::kInvalidNode);
  EXPECT_FALSE(graph.isNodeEffectivelySelectable(SceneGraph::kInvalidNode));
  EXPECT_FALSE(graph.isNodeEffectivelyHoverable(999999));
}

TEST(SceneGraphEditTests, PreparedTailPublicationIsSingleUseAndRefusesStaleOrForeignProofs) {
  SceneGraph graph;
  const auto root = add(graph, "Retained", false), child = add(graph, "Tail");
  ASSERT_TRUE(graph.setParent(child, root));
  const auto before = snapshot(graph); const auto identity = graph.identity();
  auto prepared = graph.prepareDetachTail(child);
  ASSERT_TRUE(prepared); ASSERT_NE(prepared.candidateGraph(), nullptr);
  EXPECT_EQ(prepared.candidateGraph()->nodeCount(), 1u);
  EXPECT_EQ(snapshot(graph), before);
  EXPECT_FALSE(prepared.takeDetachedTail());
  SceneGraph foreign(graph);
  EXPECT_FALSE(foreign.publishTailChange(prepared));
  graph.setVisible(root, false);
  const auto edited = snapshot(graph);
  EXPECT_FALSE(graph.publishTailChange(prepared)); EXPECT_EQ(snapshot(graph), edited);
  prepared = graph.prepareDetachTail(child);
  SceneGraph::PreparedTailChange moved(std::move(prepared));
  EXPECT_FALSE(prepared); ASSERT_TRUE(graph.publishTailChange(moved));
  EXPECT_FALSE(graph.publishTailChange(moved)); EXPECT_EQ(moved.candidateGraph(), nullptr);
  auto receipt = moved.takeDetachedTail(); ASSERT_TRUE(receipt);
  EXPECT_FALSE(moved.takeDetachedTail()); EXPECT_EQ(graph.identity(), identity);
  auto redo = graph.prepareRestoreTail(*receipt); ASSERT_TRUE(redo);
  EXPECT_EQ(redo.candidateGraph()->getNode(child)->parent, root);
  EXPECT_EQ(graph.getNode(child), nullptr);
  graph.setName(root, "Edited after preparation");
  EXPECT_FALSE(graph.publishTailChange(redo));
  redo = graph.prepareRestoreTail(*receipt); ASSERT_TRUE(graph.publishTailChange(redo));
  EXPECT_EQ(graph.getNode(root)->name, "Edited after preparation");
  EXPECT_FALSE(redo.takeDetachedTail()); EXPECT_EQ(graph.identity(), identity);
}

TEST(SceneGraphEditTests, PreparedTailEvaluatesStaleRetainedWorldsBeforeEitherPublication) {
  SceneGraph graph;
  const auto root = add(graph, "Root", false), retained = add(graph, "Retained");
  const auto tail = add(graph, "Tail");
  graph.setParent(retained, root); graph.setParent(tail, root);
  graph.setLocalTransform(retained, glm::translate(glm::mat4(1), {2, 0, 0}));
  graph.setLocalTransform(tail, glm::translate(glm::mat4(1), {3, 0, 0}));
  graph.updateWorldTransforms();
  graph.setLocalTransform(root, glm::translate(glm::mat4(1), {7, 0, 0}));
  const auto before = snapshot(graph);
  auto detach = graph.prepareDetachTail(tail); ASSERT_TRUE(detach);
  EXPECT_EQ(snapshot(graph), before);
  EXPECT_FLOAT_EQ(detach.candidateGraph()->getNode(root)->worldTransform[3][0], 7);
  EXPECT_FLOAT_EQ(detach.candidateGraph()->getNode(retained)->worldTransform[3][0], 9);
  ASSERT_TRUE(graph.publishTailChange(detach));
  auto receipt = detach.takeDetachedTail(); ASSERT_TRUE(receipt);
  EXPECT_FLOAT_EQ(graph.getNode(retained)->worldTransform[3][0], 9);
  graph.setLocalTransform(root, glm::translate(glm::mat4(1), {11, 0, 0}));
  auto restore = graph.prepareRestoreTail(*receipt); ASSERT_TRUE(restore);
  EXPECT_FLOAT_EQ(graph.getNode(retained)->worldTransform[3][0], 9);
  EXPECT_FLOAT_EQ(restore.candidateGraph()->getNode(retained)->worldTransform[3][0], 13);
  EXPECT_FLOAT_EQ(restore.candidateGraph()->getNode(tail)->worldTransform[3][0], 14);
  ASSERT_TRUE(graph.publishTailChange(restore));
  EXPECT_FLOAT_EQ(graph.getNode(retained)->worldTransform[3][0], 13);
  EXPECT_FLOAT_EQ(graph.getNode(tail)->worldTransform[3][0], 14);
}

TEST(SceneGraphEditTests, TailHistoryCapturedPosesRestoreRetainedManualAndAnimatedLocalsAndTailWorlds) {
  using container::scene::SceneNodeLocalPose;
  SceneGraph graph;
  const auto root = add(graph, "Root", false), manual = add(graph, "Manual shear", false);
  const auto animated = add(graph, "Source owner");
  graph.setParent(manual, root); graph.setParent(animated, manual);
  graph.getNode(animated)->sourceNodeIndex = 7;
  auto shear = glm::scale(glm::mat4(1), {-2, 3, .5f}); shear[1][0] = .25f;
  const std::vector<SceneNodeLocalPose> undoPose{{root, glm::translate(glm::mat4(1), {1, 2, 0})},
      {manual, shear}, {animated, glm::translate(glm::mat4(1), {2, 0, 0})}};
  const auto tail = add(graph, "New source", false), leaf = add(graph, "New manual child");
  graph.setParent(tail, root); graph.setParent(leaf, tail); graph.getNode(tail)->sourceNodeIndex = 8;
  const std::vector<SceneNodeLocalPose> redoPose{{root, glm::translate(glm::mat4(1), {5, 2, 0})},
      {manual, shear}, {animated, glm::translate(glm::mat4(1), {4, 0, 0})},
      {tail, glm::translate(glm::mat4(1), {0, 0, 6})}, {leaf, glm::scale(glm::mat4(1), {1, -2, 3})}};
  graph.setActiveRoots(std::array{root}); const auto view = graph.activeRoots();
  // Simulate animation playback and independent editor changes after addition.
  for (uint32_t id = 0; id < graph.nodeCount(); ++id)
    graph.setLocalTransform(id, glm::translate(glm::mat4(1), {float(20 + id), 0, 0}));
  graph.updateWorldTransforms(); const auto live = snapshot(graph); const auto identity = graph.identity();
  auto detach = graph.prepareDetachTail(tail, view, undoPose); ASSERT_TRUE(detach);
  EXPECT_EQ(snapshot(graph), live); const auto* before = detach.candidateGraph(); ASSERT_NE(before, nullptr);
  for (const auto& pose : undoPose) EXPECT_EQ(before->getNode(pose.node)->localTransform, pose.localTransform);
  EXPECT_EQ(before->getNode(animated)->worldTransform,
      undoPose[0].localTransform * undoPose[1].localTransform * undoPose[2].localTransform);
  ASSERT_TRUE(graph.publishTailChange(detach)); auto receipt = detach.takeDetachedTail(); ASSERT_TRUE(receipt);
  EXPECT_EQ(graph.identity(), identity); EXPECT_EQ(graph.activeRoots(), view);
  EXPECT_EQ(graph.getNode(animated)->sourceNodeIndex, 7);
  EXPECT_NE(receipt->nodes[0].localTransform, redoPose[tail].localTransform);
  graph.setLocalTransform(manual, glm::translate(glm::mat4(1), {100, 0, 0}));
  graph.setLocalTransform(animated, glm::translate(glm::mat4(1), {200, 0, 0}));
  const auto undone = snapshot(graph);
  auto restore = graph.prepareRestoreTail(*receipt, redoPose); ASSERT_TRUE(restore);
  EXPECT_EQ(snapshot(graph), undone); const auto* after = restore.candidateGraph(); ASSERT_NE(after, nullptr);
  for (const auto& pose : redoPose) EXPECT_EQ(after->getNode(pose.node)->localTransform, pose.localTransform);
  EXPECT_EQ(after->getNode(animated)->worldTransform,
      redoPose[0].localTransform * redoPose[1].localTransform * redoPose[2].localTransform);
  EXPECT_EQ(after->getNode(leaf)->worldTransform,
      redoPose[0].localTransform * redoPose[tail].localTransform * redoPose[leaf].localTransform);
  ASSERT_TRUE(graph.publishTailChange(restore)); EXPECT_EQ(graph.identity(), identity);
  EXPECT_EQ(graph.activeRoots(), view); EXPECT_EQ(graph.getNode(manual)->name, "Manual shear");
  EXPECT_EQ(graph.getNode(tail)->sourceNodeIndex, 8);
  for (const auto& pose : redoPose) EXPECT_EQ(graph.getNode(pose.node)->localTransform, pose.localTransform);
}

TEST(SceneGraphEditTests, TailPoseSnapshotsRejectPartialUnorderedDuplicateAndNonfiniteDataWithoutPublication) {
  using container::scene::SceneNodeLocalPose;
  SceneGraph graph;
  const auto root = add(graph, "Root", false), child = add(graph, "Retained");
  const auto tail = add(graph, "Tail", false), leaf = add(graph, "Leaf");
  graph.setParent(child, root); graph.setParent(tail, root); graph.setParent(leaf, tail);
  graph.setActiveRoots(std::array{root}); graph.updateWorldTransforms();
  const auto view = graph.activeRoots();
  const auto poses = [](uint32_t count) {
    std::vector<SceneNodeLocalPose> result;
    for (uint32_t id = 0; id < count; ++id) result.push_back({id, glm::mat4(1)});
    return result;
  };
  const auto corrupt = [](auto& pose, int mutation) {
    if (mutation == 0) pose.pop_back();
    if (mutation == 1) pose.push_back({uint32_t(pose.size()), glm::mat4(1)});
    if (mutation == 2) pose[1].node = 0;
    if (mutation == 3) std::swap(pose[0], pose[1]);
    if (mutation == 4) pose[0].node = SceneGraph::kInvalidNode;
    if (mutation == 5) pose[1].localTransform[0][0] = std::numeric_limits<float>::quiet_NaN();
    if (mutation == 6) pose[0].localTransform[3][0] = std::numeric_limits<float>::infinity();
    if (mutation == 7) {
      pose[0].localTransform = glm::scale(glm::mat4(1), {1e30f, 1, 1});
      pose[1].localTransform = glm::scale(glm::mat4(1), {1e30f, 1, 1});
    }
  };
  const auto before = snapshot(graph); const auto revision = graph.revision(), hierarchy = graph.hierarchyRevision();
  for (int mutation = 0; mutation < 8; ++mutation) {
    SCOPED_TRACE(mutation); auto invalid = poses(tail); corrupt(invalid, mutation);
    EXPECT_FALSE(graph.prepareDetachTail(tail, view, invalid));
    EXPECT_EQ(snapshot(graph), before); EXPECT_EQ(graph.revision(), revision);
    EXPECT_EQ(graph.hierarchyRevision(), hierarchy); EXPECT_EQ(graph.activeRoots(), view);
  }
  auto detach = graph.prepareDetachTail(tail, view, poses(tail)); ASSERT_TRUE(graph.publishTailChange(detach));
  auto receipt = detach.takeDetachedTail(); ASSERT_TRUE(receipt);
  const auto undone = snapshot(graph); const auto undoRevision = graph.revision(), undoHierarchy = graph.hierarchyRevision();
  const auto roots = graph.rootNodes(), renderables = graph.renderableNodes();
  for (int mutation = 0; mutation < 8; ++mutation) {
    SCOPED_TRACE(mutation); auto invalid = poses(leaf + 1); corrupt(invalid, mutation);
    EXPECT_FALSE(graph.prepareRestoreTail(*receipt, invalid));
    EXPECT_EQ(snapshot(graph), undone); EXPECT_EQ(graph.revision(), undoRevision);
    EXPECT_EQ(graph.hierarchyRevision(), undoHierarchy); EXPECT_EQ(graph.activeRoots(), view);
    EXPECT_EQ(graph.rootNodes(), roots); EXPECT_EQ(graph.renderableNodes(), renderables);
  }
  auto restore = graph.prepareRestoreTail(*receipt, poses(leaf + 1)); ASSERT_TRUE(graph.publishTailChange(restore));
}

TEST(SceneGraphEditTests, CapturedTailPoseProofOwnsPreparedLocalsAndStillRejectsStalePublication) {
  using container::scene::SceneNodeLocalPose;
  SceneGraph graph; const auto root = add(graph, "Root", false), tail = add(graph, "Tail");
  graph.setParent(tail, root); graph.updateWorldTransforms(); const auto view = graph.activeRoots();
  std::vector<SceneNodeLocalPose> undoPose{{root, glm::translate(glm::mat4(1), {3, 0, 0})}};
  auto detach = graph.prepareDetachTail(tail, view, undoPose); ASSERT_TRUE(detach);
  undoPose[0].localTransform = glm::translate(glm::mat4(1), {99, 0, 0});
  EXPECT_FLOAT_EQ(detach.candidateGraph()->getNode(root)->localTransform[3][0], 3);
  graph.setLocalTransform(root, glm::translate(glm::mat4(1), {8, 0, 0})); const auto changed = snapshot(graph);
  EXPECT_FALSE(graph.publishTailChange(detach)); EXPECT_EQ(snapshot(graph), changed);
  detach = graph.prepareDetachTail(tail, view, undoPose); ASSERT_TRUE(graph.publishTailChange(detach));
  EXPECT_FALSE(graph.publishTailChange(detach)); auto receipt = detach.takeDetachedTail(); ASSERT_TRUE(receipt);
  std::vector<SceneNodeLocalPose> redoPose{{root, glm::translate(glm::mat4(1), {5, 0, 0})},
      {tail, glm::translate(glm::mat4(1), {2, 0, 0})}};
  auto restore = graph.prepareRestoreTail(*receipt, redoPose); ASSERT_TRUE(restore);
  redoPose[1].localTransform = glm::translate(glm::mat4(1), {77, 0, 0});
  EXPECT_FLOAT_EQ(restore.candidateGraph()->getNode(tail)->worldTransform[3][0], 7);
  graph.setName(root, "Later edit"); const auto edited = snapshot(graph);
  EXPECT_FALSE(graph.publishTailChange(restore)); EXPECT_EQ(snapshot(graph), edited);
  restore = graph.prepareRestoreTail(*receipt, redoPose); ASSERT_TRUE(graph.publishTailChange(restore));
  EXPECT_FALSE(graph.publishTailChange(restore)); EXPECT_EQ(graph.getNode(root)->name, "Later edit");
  EXPECT_FLOAT_EQ(graph.getNode(tail)->worldTransform[3][0], 82);
}

TEST(SceneGraphEditTests, MergeTailHistoryRestoresExactViewsAndPreservesEarlierCommands) {
  for (const bool explicitView : {false, true}) {
    SCOPED_TRACE(explicitView);
    SceneGraph graph;
    const auto editor = add(graph, "Editor", false), oldMesh = add(graph, "Old mesh");
    graph.getNode(editor)->editorSceneRoot = true;
    ASSERT_TRUE(graph.setParent(oldMesh, editor));
    if (explicitView) graph.setActiveRoots(std::array{editor});
    const auto undoView = graph.activeRoots(); const auto identity = graph.identity();
    container::util::SceneEditHistory history(graph);
    ASSERT_TRUE(history.setVisible(oldMesh, false));
    const auto first = uint32_t(graph.nodeCount());
    std::vector<SceneNode> tail(4);
    tail[0].parent = editor; tail[0].children = {first + 1, first + 2};
    tail[1].parent = first; tail[1].renderable = true; tail[1].sourceNodeIndex = 1;
    tail[2].parent = first; tail[2].children = {first + 3}; tail[2].sourceNodeIndex = 2;
    tail[3].parent = first + 2; tail[3].renderable = true;
    auto candidate = graph.makeAppendCandidate(std::move(tail));
    candidate.setActiveRoots(std::array{oldMesh, first + 2});
    const auto redoView = candidate.activeRoots();
    auto append = graph.prepareAppend(std::move(candidate)); ASSERT_TRUE(graph.publishAppend(append));
    std::optional<SceneGraph::DetachedTail> receipt;
    ASSERT_TRUE(history.recordApplied({"Merge",
      [&] {
        auto change = graph.prepareDetachTail(first, undoView);
        if (!graph.publishTailChange(change)) return false;
        receipt = change.takeDetachedTail(); return receipt.has_value();
      },
      [&] {
        if (!receipt) return false;
        auto change = graph.prepareRestoreTail(*receipt);
        return graph.publishTailChange(change);
      }}));
    ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.activeRoots(), undoView);
    EXPECT_EQ(graph.nodeCount(), first); EXPECT_EQ(graph.identity(), identity);
    ASSERT_TRUE(history.undo()); EXPECT_TRUE(graph.getNode(oldMesh)->visible);
    ASSERT_TRUE(history.redo()); EXPECT_FALSE(graph.getNode(oldMesh)->visible);
    ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.activeRoots(), redoView);
    EXPECT_FALSE(graph.getNode(oldMesh)->visible); EXPECT_EQ(graph.identity(), identity);
    EXPECT_TRUE(graph.isNodeInActiveHierarchy(editor)); EXPECT_FALSE(graph.isNodeActive(editor));
    EXPECT_FALSE(graph.isNodeActive(first + 1)); EXPECT_TRUE(graph.isNodeActive(first + 3));
    ASSERT_TRUE(history.undo()); EXPECT_EQ(graph.activeRoots(), undoView);
    ASSERT_TRUE(history.redo()); EXPECT_EQ(graph.activeRoots(), redoView);
    EXPECT_EQ(history.undoCount(), 2u);
  }
}

TEST(SceneGraphEditTests, ExplicitTailUndoViewCannotActivateOrDropRetainedContent) {
  SceneGraph graph;
  const auto active = add(graph, "Active"), inactive = add(graph, "Inactive");
  graph.getNode(inactive)->sourceNodeIndex = 3;
  const auto child = add(graph, "Tail"); graph.setParent(child, active);
  graph.setActiveRoots(std::array{active});
  const auto before = snapshot(graph); const auto view = graph.activeRoots();
  const auto revision = graph.revision();
  EXPECT_FALSE(graph.prepareDetachTail(child, std::nullopt));
  EXPECT_FALSE(graph.prepareDetachTail(child, std::vector<uint32_t>{inactive}));
  EXPECT_FALSE(graph.prepareDetachTail(child, std::vector<uint32_t>{active, child}));
  EXPECT_EQ(snapshot(graph), before); EXPECT_EQ(graph.activeRoots(), view);
  EXPECT_EQ(graph.revision(), revision);
  auto change = graph.prepareDetachTail(child, view); ASSERT_TRUE(graph.publishTailChange(change));
  auto receipt = change.takeDetachedTail(); ASSERT_TRUE(receipt);
  graph.setActiveRoots(std::array{inactive});
  const auto changed = snapshot(graph); const auto changedView = graph.activeRoots();
  EXPECT_FALSE(graph.prepareRestoreTail(*receipt));
  EXPECT_EQ(snapshot(graph), changed); EXPECT_EQ(graph.activeRoots(), changedView);
}

TEST(SceneGraphEditTests, RetiredActiveRootsAreRemovedAndRestoredExactly) {
  SceneGraph graph;
  const auto oldRoot = add(graph, "Old", false), tailRoot = add(graph, "Tail root");
  graph.setActiveRoots(std::array{tailRoot, oldRoot, tailRoot});
  const auto view = graph.activeRoots();
  auto receipt = graph.detachTail(tailRoot); ASSERT_TRUE(receipt);
  ASSERT_TRUE(graph.activeRoots());
  EXPECT_EQ(*graph.activeRoots(), (std::vector<uint32_t>{oldRoot}));
  ASSERT_TRUE(graph.restoreTail(*receipt)); EXPECT_EQ(graph.activeRoots(), view);
}

TEST(SceneGraphEditTests, WideDeepAndRootTailsUseLinearPreparedAssembly) {
  for (const int shape : {0, 1, 2}) {
    SCOPED_TRACE(shape);
    const uint32_t count = shape == 1 ? 10001u : 20001u;
    SceneGraph graph; const auto root = add(graph, "Retained", false);
    std::vector<SceneNode> tail(count);
    for (uint32_t i = 0; i < count; ++i) {
      tail[i].localTransform = glm::translate(glm::mat4(1), {1, 0, 0});
      tail[i].renderable = shape != 1;
      if (shape != 2) tail[i].parent = shape == 1 && i ? i : root;
      if (shape == 1 && i + 1 < count) tail[i].children = {i + 2};
    }
    auto candidate = graph.makeAppendCandidate(std::move(tail));
    auto append = graph.prepareAppend(std::move(candidate)); ASSERT_TRUE(graph.publishAppend(append));
    const auto roots = graph.rootNodes(), renderables = graph.renderableNodes();
    auto change = graph.prepareDetachTail(1); ASSERT_TRUE(change);
    EXPECT_EQ(graph.nodeCount(), size_t(count) + 1);
    ASSERT_TRUE(graph.publishTailChange(change)); auto receipt = change.takeDetachedTail(); ASSERT_TRUE(receipt);
    EXPECT_EQ(graph.nodeCount(), 1u); EXPECT_TRUE(graph.getNode(root)->children.empty());
    auto restore = graph.prepareRestoreTail(*receipt); ASSERT_TRUE(restore);
    ASSERT_TRUE(graph.publishTailChange(restore));
    EXPECT_EQ(graph.rootNodes(), roots); EXPECT_EQ(graph.renderableNodes(), renderables);
    EXPECT_FLOAT_EQ(graph.getNode(count)->worldTransform[3][0], shape == 1 ? float(count) : 1.0f);
    if (shape == 0) EXPECT_EQ(graph.getNode(root)->children.size(), count);
    if (shape == 2) EXPECT_EQ(graph.rootNodes().size(), size_t(count) + 1);
  }
}

TEST(SceneGraphEditTests, PreparedTailRejectsMalformedTopologyOwnersAndRestoredPayloadWithoutMutation) {
  SceneGraph graph;
  const auto root = add(graph, "Retained", false), first = add(graph, "Tail", false);
  const auto child = add(graph, "Child"); graph.setParent(first, root); graph.setParent(child, first);
  graph.getNode(root)->meshOwnerGraphNode = first;
  const auto badOwner = snapshot(graph);
  EXPECT_FALSE(graph.prepareDetachTail(first)); EXPECT_EQ(snapshot(graph), badOwner);
  graph.getNode(root)->meshOwnerGraphNode = SceneGraph::kInvalidNode;
  graph.getNode(first)->children.push_back(child);
  const auto duplicate = snapshot(graph);
  EXPECT_FALSE(graph.prepareDetachTail(first)); EXPECT_EQ(snapshot(graph), duplicate);
  graph.getNode(first)->children.pop_back();
  auto receipt = graph.detachTail(first); ASSERT_TRUE(receipt);
  const auto before = snapshot(graph); const auto revision = graph.revision();
  for (int malformed = 0; malformed < 6; ++malformed) {
    SCOPED_TRACE(malformed); auto bad = *receipt;
    if (malformed == 0) bad.nodes[0].children.push_back(child);
    if (malformed == 1) { bad.nodes[0].parent = child; bad.nodes[1].children = {first}; }
    if (malformed == 2) bad.nodes[1].parent = SceneGraph::kInvalidNode;
    if (malformed == 3) bad.nodes[0].localTransform[0][0] = std::numeric_limits<float>::infinity();
    if (malformed == 4) bad.nodes[0].sourceNodeIndex = -2;
    if (malformed == 5) bad.nodes[0].meshOwnerGraphNode = 999999;
    EXPECT_FALSE(graph.prepareRestoreTail(bad)); EXPECT_EQ(snapshot(graph), before);
    EXPECT_EQ(graph.revision(), revision);
  }
  auto overflow = *receipt; overflow.nodes[0].localTransform[0][0] = 1e30f;
  graph.setLocalTransform(root, glm::scale(glm::mat4(1), {1e10f, 1, 1}));
  const auto edited = snapshot(graph); const auto editedRevision = graph.revision();
  EXPECT_FALSE(graph.prepareRestoreTail(overflow)); EXPECT_EQ(snapshot(graph), edited);
  EXPECT_EQ(graph.revision(), editedRevision);
  ASSERT_TRUE(graph.restoreTail(*receipt));
}

TEST(SceneGraphEditTests, PreparedEmptyRootHasNoSourceGeometryAndPublishesOnlyOnce) {
  SceneGraph graph;
  const auto identity = graph.identity(), revision = graph.revision();
  auto candidate = graph.makeEmptyNodeCandidate(std::nullopt);
  EXPECT_EQ(graph.nodeCount(), 0u);
  EXPECT_EQ(graph.revision(), revision);
  ASSERT_EQ(candidate.nodeCount(), 1u);
  const auto* node = candidate.getNode(0);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->name, "Empty");
  EXPECT_EQ(node->parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(node->localTransform, glm::mat4(1));
  EXPECT_EQ(node->worldTransform, glm::mat4(1));
  EXPECT_EQ(node->sourceNodeIndex, -1);
  EXPECT_EQ(node->sourceMeshIndex, -1);
  EXPECT_EQ(node->sourcePrimitiveIndex, -1);
  EXPECT_EQ(node->sourceInstanceIndex, -1);
  EXPECT_EQ(node->meshOwnerGraphNode, SceneGraph::kInvalidNode);
  EXPECT_EQ(node->primitiveIndex, SceneGraph::kInvalidNode);
  EXPECT_FALSE(node->renderable);
  EXPECT_TRUE(node->visible);
  EXPECT_TRUE(node->selectable);
  EXPECT_TRUE(node->hoverable);
  EXPECT_FALSE(node->editorSceneRoot);
  EXPECT_FALSE(node->interactionRootBoundary);
  EXPECT_FALSE(candidate.activeRoots());
  EXPECT_TRUE(candidate.renderableNodes().empty());

  SceneGraph::PreparedAppend empty;
  EXPECT_FALSE(empty);
  EXPECT_EQ(empty.candidateGraph(), nullptr);
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(prepared);
  ASSERT_NE(prepared.candidateGraph(), nullptr);
  SceneGraph::PreparedAppend moved(std::move(prepared));
  EXPECT_FALSE(prepared);
  EXPECT_EQ(prepared.candidateGraph(), nullptr);
  ASSERT_TRUE(graph.publishAppend(moved));
  EXPECT_FALSE(moved);
  EXPECT_EQ(moved.candidateGraph(), nullptr);
  EXPECT_FALSE(graph.publishAppend(moved));
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_GT(graph.revision(), revision);
  EXPECT_EQ(graph.rootNodes(), std::vector<uint32_t>{0});
}

TEST(SceneGraphEditTests, EmptyChildPreservesEveryRetainedPayloadAndEvaluatesCurrentLocalAncestors) {
  SceneGraph graph;
  const auto root = add(graph, "Root", false), parent = add(graph, "Parent", false);
  const auto sibling = add(graph, "Existing primitive");
  ASSERT_TRUE(graph.setParent(parent, root));
  ASSERT_TRUE(graph.setParent(sibling, parent));
  graph.getNode(parent)->sourceNodeIndex = 7;
  graph.setSelectable(parent, false);
  graph.setHoverable(parent, false);
  graph.setVisible(parent, false);
  const auto rootLocal = glm::translate(glm::mat4(1), {2, 3, 4}) *
      glm::rotate(glm::mat4(1), .4f, {0, 1, 0});
  const auto parentLocal = glm::scale(glm::mat4(1), {-2, 3, .5f});
  graph.setLocalTransform(root, rootLocal);
  graph.setLocalTransform(parent, parentLocal);
  // Deliberately leave old cached worlds stale. Creation must not rewrite any
  // retained payload merely to calculate the new identity child's world.
  const auto before = snapshot(graph);
  const auto roots = graph.rootNodes();
  const auto renderables = graph.renderableNodes();
  const auto revision = graph.revision();
  const auto newNode = uint32_t(graph.nodeCount());
  auto candidate = graph.makeEmptyNodeCandidate(parent, "New child");
  EXPECT_EQ(snapshot(graph), before);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(candidate.rootNodes(), roots);
  EXPECT_EQ(candidate.renderableNodes(), renderables);
  for (uint32_t id = 0; id < newNode; ++id) {
    auto expected = before[id];
    if (id == parent) expected.children.push_back(newNode);
    EXPECT_EQ(*candidate.getNode(id), expected);
  }
  EXPECT_EQ(candidate.getNode(newNode)->parent, parent);
  EXPECT_EQ(candidate.getNode(newNode)->localTransform, glm::mat4(1));
  EXPECT_EQ(candidate.getNode(newNode)->worldTransform, rootLocal * parentLocal);
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.getNode(parent)->children, (std::vector<uint32_t>{sibling, newNode}));
  EXPECT_EQ(graph.getNode(newNode)->worldTransform, rootLocal * parentLocal);
  EXPECT_EQ(graph.getNode(root)->worldTransform, before[root].worldTransform);
}

TEST(SceneGraphEditTests, EmptyNodesExtendExactSceneViewWithoutActivatingContextSiblings) {
  for (const int mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(mode);
    SceneGraph graph;
    const auto context = add(graph, "Editor", false), active = add(graph, "Active");
    const auto inactive = add(graph, "Inactive");
    ASSERT_TRUE(graph.setParent(active, context));
    ASSERT_TRUE(graph.setParent(inactive, context));
    if (mode != 0) graph.setActiveRoots(std::array{active, active});
    if (mode == 3) graph.setActiveRoots(std::span<const uint32_t>{});
    const auto oldView = graph.activeRoots();
    const auto oldRenderables = graph.renderableNodes();
    const auto first = uint32_t(graph.nodeCount());
    const std::optional<uint32_t> parent = mode == 0 || mode == 3 ? std::nullopt :
        std::optional<uint32_t>(mode == 1 ? active : context);
    auto candidate = graph.makeEmptyNodeCandidate(parent, "Empty in scene");
    if (mode == 0) EXPECT_FALSE(candidate.activeRoots());
    else {
      auto expected = *oldView;
      if (mode != 1) expected.push_back(first);
      ASSERT_TRUE(candidate.activeRoots());
      EXPECT_EQ(*candidate.activeRoots(), expected);
    }
    EXPECT_EQ(candidate.renderableNodes(), oldRenderables);
    EXPECT_TRUE(candidate.isNodeActive(first));
    EXPECT_EQ(candidate.isNodeActive(inactive), mode == 0);
    EXPECT_EQ(graph.activeRoots(), oldView);
    auto prepared = graph.prepareAppend(std::move(candidate));
    ASSERT_TRUE(graph.publishAppend(prepared));
    const auto createdView = graph.activeRoots();
    auto detach = graph.prepareDetachTail(first, oldView);
    ASSERT_TRUE(detach);
    ASSERT_TRUE(graph.publishTailChange(detach));
    EXPECT_EQ(graph.activeRoots(), oldView);
    auto receipt = detach.takeDetachedTail();
    ASSERT_TRUE(receipt);
    auto restore = graph.prepareRestoreTail(*receipt);
    ASSERT_TRUE(restore);
    ASSERT_TRUE(graph.publishTailChange(restore));
    EXPECT_EQ(graph.activeRoots(), createdView);
    EXPECT_TRUE(graph.isNodeActive(first));
    EXPECT_EQ(graph.isNodeActive(inactive), mode == 0);
  }
}

TEST(SceneGraphEditTests, EmptyNodeProofRefusesForeignAndRenamedGraphWithoutLosingEdits) {
  SceneGraph graph;
  const auto parent = add(graph, "Original parent", false);
  auto prepared = graph.prepareAppend(graph.makeEmptyNodeCandidate(parent, "Child"));
  SceneGraph foreign(graph);
  EXPECT_FALSE(foreign.publishAppend(prepared));
  const auto oldRevision = graph.revision(), oldHierarchy = graph.hierarchyRevision();
  graph.setName(parent, "Renamed after prepare");
  EXPECT_GT(graph.revision(), oldRevision);
  EXPECT_GT(graph.hierarchyRevision(), oldHierarchy);
  const auto edited = snapshot(graph);
  EXPECT_FALSE(graph.publishAppend(prepared));
  EXPECT_EQ(snapshot(graph), edited);
  EXPECT_EQ(graph.nodeCount(), 1u);
  prepared = graph.prepareAppend(graph.makeEmptyNodeCandidate(parent, "Child"));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.getNode(parent)->name, "Renamed after prepare");

  auto detach = graph.prepareDetachTail(1);
  ASSERT_TRUE(graph.publishTailChange(detach));
  auto receipt = detach.takeDetachedTail();
  ASSERT_TRUE(receipt);
  graph.setLocalTransform(parent, glm::translate(glm::mat4(1), {9, 8, 7}));
  graph.setName(parent, "Retained edit while undone");
  auto restore = graph.prepareRestoreTail(*receipt);
  ASSERT_TRUE(graph.publishTailChange(restore));
  EXPECT_EQ(graph.getNode(parent)->name, "Retained edit while undone");
  EXPECT_EQ(graph.getNode(1)->localTransform, glm::mat4(1));
  EXPECT_EQ(graph.getNode(1)->worldTransform, graph.getNode(parent)->localTransform);
}

TEST(SceneGraphEditTests, EmptyViewEditorContextActivatesOnlyNewLeafAndRestoresExactView) {
  SceneGraph graph;
  const auto editor = add(graph, "Editor", false), dormant = add(graph, "Dormant");
  ASSERT_TRUE(graph.setParent(dormant, editor));
  graph.getNode(editor)->editorSceneRoot = true;
  graph.setActiveRoots(std::span<const uint32_t>{});
  const auto oldView = graph.activeRoots();
  const auto before = snapshot(graph);
  const auto leaf = uint32_t(graph.nodeCount());
  auto candidate = graph.makeEmptyNodeCandidate(editor, "First empty");
  EXPECT_EQ(candidate.activeRoots(), (std::optional(std::vector<uint32_t>{leaf})));
  EXPECT_TRUE(candidate.isNodeActive(leaf));
  EXPECT_FALSE(candidate.isNodeActive(editor));
  EXPECT_TRUE(candidate.isNodeInActiveHierarchy(editor));
  EXPECT_FALSE(candidate.isNodeActive(dormant));
  EXPECT_EQ(snapshot(graph), before);
  auto append = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(append));
  auto detach = graph.prepareDetachTail(leaf, oldView);
  ASSERT_TRUE(graph.publishTailChange(detach));
  EXPECT_EQ(graph.activeRoots(), oldView);
  EXPECT_FALSE(graph.isNodeInActiveHierarchy(editor));
  auto receipt = detach.takeDetachedTail();
  ASSERT_TRUE(receipt);
  auto restore = graph.prepareRestoreTail(*receipt);
  ASSERT_TRUE(graph.publishTailChange(restore));
  EXPECT_TRUE(graph.isNodeActive(leaf));
  EXPECT_FALSE(graph.isNodeActive(dormant));

  for (const int invalidContext : {0, 1, 2}) {
    graph.setActiveRoots(std::span<const uint32_t>{});
    auto* node = graph.getNode(editor);
    node->renderable = invalidContext == 0;
    node->sourceNodeIndex = invalidContext == 1 ? 4 : -1;
    node->interactionRootBoundary = invalidContext == 2;
    EXPECT_THROW(graph.makeEmptyNodeCandidate(editor, "Rejected"), std::invalid_argument);
    node->renderable = false;
  }
}

TEST(SceneGraphEditTests, EmptyNodeInvalidNamesParentsAndMalformedGraphsLeaveLiveStateUntouched) {
  SceneGraph graph;
  const auto active = add(graph, "Active", false), inactive = add(graph, "Inactive", false);
  graph.setActiveRoots(std::array{active});
  const auto before = snapshot(graph);
  const auto roots = graph.rootNodes();
  const auto view = graph.activeRoots();
  const auto revision = graph.revision(), hierarchy = graph.hierarchyRevision();
  for (const auto parent : {inactive, SceneGraph::kInvalidNode, 99999u}) {
    SCOPED_TRACE(parent);
    EXPECT_THROW(graph.makeEmptyNodeCandidate(parent, "Child"), std::invalid_argument);
  }
  for (const auto& name : {std::string{}, std::string("  \t"), std::string("bad\nname"),
                           std::string("bad\0name", 8), std::string(4097, 'x')}) {
    SCOPED_TRACE(name.size());
    EXPECT_THROW(graph.makeEmptyNodeCandidate(active, name), std::invalid_argument);
  }
  EXPECT_EQ(snapshot(graph), before);
  EXPECT_EQ(graph.rootNodes(), roots);
  EXPECT_EQ(graph.activeRoots(), view);
  EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(graph.hierarchyRevision(), hierarchy);

  graph.getNode(active)->children.push_back(inactive); // Inconsistent legacy edge.
  const auto malformed = snapshot(graph);
  EXPECT_THROW(graph.makeEmptyNodeCandidate(std::nullopt, "Child"), std::invalid_argument);
  EXPECT_EQ(snapshot(graph), malformed);
  graph.getNode(active)->children.clear();
  auto nonfiniteLocal = glm::mat4(1);
  nonfiniteLocal[0][0] = std::numeric_limits<float>::infinity();
  graph.setLocalTransform(active, nonfiniteLocal);
  const auto nonfinite = snapshot(graph);
  EXPECT_THROW(graph.makeEmptyNodeCandidate(active, "Child"), std::invalid_argument);
  EXPECT_EQ(snapshot(graph), nonfinite);
}

TEST(SceneGraphEditTests, EmptyNodeCreationHandlesDeepAncestorsWithoutRecursion) {
  SceneGraph graph;
  constexpr uint32_t count = 10000;
  std::vector<SceneNode> nodes(count);
  for (uint32_t id = 0; id < count; ++id) {
    nodes[id].localTransform = glm::translate(glm::mat4(1), {1, 0, 0});
    if (id) nodes[id].parent = id - 1;
    if (id + 1 < count) nodes[id].children = {id + 1};
  }
  auto append = graph.prepareAppend(graph.makeAppendCandidate(std::move(nodes)));
  ASSERT_TRUE(graph.publishAppend(append));
  const auto before = snapshot(graph);
  auto candidate = graph.makeEmptyNodeCandidate(count - 1, "Deep empty");
  EXPECT_EQ(snapshot(graph), before);
  EXPECT_FLOAT_EQ(candidate.getNode(count)->worldTransform[3][0], float(count));
  auto prepared = graph.prepareAppend(std::move(candidate));
  ASSERT_TRUE(graph.publishAppend(prepared));
  EXPECT_EQ(graph.getNode(count)->parent, count - 1);
  EXPECT_EQ(graph.getNode(count)->localTransform, glm::mat4(1));
}

TEST(SceneGraphRetirement, KeepsEverySlotPayloadOrderAndAbsentViewAcrossUndoRedo) {
  SceneGraph graph;
  const auto left = add(graph, "left"), owner = add(graph, "owner", false);
  const auto child = add(graph, "child"), right = add(graph, "right");
  graph.getNode(owner)->sourceNodeIndex = 3;
  graph.getNode(child)->meshOwnerGraphNode = owner;
  ASSERT_TRUE(graph.setParent(child, owner));
  graph.setRenderable(left, false); graph.setRenderable(left, true);
  graph.setLocalTransform(owner, glm::translate(glm::mat4(1), {2, 3, 4}));
  graph.updateWorldTransforms();
  const auto old = snapshot(graph);
  const auto roots = graph.rootNodes(), order = graph.renderableNodes();
  const auto identity = graph.identity(), revision = graph.revision();
  auto prepared = graph.prepareRetireSubtree(owner);
  ASSERT_TRUE(prepared); ASSERT_NE(prepared.candidateGraph(), nullptr);
  EXPECT_FALSE(prepared.takeRetirement());
  EXPECT_EQ(snapshot(graph), old);
  EXPECT_EQ(prepared.candidateGraph()->nodeCount(), old.size());
  EXPECT_EQ(prepared.candidateGraph()->renderableNodes(), (std::vector<uint32_t>{right, left}));
  ASSERT_TRUE(graph.publishRetirement(prepared));
  EXPECT_EQ(graph.identity(), identity); EXPECT_GT(graph.revision(), revision);
  EXPECT_EQ(graph.rootNodes(), roots); EXPECT_FALSE(graph.activeRoots());
  EXPECT_EQ(graph.nodeCount(), old.size());
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    auto expected = old[id]; expected.retired = id == owner || id == child;
    ASSERT_NE(graph.getNode(id), nullptr); EXPECT_EQ(*graph.getNode(id), expected);
  }
  EXPECT_FALSE(graph.isNodeActive(child)); EXPECT_FALSE(graph.isNodeInActiveHierarchy(owner));
  EXPECT_FALSE(graph.isNodeEffectivelyVisible(child));
  EXPECT_EQ(graph.nearestSelectableAncestor(child), SceneGraph::kInvalidNode);
  EXPECT_EQ(graph.nearestHoverableAncestor(child), SceneGraph::kInvalidNode);
  EXPECT_FALSE(graph.publishRetirement(prepared));
  auto receipt = prepared.takeRetirement(); ASSERT_TRUE(receipt);
  EXPECT_EQ(receipt->rootNode(), owner);
  EXPECT_EQ(std::vector<uint32_t>(receipt->nodes().begin(), receipt->nodes().end()), (std::vector<uint32_t>{owner, child}));
  EXPECT_FALSE(prepared.takeRetirement());
  auto undo = graph.prepareRetirementChange(*receipt, false);
  ASSERT_TRUE(graph.publishRetirement(undo));
  EXPECT_EQ(snapshot(graph), old); EXPECT_EQ(graph.renderableNodes(), order);
  auto redo = graph.prepareRetirementChange(*receipt, true);
  ASSERT_TRUE(graph.publishRetirement(redo));
  EXPECT_EQ(graph.renderableNodes(), (std::vector<uint32_t>{right, left}));
  EXPECT_EQ(graph.rootNodes(), roots);
  const auto next = add(graph, "new"); EXPECT_EQ(next, old.size());
}

TEST(SceneGraphRetirement, PreservesExplicitViewAndPreviouslyRetiredDescendant) {
  SceneGraph graph;
  const auto editor = add(graph, "editor", false); graph.getNode(editor)->editorSceneRoot = true;
  const auto parent = add(graph, "parent", false), child = add(graph, "child"), inactive = add(graph, "inactive");
  ASSERT_TRUE(graph.setParent(parent, editor)); ASSERT_TRUE(graph.setParent(child, parent));
  ASSERT_TRUE(graph.setParent(inactive, editor));
  graph.setActiveRoots(std::array{parent});
  auto childProof = graph.prepareRetireSubtree(child); ASSERT_TRUE(graph.publishRetirement(childProof));
  auto childReceipt = childProof.takeRetirement(); ASSERT_TRUE(childReceipt);
  auto parentProof = graph.prepareRetireSubtree(parent); ASSERT_TRUE(graph.publishRetirement(parentProof));
  auto parentReceipt = parentProof.takeRetirement(); ASSERT_TRUE(parentReceipt);
  EXPECT_EQ(graph.activeRoots(), (std::optional<std::vector<uint32_t>>{{parent}}));
  EXPECT_TRUE(graph.renderableNodes().empty()); EXPECT_FALSE(graph.isNodeActive(inactive));
  EXPECT_FALSE(graph.isNodeInActiveHierarchy(editor));
  auto undoParent = graph.prepareRetirementChange(*parentReceipt, false);
  ASSERT_TRUE(graph.publishRetirement(undoParent));
  EXPECT_TRUE(graph.isNodeActive(parent)); EXPECT_TRUE(graph.isNodeInActiveHierarchy(editor));
  EXPECT_TRUE(graph.isNodeRetired(child)); EXPECT_FALSE(graph.isNodeActive(inactive));
  auto undoChild = graph.prepareRetirementChange(*childReceipt, false);
  ASSERT_TRUE(graph.publishRetirement(undoChild));
  EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{child});
  graph.setActiveRoots(std::array{inactive});
  EXPECT_FALSE(graph.prepareRetirementChange(*childReceipt, true));
}

TEST(SceneGraphRetirement, ProofIsForeignStaleReplacedAndSingleUseSafe) {
  SceneGraph graph; const auto node = add(graph, "node");
  auto prepared = graph.prepareRetireSubtree(node); SceneGraph other(graph);
  EXPECT_FALSE(other.publishRetirement(prepared));
  graph.setName(node, "edited"); const auto before = snapshot(graph);
  EXPECT_FALSE(graph.publishRetirement(prepared)); EXPECT_EQ(snapshot(graph), before);
  auto replacement = graph.prepareRetireSubtree(node);
  graph = SceneGraph(graph); EXPECT_FALSE(graph.publishRetirement(replacement));
  auto current = graph.prepareRetireSubtree(node); ASSERT_TRUE(graph.publishRetirement(current));
  auto receipt = current.takeRetirement(); ASSERT_TRUE(receipt);
  EXPECT_THROW(graph.prepareRetireSubtree(node), std::invalid_argument);
  EXPECT_FALSE(other.prepareRetirementChange(*receipt, false));
  auto undo = graph.prepareRetirementChange(*receipt, false); ASSERT_TRUE(graph.publishRetirement(undo));
  EXPECT_FALSE(graph.publishRetirement(undo));
  EXPECT_FALSE(graph.prepareRetirementChange(*receipt, false));
  ASSERT_TRUE(graph.setParent(node, add(graph, "new parent", false)));
  EXPECT_FALSE(graph.prepareRetirementChange(*receipt, true));
}

TEST(SceneGraphRetirement, StructuralWrappersAndExternalImmutableOwnersRejectAtomically) {
  for (const bool editor : {false, true}) {
    SceneGraph graph; const auto node = add(graph, "renamed wrapper", false);
    graph.getNode(node)->editorSceneRoot = editor;
    graph.getNode(node)->interactionRootBoundary = !editor;
    const auto before = snapshot(graph);
    EXPECT_THROW(graph.prepareRetireSubtree(node), std::invalid_argument);
    EXPECT_EQ(snapshot(graph), before);
  }
  SceneGraph graph; const auto owner = add(graph, "owner", false), primitive = add(graph, "moved primitive");
  graph.getNode(primitive)->meshOwnerGraphNode = owner;
  const auto before = snapshot(graph);
  EXPECT_THROW(graph.prepareRetireSubtree(owner), std::invalid_argument);
  EXPECT_EQ(snapshot(graph), before);
  ASSERT_TRUE(graph.setParent(primitive, owner));
  auto proof = graph.prepareRetireSubtree(owner); ASSERT_TRUE(graph.publishRetirement(proof));
  const auto retired = snapshot(graph); const auto rev = graph.revision();
  graph.setLocalTransform(owner, glm::mat4(2)); graph.setVisible(owner, false);
  graph.setName(owner, "bad"); graph.setMaterialIndex(owner, 8);
  graph.setRenderable(primitive, false); graph.setSelectable(owner, false); graph.setHoverable(owner, false);
  EXPECT_FALSE(graph.setParent(owner, std::nullopt)); EXPECT_FALSE(graph.setSiblingIndex(owner, 0));
  EXPECT_THROW(graph.makeEmptyNodeCandidate(owner), std::invalid_argument);
  EXPECT_EQ(snapshot(graph), retired); EXPECT_EQ(graph.revision(), rev);
  const auto live = add(graph, "live"); EXPECT_FALSE(graph.setParent(live, owner));
  SceneNode tail; tail.parent = owner;
  EXPECT_THROW(graph.makeAppendCandidate({tail}), std::invalid_argument);
}

TEST(SceneGraphRetirement, ValidatesMalformedWholeForestAndBudgetsBeforePublication) {
  for (const int kind : {0, 1, 2, 3}) {
    SceneGraph graph; const auto root = add(graph, "root", false), child = add(graph, "child");
    ASSERT_TRUE(graph.setParent(child, root));
    if (kind == 0) graph.getNode(root)->children.push_back(child);
    if (kind == 1) graph.getNode(child)->children.push_back(root);
    if (kind == 2) graph.getNode(child)->parent = SceneGraph::kInvalidNode;
    if (kind == 3) graph.getNode(root)->children.push_back(SceneGraph::kInvalidNode);
    const auto before = snapshot(graph); const auto rev = graph.revision();
    EXPECT_THROW(graph.prepareRetireSubtree(root), std::invalid_argument);
    EXPECT_EQ(snapshot(graph), before); EXPECT_EQ(graph.revision(), rev);
  }
  SceneGraph graph; const auto root = add(graph, "root");
  for (const auto limits : {container::scene::SceneRetirementLimits{0, 10, 100000},
                           container::scene::SceneRetirementLimits{10, 0, 100000},
                           container::scene::SceneRetirementLimits{10, 10, 1}})
    EXPECT_THROW(graph.prepareRetireSubtree(root, limits), std::invalid_argument);
  graph.setLocalTransform(root, glm::translate(glm::mat4(1), {4, 0, 0}));
  auto proof = graph.prepareRetireSubtree(root);
  EXPECT_FLOAT_EQ(proof.candidateGraph()->getNode(root)->worldTransform[3][0], 4);
}

TEST(SceneGraphRetirement, CapturedLocalPoseStagesExactRestoreAndRejectsInvalidOverrides) {
  SceneGraph graph; const auto parent = add(graph, "surviving animation", false), child = add(graph, "deleted");
  ASSERT_TRUE(graph.setParent(child, parent));
  graph.setLocalTransform(child, glm::translate(glm::mat4(1), {2, 0, 0}));
  auto proof = graph.prepareRetireSubtree(child); ASSERT_TRUE(graph.publishRetirement(proof));
  auto receipt = proof.takeRetirement(); ASSERT_TRUE(receipt);
  graph.setLocalTransform(parent, glm::translate(glm::mat4(1), {8, 0, 0}));
  const auto before = snapshot(graph);
  const std::array restore{container::scene::SceneNodeLocalPose{parent, glm::mat4(1)},
      container::scene::SceneNodeLocalPose{child, glm::translate(glm::mat4(1), {2, 0, 0})}};
  auto undo = graph.prepareRetirementChange(*receipt, false, restore);
  EXPECT_EQ(snapshot(graph), before);
  EXPECT_FLOAT_EQ(undo.candidateGraph()->getNode(child)->worldTransform[3][0], 2);
  ASSERT_TRUE(graph.publishRetirement(undo));
  auto redo = graph.prepareRetirementChange(*receipt, true, restore);
  ASSERT_TRUE(graph.publishRetirement(redo));
  auto invalid = restore; invalid[1].node = parent;
  EXPECT_THROW(graph.prepareRetirementChange(*receipt, false, invalid), std::invalid_argument);
  invalid = restore; invalid[0].localTransform[0][0] = std::numeric_limits<float>::infinity();
  EXPECT_THROW(graph.prepareRetirementChange(*receipt, false, invalid), std::invalid_argument);
  EXPECT_TRUE(graph.isNodeRetired(child));
}

TEST(SceneGraphRetirement, WideAndDeepSubtreesUseBoundedIterativeStableSlotPreparation) {
  for (const auto [count, deep] : {std::pair{20001u, false}, std::pair{10000u, true}}) {
    SceneGraph empty; std::vector<SceneNode> tail(count);
    for (uint32_t id = 1; id < count; ++id) {
      tail[id].parent = deep ? id - 1 : 0;
      tail[tail[id].parent].children.push_back(id);
      tail[id].renderable = id + 1 == count;
    }
    auto graph = empty.makeAppendCandidate(std::move(tail));
    auto proof = graph.prepareRetireSubtree(0); ASSERT_TRUE(graph.publishRetirement(proof));
    auto receipt = proof.takeRetirement(); ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->nodes().size(), count); EXPECT_EQ(graph.nodeCount(), count);
    EXPECT_TRUE(graph.renderableNodes().empty());
    auto undo = graph.prepareRetirementChange(*receipt, false); ASSERT_TRUE(graph.publishRetirement(undo));
    EXPECT_EQ(graph.renderableNodes(), std::vector<uint32_t>{count - 1});
    EXPECT_EQ(graph.getNode(count - 1)->parent, deep ? count - 2 : 0);
  }
}

TEST(SceneGraphEditTests, PreparedPlacementStagesExactParentLocalOrderWorldsAndViewBeforePublication) {
  SceneGraph graph;
  const auto a = add(graph, "A", false), b = add(graph, "B", false);
  const auto selected = add(graph, "Selected"), sibling = add(graph, "Sibling");
  const auto other = add(graph, "Other"), child = add(graph, "Descendant");
  graph.setParent(selected, a); graph.setParent(sibling, a); graph.setParent(other, b); graph.setParent(child, selected);
  graph.setLocalTransform(a, glm::translate(glm::mat4(1), {2, 0, 0}));
  graph.setLocalTransform(b, glm::translate(glm::mat4(1), {-3, 4, 0}));
  graph.setLocalTransform(child, glm::scale(glm::mat4(1), {1, -2, 3}));
  graph.getNode(selected)->sourceNodeIndex = 7; graph.setSelectable(selected, false);
  graph.setRenderable(sibling, false); graph.setRenderable(sibling, true); // Deliberate non-ID renderable order.
  graph.setActiveRoots(std::array{a}); graph.updateWorldTransforms();
  const auto before = snapshot(graph);
  const auto renderables = graph.renderableNodes();
  const auto identity = graph.identity(), revision = graph.revision();
  auto local = glm::scale(glm::mat4(1), {-2, 3, .5f}); local[1][0] = .25f;
  local[3] = glm::vec4(1, 2, 3, 1);
  std::optional<std::vector<uint32_t>> view = std::vector<uint32_t>{b, sibling, b};
  const auto expectedView = view;
  auto proof = graph.preparePlacement(selected, b, local, 0, view); ASSERT_TRUE(proof);
  view->clear(); // Neither the caller's view nor local matrix is borrowed.
  const auto* candidate = proof.candidateGraph(); ASSERT_NE(candidate, nullptr);
  EXPECT_EQ(snapshot(graph), before); EXPECT_EQ(graph.revision(), revision);
  EXPECT_EQ(candidate->activeRoots(), expectedView);
  EXPECT_EQ(candidate->getNode(a)->children, std::vector<uint32_t>{sibling});
  EXPECT_EQ(candidate->getNode(b)->children, (std::vector<uint32_t>{selected, other}));
  EXPECT_EQ(candidate->getNode(selected)->localTransform, local);
  EXPECT_EQ(candidate->getNode(selected)->worldTransform, before[b].localTransform * local);
  EXPECT_EQ(candidate->getNode(child)->worldTransform, before[b].localTransform * local * before[child].localTransform);
  EXPECT_EQ(candidate->getNode(selected)->sourceNodeIndex, 7); EXPECT_FALSE(candidate->getNode(selected)->selectable);
  ASSERT_TRUE(graph.publishPlacement(proof));
  EXPECT_EQ(graph.identity(), identity); EXPECT_EQ(graph.activeRoots(), expectedView);
  EXPECT_EQ(graph.rootNodes(), (std::vector<uint32_t>{a, b}));
  EXPECT_TRUE(graph.isNodeActive(selected)); EXPECT_TRUE(graph.isNodeActive(child));
  EXPECT_TRUE(graph.isNodeActive(sibling));
  graph.clearActiveRoots();
  // The raw table's original registration order survives a different active view.
  EXPECT_EQ(graph.renderableNodes(), (std::vector<uint32_t>{selected, other, child, sibling}));
  EXPECT_EQ(renderables, (std::vector<uint32_t>{selected, child, sibling}));
}

TEST(SceneGraphEditTests, PreparedPlacementPreservesNullEmptyAndRetiredRepeatedRootViewsAndRootOrder) {
  for (int mode = 0; mode < 3; ++mode) {
    SCOPED_TRACE(mode); SceneGraph graph;
    const auto a = add(graph, "A"), b = add(graph, "B"), c = add(graph, "C");
    const auto retired = add(graph, "Retired");
    auto retire = graph.prepareRetireSubtree(retired); ASSERT_TRUE(graph.publishRetirement(retire));
    std::optional<std::vector<uint32_t>> view;
    if (mode == 1) view = std::vector<uint32_t>{};
    if (mode == 2) view = std::vector<uint32_t>{retired, c, retired};
    auto proof = graph.preparePlacement(c, std::nullopt, glm::mat4(1), 0, view); ASSERT_TRUE(proof);
    ASSERT_TRUE(graph.publishPlacement(proof));
    EXPECT_EQ(graph.activeRoots(), view); EXPECT_EQ(graph.rootNodes(), (std::vector<uint32_t>{c, a, b, retired}));
    EXPECT_FALSE(graph.isNodeActive(retired)); EXPECT_EQ(graph.isNodeActive(c), mode != 1);
    EXPECT_EQ(graph.isNodeActive(a), mode == 0);
  }
}

TEST(SceneGraphEditTests, PreparedPlacementRefusesStaleForeignAndConsumedProofsAndOwnsItsSnapshot) {
  SceneGraph graph; const auto parent = add(graph, "Parent", false), child = add(graph, "Child");
  SceneGraph::PreparedPlacement empty; EXPECT_FALSE(empty); EXPECT_EQ(empty.candidateGraph(), nullptr);
  auto local = glm::translate(glm::mat4(1), {5, 0, 0});
  auto proof = graph.preparePlacement(child, parent, local, 0, std::nullopt); ASSERT_TRUE(proof);
  local[3][0] = 9; EXPECT_FLOAT_EQ(proof.candidateGraph()->getNode(child)->localTransform[3][0], 5);
  SceneGraph foreign(graph); const auto foreignBefore = snapshot(foreign);
  EXPECT_FALSE(foreign.publishPlacement(proof)); EXPECT_EQ(snapshot(foreign), foreignBefore);
  graph.setVisible(parent, false); const auto edited = snapshot(graph);
  EXPECT_FALSE(graph.publishPlacement(proof)); EXPECT_EQ(snapshot(graph), edited);
  proof = graph.preparePlacement(child, parent, local, 0, std::nullopt);
  SceneGraph::PreparedPlacement moved(std::move(proof)); EXPECT_FALSE(proof); EXPECT_EQ(proof.candidateGraph(), nullptr);
  const auto identity = graph.identity(); ASSERT_TRUE(graph.publishPlacement(moved));
  EXPECT_FALSE(moved); EXPECT_EQ(moved.candidateGraph(), nullptr); EXPECT_FALSE(graph.publishPlacement(moved));
  EXPECT_EQ(graph.identity(), identity); EXPECT_FLOAT_EQ(graph.getNode(child)->worldTransform[3][0], 9);
}

TEST(SceneGraphEditTests, PreparedPlacementRejectsInvalidForestTargetsViewsBudgetsAndWorldOverflowWithoutMutation) {
  using container::scene::ScenePlacementLimits;
  SceneGraph graph; const auto root = add(graph, "Root", false), child = add(graph, "Child");
  const auto retired = add(graph, "Retired", false); graph.setParent(child, root);
  auto retirement = graph.prepareRetireSubtree(retired); ASSERT_TRUE(graph.publishRetirement(retirement));
  graph.setLocalTransform(root, glm::scale(glm::mat4(1), {1e30f, 1, 1})); graph.updateWorldTransforms();
  const auto before = snapshot(graph); const auto roots = graph.rootNodes(), renderables = graph.renderableNodes();
  const auto revision = graph.revision(), hierarchy = graph.hierarchyRevision(); const auto view = graph.activeRoots();
  auto invalid = glm::mat4(1); invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(graph.preparePlacement(child, root, invalid, 0, view));
  EXPECT_FALSE(graph.preparePlacement(child, root, glm::scale(glm::mat4(1), {1e30f, 1, 1}), 0, view));
  EXPECT_FALSE(graph.preparePlacement(root, child, glm::mat4(1), 0, view));
  EXPECT_FALSE(graph.preparePlacement(child, 9999, glm::mat4(1), 0, view));
  EXPECT_FALSE(graph.preparePlacement(retired, root, glm::mat4(1), 1, view));
  EXPECT_FALSE(graph.preparePlacement(child, retired, glm::mat4(1), 0, view));
  EXPECT_FALSE(graph.preparePlacement(9999, root, glm::mat4(1), 0, view));
  EXPECT_FALSE(graph.preparePlacement(child, root, glm::mat4(1), 99, view));
  EXPECT_FALSE(graph.preparePlacement(child, root, glm::mat4(1), 0, std::vector<uint32_t>{9999}));
  for (int kind = 0; kind < 4; ++kind) {
    ScenePlacementLimits limits;
    if (kind == 0) limits.maxNodes = 2;
    if (kind == 1) limits.maxEdges = 0;
    if (kind == 2) limits.maxPayloadBytes = 1;
    if (kind == 3) limits.maxNodes = 0;
    EXPECT_FALSE(graph.preparePlacement(child, root, glm::mat4(1), 0, view, limits));
  }
  EXPECT_EQ(snapshot(graph), before); EXPECT_EQ(graph.rootNodes(), roots); EXPECT_EQ(graph.renderableNodes(), renderables);
  EXPECT_EQ(graph.revision(), revision); EXPECT_EQ(graph.hierarchyRevision(), hierarchy); EXPECT_EQ(graph.activeRoots(), view);
  graph.getNode(root)->children.push_back(child); const auto malformed = snapshot(graph);
  EXPECT_FALSE(graph.preparePlacement(child, std::nullopt, glm::mat4(1), 0, view));
  EXPECT_EQ(snapshot(graph), malformed); EXPECT_EQ(graph.revision(), revision);
}

TEST(SceneGraphEditTests, PreparedPlacementValidatesLargeWideAndDeepForestsWithoutRecursiveOrQuadraticWalks) {
  for (const bool deep : {false, true}) {
    SCOPED_TRACE(deep); SceneGraph graph;
    const auto from = add(graph, "From", false), to = add(graph, "To", false);
    graph.setLocalTransform(to, glm::translate(glm::mat4(1), {10, 0, 0})); graph.updateWorldTransforms();
    const uint32_t count = deep ? 10001 : 20001, first = uint32_t(graph.nodeCount());
    std::vector<SceneNode> tail(count);
    for (uint32_t i = 0; i < count; ++i) {
      tail[i].localTransform = glm::translate(glm::mat4(1), {1, 0, 0});
      tail[i].parent = deep && i ? first + i - 1 : from;
      if (deep && i + 1 < count) tail[i].children = {first + i + 1};
    }
    auto append = graph.prepareAppend(graph.makeAppendCandidate(std::move(tail))); ASSERT_TRUE(graph.publishAppend(append));
    auto proof = graph.preparePlacement(first, to, glm::translate(glm::mat4(1), {1, 0, 0}), 0, std::nullopt);
    ASSERT_TRUE(proof); ASSERT_TRUE(graph.publishPlacement(proof));
    EXPECT_FLOAT_EQ(graph.getNode(first)->worldTransform[3][0], 11);
    EXPECT_FLOAT_EQ(graph.getNode(first + count - 1)->worldTransform[3][0], deep ? float(count + 10) : 1.0f);
    EXPECT_EQ(graph.getNode(first)->parent, to); EXPECT_EQ(graph.nodeCount(), size_t(count + 2));
  }
}

} // namespace
