#include "Container/renderer/lighting/DdgiVolumeScroll.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using namespace container::renderer;

void expectMappedWorldNodes(const DdgiVolumeScrollPlan &plan) {
  const DdgiProbeVolume oldVolume(plan.previous), newVolume(plan.target);
  std::set<uint32_t> reused;
  std::vector<uint32_t> exposed;
  ASSERT_EQ(plan.previousIndexForNewProbe.size(), newVolume.probeCount());
  for (uint32_t next = 0; next < newVolume.probeCount(); ++next) {
    const auto old = plan.previousIndexForNewProbe[next];
    if (old == kDdgiNoPreviousProbe) {
      exposed.push_back(next);
      continue;
    }
    ASSERT_LT(old, oldVolume.probeCount());
    EXPECT_EQ(oldVolume.probePosition(old), newVolume.probePosition(next));
    EXPECT_TRUE(reused.insert(old).second);
  }
  EXPECT_EQ(exposed, plan.exposedProbeIndices);
  EXPECT_EQ(plan.clearsHistory, reused.empty());
}

TEST(DdgiVolumeScrollTests, StationaryCameraPreservesEveryProbe) {
  const DdgiProbeVolumeDesc desc{{-1, -2, -3}, {.5f, 1, 2}, {5, 5, 5}};
  const auto plan = planDdgiVolumeScroll(desc, {0, 0, 1});
  EXPECT_FALSE(plan.changed);
  EXPECT_FALSE(plan.clearsHistory);
  EXPECT_EQ(plan.target, desc);
  EXPECT_EQ(plan.cellShift, glm::i64vec3(0));
  EXPECT_TRUE(plan.exposedProbeIndices.empty());
  for (uint32_t index = 0; index < 125; ++index)
    EXPECT_EQ(plan.previousIndexForNewProbe[index], index);
}

TEST(DdgiVolumeScrollTests, SmallCameraMotionDoesNotJitterLattice) {
  const DdgiProbeVolumeDesc desc{{-1, -1, -1}, {1, 1, 1}, {3, 3, 3}};
  for (const glm::vec3 camera : {glm::vec3(.499f), glm::vec3(-.499f)}) {
    const auto plan = planDdgiVolumeScroll(desc, camera);
    EXPECT_FALSE(plan.changed);
    EXPECT_EQ(plan.target, desc);
    EXPECT_TRUE(plan.exposedProbeIndices.empty());
  }
}

TEST(DdgiVolumeScrollTests, HalfCellTiesRoundSymmetricallyAcrossZero) {
  const DdgiProbeVolumeDesc desc{{-1, -1, -1}, {1, 1, 1}, {3, 3, 3}};
  const auto positive = planDdgiVolumeScroll(desc, {.5f, 0, 0});
  const auto negative = planDdgiVolumeScroll(desc, {-.5f, 0, 0});
  EXPECT_EQ(positive.cellShift, glm::i64vec3(1, 0, 0));
  EXPECT_EQ(negative.cellShift, glm::i64vec3(-1, 0, 0));
  EXPECT_EQ(positive.target.origin, glm::vec3(0, -1, -1));
  EXPECT_EQ(negative.target.origin, glm::vec3(-2, -1, -1));
  EXPECT_EQ(positive.exposedProbeIndices.size(), 9u);
  EXPECT_EQ(negative.exposedProbeIndices.size(), 9u);
  expectMappedWorldNodes(positive);
  expectMappedWorldNodes(negative);
}

TEST(DdgiVolumeScrollTests, PositiveXShiftUsesExpectedXFastMapping) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {1, 1, 1}, {3, 2, 2}};
  const auto plan = planDdgiVolumeScroll(desc, {2, .5f, .5f});
  const uint32_t missing = kDdgiNoPreviousProbe;
  const std::vector<uint32_t> expected{1, 2, missing, 4, 5, missing,
                                       7, 8, missing, 10, 11, missing};
  EXPECT_EQ(plan.previousIndexForNewProbe, expected);
  EXPECT_EQ(plan.exposedProbeIndices, (std::vector<uint32_t>{2, 5, 8, 11}));
  expectMappedWorldNodes(plan);
}

TEST(DdgiVolumeScrollTests, NegativeXShiftExposesFirstColumn) {
  const DdgiProbeVolumeDesc desc{{-3, -2, -1}, {1, 1, 1}, {3, 2, 2}};
  const auto plan = planDdgiVolumeScroll(desc, {-3, -1.5f, -.5f});
  const uint32_t missing = kDdgiNoPreviousProbe;
  EXPECT_EQ(plan.cellShift, glm::i64vec3(-1, 0, 0));
  EXPECT_EQ(plan.previousIndexForNewProbe,
      (std::vector<uint32_t>{missing, 0, 1, missing, 3, 4,
                              missing, 6, 7, missing, 9, 10}));
  EXPECT_EQ(plan.exposedProbeIndices, (std::vector<uint32_t>{0, 3, 6, 9}));
  expectMappedWorldNodes(plan);
}

TEST(DdgiVolumeScrollTests, MixedAxisShiftPreservesExactOverlapCount) {
  const DdgiProbeVolumeDesc desc{{-2, 0, -4}, {.5f, 2, 4}, {5, 4, 3}};
  const auto plan = planDdgiVolumeScroll(desc, {-.5f, 1, 0});
  EXPECT_EQ(plan.cellShift, glm::i64vec3(1, -1, 0));
  EXPECT_EQ(plan.target.origin, glm::vec3(-1.5f, -2, -4));
  // 4 X columns x 3 Y rows x 3 Z layers overlap.
  EXPECT_EQ(plan.exposedProbeIndices.size(), 60u - 36u);
  expectMappedWorldNodes(plan);
}

TEST(DdgiVolumeScrollTests, EvenCountsCenterBetweenMiddleNodes) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {1, 1, 1}, {4, 4, 4}};
  EXPECT_FALSE(planDdgiVolumeScroll(desc, {1.99f, 1.01f, 1.5f}).changed);
  const auto plan = planDdgiVolumeScroll(desc, {2.f, 1.5f, 1.5f});
  EXPECT_EQ(plan.cellShift, glm::i64vec3(1, 0, 0));
  EXPECT_EQ(plan.exposedProbeIndices.size(), 16u);
}

TEST(DdgiVolumeScrollTests, SingletonAxesResetTheirOnlyLayerWhenMoved) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {1, 1, 1}, {1, 3, 1}};
  const auto plan = planDdgiVolumeScroll(desc, {1, 1, 0});
  EXPECT_TRUE(plan.changed);
  EXPECT_TRUE(plan.clearsHistory);
  EXPECT_EQ(plan.exposedProbeIndices.size(), 3u);
  expectMappedWorldNodes(plan);
}

TEST(DdgiVolumeScrollTests, TeleportClearsEveryProbeWithoutIndexOverflow) {
  const DdgiProbeVolumeDesc desc{{-1, -1, -1}, {.5f, .5f, .5f}, {5, 5, 5}};
  for (float destination : {-1000000.f, 1000000.f}) {
    const auto plan = planDdgiVolumeScroll(desc, {destination, 0, 0});
    EXPECT_TRUE(plan.changed);
    EXPECT_TRUE(plan.clearsHistory);
    EXPECT_EQ(plan.exposedProbeIndices.size(), 125u);
    EXPECT_TRUE(std::all_of(plan.previousIndexForNewProbe.begin(),
        plan.previousIndexForNewProbe.end(), [](uint32_t index) {
          return index == kDdgiNoPreviousProbe;
        }));
    expectMappedWorldNodes(plan);
  }
}

TEST(DdgiVolumeScrollTests, SuccessiveBinaryCellMovesRetainOriginalWorldNodes) {
  auto desc = DdgiProbeVolumeDesc{{-1, -1, -1}, {.25f, .25f, .25f}, {9, 9, 9}};
  for (int step = 1; step <= 16; ++step) {
    const auto plan = planDdgiVolumeScroll(desc, {.25f * step, 0, 0});
    EXPECT_EQ(plan.cellShift, glm::i64vec3(1, 0, 0));
    EXPECT_EQ(plan.exposedProbeIndices.size(), 81u);
    EXPECT_FALSE(plan.clearsHistory);
    expectMappedWorldNodes(plan);
    desc = plan.target;
  }
}

TEST(DdgiVolumeScrollTests, NonbinarySpacingNeverReusesRoundedDifferentWorldNode) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {.1f, .1f, .1f}, {9, 3, 3}};
  const auto plan = planDdgiVolumeScroll(desc, {.7f, .1f, .1f});
  EXPECT_TRUE(plan.changed);
  EXPECT_FALSE(plan.clearsHistory);
  EXPECT_GE(plan.exposedProbeIndices.size(), 27u);
  expectMappedWorldNodes(plan);
}

TEST(DdgiVolumeScrollTests, LargeResolvableCoordinatesRetainTheirHistory) {
  const DdgiProbeVolumeDesc desc{{100000000, 0, 0}, {8, 1, 1}, {5, 3, 3}};
  const auto plan = planDdgiVolumeScroll(desc, {100000024, 1, 1});
  EXPECT_EQ(plan.cellShift, glm::i64vec3(1, 0, 0));
  EXPECT_EQ(plan.exposedProbeIndices.size(), 9u);
  expectMappedWorldNodes(plan);
}

TEST(DdgiVolumeScrollTests, RejectsNonfiniteAndInt64OverflowingCameraLocations) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {1, 1, 1}, {3, 3, 3}};
  EXPECT_THROW((void)planDdgiVolumeScroll(desc,
      {std::numeric_limits<float>::quiet_NaN(), 0, 0}), std::invalid_argument);
  EXPECT_THROW((void)planDdgiVolumeScroll(desc,
      {std::numeric_limits<float>::infinity(), 0, 0}), std::invalid_argument);
  EXPECT_THROW((void)planDdgiVolumeScroll(desc,
      {std::numeric_limits<float>::max(), 0, 0}), std::invalid_argument);
  EXPECT_EQ(desc.origin, glm::vec3(0));
}

TEST(DdgiVolumeScrollTests, RejectsCollapsedLargeCoordinateTarget) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {1, 1, 1}, {3, 3, 3}};
  EXPECT_THROW((void)planDdgiVolumeScroll(desc, {100000000, 0, 0}), std::invalid_argument);
}

TEST(DdgiVolumeScrollTests, RejectsCpuWorkCapBeforeBuildingMapping) {
  const DdgiProbeVolumeDesc desc{{0, 0, 0}, {1, 1, 1}, {9, 9, 9}};
  EXPECT_THROW((void)planDdgiVolumeScroll(desc, {4, 4, 4}, 728), std::invalid_argument);
  EXPECT_NO_THROW((void)planDdgiVolumeScroll(desc, {4, 4, 4}, 729));
}

} // namespace
