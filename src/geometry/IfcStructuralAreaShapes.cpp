#include "IfcStructuralAreaShapes.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>

namespace container::geometry::ifc::detail {
namespace {

bool positiveDimensions(std::initializer_list<double> values) {
  // The corners ultimately become the renderer's float32 profile vertices.
  return std::ranges::all_of(values, [](double value) {
    return std::isfinite(value) && value > 0.0 &&
           value <= std::numeric_limits<float>::max();
  });
}

bool nonnegative(double value) {
  return std::isfinite(value) && value >= 0.0;
}

std::nullopt_t reject(std::string& error, const char* message) {
  error = message;
  return std::nullopt;
}

bool validSlope(double radians) {
  return std::isfinite(radians) && std::abs(radians) < std::numbers::pi / 2;
}

double rootThickness(const IfcAsymmetricIFlange& flange, double overhang) {
  // Thickness is specified at the free flange tip. Its inner face inclines
  // toward the web, with mirrored geometry on the opposite side.
  return flange.thickness + overhang * std::tan(flange.slopeRadians);
}

} // namespace

std::optional<IfcStructuralAreaShape>
makeIfcCShapeArea(double depth, double width, double thickness, double girth,
                  std::optional<double> internalFillet, std::string& error) {
  error.clear();
  if (!positiveDimensions({depth, width, thickness, girth})) {
    return reject(error, "C profile dimensions must be finite, positive and within renderer precision");
  }
  const double x = width / 2, y = depth / 2;
  if (thickness >= std::min(x, y)) {
    return reject(error, "C profile wall thickness must be smaller than both half-extents");
  }
  if (!(thickness < girth && girth < y)) {
    return reject(error, "C profile girth must exceed the wall thickness and be smaller than half the depth");
  }
  const double inside = internalFillet.value_or(0);
  if (!nonnegative(inside) || inside > std::min(x, y) - thickness) {
    return reject(error, "C profile internal fillet radius exceeds the inner half-extents");
  }
  // Where specified, pair the inside bend radius with its concentric outer
  // radius to preserve the authored wall thickness. The free lip ends stay
  // square. An omitted radius keeps the untapered sharp-outline convention.
  const double outside = internalFillet ? inside + thickness : 0;
  const double leftInside = -x + thickness;
  const double rightInside = x - thickness;
  const double bottomInside = -y + thickness;
  const double topInside = y - thickness;
  const double bottomLip = -y + girth;
  const double topLip = y - girth;
  return IfcStructuralAreaShape{
      {{-x, -y}, {x, -y}, {x, bottomLip}, {rightInside, bottomLip},
       {rightInside, bottomInside}, {leftInside, bottomInside},
       {leftInside, topInside}, {rightInside, topInside}, {rightInside, topLip},
       {x, topLip}, {x, y}, {-x, y}},
      {outside, outside, 0, 0, inside, inside, inside, inside, 0, 0, outside, outside}};
}

std::optional<IfcStructuralAreaShape>
makeIfcZShapeArea(double depth, double flangeWidth, double webThickness,
                  double flangeThickness, double rootFillet, double edgeRadius,
                  std::string& error) {
  error.clear();
  if (!positiveDimensions({depth, flangeWidth, webThickness, flangeThickness})) {
    return reject(error, "Z profile dimensions must be finite, positive and within renderer precision");
  }
  const double y = depth / 2, web = webThickness / 2;
  if (flangeThickness >= y) {
    return reject(error, "Z profile flange thickness must be smaller than half the depth");
  }
  if (flangeWidth <= webThickness) {
    return reject(error, "Z profile flange width must exceed the web thickness");
  }
  if (!nonnegative(rootFillet) || !nonnegative(edgeRadius)) {
    return reject(error, "Z profile corner radii must be finite and nonnegative");
  }

  // The flange spans from its free tip to the opposite outer web face.
  const double x = flangeWidth - web;
  const double innerY = y - flangeThickness;
  return IfcStructuralAreaShape{
      {{-web, -y}, {x, -y}, {x, -innerY}, {web, -innerY},
       {web, y}, {-x, y}, {-x, innerY}, {-web, innerY}},
      {0, 0, edgeRadius, rootFillet, 0, 0, edgeRadius, rootFillet}};
}

std::optional<IfcStructuralAreaShape>
makeIfcAsymmetricIShapeArea(double depth, double webThickness,
                             const IfcAsymmetricIFlange& bottom,
                             const IfcAsymmetricIFlange& top, std::string& error) {
  error.clear();
  if (!positiveDimensions({depth, webThickness, bottom.width, bottom.thickness,
                           top.width, top.thickness})) {
    return reject(error, "Asymmetric I profile dimensions must be finite, positive and within renderer precision");
  }
  if (webThickness >= std::min(bottom.width, top.width)) {
    return reject(error, "Asymmetric I profile web thickness must be smaller than both flange widths");
  }
  if (bottom.thickness + top.thickness >= depth) {
    return reject(error, "Asymmetric I profile flange thicknesses must leave an open web depth");
  }
  const double bottomOverhang = (bottom.width - webThickness) / 2;
  const double topOverhang = (top.width - webThickness) / 2;
  if (!nonnegative(bottom.filletRadius) || !nonnegative(bottom.edgeRadius) ||
      !nonnegative(top.filletRadius) || !nonnegative(top.edgeRadius)) {
    return reject(error, "Asymmetric I profile corner radii must be finite and nonnegative");
  }
  if (bottom.filletRadius > bottomOverhang || top.filletRadius > topOverhang) {
    return reject(error, "Asymmetric I profile root fillet radius exceeds its flange overhang");
  }
  if (!validSlope(bottom.slopeRadians) || !validSlope(top.slopeRadians)) {
    return reject(error, "Asymmetric I profile flange slope must be finite and strictly between -90 and 90 degrees");
  }
  const double bottomRoot = rootThickness(bottom, bottomOverhang);
  const double topRoot = rootThickness(top, topOverhang);
  if (!std::isfinite(bottomRoot) || !std::isfinite(topRoot) ||
      bottomRoot <= 0 || topRoot <= 0 || bottomRoot + topRoot >= depth) {
    return reject(error, "Asymmetric I profile flange slopes collapse a flange or overlap at the web");
  }
  const double sharedOverhang = std::min(bottomOverhang, topOverhang);
  const double bottomAtSharedTip = std::lerp(bottomRoot, bottom.thickness, sharedOverhang / bottomOverhang);
  const double topAtSharedTip = std::lerp(topRoot, top.thickness, sharedOverhang / topOverhang);
  // The gap is linear from the web to the shorter flange tip. Both ends
  // must stay open when the slopes have opposite signs.
  if (bottomAtSharedTip + topAtSharedTip >= depth) {
    return reject(error, "Asymmetric I profile flange slopes overlap away from the web");
  }

  const double xBottom = bottom.width / 2, xTop = top.width / 2;
  const double y = depth / 2, web = webThickness / 2;
  const double bottomTipY = bottom.thickness - y, bottomRootY = bottomRoot - y;
  const double topTipY = y - top.thickness, topRootY = y - topRoot;
  return IfcStructuralAreaShape{
      {{-xBottom, -y}, {xBottom, -y}, {xBottom, bottomTipY}, {web, bottomRootY},
       {web, topRootY}, {xTop, topTipY}, {xTop, y}, {-xTop, y}, {-xTop, topTipY},
       {-web, topRootY}, {-web, bottomRootY}, {-xBottom, bottomTipY}},
      {0, 0, bottom.edgeRadius, bottom.filletRadius, top.filletRadius, top.edgeRadius,
       0, 0, top.edgeRadius, top.filletRadius, bottom.filletRadius, bottom.edgeRadius}};
}

} // namespace container::geometry::ifc::detail
