#include "Container/renderer/lighting/DdgiVolumePlacement.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace container::renderer;

DdgiResourceLimits unlimitedExceptPayload(uint64_t budget) {
  return {std::numeric_limits<uint32_t>::max(),
          std::numeric_limits<uint64_t>::max(), budget};
}

void expectPadding(const DdgiDetailVolumePlacementResult &result,
                   const DdgiSceneBounds &bounds, double cells) {
  const DdgiProbeVolume volume(result.settings.additionalVolumes.back());
  EXPECT_EQ(result.boundsMin, volume.minimum());
  EXPECT_EQ(result.boundsMax, volume.maximum());
  for (int axis = 0; axis < 3; ++axis) {
    const double margin = cells * volume.description().spacing[axis];
    EXPECT_GE(double(bounds.minimum[axis]) - result.boundsMin[axis], margin);
    EXPECT_GE(double(result.boundsMax[axis]) - bounds.maximum[axis], margin);
  }
  EXPECT_EQ(result.addedProbeCount, volume.probeCount());
  EXPECT_EQ(result.totalProbeCount, DdgiVolumeSet(result.settings).probeCount());
  EXPECT_EQ(result.payloadBytes, planDdgiVolumeResources(result.settings,
      unlimitedExceptPayload(result.settings.payloadBudgetBytes)).payloadBytes);
  EXPECT_LE(result.payloadBytes, result.settings.payloadBudgetBytes);
  EXPECT_NO_THROW(validateDdgiSettings(result.settings));
}

TEST(DdgiVolumePlacementTests, AppendsWithoutChangingPrimaryOrExistingDetails) {
  DdgiSettings base;
  base.enabled = false;
  base.paused = true;
  base.intensity = 2.5f;
  base.resources.updates = {12, 64, 768};
  base.additionalVolumes.push_back({{2, 0, 1}, {.2f, .2f, .2f}, {3, 3, 3}});
  const auto original = base;
  const DdgiSceneBounds bounds{{-2, 0, -1}, {2, 3, 1}, true};
  const auto result = fitDdgiDetailVolume(base, bounds, {512, 4096});
  EXPECT_EQ(base, original);
  EXPECT_EQ(result.volumeIndex, 2u);
  EXPECT_EQ(result.settings.volume, base.volume);
  ASSERT_EQ(result.settings.additionalVolumes.size(), 2u);
  EXPECT_EQ(result.settings.additionalVolumes.front(), base.additionalVolumes.front());
  auto expected = base;
  expected.additionalVolumes.push_back(result.settings.additionalVolumes.back());
  expected.maxDistance = result.settings.maxDistance;
  EXPECT_EQ(result.settings, expected);
  EXPECT_FALSE(result.settings.enabled);
  EXPECT_TRUE(result.settings.paused);
  EXPECT_LE(result.addedProbeCount, 512u);
  expectPadding(result, bounds, 1);
}

TEST(DdgiVolumePlacementTests, PaddingCoversWideAndFractionalBlendBands) {
  const DdgiSceneBounds bounds{{-2, 1, -3}, {3, 4, 1}, true};
  for (float blend : {0.f, .5f, 1.f, 2.f, 3.25f}) {
    SCOPED_TRACE(blend);
    DdgiSettings base;
    base.volumeBlendCells = blend;
    const auto result = fitDdgiDetailVolume(base, bounds, {2048, 4096});
    expectPadding(result, bounds, std::max(1., double(blend)));
    EXPECT_EQ(result.settings.volumeBlendCells, blend);
  }
}

TEST(DdgiVolumePlacementTests, FlatAndPointSelectionsHaveFullOutsidePadding) {
  const DdgiSceneBounds selections[] = {
      {{1.25f, .25f, -.5f}, {1.25f, .25f, -.5f}, true},
      {{-4, .25f, -2}, {4, .25f, 2}, true},
      {{-4, .25f, -.5f}, {4, .25f, -.5f}, true}};
  for (const auto &bounds : selections) {
    const auto result = fitDdgiDetailVolume({}, bounds, {512, 4096});
    expectPadding(result, bounds, 1);
    EXPECT_GE(result.addedProbeCount, 64u);
    EXPECT_LE(result.addedProbeCount, 512u);
  }
}

TEST(DdgiVolumePlacementTests, TotalProbeCapReservesAllExistingLattices) {
  DdgiSettings base;
  base.additionalVolumes.push_back({{0, 0, 0}, {1, 1, 1}, {4, 4, 4}});
  const uint32_t before = DdgiVolumeSet(base).probeCount();
  const auto result = fitDdgiDetailVolume(base, {{0, 0, 0}, {1, 1, 1}, true},
                                         {2048, before + 125});
  EXPECT_LE(result.addedProbeCount, 125u);
  EXPECT_LE(result.totalProbeCount, before + 125);
  EXPECT_EQ(result.totalProbeCount, before + result.addedProbeCount);
}

TEST(DdgiVolumePlacementTests, RemainingPayloadCoarsensCandidateWithoutEvictingPrimary) {
  DdgiSettings base;
  base.resources.updates = {16, 64, 1024};
  const DdgiSceneBounds bounds{{0, 0, 0}, {1, 1, 1}, true};
  const auto unrestricted = fitDdgiDetailVolume(base, bounds, {512, 4096});
  auto minimum = base;
  minimum.additionalVolumes.push_back({{-1, -1, -1}, {1, 1, 1}, {4, 4, 4}});
  base.payloadBudgetBytes = planDdgiVolumeResources(
      minimum, unlimitedExceptPayload(base.payloadBudgetBytes)).payloadBytes + 1024;
  const auto result = fitDdgiDetailVolume(base, bounds, {512, 4096});
  EXPECT_LT(result.addedProbeCount, unrestricted.addedProbeCount);
  EXPECT_EQ(result.settings.volume, base.volume);
  expectPadding(result, bounds, 1);
}

TEST(DdgiVolumePlacementTests, DeterministicForNegativeWorldCoordinates) {
  const DdgiSceneBounds bounds{{-17.25f, -3.75f, -20.5f}, {-14.25f, .5f, -19.5f}, true};
  const auto first = fitDdgiDetailVolume({}, bounds, {512, 4096});
  const auto second = fitDdgiDetailVolume({}, bounds, {512, 4096});
  EXPECT_EQ(first.settings, second.settings);
  EXPECT_EQ(first.payloadBytes, second.payloadBytes);
  EXPECT_EQ(first.totalProbeCount, second.totalProbeCount);
  expectPadding(first, bounds, 1);
}

TEST(DdgiVolumePlacementTests, RaisesTraceDistanceOnlyWhenNewVolumeNeedsIt) {
  DdgiSettings base;
  base.maxDistance = 2.f;
  const auto result = fitDdgiDetailVolume(base, {{0, 0, 0}, {100, 20, 50}, true},
                                         {512, 4096});
  EXPECT_GT(result.settings.maxDistance, base.maxDistance);
  const DdgiProbeVolume volume(result.settings.additionalVolumes.back());
  double diagonalSquared = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const double length = double(volume.maximum()[axis]) - volume.minimum()[axis];
    diagonalSquared += length * length;
  }
  EXPECT_GE(double(result.settings.maxDistance), std::sqrt(diagonalSquared));
  EXPECT_EQ(result.settings.normalBias, base.normalBias);
  EXPECT_EQ(result.settings.viewBias, base.viewBias);
  EXPECT_EQ(result.settings.visibilityBias, base.visibilityBias);
  EXPECT_EQ(result.settings.debugProbeRadius, base.debugProbeRadius);
}

TEST(DdgiVolumePlacementTests, RetainsLargerExistingTraceDistance) {
  DdgiSettings base;
  base.maxDistance = 1000.f;
  base.enabled = true;
  const auto result = fitDdgiDetailVolume(base, {{0, 0, 0}, {1, 1, 1}, true});
  EXPECT_EQ(result.settings.maxDistance, 1000.f);
  EXPECT_TRUE(result.settings.enabled);
}

TEST(DdgiVolumePlacementTests, RejectsEmptyReversedAndNonfiniteSelectionBounds) {
  EXPECT_THROW((void)fitDdgiDetailVolume({}, {}), std::invalid_argument);
  EXPECT_THROW((void)fitDdgiDetailVolume({}, {{2, 0, 0}, {1, 1, 1}, true}), std::invalid_argument);
  EXPECT_THROW((void)fitDdgiDetailVolume({},
      {{0, 0, 0}, {1, std::numeric_limits<float>::infinity(), 1}, true}), std::invalid_argument);
}

TEST(DdgiVolumePlacementTests, RejectsFullVolumeSetWithoutReplacingExistingCoverage) {
  DdgiSettings base;
  base.additionalVolumes.assign(3, {{0, 0, 0}, {1, 1, 1}, {4, 4, 4}});
  const auto original = base;
  EXPECT_THROW((void)fitDdgiDetailVolume(base, {{0, 0, 0}, {1, 1, 1}, true}),
               std::invalid_argument);
  EXPECT_EQ(base, original);
}

TEST(DdgiVolumePlacementTests, RejectsExhaustedProbeAndBlendBandBudgets) {
  DdgiSettings base;
  const DdgiSceneBounds bounds{{0, 0, 0}, {1, 1, 1}, true};
  EXPECT_THROW((void)fitDdgiDetailVolume(base, bounds, {63, 4096}), std::invalid_argument);
  EXPECT_THROW((void)fitDdgiDetailVolume(base, bounds,
      {512, DdgiVolumeSet(base).probeCount()}), std::invalid_argument);
  base.volumeBlendCells = 3.f;
  EXPECT_THROW((void)fitDdgiDetailVolume(base, bounds, {511, 4096}), std::invalid_argument);
}

TEST(DdgiVolumePlacementTests, RejectsInsufficientSharedPayloadTransactionally) {
  DdgiSettings base;
  base.payloadBudgetBytes = planDdgiVolumeResources(
      base, unlimitedExceptPayload(base.payloadBudgetBytes)).payloadBytes;
  const auto original = base;
  EXPECT_THROW((void)fitDdgiDetailVolume(base, {{0, 0, 0}, {1, 1, 1}, true}),
               std::invalid_argument);
  EXPECT_EQ(base, original);
}

TEST(DdgiVolumePlacementTests, RejectsUnsupportedLargeSelectionDistance) {
  EXPECT_THROW((void)fitDdgiDetailVolume({},
      {{-500000, 0, 0}, {500000, 1, 1}, true}), std::invalid_argument);
}

TEST(DdgiVolumePlacementTests, LargeCoordinatesUseResolvableDistinctProbeCenters) {
  const DdgiSceneBounds bounds{{1000000, 2, -1000000},
                              {1000004, 4, -999998}, true};
  const auto result = fitDdgiDetailVolume({}, bounds, {512, 4096});
  const DdgiProbeVolume volume(result.settings.additionalVolumes.back());
  expectPadding(result, bounds, 1);
  for (int axis = 0; axis < 3; ++axis) {
    glm::uvec3 coordinate{0};
    coordinate[axis] = 1;
    EXPECT_GT(volume.probePosition(volume.linearIndex(coordinate))[axis],
              volume.probePosition(0)[axis]);
  }
}

} // namespace
