#include "Container/robotics/RoboticsDataset.h"

#include "Container/utility/AtomicFile.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/Platform.h"
#include "DatasetSimulationReport.h"
#include "RoboticsDatasetValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <istream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace container::robotics {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr uint64_t maxMetadataBytes = 1024 * 1024;
constexpr uint64_t maxLabelsBytes = 64 * 1024 * 1024;
constexpr uint64_t maxImageBytes = 256 * 1024 * 1024;
constexpr size_t maxSpoolRowBytes = 32 * 1024 * 1024;
constexpr std::array spoolNames{"base.jsonl", "imu.jsonl", "lidar.jsonl"};

[[noreturn]] void invalid(std::string_view context, std::string_view reason) {
  throw std::runtime_error(std::string(context) + ": " + std::string(reason));
}

uint64_t requiredFileSize(const fs::path& path, uint64_t limit) {
  if (!fs::is_regular_file(fs::symlink_status(path)))
    invalid(util::pathToUtf8(path), "expected a regular file");
  const auto bytes = fs::file_size(path);
  if (bytes == 0 || bytes > limit)
    invalid(util::pathToUtf8(path), "empty file or byte limit exceeded");
  return bytes;
}

void copyInput(const fs::path& source, const fs::path& destination, uint64_t limit) {
  (void)requiredFileSize(source, limit);
  fs::copy_file(source, destination);
  (void)requiredFileSize(destination, limit);
}

Json readMetadata(const fs::path& path) {
  const auto size = requiredFileSize(path, maxMetadataBytes);
  std::ifstream input(path, std::ios::binary);
  std::string contents(static_cast<size_t>(size), '\0');
  if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())) ||
      input.peek() != std::char_traits<char>::eof() || input.bad())
    invalid(util::pathToUtf8(path), "metadata changed or could not be read");
  try {
    return Json::parse(contents);
  } catch (const Json::exception& error) {
    invalid(util::pathToUtf8(path), error.what());
  }
}

uint64_t naturalField(const Json& value, const char* name, std::string_view context) {
  const auto field = value.find(name);
  if (field == value.end() || (!field->is_number_unsigned() &&
      (!field->is_number_integer() || field->get<int64_t>() < 0)))
    invalid(context, std::string(name) + " must be a nonnegative integer");
  return field->get<uint64_t>();
}

void checkTime(const Json& row, uint64_t tick, uint32_t rate, std::string_view context) {
  const auto seconds = row.find("seconds");
  const double expected = static_cast<double>(tick) / rate;
  if (seconds == row.end() || !seconds->is_number())
    invalid(context, "sample seconds must be numeric");
  const double observed = seconds->get<double>();
  if (!std::isfinite(observed) || std::abs(observed - expected) > std::max(1e-9, expected * 1e-12))
    invalid(context, std::format("sample seconds do not match fixed tick {} at {} Hz", tick, rate));
}

void compareSceneSource(const Json& camera, const Json& simulation) {
  const auto sceneContext = "camera and simulation sceneSource";
  if (!camera.is_object() || !simulation.is_object())
    invalid(sceneContext, "requires model provenance from both producers");
  const auto orderedLayers = camera.find("layers");
  if (orderedLayers == camera.end() || !orderedLayers->is_array() || orderedLayers->empty() ||
      !simulation.contains("layers") || *orderedLayers != simulation["layers"])
    invalid(sceneContext, "BIM source content differs");
  const auto cameraScale = camera.find("metersPerUnscaledRendererUnit");
  const auto simulationScale = simulation.find("metersPerUnscaledRendererUnit");
  if (cameraScale == camera.end() || simulationScale == simulation.end() ||
      !cameraScale->is_number() || !simulationScale->is_number())
    invalid(sceneContext, "requires physical unit scale from both producers");
  const double a = cameraScale->get<double>();
  const double b = simulationScale->get<double>();
  // The IFC import scale is stored as binary32 before the inverse mapping.
  // Compare physical lengths independently of renderer import scaling.
  if (!std::isfinite(a) || !std::isfinite(b) || a <= 0.0 || b <= 0.0 ||
      std::abs(a - b) > std::max(a, b) * 1e-6)
    invalid(sceneContext, "physical BIM unit scales differ");
}

class RecordingScratch {
 public:
  explicit RecordingScratch(const fs::path& directory) : path(directory / ".recording") {
    if (!fs::create_directory(path))
      invalid(util::pathToUtf8(path), "could not reserve the recording scratch directory");
  }

  ~RecordingScratch() {
    std::error_code ignored;
    for (const auto* name : spoolNames) fs::remove(path / name, ignored);
    fs::remove(path, ignored);
  }

  void finish() const {
    for (const auto* name : spoolNames) fs::remove(path / name);
    if (!fs::remove(path)) invalid(util::pathToUtf8(path), "could not retire scratch directory");
  }

  const fs::path path;
};

DatasetSimulationReport spoolReport(std::istream& input, const fs::path& scratch) {
  std::array<std::ofstream, spoolNames.size()> outputs;
  for (size_t index = 0; index < outputs.size(); ++index) {
    outputs[index].open(scratch / spoolNames[index], std::ios::binary);
    if (!outputs[index]) invalid(spoolNames[index], "could not open simulation spool");
  }

  const auto report = scanDatasetSimulationReport(input, [&](DatasetReportStream stream, const Json& row) {
    const auto index = static_cast<size_t>(stream);
    auto& output = outputs.at(index);
    const auto encoded = row.dump();
    if (encoded.size() > maxSpoolRowBytes)
      invalid(spoolNames[index], "single simulation observation exceeds the byte limit");
    if (!(output << encoded << '\n'))
      invalid(spoolNames[index], "could not spool simulation observation");
  });
  for (size_t index = 0; index < outputs.size(); ++index) {
    outputs[index].close();
    if (!outputs[index]) invalid(spoolNames[index], "could not finish simulation spool");
  }
  return report;
}

class SimulationCursor {
 public:
  explicit SimulationCursor(const fs::path& path) : name_(util::pathToUtf8(path.filename())), input_(path, std::ios::binary) {
    if (!input_) invalid(name_, "could not open simulation spool");
    advance();
  }

  Json take(uint64_t tick, uint32_t rate) {
    if (next_.is_null() || nextTick_ > tick) return nullptr;
    if (nextTick_ < tick) invalid(name_, "spooled tick preceded the current dataset tick");
    checkTime(next_, tick, rate, name_);
    auto result = std::move(next_);
    advance();
    return result;
  }

  bool exhausted() const { return next_.is_null(); }

 private:
  void advance() {
    // The spool is owned by this recording and each line was bounded before writing.
    if (std::getline(input_, line_)) {
      next_ = Json::parse(line_);
      nextTick_ = naturalField(next_, "tick", name_);
    } else if (input_.bad() || !input_.eof()) {
      invalid(name_, "could not read simulation spool");
    } else {
      next_ = nullptr;
    }
  }

  std::string name_;
  std::ifstream input_;
  std::string line_;
  Json next_;
  uint64_t nextTick_{};
};

class CameraCursor {
 public:
  CameraCursor(const fs::path& source, const fs::path& destination, uint64_t frameCount)
      : source_(source), destination_(destination), frameCount_(frameCount) { advance(); }

  Json take(uint64_t tick, uint32_t rate) {
    if (next_.is_null() || nextTick_ > tick) return nullptr;
    if (nextTick_ < tick) invalid(stem_, "camera tick is duplicate or out of order");
    checkTime(next_, tick, rate, stem_);
    const std::string image = stem_ + ".exr";
    copyInput(source_ / image, destination_ / image, maxImageBytes);
    const auto fingerprint = util::FingerprintFileContent(destination_ / image);
    Json frame{{"frame", std::move(next_)}, {"image", image},
               {"sha256", fingerprint.digest}, {"byteSize", fingerprint.byteSize}};
    ++index_;
    advance();
    return frame;
  }

  bool exhausted() const { return index_ == frameCount_; }

 private:
  void advance() {
    if (exhausted()) {
      next_ = nullptr;
      return;
    }
    stem_ = std::format("frame-{:08}", index_);
    next_ = readMetadata(source_ / (stem_ + ".json"));
    nextTick_ = naturalField(next_, "tick", stem_);
    if (naturalField(next_, "sampleIndex", stem_) != index_)
      invalid(stem_, "camera sample index does not match its filename");
  }

  const fs::path& source_;
  const fs::path& destination_;
  uint64_t frameCount_{};
  uint64_t index_{};
  uint64_t nextTick_{};
  std::string stem_;
  Json next_;
};

Json writeChunks(const fs::path& destination, const fs::path& scratch, const fs::path& cameraSource,
                 const DatasetSimulationReport& report, uint64_t cameraCount, uint32_t ticksPerChunk) {
  SimulationCursor base(scratch / spoolNames[0]);
  SimulationCursor imu(scratch / spoolNames[1]);
  SimulationCursor lidar(scratch / spoolNames[2]);
  CameraCursor camera(cameraSource, destination, cameraCount);
  Json chunks = Json::array();
  for (uint64_t first = 0; first < report.tickCount; first += ticksPerChunk) {
    const auto name = std::format("chunk-{:08}.jsonl", chunks.size());
    const auto path = destination / name;
    std::ofstream output(path, std::ios::binary);
    if (!output) invalid(name, "could not open output chunk");
    const uint64_t stop = std::min<uint64_t>(report.tickCount, first + ticksPerChunk);
    for (uint64_t tick = first; tick < stop; ++tick) {
      auto pose = base.take(tick, report.ticksPerSecond);
      if (pose.is_null()) invalid(name, std::format("missing committed pose at tick {}", tick));
      Json row{{"tick", tick}, {"seconds", static_cast<double>(tick) / report.ticksPerSecond},
               {"base", std::move(pose)}, {"imu", imu.take(tick, report.ticksPerSecond)},
               {"lidar", lidar.take(tick, report.ticksPerSecond)}, {"camera", camera.take(tick, report.ticksPerSecond)}};
      if (!(output << row.dump() << '\n')) invalid(name, "could not write aligned observation");
    }
    output.close();
    if (!output) invalid(name, "could not finish output chunk");
    const auto fingerprint = util::FingerprintFileContent(path);
    chunks.push_back({{"sha256", fingerprint.digest}, {"byteSize", fingerprint.byteSize}});
  }
  if (!base.exhausted() || !imu.exhausted() || !lidar.exhausted() || !camera.exhausted())
    invalid("dataset timeline", "a sensor observation falls outside the committed simulation ticks");
  return chunks;
}

} // namespace

void writeRoboticsDataset(const fs::path& cameraDirectory, const fs::path& simulationReport,
                          const fs::path& outputDirectory, const RoboticsDatasetWriteOptions& options) {
  if (options.ticksPerChunk == 0 || options.ticksPerChunk > 1024)
    invalid("ticks per chunk", "must be in [1, 1024]");
  if (!fs::is_directory(fs::symlink_status(cameraDirectory)))
    invalid(util::pathToUtf8(cameraDirectory), "expected a camera dataset directory");
  const auto camera = readMetadata(cameraDirectory / "dataset.json");
  if (camera.value("schema", "") != "container.sensor-camera-dataset" || camera.value("version", 0) != 1 ||
      camera.value("labels", "") != "labels.json" || camera.value("frameImagePattern", "") != "frame-%08d.exr" ||
      camera.value("frameMetadataPattern", "") != "frame-%08d.json")
    invalid("camera dataset", "expected a complete version-1 sensor camera dataset");
  const uint64_t frameCount = naturalField(camera, "frameCount", "camera dataset");
  if (frameCount == 0 || frameCount > 1024) invalid("camera dataset", "frameCount must be in [1, 1024]");

  auto destination = fs::absolute(outputDirectory).lexically_normal();
  if (!destination.has_filename()) destination = destination.parent_path();
  fs::create_directories(destination.parent_path());
  if (!fs::create_directory(destination))
    invalid(util::pathToUtf8(destination), "dataset output directory already exists");
  const RecordingScratch scratch(destination);
  DatasetSimulationReport report;
  const auto fingerprint = util::FingerprintFileWhileReading(simulationReport, [&](std::istream& input) {
    report = spoolReport(input, scratch.path);
  });
  const auto& digest = fingerprint.digest;
  if (!camera.contains("simulationReportSha256") || camera["simulationReportSha256"] != digest)
    invalid("camera dataset", "capture must replay the exact supplied simulation report");

  if (!camera.contains("sceneSource")) invalid("camera dataset", "missing BIM source provenance");
  compareSceneSource(camera["sceneSource"], report.sceneSource);
  copyInput(cameraDirectory / "labels.json", destination / "labels.json", maxLabelsBytes);
  const auto labelFingerprint = util::FingerprintFileContent(destination / "labels.json");
  auto chunks = writeChunks(destination, scratch.path, cameraDirectory, report, frameCount, options.ticksPerChunk);
  Json manifest{
      {"schema", "container.robotics-dataset"}, {"version", 1}, {"sourceNamespace", report.sourceNamespace},
      {"robotId", report.robotId}, {"coordinateConvention", "ROS REP-103"}, {"ticksPerSecond", report.ticksPerSecond},
      {"tickCount", report.tickCount}, {"ticksPerChunk", options.ticksPerChunk}, {"chunkCount", chunks.size()},
      {"chunkPattern", "chunk-%08d.jsonl"}, {"chunks", std::move(chunks)}, {"labels", "labels.json"},
      {"labelFingerprint", {{"sha256", labelFingerprint.digest}, {"byteSize", labelFingerprint.byteSize}}},
      {"cameraFrameCount", frameCount}, {"imuSampleCount", report.imuSampleCount}, {"lidarScanCount", report.lidarScanCount},
      {"sceneSource", report.sceneSource}, {"simulationReportSha256", digest},
      {"sensors", {{"camera", camera}, {"imu", report.imuCalibration}, {"lidar", report.lidarCalibration}}}};

  scratch.finish();
  (void)validateRoboticsDatasetFiles(destination, manifest);
  util::writeFileAtomically(destination / "dataset.json", manifest.dump(2) + '\n', util::AtomicFilePublication::CreateNew);
}

} // namespace container::robotics
