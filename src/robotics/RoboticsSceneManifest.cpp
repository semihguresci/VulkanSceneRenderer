#include "Container/robotics/RoboticsSceneManifest.h"

#include "Container/utility/AtomicFile.h"

#include <glm/matrix.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace container::robotics {
namespace {

using Json = nlohmann::json;

constexpr std::string_view schema = "container.robotics-scene";
constexpr unsigned version = 1;

[[noreturn]] void invalid(std::string_view why) {
  throw std::invalid_argument("Robotics scene manifest: " + std::string(why));
}

const Json& arrayField(const Json& parent, std::string_view field) {
  const auto& value = parent.at(field);
  if (!value.is_array())
    invalid("expected an array for " + std::string(field));
  return value;
}

Json encodeMatrix(const glm::dmat4& matrix) {
  auto columns = Json::array();
  for (glm::length_t c = 0; c < 4; ++c) {
    const auto column = matrix[c];
    columns.push_back(Json::array({column.x, column.y, column.z, column.w}));
  }
  return columns;
}

Json optionalMatrix(const std::optional<glm::dmat4>& matrix) {
  return matrix ? encodeMatrix(*matrix) : Json(nullptr);
}

glm::dmat4 decodeMatrix(const Json& columns) {
  if (!columns.is_array() || columns.size() != 4)
    invalid("matrix must contain four columns");
  glm::dmat4 matrix(1.0);
  for (glm::length_t c = 0; c < 4; ++c) {
    const auto& column = columns.at(c);
    if (!column.is_array() || column.size() != 4)
      invalid("matrix column must contain four numbers");
    for (glm::length_t r = 0; r < 4; ++r)
      matrix[c][r] = column.at(r).get<double>();
  }
  return matrix;
}

std::optional<glm::dmat4> decodeOptionalMatrix(const Json& value) {
  return value.is_null() ? std::nullopt : std::optional(decodeMatrix(value));
}

Json encodeNumber(const std::optional<double>& value) {
  return value ? Json(*value) : Json(nullptr);
}

std::optional<double> decodeNumber(const Json& value) {
  return value.is_null() ? std::nullopt : std::optional(value.get<double>());
}

template <class Enum, class NameOf>
Enum decodeEnum(const Json& value, Enum last, NameOf nameOf) {
  const auto name = value.get<std::string>();
  for (unsigned i = 0; i <= static_cast<unsigned>(last); ++i) {
    const auto member = static_cast<Enum>(i);
    if (nameOf(member) == name)
      return member;
  }
  invalid("unknown semantic type " + name);
}

void validateMatrix(const glm::dmat4& matrix) {
  for (glm::length_t c = 0; c < 4; ++c) {
    for (glm::length_t r = 0; r < 4; ++r)
      if (!std::isfinite(matrix[c][r]))
        invalid("matrix values must be finite");
    if (matrix[c][3] != (c == 3 ? 1.0 : 0.0))
      invalid("matrix must describe an affine transform");
  }
  const double determinant = glm::determinant(glm::dmat3(matrix));
  if (!std::isfinite(determinant) || determinant == 0.0)
    invalid("matrix basis must be nonsingular");
}

bool samePlacement(const glm::dmat4& first, const glm::dmat4& second) {
  for (glm::length_t c = 0; c < 4; ++c) {
    for (glm::length_t r = 0; r < 4; ++r) {
      const double a = first[c][r], b = second[c][r];
      if (!std::isfinite(b) || std::abs(a - b) > 1e-12 * std::max({1.0, std::abs(a), std::abs(b)}))
        return false;
    }
  }
  return true;
}

void validate(const RoboticsScene& scene) {
  if (scene.sourceNamespace.empty())
    invalid("source namespace must identify a BIM dataset");
  for (const auto scale : {scene.frame.metersPerRendererUnit, scene.frame.sourceMetersPerUnit,
                          scene.frame.importScale}) {
    if (scale && (!std::isfinite(*scale) || *scale <= 0))
      invalid("unit scales must be finite and positive");
  }
  const bool metric = scene.frame.metersPerRendererUnit.has_value();
  if (metric != scene.frame.roboticsFromRenderer.has_value())
    invalid("the metric scale and frame transform must be provided together");
  if (scene.frame.roboticsFromRenderer) {
    validateMatrix(*scene.frame.roboticsFromRenderer);
    if (*scene.frame.roboticsFromRenderer != rendererToRosTransform(*scene.frame.metersPerRendererUnit))
      invalid("frame transform does not match the declared axes and metre scale");
  }
  if (const auto& offset = scene.georeference.coordinateOffset;
      offset && (!std::isfinite(offset->x) || !std::isfinite(offset->y) || !std::isfinite(offset->z)))
    invalid("source coordinate offset must be finite");

  const auto checkPlacement = [&](const std::optional<glm::dmat4>& renderer,
                                  const std::optional<glm::dmat4>& robotics) {
    if (robotics && (!metric || !renderer))
      invalid("robotics placement requires a renderer placement and known metric frame");
    if (renderer)
      validateMatrix(*renderer);
    if (robotics) {
      validateMatrix(*robotics);
      if (!samePlacement(*robotics, *scene.frame.roboticsFromRenderer * *renderer))
        invalid("robotics placement disagrees with the renderer placement and frame transform");
    }
  };
  std::unordered_map<std::string_view, SemanticKind> ids;
  ids.reserve(scene.entities.size());
  for (const auto& entity : scene.entities) {
    if (entity.id.empty() || !ids.emplace(entity.id, entity.kind).second)
      invalid("entity IDs must be nonempty and unique");
    checkPlacement(entity.rendererWorldFromLocal, entity.roboticsWorldFromLocal);
    for (const auto& rep : entity.representations)
      checkPlacement(rep.rendererWorldFromLocal, rep.roboticsWorldFromLocal);
  }
  for (const auto& entity : scene.entities) {
    for (const auto& id : entity.storeyIds) {
      const auto storey = ids.find(id);
      if (storey == ids.end() || storey->second != SemanticKind::Storey)
        invalid("storey reference does not identify a storey entity");
    }
  }
  for (const auto& relationship : scene.relationships) {
    if (!ids.contains(relationship.from) || !ids.contains(relationship.to))
      invalid("relationship endpoint does not identify an entity");
  }
}

RoboticsScene decode(const Json& root) {
  if (root.at("schema").get_ref<const std::string&>() != schema ||
      !root.at("version").is_number_unsigned() || root.at("version") != version ||
      root.at("matrixLayout") != "column-major")
    invalid("unsupported schema, version or matrix layout");
  RoboticsScene scene;
  scene.sourceNamespace = root.at("sourceNamespace").get<std::string>();
  const auto& frame = root.at("frame");
  if (frame.at("roboticsConvention") != "ROS REP-103")
    invalid("unsupported robotics frame convention");
  scene.frame.metersPerRendererUnit = decodeNumber(frame.at("metersPerRendererUnit"));
  scene.frame.sourceMetersPerUnit = decodeNumber(frame.at("sourceMetersPerUnit"));
  scene.frame.importScale = decodeNumber(frame.at("importScale"));
  scene.frame.sourceUnits = frame.at("sourceUnits").get<std::string>();
  scene.frame.unitProvenance = frame.at("unitProvenance").get<std::string>();
  scene.frame.roboticsFromRenderer = decodeOptionalMatrix(frame.at("roboticsFromRenderer"));
  const auto& geo = root.at("georeference");
  scene.georeference.sourceUpAxis = geo.at("sourceUpAxis").get<std::string>();
  const auto& offset = geo.at("coordinateOffset");
  if (!offset.is_null()) {
    if (!offset.is_array() || offset.size() != 3)
      invalid("coordinate offset must contain three numbers");
    scene.georeference.coordinateOffset = glm::dvec3(offset[0].get<double>(), offset[1].get<double>(),
                                                     offset[2].get<double>());
  }
  scene.georeference.coordinateOffsetSource = geo.at("coordinateOffsetSource").get<std::string>();
  scene.georeference.crsName = geo.at("crsName").get<std::string>();
  scene.georeference.crsAuthority = geo.at("crsAuthority").get<std::string>();
  scene.georeference.crsCode = geo.at("crsCode").get<std::string>();
  scene.georeference.mapConversionName = geo.at("mapConversionName").get<std::string>();

  const auto& entities = arrayField(root, "entities");
  scene.entities.reserve(entities.size());
  for (const auto& value : entities) {
    SemanticEntity entity;
    entity.id = value.at("id").get<std::string>();
    entity.sourceId = value.at("sourceId").get<std::string>();
    entity.guid = value.at("guid").get<std::string>();
    entity.ifcClass = value.at("ifcClass").get<std::string>();
    entity.name = value.at("name").get<std::string>();
    entity.kind = decodeEnum(value.at("kind"), SemanticKind::Other, semanticKindName);
    entity.rendererWorldFromLocal = decodeOptionalMatrix(value.at("rendererWorldFromLocal"));
    entity.roboticsWorldFromLocal = decodeOptionalMatrix(value.at("roboticsWorldFromLocal"));
    entity.storeyIds = arrayField(value, "storeyIds").get<std::vector<std::string>>();
    for (const auto& rep : arrayField(value, "representations")) {
      const auto& meshId = rep.at("meshId");
      if (!meshId.is_number_unsigned() || meshId.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
        invalid("mesh ID must be an unsigned 32-bit integer");
      entity.representations.push_back({
          .sourceId = rep.at("sourceId").get<std::string>(), .meshId = meshId.get<uint32_t>(),
          .geometryKind = decodeEnum(rep.at("geometryKind"), SemanticGeometryKind::Curves,
                                     semanticGeometryKindName),
          .rendererWorldFromLocal = decodeMatrix(rep.at("rendererWorldFromLocal")),
          .roboticsWorldFromLocal = decodeOptionalMatrix(rep.at("roboticsWorldFromLocal"))});
    }
    scene.entities.push_back(std::move(entity));
  }
  for (const auto& value : arrayField(root, "relationships")) {
    scene.relationships.push_back({.from = value.at("from").get<std::string>(),
        .to = value.at("to").get<std::string>(),
        .kind = decodeEnum(value.at("kind"), SemanticRelationshipKind::Child, semanticRelationshipKindName)});
  }
  for (const auto& value : arrayField(root, "diagnostics")) {
    scene.diagnostics.push_back({.code = value.at("code").get<std::string>(),
        .sourceId = value.at("sourceId").get<std::string>(), .message = value.at("message").get<std::string>()});
  }
  validate(scene);
  return scene;
}

}  // namespace

std::string serializeRoboticsScene(const RoboticsScene& scene) {
  validate(scene);
  const auto& geo = scene.georeference;
  const auto& offset = geo.coordinateOffset;
  Json manifest{{"schema", schema}, {"version", version}, {"matrixLayout", "column-major"},
                {"sourceNamespace", scene.sourceNamespace},
                {"frame", {{"roboticsConvention", "ROS REP-103"},
                           {"metersPerRendererUnit", encodeNumber(scene.frame.metersPerRendererUnit)},
                           {"sourceMetersPerUnit", encodeNumber(scene.frame.sourceMetersPerUnit)},
                           {"importScale", encodeNumber(scene.frame.importScale)},
                           {"sourceUnits", scene.frame.sourceUnits},
                           {"unitProvenance", scene.frame.unitProvenance},
                           {"roboticsFromRenderer", optionalMatrix(scene.frame.roboticsFromRenderer)}}},
                {"georeference", {{"sourceUpAxis", geo.sourceUpAxis},
                    {"coordinateOffset", offset ? Json::array({offset->x, offset->y, offset->z}) : Json(nullptr)},
                    {"coordinateOffsetSource", geo.coordinateOffsetSource},
                    {"crsName", geo.crsName}, {"crsAuthority", geo.crsAuthority},
                    {"crsCode", geo.crsCode}, {"mapConversionName", geo.mapConversionName}}},
                {"entities", Json::array()}, {"relationships", Json::array()}, {"diagnostics", Json::array()}};
  auto& entities = manifest["entities"];
  for (const auto& entity : scene.entities) {
    auto representations = Json::array();
    for (const auto& rep : entity.representations) {
      representations.push_back({{"sourceId", rep.sourceId}, {"meshId", rep.meshId},
          {"geometryKind", semanticGeometryKindName(rep.geometryKind)},
          {"rendererWorldFromLocal", encodeMatrix(rep.rendererWorldFromLocal)},
          {"roboticsWorldFromLocal", optionalMatrix(rep.roboticsWorldFromLocal)}});
    }
    entities.push_back({{"id", entity.id}, {"guid", entity.guid}, {"sourceId", entity.sourceId},
        {"ifcClass", entity.ifcClass}, {"name", entity.name}, {"kind", semanticKindName(entity.kind)},
        {"rendererWorldFromLocal", optionalMatrix(entity.rendererWorldFromLocal)},
        {"roboticsWorldFromLocal", optionalMatrix(entity.roboticsWorldFromLocal)},
        {"storeyIds", entity.storeyIds}, {"representations", std::move(representations)}});
  }
  for (const auto& relationship : scene.relationships) {
    manifest["relationships"].push_back({{"from", relationship.from}, {"to", relationship.to},
                                        {"kind", semanticRelationshipKindName(relationship.kind)}});
  }
  for (const auto& diagnostic : scene.diagnostics) {
    manifest["diagnostics"].push_back({{"code", diagnostic.code}, {"sourceId", diagnostic.sourceId},
                                     {"message", diagnostic.message}});
  }
  return manifest.dump() + '\n';
}

RoboticsScene parseRoboticsScene(std::string_view manifest) {
  try {
    return decode(Json::parse(manifest));
  } catch (const Json::exception& error) {
    invalid(error.what());
  }
}

void writeRoboticsSceneManifest(const std::filesystem::path& path, const RoboticsScene& scene) {
  container::util::writeFileAtomically(path, serializeRoboticsScene(scene));
}

RoboticsScene readRoboticsSceneManifest(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Could not open the robotics scene manifest");
  try {
    return decode(Json::parse(stream));
  } catch (const Json::exception& error) {
    invalid(error.what());
  }
}

}  // namespace container::robotics
