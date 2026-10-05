#include "Container/geometry/IfcTessellatedLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/geometry/PolylineGeometry.h"
#include "Container/geometry/SweptDiskGeometry.h"
#include "Container/utility/Platform.h"
#include "IfcBrepFaceGeometry.h"
#include "IfcCurveReader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <manifold/manifold.h>
#include <mapbox/earcut.hpp>

namespace container::geometry::ifc {
namespace {

using detail::Entity;
using detail::StepValue;

struct MeshGroup {
  uint32_t meshId{0};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  dotbim::GeometryKind geometryKind{dotbim::GeometryKind::Mesh};
};

struct BoxSolid {
  glm::vec3 minBounds{0.0f};
  glm::vec3 maxBounds{0.0f};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
};

struct GeometryInstance {
  uint32_t geometryId{0};
  glm::mat4 transform{1.0f};
  std::optional<glm::vec4> curveColor{};
};

struct StoreyMetadata {
  std::string id{};
  std::string name{};
};

struct SemanticMaterialMetadata {
  std::string name{};
  std::string category{};

  [[nodiscard]] bool empty() const noexcept {
    return name.empty() && category.empty();
  }
};

struct ProductMetadata {
  std::string guid{};
  std::string displayName{};
  std::string objectType{};
  std::string storeyName{};
  std::string storeyId{};
  std::string materialName{};
  std::string materialCategory{};
  std::string discipline{};
  std::string phase{};
  std::string fireRating{};
  std::string loadBearing{};
  std::string status{};
  std::string sourceId{};
  std::vector<container::geometry::dotbim::ElementProperty> properties{};
};

struct LengthUnitMetadata {
  bool authored{false};
  float metersPerUnit{1.0f};
  std::string sourceUnits{};
};

std::string upperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

std::string semanticPropertyKey(std::string_view value) {
  std::string key;
  key.reserve(value.size());
  for (char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      key.push_back(
          static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return key;
}

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

std::string ifcSiLengthUnitLabel(const std::optional<std::string> &prefix) {
  if (!prefix.has_value()) {
    return "metre";
  }
  if (*prefix == "MILLI") {
    return "millimetre";
  }
  if (*prefix == "CENTI") {
    return "centimetre";
  }
  if (*prefix == "DECI") {
    return "decimetre";
  }
  if (*prefix == "KILO") {
    return "kilometre";
  }
  return "metre";
}

container::geometry::dotbim::ModelUnitMetadata
makeUnitMetadata(const LengthUnitMetadata &lengthUnit, float importScale) {
  const float sanitizedImportScale = sanitizeImportScale(importScale);
  container::geometry::dotbim::ModelUnitMetadata metadata{};
  if (lengthUnit.authored) {
    metadata.hasSourceUnits = true;
    metadata.sourceUnits = lengthUnit.sourceUnits;
    metadata.hasMetersPerUnit = true;
    metadata.metersPerUnit = lengthUnit.metersPerUnit;
  }
  metadata.hasImportScale = true;
  metadata.importScale = sanitizedImportScale;
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale =
      sanitizedImportScale *
      (lengthUnit.authored ? lengthUnit.metersPerUnit : 1.0f);
  return metadata;
}

class ValueParser {
public:
  explicit ValueParser(std::string_view text) : text_(text) {}

  StepValue parseArguments() {
    StepValue result{};
    result.kind = StepValue::Kind::List;

    while (true) {
      skipWhitespace();
      if (atEnd()) {
        break;
      }

      result.list.push_back(parseValue());
      skipWhitespace();
      if (atEnd()) {
        break;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      throw std::runtime_error("unexpected IFC argument token near offset " +
                               std::to_string(pos_));
    }

    return result;
  }

private:
  [[nodiscard]] bool atEnd() const { return pos_ >= text_.size(); }

  void skipWhitespace() {
    while (!atEnd() &&
           std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
  }

  StepValue parseValue() {
    skipWhitespace();
    if (atEnd()) {
      return {};
    }

    const char c = text_[pos_];
    if (c == '(') {
      return parseList();
    }
    if (c == '\'') {
      return parseString();
    }
    if (c == '#') {
      return parseRef();
    }
    if (c == '.') {
      return parseEnum();
    }
    if (c == '$' || c == '*') {
      ++pos_;
      return {};
    }
    return parseTokenOrTypedValue();
  }

  StepValue parseList() {
    StepValue result{};
    result.kind = StepValue::Kind::List;
    ++pos_;

    while (true) {
      skipWhitespace();
      if (atEnd()) {
        throw std::runtime_error("unterminated IFC list value");
      }
      if (text_[pos_] == ')') {
        ++pos_;
        break;
      }

      result.list.push_back(parseValue());
      skipWhitespace();
      if (atEnd()) {
        throw std::runtime_error("unterminated IFC list value");
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ')') {
        ++pos_;
        break;
      }
      throw std::runtime_error("unexpected IFC list token near offset " +
                               std::to_string(pos_));
    }

    return result;
  }

  StepValue parseString() {
    StepValue result{};
    result.kind = StepValue::Kind::String;
    ++pos_;

    while (!atEnd()) {
      const char c = text_[pos_++];
      if (c == '\'') {
        if (!atEnd() && text_[pos_] == '\'') {
          result.text.push_back('\'');
          ++pos_;
          continue;
        }
        return result;
      }
      result.text.push_back(c);
    }

    throw std::runtime_error("unterminated IFC string value");
  }

  StepValue parseRef() {
    StepValue result{};
    result.kind = StepValue::Kind::Ref;
    ++pos_;
    const size_t start = pos_;
    while (!atEnd() &&
           std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
    if (start == pos_) {
      throw std::runtime_error("IFC reference is missing an id");
    }
    const std::string digits(text_.substr(start, pos_ - start));
    const auto parsed = std::stoull(digits);
    if (parsed > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("IFC reference id exceeds uint32 range");
    }
    result.ref = static_cast<uint32_t>(parsed);
    return result;
  }

  StepValue parseEnum() {
    StepValue result{};
    result.kind = StepValue::Kind::Enum;
    ++pos_;
    const size_t start = pos_;
    while (!atEnd() && text_[pos_] != '.') {
      ++pos_;
    }
    if (atEnd()) {
      throw std::runtime_error("unterminated IFC enum value");
    }
    result.text = upperAscii(std::string(text_.substr(start, pos_ - start)));
    ++pos_;
    return result;
  }

  StepValue parseTokenOrTypedValue() {
    const size_t start = pos_;
    while (!atEnd()) {
      const char c = text_[pos_];
      if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',' ||
          c == '(' || c == ')') {
        break;
      }
      ++pos_;
    }

    if (start == pos_) {
      throw std::runtime_error("unexpected IFC value token near offset " +
                               std::to_string(pos_));
    }

    const std::string token(text_.substr(start, pos_ - start));
    skipWhitespace();
    if (!atEnd() && text_[pos_] == '(') {
      auto result = parseList();
      result.text = upperAscii(token);
      return result;
    }

    char *end = nullptr;
    const double number = std::strtod(token.c_str(), &end);
    if (end != token.c_str() && end != nullptr && *end == '\0' &&
        std::isfinite(number)) {
      StepValue result{};
      result.kind = StepValue::Kind::Number;
      result.number = number;
      return result;
    }

    StepValue result{};
    result.kind = StepValue::Kind::Text;
    result.text = upperAscii(token);
    return result;
  }

  std::string_view text_{};
  size_t pos_{0};
};

bool isIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

size_t findMatchingParen(std::string_view text, size_t openPos) {
  size_t depth = 0;
  bool inString = false;
  for (size_t i = openPos; i < text.size(); ++i) {
    const char c = text[i];
    if (inString) {
      if (c == '\'') {
        if (i + 1u < text.size() && text[i + 1u] == '\'') {
          ++i;
        } else {
          inString = false;
        }
      }
      continue;
    }

    if (c == '\'') {
      inString = true;
      continue;
    }
    if (c == '(') {
      ++depth;
      continue;
    }
    if (c == ')') {
      if (depth == 0u) {
        throw std::runtime_error("IFC parser encountered unmatched ')'");
      }
      --depth;
      if (depth == 0u) {
        return i;
      }
    }
  }
  throw std::runtime_error("IFC entity has unterminated argument list");
}

std::unordered_map<uint32_t, Entity> parseEntities(std::string_view text) {
  std::unordered_map<uint32_t, Entity> entities;

  size_t pos = 0;
  while (pos < text.size()) {
    const size_t hash = text.find('#', pos);
    if (hash == std::string_view::npos) {
      break;
    }

    size_t cursor = hash + 1u;
    if (cursor >= text.size() ||
        std::isdigit(static_cast<unsigned char>(text[cursor])) == 0) {
      pos = cursor;
      continue;
    }

    const size_t idStart = cursor;
    while (cursor < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    const size_t idEnd = cursor;

    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '=') {
      pos = cursor;
      continue;
    }
    ++cursor;
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }

    const size_t typeStart = cursor;
    while (cursor < text.size() && isIdentChar(text[cursor])) {
      ++cursor;
    }
    if (typeStart == cursor) {
      pos = cursor;
      continue;
    }

    const std::string type =
        upperAscii(std::string(text.substr(typeStart, cursor - typeStart)));
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '(') {
      pos = cursor;
      continue;
    }

    const size_t close = findMatchingParen(text, cursor);
    const std::string digits(text.substr(idStart, idEnd - idStart));
    const auto parsedId = std::stoull(digits);
    if (parsedId > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("IFC entity id exceeds uint32 range");
    }

    const std::string_view argsText =
        text.substr(cursor + 1u, close - cursor - 1u);
    Entity entity{};
    entity.id = static_cast<uint32_t>(parsedId);
    entity.type = type;
    entity.args = ValueParser(argsText).parseArguments();
    entities[entity.id] = std::move(entity);
    pos = close + 1u;
  }

  return entities;
}

const StepValue *argAt(const Entity &entity, size_t index) {
  if (entity.args.kind != StepValue::Kind::List ||
      index >= entity.args.list.size()) {
    return nullptr;
  }
  return &entity.args.list[index];
}

std::span<const StepValue> asList(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::List) {
    return {};
  }
  return value->list;
}

std::optional<uint32_t> refValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::Ref) {
    return std::nullopt;
  }
  return value->ref;
}

std::optional<double> numberValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::Number) {
    return std::nullopt;
  }
  return value->number;
}

std::optional<std::string> stringValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::String) {
    return std::nullopt;
  }
  return value->text;
}

std::optional<std::string> scalarLabelValue(const StepValue *value) {
  if (value == nullptr) {
    return std::nullopt;
  }
  switch (value->kind) {
  case StepValue::Kind::String:
    return value->text;
  case StepValue::Kind::Enum:
  case StepValue::Kind::Text:
    if (value->text == "T") {
      return std::string("true");
    }
    if (value->text == "F") {
      return std::string("false");
    }
    return value->text;
  case StepValue::Kind::Number:
    if (std::isfinite(value->number)) {
      return std::to_string(value->number);
    }
    return std::nullopt;
  case StepValue::Kind::List:
    for (const StepValue &item : value->list) {
      if (auto nested = scalarLabelValue(&item); nested && !nested->empty()) {
        return nested;
      }
    }
    return std::nullopt;
  case StepValue::Kind::Omitted:
  case StepValue::Kind::Ref:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> enumValue(const StepValue *value) {
  if (value == nullptr || value->kind != StepValue::Kind::Enum) {
    return std::nullopt;
  }
  return value->text;
}

std::vector<uint32_t> refList(const StepValue *value) {
  std::vector<uint32_t> refs;
  for (const StepValue &item : asList(value)) {
    if (item.kind == StepValue::Kind::Ref) {
      refs.push_back(item.ref);
    }
  }
  return refs;
}

std::optional<uint32_t> firstRef(const Entity &entity, size_t index) {
  return refValue(argAt(entity, index));
}

class IfcModelBuilder {
public:
  IfcModelBuilder(std::unordered_map<uint32_t, Entity> entities,
                  float importScale)
      : entities_(std::move(entities)),
        importScale_(sanitizeImportScale(importScale)) {
    sortedEntityIds_.reserve(entities_.size());
    for (const auto &[id, _] : entities_) {
      sortedEntityIds_.push_back(id);
    }
    std::ranges::sort(sortedEntityIds_);
    unitMetadata_ = detectLengthUnitMetadata();
    unitScale_ = unitMetadata_.authored ? unitMetadata_.metersPerUnit : 1.0f;
    model_.unitMetadata = makeUnitMetadata(unitMetadata_, importScale_);
    pointCache_.reserve(entities_.size() / 2u);
    directionCache_.reserve(entities_.size() / 8u);
    axisPlacementCache_.reserve(entities_.size() / 8u);
    localPlacementCache_.reserve(entities_.size() / 8u);
    groupsByItem_.reserve(entities_.size() / 8u);
    boxSolidsByItem_.reserve(entities_.size() / 16u);
    openingsByHostProduct_.reserve(entities_.size() / 32u);
    hostByOpeningProduct_.reserve(entities_.size() / 32u);
    storeyByProduct_.reserve(entities_.size() / 32u);
    materialByProduct_.reserve(entities_.size() / 32u);
    semanticPropertiesByProduct_.reserve(entities_.size() / 32u);
  }

  Model build() {
    cacheStyleColors();
    cacheVoidRelations();
    cacheFillRelations();
    cacheSpatialContainmentRelations();
    cacheMaterialRelations();
    cachePropertyRelations();
    cacheClassificationRelations();
    cacheGroupRelations();
    cacheHierarchyRelations();
    cacheTypeRelations();
    appendTessellatedGeometry();
    appendSweptSolidGeometry();
    appendProductElements();
    appendFallbackElements();
    finishImportReport();
    compactDrawableGeometry();
    return std::move(model_);
  }

private:
  const Entity *entity(uint32_t id) const {
    const auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : &it->second;
  }

  glm::vec3 readDirection(uint32_t ref, glm::vec3 fallback) const {
    if (const auto cached = directionCache_.find(ref);
        cached != directionCache_.end()) {
      return cached->second;
    }

    const Entity *direction = entity(ref);
    if (direction == nullptr || direction->type != "IFCDIRECTION") {
      return fallback;
    }
    const auto coords = readNumberList(argAt(*direction, 0));
    if (coords.size() < 2u) {
      return fallback;
    }
    glm::vec3 value(static_cast<float>(coords[0]),
                    static_cast<float>(coords[1]),
                    coords.size() > 2u ? static_cast<float>(coords[2]) : 0.0f);
    if (glm::dot(value, value) <= 1.0e-12f) {
      return fallback;
    }
    value = glm::normalize(value);
    directionCache_[ref] = value;
    return value;
  }

  glm::vec3 readPoint(uint32_t ref, glm::vec3 fallback) const {
    if (const auto cached = pointCache_.find(ref);
        cached != pointCache_.end()) {
      return cached->second;
    }

    const Entity *point = entity(ref);
    if (point == nullptr || point->type != "IFCCARTESIANPOINT") {
      return fallback;
    }
    const auto coords = readNumberList(argAt(*point, 0));
    if (coords.size() < 2u) {
      return fallback;
    }
    const glm::vec3 value{
        static_cast<float>(coords[0]), static_cast<float>(coords[1]),
        coords.size() > 2u ? static_cast<float>(coords[2]) : 0.0f};
    pointCache_[ref] = value;
    return value;
  }

  bool appendIndexedArc(std::vector<glm::vec3> &selected, const glm::vec3 &a,
                        const glm::vec3 &b, const glm::vec3 &c) const {
    const auto ab = glm::dvec3(b) - glm::dvec3(a),
               ac = glm::dvec3(c) - glm::dvec3(a), n = glm::cross(ab, ac);
    const double aa = glm::dot(ab, ab), cc = glm::dot(ac, ac),
                 nn = glm::dot(n, n);
    // IFC specifies a polyline fallback for collinear three-point arcs.
    if (nn <= 1e-20 * aa * cc) {
      selected.insert(selected.end(), {b, c});
      return true;
    }
    const auto center =
        glm::dvec3(a) +
        (cc * glm::cross(n, ab) + aa * glm::cross(ac, n)) / (2 * nn);
    const double radius = glm::length(glm::dvec3(a) - center);
    const auto count = circleSegmentCount(radius);
    if (!count)
      return false;
    const auto u = (glm::dvec3(a) - center) / radius,
               v = glm::normalize(glm::cross(n, u));
    const double period = 2 * std::acos(-1.0);
    const auto angle = [&](const glm::vec3 &p) {
      const auto d = glm::dvec3(p) - center;
      const double value = std::atan2(glm::dot(d, v), glm::dot(d, u));
      return value < 0 ? value + period : value;
    };
    const double middle = angle(b), end = angle(c);
    if (!std::isfinite(middle) || !std::isfinite(end) || middle <= 0 ||
        end <= middle)
      return false;
    double start = 0;
    for (const auto &[last, endpoint] :
         std::array<std::pair<double, glm::vec3>, 2>{{{middle, b}, {end, c}}}) {
      const size_t segments = std::max<size_t>(
          1, static_cast<size_t>(std::ceil(*count * (last - start) / period)));
      if (selected.size() + segments > 65536)
        return false;
      for (size_t i = 1; i <= segments; ++i) {
        const double parameter = start + (last - start) * double(i) / segments;
        selected.push_back(
            i == segments
                ? endpoint
                : glm::vec3(center + radius * (u * std::cos(parameter) +
                                               v * std::sin(parameter))));
      }
      start = last;
    }
    return true;
  }

  std::vector<glm::vec3> readPolylinePoints(uint32_t ref,
                                            bool renderCurve = false) const {
    const Entity *polyline = entity(ref);
    if (polyline == nullptr) {
      return {};
    }

    std::vector<glm::vec3> points;
    if (polyline->type == "IFCINDEXEDPOLYCURVE") {
      // Omitted Segments means consecutive straight segments. Preserve typed
      // indices to distinguish straight segments from unsupported arc segments.
      const auto *segments = argAt(*polyline, 1);
      if (!segments)
        return {};
      const auto pointsRef = firstRef(*polyline, 0);
      const Entity *pointList = pointsRef ? entity(*pointsRef) : nullptr;
      if (!pointList || (pointList->type != "IFCCARTESIANPOINTLIST2D" &&
                         pointList->type != "IFCCARTESIANPOINTLIST3D"))
        return {};
      const size_t dimension =
          pointList->type == "IFCCARTESIANPOINTLIST2D" ? 2u : 3u;
      for (const auto &tuple : asList(argAt(*pointList, 0))) {
        const auto coords = readNumberList(&tuple);
        if (coords.size() != dimension || asList(&tuple).size() != dimension)
          return {};
        points.emplace_back(
            static_cast<float>(coords[0]), static_cast<float>(coords[1]),
            dimension == 3u ? static_cast<float>(coords[2]) : 0.0f);
      }
      if (renderCurve && points.size() > 65536)
        return {};
      if (segments->kind != StepValue::Kind::Omitted) {
        if (segments->kind != StepValue::Kind::List || segments->list.empty())
          return {};
        if (renderCurve && segments->list.size() > 65536)
          return {};
        std::vector<glm::vec3> selected;
        uint32_t lastIndex = 0;
        for (const auto &segment : segments->list) {
          const bool arc = renderCurve && segment.text == "IFCARCINDEX";
          if ((!arc && segment.text != "IFCLINEINDEX") ||
              segment.list.size() != 1u)
            return {};
          const auto indices = readPositiveIndexList(&segment.list.front());
          if (indices.size() < 2u || (arc && indices.size() != 3u) ||
              indices.size() != asList(&segment.list.front()).size())
            return {};
          if (lastIndex != 0 && indices.front() != lastIndex)
            return {};
          for (auto index : indices)
            if (index > points.size())
              return {};
          if (arc) {
            if (lastIndex == 0)
              selected.push_back(points[indices.front() - 1]);
            if (!appendIndexedArc(selected, points[indices[0] - 1],
                                  points[indices[1] - 1],
                                  points[indices[2] - 1]))
              return {};
            if (selected.size() > 65536)
              return {};
            lastIndex = indices.back();
            continue;
          }
          for (size_t i = lastIndex == 0 ? 0u : 1u; i < indices.size(); ++i) {
            if (indices[i] > points.size())
              return {};
            selected.push_back(points[indices[i] - 1u]);
          }
          lastIndex = indices.back();
          if (renderCurve && selected.size() > 65536)
            return {};
        }
        points = std::move(selected);
      }
    } else if (polyline->type == "IFCPOLYLINE") {
      const auto pointRefs = refList(argAt(*polyline, 0));
      if (renderCurve && pointRefs.size() > 65536)
        return {};
      if (pointRefs.size() != asList(argAt(*polyline, 0)).size())
        return {};
      for (const auto pointRef : pointRefs) {
        const Entity *point = entity(pointRef);
        if (!point || point->type != "IFCCARTESIANPOINT")
          return {};
        const auto coords = readNumberList(argAt(*point, 0));
        if (coords.size() < 2u || coords.size() > 3u ||
            coords.size() != asList(argAt(*point, 0)).size())
          return {};
        points.push_back(readPoint(pointRef, glm::vec3(0.0f)));
      }
    } else
      return {};
    if (!renderCurve && points.size() > 2u &&
        glm::length(points.front() - points.back()) <= 1.0e-5f) {
      points.pop_back();
    }
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
  }

  std::vector<std::vector<glm::vec3>> readProfileLoops(uint32_t ref) const {
    const Entity *profile = entity(ref);
    if (profile == nullptr) {
      return {};
    }
    if (profile->type == "IFCRECTANGLEPROFILEDEF" ||
        profile->type == "IFCCIRCLEPROFILEDEF" ||
        profile->type == "IFCCIRCLEHOLLOWPROFILEDEF" ||
        profile->type == "IFCLSHAPEPROFILEDEF" ||
        profile->type == "IFCUSHAPEPROFILEDEF" ||
        profile->type == "IFCISHAPEPROFILEDEF") {
      std::vector<std::vector<glm::vec3>> loops;
      if (profile->type == "IFCRECTANGLEPROFILEDEF") {
        const auto width = numberValue(argAt(*profile, 3));
        const auto height = numberValue(argAt(*profile, 4));
        if (!width || !height || *width <= 0 || *height <= 0 ||
            !std::isfinite(*width) || !std::isfinite(*height))
          return {};
        const float x = static_cast<float>(*width * .5);
        const float y = static_cast<float>(*height * .5);
        loops = {{{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}}};
      } else if (profile->type == "IFCCIRCLEPROFILEDEF" ||
                 profile->type == "IFCCIRCLEHOLLOWPROFILEDEF") {
        const auto radius = numberValue(argAt(*profile, 3));
        if (!radius || !std::isfinite(*radius) || *radius <= 0)
          return {};
        const auto segments = circleSegmentCount(*radius);
        if (!segments)
          return {};
        loops.push_back(circleLoop(*radius, *segments));
        if (profile->type == "IFCCIRCLEHOLLOWPROFILEDEF") {
          const auto thickness = numberValue(argAt(*profile, 4));
          if (!thickness || !std::isfinite(*thickness) || *thickness <= 0 ||
              *thickness >= *radius)
            return {};
          loops.push_back(circleLoop(*radius - *thickness, *segments));
        }
      } else {
        auto loop = readStructuralProfile(*profile);
        if (loop.empty())
          return {};
        loops.push_back(std::move(loop));
      }
      glm::vec3 origin(0), xAxis(1, 0, 0), yAxis(0, 1, 0);
      if (const auto positionRef = firstRef(*profile, 2)) {
        const auto *position = entity(*positionRef);
        if (!position || position->type != "IFCAXIS2PLACEMENT2D")
          return {};
        if (const auto originRef = firstRef(*position, 0))
          origin = readPoint(*originRef, glm::vec3(0));
        if (const auto directionRef = firstRef(*position, 1))
          xAxis = readDirection(*directionRef, xAxis);
        if (std::abs(origin.z) > 1e-7f || std::abs(xAxis.z) > 1e-7f)
          return {};
        yAxis = {-xAxis.y, xAxis.x, 0};
      }
      for (auto &loop : loops)
        for (auto &point : loop)
          point = origin + xAxis * point.x + yAxis * point.y;
      return loops;
    }
    if (profile->type == "IFCARBITRARYCLOSEDPROFILEDEF" ||
        profile->type == "IFCARBITRARYPROFILEDEFWITHVOIDS") {
      const auto curveRef = firstRef(*profile, 2);
      if (!curveRef)
        return {};
      std::vector<std::vector<glm::vec3>> loops{readProfileCurve(*curveRef)};
      if (profile->type == "IFCARBITRARYPROFILEDEFWITHVOIDS") {
        const auto innerRefs = refList(argAt(*profile, 3));
        if (innerRefs.empty() ||
            innerRefs.size() != asList(argAt(*profile, 3)).size())
          return {};
        for (uint32_t inner : innerRefs)
          loops.push_back(readProfileCurve(inner));
      }
      return loops;
    }
    return {};
  }

  std::optional<size_t> circleSegmentCount(double radius) const {
    // Bound chord sagitta by both 1 mm in authored units and 0.5% of radius.
    // Reject excessive tessellation instead of silently exceeding the budget.
    if (!std::isfinite(radius) || radius <= 0 || unitScale_ <= 0)
      return std::nullopt;
    const double tolerance = std::min(radius * .005, .001 / unitScale_);
    const double angle = std::acos(1.0 - tolerance / radius);
    const double count = std::ceil(std::acos(-1.0) / angle / 4.0) * 4.0;
    if (!std::isfinite(count) || count > 4096)
      return std::nullopt;
    return std::max(size_t(16), static_cast<size_t>(count));
  }

  static std::vector<glm::vec3> circleLoop(double radius, size_t segments) {
    std::vector<glm::vec3> points;
    points.reserve(segments);
    for (size_t i = 0; i < segments; ++i) {
      const double angle = 2.0 * std::acos(-1.0) * double(i) / double(segments);
      points.emplace_back(static_cast<float>(radius * std::cos(angle)),
                          static_cast<float>(radius * std::sin(angle)), 0);
    }
    return points;
  }

  std::vector<glm::vec3> readStructuralProfile(const Entity &profile) const {
    const bool angle = profile.type == "IFCLSHAPEPROFILEDEF";
    const bool channel = profile.type == "IFCUSHAPEPROFILEDEF";
    const auto dimension = [&](size_t index,
                               std::optional<double> fallback = std::nullopt) {
      const auto *value = argAt(profile, index);
      return !value || value->kind == StepValue::Kind::Omitted
                 ? fallback
                 : numberValue(value);
    };
    const auto depth = dimension(angle || channel ? 3 : 4);
    const auto width =
        dimension(angle || channel ? 4 : 3, angle ? depth : std::nullopt);
    const auto web = dimension(5), flange = angle ? web : dimension(6);
    const auto fillet = dimension(angle ? 6 : 7, 0);
    const auto edge = dimension(angle ? 7 : 8, 0);
    const auto slope = dimension(angle ? 8 : 9, 0);
    for (const auto &value : {depth, width, web, flange, fillet, edge, slope})
      if (!value || !std::isfinite(*value))
        return {};
    if (*depth <= 0 || *width <= 0 || *web <= 0 || *flange <= 0 ||
        *fillet < 0 || *edge < 0 || *web >= *width ||
        (angle ? *flange >= *depth : *flange * 2 >= *depth))
      return {};
    const double w = *width / 2, h = *depth / 2, tw = *web, tf = *flange;
    const auto angleScale = projectPlaneAngleScale();
    if (*slope != 0 && !angleScale)
      return {};
    const double radians = *slope * angleScale.value_or(1);
    if (!std::isfinite(radians) || std::abs(radians) >= std::acos(-1.0) / 2)
      return {};
    const double taper = std::tan(radians);
    std::vector<glm::dvec2> corners;
    std::vector<double> radii;
    if (angle) {
      // Leg thickness is measured where the inner faces meet the profile
      // coordinate axes. Intersect y = -h + tf - x*taper and
      // x = -w + tw - y*taper, rather than offsetting their common corner.
      const double denominator = 1 - taper * taper;
      if (std::abs(denominator) <= 1e-10)
        return {};
      const double a = -w + tw, b = -h + tf;
      const glm::dvec2 junction((a - taper * b) / denominator,
                                (b - taper * a) / denominator);
      const double horizontalTip = b - w * taper;
      const double verticalTip = a - h * taper;
      if (horizontalTip <= -h || horizontalTip >= h || verticalTip <= -w ||
          verticalTip >= w || junction.x <= -w || junction.x >= w ||
          junction.y <= -h || junction.y >= h)
        return {};
      corners = {{-w, -h}, {w, -h},          {w, horizontalTip},
                 junction, {verticalTip, h}, {-w, h}};
      radii = {0, 0, *edge, *fillet, *edge, 0};
    } else if (channel) {
      // Channel flange thickness is measured at x = 0.
      const double tip = tf - w * taper, root = tf + (w - tw) * taper;
      if (tip <= 0 || tip >= h || root <= 0 || root >= h)
        return {};
      corners = {{-w, -h},
                 {w, -h},
                 {w, -h + tip},
                 {-w + tw, -h + root},
                 {-w + tw, h - root},
                 {w, h - tip},
                 {w, h},
                 {-w, h}};
      radii = {0, 0, *edge, *fillet, *fillet, *edge, 0, 0};
    } else {
      // Symmetric I profiles specify flange thickness at the free edges.
      const double root = tf + (w - tw / 2) * taper;
      if (root <= 0 || root >= h)
        return {};
      corners = {{-w, -h},
                 {w, -h},
                 {w, -h + tf},
                 {tw / 2, -h + root},
                 {tw / 2, h - root},
                 {w, h - tf},
                 {w, h},
                 {-w, h},
                 {-w, h - tf},
                 {-tw / 2, h - root},
                 {-tw / 2, -h + root},
                 {-w, -h + tf}};
      radii = {0, 0, *edge, *fillet, *fillet, *edge,
               0, 0, *edge, *fillet, *fillet, *edge};
    }
    std::vector<double> tangentDistance(corners.size(), 0);
    for (size_t i = 0; i < corners.size(); ++i) {
      if (radii[i] == 0)
        continue;
      const auto toPrevious = glm::normalize(
          corners[(i + corners.size() - 1) % corners.size()] - corners[i]);
      const auto toNext =
          glm::normalize(corners[(i + 1) % corners.size()] - corners[i]);
      const double halfAngle =
          std::acos(std::clamp(glm::dot(toPrevious, toNext), -1.0, 1.0)) / 2;
      tangentDistance[i] = radii[i] / std::tan(halfAngle);
      if (!std::isfinite(tangentDistance[i]))
        return {};
    }
    for (size_t i = 0; i < corners.size(); ++i)
      if (tangentDistance[i] + tangentDistance[(i + 1) % corners.size()] >=
          glm::length(corners[(i + 1) % corners.size()] - corners[i]))
        return {};
    std::vector<glm::vec3> loop;
    for (size_t i = 0; i < corners.size(); ++i) {
      const auto &corner = corners[i];
      if (radii[i] == 0) {
        loop.emplace_back(corner.x, corner.y, 0);
        continue;
      }
      const auto incoming = glm::normalize(
          corners[(i + corners.size() - 1) % corners.size()] - corner);
      const auto outgoing =
          glm::normalize(corners[(i + 1) % corners.size()] - corner);
      const auto a = corner + incoming * tangentDistance[i];
      const auto b = corner + outgoing * tangentDistance[i];
      const auto center =
          corner +
          glm::normalize(incoming + outgoing) *
              (radii[i] / std::sqrt((1 - glm::dot(incoming, outgoing)) / 2));
      const auto va = a - center, vb = b - center;
      const double start = std::atan2(va.y, va.x);
      const double sweep =
          std::atan2(va.x * vb.y - va.y * vb.x, glm::dot(va, vb));
      const auto circleSegments = circleSegmentCount(radii[i]);
      if (!circleSegments)
        return {};
      const size_t segments = std::max(
          size_t(1), size_t(std::ceil(*circleSegments * std::abs(sweep) /
                                      (2 * std::acos(-1.0)))));
      for (size_t j = 0; j <= segments; ++j) {
        // Use the computed tangent endpoints exactly to retain shared topology.
        const auto point =
            j == 0 ? a
            : j == segments
                ? b
                : center + glm::dvec2(
                               std::cos(start + sweep * double(j) / segments),
                               std::sin(start + sweep * double(j) / segments)) *
                               radii[i];
        loop.emplace_back(point.x, point.y, 0);
      }
    }
    return loop;
  }

  std::vector<glm::vec3> readPointList3D(uint32_t ref) const {
    const Entity *pointList = entity(ref);
    if (pointList == nullptr || pointList->type != "IFCCARTESIANPOINTLIST3D") {
      return {};
    }

    std::vector<glm::vec3> points;
    for (const StepValue &tuple : asList(argAt(*pointList, 0))) {
      const auto coords = readNumberList(&tuple);
      // Never shift subsequent one-based indices by dropping an invalid row.
      if (coords.size() != 3u || asList(&tuple).size() != 3u)
        return {};
      points.emplace_back(static_cast<float>(coords[0]),
                          static_cast<float>(coords[1]),
                          static_cast<float>(coords[2]));
    }
    return points;
  }

  static std::vector<double> readNumberList(const StepValue *value) {
    std::vector<double> numbers;
    for (const StepValue &item : asList(value)) {
      if (item.kind == StepValue::Kind::Number && std::isfinite(item.number)) {
        numbers.push_back(item.number);
      }
    }
    return numbers;
  }

  static std::vector<uint32_t> readPositiveIndexList(const StepValue *value) {
    std::vector<uint32_t> indices;
    for (const StepValue &item : asList(value)) {
      if (item.kind != StepValue::Kind::Number || !std::isfinite(item.number) ||
          item.number < 1.0 ||
          item.number >
              static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        continue;
      }
      const double rounded = std::round(item.number);
      if (std::abs(item.number - rounded) <= 1.0e-6) {
        indices.push_back(static_cast<uint32_t>(rounded));
      }
    }
    return indices;
  }

  struct TriangleVertices {
    std::array<glm::vec3, 3> positions{};
    std::optional<std::array<glm::vec3, 3>> normals;
  };
  using AxisIndex = glm::vec3::length_type;

  static glm::vec3 safeNormal(const glm::vec3 &a, const glm::vec3 &b,
                              const glm::vec3 &c) {
    const glm::dvec3 normal = glm::cross(glm::dvec3(b) - glm::dvec3(a),
                                         glm::dvec3(c) - glm::dvec3(a));
    const double len2 = glm::dot(normal, normal);
    if (!std::isfinite(len2) || len2 <= 0) {
      return {0.0f, 1.0f, 0.0f};
    }
    return glm::vec3(normal / std::sqrt(len2));
  }

  static glm::vec3 safeTangent(const glm::vec3 &a, const glm::vec3 &b,
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

  static Vertex makeVertex(const glm::vec3 &position, const glm::vec3 &normal,
                           const glm::vec3 &tangent) {
    Vertex vertex{};
    vertex.position = position;
    vertex.normal = normal;
    vertex.tangent = glm::vec4(tangent, 1.0f);
    return vertex;
  }

  static uint32_t packColor(glm::vec4 color) {
    auto pack = [](float component) {
      return static_cast<uint32_t>(
          std::clamp(std::lround(component * 255.0f), 0l, 255l));
    };
    return pack(color.r) | (pack(color.g) << 8u) | (pack(color.b) << 16u) |
           (pack(color.a) << 24u);
  }

  static glm::vec4 defaultColor() { return {0.8f, 0.82f, 0.86f, 1.0f}; }

  static glm::vec4 sanitizeColor(glm::vec4 color) {
    color.r = std::clamp(color.r, 0.0f, 1.0f);
    color.g = std::clamp(color.g, 0.0f, 1.0f);
    color.b = std::clamp(color.b, 0.0f, 1.0f);
    color.a = std::clamp(color.a, 0.0f, 1.0f);
    return color;
  }

  LengthUnitMetadata detectLengthUnitMetadata() const {
    for (const auto id : sortedEntityIds_) {
      const Entity *unit = entity(id);
      if (unit == nullptr || unit->type != "IFCSIUNIT") {
        continue;
      }
      const auto unitType = enumValue(argAt(*unit, 1));
      const auto unitName = enumValue(argAt(*unit, 3));
      if (!unitType.has_value() || *unitType != "LENGTHUNIT" ||
          !unitName.has_value() || *unitName != "METRE") {
        continue;
      }
      const auto prefix = enumValue(argAt(*unit, 2));
      LengthUnitMetadata metadata{};
      metadata.authored = true;
      metadata.sourceUnits = ifcSiLengthUnitLabel(prefix);
      if (!prefix.has_value()) {
        metadata.metersPerUnit = 1.0f;
        return metadata;
      }
      if (*prefix == "MILLI") {
        metadata.metersPerUnit = 0.001f;
        return metadata;
      }
      if (*prefix == "CENTI") {
        metadata.metersPerUnit = 0.01f;
        return metadata;
      }
      if (*prefix == "DECI") {
        metadata.metersPerUnit = 0.1f;
        return metadata;
      }
      if (*prefix == "KILO") {
        metadata.metersPerUnit = 1000.0f;
        return metadata;
      }
      metadata.metersPerUnit = 1.0f;
      return metadata;
    }
    return {};
  }

  glm::mat4 importUnitTransform() const {
    return container::geometry::zUpForwardYToRendererAxes() *
           glm::scale(glm::mat4(1.0f), glm::vec3(importScale_ * unitScale_));
  }

  static glm::mat4 basisTransform(const glm::vec3 &location,
                                  const glm::vec3 &xAxisHint,
                                  const glm::vec3 &zAxisHint) {
    glm::vec3 zAxis = zAxisHint;
    if (glm::dot(zAxis, zAxis) <= 1.0e-12f) {
      zAxis = {0.0f, 0.0f, 1.0f};
    }
    zAxis = glm::normalize(zAxis);

    glm::vec3 xAxis = xAxisHint;
    if (glm::dot(xAxis, xAxis) <= 1.0e-12f ||
        std::abs(glm::dot(glm::normalize(xAxis), zAxis)) > 0.999f) {
      xAxis = std::abs(zAxis.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                       : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    xAxis = glm::normalize(xAxis - zAxis * glm::dot(zAxis, xAxis));
    glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
    xAxis = glm::normalize(glm::cross(yAxis, zAxis));

    glm::mat4 result(1.0f);
    result[0] = glm::vec4(xAxis, 0.0f);
    result[1] = glm::vec4(yAxis, 0.0f);
    result[2] = glm::vec4(zAxis, 0.0f);
    result[3] = glm::vec4(location, 1.0f);
    return result;
  }

  glm::mat4 axis2Placement3D(uint32_t ref) const {
    if (const auto cached = axisPlacementCache_.find(ref);
        cached != axisPlacementCache_.end()) {
      return cached->second;
    }

    const Entity *placement = entity(ref);
    if (placement == nullptr || placement->type != "IFCAXIS2PLACEMENT3D") {
      return glm::mat4(1.0f);
    }

    glm::vec3 location(0.0f);
    if (const auto pointRef = firstRef(*placement, 0); pointRef.has_value()) {
      location = readPoint(*pointRef, glm::vec3(0.0f));
    }
    glm::vec3 zAxis(0.0f, 0.0f, 1.0f);
    if (const auto directionRef = firstRef(*placement, 1);
        directionRef.has_value()) {
      zAxis = readDirection(*directionRef, {0.0f, 0.0f, 1.0f});
    }
    glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
    if (const auto directionRef = firstRef(*placement, 2);
        directionRef.has_value()) {
      xAxis = readDirection(*directionRef, {1.0f, 0.0f, 0.0f});
    }
    const glm::mat4 result = basisTransform(location, xAxis, zAxis);
    axisPlacementCache_[ref] = result;
    return result;
  }

  glm::mat4 localPlacement(uint32_t ref) const {
    if (const auto cached = localPlacementCache_.find(ref);
        cached != localPlacementCache_.end()) {
      return cached->second;
    }

    std::unordered_set<uint32_t> visiting;
    return localPlacementRecursive(ref, visiting);
  }

  glm::mat4
  localPlacementRecursive(uint32_t ref,
                          std::unordered_set<uint32_t> &visiting) const {
    if (const auto cached = localPlacementCache_.find(ref);
        cached != localPlacementCache_.end()) {
      return cached->second;
    }
    if (!visiting.insert(ref).second) {
      return glm::mat4(1.0f);
    }

    const Entity *placement = entity(ref);
    if (placement == nullptr || placement->type != "IFCLOCALPLACEMENT") {
      visiting.erase(ref);
      return glm::mat4(1.0f);
    }

    glm::mat4 parent(1.0f);
    if (const auto parentRef = firstRef(*placement, 0); parentRef.has_value()) {
      parent = localPlacementRecursive(*parentRef, visiting);
    }
    glm::mat4 relative(1.0f);
    if (const auto relativeRef = firstRef(*placement, 1);
        relativeRef.has_value()) {
      relative = axis2Placement3D(*relativeRef);
    }
    const glm::mat4 result = parent * relative;
    visiting.erase(ref);
    localPlacementCache_[ref] = result;
    return result;
  }

  glm::mat4 cartesianTransformationOperator3D(uint32_t ref) const {
    const Entity *op = entity(ref);
    if (op == nullptr ||
        (op->type != "IFCCARTESIANTRANSFORMATIONOPERATOR3D" &&
         op->type != "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM")) {
      return glm::mat4(1.0f);
    }

    glm::vec3 xAxis(1.0f, 0.0f, 0.0f);
    if (const auto directionRef = firstRef(*op, 0); directionRef.has_value()) {
      xAxis = readDirection(*directionRef, {1.0f, 0.0f, 0.0f});
    }
    glm::vec3 yAxis(0.0f, 1.0f, 0.0f);
    if (const auto directionRef = firstRef(*op, 1); directionRef.has_value()) {
      yAxis = readDirection(*directionRef, {0.0f, 1.0f, 0.0f});
    }
    glm::vec3 origin(0.0f);
    if (const auto pointRef = firstRef(*op, 2); pointRef.has_value()) {
      origin = readPoint(*pointRef, glm::vec3(0.0f));
    }
    const float scale1 =
        static_cast<float>(numberValue(argAt(*op, 3)).value_or(1.0));
    glm::vec3 zHint = glm::cross(xAxis, yAxis);
    if (glm::dot(zHint, zHint) <= 1.0e-12f) {
      zHint = {0.0f, 0.0f, 1.0f};
    }
    glm::vec3 zAxis = glm::normalize(zHint);
    if (const auto directionRef = firstRef(*op, 4); directionRef.has_value()) {
      zAxis = readDirection(*directionRef, zHint);
    }
    const float scale2 =
        op->type == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM"
            ? static_cast<float>(numberValue(argAt(*op, 5)).value_or(scale1))
            : scale1;
    const float scale3 =
        op->type == "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM"
            ? static_cast<float>(numberValue(argAt(*op, 6)).value_or(scale1))
            : scale1;

    glm::mat4 result(1.0f);
    result[0] = glm::vec4(glm::normalize(xAxis) * scale1, 0.0f);
    result[1] = glm::vec4(glm::normalize(yAxis) * scale2, 0.0f);
    result[2] = glm::vec4(glm::normalize(zAxis) * scale3, 0.0f);
    result[3] = glm::vec4(origin, 1.0f);
    return result;
  }

  std::optional<glm::vec4> colorFromRef(uint32_t ref) const {
    return colorFromRef(ref, {});
  }

  std::optional<glm::vec4>
  colorFromRef(uint32_t ref, std::unordered_set<uint32_t> visiting) const {
    if (!visiting.insert(ref).second) {
      return std::nullopt;
    }

    const Entity *colorEntity = entity(ref);
    if (colorEntity == nullptr) {
      return std::nullopt;
    }

    if (colorEntity->type == "IFCCOLOURRGB") {
      const auto r = numberValue(argAt(*colorEntity, 1));
      const auto g = numberValue(argAt(*colorEntity, 2));
      const auto b = numberValue(argAt(*colorEntity, 3));
      if (r.has_value() && g.has_value() && b.has_value()) {
        return sanitizeColor({static_cast<float>(*r), static_cast<float>(*g),
                              static_cast<float>(*b), 1.0f});
      }
      return std::nullopt;
    }

    if (colorEntity->type == "IFCSURFACESTYLERENDERING" ||
        colorEntity->type == "IFCSURFACESTYLESHADING") {
      const auto baseColorRef = firstRef(*colorEntity, 0);
      if (!baseColorRef.has_value()) {
        return std::nullopt;
      }
      auto color = colorFromRef(*baseColorRef, visiting);
      if (!color.has_value()) {
        return std::nullopt;
      }
      const float transparency =
          static_cast<float>(numberValue(argAt(*colorEntity, 1)).value_or(0.0));
      color->a = std::clamp(1.0f - transparency, 0.0f, 1.0f);
      return sanitizeColor(*color);
    }

    if (colorEntity->type == "IFCSURFACESTYLE") {
      for (const auto styleRef : refList(argAt(*colorEntity, 2))) {
        if (auto color = colorFromRef(styleRef, visiting); color.has_value()) {
          return color;
        }
      }
      return std::nullopt;
    }

    if (colorEntity->type == "IFCCURVESTYLE") {
      if (const auto colorRef = firstRef(*colorEntity, 3))
        return colorFromRef(*colorRef, visiting);
      return std::nullopt;
    }

    if (colorEntity->type == "IFCPRESENTATIONSTYLEASSIGNMENT") {
      for (const auto styleRef : refList(argAt(*colorEntity, 0))) {
        if (auto color = colorFromRef(styleRef, visiting); color.has_value()) {
          return color;
        }
      }
    }

    return std::nullopt;
  }

  std::vector<glm::vec4> colorList(uint32_t ref) const {
    const Entity *list = entity(ref);
    if (list == nullptr || list->type != "IFCCOLOURRGBLIST") {
      return {};
    }
    std::vector<glm::vec4> colors;
    for (const StepValue &tuple : asList(argAt(*list, 0))) {
      const auto values = readNumberList(&tuple);
      if (values.size() < 3u) {
        continue;
      }
      colors.push_back(sanitizeColor({static_cast<float>(values[0]),
                                      static_cast<float>(values[1]),
                                      static_cast<float>(values[2]), 1.0f}));
    }
    return colors;
  }

  void cacheStyleColors() {
    for (const auto id : sortedEntityIds_) {
      const Entity *styledItem = entity(id);
      if (styledItem == nullptr || styledItem->type != "IFCSTYLEDITEM") {
        continue;
      }
      const auto itemRef = firstRef(*styledItem, 0);
      if (!itemRef.has_value()) {
        continue;
      }
      for (const auto styleRef : refList(argAt(*styledItem, 1))) {
        if (auto color = colorFromRef(styleRef); color.has_value()) {
          styleColorByItem_[*itemRef] = *color;
          break;
        }
      }
    }

    for (const auto id : sortedEntityIds_) {
      const Entity *colorMap = entity(id);
      if (colorMap == nullptr || colorMap->type != "IFCINDEXEDCOLOURMAP") {
        continue;
      }
      const auto mappedTo = firstRef(*colorMap, 0);
      const auto colorListRef = firstRef(*colorMap, 2);
      if (!mappedTo.has_value() || !colorListRef.has_value()) {
        continue;
      }
      const auto colors = colorList(*colorListRef);
      if (colors.empty()) {
        continue;
      }

      std::vector<glm::vec4> faceColors;
      for (const StepValue &colorIndex : asList(argAt(*colorMap, 3))) {
        if (colorIndex.kind != StepValue::Kind::Number ||
            colorIndex.number < 1.0) {
          continue;
        }
        const auto index =
            static_cast<size_t>(std::lround(colorIndex.number)) - 1u;
        if (index < colors.size()) {
          faceColors.push_back(colors[index]);
        }
      }
      if (!faceColors.empty()) {
        faceColorsByFaceSet_[*mappedTo] = std::move(faceColors);
      }
    }
  }

  void cacheVoidRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELVOIDSELEMENT") {
        continue;
      }

      const auto hostRef = firstRef(*relation, 4);
      const auto openingRef = firstRef(*relation, 5);
      if (!hostRef.has_value() || !openingRef.has_value()) {
        continue;
      }
      openingsByHostProduct_[*hostRef].push_back(*openingRef);
      hostByOpeningProduct_[*openingRef] = *hostRef;
      appendRelationshipReference(*hostRef, "IFCRELVOIDSELEMENT", "Opening",
                                  *openingRef);
    }
  }

  void cacheFillRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELFILLSELEMENT") {
        continue;
      }

      const auto openingRef = firstRef(*relation, 4);
      const auto fillerRef = firstRef(*relation, 5);
      if (!openingRef.has_value() || !fillerRef.has_value()) {
        continue;
      }

      appendRelationshipReference(*fillerRef, "IFCRELFILLSELEMENT", "Opening",
                                  *openingRef);
      appendRelationshipReference(*openingRef, "IFCRELFILLSELEMENT", "FilledBy",
                                  *fillerRef);
      if (const auto hostIt = hostByOpeningProduct_.find(*openingRef);
          hostIt != hostByOpeningProduct_.end()) {
        appendRelationshipReference(*fillerRef, "IFCRELFILLSELEMENT",
                                    "VoidsElement", hostIt->second);
        appendRelationshipReference(hostIt->second, "IFCRELFILLSELEMENT",
                                    "FilledBy", *fillerRef);
      }
    }
  }

  void cacheSpatialContainmentRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELCONTAINEDINSPATIALSTRUCTURE") {
        continue;
      }

      const auto structureRef = firstRef(*relation, 5);
      if (!structureRef.has_value()) {
        continue;
      }
      const Entity *structure = entity(*structureRef);
      if (structure == nullptr || (structure->type != "IFCBUILDINGSTOREY" &&
                                   structure->type != "IFCSPACE")) {
        continue;
      }

      StoreyMetadata storey{};
      storey.id = stringValue(argAt(*structure, 0)).value_or("");
      if (storey.id.empty()) {
        storey.id = "#" + std::to_string(structure->id);
      }
      storey.name = stringValue(argAt(*structure, 2)).value_or("");
      for (const auto productRef : refList(argAt(*relation, 4))) {
        if (structure->type == "IFCBUILDINGSTOREY") {
          storeyByProduct_[productRef] = storey;
        }
        ProductMetadata &metadata = semanticPropertiesByProduct_[productRef];
        appendProperty(
            metadata,
            makeProperty(
                "IFCRELCONTAINEDINSPATIALSTRUCTURE",
                structure->type == "IFCSPACE" ? "Space" : "BuildingStorey",
                storey.name.empty() ? storey.id : storey.name, "reference"));
      }
    }
  }

  SemanticMaterialMetadata materialMetadata(uint32_t ref) const {
    std::unordered_set<uint32_t> visiting;
    return materialMetadata(ref, visiting);
  }

  SemanticMaterialMetadata
  materialMetadata(uint32_t ref, std::unordered_set<uint32_t> &visiting) const {
    if (!visiting.insert(ref).second) {
      return {};
    }

    const Entity *source = entity(ref);
    if (source == nullptr) {
      return {};
    }

    if (source->type == "IFCMATERIAL") {
      return SemanticMaterialMetadata{
          .name = stringValue(argAt(*source, 0)).value_or(""),
          .category = stringValue(argAt(*source, 2)).value_or(""),
      };
    }

    auto fromRef = [&](size_t index) {
      if (const auto nestedRef = firstRef(*source, index);
          nestedRef.has_value()) {
        return materialMetadata(*nestedRef, visiting);
      }
      return SemanticMaterialMetadata{};
    };

    if (source->type == "IFCMATERIALLAYER") {
      SemanticMaterialMetadata material = fromRef(0);
      if (material.name.empty()) {
        material.name = stringValue(argAt(*source, 3)).value_or("");
      }
      if (material.category.empty()) {
        material.category = stringValue(argAt(*source, 5)).value_or("");
      }
      return material;
    }

    if (source->type == "IFCMATERIALLAYERSET") {
      for (const auto layerRef : refList(argAt(*source, 0))) {
        SemanticMaterialMetadata material =
            materialMetadata(layerRef, visiting);
        if (!material.empty()) {
          if (material.category.empty()) {
            material.category = stringValue(argAt(*source, 1)).value_or("");
          }
          return material;
        }
      }
      return SemanticMaterialMetadata{
          .name = stringValue(argAt(*source, 1)).value_or(""),
      };
    }

    if (source->type == "IFCMATERIALLAYERSETUSAGE") {
      return fromRef(0);
    }

    if (source->type == "IFCMATERIALPROFILE") {
      SemanticMaterialMetadata material = fromRef(2);
      if (material.name.empty()) {
        material.name = stringValue(argAt(*source, 0)).value_or("");
      }
      if (material.category.empty()) {
        material.category = stringValue(argAt(*source, 5)).value_or("");
      }
      return material;
    }

    if (source->type == "IFCMATERIALCONSTITUENT") {
      SemanticMaterialMetadata material = fromRef(2);
      if (material.name.empty()) {
        material.name = stringValue(argAt(*source, 0)).value_or("");
      }
      if (material.category.empty()) {
        material.category = stringValue(argAt(*source, 4)).value_or("");
      }
      return material;
    }

    if (source->type == "IFCMATERIALPROFILESET" ||
        source->type == "IFCMATERIALCONSTITUENTSET" ||
        source->type == "IFCMATERIALLIST") {
      for (size_t i = 0; i < source->args.list.size(); ++i) {
        for (const auto nestedRef : refList(argAt(*source, i))) {
          SemanticMaterialMetadata material =
              materialMetadata(nestedRef, visiting);
          if (!material.empty()) {
            return material;
          }
        }
        SemanticMaterialMetadata material = fromRef(i);
        if (!material.empty()) {
          return material;
        }
      }
    }

    return {};
  }

  void cacheMaterialRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELASSOCIATESMATERIAL") {
        continue;
      }

      const auto materialRef = firstRef(*relation, 5);
      if (!materialRef.has_value()) {
        continue;
      }
      const SemanticMaterialMetadata material = materialMetadata(*materialRef);
      ProductMetadata materialProperties =
          materialBrowsingMetadata(*materialRef);
      const std::string materialValue =
          material.name.empty() ? materialLabel(*materialRef) : material.name;
      appendProperty(materialProperties,
                     makeProperty("IFCRELASSOCIATESMATERIAL", "Material",
                                  materialValue, "relationship"));
      appendProperty(materialProperties,
                     makeProperty("IFCRELASSOCIATESMATERIAL", "MaterialId",
                                  fallbackRefValue(*materialRef),
                                  "relationship"));
      for (const auto productRef : refList(argAt(*relation, 4))) {
        if (!material.empty()) {
          materialByProduct_[productRef] = material;
        }
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, materialProperties);
      }
    }
  }

  static void
  appendProperty(ProductMetadata &metadata,
                 container::geometry::dotbim::ElementProperty property) {
    if (property.name.empty() || property.value.empty()) {
      return;
    }

    const auto duplicate = std::ranges::find_if(
        metadata.properties,
        [&](const container::geometry::dotbim::ElementProperty &existing) {
          return existing.set == property.set &&
                 existing.name == property.name &&
                 existing.category == property.category;
        });
    if (duplicate == metadata.properties.end()) {
      metadata.properties.push_back(std::move(property));
    }
  }

  static std::string joinedScalarValues(const StepValue *value) {
    if (value == nullptr) {
      return {};
    }
    if (value->kind != StepValue::Kind::List) {
      return scalarLabelValue(value).value_or("");
    }

    std::string result;
    for (const StepValue &item : value->list) {
      const std::string itemValue = joinedScalarValues(&item);
      if (itemValue.empty()) {
        continue;
      }
      if (!result.empty()) {
        result += ", ";
      }
      result += itemValue;
    }
    return result;
  }

  static container::geometry::dotbim::ElementProperty
  makeProperty(std::string set, std::string name, std::string value,
               std::string category) {
    container::geometry::dotbim::ElementProperty property{};
    property.set = std::move(set);
    property.name = std::move(name);
    property.value = std::move(value);
    property.category = std::move(category);
    return property;
  }

  static std::string fallbackRefValue(uint32_t ref) {
    return "#" + std::to_string(ref);
  }

  std::string entityNameOrId(uint32_t ref) const {
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return fallbackRefValue(ref);
    }
    std::string value = stringValue(argAt(*source, 2)).value_or("");
    if (value.empty()) {
      value = stringValue(argAt(*source, 0)).value_or("");
    }
    return value.empty() ? fallbackRefValue(ref) : value;
  }

  void appendRelationshipReference(ProductMetadata &metadata,
                                   std::string_view set, std::string name,
                                   uint32_t relatedRef) const {
    appendProperty(metadata,
                   makeProperty(std::string(set), name,
                                entityNameOrId(relatedRef), "relationship"));
    appendProperty(metadata,
                   makeProperty(std::string(set), name + "Id",
                                fallbackRefValue(relatedRef), "relationship"));
  }

  void appendRelationshipReference(uint32_t productRef, std::string_view set,
                                   std::string name, uint32_t relatedRef) {
    appendRelationshipReference(semanticPropertiesByProduct_[productRef], set,
                                std::move(name), relatedRef);
  }

  static void appendMaterialProperty(ProductMetadata &metadata, std::string set,
                                     std::string name, std::string value) {
    appendProperty(metadata, makeProperty(std::move(set), std::move(name),
                                          std::move(value), "material"));
  }

  static std::vector<uint32_t> refsAt(const Entity &source, size_t index) {
    std::vector<uint32_t> refs = refList(argAt(source, index));
    if (refs.empty()) {
      if (const auto ref = refValue(argAt(source, index)); ref.has_value()) {
        refs.push_back(*ref);
      }
    }
    return refs;
  }

  std::string materialLabel(uint32_t ref) const {
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return fallbackRefValue(ref);
    }

    if (source->type == "IFCMATERIAL") {
      return stringValue(argAt(*source, 0)).value_or(fallbackRefValue(ref));
    }
    if (source->type == "IFCMATERIALLAYER") {
      std::string value = stringValue(argAt(*source, 3)).value_or("");
      if (!value.empty()) {
        return value;
      }
    }
    if (source->type == "IFCMATERIALLAYERSET") {
      return stringValue(argAt(*source, 1)).value_or(fallbackRefValue(ref));
    }
    if (source->type == "IFCMATERIALLAYERSETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        return materialLabel(*setRef);
      }
    }
    if (source->type == "IFCMATERIALPROFILE" ||
        source->type == "IFCMATERIALCONSTITUENT") {
      std::string value = stringValue(argAt(*source, 0)).value_or("");
      if (!value.empty()) {
        return value;
      }
    }
    if (source->type == "IFCMATERIALPROFILESET" ||
        source->type == "IFCMATERIALCONSTITUENTSET") {
      return stringValue(argAt(*source, 0)).value_or(fallbackRefValue(ref));
    }
    if (source->type == "IFCMATERIALPROFILESETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        return materialLabel(*setRef);
      }
    }

    const SemanticMaterialMetadata material = materialMetadata(ref);
    if (!material.name.empty()) {
      return material.name;
    }
    return entityNameOrId(ref);
  }

  ProductMetadata materialBrowsingMetadata(uint32_t ref) const {
    std::unordered_set<uint32_t> visiting;
    return materialBrowsingMetadata(ref, visiting);
  }

  ProductMetadata
  materialBrowsingMetadata(uint32_t ref,
                           std::unordered_set<uint32_t> &visiting) const {
    ProductMetadata metadata{};
    if (!visiting.insert(ref).second) {
      return metadata;
    }

    const Entity *source = entity(ref);
    if (source == nullptr) {
      return metadata;
    }

    auto mergeNested = [&](uint32_t nestedRef) {
      mergeSemanticProperties(metadata,
                              materialBrowsingMetadata(nestedRef, visiting));
    };

    if (source->type == "IFCMATERIAL") {
      appendMaterialProperty(metadata, "IFCMATERIAL", "Name",
                             stringValue(argAt(*source, 0)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIAL", "Description",
                             stringValue(argAt(*source, 1)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIAL", "Category",
                             stringValue(argAt(*source, 2)).value_or(""));
      return metadata;
    }

    if (source->type == "IFCMATERIALLAYER") {
      const SemanticMaterialMetadata material = materialMetadata(ref);
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Layer",
                             materialLabel(ref));
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Material",
                             material.name);
      appendMaterialProperty(metadata, "IFCMATERIALLAYER", "Thickness",
                             joinedScalarValues(argAt(*source, 1)));
      appendMaterialProperty(
          metadata, "IFCMATERIALLAYER", "Category",
          stringValue(argAt(*source, 5)).value_or(material.category));
      if (const auto materialRef = firstRef(*source, 0);
          materialRef.has_value()) {
        mergeNested(*materialRef);
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALLAYERSET") {
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSET", "LayerSet",
                             materialLabel(ref));
      size_t layerIndex = 1u;
      for (const auto layerRef : refsAt(*source, 0)) {
        const Entity *layer = entity(layerRef);
        const SemanticMaterialMetadata material = materialMetadata(layerRef);
        const std::string prefix = "Layer." + std::to_string(layerIndex);
        appendMaterialProperty(metadata, "IFCMATERIALLAYERSET", prefix,
                               material.name.empty() ? materialLabel(layerRef)
                                                     : material.name);
        if (layer != nullptr) {
          appendMaterialProperty(metadata, "IFCMATERIALLAYERSET",
                                 prefix + ".Name",
                                 stringValue(argAt(*layer, 3)).value_or(""));
          appendMaterialProperty(metadata, "IFCMATERIALLAYERSET",
                                 prefix + ".Thickness",
                                 joinedScalarValues(argAt(*layer, 1)));
          appendMaterialProperty(
              metadata, "IFCMATERIALLAYERSET", prefix + ".Category",
              stringValue(argAt(*layer, 5)).value_or(material.category));
        }
        mergeNested(layerRef);
        ++layerIndex;
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALLAYERSETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE", "LayerSet",
                               materialLabel(*setRef));
        mergeNested(*setRef);
      }
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                             "LayerSetDirection",
                             enumValue(argAt(*source, 1)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                             "DirectionSense",
                             enumValue(argAt(*source, 2)).value_or(""));
      appendMaterialProperty(metadata, "IFCMATERIALLAYERSETUSAGE",
                             "OffsetFromReferenceLine",
                             joinedScalarValues(argAt(*source, 3)));
      return metadata;
    }

    if (source->type == "IFCMATERIALPROFILE") {
      const SemanticMaterialMetadata material = materialMetadata(ref);
      appendMaterialProperty(metadata, "IFCMATERIALPROFILE", "Profile",
                             materialLabel(ref));
      appendMaterialProperty(metadata, "IFCMATERIALPROFILE", "Material",
                             material.name);
      appendMaterialProperty(
          metadata, "IFCMATERIALPROFILE", "Category",
          stringValue(argAt(*source, 5)).value_or(material.category));
      if (const auto profileRef = firstRef(*source, 3);
          profileRef.has_value()) {
        appendMaterialProperty(metadata, "IFCMATERIALPROFILE",
                               "ProfileDefinition",
                               entityNameOrId(*profileRef));
      }
      if (const auto materialRef = firstRef(*source, 2);
          materialRef.has_value()) {
        mergeNested(*materialRef);
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALPROFILESET") {
      appendMaterialProperty(metadata, "IFCMATERIALPROFILESET", "ProfileSet",
                             materialLabel(ref));
      size_t profileIndex = 1u;
      for (const auto profileRef : refsAt(*source, 2)) {
        const SemanticMaterialMetadata material = materialMetadata(profileRef);
        const std::string prefix = "Profile." + std::to_string(profileIndex);
        appendMaterialProperty(metadata, "IFCMATERIALPROFILESET", prefix,
                               materialLabel(profileRef));
        appendMaterialProperty(metadata, "IFCMATERIALPROFILESET",
                               prefix + ".Material", material.name);
        mergeNested(profileRef);
        ++profileIndex;
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALPROFILESETUSAGE") {
      if (const auto setRef = firstRef(*source, 0); setRef.has_value()) {
        appendMaterialProperty(metadata, "IFCMATERIALPROFILESETUSAGE",
                               "ProfileSet", materialLabel(*setRef));
        mergeNested(*setRef);
      }
      appendMaterialProperty(metadata, "IFCMATERIALPROFILESETUSAGE",
                             "CardinalPoint",
                             joinedScalarValues(argAt(*source, 1)));
      appendMaterialProperty(metadata, "IFCMATERIALPROFILESETUSAGE",
                             "ReferenceExtent",
                             joinedScalarValues(argAt(*source, 2)));
      return metadata;
    }

    if (source->type == "IFCMATERIALCONSTITUENT") {
      const SemanticMaterialMetadata material = materialMetadata(ref);
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENT", "Constituent",
                             materialLabel(ref));
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENT", "Material",
                             material.name);
      appendMaterialProperty(
          metadata, "IFCMATERIALCONSTITUENT", "Category",
          stringValue(argAt(*source, 4)).value_or(material.category));
      if (const auto materialRef = firstRef(*source, 2);
          materialRef.has_value()) {
        mergeNested(*materialRef);
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALCONSTITUENTSET") {
      appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENTSET",
                             "ConstituentSet", materialLabel(ref));
      size_t constituentIndex = 1u;
      for (const auto constituentRef : refsAt(*source, 2)) {
        const SemanticMaterialMetadata material =
            materialMetadata(constituentRef);
        const std::string prefix =
            "Constituent." + std::to_string(constituentIndex);
        appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENTSET", prefix,
                               materialLabel(constituentRef));
        appendMaterialProperty(metadata, "IFCMATERIALCONSTITUENTSET",
                               prefix + ".Material", material.name);
        mergeNested(constituentRef);
        ++constituentIndex;
      }
      return metadata;
    }

    if (source->type == "IFCMATERIALLIST") {
      size_t materialIndex = 1u;
      for (const auto nestedRef : refsAt(*source, 0)) {
        appendMaterialProperty(metadata, "IFCMATERIALLIST",
                               "Material." + std::to_string(materialIndex),
                               materialLabel(nestedRef));
        mergeNested(nestedRef);
        ++materialIndex;
      }
    }
    return metadata;
  }

  static void assignSemanticProperty(ProductMetadata &metadata,
                                     std::string_view name, std::string value) {
    if (value.empty()) {
      return;
    }
    const std::string key = semanticPropertyKey(name);
    if ((key == "DISCIPLINE" || key == "IFCDISCIPLINE") &&
        metadata.discipline.empty()) {
      metadata.discipline = std::move(value);
    } else if ((key == "PHASE" || key == "PHASENAME" ||
                key == "CONSTRUCTIONPHASE") &&
               metadata.phase.empty()) {
      metadata.phase = std::move(value);
    } else if ((key == "FIRERATING" || key == "FIRERESISTANCERATING" ||
                key == "FIRECLASSIFICATION") &&
               metadata.fireRating.empty()) {
      metadata.fireRating = std::move(value);
    } else if ((key == "LOADBEARING" || key == "ISLOADBEARING") &&
               metadata.loadBearing.empty()) {
      metadata.loadBearing = std::move(value);
    } else if ((key == "STATUS" || key == "ELEMENTSTATUS") &&
               metadata.status.empty()) {
      metadata.status = std::move(value);
    }
  }

  void mergeSemanticProperties(ProductMetadata &target,
                               const ProductMetadata &source) const {
    if (target.discipline.empty()) {
      target.discipline = source.discipline;
    }
    if (target.phase.empty()) {
      target.phase = source.phase;
    }
    if (target.fireRating.empty()) {
      target.fireRating = source.fireRating;
    }
    if (target.loadBearing.empty()) {
      target.loadBearing = source.loadBearing;
    }
    if (target.status.empty()) {
      target.status = source.status;
    }
    for (const auto &property : source.properties) {
      appendProperty(target, property);
    }
  }

  ProductMetadata propertySetMetadata(uint32_t ref) const {
    ProductMetadata metadata{};
    const Entity *propertySet = entity(ref);
    if (propertySet == nullptr) {
      return metadata;
    }

    if (propertySet->type == "IFCPROPERTYSET") {
      const std::string setName =
          stringValue(argAt(*propertySet, 2)).value_or(fallbackRefValue(ref));
      for (const auto propertyRef : refList(argAt(*propertySet, 4))) {
        mergeSemanticProperties(metadata,
                                propertyMetadata(propertyRef, setName, "pset"));
      }
    } else if (propertySet->type == "IFCELEMENTQUANTITY") {
      const std::string setName =
          stringValue(argAt(*propertySet, 2)).value_or("BaseQuantities");
      for (const auto quantityRef : refList(argAt(*propertySet, 5))) {
        mergeSemanticProperties(metadata,
                                quantityMetadata(quantityRef, setName));
      }
    } else if (propertySet->type == "IFCCOMPLEXPROPERTY") {
      mergeSemanticProperties(metadata, propertyMetadata(ref, {}, "pset"));
    } else {
      mergeSemanticProperties(metadata, propertyMetadata(ref, {}, "pset"));
    }
    return metadata;
  }

  ProductMetadata propertyMetadata(uint32_t ref, std::string_view setName,
                                   std::string_view category) const {
    ProductMetadata metadata{};
    const Entity *property = entity(ref);
    if (property == nullptr) {
      return metadata;
    }

    if (property->type == "IFCPROPERTYSINGLEVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      const std::string value = joinedScalarValues(argAt(*property, 2));
      assignSemanticProperty(metadata, name, value);
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYENUMERATEDVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      const std::string value = joinedScalarValues(argAt(*property, 2));
      assignSemanticProperty(metadata, name, value);
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYLISTVALUE" ||
               property->type == "IFCPROPERTYTABLEVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      const std::string value = joinedScalarValues(argAt(*property, 2));
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYREFERENCEVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      std::string value;
      if (const auto reference = refValue(argAt(*property, 3));
          reference.has_value()) {
        value = entityNameOrId(*reference);
      } else {
        value = joinedScalarValues(argAt(*property, 3));
      }
      appendProperty(metadata, makeProperty(std::string(setName), name, value,
                                            std::string(category)));
    } else if (property->type == "IFCPROPERTYBOUNDEDVALUE") {
      const std::string name = stringValue(argAt(*property, 0)).value_or("");
      appendProperty(metadata,
                     makeProperty(std::string(setName), name + ".UpperBound",
                                  joinedScalarValues(argAt(*property, 2)),
                                  std::string(category)));
      appendProperty(metadata,
                     makeProperty(std::string(setName), name + ".LowerBound",
                                  joinedScalarValues(argAt(*property, 3)),
                                  std::string(category)));
    } else if (property->type == "IFCCOMPLEXPROPERTY") {
      const std::string complexName =
          stringValue(argAt(*property, 0)).value_or(fallbackRefValue(ref));
      std::string nestedSet = std::string(setName);
      if (nestedSet.empty()) {
        nestedSet = complexName;
      } else if (!complexName.empty()) {
        nestedSet += "." + complexName;
      }
      for (const auto propertyRef : refList(argAt(*property, 3))) {
        mergeSemanticProperties(
            metadata, propertyMetadata(propertyRef, nestedSet, category));
      }
    }
    return metadata;
  }

  ProductMetadata quantityMetadata(uint32_t ref,
                                   std::string_view setName) const {
    ProductMetadata metadata{};
    const Entity *quantity = entity(ref);
    if (quantity == nullptr) {
      return metadata;
    }

    if (quantity->type == "IFCPHYSICALCOMPLEXQUANTITY") {
      const std::string complexName =
          stringValue(argAt(*quantity, 0)).value_or(fallbackRefValue(ref));
      std::string nestedSet = std::string(setName);
      if (!complexName.empty()) {
        nestedSet =
            nestedSet.empty() ? complexName : nestedSet + "." + complexName;
      }
      for (const auto quantityRef : refList(argAt(*quantity, 2))) {
        mergeSemanticProperties(metadata,
                                quantityMetadata(quantityRef, nestedSet));
      }
      return metadata;
    }

    if (!quantity->type.starts_with("IFCQUANTITY")) {
      return metadata;
    }

    const std::string name = stringValue(argAt(*quantity, 0)).value_or("");
    const std::string value = joinedScalarValues(argAt(*quantity, 3));
    appendProperty(metadata,
                   makeProperty(std::string(setName), name, value, "quantity"));
    return metadata;
  }

  void cachePropertyRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELDEFINESBYPROPERTIES") {
        continue;
      }

      const auto propertySetRef = firstRef(*relation, 5);
      if (!propertySetRef.has_value()) {
        continue;
      }
      const ProductMetadata properties = propertySetMetadata(*propertySetRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, properties);
      }
    }
  }

  std::string classificationSourceName(uint32_t ref) const {
    const Entity *source = entity(ref);
    if (source == nullptr) {
      return {};
    }
    if (source->type == "IFCCLASSIFICATIONREFERENCE") {
      if (const auto sourceRef = firstRef(*source, 3); sourceRef.has_value()) {
        const std::string nested = classificationSourceName(*sourceRef);
        if (!nested.empty()) {
          return nested;
        }
      }
      return stringValue(argAt(*source, 2)).value_or("");
    }
    if (source->type == "IFCCLASSIFICATION") {
      for (const size_t index : {3u, 0u, 1u, 4u}) {
        const std::string value =
            stringValue(argAt(*source, index)).value_or("");
        if (!value.empty()) {
          return value;
        }
      }
    }
    return {};
  }

  ProductMetadata classificationMetadata(uint32_t ref) const {
    ProductMetadata metadata{};
    const Entity *classification = entity(ref);
    if (classification == nullptr) {
      return metadata;
    }

    if (classification->type == "IFCCLASSIFICATIONREFERENCE") {
      std::string value = stringValue(argAt(*classification, 1)).value_or("");
      if (value.empty()) {
        value = stringValue(argAt(*classification, 0)).value_or("");
      }
      std::string name =
          stringValue(argAt(*classification, 2)).value_or("Classification");
      if (name.empty()) {
        name = "Classification";
      }
      std::string set = classificationSourceName(ref);
      if (set.empty()) {
        set = "Classification";
      }
      appendProperty(metadata,
                     makeProperty(std::move(set), std::move(name),
                                  std::move(value), "classification"));
    } else if (classification->type == "IFCCLASSIFICATION") {
      std::string value = classificationSourceName(ref);
      appendProperty(metadata,
                     makeProperty("Classification", "Classification",
                                  std::move(value), "classification"));
    }
    return metadata;
  }

  void cacheClassificationRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr ||
          relation->type != "IFCRELASSOCIATESCLASSIFICATION") {
        continue;
      }
      const auto classificationRef = firstRef(*relation, 5);
      if (!classificationRef.has_value()) {
        continue;
      }
      const ProductMetadata classification =
          classificationMetadata(*classificationRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, classification);
      }
    }
  }

  ProductMetadata groupMetadata(uint32_t ref) const {
    ProductMetadata metadata{};
    const Entity *group = entity(ref);
    if (group == nullptr) {
      return metadata;
    }

    std::string name = "Group";
    if (group->type == "IFCSYSTEM") {
      name = "System";
    } else if (group->type == "IFCZONE") {
      name = "Zone";
    } else if (group->type == "IFCSPACE") {
      name = "Space";
    }
    appendProperty(metadata, makeProperty(group->type, name,
                                          entityNameOrId(ref), "reference"));
    return metadata;
  }

  void cacheGroupRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELASSIGNSTOGROUP") {
        continue;
      }
      const auto groupRef = firstRef(*relation, 6);
      if (!groupRef.has_value()) {
        continue;
      }
      const ProductMetadata group = groupMetadata(*groupRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, group);
      }
    }
  }

  void cacheHierarchyRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || (relation->type != "IFCRELAGGREGATES" &&
                                  relation->type != "IFCRELNESTS")) {
        continue;
      }

      const auto parentRef = firstRef(*relation, 4);
      if (!parentRef.has_value()) {
        continue;
      }

      size_t childIndex = 1u;
      for (const auto childRef : refList(argAt(*relation, 5))) {
        appendRelationshipReference(childRef, relation->type, "Parent",
                                    *parentRef);
        appendRelationshipReference(*parentRef, relation->type,
                                    "Child." + std::to_string(childIndex),
                                    childRef);
        ++childIndex;
      }
    }
  }

  ProductMetadata typeObjectMetadata(uint32_t typeRef) const {
    ProductMetadata metadata{};
    const Entity *typeObject = entity(typeRef);
    if (typeObject == nullptr) {
      return metadata;
    }

    appendProperty(metadata,
                   makeProperty("IFCRELDEFINESBYTYPE", "Type",
                                entityNameOrId(typeRef), "relationship"));
    appendProperty(metadata,
                   makeProperty("IFCRELDEFINESBYTYPE", "TypeId",
                                fallbackRefValue(typeRef), "relationship"));
    appendProperty(metadata, makeProperty("IFCRELDEFINESBYTYPE", "TypeEntity",
                                          typeObject->type, "relationship"));

    for (const size_t propertySetIndex : {5u, 6u}) {
      for (const auto propertySetRef : refsAt(*typeObject, propertySetIndex)) {
        mergeSemanticProperties(metadata, propertySetMetadata(propertySetRef));
      }
    }

    if (const auto typeSemanticIt = semanticPropertiesByProduct_.find(typeRef);
        typeSemanticIt != semanticPropertiesByProduct_.end()) {
      mergeSemanticProperties(metadata, typeSemanticIt->second);
    }
    return metadata;
  }

  void cacheTypeRelations() {
    for (const auto id : sortedEntityIds_) {
      const Entity *relation = entity(id);
      if (relation == nullptr || relation->type != "IFCRELDEFINESBYTYPE") {
        continue;
      }

      const auto typeRef = firstRef(*relation, 5);
      if (!typeRef.has_value()) {
        continue;
      }

      const ProductMetadata typeMetadata = typeObjectMetadata(*typeRef);
      for (const auto productRef : refList(argAt(*relation, 4))) {
        ProductMetadata &target = semanticPropertiesByProduct_[productRef];
        mergeSemanticProperties(target, typeMetadata);
        if (!materialByProduct_.contains(productRef)) {
          if (const auto materialIt = materialByProduct_.find(*typeRef);
              materialIt != materialByProduct_.end()) {
            materialByProduct_[productRef] = materialIt->second;
          }
        }
      }
    }
  }

  ProductMetadata productMetadata(const Entity &product) const {
    ProductMetadata metadata{};
    metadata.guid = stringValue(argAt(product, 0)).value_or("");
    metadata.displayName = stringValue(argAt(product, 2)).value_or("");
    metadata.objectType = stringValue(argAt(product, 4)).value_or("");
    metadata.sourceId = "#" + std::to_string(product.id);

    if (const auto storeyIt = storeyByProduct_.find(product.id);
        storeyIt != storeyByProduct_.end()) {
      metadata.storeyId = storeyIt->second.id;
      metadata.storeyName = storeyIt->second.name;
    }
    if (const auto materialIt = materialByProduct_.find(product.id);
        materialIt != materialByProduct_.end()) {
      metadata.materialName = materialIt->second.name;
      metadata.materialCategory = materialIt->second.category;
    }
    if (const auto semanticIt = semanticPropertiesByProduct_.find(product.id);
        semanticIt != semanticPropertiesByProduct_.end()) {
      mergeSemanticProperties(metadata, semanticIt->second);
    }
    return metadata;
  }

  void applyProductMetadata(container::geometry::dotbim::Element &element,
                            const ProductMetadata &metadata) const {
    element.guid = metadata.guid;
    element.displayName = metadata.displayName;
    element.objectType = metadata.objectType;
    element.storeyName = metadata.storeyName;
    element.storeyId = metadata.storeyId;
    element.materialName = metadata.materialName;
    element.materialCategory = metadata.materialCategory;
    element.discipline = metadata.discipline;
    element.phase = metadata.phase;
    element.fireRating = metadata.fireRating;
    element.loadBearing = metadata.loadBearing;
    element.status = metadata.status;
    element.sourceId = metadata.sourceId;
    element.properties = metadata.properties;
  }

  void appendTessellatedGeometry() {
    for (const auto id : sortedEntityIds_) {
      const Entity *faceSet = entity(id);
      if (faceSet == nullptr)
        continue;
      std::vector<MeshGroup> groups;
      if (faceSet->type == "IFCTRIANGULATEDFACESET") {
        groups = appendFaceSetGeometry(*faceSet);
      } else if (faceSet->type == "IFCPOLYGONALFACESET") {
        groups = appendPolygonalFaceSetGeometry(*faceSet);
      } else if (faceSet->type == "IFCFACETEDBREP" ||
                 faceSet->type == "IFCFACETEDBREPWITHVOIDS" ||
                 faceSet->type == "IFCADVANCEDBREP" ||
                 faceSet->type == "IFCADVANCEDBREPWITHVOIDS") {
        groups = appendBrepGeometry(*faceSet);
      } else {
        continue;
      }
      if (!groups.empty()) {
        groupsByItem_[id] = std::move(groups);
      }
    }
  }

  // IFC face loops are planar and preserve authored outward winding. Project
  // onto their dominant plane for Earcut, then restore the 3D orientation.
  static std::optional<std::vector<TriangleVertices>>
  triangulatePlanarLoops(std::vector<std::vector<glm::vec3>> loops) {
    if (loops.empty() || loops.front().size() < 3u)
      return std::nullopt;
    const glm::dvec3 origin(loops.front().front());
    double extent = 0.0;
    for (auto &loop : loops) {
      if (loop.size() > 3u && glm::length(loop.front() - loop.back()) < 1e-7f)
        loop.pop_back();
      if (loop.size() < 3u)
        return std::nullopt;
      for (const auto &p : loop) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          return std::nullopt;
        extent = std::max(extent, glm::length(glm::dvec3(p) - origin));
      }
    }
    glm::dvec3 normal(0.0);
    const auto &outer = loops.front();
    for (size_t i = 0; i < outer.size(); ++i)
      normal += glm::cross(glm::dvec3(outer[i]) - origin,
                           glm::dvec3(outer[(i + 1u) % outer.size()]) - origin);
    if (glm::length(normal) <= std::max(1e-20, extent * extent * 1e-12))
      return std::nullopt;
    normal = glm::normalize(normal);
    int drop = 0;
    if (std::abs(normal.y) > std::abs(normal[drop]))
      drop = 1;
    if (std::abs(normal.z) > std::abs(normal[drop]))
      drop = 2;
    const int axisA = (drop + 1) % 3, axisB = (drop + 2) % 3;
    std::vector<std::vector<std::array<double, 2>>> polygon;
    std::vector<glm::vec3> flattened;
    double expectedArea = 0.0;
    for (size_t ring = 0; ring < loops.size(); ++ring) {
      auto &projected = polygon.emplace_back();
      for (const auto &p : loops[ring]) {
        const auto delta = glm::dvec3(p) - origin;
        if (std::abs(glm::dot(delta, normal)) > std::max(1e-7, extent * 1e-5))
          return std::nullopt;
        projected.push_back({delta[axisA], delta[axisB]});
        flattened.push_back(p);
      }
      double area = 0.0;
      for (size_t i = 0; i < projected.size(); ++i) {
        const auto &a = projected[i],
                   &b = projected[(i + 1u) % projected.size()];
        area += a[0] * b[1] - b[0] * a[1];
      }
      expectedArea += (ring == 0 ? 1.0 : -1.0) * std::abs(area) * 0.5;
    }
    if (expectedArea <= 0.0)
      return std::nullopt;
    // Earcut assumes simple, disjoint loops. Validate that contract so
    // malformed holes or self-intersections cannot produce a plausible but
    // wrong mesh.
    using Point = std::array<double, 2>;
    const double epsilon = std::max(1e-24, extent * extent * 1e-12);
    auto orientation = [](const Point &a, const Point &b, const Point &c) {
      return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    };
    auto onSegment = [&](const Point &a, const Point &b, const Point &p) {
      return std::abs(orientation(a, b, p)) <= epsilon &&
             (p[0] - a[0]) * (p[0] - b[0]) + (p[1] - a[1]) * (p[1] - b[1]) <=
                 epsilon;
    };
    auto intersects = [&](const Point &a, const Point &b, const Point &c,
                          const Point &d) {
      const double abC = orientation(a, b, c), abD = orientation(a, b, d);
      const double cdA = orientation(c, d, a), cdB = orientation(c, d, b);
      if (((abC > epsilon && abD < -epsilon) ||
           (abC < -epsilon && abD > epsilon)) &&
          ((cdA > epsilon && cdB < -epsilon) ||
           (cdA < -epsilon && cdB > epsilon)))
        return true;
      return onSegment(a, b, c) || onSegment(a, b, d) || onSegment(c, d, a) ||
             onSegment(c, d, b);
    };
    auto inside = [](const Point &p, const std::vector<Point> &ring) {
      bool result = false;
      for (size_t i = 0, j = ring.size() - 1u; i < ring.size(); j = i++) {
        const auto &a = ring[i], &b = ring[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) &&
            p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
          result = !result;
      }
      return result;
    };
    for (size_t r = 0; r < polygon.size(); ++r) {
      const auto &ring = polygon[r];
      if (r > 0 && !inside(ring.front(), polygon.front()))
        return std::nullopt;
      for (size_t s = r + 1u; s < polygon.size(); ++s)
        if (r > 0 && (inside(ring.front(), polygon[s]) ||
                      inside(polygon[s].front(), ring)))
          return std::nullopt;
      for (size_t i = 0; i < ring.size(); ++i) {
        const size_t nextI = (i + 1u) % ring.size();
        for (size_t s = r; s < polygon.size(); ++s) {
          const auto &other = polygon[s];
          for (size_t j = s == r ? i + 1u : 0; j < other.size(); ++j) {
            const size_t nextJ = (j + 1u) % other.size();
            if (s == r && (j == nextI || nextJ == i))
              continue;
            if (intersects(ring[i], ring[nextI], other[j], other[nextJ]))
              return std::nullopt;
          }
        }
      }
    }
    const auto indices = mapbox::earcut<uint32_t>(polygon);
    std::vector<TriangleVertices> triangles;
    std::vector<bool> used(flattened.size(), false);
    double triangulatedArea = 0.0;
    for (size_t i = 0; i + 2u < indices.size(); i += 3u) {
      std::array<glm::vec3, 3> p{flattened.at(indices[i]),
                                 flattened.at(indices[i + 1u]),
                                 flattened.at(indices[i + 2u])};
      const auto cross = glm::cross(glm::dvec3(p[1]) - glm::dvec3(p[0]),
                                    glm::dvec3(p[2]) - glm::dvec3(p[0]));
      if (glm::dot(cross, normal) < 0.0)
        std::swap(p[1], p[2]);
      if (glm::length(cross) <= 1e-20)
        continue;
      triangulatedArea += std::abs(cross[drop]) * 0.5;
      triangles.push_back({p});
      for (size_t j = 0; j < 3; ++j)
        used[indices[i + j]] = true;
    }
    // Reject incomplete triangulation instead of displaying filled holes or
    // silently losing faces. Area alone is not a general topology validator.
    if (triangles.empty() || std::abs(triangulatedArea - expectedArea) >
                                 std::max(1e-12, expectedArea * 1e-5))
      return std::nullopt;
    // Earcut may remove collinear boundary samples. Keep those samples on
    // output edges so both faces sharing a subdivided B-rep edge stay welded.
    size_t checks = 0;
    for (size_t vertex = 0; vertex < flattened.size(); ++vertex) {
      if (used[vertex])
        continue;
      const auto &p = flattened[vertex];
      const auto delta = glm::dvec3(p) - origin;
      const Point projected{delta[axisA], delta[axisB]};
      bool inserted = false;
      for (size_t i = 0; i < triangles.size() && !inserted; ++i) {
        if (++checks > 8000000)
          return std::nullopt;
        for (size_t j = 0; j < 3; ++j) {
          const auto a = triangles[i].positions[j];
          const auto b = triangles[i].positions[(j + 1) % 3];
          const auto c = triangles[i].positions[(j + 2) % 3];
          const auto da = glm::dvec3(a) - origin, db = glm::dvec3(b) - origin;
          if (p == a || p == b ||
              !onSegment({da[axisA], da[axisB]}, {db[axisA], db[axisB]},
                         projected))
            continue;
          triangles[i] = {{a, p, c}};
          triangles.push_back({{p, b, c}});
          inserted = true;
          break;
        }
      }
      if (!inserted)
        return std::nullopt;
    }
    return triangles;
  }

  std::vector<MeshGroup> appendPolygonalFaceSetGeometry(const Entity &faceSet) {
    const auto pointsRef = firstRef(faceSet, 0);
    if (!pointsRef)
      return {};
    const auto points = readPointList3D(*pointsRef);
    const auto pnIndex = readPositiveIndexList(argAt(faceSet, 3));
    if (points.empty())
      return {};
    const bool hasPnIndex =
        argAt(faceSet, 3) && argAt(faceSet, 3)->kind == StepValue::Kind::List;
    if (hasPnIndex && pnIndex.size() != asList(argAt(faceSet, 3)).size())
      return {};
    auto readLoop = [&](const StepValue *value) -> std::vector<glm::vec3> {
      std::vector<glm::vec3> loop;
      const auto indices = readPositiveIndexList(value);
      if (indices.size() != asList(value).size())
        return {};
      for (uint32_t index : indices) {
        if (hasPnIndex) {
          if (index == 0 || index > pnIndex.size())
            return {};
          index = pnIndex[index - 1u];
        }
        if (index == 0 || index > points.size())
          return {};
        loop.push_back(points[index - 1u]);
      }
      return loop;
    };
    struct FaceGroup {
      glm::vec4 color;
      std::vector<TriangleVertices> triangles;
    };
    std::map<uint32_t, FaceGroup> groups;
    const auto colorIt = faceColorsByFaceSet_.find(faceSet.id);
    size_t faceIndex = 0;
    const auto faces = refList(argAt(faceSet, 2));
    if (faces.size() != asList(argAt(faceSet, 2)).size())
      return {};
    for (uint32_t faceRef : faces) {
      const Entity *face = entity(faceRef);
      if (!face || (face->type != "IFCINDEXEDPOLYGONALFACE" &&
                    face->type != "IFCINDEXEDPOLYGONALFACEWITHVOIDS"))
        return {};
      std::vector<std::vector<glm::vec3>> loops{readLoop(argAt(*face, 0))};
      if (face->type == "IFCINDEXEDPOLYGONALFACEWITHVOIDS")
        for (const auto &inner : asList(argAt(*face, 1)))
          loops.push_back(readLoop(&inner));
      auto triangles = triangulatePlanarLoops(std::move(loops));
      if (!triangles)
        return {};
      const auto color = colorIt != faceColorsByFaceSet_.end() &&
                                 faceIndex < colorIt->second.size()
                             ? colorIt->second[faceIndex]
                         : styleColorByItem_.contains(faceSet.id)
                             ? styleColorByItem_.at(faceSet.id)
                             : defaultColor();
      auto &group = groups[packColor(color)];
      group.color = color;
      group.triangles.insert(group.triangles.end(), triangles->begin(),
                             triangles->end());
      ++faceIndex;
    }
    std::vector<MeshGroup> result;
    for (const auto &[_, group] : groups)
      if (auto mesh = appendTriangleMesh(group.triangles, group.color))
        result.push_back(*mesh);
    return result;
  }

  std::optional<glm::dvec3> readBrepPoint(uint32_t ref) const {
    const auto *point = entity(ref);
    if (!point || point->type != "IFCCARTESIANPOINT")
      return std::nullopt;
    const auto coordinates = readNumberList(argAt(*point, 0));
    if (coordinates.size() != 3 || asList(argAt(*point, 0)).size() != 3)
      return std::nullopt;
    return glm::dvec3(coordinates[0], coordinates[1], coordinates[2]);
  }

  struct BrepEdge {
    uint32_t start = 0, end = 0;
    std::vector<glm::vec3> points;
  };

  std::optional<BrepEdge> readBrepEdge(uint32_t ref, std::string &error) const {
    const auto *edge = entity(ref);
    if (!edge || edge->type != "IFCEDGECURVE") {
      error = "Expected IfcEdgeCurve #" + std::to_string(ref);
      return std::nullopt;
    }
    const auto start = firstRef(*edge, 0), end = firstRef(*edge, 1);
    const auto vertexPoint = [&](std::optional<uint32_t> vertexRef) {
      const auto *vertex = vertexRef ? entity(*vertexRef) : nullptr;
      const auto pointRef = vertex && vertex->type == "IFCVERTEXPOINT"
                                ? firstRef(*vertex, 0)
                                : std::nullopt;
      return pointRef ? readBrepPoint(*pointRef) : std::nullopt;
    };
    const auto a = vertexPoint(start), b = vertexPoint(end);
    const auto geometry = firstRef(*edge, 2);
    const auto sense = enumValue(argAt(*edge, 3)).value_or("");
    if (!a || !b || !geometry || (sense != "T" && sense != "F")) {
      error = "Invalid vertices, geometry or SameSense on edge #" +
              std::to_string(ref);
      return std::nullopt;
    }
    auto curve = detail::readIfcCurve(entities_, *geometry, unitScale_,
                                      projectPlaneAngleScale(), error);
    if (!curve)
      return std::nullopt;
    if (curve->dimension != 3) {
      error = "B-rep edge geometry must be a three-dimensional curve";
      return std::nullopt;
    }
    const double tolerance = 1e-7 / unitScale_;
    auto first = curveParameterAtPoint(*curve, *a, tolerance);
    auto last = curveParameterAtPoint(*curve, *b, tolerance);
    if (!first || !last) {
      error = "Edge vertices do not lie on curve #" + std::to_string(*geometry);
      return std::nullopt;
    }
    if (curve->period > 0) {
      double span =
          std::fmod((sense == "T" ? 1 : -1) * (*last - *first), curve->period);
      if (span <= 0)
        span += curve->period;
      *last = *first + (sense == "T" ? span : -span);
    } else if (*start == *end && curve->domain) {
      const auto p = curve->point(curve->domain->first);
      const auto q = curve->point(curve->domain->second);
      if (!p || !q || glm::length(*p - *a) > tolerance ||
          glm::length(*q - *a) > tolerance) {
        error = "Closed edge does not cover a closed bounded curve";
        return std::nullopt;
      }
      first = sense == "T" ? curve->domain->first : curve->domain->second;
      last = sense == "T" ? curve->domain->second : curve->domain->first;
    }
    if ((sense == "T" && *last <= *first) ||
        (sense == "F" && *last >= *first)) {
      error = "Edge SameSense conflicts with its endpoint parameters";
      return std::nullopt;
    }
    // Reserve part of the face's 1 mm budget for float storage and surface
    // chart interpolation. Sampling exactly at the limit can reject valid
    // large-radius edges after rounding their shared positions.
    auto samples = sampleCurve(*curve, *first, *last, .0005 / unitScale_, 4097);
    if (samples.size() < 2 ||
        glm::length(samples.front().point - *a) > tolerance ||
        glm::length(samples.back().point - *b) > tolerance) {
      error = "Edge sampling failed or exceeded its budget";
      return std::nullopt;
    }
    BrepEdge result{*start, *end, {}};
    for (const auto &sample : samples)
      result.points.emplace_back(sample.point);
    // Adjacent edges and faces use exactly the authored vertex positions.
    result.points.front() = glm::vec3(*a);
    result.points.back() = glm::vec3(*b);
    return result;
  }

  std::vector<MeshGroup> appendBrepGeometry(const Entity &brep) {
    const auto fail = [&](std::string reason) -> std::vector<MeshGroup> {
      conversionErrors_[brep.id] = std::move(reason);
      return {};
    };
    const bool advanced = brep.type == "IFCADVANCEDBREP" ||
                          brep.type == "IFCADVANCEDBREPWITHVOIDS";
    const bool hasVoids = brep.type == "IFCFACETEDBREPWITHVOIDS" ||
                          brep.type == "IFCADVANCEDBREPWITHVOIDS";
    const auto outer = firstRef(brep, 0);
    if (!outer)
      return fail("Missing B-rep outer shell");
    std::vector<uint32_t> shells{*outer};
    if (hasVoids) {
      const auto voids = refList(argAt(brep, 1));
      if (voids.empty() || voids.size() > 128 ||
          voids.size() != asList(argAt(brep, 1)).size())
        return fail("Invalid or excessive B-rep void shells");
      shells.insert(shells.end(), voids.begin(), voids.end());
    }
    const auto baseColor = styleColorByItem_.contains(brep.id)
                               ? styleColorByItem_.at(brep.id)
                               : defaultColor();
    struct FaceGroup {
      glm::vec4 color;
      std::vector<TriangleVertices> triangles;
    };
    std::map<uint32_t, FaceGroup> groups;
    std::unordered_map<uint32_t, BrepEdge> edgeCache;
    std::unordered_set<uint32_t> shellIds, faceIds;
    std::vector<manifold::Manifold> solids;
    size_t triangleCount = 0, edgeSampleCount = 0;
    for (size_t shellIndex = 0; shellIndex < shells.size(); ++shellIndex) {
      const uint32_t shellRef = shells[shellIndex];
      const auto *shell = entity(shellRef);
      if (!shell || shell->type != "IFCCLOSEDSHELL" ||
          !shellIds.insert(shellRef).second)
        return fail("Invalid or repeated closed shell #" +
                    std::to_string(shellRef));
      const auto faces = refList(argAt(*shell, 0));
      if (faces.empty() || faces.size() > 65536 ||
          faces.size() != asList(argAt(*shell, 0)).size())
        return fail("Invalid or excessive faces in shell #" +
                    std::to_string(shellRef));
      std::vector<TriangleVertices> shellTriangles;
      std::map<uint32_t, std::pair<unsigned, int>> edgeUses;
      for (uint32_t faceRef : faces) {
        const auto faceFail = [&](std::string reason) {
          return fail("Face #" + std::to_string(faceRef) + ": " + reason);
        };
        const auto *face = entity(faceRef);
        if (!face || face->type != (advanced ? "IFCADVANCEDFACE" : "IFCFACE") ||
            !faceIds.insert(faceRef).second)
          return faceFail("Invalid or repeated face");
        std::vector<std::vector<glm::vec3>> loops;
        const auto bounds = refList(argAt(*face, 0));
        if (bounds.empty() || bounds.size() > 4096 ||
            bounds.size() != asList(argAt(*face, 0)).size())
          return faceFail("Invalid face bounds");
        bool hasOuterBound = false;
        size_t faceVertexCount = 0;
        for (uint32_t boundRef : bounds) {
          const auto *bound = entity(boundRef);
          if (!bound || (bound->type != "IFCFACEOUTERBOUND" &&
                         bound->type != "IFCFACEBOUND"))
            return faceFail("Invalid boundary #" + std::to_string(boundRef));
          const auto orientation = enumValue(argAt(*bound, 1)).value_or("");
          if (orientation != "T" && orientation != "F")
            return faceFail("Invalid boundary orientation");
          const auto loopRef = firstRef(*bound, 0);
          const auto *poly = loopRef ? entity(*loopRef) : nullptr;
          if (advanced && poly && poly->type == "IFCVERTEXLOOP") {
            const auto vertexRef = firstRef(*poly, 0);
            const auto *vertex = vertexRef ? entity(*vertexRef) : nullptr;
            const auto pointRef = vertex && vertex->type == "IFCVERTEXPOINT"
                                      ? firstRef(*vertex, 0)
                                      : std::nullopt;
            const auto point =
                pointRef ? readBrepPoint(*pointRef) : std::nullopt;
            if (!point || !std::isfinite(point->x) ||
                !std::isfinite(point->y) || !std::isfinite(point->z))
              return faceFail("Vertex loop requires a finite 3D vertex point");
            if (++faceVertexCount > 8192)
              return faceFail("Face exceeds its boundary vertex budget");
            if (bound->type == "IFCFACEOUTERBOUND") {
              if (hasOuterBound)
                return faceFail("Multiple outer bounds");
              hasOuterBound = true;
              loops.insert(loops.begin(), {glm::vec3(*point)});
            } else
              loops.push_back({glm::vec3(*point)});
            continue;
          }
          if (!poly || poly->type != (advanced ? "IFCEDGELOOP" : "IFCPOLYLOOP"))
            return faceFail("Unsupported boundary loop");
          const auto refs = refList(argAt(*poly, 0));
          if (refs.empty() || refs.size() > 8192 ||
              refs.size() != asList(argAt(*poly, 0)).size())
            return faceFail("Invalid or excessive loop entries");
          std::vector<glm::vec3> loop;
          uint32_t firstVertex = 0, lastVertex = 0;
          for (uint32_t entry : refs) {
            if (!advanced) {
              const auto p = readBrepPoint(entry);
              if (!p)
                return faceFail("Invalid Cartesian point #" +
                                std::to_string(entry));
              loop.emplace_back(*p);
              continue;
            }
            const auto *oriented = entity(entry);
            const auto edgeRef = oriented && oriented->type == "IFCORIENTEDEDGE"
                                     ? firstRef(*oriented, 2)
                                     : std::nullopt;
            const auto sense =
                oriented ? enumValue(argAt(*oriented, 3)).value_or("") : "";
            if (!edgeRef || (sense != "T" && sense != "F"))
              return faceFail("Invalid oriented edge #" +
                              std::to_string(entry));
            if (!edgeCache.contains(*edgeRef)) {
              std::string error;
              auto edge = readBrepEdge(*edgeRef, error);
              if (!edge)
                return faceFail("Edge #" + std::to_string(*edgeRef) + ": " +
                                error);
              edgeSampleCount += edge->points.size();
              if (edgeSampleCount > 1000000)
                return faceFail("B-rep exceeds its shared edge sample budget");
              edgeCache.emplace(*edgeRef, std::move(*edge));
            }
            const auto &edge = edgeCache.at(*edgeRef);
            const auto start = sense == "T" ? edge.start : edge.end;
            const auto end = sense == "T" ? edge.end : edge.start;
            if (lastVertex && lastVertex != start)
              return faceFail("Edge loop is not topologically connected");
            if (!firstVertex)
              firstVertex = start;
            lastVertex = end;
            auto &use = edgeUses[*edgeRef];
            ++use.first;
            use.second += (sense == orientation ? 1 : -1);
            for (size_t i = 0; i + 1 < edge.points.size(); ++i)
              loop.push_back(
                  edge.points[sense == "T" ? i : edge.points.size() - 1 - i]);
            if (loop.size() > 8192)
              return faceFail("Face loop exceeds its vertex budget");
          }
          if (advanced && firstVertex != lastVertex)
            return faceFail("Edge loop is not closed");
          faceVertexCount += loop.size();
          if (faceVertexCount > 8192)
            return faceFail("Face exceeds its boundary vertex budget");
          if (orientation == "F")
            std::ranges::reverse(loop);
          if (bound->type == "IFCFACEOUTERBOUND") {
            if (hasOuterBound)
              return faceFail("Multiple outer bounds");
            hasOuterBound = true;
            loops.insert(loops.begin(), std::move(loop));
          } else
            loops.push_back(std::move(loop));
        }
        if (!hasOuterBound && !advanced && loops.size() > 1) {
          // IfcFace.Bounds is an unordered set. Without an explicit outer
          // bound, identify the enclosing loop geometrically, then validate
          // all holes in triangulatePlanarLoops.
          const auto areaSquared = [](const std::vector<glm::vec3> &loop) {
            glm::dvec3 area(0);
            if (loop.empty())
              return 0.;
            const glm::dvec3 origin(loop.front());
            for (size_t i = 0; i < loop.size(); ++i)
              area +=
                  glm::cross(glm::dvec3(loop[i]) - origin,
                             glm::dvec3(loop[(i + 1) % loop.size()]) - origin);
            return glm::dot(area, area);
          };
          const auto enclosing =
              std::ranges::max_element(loops, {}, areaSquared);
          std::iter_swap(loops.begin(), enclosing);
        }
        std::optional<std::vector<TriangleVertices>> triangles;
        if (advanced) {
          const auto surfaceRef = firstRef(*face, 1);
          const auto *surface = surfaceRef ? entity(*surfaceRef) : nullptr;
          const auto sameSense = enumValue(argAt(*face, 2)).value_or("");
          if (!surface || (sameSense != "T" && sameSense != "F"))
            return faceFail("Invalid face surface or SameSense");
          if (surface->type != "IFCSPHERICALSURFACE" &&
              std::ranges::any_of(
                  loops, [](const auto &loop) { return loop.size() == 1; }))
            return faceFail(
                "Vertex-loop charts are only supported on spherical faces");
          if (surface->type != "IFCPLANE") {
            if (surface->type != "IFCCYLINDRICALSURFACE" &&
                surface->type != "IFCSPHERICALSURFACE" &&
                surface->type != "IFCTOROIDALSURFACE" &&
                surface->type != "IFCBSPLINESURFACEWITHKNOTS" &&
                surface->type != "IFCRATIONALBSPLINESURFACEWITHKNOTS" &&
                surface->type != "IFCSURFACEOFLINEAREXTRUSION" &&
                surface->type != "IFCSURFACEOFREVOLUTION")
              return faceFail("Unsupported advanced face surface " +
                              surface->type);
            std::string error;
            const auto descriptor =
                detail::readIfcSurface(entities_, *surfaceRef, unitScale_,
                                       projectPlaneAngleScale(), error);
            if (!descriptor)
              return faceFail(error);
            const auto mesh =
                detail::meshIfcCurvedFace(*descriptor, loops, hasOuterBound,
                                          sameSense == "T", unitScale_, error);
            if (!mesh)
              return faceFail(error);
            triangles.emplace();
            triangles->reserve(mesh->size());
            for (const auto &triangle : *mesh)
              triangles->push_back({triangle.positions, triangle.normals});
          } else {
            if (!hasOuterBound)
              return faceFail("A planar advanced face requires an outer bound");
            const auto placementRef = firstRef(*surface, 0);
            const auto *placement =
                placementRef ? entity(*placementRef) : nullptr;
            const auto locationRef =
                placement && placement->type == "IFCAXIS2PLACEMENT3D"
                    ? firstRef(*placement, 0)
                    : std::nullopt;
            const auto origin =
                locationRef ? readBrepPoint(*locationRef) : std::nullopt;
            const auto readAxis =
                [&](size_t index,
                    glm::dvec3 fallback) -> std::optional<glm::dvec3> {
              const auto *value =
                  placement ? argAt(*placement, index) : nullptr;
              if (!value || value->kind == StepValue::Kind::Omitted)
                return fallback;
              const auto directionRef = firstRef(*placement, index);
              const auto *direction =
                  directionRef ? entity(*directionRef) : nullptr;
              if (!direction || direction->type != "IFCDIRECTION")
                return std::nullopt;
              const auto values = readNumberList(argAt(*direction, 0));
              if (values.size() != 3 ||
                  asList(argAt(*direction, 0)).size() != 3)
                return std::nullopt;
              const glm::dvec3 axis(values[0], values[1], values[2]);
              return glm::length(axis) > 0 ? std::optional(glm::normalize(axis))
                                           : std::nullopt;
            };
            const auto normal = readAxis(1, {0, 0, 1});
            const auto refDirection = readAxis(2, {1, 0, 0});
            if (!origin || !normal || !refDirection ||
                (firstRef(*placement, 2) &&
                 glm::length(glm::cross(*normal, *refDirection)) < 1e-10) ||
                (sameSense != "T" && sameSense != "F"))
              return faceFail("Invalid plane placement or SameSense");
            double extent = 0;
            if (loops.front().empty())
              return faceFail("Empty outer loop");
            const glm::dvec3 boundaryOrigin(loops.front().front());
            for (const auto &loop : loops)
              for (const auto &p : loop)
                extent = std::max(extent,
                                  glm::length(glm::dvec3(p) - boundaryOrigin));
            const double tolerance = std::max(1e-7 / unitScale_, extent * 1e-6);
            for (const auto &loop : loops)
              for (const auto &p : loop)
                if (std::abs(glm::dot(glm::dvec3(p) - *origin, *normal)) >
                    tolerance)
                  return faceFail("Boundary does not lie on its face plane");
            glm::dvec3 winding(0);
            const auto &outerLoop = loops.front();
            for (size_t i = 0; i < outerLoop.size(); ++i)
              winding +=
                  glm::cross(glm::dvec3(outerLoop[i]) - boundaryOrigin,
                             glm::dvec3(outerLoop[(i + 1) % outerLoop.size()]) -
                                 boundaryOrigin);
            if (glm::dot(winding, *normal) * (sameSense == "T" ? 1 : -1) <= 0)
              return faceFail(
                  "Face SameSense conflicts with its oriented boundary");
          }
        }
        if (!triangles)
          triangles = triangulatePlanarLoops(std::move(loops));
        if (!triangles || (triangleCount += triangles->size()) > 1000000)
          return faceFail("Invalid face loops or excessive triangulation");
        shellTriangles.insert(shellTriangles.end(), triangles->begin(),
                              triangles->end());
        const auto color = styleColorByItem_.contains(faceRef)
                               ? styleColorByItem_.at(faceRef)
                               : baseColor;
        auto &group = groups[packColor(color)];
        group.color = color;
        group.triangles.insert(group.triangles.end(), triangles->begin(),
                               triangles->end());
      }
      for (const auto &[edge, use] : edgeUses)
        if (use.first != 2 || use.second != 0)
          return fail("Shell edge #" + std::to_string(edge) +
                      " must be used twice in opposite directions");
      // Validate in temporary buffers. No invalid shell or later void can leave
      // partial draw ranges, and exact welding also detects missing seam edges.
      manifold::MeshGL64 mesh;
      std::map<std::array<float, 3>, uint32_t> vertices;
      const glm::dvec3 origin(shellTriangles.front().positions.front());
      double signedVolume = 0;
      for (const auto &triangle : shellTriangles) {
        auto positions = triangle.positions;
        const auto cross =
            glm::cross(glm::dvec3(positions[1]) - glm::dvec3(positions[0]),
                       glm::dvec3(positions[2]) - glm::dvec3(positions[0]));
        if (!(glm::dot(cross, cross) > 0))
          return fail("Collapsed or non-finite B-rep triangle");
        signedVolume += glm::dot(glm::dvec3(positions[0]) - origin, cross) / 6;
        if (shellIndex != 0)
          std::swap(positions[1], positions[2]);
        for (const auto &p : positions) {
          if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return fail("B-rep coordinates exceed the renderer range");
          const auto [it, inserted] = vertices.try_emplace(
              {p.x, p.y, p.z}, static_cast<uint32_t>(vertices.size()));
          if (inserted)
            mesh.vertProperties.insert(mesh.vertProperties.end(),
                                       {double(p.x), double(p.y), double(p.z)});
          mesh.triVerts.push_back(it->second);
        }
      }
      if (!std::isfinite(signedVolume) ||
          (shellIndex == 0 ? signedVolume <= 0 : signedVolume >= 0))
        return fail("Shell #" + std::to_string(shellRef) +
                    " has incorrect outer/void orientation or zero volume");
      manifold::Manifold solid(mesh);
      if (solid.Status() != manifold::Manifold::Error::NoError ||
          solid.IsEmpty() || solid.Decompose().size() != 1)
        return fail("Shell #" + std::to_string(shellRef) +
                    " is not a connected closed manifold");
      solids.push_back(std::move(solid));
    }
    if (hasVoids) {
      const auto box = solids.front().BoundingBox();
      const auto size = box.max - box.min;
      const double extent = std::max({size.x, size.y, size.z});
      const double clearance = std::max(1e-7 / unitScale_, extent * 1e-7);
      const double margin = std::max(extent * .1, clearance * 4);
      const auto exterior =
          manifold::Manifold::Cube(size + manifold::vec3(margin * 2))
              .Translate(box.min - manifold::vec3(margin))
              .Boolean(solids.front(), manifold::OpType::Subtract);
      if (exterior.Status() != manifold::Manifold::Error::NoError)
        return fail("B-rep enclosure validation failed");
      for (size_t i = 1; i < solids.size(); ++i) {
        const auto outside =
            solids[i].Boolean(solids.front(), manifold::OpType::Subtract);
        if (outside.Status() != manifold::Manifold::Error::NoError ||
            !outside.IsEmpty() ||
            solids[i].MinGap(exterior, clearance * 2) <= clearance)
          return fail("Void shell #" + std::to_string(shells[i]) +
                      " must be strictly enclosed by the outer shell");
        for (size_t j = 1; j < i; ++j)
          if (solids[i].MinGap(solids[j], clearance * 2) <= clearance)
            return fail(
                "Void shells must be disjoint and cannot contain one another");
      }
    }
    std::vector<MeshGroup> result;
    for (const auto &[_, group] : groups)
      if (auto mesh = appendTriangleMesh(group.triangles, group.color))
        result.push_back(*mesh);
    return result;
  }

  std::optional<MeshGroup>
  appendTriangleMesh(std::span<const TriangleVertices> triangles,
                     glm::vec4 color) {
    if (triangles.empty()) {
      return std::nullopt;
    }

    // Validate before mutating buffers: finite STEP doubles may still overflow
    // float vertices after placement/extrusion, or collapse a tiny triangle.
    for (const auto &triangle : triangles) {
      for (const auto &p : triangle.positions)
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          return std::nullopt;
      const auto cross = glm::cross(glm::dvec3(triangle.positions[1]) -
                                        glm::dvec3(triangle.positions[0]),
                                    glm::dvec3(triangle.positions[2]) -
                                        glm::dvec3(triangle.positions[0]));
      if (glm::dot(cross, cross) <= 0)
        return std::nullopt;
    }

    const uint32_t firstIndex = static_cast<uint32_t>(model_.indices.size());
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(std::numeric_limits<float>::lowest());

    for (const auto &triangle : triangles) {
      const auto &positions = triangle.positions;
      const glm::vec3 normal =
          safeNormal(positions[0], positions[1], positions[2]);
      const uint32_t base = static_cast<uint32_t>(model_.vertices.size());
      for (size_t i = 0; i < 3; ++i) {
        const auto n = triangle.normals ? (*triangle.normals)[i] : normal;
        const auto tangent = safeTangent(positions[0], positions[1], n);
        model_.vertices.push_back(makeVertex(positions[i], n, tangent));
      }
      model_.indices.insert(model_.indices.end(), {base, base + 1u, base + 2u});

      for (const auto &position : positions) {
        minBounds = glm::min(minBounds, position);
        maxBounds = glm::max(maxBounds, position);
      }
    }

    const uint32_t indexCount =
        static_cast<uint32_t>(model_.indices.size()) - firstIndex;
    if (indexCount == 0u) {
      return std::nullopt;
    }

    const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
    float radius = 0.0f;
    for (const auto &triangle : triangles) {
      for (const auto &position : triangle.positions) {
        radius = std::max(radius, glm::length(position - center));
      }
    }

    const uint32_t meshId = nextMeshId_++;
    model_.meshRanges.push_back(
        {meshId, firstIndex, indexCount, center, radius});
    return MeshGroup{meshId, sanitizeColor(color)};
  }

  std::vector<MeshGroup> appendFaceSetGeometry(const Entity &faceSet) {
    const auto pointsRef = firstRef(faceSet, 0);
    if (!pointsRef.has_value()) {
      return {};
    }
    const auto points = readPointList3D(*pointsRef);
    if (points.empty()) {
      return {};
    }
    const auto pnIndex = readPositiveIndexList(argAt(faceSet, 4));
    const bool hasPnIndex =
        argAt(faceSet, 4) && argAt(faceSet, 4)->kind == StepValue::Kind::List;
    if (hasPnIndex && pnIndex.size() != asList(argAt(faceSet, 4)).size())
      return {};

    const auto faceColorsIt = faceColorsByFaceSet_.find(faceSet.id);
    const std::vector<glm::vec4> *faceColors =
        faceColorsIt == faceColorsByFaceSet_.end() ? nullptr
                                                   : &faceColorsIt->second;
    const glm::vec4 baseColor = styleColorByItem_.contains(faceSet.id)
                                    ? styleColorByItem_.at(faceSet.id)
                                    : defaultColor();

    struct Triangle {
      std::array<uint32_t, 3> indices{};
    };
    struct Group {
      glm::vec4 color{defaultColor()};
      std::vector<Triangle> triangles{};
    };
    std::map<uint32_t, Group> groups;

    size_t faceIndex = 0;
    for (const StepValue &faceValue : asList(argAt(faceSet, 3))) {
      auto indices = readPositiveIndexList(&faceValue);
      if (indices.size() != 3u || asList(&faceValue).size() != 3u)
        return {};
      for (auto &index : indices) {
        if (hasPnIndex) {
          if (index > pnIndex.size())
            return {};
          index = pnIndex[index - 1u];
        }
        if (index > points.size())
          return {};
      }
      // Validate every face before creating any color groups.
      const auto cross =
          glm::cross(points[indices[1] - 1u] - points[indices[0] - 1u],
                     points[indices[2] - 1u] - points[indices[0] - 1u]);
      if (!std::isfinite(cross.x) || !std::isfinite(cross.y) ||
          !std::isfinite(cross.z) || glm::dot(cross, cross) <= 1e-20f)
        return {};
      const glm::vec4 color =
          faceColors != nullptr && faceIndex < faceColors->size()
              ? (*faceColors)[faceIndex]
              : baseColor;
      const uint32_t colorKey = packColor(color);
      auto &group = groups[colorKey];
      group.color = color;
      for (size_t i = 1; i + 1u < indices.size(); ++i) {
        group.triangles.push_back(
            Triangle{{indices[0], indices[i], indices[i + 1u]}});
      }
      ++faceIndex;
    }

    std::vector<MeshGroup> meshGroups;
    for (const auto &[_, group] : groups) {
      if (group.triangles.empty()) {
        continue;
      }

      std::vector<TriangleVertices> triangles;
      triangles.reserve(group.triangles.size());
      for (const Triangle &triangle : group.triangles) {
        std::array<glm::vec3, 3> positions{};
        bool valid = true;
        for (size_t i = 0; i < triangle.indices.size(); ++i) {
          const uint32_t oneBasedIndex = triangle.indices[i];
          if (oneBasedIndex == 0u || oneBasedIndex > points.size()) {
            valid = false;
            break;
          }
          positions[i] = points[oneBasedIndex - 1u];
        }
        if (!valid) {
          continue;
        }
        triangles.push_back({positions});
      }

      auto meshGroup = appendTriangleMesh(triangles, group.color);
      if (!meshGroup.has_value()) {
        continue;
      }
      meshGroups.push_back(*meshGroup);
    }

    return meshGroups;
  }

  void appendSweptSolidGeometry() {
    for (const auto id : sortedEntityIds_) {
      const Entity *solid = entity(id);
      if (solid == nullptr) {
        continue;
      }
      std::optional<MeshGroup> group;
      if (solid->type == "IFCEXTRUDEDAREASOLID")
        group = appendExtrudedAreaSolidGeometry(*solid);
      else if (solid->type == "IFCSWEPTDISKSOLID")
        group = appendSweptDiskGeometry(*solid);
      else if (solid->type == "IFCSWEPTDISKSOLIDPOLYGONAL")
        group = appendParametricSweptDiskGeometry(*solid);
      if (group) {
        groupsByItem_[id] = {*group};
      }
    }
  }

  struct StraightCurve {
    glm::dvec3 origin{}, derivative{};
    std::optional<double> end;
  };

  std::optional<StraightCurve> readStraightCurve(uint32_t ref) const {
    const auto *curve = entity(ref);
    if (!curve)
      return std::nullopt;
    if (curve->type == "IFCLINE") {
      const auto pointRef = firstRef(*curve, 0),
                 vectorRef = firstRef(*curve, 1);
      const auto *point = pointRef ? entity(*pointRef) : nullptr;
      const auto *vector = vectorRef ? entity(*vectorRef) : nullptr;
      if (!point || point->type != "IFCCARTESIANPOINT" || !vector ||
          vector->type != "IFCVECTOR")
        return std::nullopt;
      const auto directionRef = firstRef(*vector, 0);
      const auto *direction = directionRef ? entity(*directionRef) : nullptr;
      const auto magnitude = numberValue(argAt(*vector, 1));
      if (!direction || direction->type != "IFCDIRECTION" || !magnitude ||
          !std::isfinite(*magnitude) || *magnitude <= 0)
        return std::nullopt;
      const auto p = readNumberList(argAt(*point, 0));
      const auto d = readNumberList(argAt(*direction, 0));
      if (p.size() != 3u || d.size() != 3u ||
          asList(argAt(*point, 0)).size() != 3u ||
          asList(argAt(*direction, 0)).size() != 3u)
        return std::nullopt;
      glm::dvec3 axis(d[0], d[1], d[2]);
      if (glm::length(axis) <= 0)
        return std::nullopt;
      return StraightCurve{
          {p[0], p[1], p[2]}, glm::normalize(axis) * *magnitude, std::nullopt};
    }
    if (curve->type != "IFCTRIMMEDCURVE")
      return std::nullopt;
    const auto basisRef = firstRef(*curve, 0);
    const auto *basisEntity = basisRef ? entity(*basisRef) : nullptr;
    // IFC bounded curves cannot themselves be trimmed. Avoid recursive cycles.
    if (!basisEntity || basisEntity->type != "IFCLINE")
      return std::nullopt;
    auto basis = readStraightCurve(*basisRef);
    if (!basis)
      return std::nullopt;
    const auto trimParameter =
        [&](const StepValue *trim) -> std::optional<double> {
      std::optional<double> parameter, pointParameter;
      const auto values = asList(trim);
      if (values.empty() || values.size() > 2u)
        return std::nullopt;
      for (const auto &value : values) {
        if (value.kind == StepValue::Kind::List &&
            value.text == "IFCPARAMETERVALUE") {
          if (parameter || value.list.size() != 1u)
            return std::nullopt;
          parameter = numberValue(&value.list.front());
          if (!parameter || !std::isfinite(*parameter))
            return std::nullopt;
        } else if (value.kind == StepValue::Kind::Ref) {
          const auto *point = entity(value.ref);
          if (!point || point->type != "IFCCARTESIANPOINT" || pointParameter)
            return std::nullopt;
          const auto p = readNumberList(argAt(*point, 0));
          if (p.size() != 3u || asList(argAt(*point, 0)).size() != 3u)
            return std::nullopt;
          const glm::dvec3 delta = glm::dvec3(p[0], p[1], p[2]) - basis->origin;
          pointParameter = glm::dot(delta, basis->derivative) /
                           glm::dot(basis->derivative, basis->derivative);
          if (glm::length(delta - basis->derivative * *pointParameter) >
              std::max(1e-8, glm::length(delta) * 1e-8))
            return std::nullopt;
        } else
          return std::nullopt;
      }
      if (parameter && pointParameter &&
          std::abs(*parameter - *pointParameter) >
              std::max(1e-8, std::abs(*parameter) * 1e-8))
        return std::nullopt;
      const auto preference = enumValue(argAt(*curve, 4)).value_or("");
      if (preference != "PARAMETER" && preference != "CARTESIAN" &&
          preference != "UNSPECIFIED")
        return std::nullopt;
      return preference == "CARTESIAN" && pointParameter ? pointParameter
             : parameter                                 ? parameter
                                                         : pointParameter;
    };
    const auto first = trimParameter(argAt(*curve, 1));
    const auto last = trimParameter(argAt(*curve, 2));
    const auto sense = enumValue(argAt(*curve, 3)).value_or("");
    if (!first || !last || *first == *last || (sense != "T" && sense != "F") ||
        ((*last > *first) != (sense == "T")))
      return std::nullopt;
    basis->origin += basis->derivative * *first;
    basis->derivative *= sense == "T" ? 1.0 : -1.0;
    basis->end = std::abs(*last - *first);
    return basis;
  }

  std::optional<MeshGroup> appendSweptDiskGeometry(const Entity &solid) {
    const auto directrixRef = firstRef(solid, 0);
    auto curve = directrixRef ? readStraightCurve(*directrixRef) : std::nullopt;
    if (!curve) {
      if (auto group = appendCurvedSweptDiskGeometry(solid))
        return group;
      return appendParametricSweptDiskGeometry(solid);
    }
    const auto radius = numberValue(argAt(solid, 1));
    if (!curve || !radius || !std::isfinite(*radius) || *radius <= 0)
      return std::nullopt;
    const auto startValue = argAt(solid, 3), endValue = argAt(solid, 4);
    if (!startValue || !endValue)
      return std::nullopt;
    const auto start = startValue->kind == StepValue::Kind::Omitted
                           ? std::optional<double>(0)
                           : numberValue(startValue);
    const auto end = endValue->kind == StepValue::Kind::Omitted
                         ? curve->end
                         : numberValue(endValue);
    if (!start || !end || !std::isfinite(*start) || !std::isfinite(*end) ||
        *end <= *start ||
        (curve->end &&
         (*start < 0 ||
          *end > *curve->end + std::max(1e-8, *curve->end * 1e-8))) ||
        (!curve->end && (startValue->kind == StepValue::Kind::Omitted ||
                         endValue->kind == StepValue::Kind::Omitted)))
      return std::nullopt;
    const auto segments = circleSegmentCount(*radius);
    if (!segments)
      return std::nullopt;
    std::vector<std::vector<glm::vec3>> loops{circleLoop(*radius, *segments)};
    const auto innerValue = argAt(solid, 2);
    if (!innerValue)
      return std::nullopt;
    if (innerValue->kind != StepValue::Kind::Omitted) {
      const auto inner = numberValue(innerValue);
      if (!inner || !std::isfinite(*inner) || *inner <= 0 || *inner >= *radius)
        return std::nullopt;
      loops.push_back(circleLoop(*inner, *segments));
      std::ranges::reverse(loops.back());
    }
    const auto caps = triangulatePlanarLoops(loops);
    if (!caps)
      return std::nullopt;
    const glm::dvec3 axis = glm::normalize(curve->derivative);
    const auto x = glm::normalize(glm::cross(
        std::abs(axis.z) < .9 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0),
        axis));
    const auto y = glm::cross(axis, x);
    const auto position = [&](const glm::vec3 &point, double parameter) {
      return glm::vec3(curve->origin + curve->derivative * parameter +
                       x * double(point.x) + y * double(point.y));
    };
    std::vector<TriangleVertices> triangles;
    for (const auto &cap : *caps) {
      std::array<glm::vec3, 3> bottom{}, top{};
      for (size_t i = 0; i < 3; ++i) {
        bottom[i] = position(cap.positions[i], *start);
        top[i] = position(cap.positions[i], *end);
      }
      appendCapTriangle(triangles, bottom, glm::vec3(-axis));
      appendCapTriangle(triangles, top, glm::vec3(axis));
    }
    for (const auto &loop : loops)
      for (size_t i = 0; i < loop.size(); ++i) {
        const auto a = position(loop[i], *start),
                   b = position(loop[(i + 1) % loop.size()], *start);
        const auto c = position(loop[(i + 1) % loop.size()], *end),
                   d = position(loop[i], *end);
        triangles.push_back({{a, b, c}});
        triangles.push_back({{a, c, d}});
      }
    return appendTriangleMesh(triangles, styleColorByItem_.contains(solid.id)
                                             ? styleColorByItem_.at(solid.id)
                                             : defaultColor());
  }

  std::optional<double> planeAngleUnitScale(uint32_t ref,
                                            size_t depth = 0) const {
    const auto *unit = entity(ref);
    if (!unit || depth > 4)
      return std::nullopt;
    if (unit->type == "IFCSIUNIT" &&
        enumValue(argAt(*unit, 1)) == "PLANEANGLEUNIT" &&
        enumValue(argAt(*unit, 3)) == "RADIAN" && argAt(*unit, 2) &&
        argAt(*unit, 2)->kind == StepValue::Kind::Omitted)
      return 1;
    if (unit->type != "IFCCONVERSIONBASEDUNIT" ||
        enumValue(argAt(*unit, 1)) != "PLANEANGLEUNIT")
      return std::nullopt;
    const auto measureRef = firstRef(*unit, 3);
    const auto *measure = measureRef ? entity(*measureRef) : nullptr;
    if (!measure || measure->type != "IFCMEASUREWITHUNIT")
      return std::nullopt;
    const auto *value = argAt(*measure, 0);
    const auto factor = numberValue(
        value && value->kind == StepValue::Kind::List && value->list.size() == 1
            ? &value->list.front()
            : value);
    const auto baseRef = firstRef(*measure, 1);
    const auto base =
        baseRef ? planeAngleUnitScale(*baseRef, depth + 1) : std::nullopt;
    if (!factor || !base || !std::isfinite(*factor) || *factor <= 0)
      return std::nullopt;
    return *factor * *base;
  }

  std::optional<double> projectPlaneAngleScale() const {
    for (uint32_t id : sortedEntityIds_) {
      const auto *project = entity(id);
      if (project->type != "IFCPROJECT")
        continue;
      const auto assignmentRef = firstRef(*project, 8);
      const auto *assignment = assignmentRef ? entity(*assignmentRef) : nullptr;
      if (!assignment || assignment->type != "IFCUNITASSIGNMENT")
        return std::nullopt;
      for (auto unitRef : refList(argAt(*assignment, 0))) {
        const auto *unit = entity(unitRef);
        if (unit && enumValue(argAt(*unit, 1)) == "PLANEANGLEUNIT")
          return planeAngleUnitScale(unitRef);
      }
      return 1;
    }
    return 1;
  }

  struct SweepPiece {
    glm::dvec3 origin{}, u{}, v{};
    double radius = 0, startAngle = 0, angleStep = 0, end = 0;
    glm::dvec3 point(double parameter) const {
      if (radius == 0)
        return origin + u * parameter;
      const double angle = startAngle + angleStep * parameter;
      return origin + radius * (u * std::cos(angle) + v * std::sin(angle));
    }
    glm::dvec3 tangent(double parameter) const {
      if (radius == 0)
        return glm::normalize(u);
      const double angle = startAngle + angleStep * parameter;
      return glm::normalize(angleStep *
                            (-u * std::sin(angle) + v * std::cos(angle)));
    }
    void reverse() {
      if (radius == 0) {
        origin += u * end;
        u = -u;
      } else {
        startAngle += angleStep * end;
        angleStep = -angleStep;
      }
    }
  };

  std::vector<SweepPiece>
  readSweepPieces(uint32_t ref, std::unordered_set<uint32_t> &visiting,
                  bool profileCurve = false) const {
    if (visiting.size() >= 64 || !visiting.insert(ref).second)
      return {};
    struct Guard {
      std::unordered_set<uint32_t> &s;
      uint32_t id;
      ~Guard() { s.erase(id); }
    } guard{visiting, ref};
    const auto *curve = entity(ref);
    if (!curve)
      return {};
    if (auto line = readStraightCurve(ref); line && line->end)
      return {{line->origin, line->derivative, {}, 0, 0, 0, *line->end}};
    if (curve->type == "IFCPOLYLINE" || curve->type == "IFCINDEXEDPOLYCURVE") {
      auto points = readPolylinePoints(ref);
      if (points.size() < 2)
        return {};
      std::vector<SweepPiece> pieces;
      for (size_t i = 1; i < points.size(); ++i)
        pieces.push_back({glm::dvec3(points[i - 1]),
                          glm::dvec3(points[i]) - glm::dvec3(points[i - 1]),
                          {},
                          0,
                          0,
                          0,
                          1});
      return pieces;
    }
    if (curve->type == "IFCTRIMMEDCURVE") {
      const auto basisRef = firstRef(*curve, 0);
      const auto *circle = basisRef ? entity(*basisRef) : nullptr;
      if (!circle || circle->type != "IFCCIRCLE")
        return {};
      const auto radius = numberValue(argAt(*circle, 1));
      const auto positionRef = firstRef(*circle, 0);
      const auto *position = positionRef ? entity(*positionRef) : nullptr;
      const auto angleScale = projectPlaneAngleScale();
      if (!radius || !std::isfinite(*radius) || *radius <= 0 || !position ||
          (position->type != "IFCAXIS2PLACEMENT3D" &&
           (!profileCurve || position->type != "IFCAXIS2PLACEMENT2D")) ||
          !angleScale)
        return {};
      const auto parameter =
          [](const StepValue *value) -> std::optional<double> {
        const auto list = asList(value);
        if (list.size() != 1 || list[0].text != "IFCPARAMETERVALUE" ||
            list[0].list.size() != 1)
          return std::nullopt;
        return numberValue(&list[0].list[0]);
      };
      const auto first = parameter(argAt(*curve, 1)),
                 last = parameter(argAt(*curve, 2));
      const auto sense = enumValue(argAt(*curve, 3)).value_or("");
      const auto preference = enumValue(argAt(*curve, 4)).value_or("");
      if (!first || !last || !std::isfinite(*first) || !std::isfinite(*last) ||
          (sense != "T" && sense != "F") ||
          (preference != "PARAMETER" && preference != "UNSPECIFIED"))
        return {};
      const double period = 2 * std::acos(-1.0) / *angleScale;
      double extent =
          std::fmod((sense == "T" ? *last - *first : *first - *last), period);
      if (extent < 0)
        extent += period;
      if (extent <= 1e-12 || extent >= period - 1e-12)
        return {};
      auto matrix = axis2Placement3D(*positionRef);
      if (position->type == "IFCAXIS2PLACEMENT2D") {
        const auto originRef = firstRef(*position, 0),
                   directionRef = firstRef(*position, 1);
        if (!originRef)
          return {};
        const auto origin = readPoint(*originRef, glm::vec3(0));
        const auto x = directionRef
                           ? readDirection(*directionRef, glm::vec3(1, 0, 0))
                           : glm::vec3(1, 0, 0);
        if (origin.z != 0 || x.z != 0)
          return {};
        matrix[0] = glm::vec4(x, 0);
        matrix[1] = glm::vec4(-x.y, x.x, 0, 0);
        matrix[2] = glm::vec4(0, 0, 1, 0);
        matrix[3] = glm::vec4(origin, 1);
      }
      return {{glm::dvec3(matrix[3]), glm::dvec3(matrix[0]),
               glm::dvec3(matrix[1]), *radius, *first * *angleScale,
               (sense == "T" ? 1 : -1) * *angleScale, extent}};
    }
    if (curve->type != "IFCCOMPOSITECURVE" ||
        (!profileCurve && enumValue(argAt(*curve, 1)) == "T"))
      return {};
    const auto refs = refList(argAt(*curve, 0));
    if (refs.empty() || refs.size() > 256 ||
        refs.size() != asList(argAt(*curve, 0)).size())
      return {};
    std::vector<SweepPiece> result;
    for (size_t i = 0; i < refs.size(); ++i) {
      const auto *segment = entity(refs[i]);
      if (!segment || segment->type != "IFCCOMPOSITECURVESEGMENT")
        return {};
      const auto transition = enumValue(argAt(*segment, 0)).value_or("");
      if (!profileCurve &&
          (i == refs.size() - 1
               ? transition != "DISCONTINUOUS"
               : (transition != "CONTINUOUS" &&
                  transition != "CONTSAMEGRADIENT" &&
                  transition != "CONTSAMEGRADIENTSAMECURVATURE")))
        return {};
      const auto same = enumValue(argAt(*segment, 1)).value_or("");
      const auto parentRef = firstRef(*segment, 2);
      auto pieces = parentRef
                        ? readSweepPieces(*parentRef, visiting, profileCurve)
                        : std::vector<SweepPiece>{};
      if (pieces.empty() || (same != "T" && same != "F"))
        return {};
      if (same == "F") {
        std::ranges::reverse(pieces);
        for (auto &piece : pieces)
          piece.reverse();
      }
      if (result.size() + pieces.size() > 256)
        return {};
      result.insert(result.end(), pieces.begin(), pieces.end());
    }
    return result;
  }

  std::vector<glm::vec3> readProfileCurve(uint32_t ref) const {
    if (auto points = readPolylinePoints(ref); !points.empty())
      return points;
    std::string error;
    const auto curve = detail::readIfcCurve(entities_, ref, unitScale_,
                                            projectPlaneAngleScale(), error);
    if (!curve || curve->dimension != 2)
      return {};
    auto points = readRenderCurve(ref, error, 4097);
    if (points.size() < 4 ||
        glm::length(points.front() - points.back()) > 1e-5 / unitScale_)
      return {};
    points.pop_back();
    return points;
  }

  std::vector<glm::vec3> readRenderCurve(uint32_t ref, std::string &error,
                                         size_t limit = 65536) const {
    const auto *source = entity(ref);
    if (source && (source->type == "IFCPOLYLINE" ||
                   source->type == "IFCINDEXEDPOLYCURVE")) {
      auto points = readPolylinePoints(ref, true);
      if (points.size() > limit)
        return {};
      return points;
    }
    const auto curve = detail::readIfcCurve(entities_, ref, unitScale_,
                                            projectPlaneAngleScale(), error);
    if (!curve)
      return {};
    if (!curve->domain) {
      error = "Unbounded curve needs an explicit trim or curve segment extent";
      return {};
    }
    const auto samples =
        sampleCurve(*curve, curve->domain->first, curve->domain->second,
                    .001 / unitScale_, limit);
    if (samples.empty()) {
      error = "Curve evaluation failed, contains an undefined tangent/surface "
              "point, or exceeds its tessellation budget";
      return {};
    }
    std::vector<glm::vec3> points;
    points.reserve(samples.size());
    for (const auto &sample : samples) {
      const glm::vec3 p(sample.point);
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        error = "Curve point exceeds renderer coordinate precision";
        return {};
      }
      points.push_back(p);
    }
    return points;
  }

  std::optional<MeshGroup>
  appendParametricSweptDiskGeometry(const Entity &solid) {
    std::string error;
    const auto directrix = firstRef(solid, 0);
    auto curve = directrix
                     ? detail::readIfcCurve(entities_, *directrix, unitScale_,
                                            projectPlaneAngleScale(), error)
                     : std::nullopt;
    const auto *sv = argAt(solid, 3), *ev = argAt(solid, 4),
               *iv = argAt(solid, 2);
    const auto radius = numberValue(argAt(solid, 1));
    const auto fail = [&](std::string reason) -> std::optional<MeshGroup> {
      conversionErrors_[solid.id] = "Swept disk directrix #" +
                                    std::to_string(directrix.value_or(0)) +
                                    ": " + reason;
      return std::nullopt;
    };
    if (!curve)
      return fail(error.empty() ? "Missing directrix" : error);
    if (!sv || !ev || !iv || !radius)
      return fail("Missing swept disk attributes");
    if (solid.type == "IFCSWEPTDISKSOLIDPOLYGONAL") {
      const auto *path = directrix ? entity(*directrix) : nullptr;
      if (!path ||
          (path->type != "IFCPOLYLINE" &&
           path->type != "IFCINDEXEDPOLYCURVE") ||
          (path->type == "IFCINDEXEDPOLYCURVE" &&
           (!argAt(*path, 1) ||
            argAt(*path, 1)->kind != StepValue::Kind::Omitted)))
        return fail(
            "Polygonal sweep requires a polyline without indexed segments");
      const auto *fv = argAt(solid, 5);
      if (!fv)
        return fail("Missing polygonal fillet attribute");
      if (fv->kind != StepValue::Kind::Omitted) {
        const auto fillet = numberValue(fv);
        if (!fillet || *fillet < *radius || !curve->domain)
          return fail(
              "Polygonal fillet radius must be at least the disk radius");
        std::vector<glm::dvec3> points;
        std::vector<double> parameters{curve->domain->first};
        parameters.insert(parameters.end(), curve->breaks.begin(),
                          curve->breaks.end());
        parameters.push_back(curve->domain->second);
        for (double t : parameters) {
          const auto p = curve->point(t);
          if (!p)
            return fail("Invalid polygonal directrix point");
          points.push_back(*p);
        }
        curve = makeFilletedPolyline(std::move(points), *fillet, error);
        if (!curve)
          return fail(error);
      }
    }
    const auto first =
        sv->kind == StepValue::Kind::Omitted
            ? (curve->domain ? std::optional(curve->domain->first)
                             : std::nullopt)
            : numberValue(sv);
    const auto last =
        ev->kind == StepValue::Kind::Omitted
            ? (curve->domain ? std::optional(curve->domain->second)
                             : std::nullopt)
            : numberValue(ev);
    const auto inner = iv->kind == StepValue::Kind::Omitted ? std::optional(0.)
                                                            : numberValue(iv);
    if (!first || !last || !inner ||
        (iv->kind != StepValue::Kind::Omitted && *inner <= 0))
      return fail("Unbounded directrix requires explicit parameters; hollow "
                  "radius must be positive");
    const auto positions = buildSweptDisk(*curve, *first, *last, *radius,
                                          *inner, .001 / unitScale_, error);
    if (positions.empty())
      return fail(error);
    std::vector<TriangleVertices> triangles;
    triangles.reserve(positions.size());
    for (const auto &p : positions)
      triangles.push_back(
          {{glm::vec3(p[0]), glm::vec3(p[1]), glm::vec3(p[2])}});
    auto group =
        appendTriangleMesh(triangles, styleColorByItem_.contains(solid.id)
                                          ? styleColorByItem_.at(solid.id)
                                          : defaultColor());
    if (!group)
      return fail("Swept disk collapses at renderer coordinate precision");
    return group;
  }

  std::optional<MeshGroup> appendCurvedSweptDiskGeometry(const Entity &solid) {
    const auto directrixRef = firstRef(solid, 0);
    std::unordered_set<uint32_t> visiting;
    auto pieces = directrixRef ? readSweepPieces(*directrixRef, visiting)
                               : std::vector<SweepPiece>{};
    const auto radius = numberValue(argAt(solid, 1));
    if (pieces.empty() || !radius || !std::isfinite(*radius) || *radius <= 0)
      return std::nullopt;
    double domain = 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
      const auto &piece = pieces[i];
      if (piece.radius > 0 && piece.radius <= *radius)
        return std::nullopt;
      if (i > 0 && (glm::length(pieces[i - 1].point(pieces[i - 1].end) -
                                piece.point(0)) > 1e-5 / unitScale_ ||
                    glm::dot(pieces[i - 1].tangent(pieces[i - 1].end),
                             piece.tangent(0)) < 1 - 1e-5))
        return std::nullopt;
      domain += piece.end;
    }
    const auto *sv = argAt(solid, 3), *ev = argAt(solid, 4),
               *iv = argAt(solid, 2);
    if (!sv || !ev || !iv)
      return std::nullopt;
    const auto start = sv->kind == StepValue::Kind::Omitted
                           ? std::optional<double>(0)
                           : numberValue(sv);
    const auto end = ev->kind == StepValue::Kind::Omitted
                         ? std::optional<double>(domain)
                         : numberValue(ev);
    const auto inner = iv->kind == StepValue::Kind::Omitted
                           ? std::optional<double>(0)
                           : numberValue(iv);
    if (!start || !end || !inner || !std::isfinite(*start) ||
        !std::isfinite(*end) || !std::isfinite(*inner) || *start < 0 ||
        *end <= *start || *end > domain + std::max(1e-8, domain * 1e-8) ||
        *inner < 0 || *inner >= *radius ||
        (iv->kind != StepValue::Kind::Omitted && *inner == 0))
      return std::nullopt;
    const auto crossSegments = circleSegmentCount(*radius);
    if (!crossSegments)
      return std::nullopt;
    std::vector<std::vector<glm::vec3>> loops{
        circleLoop(*radius, *crossSegments)};
    if (*inner > 0) {
      loops.push_back(circleLoop(*inner, *crossSegments));
      std::ranges::reverse(loops.back());
    }
    const auto caps = triangulatePlanarLoops(loops);
    if (!caps)
      return std::nullopt;
    struct Frame {
      glm::dvec3 point, axis, x, y;
    };
    std::vector<Frame> frames;
    double offset = 0;
    for (const auto &piece : pieces) {
      const double a = std::max(0.0, *start - offset),
                   b = std::min(piece.end, *end - offset);
      if (b > a) {
        size_t steps = 1;
        if (piece.radius > 0) {
          const auto count = circleSegmentCount(piece.radius + *radius);
          if (!count)
            return std::nullopt;
          steps = std::max(size_t(1),
                           size_t(std::ceil(*count * std::abs(piece.angleStep) *
                                            (b - a) / (2 * std::acos(-1.0)))));
        }
        for (size_t i = frames.empty() ? 0 : 1; i <= steps; ++i) {
          if (frames.size() >= 4096)
            return std::nullopt;
          const double parameter = a + (b - a) * double(i) / steps;
          const auto axis = piece.tangent(parameter),
                     point = piece.point(parameter);
          glm::dvec3 x;
          if (frames.empty())
            x = glm::normalize(glm::cross(std::abs(axis.z) < .9
                                              ? glm::dvec3(0, 0, 1)
                                              : glm::dvec3(0, 1, 0),
                                          axis));
          else {
            // Parallel transport avoids frame flips through vertical tangents.
            const auto &previous = frames.back();
            const auto rotation = glm::cross(previous.axis, axis);
            const double cosine = glm::dot(previous.axis, axis);
            if (cosine <= -1 + 1e-8)
              return std::nullopt;
            x = previous.x + glm::cross(rotation, previous.x) +
                glm::cross(rotation, glm::cross(rotation, previous.x)) /
                    (1 + cosine);
            x = glm::normalize(x - axis * glm::dot(x, axis));
          }
          frames.push_back({point, axis, x, glm::cross(axis, x)});
        }
      }
      offset += piece.end;
    }
    if (frames.size() < 2)
      return std::nullopt;
    if (glm::length(frames.front().point - frames.back().point) <=
        1e-5 / unitScale_)
      return appendParametricSweptDiskGeometry(solid);
    const auto position = [](const Frame &f, const glm::vec3 &p) {
      return glm::vec3(f.point + f.x * double(p.x) + f.y * double(p.y));
    };
    std::vector<TriangleVertices> triangles;
    for (const auto &cap : *caps) {
      std::array<glm::vec3, 3> a{}, b{};
      for (size_t i = 0; i < 3; ++i) {
        a[i] = position(frames.front(), cap.positions[i]);
        b[i] = position(frames.back(), cap.positions[i]);
      }
      appendCapTriangle(triangles, a, glm::vec3(-frames.front().axis));
      appendCapTriangle(triangles, b, glm::vec3(frames.back().axis));
    }
    for (size_t j = 1; j < frames.size(); ++j)
      for (const auto &loop : loops)
        for (size_t i = 0; i < loop.size(); ++i) {
          const auto a = position(frames[j - 1], loop[i]),
                     b = position(frames[j - 1], loop[(i + 1) % loop.size()]);
          const auto c = position(frames[j], loop[(i + 1) % loop.size()]),
                     d = position(frames[j], loop[i]);
          triangles.push_back({{a, b, c}});
          triangles.push_back({{a, c, d}});
        }
    return appendTriangleMesh(triangles, styleColorByItem_.contains(solid.id)
                                             ? styleColorByItem_.at(solid.id)
                                             : defaultColor());
  }

  std::optional<MeshGroup>
  appendExtrudedAreaSolidGeometry(const Entity &solid) {
    const auto profileRef = firstRef(solid, 0);
    if (!profileRef.has_value()) {
      return std::nullopt;
    }

    auto profileLoops = readProfileLoops(*profileRef);
    auto capTriangles = triangulatePlanarLoops(profileLoops);
    if (!capTriangles)
      return std::nullopt;
    // IFC profiles lie in XY. Normalize ring winding so sides face outwards,
    // including the inside walls of voids, regardless of the authored winding.
    for (size_t ring = 0; ring < profileLoops.size(); ++ring) {
      auto &loop = profileLoops[ring];
      double signedArea = 0.0;
      for (size_t i = 0; i < loop.size(); ++i) {
        const auto &a = loop[i], &b = loop[(i + 1u) % loop.size()];
        if (std::abs(a.z) > 1e-7f)
          return std::nullopt;
        signedArea += double(a.x) * b.y - double(b.x) * a.y;
      }
      if ((signedArea > 0) != (ring == 0))
        std::ranges::reverse(loop);
    }
    const auto &profilePoints = profileLoops.front();

    const auto depth = numberValue(argAt(solid, 3));
    if (!depth.has_value() || !std::isfinite(*depth) || *depth <= 0.0) {
      return std::nullopt;
    }

    glm::mat4 solidPlacement(1.0f);
    if (const auto positionRef = firstRef(solid, 1); positionRef.has_value()) {
      solidPlacement = axis2Placement3D(*positionRef);
    }

    glm::vec3 direction(0.0f, 0.0f, 1.0f);
    if (const auto directionRef = firstRef(solid, 2);
        directionRef.has_value()) {
      direction = readDirection(*directionRef, direction);
    }
    const glm::vec3 extrusionVector = direction * static_cast<float>(*depth);
    if (!std::isfinite(extrusionVector.x) ||
        !std::isfinite(extrusionVector.y) ||
        !std::isfinite(extrusionVector.z) ||
        std::abs(extrusionVector.z) <= 1e-7f)
      return std::nullopt;
    if (extrusionVector.z < 0)
      for (auto &loop : profileLoops)
        std::ranges::reverse(loop);

    std::vector<glm::vec3> bottom;
    std::vector<glm::vec3> top;
    bottom.reserve(profilePoints.size());
    top.reserve(profilePoints.size());
    for (const auto &point : profilePoints) {
      const glm::vec3 base = glm::vec3(solidPlacement * glm::vec4(point, 1.0f));
      const glm::vec3 cap =
          glm::vec3(solidPlacement * glm::vec4(point + extrusionVector, 1.0f));
      bottom.push_back(base);
      top.push_back(cap);
    }

    glm::vec3 extrusionWorld = top.front() - bottom.front();
    if (glm::dot(extrusionWorld, extrusionWorld) <= 1.0e-12f) {
      return std::nullopt;
    }
    extrusionWorld = glm::normalize(extrusionWorld);

    std::vector<TriangleVertices> triangles;
    triangles.reserve((profilePoints.size() - 2u) * 2u +
                      profilePoints.size() * 2u);

    for (const auto &cap : *capTriangles) {
      std::array<glm::vec3, 3> base{}, end{};
      for (size_t i = 0; i < 3u; ++i) {
        base[i] = glm::vec3(solidPlacement * glm::vec4(cap.positions[i], 1.0f));
        end[i] = glm::vec3(solidPlacement *
                           glm::vec4(cap.positions[i] + extrusionVector, 1.0f));
      }
      appendCapTriangle(triangles, base, -extrusionWorld);
      appendCapTriangle(triangles, end, extrusionWorld);
    }

    for (const auto &loop : profileLoops) {
      for (size_t i = 0; i < loop.size(); ++i) {
        const size_t next = (i + 1u) % loop.size();
        const auto a = glm::vec3(solidPlacement * glm::vec4(loop[i], 1.0f));
        const auto b = glm::vec3(solidPlacement * glm::vec4(loop[next], 1.0f));
        const auto c = glm::vec3(solidPlacement *
                                 glm::vec4(loop[next] + extrusionVector, 1.0f));
        const auto d = glm::vec3(solidPlacement *
                                 glm::vec4(loop[i] + extrusionVector, 1.0f));
        triangles.push_back({{a, b, c}});
        triangles.push_back({{a, c, d}});
      }
    }

    const glm::vec4 color = styleColorByItem_.contains(solid.id)
                                ? styleColorByItem_.at(solid.id)
                                : defaultColor();
    if (auto boxSolid = makeBoxSolid(bottom, top, color);
        profileLoops.size() == 1u && boxSolid.has_value()) {
      boxSolidsByItem_[solid.id] = *boxSolid;
    }
    return appendTriangleMesh(triangles, color);
  }

  static void appendCapTriangle(std::vector<TriangleVertices> &triangles,
                                std::array<glm::vec3, 3> positions,
                                const glm::vec3 &expectedNormal) {
    const glm::vec3 normal =
        safeNormal(positions[0], positions[1], positions[2]);
    if (glm::dot(normal, expectedNormal) < 0.0f) {
      std::swap(positions[1], positions[2]);
    }
    triangles.push_back({positions});
  }

  std::optional<manifold::Manifold>
  solidFromGroups(std::span<const MeshGroup> groups) {
    manifold::MeshGL64 mesh;
    std::map<std::array<float, 3>, uint32_t> vertices;
    for (const auto &group : groups) {
      const auto range = std::ranges::find(model_.meshRanges, group.meshId,
                                           &dotbim::MeshRange::meshId);
      if (range == model_.meshRanges.end())
        return std::nullopt;
      const uint32_t original = manifold::Manifold::ReserveIDs(1);
      solidColors_[original] = group.color;
      mesh.runIndex.push_back(static_cast<uint32_t>(mesh.triVerts.size()));
      mesh.runOriginalID.push_back(original);
      for (size_t i = range->firstIndex;
           i < size_t(range->firstIndex) + range->indexCount; ++i) {
        const auto &p = model_.vertices[model_.indices[i]].position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          return std::nullopt;
        const std::array<float, 3> key{p.x, p.y, p.z};
        const auto [it, inserted] =
            vertices.try_emplace(key, static_cast<uint32_t>(vertices.size()));
        if (inserted)
          mesh.vertProperties.insert(mesh.vertProperties.end(),
                                     {double(p.x), double(p.y), double(p.z)});
        mesh.triVerts.push_back(it->second);
      }
    }
    if (mesh.triVerts.empty())
      return std::nullopt;
    mesh.runIndex.push_back(static_cast<uint32_t>(mesh.triVerts.size()));
    manifold::Manifold solid(mesh);
    if (solid.Status() != manifold::Manifold::Error::NoError)
      return std::nullopt;
    return solid;
  }

  std::optional<manifold::Manifold>
  evaluateSolid(uint32_t ref, std::unordered_set<uint32_t> &visiting) {
    if (const auto cached = solidCache_.find(ref); cached != solidCache_.end())
      return cached->second;
    if (failedSolids_.contains(ref))
      return std::nullopt;
    if (visiting.size() >= 128u || !visiting.insert(ref).second) {
      conversionErrors_[ref] = "Cyclic or excessive Boolean nesting";
      return std::nullopt;
    }
    struct VisitGuard {
      std::unordered_set<uint32_t> &set;
      uint32_t id;
      ~VisitGuard() { set.erase(id); }
    } guard{visiting, ref};
    const auto fail =
        [&](std::string reason) -> std::optional<manifold::Manifold> {
      conversionErrors_[ref] = std::move(reason);
      failedSolids_.insert(ref);
      return std::nullopt;
    };
    const auto *item = entity(ref);
    if (!item)
      return fail("Missing solid operand #" + std::to_string(ref));
    if (item->type != "IFCBOOLEANRESULT" &&
        item->type != "IFCBOOLEANCLIPPINGRESULT") {
      const auto groups = groupsByItem_.find(ref);
      if (groups == groupsByItem_.end())
        return fail("Unsupported or invalid solid operand " + item->type +
                    " #" + std::to_string(ref));
      auto result = solidFromGroups(groups->second);
      if (!result)
        return fail(
            "Solid operand is not a closed, consistently oriented manifold: " +
            item->type + " #" + std::to_string(ref));
      solidCache_.emplace(ref, *result);
      return result;
    }
    const auto operation = enumValue(argAt(*item, 0)).value_or("");
    if (operation != "UNION" && operation != "DIFFERENCE" &&
        operation != "INTERSECTION")
      return fail("Invalid Boolean operator");
    if (item->type == "IFCBOOLEANCLIPPINGRESULT" && operation != "DIFFERENCE")
      return fail("Clipping requires DIFFERENCE");
    const auto first = firstRef(*item, 1), second = firstRef(*item, 2);
    if (!first || !second)
      return fail("Missing Boolean operand reference");
    auto a = evaluateSolid(*first, visiting);
    if (!a)
      return fail("Failed first operand #" + std::to_string(*first) + ": " +
                  conversionErrors_[*first]);
    const auto *bEntity = entity(*second);
    std::optional<manifold::Manifold> result;
    if (bEntity && (bEntity->type == "IFCHALFSPACESOLID" ||
                    bEntity->type == "IFCBOXEDHALFSPACE" ||
                    bEntity->type == "IFCPOLYGONALBOUNDEDHALFSPACE")) {
      if (operation == "UNION")
        return fail("Union with an unbounded half-space has no bounded mesh");
      const auto planeRef = firstRef(*bEntity, 0);
      const auto *plane = planeRef ? entity(*planeRef) : nullptr;
      const auto positionRef = plane ? firstRef(*plane, 0) : std::nullopt;
      const auto *position = positionRef ? entity(*positionRef) : nullptr;
      const auto flag = enumValue(argAt(*bEntity, 1)).value_or("");
      const auto validPlacement = [&](const Entity *axis) {
        if (!axis || axis->type != "IFCAXIS2PLACEMENT3D")
          return false;
        const auto locationRef = firstRef(*axis, 0);
        const auto *location = locationRef ? entity(*locationRef) : nullptr;
        if (!location || location->type != "IFCCARTESIANPOINT" ||
            asList(argAt(*location, 0)).size() != 3 ||
            readNumberList(argAt(*location, 0)).size() != 3)
          return false;
        std::vector<glm::dvec3> directions;
        for (size_t attribute : {1u, 2u}) {
          const auto *value = argAt(*axis, attribute);
          if (!value)
            return false;
          if (value->kind == StepValue::Kind::Omitted)
            continue;
          const auto ref = firstRef(*axis, attribute);
          const auto *direction = ref ? entity(*ref) : nullptr;
          const auto d = direction ? readNumberList(argAt(*direction, 0))
                                   : std::vector<double>{};
          if (!direction || direction->type != "IFCDIRECTION" ||
              d.size() != 3 || asList(argAt(*direction, 0)).size() != 3)
            return false;
          const glm::dvec3 vector(d[0], d[1], d[2]);
          if (glm::length(vector) <= 1e-15)
            return false;
          directions.push_back(glm::normalize(vector));
        }
        return directions.size() < 2 ||
               glm::length(glm::cross(directions[0], directions[1])) > 1e-12;
      };
      if (!plane || plane->type != "IFCPLANE" || !position ||
          !validPlacement(position) || (flag != "T" && flag != "F"))
        return fail("Unsupported or invalid half-space plane #" +
                    std::to_string(*second));
      const auto placement = axis2Placement3D(*positionRef);
      glm::dvec3 normal(placement[2]), point(placement[3]);
      if (!std::isfinite(glm::length(normal)) || glm::length(normal) <= 1e-12 ||
          !std::isfinite(point.x) || !std::isfinite(point.y) ||
          !std::isfinite(point.z))
        return fail("Nonfinite half-space placement");
      normal = glm::normalize(normal);
      if (bEntity->type == "IFCBOXEDHALFSPACE") {
        const auto enclosureRef = firstRef(*bEntity, 2);
        const auto *box = enclosureRef ? entity(*enclosureRef) : nullptr;
        const auto cornerRef = box ? firstRef(*box, 0) : std::nullopt;
        const auto *corner = cornerRef ? entity(*cornerRef) : nullptr;
        if (!box || box->type != "IFCBOUNDINGBOX" || !corner ||
            corner->type != "IFCCARTESIANPOINT" ||
            asList(argAt(*corner, 0)).size() != 3 ||
            readNumberList(argAt(*corner, 0)).size() != 3)
          return fail("Invalid boxed half-space enclosure");
        for (size_t axis : {1u, 2u, 3u}) {
          auto dimension = numberValue(argAt(*box, axis));
          if (!dimension || !std::isfinite(*dimension) || *dimension <= 0)
            return fail("Invalid boxed half-space enclosure dimensions");
        }
        // Enclosure is a search hint, not an extra clipping boundary.
      }
      if (bEntity->type == "IFCPOLYGONALBOUNDEDHALFSPACE") {
        const auto boundaryPlacementRef = firstRef(*bEntity, 2),
                   boundaryRef = firstRef(*bEntity, 3);
        const auto *boundaryPlacement =
            boundaryPlacementRef ? entity(*boundaryPlacementRef) : nullptr;
        if (!validPlacement(boundaryPlacement) || !boundaryRef)
          return fail("Invalid polygonal half-space boundary placement");
        const auto boundaryMatrix = axis2Placement3D(*boundaryPlacementRef);
        const glm::dvec3 x(boundaryMatrix[0]), y(boundaryMatrix[1]),
            z(boundaryMatrix[2]), origin(boundaryMatrix[3]);
        if (!std::isfinite(glm::dot(normal, z)) ||
            std::abs(glm::dot(normal, z)) <= 1e-10)
          return fail(
              "Polygonal half-space plane is parallel to its extrusion axis");
        std::string error;
        auto boundary =
            detail::readIfcCurve(entities_, *boundaryRef, unitScale_,
                                 projectPlaneAngleScale(), error);
        if (!boundary || boundary->dimension != 2 || !boundary->domain)
          return fail("Invalid polygonal half-space boundary: " + error);
        auto samples =
            sampleCurve(*boundary, boundary->domain->first,
                        boundary->domain->second, .001 / unitScale_, 4097);
        if (samples.size() < 4 ||
            glm::length(samples.front().point - samples.back().point) >
                1e-5 / unitScale_)
          return fail("Polygonal half-space boundary must be closed within its "
                      "sample budget");
        samples.pop_back();
        std::vector<glm::vec3> loop;
        manifold::SimplePolygon polygon;
        for (const auto &sample : samples) {
          loop.emplace_back(sample.point);
          polygon.push_back({sample.point.x, sample.point.y});
        }
        const auto boundaryTriangles = triangulatePlanarLoops({loop});
        if (!boundaryTriangles || boundaryTriangles->empty())
          return fail("Invalid polygonal half-space boundary topology");
        double area = 0;
        for (size_t i = 0; i < polygon.size(); ++i) {
          const auto &p = polygon[i], &q = polygon[(i + 1) % polygon.size()];
          area += p.x * q.y - p.y * q.x;
        }
        if (area < 0)
          std::ranges::reverse(polygon);
        if (a->IsEmpty()) {
          result = *a;
        } else {
          const auto bounds = a->BoundingBox();
          double low = std::numeric_limits<double>::infinity(), high = -low;
          for (size_t corner = 0; corner < 8; ++corner) {
            glm::dvec3 p;
            for (size_t axis = 0; axis < 3; ++axis)
              p[axis] =
                  (corner & (1u << axis)) ? bounds.max[axis] : bounds.min[axis];
            const double station = glm::dot(p - origin, z);
            low = std::min(low, station);
            high = std::max(high, station);
          }
          const double margin = std::max(.001 / unitScale_, (high - low) * .1);
          low -= margin;
          high += margin;
          if (!std::isfinite(low) || !std::isfinite(high) || low >= high)
            return fail("Polygonal half-space extrusion extent is invalid");
          auto cutter = manifold::Manifold::Extrude({polygon}, high - low);
          const auto vector = [](glm::dvec3 p) {
            return manifold::vec3(p.x, p.y, p.z);
          };
          cutter = cutter.Transform(
              manifold::mat3x4(manifold::mat3(vector(x), vector(y), vector(z)),
                               vector(origin + z * low)));
          const glm::dvec3 materialNormal = normal * (flag == "F" ? 1. : -1.);
          cutter = cutter.TrimByPlane(vector(materialNormal),
                                      glm::dot(materialNormal, point));
          if (cutter.Status() != manifold::Manifold::Error::NoError)
            return fail("Boolean kernel rejected the polygonal half-space");
          result = a->Boolean(cutter, operation == "DIFFERENCE"
                                          ? manifold::OpType::Subtract
                                          : manifold::OpType::Intersect);
        }
      } else {
        // IFC TRUE puts material opposite the plane normal. TrimByPlane keeps
        // the positive side; DIFFERENCE keeps the complement of the half-space.
        const double sign =
            ((flag == "F") == (operation == "INTERSECTION")) ? 1.0 : -1.0;
        normal *= sign;
        result = a->TrimByPlane({normal.x, normal.y, normal.z},
                                glm::dot(normal, point));
      }
      const auto inputMesh = a->GetMeshGL64();
      const auto color =
          !inputMesh.runOriginalID.empty() &&
                  solidColors_.contains(inputMesh.runOriginalID.front())
              ? solidColors_.at(inputMesh.runOriginalID.front())
              : defaultColor();
      for (auto original : result->GetMeshGL64().runOriginalID)
        solidColors_.try_emplace(original, color);
    } else {
      auto b = evaluateSolid(*second, visiting);
      if (!b)
        return fail("Failed second operand #" + std::to_string(*second) + ": " +
                    conversionErrors_[*second]);
      result = a->Boolean(*b, operation == "UNION" ? manifold::OpType::Add
                              : operation == "DIFFERENCE"
                                  ? manifold::OpType::Subtract
                                  : manifold::OpType::Intersect);
    }
    if (result->Status() != manifold::Manifold::Error::NoError)
      return fail("Boolean kernel rejected the result");
    if (styleColorByItem_.contains(ref)) {
      *result = result->AsOriginal();
      solidColors_[static_cast<uint32_t>(result->OriginalID())] =
          styleColorByItem_.at(ref);
    }
    solidCache_.emplace(ref, *result);
    return result;
  }

  std::vector<MeshGroup>
  appendSolidMesh(const manifold::Manifold &solid,
                  std::optional<glm::vec4> overrideColor = std::nullopt,
                  const std::unordered_set<uint32_t> *cutOrigins = nullptr,
                  std::optional<glm::vec4> cutColor = std::nullopt) {
    const auto mesh = solid.GetMeshGL64();
    std::map<uint32_t, std::pair<glm::vec4, std::vector<TriangleVertices>>>
        groups;
    for (size_t run = 0; run < mesh.runOriginalID.size(); ++run) {
      const auto original = mesh.runOriginalID[run];
      const auto color = overrideColor.value_or(
          cutOrigins && cutOrigins->contains(original) && cutColor ? *cutColor
          : solidColors_.contains(original) ? solidColors_.at(original)
                                            : defaultColor());
      auto &group = groups[packColor(color)];
      group.first = color;
      for (size_t i = mesh.runIndex[run]; i < mesh.runIndex[run + 1]; i += 3) {
        TriangleVertices triangle;
        for (size_t j = 0; j < 3; ++j) {
          const size_t vertex = size_t(mesh.triVerts[i + j]) * mesh.numProp;
          triangle.positions[j] = {mesh.vertProperties[vertex],
                                   mesh.vertProperties[vertex + 1],
                                   mesh.vertProperties[vertex + 2]};
        }
        // Boolean intersections can leave sliver faces that collapse to a
        // line at renderer float precision. They have no drawable surface.
        const auto cross = glm::cross(glm::dvec3(triangle.positions[1]) -
                                          glm::dvec3(triangle.positions[0]),
                                      glm::dvec3(triangle.positions[2]) -
                                          glm::dvec3(triangle.positions[0]));
        if (glm::dot(cross, cross) == 0)
          continue;
        group.second.push_back(triangle);
      }
    }
    std::vector<MeshGroup> result;
    for (const auto &[_, group] : groups)
      if (auto meshGroup = appendTriangleMesh(group.second, group.first))
        result.push_back(*meshGroup);
    return result;
  }

  static bool hasPositiveSpan(const BoxSolid &box) {
    for (AxisIndex axis = 0; axis < 3; ++axis) {
      if (box.maxBounds[axis] - box.minBounds[axis] <= 1.0e-4f) {
        return false;
      }
    }
    return true;
  }

  static std::optional<BoxSolid> makeBoxSolid(std::span<const glm::vec3> bottom,
                                              std::span<const glm::vec3> top,
                                              glm::vec4 color) {
    if (bottom.size() != 4u || top.size() != 4u) {
      return std::nullopt;
    }

    BoxSolid box{};
    box.minBounds = glm::vec3(std::numeric_limits<float>::max());
    box.maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    box.color = sanitizeColor(color);

    for (const auto points : {bottom, top}) {
      for (const auto &point : points) {
        for (AxisIndex axis = 0; axis < 3; ++axis) {
          if (!std::isfinite(point[axis])) {
            return std::nullopt;
          }
        }
        box.minBounds = glm::min(box.minBounds, point);
        box.maxBounds = glm::max(box.maxBounds, point);
      }
    }
    if (!hasPositiveSpan(box)) {
      return std::nullopt;
    }

    std::array<bool, 8> seenCorners{};
    for (const auto points : {bottom, top}) {
      for (const auto &point : points) {
        uint32_t cornerMask = 0;
        for (AxisIndex axis = 0; axis < 3; ++axis) {
          const float toMin = std::abs(point[axis] - box.minBounds[axis]);
          const float toMax = std::abs(point[axis] - box.maxBounds[axis]);
          if (toMin <= 1.0e-4f) {
            continue;
          }
          if (toMax <= 1.0e-4f) {
            cornerMask |= (1u << static_cast<uint32_t>(axis));
            continue;
          }
          return std::nullopt;
        }
        seenCorners[cornerMask] = true;
      }
    }

    if (!std::ranges::all_of(seenCorners, [](bool seen) { return seen; })) {
      return std::nullopt;
    }
    return box;
  }

  static std::array<glm::vec3, 8> boxCorners(const BoxSolid &box) {
    std::array<glm::vec3, 8> corners{};
    for (uint32_t mask = 0; mask < 8u; ++mask) {
      glm::vec3 corner{};
      for (AxisIndex axis = 0; axis < 3; ++axis) {
        const uint32_t axisBit = 1u << static_cast<uint32_t>(axis);
        corner[axis] =
            (mask & axisBit) != 0u ? box.maxBounds[axis] : box.minBounds[axis];
      }
      corners[mask] = corner;
    }
    return corners;
  }

  static BoxSolid transformBox(const BoxSolid &box,
                               const glm::mat4 &transform) {
    BoxSolid result{};
    result.minBounds = glm::vec3(std::numeric_limits<float>::max());
    result.maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    result.color = box.color;
    for (const auto &corner : boxCorners(box)) {
      const glm::vec3 transformed =
          glm::vec3(transform * glm::vec4(corner, 1.0f));
      result.minBounds = glm::min(result.minBounds, transformed);
      result.maxBounds = glm::max(result.maxBounds, transformed);
    }
    return result;
  }

  static std::optional<BoxSolid> intersectBoxes(const BoxSolid &a,
                                                const BoxSolid &b) {
    BoxSolid result{};
    result.minBounds = glm::max(a.minBounds, b.minBounds);
    result.maxBounds = glm::min(a.maxBounds, b.maxBounds);
    result.color = a.color;
    if (!hasPositiveSpan(result)) {
      return std::nullopt;
    }
    return result;
  }

  static glm::vec3 axisDirection(AxisIndex axis, float sign) {
    glm::vec3 direction(0.0f);
    direction[axis] = sign;
    return direction;
  }

  static std::array<AxisIndex, 2> remainingAxes(AxisIndex axis) {
    if (axis == 0) {
      return {1, 2};
    }
    if (axis == 1) {
      return {0, 2};
    }
    return {0, 1};
  }

  static std::optional<AxisIndex> throughAxisForCut(const BoxSolid &host,
                                                    const BoxSolid &cut) {
    constexpr float kEpsilon = 1.0e-4f;
    for (AxisIndex axis = 0; axis < 3; ++axis) {
      if (std::abs(cut.minBounds[axis] - host.minBounds[axis]) > kEpsilon ||
          std::abs(cut.maxBounds[axis] - host.maxBounds[axis]) > kEpsilon) {
        continue;
      }

      const auto [firstAxis, secondAxis] = remainingAxes(axis);
      if (cut.minBounds[firstAxis] <= host.minBounds[firstAxis] + kEpsilon ||
          cut.maxBounds[firstAxis] >= host.maxBounds[firstAxis] - kEpsilon ||
          cut.minBounds[secondAxis] <= host.minBounds[secondAxis] + kEpsilon ||
          cut.maxBounds[secondAxis] >= host.maxBounds[secondAxis] - kEpsilon) {
        continue;
      }
      return axis;
    }
    return std::nullopt;
  }

  static glm::vec3 pointOnPlane(AxisIndex fixedAxis, float fixedValue,
                                AxisIndex axisA, float valueA, AxisIndex axisB,
                                float valueB) {
    glm::vec3 point(0.0f);
    point[fixedAxis] = fixedValue;
    point[axisA] = valueA;
    point[axisB] = valueB;
    return point;
  }

  static void appendRectOnPlane(std::vector<TriangleVertices> &triangles,
                                AxisIndex fixedAxis, float fixedValue,
                                AxisIndex axisA, float minA, float maxA,
                                AxisIndex axisB, float minB, float maxB,
                                const glm::vec3 &expectedNormal) {
    if (maxA - minA <= 1.0e-4f || maxB - minB <= 1.0e-4f) {
      return;
    }

    const glm::vec3 p00 =
        pointOnPlane(fixedAxis, fixedValue, axisA, minA, axisB, minB);
    const glm::vec3 p10 =
        pointOnPlane(fixedAxis, fixedValue, axisA, maxA, axisB, minB);
    const glm::vec3 p11 =
        pointOnPlane(fixedAxis, fixedValue, axisA, maxA, axisB, maxB);
    const glm::vec3 p01 =
        pointOnPlane(fixedAxis, fixedValue, axisA, minA, axisB, maxB);

    appendCapTriangle(triangles, {p00, p10, p11}, expectedNormal);
    appendCapTriangle(triangles, {p00, p11, p01}, expectedNormal);
  }

  static void appendThroughCapWithHole(std::vector<TriangleVertices> &triangles,
                                       const BoxSolid &host,
                                       const BoxSolid &cut,
                                       AxisIndex throughAxis,
                                       float throughValue,
                                       const glm::vec3 &expectedNormal) {
    const auto [axisA, axisB] = remainingAxes(throughAxis);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      host.minBounds[axisA], cut.minBounds[axisA], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      expectedNormal);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      cut.maxBounds[axisA], host.maxBounds[axisA], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      expectedNormal);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      cut.minBounds[axisA], cut.maxBounds[axisA], axisB,
                      host.minBounds[axisB], cut.minBounds[axisB],
                      expectedNormal);
    appendRectOnPlane(triangles, throughAxis, throughValue, axisA,
                      cut.minBounds[axisA], cut.maxBounds[axisA], axisB,
                      cut.maxBounds[axisB], host.maxBounds[axisB],
                      expectedNormal);
  }

  std::optional<MeshGroup> appendBoxWithSingleThroughVoid(const BoxSolid &host,
                                                          const BoxSolid &cut) {
    const auto throughAxis = throughAxisForCut(host, cut);
    if (!throughAxis.has_value()) {
      return std::nullopt;
    }

    const auto [axisA, axisB] = remainingAxes(*throughAxis);
    std::vector<TriangleVertices> triangles;
    triangles.reserve(32u);

    appendThroughCapWithHole(triangles, host, cut, *throughAxis,
                             host.minBounds[*throughAxis],
                             axisDirection(*throughAxis, -1.0f));
    appendThroughCapWithHole(triangles, host, cut, *throughAxis,
                             host.maxBounds[*throughAxis],
                             axisDirection(*throughAxis, 1.0f));

    appendRectOnPlane(triangles, axisA, host.minBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      axisDirection(axisA, -1.0f));
    appendRectOnPlane(triangles, axisA, host.maxBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB,
                      host.minBounds[axisB], host.maxBounds[axisB],
                      axisDirection(axisA, 1.0f));
    appendRectOnPlane(triangles, axisB, host.minBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA,
                      host.minBounds[axisA], host.maxBounds[axisA],
                      axisDirection(axisB, -1.0f));
    appendRectOnPlane(triangles, axisB, host.maxBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA,
                      host.minBounds[axisA], host.maxBounds[axisA],
                      axisDirection(axisB, 1.0f));

    appendRectOnPlane(triangles, axisA, cut.minBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB, cut.minBounds[axisB],
                      cut.maxBounds[axisB], axisDirection(axisA, 1.0f));
    appendRectOnPlane(triangles, axisA, cut.maxBounds[axisA], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisB, cut.minBounds[axisB],
                      cut.maxBounds[axisB], axisDirection(axisA, -1.0f));
    appendRectOnPlane(triangles, axisB, cut.minBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA, cut.minBounds[axisA],
                      cut.maxBounds[axisA], axisDirection(axisB, 1.0f));
    appendRectOnPlane(triangles, axisB, cut.maxBounds[axisB], *throughAxis,
                      host.minBounds[*throughAxis],
                      host.maxBounds[*throughAxis], axisA, cut.minBounds[axisA],
                      cut.maxBounds[axisA], axisDirection(axisB, -1.0f));

    return appendTriangleMesh(triangles, host.color);
  }

  std::optional<MeshGroup>
  appendBoxWithThroughVoids(const BoxSolid &host,
                            std::span<const BoxSolid> cuts) {
    if (cuts.empty())
      return std::nullopt;
    if (cuts.size() == 1u)
      return appendBoxWithSingleThroughVoid(host, cuts.front());
    const auto throughAxis = throughAxisForCut(host, cuts.front());
    if (!throughAxis)
      return std::nullopt;
    for (const auto &cut : cuts)
      if (throughAxisForCut(host, cut) != throughAxis)
        return std::nullopt;
    const auto [axisA, axisB] = remainingAxes(*throughAxis);
    auto rectangle = [&](const BoxSolid &box, float fixed) {
      return std::vector<glm::vec3>{
          pointOnPlane(*throughAxis, fixed, axisA, box.minBounds[axisA], axisB,
                       box.minBounds[axisB]),
          pointOnPlane(*throughAxis, fixed, axisA, box.maxBounds[axisA], axisB,
                       box.minBounds[axisB]),
          pointOnPlane(*throughAxis, fixed, axisA, box.maxBounds[axisA], axisB,
                       box.maxBounds[axisB]),
          pointOnPlane(*throughAxis, fixed, axisA, box.minBounds[axisA], axisB,
                       box.maxBounds[axisB])};
    };
    std::vector<TriangleVertices> triangles;
    for (const float sign : {-1.0f, 1.0f}) {
      const float fixed = sign < 0 ? host.minBounds[*throughAxis]
                                   : host.maxBounds[*throughAxis];
      std::vector<std::vector<glm::vec3>> loops{rectangle(host, fixed)};
      for (const auto &cut : cuts)
        loops.push_back(rectangle(cut, fixed));
      const auto cap = triangulatePlanarLoops(std::move(loops));
      if (!cap)
        return std::nullopt; // Includes intersecting/nested openings.
      for (const auto &triangle : *cap)
        appendCapTriangle(triangles, triangle.positions,
                          axisDirection(*throughAxis, sign));
    }
    auto appendSides = [&](const BoxSolid &box, float outward) {
      for (const float sign : {-1.0f, 1.0f}) {
        appendRectOnPlane(
            triangles, axisA,
            sign < 0 ? box.minBounds[axisA] : box.maxBounds[axisA],
            *throughAxis, host.minBounds[*throughAxis],
            host.maxBounds[*throughAxis], axisB, box.minBounds[axisB],
            box.maxBounds[axisB], axisDirection(axisA, sign * outward));
        appendRectOnPlane(
            triangles, axisB,
            sign < 0 ? box.minBounds[axisB] : box.maxBounds[axisB],
            *throughAxis, host.minBounds[*throughAxis],
            host.maxBounds[*throughAxis], axisA, box.minBounds[axisA],
            box.maxBounds[axisA], axisDirection(axisB, sign * outward));
      }
    };
    appendSides(host, 1.0f);
    for (const auto &cut : cuts)
      appendSides(cut, -1.0f);
    return appendTriangleMesh(triangles, host.color);
  }

  void collectGeometryInstances(uint32_t ref, const glm::mat4 &transform,
                                std::vector<GeometryInstance> &instances,
                                const Entity *product = nullptr) {
    collectGeometryInstances(ref, transform, instances, {}, product);
  }

  void
  collectGeometryInstances(uint32_t ref, const glm::mat4 &transform,
                           std::vector<GeometryInstance> &instances,
                           std::unordered_set<uint32_t> visiting,
                           const Entity *product,
                           std::optional<glm::vec4> curveColor = std::nullopt) {
    if (!visiting.insert(ref).second) {
      reportGeometryIssue(ref, product, "Cyclic representation reference");
      return;
    }
    const Entity *source = entity(ref);
    if (source == nullptr) {
      reportGeometryIssue(ref, product, "Missing representation entity");
      return;
    }

    if (const auto style = styleColorByItem_.find(ref);
        style != styleColorByItem_.end())
      curveColor = style->second;

    if (detail::isCurveEntity(source->type)) {
      if (!groupsByItem_.contains(ref) && !conversionErrors_.contains(ref)) {
        std::string error;
        const auto points = readRenderCurve(ref, error);
        const std::array<size_t, 1> counts{points.size()};
        if (appendPolylineGeometry(model_, points, counts, nextMeshId_,
                                   .005f / unitScale_)) {
          groupsByItem_[ref] = {{nextMeshId_++,
                                 styleColorByItem_.contains(ref)
                                     ? styleColorByItem_.at(ref)
                                     : defaultColor(),
                                 dotbim::GeometryKind::Curves}};
        } else {
          conversionErrors_[ref] =
              error.empty() ? "Invalid curve points/indices, disconnected "
                              "segments, or excessive curve tessellation"
                            : error;
        }
      }
      if (groupsByItem_.contains(ref))
        instances.push_back({ref, transform, curveColor});
      else
        reportGeometryIssue(ref, product, conversionErrors_.at(ref));
      return;
    }

    if (source->type == "IFCGEOMETRICCURVESET") {
      const auto *items = argAt(*source, 0);
      if (!items || items->kind != StepValue::Kind::List ||
          items->list.empty()) {
        reportGeometryIssue(ref, product, "Missing geometric curve-set items");
        return;
      }
      for (const auto &item : items->list) {
        if (item.kind == StepValue::Kind::Ref)
          collectGeometryInstances(item.ref, transform, instances, visiting,
                                   product, curveColor);
        else
          reportGeometryIssue(ref, product,
                              "Invalid geometric curve-set item reference");
      }
      return;
    }
    if (source->type == "IFCBOOLEANRESULT" ||
        source->type == "IFCBOOLEANCLIPPINGRESULT") {
      if (!groupsByItem_.contains(ref) && !failedSolids_.contains(ref)) {
        std::unordered_set<uint32_t> operands;
        if (auto solid = evaluateSolid(ref, operands)) {
          auto groups = appendSolidMesh(
              *solid, styleColorByItem_.contains(ref)
                          ? std::optional(styleColorByItem_.at(ref))
                          : std::nullopt);
          if (!groups.empty())
            groupsByItem_[ref] = std::move(groups);
          else
            conversionErrors_[ref] =
                "Boolean result contains no drawable geometry";
        }
      }
      if (groupsByItem_.contains(ref))
        instances.push_back({ref, transform, curveColor});
      else
        reportGeometryIssue(ref, product,
                            conversionErrors_.contains(ref)
                                ? conversionErrors_.at(ref)
                                : "Boolean conversion failed");
      return;
    }
    if (source->type == "IFCSECTIONEDSURFACE" && !groupsByItem_.contains(ref) &&
        !conversionErrors_.contains(ref)) {
      std::string error;
      auto points = detail::readIfcSectionedSurfaceMesh(
          entities_, ref, unitScale_, projectPlaneAngleScale(), error);
      if (!points.empty()) {
        std::vector<TriangleVertices> triangles;
        triangles.reserve(points.size());
        for (const auto &t : points)
          triangles.push_back(
              {{glm::vec3(t[0]), glm::vec3(t[1]), glm::vec3(t[2])}});
        if (auto group =
                appendTriangleMesh(triangles, styleColorByItem_.contains(ref)
                                                  ? styleColorByItem_.at(ref)
                                                  : defaultColor()))
          groupsByItem_[ref] = {*group};
        else
          error =
              "Sectioned surface collapses at renderer coordinate precision";
      }
      if (!groupsByItem_.contains(ref))
        conversionErrors_[ref] = error;
    }
    if (source->type == "IFCTRIANGULATEDFACESET" ||
        source->type == "IFCPOLYGONALFACESET" ||
        source->type == "IFCFACETEDBREP" ||
        source->type == "IFCFACETEDBREPWITHVOIDS" ||
        source->type == "IFCADVANCEDBREP" ||
        source->type == "IFCADVANCEDBREPWITHVOIDS" ||
        source->type == "IFCEXTRUDEDAREASOLID" ||
        source->type == "IFCSWEPTDISKSOLID" ||
        source->type == "IFCSWEPTDISKSOLIDPOLYGONAL" ||
        source->type == "IFCSECTIONEDSURFACE") {
      if (groupsByItem_.contains(ref))
        instances.push_back({ref, transform, curveColor});
      else {
        std::string reason = conversionErrors_.contains(ref)
                                 ? conversionErrors_.at(ref)
                                 : "Invalid or unsupported shape data";
        if (source->type == "IFCEXTRUDEDAREASOLID") {
          if (const auto profileRef = firstRef(*source, 0)) {
            const auto *profile = entity(*profileRef);
            reason +=
                "; profile " +
                (profile ? profile->type : std::string("MISSING_ENTITY")) +
                " #" + std::to_string(*profileRef);
          }
        }
        reportGeometryIssue(ref, product, std::move(reason));
      }
      return;
    }

    if (source->type == "IFCPRODUCTDEFINITIONSHAPE") {
      for (const auto &rep : asList(argAt(*source, 2))) {
        if (rep.kind == StepValue::Kind::Ref)
          collectGeometryInstances(rep.ref, transform, instances, visiting,
                                   product, curveColor);
        else
          reportGeometryIssue(ref, product, "Invalid representation reference");
      }
      return;
    }

    if (source->type == "IFCSHAPEREPRESENTATION") {
      for (const auto &item : asList(argAt(*source, 3))) {
        if (item.kind == StepValue::Kind::Ref)
          collectGeometryInstances(item.ref, transform, instances, visiting,
                                   product, curveColor);
        else
          reportGeometryIssue(ref, product,
                              "Invalid representation item reference");
      }
      return;
    }

    if (source->type == "IFCMAPPEDITEM") {
      const auto sourceRef = firstRef(*source, 0);
      const auto targetRef = firstRef(*source, 1);
      if (!sourceRef.has_value()) {
        reportGeometryIssue(ref, product, "Missing mapped representation");
        return;
      }
      const glm::mat4 target =
          targetRef.has_value() ? cartesianTransformationOperator3D(*targetRef)
                                : glm::mat4(1.0f);
      collectGeometryInstances(*sourceRef, transform * target, instances,
                               visiting, product, curveColor);
      return;
    }

    if (source->type == "IFCREPRESENTATIONMAP") {
      const auto mappedRepRef = firstRef(*source, 1);
      if (!mappedRepRef.has_value()) {
        reportGeometryIssue(ref, product, "Missing mapped representation");
        return;
      }
      glm::mat4 origin(1.0f);
      if (const auto originRef = firstRef(*source, 0); originRef.has_value()) {
        origin = axis2Placement3D(*originRef);
      }
      collectGeometryInstances(*mappedRepRef, transform * glm::inverse(origin),
                               instances, visiting, product, curveColor);
      return;
    }

    if (source->type == "IFCSTYLEDITEM") {
      if (const auto itemRef = firstRef(*source, 0); itemRef.has_value()) {
        collectGeometryInstances(*itemRef, transform, instances, visiting,
                                 product, curveColor);
      }
      return;
    }
    reportGeometryIssue(ref, product, "Unsupported shape representation");
  }

  void reportGeometryIssue(uint32_t ref, const Entity *product,
                           std::string reason) {
    if (product == nullptr)
      return;
    const Entity *item = entity(ref);
    auto &diagnostics = model_.importReport.diagnostics;
    const uint64_t key = (uint64_t(product->id) << 32u) | ref;
    if (!diagnosedItems_.insert(key).second)
      return;
    diagnostics.push_back(
        {item ? item->type : "MISSING_ENTITY", ref, product->id,
         stringValue(argAt(*product, 0)).value_or(""), std::move(reason)});
  }

  void finishImportReport() {
    auto &report = model_.importReport;
    report.completeness = model_.elements.empty() ? ImportCompleteness::Failed
                          : report.diagnostics.empty()
                              ? ImportCompleteness::Complete
                              : ImportCompleteness::Partial;
    for (const auto &d : report.diagnostics)
      ++report.representationWarnings[d.representationType];
    // Raw geometry fixtures have no products; color groups are not products.
  }

  bool appendVoidedProductElement(const Entity &product,
                                  const glm::mat4 &placement,
                                  std::span<const GeometryInstance> instances,
                                  const ProductMetadata &metadata,
                                  const glm::mat4 &unitTransform) {
    const auto openingIt = openingsByHostProduct_.find(product.id);
    if (openingIt == openingsByHostProduct_.end() ||
        openingIt->second.empty() || instances.size() != 1u) {
      return false;
    }

    const GeometryInstance &hostInstance = instances.front();
    const auto hostBoxIt = boxSolidsByItem_.find(hostInstance.geometryId);
    if (hostBoxIt == boxSolidsByItem_.end()) {
      return false;
    }
    const BoxSolid &hostBox = hostBoxIt->second;

    std::vector<BoxSolid> cuts;
    for (uint32_t openingId : openingIt->second) {
      const Entity *opening = entity(openingId);
      if (opening == nullptr || opening->type != "IFCOPENINGELEMENT") {
        return false;
      }
      const auto openingPlacementRef = firstRef(*opening, 5);
      const auto openingRepresentationRef = firstRef(*opening, 6);
      if (!openingRepresentationRef.has_value()) {
        return false;
      }

      std::vector<GeometryInstance> openingInstances;
      collectGeometryInstances(*openingRepresentationRef, glm::mat4(1.0f),
                               openingInstances);
      std::erase_if(openingInstances, [&](const auto &instance) {
        return groupsByItem_.at(instance.geometryId).front().geometryKind !=
               dotbim::GeometryKind::Mesh;
      });
      if (openingInstances.size() != 1u) {
        return false;
      }
      const auto openingBoxIt =
          boxSolidsByItem_.find(openingInstances.front().geometryId);
      if (openingBoxIt == boxSolidsByItem_.end()) {
        return false;
      }

      const glm::mat4 openingPlacement =
          openingPlacementRef.has_value() ? localPlacement(*openingPlacementRef)
                                          : glm::mat4(1.0f);
      const glm::mat4 openingToHostGeometry =
          glm::inverse(hostInstance.transform) * glm::inverse(placement) *
          openingPlacement * openingInstances.front().transform;
      // Transforming a rotated box to its AABB would over-cut the host. Only
      // axis-preserving transforms can use this analytic subtraction path.
      for (int column = 0; column < 3; ++column) {
        const auto axis = glm::vec3(openingToHostGeometry[column]);
        const float length = glm::length(axis);
        if (!std::isfinite(length) || length <= 1e-12f)
          return false;
        int nonzero = 0;
        for (int row = 0; row < 3; ++row)
          if (std::abs(axis[row]) > length * 1e-5f)
            ++nonzero;
        if (nonzero != 1)
          return false;
      }
      const BoxSolid openingBox =
          transformBox(openingBoxIt->second, openingToHostGeometry);
      const auto cutBox = intersectBoxes(hostBox, openingBox);
      if (!cutBox.has_value()) {
        return false;
      }
      cuts.push_back(*cutBox);
    }

    auto group = appendBoxWithThroughVoids(hostBox, cuts);
    if (!group.has_value()) {
      return false;
    }

    container::geometry::dotbim::Element element{};
    element.meshId = group->meshId;
    element.transform = unitTransform * placement * hostInstance.transform;
    element.color = group->color;
    element.type = product.type;
    applyProductMetadata(element, metadata);
    model_.elements.push_back(std::move(element));
    return true;
  }

  static std::optional<manifold::mat3x4>
  solidTransform(const glm::mat4 &matrix) {
    manifold::mat3x4 result;
    for (int column = 0; column < 4; ++column)
      for (int row = 0; row < 3; ++row) {
        if (!std::isfinite(matrix[column][row]))
          return std::nullopt;
        result[column][row] = matrix[column][row];
      }
    if (std::abs(glm::determinant(glm::mat3(matrix))) < 1e-12f)
      return std::nullopt;
    return result;
  }

  bool appendKernelVoidedProduct(const Entity &product,
                                 const glm::mat4 &placement,
                                 const std::vector<GeometryInstance> &instances,
                                 const ProductMetadata &metadata,
                                 const glm::mat4 &unitTransform) {
    const auto openings = openingsByHostProduct_.find(product.id);
    if (openings == openingsByHostProduct_.end() || instances.empty())
      return false;
    std::vector<manifold::Manifold> cuts;
    std::unordered_set<uint32_t> cutOrigins;
    std::unordered_set<uint32_t> visiting;
    const auto inverseHost = glm::inverse(placement);
    for (auto openingId : openings->second) {
      const auto *opening = entity(openingId);
      if (!opening || opening->type != "IFCOPENINGELEMENT")
        return false;
      const auto rep = firstRef(*opening, 6), location = firstRef(*opening, 5);
      if (!rep)
        return false;
      std::vector<GeometryInstance> openingInstances;
      const size_t warningStart = model_.importReport.diagnostics.size();
      collectGeometryInstances(*rep, glm::mat4(1), openingInstances, &product);
      std::erase_if(openingInstances, [&](const auto &instance) {
        return groupsByItem_.at(instance.geometryId).front().geometryKind !=
               dotbim::GeometryKind::Mesh;
      });
      if (openingInstances.empty() ||
          model_.importReport.diagnostics.size() != warningStart)
        return false;
      const auto openingPlacement =
          location ? localPlacement(*location) : glm::mat4(1);
      for (const auto &instance : openingInstances) {
        auto cut = evaluateSolid(instance.geometryId, visiting);
        const auto transform =
            solidTransform(inverseHost * openingPlacement * instance.transform);
        if (!cut || !transform)
          return false;
        for (auto original : cut->GetMeshGL64().runOriginalID)
          cutOrigins.insert(original);
        cuts.push_back(cut->Transform(*transform));
      }
    }
    std::vector<std::pair<manifold::Manifold, glm::vec4>> results;
    for (const auto &instance : instances) {
      auto host = evaluateSolid(instance.geometryId, visiting);
      const auto transform = solidTransform(instance.transform);
      if (!host || !transform)
        return false;
      *host = host->Transform(*transform);
      for (const auto &cut : cuts)
        *host = host->Boolean(cut, manifold::OpType::Subtract);
      if (host->Status() != manifold::Manifold::Error::NoError)
        return false;
      const auto &groups = groupsByItem_.at(instance.geometryId);
      results.emplace_back(*host, groups.empty() ? defaultColor()
                                                 : groups.front().color);
    }
    const size_t start = model_.elements.size();
    // Append only after every cut succeeds so failed operations retain the
    // original host atomically. New cut surfaces inherit the host material.
    for (const auto &[solid, color] : results)
      for (const auto &group :
           appendSolidMesh(solid, std::nullopt, &cutOrigins, color)) {
        dotbim::Element element{};
        element.meshId = group.meshId;
        element.color = group.color;
        element.transform = unitTransform * placement;
        element.type = product.type;
        applyProductMetadata(element, metadata);
        model_.elements.push_back(std::move(element));
      }
    if (model_.elements.size() == start)
      reportGeometryIssue(
          product.id, &product,
          "Opening subtraction removed all drawable host geometry");
    return true;
  }

  void appendProductElements() {
    const glm::mat4 unitTransform = importUnitTransform();
    for (const auto id : sortedEntityIds_) {
      const Entity *product = entity(id);
      if (product == nullptr || product->args.kind != StepValue::Kind::List ||
          product->args.list.size() < 7u) {
        continue;
      }

      const auto placementRef = firstRef(*product, 5);
      const auto representationRef = firstRef(*product, 6);
      if (!representationRef.has_value()) {
        continue;
      }
      const Entity *representation = entity(*representationRef);
      if (representation == nullptr ||
          representation->type != "IFCPRODUCTDEFINITIONSHAPE") {
        continue;
      }

      const glm::mat4 placement = placementRef.has_value()
                                      ? localPlacement(*placementRef)
                                      : glm::mat4(1.0f);
      std::vector<GeometryInstance> instances;
      const ProductMetadata metadata = productMetadata(*product);
      if (product->type == "IFCOPENINGELEMENT") {
        continue;
      }
      auto &report = model_.importReport;
      ++report.sourceProductCount;
      const size_t warningStart = report.diagnostics.size();
      // Traverse with product identity so unsupported mapped shapes stay
      // attributable.
      collectGeometryInstances(*representationRef, glm::mat4(1.0f), instances,
                               product);
      const size_t elementStart = model_.elements.size();
      auto solidInstances = instances;
      std::erase_if(solidInstances, [&](const auto &instance) {
        return groupsByItem_.at(instance.geometryId).front().geometryKind !=
               dotbim::GeometryKind::Mesh;
      });
      const bool voided =
          appendVoidedProductElement(*product, placement, solidInstances,
                                     metadata, unitTransform) ||
          appendKernelVoidedProduct(*product, placement, solidInstances,
                                    metadata, unitTransform);
      if (!voided && openingsByHostProduct_.contains(product->id))
        reportGeometryIssue(
            product->id, product,
            "Opening subtraction is unsupported for this host or opening");
      for (const auto &instance : instances) {
        const auto groupIt = groupsByItem_.find(instance.geometryId);
        if (groupIt == groupsByItem_.end()) {
          continue;
        }
        for (const auto &group : groupIt->second) {
          if (voided && group.geometryKind == dotbim::GeometryKind::Mesh)
            continue;
          container::geometry::dotbim::Element element{};
          element.meshId = group.meshId;
          element.geometryKind = group.geometryKind;
          element.transform = unitTransform * placement * instance.transform;
          element.color = group.geometryKind == dotbim::GeometryKind::Curves
                              ? instance.curveColor.value_or(group.color)
                              : group.color;
          element.type = product->type;
          applyProductMetadata(element, metadata);
          model_.elements.push_back(std::move(element));
        }
      }
      if (model_.elements.size() == elementStart) {
        ++report.skippedProductCount;
        if (report.diagnostics.size() == warningStart)
          reportGeometryIssue(*representationRef, product,
                              "No drawable geometry produced");
      } else {
        ++report.importedProductCount;
        if (report.diagnostics.size() != warningStart)
          ++report.partialProductCount;
      }
    }
  }

  void compactDrawableGeometry() {
    std::unordered_set<uint32_t> referenced;
    for (const auto &element : model_.elements)
      referenced.insert(element.meshId);
    size_t indexCount = 0;
    const auto countIndices = [&](const auto &ranges) {
      for (const auto &range : ranges)
        if (referenced.contains(range.meshId))
          indexCount += range.indexCount;
    };
    countIndices(model_.meshRanges);
    countIndices(model_.nativeCurveRanges);
    countIndices(model_.nativePointRanges);
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(std::min(model_.vertices.size(), indexCount));
    indices.reserve(indexCount);
    const uint32_t missing = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> remap(model_.vertices.size(), missing);
    const auto compactRanges = [&](auto &sourceRanges) {
      auto ranges = sourceRanges;
      sourceRanges.clear();
      for (auto range : ranges) {
        if (!referenced.contains(range.meshId))
          continue;
        const size_t start = range.firstIndex;
        range.firstIndex = static_cast<uint32_t>(indices.size());
        for (size_t i = start; i < start + range.indexCount; ++i) {
          const auto original = model_.indices[i];
          if (remap[original] == missing) {
            remap[original] = static_cast<uint32_t>(vertices.size());
            vertices.push_back(model_.vertices[original]);
          }
          indices.push_back(remap[original]);
        }
        sourceRanges.push_back(range);
      }
    };
    compactRanges(model_.meshRanges);
    compactRanges(model_.nativeCurveRanges);
    compactRanges(model_.nativePointRanges);
    model_.vertices = std::move(vertices);
    model_.indices = std::move(indices);
  }

  void appendFallbackElements() {
    if (!model_.elements.empty() ||
        model_.importReport.sourceProductCount != 0) {
      return;
    }

    const glm::mat4 transform = importUnitTransform();
    for (const auto id : sortedEntityIds_) {
      const auto groupIt = groupsByItem_.find(id);
      if (groupIt == groupsByItem_.end()) {
        continue;
      }
      for (const auto &group : groupIt->second) {
        container::geometry::dotbim::Element element{};
        element.meshId = group.meshId;
        element.geometryKind = group.geometryKind;
        element.transform = transform;
        element.color = group.color;
        element.type = entity(id)->type;
        element.sourceId = "#" + std::to_string(id);
        model_.elements.push_back(std::move(element));
      }
    }
  }

  std::unordered_map<uint32_t, Entity> entities_;
  std::vector<uint32_t> sortedEntityIds_{};
  float importScale_{1.0f};
  LengthUnitMetadata unitMetadata_{};
  float unitScale_{1.0f};
  uint32_t nextMeshId_{1};
  Model model_{};
  mutable std::unordered_map<uint32_t, glm::vec3> pointCache_{};
  mutable std::unordered_map<uint32_t, glm::vec3> directionCache_{};
  mutable std::unordered_map<uint32_t, glm::mat4> axisPlacementCache_{};
  mutable std::unordered_map<uint32_t, glm::mat4> localPlacementCache_{};
  std::unordered_map<uint32_t, glm::vec4> styleColorByItem_{};
  std::unordered_map<uint32_t, std::vector<glm::vec4>> faceColorsByFaceSet_{};
  std::unordered_map<uint32_t, std::vector<MeshGroup>> groupsByItem_{};
  std::unordered_map<uint32_t, manifold::Manifold> solidCache_{};
  std::unordered_set<uint32_t> failedSolids_{};
  std::unordered_map<uint32_t, std::string> conversionErrors_{};
  std::unordered_map<uint32_t, glm::vec4> solidColors_{};
  std::unordered_set<uint64_t> diagnosedItems_{};
  std::unordered_map<uint32_t, BoxSolid> boxSolidsByItem_{};
  std::unordered_map<uint32_t, std::vector<uint32_t>> openingsByHostProduct_{};
  std::unordered_map<uint32_t, uint32_t> hostByOpeningProduct_{};
  std::unordered_map<uint32_t, StoreyMetadata> storeyByProduct_{};
  std::unordered_map<uint32_t, SemanticMaterialMetadata> materialByProduct_{};
  std::unordered_map<uint32_t, ProductMetadata> semanticPropertiesByProduct_{};
};

} // namespace

Model LoadFromStep(std::string_view stepText, float importScale) {
  return IfcModelBuilder(parseEntities(stepText), importScale).build();
}

Model LoadFromFile(const std::filesystem::path &path, float importScale) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open IFC file: " +
                             container::util::pathToUtf8(path));
  }
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return LoadFromStep(text, importScale);
}

} // namespace container::geometry::ifc
