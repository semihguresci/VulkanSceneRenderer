#include <Container/utility/GltfMergeGraph.h>
#include <Container/utility/SceneEditHistory.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace container::scene;
namespace gltf = container::geometry::gltf;
constexpr auto invalid = SceneGraph::kInvalidNode;

tinygltf::Model source(std::string name, bool twoScenes = false) {
  tinygltf::Model model;
  const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
  tinygltf::Buffer buffer; buffer.data.resize(sizeof(positions));
  std::memcpy(buffer.data.data(), positions.data(), sizeof(positions));
  model.buffers.push_back(std::move(buffer));
  tinygltf::BufferView view; view.buffer = 0; view.byteLength = sizeof(positions);
  model.bufferViews.push_back(view);
  tinygltf::Accessor accessor; accessor.sparse = {}; accessor.bufferView = 0;
  accessor.count = 3; accessor.type = TINYGLTF_TYPE_VEC3;
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  model.accessors.push_back(accessor);
  model.materials.emplace_back();
  tinygltf::Primitive primitive; primitive.attributes["POSITION"] = 0;
  primitive.mode = TINYGLTF_MODE_TRIANGLES; primitive.material = 0;
  tinygltf::Mesh mesh; mesh.primitives.push_back(primitive); model.meshes.push_back(mesh);
  tinygltf::Node node; node.name = name; node.mesh = 0; node.translation = {1, 2, 3};
  model.nodes.push_back(node);
  tinygltf::Scene scene; scene.nodes = {0}; model.scenes.push_back(scene); model.defaultScene = 0;
  if (twoScenes) {
    node.name = name + " inactive"; node.translation = {-4, 0, 0}; model.nodes.push_back(node);
    scene.nodes = {1}; model.scenes.push_back(scene);
  }
  return model;
}

std::array<double, 16> matrixValues(const glm::mat4& matrix) {
  std::array<double, 16> result{};
  for (size_t i = 0; i < result.size(); ++i) result[i] = matrix[int(i / 4)][int(i % 4)];
  return result;
}

void near(const glm::mat4& actual, const glm::mat4& expected) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) EXPECT_NEAR(actual[c][r], expected[c][r], 1e-5f);
}

std::vector<SceneNode> nodes(const SceneGraph& graph) {
  std::vector<SceneNode> result;
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) result.push_back(*graph.getNode(id));
  return result;
}

void retained(const std::vector<SceneNode>& previous, const SceneGraph& graph, uint32_t editor) {
  for (uint32_t id = 0; id < previous.size(); ++id) {
    auto expected = previous[id];
    if (id == editor) {
      ASSERT_EQ(graph.getNode(id)->children.size(), expected.children.size() + 1);
      EXPECT_TRUE(std::equal(expected.children.begin(), expected.children.end(), graph.getNode(id)->children.begin()));
      expected.children = graph.getNode(id)->children;
    }
    EXPECT_EQ(*graph.getNode(id), expected) << id;
  }
}

struct Fixture {
  tinygltf::Model original{source("A")}, incoming{source("B", true)};
  SceneGraph graph;
  uint32_t editor{}, group{}, procedural{}, inactive{};
  glm::mat4 scale{glm::scale(glm::mat4(1), glm::vec3(2))};
  const std::array<uint32_t, 2> materials{5, 7}, primitives{0, 2};
  GltfMergeGraphLimits limits{{32, 128, 4, 16}, 8};
  Fixture() {
    graph = BuildGltfSceneHierarchy(original, {}, std::span(materials).first(1), scale,
        limits.hierarchy, GltfHierarchyScope::CompleteAsset, std::span(primitives).first(1));
    for (uint32_t id = 0; id < graph.nodeCount(); ++id)
      if (graph.getNode(id)->sourceNodeIndex == 0) group = id;
    const auto roots = graph.rootNodes();
    editor = graph.createNode(glm::mat4(1), 0, false, invalid, "Edited editor root");
    graph.getNode(editor)->editorSceneRoot = true;
    for (const auto root : roots) EXPECT_TRUE(graph.setParent(root, editor));
    procedural = graph.createNode(glm::translate(glm::mat4(1), {11, 12, 13}), 11, true, 1, "Procedural sphere");
    inactive = graph.createNode(glm::mat4(1), 12, true, 1, "Old inactive procedural");
    EXPECT_TRUE(graph.setParent(procedural, editor)); EXPECT_TRUE(graph.setParent(inactive, editor));
    graph.setActiveRoots(std::array<uint32_t, 2>{group, procedural});
    auto edited = glm::translate(glm::mat4(1), {4, -3, 2});
    edited[1][0] = .375f; edited[2][1] = -.625f;
    graph.setLocalTransform(group, edited);
    graph.setName(group, "Renamed old group"); graph.setSelectable(group, false);
    graph.setHoverable(procedural, false); graph.setVisible(inactive, false);
    graph.updateWorldTransforms();
  }
  gltf::GltfMergePlan plan(const gltf::GltfMergeOptions& options = {}) const {
    return gltf::PlanGltfMerge(original, incoming, options);
  }
  GltfMergeGraphCandidate build(const gltf::GltfMergePlan& plan) const {
    return BuildGltfMergeGraphCandidate(graph, plan, original.nodes.size(), materials, primitives, {}, scale, limits);
  }
};
} // namespace

TEST(GltfMergeGraph, RetainsEditedProceduralPayloadAndOldHistoryAcrossPreparedPublication) {
  Fixture f;
  container::util::SceneEditHistory history(f.graph);
  ASSERT_TRUE(history.setVisible(f.procedural, false));
  const auto before = nodes(f.graph);
  const auto roots = f.graph.rootNodes();
  const auto active = *f.graph.activeRoots();
  const auto identity = f.graph.identity(), revision = f.graph.revision();
  const auto plan = f.plan();
  auto candidate = f.build(plan);
  EXPECT_EQ(f.graph.identity(), identity); EXPECT_EQ(f.graph.revision(), revision);
  EXPECT_EQ(nodes(f.graph), before);
  retained(before, candidate.graph, f.editor);
  EXPECT_EQ(candidate.graph.rootNodes(), roots);
  ASSERT_TRUE(candidate.graph.activeRoots());
  EXPECT_TRUE(std::equal(active.begin(), active.end(), candidate.graph.activeRoots()->begin()));
  EXPECT_EQ(candidate.graph.activeRoots()->back(), candidate.placementRoot);
  EXPECT_EQ(candidate.placementRoot, candidate.sourceNodeToGraph[size_t(plan.parentNode)]);
  auto proof = f.graph.prepareAppend(std::move(candidate.graph));
  ASSERT_TRUE(f.graph.publishAppend(proof));
  EXPECT_EQ(f.graph.identity(), identity); retained(before, f.graph, f.editor);
  ASSERT_TRUE(history.undo()); EXPECT_TRUE(f.graph.getNode(f.procedural)->visible);
  ASSERT_TRUE(history.redo()); EXPECT_FALSE(f.graph.getNode(f.procedural)->visible);
}

TEST(GltfMergeGraph, CanonicalScenesStayInactiveWhileOnlySelectedClosureIsPlaced) {
  Fixture f; const auto plan = f.plan(); auto result = f.build(plan);
  ASSERT_EQ(result.canonicalRoots.size(), 2u);
  ASSERT_EQ(result.sceneRoots.size(), 3u);
  EXPECT_EQ(result.sourceNodeToGraph[0], f.group);
  for (const auto root : result.canonicalRoots) {
    EXPECT_EQ(result.graph.getNode(root)->parent, result.importRoot);
    EXPECT_FALSE(result.graph.isNodeActive(root));
  }
  EXPECT_EQ(result.sceneRoots[1], (std::vector<uint32_t>{result.sourceNodeToGraph[1]}));
  EXPECT_EQ(result.sceneRoots[2], (std::vector<uint32_t>{result.sourceNodeToGraph[2]}));
  EXPECT_EQ(plan.placement.nodes[1], -1);
  EXPECT_TRUE(result.graph.isNodeActive(result.sourceNodeToGraph[size_t(plan.placement.nodes[0])]));
  EXPECT_FALSE(result.graph.isNodeActive(f.inactive));
  ASSERT_EQ(result.graph.renderableNodes().size(), 3u);
  const auto placed = result.sourceNodeToGraph[size_t(plan.placement.nodes[0])];
  const auto primitive = result.graph.getNode(placed)->children.front();
  EXPECT_EQ(result.graph.getNode(primitive)->primitiveIndex, 2u);
  EXPECT_EQ(result.graph.getNode(primitive)->materialIndex, 7u);
  EXPECT_EQ(result.graph.getNode(primitive)->meshOwnerGraphNode, placed);
  EXPECT_EQ(result.graph.getNode(primitive)->sourceMeshIndex, 1);
}

TEST(GltfMergeGraph, PlacementAndSessionScaleFollowEditorTransformOnceWithoutOldAssetWrapper) {
  Fixture f;
  const auto editorTransform = glm::rotate(glm::scale(glm::translate(glm::mat4(1), {5, 6, 7}),
      glm::vec3(-2, 3, 4)), .5f, glm::vec3(0, 1, 0));
  f.graph.setLocalTransform(f.editor, editorTransform); f.graph.updateWorldTransforms();
  const auto before = nodes(f.graph);
  const auto placement = glm::scale(glm::translate(glm::mat4(1), {8, 9, 10}), glm::vec3(-1, 2, .5f));
  gltf::GltfMergeOptions options; options.parentName = "Placed B"; options.placementMatrix = matrixValues(placement);
  const auto plan = f.plan(options); const auto result = f.build(plan);
  const auto selected = result.sourceNodeToGraph[size_t(plan.placement.nodes[0])];
  near(result.graph.getNode(selected)->worldTransform,
       editorTransform * f.scale * placement * glm::translate(glm::mat4(1), {1, 2, 3}));
  near(result.graph.getNode(result.canonicalRoots[0])->worldTransform,
       editorTransform * f.scale * glm::translate(glm::mat4(1), {1, 2, 3}));
  EXPECT_EQ(result.graph.getNode(result.placementRoot)->name, "Placed B");
  EXPECT_EQ(result.graph.getNode(result.importRoot)->localTransform, f.scale);
  EXPECT_EQ(result.graph.getNode(result.importRoot)->parent, f.editor);
  retained(before, result.graph, f.editor);
}

TEST(GltfMergeGraph, EmptySessionCreatesOnlyNewEditorAndImportWrappers) {
  SceneGraph base; const tinygltf::Model empty;
  const auto plan = gltf::PlanGltfMerge(empty, source("B"));
  const std::array<uint32_t, 1> materials{3}, primitives{1};
  auto result = BuildGltfMergeGraphCandidate(base, plan, 0, materials, primitives, {}, glm::mat4(1), {{16, 32, 2, 4}, 4});
  EXPECT_TRUE(result.graph.getNode(result.editorRoot)->editorSceneRoot);
  EXPECT_EQ(result.graph.rootNodes(), (std::vector<uint32_t>{result.editorRoot}));
  EXPECT_EQ(*result.graph.activeRoots(), (std::vector<uint32_t>{result.placementRoot}));
  ASSERT_EQ(result.canonicalRoots.size(), 1u); EXPECT_FALSE(result.graph.isNodeActive(result.canonicalRoots[0]));
  EXPECT_EQ(base.nodeCount(), 0u);
  auto proof = base.prepareAppend(std::move(result.graph)); EXPECT_TRUE(base.publishAppend(proof));
  EXPECT_EQ(base.renderableNodes().size(), 1u);
}

TEST(GltfMergeGraph, ProceduralEditorViewsRefineOriginalChildrenForExplicitAndAllActiveSessions) {
  for (const bool explicitView : {false, true}) {
    SCOPED_TRACE(explicitView);
    SceneGraph base; const auto editor = base.createNode(glm::mat4(1), 0);
    base.getNode(editor)->editorSceneRoot = true;
    const auto old = base.createNode(glm::mat4(1), 1, true, 0);
    ASSERT_TRUE(base.setParent(old, editor)); base.updateWorldTransforms();
    if (explicitView) base.setActiveRoots(std::array<uint32_t, 1>{editor});
    const tinygltf::Model empty; const auto plan = gltf::PlanGltfMerge(empty, source("B"));
    const std::array<uint32_t, 1> materials{2}, primitives{1};
    auto result = BuildGltfMergeGraphCandidate(base, plan, 0, materials, primitives, {}, glm::mat4(1), {{16, 32, 2, 4}, 4});
    EXPECT_EQ(*result.graph.activeRoots(), (std::vector<uint32_t>{old, result.placementRoot}));
    EXPECT_TRUE(result.graph.isNodeActive(old)); EXPECT_FALSE(result.graph.isNodeActive(editor));
    EXPECT_TRUE(result.graph.isNodeInActiveHierarchy(editor));
    EXPECT_FALSE(result.graph.isNodeActive(result.canonicalRoots[0]));
    auto proof = base.prepareAppend(std::move(result.graph)); EXPECT_TRUE(base.publishAppend(proof));
  }
}

TEST(GltfMergeGraph, RepeatedImportAppendsNewWrapperAndKeepsPreviousPlacementAndCanonicalIdentities) {
  Fixture f; const auto firstPlan = f.plan(); auto first = f.build(firstPlan);
  const auto firstPlacement = first.placementRoot;
  const auto firstCanonical = first.canonicalRoots;
  auto proof = f.graph.prepareAppend(std::move(first.graph)); ASSERT_TRUE(f.graph.publishAppend(proof));
  const auto before = nodes(f.graph);
  const auto active = *f.graph.activeRoots();
  const auto secondPlan = gltf::PlanGltfMerge(firstPlan.model, f.incoming);
  const std::array<uint32_t, 3> materials{5, 7, 9}, primitives{0, 2, 3};
  auto second = BuildGltfMergeGraphCandidate(f.graph, secondPlan, firstPlan.model.nodes.size(),
      materials, primitives, {}, f.scale, f.limits);
  retained(before, second.graph, f.editor);
  EXPECT_TRUE(std::equal(active.begin(), active.end(), second.graph.activeRoots()->begin()));
  EXPECT_TRUE(second.graph.isNodeActive(firstPlacement));
  for (const auto canonical : firstCanonical) EXPECT_FALSE(second.graph.isNodeActive(canonical));
  for (const auto canonical : second.canonicalRoots) EXPECT_FALSE(second.graph.isNodeActive(canonical));
  EXPECT_EQ(second.sourceNodeToGraph[size_t(firstPlan.parentNode)], firstPlacement);
  auto secondProof = f.graph.prepareAppend(std::move(second.graph)); EXPECT_TRUE(f.graph.publishAppend(secondProof));
}

TEST(GltfMergeGraph, NewInstanceChildrenRemapOwnersAndKeepSharedRuntimeGeometry) {
  Fixture f; const auto plan = f.plan();
  gltf::GltfInstancingData instances;
  for (const auto sourceNode : {plan.imported.nodes[0], plan.placement.nodes[0]}) {
    gltf::NodeInstancing item{uint32_t(sourceNode), 1, 1};
    item.localMatrices = {glm::mat4(1), glm::translate(glm::mat4(1), {4, 0, 0})};
    instances.nodes.push_back(std::move(item));
  }
  const auto result = BuildGltfMergeGraphCandidate(f.graph, plan, 1, f.materials, f.primitives, instances, f.scale, f.limits);
  const auto owner = result.sourceNodeToGraph[size_t(plan.placement.nodes[0])];
  ASSERT_EQ(result.graph.getNode(owner)->children.size(), 2u);
  for (int i = 0; i < 2; ++i) {
    const auto& child = *result.graph.getNode(result.graph.getNode(owner)->children[size_t(i)]);
    EXPECT_EQ(child.parent, owner); EXPECT_EQ(child.meshOwnerGraphNode, owner);
    EXPECT_EQ(child.primitiveIndex, 2u); EXPECT_EQ(child.sourceInstanceIndex, i);
  }
}

TEST(GltfMergeGraph, InvalidSourceBindingsAndMapsLeaveLivePayloadAndHistoryKeysUnchanged) {
  Fixture f; const auto plan = f.plan();
  const auto check = [&](const SceneGraph& graph, size_t retainedCount, std::span<const uint32_t> primitives) {
    const auto before = nodes(graph);
    const auto roots = graph.rootNodes(); const auto view = graph.activeRoots();
    const auto identity = graph.identity(), revision = graph.revision();
    EXPECT_THROW((void)BuildGltfMergeGraphCandidate(graph, plan, retainedCount, f.materials, primitives, {}, f.scale, f.limits), std::invalid_argument);
    EXPECT_EQ(nodes(graph), before); EXPECT_EQ(graph.rootNodes(), roots); EXPECT_EQ(graph.activeRoots(), view);
    EXPECT_EQ(graph.identity(), identity); EXPECT_EQ(graph.revision(), revision);
  };
  for (const std::array<uint32_t, 2> mapping : {std::array<uint32_t, 2>{0, 0}, {0, 4}, {2, 0}}) check(f.graph, 1, mapping);
  auto duplicate = f.graph; duplicate.getNode(f.procedural)->sourceNodeIndex = 0; check(duplicate, 1, f.primitives);
  auto missing = f.graph; missing.getNode(f.group)->sourceNodeIndex = -1; check(missing, 1, f.primitives);
  check(f.graph, 0, f.primitives);
  auto adopted = plan; adopted.model.nodes[size_t(adopted.parentNode)].children.push_back(0);
  EXPECT_THROW((void)f.build(adopted), std::invalid_argument);
  auto canonicalInPlacement = plan;
  canonicalInPlacement.model.scenes[1].nodes.clear();
  canonicalInPlacement.model.nodes[size_t(plan.parentNode)].children.push_back(plan.imported.nodes[0]);
  EXPECT_THROW((void)f.build(canonicalInPlacement), std::invalid_argument);
  auto orphanedPlacement = plan; orphanedPlacement.model.nodes[size_t(plan.parentNode)].children.clear();
  EXPECT_THROW((void)f.build(orphanedPlacement), std::invalid_argument);
}

TEST(GltfMergeGraph, AggregateAndActiveLimitsRejectWithoutCopyingLiveState) {
  Fixture f; const auto plan = f.plan(); const auto before = nodes(f.graph);
  for (int mode = 0; mode < 3; ++mode) {
    auto limits = f.limits;
    if (mode == 0) limits.maxActiveRenderableObjects = 2;
    if (mode == 1) limits.hierarchy.maxRenderableObjects = 5;
    if (mode == 2) limits.hierarchy.maxGraphNodes = f.graph.nodeCount() + 7;
    EXPECT_THROW((void)BuildGltfMergeGraphCandidate(f.graph, plan, 1, f.materials, f.primitives, {}, f.scale, limits), std::invalid_argument);
    EXPECT_EQ(nodes(f.graph), before);
  }
}

TEST(GltfMergeGraph, AppendedGroupsRetainIndependentSourceFlagsAndDoNotOverwriteOldEdits) {
  Fixture f;
  for (const auto [extension, property] : {
           std::pair{"KHR_node_visibility", "visible"},
           std::pair{"KHR_node_selectability", "selectable"},
           std::pair{"KHR_node_hoverability", "hoverable"}}) {
    f.incoming.extensionsUsed.emplace_back(extension);
    f.incoming.nodes[0].extensions[extension] = tinygltf::Value(tinygltf::Value::Object{
        {property, tinygltf::Value(false)}});
  }
  const auto plan = f.plan(); const auto result = f.build(plan);
  for (const auto sourceId : {plan.imported.nodes[0], plan.placement.nodes[0]}) {
    const auto& group = *result.graph.getNode(result.sourceNodeToGraph[size_t(sourceId)]);
    EXPECT_FALSE(group.visible); EXPECT_FALSE(group.selectable); EXPECT_FALSE(group.hoverable);
    const auto& primitive = *result.graph.getNode(group.children.front());
    EXPECT_TRUE(primitive.visible); EXPECT_TRUE(primitive.selectable); EXPECT_TRUE(primitive.hoverable);
  }
  EXPECT_EQ(result.graph.getNode(f.group)->selectable, f.graph.getNode(f.group)->selectable);
  EXPECT_EQ(result.graph.getNode(f.procedural)->hoverable, f.graph.getNode(f.procedural)->hoverable);
}

TEST(GltfMergeGraph, EditorComposedWorldOverflowRejectsWithoutChangingRetainedMatrices) {
  Fixture f; f.graph.setLocalTransform(f.editor, glm::scale(glm::mat4(1), {1e10f, 1, 1})); f.graph.updateWorldTransforms();
  gltf::GltfMergeOptions options; options.placementMatrix = matrixValues(glm::scale(glm::mat4(1), {1e38f, 1, 1}));
  const auto plan = f.plan(options); const auto before = nodes(f.graph);
  EXPECT_THROW((void)f.build(plan), std::invalid_argument); EXPECT_EQ(nodes(f.graph), before);
}

TEST(GltfMergeGraph, SingularEditorRejectsOtherwiseValidInstancedIncomingWorlds) {
  Fixture f;
  f.graph.setLocalTransform(f.editor, glm::scale(glm::mat4(1), {0, 1, 1}));
  f.graph.updateWorldTransforms();
  const auto before = nodes(f.graph); const auto revision = f.graph.revision();
  const auto plan = f.plan(); gltf::GltfInstancingData instances;
  for (const auto sourceNode : {plan.imported.nodes[0], plan.placement.nodes[0]}) {
    gltf::NodeInstancing item{uint32_t(sourceNode), 1, 1};
    item.localMatrices = {glm::mat4(1)}; instances.nodes.push_back(std::move(item));
  }
  EXPECT_THROW((void)BuildGltfMergeGraphCandidate(f.graph, plan, 1, f.materials, f.primitives,
      instances, f.scale, f.limits), std::invalid_argument);
  EXPECT_EQ(nodes(f.graph), before); EXPECT_EQ(f.graph.revision(), revision);
  // Ordinary non-instanced singular editor transforms remain valid engine edits.
  EXPECT_NO_THROW((void)f.build(plan));
}

TEST(GltfMergeGraph, LargeFlatWideAndDeepSourceForestsUseBoundedBulkAssembly) {
  for (const int shape : {0, 1, 2}) {
    SCOPED_TRACE(shape);
    const bool deep = shape == 2, flat = shape == 0;
    const uint32_t count = deep ? 10001u : 20001u;
    tinygltf::Model incoming; incoming.nodes.resize(count);
    for (uint32_t id = 0; id < count; ++id) {
      incoming.nodes[id].translation = {1, 0, 0};
      if (id && !flat) incoming.nodes[deep ? id - 1 : 0].children.push_back(int(id));
    }
    tinygltf::Scene scene;
    if (flat) for (uint32_t id = 0; id < count; ++id) scene.nodes.push_back(int(id));
    else scene.nodes = {0};
    incoming.scenes.push_back(scene); incoming.defaultScene = 0;
    const GltfMeshAttachmentLimits hierarchy{1, 100000, 0, 1};
    const auto generated = BuildGltfSceneHierarchy(incoming, {}, {}, glm::mat4(1), hierarchy,
        GltfHierarchyScope::CompleteAsset, {});
    ASSERT_EQ(generated.nodeCount(), count);
    const auto expectedX = deep ? float(count) : flat ? 1.0f : 2.0f;
    EXPECT_FLOAT_EQ(generated.getNode(count - 1)->worldTransform[3][0], expectedX);
    const tinygltf::Model empty; const auto plan = gltf::PlanGltfMerge(empty, incoming);
    SceneGraph base;
    auto result = BuildGltfMergeGraphCandidate(base, plan, 0, {}, {}, {}, glm::mat4(1), {hierarchy, 1});
    EXPECT_EQ(result.graph.nodeCount(), size_t(count) * 2 + 3);
    const auto leaf = result.sourceNodeToGraph[size_t(plan.placement.nodes.back())];
    EXPECT_FLOAT_EQ(result.graph.getNode(leaf)->worldTransform[3][0], expectedX);
    EXPECT_TRUE(result.graph.isNodeActive(leaf));
    EXPECT_FALSE(result.graph.isNodeActive(result.sourceNodeToGraph[size_t(plan.imported.nodes.back())]));
    auto proof = base.prepareAppend(std::move(result.graph)); EXPECT_TRUE(base.publishAppend(proof));
  }
}

TEST(GltfMergeGraph, DeepInactiveSharedMeshChainUsesBuilderComposedOwnerWorlds) {
  constexpr uint32_t count = 10001;
  auto incoming = source("Inactive shared mesh"); incoming.nodes.resize(count);
  for (uint32_t id = 0; id < count; ++id) {
    incoming.nodes[id].mesh = 0; incoming.nodes[id].translation = {1, 0, 0};
    if (id) incoming.nodes[id - 1].children.push_back(int(id));
  }
  incoming.scenes.resize(2); incoming.scenes[0].nodes.clear(); incoming.scenes[1].nodes = {0};
  incoming.defaultScene = 0;
  const tinygltf::Model empty; const auto plan = gltf::PlanGltfMerge(empty, incoming);
  const std::array<uint32_t, 1> materials{0}, primitives{0};
  SceneGraph base;
  auto result = BuildGltfMergeGraphCandidate(base, plan, 0, materials, primitives, {}, glm::mat4(1),
      {{count, uint64_t(count) * 2 + 8, 1, 1}, 1});
  EXPECT_EQ(result.graph.nodeCount(), size_t(count) * 2 + 3);
  EXPECT_TRUE(result.graph.renderableNodes().empty());
  const auto leaf = result.sourceNodeToGraph[size_t(plan.imported.nodes.back())];
  const auto primitive = result.graph.getNode(leaf)->children.front();
  EXPECT_FLOAT_EQ(result.graph.getNode(leaf)->worldTransform[3][0], float(count));
  EXPECT_FLOAT_EQ(result.graph.getNode(primitive)->worldTransform[3][0], float(count));
  EXPECT_EQ(result.graph.getNode(primitive)->meshOwnerGraphNode, leaf);
  EXPECT_EQ(result.graph.getNode(primitive)->primitiveIndex, 0u);
  EXPECT_FALSE(result.graph.isNodeActive(leaf)); EXPECT_FALSE(result.graph.isNodeActive(primitive));
  EXPECT_EQ(plan.placement.nodes.back(), -1);
  auto proof = base.prepareAppend(std::move(result.graph)); ASSERT_TRUE(base.publishAppend(proof));
  EXPECT_TRUE(base.renderableNodes().empty());
}

TEST(GltfMergeGraph, StandaloneAttachmentStillValidatesUncachedAncestorWorlds) {
  SceneGraph graph;
  const auto parent = graph.createNode(glm::scale(glm::mat4(1), {1e30f, 1, 1}), 0);
  const auto owner = graph.createNode(glm::scale(glm::mat4(1), {1e10f, 1, 1}), 0);
  graph.getNode(owner)->sourceNodeIndex = 7;
  ASSERT_TRUE(graph.setParent(owner, parent));
  // Cached owner world remains finite and stale; local composition overflows.
  const auto before = nodes(graph);
  gltf::NodeInstancing instances{7, 3, 1}; instances.localMatrices = {glm::mat4(1)};
  const std::array<uint32_t, 1> materials{0};
  EXPECT_THROW((void)AttachGltfMeshPrimitives(graph, {owner, 7, 3, 0, materials, &instances}), std::invalid_argument);
  EXPECT_EQ(nodes(graph), before);
}
