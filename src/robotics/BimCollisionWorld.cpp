#include "Container/robotics/BimCollisionWorld.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace container::robotics {
namespace {

physics::PhysicsWorldSettings worldSettings(size_t colliderCount, uint32_t workerThreads,
                                            uint32_t robotCapacity) {
  if (robotCapacity == 0 || robotCapacity > BimCollisionWorld::maxRobotCapacity)
    throw std::invalid_argument("BIM robot body capacity must be in [1, 64]");
  if (colliderCount > std::numeric_limits<uint32_t>::max() - robotCapacity)
    throw std::invalid_argument("BIM collision scene exceeds the physics body capacity");
  return {.gravity = {0, 0, -9.81f}, .maxBodies = static_cast<uint32_t>(colliderCount + robotCapacity),
          .workerThreads = workerThreads};
}

float finiteFloat(double x) {
  if (!std::isfinite(x) || std::abs(x) > std::numeric_limits<float>::max())
    throw std::invalid_argument("Collision query is outside the finite float displacement range");
  return static_cast<float>(x);
}

glm::vec3 narrowDisplacement(glm::dvec3 displacement) {
  return {finiteFloat(displacement.x), finiteFloat(displacement.y), finiteFloat(displacement.z)};
}

struct Capsule {
  float halfHeight;
  float radius;
  physics::PhysicsBodyPose pose;
};

Capsule capsuleAt(glm::dvec3 feet, double radius, double height) {
  if (!std::isfinite(height) || height < 2.0 * radius || radius <= 0.0)
    throw std::invalid_argument("A standing capsule requires a positive radius and a height of at least two radii");
  return {finiteFloat(height * 0.5 - radius), finiteFloat(radius),
          {feet + glm::dvec3(0, 0, height * 0.5), glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0))}};
}

}  // namespace

BimCollisionWorld::BimCollisionWorld(CookedBimCollision geometry, std::span<const std::string> openDoorIds,
                                     uint32_t workerThreads, uint32_t robotCapacity)
    : geometry_(std::move(geometry)), world_(worldSettings(geometry_.colliders.size(), workerThreads, robotCapacity)),
      rowBodies_(geometry_.colliders.size(), physics::kInvalidPhysicsBodyId), robotCapacity_(robotCapacity) {
  for (size_t index = 0; index < geometry_.colliders.size(); ++index) {
    const auto& row = geometry_.colliders[index];
    if (row.entityId.empty())
      throw std::invalid_argument("A BIM collider requires a semantic entity ID");
    if (row.kind == BimColliderKind::DoorMesh)
      doors_[row.entityId].rows.push_back(index);
  }
  for (const auto& id : openDoorIds) {
    const auto found = doors_.find(id);
    if (found == doors_.end())
      throw std::invalid_argument("No authored door collider for entity " + id);
    found->second.open = found->second.initiallyOpen = true;
  }
  bodyRows_.reserve(geometry_.colliders.size());
  for (size_t index = 0; index < geometry_.colliders.size(); ++index) {
    const auto& row = geometry_.colliders[index];
    if (row.kind != BimColliderKind::DoorMesh || !isDoorOpen(row.entityId))
      install(index);
  }
}

void BimCollisionWorld::acquireDriveOwner() {
  if (driveOwned_)
    throw std::invalid_argument("A differential drive or fleet is already attached to this BIM world");
  driveOwned_ = true;
}

void BimCollisionWorld::releaseDriveOwner() noexcept {
  while (!robots_.empty())
    detachRobot(robots_.begin()->first);
  driveOwned_ = false;
}

physics::PhysicsBodyId BimCollisionWorld::attachRobot(std::string robotId, float halfHeight, float radius,
                                                      physics::PhysicsBodyPose pose) {
  if (!driveOwned_)
    throw std::logic_error("A simulator must own the BIM world before attaching robots");
  if (robotId.empty() || robotBody(robotId) != physics::kInvalidPhysicsBodyId)
    throw std::invalid_argument("BIM robots require nonempty, unique IDs");
  if (std::ranges::any_of(geometry_.colliders, [&](const auto& row) { return row.entityId == robotId; }))
    throw std::invalid_argument("Robot identity collides with a BIM entity ID");
  if (robots_.size() >= robotCapacity_)
    throw std::invalid_argument("BIM collision world robot body capacity exhausted");
  const auto overlaps = world_.overlapCapsule(halfHeight, radius, pose);
  if (std::ranges::any_of(overlaps, [](const auto& hit) { return hit.penetrationDepth > 1e-6f; }))
    throw std::invalid_argument("BIM robot capsule overlaps existing geometry or a peer: " + robotId);

  const auto body = world_.createCapsule(halfHeight, radius,
      {.motionType = physics::PhysicsMotionType::Kinematic, .position = pose.position, .rotation = pose.rotation});
  try {
    robots_.emplace(body, RobotBody{std::move(robotId), halfHeight, radius});
  } catch (...) {
    (void)world_.destroyBody(body);
    throw;
  }
  return body;
}

void BimCollisionWorld::detachRobot(physics::PhysicsBodyId robot) noexcept {
  const auto found = robots_.find(robot);
  if (found == robots_.end())
    return;
  (void)world_.destroyBody(robot);
  robots_.erase(found);
}

void BimCollisionWorld::resetAuthoredBodies() {
  world_.clear();
  robots_.clear();
  bodyRows_.clear();
  std::ranges::fill(rowBodies_, physics::kInvalidPhysicsBodyId);
  for (auto& [id, door] : doors_)
    door.open = door.initiallyOpen;
  for (size_t index = 0; index < geometry_.colliders.size(); ++index) {
    const auto& row = geometry_.colliders[index];
    if (row.kind != BimColliderKind::DoorMesh || !isDoorOpen(row.entityId))
      install(index);
  }
}

bool BimCollisionWorld::isDoorOpen(std::string_view entityId) const {
  const auto found = doors_.find(entityId);
  if (found == doors_.end())
    throw std::invalid_argument("No authored door collider for entity " + std::string(entityId));
  return found->second.open;
}

void BimCollisionWorld::setDoorOpen(std::string_view entityId, bool open) {
  const auto found = doors_.find(entityId);
  if (found == doors_.end())
    throw std::invalid_argument("No authored door collider for entity " + std::string(entityId));
  auto& door = found->second;
  if (door.open == open)
    return;
  if (open) {
    for (const auto row : door.rows)
      uninstall(row);
  } else {
    size_t installed = 0;
    try {
      for (const auto row : door.rows) {
        install(row);
        ++installed;
      }
      const auto occupiesOpening = [&](const physics::PhysicsQueryHit& hit) {
        return hit.penetrationDepth > 1e-6f && std::ranges::any_of(door.rows, [&](size_t row) {
          return rowBodies_[row] == hit.body;
        });
      };
      for (const auto& [body, robot] : robots_) {
        const auto pose = world_.pose(body);
        if (!pose) continue;
        const std::array ignored{body};
        const auto contacts = world_.overlapCapsule(robot.halfHeight, robot.radius, *pose, ignored);
        if (std::ranges::any_of(contacts, occupiesOpening))
          throw std::invalid_argument("Cannot close a BIM door occupied by the robot " + robot.entityId +
                                      ": " + std::string(entityId));
      }
    } catch (...) {
      for (size_t i = 0; i < installed; ++i)
        uninstall(door.rows[i]);
      throw;
    }
  }
  door.open = open;
}

void BimCollisionWorld::install(size_t index) {
  const auto& row = geometry_.colliders.at(index);
  physics::PhysicsBodySettings settings{.motionType = physics::PhysicsMotionType::Static,
                                       .position = row.originMeters};
  physics::PhysicsBodyId body;
  if (row.kind == BimColliderKind::DynamicBox) {
    settings.motionType = physics::PhysicsMotionType::Dynamic;
    body = world_.createBox(row.halfExtents, settings);
  } else {
    if (!row.mesh)
      throw std::invalid_argument("No cooked triangle data for BIM entity " + row.entityId);
    auto shape = meshShapes_.find(row.mesh.get());
    if (shape == meshShapes_.end()) {
      shape = meshShapes_.emplace(row.mesh.get(),
          physics::PhysicsWorld::cookStaticMesh(row.mesh->vertices, row.mesh->indices)).first;
    }
    body = world_.createStaticMesh(shape->second, settings);
  }
  try {
    bodyRows_.emplace(body, index);
    rowBodies_[index] = body;
  } catch (...) {
    (void)world_.destroyBody(body);
    throw;
  }
}

void BimCollisionWorld::uninstall(size_t index) noexcept {
  const auto body = rowBodies_[index];
  (void)world_.destroyBody(body);
  bodyRows_.erase(body);
  rowBodies_[index] = physics::kInvalidPhysicsBodyId;
}

physics::PhysicsBodyId BimCollisionWorld::robotBody(std::string_view robotId) const noexcept {
  const auto found = std::ranges::find_if(robots_, [&](const auto& entry) { return entry.second.entityId == robotId; });
  return found == robots_.end() ? physics::kInvalidPhysicsBodyId : found->first;
}

std::optional<physics::PhysicsBodyPose> BimCollisionWorld::robotPose(std::string_view robotId) const {
  return world_.pose(robotBody(robotId));
}

BimCollisionWorld::ColliderIdentity BimCollisionWorld::colliderIdentity(physics::PhysicsBodyId body) const {
  if (const auto robot = robots_.find(body); robot != robots_.end())
    return {robot->second.entityId, false};
  const auto& row = geometry_.colliders[bodyRows_.at(body)];
  return {row.entityId, row.supportCandidate};
}

std::optional<LidarHit> BimCollisionWorld::castLidar(glm::dvec3 origin, glm::dvec3 displacement,
                                                    physics::PhysicsBodyId ignored) const {
  const std::array ignore{ignored};
  const auto hit = world_.rayCast(origin, narrowDisplacement(displacement), ignore);
  if (!hit) return std::nullopt;
  return LidarHit{hit->fraction, std::string(colliderIdentity(hit->body).entityId)};
}

LidarCast BimCollisionWorld::lidarRaycasts() const {
  return [this](glm::dvec3 origin, glm::dvec3 displacement) {
    const auto ignored = robots_.size() == 1 ? robots_.begin()->first : physics::kInvalidPhysicsBodyId;
    return castLidar(origin, displacement, ignored);
  };
}

LidarCast BimCollisionWorld::lidarRaycasts(std::string_view ignoredRobotId) const {
  return [this, id = std::string(ignoredRobotId)](glm::dvec3 origin, glm::dvec3 displacement) {
    return castLidar(origin, displacement, robotBody(id));
  };
}

template<class IgnoredBodies>
BimWalkabilityQueries BimCollisionWorld::walkabilityQueriesWithFilter(IgnoredBodies ignoredBodies) const {
  const auto describe = [this](const physics::PhysicsQueryHit& hit) {
    const auto collider = colliderIdentity(hit.body);
    return BimWalkabilityHit{.fraction = std::clamp<double>(hit.fraction, 0.0, 1.0),
                             .position = hit.position, .normal = glm::dvec3(hit.normal),
                             .entityId = std::string(collider.entityId),
                             .walkableSupport = collider.walkableSupport};
  };
  // An idle capsule can touch a support without penetrating it. Ignore only
  // harmless initial contacts, retaining any farther obstacle in the sweep.
  constexpr double tolerance = 1e-6;
  return {
      .castSegment = [this, describe, ignoredBodies](glm::dvec3 from,
                                                     glm::dvec3 displacement) -> std::optional<BimWalkabilityHit> {
        const auto hit = world_.rayCast(from, narrowDisplacement(displacement), ignoredBodies());
        return hit ? std::optional(describe(*hit)) : std::nullopt;
      },
      .sweepCapsule = [this, describe, ignoredBodies](glm::dvec3 bottom, glm::dvec3 displacement, double radius,
                                                     double height) -> std::optional<BimWalkabilityHit> {
        const auto capsule = capsuleAt(bottom, radius, height);
        const auto hits = world_.sweepCapsule(capsule.halfHeight, capsule.radius, capsule.pose,
                                             narrowDisplacement(displacement), ignoredBodies());
        for (const auto& hit : hits) {
          if (hit.fraction > tolerance || hit.penetrationDepth > tolerance ||
              glm::dot(glm::dvec3(hit.normal), displacement) < -tolerance)
            return describe(hit);
        }
        return std::nullopt;
      },
      .overlapCapsule = [this, describe, ignoredBodies](glm::dvec3 bottom, double radius,
                                                       double height) -> std::optional<BimWalkabilityHit> {
        const auto capsule = capsuleAt(bottom, radius, height);
        const auto hits = world_.overlapCapsule(capsule.halfHeight, capsule.radius, capsule.pose, ignoredBodies());
        for (const auto& hit : hits)
          if (hit.penetrationDepth > tolerance)
            return describe(hit);
        return std::nullopt;
      }};
}

BimWalkabilityQueries BimCollisionWorld::walkabilityQueries() const {
  return walkabilityQueries(physics::kInvalidPhysicsBodyId);
}

BimWalkabilityQueries BimCollisionWorld::walkabilityQueries(std::string_view ignoredRobotId) const {
  return walkabilityQueries(robotBody(ignoredRobotId));
}

BimWalkabilityQueries BimCollisionWorld::walkabilityQueries(physics::PhysicsBodyId ignored) const {
  return walkabilityQueriesWithFilter([ignore = std::array{ignored}] {
    return std::span<const physics::PhysicsBodyId>(ignore);
  });
}

BimWalkabilityQueries BimCollisionWorld::fleetWalkabilityQueries() const {
  auto ignore = std::make_shared<std::vector<physics::PhysicsBodyId>>();
  ignore->reserve(robots_.size());
  for (const auto& entry : robots_)
    ignore->push_back(entry.first);
  return walkabilityQueriesWithFilter([ignore = std::move(ignore)] {
    return std::span<const physics::PhysicsBodyId>(*ignore);
  });
}

}  // namespace container::robotics
