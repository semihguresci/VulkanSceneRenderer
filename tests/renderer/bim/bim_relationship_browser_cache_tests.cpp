#include "Container/utility/BimRelationshipBrowserCache.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using container::renderer::BimElementMetadata;
using container::renderer::BimElementProperty;
using container::renderer::BimRelationshipGraph;
using container::renderer::BimRelationshipKind;
using container::ui::BimBrowserPage;
using container::ui::BimRelationshipBrowserCache;

std::vector<BimElementMetadata> makeElements(size_t childCount = 205u) {
  std::vector<BimElementMetadata> elements(childCount + 1u);
  elements[0].objectIndex = 0u;
  elements[0].guid = "building";
  elements[0].displayName = "HQ";
  elements[0].type = "IfcBuilding";
  for (size_t i = 1u; i < elements.size(); ++i) {
    elements[i].objectIndex = static_cast<uint32_t>(i);
    elements[i].guid = "wall-" + std::to_string(i);
    elements[i].displayName = "Wall " + std::to_string(i);
    elements[i].type = "IfcWall";
  }
  return elements;
}

void buildGraph(BimRelationshipGraph &graph,
                const std::vector<BimElementMetadata> &elements) {
  std::vector<container::geometry::dotbim::ElementRelationship> relationships;
  for (size_t i = 1u; i < elements.size(); ++i) {
    relationships.push_back({.fromGuid = elements[0].guid,
                             .toGuid = elements[i].guid,
                             .kind = "spatial",
                             .label = "contains"});
  }
  graph.build(elements, relationships);
}

TEST(BimRelationshipBrowserCacheTests,
     PagesReachEveryEntryAndTheFinalPartialPage) {
  constexpr size_t rowCount = 100001u;
  BimBrowserPage page;
  size_t visited = 0u;
  for (size_t i = 0; i < BimBrowserPage::count(rowCount); ++i) {
    page.set(i, rowCount);
    EXPECT_EQ(page.begin(rowCount), visited);
    EXPECT_LE(page.end(rowCount) - page.begin(rowCount), 100u);
    visited = page.end(rowCount);
  }
  EXPECT_EQ(visited, rowCount);
  EXPECT_EQ(page.begin(rowCount), 100000u);
  EXPECT_EQ(page.end(rowCount), 100001u);
  page.requestedPage() = 1u;
  page.applyRequestedPage(rowCount);
  EXPECT_EQ(page.begin(rowCount), 0u);
  page.requestedPage() = 1001u;
  page.applyRequestedPage(rowCount);
  EXPECT_EQ(page.begin(rowCount), 100000u);
}

TEST(BimRelationshipBrowserCacheTests,
     EmptyShrinkingAndExtremePagesStayInRange) {
  BimBrowserPage page;
  EXPECT_EQ(BimBrowserPage::count(0u), 1u);
  EXPECT_EQ(page.begin(0u), 0u);
  EXPECT_EQ(page.end(0u), 0u);
  const size_t maximum = std::numeric_limits<size_t>::max();
  page.set(maximum, maximum);
  EXPECT_LE(page.begin(maximum), maximum);
  EXPECT_EQ(page.end(maximum), maximum);
  page.clamp(7u);
  EXPECT_EQ(page.index(), 0u);
  EXPECT_EQ(page.end(7u), 7u);
  page.requestedPage() = 0u;
  page.applyRequestedPage(7u);
  EXPECT_EQ(page.index(), 0u);
  page.requestedPage() = std::numeric_limits<uint64_t>::max();
  page.applyRequestedPage(205u);
  EXPECT_EQ(page.begin(205u), 200u);
  EXPECT_EQ(page.end(205u), 205u);
  page.clamp(0u);
  EXPECT_EQ(page.index(), 0u);
  EXPECT_EQ(page.end(0u), 0u);
}

TEST(BimRelationshipBrowserCacheTests,
     CachedResultsMatchGraphAndRetainStorage) {
  int source{};
  BimRelationshipGraph graph;
  const auto elements = makeElements();
  buildGraph(graph, elements);
  BimRelationshipBrowserCache cache;
  ASSERT_TRUE(cache.refresh(&source, 7u, &graph));
  const auto expectedSearch = graph.search("Wall");
  const auto search = cache.search("Wall");
  ASSERT_EQ(search.size(), expectedSearch.size());
  ASSERT_GT(search.size(), 200u);
  for (size_t i = 0u; i < search.size(); ++i) {
    EXPECT_EQ(search[i].objectIndex, expectedSearch[i].objectIndex);
    EXPECT_EQ(search[i].matchedText, expectedSearch[i].matchedText);
    EXPECT_EQ(search[i].reason, expectedSearch[i].reason);
  }
  const auto selected = cache.selectedEdgeIndices(0u);
  const auto expectedSelected = graph.edgesForObject(0u);
  ASSERT_EQ(selected.size(), expectedSelected.size());
  ASSERT_GT(selected.size(), 200u);
  for (size_t i = 0u; i < selected.size(); ++i) {
    const auto &actual = graph.edges()[selected[i]];
    EXPECT_EQ(actual.from, expectedSelected[i].from);
    EXPECT_EQ(actual.to, expectedSelected[i].to);
    EXPECT_EQ(actual.kind, expectedSelected[i].kind);
    EXPECT_EQ(actual.label, expectedSelected[i].label);
  }
  const auto spatial = cache.spatialEdgeIndices();
  ASSERT_EQ(spatial.size(), 205u);
  for (const size_t index : spatial) {
    EXPECT_EQ(graph.edges()[index].kind, BimRelationshipKind::SpatialParent);
  }
  cache.searchPage.set(2u, search.size());
  cache.selectedEdgesPage.set(2u, selected.size());
  cache.spatialEdgesPage.set(2u, spatial.size());
  for (int frame = 0; frame < 120; ++frame) {
    EXPECT_FALSE(cache.refresh(&source, 7u, &graph));
    EXPECT_EQ(cache.search("Wall").data(), search.data());
    EXPECT_EQ(cache.selectedEdgeIndices(0u).data(), selected.data());
    EXPECT_EQ(cache.spatialEdgeIndices().data(), spatial.data());
  }
  EXPECT_EQ(cache.searchPage.index(), 2u);
  EXPECT_EQ(cache.selectedEdgesPage.index(), 2u);
  EXPECT_EQ(cache.spatialEdgesPage.index(), 2u);
  EXPECT_EQ(graph.edges()[spatial.back()].to,
            graph.edges()[cache.spatialEdgeIndices().back()].to);
}

TEST(BimRelationshipBrowserCacheTests,
     PublicationReplacesGraphAtTheSameAddress) {
  int source{};
  BimRelationshipGraph graph;
  auto elements = makeElements();
  buildGraph(graph, elements);
  BimRelationshipBrowserCache cache;
  cache.refresh(&source, 1u, &graph);
  ASSERT_FALSE(cache.search("Wall 205").empty());
  const auto spatialCount = cache.spatialEdgeIndices().size();
  cache.spatialEdgesPage.set(2u, spatialCount);
  cache.searchPage.set(2u, cache.search("Wall").size());
  (void)cache.selectedEdgeIndices(0u);
  elements = makeElements(1u);
  elements[1].guid = "replacement-beam-1";
  elements[1].displayName = "Replacement Beam";
  elements[1].type = "IfcBeam";
  buildGraph(graph, elements);
  // GUIDs are searchable too; the replacement must remove all old wall data.
  ASSERT_TRUE(graph.search("Wall").empty());
  ASSERT_TRUE(cache.refresh(&source, 2u, &graph));
  EXPECT_TRUE(cache.search("Wall").empty());
  EXPECT_TRUE(cache.search("Wall 205").empty());
  EXPECT_FALSE(cache.search("Replacement Beam").empty());
  EXPECT_EQ(cache.spatialEdgeIndices().size(), 1u);
  EXPECT_EQ(cache.selectedEdgeIndices(0u).size(),
            graph.edgesForObject(0u).size());
  EXPECT_EQ(cache.spatialEdgesPage.index(), 0u);
  EXPECT_EQ(cache.searchPage.index(), 0u);
}

TEST(BimRelationshipBrowserCacheTests,
     SourceGraphAndExplicitClearInvalidatePages) {
  int first{}, second{};
  BimRelationshipGraph firstGraph, secondGraph;
  const auto elements = makeElements();
  buildGraph(firstGraph, elements);
  buildGraph(secondGraph, elements);
  BimRelationshipBrowserCache cache;
  cache.refresh(&first, 1u, &firstGraph);
  cache.searchPage.set(2u, cache.search("Wall").size());
  EXPECT_TRUE(cache.refresh(&second, 1u, &firstGraph));
  EXPECT_EQ(cache.searchPage.index(), 0u);
  cache.storeysPage.set(2u, 205u);
  EXPECT_TRUE(cache.refresh(&second, 1u, &secondGraph));
  EXPECT_EQ(cache.storeysPage.index(), 0u);
  cache.clear();
  EXPECT_TRUE(cache.search("Wall").empty());
  EXPECT_TRUE(cache.selectedEdgeIndices(0u).empty());
  EXPECT_TRUE(cache.spatialEdgeIndices().empty());
  EXPECT_TRUE(cache.refresh(&second, 1u, &secondGraph));
  EXPECT_FALSE(cache.search("Wall").empty());
}

TEST(BimRelationshipBrowserCacheTests, EmptyGraphAndMissingSelectionAreNormal) {
  BimRelationshipGraph graph;
  BimRelationshipBrowserCache cache;
  EXPECT_TRUE(cache.refresh(nullptr, 0u, nullptr));
  EXPECT_FALSE(cache.refresh(nullptr, 0u, nullptr));
  EXPECT_TRUE(cache.search("wall").empty());
  EXPECT_TRUE(cache.spatialEdgeIndices().empty());
  EXPECT_TRUE(cache.selectedEdgeIndices(0u).empty());
  EXPECT_TRUE(cache.refresh(nullptr, 0u, &graph));
  EXPECT_TRUE(
      cache.selectedEdgeIndices(std::numeric_limits<uint32_t>::max()).empty());
  EXPECT_TRUE(cache.spatialEdgeIndices().empty());
  EXPECT_TRUE(cache.search("").empty());
}

TEST(BimRelationshipBrowserCacheTests,
     PropertySearchScansOnlyWhenTheKeyChanges) {
  int source{};
  std::vector<BimElementProperty> properties(205u);
  for (size_t i = 0u; i < properties.size(); ++i) {
    properties[i].value = "Value " + std::to_string(i);
  }
  BimRelationshipBrowserCache cache;
  cache.refresh(&source, 1u, nullptr);
  size_t predicates = 0u;
  const auto matches = [&](const BimElementProperty &property,
                           std::string_view query) {
    ++predicates;
    return property.value.find(query) != std::string::npos;
  };
  const std::span<const BimElementProperty> values(properties);
  const auto all = cache.filteredPropertyIndices(1u, values, "Value", matches);
  ASSERT_EQ(all.size(), 205u);
  EXPECT_EQ(all.back(), 204u);
  cache.extendedPropertiesPage.set(2u, all.size());
  for (int frame = 0; frame < 120; ++frame) {
    EXPECT_EQ(
        cache.filteredPropertyIndices(1u, values, "Value", matches).data(),
        all.data());
  }
  EXPECT_EQ(predicates, 205u);
  EXPECT_EQ(cache.extendedPropertiesPage.begin(all.size()), 200u);
  const auto last =
      cache.filteredPropertyIndices(1u, values, "Value 204", matches);
  ASSERT_EQ(last.size(), 1u);
  EXPECT_EQ(last.front(), 204u);
  EXPECT_EQ(predicates, 410u);
  EXPECT_EQ(cache.extendedPropertiesPage.index(), 0u);
  EXPECT_TRUE(
      cache.filteredPropertyIndices(1u, values, "missing", matches).empty());
  (void)cache.filteredPropertyIndices(2u, values, "Value", matches);
  EXPECT_EQ(predicates, 820u);
  cache.refresh(&source, 2u, nullptr);
  (void)cache.filteredPropertyIndices(2u, values, "Value", matches);
  EXPECT_EQ(predicates, 1025u);
}

TEST(BimRelationshipBrowserCacheTests,
     PropertySummaryPreservesPairsFallbacksAndOrder) {
  const std::array properties{
      BimElementProperty{.set = "A\nB", .category = "C"},
      BimElementProperty{.set = "A", .category = "B\nC"},
      BimElementProperty{.set = "A\nB", .category = "C"},
      BimElementProperty{},
  };
  BimRelationshipBrowserCache cache;
  cache.refresh(nullptr, 1u, nullptr);
  const std::span<const BimElementProperty> values(properties);
  const auto summary = cache.propertySummary(0u, values);
  ASSERT_EQ(summary.size(), 3u);
  EXPECT_EQ(summary[0].set, "A\nB");
  EXPECT_EQ(summary[0].category, "C");
  EXPECT_EQ(summary[0].propertyCount, 2u);
  EXPECT_EQ(summary[1].set, "A");
  EXPECT_EQ(summary[1].category, "B\nC");
  EXPECT_EQ(summary[1].propertyCount, 1u);
  EXPECT_EQ(summary[2].set, "(unassigned set)");
  EXPECT_EQ(summary[2].category, "(unassigned category)");
  EXPECT_EQ(summary[2].propertyCount, 1u);
  EXPECT_EQ(cache.propertySummary(0u, values).data(), summary.data());
  const std::span<const BimElementProperty> empty;
  EXPECT_TRUE(cache.propertySummary(1u, empty).empty());
}

TEST(BimRelationshipBrowserCacheTests,
     SelectionAndQueryChangesResetTheirOwnPages) {
  BimRelationshipGraph graph;
  const auto elements = makeElements();
  buildGraph(graph, elements);
  BimRelationshipBrowserCache cache;
  cache.refresh(nullptr, 1u, &graph);
  (void)cache.selectedEdgeIndices(0u);
  cache.selectedEdgesPage.set(2u, 205u);
  cache.propertySetsPage.set(2u, 205u);
  cache.propertyPage(4u).set(2u, 205u);
  cache.spatialEdgesPage.set(2u, 205u);
  cache.searchPage.set(2u, cache.search("Wall").size());
  cache.selectObject(1u);
  EXPECT_EQ(cache.selectedEdgesPage.index(), 0u);
  EXPECT_EQ(cache.propertySetsPage.index(), 0u);
  EXPECT_EQ(cache.propertyPage(4u).index(), 0u);
  EXPECT_EQ(cache.spatialEdgesPage.index(), 2u);
  EXPECT_EQ(cache.searchPage.index(), 2u);
  EXPECT_TRUE(cache.search("").empty());
  EXPECT_EQ(cache.searchPage.index(), 0u);
  EXPECT_FALSE(cache.search("Wall").empty());
  EXPECT_EQ(cache.searchPage.index(), 0u);
}

TEST(BimRelationshipBrowserCacheTests, StoreyLookupTracksQueryAndPublication) {
  struct Storey {
    std::string label;
  };
  std::array storeys{Storey{"Level 01"}, Storey{"Level 02"}};
  BimRelationshipBrowserCache cache;
  cache.refresh(storeys.data(), 1u, nullptr);
  const std::span<const Storey> values(storeys);
  EXPECT_EQ(cache.storeyIndexForLabel(values, "Level 02"), 1);
  EXPECT_EQ(cache.storeyIndexForLabel(values, "missing"), -1);
  EXPECT_EQ(cache.storeyIndexForLabel(values, "missing"), -1);
  storeys[1].label = "Replacement";
  cache.refresh(storeys.data(), 2u, nullptr);
  EXPECT_EQ(cache.storeyIndexForLabel(values, "Level 02"), -1);
  EXPECT_EQ(cache.storeyIndexForLabel(values, "Replacement"), 1);
  const std::span<const Storey> empty;
  cache.refresh(nullptr, 3u, nullptr);
  EXPECT_EQ(cache.storeyIndexForLabel(empty, "Replacement"), -1);
}

} // namespace
