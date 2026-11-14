#include "Container/renderer/bim/BimHardClash.h"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {

BimHardClashElement Box(uint32_t id, uint8_t groups, glm::dvec3 min, glm::dvec3 max) {
  return {.objectIndex = id,
          .groups = groups,
          .vertices = {{min.x, min.y, min.z}, {max.x, min.y, min.z},
                       {max.x, max.y, min.z}, {min.x, max.y, min.z},
                       {min.x, min.y, max.z}, {max.x, min.y, max.z},
                       {max.x, max.y, max.z}, {min.x, max.y, max.z}},
          .triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                        {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
                        {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}}};
}

BimHardClashElement Tetrahedron(uint32_t id, uint8_t groups) {
  return {.objectIndex = id,
          .groups = groups,
          .vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
          .triangles = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}};
}

BimHardClashElement ReflectedTetrahedron(glm::dvec3 shift) {
  auto element = Tetrahedron(2, kBimHardClashGroupB);
  for (auto& point : element.vertices) {
    point = shift - point;
  }
  for (auto& face : element.triangles) {
    std::swap(face[1], face[2]);
  }
  return element;
}

TEST(BimHardClash, MeasuresPenetrationAndFramesBothElements) {
  const auto first = Box(5, kBimHardClashGroupA, {0, 0, 0}, {2, 2, 2});
  const auto second = Box(2, kBimHardClashGroupB, {1, 0, 0}, {3, 2, 2});
  const auto third = Box(9, kBimHardClashGroupA, {2.1, 0, 0}, {2.9, 2, 2});
  const auto result = FindBimHardClashes(std::array{first, second, third});
  EXPECT_EQ(result.checkedPairs, 2u);
  EXPECT_TRUE(result.skipped.empty());
  EXPECT_TRUE(result.failedPairs.empty());
  ASSERT_EQ(result.hits.size(), 2u);
  const auto& hit = result.hits[0];
  EXPECT_EQ(hit.firstObjectIndex, 2u);
  EXPECT_EQ(hit.secondObjectIndex, 5u);
  EXPECT_DOUBLE_EQ(hit.volume, 4.0);
  EXPECT_EQ(hit.position, (glm::dvec3{1.5, 1, 1}));
  EXPECT_EQ(hit.boundsMin, (glm::dvec3{0, 0, 0}));
  EXPECT_EQ(hit.boundsMax, (glm::dvec3{3, 2, 2}));
  EXPECT_EQ(result.hits[1].secondObjectIndex, 9u);
}

TEST(BimHardClash, CountsEachGroupOverlapPairOnceAndHandlesContainment) {
  constexpr uint8_t both = kBimHardClashGroupA | kBimHardClashGroupB;
  const glm::dvec3 origin{1e9, -1e9, 1e9};
  const auto first = Box(5, both, origin, origin + 5.0);
  const auto second = Box(2, both, origin + 2.0, origin + 3.0);
  const auto result = FindBimHardClashes(std::array{first, second});
  EXPECT_EQ(result.checkedPairs, 1u);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_DOUBLE_EQ(result.hits[0].volume, 1.0);
  EXPECT_EQ(result.hits[0].position, origin + 2.5);
}

TEST(BimHardClash, IgnoresTouchingCoplanarSlantedFaces) {
  const auto first = Tetrahedron(1, kBimHardClashGroupA);
  const auto second = ReflectedTetrahedron({0.5, 0.75, 0.75});
  const auto result = FindBimHardClashes(std::array{first, second});
  EXPECT_EQ(result.checkedPairs, 1u);
  EXPECT_TRUE(result.hits.empty());
  EXPECT_TRUE(result.failedPairs.empty());
  EXPECT_TRUE(result.skipped.empty());
}

TEST(BimHardClash, IgnoresDisjointSolidsEvenWhenTheirBoundsOverlap) {
  const auto first = Tetrahedron(1, kBimHardClashGroupA);
  const auto second = ReflectedTetrahedron({1, 1, 1});
  const auto result = FindBimHardClashes(std::array{first, second});
  EXPECT_EQ(result.checkedPairs, 1u);
  EXPECT_TRUE(result.hits.empty());
  EXPECT_TRUE(result.failedPairs.empty());
}

TEST(BimHardClash, ExcludesBoundsTouchAndSameGroup) {
  const auto first = Box(1, kBimHardClashGroupA, {0, 0, 0}, {1, 1, 1});
  const auto sameGroup = Box(2, kBimHardClashGroupA, {0.5, 0, 0}, {1, 1, 1});
  const auto touching = Box(3, kBimHardClashGroupB, {1, 0, 0}, {2, 1, 1});
  const auto result = FindBimHardClashes(std::array{first, sameGroup, touching});
  EXPECT_EQ(result.checkedPairs, 0u);
  EXPECT_TRUE(result.hits.empty());
}

TEST(BimHardClash, ReportsSmallPositiveOverlaps) {
  constexpr double depth = 1e-7;
  const auto first = Box(1, kBimHardClashGroupA, {0, 0, 0}, {1, 1, 1});
  const auto second = Box(2, kBimHardClashGroupB, {1 - depth, 0, 0}, {2, 1, 1});
  const auto result = FindBimHardClashes(std::array{first, second});
  EXPECT_EQ(result.checkedPairs, 1u);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_NEAR(result.hits[0].volume, depth, 1e-14);
}

TEST(BimHardClash, ReadsWeldedSeamsAndReportsOpenOrInvalidInputs) {
  const auto first = Box(1, kBimHardClashGroupA, {0, 0, 0}, {2, 2, 2});
  auto split = Box(2, kBimHardClashGroupB, {1, 0, 0}, {3, 2, 2});
  const auto originalVertices = std::move(split.vertices);
  split.vertices.clear();
  for (auto& triangle : split.triangles) {
    for (auto& index : triangle) {
      split.vertices.push_back(originalVertices[index]);
      index = static_cast<uint32_t>(split.vertices.size() - 1);
    }
  }
  auto open = split;
  open.objectIndex = 3;
  open.triangles.pop_back();
  auto invalid = split;
  invalid.objectIndex = 4;
  invalid.vertices[0].x = std::numeric_limits<double>::infinity();
  const auto result = FindBimHardClashes(std::array{first, split, open, invalid});
  EXPECT_EQ(result.checkedPairs, 1u);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_DOUBLE_EQ(result.hits[0].volume, 4.0);
  ASSERT_EQ(result.skipped.size(), 2u);
  EXPECT_EQ(result.skipped[0].objectIndex, 3u);
  EXPECT_EQ(result.skipped[1].objectIndex, 4u);
  EXPECT_TRUE(result.failedPairs.empty());
}

TEST(BimHardClash, ReportsGeometryAndPairLimits) {
  const auto first = Box(1, kBimHardClashGroupA, {0, 0, 0}, {2, 2, 2});
  const auto second = Box(2, kBimHardClashGroupB, {1, 0, 0}, {3, 2, 2});
  auto third = second;
  third.objectIndex = 3;
  BimHardClashLimits limits;
  limits.maxBooleanPairs = 1;
  auto result = FindBimHardClashes(std::array{third, second, first}, {}, limits);
  EXPECT_TRUE(result.pairLimitReached);
  EXPECT_EQ(result.checkedPairs, 1u);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].secondObjectIndex, 2u);
  limits.maxTriangles = 24;
  result = FindBimHardClashes(std::array{third, second, first}, {}, limits);
  EXPECT_FALSE(result.pairLimitReached);
  ASSERT_EQ(result.skipped.size(), 1u);
  EXPECT_EQ(result.skipped[0].objectIndex, 3u);
}

TEST(BimHardClash, SameGroupOverlapsDoNotConsumeCandidateLimit) {
  const auto first = Box(1, kBimHardClashGroupA, {0, 0, 0}, {2, 2, 2});
  const auto second = Box(2, kBimHardClashGroupA, {0.5, 0, 0}, {1.5, 1, 1});
  const auto third = Box(3, kBimHardClashGroupB, {1.75, 0, 0}, {3, 2, 2});
  BimHardClashLimits limits;
  limits.maxCandidateTests = 1;
  const auto result = FindBimHardClashes(std::array{first, second, third}, {}, limits);
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].secondObjectIndex, 3u);
  EXPECT_FALSE(result.pairLimitReached);
}

TEST(BimHardClash, PreservesSnapshotSkipReasons) {
  const std::array elements{
      BimHardClashElement{.objectIndex = 4,
                          .groups = kBimHardClashGroupB,
                          .unavailableReason = "No source triangle geometry"}};
  const auto result = FindBimHardClashes(elements);
  ASSERT_EQ(result.skipped.size(), 1u);
  EXPECT_EQ(result.skipped[0].objectIndex, 4u);
  EXPECT_EQ(result.skipped[0].reason, elements[0].unavailableReason);
  EXPECT_EQ(result.checkedPairs, 0u);
  EXPECT_TRUE(result.failedPairs.empty());
}

TEST(BimHardClash, RejectsAmbiguousSnapshotsAndStopsCancelledRuns) {
  const auto first = Box(1, kBimHardClashGroupA, {0, 0, 0}, {2, 2, 2});
  const std::array duplicate{first, first};
  EXPECT_THROW((void)FindBimHardClashes(duplicate), std::invalid_argument);
  std::stop_source stop;
  stop.request_stop();
  const auto result = FindBimHardClashes(duplicate, stop.get_token());
  EXPECT_TRUE(result.cancelled);
  EXPECT_TRUE(result.hits.empty());
  EXPECT_TRUE(result.skipped.empty());
  EXPECT_TRUE(result.failedPairs.empty());
  EXPECT_EQ(result.checkedPairs, 0u);
}

} // namespace
} // namespace container::renderer
