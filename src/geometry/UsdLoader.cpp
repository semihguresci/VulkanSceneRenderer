#include "Container/geometry/UsdLoader.h"

#include "Container/geometry/CoordinateSystem.h"
#include "Container/utility/ModelImportCancellation.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneData.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#if defined(CONTAINER_HAS_TINYUSDZ)
#include <composition.hh>
#include <tinyusdz.hh>
#include <tydra/render-data-converter.hh>
#include <tydra/render-data.hh>
#include <tydra/scene-access.hh>
#include <tydra/shader-network.hh>
#endif

namespace container::geometry::usd {
namespace {

enum class TokenKind { Identifier, String, Number, Symbol, End };

struct Token {
  TokenKind kind{TokenKind::End};
  std::string text{};
  double number{0.0};
};

struct StageUnitMetadata {
  bool authored{false};
  float metersPerUnit{1.0f};
};

struct MeshData {
  std::vector<glm::vec3> points{};
  std::vector<uint32_t> faceVertexCounts{};
  std::vector<uint32_t> faceVertexIndices{};
  std::vector<uint32_t> curveVertexCounts{};
  std::vector<float> widths{};
  std::vector<glm::vec3> normals{};
  std::vector<glm::vec2> texcoords0{};
  std::vector<glm::vec2> texcoords1{};
  std::vector<glm::vec3> colors{};
};

struct Node {
  std::string name{};
  std::string path{};
  std::string typeName{};
  std::string displayName{};
  std::string guid{};
  std::string semanticType{};
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
  int parent{-1};
  std::vector<int> children{};
  MeshData mesh{};
  std::unordered_map<std::string, glm::mat4> xformOps{};
  std::vector<std::string> xformOpSequence{};
  std::vector<std::string> xformOpOrder{};
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  bool hasColor{false};
  bool hasOpacity{false};
  bool doubleSided{false};
  bool hasDoubleSided{false};
  bool visible{true};
  bool hasVisibility{false};
};

bool isSymbol(char c) {
  switch (c) {
  case '{':
  case '}':
  case '[':
  case ']':
  case '(':
  case ')':
  case ',':
  case '=':
  case ';':
    return true;
  default:
    return false;
  }
}

bool isNumberStart(std::string_view text, size_t offset) {
  const char c = text[offset];
  if (std::isdigit(static_cast<unsigned char>(c))) {
    return true;
  }
  if ((c == '+' || c == '-' || c == '.') && offset + 1u < text.size()) {
    return std::isdigit(static_cast<unsigned char>(text[offset + 1u]));
  }
  return false;
}

using container::util::checkModelImportCancellation;

void checkImportProgress(size_t index, const std::stop_token &stop) {
  if (index % 1024u == 0u)
    checkModelImportCancellation(stop);
}

std::vector<Token> tokenize(std::string_view text, const std::stop_token &stop) {
  std::vector<Token> tokens;
  size_t cursor = 0;
  size_t nextCheckpoint = 0;
  const auto checkCursor = [&] {
    if (cursor >= nextCheckpoint) {
      checkModelImportCancellation(stop);
      nextCheckpoint = cursor + 4096u;
    }
  };
  while (cursor < text.size()) {
    checkCursor();
    const char c = text[cursor];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++cursor;
      continue;
    }
    if (c == '#') {
      while (cursor < text.size() && text[cursor] != '\n') {
        checkCursor();
        ++cursor;
      }
      continue;
    }
    if (c == '"') {
      ++cursor;
      std::string value;
      while (cursor < text.size()) {
        checkCursor();
        const char current = text[cursor++];
        if (current == '"') {
          break;
        }
        if (current == '\\' && cursor < text.size()) {
          value.push_back(text[cursor++]);
        } else {
          value.push_back(current);
        }
      }
      tokens.push_back({TokenKind::String, std::move(value), 0.0});
      continue;
    }
    if (isSymbol(c)) {
      tokens.push_back({TokenKind::Symbol, std::string(1, c), 0.0});
      ++cursor;
      continue;
    }
    if (isNumberStart(text, cursor)) {
      const char *begin = text.data() + cursor;
      char *end = nullptr;
      const double value = std::strtod(begin, &end);
      if (end != begin) {
        tokens.push_back({TokenKind::Number,
                          std::string(begin, static_cast<size_t>(end - begin)),
                          value});
        cursor += static_cast<size_t>(end - begin);
        continue;
      }
    }

    const size_t start = cursor;
    while (cursor < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[cursor])) &&
           !isSymbol(text[cursor]) && text[cursor] != '"' &&
           text[cursor] != '#') {
      checkCursor();
      ++cursor;
    }
    tokens.push_back({TokenKind::Identifier,
                      std::string(text.substr(start, cursor - start)), 0.0});
  }
  tokens.push_back({TokenKind::End, {}, 0.0});
  return tokens;
}

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

std::string metadataLookupKey(std::string_view name) {
  if (const size_t separator = name.find_last_of(':');
      separator != std::string_view::npos) {
    name = name.substr(separator + 1u);
  }

  std::string key;
  key.reserve(name.size());
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      key.push_back(static_cast<char>(
          std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return key;
}

bool isGuidMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "guid" || key == "globalid" || key == "ifcguid" ||
         key == "elementguid" || key == "uniqueid";
}

bool isTypeMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "type" || key == "ifctype" || key == "ifcclass" ||
         key == "class" || key == "category";
}

bool isDisplayNameMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "displayname" || key == "name" || key == "productname";
}

bool isObjectTypeMetadataName(std::string_view name) {
  return metadataLookupKey(name) == "objecttype";
}

bool isStoreyNameMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "storeyname" || key == "buildingstoreyname" ||
         key == "levelname";
}

bool isStoreyIdMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "storeyid" || key == "storeyguid" ||
         key == "buildingstoreyid" || key == "buildingstoreyguid" ||
         key == "levelid";
}

bool isMaterialNameMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "materialname" || key == "ifcmaterialname";
}

bool isMaterialCategoryMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "materialcategory" || key == "ifcmaterialcategory";
}

bool isDisciplineMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "discipline" || key == "ifcdiscipline";
}

bool isPhaseMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "phase" || key == "phasename" ||
         key == "constructionphase" || key == "ifcphase";
}

bool isFireRatingMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "firerating" || key == "fireclassification";
}

bool isLoadBearingMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "loadbearing" || key == "isloadbearing";
}

bool isStatusMetadataName(std::string_view name) {
  const std::string key = metadataLookupKey(name);
  return key == "status" || key == "elementstatus";
}

void applyCommonMetadataValue(std::string_view name, std::string value,
                              std::string &guid, std::string &semanticType,
                              std::string &displayName,
                              std::string *objectType = nullptr,
                              std::string *storeyName = nullptr,
                              std::string *storeyId = nullptr,
                              std::string *materialName = nullptr,
                              std::string *materialCategory = nullptr,
                              std::string *discipline = nullptr,
                              std::string *phase = nullptr,
                              std::string *fireRating = nullptr,
                              std::string *loadBearing = nullptr,
                              std::string *status = nullptr) {
  if (value.empty()) {
    return;
  }
  if (isGuidMetadataName(name) && guid.empty()) {
    guid = std::move(value);
  } else if (objectType != nullptr && isObjectTypeMetadataName(name) &&
             objectType->empty()) {
    *objectType = std::move(value);
  } else if (isTypeMetadataName(name) && semanticType.empty()) {
    semanticType = std::move(value);
  } else if (isDisplayNameMetadataName(name) && displayName.empty()) {
    displayName = std::move(value);
  } else if (storeyName != nullptr && isStoreyNameMetadataName(name) &&
             storeyName->empty()) {
    *storeyName = std::move(value);
  } else if (storeyId != nullptr && isStoreyIdMetadataName(name) &&
             storeyId->empty()) {
    *storeyId = std::move(value);
  } else if (materialName != nullptr && isMaterialNameMetadataName(name) &&
             materialName->empty()) {
    *materialName = std::move(value);
  } else if (materialCategory != nullptr &&
             isMaterialCategoryMetadataName(name) &&
             materialCategory->empty()) {
    *materialCategory = std::move(value);
  } else if (discipline != nullptr && isDisciplineMetadataName(name) &&
             discipline->empty()) {
    *discipline = std::move(value);
  } else if (phase != nullptr && isPhaseMetadataName(name) &&
             phase->empty()) {
    *phase = std::move(value);
  } else if (fireRating != nullptr && isFireRatingMetadataName(name) &&
             fireRating->empty()) {
    *fireRating = std::move(value);
  } else if (loadBearing != nullptr && isLoadBearingMetadataName(name) &&
             loadBearing->empty()) {
    *loadBearing = std::move(value);
  } else if (status != nullptr && isStatusMetadataName(name) &&
             status->empty()) {
    *status = std::move(value);
  }
}

std::string usdFallbackType(std::string_view typeName,
                            std::string_view fallback = "UsdPrim") {
  if (typeName.empty()) {
    return std::string(fallback);
  }
  if (startsWith(typeName, "Usd")) {
    return std::string(typeName);
  }
  if (typeName == "Mesh" || typeName == "Xform" || typeName == "Scope" ||
      typeName == "Cube" || typeName == "Sphere" || typeName == "Cone" ||
      typeName == "Cylinder" || typeName == "Capsule" ||
      typeName == "Plane" || typeName == "BasisCurves" ||
      typeName == "NurbsCurves" || typeName == "PointInstancer" ||
      typeName == "Points") {
    return "UsdGeom" + std::string(typeName);
  }
  return "Usd" + std::string(typeName);
}

bool isPrimKeyword(const Token &token) {
  return token.kind == TokenKind::Identifier &&
         (token.text == "def" || token.text == "over" || token.text == "class");
}

bool tokenIsSymbol(const Token &token, char symbol) {
  return token.kind == TokenKind::Symbol && token.text.size() == 1u &&
         token.text.front() == symbol;
}

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

float sanitizeUsdMetersPerUnit(double metersPerUnit) {
  if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0.0) {
    return 1.0f;
  }
  return static_cast<float>(std::clamp(metersPerUnit, 1.0e-9, 1.0e9));
}

bool approximatelyEqual(float lhs, float rhs) {
  const float tolerance =
      std::max(1.0e-9f, std::max(std::abs(lhs), std::abs(rhs)) * 1.0e-5f);
  return std::abs(lhs - rhs) <= tolerance;
}

std::string sourceUnitLabelFromMetersPerUnit(float metersPerUnit) {
  struct KnownUnit {
    float metersPerUnit;
    const char *label;
  };
  static constexpr KnownUnit kKnownUnits[] = {
      {0.001f, "millimeters"},
      {0.01f, "centimeters"},
      {0.1f, "decimeters"},
      {1.0f, "meters"},
      {1000.0f, "kilometers"},
      {0.0254f, "inches"},
      {0.3048f, "feet"},
  };
  for (const KnownUnit &unit : kKnownUnits) {
    if (approximatelyEqual(metersPerUnit, unit.metersPerUnit)) {
      return unit.label;
    }
  }
  return "USD units";
}

dotbim::ModelUnitMetadata makeUnitMetadata(StageUnitMetadata stageUnits,
                                           float importScale) {
  const float sanitizedImportScale = sanitizeImportScale(importScale);
  dotbim::ModelUnitMetadata metadata{};
  if (stageUnits.authored) {
    metadata.hasSourceUnits = true;
    metadata.sourceUnits =
        sourceUnitLabelFromMetersPerUnit(stageUnits.metersPerUnit);
    metadata.hasMetersPerUnit = true;
    metadata.metersPerUnit = stageUnits.metersPerUnit;
  }
  metadata.hasImportScale = true;
  metadata.importScale = sanitizedImportScale;
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale =
      sanitizedImportScale *
      (stageUnits.authored ? stageUnits.metersPerUnit : 1.0f);
  return metadata;
}

std::string stageUpAxisFromText(std::string_view usdText,
                                const std::stop_token &stop) {
  const std::vector<Token> tokens = tokenize(usdText, stop);
  int braceDepth = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    checkImportProgress(i, stop);
    const Token &token = tokens[i];
    if (token.kind == TokenKind::End) {
      break;
    }
    if (tokenIsSymbol(token, '{')) {
      ++braceDepth;
      continue;
    }
    if (tokenIsSymbol(token, '}')) {
      if (braceDepth > 0) {
        --braceDepth;
      }
      continue;
    }
    if (braceDepth != 0) {
      continue;
    }
    if (isPrimKeyword(token)) {
      break;
    }
    if (token.kind == TokenKind::Identifier && token.text == "upAxis" &&
        i + 2u < tokens.size() && tokenIsSymbol(tokens[i + 1u], '=')) {
      const Token &value = tokens[i + 2u];
      if (value.kind == TokenKind::String ||
          value.kind == TokenKind::Identifier) {
        return value.text;
      }
    }
  }
  return "Y";
}

void applySourceUpAxis(dotbim::Model& model, std::string upAxis) {
  if (upAxis.empty()) {
    upAxis = "Y";
  }
  model.georeferenceMetadata.hasSourceUpAxis = true;
  model.georeferenceMetadata.sourceUpAxis = std::move(upAxis);
}

StageUnitMetadata stageMetersPerUnitFromText(std::string_view usdText,
                                            const std::stop_token &stop) {
  const std::vector<Token> tokens = tokenize(usdText, stop);
  int braceDepth = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    checkImportProgress(i, stop);
    const Token &token = tokens[i];
    if (token.kind == TokenKind::End) {
      break;
    }
    if (tokenIsSymbol(token, '{')) {
      ++braceDepth;
      continue;
    }
    if (tokenIsSymbol(token, '}')) {
      if (braceDepth > 0) {
        --braceDepth;
      }
      continue;
    }
    if (braceDepth != 0) {
      continue;
    }
    if (isPrimKeyword(token)) {
      break;
    }
    if (token.kind == TokenKind::Identifier &&
        token.text == "metersPerUnit" && i + 2u < tokens.size() &&
        tokenIsSymbol(tokens[i + 1u], '=')) {
      const Token &value = tokens[i + 2u];
      if (value.kind == TokenKind::Number) {
        return StageUnitMetadata{
            .authored = true,
            .metersPerUnit = sanitizeUsdMetersPerUnit(value.number),
        };
      }
    }
  }
  return {};
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

container::geometry::Vertex
makeVertex(const glm::vec3 &position, const glm::vec3 &normal,
           const glm::vec4 &tangent, const glm::vec3 &color,
           const glm::vec2 &texCoord, const glm::vec2 &texCoord1) {
  container::geometry::Vertex vertex{};
  vertex.position = position;
  vertex.color = color;
  vertex.texCoord = texCoord;
  vertex.texCoord1 = texCoord1;
  vertex.normal = normal;
  vertex.tangent = tangent;
  return vertex;
}

std::pair<glm::vec3, float>
computeBounds(const std::vector<glm::vec3> &points, const std::stop_token &stop) {
  if (points.empty()) {
    return {glm::vec3(0.0f), 0.0f};
  }

  glm::vec3 minBounds = points.front();
  glm::vec3 maxBounds = points.front();
  for (size_t i = 0; i < points.size(); ++i) {
    checkImportProgress(i, stop);
    const auto &point = points[i];
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  }

  const glm::vec3 center = (minBounds + maxBounds) * 0.5f;
  float radius = 0.0f;
  for (size_t i = 0; i < points.size(); ++i) {
    checkImportProgress(i, stop);
    radius = std::max(radius, glm::length(points[i] - center));
  }
  return {center, radius};
}

std::vector<uint32_t> triangulateIndices(const MeshData &mesh,
                                         std::string_view path,
                                         const std::stop_token &stop) {
  if (mesh.points.empty() || mesh.faceVertexIndices.empty()) {
    return {};
  }

  for (size_t i = 0; i < mesh.faceVertexIndices.size(); ++i) {
    checkImportProgress(i, stop);
    if (mesh.faceVertexIndices[i] >= mesh.points.size()) {
      throw std::runtime_error("USD mesh '" + std::string(path) +
                               "' has a face vertex index outside points");
    }
  }

  if (mesh.faceVertexCounts.empty()) {
    if ((mesh.faceVertexIndices.size() % 3u) != 0u) {
      throw std::runtime_error("USD mesh '" + std::string(path) +
                               "' has non-triangle indices without counts");
    }
    return mesh.faceVertexIndices;
  }

  std::vector<uint32_t> triangles;
  size_t cursor = 0;
  for (size_t face = 0; face < mesh.faceVertexCounts.size(); ++face) {
    checkImportProgress(face, stop);
    const uint32_t count = mesh.faceVertexCounts[face];
    if (count < 3u ||
        cursor + static_cast<size_t>(count) > mesh.faceVertexIndices.size()) {
      throw std::runtime_error("USD mesh '" + std::string(path) +
                               "' has inconsistent face vertex counts");
    }
    const uint32_t first = mesh.faceVertexIndices[cursor];
    for (uint32_t i = 1; i + 1u < count; ++i) {
      checkImportProgress(i, stop);
      triangles.push_back(first);
      triangles.push_back(mesh.faceVertexIndices[cursor + i]);
      triangles.push_back(mesh.faceVertexIndices[cursor + i + 1u]);
    }
    cursor += count;
  }

  if (cursor != mesh.faceVertexIndices.size()) {
    throw std::runtime_error("USD mesh '" + std::string(path) +
                             "' has unused face vertex indices");
  }
  return triangles;
}

class UsdParser {
public:
  explicit UsdParser(std::string_view text, std::stop_token stop)
      : stop_(stop), tokens_(tokenize(text, stop_)) {}

  std::vector<Node> parse() {
    parseBlock(-1);
    return std::move(nodes_);
  }

private:
  [[nodiscard]] const Token &peek(size_t offset = 0) const {
    checkImportProgress(position_, stop_);
    const size_t index = std::min(position_ + offset, tokens_.size() - 1u);
    return tokens_[index];
  }

  [[nodiscard]] bool isEnd() const { return peek().kind == TokenKind::End; }

  [[nodiscard]] bool peekSymbol(char symbol) const {
    return peek().kind == TokenKind::Symbol && peek().text.size() == 1u &&
           peek().text.front() == symbol;
  }

  bool matchSymbol(char symbol) {
    if (!peekSymbol(symbol)) {
      return false;
    }
    ++position_;
    return true;
  }

  void skipBalanced(char opening, char closing) {
    if (!matchSymbol(opening)) {
      return;
    }

    int depth = 1;
    while (!isEnd() && depth > 0) {
      if (peekSymbol(opening)) {
        ++depth;
      } else if (peekSymbol(closing)) {
        --depth;
      }
      ++position_;
    }
  }

  std::vector<double> collectNumbersFromValue() {
    std::vector<double> values;
    collectValue([&](const Token &token) {
      if (token.kind == TokenKind::Number && std::isfinite(token.number)) {
        values.push_back(token.number);
      }
    });
    return values;
  }

  std::vector<std::string> collectStringsFromValue() {
    std::vector<std::string> values;
    collectValue([&](const Token &token) {
      if (token.kind == TokenKind::String ||
          token.kind == TokenKind::Identifier) {
        values.push_back(token.text);
      }
    });
    return values;
  }

  std::optional<std::string> collectFirstStringFromValue() {
    std::optional<std::string> result;
    collectValue([&](const Token &token) {
      if (!result && (token.kind == TokenKind::String ||
                      token.kind == TokenKind::Identifier)) {
        result = token.text;
      }
    });
    return result;
  }

  template <typename Visitor> void collectValue(Visitor &&visitor) {
    if (isEnd()) {
      return;
    }
    if (peekSymbol('(')) {
      collectBalanced('(', ')', visitor);
      return;
    }
    if (peekSymbol('[')) {
      collectBalanced('[', ']', visitor);
      return;
    }
    if (peekSymbol('{')) {
      collectBalanced('{', '}', visitor);
      return;
    }
    visitor(peek());
    ++position_;
  }

  template <typename Visitor>
  void collectBalanced(char opening, char closing, Visitor &&visitor) {
    if (!matchSymbol(opening)) {
      return;
    }
    int depth = 1;
    while (!isEnd() && depth > 0) {
      if (peekSymbol(opening)) {
        ++depth;
        ++position_;
        continue;
      }
      if (peekSymbol(closing)) {
        --depth;
        ++position_;
        continue;
      }
      visitor(peek());
      ++position_;
    }
  }

  void skipValue() {
    collectValue([](const Token &) {});
  }

  static std::optional<glm::vec3> firstVec3(const std::vector<double> &values) {
    if (values.size() < 3u) {
      return std::nullopt;
    }
    return glm::vec3(static_cast<float>(values[0]),
                     static_cast<float>(values[1]),
                     static_cast<float>(values[2]));
  }

  std::vector<glm::vec3>
  vec3ArrayFromNumbers(const std::vector<double> &values,
                       std::string_view attributeName) const {
    if ((values.size() % 3u) != 0u) {
      throw std::runtime_error("USD attribute '" + std::string(attributeName) +
                               "' must contain vec3 values");
    }
    std::vector<glm::vec3> result;
    result.reserve(values.size() / 3u);
    for (size_t i = 0; i < values.size(); i += 3u) {
      checkImportProgress(i, stop_);
      result.emplace_back(static_cast<float>(values[i]),
                          static_cast<float>(values[i + 1u]),
                          static_cast<float>(values[i + 2u]));
    }
    return result;
  }

  std::vector<glm::vec2>
  vec2ArrayFromNumbers(const std::vector<double> &values,
                       std::string_view attributeName) const {
    if ((values.size() % 2u) != 0u) {
      throw std::runtime_error("USD attribute '" + std::string(attributeName) +
                               "' must contain vec2 values");
    }
    std::vector<glm::vec2> result;
    result.reserve(values.size() / 2u);
    for (size_t i = 0; i < values.size(); i += 2u) {
      checkImportProgress(i, stop_);
      result.emplace_back(static_cast<float>(values[i]),
                          static_cast<float>(values[i + 1u]));
    }
    return result;
  }

  std::vector<float>
  floatArrayFromNumbers(const std::vector<double> &values,
                         std::string_view attributeName) const {
    std::vector<float> result;
    result.reserve(values.size());
    for (double value : values) {
      checkImportProgress(result.size(), stop_);
      if (!std::isfinite(value)) {
        throw std::runtime_error("USD attribute '" +
                                 std::string(attributeName) +
                                 "' must contain finite float values");
      }
      result.push_back(static_cast<float>(value));
    }
    return result;
  }

  std::vector<uint32_t>
  indexArrayFromNumbers(const std::vector<double> &values,
                         std::string_view attributeName) const {
    std::vector<uint32_t> result;
    result.reserve(values.size());
    for (double value : values) {
      checkImportProgress(result.size(), stop_);
      if (!std::isfinite(value) || value < 0.0 ||
          value > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
          std::floor(value) != value) {
        throw std::runtime_error("USD attribute '" +
                                 std::string(attributeName) +
                                 "' must contain non-negative integer values");
      }
      result.push_back(static_cast<uint32_t>(value));
    }
    return result;
  }

  static glm::mat4 matrixFromNumbers(const std::vector<double> &values,
                                     std::string_view attributeName) {
    if (values.size() < 16u) {
      throw std::runtime_error("USD attribute '" + std::string(attributeName) +
                               "' must contain a 4x4 matrix");
    }

    glm::mat4 result(1.0f);
    for (size_t column = 0; column < 4u; ++column) {
      for (size_t row = 0; row < 4u; ++row) {
        result[static_cast<glm::length_t>(column)]
              [static_cast<glm::length_t>(row)] =
                  static_cast<float>(values[column * 4u + row]);
      }
    }
    return result;
  }

  static glm::mat4 rotateDegrees(float degrees, const glm::vec3 &axis) {
    return glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis);
  }

  static glm::mat4 transformOpMatrix(std::string_view name,
                                     const std::vector<double> &values) {
    if (name == "xformOp:transform") {
      return matrixFromNumbers(values, name);
    }

    const glm::vec3 value = firstVec3(values).value_or(glm::vec3(0.0f));
    if (startsWith(name, "xformOp:translate")) {
      return glm::translate(glm::mat4(1.0f), value);
    }
    if (startsWith(name, "xformOp:scale")) {
      return glm::scale(glm::mat4(1.0f), value);
    }
    if (startsWith(name, "xformOp:rotateXYZ")) {
      return rotateDegrees(value.x, {1.0f, 0.0f, 0.0f}) *
             rotateDegrees(value.y, {0.0f, 1.0f, 0.0f}) *
             rotateDegrees(value.z, {0.0f, 0.0f, 1.0f});
    }

    const float scalar =
        values.empty() ? 0.0f : static_cast<float>(values.front());
    if (startsWith(name, "xformOp:rotateX")) {
      return rotateDegrees(scalar, {1.0f, 0.0f, 0.0f});
    }
    if (startsWith(name, "xformOp:rotateY")) {
      return rotateDegrees(scalar, {0.0f, 1.0f, 0.0f});
    }
    if (startsWith(name, "xformOp:rotateZ")) {
      return rotateDegrees(scalar, {0.0f, 0.0f, 1.0f});
    }

    return glm::mat4(1.0f);
  }

  std::optional<bool> collectBoolFromValue() {
    std::optional<bool> result;
    collectValue([&](const Token &token) {
      if (result)
        return;
      if (token.kind == TokenKind::Number && std::isfinite(token.number)) {
        result = token.number != 0.0;
      } else if (token.kind == TokenKind::Identifier ||
                 token.kind == TokenKind::String) {
        const std::string value = lowerAscii(token.text);
        if (value == "true" || value == "1") {
          result = true;
        } else if (value == "false" || value == "0") {
          result = false;
        }
      }
    });
    return result;
  }

  std::optional<std::string> declarationAttributeName(size_t start,
                                                      size_t equal) const {
    for (size_t i = equal; i > start;) {
      --i;
      checkImportProgress(i, stop_);
      if (tokens_[i].kind == TokenKind::Identifier) {
        return tokens_[i].text;
      }
    }
    return std::nullopt;
  }

  std::optional<size_t> findDeclarationEquals() const {
    size_t depth = 0;
    for (size_t i = position_; i < tokens_.size(); ++i) {
      checkImportProgress(i, stop_);
      const Token &token = tokens_[i];
      if (token.kind == TokenKind::End) {
        return std::nullopt;
      }
      if (depth == 0 && token.kind == TokenKind::Symbol && token.text == "}") {
        return std::nullopt;
      }
      if (depth == 0 && i != position_ && isPrimKeyword(token)) {
        return std::nullopt;
      }
      if (token.kind == TokenKind::Symbol) {
        if (token.text == "(" || token.text == "[") {
          ++depth;
        } else if ((token.text == ")" || token.text == "]") && depth > 0) {
          --depth;
        } else if (token.text == "=" && depth == 0) {
          return i;
        }
      }
    }
    return std::nullopt;
  }

  void parseBlock(int currentNode) {
    while (!isEnd()) {
      if (matchSymbol('}')) {
        return;
      }
      if (isPrimKeyword(peek())) {
        parsePrim(currentNode);
        continue;
      }
      parseAttribute(currentNode);
    }
  }

  void parsePrim(int parent) {
    ++position_;
    std::string typeName = "Prim";
    if (peek().kind == TokenKind::Identifier) {
      typeName = peek().text;
      ++position_;
    }

    std::string name = typeName + std::to_string(nodes_.size());
    if (peek().kind == TokenKind::String ||
        peek().kind == TokenKind::Identifier) {
      name = peek().text;
      ++position_;
    }

    Node node{};
    node.name = name;
    node.typeName = typeName;
    node.parent = parent;
    if (parent >= 0 && static_cast<size_t>(parent) < nodes_.size()) {
      node.path = nodes_[static_cast<size_t>(parent)].path + "/" + name;
    } else {
      node.path = "/" + name;
    }

    const int nodeIndex = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));
    if (parent >= 0 && static_cast<size_t>(parent) < nodes_.size()) {
      nodes_[static_cast<size_t>(parent)].children.push_back(nodeIndex);
    }

    while (!isEnd() && !peekSymbol('{')) {
      if (peekSymbol('(')) {
        parsePrimMetadata(nodes_[static_cast<size_t>(nodeIndex)]);
      } else {
        ++position_;
      }
    }
    if (matchSymbol('{')) {
      parseBlock(nodeIndex);
    }
  }

  void parsePrimMetadata(Node &node) {
    if (!matchSymbol('(')) {
      return;
    }

    int depth = 1;
    std::optional<std::string> key;
    while (!isEnd() && depth > 0) {
      if (peekSymbol('(') || peekSymbol('[') || peekSymbol('{')) {
        ++depth;
        ++position_;
        key.reset();
        continue;
      }
      if (peekSymbol(')') || peekSymbol(']') || peekSymbol('}')) {
        --depth;
        ++position_;
        key.reset();
        continue;
      }

      if (peek().kind == TokenKind::Identifier) {
        key = peek().text;
        ++position_;
        continue;
      }

      if (peekSymbol('=') && key) {
        const std::string name = *key;
        ++position_;
        if (peek().kind == TokenKind::String ||
            peek().kind == TokenKind::Identifier) {
          applyCommonMetadataValue(name, peek().text, node.guid,
                                   node.semanticType, node.displayName,
                                   &node.objectType, &node.storeyName,
                                   &node.storeyId, &node.materialName,
                                   &node.materialCategory, &node.discipline,
                                   &node.phase, &node.fireRating,
                                   &node.loadBearing, &node.status);
        }
        key.reset();
        continue;
      }

      ++position_;
    }
  }

  void parseAttribute(int currentNode) {
    const size_t start = position_;
    const std::optional<size_t> equal = findDeclarationEquals();
    if (!equal) {
      if (!isEnd()) {
        ++position_;
      }
      return;
    }

    const std::optional<std::string> attributeName =
        declarationAttributeName(start, *equal);
    position_ = *equal + 1u;

    if (currentNode < 0 || !attributeName) {
      skipValue();
      return;
    }

    Node &node = nodes_[static_cast<size_t>(currentNode)];
    const std::string &name = *attributeName;

    if (name == "faceVertexCounts") {
      node.mesh.faceVertexCounts =
          indexArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "curveVertexCounts") {
      node.mesh.curveVertexCounts =
          indexArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "faceVertexIndices") {
      node.mesh.faceVertexIndices =
          indexArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "points") {
      node.mesh.points = vec3ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "widths") {
      node.mesh.widths = floatArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "normals" || name == "primvars:normals") {
      node.mesh.normals = vec3ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "primvars:st" || name == "st" || name == "texcoords") {
      node.mesh.texcoords0 =
          vec2ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "primvars:st1" || name == "st1" ||
               name == "primvars:texCoord1") {
      node.mesh.texcoords1 =
          vec2ArrayFromNumbers(collectNumbersFromValue(), name);
    } else if (name == "xformOpOrder") {
      node.xformOpOrder = collectStringsFromValue();
    } else if (startsWith(name, "xformOp:")) {
      const std::vector<double> values = collectNumbersFromValue();
      node.xformOps[name] = transformOpMatrix(name, values);
      if (std::ranges::find(node.xformOpSequence, name) ==
          node.xformOpSequence.end()) {
        node.xformOpSequence.push_back(name);
      }
    } else if (name == "visibility") {
      const auto values = collectStringsFromValue();
      if (!values.empty()) {
        node.visible = values.front() != "invisible";
        node.hasVisibility = true;
      }
    } else if (name == "doubleSided") {
      if (const auto value = collectBoolFromValue()) {
        node.doubleSided = *value;
        node.hasDoubleSided = true;
      }
    } else if (name == "primvars:displayColor" || name == "displayColor") {
      const std::vector<glm::vec3> colors =
          vec3ArrayFromNumbers(collectNumbersFromValue(), name);
      if (colors.size() > 1u) {
        node.mesh.colors = colors;
        node.color.r = 1.0f;
        node.color.g = 1.0f;
        node.color.b = 1.0f;
        node.hasColor = true;
      } else if (!colors.empty()) {
        node.color.r = std::clamp(colors.front().r, 0.0f, 1.0f);
        node.color.g = std::clamp(colors.front().g, 0.0f, 1.0f);
        node.color.b = std::clamp(colors.front().b, 0.0f, 1.0f);
        node.hasColor = true;
      }
    } else if (name == "primvars:displayOpacity" || name == "displayOpacity") {
      const std::vector<double> values = collectNumbersFromValue();
      if (!values.empty() && std::isfinite(values.front())) {
        node.color.a =
            std::clamp(static_cast<float>(values.front()), 0.0f, 1.0f);
        node.hasOpacity = true;
      }
    } else if (isGuidMetadataName(name) || isTypeMetadataName(name) ||
               isDisplayNameMetadataName(name) ||
               isObjectTypeMetadataName(name) ||
               isStoreyNameMetadataName(name) ||
               isStoreyIdMetadataName(name) ||
               isMaterialNameMetadataName(name) ||
               isMaterialCategoryMetadataName(name) ||
               isDisciplineMetadataName(name) || isPhaseMetadataName(name) ||
               isFireRatingMetadataName(name) ||
               isLoadBearingMetadataName(name) || isStatusMetadataName(name)) {
      if (const auto value = collectFirstStringFromValue()) {
        applyCommonMetadataValue(name, *value, node.guid, node.semanticType,
                                 node.displayName, &node.objectType,
                                 &node.storeyName, &node.storeyId,
                                 &node.materialName, &node.materialCategory,
                                 &node.discipline, &node.phase,
                                 &node.fireRating, &node.loadBearing,
                                 &node.status);
      }
    } else {
      skipValue();
    }

    while (peekSymbol('(')) {
      skipBalanced('(', ')');
    }
  }

  std::stop_token stop_;
  std::vector<Token> tokens_{};
  size_t position_{0};
  std::vector<Node> nodes_{};
};

glm::mat4 nodeLocalTransform(const Node &node) {
  glm::mat4 transform(1.0f);
  const std::vector<std::string> &order =
      node.xformOpOrder.empty() ? node.xformOpSequence : node.xformOpOrder;
  for (const std::string &opName : order) {
    const auto opIt = node.xformOps.find(opName);
    if (opIt != node.xformOps.end()) {
      transform *= opIt->second;
    }
  }
  return transform;
}

glm::mat4 nodeWorldTransform(int nodeIndex, const std::vector<Node> &nodes,
                             std::vector<std::optional<glm::mat4>> &cache) {
  if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= nodes.size()) {
    return glm::mat4(1.0f);
  }
  if (cache[static_cast<size_t>(nodeIndex)]) {
    return *cache[static_cast<size_t>(nodeIndex)];
  }

  const Node &node = nodes[static_cast<size_t>(nodeIndex)];
  glm::mat4 transform = nodeLocalTransform(node);
  if (node.parent >= 0) {
    transform = nodeWorldTransform(node.parent, nodes, cache) * transform;
  }
  cache[static_cast<size_t>(nodeIndex)] = transform;
  return transform;
}

bool nodeVisible(int nodeIndex, const std::vector<Node> &nodes) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasVisibility && !node.visible) {
      return false;
    }
    cursor = node.parent;
  }
  return true;
}

glm::vec4 inheritedColor(int nodeIndex, const std::vector<Node> &nodes) {
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasColor) {
      color.r = node.color.r;
      color.g = node.color.g;
      color.b = node.color.b;
      break;
    }
    cursor = node.parent;
  }
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasOpacity) {
      color.a = node.color.a;
      break;
    }
    cursor = node.parent;
  }
  return color;
}

bool inheritedDoubleSided(int nodeIndex, const std::vector<Node> &nodes) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (node.hasDoubleSided) {
      return node.doubleSided;
    }
    cursor = node.parent;
  }
  return false;
}

std::optional<std::string> inheritedGuid(int nodeIndex,
                                         const std::vector<Node> &nodes) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (!node.guid.empty()) {
      return node.guid;
    }
    cursor = node.parent;
  }
  return std::nullopt;
}

std::string inheritedSemanticType(int nodeIndex,
                                  const std::vector<Node> &nodes) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    if (!node.semanticType.empty()) {
      return node.semanticType;
    }
    cursor = node.parent;
  }
  if (nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < nodes.size()) {
    return usdFallbackType(nodes[static_cast<size_t>(nodeIndex)].typeName);
  }
  return "UsdPrim";
}

std::optional<std::string> inheritedNodeString(
    int nodeIndex, const std::vector<Node> &nodes,
    const std::string Node::*member) {
  for (int cursor = nodeIndex; cursor >= 0;) {
    const Node &node = nodes[static_cast<size_t>(cursor)];
    const std::string &value = node.*member;
    if (!value.empty()) {
      return value;
    }
    cursor = node.parent;
  }
  return std::nullopt;
}

glm::vec3 normalizeOrFallback(const glm::vec3 &value,
                              const glm::vec3 &fallback) {
  const float len2 = glm::dot(value, value);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return fallback;
  }
  return value * (1.0f / std::sqrt(len2));
}

std::optional<glm::vec3> attributeVec3(const std::vector<glm::vec3> &values,
                                       uint32_t pointIndex,
                                       size_t faceVertexOrdinal) {
  if (values.empty()) {
    return std::nullopt;
  }
  if (values.size() == 1u) {
    return values.front();
  }
  if (pointIndex < values.size()) {
    return values[pointIndex];
  }
  if (faceVertexOrdinal < values.size()) {
    return values[faceVertexOrdinal];
  }
  return std::nullopt;
}

std::optional<glm::vec2> attributeVec2(const std::vector<glm::vec2> &values,
                                       uint32_t pointIndex,
                                       size_t faceVertexOrdinal) {
  if (values.empty()) {
    return std::nullopt;
  }
  if (values.size() == 1u) {
    return values.front();
  }
  if (pointIndex < values.size()) {
    return values[pointIndex];
  }
  if (faceVertexOrdinal < values.size()) {
    return values[faceVertexOrdinal];
  }
  return std::nullopt;
}

void appendMeshletClustersForRange(dotbim::Model &model,
                                   const dotbim::MeshRange &range,
                                   const std::stop_token &stop);

void appendMesh(dotbim::Model &model, const MeshData &mesh, uint32_t meshId,
                std::string_view path, const glm::vec4 &color,
                const std::stop_token &stop) {
  (void)color;
  const std::vector<uint32_t> sourceIndices = triangulateIndices(mesh, path, stop);
  if (mesh.points.empty() || sourceIndices.empty()) {
    return;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(mesh.points, stop);
  range.boundsCenter = center;
  range.boundsRadius = radius;

  for (size_t i = 0; i + 2u < sourceIndices.size(); i += 3u) {
    checkImportProgress(i, stop);
    const glm::vec3 &a = mesh.points[sourceIndices[i]];
    const glm::vec3 &b = mesh.points[sourceIndices[i + 1u]];
    const glm::vec3 &c = mesh.points[sourceIndices[i + 2u]];
    const glm::vec3 normal = safeNormal(a, b, c);
    const glm::vec3 tangent = safeTangent(a, b, normal);
    const glm::vec4 tangent4(tangent, 1.0f);
    const uint32_t base = static_cast<uint32_t>(model.vertices.size());
    for (size_t corner = 0; corner < 3u; ++corner) {
      const uint32_t sourceIndex = sourceIndices[i + corner];
      const glm::vec3 &position = mesh.points[sourceIndex];
      const glm::vec3 vertexNormal = normalizeOrFallback(
          attributeVec3(mesh.normals, sourceIndex, i + corner).value_or(normal),
          normal);
      const glm::vec3 vertexColor =
          glm::clamp(attributeVec3(mesh.colors, sourceIndex, i + corner)
                         .value_or(glm::vec3(1.0f)),
                     glm::vec3(0.0f), glm::vec3(1.0f));
      const glm::vec2 texCoord =
          attributeVec2(mesh.texcoords0, sourceIndex, i + corner)
              .value_or(glm::vec2(0.0f));
      const glm::vec2 texCoord1 =
          attributeVec2(mesh.texcoords1, sourceIndex, i + corner)
              .value_or(glm::vec2(0.0f));
      model.vertices.push_back(makeVertex(position, vertexNormal, tangent4,
                                          vertexColor, texCoord, texCoord1));
    }
    model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.meshRanges.push_back(range);
    appendMeshletClustersForRange(model, range, stop);
  }
}

float defaultPointPlaceholderRadius(const MeshData &mesh,
                                    const std::stop_token &stop) {
  const auto [center, radius] = computeBounds(mesh.points, stop);
  (void)center;
  if (std::isfinite(radius) && radius > 0.0f) {
    return std::clamp(radius * 0.025f, 0.02f, 1.0f);
  }
  return 0.05f;
}

float pointPlaceholderRadius(const MeshData &mesh, size_t pointIndex,
                             float fallback) {
  float width = 0.0f;
  if (pointIndex < mesh.widths.size()) {
    width = mesh.widths[pointIndex];
  } else if (mesh.widths.size() == 1u) {
    width = mesh.widths.front();
  }
  if (std::isfinite(width) && width > 0.0f) {
    return width * 0.5f;
  }
  return fallback;
}

void appendPlaceholderTriangle(dotbim::Model &model,
                               std::vector<glm::vec3> &rangePoints,
                               const glm::vec3 &a, const glm::vec3 &b,
                               const glm::vec3 &c,
                               const glm::vec3 &vertexColor) {
  const glm::vec2 texCoord(0.0f);
  const glm::vec3 normal = safeNormal(a, b, c);
  const glm::vec4 tangent(safeTangent(a, b, normal), 1.0f);
  const uint32_t base = static_cast<uint32_t>(model.vertices.size());
  model.vertices.push_back(
      makeVertex(a, normal, tangent, vertexColor, texCoord, texCoord));
  model.vertices.push_back(
      makeVertex(b, normal, tangent, vertexColor, texCoord, texCoord));
  model.vertices.push_back(
      makeVertex(c, normal, tangent, vertexColor, texCoord, texCoord));
  model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
  rangePoints.insert(rangePoints.end(), {a, b, c});
}

glm::vec3 fallbackPlaceholderColor(const glm::vec4 &color) {
  return glm::clamp(glm::vec3(color.r, color.g, color.b), glm::vec3(0.0f),
                    glm::vec3(1.0f));
}

glm::vec3 placeholderPointColor(const MeshData &mesh, size_t pointIndex,
                                const glm::vec4 &fallbackColor) {
  if (pointIndex < mesh.colors.size()) {
    return glm::clamp(mesh.colors[pointIndex], glm::vec3(0.0f),
                      glm::vec3(1.0f));
  }
  if (mesh.colors.size() == 1u) {
    return glm::clamp(mesh.colors.front(), glm::vec3(0.0f),
                      glm::vec3(1.0f));
  }
  return fallbackPlaceholderColor(fallbackColor);
}

glm::vec3 placeholderCurveSegmentColor(const MeshData &mesh, size_t first,
                                       size_t second,
                                       const glm::vec4 &fallbackColor) {
  const glm::vec3 firstColor =
      placeholderPointColor(mesh, first, fallbackColor);
  const glm::vec3 secondColor =
      placeholderPointColor(mesh, second, fallbackColor);
  return glm::clamp((firstColor + secondColor) * 0.5f, glm::vec3(0.0f),
                    glm::vec3(1.0f));
}

void appendNativePointRange(dotbim::Model &model, const MeshData &mesh,
                            uint32_t meshId, const glm::vec4 &color,
                            const std::stop_token &stop) {
  if (mesh.points.empty()) {
    return;
  }

  dotbim::NativePrimitiveRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(mesh.points, stop);
  range.boundsCenter = center;
  range.boundsRadius = radius;
  const glm::vec2 texCoord(0.0f);
  for (size_t pointIndex = 0; pointIndex < mesh.points.size(); ++pointIndex) {
    checkImportProgress(pointIndex, stop);
    const uint32_t vertexIndex = static_cast<uint32_t>(model.vertices.size());
    model.vertices.push_back(makeVertex(
        mesh.points[pointIndex], glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
        placeholderPointColor(mesh, pointIndex, color), texCoord, texCoord));
    model.indices.push_back(vertexIndex);
  }
  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.nativePointRanges.push_back(range);
  }
}

void appendNativeCurveRange(dotbim::Model &model, const MeshData &mesh,
                            const std::vector<uint32_t> &curveVertexCounts,
                            uint32_t meshId, const glm::vec4 &color,
                            std::string_view path, const std::stop_token &stop) {
  if (mesh.points.size() < 2u) {
    return;
  }

  dotbim::NativePrimitiveRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());
  const auto [center, radius] = computeBounds(mesh.points, stop);
  range.boundsCenter = center;
  range.boundsRadius = radius;
  const glm::vec2 texCoord(0.0f);
  size_t cursor = 0;
  for (size_t curve = 0; curve < curveVertexCounts.size(); ++curve) {
    checkImportProgress(curve, stop);
    const uint32_t count = curveVertexCounts[curve];
    if (cursor + static_cast<size_t>(count) > mesh.points.size()) {
      throw std::runtime_error("USD curve '" + std::string(path) +
                               "' has inconsistent curve vertex counts");
    }
    for (uint32_t i = 0; i + 1u < count; ++i) {
      const size_t first = cursor + i;
      checkImportProgress(first, stop);
      const size_t second = first + 1u;
      const glm::vec3 segment = mesh.points[second] - mesh.points[first];
      if (glm::dot(segment, segment) <= 1.0e-12f) {
        continue;
      }
      const uint32_t base = static_cast<uint32_t>(model.vertices.size());
      const glm::vec3 vertexColor =
          placeholderCurveSegmentColor(mesh, first, second, color);
      model.vertices.push_back(makeVertex(
          mesh.points[first], glm::vec3(0.0f, 1.0f, 0.0f),
          glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), vertexColor, texCoord,
          texCoord));
      model.vertices.push_back(makeVertex(
          mesh.points[second], glm::vec3(0.0f, 1.0f, 0.0f),
          glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), vertexColor, texCoord,
          texCoord));
      model.indices.insert(model.indices.end(), {base, base + 1u});
    }
    cursor += count;
  }
  if (cursor != mesh.points.size()) {
    throw std::runtime_error("USD curve '" + std::string(path) +
                             "' has unused points");
  }
  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount > 0u) {
    model.nativeCurveRanges.push_back(range);
  }
}

void appendMeshletClustersForRange(dotbim::Model &model,
                                   const dotbim::MeshRange &range,
                                   const std::stop_token &stop) {
  constexpr uint32_t kMeshletTriangleBudget = 64u;
  constexpr uint32_t kMeshletIndexBudget = kMeshletTriangleBudget * 3u;
  const uint32_t endIndex = range.firstIndex + range.indexCount;
  if (range.indexCount < 3u || endIndex > model.indices.size()) {
    return;
  }

  for (uint32_t firstIndex = range.firstIndex; firstIndex + 2u < endIndex;
       firstIndex += kMeshletIndexBudget) {
    checkModelImportCancellation(stop);
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
    const auto [center, radius] = computeBounds(clusterPoints, stop);
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

void appendPointPlaceholders(dotbim::Model &model, const MeshData &mesh,
                             uint32_t meshId, std::string_view path,
                             const glm::vec4 &color, const std::stop_token &stop) {
  (void)path;
  if (mesh.points.empty()) {
    return;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());

  const float fallbackRadius = defaultPointPlaceholderRadius(mesh, stop);
  std::vector<glm::vec3> rangePoints;
  rangePoints.reserve(mesh.points.size() * 24u);

  const glm::vec3 offsets[] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f},
                               {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
                               {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};
  const uint32_t faces[][3] = {{0u, 2u, 4u}, {2u, 1u, 4u},
                               {1u, 3u, 4u}, {3u, 0u, 4u},
                               {2u, 0u, 5u}, {1u, 2u, 5u},
                               {3u, 1u, 5u}, {0u, 3u, 5u}};

  for (size_t pointIndex = 0; pointIndex < mesh.points.size(); ++pointIndex) {
    checkImportProgress(pointIndex, stop);
    const glm::vec3 &point = mesh.points[pointIndex];
    const glm::vec3 vertexColor =
        placeholderPointColor(mesh, pointIndex, color);
    const float markerRadius =
        pointPlaceholderRadius(mesh, pointIndex, fallbackRadius);
    glm::vec3 markerPoints[6]{};
    for (size_t i = 0; i < std::size(offsets); ++i) {
      markerPoints[i] = point + offsets[i] * markerRadius;
    }

    for (const auto &face : faces) {
      const glm::vec3 &a = markerPoints[face[0]];
      const glm::vec3 &b = markerPoints[face[1]];
      const glm::vec3 &c = markerPoints[face[2]];
      appendPlaceholderTriangle(model, rangePoints, a, b, c, vertexColor);
    }
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount == 0u) {
    return;
  }
  const auto [center, radius] = computeBounds(rangePoints, stop);
  range.boundsCenter = center;
  range.boundsRadius = radius;
  model.meshRanges.push_back(range);
  appendNativePointRange(model, mesh, meshId, color, stop);
}

void appendCurveSegmentPlaceholder(dotbim::Model &model,
                                   std::vector<glm::vec3> &rangePoints,
                                   const glm::vec3 &a, const glm::vec3 &b,
                                   float radius,
                                   const glm::vec3 &vertexColor) {
  const glm::vec3 segment = b - a;
  const float len2 = glm::dot(segment, segment);
  if (!std::isfinite(len2) || len2 <= 1.0e-12f) {
    return;
  }

  const glm::vec3 direction = segment * (1.0f / std::sqrt(len2));
  const glm::vec3 reference =
      std::abs(direction.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                    : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 side =
      normalizeOrFallback(glm::cross(direction, reference),
                          glm::vec3(1.0f, 0.0f, 0.0f));
  const glm::vec3 up =
      normalizeOrFallback(glm::cross(side, direction),
                          glm::vec3(0.0f, 1.0f, 0.0f));
  radius = std::max(radius, 0.001f);

  const glm::vec3 s = side * radius;
  const glm::vec3 u = up * radius;
  const glm::vec3 corners[] = {a - s - u, a + s - u, a + s + u, a - s + u,
                               b - s - u, b + s - u, b + s + u, b - s + u};
  const uint32_t triangles[][3] = {
      {0u, 1u, 5u}, {0u, 5u, 4u}, {1u, 2u, 6u}, {1u, 6u, 5u},
      {2u, 3u, 7u}, {2u, 7u, 6u}, {3u, 0u, 4u}, {3u, 4u, 7u},
      {0u, 3u, 2u}, {0u, 2u, 1u}, {4u, 5u, 6u}, {4u, 6u, 7u}};

  for (const auto &triangle : triangles) {
    appendPlaceholderTriangle(model, rangePoints, corners[triangle[0]],
                              corners[triangle[1]], corners[triangle[2]],
                              vertexColor);
  }
}

void appendBasisCurvePlaceholders(dotbim::Model &model, const MeshData &mesh,
                                  uint32_t meshId, std::string_view path,
                                  const glm::vec4 &color,
                                  const std::stop_token &stop) {
  if (mesh.points.empty()) {
    return;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());

  const float fallbackRadius = defaultPointPlaceholderRadius(mesh, stop);
  std::vector<glm::vec3> rangePoints;
  rangePoints.reserve(mesh.points.size() * 36u);

  std::vector<uint32_t> curveVertexCounts = mesh.curveVertexCounts;
  if (curveVertexCounts.empty()) {
    curveVertexCounts.push_back(static_cast<uint32_t>(mesh.points.size()));
  }

  size_t cursor = 0;
  for (size_t curve = 0; curve < curveVertexCounts.size(); ++curve) {
    checkImportProgress(curve, stop);
    const uint32_t count = curveVertexCounts[curve];
    if (cursor + static_cast<size_t>(count) > mesh.points.size()) {
      throw std::runtime_error("USD curve '" + std::string(path) +
                               "' has inconsistent curve vertex counts");
    }
    if (count >= 2u) {
      for (uint32_t i = 0; i + 1u < count; ++i) {
        const size_t first = cursor + i;
        checkImportProgress(first, stop);
        const size_t second = first + 1u;
        const float radius =
            (pointPlaceholderRadius(mesh, first, fallbackRadius) +
             pointPlaceholderRadius(mesh, second, fallbackRadius)) *
            0.5f;
        const glm::vec3 vertexColor =
            placeholderCurveSegmentColor(mesh, first, second, color);
        appendCurveSegmentPlaceholder(model, rangePoints, mesh.points[first],
                                      mesh.points[second], radius,
                                      vertexColor);
      }
    }
    cursor += count;
  }

  if (cursor != mesh.points.size()) {
    throw std::runtime_error("USD curve '" + std::string(path) +
                             "' has unused points");
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  if (range.indexCount == 0u) {
    return;
  }
  const auto [center, radius] = computeBounds(rangePoints, stop);
  range.boundsCenter = center;
  range.boundsRadius = radius;
  model.meshRanges.push_back(range);
  appendNativeCurveRange(model, mesh, curveVertexCounts, meshId, color, path, stop);
}

#if defined(CONTAINER_HAS_TINYUSDZ)

std::string pathToTinyUsdUtf8(const std::filesystem::path &path) {
  const auto value = path.lexically_normal().generic_u8string();
  std::string result;
  result.reserve(value.size());
  for (const auto ch : value) {
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

float sanitizeColorComponent(float value, float fallback) {
  if (!std::isfinite(value)) {
    return fallback;
  }
  return std::clamp(value, 0.0f, 1.0f);
}

glm::vec4 colorFromTinyUsdMaterialId(
    const tinyusdz::tydra::RenderScene &scene, int materialId,
    glm::vec4 color);

glm::vec4 colorFromTinyUsdMaterial(const tinyusdz::tydra::RenderScene &scene,
                                   const tinyusdz::tydra::RenderMesh &mesh,
                                   int materialOverrideId) {
  glm::vec4 color{sanitizeColorComponent(mesh.displayColor.r, 0.8f),
                  sanitizeColorComponent(mesh.displayColor.g, 0.82f),
                  sanitizeColorComponent(mesh.displayColor.b, 0.86f),
                  sanitizeColorComponent(mesh.displayOpacity, 1.0f)};

  const int materialId =
      materialOverrideId >= 0 ? materialOverrideId : mesh.material_id;
  return colorFromTinyUsdMaterialId(scene, materialId, color);
}

glm::vec4 colorFromTinyUsdMaterialId(
    const tinyusdz::tydra::RenderScene &scene, int materialId,
    glm::vec4 color) {
  if (materialId < 0 ||
      static_cast<size_t>(materialId) >= scene.materials.size()) {
    return color;
  }

  const tinyusdz::tydra::RenderMaterial &material =
      scene.materials[static_cast<size_t>(materialId)];
  if (material.surfaceShader.has_value()) {
    const tinyusdz::tydra::PreviewSurfaceShader &shader =
        *material.surfaceShader;
    color.r = sanitizeColorComponent(shader.diffuseColor.value[0], color.r);
    color.g = sanitizeColorComponent(shader.diffuseColor.value[1], color.g);
    color.b = sanitizeColorComponent(shader.diffuseColor.value[2], color.b);
    color.a = sanitizeColorComponent(shader.opacity.value, color.a);
    return color;
  }

  if (material.openPBRShader.has_value()) {
    const tinyusdz::tydra::OpenPBRSurfaceShader &shader =
        *material.openPBRShader;
    color.r = sanitizeColorComponent(shader.base_color.value[0], color.r);
    color.g = sanitizeColorComponent(shader.base_color.value[1], color.g);
    color.b = sanitizeColorComponent(shader.base_color.value[2], color.b);
    color.a = sanitizeColorComponent(shader.opacity.value, color.a);
  }
  return color;
}

glm::mat4 matrixFromTinyUsd(const tinyusdz::value::matrix4d &matrix) {
  glm::mat4 result(1.0f);
  for (size_t column = 0; column < 4u; ++column) {
    for (size_t row = 0; row < 4u; ++row) {
      const double value = matrix.m[column][row];
      if (!std::isfinite(value)) {
        throw std::runtime_error(
            "USD node transform contains a non-finite value");
      }
      result[static_cast<glm::length_t>(column)]
            [static_cast<glm::length_t>(row)] = static_cast<float>(value);
    }
  }
  return result;
}

glm::vec2 tinyVec2(const tinyusdz::tydra::vec2 &value) {
  return {value[0], value[1]};
}

glm::vec3 tinyVec3(const tinyusdz::tydra::vec3 &value) {
  return {value[0], value[1], value[2]};
}

glm::vec3 tinyVec3(const tinyusdz::value::point3f &value) {
  return {value[0], value[1], value[2]};
}

glm::vec3 tinyVec3(const tinyusdz::value::color3f &value) {
  return {value[0], value[1], value[2]};
}

glm::vec4 tinyVec4(const tinyusdz::tydra::vec4 &value) {
  return {value[0], value[1], value[2], value[3]};
}

float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

uint32_t
texCoordSlotFromTinyUsdTexture(const tinyusdz::tydra::UVTexture &texture) {
  const std::string varname = lowerAscii(texture.varname_uv);
  if (varname == "st1" || varname == "uv1" || varname == "texcoord1" ||
      varname == "texcoord_1") {
    return 1u;
  }
  return 0u;
}

uint32_t
textureChannelFromTinyUsd(tinyusdz::tydra::UVTexture::Channel channel) {
  using Channel = tinyusdz::tydra::UVTexture::Channel;
  switch (channel) {
  case Channel::R:
    return 0u;
  case Channel::G:
    return 1u;
  case Channel::B:
    return 2u;
  case Channel::A:
    return 3u;
  case Channel::RGB:
  case Channel::RGBA:
  default:
    return 4u;
  }
}

container::material::TextureTransform
textureTransformFromTinyUsd(const tinyusdz::tydra::RenderScene &scene,
                            int32_t textureId) {
  container::material::TextureTransform transform{};
  if (textureId < 0 ||
      static_cast<size_t>(textureId) >= scene.textures.size()) {
    return transform;
  }

  const tinyusdz::tydra::UVTexture &texture =
      scene.textures[static_cast<size_t>(textureId)];
  transform.texCoord = texCoordSlotFromTinyUsdTexture(texture);
  transform.channel = textureChannelFromTinyUsd(texture.connectedOutputChannel);
  if (texture.has_transform2d) {
    transform.offset = tinyVec2(texture.tx_translation);
    transform.scale = tinyVec2(texture.tx_scale);
    transform.rotation = glm::radians(texture.tx_rotation);
  }
  return transform;
}

uint32_t
samplerWrapModeFromTinyUsd(tinyusdz::tydra::UVTexture::WrapMode wrapMode) {
  using WrapMode = tinyusdz::tydra::UVTexture::WrapMode;
  switch (wrapMode) {
  case WrapMode::REPEAT:
    return container::gpu::kMaterialSamplerWrapRepeat;
  case WrapMode::MIRROR:
    return container::gpu::kMaterialSamplerWrapMirroredRepeat;
  case WrapMode::CLAMP_TO_EDGE:
  case WrapMode::CLAMP_TO_BORDER:
  default:
    return container::gpu::kMaterialSamplerWrapClampToEdge;
  }
}

uint32_t
samplerIndexFromTinyUsdTexture(const tinyusdz::tydra::UVTexture &texture) {
  const uint32_t wrapS = samplerWrapModeFromTinyUsd(texture.wrapS);
  const uint32_t wrapT = samplerWrapModeFromTinyUsd(texture.wrapT);
  return wrapS + wrapT * container::gpu::kMaterialSamplerWrapModeCount;
}

void copyTinyUsdTextureBuffer(const tinyusdz::tydra::RenderScene &scene,
                              const tinyusdz::tydra::TextureImage &image,
                              dotbim::MaterialTextureAsset &asset) {
  if (image.buffer_id < 0 ||
      static_cast<size_t>(image.buffer_id) >= scene.buffers.size()) {
    return;
  }

  const auto &buffer = scene.buffers[static_cast<size_t>(image.buffer_id)];
  if (buffer.data.empty()) {
    return;
  }

  asset.encodedBytes.clear();
  asset.encodedBytes.reserve(buffer.data.size());
  for (uint8_t byte : buffer.data) {
    asset.encodedBytes.push_back(static_cast<std::byte>(byte));
  }
}

std::optional<dotbim::MaterialTextureAsset>
textureAssetFromTinyUsd(const tinyusdz::tydra::RenderScene &scene,
                        int32_t textureId,
                        const std::filesystem::path &sourceDir) {
  if (textureId < 0 ||
      static_cast<size_t>(textureId) >= scene.textures.size()) {
    return std::nullopt;
  }

  const tinyusdz::tydra::UVTexture &texture =
      scene.textures[static_cast<size_t>(textureId)];
  if (texture.texture_image_id < 0 ||
      static_cast<size_t>(texture.texture_image_id) >= scene.images.size()) {
    return std::nullopt;
  }

  const tinyusdz::tydra::TextureImage &image =
      scene.images[static_cast<size_t>(texture.texture_image_id)];

  dotbim::MaterialTextureAsset asset{};
  asset.samplerIndex = samplerIndexFromTinyUsdTexture(texture);
  asset.name = !image.asset_identifier.empty() ? image.asset_identifier
                                               : texture.abs_path;

  if (!image.asset_identifier.empty()) {
    std::filesystem::path path =
        container::util::pathFromUtf8(image.asset_identifier);
    if (path.is_relative()) {
      path = sourceDir / path;
    }
    asset.path = path.lexically_normal();
  }

  if (!image.decoded) {
    copyTinyUsdTextureBuffer(scene, image, asset);
  }

  return asset.empty()
             ? std::nullopt
             : std::optional<dotbim::MaterialTextureAsset>(std::move(asset));
}

template <typename T>
void assignTinyUsdTexture(
    const tinyusdz::tydra::RenderScene &scene,
    const tinyusdz::tydra::ShaderParam<T> &parameter,
    const std::filesystem::path &sourceDir,
    dotbim::MaterialTextureAsset &textureAsset,
    container::material::TextureTransform &textureTransform) {
  if (!parameter.is_texture()) {
    return;
  }
  if (auto resolved =
          textureAssetFromTinyUsd(scene, parameter.texture_id, sourceDir)) {
    textureAsset = std::move(*resolved);
    textureTransform = textureTransformFromTinyUsd(scene, parameter.texture_id);
  }
}

container::material::AlphaMode
alphaModeFromTinyUsdMaterial(const tinyusdz::tydra::RenderMaterial &material) {
  if (material.materialTag == tinyusdz::tydra::MaterialTag::Masked) {
    return container::material::AlphaMode::Mask;
  }
  if (material.materialTag == tinyusdz::tydra::MaterialTag::Translucent) {
    return container::material::AlphaMode::Blend;
  }
  if (material.surfaceShader.has_value()) {
    const auto &shader = *material.surfaceShader;
    if (shader.opacityThreshold.value > 0.0f) {
      return container::material::AlphaMode::Mask;
    }
    if (shader.opacity.value < 0.999f || shader.opacity.is_texture()) {
      return container::material::AlphaMode::Blend;
    }
  }
  if (material.openPBRShader.has_value()) {
    const auto &shader = *material.openPBRShader;
    if (shader.opacity.value < 0.999f || shader.opacity.is_texture() ||
        shader.transmission_weight.value > 0.001f ||
        shader.transmission_weight.is_texture()) {
      return container::material::AlphaMode::Blend;
    }
  }
  return container::material::AlphaMode::Opaque;
}

dotbim::Material
convertTinyUsdMaterial(const tinyusdz::tydra::RenderScene &scene,
                       const tinyusdz::tydra::RenderMaterial &source,
                       const std::filesystem::path &sourceDir) {
  dotbim::Material material{};
  material.pbr.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
  material.pbr.roughnessFactor = 0.5f;
  material.pbr.alphaMode = alphaModeFromTinyUsdMaterial(source);

  if (source.surfaceShader.has_value()) {
    const auto &shader = *source.surfaceShader;
    material.pbr.baseColor =
        glm::vec4(glm::clamp(tinyVec3(shader.diffuseColor.value),
                             glm::vec3(0.0f), glm::vec3(1.0f)),
                  1.0f);
    material.pbr.emissiveColor =
        glm::clamp(tinyVec3(shader.emissiveColor.value), glm::vec3(0.0f),
                   glm::vec3(std::numeric_limits<float>::max()));
    material.pbr.roughnessFactor =
        std::clamp(finiteOr(shader.roughness.value, 0.5f), 0.0f, 1.0f);
    if (shader.useSpecularWorkflow) {
      material.pbr.metallicFactor = 0.0f;
      material.pbr.specularFactor = 1.0f;
      material.pbr.specularColorFactor =
          glm::clamp(tinyVec3(shader.specularColor.value), glm::vec3(0.0f),
                     glm::vec3(1.0f));
    } else {
      material.pbr.metallicFactor =
          std::clamp(finiteOr(shader.metallic.value, 0.0f), 0.0f, 1.0f);
    }
    material.pbr.opacityFactor =
        std::clamp(finiteOr(shader.opacity.value, 1.0f), 0.0f, 1.0f);
    material.pbr.alphaCutoff =
        std::clamp(finiteOr(shader.opacityThreshold.value, 0.5f), 0.0f, 1.0f);
    material.pbr.ior = std::max(finiteOr(shader.ior.value, 1.5f), 1.0f);
    material.pbr.clearcoatFactor =
        std::clamp(finiteOr(shader.clearcoat.value, 0.0f), 0.0f, 1.0f);
    material.pbr.clearcoatRoughnessFactor = std::clamp(
        finiteOr(shader.clearcoatRoughness.value, 0.01f), 0.0f, 1.0f);

    assignTinyUsdTexture(scene, shader.diffuseColor, sourceDir,
                         material.texturePaths.baseColor,
                         material.pbr.baseColorTextureTransform);
    assignTinyUsdTexture(scene, shader.emissiveColor, sourceDir,
                         material.texturePaths.emissive,
                         material.pbr.emissiveTextureTransform);
    if (shader.useSpecularWorkflow) {
      assignTinyUsdTexture(scene, shader.specularColor, sourceDir,
                           material.texturePaths.specularColor,
                           material.pbr.specularColorTextureTransform);
    }
    assignTinyUsdTexture(scene, shader.normal, sourceDir,
                         material.texturePaths.normal,
                         material.pbr.normalTextureTransform);
    assignTinyUsdTexture(scene, shader.occlusion, sourceDir,
                         material.texturePaths.occlusion,
                         material.pbr.occlusionTextureTransform);
    assignTinyUsdTexture(scene, shader.metallic, sourceDir,
                         material.texturePaths.metalness,
                         material.pbr.metalnessTextureTransform);
    assignTinyUsdTexture(scene, shader.roughness, sourceDir,
                         material.texturePaths.roughness,
                         material.pbr.roughnessTextureTransform);
    assignTinyUsdTexture(scene, shader.opacity, sourceDir,
                         material.texturePaths.opacity,
                         material.pbr.opacityTextureTransform);
    assignTinyUsdTexture(scene, shader.clearcoat, sourceDir,
                         material.texturePaths.clearcoat,
                         material.pbr.clearcoatTextureTransform);
    assignTinyUsdTexture(scene, shader.clearcoatRoughness, sourceDir,
                         material.texturePaths.clearcoatRoughness,
                         material.pbr.clearcoatRoughnessTextureTransform);
    return material;
  }

  if (source.openPBRShader.has_value()) {
    const auto &shader = *source.openPBRShader;
    material.pbr.baseColor =
        glm::vec4(glm::clamp(tinyVec3(shader.base_color.value), glm::vec3(0.0f),
                             glm::vec3(1.0f)),
                  1.0f);
    material.pbr.metallicFactor =
        std::clamp(finiteOr(shader.base_metalness.value, 0.0f), 0.0f, 1.0f);
    material.pbr.roughnessFactor =
        std::clamp(finiteOr(shader.base_roughness.value, 0.5f), 0.0f, 1.0f);
    material.pbr.opacityFactor =
        std::clamp(finiteOr(shader.opacity.value, 1.0f), 0.0f, 1.0f);
    material.pbr.transmissionFactor = std::clamp(
        finiteOr(shader.transmission_weight.value, 0.0f), 0.0f, 1.0f);
    material.pbr.specularFactor =
        std::clamp(finiteOr(shader.specular_weight.value, 1.0f), 0.0f, 1.0f);
    material.pbr.specularColorFactor =
        glm::clamp(tinyVec3(shader.specular_color.value), glm::vec3(0.0f),
                   glm::vec3(1.0f));
    material.pbr.ior =
        std::max(finiteOr(shader.specular_ior.value, 1.5f), 1.0f);
    material.pbr.dispersion =
        std::max(finiteOr(shader.transmission_dispersion.value, 0.0f), 0.0f);
    material.pbr.clearcoatFactor =
        std::clamp(finiteOr(shader.coat_weight.value, 0.0f), 0.0f, 1.0f);
    material.pbr.clearcoatRoughnessFactor =
        std::clamp(finiteOr(shader.coat_roughness.value, 0.0f), 0.0f, 1.0f);
    material.pbr.emissiveColor =
        glm::clamp(tinyVec3(shader.emission_color.value), glm::vec3(0.0f),
                   glm::vec3(std::numeric_limits<float>::max()));
    material.pbr.emissiveStrength =
        std::max(finiteOr(shader.emission_luminance.value, 0.0f), 0.0f);
    material.pbr.sheenColorFactor = glm::clamp(
        tinyVec3(shader.sheen_color.value) *
            std::clamp(finiteOr(shader.sheen_weight.value, 0.0f), 0.0f, 1.0f),
        glm::vec3(0.0f), glm::vec3(1.0f));
    material.pbr.sheenRoughnessFactor =
        std::clamp(finiteOr(shader.sheen_roughness.value, 0.3f), 0.0f, 1.0f);
    material.pbr.iridescenceFactor =
        std::clamp(finiteOr(shader.thin_film_weight.value, 0.0f), 0.0f, 1.0f);
    material.pbr.iridescenceIor =
        std::max(finiteOr(shader.thin_film_ior.value, 1.3f), 1.0f);
    const float thinFilmThickness =
        std::max(finiteOr(shader.thin_film_thickness.value, 400.0f), 0.0f);
    material.pbr.iridescenceThicknessMinimum = thinFilmThickness;
    material.pbr.iridescenceThicknessMaximum = thinFilmThickness;

    assignTinyUsdTexture(scene, shader.base_color, sourceDir,
                         material.texturePaths.baseColor,
                         material.pbr.baseColorTextureTransform);
    assignTinyUsdTexture(scene, shader.normal, sourceDir,
                         material.texturePaths.normal,
                         material.pbr.normalTextureTransform);
    assignTinyUsdTexture(scene, shader.opacity, sourceDir,
                         material.texturePaths.opacity,
                         material.pbr.opacityTextureTransform);
    assignTinyUsdTexture(scene, shader.transmission_weight, sourceDir,
                         material.texturePaths.transmission,
                         material.pbr.transmissionTextureTransform);
    assignTinyUsdTexture(scene, shader.specular_weight, sourceDir,
                         material.texturePaths.specular,
                         material.pbr.specularTextureTransform);
    assignTinyUsdTexture(scene, shader.specular_color, sourceDir,
                         material.texturePaths.specularColor,
                         material.pbr.specularColorTextureTransform);
    assignTinyUsdTexture(scene, shader.coat_weight, sourceDir,
                         material.texturePaths.clearcoat,
                         material.pbr.clearcoatTextureTransform);
    assignTinyUsdTexture(scene, shader.coat_roughness, sourceDir,
                         material.texturePaths.clearcoatRoughness,
                         material.pbr.clearcoatRoughnessTextureTransform);
    assignTinyUsdTexture(scene, shader.coat_normal, sourceDir,
                         material.texturePaths.clearcoatNormal,
                         material.pbr.clearcoatNormalTextureTransform);
    assignTinyUsdTexture(scene, shader.emission_color, sourceDir,
                         material.texturePaths.emissive,
                         material.pbr.emissiveTextureTransform);
    assignTinyUsdTexture(scene, shader.sheen_color, sourceDir,
                         material.texturePaths.sheenColor,
                         material.pbr.sheenColorTextureTransform);
    assignTinyUsdTexture(scene, shader.sheen_roughness, sourceDir,
                         material.texturePaths.sheenRoughness,
                         material.pbr.sheenRoughnessTextureTransform);
    assignTinyUsdTexture(scene, shader.thin_film_weight, sourceDir,
                         material.texturePaths.iridescence,
                         material.pbr.iridescenceTextureTransform);
    assignTinyUsdTexture(scene, shader.thin_film_thickness, sourceDir,
                         material.texturePaths.iridescenceThickness,
                         material.pbr.iridescenceThicknessTextureTransform);
  }
  return material;
}

bool isValidTinyUsdMaterial(const tinyusdz::tydra::RenderScene &scene,
                            int materialId) {
  return materialId >= 0 &&
         static_cast<size_t>(materialId) < scene.materials.size();
}

uint32_t materialIndexFromTinyUsd(const tinyusdz::tydra::RenderScene &scene,
                                  int materialId) {
  return isValidTinyUsdMaterial(scene, materialId)
             ? static_cast<uint32_t>(materialId)
             : std::numeric_limits<uint32_t>::max();
}

struct TinyUsdPrimMetadata {
  std::string path{};
  std::string displayName{};
  std::string guid{};
  std::string semanticType{};
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
  std::string primType{};
};

std::optional<std::string>
stringFromTinyUsdMetaVariable(const tinyusdz::MetaVariable &variable) {
  if (const auto value = variable.get_value<std::string>()) {
    return *value;
  }
  if (const auto value = variable.get_value<tinyusdz::value::StringData>()) {
    return value->value;
  }
  if (const auto value = variable.get_value<tinyusdz::value::token>()) {
    return value->str();
  }
  return std::nullopt;
}

std::optional<std::string>
stringFromTinyUsdAttribute(const tinyusdz::Attribute &attribute) {
  if (!attribute.has_value()) {
    return std::nullopt;
  }
  if (const auto value = attribute.get_value<std::string>()) {
    return *value;
  }
  if (const auto value = attribute.get_value<tinyusdz::value::StringData>()) {
    return value->value;
  }
  if (const auto value = attribute.get_value<tinyusdz::value::token>()) {
    return value->str();
  }
  return std::nullopt;
}

std::optional<std::string>
stringFromTinyUsdProperty(const tinyusdz::Property &property) {
  const tinyusdz::Attribute *attribute = property.get_attribute_or_null();
  if (attribute == nullptr) {
    return std::nullopt;
  }
  return stringFromTinyUsdAttribute(*attribute);
}

void applyTinyUsdDictionaryMetadata(const tinyusdz::Dictionary &dictionary,
                                    TinyUsdPrimMetadata &metadata) {
  for (const auto &[name, variable] : dictionary) {
    if (const auto nested = variable.get_value<tinyusdz::Dictionary>()) {
      applyTinyUsdDictionaryMetadata(*nested, metadata);
    }
    if (const auto value = stringFromTinyUsdMetaVariable(variable)) {
      applyCommonMetadataValue(name, *value, metadata.guid,
                               metadata.semanticType, metadata.displayName,
                               &metadata.objectType, &metadata.storeyName,
                               &metadata.storeyId, &metadata.materialName,
                               &metadata.materialCategory,
                               &metadata.discipline, &metadata.phase,
                               &metadata.fireRating, &metadata.loadBearing,
                               &metadata.status);
    }
  }
}

void applyTinyUsdPropertyMetadata(
    const std::map<std::string, tinyusdz::Property> &properties,
    TinyUsdPrimMetadata &metadata) {
  for (const auto &[name, property] : properties) {
    if (!isGuidMetadataName(name) && !isTypeMetadataName(name) &&
        !isDisplayNameMetadataName(name) &&
        !isObjectTypeMetadataName(name) &&
        !isStoreyNameMetadataName(name) && !isStoreyIdMetadataName(name) &&
        !isMaterialNameMetadataName(name) &&
        !isMaterialCategoryMetadataName(name) &&
        !isDisciplineMetadataName(name) && !isPhaseMetadataName(name) &&
        !isFireRatingMetadataName(name) && !isLoadBearingMetadataName(name) &&
        !isStatusMetadataName(name)) {
      continue;
    }
    if (const auto value = stringFromTinyUsdProperty(property)) {
      applyCommonMetadataValue(name, *value, metadata.guid,
                               metadata.semanticType, metadata.displayName,
                               &metadata.objectType, &metadata.storeyName,
                               &metadata.storeyId, &metadata.materialName,
                               &metadata.materialCategory,
                               &metadata.discipline, &metadata.phase,
                               &metadata.fireRating, &metadata.loadBearing,
                               &metadata.status);
    }
  }
}

template <typename PrimT>
const std::map<std::string, tinyusdz::Property> *
tinyUsdPrimPropertiesAs(const tinyusdz::Prim &prim) {
  if (const PrimT *typed = prim.as<PrimT>()) {
    return &typed->props;
  }
  return nullptr;
}

const std::map<std::string, tinyusdz::Property> *
tinyUsdPrimProperties(const tinyusdz::Prim &prim) {
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::GeomMesh>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::Xform>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::Model>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::Scope>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::GeomCube>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::GeomSphere>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::GeomCone>(prim)) {
    return props;
  }
  if (const auto *props =
          tinyUsdPrimPropertiesAs<tinyusdz::GeomCylinder>(prim)) {
    return props;
  }
  if (const auto *props =
          tinyUsdPrimPropertiesAs<tinyusdz::GeomCapsule>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::GeomPlane>(prim)) {
    return props;
  }
  if (const auto *props =
          tinyUsdPrimPropertiesAs<tinyusdz::GeomBasisCurves>(prim)) {
    return props;
  }
  if (const auto *props =
          tinyUsdPrimPropertiesAs<tinyusdz::GeomNurbsCurves>(prim)) {
    return props;
  }
  if (const auto *props = tinyUsdPrimPropertiesAs<tinyusdz::GeomPoints>(prim)) {
    return props;
  }
  if (const auto *props =
          tinyUsdPrimPropertiesAs<tinyusdz::GeomPointInstancer>(prim)) {
    return props;
  }
  return nullptr;
}

TinyUsdPrimMetadata tinyUsdPrimMetadata(const tinyusdz::Stage &stage,
                                        std::string_view path) {
  TinyUsdPrimMetadata metadata{};
  if (path.empty()) {
    return metadata;
  }
  metadata.path = std::string(path);

  const tinyusdz::Prim *prim = nullptr;
  std::string err;
  if (!stage.find_prim_at_path(tinyusdz::Path(metadata.path, ""), prim, &err) ||
      prim == nullptr) {
    return metadata;
  }

  metadata.primType =
      !prim->prim_type_name().empty() ? prim->prim_type_name()
                                      : prim->type_name();
  if (prim->metas().has_displayName()) {
    metadata.displayName = prim->metas().get_displayName();
  }
  if (prim->metas().has_customData()) {
    applyTinyUsdDictionaryMetadata(prim->metas().get_customData(), metadata);
  }
  if (const auto *properties = tinyUsdPrimProperties(*prim)) {
    applyTinyUsdPropertyMetadata(*properties, metadata);
  }
  return metadata;
}

TinyUsdPrimMetadata mergeTinyUsdMetadata(TinyUsdPrimMetadata primary,
                                         const TinyUsdPrimMetadata &fallback) {
  if (primary.path.empty()) {
    primary.path = fallback.path;
  }
  if (primary.displayName.empty()) {
    primary.displayName = fallback.displayName;
  }
  if (primary.guid.empty()) {
    primary.guid = fallback.guid;
  }
  if (primary.semanticType.empty()) {
    primary.semanticType = fallback.semanticType;
  }
  if (primary.objectType.empty()) {
    primary.objectType = fallback.objectType;
  }
  if (primary.storeyName.empty()) {
    primary.storeyName = fallback.storeyName;
  }
  if (primary.storeyId.empty()) {
    primary.storeyId = fallback.storeyId;
  }
  if (primary.materialName.empty()) {
    primary.materialName = fallback.materialName;
  }
  if (primary.materialCategory.empty()) {
    primary.materialCategory = fallback.materialCategory;
  }
  if (primary.discipline.empty()) {
    primary.discipline = fallback.discipline;
  }
  if (primary.phase.empty()) {
    primary.phase = fallback.phase;
  }
  if (primary.fireRating.empty()) {
    primary.fireRating = fallback.fireRating;
  }
  if (primary.loadBearing.empty()) {
    primary.loadBearing = fallback.loadBearing;
  }
  if (primary.status.empty()) {
    primary.status = fallback.status;
  }
  if (primary.primType.empty()) {
    primary.primType = fallback.primType;
  }
  return primary;
}

std::string parentUsdPrimPath(std::string_view path) {
  if (path.empty()) {
    return {};
  }
  size_t end = path.size();
  while (end > 1u && path[end - 1u] == '/') {
    --end;
  }
  const std::string_view trimmed = path.substr(0u, end);
  const size_t separator = trimmed.find_last_of('/');
  if (separator == std::string_view::npos || separator == 0u) {
    return {};
  }
  return std::string(trimmed.substr(0u, separator));
}

TinyUsdPrimMetadata tinyUsdInheritedPrimMetadata(const tinyusdz::Stage &stage,
                                                 std::string_view path) {
  TinyUsdPrimMetadata metadata = tinyUsdPrimMetadata(stage, path);
  for (std::string parent = parentUsdPrimPath(path); !parent.empty();
       parent = parentUsdPrimPath(parent)) {
    metadata =
        mergeTinyUsdMetadata(std::move(metadata),
                             tinyUsdPrimMetadata(stage, parent));
  }
  return metadata;
}

std::string tinyUsdElementGuid(const TinyUsdPrimMetadata &metadata,
                               std::string_view renderPath,
                               std::string_view fallbackPrefix, int32_t id,
                               uint32_t meshId) {
  if (!metadata.guid.empty()) {
    return metadata.guid;
  }

  std::string base =
      !metadata.path.empty()
          ? metadata.path
          : (!renderPath.empty()
                 ? std::string(renderPath)
                 : (!metadata.displayName.empty()
                        ? metadata.displayName
                        : std::string(fallbackPrefix) + std::to_string(id)));
  base += "#mesh=" + std::to_string(meshId);
  return base;
}

std::string tinyUsdElementType(const TinyUsdPrimMetadata &metadata,
                               std::string_view fallback,
                               bool usePrimType) {
  if (!metadata.semanticType.empty()) {
    return metadata.semanticType;
  }
  return usePrimType ? usdFallbackType(metadata.primType, fallback)
                     : std::string(fallback);
}

const tinyusdz::GPrim *tinyUsdGPrim(const tinyusdz::Prim &prim) {
  if (const auto *typed = prim.as<tinyusdz::Xform>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomMesh>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomCube>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomSphere>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomCone>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomCylinder>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomCapsule>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomPlane>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomBasisCurves>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomNurbsCurves>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomPoints>()) {
    return typed;
  }
  if (const auto *typed = prim.as<tinyusdz::GeomPointInstancer>()) {
    return typed;
  }
  return nullptr;
}

bool tinyUsdPrimIsPointsLike(const tinyusdz::Prim &prim) {
  return prim.as<tinyusdz::GeomPoints>() != nullptr ||
         prim.prim_type_name() == "Points" || prim.type_name() == "Points" ||
         prim.type_name() == "GeomPoints";
}

bool tinyUsdPrimIsCurveLike(const tinyusdz::Prim &prim) {
  return prim.as<tinyusdz::GeomBasisCurves>() != nullptr ||
         prim.as<tinyusdz::GeomNurbsCurves>() != nullptr ||
         prim.prim_type_name() == "BasisCurves" ||
         prim.prim_type_name() == "NurbsCurves" ||
         prim.type_name() == "BasisCurves" ||
         prim.type_name() == "NurbsCurves" ||
         prim.type_name() == "GeomBasisCurves" ||
         prim.type_name() == "GeomNurbsCurves";
}

bool tinyUsdPrimNeedsPlaceholderGeometry(const tinyusdz::Prim &prim) {
  return tinyUsdPrimIsPointsLike(prim) || tinyUsdPrimIsCurveLike(prim);
}

template <typename VisibilityAttribute>
bool tinyUsdVisibilityIsVisible(const VisibilityAttribute &visibility,
                                bool inheritedVisible) {
  if (!inheritedVisible || !visibility.authored()) {
    return inheritedVisible;
  }

  const auto &value = visibility.get_value();
  tinyusdz::Visibility defaultValue{tinyusdz::Visibility::Inherited};
  if (value.get_default(&defaultValue) &&
      defaultValue == tinyusdz::Visibility::Invisible) {
    return false;
  }
  return true;
}

bool tinyUsdPrimIsVisible(const tinyusdz::Prim &prim, bool inheritedVisible) {
  if (const tinyusdz::GPrim *gprim = tinyUsdGPrim(prim)) {
    return tinyUsdVisibilityIsVisible(gprim->visibility, inheritedVisible);
  }
  if (const auto *scope = prim.as<tinyusdz::Scope>()) {
    return tinyUsdVisibilityIsVisible(scope->visibility, inheritedVisible);
  }
  return inheritedVisible;
}

std::string tinyUsdPrimPathString(const tinyusdz::Prim &prim,
                                  std::string_view parentPath) {
  std::string name = prim.element_name();
  if (name.empty()) {
    if (const auto elementName = tinyusdz::GetPrimElementName(prim.data())) {
      name = *elementName;
    }
  }

  std::string path = prim.absolute_path().prim_part();
  if (!path.empty()) {
    if (!parentPath.empty() && parentPath != "/" && !name.empty()) {
      const std::string parentPrefix = std::string(parentPath) + "/";
      if (!startsWith(path, parentPrefix)) {
        return parentPrefix + name;
      }
    }
    return path;
  }

  if (name.empty()) {
    return std::string(parentPath);
  }
  if (parentPath.empty() || parentPath == "/") {
    return "/" + name;
  }
  return std::string(parentPath) + "/" + name;
}

std::string tinyUsdPathLeaf(std::string_view path) {
  if (path.empty()) {
    return {};
  }
  size_t end = path.size();
  while (end > 1u && path[end - 1u] == '/') {
    --end;
  }
  const size_t separator = path.substr(0u, end).find_last_of('/');
  if (separator == std::string_view::npos) {
    return std::string(path.substr(0u, end));
  }
  return std::string(path.substr(separator + 1u, end - separator - 1u));
}

tinyusdz::value::matrix4d tinyUsdGlobalMatrixForPrim(
    const tinyusdz::Prim &prim,
    const tinyusdz::value::matrix4d &parentMatrix,
    std::string_view path) {
  const tinyusdz::GPrim *gprim = tinyUsdGPrim(prim);
  if (gprim == nullptr) {
    return parentMatrix;
  }

  auto matrix = gprim->GetGlobalMatrix(parentMatrix);
  if (!matrix) {
    throw std::runtime_error("USD prim '" + std::string(path) +
                             "' has an invalid transform: " + matrix.error());
  }
  return matrix.value();
}

glm::vec4 tinyUsdDisplayColor(const tinyusdz::Stage &stage,
                              std::string_view path) {
  glm::vec4 color{0.8f, 0.82f, 0.86f, 1.0f};
  if (path.empty()) {
    return color;
  }

  tinyusdz::GeomPrimvar displayColor;
  std::string err;
  if (tinyusdz::tydra::FindPrimvarWithInheritance(
          stage, tinyusdz::Path(std::string(path), ""), "displayColor",
          &displayColor, &err)) {
    std::vector<tinyusdz::value::color3f> colors;
    tinyusdz::value::color3f scalarColor{};
    if (displayColor.get_value(&colors) && !colors.empty()) {
      color = glm::vec4(glm::clamp(tinyVec3(colors.front()), glm::vec3(0.0f),
                                   glm::vec3(1.0f)),
                        color.a);
    } else if (displayColor.get_value(&scalarColor)) {
      color = glm::vec4(glm::clamp(tinyVec3(scalarColor), glm::vec3(0.0f),
                                   glm::vec3(1.0f)),
                        color.a);
    }
  }

  tinyusdz::GeomPrimvar displayOpacity;
  if (tinyusdz::tydra::FindPrimvarWithInheritance(
          stage, tinyusdz::Path(std::string(path), ""), "displayOpacity",
          &displayOpacity, &err)) {
    std::vector<float> opacities;
    float scalarOpacity = color.a;
    if (displayOpacity.get_value(&opacities) && !opacities.empty()) {
      color.a = sanitizeColorComponent(opacities.front(), color.a);
    } else if (displayOpacity.get_value(&scalarOpacity)) {
      color.a = sanitizeColorComponent(scalarOpacity, color.a);
    }
  }
  return color;
}

const tinyusdz::MaterialBinding *
tinyUsdMaterialBinding(const tinyusdz::Prim &prim) {
  if (const tinyusdz::GPrim *gprim = tinyUsdGPrim(prim)) {
    return gprim;
  }
  if (const auto *scope = prim.as<tinyusdz::Scope>()) {
    return scope;
  }
  if (const auto *model = prim.as<tinyusdz::Model>()) {
    return model;
  }
  return nullptr;
}

std::optional<std::string> tinyUsdDirectMaterialPath(const tinyusdz::Prim &prim) {
  const tinyusdz::MaterialBinding *binding = tinyUsdMaterialBinding(prim);
  if (binding != nullptr) {
    tinyusdz::Relationship relationship;
    if (binding->get_materialBinding(tinyusdz::value::token(""),
                                     &relationship)) {
      if (relationship.is_path()) {
        return relationship.targetPath.prim_part();
      }
      if (relationship.is_pathvector() &&
          !relationship.targetPathVector.empty()) {
        return relationship.targetPathVector.front().prim_part();
      }
    }
  }

  if (const auto *properties = tinyUsdPrimProperties(prim)) {
    if (const auto it = properties->find("material:binding");
        it != properties->end()) {
      if (const auto target = it->second.get_relationTarget()) {
        return target->prim_part();
      }
    }
  }
  return std::nullopt;
}

bool tinyUsdMaterialPathMatches(std::string_view scenePath,
                                std::string_view materialPath) {
  if (scenePath == materialPath) {
    return true;
  }
  return !materialPath.empty() && scenePath.ends_with(materialPath);
}

int tinyUsdMaterialIndexForPath(
    const tinyusdz::tydra::RenderScene &scene, std::string_view materialPath) {
  if (materialPath.empty()) {
    return -1;
  }
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    if (tinyUsdMaterialPathMatches(scene.materials[i].abs_path, materialPath)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const tinyusdz::Material *tinyUsdMaterialAtPath(const tinyusdz::Stage &stage,
                                                std::string_view path) {
  if (path.empty()) {
    return nullptr;
  }
  const tinyusdz::Prim *prim = nullptr;
  std::string err;
  if (!stage.find_prim_at_path(tinyusdz::Path(std::string(path), ""), prim,
                               &err) ||
      prim == nullptr) {
    return nullptr;
  }
  return prim->as<tinyusdz::Material>();
}

struct TinyUsdBoundMaterial {
  std::string path{};
  const tinyusdz::Material *material{nullptr};
};

std::optional<TinyUsdBoundMaterial>
tinyUsdBoundMaterialForPrim(const tinyusdz::Stage &stage,
                            std::string_view path) {
  if (path.empty()) {
    return std::nullopt;
  }

  tinyusdz::Path materialPath;
  const tinyusdz::Material *material = nullptr;
  std::string err;
  if (tinyusdz::tydra::GetBoundMaterial(
          stage, tinyusdz::Path(std::string(path), ""), "", &materialPath,
          &material, &err)) {
    const std::string resolvedPath = materialPath.prim_part();
    if (material == nullptr) {
      material = tinyUsdMaterialAtPath(stage, resolvedPath);
    }
    if (!resolvedPath.empty() && material != nullptr) {
      return TinyUsdBoundMaterial{resolvedPath, material};
    }
  }

  for (std::string current = std::string(path); !current.empty();
       current = parentUsdPrimPath(current)) {
    const tinyusdz::Prim *prim = nullptr;
    if (std::string directErr; stage.find_prim_at_path(
            tinyusdz::Path(current, ""), prim, &directErr) &&
        prim != nullptr) {
      if (const auto directPath = tinyUsdDirectMaterialPath(*prim)) {
        if (const tinyusdz::Material *directMaterial =
                tinyUsdMaterialAtPath(stage, *directPath)) {
          return TinyUsdBoundMaterial{*directPath, directMaterial};
        }
      }
    }
  }
  return std::nullopt;
}

int tinyUsdMaterialIndexForPrim(const tinyusdz::Stage &stage,
                                const tinyusdz::tydra::RenderScene &scene,
                                std::string_view path) {
  if (const auto boundMaterial = tinyUsdBoundMaterialForPrim(stage, path)) {
    return tinyUsdMaterialIndexForPath(scene, boundMaterial->path);
  }
  return -1;
}

MeshData tinyUsdPointsMeshData(const tinyusdz::GeomPoints &points,
                               std::string_view path,
                               const std::stop_token &stop) {
  MeshData mesh{};
  const std::vector<tinyusdz::value::point3f> sourcePoints =
      points.get_points();
  if (sourcePoints.size() >
      static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::runtime_error("USD points prim '" + std::string(path) +
                             "' has too many points for 32-bit indices");
  }

  mesh.points.reserve(sourcePoints.size());
  for (const tinyusdz::value::point3f &point : sourcePoints) {
    checkImportProgress(mesh.points.size(), stop);
    const glm::vec3 value = tinyVec3(point);
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z)) {
      throw std::runtime_error("USD points prim '" + std::string(path) +
                               "' contains a non-finite point");
    }
    mesh.points.push_back(value);
  }
  mesh.widths = points.get_widths();
  return mesh;
}

template <typename CurveT>
MeshData tinyUsdCurveMeshData(const CurveT &curves, std::string_view path,
                              const std::stop_token &stop) {
  MeshData mesh{};
  const std::vector<tinyusdz::value::point3f> sourcePoints =
      curves.get_points();
  if (sourcePoints.size() >
      static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::runtime_error("USD curve prim '" + std::string(path) +
                             "' has too many points for 32-bit indices");
  }

  mesh.points.reserve(sourcePoints.size());
  for (const tinyusdz::value::point3f &point : sourcePoints) {
    checkImportProgress(mesh.points.size(), stop);
    const glm::vec3 value = tinyVec3(point);
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z)) {
      throw std::runtime_error("USD curve prim '" + std::string(path) +
                               "' contains a non-finite point");
    }
    mesh.points.push_back(value);
  }

  for (int count : curves.get_curveVertexCounts()) {
    checkImportProgress(mesh.curveVertexCounts.size(), stop);
    if (count < 0 ||
        static_cast<uint64_t>(count) >
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
      throw std::runtime_error("USD curve prim '" + std::string(path) +
                               "' has an invalid curve vertex count");
    }
    mesh.curveVertexCounts.push_back(static_cast<uint32_t>(count));
  }
  mesh.widths = curves.get_widths();
  return mesh;
}

dotbim::Element *modelElementBySource(dotbim::Model &model,
                                      std::string_view path,
                                      const std::stop_token &stop) {
  for (size_t i = 0; i < model.elements.size(); ++i) {
    checkImportProgress(i, stop);
    auto &element = model.elements[i];
    if (element.sourceId == path) {
      return &element;
    }
  }
  return nullptr;
}

void applyTinyUsdPointsElementFields(
    dotbim::Element &element, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderScene &scene, const tinyusdz::Prim &prim,
    const tinyusdz::GeomPoints &points, const glm::mat4 &importTransform,
    const tinyusdz::value::matrix4d &globalMatrix, std::string_view path,
    uint32_t meshId) {
  const int materialId = tinyUsdMaterialIndexForPrim(stage, scene, path);
  const glm::vec4 displayColor = tinyUsdDisplayColor(stage, path);
  const glm::vec4 color =
      colorFromTinyUsdMaterialId(scene, materialId, displayColor);

  TinyUsdPrimMetadata metadata = tinyUsdInheritedPrimMetadata(stage, path);
  if (metadata.displayName.empty()) {
    metadata.displayName = prim.element_name();
    if (metadata.displayName.empty()) {
      if (const auto elementName = tinyusdz::GetPrimElementName(prim.data())) {
        metadata.displayName = *elementName;
      }
    }
    if (metadata.displayName.empty()) {
      metadata.displayName = tinyUsdPathLeaf(path);
    }
  }
  if (metadata.primType.empty()) {
    metadata.primType = "Points";
  }

  int32_t primId = 0;
  if (prim.prim_id() > 0 &&
      prim.prim_id() <= std::numeric_limits<int32_t>::max()) {
    primId = static_cast<int32_t>(prim.prim_id());
  }

  element.transform = importTransform * matrixFromTinyUsd(globalMatrix);
  element.color = color;
  if (const uint32_t materialIndex =
          materialIndexFromTinyUsd(scene, materialId);
      materialIndex != std::numeric_limits<uint32_t>::max()) {
    element.materialIndex = materialIndex;
  }
  element.doubleSided = points.doubleSided.get_value();
  element.guid =
      tinyUsdElementGuid(metadata, path, "usd-points-", primId, meshId);
  element.type = tinyUsdElementType(metadata, "UsdGeomPoints", true);
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
  element.sourceId = !metadata.path.empty() ? metadata.path : std::string(path);
  element.geometryKind = dotbim::GeometryKind::Points;
}

template <typename CurveT>
void appendTinyUsdCurvePrim(dotbim::Model &model, const tinyusdz::Stage &stage,
                            const tinyusdz::tydra::RenderScene &scene,
                            const tinyusdz::Prim &prim, const CurveT &curves,
                            const glm::mat4 &importTransform,
                            const tinyusdz::value::matrix4d &globalMatrix,
                            std::string_view path,
                            std::string_view fallbackType,
                            const std::stop_token &stop);

void appendTinyUsdPointsPrim(dotbim::Model &model, const tinyusdz::Stage &stage,
                             const tinyusdz::tydra::RenderScene &scene,
                             const tinyusdz::Prim &prim,
                             const tinyusdz::GeomPoints &points,
                             const glm::mat4 &importTransform,
                             const tinyusdz::value::matrix4d &globalMatrix,
                             std::string_view path, const std::stop_token &stop) {
  if (dotbim::Element *existing = modelElementBySource(model, path, stop)) {
    applyTinyUsdPointsElementFields(*existing, stage, scene, prim, points,
                                    importTransform, globalMatrix, path,
                                    existing->meshId);
    return;
  }

  const MeshData mesh = tinyUsdPointsMeshData(points, path, stop);
  if (mesh.points.empty()) {
    return;
  }

  const uint32_t meshId = static_cast<uint32_t>(model.meshRanges.size());
  const size_t rangeCountBefore = model.meshRanges.size();
  const glm::vec4 color = colorFromTinyUsdMaterialId(
      scene, tinyUsdMaterialIndexForPrim(stage, scene, path),
      tinyUsdDisplayColor(stage, path));
  appendPointPlaceholders(model, mesh, meshId, path, color, stop);
  if (model.meshRanges.size() == rangeCountBefore) {
    return;
  }

  dotbim::Element element{};
  element.meshId = meshId;
  applyTinyUsdPointsElementFields(element, stage, scene, prim, points,
                                  importTransform, globalMatrix, path, meshId);
  model.elements.push_back(std::move(element));
}

void appendTinyUsdPointsFromPrim(
    dotbim::Model &model, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderScene &scene, const tinyusdz::Prim &prim,
    const glm::mat4 &importTransform,
    const tinyusdz::value::matrix4d &parentMatrix, std::string_view parentPath,
    bool inheritedVisible, const std::stop_token &stop) {
  checkModelImportCancellation(stop);
  const std::string path = tinyUsdPrimPathString(prim, parentPath);
  const tinyusdz::value::matrix4d globalMatrix =
      tinyUsdGlobalMatrixForPrim(prim, parentMatrix, path);
  const bool visible = tinyUsdPrimIsVisible(prim, inheritedVisible);
  if (visible) {
    if (const auto *points = prim.as<tinyusdz::GeomPoints>()) {
      appendTinyUsdPointsPrim(model, stage, scene, prim, *points,
                              importTransform, globalMatrix, path, stop);
    } else if (const auto *basisCurves =
                   prim.as<tinyusdz::GeomBasisCurves>()) {
      appendTinyUsdCurvePrim(model, stage, scene, prim, *basisCurves,
                             importTransform, globalMatrix, path,
                             "UsdGeomBasisCurves", stop);
    } else if (const auto *nurbsCurves =
                   prim.as<tinyusdz::GeomNurbsCurves>()) {
      appendTinyUsdCurvePrim(model, stage, scene, prim, *nurbsCurves,
                             importTransform, globalMatrix, path,
                             "UsdGeomNurbsCurves", stop);
    }
  }

  for (const tinyusdz::Prim &child : prim.children()) {
    appendTinyUsdPointsFromPrim(model, stage, scene, child, importTransform,
                                globalMatrix, path, visible, stop);
  }
}

void appendTinyUsdPointsFromStage(dotbim::Model &model,
                                  const tinyusdz::Stage &stage,
                                  const tinyusdz::tydra::RenderScene &scene,
                                  const glm::mat4 &importTransform,
                                  const std::stop_token &stop) {
  const tinyusdz::value::matrix4d identity =
      tinyusdz::value::matrix4d::identity();
  for (const tinyusdz::Prim &prim : stage.root_prims()) {
    appendTinyUsdPointsFromPrim(model, stage, scene, prim, importTransform,
                                identity, "", true, stop);
  }
}

template <typename CurveT>
void applyTinyUsdCurveElementFields(
    dotbim::Element &element, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderScene &scene, const tinyusdz::Prim &prim,
    const CurveT &curves, const glm::mat4 &importTransform,
    const tinyusdz::value::matrix4d &globalMatrix, std::string_view path,
    uint32_t meshId, std::string_view fallbackType) {
  const int materialId = tinyUsdMaterialIndexForPrim(stage, scene, path);
  const glm::vec4 displayColor = tinyUsdDisplayColor(stage, path);
  const glm::vec4 color =
      colorFromTinyUsdMaterialId(scene, materialId, displayColor);

  TinyUsdPrimMetadata metadata = tinyUsdInheritedPrimMetadata(stage, path);
  if (metadata.displayName.empty()) {
    metadata.displayName = prim.element_name();
    if (metadata.displayName.empty()) {
      if (const auto elementName = tinyusdz::GetPrimElementName(prim.data())) {
        metadata.displayName = *elementName;
      }
    }
    if (metadata.displayName.empty()) {
      metadata.displayName = tinyUsdPathLeaf(path);
    }
  }

  int32_t primId = 0;
  if (prim.prim_id() > 0 &&
      prim.prim_id() <= std::numeric_limits<int32_t>::max()) {
    primId = static_cast<int32_t>(prim.prim_id());
  }

  element.transform = importTransform * matrixFromTinyUsd(globalMatrix);
  element.color = color;
  if (const uint32_t materialIndex =
          materialIndexFromTinyUsd(scene, materialId);
      materialIndex != std::numeric_limits<uint32_t>::max()) {
    element.materialIndex = materialIndex;
  }
  element.doubleSided = curves.doubleSided.get_value();
  element.guid =
      tinyUsdElementGuid(metadata, path, "usd-curves-", primId, meshId);
  element.type = tinyUsdElementType(metadata, fallbackType, true);
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
  element.sourceId = !metadata.path.empty() ? metadata.path : std::string(path);
  element.geometryKind = dotbim::GeometryKind::Curves;
}

template <typename CurveT>
void appendTinyUsdCurvePrim(dotbim::Model &model, const tinyusdz::Stage &stage,
                            const tinyusdz::tydra::RenderScene &scene,
                            const tinyusdz::Prim &prim, const CurveT &curves,
                            const glm::mat4 &importTransform,
                            const tinyusdz::value::matrix4d &globalMatrix,
                            std::string_view path,
                            std::string_view fallbackType,
                            const std::stop_token &stop) {
  if (dotbim::Element *existing = modelElementBySource(model, path, stop)) {
    applyTinyUsdCurveElementFields(*existing, stage, scene, prim, curves,
                                   importTransform, globalMatrix, path,
                                   existing->meshId, fallbackType);
    return;
  }

  const MeshData mesh = tinyUsdCurveMeshData(curves, path, stop);
  if (mesh.points.empty()) {
    return;
  }

  const uint32_t meshId = static_cast<uint32_t>(model.meshRanges.size());
  const size_t rangeCountBefore = model.meshRanges.size();
  const glm::vec4 color = colorFromTinyUsdMaterialId(
      scene, tinyUsdMaterialIndexForPrim(stage, scene, path),
      tinyUsdDisplayColor(stage, path));
  appendBasisCurvePlaceholders(model, mesh, meshId, path, color, stop);
  if (model.meshRanges.size() == rangeCountBefore) {
    return;
  }

  dotbim::Element element{};
  element.meshId = meshId;
  applyTinyUsdCurveElementFields(element, stage, scene, prim, curves,
                                 importTransform, globalMatrix, path, meshId,
                                 fallbackType);
  model.elements.push_back(std::move(element));
}

void appendTinyUsdPointMaterialIfMissing(
    tinyusdz::tydra::RenderScene &scene,
    tinyusdz::tydra::RenderSceneConverter &converter,
    const tinyusdz::tydra::RenderSceneConverterEnv &env,
    const TinyUsdBoundMaterial &boundMaterial) {
  if (boundMaterial.path.empty() || boundMaterial.material == nullptr ||
      tinyUsdMaterialIndexForPath(scene, boundMaterial.path) >= 0) {
    return;
  }

  tinyusdz::tydra::RenderMaterial renderMaterial;
  if (converter.ConvertMaterial(env, tinyusdz::Path(boundMaterial.path, ""),
                                *boundMaterial.material, &renderMaterial)) {
    scene.materials.push_back(std::move(renderMaterial));
  }
}

void appendTinyUsdPointMaterialsFromPrim(
    tinyusdz::tydra::RenderScene &scene, const tinyusdz::Stage &stage,
    tinyusdz::tydra::RenderSceneConverter &converter,
    const tinyusdz::tydra::RenderSceneConverterEnv &env,
    const tinyusdz::Prim &prim, std::string_view parentPath,
    bool inheritedVisible, const std::stop_token &stop) {
  checkModelImportCancellation(stop);
  const std::string path = tinyUsdPrimPathString(prim, parentPath);
  const bool visible = tinyUsdPrimIsVisible(prim, inheritedVisible);
  if (visible && tinyUsdPrimNeedsPlaceholderGeometry(prim)) {
    if (const auto boundMaterial = tinyUsdBoundMaterialForPrim(stage, path)) {
      appendTinyUsdPointMaterialIfMissing(scene, converter, env,
                                          *boundMaterial);
    }
  }

  for (const tinyusdz::Prim &child : prim.children()) {
    appendTinyUsdPointMaterialsFromPrim(scene, stage, converter, env, child,
                                        path, visible, stop);
  }
}

void appendTinyUsdPointMaterialsFromStage(
    tinyusdz::tydra::RenderScene &scene, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderSceneConverterEnv &env,
    const std::stop_token &stop) {
  tinyusdz::tydra::RenderSceneConverter converter;
  for (const tinyusdz::Prim &prim : stage.root_prims()) {
    appendTinyUsdPointMaterialsFromPrim(scene, stage, converter, env, prim, "",
                                        true, stop);
  }
}

bool tinyUsdPathNeedsPlaceholderGeometry(const tinyusdz::Stage &stage,
                                         std::string_view path) {
  if (path.empty()) {
    return false;
  }
  const tinyusdz::Prim *prim = nullptr;
  std::string err;
  return stage.find_prim_at_path(tinyusdz::Path(std::string(path), ""), prim,
                                 &err) &&
         prim != nullptr && tinyUsdPrimNeedsPlaceholderGeometry(*prim);
}

void applyTinyUsdPointsMaterialFallbacks(
    dotbim::Model &model, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderScene &scene, const std::stop_token &stop) {
  for (dotbim::Element &element : model.elements) {
    checkModelImportCancellation(stop);
    if (element.materialIndex != std::numeric_limits<uint32_t>::max() ||
        !tinyUsdPathNeedsPlaceholderGeometry(stage, element.sourceId)) {
      continue;
    }

    const int materialId =
        tinyUsdMaterialIndexForPrim(stage, scene, element.sourceId);
    if (const uint32_t materialIndex =
            materialIndexFromTinyUsd(scene, materialId);
        materialIndex != std::numeric_limits<uint32_t>::max()) {
      element.materialIndex = materialIndex;
      element.color = colorFromTinyUsdMaterialId(scene, materialId,
                                                 element.color);
    }
  }
}

struct TinyMeshTriangle {
  uint32_t pointIndex[3]{};
  size_t faceVertexOrdinal[3]{};
};

struct TinyMeshPart {
  uint32_t meshId{0};
  int materialId{-1};
};

size_t tinyFaceVaryingOrdinal(const tinyusdz::tydra::RenderMesh &mesh,
                              size_t triangulatedOrdinal) {
  if (triangulatedOrdinal < mesh.triangulatedToOrigFaceVertexIndexMap.size()) {
    return mesh.triangulatedToOrigFaceVertexIndexMap[triangulatedOrdinal];
  }
  return triangulatedOrdinal;
}

size_t
tinyAttributeComponentCount(const tinyusdz::tydra::VertexAttribute &attribute) {
  using Format = tinyusdz::tydra::VertexAttributeFormat;
  switch (attribute.format) {
  case Format::Float:
  case Format::Double:
    return std::max<size_t>(1u, attribute.elementSize);
  case Format::Vec2:
  case Format::Dvec2:
    return 2u;
  case Format::Vec3:
  case Format::Dvec3:
    return 3u;
  case Format::Vec4:
  case Format::Dvec4:
    return 4u;
  default:
    return 0u;
  }
}

std::optional<size_t>
tinyAttributeValueIndex(const tinyusdz::tydra::VertexAttribute &attribute,
                        uint32_t pointIndex, size_t faceVertexOrdinal) {
  if (attribute.empty()) {
    return std::nullopt;
  }

  size_t sourceIndex = pointIndex;
  if (attribute.is_constant()) {
    sourceIndex = 0u;
  } else if (attribute.is_facevarying()) {
    sourceIndex = faceVertexOrdinal;
  } else if (attribute.is_uniform()) {
    return std::nullopt;
  }

  if (!attribute.indices.empty()) {
    if (sourceIndex >= attribute.indices.size()) {
      return std::nullopt;
    }
    sourceIndex = attribute.indices[sourceIndex];
  }
  if (sourceIndex >= attribute.vertex_count()) {
    return std::nullopt;
  }
  return sourceIndex;
}

std::optional<float>
tinyAttributeComponent(const tinyusdz::tydra::VertexAttribute &attribute,
                       size_t valueIndex, size_t component) {
  const size_t componentCount = tinyAttributeComponentCount(attribute);
  if (component >= componentCount) {
    return std::nullopt;
  }

  const size_t stride = attribute.stride_bytes();
  const size_t offset = valueIndex * stride;
  if (offset >= attribute.data.size()) {
    return std::nullopt;
  }

  using Format = tinyusdz::tydra::VertexAttributeFormat;
  switch (attribute.format) {
  case Format::Float:
  case Format::Vec2:
  case Format::Vec3:
  case Format::Vec4: {
    const size_t componentOffset = offset + component * sizeof(float);
    if (componentOffset + sizeof(float) > attribute.data.size()) {
      return std::nullopt;
    }
    float value = 0.0f;
    std::memcpy(&value, attribute.data.data() + componentOffset, sizeof(float));
    return std::isfinite(value) ? std::optional<float>(value) : std::nullopt;
  }
  case Format::Double:
  case Format::Dvec2:
  case Format::Dvec3:
  case Format::Dvec4: {
    const size_t componentOffset = offset + component * sizeof(double);
    if (componentOffset + sizeof(double) > attribute.data.size()) {
      return std::nullopt;
    }
    double value = 0.0;
    std::memcpy(&value, attribute.data.data() + componentOffset,
                sizeof(double));
    return std::isfinite(value)
               ? std::optional<float>(static_cast<float>(value))
               : std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

std::optional<glm::vec2>
tinyAttributeVec2(const tinyusdz::tydra::VertexAttribute &attribute,
                  uint32_t pointIndex, size_t faceVertexOrdinal) {
  const auto valueIndex =
      tinyAttributeValueIndex(attribute, pointIndex, faceVertexOrdinal);
  if (!valueIndex) {
    return std::nullopt;
  }
  const auto x = tinyAttributeComponent(attribute, *valueIndex, 0u);
  const auto y = tinyAttributeComponent(attribute, *valueIndex, 1u);
  if (!x || !y) {
    return std::nullopt;
  }
  return glm::vec2(*x, *y);
}

std::optional<glm::vec3>
tinyAttributeVec3(const tinyusdz::tydra::VertexAttribute &attribute,
                  uint32_t pointIndex, size_t faceVertexOrdinal) {
  const auto valueIndex =
      tinyAttributeValueIndex(attribute, pointIndex, faceVertexOrdinal);
  if (!valueIndex) {
    return std::nullopt;
  }
  const auto x = tinyAttributeComponent(attribute, *valueIndex, 0u);
  const auto y = tinyAttributeComponent(attribute, *valueIndex, 1u);
  const auto z = tinyAttributeComponent(attribute, *valueIndex, 2u);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  return glm::vec3(*x, *y, *z);
}

std::optional<glm::vec4>
tinyAttributeVec4(const tinyusdz::tydra::VertexAttribute &attribute,
                  uint32_t pointIndex, size_t faceVertexOrdinal) {
  const auto valueIndex =
      tinyAttributeValueIndex(attribute, pointIndex, faceVertexOrdinal);
  if (!valueIndex) {
    return std::nullopt;
  }
  const auto x = tinyAttributeComponent(attribute, *valueIndex, 0u);
  const auto y = tinyAttributeComponent(attribute, *valueIndex, 1u);
  const auto z = tinyAttributeComponent(attribute, *valueIndex, 2u);
  if (!x || !y || !z) {
    return std::nullopt;
  }
  const auto w = tinyAttributeComponent(attribute, *valueIndex, 3u);
  return glm::vec4(*x, *y, *z, w.value_or(1.0f));
}

const tinyusdz::tydra::VertexAttribute *
tinyTexcoordAttribute(const tinyusdz::tydra::RenderMesh &mesh, uint32_t slot) {
  const auto it = mesh.texcoords.find(slot);
  return it == mesh.texcoords.end() ? nullptr : &it->second;
}

glm::vec3 tinyVertexColor(const tinyusdz::tydra::RenderMesh &mesh,
                          uint32_t pointIndex, size_t faceVertexOrdinal) {
  if (mesh.vertex_colors.vertex_count() <= 1u) {
    return glm::vec3(1.0f);
  }
  return glm::clamp(
      tinyAttributeVec3(mesh.vertex_colors, pointIndex, faceVertexOrdinal)
          .value_or(glm::vec3(1.0f)),
      glm::vec3(0.0f), glm::vec3(1.0f));
}

glm::vec4 tinyVertexTangent(const tinyusdz::tydra::RenderMesh &mesh,
                            uint32_t pointIndex, size_t faceVertexOrdinal,
                            const glm::vec3 &fallbackTangent,
                            const glm::vec3 &normal) {
  if (const auto tangent =
          tinyAttributeVec4(mesh.tangents, pointIndex, faceVertexOrdinal)) {
    glm::vec3 tangentVector =
        normalizeOrFallback(glm::vec3(*tangent), fallbackTangent);
    return glm::vec4(tangentVector, tangent->w < 0.0f ? -1.0f : 1.0f);
  }
  if (const auto tangent =
          tinyAttributeVec3(mesh.tangents, pointIndex, faceVertexOrdinal)) {
    glm::vec3 tangentVector = normalizeOrFallback(*tangent, fallbackTangent);
    float sign = 1.0f;
    if (const auto binormal =
            tinyAttributeVec3(mesh.binormals, pointIndex, faceVertexOrdinal)) {
      sign = glm::dot(glm::cross(normal, tangentVector), *binormal) < 0.0f
                 ? -1.0f
                 : 1.0f;
    }
    return glm::vec4(tangentVector, sign);
  }
  return glm::vec4(fallbackTangent, 1.0f);
}

std::vector<TinyMeshTriangle>
tinyUsdTrianglesForMaterial(const tinyusdz::tydra::RenderMesh &mesh,
                            int materialId, const std::stop_token &stop) {
  const std::vector<uint32_t> faceVertexIndices = mesh.faceVertexIndices();
  const std::vector<uint32_t> faceVertexCounts = mesh.faceVertexCounts();
  if (faceVertexIndices.empty()) {
    return {};
  }

  std::vector<int> materialByFace;
  size_t faceCount = faceVertexCounts.empty() ? faceVertexIndices.size() / 3u
                                              : faceVertexCounts.size();
  materialByFace.assign(faceCount, mesh.material_id);
  for (const auto &[name, subset] : mesh.material_subsetMap) {
    checkModelImportCancellation(stop);
    (void)name;
    const int subsetMaterial =
        subset.material_id >= 0 ? subset.material_id : mesh.material_id;
    size_t subsetFace = 0;
    for (int faceIndex : subset.indices()) {
      checkImportProgress(subsetFace++, stop);
      if (faceIndex >= 0 && static_cast<size_t>(faceIndex) < faceCount) {
        materialByFace[static_cast<size_t>(faceIndex)] = subsetMaterial;
      }
    }
  }

  std::vector<TinyMeshTriangle> triangles;
  size_t cursor = 0;
  for (size_t face = 0; face < faceCount; ++face) {
    checkImportProgress(face, stop);
    const uint32_t count =
        faceVertexCounts.empty() ? 3u : faceVertexCounts[face];
    if (count < 3u || cursor + count > faceVertexIndices.size()) {
      throw std::runtime_error("USD mesh '" + mesh.abs_path +
                               "' has inconsistent face topology");
    }
    if (materialByFace[face] == materialId) {
      for (uint32_t i = 1; i + 1u < count; ++i) {
        checkImportProgress(i, stop);
        TinyMeshTriangle triangle{};
        triangle.pointIndex[0] = faceVertexIndices[cursor];
        triangle.pointIndex[1] = faceVertexIndices[cursor + i];
        triangle.pointIndex[2] = faceVertexIndices[cursor + i + 1u];
        triangle.faceVertexOrdinal[0] = tinyFaceVaryingOrdinal(mesh, cursor);
        triangle.faceVertexOrdinal[1] =
            tinyFaceVaryingOrdinal(mesh, cursor + i);
        triangle.faceVertexOrdinal[2] =
            tinyFaceVaryingOrdinal(mesh, cursor + i + 1u);
        triangles.push_back(triangle);
      }
    }
    cursor += count;
  }
  return triangles;
}

std::vector<int>
tinyUsdMeshMaterialIds(const tinyusdz::tydra::RenderMesh &mesh,
                      const std::stop_token &stop) {
  std::vector<int> materialIds;
  auto appendUnique = [&](int materialId) {
    if (std::ranges::find(materialIds, materialId) == materialIds.end()) {
      materialIds.push_back(materialId);
    }
  };

  appendUnique(mesh.material_id);
  for (const auto &[name, subset] : mesh.material_subsetMap) {
    checkModelImportCancellation(stop);
    (void)name;
    appendUnique(subset.material_id >= 0 ? subset.material_id
                                         : mesh.material_id);
  }
  return materialIds;
}

bool appendTinyUsdTrianglePart(dotbim::Model &model,
                               const tinyusdz::tydra::RenderMesh &mesh,
                               const std::vector<TinyMeshTriangle> &triangles,
                               uint32_t meshId, const std::stop_token &stop) {
  if (mesh.points.size() >
      static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
    throw std::runtime_error("USD mesh '" + mesh.abs_path +
                             "' has too many points for 32-bit indices");
  }
  if (triangles.empty()) {
    return false;
  }

  dotbim::MeshRange range{};
  range.meshId = meshId;
  range.firstIndex = static_cast<uint32_t>(model.indices.size());

  std::vector<glm::vec3> rangePoints;
  rangePoints.reserve(triangles.size() * 3u);

  const tinyusdz::tydra::VertexAttribute *texcoord0 =
      tinyTexcoordAttribute(mesh, 0u);
  const tinyusdz::tydra::VertexAttribute *texcoord1 =
      tinyTexcoordAttribute(mesh, 1u);

  for (const TinyMeshTriangle &triangle : triangles) {
    checkImportProgress(rangePoints.size(), stop);
    glm::vec3 positions[3]{};
    for (size_t corner = 0; corner < 3u; ++corner) {
      const uint32_t pointIndex = triangle.pointIndex[corner];
      if (pointIndex >= mesh.points.size()) {
        throw std::runtime_error("USD mesh '" + mesh.abs_path +
                                 "' has a face vertex index outside points");
      }
      positions[corner] = tinyVec3(mesh.points[pointIndex]);
    }

    const glm::vec3 faceNormal =
        safeNormal(positions[0], positions[1], positions[2]);
    const glm::vec3 faceTangent =
        safeTangent(positions[0], positions[1], faceNormal);
    const uint32_t base = static_cast<uint32_t>(model.vertices.size());

    for (size_t corner = 0; corner < 3u; ++corner) {
      const uint32_t pointIndex = triangle.pointIndex[corner];
      const size_t faceVertexOrdinal = triangle.faceVertexOrdinal[corner];
      const glm::vec3 normal = normalizeOrFallback(
          tinyAttributeVec3(mesh.normals, pointIndex, faceVertexOrdinal)
              .value_or(faceNormal),
          faceNormal);
      const glm::vec4 tangent = tinyVertexTangent(
          mesh, pointIndex, faceVertexOrdinal, faceTangent, normal);
      const glm::vec2 uv0 =
          texcoord0 == nullptr
              ? glm::vec2(0.0f)
              : tinyAttributeVec2(*texcoord0, pointIndex, faceVertexOrdinal)
                    .value_or(glm::vec2(0.0f));
      const glm::vec2 uv1 =
          texcoord1 == nullptr
              ? glm::vec2(0.0f)
              : tinyAttributeVec2(*texcoord1, pointIndex, faceVertexOrdinal)
                    .value_or(glm::vec2(0.0f));
      const glm::vec3 color =
          tinyVertexColor(mesh, pointIndex, faceVertexOrdinal);
      model.vertices.push_back(
          makeVertex(positions[corner], normal, tangent, color, uv0, uv1));
      rangePoints.push_back(positions[corner]);
    }
    model.indices.insert(model.indices.end(), {base, base + 1u, base + 2u});
  }

  range.indexCount =
      static_cast<uint32_t>(model.indices.size()) - range.firstIndex;
  const auto [center, radius] = computeBounds(rangePoints, stop);
  range.boundsCenter = center;
  range.boundsRadius = radius;
  if (range.indexCount == 0u) {
    return false;
  }
  model.meshRanges.push_back(range);
  appendMeshletClustersForRange(model, range, stop);
  return true;
}

const std::vector<TinyMeshPart> *appendTinyUsdMeshIfNeeded(
    dotbim::Model &model, const tinyusdz::tydra::RenderScene &scene,
    int32_t meshIndex,
    std::unordered_map<int32_t, std::vector<TinyMeshPart>> &loadedMeshes,
    const std::stop_token &stop) {
  if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= scene.meshes.size()) {
    return nullptr;
  }

  if (const auto existing = loadedMeshes.find(meshIndex);
      existing != loadedMeshes.end()) {
    return &existing->second;
  }

  const tinyusdz::tydra::RenderMesh &mesh =
      scene.meshes[static_cast<size_t>(meshIndex)];
  std::vector<TinyMeshPart> parts;
  for (int materialId : tinyUsdMeshMaterialIds(mesh, stop)) {
    checkModelImportCancellation(stop);
    const std::vector<TinyMeshTriangle> triangles =
        tinyUsdTrianglesForMaterial(mesh, materialId, stop);
    if (triangles.empty()) {
      continue;
    }
    const uint32_t meshId = static_cast<uint32_t>(model.meshRanges.size());
    if (appendTinyUsdTrianglePart(model, mesh, triangles, meshId, stop)) {
      parts.push_back(TinyMeshPart{meshId, materialId});
    }
  }
  if (parts.empty()) {
    return nullptr;
  }
  auto [it, inserted] = loadedMeshes.emplace(meshIndex, std::move(parts));
  (void)inserted;
  return &it->second;
}

bool hasTinyUsdCompositionArcs(const tinyusdz::Layer &layer) {
  return layer.check_unresolved_inherits() ||
         layer.check_unresolved_variant() ||
         layer.check_unresolved_references() ||
         layer.check_unresolved_payload() ||
         layer.check_unresolved_specializes();
}

bool loadComposedTinyUsdStage(const std::filesystem::path &path,
                              tinyusdz::Stage &stage, std::string &warn,
                              std::string &err,
                              const tinyusdz::USDLoadOptions &options,
                              const std::stop_token &stop) {
  tinyusdz::Layer layer;
  const std::string filename = pathToTinyUsdUtf8(path);
  if (!tinyusdz::LoadLayerFromFile(filename, &layer, &warn, &err, options)) {
    return false;
  }
  checkModelImportCancellation(stop);

  tinyusdz::AssetResolutionResolver resolver;
  const std::string baseDir = pathToTinyUsdUtf8(path.parent_path());
  resolver.set_current_working_path(baseDir);
  resolver.set_search_paths({baseDir});

  tinyusdz::Layer working = std::move(layer);
  if (!working.metas().subLayers.empty()) {
    tinyusdz::Layer composited;
    if (!tinyusdz::CompositeSublayers(resolver, working, &composited, &warn,
                                      &err)) {
      return false;
    }
    working = std::move(composited);
  }

  constexpr int kMaxCompositionPasses = 16;
  for (int pass = 0; pass < kMaxCompositionPasses; ++pass) {
    checkModelImportCancellation(stop);
    if (!hasTinyUsdCompositionArcs(working)) {
      break;
    }

    tinyusdz::Layer composited;
    if (!tinyusdz::CompositeAllArcs(resolver, working, &composited, &warn,
                                    &err)) {
      return false;
    }
    working = std::move(composited);
  }

  checkModelImportCancellation(stop);
  if (hasTinyUsdCompositionArcs(working)) {
    err += "USD composition did not converge after " +
           std::to_string(kMaxCompositionPasses) + " passes.\n";
    return false;
  }

  return tinyusdz::LayerToStage(std::move(working), &stage, &warn, &err);
}

bool hasRenderableGeometry(const dotbim::Model &model) {
  return !model.vertices.empty() && !model.indices.empty() &&
         !model.meshRanges.empty() && !model.elements.empty();
}

void fillTinyUsdElementDisplayNameFallbacks(dotbim::Model &model,
                                            const std::stop_token &stop) {
  for (dotbim::Element &element : model.elements) {
    checkModelImportCancellation(stop);
    if (element.displayName.empty() && !element.sourceId.empty()) {
      element.displayName = tinyUsdPathLeaf(element.sourceId);
    }
  }
}

void appendTinyUsdNode(
    dotbim::Model &model, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderScene &scene, const tinyusdz::tydra::Node &node,
    const glm::mat4 &importTransform,
    std::unordered_map<int32_t, std::vector<TinyMeshPart>> &loadedMeshes,
    const std::unordered_set<std::string> &handledInstancePaths,
    bool insideHandledInstance, const std::stop_token &stop) {
  checkModelImportCancellation(stop);
  const bool handledInstance =
      insideHandledInstance ||
      (!node.abs_path.empty() && handledInstancePaths.contains(node.abs_path));
  if (!handledInstance &&
      node.category == tinyusdz::tydra::NodeCategory::Geom && node.id >= 0) {
    const auto *parts =
        appendTinyUsdMeshIfNeeded(model, scene, node.id, loadedMeshes, stop);
    if (parts != nullptr) {
      const tinyusdz::tydra::RenderMesh &mesh =
          scene.meshes[static_cast<size_t>(node.id)];
      TinyUsdPrimMetadata metadata = mergeTinyUsdMetadata(
          tinyUsdInheritedPrimMetadata(stage, node.abs_path),
          tinyUsdInheritedPrimMetadata(stage, mesh.abs_path));
      if (metadata.displayName.empty()) {
        metadata.displayName =
            !node.display_name.empty() ? node.display_name : mesh.display_name;
      }
      if (metadata.primType.empty()) {
        metadata.primType = "Mesh";
      }
      for (const TinyMeshPart &part : *parts) {
        checkModelImportCancellation(stop);
        const glm::vec4 color =
            colorFromTinyUsdMaterial(scene, mesh, part.materialId);
        dotbim::Element element{};
        element.meshId = part.meshId;
        element.transform =
            importTransform * matrixFromTinyUsd(node.global_matrix);
        element.color = color;
        element.materialIndex =
            materialIndexFromTinyUsd(scene, part.materialId);
        element.doubleSided =
            static_cast<size_t>(node.id) < scene.meshes.size()
                ? scene.meshes[static_cast<size_t>(node.id)].doubleSided
                : true;
        element.guid = tinyUsdElementGuid(metadata, node.abs_path, "usd-node-",
                                          node.id, part.meshId);
        element.type = tinyUsdElementType(metadata, "UsdGeomMesh", true);
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
        element.sourceId =
            !metadata.path.empty() ? metadata.path : node.abs_path;
        model.elements.push_back(std::move(element));
      }
    }
  }

  for (const tinyusdz::tydra::Node &child : node.children) {
    appendTinyUsdNode(model, stage, scene, child, importTransform, loadedMeshes,
                      handledInstancePaths, handledInstance, stop);
  }
}

void appendTinyUsdInstances(
    dotbim::Model &model, const tinyusdz::Stage &stage,
    const tinyusdz::tydra::RenderScene &scene, const glm::mat4 &importTransform,
    std::unordered_map<int32_t, std::vector<TinyMeshPart>> &loadedMeshes,
    const std::stop_token &stop) {
  for (const tinyusdz::tydra::RenderInstance &instance : scene.instances) {
    checkModelImportCancellation(stop);
    if (!instance.visible || instance.mesh_id < 0) {
      continue;
    }

    const auto *parts =
        appendTinyUsdMeshIfNeeded(model, scene, instance.mesh_id, loadedMeshes, stop);
    if (parts == nullptr) {
      continue;
    }

    const tinyusdz::tydra::RenderMesh &mesh =
        scene.meshes[static_cast<size_t>(instance.mesh_id)];
    TinyUsdPrimMetadata metadata = mergeTinyUsdMetadata(
        tinyUsdInheritedPrimMetadata(stage, instance.abs_path),
        tinyUsdInheritedPrimMetadata(stage, mesh.abs_path));
    if (metadata.displayName.empty()) {
      metadata.displayName = !instance.display_name.empty()
                                 ? instance.display_name
                                 : mesh.display_name;
    }
    for (const TinyMeshPart &part : *parts) {
      checkModelImportCancellation(stop);
      const int materialId =
          instance.material_id >= 0 ? instance.material_id : part.materialId;
      dotbim::Element element{};
      element.meshId = part.meshId;
      element.transform =
          importTransform * matrixFromTinyUsd(instance.global_matrix);
      element.color = colorFromTinyUsdMaterial(scene, mesh, materialId);
      element.materialIndex = materialIndexFromTinyUsd(scene, materialId);
      element.doubleSided =
          static_cast<size_t>(instance.mesh_id) < scene.meshes.size()
              ? scene.meshes[static_cast<size_t>(instance.mesh_id)].doubleSided
              : true;
      element.guid =
          tinyUsdElementGuid(metadata, instance.abs_path, "usd-instance-",
                             instance.mesh_id, part.meshId);
      element.type =
          tinyUsdElementType(metadata, "UsdGeomMeshInstance", false);
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
      element.sourceId =
          !metadata.path.empty() ? metadata.path : instance.abs_path;
      model.elements.push_back(std::move(element));
    }
  }
}

dotbim::Model modelFromTinyUsdStage(const tinyusdz::Stage &stage,
                                    float importScale,
                                    const std::filesystem::path &sourcePath,
                                    const std::stop_token &stop) {
  tinyusdz::tydra::RenderScene scene;
  tinyusdz::tydra::RenderSceneConverter converter;
  tinyusdz::tydra::RenderSceneConverterEnv env(stage);
  const std::string filename = pathToTinyUsdUtf8(sourcePath);
  const std::string sourceDir = pathToTinyUsdUtf8(sourcePath.parent_path());
  env.usd_filename = filename;
  env.scene_config.load_texture_assets = false;
  env.set_search_paths({sourceDir});
  checkModelImportCancellation(stop);
  const bool convertedScene = converter.ConvertToRenderScene(env, &scene);
  checkModelImportCancellation(stop);
  std::string converterError;
  if (!convertedScene) {
    converterError = converter.GetError();
    scene = tinyusdz::tydra::RenderScene{};
  }
  appendTinyUsdPointMaterialsFromStage(scene, stage, env, stop);

  dotbim::Model model{};
  model.materials.reserve(scene.materials.size());
  for (const tinyusdz::tydra::RenderMaterial &material : scene.materials) {
    checkModelImportCancellation(stop);
    model.materials.push_back(
        convertTinyUsdMaterial(scene, material, sourcePath.parent_path()));
  }

  std::unordered_map<int32_t, std::vector<TinyMeshPart>> loadedMeshes;
  loadedMeshes.reserve(scene.meshes.size());
  std::unordered_set<std::string> handledInstancePaths;
  handledInstancePaths.reserve(scene.instances.size());
  for (const tinyusdz::tydra::RenderInstance &instance : scene.instances) {
    checkModelImportCancellation(stop);
    if (instance.visible && instance.mesh_id >= 0 &&
        !instance.abs_path.empty()) {
      handledInstancePaths.insert(instance.abs_path);
    }
  }

  tinyusdz::Axis upAxis{tinyusdz::Axis::Y};
  if (stage.metas().upAxis.authored()) {
    upAxis = stage.metas().upAxis.get_value();
  }
  double metersPerUnit = 1.0;
  if (stage.metas().metersPerUnit.authored()) {
    metersPerUnit = stage.metas().metersPerUnit.get_value();
  }
  const StageUnitMetadata stageUnits{
      .authored = stage.metas().metersPerUnit.authored(),
      .metersPerUnit = sanitizeUsdMetersPerUnit(metersPerUnit),
  };
  model.unitMetadata = makeUnitMetadata(stageUnits, importScale);
  applySourceUpAxis(model, upAxis == tinyusdz::Axis::Z ? "Z" : "Y");
  const float unitScale =
      sanitizeImportScale(importScale) *
      (stageUnits.authored ? stageUnits.metersPerUnit : 1.0f);
  const glm::mat4 importTransform =
      (upAxis == tinyusdz::Axis::Z
           ? container::geometry::zUpForwardYToRendererAxes()
           : glm::mat4(1.0f)) *
      glm::scale(glm::mat4(1.0f), glm::vec3(unitScale));
  for (const tinyusdz::tydra::Node &node : scene.nodes) {
    appendTinyUsdNode(model, stage, scene, node, importTransform, loadedMeshes,
                      handledInstancePaths, false, stop);
  }
  appendTinyUsdInstances(model, stage, scene, importTransform, loadedMeshes, stop);
  appendTinyUsdPointsFromStage(model, stage, scene, importTransform, stop);
  applyTinyUsdPointsMaterialFallbacks(model, stage, scene, stop);
  fillTinyUsdElementDisplayNameFallbacks(model, stop);
  checkModelImportCancellation(stop);
  if (!convertedScene && !hasRenderableGeometry(model)) {
    throw std::runtime_error(
        "TinyUSDZ failed to convert USD scene '" + filename +
        "': " + (converterError.empty() ? "unknown error" : converterError));
  }
  return model;
}

dotbim::Model LoadWithTinyUsd(const std::filesystem::path &path,
                              float importScale, const std::stop_token &stop) {
  const std::string filename = pathToTinyUsdUtf8(path);
  tinyusdz::USDLoadOptions options;
  options.load_assets = false;
  options.do_composition = true;
  options.load_sublayers = true;
  options.load_references = true;
  options.load_payloads = true;

  std::optional<dotbim::Model> directModel;
  tinyusdz::Stage directStage;
  std::string directWarn;
  std::string directErr;
  const bool directLoaded = tinyusdz::LoadUSDFromFile(
      filename, &directStage, &directWarn, &directErr, options);
  checkModelImportCancellation(stop);
  if (directLoaded) {
    directModel = modelFromTinyUsdStage(directStage, importScale, path, stop);
    if (hasRenderableGeometry(*directModel) &&
        !directModel->materials.empty()) {
      return std::move(*directModel);
    }
  }

  tinyusdz::Stage composedStage;
  std::string composedWarn;
  std::string composedErr;
  checkModelImportCancellation(stop);
  const bool composedLoaded = loadComposedTinyUsdStage(
      path, composedStage, composedWarn, composedErr, options, stop);
  checkModelImportCancellation(stop);
  if (!composedLoaded) {
    if (directModel && hasRenderableGeometry(*directModel)) {
      return std::move(*directModel);
    }
    std::string error = composedErr.empty() ? directErr : composedErr;
    throw std::runtime_error("TinyUSDZ failed to load USD file '" + filename +
                             "': " + (error.empty() ? "unknown error" : error));
  }
  dotbim::Model composedModel =
      modelFromTinyUsdStage(composedStage, importScale, path, stop);
  if (hasRenderableGeometry(composedModel) &&
      (!composedModel.materials.empty() || !directModel ||
       !hasRenderableGeometry(*directModel))) {
    return composedModel;
  }
  if (directModel && hasRenderableGeometry(*directModel)) {
    return std::move(*directModel);
  }
  return composedModel;
}

#endif

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path,
                                    const std::stop_token &stop) {
  checkModelImportCancellation(stop);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open USD file: " +
                             container::util::pathToUtf8(path));
  }
  std::array<uint8_t, 64 * 1024> block;
  std::vector<uint8_t> bytes;
  do {
    checkModelImportCancellation(stop);
    file.read(reinterpret_cast<char *>(block.data()),
              static_cast<std::streamsize>(block.size()));
    bytes.insert(bytes.end(), block.begin(), block.begin() + file.gcount());
  } while (file);
  checkModelImportCancellation(stop);
  if (file.bad() || !file.eof()) {
    throw std::runtime_error("failed to read USD file: " +
                             container::util::pathToUtf8(path));
  }
  return bytes;
}

bool fileStartsWithUsdc(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  char magic[8]{};
  file.read(magic, sizeof(magic));
  return file.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
         std::string_view(magic, sizeof(magic)) == "PXR-USDC";
}

std::string readTextFile(const std::filesystem::path &path,
                         const std::stop_token &stop) {
  const std::vector<uint8_t> bytes = readBinaryFile(path, stop);
  if (bytes.empty()) {
    return {};
  }
  if (bytes.size() >= 8u &&
      std::string_view(reinterpret_cast<const char *>(bytes.data()), 8u) ==
          "PXR-USDC") {
    throw std::runtime_error(
        "binary USDC payloads are not supported by the lightweight USD loader");
  }
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

uint16_t readLe16(const std::vector<uint8_t> &bytes, size_t offset) {
  if (offset + 2u > bytes.size()) {
    throw std::runtime_error("truncated USDZ archive");
  }
  return static_cast<uint16_t>(bytes[offset]) |
         (static_cast<uint16_t>(bytes[offset + 1u]) << 8u);
}

uint32_t readLe32(const std::vector<uint8_t> &bytes, size_t offset) {
  if (offset + 4u > bytes.size()) {
    throw std::runtime_error("truncated USDZ archive");
  }
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
         (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
         (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

bool hasUsdTextExtension(std::string_view name) {
  const std::filesystem::path path{std::string(name)};
  const std::string extension = lowerAscii(path.extension().string());
  return extension == ".usd" || extension == ".usda";
}

std::string extractFirstUsdTextFromUsdz(const std::filesystem::path &path,
                                        const std::stop_token &stop) {
  const std::vector<uint8_t> bytes = readBinaryFile(path, stop);
  if (bytes.size() < 22u) {
    throw std::runtime_error("USDZ archive is too small");
  }

  const size_t minOffset = bytes.size() > 66000u ? bytes.size() - 66000u : 0u;
  std::optional<size_t> eocdOffset;
  for (size_t offset = bytes.size() - 22u;; --offset) {
    checkImportProgress(offset, stop);
    if (readLe32(bytes, offset) == 0x06054b50u) {
      eocdOffset = offset;
      break;
    }
    if (offset == minOffset) {
      break;
    }
  }
  if (!eocdOffset) {
    throw std::runtime_error("USDZ archive is missing a ZIP directory");
  }

  const uint16_t entryCount = readLe16(bytes, *eocdOffset + 10u);
  const uint32_t centralDirectoryOffset = readLe32(bytes, *eocdOffset + 16u);
  size_t cursor = centralDirectoryOffset;
  for (uint16_t entry = 0; entry < entryCount; ++entry) {
    checkImportProgress(entry, stop);
    if (readLe32(bytes, cursor) != 0x02014b50u) {
      throw std::runtime_error("USDZ archive has an invalid ZIP directory");
    }

    const uint16_t method = readLe16(bytes, cursor + 10u);
    const uint32_t compressedSize = readLe32(bytes, cursor + 20u);
    const uint32_t uncompressedSize = readLe32(bytes, cursor + 24u);
    const uint16_t nameLength = readLe16(bytes, cursor + 28u);
    const uint16_t extraLength = readLe16(bytes, cursor + 30u);
    const uint16_t commentLength = readLe16(bytes, cursor + 32u);
    const uint32_t localHeaderOffset = readLe32(bytes, cursor + 42u);
    if (cursor + 46u + nameLength > bytes.size()) {
      throw std::runtime_error("USDZ archive has a truncated file name");
    }
    const std::string name(
        reinterpret_cast<const char *>(bytes.data() + cursor + 46u),
        nameLength);
    cursor += 46u + nameLength + extraLength + commentLength;

    if (!hasUsdTextExtension(name)) {
      continue;
    }
    if (method != 0u) {
      throw std::runtime_error(
          "compressed USDZ entries are not supported; USDZ files must store "
          "the root USD layer without ZIP compression");
    }
    if (compressedSize != uncompressedSize) {
      throw std::runtime_error("USDZ stored entry has mismatched sizes");
    }
    if (readLe32(bytes, localHeaderOffset) != 0x04034b50u) {
      throw std::runtime_error("USDZ archive has an invalid local header");
    }

    const uint16_t localNameLength = readLe16(bytes, localHeaderOffset + 26u);
    const uint16_t localExtraLength = readLe16(bytes, localHeaderOffset + 28u);
    const size_t dataOffset =
        localHeaderOffset + 30u + localNameLength + localExtraLength;
    if (dataOffset + compressedSize > bytes.size()) {
      throw std::runtime_error("USDZ archive entry is truncated");
    }
    if (compressedSize >= 8u && std::string_view(reinterpret_cast<const char *>(
                                                     bytes.data() + dataOffset),
                                                 8u) == "PXR-USDC") {
      throw std::runtime_error(
          "binary USDC layers inside USDZ are not supported by the lightweight "
          "USD loader");
    }
    return {reinterpret_cast<const char *>(bytes.data() + dataOffset),
            compressedSize};
  }

  throw std::runtime_error(
      "USDZ archive does not contain a .usd or .usda layer");
}

} // namespace

dotbim::Model LoadFromText(std::string_view usdText, float importScale,
                           std::stop_token stop) {
  checkModelImportCancellation(stop);
  UsdParser parser(usdText, stop);
  const std::vector<Node> nodes = parser.parse();
  checkModelImportCancellation(stop);

  dotbim::Model model{};
  const StageUnitMetadata stageUnits = stageMetersPerUnitFromText(usdText, stop);
  model.unitMetadata = makeUnitMetadata(stageUnits, importScale);
  const std::string sourceUpAxis = stageUpAxisFromText(usdText, stop);
  applySourceUpAxis(model, sourceUpAxis);
  std::vector<std::optional<glm::mat4>> transformCache(nodes.size());
  const float unitScale =
      sanitizeImportScale(importScale) *
      (stageUnits.authored ? stageUnits.metersPerUnit : 1.0f);
  const glm::mat4 importTransform =
      container::geometry::usdUpAxisToRendererAxes(sourceUpAxis) *
      glm::scale(glm::mat4(1.0f), glm::vec3(unitScale));

  uint32_t nextMeshId = 0;
  for (size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
    checkModelImportCancellation(stop);
    const Node &node = nodes[nodeIndex];
    const bool isMesh = node.typeName == "Mesh";
    const bool isPoints = node.typeName == "Points";
    const bool isBasisCurves = node.typeName == "BasisCurves";
    if ((!isMesh && !isPoints && !isBasisCurves) || node.mesh.points.empty() ||
        !nodeVisible(static_cast<int>(nodeIndex), nodes)) {
      continue;
    }

    const glm::vec4 color = inheritedColor(static_cast<int>(nodeIndex), nodes);
    const uint32_t meshId = nextMeshId++;
    const size_t meshRangeCount = model.meshRanges.size();
    if (isPoints) {
      appendPointPlaceholders(model, node.mesh, meshId, node.path, color, stop);
    } else if (isBasisCurves) {
      appendBasisCurvePlaceholders(model, node.mesh, meshId, node.path, color, stop);
    } else {
      appendMesh(model, node.mesh, meshId, node.path, color, stop);
    }
    if (model.meshRanges.size() == meshRangeCount) {
      continue;
    }

    dotbim::Element element{};
    element.meshId = meshId;
    element.geometryKind = isPoints   ? dotbim::GeometryKind::Points
                           : isBasisCurves
                               ? dotbim::GeometryKind::Curves
                               : dotbim::GeometryKind::Mesh;
    element.transform =
        importTransform *
        nodeWorldTransform(static_cast<int>(nodeIndex), nodes, transformCache);
    element.color = color;
    element.doubleSided =
        inheritedDoubleSided(static_cast<int>(nodeIndex), nodes);
    element.guid =
        inheritedGuid(static_cast<int>(nodeIndex), nodes).value_or(node.path);
    element.type = inheritedSemanticType(static_cast<int>(nodeIndex), nodes);
    element.displayName =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::displayName)
            .value_or("");
    if (element.displayName.empty()) {
      element.displayName = !node.name.empty() ? node.name : node.path;
    }
    element.objectType =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::objectType)
            .value_or("");
    element.storeyName =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::storeyName)
            .value_or("");
    element.storeyId =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes, &Node::storeyId)
            .value_or("");
    element.materialName =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::materialName)
            .value_or("");
    element.materialCategory =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::materialCategory)
            .value_or("");
    element.discipline =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::discipline)
            .value_or("");
    element.phase =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes, &Node::phase)
            .value_or("");
    element.fireRating =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::fireRating)
            .value_or("");
    element.loadBearing =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes,
                            &Node::loadBearing)
            .value_or("");
    element.status =
        inheritedNodeString(static_cast<int>(nodeIndex), nodes, &Node::status)
            .value_or("");
    element.sourceId = node.path;
    model.elements.push_back(std::move(element));
  }

  checkModelImportCancellation(stop);
  return model;
}

dotbim::Model LoadFromFile(const std::filesystem::path &path,
                           float importScale, std::stop_token stop) {
  checkModelImportCancellation(stop);
  const std::string extension = lowerAscii(path.extension().string());
#if defined(CONTAINER_HAS_TINYUSDZ)
  try {
    return LoadWithTinyUsd(path, importScale, stop);
  } catch (const std::exception &tinyUsdError) {
    checkModelImportCancellation(stop);
    if (extension == ".usdc" || fileStartsWithUsdc(path)) {
      throw;
    }

    try {
      if (extension == ".usdz") {
        return LoadFromText(extractFirstUsdTextFromUsdz(path, stop), importScale, stop);
      }
      return LoadFromText(readTextFile(path, stop), importScale, stop);
    } catch (const std::exception &fallbackError) {
      checkModelImportCancellation(stop);
      throw std::runtime_error(
          std::string(tinyUsdError.what()) +
          "; lightweight USD fallback also failed: " + fallbackError.what());
    }
  }
#else
  if (extension == ".usdz") {
    return LoadFromText(extractFirstUsdTextFromUsdz(path, stop), importScale, stop);
  }
  if (extension == ".usdc") {
    throw std::runtime_error(
        "binary .usdc files are not supported by the lightweight USD loader");
  }
  return LoadFromText(readTextFile(path, stop), importScale, stop);
#endif
}

} // namespace container::geometry::usd
