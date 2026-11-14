#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <stop_token>

#include <glm/vec3.hpp>

namespace container::geometry::ifc::detail {

// Unlike a directrix check, testing the actual triangles also catches
// intersections of remote offsets on an otherwise simple path.
void validateIfcMiterMesh(std::span<const std::array<glm::dvec3, 3>> triangles,
                          std::span<const std::size_t> rows, double tolerance,
                          std::stop_token stop);

} // namespace container::geometry::ifc::detail
