#include "Container/renderer/bim/BimRelationshipGraph.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/renderer/bim/BimManager.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace container::renderer {
namespace {

constexpr uint32_t kInvalidObjectIndex =
    std::numeric_limits<uint32_t>::max();
constexpr uint32_t kInvalidNodeIndex = std::numeric_limits<uint32_t>::max();

[[nodiscard]] std::string lowerAscii(std::string_view text) {
  std::string result(text);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

[[nodiscard]] bool containsCaseInsensitive(std::string_view text,
                                           std::string_view query) {
  if (query.empty()) {
    return true;
  }
  return lowerAscii(text).find(lowerAscii(query)) != std::string::npos;
}

[[nodiscard]] bool isValidObjectIndex(uint32_t objectIndex) {
  return objectIndex != kInvalidObjectIndex;
}

[[nodiscard]] std::string firstNonEmpty(std::initializer_list<std::string_view>
                                            values) {
  for (std::string_view value : values) {
    if (!value.empty()) {
      return std::string(value);
    }
  }
  return {};
}

[[nodiscard]] BimRelationshipKind
relationshipKindFromString(std::string_view text) {
  const std::string value = lowerAscii(text);
  if (value.find("system") != std::string::npos) {
    return BimRelationshipKind::SystemAssignment;
  }
  if (value.find("zone") != std::string::npos) {
    return BimRelationshipKind::ZoneAssignment;
  }
  if (value.find("classification") != std::string::npos ||
      value.find("classifies") != std::string::npos) {
    return BimRelationshipKind::Classification;
  }
  if (value.find("propertyset") != std::string::npos ||
      value.find("property_set") != std::string::npos ||
      value.find("pset") != std::string::npos) {
    return BimRelationshipKind::PropertySet;
  }
  if (value.find("material") != std::string::npos) {
    return BimRelationshipKind::MaterialAssignment;
  }
  if (value.find("type") != std::string::npos ||
      value.find("defines") != std::string::npos) {
    return BimRelationshipKind::TypeDefinition;
  }
  return BimRelationshipKind::SpatialParent;
}

[[nodiscard]] std::string defaultRelationshipLabel(BimRelationshipKind kind) {
  return std::string(bimRelationshipKindLabel(kind));
}

void appendIdentityMatches(
    const std::unordered_map<std::string, std::vector<uint32_t>> &index,
    std::string_view key, std::vector<uint32_t> &nodes) {
  if (key.empty()) {
    return;
  }
  const auto it = index.find(std::string(key));
  if (it == index.end()) {
    return;
  }
  // Identity indexes are appended in node order. Merge the two sorted lists
  // once rather than scanning every preceding match for each draw element.
  const auto previousSize = nodes.size();
  nodes.insert(nodes.end(), it->second.begin(), it->second.end());
  std::inplace_merge(nodes.begin(), nodes.begin() + previousSize, nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
}

[[nodiscard]] bool compatibleSyntheticClass(std::string_view requested,
                                            std::string_view existing) {
  if (requested.empty() || existing.empty() || requested == existing) {
    return true;
  }
  if (requested == "IfcTypeObject") {
    return lowerAscii(existing).find("type") != std::string::npos;
  }
  return false;
}

} // namespace

std::string_view bimRelationshipKindLabel(BimRelationshipKind kind) noexcept {
  switch (kind) {
  case BimRelationshipKind::SpatialParent:
    return "Spatial containment";
  case BimRelationshipKind::TypeDefinition:
    return "Type definition";
  case BimRelationshipKind::MaterialAssignment:
    return "Material assignment";
  case BimRelationshipKind::SystemAssignment:
    return "System assignment";
  case BimRelationshipKind::ZoneAssignment:
    return "Zone assignment";
  case BimRelationshipKind::Classification:
    return "Classification";
  case BimRelationshipKind::PropertySet:
    return "Property set";
  }
  return "Relationship";
}

void BimRelationshipGraph::clear() {
  nodes_.clear();
  edges_.clear();
  edgeIdentities_.clear();
  nodeByObjectIndex_.clear();
  nodeByGuid_.clear();
  nodeBySourceId_.clear();
  nodeByLabel_.clear();
  syntheticNodeByKey_.clear();
  propertySetsByObject_.clear();
  searchFields_.clear();
}

void BimRelationshipGraph::build(
    std::span<const BimElementMetadata> metadata,
    std::span<const container::geometry::dotbim::ElementRelationship>
        relationships) {
  clear();
  nodes_.reserve(metadata.size());
  nodeByObjectIndex_.reserve(metadata.size());
  nodeByGuid_.reserve(metadata.size());
  nodeBySourceId_.reserve(metadata.size());
  nodeByLabel_.reserve(metadata.size());
  propertySetsByObject_.reserve(metadata.size());
  edgeIdentities_.reserve(metadata.size());

  for (const BimElementMetadata &element : metadata) {
    (void)addObjectNode(element);
  }

  for (const BimElementMetadata &element : metadata) {
    const uint32_t elementNode = objectNodeIndex(element.objectIndex);
    if (elementNode == kInvalidNodeIndex) {
      continue;
    }

    const std::string storey =
        firstNonEmpty({element.storeyName, element.storeyId});
    if (!storey.empty()) {
      const uint32_t storeyNode =
          syntheticNode("storey:" + storey, storey, "IfcBuildingStorey", {},
                        element.storeyId);
      addEdge(storeyNode, elementNode, BimRelationshipKind::SpatialParent,
              defaultRelationshipLabel(BimRelationshipKind::SpatialParent));
    }

    const std::string material =
        firstNonEmpty({element.materialName, element.materialCategory});
    if (!material.empty()) {
      const uint32_t materialNode =
          syntheticNode("material:" + material, material, "IfcMaterial");
      addEdge(materialNode, elementNode, BimRelationshipKind::MaterialAssignment,
              defaultRelationshipLabel(
                  BimRelationshipKind::MaterialAssignment));
    }

    const std::string type = firstNonEmpty({element.objectType, element.type});
    if (!type.empty()) {
      const uint32_t typeNode =
          syntheticNode("type:" + type, type, "IfcTypeObject");
      addEdge(typeNode, elementNode, BimRelationshipKind::TypeDefinition,
              defaultRelationshipLabel(BimRelationshipKind::TypeDefinition));
    }

    buildPropertySets(element);
  }

  for (const container::geometry::dotbim::ElementRelationship &relationship :
       relationships) {
    const BimRelationshipKind kind =
        relationshipKindFromString(relationship.kind);
    const std::string label =
        firstNonEmpty({relationship.label, bimRelationshipKindLabel(kind)});
    const std::vector<uint32_t> fromNodes = resolveRelationshipEndpoints(
        relationship.fromGuid, relationship.fromSourceId, {}, kind);
    const std::vector<uint32_t> toNodes = resolveRelationshipEndpoints(
        relationship.toGuid, relationship.toSourceId, label, kind);
    for (const uint32_t from : fromNodes) {
      for (const uint32_t to : toNodes) {
        if (from == kInvalidNodeIndex || to == kInvalidNodeIndex) {
          continue;
        }
        addEdge(from, to, kind, label);
        if (isValidObjectIndex(nodes_[from].objectIndex)) {
          addSearchField(nodes_[from].objectIndex, label, "relationship");
          addSearchField(nodes_[from].objectIndex, nodes_[to].label,
                         "relationship");
        }
        if (isValidObjectIndex(nodes_[to].objectIndex)) {
          addSearchField(nodes_[to].objectIndex, label, "relationship");
          addSearchField(nodes_[to].objectIndex, nodes_[from].label,
                         "relationship");
        }
      }
    }
  }
}

std::vector<BimRelationshipNode>
BimRelationshipGraph::parentsForObject(uint32_t objectIndex) const {
  std::vector<BimRelationshipNode> result;
  const uint32_t nodeIndex = objectNodeIndex(objectIndex);
  if (nodeIndex == kInvalidNodeIndex) {
    return result;
  }
  for (const BimRelationshipEdge &edge : edges_) {
    if (edge.to == nodeIndex && edge.from < nodes_.size()) {
      result.push_back(nodes_[edge.from]);
    } else if (edge.from == nodeIndex &&
               edge.kind != BimRelationshipKind::SpatialParent &&
               edge.to < nodes_.size()) {
      result.push_back(nodes_[edge.to]);
    }
  }
  return result;
}

std::vector<BimRelationshipNode>
BimRelationshipGraph::childrenForObject(uint32_t objectIndex) const {
  std::vector<BimRelationshipNode> result;
  const uint32_t nodeIndex = objectNodeIndex(objectIndex);
  if (nodeIndex == kInvalidNodeIndex) {
    return result;
  }
  for (const BimRelationshipEdge &edge : edges_) {
    if (edge.from == nodeIndex && edge.to < nodes_.size()) {
      result.push_back(nodes_[edge.to]);
    }
  }
  return result;
}

std::vector<BimRelationshipEdge>
BimRelationshipGraph::edgesForObject(uint32_t objectIndex) const {
  std::vector<BimRelationshipEdge> result;
  const uint32_t nodeIndex = objectNodeIndex(objectIndex);
  if (nodeIndex == kInvalidNodeIndex) {
    return result;
  }
  for (const BimRelationshipEdge &edge : edges_) {
    if (edge.from == nodeIndex || edge.to == nodeIndex) {
      result.push_back(edge);
    }
  }
  return result;
}

std::vector<BimRelationshipSearchResult>
BimRelationshipGraph::search(std::string_view query) const {
  std::vector<BimRelationshipSearchResult> result;
  if (query.empty()) {
    return result;
  }
  result.reserve(16u);
  for (const SearchField &field : searchFields_) {
    if (!isValidObjectIndex(field.objectIndex) ||
        !containsCaseInsensitive(field.text, query)) {
      continue;
    }
    result.push_back(BimRelationshipSearchResult{
        .objectIndex = field.objectIndex,
        .matchedText = field.text,
        .reason = field.reason,
    });
  }
  return result;
}

std::span<const BimPropertySetGroup>
BimRelationshipGraph::propertySetsForObject(uint32_t objectIndex) const {
  const auto it = propertySetsByObject_.find(objectIndex);
  if (it == propertySetsByObject_.end()) {
    return {};
  }
  return it->second;
}

uint32_t BimRelationshipGraph::addNode(BimRelationshipNode node) {
  if (node.label.empty()) {
    node.label = firstNonEmpty({node.guid, node.sourceId, node.ifcClass});
  }
  if (node.label.empty() && isValidObjectIndex(node.objectIndex)) {
    node.label = "Object " + std::to_string(node.objectIndex);
  }
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
  nodes_.push_back(std::move(node));
  indexNodeIdentity(nodeIndex, nodes_.back());
  return nodeIndex;
}

uint32_t
BimRelationshipGraph::addObjectNode(const BimElementMetadata &metadata) {
  if (!isValidObjectIndex(metadata.objectIndex)) {
    return kInvalidNodeIndex;
  }
  const uint32_t nodeIndex = addNode(BimRelationshipNode{
      .objectIndex = metadata.objectIndex,
      .guid = metadata.guid,
      .sourceId = metadata.sourceId,
      .label = firstNonEmpty({metadata.displayName, metadata.objectType,
                              metadata.type, metadata.guid,
                              metadata.sourceId}),
      .ifcClass = metadata.type,
  });
  nodeByObjectIndex_[metadata.objectIndex] = nodeIndex;

  addSearchField(metadata.objectIndex, metadata.displayName, "element name");
  addSearchField(metadata.objectIndex, metadata.guid, "guid");
  addSearchField(metadata.objectIndex, metadata.sourceId, "source id");
  addSearchField(metadata.objectIndex, metadata.type, "IFC class");
  addSearchField(metadata.objectIndex, metadata.objectType, "IFC class");
  addSearchField(metadata.objectIndex, metadata.storeyName, "storey");
  addSearchField(metadata.objectIndex, metadata.storeyId, "storey");
  addSearchField(metadata.objectIndex, metadata.materialName, "material");
  addSearchField(metadata.objectIndex, metadata.materialCategory, "material");
  return nodeIndex;
}

uint32_t BimRelationshipGraph::syntheticNode(std::string key,
                                             std::string label,
                                             std::string ifcClass,
                                             std::string guid,
                                             std::string sourceId) {
  if (key.empty()) {
    key = firstNonEmpty({guid, sourceId, label, ifcClass});
  }
  if (key.empty()) {
    return kInvalidNodeIndex;
  }
  const uint32_t existingObjectNode =
      existingObjectNodeForSynthetic(label, ifcClass, guid, sourceId);
  if (existingObjectNode != kInvalidNodeIndex) {
    syntheticNodeByKey_[std::move(key)] = existingObjectNode;
    return existingObjectNode;
  }
  const auto existing = syntheticNodeByKey_.find(key);
  if (existing != syntheticNodeByKey_.end()) {
    return existing->second;
  }

  const uint32_t nodeIndex = addNode(BimRelationshipNode{
      .guid = std::move(guid),
      .sourceId = std::move(sourceId),
      .label = std::move(label),
      .ifcClass = std::move(ifcClass),
  });
  syntheticNodeByKey_[std::move(key)] = nodeIndex;
  return nodeIndex;
}

uint32_t BimRelationshipGraph::externalRelationshipNode(
    std::string_view guid, std::string_view sourceId, std::string_view label,
    BimRelationshipKind kind) {
  const std::string displayLabel =
      firstNonEmpty({label, guid, sourceId, bimRelationshipKindLabel(kind)});
  const std::string key =
      "external:" + std::string(bimRelationshipKindLabel(kind)) + ":" +
      firstNonEmpty({guid, sourceId, displayLabel});
  return syntheticNode(key, displayLabel,
                       std::string(bimRelationshipKindLabel(kind)),
                       std::string(guid), std::string(sourceId));
}

uint32_t BimRelationshipGraph::objectNodeIndex(uint32_t objectIndex) const {
  const auto it = nodeByObjectIndex_.find(objectIndex);
  return it != nodeByObjectIndex_.end() ? it->second : kInvalidNodeIndex;
}

std::vector<uint32_t> BimRelationshipGraph::resolveRelationshipEndpoints(
    std::string_view guid, std::string_view sourceId, std::string_view label,
    BimRelationshipKind kind) {
  std::vector<uint32_t> result;
  appendIdentityMatches(nodeByGuid_, guid, result);
  appendIdentityMatches(nodeBySourceId_, sourceId, result);
  if (!result.empty()) {
    return result;
  }
  const uint32_t external =
      externalRelationshipNode(guid, sourceId, label, kind);
  if (external != kInvalidNodeIndex) {
    result.push_back(external);
  }
  return result;
}

uint32_t BimRelationshipGraph::existingObjectNodeForSynthetic(
    std::string_view label, std::string_view ifcClass, std::string_view guid,
    std::string_view sourceId) const {
  std::vector<uint32_t> identityMatches;
  appendIdentityMatches(nodeByGuid_, guid, identityMatches);
  appendIdentityMatches(nodeBySourceId_, sourceId, identityMatches);
  for (const uint32_t nodeIndex : identityMatches) {
    if (nodeIndex < nodes_.size() &&
        isValidObjectIndex(nodes_[nodeIndex].objectIndex)) {
      return nodeIndex;
    }
  }

  if (label.empty()) {
    return kInvalidNodeIndex;
  }
  const auto labelIt = nodeByLabel_.find(std::string(label));
  if (labelIt == nodeByLabel_.end()) {
    return kInvalidNodeIndex;
  }
  for (const uint32_t nodeIndex : labelIt->second) {
    if (nodeIndex >= nodes_.size() ||
        !isValidObjectIndex(nodes_[nodeIndex].objectIndex)) {
      continue;
    }
    if (compatibleSyntheticClass(ifcClass, nodes_[nodeIndex].ifcClass)) {
      return nodeIndex;
    }
  }
  return kInvalidNodeIndex;
}

size_t BimRelationshipGraph::EdgeIdentityHash::operator()(
    const EdgeIdentity &edge) const noexcept {
  size_t hash = std::hash<std::string>{}(edge.label);
  for (uint32_t value :
       {edge.from, edge.to, static_cast<uint32_t>(edge.kind)}) {
    hash ^= std::hash<uint32_t>{}(value) + size_t{0x9e3779b9u} + (hash << 6u) +
            (hash >> 2u);
  }
  return hash;
}

void BimRelationshipGraph::addEdge(uint32_t from, uint32_t to,
                                   BimRelationshipKind kind,
                                   std::string label) {
  if (from == kInvalidNodeIndex || to == kInvalidNodeIndex ||
      from >= nodes_.size() || to >= nodes_.size()) {
    return;
  }
  if (label.empty()) {
    label = defaultRelationshipLabel(kind);
  }
  if (edgeIdentities_.insert(EdgeIdentity{from, to, kind, label}).second) {
    edges_.push_back(BimRelationshipEdge{
        .from = from,
        .to = to,
        .kind = kind,
        .label = std::move(label),
    });
  }
}

void BimRelationshipGraph::addSearchField(uint32_t objectIndex,
                                          std::string text,
                                          std::string reason) {
  if (!isValidObjectIndex(objectIndex) || text.empty()) {
    return;
  }
  searchFields_.push_back(SearchField{
      .objectIndex = objectIndex,
      .text = std::move(text),
      .reason = std::move(reason),
  });
}

void BimRelationshipGraph::indexNodeIdentity(
    uint32_t nodeIndex, const BimRelationshipNode &node) {
  // Each new node is indexed exactly once, so these lists remain sorted.
  if (!node.guid.empty()) {
    nodeByGuid_[node.guid].push_back(nodeIndex);
  }
  if (!node.sourceId.empty()) {
    nodeBySourceId_[node.sourceId].push_back(nodeIndex);
  }
  // Label matching is only used to reuse real object nodes. Property-set and
  // other synthetic nodes can share a label thousands of times; indexing
  // them here makes every new synthetic-node lookup scan all previous ones.
  if (!node.label.empty() && isValidObjectIndex(node.objectIndex)) {
    nodeByLabel_[node.label].push_back(nodeIndex);
  }
}

void BimRelationshipGraph::buildPropertySets(
    const BimElementMetadata &metadata) {
  if (!isValidObjectIndex(metadata.objectIndex) || metadata.properties.empty()) {
    return;
  }

  auto &groups = propertySetsByObject_[metadata.objectIndex];
  for (const BimElementProperty &property : metadata.properties) {
    const std::string set =
        !property.set.empty() ? property.set : "(unassigned set)";
    const std::string category = !property.category.empty()
                                     ? property.category
                                     : "(unassigned category)";
    auto groupIt = std::ranges::find_if(
        groups, [&](const BimPropertySetGroup &group) {
          return group.set == set && group.category == category;
        });
    if (groupIt == groups.end()) {
      groups.push_back(BimPropertySetGroup{.set = set, .category = category});
      groupIt = groups.end() - 1;
    }
    groupIt->properties.push_back(BimPropertySetProperty{
        .name = property.name,
        .value = property.value,
        .category = property.category,
    });

    addSearchField(metadata.objectIndex, set, "property set");
    addSearchField(metadata.objectIndex, property.name, "property name");
    addSearchField(metadata.objectIndex, property.value, "property value");
  }

  const uint32_t elementNode = objectNodeIndex(metadata.objectIndex);
  for (const BimPropertySetGroup &group : groups) {
    const std::string key = "pset:" + std::to_string(metadata.objectIndex) +
                            ":" + group.set + ":" + group.category;
    const uint32_t propertySetNode =
        syntheticNode(key, group.set, "IfcPropertySet");
    addEdge(propertySetNode, elementNode, BimRelationshipKind::PropertySet,
            group.category.empty() ? group.set : group.category);
  }
}

} // namespace container::renderer
