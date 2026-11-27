#include "Container/robotics/DifferentialDriveSimulation.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>

namespace container::robotics {
namespace {

constexpr auto up = glm::dvec3(0, 0, 1);
constexpr auto wheelAxis = glm::dvec3(0, 1, 0);

float shapeLength(double value) {
  if (!std::isfinite(value) || value < 0 || value > std::numeric_limits<float>::max())
    throw std::invalid_argument("Drive capsule dimension exceeds the physics range");
  return static_cast<float>(value);
}

physics::PhysicsBodyPose capsulePose(const DrivePose& base, const BimWalkabilitySettings& capsule,
                                     double normalZ) {
  return {walkabilityCapsuleBottom(base.position, normalZ, capsule) + up * (capsule.height * .5),
          glm::angleAxis(static_cast<float>(base.yaw), glm::vec3(up)) *
          glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0))};
}

glm::dmat4 transform(glm::dvec3 position, double angle, glm::dvec3 axis) {
  return glm::rotate(glm::translate(glm::dmat4(1.0), position), angle, axis);
}

double encoderJitter(std::mt19937_64& generator, double deviation) {
  // Use fixed engine bits rather than a library-dependent real distribution.
  const double uniform = static_cast<double>(generator() >> 11) * 0x1.0p-53;
  return (2 * uniform - 1) * std::sqrt(3.0) * deviation;
}

DriveWheels distanceForRates(DriveWheels rates, double seconds, double radius) {
  return {rates.left * seconds * radius, rates.right * seconds * radius};
}

double wheelDistanceVariance(double shaftRadians, double radius, double variancePerMeter,
                             double jitterRadians) {
  if (shaftRadians == 0) return 0;
  const auto jitterMeters = jitterRadians * radius;
  const auto travelVariance = variancePerMeter == 0 ? 0 :
      std::abs(shaftRadians * radius) * variancePerMeter;
  return travelVariance + jitterMeters * jitterMeters;
}

double targetHeight(glm::dvec3 from, glm::dvec3 to,
                    const BimWalkabilityQueries& queries, const BimWalkabilitySettings& capsule) {
  const double margin = std::max(2 * capsule.skin, 1e-6);
  const double top = capsule.maxStepHeight + margin;
  to.z = from.z + top;
  const auto floor = queries.castSegment(to, -up * (top + capsule.maxDropHeight + margin));
  return floor && floor->walkableSupport ? floor->position.z : from.z;
}

struct Motion {
  DrivePose pose;
  double normalZ;
  double acceptedFraction{};
  BimWalkabilityFailure obstruction{BimWalkabilityFailure::None};
  std::string blocker;
};

Motion checkMotion(DrivePose start, double initialNormalZ, DriveWheels travel,
                   const DifferentialDriveSimulationOptions& options,
                   const BimWalkabilityQueries& queries) {
  Motion motion{.pose = start, .normalZ = initialNormalZ};
  const double distance = std::midpoint(travel.left, travel.right);
  const double turn = travel.right / options.drive.trackWidth - travel.left / options.drive.trackWidth;
  const double reach = std::min(options.capsule.radius, options.capsule.sampleSpacing) * .25;
  const double demand = std::max({1.0, std::ceil(std::abs(distance) / reach), std::ceil(std::abs(turn) / .1)});
  if (!std::isfinite(distance) || !std::isfinite(turn) || !std::isfinite(demand) || demand > 256) {
    motion.obstruction = BimWalkabilityFailure::SampleLimit;
    return motion;
  }
  const auto segments = static_cast<uint32_t>(demand);
  const DriveWheels segment{travel.left / segments, travel.right / segments};
  auto capsule = options.capsule;
  // The arc stays within this sagitta bound of each checked straight chord.
  // Extra lateral clearance keeps a thin obstacle outside a chord visible.
  capsule.radius += std::abs(distance / segments) * std::abs(turn / segments) / 8;
  capsule.height = std::max(capsule.height, 2 * capsule.radius);
  for (uint32_t index = 0; index < segments; ++index) {
    auto next = integrateWheelTravel(motion.pose, options.drive.trackWidth, segment);
    next.position.z = targetHeight(motion.pose.position, next.position, queries, capsule);
    const auto walk = traceWalkablePath(motion.pose.position, next.position, queries, capsule);
    if (!walk.reachable) {
      motion.obstruction = walk.reason;
      if (walk.blockingHit) motion.blocker = walk.blockingHit->entityId;
      break;
    }
    next.position = walk.reachedFeet;
    motion.pose = next;
    motion.normalZ = walk.samples.back().normal.z;
    motion.acceptedFraction = double(index + 1) / segments;
  }
  if (motion.acceptedFraction > 0) {
    // Jolt moves the body along a single chord during its fixed tick. Certify
    // that actual path as well as the wheel arc and the support samples.
    const auto from = walkabilityCapsuleBottom(start.position, initialNormalZ, options.capsule);
    const auto to = walkabilityCapsuleBottom(motion.pose.position, motion.normalZ, options.capsule);
    if (const auto hit = queries.sweepCapsule(from, to - from, options.capsule.radius,
                                             options.capsule.height)) {
      return {.pose = start, .normalZ = initialNormalZ,
              .obstruction = BimWalkabilityFailure::Blocked, .blocker = hit->entityId};
    }
  }
  return motion;
}

} // namespace

struct DifferentialDriveSimulation::Impl {
  BimCollisionWorld& world;
  DrivePose authoredSpawn;
  DrivePose groundedSpawn;
  DifferentialDriveSimulationOptions options;
  SimClock time;
  std::map<uint64_t, DriveWheels> commands;
  DriveWheels held;
  DifferentialDriveRates actuation;
  std::mt19937_64 random;
  DifferentialDriveState current;
  std::deque<DifferentialDriveState> samples;
  double groundNormalZ{1.0};
  bool needsReset{};

  Impl(BimCollisionWorld& collision, DrivePose spawn, DifferentialDriveSimulationOptions settings)
      : world(collision), authoredSpawn(spawn), options(std::move(settings)),
        time(options.ticksPerSecond, options.maxTicksPerAdvance), random(options.seed) {
    validateDifferentialDrive(options.drive);
    validateDriveActuators(options.actuators, options.wheelTravelVariancePerMeter);
    if (!options.maxHistorySamples || !std::isfinite(options.encoderNoiseRadians) ||
        options.encoderNoiseRadians < 0 || options.encoderNoiseRadians > 1 || options.robotId.empty())
      throw std::invalid_argument("Invalid drive history, encoder noise or robot identity");
    if (world.robotBody_ != physics::kInvalidPhysicsBodyId)
      throw std::invalid_argument("A differential drive is already attached to this BIM world");
    if (std::ranges::any_of(world.geometry_.colliders, [&](const auto& row) { return row.entityId == options.robotId; }))
      throw std::invalid_argument("Robot identity collides with a BIM entity ID");
    initialize();
  }

  void initialize() {
    groundedSpawn = integrateWheelTravel(authoredSpawn, options.drive.trackWidth, {});
    const auto support = traceWalkablePath(groundedSpawn.position, groundedSpawn.position,
                                           world.walkabilityQueries(), options.capsule);
    if (!support.reachable)
      throw std::invalid_argument("Robot spawn needs clear capsule space on supported ground");
    groundedSpawn.position = support.reachedFeet;
    groundNormalZ = support.samples.back().normal.z;
    current = {.base = groundedSpawn, .commanded = groundedSpawn};
    samples.clear();
    samples.push_back(current);
    held = {};
    actuation = {};
    random.seed(options.seed);

    const auto radius = shapeLength(options.capsule.radius);
    const auto halfHeight = shapeLength(options.capsule.height * .5 - options.capsule.radius);
    if (radius == 0)
      throw std::invalid_argument("Robot radius is too small for the physics world");
    const auto pose = capsulePose(groundedSpawn, options.capsule, groundNormalZ);
    world.robotEntityId_ = options.robotId;
    world.robotHalfHeight_ = halfHeight;
    world.robotRadius_ = radius;
    world.robotBody_ = world.world_.createCapsule(halfHeight, radius,
        {.motionType = physics::PhysicsMotionType::Kinematic, .position = pose.position,
         .rotation = pose.rotation});
  }

  void queueRates(uint64_t tick, DriveWheels rates) {
    if (tick < time.tick() || tick == std::numeric_limits<uint64_t>::max())
      throw std::invalid_argument("Drive command tick is outside the remaining simulation");
    if (!commands.emplace(tick, rates).second)
      throw std::invalid_argument("A drive command is already scheduled at this tick");
  }

  void execute(uint64_t tick, double seconds) {
    if (needsReset || !world.world_.contains(world.robotBody_))
      throw std::runtime_error("Reset the drive simulation before advancing");
    auto target = held;
    if (const auto at = commands.find(tick); at != commands.end())
      target = at->second;
    const auto& drive = options.drive;
    const auto rates = applyDriveActuators(drive, options.actuators, actuation, target, seconds);
    const auto requested = distanceForRates(rates.ground, seconds, drive.wheelRadius);
    const auto queries = world.walkabilityQueries(world.robotBody_);
    auto movement = checkMotion(current.base, groundNormalZ, requested, options, queries);
    DifferentialDriveState next = current;
    next.tick = tick + 1;
    next.seconds = double(next.tick) / options.ticksPerSecond;
    next.base = movement.pose;
    next.obstruction = movement.obstruction;
    next.blockingEntityId = std::move(movement.blocker);
    next.targetWheelRates = target;
    next.wheelRates = {rates.shaft.left * movement.acceptedFraction, rates.shaft.right * movement.acceptedFraction};
    next.groundWheelRates = {rates.ground.left * movement.acceptedFraction, rates.ground.right * movement.acceptedFraction};
    if (next.commanded) {
      try {
        next.commanded = integrateWheelTravel(*next.commanded, drive.trackWidth,
                                             distanceForRates(target, seconds, drive.wheelRadius));
      } catch (const std::invalid_argument&) {
        // A nominal path outside the numeric range must not stop a safe
        // collision-limited simulation.
        next.commanded.reset();
      }
    }

    const DriveWheels angle{next.wheelRates.left * seconds, next.wheelRates.right * seconds};
    next.wheelAngles = {current.wheelAngles.left + angle.left, current.wheelAngles.right + angle.right};
    auto engine = random;
    const DriveWheels encoder{
        angle.left + (angle.left && options.encoderNoiseRadians ? encoderJitter(engine, options.encoderNoiseRadians) : 0),
        angle.right + (angle.right && options.encoderNoiseRadians ? encoderJitter(engine, options.encoderNoiseRadians) : 0)};
    next.encoderAngles = {current.encoderAngles.left + encoder.left, current.encoderAngles.right + encoder.right};
    if (!std::isfinite(next.wheelAngles.left) || !std::isfinite(next.wheelAngles.right) ||
        !std::isfinite(next.encoderAngles.left) || !std::isfinite(next.encoderAngles.right))
      throw std::overflow_error("Robot wheel history exceeded its numeric range");
    const DriveWheels measured{encoder.left * drive.wheelRadius, encoder.right * drive.wheelRadius};
    next.odometry = integrateWheelTravel(current.odometry, drive.trackWidth, measured);
    const auto variance = options.wheelTravelVariancePerMeter;
    if ((angle.left != 0 || angle.right != 0) &&
        (variance.left > 0 || variance.right > 0 || options.encoderNoiseRadians > 0)) {
      const DriveWheels distanceVariance{
          wheelDistanceVariance(angle.left, drive.wheelRadius, variance.left, options.encoderNoiseRadians),
          wheelDistanceVariance(angle.right, drive.wheelRadius, variance.right, options.encoderNoiseRadians)};
      next.odometryCovariance = propagatePlanarOdometryCovariance(current.odometryCovariance,
          current.odometry, drive.trackWidth, measured, distanceVariance);
    }
    // Wheel odometry has planar coordinates and no direct elevation sensor.
    const auto pose = capsulePose(next.base, options.capsule, movement.normalZ);
    samples.push_back(next);
    try {
      if (!world.world_.moveKinematic(world.robotBody_, pose, static_cast<float>(seconds)))
        throw std::runtime_error("Kinematic robot is no longer attached");
      world.world_.step(static_cast<float>(seconds));
    } catch (...) {
      samples.pop_back();
      needsReset = true;
      throw;
    }
    if (samples.size() > options.maxHistorySamples)
      samples.pop_front();
    random = engine;
    held = target;
    // Contact is an emergency stop. The achieved travel is reported for
    // this tick, and the actuator starts the next tick at rest.
    actuation = movement.obstruction == BimWalkabilityFailure::None ? rates : DifferentialDriveRates{};
    groundNormalZ = movement.normalZ;
    current = std::move(next);
  }
};

DifferentialDriveSimulation::DifferentialDriveSimulation(BimCollisionWorld& world, DrivePose spawn,
                                                         DifferentialDriveSimulationOptions options)
    : impl_(std::make_unique<Impl>(world, spawn, std::move(options))) {}

DifferentialDriveSimulation::~DifferentialDriveSimulation() {
  auto& world = impl_->world;
  (void)world.world_.destroyBody(world.robotBody_);
  world.robotBody_ = physics::kInvalidPhysicsBodyId;
  world.robotEntityId_.clear();
}

void DifferentialDriveSimulation::queueCommand(uint64_t tick, DriveTwist command) {
  impl_->queueRates(tick, differentialWheelRates(impl_->options.drive, command));
}

void DifferentialDriveSimulation::queueWheelCommand(uint64_t tick, DriveWheels rates) {
  impl_->queueRates(tick, limitDifferentialWheelRates(impl_->options.drive, rates));
}

void DifferentialDriveSimulation::clearCommands() noexcept {
  impl_->commands.clear();
  impl_->held = {};
}

uint32_t DifferentialDriveSimulation::advance(std::chrono::nanoseconds elapsed, const TickObserver& observe) {
  SimClock::Tick afterTick;
  if (observe) afterTick = [this, &observe](uint64_t, double) { observe(impl_->current); };
  return impl_->time.advance(elapsed,
      [this](uint64_t tick, double seconds) { impl_->execute(tick, seconds); }, afterTick);
}

void DifferentialDriveSimulation::step(const TickObserver& observe) {
  SimClock::Tick afterTick;
  if (observe) afterTick = [this, &observe](uint64_t, double) { observe(impl_->current); };
  impl_->time.step([this](uint64_t tick, double seconds) { impl_->execute(tick, seconds); }, afterTick);
}

void DifferentialDriveSimulation::reset() {
  if (impl_->time.inTickCallback()) throw std::logic_error("A tick callback cannot reset the simulation");
  impl_->needsReset = true;
  impl_->world.resetAuthoredBodies();
  impl_->initialize();
  impl_->time.reset();
  impl_->time.setPaused(true);
  impl_->needsReset = false;
}

void DifferentialDriveSimulation::setPaused(bool paused) noexcept { impl_->time.setPaused(paused); }
const SimClock& DifferentialDriveSimulation::clock() const noexcept { return impl_->time; }
const DifferentialDriveState& DifferentialDriveSimulation::state() const noexcept { return impl_->current; }
const std::deque<DifferentialDriveState>& DifferentialDriveSimulation::history() const noexcept { return impl_->samples; }

DifferentialDriveFrames DifferentialDriveSimulation::frames() const {
  const auto& state = impl_->current;
  const auto radius = impl_->options.drive.wheelRadius;
  const auto halfTrack = impl_->options.drive.trackWidth * .5;
  return {.mapFromOdometry = transform(impl_->groundedSpawn.position, impl_->groundedSpawn.yaw, up),
          .odometryFromBase = transform(state.odometry.position, state.odometry.yaw, up),
          .baseFromLeftWheel = transform({0, halfTrack, radius}, state.wheelAngles.left, wheelAxis),
          .baseFromRightWheel = transform({0, -halfTrack, radius}, state.wheelAngles.right, wheelAxis)};
}

} // namespace container::robotics
