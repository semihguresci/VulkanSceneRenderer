#include <Container/geometry/GltfDeformation.h>

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
namespace gltf = container::geometry::gltf;
using container::geometry::Mesh;
using container::geometry::Vertex;

void expectVec3(const glm::vec3& actual, const glm::vec3& expected,
                float tolerance = 1.0e-5f) {
  EXPECT_NEAR(actual.x, expected.x, tolerance);
  EXPECT_NEAR(actual.y, expected.y, tolerance);
  EXPECT_NEAR(actual.z, expected.z, tolerance);
}

Vertex bindVertex(glm::vec3 position = glm::vec3(1, 0, 0)) {
  Vertex vertex;
  vertex.position = position;
  vertex.normal = glm::vec3(0, 0, 1);
  vertex.tangent = glm::vec4(1, 0, 0, -1);
  vertex.color = glm::vec3(0.2f, 0.4f, 0.6f);
  vertex.texCoord = glm::vec2(0.25f, 0.5f);
  vertex.texCoord1 = glm::vec2(0.75f, 1.0f);
  return vertex;
}

gltf::DeformationPrimitive primitiveWithVertex(Vertex vertex = bindVertex()) {
  gltf::DeformationPrimitive primitive;
  primitive.bindVertices = {vertex};
  return primitive;
}

gltf::DeformationPrimitive skinnedPrimitive(Vertex vertex = bindVertex()) {
  auto primitive = primitiveWithVertex(vertex);
  primitive.influences = {{{{0, 1.0f}}}};
  return primitive;
}

class SourceFixture {
 public:
  tinygltf::Model source;
  std::vector<Mesh> runtime;

  explicit SourceFixture(size_t vertexCount = 1) {
    source.buffers.emplace_back();
    source.meshes.emplace_back();
    source.meshes[0].primitives.emplace_back();
    std::vector<float> positions(vertexCount * 3, 0.0f);
    for (size_t i = 0; i < vertexCount; ++i) positions[i * 3] = 1.0f;
    primitive().attributes["POSITION"] = accessor(
        positions, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
    primitive().attributes["NORMAL"] = accessor(
        std::vector<float>(vertexCount * 3, 0.0f), TINYGLTF_TYPE_VEC3,
        TINYGLTF_COMPONENT_TYPE_FLOAT);
    primitive().attributes["TANGENT"] = accessor(
        std::vector<float>(vertexCount * 4, 0.0f), TINYGLTF_TYPE_VEC4,
        TINYGLTF_COMPONENT_TYPE_FLOAT);
    source.nodes.resize(1);
    source.nodes[0].mesh = 0;
    runtime.emplace_back(std::vector<Vertex>(vertexCount, bindVertex()),
                           std::vector<uint32_t>{0, 0, 0});
  }

  tinygltf::Primitive& primitive() { return source.meshes[0].primitives[0]; }

  template <typename T>
  int view(const std::vector<T>& values, size_t stride = 0) {
    auto& bytes = source.buffers[0].data;
    while (bytes.size() % 4 != 0) bytes.push_back(0);
    tinygltf::BufferView bufferView;
    bufferView.buffer = 0;
    bufferView.byteOffset = bytes.size();
    bufferView.byteLength = values.size() * sizeof(T);
    bufferView.byteStride = stride;
    const auto* begin = reinterpret_cast<const unsigned char*>(values.data());
    bytes.insert(bytes.end(), begin, begin + bufferView.byteLength);
    source.bufferViews.push_back(bufferView);
    return static_cast<int>(source.bufferViews.size() - 1);
  }

  template <typename T>
  int accessor(const std::vector<T>& values, int type, int component,
                bool normalized = false) {
    tinygltf::Accessor result;
    result.bufferView = view(values);
    result.type = type;
    result.componentType = component;
    result.normalized = normalized;
    result.count = values.size() / static_cast<size_t>(tinygltf::GetNumComponentsInType(type));
    source.accessors.push_back(result);
    return static_cast<int>(source.accessors.size() - 1);
  }

  void skin(std::vector<uint8_t> jointValues = {0, 0, 0, 0},
            std::vector<float> weightValues = {1, 0, 0, 0},
            std::vector<int> jointNodes = {1}) {
    primitive().attributes["JOINTS_0"] = accessor(
        jointValues, TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    primitive().attributes["WEIGHTS_0"] = accessor(
        weightValues, TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_FLOAT);
    for (const int node : jointNodes) {
      if (source.nodes.size() <= static_cast<size_t>(node)) source.nodes.resize(node + 1);
    }
    tinygltf::Skin skin;
    skin.joints = std::move(jointNodes);
    source.skins.push_back(std::move(skin));
    source.nodes[0].skin = 0;
  }

  void morph(glm::vec3 position = glm::vec3(2, 0, 0)) {
    const size_t count = runtime[0].vertices().size();
    std::vector<float> deltas;
    for (size_t i = 0; i < count; ++i) {
      deltas.insert(deltas.end(), {position.x, position.y, position.z});
    }
    primitive().targets.push_back({{"POSITION", accessor(
        deltas, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT)}});
  }

  gltf::DeformationData load() const {
    return gltf::LoadDeformationData(source, runtime);
  }
};

TEST(GltfDeformation, ExtractsIdentityInverseBindAndPreservesAuxiliaryAttributes) {
  SourceFixture fixture;
  fixture.skin();
  const auto data = fixture.load();
  ASSERT_EQ(data.skins.size(), 1u);
  ASSERT_EQ(data.skins[0].inverseBindMatrices.size(), 1u);
  const std::array<glm::mat4, 2> worlds{glm::mat4(1), glm::mat4(1)};
  const auto palette = gltf::ComputeSkinPalette(data.skins[0], worlds[0], worlds);
  const auto output = gltf::EvaluatePrimitive(data.primitives[0], {}, palette);
  ASSERT_EQ(output.size(), 1u);
  expectVec3(output[0].position, glm::vec3(1, 0, 0));
  expectVec3(output[0].normal, glm::vec3(0, 0, 1));
  EXPECT_EQ(output[0].tangent.w, -1.0f);
  EXPECT_EQ(output[0].color, bindVertex().color);
  EXPECT_EQ(output[0].texCoord, bindVertex().texCoord);
  EXPECT_EQ(output[0].texCoord1, bindVertex().texCoord1);
}

TEST(GltfDeformation, RetainsMissingBaseFrameProvenanceAndNormalTextureUvSelection) {
  SourceFixture fixture;
  fixture.morph();
  fixture.source.materials.emplace_back();
  fixture.primitive().material = 0;
  auto& texture = fixture.source.materials[0].normalTexture;
  texture.index = 0;
  texture.texCoord = 1;
  auto loaded = fixture.load();
  EXPECT_TRUE(loaded.primitives[0].authoredBaseNormal);
  EXPECT_TRUE(loaded.primitives[0].authoredBaseTangent);
  EXPECT_EQ(loaded.primitives[0].normalTextureTexCoord, 1u);
  fixture.primitive().attributes.erase("NORMAL");
  fixture.primitive().attributes.erase("TANGENT");
  texture.extensions["KHR_texture_transform"] = tinygltf::Value(tinygltf::Value::Object{
      {"texCoord", tinygltf::Value(0)}});
  loaded = fixture.load();
  EXPECT_FALSE(loaded.primitives[0].authoredBaseNormal);
  EXPECT_FALSE(loaded.primitives[0].authoredBaseTangent);
  EXPECT_EQ(loaded.primitives[0].normalTextureTexCoord, 0u);
  EXPECT_EQ(loaded.primitives[0].bindVertices.size(), fixture.runtime[0].vertices().size());
  texture.extensions["KHR_texture_transform"].Get<tinygltf::Value::Object>()["texCoord"] = tinygltf::Value(2);
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, NestedJointPaletteCancelsMeshNodeWorldAndAppliesInverseBind) {
  gltf::DeformationSkin skin;
  skin.joints = {2};
  // Rest parent is translated by 3 and the child by 2: joint world x=5.
  skin.inverseBindMatrices = {glm::translate(glm::mat4(1), glm::vec3(-5, 0, 0))};
  const glm::mat4 meshWorld = glm::translate(glm::mat4(1), glm::vec3(10, -4, 2)) *
                              glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4));
  const glm::mat4 parentWorld = glm::translate(glm::mat4(1), glm::vec3(3, 0, 0)) *
                                glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
  const glm::mat4 childWorld = parentWorld * glm::translate(glm::mat4(1), glm::vec3(2, 0, 0));
  const std::array<glm::mat4, 3> worlds{meshWorld, parentWorld, childWorld};
  auto primitive = skinnedPrimitive(bindVertex(glm::vec3(6, 0, 0)));
  const auto palette = gltf::ComputeSkinPalette(skin, meshWorld, worlds);
  const auto output = gltf::EvaluatePrimitive(primitive, {}, palette);
  expectVec3(glm::vec3(meshWorld * glm::vec4(output[0].position, 1)), glm::vec3(3, 3, 0));
}

TEST(GltfDeformation, InverseBindRestPoseLeavesWorldSpaceBindPositionUnchanged) {
  gltf::DeformationSkin skin;
  skin.joints = {1};
  const glm::mat4 jointWorld = glm::translate(glm::mat4(1), glm::vec3(4, 5, 6)) *
                              glm::rotate(glm::mat4(1), glm::radians(37.0f), glm::vec3(0, 1, 0));
  skin.inverseBindMatrices = {glm::inverse(jointWorld)};
  const glm::mat4 meshWorld = glm::translate(glm::mat4(1), glm::vec3(8, -2, 3));
  const std::array<glm::mat4, 2> worlds{meshWorld, jointWorld};
  auto primitive = skinnedPrimitive(bindVertex(glm::vec3(1, 2, 3)));
  const auto output = gltf::EvaluatePrimitive(primitive, {},
      gltf::ComputeSkinPalette(skin, meshWorld, worlds));
  expectVec3(glm::vec3(meshWorld * glm::vec4(output[0].position, 1)), glm::vec3(1, 2, 3));
}

TEST(GltfDeformation, MorphDeltasApplyBeforeSkinning) {
  auto primitive = skinnedPrimitive();
  primitive.morphTargets = {{{glm::vec3(1, 0, 0)}, {}, {}}};
  primitive.meshDefaultWeights = {0.5f};
  const std::array<glm::mat4, 1> palette{glm::scale(glm::mat4(1), glm::vec3(2, 3, 4))};
  const auto output = gltf::EvaluatePrimitive(primitive, {}, palette);
  expectVec3(output[0].position, glm::vec3(3, 0, 0));
}

TEST(GltfDeformation, MorphsNormalsAndTangentsThenNormalizesAndPreservesHandedness) {
  auto primitive = primitiveWithVertex();
  primitive.morphTargets = {{{}, {glm::vec3(0, 1, 0)}, {glm::vec3(1, 0, 1)}}};
  primitive.meshDefaultWeights = {1};
  const auto output = gltf::EvaluatePrimitive(primitive);
  const glm::vec3 normal = glm::normalize(glm::vec3(0, 1, 1));
  const glm::vec3 rawTangent(2, 0, 1);
  const glm::vec3 tangent = glm::normalize(rawTangent - normal * glm::dot(normal, rawTangent));
  expectVec3(output[0].normal, normal);
  expectVec3(glm::vec3(output[0].tangent), tangent);
  EXPECT_FLOAT_EQ(output[0].tangent.w, -1);
  EXPECT_NEAR(glm::dot(output[0].normal, glm::vec3(output[0].tangent)), 0, 1e-6f);
}

TEST(GltfDeformation, NonuniformMirroredSkinUsesInverseTransposeAndChangesTangentSign) {
  Vertex vertex = bindVertex();
  vertex.normal = glm::normalize(glm::vec3(1, 1, 0));
  vertex.tangent = glm::vec4(glm::normalize(glm::vec3(1, -1, 0)), -1);
  auto primitive = skinnedPrimitive(vertex);
  const std::array<glm::mat4, 1> palette{glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4))};
  const auto output = gltf::EvaluatePrimitive(primitive, {}, palette);
  expectVec3(output[0].normal, glm::normalize(glm::vec3(-0.5f, 1.0f / 3.0f, 0)));
  expectVec3(glm::vec3(output[0].tangent), glm::normalize(glm::vec3(-2, -3, 0)));
  EXPECT_FLOAT_EQ(output[0].tangent.w, 1);
}

TEST(GltfDeformation, NormalUsesInverseOfBlendedTransform) {
  Vertex vertex = bindVertex();
  vertex.normal = glm::normalize(glm::vec3(1, 1, 0));
  auto primitive = skinnedPrimitive(vertex);
  primitive.influences[0].values = {{0, 0.25f}, {1, 0.75f}};
  const std::array<glm::mat4, 2> palette{
      glm::scale(glm::mat4(1), glm::vec3(2, 1, 1)),
      glm::scale(glm::mat4(1), glm::vec3(1, 3, 1))};
  const auto output = gltf::EvaluatePrimitive(primitive, {}, palette);
  expectVec3(output[0].normal, glm::normalize(glm::vec3(1.0f / 1.25f, 1.0f / 2.5f, 0)));
}

TEST(GltfDeformation, NormalFrameIsIndependentOfAbsoluteSkinScale) {
  Vertex vertex = bindVertex();
  vertex.normal = glm::normalize(glm::vec3(1, 1, 0));
  vertex.tangent = glm::vec4(glm::normalize(glm::vec3(1, -1, 0)), -1);
  const auto primitive = skinnedPrimitive(vertex);
  for (const float scale : {1.0e-12f, 1.0e12f}) {
    const std::array<glm::mat4, 1> palette{
        glm::scale(glm::mat4(1), glm::vec3(-2, 3, 4) * scale)};
    const auto output = gltf::EvaluatePrimitive(primitive, {}, palette);
    expectVec3(output[0].normal, glm::normalize(glm::vec3(-0.5f, 1.0f / 3.0f, 0)));
    expectVec3(glm::vec3(output[0].tangent), glm::normalize(glm::vec3(-2, -3, 0)));
    EXPECT_FLOAT_EQ(output[0].tangent.w, 1);
  }
}

TEST(GltfDeformation, CollapsedSkinBlendProducesFiniteOrthonormalFrame) {
  auto primitive = skinnedPrimitive();
  primitive.influences[0].values = {{0, 0.5f}, {1, 0.5f}};
  const std::array<glm::mat4, 2> palette{
      glm::mat4(1), glm::scale(glm::mat4(1), glm::vec3(-1, -1, -1))};
  const auto output = gltf::EvaluatePrimitive(primitive, {}, palette);
  expectVec3(output[0].position, glm::vec3(0));
  EXPECT_NEAR(glm::length(output[0].normal), 1, 1e-6f);
  EXPECT_NEAR(glm::length(glm::vec3(output[0].tangent)), 1, 1e-6f);
  EXPECT_NEAR(glm::dot(output[0].normal, glm::vec3(output[0].tangent)), 0, 1e-6f);
}

TEST(GltfDeformation, MeshAndNodeDefaultMorphWeightsWorkWithoutAnimation) {
  SourceFixture fixture;
  fixture.morph();
  fixture.source.meshes[0].weights = {0.25};
  fixture.source.nodes.resize(3);
  fixture.source.nodes[1].mesh = 0;
  fixture.source.nodes[1].weights = {-0.5};
  fixture.source.nodes[2].mesh = 0;
  fixture.source.nodes[2].weights = {0};
  const auto data = fixture.load();
  const auto meshDefault = gltf::EvaluatePrimitive(data.primitives[0]);
  const auto nodeDefault = gltf::EvaluatePrimitive(data.primitives[0], data.nodes[0].defaultMorphWeights);
  const auto override = gltf::EvaluatePrimitive(data.primitives[0], data.nodes[1].defaultMorphWeights);
  const auto zero = gltf::EvaluatePrimitive(data.primitives[0], data.nodes[2].defaultMorphWeights);
  expectVec3(meshDefault[0].position, glm::vec3(1.5f, 0, 0));
  expectVec3(nodeDefault[0].position, glm::vec3(1.5f, 0, 0));
  expectVec3(override[0].position, glm::vec3(0));
  expectVec3(zero[0].position, glm::vec3(1, 0, 0));
}

TEST(GltfDeformation, MissingMorphWeightsResolveToZero) {
  SourceFixture fixture;
  fixture.morph();
  const auto data = fixture.load();
  ASSERT_EQ(data.nodes[0].defaultMorphWeights.size(), 1u);
  EXPECT_FLOAT_EQ(data.nodes[0].defaultMorphWeights[0], 0);
  expectVec3(gltf::EvaluatePrimitive(data.primitives[0])[0].position, glm::vec3(1, 0, 0));
}

TEST(GltfDeformation, ExtractsNormalAndTangentDeltasAsVec3) {
  SourceFixture fixture;
  fixture.morph();
  fixture.primitive().targets[0]["NORMAL"] = fixture.accessor(
      std::vector<float>{0, 1, 0}, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
  fixture.primitive().targets[0]["TANGENT"] = fixture.accessor(
      std::vector<float>{1, 0, 1}, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
  fixture.source.meshes[0].weights = {1};
  const auto data = fixture.load();
  const auto output = gltf::EvaluatePrimitive(data.primitives[0]);
  expectVec3(output[0].normal, glm::normalize(glm::vec3(0, 1, 1)));
  expectVec3(glm::vec3(output[0].tangent), glm::normalize(glm::vec3(2, -0.5f, 0.5f)));
  EXPECT_FLOAT_EQ(output[0].tangent.w, -1);
}

TEST(GltfDeformation, SharedMeshInstancesUseIndependentSkinsAndMorphWeights) {
  SourceFixture fixture;
  fixture.skin();
  fixture.morph();
  fixture.source.nodes.resize(4);
  fixture.source.nodes[3].mesh = 0;
  fixture.source.nodes[3].skin = 1;
  fixture.source.nodes[3].weights = {1};
  tinygltf::Skin secondSkin;
  secondSkin.joints = {2};
  fixture.source.skins.push_back(secondSkin);
  const auto data = fixture.load();
  const std::array<glm::mat4, 4> worlds{
      glm::mat4(1), glm::translate(glm::mat4(1), glm::vec3(0, 2, 0)),
      glm::translate(glm::mat4(1), glm::vec3(0, -3, 0)), glm::mat4(1)};
  const auto first = gltf::EvaluatePrimitive(data.primitives[0], data.nodes[0].defaultMorphWeights,
      gltf::ComputeSkinPalette(data.skins[data.nodes[0].skin], worlds[0], worlds));
  const auto second = gltf::EvaluatePrimitive(data.primitives[0], data.nodes[3].defaultMorphWeights,
      gltf::ComputeSkinPalette(data.skins[data.nodes[3].skin], worlds[3], worlds));
  expectVec3(first[0].position, glm::vec3(1, 2, 0));
  expectVec3(second[0].position, glm::vec3(3, -3, 0));
  expectVec3(data.primitives[0].bindVertices[0].position, glm::vec3(1, 0, 0));
}

TEST(GltfDeformation, UnskinnedInstanceIgnoresJointAttributes) {
  auto primitive = skinnedPrimitive();
  expectVec3(gltf::EvaluatePrimitive(primitive)[0].position, glm::vec3(1, 0, 0));
}

TEST(GltfDeformation, RetainsAndNormalizesAllInfluenceSets) {
  SourceFixture fixture;
  fixture.skin({0, 1, 0, 0}, {0.25f, 0.25f, 0, 0}, {1, 2, 3});
  fixture.primitive().attributes["JOINTS_1"] = fixture.accessor(
      std::vector<uint16_t>{2, 0, 0, 0}, TINYGLTF_TYPE_VEC4,
      TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
  fixture.primitive().attributes["WEIGHTS_1"] = fixture.accessor(
      std::vector<uint8_t>{255, 0, 0, 0}, TINYGLTF_TYPE_VEC4,
      TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, true);
  const auto data = fixture.load();
  const auto& influences = data.primitives[0].influences[0].values;
  ASSERT_EQ(influences.size(), 8u);
  EXPECT_EQ(influences[4].joint, 2u);
  EXPECT_NEAR(influences[0].weight, 1.0f / 6.0f, 1e-6f);
  EXPECT_NEAR(influences[1].weight, 1.0f / 6.0f, 1e-6f);
  EXPECT_NEAR(influences[4].weight, 2.0f / 3.0f, 1e-6f);
  const std::array<glm::mat4, 3> palette{
      glm::translate(glm::mat4(1), glm::vec3(6, 0, 0)),
      glm::translate(glm::mat4(1), glm::vec3(0, 6, 0)),
      glm::translate(glm::mat4(1), glm::vec3(0, 0, 6))};
  expectVec3(gltf::EvaluatePrimitive(data.primitives[0], {}, palette)[0].position,
             glm::vec3(2, 1, 4));
}

TEST(GltfDeformation, ReadsNormalizedUnsignedShortWeightsAndInterleavedAttributes) {
  SourceFixture fixture(2);
  fixture.skin({0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0});
  const int weights = fixture.accessor(
      std::vector<uint16_t>{65535, 0, 0, 0, 123, 456, 789, 321,
                              0, 65535, 0, 0},
      TINYGLTF_TYPE_VEC4, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, true);
  fixture.source.accessors[weights].count = 2;
  fixture.source.bufferViews[fixture.source.accessors[weights].bufferView].byteStride = 16;
  fixture.primitive().attributes["WEIGHTS_0"] = weights;
  const auto data = fixture.load();
  EXPECT_FLOAT_EQ(data.primitives[0].influences[0].values[0].weight, 1);
  EXPECT_FLOAT_EQ(data.primitives[0].influences[1].values[1].weight, 1);
}

TEST(GltfDeformation, SparseMorphDeltasSupportImplicitZeroBase) {
  SourceFixture fixture(2);
  fixture.morph();
  const int index = fixture.primitive().targets[0]["POSITION"];
  auto& accessor = fixture.source.accessors[index];
  accessor.bufferView = -1;
  accessor.sparse.isSparse = true;
  accessor.sparse.count = 1;
  accessor.sparse.indices.bufferView = fixture.view(std::vector<uint8_t>{1});
  accessor.sparse.indices.byteOffset = 0;
  accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  accessor.sparse.values.bufferView = fixture.view(std::vector<float>{0, 3, 0});
  accessor.sparse.values.byteOffset = 0;
  fixture.source.meshes[0].weights = {0.5};
  const auto data = fixture.load();
  const auto output = gltf::EvaluatePrimitive(data.primitives[0]);
  expectVec3(output[0].position, glm::vec3(1, 0, 0));
  expectVec3(output[1].position, glm::vec3(1, 1.5f, 0));
}

TEST(GltfDeformation, RejectsDuplicateOrOutOfRangeSparseIndices) {
  for (const std::vector<uint8_t> indices : {std::vector<uint8_t>{1, 1},
                                            std::vector<uint8_t>{0, 2}}) {
    SourceFixture fixture(2);
    fixture.morph();
    const int index = fixture.primitive().targets[0]["POSITION"];
    auto& accessor = fixture.source.accessors[index];
    accessor.bufferView = -1;
    accessor.sparse.isSparse = true;
    accessor.sparse.count = 2;
    accessor.sparse.indices.bufferView = fixture.view(indices);
    accessor.sparse.indices.byteOffset = 0;
    accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    accessor.sparse.values.bufferView = fixture.view(std::vector<float>{0, 3, 0, 0, 4, 0});
    accessor.sparse.values.byteOffset = 0;
    EXPECT_THROW(fixture.load(), std::runtime_error);
  }
}

TEST(GltfDeformation, PaletteSupportsVerySmallInvertibleMeshNodeScale) {
  gltf::DeformationSkin skin;
  skin.joints = {0};
  skin.inverseBindMatrices = {glm::mat4(1)};
  const glm::mat4 meshWorld = glm::scale(glm::mat4(1), glm::vec3(1.0e-20f));
  const std::array<glm::mat4, 1> worlds{meshWorld};
  const auto palette = gltf::ComputeSkinPalette(skin, meshWorld, worlds);
  expectVec3(glm::vec3(palette[0][0]), glm::vec3(1, 0, 0));
  expectVec3(glm::vec3(palette[0][1]), glm::vec3(0, 1, 0));
  expectVec3(glm::vec3(palette[0][2]), glm::vec3(0, 0, 1));
}

TEST(GltfDeformation, FlattensSourcePrimitiveMappingInLoaderOrder) {
  SourceFixture fixture;
  fixture.source.meshes.push_back(fixture.source.meshes[0]);
  fixture.source.meshes[0].primitives.push_back(fixture.source.meshes[0].primitives[0]);
  fixture.runtime.push_back(fixture.runtime[0]);
  fixture.runtime.push_back(fixture.runtime[0]);
  const auto data = fixture.load();
  ASSERT_EQ(data.primitives.size(), 3u);
  EXPECT_EQ(data.primitives[0].sourceMesh, 0u);
  EXPECT_EQ(data.primitives[0].sourcePrimitive, 0u);
  EXPECT_EQ(data.primitives[1].sourceMesh, 0u);
  EXPECT_EQ(data.primitives[1].sourcePrimitive, 1u);
  EXPECT_EQ(data.primitives[2].sourceMesh, 1u);
  EXPECT_EQ(data.primitives[2].sourcePrimitive, 0u);
}

TEST(GltfDeformation, ExtractsColumnMajorInverseBindsAndAllowsTrailingMatrices) {
  SourceFixture fixture;
  fixture.skin();
  std::vector<float> matrices{
      1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -2, -3, -4, 1,
      1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};
  fixture.source.skins[0].inverseBindMatrices = fixture.accessor(
      matrices, TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
  const auto data = fixture.load();
  ASSERT_EQ(data.skins[0].inverseBindMatrices.size(), 1u);
  expectVec3(glm::vec3(data.skins[0].inverseBindMatrices[0][3]), glm::vec3(-2, -3, -4));
}

TEST(GltfDeformation, RejectsUnpairedAndNonconsecutiveInfluenceSets) {
  SourceFixture fixture;
  fixture.skin();
  fixture.primitive().attributes.erase("WEIGHTS_0");
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.primitive().attributes["WEIGHTS_2"] = fixture.accessor(
      std::vector<float>{1, 0, 0, 0}, TINYGLTF_TYPE_VEC4,
      TINYGLTF_COMPONENT_TYPE_FLOAT);
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsInvalidJointAndWeightAccessorTypes) {
  SourceFixture fixture;
  fixture.skin();
  const int joints = fixture.primitive().attributes["JOINTS_0"];
  fixture.source.accessors[joints].normalized = true;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.accessors[joints].normalized = false;
  const int weights = fixture.primitive().attributes["WEIGHTS_0"];
  fixture.source.accessors[weights].componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsNegativeNonfiniteAndZeroSumSkinWeights) {
  for (const float value : {-0.1f, std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), 0.0f}) {
    SourceFixture fixture;
    fixture.skin({0, 0, 0, 0}, {value, 0, 0, 0});
    EXPECT_THROW(fixture.load(), std::runtime_error);
  }
}

TEST(GltfDeformation, RejectsOutOfRangeJointIncludingZeroWeightEntries) {
  SourceFixture fixture;
  fixture.skin({0, 1, 0, 0}, {1, 0, 0, 0});
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsMismatchedVertexCountsAndRuntimeMappings) {
  SourceFixture fixture;
  fixture.skin();
  const int joints = fixture.primitive().attributes["JOINTS_0"];
  fixture.source.accessors[joints].count = 2;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.accessors[joints].count = 1;
  fixture.runtime.clear();
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsTruncatedOverflowingAndInvalidStridedAccessors) {
  SourceFixture fixture;
  fixture.skin();
  const int index = fixture.primitive().attributes["WEIGHTS_0"];
  auto& accessor = fixture.source.accessors[index];
  auto& view = fixture.source.bufferViews[accessor.bufferView];
  const size_t originalLength = view.byteLength;
  view.byteLength = 15;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  view.byteLength = originalLength;
  accessor.byteOffset = std::numeric_limits<size_t>::max();
  EXPECT_THROW(fixture.load(), std::runtime_error);
  accessor.byteOffset = 0;
  view.byteStride = 12;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  view.byteStride = 18;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  view.byteStride = 256;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  view.byteStride = 0;
  view.byteOffset = std::numeric_limits<size_t>::max();
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsInvalidMorphCountsTypesAndWeights) {
  SourceFixture fixture;
  fixture.morph();
  const int index = fixture.primitive().targets[0]["POSITION"];
  fixture.source.accessors[index].type = TINYGLTF_TYPE_VEC4;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.accessors[index].type = TINYGLTF_TYPE_VEC3;
  fixture.source.accessors[index].count = 2;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.accessors[index].count = 1;
  fixture.source.meshes[0].weights = {0, 1};
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.meshes[0].weights = {std::numeric_limits<double>::infinity()};
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsNonfiniteAndNonaffineInverseBindMatrices) {
  SourceFixture fixture;
  fixture.skin();
  std::vector<float> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2};
  fixture.source.skins[0].inverseBindMatrices = fixture.accessor(
      matrix, TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
  EXPECT_THROW(fixture.load(), std::runtime_error);
  matrix[15] = 1;
  matrix[1] = std::numeric_limits<float>::quiet_NaN();
  fixture.source.skins[0].inverseBindMatrices = fixture.accessor(
      matrix, TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsInvalidSkinAndNodeReferences) {
  SourceFixture fixture;
  fixture.skin();
  fixture.source.skins[0].joints[0] = 99;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.skins[0].joints[0] = 1;
  fixture.source.nodes[0].skin = 99;
  EXPECT_THROW(fixture.load(), std::runtime_error);
  fixture.source.nodes[0].skin = 0;
  fixture.source.nodes[0].mesh = -1;
  EXPECT_THROW(fixture.load(), std::runtime_error);
}

TEST(GltfDeformation, RejectsSingularOrNonfiniteMeshWorldAndMissingJointWorld) {
  gltf::DeformationSkin skin;
  skin.joints = {0};
  skin.inverseBindMatrices = {glm::mat4(1)};
  const std::array<glm::mat4, 1> worlds{glm::mat4(1)};
  EXPECT_THROW(gltf::ComputeSkinPalette(skin, glm::mat4(0), worlds), std::runtime_error);
  glm::mat4 nonfinite(1);
  nonfinite[0][0] = std::numeric_limits<float>::infinity();
  EXPECT_THROW(gltf::ComputeSkinPalette(skin, nonfinite, worlds), std::runtime_error);
  EXPECT_THROW(gltf::ComputeSkinPalette(skin, glm::mat4(1), {}), std::runtime_error);
}

TEST(GltfDeformation, EvaluatorRejectsInvalidPubliclyAuthoredData) {
  auto primitive = skinnedPrimitive();
  const std::array<glm::mat4, 1> palette{glm::mat4(1)};
  primitive.influences[0].values[0].joint = 4;
  EXPECT_THROW(gltf::EvaluatePrimitive(primitive, {}, palette), std::runtime_error);
  primitive.influences[0].values[0].joint = 0;
  primitive.influences[0].values[0].weight = 0;
  EXPECT_THROW(gltf::EvaluatePrimitive(primitive, {}, palette), std::runtime_error);
  primitive.morphTargets = {{{glm::vec3(1, 0, 0)}, {}, {}}};
  const std::array<float, 1> weights{std::numeric_limits<float>::quiet_NaN()};
  EXPECT_THROW(gltf::EvaluatePrimitive(primitive, weights), std::runtime_error);
}

}  // namespace
