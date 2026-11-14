#pragma once

#include <glm/vec2.hpp>

#include <optional>
#include <string>
#include <vector>

namespace container::geometry::ifc::detail {

// A counter-clockwise ring at the authored section's bounding-box origin.
// Radii correspond to the sharp corners before tangent-arc tessellation.
struct IfcStructuralAreaShape {
  std::vector<glm::dvec2> corners;
  std::vector<double> radii;
};

struct IfcAsymmetricIFlange {
  double width{}, thickness{}, filletRadius{}, edgeRadius{}, slopeRadians{};
};

[[nodiscard]] std::optional<IfcStructuralAreaShape>
makeIfcCShapeArea(double depth, double width, double thickness, double girth,
                  std::optional<double> internalFillet, std::string& error);

[[nodiscard]] std::optional<IfcStructuralAreaShape>
makeIfcZShapeArea(double depth, double flangeWidth, double webThickness,
                  double flangeThickness, double rootFillet, double edgeRadius,
                  std::string& error);

[[nodiscard]] std::optional<IfcStructuralAreaShape>
makeIfcAsymmetricIShapeArea(double depth, double webThickness,
                             const IfcAsymmetricIFlange& bottom,
                             const IfcAsymmetricIFlange& top, std::string& error);

} // namespace container::geometry::ifc::detail
