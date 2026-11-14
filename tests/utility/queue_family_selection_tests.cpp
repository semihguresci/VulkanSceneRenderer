#include "Container/utility/AsyncComputeQueue.h"
#include "Container/utility/QueueFamilySelection.h"

#include <gtest/gtest.h>

#include <array>

namespace {
using container::gpu::AsyncComputeQueuePreference;
using container::gpu::parseAsyncComputeQueuePreference;
using container::gpu::QueueFamilySupport;
using container::gpu::selectAsyncComputeQueue;
using container::gpu::selectQueueFamilies;

constexpr VkQueueFlags kGraphicsAndCompute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

TEST(QueueFamilySelection, UsesGraphicsAndComputeWithSeparatePresentation) {
  constexpr std::array families{
      QueueFamilySupport{VK_QUEUE_GRAPHICS_BIT, 1, true},
      QueueFamilySupport{VK_QUEUE_COMPUTE_BIT, 4, false},
      QueueFamilySupport{kGraphicsAndCompute, 1, false},
      QueueFamilySupport{VK_QUEUE_TRANSFER_BIT, 1, true},
      QueueFamilySupport{kGraphicsAndCompute, 2, false},
  };
  const auto selected = selectQueueFamilies(families);
  EXPECT_TRUE(selected.isComplete());
  EXPECT_EQ(selected.graphicsFamily, 2u);
  EXPECT_EQ(selected.presentFamily, 0u);
}

TEST(QueueFamilySelection, PrefersFirstCombinedFamilyOverEarlierSplitPair) {
  constexpr std::array families{
      QueueFamilySupport{VK_QUEUE_GRAPHICS_BIT, 1, true},
      QueueFamilySupport{kGraphicsAndCompute, 1, false},
      QueueFamilySupport{kGraphicsAndCompute | VK_QUEUE_TRANSFER_BIT, 1, true},
      QueueFamilySupport{kGraphicsAndCompute, 2, true},
  };
  const auto selected = selectQueueFamilies(families);
  EXPECT_TRUE(selected.isComplete());
  EXPECT_EQ(selected.graphicsFamily, 2u);
  EXPECT_EQ(selected.presentFamily, 2u);
}

TEST(QueueFamilySelection, RejectsGraphicsAndComputeInDifferentFamilies) {
  constexpr std::array families{
      QueueFamilySupport{VK_QUEUE_GRAPHICS_BIT, 2, true},
      QueueFamilySupport{VK_QUEUE_COMPUTE_BIT, 4, true},
  };
  const auto selected = selectQueueFamilies(families);
  EXPECT_FALSE(selected.isComplete());
  EXPECT_FALSE(selected.graphicsFamily.has_value());
  EXPECT_EQ(selected.presentFamily, 0u);
}

TEST(QueueFamilySelection, RequiresPresentationSupport) {
  constexpr std::array families{
      QueueFamilySupport{kGraphicsAndCompute, 1, false},
  };
  const auto selected = selectQueueFamilies(families);
  EXPECT_FALSE(selected.isComplete());
  EXPECT_EQ(selected.graphicsFamily, 0u);
  EXPECT_FALSE(selected.presentFamily.has_value());
}

TEST(QueueFamilySelection, IgnoresFamiliesWithoutQueues) {
  constexpr std::array families{
      QueueFamilySupport{kGraphicsAndCompute, 0, true},
      QueueFamilySupport{VK_QUEUE_GRAPHICS_BIT, 0, true},
      QueueFamilySupport{VK_QUEUE_TRANSFER_BIT, 1, true},
      QueueFamilySupport{kGraphicsAndCompute, 1, false},
  };
  const auto selected = selectQueueFamilies(families);
  EXPECT_TRUE(selected.isComplete());
  EXPECT_EQ(selected.graphicsFamily, 3u);
  EXPECT_EQ(selected.presentFamily, 2u);
}

TEST(QueueFamilySelection, EmptyListHasNoFamilies) {
  const auto selected = selectQueueFamilies({});
  EXPECT_FALSE(selected.isComplete());
  EXPECT_FALSE(selected.graphicsFamily.has_value());
  EXPECT_FALSE(selected.presentFamily.has_value());
}

TEST(AsyncComputeQueueSelection, PrefersFirstUsableComputeOnlyFamily) {
  constexpr std::array families{
      VkQueueFamilyProperties{kGraphicsAndCompute, 2, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT, 0, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_TRANSFER_BIT, 4, 64, {}},
      VkQueueFamilyProperties{kGraphicsAndCompute, 2, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT, 1, 0, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 8, 64, {}},
  };
  const auto selected = selectAsyncComputeQueue(families, {0, 0}, AsyncComputeQueuePreference::DedicatedFirst);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->familyIndex, 4u);
  EXPECT_EQ(selected->queueIndex, 0u);
  EXPECT_EQ(selected->timestampValidBits, 0u);
}

TEST(AsyncComputeQueueSelection, UsesSecondGraphicsQueueWithoutDedicatedCompute) {
  constexpr std::array families{
      VkQueueFamilyProperties{VK_QUEUE_TRANSFER_BIT, 2, 64, {}},
      VkQueueFamilyProperties{kGraphicsAndCompute, 3, 48, {}},
  };
  const auto selected = selectAsyncComputeQueue(families, {1, 1}, AsyncComputeQueuePreference::DedicatedFirst);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->familyIndex, 1u);
  EXPECT_EQ(selected->queueIndex, 1u);
  EXPECT_EQ(selected->timestampValidBits, 48u);
}

TEST(AsyncComputeQueueSelection, LeavesPresentationIndependent) {
  std::array families{
      VkQueueFamilyProperties{kGraphicsAndCompute, 2, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT, 2, 32, {}},
  };
  const auto dedicated = selectAsyncComputeQueue(families, {0, 1}, AsyncComputeQueuePreference::DedicatedFirst);
  ASSERT_TRUE(dedicated);
  EXPECT_EQ(dedicated->familyIndex, 1u);
  EXPECT_EQ(dedicated->queueIndex, 1u);

  families[1].queueCount = 1;
  const auto sameFamily = selectAsyncComputeQueue(families, {0, 1}, AsyncComputeQueuePreference::DedicatedFirst);
  ASSERT_TRUE(sameFamily);
  EXPECT_EQ(sameFamily->familyIndex, 0u);
  EXPECT_EQ(sameFamily->queueIndex, 1u);

  families[0].queueCount = 1;
  EXPECT_FALSE(selectAsyncComputeQueue(families, {0, 1}, AsyncComputeQueuePreference::DedicatedFirst));
}

TEST(AsyncComputeQueueSelection, AllowsAnEligibleSecondFamilyWithOnlyOneGraphicsQueue) {
  constexpr std::array families{
      VkQueueFamilyProperties{kGraphicsAndCompute, 1, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT, 1, 0, {}},
  };
  const auto selected = selectAsyncComputeQueue(families, {0, 0}, AsyncComputeQueuePreference::DedicatedFirst);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->familyIndex, 1u);
  EXPECT_EQ(selected->queueIndex, 0u);
  EXPECT_FALSE(selectAsyncComputeQueue(families, {0, 0}, AsyncComputeQueuePreference::SameFamilyOnly));
}

TEST(AsyncComputeQueueSelection, HonorsExplicitSameFamilyAndSerializedPreferences) {
  constexpr std::array families{
      VkQueueFamilyProperties{kGraphicsAndCompute, 2, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT, 2, 32, {}},
  };
  const auto sameFamily = selectAsyncComputeQueue(families, {0, 0}, AsyncComputeQueuePreference::SameFamilyOnly);
  ASSERT_TRUE(sameFamily);
  EXPECT_EQ(sameFamily->familyIndex, 0u);
  EXPECT_EQ(sameFamily->queueIndex, 1u);
  EXPECT_FALSE(selectAsyncComputeQueue(families, {0, 0}, AsyncComputeQueuePreference::Disabled));
}

TEST(AsyncComputeQueueSelection, RejectsMissingOrInvalidGraphicsFamily) {
  constexpr std::array families{
      VkQueueFamilyProperties{VK_QUEUE_GRAPHICS_BIT, 2, 64, {}},
      VkQueueFamilyProperties{kGraphicsAndCompute, 0, 64, {}},
      VkQueueFamilyProperties{VK_QUEUE_COMPUTE_BIT, 1, 32, {}},
  };
  constexpr auto preferred = AsyncComputeQueuePreference::DedicatedFirst;
  EXPECT_FALSE(selectAsyncComputeQueue(families, {}, preferred));
  EXPECT_FALSE(selectAsyncComputeQueue(families, {0, 0}, preferred));
  EXPECT_FALSE(selectAsyncComputeQueue(families, {1, 1}, preferred));
  EXPECT_FALSE(selectAsyncComputeQueue(families, {3, 3}, preferred));
}

TEST(AsyncComputeQueueSelection, ParsesRuntimeRoutePreferences) {
  EXPECT_EQ(parseAsyncComputeQueuePreference("auto"), AsyncComputeQueuePreference::DedicatedFirst);
  EXPECT_EQ(parseAsyncComputeQueuePreference("same-family"), AsyncComputeQueuePreference::SameFamilyOnly);
  EXPECT_EQ(parseAsyncComputeQueuePreference("serialized"), AsyncComputeQueuePreference::Disabled);
  EXPECT_FALSE(parseAsyncComputeQueuePreference("async"));
}
} // namespace
