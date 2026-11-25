#include "Container/geometry/GeometryLod.h"
#include "Container/geometry/GeometryLodSelection.h"
#include "Container/utility/ModelImportCancellation.h"

#include <gtest/gtest.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace container::geometry {
namespace {

constexpr uint32_t kSide = 25;

struct Grid {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};

Grid makeGrid(bool hole = false, bool corrugated = false, bool uvSeam = false) {
  Grid grid;
  for (uint32_t y = 0; y < kSide; ++y) {
    for (uint32_t x = 0; x < kSide; ++x) {
      Vertex vertex;
      constexpr float scale = 1.0f / (kSide - 1);
      const auto u = x * scale;
      const auto v = y * scale;
      vertex.position = {u, v, corrugated ? 0.04f * std::sin(u * 9.0f) : 0.0f};
      vertex.normal = {0.0f, 0.0f, 1.0f};
      vertex.texCoord = vertex.texCoord1 = {u, v};
      grid.vertices.push_back(vertex);
    }
  }
  constexpr uint32_t seam = kSide / 2;
  if (uvSeam) {
    for (uint32_t y = 0; y < kSide; ++y) {
      auto vertex = grid.vertices[y * kSide + seam];
      vertex.texCoord.x -= 1.0f;
      grid.vertices.push_back(vertex);
    }
  }
  for (uint32_t y = 0; y + 1 < kSide; ++y) {
    for (uint32_t x = 0; x + 1 < kSide; ++x) {
      if (hole && x >= 9 && x < 15 && y >= 9 && y < 15)
        continue;
      const uint32_t right = y * kSide + x + 1;
      const uint32_t left = uvSeam && x == seam ? kSide * kSide + y : right - 1;
      const uint32_t topLeft = left + (uvSeam && x == seam ? 1 : kSide);
      grid.indices.insert(grid.indices.end(), {left, right, right + kSide,
                                                right + kSide, topLeft, left});
    }
  }
  return grid;
}

using Edge = std::array<uint32_t, 2>;

std::set<Edge> boundaryEdges(std::span<const uint32_t> indices) {
  std::map<Edge, unsigned> incidence;
  for (size_t first = 0; first < indices.size(); first += 3) {
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto a = indices[first + corner];
      const auto b = indices[first + (corner + 1) % 3];
      ++incidence[{(std::min)(a, b), (std::max)(a, b)}];
    }
  }
  std::set<Edge> boundary;
  for (const auto& [edge, adjacent] : incidence) {
    EXPECT_LE(adjacent, 2u);
    if (adjacent == 1)
      boundary.insert(edge);
  }
  return boundary;
}

void expectSimpleReduction(const Grid& grid, const std::vector<TriangleLod>& levels) {
  ASSERT_EQ(levels.size(), 2u);
  const std::set usedOriginal(grid.indices.begin(), grid.indices.end());
  auto previousSize = grid.indices.size();
  float previousError = 0.0f;
  for (const auto& level : levels) {
    EXPECT_LE(level.indices.size(), previousSize * 3 / 4);
    ASSERT_EQ(level.indices.size() % 3, 0u);
    EXPECT_TRUE(std::isfinite(level.maxError));
    EXPECT_GE(level.maxError, previousError);
    EXPECT_GT(level.maxError, 0.0f);
    const std::set usedCoarse(level.indices.begin(), level.indices.end());
    for (const auto index : usedCoarse)
      ASSERT_TRUE(usedOriginal.contains(index));
    for (const auto index : usedOriginal) {
      const auto position = glm::dvec3(grid.vertices[index].position);
      EXPECT_TRUE(std::ranges::any_of(usedCoarse, [&](uint32_t retained) {
        const auto retainedPosition = glm::dvec3(grid.vertices[retained].position);
        return glm::length(position - retainedPosition) <= level.maxError;
      })) << "No retained vertex within error bound at original vertex " << index;
    }
    for (size_t first = 0; first < level.indices.size(); first += 3) {
      const auto a = grid.vertices[level.indices[first]].position;
      const auto b = grid.vertices[level.indices[first + 1]].position;
      const auto c = grid.vertices[level.indices[first + 2]].position;
      EXPECT_GT(glm::cross(b - a, c - a).z, 0.0f);
    }
    previousSize = level.indices.size();
    previousError = level.maxError;
  }
}

TEST(GeometryLod, ReducesPlanarGridDeterministically) {
  const auto grid = makeGrid();
  const auto first = buildTriangleLods(grid.vertices, grid.indices);
  expectSimpleReduction(grid, first);
  const auto second = buildTriangleLods(grid.vertices, grid.indices);
  ASSERT_EQ(first.size(), second.size());
  for (size_t index = 0; index < first.size(); ++index) {
    EXPECT_EQ(first[index].indices, second[index].indices);
    EXPECT_EQ(first[index].maxError, second[index].maxError);
  }
}

TEST(GeometryLod, ReducesGentlyCorrugatedMeshWithConservativeErrors) {
  const auto grid = makeGrid(false, true);
  expectSimpleReduction(grid, buildTriangleLods(grid.vertices, grid.indices));
}

TEST(GeometryLod, RetainsOuterAndHoleBoundaries) {
  const auto grid = makeGrid(true);
  const auto expected = boundaryEdges(grid.indices);
  ASSERT_EQ(expected.size(), 4 * (kSide - 1) + 4 * 6u);
  const auto levels = buildTriangleLods(grid.vertices, grid.indices);
  expectSimpleReduction(grid, levels);
  for (const auto& level : levels)
    EXPECT_EQ(boundaryEdges(level.indices), expected);
}

TEST(GeometryLod, RetainsOriginalSharpCreaseEdgesAtEveryLevel) {
  auto grid = makeGrid();
  for (auto& vertex : grid.vertices)
    vertex.position.z = std::abs(vertex.position.x - 0.5f);
  const auto levels = buildTriangleLods(grid.vertices, grid.indices);
  expectSimpleReduction(grid, levels);
  constexpr uint32_t ridge = kSide / 2;
  for (const auto& level : levels) {
    std::map<Edge, unsigned> ridgeEdges;
    for (size_t first = 0; first < level.indices.size(); first += 3) {
      for (size_t corner = 0; corner < 3; ++corner) {
        const auto a = level.indices[first + corner];
        const auto b = level.indices[first + (corner + 1) % 3];
        if (a % kSide == ridge && b % kSide == ridge)
          ++ridgeEdges[{(std::min)(a, b), (std::max)(a, b)}];
      }
    }
    ASSERT_EQ(ridgeEdges.size(), kSide - 1);
    for (uint32_t y = 0; y + 1 < kSide; ++y) {
      const Edge edge{y * kSide + ridge, (y + 1) * kSide + ridge};
      EXPECT_EQ(ridgeEdges[edge], 2u);
    }
  }
}

TEST(GeometryLod, PreservesBothUvSeamWedges) {
  const auto grid = makeGrid(false, false, true);
  const auto expected = boundaryEdges(grid.indices);
  ASSERT_EQ(expected.size(), 6 * (kSide - 1));
  const auto levels = buildTriangleLods(grid.vertices, grid.indices);
  expectSimpleReduction(grid, levels);
  for (const auto& level : levels)
    EXPECT_EQ(boundaryEdges(level.indices), expected);
}

TEST(GeometryLod, PreservesSeamEdgesWithOnlyOneSplitEndpoint) {
  auto grid = makeGrid();
  constexpr size_t interiorCorner = (12 * (kSide - 1) + 12) * 6;
  auto wedge = grid.vertices[grid.indices[interiorCorner]];
  wedge.texCoord.x += 1.0f;
  grid.indices[interiorCorner] = static_cast<uint32_t>(grid.vertices.size());
  grid.vertices.push_back(wedge);
  const auto expected = boundaryEdges(grid.indices);
  ASSERT_EQ(expected.size(), 4 * (kSide - 1) + 4);
  const auto levels = buildTriangleLods(grid.vertices, grid.indices);
  expectSimpleReduction(grid, levels);
  for (const auto& level : levels)
    EXPECT_EQ(boundaryEdges(level.indices), expected);
}

TEST(GeometryLod, WeldsFaceCornersWhenOnlyProceduralTangentsDiffer) {
  const auto grid = makeGrid();
  Grid expanded;
  for (size_t corner = 0; corner < grid.indices.size(); ++corner) {
    auto vertex = grid.vertices[grid.indices[corner]];
    vertex.tangent.x = (corner / 3) % 2 == 0 ? -1.0f : 1.0f;
    expanded.indices.push_back(static_cast<uint32_t>(corner));
    expanded.vertices.push_back(vertex);
  }
  EXPECT_TRUE(buildTriangleLods(expanded.vertices, expanded.indices).empty());
  const auto levels = buildTriangleLods(expanded.vertices, expanded.indices, {},
                                       TriangleLodWeldPolicy::IgnoreGeneratedTangents);
  expectSimpleReduction(expanded, levels);
  const auto expected = boundaryEdges(grid.indices);
  for (const auto& level : levels) {
    std::vector<uint32_t> welded;
    for (const auto corner : level.indices)
      welded.push_back(grid.indices[corner]);
    EXPECT_EQ(boundaryEdges(welded), expected);
  }
}

TEST(GeometryLod, KeepsFullDetailForSmallAndMalformedRanges) {
  auto grid = makeGrid();
  EXPECT_TRUE(buildTriangleLods(grid.vertices, std::span(grid.indices).first(3)).empty());
  EXPECT_TRUE(buildTriangleLods(grid.vertices, std::span(grid.indices).subspan(1)).empty());
  const auto first = grid.indices[0];
  grid.indices[0] = static_cast<uint32_t>(grid.vertices.size());
  EXPECT_TRUE(buildTriangleLods(grid.vertices, grid.indices).empty());
  grid.indices[0] = grid.indices[1];
  EXPECT_TRUE(buildTriangleLods(grid.vertices, grid.indices).empty());
  grid.indices[0] = first;
  grid.vertices[first].position.x = (std::numeric_limits<float>::infinity)();
  EXPECT_TRUE(buildTriangleLods(grid.vertices, grid.indices).empty());
}

TEST(GeometryLod, PropagatesImportCancellation) {
  const auto grid = makeGrid();
  std::stop_source stop;
  ASSERT_TRUE(stop.request_stop());
  EXPECT_THROW((void)buildTriangleLods(grid.vertices, grid.indices, stop.get_token()),
               util::ModelImportCancelled);
}

TEST(GeometryLodSelection, UsesViewportErrorAndApproachHysteresis) {
  constexpr std::array levels{GeometryLodRange{100, 60, 0.1f},
                              GeometryLodRange{160, 30, 0.19f}};
  const glm::mat4 model(1.0f);
  GeometryLodView view;
  view.projection = math::perspectiveRH_ReverseZ(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
  view.viewport = {800, 800};
  const auto select = [&](float distance, float offset, uint32_t previous) {
    return selectGeometryLod(levels, view, {offset, 0.0f, -distance, 1.0f}, model,
                             2.0f, previous);
  };
  EXPECT_EQ(select(18.0f, 0.0f, 0), 0u);
  EXPECT_EQ(select(24.0f, 0.0f, 0), 0u);
  EXPECT_EQ(select(30.0f, 0.0f, 0), 1u);
  EXPECT_EQ(select(54.0f, 0.0f, 1), 2u);
  EXPECT_EQ(select(24.0f, 0.0f, 2), 1u);
  EXPECT_EQ(select(18.0f, 0.0f, 1), 0u);
  EXPECT_EQ(select(30.0f, 25.0f, 0), 0u);
  view.viewport = {1600, 1600};
  EXPECT_EQ(select(30.0f, 0.0f, 1), 0u);
  EXPECT_EQ(selectGeometryLod(levels, view, {0.0f, 0.0f, -1.05f, 1.0f},
                              model, 1e6f, 2), 0u);
}

TEST(GeometryLodSelection, OrthographicErrorDependsOnScaleAndZoom) {
  constexpr std::array levels{GeometryLodRange{100, 60, 0.07f},
                              GeometryLodRange{160, 30, 0.1f}};
  const glm::mat4 model(1.0f);
  const glm::mat4 largerModel = glm::scale(model, glm::vec3(2.0f, 1.0f, 1.0f));
  GeometryLodView view;
  view.projection = math::orthoRH_ReverseZ(-20.0f, 20.0f, -20.0f, 20.0f, 0.1f, 100.0f);
  view.viewport = {800, 800};
  const glm::vec4 nearSphere(0.0f, 0.0f, -3.0f, 1.0f);
  const glm::vec4 farSphere(0.0f, 0.0f, -60.0f, 1.0f);
  EXPECT_EQ(selectGeometryLod(levels, view, nearSphere, model, 2.0f, 0), 1u);
  EXPECT_EQ(selectGeometryLod(levels, view, farSphere, model, 2.0f, 0), 1u);
  EXPECT_EQ(selectGeometryLod(levels, view, farSphere, largerModel, 2.0f, 1), 0u);
  EXPECT_EQ(selectGeometryLod(levels, view, {0.0f, 0.0f, -1.05f, 1.0f},
                              model, 1e6f, 2), 0u);
  view.projection = math::orthoRH_ReverseZ(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);
  EXPECT_EQ(selectGeometryLod(levels, view, farSphere, model, 2.0f, 1), 0u);
}

} // namespace
} // namespace container::geometry
