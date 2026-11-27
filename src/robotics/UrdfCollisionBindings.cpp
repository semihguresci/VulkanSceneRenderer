#include "Container/robotics/UrdfCollisionBindings.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace container::robotics {
namespace {

struct Primitive {
  enum class Type { Box, Cylinder, Sphere };
  Type type;
  glm::vec3 dimensions;
};

float positiveLength(double value) {
  if (!std::isfinite(value) || value <= 0 || value > std::numeric_limits<float>::max() ||
      static_cast<float>(value) == 0)
    throw std::invalid_argument("URDF collision size is outside the Jolt shape range");
  return static_cast<float>(value);
}

Primitive primitiveFor(const UrdfLink& link, const UrdfCollision& collision) {
  if (const auto* box = std::get_if<UrdfBox>(&collision.geometry)) {
    return {Primitive::Type::Box, {positiveLength(box->size.x * .5),
            positiveLength(box->size.y * .5), positiveLength(box->size.z * .5)}};
  }
  if (const auto* cylinder = std::get_if<UrdfCylinder>(&collision.geometry)) {
    return {Primitive::Type::Cylinder, {positiveLength(cylinder->length * .5),
                                          positiveLength(cylinder->radius), 0}};
  }
  if (const auto* sphere = std::get_if<UrdfSphere>(&collision.geometry))
    return {Primitive::Type::Sphere, {positiveLength(sphere->radius), 0, 0}};
  throw std::invalid_argument("URDF link '" + link.name + "' collision mesh '" +
                              std::get<UrdfMesh>(collision.geometry).uri +
                              "' needs a moving mesh adapter before Jolt binding");
}

physics::PhysicsBodyPose bodyPose(const UrdfRuntime& robot, size_t linkIndex, size_t collisionIndex) {
  const auto transform = robot.worldFromCollision(linkIndex, collisionIndex);
  auto rotation = glm::quat_cast(glm::dmat3(transform));
  const auto& geometry = robot.model().links[linkIndex].collisions[collisionIndex].geometry;
  if (std::holds_alternative<UrdfCylinder>(geometry)) {
    // Jolt's primitive is Y-axial. URDF's cylinder axis is local Z.
    rotation *= glm::angleAxis(glm::half_pi<double>(), glm::dvec3(1, 0, 0));
  }
  return physics::PhysicsWorld::validatePose({glm::dvec3(transform[3]), glm::quat(rotation)});
}

physics::PhysicsBodyId createBody(physics::PhysicsWorld& world, const Primitive& primitive,
                                   const physics::PhysicsBodyPose& pose) {
  const physics::PhysicsBodySettings settings{
      .motionType = physics::PhysicsMotionType::Kinematic,
      .position = pose.position, .rotation = pose.rotation};
  switch (primitive.type) {
  case Primitive::Type::Box: return world.createBox(primitive.dimensions, settings);
  case Primitive::Type::Cylinder: return world.createCylinder(primitive.dimensions.x, primitive.dimensions.y, settings);
  case Primitive::Type::Sphere: return world.createSphere(primitive.dimensions.x, settings);
  }
  throw std::logic_error("Unknown URDF collision primitive");
}

} // namespace

UrdfCollisionBindings::UrdfCollisionBindings(physics::PhysicsWorld& world, const UrdfRuntime& robot)
    : world_(world), robot_(robot) {
  struct Pending {
    size_t link;
    size_t collision;
    Primitive primitive;
    physics::PhysicsBodyPose pose;
  };
  std::vector<Pending> prepared;
  for (size_t link = 0; link < robot.model().links.size(); ++link) {
    const auto& definition = robot.model().links[link];
    for (size_t collision = 0; collision < definition.collisions.size(); ++collision) {
      prepared.push_back({link, collision, primitiveFor(definition, definition.collisions[collision]),
                          bodyPose(robot_, link, collision)});
    }
  }
  bodies_.reserve(prepared.size());
  try {
    for (const auto& item : prepared)
      bodies_.push_back({item.link, item.collision, createBody(world_, item.primitive, item.pose)});
  } catch (...) {
    for (auto it = bodies_.rbegin(); it != bodies_.rend(); ++it)
      (void)world_.destroyBody(it->body);
    throw;
  }
}

UrdfCollisionBindings::~UrdfCollisionBindings() {
  for (auto it = bodies_.rbegin(); it != bodies_.rend(); ++it)
    (void)world_.destroyBody(it->body);
}

void UrdfCollisionBindings::synchronize() {
  std::vector<physics::PhysicsBodyPose> next;
  next.reserve(bodies_.size());
  for (const auto& body : bodies_) {
    if (!world_.contains(body.body))
      throw std::logic_error("URDF collision body was removed from its owning world");
    next.push_back(bodyPose(robot_, body.link, body.collision));
  }
  for (size_t i = 0; i < bodies_.size(); ++i)
    if (!world_.setPose(bodies_[i].body, next[i]))
      throw std::logic_error("Unable to update the URDF collision body");
}

} // namespace container::robotics
