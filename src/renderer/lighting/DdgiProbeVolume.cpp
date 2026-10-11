#include "Container/renderer/lighting/DdgiProbeVolume.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace container::renderer {
namespace {

bool finite(glm::vec3 value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[noreturn]] void invalid(const char *message) {
  throw std::invalid_argument(message);
}

uint64_t multiply(uint64_t a, uint64_t b, const char *message) {
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
    invalid(message);
  return a * b;
}

uint64_t add(uint64_t a, uint64_t b, const char *message) {
  if (b > std::numeric_limits<uint64_t>::max() - a)
    invalid(message);
  return a + b;
}

uint32_t ceilSquareRoot(uint32_t value) {
  uint32_t low = 1, high = 65536;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    if (uint64_t(middle) * middle >= value)
      high = middle;
    else
      low = middle + 1;
  }
  return low;
}

DdgiAtlasPlan atlas(uint32_t count, uint32_t interior, uint32_t dimension,
                    const char *name) {
  if (interior < 2 || interior > std::numeric_limits<uint32_t>::max() - 2)
    throw std::invalid_argument(
        std::string("DDGI ") + name +
        " interior texels must be in [2, UINT32_MAX-2]");
  DdgiAtlasPlan result;
  result.interiorTexels = interior;
  result.tileTexels = interior + 2 * DdgiResourcePlan::borderTexels;
  const uint32_t capacityPerAxis = dimension / result.tileTexels;
  if (capacityPerAxis == 0 ||
      uint64_t(capacityPerAxis) * capacityPerAxis < count)
    throw std::invalid_argument(std::string("DDGI ") + name +
                                " atlas exceeds maxImageDimension2D; reduce "
                                "probe counts/tile resolution or raise limit");
  result.tilesPerRow = std::min(ceilSquareRoot(count), capacityPerAxis);
  result.tileRows =
      uint32_t((uint64_t(count) + result.tilesPerRow - 1) / result.tilesPerRow);
  result.width = result.tilesPerRow * result.tileTexels;
  result.height = result.tileRows * result.tileTexels;
  result.bytesPerHistory = multiply(
      multiply(result.width, result.height, "DDGI atlas texel count overflow"),
      8, "DDGI atlas payload overflow");
  return result;
}

void validateBatch(const DdgiUpdateBatch &batch) {
  if (batch.volumeProbeCount == 0 ||
      batch.firstProbe >= batch.volumeProbeCount ||
      batch.probeCount > batch.volumeProbeCount || batch.raysPerProbe == 0 ||
      uint64_t(batch.probeCount) * batch.raysPerProbe != batch.rayCount ||
      batch.nextProbe != (uint64_t(batch.firstProbe) + batch.probeCount) %
                             batch.volumeProbeCount)
    invalid("DDGI update batch is malformed; use planDdgiUpdates");
}

} // namespace

DdgiProbeVolume::DdgiProbeVolume(const DdgiProbeVolumeDesc &desc)
    : desc_(desc) {
  if (!finite(desc.origin))
    invalid("DDGI volume origin must be finite");
  if (!finite(desc.spacing) || desc.spacing.x <= 0.f || desc.spacing.y <= 0.f ||
      desc.spacing.z <= 0.f)
    invalid("DDGI volume spacing must be finite and positive on every axis");
  if (desc.counts.x == 0 || desc.counts.y == 0 || desc.counts.z == 0)
    invalid("DDGI volume counts must be nonzero on every axis");
  uint64_t count = desc.counts.x;
  for (int axis = 1; axis < 3; ++axis) {
    if (count > std::numeric_limits<uint32_t>::max() / desc.counts[axis])
      invalid(
          "DDGI volume probe count exceeds uint32 GPU indexing; reduce counts");
    count *= desc.counts[axis];
  }
  probeCount_ = uint32_t(count);
  for (int axis = 0; axis < 3; ++axis) {
    const double end = double(desc.origin[axis]) +
                       double(desc.spacing[axis]) * (desc.counts[axis] - 1);
    if (!std::isfinite(end) || end > std::numeric_limits<float>::max() ||
        end < -std::numeric_limits<float>::max())
      invalid("DDGI volume world-space bounds exceed finite float coordinates");
    maximum_[axis] = float(end);
    if (desc.counts[axis] > 1) {
      // Conservatively require spacing large enough for distinct float world
      // positions throughout the lattice, including large-coordinate volumes.
      const float magnitude =
          std::max(std::abs(desc.origin[axis]), std::abs(maximum_[axis]));
      const float next =
          std::nextafter(magnitude, std::numeric_limits<float>::infinity());
      const double ulp =
          std::isfinite(next)
              ? double(next) - magnitude
              : double(magnitude) - std::nextafter(magnitude, 0.f);
      if (double(desc.spacing[axis]) < ulp)
        invalid("DDGI spacing cannot resolve distinct float probe positions; "
                "increase spacing or rebase the volume origin");
    }
  }
}

uint32_t DdgiProbeVolume::linearIndex(glm::uvec3 value) const {
  if (value.x >= desc_.counts.x || value.y >= desc_.counts.y ||
      value.z >= desc_.counts.z)
    throw std::out_of_range("DDGI probe coordinates exceed volume counts");
  return uint32_t(uint64_t(value.x) +
                  uint64_t(desc_.counts.x) *
                      (uint64_t(value.y) + uint64_t(desc_.counts.y) * value.z));
}

glm::uvec3 DdgiProbeVolume::coordinates(uint32_t index) const {
  if (index >= probeCount_)
    throw std::out_of_range(
        "DDGI linear probe index exceeds volume probe count");
  const uint32_t x = index % desc_.counts.x;
  index /= desc_.counts.x;
  return {x, index % desc_.counts.y, index / desc_.counts.y};
}

glm::vec3 DdgiProbeVolume::probePosition(uint32_t index) const {
  const auto coordinate = coordinates(index);
  glm::vec3 result;
  for (int axis = 0; axis < 3; ++axis)
    result[axis] = float(double(desc_.origin[axis]) +
                         double(desc_.spacing[axis]) * coordinate[axis]);
  return result;
}

bool DdgiProbeVolume::contains(glm::vec3 position) const noexcept {
  if (!finite(position))
    return false;
  for (int axis = 0; axis < 3; ++axis)
    if (position[axis] < desc_.origin[axis] || position[axis] > maximum_[axis])
      return false;
  return true;
}

std::optional<DdgiTrilinearSample>
DdgiProbeVolume::interpolate(glm::vec3 position,
                             DdgiOutsideVolume outside) const {
  if (!finite(position))
    invalid("DDGI interpolation position must be finite");
  if (outside != DdgiOutsideVolume::Reject &&
      outside != DdgiOutsideVolume::Clamp)
    invalid("DDGI interpolation outside-volume policy is invalid");
  const bool inside = contains(position);
  if (!inside && outside == DdgiOutsideVolume::Reject)
    return std::nullopt;
  glm::uvec3 lower{}, upper{};
  std::array<double, 3> fraction{};
  for (int axis = 0; axis < 3; ++axis) {
    const double clamped =
        std::clamp(double(position[axis]), double(desc_.origin[axis]),
                   double(maximum_[axis]));
    // Rounded float endpoints still select the endpoint probe exactly.
    const double grid =
        clamped >= maximum_[axis]
            ? double(desc_.counts[axis] - 1)
            : std::clamp((clamped - desc_.origin[axis]) / desc_.spacing[axis],
                         0.0, double(desc_.counts[axis] - 1));
    lower[axis] = uint32_t(std::floor(grid));
    const auto worldCoordinate = [&](uint32_t coordinate) {
      return double(float(double(desc_.origin[axis]) +
                          double(desc_.spacing[axis]) * coordinate));
    };
    // Match probePosition's actual float centers. At large origins, rounding
    // can shift a center by part of a spacing interval. The validated ULP
    // bound limits the ideal-cell estimate to one neighboring cell.
    if (lower[axis] > 0 && clamped < worldCoordinate(lower[axis]))
      --lower[axis];
    upper[axis] =
        lower[axis] == desc_.counts[axis] - 1 ? lower[axis] : lower[axis] + 1;
    if (upper[axis] < desc_.counts[axis] - 1 &&
        clamped > worldCoordinate(upper[axis])) {
      lower[axis] = upper[axis];
      ++upper[axis];
    }
    fraction[axis] = upper[axis] == lower[axis]
                         ? 0.0
                         : std::clamp((clamped - worldCoordinate(lower[axis])) /
                                          (worldCoordinate(upper[axis]) -
                                           worldCoordinate(lower[axis])),
                                      0.0, 1.0);
  }
  DdgiTrilinearSample result;
  result.clamped = !inside;
  for (uint32_t corner = 0; corner < 8; ++corner) {
    glm::uvec3 coordinate;
    double weight = 1;
    for (int axis = 0; axis < 3; ++axis) {
      const bool high = (corner & (1u << axis)) != 0;
      coordinate[axis] = high ? upper[axis] : lower[axis];
      weight *= high ? fraction[axis] : 1 - fraction[axis];
    }
    result.probeIndices[corner] = linearIndex(coordinate);
    result.weights[corner] = float(weight);
  }
  return result;
}

DdgiUpdateBatch planDdgiUpdates(const DdgiProbeVolume &volume,
                                const DdgiUpdateBudget &budget,
                                uint32_t firstProbe) {
  if (budget.raysPerProbe == 0)
    invalid(
        "DDGI raysPerProbe must be positive; disable with a zero frame budget");
  if (firstProbe >= volume.probeCount())
    throw std::out_of_range("DDGI update cursor exceeds volume probe count");
  DdgiUpdateBatch result;
  result.volumeProbeCount = volume.probeCount();
  result.firstProbe = firstProbe;
  result.raysPerProbe = budget.raysPerProbe;
  result.probeCount = std::min({volume.probeCount(), budget.maxProbesPerFrame,
                                budget.maxRaysPerFrame / budget.raysPerProbe});
  result.rayCount = result.probeCount * budget.raysPerProbe;
  result.nextProbe = uint32_t((uint64_t(firstProbe) + result.probeCount) %
                              volume.probeCount());
  return result;
}

void writeDdgiProbeUpdates(const DdgiUpdateBatch &batch,
                           std::span<DdgiProbeUpdate> destination) {
  validateBatch(batch);
  if (destination.size() < batch.probeCount)
    throw std::out_of_range(
        "DDGI update destination is smaller than planned probe count");
  for (uint32_t slot = 0; slot < batch.probeCount; ++slot)
    destination[slot] = {
        uint32_t((uint64_t(batch.firstProbe) + slot) % batch.volumeProbeCount),
        slot * batch.raysPerProbe, batch.raysPerProbe, 0};
}

DdgiResourcePlan planDdgiResources(const DdgiProbeVolume &volume,
                                   const DdgiResourceSettings &settings,
                                   const DdgiResourceLimits &limits) {
  return planDdgiResources(volume.probeCount(), settings, limits);
}

DdgiResourcePlan planDdgiResources(uint32_t probeCount,
                                   const DdgiResourceSettings &settings,
                                   const DdgiResourceLimits &limits) {
  if (probeCount == 0)
    invalid("DDGI packed probe count must be positive");
  if (settings.updates.raysPerProbe == 0)
    invalid("DDGI raysPerProbe must be positive");
  if (limits.maxImageDimension2D == 0)
    invalid("DDGI maxImageDimension2D must be positive");
  DdgiResourcePlan result;
  result.irradiance =
      atlas(probeCount, settings.irradianceInteriorTexels,
            limits.maxImageDimension2D, "irradiance");
  result.visibility =
      atlas(probeCount, settings.visibilityInteriorTexels,
            limits.maxImageDimension2D, "visibility");
  auto &capacity = result.updateCapacity;
  capacity.volumeProbeCount = probeCount;
  capacity.raysPerProbe = settings.updates.raysPerProbe;
  capacity.probeCount = std::min({probeCount, settings.updates.maxProbesPerFrame,
      settings.updates.maxRaysPerFrame / settings.updates.raysPerProbe});
  capacity.rayCount = capacity.probeCount * capacity.raysPerProbe;
  capacity.nextProbe = capacity.probeCount % probeCount;
  result.probeStateBytes = multiply(probeCount, sizeof(DdgiProbeState),
                                    "DDGI probe state buffer overflow");
  result.probePositionBytes =
      multiply(probeCount, 16, "DDGI probe position buffer overflow");
  result.rayScratchBytes =
      multiply(result.updateCapacity.rayCount, sizeof(DdgiRaySample),
               "DDGI ray scratch overflow");
  result.updateRecordBytes =
      multiply(result.updateCapacity.probeCount, sizeof(DdgiProbeUpdate),
               "DDGI update buffer overflow");
  for (const auto [bytes, name] :
       {std::pair{result.probeStateBytes, "probe state"},
        std::pair{result.probePositionBytes, "probe position"},
        std::pair{result.rayScratchBytes, "ray scratch"},
        std::pair{result.updateRecordBytes, "update record"}})
    if (bytes > limits.maxBufferBytes)
      throw std::invalid_argument(
          std::string("DDGI ") + name +
          " buffer exceeds maxBufferBytes; reduce volume/update budget");
  const uint64_t historyBytes = multiply(
      add(result.irradiance.bytesPerHistory, result.visibility.bytesPerHistory,
          "DDGI combined atlas payload overflow"),
      DdgiResourcePlan::historyCopies, "DDGI history atlas payload overflow");
  result.payloadBytes =
      add(historyBytes, result.probeStateBytes, "DDGI total payload overflow");
  result.payloadBytes = add(result.payloadBytes, result.rayScratchBytes,
                            "DDGI total payload overflow");
  result.payloadBytes = add(result.payloadBytes, result.updateRecordBytes,
                            "DDGI total payload overflow");
  result.payloadBytes = add(result.payloadBytes, result.probePositionBytes,
                            "DDGI total payload overflow");
  if (result.payloadBytes > limits.maxPayloadBytes)
    invalid("DDGI payload exceeds maxPayloadBytes; reduce probe count, tile "
            "resolution or frame update budget");
  return result;
}

} // namespace container::renderer
