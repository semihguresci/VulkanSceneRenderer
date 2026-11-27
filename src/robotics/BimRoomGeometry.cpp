#include "BimRoomGeometry.h"

#include "Container/geometry/DotBimLoader.h"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace container::robotics::room_geometry {
namespace {

using Triangle = std::array<glm::dvec3, 3>;
constexpr size_t maxTriangles = 1'000'000;
constexpr size_t maxRepresentations = 16'384;
constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Bounds emptyBounds() {
  return {glm::dvec3(std::numeric_limits<double>::infinity()),
          glm::dvec3(-std::numeric_limits<double>::infinity())};
}

void include(Bounds& bounds, glm::dvec3 point) {
  bounds.minimum = glm::min(bounds.minimum, point);
  bounds.maximum = glm::max(bounds.maximum, point);
}

bool validBounds(const Bounds& bounds) {
  return finite(bounds.minimum) && finite(bounds.maximum) &&
         glm::all(glm::lessThanEqual(bounds.minimum, bounds.maximum));
}

std::optional<bool> mirrored(const glm::dmat4& transform) {
  glm::dmat3 normalized(1);
  for (glm::length_t column = 0; column < 4; ++column) {
    if (!finite(glm::dvec3(transform[column])) ||
        transform[column].w != (column == 3 ? 1.0 : 0.0))
      return std::nullopt;
    if (column < 3) {
      const auto value = glm::dvec3(transform[column]);
      const double length = std::hypot(value.x, value.y, value.z);
      if (!std::isfinite(length) || length == 0)
        return std::nullopt;
      normalized[column] = value / length;
    }
  }
  const double determinant = glm::determinant(normalized);
  if (!std::isfinite(determinant) || determinant == 0)
    return std::nullopt;
  return determinant < 0;
}

struct Boundary {
  Bounds bounds{emptyBounds()};
  std::vector<size_t> faces;
};

std::optional<Boundary> closedBoundary(const Volume& volume) {
  if (!validBounds(volume.bounds) || volume.triangles.empty() ||
      volume.triangles.size() > maxTriangles)
    return std::nullopt;
  std::map<std::array<double, 3>, uint32_t> vertices;
  std::set<std::array<uint32_t, 3>> faces;
  struct Edge { unsigned count{}; int orientation{}; };
  std::map<std::pair<uint32_t, uint32_t>, Edge> edges;
  Boundary boundary;
  for (size_t triangleIndex = 0; triangleIndex < volume.triangles.size(); ++triangleIndex) {
    const auto& triangle = volume.triangles[triangleIndex];
    std::array<uint32_t, 3> ids;
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto point = triangle[corner];
      if (!finite(point))
        return std::nullopt;
      const auto entry = vertices.try_emplace(
          std::array{point.x, point.y, point.z}, static_cast<uint32_t>(vertices.size())).first;
      ids[corner] = entry->second;
    }
    const auto normal = glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
    const double area = std::hypot(normal.x, normal.y, normal.z);
    if (!std::isfinite(area))
      return std::nullopt;
    if (area == 0)
      continue;
    auto key = ids;
    std::sort(key.begin(), key.end());
    if (!faces.insert(key).second)
      continue;
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto first = ids[corner], second = ids[(corner + 1) % 3];
      auto& edge = edges[{std::min(first, second), std::max(first, second)}];
      if (++edge.count > 2)
        return std::nullopt;
      edge.orientation += first < second ? 1 : -1;
      include(boundary.bounds, triangle[corner]);
    }
    boundary.faces.push_back(triangleIndex);
  }
  if (boundary.faces.empty())
    return std::nullopt;
  for (const auto& [key, edge] : edges)
    if (edge.count != 2 || edge.orientation != 0)
      return std::nullopt;
  return boundary;
}

bool lineMeetsBounds(const Bounds& bounds, glm::dvec3 origin, glm::dvec3 direction) {
  double first = -std::numeric_limits<double>::infinity();
  double last = std::numeric_limits<double>::infinity();
  for (glm::length_t axis = 0; axis < 3; ++axis) {
    if (direction[axis] == 0) {
      if (origin[axis] < bounds.minimum[axis] || origin[axis] > bounds.maximum[axis])
        return false;
    } else {
      double near = (bounds.minimum[axis] - origin[axis]) / direction[axis];
      double far = (bounds.maximum[axis] - origin[axis]) / direction[axis];
      if (near > far)
        std::swap(near, far);
      first = std::max(first, near);
      last = std::min(last, far);
      if (first > last)
        return false;
    }
  }
  return true;
}

struct Crossing {
  double distance;
  glm::dvec3 normal;
  bool onEdge;
};

} // namespace

std::optional<Volume> loadVolume(const geometry::dotbim::Model& model,
                                  const SemanticEntity& entity) {
  if (entity.representations.size() > maxRepresentations)
    return std::nullopt;
  std::map<uint32_t, const geometry::dotbim::MeshRange*> ranges;
  for (const auto& representation : entity.representations)
    if (representation.geometryKind == SemanticGeometryKind::Mesh)
      ranges.try_emplace(representation.meshId, nullptr);
  if (ranges.empty())
    return std::nullopt;
  std::set<uint32_t> seenRanges;
  for (const auto& range : model.meshRanges) {
    if (const auto entry = ranges.find(range.meshId); entry != ranges.end())
      entry->second = seenRanges.insert(range.meshId).second ? &range : nullptr;
  }
  Volume result{emptyBounds(), {}};
  size_t triangleReferences = 0;
  for (const auto& representation : entity.representations) {
    if (representation.geometryKind != SemanticGeometryKind::Mesh)
      continue;
    const auto found = ranges.find(representation.meshId);
    const auto* range = found == ranges.end() ? nullptr : found->second;
    if (!range || !representation.roboticsWorldFromLocal || !range->indexCount ||
        range->indexCount % 3 != 0 || range->firstIndex > model.indices.size() ||
        range->indexCount > model.indices.size() - range->firstIndex)
      return std::nullopt;
    const size_t count = range->indexCount / 3;
    if (count > maxTriangles - triangleReferences)
      return std::nullopt;
    triangleReferences += count;
    const auto& transform = *representation.roboticsWorldFromLocal;
    const auto reflected = mirrored(transform);
    if (!reflected)
      return std::nullopt;
    for (size_t offset = 0; offset < range->indexCount; offset += 3) {
      Triangle triangle;
      for (size_t corner = 0; corner < 3; ++corner) {
        const auto index = model.indices[size_t(range->firstIndex) + offset + corner];
        if (index >= model.vertices.size())
          return std::nullopt;
        triangle[corner] = glm::dvec3(transform * glm::dvec4(model.vertices[index].position, 1));
        if (!finite(triangle[corner]))
          return std::nullopt;
      }
      const auto normal = glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
      const double area = std::hypot(normal.x, normal.y, normal.z);
      if (!std::isfinite(area))
        return std::nullopt;
      if (area == 0)
        continue;
      if (*reflected)
        std::swap(triangle[1], triangle[2]);
      for (const auto point : triangle)
        include(result.bounds, point);
      result.triangles.push_back(triangle);
    }
  }
  if (result.triangles.empty() || !validBounds(result.bounds))
    return std::nullopt;
  if (auto boundary = closedBoundary(result)) {
    result.usableInterior = true;
    result.interiorFaceIndices = std::move(boundary->faces);
  }
  return result;
}

std::vector<std::pair<double, double>> interiorIntervals(
    const Volume& volume, glm::dvec3 origin, glm::dvec3 direction) {
  const double directionLength = std::hypot(direction.x, direction.y, direction.z);
  if (!finite(origin) || !finite(direction) || !std::isfinite(directionLength) ||
      std::abs(directionLength - 1) > 1e-8 || std::abs(direction.z) > 1e-12)
    throw std::invalid_argument("Room probe requires a finite normalized horizontal direction");
  direction = glm::normalize(glm::dvec3(direction.x, direction.y, 0));
  if (!volume.usableInterior || !validBounds(volume.bounds) ||
      volume.interiorFaceIndices.empty() || volume.interiorFaceIndices.size() > maxTriangles ||
      !lineMeetsBounds(volume.bounds, origin, direction))
    return {};
  const auto magnitude = glm::max(glm::abs(volume.bounds.minimum), glm::abs(volume.bounds.maximum));
  const double scale = std::max({1.0, magnitude.x, magnitude.y, magnitude.z,
                                std::abs(origin.x), std::abs(origin.y), std::abs(origin.z)});
  const double distanceTolerance = roundoff * scale;
  if (origin.z <= volume.bounds.minimum.z + distanceTolerance ||
      origin.z >= volume.bounds.maximum.z - distanceTolerance)
    return {}; // A line on the upper/lower boundary has no certified interior.
  std::vector<Crossing> crossings;
  for (const size_t faceIndex : volume.interiorFaceIndices) {
    if (faceIndex >= volume.triangles.size())
      return {};
    const auto& triangle = volume.triangles[faceIndex];
    if (!finite(triangle[0]) || !finite(triangle[1]) || !finite(triangle[2]))
      return {};
    const auto first = triangle[1] - triangle[0], second = triangle[2] - triangle[0];
    const auto normal = glm::cross(first, second);
    const double area = std::hypot(normal.x, normal.y, normal.z);
    if (!std::isfinite(area) || area == 0)
      return {};
    const auto p = glm::cross(direction, second);
    const double determinant = glm::dot(first, p);
    if (std::abs(determinant) <= area * roundoff) {
      if (std::abs(glm::dot(normal / area, origin - triangle[0])) <= distanceTolerance) {
        Bounds faceBounds = emptyBounds();
        for (const auto point : triangle)
          include(faceBounds, point);
        if (lineMeetsBounds(faceBounds, origin, direction))
          return {}; // Coplanar boundary/tangent queries are ambiguous.
      }
      continue;
    }
    const auto from = origin - triangle[0];
    const double u = glm::dot(from, p) / determinant;
    const auto q = glm::cross(from, first);
    const double v = glm::dot(direction, q) / determinant;
    // Strict membership deliberately favours missing an ambiguous edge over
    // manufacturing interior immediately outside a triangle's boundary.
    if (u < 0 || v < 0 || u + v > 1)
      continue;
    const double distance = glm::dot(second, q) / determinant;
    if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(distance))
      return {};
    crossings.push_back({distance, normal / area,
                         u <= roundoff || v <= roundoff || 1 - u - v <= roundoff});
  }
  std::sort(crossings.begin(), crossings.end(), [](const auto& a, const auto& b) {
    return a.distance < b.distance;
  });
  std::vector<double> events;
  std::vector<int> orientations;
  for (size_t begin = 0; begin < crossings.size();) {
    size_t end = begin + 1;
    while (end < crossings.size() && crossings[end].distance - crossings[begin].distance <= distanceTolerance)
      ++end;
    const auto& reference = crossings[begin];
    const int orientation = glm::dot(reference.normal, direction) < 0 ? -1 : 1;
    bool onEdge = false;
    for (size_t index = begin; index < end; ++index) {
      onEdge |= crossings[index].onEdge;
      if (glm::dot(crossings[index].normal, direction) * orientation <= 0 ||
          glm::dot(crossings[index].normal, reference.normal) < 1 - 1e-8)
        return {}; // Nonplanar edge/corner or opposing tangent crossings.
    }
    if (onEdge && end - begin == 1)
      return {};
    events.push_back(reference.distance);
    orientations.push_back(orientation);
    begin = end;
  }
  if (events.size() % 2 != 0)
    return {};
  for (size_t index = 1; index < orientations.size(); ++index)
    if (orientations[index] == orientations[index - 1])
      return {}; // Overlapping/inconsistently oriented shells are unresolved.
  std::vector<std::pair<double, double>> result;
  for (size_t index = 0; index < events.size(); index += 2) {
    const double first = events[index] + distanceTolerance;
    const double last = events[index + 1] - distanceTolerance;
    if (first < last)
      result.emplace_back(first, last);
  }
  return result;
}

} // namespace container::robotics::room_geometry
