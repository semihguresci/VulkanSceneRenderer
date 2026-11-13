#include "Container/utility/QueueFamilySelection.h"

#include <gtest/gtest.h>

#include <array>

namespace {
using container::gpu::QueueFamilySupport;
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
} // namespace
