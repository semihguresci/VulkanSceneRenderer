#include "Container/renderer/bim/BimMetadataCatalog.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace {

using container::renderer::BimElementBounds;
using container::renderer::BimElementMetadata;
using container::renderer::BimGeometryKind;
using container::renderer::BimMetadataCatalog;
using container::renderer::BimMetadataSemanticCategory;
using container::renderer::BimModelUnitMetadata;
using container::renderer::BimSemanticColorMode;

[[nodiscard]] BimElementBounds bounds(float minY, float maxY) {
  BimElementBounds result{};
  result.valid = true;
  result.min = {0.0f, minY, 0.0f};
  result.max = {1.0f, maxY, 1.0f};
  return result;
}

TEST(BimMetadataCatalogTests, RegistersUniqueLabelsAndStoreyRanges) {
  BimMetadataCatalog catalog;
  catalog.reserve(4u);

  EXPECT_EQ(catalog.registerType("Wall"), 0u);
  EXPECT_EQ(catalog.registerType("Wall"), 0u);
  EXPECT_EQ(catalog.registerType("Door"), 1u);

  BimElementMetadata levelTwo{};
  levelTwo.storeyName = "Level 2";
  catalog.registerStorey(levelTwo, bounds(8.0f, 12.0f));
  catalog.registerStorey(levelTwo, bounds(7.5f, 13.0f));

  BimElementMetadata levelOne{};
  levelOne.storeyName = "Level 1";
  catalog.registerStorey(levelOne, bounds(0.0f, 4.0f));
  catalog.sortStoreyRanges();

  ASSERT_EQ(catalog.types().size(), 2u);
  EXPECT_EQ(catalog.types()[0], "Wall");
  EXPECT_EQ(catalog.types()[1], "Door");
  ASSERT_EQ(catalog.storeyRanges().size(), 2u);
  EXPECT_EQ(catalog.storeyRanges()[0].label, "Level 1");
  EXPECT_EQ(catalog.storeyRanges()[1].label, "Level 2");
  EXPECT_FLOAT_EQ(catalog.storeyRanges()[1].minElevation, 7.5f);
  EXPECT_FLOAT_EQ(catalog.storeyRanges()[1].maxElevation, 13.0f);
  EXPECT_EQ(catalog.storeyRanges()[1].objectCount, 2u);
}

TEST(BimMetadataCatalogTests, ProducesSemanticAndVisibilityIds) {
  BimMetadataCatalog catalog;
  BimElementMetadata metadata{};
  metadata.objectIndex = 9u;
  metadata.productIdentityId = 42u;
  metadata.semanticTypeId = catalog.registerType("Wall");
  metadata.storeyName = "Level 1";
  metadata.materialName = "Concrete";
  metadata.discipline = "Architecture";
  metadata.phase = "New";
  metadata.fireRating = "2h";
  metadata.loadBearing = "Yes";
  metadata.status = "Existing";
  metadata.geometryKind = BimGeometryKind::Curves;

  catalog.registerStorey(metadata, bounds(0.0f, 4.0f));
  catalog.registerMaterial(metadata);
  catalog.registerDiscipline(metadata.discipline);
  catalog.registerPhase(metadata.phase);
  catalog.registerFireRating(metadata.fireRating);
  catalog.registerLoadBearing(metadata.loadBearing);
  catalog.registerStatus(metadata.status);

  EXPECT_EQ(catalog.semanticIdForMetadata(metadata, BimSemanticColorMode::Type),
            1u);
  EXPECT_EQ(
      catalog.semanticIdForMetadata(metadata, BimSemanticColorMode::Storey),
      1u);
  EXPECT_EQ(
      catalog.semanticIdForMetadata(metadata, BimSemanticColorMode::Material),
      1u);
  EXPECT_EQ(catalog.semanticIdForCategory(
                BimMetadataSemanticCategory::Discipline, "Architecture"),
            1u);

  const auto gpuMetadata = catalog.visibilityGpuMetadata(metadata);
  EXPECT_EQ(gpuMetadata.semanticIds.x, 1u);
  EXPECT_EQ(gpuMetadata.semanticIds.y, 1u);
  EXPECT_EQ(gpuMetadata.semanticIds.z, 1u);
  EXPECT_EQ(gpuMetadata.semanticIds.w, 1u);
  EXPECT_EQ(gpuMetadata.propertyIds.x, 1u);
  EXPECT_EQ(gpuMetadata.propertyIds.y, 1u);
  EXPECT_EQ(gpuMetadata.propertyIds.z, 1u);
  EXPECT_EQ(gpuMetadata.propertyIds.w, 1u);
  EXPECT_EQ(gpuMetadata.identity.x, 42u);
  EXPECT_EQ(gpuMetadata.identity.y, 9u);
  EXPECT_EQ(gpuMetadata.identity.z,
            static_cast<uint32_t>(BimGeometryKind::Curves));
}

TEST(BimMetadataCatalogTests, ClearResetsLabelsAndModelMetadata) {
  BimMetadataCatalog catalog;
  EXPECT_EQ(catalog.registerType("Wall"), 0u);
  BimModelUnitMetadata unitMetadata{};
  unitMetadata.hasSourceUnits = true;
  unitMetadata.sourceUnits = "METER";
  catalog.setModelUnitMetadata(unitMetadata);

  catalog.clear();

  EXPECT_TRUE(catalog.types().empty());
  EXPECT_FALSE(catalog.modelUnitMetadata().hasSourceUnits);
  EXPECT_TRUE(catalog.modelUnitMetadata().sourceUnits.empty());
}

TEST(BimMetadataCatalogTests, ObjectReserveHintDoesNotAllocatePerObjectLabels) {
  BimMetadataCatalog catalog;
  // Even an unbounded object estimate is a small dictionary hint. The former
  // per-object reserves threw or attempted enormous bucket allocations here.
  ASSERT_NO_THROW(catalog.reserve(std::numeric_limits<size_t>::max()));
  for (uint32_t id = 0; id < 512u; ++id) {
    const std::string label = "Type " + std::to_string(id);
    EXPECT_EQ(catalog.registerType(label), id);
    EXPECT_EQ(catalog.registerType(label), id);
    EXPECT_EQ(catalog.semanticIdForCategory(BimMetadataSemanticCategory::Type,
                                            label), id + 1u);
  }
  EXPECT_EQ(catalog.types().size(), 512u);
}

TEST(BimMetadataCatalogTests, OwnsDictionaryLabelsResolvedFromCompactStorage) {
  container::renderer::BimMetadataStorage storage;
  BimElementMetadata metadata{};
  metadata.type = "IfcWall";
  metadata.storeyName = std::string("Level\0One", 9u);
  metadata.materialCategory = "Concrete";
  storage.push_back(std::move(metadata));
  storage.seal();

  BimMetadataCatalog catalog;
  const auto element = storage.front();
  EXPECT_EQ(catalog.registerType(element.type), 0u);
  catalog.registerStorey(element, bounds(1.0f, 3.0f));
  catalog.registerMaterial(element);
  storage.clear();

  EXPECT_EQ(catalog.types().front(), "IfcWall");
  EXPECT_EQ(catalog.storeys().front(), std::string("Level\0One", 9u));
  EXPECT_EQ(catalog.materials().front(), "Concrete");
  EXPECT_EQ(catalog.semanticIdForCategory(BimMetadataSemanticCategory::Storey,
                                          std::string("Level\0One", 9u)), 1u);
  ASSERT_EQ(catalog.storeyRanges().size(), 1u);
  EXPECT_EQ(catalog.storeyRanges().front().label,
            std::string("Level\0One", 9u));
}

} // namespace
