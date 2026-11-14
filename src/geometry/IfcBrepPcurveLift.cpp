#include "IfcBrepPcurveLift.h"

#include "Container/utility/ModelImportCancellation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <glm/geometric.hpp>

namespace container::geometry::ifc::detail {
namespace {

using Point = glm::dvec3;
using UV = glm::dvec2;

constexpr size_t maxEdgeSamples = 4097;
constexpr size_t maxWitnessSamples = 8193;
constexpr size_t maxComparisons = 8'000'000;
constexpr size_t maxEvaluations = 1'000'000;

bool finite(Point p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

std::pair<double, double> distanceToChord(Point point, Point a, Point b) {
  const auto delta = b - a;
  const double squaredLength = glm::dot(delta, delta);
  const double offset =
      squaredLength > 0
          ? std::clamp(glm::dot(point - a, delta) / squaredLength, 0., 1.)
          : 0.;
  return {glm::length(point - (a + offset * delta)), offset};
}

struct LiftHit {
  double parameter;
  size_t segment;
};

class EdgeLift {
public:
  EdgeLift(const ParametricSurface &surface, const ParametricCurve &nativeUv,
           std::span<const glm::vec3> edge, bool sameSense, double units,
           std::stop_token stop)
      : surface_(surface), nativeUv_(nativeUv), edge_(edge), stop_(stop),
        chord_(.0005 / units), witnessChord_(chord_ * .25),
        tolerance_(1e-7 / units),
        first_(sameSense ? nativeUv.domain->first : nativeUv.domain->second),
        last_(sameSense ? nativeUv.domain->second : nativeUv.domain->first) {
    const Point origin(edge.front());
    double extent = 0;
    for (const auto &p : edge) {
      if (!finite(Point(p)))
        throw std::runtime_error("Authored pcurve has a nonfinite shared edge");
      extent = std::max(extent, glm::length(Point(p) - origin));
    }
    if (!(extent > 0) || !std::isfinite(extent))
      throw std::runtime_error("Authored pcurve has a collapsed shared edge");
    tolerance_ = std::max(tolerance_, extent * 2e-7);
  }

  std::vector<UV> run() {
    prepareWitness();
    auto first = LiftHit{0., 1};
    auto last = LiftHit{1., samples_.size() - 1};
    const auto a = Point(edge_.front()), b = Point(edge_.back());
    const bool closed = edge_.front() == edge_.back();
    if (glm::length(samples_.front().point - a) > tolerance_) {
      if (closed)
        throw std::runtime_error(
            "Authored pcurve must start at the closed shared edge vertex");
      first = trimEndpoint(a);
    }
    if (glm::length(samples_.back().point - b) > tolerance_) {
      if (closed)
        throw std::runtime_error(
            "Authored pcurve must end at the closed shared edge vertex");
      last = trimEndpoint(b);
    }
    if (!(last.parameter > first.parameter))
      throw std::runtime_error(
          "Authored pcurve endpoint order conflicts with edge SameSense");

    std::vector<UV> result;
    result.reserve(edge_.size());
    result.push_back(lift(first.parameter, a));
    auto previous = first;
    for (size_t i = 1; i < edge_.size(); ++i) {
      container::util::checkModelImportCancellation(stop_);
      const Point current(edge_[i]), prior(edge_[i - 1]);
      auto hit = last;
      if (i + 1 != edge_.size()) {
        const auto candidate = findNext(current, previous, last);
        if (!candidate)
          throw std::runtime_error(
              "Authored pcurve does not follow shared edge samples in order");
        hit = *candidate;
      }
      if (!(hit.parameter > previous.parameter))
        throw std::runtime_error(
            "Authored pcurve reverses or repeats a shared edge sample");
      verifyCoverage(prior, current, previous, hit);
      result.push_back(lift(hit.parameter, current));
      previous = hit;
    }
    return result;
  }

private:
  double native(double t) const { return std::lerp(first_, last_, t); }

  std::optional<UV> parameterPoint(double t) const {
    const auto p = nativeUv_.point(native(t));
    return p && finite(*p) ? std::optional(UV(*p)) : std::nullopt;
  }

  std::optional<Point> evaluate(UV uv) {
    if (++evaluations_ > maxEvaluations)
      throw std::runtime_error("Authored pcurve exceeds its evaluation budget");
    if (evaluations_ % 256 == 0)
      container::util::checkModelImportCancellation(stop_);
    const auto p = surface_.point(uv.x, uv.y);
    return p && finite(*p) ? p : std::nullopt;
  }

  std::optional<Point> evaluate(double parameter) {
    const auto uv = parameterPoint(parameter);
    return uv ? evaluate(*uv) : std::nullopt;
  }

  Point requirePoint(double parameter) {
    const auto p = evaluate(parameter);
    if (!p)
      throw std::runtime_error(
          "Authored pcurve leaves the native surface domain");
    return *p;
  }

  void checkComparison() {
    if (++comparisons_ > maxComparisons)
      throw std::runtime_error("Authored pcurve exceeds its search budget");
  }

  void prepareWitness() {
    ParametricCurve mapped;
    mapped.domain = {{0., 1.}};
    mapped.point = [this](double t) { return evaluate(t); };
    mapped.breaks.reserve(nativeUv_.breaks.size());
    for (double t : nativeUv_.breaks)
      mapped.breaks.push_back((t - first_) / (last_ - first_));
    mapped.stepValidator = [this](double a, double b) {
      if (nativeUv_.stepValidator &&
          !nativeUv_.stepValidator(native(a), native(b)))
        return false;
      auto previous = parameterPoint(a);
      if (!previous)
        return false;
      for (double fraction : {.25, .5, .75, 1.}) {
        const auto uv = parameterPoint(std::lerp(a, b, fraction));
        if (!uv ||
            (surface_.regionStep && !surface_.regionStep(*previous, *uv)))
          return false;
        for (unsigned axis = 0; axis < 2; ++axis)
          if (surface_.periods[axis] &&
              std::abs((*uv)[axis] - (*previous)[axis]) >
                  surface_.periods[axis] / 16)
            return false;
        previous = uv;
      }
      return true;
    };
    samples_ = sampleCurve(mapped, 0., 1., witnessChord_, maxWitnessSamples);
    if (samples_.size() < 2)
      throw std::runtime_error("Authored pcurve leaves its surface or exceeds "
                               "its chord sampling budget");
  }

  std::optional<double> resolve(Point target, size_t segment, double lower,
                                double upper) {
    checkComparison();
    const auto &sampleA = samples_[segment - 1], &sampleB = samples_[segment];
    const auto a =
        lower == sampleA.parameter ? sampleA.point : requirePoint(lower);
    const auto b =
        upper == sampleB.parameter ? sampleB.point : requirePoint(upper);
    const auto [distance, fraction] = distanceToChord(target, a, b);
    if (distance > witnessChord_ + tolerance_)
      return std::nullopt;
    auto best = lower;
    double residual = glm::length(a - target);
    const auto compare = [&](double t, double error) {
      if (error < residual) {
        best = t;
        residual = error;
      }
    };
    compare(upper, glm::length(b - target));
    const auto measure = [&](double t) {
      const double error = glm::length(requirePoint(t) - target);
      compare(t, error);
      return error;
    };
    if (residual == 0.)
      return best;
    measure(std::lerp(lower, upper, fraction));

    // A dense chord bracket limits the search to one local branch. Golden
    // section minimization keeps that bracket even with mismatched parameters
    // and avoids relying on invertibility of the whole spline surface.
    constexpr double golden = .6180339887498948482;
    double left = std::lerp(upper, lower, golden);
    double right = std::lerp(lower, upper, golden);
    double leftError = measure(left), rightError = measure(right);
    for (unsigned i = 0; i < 48 && residual > tolerance_ * .001; ++i) {
      if (leftError <= rightError) {
        upper = right;
        right = left;
        rightError = leftError;
        left = std::lerp(upper, lower, golden);
        leftError = measure(left);
      } else {
        lower = left;
        left = right;
        leftError = rightError;
        right = std::lerp(lower, upper, golden);
        rightError = measure(right);
      }
    }
    return residual <= tolerance_ ? std::optional(best) : std::nullopt;
  }

  LiftHit trimEndpoint(Point target) {
    std::optional<LiftHit> found;
    for (size_t i = 1; i < samples_.size(); ++i) {
      const auto &a = samples_[i - 1], &b = samples_[i];
      const auto root = resolve(target, i, a.parameter, b.parameter);
      if (!root)
        continue;
      if (!found) {
        found = LiftHit{*root, i};
        continue;
      }
      if (i != found->segment + 1 ||
          (found->parameter != a.parameter && *root != a.parameter))
        throw std::runtime_error(
            "Authored pcurve has an ambiguous shared edge endpoint");
      // The same root can land at the boundary of one search bracket and
      // just inside the next after rounding the shared point to binary32.
      if (glm::length(requirePoint(*root) - target) <
          glm::length(requirePoint(found->parameter) - target))
        found = LiftHit{*root, i};
    }
    if (!found)
      throw std::runtime_error(
          "Authored pcurve endpoint does not match its shared edge");
    return *found;
  }

  std::optional<LiftHit> findNext(Point target, LiftHit previous,
                                  LiftHit last) {
    for (size_t i = previous.segment; i <= last.segment; ++i) {
      const double a = std::max(previous.parameter, samples_[i - 1].parameter);
      const double b = std::min(last.parameter, samples_[i].parameter);
      if (a >= b)
        continue;
      const auto root = resolve(target, i, a, b);
      if (root && *root > previous.parameter && *root < last.parameter)
        return LiftHit{*root, i};
    }
    return std::nullopt;
  }

  void verifyCoverage(Point a, Point b, LiftHit first, LiftHit last) {
    const double chordLength = glm::length(b - a);
    if (!(chordLength > 0))
      throw std::runtime_error(
          "Authored pcurve has a repeated shared edge sample");
    const double tolerance = chord_ + witnessChord_ + tolerance_;
    double progress = 0;
    for (size_t i = first.segment; i < last.segment + 1; ++i) {
      const auto &sample = samples_[i];
      if (sample.parameter <= first.parameter ||
          sample.parameter >= last.parameter)
        continue;
      checkComparison();
      const auto [distance, offset] = distanceToChord(sample.point, a, b);
      if (distance > tolerance ||
          (progress - offset) * chordLength > 4 * tolerance_)
        throw std::runtime_error("Authored pcurve leaves its shared edge or "
                                 "covers it more than once");
      progress = std::max(progress, offset);
    }
  }

  UV lift(double parameter, Point target) {
    const auto uv = parameterPoint(parameter);
    const auto position = uv ? evaluate(*uv) : std::nullopt;
    if (!position || glm::length(*position - target) > tolerance_)
      throw std::runtime_error(
          "Authored pcurve sample is outside its shared edge");
    auto snap = *uv;
    for (unsigned axis = 0; axis < 2; ++axis) {
      const auto &domain = axis ? surface_.vDomain : surface_.uDomain;
      const auto &knots = axis ? surface_.meshV : surface_.meshU;
      const double period = surface_.periods[axis];
      const double span = domain ? domain->second - domain->first : period;
      const double coordinate = (*uv)[axis];
      double difference =
          span * 1e-7 +
          std::abs(coordinate) * 32. * std::numeric_limits<double>::epsilon();
      const auto include = [&](double knot) {
        if (period)
          knot += std::round((coordinate - knot) / period) * period;
        const double delta = std::abs(coordinate - knot);
        if (delta <= difference) {
          snap[axis] = knot;
          difference = delta;
        }
      };
      if (domain) {
        include(domain->first);
        if (!period)
          include(domain->second);
      }
      for (double knot : knots)
        include(knot);
    }
    if (snap != *uv) {
      const auto snapped = evaluate(snap);
      if (snapped && glm::length(*snapped - target) <= tolerance_)
        return snap;
    }
    return *uv;
  }

  const ParametricSurface &surface_;
  const ParametricCurve &nativeUv_;
  std::span<const glm::vec3> edge_;
  std::stop_token stop_;
  double chord_, witnessChord_, tolerance_, first_, last_;
  std::vector<CurveSample> samples_;
  size_t comparisons_ = 0, evaluations_ = 0;
};

} // namespace

std::vector<UV> liftIfcBrepPcurve(const ParametricSurface &surface,
                                  const ParametricCurve &nativeUv,
                                  std::span<const glm::vec3> sharedPoints,
                                  bool edgeSameSense, double units,
                                  std::stop_token stop) {
  container::util::checkModelImportCancellation(stop);
  if (!surface.point || !nativeUv.point || nativeUv.dimension != 2 ||
      !nativeUv.domain || !std::isfinite(nativeUv.domain->first) ||
      !std::isfinite(nativeUv.domain->second - nativeUv.domain->first) ||
      nativeUv.domain->first >= nativeUv.domain->second ||
      !std::isfinite(units) || units <= 0 || sharedPoints.size() < 2 ||
      sharedPoints.size() > maxEdgeSamples)
    throw std::runtime_error(
        "Invalid authored pcurve or shared edge sample budget");
  return EdgeLift(surface, nativeUv, sharedPoints, edgeSameSense, units, stop)
      .run();
}

} // namespace container::geometry::ifc::detail
