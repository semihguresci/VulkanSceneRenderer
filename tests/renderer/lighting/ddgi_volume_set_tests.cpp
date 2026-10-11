#include "Container/renderer/lighting/DdgiVolumeSet.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace {
using namespace container::renderer;
DdgiSettings nested() {
  DdgiSettings s;
  s.volume = {{-4,-4,-4}, {2,2,2}, {5,5,5}};
  s.additionalVolumes.push_back({{-1,-1,-1}, {.5f,.5f,.5f}, {5,5,5}});
  return s;
}
TEST(DdgiVolumeSet, PackedOffsetsPreserveRoundedPositionsAndConfigurationOrder) {
  const auto s = nested();
  const DdgiVolumeSet set(s);
  EXPECT_EQ(set.volumeCount(), 2u);
  EXPECT_EQ(set.probeCount(), 250u);
  EXPECT_EQ(set.probeOffset(1), 125u);
  EXPECT_EQ(set.probePosition(125), glm::vec3(-1));
  EXPECT_EQ(set.probePosition(249), glm::vec3(1));
  EXPECT_EQ(set.positions()[126], set.volume(1).probePosition(1));
  EXPECT_THROW(set.probePosition(250), std::out_of_range);
  EXPECT_THROW(set.volume(2), std::out_of_range);
}
TEST(DdgiVolumeSet, FinestInteriorOverridesCoarseAndEdgesBlendSmoothly) {
  const DdgiVolumeSet set(nested());
  EXPECT_FLOAT_EQ(set.blend({0,0,0}).weights[1], 1.f);
  EXPECT_FLOAT_EQ(set.blend({-1,0,0}).weights[0], 1.f);
  const auto half = set.blend({-.75f,0,0});
  EXPECT_NEAR(half.weights[0], .5f, 1e-6f);
  EXPECT_NEAR(half.weights[1], .5f, 1e-6f);
  const float e = 1e-5f;
  EXPECT_LT(set.blend({-1+e,0,0}).weights[1], 1e-6f);
  EXPECT_GT(set.blend({-.5f-e,0,0}).weights[1], 1.f-1e-6f);
}
TEST(DdgiVolumeSet, NoCoarserSupportKeepsFineBoundaryValid) {
  const DdgiVolumeSet set(nested());
  const std::array available{false, true};
  EXPECT_FLOAT_EQ(set.blend({-1,0,0}, available).weights[1], 1.f);
  const std::array noFine{true, false};
  EXPECT_FLOAT_EQ(set.blend({0,0,0}, noFine).weights[0], 1.f);
  EXPECT_THROW(set.blend({0,0,0}, std::span<const bool>(available.data(),1)), std::invalid_argument);
}
TEST(DdgiVolumeSet, DisjointAndOutsideVolumesHaveNoAccidentalCoverage) {
  auto s = nested();
  s.additionalVolumes[0].origin = {10,10,10};
  const DdgiVolumeSet set(s);
  EXPECT_FLOAT_EQ(set.blend({11,11,11}).weights[1], 1.f);
  const auto outside = set.blend({8,0,0});
  EXPECT_FLOAT_EQ(std::accumulate(outside.weights.begin(), outside.weights.end(),0.f), 0.f);
  EXPECT_FLOAT_EQ(set.blend({NAN,0,0}).weights[0],0.f);
}
TEST(DdgiVolumeSet, PrimaryFineResolutionAndStableTiesWorkInEitherOrder) {
  auto s = nested();
  std::swap(s.volume, s.additionalVolumes[0]);
  EXPECT_FLOAT_EQ(DdgiVolumeSet(s).blend({0,0,0}).weights[0], 1.f);
  s.additionalVolumes[0] = s.volume;
  EXPECT_FLOAT_EQ(DdgiVolumeSet(s).blend({0,0,0}).weights[0], 1.f);
}
TEST(DdgiVolumeSet, SingletonAxesDoNotEraseBlendCoverage) {
  auto s = nested();
  s.volume = {{-2,0,0},{2,2,2},{3,1,1}};
  s.additionalVolumes[0] = {{-1,0,0},{.5f,.5f,.5f},{5,1,1}};
  EXPECT_FLOAT_EQ(DdgiVolumeSet(s).blend({0,0,0}).weights[1],1.f);
}
TEST(DdgiVolumeSet, ZeroBlendWidthExplicitlySelectsFineAtBoundary) {
  auto s = nested(); s.volumeBlendCells=0;
  EXPECT_FLOAT_EQ(DdgiVolumeSet(s).blend({-1,0,0}).weights[1],1.f);
}
TEST(DdgiVolumeSet, FourVolumesAllowedAndInvalidCountsBlendRejected) {
  auto s = nested();
  s.additionalVolumes.resize(3,s.additionalVolumes[0]);
  EXPECT_NO_THROW(validateDdgiSettings(s));
  s.additionalVolumes.push_back(s.volume);
  EXPECT_THROW(validateDdgiSettings(s),std::invalid_argument);
  s = nested(); s.additionalVolumes[0].counts.x=0;
  EXPECT_THROW(validateDdgiSettings(s),std::invalid_argument);
  s = nested(); s.volumeBlendCells=std::numeric_limits<float>::infinity();
  EXPECT_THROW(validateDdgiSettings(s),std::invalid_argument);
  s.volumeBlendCells=-.01f;
  EXPECT_THROW(validateDdgiSettings(s),std::invalid_argument);
}
TEST(DdgiVolumeSet, CombinedPayloadAndOneFrameBudgetAreEnforced) {
  auto s = nested();
  s.resources.updates = {32,64,1024};
  const auto plan = planDdgiVolumeResources(s);
  EXPECT_EQ(plan.probeStateBytes,250u*sizeof(DdgiProbeState));
  EXPECT_EQ(plan.updateCapacity.probeCount,16u);
  EXPECT_EQ(plan.updateCapacity.rayCount,1024u);
  auto single = s; single.additionalVolumes.clear();
  const auto oldPlan = planDdgiVolumeResources(single);
  EXPECT_GT(plan.payloadBytes,oldPlan.payloadBytes);
  s.payloadBudgetBytes=oldPlan.payloadBytes;
  EXPECT_THROW(validateDdgiSettings(s),std::invalid_argument);
  EXPECT_THROW(planDdgiVolumeResources(s),std::invalid_argument);
}
TEST(DdgiVolumeSet, PerVolumeBiasesScaleWithCellsAndRespectTraceDistance) {
  auto s = nested(); s.normalBias=.08f; s.viewBias=.04f; s.visibilityBias=.12f;
  const auto gpu = DdgiVolumeSet(s).gpuVolumes(s);
  EXPECT_EQ(gpu[1].countsOffset,glm::uvec4(5,5,5,125));
  EXPECT_FLOAT_EQ(gpu[1].spacingNormalBias.w,.02f);
  EXPECT_FLOAT_EQ(gpu[1].bias.x,.01f);
  EXPECT_FLOAT_EQ(gpu[1].bias.y,.03f);
  EXPECT_FLOAT_EQ(gpu[2].spacingNormalBias.w,0.f);
  s.maxDistance=.01f;
  EXPECT_FLOAT_EQ(DdgiVolumeSet(s).gpuVolumes(s)[0].spacingNormalBias.w,.01f);
}
TEST(DdgiVolumeSet, PackedCountOverflowRejectedBeforeAllocation) {
  auto s = nested();
  s.volume = {{0,0,0},{1,1,1},{65535,65535,1}};
  s.additionalVolumes[0] = s.volume;
  EXPECT_THROW((void)DdgiVolumeSet(s),std::invalid_argument);
}
} // namespace
