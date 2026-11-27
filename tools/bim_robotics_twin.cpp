#include "Container/robotics/BimSceneSourceManifest.h"
#include "Container/robotics/TwinTelemetry.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"
#include "DatasetSimulationReport.h"
#include "TwinRecordingJson.h"
#include "bim_robotics_twin_ingest.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace robotics = container::robotics;
namespace util = container::util;
using Json = nlohmann::json;

constexpr std::string_view usage =
    "Usage: BimRoboticsTwin record SIMULATION.json NEW_LOG.json [--supplement EVENTS.json]\n"
    "       BimRoboticsTwin import INPUT.json NEW_LOG.json\n"
    "       BimRoboticsTwin sample LOG.json TIMESTAMP_NS\n"
    "       BimRoboticsTwin verify LOG.json\n"
    "       BimRoboticsTwin ingest MODEL.ifc|MODEL.ifcx EVENTS.jsonl NEW_LOG.json SOURCE_NAMESPACE CLOCK_ID\n"
    "                              [--stale-after-ns N] [--metres-per-renderer-unit N]\n";
constexpr std::string_view simulationClock = "simulation-fixed-step";
constexpr uint64_t nanosecondsPerSecond = 1'000'000'000;

struct SimulationObservation {
  uint64_t tick{};
  robotics::DatasetReportStream stream;
  robotics::TwinPayload payload;
};

robotics::TwinRecording recordSimulation(const fs::path& simulation) {
  using enum robotics::DatasetReportStream;
  std::vector<SimulationObservation> observed;
  const auto report = robotics::scanDatasetSimulationReport(simulation, [&](auto stream, const Json& row) {
    const auto tick = row.at("tick").get<uint64_t>();
    if (stream == Base) {
      const auto& base = row.at("base");
      const auto& p = base.at("position");
      robotics::TwinRobotPose pose{{p[0].get<double>(), p[1].get<double>(), p[2].get<double>()},
                                 base.at("yaw").get<double>()};
      if (const auto contact = row.find("contact"); contact != row.end())
        pose.contact = robotics::twin_json::parseRobotContact(
            *contact, "history[" + std::to_string(tick) + "].contact");
      observed.push_back({tick, stream, std::move(pose)});
    } else {
      observed.push_back({tick, stream, robotics::TwinSensorStatus{"observed"}});
    }
  });
  robotics::TwinRecording recording{
      .sourceNamespace = report.sourceNamespace,
      .sceneSource = robotics::twin_json::parseSceneSource(report.sceneSource),
      .clockId = std::string(simulationClock)};
  recording.events.reserve(observed.size());
  for (auto& observation : observed) {
    auto entityId = report.robotId;
    std::string source;
    switch (observation.stream) {
    case Base: source = "simulation:base"; break;
    case Imu: source = "simulation:imu"; entityId += "/imu"; break;
    case Lidar: source = "simulation:lidar"; entityId += "/lidar"; break;
    }
    // A native report uses validated, bounded committed ticks. Integer
    // division supplies one reproducible nanosecond timestamp per tick.
    recording.events.push_back({observation.tick * nanosecondsPerSecond / report.ticksPerSecond,
                                std::move(entityId), std::move(source), std::move(observation.payload)});
  }
  return recording;
}

void appendSupplement(robotics::TwinRecording& recording, robotics::TwinRecording supplement) {
  if (recording.sourceNamespace != supplement.sourceNamespace ||
      recording.sceneSource.layers != supplement.sceneSource.layers ||
      recording.sceneSource.metersPerUnscaledRendererUnit != supplement.sceneSource.metersPerUnscaledRendererUnit)
    throw std::invalid_argument("Supplement sourceNamespace or ordered sceneSource identity does not match the simulation");
  if (recording.clockId != supplement.clockId ||
      recording.staleAfterNanoseconds != supplement.staleAfterNanoseconds)
    throw std::invalid_argument("Supplement clockId or staleAfterNanoseconds does not match the simulation");
  for (auto& event : supplement.events) {
    if (!std::holds_alternative<robotics::TwinPlannedPath>(event.payload) &&
        !std::holds_alternative<robotics::TwinAssetState>(event.payload) &&
        !std::holds_alternative<robotics::TwinOperatorAlarm>(event.payload) &&
        !std::holds_alternative<robotics::TwinSensorFrustum>(event.payload) &&
        !std::holds_alternative<robotics::TwinOccupancyGrid>(event.payload))
      throw std::invalid_argument(
          "Supplement accepts only planned-path, bim-asset-state, operator-alarm, sensor-frustum, and occupancy-grid events");
    recording.events.push_back(std::move(event));
  }
}

void publish(const fs::path& output, const robotics::TwinRecording& recording) {
  const auto bytes = robotics::serializeTwinRecording(recording);
  if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
  util::writeFileAtomically(output, bytes, util::AtomicFilePublication::CreateNew);
  std::cout << "Recorded BIM twin telemetry at " << util::pathToUtf8(output) << '\n';
}

void requireNewLog(const fs::path& path) {
  if (!path.has_filename() || fs::exists(fs::symlink_status(path)))
    throw std::invalid_argument("Output must be a new BIM twin recording file: " + util::pathToUtf8(path));
}

uint64_t timestamp(const fs::path& argument) {
  const auto text = util::pathToUtf8(argument);
  uint64_t result{};
  const auto* end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::invalid_argument("TIMESTAMP_NS must be a nonnegative 64-bit integer");
  return result;
}

Json identity(const robotics::TwinRecording& recording) {
  return {{"version", 1}, {"sourceNamespace", recording.sourceNamespace},
          {"sceneSource", robotics::bimSceneSourceJson(recording.sceneSource)}, {"clockId", recording.clockId},
          {"staleAfterNanoseconds", recording.staleAfterNanoseconds}};
}

Json verify(const robotics::TwinReplay& replay) {
  auto result = identity(replay.recording());
  result["schema"] = "container.bim-twin-verification";
  result["eventCount"] = replay.recording().events.size();
  const auto version = robotics::twin_json::recordingVersion(replay.recording());
  if (version > 1) result["recordingVersion"] = version;
  result["firstTimestampNanoseconds"] = replay.firstTimestamp();
  result["lastTimestampNanoseconds"] = replay.lastTimestamp();
  return result;
}

Json sample(const robotics::TwinReplay& replay, uint64_t when) {
  const auto sampled = replay.sample(when);
  auto result = identity(replay.recording());
  result["schema"] = "container.bim-twin-sample";
  result["version"] = robotics::twin_json::recordingVersion(replay.recording());
  result["timestampNanoseconds"] = sampled.timestampNanoseconds;
  auto tracks = Json::array();
  for (const auto& track : sampled.tracks) {
    auto observedPath = Json::array();
    for (const auto& point : track.observedPath) observedPath.push_back(robotics::twin_json::point(point));
    tracks.push_back({{"event", robotics::twin_json::event(track.event)}, {"stale", track.stale},
                      {"observedPath", std::move(observedPath)},
                      {"distanceToPlanMeters", track.distanceToPlanMeters
                          ? Json(*track.distanceToPlanMeters) : Json(nullptr)}});
  }
  result["tracks"] = std::move(tracks);
  return result;
}

template <class Char>
int run(int count, Char** args) {
  const auto operation = count > 1 ? fs::path(args[1]) : fs::path{};
  if (count == 2 && operation == "--help") {
    std::cout << usage;
    return 0;
  }
  try {
    if (operation == "record" && (count == 4 || (count == 6 && fs::path(args[4]) == "--supplement"))) {
      const fs::path output(args[3]);
      requireNewLog(output);
      auto recording = recordSimulation(fs::path(args[2]));
      if (count == 6) appendSupplement(recording, robotics::loadTwinRecording(fs::path(args[5])));
      publish(output, recording);
      return 0;
    }
    if (count == 4 && operation == "import") {
      const fs::path output(args[3]);
      requireNewLog(output);
      publish(output, robotics::loadTwinRecording(fs::path(args[2])));
      return 0;
    }
    if (count == 4 && operation == "sample") {
      const auto when = timestamp(fs::path(args[3]));
      const robotics::TwinReplay replay(robotics::loadTwinRecording(fs::path(args[2])));
      std::cout << sample(replay, when).dump(2) << '\n';
      return 0;
    }
    if (count == 3 && operation == "verify") {
      const robotics::TwinReplay replay(robotics::loadTwinRecording(fs::path(args[2])));
      std::cout << verify(replay).dump(2) << '\n';
      return 0;
    }
    if (count >= 7 && operation == "ingest") {
      std::vector<fs::path> arguments(args + 2, args + count);
      return container::tools::runTwinIngest(arguments);
    }
  } catch (const std::exception& error) {
    std::cerr << "BimRoboticsTwin: " << error.what() << '\n';
    return 1;
  }
  std::cerr << usage;
  return 2;
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** args) { return run(count, args); }
#else
int main(int count, char** args) { return run(count, args); }
#endif
