#include "Container/renderer/bim/BimStatisticsCache.h"

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <string_view>

namespace {
using namespace container::renderer;

std::array<BimElementMetadata, 3> elements() {
  std::array<BimElementMetadata, 3> result;
  for (uint32_t i = 0; i < result.size(); ++i) {
    result[i].objectIndex = i;
    result[i].guid = i < 2 ? "shared" : "other";
    result[i].type = i < 2 ? "Wall" : "Pipe";
    result[i].storeyName = i < 2 ? "Level 1" : "Level 2";
    result[i].materialName = i < 2 ? "Concrete" : "Steel";
    result[i].discipline = i < 2 ? "Architecture" : "MEP";
    result[i].phase = i < 2 ? "Existing" : "New";
    result[i].fireRating = i < 2 ? "60" : "90";
    result[i].loadBearing = i < 2 ? "Yes" : "No";
    result[i].status = i < 2 ? "Existing" : "New";
  }
  return result;
}

BimDrawFilterStateInputs inputs(std::span<const BimElementMetadata> metadata,
                                uint64_t revision = 1) {
  return {.revision = revision,
          .objectCount = metadata.size(),
          .metadata = metadata};
}

TEST(BimStatisticsCacheTests, BuildsCountsAndOnlyMeshTriangleBatchesOnce) {
  auto metadata = elements();
  metadata[0].materialIndex = 7;
  metadata[0].doubleSided = true;
  metadata[0].transparent = true;
  metadata[1].geometryKind = BimGeometryKind::Points;
  metadata[2].geometryKind = BimGeometryKind::Curves;
  std::array<container::gpu::ObjectData, 5> objects{};
  objects[3].objectInfo.x = 11;
  objects[3].objectInfo.y = container::gpu::kObjectFlagDoubleSided;
  objects[4].objectInfo.x = std::numeric_limits<uint32_t>::max();
  const std::array opaque{
      DrawCommand{.objectIndex = 0,
                  .firstIndex = 3,
                  .indexCount = 6,
                  .instanceCount = 2},
      DrawCommand{.objectIndex = 1, .firstIndex = 9, .indexCount = 3},
      DrawCommand{.objectIndex = 2, .firstIndex = 12, .indexCount = 3},
      DrawCommand{.objectIndex = 0, .firstIndex = 15, .indexCount = 0},
      DrawCommand{.objectIndex = 3,
                  .firstIndex = 18,
                  .indexCount = 3,
                  .instanceCount = 0}};
  const std::array transparent{
      DrawCommand{.objectIndex = 4, .firstIndex = 21, .indexCount = 3}};
  BimStatisticsCache cache;
  cache.rebuild({.opaqueDrawCount = opaque.size(),
                 .nativePointOpaqueDrawCount = 2,
                 .meshletClusterCount = 8,
                 .uniqueTypeCount = 2},
                metadata, objects, opaque, transparent);
  const auto stats = cache.sceneStats({}, {});
  EXPECT_EQ(stats.objectCount, 3);
  EXPECT_EQ(stats.meshObjectCount, 1);
  EXPECT_EQ(stats.pointObjectCount, 1);
  EXPECT_EQ(stats.curveObjectCount, 1);
  EXPECT_EQ(stats.nativePointOpaqueDrawCount, 2);
  EXPECT_EQ(stats.meshletClusterCount, 8);
  EXPECT_EQ(stats.uniqueTypeCount, 2);
  const auto batches = cache.triangleBatches();
  ASSERT_EQ(batches.size(), 3);
  EXPECT_EQ(batches[0].firstIndex, 3);
  EXPECT_EQ(batches[0].indexCount, 6);
  EXPECT_EQ(batches[0].materialIndex, 7);
  EXPECT_EQ(batches[0].instanceCount, 2);
  EXPECT_TRUE(batches[0].doubleSided);
  EXPECT_TRUE(batches[0].transparent);
  EXPECT_EQ(batches[1].materialIndex, 11);
  EXPECT_TRUE(batches[1].doubleSided);
  EXPECT_EQ(batches[1].instanceCount, 1);
  EXPECT_EQ(batches[2].materialIndex, -1);
  EXPECT_TRUE(batches[2].transparent);

  metadata[0].geometryKind = BimGeometryKind::Curves;
  metadata[0].materialIndex = 19;
  EXPECT_EQ(cache.sceneStats({}, {}).meshObjectCount, 1);
  EXPECT_EQ(cache.triangleBatches().data(), batches.data());
  EXPECT_EQ(cache.triangleBatches()[0].materialIndex, 7);
}

TEST(BimStatisticsCacheTests, ResidencyAndAllCompactionFieldsStayLive) {
  BimStatisticsCache cache;
  cache.rebuild({.meshletClusterCount = 20}, elements(), {}, {}, {});
  BimMeshletResidencyStats residency{};
  residency.residentObjectCount = 2;
  residency.residentClusterCount = 10;
  residency.clusterBufferBytes = 100;
  residency.objectLodBufferBytes = 200;
  residency.residencyBufferBytes = 300;
  residency.computeReady = true;
  residency.dispatchPending = true;
  std::array<BimDrawCompactionStats, 2> slots{};
  slots[0] = {.computeReady = true,
              .inputDrawCount = 3,
              .outputCapacity = 6,
              .inputBufferBytes = 50,
              .outputBufferBytes = 60,
              .countBufferBytes = 4};
  slots[1] = {.dispatchPending = true,
              .inputDrawCount = 2,
              .outputCapacity = 4,
              .inputBufferBytes = 20,
              .outputBufferBytes = 30,
              .countBufferBytes = 4};
  const auto first = cache.sceneStats(residency, slots);
  EXPECT_EQ(first.meshletClusterCount, 20);
  EXPECT_EQ(first.meshletGpuResidentObjectCount, 2);
  EXPECT_EQ(first.meshletGpuResidentClusterCount, 10);
  EXPECT_EQ(first.meshletGpuBufferBytes, 600);
  EXPECT_TRUE(first.meshletGpuComputeReady);
  EXPECT_TRUE(first.meshletGpuDispatchPending);
  EXPECT_EQ(first.gpuCompactionInputDrawCount, 5);
  EXPECT_EQ(first.gpuCompactionOutputCapacity, 10);
  EXPECT_EQ(first.gpuCompactionBufferBytes, 168);
  EXPECT_TRUE(first.gpuCompactionReady);
  EXPECT_TRUE(first.gpuCompactionDispatchPending);
  const auto next = cache.sceneStats({}, {});
  EXPECT_EQ(next.meshletClusterCount, 20);
  EXPECT_EQ(next.meshletGpuResidentObjectCount, 0);
  EXPECT_EQ(next.meshletGpuResidentClusterCount, 0);
  EXPECT_EQ(next.meshletGpuBufferBytes, 0);
  EXPECT_FALSE(next.meshletGpuComputeReady);
  EXPECT_FALSE(next.meshletGpuDispatchPending);
  EXPECT_EQ(next.gpuCompactionInputDrawCount, 0);
  EXPECT_EQ(next.gpuCompactionOutputCapacity, 0);
  EXPECT_EQ(next.gpuCompactionBufferBytes, 0);
  EXPECT_FALSE(next.gpuCompactionReady);
  EXPECT_FALSE(next.gpuCompactionDispatchPending);
}

TEST(BimStatisticsCacheTests,
     ClearAndReinstallInvalidateEvenWhenRevisionsMatch) {
  auto metadata = elements();
  std::array<BimObjectLodStreamingMetadata, 3> lod{};
  lod[0].clusterCount = 4;
  const std::array draws{
      DrawCommand{.objectIndex = 0, .firstIndex = 3, .indexCount = 6}};
  BimStatisticsCache cache;
  cache.rebuild({}, metadata, {}, draws, {});
  const auto oldBatches = cache.triangleBatches();
  EXPECT_EQ(cache.drawBudgetLodStats({}, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            4);
  cache.clear();
  EXPECT_EQ(cache.sceneStats({}, {}).objectCount, 0);
  EXPECT_TRUE(cache.triangleBatches().empty());
  EXPECT_EQ(
      cache.drawBudgetLodStats({}, inputs({}), {}, 1, 1).visibleObjectCount, 0);
  metadata[0].geometryKind = BimGeometryKind::Curves;
  cache.rebuild({}, metadata, {}, draws, {});
  EXPECT_EQ(cache.sceneStats({}, {}).curveObjectCount, 1);
  EXPECT_TRUE(cache.triangleBatches().empty());
  EXPECT_EQ(cache.drawBudgetLodStats({}, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            0);
  ASSERT_EQ(oldBatches.size(), 1);
  EXPECT_EQ(oldBatches[0].firstIndex, 3);
}

TEST(BimStatisticsCacheTests, BudgetTotalsReuseAndRefreshAllRevisions) {
  auto metadata = elements();
  std::array<BimObjectLodStreamingMetadata, 3> lod{};
  lod[0].clusterCount = 2;
  BimStatisticsCache cache;
  auto state = inputs(metadata);
  EXPECT_EQ(cache.drawBudgetLodStats({}, state, lod, 1, 1)
                .visibleMeshletClusterReferences,
            2);
  // Unpublished edits do not trigger work. Every published revision must.
  lod[0].clusterCount = 7;
  EXPECT_EQ(cache.drawBudgetLodStats({}, state, lod, 1, 1)
                .visibleMeshletClusterReferences,
            2);
  state.revision++;
  EXPECT_EQ(cache.drawBudgetLodStats({}, state, lod, 1, 1)
                .visibleMeshletClusterReferences,
            7);
  lod[0].clusterCount = 11;
  EXPECT_EQ(cache.drawBudgetLodStats({}, state, lod, 2, 1)
                .visibleMeshletClusterReferences,
            11);
  lod[0].clusterCount = 15;
  EXPECT_EQ(cache.drawBudgetLodStats({}, state, lod, 2, 2)
                .visibleMeshletClusterReferences,
            15);
}

TEST(BimStatisticsCacheTests,
     SelectionBudgetAndLodTotalsPreserveProductIdentity) {
  auto metadata = elements();
  std::array<BimObjectLodStreamingMetadata, 3> lod{};
  lod[0].clusterCount = 2;
  lod[0].maxLodLevel = 1;
  lod[1].clusterCount = 3;
  lod[1].maxLodLevel = 4;
  lod[2].clusterCount = 10;
  BimStatisticsCache cache;
  BimDrawFilter filter{};
  filter.isolateSelection = true;
  filter.selectedObjectIndex = 0;
  auto result = cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1);
  EXPECT_EQ(result.visibleObjectCount, 2);
  EXPECT_EQ(result.visibleMeshObjectCount, 2);
  EXPECT_EQ(result.visibleMeshletClusterReferences, 5);
  EXPECT_EQ(result.visibleMaxLodLevel, 4);
  filter.selectedObjectIndex = 2;
  EXPECT_EQ(cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            10);
  filter.isolateSelection = false;
  filter.hideSelection = true;
  EXPECT_EQ(cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            5);
  filter.hideSelection = false;
  filter.drawBudgetEnabled = true;
  filter.drawBudgetMaxObjects = 1;
  result = cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1);
  EXPECT_TRUE(result.enabled);
  EXPECT_EQ(result.maxObjects, 1);
  // The selected object remains visible even beyond the ordinary draw budget.
  EXPECT_EQ(result.visibleObjectCount, 2);
  EXPECT_EQ(result.visibleMeshletClusterReferences, 12);
  filter.drawBudgetMaxObjects = 2;
  EXPECT_EQ(cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            15);
  filter.selectedObjectIndex = std::numeric_limits<uint32_t>::max();
  EXPECT_EQ(cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            5);
  filter.drawBudgetMaxObjects = 1;
  EXPECT_EQ(cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1)
                .visibleMeshletClusterReferences,
            2);
}

TEST(BimStatisticsCacheTests, CompleteFilterKeyIncludesEveryUiField) {
  using Change =
      std::pair<std::string_view, std::function<void(BimDrawFilter &)>>;
  const std::array<Change, 29> changes{{
      {"type enabled", [](auto &f) { f.typeFilterEnabled = true; }},
      {"type label", [](auto &f) { f.type = "Wall"; }},
      {"storey enabled", [](auto &f) { f.storeyFilterEnabled = true; }},
      {"storey label", [](auto &f) { f.storey = "Level 1"; }},
      {"material enabled", [](auto &f) { f.materialFilterEnabled = true; }},
      {"material label", [](auto &f) { f.material = "Concrete"; }},
      {"discipline enabled", [](auto &f) { f.disciplineFilterEnabled = true; }},
      {"discipline label", [](auto &f) { f.discipline = "Architecture"; }},
      {"phase enabled", [](auto &f) { f.phaseFilterEnabled = true; }},
      {"phase label", [](auto &f) { f.phase = "Existing"; }},
      {"fire rating enabled",
       [](auto &f) { f.fireRatingFilterEnabled = true; }},
      {"fire rating label", [](auto &f) { f.fireRating = "60"; }},
      {"bearing enabled", [](auto &f) { f.loadBearingFilterEnabled = true; }},
      {"bearing label", [](auto &f) { f.loadBearing = "Yes"; }},
      {"status enabled", [](auto &f) { f.statusFilterEnabled = true; }},
      {"status label", [](auto &f) { f.status = "Existing"; }},
      {"budget enabled", [](auto &f) { f.drawBudgetEnabled = true; }},
      {"budget max", [](auto &f) { f.drawBudgetMaxObjects = 1; }},
      {"isolate", [](auto &f) { f.isolateSelection = true; }},
      {"hide", [](auto &f) { f.hideSelection = true; }},
      {"selected", [](auto &f) { f.selectedObjectIndex = 0; }},
      {"discipline preset",
       [](auto &f) { f.disciplinePreset = BimDisciplinePreset::Architecture; }},
      {"timeline enabled", [](auto &f) { f.phaseTimelineEnabled = true; }},
      {"timeline active", [](auto &f) { f.phaseTimelineActiveIndex = 1; }},
      {"timeline existing",
       [](auto &f) { f.phaseTimelineShowExisting = false; }},
      {"timeline new", [](auto &f) { f.phaseTimelineShowNew = false; }},
      {"timeline demolished",
       [](auto &f) { f.phaseTimelineShowDemolished = true; }},
      {"timeline ghost", [](auto &f) { f.phaseTimelineGhostFuture = true; }},
      {"combined timeline",
       [](auto &f) {
         f.phaseTimelineEnabled = true;
         f.phaseTimelineGhostFuture = true;
       }},
  }};
  for (const auto &[label, change] : changes) {
    SCOPED_TRACE(label);
    const auto metadata = elements();
    std::array<BimObjectLodStreamingMetadata, 3> lod{};
    lod[0].clusterCount = 2;
    BimStatisticsCache cache;
    (void)cache.drawBudgetLodStats({}, inputs(metadata), lod, 1, 1);
    lod[0].clusterCount = 9;
    BimDrawFilter filter{};
    change(filter);
    EXPECT_EQ(cache.drawBudgetLodStats(filter, inputs(metadata), lod, 1, 1)
                  .visibleMeshletClusterReferences,
              9);
  }
}

TEST(BimStatisticsCacheTests, UnionsValidBoundsAndPublishesLiveTranslation) {
  auto metadata = elements();
  metadata[0].bounds = {.valid = true, .min = {-2, 1, -3}, .max = {4, 5, 6}};
  metadata[1].bounds = {
      .valid = false, .min = {-900, -900, -900}, .max = {900, 900, 900}};
  metadata[2].bounds = {.valid = true, .min = {-1, -4, -2}, .max = {8, 3, 10}};
  const std::array draws{DrawCommand{.objectIndex = 0, .indexCount = 6}};
  BimStatisticsCache cache;
  cache.rebuild({}, metadata, {}, draws, {});
  const auto base = cache.sceneProviderBounds();
  const auto topology = cache.triangleBatches();
  ASSERT_TRUE(base.valid);
  EXPECT_EQ(base.min, glm::vec3(-2, -4, -3));
  EXPECT_EQ(base.max, glm::vec3(8, 5, 10));
  const auto translated = cache.sceneProviderBounds({10, 20, -30});
  EXPECT_TRUE(translated.valid);
  EXPECT_EQ(translated.min, glm::vec3(8, 16, -33));
  EXPECT_EQ(translated.max, glm::vec3(18, 25, -20));
  EXPECT_EQ(cache.sceneProviderBounds({-10, -20, 30}).min,
            glm::vec3(-12, -24, 27));
  EXPECT_EQ(cache.sceneProviderBounds().min, base.min);
  EXPECT_EQ(base.min, glm::vec3(-2, -4, -3));
  EXPECT_EQ(cache.triangleBatches().data(), topology.data());
}

TEST(BimStatisticsCacheTests, BoundsResetOnClearInvalidModelsAndReinstall) {
  auto metadata = elements();
  metadata[0].bounds = {.valid = true, .min = {-2, -3, -4}, .max = {5, 6, 7}};
  BimStatisticsCache cache;
  cache.rebuild({}, metadata, {}, {}, {});
  const auto retained = cache.sceneProviderBounds({1, 2, 3});
  ASSERT_TRUE(retained.valid);
  cache.clear();
  const auto empty = cache.sceneProviderBounds({100, 200, 300});
  EXPECT_FALSE(empty.valid);
  EXPECT_EQ(empty.min, glm::vec3(0));
  EXPECT_EQ(empty.max, glm::vec3(0));
  metadata[0].bounds.valid = false;
  cache.rebuild({}, metadata, {}, {}, {});
  EXPECT_FALSE(cache.sceneProviderBounds().valid);
  metadata[2].bounds = {
      .valid = true, .min = {10, 11, 12}, .max = {13, 14, 15}};
  cache.rebuild({}, metadata, {}, {}, {});
  EXPECT_EQ(cache.sceneProviderBounds().min, glm::vec3(10, 11, 12));
  EXPECT_EQ(cache.sceneProviderBounds().max, glm::vec3(13, 14, 15));
  EXPECT_EQ(retained.min, glm::vec3(-1, -1, -1));
  EXPECT_EQ(retained.max, glm::vec3(6, 8, 10));
}

} // namespace
