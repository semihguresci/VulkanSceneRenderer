#pragma once

#include "Container/renderer/bim/BimMetadata.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace container::geometry::dotbim {
struct ElementRelationship;
} // namespace container::geometry::dotbim

namespace container::renderer {

enum class BimRelationshipKind : uint32_t {
  SpatialParent,
  TypeDefinition,
  MaterialAssignment,
  SystemAssignment,
  ZoneAssignment,
  Classification,
  PropertySet,
};

struct BimRelationshipNode {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  std::string guid{};
  std::string sourceId{};
  std::string label{};
  std::string ifcClass{};
};

struct BimRelationshipEdge {
  uint32_t from{0};
  uint32_t to{0};
  BimRelationshipKind kind{BimRelationshipKind::SpatialParent};
  std::string label{};
};

struct BimRelationshipSearchResult {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  std::string matchedText{};
  std::string reason{};
};

struct BimPropertySetProperty {
  std::string name{};
  std::string value{};
  std::string category{};
};

struct BimPropertySetGroup {
  std::string set{};
  std::string category{};
  std::vector<BimPropertySetProperty> properties{};
};

class BimRelationshipGraph {
public:
  void clear(const std::function<void()> &progress = {});
  void build(
      BimMetadataTableView metadata,
      std::span<const container::geometry::dotbim::ElementRelationship>
          relationships = {},
      const std::function<void()> &progress = {});

  [[nodiscard]] std::span<const BimRelationshipNode> nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] std::span<const BimRelationshipEdge> edges() const noexcept {
    return edges_;
  }

  [[nodiscard]] std::vector<BimRelationshipNode>
  parentsForObject(uint32_t objectIndex) const;
  [[nodiscard]] std::vector<BimRelationshipNode>
  childrenForObject(uint32_t objectIndex) const;
  [[nodiscard]] std::vector<BimRelationshipEdge>
  edgesForObject(uint32_t objectIndex) const;
  [[nodiscard]] std::vector<BimRelationshipSearchResult>
  search(std::string_view query) const;
  [[nodiscard]] std::span<const BimPropertySetGroup>
  propertySetsForObject(uint32_t objectIndex) const;

private:
  struct StringHash {
    using is_transparent = void;

    size_t operator()(std::string_view value) const noexcept {
      return std::hash<std::string_view>{}(value);
    }
  };
  template <typename Value>
  using StringIndex =
      std::unordered_map<std::string, Value, StringHash, std::equal_to<>>;
  using NodeIndex = StringIndex<std::vector<uint32_t>>;

  struct EdgeIdentity {
    uint32_t from;
    uint32_t to;
    BimRelationshipKind kind;
    std::string label;
    bool operator==(const EdgeIdentity &) const = default;
  };
  struct EdgeIdentityHash {
    size_t operator()(const EdgeIdentity &edge) const noexcept;
  };

  struct SearchField {
    uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
    std::string text{};
    std::string reason{};
  };

  [[nodiscard]] uint32_t addNode(BimRelationshipNode node);
  [[nodiscard]] uint32_t addObjectNode(const BimElementMetadataView &metadata);
  [[nodiscard]] uint32_t syntheticNode(std::string key, std::string label,
                                       std::string ifcClass,
                                       std::string guid = {},
                                       std::string sourceId = {});
  [[nodiscard]] uint32_t externalRelationshipNode(std::string_view guid,
                                                  std::string_view sourceId,
                                                  std::string_view label,
                                                  BimRelationshipKind kind);
  [[nodiscard]] uint32_t objectNodeIndex(uint32_t objectIndex) const;
  [[nodiscard]] static std::span<const uint32_t>
  identityMatches(const NodeIndex &index, std::string_view key);
  void resolveRelationshipEndpoints(
      std::string_view guid, std::string_view sourceId, std::string_view label,
      BimRelationshipKind kind, std::vector<uint32_t> &result);
  [[nodiscard]] uint32_t existingObjectNodeForSynthetic(
      std::string_view label, std::string_view ifcClass,
      std::string_view guid, std::string_view sourceId);
  void addEdge(uint32_t from, uint32_t to, BimRelationshipKind kind,
               std::string label);
  void addSearchField(uint32_t objectIndex, std::string_view text,
                      std::string reason);
  void indexNodeIdentity(uint32_t nodeIndex, const BimRelationshipNode &node);
  void buildPropertySets(const BimElementMetadataView &metadata,
                         const std::function<void()> &checkpoint);

  std::vector<BimRelationshipNode> nodes_{};
  std::vector<BimRelationshipEdge> edges_{};
  std::unordered_set<EdgeIdentity, EdgeIdentityHash> edgeIdentities_{};
  std::unordered_map<uint32_t, uint32_t> nodeByObjectIndex_{};
  NodeIndex nodeByGuid_{};
  NodeIndex nodeBySourceId_{};
  NodeIndex nodeByLabel_{};
  std::unordered_map<uint32_t, StringIndex<uint32_t>> sharedLabelMatches_{};
  std::unordered_map<std::string, uint32_t> syntheticNodeByKey_{};
  std::unordered_map<uint32_t, std::vector<BimPropertySetGroup>>
      propertySetsByObject_{};
  std::vector<SearchField> searchFields_{};
};

[[nodiscard]] std::string_view
bimRelationshipKindLabel(BimRelationshipKind kind) noexcept;

} // namespace container::renderer
