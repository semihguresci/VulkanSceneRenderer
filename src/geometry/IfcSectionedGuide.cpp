#include "IfcSectionedGuide.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
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
constexpr size_t maxProjections = 8'000'000;
constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();

bool finite(glm::dvec3 point) {
  return std::isfinite(point.x) && std::isfinite(point.y) &&
         std::isfinite(point.z);
}

bool oppositeSigns(double a, double b) {
  return (a < 0 && b > 0) || (a > 0 && b < 0);
}

// Both residual endpoints are known to bracket the root. Use secant in the
// inner interval and bisect otherwise to ensure the bracket always shrinks.
template <class Evaluate>
double bracketedRoot(Evaluate evaluate, double first, double last, double low,
                     double high, double tolerance) {
  for (unsigned iteration = 0; iteration < 96; ++iteration) {
    if (std::abs(low) <= tolerance)
      return first;
    if (std::abs(high) <= tolerance)
      return last;
    const double fraction = low / (low - high);
    const double candidate = fraction > .125 && fraction < .875
                                 ? std::lerp(first, last, fraction)
                                 : std::midpoint(first, last);
    if (candidate == first || candidate == last)
      break;
    const double residual = evaluate(candidate);
    if (std::abs(residual) <= tolerance)
      return candidate;
    if (oppositeSigns(low, residual)) {
      last = candidate;
      high = residual;
    } else {
      first = candidate;
      low = residual;
    }
  }
  return std::abs(low) <= std::abs(high) ? first : last;
}

class GuideIntersection {
public:
  GuideIntersection(uint32_t id, ParametricCurve guide,
                    IfcSectionPlaneAt sectionPlane, double tolerance,
                    std::stop_token stop)
      : id_(id), guide_(std::move(guide)),
        sectionPlane_(std::move(sectionPlane)), tolerance_(tolerance),
        stop_(stop) {}

  [[noreturn]] void fail(const char *reason) const {
    throw std::runtime_error("Sectioned guide #" + std::to_string(id_) + ": " +
                             reason);
  }

  void validate(std::span<const double> stations) const {
    if (!guide_.point || guide_.dimension != 3 || !sectionPlane_ ||
        !std::isfinite(tolerance_) || tolerance_ <= 0 ||
        !std::isfinite(guide_.period) || guide_.period < 0)
      fail("requires a finite tolerance, 3D guide curve, and section planes");
    if (stations.size() < 2 || stations.size() > maxSamples ||
        !std::ranges::all_of(stations,
                             [](double t) { return std::isfinite(t); }) ||
        !std::isfinite(stations.back() - stations.front()) ||
        std::adjacent_find(stations.begin(), stations.end(),
                           std::greater_equal<>()) != stations.end())
      fail("requires a finite, strictly increasing section interval within the "
           "station budget");
    if (guide_.breaks.size() > maxSamples ||
        !std::ranges::all_of(guide_.breaks,
                             [](double t) { return std::isfinite(t); }))
      fail("has nonfinite guide knots or exceeds the guide knot budget");
    if (guide_.domain &&
        (!std::isfinite(guide_.domain->first) ||
         !std::isfinite(guide_.domain->second) ||
         !(guide_.domain->first < guide_.domain->second) ||
         !std::isfinite(guide_.domain->second - guide_.domain->first)))
      fail("has an invalid native parameter domain");
  }

  glm::dvec3 sharedPoint(double station) const {
    container::util::checkModelImportCancellation(stop_);
    if (guide_.domain && guide_.period == 0 &&
        (station < guide_.domain->first || station > guide_.domain->second))
      fail("does not cover the authored section interval");
    const auto result = evaluate(station);
    if (std::abs(project(result, plane(station))) > tolerance_)
      fail("shared-basis point leaves the section plane");
    return result;
  }

  std::vector<double> sharedBreaks(double first, double last) const {
    std::vector<double> breaks;
    for (double knot : guide_.breaks)
      if (knot > first && knot < last)
        breaks.push_back(knot);
    std::ranges::sort(breaks);
    const auto duplicate = std::ranges::unique(breaks);
    breaks.erase(duplicate.begin(), duplicate.end());
    return breaks;
  }

  void prepareIndependent(std::span<const double> stations) {
    if (!guide_.domain || guide_.period > 0)
      fail("independent basis requires a bounded nonperiodic guide");
    sampleGuide();
    const auto witnessStations = stationWitnesses(stations);
    if (witnessStations.size() > maxProjections / samples_.size())
      fail("exceeds the independent guide projection budget");
    witnesses_.reserve(witnessStations.size());
    for (double station : witnessStations) {
      const double guideParameter = intersection(station);
      if (witnesses_.size() >= 2) {
        const auto previous = witnesses_.back().parameter;
        const bool increasing = previous > witnesses_.front().parameter;
        if (increasing ? guideParameter <= previous
                       : guideParameter >= previous)
          fail("independent guide reverses its section order or is "
               "disconnected");
      } else if (!witnesses_.empty() &&
                 guideParameter == witnesses_.front().parameter) {
        fail("successive section planes resolve to the same guide point");
      }
      witnesses_.push_back({station, guideParameter});
    }
  }

  glm::dvec3 independentPoint(double station) const {
    container::util::checkModelImportCancellation(stop_);
    const auto knot =
        std::ranges::lower_bound(mappedKnots_, station, {}, &Witness::station);
    if (knot != mappedKnots_.end() && knot->station == station)
      return evaluate(knot->parameter);
    const auto upper = std::lower_bound(
        witnesses_.begin(), witnesses_.end(), station,
        [](const Witness &value, double t) { return value.station < t; });
    if (upper != witnesses_.end() && upper->station == station)
      return evaluate(upper->parameter);
    const auto before = (upper - 1)->parameter;
    const auto after = upper->parameter;
    const auto section = plane(station);
    const double parameter = certifiedProjection(section)
                                 ? localIntersection(section, before, after)
                                 : intersection(section);
    const double epsilon =
        roundoff * std::abs(guide_.domain->second - guide_.domain->first);
    if (parameter < std::min(before, after) - epsilon ||
        parameter > std::max(before, after) + epsilon)
      fail("independent guide folds between section stations");
    return evaluate(parameter);
  }

  std::vector<double> independentBreaks() {
    std::vector<double> breaks;
    const double first = witnesses_.front().parameter;
    const double last = witnesses_.back().parameter;
    const bool increasing = last > first;
    for (double knot : guide_.breaks) {
      container::util::checkModelImportCancellation(stop_);
      if (!(knot > std::min(first, last) && knot < std::max(first, last)))
        continue;
      const auto upper = std::lower_bound(
          witnesses_.begin(), witnesses_.end(), knot,
          [increasing](const Witness &value, double t) {
            return increasing ? value.parameter < t : value.parameter > t;
          });
      if (upper->parameter == knot) {
        mappedKnots_.push_back({upper->station, knot});
        continue;
      }
      const double low = (upper - 1)->station, high = upper->station;
      const auto guidePoint = evaluate(knot);
      const auto residual = [&](double t) {
        return project(guidePoint, plane(t));
      };
      const double left = residual(low), right = residual(high);
      // A numerical residual close to zero can be at an authored station,
      // even if its solved guide parameter did not exactly equal the knot.
      const double epsilon = tolerance_ * .001;
      if (!oppositeSigns(left, right) && std::abs(left) > epsilon &&
          std::abs(right) > epsilon)
        fail("cannot map an independent guide knot to its section station");
      const double station =
          bracketedRoot(residual, low, high, left, right, epsilon);
      if (std::abs(residual(station)) > tolerance_)
        fail("guide knot exceeds section-plane precision");
      mappedKnots_.push_back({station, knot});
    }
    std::ranges::sort(mappedKnots_, {}, &Witness::station);
    auto duplicate = std::ranges::unique(
        mappedKnots_, [](const Witness &a, const Witness &b) {
          return a.parameter == b.parameter && a.station == b.station;
        });
    mappedKnots_.erase(duplicate.begin(), duplicate.end());
    for (const auto &knot : mappedKnots_) {
      if (!breaks.empty() && knot.station <= breaks.back())
        fail("distinct guide knots map to the same section station");
      breaks.push_back(knot.station);
    }
    return breaks;
  }

  static std::vector<double>
  stationWitnesses(std::span<const double> stations) {
    const size_t divisions =
        std::min(size_t(4), (maxSamples - 1) / (stations.size() - 1));
    std::vector<double> witnesses{stations.front()};
    for (size_t i = 1; i < stations.size(); ++i) {
      for (size_t j = 1; j < divisions; ++j) {
        const double t =
            std::lerp(stations[i - 1], stations[i], double(j) / divisions);
        if (t > witnesses.back() && t < stations[i])
          witnesses.push_back(t);
      }
      witnesses.push_back(stations[i]);
    }
    return witnesses;
  }

private:
  struct Witness {
    double station, parameter;
  };

  glm::dvec3 evaluate(double parameter) const {
    const auto value = guide_.point(parameter);
    if (!value || !finite(*value))
      fail("is disconnected or has an undefined point in its native domain");
    return *value;
  }

  IfcSectionPlane plane(double station) const {
    const auto p = sectionPlane_(station);
    if (!p || !finite(p->origin) || !finite(p->normal))
      fail("cannot resolve the section plane at a directrix station");
    const auto length = glm::length(p->normal);
    if (!(length > 0) || !std::isfinite(length))
      fail("has a degenerate section-plane normal");
    return {p->origin, p->normal / length};
  }

  double project(glm::dvec3 point, const IfcSectionPlane &p) const {
    const double distance = glm::dot(point - p.origin, p.normal);
    if (!std::isfinite(distance))
      fail("has a nonfinite section-plane residual");
    return distance;
  }

  void sampleGuide() {
    const auto [first, last] = *guide_.domain;
    glm::dvec3 minimum = evaluate(first), maximum = minimum;
    for (size_t i = 1; i <= 16; ++i) {
      const auto point = evaluate(std::lerp(first, last, double(i) / 16));
      minimum = glm::min(minimum, point);
      maximum = glm::max(maximum, point);
    }
    const double extent = glm::length(maximum - minimum);
    if (!(extent > 0) || !std::isfinite(extent))
      fail("independent guide has no finite geometric extent");
    const double chordError = std::max(tolerance_ * .25, extent * 1e-6);
    samples_ = sampleCurve(guide_, first, last, chordError, maxSamples);
    if (samples_.size() < 2 || samples_.front().parameter != first ||
        samples_.back().parameter != last)
      fail("independent guide exceeds its chord sampling budget or is "
           "disconnected");
    if (std::ranges::any_of(samples_, [](const CurveSample &sample) {
          return !std::isfinite(sample.parameter) || !finite(sample.point);
        }))
      fail("independent guide has a nonfinite sample");
    for (double knot : guide_.breaks)
      if (knot > first && knot < last)
        samples_.push_back({knot, evaluate(knot)});
    std::ranges::sort(samples_, {}, &CurveSample::parameter);
    const auto duplicate =
        std::ranges::unique(samples_, {}, &CurveSample::parameter);
    samples_.erase(duplicate.begin(), duplicate.end());
    if (samples_.size() > maxSamples)
      fail("independent guide exceeds its sampling and knot budget");
    const auto chord = samples_.back().point - samples_.front().point;
    const double chordLength = glm::length(chord);
    if (!(chordLength > 0) || !std::isfinite(chordLength))
      fail("independent guide has coincident end points");
    guideAxis_ = chord / chordLength;
    minAlignment_ = 1;
    for (size_t i = 1; i < samples_.size(); ++i) {
      const auto edge = samples_[i].point - samples_[i - 1].point;
      const double length = glm::length(edge);
      if (!(length > 0) || !std::isfinite(length))
        fail("independent guide has a collapsed sample span");
      minAlignment_ =
          std::min(minAlignment_, glm::dot(edge / length, guideAxis_));
      maxCoordinate_ = std::max(maxCoordinate_, glm::length(samples_[i].point));
      minSpan_ = std::min(minSpan_, length);
    }
    maxCoordinate_ =
        std::max(maxCoordinate_, glm::length(samples_.front().point));
  }

  double intersection(double station) const {
    return intersection(plane(station));
  }

  // Every sampled chord lies in the cone about guideAxis_ with cosine at
  // least minAlignment_. The worst perpendicular component opposes the
  // queried normal. A positive lower bound proves all of their projections
  // strictly monotone, so no sampled branch can lie outside the local bracket.
  bool certifiedProjection(const IfcSectionPlane &section) const {
    if (minAlignment_ <= 0)
      return false;
    const double alignment =
        std::min(1., std::abs(glm::dot(section.normal, guideAxis_)));
    const double lowerBound = minAlignment_ * alignment -
                              std::sqrt(1. - minAlignment_ * minAlignment_) *
                                  std::sqrt(1. - alignment * alignment);
    return lowerBound > 1e-9 &&
           lowerBound * minSpan_ >
               roundoff * (maxCoordinate_ + glm::length(section.origin));
  }

  double localIntersection(const IfcSectionPlane &section, double before,
                           double after) const {
    const auto [first, last] = std::minmax(before, after);
    const auto residual = [&](double t) {
      return project(evaluate(t), section);
    };
    const double a = residual(first), b = residual(last);
    const double epsilon = tolerance_ * .001;
    if (!oppositeSigns(a, b) && std::abs(a) > tolerance_ &&
        std::abs(b) > tolerance_)
      fail("independent guide does not cross between its section witnesses");
    // Restrict the root to one sampled span to retain the same exact branch
    // checked by the complete guide projection scan.
    const double sense = glm::dot(section.normal, guideAxis_) > 0 ? 1. : -1.;
    const auto upper = std::ranges::lower_bound(
        samples_, 0., {}, [&](const CurveSample &sample) {
          return sense * project(sample.point, section);
        });
    const double clampedFirst = upper == samples_.begin()
                                    ? first
                                    : std::max(first, (upper - 1)->parameter);
    const double clampedLast =
        upper == samples_.end() ? last : std::min(last, upper->parameter);
    if (clampedFirst > clampedLast)
      fail("independent guide folds between section stations");
    const double left = clampedFirst == first ? a : residual(clampedFirst);
    const double right = clampedLast == last ? b : residual(clampedLast);
    const double parameter =
        oppositeSigns(left, right)
            ? bracketedRoot(residual, clampedFirst, clampedLast, left, right,
                            epsilon)
        : std::abs(left) <= std::abs(right) ? clampedFirst
                                            : clampedLast;
    if (std::abs(residual(parameter)) > tolerance_)
      fail("independent guide intersection exceeds section-plane precision");
    const auto end = upper == samples_.begin() ? upper + 1
                     : upper == samples_.end() ? upper - 1
                                               : upper;
    const double step = (end->parameter - (end - 1)->parameter) * 1e-4;
    verifyTransverse(parameter, step, section.normal, sense);
    return parameter;
  }

  double intersection(const IfcSectionPlane &section) const {
    const auto residual = [&](double t) {
      return project(evaluate(t), section);
    };
    const double first = project(samples_.front().point, section);
    const double last = project(samples_.back().point, section);
    if (std::abs(last - first) <= tolerance_)
      fail("independent guide has no transverse section-plane direction");
    const double sense = last > first ? 1. : -1.;
    if (sense * first > tolerance_ || sense * last < -tolerance_)
      fail("independent guide does not reach a section plane");
    size_t bracket = 0;
    double low = first, high = first, previous = first;
    for (size_t i = 1; i < samples_.size(); ++i) {
      if ((i & 255u) == 0)
        container::util::checkModelImportCancellation(stop_);
      const auto &left = samples_[i - 1], &right = samples_[i];
      const double current = project(right.point, section);
      const double projectedStep =
          glm::dot(right.point - left.point, section.normal);
      const double length = glm::length(right.point - left.point);
      if (sense * projectedStep < -length * 1e-10)
        fail("independent guide folds or has multiple section-plane "
             "intersections");
      if (!std::isfinite(length) || length == 0 ||
          sense * projectedStep <= length * 1e-10)
        fail("independent guide has a collapsed or section-tangent span");
      if (sense * previous <= 0 && sense * current >= 0) {
        if (bracket == 0) {
          bracket = i;
          low = previous;
          high = current;
        }
      }
      previous = current;
    }
    if (bracket == 0) {
      for (const size_t endpoint : {size_t(0), samples_.size() - 1}) {
        const double endpointResidual = endpoint == 0 ? first : last;
        if (std::abs(endpointResidual) > tolerance_)
          continue;
        const size_t neighbor = endpoint == 0 ? 1 : endpoint - 1;
        const double parameter = samples_[endpoint].parameter;
        verifyTransverse(parameter,
                         std::abs(samples_[neighbor].parameter - parameter) *
                             1e-4,
                         section.normal, sense);
        return parameter;
      }
      fail("no unique independent guide intersection with a section plane");
    }
    const double left = samples_[bracket - 1].parameter;
    const double right = samples_[bracket].parameter;
    const double parameter =
        bracketedRoot(residual, left, right, low, high, tolerance_ * .001);
    if (std::abs(residual(parameter)) > tolerance_)
      fail("independent guide intersection exceeds section-plane precision");
    verifyTransverse(parameter, (right - left) * 1e-4, section.normal, sense);
    return parameter;
  }

  void verifyTransverse(double parameter, double step, glm::dvec3 normal,
                        double sense) const {
    const auto [first, last] = *guide_.domain;
    const auto before = evaluate(std::max(first, parameter - step));
    const auto after = evaluate(std::min(last, parameter + step));
    const auto direction = after - before;
    if (!(sense * glm::dot(direction, normal) > glm::length(direction) * 1e-8))
      fail("independent guide intersection is tangent or folded");
  }

  uint32_t id_;
  ParametricCurve guide_;
  IfcSectionPlaneAt sectionPlane_;
  double tolerance_;
  std::stop_token stop_;
  std::vector<CurveSample> samples_;
  std::vector<Witness> witnesses_;
  std::vector<Witness> mappedKnots_;
  glm::dvec3 guideAxis_{0};
  double minAlignment_ = 0;
  double minSpan_ = std::numeric_limits<double>::infinity();
  double maxCoordinate_ = 0;
};

} // namespace

IfcSectionedGuide resolveIfcSectionedGuide(
    uint32_t sourceId, ParametricCurve guide, bool sharedBasis,
    std::span<const double> stationBreaks, IfcSectionPlaneAt sectionPlane,
    double pointTolerance, std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  auto resolver = std::make_shared<GuideIntersection>(
      sourceId, std::move(guide), std::move(sectionPlane), pointTolerance,
      stop);
  resolver->validate(stationBreaks);
  const double first = stationBreaks.front(), last = stationBreaks.back();
  IfcSectionedGuide result{sourceId, {}, {}};
  if (sharedBasis) {
    result.breaks = resolver->sharedBreaks(first, last);
    for (double station : GuideIntersection::stationWitnesses(stationBreaks))
      static_cast<void>(resolver->sharedPoint(station));
    for (double station : result.breaks)
      static_cast<void>(resolver->sharedPoint(station));
    result.point = [resolver, first,
                    last](double station) -> std::optional<glm::dvec3> {
      if (!std::isfinite(station) || station < first || station > last)
        return std::nullopt;
      return resolver->sharedPoint(station);
    };
  } else {
    resolver->prepareIndependent(stationBreaks);
    result.breaks = resolver->independentBreaks();
    result.point = [resolver, first,
                    last](double station) -> std::optional<glm::dvec3> {
      if (!std::isfinite(station) || station < first || station > last)
        return std::nullopt;
      return resolver->independentPoint(station);
    };
  }
  return result;
}

} // namespace container::geometry::ifc::detail
