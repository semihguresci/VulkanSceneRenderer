#pragma once

#include "Container/geometry/ParametricCurve.h"
#include "IfcStepTypes.h"
#include <array>
#include <glm/vec2.hpp>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace container::geometry::ifc::detail {
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
};
[[nodiscard]] bool isCurveEntity(std::string_view type);
// Pure conversion: errors cannot leave partial geometry in the model buffers.
[[nodiscard]] std::optional<ParametricCurve>
readIfcCurve(const std::unordered_map<uint32_t, Entity> &entities, uint32_t id,
             double metersPerUnit, std::optional<double> radiansPerAngleUnit,
             std::string &error);
[[nodiscard]] std::optional<ParametricSurface>
readIfcSurface(const std::unordered_map<uint32_t, Entity> &entities,
               uint32_t id, double metersPerUnit,
               std::optional<double> radiansPerAngleUnit, std::string &error);
[[nodiscard]] std::vector<std::array<glm::dvec3, 3>>
readIfcSectionedSurfaceMesh(
    const std::unordered_map<uint32_t, Entity> &entities, uint32_t id,
    double metersPerUnit, std::optional<double> radiansPerAngleUnit,
    std::string &error);
} // namespace container::geometry::ifc::detail
