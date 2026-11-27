#include "Container/robotics/BimRoomGraph.h"

#include "BimRoomGeometry.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/geometry/DotBimLoader.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace container::robotics {
namespace {

using room_geometry::Bounds;
using room_geometry::Volume;
using IdSet = std::set<std::string, std::less<>>;
using Links = std::map<std::string, IdSet, std::less<>>;

struct Room {
  const SemanticEntity* entity;
  std::optional<Volume> volume;
};

struct Side {
  const Room* room;
  glm::dvec3 point;
};

struct Probe {
  std::array<Side, 2> sides;
};

const IdSet& links(const Links& index, std::string_view target) {
  static const IdSet empty;
  const auto found = index.find(target);
  return found == index.end() ? empty : found->second;
}

bool sameStorey(const SemanticEntity& first, const SemanticEntity& second) {
  return first.storeyIds.size() == 1 && second.storeyIds == first.storeyIds;
}

std::optional<Bounds> apertureBounds(const geometry::dotbim::Model& model,
                                     const SemanticEntity& aperture) {
  if (aperture.roboticsBounds)
    return Bounds{aperture.roboticsBounds->minimum, aperture.roboticsBounds->maximum};
  if (const auto geometry = room_geometry::loadVolume(model, aperture))
    return geometry->bounds;
  return std::nullopt;
}

std::optional<glm::dvec3> crossingAxis(const SemanticEntity& door) {
  if (!door.roboticsWorldFromLocal)
    return std::nullopt;
  const glm::dvec3 normal((*door.roboticsWorldFromLocal)[1]);
  const auto horizontal = glm::dvec3(normal.x, normal.y, 0.0);
  const double length = glm::length(horizontal);
  if (!std::isfinite(length) || length == 0.0 || std::abs(normal.z) > length * .1)
    return std::nullopt;
  return horizontal / length;
}

bool inBounds(const Bounds& bounds, glm::dvec3 point) {
  constexpr double epsilon = 1e-5;
  for (glm::length_t axis = 0; axis < 3; ++axis)
    if (point[axis] < bounds.minimum[axis] - epsilon ||
        point[axis] > bounds.maximum[axis] + epsilon)
      return false;
  return true;
}

std::optional<Side> nearSide(const std::vector<Room>& rooms, const IdSet& candidates,
                             const SemanticEntity& storeyReference, glm::dvec3 origin,
                             glm::dvec3 direction, double portalDepth,
                             double clearance) {
  constexpr double boundaryTolerance = .025;
  constexpr double roomApproachTolerance = .5;
  std::optional<Side> result;
  for (const auto& room : rooms) {
    if ((!candidates.empty() && !candidates.contains(room.entity->id)) || !room.volume ||
        !room.volume->usableInterior ||
        room.entity->storeyIds.size() != 1 ||
        (!storeyReference.storeyIds.empty() && !sameStorey(*room.entity, storeyReference)))
      continue;
    const auto& bounds = room.volume->bounds;
    if (origin.z < bounds.minimum.z || origin.z > bounds.maximum.z)
      continue;
    const glm::dvec3 center = bounds.minimum + (bounds.maximum - bounds.minimum) * .5;
    const auto extent = (bounds.maximum - bounds.minimum) * .5;
    const glm::dvec3 across(-direction.y, direction.x, 0);
    if (std::abs(glm::dot(center - origin, across)) > glm::dot(extent, glm::abs(across)))
      continue;
    if (glm::dot(center - origin, direction) - glm::dot(extent, glm::abs(direction)) >
        portalDepth + roomApproachTolerance)
      continue;
    for (const auto& [entry, exit] : room_geometry::interiorIntervals(*room.volume, origin, direction)) {
      if (entry < -boundaryTolerance || entry > portalDepth + roomApproachTolerance)
        continue;
      const double anchor = std::max(entry, 0.0) + clearance;
      if (anchor > exit - clearance)
        continue;
      if (result)
        return std::nullopt;
      result = Side{&room, origin + direction * anchor};
      break;
    }
  }
  return result;
}

std::optional<glm::dvec3> floorAt(const BimWalkabilityQueries& queries, const Side& side,
                                 const Bounds& aperture, glm::dvec3 direction,
                                 const BimWalkabilitySettings& agent) {
  const auto& volume = *side.room->volume;
  const auto& room = volume.bounds;
  const double allowedChange = std::max(agent.maxStepHeight, agent.maxDropHeight);
  const double minHeight = std::max(room.minimum.z, aperture.minimum.z - allowedChange);
  const auto ray = queries.castSegment(
      side.point, glm::dvec3(0.0, 0.0, minHeight - side.point.z - agent.elevationTolerance));
  if (!ray || !ray->walkableSupport ||
      ray->position.z < minHeight - agent.elevationTolerance ||
      ray->position.z > room.maximum.z)
    return std::nullopt;
  // A space may taper or move sideways with height. Certify the anchor
  // again after moving it down to the physical floor.
  for (const auto rise : {std::clamp(agent.skin, .001, .02), agent.height * .5}) {
    const auto intervals = room_geometry::interiorIntervals(
        volume, ray->position + glm::dvec3(0, 0, rise), direction);
    if (!std::ranges::any_of(intervals, [](const auto& interval) {
          return interval.first < 0 && interval.second > 0;
        }))
      return std::nullopt;
  }
  return ray->position;
}

void testDirection(std::optional<BimPortalCrossing>& result, glm::dvec3 start, glm::dvec3 end,
                   const BimWalkabilityQueries& queries, const BimWalkabilitySettings& agent) {
  if (result && result->result == BimWalkabilityFailure::None)
    return;
  const auto walk = traceWalkablePath(start, end, queries, agent);
  if (!result || walk.reachable)
    result.emplace(BimPortalCrossing{start, end, walk.reason});
}

} // namespace

BimRoomGraph buildBimRoomGraph(const geometry::dotbim::Model& model, const RoboticsScene& scene,
                              const BimCollisionWorld& world, std::span<const std::string> lockedDoorIds,
                              const BimWalkabilitySettings& agent) {
  if (!std::isfinite(agent.radius) || agent.radius <= 0 || !std::isfinite(agent.skin) ||
      agent.skin <= 0 || !std::isfinite(agent.height) || agent.height < 2.0 * agent.radius)
    throw std::invalid_argument("BIM room graph requires a finite standing capsule");
  BimRoomGraph graph;
  std::vector<Room> rooms;
  std::map<std::string, const SemanticEntity*, std::less<>> entities;
  IdSet doorColliders, staticColliders;
  for (const auto& collider : world.geometry().colliders) {
    if (collider.kind == BimColliderKind::DoorMesh)
      doorColliders.insert(collider.entityId);
    if (collider.kind == BimColliderKind::StaticMesh)
      staticColliders.insert(collider.entityId);
  }
  const auto diagnostic = [&](std::string code, std::string id, std::string message) {
    graph.diagnostics.push_back({std::move(code), std::move(id), std::move(message)});
  };
  if (!scene.frame.roboticsFromRenderer)
    diagnostic("metric-units-unavailable", {}, "Room geometry and traversal require known source units.");
  for (const auto& entity : scene.entities) {
    entities.emplace(entity.id, &entity);
    if (entity.kind != SemanticKind::Space)
      continue;
    auto volume = room_geometry::loadVolume(model, entity);
    const bool hasVolume = volume && volume->usableInterior;
    if (!hasVolume)
      diagnostic("space-volume-unavailable", entity.id, "IfcSpace has no closed metric triangle volume for portal probing.");
    if (entity.storeyIds.size() != 1)
      diagnostic("space-storey-unavailable", entity.id, "IfcSpace needs a single authored storey for room pairing.");
    graph.rooms.push_back({entity.id, entity.guid, entity.name, entity.storeyIds, hasVolume});
    rooms.push_back({&entity, std::move(volume)});
  }
  if (rooms.empty())
    diagnostic("no-spaces", {}, "No authored IfcSpace products are available for a room graph.");
  const IdSet locked(lockedDoorIds.begin(), lockedDoorIds.end());
  for (const auto& id : locked) {
    const auto found = entities.find(id);
    if (found == entities.end() || found->second->kind != SemanticKind::Door)
      throw std::invalid_argument("No authored door for locked room graph ID " + id);
    if (doorColliders.contains(id) && world.isDoorOpen(id))
      throw std::invalid_argument("A door cannot be open and locked in the same room graph " + id);
  }

  Links boundaries, openings, hosts;
  for (const auto& link : scene.relationships) {
    if (link.kind == SemanticRelationshipKind::SpaceBoundary &&
        entities.at(link.from)->kind == SemanticKind::Space)
      boundaries[link.to].insert(link.from);
    if (link.kind == SemanticRelationshipKind::Fills &&
        entities.at(link.from)->kind == SemanticKind::Opening)
      openings[link.to].insert(link.from);
    if (link.kind == SemanticRelationshipKind::Voids)
      hosts[link.to].insert(link.from);
  }

  const auto queries = world.walkabilityQueries();
  for (const auto& [id, door] : entities) {
    if (door->kind != SemanticKind::Door)
      continue;
    const auto& openingIds = links(openings, id);
    const auto* opening = openingIds.size() == 1 ? entities.at(*openingIds.begin()) : nullptr;
    const auto& hostIds = links(hosts, opening ? std::string_view(opening->id) : std::string_view{});
    const auto* host = hostIds.size() == 1 ? entities.at(*hostIds.begin()) : nullptr;
    if (!opening || !host)
      diagnostic("door-opening-host-unavailable", id, "A unique authored door fill and opening host are required to certify a passage.");
    IdSet directRooms = links(boundaries, id);
    if (opening) {
      const auto& openingRooms = links(boundaries, opening->id);
      directRooms.insert(openingRooms.begin(), openingRooms.end());
    }
    BimRoomPortal portal;
    portal.doorId = id;
    portal.doorGuid = door->guid;
    portal.openingId = opening ? opening->id : "";
    portal.hostId = host ? host->id : "";
    portal.state = locked.contains(id) ? BimPortalState::Locked :
        doorColliders.contains(id) && world.isDoorOpen(id) ? BimPortalState::Open : BimPortalState::Closed;
    if (directRooms.size() == 2) {
      const auto& first = *entities.at(*directRooms.begin());
      const auto& second = *entities.at(*directRooms.rbegin());
      if (!sameStorey(first, second) || (!door->storeyIds.empty() && !sameStorey(first, *door))) {
        diagnostic("door-storey-mismatch", id, "Explicit room boundaries disagree with each other or with the door storey.");
        continue;
      }
      portal.roomIds = {first.id, second.id};
      portal.evidence = "authored-space-boundary";
    } else if (directRooms.size() > 2) {
      diagnostic("ambiguous-door-boundaries", id, "More than two rooms are explicitly associated with this door or opening.");
      continue;
    }

    const auto bounds = opening ? apertureBounds(model, *opening) : std::nullopt;
    const auto axis = crossingAxis(*door);
    std::vector<Probe> probes;
    if (host && bounds && axis) {
      const auto leaf = apertureBounds(model, *door);
      const auto lateral = glm::dvec3(-axis->y, axis->x, 0);
      const auto extent = (bounds->maximum - bounds->minimum) * .5;
      const auto center = bounds->minimum + extent;
      const double depth = glm::dot(extent, glm::abs(*axis));
      const double width = glm::dot(extent, glm::abs(lateral));
      const double clearance = agent.radius + agent.skin;
      const auto& hostRooms = links(boundaries, host->id);
      IdSet candidates;
      if (directRooms.size() == 2)
        candidates = directRooms;
      else if (hostRooms.size() >= 2 && (directRooms.empty() || hostRooms.contains(*directRooms.begin())))
        candidates = hostRooms;
      const auto* storeyReference = !door->storeyIds.empty() ? door :
          !opening->storeyIds.empty() ? opening : host;
      for (const double offset : {0., -.25, .25, -.4, .4}) {
        const auto origin = center + lateral * (2.0 * width * offset);
        // A world AABB is conservative for a rotated aperture. Restrict
        // secondary probes to the door leaf's authored lateral envelope.
        if (leaf && !inBounds(*leaf, {origin.x, origin.y, leaf->minimum.z}))
          continue;
        const auto first = nearSide(rooms, candidates, *storeyReference, origin, -*axis, depth, clearance);
        const auto second = nearSide(rooms, candidates, *storeyReference, origin, *axis, depth, clearance);
        if (!first || !second || !sameStorey(*first->room->entity, *second->room->entity))
          continue;
        std::array pair{first->room->entity->id, second->room->entity->id};
        if (pair[0] == pair[1] ||
            (!directRooms.empty() && !directRooms.contains(pair[0]) && !directRooms.contains(pair[1])))
          continue;
        Probe probe{{*first, *second}};
        if (pair[1] < pair[0]) {
          std::swap(pair[0], pair[1]);
          std::swap(probe.sides[0], probe.sides[1]);
        }
        if (portal.roomIds[0].empty()) {
          portal.roomIds = pair;
          portal.evidence = "opening-and-space-volumes";
        } else if (portal.roomIds != pair) {
          diagnostic("ambiguous-door-volume-pair", id, "Separate portal probes associate this opening with conflicting room pairs.");
          probes.clear();
          portal.roomIds = {};
          break;
        }
        probes.push_back(std::move(probe));
      }
    }
    if (portal.roomIds[0].empty()) {
      diagnostic("room-pair-unavailable", id, "No unique same-storey pair of authored room boundaries or opposite space volumes was found.");
      continue;
    }
    if (portal.state == BimPortalState::Open) {
      if (host && staticColliders.contains(host->id)) {
        for (const auto& probe : probes) {
          const auto start = floorAt(queries, probe.sides[0], *bounds, *axis, agent);
          const auto end = floorAt(queries, probe.sides[1], *bounds, *axis, agent);
          if (start && end) {
            testDirection(portal.forward, *start, *end, queries, agent);
            testDirection(portal.reverse, *end, *start, queries, agent);
          }
        }
      }
      if (!portal.forward)
        diagnostic("portal-clearance-unavailable", id, "The open door lacks a collision host or supported anchors within both room volumes.");
    }
    graph.portals.push_back(std::move(portal));
  }
  std::ranges::sort(graph.rooms, {}, &BimRoom::id);
  std::ranges::sort(graph.diagnostics, [](const auto& first, const auto& second) {
    return std::tie(first.code, first.sourceId, first.message) < std::tie(second.code, second.sourceId, second.message);
  });
  return graph;
}

} // namespace container::robotics
