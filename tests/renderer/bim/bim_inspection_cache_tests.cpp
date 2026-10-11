#include "Container/renderer/bim/BimInspectionCache.h"
#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <vector>

namespace {

using container::renderer::BimElementMetadata;
using container::renderer::BimInspectionCache;
using container::renderer::BimScheduleRow;

BimElementMetadata wall(std::string_view guid = "wall-1") {
  BimElementMetadata result{};
  result.guid = guid;
  result.sourceId = "source-1";
  result.type = "IfcWall";
  result.objectType = "Basic Wall";
  result.storeyName = "Level 01";
  result.materialName = "Concrete";
  result.bounds.valid = true;
  result.bounds.min = {0.0f, 0.0f, 0.0f};
  result.bounds.max = {1.0f, 2.0f, 3.0f};
  return result;
}

const BimScheduleRow *findRow(std::span<const BimScheduleRow> rows,
                            std::string_view key) {
  for (const auto &row : rows)
    if (row.key == key) return &row;
  return nullptr;
}

TEST(BimInspectionCacheTests, PreservesScheduleGroupingAndCompareMetadata) {
  int manager{};
  std::array elements{wall(), wall("wall-2")};
  // The metadata fallback rules must agree with the former per-frame mapping.
  elements[1].storeyName.clear();
  elements[1].storeyId = "Level 02";
  elements[1].materialName.clear();
  elements[1].materialCategory = "Concrete";
  BimInspectionCache cache;

  EXPECT_TRUE(cache.refresh(&manager, 1u, elements));
  ASSERT_EQ(cache.byClassAndStorey().size(), 2u);
  ASSERT_EQ(cache.byTypeAndStorey().size(), 2u);
  const auto *material = findRow(cache.byMaterial(), "Concrete");
  ASSERT_NE(material, nullptr);
  EXPECT_EQ(material->count, 2u);
  EXPECT_FLOAT_EQ(material->estimatedArea, 44.0f);
  EXPECT_FLOAT_EQ(material->estimatedVolume, 12.0f);
  ASSERT_EQ(cache.compareElements().size(), 2u);
  const auto &comparison = cache.compareElements()[1];
  EXPECT_EQ(comparison.guid, "wall-2");
  EXPECT_EQ(comparison.sourceId, "source-1");
  EXPECT_EQ(comparison.ifcClass, "IfcWall");
  EXPECT_EQ(comparison.type, "Basic Wall");
  EXPECT_EQ(comparison.storey, "Level 02");
  EXPECT_EQ(comparison.material, "Concrete");
  EXPECT_TRUE(comparison.bounds.valid);
  EXPECT_EQ(comparison.bounds.max, glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST(BimInspectionCacheTests, ReusesAllTablesAcrossUnchangedFrames) {
  int manager{};
  const std::array elements{wall()};
  BimInspectionCache cache;
  ASSERT_TRUE(cache.refresh(&manager, 7u, elements));
  const auto *classes = cache.byClassAndStorey().data();
  const auto *types = cache.byTypeAndStorey().data();
  const auto *materials = cache.byMaterial().data();
  const auto *comparisons = cache.compareElements().data();

  for (int frame = 0; frame < 120; ++frame) {
    EXPECT_FALSE(cache.refresh(&manager, 7u, elements));
    EXPECT_EQ(cache.byClassAndStorey().data(), classes);
    EXPECT_EQ(cache.byTypeAndStorey().data(), types);
    EXPECT_EQ(cache.byMaterial().data(), materials);
    EXPECT_EQ(cache.compareElements().data(), comparisons);
  }
}

TEST(BimInspectionCacheTests, RebuildsForReplacementModelAtSameMetadataAddress) {
  int manager{};
  std::array elements{wall()};
  BimInspectionCache cache;
  ASSERT_TRUE(cache.refresh(&manager, 1u, elements));
  elements[0].guid = "door-1";
  elements[0].type = "IfcDoor";
  elements[0].objectType = "Entrance Door";

  ASSERT_TRUE(cache.refresh(&manager, 2u, elements));
  EXPECT_EQ(findRow(cache.byClassAndStorey(), "IfcWall"), nullptr);
  ASSERT_NE(findRow(cache.byClassAndStorey(), "IfcDoor"), nullptr);
  EXPECT_EQ(cache.compareElements()[0].guid, "door-1");
  EXPECT_EQ(cache.compareElements()[0].type, "Entrance Door");
}

TEST(BimInspectionCacheTests, RebuildsForDifferentManagerWithSameRevision) {
  int firstManager{}, secondManager{};
  std::array elements{wall()};
  BimInspectionCache cache;
  ASSERT_TRUE(cache.refresh(&firstManager, 1u, elements));
  elements[0].guid = "second-model-wall";

  ASSERT_TRUE(cache.refresh(&secondManager, 1u, elements));
  EXPECT_EQ(cache.compareElements()[0].guid, "second-model-wall");
}

TEST(BimInspectionCacheTests, UnloadDropsDataAndReloadRebuilds) {
  int manager{};
  const std::array elements{wall()};
  BimInspectionCache cache;
  ASSERT_TRUE(cache.refresh(&manager, 5u, elements));

  cache.clear();
  EXPECT_TRUE(cache.byClassAndStorey().empty());
  EXPECT_TRUE(cache.byTypeAndStorey().empty());
  EXPECT_TRUE(cache.byMaterial().empty());
  EXPECT_TRUE(cache.compareElements().empty());
  EXPECT_TRUE(cache.refresh(&manager, 5u, elements));
  EXPECT_FALSE(cache.refresh(nullptr, 5u, elements));
  EXPECT_TRUE(cache.compareElements().empty());
  EXPECT_TRUE(cache.refresh(&manager, 5u, elements));
}

TEST(BimInspectionCacheTests, RevisedBoundsRefreshQuantitiesAndComparison) {
  int manager{};
  std::array elements{wall()};
  BimInspectionCache cache;
  ASSERT_TRUE(cache.refresh(&manager, 1u, elements));
  elements[0].bounds.min = {10.0f, 20.0f, 30.0f};
  elements[0].bounds.max = {12.0f, 23.0f, 34.0f};

  ASSERT_TRUE(cache.refresh(&manager, 2u, elements));
  ASSERT_EQ(cache.byClassAndStorey().size(), 1u);
  EXPECT_FLOAT_EQ(cache.byClassAndStorey()[0].estimatedArea, 52.0f);
  EXPECT_FLOAT_EQ(cache.byClassAndStorey()[0].estimatedVolume, 24.0f);
  EXPECT_EQ(cache.compareElements()[0].bounds.min,
            glm::vec3(10.0f, 20.0f, 30.0f));
  EXPECT_EQ(cache.compareElements()[0].bounds.max,
            glm::vec3(12.0f, 23.0f, 34.0f));
}

TEST(BimInspectionCacheTests, GuardsAgainstMetadataStorageOrSizeReplacement) {
  int manager{};
  const std::array first{wall()};
  const std::array second{wall("wall-2")};
  BimInspectionCache cache;
  ASSERT_TRUE(cache.refresh(&manager, 1u, first));
  EXPECT_TRUE(cache.refresh(&manager, 1u, second));
  EXPECT_EQ(cache.compareElements()[0].guid, "wall-2");
  EXPECT_TRUE(cache.refresh(&manager, 1u,
                            std::span<const BimElementMetadata>{second}.first(0)));
  EXPECT_TRUE(cache.compareElements().empty());
}

} // namespace
