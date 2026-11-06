#include "Container/renderer/bim/BimMetadataIndex.h"

#include <gtest/gtest.h>

#include <limits>

namespace {

using container::renderer::BimElementMetadata;
using container::renderer::BimMetadataIndex;

TEST(BimMetadataIndexTests, IndexesGuidAndSourceId) {
  BimMetadataIndex index;
  index.reserve(3u);

  BimElementMetadata first{};
  first.objectIndex = 2u;
  first.guid = "guid-a";
  first.sourceId = "source-a";
  index.index(first);

  BimElementMetadata second{};
  second.objectIndex = 5u;
  second.guid = "guid-a";
  second.sourceId = "source-b";
  index.index(second);

  const auto guidMatches = index.objectIndicesForGuid("guid-a");
  ASSERT_EQ(guidMatches.size(), 2u);
  EXPECT_EQ(guidMatches[0], 2u);
  EXPECT_EQ(guidMatches[1], 5u);

  const auto sourceMatches = index.objectIndicesForSourceId("source-b");
  ASSERT_EQ(sourceMatches.size(), 1u);
  EXPECT_EQ(sourceMatches[0], 5u);
}

TEST(BimMetadataIndexTests, IgnoresInvalidAndEmptyIdentities) {
  BimMetadataIndex index;

  BimElementMetadata invalid{};
  invalid.objectIndex = std::numeric_limits<uint32_t>::max();
  invalid.guid = "ignored";
  invalid.sourceId = "ignored-source";
  index.index(invalid);

  BimElementMetadata empty{};
  empty.objectIndex = 1u;
  index.index(empty);

  EXPECT_TRUE(index.objectIndicesForGuid("ignored").empty());
  EXPECT_TRUE(index.objectIndicesForSourceId("ignored-source").empty());
  EXPECT_TRUE(index.objectIndicesForGuid("").empty());
  EXPECT_TRUE(index.objectIndicesForSourceId("").empty());
}

TEST(BimMetadataIndexTests, ResolvesCompactStorageAndOwnsIdentityKeys) {
  container::renderer::BimMetadataStorage storage;
  BimElementMetadata first{};
  first.objectIndex = 3u;
  first.guid = std::string("guid\0a", 6u);
  first.sourceId = "shared-source";
  storage.push_back(first);
  first.objectIndex = 7u;
  storage.push_back(first);
  storage.seal();

  BimMetadataIndex index;
  for (const auto element : storage.view()) {
    index.index(element);
  }
  storage.clear();

  const auto guidMatches = index.objectIndicesForGuid(
      std::string_view("guid\0a", 6u));
  ASSERT_EQ(guidMatches.size(), 2u);
  EXPECT_EQ(guidMatches[0], 3u);
  EXPECT_EQ(guidMatches[1], 7u);
  EXPECT_TRUE(index.objectIndicesForGuid("guid").empty());
  EXPECT_EQ(index.objectIndicesForSourceId("shared-source").size(), 2u);
}

} // namespace
