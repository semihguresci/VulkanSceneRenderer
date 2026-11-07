#pragma once

#include "Container/renderer/bim/BimRelationshipGraph.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace container::ui {

// Wrapped table rows have different heights, so only submit a bounded page.
// Every row remains reachable, including the last partial page.
class BimBrowserPage {
public:
  static constexpr size_t kRowsPerPage = 100u;

  [[nodiscard]] static size_t count(size_t rowCount) noexcept {
    return rowCount == 0u ? 1u : 1u + (rowCount - 1u) / kRowsPerPage;
  }
  void reset() noexcept {
    index_ = 0u;
    requestedPage_ = 1u;
    resetScroll_ = true;
  }
  void clamp(size_t rowCount) noexcept {
    const size_t clamped = std::min(index_, count(rowCount) - 1u);
    if (clamped != index_) {
      set(clamped, rowCount);
    }
  }
  void set(size_t page, size_t rowCount) noexcept {
    const size_t next = std::min(page, count(rowCount) - 1u);
    resetScroll_ = resetScroll_ || next != index_;
    index_ = next;
    requestedPage_ = static_cast<uint64_t>(index_) + 1u;
  }
  [[nodiscard]] uint64_t &requestedPage() noexcept { return requestedPage_; }
  [[nodiscard]] bool takeScrollReset() noexcept {
    const bool reset = resetScroll_;
    resetScroll_ = false;
    return reset;
  }
  void applyRequestedPage(size_t rowCount) noexcept {
    set(requestedPage_ > 0u ? static_cast<size_t>(requestedPage_ - 1u) : 0u,
        rowCount);
  }
  [[nodiscard]] size_t index() const noexcept { return index_; }
  [[nodiscard]] size_t begin(size_t rowCount) const noexcept {
    return std::min(index_, count(rowCount) - 1u) * kRowsPerPage;
  }
  [[nodiscard]] size_t end(size_t rowCount) const noexcept {
    const size_t first = begin(rowCount);
    return first + std::min(kRowsPerPage, rowCount - first);
  }

private:
  size_t index_{0u};
  uint64_t requestedPage_{1u};
  bool resetScroll_{true};
};

struct BimPropertySetSummary {
  std::string set;
  std::string category;
  size_t propertyCount{0u};
};

// Own query results and compact graph edge indices, never graph spans. The
// explicit publication revision also handles replacement at reused addresses.
class BimRelationshipBrowserCache {
public:
  bool refresh(const void *source, uint64_t metadataRevision,
               const container::renderer::BimRelationshipGraph *graph) {
    if (initialized_ && source_ == source &&
        metadataRevision_ == metadataRevision && graph_ == graph) {
      return false;
    }
    clear();
    source_ = source;
    metadataRevision_ = metadataRevision;
    graph_ = graph;
    initialized_ = true;
    return true;
  }

  void clear() { *this = {}; }

  [[nodiscard]] std::span<
      const container::renderer::BimRelationshipSearchResult>
  search(std::string_view query) {
    if (!searchReady_ || query_ != query) {
      query_ = query;
      searchResults_ =
          graph_ != nullptr
              ? graph_->search(query)
              : std::vector<container::renderer::BimRelationshipSearchResult>{};
      searchReady_ = true;
      searchPage.reset();
    }
    return searchResults_;
  }

  void selectObject(uint32_t objectIndex) {
    if (selectedObjectIndex_ == objectIndex) {
      return;
    }
    selectedObjectIndex_ = objectIndex;
    selectedEdgesReady_ = false;
    selectedEdgeIndices_.clear();
    propertySummaryReady_ = false;
    propertySummary_.clear();
    selectedEdgesPage.reset();
    extendedPropertiesPage.reset();
    propertySetsPage.reset();
    propertySummaryPage.reset();
    propertyPages_.clear();
    propertyMatchesReady_ = false;
    propertyMatches_.clear();
  }

  [[nodiscard]] std::span<const size_t>
  selectedEdgeIndices(uint32_t objectIndex) {
    selectObject(objectIndex);
    if (!selectedEdgesReady_) {
      selectedEdgesReady_ = true;
      if (graph_ != nullptr &&
          objectIndex != std::numeric_limits<uint32_t>::max()) {
        const auto nodes = graph_->nodes();
        const auto edges = graph_->edges();
        for (size_t i = 0; i < edges.size(); ++i) {
          const auto &edge = edges[i];
          if (edge.from < nodes.size() && edge.to < nodes.size() &&
              (nodes[edge.from].objectIndex == objectIndex ||
               nodes[edge.to].objectIndex == objectIndex)) {
            selectedEdgeIndices_.push_back(i);
          }
        }
      }
    }
    return selectedEdgeIndices_;
  }

  [[nodiscard]] std::span<const size_t> spatialEdgeIndices() {
    if (!spatialEdgesReady_) {
      spatialEdgesReady_ = true;
      if (graph_ != nullptr) {
        const auto nodes = graph_->nodes();
        const auto edges = graph_->edges();
        for (size_t i = 0; i < edges.size(); ++i) {
          const auto &edge = edges[i];
          if (edge.kind ==
                  container::renderer::BimRelationshipKind::SpatialParent &&
              edge.from < nodes.size() && edge.to < nodes.size()) {
            spatialEdgeIndices_.push_back(i);
          }
        }
      }
    }
    return spatialEdgeIndices_;
  }

  template <typename Property>
  [[nodiscard]] std::span<const BimPropertySetSummary>
  propertySummary(uint32_t objectIndex, std::span<const Property> properties) {
    selectObject(objectIndex);
    if (!propertySummaryReady_) {
      std::unordered_map<std::string, std::unordered_map<std::string, size_t>>
          rows;
      for (const auto &property : properties) {
        const std::string set =
            property.set.empty() ? "(unassigned set)" : property.set;
        const std::string category = property.category.empty()
                                         ? "(unassigned category)"
                                         : property.category;
        auto &categories = rows[set];
        const auto [it, inserted] =
            categories.emplace(category, propertySummary_.size());
        if (inserted) {
          propertySummary_.push_back({set, category, 0u});
        }
        ++propertySummary_[it->second].propertyCount;
      }
      propertySummaryReady_ = true;
    }
    return propertySummary_;
  }

  template <typename Property, typename Matches>
  [[nodiscard]] std::span<const size_t>
  filteredPropertyIndices(uint32_t objectIndex,
                          std::span<const Property> properties,
                          std::string_view query, Matches &&matches) {
    selectObject(objectIndex);
    if (!propertyMatchesReady_ || propertyQuery_ != query) {
      propertyQuery_ = query;
      propertyMatches_.clear();
      for (size_t i = 0; i < properties.size(); ++i) {
        if (matches(properties[i], query)) {
          propertyMatches_.push_back(i);
        }
      }
      propertyMatchesReady_ = true;
      extendedPropertiesPage.reset();
    }
    return propertyMatches_;
  }

  BimBrowserPage &propertyPage(size_t groupIndex) {
    return propertyPages_[groupIndex];
  }

  template <typename StoreyRange>
  [[nodiscard]] int storeyIndexForLabel(std::span<const StoreyRange> storeys,
                                        std::string_view label) {
    if (!storeyLookupReady_ || storeyQuery_ != label) {
      storeyQuery_ = label;
      storeyLookupIndex_ = -1;
      const size_t count = std::min(
          storeys.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
      for (size_t i = 0; i < count; ++i) {
        if (storeys[i].label == label) {
          storeyLookupIndex_ = static_cast<int>(i);
          break;
        }
      }
      storeyLookupReady_ = true;
    }
    return storeyLookupIndex_;
  }

  BimBrowserPage searchPage;
  BimBrowserPage selectedEdgesPage;
  BimBrowserPage spatialEdgesPage;
  BimBrowserPage storeysPage;
  BimBrowserPage propertySetsPage;
  BimBrowserPage propertySummaryPage;
  BimBrowserPage extendedPropertiesPage;

private:
  bool initialized_{false};
  const void *source_{nullptr};
  uint64_t metadataRevision_{0u};
  const container::renderer::BimRelationshipGraph *graph_{nullptr};
  bool searchReady_{false};
  std::string query_;
  std::vector<container::renderer::BimRelationshipSearchResult> searchResults_;
  uint32_t selectedObjectIndex_{std::numeric_limits<uint32_t>::max()};
  bool selectedEdgesReady_{false};
  std::vector<size_t> selectedEdgeIndices_;
  bool spatialEdgesReady_{false};
  std::vector<size_t> spatialEdgeIndices_;
  bool propertySummaryReady_{false};
  std::vector<BimPropertySetSummary> propertySummary_;
  std::unordered_map<size_t, BimBrowserPage> propertyPages_;
  bool propertyMatchesReady_{false};
  std::string propertyQuery_;
  std::vector<size_t> propertyMatches_;
  bool storeyLookupReady_{false};
  std::string storeyQuery_;
  int storeyLookupIndex_{-1};
};

} // namespace container::ui
