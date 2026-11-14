#include "Container/renderer/bim/BimSunHours.h"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace container::renderer {
namespace {

constexpr uint32_t kMinutesPerDay = 24 * 60;
constexpr uint32_t kTrianglesPerLeaf = 8;
constexpr size_t kMaxTraversalTests = 100'000'000;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr double kRoundoff = std::numeric_limits<double>::epsilon();

struct Cancelled {};

void CheckStop(std::stop_token stop) {
  if (stop.stop_requested()) {
    throw Cancelled{};
  }
}

bool IsFinite(const glm::dvec3& point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

size_t SampleCount(const BimSunHoursSettings& settings) {
  const auto minutes = settings.endMinute - settings.startMinute;
  return (minutes + settings.stepMinutes - 1) / settings.stepMinutes;
}

glm::dvec3 SolarDirection(const SolarTime& time, double northRadians) {
  const auto angles = solarPosition(time);
  const double azimuth = glm::radians(angles.azimuthDegrees) + northRadians;
  const double elevation = glm::radians(angles.elevationDegrees);
  const double horizontal = std::cos(elevation);
  return {horizontal * std::sin(azimuth), std::sin(elevation), horizontal * std::cos(azimuth)};
}

void SetMinute(SolarTime& time, uint32_t minute) {
  time.hour = static_cast<int>(minute / 60);
  time.minute = static_cast<int>(minute % 60);
}

glm::dvec3 SampleDirection(const BimSunHoursSettings& settings, uint32_t twiceMinute) {
  auto time = settings.solar;
  const double north = glm::radians(std::remainder(settings.northDegrees, 360.0));
  const uint32_t minute = twiceMinute / 2;
  SetMinute(time, minute);
  const auto lower = SolarDirection(time, north);
  if (twiceMinute % 2 == 0) {
    return lower;
  }

  // The existing solar calculator has integer-minute input. Interpolate
  // directions across half minutes, including the last interval of a day.
  if (minute + 1 == kMinutesPerDay) {
    using namespace std::chrono;
    const sys_days date = year{time.year} / unsigned(time.month) / unsigned(time.day);
    const year_month_day tomorrow{date + days{1}};
    time.year = int(tomorrow.year());
    time.month = int(unsigned(tomorrow.month()));
    time.day = int(unsigned(tomorrow.day()));
    SetMinute(time, 0);
    if (!validSolarTime(time)) {
      // At the end of the calculator's supported dates, extend the last
      // minute by half a minute using the preceding two directions.
      time = settings.solar;
      SetMinute(time, minute - 1);
      return glm::normalize(lower + 0.5 * (lower - SolarDirection(time, north)));
    }
  } else {
    SetMinute(time, minute + 1);
  }
  return glm::normalize(lower + SolarDirection(time, north));
}

struct Bounds {
  glm::dvec3 min{kInfinity};
  glm::dvec3 max{-kInfinity};

  void Include(const glm::dvec3& point) {
    for (int axis = 0; axis < 3; ++axis) {
      min[axis] = std::min(min[axis], point[axis]);
      max[axis] = std::max(max[axis], point[axis]);
    }
  }

  void Pad() {
    for (int axis = 0; axis < 3; ++axis) {
      min[axis] = std::nextafter(min[axis], -kInfinity);
      max[axis] = std::nextafter(max[axis], kInfinity);
    }
  }
};

// The axis and shear factors are shared by all grid rays for this sun sample.
struct RayDirection {
  explicit RayDirection(const glm::dvec3& sun) : direction(sun) {
    for (int axis = 1; axis < 3; ++axis) {
      if (std::abs(sun[axis]) > std::abs(sun[depthAxis])) {
        depthAxis = axis;
      }
    }
    xAxis = (depthAxis + 1) % 3;
    yAxis = (depthAxis + 2) % 3;
    shearX = sun[xAxis] / sun[depthAxis];
    shearY = sun[yAxis] / sun[depthAxis];
  }

  glm::dvec3 direction;
  int depthAxis{};
  int xAxis{};
  int yAxis{};
  double shearX{};
  double shearY{};
};

bool Intersects(const Bounds& bounds, const glm::dvec3& origin, const glm::dvec3& direction) {
  double enter = 0.0;
  double exit = kInfinity;
  for (int axis = 0; axis < 3; ++axis) {
    if (direction[axis] == 0.0) {
      if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis]) {
        return false;
      }
      continue;
    }
    auto near = (bounds.min[axis] - origin[axis]) / direction[axis];
    auto far = (bounds.max[axis] - origin[axis]) / direction[axis];
    if (near > far) {
      std::swap(near, far);
    }
    enter = std::max(enter, near);
    // Keep the slab exit conservative when subtraction and division round in
    // opposite directions at an adjoining edge.
    exit = std::min(exit, far >= 0.0 ? far * (1.0 + 8.0 * kRoundoff) : far);
    if (exit < enter) {
      return false;
    }
  }
  return exit > 0.0;
}

bool Intersects(const BimSunTriangle& triangle, const glm::dvec3& origin,
                const RayDirection& ray) {
  // Project into the ray's coordinate frame before testing edge signs. Shared
  // edges give opposite evaluations for the two adjacent triangles.
  // Woop, Benthin and Wald, JCGT 2(1), 2013.
  const auto a = triangle.vertices[0] - origin;
  const auto b = triangle.vertices[1] - origin;
  const auto c = triangle.vertices[2] - origin;
  const double ax = a[ray.xAxis] - ray.shearX * a[ray.depthAxis];
  const double ay = a[ray.yAxis] - ray.shearY * a[ray.depthAxis];
  const double bx = b[ray.xAxis] - ray.shearX * b[ray.depthAxis];
  const double by = b[ray.yAxis] - ray.shearY * b[ray.depthAxis];
  const double cx = c[ray.xAxis] - ray.shearX * c[ray.depthAxis];
  const double cy = c[ray.yAxis] - ray.shearY * c[ray.depthAxis];
  const double u = cx * by - cy * bx;
  const double v = ax * cy - ay * cx;
  const double w = bx * ay - by * ax;
  if ((u < 0.0 || v < 0.0 || w < 0.0) && (u > 0.0 || v > 0.0 || w > 0.0)) {
    return false;
  }
  const double determinant = u + v + w;
  if (determinant == 0.0) {
    return false;
  }
  const double depth =
      (u * a[ray.depthAxis] + v * b[ray.depthAxis] + w * c[ray.depthAxis]) /
      determinant;
  return (ray.direction[ray.depthAxis] > 0.0 && depth > 0.0) ||
         (ray.direction[ray.depthAxis] < 0.0 && depth < 0.0);
}

struct Triangle {
  BimSunTriangle geometry;
  Bounds bounds;
  glm::dvec3 center;
};

struct Node {
  Bounds bounds;
  uint32_t first{};
  uint32_t count{};
  uint32_t right{};
};

class Occluders {
public:
  Occluders(std::span<const BimSunTriangle> source, const glm::dvec3& worldCenter,
            std::stop_token stop, size_t& skipped)
      : stop_(stop) {
    triangles_.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
      if (i % 256 == 0) {
        CheckStop(stop_);
      }
      Triangle triangle;
      for (size_t corner = 0; corner < 3; ++corner) {
        const auto point = source[i].vertices[corner] - worldCenter;
        triangle.geometry.vertices[corner] = point;
        triangle.bounds.Include(point);
      }
      const auto& [a, b, c] = triangle.geometry.vertices;
      const auto normal = glm::cross(b - a, c - a);
      if (!IsFinite(a) || !IsFinite(b) || !IsFinite(c) ||
          !IsFinite(normal) || normal == glm::dvec3{0.0}) {
        ++skipped;
        continue;
      }
      if (triangle.bounds.max.y <= 0.0) {
        continue; // A ray towards the sun stays above the receiver plane.
      }
      for (int axis = 0; axis < 3; ++axis) {
        triangle.center[axis] = std::midpoint(triangle.bounds.min[axis], triangle.bounds.max[axis]);
      }
      triangles_.push_back(triangle);
    }
    if (!triangles_.empty()) {
      indices_.resize(triangles_.size());
      std::iota(indices_.begin(), indices_.end(), 0u);
      Build(0, static_cast<uint32_t>(indices_.size()));
    }
  }

  [[nodiscard]] bool Blocked(const glm::dvec3& origin, const RayDirection& ray) {
    return !nodes_.empty() && BlockedByNode(0, origin, ray);
  }

  [[nodiscard]] bool Empty() const { return nodes_.empty(); }

private:
  uint32_t Build(uint32_t first, uint32_t last) {
    CheckStop(stop_);
    Bounds bounds;
    Bounds centers;
    for (uint32_t id = first; id < last; ++id) {
      if (id % 256 == 0) {
        CheckStop(stop_);
      }
      const auto& triangle = triangles_[indices_[id]];
      bounds.Include(triangle.bounds.min);
      bounds.Include(triangle.bounds.max);
      centers.Include(triangle.center);
    }
    bounds.Pad();
    const auto node = static_cast<uint32_t>(nodes_.size());
    const auto count = last - first;
    nodes_.push_back({bounds, first, count, 0});
    if (count <= kTrianglesPerLeaf) {
      return node;
    }

    const auto extent = centers.max - centers.min;
    int axis = extent.x >= extent.y ? 0 : 1;
    if (extent.z > extent[axis]) {
      axis = 2;
    }
    const auto middle = first + count / 2;
    std::nth_element(indices_.begin() + first, indices_.begin() + middle,
                     indices_.begin() + last, [&](uint32_t a, uint32_t b) {
                       return triangles_[a].center[axis] < triangles_[b].center[axis];
                     });
    const auto left = Build(first, middle);
    const auto right = Build(middle, last);
    nodes_[node] = {bounds, left, 0, right};
    return node;
  }

  void CountTest() {
    if (tests_ % 256 == 0) {
      CheckStop(stop_);
    }
    if (tests_ == kMaxTraversalTests) {
      throw std::length_error("Sun-hours geometry is too dense for this study. Reduce the grid or time samples.");
    }
    ++tests_;
  }

  bool BlockedByNode(uint32_t index, const glm::dvec3& origin, const RayDirection& ray) {
    CountTest();
    const auto& node = nodes_[index];
    if (!Intersects(node.bounds, origin, ray.direction)) {
      return false;
    }
    if (node.count == 0) {
      return BlockedByNode(node.first, origin, ray) || BlockedByNode(node.right, origin, ray);
    }
    for (uint32_t i = node.first; i < node.first + node.count; ++i) {
      CountTest();
      if (Intersects(triangles_[indices_[i]].geometry, origin, ray)) {
        return true;
      }
    }
    return false;
  }

  std::stop_token stop_;
  std::vector<Triangle> triangles_;
  std::vector<uint32_t> indices_;
  std::vector<Node> nodes_;
  size_t tests_{};
};

bool IsValidGridAxis(double min, double max, uint32_t divisions) {
  const double extent = max - min;
  const double halfCell = extent / (2.0 * divisions);
  return std::isfinite(extent) && extent > 0.0 &&
         min + halfCell > min && max - halfCell < max;
}

} // namespace

void ValidateBimSunHoursSettings(const BimSunHoursSettings& settings) {
  auto time = settings.solar;
  SetMinute(time, 0);
  if (!validSolarTime(time) || !std::isfinite(settings.northDegrees)) {
    throw std::invalid_argument("Use a valid solar location, date, UTC offset and model north angle.");
  }
  if (settings.columns == 0 || settings.rows == 0 ||
      settings.columns > kMaxBimSunHoursGridDimension || settings.rows > kMaxBimSunHoursGridDimension) {
    throw std::length_error("Use 1 to " + std::to_string(kMaxBimSunHoursGridDimension) +
                            " rows and columns for the sun-hours grid.");
  }
  if (!IsValidGridAxis(settings.minX, settings.maxX, settings.columns) ||
      !IsValidGridAxis(settings.minZ, settings.maxZ, settings.rows) ||
      !std::isfinite(settings.elevation)) {
    throw std::invalid_argument("Use finite grid bounds with positive width and depth and a finite elevation.");
  }
  if (settings.startMinute >= settings.endMinute || settings.endMinute > kMinutesPerDay ||
      settings.stepMinutes == 0 || settings.stepMinutes > kMinutesPerDay) {
    throw std::invalid_argument("Choose an ordered time window in one local day and a positive sample interval.");
  }
  const size_t cells = size_t(settings.columns) * settings.rows;
  const size_t samples = SampleCount(settings);
  if (cells > kMaxBimSunHoursCells || samples > kMaxBimSunHoursSamples ||
      cells * samples > kMaxBimSunHoursRays) {
    throw std::length_error("Sun-hours study exceeds the ray limit. Reduce the grid or increase the sample interval.");
  }
}

glm::dvec3 BimSunHoursCellCenter(const BimSunHoursSettings& settings, uint32_t column,
                                uint32_t row) {
  if (column >= settings.columns || row >= settings.rows) {
    throw std::out_of_range("Sun-hours cell is outside the grid");
  }
  return {std::lerp(settings.minX, settings.maxX, (column + 0.5) / settings.columns),
          settings.elevation,
          std::lerp(settings.minZ, settings.maxZ, (row + 0.5) / settings.rows)};
}

glm::dvec3 BimSunHoursCellCenter(const BimSunHoursSettings& settings, size_t cellIndex) {
  if (settings.columns == 0 || cellIndex >= size_t(settings.columns) * settings.rows) {
    throw std::out_of_range("Sun-hours cell is outside the grid");
  }
  return BimSunHoursCellCenter(settings, static_cast<uint32_t>(cellIndex % settings.columns),
                               static_cast<uint32_t>(cellIndex / settings.columns));
}

glm::vec3 BimSunHoursColor(double hours, double maxHours) {
  constexpr std::array colors{glm::vec3{0.11f, 0.20f, 0.49f}, glm::vec3{0.08f, 0.48f, 0.74f},
                              glm::vec3{0.16f, 0.69f, 0.65f}, glm::vec3{0.90f, 0.77f, 0.22f},
                              glm::vec3{0.99f, 0.42f, 0.13f}};
  if (!(hours > 0.0) || !(maxHours > 0.0) || !std::isfinite(maxHours)) {
    return colors.front();
  }
  const double position = std::clamp(hours / maxHours, 0.0, 1.0) * (colors.size() - 1);
  const size_t index = std::min(size_t(position), colors.size() - 2);
  const auto blend = static_cast<float>(position - double(index));
  return colors[index] + (colors[index + 1] - colors[index]) * blend;
}

BimSunHoursResult RunBimSunHours(const BimSunHoursSettings& settings,
                                std::span<const BimSunTriangle> triangles, std::stop_token stop) {
  try {
    CheckStop(stop);
    ValidateBimSunHoursSettings(settings);
    if (triangles.size() > kMaxBimSunHoursTriangles) {
      throw std::length_error("BIM geometry exceeds the triangle limit for the sun-hours study.");
    }

    BimSunHoursResult result{.settings = settings};
    result.sampleCount = SampleCount(settings);
    const size_t cells = size_t(settings.columns) * settings.rows;
    result.hours.resize(cells);
    const glm::dvec3 center{std::midpoint(settings.minX, settings.maxX), settings.elevation,
                            std::midpoint(settings.minZ, settings.maxZ)};
    Occluders occluders(triangles, center, stop, result.skippedTriangles);

    // Rebase both rays and geometry before offsetting the horizontal receiver.
    // The offset suppresses its own surface without growing at distant sites.
    const double cellWidth = (settings.maxX - settings.minX) / settings.columns;
    const double cellDepth = (settings.maxZ - settings.minZ) / settings.rows;
    const double bias = std::clamp(64.0 * kRoundoff * std::min(cellWidth, cellDepth),
                                   64.0 * kRoundoff, 1e-9);
    std::vector<glm::dvec3> origins;
    if (!occluders.Empty()) {
      origins.reserve(cells);
      for (size_t i = 0; i < cells; ++i) {
        auto point = BimSunHoursCellCenter(settings, i) - center;
        point.y = bias;
        origins.push_back(point);
      }
    }

    // Accumulate integer minutes. Convert once so each cell's value agrees
    // exactly with the sampled daylight limit when it stays in the sun.
    uint32_t daylightMinutes = 0;
    for (auto start = settings.startMinute; start < settings.endMinute;) {
      CheckStop(stop);
      const auto end = std::min(start + settings.stepMinutes, settings.endMinute);
      const auto duration = end - start;
      const auto direction = SampleDirection(settings, start + end);
      start = end;
      if (direction.y <= 0.0) {
        continue;
      }
      ++result.daylightSampleCount;
      daylightMinutes += duration;
      const RayDirection ray(direction);
      for (size_t i = 0; i < origins.size(); ++i) {
        if (i % 256 == 0) {
          CheckStop(stop);
        }
        if (!occluders.Blocked(origins[i], ray)) {
          result.hours[i] += duration;
        }
      }
    }
    result.daylightHours = daylightMinutes / 60.0;
    if (origins.empty()) {
      std::ranges::fill(result.hours, result.daylightHours);
    } else {
      for (auto& hours : result.hours) {
        hours /= 60.0;
      }
    }
    CheckStop(stop);
    return result;
  } catch (const Cancelled&) {
    return {.settings = settings, .cancelled = true};
  }
}

} // namespace container::renderer
