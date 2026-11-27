#include "Container/robotics/BimAssetCache.h"
#include "Container/robotics/BimFleetMission.h"
#include "Container/robotics/BimSceneSourceManifest.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace robotics = container::robotics;
using Json = nlohmann::json;

constexpr std::string_view usage =
    "Usage: BimRoboticsFleet MODEL.ifc|MODEL.ifcx MISSION.json OUTPUT_DIRECTORY SOURCE_NAMESPACE\n";
constexpr std::string_view scenarioSchema = "container.bim-fleet-mission";
constexpr std::streamoff maxScenarioBytes = 1024 * 1024;
constexpr char reportFilename[] = "report.json";
constexpr char recordingFilename[] = "telemetry.json";

struct Scenario {
  robotics::BimFleetMissionOptions mission;
  std::optional<double> metersPerRendererUnit;
};

void checkKeys(const Json& object, std::string_view context, std::initializer_list<std::string_view> required,
               std::initializer_list<std::string_view> optional = {}) {
  if (!object.is_object()) throw std::invalid_argument(std::string(context) + " must be an object");
  for (const auto& entry : object.items()) {
    if (std::ranges::find(required, entry.key()) == required.end() &&
        std::ranges::find(optional, entry.key()) == optional.end())
      throw std::invalid_argument(std::string(context) + ": unknown field " + entry.key());
  }
  for (const auto key : required)
    if (!object.contains(key)) throw std::invalid_argument(std::string(context) + " requires " + std::string(key));
}

uint32_t natural(const Json& value, std::string_view field) {
  if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
      value.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
    throw std::invalid_argument(std::string(field) + " must be a nonnegative 32-bit integer");
  return value.get<uint32_t>();
}

double real(const Json& value, std::string_view field) {
  if (!value.is_number() || !std::isfinite(value.get<double>()))
    throw std::invalid_argument(std::string(field) + " must be a finite number");
  return value.get<double>();
}

void readReal(const Json& value, const char* field, double& destination) {
  if (const auto entry = value.find(field); entry != value.end()) destination = real(*entry, field);
}

glm::dvec3 point(const Json& value, std::string_view field) {
  if (!value.is_array() || value.size() != 3)
    throw std::invalid_argument(std::string(field) + " must contain three REP-103 coordinates in metres");
  return {real(value[0], field), real(value[1], field), real(value[2], field)};
}

robotics::BimFleetRobotMission readRobot(const Json& value) {
  checkKeys(value, "robot", {"robotId", "taskId", "initialFeet", "waypoints"},
            {"initialYaw", "maxLinearMetersPerSecond", "maxAngularRadiansPerSecond"});
  robotics::BimFleetRobotMission robot;
  robot.robotId = value.at("robotId").get<std::string>();
  robot.taskId = value.at("taskId").get<std::string>();
  robot.spawn.position = point(value.at("initialFeet"), "initialFeet");
  readReal(value, "initialYaw", robot.spawn.yaw);
  readReal(value, "maxLinearMetersPerSecond", robot.maxLinearMetersPerSecond);
  readReal(value, "maxAngularRadiansPerSecond", robot.maxAngularRadiansPerSecond);
  const auto& waypoints = value.at("waypoints");
  if (!waypoints.is_array() || waypoints.empty())
    throw std::invalid_argument("waypoints must be a nonempty array");
  robot.waypoints.reserve(waypoints.size());
  for (const auto& waypoint : waypoints) robot.waypoints.push_back(point(waypoint, "waypoint"));
  return robot;
}

Scenario readScenario(const fs::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("Could not open the fleet mission scenario");
  const auto size = input.tellg();
  if (size <= 0 || size > maxScenarioBytes)
    throw std::invalid_argument("Fleet mission scenario must contain at most 1 MiB of JSON");
  input.seekg(0);
  std::string contents(static_cast<size_t>(size), '\0');
  if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())) ||
      input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Could not read a complete fleet mission scenario");
  if (contents.find('\0') != std::string::npos)
    throw std::invalid_argument("Fleet mission scenario contains a raw NUL byte");

  std::vector<std::set<std::string>> objectKeys;
  const auto value = Json::parse(contents, [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth > 8) throw std::invalid_argument("Fleet mission scenario JSON nesting exceeds its limit");
    using enum Json::parse_event_t;
    if (event == object_start) objectKeys.emplace_back();
    if (event == object_end) objectKeys.pop_back();
    if (event == key && !objectKeys.back().insert(parsed.get_ref<const std::string&>()).second)
      throw std::invalid_argument("Fleet mission scenario contains a duplicate JSON field");
    return true;
  });
  checkKeys(value, "mission", {"schema", "version", "robots"},
            {"ticksPerSecond", "maxTicks", "waypointToleranceMeters", "openDoorIds", "metersPerRendererUnit"});
  const auto& schema = value.at("schema");
  if (!schema.is_string() || schema.get_ref<const std::string&>() != scenarioSchema ||
      natural(value.at("version"), "version") != 1)
    throw std::invalid_argument("Unsupported fleet mission schema or version");
  Scenario scenario;
  auto& mission = scenario.mission;
  if (value.contains("ticksPerSecond")) mission.ticksPerSecond = natural(value.at("ticksPerSecond"), "ticksPerSecond");
  if (value.contains("maxTicks")) mission.maxTicks = natural(value.at("maxTicks"), "maxTicks");
  readReal(value, "waypointToleranceMeters", mission.waypointToleranceMeters);
  if (value.contains("openDoorIds")) mission.openDoorIds = value.at("openDoorIds").get<std::vector<std::string>>();
  if (value.contains("metersPerRendererUnit")) {
    scenario.metersPerRendererUnit = real(value.at("metersPerRendererUnit"), "metersPerRendererUnit");
    if (*scenario.metersPerRendererUnit <= 0)
      throw std::invalid_argument("metersPerRendererUnit must be positive");
  }
  const auto& robots = value.at("robots");
  if (!robots.is_array() || robots.empty()) throw std::invalid_argument("robots must be a nonempty array");
  mission.robots.reserve(robots.size());
  for (const auto& robot : robots) mission.robots.push_back(readRobot(robot));
  return scenario;
}

std::string_view failureName(robotics::BimWalkabilityFailure failure) {
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
  throw std::logic_error("Unknown fleet walkability failure");
}

Json xyz(const glm::dvec3& position) {
  return Json::array({position.x, position.y, position.z});
}

Json reportJson(const robotics::BimFleetMissionResult& result, const robotics::BimFleetMissionOptions& options) {
  Json robots = Json::array();
  for (const auto& robot : result.robots) {
    Json arrivals = Json::array();
    for (const auto& arrival : robot.arrivals)
      arrivals.push_back({{"waypointIndex", arrival.waypointIndex}, {"tick", arrival.tick},
                          {"position", xyz(arrival.position)}});
    robots.push_back({{"robotId", robot.robotId}, {"taskId", robot.taskId},
                      {"finalPose", {{"position", xyz(robot.finalPose.position)}, {"yaw", robot.finalPose.yaw}}},
                      {"completionTick", robot.completionTick ? Json(*robot.completionTick) : Json(nullptr)},
                      {"arrivals", std::move(arrivals)}, {"blockedTicks", robot.blockedTicks},
                      {"lastObstruction", failureName(robot.lastObstruction)}, {"blockingEntityId", robot.blockingEntityId}});
  }
  return {{"schema", "container.bim-fleet-report"}, {"version", 1}, {"coordinateConvention", "ROS REP-103"},
          {"sourceNamespace", result.recording.sourceNamespace}, {"sceneSource", robotics::bimSceneSourceJson(result.recording.sceneSource)},
          {"clockId", result.recording.clockId}, {"ticksPerSecond", options.ticksPerSecond}, {"ticks", result.ticks},
          {"waypointToleranceMeters", options.waypointToleranceMeters}, {"completed", result.completed}, {"robots", std::move(robots)}};
}

class StagedOutput {
public:
  explicit StagedOutput(const fs::path& parent) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      const auto candidate = parent / (".bim-fleet-staging-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(candidate, error)) {
        path = candidate;
        return;
      }
      if (error && error != std::errc::file_exists)
        throw fs::filesystem_error("Could not create the fleet output staging directory", candidate, error);
    }
    throw std::runtime_error("Could not create a unique fleet output staging directory");
  }

  ~StagedOutput() {
    if (path.empty()) return;
    std::error_code ignored;
    fs::remove(path / reportFilename, ignored);
    fs::remove(path / recordingFilename, ignored);
    fs::remove(path, ignored);
  }

  StagedOutput(const StagedOutput&) = delete;
  StagedOutput& operator=(const StagedOutput&) = delete;

  void publish(const fs::path& destination) {
    std::error_code error;
#ifdef _WIN32
    if (!MoveFileExW(path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
      error = {static_cast<int>(GetLastError()), std::system_category()};
#elif defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, path.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) != 0)
      error = {errno, std::generic_category()};
#elif defined(__APPLE__)
    if (::renamex_np(path.c_str(), destination.c_str(), RENAME_EXCL) != 0)
      error = {errno, std::generic_category()};
#else
    error = std::make_error_code(std::errc::operation_not_supported);
#endif
    if (error)
      throw fs::filesystem_error("Could not atomically publish the fresh fleet output directory", destination, error);
    path.clear();
  }

  fs::path path;
};

template<class Char>
int run(int count, Char** args) {
  if (count == 2 && fs::path(args[1]) == "--help") {
    std::cout << usage;
    return 0;
  }
  if (count != 5) {
    std::cerr << usage;
    return 1;
  }
  try {
    const fs::path model(args[1]), missionFile(args[2]);
    auto destination = fs::absolute(fs::path(args[3]));
    if (!destination.has_filename() && destination.has_relative_path()) destination = destination.parent_path();
    if (fs::exists(fs::symlink_status(destination)))
      throw std::invalid_argument("Output must be a fresh directory");
    if (!fs::is_directory(destination.parent_path()))
      throw std::invalid_argument("The parent of the output directory must already exist");
    const auto sourceNamespace = container::util::pathToUtf8(fs::path(args[4]));
    const auto scenario = readScenario(missionFile);
    const auto result = [&] {
      auto assets = robotics::loadBimPreprocessedAssets(model, {},
          {.scene = {.sourceNamespace = sourceNamespace, .metersPerRendererUnit = scenario.metersPerRendererUnit}});
      if (!assets.scene.frame.metersPerRendererUnit)
        throw std::invalid_argument("Fleet missions require known metric units. Supply metersPerRendererUnit if needed");
      std::vector<container::util::ContentFingerprint> sourceLayers;
      sourceLayers.reserve(assets.sourceLayers.size());
      for (auto& layer : assets.sourceLayers) sourceLayers.push_back(std::move(layer.fingerprint));
      auto source = robotics::makeBimSceneSource(std::move(sourceLayers), assets.scene.frame);
      return robotics::runBimFleetMission(std::move(assets.collision), sourceNamespace, std::move(source), scenario.mission);
    }();
    const auto report = reportJson(result, scenario.mission).dump(2) + '\n';
    const auto recording = robotics::serializeTwinRecording(result.recording);
    StagedOutput output(destination.parent_path());
    using container::util::AtomicFilePublication;
    container::util::writeFileAtomically(output.path / reportFilename, report, AtomicFilePublication::CreateNew);
    container::util::writeFileAtomically(output.path / recordingFilename, recording, AtomicFilePublication::CreateNew);
    output.publish(destination);
    std::cout << (result.completed ? "Fleet mission completed" : "Fleet mission timed out")
              << " after " << result.ticks << " ticks\n";
    return result.completed ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsFleet: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
