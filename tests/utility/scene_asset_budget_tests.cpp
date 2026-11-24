#include "Container/utility/SceneAssetBudget.h"
#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using container::gpu::SceneAssetBudget;
using container::gpu::SceneAssetBudgetExceeded;
using container::gpu::SceneAssetBytes;

TEST(SceneAssetBudget, FailedReservationLeavesBothChargesAndPeaksIntact) {
  SceneAssetBudget budget({.residentBytes = 100, .uploadBytes = 32});
  auto live = budget.reserve({.residentBytes = 60, .uploadBytes = 16});

  EXPECT_THROW((void)budget.reserve({.residentBytes = 41}), SceneAssetBudgetExceeded);
  EXPECT_THROW((void)budget.reserve({.residentBytes = 40, .uploadBytes = 17}),
               SceneAssetBudgetExceeded);

  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{60, 16}));
  EXPECT_EQ(budget.snapshot().peak, (SceneAssetBytes{60, 16}));
  EXPECT_EQ(budget.snapshot().rejectedReservations, 2u);
  auto atLimit = budget.reserve({.residentBytes = 40, .uploadBytes = 16});
  EXPECT_EQ(budget.snapshot().used, budget.snapshot().limits);
}

TEST(SceneAssetBudget, PreflightChecksWithoutClaimingMemoryOrStaging) {
  SceneAssetBudget budget({.residentBytes = 100, .uploadBytes = 25});
  EXPECT_NO_THROW(budget.check({.residentBytes = 100, .uploadBytes = 25}));
  EXPECT_THROW(budget.check({.residentBytes = 100, .uploadBytes = 26}),
               SceneAssetBudgetExceeded);
  EXPECT_EQ(budget.snapshot().used, SceneAssetBytes{});
  EXPECT_EQ(budget.snapshot().peak, SceneAssetBytes{});
  EXPECT_EQ(budget.snapshot().rejectedReservations, 1u);
}

TEST(SceneAssetBudget, StagingRetiresBeforeItsResidentAllocation) {
  SceneAssetBudget budget({.residentBytes = 100, .uploadBytes = 24});
  auto upload = budget.reserve({.residentBytes = 60, .uploadBytes = 24});
  upload.releaseUpload();
  upload.releaseUpload();
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{60, 0}));
  {
    auto next = budget.reserve({.residentBytes = 40, .uploadBytes = 24});
    EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{100, 24}));
  }
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{60, 0}));
  upload.reset();
  upload.reset();
  EXPECT_EQ(budget.snapshot().used, SceneAssetBytes{});
  EXPECT_EQ(budget.snapshot().peak, (SceneAssetBytes{100, 24}));
}

TEST(SceneAssetBudget, FailedUploadUnwindsOnlyItsOwnReservation) {
  SceneAssetBudget budget({.residentBytes = 80, .uploadBytes = 24});
  auto active = budget.reserve({.residentBytes = 35});
  bool stagingStarted = false;
  const auto cancelledUpload = [&] {
    auto pending = budget.reserve({.residentBytes = 45, .uploadBytes = 20});
    stagingStarted = true;
    throw std::runtime_error("cancelled");
  };
  EXPECT_THROW(cancelledUpload(), std::runtime_error);
  EXPECT_TRUE(stagingStarted);
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{35, 0}));
  EXPECT_EQ(budget.snapshot().peak, (SceneAssetBytes{80, 20}));
  EXPECT_EQ(budget.snapshot().rejectedReservations, 0u);
}

TEST(SceneAssetBudget, MovingRetainedOwnershipConservesLiveBytes) {
  SceneAssetBudget budget;
  auto first = budget.reserve({.residentBytes = 7, .uploadBytes = 3});
  auto second = budget.reserve({.residentBytes = 12, .uploadBytes = 5});
  SceneAssetBudget::Reservation published(std::move(first));
  first.reset();
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{19, 8}));
  published = std::move(second);
  second.reset();
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{12, 5}));
  published.reset();
  EXPECT_EQ(budget.snapshot().used, SceneAssetBytes{});
}

TEST(SceneAssetBudget, RetainedAllocationCanRetireAfterItsBudgetOwner) {
  SceneAssetBudget::Reservation retained;
  {
    SceneAssetBudget budget;
    retained = budget.reserve({.residentBytes = 456, .uploadBytes = 89});
    retained.releaseUpload();
    EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{456, 0}));
  }
  retained.reset();
}

TEST(SceneAssetBudget, UncappedCountersRejectOverflowAndTrackExactLimits) {
  constexpr auto max = (std::numeric_limits<uint64_t>::max)();
  SceneAssetBudget budget;
  auto all = budget.reserve({.residentBytes = max, .uploadBytes = max});
  EXPECT_THROW((void)budget.reserve({.residentBytes = 1}), SceneAssetBudgetExceeded);
  EXPECT_THROW((void)budget.reserve({.uploadBytes = 1}), SceneAssetBudgetExceeded);
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{max, max}));
  EXPECT_EQ(budget.snapshot().peak, (SceneAssetBytes{max, max}));
  EXPECT_EQ(budget.snapshot().rejectedReservations, 2u);
}

TEST(SceneAssetBudget, ReconfigurationRequiresRetiringActiveResources) {
  SceneAssetBudget budget({.residentBytes = 100});
  auto old = budget.reserve({.residentBytes = 90});
  EXPECT_THROW(budget.reset({.residentBytes = 2}), std::logic_error);
  EXPECT_EQ(budget.snapshot().limits, (SceneAssetBytes{100, 0}));
  EXPECT_EQ(budget.snapshot().used, (SceneAssetBytes{90, 0}));
  old.reset();

  budget.reset({.residentBytes = 2, .uploadBytes = 4});
  EXPECT_EQ(budget.snapshot().limits, (SceneAssetBytes{2, 4}));
  EXPECT_EQ(budget.snapshot().used, SceneAssetBytes{});
  EXPECT_EQ(budget.snapshot().peak, SceneAssetBytes{});
  EXPECT_EQ(budget.snapshot().rejectedReservations, 0u);
}

} // namespace
