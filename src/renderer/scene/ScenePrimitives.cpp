#include "Container/renderer/scene/ScenePrimitives.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

namespace container::renderer {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

[[nodiscard]] glm::vec3 normalizeOr(const glm::vec3 &value,
                                    const glm::vec3 &fallback) {
  const float lengthSq = glm::dot(value, value);
  if (!std::isfinite(lengthSq) || lengthSq <= 1.0e-10f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(lengthSq));
}

[[nodiscard]] float positiveOr(float value, float fallback) {
  return std::isfinite(value) && value > 0.0f ? value : fallback;
}

[[nodiscard]] uint32_t atLeast(uint32_t value, uint32_t minimum) {
  return std::max(value, minimum);
}

[[nodiscard]] glm::vec3 perpendicularFallback(const glm::vec3 &axis) {
  const glm::vec3 reference = std::abs(axis.z) < 0.999f
                                  ? glm::vec3(0.0f, 0.0f, 1.0f)
                                  : glm::vec3(0.0f, 1.0f, 0.0f);
  return normalizeOr(glm::cross(reference, axis), glm::vec3(0.0f, 1.0f, 0.0f));
}

[[nodiscard]] container::geometry::Vertex makePrimitiveVertex(
    const glm::vec3 &position, const glm::vec2 &texCoord,
    const glm::vec3 &normal, const glm::vec3 &tangent) {
  const glm::vec3 safeNormal =
      normalizeOr(normal, glm::vec3(0.0f, 0.0f, 1.0f));
  glm::vec3 safeTangent =
      tangent - safeNormal * glm::dot(safeNormal, tangent);
  safeTangent = normalizeOr(safeTangent, perpendicularFallback(safeNormal));

  container::geometry::Vertex vertex{};
  vertex.position = position;
  vertex.color = glm::vec3(1.0f);
  vertex.texCoord = texCoord;
  vertex.texCoord1 = texCoord;
  vertex.normal = safeNormal;
  vertex.tangent = glm::vec4(safeTangent, 1.0f);
  return vertex;
}

void appendQuadFace(std::vector<container::geometry::Vertex> &vertices,
                    std::vector<uint32_t> &indices,
                    const std::array<glm::vec3, 4> &positions,
                    const glm::vec3 &normal,
                    const glm::vec3 &tangent) {
  const uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
  vertices.push_back(makePrimitiveVertex(positions[0], {0.0f, 0.0f}, normal,
                                         tangent));
  vertices.push_back(makePrimitiveVertex(positions[1], {1.0f, 0.0f}, normal,
                                         tangent));
  vertices.push_back(makePrimitiveVertex(positions[2], {1.0f, 1.0f}, normal,
                                         tangent));
  vertices.push_back(makePrimitiveVertex(positions[3], {0.0f, 1.0f}, normal,
                                         tangent));

  indices.insert(indices.end(),
                 {baseIndex, baseIndex + 1u, baseIndex + 2u, baseIndex,
                  baseIndex + 2u, baseIndex + 3u});
}

void appendTriangleFace(std::vector<container::geometry::Vertex> &vertices,
                        std::vector<uint32_t> &indices,
                        std::array<glm::vec3, 3> positions,
                        const glm::vec3 &expectedNormal,
                        const glm::vec3 &tangent) {
  glm::vec3 faceNormal =
      normalizeOr(glm::cross(positions[1] - positions[0],
                             positions[2] - positions[0]),
                  expectedNormal);
  if (glm::dot(faceNormal, expectedNormal) < 0.0f) {
    std::swap(positions[1], positions[2]);
    faceNormal =
        normalizeOr(glm::cross(positions[1] - positions[0],
                               positions[2] - positions[0]),
                    expectedNormal);
  }

  const uint32_t baseIndex = static_cast<uint32_t>(vertices.size());
  vertices.push_back(makePrimitiveVertex(positions[0], {0.5f, 0.0f},
                                         faceNormal, tangent));
  vertices.push_back(makePrimitiveVertex(positions[1], {1.0f, 1.0f},
                                         faceNormal, tangent));
  vertices.push_back(makePrimitiveVertex(positions[2], {0.0f, 1.0f},
                                         faceNormal, tangent));
  indices.insert(indices.end(), {baseIndex, baseIndex + 1u, baseIndex + 2u});
}

[[nodiscard]] container::geometry::Mesh makePlaneMesh(
    const ScenePrimitivePlaneDesc &desc) {
  const glm::vec3 uAxis =
      normalizeOr(desc.uAxis, glm::vec3(1.0f, 0.0f, 0.0f));
  glm::vec3 vAxis = desc.vAxis - uAxis * glm::dot(uAxis, desc.vAxis);
  vAxis = normalizeOr(vAxis, perpendicularFallback(uAxis));

  const float halfWidth = positiveOr(desc.width, 1.0f) * 0.5f;
  const float halfHeight = positiveOr(desc.height, 1.0f) * 0.5f;
  const glm::vec3 halfU = uAxis * halfWidth;
  const glm::vec3 halfV = vAxis * halfHeight;
  const glm::vec3 normal =
      normalizeOr(glm::cross(uAxis, vAxis), glm::vec3(0.0f, 0.0f, 1.0f));

  std::vector<container::geometry::Vertex> vertices;
  vertices.reserve(4u);
  std::vector<uint32_t> indices;
  indices.reserve(6u);
  appendQuadFace(vertices, indices,
                 {{
                     desc.center - halfU - halfV,
                     desc.center + halfU - halfV,
                     desc.center + halfU + halfV,
                     desc.center - halfU + halfV,
                 }},
                 normal, uAxis);
  return {std::move(vertices), std::move(indices), desc.materialIndex,
          desc.disableBackfaceCulling};
}

[[nodiscard]] container::geometry::Vertex makeSphereVertex(
    float theta, float phi, float radius, const glm::vec2 &uv) {
  const float sinTheta = std::sin(theta);
  const glm::vec3 normal =
      normalizeOr({sinTheta * std::cos(phi), std::cos(theta),
                   sinTheta * std::sin(phi)},
                  glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::vec3 tangent =
      normalizeOr({-std::sin(phi), 0.0f, std::cos(phi)},
                  glm::vec3(1.0f, 0.0f, 0.0f));
  return makePrimitiveVertex(normal * radius, uv, normal, tangent);
}

[[nodiscard]] container::geometry::Vertex makeTorusVertex(
    float u, float v, float majorRadius, float minorRadius,
    const glm::vec2 &uv) {
  const float cosU = std::cos(u);
  const float sinU = std::sin(u);
  const float cosV = std::cos(v);
  const float sinV = std::sin(v);
  const glm::vec3 normal =
      normalizeOr({cosV * cosU, sinV, cosV * sinU},
                  glm::vec3(1.0f, 0.0f, 0.0f));
  const glm::vec3 position = {
      (majorRadius + minorRadius * cosV) * cosU, minorRadius * sinV,
      (majorRadius + minorRadius * cosV) * sinU};
  const glm::vec3 tangent =
      normalizeOr({-sinU, 0.0f, cosU}, glm::vec3(0.0f, 0.0f, 1.0f));
  return makePrimitiveVertex(position, uv, normal, tangent);
}

} // namespace

std::string_view scenePrimitiveKindLabel(ScenePrimitiveKind kind) {
  switch (kind) {
  case ScenePrimitiveKind::Quad:
    return "Quad";
  case ScenePrimitiveKind::Plane:
    return "Plane (double-sided wall)";
  case ScenePrimitiveKind::Cube:
    return "Cube";
  case ScenePrimitiveKind::Sphere:
    return "Sphere";
  case ScenePrimitiveKind::TriangularPrism:
    return "Triangular Prism";
  case ScenePrimitiveKind::Pyramid:
    return "Pyramid";
  case ScenePrimitiveKind::Torus:
    return "Torus";
  }
  return "Primitive";
}

container::geometry::Mesh makeScenePrimitive(ScenePrimitiveKind kind,
                                             int32_t materialIndex) {
  switch (kind) {
  case ScenePrimitiveKind::Quad:
    return makeScenePrimitiveQuad({.materialIndex = materialIndex});
  case ScenePrimitiveKind::Plane:
    return makeScenePrimitivePlane({.materialIndex = materialIndex});
  case ScenePrimitiveKind::Cube:
    return makeScenePrimitiveCube(1.0f, materialIndex);
  case ScenePrimitiveKind::Sphere:
    return makeScenePrimitiveSphere({.materialIndex = materialIndex});
  case ScenePrimitiveKind::TriangularPrism:
    return makeScenePrimitiveTriangularPrism({.materialIndex = materialIndex});
  case ScenePrimitiveKind::Pyramid:
    return makeScenePrimitivePyramid({.materialIndex = materialIndex});
  case ScenePrimitiveKind::Torus:
    return makeScenePrimitiveTorus({.materialIndex = materialIndex});
  }
  return makeScenePrimitiveCube(1.0f, materialIndex);
}

container::geometry::Mesh makeScenePrimitiveQuad(
    const ScenePrimitivePlaneDesc &desc) {
  ScenePrimitivePlaneDesc quadDesc = desc;
  quadDesc.disableBackfaceCulling = false;
  return makePlaneMesh(quadDesc);
}

container::geometry::Mesh makeScenePrimitivePlane(
    const ScenePrimitivePlaneDesc &desc) {
  ScenePrimitivePlaneDesc wallDesc = desc;
  wallDesc.disableBackfaceCulling = true;
  return makePlaneMesh(wallDesc);
}

container::geometry::Mesh makeScenePrimitiveCube(float size,
                                                 int32_t materialIndex) {
  const float halfSize = positiveOr(size, 1.0f) * 0.5f;
  const float n = -halfSize;
  const float p = halfSize;

  std::vector<container::geometry::Vertex> vertices;
  vertices.reserve(24u);
  std::vector<uint32_t> indices;
  indices.reserve(36u);

  appendQuadFace(vertices, indices,
                 {{{p, n, p}, {p, n, n}, {p, p, n}, {p, p, p}}},
                 {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  appendQuadFace(vertices, indices,
                 {{{n, n, n}, {n, n, p}, {n, p, p}, {n, p, n}}},
                 {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
  appendQuadFace(vertices, indices,
                 {{{n, p, p}, {p, p, p}, {p, p, n}, {n, p, n}}},
                 {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(vertices, indices,
                 {{{n, n, n}, {p, n, n}, {p, n, p}, {n, n, p}}},
                 {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(vertices, indices,
                 {{{n, n, p}, {p, n, p}, {p, p, p}, {n, p, p}}},
                 {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f});
  appendQuadFace(vertices, indices,
                 {{{p, n, n}, {n, n, n}, {n, p, n}, {p, p, n}}},
                 {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f});

  return {std::move(vertices), std::move(indices), materialIndex, false};
}

container::geometry::Mesh makeScenePrimitiveSphere(
    const ScenePrimitiveSphereDesc &desc) {
  const float radius = positiveOr(desc.radius, 0.5f);
  const uint32_t latitudeSegments = atLeast(desc.latitudeSegments, 3u);
  const uint32_t longitudeSegments = atLeast(desc.longitudeSegments, 3u);
  const uint32_t ringVertexCount = longitudeSegments + 1u;
  const uint32_t ringCount = latitudeSegments - 1u;

  std::vector<container::geometry::Vertex> vertices;
  vertices.reserve(2u + ringCount * ringVertexCount);
  std::vector<uint32_t> indices;
  indices.reserve(longitudeSegments * 6u * latitudeSegments);

  const uint32_t topIndex = static_cast<uint32_t>(vertices.size());
  vertices.push_back(makePrimitiveVertex({0.0f, radius, 0.0f}, {0.5f, 0.0f},
                                         {0.0f, 1.0f, 0.0f},
                                         {1.0f, 0.0f, 0.0f}));

  for (uint32_t lat = 1u; lat < latitudeSegments; ++lat) {
    const float v = static_cast<float>(lat) /
                    static_cast<float>(latitudeSegments);
    const float theta = kPi * v;
    for (uint32_t lon = 0u; lon <= longitudeSegments; ++lon) {
      const float u = static_cast<float>(lon) /
                      static_cast<float>(longitudeSegments);
      vertices.push_back(makeSphereVertex(theta, kTwoPi * u, radius, {u, v}));
    }
  }

  const uint32_t bottomIndex = static_cast<uint32_t>(vertices.size());
  vertices.push_back(makePrimitiveVertex({0.0f, -radius, 0.0f},
                                         {0.5f, 1.0f},
                                         {0.0f, -1.0f, 0.0f},
                                         {1.0f, 0.0f, 0.0f}));

  const auto ringStart = [ringVertexCount](uint32_t ring) {
    return 1u + ring * ringVertexCount;
  };

  for (uint32_t lon = 0u; lon < longitudeSegments; ++lon) {
    indices.insert(indices.end(),
                   {topIndex, ringStart(0u) + lon + 1u,
                    ringStart(0u) + lon});
  }

  for (uint32_t ring = 0u; ring + 1u < ringCount; ++ring) {
    const uint32_t current = ringStart(ring);
    const uint32_t next = ringStart(ring + 1u);
    for (uint32_t lon = 0u; lon < longitudeSegments; ++lon) {
      const uint32_t a = current + lon;
      const uint32_t b = current + lon + 1u;
      const uint32_t c = next + lon;
      const uint32_t d = next + lon + 1u;
      indices.insert(indices.end(), {a, b, c, b, d, c});
    }
  }

  const uint32_t lastRing = ringStart(ringCount - 1u);
  for (uint32_t lon = 0u; lon < longitudeSegments; ++lon) {
    indices.insert(indices.end(),
                   {lastRing + lon, lastRing + lon + 1u, bottomIndex});
  }

  return {std::move(vertices), std::move(indices), desc.materialIndex, false};
}

container::geometry::Mesh makeScenePrimitiveTriangularPrism(
    const ScenePrimitivePrismDesc &desc) {
  const float radius = positiveOr(desc.radius, 0.5f);
  const float halfDepth = positiveOr(desc.depth, 1.0f) * 0.5f;
  const float sqrtThreeOverTwo = 0.8660254037844386f;

  const std::array<glm::vec3, 3> front = {{
      {0.0f, radius, halfDepth},
      {-sqrtThreeOverTwo * radius, -0.5f * radius, halfDepth},
      {sqrtThreeOverTwo * radius, -0.5f * radius, halfDepth},
  }};
  const std::array<glm::vec3, 3> back = {{
      {0.0f, radius, -halfDepth},
      {-sqrtThreeOverTwo * radius, -0.5f * radius, -halfDepth},
      {sqrtThreeOverTwo * radius, -0.5f * radius, -halfDepth},
  }};

  std::vector<container::geometry::Vertex> vertices;
  vertices.reserve(18u);
  std::vector<uint32_t> indices;
  indices.reserve(24u);

  appendTriangleFace(vertices, indices, {front[0], front[1], front[2]},
                     {0.0f, 0.0f, 1.0f}, front[2] - front[1]);
  appendTriangleFace(vertices, indices, {back[0], back[2], back[1]},
                     {0.0f, 0.0f, -1.0f}, back[1] - back[2]);

  for (uint32_t edge = 0u; edge < 3u; ++edge) {
    const uint32_t next = (edge + 1u) % 3u;
    const std::array<glm::vec3, 4> positions = {
        {front[edge], front[next], back[next], back[edge]}};
    const glm::vec3 normal =
        normalizeOr(glm::cross(positions[1] - positions[0],
                               positions[2] - positions[0]),
                    glm::vec3(0.0f, 1.0f, 0.0f));
    appendQuadFace(vertices, indices, positions, normal,
                   normalizeOr(front[next] - front[edge],
                               glm::vec3(1.0f, 0.0f, 0.0f)));
  }

  return {std::move(vertices), std::move(indices), desc.materialIndex, false};
}

container::geometry::Mesh makeScenePrimitivePyramid(
    const ScenePrimitivePyramidDesc &desc) {
  const float halfBase = positiveOr(desc.baseSize, 1.0f) * 0.5f;
  const float halfHeight = positiveOr(desc.height, 1.0f) * 0.5f;
  const glm::vec3 apex{0.0f, halfHeight, 0.0f};
  const glm::vec3 leftFront{-halfBase, -halfHeight, halfBase};
  const glm::vec3 rightFront{halfBase, -halfHeight, halfBase};
  const glm::vec3 rightBack{halfBase, -halfHeight, -halfBase};
  const glm::vec3 leftBack{-halfBase, -halfHeight, -halfBase};

  std::vector<container::geometry::Vertex> vertices;
  vertices.reserve(16u);
  std::vector<uint32_t> indices;
  indices.reserve(18u);

  appendQuadFace(vertices, indices,
                 {{leftBack, rightBack, rightFront, leftFront}},
                 {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
  appendTriangleFace(vertices, indices, {apex, leftFront, rightFront},
                     {0.0f, 0.5f, 1.0f}, rightFront - leftFront);
  appendTriangleFace(vertices, indices, {apex, rightFront, rightBack},
                     {1.0f, 0.5f, 0.0f}, rightBack - rightFront);
  appendTriangleFace(vertices, indices, {apex, rightBack, leftBack},
                     {0.0f, 0.5f, -1.0f}, leftBack - rightBack);
  appendTriangleFace(vertices, indices, {apex, leftBack, leftFront},
                     {-1.0f, 0.5f, 0.0f}, leftFront - leftBack);

  return {std::move(vertices), std::move(indices), desc.materialIndex, false};
}

container::geometry::Mesh makeScenePrimitiveTorus(
    const ScenePrimitiveTorusDesc &desc) {
  const float minorRadius = positiveOr(desc.minorRadius, 0.15f);
  const float majorRadius =
      std::max(positiveOr(desc.majorRadius, 0.5f), minorRadius + 1.0e-3f);
  const uint32_t majorSegments = atLeast(desc.majorSegments, 3u);
  const uint32_t minorSegments = atLeast(desc.minorSegments, 3u);
  const uint32_t stride = minorSegments + 1u;

  std::vector<container::geometry::Vertex> vertices;
  vertices.reserve((majorSegments + 1u) * stride);
  std::vector<uint32_t> indices;
  indices.reserve(majorSegments * minorSegments * 6u);

  for (uint32_t major = 0u; major <= majorSegments; ++major) {
    const float u = static_cast<float>(major) /
                    static_cast<float>(majorSegments);
    for (uint32_t minor = 0u; minor <= minorSegments; ++minor) {
      const float v = static_cast<float>(minor) /
                      static_cast<float>(minorSegments);
      vertices.push_back(
          makeTorusVertex(kTwoPi * u, kTwoPi * v, majorRadius, minorRadius,
                          {u, v}));
    }
  }

  for (uint32_t major = 0u; major < majorSegments; ++major) {
    for (uint32_t minor = 0u; minor < minorSegments; ++minor) {
      const uint32_t a = major * stride + minor;
      const uint32_t b = a + 1u;
      const uint32_t c = (major + 1u) * stride + minor;
      const uint32_t d = c + 1u;
      indices.insert(indices.end(), {a, b, c, c, b, d});
    }
  }

  return {std::move(vertices), std::move(indices), desc.materialIndex, false};
}

std::optional<uint32_t> findReusableScenePrimitive(
    const container::geometry::Mesh& geometry,
    std::span<const container::geometry::Mesh> meshes,
    std::span<const int32_t> runtimeToSource) {
  if (geometry.empty() || meshes.size() != runtimeToSource.size() ||
      meshes.size() >= std::numeric_limits<uint32_t>::max())
    return std::nullopt;

  const auto equalVertex = [](const auto& a, const auto& b) {
    return a.position == b.position && a.normal == b.normal && a.tangent == b.tangent &&
        a.color == b.color && a.colorAlpha == b.colorAlpha &&
        a.texCoord == b.texCoord && a.texCoord1 == b.texCoord1;
  };
  for (uint32_t index = 0; index < meshes.size(); ++index) {
    const auto& mesh = meshes[index];
    if (runtimeToSource[index] == -1 &&
        mesh.materialIndex() == geometry.materialIndex() &&
        mesh.disableBackfaceCulling() == geometry.disableBackfaceCulling() &&
        mesh.indices() == geometry.indices() &&
        std::ranges::equal(mesh.vertices(), geometry.vertices(), equalVertex))
      return index;
  }
  return std::nullopt;
}

} // namespace container::renderer
