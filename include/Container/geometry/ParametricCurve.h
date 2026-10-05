#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

namespace container::geometry {

struct CurveSample {
  double parameter{0};
  glm::dvec3 point{0};
};

struct CurveFrame {
  glm::dvec3 origin{0}, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
};

// Parameters stay in the source curve's domain; positions stay in source units.
// An absent domain denotes an unbounded curve, which needs an explicit trim.
struct ParametricCurve {
  using Evaluator = std::function<std::optional<glm::dvec3>(double)>;
  Evaluator point{};
  std::optional<std::pair<double, double>> domain{};
  std::vector<double> breaks{};
  double period{0};
  unsigned dimension{3};
  Evaluator derivative{};
  std::function<bool(double, double)> stepValidator{};
  std::optional<double> constantSpeed{};
  std::function<std::optional<CurveFrame>(double)> frame{};
  std::function<double(double)> curvature{};
  std::function<std::optional<double>(const glm::dvec3 &)> inverse{};
  std::function<std::vector<CurveSample>(double, double, double, size_t)>
      sampler{};
};

[[nodiscard]] std::vector<CurveSample> sampleCurve(const ParametricCurve &,
                                                   double first, double last,
                                                   double chordError,
                                                   size_t maxPoints = 65536);
[[nodiscard]] std::optional<glm::dvec3> curveTangent(const ParametricCurve &,
                                                     double parameter);
[[nodiscard]] std::optional<glm::dvec3> curveDerivative(const ParametricCurve &,
                                                        double parameter);
[[nodiscard]] std::optional<CurveFrame> curveFrame(const ParametricCurve &,
                                                   double parameter);
[[nodiscard]] std::optional<double> curveLength(const ParametricCurve &,
                                                double first, double last,
                                                double tolerance);
[[nodiscard]] std::optional<double>
curveParameterAtDistance(const ParametricCurve &, double distance,
                         double origin, double tolerance);
[[nodiscard]] std::optional<glm::dvec3>
integrateCurveFunction(const ParametricCurve::Evaluator &, double first,
                       double last, double tolerance,
                       size_t initialIntervals = 16);
[[nodiscard]] std::optional<double>
curveParameterAtPoint(const ParametricCurve &, const glm::dvec3 &,
                      double tolerance);

// Validates explicit knots and positive rational weights. Tessellation uses
// homogeneous Bezier subdivision and a control-hull chord error bound.
[[nodiscard]] std::optional<ParametricCurve>
makeBSplineCurve(unsigned degree, std::vector<glm::dvec3> controlPoints,
                 std::vector<double> knots, std::vector<double> weights = {},
                 unsigned dimension = 3);

} // namespace container::geometry
