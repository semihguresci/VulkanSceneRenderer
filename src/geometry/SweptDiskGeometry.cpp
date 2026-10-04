#include "Container/geometry/SweptDiskGeometry.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace container::geometry {
namespace {
constexpr double pi = std::numbers::pi;
constexpr size_t maxFrames = 4096, maxTriangles = 1000000;
bool finite(glm::dvec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
double pointDistance(glm::dvec3 p, glm::dvec3 a, glm::dvec3 b) {
  const auto d = b - a;
  const double n = glm::dot(d, d);
  return glm::length(
      p - a - d * (n > 0 ? std::clamp(glm::dot(p - a, d) / n, 0., 1.) : 0));
}
double segmentDistance(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d) {
  double result = std::min({pointDistance(a, c, d), pointDistance(b, c, d),
                            pointDistance(c, a, b), pointDistance(d, a, b)});
  const auto u = b - a, v = d - c, w = a - c;
  const double aa = glm::dot(u, u), bb = glm::dot(u, v), cc = glm::dot(v, v),
               dd = glm::dot(u, w), ee = glm::dot(v, w),
               det = aa * cc - bb * bb;
  if (det > aa * cc * 1e-14) {
    const double s = (bb * ee - cc * dd) / det, t = (aa * ee - bb * dd) / det;
    if (s >= 0 && s <= 1 && t >= 0 && t <= 1)
      result = std::min(result, glm::length(w + s * u - t * v));
  }
  return result;
}
glm::dvec3 rotate(glm::dvec3 p, glm::dvec3 axis, double angle) {
  return p * std::cos(angle) + glm::cross(axis, p) * std::sin(angle) +
         axis * glm::dot(axis, p) * (1 - std::cos(angle));
}
struct Frame {
  double parameter, station;
  glm::dvec3 point, axis, x, y, miterNormal;
};
} // namespace

std::optional<ParametricCurve>
makeFilletedPolyline(std::vector<glm::dvec3> points, double radius,
                     std::string &diagnostic) {
  try {
    if (!std::isfinite(radius) || radius <= 0 || points.size() < 2 ||
        points.size() > 4097 ||
        !std::ranges::all_of(points, [](auto p) { return finite(p); }))
      throw std::runtime_error("Invalid polygonal fillet radius or points");
    const bool closed = points.front() == points.back();
    const size_t segments = points.size() - 1;
    if (closed && segments < 3)
      throw std::runtime_error("Closed polygonal sweep needs three segments");
    std::vector<double> lengths(segments), trims(points.size(), 0);
    for (size_t i = 0; i < segments; ++i) {
      lengths[i] = glm::length(points[i + 1] - points[i]);
      if (lengths[i] <= 1e-15 ||
          radius > lengths[i] *
                       ((!closed && (i == 0 || i + 1 == segments)) ? 1. : .5))
        throw std::runtime_error(
            "Fillet radius exceeds polygonal segment limit");
    }
    struct Arc {
      double first, last, angle;
      size_t corner;
      glm::dvec3 center, radial, tangent;
    };
    auto arcs = std::make_shared<std::vector<Arc>>();
    for (size_t i = closed ? 0 : 1; i < segments; ++i) {
      const size_t previous = (i + segments - 1) % segments;
      const auto incoming = (points[i] - points[previous]) / lengths[previous],
                 outgoing = (points[i + 1] - points[i]) / lengths[i];
      const double cosine = std::clamp(glm::dot(incoming, outgoing), -1., 1.);
      if (cosine > 1 - 1e-12)
        continue;
      if (cosine <= -1 + 1e-10)
        throw std::runtime_error("Polygonal fillet reverses direction");
      const double angle = std::acos(cosine),
                   trim = radius * std::tan(angle / 2);
      trims[i] = trim;
      const auto side = glm::normalize(outgoing - incoming * cosine);
      arcs->push_back({double(i) - trim / lengths[previous],
                       double(i) + trim / lengths[i], angle, i,
                       points[i] - incoming * trim + side * radius, -side,
                       incoming});
    }
    if (closed)
      trims.back() = trims.front();
    for (size_t i = 0; i < segments; ++i) {
      const double sum = trims[i] + trims[i + 1];
      if (sum > lengths[i] * (1 + 1e-12))
        throw std::runtime_error(
            "Polygonal fillets overlap on a short segment");
      if (std::abs(sum - lengths[i]) <= lengths[i] * 1e-12) {
        // Equal adjacent tangent extents meet without a residual straight
        // span. Give both arcs the exact same source parameter at that join.
        const double meeting = i + trims[i] / lengths[i];
        for (auto &arc : *arcs) {
          if (arc.corner == i)
            arc.last = meeting;
          if (arc.corner == (i + 1) % segments)
            arc.first =
                closed && arc.corner == 0 ? meeting - segments : meeting;
        }
      }
    }
    auto vertices =
        std::make_shared<std::vector<glm::dvec3>>(std::move(points));
    const auto evaluate =
        [vertices, arcs, radius, segments,
         closed](double t) -> std::optional<std::pair<glm::dvec3, glm::dvec3>> {
      if (!std::isfinite(t) || t < 0 || t > segments)
        return std::nullopt;
      if (closed && t == segments)
        t = 0;
      for (const auto &arc : *arcs) {
        double parameter = t;
        if (arc.first < 0 && parameter > arc.last)
          parameter -= segments;
        if (parameter < arc.first || parameter > arc.last)
          continue;
        const double angle =
            (parameter - arc.first) / (arc.last - arc.first) * arc.angle;
        return std::pair(arc.center + radius * (arc.radial * std::cos(angle) +
                                                arc.tangent * std::sin(angle)),
                         radius * arc.angle / (arc.last - arc.first) *
                             (-arc.radial * std::sin(angle) +
                              arc.tangent * std::cos(angle)));
      }
      const size_t i =
          std::min(t > 0 ? size_t(std::ceil(t) - 1) : 0, segments - 1);
      const auto d = (*vertices)[i + 1] - (*vertices)[i];
      return std::pair((*vertices)[i] + (t - i) * d, d);
    };
    ParametricCurve curve;
    curve.domain = {{0, double(segments)}};
    curve.point = [evaluate](double t) -> std::optional<glm::dvec3> {
      auto v = evaluate(t);
      return v ? std::optional(v->first) : std::nullopt;
    };
    curve.derivative = [evaluate](double t) -> std::optional<glm::dvec3> {
      auto v = evaluate(t);
      return v ? std::optional(v->second) : std::nullopt;
    };
    for (size_t i = 1; i < segments; ++i)
      if (trims[i] == 0)
        curve.breaks.push_back(double(i));
    for (const auto &arc : *arcs)
      for (double t : {arc.first, arc.last}) {
        if (t < 0)
          t += segments;
        if (t > 0 && t < segments)
          curve.breaks.push_back(t);
      }
    std::ranges::sort(curve.breaks);
    return curve;
  } catch (const std::runtime_error &e) {
    diagnostic = e.what();
    return std::nullopt;
  }
}

std::vector<std::array<glm::dvec3, 3>>
buildSweptDisk(const ParametricCurve &curve, double first, double last,
               double radius, double inner, double error,
               std::string &diagnostic) {
  try {
    if (!curve.point || curve.dimension != 3 || !std::isfinite(first) ||
        !std::isfinite(last) || first >= last || !std::isfinite(radius) ||
        radius <= 0 || !std::isfinite(inner) || inner < 0 || inner >= radius ||
        !std::isfinite(error) || error <= 0)
      throw std::runtime_error("Invalid swept disk extent, dimension or radii");
    if (curve.domain &&
        (first < curve.domain->first || last > curve.domain->second))
      throw std::runtime_error("Swept disk extent outside directrix domain");
    // Thin tubes need a relative bound as well as the physical chord target;
    // otherwise millimetre-scale directrices can hide overlaps between probes.
    error = std::min(error, radius * .05);
    const double tolerance = std::min(error * .01, radius * 1e-6);
    struct Join {
      glm::dvec3 incoming, outgoing, normal;
      double extent;
    };
    std::map<double, Join> joins;
    std::vector<double> boundaries{first, last};
    for (double t : curve.breaks)
      if (t > first && t < last)
        boundaries.push_back(t);
    std::ranges::sort(boundaries);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());
    const auto makeJoin = [&](double t, double before, double after,
                              double span) {
      const auto p = curve.point(before), q = curve.point(after);
      const auto l = curveDerivative(curve, before),
                 r = curveDerivative(curve, after);
      if (!p || !q || !l || !r || !finite(*l) || !finite(*r) ||
          glm::length(*l) <= 1e-15 || glm::length(*r) <= 1e-15)
        throw std::runtime_error("Undefined swept disk join tangent");
      if (glm::length(*p - *q) >
          tolerance + span * (glm::length(*l) + glm::length(*r)) * 1.1)
        throw std::runtime_error("Disconnected swept disk directrix join");
      const auto a = glm::normalize(*l), b = glm::normalize(*r);
      const double cosine = std::clamp(glm::dot(a, b), -1., 1.);
      if (cosine < -.5)
        throw std::runtime_error("Swept disk miter angle exceeds 120 degrees");
      if (cosine < 1 - 1e-6)
        joins[t] = {a, b, glm::normalize(a + b),
                    radius * std::sqrt((1 - cosine) / (1 + cosine))};
    };
    for (size_t i = 1; i + 1 < boundaries.size(); ++i) {
      const double t = boundaries[i],
                   h = std::min(t - boundaries[i - 1], boundaries[i + 1] - t) *
                       1e-8;
      makeJoin(t, t - h, t + h, h);
    }
    const auto beginning = curve.point(first), ending = curve.point(last);
    const bool closed =
        beginning && ending && glm::length(*beginning - *ending) <= tolerance;
    if (closed) {
      const double h = std::min(boundaries[1] - first,
                                last - boundaries[boundaries.size() - 2]) *
                       1e-8;
      makeJoin(first, last - h, first + h, h);
      if (joins.contains(first))
        joins[last] = joins.at(first);
    }
    if (!joins.empty()) {
      std::vector<double> corners{first, last};
      for (const auto &[t, join] : joins)
        corners.push_back(t);
      std::ranges::sort(corners);
      corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
      for (size_t i = 1; i < corners.size(); ++i) {
        const auto length =
            curveLength(curve, corners[i - 1], corners[i], tolerance);
        const double a = joins.contains(corners[i - 1])
                             ? joins.at(corners[i - 1]).extent
                             : 0,
                     b = joins.contains(corners[i])
                             ? joins.at(corners[i]).extent
                             : 0;
        if (!length || *length <= a + b + tolerance)
          throw std::runtime_error(
              "Swept disk segment is too short for its miters");
      }
    }
    const auto eval = [&](double t, bool incoming = false) {
      // Derivative differences stay inside one smooth span. A miter consumes
      // the authored tangent jump without treating it as infinite curvature.
      auto upper = std::upper_bound(boundaries.begin(), boundaries.end(), t);
      double low = upper == boundaries.begin() ? first : *(upper - 1),
             high = upper == boundaries.end() ? last : *upper;
      if ((incoming || t == last) && t == low &&
          upper - boundaries.begin() >= 2) {
        high = low;
        low = *(upper - 2);
      }
      const double h = (high - low) * 1e-5;
      double parameter = t;
      if (t == low)
        parameter = std::nextafter(low, high);
      if (t == high)
        parameter = std::nextafter(high, low);
      auto p = curve.point(t), d = curveDerivative(curve, parameter);
      if (!p || !d || !finite(*p) || !finite(*d) || glm::length(*d) <= 1e-15)
        throw std::runtime_error("Undefined swept disk point or tangent");
      const double a = std::max(std::nextafter(low, high), t - h),
                   b = std::min(std::nextafter(high, low), t + h);
      auto l = curveDerivative(curve, a), r = curveDerivative(curve, b);
      if (!l || !r || b <= a)
        throw std::runtime_error("Undefined swept disk curvature");
      const double speed = glm::length(*d),
                   curvature =
                       glm::length(glm::cross(*d, (*r - *l) / (b - a))) /
                       (speed * speed * speed);
      if (!std::isfinite(curvature) || radius * curvature >= 1 - 1e-6)
        throw std::runtime_error(
            "Swept disk radius reaches the directrix curvature radius");
      const auto join = joins.find(t);
      const auto axis =
          join == joins.end()
              ? *d / speed
              : (incoming ? join->second.incoming : join->second.outgoing);
      return Frame{t,
                   0,
                   *p,
                   axis,
                   {},
                   {},
                   join == joins.end() ? glm::dvec3(0) : join->second.normal};
    };
    auto samples = sampleCurve(curve, first, last, error * .25, maxFrames);
    if (samples.size() < 2)
      throw std::runtime_error(
          "Swept disk directrix exceeds its sampling budget");
    for (auto &sample : samples) {
      const auto near = std::lower_bound(boundaries.begin(), boundaries.end(),
                                         sample.parameter);
      const double epsilon = 32 * std::numeric_limits<double>::epsilon() *
                             std::max({1., std::abs(first), std::abs(last)});
      if (near != boundaries.end() &&
          std::abs(*near - sample.parameter) <= epsilon)
        sample.parameter = *near;
      else if (near != boundaries.begin() &&
               std::abs(*(near - 1) - sample.parameter) <= epsilon)
        sample.parameter = *(near - 1);
    }
    std::vector<Frame> frames{eval(first)};
    const double angle = std::min(
                     pi / 12, 2 * std::acos(std::clamp(1 - error * .25 / radius,
                                                       -1., 1.))),
                 cosine = std::cos(angle);
    if (angle <= 0)
      throw std::runtime_error(
          "Swept disk precision exceeds its sampling budget");
    std::function<void(Frame, Frame, unsigned)> subdivide;
    subdivide = [&](Frame a, Frame b, unsigned depth) {
      bool flat = glm::dot(a.axis, b.axis) >= cosine;
      std::array<Frame, 3> probes;
      for (size_t i = 0; i < 3; ++i) {
        probes[i] =
            eval(a.parameter + (b.parameter - a.parameter) * (i + 1) * .25);
        flat = flat && glm::dot(a.axis, probes[i].axis) >= cosine &&
               glm::dot(b.axis, probes[i].axis) >= cosine &&
               pointDistance(probes[i].point, a.point, b.point) <= error * .25;
      }
      if (flat) {
        if (frames.size() >= maxFrames)
          throw std::runtime_error("Swept disk ring budget exceeded");
        frames.push_back(b);
      } else {
        if (depth >= 24 || probes[1].parameter == a.parameter ||
            probes[1].parameter == b.parameter)
          throw std::runtime_error(
              "Swept disk could not resolve its directrix curvature");
        subdivide(a, probes[1], depth + 1);
        subdivide(probes[1], b, depth + 1);
      }
    };
    for (size_t i = 1; i < samples.size(); ++i) {
      subdivide(frames.back(), eval(samples[i].parameter, true), 0);
      if (joins.contains(samples[i].parameter))
        frames.back() = eval(samples[i].parameter);
    }
    if (closed && glm::dot(frames.front().axis, frames.back().axis) < 1 - 1e-6)
      throw std::runtime_error(
          "Closed swept disk seam has incompatible tangents");
    for (size_t i = 0; i < frames.size(); ++i) {
      auto &f = frames[i];
      if (i == 0)
        f.x = glm::normalize(glm::cross(
            std::abs(f.axis.z) < .9 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0),
            f.axis));
      else {
        const auto &previous = frames[i - 1];
        const auto rotation = glm::cross(previous.axis, f.axis);
        const double cosine = glm::dot(previous.axis, f.axis);
        if (cosine <= -1 + 1e-8)
          throw std::runtime_error("Swept disk tangent reverses direction");
        f.x = previous.x + glm::cross(rotation, previous.x) +
              glm::cross(rotation, glm::cross(rotation, previous.x)) /
                  (1 + cosine);
        f.x = glm::normalize(f.x - f.axis * glm::dot(f.x, f.axis));
        f.station = previous.station + glm::length(f.point - previous.point);
      }
      f.y = glm::cross(f.axis, f.x);
    }
    const double length = frames.back().station;
    if (length <= tolerance)
      throw std::runtime_error("Swept disk directrix has no usable length");
    if (closed) {
      const double correction =
          std::atan2(glm::dot(glm::cross(frames.back().x, frames.front().x),
                              frames.front().axis),
                     glm::dot(frames.back().x, frames.front().x));
      for (auto &f : frames) {
        f.x = rotate(f.x, f.axis, correction * f.station / length);
        f.y = glm::cross(f.axis, f.x);
      }
      frames.back().point = frames.front().point;
      frames.back().axis = frames.front().axis;
      frames.back().x = frames.front().x;
      frames.back().y = frames.front().y;
    }
    // Check nonlocal tube overlap. Nearby intervals belong to the same regular
    // tube patch; the curvature test above covers their local fold condition.
    for (size_t i = 0; i + 1 < frames.size(); ++i)
      for (size_t j = i + 2; j + 1 < frames.size(); ++j) {
        const double gap = frames[j].station - frames[i + 1].station;
        const double wrap = length - frames[j + 1].station + frames[i].station;
        if (gap <= pi * radius || (closed && wrap <= pi * radius))
          continue;
        if (segmentDistance(frames[i].point, frames[i + 1].point,
                            frames[j].point,
                            frames[j + 1].point) < 2 * radius - 2 * error)
          throw std::runtime_error(
              "Swept disk directrix produces a self-intersecting tube");
      }
    const double crossError = std::min(error * .25, radius * .005);
    const double count =
        std::max(16., std::ceil(2 * pi /
                                (2 * std::acos(std::clamp(
                                         1 - crossError / radius, -1., 1.)))));
    if (!std::isfinite(count) || count > 4096)
      throw std::runtime_error("Swept disk cross-section budget exceeded");
    const size_t n = size_t(count), walls = inner > 0 ? 2 : 1;
    const size_t total = (frames.size() - 1) * n * 2 * walls +
                         (closed ? 0 : n * (inner > 0 ? 4 : 2));
    if (total > maxTriangles)
      throw std::runtime_error("Swept disk triangle budget exceeded");
    std::vector<glm::dvec3> outer, inside;
    outer.reserve(frames.size() * n);
    if (inner > 0)
      inside.reserve(frames.size() * n);
    for (const auto &f : frames)
      for (size_t i = 0; i < n; ++i) {
        auto v =
            f.x * std::cos(2 * pi * i / n) + f.y * std::sin(2 * pi * i / n);
        if (glm::length(f.miterNormal) > 0)
          v -= f.axis * glm::dot(f.miterNormal, v) /
               glm::dot(f.miterNormal, f.axis);
        outer.push_back(f.point + radius * v);
        if (inner > 0)
          inside.push_back(f.point + inner * v);
      }
    std::vector<std::array<glm::dvec3, 3>> triangles;
    triangles.reserve(total);
    for (size_t j = 1; j < frames.size(); ++j)
      for (size_t i = 0; i < n; ++i) {
        const size_t k = (i + 1) % n, a = (j - 1) * n + i, b = (j - 1) * n + k,
                     c = j * n + k, d = j * n + i;
        triangles.push_back({outer[a], outer[b], outer[c]});
        triangles.push_back({outer[a], outer[c], outer[d]});
        if (inner > 0) {
          triangles.push_back({inside[a], inside[c], inside[b]});
          triangles.push_back({inside[a], inside[d], inside[c]});
        }
      }
    if (!closed)
      for (size_t i = 0; i < n; ++i) {
        const size_t k = (i + 1) % n, end = (frames.size() - 1) * n;
        if (inner > 0) {
          triangles.push_back({outer[i], inside[i], inside[k]});
          triangles.push_back({outer[i], inside[k], outer[k]});
          triangles.push_back(
              {outer[end + i], inside[end + k], inside[end + i]});
          triangles.push_back(
              {outer[end + i], outer[end + k], inside[end + k]});
        } else {
          triangles.push_back({frames.front().point, outer[k], outer[i]});
          triangles.push_back(
              {frames.back().point, outer[end + i], outer[end + k]});
        }
      }
    for (const auto &t : triangles)
      if (!finite(t[0]) || !finite(t[1]) || !finite(t[2]) ||
          glm::length(glm::cross(t[1] - t[0], t[2] - t[0])) <= 0)
        throw std::runtime_error(
            "Swept disk produced a nonfinite or collapsed triangle");
    return triangles;
  } catch (const std::runtime_error &e) {
    diagnostic = e.what();
    return {};
  }
}
} // namespace container::geometry
