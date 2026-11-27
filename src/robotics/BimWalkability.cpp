#include "Container/robotics/BimWalkability.h"

#include "Container/robotics/BimCollisionGeometry.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace container::robotics {
namespace {

bool finite(glm::dvec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double minimumUpNormal(double degrees) {
  if (!std::isfinite(degrees) || degrees < 0 || degrees >= 90)
    throw std::invalid_argument("Walkability slope must be in [0,90) degrees");
  return std::cos(degrees * std::numbers::pi / 180.0);
}

void positive(double value, const char* name) {
  if (!std::isfinite(value) || value <= 0)
    throw std::invalid_argument(std::string("Walkability ") + name + " must be finite and positive");
}

void nonnegative(double value, const char* name) {
  if (!std::isfinite(value) || value < 0)
    throw std::invalid_argument(std::string("Walkability ") + name + " must be finite and nonnegative");
}

void validate(const BimWalkabilitySettings& settings) {
  positive(settings.radius, "radius");
  positive(settings.height, "height");
  if (settings.height < 2 * settings.radius)
    throw std::invalid_argument("Walkability capsule height must include both hemispheres");
  (void)minimumUpNormal(settings.maxSlopeDegrees);
  nonnegative(settings.maxStepHeight, "step height");
  nonnegative(settings.maxDropHeight, "drop height");
  positive(settings.sampleSpacing, "sample spacing");
  positive(settings.skin, "skin");
  nonnegative(settings.elevationTolerance, "elevation tolerance");
  if (!settings.maxSamples)
    throw std::invalid_argument("Walkability sample limit must be positive");
}

void validateHit(const BimWalkabilityHit& hit) {
  if (!std::isfinite(hit.fraction) || hit.fraction < 0 || hit.fraction > 1 ||
      !finite(hit.position) || !finite(hit.normal))
    throw std::invalid_argument("Walkability query returned an invalid hit");
}

std::optional<BimWalkabilityHit> checked(std::optional<BimWalkabilityHit> hit) {
  if (hit)
    validateHit(*hit);
  return hit;
}

} // namespace

glm::dvec3 walkabilityCapsuleBottom(glm::dvec3 feet, double normalUp,
                                    const BimWalkabilitySettings& settings) {
  // Distance from the lower sphere centre to a sloped plane is n.z times
  // vertical separation. This preserves radius clearance on the uphill side.
  return feet + glm::dvec3(0, 0,
      (settings.radius + settings.skin) / normalUp - settings.radius);
}

std::vector<BimWalkableSurface> extractWalkableSurfaces(
    const CookedBimCollision& geometry, double maxSlopeDegrees) {
  const double minimumUp = minimumUpNormal(maxSlopeDegrees);
  std::vector<BimWalkableSurface> surfaces;
  for (const auto& collider : geometry.colliders) {
    if (!collider.supportCandidate)
      continue;
    if (!collider.mesh || collider.entityId.empty() || collider.representationId.empty() ||
        !finite(collider.originMeters) || collider.mesh->vertices.empty() ||
        collider.mesh->indices.empty() || collider.mesh->indices.size() % 3 != 0)
      throw std::invalid_argument("Malformed walkability support mesh");
    const auto& mesh = *collider.mesh;
    if (mesh.indices.size() / 3 > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Walkability triangle ordinal exceeds its supported range");
    for (size_t offset = 0; offset < mesh.indices.size(); offset += 3) {
      BimWalkableSurface surface;
      surface.entityId = collider.entityId;
      surface.representationId = collider.representationId;
      surface.triangleIndex = static_cast<uint32_t>(offset / 3);
      for (size_t corner = 0; corner < 3; ++corner) {
        const auto index = mesh.indices[offset + corner];
        if (index >= mesh.vertices.size())
          throw std::invalid_argument("Walkability support index exceeds its vertex array");
        surface.vertices[corner] = collider.originMeters + glm::dvec3(mesh.vertices[index]);
        if (!finite(surface.vertices[corner]))
          throw std::invalid_argument("Walkability support has nonfinite vertices");
      }
      const auto normal = glm::cross(surface.vertices[1] - surface.vertices[0],
                                     surface.vertices[2] - surface.vertices[0]);
      const double length = glm::length(normal);
      if (!std::isfinite(length))
        throw std::invalid_argument("Walkability support normal exceeds its supported range");
      if (length == 0)
        continue;
      surface.normal = normal / length;
      if (surface.normal.z >= minimumUp)
        surfaces.push_back(std::move(surface));
    }
  }
  std::sort(surfaces.begin(), surfaces.end(), [](const auto& first, const auto& second) {
    const auto firstId = std::tie(first.entityId, first.representationId, first.triangleIndex);
    const auto secondId = std::tie(second.entityId, second.representationId, second.triangleIndex);
    if (firstId != secondId)
      return firstId < secondId;
    for (size_t corner = 0; corner < 3; ++corner)
      for (glm::length_t axis = 0; axis < 3; ++axis)
        if (first.vertices[corner][axis] != second.vertices[corner][axis])
          return first.vertices[corner][axis] < second.vertices[corner][axis];
    return false;
  });
  return surfaces;
}

BimWalkabilityResult traceWalkablePath(
    glm::dvec3 startFeet, glm::dvec3 endFeet, const BimWalkabilityQueries& queries,
    const BimWalkabilitySettings& settings) {
  validate(settings);
  if (!finite(startFeet) || !finite(endFeet))
    throw std::invalid_argument("Walkability endpoints must be finite");
  if (!queries.castSegment || !queries.sweepCapsule || !queries.overlapCapsule)
    throw std::invalid_argument("Walkability requires support, sweep and overlap queries");
  BimWalkabilityResult result;
  result.reachedFeet = startFeet;
  const auto delta = endFeet - startFeet;
  const double distance = std::hypot(delta.x, delta.y);
  // Capsule sweeps test obstacle clearance, not continuing floor support.
  // A coarse caller setting must not skip an unsupported span wider than
  // the robot's footprint. The configured spacing remains an upper bound.
  const double supportSpacing = std::min(settings.sampleSpacing, settings.radius);
  const double intervals = std::max(1.0, std::ceil(distance / supportSpacing));
  if (!std::isfinite(intervals) || intervals >= double(settings.maxSamples)) {
    result.reason = BimWalkabilityFailure::SampleLimit;
    return result;
  }
  const auto count = static_cast<size_t>(intervals);
  const double minimumUp = minimumUpNormal(settings.maxSlopeDegrees);
  const double probeSkin = std::max(settings.skin * 2, 1e-6);
  const auto fail = [&](BimWalkabilityFailure reason,
                        std::optional<BimWalkabilityHit> hit = {}) {
    result.reason = reason;
    result.blockingHit = std::move(hit);
  };
  const auto probe = [&](size_t index, double referenceZ,
                          BimWalkabilitySample& sample) {
    const double fraction = double(index) / double(count);
    auto point = startFeet + delta * fraction;
    point.z = referenceZ;
    const auto origin = point + glm::dvec3(0, 0, settings.maxStepHeight + probeSkin);
    const auto down = glm::dvec3(0, 0,
        -(settings.maxStepHeight + settings.maxDropHeight + 2 * probeSkin));
    if (!finite(origin) || !finite(down))
      throw std::invalid_argument("Walkability support probe exceeds its supported range");
    const auto support = checked(queries.castSegment(origin, down));
    if (!support) {
      fail(BimWalkabilityFailure::MissingSupport);
      return false;
    }
    if (!support->walkableSupport) {
      fail(BimWalkabilityFailure::UnsupportedSurface, support);
      return false;
    }
    const double normalLength = glm::length(support->normal);
    if (!std::isfinite(normalLength) || normalLength == 0 ||
        support->normal.z / normalLength < minimumUp) {
      fail(BimWalkabilityFailure::SteepSurface, support);
      return false;
    }
    sample = {support->position, support->normal / normalLength, support->entityId};
    const double change = sample.feet.z - point.z;
    if (change > settings.maxStepHeight + 1e-9) {
      fail(BimWalkabilityFailure::StepTooHigh, support);
      return false;
    }
    if (change < -settings.maxDropHeight - 1e-9) {
      fail(BimWalkabilityFailure::DropTooFar, support);
      return false;
    }
    return true;
  };
  const auto bottomOf = [&](const BimWalkabilitySample& sample) {
    const auto bottom = walkabilityCapsuleBottom(sample.feet, sample.normal.z, settings);
    if (!finite(bottom))
      throw std::invalid_argument("Walkability capsule placement exceeds its supported range");
    return bottom;
  };
  const auto sweep = [&](glm::dvec3 from, glm::dvec3 to) {
    return checked(queries.sweepCapsule(from, to - from, settings.radius, settings.height));
  };
  for (size_t index = 0; index <= count;) {
    BimWalkabilitySample next;
    if (!probe(index, result.samples.empty() ? startFeet.z : result.reachedFeet.z, next))
      return result;
    const auto bottom = bottomOf(next);
    auto collision = checked(queries.overlapCapsule(bottom, settings.radius, settings.height));
    if (!collision && !result.samples.empty())
      collision = sweep(bottomOf(result.samples.back()), bottom);
    if (collision) {
      bool stepped = false;
      if (!result.samples.empty() && settings.maxStepHeight > 0) {
        const auto previous = bottomOf(result.samples.back());
        // A capsule reaches a stair corner before its centre ray reaches
        // the tread. Look ahead only across one footprint, checking every
        // intervening support sample before accepting a raised landing.
        const double lookahead = std::ceil((2 * settings.radius + supportSpacing) /
                                           supportSpacing);
        const size_t last = index + static_cast<size_t>(std::min(double(count - index), lookahead));
        std::vector<BimWalkabilitySample> supports;
        for (size_t landingIndex = index; landingIndex <= last; ++landingIndex) {
          BimWalkabilitySample landing;
          if (!probe(landingIndex, result.reachedFeet.z, landing))
            break;
          supports.push_back(landing);
          if (landing.feet.z <= result.reachedFeet.z + 1e-9)
            continue;
          const auto landingBottom = bottomOf(landing);
          if (checked(queries.overlapCapsule(landingBottom, settings.radius, settings.height)))
            continue;
          // Raise only as far as this landing needs, rather than consuming
          // the entire allowed step height beneath a low ceiling.
          const double raisedZ = std::max(previous.z, landingBottom.z);
          const glm::dvec3 raisedStart(previous.x, previous.y, raisedZ);
          const glm::dvec3 raisedEnd(landingBottom.x, landingBottom.y, raisedZ);
          if (sweep(previous, raisedStart) || sweep(raisedStart, raisedEnd) ||
              sweep(raisedEnd, landingBottom))
            continue;
          result.samples.insert(result.samples.end(), supports.begin(), supports.end());
          result.reachedFeet = landing.feet;
          index = landingIndex + 1;
          result.reason = BimWalkabilityFailure::None;
          result.blockingHit.reset();
          stepped = true;
          break;
        }
      }
      if (stepped)
        continue;
      fail(BimWalkabilityFailure::Blocked, std::move(collision));
      return result;
    }
    result.reachedFeet = next.feet;
    result.samples.push_back(next);
    ++index;
  }
  if (std::abs(result.reachedFeet.z - endFeet.z) > settings.elevationTolerance) {
    fail(BimWalkabilityFailure::WrongElevation);
    return result;
  }
  result.reachable = true;
  return result;
}

} // namespace container::robotics
