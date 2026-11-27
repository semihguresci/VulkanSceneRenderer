#include "Container/robotics/BimAssetCache.h"
#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
namespace robotics = container::robotics;

constexpr std::string_view usage =
    "Usage: BimRoboticsSim INPUT.ifc|INPUT.ifcx OUTPUT.json SOURCE_NAMESPACE SCENARIO.json\n";
constexpr uint64_t nanosPerSecond = 1'000'000'000;
constexpr uint64_t maxScenarioBytes = 4u * 1024u * 1024u;
constexpr uint64_t maxTicks = 8191;
constexpr uint64_t maxFrames = 1'000'000;

struct Command {
  uint64_t tick;
  robotics::DriveTwist twist;
};

struct Scenario {
  robotics::DrivePose spawn;
  robotics::DifferentialDriveSimulationOptions options;
  uint64_t ticks{};
  uint64_t elapsedNanos{};
  std::optional<uint64_t> frameNanos;
  std::vector<Command> commands;
  std::vector<std::string> openDoorIds;
  std::optional<double> metersPerRendererUnit;
  std::filesystem::path cacheDirectory;
};

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

void readTiming(const Json& value, Scenario& scenario) {
  auto& options = scenario.options;
  if (value.contains("ticksPerSecond"))
    options.ticksPerSecond = static_cast<uint32_t>(natural(value.at("ticksPerSecond"), "ticksPerSecond", 10'000));
  if (value.contains("maxTicksPerAdvance"))
    options.maxTicksPerAdvance = static_cast<uint32_t>(natural(value.at("maxTicksPerAdvance"), "maxTicksPerAdvance", maxTicks));
  if (options.ticksPerSecond == 0 || options.maxTicksPerAdvance == 0)
    throw std::invalid_argument("Simulation tick rate and batch budget must be positive");
  if (value.contains("frameNanoseconds")) {
    scenario.frameNanos = natural(value.at("frameNanoseconds"), "frameNanoseconds",
                                  static_cast<uint64_t>(std::chrono::nanoseconds::max().count()));
    if (*scenario.frameNanos == 0) throw std::invalid_argument("frameNanoseconds must be positive");
  }
  const bool hasTicks = value.contains("ticks");
  if (hasTicks == value.contains("durationNanoseconds"))
    throw std::invalid_argument("Specify exactly one of ticks or durationNanoseconds");
  if (hasTicks) {
    scenario.ticks = natural(value.at("ticks"), "ticks", maxTicks);
    // The shortest whole-nanosecond duration that reaches the requested tick.
    scenario.elapsedNanos = (scenario.ticks * nanosPerSecond + options.ticksPerSecond - 1) / options.ticksPerSecond;
  } else {
    if (!scenario.frameNanos) throw std::invalid_argument("durationNanoseconds requires frameNanoseconds");
    const auto limit = ((maxTicks + 1) * nanosPerSecond - 1) / options.ticksPerSecond;
    scenario.elapsedNanos = natural(value.at("durationNanoseconds"), "durationNanoseconds", limit);
    scenario.ticks = scenario.elapsedNanos * options.ticksPerSecond / nanosPerSecond;
  }
  if (scenario.frameNanos && scenario.elapsedNanos / *scenario.frameNanos +
                                (scenario.elapsedNanos % *scenario.frameNanos != 0) > maxFrames)
    throw std::invalid_argument("Frame cadence exceeds the scenario frame limit");
  options.maxHistorySamples = static_cast<size_t>(scenario.ticks + 1);
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
                    "ticksPerSecond", "maxTicksPerAdvance", "commands", "openDoorIds", "assetCacheDirectory",
                    "metersPerRendererUnit", "seed", "encoderNoiseRadians", "robotId", "drive", "capsule"});
  if (natural(value.at("version"), "version", 1) != 1)
    throw std::invalid_argument("Unsupported simulation scenario version");

  Scenario scenario;
  scenario.spawn.position = initialFeet(value.at("initialFeet"));
  readReal(value, "initialYaw", scenario.spawn.yaw);
  readTiming(value, scenario);
  if (value.contains("drive")) readDrive(value.at("drive"), scenario.options.drive);
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
    for (const auto& command : *commands) {
      checkKeys(command, {"tick", "linear", "angular"});
      const auto tick = natural(command.at("tick"), "command tick", maxTicks);
      if (tick >= scenario.ticks) throw std::invalid_argument("A command is scheduled after the last executable tick");
      scenario.commands.push_back({tick, {real(command.at("linear"), "linear"), real(command.at("angular"), "angular")}});
    }
  }
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

Json pose(const robotics::DrivePose& value) {
  return {{"position", Json::array({value.position.x, value.position.y, value.position.z})}, {"yaw", value.yaw}};
}

Json wheels(const robotics::DriveWheels& value) {
  return {{"left", value.left}, {"right", value.right}};
}

Json sample(const robotics::DifferentialDriveState& state) {
  return {{"tick", state.tick}, {"seconds", state.seconds}, {"base", pose(state.base)},
          {"odometry", pose(state.odometry)}, {"wheelAngles", wheels(state.wheelAngles)},
          {"wheelRates", wheels(state.wheelRates)}, {"encoderAngles", wheels(state.encoderAngles)},
          {"obstruction", obstructionName(state.obstruction)}, {"blockingEntityId", state.blockingEntityId}};
}

Json matrix(const glm::dmat4& value) {
  Json columns = Json::array();
  for (glm::length_t column = 0; column < 4; ++column)
    columns.push_back(Json::array({value[column].x, value[column].y, value[column].z, value[column].w}));
  return columns;
}

void play(robotics::DifferentialDriveSimulation& simulation, const Scenario& scenario) {
  for (const auto& command : scenario.commands) simulation.queueCommand(command.tick, command.twist);
  if (!scenario.frameNanos) {
    for (uint64_t tick = 0; tick < scenario.ticks; ++tick) simulation.step();
    return;
  }
  uint64_t elapsed = 0;
  while (elapsed < scenario.elapsedNanos) {
    const auto frame = std::min(*scenario.frameNanos, scenario.elapsedNanos - elapsed);
    simulation.advance(std::chrono::nanoseconds(static_cast<int64_t>(frame)));
    elapsed += frame;
  }
  while (simulation.clock().pendingTicks() != 0)
    if (simulation.advance(std::chrono::nanoseconds::zero()) == 0)
      throw std::logic_error("Simulation paused before completing the scenario");
  if (simulation.clock().tick() != scenario.ticks)
    throw std::logic_error("Simulation did not execute the scenario's expected ticks");
}

bool sameFile(const std::filesystem::path& first, const std::filesystem::path& second) {
  std::error_code error;
  return std::filesystem::equivalent(first, second, error);
}

template<class Char>
int run(int count, Char** args) {
  if (count == 2 && std::filesystem::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count != 5) {
    std::cerr << usage;
    return 2;
  }
  try {
    const std::filesystem::path input(args[1]), output(args[2]), scenarioPath(args[4]);
    if (output.extension() != ".json") throw std::invalid_argument("Output must be a .json trajectory report");
    if (sameFile(output, scenarioPath) || sameFile(output, input))
      throw std::invalid_argument("Output must be separate from the scene and scenario inputs");
    const auto sourceNamespace = container::util::pathToUtf8(std::filesystem::path(args[3]));
    const auto scenario = readScenario(scenarioPath);
    Json assetCache;
    auto collision = [&] {
      auto assets = robotics::loadBimPreprocessedAssets(input, scenario.cacheDirectory,
          {.scene = {.sourceNamespace = sourceNamespace, .metersPerRendererUnit = scenario.metersPerRendererUnit},
           .maxSlopeDegrees = scenario.options.capsule.maxSlopeDegrees});
      if (!assets.cacheDiagnostic.empty()) std::cerr << assets.cacheDiagnostic << '\n';
      if (!assets.scene.frame.metersPerRendererUnit)
        throw std::invalid_argument("Simulation requires known metric units. Supply metersPerRendererUnit for an unknown source scale");
      if (!scenario.cacheDirectory.empty())
        assetCache = {{"key", assets.cacheKey}, {"producerRevision", assets.producerRevision},
                      {"status", container::util::derivedAssetCacheStatusName(assets.cacheStatus)}};
      return std::move(assets.collision);
    }();
    robotics::BimCollisionWorld world(std::move(collision), scenario.openDoorIds, 1);
    robotics::DifferentialDriveSimulation simulation(world, scenario.spawn, scenario.options);
    play(simulation, scenario);

    Json history = Json::array();
    for (const auto& entry : simulation.history()) history.push_back(sample(entry));
    if (history.size() != scenario.ticks + 1)
      throw std::logic_error("Simulation history does not contain every requested tick");
    const auto transforms = simulation.frames();
    Json report{{"schema", "container.bim-robotics-simulation"}, {"version", 1},
                {"sourceNamespace", sourceNamespace}, {"robotId", scenario.options.robotId},
                {"coordinateConvention", "ROS REP-103"}, {"matrixLayout", "column-major"},
                {"seed", scenario.options.seed}, {"ticksPerSecond", scenario.options.ticksPerSecond},
                {"state", sample(simulation.state())}, {"history", std::move(history)},
                {"transforms", {{"mapFromOdometry", matrix(transforms.mapFromOdometry)},
                                {"odometryFromBase", matrix(transforms.odometryFromBase)},
                                {"baseFromLeftWheel", matrix(transforms.baseFromLeftWheel)},
                                {"baseFromRightWheel", matrix(transforms.baseFromRightWheel)}}}};
    if (!assetCache.is_null()) report["assetCache"] = std::move(assetCache);
    container::util::writeFileAtomically(output, report.dump(2) + '\n');
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
