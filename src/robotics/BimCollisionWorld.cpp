#include "Container/robotics/BimCollisionWorld.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::robotics {
namespace {

physics::PhysicsWorldSettings worldSettings(size_t colliderCount, uint32_t workerThreads) {
  if (colliderCount >= std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("BIM collision scene exceeds the physics body capacity");
  return {.gravity = {0, 0, -9.81f}, .maxBodies = static_cast<uint32_t>(colliderCount + 1),
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
                                     uint32_t workerThreads)
    : geometry_(std::move(geometry)), world_(worldSettings(geometry_.colliders.size(), workerThreads)),
      rowBodies_(geometry_.colliders.size(), physics::kInvalidPhysicsBodyId) {
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

void BimCollisionWorld::resetAuthoredBodies() {
  world_.clear();
  robotBody_ = physics::kInvalidPhysicsBodyId;
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
      if (const auto robotPose = world_.pose(robotBody_)) {
        const std::array ignored{robotBody_};
        const auto contacts = world_.overlapCapsule(robotHalfHeight_, robotRadius_, *robotPose, ignored);
        const auto occupiesOpening = [&](const physics::PhysicsQueryHit& hit) {
          return hit.penetrationDepth > 1e-6f && std::ranges::any_of(door.rows, [&](size_t row) {
            return rowBodies_[row] == hit.body;
          });
        };
        if (std::ranges::any_of(contacts, occupiesOpening))
          throw std::invalid_argument("Cannot close a BIM door occupied by the robot: " + std::string(entityId));
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

BimWalkabilityQueries BimCollisionWorld::walkabilityQueries() const {
  return walkabilityQueries(physics::kInvalidPhysicsBodyId);
}

BimWalkabilityQueries BimCollisionWorld::walkabilityQueries(physics::PhysicsBodyId ignored) const {
  const auto describe = [this](const physics::PhysicsQueryHit& hit) {
    const auto* row = hit.body == robotBody_ ? nullptr : &geometry_.colliders[bodyRows_.at(hit.body)];
    return BimWalkabilityHit{.fraction = std::clamp<double>(hit.fraction, 0.0, 1.0),
                             .position = hit.position, .normal = glm::dvec3(hit.normal),
                             .entityId = row ? row->entityId : robotEntityId_,
                             .walkableSupport = row && row->supportCandidate};
  };
  // An idle capsule can touch a support without penetrating it. Ignore only
  // harmless initial contacts, retaining any farther obstacle in the sweep.
  constexpr double tolerance = 1e-6;
  const std::array ignore{ignored};
  return {
      .castSegment = [this, describe, ignore](glm::dvec3 from, glm::dvec3 displacement) -> std::optional<BimWalkabilityHit> {
        const auto hit = world_.rayCast(from, narrowDisplacement(displacement), ignore);
        return hit ? std::optional(describe(*hit)) : std::nullopt;
      },
      .sweepCapsule = [this, describe, ignore](glm::dvec3 bottom, glm::dvec3 displacement, double radius,
                                      double height) -> std::optional<BimWalkabilityHit> {
        const auto capsule = capsuleAt(bottom, radius, height);
        const auto hits = world_.sweepCapsule(capsule.halfHeight, capsule.radius, capsule.pose,
                                             narrowDisplacement(displacement), ignore);
        for (const auto& hit : hits) {
          if (hit.fraction > tolerance || hit.penetrationDepth > tolerance ||
              glm::dot(glm::dvec3(hit.normal), displacement) < -tolerance)
            return describe(hit);
        }
        return std::nullopt;
      },
      .overlapCapsule = [this, describe, ignore](glm::dvec3 bottom, double radius,
                                        double height) -> std::optional<BimWalkabilityHit> {
        const auto capsule = capsuleAt(bottom, radius, height);
        const auto hits = world_.overlapCapsule(capsule.halfHeight, capsule.radius, capsule.pose, ignore);
        for (const auto& hit : hits)
          if (hit.penetrationDepth > tolerance)
            return describe(hit);
        return std::nullopt;
      }};
}

}  // namespace container::robotics
