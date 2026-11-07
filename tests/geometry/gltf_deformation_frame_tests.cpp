#include <Container/geometry/GltfDeformationFrames.h>

#include <gtest/gtest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace gltf = container::geometry::gltf;
using container::geometry::Vertex;

Vertex vertex(glm::vec3 position, glm::vec2 uv = {}) {
  Vertex result;
  result.position = position;
  result.normal = {0, 0, 1};
  result.tangent = {1, 0, 0, 1};
  result.texCoord = uv;
  result.texCoord1 = uv;
  return result;
}

gltf::DeformationPrimitive triangle() {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {vertex({0, 0, 0}, {0, 0}),
                            vertex({1, 0, 0}, {1, 0}),
                            vertex({0, 1, 0}, {0, 1})};
  return primitive;
}

void expectVector(glm::vec3 actual, glm::vec3 expected, float epsilon = 1e-5f) {
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(actual[i], expected[i], epsilon);
}

void expectFrame(const Vertex& value) {
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(std::isfinite(value.position[i]));
    EXPECT_TRUE(std::isfinite(value.normal[i]));
    EXPECT_TRUE(std::isfinite(value.tangent[i]));
  }
  EXPECT_NEAR(glm::length(value.normal), 1, 1e-5f);
  EXPECT_NEAR(glm::length(glm::vec3(value.tangent)), 1, 1e-5f);
  EXPECT_NEAR(glm::dot(value.normal, glm::vec3(value.tangent)), 0, 1e-5f);
  EXPECT_FLOAT_EQ(std::abs(value.tangent.w), 1);
}

constexpr std::array<uint32_t, 3> triangleIndices{0, 1, 2};
}  // namespace

TEST(GltfDeformationFrames, RemapsEveryCornerAttributeDeltaAndInfluenceSet) {
  auto primitive = triangle();
  primitive.sourceMesh = 7;
  primitive.sourcePrimitive = 2;
  primitive.bindVertices.push_back(vertex({1, 1, 0}, {1, 1}));
  primitive.influences.resize(4);
  gltf::MorphTarget target;
  for (uint32_t i = 0; i < 4; ++i) {
    primitive.bindVertices[i].color = {float(i), 0.2f, 0.7f};
    primitive.bindVertices[i].texCoord1 = {float(i), 0.75f};
    primitive.influences[i].values = {{i, 0.25f}, {i + 5, 0.75f}};
    target.positions.push_back({float(i), 0, 0});
    target.normals.push_back({0, float(i), 0});
    target.tangents.push_back({0, 0, float(i)});
  }
  primitive.morphTargets = {target, {}};
  primitive.meshDefaultWeights = {0, 0};
  const std::array<uint32_t, 6> indices{0, 1, 2, 1, 3, 2};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, indices, {});
  ASSERT_EQ(prepared.primitive.bindVertices.size(), 6u);
  EXPECT_EQ(prepared.indices, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5}));
  EXPECT_EQ(prepared.sourceVertexMap, (std::vector<uint32_t>{0, 1, 2, 1, 3, 2}));
  EXPECT_EQ(prepared.primitive.sourceMesh, 7u);
  EXPECT_EQ(prepared.primitive.sourcePrimitive, 2u);
  EXPECT_EQ(prepared.primitive.meshDefaultWeights, primitive.meshDefaultWeights);
  for (size_t corner = 0; corner < indices.size(); ++corner) {
    const auto source = indices[corner];
    const auto& actual = prepared.primitive.bindVertices[corner];
    const auto& expected = primitive.bindVertices[source];
    EXPECT_EQ(actual.position, expected.position);
    EXPECT_EQ(actual.normal, expected.normal);
    EXPECT_EQ(actual.tangent, expected.tangent);
    EXPECT_EQ(actual.color, expected.color);
    EXPECT_EQ(actual.texCoord, expected.texCoord);
    EXPECT_EQ(actual.texCoord1, expected.texCoord1);
    const auto& influences = prepared.primitive.influences[corner].values;
    ASSERT_EQ(influences.size(), 2u);
    EXPECT_EQ(influences[0].joint, source);
    EXPECT_EQ(influences[1].joint, source + 5);
    EXPECT_FLOAT_EQ(influences[0].weight, 0.25f);
    EXPECT_FLOAT_EQ(influences[1].weight, 0.75f);
    EXPECT_EQ(prepared.primitive.morphTargets[0].positions[corner], target.positions[source]);
    EXPECT_EQ(prepared.primitive.morphTargets[0].normals[corner], target.normals[source]);
    EXPECT_EQ(prepared.primitive.morphTargets[0].tangents[corner], target.tangents[source]);
  }
  EXPECT_TRUE(prepared.primitive.morphTargets[1].positions.empty());
  EXPECT_TRUE(prepared.primitive.morphTargets[1].normals.empty());
  EXPECT_TRUE(prepared.primitive.morphTargets[1].tangents.empty());
  EXPECT_EQ(primitive.bindVertices.size(), 4u);
  EXPECT_EQ(primitive.morphTargets[0].positions.size(), 4u);
}

TEST(GltfDeformationFrames, MissingNormalsStayFlatAcrossSharedSourceVertices) {
  auto primitive = triangle();
  primitive.bindVertices.push_back(vertex({0, 0, 1}, {1, 1}));
  const std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, indices, {false, true, 0});
  EXPECT_FALSE(prepared.primitive.authoredBaseNormal);
  EXPECT_TRUE(prepared.primitive.authoredBaseTangent);
  const auto actual = gltf::EvaluateDeformationFrames(prepared);
  ASSERT_EQ(actual.size(), 6u);
  for (size_t i = 0; i < actual.size(); ++i) {
    expectVector(actual[i].normal, i < 3 ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
    expectFrame(actual[i]);
  }
  EXPECT_EQ(actual[0].position, actual[3].position);
  EXPECT_NE(actual[0].normal, actual[3].normal);
}

TEST(GltfDeformationFrames, ComputesFinalNormalsForMultiplePartialAndNegativeMorphWeights) {
  auto primitive = triangle();
  gltf::MorphTarget a, b;
  a.positions = {{0, 0, 0}, {0, 0, 2}, {0, 0, 0}};
  b.positions = {{0, 0, 0}, {0, 0, 0}, {0, 0, 3}};
  primitive.morphTargets = {a, b};
  primitive.meshDefaultWeights = {0.5f, -0.25f};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {false, false, 0});
  const auto actual = gltf::EvaluateDeformationFrames(prepared);
  const auto expected = glm::normalize(glm::vec3(-1, 0.75f, 1));
  const auto interpolatedTargetNormals = glm::normalize(glm::vec3(0, 0, 1) +
      0.5f * (glm::normalize(glm::vec3(-2, 0, 1)) - glm::vec3(0, 0, 1)) -
      0.25f * (glm::normalize(glm::vec3(0, -3, 1)) - glm::vec3(0, 0, 1)));
  EXPECT_GT(glm::distance(expected, interpolatedTargetNormals), 0.1f);
  for (const auto& value : actual) {
    expectVector(value.normal, expected);
    expectFrame(value);
  }
  expectVector(actual[1].position, {1, 0, 1});
  expectVector(actual[2].position, {0, 1, -0.75f});
}

TEST(GltfDeformationFrames, MissingBaseNormalIgnoresAuthoredTangentAndFrameDeltas) {
  auto primitive = triangle();
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  for (auto& value : primitive.bindVertices) {
    value.normal = glm::vec3(nan);
    value.tangent = glm::vec4(nan);
  }
  gltf::MorphTarget target;
  target.positions = {{0, 0, 0}, {0, 0, 1}, {0, 0, 0}};
  target.normals.assign(3, glm::vec3(nan));
  target.tangents.assign(3, glm::vec3(nan));
  primitive.morphTargets = {target};
  primitive.meshDefaultWeights = {1};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {false, true, 0});
  EXPECT_TRUE(prepared.primitive.morphTargets[0].normals.empty());
  EXPECT_TRUE(prepared.primitive.morphTargets[0].tangents.empty());
  const auto actual = gltf::EvaluateDeformationFrames(prepared);
  for (const auto& value : actual) {
    expectVector(value.normal, glm::normalize(glm::vec3(-1, 0, 1)));
    expectVector(glm::vec3(value.tangent), glm::normalize(glm::vec3(1, 0, 1)));
    EXPECT_FLOAT_EQ(value.tangent.w, 1);
  }
}

TEST(GltfDeformationFrames, AuthoredNormalSurvivesAnOmittedTargetNormal) {
  auto primitive = triangle();
  const auto normal = glm::normalize(glm::vec3(0.1f, 0.2f, 1));
  for (auto& value : primitive.bindVertices) value.normal = normal;
  gltf::MorphTarget target;
  target.positions = {{0, 0, 0}, {0, 0, 2}, {0, 0, -1}};
  primitive.morphTargets = {target};
  primitive.meshDefaultWeights = {0.5f};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {true, false, 0});
  const auto actual = gltf::EvaluateDeformationFrames(prepared);
  for (const auto& value : actual) {
    expectVector(value.normal, normal);
    expectFrame(value);
  }
  const auto face = glm::normalize(glm::cross(actual[1].position - actual[0].position,
                                             actual[2].position - actual[0].position));
  EXPECT_GT(glm::distance(normal, face), 0.5f);
}

TEST(GltfDeformationFrames, RegeneratedTangentsUseMorphedAuthoredNormals) {
  auto primitive = triangle();
  gltf::MorphTarget target;
  target.normals.assign(3, glm::vec3(0.4f, 0.2f, 0));
  primitive.morphTargets = {target};
  primitive.meshDefaultWeights = {0.5f};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {true, false, 0});
  const auto actual = gltf::EvaluateDeformationFrames(prepared);
  for (const auto& value : actual) {
    expectVector(value.normal, glm::normalize(glm::vec3(0.2f, 0.1f, 1)));
    expectFrame(value);
  }
}

TEST(GltfDeformationFrames, Uv1MikkOrientationSeamsKeepIndependentCornerHandedness) {
  auto primitive = triangle();
  primitive.bindVertices.push_back(vertex({1, 1, 0}, {1, 1}));
  primitive.bindVertices[1].texCoord1 = {-1, 0};
  const std::array<uint32_t, 6> indices{0, 1, 2, 1, 3, 2};
  const auto uv0 = gltf::PrepareDeformationFrames(primitive, indices, {true, false, 0});
  const auto uv1 = gltf::PrepareDeformationFrames(primitive, indices, {true, false, 1});
  const auto ordinary = gltf::EvaluateDeformationFrames(uv0);
  const auto mirrored = gltf::EvaluateDeformationFrames(uv1);
  ASSERT_EQ(mirrored.size(), 6u);
  for (size_t i = 0; i < mirrored.size(); ++i) {
    expectVector(glm::vec3(ordinary[i].tangent), {1, 0, 0});
    EXPECT_FLOAT_EQ(ordinary[i].tangent.w, 1);
    expectVector(glm::vec3(mirrored[i].tangent), i < 3 ? glm::vec3(-1, 0, 0) : glm::vec3(1, 0, 0));
    EXPECT_FLOAT_EQ(mirrored[i].tangent.w, i < 3 ? -1.0f : 1.0f);
    expectFrame(mirrored[i]);
  }
  EXPECT_EQ(uv1.sourceVertexMap[1], uv1.sourceVertexMap[3]);
  EXPECT_NE(mirrored[1].tangent.w, mirrored[3].tangent.w);
}

TEST(GltfDeformationFrames, GeneratesMorphedFramesBeforeNonuniformSkinning) {
  auto primitive = triangle();
  primitive.influences.resize(3);
  for (auto& value : primitive.influences) value.values = {{0, 1}};
  gltf::MorphTarget target;
  target.positions = {{0, 0, 0}, {0, 0, 2}, {0, 0, -1}};
  primitive.morphTargets = {target};
  primitive.meshDefaultWeights = {0.5f};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {false, false, 0});
  const auto unskinned = gltf::EvaluateDeformationFrames(prepared);
  const auto skin = glm::translate(glm::mat4(1), glm::vec3(2, 3, 4)) *
      glm::rotate(glm::mat4(1), 0.7f, glm::normalize(glm::vec3(1, 2, 3))) *
      glm::scale(glm::mat4(1), glm::vec3(2, 3, 0.5f));
  const auto skinned = gltf::EvaluateDeformationFrames(prepared, {}, std::array{skin});
  const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(skin)));
  for (size_t i = 0; i < skinned.size(); ++i) {
    expectVector(skinned[i].position, glm::vec3(skin * glm::vec4(unskinned[i].position, 1)));
    expectVector(skinned[i].normal, glm::normalize(normalMatrix * unskinned[i].normal));
    expectVector(glm::vec3(skinned[i].tangent), glm::normalize(glm::mat3(skin) * glm::vec3(unskinned[i].tangent)));
    expectFrame(skinned[i]);
  }
}

TEST(GltfDeformationFrames, MirrorsSkinHandednessOnceAndRetainsFiniteSingularFrames) {
  auto primitive = triangle();
  primitive.influences.resize(3);
  for (auto& value : primitive.influences) value.values = {{0, 1}};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {false, false, 0});
  const auto mirrored = gltf::EvaluateDeformationFrames(prepared, {},
      std::array{glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4))});
  const auto singular = gltf::EvaluateDeformationFrames(prepared, {},
      std::array{glm::scale(glm::mat4(1), glm::vec3(0))});
  for (size_t i = 0; i < mirrored.size(); ++i) {
    expectVector(mirrored[i].normal, {0, 0, 1});
    expectVector(glm::vec3(mirrored[i].tangent), {-1, 0, 0});
    EXPECT_FLOAT_EQ(mirrored[i].tangent.w, -1);
    expectVector(singular[i].position, {0, 0, 0});
    expectVector(singular[i].normal, {0, 0, 1});
    EXPECT_FLOAT_EQ(singular[i].tangent.w, 1);
    expectFrame(mirrored[i]); expectFrame(singular[i]);
  }
}

TEST(GltfDeformationFrames, CompleteAuthoredFramesMatchOrdinaryEvaluatorAndStayPure) {
  auto primitive = triangle();
  gltf::MorphTarget target;
  target.positions.assign(3, glm::vec3(0, 0, 0.5f));
  target.normals.assign(3, glm::vec3(0.2f, 0.3f, 0));
  target.tangents.assign(3, glm::vec3(0, 0.2f, 0.4f));
  primitive.morphTargets = {target}; primitive.meshDefaultWeights = {0.75f};
  primitive.influences.resize(3);
  for (auto& value : primitive.influences) value.values = {{0, 1}};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {});
  const std::array palette{glm::scale(glm::mat4(1), glm::vec3(2, 3, 0.25f))};
  const auto expected = gltf::EvaluatePrimitive(primitive, {}, palette);
  const auto actual = gltf::EvaluateDeformationFrames(prepared, {}, palette);
  const auto repeat = gltf::EvaluateDeformationFrames(prepared, {}, palette);
  for (size_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual[i].position, expected[i].position);
    EXPECT_EQ(actual[i].normal, expected[i].normal);
    EXPECT_EQ(actual[i].tangent, expected[i].tangent);
    EXPECT_EQ(repeat[i].position, actual[i].position);
    EXPECT_EQ(repeat[i].normal, actual[i].normal);
    EXPECT_EQ(repeat[i].tangent, actual[i].tangent);
    EXPECT_EQ(prepared.primitive.bindVertices[i].position, primitive.bindVertices[i].position);
    EXPECT_EQ(prepared.primitive.morphTargets[0].normals[i], target.normals[i]);
  }
}

TEST(GltfDeformationFrames, CollapsedMorphAndDegenerateUvKeepFiniteRepeatableFrames) {
  auto primitive = triangle();
  for (auto& value : primitive.bindVertices) value.texCoord = {0, 0};
  gltf::MorphTarget target;
  target.positions = {{0, 0, 0}, {-1, 0, 0}, {0, 0, 0}};
  primitive.morphTargets = {target}; primitive.meshDefaultWeights = {1};
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {false, false, 0});
  const auto first = gltf::EvaluateDeformationFrames(prepared);
  const auto second = gltf::EvaluateDeformationFrames(prepared);
  for (size_t i = 0; i < first.size(); ++i) {
    expectFrame(first[i]);
    expectVector(first[i].normal, {0, 1, 0});
    EXPECT_EQ(first[i].position, second[i].position);
    EXPECT_EQ(first[i].normal, second[i].normal);
    EXPECT_EQ(first[i].tangent, second[i].tangent);
  }
  EXPECT_THROW((void)gltf::EvaluateDeformationFrames(prepared,
      std::array{std::numeric_limits<float>::quiet_NaN()}), std::runtime_error);
}

TEST(GltfDeformationFrames, RejectsInvalidTopologyPolicyAndRetainedInputs) {
  auto primitive = triangle();
  EXPECT_THROW((void)gltf::PrepareDeformationFrames(primitive, std::array<uint32_t, 2>{0, 1}, {}), std::runtime_error);
  EXPECT_THROW((void)gltf::PrepareDeformationFrames(primitive, std::array<uint32_t, 3>{0, 1, 3}, {}), std::runtime_error);
  EXPECT_THROW((void)gltf::PrepareDeformationFrames(primitive, triangleIndices, {true, false, 2}), std::runtime_error);
  primitive.morphTargets.resize(1);
  primitive.meshDefaultWeights = {0};
  primitive.morphTargets[0].positions.resize(2);
  EXPECT_THROW((void)gltf::PrepareDeformationFrames(primitive, triangleIndices, {}), std::runtime_error);
  primitive.morphTargets.clear(); primitive.meshDefaultWeights.clear();
  primitive.influences.resize(1);
  EXPECT_THROW((void)gltf::PrepareDeformationFrames(primitive, triangleIndices, {}), std::runtime_error);
  primitive.influences.clear();
  primitive.bindVertices[1].texCoord1.x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)gltf::PrepareDeformationFrames(primitive, triangleIndices, {true, false, 1}), std::runtime_error);
  const auto prepared = gltf::PrepareDeformationFrames(primitive, triangleIndices, {true, false, 0});
  EXPECT_NO_THROW((void)gltf::EvaluateDeformationFrames(prepared));
  auto invalid = prepared;
  invalid.indices[1] = 0;
  EXPECT_THROW((void)gltf::EvaluateDeformationFrames(invalid), std::runtime_error);
}
