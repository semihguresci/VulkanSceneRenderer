#include "Container/robotics/RoboticsScene.h"

#include "Container/geometry/DotBimLoader.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace container::robotics {
namespace {

namespace bim = container::geometry::dotbim;

std::string normalizedClass(std::string_view value) {
  std::string result;
  for (const unsigned char character : value)
    if (!std::isspace(character)) result.push_back(static_cast<char>(std::toupper(character)));
  return result;
}

void requirePositive(double value, std::string_view name) {
  if (!std::isfinite(value) || value <= 0.0)
    throw std::invalid_argument(std::string(name) + " must be finite and positive");
}

void validateTransform(const glm::dmat4& transform, std::string_view source) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(transform[column][row]))
        throw std::invalid_argument("Nonfinite robotics source transform for " + std::string(source));
  if (transform[0][3] != 0.0 || transform[1][3] != 0.0 || transform[2][3] != 0.0 || transform[3][3] != 1.0)
    throw std::invalid_argument("Nonaffine robotics source transform for " + std::string(source));
  const auto determinant = glm::determinant(glm::dmat3(transform));
  if (!std::isfinite(determinant) || determinant == 0.0)
    throw std::invalid_argument("Singular robotics source transform for " + std::string(source));
}

bool sameTransform(const glm::dmat4& first, const glm::dmat4& second) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (first[column][row] != second[column][row]) return false;
  return true;
}

std::string component(std::string_view value) {
  return std::to_string(value.size()) + ":" + std::string(value);
}

std::string occurrenceKey(std::string_view sourceId, std::string_view guid) {
  if (!sourceId.empty()) return "source/" + component(sourceId);
  if (!guid.empty()) return "guid/" + component(guid);
  throw std::invalid_argument("A robotics semantic product requires source identity or GUID");
}

SemanticGeometryKind geometryKind(bim::GeometryKind kind) {
  switch (kind) {
  case bim::GeometryKind::Mesh: return SemanticGeometryKind::Mesh;
  case bim::GeometryKind::Points: return SemanticGeometryKind::Points;
  case bim::GeometryKind::Curves: return SemanticGeometryKind::Curves;
  }
  throw std::invalid_argument("Unknown BIM representation geometry kind");
}

SemanticRelationshipKind relationshipKind(bim::SemanticRelationshipKind kind) {
  switch (kind) {
  case bim::SemanticRelationshipKind::Contains: return SemanticRelationshipKind::Contains;
  case bim::SemanticRelationshipKind::Aggregates: return SemanticRelationshipKind::Aggregates;
  case bim::SemanticRelationshipKind::Nests: return SemanticRelationshipKind::Nests;
  case bim::SemanticRelationshipKind::Voids: return SemanticRelationshipKind::Voids;
  case bim::SemanticRelationshipKind::Fills: return SemanticRelationshipKind::Fills;
  case bim::SemanticRelationshipKind::Child: return SemanticRelationshipKind::Child;
  case bim::SemanticRelationshipKind::SpaceBoundary: return SemanticRelationshipKind::SpaceBoundary;
  }
  throw std::invalid_argument("Unknown BIM semantic relationship kind");
}

bool spatialRelationship(SemanticRelationshipKind kind) {
  return kind == SemanticRelationshipKind::Contains || kind == SemanticRelationshipKind::Aggregates ||
         kind == SemanticRelationshipKind::Nests || kind == SemanticRelationshipKind::Child;
}

RoboticsFrame frameFor(const bim::Model& model, const RoboticsSceneOptions& options) {
  const auto& units = model.unitMetadata;
  RoboticsFrame frame;
  if (units.hasImportScale) {
    requirePositive(units.importScale, "BIM importScale");
    frame.importScale = units.importScale;
  }
  if (model.semanticUnitContext) {
    const auto& context = *model.semanticUnitContext;
    frame.sourceUnits = context.sourceUnits;
    frame.unitProvenance = context.provenance;
    if (context.sourceMetersPerUnit) {
      requirePositive(*context.sourceMetersPerUnit, "BIM semantic sourceMetersPerUnit");
      frame.sourceMetersPerUnit = context.sourceMetersPerUnit;
    }
    if (context.metersPerImportedUnit) {
      requirePositive(*context.metersPerImportedUnit, "BIM semantic metersPerImportedUnit");
      frame.metersPerRendererUnit = context.metersPerImportedUnit;
    }
  } else {
    if (units.hasSourceUnits) frame.sourceUnits = units.sourceUnits;
    if (units.hasMetersPerUnit) {
      requirePositive(units.metersPerUnit, "BIM metersPerUnit");
      frame.sourceMetersPerUnit = units.metersPerUnit;
    }
    if (units.hasEffectiveImportScale) requirePositive(units.effectiveImportScale, "BIM effectiveImportScale");
    if (frame.sourceMetersPerUnit && frame.importScale && units.hasEffectiveImportScale) {
      const double expected = *frame.sourceMetersPerUnit * *frame.importScale;
      if (std::abs(double(units.effectiveImportScale) - expected) > expected * 1e-5)
        throw std::invalid_argument("BIM unit metadata disagrees with the effective import scale");
    }
    if (frame.sourceMetersPerUnit && frame.importScale) {
      // Legacy importers have already applied their source unit conversion.
      frame.metersPerRendererUnit = 1.0 / *frame.importScale;
      frame.unitProvenance = "Legacy BIM source units and applied import scale";
    }
  }
  if (options.metersPerRendererUnit) {
    requirePositive(*options.metersPerRendererUnit, "Robotics metersPerRendererUnit");
    frame.metersPerRendererUnit = options.metersPerRendererUnit;
    frame.unitProvenance += " Explicit metersPerRendererUnit override.";
  }
  if (frame.metersPerRendererUnit)
    frame.roboticsFromRenderer = rendererToRosTransform(*frame.metersPerRendererUnit);
  return frame;
}

void assignMetricTransform(std::optional<glm::dmat4>& destination,
                           const glm::dmat4& source, const RoboticsFrame& frame) {
  if (!frame.roboticsFromRenderer) return;
  destination = *frame.roboticsFromRenderer * source;
  validateTransform(*destination, "converted metric placement");
}

std::optional<SemanticBounds> metricBounds(const bim::SemanticBounds& bounds, const RoboticsFrame& frame) {
  if (!frame.roboticsFromRenderer) return std::nullopt;
  // The renderer-to-robotics basis is a signed axis permutation and uniform scale.
  const glm::dvec3 first(*frame.roboticsFromRenderer * glm::dvec4(bounds.minimum, 1.0));
  const glm::dvec3 second(*frame.roboticsFromRenderer * glm::dvec4(bounds.maximum, 1.0));
  for (int axis = 0; axis < 3; ++axis)
    if (bounds.minimum[axis] > bounds.maximum[axis] ||
        !std::isfinite(first[axis]) || !std::isfinite(second[axis]))
      return std::nullopt;
  return SemanticBounds{glm::min(first, second), glm::max(first, second)};
}

} // namespace

glm::dmat4 rendererToRosTransform(double metersPerRendererUnit) {
  requirePositive(metersPerRendererUnit, "Robotics metersPerRendererUnit");
  glm::dmat4 transform(0.0);
  transform[0][1] = -metersPerRendererUnit;
  transform[1][2] = metersPerRendererUnit;
  transform[2][0] = -metersPerRendererUnit;
  transform[3][3] = 1.0;
  return transform;
}

SemanticKind classifySemanticKind(std::string_view ifcClass) {
  const auto value = normalizedClass(ifcClass);
  if (value.empty()) return SemanticKind::Unknown;
  if (value == "IFCSITE") return SemanticKind::Site;
  if (value == "IFCBUILDING") return SemanticKind::Building;
  if (value == "IFCBUILDINGSTOREY") return SemanticKind::Storey;
  if (value == "IFCSPACE") return SemanticKind::Space;
  if (value == "IFCWALL" || value == "IFCWALLSTANDARDCASE" || value == "IFCWALLELEMENTEDCASE") return SemanticKind::Wall;
  if (value == "IFCSLAB" || value == "IFCSLABSTANDARDCASE" || value == "IFCSLABELEMENTEDCASE") return SemanticKind::Slab;
  if (value == "IFCDOOR" || value == "IFCDOORSTANDARDCASE") return SemanticKind::Door;
  if (value == "IFCOPENINGELEMENT" || value == "IFCOPENINGSTANDARDCASE") return SemanticKind::Opening;
  return SemanticKind::Other;
}

std::string_view semanticKindName(SemanticKind kind) {
  switch (kind) {
  case SemanticKind::Unknown: return "unknown";
  case SemanticKind::Site: return "site";
  case SemanticKind::Building: return "building";
  case SemanticKind::Storey: return "storey";
  case SemanticKind::Space: return "space";
  case SemanticKind::Wall: return "wall";
  case SemanticKind::Slab: return "slab";
  case SemanticKind::Door: return "door";
  case SemanticKind::Opening: return "opening";
  case SemanticKind::Other: return "other";
  }
  throw std::invalid_argument("Unknown robotics semantic kind");
}

std::string_view semanticRelationshipKindName(SemanticRelationshipKind kind) {
  switch (kind) {
  case SemanticRelationshipKind::Contains: return "contains";
  case SemanticRelationshipKind::Aggregates: return "aggregates";
  case SemanticRelationshipKind::Nests: return "nests";
  case SemanticRelationshipKind::Voids: return "voids";
  case SemanticRelationshipKind::Fills: return "fills";
  case SemanticRelationshipKind::Child: return "child";
  case SemanticRelationshipKind::SpaceBoundary: return "space-boundary";
  }
  throw std::invalid_argument("Unknown robotics semantic relationship kind");
}

std::string_view semanticGeometryKindName(SemanticGeometryKind kind) {
  switch (kind) {
  case SemanticGeometryKind::Mesh: return "mesh";
  case SemanticGeometryKind::Points: return "points";
  case SemanticGeometryKind::Curves: return "curves";
  }
  throw std::invalid_argument("Unknown robotics geometry kind");
}

RoboticsScene buildRoboticsScene(const bim::Model& model, const RoboticsSceneOptions& options) {
  if (options.sourceNamespace.empty())
    throw std::invalid_argument("Robotics sourceNamespace must identify the source asset");
  RoboticsScene scene;
  scene.sourceNamespace = options.sourceNamespace;
  scene.frame = frameFor(model, options);
  const auto diagnostic = [&](std::string code, std::string sourceId, std::string message) {
    scene.diagnostics.push_back({std::move(code), std::move(sourceId), std::move(message)});
  };
  if (!scene.frame.roboticsFromRenderer)
    diagnostic("missing-metric-units", {}, "Source length units are unknown; metric placements require an explicit metersPerRendererUnit value.");
  const auto& georeference = model.georeferenceMetadata;
  if (georeference.hasSourceUpAxis) scene.georeference.sourceUpAxis = georeference.sourceUpAxis;
  if (georeference.hasCoordinateOffset) {
    for (int axis = 0; axis < 3; ++axis)
      if (!std::isfinite(georeference.coordinateOffset[axis]))
        throw std::invalid_argument("Nonfinite BIM georeference coordinate offset");
    scene.georeference.coordinateOffset = georeference.coordinateOffset;
  }
  scene.georeference.coordinateOffsetSource = georeference.coordinateOffsetSource;
  scene.georeference.crsName = georeference.crsName;
  scene.georeference.crsAuthority = georeference.crsAuthority;
  scene.georeference.crsCode = georeference.crsCode;
  scene.georeference.mapConversionName = georeference.mapConversionName;
  if (scene.georeference.coordinateOffset || !scene.georeference.crsName.empty())
    diagnostic("georeference-not-applied", {}, "CRS and map-conversion metadata are retained as source provenance; no geographic or map-projection transform is applied.");

  std::map<std::string, SemanticEntity> products;
  std::set<std::string> authoritativeProducts;
  std::set<std::string> composedOccurrences;
  for (const auto& product : model.semanticProducts) {
    const auto key = occurrenceKey(product.sourceId, product.guid);
    SemanticEntity entity;
    entity.sourceId = product.sourceId;
    entity.guid = product.guid;
    entity.ifcClass = product.type;
    entity.name = product.name;
    entity.kind = classifySemanticKind(product.type);
    if (product.hasPlacement) {
      validateTransform(product.transform, product.sourceId);
      entity.rendererWorldFromLocal = product.transform;
      assignMetricTransform(entity.roboticsWorldFromLocal, product.transform, scene.frame);
    }
    if (product.rendererBounds) {
      entity.roboticsBounds = metricBounds(*product.rendererBounds, scene.frame);
      if (scene.frame.roboticsFromRenderer && !entity.roboticsBounds)
        diagnostic("invalid-product-bounds", entity.sourceId, "The imported geometry bounds cannot be converted to the metric robotics frame.");
    }
    if (const auto found = products.find(key); found != products.end()) {
      const auto& original = found->second;
      if (product.sourceId.empty() || original.guid != entity.guid ||
          normalizedClass(original.ifcClass) != normalizedClass(entity.ifcClass) || original.name != entity.name ||
          composedOccurrences.contains(key) != product.isComposedOccurrence ||
          original.roboticsBounds != entity.roboticsBounds ||
          bool(original.rendererWorldFromLocal) != bool(entity.rendererWorldFromLocal) ||
          (original.rendererWorldFromLocal && !sameTransform(*original.rendererWorldFromLocal, *entity.rendererWorldFromLocal)))
        throw std::invalid_argument("Conflicting robotics semantic source occurrence " + product.sourceId);
    } else {
      products.emplace(key, std::move(entity));
    }
    authoritativeProducts.insert(key);
    if (product.isComposedOccurrence) composedOccurrences.insert(key);
  }
  for (const auto& space : model.spacesWithoutDrawableGeometry) {
    const auto key = occurrenceKey(space.sourceId, space.guid);
    if (!products.contains(key)) {
      SemanticEntity entity;
      entity.sourceId = space.sourceId;
      entity.guid = space.guid;
      entity.ifcClass = "IfcSpace";
      entity.name = space.name;
      entity.kind = SemanticKind::Space;
      products.emplace(key, std::move(entity));
    }
    diagnostic("space-without-geometry", space.sourceId, space.reason);
  }
  std::set<std::string> ambiguousPlacement;
  for (const auto& element : model.elements) {
    if (element.isRepresentationOnly) {
      diagnostic("representation-without-product", element.sourceId,
                 "Imported geometry has no authored semantic product; no navigation entity is created.");
      continue;
    }
    const auto& sourceId = element.semanticSourceId.empty() ? element.sourceId : element.semanticSourceId;
    const auto key = occurrenceKey(sourceId, element.guid);
    auto [position, added] = products.try_emplace(key);
    auto& entity = position->second;
    if (added) {
      entity.sourceId = sourceId;
      entity.guid = element.guid;
      entity.ifcClass = element.type;
      entity.name = element.displayName;
      entity.kind = classifySemanticKind(element.type);
    } else if (!authoritativeProducts.contains(key) &&
               ((!element.guid.empty() && !entity.guid.empty() && element.guid != entity.guid) ||
                (!element.type.empty() && !entity.ifcClass.empty() && normalizedClass(element.type) != normalizedClass(entity.ifcClass)) ||
                (!element.displayName.empty() && !entity.name.empty() && element.displayName != entity.name))) {
      throw std::invalid_argument("Conflicting robotics semantic facts for " + sourceId);
    }
    if (!authoritativeProducts.contains(key)) {
      if (entity.guid.empty()) entity.guid = element.guid;
      if (entity.ifcClass.empty() || (!element.type.empty() && element.type < entity.ifcClass)) {
        entity.ifcClass = element.type;
        entity.kind = classifySemanticKind(entity.ifcClass);
      }
      if (entity.name.empty()) entity.name = element.displayName;
    }
    SemanticRepresentation representation;
    representation.sourceId = element.sourceId;
    representation.meshId = element.meshId;
    representation.geometryKind = geometryKind(element.geometryKind);
    representation.rendererWorldFromLocal = glm::dmat4(element.transform);
    validateTransform(representation.rendererWorldFromLocal, element.sourceId);
    if (!authoritativeProducts.contains(key) || entity.rendererWorldFromLocal)
      assignMetricTransform(representation.roboticsWorldFromLocal, representation.rendererWorldFromLocal, scene.frame);
    if (!authoritativeProducts.contains(key) && !ambiguousPlacement.contains(key)) {
      if (!entity.rendererWorldFromLocal || sameTransform(*entity.rendererWorldFromLocal, representation.rendererWorldFromLocal)) {
        entity.rendererWorldFromLocal = representation.rendererWorldFromLocal;
        entity.roboticsWorldFromLocal = representation.roboticsWorldFromLocal;
      } else {
        entity.rendererWorldFromLocal.reset();
        entity.roboticsWorldFromLocal.reset();
        ambiguousPlacement.insert(key);
      }
    }
    entity.representations.push_back(std::move(representation));
  }

  std::map<std::string, size_t> guidOccurrences;
  for (const auto& [key, entity] : products)
    if (!entity.guid.empty()) ++guidOccurrences[entity.guid];
  std::map<std::string, std::string> sourceToId;
  std::set<std::string> unsupportedClasses;
  for (auto& [key, entity] : products) {
    entity.id = "bim/" + component(options.sourceNamespace);
    if (!entity.guid.empty()) {
      entity.id += "/guid/" + component(entity.guid);
      if (composedOccurrences.contains(key) || guidOccurrences[entity.guid] > 1) {
        if (entity.sourceId.empty())
          throw std::invalid_argument("A composed or repeated GUID requires an explicit source occurrence " + entity.guid);
        entity.id += "/occurrence/" + component(entity.sourceId);
        if (guidOccurrences[entity.guid] > 1)
          diagnostic("shared-guid-occurrences", entity.sourceId, "The authored GUID occurs in multiple source instances; the stable ID includes its source occurrence.");
      }
    } else {
      entity.id += "/source/" + component(entity.sourceId);
      diagnostic("missing-guid", entity.sourceId, "Stable identity uses the explicit source occurrence because no authored GUID is available.");
    }
    if (!entity.sourceId.empty()) sourceToId.emplace(entity.sourceId, entity.id);
    if (entity.kind == SemanticKind::Unknown)
      diagnostic("missing-classification", entity.sourceId, "No authored IFC class is available.");
    if (entity.kind == SemanticKind::Other && unsupportedClasses.insert(entity.ifcClass).second)
      diagnostic("unsupported-semantic-class", entity.sourceId, "The source class " + entity.ifcClass + " is retained without navigation semantics.");
    if (!entity.rendererWorldFromLocal)
      diagnostic("missing-product-placement", entity.sourceId, "An exact product placement is unavailable; representation placements remain independent.");
    if (entity.representations.empty())
      diagnostic("product-without-geometry", entity.sourceId, "The semantic product has no imported drawable representation.");
    std::sort(entity.representations.begin(), entity.representations.end(), [](const auto& first, const auto& second) {
      const auto firstKey = std::tie(first.sourceId, first.meshId, first.geometryKind);
      const auto secondKey = std::tie(second.sourceId, second.meshId, second.geometryKind);
      if (firstKey != secondKey) return firstKey < secondKey;
      for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
          if (first.rendererWorldFromLocal[column][row] != second.rendererWorldFromLocal[column][row])
            return first.rendererWorldFromLocal[column][row] < second.rendererWorldFromLocal[column][row];
      return false;
    });
    scene.entities.push_back(std::move(entity));
  }
  std::sort(scene.entities.begin(), scene.entities.end(), [](const auto& first, const auto& second) { return first.id < second.id; });
  std::set<std::tuple<std::string, std::string, SemanticRelationshipKind>> relationships;
  for (const auto& relationship : model.semanticRelationships) {
    const auto from = sourceToId.find(relationship.fromSourceId);
    const auto to = sourceToId.find(relationship.toSourceId);
    if (from == sourceToId.end() || to == sourceToId.end()) {
      const auto& missing = from == sourceToId.end() ? relationship.fromSourceId : relationship.toSourceId;
      diagnostic("unresolved-relationship", missing, "A semantic relationship endpoint is absent from the imported product metadata.");
      continue;
    }
    const auto kind = relationshipKind(relationship.kind);
    if (from->second == to->second) {
      diagnostic("self-relationship", relationship.fromSourceId, "A semantic relationship references the same product at both endpoints.");
      continue;
    }
    relationships.emplace(from->second, to->second, kind);
  }
  std::map<std::string, std::vector<std::string>> parents;
  for (const auto& [from, to, kind] : relationships) {
    scene.relationships.push_back({from, to, kind});
    if (spatialRelationship(kind)) parents[to].push_back(from);
  }
  std::map<std::string, SemanticKind> kinds;
  for (const auto& entity : scene.entities) kinds.emplace(entity.id, entity.kind);
  for (auto& entity : scene.entities) {
    std::vector<std::string> pending{entity.id};
    std::set<std::string> visited;
    bool cyclic = false;
    for (size_t index = 0; index < pending.size(); ++index) {
      const auto& id = pending[index];
      if (index != 0 && id == entity.id) cyclic = true;
      if (!visited.insert(id).second) continue;
      if (kinds.at(id) == SemanticKind::Storey) entity.storeyIds.push_back(id);
      if (const auto found = parents.find(id); found != parents.end())
        pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    std::sort(entity.storeyIds.begin(), entity.storeyIds.end());
    if (cyclic)
      diagnostic("cyclic-spatial-relationship", entity.sourceId, "Authored spatial relationships contain a cycle; no navigation hierarchy is inferred.");
    if (entity.storeyIds.size() > 1)
      diagnostic("ambiguous-storey", entity.sourceId, "Multiple authored storey ancestors are retained.");
    if (entity.storeyIds.empty() && entity.kind != SemanticKind::Site && entity.kind != SemanticKind::Building)
      diagnostic("missing-storey", entity.sourceId, "No authored storey ancestor is available.");
  }
  std::sort(scene.diagnostics.begin(), scene.diagnostics.end(), [](const auto& first, const auto& second) {
    return std::tie(first.code, first.sourceId, first.message) < std::tie(second.code, second.sourceId, second.message);
  });
  scene.diagnostics.erase(std::unique(scene.diagnostics.begin(), scene.diagnostics.end(), [](const auto& first, const auto& second) {
    return std::tie(first.code, first.sourceId, first.message) == std::tie(second.code, second.sourceId, second.message);
  }), scene.diagnostics.end());
  return scene;
}

} // namespace container::robotics
