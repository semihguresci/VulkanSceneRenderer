#include "Container/renderer/bim/BimMetadata.h"

#include <gtest/gtest.h>

#include <array>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using container::renderer::BimElementMetadata;
using container::renderer::BimElementMetadataView;
using container::renderer::BimGeometryKind;
using container::renderer::BimMetadataStorage;
using container::renderer::BimMetadataTableView;

static_assert(std::ranges::forward_range<BimMetadataTableView>);
static_assert(std::ranges::forward_range<BimMetadataStorage>);

BimElementMetadata completeMetadata() {
  BimElementMetadata value;
  value.objectIndex = 7u;
  value.sourceElementIndex = 13u;
  value.meshId = 17u;
  value.sourceMaterialIndex = 19u;
  value.materialIndex = 23u;
  value.semanticTypeId = 29u;
  value.productIdentityId = 31u;
  value.sourceColor = {0.1f, 0.2f, 0.3f, 0.4f};
  value.guid = "unique-guid";
  value.type = "IfcWall";
  value.displayName = "Wall on the eastern side";
  value.objectType = "Exterior wall";
  value.storeyName = "Level 02";
  value.storeyId = "level-02";
  value.materialName = "Concrete";
  value.materialCategory = "Structural";
  value.discipline = "Architecture";
  value.phase = "New construction";
  value.fireRating = "2h";
  value.loadBearing = "Yes";
  value.status = "Approved";
  value.sourceId = "original-source-id";
  value.properties = {{"Dimensions", "Width", "0.3", "Length"},
                      {"Identity", "External", "true", "Boolean"}};
  value.transparent = true;
  value.doubleSided = true;
  value.bounds.valid = true;
  value.bounds.min = {-1.0f, -2.0f, -3.0f};
  value.bounds.max = {4.0f, 5.0f, 6.0f};
  value.bounds.center = {1.5f, 1.5f, 1.5f};
  value.bounds.size = {5.0f, 7.0f, 9.0f};
  value.bounds.radius = 8.0f;
  value.bounds.floorElevation = -2.0f;
  value.geometryKind = BimGeometryKind::Curves;
  return value;
}

void expectSame(BimElementMetadataView actual, BimElementMetadataView expected) {
  EXPECT_EQ(actual.objectIndex, expected.objectIndex);
  EXPECT_EQ(actual.sourceElementIndex, expected.sourceElementIndex);
  EXPECT_EQ(actual.meshId, expected.meshId);
  EXPECT_EQ(actual.sourceMaterialIndex, expected.sourceMaterialIndex);
  EXPECT_EQ(actual.materialIndex, expected.materialIndex);
  EXPECT_EQ(actual.semanticTypeId, expected.semanticTypeId);
  EXPECT_EQ(actual.productIdentityId, expected.productIdentityId);
  EXPECT_EQ(actual.sourceColor, expected.sourceColor);
  EXPECT_EQ(actual.guid, expected.guid);
  EXPECT_EQ(actual.type, expected.type);
  EXPECT_EQ(actual.displayName, expected.displayName);
  EXPECT_EQ(actual.objectType, expected.objectType);
  EXPECT_EQ(actual.storeyName, expected.storeyName);
  EXPECT_EQ(actual.storeyId, expected.storeyId);
  EXPECT_EQ(actual.materialName, expected.materialName);
  EXPECT_EQ(actual.materialCategory, expected.materialCategory);
  EXPECT_EQ(actual.discipline, expected.discipline);
  EXPECT_EQ(actual.phase, expected.phase);
  EXPECT_EQ(actual.fireRating, expected.fireRating);
  EXPECT_EQ(actual.loadBearing, expected.loadBearing);
  EXPECT_EQ(actual.status, expected.status);
  EXPECT_EQ(actual.sourceId, expected.sourceId);
  ASSERT_EQ(actual.properties.size(), expected.properties.size());
  for (size_t index = 0; index < expected.properties.size(); ++index) {
    EXPECT_EQ(actual.properties[index].set, expected.properties[index].set);
    EXPECT_EQ(actual.properties[index].name, expected.properties[index].name);
    EXPECT_EQ(actual.properties[index].value, expected.properties[index].value);
    EXPECT_EQ(actual.properties[index].category, expected.properties[index].category);
  }
  EXPECT_EQ(actual.transparent, expected.transparent);
  EXPECT_EQ(actual.doubleSided, expected.doubleSided);
  EXPECT_EQ(actual.bounds.valid, expected.bounds.valid);
  EXPECT_EQ(actual.bounds.min, expected.bounds.min);
  EXPECT_EQ(actual.bounds.max, expected.bounds.max);
  EXPECT_EQ(actual.bounds.center, expected.bounds.center);
  EXPECT_EQ(actual.bounds.size, expected.bounds.size);
  EXPECT_EQ(actual.bounds.radius, expected.bounds.radius);
  EXPECT_EQ(actual.bounds.floorElevation, expected.bounds.floorElevation);
  EXPECT_EQ(actual.geometryKind, expected.geometryKind);
}

TEST(BimMetadataStorageTests, RoundTripsAllFieldsAndPackedBoundaries) {
  const auto metadata = completeMetadata();
  BimMetadataStorage storage;
  storage.push_back(metadata);
  storage.seal();

  ASSERT_EQ(storage.size(), 1u);
  expectSame(storage.front(), metadata);
  const auto row = storage.front();
  EXPECT_EQ(row.displayName.data(), row.guid.data() + row.guid.size());
  EXPECT_EQ(row.sourceId.data(), row.displayName.data() + row.displayName.size());
  EXPECT_EQ(storage.sharedStringCount(), 11u);
  EXPECT_LT(BimMetadataStorage::recordSize(), sizeof(BimElementMetadata));
}

TEST(BimMetadataStorageTests, EmptyFieldsAndDefaultValuesRequireNoSharedLabels) {
  const BimElementMetadata metadata;
  BimMetadataStorage storage;
  storage.push_back(metadata);
  storage.seal();

  ASSERT_EQ(storage.size(), 1u);
  expectSame(storage.back(), metadata);
  EXPECT_EQ(storage.sharedStringCount(), 0u);
  EXPECT_TRUE(storage.front().guid.empty());
  EXPECT_TRUE(storage.front().properties.empty());
}

TEST(BimMetadataStorageTests, InternsExactTextAcrossFieldsAndEntities) {
  BimElementMetadata first;
  first.type = "Shared label";
  first.materialName = "Shared label";
  first.storeyName = "shared label";
  BimElementMetadata second;
  second.type = "Shared label";
  second.materialName = "Shared label suffix";
  BimMetadataStorage storage;
  storage.push_back(first);
  storage.push_back(second);
  storage.seal();

  EXPECT_EQ(storage.sharedStringCount(), 3u);
  EXPECT_EQ(storage[0].type.data(), storage[0].materialName.data());
  EXPECT_EQ(storage[0].type.data(), storage[1].type.data());
  EXPECT_NE(storage[0].type, storage[0].storeyName);
  EXPECT_NE(storage[0].type, storage[1].materialName);
}

TEST(BimMetadataStorageTests, PreservesEmbeddedNulsAndLongExactLabels) {
  BimElementMetadata first;
  first.type = std::string{"prefix\0A", 8u};
  first.storeyName = std::string{"prefix\0B", 8u};
  first.materialName = std::string(8192u, 'M') + "tail-one";
  first.guid = std::string{"guid\0one", 8u};
  first.displayName = std::string{"name\0one", 8u};
  first.sourceId = std::string{"source\0one", 10u};
  auto second = first;
  second.materialName.back() = 'X';
  BimMetadataStorage storage;
  storage.push_back(first);
  storage.push_back(second);
  storage.seal();

  expectSame(storage[0], first);
  expectSame(storage[1], second);
  EXPECT_EQ(storage.sharedStringCount(), 4u);
  EXPECT_NE(storage[0].type, storage[0].storeyName);
  EXPECT_NE(storage[0].materialName, storage[1].materialName);
  EXPECT_EQ(storage[0].type.data(), storage[1].type.data());
}

TEST(BimMetadataStorageTests, OwnsDataAfterImportDtoHasBeenDestroyed) {
  BimMetadataStorage storage;
  {
    auto transient = completeMetadata();
    storage.push_back(std::move(transient));
  }
  storage.seal();
  const auto expected = completeMetadata();
  expectSame(storage.front(), expected);
}

TEST(BimMetadataStorageTests, TableViewsAdaptDtosAndCompactStorage) {
  const std::array metadata{completeMetadata(), BimElementMetadata{}};
  const std::vector<BimElementMetadata> vector(metadata.begin(), metadata.end());
  const BimMetadataTableView arrayView = metadata;
  const BimMetadataTableView vectorView = vector;
  const BimMetadataTableView spanView = std::span<const BimElementMetadata>{metadata};
  const BimMetadataTableView pointerView{metadata.data(), metadata.size()};
  BimMetadataStorage storage;
  storage.replace(arrayView);
  const BimMetadataTableView compactView = storage;

  EXPECT_EQ(arrayView.data(), metadata.data());
  EXPECT_EQ(vectorView.data(), vector.data());
  EXPECT_EQ(compactView.data(), storage.data());
  for (const auto view : {arrayView, vectorView, spanView, pointerView, compactView}) {
    ASSERT_EQ(view.size(), metadata.size());
    size_t index = 0;
    for (const auto row : view)
      expectSame(row, metadata[index++]);
    EXPECT_EQ(index, metadata.size());
    expectSame(view.front(), metadata.front());
    expectSame(view.back(), metadata.back());
  }
  size_t count = 0;
  for (const auto row : storage) {
    expectSame(row, metadata[count++]);
  }
  EXPECT_EQ(count, metadata.size());
}

TEST(BimMetadataStorageTests, CopiesAndMovesRetainIndependentTextAndProperties) {
  const auto metadata = completeMetadata();
  BimMetadataStorage original;
  original.push_back(metadata);
  original.seal();
  BimMetadataStorage copy{original};
  BimMetadataStorage assigned;
  assigned = original;
  EXPECT_NE(copy.data(), original.data());
  EXPECT_NE(copy.front().type.data(), original.front().type.data());
  EXPECT_NE(copy.front().guid.data(), original.front().guid.data());
  EXPECT_NE(copy.front().properties.data(), original.front().properties.data());
  original.clear();
  expectSame(copy.front(), metadata);
  expectSame(assigned.front(), metadata);

  BimMetadataStorage moved{std::move(copy)};
  EXPECT_TRUE(copy.empty());
  EXPECT_EQ(copy.estimatedBytes(), 0u);
  expectSame(moved.front(), metadata);
  assigned = std::move(moved);
  EXPECT_TRUE(moved.empty());
  expectSame(assigned.front(), metadata);
  assigned = assigned;
  expectSame(assigned.front(), metadata);
}

TEST(BimMetadataStorageTests, ReplacementCanReadItsOwnCurrentPublication) {
  const std::array metadata{completeMetadata(), BimElementMetadata{}};
  BimMetadataStorage storage;
  storage.replace(metadata);
  storage.replace(storage.view());
  ASSERT_EQ(storage.size(), 2u);
  expectSame(storage[0], metadata[0]);
  expectSame(storage[1], metadata[1]);

  const std::array replacement{BimElementMetadata{}};
  storage.replace(replacement);
  ASSERT_EQ(storage.size(), 1u);
  EXPECT_EQ(storage.sharedStringCount(), 0u);
  expectSame(storage.front(), replacement[0]);
}

TEST(BimMetadataStorageTests, ProgressivePopAndAppendAfterSealPreserveRows) {
  auto first = completeMetadata();
  auto second = completeMetadata();
  second.guid = "second-guid";
  second.type = "IfcDoor";
  second.properties.push_back({"Other", "Second", "value", "Text"});
  BimMetadataStorage storage;
  storage.push_back(first);
  storage.push_back(second);
  storage.seal();
  storage.pop_back();
  ASSERT_EQ(storage.size(), 1u);
  expectSame(storage.front(), first);
  storage.push_back(second);
  storage.seal();
  ASSERT_EQ(storage.size(), 2u);
  expectSame(storage.front(), first);
  expectSame(storage.back(), second);
  EXPECT_EQ(storage.sharedStringCount(), 12u);
  storage.pop_back();
  storage.pop_back();
  storage.pop_back();
  EXPECT_TRUE(storage.empty());
  storage.clear();
  EXPECT_EQ(storage.estimatedBytes(), 0u);
  EXPECT_EQ(storage.sharedStringCount(), 0u);
  EXPECT_EQ(storage.data(), nullptr);
}

TEST(BimMetadataStorageTests, RepeatedMetadataStaysWithinCompactMemoryBudget) {
  constexpr size_t count = 100'000u;
  BimMetadataStorage storage;
  storage.reserve(count);
  BimElementMetadata metadata;
  metadata.type = "IfcWall";
  metadata.objectType = "Exterior";
  metadata.storeyName = "Level 01";
  metadata.storeyId = "L1";
  metadata.materialName = "Concrete";
  metadata.materialCategory = "Structural";
  metadata.discipline = "Architecture";
  metadata.phase = "New";
  metadata.fireRating = "2h";
  metadata.loadBearing = "Yes";
  metadata.status = "Approved";
  for (size_t index = 0; index < count; ++index) {
    metadata.objectIndex = static_cast<uint32_t>(index);
    metadata.guid = "GUID-" + std::to_string(index);
    metadata.displayName = "Wall-" + std::to_string(index);
    metadata.sourceId = "SRC-" + std::to_string(index);
    storage.push_back(metadata);
  }
  const size_t beforeSeal = storage.estimatedBytes();
  storage.seal();
  ASSERT_EQ(storage.size(), count);
  EXPECT_EQ(storage.sharedStringCount(), 11u);
  EXPECT_LT(storage.estimatedBytes(), beforeSeal);
  // Includes production records, all unique text and dictionary capacities.
  // Existing DTOs alone cost sizeof(BimElementMetadata) per row before heaps.
  EXPECT_LE(storage.estimatedBytes(), count * (BimMetadataStorage::recordSize() + 80u));
  EXPECT_LT(storage.estimatedBytes(), count * sizeof(BimElementMetadata));
  EXPECT_EQ(storage.back().objectIndex, count - 1u);
  EXPECT_EQ(storage.back().guid, "GUID-99999");
  EXPECT_EQ(storage.back().materialName, "Concrete");
}

} // namespace
