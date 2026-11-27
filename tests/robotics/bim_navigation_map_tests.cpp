#include "Container/robotics/BimNavigationMap.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using namespace container::robotics;
using enum BimNavigationCell;

void addFloor(std::vector<BimWalkableSurface>& support, glm::dvec2 minimum, glm::dvec2 maximum) {
  const glm::dvec3 lowerLeft{minimum, 0};
  const glm::dvec3 lowerRight{maximum.x, minimum.y, 0};
  const glm::dvec3 upperLeft{minimum.x, maximum.y, 0};
  const glm::dvec3 upperRight{maximum, 0};
  support.push_back({.entityId = "floor", .vertices = {lowerLeft, lowerRight, upperRight}, .normal = {0, 0, 1}});
  support.push_back({.entityId = "floor", .vertices = {lowerLeft, upperRight, upperLeft}, .normal = {0, 0, 1}});
}

TEST(BimNavigationMap, RequiresJointSupportAcrossTheWholeCell) {
  std::vector<BimWalkableSurface> support;
  addFloor(support, {0, 0}, {1, 1});
  // The second cell's centre and all four corners have support. Its narrow
  // interior gap still leaves it unknown. The third cell is only half covered.
  addFloor(support, {1, 0}, {1.63, 1});
  addFloor(support, {1.64, 0}, {2, 1});
  addFloor(support, {2, 0}, {2.5, 1});
  const BimNavigationMapSettings settings{.width = 4, .height = 1, .resolutionMeters = 1};
  EXPECT_EQ(rasterizeBimNavigationMap({}, support, {}, settings),
            (std::vector{Free, Unknown, Unknown, Unknown}));
}

TEST(BimNavigationMap, InflatesAProjectedLineByMetricDistanceFromTheCellSquare) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{2.15f, 1.15f, -.5f}, {2.15f, 1.85f, -.5f},
                    {2.15f, 1.85f, 2.5f}, {2.15f, 1.15f, 2.5f}};
  mesh->indices = {0, 1, 2, 0, 2, 3};
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "vertical-panel", .mesh = mesh});
  std::vector<BimWalkableSurface> support;
  addFloor(support, {0, 0}, {4, 3});
  BimNavigationMapSettings settings{.width = 4, .height = 3, .resolutionMeters = 1, .robotRadiusMeters = 0};
  EXPECT_EQ(rasterizeBimNavigationMap(geometry, support, {}, settings),
            (std::vector{Free, Free, Free,     Free,
                         Free, Free, Occupied, Free,
                         Free, Free, Free,     Free}));

  // The closest square in the adjacent column is 0.15 m away even though
  // its centre is 0.65 m away. A diagonal neighbour is over 0.20 m away.
  settings.robotRadiusMeters = 0.2;
  EXPECT_EQ(rasterizeBimNavigationMap(geometry, support, {}, settings),
            (std::vector{Free, Free,     Occupied, Free,
                         Free, Occupied, Occupied, Free,
                         Free, Free,     Occupied, Free}));
}

TEST(BimNavigationMap, FillsClosedSolidInteriorsWithoutFillingOpenShells) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{2, 2, -1}, {7, 2, -1}, {7, 7, -1}, {2, 7, -1},
                    {2, 2, 3}, {7, 2, 3}, {7, 7, 3}, {2, 7, 3}};
  mesh->indices = {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
                   2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "thick-obstacle", .mesh = mesh});
  std::vector<BimWalkableSurface> support;
  addFloor(support, {0, 0}, {9, 9});
  const BimNavigationMapSettings settings{.width = 9, .height = 9, .resolutionMeters = 1};
  constexpr size_t interior = 4 * 9 + 4;
  constexpr size_t boundary = 4 * 9 + 2;
  constexpr size_t exterior = 4 * 9 + 8;
  const auto open = rasterizeBimNavigationMap(geometry, support, {}, settings);
  ASSERT_EQ(open.size(), 81u);
  EXPECT_EQ(open[interior], Free);
  EXPECT_EQ(open[boundary], Occupied);
  EXPECT_EQ(open[exterior], Free);

  // Both caps are outside the blocking height band. Only a valid closed
  // mesh cross-section can mark the cell two metres from every side.
  mesh->indices.insert(mesh->indices.end(), {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7});
  const auto closed = rasterizeBimNavigationMap(geometry, support, {}, settings);
  ASSERT_EQ(closed.size(), open.size());
  EXPECT_EQ(closed[interior], Occupied);
  EXPECT_EQ(closed[boundary], Occupied);
  EXPECT_EQ(closed[exterior], Free);
}

TEST(BimNavigationMap, RejectsInvalidBoundsAndCapsuleBeforeRasterizing) {
  BimNavigationMapSettings settings{.width = 1, .height = 1};
  const auto rasterize = [&] { return rasterizeBimNavigationMap({}, {}, {}, settings); };
  settings.width = std::numeric_limits<uint32_t>::max();
  EXPECT_THROW(rasterize(), std::invalid_argument);
  settings = {.width = 1, .height = 1, .resolutionMeters = std::numeric_limits<double>::infinity()};
  EXPECT_THROW(rasterize(), std::invalid_argument);
  settings = {.originMeters = {1e80, 0}, .width = 1, .height = 1};
  EXPECT_THROW(rasterize(), std::invalid_argument);
  settings = {.width = 1, .height = 1, .robotRadiusMeters = 0.9, .robotHeightMeters = 1.6};
  EXPECT_THROW(rasterize(), std::invalid_argument);
}

} // namespace
