#include "Container/robotics/DifferentialDriveSimulation.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>

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

struct DriveProposal {
  DriveWheels target;
  DifferentialDriveRates rates;
  Motion motion;
};

struct DriveCommit {
  DifferentialDriveState state;
  std::mt19937_64 random;
  DriveWheels held;
  DifferentialDriveRates actuation;
  double groundNormalZ;
  physics::PhysicsBodyPose pose;
};

struct DriveCapsule {
  float halfHeight;
  float radius;
  physics::PhysicsBodyPose pose;
};

// Shared one-drive arithmetic. Proposal and commit preparation are local
// reads until the owner submits all targets and advances the physics world.
struct DriveKernel {
  DrivePose authoredSpawn;
  DrivePose groundedSpawn;
  DifferentialDriveSimulationOptions options;
  physics::PhysicsBodyId body{physics::kInvalidPhysicsBodyId};
  std::map<uint64_t, DriveWheels> commands;
  DriveWheels held;
  DifferentialDriveRates actuation;
  std::mt19937_64 random;
  DifferentialDriveState current;
  std::deque<DifferentialDriveState> samples;
  double groundNormalZ{1.0};

  DriveKernel(DrivePose spawn, DifferentialDriveSimulationOptions settings)
      : authoredSpawn(spawn), options(std::move(settings)), random(options.seed) {
    validateDifferentialDrive(options.drive);
    validateDriveActuators(options.actuators, options.wheelTravelVariancePerMeter);
    if (!options.maxHistorySamples || !std::isfinite(options.encoderNoiseRadians) ||
        options.encoderNoiseRadians < 0 || options.encoderNoiseRadians > 1 || options.robotId.empty())
      throw std::invalid_argument("Invalid drive history, encoder noise or robot identity");
  }

  DriveCapsule initialize(const BimWalkabilityQueries& queries) {
    groundedSpawn = integrateWheelTravel(authoredSpawn, options.drive.trackWidth, {});
    const auto support = traceWalkablePath(groundedSpawn.position, groundedSpawn.position,
                                           queries, options.capsule);
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
    return {halfHeight, radius, capsulePose(groundedSpawn, options.capsule, groundNormalZ)};
  }

  void queueRates(uint64_t currentTick, uint64_t tick, DriveWheels rates) {
    if (tick < currentTick || tick == std::numeric_limits<uint64_t>::max())
      throw std::invalid_argument("Drive command tick is outside the remaining simulation");
    if (!commands.emplace(tick, rates).second)
      throw std::invalid_argument("A drive command is already scheduled at this tick");
  }

  DriveProposal propose(uint64_t tick, double seconds, const BimWalkabilityQueries& queries) const {
    auto target = held;
    if (const auto at = commands.find(tick); at != commands.end())
      target = at->second;
    const auto& drive = options.drive;
    const auto rates = applyDriveActuators(drive, options.actuators, actuation, target, seconds);
    const auto requested = distanceForRates(rates.ground, seconds, drive.wheelRadius);
    return {target, rates, checkMotion(current.base, groundNormalZ, requested, options, queries)};
  }

  DriveCommit prepareCommit(uint64_t tick, double seconds, DriveProposal proposal) const {
    const auto& drive = options.drive;
    auto& [target, rates, movement] = proposal;
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
    // Contact is an emergency stop. The achieved travel is recorded for
    // this tick, and the actuator starts the next tick at rest.
    return {std::move(next), std::move(engine), target,
            movement.obstruction == BimWalkabilityFailure::None ? rates : DifferentialDriveRates{},
            movement.normalZ, pose};
  }

  void commit(DriveCommit prepared) noexcept {
    if (samples.size() > options.maxHistorySamples)
      samples.pop_front();
    random = std::move(prepared.random);
    held = prepared.held;
    actuation = prepared.actuation;
    groundNormalZ = prepared.groundNormalZ;
    current = std::move(prepared.state);
  }
};

struct CapsuleSweep {
  glm::dvec3 begin;
  glm::dvec3 end;
  double radius;
  double height;
};

CapsuleSweep sweptCapsule(const DriveKernel& robot, const Motion& motion) {
  const auto& settings = robot.options.capsule;
  // Include float dimensions actually passed to Jolt in the conservative
  // envelope, while retaining the double-precision ground alignment.
  const double radius = shapeLength(settings.radius);
  const double height = std::max(settings.height,
      2.0 * (radius + shapeLength(settings.height * .5 - settings.radius)));
  const auto margin = up * ((height - settings.height) * .5);
  return {walkabilityCapsuleBottom(robot.current.base.position, robot.groundNormalZ, settings) - margin,
          walkabilityCapsuleBottom(motion.pose.position, motion.normalZ, settings) - margin,
          std::max(radius, settings.radius), height};
}

// A conservative upright cylinder bounds the capsule at each instant. First
// intersect the intervals in which their vertical ranges overlap, then find
// the least horizontal separation *within the same part of the tick*. This
// catches crossings between endpoints without treating stacked floors as peers.
bool sweptPeersOverlap(const CapsuleSweep& a, const CapsuleSweep& b) {
  const auto begin = a.begin - b.begin;
  const auto delta = (a.end - b.end) - begin;
  double first = 0.0, last = 1.0;
  if (delta.z == 0.0) {
    if (begin.z <= -a.height || begin.z >= b.height)
      return false;
  } else {
    const auto enter = (-a.height - begin.z) / delta.z;
    const auto leave = (b.height - begin.z) / delta.z;
    first = std::max(0.0, std::min(enter, leave));
    last = std::min(1.0, std::max(enter, leave));
    if (first >= last)
      return false;
  }
  const glm::dvec2 position(begin), velocity(delta);
  const auto lengthSquared = glm::dot(velocity, velocity);
  const auto closest = lengthSquared == 0.0 ? first :
      std::clamp(-glm::dot(position, velocity) / lengthSquared, first, last);
  const auto separation = position + closest * velocity;
  const auto clearance = a.radius + b.radius;
  return glm::dot(separation, separation) < clearance * clearance;
}

bool capsuleMoves(const CapsuleSweep& sweep) {
  const auto delta = sweep.end - sweep.begin;
  // A stationary capsule may re-sample its support a fraction of a
  // micrometre higher or lower through Jolt's float query fractions.
  return delta.x != 0.0 || delta.y != 0.0 || std::abs(delta.z) > 1e-6;
}

std::vector<std::optional<size_t>> arbitratePeerMotion(const std::vector<DriveKernel>& robots,
                                                       const std::vector<DriveProposal>& proposals) {
  std::vector<CapsuleSweep> sweeps;
  sweeps.reserve(robots.size());
  for (size_t i = 0; i < robots.size(); ++i)
    sweeps.push_back(sweptCapsule(robots[i], proposals[i].motion));
  std::vector<std::optional<size_t>> blocked(robots.size());
  std::vector<std::optional<size_t>> stopped(robots.size());
  bool changed = false;
  do {
    changed = false;
    std::ranges::fill(blocked, std::nullopt);
    for (size_t i = 0; i < robots.size(); ++i) {
      const bool movingA = capsuleMoves(sweeps[i]);
      for (size_t j = i + 1; j < robots.size(); ++j) {
        const bool movingB = capsuleMoves(sweeps[j]);
        if ((!movingA && !movingB) || !sweptPeersOverlap(sweeps[i], sweeps[j]))
          continue;
        if (movingA && !blocked[i]) blocked[i] = j;
        if (movingB && !blocked[j]) blocked[j] = i;
      }
    }
    // Apply all stops together. A halted robot can obstruct another
    // trajectory which was safe while both moved, so recheck until stable.
    for (size_t i = 0; i < robots.size(); ++i) {
      if (!blocked[i])
        continue;
      stopped[i] = blocked[i];
      sweeps[i].end = sweeps[i].begin;
      changed = true;
    }
  } while (changed);
  return stopped;
}

} // namespace

struct DifferentialDriveSimulation::Impl {
  BimCollisionWorld& world;
  DriveKernel robot;
  SimClock time;
  bool needsReset{};

  Impl(BimCollisionWorld& collision, DrivePose spawn, DifferentialDriveSimulationOptions options)
      : world(collision), robot(spawn, std::move(options)),
        time(robot.options.ticksPerSecond, robot.options.maxTicksPerAdvance) {
    world.acquireDriveOwner();
    try {
      initialize();
    } catch (...) {
      world.releaseDriveOwner();
      throw;
    }
  }

  ~Impl() { world.releaseDriveOwner(); }

  void initialize() {
    const auto capsule = robot.initialize(world.walkabilityQueries());
    robot.body = world.attachRobot(robot.options.robotId, capsule.halfHeight, capsule.radius, capsule.pose);
  }

  void execute(uint64_t tick, double seconds) {
    if (needsReset || !world.world_.contains(robot.body))
      throw std::runtime_error("Reset the drive simulation before advancing");
    auto prepared = robot.prepareCommit(tick, seconds, robot.propose(tick, seconds, world.walkabilityQueries(robot.body)));
    robot.samples.push_back(prepared.state);
    try {
      if (!world.world_.moveKinematic(robot.body, prepared.pose, static_cast<float>(seconds)))
        throw std::runtime_error("Kinematic robot is no longer attached");
      world.world_.step(static_cast<float>(seconds));
    } catch (...) {
      robot.samples.pop_back();
      needsReset = true;
      throw;
    }
    robot.commit(std::move(prepared));
  }
};

DifferentialDriveSimulation::DifferentialDriveSimulation(BimCollisionWorld& world, DrivePose spawn,
                                                         DifferentialDriveSimulationOptions options)
    : impl_(std::make_unique<Impl>(world, spawn, std::move(options))) {}

DifferentialDriveSimulation::~DifferentialDriveSimulation() = default;

void DifferentialDriveSimulation::queueCommand(uint64_t tick, DriveTwist command) {
  auto& robot = impl_->robot;
  robot.queueRates(impl_->time.tick(), tick, differentialWheelRates(robot.options.drive, command));
}

void DifferentialDriveSimulation::queueWheelCommand(uint64_t tick, DriveWheels rates) {
  auto& robot = impl_->robot;
  robot.queueRates(impl_->time.tick(), tick, limitDifferentialWheelRates(robot.options.drive, rates));
}

void DifferentialDriveSimulation::clearCommands() noexcept {
  impl_->robot.commands.clear();
  impl_->robot.held = {};
}

uint32_t DifferentialDriveSimulation::advance(std::chrono::nanoseconds elapsed, const TickObserver& observe) {
  SimClock::Tick afterTick;
  if (observe) afterTick = [this, &observe](uint64_t, double) { observe(impl_->robot.current); };
  return impl_->time.advance(elapsed,
      [this](uint64_t tick, double seconds) { impl_->execute(tick, seconds); }, afterTick);
}

void DifferentialDriveSimulation::step(const TickObserver& observe) {
  SimClock::Tick afterTick;
  if (observe) afterTick = [this, &observe](uint64_t, double) { observe(impl_->robot.current); };
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
const DifferentialDriveState& DifferentialDriveSimulation::state() const noexcept { return impl_->robot.current; }
const std::deque<DifferentialDriveState>& DifferentialDriveSimulation::history() const noexcept { return impl_->robot.samples; }

DifferentialDriveFrames DifferentialDriveSimulation::frames() const {
  const auto& robot = impl_->robot;
  const auto& state = robot.current;
  const auto radius = robot.options.drive.wheelRadius;
  const auto halfTrack = robot.options.drive.trackWidth * .5;
  return {.mapFromOdometry = transform(robot.groundedSpawn.position, robot.groundedSpawn.yaw, up),
          .odometryFromBase = transform(state.odometry.position, state.odometry.yaw, up),
          .baseFromLeftWheel = transform({0, halfTrack, radius}, state.wheelAngles.left, wheelAxis),
          .baseFromRightWheel = transform({0, -halfTrack, radius}, state.wheelAngles.right, wheelAxis)};
}

struct DifferentialDriveFleetSimulation::Impl {
  BimCollisionWorld& world;
  std::vector<DriveKernel> robots;
  SimClock time;
  bool needsReset{};

  Impl(BimCollisionWorld& collision, std::vector<FleetRobotSpawn> spawns)
      : world(collision),
        time(spawns.empty() ? 60u : spawns.front().options.ticksPerSecond,
             spawns.empty() ? 120u : spawns.front().options.maxTicksPerAdvance) {
    if (spawns.empty() || spawns.size() > world.robotCapacity())
      throw std::invalid_argument("Fleet size must be positive and within the BIM world's robot capacity");
    const auto ticksPerSecond = spawns.front().options.ticksPerSecond;
    const auto maxTicksPerAdvance = spawns.front().options.maxTicksPerAdvance;
    const auto id = [](const FleetRobotSpawn& spawn) -> const std::string& { return spawn.options.robotId; };
    std::ranges::sort(spawns, {}, id);
    if (std::ranges::adjacent_find(spawns, {}, id) != spawns.end())
      throw std::invalid_argument("Fleet robot IDs must be unique");
    robots.reserve(spawns.size());
    for (auto& spawn : spawns) {
      if (spawn.options.ticksPerSecond != ticksPerSecond || spawn.options.maxTicksPerAdvance != maxTicksPerAdvance)
        throw std::invalid_argument("Fleet robots must use the same fixed clock configuration");
      robots.emplace_back(spawn.spawn, std::move(spawn.options));
    }
    world.acquireDriveOwner();
    try {
      initialize();
    } catch (...) {
      world.releaseDriveOwner();
      throw;
    }
  }

  ~Impl() { world.releaseDriveOwner(); }

  void initialize() {
    const auto queries = world.fleetWalkabilityQueries();
    std::vector<DriveCapsule> capsules;
    std::vector<CapsuleSweep> standing;
    capsules.reserve(robots.size());
    standing.reserve(robots.size());
    for (auto& robot : robots) {
      capsules.push_back(robot.initialize(queries));
      const auto sweep = sweptCapsule(robot, {.pose = robot.current.base, .normalZ = robot.groundNormalZ});
      for (size_t i = 0; i < standing.size(); ++i)
        if (sweptPeersOverlap(standing[i], sweep))
          throw std::invalid_argument("Fleet robot spawn envelopes overlap: " + robots[i].options.robotId +
                                      " and " + robot.options.robotId);
      standing.push_back(sweep);
    }
    for (size_t i = 0; i < robots.size(); ++i) {
      const auto& capsule = capsules[i];
      robots[i].body = world.attachRobot(robots[i].options.robotId, capsule.halfHeight, capsule.radius, capsule.pose);
    }
  }

  size_t indexOf(std::string_view robotId) const {
    const auto found = std::ranges::lower_bound(robots, robotId, {}, [](const auto& robot) -> std::string_view {
      return robot.options.robotId;
    });
    if (found == robots.end() || found->options.robotId != robotId)
      throw std::invalid_argument("No fleet robot with ID " + std::string(robotId));
    return static_cast<size_t>(found - robots.begin());
  }

  void execute(uint64_t tick, double seconds) {
    if (needsReset || std::ranges::any_of(robots, [this](const auto& robot) {
          return !world.world_.contains(robot.body);
        }))
      throw std::runtime_error("Reset the drive fleet before advancing");
    const auto queries = world.fleetWalkabilityQueries();
    std::vector<DriveProposal> proposals;
    proposals.reserve(robots.size());
    for (const auto& robot : robots) {
      auto proposal = robot.propose(tick, seconds, queries);
      const auto delta = proposal.motion.pose.position - robot.current.base.position;
      if (proposal.rates.ground.left == 0.0 && proposal.rates.ground.right == 0.0 &&
          delta.x == 0.0 && delta.y == 0.0 && std::abs(delta.z) <= 1e-6) {
        // Repeated harmless support rounding must not make a parked robot
        // drift, or publish a different final mission pose at every tick.
        proposal.motion.pose = robot.current.base;
        proposal.motion.normalZ = robot.groundNormalZ;
      }
      proposals.push_back(std::move(proposal));
    }
    const auto blocked = arbitratePeerMotion(robots, proposals);
    std::vector<DriveCommit> prepared;
    prepared.reserve(robots.size());
    for (size_t i = 0; i < robots.size(); ++i) {
      auto proposal = std::move(proposals[i]);
      if (blocked[i])
        proposal.motion = {.pose = robots[i].current.base, .normalZ = robots[i].groundNormalZ,
            .obstruction = BimWalkabilityFailure::Blocked, .blocker = robots[*blocked[i]].options.robotId};
      prepared.push_back(robots[i].prepareCommit(tick, seconds, std::move(proposal)));
    }

    size_t staged = 0;
    try {
      for (; staged < robots.size(); ++staged)
        robots[staged].samples.push_back(prepared[staged].state);
    } catch (...) {
      while (staged > 0)
        robots[--staged].samples.pop_back();
      throw;
    }
    try {
      for (size_t i = 0; i < robots.size(); ++i)
        if (!world.world_.moveKinematic(robots[i].body, prepared[i].pose, static_cast<float>(seconds)))
          throw std::runtime_error("Kinematic fleet robot is no longer attached: " + robots[i].options.robotId);
      world.world_.step(static_cast<float>(seconds));
    } catch (...) {
      for (auto& robot : robots)
        robot.samples.pop_back();
      needsReset = true;
      throw;
    }
    for (size_t i = 0; i < robots.size(); ++i)
      robots[i].commit(std::move(prepared[i]));
  }
};

DifferentialDriveFleetSimulation::DifferentialDriveFleetSimulation(BimCollisionWorld& world,
                                                                   std::vector<FleetRobotSpawn> robots)
    : impl_(std::make_unique<Impl>(world, std::move(robots))) {}

DifferentialDriveFleetSimulation::~DifferentialDriveFleetSimulation() = default;

void DifferentialDriveFleetSimulation::queueCommand(std::string_view id, uint64_t tick, DriveTwist command) {
  auto& robot = impl_->robots[impl_->indexOf(id)];
  robot.queueRates(impl_->time.tick(), tick, differentialWheelRates(robot.options.drive, command));
}

void DifferentialDriveFleetSimulation::queueWheelCommand(std::string_view id, uint64_t tick, DriveWheels rates) {
  auto& robot = impl_->robots[impl_->indexOf(id)];
  robot.queueRates(impl_->time.tick(), tick, limitDifferentialWheelRates(robot.options.drive, rates));
}

uint32_t DifferentialDriveFleetSimulation::advance(std::chrono::nanoseconds elapsed) {
  return impl_->time.advance(elapsed, [this](uint64_t tick, double seconds) { impl_->execute(tick, seconds); });
}

void DifferentialDriveFleetSimulation::step() {
  impl_->time.step([this](uint64_t tick, double seconds) { impl_->execute(tick, seconds); });
}

void DifferentialDriveFleetSimulation::reset() {
  if (impl_->time.inTickCallback()) throw std::logic_error("A tick callback cannot reset the fleet");
  impl_->needsReset = true;
  impl_->world.resetAuthoredBodies();
  impl_->initialize();
  impl_->time.reset();
  impl_->time.setPaused(true);
  impl_->needsReset = false;
}

void DifferentialDriveFleetSimulation::setPaused(bool paused) noexcept { impl_->time.setPaused(paused); }
const SimClock& DifferentialDriveFleetSimulation::clock() const noexcept { return impl_->time; }

const DifferentialDriveState& DifferentialDriveFleetSimulation::state(std::string_view id) const {
  return impl_->robots[impl_->indexOf(id)].current;
}

const std::deque<DifferentialDriveState>& DifferentialDriveFleetSimulation::history(std::string_view id) const {
  return impl_->robots[impl_->indexOf(id)].samples;
}

} // namespace container::robotics
