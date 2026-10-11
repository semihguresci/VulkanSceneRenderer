#include "Container/renderer/bim/BimRelationshipGraph.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>

#include <algorithm>
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

} // namespace
