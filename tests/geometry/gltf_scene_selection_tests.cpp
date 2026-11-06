#include <Container/utility/GltfSceneSelection.h>
#include <Container/utility/GltfSceneInstancing.h>
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>

namespace {
using namespace container::scene;
tinygltf::Model source() {
  tinygltf::Model model;
  model.meshes.resize(1);model.meshes[0].primitives.resize(1);
  model.nodes.resize(3);model.nodes[0].mesh=model.nodes[1].mesh=0;
  model.nodes[0].translation={-1,0,0};model.nodes[1].translation={1,0,0};
  model.nodes[1].children={2};
  model.scenes.resize(4);model.scenes[0].name=model.scenes[1].name="Named";
  model.scenes[0].nodes={0};model.scenes[1].nodes={1};model.scenes[2].nodes={0,1};
  model.defaultScene=1;
  return model;
}
SceneGraph authoringGraph(const tinygltf::Model& model) {
  return BuildGltfSceneHierarchy(model,{},std::array<uint32_t,1>{7},glm::mat4(1),{},GltfHierarchyScope::CompleteAsset);
}
}

TEST(GltfSceneSelection, CatalogHonorsAuthoredDefaultNamesAndSharedRoots) {
  const auto model=source();const auto catalog=BuildGltfSceneCatalog(model);
  ASSERT_EQ(catalog.entries.size(),4u);EXPECT_TRUE(catalog.authoredScenes);
  EXPECT_EQ(catalog.defaultScene,1u);EXPECT_EQ(catalog.entries[0].name,"Named");
  EXPECT_EQ(catalog.entries[1].name,"Named");EXPECT_EQ(catalog.entries[2].name,"Scene 2");
  EXPECT_EQ(catalog.entries[2].sourceRoots,(std::vector<int32_t>{0,1}));
  EXPECT_TRUE(catalog.entries[3].sourceRoots.empty());
}

TEST(GltfSceneSelection, RetiredSourceRootKeepsItsIdentityAndIsPrunedFromEveryScene) {
  const auto model = source();
  const auto catalog = BuildGltfSceneCatalog(model);
  auto graph = authoringGraph(model);
  graph.setActiveRoots(PlanGltfSceneActivation(catalog, graph, 0).runtimeRoots);
  uint32_t target = SceneGraph::kInvalidNode;
  for (uint32_t id = 0; id < graph.nodeCount(); ++id)
    if (graph.getNode(id)->sourceNodeIndex == 0) target = id;
  ASSERT_NE(target, SceneGraph::kInvalidNode);
  const auto count = graph.nodeCount();
  const auto identity = graph.identity();
  auto prepared = graph.prepareRetireSubtree(target);
  ASSERT_TRUE(prepared);
  ASSERT_TRUE(graph.publishRetirement(prepared));
  auto receipt = prepared.takeRetirement();
  ASSERT_TRUE(receipt);
  EXPECT_EQ(graph.nodeCount(), count);
  EXPECT_EQ(graph.identity(), identity);
  EXPECT_EQ(graph.getNode(target)->sourceNodeIndex, 0);
  EXPECT_EQ(PlanGltfSceneActivation(catalog, graph, 0, 1).renderableCount, 0u);
  EXPECT_EQ(PlanGltfSceneActivation(catalog, graph, 1, 1).renderableCount, 1u);
  EXPECT_EQ(PlanGltfSceneActivation(catalog, graph, 2, 1).renderableCount, 1u);
  auto undo = graph.prepareRetirementChange(*receipt, false);
  ASSERT_TRUE(graph.publishRetirement(undo));
  EXPECT_EQ(PlanGltfSceneActivation(catalog, graph, 0).renderableCount, 1u);
  EXPECT_EQ(PlanGltfSceneActivation(catalog, graph, 2).renderableCount, 2u);
}

TEST(GltfSceneSelection, SwitchingViewsPreservesOneHierarchyAndSharedGeometryBindings) {
  const auto model=source();const auto catalog=BuildGltfSceneCatalog(model);
  auto graph=authoringGraph(model);ASSERT_EQ(graph.nodeCount(),5u);
  const auto identity=graph.identity();
  std::vector<uint32_t> originalRenderables=graph.renderableNodes();
  ASSERT_EQ(originalRenderables.size(),2u);
  graph.setName(originalRenderables[0],"Edited while inactive");
  graph.setMaterialIndex(originalRenderables[0],99);
  for(const auto [scene,count]:{std::pair{1,1},std::pair{0,1},std::pair{2,2},std::pair{3,0},std::pair{0,1}}) {
    const auto plan=PlanGltfSceneActivation(catalog,graph,size_t(scene));
    EXPECT_EQ(plan.renderableCount,uint64_t(count));graph.setActiveRoots(plan.runtimeRoots);
    EXPECT_EQ(graph.renderableNodes().size(),size_t(count));EXPECT_EQ(graph.identity(),identity);
    EXPECT_EQ(graph.nodeCount(),5u);
    for(const auto id:graph.renderableNodes()) EXPECT_EQ(graph.getNode(id)->primitiveIndex,0u);
  }
  EXPECT_EQ(graph.getNode(originalRenderables[0])->materialIndex,99u);
  EXPECT_EQ(graph.getNode(originalRenderables[0])->name,"Edited while inactive");
  EXPECT_EQ(model.meshes.size(),1u);EXPECT_EQ(model.meshes[0].primitives.size(),1u);
}

TEST(GltfSceneSelection, CapacityOrMappingFailureLeavesTheCurrentViewUntouched) {
  const auto model=source();const auto catalog=BuildGltfSceneCatalog(model);
  auto graph=authoringGraph(model);const auto initial=PlanGltfSceneActivation(catalog,graph,1);
  graph.setActiveRoots(initial.runtimeRoots);
  const auto before=graph.renderableNodes();const auto revision=graph.revision();
  EXPECT_THROW((void)PlanGltfSceneActivation(catalog,graph,2,1),std::invalid_argument);
  EXPECT_THROW((void)PlanGltfSceneActivation(catalog,graph,99),std::invalid_argument);
  EXPECT_EQ(graph.revision(),revision);EXPECT_EQ(graph.renderableNodes(),before);
  auto ambiguous=graph;const auto duplicate=ambiguous.createNode(glm::mat4(1),0);
  ambiguous.getNode(duplicate)->sourceNodeIndex=0;
  EXPECT_THROW((void)PlanGltfSceneActivation(catalog,ambiguous,0),std::invalid_argument);
}

TEST(GltfSceneSelection, RejectsInvalidRootsDefaultsAndDisconnectedSourceCycles) {
  for(int mode=0;mode<5;++mode) {
    auto model=source();
    if(mode==0) model.defaultScene=99;
    if(mode==1) model.scenes[0].nodes={2}; // A scene root cannot have a node parent.
    if(mode==2) model.scenes[0].nodes={0,0};
    if(mode==3) model.nodes[0].children={2};
    if(mode==4) { model.nodes.resize(5);model.nodes[3].children={4};model.nodes[4].children={3}; }
    EXPECT_THROW((void)BuildGltfSceneCatalog(model),std::invalid_argument);
    if(mode==4) EXPECT_THROW((void)authoringGraph(model),std::invalid_argument);
  }
}

TEST(GltfSceneSelection, ImplicitAndExplicitEmptyScenesHaveDistinctActivation) {
  auto model=source();model.scenes.clear();model.defaultScene=-1;
  auto catalog=BuildGltfSceneCatalog(model);auto graph=authoringGraph(model);
  EXPECT_FALSE(catalog.authoredScenes);EXPECT_EQ(catalog.entries.size(),1u);
  EXPECT_EQ(PlanGltfSceneActivation(catalog,graph,0).renderableCount,2u);
  model.scenes.resize(1);model.scenes[0].nodes.clear();
  catalog=BuildGltfSceneCatalog(model);
  const auto empty=PlanGltfSceneActivation(catalog,graph,0,0);
  EXPECT_TRUE(catalog.authoredScenes);EXPECT_TRUE(empty.runtimeRoots.empty());
  graph.setActiveRoots(empty.runtimeRoots);EXPECT_TRUE(graph.renderableNodes().empty());
}

TEST(GltfSceneSelection, CatalogBoundsAndMeshlessHierarchyAreChecked) {
  const auto model=source();
  EXPECT_THROW((void)BuildGltfSceneCatalog(model,{2,100}),std::invalid_argument);
  EXPECT_THROW((void)BuildGltfSceneCatalog(model,{10,3}),std::invalid_argument);
  EXPECT_NO_THROW((void)BuildGltfSceneCatalog(model,{10,4}));
  tinygltf::Model cameras; cameras.nodes.resize(2);cameras.scenes.resize(1);cameras.scenes[0].nodes={0,1};
  const auto graph=BuildGltfSceneHierarchy(cameras,{}, {},glm::mat4(1),{},GltfHierarchyScope::CompleteAsset);
  EXPECT_EQ(graph.nodeCount(),2u);
  EXPECT_EQ(PlanGltfSceneActivation(BuildGltfSceneCatalog(cameras),graph,0,0).renderableCount,0u);
}

TEST(GltfSceneSelection, AddedRootsFollowTheirSceneAcrossUndoAndRedo) {
  const auto model=source();const auto catalog=BuildGltfSceneCatalog(model);
  auto graph=authoringGraph(model);
  const auto first=uint32_t(graph.nodeCount());
  const auto added=graph.createNode(glm::mat4(1),7,true,0,"Added in empty scene");
  const std::array extra{added,added};
  EXPECT_EQ(PlanGltfSceneActivation(catalog,graph,3,1,extra).renderableCount,1u);
  EXPECT_EQ(PlanGltfSceneActivation(catalog,graph,0,1).renderableCount,1u);
  auto removed=graph.detachTail(first); ASSERT_TRUE(removed);
  const auto empty=PlanGltfSceneActivation(catalog,graph,3,1,extra);
  EXPECT_TRUE(empty.runtimeRoots.empty());EXPECT_EQ(empty.renderableCount,0u);
  ASSERT_TRUE(graph.restoreTail(*removed));
  EXPECT_EQ(PlanGltfSceneActivation(catalog,graph,3,1,extra).runtimeRoots.size(),1u);
  EXPECT_THROW((void)PlanGltfSceneActivation(catalog,graph,2,2,extra),std::invalid_argument);
}

TEST(GltfSceneSelection, ReparentedRootsExportOnlyTopmostEntriesOfTheirView) {
  const auto model=source();const auto catalog=BuildGltfSceneCatalog(model);
  auto graph=authoringGraph(model);
  const auto first=PlanGltfSceneActivation(catalog,graph,0).runtimeRoots.front();
  const auto second=PlanGltfSceneActivation(catalog,graph,1).runtimeRoots.front();
  ASSERT_TRUE(graph.setParent(first,second));
  const auto plan=PlanGltfSceneActivation(catalog,graph,2);
  EXPECT_EQ(plan.runtimeRoots,std::vector<uint32_t>{second});
  EXPECT_EQ(plan.renderableCount,2u);
  graph.setActiveRoots(plan.runtimeRoots);EXPECT_TRUE(graph.isNodeActive(first));
}
