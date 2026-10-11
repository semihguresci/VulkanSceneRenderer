#include "Container/renderer/lighting/DdgiPriorityScheduler.h"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using namespace container::renderer;
std::vector<glm::vec3> positions(uint32_t count) {
  std::vector<glm::vec3> result(count);
  for (uint32_t i = 0; i < count; ++i) result[i] = {float(i), 0, 0};
  return result;
}
std::vector<uint32_t> indices(const DdgiPriorityPlan &plan) {
  std::vector<uint32_t> result;
  for (const auto &update : plan.updates) result.push_back(update.probeIndex);
  return result;
}

TEST(DdgiPrioritySchedulerTests, CompleteRaySetsRespectBothBudgetsAndAreUnique) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(17);
  auto p = positions(17);
  const auto plan = scheduler.plan(p, {7, 32, 160});
  EXPECT_EQ(plan.batch.probeCount, 5u);
  EXPECT_EQ(plan.batch.rayCount, 160u);
  std::set<uint32_t> unique;
  for (uint32_t slot = 0; slot < plan.updates.size(); ++slot) {
    EXPECT_TRUE(unique.insert(plan.updates[slot].probeIndex).second);
    EXPECT_EQ(plan.updates[slot].firstRay, slot * 32u);
    EXPECT_EQ(plan.updates[slot].rayCount, 32u);
  }
}

TEST(DdgiPrioritySchedulerTests, NoHintsPreservesContiguousSubmittedSweep) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(7);
  auto p = positions(7);
  for (uint32_t frame = 0; frame < 4; ++frame) {
    const auto plan = scheduler.plan(p, {2, 4, 8});
    EXPECT_EQ(plan.batch.firstProbe, frame * 2 % 7);
    EXPECT_EQ(indices(plan), (std::vector<uint32_t>{frame * 2 % 7,
                                                   (frame * 2 + 1) % 7}));
    scheduler.commit(plan);
  }
  EXPECT_EQ(scheduler.refreshRemaining(), 0u);
}

TEST(DdgiPrioritySchedulerTests, PlanningAndPausedBudgetsDoNotConsumeDirtiness) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(9);
  const auto p = positions(9);
  const auto first = scheduler.plan(p, {4, 8, 32});
  EXPECT_EQ(indices(scheduler.plan(p, {4, 8, 32})), indices(first));
  for (const auto budget : {DdgiUpdateBudget{0, 8, 32},
                            DdgiUpdateBudget{4, 8, 0},
                            DdgiUpdateBudget{4, 8, 7}}) {
    const auto paused = scheduler.plan(p, budget);
    EXPECT_TRUE(paused.updates.empty());
    scheduler.commit(paused);
    EXPECT_EQ(scheduler.nextProbe(), 0u);
    EXPECT_EQ(scheduler.refreshRemaining(), 9u);
  }
  scheduler.commit(first);
  EXPECT_EQ(scheduler.refreshRemaining(), 5u);
}

TEST(DdgiPrioritySchedulerTests, ChangedRegionPrioritizesRemoteAffectedProbe) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(12);
  const auto p = positions(12);
  scheduler.commit(scheduler.plan(p, {12, 1, 12}));
  const std::array regions{DdgiDirtyRegion{{10, -1, -1}, {11, 1, 1}}};
  scheduler.markDirty(p, regions, 7);
  EXPECT_EQ(scheduler.refreshRemaining(), 2u);
  const auto plan = scheduler.plan(p, {4, 1, 4});
  EXPECT_EQ(indices(plan), (std::vector<uint32_t>{0, 1, 10, 11}));
  scheduler.commit(plan);
  EXPECT_EQ(scheduler.refreshRemaining(), 0u);
}

TEST(DdgiPrioritySchedulerTests, RepeatedRevisionDoesNotRedirtySubmittedProbes) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(6);
  const auto p = positions(6);
  scheduler.markDirty(p, {}, 44);
  scheduler.commit(scheduler.plan(p, {6, 1, 6}));
  scheduler.markDirty(p, {}, 44);
  EXPECT_EQ(scheduler.refreshRemaining(), 0u);
  scheduler.markDirty(p, {}, 45);
  EXPECT_EQ(scheduler.refreshRemaining(), 6u);
}

TEST(DdgiPrioritySchedulerTests, CameraPrioritizesForwardNearbyStaleSamples) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(10);
  auto p = positions(10);
  p[8] = {0, 0, -1}; p[9] = {0, 0, -2};
  DdgiSchedulingHints hints;
  hints.cameraPosition = glm::vec3{0, 0, 0};
  const auto plan = scheduler.plan(p, {4, 1, 4}, hints);
  EXPECT_EQ(indices(plan), (std::vector<uint32_t>{0, 1, 8, 9}));
}

TEST(DdgiPrioritySchedulerTests, ContinuallyChangingNearRegionCannotStarveSweep) {
  DdgiPriorityScheduler scheduler;
  constexpr uint32_t count = 37;
  scheduler.reset(count);
  const auto p = positions(count);
  DdgiSchedulingHints hints;
  hints.cameraPosition = glm::vec3{36, 0, 0};
  hints.cameraForward = {1, 0, 0};
  hints.dirtyRegions = {{{35, -1, -1}, {36, 1, 1}}};
  std::set<uint32_t> refreshed;
  // Six budget slots reserve three fairness slots. Every probe must receive
  // a complete submitted ray set within ceil(count / 3) submissions.
  for (uint32_t frame = 0; frame < (count + 2) / 3; ++frame) {
    scheduler.markDirty(p, hints.dirtyRegions, frame + 1);
    const auto plan = scheduler.plan(p, {6, 8, 48}, hints);
    for (const auto &update : plan.updates) refreshed.insert(update.probeIndex);
    scheduler.commit(plan);
  }
  EXPECT_EQ(refreshed.size(), count);
}

TEST(DdgiPrioritySchedulerTests, OneProbeBudgetAlwaysAdvancesFairness) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(5);
  const auto p = positions(5);
  DdgiSchedulingHints hints;
  hints.cameraPosition = p.back();
  for (uint32_t frame = 0; frame < 10; ++frame) {
    scheduler.markDirty(p, {}, frame + 1);
    const auto plan = scheduler.plan(p, {1, 64, 64}, hints);
    EXPECT_EQ(indices(plan), (std::vector<uint32_t>{frame % 5}));
    scheduler.commit(plan);
  }
}

TEST(DdgiPrioritySchedulerTests, NewDirtyRevisionAfterPlanningSurvivesCommit) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(6);
  const auto p = positions(6);
  const auto plan = scheduler.plan(p, {3, 1, 3});
  const std::array region{DdgiDirtyRegion{{0, -1, -1}, {0, 1, 1}}};
  scheduler.markDirty(p, region, 1);
  scheduler.commit(plan);
  EXPECT_EQ(scheduler.refreshRemaining(), 4u);
}

TEST(DdgiPrioritySchedulerTests, StaleDuplicateAndInvalidCommitsAreAtomic) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(8);
  const auto p = positions(8);
  const auto plan = scheduler.plan(p, {4, 1, 4});
  auto malformed = plan;
  malformed.updates.back().probeIndex = malformed.updates.front().probeIndex;
  EXPECT_THROW(scheduler.commit(malformed), std::invalid_argument);
  EXPECT_EQ(scheduler.refreshRemaining(), 8u);
  EXPECT_EQ(scheduler.nextProbe(), 0u);
  scheduler.commit(plan);
  EXPECT_THROW(scheduler.commit(plan), std::invalid_argument);
  scheduler.reset(8);
  EXPECT_THROW(scheduler.commit(plan), std::invalid_argument);
}

TEST(DdgiPrioritySchedulerTests, InvalidPositionsRegionsAndCameraFailBeforeMutation) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(2);
  auto p = positions(2);
  EXPECT_THROW((void)scheduler.plan(std::span<const glm::vec3>(p).first(1), {}),
               std::invalid_argument);
  const std::array reversed{DdgiDirtyRegion{{1, 0, 0}, {0, 1, 1}}};
  EXPECT_THROW(scheduler.markDirty(p, reversed, 1), std::invalid_argument);
  DdgiSchedulingHints hints;
  hints.cameraPosition = glm::vec3{std::numeric_limits<float>::infinity(), 0, 0};
  EXPECT_THROW((void)scheduler.plan(p, {}, hints), std::invalid_argument);
  p[0].z = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)scheduler.plan(p, {}), std::invalid_argument);
  EXPECT_EQ(scheduler.refreshRemaining(), 2u);
  EXPECT_EQ(scheduler.nextProbe(), 0u);
}

TEST(DdgiPrioritySchedulerTests, EmptyProbeSetIsValidAndDoesNotSubmit) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(0);
  const auto plan = scheduler.plan({}, {});
  EXPECT_TRUE(plan.updates.empty());
  EXPECT_EQ(plan.batch.probeCount, 0u);
  EXPECT_NO_THROW(scheduler.commit(plan));
}
TEST(DdgiPrioritySchedulerTests, ScrollPreservesFairnessAndMarksOnlyNewNodesDirty) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(6);
  const auto p = positions(6);
  scheduler.commit(scheduler.plan(p, {6, 1, 6}));
  scheduler.commit(scheduler.plan(p, {2, 1, 2}));
  const auto cursor = scheduler.nextProbe();
  const std::array mapping{1u, 2u, 3u, 4u, 5u, UINT32_MAX};
  scheduler.remap(mapping);
  EXPECT_EQ(scheduler.nextProbe(), cursor);
  EXPECT_EQ(scheduler.refreshRemaining(), 1u);
  const auto plan = scheduler.plan(p, {2, 1, 2});
  EXPECT_EQ(plan.updates.front().probeIndex, cursor);
  EXPECT_EQ(plan.updates.back().probeIndex, 5u);
}
TEST(DdgiPrioritySchedulerTests, ScrollInvalidatesOldPlanWithoutPartialMutation) {
  DdgiPriorityScheduler scheduler;
  scheduler.reset(3);
  const auto p = positions(3);
  const auto oldPlan = scheduler.plan(p, {1, 1, 1});
  const std::array bad{0u, 3u, UINT32_MAX};
  EXPECT_THROW(scheduler.remap(bad), std::invalid_argument);
  EXPECT_NO_THROW(scheduler.commit(oldPlan));
  const auto before = scheduler.nextProbe();
  const auto stale = scheduler.plan(p, {1, 1, 1});
  const std::array valid{1u, 2u, UINT32_MAX};
  scheduler.remap(valid);
  EXPECT_EQ(scheduler.nextProbe(), before);
  EXPECT_THROW(scheduler.commit(stale), std::invalid_argument);
}
TEST(DdgiPrioritySchedulerTests, ContinuousScrollDoesNotRestartFairSweep) {
  DdgiPriorityScheduler scheduler;
  constexpr uint32_t count = 9;
  scheduler.reset(count);
  const auto p = positions(count);
  std::array<uint32_t, count> mapping{};
  for (uint32_t index = 0; index < count; ++index)
    mapping[index] = index + 1 == count ? UINT32_MAX : index + 1;
  std::set<uint32_t> visited;
  for (uint32_t frame = 0; frame < count; ++frame) {
    scheduler.remap(mapping);
    const auto plan = scheduler.plan(p, {1, 1, 1});
    visited.insert(plan.updates.front().probeIndex);
    scheduler.commit(plan);
  }
  EXPECT_EQ(visited.size(), count);
}
} // namespace
