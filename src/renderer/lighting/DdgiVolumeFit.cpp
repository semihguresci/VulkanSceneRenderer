#include "Container/renderer/lighting/DdgiVolumeFit.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace container::renderer {
namespace {

constexpr double kMinimumCell = 1e-3;

double floatPrecision(double magnitude) {
  if (!std::isfinite(magnitude) ||
      magnitude > std::numeric_limits<float>::max())
    throw std::invalid_argument(
        "DDGI scene-fit padding exceeds float coordinates; rebase the scene");
  float rounded = float(magnitude);
  if (double(rounded) < magnitude)
    rounded = std::nextafter(rounded, std::numeric_limits<float>::infinity());
  const float next =
      std::nextafter(rounded, std::numeric_limits<float>::infinity());
  return std::isfinite(next) ? double(next) - rounded
                             : double(rounded) - std::nextafter(rounded, 0.f);
}

float roundUp(double value) {
  float result = float(value);
  if (double(result) < value)
    result = std::nextafter(result, std::numeric_limits<float>::infinity());
  return result;
}

DdgiResourceLimits payloadLimits(uint64_t budget) {
  return {std::numeric_limits<uint32_t>::max(),
          std::numeric_limits<uint64_t>::max(), budget};
}

struct FitCandidate {
  DdgiProbeVolumeDesc volume;
  uint32_t probeCount;
  uint64_t payloadBytes;
};

} // namespace

DdgiVolumeFitResult fitDdgiVolume(const DdgiSettings &current,
                                  glm::vec3 minimum, glm::vec3 maximum,
                                  DdgiVolumeFitOptions options) {
  std::array<double, 3> extent{};
  glm::uvec3 minimumCounts{3u};
  double maximumExtent = 0, maximumMagnitude = 0, sceneDiagonalSquared = 0;
  uint32_t minimumProbeCount = 1;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
        minimum[axis] > maximum[axis])
      throw std::invalid_argument(
          "DDGI scene-fit bounds must be finite and ordered on every axis");
    extent[axis] = double(maximum[axis]) - minimum[axis];
    if (extent[axis] > 0)
      minimumCounts[axis] = 4;
    minimumProbeCount *= minimumCounts[axis];
    maximumExtent = std::max(maximumExtent, extent[axis]);
    maximumMagnitude =
        std::max({maximumMagnitude, std::abs(double(minimum[axis])),
                  std::abs(double(maximum[axis]))});
    sceneDiagonalSquared += extent[axis] * extent[axis];
  }
  if (options.maxProbes < minimumProbeCount)
    throw std::invalid_argument(
        "DDGI scene fit needs at least " + std::to_string(minimumProbeCount) +
        " probes for outside padding; increase maxProbes");
  if (std::sqrt(sceneDiagonalSquared) >= kMaxDdgiTraceDistance)
    throw std::invalid_argument(
        "DDGI scene-fit bounds leave no padding within the 1000000 trace "
        "distance limit; fit smaller bounds or rescale the scene");

  // Validate preserved controls against the smallest possible fitted lattice.
  // The previous lattice and scale may be invalid for a newly lowered memory
  // budget or a newly loaded scene; neither should prevent a valid new fit.
  DdgiSettings preserved = current;
  preserved.volume = {{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, minimumCounts};
  preserved.maxDistance = 20.f;
  preserved.normalBias = .03f;
  preserved.viewBias = .01f;
  preserved.visibilityBias = .05f;
  preserved.debugProbeRadius = .06f;
  try {
    validateDdgiSettings(preserved);
  } catch (const std::invalid_argument &error) {
    throw std::invalid_argument(
        std::string("DDGI scene fit cannot preserve the configured controls "
                    "even at the minimum padded lattice: ") +
        error.what());
  }

  // A fixed power-of-two grid makes all node coordinates representable as
  // float and keeps the count/payload predicate monotonic during the search.
  // Its precision covers even the coarse bracket's origin and far endpoint.
  double coarseSpacing = std::max(kMinimumCell, 2 * maximumExtent);
  double precision = 0;
  for (;;) {
    precision = floatPrecision(maximumMagnitude + 4 * coarseSpacing);
    if (precision > kMaxDdgiTraceDistance)
      throw std::invalid_argument(
          "DDGI scene coordinates cannot resolve a cell within the trace "
          "distance limit; rebase the scene near the origin");
    const double nextCoarse = std::max(coarseSpacing, 8 * precision);
    if (nextCoarse == coarseSpacing)
      break;
    coarseSpacing = nextCoarse;
  }

  std::array<int64_t, 3> firstUnits{};
  std::array<uint64_t, 3> widthUnits{};
  for (int axis = 0; axis < 3; ++axis) {
    if (extent[axis] == 0) {
      firstUnits[axis] = int64_t(std::round(double(minimum[axis]) / precision));
    } else {
      firstUnits[axis] = int64_t(std::floor(double(minimum[axis]) / precision));
      const auto last = int64_t(std::ceil(double(maximum[axis]) / precision));
      widthUnits[axis] = uint64_t(last - firstUnits[axis]);
    }
  }

  const auto candidate =
      [&](uint64_t cellUnits) -> std::optional<FitCandidate> {
    DdgiProbeVolumeDesc description;
    description.spacing = glm::vec3(float(double(cellUnits) * precision));
    uint64_t count = 1;
    for (int axis = 0; axis < 3; ++axis) {
      const uint64_t intervals =
          widthUnits[axis] / cellUnits + (widthUnits[axis] % cellUnits != 0);
      const uint64_t axisCount = intervals + 3;
      if (axisCount > options.maxProbes ||
          count > options.maxProbes / axisCount)
        return std::nullopt;
      count *= axisCount;
      description.counts[axis] = uint32_t(axisCount);
      description.origin[axis] =
          float(double(firstUnits[axis] - int64_t(cellUnits)) * precision);
    }
    try {
      auto packed = current;
      packed.volume = description;
      const auto plan = planDdgiVolumeResources(
          packed, payloadLimits(current.payloadBudgetBytes));
      return FitCandidate{description, uint32_t(count), plan.payloadBytes};
    } catch (const std::invalid_argument &) {
      // Minimum-lattice validation already checked the preserved controls;
      // larger candidates may exceed atlas arithmetic or the payload ceiling.
      return std::nullopt;
    }
  };

  uint64_t low =
      uint64_t(std::ceil(std::max(kMinimumCell, precision) / precision));
  uint64_t high = uint64_t(std::ceil(coarseSpacing / precision));
  if (!candidate(high))
    throw std::invalid_argument(
        "DDGI scene fit cannot satisfy the padded lattice; increase maxProbes "
        "or the payload budget, or reduce atlas resolution/frame budgets");
  while (low < high) {
    const uint64_t middle = low + (high - low) / 2;
    if (candidate(middle))
      high = middle;
    else
      low = middle + 1;
  }
  const auto distanceFailure = [] {
    return std::invalid_argument(
        "DDGI fitted padding exceeds the 1000000 trace distance limit; "
        "increase maxProbes/payload budget or fit smaller bounds");
  };
  FitCandidate fitted = *candidate(low);
  double diagonal = 0;
  for (;;) {
    const double cell = double(low) * precision;
    double lowerBoundSquared = 0;
    for (const uint64_t width : widthUnits) {
      const double length = double(width) * precision + 2 * cell;
      lowerBoundSquared += length * length;
    }
    // Every future lattice spans at least the quantized scene width plus two
    // cells on each axis. This bound increases with spacing, so it also rules
    // out all later topologies without a potentially long breakpoint scan.
    if (std::sqrt(lowerBoundSquared) > kMaxDdgiTraceDistance)
      throw distanceFailure();

    const DdgiProbeVolume volume(fitted.volume);
    double diagonalSquared = 0;
    for (int axis = 0; axis < 3; ++axis) {
      const double length =
          double(volume.maximum()[axis]) - volume.minimum()[axis];
      diagonalSquared += length * length;
    }
    diagonal = std::sqrt(diagonalSquared);
    if (diagonal <= kMaxDdgiTraceDistance)
      break;

    // The padded diagonal grows within a fixed count topology, then drops
    // when ceil(width/cell) decreases. Keep the monotonic count/payload search
    // above and check only these next useful thresholds for distance support.
    uint64_t nextUnits = std::numeric_limits<uint64_t>::max();
    for (const uint64_t width : widthUnits) {
      const uint64_t intervals = width / low + (width % low != 0);
      if (intervals > 1) {
        const uint64_t remaining = intervals - 1;
        const uint64_t threshold = width / remaining + (width % remaining != 0);
        nextUnits = std::min(nextUnits, threshold);
      }
    }
    if (nextUnits == std::numeric_limits<uint64_t>::max())
      throw distanceFailure();
    low = nextUnits;
    // Counts only decrease, preserving the already satisfied payload/cap.
    fitted = *candidate(low);
  }
  const DdgiProbeVolume volume(fitted.volume);

  DdgiVolumeFitResult result;
  result.settings = current;
  result.settings.volume = fitted.volume;
  result.settings.maxDistance = roundUp(diagonal);
  const float cell = fitted.volume.spacing.x;
  result.settings.normalBias = .06f * cell;
  result.settings.viewBias = .02f * cell;
  result.settings.visibilityBias = .1f * cell;
  result.settings.debugProbeRadius = .12f * cell;
  validateDdgiSettings(result.settings);
  result.boundsMin = volume.minimum();
  result.boundsMax = volume.maximum();
  result.probeCount = fitted.probeCount;
  result.payloadBytes = fitted.payloadBytes;
  return result;
}

} // namespace container::renderer
