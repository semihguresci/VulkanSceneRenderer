#include "Container/robotics/RoboticsDataset.h"
#include "Container/utility/ContentFingerprint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace container::robotics;
using Json = nlohmann::json;
namespace fs = std::filesystem;

void write(const fs::path& path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) throw std::runtime_error("Cannot write robotics dataset test file");
}

Json fingerprint(const fs::path& path) {
  const auto value = container::util::FingerprintFileContent(path);
  return {{"sha256", value.digest}, {"byteSize", value.byteSize}};
}

void integer(std::string& bytes, uint64_t value, unsigned count = 4) {
  for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}

void terminated(std::string& bytes, std::string_view value) {
  bytes.append(value);
  bytes.push_back('\0');
}

void attribute(std::string& bytes, std::string_view name, std::string_view type, std::string_view payload) {
  terminated(bytes, name);
  terminated(bytes, type);
  integer(bytes, payload.size());
  bytes.append(payload);
}

// Independent, tiny OpenEXR scanline fixture. It does not call the production
// encoder or require TinyEXR. Each channel contains zero-valued pixels.
std::string image(uint32_t width = 2, uint32_t height = 1, uint32_t depthType = 2,
                  std::optional<uint32_t> groundTruthType = std::nullopt) {
  std::string bytes;
  integer(bytes, 20000630);
  integer(bytes, 2);
  std::string channels;
  const auto addChannel = [&](std::string_view name, uint32_t type) {
    terminated(channels, name);
    integer(channels, type);
    integer(channels, 0); // pLinear and three reserved bytes.
    integer(channels, 1);
    integer(channels, 1);
  };
  for (const auto& [name, type] : std::array<std::pair<std::string_view, uint32_t>, 6>{{
           {"B", 2}, {"G", 2}, {"InstanceID", 0}, {"R", 2}, {"SemanticID", 0}, {"Z", depthType}}}) {
    addChannel(name, type);
    if (name == "G" && groundTruthType) addChannel("GroundTruthZ", *groundTruthType);
  }
  channels.push_back('\0');
  attribute(bytes, "channels", "chlist", channels);
  attribute(bytes, "compression", "compression", std::string(1, '\0'));
  std::string window;
  integer(window, 0);
  integer(window, 0);
  integer(window, width - 1);
  integer(window, height - 1);
  attribute(bytes, "dataWindow", "box2i", window);
  attribute(bytes, "displayWindow", "box2i", window);
  attribute(bytes, "lineOrder", "lineOrder", std::string(1, '\0'));
  std::string one;
  integer(one, std::bit_cast<uint32_t>(1.0f));
  attribute(bytes, "pixelAspectRatio", "float", one);
  attribute(bytes, "screenWindowCenter", "v2f", std::string(8, '\0'));
  attribute(bytes, "screenWindowWidth", "float", one);
  bytes.push_back('\0');
  const auto first = bytes.size() + size_t(height) * 8;
  const auto rowBytes = size_t(width) * (groundTruthType ? 28 : 24);
  for (uint32_t row = 0; row < height; ++row)
    integer(bytes, first + uint64_t(row) * (8 + rowBytes), 8);
  for (uint32_t row = 0; row < height; ++row) {
    integer(bytes, row);
    integer(bytes, rowBytes);
    bytes.append(rowBytes, '\0');
  }
  return bytes;
}

class RoboticsDatasetReader : public testing::Test {
protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{};
    parent_ = fs::weakly_canonical(fs::temp_directory_path());
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      auto candidate = parent_ / ("container-robotics-reader-" + std::to_string(stamp) + "-" + std::to_string(++serial));
      if (fs::create_directory(candidate)) { root_ = std::move(candidate); break; }
    }
    ASSERT_FALSE(root_.empty());
    directory_ = root_ / "dataset";
    ASSERT_TRUE(fs::create_directory(directory_));
    const Json source{{"layers", Json::array({{{"sha256", std::string(64, 'a')}, {"byteSize", 123}}})},
                      {"metersPerUnscaledRendererUnit", 1.0}};
    const Json camera{{"schema", "container.sensor-camera-dataset"}, {"version", 1}, {"frameCount", 2},
                      {"labels", "labels.json"}, {"frameImagePattern", "frame-%08d.exr"},
                      {"frameMetadataPattern", "frame-%08d.json"}, {"colorSpace", "linear-sRGB"},
                      {"depthUnits", "metres"}, {"surfacePolicy", "opaque-and-alpha-mask"},
                      {"sceneSource", source}, {"simulationReportSha256", std::string(64, 'b')}};
    manifest_ = {{"schema", "container.robotics-dataset"}, {"version", 1}, {"sourceNamespace", "reader-fixture"},
                 {"robotId", "base"}, {"coordinateConvention", "ROS REP-103"}, {"ticksPerSecond", 60},
                 {"tickCount", 3}, {"ticksPerChunk", 2}, {"chunkCount", 2}, {"cameraFrameCount", 2},
                 {"imuSampleCount", 3}, {"lidarScanCount", 2}, {"labels", "labels.json"},
                 {"sceneSource", source}, {"simulationReportSha256", std::string(64, 'b')},
                 {"sensors", {{"camera", camera}, {"imu", {{"samplesPerSecondMilliHz", 60000}, {"firstTick", 0}}},
                              {"lidar", {{"scansPerSecondMilliHz", 30000}, {"firstTick", 0}, {"raysPerScan", 1},
                                         {"minRangeMeters", .05}, {"maxRangeMeters", 25.0}}}}},
                 {"chunkPattern", "chunk-%08d.jsonl"}, {"chunks", Json::array()}};
    labels_ = {{"schema", "container.bim-sensor-labels"}, {"version", 1}, {"sourceNamespace", "reader-fixture"},
               {"metersPerRendererUnit", 1.0}, {"backgroundId", 0},
               {"classes", Json::array({{{"semanticId", 1}, {"ifcClass", "UNCLASSIFIED"}},
                                         {{"semanticId", 2}, {"ifcClass", "IFCWALL"}}})},
               {"instances", Json::array({{{"instanceId", 1}, {"semanticId", 2}, {"entityId", "wall"},
                                           {"guid", "authored-guid"}, {"occurrenceId", "wall-0"},
                                           {"representationIds", Json::array({"Body"})}}})},
               {"diagnostics", Json::array()}};
    const Json intrinsics{{"width", 2}, {"height", 1}, {"fx", 1.0}, {"fy", 1.0}, {"cx", 1.0},
                          {"cy", .5}, {"nearMeters", .1}, {"farMeters", 10.0}};
    const Json matrix = Json::array({Json::array({1, 0, 0, 0}), Json::array({0, 1, 0, 0}),
                                     Json::array({0, 0, 1, 0}), Json::array({2, 3, 4, 1})});
    for (uint64_t tick = 0; tick < 3; ++tick) {
      const auto seconds = double(tick) / 60;
      Json base{{"tick", tick}, {"seconds", seconds},
                {"base", {{"position", Json::array({double(tick), 0.0, 1.2})}, {"yaw", .25}}},
                {"extraPreserved", {{"encoderRadians", Json::array({-1.25, 4.5})}, {"tag", "exact payload"}}}};
      Json row{{"tick", tick}, {"seconds", seconds}, {"base", base},
               {"imu", {{"tick", tick}, {"seconds", seconds}, {"specificForce", Json::array({0, 0, 9.81})},
                         {"angularVelocity", Json::array({0, 0, 0})}, {"encoderCounts", {{"left", -3}, {"right", 5}}},
                         {"encoderRadians", {{"left", -.1}, {"right", .2}}}, {"odometry", base.at("base")}}},
               {"lidar", tick % 2 == 0 ? Json{{"tick", tick}, {"seconds", seconds},
                                               {"worldFromSensor", {{"xyz", Json::array({0, 0, 1.2})},
                                                                     {"quaternionXyzw", Json::array({0, 0, 0, 1})}}},
                                               {"returns", Json::array({{{"rangeMeters", nullptr}, {"status", "no-hit"},
                                                                         {"entityId", nullptr}}})}} : Json(nullptr)},
               {"camera", nullptr}};
      if (tick % 2 == 0) {
        const auto index = tick / 2;
        const auto name = std::format("frame-{:08}.exr", index);
        write(directory_ / name, image());
        const Json frame{{"schema", "container.sensor-camera-frame"}, {"version", 1}, {"tick", tick},
                         {"seconds", seconds}, {"sampleIndex", index}, {"intrinsics", intrinsics},
                         {"worldFromCamera", matrix}};
        row["camera"] = fingerprint(directory_ / name);
        row["camera"]["frame"] = frame;
        row["camera"]["image"] = name;
      }
      rows_.push_back(std::move(row));
    }
    save();
  }

  void TearDown() override {
    if (root_.empty()) return;
    std::error_code error;
    const auto resolved = fs::weakly_canonical(root_, error);
    if (!error && resolved.parent_path() == parent_ && resolved.filename() == root_.filename() &&
        resolved.filename().string().starts_with("container-robotics-reader-"))
      fs::remove_all(resolved, error);
  }

  void saveManifest() { write(directory_ / "dataset.json", manifest_.dump(2) + '\n'); }
  void save() {
    write(directory_ / "labels.json", labels_.dump(2) + '\n');
    manifest_["labelFingerprint"] = fingerprint(directory_ / "labels.json");
    manifest_["chunks"] = Json::array();
    for (size_t chunk = 0; chunk < 2; ++chunk) {
      std::string bytes;
      for (size_t tick = chunk * 2; tick < std::min(rows_.size(), (chunk + 1) * 2); ++tick)
        bytes += rows_[tick].dump() + '\n';
      const auto path = directory_ / std::format("chunk-{:08}.jsonl", chunk);
      write(path, bytes);
      manifest_["chunks"].push_back(fingerprint(path));
    }
    saveManifest();
  }
  void replaceChunk(size_t index, std::string_view bytes) {
    const auto path = directory_ / std::format("chunk-{:08}.jsonl", index);
    write(path, bytes);
    manifest_["chunks"][index] = fingerprint(path);
    saveManifest();
  }
  std::pair<fs::path, fs::path> sourceCaptureAndReport() {
    const auto camera = root_ / "camera-source";
    fs::create_directory(camera);
    write(camera / "labels.json", labels_.dump(2) + '\n');
    Json history = Json::array();
    for (const auto& row : rows_) {
      history.push_back(row.at("base"));
      if (!row.at("camera").is_null()) {
        const auto& capture = row.at("camera");
        const auto name = capture.at("image").get<std::string>();
        fs::copy_file(directory_ / name, camera / name);
        auto metadata = camera / name;
        metadata.replace_extension(".json");
        write(metadata, capture.at("frame").dump(2) + '\n');
      }
    }
    const Json report{{"schema", "container.bim-robotics-simulation"}, {"version", 1},
                      {"coordinateConvention", "ROS REP-103"}, {"sourceNamespace", "reader-fixture"},
                      {"robotId", "base"}, {"ticksPerSecond", 60}, {"sceneSource", manifest_.at("sceneSource")},
                      {"history", history}};
    const auto reportPath = root_ / "simulation-report.json";
    write(reportPath, report.dump(2) + '\n');
    auto cameraManifest = manifest_.at("sensors").at("camera");
    cameraManifest["simulationReportSha256"] = fingerprint(reportPath).at("sha256");
    write(camera / "dataset.json", cameraManifest.dump(2) + '\n');
    return {camera, reportPath};
  }
  void expectInvalid(std::string_view message, const fs::path& directory = {}) {
    try {
      (void)readRoboticsDataset(directory.empty() ? directory_ : directory);
      ADD_FAILURE() << "Expected invalid dataset: " << message;
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string_view(error.what()).find(message), std::string_view::npos) << error.what();
    }
  }

  fs::path parent_, root_, directory_;
  Json manifest_, labels_;
  std::vector<Json> rows_;
};

TEST_F(RoboticsDatasetReader, StreamsExactPayloadAndPreservesIndependentManifests) {
  std::vector<Json> replay;
  const auto summary = readRoboticsDataset(directory_, [&](const Json& row) {
    EXPECT_EQ(row, rows_.at(replay.size()));
    if (!row.at("camera").is_null()) {
      const fs::path imageName = row.at("camera").at("image").get<std::string>();
      EXPECT_TRUE(imageName.is_relative());
      EXPECT_TRUE(fs::is_regular_file(directory_ / imageName));
    }
    replay.push_back(row);
  });
  EXPECT_EQ(replay, rows_);
  EXPECT_EQ(summary.manifest, manifest_);
  EXPECT_EQ(summary.labels, labels_);
  EXPECT_EQ(readRoboticsDataset(directory_).manifest, summary.manifest);
}

TEST_F(RoboticsDatasetReader, AcceptsEquivalentUnscaledMetricEvidenceWithoutRewritingIt) {
  manifest_["sensors"]["camera"]["sceneSource"]["metersPerUnscaledRendererUnit"] = 1.0000001;
  saveManifest();
  EXPECT_EQ(readRoboticsDataset(directory_).manifest, manifest_);
  manifest_["sensors"]["camera"]["sceneSource"]["layers"][0]["sha256"] = std::string(64, 'c');
  saveManifest();
  expectInvalid("provenance");
}

TEST_F(RoboticsDatasetReader, ReplaysRowsThatSpanSeveralInputBlocks) {
  rows_[0]["extensions"] = Json::array({std::string(48 * 1024, 'a'), std::string(48 * 1024, 'b'),
                                      std::string(48 * 1024, 'c')});
  save();

  size_t index = 0;
  const auto summary = readRoboticsDataset(directory_, [&](const Json& row) {
    EXPECT_EQ(row, rows_.at(index++));
  });
  EXPECT_EQ(index, rows_.size());
  EXPECT_EQ(summary.manifest, manifest_);
}

TEST_F(RoboticsDatasetReader, ReplaysMaximumSizeLidarScans) {
  constexpr size_t rays = 65'536;
  manifest_["sensors"]["lidar"]["raysPerScan"] = rays;
  const auto noHit = rows_[0]["lidar"]["returns"][0];
  for (auto& row : rows_) {
    if (!row["lidar"].is_null()) row["lidar"]["returns"] = Json::array_t(rays, noHit);
  }
  save();

  size_t scans = 0;
  (void)readRoboticsDataset(directory_, [&](const Json& row) {
    if (row["lidar"].is_null()) return;
    const auto& returns = row["lidar"]["returns"];
    ASSERT_EQ(returns.size(), rays);
    EXPECT_EQ(returns.front(), noHit);
    EXPECT_EQ(returns.back(), noHit);
    ++scans;
  });
  EXPECT_EQ(scans, 2u);
}

TEST_F(RoboticsDatasetReader, PreservesAndChecksIdealLidarRangesAlongsideMeasuredReturns) {
  manifest_["sensors"]["lidar"]["recordIdealRanges"] = true;
  auto& miss = rows_[0]["lidar"]["returns"][0];
  auto& hit = rows_[2]["lidar"]["returns"][0];
  miss["idealRangeMeters"] = nullptr;
  hit = {{"rangeMeters", nullptr}, {"status", "dropped"}, {"entityId", "wall"}, {"idealRangeMeters", 2.47}};
  save();

  size_t index = 0;
  (void)readRoboticsDataset(directory_, [&](const Json& row) { EXPECT_EQ(row, rows_.at(index++)); });
  EXPECT_EQ(index, rows_.size());

  miss["idealRangeMeters"] = 2.47;
  save();
  expectInvalid("must be null for a miss");
  miss["idealRangeMeters"] = nullptr;
  hit["idealRangeMeters"] = 25.01;
  save();
  expectInvalid("calibrated range limits");
  hit.erase("idealRangeMeters");
  save();
  expectInvalid("idealRangeMeters presence");
}

TEST_F(RoboticsDatasetReader, WriterRoundTripPreservesCameraLabelsAndEveryCommittedBasePose) {
  const auto [camera, report] = sourceCaptureAndReport();
  const auto output = root_ / "joined";
  writeRoboticsDataset(camera, report, output, {.ticksPerChunk = 1});
  std::vector<Json> replay;
  const auto summary = readRoboticsDataset(output, [&](const Json& row) { replay.push_back(row); });
  auto expected = rows_;
  for (auto& row : expected) { row["imu"] = nullptr; row["lidar"] = nullptr; }
  EXPECT_EQ(replay, expected);
  EXPECT_EQ(summary.labels, labels_);
  EXPECT_EQ(summary.manifest.at("chunkCount"), 3);
  EXPECT_EQ(summary.manifest.at("simulationReportSha256"), fingerprint(report).at("sha256"));
  std::ifstream cameraManifest(camera / "dataset.json");
  EXPECT_EQ(summary.manifest.at("sensors").at("camera"), Json::parse(cameraManifest));
  EXPECT_TRUE(fs::is_regular_file(output / "dataset.json"));
}

TEST_F(RoboticsDatasetReader, WriterRejectsChangedReportBeforePublishingCompletionMarker) {
  const auto [camera, report] = sourceCaptureAndReport();
  std::ofstream append(report, std::ios::binary | std::ios::app);
  append << ' ';
  append.close();
  const auto output = root_ / "rejected";
  EXPECT_THROW(writeRoboticsDataset(camera, report, output), std::runtime_error);
  EXPECT_FALSE(fs::exists(output / "dataset.json"));
}

TEST_F(RoboticsDatasetReader, CalibrationCanStartAfterTheRecordedHorizonWithNoSamples) {
  manifest_["sensors"]["imu"]["firstTick"] = std::numeric_limits<uint64_t>::max();
  manifest_["sensors"]["lidar"]["firstTick"] = std::numeric_limits<uint64_t>::max();
  manifest_["imuSampleCount"] = 0;
  manifest_["lidarScanCount"] = 0;
  for (auto& row : rows_) { row["imu"] = nullptr; row["lidar"] = nullptr; }
  save();
  EXPECT_EQ(readRoboticsDataset(directory_).manifest, manifest_);
}

TEST_F(RoboticsDatasetReader, RequiresCompletionMarkerAndEveryFixedChunk) {
  fs::remove(directory_ / "dataset.json");
  expectInvalid("dataset.json");
  saveManifest();
  fs::remove(directory_ / "chunk-00000001.jsonl");
  expectInvalid("chunk-00000001.jsonl");
}

TEST_F(RoboticsDatasetReader, RejectsChangedChunkBeforeDeliveringAnyRow) {
  write(directory_ / "chunk-00000001.jsonl", rows_[2].dump() + " \n");
  size_t visits = 0;
  EXPECT_THROW((void)readRoboticsDataset(directory_, [&](const Json&) { ++visits; }), std::invalid_argument);
  EXPECT_EQ(visits, 0);
}

TEST_F(RoboticsDatasetReader, RejectsLabelByteTamperingBeforeDeliveringAnyRow) {
  auto changed = labels_;
  changed["instances"][0]["guid"] = "changed-guid";
  write(directory_ / "labels.json", changed.dump(2) + '\n');
  size_t visits = 0;
  EXPECT_THROW((void)readRoboticsDataset(directory_, [&](const Json&) { ++visits; }), std::invalid_argument);
  EXPECT_EQ(visits, 0);
}

TEST_F(RoboticsDatasetReader, RejectsTruncatedBlankDuplicateKeyAndExtraRowsWithUpdatedDigests) {
  replaceChunk(1, rows_[2].dump());
  expectInvalid("final newline");
  replaceChunk(1, "\n");
  expectInvalid("missing tick row");
  replaceChunk(1, "{\"tick\":2,\"tick\":2}\n");
  expectInvalid("duplicate JSON key");
  replaceChunk(1, rows_[2].dump() + '\n' + rows_[2].dump() + '\n');
  expectInvalid("extra tick row");
}

TEST_F(RoboticsDatasetReader, RejectsTickTimestampSampleIndexAndScheduleMismatches) {
  rows_[1]["tick"] = 2;
  save();
  expectInvalid("sample tick");
  rows_[1]["tick"] = 1;
  rows_[1]["base"]["seconds"] = .5;
  save();
  expectInvalid("sample seconds");
  rows_[1]["base"]["seconds"] = 1.0 / 60;
  rows_[2]["camera"]["frame"]["sampleIndex"] = 7;
  save();
  expectInvalid("sample index");
  rows_[2]["camera"]["frame"]["sampleIndex"] = 1;
  rows_[1]["imu"] = nullptr;
  save();
  expectInvalid("IMU sample schedule");
}

TEST_F(RoboticsDatasetReader, RejectsImageTraversalChangedBytesAndInvalidExactChannelTypes) {
  rows_[0]["camera"]["image"] = "../frame-00000000.exr";
  save();
  expectInvalid("image name");
  rows_[0]["camera"]["image"] = "frame-00000000.exr";
  save();
  write(directory_ / "frame-00000000.exr", image(2, 1, 1));
  expectInvalid("sha256"); // Same size, different depth pixel type.
  rows_[0]["camera"].update(fingerprint(directory_ / "frame-00000000.exr"));
  save();
  expectInvalid("pixel type");
}

TEST_F(RoboticsDatasetReader, RejectsTruncatedExrEvenWithMatchingImageDigest) {
  auto bytes = image();
  bytes.pop_back();
  write(directory_ / "frame-00000000.exr", bytes);
  rows_[0]["camera"].update(fingerprint(directory_ / "frame-00000000.exr"));
  save();
  expectInvalid("payload size");
}

TEST_F(RoboticsDatasetReader, WriterAndReplayPreserveOptionalGroundTruthDepthChannel) {
  for (auto& row : rows_) {
    auto& camera = row["camera"];
    if (camera.is_null()) continue;
    const auto path = directory_ / camera.at("image").get<std::string>();
    write(path, image(2, 1, 2, 2));
    camera.update(fingerprint(path));
    camera["frame"]["channels"]["GroundTruthZ"] = "axial metres FLOAT";
  }
  save();
  EXPECT_NO_THROW((void)readRoboticsDataset(directory_));
  const auto [camera, report] = sourceCaptureAndReport();
  const auto output = root_ / "augmented";
  ASSERT_NO_THROW(writeRoboticsDataset(camera, report, output));
  size_t frames = 0;
  (void)readRoboticsDataset(output, [&](const Json& row) {
    if (row.at("camera").is_null()) return;
    EXPECT_EQ(row.at("camera"), rows_.at(row.at("tick").get<size_t>()).at("camera"));
    ++frames;
  });
  EXPECT_EQ(frames, 2u);
}

TEST_F(RoboticsDatasetReader, RejectsMismatchedGroundTruthDepthDeclarationsAndLayouts) {
  auto& first = rows_[0]["camera"];
  const auto path = directory_ / first.at("image").get<std::string>();
  first["frame"]["channels"]["GroundTruthZ"] = "axial metres FLOAT";
  save();
  expectInvalid("missing EXR dataset channel");
  write(path, image(2, 1, 2, 0));
  first.update(fingerprint(path));
  save();
  expectInvalid("pixel type");
  write(path, image(2, 1, 2, 2));
  first.update(fingerprint(path));
  save();
  expectInvalid("channel layout changed");
  first["frame"].erase("channels");
  save();
  expectInvalid("invalid EXR channel");
}

TEST_F(RoboticsDatasetReader, RejectsWrongDimensionsLabelsCountsAndStrayChunk) {
  rows_[0]["camera"]["frame"]["intrinsics"]["width"] = 3;
  save();
  expectInvalid("dimensions");
  rows_[0]["camera"]["frame"]["intrinsics"]["width"] = 2;
  labels_["instances"][0]["semanticId"] = 99;
  save();
  expectInvalid("unknown semantic");
  labels_["instances"][0]["semanticId"] = 2;
  manifest_["imuSampleCount"] = 2;
  save();
  expectInvalid("sample count");
  manifest_["imuSampleCount"] = 3;
  save();
  write(directory_ / "chunk-00000002.jsonl", "{}\n");
  expectInvalid("unexpected dataset file");
}

TEST_F(RoboticsDatasetReader, RejectsInvalidManifestLimitsDigestAndDuplicateLabels) {
  manifest_["tickCount"] = 8193;
  saveManifest();
  expectInvalid("tickCount");
  manifest_["tickCount"] = 3;
  manifest_["chunks"][0]["sha256"] = std::string(64, 'X');
  saveManifest();
  expectInvalid("lowercase hexadecimal");
  save();
  labels_["classes"].push_back(labels_["classes"][0]);
  save();
  expectInvalid("duplicate or zero semantic");
}

TEST_F(RoboticsDatasetReader, RejectsSymlinkedImageAndDatasetRootWhenSupported) {
  const auto original = directory_ / "frame-00000000.exr";
  const auto target = root_ / "actual.exr";
  fs::rename(original, target);
  std::error_code error;
  fs::create_symlink(target, original, error);
  if (error) GTEST_SKIP() << "File symlink creation unavailable: " << error.message();
  expectInvalid("symbolic-link file");
  fs::remove(original);
  fs::rename(target, original);
  const auto link = root_ / "linked";
  fs::create_directory_symlink(directory_, link, error);
  if (error) GTEST_SKIP() << "Directory symlink creation unavailable: " << error.message();
  expectInvalid("symbolic link", link);
}

TEST_F(RoboticsDatasetReader, ObserverExceptionPropagatesWithoutReadingLaterRows) {
  size_t visits = 0;
  EXPECT_THROW((void)readRoboticsDataset(directory_, [&](const Json&) {
    ++visits;
    throw std::runtime_error("observer stopped");
  }), std::runtime_error);
  EXPECT_EQ(visits, 1);
}

} // namespace
