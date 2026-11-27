#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry::dotbim {
struct Model;
}

namespace container::robotics {

enum class SemanticKind : uint8_t {
  Unknown,
  Site,
  Building,
  Storey,
  Space,
  Wall,
  Slab,
  Door,
  Opening,
  Other,
};

enum class SemanticRelationshipKind : uint8_t {
  Contains,
  Aggregates,
  Nests,
  Voids,
  Fills,
  Child,
};

enum class SemanticGeometryKind : uint8_t { Mesh, Points, Curves };

struct RoboticsDiagnostic {
  std::string code;
  std::string sourceId;
  std::string message;
};

struct SemanticRepresentation {
  std::string sourceId;
  uint32_t meshId{};
  SemanticGeometryKind geometryKind{SemanticGeometryKind::Mesh};
  glm::dmat4 rendererWorldFromLocal{1.0};
  std::optional<glm::dmat4> roboticsWorldFromLocal;
};

struct SemanticEntity {
  std::string id;
  std::string sourceId;
  std::string guid;
  std::string ifcClass;
  std::string name;
  SemanticKind kind{SemanticKind::Unknown};
  std::optional<glm::dmat4> rendererWorldFromLocal;
  // Full affine transform from the original product-local coordinate system.
  // Reflections and source-unit scale remain explicit, without a rigid-pose fit.
  std::optional<glm::dmat4> roboticsWorldFromLocal;
  std::vector<SemanticRepresentation> representations;
  std::vector<std::string> storeyIds;
};

struct SemanticRelationship {
  std::string from;
  std::string to;
  SemanticRelationshipKind kind{SemanticRelationshipKind::Child};
};

struct RoboticsFrame {
  std::optional<double> metersPerRendererUnit;
  std::optional<double> sourceMetersPerUnit;
  std::optional<double> importScale;
  std::string sourceUnits;
  std::string unitProvenance;
  std::optional<glm::dmat4> roboticsFromRenderer;
};

struct RoboticsGeoreference {
  std::string sourceUpAxis;
  std::optional<glm::dvec3> coordinateOffset;
  std::string coordinateOffsetSource;
  std::string crsName;
  std::string crsAuthority;
  std::string crsCode;
  std::string mapConversionName;
};

struct RoboticsScene {
  std::string sourceNamespace;
  RoboticsFrame frame;
  RoboticsGeoreference georeference;
  std::vector<SemanticEntity> entities;
  std::vector<SemanticRelationship> relationships;
  std::vector<RoboticsDiagnostic> diagnostics;
};

struct RoboticsSceneOptions {
  std::string sourceNamespace;
  // Explicit metric evidence for inputs whose importer retained no units.
  std::optional<double> metersPerRendererUnit;
};

// Renderer (-Z forward, -X left, +Y up) to ROS REP-103 (+X, +Y, +Z).
// Transforms act on column vectors. Metric scale applies once to world space.
[[nodiscard]] glm::dmat4 rendererToRosTransform(double metersPerRendererUnit);
[[nodiscard]] SemanticKind classifySemanticKind(std::string_view ifcClass);
[[nodiscard]] std::string_view semanticKindName(SemanticKind kind);
[[nodiscard]] std::string_view semanticRelationshipKindName(SemanticRelationshipKind kind);
[[nodiscard]] std::string_view semanticGeometryKindName(SemanticGeometryKind kind);

// Consumes the CPU importer model. Entity IDs and output order are independent
// of input row order and renderer object indices. Composed instances always
// retain their occurrence in the ID, including when their GUID is unique.
// Unknown units/placements and
// unsupported semantics receive diagnostics. No traversability is inferred.
// Contradictory facts for one source occurrence and invalid finite/affine input
// throw invalid_argument before any output is published.
[[nodiscard]] RoboticsScene buildRoboticsScene(
    const geometry::dotbim::Model& model, const RoboticsSceneOptions& options);

}  // namespace container::robotics
