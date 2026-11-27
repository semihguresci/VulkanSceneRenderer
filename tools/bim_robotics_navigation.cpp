#include "Container/robotics/DifferentialDriveSimulation.h"
#include "Container/robotics/NavigationTrainingLayout.h"
#include "Container/robotics/RaycastLidar.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef CONTAINER_NAVIGATION_REVISION
#define CONTAINER_NAVIGATION_REVISION "unknown"
#endif

namespace {

namespace robotics = container::robotics;
using Json = nlohmann::json;

constexpr uint32_t rayCount = 36;
constexpr double rangeMeters = 6.0;
constexpr double sensorHeightMeters = .4;
constexpr uint32_t ticksPerStep = 6;
constexpr uint32_t ticksPerSecond = 60;
constexpr double linearLimit = .8;
constexpr double angularLimit = 2.0;
constexpr size_t maxRequestBytes = 4096;
constexpr std::string_view robotId = "robot/navigation";
constexpr std::string_view producerRevision = CONTAINER_NAVIGATION_REVISION;
constexpr std::string_view usage = "Usage: BimRoboticsNavigation <navigation-scene.json>\n";

std::string_view obstructionName(robotics::BimWalkabilityFailure reason) {
  using enum robotics::BimWalkabilityFailure;
  switch (reason) {
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
  throw std::logic_error("Unknown navigation obstruction");
}

Json poseJson(const robotics::DrivePose& pose) {
  return Json::array({pose.position.x, pose.position.y, pose.position.z, pose.yaw});
}

robotics::DifferentialDriveSimulationOptions driveOptions(uint64_t seed, double encoderNoise) {
  robotics::DifferentialDriveSimulationOptions options;
  options.ticksPerSecond = ticksPerSecond;
  options.maxHistorySamples = 1;
  options.seed = seed;
  options.encoderNoiseRadians = encoderNoise;
  options.robotId = robotId;
  return options;
}

robotics::RaycastLidarSettings lidarSettings(uint64_t seed, double rangeNoise) {
  robotics::RaycastLidarSettings settings;
  settings.azimuthSamples = rayCount;
  settings.azimuthStart = 0;
  settings.minRangeMeters = 0;
  settings.maxRangeMeters = rangeMeters;
  settings.noiseStdDevMeters = rangeNoise;
  settings.seed = seed;
  settings.baseFromSensor.position.z = sensorHeightMeters;
  return settings;
}

class NavigationEpisode {
public:
  NavigationEpisode(robotics::NavigationTrainingLayout layout, uint64_t seed)
      : goal_(layout.goal), ambientLight_(layout.ambientLight), directionalLight_(layout.directionalLight),
        towardsLight_(glm::normalize(glm::dvec3(std::cos(layout.lightYaw), std::sin(layout.lightYaw), .5))),
        world_(std::move(layout.geometry), layout.openDoorIds, 1),
        drive_(world_, layout.spawn, driveOptions(seed, layout.encoderNoiseRadians)),
        lidar_(lidarSettings(seed, layout.rangeNoiseMeters)),
        cast_(world_.walkabilityQueries(robotId).castSegment) {}

  Json state() const {
    const auto& state = drive_.state();
    const robotics::LidarPose worldFromBase{
        state.base.position, glm::angleAxis(state.base.yaw, glm::dvec3(0, 0, 1))};
    std::array<double, rayCount> intensities{};
    size_t ray = 0;
    // Reuse each geometric hit for both the noisy range and a simple
    // ambient-plus-Lambert optical profile from the same sensor origin.
    const auto scan = lidar_.scan(worldFromBase, lidarSchedule_, state.tick,
        [this, &intensities, &ray](glm::dvec3 origin, glm::dvec3 displacement)
            -> std::optional<robotics::LidarHit> {
          auto& intensity = intensities.at(ray++);
          auto hit = cast_(origin, displacement);
          if (!hit) return std::nullopt;
          const double incidence = std::max(0.0, glm::dot(hit->normal, towardsLight_));
          intensity = std::clamp(ambientLight_ + directionalLight_ * incidence, 0.0, 1.0);
          return robotics::LidarHit{hit->fraction, std::move(hit->entityId)};
        });
    std::array<double, rayCount> ranges{};
    for (size_t bearing = 0; bearing < rayCount; ++bearing)
      ranges[bearing] = scan.returns.at(bearing).rangeMeters.value_or(rangeMeters);

    return {{"tick", state.tick}, {"pose", poseJson(state.base)}, {"odometry", poseJson(state.odometry)},
            {"goal", Json::array({goal_.x, goal_.y, goal_.z})}, {"ranges", ranges}, {"intensity", intensities},
            {"collision", obstruction_ == robotics::BimWalkabilityFailure::Blocked},
            {"obstruction", obstructionName(obstruction_)}, {"blockingEntityId", blockingEntityId_},
            {"travelledMeters", travelledMeters_}};
  }

  void step(robotics::DriveTwist command) {
    using enum robotics::BimWalkabilityFailure;
    drive_.queueCommand(drive_.state().tick, command);
    obstruction_ = None;
    blockingEntityId_.clear();
    auto previous = drive_.state().base.position;
    for (uint32_t tick = 0; tick < ticksPerStep; ++tick) {
      drive_.step();
      const auto& state = drive_.state();
      travelledMeters_ += glm::length(state.base.position - previous);
      previous = state.base.position;
      // Retain the first obstruction, with physical contacts taking priority.
      if (state.obstruction != None && (obstruction_ == None ||
          (obstruction_ != Blocked && state.obstruction == Blocked))) {
        obstruction_ = state.obstruction;
        blockingEntityId_ = state.blockingEntityId;
      }
    }
    drive_.clearCommands();
  }

private:
  glm::dvec3 goal_;
  double ambientLight_;
  double directionalLight_;
  glm::dvec3 towardsLight_;
  robotics::BimCollisionWorld world_;
  robotics::DifferentialDriveSimulation drive_;
  robotics::RaycastLidar lidar_;
  robotics::LidarSchedule lidarSchedule_{ticksPerSecond, 1000 * ticksPerSecond / ticksPerStep};
  decltype(robotics::BimWalkabilityQueries::castSegment) cast_;
  double travelledMeters_{};
  robotics::BimWalkabilityFailure obstruction_{robotics::BimWalkabilityFailure::None};
  std::string blockingEntityId_;
};

void checkFields(const Json& request, std::initializer_list<std::string_view> keys) {
  for (const auto& [key, _] : request.items()) {
    if (key != "op" && std::ranges::find(keys, std::string_view(key)) == keys.end())
      throw std::invalid_argument("Unexpected navigation request field: " + key);
  }
}

uint64_t readSeed(const Json& value) {
  if (value.is_number_unsigned()) return value.get<uint64_t>();
  if (value.is_number_integer() && value.get<int64_t>() >= 0) return value.get<uint64_t>();
  throw std::invalid_argument("Seed must be an unsigned 64-bit integer");
}

double readAction(const Json& value) {
  if (!value.is_number()) throw std::invalid_argument("Action components must be numbers in [-1, 1]");
  const double parsed = value.get<double>();
  if (!std::isfinite(parsed) || parsed < -1 || parsed > 1)
    throw std::invalid_argument("Action components must be numbers in [-1, 1]");
  return parsed;
}

Json handleRequest(const Json& request, const robotics::NavigationTrainingScene& scene,
                   std::unique_ptr<NavigationEpisode>& episode, bool& close) {
  if (!request.is_object() || !request.contains("op") || !request.at("op").is_string())
    throw std::invalid_argument("Navigation request must be an object with a string op");
  const auto& op = request.at("op").get_ref<const std::string&>();
  if (op == "describe") {
    checkFields(request, {});
    return {{"ok", true}, {"version", 1}, {"scene", scene.identity()}, {"rayCount", rayCount},
            {"rangeMeters", rangeMeters}, {"ticksPerStep", ticksPerStep}, {"ticksPerSecond", ticksPerSecond},
            {"linearLimit", linearLimit}, {"angularLimit", angularLimit}, {"producerRevision", producerRevision}};
  }
  if (op == "reset") {
    checkFields(request, {"seed", "randomize"});
    const uint64_t seed = readSeed(request.at("seed"));
    const bool randomize = request.value("randomize", true);
    auto layout = scene.sample(seed, randomize);
    auto manifest = std::move(layout.manifest);
    manifest["producerRevision"] = producerRevision;
    auto candidate = std::make_unique<NavigationEpisode>(std::move(layout), seed);
    Json reply{{"ok", true}, {"state", candidate->state()}, {"manifest", std::move(manifest)}};
    episode = std::move(candidate);
    return reply;
  }
  if (op == "step") {
    checkFields(request, {"action"});
    if (!episode) throw std::invalid_argument("Reset the navigation episode before stepping");
    const auto& action = request.at("action");
    if (!action.is_array() || action.size() != 2)
      throw std::invalid_argument("Action must contain exactly two components");
    const robotics::DriveTwist command{readAction(action[0]) * linearLimit, readAction(action[1]) * angularLimit};
    try {
      episode->step(command);
      return {{"ok", true}, {"state", episode->state()}};
    } catch (...) {
      // A world or sensor failure after a valid action may follow committed
      // ticks. Require a clean reset before accepting another action.
      episode.reset();
      throw;
    }
  }
  if (op == "close") {
    checkFields(request, {});
    close = true;
    return {{"ok", true}};
  }
  throw std::invalid_argument("Unknown navigation operation: " + op);
}

void writeReply(const Json& reply) {
  std::cout << reply.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n' << std::flush;
  if (!std::cout) throw std::runtime_error("Could not write the navigation response");
}

void serve(const robotics::NavigationTrainingScene& scene) {
  std::unique_ptr<NavigationEpisode> episode;
  std::array<char, maxRequestBytes + 1> input{};
  bool close = false;
  while (!close) {
    std::cin.getline(input.data(), static_cast<std::streamsize>(input.size()));
    if (std::cin.bad()) throw std::runtime_error("Could not read the navigation request");
    if (std::cin.fail()) {
      if (std::cin.eof()) break;
      std::cin.clear();
      std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      writeReply({{"ok", false}, {"error", "Navigation request exceeds 4096 bytes"}});
      continue;
    }
    const auto bytes = std::cin.gcount() - (std::cin.eof() ? 0 : 1);
    Json reply;
    try {
      const std::string_view line(input.data(), static_cast<size_t>(bytes));
      if (line.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Navigation request contains a NUL byte");
      reply = handleRequest(Json::parse(line.begin(), line.end()), scene, episode, close);
    } catch (const std::exception& error) {
      reply = {{"ok", false}, {"error", error.what()}};
    }
    writeReply(reply);
  }
}

template<class Char>
int run(int count, Char** args) {
  if (count == 2 && std::filesystem::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count != 2) {
    std::cerr << usage;
    return 2;
  }
  try {
    serve(robotics::NavigationTrainingScene(std::filesystem::path(args[1])));
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsNavigation: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
