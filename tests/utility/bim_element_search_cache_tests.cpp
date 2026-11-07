#include "Container/utility/BimElementSearchCache.h"

#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace {

using container::renderer::BimElementMetadata;
using container::ui::BimElementSearchCache;

BimElementMetadata element(std::string guid = "wall-1") {
  BimElementMetadata result{};
  result.guid = std::move(guid);
  result.sourceId = "Source-27";
  result.type = "IfcWall";
  result.displayName = "Exterior Wall";
  result.storeyName = "Level 01";
  result.materialName = "Concrete";
  return result;
}

TEST(BimElementSearchCacheTests,
     PreservesAllRowsAndTheFinalMatchPastTenThousand) {
  int model{};
  std::vector<BimElementMetadata> elements(10'001);
  for (size_t index = 0; index < elements.size(); ++index) {
    elements[index].objectIndex = static_cast<uint32_t>(index + 100u);
    elements[index].guid = "object-" + std::to_string(index);
  }
  elements.back().displayName = "Only final row";
  BimElementSearchCache cache;

  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  auto matches = cache.matches("");
  ASSERT_EQ(matches.size(), elements.size());
  EXPECT_EQ(matches.front(), 0u);
  EXPECT_EQ(matches.back(), 10'000u);
  for (size_t index = 0; index < matches.size(); ++index)
    EXPECT_EQ(matches[index], index);

  matches = cache.matches("ONLY FINAL ROW");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches.front(), 10'000u);
  EXPECT_EQ(elements[matches.front()].objectIndex, 10'100u);
}

TEST(BimElementSearchCacheTests, SearchesAllSixFieldsCaseInsensitively) {
  int model{};
  const std::array elements{element()};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 7u, elements));

  for (const auto query :
       {"WALL-1", "SOURCE-27", "IFCWALL", "EXTERIOR", "LEVEL 01", "CONCRETE"}) {
    const auto matches = cache.matches(query);
    ASSERT_EQ(matches.size(), 1u) << query;
    EXPECT_EQ(matches.front(), 0u) << query;
  }
  EXPECT_EQ(cache.metadataBuildCount(), 1u);
}

TEST(BimElementSearchCacheTests,
     PreservesFieldBoundariesAndLiteralSubstringRules) {
  int model{};
  BimElementMetadata metadata{};
  metadata.guid = "ab";
  metadata.sourceId = "cd";
  metadata.type = "IfcWall";
  metadata.displayName = "Outside Wall";
  metadata.objectType = "UnsearchedObjectType";
  metadata.storeyId = "UnsearchedStoreyId";
  metadata.materialCategory = "UnsearchedMaterialCategory";
  metadata.properties.push_back({.name = "UnsearchedProperty"});
  const std::array elements{metadata};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));

  EXPECT_TRUE(cache.matches("bc").empty());
  EXPECT_TRUE(cache.matches("ab cd").empty());
  EXPECT_TRUE(cache.matches(" wall ").empty());
  EXPECT_TRUE(cache.matches("outside ifcwall").empty());
  EXPECT_TRUE(cache.matches("unsearched").empty());
  EXPECT_EQ(cache.matches("side wa").size(), 1u);
  EXPECT_EQ(cache.matches(" ").size(), 1u);
}

TEST(BimElementSearchCacheTests,
     EmptyAndEmbeddedNullFieldsPreserveByteLengthsAndRowBoundaries) {
  int model{};
  std::array<BimElementMetadata, 4> elements{};
  elements[1].sourceId = std::string{"AB\0CD", 5u};
  elements[1].materialName = "EF";
  elements[2].displayName = std::string(8'192u, 'X') + "TAIL";
  elements[3].guid = "ROW";
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));

  const auto embeddedNull = cache.matches(std::string_view{"b\0c", 3u});
  ASSERT_EQ(embeddedNull.size(), 1u);
  EXPECT_EQ(embeddedNull.front(), 1u);
  EXPECT_EQ(cache.matches(std::string_view{"\0", 1u}).size(), 1u);
  const auto longField = cache.matches("xtail");
  ASSERT_EQ(longField.size(), 1u);
  EXPECT_EQ(longField.front(), 2u);
  EXPECT_EQ(cache.matches(std::string(8'192u, 'x') + "tail").size(), 1u);
  EXPECT_TRUE(cache.matches("cdef").empty());
  EXPECT_TRUE(cache.matches("tailrow").empty());
  EXPECT_TRUE(cache.matches(std::string_view{"d\0e", 3u}).empty());

  const auto all = cache.matches("");
  ASSERT_EQ(all.size(), elements.size());
  EXPECT_EQ(all.front(), 0u);
  EXPECT_EQ(all.back(), 3u);
}

TEST(BimElementSearchCacheTests,
     EmptyFieldReplacementDropsOldArenaAndRebuildsTheSameQuery) {
  int model{};
  std::array<BimElementMetadata, 3> elements{};
  elements[0].guid = "previous";
  elements[2].materialName = "previous";
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  ASSERT_EQ(cache.matches("PREVIOUS").size(), 2u);
  const auto queryBuilds = cache.matchBuildCount();

  // Reuse the same metadata allocation, keeping empty fields between rows.
  elements = {};
  ASSERT_TRUE(cache.refresh(&model, 2u, elements));
  EXPECT_TRUE(cache.matches("PREVIOUS").empty());
  EXPECT_EQ(cache.matchBuildCount(), queryBuilds + 1u);
  EXPECT_EQ(cache.matches("").size(), 3u);

  elements[1].storeyName = "previous";
  ASSERT_TRUE(cache.refresh(&model, 3u, elements));
  const auto matches = cache.matches("previous");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches.front(), 1u);
}

TEST(BimElementSearchCacheTests,
     HundredThousandRepeatedLabelIndexStaysWithin80BytesPerEntity) {
  int model{};
  constexpr size_t entityCount = 100'000u;
  const std::vector<BimElementMetadata> elements(entityCount, element());
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  ASSERT_TRUE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 3u);
  ASSERT_EQ(cache.matches("").size(), entityCount);

  // Of 51 source characters per entity, the 23 repeated categorical characters
  // live once in the shared pool. Including IDs, boundaries and original row
  // identity, the normalized index must stay below 80 bytes/entity before
  // filtered results. This is owned capacity, excluding source metadata/RSS.
  EXPECT_LE(cache.estimatedBytes(), entityCount * 80u + 256u);
  const auto metadataBuilds = cache.metadataBuildCount();
  EXPECT_TRUE(cache.matches("missing entity").empty());
  EXPECT_LE(cache.estimatedBytes(), entityCount * 80u + 256u);
  EXPECT_EQ(cache.metadataBuildCount(), metadataBuilds);
}

TEST(BimElementSearchCacheTests,
     SharesNormalizedLabelsAcrossCategoriesWithoutEditingSourceText) {
  int model{};
  std::vector<BimElementMetadata> elements(512u, element());
  for (size_t index = 0; index < elements.size(); ++index) {
    elements[index].type = "Shared LABEL";
    elements[index].storeyName =
        index % 2u == 0u ? "shared label" : "SHARED LABEL";
  }
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  ASSERT_TRUE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 2u);
  EXPECT_EQ(cache.matches("shared label").size(), elements.size());
  const auto matchBuilds = cache.matchBuildCount();
  EXPECT_EQ(cache.matches("SHARED LABEL").size(), elements.size());
  EXPECT_EQ(cache.matchBuildCount(), matchBuilds);
  EXPECT_EQ(cache.matches("CONCRETE").size(), elements.size());
  EXPECT_TRUE(cache.matches("shared labelconcrete").empty());
  EXPECT_EQ(elements.front().type, "Shared LABEL");
  EXPECT_EQ(elements.front().storeyName, "shared label");
}

TEST(BimElementSearchCacheTests,
     InternedPrefixAndNullLabelsKeepCompleteLengthAwareIdentity) {
  int model{};
  const std::array<std::string, 5> labels{
      "ab", "AB", "abc", std::string{"AB\0X", 4u}, std::string{"AB\0Y", 4u}};
  std::vector<BimElementMetadata> elements(500u);
  for (size_t index = 0; index < elements.size(); ++index)
    elements[index].type = labels[index % labels.size()];
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  ASSERT_TRUE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 4u);
  EXPECT_EQ(cache.matches("AB").size(), 500u);
  EXPECT_EQ(cache.matches("abc").size(), 100u);
  const auto embeddedNull = cache.matches(std::string_view{"ab\0x", 4u});
  ASSERT_EQ(embeddedNull.size(), 100u);
  EXPECT_EQ(embeddedNull.front(), 3u);
  EXPECT_EQ(embeddedNull.back(), 498u);
  EXPECT_EQ(cache.matches(std::string_view{"ab\0", 3u}).size(), 200u);
  EXPECT_TRUE(cache.matches("abx").empty());
  EXPECT_TRUE(cache.matches("xabc").empty());
}

TEST(BimElementSearchCacheTests,
     InternedMembershipBitsPreserveRowsAcrossEvery64BitWordBoundary) {
  int model{};
  constexpr size_t labelCount = 256u;
  constexpr size_t repetitions = 4u;
  std::array<std::string, labelCount> labels;
  for (size_t label = 0; label < labels.size(); ++label) {
    const auto number = std::to_string(label);
    labels[label] = "type-" + std::string(3u - number.size(), '0') + number;
  }
  std::vector<BimElementMetadata> elements(labelCount * repetitions);
  for (size_t index = 0; index < elements.size(); ++index) {
    elements[index].type = labels[index % labelCount];
    elements[index].storeyName = "shared-storey";
    elements[index].materialName = "shared-material";
  }
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  ASSERT_TRUE(cache.usesInternedLabels());
  ASSERT_EQ(cache.internedLabelCount(), labelCount + 2u);

  // The two shared categories take IDs beside the first type. Searching every
  // equal-length type exercises IDs on both sides of 63/64 and 127/128, and
  // clears bits left by the previous query across all five membership words.
  for (size_t label = 0; label < labels.size(); ++label) {
    const auto matches = cache.matches(labels[label]);
    ASSERT_EQ(matches.size(), repetitions) << labels[label];
    for (size_t repeat = 0; repeat < repetitions; ++repeat)
      EXPECT_EQ(matches[repeat], label + repeat * labelCount) << labels[label];
  }
  EXPECT_EQ(cache.matches("shared-storey").size(), elements.size());
  EXPECT_EQ(cache.matches("shared-material").size(), elements.size());
  EXPECT_TRUE(cache.matches("absent-label").empty());
}

TEST(BimElementSearchCacheTests,
     DiscoveryLabelLimitFallsBackWithoutLosingRowsOrRetainingPoolStorage) {
  int model{};
  constexpr size_t entityCount = 90'000u;
  std::vector<BimElementMetadata> elements(entityCount);
  for (size_t index = 0; index < elements.size(); ++index) {
    const auto number = std::to_string(index);
    const auto suffix = std::string(5u - number.size(), '0') + number;
    elements[index].objectIndex = static_cast<uint32_t>(index);
    elements[index].type = "t-" + suffix;
    elements[index].storeyName = "s-" + suffix;
    elements[index].materialName = "m-" + suffix;
  }
  // 270,000 distinct short labels exceed the bounded discovery limit of
  // 262,144, independently of the smaller cost-based fallback fixture below.
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  EXPECT_FALSE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 0u);
  EXPECT_LE(cache.estimatedBytes(), entityCount * 80u + 256u);
  const auto all = cache.matches("");
  ASSERT_EQ(all.size(), entityCount);
  EXPECT_EQ(all.front(), 0u);
  EXPECT_EQ(all.back(), entityCount - 1u);
  const std::array sentinels{std::pair{"T-00000", size_t{0u}},
                            std::pair{"S-45000", size_t{45'000u}},
                            std::pair{"M-89999", size_t{89'999u}}};
  for (const auto &[query, expected] : sentinels) {
    const auto matches = cache.matches(query);
    ASSERT_EQ(matches.size(), 1u) << query;
    EXPECT_EQ(matches.front(), expected) << query;
    EXPECT_EQ(elements[matches.front()].objectIndex, expected) << query;
  }
  cache.clear();
  EXPECT_EQ(cache.estimatedBytes(), BimElementSearchCache{}.estimatedBytes());
}

TEST(BimElementSearchCacheTests,
     HighCardinalityFallbackAndReinterningInvalidateTheExistingQuery) {
  int model{};
  std::vector<BimElementMetadata> elements(1'000u, element());
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  ASSERT_TRUE(cache.usesInternedLabels());
  ASSERT_EQ(cache.matches("ifcwall").size(), elements.size());

  for (size_t index = 0; index < elements.size(); ++index) {
    elements[index].type = "type-" + std::to_string(index);
    elements[index].storeyName = "storey-" + std::to_string(index);
    elements[index].materialName = "material-" + std::to_string(index);
  }
  ASSERT_TRUE(cache.refresh(&model, 2u, elements));
  EXPECT_FALSE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 0u);
  EXPECT_TRUE(cache.matches("ifcwall").empty());
  for (const auto query : {"TYPE-999", "STOREY-999", "MATERIAL-999"}) {
    const auto matches = cache.matches(query);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front(), 999u);
  }
  EXPECT_EQ(cache.matches("EXTERIOR").size(), elements.size());

  std::ranges::fill(elements, element());
  EXPECT_TRUE(cache.matches("ifcwall").empty());
  ASSERT_TRUE(cache.refresh(&model, 3u, elements));
  EXPECT_TRUE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 3u);
  EXPECT_EQ(cache.matches("ifcwall").size(), elements.size());
  EXPECT_EQ(cache.metadataBuildCount(), 3u);
}

TEST(BimElementSearchCacheTests,
     SearchOwnsItsPoolAfterCompactSourceStorageRetires) {
  int model{};
  BimElementSearchCache cache;
  {
    container::renderer::BimMetadataStorage storage;
    storage.reserve(512u);
    for (size_t index = 0; index < 512u; ++index)
      storage.push_back(element("wall-" + std::to_string(index)));
    storage.seal();
    ASSERT_TRUE(cache.refresh(&model, 1u, storage));
    ASSERT_TRUE(cache.usesInternedLabels());
  }
  EXPECT_EQ(cache.matches("CONCRETE").size(), 512u);
  const auto final = cache.matches("wall-511");
  ASSERT_EQ(final.size(), 1u);
  EXPECT_EQ(final.front(), 511u);
  const auto metadataBuilds = cache.metadataBuildCount();
  const auto queryBuilds = cache.matchBuildCount();
  cache.clear();
  EXPECT_FALSE(cache.usesInternedLabels());
  EXPECT_EQ(cache.internedLabelCount(), 0u);
  EXPECT_EQ(cache.estimatedBytes(), BimElementSearchCache{}.estimatedBytes());
  EXPECT_EQ(cache.metadataBuildCount(), metadataBuilds);
  EXPECT_EQ(cache.matchBuildCount(), queryBuilds);
}

TEST(BimElementSearchCacheTests,
     ReusesNormalizedMetadataAndUnchangedMatchStorage) {
  int model{};
  const std::array elements{element(), element("door-1")};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 5u, elements));
  const auto initial = cache.matches("wall-1");
  ASSERT_EQ(initial.size(), 1u);
  const auto *matchStorage = initial.data();
  const uint64_t matchBuilds = cache.matchBuildCount();

  for (size_t frame = 0; frame < 120; ++frame) {
    EXPECT_FALSE(cache.refresh(&model, 5u, elements));
    const auto matches = cache.matches("wall-1");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.data(), matchStorage);
  }
  EXPECT_EQ(cache.matches("WALL-1").data(), matchStorage);
  EXPECT_EQ(cache.matches("Wall-1").data(), matchStorage);
  EXPECT_EQ(cache.metadataBuildCount(), 1u);
  EXPECT_EQ(cache.matchBuildCount(), matchBuilds);

  const auto all = cache.matches("");
  ASSERT_EQ(all.size(), elements.size());
  const auto *allStorage = all.data();
  const auto emptyQueryBuilds = cache.matchBuildCount();
  for (size_t frame = 0; frame < 120; ++frame)
    EXPECT_EQ(cache.matches("").data(), allStorage);
  EXPECT_EQ(cache.matchBuildCount(), emptyQueryBuilds);
}

TEST(BimElementSearchCacheTests,
     RecoversFromEmptyAndNoMatchQueriesInMetadataOrder) {
  int model{};
  const std::array elements{element("wall-1"), element("door-1"),
                            element("wall-2")};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  EXPECT_EQ(cache.matches("").size(), 3u);
  EXPECT_TRUE(cache.matches("missing").empty());
  const auto noMatchBuilds = cache.matchBuildCount();
  EXPECT_TRUE(cache.matches("MISSING").empty());
  EXPECT_EQ(cache.matchBuildCount(), noMatchBuilds);

  const auto walls = cache.matches("wall-");
  ASSERT_EQ(walls.size(), 2u);
  EXPECT_EQ(walls[0], 0u);
  EXPECT_EQ(walls[1], 2u);
  const auto all = cache.matches("");
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0], 0u);
  EXPECT_EQ(all[1], 1u);
  EXPECT_EQ(all[2], 2u);
}

TEST(BimElementSearchCacheTests,
     PublicationRevisionHandlesSameSizedStorageReuse) {
  int model{};
  std::array elements{element()};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  EXPECT_EQ(cache.matches("wall-1").size(), 1u);
  elements[0].guid = "door-2";

  ASSERT_TRUE(cache.refresh(&model, 2u, elements));
  EXPECT_TRUE(cache.matches("wall-1").empty());
  const auto matches = cache.matches("door-2");
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches.front(), 0u);
  EXPECT_EQ(cache.metadataBuildCount(), 2u);
}

TEST(BimElementSearchCacheTests,
     SourceStorageAndCountEachInvalidateSearchData) {
  int firstModel{}, secondModel{};
  const std::array first{element()};
  const std::array second{element("door-2"), element("door-3")};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&firstModel, 1u, first));
  EXPECT_EQ(cache.matches("wall-1").size(), 1u);

  ASSERT_TRUE(cache.refresh(&secondModel, 1u, first));
  EXPECT_EQ(cache.matches("wall-1").size(), 1u);
  ASSERT_TRUE(cache.refresh(
      &secondModel, 1u, std::span<const BimElementMetadata>{second}.first(1)));
  EXPECT_TRUE(cache.matches("wall-1").empty());
  EXPECT_EQ(cache.matches("door-").size(), 1u);
  ASSERT_TRUE(cache.refresh(&secondModel, 1u, second));
  EXPECT_EQ(cache.matches("door-").size(), 2u);
  EXPECT_EQ(cache.metadataBuildCount(), 4u);
}

TEST(BimElementSearchCacheTests,
     ClearNullSourceAndEmptyPublicationDropOldMatches) {
  int model{};
  const std::array elements{element()};
  BimElementSearchCache cache;
  ASSERT_TRUE(cache.refresh(&model, 4u, elements));
  EXPECT_EQ(cache.matches("wall-1").size(), 1u);
  cache.clear();
  EXPECT_TRUE(cache.matches("wall-1").empty());
  EXPECT_TRUE(cache.matches("").empty());
  EXPECT_EQ(cache.metadataBuildCount(), 1u);

  ASSERT_TRUE(cache.refresh(&model, 4u, elements));
  EXPECT_EQ(cache.matches("wall-1").size(), 1u);
  EXPECT_FALSE(cache.refresh(nullptr, 4u, elements));
  EXPECT_TRUE(cache.matches("wall-1").empty());
  ASSERT_TRUE(cache.refresh(&model, 4u, elements));
  EXPECT_EQ(cache.matches("wall-1").size(), 1u);
  ASSERT_TRUE(cache.refresh(&model, 5u, {}));
  EXPECT_TRUE(cache.matches("wall-1").empty());
  EXPECT_TRUE(cache.matches("").empty());
}

TEST(BimElementSearchCacheTests, UnloadReleasesTheLargeIndexAndQueryStorage) {
  int model{};
  const std::vector<BimElementMetadata> elements(10'001u, element());
  BimElementSearchCache cache;
  const BimElementSearchCache empty;
  ASSERT_TRUE(cache.refresh(&model, 1u, elements));
  EXPECT_EQ(cache.matches("wall").size(), elements.size());
  EXPECT_TRUE(cache.matches(std::string(10'000u, 'x')).empty());
  EXPECT_GT(cache.estimatedBytes(), empty.estimatedBytes());
  const auto metadataBuilds = cache.metadataBuildCount();
  const auto queryBuilds = cache.matchBuildCount();
  cache.clear();
  EXPECT_EQ(cache.estimatedBytes(), empty.estimatedBytes());
  EXPECT_EQ(cache.metadataBuildCount(), metadataBuilds);
  EXPECT_EQ(cache.matchBuildCount(), queryBuilds);
}

} // namespace
