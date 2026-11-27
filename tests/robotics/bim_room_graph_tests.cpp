#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/BimRoomGraph.h"
#include "Container/robotics/RoboticsSceneManifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace container::robotics;
namespace bim = container::geometry::dotbim;

constexpr std::string_view lowerWest = "0000000000000000000087";
constexpr std::string_view lowerEast = "0000000000000000000093";
constexpr std::string_view upperWest = "0000000000000000000095";
constexpr std::string_view roomDoor = "0000000000000000000044";
constexpr std::string_view doorOpening = "0000000000000000000034";
constexpr std::string_view partition = "0000000000000000000027";

bim::Model loadRoomFixture() {
  return container::geometry::ifc::LoadFromFile(
      std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "room-connectivity.ifc");
}

RoboticsScene roomScene(const bim::Model& model) {
  return buildRoboticsScene(model, {.sourceNamespace = "room-connectivity"});
}

const SemanticEntity& byGuid(const RoboticsScene& scene, std::string_view guid) {
  const auto found = std::ranges::find(scene.entities, guid, &SemanticEntity::guid);
  if (found == scene.entities.end())
    throw std::runtime_error("Room graph fixture lost IFC product " + std::string(guid));
  return *found;
}

const BimRoom& roomByGuid(const BimRoomGraph& graph, std::string_view guid) {
  const auto found = std::ranges::find(graph.rooms, guid, &BimRoom::guid);
  if (found == graph.rooms.end())
    throw std::runtime_error("Room graph fixture lost space " + std::string(guid));
  return *found;
}

void expectLowerPair(const BimRoomGraph& graph, const RoboticsScene& scene) {
  ASSERT_EQ(graph.rooms.size(), 3u);
  ASSERT_EQ(graph.portals.size(), 1u);
  const auto& west = roomByGuid(graph, lowerWest);
  const auto& east = roomByGuid(graph, lowerEast);
  const auto& upper = roomByGuid(graph, upperWest);
  ASSERT_EQ(west.storeyIds.size(), 1u);
  ASSERT_EQ(upper.storeyIds.size(), 1u);
  EXPECT_EQ(west.storeyIds, east.storeyIds);
  EXPECT_NE(west.storeyIds, upper.storeyIds);
  EXPECT_TRUE(west.hasVolume && east.hasVolume);
  const auto& portal = graph.portals.front();
  EXPECT_EQ(portal.roomIds, (std::array{west.id, east.id}));
  EXPECT_EQ(portal.doorId, byGuid(scene, roomDoor).id);
  EXPECT_EQ(portal.doorGuid, roomDoor);
  EXPECT_EQ(portal.openingId, byGuid(scene, doorOpening).id);
  EXPECT_EQ(portal.hostId, byGuid(scene, partition).id);
}

void expectOpenCrossings(const BimRoomPortal& portal) {
  ASSERT_EQ(portal.state, BimPortalState::Open);
  ASSERT_TRUE(portal.forward);
  ASSERT_TRUE(portal.reverse);
  EXPECT_EQ(portal.forward->result, BimWalkabilityFailure::None);
  EXPECT_EQ(portal.reverse->result, BimWalkabilityFailure::None);
  EXPECT_LT(portal.forward->fromFeet.x, -.1);
  EXPECT_GT(portal.forward->toFeet.x, .1);
  EXPECT_GT(portal.reverse->fromFeet.x, .1);
  EXPECT_LT(portal.reverse->toFeet.x, -.1);
  EXPECT_NEAR(portal.forward->fromFeet.z, 1.2, 1e-5);
  EXPECT_NEAR(portal.forward->toFeet.z, 1.2, 1e-5);
  EXPECT_NEAR(portal.reverse->fromFeet.z, 1.2, 1e-5);
  EXPECT_NEAR(portal.reverse->toFeet.z, 1.2, 1e-5);
}

TEST(BimRoomGraph, AuthoredPortalKeepsAdjacencyWhileClosedOrLocked) {
  const auto model = loadRoomFixture();
  const auto scene = roomScene(model);
  const BimCollisionWorld world(cookBimCollisionGeometry(model, scene));
  const auto closed = buildBimRoomGraph(model, scene, world);
  expectLowerPair(closed, scene);
  ASSERT_EQ(closed.portals.size(), 1u);
  EXPECT_TRUE(roomByGuid(closed, upperWest).hasVolume);
  EXPECT_EQ(closed.portals.front().evidence, "authored-space-boundary");
  EXPECT_EQ(closed.portals.front().state, BimPortalState::Closed);
  EXPECT_FALSE(closed.portals.front().forward);
  EXPECT_FALSE(closed.portals.front().reverse);

  const std::array lockedDoors{byGuid(scene, roomDoor).id};
  const auto locked = buildBimRoomGraph(model, scene, world, lockedDoors);
  expectLowerPair(locked, scene);
  ASSERT_EQ(locked.portals.size(), 1u);
  EXPECT_EQ(locked.portals.front().evidence, closed.portals.front().evidence);
  EXPECT_EQ(locked.portals.front().state, BimPortalState::Locked);
  EXPECT_FALSE(locked.portals.front().forward);
  EXPECT_FALSE(locked.portals.front().reverse);
}

TEST(BimRoomGraph, OpenDoorAllowsBidirectionalTravelOnLowerFloorAndCannotBeLocked) {
  const auto model = loadRoomFixture();
  const auto scene = roomScene(model);
  const std::array doorIds{byGuid(scene, roomDoor).id};
  const BimCollisionWorld world(cookBimCollisionGeometry(model, scene), doorIds);
  const auto graph = buildBimRoomGraph(model, scene, world);
  expectLowerPair(graph, scene);
  ASSERT_EQ(graph.portals.size(), 1u);
  EXPECT_EQ(graph.portals.front().evidence, "authored-space-boundary");
  expectOpenCrossings(graph.portals.front());
  EXPECT_THROW((void)buildBimRoomGraph(model, scene, world, doorIds), std::invalid_argument);
}

TEST(BimRoomGraph, OppositeSpaceVolumesRecoverPortalWhenSpaceBoundariesAreAbsent) {
  auto model = loadRoomFixture();
  ASSERT_EQ(std::erase_if(model.semanticRelationships, [](const auto& relationship) {
    return relationship.kind == bim::SemanticRelationshipKind::SpaceBoundary;
  }), 5u);
  const auto scene = roomScene(model);
  const std::array openDoors{byGuid(scene, roomDoor).id};
  const BimCollisionWorld world(cookBimCollisionGeometry(model, scene), openDoors);
  const auto graph = buildBimRoomGraph(model, scene, world);
  expectLowerPair(graph, scene);
  ASSERT_EQ(graph.portals.size(), 1u);
  EXPECT_EQ(graph.portals.front().evidence, "opening-and-space-volumes");
  expectOpenCrossings(graph.portals.front());
}

TEST(BimRoomGraph, KeepsSpaceWithoutVolumeAndAttributesItsDiagnostic) {
  auto model = loadRoomFixture();
  ASSERT_EQ(std::erase_if(model.elements, [](const auto& element) {
    return element.guid == upperWest;
  }), 1u);
  const auto scene = roomScene(model);
  const BimCollisionWorld world(cookBimCollisionGeometry(model, scene));
  const auto graph = buildBimRoomGraph(model, scene, world);
  expectLowerPair(graph, scene);
  EXPECT_FALSE(roomByGuid(graph, upperWest).hasVolume);
  const auto& missing = byGuid(scene, upperWest);
  const auto diagnostic = std::ranges::find_if(graph.diagnostics, [&](const auto& entry) {
    return entry.code == "space-volume-unavailable" && entry.sourceId == missing.id;
  });
  EXPECT_NE(diagnostic, graph.diagnostics.end());
}

TEST(BimRoomGraph, ManifestRoundTripPreservesNativeOpeningAndPhysicalBoundaries) {
  const auto model = loadRoomFixture();
  const auto scene = roomScene(model);
  const auto serialized = serializeRoboticsScene(scene);
  const auto parsed = parseRoboticsScene(serialized);
  EXPECT_EQ(serializeRoboticsScene(parsed), serialized);

  const auto& opening = byGuid(parsed, doorOpening);
  EXPECT_EQ(opening.kind, SemanticKind::Opening);
  EXPECT_TRUE(opening.representations.empty());
  ASSERT_TRUE(opening.roboticsBounds);
  EXPECT_EQ(opening.roboticsBounds, byGuid(scene, doorOpening).roboticsBounds);
  const auto& bounds = *opening.roboticsBounds;
  EXPECT_NEAR(bounds.minimum.x, -.2, 1e-6);
  EXPECT_NEAR(bounds.minimum.y, -.6, 1e-6);
  EXPECT_NEAR(bounds.minimum.z, 1.2, 1e-6);
  EXPECT_NEAR(bounds.maximum.x, .2, 1e-6);
  EXPECT_NEAR(bounds.maximum.y, .6, 1e-6);
  EXPECT_NEAR(bounds.maximum.z, 3.3, 1e-6);

  constexpr std::string_view upperSlab = "0000000000000000000053";
  const std::array expectedBoundaries{std::pair{lowerWest, roomDoor}, std::pair{lowerEast, roomDoor},
      std::pair{lowerWest, partition}, std::pair{lowerEast, partition}, std::pair{upperWest, upperSlab}};
  EXPECT_EQ(std::ranges::count(parsed.relationships, SemanticRelationshipKind::SpaceBoundary,
                               &SemanticRelationship::kind), expectedBoundaries.size());
  for (const auto& [roomGuid, elementGuid] : expectedBoundaries) {
    const auto& room = byGuid(parsed, roomGuid);
    const auto& element = byGuid(parsed, elementGuid);
    EXPECT_TRUE(std::ranges::any_of(parsed.relationships, [&](const auto& edge) {
      return edge.kind == SemanticRelationshipKind::SpaceBoundary &&
             edge.from == room.id && edge.to == element.id;
    })) << roomGuid << " to " << elementGuid;
  }

  const std::array openDoors{byGuid(parsed, roomDoor).id};
  const BimCollisionWorld world(cookBimCollisionGeometry(model, parsed), openDoors);
  const auto graph = buildBimRoomGraph(model, parsed, world);
  expectLowerPair(graph, parsed);
  ASSERT_EQ(graph.portals.size(), 1u);
  EXPECT_EQ(graph.portals.front().evidence, "authored-space-boundary");
  expectOpenCrossings(graph.portals.front());
}

TEST(BimRoomGraph, ReloadAndInputOrderingKeepKeysAndWalkWitnessesStable) {
  const auto source = loadRoomFixture();
  const auto scene = roomScene(source);
  const std::array openDoors{byGuid(scene, roomDoor).id};
  const BimCollisionWorld world(cookBimCollisionGeometry(source, scene), openDoors);
  const auto expected = buildBimRoomGraph(source, scene, world);
  ASSERT_EQ(expected.portals.size(), 1u);
  expectOpenCrossings(expected.portals.front());

  auto reloaded = loadRoomFixture();
  std::ranges::reverse(reloaded.semanticProducts);
  std::ranges::reverse(reloaded.semanticRelationships);
  std::ranges::reverse(reloaded.elements);
  auto reorderedScene = roomScene(reloaded);
  std::ranges::reverse(reorderedScene.entities);
  std::ranges::reverse(reorderedScene.relationships);
  const BimCollisionWorld reloadedWorld(cookBimCollisionGeometry(reloaded, reorderedScene), openDoors);
  const auto actual = buildBimRoomGraph(reloaded, reorderedScene, reloadedWorld);
  ASSERT_EQ(actual.rooms.size(), expected.rooms.size());
  EXPECT_TRUE(std::ranges::is_sorted(actual.rooms, {}, &BimRoom::id));
  for (size_t i = 0; i < expected.rooms.size(); ++i) {
    EXPECT_EQ(actual.rooms[i].id, expected.rooms[i].id);
    EXPECT_EQ(actual.rooms[i].storeyIds, expected.rooms[i].storeyIds);
    EXPECT_EQ(actual.rooms[i].hasVolume, expected.rooms[i].hasVolume);
  }
  ASSERT_EQ(actual.portals.size(), 1u);
  const auto& first = expected.portals.front();
  const auto& second = actual.portals.front();
  EXPECT_EQ(second.doorId, first.doorId);
  EXPECT_EQ(second.openingId, first.openingId);
  EXPECT_EQ(second.hostId, first.hostId);
  EXPECT_EQ(second.roomIds, first.roomIds);
  EXPECT_EQ(second.evidence, first.evidence);
  expectOpenCrossings(second);
  ASSERT_TRUE(first.forward && first.reverse && second.forward && second.reverse);
  EXPECT_EQ(second.forward->fromFeet, first.forward->fromFeet);
  EXPECT_EQ(second.forward->toFeet, first.forward->toFeet);
  EXPECT_EQ(second.reverse->fromFeet, first.reverse->fromFeet);
  EXPECT_EQ(second.reverse->toFeet, first.reverse->toFeet);
}

TEST(BimRoomGraph, OpenDoorRetainsAdjacencyWhenAgentCannotClearLintel) {
  const auto model = loadRoomFixture();
  const auto scene = roomScene(model);
  const std::array openDoors{byGuid(scene, roomDoor).id};
  const BimCollisionWorld world(cookBimCollisionGeometry(model, scene), openDoors);
  const BimWalkabilitySettings tallAgent{.radius = .20, .height = 2.15};
  const auto graph = buildBimRoomGraph(model, scene, world, {}, tallAgent);
  expectLowerPair(graph, scene);
  ASSERT_EQ(graph.portals.size(), 1u);
  const auto& portal = graph.portals.front();
  EXPECT_EQ(portal.state, BimPortalState::Open);
  ASSERT_TRUE(portal.forward && portal.reverse);
  EXPECT_EQ(portal.forward->result, BimWalkabilityFailure::Blocked);
  EXPECT_EQ(portal.reverse->result, BimWalkabilityFailure::Blocked);
}

TEST(BimRoomGraph, SupportPointMustBeInsideTheRoomAtFloorHeight) {
  auto model = loadRoomFixture();
  const auto west = std::ranges::find(model.elements, lowerWest, &bim::Element::guid);
  ASSERT_NE(west, model.elements.end());
  const auto mesh = std::ranges::find(model.meshRanges, west->meshId, &bim::MeshRange::meshId);
  ASSERT_NE(mesh, model.meshRanges.end());
  const auto lastMesh = std::ranges::max_element(model.meshRanges, {}, &bim::MeshRange::meshId);
  auto sheared = *mesh;
  sheared.meshId = lastMesh->meshId + 1;
  sheared.firstIndex = static_cast<uint32_t>(model.indices.size());
  // Give only Lower West a separate geometry range. Native IFC Y maps to REP-103 X.
  // The shape is unchanged at the aperture midpoint (1.05 m above the floor),
  // but at floor height the wall-facing side recedes from X=-.1 m to X=-1.15 m.
  for (uint32_t i = 0; i < mesh->indexCount; ++i) {
    auto vertex = model.vertices.at(model.indices.at(mesh->firstIndex + i));
    vertex.position.y += vertex.position.z - 1050.f;
    model.indices.push_back(static_cast<uint32_t>(model.vertices.size()));
    model.vertices.push_back(vertex);
  }
  model.meshRanges.push_back(sheared);
  west->meshId = sheared.meshId;

  const auto scene = roomScene(model);
  const std::array openDoors{byGuid(scene, roomDoor).id};
  const BimCollisionWorld world(cookBimCollisionGeometry(model, scene), openDoors);
  EXPECT_TRUE(traceWalkablePath({-.452, 0, 1.2}, {.452, 0, 1.2}, world.walkabilityQueries()).reachable);
  const auto graph = buildBimRoomGraph(model, scene, world);
  expectLowerPair(graph, scene);
  ASSERT_EQ(graph.portals.size(), 1u);
  const auto& portal = graph.portals.front();
  EXPECT_EQ(portal.evidence, "authored-space-boundary");
  EXPECT_EQ(portal.state, BimPortalState::Open);
  EXPECT_FALSE(portal.forward && portal.forward->result == BimWalkabilityFailure::None);
  EXPECT_FALSE(portal.reverse && portal.reverse->result == BimWalkabilityFailure::None);
}

} // namespace
