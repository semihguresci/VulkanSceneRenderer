#include "DatasetSimulationReport.h"
#include "Container/robotics/SensorCamera.h"
#include "Container/utility/ContentFingerprint.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using container::robotics::DatasetReportStream;
using container::robotics::scanDatasetSimulationReport;
using enum DatasetReportStream;
using Json = nlohmann::ordered_json;
namespace fs = std::filesystem;

class TemporaryReport {
public:
  explicit TemporaryReport(std::string_view contents) {
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto parent = fs::temp_directory_path();
    do {
      directory_ = parent / ("container-dataset-report-" + std::to_string(now) + "-" +
                              std::to_string(sequence.fetch_add(1)));
    } while (!fs::create_directory(directory_));
    path_ = directory_ / "report.json";
    std::ofstream output(path_, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("Could not write the temporary simulation report");
  }

  ~TemporaryReport() {
    std::error_code error;
    fs::remove(path_, error);
    fs::remove(directory_, error);
  }

  const fs::path& path() const noexcept { return path_; }

private:
  fs::path directory_;
  fs::path path_;
};

constexpr int tickRate = 12;

Json drivePose(int tick) {
  return {{"position", {tick * .1, -.02, .3}}, {"yaw", tick * -.01}};
}

Json sensorPose() {
  return {{"xyz", {.2, 0., .1}}, {"quaternionXyzw", {0., 0., 0., 1.}}};
}

Json base(int tick) {
  return {{"tick", tick}, {"seconds", double(tick) / tickRate}, {"base", drivePose(tick)},
          {"odometry", drivePose(tick)}, {"obstruction", "none"}, {"blockingEntityId", ""}};
}

Json imu(int tick) {
  return {{"tick", tick}, {"seconds", double(tick) / tickRate}, {"specificForce", {0., 0., 9.81}},
          {"angularVelocity", {0., 0., -.01}}, {"encoderCounts", {{"left", -1}, {"right", 1}}},
          {"encoderRadians", {{"left", -.02}, {"right", .02}}}, {"odometry", drivePose(tick)}};
}

Json lidar(int tick) {
  return {{"tick", tick}, {"seconds", double(tick) / tickRate}, {"worldFromSensor", sensorPose()},
          {"returns", Json::array({{{"status", "valid"}, {"rangeMeters", 2.5}, {"entityId", "wall-1"}},
                                  {{"status", "no-hit"}, {"rangeMeters", nullptr}, {"entityId", nullptr}}})}};
}

Json report(bool sensors = true) {
  // Emit observations before their calibration, the common metadata, and
  // even base history to exercise independent stream order.
  auto result = Json::object();
  if (sensors) {
    result["lidar"] = {{"scans", Json::array({lidar(0), lidar(4)})},
        {"scansPerSecondMilliHz", 3000}, {"firstTick", 0}, {"azimuthSamples", 2},
        {"raysPerScan", 2}, {"elevationAngles", Json::array({0.0})},
        {"minRangeMeters", .1}, {"maxRangeMeters", 10.}, {"baseFromSensor", sensorPose()}};
    result["driveSensors"] = {{"samples", Json::array({imu(1), imu(4)})},
        {"samplesPerSecondMilliHz", 5000}, {"firstTick", 1}, {"imuFrame", "base"}};
  }
  result["history"] = Json::array({base(0), base(1), base(2), base(3), base(4)});
  result["ignored"] = {{"history", Json::array({42})}};
  result["schema"] = "container.bim-robotics-simulation";
  result["version"] = 1;
  result["coordinateConvention"] = "ROS REP-103";
  result["ticksPerSecond"] = tickRate;
  result["sourceNamespace"] = "native IFC example";
  result["robotId"] = "inspection-robot";
  return result;
}

void expectInvalid(std::string_view contents, std::string_view diagnostic) {
  const TemporaryReport input(contents);
  try {
    (void)scanDatasetSimulationReport(input.path(), {});
    ADD_FAILURE() << "Accepted invalid input, expected " << diagnostic;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string_view(error.what()).find(diagnostic), std::string_view::npos) << error.what();
  }
}

TEST(DatasetSimulationReport, StreamsCompleteObservationsInFileOrder) {
  auto source = report();
  source["sceneSource"] = {{"metersPerUnscaledRendererUnit", 1.0},
                          {"layers", Json::array({{{"sha256", std::string(64, 'a')}, {"byteSize", 123}}})}};
  source["history"][3]["newPayload"] = {{"caption", "Escaped \"quotation\", slash \\ and UTF-8 \xcf\x80"}};

  const TemporaryReport input(source.dump(2));
  std::vector<std::pair<DatasetReportStream, nlohmann::json>> retained;
  const auto read = scanDatasetSimulationReport(input.path(), [&](auto stream, const auto& row) {
    retained.emplace_back(stream, row);
  });
  EXPECT_EQ(read.sourceNamespace, "native IFC example");
  EXPECT_EQ(read.robotId, "inspection-robot");
  EXPECT_EQ(read.ticksPerSecond, tickRate);
  EXPECT_EQ(read.tickCount, 5u);
  EXPECT_EQ(read.imuSampleCount, 2u);
  EXPECT_EQ(read.lidarScanCount, 2u);
  EXPECT_EQ(read.sceneSource, nlohmann::json(source["sceneSource"]));

  const std::vector<std::pair<DatasetReportStream, uint64_t>> expected{
      {Lidar, 0}, {Lidar, 4}, {Imu, 1}, {Imu, 4}, {Base, 0}, {Base, 1}, {Base, 2}, {Base, 3}, {Base, 4}};
  ASSERT_EQ(retained.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(retained[i].first, expected[i].first);
    EXPECT_EQ(retained[i].second.at("tick"), expected[i].second);
  }
  EXPECT_EQ(retained.front().second, nlohmann::json(source["lidar"]["scans"][0]));
  EXPECT_EQ(retained[2].second, nlohmann::json(source["driveSensors"]["samples"][0]));
  EXPECT_EQ(retained[7].second, nlohmann::json(source["history"][3]));
  source["lidar"].erase("scans");
  source["driveSensors"].erase("samples");
  EXPECT_EQ(read.lidarCalibration, nlohmann::json(source["lidar"]));
  EXPECT_EQ(read.imuCalibration, nlohmann::json(source["driveSensors"]));
}

TEST(DatasetSimulationReport, AcceptsLegacyBaseOnlyAndAnAuthoredEmptySensorStream) {
  auto source = report(false);
  source["history"].erase(source["history"].begin() + 1, source["history"].end());
  const TemporaryReport legacy(source.dump());
  const auto read = scanDatasetSimulationReport(legacy.path(), {});
  EXPECT_EQ(read.tickCount, 1u);
  EXPECT_EQ(read.imuSampleCount, 0u);
  EXPECT_EQ(read.lidarScanCount, 0u);
  EXPECT_TRUE(read.sceneSource.is_null());
  EXPECT_TRUE(read.imuCalibration.is_null());
  EXPECT_TRUE(read.lidarCalibration.is_null());

  source["driveSensors"] = {{"samples", Json::array()}, {"samplesPerSecondMilliHz", 5000}, {"firstTick", 1}};
  const TemporaryReport emptyStream(source.dump());
  const auto empty = scanDatasetSimulationReport(emptyStream.path(), {});
  EXPECT_EQ(empty.imuSampleCount, 0u);
  ASSERT_TRUE(empty.imuCalibration.is_object());
  EXPECT_EQ(empty.imuCalibration.at("firstTick"), 1);
}

TEST(DatasetSimulationReport, RejectsMissingTicksAndWrongOptionalStreamSchedules) {
  auto source = report();
  source["history"].erase(source["history"].begin() + 1);
  expectInvalid(source.dump(), "history[1].tick");

  source = report();
  source["driveSensors"]["samples"][1]["tick"] = 3;
  expectInvalid(source.dump(), "outside the authored schedule");

  source = report();
  source["lidar"]["scans"].erase(1);
  expectInvalid(source.dump(), "observation count");

  source = report();
  source["lidar"]["scans"][1]["tick"] = 0;
  expectInvalid(source.dump(), "strictly increasing");

  source = report();
  source["driveSensors"].erase("samples");
  expectInvalid(source.dump(), "missing its observation array");
}

TEST(DatasetSimulationReport, DiagnosesInvalidNumericAndStructuralPayloads) {
  auto source = report();
  source["history"][0]["base"]["position"][1] = nullptr;
  expectInvalid(source.dump(), "history[0].base.position");

  source = report();
  source["driveSensors"]["samples"][1]["encoderCounts"]["left"] = 1.5;
  expectInvalid(source.dump(), "encoderCounts.left");

  source = report();
  source["lidar"]["scans"][0]["returns"][0]["rangeMeters"] = 20.;
  expectInvalid(source.dump(), "inside the authored calibration");

  source = report();
  source["lidar"]["scans"][0]["returns"][1]["status"] = "detected";
  expectInvalid(source.dump(), "returns[1].status");

  source = report();
  source["lidar"]["scans"][0]["returns"].erase(1);
  expectInvalid(source.dump(), "ray count changed");

  source = report();
  source["sceneSource"] = Json::array();
  expectInvalid(source.dump(), "sceneSource");
  expectInvalid(report().dump() + "{}", "malformed input");
  expectInvalid(R"({"discarded":1e999})", "malformed input");
  expectInvalid(R"({"history":[{"tick":0,"tick":0}]})", "duplicate object key");
}

TEST(DatasetSimulationReport, BoundsIndividualTokensNestingAndObservationAllocation) {
  expectInvalid("{\"ignored\":\"" + std::string(64 * 1024 + 1, 'A') + "\"}", "individual token");
  expectInvalid("{\"ignored\":" + std::string(40, '[') + "0" + std::string(40, ']') + "}", "nesting");
  std::string oversized = R"({"history":[{"extra":[0)";
  for (auto i = 0; i < 400'000; ++i) oversized += ",0";
  oversized += "]}]}";
  expectInvalid(oversized, "history: exceeds the JSON subtree limit");
}

TEST(DatasetSimulationReport, ValidatesOptInIdealRangesAgainstCalibrationAfterTheScans) {
  auto source = report();
  auto& calibration = source["lidar"];
  calibration["recordIdealRanges"] = true;
  calibration["noiseStdDevPerMeter"] = .015;
  calibration["rangeResolutionMeters"] = .05;
  for (auto& scan : calibration["scans"]) {
    scan["returns"][0]["idealRangeMeters"] = 2.47;
    scan["returns"][1]["idealRangeMeters"] = nullptr;
  }
  auto& returns = calibration["scans"][1]["returns"];
  returns[0]["status"] = "dropped";
  returns[0]["rangeMeters"] = nullptr;
  const TemporaryReport input(source.dump());
  const auto read = scanDatasetSimulationReport(input.path(), {});
  EXPECT_EQ(read.lidarCalibration.at("recordIdealRanges"), true);
  EXPECT_EQ(read.lidarCalibration.at("noiseStdDevPerMeter"), .015);
  EXPECT_EQ(read.lidarCalibration.at("rangeResolutionMeters"), .05);

  returns[0].erase("idealRangeMeters");
  expectInvalid(source.dump(), "idealRangeMeters presence");
  returns[0]["idealRangeMeters"] = .05;
  expectInvalid(source.dump(), "calibrated range limits");
  returns[0]["status"] = "below-minimum";
  returns[1]["idealRangeMeters"] = 2.47;
  expectInvalid(source.dump(), "must be null for a miss");
  returns[1]["idealRangeMeters"] = nullptr;
  calibration["recordIdealRanges"] = false;
  expectInvalid(source.dump(), "idealRangeMeters presence");
  calibration["recordIdealRanges"] = true;
  calibration["noiseStdDevPerMeter"] = -.1;
  expectInvalid(source.dump(), "lidar.noiseStdDevPerMeter");
  calibration["noiseStdDevPerMeter"] = .015;
  calibration["rangeResolutionMeters"] = -.1;
  expectInvalid(source.dump(), "lidar.rangeResolutionMeters");
}

TEST(DatasetSimulationReport, AcceptsMaximumLegalOrganizedScan) {
  auto source = report();
  source.erase("driveSensors");
  source["history"] = Json::array({base(0)});
  auto& calibration = source["lidar"];
  calibration["raysPerScan"] = 65'536;
  calibration["azimuthSamples"] = 65'536;
  calibration["recordIdealRanges"] = true;
  calibration["scans"].erase(1);
  auto& returns = calibration["scans"][0]["returns"];
  auto miss = returns.back();
  miss["idealRangeMeters"] = nullptr;
  returns = Json::array_t(65'536, miss);
  const TemporaryReport input(source.dump());
  uint64_t observedRays = 0;
  const auto read = scanDatasetSimulationReport(input.path(), [&](auto stream, const auto& row) {
    if (stream == Lidar) observedRays = row.at("returns").size();
  });
  EXPECT_EQ(read.lidarScanCount, 1u);
  EXPECT_EQ(observedRays, 65'536u);
}

TEST(DatasetSimulationReport, PreservesObserverExceptions) {
  struct ObserverStopped : std::runtime_error {
    using std::runtime_error::runtime_error;
  };
  const TemporaryReport input(report().dump());
  EXPECT_THROW((void)scanDatasetSimulationReport(input.path(), [](auto, const auto&) {
    throw ObserverStopped("spool write failed");
  }), ObserverStopped);
}

TEST(DatasetSimulationReport, CameraStreamsOnlyBaseFromDenseLegacyReportAndHashesRawBytes) {
  const TemporaryReport config(R"({
    "schema":"container.sensor-camera","version":1,
    "intrinsics":{"width":128,"height":96,"fx":60,"fy":62,"cx":63.5,"cy":47.5,
                  "nearMeters":0.1,"farMeters":80},
    "timing":{"firstTick":1}
  })");
  std::string source = "{\r\n\"history\":["
      R"({"tick":0,"seconds":0,"base":{"position":[0,0,0],"yaw":0},"odometry":false,"extra":[0)";
  // A retained observation would exceed the recorder's 400,000-node bound.
  for (auto i = 0; i < 410'000; ++i) source += ",0";
  source += R"(]},{"tick":1,"seconds":0.00005,"base":{"position":[1.25,2,3],"yaw":0}}],)";
  source += R"("lidar":{"scans":[{"returns":[{"status":"no-hit","rangeMeters":null})";
  for (auto i = 1; i < 65'536; ++i) source += R"(,{"status":"no-hit","rangeMeters":null})";
  source += R"(]}]},"ticksPerSecond":20000,"coordinateConvention":"ROS REP-103",)";
  source += "\"schema\":\"container.bim-robotics-simulation\",\"version\":1}\r\n\t ";
  const TemporaryReport reportFile(source);
  const auto playback = container::robotics::loadSensorCameraPlayback(config.path(), reportFile.path());
  ASSERT_EQ(playback.frames.size(), 1u);
  EXPECT_EQ(playback.frames.front().tick, 1u);
  EXPECT_DOUBLE_EQ(playback.frames.front().seconds, .00005);
  EXPECT_DOUBLE_EQ(playback.frames.front().worldFromCamera[3].x, 1.25);
  ASSERT_TRUE(playback.simulationReportSha256);
  EXPECT_EQ(*playback.simulationReportSha256,
            container::util::FingerprintFileContent(reportFile.path()).digest);

  // Trailing whitespace does not change the trajectory, but is covered by
  // its exact source fingerprint.
  const TemporaryReport changedWhitespace(source + "\n");
  const auto changed = container::robotics::loadSensorCameraPlayback(config.path(), changedWhitespace.path());
  ASSERT_TRUE(changed.simulationReportSha256);
  EXPECT_NE(*changed.simulationReportSha256, *playback.simulationReportSha256);
  EXPECT_EQ(changed.frames.front().worldFromCamera, playback.frames.front().worldFromCamera);
}

TEST(DatasetSimulationReport, FingerprintedInputRejectsPartiallyReadFiles) {
  const TemporaryReport input("these bytes belong to one fingerprint");
  EXPECT_THROW((void)container::util::FingerprintFileWhileReading(input.path(), [](std::istream& stream) {
    EXPECT_EQ(stream.get(), 't');
  }), std::invalid_argument);
}

} // namespace
