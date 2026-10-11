#include "Container/renderer/lighting/DdgiVolumePlacement.h"
#include "Container/renderer/lighting/DdgiVolumeFit.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace container::renderer {
namespace {

DdgiResourceLimits limits(uint64_t budget) {
  return {std::numeric_limits<uint32_t>::max(),
          std::numeric_limits<uint64_t>::max(), budget};
}

float outward(double value, bool lower) {
  if (!std::isfinite(value) ||
      std::abs(value) > std::numeric_limits<float>::max())
    throw std::invalid_argument(
        "DDGI detail placement padding exceeds float coordinates; rebase the selection");
  float result = float(value);
  if ((lower && double(result) > value) ||
      (!lower && double(result) < value))
    result = std::nextafter(result, lower ? -std::numeric_limits<float>::infinity()
                                         : std::numeric_limits<float>::infinity());
  return result;
}

} // namespace

DdgiDetailVolumePlacementResult
fitDdgiDetailVolume(const DdgiSettings &base, const DdgiSceneBounds &bounds,
                   DdgiDetailVolumePlacementOptions options) {
  if (!bounds.valid)
    throw std::invalid_argument(
        "DDGI detail placement needs current triangle bounds; select a room or rendered element");
  double maximumExtent = 0;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(bounds.minimum[axis]) ||
        !std::isfinite(bounds.maximum[axis]) ||
        bounds.minimum[axis] > bounds.maximum[axis])
      throw std::invalid_argument(
          "DDGI detail placement bounds must be finite and ordered");
    maximumExtent = std::max(maximumExtent,
                            double(bounds.maximum[axis]) - bounds.minimum[axis]);
  }
  validateDdgiSettings(base);
  if (base.additionalVolumes.size() + 1 >= kMaxDdgiVolumes)
    throw std::invalid_argument(
        "DDGI detail placement already has four volumes; remove a detail volume first");
  const DdgiVolumeSet previous(base);
  if (options.maxTotalProbes <= previous.probeCount())
    throw std::invalid_argument(
        "DDGI detail placement has no remaining maxTotalProbes budget");
  const uint32_t cap = std::min(options.maxDetailProbes,
                              options.maxTotalProbes - previous.probeCount());
  const double paddingCells = std::max(1., double(base.volumeBlendCells));
  // c nodes provide c-1 intervals. Require positive room for a receiver
  // between both padding bands, even with point/flat selection bounds.
  const auto minimumAxis = uint32_t(std::floor(2 * paddingCells)) + 2;
  const uint64_t minimumCount = uint64_t(minimumAxis) * minimumAxis * minimumAxis;
  if (minimumCount > cap)
    throw std::invalid_argument(
        "DDGI detail placement needs more maxDetailProbes/maxTotalProbes for its blend-band padding");

  // Put the candidate in the fitter's primary slot. Its existing details
  // reserve all old lattices, so the fitter sees the exact combined payload.
  DdgiSettings fitting = base;
  fitting.additionalVolumes.insert(fitting.additionalVolumes.begin(), base.volume);
  fitting.volume = {{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, glm::uvec3(minimumAxis)};
  try {
    (void)planDdgiVolumeResources(fitting, limits(base.payloadBudgetBytes));
  } catch (const std::invalid_argument &error) {
    throw std::invalid_argument(
        std::string("DDGI detail placement cannot fit padding in the remaining shared payload budget: ") +
        error.what());
  }

  uint32_t cubeAxis = minimumAxis;
  while (uint64_t(cubeAxis + 1) * (cubeAxis + 1) * (cubeAxis + 1) <= cap)
    ++cubeAxis;
  double paddingHint = std::max(.001,
      maximumExtent / (double(cubeAxis) - 1 - 2 * paddingCells));
  // Extra padding uses a spacing upper bound. If payload constraints require
  // a coarser lattice, increase that bound and refit. This is bounded CPU work;
  // incompatible large-coordinate/trace-distance fits fail actionably.
  for (uint32_t attempt = 0; attempt < 64; ++attempt) {
    glm::vec3 minimum, maximum;
    const double extra = (paddingCells - 1) * paddingHint;
    for (int axis = 0; axis < 3; ++axis) {
      minimum[axis] = std::nextafter(
          outward(double(bounds.minimum[axis]) - extra, true),
          -std::numeric_limits<float>::infinity());
      maximum[axis] = std::nextafter(
          outward(double(bounds.maximum[axis]) + extra, false),
          std::numeric_limits<float>::infinity());
    }
    const auto fit = fitDdgiVolume(fitting, minimum, maximum, {cap});
    const double required = paddingCells * fit.settings.volume.spacing.x;
    bool padded = true;
    for (int axis = 0; axis < 3; ++axis)
      padded &= double(bounds.minimum[axis]) - fit.boundsMin[axis] >= required &&
                double(fit.boundsMax[axis]) - bounds.maximum[axis] >= required;
    if (!padded) {
      // Round above the fitted cell instead of converging from below forever
      // at a count breakpoint. Flat-axis quantization also gets this margin.
      paddingHint = std::max(paddingHint * 1.0625,
                             double(fit.settings.volume.spacing.x) * 1.0625);
      // The outward precision sliver makes point/flat axes nondegenerate,
      // retaining the fitter's full outside-cell guarantee.
      if (paddingCells == 1)
        throw std::invalid_argument(
            "DDGI detail placement cannot resolve outside padding at these coordinates; rebase the selection");
      continue;
    }
    DdgiDetailVolumePlacementResult result;
    result.settings = base;
    result.settings.additionalVolumes.push_back(fit.settings.volume);
    result.settings.maxDistance = std::max(base.maxDistance, fit.settings.maxDistance);
    validateDdgiSettings(result.settings);
    const auto resourcePlan = planDdgiVolumeResources(
        result.settings, limits(base.payloadBudgetBytes));
    result.volumeIndex = uint32_t(result.settings.additionalVolumes.size());
    result.addedProbeCount = fit.probeCount;
    result.totalProbeCount = previous.probeCount() + fit.probeCount;
    result.boundsMin = fit.boundsMin;
    result.boundsMax = fit.boundsMax;
    result.payloadBytes = resourcePlan.payloadBytes;
    return result;
  }
  throw std::invalid_argument(
      "DDGI detail placement cannot fit blend-band padding; increase probe/payload budgets or reduce the blend band");
}

} // namespace container::renderer
