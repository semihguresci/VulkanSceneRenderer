#include "Container/renderer/lighting/DdgiVolumeScroll.h"

#include <cmath>
#include <stdexcept>

namespace container::renderer {

DdgiVolumeScrollPlan
planDdgiVolumeScroll(const DdgiProbeVolumeDesc &previous,
                     glm::vec3 cameraWorldPosition, uint32_t maxProbes) {
  const DdgiProbeVolume oldVolume(previous);
  if (oldVolume.probeCount() > maxProbes)
    throw std::invalid_argument(
        "DDGI scrolling exceeds maxProbes CPU-work budget; reduce the lattice counts");
  DdgiVolumeScrollPlan result;
  result.previous = previous;
  result.target = previous;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(cameraWorldPosition[axis]))
      throw std::invalid_argument("DDGI scrolling camera position must be finite");
    const double center = double(previous.origin[axis]) +
                          .5 * previous.spacing[axis] * (previous.counts[axis] - 1);
    const double shift = std::round((double(cameraWorldPosition[axis]) - center) /
                                   previous.spacing[axis]);
    // Double cannot represent INT64_MAX exactly; use the exclusive upper
    // endpoint to avoid undefined conversion at +2^63.
    constexpr double exclusiveMaximum = 9223372036854775808.;
    if (!std::isfinite(shift) || shift < -exclusiveMaximum ||
        shift >= exclusiveMaximum)
      throw std::invalid_argument(
          "DDGI scrolling camera cell shift exceeds int64; rebase the scene");
    result.cellShift[axis] = int64_t(shift);
    const double origin = double(previous.origin[axis]) + shift * previous.spacing[axis];
    if (!std::isfinite(origin) || std::abs(origin) > std::numeric_limits<float>::max())
      throw std::invalid_argument("DDGI scrolling target origin exceeds float coordinates");
    result.target.origin[axis] = float(origin);
    result.changed |= result.cellShift[axis] != 0;
  }
  const DdgiProbeVolume newVolume(result.target);
  result.previousIndexForNewProbe.assign(oldVolume.probeCount(), kDdgiNoPreviousProbe);
  result.exposedProbeIndices.reserve(oldVolume.probeCount());
  uint32_t preserved = 0;
  bool disjoint = false;
  for (int axis = 0; axis < 3; ++axis)
    disjoint |= result.cellShift[axis] <= -int64_t(previous.counts[axis]) ||
                result.cellShift[axis] >= int64_t(previous.counts[axis]);
  for (uint32_t index = 0; index < newVolume.probeCount(); ++index) {
    const auto coordinate = newVolume.coordinates(index);
    glm::uvec3 oldCoordinate{};
    bool overlaps = !disjoint;
    if (overlaps) {
      for (int axis = 0; axis < 3; ++axis) {
        // The prior disjoint check bounds the shift by a uint32 count, so
        // adding a coordinate here cannot overflow int64.
        const int64_t old = int64_t(coordinate[axis]) + result.cellShift[axis];
        overlaps &= old >= 0 && old < previous.counts[axis];
        if (old >= 0 && old < previous.counts[axis])
          oldCoordinate[axis] = uint32_t(old);
      }
    }
    if (overlaps) {
      const uint32_t previousIndex = oldVolume.linearIndex(oldCoordinate);
      if (oldVolume.probePosition(previousIndex) == newVolume.probePosition(index)) {
        result.previousIndexForNewProbe[index] = previousIndex;
        ++preserved;
        continue;
      }
    }
    result.exposedProbeIndices.push_back(index);
  }
  result.clearsHistory = preserved == 0;
  return result;
}

} // namespace container::renderer
