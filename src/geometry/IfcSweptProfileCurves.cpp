#include "IfcSweptProfileCurves.h"

#include "Container/utility/ModelImportCancellation.h"
#include "IfcCurveReader.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace container::geometry::ifc::detail {
namespace {

using Point = glm::dvec2;
constexpr double fullTurn = 2 * std::numbers::pi;
constexpr double quarterTurn = std::numbers::pi / 2;
constexpr double roundoff = 64 * std::numeric_limits<double>::epsilon();
constexpr size_t maxSamples = 4097;
constexpr size_t maxChecks = 8'000'000;

bool finite(Point point) {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

bool fitsRenderer(Point point) {
  constexpr double max = std::numeric_limits<float>::max();
  return finite(point) && std::abs(point.x) <= max && std::abs(point.y) <= max;
}

double wrap(double parameter, double period) {
  const double value = std::fmod(parameter, period);
  const double positive = value < 0 ? value + period : value;
  return positive < period ? positive : 0.;
}

double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }

glm::dvec3 place(const CurveFrame &frame, Point point) {
  return frame.origin + frame.x * point.x + frame.y * point.y;
}

Point unplace(const CurveFrame &frame, glm::dvec3 point) {
  const auto relative = point - frame.origin;
  return {glm::dot(relative, frame.x), glm::dot(relative, frame.y)};
}

struct RectanglePiece {
  Point first, last, center{0};
  double radius = 0;

  Point point(double fraction) const {
    if (fraction == 0)
      return first;
    if (fraction == 1)
      return last;
    if (radius == 0)
      return first + fraction * (last - first);
    const auto radial = first - center;
    const double angle = fraction * quarterTurn;
    return center + std::cos(angle) * radial +
           std::sin(angle) * Point(-radial.y, radial.x);
  }

  double closestFraction(Point target) const {
    const auto displacement = last - first;
    if (radius == 0)
      return std::clamp(glm::dot(target - first, displacement) /
                            glm::dot(displacement, displacement),
                        0., 1.);
    const auto radial = first - center;
    const auto query = target - center;
    const double angle =
        std::atan2(cross(radial, query), glm::dot(radial, query));
    if (angle >= 0 && angle <= quarterTurn)
      return angle / quarterTurn;
    return glm::dot(target - (first + displacement * .5), displacement) > 0
               ? 1.
               : 0.;
  }
};

ParametricCurve roundedRectangle(double width, double height, double radius,
                                 const CurveFrame &frame) {
  const double x = width * .5, y = height * .5;
  if (!(x > 0 && y > 0) || !fitsRenderer({x, y}) || !std::isfinite(radius) ||
      radius < 0 || radius > std::min(x, y))
    throw std::runtime_error(
        "Rectangle dimensions and radius must fit its positive half-extents");

  auto pieces = std::make_shared<std::vector<RectanglePiece>>();
  if (radius == 0) {
    const std::array<Point, 4> corners{{{-x, -y}, {x, -y}, {x, y}, {-x, y}}};
    for (size_t i = 0; i < corners.size(); ++i)
      pieces->push_back({corners[i], corners[(i + 1) % corners.size()]});
  } else {
    const double dx = x - radius, dy = y - radius;
    const std::array<Point, 4> center{
        {{dx, dy}, {-dx, dy}, {-dx, -dy}, {dx, -dy}}};
    const std::array<Point, 4> start{{{x, dy}, {-dx, y}, {-x, -dy}, {dx, -y}}};
    const std::array<Point, 4> end{{{dx, y}, {-x, dy}, {-dx, -y}, {x, -dy}}};
    for (size_t i = 0; i < center.size(); ++i) {
      const auto next = start[(i + 1) % center.size()];
      if (start[i] == end[i])
        throw std::runtime_error(
            "Rectangle fillet collapses at coordinate precision");
      pieces->push_back({start[i], end[i], center[i], radius});
      if (end[i] != next)
        pieces->push_back({end[i], next});
    }
  }

  const double period = double(pieces->size());
  const auto point = [pieces, period,
                      frame](double t) -> std::optional<glm::dvec3> {
    if (!std::isfinite(t))
      return std::nullopt;
    t = wrap(t, period);
    const auto index = std::min(static_cast<size_t>(t), pieces->size() - 1);
    return place(frame, (*pieces)[index].point(t - index));
  };
  ParametricCurve curve;
  curve.point = point;
  curve.domain = {{0., period}};
  curve.period = period;
  curve.dimension = 2;
  for (size_t i = 1; i < pieces->size(); ++i)
    curve.breaks.push_back(double(i));
  curve.derivative = [pieces, period,
                      frame](double t) -> std::optional<glm::dvec3> {
    if (!std::isfinite(t))
      return std::nullopt;
    t = wrap(t, period);
    const auto index = std::min(static_cast<size_t>(t), pieces->size() - 1);
    const auto &piece = (*pieces)[index];
    Point derivative = piece.last - piece.first;
    if (piece.radius != 0) {
      const auto radial = piece.point(t - index) - piece.center;
      derivative = quarterTurn * Point(-radial.y, radial.x);
    }
    return frame.x * derivative.x + frame.y * derivative.y;
  };
  curve.inverse = [pieces, period, frame, x,
                   y](glm::dvec3 target) -> std::optional<double> {
    const Point local = unplace(frame, target);
    if (!finite(local))
      return std::nullopt;
    std::array<std::pair<double, double>, 8> candidates{};
    for (size_t i = 0; i < pieces->size(); ++i) {
      const auto &piece = (*pieces)[i];
      const double fraction = piece.closestFraction(local);
      const auto delta = piece.point(fraction) - local;
      candidates[i] = {glm::length(delta), wrap(i + fraction, period)};
    }
    const double error =
        roundoff * std::max({x, y, std::abs(local.x), std::abs(local.y)});
    const auto last = candidates.begin() + pieces->size();
    const auto best = std::min_element(candidates.begin(), last);
    for (auto candidate = candidates.begin(); candidate != last; ++candidate)
      if (candidate->first <= best->first + error &&
          std::abs(std::remainder(candidate->second - best->second, period)) >
              roundoff)
        return std::nullopt;
    return best->second;
  };
  curve.sampler = [pieces, period, point](double first, double last,
                                          double error, size_t limit) {
    std::vector<CurveSample> result{{first, *point(first)}};
    for (double start = first; start != last;) {
      const double end = first < last ? std::min(last, std::floor(start) + 1)
                                      : std::max(last, std::ceil(start) - 1);
      if (end == start || result.size() == limit)
        return std::vector<CurveSample>{};
      const double middle = wrap(start + (end - start) * .5, period);
      const auto index =
          std::min(static_cast<size_t>(middle), pieces->size() - 1);
      const auto &piece = (*pieces)[index];
      double count = 1;
      if (piece.radius > 0) {
        const double angle =
            4 * std::asin(std::sqrt(std::min(.5, error / piece.radius * .5)));
        count = std::max(
            1., std::ceil(std::abs(end - start) * quarterTurn / angle));
      }
      if (!std::isfinite(count) || count > double(limit - result.size()))
        return std::vector<CurveSample>{};
      const auto segments = static_cast<size_t>(count);
      for (size_t i = 1; i <= segments; ++i) {
        const double t =
            i == segments ? end : start + (end - start) * (double(i) / count);
        if (t == result.back().parameter)
          return std::vector<CurveSample>{};
        result.push_back({t, *point(t)});
      }
      start = end;
    }
    return result;
  };
  return curve;
}

ParametricCurve circle(double radius, double radians, const CurveFrame &frame) {
  if (!(radius > 0) || !fitsRenderer({radius, 0}) || !std::isfinite(radians) ||
      radians <= 0)
    throw std::runtime_error(
        "Hollow circle requires valid radii and project angle units");
  const double period = fullTurn / radians;
  const double speed = radius * radians;
  if (!std::isfinite(period) || period <= 0 || !std::isfinite(speed) ||
      speed <= 0)
    throw std::runtime_error(
        "Hollow circle angle period exceeds parameter precision");
  ParametricCurve curve;
  curve.dimension = 2;
  curve.period = period;
  curve.domain = {{0., period}};
  curve.breaks = {.25 * period, .5 * period, .75 * period};
  curve.point = [=](double t) -> std::optional<glm::dvec3> {
    if (!std::isfinite(t))
      return std::nullopt;
    const double angle = wrap(t, period) * radians;
    return place(frame, radius * Point(std::cos(angle), std::sin(angle)));
  };
  curve.derivative = [=](double t) -> std::optional<glm::dvec3> {
    if (!std::isfinite(t))
      return std::nullopt;
    const double angle = wrap(t, period) * radians;
    return speed * (-std::sin(angle) * frame.x + std::cos(angle) * frame.y);
  };
  curve.inverse = [=](glm::dvec3 point) -> std::optional<double> {
    const auto local = unplace(frame, point);
    if (!finite(local) || local == Point(0))
      return std::nullopt;
    return wrap(std::atan2(local.y, local.x) / radians, period);
  };
  curve.constantSpeed = speed;
  return curve;
}

struct Bounds {
  Point min{std::numeric_limits<double>::infinity()};
  Point max{-std::numeric_limits<double>::infinity()};

  void include(Point point) {
    min = glm::min(min, point);
    max = glm::max(max, point);
  }

  bool overlaps(const Bounds &other, double distance) const {
    return min.x - other.max.x <= distance && other.min.x - max.x <= distance &&
           min.y - other.max.y <= distance && other.min.y - max.y <= distance;
  }
};

struct SampledBoundary {
  std::vector<Point> points;
  Bounds bounds;
  double chordError = 0;
};

SampledBoundary sampleBoundary(const ParametricCurve &curve, double tolerance) {
  if (curve.dimension != 2 || !curve.point || !curve.domain ||
      !std::isfinite(curve.domain->first) ||
      !std::isfinite(curve.domain->second) ||
      curve.domain->first >= curve.domain->second)
    throw std::runtime_error(
        "Swept profile boundary must be a bounded 2D curve");
  const auto [first, last] = *curve.domain;
  Bounds estimate;
  const auto checkedPoint = [](const std::optional<glm::dvec3> &point) {
    if (!point || point->z != 0 || !fitsRenderer(Point(*point)))
      throw std::runtime_error(
          "Swept profile boundary is outside finite 2D renderer coordinates");
    return Point(*point);
  };
  for (size_t i = 0; i <= 16; ++i)
    estimate.include(
        checkedPoint(curve.point(std::lerp(first, last, double(i) / 16))));
  const auto extent = estimate.max - estimate.min;
  SampledBoundary result;
  result.chordError =
      std::max(tolerance * .25, std::max(extent.x, extent.y) * 1e-6);
  const auto samples =
      sampleCurve(curve, first, last, result.chordError, maxSamples);
  if (samples.size() < 2)
    throw std::runtime_error("Swept profile boundary exceeds its validation "
                             "sample budget or collapses");
  result.points.reserve(samples.size());
  for (const auto &sample : samples) {
    const auto point = checkedPoint(sample.point);
    if (!result.points.empty() && point == result.points.back())
      throw std::runtime_error("Swept profile boundary has a zero-length step");
    result.points.push_back(point);
    result.bounds.include(point);
  }
  return result;
}

double pointSegmentDistance(Point point, Point first, Point last) {
  const auto edge = last - first;
  const double fraction =
      std::clamp(glm::dot(point - first, edge) / glm::dot(edge, edge), 0., 1.);
  return glm::length(point - first - edge * fraction);
}

bool segmentsClose(Point a, Point b, Point c, Point d, double distance) {
  Bounds ab, cd;
  ab.include(a);
  ab.include(b);
  cd.include(c);
  cd.include(d);
  if (!ab.overlaps(cd, distance))
    return false;
  const auto x = b - a, y = d - c;
  const double ac = cross(x, c - a), ad = cross(x, d - a);
  const double ca = cross(y, a - c), cb = cross(y, b - c);
  if (((ac < 0 && ad > 0) || (ac > 0 && ad < 0)) &&
      ((ca < 0 && cb > 0) || (ca > 0 && cb < 0)))
    return true;
  return pointSegmentDistance(a, c, d) <= distance ||
         pointSegmentDistance(b, c, d) <= distance ||
         pointSegmentDistance(c, a, b) <= distance ||
         pointSegmentDistance(d, a, b) <= distance;
}

const StepValue *argument(const Entity &entity, size_t index) {
  return index < entity.args.list.size() ? &entity.args.list[index] : nullptr;
}

bool omitted(const StepValue *value) {
  return !value || value->kind == StepValue::Kind::Omitted;
}

double number(const Entity &entity, size_t index, const char *name,
              std::optional<double> fallback = std::nullopt) {
  const auto *value = argument(entity, index);
  if (omitted(value) && fallback)
    return *fallback;
  if (!value || value->kind != StepValue::Kind::Number ||
      !std::isfinite(value->number))
    throw std::runtime_error(std::string(name) + " must be a finite number");
  return value->number;
}

uint32_t reference(const StepValue *value, const char *name) {
  if (!value || value->kind != StepValue::Kind::Ref || value->ref == 0)
    throw std::runtime_error(std::string(name) +
                             " must be an entity reference");
  return value->ref;
}

bool isCurveProfile(const Entity &entity) {
  const auto *type = argument(entity, 0);
  return type && type->kind == StepValue::Kind::Enum && type->text == "CURVE";
}

class ProfileReader {
public:
  ProfileReader(const IfcSweptProfileCallbacks &callbacks, double tolerance,
                std::optional<double> angle, std::stop_token stop)
      : callbacks_(callbacks), tolerance_(tolerance), angle_(angle),
        stop_(stop) {}

  std::vector<IfcSweptProfileBoundary> read(uint32_t id,
                                            bool compositeMember = false) {
    container::util::checkModelImportCancellation(stop_);
    if (visiting_.size() >= 64 || ++visits_ > 4096 ||
        !visiting_.insert(id).second)
      throw std::runtime_error("Cyclic or excessive swept profile graph at #" +
                               std::to_string(id));
    struct Visit {
      std::unordered_set<uint32_t> &path;
      uint32_t id;
      ~Visit() { path.erase(id); }
    } visit{visiting_, id};
    const auto &entity = callbacks_.entity(id);
    try {
      if (compositeMember && !isCurveProfile(entity))
        throw std::runtime_error(
            "A composite swept profile member must have CURVE type");
      if (entity.type == "IFCDERIVEDPROFILEDEF" ||
          entity.type == "IFCMIRROREDPROFILEDEF") {
        const auto transform = callbacks_.readTransform(id);
        auto boundaries = read(transform.parent, compositeMember);
        const bool singleMember =
            std::ranges::all_of(boundaries, [&](const auto &boundary) {
              return boundary.sourceId == boundaries.front().sourceId;
            });
        for (auto &boundary : boundaries) {
          boundary.curve =
              callbacks_.applyTransform(std::move(boundary.curve), transform);
          if (singleMember)
            boundary.sourceId = id;
        }
        return boundaries;
      }
      if (entity.type == "IFCCOMPOSITEPROFILEDEF") {
        if (compositeMember)
          throw std::runtime_error(
              "Nested composite swept profiles are unsupported");
        return composite(entity);
      }
      if (entity.type == "IFCCENTERLINEPROFILEDEF") {
        requireCurve(entity);
        if (!(number(entity, 3, "Centerline Thickness") > 0))
          throw std::runtime_error("Centerline Thickness must be positive");
        auto curve = callbacks_.readCurve(
            reference(argument(entity, 2), "Centerline Curve"));
        validateCenterline(curve);
        return {{id, std::move(curve)}};
      }
      if (entity.type == "IFCCIRCLEHOLLOWPROFILEDEF") {
        requireCurve(entity);
        const auto frame = placement(entity);
        const auto radius = number(entity, 3, "Radius");
        const auto wall = number(entity, 4, "WallThickness");
        if (!(wall > 0 && wall < radius))
          throw std::runtime_error(
              "WallThickness must be positive and smaller than Radius");
        const double inner = radius - wall;
        if (inner == radius)
          throw std::runtime_error(
              "WallThickness collapses at profile coordinate precision");
        return {{id, circle(radius, angle_.value_or(0.), frame)},
                {id, circle(inner, angle_.value_or(0.), frame), true}};
      }
      if (entity.type == "IFCRECTANGLEHOLLOWPROFILEDEF" ||
          entity.type == "IFCROUNDEDRECTANGLEPROFILEDEF") {
        requireCurve(entity);
        return rectangles(entity);
      }
      if (entity.type == "IFCARBITRARYPROFILEDEFWITHVOIDS")
        throw std::runtime_error("An arbitrary profile with voids requires "
                                 "AREA type and cannot be a swept curve");
      return {{id, callbacks_.readSingleProfile(id)}};
    } catch (const std::runtime_error &error) {
      throw std::runtime_error(entity.type + " #" + std::to_string(id) + ": " +
                               error.what());
    }
  }

private:
  void requireCurve(const Entity &entity) const {
    if (!isCurveProfile(entity))
      throw std::runtime_error("Swept surface profile must have CURVE type");
  }

  CurveFrame placement(const Entity &entity) const {
    const auto *value = argument(entity, 2);
    return omitted(value) ? CurveFrame{}
                          : callbacks_.readProfilePlacement(
                                reference(value, "Profile Position"));
  }

  std::vector<IfcSweptProfileBoundary> rectangles(const Entity &entity) const {
    const auto frame = placement(entity);
    const auto width = number(entity, 3, "XDim");
    const auto height = number(entity, 4, "YDim");
    if (entity.type == "IFCROUNDEDRECTANGLEPROFILEDEF") {
      const auto radius = number(entity, 5, "RoundingRadius");
      if (!(radius > 0))
        throw std::runtime_error("RoundingRadius must be positive");
      return {{entity.id, roundedRectangle(width, height, radius, frame)}};
    }
    const auto wall = number(entity, 5, "WallThickness");
    if (!(wall > 0 && wall < .5 * std::min(width, height)))
      throw std::runtime_error(
          "WallThickness must be positive and smaller than both half-extents");
    const auto inner = number(entity, 6, "InnerFilletRadius", 0.);
    const auto outer = number(entity, 7, "OuterFilletRadius", 0.);
    const double innerWidth = width - 2 * wall, innerHeight = height - 2 * wall;
    if (innerWidth == width || innerHeight == height)
      throw std::runtime_error(
          "WallThickness collapses at profile coordinate precision");
    // The support gap between the convex rings reaches its minimum in an
    // axial or diagonal direction. This catches tangency between arc samples.
    const double diagonal = std::sqrt(2.);
    if (diagonal * wall <= (diagonal - 1) * (outer - inner))
      throw std::runtime_error(
          "Inner profile touches or crosses the rounded exterior");
    return {{entity.id, roundedRectangle(width, height, outer, frame)},
            {entity.id, roundedRectangle(innerWidth, innerHeight, inner, frame),
             true}};
  }

  std::vector<IfcSweptProfileBoundary> composite(const Entity &entity) {
    requireCurve(entity);
    const auto *value = argument(entity, 2);
    if (!value || value->kind != StepValue::Kind::List ||
        value->list.size() < 2 || value->list.size() > 64)
      throw std::runtime_error(
          "Composite swept profile requires 2 to 64 distinct CURVE members");
    std::vector<uint32_t> members;
    for (const auto &entry : value->list)
      members.push_back(reference(&entry, "Composite member"));
    std::ranges::sort(members);
    if (std::adjacent_find(members.begin(), members.end()) != members.end())
      throw std::runtime_error(
          "Composite swept profile requires distinct member references");
    std::vector<IfcSweptProfileBoundary> boundaries;
    std::vector<SampledBoundary> sampled;
    size_t sampleCount = 0;
    for (const auto member : members) {
      const size_t priorBoundaries = boundaries.size();
      for (auto &boundary : read(member, true)) {
        auto points = sampleBoundary(boundary.curve, tolerance_);
        sampleCount += points.points.size();
        if (sampleCount > 32768)
          throw std::runtime_error("Composite swept profile exceeds its total "
                                   "validation sample budget");
        for (size_t i = 0; i < priorBoundaries; ++i) {
          if (boundariesTouch(sampled[i], points))
            throw std::runtime_error(
                "Composite member #" + std::to_string(boundaries[i].sourceId) +
                " touches or crosses member #" + std::to_string(member));
        }
        boundaries.push_back(std::move(boundary));
        sampled.push_back(std::move(points));
      }
    }
    return boundaries;
  }

  void consumeCheck() {
    if (checks_ == maxChecks)
      throw std::runtime_error("Swept profile intersection validation exceeds "
                               "its comparison budget");
    ++checks_;
  }

  bool boundariesTouch(const SampledBoundary &a, const SampledBoundary &b) {
    const double clearance = tolerance_ + a.chordError + b.chordError;
    if (!a.bounds.overlaps(b.bounds, clearance))
      return false;
    for (size_t i = 1; i < a.points.size(); ++i) {
      container::util::checkModelImportCancellation(stop_);
      Bounds segment;
      segment.include(a.points[i - 1]);
      segment.include(a.points[i]);
      if (!segment.overlaps(b.bounds, clearance))
        continue;
      for (size_t j = 1; j < b.points.size(); ++j) {
        consumeCheck();
        if (segmentsClose(a.points[i - 1], a.points[i], b.points[j - 1],
                          b.points[j], clearance))
          return true;
      }
    }
    return false;
  }

  void validateCenterline(const ParametricCurve &curve) {
    const auto sampled = sampleBoundary(curve, tolerance_);
    if (curve.period > 0 || glm::length(sampled.points.back() -
                                        sampled.points.front()) <= tolerance_)
      throw std::runtime_error("Centerline Curve must be open");
    const double clearance = tolerance_ + 2 * sampled.chordError;
    for (size_t i = 1; i < sampled.points.size(); ++i) {
      container::util::checkModelImportCancellation(stop_);
      const auto a = sampled.points[i - 1], b = sampled.points[i];
      if (i + 1 < sampled.points.size()) {
        const auto incoming = b - a;
        const auto outgoing = sampled.points[i + 1] - b;
        if (glm::dot(incoming, outgoing) < 0 &&
            std::abs(cross(incoming, outgoing)) <=
                roundoff * glm::length(incoming) * glm::length(outgoing))
          throw std::runtime_error("Centerline Curve reverses along itself");
      }
      for (size_t j = i + 2; j < sampled.points.size(); ++j) {
        consumeCheck();
        if (segmentsClose(a, b, sampled.points[j - 1], sampled.points[j],
                          clearance))
          throw std::runtime_error(
              "Centerline Curve crosses or touches itself");
      }
    }
  }

  const IfcSweptProfileCallbacks &callbacks_;
  double tolerance_;
  std::optional<double> angle_;
  std::stop_token stop_;
  std::unordered_set<uint32_t> visiting_;
  size_t visits_ = 0;
  size_t checks_ = 0;
};

} // namespace

std::vector<IfcSweptProfileBoundary> readIfcSweptProfileBoundaries(
    uint32_t profile, const IfcSweptProfileCallbacks &callbacks,
    double pointTolerance, std::optional<double> radiansPerAngleUnit,
    std::stop_token stop) {
  if (!(pointTolerance > 0) || !std::isfinite(pointTolerance))
    throw std::runtime_error(
        "Swept profile point tolerance must be finite and positive");
  return ProfileReader(callbacks, pointTolerance, radiansPerAngleUnit, stop)
      .read(profile);
}

} // namespace container::geometry::ifc::detail
