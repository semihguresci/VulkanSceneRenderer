#include "IfcAreaProfileGeometry.h"

#include "Container/utility/ModelImportCancellation.h"

#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>

namespace container::geometry::ifc::detail {
namespace {

constexpr size_t kMaxQuarterSegments = 1024;
constexpr size_t kMaxCompositeRegions = 128;
constexpr size_t kMaxCompositePoints = 32768;
constexpr size_t kMaxCompositeChecks = 8'000'000;
constexpr double kRoundoff = 32.0 * std::numeric_limits<double>::epsilon();

using Point = glm::dvec2;

std::vector<glm::vec3> rejected(std::string& error, const char* reason) {
  error = reason;
  return {};
}

double cross(Point a, Point b) {
  return a.x * b.y - a.y * b.x;
}

bool finite(Point point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

bool fitsFloat(Point point) {
  constexpr double max = std::numeric_limits<float>::max();
  return finite(point) && std::abs(point.x) <= max && std::abs(point.y) <= max;
}

int orientation(Point a, Point b, Point p) {
  const auto d = b - a;
  const auto v = p - a;
  const double determinant = cross(d, v);
  const double rounding = kRoundoff * (std::abs(d.x * v.y) + std::abs(d.y * v.x));
  if (determinant > rounding) return 1;
  if (determinant < -rounding) return -1;
  return 0;
}

bool inSegmentBounds(Point a, Point b, Point p, double tolerance) {
  for (int axis = 0; axis < 2; ++axis) {
    if (p[axis] - std::min(a[axis], b[axis]) < -tolerance ||
        p[axis] - std::max(a[axis], b[axis]) > tolerance) return false;
  }
  return true;
}

bool intersects(Point a, Point b, Point c, Point d, double tolerance = 0.0) {
  for (int axis = 0; axis < 2; ++axis) {
    if (std::min(c[axis], d[axis]) - std::max(a[axis], b[axis]) > tolerance ||
        std::min(a[axis], b[axis]) - std::max(c[axis], d[axis]) > tolerance) return false;
  }
  const int abc = orientation(a, b, c);
  const int abd = orientation(a, b, d);
  const int cda = orientation(c, d, a);
  const int cdb = orientation(c, d, b);
  if (abc * abd < 0 && cda * cdb < 0) return true;
  const auto onEdge = [tolerance](Point start, Point end, Point point, int side) {
    if (!inSegmentBounds(start, end, point, tolerance)) return false;
    if (side == 0 && tolerance == 0.0) return true;
    const auto edge = end - start;
    return std::abs(cross(edge, point - start)) <= tolerance * std::hypot(edge.x, edge.y);
  };
  return onEdge(a, b, c, abc) || onEdge(a, b, d, abd) ||
         onEdge(c, d, a, cda) || onEdge(c, d, b, cdb);
}

bool isSimple(std::span<const Point> points, bool closed, std::stop_token stop) {
  const size_t edgeCount = closed ? points.size() : points.size() - 1;
  for (size_t i = 0; i < edgeCount; ++i) {
    container::util::checkModelImportCancellation(stop);
    const auto a = points[i];
    const auto b = points[(i + 1) % points.size()];
    for (size_t j = i + 2; j < edgeCount; ++j) {
      if (closed && i == 0 && j == edgeCount - 1) continue;
      if (intersects(a, b, points[j], points[(j + 1) % points.size()])) return false;
    }
  }
  return true;
}

double signedArea(std::span<const Point> ring) {
  double sum = 0.0;
  for (size_t i = 1; i + 1 < ring.size(); ++i) {
    sum += cross(ring[i] - ring.front(), ring[i + 1] - ring.front());
  }
  return 0.5 * sum;
}

std::vector<glm::vec3> toFloatRing(std::span<const Point> ring) {
  std::vector<glm::vec3> result;
  result.reserve(ring.size());
  for (const auto point : ring) {
    if (!fitsFloat(point)) return {};
    const glm::vec3 vertex(float(point.x), float(point.y), 0.f);
    if (!result.empty() && result.back() == vertex) return {};
    result.push_back(vertex);
  }
  if (result.size() < 3 || result.front() == result.back()) return {};
  return result;
}

struct Bounds {
  Point min{std::numeric_limits<double>::infinity()};
  Point max{-std::numeric_limits<double>::infinity()};

  void include(Point point) {
    for (int axis = 0; axis < 2; ++axis) {
      min[axis] = std::min(min[axis], point[axis]);
      max[axis] = std::max(max[axis], point[axis]);
    }
  }

  [[nodiscard]] double size() const { return std::max(max.x - min.x, max.y - min.y); }
};

struct Region {
  std::span<const std::vector<glm::vec3>> rings;
  Bounds bounds;
  size_t index{};
};

bool containsPoint(std::span<const glm::vec3> ring, Point point) {
  bool inside = false;
  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    const Point a = Point(ring[j]) - point;
    const Point b = Point(ring[i]) - point;
    if ((a.y > 0.0) != (b.y > 0.0) && a.x - a.y * (b.x - a.x) / (b.y - a.y) > 0.0) {
      inside = !inside;
    }
  }
  return inside;
}

bool containsFilledPoint(const Region& region, Point point) {
  if (point.x < region.bounds.min.x || point.x > region.bounds.max.x ||
      point.y < region.bounds.min.y || point.y > region.bounds.max.y ||
      !containsPoint(region.rings.front(), point)) return false;
  return std::ranges::none_of(region.rings.subspan(1), [point](const auto& hole) {
    return containsPoint(hole, point);
  });
}

std::string regionPair(const Region& a, const Region& b) {
  return "Composite area profile component " + std::to_string(a.index + 1) +
         " and component " + std::to_string(b.index + 1);
}

} // namespace

std::vector<glm::vec3> makeIfcRoundedRectangleLoop(double width, double height, double radius,
                                                 size_t quarterSegments, std::string& error) {
  error.clear();
  const double x = width * 0.5, y = height * 0.5;
  if (!(x > 0.0) || !(y > 0.0) || !fitsFloat({x, y})) {
    return rejected(error, "Rectangle dimensions must be positive and fit renderer precision");
  }
  if (!std::isfinite(radius) || radius < 0.0 || radius > std::min(x, y)) {
    return rejected(error, "Rectangle corner radius must be nonnegative and no larger than either half dimension");
  }
  if (radius == 0.0) {
    const std::array<Point, 4> corners{{{-x, -y}, {x, -y}, {x, y}, {-x, y}}};
    auto ring = toFloatRing(corners);
    if (ring.empty()) return rejected(error, "Rectangle corners collapse at renderer precision");
    return ring;
  }
  if (quarterSegments == 0 || quarterSegments > kMaxQuarterSegments) {
    return rejected(error, "Rectangle fillet exceeds its arc tessellation budget");
  }

  const auto dx = x - radius, dy = y - radius;
  const std::array<Point, 4> centers{{{dx, dy}, {-dx, dy}, {-dx, -dy}, {dx, -dy}}};
  const std::array<Point, 4> starts{{{x, dy}, {-dx, y}, {-x, -dy}, {dx, -y}}};
  const std::array<Point, 4> ends{{{dx, y}, {-x, dy}, {-dx, -y}, {x, -dy}}};
  std::vector<Point> ring;
  ring.reserve(4 * (quarterSegments + 1));
  constexpr double quarterTurn = std::numbers::pi / 2.0;
  for (size_t quarter = 0; quarter < 4; ++quarter) {
    // Analytic axis endpoints remain exact for both round and capsule limits.
    // At a maximum radius adjacent arcs can share a tangent endpoint.
    if (ring.empty() || starts[quarter] != ring.back()) ring.push_back(starts[quarter]);
    for (size_t segment = 1; segment < quarterSegments; ++segment) {
      const auto angle = quarterTurn * (double(quarter) + double(segment) / double(quarterSegments));
      ring.push_back(centers[quarter] + radius * Point{std::cos(angle), std::sin(angle)});
    }
    ring.push_back(ends[quarter]);
  }
  if (ring.front() == ring.back()) ring.pop_back();
  auto result = toFloatRing(ring);
  if (result.empty()) return rejected(error, "Rectangle fillet samples collapse at renderer precision");
  return result;
}

std::vector<glm::vec3> makeIfcCenterLineAreaLoop(std::span<const glm::vec3> points,
                                              double fullThickness, std::string& error,
                                              std::stop_token stop) {
  error.clear();
  container::util::checkModelImportCancellation(stop);
  if (points.size() < 2 || points.size() > kMaxIfcCenterLinePoints) {
    return rejected(error, "Centerline needs between 2 and 1024 points");
  }
  const double halfThickness = fullThickness * 0.5;
  if (!(halfThickness > 0.0) || !fitsFloat({halfThickness, 0.0})) {
    return rejected(error, "Centerline thickness must be positive and fit renderer precision");
  }
  const Point origin(points.front());
  std::vector<Point> path;
  path.reserve(points.size());
  for (const auto& point : points) {
    const Point xy(point);
    if (!finite(xy) || point.z != 0.0f) {
      return rejected(error, "Centerline points must be finite and two dimensional");
    }
    path.push_back(xy - origin);
  }
  if (path.front() == path.back()) return rejected(error, "Centerline must be open");

  std::vector<Point> directions, normals;
  directions.reserve(path.size() - 1);
  normals.reserve(path.size() - 1);
  double totalLength = 0.0;
  for (size_t i = 1; i < path.size(); ++i) {
    const auto delta = path[i] - path[i - 1];
    const double length = std::hypot(delta.x, delta.y);
    if (length == 0.0) return rejected(error, "Centerline has coincident consecutive points");
    totalLength += length;
    const auto direction = delta / length;
    directions.push_back(direction);
    normals.emplace_back(-direction.y, direction.x);
  }
  if (!isSimple(path, false, stop)) return rejected(error, "Centerline crosses or touches itself");

  std::vector<Point> offsets;
  offsets.reserve(path.size());
  offsets.push_back(halfThickness * normals.front());
  for (size_t i = 1; i + 1 < path.size(); ++i) {
    const auto previous = directions[i - 1], next = directions[i];
    const double cosine = previous.x * next.x + previous.y * next.y;
    // Limit the miter to twice the half-thickness (a 120-degree turn).
    if (cosine < -0.5 - kRoundoff) {
      return rejected(error, "Centerline turn exceeds the supported 120 degree miter");
    }
    offsets.push_back((normals[i - 1] + normals[i]) * (halfThickness / (1.0 + cosine)));
  }
  offsets.push_back(halfThickness * normals.back());

  for (size_t i = 0; i < directions.size(); ++i) {
    const auto step = path[i + 1] - path[i];
    const auto offsetChange = offsets[i + 1] - offsets[i];
    const auto direction = directions[i];
    const double advance = step.x * direction.x + step.y * direction.y;
    const double shift = offsetChange.x * direction.x + offsetChange.y * direction.y;
    if (advance <= std::abs(shift)) {
      return rejected(error, "Centerline thickness folds an offset segment at a miter");
    }
  }
  std::vector<Point> outline;
  outline.reserve(path.size() * 2);
  for (size_t i = 0; i < path.size(); ++i) outline.push_back(origin + (path[i] - offsets[i]));
  for (size_t i = path.size(); i > 0; --i) outline.push_back(origin + (path[i - 1] + offsets[i - 1]));
  auto result = toFloatRing(outline);
  if (result.empty()) return rejected(error, "Centerline outline collapses at renderer precision");

  // Validate the rounded result as well. A valid double-precision strip can
  // still lose width or make nonadjacent edges touch after conversion.
  for (size_t i = 0; i < result.size(); ++i) outline[i] = Point(result[i]) - origin;
  const double expectedArea = fullThickness * totalLength;
  const double area = signedArea(outline);
  if (area <= 0.0 || !isSimple(outline, true, stop)) {
    return rejected(error, "Centerline offset boundary intersects, touches, or collapses");
  }
  if (std::abs(area - expectedArea) > expectedArea * 1e-3) {
    return rejected(error, "Centerline outline loses its thickness at renderer precision");
  }
  return result;
}

bool validateIfcCompositeAreaRegions(
    std::span<const std::span<const std::vector<glm::vec3>>> regions,
    std::string& error, std::stop_token stop) {
  error.clear();
  container::util::checkModelImportCancellation(stop);
  if (regions.size() > kMaxCompositeRegions) {
    error = "Composite area profile exceeds its 128 component limit";
    return false;
  }
  std::vector<Region> ordered;
  ordered.reserve(regions.size());
  double sceneExtent = 0.0;
  size_t remaining = kMaxCompositePoints;
  for (size_t index = 0; index < regions.size(); ++index) {
    container::util::checkModelImportCancellation(stop);
    Region region{.rings = regions[index], .index = index};
    if (region.rings.empty()) {
      error = "Composite area profile component has no boundary";
      return false;
    }
    for (const auto& ring : region.rings) {
      if (ring.size() < 3 || ring.size() > remaining) {
        error = "Composite area profile has an invalid ring or exceeds its boundary point limit";
        return false;
      }
      remaining -= ring.size();
      for (const auto& vertex : ring) {
        const Point point(vertex);
        if (!finite(point) || vertex.z != 0.0f) {
          error = "Composite area profile boundary is not finite and planar";
          return false;
        }
        region.bounds.include(point);
      }
    }
    sceneExtent = std::max(sceneExtent, region.bounds.size());
    ordered.push_back(region);
  }
  std::ranges::sort(ordered, [](const Region& a, const Region& b) {
    return std::tie(a.bounds.min.x, a.index) < std::tie(b.bounds.min.x, b.index);
  });

  size_t checks = 0;
  for (size_t i = 0; i < ordered.size(); ++i) {
    const auto& a = ordered[i];
    for (size_t j = i + 1; j < ordered.size(); ++j) {
      container::util::checkModelImportCancellation(stop);
      const auto& b = ordered[j];
      const double gapX = b.bounds.min.x - a.bounds.max.x;
      if (gapX > kRoundoff * sceneExtent) break;
      const double tolerance = kRoundoff * std::max(a.bounds.size(), b.bounds.size());
      // Bounds use the already quantized mesh coordinates. A local relative
      // tolerance avoids rejecting real gaps elsewhere in a large IFC model.
      if (gapX > tolerance || b.bounds.min.y - a.bounds.max.y > tolerance ||
          a.bounds.min.y - b.bounds.max.y > tolerance) continue;
      for (const auto& ringA : a.rings) {
        for (const auto& ringB : b.rings) {
          for (size_t ea = 0; ea < ringA.size(); ++ea) {
            container::util::checkModelImportCancellation(stop);
            if (ringB.size() > kMaxCompositeChecks - checks) {
              error = "Composite area profile exceeds its boundary comparison budget";
              return false;
            }
            checks += ringB.size();
            for (size_t eb = 0; eb < ringB.size(); ++eb) {
              if (intersects(Point(ringA[ea]), Point(ringA[(ea + 1) % ringA.size()]),
                             Point(ringB[eb]), Point(ringB[(eb + 1) % ringB.size()]),
                             tolerance)) {
                error = regionPair(a, b) + " have crossing or touching boundaries";
                return false;
              }
            }
          }
        }
      }
      // With disjoint boundaries, overlapping material requires one outer
      // ring to lie in the other's filled area. A hole is unoccupied space.
      if (containsFilledPoint(a, Point(b.rings.front().front())) ||
          containsFilledPoint(b, Point(a.rings.front().front()))) {
        error = regionPair(a, b) + " overlap in filled area";
        return false;
      }
    }
  }
  return true;
}

} // namespace container::geometry::ifc::detail
