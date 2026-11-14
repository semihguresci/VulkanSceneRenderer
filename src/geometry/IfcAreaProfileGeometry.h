#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace container::geometry::ifc::detail {

inline constexpr size_t kMaxIfcCenterLinePoints = 1024;

// Generate one counter-clockwise ring in IFC's local profile XY plane.
// A radius of zero gives sharp corners. Positive radii need at least one
// segment per quarter arc. Shared tangent endpoints appear once.
[[nodiscard]] std::vector<glm::vec3>
makeIfcRoundedRectangleLoop(double width, double height, double radius,
                            size_t quarterSegments, std::string& error);

// Generate a constant-width area around an open 2D polyline using perpendicular
// ends and bounded miters. The polyline and its offset boundary must be simple.
// The caller verifies that the source IFC curve is of dimension 2.
[[nodiscard]] std::vector<glm::vec3>
makeIfcCenterLineAreaLoop(std::span<const glm::vec3> points, double fullThickness,
                          std::string& error, std::stop_token stop = {});

// Each region has one outer ring followed by its holes. The caller validates
// the individual regions. Filled areas must be strictly disjoint.
[[nodiscard]] bool validateIfcCompositeAreaRegions(
    std::span<const std::span<const std::vector<glm::vec3>>> regions,
    std::string& error, std::stop_token stop = {});

} // namespace container::geometry::ifc::detail
