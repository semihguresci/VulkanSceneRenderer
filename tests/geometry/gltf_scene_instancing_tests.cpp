#include <Container/utility/GltfSceneInstancing.h>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>
#include <stdexcept>

namespace {
using namespace container::scene;
namespace gltf = container::geometry::gltf;

void near(const glm::mat4& actual, const glm::mat4& expected) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      EXPECT_NEAR(actual[column][row], expected[column][row], 1e-5f);
}

struct AttachmentFixture {
  SceneGraph graph;
  uint32_t parent{}, owner{}, authoredChild{};
  std::array<uint32_t, 2> materials{10, 11};
  gltf::NodeInstancing instances{7, 3, 2};
  AttachmentFixture() {
    parent = graph.createNode(glm::scale(glm::mat4(1), {2, 1, -3}), 0);
    owner = graph.createNode(glm::rotate(glm::translate(glm::mat4(1), {1, 2, 3}),
                                        .4f, glm::vec3(0, 1, 0)), 0);
    graph.getNode(owner)->sourceNodeIndex = 7;
    EXPECT_TRUE(graph.setParent(owner, parent));
    authoredChild = graph.createNode(glm::translate(glm::mat4(1), {0, 4, 0}), 0);
    EXPECT_TRUE(graph.setParent(authoredChild, owner));
    instances.localMatrices = {
        glm::translate(glm::mat4(1), {5, 0, 0}),
        glm::scale(glm::translate(glm::mat4(1), {-2, 0, 1}), {-1, 2, 1})};
  }
  GltfMeshAttachment input() const { return {owner, 7, 3, 5, materials, &instances}; }
};
} // namespace

TEST(GltfSceneInstancing, InstancesSharePrimitiveRangesAndLeaveAuthoredChildrenUnchanged) {
  AttachmentFixture f;
  f.graph.updateWorldTransforms();
  const auto ownerWorld = f.graph.getNode(f.owner)->worldTransform;
  const auto childWorld = f.graph.getNode(f.authoredChild)->worldTransform;
  const auto identity = f.graph.identity();
  const auto attached = AttachGltfMeshPrimitives(f.graph, f.input());
  ASSERT_EQ(attached.size(), 4u);
  ASSERT_EQ(f.graph.renderableNodes().size(), 4u); // No non-instanced fallback draw.
  EXPECT_EQ(f.graph.nodeCount(), 7u);
  EXPECT_EQ(f.graph.identity(), identity);
  f.graph.updateWorldTransforms();
  near(f.graph.getNode(f.owner)->worldTransform, ownerWorld);
  near(f.graph.getNode(f.authoredChild)->worldTransform, childWorld);
  for (uint32_t i = 0; i < attached.size(); ++i) {
    const auto& item = attached[i];
    const auto& node = *f.graph.getNode(item.graphNode);
    EXPECT_EQ(node.parent, f.owner);
    EXPECT_EQ(node.meshOwnerGraphNode, f.owner);
    EXPECT_EQ(node.primitiveIndex, 5u + i % 2);
    EXPECT_EQ(node.materialIndex, f.materials[i % 2]);
    EXPECT_EQ(node.sourceMeshIndex, 3);
    EXPECT_EQ(node.sourcePrimitiveIndex, int(i % 2));
    EXPECT_EQ(node.sourceNodeIndex, -1); // Animation continues to target the owner.
    ASSERT_TRUE(item.sourceInstance);
    EXPECT_EQ(*item.sourceInstance, (gltf::SourceInstanceIdentity{7, 3, i % 2, i / 2}));
    near(node.worldTransform, ownerWorld * f.instances.localMatrices[i / 2]);
  }
}

TEST(GltfSceneInstancing, OrdinaryUnmappedMeshesAttachOnceAndInheritVisibility) {
  SceneGraph graph;
  const auto owner = graph.createNode(glm::mat4(1), 0);
  const std::array<uint32_t, 2> materials{2, 3};
  const auto attached = AttachGltfMeshPrimitives(graph, {owner, -1, -1, 4, materials});
  ASSERT_EQ(attached.size(), 2u);
  for (uint32_t i = 0; i < attached.size(); ++i) {
    EXPECT_FALSE(attached[i].sourceInstance);
    const auto& node = *graph.getNode(attached[i].graphNode);
    EXPECT_EQ(node.primitiveIndex, 4u + i);
    EXPECT_EQ(node.sourceMeshIndex, -1);
    EXPECT_EQ(node.sourcePrimitiveIndex, -1);
    near(node.localTransform, glm::mat4(1));
  }
  graph.setVisible(owner, false);
  for (const auto& child : attached) EXPECT_FALSE(graph.isNodeEffectivelyVisible(child.graphNode));
  graph.setVisible(owner, true);
  for (const auto& child : attached) EXPECT_TRUE(graph.isNodeEffectivelyVisible(child.graphNode));
}

TEST(GltfSceneInstancing, ExplicitPrimitiveIdsKeepSourceProvenanceAcrossProceduralHoles) {
  tinygltf::Model source;
  source.meshes.resize(2);
  for (auto& mesh : source.meshes) mesh.primitives.resize(1);
  source.nodes.resize(2);
  source.nodes[0].mesh = 0; source.nodes[1].mesh = 1;
  tinygltf::Scene scene; scene.nodes = {0, 1}; source.scenes.push_back(scene); source.defaultScene = 0;
  const std::array<uint32_t, 2> materials{4, 7}, primitives{0, 2};
  const auto graph = BuildGltfSceneHierarchy(source, {}, materials, glm::mat4(1),
      {16, 32, 3, 8}, GltfHierarchyScope::CompleteAsset, primitives);
  ASSERT_EQ(graph.renderableNodes().size(), 2u);
  for (size_t i = 0; i < 2; ++i) {
    const auto& node = *graph.getNode(graph.renderableNodes()[i]);
    EXPECT_EQ(node.primitiveIndex, primitives[i]);
    EXPECT_EQ(node.sourceMeshIndex, int32_t(i));
    EXPECT_EQ(node.sourcePrimitiveIndex, 0);
    EXPECT_EQ(node.materialIndex, materials[i]);
  }
}

TEST(GltfSceneInstancing, InvalidExplicitAttachmentIdsLeaveGraphAndHistoryKeysUntouched) {
  AttachmentFixture f;
  const auto identity = f.graph.identity(), revision = f.graph.revision();
  const auto hierarchy = f.graph.hierarchyRevision();
  const auto count = f.graph.nodeCount();
  const auto children = f.graph.getNode(f.owner)->children;
  for (const std::array<uint32_t, 2> ids : {std::array<uint32_t, 2>{0, 0}, {0, 3}}) {
    auto input = f.input(); input.runtimePrimitiveIndices = ids;
    EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, input, {16, 32, 3, 12}), std::invalid_argument);
    EXPECT_EQ(f.graph.identity(), identity); EXPECT_EQ(f.graph.revision(), revision);
    EXPECT_EQ(f.graph.hierarchyRevision(), hierarchy); EXPECT_EQ(f.graph.nodeCount(), count);
    EXPECT_EQ(f.graph.getNode(f.owner)->children, children);
  }
  auto input = f.input(); const std::array<uint32_t, 1> shortIds{0}; input.runtimePrimitiveIndices = shortIds;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, input), std::invalid_argument);
  EXPECT_EQ(f.graph.revision(), revision);
}

TEST(GltfSceneInstancing, ExplicitInstanceRuntimeIdsDoNotReplaceAuthoredPrimitiveIdentity) {
  AttachmentFixture f;
  const std::array<uint32_t, 2> primitives{0, 2};
  auto input = f.input(); input.runtimePrimitiveIndices = primitives;
  const auto attached = AttachGltfMeshPrimitives(f.graph, input, {16, 32, 3, 12});
  ASSERT_EQ(attached.size(), 4u);
  for (uint32_t i = 0; i < attached.size(); ++i) {
    const auto& node = *f.graph.getNode(attached[i].graphNode);
    EXPECT_EQ(node.primitiveIndex, primitives[i % 2]);
    EXPECT_EQ(node.sourceMeshIndex, 3);
    EXPECT_EQ(node.sourcePrimitiveIndex, int32_t(i % 2));
    EXPECT_EQ(node.sourceInstanceIndex, int32_t(i / 2));
    ASSERT_TRUE(attached[i].sourceInstance);
    EXPECT_EQ(*attached[i].sourceInstance, (gltf::SourceInstanceIdentity{7, 3, i % 2, i / 2}));
    near(node.localTransform, f.instances.localMatrices[i / 2]);
  }
}

TEST(GltfSceneInstancing, LateValidationFailuresDoNotChangeGraphTopologyOrRevisions) {
  AttachmentFixture f;
  const auto identity = f.graph.identity(), revision = f.graph.revision();
  const auto children = f.graph.getNode(f.owner)->children;
  const auto checkUnchanged = [&] {
    EXPECT_EQ(f.graph.nodeCount(), 3u);
    EXPECT_TRUE(f.graph.renderableNodes().empty());
    EXPECT_EQ(f.graph.identity(), identity);
    EXPECT_EQ(f.graph.revision(), revision);
    EXPECT_EQ(f.graph.getNode(f.owner)->children, children);
  };
  GltfMeshAttachmentLimits limits;
  limits.maxRenderableObjects = 3;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, f.input(), limits), std::invalid_argument);
  checkUnchanged();
  limits = {}; limits.maxGraphNodes = 6;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, f.input(), limits), std::invalid_argument);
  checkUnchanged();
  limits = {}; limits.primitiveRangeCount = 6;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, f.input(), limits), std::invalid_argument);
  checkUnchanged();
  limits = {}; limits.runtimeMaterialCount = 11;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, f.input(), limits), std::invalid_argument);
  checkUnchanged();
  f.instances.localMatrices.back()[0][0] = std::numeric_limits<float>::infinity();
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, f.input()), std::invalid_argument);
  checkUnchanged();
}

TEST(GltfSceneInstancing, OwnerSourceAndInstanceDeclarationsMustAgree) {
  AttachmentFixture f;
  auto input = f.input(); input.sourceNodeIndex = 8;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, input), std::invalid_argument);
  input = f.input(); input.sourceMeshIndex = 4;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, input), std::invalid_argument);
  input = f.input(); input.ownerNode = SceneGraph::kInvalidNode;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, input), std::invalid_argument);
  f.instances.primitiveCount = 1;
  EXPECT_THROW((void)AttachGltfMeshPrimitives(f.graph, f.input()), std::invalid_argument);
  EXPECT_EQ(f.graph.nodeCount(), 3u);
  EXPECT_TRUE(f.graph.renderableNodes().empty());
}

TEST(GltfSceneInstancing, DetachedHierarchyComposesInstancesAndKeepsAuthoredChildrenOutsideInstances) {
  tinygltf::Model source;
  source.meshes.resize(1); source.meshes[0].primitives.resize(2);
  source.nodes.resize(3); source.nodes[0].children={1}; source.nodes[0].scale={2,.5,-1};
  source.nodes[1].mesh=0; source.nodes[1].children={2}; source.nodes[1].translation={1,0,0};
  source.nodes[2].mesh=0; source.nodes[2].translation={0,3,0};
  source.scenes.resize(1); source.scenes[0].nodes={0}; source.defaultScene=0;
  gltf::GltfInstancingData data;
  data.nodes.push_back({1,0,2,{glm::translate(glm::mat4(1),{3,0,0}),
      glm::scale(glm::mat4(1),{-1,2,1})}});
  const std::array<uint32_t,2> materials{4,5};
  const auto import=glm::scale(glm::mat4(1),glm::vec3(3));
  auto graph=BuildGltfSceneHierarchy(source,data,materials,import,{6,50,2,6});
  ASSERT_EQ(graph.renderableNodes().size(),6u); EXPECT_EQ(graph.nodeCount(),10u);
  uint32_t instanceCount=0,ordinaryCount=0;
  for(auto id:graph.renderableNodes()) {
    const auto* node=graph.getNode(id);
    if(node->sourceInstanceIndex>=0) {
      ++instanceCount;
      EXPECT_EQ(node->sourceMeshIndex,0); EXPECT_EQ(node->sourcePrimitiveIndex,int(node->primitiveIndex));
      near(node->worldTransform,import*glm::scale(glm::mat4(1),{2,.5,-1})*
          glm::translate(glm::mat4(1),{1,0,0})*data.nodes[0].localMatrices[size_t(node->sourceInstanceIndex)]);
    } else {
      ++ordinaryCount;
      near(node->worldTransform,import*glm::scale(glm::mat4(1),{2,.5,-1})*
          glm::translate(glm::mat4(1),{1,3,0}));
    }
  }
  EXPECT_EQ(instanceCount,4u); EXPECT_EQ(ordinaryCount,2u);
  graph.setVisible(2,false); // Imported instance owner, before synthetic children.
  for(auto id:graph.renderableNodes()) EXPECT_FALSE(graph.isNodeEffectivelyVisible(id));
  EXPECT_THROW((void)BuildGltfSceneHierarchy(source,data,materials,import,{5,50,2,6}),std::invalid_argument);
  source.nodes[2].children={0};
  EXPECT_THROW((void)BuildGltfSceneHierarchy(source,data,materials,import),std::invalid_argument);
}
TEST(GltfSceneInstancing, SingularInstancesRejectDetachedPublicationButOrdinaryDefaultsStillBuild) {
  tinygltf::Model source; source.meshes.resize(1); source.meshes[0].primitives.resize(1);
  source.nodes.resize(1); source.nodes[0].mesh=0;
  source.scenes.resize(1); source.scenes[0].nodes={0};
  gltf::GltfInstancingData data; data.nodes.push_back({0,0,1,{glm::scale(glm::mat4(1),{1,1,0})}});
  const std::array<uint32_t,1> materials{0};
  EXPECT_THROW((void)BuildGltfSceneHierarchy(source,data,materials),std::invalid_argument);
  const auto ordinary=BuildGltfSceneHierarchy(source,{},materials);
  ASSERT_EQ(ordinary.renderableNodes().size(),1u);
  near(ordinary.getNode(ordinary.renderableNodes()[0])->localTransform,glm::mat4(1));
  source.nodes.clear();
  const auto fallback=BuildGltfSceneHierarchy(source,{},materials);
  ASSERT_EQ(fallback.renderableNodes().size(),1u);
  EXPECT_EQ(fallback.getNode(fallback.renderableNodes()[0])->sourceMeshIndex,0);
}

TEST(GltfSceneInstancing, LocalNodeFlagsBelongToSourceGroupsAndRouteAllInstancesThroughAncestors) {
  using Value=tinygltf::Value;
  using Object=Value::Object;
  tinygltf::Model source;
  source.extensionsUsed={"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"};
  source.meshes.resize(1); source.meshes[0].primitives.resize(1);
  source.nodes.resize(3); source.nodes[0].children={1,2};
  source.nodes[1].mesh=source.nodes[2].mesh=0;
  source.nodes[0].extensions["KHR_node_hoverability"]=Value(Object{{"hoverable",Value(false)}});
  source.nodes[1].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
  source.nodes[1].extensions["KHR_node_selectability"]=Value(Object{{"selectable",Value(false)}});
  source.nodes[1].extensions["KHR_node_hoverability"]=Value(Object{{"hoverable",Value(true)}});
  source.scenes.resize(1); source.scenes[0].nodes={0};
  gltf::GltfInstancingData instances;
  instances.nodes.push_back({1,0,1,{glm::mat4(1),glm::translate(glm::mat4(1),{2,0,0})}});
  const std::array<uint32_t,1> materials{0};
  auto graph=BuildGltfSceneHierarchy(source,instances,materials,
      glm::scale(glm::mat4(1),glm::vec3(2)));
  std::array<uint32_t,3> groups{SceneGraph::kInvalidNode,SceneGraph::kInvalidNode,SceneGraph::kInvalidNode};
  for(uint32_t i=0;i<graph.nodeCount();++i)
    if(graph.getNode(i)->sourceNodeIndex>=0) groups[size_t(graph.getNode(i)->sourceNodeIndex)]=i;
  for(const auto id:groups) ASSERT_NE(id,SceneGraph::kInvalidNode);
  EXPECT_FALSE(graph.getNode(groups[0])->hoverable);
  EXPECT_FALSE(graph.getNode(groups[1])->visible); EXPECT_FALSE(graph.getNode(groups[1])->selectable);
  EXPECT_TRUE(graph.getNode(groups[1])->hoverable);
  EXPECT_TRUE(graph.getNode(groups[2])->selectable); EXPECT_TRUE(graph.getNode(groups[2])->hoverable);
  ASSERT_EQ(graph.renderableNodes().size(),3u);
  uint32_t instanceCount=0;
  for(const auto id:graph.renderableNodes()) {
    const auto* node=graph.getNode(id);
    EXPECT_TRUE(node->visible); EXPECT_TRUE(node->selectable); EXPECT_TRUE(node->hoverable);
    EXPECT_FALSE(node->interactionRootBoundary);
    EXPECT_EQ(graph.nearestHoverableAncestor(id),SceneGraph::kInvalidNode);
    if(node->sourceInstanceIndex>=0) {
      ++instanceCount;
      EXPECT_EQ(graph.nearestSelectableAncestor(id),groups[0]);
      EXPECT_FALSE(graph.isNodeEffectivelyVisible(id));
    } else {
      EXPECT_EQ(graph.nearestSelectableAncestor(id),id);
      EXPECT_TRUE(graph.isNodeEffectivelyVisible(id));
    }
  }
  EXPECT_EQ(instanceCount,2u);
  graph.setHoverable(groups[0],true);
  for(const auto id:graph.renderableNodes()) EXPECT_EQ(graph.nearestHoverableAncestor(id),id);
}

TEST(GltfSceneInstancing, StrictNodeFlagParserRejectsMalformedInactiveNodesBeforeHierarchyPublication) {
  using Value=tinygltf::Value;
  using Object=Value::Object;
  tinygltf::Model source;
  source.extensionsUsed={"KHR_node_selectability"};
  source.meshes.resize(1); source.meshes[0].primitives.resize(1);
  source.nodes.resize(2); source.nodes[0].mesh=0;
  source.scenes.resize(1); source.scenes[0].nodes={0};
  source.nodes[1].extensions["KHR_node_selectability"]=Value(Object{{"selectable",Value(false)}});
  const std::array<uint32_t,1> materials{0};
  const auto active=BuildGltfSceneHierarchy(source,{},materials);
  const auto complete=BuildGltfSceneHierarchy(source,{},materials,glm::mat4(1),{},GltfHierarchyScope::CompleteAsset);
  EXPECT_EQ(complete.nodeCount(),active.nodeCount()+1);
  source.nodes[1].extensions["KHR_node_selectability"]=Value(Object{{"selectable",Value(0)}});
  EXPECT_THROW((void)BuildGltfSceneHierarchy(source,{},materials),std::runtime_error);
  source.nodes[1].extensions["KHR_node_selectability"]=Value(Object{});
  source.extensionsUsed.clear();
  EXPECT_THROW((void)BuildGltfSceneHierarchy(source,{},materials),std::runtime_error);
}

TEST(GltfSceneInstancing, EmptyFlagObjectsDefaultTrueAndSyntheticImportBoundariesPreserveNormalHits) {
  using Value=tinygltf::Value;
  tinygltf::Model source;
  source.extensionsUsed={"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"};
  source.nodes.resize(1); source.scenes.resize(1); source.scenes[0].nodes={0};
  for(const auto& extension:source.extensionsUsed) source.nodes[0].extensions[extension]=Value(Value::Object{});
  const auto meshless=BuildGltfSceneHierarchy(source,{},{});
  ASSERT_EQ(meshless.nodeCount(),1u);
  EXPECT_TRUE(meshless.getNode(0)->visible); EXPECT_TRUE(meshless.getNode(0)->selectable);
  EXPECT_TRUE(meshless.getNode(0)->hoverable); EXPECT_FALSE(meshless.getNode(0)->interactionRootBoundary);
  source={};
  const std::array<uint32_t,1> materials{0};
  const auto fallback=BuildGltfSceneHierarchy(source,{},materials);
  ASSERT_EQ(fallback.nodeCount(),2u); ASSERT_EQ(fallback.renderableNodes().size(),1u);
  const auto hit=fallback.renderableNodes()[0];
  EXPECT_TRUE(fallback.getNode(fallback.getNode(hit)->parent)->interactionRootBoundary);
  EXPECT_EQ(fallback.nearestSelectableAncestor(hit),hit);
  EXPECT_EQ(fallback.nearestHoverableAncestor(hit),hit);
}
