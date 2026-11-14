#include "Container/renderer/bim/BimHardClash.h"

#include <glm/geometric.hpp>
#include <manifold/manifold.h>
#if __has_include(<manifold/version.h>)
#include <manifold/version.h>
#if MANIFOLD_VERSION >= MANIFOLD_VERSION_NUMBER(3, 5, 4)
#define CONTAINER_MANIFOLD_EXECUTION_CONTEXT 1
#endif
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace container::renderer {
namespace {

constexpr uint8_t kGroups = kBimHardClashGroupA | kBimHardClashGroupB;
using ObjectPair = std::array<uint32_t, 2>;

struct Bounds {
  glm::dvec3 min{std::numeric_limits<double>::infinity()};
  glm::dvec3 max{-std::numeric_limits<double>::infinity()};

  void Include(const glm::dvec3& point) {
    for (int axis = 0; axis < 3; ++axis) {
      min[axis] = std::min(min[axis], point[axis]);
      max[axis] = std::max(max[axis], point[axis]);
    }
  }

  [[nodiscard]] glm::dvec3 Center() const {
    return {std::midpoint(min.x, max.x), std::midpoint(min.y, max.y),
            std::midpoint(min.z, max.z)};
  }
};

struct Solid {
  uint32_t objectIndex;
  uint8_t groups;
  Bounds bounds;
  glm::dvec3 origin;
  manifold::Manifold mesh;
};

// The vcpkg baseline's Manifold 3.0 checks cancellation between operations.
// Newer Manifold builds can also observe the token during import and CSG.
class BooleanOperations {
public:
  explicit BooleanOperations([[maybe_unused]] std::stop_token stop)
#ifdef CONTAINER_MANIFOLD_EXECUTION_CONTEXT
      : cancel_(stop, [this] { context_.Cancel(); })
#endif
  {}

  manifold::Manifold FromMesh(const manifold::MeshGL64& mesh) {
#ifdef CONTAINER_MANIFOLD_EXECUTION_CONTEXT
    return context_.FromMeshGL(mesh);
#else
    return manifold::Manifold(mesh);
#endif
  }

  manifold::Manifold Intersection(const manifold::Manifold& left,
                                  const manifold::Manifold& right) {
    auto intersection = left ^ right;
#ifdef CONTAINER_MANIFOLD_EXECUTION_CONTEXT
    return intersection.WithContext(context_);
#else
    return intersection;
#endif
  }

private:
#ifdef CONTAINER_MANIFOLD_EXECUTION_CONTEXT
  manifold::ExecutionContext context_;
  std::stop_callback<std::function<void()>> cancel_;
#endif
};

struct PositionHash {
  size_t operator()(const std::array<double, 3>& position) const noexcept {
    size_t seed = 0;
    for (double coordinate : position) {
      const size_t hash = std::hash<double>{}(coordinate);
      seed ^= hash + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

bool IsFinite(const glm::dvec3& point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool HasPositiveExtent(const Bounds& bounds) {
  return bounds.min.x < bounds.max.x && bounds.min.y < bounds.max.y &&
         bounds.min.z < bounds.max.z;
}

Bounds Overlap(const Bounds& first, const Bounds& second) {
  Bounds overlap;
  for (int axis = 0; axis < 3; ++axis) {
    overlap.min[axis] = std::max(first.min[axis], second.min[axis]);
    overlap.max[axis] = std::min(first.max[axis], second.max[axis]);
  }
  return overlap;
}

manifold::vec3 ToManifold(const glm::dvec3& translation) {
  return {translation.x, translation.y, translation.z};
}

std::string BuildSolid(const BimHardClashElement& element, std::stop_token stop,
                       BooleanOperations& booleans, Solid& solid) {
  if (element.triangles.size() < 4) {
    return "Too few triangles for a closed solid";
  }

  // Vertex splits for normals and materials do not open a wall. Only weld
  // exactly equal positions so gaps in the input surface stay visible.
  manifold::MeshGL64 mesh;
  mesh.numProp = 3;
  mesh.triVerts.reserve(element.triangles.size() * 3);
  const size_t vertexCount = std::min(element.vertices.size(), mesh.triVerts.capacity());
  mesh.vertProperties.reserve(vertexCount * 3);
  std::unordered_map<std::array<double, 3>, uint64_t, PositionHash> positions;
  positions.reserve(vertexCount);
  for (size_t i = 0; i < element.triangles.size(); ++i) {
    if (i % 256 == 0 && stop.stop_requested()) {
      return {};
    }
    const auto& triangle = element.triangles[i];
    for (uint32_t vertexIndex : triangle) {
      if (vertexIndex >= element.vertices.size()) {
        return "Triangle references a missing vertex";
      }
      const glm::dvec3& vertex = element.vertices[vertexIndex];
      if (!IsFinite(vertex)) {
        return "Triangle contains a non-finite vertex";
      }
      const auto [position, inserted] =
          positions.try_emplace({vertex.x, vertex.y, vertex.z}, positions.size());
      if (inserted) {
        solid.bounds.Include(vertex);
        mesh.vertProperties.insert(mesh.vertProperties.end(), {vertex.x, vertex.y, vertex.z});
      }
      mesh.triVerts.push_back(position->second);
    }
    const glm::dvec3& a = element.vertices[triangle[0]];
    const glm::dvec3& b = element.vertices[triangle[1]];
    const glm::dvec3& c = element.vertices[triangle[2]];
    const auto normal = glm::cross(b - a, c - a);
    if (!IsFinite(normal) || normal == glm::dvec3{0.0}) {
      return "Geometry contains a degenerate triangle";
    }
  }

  if (!HasPositiveExtent(solid.bounds)) {
    return "Geometry has no three-dimensional bounds";
  }
  solid.origin = solid.bounds.Center();
  for (size_t i = 0; i < mesh.vertProperties.size(); i += 3) {
    mesh.vertProperties[i] -= solid.origin.x;
    mesh.vertProperties[i + 1] -= solid.origin.y;
    mesh.vertProperties[i + 2] -= solid.origin.z;
  }

  solid.mesh = booleans.FromMesh(mesh);
  auto status = solid.mesh.Status();
  if (status == manifold::Manifold::Error::NotManifold) {
    return "Open or non-manifold triangle surface";
  }
  if (status != manifold::Manifold::Error::NoError) {
    return "Manifold rejected the solid geometry (status " +
           std::to_string(static_cast<int>(status)) + ")";
  }
  const double volume = solid.mesh.Volume();
  if (!(volume > 0.0) || !std::isfinite(volume)) {
    return "Geometry does not enclose a finite positive volume with outward winding";
  }
  return {};
}

void CheckPair(const Solid& first, const Solid& second, const glm::dvec3& origin,
               BooleanOperations& booleans, BimHardClashResult& result) {
  const ObjectPair pair{std::min(first.objectIndex, second.objectIndex),
                        std::max(first.objectIndex, second.objectIndex)};
  // Rebase again at the overlap so the Boolean does not use a distant world
  // origin to decide floating point tolerances.
  const auto left = first.mesh.Translate(ToManifold(first.origin - origin));
  const auto right = second.mesh.Translate(ToManifold(second.origin - origin));
  const auto intersection = booleans.Intersection(left, right);
  if (intersection.Status() != manifold::Manifold::Error::NoError) {
    result.failedPairs.push_back(pair);
    return;
  }
  if (intersection.IsEmpty()) {
    ++result.checkedPairs;
    return;
  }
  const double volume = intersection.Volume();
  if (!std::isfinite(volume) || volume < 0.0) {
    result.failedPairs.push_back(pair);
    return;
  }
  if (volume > 0.0) {
    const auto bounds = intersection.BoundingBox();
    const glm::dvec3 position =
        origin + glm::dvec3{std::midpoint(bounds.min[0], bounds.max[0]),
                            std::midpoint(bounds.min[1], bounds.max[1]),
                            std::midpoint(bounds.min[2], bounds.max[2])};
    if (!IsFinite(position)) {
      result.failedPairs.push_back(pair);
      return;
    }
    Bounds pairBounds = first.bounds;
    pairBounds.Include(second.bounds.min);
    pairBounds.Include(second.bounds.max);
    result.hits.push_back({pair[0], pair[1], volume, position, pairBounds.min, pairBounds.max});
  }
  ++result.checkedPairs;
}

} // namespace

BimHardClashResult FindBimHardClashes(std::span<const BimHardClashElement> elements,
                                      std::stop_token stop, const BimHardClashLimits& limits) {
  if (stop.stop_requested()) {
    return {.cancelled = true};
  }
  if (elements.size() > limits.maxElements) {
    throw std::length_error("Hard clash element limit exceeded");
  }
  std::vector<const BimHardClashElement*> selected;
  selected.reserve(elements.size());
  for (const auto& element : elements) {
    if (element.groups & kGroups) {
      selected.push_back(&element);
    }
  }
  std::ranges::sort(selected, {}, &BimHardClashElement::objectIndex);
  if (std::ranges::adjacent_find(selected, {}, &BimHardClashElement::objectIndex) != selected.end()) {
    throw std::invalid_argument("Hard clash snapshot contains duplicate object IDs");
  }

  BimHardClashResult result;
  BooleanOperations booleanOperations(stop);
  std::vector<Solid> solids;
  solids.reserve(selected.size());
  size_t triangles = 0;
  for (const auto* element : selected) {
    if (stop.stop_requested()) {
      return {.cancelled = true};
    }
    if (!element->unavailableReason.empty()) {
      result.skipped.push_back({element->objectIndex, element->unavailableReason});
      continue;
    }
    const size_t elementTriangles = element->triangles.size();
    if (element->vertices.size() > limits.maxVerticesPerElement ||
        elementTriangles > limits.maxTrianglesPerElement ||
        elementTriangles > limits.maxTriangles - triangles) {
      result.skipped.push_back({element->objectIndex, "Geometry exceeds hard clash size limits"});
      continue;
    }
    triangles += elementTriangles;
    Solid solid{.objectIndex = element->objectIndex,
                .groups = static_cast<uint8_t>(element->groups & kGroups)};
    auto reason = BuildSolid(*element, stop, booleanOperations, solid);
    if (stop.stop_requested()) {
      return {.cancelled = true};
    }
    if (reason.empty()) {
      solids.push_back(std::move(solid));
    } else {
      result.skipped.push_back({element->objectIndex, std::move(reason)});
    }
  }

  std::ranges::sort(solids, [](const Solid& left, const Solid& right) {
    return std::tie(left.bounds.min.x, left.objectIndex) <
           std::tie(right.bounds.min.x, right.objectIndex);
  });

  // Keep the sweep in one order, but visit only the opposite group's entries
  // for single-group elements. A dual-group element visits either group once.
  std::array<std::vector<size_t>, kGroups + 1> indicesByGroup;
  for (size_t i = 0; i < solids.size(); ++i) {
    indicesByGroup[kGroups].push_back(i);
    if (solids[i].groups & kBimHardClashGroupA) {
      indicesByGroup[kBimHardClashGroupA].push_back(i);
    }
    if (solids[i].groups & kBimHardClashGroupB) {
      indicesByGroup[kBimHardClashGroupB].push_back(i);
    }
  }

  size_t candidates = 0;
  size_t booleans = 0;
  for (size_t i = 0; i < solids.size() && !result.pairLimitReached; ++i) {
    if (stop.stop_requested()) {
      return {.cancelled = true};
    }
    const auto& first = solids[i];
    const auto otherGroup = first.groups == kGroups ? kGroups : kGroups ^ first.groups;
    const auto& otherIndices = indicesByGroup[otherGroup];
    for (auto j = std::ranges::upper_bound(otherIndices, i); j != otherIndices.end(); ++j) {
      const auto& second = solids[*j];
      if (second.bounds.min.x >= first.bounds.max.x) {
        break;
      }
      if (stop.stop_requested()) {
        return {.cancelled = true};
      }
      if (candidates == limits.maxCandidateTests) {
        result.pairLimitReached = true;
        break;
      }
      ++candidates;
      const auto overlap = Overlap(first.bounds, second.bounds);
      if (!HasPositiveExtent(overlap)) {
        continue;
      }
      if (booleans == limits.maxBooleanPairs) {
        result.pairLimitReached = true;
        break;
      }
      ++booleans;
      CheckPair(first, second, overlap.Center(), booleanOperations, result);
    }
  }
  if (stop.stop_requested()) {
    return {.cancelled = true};
  }
  std::ranges::sort(result.hits, [](const BimHardClashHit& left, const BimHardClashHit& right) {
    return std::tie(left.firstObjectIndex, left.secondObjectIndex) <
           std::tie(right.firstObjectIndex, right.secondObjectIndex);
  });
  std::ranges::sort(result.failedPairs);
  return result;
}

} // namespace container::renderer
