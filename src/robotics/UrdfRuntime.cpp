#include "Container/robotics/UrdfRuntime.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace container::robotics {
namespace {

[[noreturn]] void invalidJoint(const UrdfJoint& joint, const char* reason) {
  throw std::invalid_argument("URDF joint '" + joint.name + "': " + reason);
}

glm::dvec3 jointAxis(const UrdfJoint& joint) {
  const auto axis = joint.axis;
  if (!std::isfinite(axis.x) || !std::isfinite(axis.y) || !std::isfinite(axis.z))
    invalidJoint(joint, "axis must be finite");
  const auto scale = std::max({std::abs(axis.x), std::abs(axis.y), std::abs(axis.z)});
  if (scale == 0)
    invalidJoint(joint, "axis must be nonzero");
  // Scale first so finite authored axes do not overflow or underflow the norm.
  const auto scaled = axis / scale;
  return scaled / glm::length(scaled);
}

bool bounded(const UrdfJoint& joint) {
  return joint.type == UrdfJointType::Revolute || joint.type == UrdfJointType::Prismatic;
}

void validateJoint(const UrdfJoint& joint) {
  switch (joint.type) {
  case UrdfJointType::Fixed:
  case UrdfJointType::Revolute:
  case UrdfJointType::Continuous:
  case UrdfJointType::Prismatic:
    break;
  default:
    invalidJoint(joint, "unsupported joint type");
  }
  if (joint.type != UrdfJointType::Fixed)
    (void)jointAxis(joint);
  if (bounded(joint) && (!joint.limits || !joint.limits->lower || !joint.limits->upper))
    invalidJoint(joint, "bounded joint requires lower and upper limits");
  if (joint.limits) {
    const auto& limits = *joint.limits;
    if ((limits.lower && !std::isfinite(*limits.lower)) ||
        (limits.upper && !std::isfinite(*limits.upper)) ||
        !std::isfinite(limits.velocity) || limits.velocity < 0 ||
        !std::isfinite(limits.effort) || limits.effort < 0)
      invalidJoint(joint, "limits must be finite with nonnegative velocity and effort");
    if (bounded(joint) && *limits.lower > *limits.upper)
      invalidJoint(joint, "lower limit exceeds upper limit");
  }
  (void)urdfPoseMatrix(joint.parentFromJoint);
}

void validatePosition(const UrdfJoint& joint, double position) {
  if (!std::isfinite(position))
    invalidJoint(joint, "position must be finite");
  if (bounded(joint) &&
      (position < *joint.limits->lower || position > *joint.limits->upper))
    invalidJoint(joint, "position is outside the authored limits");
}

void requireFinite(const glm::dmat4& matrix) {
  for (glm::length_t column = 0; column < 4; ++column)
    for (glm::length_t row = 0; row < 4; ++row)
      if (!std::isfinite(matrix[column][row]))
        throw std::invalid_argument("URDF world transform exceeds the finite numeric range");
}

glm::dmat4 compose(const glm::dmat4& first, const glm::dmat4& second) {
  const auto result = first * second;
  requireFinite(result);
  return result;
}

glm::dmat4 jointMotion(const UrdfJoint& joint, double position) {
  switch (joint.type) {
  case UrdfJointType::Fixed:
    return glm::dmat4(1.0);
  case UrdfJointType::Prismatic:
    return glm::translate(glm::dmat4(1.0), jointAxis(joint) * position);
  case UrdfJointType::Revolute:
  case UrdfJointType::Continuous:
    // State retains the authored, unwrapped coordinate. Trigonometry alone is
    // periodic, including when a large finite continuous coordinate is used.
    return glm::mat4_cast(glm::angleAxis(std::remainder(position, 2 * std::numbers::pi),
                                         jointAxis(joint)));
  }
  invalidJoint(joint, "unsupported joint type");
}

double approach(double position, double target, double distance) {
  // The subtraction may overflow for opposite extreme endpoints, but the
  // comparison then still correctly selects the bounded, finite step.
  if (target > position)
    return target - position <= distance ? target : std::min(target, position + distance);
  if (target < position)
    return position - target <= distance ? target : std::max(target, position - distance);
  return position;
}

double actualVelocity(const UrdfJoint& joint, double previous, double next, double dt) {
  const auto velocity = (next - previous) / dt;
  if (!std::isfinite(velocity))
    invalidJoint(joint, "requested tick velocity exceeds the finite numeric range");
  return velocity;
}

} // namespace

UrdfRuntime::UrdfRuntime(UrdfModel model, UrdfPose worldFromRoot,
                         uint32_t ticksPerSecond, uint32_t maxTicksPerAdvance)
    : model_(std::move(model)), clock_(ticksPerSecond, maxTicksPerAdvance),
      initialRoot_(urdfPoseMatrix(worldFromRoot)), root_(initialRoot_) {
  validateUrdfTopology(model_);
  initialJoints_.resize(model_.joints.size());
  for (size_t index = 0; index < model_.joints.size(); ++index) {
    const auto& joint = model_.joints[index];
    validateJoint(joint);
    const double initial = bounded(joint) ?
        std::clamp(0.0, *joint.limits->lower, *joint.limits->upper) : 0.0;
    initialJoints_[index] = {.position = initial, .target = initial};
  }
  joints_ = initialJoints_;
  worldFromLinks_ = placeLinks(root_, joints_);
}

std::vector<UrdfJointState> UrdfRuntime::validated(std::span<const UrdfJointValue> values,
                                                bool direct) const {
  auto result = joints_;
  std::unordered_map<std::string_view, size_t> named;
  named.reserve(model_.joints.size());
  for (size_t index = 0; index < model_.joints.size(); ++index)
    named.emplace(model_.joints[index].name, index);
  std::unordered_set<size_t> assigned;
  assigned.reserve(std::min(values.size(), model_.joints.size()));
  for (const auto& value : values) {
    const auto found = named.find(value.joint);
    if (found == named.end())
      throw std::invalid_argument("Unknown URDF joint '" + std::string(value.joint) + "'");
    const auto index = found->second;
    const auto& joint = model_.joints[index];
    if (!assigned.insert(index).second)
      invalidJoint(joint, "joint is repeated in a command batch");
    if (joint.type == UrdfJointType::Fixed)
      invalidJoint(joint, "fixed joints are not commandable");
    validatePosition(joint, value.position);
    if (direct) {
      result[index].position = value.position;
      result[index].velocity = 0;
    } else if (!joint.limits) {
      // An unlimited next-tick target is still required to yield a finite
      // reported velocity. Reject it before changing any target in the batch.
      (void)actualVelocity(joint, result[index].position, value.position, clock_.tickSeconds());
    }
    result[index].target = value.position;
  }
  return result;
}

std::vector<glm::dmat4> UrdfRuntime::placeLinks(const glm::dmat4& root,
                                              std::span<const UrdfJointState> joints) const {
  requireFinite(root);
  if (joints.size() != model_.joints.size())
    throw std::invalid_argument("URDF joint state does not match its articulation model");
  std::vector<glm::dmat4> result(model_.links.size(), glm::dmat4(1.0));
  result[model_.rootLink] = root;
  for (const auto index : model_.jointOrder) {
    const auto& joint = model_.joints[index];
    validatePosition(joint, joints[index].position);
    const auto parentFromChild = compose(urdfPoseMatrix(joint.parentFromJoint),
                                         jointMotion(joint, joints[index].position));
    result[joint.childLink] = compose(result[joint.parentLink], parentFromChild);
  }
  // Preflight the separate authored geometry and COM frames too. A state
  // transaction must never publish links whose projections overflow later.
  for (size_t index = 0; index < model_.links.size(); ++index) {
    const auto& link = model_.links[index];
    for (const auto& visual : link.visuals)
      (void)compose(result[index], urdfPoseMatrix(visual.linkFromVisual));
    for (const auto& collision : link.collisions)
      (void)compose(result[index], urdfPoseMatrix(collision.linkFromCollision));
    if (link.inertial)
      (void)compose(result[index], urdfPoseMatrix(link.inertial->linkFromInertial));
  }
  return result;
}

void UrdfRuntime::setPositions(std::span<const UrdfJointValue> values) {
  auto next = validated(values, true);
  auto placed = placeLinks(root_, next);
  joints_.swap(next);
  worldFromLinks_.swap(placed);
}

void UrdfRuntime::setTargets(std::span<const UrdfJointValue> values) {
  auto next = validated(values, false);
  auto targetPositions = next;
  for (auto& joint : targetPositions)
    joint.position = joint.target;
  (void)placeLinks(root_, targetPositions);
  joints_.swap(next);
}

void UrdfRuntime::setWorldFromRoot(const UrdfPose& pose) {
  const auto root = urdfPoseMatrix(pose);
  auto placed = placeLinks(root, joints_);
  root_ = root;
  worldFromLinks_.swap(placed);
}

void UrdfRuntime::executeTick(double dt) {
  auto next = joints_;
  for (size_t index = 0; index < model_.joints.size(); ++index) {
    const auto& joint = model_.joints[index];
    auto& state = next[index];
    if (joint.type == UrdfJointType::Fixed) {
      state.velocity = 0;
      continue;
    }
    const auto position = joint.limits ?
        approach(state.position, state.target, joint.limits->velocity * dt) : state.target;
    validatePosition(joint, position);
    state.velocity = actualVelocity(joint, state.position, position, dt);
    state.position = position;
  }
  auto placed = placeLinks(root_, next);
  joints_.swap(next);
  worldFromLinks_.swap(placed);
}

uint32_t UrdfRuntime::advance(std::chrono::nanoseconds elapsed) {
  return clock_.advance(elapsed, [this](uint64_t, double dt) { executeTick(dt); });
}

void UrdfRuntime::step() {
  clock_.step([this](uint64_t, double dt) { executeTick(dt); });
}

void UrdfRuntime::reset() {
  auto next = initialJoints_;
  auto placed = placeLinks(initialRoot_, next);
  clock_.reset();
  clock_.setPaused(true);
  root_ = initialRoot_;
  joints_.swap(next);
  worldFromLinks_.swap(placed);
}

glm::dmat4 UrdfRuntime::worldFromVisual(size_t link, size_t visual) const {
  return compose(worldFromLinks_.at(link),
                 urdfPoseMatrix(model_.links.at(link).visuals.at(visual).linkFromVisual));
}

glm::dmat4 UrdfRuntime::worldFromCollision(size_t link, size_t collision) const {
  return compose(worldFromLinks_.at(link),
                 urdfPoseMatrix(model_.links.at(link).collisions.at(collision).linkFromCollision));
}

std::optional<glm::dmat4> UrdfRuntime::worldFromInertial(size_t link) const {
  const auto& inertial = model_.links.at(link).inertial;
  if (!inertial)
    return std::nullopt;
  return compose(worldFromLinks_.at(link), urdfPoseMatrix(inertial->linkFromInertial));
}

} // namespace container::robotics
