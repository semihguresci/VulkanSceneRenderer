#include <Container/geometry/GltfPrimitiveFrameVariants.h>

#include <gtest/gtest.h>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using container::geometry::Vertex;

constexpr std::array<uint32_t, 3> indices{0, 1, 2};

DeformationPrimitive triangle() {
  DeformationPrimitive result;
  result.sourceMesh = 7;
  result.sourcePrimitive = 2;
  result.bindVertices.resize(3);
  for (size_t i = 0; i < 3; ++i) {
    auto& v = result.bindVertices[i];
    v.position = {i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f, 0};
    v.normal = {0, 0, 1};
    v.tangent = {1, 0, 0, 1};
    v.texCoord = {v.position.x, v.position.y};
    v.texCoord1 = {-v.position.x, v.position.y};
    v.color = {.2f + float(i) * .1f, .4f, .7f};
    v.colorAlpha = .8f;
  }
  return result;
}

CanonicalPrimitiveFrameMetadata metadata(const DeformationPrimitive& primitive,
                                         uint8_t uvMask = 3, uint64_t revision = 11) {
  return {9, 4, primitive.authoredBaseNormal, primitive.authoredBaseTangent,
          primitive.normalTextureTexCoord, uvMask, revision};
}

template<size_t N> PrimitiveFrameVariantPlan plan(const DeformationPrimitive& primitive,
    const std::array<PrimitiveFrameBindingRequest, N>& requests) {
  return PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests);
}

void expectVector(glm::vec3 actual, glm::vec3 expected, float epsilon = 1e-5f) {
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(actual[i], expected[i], epsilon);
}

void expectVerticesEqual(const std::vector<Vertex>& a, const std::vector<Vertex>& b) {
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].position, b[i].position);
    EXPECT_EQ(a[i].normal, b[i].normal);
    EXPECT_EQ(a[i].tangent, b[i].tangent);
    EXPECT_EQ(a[i].texCoord, b[i].texCoord);
    EXPECT_EQ(a[i].texCoord1, b[i].texCoord1);
    EXPECT_EQ(a[i].color, b[i].color);
    EXPECT_EQ(a[i].colorAlpha, b[i].colorAlpha);
  }
}
} // namespace

TEST(GltfPrimitiveFrameVariants, OppositeUvsReplaceAuthoredTangentOnlyForTheRequestedBinding) {
  const auto primitive = triangle();
  const std::array<PrimitiveFrameBindingRequest, 3> requests{{{10, 0}, {20, 1}, {30, std::nullopt}}};
  const auto result = plan(primitive, requests);
  ASSERT_EQ(result.templates().size(), 1u);
  ASSERT_EQ(result.selections().size(), 3u);
  EXPECT_FALSE(result.selections()[0].templateIndex);
  EXPECT_FALSE(result.selections()[2].templateIndex);
  EXPECT_EQ(result.selections()[0].key.kind, PrimitiveFrameVariantKind::Canonical);
  EXPECT_EQ(result.selections()[1].key.kind, PrimitiveFrameVariantKind::GeneratedUV1);
  ASSERT_EQ(result.selections()[1].templateIndex, 0u);
  const auto& replacement = result.templates()[0];
  EXPECT_FALSE(replacement.frames().policy.authoredBaseTangent);
  EXPECT_TRUE(replacement.frames().policy.authoredBaseNormal);
  const auto evaluated = EvaluateDeformationFrames(replacement.frames());
  for (const auto& v : evaluated) {
    expectVector(glm::vec3(v.tangent), {-1, 0, 0});
    EXPECT_FLOAT_EQ(v.tangent.w, -1);
  }
  for (const auto& v : primitive.bindVertices) EXPECT_EQ(v.tangent, glm::vec4(1, 0, 0, 1));
}

TEST(GltfPrimitiveFrameVariants, MissingFramesAtTheOriginalUvReuseTheExistingCanonicalFallback) {
  auto primitive = triangle();
  primitive.authoredBaseNormal = false;
  primitive.authoredBaseTangent = false;
  const std::array<PrimitiveFrameBindingRequest, 2> requests{{{10, 0}, {11, std::nullopt}}};
  const auto result = plan(primitive, requests);
  EXPECT_TRUE(result.templates().empty());
  for (const auto& selection : result.selections()) {
    EXPECT_EQ(selection.key.kind, PrimitiveFrameVariantKind::Canonical);
    EXPECT_FALSE(selection.templateIndex);
    EXPECT_EQ(selection.exportPolicy, (PrimitiveFrameExportPolicy{false, false, false, false}));
  }
}

TEST(GltfPrimitiveFrameVariants, UvZeroReplacementOfAnUvOneBindHasIndependentAnalyticalFrame) {
  auto primitive = triangle();
  primitive.normalTextureTexCoord = 1;
  for (auto& v : primitive.bindVertices) v.tangent = {-1, 0, 0, -1};
  const std::array<PrimitiveFrameBindingRequest, 2> requests{{{4, 1}, {8, 0}}};
  const auto result = plan(primitive, requests);
  ASSERT_EQ(result.templates().size(), 1u);
  EXPECT_EQ(result.templates()[0].key().kind, PrimitiveFrameVariantKind::GeneratedUV0);
  EXPECT_FALSE(result.selections()[0].templateIndex);
  for (const auto& v : EvaluateDeformationFrames(result.templates()[0].frames())) {
    expectVector(glm::vec3(v.tangent), {1, 0, 0});
    EXPECT_FLOAT_EQ(v.tangent.w, 1);
  }
}

TEST(GltfPrimitiveFrameVariants, MaterialOwnerAndTransformIndependentRequestsShareOneTemplate) {
  const auto primitive = triangle();
  const std::array<PrimitiveFrameBindingRequest, 5> requests{{{99, 1}, {1, 0}, {42, 1}, {700, 1}, {2, std::nullopt}}};
  const auto result = plan(primitive, requests);
  ASSERT_EQ(result.templates().size(), 1u);
  EXPECT_EQ(result.selections()[0].templateIndex, result.selections()[2].templateIndex);
  EXPECT_EQ(result.selections()[2].templateIndex, result.selections()[3].templateIndex);
  EXPECT_EQ(result.selections()[0].key, result.selections()[3].key);
  EXPECT_EQ(result.selections()[0].graphNode, 99u);
  EXPECT_EQ(result.selections()[3].graphNode, 700u);
  const auto originalOutputBytes = result.outputBytes();
  auto second = requests;
  second[0].graphNode = 3;
  const auto otherBindings = plan(primitive, second);
  EXPECT_EQ(result.templates()[0].key(), otherBindings.templates()[0].key());
  EXPECT_EQ(originalOutputBytes, otherBindings.outputBytes());
}

TEST(GltfPrimitiveFrameVariants, ExportPolicySeparatesCanonicalTangentsFromReplacementTangents) {
  auto primitive = triangle();
  MorphTarget target;
  target.positions.assign(3, glm::vec3(0, 0, .25f));
  target.normals.assign(3, glm::vec3(.1f, 0, 0));
  target.tangents.assign(3, glm::vec3(0, .4f, 0));
  primitive.morphTargets.push_back(target);
  primitive.meshDefaultWeights = {.5f};
  const std::array<PrimitiveFrameBindingRequest, 2> requests{{{1, 0}, {2, 1}}};
  const auto result = plan(primitive, requests);
  EXPECT_EQ(result.selections()[0].exportPolicy, (PrimitiveFrameExportPolicy{true, true, true, true}));
  EXPECT_EQ(result.selections()[1].exportPolicy, (PrimitiveFrameExportPolicy{true, false, true, false}));
  const auto& derived = result.templates()[0].frames().primitive;
  EXPECT_EQ(derived.morphTargets[0].positions, target.positions);
  EXPECT_EQ(derived.morphTargets[0].normals, target.normals);
  EXPECT_TRUE(derived.morphTargets[0].tangents.empty());
  EXPECT_EQ(primitive.morphTargets[0].tangents, target.tangents);
  EXPECT_EQ(primitive.meshDefaultWeights, (std::vector<float>{.5f}));
}

TEST(GltfPrimitiveFrameVariants, CornerExpansionKeepsMorphSkinAndAuxiliaryAttributesAligned) {
  auto primitive = triangle();
  primitive.bindVertices.push_back(primitive.bindVertices[2]);
  primitive.bindVertices[3].position = {1, 1, 0};
  primitive.bindVertices[3].texCoord = {1, 1};
  primitive.bindVertices[3].texCoord1 = {-1, 1};
  primitive.influences.resize(4);
  MorphTarget target;
  for (size_t i = 0; i < 4; ++i) {
    primitive.influences[i].values = {{0, .25f}, {1, .75f}};
    target.positions.push_back({0, 0, float(i) * .2f});
    target.normals.push_back({.1f, 0, 0});
    target.tangents.push_back({0, .3f, 0});
    primitive.bindVertices[i].colorAlpha = float(i) * .2f;
  }
  primitive.morphTargets = {target};
  primitive.meshDefaultWeights = {.5f};
  const std::array<uint32_t, 6> quadIndices{0, 1, 2, 1, 3, 2};
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{12, 1}}};
  const auto result = PreparePrimitiveFrameVariants(metadata(primitive), primitive, quadIndices, requests);
  const auto& frames = result.templates()[0].frames();
  EXPECT_EQ(frames.sourceVertexMap, (std::vector<uint32_t>{0, 1, 2, 1, 3, 2}));
  EXPECT_EQ(frames.indices, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5}));
  EXPECT_EQ(frames.primitive.sourceMesh, 7u);
  EXPECT_EQ(frames.primitive.sourcePrimitive, 2u);
  const std::array<glm::mat4, 2> palette{
      glm::translate(glm::mat4(1), glm::vec3(0, 2, 0)),
      glm::translate(glm::mat4(1), glm::vec3(0, 6, 0))};
  const std::array<float, 1> weights{.5f};
  const auto evaluated = EvaluateDeformationFrames(frames, weights, palette);
  ASSERT_EQ(evaluated.size(), 6u);
  for (size_t i = 0; i < 6; ++i) {
    const auto source = quadIndices[i];
    const auto& bind = primitive.bindVertices[source];
    EXPECT_EQ(frames.primitive.bindVertices[i].colorAlpha, bind.colorAlpha);
    EXPECT_EQ(frames.primitive.bindVertices[i].texCoord, bind.texCoord);
    EXPECT_EQ(frames.primitive.bindVertices[i].texCoord1, bind.texCoord1);
    EXPECT_EQ(frames.primitive.morphTargets[0].positions[i], target.positions[source]);
    ASSERT_EQ(frames.primitive.influences[i].values.size(), 2u);
    EXPECT_FLOAT_EQ(frames.primitive.influences[i].values[1].weight, .75f);
    expectVector(evaluated[i].position, bind.position + .5f * target.positions[source] + glm::vec3(0, 5, 0));
    EXPECT_TRUE(frames.primitive.morphTargets[0].tangents.empty());
  }
}

TEST(GltfPrimitiveFrameVariants, MissingNormalIgnoresAuthoredTangentAndNormalTangentMorphDeltas) {
  auto primitive = triangle();
  primitive.authoredBaseNormal = false;
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  for (auto& v : primitive.bindVertices) {
    v.normal = glm::vec3(nan);
    v.tangent = glm::vec4(nan);
  }
  MorphTarget target;
  target.positions = {{0, 0, 0}, {0, 0, 1}, {0, 0, 0}};
  target.normals.assign(3, glm::vec3(nan));
  target.tangents.assign(3, glm::vec3(nan));
  primitive.morphTargets = {target};
  primitive.meshDefaultWeights = {1};
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{12, 1}}};
  const auto result = plan(primitive, requests);
  const auto& frames = result.templates()[0].frames();
  EXPECT_TRUE(frames.primitive.morphTargets[0].normals.empty());
  EXPECT_TRUE(frames.primitive.morphTargets[0].tangents.empty());
  EXPECT_EQ(result.selections()[0].exportPolicy, (PrimitiveFrameExportPolicy{false, false, false, false}));
  for (const auto& v : EvaluateDeformationFrames(frames)) {
    expectVector(v.normal, glm::normalize(glm::vec3(-1, 0, 1)));
    expectVector(glm::vec3(v.tangent), glm::normalize(glm::vec3(-1, 0, -1)));
    EXPECT_FLOAT_EQ(v.tangent.w, -1);
  }
}

TEST(GltfPrimitiveFrameVariants, UntexturedNoUvBindingAliasesCanonicalButExplicitAbsentUvFails) {
  const auto primitive = triangle();
  const auto capture = metadata(primitive, 0);
  const std::array<PrimitiveFrameBindingRequest, 1> untextured{{{1, std::nullopt}}};
  const auto result = PreparePrimitiveFrameVariants(capture, primitive, indices, untextured);
  EXPECT_TRUE(result.templates().empty());
  EXPECT_EQ(result.selections()[0].key.kind, PrimitiveFrameVariantKind::Canonical);
  for (uint32_t uv : {0u, 1u, 2u, std::numeric_limits<uint32_t>::max()}) {
    const std::array<PrimitiveFrameBindingRequest, 1> textured{{{1, uv}}};
    EXPECT_THROW(PreparePrimitiveFrameVariants(capture, primitive, indices, textured), std::runtime_error);
  }
}

TEST(GltfPrimitiveFrameVariants, CapturedUvNeverTracksAMaterialEditAndMismatchedCanonicalInputRejects) {
  auto primitive = triangle();
  const auto capture = metadata(primitive);
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{1, 1}}};
  const auto result = PreparePrimitiveFrameVariants(capture, primitive, indices, requests);
  EXPECT_EQ(capture.bindNormalTexCoord(), 0u);
  EXPECT_EQ(result.selections()[0].key.kind, PrimitiveFrameVariantKind::GeneratedUV1);
  primitive.normalTextureTexCoord = 1;
  EXPECT_THROW(PreparePrimitiveFrameVariants(capture, primitive, indices, requests), std::runtime_error);
  primitive.normalTextureTexCoord = 0;
  primitive.authoredBaseTangent = false;
  EXPECT_THROW(PreparePrimitiveFrameVariants(capture, primitive, indices, requests), std::runtime_error);
}

TEST(GltfPrimitiveFrameVariants, RevisionAndPrimitiveIdentityInvalidateKeysWithoutInventingSourceIds) {
  const auto primitive = triangle();
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{1, 1}}};
  const auto first = PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests);
  const auto changed = PreparePrimitiveFrameVariants(metadata(primitive, 3, 12), primitive, indices, requests);
  EXPECT_NE(first.selections()[0].key, changed.selections()[0].key);
  const CanonicalPrimitiveFrameMetadata procedural(14, std::nullopt, true, true, 0, 3, 11);
  const auto generated = PreparePrimitiveFrameVariants(procedural, primitive, indices, requests);
  EXPECT_FALSE(generated.canonical().sourceFlatPrimitive());
  EXPECT_EQ(generated.selections()[0].key.runtimePrimitive, 14u);
  EXPECT_EQ(first.canonical().sourceFlatPrimitive(), 4u);
  EXPECT_EQ(generated.templates()[0].frames().primitive.sourceMesh, primitive.sourceMesh);
  EXPECT_EQ(generated.selections()[0].exportPolicy, (PrimitiveFrameExportPolicy{true, false, true, false}));
}

TEST(GltfPrimitiveFrameVariants, DuplicateOrReservedBindingsRejectWithoutChangingInput) {
  const auto primitive = triangle();
  const auto before = primitive.bindVertices;
  const std::array<PrimitiveFrameBindingRequest, 2> duplicate{{{3, 0}, {3, 1}}};
  EXPECT_THROW(plan(primitive, duplicate), std::runtime_error);
  const std::array<PrimitiveFrameBindingRequest, 1> reserved{{{std::numeric_limits<uint32_t>::max(), 1}}};
  EXPECT_THROW(plan(primitive, reserved), std::runtime_error);
  expectVerticesEqual(primitive.bindVertices, before);
}

TEST(GltfPrimitiveFrameVariants, InputOutputAndWorkspaceBoundariesFailBeforeCopyAndRetainSource) {
  const auto primitive = triangle();
  const auto before = primitive.bindVertices;
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{1, 1}}};
  const auto result = plan(primitive, requests);
  PrimitiveFrameVariantLimits limits;
  limits.maxOutputBytes = result.outputBytes();
  limits.maxWorkspaceBytes = result.workspaceBytes();
  EXPECT_NO_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits));
  --limits.maxOutputBytes;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits), std::runtime_error);
  limits = {};
  limits.maxWorkspaceBytes = result.workspaceBytes() - 1;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits), std::runtime_error);
  limits = {};
  limits.maxInputBytes = 1;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits), std::runtime_error);
  expectVerticesEqual(primitive.bindVertices, before);
}

TEST(GltfPrimitiveFrameVariants, CountCapsAndMalformedLayoutsRejectWithoutOutputPublication) {
  auto primitive = triangle();
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{1, 1}}};
  PrimitiveFrameVariantLimits limits;
  limits.maxInputVertices = 2;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits), std::runtime_error);
  limits = {};
  limits.maxTriangleCorners = 2;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits), std::runtime_error);
  limits = {};
  limits.maxBindings = 0;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits), std::runtime_error);
  const std::array<uint32_t, 3> badIndex{0, 1, 3};
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, badIndex, requests), std::runtime_error);
  primitive.morphTargets.resize(1);
  primitive.meshDefaultWeights = {0};
  primitive.morphTargets[0].positions.resize(2);
  EXPECT_THROW(plan(primitive, requests), std::runtime_error);
  primitive = triangle();
  primitive.influences.resize(2);
  EXPECT_THROW(plan(primitive, requests), std::runtime_error);
}

TEST(GltfPrimitiveFrameVariants, ExpandedInfluenceBudgetAndInvalidWeightsReject) {
  auto primitive = triangle();
  primitive.influences.resize(3);
  for (auto& vertex : primitive.influences) vertex.values = {{65535, .25f}, {3, .75f}};
  const std::array<uint32_t, 6> repeated{0, 1, 2, 0, 1, 2};
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{1, 1}}};
  PrimitiveFrameVariantLimits limits;
  limits.maxInfluences = 11;
  EXPECT_THROW(PreparePrimitiveFrameVariants(metadata(primitive), primitive, repeated, requests, limits), std::runtime_error);
  limits.maxInfluences = 12;
  const auto result = PreparePrimitiveFrameVariants(metadata(primitive), primitive, repeated, requests, limits);
  EXPECT_EQ(result.templates()[0].frames().primitive.influences[3].values[0].joint, 65535u);
  EXPECT_FLOAT_EQ(result.templates()[0].frames().primitive.influences[3].values[0].weight, .25f);
  primitive.influences[0].values[0].weight = -1;
  EXPECT_THROW(plan(primitive, requests), std::runtime_error);
}

TEST(GltfPrimitiveFrameVariants, NonFiniteRetainedPayloadAndUnsupportedMetadataReject) {
  auto primitive = triangle();
  const std::array<PrimitiveFrameBindingRequest, 1> requests{{{1, 1}}};
  primitive.bindVertices[1].texCoord1.x = std::numeric_limits<float>::infinity();
  EXPECT_THROW(plan(primitive, requests), std::runtime_error);
  primitive = triangle();
  primitive.bindVertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(plan(primitive, requests), std::runtime_error);
  EXPECT_THROW((CanonicalPrimitiveFrameMetadata(1, 0, true, true, 2, 3, 0)), std::runtime_error);
  EXPECT_THROW((CanonicalPrimitiveFrameMetadata(1, 0, true, true, 0, 4, 0)), std::runtime_error);
}

TEST(GltfPrimitiveFrameVariants, CanonicalAliasesNeedNoEvaluatorWorkspaceOrGeometryCopyBudget) {
  const auto primitive = triangle();
  const std::array<PrimitiveFrameBindingRequest, 2> requests{{{1, 0}, {2, std::nullopt}}};
  PrimitiveFrameVariantLimits limits;
  limits.maxWorkspaceBytes = requests.size() * sizeof(uint32_t);
  const auto result = PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, requests, limits);
  EXPECT_TRUE(result.templates().empty());
  EXPECT_EQ(result.workspaceBytes(), requests.size() * sizeof(uint32_t));
  const std::array<PrimitiveFrameBindingRequest, 0> none{};
  limits.maxWorkspaceBytes = 0;
  const auto empty = PreparePrimitiveFrameVariants(metadata(primitive), primitive, indices, none, limits);
  EXPECT_TRUE(empty.selections().empty());
  EXPECT_TRUE(empty.templates().empty());
  EXPECT_EQ(empty.workspaceBytes(), 0u);
}
