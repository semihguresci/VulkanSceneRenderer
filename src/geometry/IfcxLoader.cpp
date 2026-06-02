#include "Container/geometry/IfcxLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace container::geometry::ifcx {
namespace {

using Json = nlohmann::json;

constexpr float kLineMarkingSurfaceFeatureOffset = 0.002f;

struct Node {
  std::string path{};
  Json attributes{Json::object()};
  std::vector<std::string> children{};
  std::vector<std::string> inherits{};
};

struct NodeGraph {
  std::unordered_map<std::string, Node> nodes{};
  std::unordered_set<std::string> directChildren{};
  std::unordered_set<std::string> inheritedReferences{};
};

struct PointCloud {
  std::vector<glm::vec3> positions{};
  std::vector<glm::vec3> colors{};
};

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

dotbim::ModelUnitMetadata makeUnitMetadata(float importScale) {
  const float sanitizedImportScale = sanitizeImportScale(importScale);
  dotbim::ModelUnitMetadata metadata{};
  metadata.hasImportScale = true;
  metadata.importScale = sanitizedImportScale;
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale = sanitizedImportScale;
  return metadata;
}

const Json &requiredArray(const Json &object, const char *key,
                          std::string_view context) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_array()) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " is missing array member '" + key + "'");
  }
  return object.at(key);
}

std::optional<float> numberAsFloat(const Json &value) {
  if (!value.is_number()) {
    return std::nullopt;
  }
  const double parsed = value.get<double>();
  if (!std::isfinite(parsed)) {
    return std::nullopt;
  }
  return static_cast<float>(parsed);
}

float numberOr(const Json &object, const char *key, float fallback) {
  if (!object.is_object() || !object.contains(key)) {
    return fallback;
  }
  return numberAsFloat(object.at(key)).value_or(fallback);
}

std::optional<glm::vec3> readVec3(const Json &value) {
  if (!value.is_array() || value.size() < 3u) {
    return std::nullopt;
  }

  const auto x = numberAsFloat(value[0]);
  const auto y = numberAsFloat(value[1]);
  const auto z = numberAsFloat(value[2]);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

std::optional<glm::vec4> readVec4(const Json &value) {
  if (!value.is_array() || value.size() < 4u) {
    return std::nullopt;
  }

  const auto x = numberAsFloat(value[0]);
  const auto y = numberAsFloat(value[1]);
  const auto z = numberAsFloat(value[2]);
  const auto w = numberAsFloat(value[3]);
  if (!x || !y || !z || !w) {
    return std::nullopt;
  }
  return glm::vec4(*x, *y, *z, *w);
}

std::optional<uint32_t> readIndex(const Json &value, size_t vertexCount) {
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    return std::nullopt;
  }
  const auto parsed = value.get<int64_t>();
  if (parsed < 0 ||
      static_cast<uint64_t>(parsed) >= static_cast<uint64_t>(vertexCount) ||
      static_cast<uint64_t>(parsed) >
          static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(parsed);
}

glm::vec3 normalizeOrFallback(const glm::vec3 &value,
                              const glm::vec3 &fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

glm::vec3 safeNormal(const glm::vec3 &a, const glm::vec3 &b,
                     const glm::vec3 &c) {
  return normalizeOrFallback(glm::cross(b - a, c - a),
                             glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::vec3 safeTangent(const glm::vec3 &a, const glm::vec3 &b,
                      const glm::vec3 &normal) {
  glm::vec3 tangent = b - a;
  tangent -= normal * glm::dot(tangent, normal);
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

container::geometry::Vertex makeVertex(const glm::vec3 &position,
                                       const glm::vec3 &normal,
                                       const glm::vec3 &tangent,
                                       const glm::vec3 &color) {
  container::geometry::Vertex vertex{};
  vertex.position = position;
  vertex.color = color;
  vertex.normal = normal;
  vertex.tangent = glm::vec4(tangent, 1.0f);
  return vertex;
}

std::pair<glm::vec3, float>
computeBounds(const std::vector<glm::vec3> &points) {
  if (points.empty()) {
    return {glm::vec3(0.0f), 0.0f};
  }

  glm::vec3 minBounds = points.front();
  glm::vec3 maxBounds = points.front();
  for (const glm::vec3 &point : points) {
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  }

  const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
  float radius = 0.0f;
  for (const glm::vec3 &point : points) {
    radius = std::max(radius, glm::length(point - center));
  }
  return {center, radius};
}

std::vector<glm::vec3> readPoints(const Json &mesh, std::string_view context) {
  const Json &points = requiredArray(mesh, "points", context);
  std::vector<glm::vec3> result;
  result.reserve(points.size());
  for (const Json &point : points) {
    if (auto parsed = readVec3(point)) {
      result.push_back(*parsed);
    } else {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid point");
    }
  }
  return result;
}

std::vector<uint32_t> readTriangulatedIndices(const Json &mesh,
                                              std::string_view context,
                                              size_t vertexCount) {
  const Json &sourceIndices = requiredArray(mesh, "faceVertexIndices", context);
  const Json *faceVertexCounts = nullptr;
  if (mesh.contains("faceVertexCounts") &&
      mesh.at("faceVertexCounts").is_array()) {
    faceVertexCounts = &mesh.at("faceVertexCounts");
  }

  std::vector<uint32_t> rawIndices;
  rawIndices.reserve(sourceIndices.size());
  for (const Json &value : sourceIndices) {
    if (auto index = readIndex(value, vertexCount)) {
      rawIndices.push_back(*index);
    } else {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid face vertex index");
    }
  }

  if (faceVertexCounts == nullptr) {
    if ((rawIndices.size() % 3u) != 0u) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has non-triangle indices without counts");
    }
    return rawIndices;
  }

  std::vector<uint32_t> triangles;
  size_t cursor = 0;
  for (const Json &countValue : *faceVertexCounts) {
    if (!countValue.is_number_integer() && !countValue.is_number_unsigned()) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid face vertex count");
    }
    const auto count = countValue.get<int64_t>();
    if (count < 3 || cursor + static_cast<size_t>(count) > rawIndices.size()) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has inconsistent face vertex counts");
    }

    const uint32_t first = rawIndices[cursor];
    for (int64_t i = 1; i + 1 < count; ++i) {
      triangles.push_back(first);
      triangles.push_back(rawIndices[cursor + static_cast<size_t>(i)]);
      triangles.push_back(rawIndices[cursor + static_cast<size_t>(i + 1)]);
    }
    cursor += static_cast<size_t>(count);
  }

  if (cursor != rawIndices.size()) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " has unused face vertex indices");
  }
  return triangles;
}

glm::mat4 readUsdTransform(const Json &attributes) {
  if (!attributes.is_object() || !attributes.contains("usd::xformop")) {
    return glm::mat4(1.0f);
  }
  const Json &xform = attributes.at("usd::xformop");
  if (!xform.is_object() || !xform.contains("transform") ||
      !xform.at("transform").is_array() || xform.at("transform").size() < 4u) {
    return glm::mat4(1.0f);
  }

  glm::mat4 result(1.0f);
  const Json &rows = xform.at("transform");
  for (size_t column = 0; column < 4u; ++column) {
    if (!rows[column].is_array() || rows[column].size() < 4u) {
      return glm::mat4(1.0f);
    }
    for (size_t row = 0; row < 4u; ++row) {
      const auto value = numberAsFloat(rows[column][row]);
      if (!value) {
        return glm::mat4(1.0f);
      }
      result[static_cast<glm::length_t>(column)]
            [static_cast<glm::length_t>(row)] = *value;
    }
  }
  return result;
}

void mergeAttributes(Node &node, const Json &attributes) {
  if (!attributes.is_object()) {
    return;
  }
  for (auto it = attributes.begin(); it != attributes.end(); ++it) {
    node.attributes[it.key()] = it.value();
  }
}

void collectReferences(const Json &value,
                       std::vector<std::string> &references) {
  if (value.is_string()) {
    references.push_back(value.get<std::string>());
    return;
  }
  if (value.is_array()) {
    for (const Json &item : value) {
      collectReferences(item, references);
    }
    return;
  }
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      collectReferences(it.value(), references);
    }
  }
}

void appendUniqueReference(std::vector<std::string> &references,
                           const std::string &reference) {
  if (std::ranges::find(references, reference) == references.end()) {
    references.push_back(reference);
  }
}

NodeGraph buildGraph(const Json &data) {
  NodeGraph graph{};
  for (const Json &item : data) {
    if (!item.is_object() || !item.contains("path") ||
        !item.at("path").is_string()) {
      continue;
    }

    const std::string path = item.at("path").get<std::string>();
    Node &node = graph.nodes[path];
    node.path = path;
    if (item.contains("attributes")) {
      mergeAttributes(node, item.at("attributes"));
    }
    if (item.contains("children") && item.at("children").is_object()) {
      for (auto it = item.at("children").begin();
           it != item.at("children").end(); ++it) {
        std::vector<std::string> childPaths;
        collectReferences(it.value(), childPaths);
        for (const std::string &childPath : childPaths) {
          appendUniqueReference(node.children, childPath);
          graph.directChildren.insert(childPath);
        }
      }
    }
    if (item.contains("inherits")) {
      std::vector<std::string> inheritedPaths;
      collectReferences(item.at("inherits"), inheritedPaths);
      for (const std::string &inheritedPath : inheritedPaths) {
        appendUniqueReference(node.inherits, inheritedPath);
        graph.inheritedReferences.insert(inheritedPath);
      }
    }
  }
  return graph;
}

const Node *nodeOrInheritedNodeWithAttribute(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    std::string_view key, std::unordered_set<std::string> &visiting) {
  if (!visiting.insert(path).second) {
    return nullptr;
  }

  const Node *result = nullptr;
  if (const auto nodeIt = nodes.find(path); nodeIt != nodes.end()) {
    const Node &node = nodeIt->second;
    if (node.attributes.contains(key)) {
      result = &node;
    } else {
      for (const std::string &inheritedPath : node.inherits) {
        if ((result = nodeOrInheritedNodeWithAttribute(
                 inheritedPath, nodes, key, visiting)) != nullptr) {
          break;
        }
      }
    }
  }

  visiting.erase(path);
  return result;
}

const Node *
nearestNodeWithAttribute(const std::vector<std::string> &chain,
                         const std::unordered_map<std::string, Node> &nodes,
                         std::string_view key) {
  std::unordered_set<std::string> checked;
  for (const std::string &path : chain) {
    if (!checked.insert(path).second) {
      continue;
    }
    std::unordered_set<std::string> inheritedVisited;
    if (const Node *node = nodeOrInheritedNodeWithAttribute(path, nodes, key,
                                                            inheritedVisited)) {
      return node;
    }
  }
  return nullptr;
}

bool isInvisible(const std::vector<std::string> &chain,
                 const std::unordered_map<std::string, Node> &nodes) {
  const Node *node =
      nearestNodeWithAttribute(chain, nodes, "usd::usdgeom::visibility");
  if (node == nullptr) {
    return false;
  }
  const Json &value = node->attributes.at("usd::usdgeom::visibility");
  if (value.is_object() && value.contains("visibility") &&
      value.at("visibility").is_string()) {
    return value.at("visibility").get<std::string>() == "invisible";
  }
  if (value.is_string()) {
    return value.get<std::string>() == "invisible";
  }
  return false;
}

glm::vec4 inheritedColor(const std::vector<std::string> &chain,
                         const std::unordered_map<std::string, Node> &nodes) {
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  if (const Node *node = nearestNodeWithAttribute(
          chain, nodes, "bsi::ifc::presentation::diffuseColor")) {
    const Json &diffuse =
        node->attributes.at("bsi::ifc::presentation::diffuseColor");
    if (diffuse.is_array() && diffuse.size() >= 3u) {
      const auto r = numberAsFloat(diffuse[0]);
      const auto g = numberAsFloat(diffuse[1]);
      const auto b = numberAsFloat(diffuse[2]);
      if (r && g && b) {
        color.r = std::clamp(*r, 0.0f, 1.0f);
        color.g = std::clamp(*g, 0.0f, 1.0f);
        color.b = std::clamp(*b, 0.0f, 1.0f);
      }
    }
  }
  if (const Node *node = nearestNodeWithAttribute(
          chain, nodes, "bsi::ifc::presentation::opacity")) {
    const auto opacity =
        numberAsFloat(node->attributes.at("bsi::ifc::presentation::opacity"));
    if (opacity) {
      color.a = std::clamp(*opacity, 0.0f, 1.0f);
    }
  }
  return color;
}

std::string inheritedType(const std::vector<std::string> &chain,
                          const std::unordered_map<std::string, Node> &nodes) {
  const Node *node = nearestNodeWithAttribute(chain, nodes, "bsi::ifc::class");
  if (node == nullptr) {
    return {};
  }

  const Json &value = node->attributes.at("bsi::ifc::class");
  if (value.is_object() && value.contains("code") &&
      value.at("code").is_string()) {
    return value.at("code").get<std::string>();
  }
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return {};
}

std::optional<std::string> stringValue(const Json &value) {
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
    for (const char *key : {"code", "value", "name", "id", "category",
                            "label"}) {
      if (value.contains(key)) {
        if (auto nested = stringValue(value.at(key)); nested && !nested->empty()) {
          return nested;
        }
      }
    }
  }
  return std::nullopt;
}

std::string metadataLookupKey(std::string_view name) {
  if (const size_t separator = name.find_last_of(':');
      separator != std::string_view::npos) {
    name = name.substr(separator + 1u);
  }

  std::string key;
  key.reserve(name.size());
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      key.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return key;
}

std::string lowerMetadataName(std::string_view name) {
  std::string result;
  result.reserve(name.size());
  for (char c : name) {
    result.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

bool isMappedIfcMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "guid" || key == "globalid" || key == "ifcguid" ||
         key == "elementguid" || key == "uniqueid" || key == "type" ||
         key == "ifctype" || key == "ifcclass" || key == "class" ||
         key == "category" || key == "displayname" || key == "name" ||
         key == "productname" || key == "objecttype" ||
         key == "storeyname" || key == "buildingstoreyname" ||
         key == "levelname" || key == "storeyid" || key == "storeyguid" ||
         key == "buildingstoreyid" || key == "buildingstoreyguid" ||
         key == "levelid" || key == "materialname" ||
         key == "ifcmaterialname" || key == "materialcategory" ||
         key == "ifcmaterialcategory" || key == "discipline" ||
         key == "ifcdiscipline" || key == "phase" || key == "phasename" ||
         key == "constructionphase" || key == "ifcphase" ||
         key == "firerating" || key == "fireclassification" ||
         key == "loadbearing" || key == "isloadbearing" || key == "status" ||
         key == "elementstatus";
}

std::vector<std::string> splitMetadataPath(std::string_view name) {
  std::vector<std::string> result;
  size_t cursor = 0;
  while (cursor < name.size()) {
    size_t separator = name.find("::", cursor);
    size_t separatorLength = 2u;
    const size_t dot = name.find('.', cursor);
    if (dot != std::string_view::npos &&
        (separator == std::string_view::npos || dot < separator)) {
      separator = dot;
      separatorLength = 1u;
    }
    const std::string_view segment =
        separator == std::string_view::npos
            ? name.substr(cursor)
            : name.substr(cursor, separator - cursor);
    if (!segment.empty()) {
      result.emplace_back(segment);
    }
    if (separator == std::string_view::npos) {
      break;
    }
    cursor = separator + separatorLength;
  }
  return result;
}

bool startsWithAsciiInsensitive(std::string_view value,
                                std::string_view prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin(),
                    [](char lhs, char rhs) {
                      return std::tolower(static_cast<unsigned char>(lhs)) ==
                             std::tolower(static_cast<unsigned char>(rhs));
                    });
}

bool isIfcGeoreferenceAttributeName(std::string_view name) {
  const std::string lower = lowerMetadataName(name);
  return lower.find("projectedcrs") != std::string::npos ||
         lower.find("mapconversion") != std::string::npos ||
         lower.find("coordinateoperation") != std::string::npos ||
         lower.find("georeference") != std::string::npos ||
         lower.find("crs") != std::string::npos ||
         lower.find("eastings") != std::string::npos ||
         lower.find("northings") != std::string::npos ||
         lower.find("orthogonalheight") != std::string::npos ||
         lower.find("sourceupaxis") != std::string::npos ||
         lower == "upaxis" || lower == "usd::upaxis";
}

bool isIfcPropertyAttributeName(std::string_view name) {
  const std::string lower = lowerMetadataName(name);
  if (lower.find("presentation::") != std::string::npos ||
      lower.find("usd::") != std::string::npos ||
      lower.find("gltf::") != std::string::npos ||
      lower.find("points::") != std::string::npos ||
      lower.find("pcd::") != std::string::npos ||
      lower == "sourceid" || lower == "source_id") {
    return false;
  }
  if (isMappedIfcMetadataName(name) || isIfcGeoreferenceAttributeName(name)) {
    return false;
  }
  return lower.find("bsi::ifc") != std::string::npos ||
         lower.find("ifc::") != std::string::npos ||
         lower.find("pset") != std::string::npos ||
         lower.find("propertyset") != std::string::npos ||
         lower.find("classification") != std::string::npos ||
         lower.find("quantity") != std::string::npos ||
         lower.find("qto") != std::string::npos ||
         lower.find("system") != std::string::npos ||
         lower.find("zone") != std::string::npos ||
         lower.find("space") != std::string::npos;
}

std::string propertyCategoryForAttribute(std::string_view name) {
  const std::string lower = lowerMetadataName(name);
  if (lower.find("classification") != std::string::npos) {
    return "classification";
  }
  if (lower.find("quantity") != std::string::npos ||
      lower.find("qto") != std::string::npos) {
    return "quantity";
  }
  if (lower.find("system") != std::string::npos ||
      lower.find("zone") != std::string::npos ||
      lower.find("space") != std::string::npos) {
    return "reference";
  }
  if (lower.find("pset") != std::string::npos ||
      lower.find("propertyset") != std::string::npos ||
      lower.find("::prop::") != std::string::npos) {
    return "pset";
  }
  return "ifc";
}

std::optional<dotbim::ElementProperty>
propertyFromIfcAttribute(std::string_view name, const Json &value) {
  if (!isIfcPropertyAttributeName(name)) {
    return std::nullopt;
  }
  const auto string = stringValue(value);
  if (!string || string->empty()) {
    return std::nullopt;
  }

  dotbim::ElementProperty property{};
  property.category = propertyCategoryForAttribute(name);
  property.value = *string;

  std::vector<std::string> segments = splitMetadataPath(name);
  while (!segments.empty() &&
         (segments.front() == "bsi" || segments.front() == "ifc")) {
    segments.erase(segments.begin());
  }
  if (!segments.empty() && segments.front() == "ifc") {
    segments.erase(segments.begin());
  }

  for (size_t i = 0; i < segments.size(); ++i) {
    const std::string &segment = segments[i];
    if (startsWithAsciiInsensitive(segment, "Pset_") ||
        startsWithAsciiInsensitive(segment, "Qto_")) {
      property.set = segment;
      if (i + 1u < segments.size()) {
        property.name = segments[i + 1u];
      }
      break;
    }
    const std::string key = metadataLookupKey(segment);
    if ((key == "pset" || key == "propertyset" || key == "prop" ||
         key == "quantity" || key == "classification" || key == "system" ||
         key == "zone" || key == "space") &&
        i + 1u < segments.size()) {
      property.set = segment;
      property.name = segments[i + 1u];
      break;
    }
  }

  if (property.name.empty() && !segments.empty()) {
    property.name = segments.back();
    if (segments.size() > 1u) {
      property.set = segments[segments.size() - 2u];
    }
  }
  if (isMappedIfcMetadataName(property.name)) {
    return std::nullopt;
  }
  return property;
}

void appendProperty(std::vector<dotbim::ElementProperty> &properties,
                    dotbim::ElementProperty property) {
  if (property.name.empty() || property.value.empty()) {
    return;
  }
  const auto duplicate = std::ranges::find_if(
      properties, [&](const dotbim::ElementProperty &existing) {
        return existing.set == property.set && existing.name == property.name &&
               existing.category == property.category;
      });
  if (duplicate == properties.end()) {
    properties.push_back(std::move(property));
  }
}

void collectPropertiesFromPath(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    std::vector<dotbim::ElementProperty> &properties,
    std::unordered_set<std::string> &visiting) {
  if (!visiting.insert(path).second) {
    return;
  }
  if (const auto nodeIt = nodes.find(path); nodeIt != nodes.end()) {
    const Node &node = nodeIt->second;
    for (auto it = node.attributes.begin(); it != node.attributes.end(); ++it) {
      if (auto property = propertyFromIfcAttribute(it.key(), it.value())) {
        appendProperty(properties, std::move(*property));
      }
    }
    for (const std::string &inheritedPath : node.inherits) {
      collectPropertiesFromPath(inheritedPath, nodes, properties, visiting);
    }
  }
  visiting.erase(path);
}

std::vector<dotbim::ElementProperty> inheritedProperties(
    const std::vector<std::string> &chain,
    const std::unordered_map<std::string, Node> &nodes) {
  std::vector<dotbim::ElementProperty> properties;
  std::unordered_set<std::string> checked;
  for (const std::string &path : chain) {
    if (!checked.insert(path).second) {
      continue;
    }
    std::unordered_set<std::string> visiting;
    collectPropertiesFromPath(path, nodes, properties, visiting);
  }
  return properties;
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

std::optional<glm::dvec3> readDVec3(const Json &value) {
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

void applyGeoreferenceAttribute(dotbim::ModelGeoreferenceMetadata &metadata,
                                std::string_view name, const Json &value) {
  const std::string lower = lowerMetadataName(name);
  if ((lower == "upaxis" || lower == "usd::upaxis" ||
       lower.find("sourceupaxis") != std::string::npos) &&
      stringValue(value)) {
    metadata.sourceUpAxis = *stringValue(value);
    metadata.hasSourceUpAxis = !metadata.sourceUpAxis.empty();
    return;
  }
  if (lower.find("coordinateoffset") != std::string::npos ||
      lower.find("falseorigin") != std::string::npos ||
      lower.find("siteorigin") != std::string::npos) {
    if (auto offset = readDVec3(value)) {
      metadata.coordinateOffset = *offset;
      metadata.hasCoordinateOffset = true;
      metadata.coordinateOffsetSource = std::string(name);
      return;
    }
  }
  if (lower.find("eastings") != std::string::npos ||
      lower.find("easting") != std::string::npos) {
    if (const auto number = numberAsDouble(value)) {
      metadata.coordinateOffset.x = *number;
      metadata.hasCoordinateOffset = true;
      metadata.coordinateOffsetSource = "IFCX map conversion";
    }
  } else if (lower.find("northings") != std::string::npos ||
             lower.find("northing") != std::string::npos) {
    if (const auto number = numberAsDouble(value)) {
      metadata.coordinateOffset.y = *number;
      metadata.hasCoordinateOffset = true;
      metadata.coordinateOffsetSource = "IFCX map conversion";
    }
  } else if (lower.find("orthogonalheight") != std::string::npos ||
             (lower.find("mapconversion") != std::string::npos &&
              lower.find("height") != std::string::npos)) {
    if (const auto number = numberAsDouble(value)) {
      metadata.coordinateOffset.z = *number;
      metadata.hasCoordinateOffset = true;
      metadata.coordinateOffsetSource = "IFCX map conversion";
    }
  }

  const auto string = stringValue(value);
  if (!string || string->empty()) {
    return;
  }
  if (metadata.crsName.empty() &&
      ((lower.find("crs") != std::string::npos &&
        lower.find("name") != std::string::npos) ||
       lower.find("projectedcrs::name") != std::string::npos)) {
    metadata.crsName = *string;
  } else if (metadata.crsAuthority.empty() &&
             lower.find("authority") != std::string::npos) {
    metadata.crsAuthority = *string;
  } else if (metadata.crsCode.empty() &&
             (lower.find("epsg") != std::string::npos ||
              (lower.find("crs") != std::string::npos &&
               lower.find("code") != std::string::npos))) {
    metadata.crsCode = *string;
    if (metadata.crsAuthority.empty() &&
        lower.find("epsg") != std::string::npos) {
      metadata.crsAuthority = "EPSG";
    }
  } else if (metadata.mapConversionName.empty() &&
             lower.find("mapconversion") != std::string::npos &&
             lower.find("name") != std::string::npos) {
    metadata.mapConversionName = *string;
  }
}

void applyGeoreferenceObject(dotbim::ModelGeoreferenceMetadata &metadata,
                             const Json &object) {
  if (!object.is_object()) {
    return;
  }
  for (auto it = object.begin(); it != object.end(); ++it) {
    applyGeoreferenceAttribute(metadata, it.key(), it.value());
  }
}

dotbim::ModelGeoreferenceMetadata readGeoreferenceMetadata(
    const Json &root, const NodeGraph &graph) {
  dotbim::ModelGeoreferenceMetadata metadata{};
  if (root.is_object()) {
    if (root.contains("attributes")) {
      applyGeoreferenceObject(metadata, root.at("attributes"));
    }
    for (const char *key : {"georeference", "georeferencing",
                            "geoReference"}) {
      if (root.contains(key)) {
        applyGeoreferenceObject(metadata, root.at(key));
      }
    }
  }
  for (const auto &[path, node] : graph.nodes) {
    (void)path;
    applyGeoreferenceObject(metadata, node.attributes);
  }
  if (!metadata.hasSourceUpAxis) {
    metadata.hasSourceUpAxis = true;
    metadata.sourceUpAxis = "Z";
  }
  return metadata;
}

std::optional<std::string> directStringAttribute(
    const Node &node, std::initializer_list<const char *> keys) {
  for (const char *key : keys) {
    const auto attrIt = node.attributes.find(key);
    if (attrIt == node.attributes.end()) {
      continue;
    }
    if (auto value = stringValue(attrIt.value()); value && !value->empty()) {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<std::string> inheritedStringAttributeFromPath(
    const std::string &path, const std::unordered_map<std::string, Node> &nodes,
    std::initializer_list<const char *> keys,
    std::unordered_set<std::string> &visiting) {
  if (!visiting.insert(path).second) {
    return std::nullopt;
  }

  std::optional<std::string> result;
  if (const auto nodeIt = nodes.find(path); nodeIt != nodes.end()) {
    const Node &node = nodeIt->second;
    result = directStringAttribute(node, keys);
    if (!result) {
      for (const std::string &inheritedPath : node.inherits) {
        result =
            inheritedStringAttributeFromPath(inheritedPath, nodes, keys,
                                             visiting);
        if (result) {
          break;
        }
      }
    }
  }

  visiting.erase(path);
  return result;
}

std::optional<std::string> inheritedStringAttribute(
    const std::vector<std::string> &chain,
    const std::unordered_map<std::string, Node> &nodes,
    std::initializer_list<const char *> keys) {
  for (const std::string &path : chain) {
    std::unordered_set<std::string> visiting;
    if (auto value =
            inheritedStringAttributeFromPath(path, nodes, keys, visiting)) {
      return value;
    }
  }
  return std::nullopt;
}

std::filesystem::path resolveAssetPath(const std::filesystem::path *sourceDir,
                                       const std::string &pathText) {
  std::filesystem::path path = container::util::pathFromUtf8(pathText);
  if (sourceDir != nullptr && path.is_relative()) {
    path = *sourceDir / path;
  }
  return path.lexically_normal();
}

void assignTexturePath(const Json &value,
                       const std::filesystem::path *sourceDir,
                       dotbim::MaterialTextureAsset &asset) {
  if (!value.is_string()) {
    return;
  }
  const std::string pathText = value.get<std::string>();
  if (pathText.empty()) {
    return;
  }
  asset.path = resolveAssetPath(sourceDir, pathText);
  asset.name = pathText;
}

void normalizeMaterialTexturePath(Json &value,
                                  const std::filesystem::path &sourceDir) {
  if (!value.is_string()) {
    return;
  }
  const std::string pathText = value.get<std::string>();
  if (pathText.empty()) {
    return;
  }
  value = container::util::pathToUtf8(resolveAssetPath(&sourceDir, pathText));
}

void normalizeGltfMaterialTexturePaths(Json &value,
                                       const std::filesystem::path &sourceDir) {
  if (!value.is_object()) {
    return;
  }

  if (value.contains("pbrMetallicRoughness") &&
      value.at("pbrMetallicRoughness").is_object()) {
    Json &pbr = value.at("pbrMetallicRoughness");
    if (pbr.contains("baseColorTexture")) {
      normalizeMaterialTexturePath(pbr.at("baseColorTexture"), sourceDir);
    }
    if (pbr.contains("metallicRoughnessTexture")) {
      normalizeMaterialTexturePath(pbr.at("metallicRoughnessTexture"),
                                   sourceDir);
    }
  }

  if (value.contains("normalTexture") &&
      value.at("normalTexture").is_object()) {
    Json &normal = value.at("normalTexture");
    if (normal.contains("texture")) {
      normalizeMaterialTexturePath(normal.at("texture"), sourceDir);
    }
  }
  if (value.contains("occlusionTexture") &&
      value.at("occlusionTexture").is_object()) {
    Json &occlusion = value.at("occlusionTexture");
    if (occlusion.contains("texture")) {
      normalizeMaterialTexturePath(occlusion.at("texture"), sourceDir);
    }
  }
  if (value.contains("emissiveTexture")) {
    normalizeMaterialTexturePath(value.at("emissiveTexture"), sourceDir);
  }
}

bool equalsAsciiInsensitive(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

container::material::AlphaMode alphaModeFromString(std::string_view mode) {
  if (equalsAsciiInsensitive(mode, "mask")) {
    return container::material::AlphaMode::Mask;
  }
  if (equalsAsciiInsensitive(mode, "blend")) {
    return container::material::AlphaMode::Blend;
  }
  return container::material::AlphaMode::Opaque;
}

dotbim::Material readGltfMaterial(const Json &value,
                                  const std::filesystem::path *sourceDir) {
  dotbim::Material material{};
  material.pbr.baseColor = glm::vec4(0.8f, 0.82f, 0.86f, 1.0f);
  material.pbr.roughnessFactor = 0.5f;

  if (!value.is_object()) {
    return material;
  }

  if (value.contains("pbrMetallicRoughness") &&
      value.at("pbrMetallicRoughness").is_object()) {
    const Json &pbr = value.at("pbrMetallicRoughness");
    if (pbr.contains("baseColorFactor")) {
      if (auto baseColor = readVec4(pbr.at("baseColorFactor"))) {
        material.pbr.baseColor =
            glm::clamp(*baseColor, glm::vec4(0.0f), glm::vec4(1.0f));
        material.pbr.opacityFactor = material.pbr.baseColor.a;
      }
    }
    material.pbr.metallicFactor =
        std::clamp(numberOr(pbr, "metallicFactor", material.pbr.metallicFactor),
                   0.0f, 1.0f);
    material.pbr.roughnessFactor = std::clamp(
        numberOr(pbr, "roughnessFactor", material.pbr.roughnessFactor), 0.0f,
        1.0f);
    if (pbr.contains("baseColorTexture")) {
      assignTexturePath(pbr.at("baseColorTexture"), sourceDir,
                        material.texturePaths.baseColor);
    }
    if (pbr.contains("metallicRoughnessTexture")) {
      assignTexturePath(pbr.at("metallicRoughnessTexture"), sourceDir,
                        material.texturePaths.metallicRoughness);
    }
  }

  if (value.contains("normalTexture")) {
    const Json &normal = value.at("normalTexture");
    if (normal.is_object()) {
      if (normal.contains("texture")) {
        assignTexturePath(normal.at("texture"), sourceDir,
                          material.texturePaths.normal);
      }
      material.pbr.normalTextureScale =
          numberOr(normal, "scale", material.pbr.normalTextureScale);
    }
  }
  if (value.contains("occlusionTexture")) {
    const Json &occlusion = value.at("occlusionTexture");
    if (occlusion.is_object()) {
      if (occlusion.contains("texture")) {
        assignTexturePath(occlusion.at("texture"), sourceDir,
                          material.texturePaths.occlusion);
      }
      material.pbr.occlusionStrength = std::clamp(
          numberOr(occlusion, "strength", material.pbr.occlusionStrength), 0.0f,
          1.0f);
    }
  }
  if (value.contains("emissiveTexture")) {
    assignTexturePath(value.at("emissiveTexture"), sourceDir,
                      material.texturePaths.emissive);
  }
  if (value.contains("emissiveFactor")) {
    if (auto emissive = readVec3(value.at("emissiveFactor"))) {
      material.pbr.emissiveColor =
          glm::clamp(*emissive, glm::vec3(0.0f),
                     glm::vec3(std::numeric_limits<float>::max()));
    }
  }
  if (value.contains("alphaMode") && value.at("alphaMode").is_string()) {
    material.pbr.alphaMode =
        alphaModeFromString(value.at("alphaMode").get<std::string>());
  } else if (material.pbr.baseColor.a < 0.999f) {
    material.pbr.alphaMode = container::material::AlphaMode::Blend;
  }
  material.pbr.alphaCutoff = std::clamp(
      numberOr(value, "alphaCutoff", material.pbr.alphaCutoff), 0.0f, 1.0f);
  if (value.contains("doubleSided") && value.at("doubleSided").is_boolean()) {
    material.pbr.doubleSided = value.at("doubleSided").get<bool>();
  }
  return material;
}

std::optional<uint32_t> inheritedMaterialIndex(
    Model &model, const std::vector<std::string> &chain,
    const std::unordered_map<std::string, Node> &nodes,
    const std::filesystem::path *sourceDir,
    std::unordered_map<std::string, uint32_t> &materialCache) {
  const Node *node = nearestNodeWithAttribute(chain, nodes, "gltf::material");
  if (node == nullptr) {
    return std::nullopt;
  }

  const auto cached = materialCache.find(node->path);
  if (cached != materialCache.end()) {
    return cached->second;
  }

  const uint32_t materialIndex = static_cast<uint32_t>(model.materials.size());
  model.materials.push_back(
      readGltfMaterial(node->attributes.at("gltf::material"), sourceDir));
  materialCache.emplace(node->path, materialIndex);
  return materialIndex;
}

void appendTriangle(Model &model, const glm::vec3 &a, const glm::vec3 &b,
                    const glm::vec3 &c, const glm::vec3 &color) {
  const glm::vec3 normal = safeNormal(a, b, c);
  const glm::vec3 tangent = safeTangent(a, b, normal);
  const uint32_t base = static_cast<uint32_t>(model.vertices.size());
  model.vertices.push_back(makeVertex(a, normal, tangent, color));
  model.vertices.push_back(makeVertex(b, normal, tangent, color));
  model.vertices.push_back(makeVertex(c, normal, tangent, color));
  model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
}

void appendQuad(Model &model, const glm::vec3 &a, const glm::vec3 &b,
                const glm::vec3 &c, const glm::vec3 &d,
                const glm::vec3 &color) {
  appendTriangle(model, a, b, c, color);
  appendTriangle(model, a, c, d, color);
}

void appendNativePointRange(Model &model, const PointCloud &cloud,
                            uint32_t meshId, const glm::vec3 &boundsCenter,
                            float boundsRadius) {
  if (cloud.positions.empty()) {
    return;
  }

  dotbim::NativePrimitiveRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  range.boundsCenter = boundsCenter;
  range.boundsRadius = boundsRadius;
  for (size_t i = 0; i < cloud.positions.size(); ++i) {
    const glm::vec3 color =
        i < cloud.colors.size() ? cloud.colors[i] : glm::vec3(1.0f);
    const uint32_t vertexIndex = static_cast<uint32_t>(model.vertices.size());
    model.vertices.push_back(makeVertex(cloud.positions[i],
                                        glm::vec3(0.0f, 1.0f, 0.0f),
                                        glm::vec3(1.0f, 0.0f, 0.0f), color));
    model.indices.push_back(vertexIndex);
  }
  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.nativePointRanges.push_back(range);
  }
}

void appendNativeCurveRange(Model &model, const std::vector<glm::vec3> &points,
                            const std::vector<size_t> &curveVertexCounts,
                            uint32_t meshId,
                            const glm::vec3 &boundsCenter,
                            float boundsRadius) {
  if (points.size() < 2u) {
    return;
  }

  dotbim::NativePrimitiveRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  range.boundsCenter = boundsCenter;
  range.boundsRadius = boundsRadius;
  size_t cursor = 0;
  for (const size_t count : curveVertexCounts) {
    for (size_t segment = 0; segment + 1u < count; ++segment) {
      const glm::vec3 a = points[cursor + segment];
      const glm::vec3 b = points[cursor + segment + 1u];
      if (glm::dot(b - a, b - a) <= 1.0e-12f) {
        continue;
      }
      const uint32_t base = static_cast<uint32_t>(model.vertices.size());
      constexpr glm::vec3 color{1.0f, 1.0f, 1.0f};
      model.vertices.push_back(makeVertex(a, glm::vec3(0.0f, 1.0f, 0.0f),
                                          glm::vec3(1.0f, 0.0f, 0.0f),
                                          color));
      model.vertices.push_back(makeVertex(b, glm::vec3(0.0f, 1.0f, 0.0f),
                                          glm::vec3(1.0f, 0.0f, 0.0f),
                                          color));
      model.indices.insert(model.indices.end(), {base, base + 1u});
    }
    cursor += count;
  }
  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.nativeCurveRanges.push_back(range);
  }
}

void appendMeshletClustersForRange(Model &model,
                                   const dotbim::MeshRange &range) {
  constexpr uint32_t kMeshletTriangleBudget = 64u;
  constexpr uint32_t kMeshletIndexBudget = kMeshletTriangleBudget * 3u;
  const uint32_t endIndex = range.firstIndex + range.indexCount;
  if (range.indexCount < 3u || endIndex > model.indices.size()) {
    return;
  }

  for (uint32_t firstIndex = range.firstIndex; firstIndex + 2u < endIndex;
       firstIndex += kMeshletIndexBudget) {
    const uint32_t indexCount =
        std::min(kMeshletIndexBudget, endIndex - firstIndex);
    const uint32_t triangleAlignedIndexCount = (indexCount / 3u) * 3u;
    if (triangleAlignedIndexCount == 0u) {
      continue;
    }
    std::vector<glm::vec3> clusterPoints;
    clusterPoints.reserve(triangleAlignedIndexCount);
    for (uint32_t offset = 0u; offset < triangleAlignedIndexCount; ++offset) {
      const uint32_t vertexIndex = model.indices[firstIndex + offset];
      if (vertexIndex < model.vertices.size()) {
        clusterPoints.push_back(model.vertices[vertexIndex].position);
      }
    }
    if (clusterPoints.empty()) {
      continue;
    }
    const auto [center, radius] = computeBounds(clusterPoints);
    model.meshletClusters.push_back(dotbim::MeshletClusterRange{
        .meshId = range.meshId,
        .firstIndex = firstIndex,
        .indexCount = triangleAlignedIndexCount,
        .triangleCount = triangleAlignedIndexCount / 3u,
        .lodLevel = 0u,
        .boundsCenter = center,
        .boundsRadius = radius,
    });
  }
}

bool appendMesh(Model &model, const Json &mesh, uint32_t meshId,
                std::string_view context) {
  const std::vector<glm::vec3> points = readPoints(mesh, context);
  const std::vector<uint32_t> sourceIndices =
      readTriangulatedIndices(mesh, context, points.size());
  if (points.empty() || sourceIndices.empty()) {
    return false;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(points);
  range.boundsCenter = center;
  range.boundsRadius = radius;

  for (size_t i = 0; i + 2u < sourceIndices.size(); i += 3u) {
    const glm::vec3 &a = points[sourceIndices[i]];
    const glm::vec3 &b = points[sourceIndices[i + 1u]];
    const glm::vec3 &c = points[sourceIndices[i + 2u]];
    appendTriangle(model, a, b, c, glm::vec3(1.0f));
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount == 0u) {
    return false;
  }
  model.meshRanges.push_back(range);
  appendMeshletClustersForRange(model, range);
  return true;
}

bool appendBasisCurves(Model &model, const Json &curves, uint32_t meshId,
                       std::string_view context) {
  const std::vector<glm::vec3> points = readPoints(curves, context);
  if (points.size() < 2u) {
    return false;
  }

  std::vector<size_t> curveVertexCounts;
  if (curves.contains("curveVertexCounts") &&
      curves.at("curveVertexCounts").is_array()) {
    const Json &counts = curves.at("curveVertexCounts");
    curveVertexCounts.reserve(counts.size());
    size_t total = 0;
    for (const Json &countValue : counts) {
      if (!countValue.is_number_integer() && !countValue.is_number_unsigned()) {
        throw std::runtime_error("IFCX " + std::string(context) +
                                 " has an invalid curve vertex count");
      }
      const auto count = countValue.get<int64_t>();
      if (count <= 0) {
        throw std::runtime_error("IFCX " + std::string(context) +
                                 " has a non-positive curve vertex count");
      }
      total += static_cast<size_t>(count);
      curveVertexCounts.push_back(static_cast<size_t>(count));
    }
    if (total != points.size()) {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has inconsistent curve vertex counts");
    }
  } else {
    curveVertexCounts.push_back(points.size());
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(points);
  const float tubeRadius =
      radius > 0.0f ? std::clamp(radius * 0.004f, 0.01f, 0.05f) : 0.02f;
  range.boundsCenter = center;
  range.boundsRadius = radius + tubeRadius;

  size_t cursor = 0;
  for (const size_t count : curveVertexCounts) {
    for (size_t segment = 0; segment + 1u < count; ++segment) {
      const glm::vec3 a = points[cursor + segment];
      const glm::vec3 b = points[cursor + segment + 1u];
      const glm::vec3 dir = b - a;
      if (glm::dot(dir, dir) <= 1.0e-12f) {
        continue;
      }
      const glm::vec3 forward = normalizeOrFallback(dir, {0.0f, 0.0f, 1.0f});
      const glm::vec3 axis = std::abs(forward.y) < 0.9f
                                 ? glm::vec3(0.0f, 1.0f, 0.0f)
                                 : glm::vec3(1.0f, 0.0f, 0.0f);
      const glm::vec3 side =
          normalizeOrFallback(glm::cross(axis, forward), {1.0f, 0.0f, 0.0f}) *
          tubeRadius;
      const glm::vec3 up =
          normalizeOrFallback(glm::cross(forward, side), {0.0f, 1.0f, 0.0f}) *
          tubeRadius;

      const std::array<glm::vec3, 4> start{a + side + up, a - side + up,
                                           a - side - up, a + side - up};
      const std::array<glm::vec3, 4> end{b + side + up, b - side + up,
                                         b - side - up, b + side - up};
      constexpr glm::vec3 color{1.0f, 1.0f, 1.0f};
      appendQuad(model, start[0], end[0], end[1], start[1], color);
      appendQuad(model, start[1], end[1], end[2], start[2], color);
      appendQuad(model, start[2], end[2], end[3], start[3], color);
      appendQuad(model, start[3], end[3], end[0], start[0], color);
      appendQuad(model, start[0], start[1], start[2], start[3], color);
      appendQuad(model, end[3], end[2], end[1], end[0], color);
    }
    cursor += count;
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount == 0u) {
    return false;
  }
  model.meshRanges.push_back(range);
  appendNativeCurveRange(model, points, curveVertexCounts, meshId, center,
                         radius);
  return true;
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

std::vector<uint8_t> decodeBase64(std::string_view text) {
  std::vector<uint8_t> result;
  uint32_t accumulator = 0;
  int bits = -8;
  for (char c : text) {
    if (c == '=') {
      break;
    }
    const int value = base64Value(c);
    if (value < 0) {
      continue;
    }
    accumulator = (accumulator << 6u) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 0) {
      result.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }
  return result;
}

uint32_t readLe32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8u) |
         (static_cast<uint32_t>(bytes[2]) << 16u) |
         (static_cast<uint32_t>(bytes[3]) << 24u);
}

float readLeFloat32(const uint8_t *bytes) {
  const uint32_t raw = readLe32(bytes);
  float value = 0.0f;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

std::vector<glm::vec3>
vec3ArrayFromFloatBytes(const std::vector<uint8_t> &bytes,
                        std::string_view context) {
  if ((bytes.size() % (sizeof(float) * 3u)) != 0u) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " base64 vec3 data has an invalid size");
  }

  std::vector<glm::vec3> result;
  result.reserve(bytes.size() / (sizeof(float) * 3u));
  for (size_t offset = 0; offset + 11u < bytes.size(); offset += 12u) {
    result.emplace_back(readLeFloat32(bytes.data() + offset),
                        readLeFloat32(bytes.data() + offset + 4u),
                        readLeFloat32(bytes.data() + offset + 8u));
  }
  return result;
}

PointCloud readPointsArray(const Json &value, std::string_view context) {
  if (!value.is_object() || !value.contains("positions") ||
      !value.at("positions").is_array()) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " points::array is missing positions");
  }

  PointCloud cloud{};
  const Json &positions = value.at("positions");
  cloud.positions.reserve(positions.size());
  for (const Json &position : positions) {
    if (auto parsed = readVec3(position)) {
      cloud.positions.push_back(*parsed);
    } else {
      throw std::runtime_error("IFCX " + std::string(context) +
                               " has an invalid point position");
    }
  }
  if (value.contains("colors") && value.at("colors").is_array()) {
    const Json &colors = value.at("colors");
    const size_t colorCount = std::min(colors.size(), cloud.positions.size());
    cloud.colors.assign(colorCount, glm::vec3(1.0f));
    for (size_t i = 0; i < colorCount; ++i) {
      if (auto parsed = readVec3(colors[i])) {
        cloud.colors[i] =
            glm::clamp(*parsed, glm::vec3(0.0f), glm::vec3(1.0f));
      }
    }
  }
  return cloud;
}

PointCloud readPointsBase64(const Json &value, std::string_view context) {
  if (!value.is_object() || !value.contains("positions") ||
      !value.at("positions").is_string()) {
    throw std::runtime_error("IFCX " + std::string(context) +
                             " points::base64 is missing positions");
  }

  PointCloud cloud{};
  cloud.positions = vec3ArrayFromFloatBytes(
      decodeBase64(value.at("positions").get<std::string>()), context);
  if (value.contains("colors") && value.at("colors").is_string()) {
    cloud.colors = vec3ArrayFromFloatBytes(
        decodeBase64(value.at("colors").get<std::string>()), context);
    for (glm::vec3 &color : cloud.colors) {
      color = glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f));
    }
  }
  return cloud;
}

std::string trimAscii(std::string value) {
  const auto first = std::ranges::find_if(
      value, [](unsigned char c) { return !std::isspace(c); });
  const auto last =
      std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
      }).base();
  if (first >= last) {
    return {};
  }
  return {first, last};
}

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool isLineMarkingSurfaceFeature(std::string_view type) {
  const std::string lower = lowerAscii(std::string(type));
  return lower.starts_with("ifcsurfacefeature") &&
         lower.find("linemarking") != std::string::npos;
}

void liftLineMarkingSurfaceFeature(dotbim::Element &element) {
  if (!isLineMarkingSurfaceFeature(element.type) ||
      element.geometryKind != dotbim::GeometryKind::Mesh) {
    return;
  }

  element.transform[3] +=
      element.transform[2] * kLineMarkingSurfaceFeatureOffset;
}

std::vector<std::string> splitWords(std::string_view line) {
  std::istringstream stream{std::string(line)};
  std::vector<std::string> words;
  std::string word;
  while (stream >> word) {
    words.push_back(word);
  }
  return words;
}

struct PcdHeader {
  std::vector<std::string> fields{};
  std::vector<uint32_t> sizes{};
  std::vector<char> types{};
  std::vector<uint32_t> counts{};
  size_t width{0};
  size_t height{1};
  size_t points{0};
  std::string data{};
  size_t dataOffset{0};
};

PcdHeader parsePcdHeader(const std::vector<uint8_t> &bytes) {
  PcdHeader header{};
  size_t cursor = 0;
  while (cursor < bytes.size()) {
    const size_t lineStart = cursor;
    while (cursor < bytes.size() && bytes[cursor] != '\n') {
      ++cursor;
    }
    const size_t lineEnd = cursor;
    if (cursor < bytes.size() && bytes[cursor] == '\n') {
      ++cursor;
    }

    std::string line(reinterpret_cast<const char *>(bytes.data() + lineStart),
                     lineEnd - lineStart);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    line = trimAscii(std::move(line));
    if (line.empty() || line.front() == '#') {
      continue;
    }

    std::vector<std::string> words = splitWords(line);
    if (words.empty()) {
      continue;
    }
    const std::string key = lowerAscii(words.front());
    if (key == "fields") {
      header.fields.assign(words.begin() + 1, words.end());
    } else if (key == "size") {
      for (auto it = words.begin() + 1; it != words.end(); ++it) {
        header.sizes.push_back(static_cast<uint32_t>(std::stoul(*it)));
      }
    } else if (key == "type") {
      for (auto it = words.begin() + 1; it != words.end(); ++it) {
        header.types.push_back(it->empty() ? 'F' : (*it)[0]);
      }
    } else if (key == "count") {
      for (auto it = words.begin() + 1; it != words.end(); ++it) {
        header.counts.push_back(static_cast<uint32_t>(std::stoul(*it)));
      }
    } else if (key == "width" && words.size() > 1u) {
      header.width = static_cast<size_t>(std::stoull(words[1]));
    } else if (key == "height" && words.size() > 1u) {
      header.height = static_cast<size_t>(std::stoull(words[1]));
    } else if (key == "points" && words.size() > 1u) {
      header.points = static_cast<size_t>(std::stoull(words[1]));
    } else if (key == "data" && words.size() > 1u) {
      header.data = lowerAscii(words[1]);
      header.dataOffset = cursor;
      break;
    }
  }

  const size_t fieldCount = header.fields.size();
  if (header.sizes.empty()) {
    header.sizes.assign(fieldCount, 4u);
  }
  if (header.types.empty()) {
    header.types.assign(fieldCount, 'F');
  }
  if (header.counts.empty()) {
    header.counts.assign(fieldCount, 1u);
  }
  if (header.points == 0u && header.width > 0u) {
    const size_t height = std::max<size_t>(header.height, 1u);
    if (header.width > std::numeric_limits<size_t>::max() / height) {
      throw std::runtime_error("IFCX PCD width and height overflow points");
    }
    header.points = header.width * height;
  }
  if (header.sizes.size() != fieldCount || header.types.size() != fieldCount ||
      header.counts.size() != fieldCount || fieldCount == 0u ||
      header.points == 0u || header.data.empty()) {
    throw std::runtime_error("IFCX PCD header is incomplete");
  }
  return header;
}

std::optional<size_t> pcdFieldIndex(const PcdHeader &header,
                                    std::string_view name) {
  for (size_t i = 0; i < header.fields.size(); ++i) {
    if (lowerAscii(header.fields[i]) == name) {
      return i;
    }
  }
  return std::nullopt;
}

uint32_t pcdFieldSize(const PcdHeader &header, size_t field) {
  return header.sizes[field] * header.counts[field];
}

std::vector<uint32_t> pcdFieldOffsets(const PcdHeader &header) {
  std::vector<uint32_t> offsets(header.fields.size(), 0u);
  uint32_t offset = 0;
  for (size_t i = 0; i < header.fields.size(); ++i) {
    offsets[i] = offset;
    offset += pcdFieldSize(header, i);
  }
  return offsets;
}

float readPcdScalar(const uint8_t *bytes, uint32_t size, char type) {
  if (type == 'F' || type == 'f') {
    if (size == 4u) {
      return readLeFloat32(bytes);
    }
    if (size == 8u) {
      uint64_t raw = 0;
      for (size_t i = 0; i < 8u; ++i) {
        raw |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
      }
      double value = 0.0;
      std::memcpy(&value, &raw, sizeof(value));
      return static_cast<float>(value);
    }
  }
  if (type == 'U' || type == 'u') {
    uint32_t value = 0;
    for (uint32_t i = 0; i < std::min(size, 4u); ++i) {
      value |= static_cast<uint32_t>(bytes[i]) << (i * 8u);
    }
    return static_cast<float>(value);
  }
  const uint32_t byteCount = std::min(size, 4u);
  uint32_t raw = 0;
  for (uint32_t i = 0; i < byteCount; ++i) {
    raw |= static_cast<uint32_t>(bytes[i]) << (i * 8u);
  }
  if (byteCount > 0u && byteCount < 4u) {
    const uint32_t signBit = 1u << (byteCount * 8u - 1u);
    if ((raw & signBit) != 0u) {
      raw |= ~((1u << (byteCount * 8u)) - 1u);
    }
  }
  const int32_t value = static_cast<int32_t>(raw);
  return static_cast<float>(value);
}

glm::vec3 colorFromPackedRgb(uint32_t packed) {
  return {static_cast<float>((packed >> 16u) & 0xffu) / 255.0f,
          static_cast<float>((packed >> 8u) & 0xffu) / 255.0f,
          static_cast<float>(packed & 0xffu) / 255.0f};
}

std::vector<uint8_t> lzfDecompress(const uint8_t *input, size_t inputSize,
                                   size_t outputSize) {
  std::vector<uint8_t> output(outputSize);
  size_t in = 0;
  size_t out = 0;
  while (in < inputSize && out < outputSize) {
    const uint8_t ctrl = input[in++];
    if (ctrl < 32u) {
      const size_t length = static_cast<size_t>(ctrl) + 1u;
      if (length > inputSize - in || length > outputSize - out) {
        throw std::runtime_error("IFCX PCD LZF literal overruns buffer");
      }
      std::memcpy(output.data() + out, input + in, length);
      in += length;
      out += length;
      continue;
    }

    size_t length = ctrl >> 5u;
    size_t referenceOffset = static_cast<size_t>(ctrl & 0x1fu) << 8u;
    if (length == 7u) {
      if (in >= inputSize) {
        throw std::runtime_error("IFCX PCD LZF length is truncated");
      }
      length += input[in++];
    }
    if (in >= inputSize) {
      throw std::runtime_error("IFCX PCD LZF reference is truncated");
    }
    referenceOffset += input[in++] + 1u;
    length += 2u;
    if (referenceOffset > out || length > outputSize - out) {
      throw std::runtime_error("IFCX PCD LZF reference overruns buffer");
    }
    size_t reference = out - referenceOffset;
    for (size_t i = 0; i < length; ++i) {
      output[out++] = output[reference++];
    }
  }
  if (out != outputSize) {
    throw std::runtime_error("IFCX PCD LZF output size mismatch");
  }
  return output;
}

PointCloud readPcdAscii(const PcdHeader &header,
                        const std::vector<uint8_t> &bytes) {
  const auto xField = pcdFieldIndex(header, "x");
  const auto yField = pcdFieldIndex(header, "y");
  const auto zField = pcdFieldIndex(header, "z");
  if (!xField || !yField || !zField) {
    return {};
  }
  const auto rgbField = pcdFieldIndex(header, "rgb");
  const auto rgbaField = pcdFieldIndex(header, "rgba");

  std::string body(
      reinterpret_cast<const char *>(bytes.data() + header.dataOffset),
      bytes.size() - header.dataOffset);
  std::istringstream stream(body);
  PointCloud cloud{};
  cloud.positions.reserve(header.points);
  if (rgbField || rgbaField) {
    cloud.colors.reserve(header.points);
  }

  std::string line;
  while (std::getline(stream, line) && cloud.positions.size() < header.points) {
    const std::vector<std::string> words = splitWords(line);
    if (words.size() < header.fields.size()) {
      continue;
    }
    cloud.positions.emplace_back(std::stof(words[*xField]),
                                 std::stof(words[*yField]),
                                 std::stof(words[*zField]));
    const auto colorField = rgbField ? rgbField : rgbaField;
    if (colorField) {
      uint32_t packed = 0;
      if (header.types[*colorField] == 'F' ||
          header.types[*colorField] == 'f') {
        const float packedFloat = std::stof(words[*colorField]);
        std::memcpy(&packed, &packedFloat, sizeof(packed));
      } else {
        packed = static_cast<uint32_t>(std::stoul(words[*colorField]));
      }
      cloud.colors.push_back(colorFromPackedRgb(packed));
    }
  }
  return cloud;
}

PointCloud readPcdBinaryPayload(const PcdHeader &header,
                                std::vector<uint8_t> payload, bool compressed) {
  const auto xField = pcdFieldIndex(header, "x");
  const auto yField = pcdFieldIndex(header, "y");
  const auto zField = pcdFieldIndex(header, "z");
  if (!xField || !yField || !zField) {
    return {};
  }

  if (compressed) {
    if (payload.size() < 8u) {
      throw std::runtime_error("IFCX compressed PCD payload is too small");
    }
    const uint32_t compressedSize = readLe32(payload.data());
    const uint32_t uncompressedSize = readLe32(payload.data() + 4u);
    if (8u + compressedSize > payload.size()) {
      throw std::runtime_error("IFCX compressed PCD payload is truncated");
    }
    payload =
        lzfDecompress(payload.data() + 8u, compressedSize, uncompressedSize);
  }

  const std::vector<uint32_t> offsets = pcdFieldOffsets(header);
  uint32_t pointStep = 0;
  for (size_t i = 0; i < header.fields.size(); ++i) {
    pointStep += pcdFieldSize(header, i);
  }
  if (pointStep == 0u) {
    return {};
  }

  auto fieldPointer = [&](size_t point, size_t field) -> const uint8_t * {
    const size_t scalarSize = header.sizes[field];
    if (compressed) {
      size_t base = 0;
      for (size_t i = 0; i < field; ++i) {
        base += static_cast<size_t>(pcdFieldSize(header, i)) * header.points;
      }
      const size_t offset =
          base + point * static_cast<size_t>(pcdFieldSize(header, field));
      return offset + scalarSize <= payload.size() ? payload.data() + offset
                                                   : nullptr;
    }
    const size_t offset =
        point * static_cast<size_t>(pointStep) + offsets[field];
    return offset + scalarSize <= payload.size() ? payload.data() + offset
                                                 : nullptr;
  };

  PointCloud cloud{};
  cloud.positions.reserve(header.points);
  const auto rgbField = pcdFieldIndex(header, "rgb");
  const auto rgbaField = pcdFieldIndex(header, "rgba");
  if (rgbField || rgbaField) {
    cloud.colors.reserve(header.points);
  }
  for (size_t point = 0; point < header.points; ++point) {
    const uint8_t *x = fieldPointer(point, *xField);
    const uint8_t *y = fieldPointer(point, *yField);
    const uint8_t *z = fieldPointer(point, *zField);
    if (x == nullptr || y == nullptr || z == nullptr) {
      break;
    }
    cloud.positions.emplace_back(
        readPcdScalar(x, header.sizes[*xField], header.types[*xField]),
        readPcdScalar(y, header.sizes[*yField], header.types[*yField]),
        readPcdScalar(z, header.sizes[*zField], header.types[*zField]));
    const auto colorField = rgbField ? rgbField : rgbaField;
    if (colorField) {
      const uint8_t *rgb = fieldPointer(point, *colorField);
      if (rgb != nullptr) {
        uint32_t packed = 0;
        if (header.types[*colorField] == 'F' ||
            header.types[*colorField] == 'f') {
          const float packedFloat = readPcdScalar(
              rgb, header.sizes[*colorField], header.types[*colorField]);
          std::memcpy(&packed, &packedFloat, sizeof(packed));
        } else {
          packed = static_cast<uint32_t>(readPcdScalar(
              rgb, header.sizes[*colorField], header.types[*colorField]));
        }
        cloud.colors.push_back(colorFromPackedRgb(packed));
      }
    }
  }
  return cloud;
}

PointCloud readPcdBase64(const Json &value) {
  if (!value.is_string()) {
    return {};
  }

  const std::vector<uint8_t> bytes = decodeBase64(value.get<std::string>());
  const PcdHeader header = parsePcdHeader(bytes);
  if (header.data == "ascii") {
    return readPcdAscii(header, bytes);
  }
  if (header.data == "binary" || header.data == "binary_compressed") {
    std::vector<uint8_t> payload(
        bytes.begin() + static_cast<std::ptrdiff_t>(header.dataOffset),
        bytes.end());
    return readPcdBinaryPayload(header, std::move(payload),
                                header.data == "binary_compressed");
  }
  return {};
}

bool appendPointCloud(Model &model, const PointCloud &cloud, uint32_t meshId) {
  if (cloud.positions.empty()) {
    return false;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(cloud.positions);
  const float pointRadius =
      radius > 0.0f ? std::clamp(radius * 0.012f, 0.006f, 0.04f) : 0.02f;
  range.boundsCenter = center;
  range.boundsRadius = radius + pointRadius;

  for (size_t i = 0; i < cloud.positions.size(); ++i) {
    const glm::vec3 color =
        i < cloud.colors.size() ? cloud.colors[i] : glm::vec3(1.0f);
    const glm::vec3 p = cloud.positions[i];
    const glm::vec3 xp = p + glm::vec3(pointRadius, 0.0f, 0.0f);
    const glm::vec3 xn = p - glm::vec3(pointRadius, 0.0f, 0.0f);
    const glm::vec3 yp = p + glm::vec3(0.0f, pointRadius, 0.0f);
    const glm::vec3 yn = p - glm::vec3(0.0f, pointRadius, 0.0f);
    const glm::vec3 zp = p + glm::vec3(0.0f, 0.0f, pointRadius);
    const glm::vec3 zn = p - glm::vec3(0.0f, 0.0f, pointRadius);

    appendTriangle(model, zp, xp, yp, color);
    appendTriangle(model, zp, yp, xn, color);
    appendTriangle(model, zp, xn, yn, color);
    appendTriangle(model, zp, yn, xp, color);
    appendTriangle(model, zn, yp, xp, color);
    appendTriangle(model, zn, xn, yp, color);
    appendTriangle(model, zn, yn, xn, color);
    appendTriangle(model, zn, xp, yn, color);
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount == 0u) {
    return false;
  }
  model.meshRanges.push_back(range);
  appendNativePointRange(model, cloud, meshId, center, radius);
  return true;
}

struct SceneBuilder {
  Model model{};
  const NodeGraph &graph;
  const std::filesystem::path *sourceDir{nullptr};
  std::unordered_map<std::string, uint32_t> geometryCache{};
  std::unordered_map<std::string, uint32_t> materialCache{};
  std::unordered_map<std::string, uint32_t> materialOpacityCache{};
  uint32_t nextMeshId{0};
  glm::mat4 importTransform{1.0f};
};

bool appendGeometryIfNeeded(SceneBuilder &builder, const std::string &key,
                            const Json &geometry, std::string_view context) {
  if (builder.geometryCache.contains(key)) {
    return true;
  }

  const uint32_t meshId = builder.nextMeshId++;
  bool appended = false;
  if (key.starts_with("mesh:")) {
    appended = appendMesh(builder.model, geometry, meshId, context);
  } else if (key.starts_with("curve:")) {
    appended = appendBasisCurves(builder.model, geometry, meshId, context);
  }
  if (!appended) {
    return false;
  }
  builder.geometryCache.emplace(key, meshId);
  return true;
}

void appendElement(SceneBuilder &builder, const std::string &geometryKey,
                   const glm::mat4 &worldTransform,
                   const std::vector<std::string> &chain,
                   const std::string &logicalPath) {
  const auto meshIt = builder.geometryCache.find(geometryKey);
  if (meshIt == builder.geometryCache.end()) {
    return;
  }

  glm::vec4 color = inheritedColor(chain, builder.graph.nodes);
  std::optional<uint32_t> materialIndex =
      inheritedMaterialIndex(builder.model, chain, builder.graph.nodes,
                             builder.sourceDir, builder.materialCache);

  dotbim::Element element{};
  element.meshId = meshIt->second;
  if (geometryKey.starts_with("curve:")) {
    element.geometryKind = dotbim::GeometryKind::Curves;
  } else if (geometryKey.starts_with("points-") ||
             geometryKey.starts_with("pcd-")) {
    element.geometryKind = dotbim::GeometryKind::Points;
  }
  element.transform = builder.importTransform * worldTransform;
  element.color = color;
  if (materialIndex) {
    const dotbim::Material &material = builder.model.materials[*materialIndex];
    element.color = material.pbr.baseColor;
    const float inheritedAlpha = std::clamp(color.a, 0.0f, 1.0f);
    if (inheritedAlpha < 0.999f) {
      dotbim::Material adjustedMaterial = material;
      adjustedMaterial.pbr.baseColor.a =
          std::clamp(material.pbr.baseColor.a * inheritedAlpha, 0.0f, 1.0f);
      if (adjustedMaterial.pbr.baseColor.a < 0.999f &&
          adjustedMaterial.pbr.alphaMode ==
              container::material::AlphaMode::Opaque) {
        adjustedMaterial.pbr.alphaMode = container::material::AlphaMode::Blend;
      }
      const auto alphaKey = static_cast<int64_t>(
          std::llround(adjustedMaterial.pbr.baseColor.a * 1000000.0f));
      const std::string cacheKey =
          std::to_string(*materialIndex) + ":" + std::to_string(alphaKey);
      const auto cached = builder.materialOpacityCache.find(cacheKey);
      if (cached != builder.materialOpacityCache.end()) {
        element.materialIndex = cached->second;
      } else {
        element.materialIndex =
            static_cast<uint32_t>(builder.model.materials.size());
        builder.model.materials.push_back(std::move(adjustedMaterial));
        builder.materialOpacityCache.emplace(cacheKey, element.materialIndex);
      }
      element.color = builder.model.materials[element.materialIndex].pbr.baseColor;
    } else {
      element.materialIndex = *materialIndex;
    }
    element.doubleSided = material.pbr.doubleSided;
  }
  element.guid =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::globalId", "bsi::ifc::GlobalId",
                                "ifc::globalId", "globalId", "GlobalId",
                                "guid"})
          .value_or(logicalPath);
  element.type = inheritedType(chain, builder.graph.nodes);
  liftLineMarkingSurfaceFeature(element);
  element.displayName =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::name", "bsi::ifc::prop::Name",
                                "bsi::ifc::prop::name", "displayName",
                                "display_name", "name", "Name"})
          .value_or("");
  element.objectType =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::objectType", "objectType",
                                "object_type"})
          .value_or("");
  element.storeyName =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::storeyName",
                                "bsi::ifc::storey::name", "storeyName",
                                "storey_name"})
          .value_or("");
  element.storeyId =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::storeyId",
                                "bsi::ifc::storey::globalId", "storeyId",
                                "storey_id", "storeyGuid", "storey_guid"})
          .value_or("");
  element.materialName =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::materialName",
                                "bsi::ifc::material::name", "materialName",
                                "material_name"})
          .value_or("");
  element.materialCategory =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::materialCategory",
                                "bsi::ifc::material::category",
                                "materialCategory", "material_category"})
          .value_or("");
  element.discipline =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::discipline", "ifc::discipline",
                                "discipline", "Discipline"})
          .value_or("");
  element.phase =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::phase", "bsi::ifc::phaseName",
                                "bsi::ifc::constructionPhase", "ifc::phase",
                                "phase", "phaseName", "phase_name",
                                "constructionPhase", "construction_phase"})
          .value_or("");
  element.fireRating =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::fireRating",
                                "bsi::ifc::FireRating",
                                "bsi::ifc::prop::FireRating",
                                "bsi::ifc::pset::FireRating",
                                "bsi::ifc::Pset_WallCommon::FireRating",
                                "Pset_WallCommon.FireRating", "fireRating",
                                "fire_rating", "FireRating"})
          .value_or("");
  element.loadBearing =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::loadBearing",
                                "bsi::ifc::LoadBearing",
                                "bsi::ifc::prop::LoadBearing",
                                "bsi::ifc::pset::LoadBearing",
                                "bsi::ifc::Pset_WallCommon::LoadBearing",
                                "Pset_WallCommon.LoadBearing", "loadBearing",
                                "load_bearing", "LoadBearing",
                                "isLoadBearing", "is_load_bearing"})
          .value_or("");
  element.status =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"bsi::ifc::status", "bsi::ifc::Status",
                                "bsi::ifc::prop::Status",
                                "bsi::ifc::pset::Status",
                                "Pset_WallCommon.Status", "status", "Status",
                                "elementStatus", "element_status"})
          .value_or("");
  element.sourceId =
      inheritedStringAttribute(chain, builder.graph.nodes,
                               {"sourceId", "source_id"})
          .value_or(logicalPath);
  element.properties = inheritedProperties(chain, builder.graph.nodes);
  builder.model.elements.push_back(std::move(element));
}

std::vector<std::string>
makeMetadataChain(const std::vector<std::string> &localChain,
                  const std::vector<std::string> &overrideChain,
                  const std::vector<std::string> &fallbackChain) {
  std::vector<std::string> chain;
  chain.reserve(localChain.size() + overrideChain.size() +
                fallbackChain.size());
  chain.insert(chain.end(), localChain.begin(), localChain.end());
  chain.insert(chain.end(), overrideChain.begin(), overrideChain.end());
  chain.insert(chain.end(), fallbackChain.begin(), fallbackChain.end());
  return chain;
}

void traverseNode(SceneBuilder &builder, const std::string &path,
                  const std::string &logicalPath,
                  const glm::mat4 &parentTransform,
                  const std::vector<std::string> &localChain,
                  const std::vector<std::string> &overrideChain,
                  const std::vector<std::string> &fallbackChain,
                  bool currentIsFallback,
                  std::unordered_set<std::string> &visiting) {
  const auto nodeIt = builder.graph.nodes.find(path);
  if (nodeIt == builder.graph.nodes.end()) {
    return;
  }
  const Node &node = nodeIt->second;
  if (!visiting.insert(path).second) {
    return;
  }

  const glm::mat4 worldTransform =
      parentTransform * readUsdTransform(node.attributes);
  std::vector<std::string> nextLocalChain = localChain;
  std::vector<std::string> nextFallbackChain = fallbackChain;
  if (currentIsFallback) {
    nextFallbackChain.insert(nextFallbackChain.begin(), path);
  } else {
    nextLocalChain.insert(nextLocalChain.begin(), path);
  }
  const std::vector<std::string> chain =
      makeMetadataChain(nextLocalChain, overrideChain, nextFallbackChain);

  if (!isInvisible(chain, builder.graph.nodes)) {
    if (node.attributes.contains("usd::usdgeom::mesh") &&
        node.attributes.at("usd::usdgeom::mesh").is_object()) {
      const std::string key = "mesh:" + path;
      if (appendGeometryIfNeeded(builder, key,
                                 node.attributes.at("usd::usdgeom::mesh"),
                                 "mesh[" + logicalPath + "]")) {
        appendElement(builder, key, worldTransform, chain, logicalPath);
      }
    }
    if (node.attributes.contains("usd::usdgeom::basiscurves") &&
        node.attributes.at("usd::usdgeom::basiscurves").is_object()) {
      const std::string key = "curve:" + path;
      if (appendGeometryIfNeeded(
              builder, key, node.attributes.at("usd::usdgeom::basiscurves"),
              "basiscurves[" + logicalPath + "]")) {
        appendElement(builder, key, worldTransform, chain, logicalPath);
      }
    }
    if (node.attributes.contains("points::array") &&
        node.attributes.at("points::array").is_object()) {
      const std::string key = "points-array:" + path;
      if (!builder.geometryCache.contains(key)) {
        const uint32_t meshId = builder.nextMeshId++;
        const PointCloud cloud = readPointsArray(
            node.attributes.at("points::array"), "points[" + logicalPath + "]");
        if (appendPointCloud(builder.model, cloud, meshId)) {
          builder.geometryCache.emplace(key, meshId);
        }
      }
      appendElement(builder, key, worldTransform, chain, logicalPath);
    }
    if (node.attributes.contains("points::base64") &&
        node.attributes.at("points::base64").is_object()) {
      const std::string key = "points-base64:" + path;
      if (!builder.geometryCache.contains(key)) {
        const uint32_t meshId = builder.nextMeshId++;
        const PointCloud cloud =
            readPointsBase64(node.attributes.at("points::base64"),
                             "points-base64[" + logicalPath + "]");
        if (appendPointCloud(builder.model, cloud, meshId)) {
          builder.geometryCache.emplace(key, meshId);
        }
      }
      appendElement(builder, key, worldTransform, chain, logicalPath);
    }
    if (node.attributes.contains("pcd::base64")) {
      const std::string key = "pcd-base64:" + path;
      if (!builder.geometryCache.contains(key)) {
        const uint32_t meshId = builder.nextMeshId++;
        const PointCloud cloud =
            readPcdBase64(node.attributes.at("pcd::base64"));
        if (appendPointCloud(builder.model, cloud, meshId)) {
          builder.geometryCache.emplace(key, meshId);
        }
      }
      appendElement(builder, key, worldTransform, chain, logicalPath);
    }

    for (const std::string &childPath : node.children) {
      traverseNode(builder, childPath, logicalPath + "/" + childPath,
                   worldTransform, nextLocalChain, overrideChain,
                   nextFallbackChain, false, visiting);
    }
    for (const std::string &inheritedPath : node.inherits) {
      traverseNode(builder, inheritedPath, logicalPath + "=>" + inheritedPath,
                   worldTransform, {}, chain, {}, true, visiting);
    }
  }

  visiting.erase(path);
}

bool hasRenderableData(const Json &root) {
  if (!root.is_object() || !root.contains("data") ||
      !root.at("data").is_array()) {
    return false;
  }
  for (const Json &item : root.at("data")) {
    if (!item.is_object() || !item.contains("attributes") ||
        !item.at("attributes").is_object()) {
      continue;
    }
    const Json &attributes = item.at("attributes");
    if (attributes.contains("usd::usdgeom::mesh") ||
        attributes.contains("usd::usdgeom::basiscurves") ||
        attributes.contains("points::array") ||
        attributes.contains("points::base64") ||
        attributes.contains("pcd::base64")) {
      return true;
    }
  }
  return false;
}

bool hasUnresolvedGraphReferences(const Json &root) {
  if (!root.is_object() || !root.contains("data") ||
      !root.at("data").is_array()) {
    return false;
  }

  const NodeGraph graph = buildGraph(root.at("data"));
  for (const std::string &childPath : graph.directChildren) {
    if (!graph.nodes.contains(childPath)) {
      return true;
    }
  }
  for (const std::string &inheritedPath : graph.inheritedReferences) {
    if (!graph.nodes.contains(inheritedPath)) {
      return true;
    }
  }
  return false;
}

void normalizeRootMaterialTexturePaths(Json &root,
                                       const std::filesystem::path &sourceDir) {
  if (!root.is_object() || !root.contains("data") ||
      !root.at("data").is_array()) {
    return;
  }
  for (Json &item : root.at("data")) {
    if (!item.is_object() || !item.contains("attributes") ||
        !item.at("attributes").is_object()) {
      continue;
    }
    Json &attributes = item.at("attributes");
    if (attributes.contains("gltf::material")) {
      normalizeGltfMaterialTexturePaths(attributes.at("gltf::material"),
                                        sourceDir);
    }
  }
}

void appendRootData(Json &data, const Json &root) {
  if (!root.is_object() || !root.contains("data") ||
      !root.at("data").is_array()) {
    return;
  }
  for (const Json &item : root.at("data")) {
    data.push_back(item);
  }
}

Json readJsonFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open IFCX file: " +
                             container::util::pathToUtf8(path));
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return Json::parse(text.begin(), text.end());
}

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

std::optional<std::filesystem::path>
resolveLocalImport(const std::filesystem::path &sourceDir,
                   std::string uriText) {
  if (uriText.empty()) {
    return std::nullopt;
  }
  const std::string lower = lowerAscii(uriText);
  if (startsWith(lower, "http://") || startsWith(lower, "https://")) {
    return std::nullopt;
  }
  if (startsWith(lower, "file://")) {
    uriText = uriText.substr(7u);
    if (uriText.size() >= 3u && uriText[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(uriText[1])) &&
        uriText[2] == ':') {
      uriText.erase(uriText.begin());
    }
  }
  if (const size_t fragment = uriText.find_first_of("?#");
      fragment != std::string::npos) {
    uriText.erase(fragment);
  }

  std::filesystem::path path = container::util::pathFromUtf8(uriText);
  if (lowerAscii(path.extension().string()) != ".ifcx") {
    return std::nullopt;
  }
  if (path.is_relative()) {
    path = sourceDir / path;
  }
  std::error_code existsError;
  if (!std::filesystem::exists(path, existsError)) {
    return std::nullopt;
  }
  return path.lexically_normal();
}

std::vector<std::filesystem::path>
localImportPaths(const Json &root, const std::filesystem::path &sourceDir) {
  std::vector<std::filesystem::path> paths;
  if (!root.is_object() || !root.contains("imports") ||
      !root.at("imports").is_array()) {
    return paths;
  }
  for (const Json &import : root.at("imports")) {
    if (!import.is_object() || !import.contains("uri") ||
        !import.at("uri").is_string()) {
      continue;
    }
    if (auto path = resolveLocalImport(sourceDir,
                                       import.at("uri").get<std::string>())) {
      paths.push_back(*path);
    }
  }
  return paths;
}

std::optional<std::filesystem::path>
heuristicBaseLayerPath(const std::filesystem::path &path) {
  const std::filesystem::path dir = path.parent_path();
  const std::string stem = path.stem().string();
  const std::string lowerStem = lowerAscii(stem);
  if (const size_t add = lowerStem.find("-add-");
      add != std::string::npos && add > 0u) {
    std::filesystem::path candidate = dir / (stem.substr(0, add) + ".ifcx");
    std::error_code existsError;
    if (std::filesystem::exists(candidate, existsError)) {
      return candidate.lexically_normal();
    }
  }

  if ((startsWith(lowerStem, "add-") ||
       lowerAscii(dir.filename().string()) == "advanced") &&
      dir.has_parent_path()) {
    const std::filesystem::path parent = dir.parent_path();
    std::error_code iterError;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(parent, iterError)) {
      if (iterError) {
        break;
      }
      if (!entry.is_regular_file() ||
          lowerAscii(entry.path().extension().string()) != ".ifcx") {
        continue;
      }
      const std::string candidateStem = entry.path().stem().string();
      if (candidateStem.find("-add-") == std::string::npos &&
          !startsWith(candidateStem, "add-")) {
        return entry.path().lexically_normal();
      }
    }
  }
  return std::nullopt;
}

Json composeRootFromFile(const std::filesystem::path &path,
                         std::unordered_set<std::string> &visiting) {
  const std::filesystem::path normalized = path.lexically_normal();
  const std::string key = container::util::pathToUtf8(normalized);
  if (!visiting.insert(key).second) {
    return Json{{"data", Json::array()}};
  }

  const Json root = readJsonFile(normalized);
  Json localRoot = root;
  normalizeRootMaterialTexturePaths(localRoot, normalized.parent_path());
  Json composed{{"data", Json::array()}};
  const std::vector<std::filesystem::path> imports =
      localImportPaths(root, normalized.parent_path());
  for (const std::filesystem::path &importPath : imports) {
    appendRootData(composed["data"], composeRootFromFile(importPath, visiting));
  }
  if (imports.empty() &&
      (!hasRenderableData(root) || hasUnresolvedGraphReferences(root))) {
    if (auto baseLayer = heuristicBaseLayerPath(normalized)) {
      appendRootData(composed["data"],
                     composeRootFromFile(*baseLayer, visiting));
    }
  }
  appendRootData(composed["data"], localRoot);

  visiting.erase(key);
  return composed;
}

Model loadFromRoot(const Json &root, float importScale,
                   const std::filesystem::path *sourceDir) {
  const Json &data = requiredArray(root, "data", "file");
  const NodeGraph graph = buildGraph(data);

  SceneBuilder builder{
      Model{},
      graph,
      sourceDir,
      {},
      {},
      {},
      0u,
      container::geometry::zUpForwardYToRendererAxes() *
           glm::scale(glm::mat4(1.0f),
                     glm::vec3(sanitizeImportScale(importScale)))};
  builder.model.unitMetadata = makeUnitMetadata(importScale);
  builder.model.georeferenceMetadata =
      readGeoreferenceMetadata(root, graph);

  std::vector<std::string> roots;
  roots.reserve(graph.nodes.size());
  for (const auto &[path, node] : graph.nodes) {
    (void)node;
    if (!graph.directChildren.contains(path) &&
        !graph.inheritedReferences.contains(path)) {
      roots.push_back(path);
    }
  }
  if (roots.empty()) {
    for (const auto &[path, node] : graph.nodes) {
      if (node.attributes.contains("usd::usdgeom::mesh") ||
          node.attributes.contains("usd::usdgeom::basiscurves") ||
          node.attributes.contains("points::array") ||
          node.attributes.contains("points::base64") ||
          node.attributes.contains("pcd::base64")) {
        roots.push_back(path);
      }
    }
  }
  std::ranges::sort(roots);

  std::unordered_set<std::string> visiting;
  for (const std::string &rootPath : roots) {
    traverseNode(builder, rootPath, rootPath, glm::mat4(1.0f), {}, {}, {},
                 false, visiting);
  }
  return std::move(builder.model);
}

} // namespace

Model LoadFromJson(std::string_view jsonText, float importScale) {
  const Json root = Json::parse(jsonText.begin(), jsonText.end());
  return loadFromRoot(root, importScale, nullptr);
}

Model LoadFromFile(const std::filesystem::path &path, float importScale) {
  std::unordered_set<std::string> visiting;
  const Json root = composeRootFromFile(path, visiting);
  const std::filesystem::path sourceDir = path.parent_path();
  return loadFromRoot(root, importScale, &sourceDir);
}

} // namespace container::geometry::ifcx
