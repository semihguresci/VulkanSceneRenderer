#include "Container/renderer/scene/ScenePrimitives.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace {

using container::renderer::ScenePrimitiveKind;
using container::renderer::findReusableScenePrimitive;
using container::renderer::makeScenePrimitive;
using container::renderer::makeScenePrimitiveCube;
using container::renderer::makeScenePrimitivePlane;
using container::renderer::makeScenePrimitivePyramid;
using container::renderer::makeScenePrimitiveQuad;
using container::renderer::makeScenePrimitiveSphere;
using container::renderer::makeScenePrimitiveTorus;
using container::renderer::makeScenePrimitiveTriangularPrism;

using Edge = std::pair<uint32_t, uint32_t>;

TEST(ScenePrimitives, ReusesOnlyMatchingRuntimeGeometry) {
  const auto torus = makeScenePrimitive(ScenePrimitiveKind::Torus, 4);
  auto changedVertices = torus.vertices();
  changedVertices.front().texCoord1.x = 0.5f;
  const std::array meshes{
      torus,
      makeScenePrimitive(ScenePrimitiveKind::Torus, 5),
      container::geometry::Mesh(torus.vertices(), torus.indices(), 4, true),
      container::geometry::Mesh(std::move(changedVertices), torus.indices(), 4),
      torus,
  };
  const std::array<int32_t, 5> runtimeToSource{0, -1, -1, -1, -1};
  EXPECT_EQ(findReusableScenePrimitive(torus, meshes, runtimeToSource), 4u);
  EXPECT_FALSE(findReusableScenePrimitive(torus, std::span(meshes).first(4),
                                           std::span(runtimeToSource).first(4)));
  EXPECT_FALSE(findReusableScenePrimitive(torus, meshes, {}));
  EXPECT_FALSE(findReusableScenePrimitive({}, meshes, runtimeToSource));
}

TEST(ScenePrimitives, RepeatedAddsRetainOneMeshPerShape) {
  std::vector<container::geometry::Mesh> retained;
  std::vector<int32_t> runtimeToSource;
  constexpr std::array kinds{ScenePrimitiveKind::Torus, ScenePrimitiveKind::Sphere,
      ScenePrimitiveKind::Cube};
  for (size_t edit = 0; edit < 130; ++edit) {
    auto geometry = makeScenePrimitive(kinds[edit % kinds.size()], 4);
    const auto existing = findReusableScenePrimitive(geometry, retained, runtimeToSource);
    if (edit < kinds.size()) {
      EXPECT_FALSE(existing);
      retained.push_back(std::move(geometry));
      runtimeToSource.push_back(-1);
    } else {
      EXPECT_EQ(existing, edit % kinds.size());
    }
  }
  EXPECT_EQ(retained.size(), kinds.size());
}

Edge orderedEdge(uint32_t a, uint32_t b) {
  return a < b ? Edge{a, b} : Edge{b, a};
}

std::map<Edge, uint32_t> edgeUseCounts(std::span<const uint32_t> indices) {
  std::map<Edge, uint32_t> edges;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    ++edges[orderedEdge(indices[i], indices[i + 1])];
    ++edges[orderedEdge(indices[i + 1], indices[i + 2])];
    ++edges[orderedEdge(indices[i + 2], indices[i])];
  }
  return edges;
}

glm::vec3 triangleNormal(const std::vector<container::geometry::Vertex> &verts,
                         const std::array<uint32_t, 3> &triangle) {
  const glm::vec3 p0 = verts[triangle[0]].position;
  const glm::vec3 p1 = verts[triangle[1]].position;
  const glm::vec3 p2 = verts[triangle[2]].position;
  return glm::normalize(glm::cross(p1 - p0, p2 - p0));
}

void expectUvNear(const container::geometry::Vertex &vertex, float u, float v) {
  EXPECT_NEAR(vertex.texCoord.x, u, 1.0e-6f);
  EXPECT_NEAR(vertex.texCoord.y, v, 1.0e-6f);
}

void expectValidIndicesAndUvs(const container::geometry::Mesh &mesh) {
  ASSERT_FALSE(mesh.vertices().empty());
  ASSERT_FALSE(mesh.indices().empty());
  ASSERT_EQ(mesh.indices().size() % 3u, 0u);

  for (const uint32_t index : mesh.indices()) {
    EXPECT_LT(index, mesh.vertices().size());
  }

  for (const auto &vertex : mesh.vertices()) {
    EXPECT_TRUE(std::isfinite(vertex.position.x));
    EXPECT_TRUE(std::isfinite(vertex.position.y));
    EXPECT_TRUE(std::isfinite(vertex.position.z));
    EXPECT_TRUE(std::isfinite(vertex.texCoord.x));
    EXPECT_TRUE(std::isfinite(vertex.texCoord.y));
    EXPECT_GE(vertex.texCoord.x, -1.0e-6f);
    EXPECT_GE(vertex.texCoord.y, -1.0e-6f);
    EXPECT_LE(vertex.texCoord.x, 1.0f + 1.0e-6f);
    EXPECT_LE(vertex.texCoord.y, 1.0f + 1.0e-6f);
    EXPECT_FLOAT_EQ(vertex.texCoord1.x, vertex.texCoord.x);
    EXPECT_FLOAT_EQ(vertex.texCoord1.y, vertex.texCoord.y);
    EXPECT_NEAR(glm::length(vertex.normal), 1.0f, 1.0e-5f);
    EXPECT_NEAR(glm::length(glm::vec3(vertex.tangent)), 1.0f, 1.0e-5f);
    EXPECT_NEAR(glm::dot(vertex.normal, glm::vec3(vertex.tangent)), 0.0f,
                1.0e-5f);
  }
}

void expectTriangleWindingFollowsVertexNormals(
    const container::geometry::Mesh &mesh) {
  for (size_t i = 0; i + 2 < mesh.indices().size(); i += 3) {
    const auto &v0 = mesh.vertices()[mesh.indices()[i + 0u]];
    const auto &v1 = mesh.vertices()[mesh.indices()[i + 1u]];
    const auto &v2 = mesh.vertices()[mesh.indices()[i + 2u]];
    const glm::vec3 geometric =
        glm::cross(v1.position - v0.position, v2.position - v0.position);
    ASSERT_GT(glm::length(geometric), 1.0e-7f) << "triangle " << i / 3u;
    const glm::vec3 averageNormal =
        glm::normalize(v0.normal + v1.normal + v2.normal);
    EXPECT_GT(glm::dot(glm::normalize(geometric), averageNormal), 0.25f)
        << "triangle " << i / 3u;
  }
}

void expectTangentFrame(const container::geometry::Vertex &vertex,
                        const glm::vec3 &normal,
                        const glm::vec3 &tangent) {
  EXPECT_NEAR(glm::length(vertex.normal), 1.0f, 1.0e-6f);
  EXPECT_NEAR(glm::length(glm::vec3(vertex.tangent)), 1.0f, 1.0e-6f);
  EXPECT_NEAR(glm::dot(vertex.normal, glm::vec3(vertex.tangent)), 0.0f,
              1.0e-6f);
  EXPECT_NEAR(glm::dot(vertex.normal, normal), 1.0f, 1.0e-6f);
  EXPECT_NEAR(glm::dot(glm::vec3(vertex.tangent), tangent), 1.0f, 1.0e-6f);
  EXPECT_FLOAT_EQ(vertex.tangent.w, 1.0f);
}

} // namespace

TEST(ScenePrimitivesTests, QuadBuildsSingleSidedSurface) {
  const auto quad = makeScenePrimitiveQuad();

  ASSERT_EQ(quad.vertices().size(), 4u);
  ASSERT_EQ(quad.indices().size(), 6u);
  EXPECT_FALSE(quad.disableBackfaceCulling());
  expectValidIndicesAndUvs(quad);
  expectTriangleWindingFollowsVertexNormals(quad);
}

TEST(ScenePrimitivesTests, PlaneBuildsFourVerticesTwoFacesAndStableUvs) {
  const auto plane = makeScenePrimitivePlane();

  ASSERT_EQ(plane.vertices().size(), 4u);
  ASSERT_EQ(plane.indices().size(), 6u);
  EXPECT_TRUE(plane.disableBackfaceCulling());
  EXPECT_EQ(plane.indices(),
            (std::vector<uint32_t>{0u, 1u, 2u, 0u, 2u, 3u}));

  expectUvNear(plane.vertices()[0], 0.0f, 0.0f);
  expectUvNear(plane.vertices()[1], 1.0f, 0.0f);
  expectUvNear(plane.vertices()[2], 1.0f, 1.0f);
  expectUvNear(plane.vertices()[3], 0.0f, 1.0f);

  EXPECT_NEAR(triangleNormal(plane.vertices(), {0u, 1u, 2u}).z, 1.0f,
              1.0e-6f);
  EXPECT_NEAR(triangleNormal(plane.vertices(), {0u, 2u, 3u}).z, 1.0f,
              1.0e-6f);

  const auto edges = edgeUseCounts(plane.indices());
  ASSERT_EQ(edges.size(), 5u);
  EXPECT_EQ(edges.at({0u, 1u}), 1u);
  EXPECT_EQ(edges.at({1u, 2u}), 1u);
  EXPECT_EQ(edges.at({2u, 3u}), 1u);
  EXPECT_EQ(edges.at({0u, 3u}), 1u);
  EXPECT_EQ(edges.at({0u, 2u}), 2u);

  for (const auto &vertex : plane.vertices()) {
    expectTangentFrame(vertex, glm::vec3(0.0f, 0.0f, 1.0f),
                       glm::vec3(1.0f, 0.0f, 0.0f));
    EXPECT_FLOAT_EQ(vertex.texCoord1.x, vertex.texCoord.x);
    EXPECT_FLOAT_EQ(vertex.texCoord1.y, vertex.texCoord.y);
  }
}

TEST(ScenePrimitivesTests, PlaneSupportsExplicitRendererFloorAxes) {
  const auto plane = makeScenePrimitivePlane({
      .center = {0.0f, 2.0f, 0.0f},
      .uAxis = {1.0f, 0.0f, 0.0f},
      .vAxis = {0.0f, 0.0f, -1.0f},
      .width = 4.0f,
      .height = 2.0f,
  });

  ASSERT_EQ(plane.vertices().size(), 4u);
  EXPECT_EQ(plane.vertices()[0].position, glm::vec3(-2.0f, 2.0f, 1.0f));
  EXPECT_EQ(plane.vertices()[1].position, glm::vec3(2.0f, 2.0f, 1.0f));
  EXPECT_EQ(plane.vertices()[2].position, glm::vec3(2.0f, 2.0f, -1.0f));
  EXPECT_EQ(plane.vertices()[3].position, glm::vec3(-2.0f, 2.0f, -1.0f));
  EXPECT_NEAR(triangleNormal(plane.vertices(), {0u, 1u, 2u}).y, 1.0f,
              1.0e-6f);

  for (const auto &vertex : plane.vertices()) {
    expectTangentFrame(vertex, glm::vec3(0.0f, 1.0f, 0.0f),
                       glm::vec3(1.0f, 0.0f, 0.0f));
  }
}

TEST(ScenePrimitivesTests, PrimitiveFactoryCreatesAllMenuShapes) {
  struct ExpectedShape {
    ScenePrimitiveKind kind;
    size_t vertices;
    size_t indices;
    bool doubleSided;
  };

  const std::array expectedShapes = {
      ExpectedShape{ScenePrimitiveKind::Quad, 4u, 6u, false},
      ExpectedShape{ScenePrimitiveKind::Plane, 4u, 6u, true},
      ExpectedShape{ScenePrimitiveKind::Cube, 24u, 36u, false},
      ExpectedShape{ScenePrimitiveKind::Sphere, 377u, 2160u, false},
      ExpectedShape{ScenePrimitiveKind::TriangularPrism, 18u, 24u, false},
      ExpectedShape{ScenePrimitiveKind::Pyramid, 16u, 18u, false},
      ExpectedShape{ScenePrimitiveKind::Torus, 429u, 2304u, false},
  };

  for (const ExpectedShape &expected : expectedShapes) {
    const auto mesh = makeScenePrimitive(expected.kind);
    EXPECT_EQ(mesh.vertices().size(), expected.vertices);
    EXPECT_EQ(mesh.indices().size(), expected.indices);
    EXPECT_EQ(mesh.disableBackfaceCulling(), expected.doubleSided);
    expectValidIndicesAndUvs(mesh);
    expectTriangleWindingFollowsVertexNormals(mesh);
  }
}

TEST(ScenePrimitivesTests, PrismPyramidSphereAndTorusHaveValidUvTopology) {
  const std::array meshes = {
      makeScenePrimitiveTriangularPrism(),
      makeScenePrimitivePyramid(),
      makeScenePrimitiveSphere(),
      makeScenePrimitiveTorus(),
  };

  for (const auto &mesh : meshes) {
    expectValidIndicesAndUvs(mesh);
    expectTriangleWindingFollowsVertexNormals(mesh);
  }
}

TEST(ScenePrimitivesTests, CubeUsesIndependentUvMappedQuadFaces) {
  const auto cube = makeScenePrimitiveCube();

  ASSERT_EQ(cube.vertices().size(), 24u);
  ASSERT_EQ(cube.indices().size(), 36u);

  for (uint32_t face = 0; face < 6u; ++face) {
    const uint32_t vertexBase = face * 4u;
    const uint32_t indexBase = face * 6u;
    EXPECT_EQ(cube.indices()[indexBase + 0u], vertexBase + 0u);
    EXPECT_EQ(cube.indices()[indexBase + 1u], vertexBase + 1u);
    EXPECT_EQ(cube.indices()[indexBase + 2u], vertexBase + 2u);
    EXPECT_EQ(cube.indices()[indexBase + 3u], vertexBase + 0u);
    EXPECT_EQ(cube.indices()[indexBase + 4u], vertexBase + 2u);
    EXPECT_EQ(cube.indices()[indexBase + 5u], vertexBase + 3u);

    expectUvNear(cube.vertices()[vertexBase + 0u], 0.0f, 0.0f);
    expectUvNear(cube.vertices()[vertexBase + 1u], 1.0f, 0.0f);
    expectUvNear(cube.vertices()[vertexBase + 2u], 1.0f, 1.0f);
    expectUvNear(cube.vertices()[vertexBase + 3u], 0.0f, 1.0f);
  }

  const auto edges = edgeUseCounts(cube.indices());
  ASSERT_EQ(edges.size(), 30u);
  EXPECT_EQ(std::ranges::count_if(edges, [](const auto &entry) {
              return entry.second == 1u;
            }),
            24);
  EXPECT_EQ(std::ranges::count_if(edges, [](const auto &entry) {
              return entry.second == 2u;
            }),
            6);
}
