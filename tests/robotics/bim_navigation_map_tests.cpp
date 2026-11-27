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

void addFloor(std::vector<BimWalkableSurface>& support, glm::dvec2 minimum, glm::dvec2 maximum,
              double height = 0) {
  const glm::dvec3 lowerLeft{minimum, height};
  const glm::dvec3 lowerRight{maximum.x, minimum.y, height};
  const glm::dvec3 upperLeft{minimum.x, maximum.y, height};
  const glm::dvec3 upperRight{maximum, height};
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

TEST(BimNavigationMap, KeepsObstaclesBelowTheFloorSelectionTolerance) {
  std::vector<BimWalkableSurface> support;
  addFloor(support, {0, 0}, {4, 1});
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "low-box", .kind = BimColliderKind::DynamicBox,
      .originMeters = {1.5, 0.5, .011}, .halfExtents = {.1f, .1f, .007f}});
  geometry.colliders.push_back({.entityId = "buried-box", .kind = BimColliderKind::DynamicBox,
      .originMeters = {2.5, 0.5, -.01}, .halfExtents = {.1f, .1f, .005f}});
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{3.5f, .4f, .004f}, {3.5f, .6f, .004f}, {3.5f, .5f, .018f}};
  mesh->indices = {0, 1, 2};
  geometry.colliders.push_back({.entityId = "low-panel", .mesh = mesh});
  const BimNavigationMapSettings settings{.width = 4, .height = 1, .resolutionMeters = 1};
  EXPECT_EQ(rasterizeBimNavigationMap(geometry, support, {}, settings),
            (std::vector{Free, Occupied, Free, Occupied}));
}

TEST(BimNavigationMap, AccountsForCapsuleHeadroomAboveElevatedSupport) {
  std::vector<BimWalkableSurface> support;
  addFloor(support, {0, 0}, {4, 1}, .01);
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "low-overhang", .kind = BimColliderKind::DynamicBox,
      .originMeters = {1.5, .5, 1.608}, .halfExtents = {.1f, .1f, .004f}});
  geometry.colliders.push_back({.entityId = "high-overhang", .kind = BimColliderKind::DynamicBox,
      .originMeters = {2.5, .5, 1.63}, .halfExtents = {.1f, .1f, .004f}});
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{3.4f, .4f, 1.606f}, {3.6f, .4f, 1.606f}, {3.5f, .6f, 1.606f}};
  mesh->indices = {0, 1, 2};
  geometry.colliders.push_back({.entityId = "low-soffit", .mesh = mesh});
  const BimNavigationMapSettings settings{.width = 4, .height = 1, .resolutionMeters = 1};
  EXPECT_EQ(rasterizeBimNavigationMap(geometry, support, {}, settings),
            (std::vector{Free, Occupied, Free, Occupied}));
}

TEST(BimNavigationMap, ExcludesSelectedSupportByRepresentationAndTriangle) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{0, 0, 0}, {4, 0, .015f}, {4, 1, .015f}, {0, 1, 0},
                    {2.4f, .4f, .019f}, {2.6f, .4f, .019f}, {2.5f, .6f, .019f}};
  mesh->indices = {0, 1, 2, 0, 2, 3, 4, 5, 6};
  auto trim = std::make_shared<BimCollisionMesh>();
  trim->vertices = {{3.4f, .4f, .019f}, {3.6f, .4f, .019f}, {3.5f, .6f, .019f}};
  trim->indices = {0, 1, 2};
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "ramp", .representationId = "walkway",
                                .mesh = mesh, .supportCandidate = true});
  geometry.colliders.push_back({.entityId = "ramp", .representationId = "trim",
                                .mesh = trim, .supportCandidate = true});
  const glm::dvec3 normal{-0.015 / 4, 0, 1};
  const std::vector<BimWalkableSurface> support{
      {.entityId = "ramp", .representationId = "walkway", .triangleIndex = 0,
       .vertices = {{{0, 0, 0}, {4, 0, .015}, {4, 1, .015}}}, .normal = normal},
      {.entityId = "ramp", .representationId = "walkway", .triangleIndex = 1,
       .vertices = {{{0, 0, 0}, {4, 1, .015}, {0, 1, 0}}}, .normal = normal},
  };
  const BimNavigationMapSettings settings{.width = 4, .height = 1, .resolutionMeters = 1};
  EXPECT_EQ(rasterizeBimNavigationMap(geometry, support, {}, settings),
            (std::vector{Free, Free, Occupied, Occupied}));
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

TEST(BimNavigationMap, UnionsOverlappingClosedComponentsInOneMesh) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  const auto addShell = [&](float minX, float minY, float maxX, float maxY, bool capped) {
    const auto offset = static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.insert(mesh->vertices.end(), {
        {minX, minY, -1}, {maxX, minY, -1}, {maxX, maxY, -1}, {minX, maxY, -1},
        {minX, minY, 3}, {maxX, minY, 3}, {maxX, maxY, 3}, {minX, maxY, 3}});
    for (uint32_t index : {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
                           2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7})
      mesh->indices.push_back(offset + index);
    if (capped)
      for (uint32_t index : {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7})
        mesh->indices.push_back(offset + index);
  };
  addShell(2, 2, 8, 7, true);
  addShell(5, 3, 11, 8, true);
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "overlapping-shells", .mesh = mesh});
  std::vector<BimWalkableSurface> support;
  addFloor(support, {0, 0}, {18, 10});
  const BimNavigationMapSettings settings{.width = 18, .height = 10, .resolutionMeters = 1};
  constexpr size_t row = 4 * 18;

  const auto closed = rasterizeBimNavigationMap(geometry, support, {}, settings);
  ASSERT_EQ(closed.size(), 180u);
  EXPECT_EQ(closed[row + 3], Occupied);
  EXPECT_EQ(closed[row + 6], Occupied);
  EXPECT_EQ(closed[row + 9], Occupied);
  EXPECT_EQ(closed[row + 17], Free);

  // An unrelated open component still contributes its projected walls.
  // It neither fills itself nor prevents the closed components from filling.
  addShell(12, 2, 16, 7, false);
  const auto mixed = rasterizeBimNavigationMap(geometry, support, {}, settings);
  ASSERT_EQ(mixed.size(), closed.size());
  EXPECT_EQ(mixed[row + 6], Occupied);
  EXPECT_EQ(mixed[row + 12], Occupied);
  EXPECT_EQ(mixed[row + 14], Free);
  EXPECT_EQ(mixed[row + 17], Free);
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
