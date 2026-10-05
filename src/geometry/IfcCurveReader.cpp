#include "IfcCurveReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace container::geometry::ifc::detail {
namespace {
constexpr double pi = std::numbers::pi;
const StepValue *arg(const Entity &e, size_t i) {
  return i < e.args.list.size() ? &e.args.list[i] : nullptr;
}
bool omitted(const StepValue *v) {
  return !v || v->kind == StepValue::Kind::Omitted;
}
double number(const StepValue *v) {
  if (!v || v->kind != StepValue::Kind::Number || !std::isfinite(v->number))
    throw std::runtime_error("Expected a finite numeric curve attribute");
  return v->number;
}
std::string enumeration(const StepValue *v) {
  if (!v || v->kind != StepValue::Kind::Enum)
    throw std::runtime_error("Missing curve enumeration");
  return v->text;
}
bool boolean(const StepValue *v) {
  const auto s = enumeration(v);
  if (s != "T" && s != "F")
    throw std::runtime_error("Invalid curve sense");
  return s == "T";
}
const std::vector<StepValue> &list(const StepValue *v, size_t maximum = 65536) {
  if (!v || v->kind != StepValue::Kind::List || v->list.empty() ||
      v->list.size() > maximum)
    throw std::runtime_error(
        "Empty, invalid or excessive curve attribute list");
  return v->list;
}
std::vector<double> numbers(const StepValue *v, size_t maximum = 65536) {
  std::vector<double> r;
  for (const auto &n : list(v, maximum))
    r.push_back(number(&n));
  return r;
}
uint32_t reference(const StepValue *v) {
  if (!v || v->kind != StepValue::Kind::Ref)
    throw std::runtime_error("Missing curve entity reference");
  return v->ref;
}
bool finite(glm::dvec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
glm::dvec3 normalized(glm::dvec3 v) {
  const double l = glm::length(v);
  if (!std::isfinite(l) || l < 1e-15)
    throw std::runtime_error("Degenerate curve direction");
  return v / l;
}
double wrapped(double t, double period) {
  double v = std::fmod(t, period);
  if (v < 0)
    v += period;
  return v;
}
struct Measure {
  double value;
  bool parameter;
};
using Surface = ParametricSurface;
Measure measure(const StepValue *v) {
  if (!v || v->kind != StepValue::Kind::List || v->list.size() != 1 ||
      (v->text != "IFCPARAMETERVALUE" && v->text != "IFCLENGTHMEASURE" &&
       v->text != "IFCNONNEGATIVELENGTHMEASURE"))
    throw std::runtime_error("Invalid curve parameter/length measure select");
  const double n = number(&v->list.front());
  if (v->text == "IFCNONNEGATIVELENGTHMEASURE" && n < 0)
    throw std::runtime_error("Negative nonnegative curve measure");
  return {n, v->text == "IFCPARAMETERVALUE"};
}

template <class Function> void shareFunction(Function &function) {
  if (function) {
    auto shared = std::make_shared<Function>(std::move(function));
    function = [shared](auto &&...args) {
      return (*shared)(std::forward<decltype(args)>(args)...);
    };
  }
}
ParametricCurve sharedCurve(ParametricCurve curve) {
  // Sharing callbacks bounds closure storage for deeply nested curve parents.
  shareFunction(curve.point);
  shareFunction(curve.derivative);
  shareFunction(curve.stepValidator);
  shareFunction(curve.inverse);
  shareFunction(curve.sampler);
  shareFunction(curve.frame);
  shareFunction(curve.curvature);
  return curve;
}

ParametricCurve remap(ParametricCurve parent, double first, double last,
                      double extent) {
  if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(extent) ||
      extent <= 0 || first == last)
    throw std::runtime_error("Empty or nonfinite curve interval");
  const double scale = (last - first) / extent;
  ParametricCurve r;
  r.domain = {{0, extent}};
  r.dimension = parent.dimension;
  if (parent.stepValidator)
    r.stepValidator = [parent, first, scale](double a, double b) {
      return parent.stepValidator(first + scale * a, first + scale * b);
    };
  r.point = [parent, first, scale, extent](double t) {
    return parent.point(first + scale * std::clamp(t, 0., extent));
  };
  r.derivative = [parent, first, scale,
                  extent](double t) -> std::optional<glm::dvec3> {
    auto d = curveDerivative(parent, first + scale * std::clamp(t, 0., extent));
    return d ? std::optional(*d * scale) : std::nullopt;
  };
  if (parent.constantSpeed)
    r.constantSpeed = *parent.constantSpeed * std::abs(scale);
  for (double t : parent.breaks) {
    if (parent.period) {
      const double low = std::min(first, last), high = std::max(first, last);
      if ((high - low) / parent.period > 16384)
        throw std::runtime_error("Excessive periodic curve extent");
      double value = low + wrapped(t - low, parent.period);
      for (size_t i = 0; value < high && i <= 16384; ++i) {
        const double s = (value - first) / scale;
        if (s > 0 && s < extent)
          r.breaks.push_back(s);
        const double next = value + parent.period;
        if (next <= value)
          throw std::runtime_error("Curve parameter precision exhausted");
        value = next;
      }
    } else {
      const double s = (t - first) / scale;
      if (s > 0 && s < extent)
        r.breaks.push_back(s);
    }
  }
  r.sampler = [parent, first, scale, extent](double a, double b, double e,
                                             size_t n) {
    if (a < 0 || a > extent || b < 0 || b > extent)
      return std::vector<CurveSample>{};
    auto samples =
        sampleCurve(parent, first + scale * a, first + scale * b, e, n);
    for (auto &v : samples)
      v.parameter = (v.parameter - first) / scale;
    return samples;
  };
  if (parent.frame)
    r.frame = [parent, first, scale](double t) -> std::optional<CurveFrame> {
      auto f = curveFrame(parent, first + scale * t);
      if (f && scale < 0) {
        f->x = -f->x;
        f->y = -f->y;
      }
      return f;
    };
  if (parent.curvature)
    r.curvature = [parent, first, scale](double t) {
      return parent.curvature(first + scale * t) * (scale < 0 ? -1 : 1);
    };
  return sharedCurve(std::move(r));
}
ParametricCurve transformed(ParametricCurve parent, CurveFrame placement) {
  const auto vector = [placement](glm::dvec3 p) {
    return placement.x * p.x + placement.y * p.y + placement.z * p.z;
  };
  ParametricCurve r = parent;
  r.inverse = {};
  r.point = [parent, placement, vector](double t) -> std::optional<glm::dvec3> {
    auto p = parent.point(t);
    return p ? std::optional(placement.origin + vector(*p)) : std::nullopt;
  };
  r.derivative = [parent, vector](double t) -> std::optional<glm::dvec3> {
    auto p = curveDerivative(parent, t);
    return p ? std::optional(vector(*p)) : std::nullopt;
  };
  r.sampler = [parent, placement, vector](double a, double b, double e,
                                          size_t n) {
    auto samples = sampleCurve(parent, a, b, e, n);
    for (auto &v : samples)
      v.point = placement.origin + vector(v.point);
    return samples;
  };
  if (parent.inverse)
    r.inverse = [parent, placement](glm::dvec3 p) {
      const auto d = p - placement.origin;
      return parent.inverse({glm::dot(d, placement.x), glm::dot(d, placement.y),
                             glm::dot(d, placement.z)});
    };
  r.frame = [parent, placement, vector](double t) -> std::optional<CurveFrame> {
    auto f = curveFrame(parent, t);
    return f ? std::optional(CurveFrame{placement.origin + vector(f->origin),
                                        vector(f->x), vector(f->y),
                                        vector(f->z)})
             : std::nullopt;
  };
  return sharedCurve(std::move(r));
}

class Reader {
public:
  Reader(const std::unordered_map<uint32_t, Entity> &entities, double units,
         std::optional<double> angle)
      : entities_(entities), tolerance_(1e-5 / units),
        chordError_(.001 / units), angle_(angle) {}
  ParametricCurve read(uint32_t id, double spiralLength = 0);
  ParametricCurve readImpl(uint32_t id, double spiralLength);
  std::vector<std::array<glm::dvec3, 3>> sectionedMesh(uint32_t id);
  Surface surface(uint32_t id);

private:
  const Entity &get(uint32_t id) const {
    const auto it = entities_.find(id);
    if (it == entities_.end())
      throw std::runtime_error("Missing curve entity #" + std::to_string(id));
    return it->second;
  }
  const Entity &referenced(const Entity &e, size_t i) const {
    return get(reference(arg(e, i)));
  }
  std::pair<glm::dvec3, unsigned> point(uint32_t id) const {
    const auto &e = get(id);
    if (e.type != "IFCCARTESIANPOINT")
      throw std::runtime_error("Expected Cartesian curve point");
    const auto v = numbers(arg(e, 0), 3);
    if (v.size() != 2 && v.size() != 3)
      throw std::runtime_error("Invalid curve point dimension");
    return {{v[0], v[1], v.size() == 3 ? v[2] : 0}, unsigned(v.size())};
  }
  glm::dvec3 direction(uint32_t id, unsigned dimension) const {
    const auto &e = get(id);
    if (e.type != "IFCDIRECTION")
      throw std::runtime_error("Expected curve direction");
    const auto v = numbers(arg(e, 0), 3);
    if (v.size() != dimension)
      throw std::runtime_error("Mismatched curve direction dimension");
    return normalized({v[0], v[1], v.size() == 3 ? v[2] : 0});
  }
  CurveFrame placement(uint32_t id, unsigned &dimension);
  std::pair<double, glm::dvec3>
  distanceExpression(const Entity &, const ParametricCurve &, uint32_t basisId);
  ParametricCurve polyline(const Entity &);
  ParametricCurve spline(const Entity &);
  ParametricCurve trimmed(const Entity &);
  ParametricCurve segment(const Entity &, bool polynomialProfile = false);
  ParametricCurve composite(const Entity &,
                            std::optional<uint32_t> uvBasis = {});
  ParametricCurve alignment(const Entity &);
  ParametricCurve spiral(const Entity &, double length);
  ParametricCurve offset(const Entity &);
  ParametricCurve pcurve(const Entity &);
  Surface surfaceImpl(uint32_t id);
  ParametricCurve profile(uint32_t id);
  ParametricCurve parameterCurve(uint32_t id, uint32_t basis);
  double toParameter(const ParametricCurve &c, Measure m, double origin) const {
    if (m.parameter)
      return m.value;
    auto t = curveParameterAtDistance(c, m.value, origin, tolerance_ * .01);
    if (!t)
      throw std::runtime_error(
          "Curve distance is outside its domain or could not be integrated");
    return *t;
  }
  void consumePoints(size_t count) {
    if (count > 65536 - sourcePoints_)
      throw std::runtime_error("Curve control/source point budget exceeded");
    sourcePoints_ += count;
  }
  const std::unordered_map<uint32_t, Entity> &entities_;
  double tolerance_, chordError_;
  std::optional<double> angle_;
  std::unordered_set<uint32_t> visiting_;
  size_t nodes_{0};
  size_t sourcePoints_{0};
  bool meshing_{false};
};

CurveFrame Reader::placement(uint32_t id, unsigned &dimension) {
  const auto &e = get(id);
  CurveFrame f;
  if (e.type == "IFCAXIS2PLACEMENTLINEAR") {
    const auto &expression = referenced(e, 0);
    const uint32_t basisId = reference(arg(expression, 4));
    auto basis = read(basisId);
    const auto [t, offsets] = distanceExpression(expression, basis, basisId);
    auto base = curveFrame(basis, t);
    if (!base)
      throw std::runtime_error("Undefined linear placement frame");
    f.origin = base->origin + offsets.x * base->y + offsets.y * base->z +
               offsets.z * base->x;
    f.z = omitted(arg(e, 1)) ? base->z : direction(reference(arg(e, 1)), 3);
    f.x = omitted(arg(e, 2)) ? base->x : direction(reference(arg(e, 2)), 3);
    dimension = 3;
  } else {
    if (e.type != "IFCAXIS2PLACEMENT2D" && e.type != "IFCAXIS2PLACEMENT3D")
      throw std::runtime_error("Unsupported curve placement " + e.type);
    auto p = point(reference(arg(e, 0)));
    dimension = e.type == "IFCAXIS2PLACEMENT2D" ? 2 : 3;
    if (p.second != dimension)
      throw std::runtime_error("Mismatched curve placement dimension");
    f.origin = p.first;
    if (dimension == 2) {
      if (!omitted(arg(e, 1)))
        f.x = direction(reference(arg(e, 1)), 2);
    } else {
      if (!omitted(arg(e, 1)))
        f.z = direction(reference(arg(e, 1)), 3);
      if (!omitted(arg(e, 2)))
        f.x = direction(reference(arg(e, 2)), 3);
      else if (std::abs(glm::dot(f.x, f.z)) > .999)
        f.x = {0, 1, 0};
    }
  }
  f.x = normalized(f.x - f.z * glm::dot(f.x, f.z));
  f.y = normalized(glm::cross(f.z, f.x));
  return f;
}

ParametricCurve Reader::spline(const Entity &e) {
  const double d = number(arg(e, 0));
  if (d < 1 || d > 32 || std::floor(d) != d)
    throw std::runtime_error("Spline degree must be an integer in [1,32]");
  std::vector<glm::dvec3> controls;
  unsigned dimension = 0;
  consumePoints(list(arg(e, 1), 4096).size());
  for (const auto &r : list(arg(e, 1), 4096)) {
    auto p = point(reference(&r));
    if (dimension && p.second != dimension)
      throw std::runtime_error("Mixed spline control point dimensions");
    dimension = p.second;
    controls.push_back(p.first);
  }
  const auto values = numbers(arg(e, 6), 4096),
             multiplicities = numbers(arg(e, 5), 4096);
  if (values.size() != multiplicities.size())
    throw std::runtime_error("Spline knot/multiplicity count mismatch");
  std::vector<double> knots;
  for (size_t i = 0; i < values.size(); ++i) {
    const double m = multiplicities[i];
    if ((i && values[i] <= values[i - 1]) || m < 1 || m > d + 1 ||
        m != std::floor(m) || knots.size() + m > controls.size() + d + 1)
      throw std::runtime_error("Invalid spline knot order or multiplicity");
    knots.insert(knots.end(), size_t(m), values[i]);
  }
  auto weights = e.type == "IFCRATIONALBSPLINECURVEWITHKNOTS"
                     ? numbers(arg(e, 8), 4096)
                     : std::vector<double>{};
  auto c = makeBSplineCurve(unsigned(d), std::move(controls), std::move(knots),
                            std::move(weights), dimension);
  if (!c)
    throw std::runtime_error(
        "Invalid B-spline knots, control points or rational weights");
  return *c;
}

ParametricCurve Reader::polyline(const Entity &e) {
  std::vector<glm::dvec3> points;
  unsigned dimension = 0;
  if (e.type == "IFCPOLYLINE") {
    consumePoints(list(arg(e, 0)).size());
    for (const auto &v : list(arg(e, 0))) {
      auto p = point(reference(&v));
      if (dimension && dimension != p.second)
        throw std::runtime_error("Mixed polyline point dimensions");
      dimension = p.second;
      points.push_back(p.first);
    }
  } else {
    const auto &pl = referenced(e, 0);
    dimension = pl.type == "IFCCARTESIANPOINTLIST2D"   ? 2
                : pl.type == "IFCCARTESIANPOINTLIST3D" ? 3
                                                       : 0;
    if (!dimension)
      throw std::runtime_error("Invalid indexed curve point list");
    consumePoints(list(arg(pl, 0)).size());
    for (const auto &v : list(arg(pl, 0))) {
      auto p = numbers(&v, 3);
      if (p.size() != dimension)
        throw std::runtime_error("Mixed indexed curve dimensions");
      points.emplace_back(p[0], p[1], dimension == 3 ? p[2] : 0);
    }
  }
  std::vector<ParametricCurve> pieces;
  const auto line = [&](size_t a, size_t b) {
    if (a >= points.size() || b >= points.size() ||
        glm::length(points[b] - points[a]) <= 1e-15)
      throw std::runtime_error("Degenerate or out-of-range polyline segment");
    ParametricCurve c;
    c.domain = {{0, 1}};
    c.dimension = dimension;
    const auto p = points[a], d = points[b] - p;
    c.point = [p, d](double t) -> std::optional<glm::dvec3> {
      return p + d * t;
    };
    c.derivative = [d](double) -> std::optional<glm::dvec3> { return d; };
    c.constantSpeed = glm::length(d);
    c.sampler = [p, d](double a, double b, double, size_t n) {
      return n >= 2 ? std::vector<CurveSample>{{a, p + d * a}, {b, p + d * b}}
                    : std::vector<CurveSample>{};
    };
    pieces.push_back(c);
  };
  if (e.type == "IFCPOLYLINE" || omitted(arg(e, 1))) {
    for (size_t i = 1; i < points.size(); ++i)
      if (points[i] != points[i - 1])
        line(i - 1, i);
  } else {
    size_t previous = std::numeric_limits<size_t>::max();
    for (const auto &v : list(arg(e, 1))) {
      if (v.text != "IFCLINEINDEX" && v.text != "IFCARCINDEX")
        throw std::runtime_error("Unknown indexed curve segment");
      if (v.list.size() != 1)
        throw std::runtime_error("Invalid indexed curve segment select");
      const auto indices = numbers(&v.list.front());
      std::vector<size_t> index;
      for (double i : indices) {
        if (i < 1 || i > points.size() || std::floor(i) != i)
          throw std::runtime_error("Invalid curve point index");
        index.push_back(size_t(i) - 1);
      }
      if (index.size() < 2 || (v.text == "IFCARCINDEX" && index.size() != 3) ||
          (previous != std::numeric_limits<size_t>::max() &&
           index.front() != previous))
        throw std::runtime_error("Disconnected or invalid indexed segments");
      previous = index.back();
      if (v.text == "IFCLINEINDEX") {
        for (size_t i = 1; i < index.size(); ++i)
          line(index[i - 1], index[i]);
        continue;
      }
      const auto a = points[index[0]], b = points[index[1]],
                 c = points[index[2]], ab = b - a, ac = c - a,
                 n = glm::cross(ab, ac);
      const double n2 = glm::dot(n, n);
      if (n2 <= 1e-24 * glm::dot(ab, ab) * glm::dot(ac, ac)) {
        line(index[0], index[1]);
        line(index[1], index[2]);
        continue;
      }
      const auto center = a + (glm::dot(ab, ab) * glm::cross(ac, n) +
                               glm::dot(ac, ac) * glm::cross(n, ab)) /
                                  (2 * n2);
      const double radius = glm::length(a - center);
      const auto u = normalized(a - center), normal = normalized(n),
                 w = glm::cross(normal, u);
      const auto angle = [&](glm::dvec3 p) {
        return wrapped(
            std::atan2(glm::dot(p - center, w), glm::dot(p - center, u)),
            2 * pi);
      };
      const double middle = angle(b), end = angle(c);
      const double sweep = middle < end ? end : end - 2 * pi;
      ParametricCurve curve;
      curve.dimension = dimension;
      curve.domain = {{0, 1}};
      curve.breaks = {middle / sweep};
      curve.constantSpeed = radius * std::abs(sweep);
      curve.point = [center, u, w, radius,
                     sweep](double t) -> std::optional<glm::dvec3> {
        return center +
               radius * (u * std::cos(t * sweep) + w * std::sin(t * sweep));
      };
      curve.derivative = [u, w, radius,
                          sweep](double t) -> std::optional<glm::dvec3> {
        return radius * sweep *
               (-u * std::sin(t * sweep) + w * std::cos(t * sweep));
      };
      pieces.push_back(curve);
    }
  }
  if (pieces.empty())
    throw std::runtime_error("Polyline contains no drawable segments");
  auto p = std::make_shared<std::vector<ParametricCurve>>(std::move(pieces));
  ParametricCurve r;
  r.domain = {{0, double(p->size())}};
  r.dimension = dimension;
  for (size_t i = 1; i < p->size(); ++i)
    r.breaks.push_back(double(i));
  const auto locate = [p](double t) {
    size_t i = t > 0 ? size_t(std::ceil(t) - 1) : 0;
    i = std::min(i, p->size() - 1);
    return std::pair(i, std::clamp(t - i, 0., 1.));
  };
  r.point = [p, locate](double t) {
    const auto [i, s] = locate(t);
    return (*p)[i].point(s);
  };
  r.derivative = [p, locate](double t) {
    const auto [i, s] = locate(t);
    return curveDerivative((*p)[i], s);
  };
  r.sampler = [p](double a, double b, double e, size_t n) {
    const bool reverse = a > b;
    if (reverse)
      std::swap(a, b);
    std::vector<CurveSample> result;
    for (size_t i = 0; i < p->size(); ++i) {
      const double lo = std::max(a, double(i)), hi = std::min(b, double(i + 1));
      if (hi <= lo)
        continue;
      auto v = sampleCurve((*p)[i], lo - i, hi - i, e,
                           n - result.size() + (result.empty() ? 0 : 1));
      if (v.empty())
        return std::vector<CurveSample>{};
      for (auto &s : v)
        s.parameter += i;
      result.insert(result.end(), v.begin() + (result.empty() ? 0 : 1),
                    v.end());
    }
    if (reverse)
      std::ranges::reverse(result);
    return result;
  };
  return r;
}

ParametricCurve Reader::trimmed(const Entity &e) {
  const bool sense = boolean(arg(e, 3));
  const auto preference = enumeration(arg(e, 4));
  if (preference != "PARAMETER" && preference != "CARTESIAN" &&
      preference != "UNSPECIFIED")
    throw std::runtime_error("Invalid trimming preference");
  double spiralLength = 0;
  // Trims provide the finite extent required by sine/cosine spiral definitions.
  for (size_t i : {size_t(1), size_t(2)})
    for (const auto &v : list(arg(e, i), 2))
      if (v.text == "IFCPARAMETERVALUE" && v.list.size() == 1)
        spiralLength += i == 1 ? -number(&v.list[0]) : number(&v.list[0]);
  auto basis = read(reference(arg(e, 0)), std::abs(spiralLength));
  const auto trim = [&](const StepValue *values) {
    std::optional<double> parameter, cartesian;
    std::optional<glm::dvec3> cartesianPoint;
    for (const auto &v : list(values, 2)) {
      if (v.kind == StepValue::Kind::Ref) {
        if (cartesianPoint)
          throw std::runtime_error("Duplicate Cartesian trim");
        const auto p = point(v.ref);
        if (p.second != basis.dimension)
          throw std::runtime_error("Trim point dimension mismatch");
        cartesianPoint = p.first;
      } else {
        if (parameter || v.text != "IFCPARAMETERVALUE" || v.list.size() != 1)
          throw std::runtime_error("Invalid parameter trim");
        parameter = number(&v.list[0]);
      }
    }
    if (parameter && cartesianPoint) {
      auto p = basis.point(*parameter);
      if (!p || glm::length(*p - *cartesianPoint) > tolerance_)
        throw std::runtime_error("Inconsistent parameter and Cartesian trims");
      // The authored parameter also disambiguates a Cartesian point at a
      // self intersection or a repeated spline location.
      cartesian = parameter;
    } else if (cartesianPoint) {
      cartesian = curveParameterAtPoint(basis, *cartesianPoint, tolerance_);
      if (!cartesian)
        throw std::runtime_error(
            "Cartesian trim is off the curve or ambiguous");
    }
    if (parameter && basis.domain && !basis.period &&
        (*parameter < basis.domain->first || *parameter > basis.domain->second))
      throw std::runtime_error("Parameter trim outside basis domain");
    const auto selected = preference == "CARTESIAN" && cartesian ? cartesian
                          : parameter                            ? parameter
                                                                 : cartesian;
    if (!selected)
      throw std::runtime_error("Missing usable curve trim");
    return *selected;
  };
  double first = trim(arg(e, 1)), last = trim(arg(e, 2));
  if (basis.period) {
    first = wrapped(first, basis.period);
    last = wrapped(last, basis.period);
    const double delta =
        wrapped(sense ? last - first : first - last, basis.period);
    if (delta < 1e-12 * basis.period)
      throw std::runtime_error("Coincident periodic trims");
    last = first + (sense ? delta : -delta);
  } else {
    if (basis.domain &&
        (first < basis.domain->first || first > basis.domain->second ||
         last < basis.domain->first || last > basis.domain->second))
      throw std::runtime_error("Trim parameters outside curve domain");
    if ((sense && last <= first) || (!sense && last >= first))
      throw std::runtime_error("Trim order conflicts with curve sense");
  }
  return remap(std::move(basis), first, last, std::abs(last - first));
}

ParametricCurve Reader::spiral(const Entity &e, double length) {
  unsigned dimension = 0;
  const auto position = placement(reference(arg(e, 0)), dimension);
  std::function<double(double)> heading, curvature, bound, curvatureBound;
  double speed = 1, orientation = 1;
  if (e.type == "IFCCLOTHOID") {
    const double a = number(arg(e, 1));
    if (a == 0)
      throw std::runtime_error("Zero clothoid constant");
    speed = std::abs(a) * std::sqrt(pi);
    orientation = a > 0 ? 1 : -1;
    heading = [orientation](double t) { return orientation * pi * t * t * .5; };
    curvature = [orientation, speed](double t) {
      return orientation * pi * t / speed;
    };
    bound = [](double t) { return pi * t * t * .5; };
    curvatureBound = [speed](double t) { return pi * std::abs(t) / speed; };
  } else if (e.type == "IFCCOSINESPIRAL" || e.type == "IFCSINESPIRAL") {
    if (!std::isfinite(length) || length <= 0)
      throw std::runtime_error("Sine/cosine spiral requires a finite segment "
                               "or parameter trim extent");
    const double wave = number(arg(e, 1));
    if (wave == 0)
      throw std::runtime_error("Zero trigonometric spiral term");
    const bool cosine = e.type == "IFCCOSINESPIRAL";
    const size_t constantIndex = cosine ? 2 : 3;
    const double constant =
        omitted(arg(e, constantIndex)) ? 0 : number(arg(e, constantIndex));
    const double linear = cosine || omitted(arg(e, 2)) ? 0 : number(arg(e, 2));
    const double a0 = constant == 0 ? 0 : 1 / constant,
                 a1 = linear == 0
                          ? 0
                          : std::copysign(1., linear) / (linear * linear);
    heading = [=](double s) {
      return a0 * s + (cosine ? length / (pi * wave) * std::sin(pi * s / length)
                              : a1 * s * s * .5 +
                                    length / (2 * pi * wave) *
                                        (1 - std::cos(2 * pi * s / length)));
    };
    curvature = [=](double s) {
      return a0 + (cosine ? std::cos(pi * s / length) / wave
                          : a1 * s + std::sin(2 * pi * s / length) / wave);
    };
    bound = [=](double s) {
      return std::abs(a0 * s) + std::abs(a1 * s * s * .5) +
             2 * length / (pi * std::abs(wave));
    };
    curvatureBound = [=](double s) {
      return std::abs(a0) + std::abs(a1 * s) + 1 / std::abs(wave);
    };
  } else {
    const unsigned order = e.type == "IFCSECONDORDERPOLYNOMIALSPIRAL"  ? 2
                           : e.type == "IFCTHIRDORDERPOLYNOMIALSPIRAL" ? 3
                                                                       : 7;
    std::vector<double> coefficients(order + 1, 0);
    for (unsigned i = 0; i <= order; ++i) {
      const auto *value = arg(e, 1 + order - i);
      if (omitted(value)) {
        if (i == order)
          throw std::runtime_error("Missing leading polynomial spiral term");
        continue;
      }
      const double a = number(value);
      if (a == 0) {
        if (i == order)
          throw std::runtime_error("Zero leading polynomial spiral term");
        continue;
      }
      coefficients[i] =
          std::copysign(1., a) / std::pow(std::abs(a), double(i + 1));
      if (!std::isfinite(coefficients[i]))
        throw std::runtime_error("Polynomial spiral coefficient overflow");
    }
    heading = [coefficients](double s) {
      double result = 0, power = s;
      for (size_t i = 0; i < coefficients.size(); ++i) {
        result += coefficients[i] * power / double(i + 1);
        power *= s;
      }
      return result;
    };
    curvature = [coefficients](double s) {
      double result = 0;
      for (auto i = coefficients.rbegin(); i != coefficients.rend(); ++i)
        result = result * s + *i;
      return result;
    };
    bound = [coefficients](double s) {
      double result = 0, power = std::abs(s);
      for (size_t i = 0; i < coefficients.size(); ++i) {
        result += std::abs(coefficients[i]) * power / double(i + 1);
        power *= std::abs(s);
      }
      return result;
    };
    curvatureBound = [coefficients](double s) {
      double result = 0;
      for (auto i = coefficients.rbegin(); i != coefficients.rend(); ++i)
        result = result * std::abs(s) + std::abs(*i);
      return result;
    };
  }
  ParametricCurve c;
  c.dimension = dimension;
  c.constantSpeed = speed;
  c.curvature = curvature;
  c.stepValidator = [curvatureBound, speed](double a, double b) {
    return std::abs(b - a) * speed *
               curvatureBound(std::max(std::abs(a), std::abs(b))) <=
           pi / 8;
  };
  c.derivative = [heading, speed,
                  orientation](double t) -> std::optional<glm::dvec3> {
    const double h = heading(t);
    if (!std::isfinite(h))
      return std::nullopt;
    return orientation * speed * glm::dvec3(std::cos(h), std::sin(h), 0);
  };
  const auto derivative = c.derivative;
  const double integrationError = chordError_ * .001;
  c.point = [derivative, bound,
             integrationError](double t) -> std::optional<glm::dvec3> {
    const double phase = bound(t);
    if (!std::isfinite(phase) || phase > 512 * pi)
      return std::nullopt;
    return integrateCurveFunction(
        derivative, 0, t, integrationError,
        std::max(size_t(16), size_t(std::ceil(phase * 8 / pi))));
  };
  return transformed(std::move(c), position);
}

ParametricCurve Reader::segment(const Entity &e, bool polynomialProfile) {
  const auto start = measure(arg(e, 2)), length = measure(arg(e, 3));
  unsigned dimension = 0;
  const auto position = placement(reference(arg(e, 1)), dimension);
  if (length.value == 0) {
    ParametricCurve r;
    r.dimension = dimension;
    r.domain = {{0, 0}};
    r.point = [position](double) -> std::optional<glm::dvec3> {
      return position.origin;
    };
    r.frame = [position](double) -> std::optional<CurveFrame> {
      return position;
    };
    return r;
  }
  auto parent = read(reference(arg(e, 4)), std::abs(length.value));
  polynomialProfile =
      polynomialProfile && referenced(e, 4).type == "IFCPOLYNOMIALCURVE";
  if (parent.dimension > dimension)
    throw std::runtime_error("Curve segment/placement dimension mismatch");
  parent.dimension = dimension;
  const double origin =
      parent.domain && !parent.period ? parent.domain->first : 0;
  const double first =
      polynomialProfile ? start.value : toParameter(parent, start, origin);
  const double last = length.parameter || polynomialProfile
                          ? first + length.value
                          : toParameter(parent, {length.value, false}, first);
  if (parent.domain && !parent.period &&
      (first < parent.domain->first || first > parent.domain->second ||
       last < parent.domain->first || last > parent.domain->second))
    throw std::runtime_error("Segment parameters outside parent curve");
  auto frame = curveFrame(parent, first);
  if (polynomialProfile) {
    // Vertical polynomial coefficients already include the grade relative to
    // the station axis. Their length select is the horizontal station extent;
    // normalizing by their initial tangent would remove that authored grade.
    unsigned parentDimension = 0;
    auto axes = placement(reference(arg(referenced(e, 4), 0)), parentDimension);
    const auto p = parent.point(first);
    if (!p)
      throw std::runtime_error("Invalid polynomial profile insertion point");
    axes.origin = *p;
    frame = axes;
  }
  if (!frame)
    throw std::runtime_error("Undefined curve segment insertion tangent");
  if (last < first) {
    frame->x = -frame->x;
    frame->y = -frame->y;
  }
  const double extent = std::abs(length.value);
  ParametricCurve clipped;
  if (length.parameter || parent.constantSpeed || polynomialProfile)
    clipped = remap(parent, first, last, extent);
  else {
    clipped.dimension = parent.dimension;
    clipped.domain = {{0, extent}};
    clipped.constantSpeed = 1;
    const double sign = last > first ? 1 : -1, tolerance = tolerance_ * .01;
    const auto parameter = [parent, first, sign, tolerance](double t) {
      return curveParameterAtDistance(parent, sign * t, first, tolerance);
    };
    clipped.point = [parent, parameter](double t) {
      auto p = parameter(t);
      return p ? parent.point(*p) : std::nullopt;
    };
    clipped.derivative = [parent, parameter,
                          sign](double t) -> std::optional<glm::dvec3> {
      auto p = parameter(t);
      auto d = p ? curveTangent(parent, *p) : std::nullopt;
      return d ? std::optional(*d * sign) : std::nullopt;
    };
    for (double t : parent.breaks)
      if (t > std::min(first, last) && t < std::max(first, last)) {
        auto l = curveLength(parent, first, t, tolerance);
        if (!l)
          throw std::runtime_error("Segment length integration failed");
        clipped.breaks.push_back(*l);
      }
    clipped.sampler = [parent, first, last, extent, sign, parameter,
                       tolerance](double a, double b, double error, size_t n) {
      auto lo = a == 0        ? std::optional(first)
                : a == extent ? std::optional(last)
                              : parameter(a),
           hi = b == 0        ? std::optional(first)
                : b == extent ? std::optional(last)
                              : parameter(b);
      if (!lo || !hi)
        return std::vector<CurveSample>{};
      auto samples = sampleCurve(parent, *lo, *hi, error, n);
      for (auto &sample : samples) {
        auto l = curveLength(parent, first, sample.parameter, tolerance);
        if (!l)
          return std::vector<CurveSample>{};
        sample.parameter = *l;
      }
      return samples;
    };
  }
  CurveFrame inverse;
  inverse.x = {frame->x.x, frame->y.x, frame->z.x};
  inverse.y = {frame->x.y, frame->y.y, frame->z.y};
  inverse.z = {frame->x.z, frame->y.z, frame->z.z};
  inverse.origin = {-glm::dot(frame->origin, frame->x),
                    -glm::dot(frame->origin, frame->y),
                    -glm::dot(frame->origin, frame->z)};
  return transformed(transformed(std::move(clipped), inverse), position);
}

ParametricCurve Reader::composite(const Entity &e,
                                  std::optional<uint32_t> uvBasis) {
  std::vector<ParametricCurve> parts;
  std::vector<double> ends;
  std::vector<std::string> transitions;
  double extent = 0;
  unsigned dimension = 0;
  const auto &segments = list(arg(e, 0), 4096);
  for (size_t i = 0; i < segments.size(); ++i) {
    const auto &s = get(reference(&segments[i]));
    const auto transition = enumeration(arg(s, 0));
    if (transition != "DISCONTINUOUS" && transition != "CONTINUOUS" &&
        transition != "CONTSAMEGRADIENT" &&
        transition != "CONTSAMEGRADIENTSAMECURVATURE")
      throw std::runtime_error("Invalid composite transition");
    if (i + 1 < segments.size() && transition == "DISCONTINUOUS")
      throw std::runtime_error("Internal discontinuity in composite curve");
    ParametricCurve c;
    if (s.type == "IFCCURVESEGMENT")
      c = read(s.id);
    else if (s.type == "IFCCOMPOSITECURVESEGMENT" ||
             s.type == "IFCREPARAMETRISEDCOMPOSITECURVESEGMENT") {
      auto parent = uvBasis ? parameterCurve(reference(arg(s, 2)), *uvBasis)
                            : read(reference(arg(s, 2)));
      if (!parent.domain)
        throw std::runtime_error("Unbounded composite parent needs a trim");
      const bool sense = boolean(arg(s, 1));
      const double a = parent.domain->first, b = parent.domain->second;
      const double length = s.type == "IFCREPARAMETRISEDCOMPOSITECURVESEGMENT"
                                ? number(arg(s, 3))
                                : b - a;
      c = remap(parent, sense ? a : b, sense ? b : a, length);
    } else
      throw std::runtime_error("Unsupported composite segment " + s.type);
    if (!c.domain)
      throw std::runtime_error("Missing bounded composite segment");
    if (dimension && dimension != c.dimension)
      throw std::runtime_error("Mixed composite segment dimensions");
    dimension = c.dimension;
    const auto begin = c.point(c.domain->first),
               end = c.point(c.domain->second);
    if (!begin || !end)
      throw std::runtime_error("Invalid composite segment endpoints");
    if (!parts.empty()) {
      const auto previous = parts.back().point(parts.back().domain->second);
      if (!previous || glm::length(*previous - *begin) > chordError_ * .1)
        throw std::runtime_error(
            "Disconnected composite endpoints at segment #" +
            std::to_string(s.id) + " (gap " +
            std::to_string(previous ? glm::length(*previous - *begin) : -1.) +
            " source units)");
      if (transitions.back() == "CONTSAMEGRADIENT" ||
          transitions.back() == "CONTSAMEGRADIENTSAMECURVATURE") {
        const auto a = curveTangent(parts.back(), parts.back().domain->second);
        const auto b = c.domain->second > c.domain->first
                           ? curveTangent(c, c.domain->first)
                           : std::optional<glm::dvec3>{};
        if (!a || (b && glm::dot(*a, *b) < 1 - 1e-6))
          throw std::runtime_error(
              "Composite transition has incompatible tangents");
      }
    }
    if (c.domain->second == c.domain->first) {
      if (i + 1 != segments.size() || parts.empty())
        throw std::runtime_error(
            "Zero-length segment is only allowed as a final end marker");
      transitions.back() = transition;
      continue;
    }
    extent += c.domain->second - c.domain->first;
    if (!std::isfinite(extent))
      throw std::runtime_error("Composite parameter overflow");
    parts.push_back(c);
    ends.push_back(extent);
    transitions.push_back(transition);
  }
  if (parts.empty())
    throw std::runtime_error("Composite has no drawable segments");
  if ((e.type == "IFCBOUNDARYCURVE" || e.type == "IFCOUTERBOUNDARYCURVE") &&
      transitions.back() == "DISCONTINUOUS")
    throw std::runtime_error("Surface boundary curve must be closed");
  if (transitions.back() != "DISCONTINUOUS") {
    const auto a = parts.front().point(parts.front().domain->first),
               b = parts.back().point(parts.back().domain->second);
    if (!a || !b || glm::length(*a - *b) > chordError_ * .1)
      throw std::runtime_error("Closed composite endpoints do not meet");
  }
  struct Pieces {
    std::vector<ParametricCurve> parts;
    std::vector<double> ends;
  };
  auto pieces =
      std::make_shared<Pieces>(Pieces{std::move(parts), std::move(ends)});
  ParametricCurve c;
  c.domain = {{0, extent}};
  c.dimension = dimension;
  for (size_t i = 0; i < pieces->parts.size(); ++i) {
    const double start = i ? pieces->ends[i - 1] : 0;
    if (i)
      c.breaks.push_back(start);
    for (double t : pieces->parts[i].breaks)
      c.breaks.push_back(start + t - pieces->parts[i].domain->first);
  }
  const auto locate = [pieces](double t) {
    size_t i =
        size_t(std::lower_bound(pieces->ends.begin(), pieces->ends.end(), t) -
               pieces->ends.begin());
    i = std::min(i, pieces->parts.size() - 1);
    const auto domain = *pieces->parts[i].domain;
    return std::pair(
        i, std::clamp(t - (i ? pieces->ends[i - 1] : 0) + domain.first,
                      domain.first, domain.second));
  };
  c.point = [pieces, locate](double t) {
    const auto [i, s] = locate(t);
    return pieces->parts[i].point(s);
  };
  c.derivative = [pieces, locate](double t) {
    const auto [i, s] = locate(t);
    return curveDerivative(pieces->parts[i], s);
  };
  c.frame = [pieces, locate](double t) {
    const auto [i, s] = locate(t);
    return curveFrame(pieces->parts[i], s);
  };
  c.sampler = [pieces](double a, double b, double error, size_t n) {
    bool reverse = a > b;
    if (reverse)
      std::swap(a, b);
    std::vector<CurveSample> result;
    for (size_t i = 0; i < pieces->parts.size(); ++i) {
      const double start = i ? pieces->ends[i - 1] : 0, lo = std::max(a, start),
                   hi = std::min(b, pieces->ends[i]);
      if (lo >= hi)
        continue;
      const auto &part = pieces->parts[i];
      // Subtracting accumulated segment domains can round an endpoint a
      // few ulps outside its child interval. Keep authored trim validation
      // strict and clamp only this derived local coordinate.
      const auto local = [&](double t) {
        return std::clamp(t - start + part.domain->first, part.domain->first,
                          part.domain->second);
      };
      auto samples = sampleCurve(part, local(lo), local(hi), error,
                                 n - result.size() + (result.empty() ? 0 : 1));
      if (samples.empty())
        return std::vector<CurveSample>{};
      for (auto &p : samples)
        p.parameter += start - part.domain->first;
      result.insert(result.end(), samples.begin() + (result.empty() ? 0 : 1),
                    samples.end());
    }
    if (reverse)
      std::ranges::reverse(result);
    return result;
  };
  return c;
}

std::pair<double, glm::dvec3>
Reader::distanceExpression(const Entity &e, const ParametricCurve &basis,
                           uint32_t basisId) {
  if (e.type != "IFCPOINTBYDISTANCEEXPRESSION" ||
      reference(arg(e, 4)) != basisId)
    throw std::runtime_error(
        "Offset distance expression has an incompatible basis curve");
  const double origin = basis.domain ? basis.domain->first : 0;
  const double t = toParameter(basis, measure(arg(e, 0)), origin);
  if (basis.domain && !basis.period &&
      (t < basis.domain->first || t > basis.domain->second))
    throw std::runtime_error("Offset station outside basis curve domain");
  glm::dvec3 offsets(0);
  for (size_t i = 0; i < 3; ++i)
    if (!omitted(arg(e, i + 1)))
      offsets[i] = number(arg(e, i + 1));
  return {t, offsets};
}

ParametricCurve Reader::offset(const Entity &e) {
  const uint32_t basisId = reference(arg(e, 0));
  auto basis = read(basisId);
  ParametricCurve c;
  c.dimension = basis.dimension;
  c.domain = basis.domain;
  c.breaks = basis.breaks;
  c.period = basis.period;
  c.stepValidator = basis.stepValidator;
  if (e.type != "IFCOFFSETCURVEBYDISTANCES") {
    const unsigned dimension = e.type == "IFCOFFSETCURVE2D" ? 2 : 3;
    if (basis.dimension != dimension)
      throw std::runtime_error("Offset curve dimension mismatch");
    const double distance = number(arg(e, 1));
    const glm::dvec3 normal = dimension == 2
                                  ? glm::dvec3(0, 0, 1)
                                  : direction(reference(arg(e, 3)), 3);
    // Constant offsets need a unique tangent, including at composite joins.
    for (double b : basis.breaks)
      if (basis.domain && b > basis.domain->first && b < basis.domain->second) {
        const double h = (basis.domain->second - basis.domain->first) * 1e-8;
        const auto l = curveTangent(basis, b - h),
                   r = curveTangent(basis, b + h);
        if (!l || !r || glm::dot(*l, *r) < 1 - 1e-6)
          throw std::runtime_error("Offset basis has a tangent discontinuity");
      }
    c.point = [basis, distance, normal](double t) -> std::optional<glm::dvec3> {
      auto p = basis.point(t), d = curveTangent(basis, t);
      if (!p || !d)
        return std::nullopt;
      const auto n = glm::cross(normal, *d);
      const double l = glm::length(n);
      if (l < 1e-12)
        return std::nullopt;
      return *p + distance * n / l;
    };
    return c;
  }
  struct Offset {
    double parameter, station;
    glm::dvec3 values;
  };
  std::vector<Offset> offsets;
  const double origin = basis.domain ? basis.domain->first : 0;
  for (const auto &v : list(arg(e, 1), 4096)) {
    const auto [t, values] =
        distanceExpression(get(reference(&v)), basis, basisId);
    const auto length = curveLength(basis, origin, t, tolerance_ * .01);
    if (!length)
      throw std::runtime_error("Offset station length integration failed");
    const double station = t < origin ? -*length : *length;
    if (!offsets.empty() && station <= offsets.back().station)
      throw std::runtime_error("Offset stations must be strictly increasing");
    offsets.push_back({t, station, values});
    c.breaks.push_back(t);
  }
  const double tolerance = tolerance_ * .01;
  c.point = [basis, offsets, origin,
             tolerance](double t) -> std::optional<glm::dvec3> {
    auto f = curveFrame(basis, t);
    if (!f)
      return std::nullopt;
    const auto length = curveLength(basis, origin, t, tolerance);
    if (!length)
      return std::nullopt;
    const double station = t < origin ? -*length : *length;
    auto hi = std::upper_bound(
        offsets.begin(), offsets.end(), station,
        [](double s, const Offset &p) { return s < p.station; });
    glm::dvec3 values;
    if (hi == offsets.begin())
      values = offsets.front().values;
    else if (hi == offsets.end())
      values = offsets.back().values;
    else {
      const auto &lo = *(hi - 1);
      const double fraction =
          (station - lo.station) / (hi->station - lo.station);
      values = lo.values * (1 - fraction) + hi->values * fraction;
    }
    return f->origin + values.x * f->y + values.y * f->z + values.z * f->x;
  };
  return c;
}

ParametricCurve Reader::profile(uint32_t id) {
  const auto &e = get(id);
  if (e.type == "IFCOPENCROSSPROFILEDEF") {
    if (enumeration(arg(e, 0)) != "CURVE" || !angle_ || *angle_ <= 0)
      throw std::runtime_error("Invalid open cross profile type/angle units");
    const bool horizontal = boolean(arg(e, 2));
    const auto widths = numbers(arg(e, 3), 4096),
               slopes = numbers(arg(e, 4), 4096);
    if (widths.size() != slopes.size())
      throw std::runtime_error("Open cross profile width/slope counts differ");
    if (!omitted(arg(e, 5))) {
      const auto &tags = list(arg(e, 5), 4097);
      if (tags.size() != widths.size() + 1)
        throw std::runtime_error(
            "Open cross profile tag count differs from points");
      for (const auto &tag : tags)
        if (tag.kind != StepValue::Kind::String || tag.text.empty())
          throw std::runtime_error("Invalid open cross profile tag");
    }
    glm::dvec3 start(0);
    if (!omitted(arg(e, 6))) {
      auto p = point(reference(arg(e, 6)));
      if (p.second != 2)
        throw std::runtime_error("Open cross profile offset must be 2D");
      start = p.first;
    }
    std::vector<glm::dvec3> points{start};
    std::vector<double> knots{0, 0};
    for (size_t i = 0; i < widths.size(); ++i) {
      const double radians = slopes[i] * *angle_, cosine = std::cos(radians);
      if (widths[i] < 0 || (horizontal && std::abs(cosine) < 1e-12))
        throw std::runtime_error("Degenerate open cross profile width/slope");
      points.push_back(points.back() +
                       glm::dvec3(horizontal ? widths[i] : widths[i] * cosine,
                                  horizontal ? widths[i] * std::tan(radians)
                                             : widths[i] * std::sin(radians),
                                  0));
      knots.push_back(double(i + 1));
    }
    knots.push_back(double(widths.size()));
    consumePoints(points.size());
    auto c = makeBSplineCurve(1, std::move(points), std::move(knots), {}, 2);
    if (!c)
      throw std::runtime_error("Invalid open cross profile coordinates");
    return *c;
  }
  if (e.type == "IFCARBITRARYOPENPROFILEDEF" ||
      e.type == "IFCARBITRARYCLOSEDPROFILEDEF") {
    auto c = read(reference(arg(e, 2)));
    if (c.dimension != 2 || !c.domain)
      throw std::runtime_error(
          "Swept surface profile must be a bounded 2D curve");
    return c;
  }
  CurveFrame f;
  if (!omitted(arg(e, 2))) {
    unsigned d = 0;
    f = placement(reference(arg(e, 2)), d);
    if (d != 2)
      throw std::runtime_error("Swept surface profile placement must be 2D");
  }
  ParametricCurve c;
  c.dimension = 2;
  if (e.type == "IFCCIRCLEPROFILEDEF" || e.type == "IFCELLIPSEPROFILEDEF") {
    const double a = number(arg(e, 3)),
                 b = e.type == "IFCELLIPSEPROFILEDEF" ? number(arg(e, 4)) : a;
    if (a <= 0 || b <= 0 || !angle_ || *angle_ <= 0)
      throw std::runtime_error(
          "Invalid swept surface profile radii/angle units");
    const double radians = *angle_;
    c.period = 2 * pi / radians;
    c.domain = {{0, c.period}};
    c.breaks = {c.period * .25, c.period * .5, c.period * .75};
    c.point = [=](double t) -> std::optional<glm::dvec3> {
      return f.origin + a * std::cos(t * radians) * f.x +
             b * std::sin(t * radians) * f.y;
    };
    c.derivative = [=](double t) -> std::optional<glm::dvec3> {
      return radians * (-a * std::sin(t * radians) * f.x +
                        b * std::cos(t * radians) * f.y);
    };
    return sharedCurve(std::move(c));
  }
  if (e.type == "IFCRECTANGLEPROFILEDEF") {
    const double x = number(arg(e, 3)) * .5, y = number(arg(e, 4)) * .5;
    if (x <= 0 || y <= 0)
      throw std::runtime_error("Invalid swept surface rectangular profile");
    const std::array<glm::dvec3, 5> p{
        {{-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}, {-x, -y, 0}}};
    c.domain = {{0, 4}};
    c.breaks = {1, 2, 3};
    c.point = [=](double t) -> std::optional<glm::dvec3> {
      if (t < 0 || t > 4)
        return std::nullopt;
      const size_t i = std::min(size_t(t), size_t(3));
      const auto v = p[i] + (t - i) * (p[i + 1] - p[i]);
      return f.origin + f.x * v.x + f.y * v.y;
    };
    return sharedCurve(std::move(c));
  }
  throw std::runtime_error("Unsupported swept surface profile " + e.type);
}

ParametricCurve Reader::parameterCurve(uint32_t id, uint32_t basis) {
  const auto &e = get(id);
  const Entity *pc = &e;
  if (e.type == "IFCSURFACECURVE" || e.type == "IFCINTERSECTIONCURVE") {
    pc = nullptr;
    for (const auto &v : list(arg(e, 1), 2)) {
      const auto &candidate = get(reference(&v));
      if (candidate.type == "IFCPCURVE" &&
          reference(arg(candidate, 0)) == basis) {
        if (pc)
          throw std::runtime_error(
              "Ambiguous boundary pcurve for basis surface");
        pc = &candidate;
      }
    }
  }
  if (!pc || pc->type != "IFCPCURVE" || reference(arg(*pc, 0)) != basis)
    throw std::runtime_error(
        "Boundary segment has no pcurve on its basis surface");
  const auto &ref = referenced(*pc, 1);
  auto c = read(ref.type == "IFCDEFINITIONALREPRESENTATION"
                    ? reference(&list(arg(ref, 3), 1).front())
                    : ref.id);
  if (c.dimension != 2)
    throw std::runtime_error("Surface boundary reference curve must be 2D");
  return c;
}

Surface Reader::surface(uint32_t id) {
  if (++nodes_ > 4096 || visiting_.size() >= 64 || !visiting_.insert(id).second)
    throw std::runtime_error("Cyclic or excessive surface reference tree");
  struct Visit {
    std::unordered_set<uint32_t> &set;
    uint32_t id;
    ~Visit() { set.erase(id); }
  } visit{visiting_, id};
  auto result = surfaceImpl(id);
  shareFunction(result.point);
  shareFunction(result.regionStep);
  shareFunction(result.inverse);
  return result;
}

Surface Reader::surfaceImpl(uint32_t id) {
  const auto &e = get(id);
  if (e.type == "IFCSECTIONEDSURFACE") {
    const uint32_t directrixId = reference(arg(e, 0));
    auto directrix = read(directrixId);
    if (directrix.dimension != 3)
      throw std::runtime_error("Sectioned surface directrix must be 3D");
    const auto &positions = list(arg(e, 1), 128),
               &profiles = list(arg(e, 2), 128);
    if (positions.size() < 2 || positions.size() != profiles.size())
      throw std::runtime_error(
          "Sectioned surface section/position counts differ");
    struct Section {
      double parameter, station;
      ParametricCurve curve;
      glm::dvec3 up, normal;
      std::vector<std::string> tags;
    };
    auto sections = std::make_shared<std::vector<Section>>();
    std::string family;
    bool coincidentEdges = false;
    for (size_t i = 0; i < positions.size(); ++i) {
      const auto &position = get(reference(&positions[i])),
                 &p = get(reference(&profiles[i]));
      if (position.type != "IFCAXIS2PLACEMENTLINEAR" ||
          enumeration(arg(p, 0)) != "CURVE" || (i && p.type != family))
        throw std::runtime_error("Sectioned surface requires matching CURVE "
                                 "profiles and linear positions");
      family = p.type;
      const auto &location = referenced(position, 0);
      if (location.type != "IFCPOINTBYDISTANCEEXPRESSION" ||
          reference(arg(location, 4)) != directrixId)
        throw std::runtime_error(
            "Sectioned position must reference its directrix");
      for (size_t offset : {1u, 2u, 3u})
        if (!omitted(arg(location, offset)))
          throw std::runtime_error("Sectioned positions must not have offsets");
      const auto [t, offsets] =
          distanceExpression(location, directrix, directrixId);
      if (i && t <= sections->back().parameter)
        throw std::runtime_error("Sectioned surface positions must increase");
      auto base = curveFrame(directrix, t);
      auto station = curveLength(
          directrix, sections->empty() ? t : sections->front().parameter, t,
          tolerance_ * .01);
      auto c = profile(p.id);
      if (!base || !station || !c.domain || c.period ||
          c.domain->second <= c.domain->first)
        throw std::runtime_error(
            "Sectioned surface requires regular open cross sections");
      const auto endpoints = *c.domain;
      auto start = c.point(endpoints.first), end = c.point(endpoints.second);
      if (!start || !end || glm::length(*start - *end) <= tolerance_)
        throw std::runtime_error(
            "Sectioned surface requires regular open cross sections");
      c = remap(std::move(c), endpoints.first, endpoints.second, 1);
      std::vector<std::string> tags;
      if (p.type == "IFCOPENCROSSPROFILEDEF")
        for (const auto &width : list(arg(p, 3), 4096))
          coincidentEdges |= number(&width) == 0;
      if (p.type == "IFCOPENCROSSPROFILEDEF" && !omitted(arg(p, 5))) {
        for (const auto &tag : list(arg(p, 5), 4097)) {
          if (tag.kind != StepValue::Kind::String || tag.text.empty())
            throw std::runtime_error("Invalid sectioned profile tag");
          tags.push_back(tag.text);
        }
        if (tags.size() != list(arg(p, 3), 4096).size() + 1)
          throw std::runtime_error(
              "Sectioned profile tag count differs from points");
      }
      const auto up = omitted(arg(position, 1))
                          ? base->z
                          : direction(reference(arg(position, 1)), 3);
      auto normal = omitted(arg(position, 2))
                        ? base->x
                        : direction(reference(arg(position, 2)), 3);
      normal = normalized(normal - up * glm::dot(normal, up));
      const auto local = [base](glm::dvec3 p) {
        return glm::dvec3(glm::dot(p, base->x), glm::dot(p, base->y),
                          glm::dot(p, base->z));
      };
      sections->push_back({t, *station, std::move(c), local(up), local(normal),
                           std::move(tags)});
    }
    bool branching = false;
    if (sections->front().tags.empty()) {
      for (const auto &s : *sections)
        if (!s.tags.empty() || s.curve.breaks != sections->front().curve.breaks)
          throw std::runtime_error(
              "Untagged section profiles require matching topology");
    } else {
      // Consecutive occurrences of one tag identify a split/merge breakline.
      // Refine each ordered run at every authored occurrence fraction. A
      // single source point becomes the common tip of all its branches, while
      // larger runs retain their original corners when multiplicities differ.
      using Run = std::pair<std::string, size_t>;
      std::vector<std::vector<Run>> runs;
      std::vector<std::set<double>> divisions;
      for (const auto &s : *sections) {
        auto &current = runs.emplace_back();
        std::unordered_set<std::string> seen;
        for (const auto &tag : s.tags) {
          if (!current.empty() && current.back().first == tag) {
            ++current.back().second;
          } else {
            if (!seen.insert(tag).second)
              throw std::runtime_error(
                  "Crossing or reordered sectioned tag branches");
            current.push_back({tag, 1});
          }
        }
        if (current.size() < 2 ||
            (!divisions.empty() && current.size() != divisions.size()))
          throw std::runtime_error(
              "Sectioned tag transitions require matching ordered runs");
        if (divisions.empty())
          divisions.resize(current.size());
        for (size_t j = 0; j < current.size(); ++j) {
          if (current[j].first != runs.front()[j].first)
            throw std::runtime_error(
                "Sectioned tag transitions require matching ordered runs");
          divisions[j].insert(0);
          if (current[j].second > 1)
            for (size_t k = 0; k < current[j].second; ++k)
              divisions[j].insert(double(k) / (current[j].second - 1));
          branching |= current[j].second != runs.front()[j].second;
        }
      }
      size_t total = 0;
      for (const auto &run : divisions)
        total += run.size();
      if (total > 4097)
        throw std::runtime_error(
            "Sectioned tag branches exceed their point budget");
      for (size_t i = 0; i < sections->size(); ++i) {
        auto &s = (*sections)[i];
        std::vector<double> parameters;
        size_t source = 0;
        for (size_t j = 0; j < divisions.size(); ++j) {
          for (double fraction : divisions[j])
            parameters.push_back((source + fraction * (runs[i][j].second - 1)) /
                                 (s.tags.size() - 1));
          source += runs[i][j].second;
        }
        std::vector<glm::dvec3> points;
        std::vector<double> knots{0, 0};
        for (size_t k = 0; k < total; ++k) {
          const auto p = s.curve.point(parameters[k]);
          if (!p)
            throw std::runtime_error("Undefined sectioned branch point");
          points.push_back(*p);
          if (k)
            knots.push_back(double(k) / (total - 1));
        }
        knots.push_back(1);
        consumePoints(points.size());
        auto aligned =
            makeBSplineCurve(1, std::move(points), std::move(knots), {}, 2);
        if (!aligned)
          throw std::runtime_error("Invalid sectioned branch profile");
        s.curve = sharedCurve(std::move(*aligned));
      }
    }
    const double first = sections->front().parameter,
                 last = sections->back().parameter;
    std::vector<double> uBreaks{first, last};
    for (double t : directrix.breaks)
      if (t > first && t < last)
        uBreaks.push_back(t);
    for (const auto &s : *sections)
      uBreaks.push_back(s.parameter);
    std::ranges::sort(uBreaks);
    uBreaks.erase(std::unique(uBreaks.begin(), uBreaks.end()), uBreaks.end());
    std::map<double, glm::dvec3> miters;
    for (size_t i = 1; i + 1 < uBreaks.size(); ++i) {
      const double t = uBreaks[i],
                   h = std::min(t - uBreaks[i - 1], uBreaks[i + 1] - t) * 1e-7;
      auto a = curveTangent(directrix, t - h),
           b = curveTangent(directrix, t + h);
      if (!a || !b)
        throw std::runtime_error("Undefined sectioned directrix tangent");
      const double cosine = glm::dot(*a, *b);
      if (cosine >= 1 - 1e-6)
        continue;
      const double halfCosine = std::sqrt(std::max(0., (1 + cosine) / 2));
      if (halfCosine < .1)
        throw std::runtime_error(
            "Sectioned directrix miter exceeds its 10x extension limit");
      miters[t] = normalized(*a + *b) / halfCosine;
    }
    Surface result{
        .point = [directrix, sections, tolerance = tolerance_ * .01](
                     double u, double v) -> std::optional<glm::dvec3> {
          if (!std::isfinite(u) || !std::isfinite(v) || v < 0 || v > 1 ||
              u < sections->front().parameter || u > sections->back().parameter)
            return std::nullopt;
          auto upper = std::upper_bound(
              sections->begin(), sections->end(), u,
              [](double t, const Section &s) { return t < s.parameter; });
          const size_t i =
              upper == sections->end()
                  ? sections->size() - 2
                  : std::max(size_t(1), size_t(upper - sections->begin())) - 1;
          const auto &a = (*sections)[i], &b = (*sections)[i + 1];
          auto distance = curveLength(directrix, a.parameter, u, tolerance);
          auto f = curveFrame(directrix, u);
          auto p = a.curve.point(v), q = b.curve.point(v);
          if (!distance || !f || !p || !q || b.station <= a.station)
            return std::nullopt;
          const double fraction =
              std::clamp(*distance / (b.station - a.station), 0., 1.);
          const auto world = [f](glm::dvec3 d) {
            return f->x * d.x + f->y * d.y + f->z * d.z;
          };
          auto up = world(a.up * (1 - fraction) + b.up * fraction),
               normal = world(a.normal * (1 - fraction) + b.normal * fraction);
          if (glm::length(up) <= 1e-12 || glm::length(normal) <= 1e-12)
            return std::nullopt;
          up = glm::normalize(up);
          normal -= up * glm::dot(normal, up);
          if (glm::length(normal) <= 1e-12)
            return std::nullopt;
          normal = glm::normalize(normal);
          const auto x = glm::cross(up, normal),
                     local = *p * (1 - fraction) + *q * fraction;
          const auto point = f->origin + x * local.x + up * local.y;
          return finite(point) ? std::optional(point) : std::nullopt;
        },
        .angularScale = {directrix.period ? 2 * pi / directrix.period : 0, 0},
        .uDomain = {{first, last}},
        .vDomain = {{0, 1}}};
    result.allowCoincidentEdges = branching || coincidentEdges;
    if (!miters.empty()) {
      if (uBreaks.size() > 4097)
        throw std::runtime_error("Sectioned miter exceeds its anchor budget");
      const auto frame = curveFrame(directrix, first);
      if (!frame)
        throw std::runtime_error("Undefined sectioned miter frame");
      const auto &initial = sections->front();
      const auto up =
          normalized(frame->x * initial.up.x + frame->y * initial.up.y +
                     frame->z * initial.up.z);
      for (const auto &s : *sections) {
        const auto f = curveFrame(directrix, s.parameter);
        if (!f)
          throw std::runtime_error("Undefined sectioned miter section frame");
        const auto worldUp = f->x * s.up.x + f->y * s.up.y + f->z * s.up.z,
                   worldNormal = f->x * s.normal.x + f->y * s.normal.y +
                                 f->z * s.normal.z;
        if (glm::dot(worldUp, up) < 1 - 1e-8 ||
            glm::dot(worldNormal, f->x) < 1 - 1e-8)
          throw std::runtime_error(
              "Sectioned miters require a common perpendicular axis and "
              "tangent profile normals");
      }
      std::vector<glm::dvec3> anchors;
      std::vector<double> stations;
      for (double u : uBreaks) {
        const auto point = directrix.point(u);
        const auto station = curveLength(directrix, first, u, tolerance_ * .01);
        if (!point || !station || !finite(*point))
          throw std::runtime_error("Undefined sectioned miter anchor");
        anchors.push_back(*point);
        stations.push_back(*station);
      }
      for (size_t i = 1; i < anchors.size(); ++i) {
        const auto d = anchors[i] - anchors[i - 1];
        if (glm::length(d) <= tolerance_ * .01 ||
            std::abs(glm::dot(normalized(d), up)) > 1e-7)
          throw std::runtime_error(
              "Sectioned miters require planar nonzero directrix segments");
        for (double t : {.25, .5, .75}) {
          const auto p =
              directrix.point(std::lerp(uBreaks[i - 1], uBreaks[i], t));
          if (!p ||
              glm::length(*p - (anchors[i - 1] + d * t)) > tolerance_ * .01)
            throw std::runtime_error("Sharp sectioned joins require piecewise "
                                     "linear directrix spans");
        }
      }
      const auto x = normalized(anchors[1] - anchors[0]), y = glm::cross(up, x);
      const auto uv = [&](glm::dvec3 p) {
        const auto d = p - anchors.front();
        return glm::dvec2(glm::dot(d, x), glm::dot(d, y));
      };
      const auto orient = [](glm::dvec2 a, glm::dvec2 b, glm::dvec2 c) {
        const auto p = b - a, q = c - a;
        return p.x * q.y - p.y * q.x;
      };
      size_t checks = 0;
      for (size_t i = 1; i < anchors.size(); ++i)
        for (size_t j = i + 2; j < anchors.size(); ++j) {
          if (++checks > 8000000)
            throw std::runtime_error(
                "Sectioned directrix intersection budget exceeded");
          const auto a = uv(anchors[i - 1]), b = uv(anchors[i]),
                     c = uv(anchors[j - 1]), d = uv(anchors[j]);
          const auto on = [&](glm::dvec2 p, glm::dvec2 q, glm::dvec2 r) {
            const double tolerance = tolerance_ * .01;
            return std::abs(orient(q, r, p)) <=
                       tolerance * glm::length(r - q) &&
                   p.x >= std::min(q.x, r.x) - tolerance &&
                   p.x <= std::max(q.x, r.x) + tolerance &&
                   p.y >= std::min(q.y, r.y) - tolerance &&
                   p.y <= std::max(q.y, r.y) + tolerance;
          };
          if ((orient(a, b, c) * orient(a, b, d) < 0 &&
               orient(c, d, a) * orient(c, d, b) < 0) ||
              on(a, c, d) || on(b, c, d) || on(c, a, b) || on(d, a, b))
            throw std::runtime_error("Sectioned directrix intersects itself");
        }
      // Straight spans are ruled between authored sections and miter anchors.
      // Both incident spans reuse the same half-angle cross section exactly.
      const auto control = [=](size_t k,
                               double v) -> std::optional<glm::dvec3> {
        const double u = uBreaks[k];
        auto upper = std::upper_bound(
            sections->begin(), sections->end(), u,
            [](double t, const Section &s) { return t < s.parameter; });
        const size_t i =
            upper == sections->end()
                ? sections->size() - 2
                : std::max(size_t(1), size_t(upper - sections->begin())) - 1;
        const auto &a = (*sections)[i], &b = (*sections)[i + 1];
        const auto p = a.curve.point(v), q = b.curve.point(v);
        if (!p || !q)
          return std::nullopt;
        const double fraction = std::clamp(
            (stations[k] - a.station) / (b.station - a.station), 0., 1.);
        const auto local = *p * (1 - fraction) + *q * fraction;
        const auto direction =
            miters.contains(u) ? miters.at(u)
                               : normalized(k + 1 < anchors.size()
                                                ? anchors[k + 1] - anchors[k]
                                                : anchors[k] - anchors[k - 1]);
        return anchors[k] + glm::cross(up, direction) * local.x + up * local.y;
      };
      result.point = [=](double u, double v) -> std::optional<glm::dvec3> {
        if (!std::isfinite(u) || !std::isfinite(v) || u < first || u > last ||
            v < 0 || v > 1)
          return std::nullopt;
        const auto upper = std::upper_bound(uBreaks.begin(), uBreaks.end(), u);
        const size_t i =
            upper == uBreaks.end()
                ? uBreaks.size() - 2
                : std::max(size_t(1), size_t(upper - uBreaks.begin())) - 1;
        const auto a = control(i, v), b = control(i + 1, v);
        if (!a || !b)
          return std::nullopt;
        return *a * (1 - (u - uBreaks[i]) / (uBreaks[i + 1] - uBreaks[i])) +
               *b * ((u - uBreaks[i]) / (uBreaks[i + 1] - uBreaks[i]));
      };
      result.meshUp = up;
    }
    const auto vBreaks = sections->front().curve.breaks;
    if (meshing_) {
      auto samples =
          sampleCurve(directrix, first, last, chordError_ * .25, 4097);
      if (samples.empty())
        throw std::runtime_error(
            "Sectioned directrix exceeds its mesh sample budget");
      result.meshU = uBreaks;
      std::erase_if(result.meshU,
                    [first, last](double t) { return t < first || t > last; });
      for (const auto &p : samples)
        result.meshU.push_back(p.parameter);
      result.meshV = {0, 1};
      for (const auto &section : *sections) {
        auto points = sampleCurve(section.curve, 0, 1, chordError_ * .25, 4097);
        if (points.empty())
          throw std::runtime_error(
              "Sectioned profile exceeds its mesh sample budget");
        for (const auto &p : points)
          result.meshV.push_back(p.parameter);
      }
      for (auto *values : {&result.meshU, &result.meshV}) {
        std::ranges::sort(*values);
        values->erase(std::unique(values->begin(), values->end()),
                      values->end());
      }
    }
    result.regionStep = [uBreaks, vBreaks](glm::dvec2 a, glm::dvec2 b) {
      const auto spansMultiple = [](const auto &breaks, double a, double b) {
        auto first =
            std::upper_bound(breaks.begin(), breaks.end(), std::min(a, b));
        auto last =
            std::lower_bound(breaks.begin(), breaks.end(), std::max(a, b));
        return last - first > 1;
      };
      return !spansMultiple(uBreaks, a.x, b.x) &&
             !spansMultiple(vBreaks, a.y, b.y);
    };
    return result;
  }
  if (e.type == "IFCSURFACEOFLINEAREXTRUSION" ||
      e.type == "IFCSURFACEOFREVOLUTION") {
    auto c = profile(reference(arg(e, 0)));
    CurveFrame f;
    if (!omitted(arg(e, 1))) {
      unsigned d = 0;
      f = placement(reference(arg(e, 1)), d);
      if (d != 3)
        throw std::runtime_error("Swept surface placement must be 3D");
    }
    const auto place = [f](glm::dvec3 p) {
      return f.origin + f.x * p.x + f.y * p.y + f.z * p.z;
    };
    if (e.type == "IFCSURFACEOFLINEAREXTRUSION") {
      const double depth = number(arg(e, 3));
      if (depth <= 0)
        throw std::runtime_error("Nonpositive swept surface depth");
      const auto axis = direction(reference(arg(e, 2)), 3) * depth;
      return {.point = [=](double u, double v) -> std::optional<glm::dvec3> {
                if (!std::isfinite(u) || !std::isfinite(v) ||
                    (c.domain && !c.period &&
                     (u < c.domain->first || u > c.domain->second)))
                  return std::nullopt;
                auto p = c.point(u);
                if (!p)
                  return std::nullopt;
                const auto result = place(*p + axis * v);
                return finite(result) ? std::optional(result) : std::nullopt;
              },
              .angularScale = {c.period ? 2 * pi / c.period : 0, 0},
              .periods = {c.period, 0},
              .uDomain = c.domain,
              .inverse = [=, tolerance = tolerance_ * .01](
                             glm::dvec3 p) -> std::optional<glm::dvec2> {
                if (c.dimension != 2 || std::abs(axis.z) <= 1e-12)
                  return std::nullopt;
                const auto d = p - f.origin;
                const glm::dvec3 local(glm::dot(d, f.x), glm::dot(d, f.y),
                                       glm::dot(d, f.z));
                const double v = local.z / axis.z;
                // Face boundaries have already been stored as renderer floats.
                // Account for that rounding here; the face mesher independently
                // verifies surface membership and the physical chord budget.
                const double positionTolerance = std::max(
                    tolerance, glm::length(local) *
                                   std::numeric_limits<float>::epsilon() * 2);
                auto u = curveParameterAtPoint(c, local - axis * v,
                                               positionTolerance);
                return u ? std::optional(glm::dvec2(*u, v)) : std::nullopt;
              }};
    }
    const auto &axisPlacement = referenced(e, 2);
    if (axisPlacement.type != "IFCAXIS1PLACEMENT")
      throw std::runtime_error("Invalid surface revolution axis placement");
    const auto origin = point(reference(arg(axisPlacement, 0)));
    if (origin.second != 3 || !angle_ || *angle_ <= 0)
      throw std::runtime_error("Invalid surface revolution origin/angle units");
    const auto axis = omitted(arg(axisPlacement, 1))
                          ? glm::dvec3(0, 0, 1)
                          : direction(reference(arg(axisPlacement, 1)), 3);
    const double radians = *angle_;
    return {.point = [=](double u, double v) -> std::optional<glm::dvec3> {
              if (!std::isfinite(u) || !std::isfinite(v) ||
                  (c.domain && !c.period &&
                   (v < c.domain->first || v > c.domain->second)))
                return std::nullopt;
              auto p = c.point(v);
              if (!p)
                return std::nullopt;
              const auto q = *p - origin.first;
              const double cosine = std::cos(u * radians),
                           sine = std::sin(u * radians);
              const auto result =
                  place(origin.first + q * cosine + glm::cross(axis, q) * sine +
                        axis * glm::dot(axis, q) * (1 - cosine));
              return finite(result) ? std::optional(result) : std::nullopt;
            },
            .angularScale = {radians, c.period ? 2 * pi / c.period : 0},
            .periods = {2 * pi / radians, c.period},
            .vDomain = c.domain};
  }
  if (e.type == "IFCRECTANGULARTRIMMEDSURFACE") {
    auto base = surface(reference(arg(e, 0)));
    const double u1 = number(arg(e, 1)), v1 = number(arg(e, 2)),
                 u2 = number(arg(e, 3)), v2 = number(arg(e, 4));
    const bool us = boolean(arg(e, 5)), vs = boolean(arg(e, 6));
    const auto extent = [](double a, double b, bool sense, double period) {
      if (a == b || (!period && sense != (b > a)))
        throw std::runtime_error("Incompatible rectangular surface trim/sense");
      double d = (sense ? 1 : -1) * (b - a);
      if (period && d <= 0)
        d += (std::floor(-d / period) + 1) * period;
      if (!std::isfinite(d) || d <= 0)
        throw std::runtime_error("Invalid rectangular surface trim extent");
      return d;
    };
    const double du = extent(u1, u2, us, base.periods.x),
                 dv = extent(v1, v2, vs, 0);
    for (double u : {u1, u1 + (us ? du : -du)})
      for (double v : {v1, v2})
        if (!base.point(u, v))
          throw std::runtime_error(
              "Rectangular surface trim outside basis domain");
    Surface result{
        .point = [=](double u, double v) -> std::optional<glm::dvec3> {
          if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u > du ||
              v < 0 || v > dv)
            return std::nullopt;
          return base.point(u1 + (us ? u : -u), v1 + (vs ? v : -v));
        },
        .angularScale = base.angularScale,
        .uDomain = {{0, du}},
        .vDomain = {{0, dv}}};
    if (base.regionStep)
      result.regionStep = [=](glm::dvec2 a, glm::dvec2 b) {
        const auto map = [=](glm::dvec2 p) {
          return glm::dvec2(u1 + (us ? p.x : -p.x), v1 + (vs ? p.y : -p.y));
        };
        return base.regionStep(map(a), map(b));
      };
    return result;
  }
  if (e.type == "IFCCURVEBOUNDEDPLANE" || e.type == "IFCCURVEBOUNDEDSURFACE") {
    const uint32_t basisId = reference(arg(e, 0));
    auto base = surface(basisId);
    const bool plane = e.type == "IFCCURVEBOUNDEDPLANE";
    if (plane && get(basisId).type != "IFCPLANE")
      throw std::runtime_error("Curve bounded plane requires a plane basis");
    const bool implicit = !plane && boolean(arg(e, 2));
    if (implicit && (!base.uDomain || !base.vDomain))
      throw std::runtime_error("Bounded surface has no implicit outer domain");
    const double tolerance = plane ? tolerance_ : 1e-6;
    std::vector<std::vector<glm::dvec2>> rings;
    std::optional<size_t> outer;
    size_t count = 0;
    const auto boundary = [&](uint32_t ref, bool isOuter) {
      auto c = plane ? read(ref) : composite(get(ref), basisId);
      if (c.dimension != 2 || !c.domain)
        throw std::runtime_error(
            "Surface boundary must have a bounded 2D parameter curve");
      auto points = sampleCurve(c, c.domain->first, c.domain->second,
                                plane ? chordError_ * .1 : 1e-6, 4097);
      if (points.size() < 4 ||
          glm::length(points.front().point - points.back().point) > tolerance)
        throw std::runtime_error(
            "Surface boundary is not closed or exceeds its sample budget");
      if (isOuter) {
        if (outer)
          throw std::runtime_error("Multiple outer surface boundaries");
        outer = rings.size();
      }
      auto &ring = rings.emplace_back();
      for (size_t i = 0; i + 1 < points.size(); ++i)
        ring.emplace_back(points[i].point);
      count += ring.size();
      if (count > 4096)
        throw std::runtime_error("Surface boundary sample budget exceeded");
    };
    if (plane) {
      boundary(reference(arg(e, 1)), true);
      const auto *inner = arg(e, 2);
      if (!inner || inner->kind != StepValue::Kind::List ||
          inner->list.size() > 4096)
        throw std::runtime_error("Invalid inner surface boundary list");
      for (const auto &v : inner->list)
        boundary(reference(&v), false);
    } else {
      for (const auto &v : list(arg(e, 1), 4096)) {
        const uint32_t ref = reference(&v);
        const auto &b = get(ref);
        if (b.type != "IFCBOUNDARYCURVE" && b.type != "IFCOUTERBOUNDARYCURVE")
          throw std::runtime_error("Invalid bounded surface boundary family");
        boundary(ref, b.type == "IFCOUTERBOUNDARYCURVE");
      }
      if (implicit && outer)
        throw std::runtime_error(
            "Implicit surface also declares an outer boundary");
    }
    const auto orient = [](glm::dvec2 a, glm::dvec2 b, glm::dvec2 c) {
      const auto x = b - a, y = c - a;
      return x.x * y.y - x.y * y.x;
    };
    const auto classify = [tolerance](glm::dvec2 p,
                                      const std::vector<glm::dvec2> &ring) {
      bool inside = false;
      for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const auto a = ring[j], b = ring[i], d = b - a;
        const double n = glm::dot(d, d);
        if (n > 0 && glm::length(p - a -
                                 d * std::clamp(glm::dot(p - a, d) / n, 0.,
                                                1.)) <= tolerance)
          return 0;
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y))
          inside = !inside;
      }
      return inside ? 1 : -1;
    };
    for (size_t r = 0; r < rings.size(); ++r) {
      const auto &a = rings[r];
      double area = 0;
      for (size_t i = 0; i < a.size(); ++i)
        area += orient(glm::dvec2(0), a[i], a[(i + 1) % a.size()]);
      if (std::abs(area) <= tolerance * tolerance)
        throw std::runtime_error("Degenerate surface boundary area");
      if (!plane && !implicit && !outer && area > 0) {
        // Outer boundaries follow the positive surface normal in UV space.
        outer = r;
      }
      for (size_t i = 0; i < a.size(); ++i) {
        const auto p = a[i], q = a[(i + 1) % a.size()];
        if (glm::length(q - p) <= tolerance * .01)
          throw std::runtime_error("Degenerate surface boundary edge");
        if (!base.point(p.x, p.y))
          throw std::runtime_error("Surface boundary outside its basis domain");
        for (size_t s = r; s < rings.size(); ++s) {
          const auto &b = rings[s];
          for (size_t j = s == r ? i + 1 : 0; j < b.size(); ++j) {
            if (s == r && (j == i + 1 || (i == 0 && j + 1 == a.size())))
              continue;
            const auto x = b[j], y = b[(j + 1) % b.size()];
            if (std::max(p.x, q.x) < std::min(x.x, y.x) ||
                std::max(x.x, y.x) < std::min(p.x, q.x) ||
                std::max(p.y, q.y) < std::min(x.y, y.y) ||
                std::max(x.y, y.y) < std::min(p.y, q.y))
              continue;
            if (orient(p, q, x) * orient(p, q, y) <= 0 &&
                orient(x, y, p) * orient(x, y, q) <= 0)
              throw std::runtime_error("Intersecting surface boundary loops");
          }
        }
      }
    }
    if (!implicit && !outer)
      throw std::runtime_error("No outer surface boundary");
    for (size_t r = 0; r < rings.size(); ++r) {
      if (outer && r == *outer)
        continue;
      if (outer && classify(rings[r].front(), rings[*outer]) != 1)
        throw std::runtime_error("Inner surface boundary outside outer loop");
      for (size_t s = r + 1; s < rings.size(); ++s)
        if ((!outer || s != *outer) &&
            (classify(rings[r].front(), rings[s]) >= 0 ||
             classify(rings[s].front(), rings[r]) >= 0))
          throw std::runtime_error("Nested inner surface boundaries");
    }
    auto loops = std::make_shared<std::vector<std::vector<glm::dvec2>>>(
        std::move(rings));
    const auto contains = [loops, outer, classify](glm::dvec2 p) {
      if (outer && classify(p, (*loops)[*outer]) < 0)
        return false;
      for (size_t i = 0; i < loops->size(); ++i)
        if ((!outer || i != *outer) && classify(p, (*loops)[i]) > 0)
          return false;
      return true;
    };
    Surface result{
        .point = [base, contains](double u,
                                  double v) -> std::optional<glm::dvec3> {
          if (!std::isfinite(u) || !std::isfinite(v))
            return std::nullopt;
          const glm::dvec2 p(u, v);
          if (!contains(p))
            return std::nullopt;
          return base.point(u, v);
        },
        .angularScale = base.angularScale,
        .periods = base.periods,
        .uDomain = base.uDomain,
        .vDomain = base.vDomain};
    result.regionStep = [base, loops, contains, orient](glm::dvec2 a,
                                                        glm::dvec2 b) {
      if (base.regionStep && !base.regionStep(a, b))
        return false;
      const auto d = b - a;
      std::vector<double> cuts{0, 1};
      for (const auto &ring : *loops)
        for (size_t i = 0; i < ring.size(); ++i) {
          const auto c = ring[i], v = ring[(i + 1) % ring.size()] - c;
          const double determinant = orient(glm::dvec2(0), d, v);
          if (std::abs(determinant) <= 1e-15 * glm::length(d) * glm::length(v))
            continue;
          const double t = orient(glm::dvec2(0), c - a, v) / determinant,
                       u = orient(glm::dvec2(0), c - a, d) / determinant;
          if (t > 0 && t < 1 && u >= 0 && u <= 1)
            cuts.push_back(t);
        }
      std::ranges::sort(cuts);
      for (size_t i = 1; i < cuts.size(); ++i)
        if (!contains(a + d * (cuts[i - 1] + (cuts[i] - cuts[i - 1]) * .5)))
          return false;
      return true;
    };
    return result;
  }
  if (e.type == "IFCBSPLINESURFACEWITHKNOTS" ||
      e.type == "IFCRATIONALBSPLINESURFACEWITHKNOTS") {
    const double du = number(arg(e, 0)), dv = number(arg(e, 1));
    if (du < 1 || dv < 1 || du > 32 || dv > 32 || du != std::floor(du) ||
        dv != std::floor(dv))
      throw std::runtime_error("Invalid pcurve surface degrees");
    std::vector<std::vector<glm::dvec3>> grid;
    for (const auto &row : list(arg(e, 2), 256)) {
      std::vector<glm::dvec3> points;
      for (const auto &r : list(&row, 256)) {
        auto p = point(reference(&r));
        if (p.second != 3)
          throw std::runtime_error("Spline surface controls must be 3D");
        points.push_back(p.first);
      }
      if (!grid.empty() && points.size() != grid.front().size())
        throw std::runtime_error("Ragged spline surface control grid");
      grid.push_back(std::move(points));
    }
    const size_t nu = grid.size(), nv = grid.front().size();
    consumePoints(nu * nv);
    if (nu * nv > 4096)
      throw std::runtime_error("Excessive spline surface control grid");
    const auto expand = [&](size_t mi, size_t ki, size_t count, double degree) {
      auto multiplicities = numbers(arg(e, mi), 256),
           values = numbers(arg(e, ki), 256);
      if (values.size() != multiplicities.size())
        throw std::runtime_error("Spline surface knot counts mismatch");
      std::vector<double> knots;
      for (size_t i = 0; i < values.size(); ++i) {
        double m = multiplicities[i];
        if ((i && values[i] <= values[i - 1]) || m < 1 || m > degree + 1 ||
            m != std::floor(m) || knots.size() + m > count + degree + 1)
          throw std::runtime_error("Invalid spline surface knots");
        knots.insert(knots.end(), size_t(m), values[i]);
      }
      return knots;
    };
    const auto ku = expand(7, 9, nu, du), kv = expand(8, 10, nv, dv);
    auto ucurve = makeBSplineCurve(
             unsigned(du), std::vector<glm::dvec3>(nu, glm::dvec3(0)), ku),
         vcurve = makeBSplineCurve(
             unsigned(dv), std::vector<glm::dvec3>(nv, glm::dvec3(0)), kv);
    if (!ucurve || !vcurve)
      throw std::runtime_error("Invalid spline surface knot domain");
    std::vector<std::vector<double>> weights(nu, std::vector<double>(nv, 1));
    if (e.type == "IFCRATIONALBSPLINESURFACEWITHKNOTS") {
      const auto &rows = list(arg(e, 12), 256);
      if (rows.size() != nu)
        throw std::runtime_error("Spline surface weight row count mismatch");
      double scale = 0;
      for (size_t i = 0; i < nu; ++i) {
        weights[i] = numbers(&rows[i], 256);
        if (weights[i].size() != nv)
          throw std::runtime_error(
              "Spline surface weight column count mismatch");
        for (double w : weights[i]) {
          if (w <= 0)
            throw std::runtime_error("Nonpositive spline surface weight");
          scale = std::max(scale, w);
        }
      }
      for (auto &row : weights)
        for (auto &w : row) {
          w /= scale;
          if (w <= 0)
            throw std::runtime_error(
                "Spline surface weight range exceeds precision");
        }
    }
    const auto basis = [](const std::vector<double> &knots, size_t count,
                          unsigned degree, double t) {
      std::vector<double> b(count + degree, 0);
      if (t == knots[count])
        t = std::nextafter(t, -std::numeric_limits<double>::infinity());
      for (size_t i = 0; i < b.size(); ++i)
        b[i] = knots[i] <= t && t < knots[i + 1] ? 1 : 0;
      for (unsigned p = 1; p <= degree; ++p)
        for (size_t i = 0; i < count + degree - p; ++i) {
          const double l = knots[i + p] - knots[i],
                       r = knots[i + p + 1] - knots[i + 1];
          b[i] = (l > 0 ? (t - knots[i]) * b[i] / l : 0) +
                 (r > 0 ? (knots[i + p + 1] - t) * b[i + 1] / r : 0);
        }
      b.resize(count);
      return b;
    };
    const auto ud = *ucurve->domain, vd = *vcurve->domain;
    return {.point = [=](double u, double v) -> std::optional<glm::dvec3> {
              if (!std::isfinite(u) || !std::isfinite(v) || u < ud.first ||
                  u > ud.second || v < vd.first || v > vd.second)
                return std::nullopt;
              const auto bu = basis(ku, nu, unsigned(du), u),
                         bv = basis(kv, nv, unsigned(dv), v);
              glm::dvec3 p(0);
              double w = 0;
              for (size_t i = 0; i < nu; ++i)
                for (size_t j = 0; j < nv; ++j) {
                  const double f = bu[i] * bv[j] * weights[i][j];
                  p += grid[i][j] * f;
                  w += f;
                }
              return w > 0 && finite(p / w) ? std::optional(p / w)
                                            : std::nullopt;
            },
            .uDomain = ud,
            .vDomain = vd,
            .meshU = ucurve->breaks,
            .meshV = vcurve->breaks};
  }
  if (e.type != "IFCPLANE" && e.type != "IFCCYLINDRICALSURFACE" &&
      e.type != "IFCSPHERICALSURFACE" && e.type != "IFCTOROIDALSURFACE")
    throw std::runtime_error("Unsupported pcurve basis surface " + e.type);
  unsigned dimension = 0;
  const auto f = placement(reference(arg(e, 0)), dimension);
  if (dimension != 3)
    throw std::runtime_error("Pcurve surface placement must be 3D");
  if (e.type == "IFCPLANE")
    return {.point = [f](double u, double v) -> std::optional<glm::dvec3> {
              const auto p = f.origin + u * f.x + v * f.y;
              return finite(p) ? std::optional(p) : std::nullopt;
            },
            .inverse = [f](glm::dvec3 p) -> std::optional<glm::dvec2> {
              const auto d = p - f.origin;
              return glm::dvec2(glm::dot(d, f.x), glm::dot(d, f.y));
            }};
  if (!angle_ || *angle_ <= 0)
    throw std::runtime_error("Invalid pcurve surface angular units");
  const double radians = *angle_, radius = number(arg(e, 1)),
               minor = e.type == "IFCTOROIDALSURFACE" ? number(arg(e, 2)) : 0;
  if (radius <= 0 ||
      (e.type == "IFCTOROIDALSURFACE" && (minor <= 0 || minor >= radius)))
    throw std::runtime_error("Invalid pcurve surface radii");
  const auto type = e.type;
  Surface result{
      .point = [=](double u, double v) -> std::optional<glm::dvec3> {
        if (!std::isfinite(u) || !std::isfinite(v))
          return std::nullopt;
        u = wrapped(u, 2 * pi / radians);
        const auto radial =
            f.x * std::cos(u * radians) + f.y * std::sin(u * radians);
        if (type == "IFCCYLINDRICALSURFACE")
          return f.origin + radius * radial + v * f.z;
        if (type == "IFCSPHERICALSURFACE") {
          if (std::abs(v * radians) > pi * .5)
            return std::nullopt;
          return f.origin + radius * (radial * std::cos(v * radians) +
                                      f.z * std::sin(v * radians));
        }
        v = wrapped(v, 2 * pi / radians);
        return f.origin + (radius + minor * std::cos(v * radians)) * radial +
               minor * std::sin(v * radians) * f.z;
      },
      .angularScale = {radians, type == "IFCCYLINDRICALSURFACE" ? 0 : radians},
      .periods = {2 * pi / radians,
                  type == "IFCTOROIDALSURFACE" ? 2 * pi / radians : 0}};
  if (type == "IFCSPHERICALSURFACE") {
    result.vDomain = {{-pi / (2 * radians), pi / (2 * radians)}};
    result.sphere = SphericalSurface{f, radius};
  }
  result.inverse = [=](glm::dvec3 p) -> std::optional<glm::dvec2> {
    const auto d = p - f.origin;
    const double x = glm::dot(d, f.x), y = glm::dot(d, f.y),
                 z = glm::dot(d, f.z), r = std::hypot(x, y);
    if (!finite(d) || r <= radius * 1e-12)
      return std::nullopt; // Polar singularities need explicit vertex-loop
                           // charts.
    const double u = wrapped(std::atan2(y, x) / radians, 2 * pi / radians);
    const double v =
        type == "IFCCYLINDRICALSURFACE" ? z
        : type == "IFCSPHERICALSURFACE"
            ? std::atan2(z, r) / radians
            : wrapped(std::atan2(z, r - radius) / radians, 2 * pi / radians);
    return glm::dvec2(u, v);
  };
  return result;
}

ParametricCurve Reader::pcurve(const Entity &e) {
  const auto evaluate = surface(reference(arg(e, 0)));
  const auto &referenceCurve = referenced(e, 1);
  // IFC4.3 references the 2D curve directly. Older exchanges can use a
  // definitional representation containing that single reference curve.
  uint32_t id = referenceCurve.id;
  if (referenceCurve.type == "IFCDEFINITIONALREPRESENTATION") {
    const auto &items = list(arg(referenceCurve, 3), 1);
    id = reference(&items.front());
  }
  auto uv = read(id);
  if (uv.dimension != 2)
    throw std::runtime_error("Pcurve reference curve must be 2D");
  ParametricCurve c;
  c.dimension = 3;
  c.domain = uv.domain;
  c.breaks = uv.breaks;
  c.period = uv.period;
  c.point = [uv, evaluate](double t) {
    auto p = uv.point(t);
    return p ? evaluate.point(p->x, p->y) : std::nullopt;
  };
  if (evaluate.angularScale != glm::dvec2(0) || evaluate.regionStep ||
      uv.stepValidator) {
    c.stepValidator = [uv, scale = evaluate.angularScale,
                       region = evaluate.regionStep](double a, double b) {
      if (uv.stepValidator && !uv.stepValidator(a, b))
        return false;
      glm::dvec3 low(std::numeric_limits<double>::infinity()),
          high(-std::numeric_limits<double>::infinity());
      std::optional<glm::dvec2> previous;
      for (unsigned i = 0; i <= 4; ++i) {
        auto p = uv.point(a + (b - a) * i / 4);
        if (!p)
          return false;
        if (previous && region && !region(*previous, glm::dvec2(*p)))
          return false;
        previous = glm::dvec2(*p);
        low = glm::min(low, *p);
        high = glm::max(high, *p);
      }
      return glm::dot(scale, glm::dvec2(high - low)) <= pi / 8;
    };
  }
  return c;
}

std::vector<std::array<glm::dvec3, 3>> Reader::sectionedMesh(uint32_t id) {
  if (get(id).type != "IFCSECTIONEDSURFACE")
    throw std::runtime_error("Expected a sectioned surface");
  meshing_ = true;
  auto s = surface(id);
  auto u = std::move(s.meshU), v = std::move(s.meshV);
  std::map<std::pair<double, double>, glm::dvec3> cache;
  const auto eval = [&](double a, double b) {
    const auto key = std::pair(a, b);
    if (auto it = cache.find(key); it != cache.end())
      return it->second;
    if (cache.size() >= 262144)
      throw std::runtime_error("Sectioned surface evaluation budget exceeded");
    auto p = s.point(a, b);
    if (!p || !finite(*p))
      throw std::runtime_error("Undefined sectioned surface mesh point");
    cache.emplace(key, *p);
    return *p;
  };
  const auto distance = [](glm::dvec3 p, glm::dvec3 a, glm::dvec3 b,
                           glm::dvec3 c) {
    const auto normal = glm::cross(b - a, c - a);
    const double n = glm::dot(normal, normal);
    if (n > 0) {
      const auto projected = p - normal * (glm::dot(p - a, normal) / n);
      if (glm::dot(glm::cross(b - a, projected - a), normal) >= -n * 1e-12 &&
          glm::dot(glm::cross(c - b, projected - b), normal) >= -n * 1e-12 &&
          glm::dot(glm::cross(a - c, projected - c), normal) >= -n * 1e-12)
        return std::abs(glm::dot(p - a, normal)) / std::sqrt(n);
    }
    double result = std::numeric_limits<double>::infinity();
    for (const auto &edge :
         {std::pair(a, b), std::pair(b, c), std::pair(c, a)}) {
      const auto d = edge.second - edge.first;
      const double length = glm::dot(d, d);
      result = std::min(
          result,
          glm::length(
              p - edge.first -
              d * (length > 0 ? std::clamp(glm::dot(p - edge.first, d) / length,
                                           0., 1.)
                              : 0.)));
    }
    return result;
  };
  for (unsigned depth = 0;; ++depth) {
    if (u.size() < 2 || v.size() < 2 || u.size() > 4097 || v.size() > 4097 ||
        (u.size() - 1) * (v.size() - 1) > 500000)
      throw std::runtime_error(
          "Sectioned surface triangle/grid budget exceeded");
    std::vector<double> addU, addV;
    for (size_t i = 1; i < u.size(); ++i)
      for (size_t j = 1; j < v.size(); ++j) {
        const auto a = eval(u[i - 1], v[j - 1]), b = eval(u[i], v[j - 1]),
                   c = eval(u[i], v[j]), d = eval(u[i - 1], v[j]);
        bool splitU = false, splitV = false;
        for (double x : {0., .25, .5, .75, 1.})
          for (double y : {0., .25, .5, .75, 1.}) {
            const auto actual = eval(std::lerp(u[i - 1], u[i], x),
                                     std::lerp(v[j - 1], v[j], y));
            if (std::min(distance(actual, a, b, c), distance(actual, a, c, d)) >
                chordError_ * .25) {
              splitU = splitU || (x > 0 && x < 1);
              splitV = splitV || (y > 0 && y < 1);
            }
          }
        if (splitU)
          addU.push_back(u[i - 1] + (u[i] - u[i - 1]) * .5);
        if (splitV)
          addV.push_back(v[j - 1] + (v[j] - v[j - 1]) * .5);
      }
    if (addU.empty() && addV.empty())
      break;
    if (depth >= 16)
      throw std::runtime_error(
          "Sectioned surface could not resolve its chord error");
    for (const auto &values : {std::pair(&u, &addU), std::pair(&v, &addV)}) {
      const auto before = values.first->size();
      values.first->insert(values.first->end(), values.second->begin(),
                           values.second->end());
      std::ranges::sort(*values.first);
      values.first->erase(
          std::unique(values.first->begin(), values.first->end()),
          values.first->end());
      if (!values.second->empty() && before == values.first->size())
        throw std::runtime_error(
            "Sectioned surface parameter precision exhausted");
    }
  }
  std::vector<std::array<glm::dvec3, 3>> triangles;
  triangles.reserve((u.size() - 1) * (v.size() - 1) * 2);
  for (size_t i = 1; i < u.size(); ++i)
    for (size_t j = 1; j < v.size(); ++j) {
      const auto a = eval(u[i - 1], v[j - 1]), b = eval(u[i], v[j - 1]),
                 c = eval(u[i], v[j]), d = eval(u[i - 1], v[j]);
      for (const std::array<glm::dvec3, 3> triangle :
           {std::array{a, b, c}, std::array{a, c, d}}) {
        const auto normal =
            glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
        if (glm::length(normal) <= 1e-15) {
          if (s.allowCoincidentEdges && triangle[0] != triangle[1] &&
              (triangle[0] == triangle[2] || triangle[1] == triangle[2]))
            continue;
          if (s.allowCoincidentEdges && triangle[0] == triangle[1] &&
              triangle[0] != triangle[2])
            continue;
          throw std::runtime_error(
              "Sectioned surface has a collapsed mesh triangle");
        }
        if (s.meshUp && glm::dot(normal, *s.meshUp) <= 0)
          throw std::runtime_error(
              "Sectioned miter folds or reverses its surface");
        triangles.push_back(triangle);
      }
    }
  return triangles;
}

ParametricCurve Reader::alignment(const Entity &e) {
  const uint32_t baseId = reference(arg(e, 2));
  auto base = read(baseId);
  if (!base.domain)
    throw std::runtime_error("Alignment base must be bounded");
  const bool gradient = e.type == "IFCGRADIENTCURVE";
  if ((gradient && base.dimension != 2) || (!gradient && base.dimension != 3))
    throw std::runtime_error("Alignment base dimension mismatch");
  // Gradient and cant parameters remain those of the original horizontal base.
  uint32_t horizontalId = baseId;
  std::unordered_set<uint32_t> horizontalReferences;
  while (get(horizontalId).type == "IFCGRADIENTCURVE" ||
         get(horizontalId).type == "IFCSEGMENTEDREFERENCECURVE") {
    if (!horizontalReferences.insert(horizontalId).second ||
        horizontalReferences.size() > 64)
      throw std::runtime_error("Cyclic alignment base");
    horizontalId = reference(arg(get(horizontalId), 2));
  }
  auto horizontal = horizontalId == baseId ? base : read(horizontalId);
  if (!horizontal.domain || horizontal.dimension != 2)
    throw std::runtime_error("Alignment requires a bounded horizontal base");
  const double origin = horizontal.domain->first, tolerance = tolerance_ * .01;
  const auto station = [horizontal, origin, tolerance](double t) {
    return curveLength(horizontal, origin, t, tolerance);
  };
  const auto parameter = [horizontal, origin, tolerance](double s) {
    return curveParameterAtDistance(horizontal, s, origin, tolerance);
  };
  const auto &segments = list(arg(e, 0), 4096);
  struct Part {
    ParametricCurve curve, parent;
    CurveFrame start, end;
    double first, last, begin, finish;
  };
  std::vector<Part> parts;
  std::optional<CurveFrame> explicitEnd;
  if (!omitted(arg(e, 3))) {
    unsigned d = 0;
    explicitEnd = placement(reference(arg(e, 3)), d);
    if (d != (gradient ? 2u : 3u))
      throw std::runtime_error("Alignment end placement dimension mismatch");
  }
  for (size_t i = 0; i < segments.size(); ++i) {
    const auto &s = get(reference(&segments[i]));
    if (s.type != "IFCCURVESEGMENT")
      throw std::runtime_error(
          "Alignment interpolation requires curve segments");
    const auto transition = enumeration(arg(s, 0));
    if (transition != "DISCONTINUOUS" && transition != "CONTINUOUS" &&
        transition != "CONTSAMEGRADIENT" &&
        transition != "CONTSAMEGRADIENTSAMECURVATURE")
      throw std::runtime_error("Invalid alignment transition");
    if (i + 1 < segments.size() && transition == "DISCONTINUOUS")
      throw std::runtime_error("Internal alignment discontinuity");
    const auto length = measure(arg(s, 3));
    unsigned dimension = 0;
    const auto start = placement(reference(arg(s, 1)), dimension);
    if (dimension != (gradient ? 2u : 3u))
      throw std::runtime_error(
          "Alignment interpolation placement dimension mismatch");
    if (length.value == 0) {
      if (i + 1 != segments.size() || parts.empty())
        throw std::runtime_error("Invalid alignment end marker");
      if (explicitEnd &&
          glm::length(explicitEnd->origin - start.origin) > tolerance_)
        throw std::runtime_error("Conflicting alignment end placements");
      explicitEnd = start;
      continue;
    }
    if (gradient) {
      auto curve = segment(s, true);
      const auto a = curve.point(curve.domain->first),
                 b = curve.point(curve.domain->second);
      if (!a || !b || b->x <= a->x)
        throw std::runtime_error(
            "Vertical alignment has no increasing station extent");
      for (unsigned j = 0; j <= 64; ++j) {
        const double t = curve.domain->first +
                         (curve.domain->second - curve.domain->first) * j / 64;
        const auto d = curveDerivative(curve, t);
        if (!d || d->x <= 0)
          throw std::runtime_error(
              "Vertical alignment reverses its station direction");
      }
      if (!parts.empty() &&
          glm::length(*a - *parts.back().curve.point(
                               parts.back().curve.domain->second)) >
              chordError_ * .1)
        throw std::runtime_error(
            "Disconnected vertical alignment profile at segment #" +
            std::to_string(s.id) + " (gap " +
            std::to_string(glm::length(
                *a -
                *parts.back().curve.point(parts.back().curve.domain->second))) +
            " source units)");
      parts.push_back({curve, {}, start, {}, 0, 0, a->x, b->x});
    } else {
      auto parent = read(reference(arg(s, 4)), std::abs(length.value));
      const auto startMeasure = measure(arg(s, 2));
      const double pOrigin = parent.domain && !parent.period
                                 ? parent.domain->first
                                 : 0,
                   first = toParameter(parent, startMeasure, pOrigin);
      const double last = length.parameter ? first + length.value
                                           : toParameter(parent, length, first);
      auto physicalLength = curveLength(parent, first, last, tolerance);
      if (!physicalLength || *physicalLength <= 0)
        throw std::runtime_error("Invalid cant segment length");
      if (std::abs(start.origin.z) > tolerance_)
        throw std::runtime_error(
            "Cant placement is outside station/elevation plane");
      if (!parts.empty() &&
          std::abs(start.origin.x - parts.back().finish) > tolerance_)
        throw std::runtime_error("Disconnected cant stations");
      parts.push_back({{},
                       parent,
                       start,
                       {},
                       first,
                       last,
                       start.origin.x,
                       start.origin.x + *physicalLength});
    }
  }
  if (parts.empty())
    throw std::runtime_error("Alignment has no nonzero interpolation segments");
  if (gradient) {
    if (explicitEnd) {
      const auto end =
          parts.back().curve.point(parts.back().curve.domain->second);
      if (!end || glm::length(*end - explicitEnd->origin) > tolerance_)
        throw std::runtime_error(
            "Vertical alignment end marker does not match profile");
    }
  } else {
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i + 1 < parts.size())
        parts[i].end = parts[i + 1].start;
      else if (explicitEnd)
        parts[i].end = *explicitEnd;
      else {
        parts[i].end = parts[i].start;
        parts[i].end.origin.x = parts[i].finish;
      }
      if (std::abs(parts[i].end.origin.x - parts[i].finish) > tolerance_)
        throw std::runtime_error("Cant end placement station mismatch");
    }
  }
  const auto baseFirst = station(base.domain->first),
             baseLast = station(base.domain->second);
  if (!baseFirst || !baseLast)
    throw std::runtime_error("Alignment horizontal station integration failed");
  const double firstStation = std::max(*baseFirst, parts.front().begin),
               lastStation = std::min(*baseLast, parts.back().finish);
  if (firstStation >= lastStation)
    throw std::runtime_error(
        "Alignment interpolation has no common base domain");
  auto first = parameter(firstStation), last = parameter(lastStation);
  if (!first || !last)
    throw std::runtime_error(
        "Alignment station cannot be converted to base parameter");
  auto shared = std::make_shared<std::vector<Part>>(std::move(parts));
  const auto locate = [shared](double s) -> const Part & {
    auto it =
        std::lower_bound(shared->begin(), shared->end(), s,
                         [](const Part &p, double t) { return p.finish < t; });
    return it == shared->end() ? shared->back() : *it;
  };
  const auto curvature = [](const ParametricCurve &curve,
                            double t) -> std::optional<double> {
    if (curve.curvature) {
      const double k = curve.curvature(t);
      return std::isfinite(k) ? std::optional(k) : std::nullopt;
    }
    {
      const auto d = curveDerivative(curve, t);
      const double h = curve.domain
                           ? (curve.domain->second - curve.domain->first) * 1e-5
                           : 1e-5;
      const double a = curve.domain && !curve.period
                           ? std::max(curve.domain->first, t - h)
                           : t - h,
                   b = curve.domain && !curve.period
                           ? std::min(curve.domain->second, t + h)
                           : t + h;
      const auto l = curveDerivative(curve, a), r = curveDerivative(curve, b);
      if (!d || !l || !r)
        return std::nullopt;
      if (b <= a)
        return std::nullopt;
      const auto second = (*r - *l) / (b - a);
      const double speed = glm::length(*d);
      if (speed <= 0)
        return std::nullopt;
      const auto frame = curveFrame(curve, t);
      if (!frame)
        return std::nullopt;
      return glm::dot(glm::cross(*d, second), frame->z) /
             (speed * speed * speed);
    }
    return std::nullopt;
  };
  const auto cant = [locate, curvature, tolerance](
                        double s) -> std::optional<std::pair<double, double>> {
    const auto &p = locate(s);
    const double f = std::clamp((s - p.begin) / (p.finish - p.begin), 0., 1.);
    const auto parameter = curveParameterAtDistance(
        p.parent, (p.last > p.first ? 1 : -1) * (p.finish - p.begin) * f,
        p.first, tolerance);
    if (!parameter)
      return std::nullopt;
    const double t = *parameter;
    const auto k0 = curvature(p.parent, p.first),
               k1 = curvature(p.parent, p.last), k = curvature(p.parent, t);
    if (!k0 || !k1 || !k)
      return std::nullopt;
    const double delta = *k1 - *k0;
    const double progress = std::abs(delta) > 1e-14 ? (*k - *k0) / delta : f;
    if (!std::isfinite(progress))
      return std::nullopt;
    const double elevation =
        p.start.origin.y + (p.end.origin.y - p.start.origin.y) * progress;
    const double a = std::atan2(-p.start.z.y, p.start.z.z),
                 b = std::atan2(-p.end.z.y, p.end.z.z);
    const double angle = a + std::remainder(b - a, 2 * pi) * progress;
    return std::pair(elevation, angle);
  };
  ParametricCurve c;
  c.dimension = 3;
  c.domain = {{*first, *last}};
  c.breaks = base.breaks;
  for (const auto &p : *shared) {
    auto t = parameter(p.begin);
    if (t && *t > *first && *t < *last)
      c.breaks.push_back(*t);
  }
  c.point = [base, station, locate, gradient,
             cant](double t) -> std::optional<glm::dvec3> {
    auto b = base.point(t);
    auto s = station(t);
    if (!b || !s)
      return std::nullopt;
    if (!gradient) {
      auto value = cant(*s);
      if (!value)
        return std::nullopt;
      b->z += value->first;
      return b;
    }
    const auto &p = locate(*s);
    double lo = p.curve.domain->first, hi = p.curve.domain->second;
    for (unsigned i = 0; i < 56; ++i) {
      const double mid = lo + (hi - lo) * .5;
      const auto v = p.curve.point(mid);
      if (!v)
        return std::nullopt;
      if (v->x < *s)
        lo = mid;
      else
        hi = mid;
    }
    const auto v = p.curve.point(lo + (hi - lo) * .5);
    return v ? std::optional(glm::dvec3(b->x, b->y, v->y)) : std::nullopt;
  };
  if (!gradient) {
    const auto unrolled = c;
    c.frame = [unrolled, station, cant](double t) -> std::optional<CurveFrame> {
      auto f = curveFrame(unrolled, t);
      auto s = station(t);
      auto v = s ? cant(*s) : std::nullopt;
      if (!f || !v)
        return std::nullopt;
      const auto y = f->y * std::cos(v->second) + f->z * std::sin(v->second),
                 z = -f->y * std::sin(v->second) + f->z * std::cos(v->second);
      f->y = y;
      f->z = z;
      return f;
    };
  }
  return c;
}

ParametricCurve Reader::read(uint32_t id, double spiralLength) {
  return sharedCurve(readImpl(id, spiralLength));
}

ParametricCurve Reader::readImpl(uint32_t id, double spiralLength) {
  if (++nodes_ > 4096 || visiting_.size() >= 64)
    throw std::runtime_error(
        "Excessive curve reference depth or component count");
  if (!visiting_.insert(id).second)
    throw std::runtime_error("Cyclic curve reference #" + std::to_string(id));
  struct Visit {
    std::unordered_set<uint32_t> &set;
    uint32_t id;
    ~Visit() { set.erase(id); }
  } visit{visiting_, id};
  const auto &e = get(id);
  try {
    if (e.type == "IFCPOLYLINE" || e.type == "IFCINDEXEDPOLYCURVE")
      return polyline(e);
    if (e.type == "IFCBSPLINECURVEWITHKNOTS" ||
        e.type == "IFCRATIONALBSPLINECURVEWITHKNOTS")
      return spline(e);
    if (e.type == "IFCTRIMMEDCURVE")
      return trimmed(e);
    if (e.type == "IFCCURVESEGMENT")
      return segment(e);
    if (e.type == "IFCCOMPOSITECURVE" ||
        e.type == "IFCCOMPOSITECURVEONSURFACE" ||
        e.type == "IFCBOUNDARYCURVE" || e.type == "IFCOUTERBOUNDARYCURVE")
      return composite(e);
    if (e.type == "IFCGRADIENTCURVE" || e.type == "IFCSEGMENTEDREFERENCECURVE")
      return alignment(e);
    if (e.type == "IFCOFFSETCURVE2D" || e.type == "IFCOFFSETCURVE3D" ||
        e.type == "IFCOFFSETCURVEBYDISTANCES")
      return offset(e);
    if (e.type == "IFCPCURVE")
      return pcurve(e);
    if (e.type == "IFCCLOTHOID" || e.type == "IFCCOSINESPIRAL" ||
        e.type == "IFCSINESPIRAL" ||
        e.type == "IFCSECONDORDERPOLYNOMIALSPIRAL" ||
        e.type == "IFCTHIRDORDERPOLYNOMIALSPIRAL" ||
        e.type == "IFCSEVENTHORDERPOLYNOMIALSPIRAL")
      return spiral(e, spiralLength);
    if (e.type == "IFCLINE") {
      const auto p = point(reference(arg(e, 0)));
      const auto &v = referenced(e, 1);
      if (v.type != "IFCVECTOR")
        throw std::runtime_error("Missing line vector");
      const double magnitude = number(arg(v, 1));
      if (magnitude <= 0)
        throw std::runtime_error("Nonpositive line vector magnitude");
      const auto d = direction(reference(arg(v, 0)), p.second) * magnitude;
      ParametricCurve c;
      c.dimension = p.second;
      c.constantSpeed = magnitude;
      c.point = [p, d](double t) -> std::optional<glm::dvec3> {
        const auto q = p.first + t * d;
        return finite(q) ? std::optional(q) : std::nullopt;
      };
      c.derivative = [d](double) -> std::optional<glm::dvec3> { return d; };
      c.inverse = [p, d](glm::dvec3 q) -> std::optional<double> {
        return glm::dot(q - p.first, d) / glm::dot(d, d);
      };
      c.sampler = [c](double a, double b, double, size_t n) {
        auto p = c.point(a), q = c.point(b);
        return p && q && n >= 2 ? std::vector<CurveSample>{{a, *p}, {b, *q}}
                                : std::vector<CurveSample>{};
      };
      return c;
    }
    if (e.type == "IFCCIRCLE" || e.type == "IFCELLIPSE") {
      unsigned dimension = 0;
      auto f = placement(reference(arg(e, 0)), dimension);
      const double a = number(arg(e, 1)),
                   b = e.type == "IFCELLIPSE" ? number(arg(e, 2)) : a;
      if (a <= 0 || b <= 0 || !angle_ || *angle_ <= 0)
        throw std::runtime_error(
            "Invalid conic radii or project angular units");
      const double radians = *angle_, period = 2 * pi / radians;
      ParametricCurve c;
      c.dimension = dimension;
      c.domain = {{0, period}};
      c.period = period;
      c.breaks = {period * .25, period * .5, period * .75};
      if (a == b)
        c.constantSpeed = a * radians;
      c.point = [a, b, radians, period,
                 f](double t) -> std::optional<glm::dvec3> {
        const double s = wrapped(t, period) * radians;
        return f.origin + a * f.x * std::cos(s) + b * f.y * std::sin(s);
      };
      c.derivative = [a, b, radians, period,
                      f](double t) -> std::optional<glm::dvec3> {
        const double s = wrapped(t, period) * radians;
        return radians * (-a * f.x * std::sin(s) + b * f.y * std::cos(s));
      };
      c.inverse = [a, b, radians, period,
                   f](glm::dvec3 p) -> std::optional<double> {
        const auto q = p - f.origin;
        return wrapped(std::atan2(glm::dot(q, f.y) / b, glm::dot(q, f.x) / a) /
                           radians,
                       period);
      };
      c.frame = [c, f](double t) -> std::optional<CurveFrame> {
        auto p = c.point(t), d = curveTangent(c, t);
        if (!p || !d)
          return std::nullopt;
        return CurveFrame{*p, *d, glm::cross(f.z, *d), f.z};
      };
      const auto evaluate = c.point;
      const double radius = std::max(a, b);
      c.sampler = [evaluate, radians, radius](double first, double last,
                                              double error, size_t n) {
        const double step = std::min(
            pi / 8, 2 * std::acos(std::clamp(1 - error / radius, -1., 1.)));
        if (step <= 0)
          return std::vector<CurveSample>{};
        const double count = std::ceil(std::abs(last - first) * radians / step);
        if (!std::isfinite(count) || count < 1 || count + 1 > n)
          return std::vector<CurveSample>{};
        std::vector<CurveSample> r;
        for (size_t i = 0; i <= size_t(count); ++i) {
          const double t = first + (last - first) * double(i) / count;
          auto p = evaluate(t);
          if (!p)
            return std::vector<CurveSample>{};
          r.push_back({t, *p});
        }
        return r;
      };
      return c;
    }
    if (e.type == "IFCPOLYNOMIALCURVE") {
      unsigned dimension = 0;
      const auto f = placement(reference(arg(e, 0)), dimension);
      std::array<std::vector<double>, 3> coefficients;
      for (size_t i = 0; i < 3; ++i)
        if (!omitted(arg(e, i + 1)))
          coefficients[i] = numbers(arg(e, i + 1), 33);
      if (coefficients[0].empty() || coefficients[1].empty() ||
          (dimension == 2 && !coefficients[2].empty()) ||
          (dimension == 3 && coefficients[2].empty()))
        throw std::runtime_error(
            "Invalid polynomial coefficients or placement dimension");
      ParametricCurve c;
      c.dimension = dimension;
      c.point = [coefficients](double t) -> std::optional<glm::dvec3> {
        glm::dvec3 p(0);
        for (size_t i = 0; i < 3; ++i)
          for (auto v = coefficients[i].rbegin(); v != coefficients[i].rend();
               ++v)
            p[i] = p[i] * t + *v;
        return finite(p) ? std::optional(p) : std::nullopt;
      };
      c.derivative = [coefficients](double t) -> std::optional<glm::dvec3> {
        glm::dvec3 p(0);
        for (size_t i = 0; i < 3; ++i)
          for (size_t j = coefficients[i].size(); j > 1; --j)
            p[i] = p[i] * t + double(j - 1) * coefficients[i][j - 1];
        return finite(p) ? std::optional(p) : std::nullopt;
      };
      c.sampler = [coefficients, dimension](double first, double last,
                                            double error, size_t limit) {
        size_t degree = 0;
        for (const auto &v : coefficients)
          degree = std::max(degree, v.empty() ? size_t(0) : v.size() - 1);
        if (!degree)
          return std::vector<CurveSample>{};
        const auto binomial = [](size_t n, size_t k) {
          double b = 1;
          for (size_t i = 1; i <= k; ++i)
            b = b * double(n - k + i) / double(i);
          return b;
        };
        std::vector<glm::dvec3> powers(degree + 1, glm::dvec3(0)),
            controls(degree + 1, glm::dvec3(0));
        for (size_t axis = 0; axis < 3; ++axis)
          for (size_t j = 0; j < coefficients[axis].size(); ++j)
            for (size_t k = j; k < coefficients[axis].size(); ++k)
              powers[j][axis] += coefficients[axis][k] * binomial(k, j) *
                                 std::pow(first, double(k - j)) *
                                 std::pow(last - first, double(j));
        for (size_t i = 0; i <= degree; ++i)
          for (size_t j = 0; j <= i; ++j)
            controls[i] += powers[j] * (binomial(i, j) / binomial(degree, j));
        std::vector<double> knots(degree + 1, 0);
        knots.insert(knots.end(), degree + 1, 1);
        auto bezier = makeBSplineCurve(unsigned(degree), std::move(controls),
                                       std::move(knots), {}, dimension);
        if (!bezier)
          return std::vector<CurveSample>{};
        auto samples = sampleCurve(*bezier, 0, 1, error, limit);
        for (auto &sample : samples)
          sample.parameter = first + (last - first) * sample.parameter;
        return samples;
      };
      if (std::ranges::all_of(coefficients,
                              [](const auto &a) { return a.size() <= 2; })) {
        auto d = c.derivative(0);
        if (d && glm::length(*d) > 0)
          c.constantSpeed = glm::length(*d);
      }
      return transformed(std::move(c), f);
    }
    if (e.type == "IFCSURFACECURVE" || e.type == "IFCINTERSECTIONCURVE" ||
        e.type == "IFCSEAMCURVE") {
      auto c = read(reference(arg(e, 0)));
      if (c.dimension != 3)
        throw std::runtime_error("Surface curve's explicit curve must be 3D");
      const auto &associated = list(arg(e, 1), 2);
      for (const auto &item : associated)
        get(reference(&item));
      if (e.type == "IFCSEAMCURVE") {
        if (associated.size() != 2 ||
            reference(&associated[0]) == reference(&associated[1]))
          throw std::runtime_error("Seam curve requires two distinct pcurves");
        const auto &a = get(reference(&associated[0])),
                   &b = get(reference(&associated[1]));
        if (a.type != "IFCPCURVE" || b.type != "IFCPCURVE" ||
            reference(arg(a, 0)) != reference(arg(b, 0)))
          throw std::runtime_error(
              "Seam pcurves must reference the same surface");
      }
      const auto master = enumeration(arg(e, 2));
      if (master == "CURVE3D")
        return c;
      const size_t i = master == "PCURVE_S1"   ? 0
                       : master == "PCURVE_S2" ? 1
                                               : associated.size();
      if (i >= associated.size())
        throw std::runtime_error("Invalid surface curve master representation");
      const auto &pc = get(reference(&associated[i]));
      if (pc.type != "IFCPCURVE")
        throw std::runtime_error(
            "Preferred surface curve representation is not a pcurve");
      return read(pc.id);
    }
    throw std::runtime_error("Unsupported curve family " + e.type);
  } catch (const std::runtime_error &error) {
    throw std::runtime_error(e.type + " #" + std::to_string(id) + ": " +
                             error.what());
  }
}

} // namespace

bool isCurveEntity(std::string_view type) {
  constexpr std::array types{"IFCPOLYLINE",
                             "IFCINDEXEDPOLYCURVE",
                             "IFCCIRCLE",
                             "IFCELLIPSE",
                             "IFCLINE",
                             "IFCBSPLINECURVEWITHKNOTS",
                             "IFCRATIONALBSPLINECURVEWITHKNOTS",
                             "IFCTRIMMEDCURVE",
                             "IFCCOMPOSITECURVE",
                             "IFCCOMPOSITECURVEONSURFACE",
                             "IFCBOUNDARYCURVE",
                             "IFCOUTERBOUNDARYCURVE",
                             "IFCOFFSETCURVE2D",
                             "IFCOFFSETCURVE3D",
                             "IFCOFFSETCURVEBYDISTANCES",
                             "IFCPCURVE",
                             "IFCSURFACECURVE",
                             "IFCSEAMCURVE",
                             "IFCINTERSECTIONCURVE",
                             "IFCPOLYNOMIALCURVE",
                             "IFCCLOTHOID",
                             "IFCCOSINESPIRAL",
                             "IFCSINESPIRAL",
                             "IFCSECONDORDERPOLYNOMIALSPIRAL",
                             "IFCTHIRDORDERPOLYNOMIALSPIRAL",
                             "IFCSEVENTHORDERPOLYNOMIALSPIRAL",
                             "IFCGRADIENTCURVE",
                             "IFCSEGMENTEDREFERENCECURVE",
                             "IFCCURVESEGMENT"};
  return std::ranges::find(types, type) != types.end();
}
std::optional<ParametricCurve>
readIfcCurve(const std::unordered_map<uint32_t, Entity> &entities, uint32_t id,
             double units, std::optional<double> angle, std::string &error) {
  try {
    if (!std::isfinite(units) || units <= 0)
      throw std::runtime_error("Invalid curve length units");
    return Reader(entities, units, angle).read(id);
  } catch (const std::runtime_error &e) {
    error = e.what();
    return std::nullopt;
  }
}
std::optional<ParametricSurface>
readIfcSurface(const std::unordered_map<uint32_t, Entity> &entities,
               uint32_t id, double units, std::optional<double> angle,
               std::string &error) {
  try {
    if (!std::isfinite(units) || units <= 0)
      throw std::runtime_error("Invalid surface length units");
    return Reader(entities, units, angle).surface(id);
  } catch (const std::runtime_error &e) {
    error = e.what();
    return std::nullopt;
  }
}
std::vector<std::array<glm::dvec3, 3>> readIfcSectionedSurfaceMesh(
    const std::unordered_map<uint32_t, Entity> &entities, uint32_t id,
    double units, std::optional<double> angle, std::string &error) {
  try {
    if (!std::isfinite(units) || units <= 0)
      throw std::runtime_error("Invalid sectioned surface length units");
    return Reader(entities, units, angle).sectionedMesh(id);
  } catch (const std::runtime_error &e) {
    error = e.what();
    return {};
  }
}
} // namespace container::geometry::ifc::detail
