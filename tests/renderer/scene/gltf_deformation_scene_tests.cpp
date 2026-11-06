#include <Container/renderer/scene/GltfDeformationScene.h>

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::renderer;
using container::geometry::Mesh;
using container::geometry::Model;
using container::geometry::Vertex;
using container::scene::SceneGraph;
namespace gltf = container::geometry::gltf;
constexpr uint32_t invalid = SceneGraph::kInvalidNode;

std::vector<Vertex> triangle() {
  std::vector<Vertex> vertices(3);
  vertices[0].position = glm::vec3(0, 0, 0);
  vertices[1].position = glm::vec3(1, 0, 0);
  vertices[2].position = glm::vec3(0, 1, 0);
  for (auto& vertex : vertices) {
    vertex.normal = glm::vec3(0, 0, 1);
    vertex.tangent = glm::vec4(1, 0, 0, -1);
  }
  return vertices;
}

void expectPosition(glm::vec3 actual, glm::vec3 expected) {
  EXPECT_NEAR(actual.x, expected.x, 1e-5f);
  EXPECT_NEAR(actual.y, expected.y, 1e-5f);
  EXPECT_NEAR(actual.z, expected.z, 1e-5f);
}

struct Fixture {
  Model model = Model::FromMeshes({Mesh(triangle(), {0, 1, 2})});
  gltf::DeformationData data;
  tinygltf::Model source;
  SceneGraph graph;
  std::vector<uint32_t> owners;
  uint32_t owner{0}, primitive{0};
  GltfDeformationScene scene;

  Fixture(bool morph = true, bool skin = false) {
    gltf::DeformationPrimitive imported;
    imported.bindVertices = triangle();
    if (morph) {
      imported.morphTargets = {{{glm::vec3(2, 0, 0), glm::vec3(2, 0, 0), glm::vec3(2, 0, 0)}, {}, {}}};
      imported.meshDefaultWeights = {0.5f};
    }
    if (skin) imported.influences = {{{{0, 1}}}, {{{0, 1}}}, {{{0, 1}}}};
    data.primitives.push_back(imported);
    data.nodes.push_back({0, skin ? 0 : -1, morph ? std::vector<float>{0.25f} : std::vector<float>{}});
    source.nodes.resize(1);
    source.nodes[0].mesh = 0;
    source.nodes[0].skin = skin ? 0 : -1;
    if (morph) source.nodes[0].weights = {0.25};
    source.meshes.resize(1);
    if (morph) source.meshes[0].weights = {0.5};
    owner = graph.createNode(glm::mat4(1), 0);
    graph.getNode(owner)->sourceNodeIndex = 0;
    primitive = graph.createNode(glm::mat4(1), 0, true, 0);
    graph.setParent(primitive, owner);
    owners.assign(graph.nodeCount(), invalid);
    owners[primitive] = owner;
    if (skin) {
      data.nodes.push_back({-1, -1, {}});
      source.nodes.resize(2);
      const uint32_t joint = graph.createNode(glm::mat4(1), 0);
      graph.getNode(joint)->sourceNodeIndex = 1;
      data.skins.push_back({{1}, {glm::mat4(1)}, -1});
      owners.resize(graph.nodeCount(), invalid);
    }
    graph.updateWorldTransforms();
  }

  void configure() { scene.configure(data, model); }
  GltfDeformationSceneUpdate update(bool reset = false) {
    graph.updateWorldTransforms();
    return scene.update(graph, owners, &source, reset);
  }
  uint32_t offset() const { return scene.drawRange(primitive)->vertexOffset; }
};

TEST(GltfDeformationScene, EmptyAndProceduralScenesNeedNoDynamicStreams) {
  GltfDeformationScene scene;
  SceneGraph graph;
  scene.configure({}, Model{});
  auto update = scene.update(graph, {});
  EXPECT_FALSE(update.topologyChanged);
  EXPECT_FALSE(update.geometryChanged);
  EXPECT_TRUE(scene.currentVertices().empty());
  EXPECT_TRUE(scene.gpuInstances().empty());
  const Model model = Model::FromMeshes({Mesh(triangle(), {0, 1, 2})});
  scene.configure({}, model);
  const uint32_t node = graph.createNode(glm::mat4(1), 0, true, 0);
  graph.updateWorldTransforms();
  update = scene.update(graph, {});
  EXPECT_TRUE(update.geometryChanged);
  ASSERT_NE(scene.drawRange(node), nullptr);
  EXPECT_FALSE(scene.drawRange(node)->deformed());
  EXPECT_EQ(scene.drawRange(node)->firstIndex, 0u);
  EXPECT_EQ(scene.currentVertices().size(), 3u);
  EXPECT_EQ(scene.indices().size(), 3u);
  EXPECT_TRUE(scene.gpuInstances().empty());
  EXPECT_EQ(scene.currentVertices().data(), scene.bindVertices().data());
  EXPECT_EQ(scene.previousVertices().data(), scene.bindVertices().data());
}

TEST(GltfDeformationScene, MorphDefaultsAppendIndependentGlobalRangeAndBounds) {
  Fixture fixture;
  fixture.configure();
  const auto update = fixture.update();
  EXPECT_TRUE(update.topologyChanged);
  EXPECT_TRUE(update.geometryChanged);
  EXPECT_TRUE(update.historyReset);
  ASSERT_EQ(fixture.scene.currentVertices().size(), 6u);
  EXPECT_EQ(std::vector<uint32_t>(fixture.scene.indices().begin(), fixture.scene.indices().end()),
            (std::vector<uint32_t>{0, 1, 2, 3, 4, 5}));
  const auto* range = fixture.scene.drawRange(fixture.primitive);
  ASSERT_NE(range, nullptr);
  EXPECT_EQ(range->firstIndex, 3u);
  EXPECT_EQ(range->indexCount, 3u);
  EXPECT_EQ(range->vertexOffset, 3u);
  EXPECT_EQ(range->meshOwnerGraphNode, fixture.owner);
  EXPECT_TRUE(range->deformed());
  expectPosition(fixture.scene.currentVertices()[0].position, glm::vec3(0));
  expectPosition(fixture.scene.currentVertices()[3].position, glm::vec3(0.5f, 0, 0));
  expectPosition(fixture.scene.bindVertices()[3].position, glm::vec3(0));
  expectPosition(range->bounds.min, glm::vec3(0.5f, 0, 0));
  expectPosition(range->bounds.max, glm::vec3(1.5f, 1, 0));
  expectPosition(range->bounds.center, glm::vec3(1, 0.5f, 0));
  EXPECT_NEAR(range->bounds.radius, std::sqrt(0.5f), 1e-6f);
  EXPECT_EQ(range->orientation, GltfDeformationOrientation::Preserved);
  ASSERT_EQ(fixture.scene.gpuInstances().size(), 1u);
  EXPECT_EQ(fixture.scene.gpuInstances()[0].outputVertexOffset, 3u);
  EXPECT_FLOAT_EQ(fixture.scene.gpuPoses()[0].morphWeights[0], 0.25f);
}

TEST(GltfDeformationScene, NullEvaluatedSourceUsesResolvedImportedNodeDefaults) {
  Fixture fixture;
  fixture.configure();
  fixture.scene.update(fixture.graph, fixture.owners);
  expectPosition(fixture.scene.currentVertices()[fixture.offset()].position, glm::vec3(0.5f, 0, 0));
  fixture.source.nodes[0].weights.clear();
  fixture.source.meshes[0].weights = {-0.5};
  fixture.update();
  expectPosition(fixture.scene.currentVertices()[fixture.offset()].position, glm::vec3(-1, 0, 0));
}

TEST(GltfDeformationScene, PreviousPoseAdvancesOnlyOnSuccessfulSubmissionCommit) {
  Fixture fixture;
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  fixture.source.nodes[0].weights = {1};
  fixture.update();
  const uint32_t offset = fixture.offset();
  expectPosition(fixture.scene.currentVertices()[offset].position, glm::vec3(2, 0, 0));
  expectPosition(fixture.scene.previousVertices()[offset].position, glm::vec3(0.5f, 0, 0));
  EXPECT_FLOAT_EQ(fixture.scene.gpuPoses()[0].previousMorphWeights[0], 0.25f);
  fixture.source.nodes[0].weights = {2};
  fixture.update();  // The preceding prepared frame was abandoned.
  expectPosition(fixture.scene.currentVertices()[offset].position, glm::vec3(4, 0, 0));
  expectPosition(fixture.scene.previousVertices()[offset].position, glm::vec3(0.5f, 0, 0));
  fixture.scene.commitSubmitted();
  fixture.source.nodes[0].weights = {3};
  fixture.update();
  expectPosition(fixture.scene.previousVertices()[offset].position, glm::vec3(4, 0, 0));
}

TEST(GltfDeformationScene, ResetPersistsAcrossAbandonedFramesWithoutChangingGeometryRevision) {
  Fixture fixture;
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  const uint64_t revision = fixture.scene.geometryRevision();
  auto update = fixture.update(true);
  EXPECT_FALSE(update.geometryChanged);
  EXPECT_EQ(fixture.scene.geometryRevision(), revision);
  EXPECT_TRUE(update.historyReset);
  fixture.source.nodes[0].weights = {2};
  update = fixture.update();
  EXPECT_TRUE(update.historyReset);
  expectPosition(fixture.scene.previousVertices()[fixture.offset()].position, glm::vec3(4, 0, 0));
  EXPECT_TRUE(fixture.scene.gpuPoses()[0].resetHistory);
  fixture.scene.commitSubmitted();
  EXPECT_FALSE(fixture.update().historyReset);
}

TEST(GltfDeformationScene, SourceWeightsChangeGeometryRevisionButObjectTransformsDoNot) {
  Fixture fixture;
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  const uint64_t geometry = fixture.scene.geometryRevision();
  const uint64_t topology = fixture.scene.topologyRevision();
  EXPECT_FALSE(fixture.update().geometryChanged);
  fixture.graph.setLocalTransform(fixture.owner,
      glm::translate(glm::mat4(1), glm::vec3(10, 0, 0)));
  EXPECT_FALSE(fixture.update().geometryChanged);
  EXPECT_EQ(fixture.scene.geometryRevision(), geometry);
  EXPECT_EQ(fixture.scene.topologyRevision(), topology);
  fixture.source.nodes[0].weights = {0.75};
  EXPECT_TRUE(fixture.update().geometryChanged);
  EXPECT_EQ(fixture.scene.geometryRevision(), geometry + 1);
  EXPECT_EQ(fixture.scene.topologyRevision(), topology);
}

TEST(GltfDeformationScene, ReparentKeepsExplicitMeshOwnerAndSubmittedHistory) {
  Fixture fixture(false, true);
  fixture.graph.setLocalTransform(fixture.owner,
      glm::translate(glm::mat4(1), glm::vec3(10, 0, 0)));
  fixture.graph.setLocalTransform(2,
      glm::translate(glm::mat4(1), glm::vec3(1, 2, 0)));
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  const uint64_t geometry = fixture.scene.geometryRevision();
  const uint64_t topology = fixture.scene.topologyRevision();
  const uint32_t editor = fixture.graph.createNode(
      glm::translate(glm::mat4(1), glm::vec3(20, 0, 0)), 0);
  fixture.graph.setParent(fixture.primitive, editor);
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  const auto update = fixture.update();
  EXPECT_FALSE(update.topologyChanged);
  EXPECT_FALSE(update.geometryChanged);
  EXPECT_FALSE(update.historyReset);
  EXPECT_EQ(fixture.scene.geometryRevision(), geometry);
  EXPECT_EQ(fixture.scene.topologyRevision(), topology);
  const auto local = fixture.scene.currentVertices()[fixture.offset()].position;
  expectPosition(local, glm::vec3(-9, 2, 0));
  const glm::vec3 world(fixture.graph.getNode(fixture.primitive)->worldTransform * glm::vec4(local, 1));
  expectPosition(world, glm::vec3(11, 2, 0));
}

TEST(GltfDeformationScene, CompatibleGraphIdentityReplacementDoesNotResetHistory) {
  Fixture fixture;
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  SceneGraph replacement = fixture.graph;
  replacement.updateWorldTransforms();
  const uint64_t revision = fixture.scene.geometryRevision();
  const auto update = fixture.scene.update(replacement, fixture.owners, &fixture.source);
  EXPECT_FALSE(update.topologyChanged);
  EXPECT_FALSE(update.geometryChanged);
  EXPECT_FALSE(update.historyReset);
  EXPECT_EQ(fixture.scene.geometryRevision(), revision);
}

TEST(GltfDeformationScene, SameMeshUsesIndependentSkinsAndWeightsAndRetainsStaticInstance) {
  Fixture fixture(true, true);
  fixture.data.nodes.push_back({0, 1, {1}});
  fixture.data.nodes.push_back({-1, -1, {}});
  fixture.data.nodes.push_back({0, -1, {0}});
  fixture.data.skins.push_back({{3}, {glm::mat4(1)}, -1});
  fixture.source.nodes.resize(5);
  fixture.source.nodes[2].mesh = 0;
  fixture.source.nodes[2].skin = 1;
  fixture.source.nodes[2].weights = {1};
  fixture.source.nodes[4].mesh = 0;
  fixture.source.nodes[4].weights = {0};
  fixture.graph.setLocalTransform(fixture.owner,
      glm::translate(glm::mat4(1), glm::vec3(10, 0, 0)));
  fixture.graph.setLocalTransform(2,
      glm::translate(glm::mat4(1), glm::vec3(1, 2, 0)));
  const uint32_t secondOwner = fixture.graph.createNode(
      glm::translate(glm::mat4(1), glm::vec3(100, 0, 0)), 0);
  fixture.graph.getNode(secondOwner)->sourceNodeIndex = 2;
  const uint32_t second = fixture.graph.createNode(glm::mat4(1), 0, true, 0);
  fixture.graph.setParent(second, secondOwner);
  const uint32_t joint = fixture.graph.createNode(
      glm::translate(glm::mat4(1), glm::vec3(3, -2, 0)), 0);
  fixture.graph.getNode(joint)->sourceNodeIndex = 3;
  const uint32_t thirdOwner = fixture.graph.createNode(glm::mat4(1), 0);
  fixture.graph.getNode(thirdOwner)->sourceNodeIndex = 4;
  const uint32_t third = fixture.graph.createNode(glm::mat4(1), 0, true, 0);
  fixture.graph.setParent(third, thirdOwner);
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  fixture.owners[second] = secondOwner;
  fixture.owners[third] = thirdOwner;
  fixture.configure();
  fixture.update();
  ASSERT_EQ(fixture.scene.gpuInstances().size(), 3u);
  const auto* a = fixture.scene.drawRange(fixture.primitive);
  const auto* b = fixture.scene.drawRange(second);
  const auto* c = fixture.scene.drawRange(third);
  expectPosition(fixture.scene.currentVertices()[a->vertexOffset].position, glm::vec3(-8.5f, 2, 0));
  expectPosition(fixture.scene.currentVertices()[b->vertexOffset].position, glm::vec3(-95, -2, 0));
  expectPosition(fixture.scene.currentVertices()[c->vertexOffset].position, glm::vec3(0));
  EXPECT_NE(a->firstIndex, b->firstIndex);
  EXPECT_NE(b->firstIndex, c->firstIndex);
  EXPECT_TRUE(fixture.scene.gpuPoses()[c->instance].skinPalette.empty());
}

TEST(GltfDeformationScene, JointAttributesWithoutNodeSkinRemainStatic) {
  Fixture fixture(false, true);
  fixture.data.nodes[0].skin = -1;
  fixture.source.nodes[0].skin = -1;
  fixture.configure();
  fixture.update();
  EXPECT_TRUE(fixture.scene.gpuInstances().empty());
  EXPECT_EQ(fixture.scene.currentVertices().size(), 3u);
  EXPECT_EQ(fixture.scene.indices().size(), 3u);
  EXPECT_FALSE(fixture.scene.drawRange(fixture.primitive)->deformed());
}

TEST(GltfDeformationScene, PaletteMotionTracksNestedJointWorldAndPreviousPose) {
  Fixture fixture(false, true);
  const uint32_t parent = fixture.graph.createNode(
      glm::translate(glm::mat4(1), glm::vec3(3, 0, 0)), 0);
  fixture.graph.setParent(2, parent);
  fixture.graph.setLocalTransform(2, glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)));
  fixture.data.skins[0].inverseBindMatrices[0] =
      glm::translate(glm::mat4(1), glm::vec3(-5, 0, 0));
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  fixture.graph.setLocalTransform(parent,
      glm::translate(glm::mat4(1), glm::vec3(3, 0, 0)) *
      glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1)));
  const auto update = fixture.update();
  EXPECT_TRUE(update.geometryChanged);
  expectPosition(fixture.scene.currentVertices()[fixture.offset()].position, glm::vec3(3, -3, 0));
  expectPosition(fixture.scene.previousVertices()[fixture.offset()].position, glm::vec3(0));
}

TEST(GltfDeformationScene, UniformMirroredSkinReportsReversedOrientationAndBounds) {
  Fixture fixture(false, true);
  fixture.graph.setLocalTransform(2, glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4)));
  fixture.configure();
  fixture.update();
  const auto* range = fixture.scene.drawRange(fixture.primitive);
  EXPECT_EQ(range->orientation, GltfDeformationOrientation::Reversed);
  expectPosition(range->bounds.min, glm::vec3(-2, 0, 0));
  expectPosition(range->bounds.max, glm::vec3(0, 3, 0));
  EXPECT_FLOAT_EQ(fixture.scene.currentVertices()[range->vertexOffset].tangent.w, 1);
}

TEST(GltfDeformationScene, TinyValidMirroredSkinRetainsOrientationClassification) {
  Fixture fixture(false, true);
  fixture.graph.setLocalTransform(2, glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4) * 1e-12f));
  fixture.configure();
  fixture.update();
  EXPECT_EQ(fixture.scene.drawRange(fixture.primitive)->orientation,
      GltfDeformationOrientation::Reversed);
}

TEST(GltfDeformationScene, AddingStaticDrawMappingDoesNotChangeDeformedGeometryOrHistory) {
  Fixture fixture;
  fixture.model = Model::FromMeshes({Mesh(triangle(), {0, 1, 2}), Mesh(triangle(), {0, 1, 2})});
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  const uint64_t geometry = fixture.scene.geometryRevision();
  const uint32_t node = fixture.graph.createNode(glm::mat4(1), 0, true, 1);
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  const auto update = fixture.update();
  EXPECT_TRUE(update.topologyChanged);
  EXPECT_FALSE(update.geometryChanged);
  EXPECT_FALSE(update.historyReset);
  EXPECT_EQ(fixture.scene.geometryRevision(), geometry);
  EXPECT_FALSE(fixture.scene.drawRange(node)->deformed());
  EXPECT_EQ(fixture.scene.drawRange(node)->firstIndex, 3u);
}

TEST(GltfDeformationScene, MixedSkinAndCollapsedTrianglesRequireConservativeCulling) {
  Fixture fixture(false, true);
  fixture.data.nodes.push_back({-1, -1, {}});
  fixture.source.nodes.resize(3);
  fixture.data.skins[0].joints.push_back(2);
  fixture.data.skins[0].inverseBindMatrices.push_back(glm::mat4(1));
  const uint32_t mirrored = fixture.graph.createNode(glm::scale(glm::mat4(1), glm::vec3(-1, 1, 1)), 0);
  fixture.graph.getNode(mirrored)->sourceNodeIndex = 2;
  fixture.data.primitives[0].influences[1].values[0].joint = 1;
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  fixture.configure();
  fixture.update();
  EXPECT_EQ(fixture.scene.drawRange(fixture.primitive)->orientation,
      GltfDeformationOrientation::MixedOrDegenerate);
  fixture.graph.setLocalTransform(2, glm::scale(glm::mat4(1), glm::vec3(0, 1, 1)));
  fixture.update();
  EXPECT_EQ(fixture.scene.drawRange(fixture.primitive)->orientation,
      GltfDeformationOrientation::MixedOrDegenerate);
}

TEST(GltfDeformationScene, TopologyCompactionPreservesSurvivingSubmittedPose) {
  Fixture fixture;
  const uint32_t second = fixture.graph.createNode(glm::mat4(1), 0, true, 0);
  fixture.graph.setParent(second, fixture.owner);
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  fixture.owners[second] = fixture.owner;
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  fixture.source.nodes[0].weights = {1};
  fixture.graph.setRenderable(fixture.primitive, false);
  const auto update = fixture.update();
  EXPECT_TRUE(update.topologyChanged);
  EXPECT_TRUE(update.geometryChanged);
  EXPECT_FALSE(update.historyReset);
  EXPECT_EQ(fixture.scene.gpuInstances().size(), 1u);
  const auto* range = fixture.scene.drawRange(second);
  ASSERT_NE(range, nullptr);
  EXPECT_EQ(range->vertexOffset, 3u);
  expectPosition(fixture.scene.currentVertices()[3].position, glm::vec3(2, 0, 0));
  expectPosition(fixture.scene.previousVertices()[3].position, glm::vec3(0.5f, 0, 0));
}

TEST(GltfDeformationScene, RejectedExpansionKeepsSubmittedPoseAndRanges) {
  Fixture fixture;
  fixture.data.nodes.push_back({0, -1, {0.25f}});
  fixture.source.nodes.push_back(fixture.source.nodes.front());
  fixture.scene.configure(fixture.data, fixture.model, {}, 6);
  fixture.update();
  fixture.scene.commitSubmitted();
  const auto revision = fixture.scene.geometryRevision();
  const auto offset = fixture.offset();
  const auto before = fixture.scene.currentVertices()[offset].position;

  const auto owner = fixture.graph.createNode(glm::mat4(1), 0);
  fixture.graph.getNode(owner)->sourceNodeIndex = 1;
  const auto draw = fixture.graph.createNode(glm::mat4(1), 0, true, 0);
  fixture.graph.setParent(draw, owner);
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  fixture.owners[draw] = owner;
  EXPECT_THROW(fixture.update(), std::runtime_error);
  EXPECT_EQ(fixture.scene.geometryRevision(), revision);
  EXPECT_EQ(fixture.scene.gpuInstances().size(), 1u);
  EXPECT_EQ(fixture.scene.drawRange(draw), nullptr);
  EXPECT_EQ(fixture.scene.currentVertices()[offset].position, before);
  EXPECT_EQ(fixture.scene.previousVertices()[offset].position, before);
}

TEST(GltfDeformationScene, MissingAndAmbiguousJointNodesAreExplicitErrors) {
  Fixture fixture(false, true);
  fixture.configure();
  fixture.update();
  fixture.scene.commitSubmitted();
  fixture.graph.getNode(2)->sourceNodeIndex = -1;
  EXPECT_THROW(fixture.update(), std::runtime_error);
  fixture.graph.getNode(2)->sourceNodeIndex = 1;
  const uint32_t duplicate = fixture.graph.createNode(glm::mat4(1), 0);
  fixture.graph.getNode(duplicate)->sourceNodeIndex = 1;
  fixture.owners.resize(fixture.graph.nodeCount(), invalid);
  EXPECT_THROW(fixture.update(), std::runtime_error);
  fixture.graph.getNode(duplicate)->sourceNodeIndex = -1;
  EXPECT_FALSE(fixture.update().historyReset);
  expectPosition(fixture.scene.previousVertices()[fixture.offset()].position, glm::vec3(0));
}

TEST(GltfDeformationScene, MissingOwnerAndInvalidMorphWeightsAreExplicitErrors) {
  Fixture fixture;
  fixture.configure();
  const uint32_t owner = fixture.owners[fixture.primitive];
  fixture.owners[fixture.primitive] = invalid;
  EXPECT_THROW(fixture.update(), std::runtime_error);
  fixture.owners[fixture.primitive] = owner;
  fixture.source.nodes[0].weights = {std::numeric_limits<double>::quiet_NaN()};
  EXPECT_THROW(fixture.update(), std::runtime_error);
  fixture.source.nodes[0].weights = {0, 1};
  EXPECT_THROW(fixture.update(), std::runtime_error);
}

void missingFrameQuad(Fixture& fixture, bool normal, bool tangent) {
  auto vertices = triangle();
  vertices.push_back(vertices[0]);
  vertices[2].position = glm::vec3(1, 1, 0);
  vertices[3].position = glm::vec3(0, 1, 0);
  for (auto& vertex : vertices) {
    vertex.texCoord = glm::vec2(vertex.position);
    vertex.texCoord1 = glm::vec2(vertex.position.y, vertex.position.x);
  }
  fixture.model = Model::FromMeshes({Mesh(vertices, {0, 1, 2, 0, 2, 3})});
  auto& primitive = fixture.data.primitives[0];
  primitive.bindVertices = vertices;
  primitive.morphTargets = {{{glm::vec3(0), glm::vec3(0), glm::vec3(0, 0, 1), glm::vec3(0)}, {}, {}}};
  primitive.meshDefaultWeights = {1};
  primitive.authoredBaseNormal = normal;
  primitive.authoredBaseTangent = tangent;
  fixture.source.nodes[0].weights = {1};
}

TEST(GltfDeformationScene, MissingFramesExpandCornersAndRetainSubmittedNormals) {
  Fixture fixture;
  missingFrameQuad(fixture, false, false);
  fixture.configure();
  fixture.update();
  const uint32_t offset = fixture.offset();
  EXPECT_EQ(offset, 4u);
  EXPECT_EQ(fixture.scene.drawRange(fixture.primitive)->vertexCount, 6u);
  EXPECT_EQ(fixture.scene.currentVertices().size(), 10u);
  EXPECT_EQ(fixture.data.primitives[0].bindVertices.size(), 4u);
  EXPECT_EQ(fixture.scene.gpuInstances()[0].primitive->bindVertices.size(), 6u);
  EXPECT_EQ(std::vector<uint32_t>(fixture.scene.indices().begin(), fixture.scene.indices().end()),
            (std::vector<uint32_t>{0, 1, 2, 0, 2, 3, 4, 5, 6, 7, 8, 9}));
  const float rootHalf = std::sqrt(0.5f);
  expectPosition(fixture.scene.currentVertices()[offset].normal, {0, -rootHalf, rootHalf});
  expectPosition(fixture.scene.currentVertices()[offset + 3].normal, {-rootHalf, 0, rootHalf});
  ASSERT_EQ(fixture.scene.gpuPoses()[0].evaluatedVertices.size(), 6u);
  fixture.scene.commitSubmitted();
  fixture.source.nodes[0].weights = {0.5};
  fixture.update();
  const auto& pose = fixture.scene.gpuPoses()[0];
  expectPosition(pose.evaluatedVertices[0].normal, glm::normalize(glm::vec3(0, -.5f, 1)));
  expectPosition(pose.previousEvaluatedVertices[0].normal, {0, -rootHalf, rootHalf});
  expectPosition(fixture.scene.previousVertices()[offset].normal, {0, -rootHalf, rootHalf});
  fixture.update(true);
  expectPosition(fixture.scene.gpuPoses()[0].previousEvaluatedVertices[0].normal,
                   fixture.scene.gpuPoses()[0].evaluatedVertices[0].normal);
}

TEST(GltfDeformationScene, MissingTangentsUseNormalTextureUvSetWithoutReplacingAuthoredNormal) {
  Fixture fixture;
  missingFrameQuad(fixture, true, false);
  fixture.data.primitives[0].normalTextureTexCoord = 1;
  fixture.configure();
  fixture.update();
  const auto& vertex = fixture.scene.currentVertices()[fixture.offset()];
  expectPosition(vertex.normal, {0, 0, 1});
  expectPosition(glm::vec3(vertex.tangent), {0, 1, 0});
  EXPECT_FLOAT_EQ(vertex.tangent.w, -1);
  EXPECT_EQ(fixture.scene.gpuPoses()[0].evaluatedVertices.size(), 6u);
}

TEST(GltfDeformationScene, AuthoredFramesKeepGpuEvaluationAndOriginalVertexCount) {
  Fixture fixture;
  missingFrameQuad(fixture, true, true);
  fixture.configure();
  fixture.update();
  EXPECT_EQ(fixture.scene.drawRange(fixture.primitive)->vertexCount, 4u);
  EXPECT_TRUE(fixture.scene.gpuPoses()[0].evaluatedVertices.empty());
  EXPECT_TRUE(fixture.scene.gpuPoses()[0].previousEvaluatedVertices.empty());
}

}  // namespace

TEST(GltfDeformationScene, InstanceChildrenShareOwnerPoseAndKeepHistoryWhenFirstDrawDisappears) {
  for(bool skin:{false,true}) {
    Fixture f(true,skin);
    std::vector<uint32_t> nodes{f.primitive};
    for(int i=1;i<4;++i) {
      const auto child=f.graph.createNode(glm::translate(glm::mat4(1),{float(i)*3,0,0}),0,true,0);
      f.graph.setParent(child,f.owner); nodes.push_back(child);
    }
    f.owners.resize(f.graph.nodeCount(),invalid);
    for(auto node:nodes) f.owners[node]=f.owner;
    f.configure(); f.update(); f.scene.commitSubmitted();
    f.source.nodes[0].weights={1}; f.update();
    ASSERT_EQ(f.scene.gpuInstances().size(),1u);
    EXPECT_EQ(f.scene.currentVertices().size(),6u); EXPECT_EQ(f.scene.indices().size(),6u);
    const auto offset=f.scene.drawRange(nodes[0])->vertexOffset;
    const auto firstIndex=f.scene.drawRange(nodes[0])->firstIndex;
    for(auto node:nodes) {
      EXPECT_EQ(f.scene.drawRange(node)->vertexOffset,offset);
      EXPECT_EQ(f.scene.drawRange(node)->firstIndex,firstIndex);
      EXPECT_EQ(f.scene.drawRange(node)->instance,0u);
    }
    expectPosition(f.scene.previousVertices()[offset].position,{.5f,0,0});
    expectPosition(f.scene.currentVertices()[offset].position,{2,0,0});
    f.graph.setRenderable(nodes[0],false);
    const auto changed=f.update();
    EXPECT_TRUE(changed.topologyChanged); EXPECT_FALSE(changed.geometryChanged);
    EXPECT_FALSE(changed.historyReset); ASSERT_EQ(f.scene.gpuInstances().size(),1u);
    expectPosition(f.scene.previousVertices()[offset].position,{.5f,0,0});
    EXPECT_EQ(f.scene.drawRange(nodes[1])->vertexOffset,offset);
  }
}

TEST(GltfDeformationScene, ExplicitDefinitionsSkipProceduralGeometryAndRetainSkinMorphIndexStreams) {
  for (const bool regenerateFrames : {false, true}) {
    SCOPED_TRACE(regenerateFrames);
    Fixture f(true, true);
    auto second = f.data.primitives[0]; second.sourceMesh = 1;
    second.authoredBaseNormal = second.authoredBaseTangent = !regenerateFrames;
    for (auto& vertex : second.bindVertices) vertex.position.x += 5;
    second.morphTargets[0].positions.assign(3, glm::vec3(0, 0, 4));
    f.data.primitives.push_back(second);
    auto procedural = triangle(); for (auto& vertex : procedural) vertex.position.x += 100;
    f.model = Model::FromMeshes({Mesh(triangle(), {0, 1, 2}), Mesh(procedural, {0, 1, 2}),
                               Mesh(second.bindVertices, {2, 1, 0})});
    f.data.nodes.push_back({1, 0, {.5f}});
    tinygltf::Node source; source.mesh = 1; source.skin = 0; source.weights = {.5};
    f.source.nodes.push_back(source); f.source.meshes.resize(2); f.source.meshes[1].weights = {.5};
    const auto plain = f.graph.createNode(glm::mat4(1), 0, true, 1);
    const auto owner = f.graph.createNode(glm::translate(glm::mat4(1), glm::vec3(10, 0, 0)), 0);
    f.graph.getNode(owner)->sourceNodeIndex = 2;
    const auto child = f.graph.createNode(glm::mat4(1), 0, true, 2);
    f.graph.setParent(child, owner);
    f.owners.resize(f.graph.nodeCount(), invalid); f.owners[child] = owner;
    f.graph.setLocalTransform(2, glm::translate(glm::mat4(1), glm::vec3(0, 3, 0)));
    const std::array<uint32_t, 2> mapping{0, 2};
    f.scene.configure(f.data, f.model, mapping); f.update();
    ASSERT_EQ(f.scene.gpuInstances().size(), 2u);
    ASSERT_NE(f.scene.drawRange(plain), nullptr); EXPECT_FALSE(f.scene.drawRange(plain)->deformed());
    EXPECT_EQ(f.scene.drawRange(plain)->sourcePrimitive, 1u);
    const auto* range = f.scene.drawRange(child); ASSERT_NE(range, nullptr);
    EXPECT_EQ(range->sourcePrimitive, 2u); EXPECT_TRUE(range->deformed());
    EXPECT_EQ(f.scene.gpuInstances()[range->instance].primitive->sourceMesh, 1u);
    expectPosition(f.scene.currentVertices()[range->vertexOffset].position,
                   {-5, regenerateFrames ? 4.0f : 3.0f, 2});
    EXPECT_EQ(f.scene.indices()[range->firstIndex], range->vertexOffset + (regenerateFrames ? 0 : 2));
    expectPosition(f.scene.currentVertices()[3].position, {100, 0, 0});
    expectPosition(f.scene.currentVertices()[f.offset()].position, {.5f, 3, 0});
  }
}

TEST(GltfDeformationScene, InvalidDefinitionMapsCannotRetireCurrentSubmittedPose) {
  Fixture f; f.configure(); f.update(); f.scene.commitSubmitted();
  const auto revision = f.scene.geometryRevision(), topology = f.scene.topologyRevision();
  const auto offset = f.offset(); const auto before = f.scene.currentVertices()[offset].position;
  const std::array<uint32_t, 1> unavailable{1};
  EXPECT_THROW(f.scene.configure(f.data, f.model, unavailable), std::invalid_argument);
  auto duplicated = f.data; duplicated.primitives.push_back(duplicated.primitives.front());
  const auto geometry = Model::FromMeshes({f.model.meshes()[0], f.model.meshes()[0]});
  const std::array<uint32_t, 2> duplicateIds{0, 0};
  EXPECT_THROW(f.scene.configure(duplicated, geometry, duplicateIds), std::invalid_argument);
  EXPECT_EQ(f.scene.geometryRevision(), revision); EXPECT_EQ(f.scene.topologyRevision(), topology);
  expectPosition(f.scene.currentVertices()[offset].position, before);
  EXPECT_FALSE(f.update().historyReset);
}
