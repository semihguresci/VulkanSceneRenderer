#include "IfcSectionedMeshValidation.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace container::geometry::ifc::detail {
namespace {

using Triangle = std::array<glm::dvec3, 3>;

struct Bounds {
  glm::dvec3 low, high;
  size_t index;
};

Bounds bounds(const Triangle &triangle, size_t index) {
  return {glm::min(triangle[0], glm::min(triangle[1], triangle[2])),
          glm::max(triangle[0], glm::max(triangle[1], triangle[2])), index};
}

bool overlaps(const Bounds &a, const Bounds &b, double tolerance) {
  for (unsigned axis = 0; axis < 3; ++axis)
    if (a.low[axis] > b.high[axis] + tolerance ||
        b.low[axis] > a.high[axis] + tolerance)
      return false;
  return true;
}

bool intersects(const Triangle &a, const Triangle &b, double tolerance) {
  const auto edges = [](const Triangle &triangle) {
    return std::array{triangle[1] - triangle[0], triangle[2] - triangle[1],
                      triangle[0] - triangle[2]};
  };
  const auto ea = edges(a), eb = edges(b);
  const auto na = glm::normalize(glm::cross(ea[0], ea[1]));
  const auto nb = glm::normalize(glm::cross(eb[0], eb[1]));
  const auto separated = [&](glm::dvec3 axis) {
    const double length = glm::length(axis);
    if (!(length > 0) || !std::isfinite(length))
      return false;
    axis /= length;
    const auto interval = [&](const Triangle &triangle) {
      return std::minmax({glm::dot(axis, triangle[0] - a[0]),
                          glm::dot(axis, triangle[1] - a[0]),
                          glm::dot(axis, triangle[2] - a[0])});
    };
    const auto [aMin, aMax] = interval(a);
    const auto [bMin, bMax] = interval(b);
    return aMax + tolerance < bMin || bMax + tolerance < aMin;
  };
  if (separated(na) || separated(nb))
    return false;
  // The in-plane edge normals cover coplanar pairs. For noncoplanar
  // triangles, the two face normals and edge cross products are sufficient.
  for (const auto &edgeA : ea) {
    if (separated(glm::cross(edgeA, na)))
      return false;
    for (const auto &edgeB : eb)
      if (separated(glm::cross(edgeA, edgeB)))
        return false;
  }
  return std::ranges::none_of(
      eb, [&](glm::dvec3 edge) { return separated(glm::cross(edge, nb)); });
}

struct SharedBoundary {
  Triangle vertices{};
  size_t count = 0;
};

SharedBoundary sharedBoundary(const Triangle &a, const Triangle &b) {
  SharedBoundary shared;
  for (const auto &point : a)
    if (std::ranges::find(b, point) != b.end())
      shared.vertices[shared.count++] = point;
  return shared;
}

struct Face {
  const Triangle &vertices;
  glm::dvec3 normal;
  Triangle inward;
};

Face face(const Triangle &vertices) {
  const auto normal = glm::normalize(
      glm::cross(vertices[1] - vertices[0], vertices[2] - vertices[0]));
  Triangle inward;
  for (size_t i = 0; i < vertices.size(); ++i)
    inward[i] =
        glm::cross(normal, glm::normalize(vertices[(i + 1) % 3] - vertices[i]));
  return {vertices, normal, inward};
}

double boundaryDistance(glm::dvec3 point, glm::dvec3 seam) {
  const double lengthSquared = glm::dot(seam, seam);
  if (lengthSquared > 0)
    point -= seam * std::clamp(glm::dot(point, seam) / lengthSquared, 0., 1.);
  return glm::length(point);
}

bool coplanarEdgeCrosses(glm::dvec3 a, glm::dvec3 b, const Face &target,
                         glm::dvec3 seam, double tolerance, double precision) {
  double first = 0, last = 1;
  for (size_t i = 0; i < target.vertices.size(); ++i) {
    const auto &origin = target.vertices[i], &inward = target.inward[i];
    double start = glm::dot(a - origin, inward);
    double end = glm::dot(b - origin, inward);
    if (std::abs(start) <= precision)
      start = 0;
    if (std::abs(end) <= precision)
      end = 0;
    if (start < 0 && end < 0)
      return false;
    if (start < 0)
      first = std::max(first, start / (start - end));
    if (end < 0)
      last = std::min(last, start / (start - end));
    if (first > last)
      return false;
  }
  // Distance to the shared point or segment is convex along the clipped edge.
  // Its extremes bound all contacts.
  return boundaryDistance(a + (b - a) * first, seam) > tolerance ||
         boundaryDistance(a + (b - a) * last, seam) > tolerance;
}

bool crossesFace(const Triangle &source, const Face &target, glm::dvec3 seam,
                 double tolerance, double precision) {
  const auto inside = [&](glm::dvec3 point) {
    for (size_t i = 0; i < target.vertices.size(); ++i)
      if (glm::dot(point - target.vertices[i], target.inward[i]) < -precision)
        return false;
    return true;
  };
  for (size_t i = 0; i < source.size(); ++i) {
    const auto &a = source[i], &b = source[(i + 1) % 3];
    const double da = glm::dot(a - target.vertices[0], target.normal);
    const double db = glm::dot(b - target.vertices[0], target.normal);
    if (std::abs(da) <= precision && std::abs(db) <= precision) {
      if (coplanarEdgeCrosses(a, b, target, seam, tolerance, precision))
        return true;
    } else if ((da <= 0 && db >= 0) || (da >= 0 && db <= 0)) {
      const auto point = a + (b - a) * (da / (da - db));
      if (boundaryDistance(point, seam) > tolerance && inside(point))
        return true;
    } else if ((std::abs(da) <= precision &&
                boundaryDistance(a, seam) > tolerance && inside(a)) ||
               (std::abs(db) <= precision &&
                boundaryDistance(b, seam) > tolerance && inside(b))) {
      return true;
    }
  }
  return false;
}

bool crossesSharedBoundary(Triangle a, Triangle b, const SharedBoundary &shared,
                           double tolerance) {
  if (shared.count == 3)
    return true;
  const auto origin = shared.vertices[0];
  const auto seam =
      shared.count == 2 ? shared.vertices[1] - origin : glm::dvec3(0);
  double extent = 0;
  for (auto *triangle : {&a, &b})
    for (auto &point : *triangle) {
      point -= origin;
      extent = std::max(extent, glm::length(point));
    }
  const double precision = 64 * std::numeric_limits<double>::epsilon() * extent;
  tolerance = std::max(tolerance, 4 * precision);
  // Any intersection outside the shared simplex has an endpoint on one
  // triangle edge. Coplanar edges also cover a face contained in another.
  return crossesFace(a, face(b), seam, tolerance, precision) ||
         crossesFace(b, face(a), seam, tolerance, precision);
}

} // namespace

void validateIfcMiterMesh(std::span<const Triangle> triangles,
                          std::span<const size_t> rows, double tolerance,
                          std::stop_token stop) {
  if (rows.size() != triangles.size())
    throw std::logic_error("Sectioned miter triangle rows are incomplete");
  if (triangles.empty())
    return;
  std::vector<Bounds> boxes;
  boxes.reserve(triangles.size());
  auto overall = bounds(triangles.front(), 0);
  for (size_t i = 0; i < triangles.size(); ++i) {
    auto box = bounds(triangles[i], i);
    overall.low = glm::min(overall.low, box.low);
    overall.high = glm::max(overall.high, box.high);
    boxes.push_back(box);
  }
  const auto extent = overall.high - overall.low;
  unsigned axis = 0;
  for (unsigned i : {1u, 2u})
    if (extent[i] > extent[axis])
      axis = i;
  std::ranges::sort(boxes, {},
                    [axis](const Bounds &box) { return box.low[axis]; });
  size_t checks = 0;
  constexpr size_t maxChecks = 8'000'000;
  for (size_t i = 0; i < boxes.size(); ++i) {
    if (i % 256 == 0)
      container::util::checkModelImportCancellation(stop);
    const auto &a = boxes[i];
    for (size_t j = i + 1;
         j < boxes.size() && boxes[j].low[axis] <= a.high[axis] + tolerance;
         ++j) {
      if (++checks % 4096 == 0)
        container::util::checkModelImportCancellation(stop);
      if (checks > maxChecks)
        throw std::runtime_error(
            "Sectioned miter triangle overlap budget exceeded");
      const auto &b = boxes[j];
      if (!overlaps(a, b, tolerance))
        continue;
      const auto &pa = triangles[a.index], &pb = triangles[b.index];
      const auto rowA = rows[a.index], rowB = rows[b.index];
      const size_t rowsApart = std::max(rowA, rowB) - std::min(rowA, rowB);
      // Neighboring triangles can share evaluated vertices across an exact
      // seam, including more than one collapsed transverse profile strip.
      const auto shared =
          rowsApart <= 1 ? sharedBoundary(pa, pb) : SharedBoundary{};
      const bool crossing =
          shared.count > 0 ? crossesSharedBoundary(pa, pb, shared, tolerance)
                           : intersects(pa, pb, tolerance);
      if (crossing)
        throw std::runtime_error(
            "Sectioned miter surface intersects itself between mesh rows " +
            std::to_string(rowA) + " and " + std::to_string(rowB));
    }
  }
}

} // namespace container::geometry::ifc::detail
