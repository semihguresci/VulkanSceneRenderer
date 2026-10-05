#include "Container/geometry/PolylineGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>

namespace container::geometry {

bool appendPolylineGeometry(dotbim::Model &model,
                            std::span<const glm::vec3> points,
                            std::span<const size_t> pathVertexCounts,
                            uint32_t meshId, float pickingRadius) {
  if (points.size() < 2 || pathVertexCounts.empty() ||
      !std::isfinite(pickingRadius) || pickingRadius <= 0)
    return false;
  const auto finite = [](const glm::vec3 &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  };
  for (const auto &p : points)
    if (!finite(p))
      return false;

  struct Segment {
    glm::vec3 a, b;
    std::array<glm::vec3, 4> start, end;
  };
  std::vector<Segment> segments;
  size_t cursor = 0;
  glm::dvec3 minimum(std::numeric_limits<double>::max()), maximum(-minimum);
  for (size_t count : pathVertexCounts) {
    if (count == 0 || count > points.size() - cursor)
      return false;
    for (size_t i = 1; i < count; ++i) {
      const auto a = points[cursor + i - 1], b = points[cursor + i];
      const auto delta = glm::dvec3(b) - glm::dvec3(a);
      if (glm::dot(delta, delta) == 0)
        continue;
      const auto forward = glm::normalize(delta);
      const glm::dvec3 axis =
          std::abs(forward.y) < .9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
      const auto side = glm::normalize(glm::cross(axis, forward)) *
                        double(pickingRadius),
                 up = glm::normalize(glm::cross(forward, side)) *
                      double(pickingRadius);
      const std::array<glm::dvec3, 4> offsets{side + up, -side + up, -side - up,
                                              side - up};
      Segment segment{a, b};
      for (size_t j = 0; j < 4; ++j) {
        segment.start[j] = glm::vec3(glm::dvec3(a) + offsets[j]);
        segment.end[j] = glm::vec3(glm::dvec3(b) + offsets[j]);
        if (!finite(segment.start[j]) || !finite(segment.end[j]))
          return false;
      }
      segments.push_back(segment);
      minimum = glm::min(minimum, glm::min(glm::dvec3(a), glm::dvec3(b)));
      maximum = glm::max(maximum, glm::max(glm::dvec3(a), glm::dvec3(b)));
    }
    cursor += count;
  }
  if (cursor != points.size() || segments.empty())
    return false;
  const glm::vec3 center((minimum + maximum) * .5);
  const float radius =
      static_cast<float>(glm::length(maximum - glm::dvec3(center)));
  const float proxyRadius = radius + pickingRadius * std::sqrt(2.0f);
  if (!finite(center) || !std::isfinite(proxyRadius))
    return false;
  // Each segment has 12 proxy triangles and two native endpoints.
  constexpr size_t entriesPerSegment = 38;
  const size_t limit = std::numeric_limits<uint32_t>::max();
  if (model.vertices.size() > limit || model.indices.size() > limit ||
      segments.size() >
          (limit - std::max(model.vertices.size(), model.indices.size())) /
              entriesPerSegment)
    return false;

  auto triangle = [&](const glm::vec3 &a, const glm::vec3 &b,
                      const glm::vec3 &c) {
    const auto cross = glm::cross(glm::dvec3(b) - glm::dvec3(a),
                                  glm::dvec3(c) - glm::dvec3(a));
    const auto normal = glm::dot(cross, cross) > 0
                            ? glm::vec3(glm::normalize(cross))
                            : glm::vec3(0, 1, 0);
    for (const auto &p : {a, b, c}) {
      Vertex vertex{};
      vertex.position = p;
      vertex.normal = normal;
      model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
      model.vertices.push_back(vertex);
    }
  };
  auto quad = [&](const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c,
                  const glm::vec3 &d) {
    triangle(a, b, c);
    triangle(a, c, d);
  };
  dotbim::MeshRange proxy{meshId, static_cast<uint32_t>(model.indices.size()),
                          0, center, proxyRadius};
  for (const auto &s : segments) {
    for (size_t i = 0; i < 4; ++i)
      quad(s.start[i], s.end[i], s.end[(i + 1) % 4], s.start[(i + 1) % 4]);
    quad(s.start[0], s.start[1], s.start[2], s.start[3]);
    quad(s.end[3], s.end[2], s.end[1], s.end[0]);
  }
  proxy.indexCount =
      static_cast<uint32_t>(model.indices.size()) - proxy.firstIndex;
  model.meshRanges.push_back(proxy);
  dotbim::NativePrimitiveRange lines{
      meshId, static_cast<uint32_t>(model.indices.size()), 0, center, radius};
  for (const auto &s : segments)
    for (const auto &p : {s.a, s.b}) {
      Vertex vertex{};
      vertex.position = p;
      model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
      model.vertices.push_back(vertex);
    }
  lines.indexCount =
      static_cast<uint32_t>(model.indices.size()) - lines.firstIndex;
  model.nativeCurveRanges.push_back(lines);
  return true;
}

} // namespace container::geometry
