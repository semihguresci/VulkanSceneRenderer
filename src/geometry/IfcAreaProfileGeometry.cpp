#include "IfcAreaProfileGeometry.h"

#include "Container/utility/ModelImportCancellation.h"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <tuple>

namespace container::geometry::ifc::detail {
namespace {

constexpr size_t kMaxQuarterSegments = 1024;
constexpr size_t kMaxCurvedCenterLineVertices = 4096;
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

double length(Point point) {
  return std::hypot(point.x, point.y);
}

Point leftNormal(Point tangent) {
  return {-tangent.y, tangent.x};
}

Point rotate(Point point, double radians) {
  return std::cos(radians) * point + std::sin(radians) * leftNormal(point);
}

struct CenterLinePiece {
  Point start{}, end{}, center{};
  double radius = 0.0;
  double sweep = 0.0;

  [[nodiscard]] Point tangent(bool atEnd) const {
    if (radius == 0.0) {
      const auto delta = end - start;
      return delta / length(delta);
    }
    const auto radial = (atEnd ? end : start) - center;
    return leftNormal(radial) / std::copysign(length(radial), sweep);
  }

  [[nodiscard]] CenterLinePiece reversed() const {
    return {end, start, center, radius, -sweep};
  }
};

bool onArc(const CenterLinePiece& arc, Point point, double tolerance) {
  const auto radial = point - arc.center;
  if (std::abs(length(radial) - arc.radius) > tolerance) return false;
  const auto first = (arc.start - arc.center) / arc.radius;
  const auto direction = radial / arc.radius;
  const double angle =
      std::atan2(cross(first, direction), glm::dot(first, direction)) *
      std::copysign(1.0, arc.sweep);
  constexpr double turn = 2.0 * std::numbers::pi;
  const double along = angle < 0 ? angle + turn : angle;
  const double angularTolerance = tolerance / arc.radius;
  return along <= std::abs(arc.sweep) + angularTolerance ||
         along >= turn - angularTolerance;
}

bool piecesMeet(const CenterLinePiece& a, const CenterLinePiece& b,
                std::optional<Point> shared) {
  const double tolerance =
      4.0 * kRoundoff * std::max({length(a.start), length(a.end), a.radius,
                                  length(b.start), length(b.end), b.radius});
  const auto extra = [&](Point point) {
    return !shared || length(point - *shared) > tolerance;
  };
  if (a.radius == 0.0 && b.radius == 0.0) {
    if (!shared) return intersects(a.start, a.end, b.start, b.end, tolerance);
    // Two connected line segments only meet again when they overlap.
    const auto away = [&](const CenterLinePiece& piece) {
      return length(piece.start - *shared) > tolerance ? piece.start : piece.end;
    };
    const auto p = away(a), q = away(b);
    return orientation(*shared, p, q) == 0 &&
           glm::dot(p - *shared, q - *shared) > 0.0;
  }

  if (a.radius == 0.0 || b.radius == 0.0) {
    const auto& line = a.radius == 0.0 ? a : b;
    const auto& arc = a.radius == 0.0 ? b : a;
    const auto delta = line.end - line.start;
    const double extent = length(delta);
    if (extent <= tolerance) return true;
    const auto direction = delta / extent;
    const auto toCenter = arc.center - line.start;
    const double along = glm::dot(toCenter, direction);
    const double distance = std::abs(cross(toCenter, direction));
    const double gap = arc.radius - distance;
    if (gap < -tolerance) return false;
    const auto hit = [&](double t) {
      const auto point = line.start + t * direction;
      return t >= -tolerance && t <= extent + tolerance && extra(point) &&
             onArc(arc, point, tolerance);
    };
    if (gap <= tolerance) return !shared && hit(along);
    const double halfChord = std::sqrt(gap * (arc.radius + distance));
    return hit(along - halfChord) || hit(along + halfChord);
  }

  const auto delta = b.center - a.center;
  const double distance = length(delta);
  const double outerGap = a.radius + b.radius - distance;
  const double innerGap = distance - std::abs(a.radius - b.radius);
  if (outerGap < -tolerance || innerGap < -tolerance) return false;
  if (distance <= tolerance) {
    if (std::abs(a.radius - b.radius) > tolerance) return false;
    // Intersecting intervals on the same circle include an endpoint.
    return (extra(a.start) && onArc(b, a.start, tolerance)) ||
           (extra(a.end) && onArc(b, a.end, tolerance)) ||
           (extra(b.start) && onArc(a, b.start, tolerance)) ||
           (extra(b.end) && onArc(a, b.end, tolerance));
  }
  const auto direction = delta / distance;
  const auto foot =
      a.center + .5 * (distance + (a.radius - b.radius) / distance *
                                      (a.radius + b.radius)) * direction;
  const auto hit = [&](Point point) {
    return extra(point) && onArc(a, point, tolerance) && onArc(b, point, tolerance);
  };
  if (outerGap <= tolerance || innerGap <= tolerance) return !shared && hit(foot);
  // Heron's factorization stays stable at internal and external tangency.
  const double halfChord = .5 * std::sqrt(
      (outerGap * (a.radius + b.radius + distance)) *
      (innerGap * (distance + std::abs(a.radius - b.radius)))) / distance;
  const auto perpendicular = halfChord * leftNormal(direction);
  return hit(foot - perpendicular) || hit(foot + perpendicular);
}

bool isSimple(std::span<const CenterLinePiece> pieces, bool closed,
              std::stop_token stop) {
  for (size_t i = 0; i < pieces.size(); ++i) {
    container::util::checkModelImportCancellation(stop);
    for (size_t j = i + 1; j < pieces.size(); ++j) {
      std::optional<Point> shared;
      if (j == i + 1) shared = pieces[i].end;
      if (closed && i == 0 && j + 1 == pieces.size()) shared = pieces[i].start;
      if (piecesMeet(pieces[i], pieces[j], shared)) return false;
    }
  }
  return true;
}

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

std::vector<glm::vec3>
makeIfcCurvedCenterLineAreaLoop(std::span<const IfcCenterLineSegment> segments,
                                double fullThickness, double chordError,
                                std::string& error, std::stop_token stop) {
  error.clear();
  container::util::checkModelImportCancellation(stop);
  const double halfThickness = fullThickness * 0.5;
  if (!(halfThickness > 0.0) || !fitsFloat({halfThickness, 0.0})) {
    return rejected(error, "Centerline thickness must be positive and fit renderer precision");
  }
  if (!(chordError > 0.0) || !std::isfinite(chordError) || segments.empty() ||
      segments.size() >= kMaxIfcCenterLinePoints) {
    return rejected(error, "Invalid centerline chord tolerance or segment count");
  }
  const Point origin = segments.front().start;
  const double joinTolerance = std::min(chordError * .01, halfThickness * 1e-6);
  std::vector<CenterLinePiece> path;
  path.reserve(segments.size());
  std::vector<size_t> steps;
  steps.reserve(segments.size());
  size_t railVertices = 1;
  double areaPerUnitThickness = 0.0;
  for (const auto& segment : segments) {
    container::util::checkModelImportCancellation(stop);
    if (!fitsFloat(segment.start) || !fitsFloat(segment.end) ||
        !std::isfinite(segment.sweepRadians)) {
      return rejected(error, "Centerline segments must have finite 2D geometry");
    }
    const double tolerance =
        joinTolerance + 4.0 * kRoundoff *
                            std::max(length(segment.start), length(segment.end));
    CenterLinePiece piece{segment.start - origin, segment.end - origin};
    if (!path.empty()) {
      if (length(path.back().end - piece.start) > tolerance) {
        return rejected(error, "Centerline segments are discontinuous");
      }
      piece.start = path.back().end;
    }
    const double chordLength = length(piece.end - piece.start);
    if (!(chordLength > tolerance)) {
      return rejected(error, "Centerline segment endpoints coincide at profile precision");
    }
    size_t count = 1;
    if (segment.sweepRadians != 0.0) {
      const double extent = std::abs(segment.sweepRadians);
      if (!fitsFloat(segment.center) || extent >= 2.0 * std::numbers::pi) {
        return rejected(error, "Centerline arc requires a finite center and an open sweep");
      }
      piece.center = segment.center - origin;
      piece.radius = length(piece.start - piece.center);
      piece.sweep = segment.sweepRadians;
      const auto predicted = piece.center + rotate(piece.start - piece.center, piece.sweep);
      if (length(predicted - piece.end) >
          tolerance + 4.0 * kRoundoff * piece.radius) {
        return rejected(error, "Centerline arc endpoints disagree with its signed sweep");
      }
      if (!(piece.radius - halfThickness > tolerance)) {
        return rejected(error, "Centerline thickness collapses the inner arc offset");
      }
      const double outerRadius = piece.radius + halfThickness;
      const double maxAngle = std::min(
          std::numbers::pi / 2.0,
          4.0 * std::asin(std::sqrt(std::min(.5, (chordError / outerRadius) * .5))));
      const double stepCount = std::max(1.0, std::ceil(extent / maxAngle));
      if (!std::isfinite(stepCount) ||
          stepCount > double(kMaxCurvedCenterLineVertices / 2)) {
        return rejected(error, "Centerline arc exceeds its chord sampling budget");
      }
      count = static_cast<size_t>(stepCount);
      // Each pair of chords bounds an annular sector with its sine in place
      // of the angle. Compare against that area after float conversion.
      areaPerUnitThickness +=
          piece.radius * double(count) * std::sin(extent / double(count));
    } else {
      areaPerUnitThickness += chordLength;
    }
    if (count > kMaxCurvedCenterLineVertices / 2 - railVertices) {
      return rejected(error, "Centerline outline exceeds its boundary vertex budget");
    }
    railVertices += count;
    steps.push_back(count);
    path.push_back(piece);
  }
  if (length(path.back().end) <= joinTolerance) {
    return rejected(error, "Centerline must be open");
  }
  if (!isSimple(path, false, stop)) {
    return rejected(error, "Centerline crosses or touches itself");
  }

  std::vector<Point> offsets;
  offsets.reserve(path.size() + 1);
  offsets.push_back(halfThickness * leftNormal(path.front().tangent(false)));
  for (size_t i = 1; i < path.size(); ++i) {
    const auto previous = path[i - 1].tangent(true);
    const auto next = path[i].tangent(false);
    const double cosine = glm::dot(previous, next);
    if (path[i - 1].radius != 0.0 || path[i].radius != 0.0) {
      if (cosine <= 0.0 || std::abs(cross(previous, next)) > 1e-8) {
        return rejected(error, "Centerline joins involving circular arcs must be tangent");
      }
      const auto tangent = path[i].radius != 0.0 ? next : previous;
      offsets.push_back(halfThickness * leftNormal(tangent));
    } else {
      if (cosine < -0.5 - kRoundoff) {
        return rejected(error, "Centerline turn exceeds the supported 120 degree miter");
      }
      offsets.push_back(leftNormal(previous + next) *
                        (halfThickness / (1.0 + cosine)));
    }
  }
  offsets.push_back(halfThickness * leftNormal(path.back().tangent(true)));

  const auto offsetPiece = [&](size_t index, double side) {
    auto piece = path[index];
    piece.start += side * offsets[index];
    piece.end += side * offsets[index + 1];
    if (piece.radius != 0.0)
      piece.radius -= std::copysign(halfThickness, piece.sweep) * side;
    return piece;
  };
  std::vector<CenterLinePiece> outline;
  outline.reserve(path.size() * 2 + 2);
  for (size_t i = 0; i < path.size(); ++i) {
    const auto right = offsetPiece(i, -1.0);
    const auto left = offsetPiece(i, 1.0);
    if (path[i].radius == 0.0) {
      const auto tangent = path[i].tangent(false);
      if (glm::dot(right.end - right.start, tangent) <= 0.0 ||
          glm::dot(left.end - left.start, tangent) <= 0.0) {
        return rejected(error, "Centerline thickness folds an offset segment at a miter");
      }
    }
    outline.push_back(right);
  }
  outline.push_back({outline.back().end, offsetPiece(path.size() - 1, 1.0).end});
  for (size_t i = path.size(); i > 0; --i)
    outline.push_back(offsetPiece(i - 1, 1.0).reversed());
  outline.push_back({outline.back().end, outline.front().start});
  if (!isSimple(outline, true, stop)) {
    return rejected(error, "Centerline offset boundary intersects or touches itself");
  }

  std::vector<Point> ring;
  ring.reserve(railVertices * 2);
  for (size_t i = 0; i < outline.size(); ++i) {
    container::util::checkModelImportCancellation(stop);
    const auto& piece = outline[i];
    ring.push_back(origin + piece.start);
    if (piece.radius == 0.0) continue;
    const size_t source = i < path.size() ? i : 2 * path.size() - i;
    const size_t count = steps[source];
    const auto radial = piece.start - piece.center;
    const auto from = (piece.radius / length(radial)) * radial;
    for (size_t j = 1; j < count; ++j) {
      const double radians = piece.sweep * (double(j) / double(count));
      ring.push_back(origin + (piece.center + rotate(from, radians)));
    }
  }
  auto result = toFloatRing(ring);
  if (result.empty()) {
    return rejected(error, "Centerline outline collapses at renderer precision");
  }
  for (size_t i = 0; i < result.size(); ++i) ring[i] = Point(result[i]) - origin;
  const double area = signedArea(ring);
  const double expectedArea = fullThickness * areaPerUnitThickness;
  if (!(area > 0.0) || !isSimple(ring, true, stop)) {
    return rejected(error, "Sampled centerline offset boundary intersects, touches, or collapses");
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
