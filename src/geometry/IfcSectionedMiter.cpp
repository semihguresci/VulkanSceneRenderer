#include "IfcSectionedMiter.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace container::geometry::ifc::detail {
namespace {

constexpr size_t maxSamples = 4097;
constexpr size_t maxChecks = 8'000'000;
constexpr size_t maxCellProbes = 262144;
constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();

bool finite(glm::dvec3 p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool validBreaks(std::span<const double> breaks) {
  return std::ranges::all_of(breaks,
                             [](double t) { return std::isfinite(t); }) &&
         std::adjacent_find(breaks.begin(), breaks.end(),
                            std::greater_equal<>()) == breaks.end();
}

[[noreturn]] void fail(double station, const char *reason) {
  throw std::runtime_error("Sectioned miter at parameter " +
                           std::to_string(station) + ": " + reason);
}

glm::dvec3 unit(glm::dvec3 point, double station) {
  const double length = glm::length(point);
  if (!(length > 0) || !std::isfinite(length))
    fail(station, "undefined section frame direction");
  return point / length;
}

glm::dvec3 rotateBetween(glm::dvec3 point, glm::dvec3 from, glm::dvec3 to,
                         double station) {
  const double cosine = std::clamp(glm::dot(from, to), -1., 1.);
  if (cosine <= -1 + 1e-8)
    fail(station, "section frame tangent reverses");
  const auto axis = glm::cross(from, to);
  return point + glm::cross(axis, point) +
         glm::cross(axis, glm::cross(axis, point)) / (1 + cosine);
}

struct Side {
  double limit = 0, length = 0;
  IfcSectionMiterFrame frame;
  double displacement = 0;
  bool linear = false;
  double frameLimit = 0;
};

struct Join {
  double parameter;
  glm::dvec3 origin, miterX, miterY;
  std::array<Side, 2> sides;
};

struct Bounds {
  glm::dvec3 min{std::numeric_limits<double>::infinity()};
  glm::dvec3 max{-std::numeric_limits<double>::infinity()};

  void include(glm::dvec3 p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
  }

  bool overlaps(const Bounds &other, double tolerance) const {
    for (int axis = 0; axis < 3; ++axis)
      if (min[axis] - other.max[axis] > tolerance ||
          other.min[axis] - max[axis] > tolerance)
        return false;
    return true;
  }
};

double segmentDistance(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d) {
  const auto x = b - a, y = d - c, r = a - c;
  const double xx = glm::dot(x, x), yy = glm::dot(y, y);
  const double xy = glm::dot(x, y), xr = glm::dot(x, r), yr = glm::dot(y, r);
  const double denominator = xx * yy - xy * xy;
  double s = denominator > roundoff * xx * yy
                 ? std::clamp((xy * yr - yy * xr) / denominator, 0., 1.)
                 : 0.;
  const double t = yy > 0 ? std::clamp((xy * s + yr) / yy, 0., 1.) : 0.;
  if (xx > 0)
    s = std::clamp((xy * t - xr) / xx, 0., 1.);
  return glm::length(r + s * x - t * y);
}

class MiterSurface {
public:
  MiterSurface(ParametricCurve directrix, IfcSectionMiterFrameAt sectionFrame,
               IfcSectionMiterProfileAt profilePoint, double tolerance,
               double chordError, std::stop_token stop)
      : directrix_(std::move(directrix)),
        sectionFrame_(std::move(sectionFrame)),
        profilePoint_(std::move(profilePoint)), tolerance_(tolerance),
        chordError_(chordError), stop_(stop) {}

  void prepare(std::span<const double> stations,
               std::span<const IfcSectionMiterCorner> corners,
               std::span<const double> authoredStations,
               std::span<const double> profileBreaks, bool allowTransport) {
    container::util::checkModelImportCancellation(stop_);
    if (!directrix_.point || !sectionFrame_ || !profilePoint_ ||
        !(tolerance_ > 0) || !std::isfinite(tolerance_) || !(chordError_ > 0) ||
        !std::isfinite(chordError_) || stations.size() < 2 ||
        stations.size() > maxSamples || corners.empty() ||
        corners.size() > maxSamples || authoredStations.size() > maxSamples ||
        profileBreaks.size() > maxSamples || !validBreaks(stations) ||
        !validBreaks(authoredStations))
      throw std::runtime_error(
          "Invalid sectioned miter source or station budget");
    first_ = stations.front();
    last_ = stations.back();
    if (!std::isfinite(last_ - first_))
      throw std::runtime_error("Sectioned miter parameter range is nonfinite");

    profileBreaks_ = {0., 1.};
    for (double v : profileBreaks) {
      if (!std::isfinite(v) || v < 0 || v > 1)
        throw std::runtime_error("Invalid sectioned miter profile knot");
      profileBreaks_.push_back(v);
    }
    sortUnique(profileBreaks_);
    if (profileBreaks_.size() > maxSamples)
      throw std::runtime_error(
          "Sectioned miter exceeds its profile knot budget");
    if (corners.size() * (profileBreaks_.size() - 1) > maxCellProbes / 5)
      throw std::runtime_error(
          "Sectioned miter exceeds its join profile budget");
    if (!authoredStations.empty() &&
        (authoredStations.front() < first_ || authoredStations.back() > last_))
      throw std::runtime_error(
          "Authored section station falls outside the miter interval");
    breaks_.assign(stations.begin(), stations.end());
    authoredStations_.assign(authoredStations.begin(), authoredStations.end());
    limits_.assign(authoredStations.begin(), authoredStations.end());
    limits_.insert(limits_.end(), {first_, last_});
    double prior = first_;
    for (const auto &corner : corners) {
      if (!std::isfinite(corner.parameter) || corner.parameter <= prior ||
          corner.parameter >= last_ ||
          !std::ranges::binary_search(stations, corner.parameter))
        fail(corner.parameter, "invalid or unsorted sharp-corner station");
      prior = corner.parameter;
      limits_.push_back(prior);
    }
    sortUnique(limits_);
    for (const auto &corner : corners)
      addJoin(corner, stations, allowTransport);
    sortUnique(breaks_);
    if (breaks_.size() > maxSamples)
      throw std::runtime_error(
          "Sectioned miter exceeds its station break budget");
    validateSurface();
  }

  const std::vector<double> &breaks() const { return breaks_; }

  std::optional<glm::dvec3> point(double u, double v) const {
    if (!validParameters(u, v))
      return std::nullopt;
    container::util::checkModelImportCancellation(stop_);
    const auto next = std::ranges::lower_bound(joins_, u, {}, &Join::parameter);
    if (next != joins_.end() && next->parameter == u) {
      const auto local = profile(u, v);
      return next->origin + local.x * next->miterX + local.y * next->miterY;
    }
    const auto f = frame(u);
    auto result = basePoint(f, profile(u, v));
    if (next != joins_.end() && u > next->sides[0].limit)
      result += correction(*next, next->sides[0], f, u, v);
    if (next != joins_.begin() && u < (next - 1)->sides[1].limit)
      result += correction(*(next - 1), (next - 1)->sides[1], f, u, v);
    return result;
  }

  std::optional<IfcSectionMiterFrame> guideFrame(double u) const {
    if (!validParameters(u, 0.))
      return std::nullopt;
    container::util::checkModelImportCancellation(stop_);
    const auto next = std::ranges::lower_bound(joins_, u, {}, &Join::parameter);
    glm::dvec3 origin, lateral, up;
    if (next != joins_.end() && next->parameter == u) {
      origin = next->origin;
      lateral = next->miterX;
      up = next->miterY;
    } else {
      const auto f = frame(u);
      origin = f.origin;
      lateral = f.lateral;
      up = f.up;
      const auto transport = [&](const Join &join, const Side &side) {
        // A flat profile parallel to the bend axis may need no point
        // correction. Its guide planes still turn through the bisector.
        const double limit = side.length > 0 ? side.limit : side.frameLimit;
        if (u <= std::min(limit, join.parameter) ||
            u >= std::max(limit, join.parameter))
          return;
        const double weight = side.length > 0
                                  ? correctionWeight(join, side, u)
                                  : 1 - (u - join.parameter) /
                                            (limit - join.parameter);
        lateral += weight * transportedCorrection(join, side, f, {1, 0});
        up += weight * transportedCorrection(join, side, f, {0, 1});
      };
      if (next != joins_.end())
        transport(*next, next->sides[0]);
      if (next != joins_.begin())
        transport(*(next - 1), (next - 1)->sides[1]);
    }
    const auto normal = unit(glm::cross(lateral, up), u);
    lateral = unit(lateral, u);
    return IfcSectionMiterFrame{origin, lateral, glm::cross(normal, lateral),
                                normal};
  }

  std::optional<glm::dvec3> normal(double u, double v) const {
    if (!validParameters(u, v))
      return std::nullopt;
    if (std::ranges::binary_search(joins_, u, {}, &Join::parameter)) {
      // Curved-face vertices on a crease are duplicated by the mesher's
      // side handling. Supply one well-defined limit at the exact knot.
      return normalAt(u, v, true);
    }
    return normalAt(u, v, false);
  }

private:
  static void sortUnique(std::vector<double> &values) {
    std::ranges::sort(values);
    const auto duplicate = std::ranges::unique(values);
    values.erase(duplicate.begin(), duplicate.end());
  }

  bool validParameters(double u, double v) const {
    return std::isfinite(u) && std::isfinite(v) && u >= first_ && u <= last_ &&
           v >= 0 && v <= 1;
  }

  IfcSectionMiterFrame frame(double t) const {
    const auto f = sectionFrame_(t);
    if (!f || !finite(f->origin) || !finite(f->lateral) || !finite(f->up) ||
        !finite(f->normal))
      fail(t, "undefined section frame");
    return *f;
  }

  glm::dvec2 profile(double u, double v) const {
    const auto p = profilePoint_(u, v);
    if (!p || !std::isfinite(p->x) || !std::isfinite(p->y))
      fail(u, "undefined local profile point");
    return *p;
  }

  static glm::dvec3 basePoint(const IfcSectionMiterFrame &f, glm::dvec2 p) {
    return f.origin + profileOffset(f, p);
  }

  static glm::dvec3 profileOffset(const IfcSectionMiterFrame &f, glm::dvec2 p) {
    return p.x * f.lateral + p.y * f.up;
  }

  double length(double a, double b, double station) const {
    const auto result = curveLength(directrix_, std::min(a, b), std::max(a, b),
                                    tolerance_ * .1);
    if (!result || !std::isfinite(*result))
      fail(station, "cannot resolve arc length on join span");
    return *result;
  }

  double correctionLimit(double corner, double endpoint, double wanted,
                         double available) const {
    double near = corner;
    double far = endpoint;
    double parameter = std::lerp(corner, endpoint, wanted / available);
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
      container::util::checkModelImportCancellation(stop_);
      if (parameter == near || parameter == far)
        break;
      const double distance = length(corner, parameter, corner);
      if (std::abs(distance - wanted) <= tolerance_ * .1)
        return parameter;
      if (distance < wanted)
        near = parameter;
      else
        far = parameter;
      parameter = std::midpoint(near, far);
    }
    fail(corner, "cannot place a miter correction within its incident span");
  }

  glm::dvec3 project(glm::dvec3 offset, glm::dvec3 tangent,
                     glm::dvec3 bisector) const {
    return offset -
           tangent * (glm::dot(bisector, offset) / glm::dot(bisector, tangent));
  }

  IfcSectionMiterFrame limitFrame(double t, double probe, glm::dvec3 corner,
                                  glm::dvec3 tangent) const {
    auto f = frame(probe);
    const auto probeTangent = curveTangent(directrix_, probe);
    if (!probeTangent || glm::dot(*probeTangent, tangent) < 1 - 1e-5)
      fail(t, "one-sided section frame does not approach its incident tangent");
    const double gap = glm::length(f.origin - corner);
    const auto d = curveDerivative(directrix_, probe);
    if (!d || gap > tolerance_ + std::abs(t - probe) * glm::length(*d) * 1.2)
      fail(t, "incident directrix spans do not meet");
    if (glm::dot(unit(f.normal, t), *probeTangent) < 1 - 1e-6)
      fail(t, "sharp join requires tangent-normal incident section frames");
    f.origin = corner;
    f.up = rotateBetween(f.up, *probeTangent, tangent, t);
    f.up = unit(f.up - tangent * glm::dot(f.up, tangent), t);
    f.normal = tangent;
    f.lateral = glm::cross(f.up, tangent);
    return f;
  }

  void addJoin(const IfcSectionMiterCorner &corner,
               std::span<const double> stations, bool allowTransport) {
    container::util::checkModelImportCancellation(stop_);
    const double t = corner.parameter;
    const auto c = directrix_.point(t);
    if (!c || !finite(*c))
      fail(t, "undefined sharp join point");
    const auto in = unit(corner.incoming, t), out = unit(corner.outgoing, t);
    const auto bisector = unit(in + out, t);
    const double halfCosine = glm::dot(in, bisector);
    if (!(halfCosine >= .1) || glm::dot(out, bisector) < .1)
      fail(t, "extension exceeds the 10x miter limit");
    const auto station = std::ranges::lower_bound(stations, t);
    const double leftProbe =
        corner.incomingProbe.value_or(t - (t - *(station - 1)) * 1e-7);
    const double rightProbe =
        corner.outgoingProbe.value_or(t + (*(station + 1) - t) * 1e-7);
    if (!(leftProbe >= first_ && leftProbe < t && rightProbe > t &&
          rightProbe <= last_))
      fail(t, "invalid one-sided section frame probes");
    const auto incoming = limitFrame(t, leftProbe, *c, in);
    const auto outgoing = limitFrame(t, rightProbe, *c, out);
    Join join{t,
              *c,
              project(incoming.lateral, in, bisector),
              project(incoming.up, in, bisector),
              {Side{0, 0, incoming}, Side{0, 0, outgoing}}};
    const auto next = std::ranges::upper_bound(limits_, t);
    const auto previous = std::ranges::lower_bound(limits_, t) - 1;
    const auto miterPoint = [&](glm::dvec2 p) {
      return p.x * join.miterX + p.y * join.miterY;
    };
    const bool authoredCorner =
        !allowTransport && std::ranges::binary_search(authoredStations_, t);
    const auto authoredFrame =
        authoredCorner ? std::optional(frame(t)) : std::nullopt;
    std::array<double, 2> displacement{};
    double offsetScale = 1.;
    for (size_t i = 1; i < profileBreaks_.size(); ++i) {
      for (double part : {0., .25, .5, .75, 1.}) {
        const auto p = profile(
            t, std::lerp(profileBreaks_[i - 1], profileBreaks_[i], part));
        const auto m = miterPoint(p);
        const double scale = std::max(1., glm::length(p));
        offsetScale = std::max(offsetScale, scale);
        const double coincidence = std::max(tolerance_, scale * 1e-6);
        const auto b = profileOffset(outgoing, p);
        const auto plus = project(b, out, bisector);
        if (glm::length(m - plus) > coincidence) {
          const auto transportedUp = rotateBetween(incoming.up, in, out, t);
          if (!allowTransport ||
              glm::dot(transportedUp, outgoing.up) < std::sqrt(.5))
            fail(t, "incident sections have incompatible roll at their "
                    "half-angle row");
        }
        if (authoredFrame &&
            glm::length(basePoint(*authoredFrame, p) - (*c + m)) > coincidence)
          fail(t, "half-angle row conflicts with an authored section station");
        for (size_t side = 0; side < join.sides.size(); ++side)
          displacement[side] = std::max(
              displacement[side],
              glm::length(m - profileOffset(join.sides[side].frame, p)));
      }
    }
    for (size_t index = 0; index < join.sides.size(); ++index) {
      auto &side = join.sides[index];
      const double selection = index == 0 ? *previous : *next;
      side.frameLimit = selection;
      if (displacement[index] <= roundoff * offsetScale) {
        // The profile already meets the half-angle row on this side.
        side.limit = t;
        continue;
      }
      const double available = length(t, selection, t);
      side.displacement = displacement[index];
      const double sign = index == 0 ? -1. : 1.;
      side.linear =
          isLinearSide(t, selection, side.frame.normal, available, sign);
      if (side.linear) {
        if (available <= displacement[index] + tolerance_)
          fail(t, "incident span is too short for the miter extension");
        side.limit = selection;
        side.length = available;
        continue;
      }
      // Smoothstep has peak derivative 1.5. Four times the displacement
      // leaves at least 0.625 of nominal tangent progress on a straight
      // side. Curvature, twist and varying width are checked separately.
      const double wanted = std::max(4 * displacement[index],
                                     std::min(available * .125, chordError_));
      if (available <= tolerance_ || wanted >= available - tolerance_)
        fail(t, "incident span is too short for the miter extension");
      side.limit = correctionLimit(t, selection, wanted, available);
      side.length = length(t, side.limit, t);
      breaks_.push_back(side.limit);
    }
    if (!joins_.empty() && joins_.back().sides[1].limit > join.sides[0].limit) {
      const auto &prior = joins_.back().sides[1];
      const auto &current = join.sides[0];
      if (!prior.linear || !current.linear || prior.limit != t ||
          current.limit != joins_.back().parameter ||
          prior.displacement + current.displacement >=
              prior.length - tolerance_)
        fail(t, "adjacent miter corrections overlap or consume their span");
    }
    joins_.push_back(join);
  }

  bool isLinearSide(double origin, double endpoint, glm::dvec3 tangent,
                    double length, double sign) const {
    const auto start = directrix_.point(origin),
               end = directrix_.point(endpoint);
    if (!(start && end && std::isfinite(length)) || length <= tolerance_)
      return false;
    if (glm::length(*end - *start - sign * length * tangent) > tolerance_)
      return false;
    // Probe every authored directrix span, including its internal breaks.
    // Curved or skewed spans stay on the bounded smooth correction path.
    std::vector<double> knots{std::min(origin, endpoint),
                              std::max(origin, endpoint)};
    for (double knot : directrix_.breaks)
      if (knot > knots[0] && knot < knots[1])
        knots.push_back(knot);
    sortUnique(knots);
    for (size_t i = 1; i < knots.size(); ++i) {
      const auto a = directrix_.point(knots[i - 1]),
                 b = directrix_.point(knots[i]);
      if (!a || !b ||
          glm::length(*b - *a - glm::dot(*b - *a, tangent) * tangent) >
              tolerance_)
        return false;
      for (double w : {.25, .5, .75}) {
        const auto point =
            directrix_.point(std::lerp(knots[i - 1], knots[i], w));
        if (!point ||
            glm::length(*point - *a -
                        glm::dot(*point - *a, tangent) * tangent) > tolerance_)
          return false;
      }
    }
    return true;
  }

  glm::dvec3 correction(const Join &join, const Side &side,
                        const IfcSectionMiterFrame &f, double u,
                        double v) const {
    return correctionWeight(join, side, u) *
           transportedCorrection(join, side, f, profile(join.parameter, v));
  }

  static glm::dvec3 transportedCorrection(const Join &join, const Side &side,
                                          const IfcSectionMiterFrame &f,
                                          glm::dvec2 point) {
    const auto shared = point.x * join.miterX + point.y * join.miterY;
    const auto difference = shared - profileOffset(side.frame, point);
    // Transport the same correction in the side's actual section frame.
    return glm::dot(difference, side.frame.lateral) * f.lateral +
           glm::dot(difference, side.frame.up) * f.up +
           glm::dot(difference, side.frame.normal) * f.normal;
  }

  double correctionWeight(const Join &join, const Side &side, double u) const {
    const double fraction = std::clamp(
        length(u, join.parameter, join.parameter) / side.length, 0., 1.);
    return side.linear
               ? 1 - fraction
               : (1 - fraction) * (1 - fraction) * (1 + 2 * fraction);
  }

  std::optional<glm::dvec3> normalAt(double u, double v, bool atJoin) const {
    auto hi = std::ranges::upper_bound(breaks_, u);
    if (hi == breaks_.end())
      --hi;
    const auto lo = hi - 1;
    const double du = (*hi - *lo) * 1e-4;
    const auto a = std::max(*lo, u - du), b = std::min(*hi, u + du);
    auto colHi = std::ranges::upper_bound(profileBreaks_, v);
    if (colHi == profileBreaks_.end())
      --colHi;
    const auto colLo = colHi - 1;
    const double dv = (*colHi - *colLo) * 1e-4;
    const double left = std::max(*colLo, v - dv),
                 right = std::min(*colHi, v + dv);
    const auto p = point(a, v), q = point(b, v), l = point(u, left),
               r = point(u, right);
    if (!p || !q || !l || !r || a == b || left == right)
      return std::nullopt;
    const auto tangent = *q - *p, across = *r - *l;
    if (glm::length(across) == 0)
      return std::nullopt;
    const auto n = glm::cross(tangent, across);
    const auto magnitude = glm::length(n);
    if (!(magnitude > glm::length(tangent) * glm::length(across) * 1e-10))
      fail(u, "section profile folds or becomes singular near the miter");
    if (!atJoin) {
      const auto f = frame(u);
      if (glm::dot(tangent, f.normal) <= glm::length(tangent) * 1e-9)
        fail(u, "longitudinal profile strand reverses near the miter");
      const auto width = profile(u, right) - profile(u, left);
      const auto referenceNormal =
          glm::cross(f.normal, width.x * f.lateral + width.y * f.up);
      const double referenceMagnitude = glm::length(referenceNormal);
      if (referenceMagnitude > 0 &&
          glm::dot(n, referenceNormal) <= magnitude * referenceMagnitude * 1e-9)
        fail(u, "transverse profile order reverses near the miter");
    }
    return n / magnitude;
  }

  void validateSurface() const {
    if ((breaks_.size() - 1) * (profileBreaks_.size() - 1) > maxCellProbes / 9)
      throw std::runtime_error(
          "Sectioned miter exceeds its validation grid budget");
    // Check interior samples on either side of every authored profile/station
    // break. Degenerate zero-width panels are existing sectioned topology;
    // the mesher handles them while adjacent nondegenerate panels still pass.
    for (size_t u = 1; u < breaks_.size(); ++u) {
      container::util::checkModelImportCancellation(stop_);
      for (size_t v = 1; v < profileBreaks_.size(); ++v)
        for (double a : {.125, .5, .875})
          for (double b : {0., .5, 1.}) {
            const double t = std::lerp(breaks_[u - 1], breaks_[u], a);
            const double p =
                std::lerp(profileBreaks_[v - 1], profileBreaks_[v], b);
            static_cast<void>(normalAt(t, p, false));
          }
    }
    validateClearance();
  }

  void validateClearance() const {
    const auto samples =
        sampleCurve(directrix_, first_, last_, chordError_ * .25, maxSamples);
    if (samples.size() < 2)
      throw std::runtime_error(
          "Sectioned miter directrix exceeds its clearance sampling budget");
    std::vector<Bounds> bounds;
    bounds.reserve(samples.size() - 1);
    for (size_t i = 1; i < samples.size(); ++i) {
      auto &bound = bounds.emplace_back();
      bound.include(samples[i - 1].point);
      bound.include(samples[i].point);
    }
    size_t checks = 0;
    for (size_t i = 0; i < bounds.size(); ++i) {
      container::util::checkModelImportCancellation(stop_);
      for (size_t j = i + 2; j < bounds.size(); ++j) {
        if (++checks > maxChecks)
          throw std::runtime_error(
              "Sectioned miter exceeds its clearance comparison budget");
        if (bounds[i].overlaps(bounds[j], tolerance_) &&
            segmentDistance(samples[i].point, samples[i + 1].point,
                            samples[j].point,
                            samples[j + 1].point) <= tolerance_)
          fail(samples[i].parameter, "directrix intersects another span");
      }
    }
  }

  ParametricCurve directrix_;
  IfcSectionMiterFrameAt sectionFrame_;
  IfcSectionMiterProfileAt profilePoint_;
  double tolerance_, chordError_;
  std::stop_token stop_;
  double first_ = 0, last_ = 0;
  std::vector<double> authoredStations_, limits_, profileBreaks_, breaks_;
  std::vector<Join> joins_;
};

} // namespace

IfcSectionMiterResult buildIfcSectionedMiter(
    const ParametricCurve &directrix, std::span<const double> stationBreaks,
    std::span<const IfcSectionMiterCorner> corners,
    IfcSectionMiterFrameAt sectionFrame, IfcSectionMiterProfileAt profilePoint,
    std::span<const double> authoredStations,
    std::span<const double> profileBreaks, double tolerance, double chordError,
    std::stop_token stop, bool allowDefaultFrameTransport) {
  auto surface = std::make_shared<MiterSurface>(
      directrix, std::move(sectionFrame), std::move(profilePoint), tolerance,
      chordError, stop);
  surface->prepare(stationBreaks, corners, authoredStations, profileBreaks,
                   allowDefaultFrameTransport);
  return {[surface](double u, double v) { return surface->point(u, v); },
          surface->breaks(),
          [surface](double u, double v) { return surface->normal(u, v); },
          [surface](double u) { return surface->guideFrame(u); }};
}

} // namespace container::geometry::ifc::detail
