#pragma once

#include "Container/geometry/ParametricCurve.h"
#include "IfcStepTypes.h"
#include <array>
#include <glm/vec2.hpp>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <unordered_map>

namespace container::geometry::ifc::detail {
struct ProfileTransform2D {
  uint32_t parent;
  // Columns include scale; the origin is applied after the parent placement.
  glm::dvec2 origin{0}, x{1, 0}, y{0, 1};
  double maxScale{1};
};
[[nodiscard]] std::optional<ProfileTransform2D>
readIfcProfileTransform(const std::unordered_map<uint32_t, Entity> &entities,
                        uint32_t id, std::string &error);
struct SphericalSurface {
  CurveFrame frame;
  double radius;
};
struct ParametricSurface {
  std::function<std::optional<glm::dvec3>(double, double)> point;
  glm::dvec2 angularScale{0};
  glm::dvec2 periods{0};
  std::optional<std::pair<double, double>> uDomain, vDomain;
  std::function<bool(glm::dvec2, glm::dvec2)> regionStep;
  std::vector<double> meshU, meshV;
  // Analytic inverses return a canonical parameter; callers unwrap periods.
  std::function<std::optional<glm::dvec2>(glm::dvec3)> inverse;
  // Polar topology needs regular local charts instead of angular coordinates.
  std::optional<SphericalSurface> sphere;
  // Branch tips and zero-width profile segments create coincident mesh edges.
  bool allowCoincidentEdges = false;
  std::optional<glm::dvec3> meshUp;
  std::function<std::optional<glm::dvec3>(double, double)> meshNormal;
  uint32_t profileSourceId = 0;
  bool innerProfile = false;
  bool reverseMeshWinding = false;
  bool standaloneRegular = true;
  bool ruledSectionPanels = false;
  std::vector<double> creaseU, creaseV;
};
[[nodiscard]] bool isCurveEntity(std::string_view type);
// Pure conversion: errors cannot leave partial geometry in the model buffers.
[[nodiscard]] std::optional<ParametricCurve>
readIfcCurve(const std::unordered_map<uint32_t, Entity> &entities, uint32_t id,
             double metersPerUnit, std::optional<double> radiansPerAngleUnit,
             std::string &error, std::span<const uint32_t> guides = {},
             std::stop_token stop = {},
             std::span<const uint32_t> differentFrameGuides = {});
[[nodiscard]] std::optional<ParametricCurve>
readIfcEdgeCurve(const std::unordered_map<uint32_t, Entity> &entities,
                 uint32_t id, double metersPerUnit,
                 std::optional<double> radiansPerAngleUnit, glm::dvec3 start,
                 glm::dvec3 end, std::string &error);
[[nodiscard]] std::optional<ParametricSurface>
readIfcSurface(const std::unordered_map<uint32_t, Entity> &entities,
               uint32_t id, double metersPerUnit,
               std::optional<double> radiansPerAngleUnit, std::string &error);
[[nodiscard]] bool
readIfcSurfaceCharts(const std::unordered_map<uint32_t, Entity> &entities,
                     uint32_t id, double metersPerUnit,
                     std::optional<double> radiansPerAngleUnit,
                     std::vector<ParametricSurface> &charts, std::string &error,
                     std::stop_token stop = {});
[[nodiscard]] std::vector<std::array<glm::dvec3, 3>>
readIfcSectionedSurfaceMesh(
    const std::unordered_map<uint32_t, Entity> &entities, uint32_t id,
    double metersPerUnit, std::optional<double> radiansPerAngleUnit,
    std::string &error, std::span<const uint32_t> guides = {},
    std::stop_token stop = {},
    std::span<const uint32_t> differentFrameGuides = {});
} // namespace container::geometry::ifc::detail
