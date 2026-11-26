#include "IfcBrepFaceGeometry.h"
#include "IfcBrepTrimPlacement.h"

#include "Container/utility/ModelImportCancellation.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <glm/geometric.hpp>
#include <limits>
#include <map>
#include <mapbox/earcut.hpp>
#include <numbers>
#include <numeric>
#include <set>
#include <stdexcept>

namespace container::geometry::ifc::detail {
namespace {
using UV = glm::dvec2;
using Point = glm::dvec3;
using Edge = std::pair<uint32_t, uint32_t>;
using Triangle = std::array<uint32_t, 3>;
using ParameterPolygon = std::vector<std::vector<std::array<double, 2>>>;
Edge edge(uint32_t a, uint32_t b) { return std::minmax(a, b); }
double cross(UV a, UV b) { return a.x * b.y - a.y * b.x; }
double orient(UV a, UV b, UV c) { return cross(b - a, c - a); }

std::vector<uint32_t>
triangulateSplinePolygon(const ParameterPolygon &polygon) {
  // Independent inverse solves on a straight trim can disagree by a few ULPs.
  // Keep those samples out of Earcut's seed polygon to avoid spurious ears
  // along the trim. The original UVs and 3D boundary samples are restored
  // below, before refinement.
  constexpr double roundoff = 1e-12;
  ParameterPolygon corners;
  std::vector<uint32_t> originalIndices;
  uint32_t base = 0;
  for (const auto &ring : polygon) {
    auto &border = corners.emplace_back();
    const auto point = [&](size_t i) { return UV(ring[i][0], ring[i][1]); };
    std::vector<size_t> turns;
    for (size_t i = 0; i < ring.size(); ++i) {
      const UV a = point((i + ring.size() - 1) % ring.size()), b = point(i),
               c = point((i + 1) % ring.size());
      if (glm::dot(b - a, c - b) <= 0 ||
          std::abs(orient(a, c, b)) > roundoff * glm::length(c - a))
        turns.push_back(i);
    }
    const auto append = [&](size_t i) {
      border.push_back(ring[i]);
      originalIndices.push_back(base + static_cast<uint32_t>(i));
    };
    if (turns.size() < 3) {
      for (size_t i = 0; i < ring.size(); ++i)
        append(i);
    } else {
      for (size_t turn = 0; turn < turns.size(); ++turn) {
        const size_t first = turns[turn],
                     last = turns[(turn + 1) % turns.size()];
        append(first);
        const UV a = point(first), direction = point(last) - a;
        const double length = glm::length(direction);
        bool straight = length > 0;
        double previous = 0;
        for (size_t i = (first + 1) % ring.size(); straight && i != last;
             i = (i + 1) % ring.size()) {
          const UV offset = point(i) - a;
          const double along = glm::dot(direction, offset) / length;
          straight = along > previous && along < length &&
                     std::abs(cross(direction, offset)) <= roundoff * length;
          previous = along;
        }
        if (!straight)
          for (size_t i = (first + 1) % ring.size(); i != last;
               i = (i + 1) % ring.size())
            append(i);
      }
    }
    base += static_cast<uint32_t>(ring.size());
  }
  auto indices = mapbox::earcut<uint32_t>(corners);
  for (auto &index : indices)
    index = originalIndices[index];
  return indices;
}

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

struct InteriorPoint {
  glm::vec3 point;
  bool required;
};

struct CreaseProbe {
  CurveSample source;
  UV uv;
};

UV unwrapSplineProjection(UV uv, UV reference, UV periods) {
  for (unsigned axis = 0; axis < 2; ++axis) {
    if (periods[axis] == 0)
      continue;
    const double turns = (uv[axis] - reference[axis]) / periods[axis];
    const double shift = std::round(turns);
    if (std::abs(std::abs(turns - shift) - .5) <= 1e-7)
      throw std::runtime_error(
          "Inferred spline edge has an ambiguous half-period step");
    uv[axis] -= shift * periods[axis];
  }
  return uv;
}

class Mesher {
public:
  Mesher(const ParametricSurface &surface, double units, double extent,
         std::optional<glm::vec3> fanPoint = {},
         std::optional<InteriorPoint> interiorPoint = {})
      : s_(surface), chord_(.001 / units),
        tolerance_(std::max(1e-7 / units, extent * 2e-7)), fanPoint_(fanPoint),
        interiorPoint_(interiorPoint) {}
  std::vector<CurvedFaceTriangle>
  run(const std::vector<std::vector<glm::vec3>> &boundaries, bool explicitOuter,
      bool sameSense, std::span<const CurvedFaceChartLoop> authoredChart = {});
  std::vector<UV> projectEdge(std::span<const glm::vec3> sharedPoints,
                              const std::stop_token &stop);
  std::vector<CurveSample>
  sampleEdgeCreases(const ParametricCurve &source,
                    std::span<const CurveSample> samples,
                    const std::stop_token &stop);

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
  std::pair<Point, Point> derivatives(UV uv, std::optional<UV> interior = {}) {
    if (s_.derivatives) {
      if (interior)
        for (unsigned axis = 0; axis < 2; ++axis)
          if (onCrease(uv[axis], axis)) {
            const double offset = (*interior)[axis] - uv[axis];
            uv[axis] += std::copysign(
                std::min(std::abs(offset) * .5, scale_[axis] * 1e-9), offset);
          }
      if (++evaluations_ > 4000000)
        throw std::runtime_error("Curved face exceeds its evaluation budget");
      const auto value = s_.derivatives(uv.x, uv.y);
      if (!value)
        throw std::runtime_error("Undefined curved face derivatives");
      return *value;
    }
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
      if (interior && onCrease(uv[axis], axis)) {
        if ((*interior)[axis] > uv[axis]) {
          a[axis] = uv[axis];
          b[axis] = std::min(b[axis], (*interior)[axis]);
        } else if ((*interior)[axis] < uv[axis]) {
          b[axis] = uv[axis];
          a[axis] = std::max(a[axis], (*interior)[axis]);
        }
      }
      if (b[axis] <= a[axis])
        throw std::runtime_error("Degenerate curved face parameter domain");
      result[axis] = (evaluate(b) - evaluate(a)) / (b[axis] - a[axis]);
    }
    return {result[0], result[1]};
  }
  UV inverse(Point point, const std::stop_token &stop = {},
             bool requireNumericalInverse = false);
  std::optional<UV> solve(Point point, UV seed, const std::stop_token &stop);
  void seedInverse(const std::stop_token &stop);
  UV snapProjectedKnots(UV uv, Point point);
  CreaseProbe projectCreaseProbe(const ParametricCurve &source, double t,
                                 std::optional<UV> reference,
                                 const std::stop_token &stop,
                                 std::optional<UV> localSeed = {});
  std::optional<UV> solveNearCreaseProbe(Point point, UV predicted,
                                         const std::stop_token &stop);
  CurveSample refineEdgeCrease(const ParametricCurve &source, CreaseProbe a,
                               CreaseProbe b, unsigned axis, double knot,
                               double epsilon, const std::stop_token &stop);
  void joinSplineBand(std::vector<std::vector<Vertex>> &rings, UV winding,
                      bool sameSense);
  void joinAxialBand(std::vector<std::vector<Vertex>> &rings, UV winding,
                     UV oppositeWinding, bool sameSense);
  void joinNonmonotoneSplineBand(std::vector<std::vector<Vertex>> &rings,
                                  UV winding, bool sameSense);
  double bandProgress(UV uv) const {
    return glm::dot(bandProgress_, uv - low_);
  }
  void alignBandProgress(UV &uv, double progress) const {
    uv += bandDirection_ * (progress - bandProgress(uv));
  }
  void triangulate(const std::vector<std::vector<Vertex>> &rings,
                   bool sameSense, bool checkPeriodicCover);
  void validatePeriodicCover(std::span<const Triangle> triangles,
                             std::span<const UV> normalized, size_t &checks);
  void restoreBoundaryChains(std::vector<Triangle> &triangles,
                             std::span<const UV> normalized, size_t &checks);
  void seedInteriorPoint(std::vector<Triangle> &triangles,
                         std::span<const UV> normalized, UV uv, bool sameSense);
  std::vector<double> liftKnots(std::span<const double> knots,
                                unsigned axis) const;
  std::vector<double> bandCutCreases(const std::array<Edge, 2> &cuts) const;
  void splitBandCuts(std::vector<Triangle> &triangles,
                     const std::array<Edge, 2> &cuts,
                     std::span<const double> fractions);
  void constrainCreases(std::vector<Triangle> &triangles);
  bool onCrease(double parameter, unsigned axis) const {
    const double epsilon = scale_[axis] * 1e-10;
    const auto &creases = creases_[axis];
    const auto knot =
        std::lower_bound(creases.begin(), creases.end(), parameter - epsilon);
    return knot != creases.end() && *knot <= parameter + epsilon;
  }
  bool creaseEdge(Edge e) const {
    for (unsigned axis = 0; axis < 2; ++axis)
      if (onCrease(vertices_[e.first].uv[axis], axis) &&
          std::abs(vertices_[e.first].uv[axis] -
                   vertices_[e.second].uv[axis]) <= scale_[axis] * 1e-10)
        return true;
    return false;
  }
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
  std::array<std::vector<double>, 2> creases_, probeKnots_;
  std::map<Edge, Edge> seams_;
  std::deque<Edge> flipPending_;
  size_t flipChecks_ = 0;
  size_t bandFirstSize_ = 0;
  UV bandProgress_{0}, bandDirection_{0};
  std::optional<glm::vec3> fanPoint_;
  std::optional<InteriorPoint> interiorPoint_;
  std::map<Edge, std::set<uint32_t>> adjacent_;
};

std::vector<double> Mesher::liftKnots(std::span<const double> knots,
                                      unsigned axis) const {
  const double first = low_[axis], last = first + scale_[axis];
  const double epsilon = scale_[axis] * 1e-10;
  const double period = s_.periods[axis];
  std::vector<double> lifted;
  for (double knot : knots) {
    if (period) {
      knot += std::ceil((first - epsilon - knot) / period) * period;
      for (unsigned repeat = 0; knot <= last + epsilon; ++repeat) {
        if (repeat >= 16 || lifted.size() >= 4096)
          throw std::runtime_error(
              "Curved face periodic knot lifting exceeds its budget");
        lifted.push_back(knot);
        knot += period;
      }
    } else if (knot >= first - epsilon && knot <= last + epsilon)
      lifted.push_back(knot);
  }
  std::ranges::sort(lifted);
  lifted.erase(std::unique(lifted.begin(), lifted.end()), lifted.end());
  return lifted;
}

void Mesher::constrainCreases(std::vector<Triangle> &triangles) {
  size_t checks = 0;
  for (unsigned axis = 0; axis < 2; ++axis) {
    const double first = low_[axis], last = first + scale_[axis];
    const double epsilon = scale_[axis] * 1e-10;
    for (double knot : creases_[axis]) {
      if (knot <= first + epsilon || knot >= last - epsilon)
        continue;
      std::vector<Triangle> pieces;
      std::map<Edge, uint32_t> intersections;
      const auto side = [&](uint32_t v) {
        const double d = vertices_[v].uv[axis] - knot;
        return (d > epsilon) - (d < -epsilon);
      };
      const auto crossing = [&](uint32_t a, uint32_t b) {
        const auto e = edge(a, b);
        if (boundary_.contains(e))
          throw std::runtime_error(
              "Shared edge must have a sample at its swept profile crease");
        if (const auto it = intersections.find(e); it != intersections.end())
          return it->second;
        const auto p = vertices_[a].uv, q = vertices_[b].uv;
        auto uv = p + (q - p) * ((knot - p[axis]) / (q[axis] - p[axis]));
        uv[axis] = knot;
        const auto v = insert(uv);
        intersections.emplace(e, v);
        return v;
      };
      for (const auto &triangle : triangles) {
        if (++checks > 8000000 || pieces.size() >= 524284)
          throw std::runtime_error(
              "Curved face crease constraint exceeds its budget");
        const std::array sides{side(triangle[0]), side(triangle[1]),
                               side(triangle[2])};
        if (*std::ranges::min_element(sides) >= 0 ||
            *std::ranges::max_element(sides) <= 0) {
          pieces.push_back(triangle);
          continue;
        }
        for (int half : {-1, 1}) {
          std::array<uint32_t, 4> polygon{};
          unsigned count = 0;
          for (unsigned i = 0; i < 3; ++i) {
            const unsigned j = (i + 1) % 3;
            if (sides[i] * half >= 0)
              polygon[count++] = triangle[i];
            if (sides[i] * sides[j] < 0)
              polygon[count++] = crossing(triangle[i], triangle[j]);
          }
          for (unsigned i = 1; i + 1 < count; ++i)
            pieces.push_back({polygon[0], polygon[i], polygon[i + 1]});
        }
      }
      triangles = std::move(pieces);
    }
  }
}

std::vector<double>
Mesher::bandCutCreases(const std::array<Edge, 2> &cuts) const {
  std::vector<double> fractions;
  for (const auto &[first, last] : cuts) {
    const auto a = vertices_[first].uv, b = vertices_[last].uv;
    for (unsigned axis = 0; axis < 2; ++axis) {
      const double epsilon = scale_[axis] * 1e-10;
      const double low = std::min(a[axis], b[axis]) + epsilon;
      const double high = std::max(a[axis], b[axis]) - epsilon;
      for (double knot : creases_[axis])
        if (knot > low && knot < high)
          fractions.push_back((knot - a[axis]) / (b[axis] - a[axis]));
    }
  }
  std::ranges::sort(fractions);
  // These cuts span at most the chart extent on each axis. Roots this close
  // lie on all of the corresponding crease lines at the side-test tolerance.
  fractions.erase(
      std::unique(fractions.begin(), fractions.end(),
                  [](double a, double b) { return b - a <= 1e-12; }),
      fractions.end());
  return fractions;
}

void Mesher::splitBandCuts(std::vector<Triangle> &triangles,
                           const std::array<Edge, 2> &cuts,
                           std::span<const double> fractions) {
  if (fractions.empty())
    return;
  std::array<std::vector<uint32_t>, 2> chains;
  for (unsigned i = 0; i < cuts.size(); ++i)
    chains[i].push_back(cuts[i].first);
  for (double fraction : fractions) {
    for (unsigned i = 0; i < cuts.size(); ++i) {
      const auto a = vertices_[cuts[i].first].uv,
                 b = vertices_[cuts[i].second].uv;
      chains[i].push_back(insert(
          {std::lerp(a.x, b.x, fraction), std::lerp(a.y, b.y, fraction)}));
    }
    vertices_[chains[1].back()].point = vertices_[chains[0].back()].point;
  }
  for (unsigned i = 0; i < cuts.size(); ++i) {
    const auto &cut = cuts[i];
    auto &chain = chains[i];
    chain.push_back(cut.second);
    const auto triangle = std::ranges::find_if(triangles, [&](const auto &t) {
      return std::ranges::find(t, cut.first) != t.end() &&
             std::ranges::find(t, cut.second) != t.end();
    });
    if (triangle == triangles.end())
      throw std::runtime_error("Periodic band lost its cut edge");
    const auto old = *triangle;
    const size_t first = std::ranges::find(old, cut.first) - old.begin();
    if (old[(first + 1) % 3] != cut.second)
      std::ranges::reverse(chain);
    const auto opposite = *std::ranges::find_if(
        old, [&](uint32_t v) { return v != cut.first && v != cut.second; });
    boundary_.erase(cut);
    *triangle = {chain[0], chain[1], opposite};
    boundary_.insert(edge(chain[0], chain[1]));
    for (size_t j = 2; j < chain.size(); ++j) {
      triangles.push_back({chain[j - 1], chain[j], opposite});
      boundary_.insert(edge(chain[j - 1], chain[j]));
    }
  }
}

void Mesher::seedInverse(const std::stop_token &stop) {
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
    for (unsigned i = 0; i < 8; ++i)
      parameters[axis].push_back(a + (b - a) * i / 8);
    parameters[axis].push_back(b);
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
  for (double u : parameters[0]) {
    container::util::checkModelImportCancellation(stop);
    for (double v : parameters[1])
      seeds_.push_back({{u, v}, evaluate({u, v})});
  }
}

std::optional<UV> Mesher::solve(Point point, UV uv,
                                const std::stop_token &stop) {
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
    container::util::checkModelImportCancellation(stop);
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

UV Mesher::inverse(Point point, const std::stop_token &stop,
                   bool requireNumericalInverse) {
  std::optional<UV> result;
  if (s_.inverse && !requireNumericalInverse) {
    result = s_.inverse(point);
  } else {
    if (seeds_.empty())
      seedInverse(stop);
    std::vector<size_t> order(seeds_.size());
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::ranges::sort(order, [&](size_t a, size_t b) {
      const auto x = seeds_[a].second - point, y = seeds_[b].second - point;
      return glm::dot(x, x) < glm::dot(y, y);
    });
    if (previous_)
      result = solve(point, *previous_, stop);
    // Search multiple basins even with a good continuation seed. Distinct roots
    // mean the boundary needs a pcurve to identify its intended surface chart.
    for (size_t k = 0; k < std::min(size_t(12), order.size()); ++k) {
      auto candidate = solve(point, seeds_[order[k]].first, stop);
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

UV Mesher::snapProjectedKnots(UV uv, Point point) {
  // Mesh boundaries need the exact crease parameter when an existing shared
  // sample already lies on it. Keep a snap only if it witnesses that sample.
  auto snapped = uv;
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto domain = axis ? s_.vDomain : s_.uDomain;
    if (!domain)
      continue;
    const auto [first, last] = *domain;
    const auto &knots = axis ? s_.meshV : s_.meshU;
    const double coordinate = uv[axis];
    double distance =
        (last - first) * 1e-7 +
        std::abs(coordinate) * (8 * std::numeric_limits<double>::epsilon());
    const auto consider = [&](double knot) {
      const double delta = std::abs(coordinate - knot);
      if (delta < distance) {
        snapped[axis] = knot;
        distance = delta;
      }
    };
    consider(first);
    consider(last);
    for (const double knot : knots)
      if (knot > first && knot < last)
        consider(knot);
  }
  if (snapped != uv && glm::length(evaluate(snapped) - point) <= tolerance_)
    return snapped;
  return uv;
}

std::vector<UV> Mesher::projectEdge(std::span<const glm::vec3> sharedPoints,
                                    const std::stop_token &stop) {
  std::vector<UV> result;
  result.reserve(sharedPoints.size());
  const bool closed = sharedPoints.front() == sharedPoints.back();
  for (size_t i = 0; i < sharedPoints.size(); ++i) {
    container::util::checkModelImportCancellation(stop);
    const Point point(sharedPoints[i]);
    auto uv = closed && i + 1 == sharedPoints.size()
                  ? result.front()
                  : snapProjectedKnots(inverse(point, stop, true), point);
    if (!result.empty()) {
      const auto previous = result.back();
      uv = unwrapSplineProjection(uv, previous, s_.periods);
      if (!std::isfinite(uv.x) || !std::isfinite(uv.y) || uv == previous)
        throw std::runtime_error(
            "Inferred spline edge has a repeated or invalid chart sample");
      const Point prior(sharedPoints[i - 1]);
      for (const double fraction : {.25, .5, .75}) {
        const UV between(std::lerp(previous.x, uv.x, fraction),
                         std::lerp(previous.y, uv.y, fraction));
        if (segmentDistanceSquared(evaluate(between), prior, point) >
            chord_ * chord_)
          throw std::runtime_error(
              "Inferred spline edge exceeds the shared edge chord budget");
      }
    }
    result.push_back(uv);
  }
  return result;
}

CreaseProbe Mesher::projectCreaseProbe(const ParametricCurve &source, double t,
                                       std::optional<UV> reference,
                                       const std::stop_token &stop,
                                       std::optional<UV> localSeed) {
  container::util::checkModelImportCancellation(stop);
  const auto point = source.point(t);
  if (!point || !std::isfinite(point->x) || !std::isfinite(point->y) ||
      !std::isfinite(point->z))
    throw std::runtime_error(
        "Undefined native edge during spline crease search");
  const auto local =
      localSeed ? solveNearCreaseProbe(*point, *localSeed, stop) : std::nullopt;
  auto uv = local ? *local : inverse(*point, stop, true);
  if (reference)
    uv = unwrapSplineProjection(uv, *reference, s_.periods);
  if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
    throw std::runtime_error("Invalid inferred spline crease parameter");
  return {{t, *point}, uv};
}

std::optional<UV> Mesher::solveNearCreaseProbe(Point point, UV predicted,
                                               const std::stop_token &stop) {
  UV seed = predicted;
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto [first, last] = *(axis ? s_.vDomain : s_.uDomain);
    const double period = s_.periods[axis];
    if (period && (seed[axis] < first || seed[axis] > last)) {
      double phase = std::fmod(seed[axis] - first, period);
      if (!std::isfinite(phase))
        return std::nullopt;
      if (phase < 0)
        phase += period;
      seed[axis] = first + phase;
    }
    seed[axis] = std::clamp(seed[axis], first, last);
  }
  auto uv = solve(point, seed, stop);
  if (!uv)
    return std::nullopt;
  // An interior continuation must stay near the prediction. A distant root
  // or a wrap too large for the local bracket needs the full basin search.
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto [first, last] = *(axis ? s_.vDomain : s_.uDomain);
    const double period = s_.periods[axis];
    const double delta = (*uv)[axis] - predicted[axis];
    const double separation = period ? std::remainder(delta, period) : delta;
    if (!(std::abs(separation) <= (last - first) * .25))
      return std::nullopt;
  }
  return uv;
}

CurveSample Mesher::refineEdgeCrease(const ParametricCurve &source,
                                     CreaseProbe a, CreaseProbe b,
                                     unsigned axis, double knot, double epsilon,
                                     const std::stop_token &stop) {
  const auto distance = [axis, knot](const CreaseProbe &probe) {
    return std::abs(probe.uv[axis] - knot);
  };
  auto best = distance(a) < distance(b) ? a : b;
  const bool firstBelow = a.uv[axis] < knot;
  for (unsigned iteration = 0; iteration < 53; ++iteration) {
    const double t = std::midpoint(a.source.parameter, b.source.parameter);
    if (t == a.source.parameter || t == b.source.parameter)
      break;
    const UV reference(std::midpoint(a.uv.x, b.uv.x),
                       std::midpoint(a.uv.y, b.uv.y));
    const auto probe =
        projectCreaseProbe(source, t, reference, stop, reference);
    if (distance(probe) < distance(best))
      best = probe;
    if (distance(probe) <= epsilon * .125)
      break;
    if ((probe.uv[axis] < knot) == firstBelow)
      a = probe;
    else
      b = probe;
  }
  // Public boundary samples still need the same multiple-root check as the
  // original endpoints. Refine in one basin, then independently verify it.
  best = projectCreaseProbe(source, best.source.parameter, best.uv, stop);
  if (distance(best) > epsilon)
    throw std::runtime_error(
        "Could not isolate the native spline crease crossing");
  best.uv[axis] = knot;
  if (glm::length(evaluate(best.uv) - best.source.point) > tolerance_)
    throw std::runtime_error(
        "Inferred spline crease does not meet the native edge");
  return best.source;
}

std::vector<CurveSample>
Mesher::sampleEdgeCreases(const ParametricCurve &source,
                          std::span<const CurveSample> samples,
                          const std::stop_token &stop) {
  constexpr size_t probesPerInterval = 5;
  std::array<CreaseProbe, probesPerInterval> probes;
  probes.front() =
      projectCreaseProbe(source, samples.front().parameter, {}, stop);
  std::vector<CurveSample> additions;
  size_t checks = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    const double first = samples[i - 1].parameter, last = samples[i].parameter;
    // Each original endpoint receives the full inverse check. Predict only
    // the intermediate probes from the nearest lifted copies of these roots.
    probes.back() = projectCreaseProbe(source, last, {}, stop);
    const auto continuation = previous_;
    for (size_t j = 1; j + 1 < probes.size(); ++j) {
      const double t = std::lerp(first, last, double(j) / (probes.size() - 1));
      const UV previous = probes[j - 1].uv;
      UV end = probes.back().uv;
      for (unsigned axis = 0; axis < 2; ++axis)
        if (s_.periods[axis])
          end[axis] =
              previous[axis] +
              std::remainder(end[axis] - previous[axis], s_.periods[axis]);
      const double step = 1. / (probes.size() - j);
      const UV predicted(std::lerp(previous.x, end.x, step),
                         std::lerp(previous.y, end.y, step));
      probes[j] = projectCreaseProbe(source, t, previous, stop, predicted);
    }
    probes.back().uv = unwrapSplineProjection(
        probes.back().uv, probes[probes.size() - 2].uv, s_.periods);
    for (unsigned axis = 0; axis < 2; ++axis) {
      const auto &creases = axis ? s_.creaseV : s_.creaseU;
      if (creases.empty())
        continue;
      const auto domain = *(axis ? s_.vDomain : s_.uDomain);
      const auto [lo, hi] = std::ranges::minmax(
          probes, {}, [axis](const auto &p) { return p.uv[axis]; });
      const double low = lo.uv[axis], high = hi.uv[axis];
      const double epsilon = (high - low) * 1e-10 +
                             std::max({std::abs(low), std::abs(high),
                                       domain.second - domain.first}) *
                                 (32 * std::numeric_limits<double>::epsilon());
      const double period = s_.periods[axis];
      for (double knot : creases) {
        if (++checks > 1'000'000)
          throw std::runtime_error(
              "Inferred spline crease search exceeds its work budget");
        if (period)
          knot += std::ceil((low - epsilon - knot) / period) * period;
        for (size_t copies = 0; knot <= high + epsilon; ++copies) {
          if (copies >= 16)
            throw std::runtime_error(
                "Inferred spline crease search exceeds its period budget");
          if (knot >= low - epsilon) {
            // Skip samples already on the crease. Opposite nonzero signs on
            // either side still identify a crossing when a probe hits it.
            std::optional<size_t> prior;
            for (size_t j = 0; j < probes.size(); ++j) {
              const double offset = probes[j].uv[axis] - knot;
              if (std::abs(offset) <= epsilon)
                continue;
              if (prior && (probes[*prior].uv[axis] < knot) != (offset < 0)) {
                const auto root =
                    refineEdgeCrease(source, probes[*prior], probes[j], axis,
                                     knot, epsilon, stop);
                if (root.parameter != first && root.parameter != last) {
                  if (additions.size() >= 4097)
                    throw std::runtime_error("Inferred spline crease search "
                                             "exceeds its sample budget");
                  additions.push_back(root);
                }
              }
              prior = j;
            }
          }
          if (!period)
            break;
          knot += period;
        }
      }
    }
    previous_ = continuation;
    probes.front() = probes.back();
  }
  const bool forward = samples.back().parameter > samples.front().parameter;
  std::ranges::sort(additions, [forward](const auto &a, const auto &b) {
    return forward ? a.parameter < b.parameter : a.parameter > b.parameter;
  });
  const auto duplicates =
      std::ranges::unique(additions, {}, &CurveSample::parameter);
  additions.erase(duplicates.begin(), duplicates.end());
  return additions;
}

void Mesher::validatePeriodicCover(std::span<const Triangle> triangles,
                                   std::span<const UV> normalized,
                                   size_t &checks) {
  constexpr double epsilon = 1e-10;
  const UV period = s_.periods / scale_;
  glm::ivec2 copies(0);
  for (unsigned axis = 0; axis < 2; ++axis) {
    if (!s_.periods[axis])
      continue;
    if (period[axis] < 1. / 16)
      throw std::runtime_error("Spline chart image search exceeds its budget");
    copies[axis] = static_cast<int>((1 - epsilon) / period[axis]);
  }
  if (copies == glm::ivec2(0))
    return;
  struct BoundedTriangle {
    std::array<UV, 3> points;
    UV low, high;
  };
  std::vector<BoundedTriangle> bounded;
  bounded.reserve(triangles.size());
  for (const auto &t : triangles) {
    const auto a = normalized[t[0]], b = normalized[t[1]], c = normalized[t[2]];
    bounded.push_back(
        {{a, b, c}, glm::min(a, glm::min(b, c)), glm::max(a, glm::max(b, c))});
  }
  std::ranges::sort(bounded, {}, [](const auto &t) { return t.low.x; });
  const auto separated = [&](const BoundedTriangle &a, const BoundedTriangle &b,
                             UV offset, const BoundedTriangle &axes) {
    for (size_t i = 0; i < axes.points.size(); ++i) {
      const auto edge = axes.points[(i + 1) % 3] - axes.points[i];
      const UV normal(-edge.y, edge.x);
      const auto range = [&](const BoundedTriangle &t) {
        return std::minmax({glm::dot(normal, t.points[0]),
                            glm::dot(normal, t.points[1]),
                            glm::dot(normal, t.points[2])});
      };
      const auto [aLow, aHigh] = range(a);
      const auto [bLow, bHigh] = range(b);
      const double shift = glm::dot(normal, offset);
      if (std::min(aHigh, bHigh + shift) - std::max(aLow, bLow + shift) <=
          epsilon * glm::length(normal))
        return true;
    }
    return false;
  };
  // Test one translation of each opposite pair. Periodic images may touch
  // along a seam but they must never share triangle interiors.
  for (int x = 0; x <= copies.x; ++x)
    for (int y = x == 0 ? 1 : -copies.y; y <= copies.y; ++y) {
      const UV offset = UV(x, y) * period;
      for (const auto &b : bounded)
        for (const auto &a : bounded) {
          if (++checks > 8000000)
            throw std::runtime_error(
                "Spline chart image search exceeds its budget");
          if (a.low.x >= b.high.x + offset.x - epsilon)
            break;
          if (a.high.x <= b.low.x + offset.x + epsilon ||
              a.high.y <= b.low.y + offset.y + epsilon ||
              a.low.y >= b.high.y + offset.y - epsilon)
            continue;
          if (!separated(a, b, offset, a) && !separated(a, b, offset, b))
            throw std::runtime_error("Spline chart overlaps a periodic image");
        }
    }
}

void Mesher::triangulate(const std::vector<std::vector<Vertex>> &rings,
                         bool sameSense, bool checkPeriodicCover) {
  low_ = rings.front().front().uv;
  UV high = low_;
  for (const auto &ring : rings)
    for (const auto &v : ring) {
      low_ = glm::min(low_, v.uv);
      high = glm::max(high, v.uv);
    }
  scale_ = high - low_;
  if (!std::isfinite(scale_.x) || !std::isfinite(scale_.y) || scale_.x <= 0 ||
      scale_.y <= 0)
    throw std::runtime_error("Degenerate curved face parameter region");
  for (unsigned axis = 0; axis < 2; ++axis) {
    creases_[axis] = liftKnots(axis ? s_.creaseV : s_.creaseU, axis);
    const auto &knots = axis ? s_.meshV : s_.meshU;
    probeKnots_[axis] = s_.spline ? liftKnots(knots, axis) : knots;
  }
  ParameterPolygon polygon;
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
  const bool monotoneBand = bandFirstSize_ && bandDirection_ != UV(0);
  const bool cutSplineBand = bandFirstSize_ && !monotoneBand;
  const bool splinePolygon =
      !monotoneBand && !fanPoint_ && s_.spline && bool(s_.derivatives);
  bool bandGrid = false;
  std::array<Edge, 2> bandCuts{};
  if (bandFirstSize_) {
    const auto count = static_cast<uint32_t>(bandFirstSize_);
    bandCuts = {Edge{0, static_cast<uint32_t>(vertices_.size() - 1)},
                Edge{count - 1, count}};
  }
  const double progressTolerance =
      glm::dot(glm::abs(bandProgress_), scale_) * 1e-10;
  if (fanPoint_) {
    if (rings.size() != 1)
      throw std::runtime_error("Polar chart requires a single enclosing ring");
    const auto uv = inverse(Point(*fanPoint_));
    const auto center = insert(uv);
    vertices_[center].point = *fanPoint_;
    normalized.push_back((uv - low_) / scale_);
    for (uint32_t i = 0; i < center; ++i)
      indices.insert(indices.end(), {i, (i + 1) % center, center});
  } else if (monotoneBand && vertices_.size() == 2 * bandFirstSize_) {
    const auto count = static_cast<uint32_t>(bandFirstSize_);
    const double offset =
        bandProgress(vertices_.back().uv) - bandProgress(vertices_.front().uv);
    bandGrid = true;
    for (uint32_t i = 0; i < count; ++i)
      if (std::abs(bandProgress(vertices_[i].uv) -
                   bandProgress(vertices_[2 * count - 1 - i].uv) + offset) >
          progressTolerance)
        bandGrid = false;
    if (bandGrid) {
      std::set<double> fractions{0, 1};
      for (uint32_t i = 0; i < count; ++i) {
        const auto a = vertices_[i].uv;
        auto &b = vertices_[2 * count - 1 - i].uv;
        alignBandProgress(b, bandProgress(a) + offset);
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
      const auto cutCreases = bandCutCreases(bandCuts);
      for (double crease : cutCreases) {
        // Keep the knot row when adaptive sampling chose a nearby row.
        constexpr double epsilon = 1e-10;
        const auto low = std::max(std::nextafter(0., 1.), crease - epsilon);
        const auto high = std::min(std::nextafter(1., 0.), crease + epsilon);
        fractions.erase(fractions.lower_bound(low),
                        fractions.upper_bound(high));
      }
      fractions.insert(cutCreases.begin(), cutCreases.end());
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
  if (monotoneBand && !bandGrid) {
    // A zipper connects the two monotone rings without Earcut's long fans
    // across a full revolution. Boundary sample counts can differ.
    uint32_t a = 0, b = static_cast<uint32_t>(vertices_.size() - 1);
    const auto endA = static_cast<uint32_t>(bandFirstSize_ - 1),
               endB = static_cast<uint32_t>(bandFirstSize_);
    const auto progress = [&](uint32_t id) {
      return bandProgress(vertices_[id].uv);
    };
    const double sense = progress(endA) > progress(0) ? 1 : -1;
    const double offset = progress(b) - progress(a);
    while (a < endA || b > endB) {
      if (a < endA && b > endB &&
          std::abs(progress(a + 1) - progress(b - 1) + offset) <
              progressTolerance) {
        alignBandProgress(vertices_[b - 1].uv, progress(a + 1) + offset);
        normalized[b - 1] = (vertices_[b - 1].uv - low_) / scale_;
        indices.insert(indices.end(), {a, a + 1, b, a + 1, b - 1, b});
        ++a;
        --b;
        continue;
      }
      if (a < endA && (b == endB || sense * progress(a + 1) <=
                                        sense * (progress(b - 1) - offset))) {
        indices.insert(indices.end(), {a, a + 1, b});
        ++a;
      } else {
        indices.insert(indices.end(), {a, b - 1, b});
        --b;
      }
    }
  } else if (!bandGrid && !fanPoint_) {
    if (splinePolygon)
      indices = triangulateSplinePolygon(polygon);
    else
      indices = mapbox::earcut<uint32_t>(polygon);
  }
  std::vector<Triangle> initial;
  std::vector<bool> used(vertices_.size(), false);
  double triangulatedArea = 0;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    Triangle t{indices[i], indices[i + 1], indices[i + 2]};
    const auto a = normalized[t[0]], b = normalized[t[1]], c = normalized[t[2]];
    double area = orient(a, b, c);
    // Distinct trim corners can lie on the same chart line to numerical
    // precision. Drop zero-width ears before restoring the real edge chains.
    const double roundoff = splinePolygon
                                ? 1e-12 * std::max({glm::length(b - a),
                                                    glm::length(c - b),
                                                    glm::length(a - c)})
                                : 1e-18;
    if (std::abs(area) <= roundoff)
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
  if (checkPeriodicCover && !cutSplineBand)
    validatePeriodicCover(initial, normalized, checks);
  // Earcut elides collinear UV samples (e.g. all samples on a cylinder's cap).
  // Restore them before refinement so the adjacent cap keeps identical edges.
  const bool restoreChains =
      bool(s_.derivatives) && !fanPoint_ && !monotoneBand;
  for (uint32_t v = 0; !restoreChains && v < used.size(); ++v) {
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
  if (restoreChains) {
    restoreBoundaryChains(initial, normalized, checks);
    double restoredArea = 0.;
    for (const auto &t : initial)
      restoredArea +=
          orient(normalized[t[0]], normalized[t[1]], normalized[t[2]]);
    if (std::abs(expectedArea - restoredArea) > std::abs(expectedArea) * 1e-7)
      throw std::runtime_error(
          "Curved face boundary restoration changed its area");
  }
  if (cutSplineBand)
    validatePeriodicCover(initial, normalized, checks);
  if (interiorPoint_) {
    const auto uv = inverse(Point(interiorPoint_->point));
    const auto p = (uv - low_) / scale_;
    const bool contained = inside(p, polygon.front()) == 1 &&
                           std::ranges::none_of(std::span(polygon).subspan(1),
                                                [&](const auto &hole) {
                                                  return inside(p, hole) >= 0;
                                                });
    if (contained)
      seedInteriorPoint(initial, normalized, uv, sameSense);
    else if (interiorPoint_->required)
      throw std::runtime_error("Curved face apex must lie inside its trim");
  }
  if (bandFirstSize_ && !bandGrid)
    splitBandCuts(initial, bandCuts, bandCutCreases(bandCuts));
  constrainCreases(initial);
  for (const auto &t : initial)
    add(t);
  for (const auto &[e, neighbors] : adjacent_) {
    const bool boundary = boundary_.contains(e);
    if (neighbors.size() != (boundary ? 1u : 2u)) {
      const auto a = vertices_[e.first].uv, b = vertices_[e.second].uv;
      throw std::runtime_error(
          "Curved face parameter mesh has disconnected " +
          std::string(boundary ? "source boundary edge " : "interior edge ") +
          std::to_string(e.first) + "-" + std::to_string(e.second) + " (" +
          std::to_string(a.x) + "," + std::to_string(a.y) + " to " +
          std::to_string(b.x) + "," + std::to_string(b.y) + ")");
    }
  }
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

void Mesher::restoreBoundaryChains(std::vector<Triangle> &triangles,
                                   std::span<const UV> normalized,
                                   size_t &checks) {
  // Earcut can use an intermediate source vertex on one side of a bridge
  // while joining the entire collinear run on the other side.
  struct EdgeUse {
    size_t count = 0;
    size_t triangle = 0;
  };
  std::map<Edge, EdgeUse> edges;
  for (size_t i = 0; i < triangles.size(); ++i)
    for (size_t j = 0; j < 3; ++j) {
      auto &use = edges[edge(triangles[i][j], triangles[i][(j + 1) % 3])];
      use.triangle = i;
      ++use.count;
    }
  using Chain = std::pair<Edge, std::vector<uint32_t>>;
  std::vector<std::vector<Chain>> splits(triangles.size());
  // Projected samples came through float storage. Compare in the normalized
  // chart, then require matching topology before accepting the result.
  constexpr double epsilon = 1e-7;
  for (const auto &[e, use] : edges) {
    if (use.count != 1 || boundary_.contains(e))
      continue;
    const auto a = normalized[e.first], b = normalized[e.second];
    const auto direction = b - a;
    const double lengthSquared = glm::dot(direction, direction);
    if (lengthSquared <= epsilon * epsilon)
      continue;
    const double endMargin = epsilon / std::sqrt(lengthSquared);
    std::vector<std::pair<double, uint32_t>> ordered;
    // Before interior refinement every input vertex is a source sample.
    for (uint32_t v = 0; v < normalized.size(); ++v) {
      if (++checks > 8000000)
        throw std::runtime_error("Curved face boundary chain budget exceeded");
      if (std::ranges::find(triangles[use.triangle], v) !=
          triangles[use.triangle].end())
        continue;
      const auto offset = normalized[v] - a;
      const double t = glm::dot(offset, direction) / lengthSquared;
      if (t > endMargin && t < 1 - endMargin &&
          glm::length(offset - t * direction) <= epsilon)
        ordered.emplace_back(t, v);
    }
    if (ordered.empty())
      continue;
    std::ranges::sort(ordered);
    auto &run =
        splits[use.triangle].emplace_back(e, std::vector<uint32_t>{}).second;
    for (const auto &[_, v] : ordered)
      run.push_back(v);
  }
  for (size_t i = 0; i < splits.size(); ++i) {
    auto &requests = splits[i];
    if (requests.empty())
      continue;
    const auto original = triangles[i];
    std::vector<Triangle> pieces{original};
    for (auto &[e, chain] : requests) {
      bool found = false;
      for (size_t k = 0; k < pieces.size() && !found; ++k)
        for (size_t j = 0; j < 3; ++j) {
          const auto t = pieces[k];
          const auto a = t[j], b = t[(j + 1) % 3], c = t[(j + 2) % 3];
          if (edge(a, b) != e)
            continue;
          if (a != e.first)
            std::ranges::reverse(chain);
          pieces[k] = {a, chain.front(), c};
          for (size_t split = 1; split < chain.size(); ++split)
            pieces.push_back({chain[split - 1], chain[split], c});
          pieces.push_back({chain.back(), b, c});
          found = true;
          break;
        }
      if (!found)
        throw std::runtime_error("Curved face lost a boundary chain");
    }
    if (triangles.size() + pieces.size() > 524288)
      throw std::runtime_error(
          "Curved face boundary chains exceed the triangle budget");
    const double sign = orient(normalized[original[0]], normalized[original[1]],
                               normalized[original[2]]) > 0
                            ? 1.
                            : -1.;
    for (const auto &t : pieces)
      if (sign * orient(normalized[t[0]], normalized[t[1]], normalized[t[2]]) <=
          0)
        throw std::runtime_error(
            "Curved face boundary chain reverses a triangle");
    triangles[i] = pieces.front();
    triangles.insert(triangles.end(), pieces.begin() + 1, pieces.end());
  }
}

void Mesher::seedInteriorPoint(std::vector<Triangle> &triangles,
                               std::span<const UV> normalized, UV uv,
                               bool sameSense) {
  const auto p = (uv - low_) / scale_;
  const auto v = insert(uv);
  vertices_[v].point = interiorPoint_->point;
  const size_t count = triangles.size();
  size_t edgeSplits = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto t = triangles[i];
    std::array<double, 3> side;
    for (size_t j = 0; j < t.size(); ++j)
      side[j] = orient(normalized[t[j]], normalized[t[(j + 1) % 3]], p) *
                (sameSense ? 1. : -1.);
    const auto onEdge = std::ranges::min_element(side);
    if (*onEdge < -1e-14)
      continue;
    if (*onEdge > 1e-14) {
      triangles[i] = {t[0], t[1], v};
      triangles.push_back({t[1], t[2], v});
      triangles.push_back({t[2], t[0], v});
      return;
    }
    const size_t j = onEdge - side.begin();
    triangles[i] = {t[j], v, t[(j + 2) % 3]};
    triangles.push_back({v, t[(j + 1) % 3], t[(j + 2) % 3]});
    if (++edgeSplits == 2)
      return;
  }
  throw std::runtime_error(
      "Curved face could not preserve its interior apex sample");
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
  for (unsigned axis = 0; axis < 2; ++axis) {
    const double low = std::min({a.uv[axis], b.uv[axis], c.uv[axis]});
    const double high = std::max({a.uv[axis], b.uv[axis], c.uv[axis]});
    const auto &knots = probeKnots_[axis];
    for (auto it = std::upper_bound(knots.begin(), knots.end(), low);
         it != knots.end() && *it < high; ++it) {
      const double knot = *it;
      std::array<UV, 2> hits{};
      size_t count = 0;
      for (unsigned j = 0; j < 3; ++j) {
        const auto x = vertices_[t[j]].uv, y = vertices_[t[(j + 1) % 3]].uv;
        if (knot > std::min(x[axis], y[axis]) &&
            knot < std::max(x[axis], y[axis]))
          hits[count++] =
              x + (y - x) * ((knot - x[axis]) / (y[axis] - x[axis]));
      }
      for (size_t i = 0; i < count; ++i)
        if (!check(hits[i]))
          return false;
      if (count == 2 && !check((hits[0] + hits[1]) / 2.))
        return false;
    }
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
    if (boundary_.contains(e) || creaseEdge(e) || found == adjacent_.end() ||
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

void Mesher::joinSplineBand(std::vector<std::vector<Vertex>> &rings, UV winding,
                            bool sameSense) {
  // In integer period coordinates a simple closed band boundary must
  // follow a primitive lattice vector. Choose an integral progress
  // coordinate with one unit of advance along that vector.
  constexpr int maxTurns = 8;
  const UV turns = glm::round(winding / s_.periods);
  if (!std::isfinite(turns.x) || !std::isfinite(turns.y) ||
      glm::any(glm::greaterThan(glm::abs(turns), UV(maxTurns))) ||
      glm::length(winding / s_.periods - turns) > 1e-8 ||
      std::gcd(static_cast<int>(turns.x), static_cast<int>(turns.y)) != 1)
    throw std::runtime_error("Spline band requires bounded primitive winding");

  auto &first = rings[0];
  const auto &second = rings[1];
  std::vector<Vertex> forward;
  forward.reserve(second.size());
  forward.push_back(second.front());
  for (auto i = second.rbegin(); i != second.rend() - 1; ++i)
    forward.push_back({i->uv + winding, i->point});

  const UV origin = first.front().uv;
  const auto periodCoordinates = [&](UV uv) {
    return (uv - origin) / s_.periods;
  };
  constexpr double epsilon = 1e-10;
  const auto monotone = [&](const std::vector<Vertex> &ring, UV basis) {
    const double begin = glm::dot(basis, periodCoordinates(ring.front().uv));
    double prior = begin;
    for (const auto &v : std::span(ring).subspan(1)) {
      const double current = glm::dot(basis, periodCoordinates(v.uv));
      if (current <= prior + epsilon)
        return false;
      prior = current;
    }
    return prior + epsilon < begin + 1;
  };
  UV progress(0);
  int best = 2 * maxTurns * maxTurns + 1;
  for (int x = -maxTurns; x <= maxTurns; ++x)
    for (int y = -maxTurns; y <= maxTurns; ++y) {
      const UV candidate(x, y);
      const int norm = x * x + y * y;
      if (norm < best && glm::dot(candidate, turns) == 1 &&
          monotone(first, candidate) && monotone(forward, candidate)) {
        progress = candidate;
        best = norm;
      }
    }
  if (progress == UV(0)) {
    joinNonmonotoneSplineBand(rings, turns * s_.periods, sameSense);
    return;
  }

  // The transform has determinant one. Each boundary now forms a
  // period-one graph. A single cover exists precisely when the graphs
  // remain ordered between the same two consecutive transverse images.
  const auto graph = [&](const std::vector<Vertex> &ring) {
    std::vector<UV> points;
    points.reserve(ring.size() + 1);
    for (const auto &v : ring) {
      const auto uv = periodCoordinates(v.uv);
      points.emplace_back(glm::dot(progress, uv), cross(turns, uv));
    }
    points.push_back(points.front() + UV(1, 0));
    return points;
  };
  const auto a = graph(first), b = graph(forward);
  const auto height = [](const std::vector<UV> &points, double position) {
    position -= std::floor(position - points.front().x);
    if (position <= points.front().x || position >= points.back().x)
      return points.front().y;
    const auto next =
        std::upper_bound(points.begin(), points.end(), position,
                         [](double p, UV vertex) { return p < vertex.x; });
    const auto prior = next - 1;
    return std::lerp(prior->y, next->y,
                     (position - prior->x) / (next->x - prior->x));
  };
  const double sense = sameSense ? 1 : -1;
  const double shift = -std::floor(sense * height(b, 0)) * sense;
  const auto separated = [&](UV vertex) {
    const double gap =
        sense * (height(b, vertex.x) + shift - height(a, vertex.x));
    return gap > epsilon && gap < 1 - epsilon;
  };
  if (!std::ranges::all_of(a, separated) || !std::ranges::all_of(b, separated))
    throw std::runtime_error("Spline band bounds intersect a periodic image");

  // Cut at existing shared samples. The second cut is the first translated
  // by the full winding vector; both copies refine with identical positions.
  const auto phase = [](double p) { return p - std::round(p); };
  const UV transverse = UV(-progress.y, progress.x) * s_.periods * shift;
  std::optional<size_t> start;
  double shortest = std::numeric_limits<double>::infinity();
  size_t checks = 0;
  for (size_t i = 0; i < forward.size(); ++i) {
    const double p = phase(b[i].x);
    const auto end = forward[i].uv + transverse - std::round(b[i].x) * winding;
    auto previous = Point(first.front().point);
    const Point endpoint(forward[i].point);
    if (glm::length(endpoint - previous) >= shortest)
      continue;
    double length = 0;
    for (double fraction : {.125, .25, .375, .5, .625, .75, .875}) {
      const auto point = evaluate(origin + fraction * (end - origin));
      length += glm::length(point - previous);
      previous = point;
    }
    length += glm::length(endpoint - previous);
    if (length >= shortest)
      continue;
    const auto onCut = [&](UV v) {
      if (++checks > 8000000)
        throw std::runtime_error("Spline band seam search exceeds its budget");
      double x = v.x - std::floor(v.x - std::min(0., p));
      if (x <= std::min(0., p) || x >= std::max(0., p))
        return true;
      const double y = (x / p) * (b[i].y + shift);
      return sense * (y - height(a, x)) > 0 &&
             sense * (height(b, x) + shift - y) > 0;
    };
    if (std::ranges::all_of(a, onCut) && std::ranges::all_of(b, onCut)) {
      start = i;
      shortest = length;
    }
  }
  if (!start) {
    // A rail may turn tightly around its first sample and block every cut
    // from that anchor. Search both rails before rejecting their shared samples.
    joinNonmonotoneSplineBand(rings, turns * s_.periods, sameSense);
    return;
  }
  const size_t j = *start;
  const UV translation = transverse + (1 - std::round(b[j].x)) * winding;
  first.push_back({first.front().uv + winding, first.front().point});
  bandFirstSize_ = first.size();
  bandProgress_ = progress / s_.periods;
  bandDirection_ = winding;
  for (size_t step = 0; step <= forward.size(); ++step) {
    auto v = forward[(j + forward.size() - step) % forward.size()];
    v.uv += translation - (step > j ? winding : UV(0));
    first.push_back(v);
  }
  rings.resize(1);
}

void Mesher::joinNonmonotoneSplineBand(
    std::vector<std::vector<Vertex>> &rings, UV winding, bool sameSense) {
  const auto parameters = [](const std::vector<Vertex> &ring) {
    std::vector<UV> points;
    points.reserve(ring.size());
    for (const auto &v : ring)
      points.push_back(v.uv);
    return points;
  };
  const auto first = parameters(rings[0]), second = parameters(rings[1]);
  const auto cut =
      cutIfcBrepPeriodicSplineBand(first, second, winding, s_.periods, sameSense);

  // Keep the original 3D samples in cyclic order. Duplicate only the two cut
  // endpoints, translating the lifts by the winding vector on the far side.
  std::vector<Vertex> stitched;
  stitched.reserve(first.size() + second.size() + 2);
  const auto append = [&](const std::vector<Vertex> &rail, size_t start,
                          UV lift, UV advance) {
    const auto begin = stitched.size();
    for (size_t i = start; i < rail.size(); ++i)
      stitched.push_back({rail[i].uv + lift, rail[i].point});
    for (size_t i = 0; i < start; ++i)
      stitched.push_back({rail[i].uv + lift + advance, rail[i].point});
    const auto head = stitched[begin];
    stitched.push_back({head.uv + advance, head.point});
  };
  append(rings[0], cut.firstStart, UV(0), winding);
  bandFirstSize_ = stitched.size();
  append(rings[1], cut.secondStart, cut.secondOffset + winding, -winding);
  rings[0] = std::move(stitched);
  rings.resize(1);
}

void Mesher::joinAxialBand(std::vector<std::vector<Vertex>> &rings,
                          UV winding, UV oppositeWinding, bool sameSense) {
  const unsigned axis = std::abs(winding.x) > 0 ? 0 : 1;
  const unsigned other = 1 - axis;
  const double period = s_.periods[axis];
  if (!period || std::abs(winding[other]) > 1e-10 ||
      std::abs(std::abs(winding[axis]) - period) > period * 1e-8)
    throw std::runtime_error(
        "Unsupported multiple winding curved face chart");
  auto &first = rings[0];
  auto &second = rings[1];
  if (s_.spline) {
    const auto advances = [axis](const std::vector<Vertex> &rail, double step) {
      const double sign = step > 0 ? 1. : -1.;
      const double begin = rail.front().uv[axis];
      double prior = begin;
      for (const auto &vertex : std::span(rail).subspan(1)) {
        const double current = vertex.uv[axis];
        if (sign * (current - prior) <= 0)
          return false;
        prior = current;
      }
      return sign * (begin + step - prior) > 0;
    };
    if (!advances(first, winding[axis]) ||
        !advances(second, oppositeWinding[axis])) {
      UV periodWinding(0);
      periodWinding[axis] = std::copysign(period, winding[axis]);
      joinNonmonotoneSplineBand(rings, periodWinding, sameSense);
      return;
    }
  }
  if (s_.periods[other]) {
    double a = 0, b = 0;
    for (const auto &v : first)
      a += v.uv[other];
    for (const auto &v : second)
      b += v.uv[other];
    const double difference = b / second.size() - a / first.size();
    const double sense = (sameSense ? 1. : -1.) *
                         (winding[axis] > 0 ? 1. : -1.) *
                         (axis == 0 ? 1. : -1.);
    if (difference * sense <= 0) {
      const double shift =
          sense * s_.periods[other] *
          (std::floor(-difference * sense / s_.periods[other]) + 1);
      for (auto &v : second)
        v.uv[other] += shift;
    }
  }
  const double end = first.front().uv[axis] + winding[axis];
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
  first.push_back({first.front().uv + winding, first.front().point});
  second.push_back({second.front().uv + oppositeWinding, second.front().point});
  const double sign = winding[axis] > 0 ? 1 : -1;
  for (const auto *ring : {&first, &second})
    for (size_t i = 1; i < ring->size(); ++i)
      if (sign * ((*ring)[i].uv[axis] - (*ring)[i - 1].uv[axis]) *
              (ring == &first ? 1 : -1) <=
          0)
        throw std::runtime_error("Periodic band bounds must be monotone in "
                                 "their cyclic parameter");
  bandFirstSize_ = first.size();
  bandProgress_[axis] = bandDirection_[axis] = 1;
  first.insert(first.end(), second.begin(), second.end());
  rings.resize(1);
}

std::vector<CurvedFaceTriangle>
Mesher::run(const std::vector<std::vector<glm::vec3>> &boundaries,
            bool explicitOuter, bool sameSense,
            std::span<const CurvedFaceChartLoop> authoredChart) {
  std::vector<std::vector<Vertex>> rings;
  std::vector<UV> winding;
  if (!authoredChart.empty() && authoredChart.size() != boundaries.size())
    throw std::runtime_error("Incomplete authored curved face chart");
  size_t samples = 0;
  for (size_t loopIndex = 0; loopIndex < boundaries.size(); ++loopIndex) {
    const auto &loop = boundaries[loopIndex];
    if (loop.size() < 3 || (samples += loop.size()) > 8192)
      throw std::runtime_error(
          "Invalid or excessive curved face boundary samples");
    const auto *authored =
        authoredChart.empty() ? nullptr : &authoredChart[loopIndex];
    if (authored && authored->vertices.size() != loop.size())
      throw std::runtime_error(
          "Authored chart does not match shared edge samples");
    previous_.reset();
    auto &ring = rings.emplace_back();
    for (size_t i = 0; i < loop.size(); ++i) {
      const auto p = loop[i];
      auto uv = authored ? authored->vertices[i] : inverse(Point(p));
      if (!authored && s_.spline)
        uv = snapProjectedKnots(uv, Point(p));
      if (authored && glm::length(evaluate(uv) - Point(p)) > tolerance_)
        throw std::runtime_error(
            "Authored pcurve sample leaves its shared edge");
      if (!authored && !ring.empty())
        for (unsigned axis = 0; axis < 2; ++axis)
          if (s_.periods[axis])
            uv[axis] += std::round((ring.back().uv[axis] - uv[axis]) /
                                   s_.periods[axis]) *
                        s_.periods[axis];
      ring.push_back({uv, p});
    }
    auto closure = authored ? authored->closure : ring.front().uv;
    if (!authored)
      for (unsigned axis = 0; axis < 2; ++axis)
        if (s_.periods[axis])
          closure[axis] += std::round((ring.back().uv[axis] - closure[axis]) /
                                      s_.periods[axis]) *
                           s_.periods[axis];
    winding.push_back(closure - ring.front().uv);
  }
  const auto windingCoordinates = [&](UV w) {
    if (!authoredChart.empty())
      for (unsigned axis = 0; axis < 2; ++axis)
        if (s_.periods[axis])
          w[axis] /= s_.periods[axis];
    return w;
  };
  const auto winds = [&](UV w) {
    return glm::length(windingCoordinates(w)) > 1e-10;
  };
  if (!explicitOuter) {
    // Two oppositely wound noncontractible rings describe a periodic band.
    // Join existing samples with a possibly slanted cut in parameter space.
    // Its two sides differ by exactly one period and share exact 3D endpoints.
    // No real boundary is resampled independently of its adjacent cap.
    if (rings.size() != 2 || !winds(winding[0]) ||
        glm::length(windingCoordinates(winding[0] + winding[1])) >
            1e-8 * glm::length(windingCoordinates(winding[0])))
      throw std::runtime_error(
          "Closed curved face requires two opposite periodic band bounds");
    if (!authoredChart.empty() && s_.periods.x && s_.periods.y) {
      joinSplineBand(rings, winding[0], sameSense);
    } else {
      joinAxialBand(rings, winding[0], winding[1], sameSense);
    }
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
      for (unsigned axis = 0; axis < 2 && authoredChart.empty(); ++axis)
        if (s_.periods[axis])
          shift[axis] =
              std::round((center[axis] - inner[axis]) / s_.periods[axis]) *
              s_.periods[axis];
      for (auto &v : rings[i])
        v.uv += shift;
    }
  }
  triangulate(rings, sameSense, explicitOuter && !authoredChart.empty());
  refine(sameSense);
  const auto normal = [&](UV uv, std::optional<UV> interior = {}) {
    const auto [x, y] = derivatives(uv, interior);
    const auto n = glm::cross(x, y);
    if (glm::length(n) <= glm::length(x) * glm::length(y) * 1e-10)
      throw std::runtime_error(
          "Curved face contains a singular boundary parameter");
    return glm::vec3(glm::normalize(n) * (sameSense ? 1. : -1.));
  };
  std::vector<std::optional<glm::vec3>> normals(vertices_.size());
  std::vector<CurvedFaceTriangle> result;
  for (size_t i = 0; i < triangles_.size(); ++i)
    if (active_[i]) {
      const auto &t = triangles_[i];
      const auto center =
          (vertices_[t[0]].uv + vertices_[t[1]].uv + vertices_[t[2]].uv) / 3.;
      std::array<glm::vec3, 3> triangleNormals;
      for (unsigned j = 0; j < 3; ++j) {
        const auto v = t[j];
        const auto uv = vertices_[v].uv;
        if (onCrease(uv.x, 0) || onCrease(uv.y, 1))
          triangleNormals[j] = normal(uv, center);
        else {
          if (!normals[v])
            normals[v] = normal(uv);
          triangleNormals[j] = *normals[v];
        }
      }
      result.push_back({{vertices_[t[0]].point, vertices_[t[1]].point,
                         vertices_[t[2]].point},
                        triangleNormals});
    }
  return result;
}

ParametricSurface sphericalChart(const SphericalSurface &sphere,
                                 const CurveFrame &axes, double tolerance) {
  const auto origin = sphere.frame.origin;
  const double radius = sphere.radius;
  const auto x = axes.x, y = axes.y, z = axes.z;
  ParametricSurface chart;
  // Project from the antipode of z. The right-handed basis keeps the
  // parameter normal outward, regardless of the sphere's authored pole.
  chart.point = [=](double u, double v) -> std::optional<Point> {
    const double squared = u * u + v * v, denominator = 1 + squared;
    if (!std::isfinite(denominator))
      return std::nullopt;
    return origin +
           (radius / denominator) * (2 * u * x + 2 * v * y + (1 - squared) * z);
  };
  chart.inverse = [=](Point p) -> std::optional<UV> {
    const auto d = p - origin;
    const double denominator = radius + glm::dot(d, z);
    if (std::abs(glm::length(d) - radius) > tolerance ||
        denominator <= radius * 1e-12)
      return std::nullopt;
    return UV(glm::dot(d, x), glm::dot(d, y)) / denominator;
  };
  return chart;
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
    return sphericalChart(
        sphere, {f.origin, f.x, double(sign) * f.y, double(sign) * f.z},
        tolerance);
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

bool meetsSphericalPole(const SphericalSurface &sphere,
                        const std::vector<std::vector<glm::vec3>> &loops,
                        double units) {
  const auto &f = sphere.frame;
  const double r = sphere.radius;
  const double tolerance = std::max(.0005 / units, r * 4e-7);
  for (const auto &loop : loops)
    for (size_t i = 0; i < loop.size(); ++i) {
      const Point a(loop[i]), b(loop[(i + 1) % loop.size()]);
      for (double sign : {-1., 1.})
        if (segmentDistanceSquared(f.origin + sign * r * f.z, a, b) <=
            tolerance * tolerance)
          return true;
    }
  return false;
}

std::vector<CurvedFaceTriangle>
meshSphericalEdgeLoops(const SphericalSurface &sphere,
                       const std::vector<std::vector<glm::vec3>> &boundaries,
                       bool explicitOuter, bool sameSense, double units,
                       std::optional<Point> sectorCenter = {}) {
  const auto &f = sphere.frame;
  const double radius = sphere.radius;
  const double tolerance = std::max(1e-7 / units, radius * 4e-7);
  const double sense = sameSense ? 1. : -1.;
  std::vector<std::vector<Point>> directions;
  std::vector<Point> centers{f.x, -f.x, f.y, -f.y, f.z, -f.z};
  if (sectorCenter)
    centers.push_back(*sectorCenter);
  size_t samples = 0;
  for (const auto &loop : boundaries) {
    if (loop.size() < 3 || (samples += loop.size()) > 8192)
      throw std::runtime_error("Invalid spherical edge-loop sample count");
    auto &ring = directions.emplace_back();
    ring.reserve(loop.size());
    for (auto p : loop) {
      const auto relative = Point(p) - f.origin;
      const auto length = glm::length(relative);
      if (!std::isfinite(length) || length <= 0 ||
          std::abs(length - radius) > tolerance)
        throw std::runtime_error("Spherical edge loop leaves its face surface");
      ring.push_back(relative / length);
    }
  }
  // Try the source axes and finite projection centers derived from each trim.
  // A step to the exterior of an oriented edge proposes an excluded point.
  // This also finds charts for faces larger than a hemisphere.
  for (const auto &ring : directions) {
    Point area(0);
    for (size_t i = 0; i < ring.size(); ++i)
      area += glm::cross(ring[i], ring[(i + 1) % ring.size()]);
    if (glm::length(area) > 1e-10) {
      const auto normal = glm::normalize(area);
      centers.push_back(normal);
      centers.push_back(-normal);
    }
    const size_t probes = std::min(size_t(12), ring.size());
    for (size_t i = 0; i < probes; ++i) {
      const size_t edgeIndex = i * ring.size() / probes;
      const auto &a = ring[edgeIndex], &b = ring[(edgeIndex + 1) % ring.size()];
      const auto midpoint = a + b;
      const auto inside = glm::cross(a, b);
      if (glm::length(midpoint) <= 1e-10 || glm::length(inside) <= 1e-10)
        continue;
      for (double offset : {.03, .15, .5})
        centers.push_back(
            glm::normalize(sense * offset * glm::normalize(inside) -
                           glm::normalize(midpoint)));
    }
    if (centers.size() >= 128)
      break;
  }
  struct Choice {
    CurveFrame axes;
    size_t outer;
    double clearance;
  };
  std::vector<Choice> choices;
  for (const auto z : centers) {
    const auto reference = std::abs(glm::dot(z, f.x)) < .8 ? f.x : f.y;
    const auto y = glm::normalize(glm::cross(z, reference));
    const CurveFrame axes{f.origin, glm::cross(y, z), y, z};
    double clearance = 4.;
    size_t outer = boundaries.size(), positive = 0;
    bool valid = true;
    for (size_t j = 0; j < directions.size() && valid; ++j) {
      const auto &ring = directions[j];
      double area = 0.;
      std::optional<UV> previous;
      UV first(0);
      for (size_t i = 0; i < ring.size(); ++i) {
        const auto &p = ring[i];
        const double denominator = 1 + glm::dot(p, z);
        const auto &next = ring[(i + 1) % ring.size()];
        clearance = std::min(clearance, segmentDistanceSquared(-z, p, next));
        if (denominator <= 1e-5 || clearance <= 2e-5) {
          valid = false;
          break;
        }
        const UV uv =
            UV(glm::dot(p, axes.x), glm::dot(p, axes.y)) / denominator;
        if (previous)
          area += cross(*previous, uv);
        else
          first = uv;
        previous = uv;
      }
      if (!valid)
        break;
      area += cross(*previous, first);
      if (std::abs(area) <= 1e-12)
        valid = false;
      else if (area * sense > 0) {
        outer = j;
        ++positive;
      }
    }
    if (valid && positive == 1 && (!explicitOuter || outer == 0))
      choices.push_back({axes, outer, clearance});
  }
  std::ranges::stable_sort(choices, std::greater<>(), &Choice::clearance);
  if (choices.empty())
    throw std::runtime_error(
        "No spherical chart agrees with the oriented edge loops");
  std::vector<CurveFrame> tried;
  std::string reason = "no projection agrees with the oriented boundaries";
  for (const auto &choice : choices) {
    if (std::ranges::any_of(tried, [&](const CurveFrame &frame) {
          return glm::dot(frame.z, choice.axes.z) > 1 - 1e-10;
        }))
      continue;
    if (tried.size() == 6)
      break;
    tried.push_back(choice.axes);
    try {
      auto ordered = boundaries;
      std::swap(ordered.front(), ordered[choice.outer]);
      const auto chart = sphericalChart(sphere, choice.axes, tolerance);
      return Mesher(chart, units, 2 * radius).run(ordered, true, sameSense);
    } catch (const std::runtime_error &error) {
      reason = error.what();
    }
  }
  throw std::runtime_error("Spherical edge-loop chart failed: " + reason);
}

using FaceLoops = std::vector<std::vector<glm::vec3>>;
constexpr double polarTurn = 2 * std::numbers::pi;
using AzimuthInterval = std::pair<double, double>;

struct PolarAzimuth {
  UV seamDirection;
  double sense;

  double angle(UV radial) const {
    const double radians = std::atan2(sense * cross(seamDirection, radial),
                                       glm::dot(seamDirection, radial));
    return radians < 0 ? radians + polarTurn : radians;
  }
};

struct PolarHoleAngles {
  std::vector<AzimuthInterval> byHole, blocked;

  bool clears(double angle) const {
    const auto after = std::ranges::upper_bound(
        blocked, angle, {}, &AzimuthInterval::first);
    return after == blocked.begin() || (after - 1)->second < angle;
  }

  double oppositeCut() const {
    const double opposite = std::numbers::pi;
    if (clears(opposite))
      return opposite;
    double last = 0, best = 0;
    const auto consider = [&](double end) {
      if (end <= last)
        return;
      const double inset = std::min((end - last) / 4., opposite / 16.);
      const double angle = std::clamp(opposite, last + inset, end - inset);
      if (angle > last && angle < end &&
          (!best || std::abs(angle - opposite) < std::abs(best - opposite)))
        best = angle;
    };
    for (const auto &[begin, end] : blocked) {
      consider(begin);
      last = end;
    }
    consider(polarTurn);
    if (!best)
      throw std::runtime_error("No spherical meridian cut clears the holes");
    return best;
  }

  void append(const FaceLoops &boundaries, double cut,
              std::array<FaceLoops, 2> &pieces) const {
    for (size_t i = 0; i < byHole.size(); ++i)
      pieces[byHole[i].second < cut ? 0 : 1].push_back(boundaries[i + 1]);
  }
};

PolarHoleAngles polarHoleAngles(const FaceLoops &boundaries,
                               const auto &radialAt, PolarAzimuth azimuth,
                               double clearance) {
  PolarHoleAngles result;
  for (const auto &hole : std::span(boundaries).subspan(1)) {
    if (hole.size() < 3)
      throw std::runtime_error("Invalid polar seam hole");
    AzimuthInterval span{polarTurn, 0.};
    auto a = radialAt(hole.back());
    for (const auto &point : hole) {
      const auto b = radialAt(point), delta = b - a;
      const double squared = glm::dot(delta, delta);
      const double t = squared > 0 ? std::clamp(-glm::dot(a, delta) / squared,
                                                0., 1.) : 0.;
      const auto nearest = a + t * delta;
      if (!(glm::length(nearest) > clearance))
        throw std::runtime_error("Polar seam hole is too close to its axis");

      // The chord and its source error lie in the convex hull of the two
      // endpoint disks. Their angles bound that capsule from the polar axis.
      const double center = azimuth.angle(nearest);
      for (const auto endpoint : {a, b}) {
        const double angle = center + std::remainder(
            azimuth.angle(endpoint) - center, polarTurn);
        const double margin = std::asin(clearance / glm::length(endpoint));
        span.first = std::min(span.first, angle - margin);
        span.second = std::max(span.second, angle + margin);
      }
      if (!(span.first > 0 && span.second < polarTurn))
        throw std::runtime_error("Authored polar seam meets a hole");
      a = b;
    }
    result.byHole.push_back(span);
  }
  auto ordered = result.byHole;
  std::ranges::sort(ordered);
  for (const auto &span : ordered) {
    if (result.blocked.empty() || result.blocked.back().second < span.first)
      result.blocked.push_back(span);
    else
      result.blocked.back().second = std::max(result.blocked.back().second,
                                               span.second);
  }
  return result;
}

struct PolarSeamParts {
  std::array<FaceLoops, 2> pieces;
  double cutAngle;
};

PolarSeamParts partitionPolarSeam(const FaceLoops &boundaries,
                                  std::span<const glm::vec3> seam,
                                  double direction, double units,
                                  double tolerance, const auto &radialAt,
                                  const auto &cutAt) {
  const auto &rim = boundaries.front();
  const auto seamEnd = std::ranges::find(rim, seam.back());
  if (rim.size() < 3 || seamEnd == rim.end())
    throw std::runtime_error("Polar seam must end at an exact rim sample");
  std::vector<glm::vec3> rimFromSeam(rim);
  std::rotate(rimFromSeam.begin(),
              rimFromSeam.begin() + (seamEnd - rim.begin()), rimFromSeam.end());
  const auto seamRadial = radialAt(seam.back());
  if (!(glm::length(seamRadial) > tolerance))
    throw std::runtime_error("Polar seam rim meets its axis");
  const PolarAzimuth azimuth{glm::normalize(seamRadial), direction};
  const auto holes = polarHoleAngles(boundaries, radialAt, azimuth,
                                      .0005 / units + tolerance);
  size_t cutIndex = 0;
  double cutAngle = 0, previous = 0;
  // This cut requires a rim with one radial intersection per meridian.
  // Use an existing rim sample so the adjoining face keeps its exact boundary.
  for (size_t i = 1; i < rimFromSeam.size(); ++i) {
    const auto radial = radialAt(rimFromSeam[i]);
    if (!(glm::length(radial) > tolerance))
      throw std::runtime_error("Polar seam rim meets its axis");
    const double angle = azimuth.angle(radial);
    if (angle <= previous || angle - previous >= std::numbers::pi)
      throw std::runtime_error("Polar seam rim must wind once monotonically");
    if (holes.clears(angle) &&
        (!cutIndex || std::abs(angle - std::numbers::pi) <
                          std::abs(cutAngle - std::numbers::pi))) {
      cutIndex = i;
      cutAngle = angle;
    }
    previous = angle;
  }
  if (previous <= std::numbers::pi)
    throw std::runtime_error("Polar seam rim must wind once monotonically");
  if (!cutIndex)
    throw std::runtime_error("No polar meridian through a rim sample clears the holes");
  const auto meridian = cutAt(rimFromSeam[cutIndex]);
  const auto samples = sampleCurve(meridian, 0, 1, .0005 / units, 4097);
  if (samples.size() < 2)
    throw std::runtime_error("Polar rim cut exceeds its sample budget");
  std::vector<glm::vec3> cut;
  cut.reserve(samples.size());
  for (const auto &sample : samples)
    cut.emplace_back(sample.point);
  cut.front() = seam.front();
  cut.back() = rimFromSeam[cutIndex];
  std::array<FaceLoops, 2> pieces;
  auto &first = pieces[0].emplace_back(seam.begin(), seam.end());
  first.insert(first.end(), rimFromSeam.begin() + 1,
               rimFromSeam.begin() + cutIndex + 1);
  first.insert(first.end(), cut.rbegin() + 1, cut.rend() - 1);
  auto &second = pieces[1].emplace_back(cut.begin(), cut.end());
  second.insert(second.end(), rimFromSeam.begin() + cutIndex + 1,
                rimFromSeam.end());
  second.insert(second.end(), seam.rbegin(), seam.rend() - 1);
  holes.append(boundaries, cutAngle, pieces);
  return {std::move(pieces), cutAngle};
}

UV sphericalRadial(const CurveFrame &frame, glm::vec3 position) {
  const auto p = Point(position) - frame.origin;
  return {glm::dot(p, frame.x), glm::dot(p, frame.y)};
}

Point polarDirection(const CurveFrame &frame, PolarAzimuth azimuth, double angle) {
  const auto radial = azimuth.seamDirection;
  const double c = std::cos(angle), s = azimuth.sense * std::sin(angle);
  return (c * radial.x - s * radial.y) * frame.x +
         (s * radial.x + c * radial.y) * frame.y;
}

std::vector<CurvedFaceTriangle>
meshSphericalSectors(const SphericalSurface &sphere, const PolarSeamParts &parts,
                      PolarAzimuth azimuth, bool sameSense, double units) {
  // The antipode of a sector's angular midpoint lies outside that sector,
  // including when it is wider than a hemisphere.
  const auto center = polarDirection(sphere.frame, azimuth, parts.cutAngle / 2.);
  auto result = meshSphericalEdgeLoops(sphere, parts.pieces[0], true, sameSense,
                                       units, center);
  auto remainder = meshSphericalEdgeLoops(sphere, parts.pieces[1], true,
                                          sameSense, units, -center);
  result.insert(result.end(), remainder.begin(), remainder.end());
  return result;
}

std::vector<CurvedFaceTriangle>
meshTrimmedSphericalSeam(const SphericalSurface &sphere,
                         const FaceLoops &boundaries,
                         std::span<const glm::vec3> seam, bool sameSense,
                         double units, double sign, double tolerance) {
  const auto &f = sphere.frame;
  const double radius = sphere.radius;
  const auto polarAxis = sign * f.z;
  const double direction = (sameSense ? 1. : -1.) * sign;
  const auto radialAt = [&](glm::vec3 position) {
    return sphericalRadial(f, position);
  };
  const auto cutAt = [&](glm::vec3 position) {
    const auto direction = glm::normalize(Point(position) - f.origin);
    const double cosine = std::clamp(glm::dot(polarAxis, direction), -1., 1.);
    const auto radial = glm::normalize(direction - cosine * polarAxis);
    const double end = std::acos(cosine);
    ParametricCurve meridian;
    meridian.domain = {{0, 1}};
    meridian.point = [=](double t) -> std::optional<Point> {
      return f.origin + radius * (std::cos(t * end) * polarAxis +
                                  std::sin(t * end) * radial);
    };
    return meridian;
  };
  const auto parts = partitionPolarSeam(boundaries, seam, direction, units,
                                         tolerance, radialAt, cutAt);
  const PolarAzimuth azimuth{glm::normalize(radialAt(seam.back())), direction};
  return meshSphericalSectors(sphere, parts, azimuth, sameSense, units);
}

std::vector<CurvedFaceTriangle>
meshSphericalSeam(const SphericalSurface &sphere,
                  const std::vector<std::vector<glm::vec3>> &boundaries,
                  std::span<const glm::vec3> seam, bool sameSense,
                  double units) {
  const auto &f = sphere.frame;
  const double radius = sphere.radius;
  const double tolerance = std::max(1e-7 / units, radius * 4e-7);
  if (seam.size() < 3 || seam.size() > 4097)
    throw std::runtime_error("Invalid spherical seam sample count");
  const double sign =
      glm::dot(Point(seam.front()) - f.origin, f.z) > 0 ? 1 : -1;
  if (glm::length(Point(seam.front()) - f.origin - sign * radius * f.z) >
      tolerance)
    throw std::runtime_error("Spherical seam must start at a surface pole");
  if (glm::length(Point(seam.back()) - f.origin + sign * radius * f.z) >
      tolerance)
    return meshTrimmedSphericalSeam(sphere, boundaries, seam, sameSense, units,
                                    sign, tolerance);
  const PolarAzimuth azimuth{{1., 0.}, sign * (sameSense ? 1. : -1.)};
  const auto radialAt = [&](glm::vec3 position) {
    return sphericalRadial(f, position);
  };
  const auto holes = polarHoleAngles(boundaries, radialAt, azimuth,
                                      .0005 / units + tolerance);
  const auto cut = holes.oppositeCut();
  const auto radial = cut == std::numbers::pi ? -f.x : polarDirection(f, azimuth, cut);
  ParametricCurve meridian;
  meridian.domain = {{0, std::numbers::pi}};
  meridian.point = [=](double t) -> std::optional<Point> {
    return f.origin + radius * (std::sin(t) * radial - sign * std::cos(t) * f.z);
  };
  const auto samples =
      sampleCurve(meridian, 0, std::numbers::pi, .0005 / units, 4097);
  if (samples.size() < 3)
    throw std::runtime_error(
        "Spherical seam partition exceeds its sample budget");
  std::vector<glm::vec3> ring(seam.begin(), seam.end());
  for (size_t i = 1; i + 1 < samples.size(); ++i)
    ring.emplace_back(samples[i].point);
  PolarSeamParts parts{{}, cut};
  parts.pieces[1].emplace_back(ring.rbegin(), ring.rend());
  parts.pieces[0].push_back(std::move(ring));
  holes.append(boundaries, cut, parts.pieces);
  return meshSphericalSectors(sphere, parts, azimuth, sameSense, units);
}

std::vector<CurvedFaceTriangle>
meshRevolvedRadialGraph(const RevolvedRadialGraph &graph,
                        const FaceLoops &boundaries, bool explicitOuter,
                        bool sameSense, double units,
                        std::span<const glm::vec3> seam) {
  const double extent = graph.maxRadius * 2;
  const double tolerance = std::max(1e-7 / units, extent * 2e-7);
  ParametricSurface chart;
  chart.point = graph.point;
  chart.inverse = graph.inverse;
  chart.derivatives = graph.derivatives;
  if (!seam.empty()) {
    if (seam.size() < 2 || seam.size() > 4097 ||
        glm::length(Point(seam.front()) - graph.frame.origin) > tolerance)
      throw std::runtime_error("Revolved seam must start at its smooth apex");
    const auto radialAt = [&](glm::vec3 position) {
      const auto uv = graph.inverse(Point(position));
      if (!uv)
        throw std::runtime_error("Revolved seam rim or hole leaves its surface");
      return *uv;
    };
    const auto cutAt = [&](glm::vec3 position) {
      const auto uv = graph.inverse(Point(position));
      if (!uv)
        throw std::runtime_error("Revolved rim cut leaves its surface");
      ParametricCurve meridian;
      meridian.domain = {{0, 1}};
      meridian.point = [point = graph.point, end = *uv](double t) {
        return point(t * end.x, t * end.y);
      };
      return meridian;
    };
    const auto parts = partitionPolarSeam(boundaries, seam, sameSense ? 1. : -1.,
                                          units, tolerance, radialAt, cutAt);
    auto result = Mesher(chart, units, extent).run(parts.pieces[0], true, sameSense);
    auto remainder =
        Mesher(chart, units, extent).run(parts.pieces[1], true, sameSense);
    result.insert(result.end(), remainder.begin(), remainder.end());
    return result;
  }
  FaceLoops rings;
  InteriorPoint apex{glm::vec3(graph.frame.origin), false};
  for (const auto &boundary : boundaries) {
    if (boundary.size() != 1) {
      rings.push_back(boundary);
      continue;
    }
    if (apex.required || (explicitOuter && rings.empty()) ||
        glm::length(Point(boundary.front()) - graph.frame.origin) > tolerance)
      throw std::runtime_error(
          "Revolved vertex loop must be the single smooth apex inside a rim");
    apex = {boundary.front(), true};
  }
  if (rings.empty())
    throw std::runtime_error("Revolved apex face requires an edge-loop rim");
  if (!explicitOuter && rings.size() > 1)
    throw std::runtime_error(
        "Revolved apex face with holes requires an outer bound");
  return Mesher(chart, units, extent, {}, apex).run(rings, true, sameSense);
}
void requireSplineEdgeProjection(const ParametricSurface &surface,
                                 size_t samples, double units) {
  const double chord = .001 / units;
  if (!surface.spline || !surface.point || !std::isfinite(units) ||
      units <= 0 || !std::isfinite(chord * chord) || chord * chord == 0 ||
      samples < 2 || samples > 4097)
    throw std::runtime_error("Invalid spline surface or shared edge samples");
  for (unsigned axis = 0; axis < 2; ++axis) {
    const auto &domain = axis ? surface.vDomain : surface.uDomain;
    const double span = domain ? domain->second - domain->first : 0.;
    const double period = surface.periods[axis];
    if (!std::isfinite(span) || span <= 0 || !std::isfinite(period) ||
        period < 0 || (period > 0 && std::abs(period / span - 1.) > 1e-8))
      throw std::runtime_error("Invalid native spline domain or period");
  }
}
} // namespace

std::vector<glm::dvec2>
projectIfcBrepSplineEdge(const ParametricSurface &surface,
                         std::span<const glm::vec3> sharedPoints, double units,
                         std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  requireSplineEdgeProjection(surface, sharedPoints.size(), units);

  const auto origin = Point(sharedPoints.front());
  const bool closed = sharedPoints.front() == sharedPoints.back();
  const size_t uniqueCount = sharedPoints.size() - (closed ? 1 : 0);
  if (closed && uniqueCount < 3)
    throw std::runtime_error("Closed inferred spline edge has too few samples");
  std::set<std::array<float, 3>> unique;
  double extent = 0;
  for (const auto p : sharedPoints.first(uniqueCount)) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      throw std::runtime_error("Inferred spline edge has a nonfinite sample");
    if (!unique.emplace(std::array{p.x, p.y, p.z}).second)
      throw std::runtime_error("Inferred spline edge repeats a shared sample");
    extent = std::max(extent, glm::length(Point(p) - origin));
  }
  return Mesher(surface, units, extent).projectEdge(sharedPoints, stop);
}

std::vector<CurveSample> sampleIfcBrepSplineEdgeCreases(
    const ParametricSurface &surface, const ParametricCurve &source,
    std::span<const CurveSample> samples, double units, std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  if (surface.creaseU.empty() && surface.creaseV.empty())
    return {};
  requireSplineEdgeProjection(surface, samples.size(), units);
  if (!source.point || source.dimension != 3)
    throw std::runtime_error(
        "Expected a three-dimensional native spline edge witness");
  const auto finiteKnot = [](double knot) { return std::isfinite(knot); };
  if (!std::ranges::all_of(surface.creaseU, finiteKnot) ||
      !std::ranges::all_of(surface.creaseV, finiteKnot))
    throw std::runtime_error("Nonfinite spline surface crease parameter");
  const auto origin = samples.front().point;
  const bool forward = samples.front().parameter < samples.back().parameter;
  double extent = 0;
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto &sample = samples[i];
    const auto p = sample.point - origin;
    if (!std::isfinite(sample.parameter) || !std::isfinite(p.x) ||
        !std::isfinite(p.y) || !std::isfinite(p.z) ||
        (i > 0 && !(forward ? sample.parameter > samples[i - 1].parameter
                            : sample.parameter < samples[i - 1].parameter)))
      throw std::runtime_error(
          "Invalid or unordered native spline edge witness samples");
    extent = std::max(extent, glm::length(p));
  }
  if (!std::isfinite(extent) || extent <= 0)
    throw std::runtime_error("Collapsed native spline edge witness");
  return Mesher(surface, units, extent)
      .sampleEdgeCreases(source, samples, stop);
}

std::optional<std::vector<CurvedFaceTriangle>>
meshIfcCurvedFace(const ParametricSurface &surface,
                  const std::vector<std::vector<glm::vec3>> &boundaries,
                  bool explicitOuter, bool sameSense, double units,
                  std::string &error, std::span<const glm::vec3> polarSeam,
                  std::span<const CurvedFaceChartLoop> authoredChart) {
  try {
    if (boundaries.empty() || !std::isfinite(units) || units <= 0 ||
        !surface.point)
      throw std::runtime_error(
          "Missing curved face boundaries, surface or length units");
    double extent = 0;
    if (boundaries.front().empty())
      throw std::runtime_error("Empty curved face outer loop");
    if (surface.radialGraph)
      return meshRevolvedRadialGraph(*surface.radialGraph, boundaries,
                                     explicitOuter, sameSense, units,
                                     polarSeam);
    if (!surface.radialGraphError.empty())
      throw std::runtime_error(surface.radialGraphError);
    if (!polarSeam.empty()) {
      if (!surface.sphere)
        throw std::runtime_error("Spherical seam requires a spherical face");
      return meshSphericalSeam(*surface.sphere, boundaries, polarSeam,
                               sameSense, units);
    }
    if (std::ranges::any_of(
            boundaries, [](const auto &loop) { return loop.size() == 1; })) {
      if (!surface.sphere)
        throw std::runtime_error("Unsupported vertex-loop surface chart");
      return meshSphericalPoleFace(surface, boundaries, explicitOuter,
                                   sameSense, units);
    }
    // A single ordinary bound on a closed sphere can enclose either side of
    // its rim. The oriented stereographic chart selects the enclosed region.
    if (surface.sphere &&
        ((!explicitOuter && boundaries.size() == 1) ||
         meetsSphericalPole(*surface.sphere, boundaries, units)))
      return meshSphericalEdgeLoops(*surface.sphere, boundaries, explicitOuter,
                                    sameSense, units);
    const Point origin(boundaries.front().front());
    for (const auto &loop : boundaries)
      for (auto p : loop)
        extent = std::max(extent, glm::length(Point(p) - origin));
    return Mesher(surface, units, extent)
        .run(boundaries, explicitOuter, sameSense, authoredChart);
  } catch (const std::runtime_error &e) {
    error = e.what();
    return std::nullopt;
  }
}
} // namespace container::geometry::ifc::detail
