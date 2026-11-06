#include "Container/physics/PhysicsWorld.h"

#include <glm/geometric.hpp>

// Jolt requires this header before every other Jolt header.
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace container::physics {
namespace {

void traceJolt(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertJolt(const char *expression, const char *message,
                 const char *file, JPH::uint line) {
  std::fprintf(stderr, "Jolt assertion at %s:%u: %s (%s)\n", file, line,
               expression, message != nullptr ? message : "");
  std::abort();
}
#endif

class RuntimeOwner;
struct RuntimeState {
  std::mutex mutex;
  std::condition_variable retired;
  std::weak_ptr<RuntimeOwner> owner;
  bool registered{false};
};

RuntimeState &runtimeState() {
  static RuntimeState state;
  return state;
}

class RuntimeOwner {
public:
  void activate() noexcept { active_ = true; }
  ~RuntimeOwner() {
    if (!active_) return;
    auto &state = runtimeState();
    {
      std::lock_guard lock(state.mutex);
      JPH::UnregisterTypes();
      delete JPH::Factory::sInstance;
      JPH::Factory::sInstance = nullptr;
      state.registered = false;
    }
    state.retired.notify_all();
  }

private:
  bool active_{false};
};

std::shared_ptr<RuntimeOwner> acquireRuntime() {
  auto &state = runtimeState();
  std::unique_lock lock(state.mutex);
  for (;;) {
    if (auto owner = state.owner.lock())
      return owner;
    // The last shared owner may already be expiring on another thread. Wait
    // for its destructor to retire the old factory before registering anew.
    if (!state.registered)
      break;
    state.retired.wait(lock);
  }
  static std::once_flag allocatorInitialized;
  std::call_once(allocatorInitialized, [] { JPH::RegisterDefaultAllocator(); });
  if (JPH::Factory::sInstance != nullptr)
    throw std::runtime_error("A different owner already registered Jolt types");
  auto owner = std::make_shared<RuntimeOwner>();
  JPH::Trace = traceJolt;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = assertJolt;
#endif
  JPH::Factory::sInstance = new JPH::Factory();
  try {
    JPH::RegisterTypes();
  } catch (...) {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    throw;
  }
  owner->activate();
  state.registered = true;
  state.owner = owner;
  return owner;
}

constexpr JPH::ObjectLayer kStaticLayer = 0u;
constexpr JPH::ObjectLayer kMovingLayer = 1u;
constexpr JPH::BroadPhaseLayer kStaticBroadPhase{0u};
constexpr JPH::BroadPhaseLayer kMovingBroadPhase{1u};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 2u; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return layer == kStaticLayer ? kStaticBroadPhase : kMovingBroadPhase;
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer == kStaticBroadPhase ? "Static" : "Moving";
  }
#endif
};

class BroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer object,
                     JPH::BroadPhaseLayer broadPhase) const override {
    return object != kStaticLayer || broadPhase == kMovingBroadPhase;
  }
};

class ObjectFilter final : public JPH::ObjectLayerPairFilter {
public:
  bool ShouldCollide(JPH::ObjectLayer first,
                     JPH::ObjectLayer second) const override {
    return first != kStaticLayer || second != kStaticLayer;
  }
};

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

void validatePosition(glm::dvec3 position) {
  // Body origins retain double precision, but Jolt's broad-phase bounds remain
  // floats. Leave headroom for finite shape extents and bounds calculations.
  constexpr double maximumPosition =
      static_cast<double>(std::numeric_limits<float>::max()) * 0.25;
  for (size_t axis = 0; axis < 3u; ++axis)
    if (!std::isfinite(position[axis]) ||
        std::abs(position[axis]) > maximumPosition)
      throw std::invalid_argument("Physics position must be finite and representable");
}

glm::quat normalizedRotation(glm::quat rotation) {
  const double lengthSquared = double(rotation.w) * rotation.w +
      double(rotation.x) * rotation.x + double(rotation.y) * rotation.y +
      double(rotation.z) * rotation.z;
  if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
    throw std::invalid_argument("Physics rotation must be finite and nonzero");
  const double inverseLength = 1.0 / std::sqrt(lengthSquared);
  return {float(rotation.w * inverseLength), float(rotation.x * inverseLength),
          float(rotation.y * inverseLength), float(rotation.z * inverseLength)};
}

void validateDuration(float deltaSeconds) {
  if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f ||
      !std::isfinite(1.0f / deltaSeconds))
    throw std::invalid_argument("Physics timestep must be finite and positive");
}

void validateBodySettings(const PhysicsBodySettings &settings) {
  switch (settings.motionType) {
  case PhysicsMotionType::Static:
  case PhysicsMotionType::Kinematic:
  case PhysicsMotionType::Dynamic:
    break;
  default:
    throw std::invalid_argument("Unknown physics motion type");
  }
  validatePosition(settings.position);
  (void)normalizedRotation(settings.rotation);
  if (!finite(settings.linearVelocity) || !finite(settings.angularVelocity) ||
      !std::isfinite(settings.mass) || settings.mass <= 0.0f ||
      !std::isfinite(1.0f / settings.mass) ||
      !std::isfinite(settings.friction) || settings.friction < 0.0f ||
      !std::isfinite(settings.restitution) || settings.restitution < 0.0f ||
      settings.restitution > 1.0f || !std::isfinite(settings.gravityFactor))
    throw std::invalid_argument("Invalid physics material, mass or velocity");
}

JPH::Vec3 toJolt(glm::vec3 value) { return {value.x, value.y, value.z}; }
JPH::RVec3 toJolt(glm::dvec3 value) { return {value.x, value.y, value.z}; }

JPH::Vec3 boundedVelocity(glm::vec3 velocity, float maximum) {
  // Jolt's creation path asserts above its velocity caps, while its public
  // mutation path clamps. Use the same policy for both, without overflowing
  // the float length calculation for otherwise finite input components.
  const glm::dvec3 precise{velocity};
  const double length = glm::length(precise);
  if (length > maximum)
    velocity = glm::vec3{precise * (double(maximum) / length)};
  auto result = toJolt(velocity);
  const float roundedLength = result.Length();
  if (roundedLength > maximum)
    result *= std::nextafter(maximum / roundedLength, 0.0f);
  return result;
}

JPH::Quat toJolt(glm::quat value) {
  value = normalizedRotation(value);
  return {value.x, value.y, value.z, value.w};
}

JPH::EMotionType toJolt(PhysicsMotionType motion) {
  switch (motion) {
  case PhysicsMotionType::Static: return JPH::EMotionType::Static;
  case PhysicsMotionType::Kinematic: return JPH::EMotionType::Kinematic;
  case PhysicsMotionType::Dynamic: return JPH::EMotionType::Dynamic;
  }
  throw std::invalid_argument("Unknown physics motion type");
}

int workerCount(const PhysicsWorldSettings &settings) {
  if (settings.workerThreads > uint32_t(std::numeric_limits<int>::max()))
    throw std::invalid_argument("Physics worker count is too large");
  if (settings.workerThreads != 0u)
    return static_cast<int>(settings.workerThreads);
  const unsigned int hardware = std::thread::hardware_concurrency();
  return static_cast<int>(std::min(hardware > 1u ? hardware - 1u : 0u, 4u));
}

uint32_t collisionBudget(uint32_t maxBodies) {
  const uint64_t contacts = std::max(uint64_t(maxBodies) * 4u, uint64_t{1024u});
  if (contacts > JPH::ContactConstraintManager::cMaxContactConstraintsLimit ||
      contacts > JPH::ContactConstraintManager::cMaxBodyPairsLimit)
    throw std::invalid_argument("Physics body capacity exceeds the supported collision budget");
  return static_cast<uint32_t>(contacts);
}

PhysicsBodyId nextBodyId() {
  static std::atomic<PhysicsBodyId> next{1u};
  auto id = next.load(std::memory_order_relaxed);
  for (;;) {
    if (id == std::numeric_limits<PhysicsBodyId>::max())
      throw std::overflow_error("Physics body identity space is exhausted");
    if (next.compare_exchange_weak(id, id + 1u, std::memory_order_relaxed))
      return id;
  }
}

PhysicsJointId nextJointId() {
  static std::atomic<PhysicsJointId> next{1u};
  auto id = next.load(std::memory_order_relaxed);
  for (;;) {
    if (id == std::numeric_limits<PhysicsJointId>::max())
      throw std::overflow_error("Physics joint identity space is exhausted");
    if (next.compare_exchange_weak(id, id + 1u, std::memory_order_relaxed))
      return id;
  }
}

PhysicsJointFrame normalizedFrame(PhysicsJointFrame frame) {
  validatePosition(frame.anchor);
  if (!finite(frame.axis) || !finite(frame.normal))
    throw std::invalid_argument("Physics joint axes must be finite");
  const double axisLength = glm::length(glm::dvec3{frame.axis});
  const double normalLength = glm::length(glm::dvec3{frame.normal});
  if (axisLength < 1e-12 || normalLength < 1e-12)
    throw std::invalid_argument("Physics joint axes must be nonzero");
  frame.axis = glm::vec3{glm::dvec3{frame.axis} / axisLength};
  frame.normal = glm::vec3{glm::dvec3{frame.normal} / normalLength};
  const float projection = glm::dot(frame.axis, frame.normal);
  if (std::abs(projection) > 1e-4f)
    throw std::invalid_argument("Physics joint axis and normal must be perpendicular");
  frame.normal = glm::normalize(frame.normal - projection * frame.axis);
  return frame;
}

void validateMotor(const PhysicsJointMotorSettings &motor) {
  if (motor.mode != PhysicsJointMotorMode::Off &&
      motor.mode != PhysicsJointMotorMode::Velocity &&
      motor.mode != PhysicsJointMotorMode::Position)
    throw std::invalid_argument("Unknown physics joint motor mode");
  if (!std::isfinite(motor.targetVelocity) || !std::isfinite(motor.targetPosition) ||
      !std::isfinite(motor.maxForceOrTorque) || motor.maxForceOrTorque < 0.0f ||
      !std::isfinite(motor.frequencyHz) || motor.frequencyHz < 0.0f ||
      (motor.mode == PhysicsJointMotorMode::Position && motor.frequencyHz <= 0.0f) ||
      !std::isfinite(motor.dampingRatio) || motor.dampingRatio < 0.0f)
    throw std::invalid_argument("Invalid physics joint motor settings");
  if (motor.mode == PhysicsJointMotorMode::Position) {
    // Jolt forms these float spring coefficients before applying the joint's
    // effective mass. Underflow can erase the spring entirely; overflow can
    // produce nonfinite solver bias. Off/Velocity do not evaluate the spring.
    const float omega = 2.0f * JPH::JPH_PI * motor.frequencyHz;
    const float stiffnessPerMass = omega * omega;
    const float dampingPerMass = 2.0f * motor.dampingRatio * omega;
    if (!std::isfinite(stiffnessPerMass) || stiffnessPerMass <= 0.0f ||
        !std::isfinite(dampingPerMass))
      throw std::invalid_argument("Physics position motor spring coefficients are not representable");
  }
}

void validateLimits(PhysicsJointType type, bool enabled, float lower, float upper) {
  constexpr float pi = 3.14159265358979323846f;
  if (!std::isfinite(lower) || !std::isfinite(upper) || lower > upper ||
      (enabled && (lower >= upper || lower > 0.0f || upper < 0.0f)) ||
      (enabled && type == PhysicsJointType::Hinge && (lower < -pi || upper > pi)) ||
      (enabled && type == PhysicsJointType::Fixed))
    throw std::invalid_argument("Physics joint limits must contain zero and fit their joint type");
}

PhysicsJointSettings validatedJointSettings(PhysicsJointSettings settings) {
  if (settings.type != PhysicsJointType::Fixed &&
      settings.type != PhysicsJointType::Hinge &&
      settings.type != PhysicsJointType::Slider)
    throw std::invalid_argument("Unknown physics joint type");
  validateLimits(settings.type, settings.limitsEnabled, settings.lowerLimit, settings.upperLimit);
  validateMotor(settings.motor);
  if (settings.type == PhysicsJointType::Fixed && settings.motor.mode != PhysicsJointMotorMode::Off)
    throw std::invalid_argument("Fixed joints do not support motors");
  if (settings.limitsEnabled)
    settings.motor.targetPosition = std::clamp(settings.motor.targetPosition,
                                               settings.lowerLimit, settings.upperLimit);
  else if (settings.type == PhysicsJointType::Hinge &&
           settings.motor.mode == PhysicsJointMotorMode::Position) {
    // Jolt reduces hinge position error with repeated +/- 2*pi. Large finite
    // targets can stop changing under those float additions and hang a step.
    // An unlimited hinge has a periodic target, so reduce it before Jolt runs.
    settings.motor.targetPosition = std::remainder(settings.motor.targetPosition,
                                                     2.0f * JPH::JPH_PI);
  }
  return settings;
}

JPH::EMotorState motorState(PhysicsJointMotorMode mode) {
  switch (mode) {
  case PhysicsJointMotorMode::Off: return JPH::EMotorState::Off;
  case PhysicsJointMotorMode::Velocity: return JPH::EMotorState::Velocity;
  case PhysicsJointMotorMode::Position: return JPH::EMotorState::Position;
  }
  return JPH::EMotorState::Off;
}

template <typename Constraint>
void configureMotor(Constraint &constraint, const PhysicsJointMotorSettings &motor,
                      bool angular) {
  auto &settings = constraint.GetMotorSettings();
  settings.mSpringSettings = JPH::SpringSettings{JPH::ESpringMode::FrequencyAndDamping,
                                                motor.frequencyHz, motor.dampingRatio};
  if (angular)
    settings.SetTorqueLimit(motor.maxForceOrTorque);
  else
    settings.SetForceLimit(motor.maxForceOrTorque);
  constraint.SetMotorState(motorState(motor.mode));
}

uint64_t collisionPair(JPH::BodyID first, JPH::BodyID second) noexcept {
  const uint32_t a = first.GetIndexAndSequenceNumber();
  const uint32_t b = second.GetIndexAndSequenceNumber();
  return (uint64_t(std::min(a, b)) << 32u) | std::max(a, b);
}

class JointCollisionFilter final : public JPH::GroupFilter {
public:
  std::unordered_map<uint64_t, uint32_t> disabledPairs;

  bool CanCollide(const JPH::CollisionGroup &first,
                  const JPH::CollisionGroup &second) const override {
    if (first.GetGroupFilter() != this || second.GetGroupFilter() != this)
      return true;
    const uint32_t a = first.GetSubGroupID(), b = second.GetSubGroupID();
    const uint64_t key = (uint64_t(std::min(a, b)) << 32u) | std::max(a, b);
    return !disabledPairs.contains(key);
  }
};

template <class ShapeSettings>
JPH::ShapeRefC makeShape(const ShapeSettings &settings) {
  const auto result = settings.Create();
  if (result.HasError())
    throw std::invalid_argument("Invalid physics shape: " +
                                std::string(result.GetError().c_str()));
  return result.Get();
}

} // namespace

struct PhysicsWorld::Impl {
  struct WorldAnchorDeleter {
    const JPH::BodyInterface *interface{};
    void operator()(JPH::Body *body) const noexcept {
      interface->DestroyBodyWithoutID(body);
    }
  };
  using WorldAnchor = std::unique_ptr<JPH::Body, WorldAnchorDeleter>;
  struct Joint {
    PhysicsBodyId bodyA{kInvalidPhysicsBodyId};
    PhysicsBodyId bodyB{kInvalidPhysicsBodyId};
    PhysicsJointSettings settings{};
    PhysicsJointFrame frameA{};
    PhysicsJointFrame frameB{};
    // Unregistered static endpoints give world joints a nearby COM origin.
    // Release the constraint before the body it references, including on a
    // failed insertion. Neither an ID nor a broad-phase slot is allocated.
    WorldAnchor worldAnchor{nullptr, WorldAnchorDeleter{}};
    JPH::Ref<JPH::TwoBodyConstraint> constraint;
  };
  // Runtime is destroyed last, after all world resources and shape references.
  std::shared_ptr<RuntimeOwner> runtime;
  JPH::Ref<JointCollisionFilter> jointCollisionFilter{new JointCollisionFilter};
  BroadPhaseLayers broadPhaseLayers;
  BroadPhaseFilter broadPhaseFilter;
  ObjectFilter objectFilter;
  JPH::TempAllocatorImplWithMallocFallback temporary{8u * 1024u * 1024u};
  JPH::JobSystemThreadPool jobs;
  JPH::PhysicsSystem system;
  std::unordered_map<PhysicsBodyId, JPH::BodyID> bodies;
  // Only prescribed Kinematic motion needs per-tick sleep maintenance. Keep
  // this sparse so large static/model populations add no simulation scan.
  std::unordered_set<PhysicsBodyId> kinematicDrivers;
  std::unordered_map<PhysicsJointId, Joint> joints;
  std::unordered_map<PhysicsBodyId, std::unordered_set<PhysicsJointId>> jointsByBody;
  std::vector<PhysicsBodyId> componentPending;
  std::vector<JPH::BodyID> componentBodies;
  std::vector<uint32_t> componentVisited;
  std::vector<JPH::BodyID> nearbyBodies;
  uint32_t componentGeneration{0};

  explicit Impl(const PhysicsWorldSettings &settings)
      : runtime(acquireRuntime()),
        jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerCount(settings)) {
    const uint32_t contacts = collisionBudget(settings.maxBodies);
    system.Init(settings.maxBodies, 0u, contacts, contacts, broadPhaseLayers,
                broadPhaseFilter, objectFilter);
    system.SetGravity(toJolt(settings.gravity));
    // Traversal also serves noexcept joint/body removal. Allocate once while
    // constructing the world, not while committing a topology edit.
    componentPending.reserve(settings.maxBodies);
    componentBodies.reserve(settings.maxBodies);
    componentVisited.resize(settings.maxBodies, 0u);
    nearbyBodies.reserve(settings.maxBodies);
  }

  ~Impl() { clear(); }

  void clear() noexcept {
    for (const auto &[handle, joint] : joints) {
      (void)handle;
      system.RemoveConstraint(joint.constraint);
    }
    joints.clear();
    jointsByBody.clear();
    jointCollisionFilter->disabledPairs.clear();
    auto &interface = system.GetBodyInterface();
    for (const auto &[handle, body] : bodies) {
      (void)handle;
      interface.RemoveBody(body);
      interface.DestroyBody(body);
    }
    bodies.clear();
    kinematicDrivers.clear();
  }

  JPH::BodyID bodyForJoint(PhysicsBodyId body, bool allowWorld) const {
    if (allowWorld && body == kInvalidPhysicsBodyId)
      return JPH::BodyID{};
    const auto found = bodies.find(body);
    if (found == bodies.end())
      throw std::invalid_argument("Physics joint endpoint is stale or belongs to another world");
    return found->second;
  }

  std::pair<JPH::BodyID, JPH::BodyID> endpoints(PhysicsBodyId a, PhysicsBodyId b) {
    const auto first = bodyForJoint(a, true);
    const auto second = bodyForJoint(b, false);
    if (a == b)
      throw std::invalid_argument("Physics joint endpoints must be different bodies");
    auto &interface = system.GetBodyInterface();
    if ((first.IsInvalid() || interface.GetMotionType(first) != JPH::EMotionType::Dynamic) &&
        interface.GetMotionType(second) != JPH::EMotionType::Dynamic)
      throw std::invalid_argument("Physics joints require at least one dynamic endpoint");
    return {first, second};
  }

  PhysicsJointFrame localFrame(JPH::BodyID body, PhysicsJointFrame frame) {
    if (body.IsInvalid())
      return frame;
    auto &interface = system.GetBodyInterface();
    const auto position = interface.GetCenterOfMassPosition(body);
    const auto rotation = interface.GetRotation(body);
    const glm::dquat inverse = glm::conjugate(glm::dquat{
        rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()});
    frame.anchor = inverse * (frame.anchor -
        glm::dvec3{position.GetX(), position.GetY(), position.GetZ()});
    frame.axis = glm::vec3{inverse * glm::dvec3{frame.axis}};
    frame.normal = glm::vec3{inverse * glm::dvec3{frame.normal}};
    return normalizedFrame(frame);
  }

  PhysicsJointFrame worldFrame(JPH::BodyID body, PhysicsJointFrame frame) {
    if (body.IsInvalid())
      return frame;
    auto &interface = system.GetBodyInterface();
    const auto position = interface.GetCenterOfMassPosition(body);
    const auto rotation = interface.GetRotation(body);
    const glm::dquat orientation{
        rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()};
    frame.anchor = glm::dvec3{position.GetX(), position.GetY(), position.GetZ()} +
                   orientation * frame.anchor;
    frame.axis = glm::vec3{orientation * glm::dvec3{frame.axis}};
    frame.normal = glm::vec3{orientation * glm::dvec3{frame.normal}};
    return normalizedFrame(frame);
  }

  void activatePair(JPH::BodyID a, JPH::BodyID b, bool invalidateContacts,
                    std::span<const JPH::BodyID> component) {
    auto &interface = system.GetBodyInterface();
    for (const auto body : {a, b}) {
      if (body.IsInvalid())
        continue;
      if (invalidateContacts)
        interface.InvalidateContactCache(body);
    }
    interface.ActivateBodies(component.data(), static_cast<int>(component.size()));
  }

  template <typename SeedVisitor>
  std::span<const JPH::BodyID> dynamicJointComponents(SeedVisitor &&visitSeeds) noexcept {
    // Jolt snapshots active constraints before building islands. Waking only
    // one endpoint can therefore leave farther joints asleep for one tick.
    // Scratch is bounded by the world's body capacity, and each live backend
    // index is visited once. This span lasts until the next traversal; callers
    // consume it before any other world operation can reuse the buffers.
    componentPending.clear();
    componentBodies.clear();
    if (++componentGeneration == 0u) {
      std::fill(componentVisited.begin(), componentVisited.end(), 0u);
      ++componentGeneration;
    }
    auto &interface = system.GetBodyInterface();
    const auto append = [&](PhysicsBodyId id) {
      const auto body = bodies.find(id);
      if (body == bodies.end() ||
          interface.GetMotionType(body->second) != JPH::EMotionType::Dynamic)
        return;
      const auto index = body->second.GetIndex();
      JPH_ASSERT(index < componentVisited.size());
      if (componentVisited[index] == componentGeneration)
        return;
      componentVisited[index] = componentGeneration;
      JPH_ASSERT(componentPending.size() < componentPending.capacity());
      JPH_ASSERT(componentBodies.size() < componentBodies.capacity());
      componentPending.push_back(id);
      componentBodies.push_back(body->second);
    };
    const auto appendNeighbors = [&](PhysicsBodyId id) {
      const auto attached = jointsByBody.find(id);
      if (attached == jointsByBody.end())
        return;
      for (const auto handle : attached->second) {
        const auto &joint = joints.at(handle);
        append(joint.bodyA);
        append(joint.bodyB);
      }
    };
    const auto appendSeed = [&](PhysicsBodyId seed, bool includeSeedNeighbors = false) {
      append(seed);
      // A moved static/kinematic support explicitly affects all its direct
      // dynamic neighbors. Otherwise these supports are traversal boundaries:
      // independent mechanisms sharing one support must remain asleep.
      if (includeSeedNeighbors)
        appendNeighbors(seed);
    };
    visitSeeds(appendSeed);
    for (size_t index = 0; index < componentPending.size(); ++index)
      appendNeighbors(componentPending[index]);
    return componentBodies;
  }

  std::span<const JPH::BodyID> dynamicJointComponent(
      std::initializer_list<PhysicsBodyId> seeds,
      bool includeSeedNeighbors = false,
      std::span<const JPH::BodyID> contactSeeds = {}) noexcept {
    // Collect the whole contact neighborhood into one traversal. User data is
    // assigned at body creation and resolves native hits without a world scan.
    // append() accepts only Dynamic bodies, so a stationary Kinematic hit does
    // not connect independent mechanisms sharing that support.
    return dynamicJointComponents([&](const auto &appendSeed) {
      for (const auto seed : seeds)
        appendSeed(seed, includeSeedNeighbors);
      for (const auto seed : contactSeeds)
        appendSeed(system.GetBodyInterface().GetUserData(seed));
    });
  }

  static double motorProgressTolerance(float coordinate, float target) noexcept {
    const auto spacing = [](float value) {
      return std::abs(double(value) - double(std::nextafter(value, 0.0f)));
    };
    // 0.1 mm in meter scenes / 0.006 degrees for hinges. Two float ULPs
    // accommodate coordinate quantization without forcing an unreachable goal.
    return std::max(1e-4, 2.0 * std::max(spacing(coordinate), spacing(target)));
  }

  static bool hasEffectiveLimits(const Joint &joint) noexcept {
    // Jolt treats the full [-pi, pi] hinge interval as unrestricted even
    // when the UI's limits checkbox remains enabled. Match that solver state
    // for both progress detection and constrained Position routing.
    return joint.settings.type == PhysicsJointType::Hinge
        ? static_cast<const JPH::HingeConstraint *>(joint.constraint.GetPtr())->HasLimits()
        : joint.settings.limitsEnabled;
  }

  bool motorNeedsProgress(const Joint &joint) const noexcept {
    const auto &settings = joint.settings;
    const auto &motor = settings.motor;
    if (settings.type == PhysicsJointType::Fixed ||
        motor.mode == PhysicsJointMotorMode::Off || motor.maxForceOrTorque == 0.0f ||
        (motor.mode == PhysicsJointMotorMode::Velocity && motor.targetVelocity == 0.0f))
      return false;
    const bool limited = hasEffectiveLimits(joint);
    if (motor.mode == PhysicsJointMotorMode::Velocity && !limited)
      return true;
    const float coordinate = settings.type == PhysicsJointType::Hinge
        ? static_cast<const JPH::HingeConstraint *>(joint.constraint.GetPtr())->GetCurrentAngle()
        : static_cast<const JPH::SliderConstraint *>(joint.constraint.GetPtr())->GetCurrentPosition();
    if (motor.mode == PhysicsJointMotorMode::Velocity) {
      const float stop = motor.targetVelocity > 0.0f ? settings.upperLimit : settings.lowerLimit;
      const double tolerance = motorProgressTolerance(coordinate, stop);
      // An outward drive at a hard stop has no remaining admissible travel.
      // Reversal and limit edits explicitly wake it for subsequent progress.
      return motor.targetVelocity > 0.0f ? coordinate < double(stop) - tolerance
                                        : coordinate > double(stop) + tolerance;
    }
    double error = double(motor.targetPosition) - coordinate;
    if (settings.type == PhysicsJointType::Hinge && !limited)
      error = std::remainder(error, double(2.0f * JPH::JPH_PI));
    return std::abs(error) > motorProgressTolerance(coordinate, motor.targetPosition);
  }

  void prepareHingePositionGoal(Joint &joint) noexcept {
    const auto &settings = joint.settings;
    if (settings.type != PhysicsJointType::Hinge ||
        settings.motor.mode != PhysicsJointMotorMode::Position)
      return;
    auto &hinge = *static_cast<JPH::HingeConstraint *>(joint.constraint.GetPtr());
    float target = settings.motor.targetPosition;
    if (hasEffectiveLimits(joint)) {
      const float coordinate = hinge.GetCurrentAngle();
      if (std::abs(double(target) - coordinate) > double(JPH::JPH_PI)) {
        // Jolt always chooses the shortest periodic error. With hard stops,
        // that shortcut can cross the forbidden gap instead of taking the
        // admissible interval. Its midpoint stays on the direct route and
        // within pi of the current angle. Restore the actual goal as soon as
        // that direct error fits, without changing canonical public settings.
        target = static_cast<float>((double(coordinate) + target) * 0.5);
      }
    }
    hinge.SetTargetAngle(target);
  }

  void prepareKinematicDrive(PhysicsBodyId id, JPH::Vec3Arg linear,
                             JPH::Vec3Arg angular) {
    // Allocate when starting a drive, before changing the backend velocity.
    // Even velocities below Jolt's IsNearZero threshold are prescribed motion.
    if (linear != JPH::Vec3::sZero() || angular != JPH::Vec3::sZero())
      kinematicDrivers.insert(id);
    else
      kinematicDrivers.erase(id);
  }

  void wakeDrivenBodies() noexcept {
    if (joints.empty() && kinematicDrivers.empty())
      return;
    auto &interface = system.GetBodyInterface();
    // Seed all prescribed drivers before walking, so overlapping mechanisms
    // are visited only once. Resetting timers also keeps slow active drives
    // from sleeping even though their movement is below Jolt's sleep threshold.
    const auto connected = dynamicJointComponents([&](const auto &appendSeed) {
      for (auto &[id, joint] : joints) {
        (void)id;
        prepareHingePositionGoal(joint);
        if (motorNeedsProgress(joint)) {
          appendSeed(joint.bodyA);
          appendSeed(joint.bodyB);
        }
      }
      for (const auto id : kinematicDrivers) {
        // Jolt zeros velocities when a low-speed island sleeps. A prescribed
        // Kinematic driver must keep its command until an explicit stop, and
        // all its Dynamic branches must enter the first constraint snapshot.
        interface.ActivateBody(bodies.at(id));
        appendSeed(id, true);
      }
    });
    interface.ActivateBodies(connected.data(), static_cast<int>(connected.size()));
  }

  bool disablePair(JPH::BodyID a, JPH::BodyID b) {
    if (a.IsInvalid() || b.IsInvalid())
      return false;
    auto [entry, inserted] = jointCollisionFilter->disabledPairs.try_emplace(collisionPair(a, b), 0u);
    if (entry->second == std::numeric_limits<uint32_t>::max())
      throw std::overflow_error("Too many physics joints share one collision pair");
    ++entry->second;
    return inserted;
  }

  bool enablePair(JPH::BodyID a, JPH::BodyID b) noexcept {
    if (a.IsInvalid() || b.IsInvalid())
      return false;
    const auto found = jointCollisionFilter->disabledPairs.find(collisionPair(a, b));
    if (found == jointCollisionFilter->disabledPairs.end())
      return false;
    if (--found->second != 0u)
      return false;
    jointCollisionFilter->disabledPairs.erase(found);
    return true;
  }

  JPH::Ref<JPH::TwoBodyConstraint> makeConstraint(
      JPH::BodyID first, JPH::BodyID second,
      const PhysicsJointSettings &settings,
      PhysicsJointFrame frameA, const PhysicsJointFrame &frameB,
      JPH::Body *worldAnchor) {
    auto &interface = system.GetBodyInterface();
    if (worldAnchor != nullptr)
      frameA.anchor = glm::dvec3{0};
    const auto create = [&](const JPH::TwoBodyConstraintSettings &definition) {
      if (worldAnchor != nullptr) {
        // BodyInterface's ID-based path substitutes the global origin for an
        // invalid ID. Use the owned, unregistered static endpoint directly.
        JPH::BodyLockWrite lock(system.GetBodyLockInterface(), second);
        if (!lock.Succeeded())
          throw std::runtime_error("Physics joint endpoint became unavailable");
        return definition.Create(*worldAnchor, lock.GetBody());
      }
      return interface.CreateConstraint(&definition, first, second);
    };
    if (settings.type == PhysicsJointType::Fixed) {
      JPH::FixedConstraintSettings definition;
      definition.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
      definition.mPoint1 = toJolt(frameA.anchor);
      definition.mPoint2 = toJolt(frameB.anchor);
      definition.mAxisX1 = toJolt(frameA.axis);
      definition.mAxisY1 = toJolt(frameA.normal);
      definition.mAxisX2 = toJolt(frameB.axis);
      definition.mAxisY2 = toJolt(frameB.normal);
      return create(definition);
    }
    if (settings.type == PhysicsJointType::Hinge) {
      JPH::HingeConstraintSettings definition;
      definition.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
      definition.mPoint1 = toJolt(frameA.anchor);
      definition.mPoint2 = toJolt(frameB.anchor);
      definition.mHingeAxis1 = toJolt(frameA.axis);
      definition.mNormalAxis1 = toJolt(frameA.normal);
      definition.mHingeAxis2 = toJolt(frameB.axis);
      definition.mNormalAxis2 = toJolt(frameB.normal);
      if (settings.limitsEnabled) {
        definition.mLimitsMin = settings.lowerLimit;
        definition.mLimitsMax = settings.upperLimit;
      }
      JPH::Ref<JPH::HingeConstraint> hinge = static_cast<JPH::HingeConstraint *>(
          create(definition));
      configureMotor(*hinge, settings.motor, true);
      hinge->SetTargetAngularVelocity(settings.motor.targetVelocity);
      hinge->SetTargetAngle(settings.motor.targetPosition);
      return hinge.GetPtr();
    }
    JPH::SliderConstraintSettings definition;
    definition.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    definition.mPoint1 = toJolt(frameA.anchor);
    definition.mPoint2 = toJolt(frameB.anchor);
    definition.mSliderAxis1 = toJolt(frameA.axis);
    definition.mNormalAxis1 = toJolt(frameA.normal);
    definition.mSliderAxis2 = toJolt(frameB.axis);
    definition.mNormalAxis2 = toJolt(frameB.normal);
    if (settings.limitsEnabled) {
      definition.mLimitsMin = settings.lowerLimit;
      definition.mLimitsMax = settings.upperLimit;
    }
    JPH::Ref<JPH::SliderConstraint> slider = static_cast<JPH::SliderConstraint *>(
        create(definition));
    configureMotor(*slider, settings.motor, false);
    slider->SetTargetVelocity(settings.motor.targetVelocity);
    slider->SetTargetPosition(settings.motor.targetPosition);
    return slider.GetPtr();
  }

  void unlinkEndpoint(PhysicsBodyId body, PhysicsJointId joint) noexcept {
    const auto found = jointsByBody.find(body);
    if (found == jointsByBody.end())
      return;
    found->second.erase(joint);
    if (found->second.empty())
      jointsByBody.erase(found);
  }

  PhysicsJointId createJoint(PhysicsBodyId bodyA, PhysicsBodyId bodyB,
                             PhysicsJointSettings settings,
                             PhysicsJointFrame frameA, PhysicsJointFrame frameB) {
    const auto [first, second] = endpoints(bodyA, bodyB);
    frameA = normalizedFrame(frameA);
    frameB = normalizedFrame(frameB);
    const auto world = worldFrame(first, frameA);
    settings.anchor = world.anchor;
    settings.axis = world.axis;
    settings.normal = world.normal;
    const auto connected = dynamicJointComponent({bodyA, bodyB});
    Joint candidate;
    candidate.bodyA = bodyA;
    candidate.bodyB = bodyB;
    candidate.settings = settings;
    candidate.frameA = frameA;
    candidate.frameB = frameB;
    if (first.IsInvalid()) {
      auto &interface = system.GetBodyInterface();
      // Reuse Jolt's permanent empty world shape. The body is never registered
      // or added to the broad phase; only its double-precision COM is needed.
      JPH::BodyCreationSettings definition(JPH::Body::sFixedToWorld.GetShape(),
          toJolt(frameA.anchor), JPH::Quat::sIdentity(), JPH::EMotionType::Static, kStaticLayer);
      candidate.worldAnchor = WorldAnchor{interface.CreateBodyWithoutID(definition),
                                          WorldAnchorDeleter{&interface}};
      if (!candidate.worldAnchor)
        throw std::runtime_error("Physics world anchor allocation failed");
    }
    candidate.constraint = makeConstraint(first, second, settings, frameA, frameB,
                                          candidate.worldAnchor.get());
    const auto handle = nextJointId();
    bool filtered = false;
    bool filterChanged = false;
    bool added = false;
    try {
      joints.emplace(handle, std::move(candidate));
      if (bodyA != kInvalidPhysicsBodyId)
        jointsByBody[bodyA].insert(handle);
      jointsByBody[bodyB].insert(handle);
      if (!settings.collideConnected) {
        filterChanged = disablePair(first, second);
        filtered = !first.IsInvalid();
      }
      system.AddConstraint(joints.at(handle).constraint);
      added = true;
      activatePair(first, second, filterChanged, connected);
    } catch (...) {
      if (added)
        system.RemoveConstraint(joints.at(handle).constraint);
      if (filtered)
        (void)enablePair(first, second);
      unlinkEndpoint(bodyA, handle);
      unlinkEndpoint(bodyB, handle);
      joints.erase(handle);
      throw;
    }
    return handle;
  }

  bool destroyJoint(PhysicsJointId id) noexcept {
    const auto found = joints.find(id);
    if (found == joints.end())
      return false;
    auto &joint = found->second;
    const auto first = joint.bodyA == kInvalidPhysicsBodyId ? JPH::BodyID{} : bodies.at(joint.bodyA);
    const auto second = bodies.at(joint.bodyB);
    system.RemoveConstraint(joint.constraint);
    const bool filterChanged = !joint.settings.collideConnected && enablePair(first, second);
    unlinkEndpoint(joint.bodyA, id);
    unlinkEndpoint(joint.bodyB, id);
    const auto connected = dynamicJointComponent({joint.bodyA, joint.bodyB});
    activatePair(first, second, filterChanged, connected);
    joints.erase(found);
    return true;
  }

  void wakeNearbyMovingBodies(JPH::AABox bounds) noexcept {
    if (!bounds.IsValid())
      return;
    // Include resting contacts separated by the solver's permitted slop.
    // Query each old/new neighborhood separately: a large teleport must not
    // wake every body in the corridor between those two bounding boxes.
    const auto &settings = system.GetPhysicsSettings();
    const float margin = settings.mSpeculativeContactDistance +
                         settings.mPenetrationSlop + 0.001f;
    bounds.ExpandBy(JPH::Vec3::sReplicate(margin));
    const JPH::SpecifiedBroadPhaseLayerFilter movingBroadPhase{kMovingBroadPhase};
    const JPH::SpecifiedObjectLayerFilter movingObjects{kMovingLayer};
    struct NearbyCollector final : JPH::CollideShapeBodyCollector {
      std::vector<JPH::BodyID> &bodies;
      explicit NearbyCollector(std::vector<JPH::BodyID> &storage) : bodies(storage) {}
      void AddHit(const JPH::BodyID &body) override {
        // An AABox query visits each body at most once. Capacity was reserved
        // for every possible live body, including during noexcept removal.
        JPH_ASSERT(bodies.size() < bodies.capacity());
        bodies.push_back(body);
      }
    } collector{nearbyBodies};
    nearbyBodies.clear();
    // Do not acquire body write locks while the broad-phase query is running.
    system.GetBroadPhaseQuery().CollideAABox(bounds, collector, movingBroadPhase,
                                            movingObjects);
    const auto connected = dynamicJointComponent({}, false, nearbyBodies);
    system.GetBodyInterface().ActivateBodies(connected.data(), static_cast<int>(connected.size()));
  }

  PhysicsBodyId create(JPH::ShapeRefC shape,
                        const PhysicsBodySettings &settings) {
    if (settings.motionType == PhysicsMotionType::Dynamic) {
      // Jolt multiplies these float values before integrating and computing
      // squared speed for clamping. Finite settings alone do not guarantee a
      // representable acceleration. Unused static/kinematic factors and zero
      // gravity retain their normal semantics without an arbitrary factor cap.
      const auto acceleration = system.GetGravity() * settings.gravityFactor;
      if (!std::isfinite(acceleration.LengthSq()))
        throw std::invalid_argument("Physics gravity acceleration is not representable");
    }
    const auto handle = nextBodyId();
    JPH::BodyCreationSettings bodySettings(
        shape, toJolt(settings.position), toJolt(settings.rotation),
        toJolt(settings.motionType), settings.motionType == PhysicsMotionType::Static
            ? kStaticLayer : kMovingLayer);
    bodySettings.mUserData = handle;
    bodySettings.mLinearVelocity = boundedVelocity(settings.linearVelocity,
                                                    bodySettings.mMaxLinearVelocity);
    bodySettings.mAngularVelocity = boundedVelocity(settings.angularVelocity,
                                                     bodySettings.mMaxAngularVelocity);
    bodySettings.mFriction = settings.friction;
    bodySettings.mRestitution = settings.restitution;
    bodySettings.mGravityFactor = settings.gravityFactor;
    bodySettings.mMotionQuality = settings.useCCD
        ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    bodySettings.mEnhancedInternalEdgeRemoval = true;
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = settings.mass;
    if (settings.motionType != PhysicsMotionType::Static) {
      const auto massProperties = bodySettings.GetMassProperties();
      if (!std::isfinite(massProperties.mMass) || massProperties.mMass <= 0.0f)
        throw std::invalid_argument("Physics shape has invalid mass properties");
      for (JPH::uint row = 0; row < 3u; ++row)
        for (JPH::uint column = 0; column < 3u; ++column)
          if (!std::isfinite(massProperties.mInertia(row, column)))
            throw std::invalid_argument("Physics shape inertia is not representable");
      // Finite inertia alone is insufficient: Jolt can overflow its principal
      // inverse or near-zero fallback (2.5 / mass). Probe its exact conversion
      // before creating a body. Unchecked reads avoid cached motion-type state
      // that only Body initializes on a live MotionProperties instance.
      JPH::MotionProperties probe;
      probe.SetMassProperties(JPH::EAllowedDOFs::All, massProperties);
      if (!std::isfinite(probe.GetInverseMassUnchecked()) ||
          probe.GetInverseMassUnchecked() <= 0.0f)
        throw std::invalid_argument("Physics shape inverse mass is not representable");
      const auto inverseInertia = probe.GetLocalSpaceInverseInertiaUnchecked();
      for (JPH::uint row = 0; row < 3u; ++row) {
        if (inverseInertia(row, row) <= 0.0f)
          throw std::invalid_argument("Physics shape inverse inertia must be positive");
        for (JPH::uint column = 0; column < 3u; ++column)
          if (!std::isfinite(inverseInertia(row, column)))
            throw std::invalid_argument("Physics shape inverse inertia is not representable");
      }
      bodySettings.mMassPropertiesOverride = massProperties;
      bodySettings.mOverrideMassProperties =
          JPH::EOverrideMassProperties::MassAndInertiaProvided;
    }
    const auto activation = settings.motionType == PhysicsMotionType::Static
        ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
    auto &interface = system.GetBodyInterface();
    const auto body = interface.CreateAndAddBody(bodySettings, activation);
    if (body.IsInvalid())
      throw std::runtime_error("Physics world body capacity was exhausted");
    try {
      interface.SetCollisionGroup(body, JPH::CollisionGroup{
          jointCollisionFilter, 1u, body.GetIndexAndSequenceNumber()});
      bodies.emplace(handle, body);
      if (settings.motionType == PhysicsMotionType::Kinematic)
        prepareKinematicDrive(handle, bodySettings.mLinearVelocity,
                                bodySettings.mAngularVelocity);
      // Contact discovery follows Jolt's active-constraint snapshot. Even an
      // active new collider must wake overlapping Dynamic joint components
      // before their first solve. This also covers replacement/growth.
      wakeNearbyMovingBodies(interface.GetTransformedShape(body).GetWorldSpaceBounds());
    } catch (...) {
      kinematicDrivers.erase(handle);
      bodies.erase(handle);
      interface.RemoveBody(body);
      interface.DestroyBody(body);
      throw;
    }
    return handle;
  }
};

PhysicsWorld::PhysicsWorld(const PhysicsWorldSettings &settings) {
  if (!finite(settings.gravity) || settings.maxBodies == 0u ||
      settings.maxBodies > JPH::BodyID::cMaxBodyIndex + 1u)
    throw std::invalid_argument("Invalid physics world gravity or body capacity");
  // Validate before acquiring Jolt or allocating body/broad-phase storage.
  // Its contact constraint budget is lower than the body identity limit.
  (void)collisionBudget(settings.maxBodies);
  (void)workerCount(settings);
  impl_ = std::make_unique<Impl>(settings);
}

PhysicsWorld::~PhysicsWorld() = default;

PhysicsBodyPose PhysicsWorld::validatePose(PhysicsBodyPose pose) {
  validatePosition(pose.position);
  pose.rotation = normalizedRotation(pose.rotation);
  return pose;
}

PhysicsJointSettings PhysicsWorld::validateJointSettings(PhysicsJointSettings settings) {
  settings = validatedJointSettings(settings);
  const auto frame = normalizedFrame({settings.anchor, settings.axis, settings.normal});
  settings.anchor = frame.anchor;
  settings.axis = frame.axis;
  settings.normal = frame.normal;
  return settings;
}

PhysicsBodyId PhysicsWorld::createBox(glm::vec3 halfExtents,
                                      const PhysicsBodySettings &settings) {
  validateBodySettings(settings);
  if (!finite(halfExtents) || halfExtents.x <= 0.0f || halfExtents.y <= 0.0f ||
      halfExtents.z <= 0.0f)
    throw std::invalid_argument("Physics box half extents must be finite and positive");
  const float convexRadius = std::min({0.05f, halfExtents.x * 0.1f,
                                      halfExtents.y * 0.1f, halfExtents.z * 0.1f});
  return impl_->create(makeShape(JPH::BoxShapeSettings(toJolt(halfExtents),
                                                      convexRadius)), settings);
}

PhysicsBodyId PhysicsWorld::createSphere(float radius,
                                         const PhysicsBodySettings &settings) {
  validateBodySettings(settings);
  if (!std::isfinite(radius) || radius <= 0.0f)
    throw std::invalid_argument("Physics sphere radius must be finite and positive");
  return impl_->create(makeShape(JPH::SphereShapeSettings(radius)), settings);
}

PhysicsBodyId PhysicsWorld::createCapsule(float halfHeight, float radius,
                                          const PhysicsBodySettings &settings) {
  validateBodySettings(settings);
  if (!std::isfinite(halfHeight) || halfHeight < 0.0f ||
      !std::isfinite(radius) || radius <= 0.0f)
    throw std::invalid_argument("Physics capsule dimensions must be finite and nonnegative");
  return impl_->create(makeShape(JPH::CapsuleShapeSettings(halfHeight, radius)),
                        settings);
}

PhysicsBodyId PhysicsWorld::createStaticMesh(
    std::span<const glm::vec3> vertices, std::span<const uint32_t> indices,
    const PhysicsBodySettings &settings) {
  if (settings.motionType != PhysicsMotionType::Static)
    throw std::invalid_argument("Triangle mesh physics bodies must be static");
  validateBodySettings(settings);
  if (vertices.empty() || indices.empty() || indices.size() % 3u != 0u ||
      vertices.size() > std::numeric_limits<uint32_t>::max() ||
      indices.size() / 3u > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument("Physics triangle mesh requires indexed triangles");
  JPH::VertexList meshVertices;
  meshVertices.reserve(vertices.size());
  for (const auto vertex : vertices) {
    if (!finite(vertex))
      throw std::invalid_argument("Physics mesh vertices must be finite");
    meshVertices.emplace_back(vertex.x, vertex.y, vertex.z);
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(indices.size() / 3u);
  for (size_t index = 0; index < indices.size(); index += 3u) {
    const auto a = indices[index], b = indices[index + 1u], c = indices[index + 2u];
    if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
      throw std::invalid_argument("Physics mesh index is outside its vertex array");
    triangles.emplace_back(a, b, c, 0u);
  }
  return impl_->create(makeShape(JPH::MeshShapeSettings(std::move(meshVertices),
                                                        std::move(triangles))),
                        settings);
}

bool PhysicsWorld::destroyBody(PhysicsBodyId id) noexcept {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return false;
  for (;;) {
    const auto attached = impl_->jointsByBody.find(id);
    if (attached == impl_->jointsByBody.end() || attached->second.empty())
      break;
    (void)impl_->destroyJoint(*attached->second.begin());
  }
  auto &interface = impl_->system.GetBodyInterface();
  const auto oldBounds = interface.GetTransformedShape(body->second).GetWorldSpaceBounds();
  interface.RemoveBody(body->second);
  interface.DestroyBody(body->second);
  impl_->kinematicDrivers.erase(id);
  impl_->bodies.erase(body);
  impl_->wakeNearbyMovingBodies(oldBounds);
  return true;
}

bool PhysicsWorld::contains(PhysicsBodyId id) const noexcept {
  return impl_->bodies.contains(id);
}

std::optional<PhysicsBodyPose> PhysicsWorld::pose(PhysicsBodyId id) const {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return std::nullopt;
  JPH::RVec3 position;
  JPH::Quat rotation;
  impl_->system.GetBodyInterface().GetPositionAndRotation(body->second,
                                                          position, rotation);
  return PhysicsBodyPose{{position.GetX(), position.GetY(), position.GetZ()},
                         {rotation.GetW(), rotation.GetX(), rotation.GetY(),
                          rotation.GetZ()}};
}

std::optional<glm::vec3> PhysicsWorld::linearVelocity(PhysicsBodyId id) const {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return std::nullopt;
  const auto velocity = impl_->system.GetBodyInterface().GetLinearVelocity(body->second);
  return glm::vec3{velocity.GetX(), velocity.GetY(), velocity.GetZ()};
}

std::optional<glm::vec3> PhysicsWorld::angularVelocity(PhysicsBodyId id) const {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return std::nullopt;
  const auto velocity = impl_->system.GetBodyInterface().GetAngularVelocity(body->second);
  return glm::vec3{velocity.GetX(), velocity.GetY(), velocity.GetZ()};
}

bool PhysicsWorld::setPose(PhysicsBodyId id, const PhysicsBodyPose &pose,
                            bool activate) {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return false;
  validatePosition(pose.position);
  const auto rotation = toJolt(pose.rotation);
  auto &interface = impl_->system.GetBodyInterface();
  const bool isSupport = interface.GetMotionType(body->second) != JPH::EMotionType::Dynamic;
  JPH::AABox oldBounds;
  bool movedSupport = false;
  bool changedPose = false;
  bool wakeContacts = false;
  if (isSupport || activate) {
    JPH::RVec3 previousPosition;
    JPH::Quat previousRotation;
    interface.GetPositionAndRotation(body->second, previousPosition, previousRotation);
    changedPose = previousPosition != toJolt(pose.position) ||
                   (!previousRotation.IsClose(rotation) &&
                    !previousRotation.IsClose(-rotation));
    movedSupport = isSupport && changedPose;
    wakeContacts = changedPose && (isSupport || activate);
    if (wakeContacts)
      oldBounds = interface.GetTransformedShape(body->second).GetWorldSpaceBounds();
  }
  const auto connected = movedSupport || (!isSupport && activate && changedPose)
      ? impl_->dynamicJointComponent({id}, movedSupport)
      : std::span<const JPH::BodyID>{};
  interface.SetPositionAndRotation(
      body->second, toJolt(pose.position), rotation,
      activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  // Consume the borrowed component before any neighborhood wake operation.
  interface.ActivateBodies(connected.data(), static_cast<int>(connected.size()));
  // Preserve activation of an explicitly moved Kinematic support, including
  // the support-edit policy when activate=false. Incidental Kinematic hits in
  // contact neighborhoods remain asleep and do not wake unrelated branches.
  if (movedSupport && interface.GetMotionType(body->second) == JPH::EMotionType::Kinematic)
    interface.ActivateBody(body->second);
  if (wakeContacts) {
    // Teleports bypass contact-driven wakeup at the departed position. An
    // activating dynamic edit can remove a sleeping stack's support too.
    impl_->wakeNearbyMovingBodies(oldBounds);
    impl_->wakeNearbyMovingBodies(
        interface.GetTransformedShape(body->second).GetWorldSpaceBounds());
  }
  if (movedSupport) {
    // A joint can reach well beyond the support's shape bounds. Static bodies
    // cannot activate themselves, so wake their connected endpoints explicitly
    // rather than leaving a sleeping mechanism at its old pose indefinitely.
    if (const auto attached = impl_->jointsByBody.find(id);
        attached != impl_->jointsByBody.end())
      for (const auto joint : attached->second)
        interface.ActivateConstraint(impl_->joints.at(joint).constraint);
  }
  return true;
}

bool PhysicsWorld::setLinearVelocity(PhysicsBodyId id, glm::vec3 velocity) {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return false;
  if (!finite(velocity))
    throw std::invalid_argument("Physics linear velocity must be finite");
  auto &interface = impl_->system.GetBodyInterface();
  const auto motion = interface.GetMotionType(body->second);
  if (motion == JPH::EMotionType::Static)
    return false;
  const JPH::BodyCreationSettings limits;
  const auto bounded = boundedVelocity(velocity, limits.mMaxLinearVelocity);
  if (motion == JPH::EMotionType::Kinematic)
    impl_->prepareKinematicDrive(id, bounded, interface.GetAngularVelocity(body->second));
  const auto connected = bounded != interface.GetLinearVelocity(body->second) &&
      !bounded.IsNearZero()
      ? impl_->dynamicJointComponent({id}, motion == JPH::EMotionType::Kinematic)
      : std::span<const JPH::BodyID>{};
  interface.SetLinearVelocity(body->second, bounded);
  interface.ActivateBodies(connected.data(), static_cast<int>(connected.size()));
  return true;
}

bool PhysicsWorld::setAngularVelocity(PhysicsBodyId id, glm::vec3 velocity) {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return false;
  if (!finite(velocity))
    throw std::invalid_argument("Physics angular velocity must be finite");
  auto &interface = impl_->system.GetBodyInterface();
  const auto motion = interface.GetMotionType(body->second);
  if (motion == JPH::EMotionType::Static)
    return false;
  const JPH::BodyCreationSettings limits;
  const auto bounded = boundedVelocity(velocity, limits.mMaxAngularVelocity);
  if (motion == JPH::EMotionType::Kinematic)
    impl_->prepareKinematicDrive(id, interface.GetLinearVelocity(body->second), bounded);
  const auto connected = bounded != interface.GetAngularVelocity(body->second) &&
      !bounded.IsNearZero()
      ? impl_->dynamicJointComponent({id}, motion == JPH::EMotionType::Kinematic)
      : std::span<const JPH::BodyID>{};
  interface.SetAngularVelocity(body->second, bounded);
  interface.ActivateBodies(connected.data(), static_cast<int>(connected.size()));
  return true;
}

bool PhysicsWorld::moveKinematic(PhysicsBodyId id,
                                  const PhysicsBodyPose &target,
                                  float deltaSeconds) {
  const auto body = impl_->bodies.find(id);
  if (body == impl_->bodies.end()) return false;
  auto &interface = impl_->system.GetBodyInterface();
  if (interface.GetMotionType(body->second) != JPH::EMotionType::Kinematic)
    return false;
  validateDuration(deltaSeconds);
  validatePosition(target.position);
  const auto rotation = toJolt(target.rotation);
  // MoveKinematic derives velocities without clamping. Finite target poses
  // and durations can still overflow those float calculations (including
  // their squared speeds used by contacts), so validate before it mutates
  // either velocity. Keep representable fast kinematic target motion intact.
  const auto current = interface.GetTransformedShape(body->second);
  const auto targetCenter = toJolt(target.position) +
                            rotation * current.mShape->GetCenterOfMass();
  const auto linear = JPH::Vec3(targetCenter - current.mShapePositionCOM) /
                      deltaSeconds;
  const auto angular = (rotation * current.mShapeRotation.Conjugated())
                           .GetAngularVelocity(deltaSeconds);
  if (!std::isfinite(linear.LengthSq()) || !std::isfinite(angular.LengthSq()))
    throw std::invalid_argument("Physics kinematic target requires unrepresentable velocities");
  impl_->prepareKinematicDrive(id, linear, angular);
  const auto connected = !linear.IsNearZero() || !angular.IsNearZero()
      ? impl_->dynamicJointComponent({id}, true) : std::span<const JPH::BodyID>{};
  interface.MoveKinematic(body->second, toJolt(target.position), rotation,
                           deltaSeconds);
  interface.ActivateBodies(connected.data(), static_cast<int>(connected.size()));
  return true;
}

PhysicsJointId PhysicsWorld::createJoint(PhysicsBodyId bodyA, PhysicsBodyId bodyB,
                                         PhysicsJointSettings settings) {
  settings = validateJointSettings(settings);
  const auto frame = normalizedFrame({settings.anchor, settings.axis, settings.normal});
  const auto [first, second] = impl_->endpoints(bodyA, bodyB);
  return impl_->createJoint(bodyA, bodyB, settings,
                            impl_->localFrame(first, frame), impl_->localFrame(second, frame));
}

PhysicsJointId PhysicsWorld::createJointLocal(
    PhysicsBodyId bodyA, PhysicsBodyId bodyB, PhysicsJointSettings settings,
    PhysicsJointFrame frameA, PhysicsJointFrame frameB) {
  return impl_->createJoint(bodyA, bodyB, validatedJointSettings(settings), frameA, frameB);
}

bool PhysicsWorld::destroyJoint(PhysicsJointId id) noexcept {
  return impl_->destroyJoint(id);
}

bool PhysicsWorld::containsJoint(PhysicsJointId id) const noexcept {
  return impl_->joints.contains(id);
}

std::optional<PhysicsJointState> PhysicsWorld::jointState(PhysicsJointId id) const {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end())
    return std::nullopt;
  const auto &joint = found->second;
  float coordinate = 0.0f;
  if (joint.settings.type == PhysicsJointType::Hinge)
    coordinate = static_cast<const JPH::HingeConstraint *>(joint.constraint.GetPtr())->GetCurrentAngle();
  else if (joint.settings.type == PhysicsJointType::Slider)
    coordinate = static_cast<const JPH::SliderConstraint *>(joint.constraint.GetPtr())->GetCurrentPosition();
  return PhysicsJointState{joint.settings.type, joint.bodyA, joint.bodyB, coordinate,
                            joint.settings, joint.frameA, joint.frameB};
}

bool PhysicsWorld::setJointMotor(PhysicsJointId id, PhysicsJointMotorSettings motor) {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end())
    return false;
  auto &joint = found->second;
  auto settings = joint.settings;
  settings.motor = motor;
  settings = validatedJointSettings(settings);
  const auto connected = impl_->dynamicJointComponent({joint.bodyA, joint.bodyB});
  if (settings.type == PhysicsJointType::Hinge) {
    auto &hinge = *static_cast<JPH::HingeConstraint *>(joint.constraint.GetPtr());
    configureMotor(hinge, settings.motor, true);
    hinge.SetTargetAngularVelocity(settings.motor.targetVelocity);
    hinge.SetTargetAngle(settings.motor.targetPosition);
  } else if (settings.type == PhysicsJointType::Slider) {
    auto &slider = *static_cast<JPH::SliderConstraint *>(joint.constraint.GetPtr());
    configureMotor(slider, settings.motor, false);
    slider.SetTargetVelocity(settings.motor.targetVelocity);
    slider.SetTargetPosition(settings.motor.targetPosition);
  }
  joint.settings = settings;
  // A stationary kinematic endpoint is an unchanged support, not another
  // dynamic island to wake. An already-moving endpoint retains its state.
  impl_->system.GetBodyInterface().ActivateBodies(connected.data(),
                                                  static_cast<int>(connected.size()));
  return true;
}

bool PhysicsWorld::setJointLimits(PhysicsJointId id, bool enabled, float lower, float upper) {
  const auto found = impl_->joints.find(id);
  if (found == impl_->joints.end())
    return false;
  auto &joint = found->second;
  auto settings = joint.settings;
  settings.limitsEnabled = enabled;
  settings.lowerLimit = lower;
  settings.upperLimit = upper;
  settings = validatedJointSettings(settings);
  const auto connected = impl_->dynamicJointComponent({joint.bodyA, joint.bodyB});
  if (settings.type == PhysicsJointType::Hinge) {
    auto &hinge = *static_cast<JPH::HingeConstraint *>(joint.constraint.GetPtr());
    constexpr float pi = 3.14159265358979323846f;
    hinge.SetLimits(enabled ? lower : -pi, enabled ? upper : pi);
    hinge.SetTargetAngle(settings.motor.targetPosition);
  } else if (settings.type == PhysicsJointType::Slider) {
    auto &slider = *static_cast<JPH::SliderConstraint *>(joint.constraint.GetPtr());
    slider.SetLimits(enabled ? lower : -std::numeric_limits<float>::max(),
                      enabled ? upper : std::numeric_limits<float>::max());
    slider.SetTargetPosition(settings.motor.targetPosition);
  }
  joint.settings = settings;
  impl_->system.GetBodyInterface().ActivateBodies(connected.data(),
                                                  static_cast<int>(connected.size()));
  return true;
}

size_t PhysicsWorld::jointCount() const noexcept { return impl_->joints.size(); }

void PhysicsWorld::step(float deltaSeconds) {
  validateDuration(deltaSeconds);
  impl_->wakeDrivenBodies();
  const auto error = impl_->system.Update(deltaSeconds, 1, &impl_->temporary,
                                          &impl_->jobs);
  if (error != JPH::EPhysicsUpdateError::None)
    throw std::runtime_error("Physics update exhausted collision capacity (" +
                              std::to_string(static_cast<uint32_t>(error)) + ")");
}

void PhysicsWorld::clear() noexcept { impl_->clear(); }
size_t PhysicsWorld::bodyCount() const noexcept { return impl_->bodies.size(); }
size_t PhysicsWorld::activeBodyCount() const noexcept {
  return impl_->system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

} // namespace container::physics
