#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/BimCollisionOverlay.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>

namespace {

using namespace container::robotics;

struct DoorwayFixture {
  container::geometry::dotbim::Model source = container::geometry::ifc::LoadFromFile(
      std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "doorway.ifc");
  RoboticsScene semantic = buildRoboticsScene(source, {.sourceNamespace = "doorway"});
  CookedBimCollision geometry = cookBimCollisionGeometry(source, semantic);
};

const SemanticEntity* byGuid(const RoboticsScene& scene, std::string_view guid) {
  const auto found = std::ranges::find(scene.entities, guid, &SemanticEntity::guid);
  return found == scene.entities.end() ? nullptr : &*found;
}

TEST(BimCollisionWorld, NativeIfcDoorControlsCapsuleClearanceAndRespectsBothFloors) {
  DoorwayFixture fixture;
  const auto* door = byGuid(fixture.semantic, "door-guid");
  const auto* wall = byGuid(fixture.semantic, "wall-guid");
  const auto* lower = byGuid(fixture.semantic, "lower-slab");
  const auto* upper = byGuid(fixture.semantic, "upper-slab");
  ASSERT_TRUE(door && wall && lower && upper);
  BimCollisionWorld world(fixture.geometry);
  const auto queries = world.walkabilityQueries();
  const size_t closedBodies = world.activeColliderCount();
  const size_t cachedShapes = world.cachedStaticMeshCount();
  ASSERT_GT(cachedShapes, 0u);
  EXPECT_FALSE(world.isDoorOpen(door->id));
  auto doorRay = queries.castSegment({-1.5, 0, 2}, {3, 0, 0});
  ASSERT_TRUE(doorRay);
  EXPECT_EQ(doorRay->entityId, door->id);

  const glm::dvec3 start{-1.5, 0, 1.2}, end{1.5, 0, 1.2};
  const auto closed = traceWalkablePath(start, end, queries);
  EXPECT_FALSE(closed.reachable);
  EXPECT_EQ(closed.reason, BimWalkabilityFailure::Blocked);
  ASSERT_TRUE(closed.blockingHit);
  EXPECT_EQ(closed.blockingHit->entityId, door->id);

  world.setDoorOpen(door->id, true);
  EXPECT_TRUE(world.isDoorOpen(door->id));
  EXPECT_LT(world.activeColliderCount(), closedBodies);
  EXPECT_FALSE(queries.castSegment({-1.5, 0, 2}, {3, 0, 0}));
  const auto wallRay = queries.castSegment({-1.5, .9, 2}, {3, 0, 0});
  ASSERT_TRUE(wallRay);
  EXPECT_EQ(wallRay->entityId, wall->id);
  EXPECT_NEAR(wallRay->position.x, -.1, 5e-4);

  const auto open = traceWalkablePath(start, end, queries);
  ASSERT_TRUE(open.reachable) << static_cast<int>(open.reason);
  EXPECT_NEAR(open.reachedFeet.z, 1.2, 1e-5);
  ASSERT_FALSE(open.samples.empty());
  EXPECT_TRUE(std::ranges::all_of(open.samples, [&](const auto& sample) {
    return sample.entityId == lower->id && std::abs(sample.feet.z - 1.2) < 1e-5;
  }));
  const auto jamb = traceWalkablePath({-1.5, .45, 1.2}, {1.5, .45, 1.2}, queries);
  EXPECT_FALSE(jamb.reachable);
  ASSERT_TRUE(jamb.blockingHit);
  EXPECT_EQ(jamb.blockingHit->entityId, wall->id);

  const auto upstairs = traceWalkablePath({-1.5, 0, 3.7}, {1.5, 0, 3.7}, queries);
  ASSERT_TRUE(upstairs.reachable) << static_cast<int>(upstairs.reason);
  EXPECT_NEAR(upstairs.reachedFeet.z, 3.7, 1e-5);
  EXPECT_EQ(upstairs.samples.back().entityId, upper->id);
  const auto wrongFloor = traceWalkablePath(start, {-1.0, 0, 3.7}, queries);
  EXPECT_FALSE(wrongFloor.reachable);
  EXPECT_EQ(wrongFloor.reason, BimWalkabilityFailure::WrongElevation);

  world.setDoorOpen(door->id, false);
  EXPECT_EQ(world.activeColliderCount(), closedBodies);
  EXPECT_EQ(world.cachedStaticMeshCount(), cachedShapes);
  const auto closedAgain = queries.castSegment({-1.5, 0, 2}, {3, 0, 0});
  ASSERT_TRUE(closedAgain);
  EXPECT_EQ(closedAgain->entityId, doorRay->entityId);
  EXPECT_DOUBLE_EQ(closedAgain->fraction, doorRay->fraction);
  EXPECT_EQ(closedAgain->position, doorRay->position);
  EXPECT_THROW(world.setDoorOpen(wall->id, true), std::invalid_argument);
}

BimCollider supportBox(std::string id, glm::dvec3 minimum, glm::vec3 size) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  const float x = size.x, y = size.y, z = size.z;
  mesh->vertices = {{0, 0, 0}, {x, 0, 0}, {x, y, 0}, {0, y, 0},
                    {0, 0, z}, {x, 0, z}, {x, y, z}, {0, y, z}};
  mesh->indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                   1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  return {.entityId = std::move(id), .representationId = "box-top-and-sides", .originMeters = minimum,
          .mesh = std::move(mesh), .supportCandidate = true};
}

TEST(BimCollisionWorld, BackFaceRayPreservesUndersideNormalAndRejectsItAsWalkableSupport) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
  mesh->indices = {0, 2, 1, 0, 3, 2}; // Authored underside: normal is -Z.
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "slab-underside", .representationId = "underside",
                                .originMeters = {0, 0, 2}, .mesh = std::move(mesh),
                                .supportCandidate = true});
  EXPECT_TRUE(extractWalkableSurfaces(geometry, 30).empty());
  const BimCollisionWorld world(std::move(geometry));
  const auto queries = world.walkabilityQueries();
  const auto backFace = queries.castSegment({.25, 0, 3}, {0, 0, -2});
  ASSERT_TRUE(backFace);
  EXPECT_EQ(backFace->entityId, "slab-underside");
  EXPECT_NEAR(backFace->position.z, 2, 1e-6);
  EXPECT_LT(backFace->normal.z, -.99);
  const auto frontFace = queries.castSegment({.25, 0, 1}, {0, 0, 2});
  ASSERT_TRUE(frontFace);
  EXPECT_EQ(frontFace->normal, backFace->normal);
  const auto path = traceWalkablePath({-.5, 0, 2}, {.5, 0, 2}, queries);
  EXPECT_FALSE(path.reachable);
  EXPECT_EQ(path.reason, BimWalkabilityFailure::SteepSurface);
  EXPECT_TRUE(path.samples.empty());
  ASSERT_TRUE(path.blockingHit);
  EXPECT_EQ(path.blockingHit->entityId, "slab-underside");
  EXPECT_LT(path.blockingHit->normal.z, -.99);
}

TEST(BimCollisionWorld, CapsuleWalksOnARampAndRespectsTheSlopeLimit) {
  auto mesh = std::make_shared<BimCollisionMesh>();
  mesh->vertices = {{0, -1, 0}, {2, -1, 1}, {2, 1, 1}, {0, 1, 0}};
  mesh->indices = {0, 1, 2, 0, 2, 3};
  CookedBimCollision geometry;
  geometry.colliders.push_back({.entityId = "ramp", .representationId = "ramp-support", .originMeters = {0, 0, 2},
                                .mesh = std::move(mesh), .supportCandidate = true});
  const BimCollisionWorld world(geometry);
  EXPECT_EQ(extractWalkableSurfaces(geometry, 30).size(), 2u);
  EXPECT_TRUE(extractWalkableSurfaces(geometry, 20).empty());
  const auto accepted = traceWalkablePath({.3, 0, 2.15}, {1.7, 0, 2.85}, world.walkabilityQueries());
  ASSERT_TRUE(accepted.reachable) << static_cast<int>(accepted.reason);
  EXPECT_NEAR(accepted.reachedFeet.z, 2.85, 1e-5);
  BimWalkabilitySettings settings;
  settings.maxSlopeDegrees = 20;
  const auto rejected = traceWalkablePath({.3, 0, 2.15}, {1.7, 0, 2.85}, world.walkabilityQueries(), settings);
  EXPECT_FALSE(rejected.reachable);
  EXPECT_EQ(rejected.reason, BimWalkabilityFailure::SteepSurface);
}

TEST(BimCollisionWorld, CapsuleStepsUpAnAuthoredTreadWithinItsStepLimit) {
  CookedBimCollision geometry;
  geometry.colliders = {supportBox("approach", {-1, -1, 0}, {1, 2, 1}),
                        supportBox("tread", {0, -1, 0}, {1, 2, 1.14f})};
  const BimCollisionWorld world(std::move(geometry));
  const auto queries = world.walkabilityQueries();
  const auto accepted = traceWalkablePath({-.65, 0, 1}, {.65, 0, 1.14}, queries);
  ASSERT_TRUE(accepted.reachable) << static_cast<int>(accepted.reason);
  EXPECT_EQ(accepted.samples.back().entityId, "tread");
  BimWalkabilitySettings settings;
  settings.maxStepHeight = .08;
  const auto rejected = traceWalkablePath({-.65, 0, 1}, {.65, 0, 1.14}, queries, settings);
  EXPECT_FALSE(rejected.reachable);
  EXPECT_LT(rejected.reachedFeet.x, 0.0);
}

TEST(BimCollisionWorld, CoarseSupportSpacingCannotCrossAnUnsupportedFloorGap) {
  CookedBimCollision geometry;
  // The 0.8 metre gap is wider than the default capsule's 0.5 metre
  // diameter. A horizontal capsule sweep sees no obstacle in empty air.
  geometry.colliders = {supportBox("approach", {-2, -1, 0}, {1.6f, 2, 1}),
                        supportBox("landing", {.4, -1, 0}, {1.6f, 2, 1})};
  const BimCollisionWorld world(std::move(geometry));
  const auto queries = world.walkabilityQueries();
  EXPECT_FALSE(queries.sweepCapsule({-1, 0, 1.002}, {2, 0, 0}, .25, 1.6));
  BimWalkabilitySettings settings;
  settings.sampleSpacing = 2;
  const auto route = traceWalkablePath({-1, 0, 1}, {1, 0, 1}, queries, settings);
  EXPECT_FALSE(route.reachable);
  EXPECT_EQ(route.reason, BimWalkabilityFailure::MissingSupport);
  EXPECT_LT(route.reachedFeet.x, -.39);

  // The configured sample budget must count the radius-bounded probes,
  // rather than only the two samples requested by the coarse setting.
  settings.maxSamples = 2;
  EXPECT_EQ(traceWalkablePath({-1, 0, 1}, {1, 0, 1}, queries, settings).reason,
            BimWalkabilityFailure::SampleLimit);
}

TEST(BimCollisionWorld, DebugOverlayUsesRendererCoordinatesAndOnlyActiveColliders) {
  DoorwayFixture fixture;
  const auto* door = byGuid(fixture.semantic, "door-guid");
  ASSERT_TRUE(door);
  const std::array openDoors{door->id};
  const BimCollisionWorld world(fixture.geometry, openDoors);
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto destination = std::filesystem::temp_directory_path() /
      ("container-doorway-collision-" + std::to_string(nonce) + ".glb");
  struct Cleanup {
    std::filesystem::path file;
    ~Cleanup() { std::error_code error; std::filesystem::remove(file, error); }
  } cleanup{destination};
  writeBimCollisionOverlay(destination, world, fixture.semantic.frame, 30);

  std::ifstream stream(destination, std::ios::binary);
  ASSERT_TRUE(stream);
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), {});
  ASSERT_LT(bytes.size(), std::numeric_limits<unsigned int>::max());
  tinygltf::TinyGLTF loader;
  tinygltf::Model overlay;
  std::string error, warning;
  ASSERT_TRUE(loader.LoadBinaryFromMemory(&overlay, &error, &warning, bytes.data(),
                                         static_cast<unsigned int>(bytes.size()))) << error;
  EXPECT_EQ(overlay.meshes.size(), 3u);
  EXPECT_FALSE(std::ranges::any_of(overlay.meshes, [](const auto& mesh) { return mesh.name == "Closed doors"; }));
  const auto support = std::ranges::find(overlay.meshes, "Walkable support candidates", &tinygltf::Mesh::name);
  ASSERT_NE(support, overlay.meshes.end());
  const auto& positions = overlay.accessors[support->primitives.front().attributes.at("POSITION")];
  EXPECT_NEAR(positions.minValues.at(1), 1.2, 1e-5);
  EXPECT_NEAR(positions.maxValues.at(1), 3.7, 1e-5);
}

}  // namespace
