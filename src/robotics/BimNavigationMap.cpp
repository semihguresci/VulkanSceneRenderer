#include "Container/robotics/BimNavigationMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace container::robotics {
namespace {

using Point = glm::dvec2;
using Polygon = std::vector<Point>;
using Segment = std::array<Point, 2>;
using Triangle = std::array<glm::dvec3, 3>;

constexpr uint32_t maxAxisCells = 4096;
constexpr uint64_t maxCells = 4'194'304;
constexpr size_t maxTriangleReferences = 4'000'000;
constexpr size_t maxWork = 50'000'000;
constexpr size_t maxSectionCrossings = 1'000'000;
constexpr size_t maxStoredFragments = 262'144;
constexpr size_t maxFragmentsPerCell = 64;
constexpr double minimumResolution = 1e-5;
constexpr double capsuleSkin = BimWalkabilitySettings{}.skin;

struct Bounds {
  Point minimum;
  Point maximum;
};

struct CellRange {
  uint32_t first{};
  uint32_t end{};
};

struct WorkBudget {
  size_t remaining{maxWork};

  void use(size_t amount) {
    if (amount > remaining)
      throw std::length_error("BIM navigation map exceeds its raster work budget");
    remaining -= amount;
  }
};

bool finite(Point point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

bool finite(glm::dvec3 point) {
  return finite(Point(point)) && std::isfinite(point.z);
}

void validate(const BimNavigationMapSettings& settings) {
  if (settings.width == 0 || settings.height == 0 || settings.width > maxAxisCells ||
      settings.height > maxAxisCells || uint64_t(settings.width) * settings.height > maxCells)
    throw std::invalid_argument("BIM navigation map requires at most 4096 cells per axis and 4194304 cells total");
  if (!std::isfinite(settings.resolutionMeters) || settings.resolutionMeters < minimumResolution)
    throw std::invalid_argument("BIM navigation map resolution must be finite and at least 0.00001 metres");
  const auto extent = Point(settings.width, settings.height) * settings.resolutionMeters;
  const auto maximum = settings.originMeters + extent;
  if (!finite(settings.originMeters) || !finite(extent) || !finite(maximum) ||
      settings.originMeters.x + settings.resolutionMeters <= settings.originMeters.x ||
      settings.originMeters.y + settings.resolutionMeters <= settings.originMeters.y ||
      maximum.x - settings.resolutionMeters >= maximum.x || maximum.y - settings.resolutionMeters >= maximum.y ||
      !std::isfinite(glm::dot(extent, extent)))
    throw std::invalid_argument("BIM navigation map extents must be finite with distinguishable cell coordinates");
  if (!std::isfinite(settings.floorMeters) || !std::isfinite(settings.floorToleranceMeters) ||
      settings.floorToleranceMeters < 0 || !std::isfinite(settings.floorMeters - settings.floorToleranceMeters) ||
      !std::isfinite(settings.floorMeters + settings.robotHeightMeters) ||
      !std::isfinite(settings.robotRadiusMeters) || settings.robotRadiusMeters < 0 ||
      !std::isfinite(settings.robotRadiusMeters * settings.robotRadiusMeters) ||
      !std::isfinite(settings.robotHeightMeters) || settings.robotHeightMeters < 2 * settings.robotRadiusMeters ||
      settings.robotHeightMeters <= settings.floorToleranceMeters + capsuleSkin)
    throw std::invalid_argument("Invalid BIM navigation map floor band or robot dimensions");
}

Bounds bounds(std::span<const Point> polygon) {
  Bounds result{polygon.front(), polygon.front()};
  for (const auto point : polygon) {
    result.minimum = glm::min(result.minimum, point);
    result.maximum = glm::max(result.maximum, point);
  }
  return result;
}

CellRange cellsBetween(double minimum, double maximum, double pitch, uint32_t count) {
  if (maximum < 0 || minimum >= count * pitch)
    return {};
  const auto clippedIndex = [&](double value) {
    return std::clamp(std::floor(value / pitch), 0.0, double(count));
  };
  return {static_cast<uint32_t>(clippedIndex(minimum)),
          static_cast<uint32_t>(std::min(clippedIndex(maximum) + 1.0, double(count)))};
}

struct Grid {
  const BimNavigationMapSettings& settings;
  const double pitch{settings.resolutionMeters};
  const double tolerance{std::clamp(pitch * 1e-5, 1e-8, 1e-5)};
  std::vector<BimNavigationCell> values{
      size_t(settings.width) * settings.height, BimNavigationCell::Unknown};
  WorkBudget budget;

  size_t index(uint32_t x, uint32_t y) const { return size_t(y) * settings.width + x; }

  Bounds cell(uint32_t x, uint32_t y) const {
    return {Point(x, y) * pitch, Point(x + 1, y + 1) * pitch};
  }

  template<class Visitor>
  void visit(Bounds area, Visitor visitor) {
    const auto columns = cellsBetween(area.minimum.x, area.maximum.x, pitch, settings.width);
    const auto rows = cellsBetween(area.minimum.y, area.maximum.y, pitch, settings.height);
    if (columns.first == columns.end || rows.first == rows.end)
      return;
    budget.use(size_t(columns.end - columns.first) * (rows.end - rows.first));
    for (uint32_t y = rows.first; y < rows.end; ++y)
      for (uint32_t x = columns.first; x < columns.end; ++x)
        visitor(index(x, y), cell(x, y));
  }
};

double cross(Point a, Point b) {
  return a.x * b.y - a.y * b.x;
}

double signedArea(std::span<const Point> polygon) {
  double twiceArea = 0;
  for (size_t i = 2; i < polygon.size(); ++i)
    twiceArea += cross(polygon[i - 1] - polygon.front(), polygon[i] - polygon.front());
  return twiceArea * 0.5;
}

template<class Vector, class Distance>
std::vector<Vector> clip(std::span<const Vector> polygon, Distance distance) {
  if (polygon.empty())
    return {};
  std::vector<Vector> result;
  result.reserve(polygon.size() + 1);
  auto previous = polygon.back();
  auto from = distance(previous);
  for (const auto current : polygon) {
    const auto to = distance(current);
    if ((from < 0 && to > 0) || (from > 0 && to < 0))
      result.push_back(previous + (current - previous) * (from / (from - to)));
    if (to >= 0)
      result.push_back(current);
    previous = current;
    from = to;
  }
  return result;
}

Polygon projectBand(const Triangle& triangle, double bottom, double top) {
  auto clipped = clip<glm::dvec3>(triangle, [bottom](auto point) { return point.z - bottom; });
  clipped = clip<glm::dvec3>(clipped, [top](auto point) { return top - point.z; });
  Polygon result;
  result.reserve(clipped.size());
  for (const auto point : clipped)
    result.emplace_back(point);
  return result;
}

std::array<Point, 4> corners(const Bounds& rectangle) {
  return {rectangle.minimum, Point(rectangle.maximum.x, rectangle.minimum.y), rectangle.maximum,
          Point(rectangle.minimum.x, rectangle.maximum.y)};
}

double distanceSquared(Point point, const Bounds& rectangle) {
  const auto offset = point - glm::clamp(point, rectangle.minimum, rectangle.maximum);
  return glm::dot(offset, offset);
}

double distanceSquared(Point point, Point first, Point second) {
  const auto edge = second - first;
  const double lengthSquared = glm::dot(edge, edge);
  if (lengthSquared > 0)
    first += edge * std::clamp(glm::dot(point - first, edge) / lengthSquared, 0.0, 1.0);
  const auto offset = point - first;
  return glm::dot(offset, offset);
}

bool segmentMeetsSquare(Point start, Point finish, const Bounds& square) {
  double entry = 0;
  double exit = 1;
  for (glm::length_t axis = 0; axis < 2; ++axis) {
    const auto step = finish[axis] - start[axis];
    if (step == 0) {
      if (start[axis] < square.minimum[axis] || start[axis] > square.maximum[axis])
        return false;
      continue;
    }
    const auto first = (square.minimum[axis] - start[axis]) / step;
    const auto second = (square.maximum[axis] - start[axis]) / step;
    entry = std::max(entry, std::min(first, second));
    exit = std::min(exit, std::max(first, second));
    if (entry > exit)
      return false;
  }
  return true;
}

bool withinRadius(const Polygon& polygon, double area, const Bounds& square, double squaredRadius) {
  const auto squareCorners = corners(square);
  bool containsSquareCorner = area != 0;
  const auto orientation = area > 0 ? 1.0 : -1.0;
  auto previous = polygon.back();
  for (const auto point : polygon) {
    if (distanceSquared(point, square) <= squaredRadius || segmentMeetsSquare(previous, point, square))
      return true;
    if (std::ranges::any_of(squareCorners, [&](Point corner) {
          return distanceSquared(corner, previous, point) <= squaredRadius;
        }))
      return true;
    containsSquareCorner &= cross(point - previous, squareCorners.front() - previous) * orientation >= 0;
    previous = point;
  }
  // If the square is enclosed without crossing an edge, any corner suffices.
  return containsSquareCorner;
}

void markObstacle(Grid& grid, const Polygon& polygon) {
  if (polygon.empty())
    return;
  const auto area = signedArea(polygon);
  const auto radius = grid.settings.robotRadiusMeters + grid.tolerance;
  const auto box = bounds(polygon);
  grid.visit({box.minimum - radius, box.maximum + radius}, [&](size_t index, const Bounds& square) {
    if (grid.values[index] != BimNavigationCell::Occupied && withinRadius(polygon, area, square, radius * radius))
      grid.values[index] = BimNavigationCell::Occupied;
  });
}

void markBox(Grid& grid, glm::dvec3 center, glm::dvec3 halfExtents, double bottom, double top) {
  if (!finite(halfExtents) || halfExtents.x <= 0 || halfExtents.y <= 0 || halfExtents.z <= 0)
    throw std::invalid_argument("BIM navigation map encountered an invalid dynamic collider box");
  if (center.z + halfExtents.z < bottom || center.z - halfExtents.z > top)
    return;
  const auto minimum = Point(center - halfExtents);
  const auto maximum = Point(center + halfExtents);
  const auto footprint = corners({minimum, maximum});
  markObstacle(grid, Polygon(footprint.begin(), footprint.end()));
}

bool isClosed(const BimCollisionMesh& mesh) {
  // The cooker welds exactly equal local vertices. Two incident triangles
  // per undirected edge are required before a cross-section can fill space.
  std::vector<uint64_t> edges;
  edges.reserve(mesh.indices.size());
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto first = mesh.indices[i + corner];
      const auto second = mesh.indices[i + (corner + 1) % 3];
      if (first == second)
        return false;
      edges.push_back((uint64_t(std::min(first, second)) << 32) | std::max(first, second));
    }
  }
  if (edges.size() % 2 != 0)
    return false;
  std::ranges::sort(edges);
  for (size_t i = 0; i < edges.size(); i += 2)
    if (edges[i] != edges[i + 1] || (i > 0 && edges[i] == edges[i - 1]))
      return false;
  return true;
}

void intersectSection(std::vector<Segment>& segments, const Triangle& triangle, double height) {
  Segment section;
  size_t points = 0;
  auto previous = triangle.back();
  for (const auto point : triangle) {
    if ((point.z > height) != (previous.z > height)) {
      const double t = (height - previous.z) / (point.z - previous.z);
      section[points++] = Point(previous + (point - previous) * t);
    }
    previous = point;
  }
  if (points == 2 && section[0] != section[1])
    segments.push_back(section);
}

void fillSection(Grid& grid, std::span<const Segment> segments) {
  std::vector<std::vector<double>> crossings(grid.settings.height);
  size_t count = 0;
  for (auto [start, finish] : segments) {
    if (start.y > finish.y)
      std::swap(start, finish);
    const auto rows = cellsBetween(start.y, finish.y, grid.pitch, grid.settings.height);
    grid.budget.use(rows.end - rows.first);
    for (uint32_t y = rows.first; y < rows.end; ++y) {
      const auto center = (double(y) + 0.5) * grid.pitch;
      // Half-open spans count shared vertices exactly once.
      if (center < start.y || center >= finish.y)
        continue;
      if (count++ == maxSectionCrossings)
        throw std::length_error("BIM navigation map exceeds its cross-section limit");
      const auto t = (center - start.y) / (finish.y - start.y);
      crossings[y].push_back(std::lerp(start.x, finish.x, t));
    }
  }
  for (uint32_t y = 0; y < grid.settings.height; ++y) {
    auto& row = crossings[y];
    if (row.size() % 2 != 0)
      continue;
    std::ranges::sort(row);
    for (size_t i = 0; i < row.size(); i += 2) {
      if (row[i + 1] - row[i] <= grid.tolerance)
        continue;
      const auto columns = cellsBetween(row[i], row[i + 1], grid.pitch, grid.settings.width);
      grid.budget.use(columns.end - columns.first);
      for (uint32_t x = columns.first; x < columns.end; ++x) {
        const auto center = (double(x) + 0.5) * grid.pitch;
        if (row[i] <= center && center <= row[i + 1])
          grid.values[grid.index(x, y)] = BimNavigationCell::Occupied;
      }
    }
  }
}

void rasterizeObstacles(Grid& grid, const CookedBimCollision& geometry,
                        std::span<const std::string> openDoorIds, glm::dvec3 origin) {
  if (geometry.colliders.size() > BimCollisionCookOptions{}.maxColliders ||
      openDoorIds.size() > geometry.colliders.size())
    throw std::length_error("BIM navigation map exceeds its collider or open door limit");
  std::unordered_map<std::string_view, bool> foundDoors;
  for (const auto& id : openDoorIds)
    if (!foundDoors.emplace(id, false).second)
      throw std::invalid_argument("BIM navigation map received duplicate open door IDs");
  const auto bottom = grid.settings.floorToleranceMeters + capsuleSkin;
  const auto top = grid.settings.robotHeightMeters;
  const auto sectionHeight = bottom + (top - bottom) * 0.5;
  size_t triangles = 0;
  for (const auto& collider : geometry.colliders) {
    if (collider.kind == BimColliderKind::DoorMesh) {
      if (const auto door = foundDoors.find(collider.entityId); door != foundDoors.end()) {
        door->second = true;
        continue;
      }
    }
    const auto local = collider.originMeters - origin;
    if (!finite(local))
      throw std::invalid_argument("BIM navigation map encountered a nonfinite collider origin");
    if (collider.kind == BimColliderKind::DynamicBox) {
      markBox(grid, local, glm::dvec3(collider.halfExtents), bottom, top);
      continue;
    }
    const auto* mesh = collider.mesh.get();
    if (!mesh || mesh->indices.empty() || mesh->indices.size() % 3 != 0)
      throw std::invalid_argument("BIM navigation map encountered an invalid collider mesh");
    if (mesh->indices.size() / 3 > maxTriangleReferences - triangles)
      throw std::length_error("BIM navigation map exceeds its collider triangle limit");
    triangles += mesh->indices.size() / 3;
    std::vector<Segment> sections;
    for (size_t i = 0; i < mesh->indices.size(); i += 3) {
      Triangle triangle;
      for (size_t corner = 0; corner < 3; ++corner) {
        const auto index = mesh->indices[i + corner];
        if (index >= mesh->vertices.size())
          throw std::invalid_argument("BIM navigation map collider triangle references a missing vertex");
        triangle[corner] = local + glm::dvec3(mesh->vertices[index]);
        if (!finite(triangle[corner]))
          throw std::invalid_argument("BIM navigation map collider triangle has a nonfinite vertex");
      }
      const auto lowest = std::min({triangle[0].z, triangle[1].z, triangle[2].z});
      const auto highest = std::max({triangle[0].z, triangle[1].z, triangle[2].z});
      if (highest < bottom - grid.tolerance || lowest > top + grid.tolerance)
        continue;
      markObstacle(grid, projectBand(triangle, bottom - grid.tolerance, top + grid.tolerance));
      intersectSection(sections, triangle, sectionHeight);
      if (sections.size() > maxSectionCrossings)
        throw std::length_error("BIM navigation map exceeds its cross-section limit");
    }
    if (!sections.empty() && isClosed(*mesh))
      fillSection(grid, sections);
  }
  for (const auto& id : openDoorIds)
    if (!foundDoors.at(id))
      throw std::invalid_argument("BIM navigation map has no authored door collider for " + id);
}

struct SupportEdge {
  Point start;
  Point direction;

  double distance(Point point, double tolerance) const {
    return cross(direction, point - start) + tolerance;
  }
};

enum class Coverage { Outside, Partial, Full };

Coverage coverage(std::span<const Point> corners, std::span<const SupportEdge> edges, double tolerance) {
  auto result = Coverage::Full;
  for (const auto& edge : edges) {
    size_t inside = 0;
    for (const auto point : corners)
      inside += edge.distance(point, tolerance) >= 0;
    if (inside == 0)
      return Coverage::Outside;
    if (inside != corners.size())
      result = Coverage::Partial;
  }
  return result;
}

void subtract(std::vector<Polygon>& result, Polygon remainder, std::span<const SupportEdge> edges, Grid& grid) {
  // Peel off the part outside each half-plane. The part inside every edge is
  // supported, while the disjoint convex pieces form the remaining footprint.
  for (const auto& edge : edges) {
    if (remainder.empty())
      break;
    grid.budget.use(remainder.size() * 2);
    const auto distance = [&](Point point) { return edge.distance(point, grid.tolerance); };
    auto outside = clip<Point>(remainder, [&](Point point) { return -distance(point); });
    if (signedArea(outside) > 0) {
      if (result.size() == maxFragmentsPerCell || outside.size() > maxFragmentsPerCell)
        throw std::length_error("BIM navigation map exceeds the per-cell support complexity limit");
      result.push_back(std::move(outside));
    }
    remainder = clip<Point>(remainder, distance);
  }
}

void rasterizeSupport(Grid& grid, std::span<const BimWalkableSurface> support, glm::dvec3 origin) {
  if (support.size() > maxTriangleReferences)
    throw std::length_error("BIM navigation map exceeds its support triangle limit");
  const auto band = grid.settings.floorToleranceMeters + grid.tolerance;
  size_t storedFragments = 0;
  std::unordered_map<size_t, std::vector<Polygon>> partial;
  for (const auto& surface : support) {
    Triangle triangle;
    for (size_t i = 0; i < triangle.size(); ++i) {
      triangle[i] = surface.vertices[i] - origin;
      if (!finite(triangle[i]))
        throw std::invalid_argument("BIM navigation map support triangle has a nonfinite vertex");
    }
    auto polygon = projectBand(triangle, -band, band);
    const auto area = signedArea(polygon);
    if (!std::isfinite(area))
      throw std::invalid_argument("BIM navigation map support triangle exceeds the geometric range");
    if (std::abs(area) <= grid.tolerance * grid.tolerance)
      continue;
    if (area < 0)
      std::ranges::reverse(polygon);
    std::vector<SupportEdge> edges;
    auto previous = polygon.back();
    for (const auto point : polygon) {
      const auto length = glm::length(point - previous);
      if (length > 0)
        edges.push_back({previous, (point - previous) / length});
      previous = point;
    }
    const auto box = bounds(polygon);
    grid.visit({box.minimum - grid.tolerance, box.maximum + grid.tolerance}, [&](size_t index, const Bounds& square) {
      if (grid.values[index] != BimNavigationCell::Unknown)
        return;
      const auto squareCorners = corners(square);
      const auto covered = coverage(squareCorners, edges, grid.tolerance);
      if (covered == Coverage::Outside)
        return;
      auto entry = partial.find(index);
      std::vector<Polygon> uncovered;
      if (covered == Coverage::Partial) {
        if (entry == partial.end()) {
          subtract(uncovered, Polygon(squareCorners.begin(), squareCorners.end()), edges, grid);
        } else {
          for (auto& fragment : entry->second)
            subtract(uncovered, std::move(fragment), edges, grid);
        }
      }
      if (entry != partial.end()) {
        storedFragments -= entry->second.size();
        partial.erase(entry);
      }
      if (uncovered.empty()) {
        grid.values[index] = BimNavigationCell::Free;
        return;
      }
      if (uncovered.size() > maxStoredFragments - storedFragments)
        throw std::length_error("BIM navigation map exceeds its support fragment budget");
      storedFragments += uncovered.size();
      partial.emplace(index, std::move(uncovered));
    });
  }
}

} // namespace

std::vector<BimNavigationCell> rasterizeBimNavigationMap(
    const CookedBimCollision& geometry, std::span<const BimWalkableSurface> support,
    std::span<const std::string> openDoorIds, const BimNavigationMapSettings& settings) {
  validate(settings);
  Grid grid{settings};
  const glm::dvec3 origin(settings.originMeters, settings.floorMeters);
  rasterizeObstacles(grid, geometry, openDoorIds, origin);
  rasterizeSupport(grid, support, origin);
  return std::move(grid.values);
}

} // namespace container::robotics
