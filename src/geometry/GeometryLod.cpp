#include "Container/geometry/GeometryLod.h"

#include "Container/utility/ModelImportCancellation.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <limits>
#include <unordered_map>

namespace container::geometry {
namespace {

constexpr size_t kMinTriangles = 96;
constexpr size_t kMaxTriangles = 131'072;
constexpr size_t kMaxValence = 32;
constexpr unsigned kRoundsPerLevel = 8;
constexpr double kCreaseCosine = 0.866;
constexpr double kMinFaceCosine = 0.5;

using Index = uint32_t;
using PositionKey = std::array<uint32_t, 3>;

struct PositionHash {
  size_t operator()(const PositionKey& key) const noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const auto value : key)
      hash = (hash ^ value) * 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

uint32_t positionBits(float value) {
  return value == 0.0f ? 0u : std::bit_cast<uint32_t>(value);
}

bool finitePosition(glm::vec3 position) {
  return std::isfinite(position.x) && std::isfinite(position.y) &&
         std::isfinite(position.z);
}

bool sameWedge(const Vertex& lhs, const Vertex& rhs,
               TriangleLodWeldPolicy weld) {
  if (lhs.color != rhs.color || lhs.colorAlpha != rhs.colorAlpha ||
      lhs.texCoord != rhs.texCoord || lhs.texCoord1 != rhs.texCoord1)
    return false;
  if (weld == TriangleLodWeldPolicy::IgnoreGeneratedTangents) {
    // Coplanar face normals from independent importer calculations can
    // differ in their final bits even when they represent the same plane.
    return glm::all(glm::lessThanEqual(glm::abs(lhs.normal - rhs.normal),
                                     glm::vec3(1e-5f)));
  }
  return lhs.normal == rhs.normal && lhs.tangent == rhs.tangent;
}

bool compatibleEndpoints(const Vertex& lhs, const Vertex& rhs,
                         TriangleLodWeldPolicy weld) {
  if (lhs.colorAlpha != rhs.colorAlpha)
    return false;
  if (weld == TriangleLodWeldPolicy::PreserveAttributes &&
      lhs.tangent.w != rhs.tangent.w)
    return false;
  return glm::dot(lhs.normal, rhs.normal) >= kCreaseCosine &&
      glm::all(glm::lessThanEqual(glm::abs(lhs.color - rhs.color), glm::vec3(0.125f))) &&
      glm::all(glm::lessThanEqual(glm::abs(lhs.texCoord - rhs.texCoord), glm::vec2(0.25f))) &&
      glm::all(glm::lessThanEqual(glm::abs(lhs.texCoord1 - rhs.texCoord1), glm::vec2(0.25f)));
}

struct Face {
  std::array<Index, 3> nodes;
  std::array<Index, 3> corners;
  glm::dvec3 originalNormal;
};

struct Node {
  glm::dvec3 position;
  Index representative{};
  bool seam{};
  bool protectedFeature{};
  double error{};
};

struct Edge {
  Index first{};
  Index second{};
  Index face{};
  bool ascending{};

  auto operator<=>(const Edge&) const = default;
};

struct Candidate {
  Index source{};
  Index target{};
  std::array<Index, 2> opposite{};
  double error{};
};

bool contains(const std::array<Index, 3>& nodes, Index vertex) {
  return std::ranges::find(nodes, vertex) != nodes.end();
}

Index oppositeVertex(const Face& face, Index first, Index second) {
  return *std::ranges::find_if(face.nodes, [&](Index vertex) {
    return vertex != first && vertex != second;
  });
}

bool degenerate(const std::array<Index, 3>& nodes) {
  return nodes[0] == nodes[1] || nodes[1] == nodes[2] ||
         nodes[2] == nodes[0];
}

glm::dvec3 faceNormal(const std::array<Index, 3>& face,
                     std::span<const Node> nodes) {
  const auto origin = nodes[face[0]].position;
  return glm::cross(nodes[face[1]].position - origin,
                    nodes[face[2]].position - origin);
}

struct Mesh {
  std::vector<Node> nodes;
  std::vector<Face> faces;
  double maxError{};
};

Mesh readMesh(std::span<const Vertex> vertices, std::span<const Index> indices,
              TriangleLodWeldPolicy weld, std::stop_token stop) {
  Mesh mesh;
  mesh.faces.reserve(indices.size() / 3);
  std::unordered_map<PositionKey, Index, PositionHash> coincident;
  coincident.reserve(indices.size());
  for (size_t first = 0; first < indices.size(); first += 3) {
    if (first % 3072 == 0)
      util::checkModelImportCancellation(stop);
    Face face{};
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto index = indices[first + corner];
      if (index >= vertices.size() || !finitePosition(vertices[index].position))
        return {};
      const auto& vertex = vertices[index];
      const auto [entry, inserted] = coincident.try_emplace(
          PositionKey{positionBits(vertex.position.x), positionBits(vertex.position.y),
                      positionBits(vertex.position.z)}, static_cast<Index>(mesh.nodes.size()));
      if (inserted) {
        mesh.nodes.push_back({glm::dvec3(vertex.position), index});
      } else {
        auto& node = mesh.nodes[entry->second];
        node.seam |= !sameWedge(vertex, vertices[node.representative], weld);
      }
      face.nodes[corner] = entry->second;
      face.corners[corner] = index;
    }
    if (degenerate(face.nodes))
      return {};
    const auto normal = faceNormal(face.nodes, mesh.nodes);
    const auto areaSquared = glm::dot(normal, normal);
    if (!(areaSquared > 0.0))
      return {};
    face.originalNormal = normal / std::sqrt(areaSquared);
    mesh.faces.push_back(face);
  }
  return mesh;
}

struct MeshLinks {
  std::vector<Edge> edges;
  std::vector<glm::dvec3> normals;
  std::vector<std::vector<Index>> incident;
  std::vector<std::vector<Index>> neighbors;
};

MeshLinks buildLinks(const Mesh& mesh, std::stop_token stop) {
  MeshLinks links;
  links.edges.reserve(mesh.faces.size() * 3);
  links.normals.reserve(mesh.faces.size());
  links.incident.resize(mesh.nodes.size());
  links.neighbors.resize(mesh.nodes.size());
  for (Index index = 0; index < mesh.faces.size(); ++index) {
    if (index % 1024 == 0)
      util::checkModelImportCancellation(stop);
    const auto& face = mesh.faces[index];
    links.normals.push_back(glm::normalize(faceNormal(face.nodes, mesh.nodes)));
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto from = face.nodes[corner];
      const auto to = face.nodes[(corner + 1) % 3];
      links.edges.push_back({(std::min)(from, to), (std::max)(from, to), index, from < to});
      links.incident[from].push_back(index);
    }
  }
  std::ranges::sort(links.edges);
  return links;
}

struct EdgeGroup {
  Index first{};
  Index second{};
  std::array<Index, 2> opposite{};
};

Index cornerIndex(const Face& face, Index node) {
  return face.corners[std::ranges::find(face.nodes, node) - face.nodes.begin()];
}

std::vector<EdgeGroup> findContinuousEdges(MeshLinks& links, const Mesh& mesh,
    std::vector<bool>& locked, std::span<const Vertex> vertices,
    TriangleLodWeldPolicy weld, std::stop_token stop) {
  std::vector<EdgeGroup> continuous;
  continuous.reserve(links.edges.size() / 2);
  size_t nextCancellationCheck = 0;
  for (size_t start = 0, end = 0; start < links.edges.size(); start = end) {
    if (start >= nextCancellationCheck) {
      util::checkModelImportCancellation(stop);
      nextCancellationCheck = start + 1024;
    }
    const auto& edge = links.edges[start];
    end = start + 1;
    while (end < links.edges.size() && links.edges[end].first == edge.first &&
           links.edges[end].second == edge.second) {
      ++end;
      if (end % 1024 == 0)
        util::checkModelImportCancellation(stop);
    }
    links.neighbors[edge.first].push_back(edge.second);
    links.neighbors[edge.second].push_back(edge.first);
    if (end - start != 2 ||
        edge.ascending == links.edges[start + 1].ascending ||
        glm::dot(links.normals[edge.face],
                 links.normals[links.edges[start + 1].face]) < kCreaseCosine) {
      // Keep both endpoints of open borders, nonmanifold edges and creases.
      locked[edge.first] = locked[edge.second] = true;
      continue;
    }
    const auto& first = mesh.faces[edge.face];
    const auto& second = mesh.faces[links.edges[start + 1].face];
    const auto differentWedges = [&](Index node) {
      return mesh.nodes[node].seam &&
          !sameWedge(vertices[cornerIndex(first, node)], vertices[cornerIndex(second, node)], weld);
    };
    if (differentWedges(edge.first) || differentWedges(edge.second)) {
      // Preserve a seam's entire edge, even when both wedges use the same
      // attributes at one endpoint.
      locked[edge.first] = locked[edge.second] = true;
      continue;
    }
    continuous.push_back({edge.first, edge.second,
                          {oppositeVertex(first, edge.first, edge.second),
                           oppositeVertex(second, edge.first, edge.second)}});
  }
  return continuous;
}

// Interior collapses preserve a single connected fan at either endpoint.
bool singleFan(Index node, const Mesh& mesh, const MeshLinks& links) {
  const auto& faces = links.incident[node];
  if (faces.size() < 3 || faces.size() > kMaxValence ||
      faces.size() != links.neighbors[node].size())
    return false;
  std::array<bool, kMaxValence> seen{};
  std::array<size_t, kMaxValence> queue{};
  size_t tail = 1;
  seen[0] = true;
  for (size_t head = 0; head < tail; ++head) {
    const auto& around = mesh.faces[faces[queue[head]]].nodes;
    for (size_t i = 0; i < faces.size(); ++i) {
      if (seen[i])
        continue;
      const auto& next = mesh.faces[faces[i]].nodes;
      if (std::ranges::any_of(around, [&](Index nextNode) {
            return nextNode != node && contains(next, nextNode);
          })) {
        seen[i] = true;
        queue[tail++] = i;
      }
    }
  }
  return tail == faces.size();
}

bool satisfiesLinkCondition(const Candidate& candidate, const Mesh& mesh,
                            const MeshLinks& links) {
  const auto [first, second] = candidate.opposite;
  if (first == second)
    return false;
  const auto& targetNeighbors = links.neighbors[candidate.target];
  // The two removed triangle opposites are exactly the common neighbors.
  for (const auto neighbor : links.neighbors[candidate.source])
    if (neighbor != first && neighbor != second &&
        std::ranges::find(targetNeighbors, neighbor) != targetNeighbors.end())
      return false;
  // A common edge in the two vertex links (e.g. a tetrahedron) would make
  // duplicate faces after collapse even though the vertex condition holds.
  const auto hasOppositeEdge = [&](Index node) {
    return std::ranges::any_of(links.incident[node], [&](Index face) {
      const auto& nodes = mesh.faces[face].nodes;
      return contains(nodes, first) && contains(nodes, second);
    });
  };
  return !hasOppositeEdge(candidate.source) || !hasOppositeEdge(candidate.target);
}

bool preservesFaceOrientation(const Candidate& candidate, const Mesh& mesh,
                              const MeshLinks& links) {
  for (const auto index : links.incident[candidate.source]) {
    auto moved = mesh.faces[index].nodes;
    if (contains(moved, candidate.target))
      continue; // One of the two disappearing edge faces.
    std::ranges::replace(moved, candidate.source, candidate.target);
    const auto changed = faceNormal(moved, mesh.nodes);
    const double lengthSquared = glm::dot(changed, changed);
    const auto minDot = kMinFaceCosine * std::sqrt(lengthSquared);
    if (!(lengthSquared > 0.0) ||
        glm::dot(changed, links.normals[index]) < minDot ||
        glm::dot(changed, mesh.faces[index].originalNormal) < minDot)
      return false;
  }
  return true;
}

std::vector<Candidate> candidatesFor(const Mesh& mesh, const MeshLinks& links,
    std::span<const EdgeGroup> edges, std::vector<bool>& locked,
    std::span<const Vertex> vertices, TriangleLodWeldPolicy weld,
    double errorLimit, std::stop_token stop) {
  for (Index index = 0; index < mesh.nodes.size(); ++index) {
    if (index % 1024 == 0)
      util::checkModelImportCancellation(stop);
    locked[index] = locked[index] || !singleFan(index, mesh, links);
  }
  std::vector<Candidate> candidates;
  candidates.reserve(edges.size());
  for (size_t index = 0; index < edges.size(); ++index) {
    if (index % 1024 == 0)
      util::checkModelImportCancellation(stop);
    const auto& edge = edges[index];
    if (locked[edge.first] || locked[edge.second])
      continue;
    const auto& first = mesh.nodes[edge.first];
    const auto& second = mesh.nodes[edge.second];
    if (!compatibleEndpoints(vertices[first.representative],
                             vertices[second.representative], weld))
      continue;
    const auto distance = glm::length(first.position - second.position);
    const auto errorToFirst = (std::max)(first.error, second.error + distance);
    const auto errorToSecond = (std::max)(second.error, first.error + distance);
    const bool preferFirst = errorToFirst <= errorToSecond;
    Candidate candidate{preferFirst ? edge.second : edge.first,
                        preferFirst ? edge.first : edge.second,
                        edge.opposite, (std::min)(errorToFirst, errorToSecond)};
    if (candidate.error <= errorLimit)
      candidates.push_back(candidate);
  }
  std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
    if (a.error != b.error) return a.error < b.error;
    if (a.target != b.target) return a.target < b.target;
    return a.source < b.source;
  });
  return candidates;
}

bool simplifyRound(Mesh& mesh, std::span<const Vertex> vertices,
                   TriangleLodWeldPolicy weld, size_t targetCount,
                   double errorLimit, std::stop_token stop) {
  auto links = buildLinks(mesh, stop);
  std::vector<bool> locked(mesh.nodes.size());
  for (size_t index = 0; index < mesh.nodes.size(); ++index) {
    const auto& node = mesh.nodes[index];
    locked[index] = node.seam || node.protectedFeature;
  }
  const auto edges = findContinuousEdges(links, mesh, locked, vertices, weld, stop);
  for (size_t index = 0; index < mesh.nodes.size(); ++index)
    mesh.nodes[index].protectedFeature = locked[index];
  const auto candidates = candidatesFor(mesh, links, edges, locked, vertices, weld,
                                        errorLimit, stop);
  constexpr Index unchanged = (std::numeric_limits<Index>::max)();
  std::vector<Index> remap(mesh.nodes.size(), unchanged);
  std::vector<bool> busy(mesh.nodes.size());
  size_t selected = 0;
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (index % 1024 == 0)
      util::checkModelImportCancellation(stop);
    const auto& candidate = candidates[index];
    if (busy[candidate.source] || busy[candidate.target] ||
        !satisfiesLinkCondition(candidate, mesh, links) ||
        !preservesFaceOrientation(candidate, mesh, links))
      continue;
    remap[candidate.source] = candidate.target;
    mesh.nodes[candidate.target].error = candidate.error;
    mesh.maxError = (std::max)(mesh.maxError, candidate.error);
    // No face or endpoint can participate in two simultaneous collapses.
    for (const auto endpoint : {candidate.source, candidate.target}) {
      busy[endpoint] = true;
      for (const auto neighbor : links.neighbors[endpoint])
        busy[neighbor] = true;
    }
    if (mesh.faces.size() - 2 * ++selected <= targetCount)
      break;
  }
  if (selected == 0)
    return false;
  size_t output = 0;
  for (auto face : mesh.faces) {
    for (size_t corner = 0; corner < 3; ++corner) {
      const auto target = remap[face.nodes[corner]];
      if (target == unchanged)
        continue;
      face.nodes[corner] = target;
      face.corners[corner] = mesh.nodes[target].representative;
    }
    if (!degenerate(face.nodes))
      mesh.faces[output++] = face;
  }
  mesh.faces.resize(output);
  return true;
}

} // namespace

std::vector<TriangleLod> buildTriangleLods(
    std::span<const Vertex> vertices, std::span<const uint32_t> indices,
    std::stop_token stop, TriangleLodWeldPolicy weld) {
  util::checkModelImportCancellation(stop);
  const auto baseCount = indices.size() / 3;
  if (indices.size() % 3 || baseCount < kMinTriangles || baseCount > kMaxTriangles)
    return {};
  auto mesh = readMesh(vertices, indices, weld, stop);
  if (mesh.faces.empty())
    return {};
  auto minimum = mesh.nodes.front().position;
  auto maximum = minimum;
  for (const auto& node : mesh.nodes) {
    minimum = glm::min(minimum, node.position);
    maximum = glm::max(maximum, node.position);
  }
  const auto errorLimit = glm::length(maximum - minimum) / 8.0;
  std::vector<TriangleLod> levels;
  size_t previousCount = baseCount;
  for (const auto divisor : {2u, 4u}) {
    const auto target = baseCount / divisor;
    for (unsigned round = 0; round < kRoundsPerLevel && mesh.faces.size() > target; ++round)
      if (!simplifyRound(mesh, vertices, weld, target, errorLimit, stop))
        break;
    // A larger index footprint is worthwhile only for a real reduction.
    if (mesh.faces.empty() || mesh.faces.size() * 5 > previousCount * 4)
      break;
    TriangleLod level;
    level.indices.reserve(mesh.faces.size() * 3);
    for (const auto& face : mesh.faces)
      level.indices.insert(level.indices.end(), face.corners.begin(), face.corners.end());
    level.maxError = std::nextafter(static_cast<float>(mesh.maxError),
                                    (std::numeric_limits<float>::infinity)());
    if (!std::isfinite(level.maxError))
      break;
    levels.push_back(std::move(level));
    previousCount = mesh.faces.size();
  }
  util::checkModelImportCancellation(stop);
  return levels;
}

} // namespace container::geometry
