#include "IfcBrepFaceGeometry.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <glm/geometric.hpp>
#include <map>
#include <mapbox/earcut.hpp>
#include <numbers>
#include <set>
#include <stdexcept>

namespace container::geometry::ifc::detail {
namespace {
using UV = glm::dvec2;
using Point = glm::dvec3;
using Edge = std::pair<uint32_t, uint32_t>;
using Triangle = std::array<uint32_t, 3>;
Edge edge(uint32_t a, uint32_t b) { return std::minmax(a, b); }
double cross(UV a, UV b) { return a.x * b.y - a.y * b.x; }
double orient(UV a, UV b, UV c) { return cross(b - a, c - a); }
double segmentDistanceSquared(Point p, Point a, Point b) {
  const auto d = b - a;
  const double n = glm::dot(d, d);
  const auto r =
      p - a - d * (n > 0 ? std::clamp(glm::dot(p - a, d) / n, 0., 1.) : 0);
  return glm::dot(r, r);
}
double triangleDistanceSquared(Point p, Point a, Point b, Point c) {
  const auto ab = b - a, ac = c - a, n = glm::cross(ab, ac);
  const double nn = glm::dot(n, n);
  if (nn > 0) {
    const auto q = p - n * (glm::dot(p - a, n) / nn);
    if (glm::dot(glm::cross(b - a, q - a), n) >= 0 &&
        glm::dot(glm::cross(c - b, q - b), n) >= 0 &&
        glm::dot(glm::cross(a - c, q - c), n) >= 0) {
      const double d = glm::dot(p - a, n);
      return d * d / nn;
    }
  }
  return std::min({segmentDistanceSquared(p, a, b),
                   segmentDistanceSquared(p, b, c),
                   segmentDistanceSquared(p, c, a)});
}
struct Vertex {
  UV uv;
  glm::vec3 point;
};

class Mesher {
public:
  Mesher(const ParametricSurface &surface, double units, double extent,
         std::optional<glm::vec3> fanPoint = {})
      : s_(surface), chord_(.001 / units),
        tolerance_(std::max(1e-7 / units, extent * 2e-7)), fanPoint_(fanPoint) {
  }
  std::vector<CurvedFaceTriangle>
  run(const std::vector<std::vector<glm::vec3>> &boundaries, bool explicitOuter,
      bool sameSense);

private:
  Point evaluate(UV uv) {
    if (++evaluations_ > 4000000)
      throw std::runtime_error(
          "Curved face exceeds its surface evaluation budget");
    const auto p = s_.point(uv.x, uv.y);
    if (!p || !std::isfinite(p->x) || !std::isfinite(p->y) ||
        !std::isfinite(p->z))
      throw std::runtime_error(
          "Curved face parameter is outside a regular surface domain");
    return *p;
  }
  std::pair<Point, Point> derivatives(UV uv) {
    std::array<Point, 2> result;
    for (unsigned axis = 0; axis < 2; ++axis) {
      const auto domain = axis ? s_.vDomain : s_.uDomain;
      const double span = domain             ? domain->second - domain->first
                          : s_.periods[axis] ? s_.periods[axis]
                                             : std::max(1., std::abs(uv[axis]));
      const double h = span * 1e-6;
      auto a = uv, b = uv;
      a[axis] -= h;
      b[axis] += h;
      if (domain && !s_.periods[axis]) {
        a[axis] = std::max(a[axis], domain->first);
        b[axis] = std::min(b[axis], domain->second);
      }
      if (b[axis] <= a[axis])
        throw std::runtime_error("Degenerate curved face parameter domain");
      result[axis] = (evaluate(b) - evaluate(a)) / (b[axis] - a[axis]);
    }
    return {result[0], result[1]};
  }
  UV inverse(Point point);
  std::optional<UV> solve(Point point, UV seed);
  void seedInverse();
  void triangulate(const std::vector<std::vector<Vertex>> &rings,
                   bool sameSense);
  bool acceptable(const Triangle &t, bool sameSense);
  uint32_t insert(UV uv) {
    if (vertices_.size() >= 131072)
      throw std::runtime_error("Curved face exceeds its vertex budget");
    const auto p = evaluate(uv);
    const glm::vec3 stored(p);
    if (glm::length(Point(stored) - p) > chord_ * .1)
      throw std::runtime_error(
          "Curved face coordinates exceed its mesh precision");
    vertices_.push_back({uv, stored});
    return static_cast<uint32_t>(vertices_.size() - 1);
  }
  void add(Triangle t) {
    if (triangles_.size() >= 524288)
      throw std::runtime_error("Curved face exceeds its triangulation budget");
    uint32_t id;
    if (freeTriangles_.empty()) {
      id = static_cast<uint32_t>(triangles_.size());
      triangles_.push_back(t);
      active_.push_back(true);
      generations_.push_back(0);
    } else {
      id = freeTriangles_.back();
      freeTriangles_.pop_back();
      triangles_[id] = t;
      active_[id] = true;
    }
    pending_.push_back({id, ++generations_[id]});
    for (size_t i = 0; i < 3; ++i) {
      const auto e = edge(t[i], t[(i + 1) % 3]);
      adjacent_[e].insert(id);
      flipPending_.push_back(e);
    }
  }
  void remove(uint32_t id) {
    active_[id] = false;
    freeTriangles_.push_back(id);
    const auto t = triangles_[id];
    for (size_t i = 0; i < 3; ++i) {
      auto found = adjacent_.find(edge(t[i], t[(i + 1) % 3]));
      found->second.erase(id);
      if (found->second.empty())
        adjacent_.erase(found);
    }
  }
  void refine(bool sameSense);
  void improveQuality(bool sameSense);
  const ParametricSurface &s_;
  double chord_, tolerance_;
  size_t evaluations_ = 0;
  std::vector<std::pair<UV, Point>> seeds_;
  std::optional<UV> previous_;
  UV low_{0}, scale_{1};
  std::vector<Vertex> vertices_;
  std::vector<Triangle> triangles_;
  std::vector<bool> active_;
  std::deque<std::pair<uint32_t, uint32_t>> pending_;
  std::vector<uint32_t> freeTriangles_, generations_;
  std::set<Edge> boundary_;
  std::map<Edge, Edge> seams_;
  std::deque<Edge> flipPending_;
  size_t flipChecks_ = 0;
  size_t bandFirstSize_ = 0;
  unsigned bandAxis_ = 0;
  std::optional<glm::vec3> fanPoint_;
  std::map<Edge, std::set<uint32_t>> adjacent_;
};

void Mesher::seedInverse() {
  std::array<std::vector<double>, 2> parameters;
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto domain = axis ? s_.vDomain : s_.uDomain;
    if (!domain && !s_.periods[axis])
      throw std::runtime_error(
          "Surface has no bounded or analytic inverse for this face");
    const double a = domain ? domain->first : 0,
                 b = domain ? domain->second : s_.periods[axis];
    if (!std::isfinite(a) || !std::isfinite(b) || b <= a)
      throw std::runtime_error("Invalid curved face surface domain");
    for (unsigned i = 0; i <= 8; ++i)
      parameters[axis].push_back(a + (b - a) * i / 8);
    for (double t : axis ? s_.meshV : s_.meshU)
      if (t > a && t < b)
        parameters[axis].push_back(t);
    std::ranges::sort(parameters[axis]);
    parameters[axis].erase(
        std::unique(parameters[axis].begin(), parameters[axis].end()),
        parameters[axis].end());
  }
  if (parameters[0].size() * parameters[1].size() > 1024)
    throw std::runtime_error("Curved face inverse seed budget exceeded");
  for (double u : parameters[0])
    for (double v : parameters[1])
      seeds_.push_back({{u, v}, evaluate({u, v})});
}

std::optional<UV> Mesher::solve(Point point, UV uv) {
  const auto clamp = [&](UV p) {
    for (unsigned axis = 0; axis < 2; ++axis) {
      const auto domain = axis ? s_.vDomain : s_.uDomain;
      const double a = domain ? domain->first : 0,
                   b = domain ? domain->second : s_.periods[axis];
      p[axis] = std::clamp(p[axis], a, b);
    }
    return p;
  };
  for (unsigned iteration = 0; iteration < 40; ++iteration) {
    const auto residual = evaluate(uv) - point;
    const double distance = glm::dot(residual, residual);
    const auto [a, b] = derivatives(uv);
    const double aa = glm::dot(a, a), ab = glm::dot(a, b), bb = glm::dot(b, b),
                 determinant = aa * bb - ab * ab;
    if (determinant <= aa * bb * 1e-14)
      return std::nullopt;
    const double ar = glm::dot(a, residual), br = glm::dot(b, residual);
    const UV step((bb * ar - ab * br) / determinant,
                  (aa * br - ab * ar) / determinant);
    bool progressed = false;
    for (double fraction = 1; fraction >= 1. / 256; fraction *= .5) {
      const auto next = clamp(uv - step * fraction);
      const auto r = evaluate(next) - point;
      if (glm::dot(r, r) < distance) {
        uv = next;
        progressed = true;
        break;
      }
    }
    if (!progressed)
      break;
  }
  return glm::length(evaluate(uv) - point) <= tolerance_ ? std::optional(uv)
                                                         : std::nullopt;
}

UV Mesher::inverse(Point point) {
  std::optional<UV> result;
  if (s_.inverse) {
    result = s_.inverse(point);
  } else {
    if (seeds_.empty())
      seedInverse();
    std::vector<size_t> order(seeds_.size());
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::ranges::sort(order, [&](size_t a, size_t b) {
      const auto x = seeds_[a].second - point, y = seeds_[b].second - point;
      return glm::dot(x, x) < glm::dot(y, y);
    });
    if (previous_)
      result = solve(point, *previous_);
    // Search multiple basins even with a good continuation seed. Distinct roots
    // mean the boundary needs a pcurve to identify its intended surface chart.
    for (size_t k = 0; k < std::min(size_t(12), order.size()); ++k) {
      auto candidate = solve(point, seeds_[order[k]].first);
      if (!candidate)
        continue;
      if (result) {
        auto d = *candidate - *result;
        for (unsigned axis = 0; axis < 2; ++axis) {
          const double period = s_.periods[axis];
          if (period)
            d[axis] -= std::round(d[axis] / period) * period;
          const auto domain = axis ? s_.vDomain : s_.uDomain;
          d[axis] /= domain ? domain->second - domain->first : period;
        }
        if (glm::length(d) > 1e-5)
          throw std::runtime_error("Ambiguous curved face inverse; an explicit "
                                   "surface chart is required");
      } else
        result = candidate;
    }
  }
  if (!result || !std::isfinite(result->x) || !std::isfinite(result->y) ||
      glm::length(evaluate(*result) - point) > tolerance_)
    throw std::runtime_error(
        "Boundary does not lie on a regular part of its face surface");
  previous_ = result;
  return *result;
}

void Mesher::triangulate(const std::vector<std::vector<Vertex>> &rings,
                         bool sameSense) {
  low_ = rings.front().front().uv;
  UV high = low_;
  for (const auto &ring : rings)
    for (const auto &v : ring) {
      low_ = glm::min(low_, v.uv);
      high = glm::max(high, v.uv);
    }
  scale_ = high - low_;
  if (scale_.x <= 0 || scale_.y <= 0)
    throw std::runtime_error("Degenerate curved face parameter region");
  std::vector<std::vector<std::array<double, 2>>> polygon;
  std::vector<UV> normalized;
  constexpr double epsilon = 1e-10;
  const auto onSegment = [](UV p, UV a, UV b) {
    return std::abs(orient(a, b, p)) <= epsilon &&
           p.x >= std::min(a.x, b.x) - epsilon &&
           p.x <= std::max(a.x, b.x) + epsilon &&
           p.y >= std::min(a.y, b.y) - epsilon &&
           p.y <= std::max(a.y, b.y) + epsilon;
  };
  const auto inside = [&](UV p, const auto &ring) {
    bool result = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
      const UV a(ring[j][0], ring[j][1]), b(ring[i][0], ring[i][1]);
      if (onSegment(p, a, b))
        return 0;
      if ((a.y > p.y) != (b.y > p.y) &&
          p.x < a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y))
        result = !result;
    }
    return result ? 1 : -1;
  };
  std::vector<double> areas;
  for (const auto &ring : rings) {
    if (ring.size() < 3)
      throw std::runtime_error("Curved face loop has too few samples");
    auto &p = polygon.emplace_back();
    const auto start = static_cast<uint32_t>(vertices_.size());
    double area = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
      const auto a = (ring[i].uv - low_) / scale_,
                 b = (ring[(i + 1) % ring.size()].uv - low_) / scale_;
      if (glm::length(b - a) <= epsilon)
        throw std::runtime_error("Degenerate curved face boundary segment");
      area += cross(a, b);
      normalized.push_back(a);
      p.push_back({a.x, a.y});
      vertices_.push_back(ring[i]);
      boundary_.insert(
          edge(start + static_cast<uint32_t>(i),
               start + static_cast<uint32_t>((i + 1) % ring.size())));
    }
    if (std::abs(area) <= epsilon)
      throw std::runtime_error("Degenerate curved face boundary area");
    if ((area > 0) != (areas.empty() ? sameSense : !sameSense))
      throw std::runtime_error(
          "Face SameSense conflicts with its oriented boundary");
    areas.push_back(area);
  }
  size_t checks = 0;
  for (size_t r = 0; r < polygon.size(); ++r) {
    const auto &a = polygon[r];
    if (r && inside(UV(a[0][0], a[0][1]), polygon.front()) != 1)
      throw std::runtime_error("Curved face hole lies outside its outer bound");
    for (size_t s = r; s < polygon.size(); ++s) {
      const auto &b = polygon[s];
      if (s > r && r &&
          (inside(UV(b[0][0], b[0][1]), a) >= 0 ||
           inside(UV(a[0][0], a[0][1]), b) >= 0))
        throw std::runtime_error("Nested or intersecting curved face holes");
      for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = s == r ? i + 1 : 0; j < b.size(); ++j) {
          if (++checks > 8000000)
            throw std::runtime_error(
                "Curved face boundary validation budget exceeded");
          if (r == s && (j == i + 1 || (i == 0 && j + 1 == a.size())))
            continue;
          const UV p(a[i][0], a[i][1]),
              q(a[(i + 1) % a.size()][0], a[(i + 1) % a.size()][1]),
              x(b[j][0], b[j][1]),
              y(b[(j + 1) % b.size()][0], b[(j + 1) % b.size()][1]);
          const double px = orient(p, q, x), py = orient(p, q, y),
                       xp = orient(x, y, p), xq = orient(x, y, q);
          if ((px * py < 0 && xp * xq < 0) || onSegment(x, p, q) ||
              onSegment(y, p, q) || onSegment(p, x, y) || onSegment(q, x, y))
            throw std::runtime_error("Intersecting curved face boundary loops");
        }
    }
  }
  std::vector<uint32_t> indices;
  bool bandGrid = false;
  if (fanPoint_) {
    if (rings.size() != 1)
      throw std::runtime_error("Polar chart requires a single enclosing ring");
    const auto uv = inverse(Point(*fanPoint_));
    const auto center = insert(uv);
    vertices_[center].point = *fanPoint_;
    normalized.push_back((uv - low_) / scale_);
    for (uint32_t i = 0; i < center; ++i)
      indices.insert(indices.end(), {i, (i + 1) % center, center});
  } else if (bandFirstSize_ && vertices_.size() == 2 * bandFirstSize_) {
    const auto count = static_cast<uint32_t>(bandFirstSize_);
    const double offset =
        vertices_.back().uv[bandAxis_] - vertices_.front().uv[bandAxis_];
    bandGrid = true;
    for (uint32_t i = 0; i < count; ++i)
      if (std::abs(vertices_[i].uv[bandAxis_] -
                   vertices_[2 * count - 1 - i].uv[bandAxis_] + offset) >
          scale_[bandAxis_] * 1e-10)
        bandGrid = false;
    if (bandGrid) {
      std::set<double> fractions{0, 1};
      for (uint32_t i = 0; i < count; ++i) {
        const auto a = vertices_[i].uv;
        auto &b = vertices_[2 * count - 1 - i].uv;
        b[bandAxis_] = a[bandAxis_] + offset;
        normalized[2 * count - 1 - i] = (b - low_) / scale_;
        ParametricCurve curve;
        curve.domain = {{0, 1}};
        curve.point = [&, a, b](double t) -> std::optional<Point> {
          return evaluate(a * (1 - t) + b * t);
        };
        const auto samples = sampleCurve(curve, 0, 1, chord_ * .25, 4097);
        if (samples.empty())
          throw std::runtime_error(
              "Periodic band cross section exceeds its sample budget");
        for (const auto &p : samples)
          fractions.insert(p.parameter);
      }
      if (fractions.size() * count > 131072)
        throw std::runtime_error(
            "Periodic band exceeds its grid vertex budget");
      boundary_.erase(edge(0, 2 * count - 1));
      boundary_.erase(edge(count - 1, count));
      std::vector<uint32_t> previous;
      for (double fraction : fractions) {
        std::vector<uint32_t> row;
        for (uint32_t i = 0; i < count; ++i) {
          const auto opposite = 2 * count - 1 - i;
          uint32_t id;
          if (fraction == 0)
            id = i;
          else if (fraction == 1)
            id = opposite;
          else {
            id = insert(vertices_[i].uv * (1 - fraction) +
                        vertices_[opposite].uv * fraction);
            normalized.push_back((vertices_[id].uv - low_) / scale_);
            if (i == count - 1)
              vertices_[id].point = vertices_[row.front()].point;
          }
          row.push_back(id);
        }
        if (!previous.empty()) {
          boundary_.insert(edge(previous.front(), row.front()));
          boundary_.insert(edge(previous.back(), row.back()));
          for (uint32_t i = 0; i + 1 < count; ++i)
            indices.insert(indices.end(),
                           {previous[i], previous[i + 1], row[i],
                            previous[i + 1], row[i + 1], row[i]});
        }
        previous = std::move(row);
      }
    }
  }
  if (bandFirstSize_ && !bandGrid) {
    // A zipper connects the two monotone rings without Earcut's long fans
    // across a full revolution. Boundary sample counts can differ.
    uint32_t a = 0, b = static_cast<uint32_t>(vertices_.size() - 1);
    const auto endA = static_cast<uint32_t>(bandFirstSize_ - 1),
               endB = static_cast<uint32_t>(bandFirstSize_);
    const double sense =
        vertices_[endA].uv[bandAxis_] > vertices_[0].uv[bandAxis_] ? 1 : -1;
    const double offset =
        vertices_[b].uv[bandAxis_] - vertices_[a].uv[bandAxis_];
    while (a < endA || b > endB) {
      if (a < endA && b > endB &&
          std::abs(vertices_[a + 1].uv[bandAxis_] -
                   vertices_[b - 1].uv[bandAxis_] + offset) <
              scale_[bandAxis_] * 1e-10) {
        vertices_[b - 1].uv[bandAxis_] =
            vertices_[a + 1].uv[bandAxis_] + offset;
        normalized[b - 1] = (vertices_[b - 1].uv - low_) / scale_;
        indices.insert(indices.end(), {a, a + 1, b, a + 1, b - 1, b});
        ++a;
        --b;
        continue;
      }
      if (a < endA && (b == endB ||
                       sense * vertices_[a + 1].uv[bandAxis_] <=
                           sense * (vertices_[b - 1].uv[bandAxis_] - offset))) {
        indices.insert(indices.end(), {a, a + 1, b});
        ++a;
      } else {
        indices.insert(indices.end(), {a, b - 1, b});
        --b;
      }
    }
  } else if (!bandGrid && !fanPoint_)
    indices = mapbox::earcut<uint32_t>(polygon);
  std::vector<Triangle> initial;
  std::vector<bool> used(vertices_.size(), false);
  double triangulatedArea = 0;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    Triangle t{indices[i], indices[i + 1], indices[i + 2]};
    double area = orient(normalized[t[0]], normalized[t[1]], normalized[t[2]]);
    if (std::abs(area) <= 1e-18)
      continue;
    if ((area > 0) != sameSense) {
      std::swap(t[1], t[2]);
      area = -area;
    }
    triangulatedArea += area;
    for (auto v : t)
      used[v] = true;
    initial.push_back(t);
  }
  double expectedArea = 0;
  for (double area : areas)
    expectedArea += area;
  if (initial.empty() ||
      std::abs(expectedArea - triangulatedArea) > std::abs(expectedArea) * 1e-7)
    throw std::runtime_error("Invalid curved face parameter triangulation");
  // Earcut elides collinear UV samples (e.g. all samples on a cylinder's cap).
  // Restore them before refinement so the adjacent cap keeps identical edges.
  for (uint32_t v = 0; v < used.size(); ++v) {
    if (used[v])
      continue;
    bool inserted = false;
    for (size_t i = 0; i < initial.size() && !inserted; ++i) {
      if (++checks > 8000000)
        throw std::runtime_error(
            "Curved face boundary restoration budget exceeded");
      const auto t = initial[i];
      for (size_t j = 0; j < 3; ++j) {
        const auto a = t[j], b = t[(j + 1) % 3], c = t[(j + 2) % 3];
        if (onSegment(normalized[v], normalized[a], normalized[b])) {
          initial[i] = {a, v, c};
          initial.push_back({v, b, c});
          inserted = true;
          break;
        }
      }
    }
    if (!inserted)
      throw std::runtime_error("Curved face lost a shared boundary sample");
  }
  for (const auto &t : initial)
    add(t);
  for (const auto &[e, neighbors] : adjacent_)
    if (neighbors.size() != (boundary_.contains(e) ? 1u : 2u))
      throw std::runtime_error(
          "Curved face parameter mesh has disconnected boundary edges");
  std::map<std::pair<std::array<float, 3>, std::array<float, 3>>, Edge>
      spatialEdges;
  for (const auto &e : boundary_) {
    const auto key = [](glm::vec3 p) {
      return std::array<float, 3>{p.x, p.y, p.z};
    };
    auto a = key(vertices_[e.first].point), b = key(vertices_[e.second].point);
    if (b < a)
      std::swap(a, b);
    const auto [found, inserted] = spatialEdges.emplace(std::pair(a, b), e);
    if (!inserted) {
      if (seams_.contains(found->second))
        throw std::runtime_error("Repeated curved face seam segment");
      seams_[e] = found->second;
      seams_[found->second] = e;
    }
  }
  for (const auto &e : boundary_) {
    if (!adjacent_.contains(e))
      throw std::runtime_error("Curved face lost a boundary segment");
    if (seams_.contains(e))
      continue;
    const auto &a = vertices_[e.first], &b = vertices_[e.second];
    for (double t : {.25, .5, .75})
      if (segmentDistanceSquared(evaluate(a.uv * (1 - t) + b.uv * t),
                                 Point(a.point),
                                 Point(b.point)) > chord_ * chord_)
        throw std::runtime_error(
            "Shared boundary exceeds the curved face chord budget");
  }
}

bool Mesher::acceptable(const Triangle &t, bool sameSense) {
  const auto &a = vertices_[t[0]], &b = vertices_[t[1]], &c = vertices_[t[2]];
  const auto normal = glm::cross(Point(b.point) - Point(a.point),
                                 Point(c.point) - Point(a.point));
  const auto [du, dv] = derivatives((a.uv + b.uv + c.uv) / 3.);
  const auto surfaceNormal = glm::cross(du, dv);
  if (glm::length(surfaceNormal) <= glm::length(du) * glm::length(dv) * 1e-10)
    throw std::runtime_error(
        "Curved face contains a singular surface parameter");
  const auto agrees = [&](Point n) {
    return glm::dot(normal, n) * (sameSense ? 1 : -1) > 0 &&
           glm::length(normal) > 0 && glm::length(n) > 0;
  };
  if (!agrees(surfaceNormal))
    return false;
  const auto check = [&](UV uv) {
    return triangleDistanceSquared(evaluate(uv), Point(a.point), Point(b.point),
                                   Point(c.point)) <= chord_ * chord_;
  };
  for (unsigned i = 0; i <= 4; ++i)
    for (unsigned j = 0; j + i <= 4; ++j) {
      if ((i == 0 && j == 0) || i == 4 || j == 4)
        continue;
      if (!check(a.uv * (1 - (i + j) / 4.) + b.uv * (i / 4.) + c.uv * (j / 4.)))
        return false;
    }
  if (!check((a.uv + b.uv + c.uv) / 3.))
    return false;
  // Include knot lines: a narrow spline span must not hide between probes.
  for (unsigned axis = 0; axis < 2; ++axis)
    for (double knot : axis ? s_.meshV : s_.meshU) {
      std::vector<UV> hits;
      for (unsigned j = 0; j < 3; ++j) {
        const auto x = vertices_[t[j]].uv, y = vertices_[t[(j + 1) % 3]].uv;
        if (knot > std::min(x[axis], y[axis]) &&
            knot < std::max(x[axis], y[axis]))
          hits.push_back(x +
                         (y - x) * ((knot - x[axis]) / (y[axis] - x[axis])));
      }
      for (auto p : hits)
        if (!check(p))
          return false;
      if (hits.size() == 2 && !check((hits[0] + hits[1]) / 2.))
        return false;
    }
  return true;
}

void Mesher::improveQuality(bool sameSense) {
  const auto uv = [&](uint32_t id) {
    return (vertices_[id].uv - low_) / scale_;
  };
  const auto quality = [](UV a, UV b, UV c) {
    const auto ab = b - a, bc = c - b, ca = a - c;
    const double denominator =
        glm::dot(ab, ab) + glm::dot(bc, bc) + glm::dot(ca, ca);
    return denominator > 0 ? std::abs(orient(a, b, c)) / denominator : 0;
  };
  while (!flipPending_.empty()) {
    const auto e = flipPending_.front();
    flipPending_.pop_front();
    if (++flipChecks_ > 8000000)
      throw std::runtime_error("Curved face mesh quality budget exceeded");
    const auto found = adjacent_.find(e);
    if (boundary_.contains(e) || found == adjacent_.end() ||
        found->second.size() != 2)
      continue;
    const auto first = *found->second.begin(), second = *found->second.rbegin();
    const auto opposite = [&](uint32_t t) {
      for (auto v : triangles_[t])
        if (v != e.first && v != e.second)
          return v;
      throw std::runtime_error("Invalid curved face mesh adjacency");
    };
    const auto c = opposite(first), d = opposite(second);
    const auto aUV = uv(e.first), bUV = uv(e.second), cUV = uv(c), dUV = uv(d);
    if (orient(cUV, dUV, aUV) * orient(cUV, dUV, bUV) >= 0 ||
        adjacent_.contains(edge(c, d)))
      continue;
    const double oldQuality =
                     std::min(quality(aUV, bUV, cUV), quality(aUV, bUV, dUV)),
                 newQuality =
                     std::min(quality(cUV, dUV, aUV), quality(cUV, dUV, bUV));
    if (newQuality <= oldQuality * (1 + 1e-5))
      continue;
    Triangle x{c, d, e.first}, y{d, c, e.second};
    for (auto *t : {&x, &y})
      if ((orient(uv((*t)[0]), uv((*t)[1]), uv((*t)[2])) > 0) != sameSense)
        std::swap((*t)[1], (*t)[2]);
    remove(first);
    remove(second);
    add(x);
    add(y);
  }
}

void Mesher::refine(bool sameSense) {
  improveQuality(sameSense);
  while (!pending_.empty()) {
    const auto [id, generation] = pending_.front();
    pending_.pop_front();
    if (!active_[id] || generations_[id] != generation)
      continue;
    const auto t = triangles_[id];
    if (acceptable(t, sameSense))
      continue;
    std::optional<Edge> split;
    double longest = -1;
    for (unsigned j = 0; j < 3; ++j) {
      const auto e = edge(t[j], t[(j + 1) % 3]);
      if (boundary_.contains(e) && !seams_.contains(e))
        continue;
      const auto d = (vertices_[e.first].uv - vertices_[e.second].uv) / scale_;
      const double length = glm::dot(d, d);
      if (length > longest) {
        longest = length;
        split = e;
      }
    }
    if (!split) {
      const auto v = insert(
          (vertices_[t[0]].uv + vertices_[t[1]].uv + vertices_[t[2]].uv) / 3.);
      remove(id);
      for (unsigned j = 0; j < 3; ++j)
        add({t[j], t[(j + 1) % 3], v});
      improveQuality(sameSense);
      continue;
    }
    if (longest <= 1e-20)
      throw std::runtime_error("Curved face refinement cannot resolve its "
                               "chord or orientation error");
    const auto v =
        insert((vertices_[split->first].uv + vertices_[split->second].uv) / 2.);
    const auto divide = [&](Edge e, uint32_t midpoint) {
      const auto neighbors = adjacent_.at(e);
      for (auto n : neighbors) {
        const auto old = triangles_[n];
        remove(n);
        for (unsigned j = 0; j < 3; ++j)
          if (edge(old[j], old[(j + 1) % 3]) == e) {
            add({old[j], midpoint, old[(j + 2) % 3]});
            add({midpoint, old[(j + 1) % 3], old[(j + 2) % 3]});
            break;
          }
      }
    };
    if (seams_.contains(*split)) {
      const auto partner = seams_.at(*split);
      const auto w = insert(
          (vertices_[partner.first].uv + vertices_[partner.second].uv) / 2.);
      vertices_[w].point = vertices_[v].point;
      uint32_t a = partner.first, b = partner.second;
      if (vertices_[a].point != vertices_[split->first].point)
        std::swap(a, b);
      seams_.erase(*split);
      seams_.erase(partner);
      boundary_.erase(*split);
      boundary_.erase(partner);
      for (auto pair : {std::pair(edge(split->first, v), edge(a, w)),
                        std::pair(edge(v, split->second), edge(w, b))}) {
        seams_[pair.first] = pair.second;
        seams_[pair.second] = pair.first;
        boundary_.insert(pair.first);
        boundary_.insert(pair.second);
      }
      divide(partner, w);
    }
    divide(*split, v);
    improveQuality(sameSense);
  }
}

std::vector<CurvedFaceTriangle>
Mesher::run(const std::vector<std::vector<glm::vec3>> &boundaries,
            bool explicitOuter, bool sameSense) {
  std::vector<std::vector<Vertex>> rings;
  std::vector<UV> winding;
  size_t samples = 0;
  for (const auto &loop : boundaries) {
    if (loop.size() < 3 || (samples += loop.size()) > 8192)
      throw std::runtime_error(
          "Invalid or excessive curved face boundary samples");
    previous_.reset();
    auto &ring = rings.emplace_back();
    for (auto p : loop) {
      auto uv = inverse(Point(p));
      if (!ring.empty())
        for (unsigned axis = 0; axis < 2; ++axis)
          if (s_.periods[axis])
            uv[axis] += std::round((ring.back().uv[axis] - uv[axis]) /
                                   s_.periods[axis]) *
                        s_.periods[axis];
      ring.push_back({uv, p});
    }
    auto closure = ring.front().uv;
    for (unsigned axis = 0; axis < 2; ++axis)
      if (s_.periods[axis])
        closure[axis] += std::round((ring.back().uv[axis] - closure[axis]) /
                                    s_.periods[axis]) *
                         s_.periods[axis];
    winding.push_back(closure - ring.front().uv);
  }
  const auto winds = [](UV w) { return glm::length(w) > 1e-10; };
  if (!explicitOuter) {
    // Two oppositely wound noncontractible rings describe a periodic band.
    // Join existing samples with a possibly slanted cut in parameter space.
    // Its two sides differ by exactly one period and share exact 3D endpoints.
    // No real boundary is resampled independently of its adjacent cap.
    if (rings.size() != 2 || !winds(winding[0]) ||
        glm::length(winding[0] + winding[1]) > 1e-8 * glm::length(winding[0]))
      throw std::runtime_error(
          "Closed curved face requires two opposite periodic band bounds");
    const unsigned axis = std::abs(winding[0].x) > 0 ? 0 : 1;
    const unsigned other = 1 - axis;
    const double period = s_.periods[axis];
    if (!period || std::abs(winding[0][other]) > 1e-10 ||
        std::abs(std::abs(winding[0][axis]) - period) > period * 1e-8)
      throw std::runtime_error(
          "Unsupported multiple winding curved face chart");
    auto &first = rings[0];
    auto &second = rings[1];
    if (s_.periods[other]) {
      double a = 0, b = 0;
      for (const auto &v : first)
        a += v.uv[other];
      for (const auto &v : second)
        b += v.uv[other];
      const double difference = b / second.size() - a / first.size();
      const double sense = (sameSense ? 1. : -1.) *
                           (winding[0][axis] > 0 ? 1. : -1.) *
                           (axis == 0 ? 1. : -1.);
      if (difference * sense <= 0) {
        const double shift =
            sense * s_.periods[other] *
            (std::floor(-difference * sense / s_.periods[other]) + 1);
        for (auto &v : second)
          v.uv[other] += shift;
      }
    }
    const double end = first.front().uv[axis] + winding[0][axis];
    size_t start = 0;
    double distance = period;
    for (size_t i = 0; i < second.size(); ++i) {
      const double d = (second[i].uv[axis] - end) / period;
      const double candidate = std::abs(d - std::round(d)) * period;
      if (candidate < distance) {
        start = i;
        distance = candidate;
      }
    }
    std::rotate(second.begin(), second.begin() + start, second.end());
    second.front().uv[axis] +=
        std::round((end - second.front().uv[axis]) / period) * period;
    for (size_t i = 1; i < second.size(); ++i)
      second[i].uv[axis] +=
          std::round((second[i - 1].uv[axis] - second[i].uv[axis]) / period) *
          period;
    first.push_back({first.front().uv + winding[0], first.front().point});
    second.push_back({second.front().uv + winding[1], second.front().point});
    const double sign = winding[0][axis] > 0 ? 1 : -1;
    for (const auto *ring : {&first, &second})
      for (size_t i = 1; i < ring->size(); ++i)
        if (sign * ((*ring)[i].uv[axis] - (*ring)[i - 1].uv[axis]) *
                (ring == &first ? 1 : -1) <=
            0)
          throw std::runtime_error("Periodic band bounds must be monotone in "
                                   "their cyclic parameter");
    bandFirstSize_ = first.size();
    bandAxis_ = axis;
    first.insert(first.end(), second.begin(), second.end());
    rings.resize(1);
  } else {
    if (std::ranges::any_of(winding, winds))
      throw std::runtime_error(
          "Outer bound does not close in its curved surface chart");
    UV center(0);
    for (const auto &v : rings.front())
      center += v.uv;
    center /= rings.front().size();
    for (size_t i = 1; i < rings.size(); ++i) {
      UV inner(0), shift(0);
      for (const auto &v : rings[i])
        inner += v.uv;
      inner /= rings[i].size();
      for (unsigned axis = 0; axis < 2; ++axis)
        if (s_.periods[axis])
          shift[axis] =
              std::round((center[axis] - inner[axis]) / s_.periods[axis]) *
              s_.periods[axis];
      for (auto &v : rings[i])
        v.uv += shift;
    }
  }
  triangulate(rings, sameSense);
  refine(sameSense);
  std::vector<glm::vec3> normals;
  for (const auto &v : vertices_) {
    const auto [x, y] = derivatives(v.uv);
    const auto n = glm::cross(x, y);
    if (glm::length(n) <= glm::length(x) * glm::length(y) * 1e-10)
      throw std::runtime_error(
          "Curved face contains a singular boundary parameter");
    normals.emplace_back(glm::normalize(n) * (sameSense ? 1. : -1.));
  }
  std::vector<CurvedFaceTriangle> result;
  for (size_t i = 0; i < triangles_.size(); ++i)
    if (active_[i]) {
      const auto &t = triangles_[i];
      result.push_back({{vertices_[t[0]].point, vertices_[t[1]].point,
                         vertices_[t[2]].point},
                        {normals[t[0]], normals[t[1]], normals[t[2]]}});
    }
  return result;
}

std::vector<CurvedFaceTriangle>
meshSphericalPoleFace(const ParametricSurface &surface,
                      const std::vector<std::vector<glm::vec3>> &boundaries,
                      bool explicitOuter, bool sameSense, double units) {
  const auto &sphere = *surface.sphere;
  const auto &f = sphere.frame;
  const double radius = sphere.radius;
  if (!std::isfinite(radius) || radius <= 0)
    throw std::runtime_error("Invalid polar sphere radius");
  const double tolerance = std::max(1e-7 / units, radius * 4e-7);
  std::optional<glm::vec3> authoredPole;
  std::optional<int> poleSign;
  const std::vector<glm::vec3> *ring = nullptr;
  for (const auto &loop : boundaries) {
    if (loop.size() == 1) {
      if (authoredPole)
        throw std::runtime_error(
            "Spherical face requires a single vertex loop");
      const Point p(loop.front());
      const double north = glm::length(p - f.origin - f.z * radius),
                   south = glm::length(p - f.origin + f.z * radius);
      if (std::min(north, south) > tolerance)
        throw std::runtime_error(
            "Spherical vertex loop must identify a surface pole");
      authoredPole = loop.front();
      poleSign = north < south ? 1 : -1;
    } else if (loop.size() >= 3 && !ring)
      ring = &loop;
    else
      throw std::runtime_error("Unsupported spherical vertex-loop bounds");
  }
  if (!authoredPole ||
      (ring && explicitOuter && boundaries.front().size() == 1))
    throw std::runtime_error("Spherical cap outer bound must be its edge loop");
  const auto chart = [&](int sign) {
    ParametricSurface s;
    // Stereographic coordinates stay regular at the selected pole. Reflecting
    // the southern chart's V axis keeps both charts' normals outward.
    s.point = [=](double u, double v) -> std::optional<Point> {
      const double squared = u * u + v * v, denominator = 1 + squared;
      const auto p = f.origin + radius / denominator *
                                    (2 * u * f.x + 2 * sign * v * f.y +
                                     sign * (1 - squared) * f.z);
      return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)
                 ? std::optional(p)
                 : std::nullopt;
    };
    s.inverse = [=](Point p) -> std::optional<UV> {
      const auto d = p - f.origin;
      const double denominator = radius + sign * glm::dot(d, f.z);
      if (std::abs(glm::length(d) - radius) > tolerance ||
          denominator <= radius * 1e-12)
        return std::nullopt;
      return UV(glm::dot(d, f.x), sign * glm::dot(d, f.y)) / denominator;
    };
    return s;
  };
  const auto cap = [&](const auto &loop, int sign, glm::vec3 pole) {
    const auto s = chart(sign);
    // One monotone revolution must enclose the pole. The ordinary chart
    // validation then rejects intersections and conflicting face orientation.
    double winding = 0;
    std::optional<double> previous;
    double first = 0;
    for (const auto &p : loop) {
      const auto uv = s.inverse(Point(p));
      if (!uv || glm::length(*uv) <= 1e-12)
        throw std::runtime_error("Polar face boundary meets a singular pole");
      const double angle = std::atan2(uv->y, uv->x);
      if (previous) {
        const double step =
            std::remainder(angle - *previous, 2 * std::numbers::pi);
        if (step * (sameSense ? 1 : -1) <= 0)
          throw std::runtime_error(
              "Polar face boundary must wind monotonically around its pole");
        winding += step;
      } else
        first = angle;
      previous = angle;
    }
    winding += std::remainder(first - *previous, 2 * std::numbers::pi);
    if (std::abs(winding - (sameSense ? 2 : -2) * std::numbers::pi) > 1e-8)
      throw std::runtime_error("Polar face boundary must wind exactly once");
    return Mesher(s, units, 2 * radius, pole).run({loop}, true, sameSense);
  };
  const auto latitudeRing = [&](double height) {
    const double radialRadius = std::sqrt(radius * radius - height * height);
    ParametricCurve circle;
    circle.domain = {{0, 2 * std::numbers::pi}};
    circle.point = [=](double u) -> std::optional<Point> {
      return f.origin + height * f.z +
             radialRadius * (std::cos(u) * f.x + std::sin(u) * f.y);
    };
    const auto samples =
        sampleCurve(circle, 0, 2 * std::numbers::pi, .0005 / units, 4097);
    if (samples.size() < 4)
      throw std::runtime_error("Spherical latitude exceeds its sample budget");
    std::vector<glm::vec3> result;
    for (size_t i = 0; i + 1 < samples.size(); ++i)
      result.emplace_back(samples[i].point);
    return result;
  };
  if (ring) {
    // Keep the pole chart within its hemisphere. The original ring can lie
    // past the equator; a regular angular band joins it to an interior latitude
    // without moving or adding any source boundary samples.
    double highest = -radius;
    const auto s = chart(*poleSign);
    for (const auto &p : *ring) {
      if (!s.inverse(Point(p)))
        throw std::runtime_error(
            "Polar boundary is outside its spherical chart");
      highest =
          std::max(highest, *poleSign * glm::dot(Point(p) - f.origin, f.z));
    }
    auto collar =
        latitudeRing(*poleSign * std::max(0., (highest + radius) / 2));
    if ((sameSense ? 1 : -1) * *poleSign < 0)
      std::ranges::reverse(collar);
    auto result = cap(collar, *poleSign, *authoredPole);
    std::ranges::reverse(collar);
    const auto band = Mesher(surface, units, 2 * radius)
                          .run({*ring, collar}, false, sameSense);
    result.insert(result.end(), band.begin(), band.end());
    return result;
  }

  // A spherical face bounded only by its degenerate pole loop covers the
  // closed sphere. Two regular hemispheres share one exact sampled equator.
  auto equatorialRing = latitudeRing(0);
  if (!sameSense)
    std::ranges::reverse(equatorialRing);
  auto north =
      cap(equatorialRing, 1,
          *poleSign == 1 ? *authoredPole : glm::vec3(f.origin + radius * f.z));
  std::ranges::reverse(equatorialRing);
  auto south =
      cap(equatorialRing, -1,
          *poleSign == -1 ? *authoredPole : glm::vec3(f.origin - radius * f.z));
  north.insert(north.end(), south.begin(), south.end());
  return north;
}
} // namespace

std::optional<std::vector<CurvedFaceTriangle>>
meshIfcCurvedFace(const ParametricSurface &surface,
                  const std::vector<std::vector<glm::vec3>> &boundaries,
                  bool explicitOuter, bool sameSense, double units,
                  std::string &error) {
  try {
    if (boundaries.empty() || !std::isfinite(units) || units <= 0 ||
        !surface.point)
      throw std::runtime_error(
          "Missing curved face boundaries, surface or length units");
    double extent = 0;
    if (boundaries.front().empty())
      throw std::runtime_error("Empty curved face outer loop");
    if (std::ranges::any_of(
            boundaries, [](const auto &loop) { return loop.size() == 1; })) {
      if (!surface.sphere)
        throw std::runtime_error("Unsupported vertex-loop surface chart");
      return meshSphericalPoleFace(surface, boundaries, explicitOuter,
                                   sameSense, units);
    }
    const Point origin(boundaries.front().front());
    for (const auto &loop : boundaries)
      for (auto p : loop)
        extent = std::max(extent, glm::length(Point(p) - origin));
    return Mesher(surface, units, extent)
        .run(boundaries, explicitOuter, sameSense);
  } catch (const std::runtime_error &e) {
    error = e.what();
    return std::nullopt;
  }
}
} // namespace container::geometry::ifc::detail
