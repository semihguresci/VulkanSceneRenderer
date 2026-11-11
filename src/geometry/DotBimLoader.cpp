#include "Container/geometry/DotBimLoader.h"
#include "BimImportInput.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace container::geometry::dotbim {
namespace {

using Json = nlohmann::json;

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

const Json &requiredMember(const Json &object, const char *key,
                           std::string_view context) {
  if (!object.is_object() || !object.contains(key)) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " is missing '" + key + "'");
  }
  return object.at(key);
}

const Json &requiredArray(const Json &object, const char *key,
                          std::string_view context) {
  const Json &value = requiredMember(object, key, context);
  if (!value.is_array()) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " '" + key + "' must be an array");
  }
  return value;
}

float numberOr(const Json &object, const char *key, float fallback) {
  if (!object.is_object() || !object.contains(key)) {
    return fallback;
  }
  const Json &value = object.at(key);
  if (!value.is_number()) {
    return fallback;
  }
  const double result = value.get<double>();
  return std::isfinite(result) ? static_cast<float>(result) : fallback;
}

uint32_t requiredMeshId(const Json &object, std::string_view context) {
  const Json &value = requiredMember(object, "mesh_id", context);
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " mesh_id must be an integer");
  }
  const int64_t id = value.get<int64_t>();
  if (id < 0 || id > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " mesh_id is outside uint32 range");
  }
  return static_cast<uint32_t>(id);
}

glm::vec3 readVector(const Json &object, const char *key, glm::vec3 fallback) {
  if (!object.is_object() || !object.contains(key)) {
    return fallback;
  }
  const Json &value = object.at(key);
  if (!value.is_object()) {
    return fallback;
  }
  return {numberOr(value, "x", fallback.x), numberOr(value, "y", fallback.y),
          numberOr(value, "z", fallback.z)};
}

glm::quat readRotation(const Json &object) {
  if (!object.is_object() || !object.contains("rotation")) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const Json &value = object.at("rotation");
  if (!value.is_object()) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  glm::quat rotation(numberOr(value, "qw", 1.0f), numberOr(value, "qx", 0.0f),
                     numberOr(value, "qy", 0.0f), numberOr(value, "qz", 0.0f));
  const float len2 = glm::dot(rotation, rotation);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  return glm::normalize(rotation);
}

float colorChannelOr(const Json &color, const char *key, float fallback) {
  if (!color.is_object() || !color.contains(key) ||
      !color.at(key).is_number()) {
    return fallback;
  }
  const double value = color.at(key).get<double>();
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(static_cast<float>(value), 0.0f, 255.0f) / 255.0f;
}

glm::vec4 readColor(const Json &element) {
  if (!element.is_object() || !element.contains("color") ||
      !element.at("color").is_object()) {
    return {0.8f, 0.82f, 0.86f, 1.0f};
  }
  const Json &color = element.at("color");
  return {
      colorChannelOr(color, "r", 204.0f), colorChannelOr(color, "g", 209.0f),
      colorChannelOr(color, "b", 219.0f), colorChannelOr(color, "a", 255.0f)};
}

std::string stringOrEmpty(const Json &object, const char *key) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_string()) {
    return {};
  }
  return object.at(key).get<std::string>();
}

std::string scalarStringOrEmpty(const Json &value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<int64_t>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<uint64_t>());
  }
  if (value.is_number_float()) {
    const double number = value.get<double>();
    if (std::isfinite(number)) {
      return std::to_string(number);
    }
  }
  if (value.is_object()) {
    for (const char *key : {"code", "value", "name", "id", "label"}) {
      if (value.contains(key)) {
        std::string nested = scalarStringOrEmpty(value.at(key));
        if (!nested.empty()) {
          return nested;
        }
      }
    }
  }
  return {};
}

std::string scalarStringOrEmpty(const Json &object, const char *key) {
  if (!object.is_object() || !object.contains(key)) {
    return {};
  }
  return scalarStringOrEmpty(object.at(key));
}

std::string firstStringOrEmpty(const Json &object,
                               std::initializer_list<const char *> keys) {
  for (const char *key : keys) {
    std::string value = stringOrEmpty(object, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

std::string firstScalarStringOrEmpty(
    const Json &object, std::initializer_list<const char *> keys) {
  for (const char *key : keys) {
    std::string value = scalarStringOrEmpty(object, key);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

void appendProperty(std::vector<ElementProperty> &properties,
                    ElementProperty property) {
  if (property.name.empty() || property.value.empty()) {
    return;
  }
  const auto duplicate = std::ranges::find_if(
      properties, [&](const ElementProperty &existing) {
        return existing.set == property.set && existing.name == property.name &&
               existing.category == property.category;
      });
  if (duplicate == properties.end()) {
    properties.push_back(std::move(property));
  }
}

ElementProperty propertyFromJsonObject(const Json &value,
                                       std::string fallbackName = {}) {
  ElementProperty property{};
  if (!value.is_object()) {
    property.name = std::move(fallbackName);
    property.value = scalarStringOrEmpty(value);
    return property;
  }

  property.set = firstScalarStringOrEmpty(
      value, {"set", "propertySet", "property_set", "setName", "set_name"});
  property.name =
      firstScalarStringOrEmpty(value, {"name", "property", "propertyName",
                                       "property_name"});
  if (property.name.empty()) {
    property.name = std::move(fallbackName);
  }
  property.value =
      firstScalarStringOrEmpty(value, {"value", "displayValue",
                                       "display_value", "nominalValue",
                                       "nominal_value"});
  property.category =
      firstScalarStringOrEmpty(value, {"category", "type", "propertyType",
                                       "property_type"});
  return property;
}

std::vector<ElementProperty> readElementProperties(const Json &source,
                                                   std::stop_token stop) {
  std::vector<ElementProperty> properties;
  if (!source.is_object() || !source.contains("properties")) {
    return properties;
  }

  const Json &value = source.at("properties");
  if (value.is_array()) {
    properties.reserve(value.size());
    for (const Json &item : value) {
      container::util::checkModelImportCancellation(stop);
      appendProperty(properties, propertyFromJsonObject(item));
    }
  } else if (value.is_object()) {
    properties.reserve(value.size());
    for (auto it = value.begin(); it != value.end(); ++it) {
      container::util::checkModelImportCancellation(stop);
      appendProperty(properties, propertyFromJsonObject(it.value(), it.key()));
    }
  }
  return properties;
}

ElementRelationship relationshipFromJsonObject(const Json &value) {
  ElementRelationship relationship{};
  if (!value.is_object()) {
    return relationship;
  }
  relationship.fromGuid = firstScalarStringOrEmpty(
      value, {"fromGuid", "from_guid", "sourceGuid", "source_guid",
              "relatingGuid", "relating_guid"});
  relationship.fromSourceId = firstScalarStringOrEmpty(
      value, {"fromSourceId", "from_source_id", "sourceId", "source_id",
              "relatingSourceId", "relating_source_id"});
  relationship.toGuid = firstScalarStringOrEmpty(
      value, {"toGuid", "to_guid", "targetGuid", "target_guid",
              "relatedGuid", "related_guid"});
  relationship.toSourceId = firstScalarStringOrEmpty(
      value, {"toSourceId", "to_source_id", "targetSourceId",
              "target_source_id", "relatedSourceId", "related_source_id"});
  relationship.kind =
      firstScalarStringOrEmpty(value, {"kind", "relationshipKind",
                                       "relationship_kind", "relationship",
                                       "type"});
  relationship.label =
      firstScalarStringOrEmpty(value, {"label", "name", "description"});
  return relationship;
}

std::vector<ElementRelationship> readElementRelationships(const Json &root,
                                                          std::stop_token stop) {
  std::vector<ElementRelationship> relationships;
  if (!root.is_object()) {
    return relationships;
  }

  const Json *relationshipArray = nullptr;
  for (const char *key : {"relationships", "elementRelationships",
                          "element_relationships"}) {
    if (root.contains(key) && root.at(key).is_array()) {
      relationshipArray = &root.at(key);
      break;
    }
  }
  if (relationshipArray == nullptr) {
    return relationships;
  }

  relationships.reserve(relationshipArray->size());
  for (const Json &item : *relationshipArray) {
    container::util::checkModelImportCancellation(stop);
    ElementRelationship relationship = relationshipFromJsonObject(item);
    if (relationship.fromGuid.empty() && relationship.fromSourceId.empty()) {
      continue;
    }
    if (relationship.toGuid.empty() && relationship.toSourceId.empty()) {
      continue;
    }
    relationships.push_back(std::move(relationship));
  }
  return relationships;
}

std::optional<double> numberAsDouble(const Json &value) {
  if (!value.is_number()) {
    return std::nullopt;
  }
  const double parsed = value.get<double>();
  if (!std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<glm::dvec3> dvec3FromJson(const Json &value) {
  if (value.is_array() && value.size() >= 3u) {
    const auto x = numberAsDouble(value[0]);
    const auto y = numberAsDouble(value[1]);
    const auto z = numberAsDouble(value[2]);
    if (x && y && z) {
      return glm::dvec3(*x, *y, *z);
    }
  }
  if (value.is_object()) {
    const auto x = value.contains("x") ? numberAsDouble(value.at("x"))
                                       : std::optional<double>{};
    const auto y = value.contains("y") ? numberAsDouble(value.at("y"))
                                       : std::optional<double>{};
    const auto z = value.contains("z") ? numberAsDouble(value.at("z"))
                                       : std::optional<double>{};
    if (x && y && z) {
      return glm::dvec3(*x, *y, *z);
    }
  }
  return std::nullopt;
}

void applyGeoreferenceObject(const Json &value,
                             ModelGeoreferenceMetadata &metadata) {
  if (!value.is_object()) {
    return;
  }
  metadata.sourceUpAxis = firstScalarStringOrEmpty(
      value, {"sourceUpAxis", "source_up_axis", "upAxis", "up_axis"});
  metadata.hasSourceUpAxis = !metadata.sourceUpAxis.empty();
  metadata.crsName =
      firstScalarStringOrEmpty(value, {"crsName", "crs_name", "crs"});
  metadata.crsAuthority = firstScalarStringOrEmpty(
      value, {"crsAuthority", "crs_authority", "authority"});
  metadata.crsCode =
      firstScalarStringOrEmpty(value, {"crsCode", "crs_code", "epsg"});
  metadata.mapConversionName =
      firstScalarStringOrEmpty(value, {"mapConversionName",
                                       "map_conversion_name",
                                       "mapConversion"});
  for (const char *key : {"coordinateOffset", "coordinate_offset",
                          "siteCoordinateOffset", "projectCoordinateOffset",
                          "origin", "falseOrigin"}) {
    if (!value.contains(key)) {
      continue;
    }
    if (auto offset = dvec3FromJson(value.at(key))) {
      metadata.coordinateOffset = *offset;
      metadata.hasCoordinateOffset = true;
      metadata.coordinateOffsetSource = key;
      break;
    }
  }
}

ModelGeoreferenceMetadata readGeoreferenceMetadata(const Json &root) {
  ModelGeoreferenceMetadata metadata{};
  if (!root.is_object()) {
    return metadata;
  }
  for (const char *key : {"georeference", "georeferencing",
                          "geoReference"}) {
    if (root.contains(key)) {
      applyGeoreferenceObject(root.at(key), metadata);
    }
  }
  if (root.contains("info") && root.at("info").is_object()) {
    const Json &info = root.at("info");
    for (const char *key : {"georeference", "georeferencing",
                            "geoReference"}) {
      if (info.contains(key)) {
        applyGeoreferenceObject(info.at(key), metadata);
      }
    }
  }
  return metadata;
}

glm::vec3 safeNormal(const glm::vec3 &a, const glm::vec3 &b,
                     const glm::vec3 &c) {
  const glm::vec3 normal = glm::cross(b - a, c - a);
  const float len2 = glm::dot(normal, normal);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return {0.0f, 1.0f, 0.0f};
  }
  return normal * (1.0f / std::sqrt(len2));
}

glm::vec3 safeTangent(const glm::vec3 &a, const glm::vec3 &b,
                      const glm::vec3 &normal) {
  glm::vec3 tangent = b - a;
  tangent -= normal * glm::dot(normal, tangent);
  float len2 = glm::dot(tangent, tangent);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    const glm::vec3 axis = std::abs(normal.y) < 0.999f
                               ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
    tangent = glm::cross(axis, normal);
    len2 = glm::dot(tangent, tangent);
  }
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return {1.0f, 0.0f, 0.0f};
  }
  return tangent * (1.0f / std::sqrt(len2));
}

Vertex makeVertex(const glm::vec3 &position, const glm::vec3 &normal,
                  const glm::vec3 &tangent) {
  Vertex vertex{};
  vertex.position = position;
  vertex.normal = normal;
  vertex.tangent = glm::vec4(tangent, 1.0f);
  return vertex;
}

std::vector<glm::vec3> readCoordinates(const Json &mesh,
                                       std::string_view context,
                                       std::stop_token stop) {
  const Json *coordinates = nullptr;
  if (mesh.contains("coordinates")) {
    coordinates = &requiredArray(mesh, "coordinates", context);
  } else {
    coordinates = &requiredArray(mesh, "vertices_coordinates", context);
  }
  if ((coordinates->size() % 3u) != 0u) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " coordinates count must be divisible by 3");
  }

  std::vector<glm::vec3> result;
  result.reserve(coordinates->size() / 3u);
  for (size_t i = 0; i < coordinates->size(); i += 3u) {
    if ((i % 3072u) == 0u)
      container::util::checkModelImportCancellation(stop);
    if (!(*coordinates)[i].is_number() || !(*coordinates)[i + 1].is_number() ||
        !(*coordinates)[i + 2].is_number()) {
      throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                               " coordinates must contain numbers");
    }
    result.emplace_back((*coordinates)[i].get<float>(),
                        (*coordinates)[i + 1].get<float>(),
                        (*coordinates)[i + 2].get<float>());
  }
  return result;
}

std::vector<uint32_t> readIndices(const Json &mesh, std::string_view context,
                                  size_t vertexCount, std::stop_token stop) {
  const Json &indices = requiredArray(mesh, "indices", context);
  if ((indices.size() % 3u) != 0u) {
    throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                             " indices count must be divisible by 3");
  }

  std::vector<uint32_t> result;
  result.reserve(indices.size());
  for (const Json &value : indices) {
    if ((result.size() % 4096u) == 0u)
      container::util::checkModelImportCancellation(stop);
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
      throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                               " indices must contain integers");
    }
    const int64_t index = value.get<int64_t>();
    if (index < 0 || static_cast<uint64_t>(index) >= vertexCount) {
      throw std::runtime_error(std::string("dotbim ") + std::string(context) +
                               " contains an out-of-range index");
    }
    result.push_back(static_cast<uint32_t>(index));
  }
  return result;
}

std::pair<glm::vec3, float>
computeBounds(const std::vector<glm::vec3> &coordinates, std::stop_token stop) {
  if (coordinates.empty()) {
    return {{0.0f, 0.0f, 0.0f}, 0.0f};
  }

  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
  for (size_t i = 0; i < coordinates.size(); ++i) {
    if ((i % 4096u) == 0u)
      container::util::checkModelImportCancellation(stop);
    const auto &point = coordinates[i];
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  }

  const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
  float radius = 0.0f;
  for (size_t i = 0; i < coordinates.size(); ++i) {
    if ((i % 4096u) == 0u)
      container::util::checkModelImportCancellation(stop);
    const auto &point = coordinates[i];
    radius = std::max(radius, glm::length(point - center));
  }
  return {center, radius};
}

glm::mat4 makeTransform(const Json &element, float importScale) {
  const glm::vec3 translation = readVector(element, "vector", glm::vec3(0.0f));
  const glm::quat rotation = readRotation(element);
  return container::geometry::zUpForwardYToRendererAxes() *
         glm::scale(glm::mat4(1.0f),
                    glm::vec3(sanitizeImportScale(importScale))) *
         glm::translate(glm::mat4(1.0f), translation) *
         glm::mat4_cast(rotation);
}

ModelUnitMetadata makeUnitMetadata(float importScale) {
  const float sanitizedImportScale = sanitizeImportScale(importScale);
  ModelUnitMetadata metadata{};
  metadata.hasImportScale = true;
  metadata.importScale = sanitizedImportScale;
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale = sanitizedImportScale;
  return metadata;
}

} // namespace

Model LoadFromJson(std::string_view jsonText, float importScale,
                   std::stop_token stop) {
  const Json root = bim_import::parseJson(jsonText, stop);
  const Json &meshes = requiredArray(root, "meshes", "file");
  const Json &elements = requiredArray(root, "elements", "file");

  Model model{};
  model.unitMetadata = makeUnitMetadata(importScale);
  model.georeferenceMetadata = readGeoreferenceMetadata(root);
  model.relationships = readElementRelationships(root, stop);
  model.meshRanges.reserve(meshes.size());
  std::unordered_set<uint32_t> seenMeshIds;

  for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
    container::util::checkModelImportCancellation(stop);
    const Json &mesh = meshes[meshIndex];
    const std::string context = "mesh[" + std::to_string(meshIndex) + "]";
    const uint32_t meshId = requiredMeshId(mesh, context);
    if (!seenMeshIds.insert(meshId).second) {
      throw std::runtime_error("dotbim duplicate mesh_id " +
                               std::to_string(meshId));
    }

    const auto coordinates = readCoordinates(mesh, context, stop);
    const auto sourceIndices = readIndices(mesh, context, coordinates.size(), stop);

    MeshRange range{};
    range.meshId = meshId;
    range.firstIndex = static_cast<uint32_t>(model.indices.size());
    const auto [center, radius] = computeBounds(coordinates, stop);
    range.boundsCenter = center;
    range.boundsRadius = radius;

    for (size_t i = 0; i < sourceIndices.size(); i += 3u) {
      if ((i % 3072u) == 0u)
        container::util::checkModelImportCancellation(stop);
      const glm::vec3 &a = coordinates[sourceIndices[i]];
      const glm::vec3 &b = coordinates[sourceIndices[i + 1u]];
      const glm::vec3 &c = coordinates[sourceIndices[i + 2u]];
      const glm::vec3 normal = safeNormal(a, b, c);
      const glm::vec3 tangent = safeTangent(a, b, normal);
      const uint32_t base = static_cast<uint32_t>(model.vertices.size());
      model.vertices.push_back(makeVertex(a, normal, tangent));
      model.vertices.push_back(makeVertex(b, normal, tangent));
      model.vertices.push_back(makeVertex(c, normal, tangent));
      model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
    }

    range.indexCount =
        static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
    model.meshRanges.push_back(range);
  }

  model.elements.reserve(elements.size());
  for (size_t elementIndex = 0; elementIndex < elements.size();
       ++elementIndex) {
    container::util::checkModelImportCancellation(stop);
    const Json &source = elements[elementIndex];
    const std::string context = "element[" + std::to_string(elementIndex) + "]";
    Element element{};
    element.meshId = requiredMeshId(source, context);
    element.transform = makeTransform(source, importScale);
    element.color = readColor(source);
    element.guid = stringOrEmpty(source, "guid");
    element.type = stringOrEmpty(source, "type");
    element.displayName =
        firstStringOrEmpty(source, {"displayName", "display_name", "name"});
    element.objectType =
        firstStringOrEmpty(source, {"objectType", "object_type"});
    element.storeyName =
        firstStringOrEmpty(source, {"storeyName", "storey_name"});
    element.storeyId =
        firstStringOrEmpty(source, {"storeyId", "storey_id", "storeyGuid",
                                    "storey_guid"});
    element.materialName =
        firstStringOrEmpty(source, {"materialName", "material_name"});
    element.materialCategory =
        firstStringOrEmpty(source, {"materialCategory", "material_category"});
    element.discipline =
        firstScalarStringOrEmpty(source, {"discipline", "ifcDiscipline",
                                          "ifc_discipline"});
    element.phase = firstScalarStringOrEmpty(
        source, {"phase", "phaseName", "phase_name", "constructionPhase",
                 "construction_phase"});
    element.fireRating =
        firstScalarStringOrEmpty(source, {"fireRating", "fire_rating",
                                          "FireRating"});
    element.loadBearing =
        firstScalarStringOrEmpty(source, {"loadBearing", "load_bearing",
                                          "LoadBearing", "isLoadBearing",
                                          "is_load_bearing"});
    element.status =
        firstScalarStringOrEmpty(source, {"status", "Status", "elementStatus",
                                          "element_status"});
    element.sourceId =
        firstStringOrEmpty(source, {"sourceId", "source_id"});
    element.properties = readElementProperties(source, stop);
    model.elements.push_back(std::move(element));
  }

  container::util::checkModelImportCancellation(stop);
  return model;
}

Model LoadFromFile(const std::filesystem::path &path, float importScale,
                   std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open dotbim file: " +
                             container::util::pathToUtf8(path));
  }
  return LoadFromJson(bim_import::readText(file, stop), importScale, stop);
}

} // namespace container::geometry::dotbim
