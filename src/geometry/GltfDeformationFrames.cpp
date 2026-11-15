#include <Container/geometry/GltfDeformationFrames.h>

#include "GltfDeformationInternal.h"

#include <mikktspace_bounded.h>

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

glm::vec2 normalTexCoord(const Vertex& vertex, uint32_t texCoord) {
  return texCoord == 0 ? vertex.texCoord : vertex.texCoord1;
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
  const auto value = normalTexCoord(mikkVertex(context, face, corner), mikkData(context).texCoord);
  result[0] = value.x; result[1] = value.y;
}

void mikkTangent(const SMikkTSpaceContext* context, const float tangent[3],
                 float sign, int face, int corner) {
  auto& vertex = mikkVertex(context, face, corner);
  vertex.tangent = glm::vec4(tangentFor(vertex.normal, {tangent[0], tangent[1], tangent[2]}),
                            std::isfinite(sign) && sign < 0 ? -1.0f : 1.0f);
}

void generateTriangleTangents(std::vector<Vertex>& vertices, uint32_t texCoord) {
  for (size_t i = 0; i < vertices.size(); i += 3) {
    const glm::dvec3 e1 = glm::dvec3(vertices[i + 1].position) - glm::dvec3(vertices[i].position);
    const glm::dvec3 e2 = glm::dvec3(vertices[i + 2].position) - glm::dvec3(vertices[i].position);
    const glm::dvec2 uv(normalTexCoord(vertices[i], texCoord));
    const glm::dvec2 duv1 = glm::dvec2(normalTexCoord(vertices[i + 1], texCoord)) - uv;
    const glm::dvec2 duv2 = glm::dvec2(normalTexCoord(vertices[i + 2], texCoord)) - uv;
    const double det = duv1.x * duv2.y - duv1.y * duv2.x;
    const auto area = glm::cross(e1, e2);
    const bool hasDerivatives = det != 0.0 && glm::dot(area, area) > 0.0;
    const double uvSign = det < 0.0 ? -1.0 : 1.0;
    // Only the directions of dP/du and dP/dv matter. Keeping just the sign
    // of the denominator avoids amplifying the derivatives for tiny UVs.
    const auto du = (e1 * duv2.y - e2 * duv1.y) * uvSign;
    const auto dv = (e2 * duv1.x - e1 * duv2.x) * uvSign;
    for (size_t corner = 0; corner < 3; ++corner) {
      auto& vertex = vertices[i + corner];
      if (hasDerivatives) {
        const glm::dvec3 n(vertex.normal);
        const auto t = unitOr(glm::cross(glm::cross(n, du), n), glm::vec3(0));
        if (t != glm::vec3(0)) {
          const double orientation = glm::dot(glm::cross(n, glm::dvec3(t)), dv);
          vertex.tangent = glm::vec4(t, orientation < 0.0 ? -1.0f : 1.0f);
          continue;
        }
      }
      vertex.tangent = glm::vec4(tangentFor(vertex.normal, glm::vec3(0)), 1);
    }
  }
}

void generateTangents(std::vector<Vertex>& vertices, uint32_t texCoord,
                      DeformationTangentBudget* budget) {
  MikkContext data{vertices, texCoord};
  SMikkTSpaceInterface callbacks{};
  callbacks.m_getNumFaces = mikkFaceCount;
  callbacks.m_getNumVerticesOfFace = mikkCornerCount;
  callbacks.m_getPosition = mikkPosition;
  callbacks.m_getNormal = mikkNormal;
  callbacks.m_getTexCoord = mikkTexCoord;
  callbacks.m_setTSpaceBasic = mikkTangent;
  SMikkTSpaceContext context{&callbacks, &data};
  if (!budget) {
    // Preserve the original evaluator's result when MikkTSpace fails.
    for (auto& vertex : vertices)
      vertex.tangent = glm::vec4(tangentFor(vertex.normal, glm::vec3(0)), 1);
    (void)genTangSpaceDefault(&context);
    return;
  }

  for (const auto& vertex : vertices)
    if (!finite(normalTexCoord(vertex, texCoord))) fail("normal texture coordinate is non-finite");

  const auto result = genTangSpaceDefaultBounded(&context, &budget->workRemaining,
                                                 budget->maxMikkScratchBytes);
  if (result == MIKKTS_BOUNDED_SUCCESS) return;
  if (result == MIKKTS_BOUNDED_INVALID_INPUT) fail("invalid input to bounded MikkTSpace");
  if (!budget->allowTriangleFallback) {
    if (result == MIKKTS_BOUNDED_WORK_LIMIT) fail("MikkTSpace work budget exceeded");
    if (result == MIKKTS_BOUNDED_MEMORY_LIMIT) fail("MikkTSpace scratch budget exceeded");
    fail("cannot allocate MikkTSpace scratch");
  }

  // A bounded failure emits no tangents. Use the current posed geometry and
  // the selected UVs, without a second attempt at welding or any scratch.
  generateTriangleTangents(vertices, texCoord);
  budget->usedTriangleFallback = true;
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
    if (regenerateTangents && !finite(normalTexCoord(vertex, policy.normalTextureTexCoord)))
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
    std::span<const glm::mat4> skinPalette,
    DeformationTangentBudget* tangentBudget) {
  validatePolicy(prepared.policy);
  const auto count = prepared.primitive.bindVertices.size();
  if (!count || count % 3 || prepared.indices.size() != count || prepared.sourceVertexMap.size() != count ||
      count / 3 > size_t(std::numeric_limits<int>::max()))
    fail("prepared triangle corners are inconsistent");
  for (size_t i = 0; i < count; ++i)
    if (prepared.indices[i] != i) fail("prepared indices must retain independent triangle corners");
  if (prepared.policy.authoredBaseNormal && prepared.policy.authoredBaseTangent)
    return EvaluatePrimitive(prepared.primitive, morphWeights, skinPalette);

  auto vertices = EvaluatePrimitive(prepared.primitive, morphWeights);
  if (!prepared.policy.authoredBaseNormal) {
    for (size_t i = 0; i < count; i += 3) {
      const glm::dvec3 a(vertices[i].position);
      const glm::dvec3 b(vertices[i + 1].position);
      const glm::dvec3 c(vertices[i + 2].position);
      const auto normal = unitOr(glm::cross(b - a, c - a), glm::vec3(0, 1, 0));
      for (size_t corner = 0; corner < 3; ++corner) vertices[i + corner].normal = normal;
    }
  }
  generateTangents(vertices, prepared.policy.normalTextureTexCoord, tangentBudget);
  detail::FinalizeDeformedFrames(vertices, prepared.primitive.influences, skinPalette);
  return vertices;
}

}  // namespace container::geometry::gltf
