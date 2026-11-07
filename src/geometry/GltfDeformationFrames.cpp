#include <Container/geometry/GltfDeformationFrames.h>

extern "C" {
#include <mikktspace.h>
}

#include <glm/geometric.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace container::geometry::gltf {
namespace {

[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("glTF deformation frames: ") + message);
}

bool finite(glm::vec2 value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

glm::vec3 unitOr(glm::dvec3 value, glm::vec3 fallback) {
  const double magnitude = glm::length(value);
  if (!std::isfinite(magnitude) || !(magnitude > 0)) return fallback;
  return glm::vec3(value / magnitude);
}

glm::vec3 tangentFor(glm::vec3 normal, glm::vec3 tangent) {
  const glm::dvec3 n(normal);
  const glm::dvec3 axis = std::abs(normal.x) < 0.9f ? glm::dvec3(1, 0, 0)
                                                  : glm::dvec3(0, 1, 0);
  const auto fallback = unitOr(axis - n * glm::dot(n, axis), glm::vec3(0, 0, 1));
  return unitOr(glm::dvec3(tangent) - n * glm::dot(n, glm::dvec3(tangent)), fallback);
}

void validatePolicy(DeformationFramePolicy policy) {
  if (policy.normalTextureTexCoord > 1) fail("normal texture requires unsupported UV set");
}

struct MikkContext {
  std::vector<Vertex>& vertices;
  uint32_t texCoord;
};

MikkContext& mikkData(const SMikkTSpaceContext* context) {
  return *static_cast<MikkContext*>(context->m_pUserData);
}

Vertex& mikkVertex(const SMikkTSpaceContext* context, int face, int corner) {
  return mikkData(context).vertices[size_t(face) * 3 + size_t(corner)];
}

int mikkFaceCount(const SMikkTSpaceContext* context) {
  return static_cast<int>(mikkData(context).vertices.size() / 3);
}

int mikkCornerCount(const SMikkTSpaceContext*, int) { return 3; }

void mikkPosition(const SMikkTSpaceContext* context, float result[3], int face, int corner) {
  const auto value = mikkVertex(context, face, corner).position;
  for (int i = 0; i < 3; ++i) result[i] = value[i];
}

void mikkNormal(const SMikkTSpaceContext* context, float result[3], int face, int corner) {
  const auto value = mikkVertex(context, face, corner).normal;
  for (int i = 0; i < 3; ++i) result[i] = value[i];
}

void mikkTexCoord(const SMikkTSpaceContext* context, float result[2], int face, int corner) {
  const auto& vertex = mikkVertex(context, face, corner);
  const auto value = mikkData(context).texCoord == 0 ? vertex.texCoord : vertex.texCoord1;
  result[0] = value.x; result[1] = value.y;
}

void mikkTangent(const SMikkTSpaceContext* context, const float tangent[3],
                 float sign, int face, int corner) {
  auto& vertex = mikkVertex(context, face, corner);
  vertex.tangent = glm::vec4(tangentFor(vertex.normal, {tangent[0], tangent[1], tangent[2]}),
                            std::isfinite(sign) && sign < 0 ? -1.0f : 1.0f);
}

void generateTangents(std::vector<Vertex>& vertices, uint32_t texCoord) {
  // A finite deterministic frame also covers degenerate positions/UVs for
  // which MikkTSpace cannot define a tangent or does not write a corner.
  for (auto& vertex : vertices)
    vertex.tangent = glm::vec4(tangentFor(vertex.normal, glm::vec3(0)), 1);
  MikkContext data{vertices, texCoord};
  SMikkTSpaceInterface callbacks{};
  callbacks.m_getNumFaces = mikkFaceCount;
  callbacks.m_getNumVerticesOfFace = mikkCornerCount;
  callbacks.m_getPosition = mikkPosition;
  callbacks.m_getNormal = mikkNormal;
  callbacks.m_getTexCoord = mikkTexCoord;
  callbacks.m_setTSpaceBasic = mikkTangent;
  SMikkTSpaceContext context{&callbacks, &data};
  (void)genTangSpaceDefault(&context);
}

}  // namespace

PreparedDeformationFrames PrepareDeformationFrames(
    const DeformationPrimitive& primitive,
    std::span<const uint32_t> localTriangleIndices,
    DeformationFramePolicy policy) {
  validatePolicy(policy);
  const auto count = primitive.bindVertices.size();
  if (!count || localTriangleIndices.empty() || localTriangleIndices.size() % 3)
    fail("a nonempty local triangle list is required");
  if (localTriangleIndices.size() > std::numeric_limits<uint32_t>::max() ||
      localTriangleIndices.size() / 3 > size_t(std::numeric_limits<int>::max()))
    fail("triangle corner count exceeds the supported range");
  if (!primitive.influences.empty() && primitive.influences.size() != count)
    fail("influence count differs from bind vertex count");
  for (const auto& target : primitive.morphTargets)
    for (const auto* values : {&target.positions, &target.normals, &target.tangents})
      if (!values->empty() && values->size() != count)
        fail("morph delta count differs from bind vertex count");

  PreparedDeformationFrames result;
  result.policy = policy;
  result.primitive.sourceMesh = primitive.sourceMesh;
  result.primitive.sourcePrimitive = primitive.sourcePrimitive;
  result.primitive.meshDefaultWeights = primitive.meshDefaultWeights;
  result.primitive.authoredBaseNormal = policy.authoredBaseNormal;
  result.primitive.authoredBaseTangent = policy.authoredBaseTangent;
  result.primitive.normalTextureTexCoord = policy.normalTextureTexCoord;
  result.primitive.morphTargets.resize(primitive.morphTargets.size());
  result.sourceVertexMap.assign(localTriangleIndices.begin(), localTriangleIndices.end());
  result.indices.reserve(localTriangleIndices.size());
  result.primitive.bindVertices.reserve(localTriangleIndices.size());
  if (!primitive.influences.empty()) result.primitive.influences.reserve(localTriangleIndices.size());
  const bool regenerateTangents = !policy.authoredBaseNormal || !policy.authoredBaseTangent;
  for (const auto source : localTriangleIndices) {
    if (source >= count) fail("local triangle index exceeds bind vertex count");
    auto vertex = primitive.bindVertices[source];
    if (!finite(vertex.position)) fail("bind position is non-finite");
    if (regenerateTangents && !finite(policy.normalTextureTexCoord == 0 ? vertex.texCoord : vertex.texCoord1))
      fail("normal texture coordinate is non-finite");
    if (!policy.authoredBaseNormal) vertex.normal = {0, 1, 0};
    if (regenerateTangents) vertex.tangent = {1, 0, 0, 1};
    result.indices.push_back(static_cast<uint32_t>(result.primitive.bindVertices.size()));
    result.primitive.bindVertices.push_back(vertex);
    if (!primitive.influences.empty()) result.primitive.influences.push_back(primitive.influences[source]);
    for (size_t target = 0; target < primitive.morphTargets.size(); ++target) {
      const auto& input = primitive.morphTargets[target];
      auto& output = result.primitive.morphTargets[target];
      if (!input.positions.empty()) output.positions.push_back(input.positions[source]);
      if (policy.authoredBaseNormal && !input.normals.empty()) output.normals.push_back(input.normals[source]);
      if (!regenerateTangents && !input.tangents.empty()) output.tangents.push_back(input.tangents[source]);
    }
  }
  // Validate resolved weights and every retained morph/frame attribute using
  // the same numerical contract that the ordinary CPU/GPU path implements.
  (void)EvaluatePrimitive(result.primitive);
  return result;
}

std::vector<Vertex> EvaluateDeformationFrames(
    const PreparedDeformationFrames& prepared,
    std::span<const float> morphWeights,
    std::span<const glm::mat4> skinPalette) {
  validatePolicy(prepared.policy);
  const auto count = prepared.primitive.bindVertices.size();
  if (!count || count % 3 || prepared.indices.size() != count || prepared.sourceVertexMap.size() != count ||
      count / 3 > size_t(std::numeric_limits<int>::max()))
    fail("prepared triangle corners are inconsistent");
  for (size_t i = 0; i < count; ++i)
    if (prepared.indices[i] != i) fail("prepared indices must retain independent triangle corners");
  if (prepared.policy.authoredBaseNormal && prepared.policy.authoredBaseTangent)
    return EvaluatePrimitive(prepared.primitive, morphWeights, skinPalette);

  DeformationPrimitive pose;
  pose.bindVertices = EvaluatePrimitive(prepared.primitive, morphWeights);
  pose.influences = prepared.primitive.influences;
  if (!prepared.policy.authoredBaseNormal) {
    for (size_t i = 0; i < count; i += 3) {
      const glm::dvec3 a(pose.bindVertices[i].position);
      const glm::dvec3 b(pose.bindVertices[i + 1].position);
      const glm::dvec3 c(pose.bindVertices[i + 2].position);
      const auto normal = unitOr(glm::cross(b - a, c - a), glm::vec3(0, 1, 0));
      for (size_t corner = 0; corner < 3; ++corner) pose.bindVertices[i + corner].normal = normal;
    }
  }
  generateTangents(pose.bindVertices, prepared.policy.normalTextureTexCoord);
  return EvaluatePrimitive(pose, {}, skinPalette);
}

}  // namespace container::geometry::gltf
