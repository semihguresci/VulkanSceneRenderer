#include "IfcSweptSurfaceMesh.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

#include <glm/geometric.hpp>

namespace container::geometry::ifc::detail {
namespace {

constexpr size_t maxAxisPoints = 4097;
constexpr size_t maxTriangles = 200000;
constexpr size_t maxEvaluations = 4000000;

bool finite(glm::dvec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

double segmentDistanceSquared(glm::dvec3 p, glm::dvec3 a, glm::dvec3 b) {
  const auto edge = b - a;
  const double lengthSquared = glm::dot(edge, edge);
  const double t =
      lengthSquared > 0
          ? std::clamp(glm::dot(p - a, edge) / lengthSquared, 0., 1.)
          : 0;
  const auto delta = p - a - t * edge;
  return glm::dot(delta, delta);
}

double triangleDistanceSquared(glm::dvec3 p, glm::dvec3 a, glm::dvec3 b,
                               glm::dvec3 c) {
  const auto normal = glm::cross(b - a, c - a);
  const double areaSquared = glm::dot(normal, normal);
  if (areaSquared > 0) {
    const double height = glm::dot(p - a, normal);
    const auto projected = p - normal * (height / areaSquared);
    if (glm::dot(glm::cross(b - a, projected - a), normal) >= 0 &&
        glm::dot(glm::cross(c - b, projected - b), normal) >= 0 &&
        glm::dot(glm::cross(a - c, projected - c), normal) >= 0)
      return height * height / areaSquared;
  }
  return std::min({segmentDistanceSquared(p, a, b),
                   segmentDistanceSquared(p, b, c),
                   segmentDistanceSquared(p, c, a)});
}

using Domain = std::pair<double, double>;

std::vector<double> seedAxis(std::optional<Domain> domain,
                             std::span<const double> breaks, double period,
                             double radiansPerParameter) {
  if (!domain && period > 0)
    domain = Domain{0, period};
  if (!domain || !std::isfinite(domain->first) ||
      !std::isfinite(domain->second) || domain->second <= domain->first)
    throw std::runtime_error("Swept surface requires a finite mesh domain");
  const auto [low, high] = *domain;
  const double length = high - low;
  if (!(std::isfinite(length) && std::isfinite(radiansPerParameter)) ||
      breaks.size() >= maxAxisPoints)
    throw std::runtime_error("Swept surface exceeds its parameter grid budget");

  std::vector<double> values{low, high};
  const double angularSegments = std::ceil(
      length * std::abs(radiansPerParameter) / (std::numbers::pi / 2));
  if (angularSegments >= maxAxisPoints)
    throw std::runtime_error("Swept surface exceeds its angular grid budget");
  const auto regularSegments = static_cast<size_t>(angularSegments);
  for (size_t i = 1; i < regularSegments; ++i)
    values.push_back(std::lerp(low, high, double(i) / regularSegments));
  for (double knot : breaks)
    if (knot > low && knot < high)
      values.push_back(knot);
  std::ranges::sort(values);
  const auto duplicate = std::ranges::unique(values);
  values.erase(duplicate.begin(), duplicate.end());
  return values;
}

struct Cell {
  double u0, u1, v0, v1;
};

struct GridMesher {
  const ParametricSurface &surface;
  double chordSquared;
  const std::stop_token &stop;
  size_t &evaluations;
  std::optional<Domain> seamU, seamV;

  bool atApex(double v) const {
    return surface.standaloneApex && v == surface.standaloneApex->v;
  }

  glm::dvec3 evaluate(double u, double v) const {
    if (++evaluations > maxEvaluations)
      throw std::runtime_error("Swept surface exceeds its evaluation budget");
    if (evaluations % 1024 == 0)
      container::util::checkModelImportCancellation(stop);
    // Every angular sample denotes the same point at a certified axis endpoint.
    if (atApex(v))
      return surface.standaloneApex->point;
    const auto p =
        surface.point(seamU && u == seamU->second ? seamU->first : u,
                      seamV && v == seamV->second ? seamV->first : v);
    if (!p || !finite(*p))
      throw std::runtime_error("Undefined swept surface mesh point");
    return *p;
  }

  std::array<glm::dvec3, 4> corners(Cell cell) const {
    return {evaluate(cell.u0, cell.v0), evaluate(cell.u1, cell.v0),
            evaluate(cell.u1, cell.v1), evaluate(cell.u0, cell.v1)};
  }

  std::pair<bool, bool> needsSubdivision(Cell cell) const {
    const auto [a, b, c, d] = corners(cell);
    bool splitU = false, splitV = false;
    for (double t : {.25, .5, .75}) {
      const double u = std::lerp(cell.u0, cell.u1, t);
      const double v = std::lerp(cell.v0, cell.v1, t);
      splitU |=
          segmentDistanceSquared(evaluate(u, cell.v0), a, b) > chordSquared;
      splitU |=
          segmentDistanceSquared(evaluate(u, cell.v1), d, c) > chordSquared;
      splitV |=
          segmentDistanceSquared(evaluate(cell.u0, v), a, d) > chordSquared;
      splitV |=
          segmentDistanceSquared(evaluate(cell.u1, v), b, c) > chordSquared;
    }
    if (splitU || splitV)
      return {splitU, splitV};

    // Interior samples detect chord error inside twisted quadrilaterals.
    for (double x : {.25, .5, .75})
      for (double y : {.25, .5, .75}) {
        const auto p = evaluate(std::lerp(cell.u0, cell.u1, x),
                                std::lerp(cell.v0, cell.v1, y));
        if (std::min(triangleDistanceSquared(p, a, b, c),
                     triangleDistanceSquared(p, a, c, d)) > chordSquared)
          return {true, true};
      }
    return {false, false};
  }

  glm::dvec3 normalAt(double u, double v, double insideU, double insideV,
                      glm::dvec3 point) const {
    if (atApex(v))
      return surface.standaloneApex->normal;
    // A one-sided stencil remains in the cell. At a profile knot the
    // neighboring cell can therefore retain its own normal at the same point.
    const double du = (insideU - u) * 1e-4, dv = (insideV - v) * 1e-4;
    const auto tangentU = (4. * (evaluate(u + du, v) - point) -
                           (evaluate(u + 2 * du, v) - point)) /
                          du;
    const auto tangentV = (4. * (evaluate(u, v + dv) - point) -
                           (evaluate(u, v + 2 * dv) - point)) /
                          dv;
    const auto normal = glm::cross(tangentU, tangentV);
    const double magnitude = glm::length(normal);
    if (!std::isfinite(magnitude) ||
        magnitude <= glm::length(tangentU) * glm::length(tangentV) * 1e-10)
      throw std::runtime_error("Swept surface has a singular mesh parameter");
    return normal / magnitude;
  }

  void appendCell(Cell cell, std::vector<CurvedFaceTriangle> &result) const {
    const auto points = corners(cell);
    const std::array normals{
        normalAt(cell.u0, cell.v0, cell.u1, cell.v1, points[0]),
        normalAt(cell.u1, cell.v0, cell.u0, cell.v1, points[1]),
        normalAt(cell.u1, cell.v1, cell.u0, cell.v0, points[2]),
        normalAt(cell.u0, cell.v1, cell.u1, cell.v0, points[3])};
    const auto appendTriangle = [&](std::array<size_t, 3> indices) {
      const auto normal = glm::cross(points[indices[1]] - points[indices[0]],
                                     points[indices[2]] - points[indices[0]]);
      CurvedFaceTriangle triangle;
      for (size_t i = 0; i < indices.size(); ++i) {
        const auto index = indices[i];
        if (!(glm::dot(normal, normals[index]) > 0))
          throw std::runtime_error("Swept surface mesh folds or collapses");
        triangle.positions[i] = glm::vec3(points[index]);
        triangle.normals[i] =
            glm::vec3(normals[index] * (surface.reverseMeshWinding ? -1. : 1.));
      }
      if (surface.reverseMeshWinding) {
        std::swap(triangle.positions[1], triangle.positions[2]);
        std::swap(triangle.normals[1], triangle.normals[2]);
      }
      result.push_back(triangle);
    };
    // Each cell adjoining the apex contributes one triangle to the fan.
    if (!atApex(cell.v0))
      appendTriangle({0, 1, 2});
    if (!atApex(cell.v1))
      appendTriangle({0, 2, 3});
  }
};

void insertMidpoints(std::vector<double> &knots,
                     std::span<const uint8_t> splits) {
  std::vector<double> refined;
  refined.reserve(knots.size() * 2);
  refined.push_back(knots.front());
  for (size_t i = 0; i < splits.size(); ++i) {
    const auto a = knots[i], b = knots[i + 1];
    if (splits[i]) {
      const auto midpoint = std::lerp(a, b, .5);
      if (!(midpoint > a && midpoint < b))
        throw std::runtime_error("Swept surface parameter precision exhausted");
      refined.push_back(midpoint);
    }
    refined.push_back(b);
  }
  knots = std::move(refined);
}

std::optional<Domain> closedAxis(std::span<const double> knots, double period) {
  if (period > 0 &&
      std::abs(knots.back() - knots.front() - period) <= period * 1e-12)
    return Domain{knots.front(), knots.back()};
  return std::nullopt;
}

void meshChart(GridMesher mesh, bool bareExtrusion,
               std::vector<CurvedFaceTriangle> &result) {
  const auto &surface = mesh.surface;
  if (!surface.standaloneRegular)
    throw std::runtime_error(
        surface.radialGraphError.empty()
            ? "Swept surface does not have a regular standalone chart"
            : surface.radialGraphError);
  auto u = seedAxis(surface.uDomain, surface.meshU, surface.periods.x,
                    surface.angularScale.x);
  // Depth scales V for analytic evaluation. Its finite display convention
  // applies only at the standalone mesh entry point.
  const auto vDomain =
      bareExtrusion ? std::optional(Domain{0, 1}) : surface.vDomain;
  auto v = seedAxis(vDomain, surface.meshV, surface.periods.y,
                    surface.angularScale.y);
  if (const auto &apex = surface.standaloneApex;
      apex && ((apex->v != v.front() && apex->v != v.back()) ||
               surface.periods.y != 0 || !finite(apex->point) ||
               !finite(apex->normal) ||
               glm::dot(apex->normal, apex->normal) <= 0))
    throw std::runtime_error("Invalid swept surface apex frame");
  // Reuse the exact same point at opposite ends of a full period so a
  // floating-point residual at sin(2*pi) cannot leave an unwelded seam.
  mesh.seamU = closedAxis(u, surface.periods.x);
  mesh.seamV = closedAxis(v, surface.periods.y);
  const auto cell = [&](size_t i, size_t j) {
    return Cell{u[i], u[i + 1], v[j], v[j + 1]};
  };
  for (unsigned pass = 0;; ++pass) {
    if (u.size() > maxAxisPoints || v.size() > maxAxisPoints ||
        (u.size() - 1) * (v.size() - 1) > (maxTriangles - result.size()) / 2)
      throw std::runtime_error(
          "Swept surface exceeds its triangle or grid budget");
    std::vector<uint8_t> splitU(u.size() - 1), splitV(v.size() - 1);
    bool refine = false;
    for (size_t i = 0; i < splitU.size(); ++i)
      for (size_t j = 0; j < splitV.size(); ++j) {
        const auto [a, b] = mesh.needsSubdivision(cell(i, j));
        splitU[i] = splitU[i] || a;
        splitV[j] = splitV[j] || b;
        refine |= a || b;
      }
    if (!refine)
      break;
    if (pass >= 16)
      throw std::runtime_error(
          "Swept surface could not resolve its chord error");
    insertMidpoints(u, splitU);
    insertMidpoints(v, splitV);
  }
  result.reserve(result.size() + (u.size() - 1) * (v.size() - 1) * 2);
  for (size_t i = 0; i + 1 < u.size(); ++i)
    for (size_t j = 0; j + 1 < v.size(); ++j)
      mesh.appendCell(cell(i, j), result);
}

} // namespace

std::optional<std::vector<CurvedFaceTriangle>>
meshIfcSweptSurfaces(std::span<const ParametricSurface> charts,
                     bool bareExtrusion, double metersPerUnit,
                     std::stop_token stop, std::string &error) {
  container::util::checkModelImportCancellation(stop);
  std::vector<CurvedFaceTriangle> result;
  size_t evaluations = 0;
  const double chord = .00025 / metersPerUnit;
  const double chordSquared = chord * chord;
  if (charts.empty() || !std::isfinite(chordSquared) || chordSquared <= 0) {
    error = "Swept surface has no mesh charts or an invalid length unit";
    return std::nullopt;
  }
  for (const auto &chart : charts) {
    try {
      meshChart({chart, chordSquared, stop, evaluations, {}, {}}, bareExtrusion,
                result);
    } catch (const std::runtime_error &e) {
      error = e.what();
      if (chart.profileSourceId)
        error += " at profile #" + std::to_string(chart.profileSourceId);
      return std::nullopt;
    }
  }
  return result;
}

} // namespace container::geometry::ifc::detail
