#pragma once

#include "Container/geometry/ParametricCurve.h"
#include "IfcStepTypes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>

namespace container::geometry::ifc::detail {

struct ProfileTransform2D;

struct IfcSweptProfileBoundary {
  uint32_t sourceId;
  ParametricCurve curve;
  bool inner = false;
};

struct IfcSweptProfileCallbacks {
  std::function<const Entity &(uint32_t)> entity;
  std::function<ParametricCurve(uint32_t)> readCurve;
  std::function<ParametricCurve(uint32_t)> readSingleProfile;
  std::function<CurveFrame(uint32_t)> readProfilePlacement;
  std::function<ProfileTransform2D(uint32_t)> readTransform;
  std::function<ParametricCurve(ParametricCurve, ProfileTransform2D)>
      applyTransform;
};

// Boundaries have independent source parameter domains. Inner walls retain the
// native parameter direction; the caller uses the role to orient their mesh.
[[nodiscard]] std::vector<IfcSweptProfileBoundary>
readIfcSweptProfileBoundaries(uint32_t profile,
                              const IfcSweptProfileCallbacks &callbacks,
                              double pointTolerance,
                              std::optional<double> radiansPerAngleUnit,
                              std::stop_token stop = {});

} // namespace container::geometry::ifc::detail
