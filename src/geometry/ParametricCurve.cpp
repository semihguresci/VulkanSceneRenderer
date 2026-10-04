#include "Container/geometry/ParametricCurve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/vec4.hpp>
#include <limits>
#include <memory>

namespace container::geometry {
namespace {
bool finite(const glm::dvec3 &p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
double segmentDistance(const glm::dvec3 &p, const glm::dvec3 &a,
                       const glm::dvec3 &b) {
  const auto d = b - a;
  const double length2 = glm::dot(d, d);
  return glm::length(
      p - a -
      d * (length2 > 0 ? std::clamp(glm::dot(p - a, d) / length2, 0.0, 1.0)
                       : 0.0));
}
struct Spline {
  unsigned degree;
  std::vector<glm::dvec4> controls;
  std::vector<double> knots;
  std::optional<glm::dvec4> homogeneous(double t) const {
    const size_t n = controls.size() - 1;
    if (!std::isfinite(t) || t < knots[degree] || t > knots[n + 1])
      return std::nullopt;
    const size_t span =
        t == knots[n + 1]
            ? n
            : size_t(std::upper_bound(knots.begin(), knots.end(), t) -
                     knots.begin() - 1);
    std::array<glm::dvec4, 33> work{};
    for (unsigned j = 0; j <= degree; ++j)
      work[j] = controls[span - degree + j];
    for (unsigned r = 1; r <= degree; ++r)
      for (unsigned j = degree; j >= r; --j) {
        const size_t i = span - degree + j;
        const double denominator = knots[i + degree - r + 1] - knots[i];
        const double alpha = denominator > 0 ? (t - knots[i]) / denominator : 0;
        work[j] = (1 - alpha) * work[j - 1] + alpha * work[j];
      }
    return work[degree];
  }
  std::optional<glm::dvec3> point(double t) const {
    const auto h = homogeneous(t);
    if (!h || h->w <= 0)
      return std::nullopt;
    const glm::dvec3 p = glm::dvec3(*h) / h->w;
    return finite(p) ? std::optional(p) : std::nullopt;
  }
  bool insert(double t) {
    const size_t n = controls.size() - 1;
    const auto upper = std::upper_bound(knots.begin(), knots.end(), t);
    const size_t k = size_t(upper - knots.begin() - 1),
                 s = size_t(std::count(knots.begin(), knots.end(), t));
    if (s >= degree + 1)
      return true;
    if (k < degree || k > n + 1 || k - s > n)
      return false;
    std::vector<glm::dvec4> next(controls.size() + 1);
    for (size_t i = 0; i <= k - degree; ++i)
      next[i] = controls[i];
    for (size_t i = k - s; i <= n; ++i)
      next[i + 1] = controls[i];
    for (size_t i = k - degree + 1; i <= k - s; ++i) {
      const double denominator = knots[i + degree] - knots[i];
      if (denominator <= 0)
        return false;
      const double alpha = (t - knots[i]) / denominator;
      next[i] = (1 - alpha) * controls[i - 1] + alpha * controls[i];
    }
    controls = std::move(next);
    knots.insert(upper, t);
    return true;
  }
  std::vector<CurveSample> sample(double first, double last, double error,
                                  size_t limit) const {
    if (first > last) {
      auto result = sample(last, first, error, limit);
      std::ranges::reverse(result);
      return result;
    }
    if (first < knots[degree] || last > knots[controls.size()] || first == last)
      return {};
    Spline refined = *this;
    std::vector<double> boundaries{first, last};
    for (double t : knots)
      if (t > first && t < last)
        boundaries.push_back(t);
    std::ranges::sort(boundaries);
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                     boundaries.end());
    for (double t : boundaries) {
      const size_t target = (t == first || t == last) ? degree + 1 : degree;
      while (size_t(std::count(refined.knots.begin(), refined.knots.end(), t)) <
             target)
        if (!refined.insert(t))
          return {};
    }
    std::vector<CurveSample> result;
    const auto begin = point(first);
    if (!begin)
      return {};
    result.push_back({first, *begin});
    std::function<bool(std::vector<glm::dvec4>, double, double, unsigned)>
        subdivide;
    subdivide = [&](std::vector<glm::dvec4> c, double a, double b,
                    unsigned depth) {
      const auto p = glm::dvec3(c.front()) / c.front().w,
                 q = glm::dvec3(c.back()) / c.back().w;
      bool flat = true;
      const auto chord = q - p;
      const double chordLength = glm::length(chord);
      double previousProjection = -std::numeric_limits<double>::infinity();
      for (const auto &h : c) {
        const auto v = glm::dvec3(h) / h.w;
        if (!finite(v))
          return false;
        if (segmentDistance(v, p, q) > error)
          flat = false;
        if (chordLength > error) {
          const double projection = glm::dot(v - p, chord) / chordLength;
          if (projection < previousProjection - error)
            flat = false;
          previousProjection = projection;
        }
      }
      if (flat) {
        if (result.size() >= limit)
          return false;
        result.push_back({b, q});
        return true;
      }
      if (depth >= 32 || a + (b - a) * .5 == a || a + (b - a) * .5 == b)
        return false;
      std::vector<glm::dvec4> left(c.size()), right(c.size());
      left[0] = c.front();
      right.back() = c.back();
      for (size_t r = 1; r < c.size(); ++r) {
        for (size_t j = 0; j < c.size() - r; ++j)
          c[j] = (c[j] + c[j + 1]) * .5;
        left[r] = c[0];
        right[c.size() - r - 1] = c[c.size() - r - 1];
      }
      const double mid = a + (b - a) * .5;
      return subdivide(std::move(left), a, mid, depth + 1) &&
             subdivide(std::move(right), mid, b, depth + 1);
    };
    for (size_t k = degree; k < refined.controls.size(); ++k) {
      const double a = refined.knots[k], b = refined.knots[k + 1];
      if (a < first || b > last || b <= a)
        continue;
      std::vector<glm::dvec4> c(refined.controls.begin() + k - degree,
                                refined.controls.begin() + k + 1);
      if (!subdivide(std::move(c), a, b, 0))
        return {};
    }
    return result.size() > 1 ? result : std::vector<CurveSample>{};
  }
};
} // namespace

std::vector<CurveSample> sampleCurve(const ParametricCurve &curve, double first,
                                     double last, double error, size_t limit) {
  if (!curve.point || !std::isfinite(first) || !std::isfinite(last) ||
      first == last || !std::isfinite(error) || error <= 0 || limit < 2)
    return {};
  if (curve.sampler)
    return curve.sampler(first, last, error, limit);
  std::vector<double> boundaries{first, last};
  for (double t : curve.breaks)
    if (t > std::min(first, last) && t < std::max(first, last))
      boundaries.push_back(t);
  std::ranges::sort(boundaries);
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());
  if (last < first)
    std::ranges::reverse(boundaries);
  std::vector<CurveSample> result;
  auto begin = curve.point(first);
  if (!begin || !finite(*begin))
    return {};
  result.push_back({first, *begin});
  std::function<bool(double, double, glm::dvec3, glm::dvec3, unsigned)>
      subdivide;
  subdivide = [&](double a, double b, glm::dvec3 p, glm::dvec3 q,
                  unsigned depth) {
    std::array<glm::dvec3, 3> probes;
    bool flat = !curve.stepValidator || curve.stepValidator(a, b);
    for (size_t i = 0; i < 3; ++i) {
      auto v = curve.point(a + (b - a) * double(i + 1) * .25);
      if (!v || !finite(*v))
        return false;
      probes[i] = *v;
      if (segmentDistance(*v, p, q) > error)
        flat = false;
    }
    if (flat) {
      if (result.size() >= limit)
        return false;
      result.push_back({b, q});
      return true;
    }
    const double mid = a + (b - a) * .5;
    if (depth >= 24 || mid == a || mid == b)
      return false;
    return subdivide(a, mid, p, probes[1], depth + 1) &&
           subdivide(mid, b, probes[1], q, depth + 1);
  };
  for (size_t i = 1; i < boundaries.size(); ++i) {
    // Initial subdivisions avoid midpoint aliasing for oscillatory curves.
    for (unsigned j = 0; j < 8; ++j) {
      const double a = boundaries[i - 1] +
                       (boundaries[i] - boundaries[i - 1]) * double(j) / 8,
                   b = boundaries[i - 1] +
                       (boundaries[i] - boundaries[i - 1]) * double(j + 1) / 8;
      auto p = curve.point(a), q = curve.point(b);
      if (!p || !q || !finite(*p) || !finite(*q) || !subdivide(a, b, *p, *q, 0))
        return {};
    }
  }
  return result;
}

std::optional<glm::dvec3> curveDerivative(const ParametricCurve &curve,
                                          double t) {
  if (!std::isfinite(t) || !curve.point)
    return std::nullopt;
  if (curve.derivative) {
    const auto d = curve.derivative(t);
    return d && finite(*d) ? d : std::nullopt;
  }
  const double extent =
      curve.domain ? curve.domain->second - curve.domain->first : 1;
  double a = t - extent * 1e-6, b = t + extent * 1e-6;
  if (curve.domain && !curve.period) {
    a = std::max(a, curve.domain->first);
    b = std::min(b, curve.domain->second);
  }
  const auto p = curve.point(a), q = curve.point(b);
  if (!p || !q || !finite(*p) || !finite(*q) || b <= a)
    return std::nullopt;
  return (*q - *p) / (b - a);
}

std::optional<glm::dvec3> curveTangent(const ParametricCurve &curve, double t) {
  const auto d = curveDerivative(curve, t);
  return d && glm::length(*d) > 1e-15 ? std::optional(glm::normalize(*d))
                                      : std::nullopt;
}

std::optional<CurveFrame> curveFrame(const ParametricCurve &curve, double t) {
  if (curve.frame)
    return curve.frame(t);
  const auto p = curve.point(t), x = curveTangent(curve, t);
  if (!p || !x)
    return std::nullopt;
  auto y = glm::cross(glm::dvec3(0, 0, 1), *x);
  if (glm::length(y) < 1e-12)
    y = glm::cross(glm::dvec3(1, 0, 0), *x);
  return CurveFrame{*p, *x, glm::normalize(y),
                    glm::normalize(glm::cross(*x, y))};
}

std::optional<glm::dvec3>
integrateCurveFunction(const ParametricCurve::Evaluator &f, double a, double b,
                       double tolerance, size_t intervals) {
  if (!f || !std::isfinite(a) || !std::isfinite(b) ||
      !std::isfinite(tolerance) || tolerance <= 0 || intervals < 1 ||
      intervals > 4096)
    return std::nullopt;
  if (a == b)
    return glm::dvec3(0);
  if (a > b) {
    auto r = integrateCurveFunction(f, b, a, tolerance, intervals);
    return r ? std::optional(-*r) : std::nullopt;
  }
  size_t evaluations = 0;
  const auto eval = [&](double t) -> std::optional<glm::dvec3> {
    if (++evaluations > 131072)
      return std::nullopt;
    auto r = f(t);
    return r && finite(*r) ? r : std::nullopt;
  };
  std::function<std::optional<glm::dvec3>(double, double, glm::dvec3,
                                          glm::dvec3, glm::dvec3, glm::dvec3,
                                          double, unsigned)>
      integrate;
  integrate = [&](double l, double r, glm::dvec3 p, glm::dvec3 m, glm::dvec3 q,
                  glm::dvec3 whole, double e,
                  unsigned depth) -> std::optional<glm::dvec3> {
    const double mid = l + (r - l) * .5;
    auto u = eval(l + (mid - l) * .5), v = eval(mid + (r - mid) * .5);
    if (!u || !v)
      return std::nullopt;
    const auto left = (mid - l) / 6 * (p + 4. * (*u) + m),
               right = (r - mid) / 6 * (m + 4. * (*v) + q),
               difference = left + right - whole;
    if (glm::length(difference) <= 15 * e)
      return left + right + difference / 15.;
    if (depth >= 22 || mid == l || mid == r)
      return std::nullopt;
    auto x = integrate(l, mid, p, *u, m, left, e * .5, depth + 1),
         y = integrate(mid, r, m, *v, q, right, e * .5, depth + 1);
    return x && y ? std::optional(*x + *y) : std::nullopt;
  };
  glm::dvec3 result(0);
  for (size_t i = 0; i < intervals; ++i) {
    const double l = a + (b - a) * i / intervals,
                 r = a + (b - a) * (i + 1) / intervals;
    auto p = eval(l), m = eval(l + (r - l) * .5), q = eval(r);
    if (!p || !m || !q)
      return std::nullopt;
    auto v = integrate(l, r, *p, *m, *q, (r - l) / 6 * (*p + 4. * (*m) + *q),
                       tolerance / intervals, 0);
    if (!v)
      return std::nullopt;
    result += *v;
  }
  return finite(result) ? std::optional(result) : std::nullopt;
}

std::optional<double> curveLength(const ParametricCurve &curve, double a,
                                  double b, double tolerance) {
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(tolerance) ||
      tolerance <= 0)
    return std::nullopt;
  if (curve.domain && !curve.period &&
      (a < curve.domain->first || a > curve.domain->second ||
       b < curve.domain->first || b > curve.domain->second))
    return std::nullopt;
  if (a == b)
    return 0.;
  if (curve.constantSpeed)
    return std::abs(b - a) * (*curve.constantSpeed);
  std::vector<double> bounds{std::min(a, b), std::max(a, b)};
  for (double t : curve.breaks)
    if (t > bounds.front() && t < bounds[1])
      bounds.push_back(t);
  std::ranges::sort(bounds);
  bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
  double length = 0;
  for (size_t i = 1; i < bounds.size(); ++i) {
    auto v = integrateCurveFunction(
        [&](double t) -> std::optional<glm::dvec3> {
          // Speed at a C0 join belongs to the span being integrated. The
          // other span's endpoint derivative can otherwise prevent Simpson
          // convergence even though the one-sided integral is regular.
          if (t == bounds[i - 1])
            t = std::nextafter(t, bounds[i]);
          else if (t == bounds[i])
            t = std::nextafter(t, bounds[i - 1]);
          auto d = curveDerivative(curve, t);
          return d ? std::optional(glm::dvec3(glm::length(*d), 0, 0))
                   : std::nullopt;
        },
        bounds[i - 1], bounds[i], tolerance / bounds.size());
    if (!v)
      return std::nullopt;
    length += v->x;
  }
  return std::isfinite(length) ? std::optional(length) : std::nullopt;
}

std::optional<double> curveParameterAtDistance(const ParametricCurve &curve,
                                               double distance, double origin,
                                               double tolerance) {
  if (!std::isfinite(distance) || !std::isfinite(origin) ||
      !std::isfinite(tolerance) || tolerance <= 0)
    return std::nullopt;
  if (curve.domain && !curve.period &&
      (origin < curve.domain->first || origin > curve.domain->second))
    return std::nullopt;
  if (distance == 0)
    return origin;
  const double sign = distance > 0 ? 1 : -1, target = std::abs(distance);
  if (curve.constantSpeed && *curve.constantSpeed > 0) {
    const double t = origin + distance / (*curve.constantSpeed);
    if (curve.domain && !curve.period &&
        (t < curve.domain->first || t > curve.domain->second))
      return std::nullopt;
    return std::isfinite(t) ? std::optional(t) : std::nullopt;
  }
  double lo = 0, hi = std::max(1., target);
  const double bound = curve.domain && !curve.period
                           ? (sign > 0 ? curve.domain->second - origin
                                       : origin - curve.domain->first)
                           : std::numeric_limits<double>::infinity();
  for (unsigned i = 0; i < 64; ++i) {
    hi = std::min(hi, bound);
    const auto length =
        curveLength(curve, origin, origin + sign * hi, tolerance * .1);
    if (!length)
      return std::nullopt;
    // Preserve an authored terminal station exactly. A bisection result just
    // inside the endpoint can otherwise shrink a derived surface's domain.
    if (hi == bound && std::abs(*length - target) <= tolerance)
      return origin + sign * hi;
    if (*length >= target - tolerance)
      break;
    if (hi >= bound || i == 63)
      return std::nullopt;
    hi *= 2;
  }
  for (unsigned i = 0; i < 64; ++i) {
    const double mid = lo + (hi - lo) * .5;
    const auto length =
        curveLength(curve, origin, origin + sign * mid, tolerance * .1);
    if (!length)
      return std::nullopt;
    if (std::abs(*length - target) <= tolerance)
      return origin + sign * mid;
    if (*length < target)
      lo = mid;
    else
      hi = mid;
  }
  return std::nullopt;
}

std::optional<double> curveParameterAtPoint(const ParametricCurve &curve,
                                            const glm::dvec3 &point,
                                            double tolerance) {
  if (!finite(point) || !std::isfinite(tolerance) || tolerance <= 0)
    return std::nullopt;
  if (curve.inverse) {
    auto t = curve.inverse(point);
    const auto p = t ? curve.point(*t) : std::nullopt;
    return p && glm::length(*p - point) <= tolerance ? t : std::nullopt;
  }
  if (!curve.domain)
    return std::nullopt;
  std::vector<CurveSample> samples;
  for (unsigned i = 0; i < 128; ++i) {
    const double a = curve.domain->first +
                     (curve.domain->second - curve.domain->first) * i / 128,
                 b = curve.domain->first +
                     (curve.domain->second - curve.domain->first) * (i + 1) /
                         128;
    auto part = sampleCurve(curve, a, b, tolerance * .25,
                            65536 - samples.size() + (samples.empty() ? 0 : 1));
    if (part.empty())
      return std::nullopt;
    samples.insert(samples.end(), part.begin() + (samples.empty() ? 0 : 1),
                   part.end());
  }
  if (samples.empty())
    return std::nullopt;
  std::vector<std::pair<double, double>> candidates;
  for (size_t i = 1; i < samples.size(); ++i) {
    if (segmentDistance(point, samples[i - 1].point, samples[i].point) >
        tolerance * 18)
      continue;
    double a = samples[i - 1].parameter, b = samples[i].parameter;
    const auto distance = [&](double t) {
      auto p = curve.point(t);
      return p ? glm::dot(*p - point, *p - point)
               : std::numeric_limits<double>::infinity();
    };
    for (unsigned j = 0; j < 72; ++j) {
      const double l = a + (b - a) / 3, r = b - (b - a) / 3;
      if (distance(l) < distance(r))
        b = r;
      else
        a = l;
    }
    double t = a + (b - a) * .5;
    for (double endpoint : {samples[i - 1].parameter, samples[i].parameter})
      if (distance(endpoint) < distance(t))
        t = endpoint;
    if (distance(t) > tolerance * tolerance)
      continue;
    candidates.emplace_back(t, distance(t));
  }
  if (candidates.empty())
    return std::nullopt;
  const auto best =
      *std::min_element(candidates.begin(), candidates.end(),
                        [](auto a, auto b) { return a.second < b.second; });
  for (const auto &candidate : candidates)
    if (candidate.second <=
            best.second + std::max(1e-24, tolerance * tolerance * 1e-8) &&
        std::abs(candidate.first - best.first) >
            (curve.domain->second - curve.domain->first) * 1e-7)
      return std::nullopt;
  return best.first;
}

std::optional<ParametricCurve> makeBSplineCurve(unsigned degree,
                                                std::vector<glm::dvec3> points,
                                                std::vector<double> knots,
                                                std::vector<double> weights,
                                                unsigned dimension) {
  if (degree < 1 || degree > 32 || points.size() <= degree ||
      points.size() > 4096 || knots.size() != points.size() + degree + 1 ||
      (dimension != 2 && dimension != 3))
    return std::nullopt;
  if (weights.empty())
    weights.assign(points.size(), 1);
  if (weights.size() != points.size())
    return std::nullopt;
  for (size_t i = 0; i < points.size(); ++i)
    if (!finite(points[i]) || !std::isfinite(weights[i]) || weights[i] <= 0)
      return std::nullopt;
  for (size_t i = 0; i < knots.size(); ++i)
    if (!std::isfinite(knots[i]) || (i && knots[i] < knots[i - 1]))
      return std::nullopt;
  const double first = knots[degree], last = knots[points.size()];
  if (first >= last)
    return std::nullopt;
  for (size_t i = 0; i < knots.size();) {
    size_t j = i + 1;
    while (j < knots.size() && knots[i] == knots[j])
      ++j;
    if (j - i > degree + 1 ||
        (knots[i] > first && knots[i] < last && j - i > degree))
      return std::nullopt;
    i = j;
  }
  auto spline = std::make_shared<Spline>();
  spline->degree = degree;
  spline->knots = std::move(knots);
  const double scale = *std::max_element(weights.begin(), weights.end());
  for (size_t i = 0; i < points.size(); ++i) {
    const double w = weights[i] / scale;
    if (w <= 0)
      return std::nullopt;
    spline->controls.emplace_back(points[i] * w, w);
  }
  ParametricCurve curve;
  curve.dimension = dimension;
  curve.domain = {{first, last}};
  curve.breaks = spline->knots;
  curve.point = [spline](double t) { return spline->point(t); };
  auto derivative = std::make_shared<Spline>();
  derivative->degree = degree - 1;
  derivative->knots.assign(spline->knots.begin() + 1, spline->knots.end() - 1);
  for (size_t i = 0; i + 1 < spline->controls.size(); ++i) {
    const double d = spline->knots[i + degree + 1] - spline->knots[i + 1];
    derivative->controls.push_back(
        d > 0 ? double(degree) *
                    (spline->controls[i + 1] - spline->controls[i]) / d
              : glm::dvec4(0));
  }
  curve.derivative = [spline,
                      derivative](double t) -> std::optional<glm::dvec3> {
    auto h = spline->homogeneous(t), d = derivative->homogeneous(t);
    if (!h || !d || h->w <= 0)
      return std::nullopt;
    return (glm::dvec3(*d) - glm::dvec3(*h) * (d->w / h->w)) / h->w;
  };
  curve.sampler = [spline](double a, double b, double e, size_t n) {
    return spline->sample(a, b, e, n);
  };
  return curve;
}
} // namespace container::geometry
