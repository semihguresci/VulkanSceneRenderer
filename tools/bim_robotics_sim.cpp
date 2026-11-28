#include "Container/robotics/BimAssetCache.h"
#include "Container/robotics/BimSceneSourceManifest.h"
#include "Container/robotics/CollisionSensorCamera.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/DriveSensors.h"
#include "Container/robotics/RaycastLidar.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using Json = nlohmann::json;
namespace robotics = container::robotics;
using BenchmarkClock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr std::string_view usage =
    "Usage: BimRoboticsSim INPUT.ifc|INPUT.ifcx OUTPUT.json SOURCE_NAMESPACE SCENARIO.json"
    " [--benchmark TIMINGS.json]\n"
    "       BimRoboticsSim --stream INPUT.ifc|INPUT.ifcx SOURCE_NAMESPACE SCENARIO.json"
    " [--camera CAMERA.json]\n";
constexpr uint64_t nanosPerSecond = 1'000'000'000;
constexpr uint64_t maxScenarioBytes = 4u * 1024u * 1024u;
constexpr uint64_t maxTicks = 8191;
constexpr uint64_t maxFrames = 1'000'000;
constexpr uint64_t maxLidarReturns = 1'000'000;

struct Command {
  uint64_t tick;
  std::variant<robotics::DriveTwist, robotics::DriveWheels> target;
};

struct ScenarioLidar {
  robotics::LidarSchedule schedule;
  robotics::RaycastLidar sensor;
};

struct Scenario {
  robotics::DrivePose spawn;
  robotics::DifferentialDriveSimulationOptions options;
  uint64_t ticks{};
  uint64_t elapsedNanos{};
  std::optional<uint64_t> frameNanos;
  std::vector<uint64_t> frameNanosSequence;
  std::vector<Command> commands;
  std::vector<std::string> openDoorIds;
  std::optional<double> metersPerRendererUnit;
  std::filesystem::path cacheDirectory;
  std::optional<ScenarioLidar> lidar;
  std::optional<robotics::DriveSensorModel> driveSensors;
  bool reportMotionModel{};
};

struct BenchmarkMeasurements {
  robotics::BimAssetPreprocessTimings assets;
  Nanoseconds collisionInstall{}, simulationSetup{}, physicsTicks{}, observationTime{}, lidarSampling{};
  std::string_view cacheStatus;
  uint64_t sourceBytes{};
  size_t bimElements{}, semanticEntities{}, collisionColliders{}, walkableSurfaces{};
};

class BenchmarkTimer {
public:
  explicit BenchmarkTimer(Nanoseconds* elapsed)
      : elapsed_(elapsed), start_(elapsed ? BenchmarkClock::now() : BenchmarkClock::time_point{}) {}
  ~BenchmarkTimer() {
    if (elapsed_)
      *elapsed_ += std::chrono::duration_cast<Nanoseconds>(BenchmarkClock::now() - start_);
  }
  BenchmarkTimer(const BenchmarkTimer&) = delete;
  BenchmarkTimer& operator=(const BenchmarkTimer&) = delete;

private:
  Nanoseconds* elapsed_;
  BenchmarkClock::time_point start_;
};

Json benchmarkReport(const BenchmarkMeasurements& benchmark, uint64_t ticks, size_t scans, size_t rays) {
  const auto& assets = benchmark.assets;
  const auto elapsed = [](const std::optional<Nanoseconds>& duration) {
    return duration ? Json(duration->count()) : Json(nullptr);
  };
  return {{"schema", "container.bim-robotics-cpu-benchmark"}, {"version", 1}, {"clock", "steady_clock"},
          {"assetCacheStatus", benchmark.cacheStatus}, {"physicsWorkerThreads", 1},
          {"timingsNanoseconds",
           {{"sourceReadAndFingerprint", assets.sourceReadAndFingerprint.count()},
            {"bimImport", elapsed(assets.bimImport)}, {"semanticScene", elapsed(assets.semanticScene)},
            {"collisionCook", elapsed(assets.collisionCook)}, {"walkableSurfaces", elapsed(assets.walkableSurfaces)},
            {"collisionInstall", benchmark.collisionInstall.count()}, {"simulationSetup", benchmark.simulationSetup.count()},
            {"physicsTicks", benchmark.physicsTicks.count()}, {"lidarSampling", benchmark.lidarSampling.count()}}},
          {"counts", {{"sourceBytes", benchmark.sourceBytes}, {"bimElements", benchmark.bimElements},
                      {"semanticEntities", benchmark.semanticEntities}, {"collisionColliders", benchmark.collisionColliders},
                      {"walkableSurfaces", benchmark.walkableSurfaces}, {"physicsTicks", ticks},
                      {"lidarScans", scans}, {"lidarRays", scans * rays}}}};
}

void checkKeys(const Json& object, std::initializer_list<std::string_view> keys) {
  if (!object.is_object()) throw std::invalid_argument("Expected a JSON scenario object");
  for (auto it = object.begin(); it != object.end(); ++it)
    if (std::ranges::find(keys, std::string_view(it.key())) == keys.end())
      throw std::invalid_argument("Unknown simulation field " + it.key());
}

uint64_t natural(const Json& value, std::string_view field, uint64_t upper) {
  if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
      (!value.is_number_unsigned() && value.get<int64_t>() < 0))
    throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
  const auto number = value.get<uint64_t>();
  if (number > upper) throw std::invalid_argument(std::string(field) + " exceeds the scenario limit");
  return number;
}

double real(const Json& value, std::string_view field) {
  if (!value.is_number()) throw std::invalid_argument(std::string(field) + " must be a number");
  const auto number = value.get<double>();
  if (!std::isfinite(number)) throw std::invalid_argument(std::string(field) + " must be finite");
  return number;
}

void readReal(const Json& object, const char* key, double& target) {
  if (const auto field = object.find(key); field != object.end()) target = real(*field, key);
}

glm::dvec3 initialFeet(const Json& value) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument("initialFeet must be [x, y, z] in metres");
  return {real(value[0], "initialFeet.x"), real(value[1], "initialFeet.y"), real(value[2], "initialFeet.z")};
}

void readDrive(const Json& value, robotics::DifferentialDriveSettings& drive) {
  checkKeys(value, {"wheelRadius", "trackWidth", "maxWheelRadiansPerSecond"});
  readReal(value, "wheelRadius", drive.wheelRadius);
  readReal(value, "trackWidth", drive.trackWidth);
  readReal(value, "maxWheelRadiansPerSecond", drive.maxWheelRadiansPerSecond);
}

void readCapsule(const Json& value, robotics::BimWalkabilitySettings& capsule) {
  checkKeys(value, {"radius", "height", "maxSlopeDegrees", "maxStepHeight", "maxDropHeight",
                    "sampleSpacing", "skin", "elevationTolerance", "maxSamples"});
  readReal(value, "radius", capsule.radius);
  readReal(value, "height", capsule.height);
  readReal(value, "maxSlopeDegrees", capsule.maxSlopeDegrees);
  readReal(value, "maxStepHeight", capsule.maxStepHeight);
  readReal(value, "maxDropHeight", capsule.maxDropHeight);
  readReal(value, "sampleSpacing", capsule.sampleSpacing);
  readReal(value, "skin", capsule.skin);
  readReal(value, "elevationTolerance", capsule.elevationTolerance);
  if (value.contains("maxSamples")) capsule.maxSamples = natural(value.at("maxSamples"), "maxSamples", 100'000);
}

uint64_t readFrameSequence(const Json& value, uint64_t limit, std::vector<uint64_t>& frames) {
  if (!value.is_array() || value.empty() || value.size() > maxFrames)
    throw std::invalid_argument("frameNanosecondsSequence must be a nonempty array within the frame limit");
  frames.reserve(value.size());
  uint64_t elapsed = 0;
  for (const auto& entry : value) {
    const auto frame = natural(entry, "frameNanosecondsSequence entry", limit - elapsed);
    if (frame == 0) throw std::invalid_argument("frameNanosecondsSequence entries must be positive");
    frames.push_back(frame);
    elapsed += frame;
  }
  return elapsed;
}

void readTiming(const Json& value, Scenario& scenario) {
  auto& options = scenario.options;
  if (value.contains("ticksPerSecond"))
    options.ticksPerSecond = static_cast<uint32_t>(natural(value.at("ticksPerSecond"), "ticksPerSecond", 10'000));
  if (value.contains("maxTicksPerAdvance"))
    options.maxTicksPerAdvance = static_cast<uint32_t>(natural(value.at("maxTicksPerAdvance"), "maxTicksPerAdvance", maxTicks));
  if (options.ticksPerSecond == 0 || options.maxTicksPerAdvance == 0)
    throw std::invalid_argument("Simulation tick rate and batch budget must be positive");
  const auto limit = ((maxTicks + 1) * nanosPerSecond - 1) / options.ticksPerSecond;
  std::optional<uint64_t> sequenceElapsed;
  if (value.contains("frameNanoseconds")) {
    scenario.frameNanos = natural(value.at("frameNanoseconds"), "frameNanoseconds",
                                  static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
    if (*scenario.frameNanos == 0) throw std::invalid_argument("frameNanoseconds must be positive");
  }
  if (value.contains("frameNanosecondsSequence")) {
    if (scenario.frameNanos)
      throw std::invalid_argument("Specify only one of frameNanoseconds and frameNanosecondsSequence");
    sequenceElapsed = readFrameSequence(value.at("frameNanosecondsSequence"), limit,
                                       scenario.frameNanosSequence);
  }
  const bool hasTicks = value.contains("ticks");
  if (hasTicks == value.contains("durationNanoseconds"))
    throw std::invalid_argument("Specify exactly one of ticks or durationNanoseconds");
  if (hasTicks) {
    scenario.ticks = natural(value.at("ticks"), "ticks", maxTicks);
    // The shortest whole-nanosecond duration that reaches the requested tick.
    scenario.elapsedNanos = (scenario.ticks * nanosPerSecond + options.ticksPerSecond - 1) / options.ticksPerSecond;
    if (sequenceElapsed) {
      if (*sequenceElapsed * options.ticksPerSecond / nanosPerSecond != scenario.ticks)
        throw std::invalid_argument("frameNanosecondsSequence must reach exactly the requested ticks");
      scenario.elapsedNanos = *sequenceElapsed;
    }
  } else {
    if (!scenario.frameNanos && !sequenceElapsed)
      throw std::invalid_argument("durationNanoseconds requires frameNanoseconds or frameNanosecondsSequence");
    scenario.elapsedNanos = natural(value.at("durationNanoseconds"), "durationNanoseconds", limit);
    if (sequenceElapsed && *sequenceElapsed != scenario.elapsedNanos)
      throw std::invalid_argument("frameNanosecondsSequence total must equal durationNanoseconds");
    scenario.ticks = scenario.elapsedNanos * options.ticksPerSecond / nanosPerSecond;
  }
  if (scenario.frameNanos && scenario.elapsedNanos / *scenario.frameNanos +
                                (scenario.elapsedNanos % *scenario.frameNanos != 0) > maxFrames)
    throw std::invalid_argument("Frame cadence exceeds the scenario frame limit");
  options.maxHistorySamples = static_cast<size_t>(scenario.ticks + 1);
}

glm::dvec3 xyzVector(const Json& value, std::string_view field) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument(std::string(field) + " must contain three numbers");
  return {real(value[0], field), real(value[1], field), real(value[2], field)};
}

robotics::LidarPose readMount(const Json& value) {
  checkKeys(value, {"xyz", "rpy"});
  robotics::LidarPose mount;
  if (value.contains("xyz")) mount.position = xyzVector(value.at("xyz"), "lidar.mount.xyz");
  if (value.contains("rpy")) {
    const auto rpy = xyzVector(value.at("rpy"), "lidar.mount.rpy");
    const auto roll = glm::angleAxis(rpy.x, glm::dvec3(1.0, 0.0, 0.0));
    const auto pitch = glm::angleAxis(rpy.y, glm::dvec3(0.0, 1.0, 0.0));
    const auto yaw = glm::angleAxis(rpy.z, glm::dvec3(0.0, 0.0, 1.0));
    mount.rotation = yaw * pitch * roll;
  }
  return mount;
}

ScenarioLidar readLidar(const Json& value, uint32_t ticksPerSecond, uint64_t lastTick) {
  checkKeys(value, {"scansPerSecondMilliHz", "firstTick", "azimuthSamples", "azimuthStart",
                    "azimuthSpan", "elevationAngles", "minRangeMeters", "maxRangeMeters",
                    "noiseStdDevMeters", "noiseStdDevPerMeter", "rangeResolutionMeters",
                    "dropoutProbability", "recordIdealRanges", "seed", "mount"});
  if (!value.contains("scansPerSecondMilliHz"))
    throw std::invalid_argument("lidar requires scansPerSecondMilliHz");
  const auto rate = static_cast<uint32_t>(natural(value.at("scansPerSecondMilliHz"), "scansPerSecondMilliHz",
                                                  uint64_t(ticksPerSecond) * 1000));
  const auto firstTick = value.contains("firstTick")
      ? natural(value.at("firstTick"), "firstTick", std::numeric_limits<uint64_t>::max()) : 0u;
  robotics::LidarSchedule schedule(ticksPerSecond, rate, firstTick);
  robotics::RaycastLidarSettings settings;
  if (value.contains("azimuthSamples"))
    settings.azimuthSamples = static_cast<uint32_t>(natural(value.at("azimuthSamples"), "azimuthSamples", maxLidarReturns));
  readReal(value, "azimuthStart", settings.azimuthStart);
  readReal(value, "azimuthSpan", settings.azimuthSpan);
  readReal(value, "minRangeMeters", settings.minRangeMeters);
  readReal(value, "maxRangeMeters", settings.maxRangeMeters);
  readReal(value, "noiseStdDevMeters", settings.noiseStdDevMeters);
  readReal(value, "noiseStdDevPerMeter", settings.noiseStdDevPerMeter);
  readReal(value, "rangeResolutionMeters", settings.rangeResolutionMeters);
  readReal(value, "dropoutProbability", settings.dropoutProbability);
  if (const auto record = value.find("recordIdealRanges"); record != value.end()) {
    if (!record->is_boolean())
      throw std::invalid_argument("lidar.recordIdealRanges must be a boolean");
    settings.recordIdealRanges = record->get<bool>();
  }
  if (value.contains("seed"))
    settings.seed = natural(value.at("seed"), "lidar.seed", std::numeric_limits<uint64_t>::max());
  if (value.contains("mount")) settings.baseFromSensor = readMount(value.at("mount"));
  if (const auto angles = value.find("elevationAngles"); angles != value.end()) {
    if (!angles->is_array() || angles->empty() || angles->size() > maxLidarReturns)
      throw std::invalid_argument("lidar.elevationAngles must be a nonempty array within the return limit");
    settings.elevationAngles.clear();
    settings.elevationAngles.reserve(angles->size());
    for (const auto& angle : *angles) settings.elevationAngles.push_back(real(angle, "elevationAngles"));
  }
  const auto rays = uint64_t(settings.azimuthSamples) * settings.elevationAngles.size();
  if (rays == 0 || rays > maxLidarReturns || schedule.scansThrough(lastTick) > maxLidarReturns / rays)
    throw std::invalid_argument("LiDAR scenario exceeds the one-million-return limit or requests no rays");
  return {std::move(schedule), robotics::RaycastLidar(std::move(settings))};
}

robotics::ImuAxisError readImuError(const Json& value) {
  checkKeys(value, {"bias", "noiseStdDev", "driftPerSecond"});
  robotics::ImuAxisError error;
  if (value.contains("bias")) error.bias = xyzVector(value.at("bias"), "IMU bias");
  if (value.contains("noiseStdDev")) error.noiseStdDev = xyzVector(value.at("noiseStdDev"), "IMU noiseStdDev");
  if (value.contains("driftPerSecond")) error.driftPerSecond = xyzVector(value.at("driftPerSecond"), "IMU driftPerSecond");
  return error;
}

robotics::DriveWheels readWheelPair(const Json& value, std::string_view field) {
  checkKeys(value, {"left", "right"});
  robotics::DriveWheels pair;
  if (value.contains("left")) pair.left = real(value.at("left"), std::string(field) + ".left");
  if (value.contains("right")) pair.right = real(value.at("right"), std::string(field) + ".right");
  return pair;
}

void readActuators(const Json& value, robotics::DifferentialDriveSimulationOptions& options) {
  checkKeys(value, {"maxWheelAccelerationRadiansPerSecondSquared", "tractionCoefficient", "slipRatio"});
  auto& actuators = options.actuators;
  readReal(value, "maxWheelAccelerationRadiansPerSecondSquared", actuators.maxWheelAccelerationRadiansPerSecondSquared);
  if (value.contains("tractionCoefficient"))
    actuators.tractionCoefficient = real(value.at("tractionCoefficient"), "tractionCoefficient");
  if (value.contains("slipRatio")) actuators.slipRatio = readWheelPair(value.at("slipRatio"), "slipRatio");
}

Command readCommand(const Json& value, uint64_t ticks) {
  checkKeys(value, {"tick", "linear", "angular", "wheelRates"});
  const auto tick = natural(value.at("tick"), "command tick", maxTicks);
  if (tick >= ticks) throw std::invalid_argument("A command is scheduled after the last executable tick");
  const bool wheel = value.contains("wheelRates");
  const bool linear = value.contains("linear"), angular = value.contains("angular");
  if ((wheel && (linear || angular)) || (!wheel && (!linear || !angular)))
    throw std::invalid_argument("A command must specify both linear and angular, or only wheelRates");
  if (wheel) {
    const auto& rates = value.at("wheelRates");
    checkKeys(rates, {"left", "right"});
    if (!rates.contains("left") || !rates.contains("right"))
      throw std::invalid_argument("wheelRates must contain both left and right wheel speeds in rad/s");
    return {tick, robotics::DriveWheels{real(rates.at("left"), "wheelRates.left"),
                                        real(rates.at("right"), "wheelRates.right")}};
  }
  return {tick, robotics::DriveTwist{real(value.at("linear"), "linear"), real(value.at("angular"), "angular")}};
}

robotics::EncoderObservationSettings readEncoderSettings(const Json& value) {
  checkKeys(value, {"countsPerRevolution", "biasRadians", "noiseStdDevRadians", "driftRadiansPerSecond"});
  robotics::EncoderObservationSettings settings;
  if (value.contains("countsPerRevolution"))
    settings.countsPerRevolution = static_cast<uint32_t>(natural(value.at("countsPerRevolution"),
        "countsPerRevolution", std::numeric_limits<uint32_t>::max()));
  if (value.contains("biasRadians")) settings.biasRadians = readWheelPair(value.at("biasRadians"), "biasRadians");
  if (value.contains("noiseStdDevRadians")) settings.noiseStdDevRadians = readWheelPair(value.at("noiseStdDevRadians"), "noiseStdDevRadians");
  if (value.contains("driftRadiansPerSecond"))
    settings.driftRadiansPerSecond = readWheelPair(value.at("driftRadiansPerSecond"), "driftRadiansPerSecond");
  return settings;
}

robotics::DriveSensorModel readDriveSensors(const Json& value, const Scenario& scenario) {
  checkKeys(value, {"samplesPerSecondMilliHz", "firstTick", "accelerometer", "gyroscope", "encoders", "seed"});
  if (!value.contains("samplesPerSecondMilliHz"))
    throw std::invalid_argument("driveSensors requires samplesPerSecondMilliHz");
  const auto ticksPerSecond = scenario.options.ticksPerSecond;
  const auto rate = static_cast<uint32_t>(natural(value.at("samplesPerSecondMilliHz"), "samplesPerSecondMilliHz",
                                                  uint64_t(ticksPerSecond) * 1000));
  const auto firstTick = value.contains("firstTick")
      ? natural(value.at("firstTick"), "firstTick", std::numeric_limits<uint64_t>::max()) : 0u;
  robotics::SensorSchedule schedule(ticksPerSecond, rate, firstTick);
  robotics::DriveSensorSettings settings;
  if (value.contains("accelerometer")) settings.accelerometer = readImuError(value.at("accelerometer"));
  if (value.contains("gyroscope")) settings.gyroscope = readImuError(value.at("gyroscope"));
  if (value.contains("encoders")) settings.encoders = readEncoderSettings(value.at("encoders"));
  if (value.contains("seed"))
    settings.seed = natural(value.at("seed"), "driveSensors.seed", std::numeric_limits<uint64_t>::max());
  return robotics::DriveSensorModel(schedule, scenario.options.drive, settings);
}

Scenario readScenario(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Could not open the BIM simulation scenario");
  const auto size = input.tellg();
  if (size < 0 || size > static_cast<std::streamoff>(maxScenarioBytes))
    throw std::invalid_argument("Simulation scenario exceeds the input size limit");
  input.seekg(0);
  std::string contents(static_cast<size_t>(size), '\0');
  if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())))
    throw std::runtime_error("Could not read the BIM simulation scenario");
  const auto value = Json::parse(contents);
  checkKeys(value, {"version", "initialFeet", "initialYaw", "ticks", "durationNanoseconds", "frameNanoseconds",
                    "frameNanosecondsSequence",
                    "ticksPerSecond", "maxTicksPerAdvance", "commands", "openDoorIds", "assetCacheDirectory",
                    "metersPerRendererUnit", "seed", "encoderNoiseRadians", "robotId", "drive", "capsule", "lidar",
                    "driveSensors", "actuators", "wheelTravelVariancePerMeter"});
  if (natural(value.at("version"), "version", 1) != 1)
    throw std::invalid_argument("Unsupported simulation scenario version");

  Scenario scenario;
  scenario.spawn.position = initialFeet(value.at("initialFeet"));
  readReal(value, "initialYaw", scenario.spawn.yaw);
  readTiming(value, scenario);
  if (value.contains("lidar"))
    scenario.lidar = readLidar(value.at("lidar"), scenario.options.ticksPerSecond, scenario.ticks);
  if (value.contains("drive")) readDrive(value.at("drive"), scenario.options.drive);
  if (value.contains("actuators")) {
    readActuators(value.at("actuators"), scenario.options);
    scenario.reportMotionModel = true;
  }
  if (value.contains("wheelTravelVariancePerMeter")) {
    scenario.options.wheelTravelVariancePerMeter =
        readWheelPair(value.at("wheelTravelVariancePerMeter"), "wheelTravelVariancePerMeter");
    scenario.reportMotionModel = true;
  }
  if (value.contains("driveSensors")) scenario.driveSensors = readDriveSensors(value.at("driveSensors"), scenario);
  if (value.contains("capsule")) readCapsule(value.at("capsule"), scenario.options.capsule);
  if (value.contains("seed"))
    scenario.options.seed = natural(value.at("seed"), "seed", std::numeric_limits<uint64_t>::max());
  readReal(value, "encoderNoiseRadians", scenario.options.encoderNoiseRadians);
  if (value.contains("robotId")) scenario.options.robotId = value.at("robotId").get<std::string>();
  if (value.contains("openDoorIds")) scenario.openDoorIds = value.at("openDoorIds").get<std::vector<std::string>>();
  if (value.contains("metersPerRendererUnit"))
    scenario.metersPerRendererUnit = real(value.at("metersPerRendererUnit"), "metersPerRendererUnit");
  if (value.contains("assetCacheDirectory")) {
    scenario.cacheDirectory = container::util::pathFromUtf8(value.at("assetCacheDirectory").get<std::string>());
    if (scenario.cacheDirectory.empty()) throw std::invalid_argument("assetCacheDirectory must name a directory");
    if (scenario.cacheDirectory.is_relative()) scenario.cacheDirectory = path.parent_path() / scenario.cacheDirectory;
  }

  if (const auto commands = value.find("commands"); commands != value.end()) {
    if (!commands->is_array() || commands->size() > maxTicks)
      throw std::invalid_argument("commands must be an array within the scenario command limit");
    for (const auto& input : *commands) {
      auto command = readCommand(input, scenario.ticks);
      scenario.reportMotionModel = scenario.reportMotionModel || std::holds_alternative<robotics::DriveWheels>(command.target);
      scenario.commands.push_back(std::move(command));
    }
  }
  if (scenario.reportMotionModel)
    robotics::validateDriveActuators(scenario.options.actuators, scenario.options.wheelTravelVariancePerMeter);
  return scenario;
}

std::string_view obstructionName(robotics::BimWalkabilityFailure failure) {
  using enum robotics::BimWalkabilityFailure;
  switch (failure) {
  case None: return "none";
  case MissingSupport: return "missing-support";
  case UnsupportedSurface: return "unsupported-surface";
  case SteepSurface: return "steep-surface";
  case StepTooHigh: return "step-too-high";
  case DropTooFar: return "drop-too-far";
  case Blocked: return "blocked";
  case WrongElevation: return "wrong-elevation";
  case SampleLimit: return "sample-limit";
  }
  throw std::logic_error("Unknown simulation obstruction");
}

Json xyz(glm::dvec3 value) {
  return Json::array({value.x, value.y, value.z});
}

Json pose(const robotics::DrivePose& value) {
  return {{"position", xyz(value.position)}, {"yaw", value.yaw}};
}

Json wheels(const robotics::DriveWheels& value) {
  return {{"left", value.left}, {"right", value.right}};
}

Json sample(const robotics::DifferentialDriveState& state, bool motionModel) {
  Json result{{"tick", state.tick}, {"seconds", state.seconds}, {"base", pose(state.base)},
              {"odometry", pose(state.odometry)}, {"wheelAngles", wheels(state.wheelAngles)},
              {"wheelRates", wheels(state.wheelRates)}, {"encoderAngles", wheels(state.encoderAngles)},
              {"obstruction", obstructionName(state.obstruction)}, {"blockingEntityId", state.blockingEntityId}};
  if (state.contact) {
    auto& contact = result["contact"];
    contact = {{"otherEntityId", state.blockingEntityId}, {"positionMeters", xyz(state.contact->position)}};
    if (state.contact->normal)
      contact["normal"] = xyz(*state.contact->normal);
  }
  if (motionModel) {
    result["commanded"] = state.commanded ? pose(*state.commanded) : Json(nullptr);
    result["targetWheelRates"] = wheels(state.targetWheelRates);
    result["groundWheelRates"] = wheels(state.groundWheelRates);
    result["odometryCovariance"] = state.odometryCovariance;
  }
  return result;
}

Json motionModelReport(const robotics::DifferentialDriveSimulationOptions& options) {
  const auto& actuators = options.actuators;
  return {{"actuators", {{"maxWheelAccelerationRadiansPerSecondSquared", actuators.maxWheelAccelerationRadiansPerSecondSquared},
                          {"tractionCoefficient", actuators.tractionCoefficient ? Json(*actuators.tractionCoefficient) : Json(nullptr)},
                          {"slipRatio", wheels(actuators.slipRatio)}}},
          {"wheelTravelVariancePerMeter", wheels(options.wheelTravelVariancePerMeter)}, {"wheelRateUnit", "rad/s"},
          {"odometryCovariance", {{"frame", "odometry"}, {"layout", "row-major"},
                                  {"axes", Json::array({"x", "y", "yaw"})}, {"units", Json::array({"m", "m", "rad"})}}}};
}

Json matrix(const glm::dmat4& value) {
  Json columns = Json::array();
  for (glm::length_t column = 0; column < 4; ++column)
    columns.push_back(Json::array({value[column].x, value[column].y, value[column].z, value[column].w}));
  return columns;
}

std::string_view lidarStatus(robotics::LidarReturnStatus status) {
  using enum robotics::LidarReturnStatus;
  switch (status) {
  case Valid: return "valid";
  case NoHit: return "no-hit";
  case BelowMinimum: return "below-minimum";
  case Dropped: return "dropped";
  case NoiseOutOfRange: return "noise-out-of-range";
  }
  throw std::logic_error("Unknown LiDAR return status");
}

Json lidarPose(const robotics::LidarPose& pose) {
  const auto& p = pose.position;
  const auto& q = pose.rotation;
  return {{"xyz", Json::array({p.x, p.y, p.z})}, {"quaternionXyzw", Json::array({q.x, q.y, q.z, q.w})}};
}

Json lidarScan(const robotics::LidarScan& scan, bool recordIdealRanges) {
  Json returns = Json::array();
  returns.get_ref<Json::array_t&>().reserve(scan.returns.size());
  for (const auto& hit : scan.returns) {
    Json row{{"rangeMeters", hit.rangeMeters ? Json(*hit.rangeMeters) : Json(nullptr)},
             {"status", lidarStatus(hit.status)},
             {"entityId", hit.entityId.empty() ? Json(nullptr) : Json(hit.entityId)}};
    if (recordIdealRanges)
      row["idealRangeMeters"] = hit.idealRangeMeters ? Json(*hit.idealRangeMeters) : Json(nullptr);
    returns.push_back(std::move(row));
  }
  return {{"tick", scan.tick}, {"seconds", scan.seconds}, {"worldFromSensor", lidarPose(scan.worldFromSensor)},
          {"returns", std::move(returns)}};
}

Json lidarReport(const ScenarioLidar& lidar, Json scans) {
  const auto& settings = lidar.sensor.settings();
  Json report{{"scansPerSecondMilliHz", lidar.schedule.scansPerSecondMilliHz()}, {"firstTick", lidar.schedule.firstTick()},
              {"azimuthSamples", settings.azimuthSamples}, {"azimuthStart", settings.azimuthStart},
              {"azimuthSpan", settings.azimuthSpan}, {"elevationAngles", settings.elevationAngles},
              {"minRangeMeters", settings.minRangeMeters}, {"maxRangeMeters", settings.maxRangeMeters},
              {"noiseStdDevMeters", settings.noiseStdDevMeters}, {"dropoutProbability", settings.dropoutProbability},
              {"seed", settings.seed}, {"baseFromSensor", lidarPose(settings.baseFromSensor)},
              {"raysPerScan", lidar.sensor.directions().size()}, {"scans", std::move(scans)}};
  if (settings.noiseStdDevPerMeter > 0)
    report["noiseStdDevPerMeter"] = settings.noiseStdDevPerMeter;
  if (settings.rangeResolutionMeters > 0)
    report["rangeResolutionMeters"] = settings.rangeResolutionMeters;
  if (settings.recordIdealRanges)
    report["recordIdealRanges"] = true;
  return report;
}

Json imuError(const robotics::ImuAxisError& error) {
  return {{"bias", xyz(error.bias)}, {"noiseStdDev", xyz(error.noiseStdDev)},
          {"driftPerSecond", xyz(error.driftPerSecond)}};
}

Json driveSensorSample(const robotics::DriveSensorSample& sample) {
  return {{"tick", sample.tick}, {"seconds", sample.seconds}, {"specificForce", xyz(sample.specificForce)},
          {"angularVelocity", xyz(sample.angularVelocity)},
          {"encoderCounts", {{"left", sample.encoderCounts.left}, {"right", sample.encoderCounts.right}}},
          {"encoderRadians", wheels(sample.encoderRadians)}, {"odometry", pose(sample.odometry)}};
}

Json driveSensorReport(const robotics::DriveSensorModel& sensors, Json samples) {
  const auto& schedule = sensors.schedule();
  const auto& settings = sensors.settings();
  const auto& encoders = settings.encoders;
  return {{"samplesPerSecondMilliHz", schedule.samplesPerSecondMilliHz()}, {"firstTick", schedule.firstTick()},
          {"seed", settings.seed}, {"imuFrame", "base"},
          {"accelerometer", imuError(settings.accelerometer)}, {"gyroscope", imuError(settings.gyroscope)},
          {"encoders", {{"countsPerRevolution", encoders.countsPerRevolution},
                         {"biasRadians", wheels(encoders.biasRadians)},
                         {"noiseStdDevRadians", wheels(encoders.noiseStdDevRadians)},
                         {"driftRadiansPerSecond", wheels(encoders.driftRadiansPerSecond)}}},
          {"units", {{"time", "s"}, {"specificForce", "m/s^2"}, {"angularVelocity", "rad/s"},
                     {"encoderCounts", "counts"}, {"encoderRadians", "rad"}, {"odometryPosition", "m"},
                     {"odometryYaw", "rad"}}}, {"samples", std::move(samples)}};
}

void runTicks(robotics::DifferentialDriveSimulation& simulation, const Scenario& scenario,
              const robotics::DifferentialDriveSimulation::TickObserver& observe) {
  if (!scenario.frameNanos && scenario.frameNanosSequence.empty()) {
    for (uint64_t tick = 0; tick < scenario.ticks; ++tick) simulation.step(observe);
    return;
  }
  if (scenario.frameNanos) {
    uint64_t elapsed = 0;
    while (elapsed < scenario.elapsedNanos) {
      const auto frame = std::min(*scenario.frameNanos, scenario.elapsedNanos - elapsed);
      simulation.advance(std::chrono::nanoseconds(static_cast<int64_t>(frame)), observe);
      elapsed += frame;
    }
  } else {
    for (const auto frame : scenario.frameNanosSequence)
      simulation.advance(std::chrono::nanoseconds(static_cast<int64_t>(frame)), observe);
  }
  while (simulation.clock().pendingTicks() != 0)
    if (simulation.advance(std::chrono::nanoseconds::zero(), observe) == 0)
      throw std::logic_error("Simulation paused before completing the scenario");
  if (simulation.clock().tick() != scenario.ticks)
    throw std::logic_error("Simulation did not execute the scenario's expected ticks");
}

void play(robotics::DifferentialDriveSimulation& simulation, const Scenario& scenario,
          const robotics::DifferentialDriveSimulation::TickObserver& observe, BenchmarkMeasurements* benchmark) {
  for (const auto& command : scenario.commands) {
    if (const auto* twist = std::get_if<robotics::DriveTwist>(&command.target)) simulation.queueCommand(command.tick, *twist);
    else simulation.queueWheelCommand(command.tick, std::get<robotics::DriveWheels>(command.target));
  }
  if (observe) observe(simulation.state());
  if (!benchmark) {
    runTicks(simulation, scenario, observe);
    return;
  }
  const auto observationsBeforeTicks = benchmark->observationTime;
  {
    const BenchmarkTimer timer(&benchmark->physicsTicks);
    runTicks(simulation, scenario, observe);
  }
  // Observers query the live world while each timed tick is committed.
  benchmark->physicsTicks -= benchmark->observationTime - observationsBeforeTicks;
}

bool sameFile(const std::filesystem::path& first, const std::filesystem::path& second) {
  std::error_code error;
  return std::filesystem::equivalent(first, second, error);
}

constexpr size_t maxStreamRequestBytes = 4096;
constexpr double maxStreamLinear = 10.0;
constexpr double maxStreamAngular = 50.0;

struct StreamCamera {
  robotics::SensorCameraSettings settings;
  robotics::SensorSchedule schedule;
  uint64_t lastTick;
};

StreamCamera readStreamCamera(const std::filesystem::path& path, const Scenario& scenario) {
  constexpr std::streamoff maxSpecBytes = 64 * 1024;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Could not open the drive stream camera spec");
  const auto size = input.tellg();
  if (size < 0 || size > maxSpecBytes)
    throw std::invalid_argument("Drive stream camera spec exceeds the input size limit");
  input.seekg(0);
  std::string contents(static_cast<size_t>(size), '\0');
  if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())))
    throw std::runtime_error("Could not read the drive stream camera spec");
  if (contents.find('\0') != std::string::npos)
    throw std::invalid_argument("Drive stream camera spec contains a NUL byte");
  auto spec = Json::parse(contents);
  if (!spec.is_object()) throw std::invalid_argument("Drive stream camera spec must be an object");
  if (spec.contains("worldFromBase"))
    throw std::invalid_argument("The drive stream camera uses the simulated base pose instead of worldFromBase");
  if (!spec.contains("timing")) spec["timing"] = Json::object();
  auto& timing = spec.at("timing");
  if (!timing.is_object()) throw std::invalid_argument("Drive stream camera timing must be an object");
  const auto frequency = scenario.options.ticksPerSecond;
  if (timing.contains("ticksPerSecond") &&
      natural(timing.at("ticksPerSecond"), "timing.ticksPerSecond", 1'000'000) != frequency)
    throw std::invalid_argument("Drive stream camera ticksPerSecond must match the simulation");
  const auto firstTick = timing.contains("firstTick")
      ? natural(timing.at("firstTick"), "timing.firstTick", scenario.ticks) : 0;
  const auto lastTick = timing.contains("lastTick")
      ? natural(timing.at("lastTick"), "timing.lastTick", scenario.ticks) : scenario.ticks;
  if (lastTick < firstTick) throw std::invalid_argument("Drive stream camera lastTick precedes firstTick");
  // Reuse the camera parser and its calibration rules. Only a single dummy
  // frame is needed because stream acquisition uses committed base poses.
  timing["ticksPerSecond"] = frequency;
  timing["lastTick"] = firstTick;
  auto playback = robotics::parseSensorCameraPlayback(spec.dump());
  robotics::validateCollisionSensorCameraSettings(playback.settings);
  return {std::move(playback.settings), playback.schedule, lastTick};
}

Json describeStreamCamera(const robotics::SensorCameraSettings& settings) {
  const auto& mount = settings.baseFromCamera;
  auto baseFromCamera = glm::mat4_cast(glm::normalize(mount.rotation));
  baseFromCamera[3] = glm::dvec4(mount.position, 1.0);
  return {{"width", settings.width}, {"height", settings.height}, {"fx", settings.fx}, {"fy", settings.fy},
          {"cx", settings.cx}, {"cy", settings.cy}, {"nearMeters", settings.nearMeters},
          {"farMeters", settings.farMeters}, {"baseFromCamera", matrix(baseFromCamera)}};
}

robotics::CollisionSensorColorLookup streamCameraColors(const robotics::BimPreprocessedAssets& assets) {
  std::map<std::string_view, const robotics::SemanticEntity*, std::less<>> sourceEntities, guidEntities;
  for (const auto& entity : assets.scene.entities) {
    if (!entity.sourceId.empty()) sourceEntities.try_emplace(entity.sourceId, &entity);
    if (!entity.guid.empty()) {
      const auto [entry, inserted] = guidEntities.try_emplace(entity.guid, &entity);
      if (!inserted) entry->second = nullptr;
    }
  }
  using Rgb8 = std::array<uint8_t, 3>;
  std::map<std::string, Rgb8, std::less<>> colors;
  const auto srgbByte = [](float component) {
    const auto linear = std::clamp(static_cast<double>(component), 0.0, 1.0);
    const auto srgb = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(srgb * 255.0));
  };
  for (const auto& element : assets.renderModel.elements) {
    if (element.isRepresentationOnly || element.geometryKind != container::geometry::dotbim::GeometryKind::Mesh)
      continue;
    const auto& source = element.semanticSourceId.empty() ? element.sourceId : element.semanticSourceId;
    const auto& entities = source.empty() ? guidEntities : sourceEntities;
    const auto owner = entities.find(source.empty() ? element.guid : source);
    if (owner == entities.end() || !owner->second || colors.contains(owner->second->id)) continue;
    // As in the cached render preview, use the authored PBR material factor
    // when available. Several drawables can share one collision entity ID.
    const auto color = element.materialIndex < assets.renderModel.materials.size()
        ? assets.renderModel.materials[element.materialIndex].pbr.baseColor : element.color;
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) continue;
    colors.emplace(owner->second->id, Rgb8{srgbByte(color.r), srgbByte(color.g), srgbByte(color.b)});
  }
  return [colors = std::move(colors)](std::string_view id) -> std::optional<Rgb8> {
    const auto entry = colors.find(id);
    if (entry == colors.end()) return std::nullopt;
    return entry->second;
  };
}

std::string streamBase64(std::span<const uint8_t> bytes) {
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const auto a = bytes[i];
    const auto b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const auto c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    encoded.push_back(alphabet[a >> 2]);
    encoded.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
    encoded.push_back(i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
    encoded.push_back(i + 2 < bytes.size() ? alphabet[c & 63] : '=');
  }
  return encoded;
}

Json streamImage(const StreamCamera& camera, const robotics::DifferentialDriveState& state,
                 const robotics::LidarCast& rays, const robotics::CollisionSensorColorLookup& colors) {
  if (state.tick > camera.lastTick || !camera.schedule.due(state.tick)) return nullptr;
  const robotics::SensorCameraPose worldFromBase{
      state.base.position, glm::angleAxis(state.base.yaw, glm::dvec3(0.0, 0.0, 1.0))};
  const auto frame = robotics::makeSensorCameraFrame(camera.settings, camera.schedule, state.tick, worldFromBase);
  const auto image = robotics::captureCollisionSensorCamera(camera.settings, frame, rays, colors);
  static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
  std::vector<uint8_t> depth;
  depth.reserve(image.depth32fc1.size() * 4);
  for (const auto value : image.depth32fc1) {
    const auto bits = std::bit_cast<uint32_t>(value);
    for (const auto shift : {0, 8, 16, 24}) depth.push_back(static_cast<uint8_t>(bits >> shift));
  }
  return {{"sampleIndex", frame.sampleIndex}, {"rgb8Base64", streamBase64(image.rgb8)},
          {"depth32fc1Base64", streamBase64(depth)}};
}

void validateStreamScenario(const Scenario& scenario) {
  if (!scenario.commands.empty() || scenario.frameNanos || !scenario.frameNanosSequence.empty())
    throw std::invalid_argument("Stream scenarios require ticks and no prerecorded commands or frame cadence");
  if (scenario.lidar || scenario.driveSensors)
    throw std::invalid_argument("Stream scenarios do not support batch LiDAR or drive sensor settings");
}

Json streamEnvelope(std::string_view op, bool ok) {
  return {{"schema", "container.bim-drive-stream"}, {"version", 1}, {"op", op}, {"ok", ok}};
}

void writeStreamReply(const Json& reply) {
  std::cout << reply.dump() << '\n' << std::flush;
  if (!std::cout) throw std::runtime_error("Could not write a drive stream response");
}

void writeStreamError(std::string_view op, std::string_view error) {
  auto reply = streamEnvelope(op, false);
  reply["error"] = error;
  writeStreamReply(reply);
}

Json parseStreamRequest(std::string_view line) {
  if (line.find('\0') != std::string_view::npos)
    throw std::invalid_argument("Drive stream request contains a NUL byte");
  std::set<std::string> keys;
  bool duplicate = false;
  const auto request = Json::parse(line, [&](int depth, Json::parse_event_t event, const Json& value) {
    if (depth == 1 && event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
      duplicate = true;
    return true;
  }, false);
  if (request.is_discarded()) throw std::invalid_argument("Drive stream request must be valid JSON");
  if (duplicate) throw std::invalid_argument("Drive stream request contains a duplicate field");
  return request;
}

robotics::DriveTwist readStreamCommand(const Json& request, std::string& op, uint64_t currentTick,
                                     uint64_t lastTick) {
  if (!request.is_object() || !request.contains("op") || !request.at("op").is_string())
    throw std::invalid_argument("Drive stream request must be an object with a string op");
  op = request.at("op").get<std::string>();
  if (op == "describe" || op == "observe" || op == "close") {
    checkKeys(request, {"op"});
    return {};
  }
  if (op != "step") throw std::invalid_argument("Unknown drive stream operation");
  checkKeys(request, {"op", "tick", "linearMetersPerSecond", "angularRadiansPerSecond"});
  if (!request.contains("tick") || !request.contains("linearMetersPerSecond") ||
      !request.contains("angularRadiansPerSecond"))
    throw std::invalid_argument("Drive stream step requires tick, linearMetersPerSecond and angularRadiansPerSecond");
  const auto tick = natural(request.at("tick"), "tick", std::numeric_limits<uint64_t>::max());
  if (currentTick >= lastTick) throw std::invalid_argument("Drive stream has reached the scenario tick limit");
  if (tick != currentTick + 1) throw std::invalid_argument("Drive stream step must name the next tick");
  const auto linear = real(request.at("linearMetersPerSecond"), "linearMetersPerSecond");
  const auto angular = real(request.at("angularRadiansPerSecond"), "angularRadiansPerSecond");
  if (std::abs(linear) > maxStreamLinear || std::abs(angular) > maxStreamAngular)
    throw std::invalid_argument("Drive stream step limits are 10 m/s and 50 rad/s in either direction");
  return {linear, angular};
}

Json streamSample(const robotics::DifferentialDriveSimulation& simulation, const Scenario& scenario,
                  robotics::DriveWheels previousEncoder) {
  const auto& state = simulation.state();
  auto result = sample(state, true);
  const auto frequency = scenario.options.ticksPerSecond;
  result["timestampNanoseconds"] = state.tick * nanosPerSecond / frequency;
  const auto transforms = simulation.frames();
  result["transforms"] = {{"mapFromOdometry", matrix(transforms.mapFromOdometry)},
                          {"odometryFromBase", matrix(transforms.odometryFromBase)}};
  // Use the wheel deltas that integrate odometry, including encoder noise and
  // contact clipping, so turns retain their signed rate across heading wraps.
  const auto& drive = scenario.options.drive;
  const auto left = (state.encoderAngles.left - previousEncoder.left) * drive.wheelRadius;
  const auto right = (state.encoderAngles.right - previousEncoder.right) * drive.wheelRadius;
  const auto linear = std::midpoint(left, right) * frequency;
  const auto angular = (right - left) / drive.trackWidth * frequency;
  if (!std::isfinite(linear) || !std::isfinite(angular))
    throw std::overflow_error("Drive stream odometry twist exceeds its numeric range");
  result["twist"] = {{"linearMetersPerSecond", linear}, {"angularRadiansPerSecond", angular}};
  return result;
}

void serveDriveStream(robotics::DifferentialDriveSimulation& simulation, const Scenario& scenario,
                      const Json& description, const std::optional<StreamCamera>& camera,
                      const robotics::LidarCast& rays, const robotics::CollisionSensorColorLookup& colors) {
  const auto observe = [&](robotics::DriveWheels previousEncoder) {
    auto sample = streamSample(simulation, scenario, previousEncoder);
    sample["camera"] = camera ? streamImage(*camera, simulation.state(), rays, colors) : Json(nullptr);
    return sample;
  };
  auto committed = observe(simulation.state().encoderAngles);
  std::array<char, maxStreamRequestBytes + 1> buffer{};
  for (;;) {
    std::cin.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (std::cin.bad()) throw std::runtime_error("Could not read a drive stream request");
    if (std::cin.fail()) {
      if (std::cin.eof()) break;
      std::cin.clear();
      std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      writeStreamError("", "Drive stream request exceeds 4096 bytes");
      continue;
    }
    const auto length = static_cast<size_t>(std::cin.gcount() - (std::cin.eof() ? 0 : 1));
    std::string op;
    robotics::DriveTwist command;
    try {
      command = readStreamCommand(parseStreamRequest({buffer.data(), length}), op,
                                  simulation.state().tick, scenario.ticks);
    } catch (const std::invalid_argument& error) {
      writeStreamError(op, error.what());
      continue;
    }
    auto reply = streamEnvelope(op, true);
    if (op == "step") {
      const auto previousEncoder = simulation.state().encoderAngles;
      simulation.queueCommand(simulation.state().tick, command);
      simulation.step();
      simulation.clearCommands();
      committed = observe(previousEncoder);
    }
    if (op == "describe") reply.update(description);
    if (op == "step" || op == "observe") reply.update(committed);
    writeStreamReply(reply);
    if (op == "close") break;
  }
}

void runDriveStream(const std::filesystem::path& input, const std::string& sourceNamespace,
                    const std::filesystem::path& scenarioPath, const std::optional<std::filesystem::path>& cameraPath) {
#ifdef _WIN32
  // Keep wire bytes intact, including malformed input that contains Ctrl-Z.
  if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
    throw std::runtime_error("Could not configure the drive stream pipes");
#endif
  auto scenario = readScenario(scenarioPath);
  validateStreamScenario(scenario);
  scenario.options.maxHistorySamples = 1;
  const auto camera = cameraPath ? std::optional{readStreamCamera(*cameraPath, scenario)} : std::nullopt;
  robotics::CollisionSensorColorLookup colors;
  robotics::BimSceneSource sceneSource;
  auto collision = [&] {
    auto assets = robotics::loadBimPreprocessedAssets(input, scenario.cacheDirectory,
        {.scene = {.sourceNamespace = sourceNamespace, .metersPerRendererUnit = scenario.metersPerRendererUnit},
         .maxSlopeDegrees = scenario.options.capsule.maxSlopeDegrees});
    if (!assets.cacheDiagnostic.empty()) std::cerr << assets.cacheDiagnostic << '\n';
    if (!assets.scene.frame.metersPerRendererUnit)
      throw std::invalid_argument("Simulation requires known metric units. Supply metersPerRendererUnit for an unknown source scale");
    std::vector<container::util::ContentFingerprint> sourceLayers;
    sourceLayers.reserve(assets.sourceLayers.size());
    for (auto& layer : assets.sourceLayers) sourceLayers.push_back(std::move(layer.fingerprint));
    sceneSource = robotics::makeBimSceneSource(std::move(sourceLayers), assets.scene.frame);
    if (camera) colors = streamCameraColors(assets);
    return std::move(assets.collision);
  }();
  const auto& capsule = scenario.options.capsule;
  const Json description{{"ticksPerSecond", scenario.options.ticksPerSecond}, {"tickLimit", scenario.ticks},
                         {"robotId", scenario.options.robotId},
                         {"sourceNamespace", sourceNamespace}, {"sceneSource", robotics::bimSceneSourceJson(sceneSource)},
                         {"capsule", {{"radiusMeters", capsule.radius}, {"heightMeters", capsule.height}}},
                         {"openDoorIds", scenario.openDoorIds},
                         {"camera", camera ? describeStreamCamera(camera->settings) : Json(nullptr)}};
  robotics::BimCollisionWorld world(std::move(collision), scenario.openDoorIds, 1);
  robotics::DifferentialDriveSimulation simulation(world, scenario.spawn, scenario.options);
  const auto rays = camera ? world.lidarRaycasts(scenario.options.robotId) : robotics::LidarCast{};
  serveDriveStream(simulation, scenario, description, camera, rays, colors);
}

template<class Char>
int run(int count, Char** args) {
  if (count == 2 && std::filesystem::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count >= 2 && std::filesystem::path(args[1]) == "--stream") {
    if (count != 5 && (count != 7 || std::filesystem::path(args[5]) != "--camera")) {
      std::cerr << usage;
      return 2;
    }
    try {
      runDriveStream(std::filesystem::path(args[2]), container::util::pathToUtf8(std::filesystem::path(args[3])),
                     std::filesystem::path(args[4]),
                     count == 7 ? std::optional{std::filesystem::path(args[6])} : std::nullopt);
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "BimRoboticsSim: " << error.what() << '\n';
      return 1;
    }
  }
  if ((count != 5 && count != 7) ||
      (count == 7 && std::filesystem::path(args[5]) != "--benchmark")) {
    std::cerr << usage;
    return 2;
  }
  try {
    const std::filesystem::path input(args[1]), output(args[2]), scenarioPath(args[4]);
    if (output.extension() != ".json") throw std::invalid_argument("Output must be a .json trajectory report");
    if (sameFile(output, scenarioPath) || sameFile(output, input))
      throw std::invalid_argument("Output must be separate from the scene and scenario inputs");
    std::filesystem::path benchmarkPath;
    BenchmarkMeasurements measurements;
    BenchmarkMeasurements* benchmark = nullptr;
    if (count == 7) {
      benchmarkPath = args[6];
      if (benchmarkPath.extension() != ".json")
        throw std::invalid_argument("Benchmark output must be a .json timing sidecar");
      if (std::filesystem::exists(std::filesystem::symlink_status(benchmarkPath)) ||
          std::filesystem::weakly_canonical(benchmarkPath) == std::filesystem::weakly_canonical(output))
        throw std::invalid_argument("Benchmark timing output must be a fresh file separate from the simulation report");
      benchmark = &measurements;
    }
    const auto sourceNamespace = container::util::pathToUtf8(std::filesystem::path(args[3]));
    auto scenario = readScenario(scenarioPath);
    Json assetCache;
    robotics::BimSceneSource sceneSource;
    auto collision = [&] {
      auto assets = robotics::loadBimPreprocessedAssets(input, scenario.cacheDirectory,
          {.scene = {.sourceNamespace = sourceNamespace, .metersPerRendererUnit = scenario.metersPerRendererUnit},
           .maxSlopeDegrees = scenario.options.capsule.maxSlopeDegrees}, {},
          benchmark ? &benchmark->assets : nullptr);
      if (!assets.cacheDiagnostic.empty()) std::cerr << assets.cacheDiagnostic << '\n';
      if (!assets.scene.frame.metersPerRendererUnit)
        throw std::invalid_argument("Simulation requires known metric units. Supply metersPerRendererUnit for an unknown source scale");
      std::vector<container::util::ContentFingerprint> sourceLayers;
      sourceLayers.reserve(assets.sourceLayers.size());
      for (auto& layer : assets.sourceLayers)
        sourceLayers.push_back(std::move(layer.fingerprint));
      sceneSource = robotics::makeBimSceneSource(std::move(sourceLayers), assets.scene.frame);
      if (!scenario.cacheDirectory.empty())
        assetCache = {{"key", assets.cacheKey}, {"producerRevision", assets.producerRevision},
                      {"status", container::util::derivedAssetCacheStatusName(assets.cacheStatus)}};
      if (benchmark) {
        benchmark->cacheStatus = container::util::derivedAssetCacheStatusName(assets.cacheStatus);
        benchmark->bimElements = assets.renderModel.elements.size();
        benchmark->semanticEntities = assets.scene.entities.size();
        benchmark->collisionColliders = assets.collision.colliders.size();
        benchmark->walkableSurfaces = assets.walkableSurfaces.size();
        for (const auto& layer : sceneSource.layers)
          benchmark->sourceBytes += layer.byteSize;
      }
      return std::move(assets.collision);
    }();
    auto world = [&] {
      const BenchmarkTimer timer(benchmark ? &benchmark->collisionInstall : nullptr);
      return robotics::BimCollisionWorld(std::move(collision), scenario.openDoorIds, 1);
    }();
    auto simulation = [&] {
      const BenchmarkTimer timer(benchmark ? &benchmark->simulationSetup : nullptr);
      return robotics::DifferentialDriveSimulation(world, scenario.spawn, scenario.options);
    }();
    Json scans = Json::array();
    Json driveSamples = Json::array();
    robotics::DifferentialDriveSimulation::TickObserver observe;
    if (scenario.lidar)
      scans.get_ref<Json::array_t&>().reserve(static_cast<size_t>(scenario.lidar->schedule.scansThrough(scenario.ticks)));
    if (scenario.driveSensors)
      driveSamples.get_ref<Json::array_t&>().reserve(static_cast<size_t>(
          scenario.driveSensors->schedule().samplesThrough(scenario.ticks)));
    if (scenario.lidar || scenario.driveSensors) {
      auto cast = scenario.lidar ? world.lidarRaycasts() : robotics::LidarCast{};
      observe = [&scenario, &scans, &driveSamples, benchmark,
                 cast = std::move(cast)](const robotics::DifferentialDriveState& state) {
        const BenchmarkTimer timer(benchmark ? &benchmark->observationTime : nullptr);
        if (const auto& lidar = scenario.lidar; lidar && lidar->schedule.due(state.tick)) {
          const robotics::LidarPose worldFromBase{
              state.base.position, glm::angleAxis(state.base.yaw, glm::dvec3(0.0, 0.0, 1.0))};
          const auto scan = [&] {
            const BenchmarkTimer lidarTimer(benchmark ? &benchmark->lidarSampling : nullptr);
            return lidar->sensor.scan(worldFromBase, lidar->schedule, state.tick, cast);
          }();
          scans.push_back(lidarScan(scan, lidar->sensor.settings().recordIdealRanges));
        }
        if (auto& sensors = scenario.driveSensors) {
          const auto reading = sensors->observe({.tick = state.tick, .base = state.base,
              .encoderRadians = state.encoderAngles,
              .yawRateRadiansPerSecond = robotics::differentialYawRate(scenario.options.drive, state.groundWheelRates)});
          if (reading) driveSamples.push_back(driveSensorSample(*reading));
        }
      };
    }
    play(simulation, scenario, observe, benchmark);
    if (scenario.lidar && scans.size() != scenario.lidar->schedule.scansThrough(scenario.ticks))
      throw std::logic_error("Simulation did not produce every scheduled LiDAR scan");
    if (scenario.driveSensors && driveSamples.size() != scenario.driveSensors->schedule().samplesThrough(scenario.ticks))
      throw std::logic_error("Simulation did not produce every scheduled drive sensor sample");

    Json history = Json::array();
    for (const auto& entry : simulation.history()) history.push_back(sample(entry, scenario.reportMotionModel));
    if (history.size() != scenario.ticks + 1)
      throw std::logic_error("Simulation history does not contain every requested tick");
    const auto transforms = simulation.frames();
    Json report{{"schema", "container.bim-robotics-simulation"}, {"version", 1},
                {"sourceNamespace", sourceNamespace}, {"robotId", scenario.options.robotId},
                {"sceneSource", robotics::bimSceneSourceJson(sceneSource)},
                {"coordinateConvention", "ROS REP-103"}, {"matrixLayout", "column-major"},
                {"seed", scenario.options.seed}, {"ticksPerSecond", scenario.options.ticksPerSecond},
                {"state", sample(simulation.state(), scenario.reportMotionModel)}, {"history", std::move(history)},
                {"transforms", {{"mapFromOdometry", matrix(transforms.mapFromOdometry)},
                                {"odometryFromBase", matrix(transforms.odometryFromBase)},
                                {"baseFromLeftWheel", matrix(transforms.baseFromLeftWheel)},
                                {"baseFromRightWheel", matrix(transforms.baseFromRightWheel)}}}};
    if (!assetCache.is_null()) report["assetCache"] = std::move(assetCache);
    if (scenario.lidar) report["lidar"] = lidarReport(*scenario.lidar, std::move(scans));
    if (scenario.driveSensors) report["driveSensors"] = driveSensorReport(*scenario.driveSensors, std::move(driveSamples));
    if (scenario.reportMotionModel) report["motionModel"] = motionModelReport(scenario.options);
    container::util::writeFileAtomically(output, report.dump(2) + '\n');
    if (benchmark) {
      const auto rays = scenario.lidar ? scenario.lidar->sensor.directions().size() : 0;
      const auto timing = benchmarkReport(*benchmark, simulation.clock().tick(),
                                          scenario.lidar ? scenario.lidar->schedule.scansThrough(scenario.ticks) : 0,
                                          rays);
      container::util::writeFileAtomically(benchmarkPath, timing.dump(2) + '\n',
                                          container::util::AtomicFilePublication::CreateNew);
    }
    std::cout << "Simulated " << simulation.clock().tick() << " fixed ticks\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsSim: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
