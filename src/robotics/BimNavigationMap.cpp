#include "Container/robotics/BimNavigationMap.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
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

using RepresentationKey = std::pair<std::string_view, std::string_view>;

struct SupportSelection {
  double bottom{std::numeric_limits<double>::infinity()};
  double top{-std::numeric_limits<double>::infinity()};
  std::map<RepresentationKey, std::vector<uint32_t>> triangles;
};

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

std::vector<glm::dvec3> clipBand(const Triangle& triangle, double bottom, double top) {
  auto clipped = clip<glm::dvec3>(triangle, [bottom](auto point) { return point.z - bottom; });
  return clip<glm::dvec3>(clipped, [top](auto point) { return top - point.z; });
}

Polygon project(std::span<const glm::dvec3> points) {
  Polygon result;
  result.reserve(points.size());
  for (const auto point : points)
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

struct MeshComponent {
  uint32_t parent{};
  uint32_t size{1};
  bool closed{true};
};

uint32_t rootOf(std::vector<MeshComponent>& components, uint32_t face) {
  while (components[face].parent != face) {
    components[face].parent = components[components[face].parent].parent;
    face = components[face].parent;
  }
  return face;
}

std::vector<MeshComponent> meshComponents(const BimCollisionMesh& mesh) {
  struct Edge {
    std::array<uint32_t, 2> vertices;
    uint32_t face;
  };
  std::vector<MeshComponent> components(mesh.indices.size() / 3);
  std::vector<Edge> edges;
  edges.reserve(mesh.indices.size());
  for (uint32_t face = 0; face < components.size(); ++face) {
    components[face].parent = face;
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto first = mesh.indices[3 * size_t(face) + corner];
      const auto second = mesh.indices[3 * size_t(face) + (corner + 1) % 3];
      edges.push_back({{std::min(first, second), std::max(first, second)}, face});
    }
  }
  std::ranges::sort(edges, {}, &Edge::vertices);
  for (size_t first = 0; first < edges.size();) {
    size_t end = first + 1;
    auto root = rootOf(components, edges[first].face);
    while (end < edges.size() && edges[end].vertices == edges[first].vertices) {
      auto adjacent = rootOf(components, edges[end++].face);
      if (adjacent == root)
        continue;
      if (components[root].size < components[adjacent].size)
        std::swap(root, adjacent);
      components[adjacent].parent = root;
      components[root].size += components[adjacent].size;
      components[root].closed &= components[adjacent].closed;
    }
    // Cooked vertices are welded by exact local position. A boundary,
    // degenerate, or non-manifold edge invalidates only its own component.
    components[root].closed &= end - first == 2 && edges[first].vertices[0] != edges[first].vertices[1];
    first = end;
  }
  return components;
}

bool intersectSection(Segment& section, const Triangle& triangle, double height) {
  size_t points = 0;
  auto previous = triangle.back();
  for (const auto point : triangle) {
    if ((point.z > height) != (previous.z > height)) {
      // Adjacent faces may traverse a shared edge in opposite directions.
      // Interpolate from the same endpoint so their section vertices coincide.
      const auto& low = point.z < previous.z ? point : previous;
      const auto& high = point.z < previous.z ? previous : point;
      const auto t = (height - low.z) / (high.z - low.z);
      section[points++] = {std::lerp(low.x, high.x, t), std::lerp(low.y, high.y, t)};
    }
    previous = point;
  }
  return points == 2 && section[0] != section[1];
}

struct SectionCrossing {
  uint32_t row;
  uint32_t component;
  double x;
};

void rasterizeClosedSections(Grid& grid, const BimCollisionMesh& mesh, glm::dvec3 offset, double sectionHeight) {
  grid.budget.use(mesh.indices.size());
  auto components = meshComponents(mesh);
  std::vector<SectionCrossing> crossings;
  for (uint32_t face = 0; face < components.size(); ++face) {
    const auto component = rootOf(components, face);
    if (!components[component].closed)
      continue;
    const auto index = 3 * size_t(face);
    const Triangle triangle{offset + glm::dvec3(mesh.vertices[mesh.indices[index]]),
                            offset + glm::dvec3(mesh.vertices[mesh.indices[index + 1]]),
                            offset + glm::dvec3(mesh.vertices[mesh.indices[index + 2]])};
    Segment section;
    if (!intersectSection(section, triangle, sectionHeight))
      continue;
    auto [start, finish] = section;
    if (start.y > finish.y)
      std::swap(start, finish);
    const auto rows = cellsBetween(start.y, finish.y, grid.pitch, grid.settings.height);
    grid.budget.use(rows.end - rows.first);
    for (uint32_t y = rows.first; y < rows.end; ++y) {
      const auto center = (double(y) + 0.5) * grid.pitch;
      // Half-open spans count shared vertices exactly once.
      if (center < start.y || center >= finish.y)
        continue;
      if (crossings.size() == maxSectionCrossings)
        throw std::length_error("BIM navigation map exceeds its cross-section limit");
      const auto t = (center - start.y) / (finish.y - start.y);
      crossings.push_back({y, component, std::lerp(start.x, finish.x, t)});
    }
  }
  // Pair crossings separately for each shell and each scanline, then mark
  // their union. Pooling intersecting shells before pairing cancels overlap.
  std::ranges::sort(crossings, [](const auto& first, const auto& second) {
    if (first.row != second.row) return first.row < second.row;
    if (first.component != second.component) return first.component < second.component;
    return first.x < second.x;
  });
  for (size_t first = 0; first < crossings.size();) {
    const auto start = first;
    const auto y = crossings[first].row;
    const auto component = crossings[first].component;
    do {
      ++first;
    } while (first < crossings.size() && crossings[first].row == y && crossings[first].component == component);
    if ((first - start) % 2 != 0)
      throw std::runtime_error("BIM navigation map closed collider shell has an odd crossing count at row " +
                               std::to_string(y) + " (component rooted at triangle " + std::to_string(component) + ")");
    for (size_t i = start; i < first; i += 2) {
      const auto left = crossings[i].x;
      const auto right = crossings[i + 1].x;
      if (right - left <= grid.tolerance)
        continue;
      const auto columns = cellsBetween(left, right, grid.pitch, grid.settings.width);
      grid.budget.use(columns.end - columns.first);
      for (uint32_t x = columns.first; x < columns.end; ++x) {
        const auto center = (double(x) + 0.5) * grid.pitch;
        if (left <= center && center <= right)
          grid.values[grid.index(x, y)] = BimNavigationCell::Occupied;
      }
    }
  }
}

void rasterizeObstacles(Grid& grid, const CookedBimCollision& geometry,
                        std::span<const std::string> openDoorIds, glm::dvec3 origin,
                        const SupportSelection& support) {
  if (geometry.colliders.size() > BimCollisionCookOptions{}.maxColliders ||
      openDoorIds.size() > geometry.colliders.size())
    throw std::length_error("BIM navigation map exceeds its collider or open door limit");
  std::unordered_map<std::string_view, bool> foundDoors;
  for (const auto& id : openDoorIds)
    if (!foundDoors.emplace(id, false).second)
      throw std::invalid_argument("BIM navigation map received duplicate open door IDs");
  const auto height = grid.settings.robotHeightMeters;
  const auto bottom = std::isfinite(support.bottom) ? support.bottom : capsuleSkin;
  const auto top = std::isfinite(support.top) ? support.top : height + capsuleSkin;
  const auto floorBand = grid.settings.floorToleranceMeters + grid.tolerance;
  // Slice above the highest selected support, inside the corresponding
  // capsule. Projected faces cover obstacles which start or end in the band.
  const auto sectionHeight = top - height * 0.5;
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
    std::span<const uint32_t> selectedTriangles;
    if (collider.supportCandidate) {
      const auto found = support.triangles.find({collider.entityId, collider.representationId});
      if (found != support.triangles.end()) selectedTriangles = found->second;
    }
    bool hasSection = false;
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
      const auto ordinal = static_cast<uint32_t>(i / 3);
      // Exclude only the part of a selected support triangle in the floor
      // band. A ramp continuing above this floor can still block clearance.
      const auto clipBottom = std::ranges::binary_search(selectedTriangles, ordinal)
          ? std::max(bottom - grid.tolerance, floorBand + grid.tolerance) : bottom - grid.tolerance;
      if (highest < clipBottom || lowest > top + grid.tolerance)
        continue;
      markObstacle(grid, project(clipBand(triangle, clipBottom, top + grid.tolerance)));
      hasSection |= lowest <= sectionHeight && highest > sectionHeight;
    }
    if (hasSection)
      rasterizeClosedSections(grid, *mesh, local, sectionHeight);
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

SupportSelection rasterizeSupport(Grid& grid, std::span<const BimWalkableSurface> support, glm::dvec3 origin) {
  if (support.size() > maxTriangleReferences)
    throw std::length_error("BIM navigation map exceeds its support triangle limit");
  const auto band = grid.settings.floorToleranceMeters + grid.tolerance;
  const BimWalkabilitySettings agent{.radius = grid.settings.robotRadiusMeters,
                                     .height = grid.settings.robotHeightMeters};
  const auto extent = Point(grid.settings.width, grid.settings.height) * grid.pitch;
  SupportSelection selected;
  size_t storedFragments = 0;
  std::unordered_map<size_t, std::vector<Polygon>> partial;
  for (const auto& surface : support) {
    Triangle triangle;
    for (size_t i = 0; i < triangle.size(); ++i) {
      triangle[i] = surface.vertices[i] - origin;
      if (!finite(triangle[i]))
        throw std::invalid_argument("BIM navigation map support triangle has a nonfinite vertex");
    }
    const auto clipped = clipBand(triangle, -band, band);
    auto polygon = project(clipped);
    const auto area = signedArea(polygon);
    if (!std::isfinite(area))
      throw std::invalid_argument("BIM navigation map support triangle exceeds the geometric range");
    if (std::abs(area) <= grid.tolerance * grid.tolerance)
      continue;
    const auto box = bounds(polygon);
    if (box.maximum.x < 0 || box.maximum.y < 0 || box.minimum.x > extent.x || box.minimum.y > extent.y)
      continue;

    const auto normalLength = std::hypot(surface.normal.x, surface.normal.y, surface.normal.z);
    if (!std::isfinite(normalLength) || surface.normal.z <= 0)
      throw std::invalid_argument("BIM navigation map support triangle must have a finite upward normal");
    const auto normalUp = surface.normal.z / normalLength;
    for (const auto point : clipped) {
      const auto capsule = walkabilityCapsuleBottom({0, 0, point.z}, normalUp, agent);
      const auto top = capsule.z + agent.height;
      if (!std::isfinite(capsule.z) || !std::isfinite(top))
        throw std::invalid_argument("BIM navigation map support triangle exceeds the capsule height range");
      selected.bottom = std::min(selected.bottom, capsule.z);
      selected.top = std::max(selected.top, top);
    }
    selected.triangles[{surface.entityId, surface.representationId}].push_back(surface.triangleIndex);
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
  for (auto& entry : selected.triangles)
    std::ranges::sort(entry.second);
  return selected;
}

} // namespace

std::vector<BimNavigationCell> rasterizeBimNavigationMap(
    const CookedBimCollision& geometry, std::span<const BimWalkableSurface> support,
    std::span<const std::string> openDoorIds, const BimNavigationMapSettings& settings) {
  validate(settings);
  Grid grid{settings};
  const glm::dvec3 origin(settings.originMeters, settings.floorMeters);
  const auto selected = rasterizeSupport(grid, support, origin);
  rasterizeObstacles(grid, geometry, openDoorIds, origin, selected);
  return std::move(grid.values);
}

} // namespace container::robotics
