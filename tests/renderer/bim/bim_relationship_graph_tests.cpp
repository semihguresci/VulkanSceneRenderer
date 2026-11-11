#include "Container/renderer/bim/BimRelationshipGraph.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/utility/ModelImportCancellation.h"
#include "Container/utility/Platform.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using container::geometry::dotbim::ElementRelationship;
using container::renderer::BimElementMetadata;
using container::renderer::BimElementProperty;
using container::renderer::BimRelationshipEdge;
using container::renderer::BimRelationshipGraph;
using container::renderer::BimRelationshipKind;
using container::renderer::BimRelationshipNode;

[[nodiscard]] BimElementMetadata makeElement(uint32_t objectIndex,
                                             std::string_view guid,
                                             std::string_view sourceId,
                                             std::string_view ifcClass,
                                             std::string_view displayName) {
  BimElementMetadata metadata{};
  metadata.objectIndex = objectIndex;
  metadata.guid = std::string(guid);
  metadata.sourceId = std::string(sourceId);
  metadata.type = std::string(ifcClass);
  metadata.displayName = std::string(displayName);
  return metadata;
}

[[nodiscard]] bool hasEdge(const std::vector<BimRelationshipEdge> &edges,
                           BimRelationshipKind kind) {
  return std::ranges::any_of(edges, [kind](const BimRelationshipEdge &edge) {
    return edge.kind == kind;
  });
}

[[nodiscard]] bool hasSearchHit(const BimRelationshipGraph &graph,
                                std::string_view query,
                                uint32_t objectIndex,
                                std::string_view reason) {
  const auto hits = graph.search(query);
  return std::ranges::any_of(hits, [&](const auto &hit) {
    return hit.objectIndex == objectIndex && hit.reason == reason;
  });
}

TEST(BimRelationshipGraphTests, BuildsSpatialContainmentAndPropertySets) {
  std::vector<BimElementMetadata> metadata;
  metadata.push_back(makeElement(0u, "building-guid", "building-1",
                                 "IfcBuilding", "HQ Building"));
  metadata.push_back(
      makeElement(1u, "storey-guid", "storey-1", "IfcBuildingStorey",
                  "Level 01"));
  BimElementMetadata wall =
      makeElement(2u, "wall-guid", "wall-1", "IfcWall", "Rated Wall");
  wall.objectType = "Basic Wall";
  wall.storeyName = "Level 01";
  wall.storeyId = "storey-1";
  wall.materialName = "Concrete";
  wall.properties.push_back(BimElementProperty{.set = "Pset_WallCommon",
                                               .name = "FireRating",
                                               .value = "2HR",
                                               .category = "IfcPropertySet"});
  metadata.push_back(std::move(wall));

  std::vector<ElementRelationship> relationships;
  relationships.push_back(ElementRelationship{.fromGuid = "building-guid",
                                              .toGuid = "storey-guid",
                                              .kind = "spatial",
                                              .label = "contains"});
  relationships.push_back(ElementRelationship{.fromGuid = "storey-guid",
                                              .toGuid = "wall-guid",
                                              .kind = "spatial",
                                              .label = "contains"});

  BimRelationshipGraph graph;
  graph.build(metadata, relationships);

  EXPECT_GE(graph.nodes().size(), metadata.size() + 3u);
  EXPECT_TRUE(hasEdge(graph.edgesForObject(2u),
                      BimRelationshipKind::SpatialParent));
  EXPECT_TRUE(hasEdge(graph.edgesForObject(2u),
                      BimRelationshipKind::MaterialAssignment));
  EXPECT_TRUE(
      hasEdge(graph.edgesForObject(2u), BimRelationshipKind::TypeDefinition));
  EXPECT_TRUE(
      hasEdge(graph.edgesForObject(2u), BimRelationshipKind::PropertySet));

  const auto parents = graph.parentsForObject(2u);
  EXPECT_NE(std::ranges::find_if(parents,
                                 [](const auto &node) {
                                   return node.label == "Level 01";
                                 }),
            parents.end());

  const auto children = graph.childrenForObject(0u);
  EXPECT_NE(std::ranges::find_if(children,
                                 [](const auto &node) {
                                   return node.label == "Level 01";
                                 }),
            children.end());

  const auto propertySets = graph.propertySetsForObject(2u);
  ASSERT_EQ(propertySets.size(), 1u);
  EXPECT_EQ(propertySets[0].set, "Pset_WallCommon");
  ASSERT_EQ(propertySets[0].properties.size(), 1u);
  EXPECT_EQ(propertySets[0].properties[0].name, "FireRating");
  EXPECT_EQ(propertySets[0].properties[0].value, "2HR");
}

TEST(BimRelationshipGraphTests, FindsSystemsZonesAndClassificationsByGuid) {
  std::vector<BimElementMetadata> metadata;
  metadata.push_back(
      makeElement(0u, "wall-guid", "wall-1", "IfcWall", "Rated Wall"));

  std::vector<ElementRelationship> relationships;
  relationships.push_back(ElementRelationship{.fromGuid = "wall-guid",
                                              .toGuid = "system-guid",
                                              .toSourceId = "system-1",
                                              .kind = "system",
                                              .label = "Supply Air"});
  relationships.push_back(ElementRelationship{.fromSourceId = "wall-1",
                                              .toGuid = "zone-guid",
                                              .toSourceId = "zone-1",
                                              .kind = "zone",
                                              .label = "Smoke Zone"});
  relationships.push_back(ElementRelationship{.fromGuid = "wall-guid",
                                              .toSourceId = "OmniClass 23-11",
                                              .kind = "classification",
                                              .label = "OmniClass"});

  BimRelationshipGraph graph;
  graph.build(metadata, relationships);

  const auto edges = graph.edgesForObject(0u);
  EXPECT_TRUE(hasEdge(edges, BimRelationshipKind::SystemAssignment));
  EXPECT_TRUE(hasEdge(edges, BimRelationshipKind::ZoneAssignment));
  EXPECT_TRUE(hasEdge(edges, BimRelationshipKind::Classification));

  EXPECT_TRUE(hasSearchHit(graph, "Supply Air", 0u, "relationship"));
  EXPECT_TRUE(hasSearchHit(graph, "Smoke Zone", 0u, "relationship"));
  EXPECT_TRUE(hasSearchHit(graph, "OmniClass", 0u, "relationship"));
}

TEST(BimRelationshipGraphTests, ReusesRealStoreyNodeForInferredSpatialParent) {
  std::vector<BimElementMetadata> metadata;
  metadata.push_back(
      makeElement(1u, "storey-guid", "storey-1", "IfcBuildingStorey",
                  "Level 01"));
  BimElementMetadata wall =
      makeElement(2u, "wall-guid", "wall-1", "IfcWall", "Rated Wall");
  wall.storeyName = "Level 01";
  wall.storeyId = "storey-1";
  metadata.push_back(std::move(wall));

  BimRelationshipGraph graph;
  graph.build(metadata, {});

  const auto children = graph.childrenForObject(1u);
  EXPECT_NE(std::ranges::find_if(children,
                                 [](const auto &node) {
                                   return node.objectIndex == 2u;
                                 }),
            children.end());

  const auto nodes = graph.nodes();
  const auto storeyIdentityCount =
      std::ranges::count_if(nodes, [](const auto &node) {
        return node.sourceId == "storey-1";
      });
  EXPECT_EQ(storeyIdentityCount, 1);
}

TEST(BimRelationshipGraphTests, ResolvesSharedLabelsByCompatibleClass) {
  std::vector<BimElementMetadata> metadata{
      makeElement(30u, "wall", "", "IfcWall", "Shared label"),
      makeElement(20u, "first-type", "", "IfcWallTYPE", "Shared label"),
      makeElement(10u, "second-type", "", "IfcSlabType", "Shared label"),
      makeElement(40u, "storey", "", "IfcBuildingStorey", "Shared label"),
      makeElement(50u, "first-child", "", "IfcWall", "Wall A"),
      makeElement(60u, "second-child", "", "IfcWall", "Wall B"),
  };
  for (auto &child : std::span(metadata).subspan(4)) {
    child.objectType = "Shared label";
    child.storeyName = "Shared label";
    child.materialName = "Shared label";
  }

  BimRelationshipGraph graph;
  graph.build(metadata);

  for (const uint32_t child : {50u, 60u}) {
    const auto parents = graph.parentsForObject(child);
    ASSERT_EQ(parents.size(), 3u);
    EXPECT_EQ(parents[0].objectIndex, 40u);
    EXPECT_EQ(parents[1].ifcClass, "IfcMaterial");
    EXPECT_TRUE(parents[1].guid.empty());
    EXPECT_EQ(parents[2].objectIndex, 20u);
  }
  EXPECT_EQ(std::ranges::count(graph.nodes(), std::string_view("IfcMaterial"),
                               &BimRelationshipNode::ifcClass),
            1);

  metadata[0].type = "IfcWallType";
  graph.build(metadata);
  const auto parents = graph.parentsForObject(60u);
  ASSERT_EQ(parents.size(), 3u);
  EXPECT_EQ(parents[2].objectIndex, 30u);
}

TEST(BimRelationshipGraphTests, StoreyIdentityTakesPrecedenceOverSharedLabel) {
  std::vector<BimElementMetadata> metadata{
      makeElement(1u, "first", "storey-1", "IfcBuildingStorey", "Same level"),
      makeElement(2u, "second", "storey-2", "IfcBuildingStorey", "Same level"),
      makeElement(3u, "first-child", "", "IfcWall", "Wall A"),
      makeElement(4u, "second-child", "", "IfcWall", "Wall B"),
      makeElement(5u, "third-child", "", "IfcWall", "Wall C"),
  };
  metadata[2].storeyName = "Same level";
  metadata[2].storeyId = "storey-2";
  metadata[3].storeyName = "Same level";
  metadata[4].storeyName = "Same level";
  metadata[4].storeyId = "storey-2";

  BimRelationshipGraph graph;
  graph.build(metadata);

  const auto firstChildren = graph.childrenForObject(1u);
  ASSERT_EQ(firstChildren.size(), 1u);
  EXPECT_EQ(firstChildren[0].objectIndex, 4u);
  const auto secondChildren = graph.childrenForObject(2u);
  ASSERT_EQ(secondChildren.size(), 2u);
  EXPECT_EQ(secondChildren[0].objectIndex, 3u);
  EXPECT_EQ(secondChildren[1].objectIndex, 5u);
}

TEST(BimRelationshipGraphTests,
     ExplicitRelationshipsAttachToDuplicateProductIdentities) {
  std::vector<BimElementMetadata> metadata;
  metadata.push_back(
      makeElement(10u, "wall-guid", "wall-1", "IfcWall", "Wall draw A"));
  metadata.push_back(
      makeElement(11u, "wall-guid", "wall-1", "IfcWall", "Wall draw B"));

  std::vector<ElementRelationship> relationships;
  relationships.push_back(ElementRelationship{.fromGuid = "system-guid",
                                              .fromSourceId = "system-1",
                                              .toGuid = "wall-guid",
                                              .toSourceId = "wall-1",
                                              .kind = "system",
                                              .label = "Supply Air"});

  BimRelationshipGraph graph;
  graph.build(metadata, relationships);

  EXPECT_TRUE(hasEdge(graph.edgesForObject(10u),
                      BimRelationshipKind::SystemAssignment));
  EXPECT_TRUE(hasEdge(graph.edgesForObject(11u),
                      BimRelationshipKind::SystemAssignment));
  EXPECT_TRUE(hasSearchHit(graph, "Supply Air", 10u, "relationship"));
  EXPECT_TRUE(hasSearchHit(graph, "Supply Air", 11u, "relationship"));
}

TEST(BimRelationshipGraphTests, ExplicitRelationshipsMergeIdentitiesInNodeOrder) {
  const std::vector<BimElementMetadata> metadata{
      makeElement(42u, "other-guid", "shared-source", "IfcWall", "Wall A"),
      makeElement(13u, "shared-guid", "shared-source", "IfcWall", "Wall B"),
      makeElement(2u, "shared-guid", "other-source", "IfcWall", "Wall C"),
      makeElement(0u, "other-guid", "other-source", "IfcWall", "Wall D"),
  };
  const ElementRelationship relationship{.fromGuid = "system-guid",
                                         .toGuid = "shared-guid",
                                         .toSourceId = "shared-source",
                                         .kind = "system",
                                         .label = "Mechanical Circuit"};
  const std::array relationships{relationship, relationship};
  BimRelationshipGraph graph;
  graph.build(metadata, relationships);

  std::vector<uint32_t> linkedObjects;
  for (const auto &edge : graph.edges()) {
    if (edge.kind == BimRelationshipKind::SystemAssignment) {
      linkedObjects.push_back(graph.nodes()[edge.to].objectIndex);
    }
  }
  EXPECT_EQ(linkedObjects, (std::vector<uint32_t>{42u, 13u, 2u}));

  std::vector<uint32_t> searchObjects;
  for (const auto &hit : graph.search("Mechanical Circuit")) {
    EXPECT_EQ(hit.reason, "relationship");
    searchObjects.push_back(hit.objectIndex);
  }
  EXPECT_EQ(searchObjects,
            (std::vector<uint32_t>{42u, 13u, 2u, 42u, 13u, 2u}));
}

TEST(BimRelationshipGraphTests,
     SearchMatchesPropertySetNameValueAndElementName) {
  BimElementMetadata wall =
      makeElement(4u, "wall-guid", "wall-1", "IfcWall", "Lobby Wall");
  wall.properties.push_back(BimElementProperty{.set = "Pset_WallCommon",
                                               .name = "FireRating",
                                               .value = "2HR",
                                               .category = "IfcPropertySet"});

  BimRelationshipGraph graph;
  graph.build(std::vector<BimElementMetadata>{wall}, {});

  EXPECT_TRUE(hasSearchHit(graph, "Lobby", 4u, "element name"));
  EXPECT_TRUE(hasSearchHit(graph, "wall-guid", 4u, "guid"));
  EXPECT_TRUE(hasSearchHit(graph, "wall-1", 4u, "source id"));
  EXPECT_TRUE(hasSearchHit(graph, "IfcWall", 4u, "IFC class"));
  EXPECT_TRUE(hasSearchHit(graph, "Pset_WallCommon", 4u, "property set"));
  EXPECT_TRUE(hasSearchHit(graph, "FireRating", 4u, "property name"));
  EXPECT_TRUE(hasSearchHit(graph, "2HR", 4u, "property value"));
}

TEST(BimRelationshipGraphTests,
     LargeSharedGroupsRetainDistinctEdgesAndRebuild) {
  constexpr uint32_t count = 20000;
  std::vector<BimElementMetadata> metadata;
  metadata.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto element = makeElement(i, "shared-product", "shared-source", "IfcWall",
                               "Repeated wall");
    element.storeyName = "Level 01";
    element.materialName = "Concrete";
    element.properties.push_back({.set = "Repeated property set",
                                  .name = "LoadBearing",
                                  .value = "true"});
    metadata.push_back(std::move(element));
  }
  std::vector<ElementRelationship> relationships{{.fromGuid = "system-guid",
                                                  .toGuid = "shared-product",
                                                  .kind = "system",
                                                  .label = "Supply"},
                                                 {.fromGuid = "system-guid",
                                                  .toGuid = "shared-product",
                                                  .kind = "system",
                                                  .label = "Supply"},
                                                 {.fromGuid = "system-guid",
                                                  .toGuid = "shared-product",
                                                  .kind = "system",
                                                  .label = "Return"},
                                                 {.fromGuid = "system-guid",
                                                  .toGuid = "shared-product",
                                                  .kind = "zone",
                                                  .label = "Supply"}};
  BimRelationshipGraph graph;
  graph.build(metadata, relationships);
  EXPECT_EQ(graph.edges().size(), size_t{count} * 7);
  const auto edges = graph.edgesForObject(count - 1);
  EXPECT_EQ(edges.size(), 7u);
  EXPECT_EQ(std::ranges::count_if(
                edges,
                [](const auto &edge) {
                  return edge.kind == BimRelationshipKind::SystemAssignment &&
                         edge.label == "Supply";
                }),
            1);
  metadata.resize(1);
  graph.build(metadata, relationships);
  EXPECT_EQ(graph.edges().size(), 7u);
  graph.clear();
  EXPECT_TRUE(graph.edges().empty());
  EXPECT_TRUE(graph.nodes().empty());
}

TEST(BimRelationshipGraphTests,
     CooperativeBuildPreservesNodesEdgesPropertiesAndSearch) {
  constexpr uint32_t count = 300u;
  std::vector<BimElementMetadata> metadata;
  std::vector<ElementRelationship> relationships;
  metadata.reserve(count);
  relationships.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    auto element = makeElement(i, "wall-guid-" + std::to_string(i),
                               "wall-source-" + std::to_string(i), "IfcWall",
                               "Wall " + std::to_string(i));
    element.storeyName = "Level " + std::to_string(i % 4u);
    element.materialName = "Concrete";
    element.properties.push_back({.set = "Pset_WallCommon",
                                  .name = "FireRating",
                                  .value = "2HR",
                                  .category = "IfcPropertySet"});
    metadata.push_back(std::move(element));
    relationships.push_back({.fromGuid = "system-guid",
                              .toGuid = "wall-guid-" + std::to_string(i),
                              .toSourceId = "wall-source-" + std::to_string(i),
                              .kind = "system",
                              .label = "Supply Air"});
  }

  BimRelationshipGraph expected;
  expected.build(metadata, relationships);
  BimRelationshipGraph actual;
  size_t checkpoints = 0u;
  actual.build(metadata, relationships, [&] { ++checkpoints; });
  EXPECT_GE(checkpoints, 8u);

  ASSERT_EQ(actual.nodes().size(), expected.nodes().size());
  for (size_t i = 0; i < expected.nodes().size(); ++i) {
    SCOPED_TRACE(i);
    const auto &lhs = actual.nodes()[i];
    const auto &rhs = expected.nodes()[i];
    EXPECT_EQ(lhs.objectIndex, rhs.objectIndex);
    EXPECT_EQ(lhs.guid, rhs.guid);
    EXPECT_EQ(lhs.sourceId, rhs.sourceId);
    EXPECT_EQ(lhs.label, rhs.label);
    EXPECT_EQ(lhs.ifcClass, rhs.ifcClass);
  }
  ASSERT_EQ(actual.edges().size(), expected.edges().size());
  for (size_t i = 0; i < expected.edges().size(); ++i) {
    SCOPED_TRACE(i);
    const auto &lhs = actual.edges()[i];
    const auto &rhs = expected.edges()[i];
    EXPECT_EQ(lhs.from, rhs.from);
    EXPECT_EQ(lhs.to, rhs.to);
    EXPECT_EQ(lhs.kind, rhs.kind);
    EXPECT_EQ(lhs.label, rhs.label);
  }
  for (uint32_t i = 0; i < count; ++i) {
    SCOPED_TRACE(i);
    const auto lhs = actual.propertySetsForObject(i);
    const auto rhs = expected.propertySetsForObject(i);
    ASSERT_EQ(lhs.size(), rhs.size());
    ASSERT_EQ(lhs.size(), 1u);
    EXPECT_EQ(lhs[0].set, rhs[0].set);
    EXPECT_EQ(lhs[0].category, rhs[0].category);
    ASSERT_EQ(lhs[0].properties.size(), rhs[0].properties.size());
    ASSERT_EQ(lhs[0].properties.size(), 1u);
    EXPECT_EQ(lhs[0].properties[0].name, rhs[0].properties[0].name);
    EXPECT_EQ(lhs[0].properties[0].value, rhs[0].properties[0].value);
    EXPECT_EQ(lhs[0].properties[0].category, rhs[0].properties[0].category);
  }
  for (const std::string_view query : {"Wall", "Supply Air", "2HR"}) {
    SCOPED_TRACE(query);
    const auto lhs = actual.search(query);
    const auto rhs = expected.search(query);
    ASSERT_EQ(lhs.size(), rhs.size());
    for (size_t i = 0; i < rhs.size(); ++i) {
      EXPECT_EQ(lhs[i].objectIndex, rhs[i].objectIndex);
      EXPECT_EQ(lhs[i].matchedText, rhs[i].matchedText);
      EXPECT_EQ(lhs[i].reason, rhs[i].reason);
    }
  }
}

TEST(BimRelationshipGraphTests,
     CooperativeClearDropsIndexesPropertiesAndAllowsFreshRebuild) {
  std::vector<BimElementMetadata> metadata;
  for (uint32_t i = 0; i < 300u; ++i) {
    auto element = makeElement(i, "old-guid-" + std::to_string(i),
                               "old-source-" + std::to_string(i), "IfcWall",
                               "Old wall " + std::to_string(i));
    element.storeyName = "Old storey";
    element.properties.push_back({.set = "Old property set",
                                  .name = "Old property",
                                  .value = "Old value"});
    metadata.push_back(std::move(element));
  }
  BimRelationshipGraph graph;
  graph.build(metadata);
  ASSERT_FALSE(graph.nodes().empty());
  ASSERT_FALSE(graph.edges().empty());
  ASSERT_FALSE(graph.search("Old").empty());
  size_t checkpoints = 0u;
  graph.clear([&] { ++checkpoints; });
  EXPECT_GE(checkpoints, 6u);
  EXPECT_TRUE(graph.nodes().empty());
  EXPECT_TRUE(graph.edges().empty());
  EXPECT_TRUE(graph.search("Old").empty());
  for (uint32_t i = 0; i < 300u; ++i) {
    EXPECT_TRUE(graph.propertySetsForObject(i).empty());
    EXPECT_TRUE(graph.parentsForObject(i).empty());
    EXPECT_TRUE(graph.childrenForObject(i).empty());
  }

  auto fresh = makeElement(0u, "fresh-guid", "fresh-source", "IfcDoor",
                           "Fresh door");
  fresh.storeyName = "Fresh storey";
  const std::vector<ElementRelationship> relationships{
      {.fromGuid = "fresh-system", .toGuid = "fresh-guid", .kind = "system",
       .label = "Fresh system"}};
  graph.build(std::vector<BimElementMetadata>{fresh}, relationships);
  EXPECT_TRUE(graph.search("Old").empty());
  EXPECT_TRUE(hasSearchHit(graph, "Fresh system", 0u, "relationship"));
  EXPECT_TRUE(hasEdge(graph.edgesForObject(0u),
                      BimRelationshipKind::SystemAssignment));
  EXPECT_EQ(graph.parentsForObject(0u).size(), 3u);
  EXPECT_TRUE(graph.propertySetsForObject(0u).empty());
  EXPECT_TRUE(graph.edgesForObject(299u).empty());
}

TEST(BimRelationshipGraphTests, OwnsResolvedCompactStorageMetadata) {
  auto wall = makeElement(0u, "wall-guid", "wall-source", "IfcWall",
                           "Rated wall");
  wall.objectType = "Basic Wall";
  wall.storeyName = "Level 01";
  wall.storeyId = "storey-source";
  wall.materialName = "Concrete";
  wall.properties.push_back({.set = "Pset_WallCommon",
                             .name = "FireRating",
                             .value = "2HR",
                             .category = "IfcPropertySet"});
  container::renderer::BimMetadataStorage storage;
  storage.push_back(std::move(wall));
  storage.seal();
  BimRelationshipGraph graph;
  graph.build(storage);
  storage.clear();

  ASSERT_FALSE(graph.nodes().empty());
  EXPECT_EQ(graph.nodes().front().guid, "wall-guid");
  EXPECT_EQ(graph.nodes().front().sourceId, "wall-source");
  EXPECT_EQ(graph.nodes().front().label, "Rated wall");
  EXPECT_EQ(graph.nodes().front().ifcClass, "IfcWall");
  EXPECT_TRUE(hasSearchHit(graph, "wall-guid", 0u, "guid"));
  EXPECT_TRUE(hasSearchHit(graph, "Concrete", 0u, "material"));
  EXPECT_TRUE(hasSearchHit(graph, "2HR", 0u, "property value"));
  EXPECT_TRUE(hasEdge(graph.edgesForObject(0u),
                      BimRelationshipKind::SpatialParent));
  EXPECT_TRUE(hasEdge(graph.edgesForObject(0u),
                      BimRelationshipKind::MaterialAssignment));
  ASSERT_EQ(graph.propertySetsForObject(0u).size(), 1u);
  const auto &group = graph.propertySetsForObject(0u).front();
  EXPECT_EQ(group.set, "Pset_WallCommon");
  ASSERT_EQ(group.properties.size(), 1u);
  EXPECT_EQ(group.properties.front().name, "FireRating");
  EXPECT_EQ(group.properties.front().value, "2HR");
}

TEST(BimRelationshipGraphTests, CancelDuringPropertyIndexing) {
  auto wall = makeElement(0u, "wall-guid", "wall-source", "IfcWall", "Wall");
  wall.properties.resize(4096u, {.set = "Pset_WallCommon", .name = "FireRating",
                                 .value = "2HR", .category = "IfcPropertySet"});
  const std::array elements{std::move(wall)};
  std::stop_source cancellation;
  const auto stop = cancellation.get_token();
  BimRelationshipGraph graph;
  size_t checkpoints = 0;
  EXPECT_THROW(graph.build(elements, {}, [&] {
    // The second build checkpoint is inside the first row's properties.
    if (++checkpoints == 2u)
      cancellation.request_stop();
    container::util::checkModelImportCancellation(stop);
  }), container::util::ModelImportCancelled);
  ASSERT_EQ(graph.propertySetsForObject(0u).size(), 1u);
  const auto processed = graph.propertySetsForObject(0u).front().properties.size();
  EXPECT_GT(processed, 0u);
  EXPECT_LT(processed, elements.front().properties.size());
}

class BimPreparationTests : public testing::Test {
protected:
  void SetUp() override {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fixture_ = std::filesystem::temp_directory_path() /
        ("container-bim-preparation-" + std::to_string(nonce) + ".bim");
    std::ofstream file(fixture_, std::ios::binary);
    file << R"json({
      "schema_version": "1.1.0",
      "meshes": [{"mesh_id": 1, "coordinates": [0,0,0, 2,0,0, 0,0,2],
                  "indices": [0,1,2]}],
      "elements": [{"mesh_id": 1, "guid": "wall-guid", "type": "IfcWall",
                     "storeyName": "Ground floor", "materialName": "Brick"}]
    })json";
    file.close();
    ASSERT_TRUE(file);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove(fixture_, ignored);
  }

  const std::filesystem::path &fixturePath() const { return fixture_; }

  void cancelAt(std::string_view target) const {
    const auto path = container::util::pathToUtf8(fixture_);
    std::stop_source cancellation;
    size_t observed = 0;
    EXPECT_THROW((void)container::renderer::BimManager::prepareScene(
        path, 1.0f, {}, cancellation.get_token(), [&](std::string_view stage) {
          if (stage == target) {
            ++observed;
            cancellation.request_stop();
          }
        }), container::util::ModelImportCancelled);
    EXPECT_EQ(observed, 1u);

    const auto replay = container::renderer::BimManager::prepareScene(path, 1.0f);
    ASSERT_EQ(replay.model.elements.size(), 1u);
    EXPECT_EQ(replay.model.elements.front().guid, "wall-guid");
    EXPECT_EQ(replay.floorPlanGround.indexCount, 2u);
    EXPECT_EQ(replay.floorPlanSourceElevation.indexCount, 2u);
    EXPECT_EQ(replay.optimizedMetadata.meshObjectCount, 1u);
  }

private:
  std::filesystem::path fixture_;
};

TEST_F(BimPreparationTests, CancelsDuringFloorPlanProjection) {
  cancelAt("Building BIM floor plans");
}

TEST_F(BimPreparationTests, CancelsDuringMetadataIndexing) {
  cancelAt("Indexing BIM objects");
}

TEST_F(BimPreparationTests, BuildsGroundAndStoreyFloorPlansInOrder) {
  std::ofstream file(fixturePath(), std::ios::binary);
  file << R"json({
    "schema_version": "1.1.0",
    "meshes": [
      {"mesh_id": 1, "coordinates": [0,0,0, 2,0,0, 0,0,2],
       "indices": [0,1,2]},
      {"mesh_id": 2, "coordinates": [0,0,0, 2,0,0, 0,2,0],
       "indices": [0,1,2]}
    ],
    "elements": [
      {"mesh_id": 1, "guid": "first", "type": "IfcWall",
       "storeyName": "Lower", "vector": {"z": 3}},
      {"mesh_id": 1, "guid": "second", "type": "IfcWall",
       "storeyId": "Lower", "vector": {"x": 4, "z": 1}},
      {"mesh_id": 1, "guid": "roof", "type": "IfcRoof",
       "storeyName": "Lower", "vector": {"x": -100, "z": -10}},
      {"mesh_id": 1, "guid": "upper", "type": "IfcWall",
       "storeyName": "Upper", "storeyId": "Lower", "vector": {"z": 7}},
      {"mesh_id": 1, "guid": "unassigned", "type": "IfcWall",
       "vector": {"x": 8, "y": -3, "z": 5}},
      {"mesh_id": 1, "guid": "duplicate", "type": "IfcWall",
       "storeyName": "Lower", "vector": {"z": 2}},
      {"mesh_id": 2, "guid": "slab", "type": "IfcSlab",
       "storeyName": "Lower", "vector": {"x": 12, "z": -2}}
    ]
  })json";
  file.close();
  ASSERT_TRUE(file);

  const auto scene = container::renderer::BimManager::prepareScene(
      container::util::pathToUtf8(fixturePath()), 1.0f);
  const auto &ground = scene.floorPlanGround;
  const auto &storeys = scene.floorPlanSourceElevation;
  const std::array groundPositions{
      glm::vec3{0, 0, 0}, glm::vec3{2, 0, 0}, glm::vec3{4, 0, 0},
      glm::vec3{6, 0, 0}, glm::vec3{8, 0, 3}, glm::vec3{10, 0, 3}};
  const std::array storeyPositions{
      glm::vec3{0, -2, 0}, glm::vec3{2, -2, 0}, glm::vec3{4, -2, 0},
      glm::vec3{6, -2, 0}, glm::vec3{0, 7, 0}, glm::vec3{2, 7, 0},
      glm::vec3{8, 5, 3}, glm::vec3{10, 5, 3}};
  const auto verify = [&](const auto &overlay, const auto &positions,
                          size_t firstVertex, size_t firstIndex) {
    EXPECT_EQ(overlay.firstIndex, firstIndex);
    ASSERT_EQ(overlay.indexCount, positions.size());
    ASSERT_LE(firstIndex + positions.size(), scene.uploadIndices.size());
    ASSERT_LE(firstVertex + positions.size(), scene.uploadVertices.size());
    for (size_t i = 0; i < positions.size(); ++i) {
      EXPECT_EQ(scene.uploadIndices[firstIndex + i], firstVertex + i);
      EXPECT_EQ(scene.uploadVertices[firstVertex + i].position, positions[i]);
    }
  };
  verify(ground, groundPositions, scene.model.vertices.size(),
         scene.model.indices.size());
  verify(storeys, storeyPositions,
         scene.model.vertices.size() + groundPositions.size(),
         scene.model.indices.size() + groundPositions.size());
  const size_t overlaySize = groundPositions.size() + storeyPositions.size();
  EXPECT_EQ(scene.uploadVertices.size(), scene.model.vertices.size() + overlaySize);
  EXPECT_EQ(scene.uploadIndices.size(), scene.model.indices.size() + overlaySize);
  EXPECT_EQ(ground.boundsCenter, (glm::vec3{5, 0, 1.5f}));
  EXPECT_EQ(storeys.boundsCenter, (glm::vec3{5, 2.5f, 1.5f}));
  EXPECT_NEAR(ground.boundsRadius * ground.boundsRadius, 27.25f, 0.00001f);
  EXPECT_NEAR(storeys.boundsRadius * storeys.boundsRadius, 47.5f, 0.00001f);
}

TEST_F(BimPreparationTests, DiscardsFloorPlanWithNoDistinctLines) {
  std::ofstream file(fixturePath(), std::ios::binary);
  file << R"json({
    "schema_version": "1.1.0",
    "meshes": [{"mesh_id": 1, "indices": [0,1,2],
      "coordinates": [-0.00049,0.00049,0, 0.00049,-0.00049,0, -0.00049,0.00049,2]}],
    "elements": [{"mesh_id": 1, "guid": "wall", "type": "IfcWall"}]
  })json";
  file.close();
  ASSERT_TRUE(file);

  const auto scene = container::renderer::BimManager::prepareScene(
      container::util::pathToUtf8(fixturePath()), 1.0f);
  EXPECT_EQ(scene.uploadIndices, scene.model.indices);
  EXPECT_EQ(scene.uploadVertices.size(), scene.model.vertices.size());
  for (const auto *overlay : {&scene.floorPlanGround, &scene.floorPlanSourceElevation}) {
    EXPECT_EQ(overlay->firstIndex, scene.model.indices.size());
    EXPECT_EQ(overlay->indexCount, 0u);
    EXPECT_EQ(overlay->boundsCenter, glm::vec3(0.0f));
    EXPECT_EQ(overlay->boundsRadius, 0.0f);
  }
}

} // namespace
