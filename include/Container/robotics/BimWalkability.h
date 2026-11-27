#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace container::robotics {

struct CookedBimCollision;

struct BimWalkableSurface {
  std::string entityId;
  std::string representationId;
  uint32_t triangleIndex{}; // Triangle ordinal in the cooked mesh.
  std::array<glm::dvec3, 3> vertices;
  glm::dvec3 normal;
};

// Actual +Z-up metre triangles, retaining their authored elevation. Only
// supportCandidate geometry with an upward normal within the slope limit is
// returned. Degenerate triangles are omitted; malformed recipes throw.
[[nodiscard]] std::vector<BimWalkableSurface> extractWalkableSurfaces(
    const CookedBimCollision& geometry, double maxSlopeDegrees);

struct BimWalkabilityHit {
  double fraction{};
  glm::dvec3 position{};
  glm::dvec3 normal{};
  std::string entityId;
  bool walkableSupport{};
};

struct BimWalkabilityQueries {
  // Return the nearest hit on a finite segment (fraction in [0,1]).
  std::function<std::optional<BimWalkabilityHit>(glm::dvec3 origin,
                                                glm::dvec3 displacement)> castSegment;
  // The capsule is upright along +Z. bottom is its geometric lowest point,
  // radius is horizontal clearance, and height includes both hemispheres.
  // Return blocking contacts, including initial overlaps. Supporting contacts
  // within the backend's contact tolerance may be filtered by the adapter.
  std::function<std::optional<BimWalkabilityHit>(glm::dvec3 bottom,
      glm::dvec3 displacement, double radius, double height)> sweepCapsule;
  std::function<std::optional<BimWalkabilityHit>(glm::dvec3 bottom,
      double radius, double height)> overlapCapsule;
};

struct BimWalkabilitySettings {
  double radius{.25};
  double height{1.6};
  double maxSlopeDegrees{30};
  double maxStepHeight{.18};
  double maxDropHeight{.25};
  double sampleSpacing{.1}; // Upper bound; support spacing is also capped by radius.
  double skin{.002};
  double elevationTolerance{.02};
  size_t maxSamples{100'000};
};

enum class BimWalkabilityFailure : uint8_t {
  None,
  MissingSupport,
  UnsupportedSurface,
  SteepSurface,
  StepTooHigh,
  DropTooFar,
  Blocked,
  WrongElevation,
  SampleLimit,
};

struct BimWalkabilitySample {
  glm::dvec3 feet{}; // Ground point vertically below the capsule axis.
  glm::dvec3 normal{};
  std::string entityId;
};

struct BimWalkabilityResult {
  bool reachable{};
  BimWalkabilityFailure reason{BimWalkabilityFailure::None};
  glm::dvec3 reachedFeet{};
  std::optional<BimWalkabilityHit> blockingHit;
  std::vector<BimWalkabilitySample> samples;
};

// Test a straight XY route with bounded support samples, actual capsule
// clearance, and optional up/across/down steps. Feet are surface points,
// not capsule centres: slope-dependent tangency and skin are applied here.
// This does not find a route around obstacles or infer semantic portals.
// Queries must outlive the call; adapters may reference their owning world.
[[nodiscard]] BimWalkabilityResult traceWalkablePath(
    glm::dvec3 startFeet, glm::dvec3 endFeet,
    const BimWalkabilityQueries& queries,
    const BimWalkabilitySettings& settings = {});

} // namespace container::robotics
