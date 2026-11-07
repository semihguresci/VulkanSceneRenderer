#include <Container/utility/GraphCloneProjection.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using namespace container::scene;
using Role=container::geometry::gltf::GltfCloneNodeRole;
constexpr auto invalid=SceneGraph::kInvalidNode;

struct Fixture {
  SceneGraph graph;tinygltf::Model source;
  std::vector<std::vector<uint32_t>> scenes{{2,6,8},{2,8}};
  const std::array<uint32_t,1> primitives{2};
  Fixture() {
    source.asset.version="2.0";source.nodes.resize(3);source.meshes.resize(1);
    source.meshes[0].primitives.emplace_back();source.nodes[0].mesh=source.nodes[1].mesh=0;
    source.nodes[0].skin=0;source.skins.resize(1);source.skins[0].joints={2};source.skins[0].skeleton=2;
    source.scenes.resize(2);source.scenes[0].nodes={0,1,2};source.scenes[1].nodes={0,2};source.defaultScene=0;
    graph.createNode(glm::mat4(1),0,false,invalid,"Editor");graph.getNode(0)->editorSceneRoot=true;
    graph.createNode(glm::scale(glm::mat4(1),{2,2,2}),0,false,invalid,"Import");
    graph.getNode(1)->interactionRootBoundary=true;graph.setParent(1,0);
    graph.createNode(glm::translate(glm::mat4(1),{1,0,0}),0,false,invalid,"A edited");
    graph.getNode(2)->sourceNodeIndex=0;graph.setParent(2,1);
    graph.createNode(glm::translate(glm::mat4(1),{.25f,0,0}),3,true,2,"A primitive edit");
    graph.getNode(3)->sourceMeshIndex=0;graph.getNode(3)->sourcePrimitiveIndex=0;
    graph.getNode(3)->meshOwnerGraphNode=2;graph.setParent(3,2);
    graph.createNode(glm::translate(glm::mat4(1),{0,3,0}),1,false,invalid,"Graph-only empty");graph.setParent(4,2);
    graph.createNode(glm::scale(glm::mat4(1),{1,2,3}),4,true,1,"Procedural gap");graph.setParent(5,4);
    graph.createNode(glm::translate(glm::mat4(1),{-1,0,0}),0,false,invalid,"B");
    graph.getNode(6)->sourceNodeIndex=1;graph.setParent(6,1);
    graph.createNode(glm::mat4(1),5,true,2,"B primitive");
    graph.getNode(7)->sourceMeshIndex=0;graph.getNode(7)->sourcePrimitiveIndex=0;
    graph.getNode(7)->meshOwnerGraphNode=6;graph.setParent(7,6);
    graph.createNode(glm::translate(glm::mat4(1),{0,2,0}),0,false,invalid,"Joint");
    graph.getNode(8)->sourceNodeIndex=2;graph.setParent(8,1);
    graph.setSelectable(4,false);graph.setHoverable(5,false);
    graph.setActiveRoots(scenes[0]);graph.updateWorldTransforms();
  }
  GraphCloneProjectionInput input(uint32_t root) const {return {graph,source,root,scenes,0,3,6,primitives};}
  const container::geometry::gltf::GltfProjectedCloneNode* row(const GraphCloneProjectionPlan& plan,int sourceId) const {
    for(const auto& value:plan.sourceProjection.nodes) if(value.sourceNode==sourceId) return &value;
    return nullptr;
  }
};
std::vector<SceneNode> nodes(const SceneGraph& graph) {
  std::vector<SceneNode> result;for(uint32_t id=0;id<graph.nodeCount();++id) result.push_back(*graph.getNode(id));return result;
}

TEST(GraphCloneProjection,PreservesEditedPayloadsGraphOnlyChildrenProceduralGapsAndSharedBindings) {
  Fixture f;const auto before=nodes(f.graph);auto input=f.input(2);input.rootName="A copy";
  const auto plan=BuildGraphCloneProjection(input);
  EXPECT_EQ(nodes(f.graph),before);EXPECT_EQ(plan.firstNode,f.graph.nodeCount());
  ASSERT_NE(plan.oldToNewGraph[2],invalid);EXPECT_EQ(plan.root,plan.oldToNewGraph[2]);
  EXPECT_EQ(plan.clonedRenderables,2u);EXPECT_EQ(plan.graph.getNode(plan.root)->name,"A copy");
  for(const auto original:{3u,4u,5u}) {
    const auto* copy=plan.graph.getNode(plan.oldToNewGraph[original]);
    EXPECT_EQ(copy->localTransform,before[original].localTransform);EXPECT_EQ(copy->worldTransform,before[original].worldTransform);
    EXPECT_EQ(copy->materialIndex,before[original].materialIndex);EXPECT_EQ(copy->primitiveIndex,before[original].primitiveIndex);
    EXPECT_EQ(copy->name,before[original].name);EXPECT_EQ(copy->selectable,before[original].selectable);
    EXPECT_EQ(copy->hoverable,before[original].hoverable);
  }
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[3])->meshOwnerGraphNode,plan.root);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[5])->sourceNodeIndex,-1);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[5])->parent,plan.oldToNewGraph[4]);
  ASSERT_NE(f.row(plan,0),nullptr);EXPECT_EQ(f.row(plan,0)->role,Role::Visual);EXPECT_TRUE(f.row(plan,0)->retainMeshBinding);
  ASSERT_NE(f.row(plan,2),nullptr);EXPECT_EQ(f.row(plan,2)->role,Role::JointTransformOnly);
  EXPECT_FALSE(plan.graph.getNode(plan.oldToNewGraph[8])->renderable);
  EXPECT_EQ(plan.sourceProjection.affectedScenes,(std::vector<size_t>{0}));
  EXPECT_EQ(plan.sceneRoots[1],f.scenes[1]);
  for(uint32_t id=0;id<f.graph.nodeCount();++id) {
    auto retained=*plan.graph.getNode(id);const auto& previous=before[id];
    ASSERT_GE(retained.children.size(),previous.children.size());
    EXPECT_TRUE(std::equal(previous.children.begin(),previous.children.end(),retained.children.begin()));
    retained.children=previous.children;EXPECT_EQ(retained,previous);
  }
  auto append=f.graph.prepareAppend(plan.graph);ASSERT_TRUE(append);ASSERT_TRUE(f.graph.publishAppend(append));
  EXPECT_EQ(f.graph.renderableNodes().size(),5u);
}

TEST(GraphCloneProjection,GraphOnlyEmptyAndProceduralCopiesNeedNoNewSourceIdentities) {
  Fixture f;const auto plan=BuildGraphCloneProjection(f.input(4));
  EXPECT_TRUE(plan.sourceProjection.nodes.empty());EXPECT_TRUE(plan.dependencyRoots.empty());
  EXPECT_EQ(plan.sourceProjection.affectedScenes,(std::vector<size_t>{0,1}));
  EXPECT_TRUE(plan.addedSceneRoots[0].empty());EXPECT_TRUE(plan.addedSceneRoots[1].empty());
  EXPECT_EQ(plan.graph.getNode(plan.root)->parent,2u);
  EXPECT_EQ(plan.graph.getNode(plan.root)->sourceNodeIndex,-1);
  const auto* primitive=plan.graph.getNode(plan.oldToNewGraph[5]);
  EXPECT_EQ(primitive->primitiveIndex,1u);EXPECT_EQ(primitive->materialIndex,4u);EXPECT_EQ(primitive->sourceNodeIndex,-1);
  EXPECT_EQ(plan.clonedRenderables,1u);
}

TEST(GraphCloneProjection,ReparentedSourceBelowAnEmptyNodeInheritsTheActualSharedParentScenes) {
  Fixture f;ASSERT_TRUE(f.graph.setParent(6,4));f.scenes={{2,8},{2,8}};
  f.graph.setActiveRoots(f.scenes[0]);f.graph.updateWorldTransforms();
  const auto plan=BuildGraphCloneProjection(f.input(6));
  EXPECT_EQ(plan.graph.getNode(plan.root)->parent,4u);
  EXPECT_EQ(plan.graph.getNode(plan.root)->worldTransform,f.graph.getNode(6)->worldTransform);
  EXPECT_EQ(plan.sourceProjection.affectedScenes,(std::vector<size_t>{0,1}));
  EXPECT_TRUE(plan.addedSceneRoots[0].empty());EXPECT_TRUE(plan.addedSceneRoots[1].empty());
  ASSERT_NE(f.row(plan,1),nullptr);EXPECT_EQ(f.row(plan,1)->parentSourceNode,-1);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[7])->meshOwnerGraphNode,plan.root);
  EXPECT_EQ(plan.oldToNewGraph[2],invalid);EXPECT_EQ(plan.oldToNewGraph[5],invalid);
}

TEST(GraphCloneProjection,MovedPrimitiveInsideSelectionGetsNonvisualExternalOwnerAndJointCopies) {
  Fixture f;const auto external=f.graph.createNode(glm::translate(glm::mat4(1),{4,0,0}),0,false,invalid,"Selected external container");
  f.graph.setParent(external,1);f.graph.setParent(3,external);f.scenes[0].push_back(external);
  f.graph.setActiveRoots(f.scenes[0]);f.graph.updateWorldTransforms();const auto before=f.graph.renderableNodes().size();
  const auto plan=BuildGraphCloneProjection(f.input(external));
  EXPECT_EQ(plan.clonedRenderables,1u);EXPECT_EQ(plan.graph.renderableNodes().size(),before+1);
  ASSERT_NE(f.row(plan,0),nullptr);EXPECT_EQ(f.row(plan,0)->role,Role::MeshBindingOnly);
  EXPECT_TRUE(f.row(plan,0)->retainMeshBinding);
  const auto owner=plan.oldToNewGraph[2];ASSERT_NE(owner,invalid);
  EXPECT_FALSE(plan.graph.getNode(owner)->renderable);EXPECT_TRUE(plan.graph.getNode(owner)->children.empty());
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[3])->parent,plan.root);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[3])->meshOwnerGraphNode,owner);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[3])->worldTransform,f.graph.getNode(3)->worldTransform);
  EXPECT_EQ(plan.oldToNewGraph[4],invalid);EXPECT_EQ(plan.oldToNewGraph[5],invalid);EXPECT_EQ(plan.oldToNewGraph[7],invalid);
  ASSERT_NE(f.row(plan,2),nullptr);EXPECT_EQ(f.row(plan,2)->role,Role::JointTransformOnly);
}

TEST(GraphCloneProjection,VisualOwnerWithAllPrimitivesMovedOutDropsItsUnusedSourceMeshBinding) {
  Fixture f;const auto external=f.graph.createNode(glm::mat4(1),0,false,invalid,"Elsewhere");
  f.graph.setParent(external,1);f.graph.setParent(3,external);f.scenes[0].push_back(external);
  f.graph.setActiveRoots(f.scenes[0]);f.graph.updateWorldTransforms();
  const auto plan=BuildGraphCloneProjection(f.input(2));
  ASSERT_NE(f.row(plan,0),nullptr);EXPECT_EQ(f.row(plan,0)->role,Role::Visual);EXPECT_FALSE(f.row(plan,0)->retainMeshBinding);
  EXPECT_EQ(plan.oldToNewGraph[3],invalid);EXPECT_EQ(plan.oldToNewGraph[8],invalid);
  EXPECT_EQ(plan.clonedRenderables,1u);EXPECT_TRUE(plan.dependencyRoots.empty());
  EXPECT_NE(plan.oldToNewGraph[5],invalid); // Actual procedural descendant remains part of the copy.
}

TEST(GraphCloneProjection,GraphOnlyJointAncestorContextsPreserveWorldAndDoNotBecomeVisualCopies) {
  Fixture f;const auto spacer=f.graph.createNode(glm::translate(glm::mat4(1),{0,0,7}),2,true,1,"External procedural ancestor");
  f.graph.setParent(spacer,1);f.graph.setParent(8,spacer);f.graph.updateWorldTransforms();
  const auto plan=BuildGraphCloneProjection(f.input(2));
  const auto newSpacer=plan.oldToNewGraph[spacer];ASSERT_NE(newSpacer,invalid);
  EXPECT_FALSE(plan.graph.getNode(newSpacer)->renderable);EXPECT_EQ(plan.graph.getNode(newSpacer)->primitiveIndex,invalid);
  EXPECT_EQ(plan.graph.getNode(newSpacer)->sourceNodeIndex,-1);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[8])->parent,newSpacer);
  EXPECT_EQ(plan.graph.getNode(plan.oldToNewGraph[8])->worldTransform,f.graph.getNode(8)->worldTransform);
  EXPECT_EQ(plan.clonedRenderables,2u);
}

TEST(GraphCloneProjection,RetiredDescendantsStayInTheOriginalSlotsAndAreNotDuplicated) {
  Fixture f;auto retire=f.graph.prepareRetireSubtree(5);ASSERT_TRUE(retire);ASSERT_TRUE(f.graph.publishRetirement(retire));
  const auto plan=BuildGraphCloneProjection(f.input(2));
  EXPECT_EQ(plan.oldToNewGraph[5],invalid);EXPECT_TRUE(plan.graph.isNodeRetired(5));
  EXPECT_TRUE(plan.graph.getNode(plan.oldToNewGraph[4])->children.empty());EXPECT_EQ(plan.clonedRenderables,1u);
}

TEST(GraphCloneProjection,ParentlessVisualRootsReceiveNewSourceIdsAndGlobalRootMembership) {
  Fixture f;ASSERT_TRUE(f.graph.setParent(2,std::nullopt));f.graph.updateWorldTransforms();
  const auto plan=BuildGraphCloneProjection(f.input(2));
  ASSERT_NE(f.row(plan,0),nullptr);EXPECT_EQ(f.row(plan,0)->parentSourceNode,-1);
  EXPECT_GE(plan.graph.getNode(plan.root)->sourceNodeIndex,int32_t(f.source.nodes.size()));
  EXPECT_EQ(plan.graph.getNode(plan.root)->parent,invalid);
  EXPECT_NE(std::ranges::find(plan.graph.rootNodes(),plan.root),plan.graph.rootNodes().end());
  EXPECT_NE(std::ranges::find(plan.sceneRoots[0],plan.root),plan.sceneRoots[0].end());
  EXPECT_EQ(plan.sceneRoots[1],f.scenes[1]);
}

TEST(GraphCloneProjection,OpaqueOwnershipProofAcceptsOnlyTheBoundSourceMapsRolesAndAttachments) {
  Fixture f;f.source.skins[0].extras=tinygltf::Value(1.0);f.source.scenes[0].extras=tinygltf::Value(1.0);
  const auto graph=BuildGraphCloneProjection(f.input(2));ASSERT_TRUE(graph.ownership);
  const auto source=container::geometry::gltf::PlanGltfProjectedSubtreeClone(f.source,f.source,graph.sourceProjection);
  EXPECT_NO_THROW(graph.ownership.validate(f.source,source));
  auto forged=source;forged.copyWeightTracks[0]=false;
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.nodes[0]=forged.clone.nodes[2];
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.roles[0]=Role::MeshBindingOnly;
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.authoredSource.nodes[size_t(forged.clone.nodes[0])].skin=0;
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.evaluatedSource.nodes[size_t(forged.clone.nodes[0])].children.push_back(0);
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.authoredSource.skins[1].extras=tinygltf::Value(std::nextafter(1.0,2.0));
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.authoredSource.scenes[0].extras=tinygltf::Value(std::nextafter(1.0,2.0));
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.evaluatedSource.scenes[0].nodes.pop_back();
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  forged=source;forged.clone.authoredSource.nodes[size_t(forged.clone.nodes[0])].extensions["KHR_node_visibility"]=
      tinygltf::Value(tinygltf::Value::Object{{"visible",tinygltf::Value(false)}});
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  EXPECT_GT(graph.ownership.retainedBytes(),0u);
}

TEST(GraphCloneProjection,OwnershipProofDistinguishesBindingOnlyAndVisualWithoutBindingFromJointContext) {
  Fixture f;const auto external=f.graph.createNode(glm::mat4(1),0,false,invalid,"Moved primitive container");
  f.graph.setParent(external,1);f.graph.setParent(3,external);f.scenes[0].push_back(external);
  f.graph.setActiveRoots(f.scenes[0]);f.graph.updateWorldTransforms();
  for(const auto root:{2u,external}) {
    const auto graph=BuildGraphCloneProjection(f.input(root));
    const auto source=container::geometry::gltf::PlanGltfProjectedSubtreeClone(f.source,f.source,graph.sourceProjection);
    EXPECT_NO_THROW(graph.ownership.validate(f.source,source));
    EXPECT_FALSE(source.clone.contextOnly[0]);
    EXPECT_EQ(source.copyWeightTracks[0],root==external);
    auto forged=source;forged.copyWeightTracks[0]=!forged.copyWeightTracks[0];
    EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  }
}

TEST(GraphCloneProjection,GraphOnlyOwnershipProofAllowsZeroSourceGrowthButRejectsInventedTail) {
  Fixture f;const auto graph=BuildGraphCloneProjection(f.input(4));ASSERT_TRUE(graph.ownership);
  const auto source=container::geometry::gltf::PlanGltfProjectedSubtreeClone(f.source,f.source,graph.sourceProjection);
  EXPECT_NO_THROW(graph.ownership.validate(f.source,source));
  auto forged=source;forged.clone.authoredSource.nodes.emplace_back();
  EXPECT_THROW(graph.ownership.validate(f.source,forged),std::runtime_error);
  container::geometry::gltf::GltfProjectedCloneOwnershipProof absent;
  EXPECT_FALSE(absent);EXPECT_THROW(absent.validate(f.source,source),std::runtime_error);
}

TEST(GraphCloneProjection,ActiveEditorAncestorCoversDependencyRootsWithoutChangingRootOrder) {
  Fixture f;f.scenes={{0},{0}};f.graph.setActiveRoots(f.scenes[0]);
  const auto plan=BuildGraphCloneProjection(f.input(2));
  EXPECT_FALSE(plan.dependencyRoots.empty());
  EXPECT_EQ(plan.sourceProjection.affectedScenes,(std::vector<size_t>{0,1}));
  EXPECT_EQ(plan.sceneRoots,f.scenes);EXPECT_TRUE(plan.addedSceneRoots[0].empty());
  EXPECT_TRUE(plan.addedSceneRoots[1].empty());EXPECT_EQ(*plan.graph.activeRoots(),f.scenes[0]);
  EXPECT_EQ(plan.graph.renderableNodes().size(),5u);
}

TEST(GraphCloneProjection,ContextDependenciesOutsideTheSceneRootRemainExplicitOnlyInAffectedScenes) {
  Fixture f;const auto plan=BuildGraphCloneProjection(f.input(2));
  ASSERT_EQ(plan.dependencyRoots.size(),1u);
  EXPECT_EQ(plan.addedSceneRoots[0],(std::vector<uint32_t>{plan.root,plan.dependencyRoots[0]}));
  EXPECT_TRUE(plan.addedSceneRoots[1].empty());EXPECT_EQ(plan.sceneRoots[1],f.scenes[1]);
  EXPECT_EQ(plan.sceneRoots[0],(std::vector<uint32_t>{2,6,8,plan.root,plan.dependencyRoots[0]}));
}

TEST(GraphCloneProjection,RejectsBadIdentityBudgetAndCatalogInputsWithoutChangingGraphOrSource) {
  Fixture f;const auto before=nodes(f.graph);const auto source=f.source;
  GraphCloneProjectionLimits limits;limits.maxOutputBytes=1;
  EXPECT_THROW((void)BuildGraphCloneProjection(f.input(2),limits),std::runtime_error);
  limits={};limits.maxWorkspaceBytes=1;EXPECT_THROW((void)BuildGraphCloneProjection(f.input(2),limits),std::runtime_error);
  limits={};limits.graph.maxActiveRenderableObjects=3;EXPECT_THROW((void)BuildGraphCloneProjection(f.input(2),limits),std::runtime_error);
  {auto input=f.input(2);input.runtimePrimitiveCount=2;EXPECT_THROW((void)BuildGraphCloneProjection(input),std::runtime_error);}
  {auto input=f.input(2);input.root=1;EXPECT_THROW((void)BuildGraphCloneProjection(input),std::runtime_error);}
  {auto input=f.input(2);input.rootName=std::string("\xc0\x80",2);EXPECT_THROW((void)BuildGraphCloneProjection(input),std::invalid_argument);}
  {auto badScenes=f.scenes;badScenes[0].push_back(2);auto input=f.input(2);input.sceneRoots=badScenes;
    EXPECT_THROW((void)BuildGraphCloneProjection(input),std::runtime_error);}
  EXPECT_EQ(nodes(f.graph),before);EXPECT_EQ(f.source,source);
}

TEST(GraphCloneProjection,LargeWideAndDeepGraphOnlyCopiesPreserveAnAbsentAllActiveView) {
  for(const bool deep:{false,true}) {
    SceneGraph graph;tinygltf::Model source;source.asset.version="2.0";
    const auto editor=graph.createNode(glm::mat4(1),0);graph.getNode(editor)->editorSceneRoot=true;
    const auto root=1u;const uint32_t total=deep?10000:20000;
    std::vector<SceneNode> tail(size_t(total)+1);tail[0].parent=editor;
    for(uint32_t n=0;n<total;++n) {
      auto& node=tail[size_t(n)+1];node.parent=deep?root+n:root;
      node.renderable=!deep;node.primitiveIndex=deep?invalid:0;node.name="N";
      tail[node.parent-root].children.push_back(root+n+1);
    }
    graph=graph.makeAppendCandidate(std::move(tail));
    graph.updateWorldTransforms();ASSERT_FALSE(graph.activeRoots());
    const std::vector<std::vector<uint32_t>> roots{{editor}};
    GraphCloneProjectionLimits limits;limits.graph.maxActiveRenderableObjects=50000;
    limits.graph.hierarchy.maxRenderableObjects=50000;
    const auto plan=BuildGraphCloneProjection({graph,source,root,roots,0,1,1},limits);
    EXPECT_FALSE(plan.graph.activeRoots());EXPECT_TRUE(plan.sourceProjection.nodes.empty());
    EXPECT_EQ(plan.graph.nodeCount(),graph.nodeCount()+total+1);
    EXPECT_EQ(plan.clonedRenderables,deep?0u:total);
  }
}
} // namespace
