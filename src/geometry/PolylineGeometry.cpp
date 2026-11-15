#include "Container/geometry/PolylineGeometry.h"
#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

#include <glm/geometric.hpp>

namespace container::geometry {

bool appendPolylineGeometry(dotbim::Model &model,
                            std::span<const glm::vec3> points,
                            std::span<const size_t> pathVertexCounts,
                            uint32_t meshId, float pickingRadius,
                            std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  if (points.size() < 2 || pathVertexCounts.empty() ||
      !std::isfinite(pickingRadius) || pickingRadius <= 0)
    return false;
  const auto finite = [](const glm::vec3 &p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  };
  for (size_t i = 0; i < points.size(); ++i) {
    if ((i % 1024u) == 0u)
      container::util::checkModelImportCancellation(stop);
    if (!finite(points[i]))
      return false;
  }

  struct Segment {
    glm::vec3 a, b;
    std::array<glm::vec3, 4> start, end;
  };
  std::vector<Segment> segments;
  size_t cursor = 0;
  glm::dvec3 minimum(std::numeric_limits<double>::max()), maximum(-minimum);
  for (size_t count : pathVertexCounts) {
    container::util::checkModelImportCancellation(stop);
    if (count == 0 || count > points.size() - cursor)
      return false;
    for (size_t i = 1; i < count; ++i) {
      if ((i % 1024u) == 0u)
        container::util::checkModelImportCancellation(stop);
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
  for (size_t index = 0; index < segments.size(); ++index) {
    if ((index % 1024u) == 0u)
      container::util::checkModelImportCancellation(stop);
    const auto &s = segments[index];
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
  for (size_t index = 0; index < segments.size(); ++index) {
    if ((index % 1024u) == 0u)
      container::util::checkModelImportCancellation(stop);
    const auto &s = segments[index];
    for (const auto &p : {s.a, s.b}) {
      Vertex vertex{};
      vertex.position = p;
      model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
      model.vertices.push_back(vertex);
    }
  }
  lines.indexCount =
      static_cast<uint32_t>(model.indices.size()) - lines.firstIndex;
  model.nativeCurveRanges.push_back(lines);
  return true;
}

bool appendMetricPolylineGeometry(dotbim::Model &model,
                                  std::span<const glm::vec3> points,
                                  std::span<const size_t> pathVertexCounts,
                                  std::span<const glm::vec3> dots,
                                  uint32_t meshId, double width,
                                  std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  const double radius = width * .5;
  if (!std::isfinite(radius) || radius <= 0)
    return false;
  const auto finite = [](glm::vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  };
  if (!std::ranges::all_of(points, finite) ||
      !std::ranges::all_of(dots, finite))
    return false;

  constexpr uint32_t sides = 16;
  constexpr uint32_t latitudes = 8;
  static const auto circle = [] {
    std::array<glm::dvec2, sides> samples;
    for (uint32_t i = 0; i < sides; ++i) {
      const double angle = 2 * std::numbers::pi * i / sides;
      samples[i] = {std::cos(angle), std::sin(angle)};
    }
    return samples;
  }();
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<glm::vec3> lineEnds;
  constexpr size_t entryLimit = std::numeric_limits<uint32_t>::max();
  // A style can multiply source points into tubes and round joins. Limit
  // that temporary mesh before adding it to the model's 32-bit buffers.
  constexpr size_t maxPoints = 131072;
  if (points.size() > maxPoints || dots.size() > maxPoints - points.size())
    return false;
  const auto withinStrokeBudget = [&] {
    return vertices.size() <= 1'048'576 && indices.size() <= 6'291'456;
  };
  glm::dvec3 low(std::numeric_limits<double>::max()), high(-low);
  bool valid = true;
  const auto vertex = [&](glm::dvec3 position, glm::dvec3 normal) {
    Vertex out{};
    out.position = glm::vec3(position);
    out.normal = glm::vec3(normal);
    valid &= finite(out.position);
    low = glm::min(low, glm::dvec3(out.position));
    high = glm::max(high, glm::dvec3(out.position));
    vertices.push_back(out);
    return static_cast<uint32_t>(vertices.size() - 1);
  };
  const auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
    const glm::dvec3 x(vertices[a].position), y(vertices[b].position),
        z(vertices[c].position);
    const auto normal = glm::cross(y - x, z - x);
    valid &= glm::dot(normal, normal) > 0;
    indices.insert(indices.end(), {a, b, c});
  };
  const auto join = [&](glm::dvec3 center) {
    const auto top = vertex(center + glm::dvec3(0, radius, 0), {0, 1, 0});
    const auto bottom = vertex(center - glm::dvec3(0, radius, 0), {0, -1, 0});
    const uint32_t first = bottom + 1;
    for (uint32_t row = 1; row < latitudes; ++row) {
      const double angle = std::numbers::pi * row / latitudes;
      const double horizontal = std::sin(angle), vertical = std::cos(angle);
      for (auto sample : circle) {
        const glm::dvec3 normal(horizontal * sample.x, vertical,
                                horizontal * sample.y);
        vertex(center + radius * normal, normal);
      }
    }
    for (uint32_t i = 0; i < sides; ++i) {
      const auto next = (i + 1) % sides;
      triangle(top, first + next, first + i);
      for (uint32_t row = 0; row + 2 < latitudes; ++row) {
        const uint32_t a = first + row * sides, b = a + sides;
        triangle(a + i, a + next, b + next);
        triangle(a + i, b + next, b + i);
      }
      const uint32_t last = first + (latitudes - 2) * sides;
      triangle(bottom, last + i, last + next);
    }
  };
  const auto segment = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 direction) {
    const glm::dvec3 axis =
        std::abs(direction.y) < .9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const auto side = glm::normalize(glm::cross(axis, direction));
    const auto up = glm::cross(direction, side);
    const auto start = vertex(a, -direction), end = vertex(b, direction);
    const uint32_t first = end + 1, second = first + sides;
    for (auto center : {a, b})
      for (auto sample : circle) {
        const auto normal = sample.x * side + sample.y * up;
        vertex(center + radius * normal, normal);
      }
    for (uint32_t i = 0; i < sides; ++i) {
      const auto next = (i + 1) % sides;
      triangle(first + i, first + next, second + next);
      triangle(first + i, second + next, second + i);
      triangle(start, first + next, first + i);
      triangle(end, second + i, second + next);
    }
  };
  size_t cursor = 0;
  for (size_t count : pathVertexCounts) {
    if (count == 0 || count > points.size() - cursor)
      return false;
    std::optional<glm::dvec3> previous;
    for (size_t i = 1; i < count; ++i) {
      if (((cursor + i) % 1024) == 1)
        container::util::checkModelImportCancellation(stop);
      const glm::dvec3 a(points[cursor + i - 1]), b(points[cursor + i]);
      if (a == b)
        continue;
      const auto direction = glm::normalize(b - a);
      if (previous && glm::dot(*previous, direction) < .999999)
        join(a);
      segment(a, b, direction);
      if (!valid || !withinStrokeBudget())
        return false;
      previous = direction;
      lineEnds.push_back(glm::vec3(a));
      lineEnds.push_back(glm::vec3(b));
    }
    cursor += count;
  }
  for (size_t i = 0; i < dots.size(); ++i) {
    if (i % 1024 == 0)
      container::util::checkModelImportCancellation(stop);
    join(dots[i]);
    if (!valid || !withinStrokeBudget())
      return false;
  }
  if (cursor != points.size() || indices.empty() ||
      model.vertices.size() + vertices.size() + lineEnds.size() > entryLimit ||
      model.indices.size() + indices.size() + lineEnds.size() > entryLimit)
    return false;
  const glm::vec3 center((low + high) * .5);
  const float bound = static_cast<float>(glm::length(
      glm::max(high - glm::dvec3(center), glm::dvec3(center) - low)));
  if (!std::isfinite(bound))
    return false;

  const uint32_t vertexBase = static_cast<uint32_t>(model.vertices.size());
  const uint32_t firstIndex = static_cast<uint32_t>(model.indices.size());
  for (uint32_t i : indices)
    model.indices.push_back(vertexBase + i);
  model.vertices.insert(model.vertices.end(), vertices.begin(), vertices.end());
  model.meshRanges.push_back({meshId, firstIndex,
                              static_cast<uint32_t>(indices.size()), center,
                              bound});
  if (!lineEnds.empty()) {
    const auto firstLine = static_cast<uint32_t>(model.indices.size());
    for (auto point : lineEnds) {
      Vertex out{};
      out.position = point;
      model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
      model.vertices.push_back(out);
    }
    model.nativeCurveRanges.push_back({meshId, firstLine,
                                       static_cast<uint32_t>(lineEnds.size()),
                                       center, bound});
  }
  return true;
}

} // namespace container::geometry
