#include "Container/robotics/BimSensorLabels.h"

#include "Container/geometry/DotBimLoader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace container::robotics {
namespace {

namespace bim = geometry::dotbim;
constexpr std::string_view kUnclassified = "UNCLASSIFIED";

std::string canonicalClass(std::string_view name) {
  std::string key;
  for (const char c : name) {
    if (std::string_view(" \t\r\n\f\v").contains(c)) continue;
    key.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c);
  }
  return key.empty() ? std::string(kUnclassified) : key;
}

std::string idComponent(std::string_view value) {
  return std::to_string(value.size()) + ":" + std::string(value);
}

uint32_t nextId(size_t size) {
  if (size >= std::numeric_limits<uint32_t>::max())
    throw std::length_error("BIM sensor label IDs exceed their uint32 capacity");
  return static_cast<uint32_t>(size + 1);
}

SemanticGeometryKind geometryKind(bim::GeometryKind kind) {
  switch (kind) {
    case bim::GeometryKind::Mesh: return SemanticGeometryKind::Mesh;
    case bim::GeometryKind::Points: return SemanticGeometryKind::Points;
    case bim::GeometryKind::Curves: return SemanticGeometryKind::Curves;
  }
  throw std::invalid_argument("BIM sensor labels encountered an unknown geometry kind");
}

using RepresentationKey = std::tuple<std::string_view, uint32_t, SemanticGeometryKind>;

struct RepresentationOwner {
  RepresentationKey key;
  const SemanticEntity* entity;
  const SemanticRepresentation* representation;
};

struct SourceMatch {
  const SemanticEntity* entity{};
  bool ambiguous{};
};

SourceMatch matchElement(const bim::Element& element, const std::vector<RepresentationOwner>& owners) {
  const auto range = std::ranges::equal_range(owners,
      RepresentationKey{element.sourceId, element.meshId, geometryKind(element.geometryKind)},
      {}, &RepresentationOwner::key);
  const glm::dmat4 placement(element.transform);
  const auto& sourceId = element.semanticSourceId.empty() ? element.sourceId : element.semanticSourceId;
  SourceMatch result;
  int bestRank = -1;
  for (const auto& owner : range) {
    if (owner.representation->rendererWorldFromLocal != placement)
      continue;
    const bool sameSource = !sourceId.empty() && owner.entity->sourceId == sourceId;
    const bool sameGuid = !element.guid.empty() && owner.entity->guid == element.guid;
    // IFCX render rows can carry a fallback GUID. The explicitly authored
    // occurrence and representation take precedence over that render field.
    if ((!element.semanticSourceId.empty() && !sameSource) ||
        (!sameSource && !sameGuid && !element.guid.empty() && !owner.entity->guid.empty()))
      continue;
    const int rank = 2 * int(sameSource) + int(sameGuid);
    if (rank > bestRank) {
      result = {owner.entity, false};
      bestRank = rank;
    } else if (rank == bestRank && owner.entity != result.entity) {
      result.ambiguous = true;
    }
  }
  return result;
}

struct PendingInstance {
  BimSensorInstance row;
  std::string className;
};

struct StandaloneRepresentation {
  const bim::Element* element;
  bool ambiguous{};
};

} // namespace

BimSensorLabelMap buildBimSensorLabels(const bim::Model& model, const RoboticsScene& scene) {
  if (scene.sourceNamespace.empty())
    throw std::invalid_argument("BIM sensor label source namespace must identify the asset");
  if (model.elements.size() > std::numeric_limits<uint32_t>::max())
    throw std::length_error("BIM sensor label element rows exceed their uint32 capacity");

  BimSensorLabelMap result;
  result.sourceNamespace = scene.sourceNamespace;
  result.metersPerRendererUnit = scene.frame.metersPerRendererUnit;
  result.elements.resize(model.elements.size());
  const auto diagnostic = [&](std::string_view code, std::string_view sourceId, std::string message) {
    result.diagnostics.push_back({std::string(code), std::string(sourceId), std::move(message)});
  };

  std::map<std::string, PendingInstance, std::less<>> pending;
  std::map<std::string_view, size_t> guidCounts;
  std::vector<RepresentationOwner> owners;
  for (const auto& entity : scene.entities) {
    if (entity.id.empty())
      throw std::invalid_argument("BIM sensor label entity identity is empty");
    const auto [entry, inserted] = pending.try_emplace(entity.id);
    if (!inserted)
      throw std::invalid_argument("Duplicate BIM sensor label entity identity " + entity.id);
    auto& instance = entry->second;
    instance.row.entityId = entity.id;
    instance.row.guid = entity.guid;
    instance.row.occurrenceId = entity.sourceId;
    instance.className = canonicalClass(entity.ifcClass);
    if (!entity.guid.empty()) ++guidCounts[entity.guid];
    for (const auto& representation : entity.representations) {
      if (!representation.sourceId.empty())
        instance.row.representationIds.push_back(representation.sourceId);
      owners.push_back({{representation.sourceId, representation.meshId, representation.geometryKind},
                        &entity, &representation});
    }
    auto& representations = instance.row.representationIds;
    std::ranges::sort(representations);
    representations.erase(std::unique(representations.begin(), representations.end()), representations.end());
  }
  std::ranges::sort(owners, {}, &RepresentationOwner::key);
  for (const auto& entity : scene.entities)
    if (!entity.guid.empty() && guidCounts.at(entity.guid) > 1)
      diagnostic("shared-guid-occurrences", entity.sourceId,
                 "GUID " + entity.guid + " occurs in multiple logical instances.");

  // A standalone source representation can produce several material batches.
  // Its stable ID is ambiguous if reused at another placement or as another
  // geometry kind.
  std::map<std::string_view, StandaloneRepresentation> standalone;
  for (const auto& element : model.elements) {
    if (!element.isRepresentationOnly || element.sourceId.empty())
      continue;
    auto [entry, inserted] = standalone.try_emplace(element.sourceId, StandaloneRepresentation{&element});
    if (!inserted) {
      const auto& first = *entry->second.element;
      entry->second.ambiguous |= first.geometryKind != element.geometryKind ||
          first.transform != element.transform;
    }
  }
  const auto standalonePrefix = "bim/" + idComponent(scene.sourceNamespace) + "/representation/";
  for (const auto& [source, representation] : standalone) {
    if (representation.ambiguous) {
      diagnostic("ambiguous-standalone-representation", source,
                 "One standalone representation ID identifies conflicting drawable geometry.");
      continue;
    }
    const auto entityId = standalonePrefix + idComponent(source);
    const auto [entry, inserted] = pending.try_emplace(entityId);
    if (!inserted)
      throw std::invalid_argument("Duplicate BIM sensor label entity identity " + entityId);
    entry->second.row.entityId = entityId;
    entry->second.row.occurrenceId = source;
    entry->second.row.representationIds.emplace_back(source);
    entry->second.className = kUnclassified;
  }

  std::set<std::string_view> classNames;
  for (const auto& [id, instance] : pending)
    if (instance.className != kUnclassified) classNames.insert(instance.className);
  std::map<std::string_view, uint32_t> classIds{{kUnclassified, 1}};
  result.classes.push_back({1, std::string(kUnclassified)});
  for (const auto name : classNames) {
    const auto id = nextId(result.classes.size());
    classIds.emplace(name, id);
    result.classes.push_back({id, std::string(name)});
  }
  size_t instanceCount = 0;
  for (auto& [id, instance] : pending) {
    instance.row.instanceId = nextId(instanceCount++);
    instance.row.semanticId = classIds.at(instance.className);
  }

  for (size_t i = 0; i < model.elements.size(); ++i) {
    const auto& element = model.elements[i];
    const BimSensorInstance* instance = nullptr;
    if (element.isRepresentationOnly) {
      if (element.sourceId.empty()) {
        diagnostic("missing-standalone-identity", {}, "A standalone drawable has no stable source representation ID.");
      } else if (!standalone.at(element.sourceId).ambiguous) {
        instance = &pending.at(standalonePrefix + idComponent(element.sourceId)).row;
      }
    } else {
      const auto match = matchElement(element, owners);
      if (match.ambiguous || !match.entity) {
        diagnostic(match.ambiguous ? "ambiguous-element" : "unmatched-element", element.sourceId,
            match.ambiguous ? "The drawable matches multiple semantic occurrences."
                            : "The drawable does not match a semantic representation in this scene.");
      } else {
        instance = &pending.at(match.entity->id).row;
      }
    }
    if (instance)
      result.elements[i] = {instance->instanceId, instance->semanticId};
  }
  result.instances.reserve(pending.size());
  for (auto& [id, instance] : pending)
    result.instances.push_back(std::move(instance.row));

  const auto diagnosticKey = [](const RoboticsDiagnostic& entry) {
    return std::tie(entry.code, entry.sourceId, entry.message);
  };
  std::ranges::sort(result.diagnostics, {}, diagnosticKey);
  result.diagnostics.erase(std::unique(result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto& a, const auto& b) { return diagnosticKey(a) == diagnosticKey(b); }), result.diagnostics.end());
  return result;
}

std::string serializeBimSensorLabelManifest(const BimSensorLabelMap& labels) {
  using Json = nlohmann::json;
  Json classes = Json::array(), instances = Json::array(), diagnostics = Json::array();
  for (const auto& row : labels.classes)
    classes.push_back({{"semanticId", row.semanticId}, {"ifcClass", row.ifcClass}});
  for (const auto& row : labels.instances)
    instances.push_back({{"instanceId", row.instanceId}, {"semanticId", row.semanticId},
        {"entityId", row.entityId}, {"guid", row.guid}, {"occurrenceId", row.occurrenceId},
        {"representationIds", row.representationIds}});
  for (const auto& entry : labels.diagnostics)
    diagnostics.push_back({{"code", entry.code}, {"sourceId", entry.sourceId}, {"message", entry.message}});
  return Json{{"schema", "container.bim-sensor-labels"}, {"version", 1}, {"backgroundId", 0},
      {"sourceNamespace", labels.sourceNamespace}, {"classes", std::move(classes)},
      {"metersPerRendererUnit", labels.metersPerRendererUnit ? Json(*labels.metersPerRendererUnit) : Json(nullptr)},
      {"instances", std::move(instances)}, {"diagnostics", std::move(diagnostics)}}.dump(2) + '\n';
}

} // namespace container::robotics
