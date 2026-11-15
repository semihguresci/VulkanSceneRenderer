#include "IfcBrepTrimPlacement.h"

#include "Container/utility/ModelImportCancellation.h"
#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <limits>
#include <stdexcept>

namespace container::geometry::ifc::detail {
namespace {

using UV = glm::dvec2;
constexpr double epsilon = 1e-10;
constexpr double maxPeriodOffset = 1'048'576;
constexpr size_t maxSamples = 8192;
constexpr size_t maxOffsetsPerLoop = 256;
constexpr size_t maxPlacedSamples = 524288;
constexpr size_t maxChecks = 8'000'000;

bool finite(UV p) { return std::isfinite(p.x) && std::isfinite(p.y); }

struct Bounds {
  UV low{std::numeric_limits<double>::infinity()};
  UV high{-std::numeric_limits<double>::infinity()};

  void include(UV p) {
    low = glm::min(low, p);
    high = glm::max(high, p);
  }

  Bounds translated(UV shift) const { return {low + shift, high + shift}; }
};

bool meets(Bounds a, Bounds b) {
  return a.low.x <= b.high.x + epsilon && b.low.x <= a.high.x + epsilon &&
         a.low.y <= b.high.y + epsilon && b.low.y <= a.high.y + epsilon;
}

bool encloses(Bounds outer, Bounds inner) {
  return inner.low.x > outer.low.x + epsilon &&
         inner.high.x < outer.high.x - epsilon &&
         inner.low.y > outer.low.y + epsilon &&
         inner.high.y < outer.high.y - epsilon;
}

struct Segment {
  UV a, b;
  Bounds bounds;
  double sideTolerance;
};

Segment segment(UV a, UV b) {
  const UV d = glm::abs(b - a);
  return {a, b, {glm::min(a, b), glm::max(a, b)}, epsilon * std::max(d.x, d.y)};
}

double cross(UV a, UV b) { return a.x * b.y - a.y * b.x; }

int side(const Segment &s, UV p) {
  const double area = cross(s.b - s.a, p - s.a);
  return (area > s.sideTolerance) - (area < -s.sideTolerance);
}

bool segmentsMeet(const Segment &a, const Segment &b) {
  if (!meets(a.bounds, b.bounds))
    return false;
  const int ab = side(a, b.a), ac = side(a, b.b);
  const int ba = side(b, a.a), bc = side(b, a.b);
  if (ab * ac < 0 && ba * bc < 0)
    return true;
  return (!ab && meets(a.bounds, {b.a, b.a})) ||
         (!ac && meets(a.bounds, {b.b, b.b})) ||
         (!ba && meets(b.bounds, {a.a, a.a})) ||
         (!bc && meets(b.bounds, {a.b, a.b}));
}

struct Placement {
  UV offset;
  Bounds bounds;
  std::vector<Segment> segments;
};

class TrimPlacement {
public:
  TrimPlacement(std::span<const CurvedFaceChartLoop> loops,
                std::span<const uint8_t> anchored, UV periods,
                const std::stop_token &stop)
      : loops_(loops), anchored_(anchored), periods_(periods), stop_(stop) {}

  std::vector<UV> run();

private:
  struct HoleChoices {
    size_t loop;
    std::vector<Placement> choices;
  };

  void checkWork() {
    if (++checks_ > maxChecks)
      throw std::runtime_error(
          "Periodic spline trim placement exceeds its work budget");
    if (checks_ % 256 == 0)
      container::util::checkModelImportCancellation(stop_);
  }

  std::vector<UV> offsets(Bounds range) const;
  Placement place(size_t index, UV offset, UV origin);
  bool containsPoint(const Placement &loop, UV p);
  bool boundariesMeet(const Placement &a, const Placement &b);
  bool disjoint(const Placement &a, const Placement &b);
  void validateSimple(const Placement &loop);
  void search(const Placement &outer, std::span<const HoleChoices> holes);

  std::span<const CurvedFaceChartLoop> loops_;
  std::span<const uint8_t> anchored_;
  UV periods_, scale_{};
  const std::stop_token &stop_;
  std::vector<Bounds> bounds_;
  std::optional<std::vector<UV>> solution_;
  size_t checks_ = 0;
  size_t placedSamples_ = 0;
};

std::vector<UV> TrimPlacement::offsets(Bounds range) const {
  std::array<int, 2> first{}, last{};
  for (unsigned axis = 0; axis < 2; ++axis) {
    const double low = range.low[axis] - epsilon * scale_[axis];
    const double high = range.high[axis] + epsilon * scale_[axis];
    const double period = periods_[axis];
    if (low > high)
      return {};
    if (period == 0.) {
      if (low > 0. || high < 0.)
        return {};
      continue;
    }
    const double begin = std::ceil(low / period),
                 end = std::floor(high / period);
    if (begin > end)
      return {};
    if (!std::isfinite(begin) || !std::isfinite(end) ||
        std::abs(begin) > maxPeriodOffset || std::abs(end) > maxPeriodOffset ||
        end - begin + 1 > maxOffsetsPerLoop)
      throw std::runtime_error(
          "Periodic spline trim exceeds its offset budget");
    first[axis] = static_cast<int>(begin);
    last[axis] = static_cast<int>(end);
  }
  const auto count = static_cast<size_t>(last[0] - first[0] + 1) *
                     static_cast<size_t>(last[1] - first[1] + 1);
  if (count > maxOffsetsPerLoop)
    throw std::runtime_error("Periodic spline trim exceeds its offset budget");
  std::vector<UV> result;
  result.reserve(count);
  for (int u = first[0]; u <= last[0]; ++u)
    for (int v = first[1]; v <= last[1]; ++v)
      result.push_back(UV(u, v) * periods_);
  return result;
}

Placement TrimPlacement::place(size_t index, UV offset, UV origin) {
  const auto &vertices = loops_[index].vertices;
  if ((placedSamples_ += vertices.size()) > maxPlacedSamples)
    throw std::runtime_error(
        "Periodic spline trim exceeds its placement sample budget");
  Placement result{offset, {}, {}};
  result.segments.reserve(vertices.size());
  const UV first = vertices.front();
  const UV translatedFirst = first + offset;
  const auto normalize = [&](UV p) {
    // Subtract the candidate origin before any geometric predicates. Evaluate
    // the same UV addition that the loader will publish.
    const UV translated = p + offset;
    const UV value = (translated - origin) / scale_;
    if (!finite(value))
      throw std::runtime_error(
          "Periodic spline trim translation exceeds chart precision");
    const UV drift =
        glm::abs(((translated - translatedFirst) - (p - first)) / scale_);
    if (drift.x > 8 * epsilon || drift.y > 8 * epsilon)
      throw std::runtime_error(
          "Periodic spline trim translation loses chart precision");
    return value;
  };
  UV prior = normalize(vertices.back());
  for (UV p : vertices) {
    checkWork();
    const UV next = normalize(p);
    result.bounds.include(next);
    result.segments.push_back(segment(prior, next));
    prior = next;
  }
  return result;
}

bool TrimPlacement::containsPoint(const Placement &loop, UV p) {
  bool inside = false;
  for (const Segment &s : loop.segments) {
    checkWork();
    if (meets(s.bounds, {p, p}) && side(s, p) == 0)
      return false;
    if ((s.a.y > p.y) != (s.b.y > p.y)) {
      const double x = std::lerp(s.a.x, s.b.x, (p.y - s.a.y) / (s.b.y - s.a.y));
      if (p.x < x)
        inside = !inside;
    }
  }
  return inside;
}

bool TrimPlacement::boundariesMeet(const Placement &a, const Placement &b) {
  for (const Segment &x : a.segments) {
    checkWork();
    if (!meets(x.bounds, b.bounds))
      continue;
    for (const Segment &y : b.segments) {
      checkWork();
      if (segmentsMeet(x, y))
        return true;
    }
  }
  return false;
}

bool TrimPlacement::disjoint(const Placement &a, const Placement &b) {
  return !meets(a.bounds, b.bounds) ||
         (!boundariesMeet(a, b) && !containsPoint(a, b.segments.front().b) &&
          !containsPoint(b, a.segments.front().b));
}

void TrimPlacement::validateSimple(const Placement &loop) {
  double area = 0.;
  const auto &segments = loop.segments;
  for (size_t i = 0; i < segments.size(); ++i) {
    const Segment &a = segments[i];
    const Segment &next = segments[(i + 1) % segments.size()];
    const UV d = glm::abs(a.b - a.a);
    if (std::max(d.x, d.y) <= epsilon ||
        (side(a, next.b) == 0 && side(next, a.a) == 0 &&
         (meets(a.bounds, {next.b, next.b}) || meets(next.bounds, {a.a, a.a}))))
      throw std::runtime_error("Degenerate periodic spline trim segment");
    area += cross(a.a, a.b);
    for (size_t j = i + 2; j < segments.size(); ++j) {
      if (i == 0 && j + 1 == segments.size())
        continue;
      checkWork();
      if (segmentsMeet(a, segments[j]))
        throw std::runtime_error("Periodic spline trim loop intersects itself");
    }
  }
  if (!std::isfinite(area) || std::abs(area) <= epsilon)
    throw std::runtime_error("Degenerate periodic spline trim loop area");
}

void TrimPlacement::search(const Placement &outer,
                           std::span<const HoleChoices> holes) {
  std::vector<size_t> selected(holes.size()), next(holes.size());
  size_t depth = 0;
  while (true) {
    checkWork();
    if (depth == holes.size()) {
      if (solution_)
        throw std::runtime_error("Ambiguous periodic spline trim placement");
      auto &solution = solution_.emplace(loops_.size(), UV(0));
      solution.front() = outer.offset;
      for (size_t i = 0; i < holes.size(); ++i)
        solution[holes[i].loop] = holes[i].choices[selected[i]].offset;
      if (depth == 0)
        return;
      --depth;
      continue;
    }
    const auto &choices = holes[depth].choices;
    if (next[depth] == choices.size()) {
      if (depth == 0)
        return;
      --depth;
      continue;
    }
    const size_t candidate = next[depth]++;
    bool fits = true;
    for (size_t i = 0; i < depth; ++i) {
      checkWork();
      if (!disjoint(choices[candidate], holes[i].choices[selected[i]])) {
        fits = false;
        break;
      }
    }
    if (!fits)
      continue;
    selected[depth++] = candidate;
    if (depth < holes.size())
      next[depth] = 0;
  }
}

std::vector<UV> TrimPlacement::run() {
  container::util::checkModelImportCancellation(stop_);
  if (loops_.empty() || loops_.size() != anchored_.size() ||
      loops_.size() > maxSamples / 3 || !finite(periods_) || periods_.x < 0 ||
      periods_.y < 0)
    throw std::runtime_error("Invalid periodic spline trim placement input");
  if (periods_ != UV(0) &&
      std::ranges::none_of(anchored_, [](uint8_t a) { return a != 0; }))
    throw std::runtime_error(
        "Periodic spline trim placement needs an authored anchor");

  bounds_.resize(loops_.size());
  size_t samples = 0;
  for (size_t i = 0; i < loops_.size(); ++i) {
    const auto &vertices = loops_[i].vertices;
    if (vertices.size() < 3 || vertices.size() > maxSamples - samples)
      throw std::runtime_error(
          "Invalid or excessive periodic spline trim samples");
    samples += vertices.size();
    for (const UV p : vertices) {
      checkWork();
      if (!finite(p))
        throw std::runtime_error("Nonfinite periodic spline trim sample");
      bounds_[i].include(p);
    }
  }
  const Bounds outerBounds = bounds_.front();
  scale_ = outerBounds.high - outerBounds.low;
  if (!finite(scale_) || scale_.x <= 0. || scale_.y <= 0.)
    throw std::runtime_error("Degenerate periodic spline trim outer extent");
  for (size_t i = 0; i < loops_.size(); ++i) {
    const UV extent = (bounds_[i].high - bounds_[i].low) / scale_;
    if (!finite(extent) || (i && (extent.x >= 1. || extent.y >= 1.)))
      throw std::runtime_error(
          "Periodic spline trim hole cannot fit its outer bound");
    validateSimple(place(i, UV(0), loops_[i].vertices.front()));
  }

  std::vector<UV> outerOffsets{UV(0)};
  if (!anchored_.front() && periods_ != UV(0)) {
    Bounds range{UV(-std::numeric_limits<double>::infinity()),
                 UV(std::numeric_limits<double>::infinity())};
    for (size_t i = 1; i < loops_.size(); ++i)
      if (anchored_[i]) {
        range.low = glm::max(range.low, bounds_[i].high - outerBounds.high);
        range.high = glm::min(range.high, bounds_[i].low - outerBounds.low);
      }
    outerOffsets = offsets(range);
  }
  for (const UV offset : outerOffsets) {
    const UV origin = loops_.front().vertices.front() + offset;
    const auto outer = place(0, offset, origin);
    const Bounds shiftedOuter = outerBounds.translated(offset);
    std::vector<HoleChoices> holes;
    for (size_t i = 1; i < loops_.size(); ++i) {
      HoleChoices hole{i, {}};
      const auto possible =
          anchored_[i] ? std::vector{UV(0)}
                       : offsets({shiftedOuter.low - bounds_[i].low,
                                  shiftedOuter.high - bounds_[i].high});
      for (const UV shift : possible) {
        const Bounds box = bounds_[i].translated(shift);
        if (!encloses(outer.bounds, {(box.low - origin) / scale_,
                                     (box.high - origin) / scale_}))
          continue;
        auto placed = place(i, shift, origin);
        if (encloses(outer.bounds, placed.bounds) &&
            containsPoint(outer, placed.segments.front().b) &&
            !boundariesMeet(outer, placed))
          hole.choices.push_back(std::move(placed));
      }
      if (hole.choices.empty())
        break;
      holes.push_back(std::move(hole));
    }
    if (holes.size() + 1 == loops_.size()) {
      std::ranges::stable_sort(holes, {}, [](const HoleChoices &hole) {
        return hole.choices.size();
      });
      search(outer, holes);
    }
  }
  if (!solution_)
    throw std::runtime_error("No valid periodic spline trim placement");
  return std::move(*solution_);
}

} // namespace

std::vector<glm::dvec2>
placeIfcBrepPeriodicTrimLoops(std::span<const CurvedFaceChartLoop> loops,
                              std::span<const uint8_t> anchored,
                              glm::dvec2 periods, std::stop_token stop) {
  return TrimPlacement(loops, anchored, periods, stop).run();
}

} // namespace container::geometry::ifc::detail
