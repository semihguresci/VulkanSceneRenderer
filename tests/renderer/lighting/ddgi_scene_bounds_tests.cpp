#include "Container/renderer/lighting/DdgiSceneBounds.h"
#include "Container/renderer/raytracing/RaySceneExtraction.h"

#include <gtest/gtest.h>

#include <array>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace container::renderer;

struct SourceFixture {
  std::vector<container::geometry::Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<container::gpu::ObjectData> objects{1};
  std::vector<DrawCommand> draws;

  RaySceneProviderSource source(std::string identity = "mesh") const {
    return {{std::move(identity)}, 1, vertices, indices, objects, draws};
  }
};

SourceFixture cube() {
  SourceFixture fixture;
  const glm::vec3 minimum(-1, -2, -3), maximum(2, 3, 4);
  for (uint32_t corner = 0; corner < 8; ++corner) {
    container::geometry::Vertex vertex;
    for (uint32_t axis = 0; axis < 3; ++axis)
      vertex.position[axis] = (corner & (1u << axis)) != 0
                                  ? maximum[axis] : minimum[axis];
    fixture.vertices.push_back(vertex);
  }
  fixture.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6,
                     0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7,
                     0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
  fixture.draws = {{0, 0, uint32_t(fixture.indices.size()), 1}};
  return fixture;
}

void expectBounds(const DdgiSceneBounds &bounds, glm::vec3 minimum,
                  glm::vec3 maximum) {
  ASSERT_TRUE(bounds.valid);
  for (uint32_t axis = 0; axis < 3; ++axis) {
    EXPECT_NEAR(bounds.minimum[axis], minimum[axis], 1e-6f);
    EXPECT_NEAR(bounds.maximum[axis], maximum[axis], 1e-6f);
  }
}

template <typename Function>
void expectInvalid(Function function, const char *reason) {
  try {
    (void)function();
    FAIL() << "Expected invalid_argument: " << reason;
  } catch (const std::invalid_argument &error) {
    const std::string message(error.what());
    EXPECT_NE(message.find("DDGI scene bounds"), std::string::npos);
    EXPECT_NE(message.find(reason), std::string::npos) << message;
  }
}

TEST(DdgiSceneBoundsTests, UsesCurrentWorldModelInsteadOfImportedBoundsOrSpheres) {
  auto fixture = cube();
  fixture.objects[0].boundingSphere = {10000, 10000, 10000, 0.1f};
  auto sources = std::array{fixture.source()};
  expectBounds(computeDdgiSceneBounds(sources), {-1, -2, -3}, {2, 3, 4});
  fixture.objects[0].model = glm::translate(glm::mat4(1), {10, -20, 30});
  expectBounds(computeDdgiSceneBounds(sources), {9, -22, 27}, {12, -17, 34});
}

TEST(DdgiSceneBoundsTests, HandlesRotationNonuniformScaleAndReflection) {
  auto fixture = cube();
  auto sources = std::array{fixture.source()};
  // Nonuniform scale followed by exact +90-degree rotation around world Z.
  fixture.objects[0].model[0] = {0, 2, 0, 0};
  fixture.objects[0].model[1] = {-3, 0, 0, 0};
  fixture.objects[0].model[2] = {0, 0, 0.5f, 0};
  expectBounds(computeDdgiSceneBounds(sources), {-9, -2, -1.5f}, {6, 4, 2});
  fixture.objects[0].model = glm::scale(glm::mat4(1), {-2, 3, 0.5f});
  expectBounds(computeDdgiSceneBounds(sources), {-4, -6, -1.5f}, {2, 9, 2});
}

TEST(DdgiSceneBoundsTests, ShearBoundsEveryReferencedVertex) {
  auto fixture = cube();
  // x_world = x_object + 2*y_object. Column-length-scaled spheres are not
  // guaranteed conservative for general hierarchical/sheared transforms.
  fixture.objects[0].model[1].x = 2;
  const auto sources = std::array{fixture.source()};
  expectBounds(computeDdgiSceneBounds(sources), {-5, -2, -3}, {8, 3, 4});
}

TEST(DdgiSceneBoundsTests, IndexOffsetAndObjectInstanceRangeSelectOnlyReferencedData) {
  SourceFixture fixture;
  fixture.vertices.resize(5);
  fixture.vertices[0].position = {10000, 10000, 10000};
  fixture.vertices[1].position = {-1, 0, 0};
  fixture.vertices[2].position = {1, 0, 0};
  fixture.vertices[3].position = {0, 1, 0};
  fixture.vertices[4].position = {-10000, -10000, -10000};
  // Prefix indices need not be valid because firstIndex skips them. Indices
  // after firstIndex are absolute vertex indices, without a vertex offset.
  fixture.indices = {99, 99, 99, 1, 2, 3};
  fixture.objects.resize(4);
  fixture.objects[0].model = glm::mat4(0);
  fixture.objects[1].model = glm::translate(glm::mat4(1), {10, 0, 0});
  fixture.objects[2].model = glm::translate(glm::mat4(1), {-10, 0, 0});
  fixture.objects[3].model = glm::translate(glm::mat4(1), {10000, 0, 0});
  fixture.draws = {{1, 3, 3, 2}};
  const auto sources = std::array{fixture.source()};
  expectBounds(computeDdgiSceneBounds(sources), {-11, 0, 0}, {11, 1, 0});
}

TEST(DdgiSceneBoundsTests, UnreferencedDiagnosticObjectsAndAttributesDoNotContribute) {
  auto fixture = cube();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  fixture.objects.resize(2);
  fixture.objects[1].temporalInfo.z = std::numeric_limits<uint32_t>::max();
  fixture.objects[1].model[3].x = nan;
  fixture.vertices.emplace_back().position = {nan, nan, nan};
  // Lighting/material attributes are irrelevant to geometric volume coverage.
  fixture.vertices[0].normal.x = nan;
  const auto sources = std::array{fixture.source()};
  expectBounds(computeDdgiSceneBounds(sources), {-1, -2, -3}, {2, 3, 4});
}

TEST(DdgiSceneBoundsTests, UnionsPrimaryAndAuxiliaryTrianglesAtTheirCurrentTransforms) {
  auto mesh = cube();
  auto bim = cube();
  auto usd = cube();
  bim.objects[0].model = glm::translate(glm::mat4(1), {30, 0, 0});
  usd.objects[0].model = glm::translate(glm::mat4(1), {0, -40, 0});
  const auto sources = std::array{mesh.source("mesh"), bim.source("bim"),
                                 usd.source("usd")};
  expectBounds(computeDdgiSceneBounds(sources), {-1, -42, -3}, {32, 3, 4});
}

TEST(DdgiSceneBoundsTests, EmptyAndInactiveDrawsDoNotFabricateBounds) {
  EXPECT_FALSE(computeDdgiSceneBounds({}).valid);
  SourceFixture empty;
  auto sources = std::array{empty.source()};
  EXPECT_FALSE(computeDdgiSceneBounds(sources).valid);
  empty.draws = {{99, 99, 0, 99}, {99, 99, 3, 0}};
  sources = {empty.source()};
  EXPECT_FALSE(computeDdgiSceneBounds(sources).valid);
}

TEST(DdgiSceneBoundsTests, InvalidDrawRangesFailBeforePartialBoundsCanBeReturned) {
  auto good = cube();
  auto invalid = cube();
  for (const auto draw : {DrawCommand{0, 0, 2, 1}, {0, 35, 3, 1},
                          {0, std::numeric_limits<uint32_t>::max(), 3, 1},
                          {1, 0, 3, 1}, {0, 0, 3, 2},
                          {std::numeric_limits<uint32_t>::max(), 0, 3, 2}}) {
    invalid.draws = {draw};
    const auto sources = std::array{good.source("good"), invalid.source("bad")};
    expectInvalid([&] { return computeDdgiSceneBounds(sources); },
                  draw.indexCount == 2 ? "triangle" : "range");
  }
}

TEST(DdgiSceneBoundsTests, RejectsBadReferencedIndexAndNonfinitePosition) {
  auto fixture = cube();
  auto sources = std::array{fixture.source()};
  fixture.indices[0] = uint32_t(fixture.vertices.size());
  expectInvalid([&] { return computeDdgiSceneBounds(sources); }, "vertex index");
  fixture.indices[0] = 0;
  fixture.vertices[0].position.x = std::numeric_limits<float>::infinity();
  expectInvalid([&] { return computeDdgiSceneBounds(sources); }, "position");
}

TEST(DdgiSceneBoundsTests, RejectsNonfiniteNonaffineAndSingularCurrentTransforms) {
  auto fixture = cube();
  const auto sources = std::array{fixture.source()};
  fixture.objects[0].model[0].x = std::numeric_limits<float>::quiet_NaN();
  expectInvalid([&] { return computeDdgiSceneBounds(sources); }, "finite");
  fixture.objects[0].model = glm::mat4(1);
  fixture.objects[0].model[0][3] = 0.1f;
  expectInvalid([&] { return computeDdgiSceneBounds(sources); }, "affine");
  fixture.objects[0].model = glm::scale(glm::mat4(1), {0, 1, 1});
  expectInvalid([&] { return computeDdgiSceneBounds(sources); }, "invertible");
}

TEST(DdgiSceneBoundsTests, RejectsWorldCoordinateOverflowFromFiniteSourceData) {
  auto fixture = cube();
  fixture.vertices[0].position.x = std::numeric_limits<float>::max();
  fixture.objects[0].model = glm::scale(glm::mat4(1), {2, 1, 1});
  const auto sources = std::array{fixture.source()};
  expectInvalid([&] { return computeDdgiSceneBounds(sources); }, "world coordinates");
}

} // namespace
