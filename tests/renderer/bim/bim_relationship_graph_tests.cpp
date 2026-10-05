#include "Container/renderer/bim/BimRelationshipGraph.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>

#include <algorithm>
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

} // namespace
