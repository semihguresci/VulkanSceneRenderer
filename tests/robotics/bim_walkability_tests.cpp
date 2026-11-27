#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/BimWalkability.h"

#include <gtest/gtest.h>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {

using namespace container::robotics;

struct Box {
  glm::dvec3 minimum;
  glm::dvec3 maximum;
  std::string id;
};

// Independent capsule/AABB distance oracle: an upright capsule is the radius
// offset of a vertical segment. This does not use the production sweep code.
bool overlaps(glm::dvec3 bottom, double radius, double height, const Box& box) {
  const double dx = std::max({box.minimum.x - bottom.x, 0.0, bottom.x - box.maximum.x});
  const double dy = std::max({box.minimum.y - bottom.y, 0.0, bottom.y - box.maximum.y});
  const double dz = std::max({box.minimum.z - (bottom.z + height - radius), 0.0,
                              bottom.z + radius - box.maximum.z});
  return dx * dx + dy * dy + dz * dz < radius * radius - 1e-12;
}

struct AnalyticWorld {
  std::function<std::optional<double>(double)> floor = [](double) { return 0.0; };
  glm::dvec3 floorNormal{0, 0, 1};
  std::optional<double> upperFloor;
  std::vector<Box> boxes;
  mutable std::vector<glm::dvec3> observedBottoms;

  BimWalkabilityQueries queries() const {
    const auto occupied = [this](glm::dvec3 bottom, double radius, double height)
        -> std::optional<BimWalkabilityHit> {
      observedBottoms.push_back(bottom);
      for (const auto& box : boxes)
        if (overlaps(bottom, radius, height, box))
          return BimWalkabilityHit{0, bottom, {1, 0, 0}, box.id, false};
      return std::nullopt;
    };
    return {
      .castSegment = [this](glm::dvec3 origin, glm::dvec3 displacement)
          -> std::optional<BimWalkabilityHit> {
        EXPECT_DOUBLE_EQ(displacement.x, 0);
        EXPECT_DOUBLE_EQ(displacement.y, 0);
        std::optional<BimWalkabilityHit> nearest;
        const auto consider = [&](double elevation) {
          const double fraction = (elevation - origin.z) / displacement.z;
          if (fraction >= 0 && fraction <= 1 && (!nearest || fraction < nearest->fraction))
            nearest = BimWalkabilityHit{fraction, {origin.x, origin.y, elevation}, floorNormal, "floor", true};
        };
        if (const auto elevation = floor(origin.x))
          consider(*elevation);
        if (upperFloor)
          consider(*upperFloor);
        return nearest;
      },
      .sweepCapsule = [occupied](glm::dvec3 bottom, glm::dvec3 displacement,
                                 double radius, double height) -> std::optional<BimWalkabilityHit> {
        // 1 mm oracle sampling is much smaller than every authored obstacle
        // in these fixtures; production uses the actual collision backend.
        const size_t steps = std::max<size_t>(1, static_cast<size_t>(std::ceil(glm::length(displacement) / .001)));
        for (size_t i = 0; i <= steps; ++i) {
          const double fraction = double(i) / double(steps);
          if (auto hit = occupied(bottom + fraction * displacement, radius, height)) {
            hit->fraction = fraction;
            return hit;
          }
        }
        return std::nullopt;
      },
      .overlapCapsule = occupied,
    };
  }
};

AnalyticWorld doorway(bool closed) {
  AnalyticWorld world;
  world.boxes = {
      {{-.1, -3, 0}, {.1, -.6, 2.5}, "wall-left"},
      {{-.1, .6, 0}, {.1, 3, 2.5}, "wall-right"},
      {{-.1, -.6, 2.1}, {.1, .6, 2.5}, "lintel"},
  };
  if (closed)
    world.boxes.push_back({{-.04, -.6, 0}, {.04, .6, 2.05}, "door"});
  return world;
}

TEST(BimWalkability, ExtractsSourceTrianglesBySlopeAndPreservesElevationAndIdentity) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {
      {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, // floor
      {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, // underside
      {0, 0, 0}, {0, 1, 0}, {0, 0, 1}, // wall
      {0, 0, 0}, {1, 0, .25f}, {0, 1, 0}, // 14 degree ramp
      {0, 0, 0}, {1, 0, 1}, {0, 1, 0}, // 45 degree ramp
  };
  for (uint32_t index = 0; index < mesh->vertices.size(); ++index)
    mesh->indices.push_back(index);
  mesh->indices.insert(mesh->indices.end(), {0, 0, 0}); // degenerate
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "slab", .representationId = "body",
      .originMeters = {4, -7, 3}, .mesh = mesh, .supportCandidate = true});
  const auto surfaces = extractWalkableSurfaces(geometry, 30);
  ASSERT_EQ(surfaces.size(), 2u);
  EXPECT_EQ(surfaces[0].entityId, "slab");
  EXPECT_EQ(surfaces[0].representationId, "body");
  EXPECT_EQ(surfaces[0].triangleIndex, 0u);
  EXPECT_EQ(surfaces[0].vertices[0], glm::dvec3(4, -7, 3));
  EXPECT_EQ(surfaces[0].normal, glm::dvec3(0, 0, 1));
  EXPECT_EQ(surfaces[1].triangleIndex, 3u);
  EXPECT_DOUBLE_EQ(surfaces[1].vertices[1].z, 3.25);
  geometry.colliders.front().supportCandidate = false;
  EXPECT_TRUE(extractWalkableSurfaces(geometry, 30).empty());
  geometry.colliders.front().supportCandidate = true;
  mesh->indices[0] = 999;
  EXPECT_THROW((void)extractWalkableSurfaces(geometry, 30), std::invalid_argument);
  EXPECT_THROW((void)extractWalkableSurfaces({}, 90), std::invalid_argument);
}

TEST(BimWalkability, DoorAndWallClearanceUseRadiusAndHeight) {
  const auto closed = doorway(true);
  const auto blocked = traceWalkablePath({-2, 0, 0}, {2, 0, 0}, closed.queries());
  EXPECT_FALSE(blocked.reachable);
  EXPECT_EQ(blocked.reason, BimWalkabilityFailure::Blocked);
  ASSERT_TRUE(blocked.blockingHit);
  EXPECT_EQ(blocked.blockingHit->entityId, "door");
  const auto open = doorway(false);
  EXPECT_TRUE(traceWalkablePath({-2, 0, 0}, {2, 0, 0}, open.queries()).reachable);
  EXPECT_TRUE(traceWalkablePath({-2, .3, 0}, {2, .3, 0}, open.queries()).reachable);
  EXPECT_FALSE(traceWalkablePath({-2, .4, 0}, {2, .4, 0}, open.queries()).reachable);
  EXPECT_FALSE(traceWalkablePath({-2, 1, 0}, {2, 1, 0}, open.queries()).reachable);
  BimWalkabilitySettings wide;
  wide.radius = .65;
  EXPECT_FALSE(traceWalkablePath({-2, 0, 0}, {2, 0, 0}, open.queries(), wide).reachable);
  BimWalkabilitySettings tall;
  tall.height = 2.2;
  EXPECT_FALSE(traceWalkablePath({-2, 0, 0}, {2, 0, 0}, open.queries(), tall).reachable);
  EXPECT_FALSE(traceWalkablePath({0, 0, 0}, {2, 0, 0}, closed.queries()).reachable);
}

TEST(BimWalkability, FloorElevationCannotTeleportAndSupportLossIsRejected) {
  AnalyticWorld floors;
  floors.upperFloor = 3;
  const auto ground = traceWalkablePath({-1, 0, 0}, {1, 0, 0}, floors.queries());
  ASSERT_TRUE(ground.reachable);
  EXPECT_DOUBLE_EQ(ground.reachedFeet.z, 0);
  const auto upper = traceWalkablePath({-1, 0, 3}, {1, 0, 3}, floors.queries());
  ASSERT_TRUE(upper.reachable);
  EXPECT_DOUBLE_EQ(upper.reachedFeet.z, 3);
  const auto wrong = traceWalkablePath({-1, 0, 0}, {1, 0, 3}, floors.queries());
  EXPECT_FALSE(wrong.reachable);
  EXPECT_EQ(wrong.reason, BimWalkabilityFailure::WrongElevation);
  AnalyticWorld gap;
  gap.floor = [](double x) -> std::optional<double> {
    return std::abs(x) < .2 ? std::nullopt : std::optional(0.0);
  };
  EXPECT_EQ(traceWalkablePath({-1, 0, 0}, {1, 0, 0}, gap.queries()).reason,
            BimWalkabilityFailure::MissingSupport);
}

TEST(BimWalkability, RampSlopeUsesWorldNormalAndCapsuleTangencyLift) {
  AnalyticWorld ramp;
  ramp.floor = [](double x) { return x * .25; };
  ramp.floorNormal = glm::normalize(glm::dvec3(-.25, 0, 1));
  const auto route = traceWalkablePath({0, 0, 0}, {2, 0, .5}, ramp.queries());
  ASSERT_TRUE(route.reachable);
  ASSERT_FALSE(ramp.observedBottoms.empty());
  const BimWalkabilitySettings settings;
  for (const auto bottom : ramp.observedBottoms) {
    const double lowerSphereDistance = glm::dot(ramp.floorNormal,
        bottom + glm::dvec3(0, 0, settings.radius));
    EXPECT_GE(lowerSphereDistance, settings.radius + settings.skin - 1e-10);
  }
  ramp.floorNormal = glm::normalize(glm::dvec3(-1, 0, 1));
  EXPECT_EQ(traceWalkablePath({0, 0, 0}, {2, 0, .5}, ramp.queries()).reason,
            BimWalkabilityFailure::SteepSurface);
}

TEST(BimWalkability, CapsuleCanStepOntoLowTreadButCannotBypassHighRiser) {
  const auto stairs = [](double rise) {
    AnalyticWorld world;
    world.floor = [rise](double x) { return x < 0 ? 0.0 : rise; };
    world.boxes.push_back({{0, -3, -.2}, {3, 3, rise}, "stair"});
    return world;
  };
  const auto low = stairs(.15);
  const auto climb = traceWalkablePath({-1, 0, 0}, {1, 0, .15}, low.queries());
  EXPECT_TRUE(climb.reachable);
  EXPECT_DOUBLE_EQ(climb.reachedFeet.z, .15);
  auto lowCeiling = stairs(.15);
  lowCeiling.boxes.push_back({{-2, -3, 1.76}, {3, 3, 2}, "ceiling"});
  EXPECT_TRUE(traceWalkablePath({-1, 0, 0}, {1, 0, .15}, lowCeiling.queries()).reachable);
  lowCeiling.boxes.back().minimum.z = 1.7;
  EXPECT_FALSE(traceWalkablePath({-1, 0, 0}, {1, 0, .15}, lowCeiling.queries()).reachable);
  const auto high = stairs(.3);
  EXPECT_FALSE(traceWalkablePath({-1, 0, 0}, {1, 0, .3}, high.queries()).reachable);
}

TEST(BimWalkability, RepeatabilityBudgetsAndInvalidInputAreExplicit) {
  const auto world = doorway(true);
  const auto baseline = traceWalkablePath({-2, 0, 0}, {2, 0, 0}, world.queries());
  for (int repeat = 0; repeat < 10; ++repeat) {
    const auto result = traceWalkablePath({-2, 0, 0}, {2, 0, 0}, world.queries());
    EXPECT_EQ(result.reason, baseline.reason);
    EXPECT_EQ(result.reachedFeet, baseline.reachedFeet);
    EXPECT_EQ(result.samples.size(), baseline.samples.size());
    ASSERT_TRUE(result.blockingHit);
    EXPECT_EQ(result.blockingHit->entityId, baseline.blockingHit->entityId);
  }
  BimWalkabilitySettings bounded;
  bounded.maxSamples = 5;
  EXPECT_EQ(traceWalkablePath({-2, 0, 0}, {2, 0, 0}, world.queries(), bounded).reason,
            BimWalkabilityFailure::SampleLimit);
  bounded.radius = -1;
  EXPECT_THROW((void)traceWalkablePath({}, {}, world.queries(), bounded), std::invalid_argument);
  EXPECT_THROW((void)traceWalkablePath({}, {}, {}), std::invalid_argument);
}

} // namespace
