#include "Container/robotics/RoboticsDataset.h"

#include "RoboticsDatasetValidation.h"
#include "Container/utility/ContentFingerprint.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::robotics {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr size_t maxManifestBytes = 64u * 1024u * 1024u;
constexpr size_t maxLineBytes = 32u * 1024u * 1024u;
constexpr size_t maxExrHeaderBytes = 64u * 1024u;

[[noreturn]] void invalid(std::string_view message) {
  throw std::invalid_argument("Robotics dataset: " + std::string(message));
}

void require(bool condition, std::string_view message) {
  if (!condition) invalid(message);
}

const Json& field(const Json& object, std::string_view key) {
  require(object.is_object() && object.contains(key), "missing field " + std::string(key));
  return object.at(key);
}

uint64_t natural(const Json& value, std::string_view name,
                 uint64_t maximum = std::numeric_limits<uint64_t>::max()) {
  require(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0),
          std::string(name) + " must be an unsigned integer");
  const auto result = value.get<uint64_t>();
  require(result <= maximum, std::string(name) + " exceeds its limit");
  return result;
}

double number(const Json& value, std::string_view name) {
  require(value.is_number(), std::string(name) + " must be a finite number");
  const auto result = value.get<double>();
  require(std::isfinite(result), std::string(name) + " must be a finite number");
  return result;
}

std::string string(const Json& value, std::string_view name, bool nonempty = true) {
  require(value.is_string(), std::string(name) + " must be a string");
  auto result = value.get<std::string>();
  require(!nonempty || !result.empty(), std::string(name) + " must not be empty");
  return result;
}

void schema(const Json& value, std::string_view expected) {
  require(string(field(value, "schema"), "schema") == expected && natural(field(value, "version"), "version") == 1,
          "unsupported schema or version for " + std::string(expected));
}

std::string digest(const Json& value) {
  const auto result = string(value, "sha256");
  require(result.size() == 64 && std::ranges::all_of(result, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  }), "sha256 must contain 64 lowercase hexadecimal digits");
  return result;
}

fs::path directoryPath(const fs::path& input) {
  const auto absolute = fs::absolute(input).lexically_normal();
  fs::path current;
  for (const auto& component : absolute) {
    current /= component;
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    require(!error && !fs::is_symlink(status), "directory must not traverse a symbolic link");
  }
  require(fs::is_directory(absolute), "input is not a dataset directory");
  return absolute;
}

fs::path filePath(const fs::path& directory, std::string_view name) {
  // Dataset v1 has no subdirectories. Check the portable string before the
  // platform path parser, which treats backslashes and drive names differently.
  require(!name.empty() && name != "." && name != ".." &&
          name.find_first_of("/\\:") == std::string_view::npos && name.find('\0') == std::string_view::npos,
          "file name must be a confined portable relative name");
  const auto result = directory / std::string(name);
  std::error_code error;
  const auto status = fs::symlink_status(result, error);
  require(!error && fs::is_regular_file(status) && !fs::is_symlink(status),
          "missing, nonregular or symbolic-link file " + std::string(name));
  return result;
}

class JsonShapeValidator {
 public:
  explicit JsonShapeValidator(std::string_view name) : name_(name) {}

  bool null() { return true; }
  bool boolean(bool) { return true; }
  bool number_integer(Json::number_integer_t) { return true; }
  bool number_unsigned(Json::number_unsigned_t) { return true; }
  bool number_float(Json::number_float_t value, const Json::string_t&) {
    if (!std::isfinite(value)) fail("nonfinite JSON number");
    return true;
  }
  bool string(Json::string_t&) { return true; }
  bool binary(Json::binary_t&) { fail("unexpected binary JSON value"); }
  bool start_object(size_t) { return start(); }
  bool start_array(size_t) { return start(); }
  bool end_object() { objectKeys_.pop_back(); return true; }
  bool end_array() { objectKeys_.pop_back(); return true; }
  bool key(Json::string_t& key) {
    if (!objectKeys_.back().insert(key).second) fail("duplicate JSON key");
    return true;
  }
  bool parse_error(size_t, const std::string&, const Json::exception&) { fail("invalid JSON"); }

 private:
  bool start() {
    if (objectKeys_.size() == 64) fail("JSON nesting exceeds its limit");
    objectKeys_.emplace_back();
    return true;
  }
  [[noreturn]] void fail(std::string_view reason) const {
    invalid(std::string(reason) + " in " + std::string(name_));
  }

  std::string_view name_;
  std::vector<std::set<std::string>> objectKeys_;
};

Json parse(std::string_view bytes, std::string_view name) {
  // The DOM callback parser scans the parent array at each object end.
  // Validate shape separately to keep a full LiDAR scan linear in its size.
  JsonShapeValidator validate(name);
  if (!Json::sax_parse(bytes, &validate)) invalid("invalid JSON in " + std::string(name));
  try {
    return Json::parse(bytes);
  } catch (const Json::exception& error) {
    invalid("invalid JSON in " + std::string(name) + ": " + error.what());
  }
}

Json readJson(const fs::path& path) {
  const auto size = fs::file_size(path);
  require(size > 0 && size <= maxManifestBytes, "JSON manifest size exceeds its limit");
  std::ifstream input(path, std::ios::binary);
  require(bool(input), "cannot open JSON manifest");
  std::string bytes(static_cast<size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  require(input.gcount() == static_cast<std::streamsize>(bytes.size()) && input.peek() == EOF,
          "JSON manifest changed or was truncated while reading");
  return parse(bytes, path.filename().string());
}

void verifyFingerprint(const fs::path& path, const Json& descriptor) {
  const auto expectedDigest = digest(field(descriptor, "sha256"));
  const auto expectedSize = natural(field(descriptor, "byteSize"), "byteSize");
  require(fs::file_size(path) == expectedSize, "byteSize mismatch for " + path.filename().string());
  const auto actual = util::FingerprintFileContent(path);
  require(actual.byteSize == expectedSize && actual.digest == expectedDigest,
          "sha256 mismatch for " + path.filename().string());
}

void timestamp(const Json& sample, uint64_t tick, uint32_t frequency) {
  require(natural(field(sample, "tick"), "tick") == tick, "sample tick does not match its row");
  const double expected = double(tick) / frequency;
  const double seconds = number(field(sample, "seconds"), "seconds");
  require(seconds >= 0 && std::abs(seconds - expected) <= 1e-12 * std::max(1.0, expected),
          "sample seconds do not match its fixed tick");
}

void vector(const Json& value, size_t components, std::string_view name) {
  require(value.is_array() && value.size() == components, "invalid " + std::string(name) + " components");
  for (const auto& component : value) (void)number(component, name);
}

void pose(const Json& value) {
  vector(field(value, "position"), 3, "drive pose position");
  (void)number(field(value, "yaw"), "drive pose yaw");
}

void validateBase(const Json& value) {
  pose(field(value, "base"));
  for (const std::string_view name : {"odometry", "commanded"})
    if (value.contains(name) && !value.at(name).is_null()) pose(value.at(name));
  if (value.contains("odometryCovariance")) vector(value.at("odometryCovariance"), 9, "odometry covariance");
  for (const std::string_view name : {"wheelAngles", "wheelRates", "encoderAngles", "targetWheelRates", "groundWheelRates"}) {
    if (!value.contains(name)) continue;
    for (const std::string_view side : {"left", "right"}) (void)number(field(value.at(name), side), name);
  }
}

void validateImu(const Json& value) {
  vector(field(value, "specificForce"), 3, "specificForce");
  vector(field(value, "angularVelocity"), 3, "angularVelocity");
  for (const std::string_view side : {"left", "right"}) {
    const auto& count = field(field(value, "encoderCounts"), side);
    require(count.is_number_integer() && (!count.is_number_unsigned() ||
            count.get<uint64_t>() <= uint64_t(std::numeric_limits<int64_t>::max())), "invalid encoder count");
    (void)number(field(field(value, "encoderRadians"), side), "encoderRadians");
  }
  pose(field(value, "odometry"));
}

void validateLidar(const Json& value, const Json& calibration) {
  const auto& mount = field(value, "worldFromSensor");
  vector(field(mount, "xyz"), 3, "worldFromSensor xyz");
  const auto& rotation = field(mount, "quaternionXyzw");
  vector(rotation, 4, "worldFromSensor quaternion");
  double norm = 0;
  for (const auto& component : rotation) norm = std::hypot(norm, component.get<double>());
  require(std::abs(norm - 1.0) <= 1e-6, "LiDAR pose quaternion must have unit length");
  const auto& returns = field(value, "returns");
  const auto rays = natural(field(calibration, "raysPerScan"), "raysPerScan", 65'536);
  require(rays > 0 && returns.is_array() && returns.size() == rays, "LiDAR return count mismatch");
  const auto minimum = number(field(calibration, "minRangeMeters"), "minRangeMeters");
  const auto maximum = number(field(calibration, "maxRangeMeters"), "maxRangeMeters");
  require(minimum >= 0 && maximum > minimum, "invalid LiDAR range calibration");
  for (const auto& hit : returns) {
    const auto status = string(field(hit, "status"), "LiDAR return status");
    const auto& range = field(hit, "rangeMeters");
    const auto& entity = field(hit, "entityId");
    if (!entity.is_null()) (void)string(entity, "LiDAR entityId");
    if (status == "valid") {
      const auto distance = number(range, "LiDAR rangeMeters");
      require(distance >= minimum && distance <= maximum, "LiDAR range is outside calibration");
    } else {
      require(status == "no-hit" || status == "below-minimum" || status == "dropped" || status == "noise-out-of-range",
              "invalid LiDAR return status");
      require(range.is_null() && (status != "no-hit" || entity.is_null()), "invalid LiDAR return payload");
    }
  }
}

struct Schedule {
  uint64_t first{}, rate{}, denominator{};
  [[nodiscard]] bool due(uint64_t tick) const {
    if (tick < first) return false;
    const auto elapsed = tick - first;
    return elapsed == 0 || elapsed * rate / denominator != (elapsed - 1) * rate / denominator;
  }
};

std::optional<Schedule> schedule(const Json& calibration, std::string_view rateName, uint32_t frequency) {
  if (calibration.is_null()) return std::nullopt;
  require(calibration.is_object(), "sensor calibration must be an object or null");
  const auto rate = natural(field(calibration, rateName), rateName, uint64_t(frequency) * 1000);
  require(rate > 0, "sensor rate must be positive");
  return Schedule{natural(field(calibration, "firstTick"), "firstTick"), rate,
                  uint64_t(frequency) * 1000};
}

void validateLabels(const Json& labels, const Json& manifest) {
  schema(labels, "container.bim-sensor-labels");
  require(field(labels, "sourceNamespace") == field(manifest, "sourceNamespace"), "label namespace mismatch");
  require(natural(field(labels, "backgroundId"), "backgroundId") == 0, "background label must be zero");
  std::set<uint64_t> classes, instances;
  const auto& classRows = field(labels, "classes");
  const auto& instanceRows = field(labels, "instances");
  require(classRows.is_array() && !classRows.empty() && instanceRows.is_array(), "invalid label tables");
  for (const auto& row : classRows) {
    const auto id = natural(field(row, "semanticId"), "semanticId", UINT32_MAX);
    require(id != 0 && classes.insert(id).second, "duplicate or zero semantic label");
    (void)string(field(row, "ifcClass"), "ifcClass");
  }
  for (const auto& row : instanceRows) {
    const auto id = natural(field(row, "instanceId"), "instanceId", UINT32_MAX);
    require(id != 0 && instances.insert(id).second, "duplicate or zero instance label");
    require(classes.contains(natural(field(row, "semanticId"), "semanticId", UINT32_MAX)),
            "instance references an unknown semantic label");
    (void)string(field(row, "entityId"), "entityId");
  }
}

void validateSource(const Json& source) {
  require(source.is_object(), "sceneSource must be an object");
  const auto& layers = field(source, "layers");
  require(layers.is_array() && !layers.empty() && layers.size() <= 8192, "invalid sceneSource layers");
  for (const auto& layer : layers) {
    (void)digest(field(layer, "sha256"));
    (void)natural(field(layer, "byteSize"), "sceneSource byteSize");
  }
  require(number(field(source, "metersPerUnscaledRendererUnit"), "sceneSource scale") > 0,
          "sceneSource scale must be positive");
}

bool sameSource(const Json& left, const Json& right) {
  validateSource(left);
  validateSource(right);
  const auto a = number(field(left, "metersPerUnscaledRendererUnit"), "sceneSource scale");
  const auto b = number(field(right, "metersPerUnscaledRendererUnit"), "sceneSource scale");
  return field(left, "layers") == field(right, "layers") &&
         std::abs(a - b) <= 1e-6 * std::max(a, b);
}

// EXR validation deliberately supports only the portable scanline subset
// emitted by SensorCameraDataset. It checks all offsets and complete raw
// scanline lengths without allocating or decoding image planes.
class ExrInput {
public:
  explicit ExrInput(const fs::path& path) : stream_(path, std::ios::binary), size_(fs::file_size(path)) {
    require(bool(stream_), "cannot open EXR image");
  }
  uint64_t position() { return static_cast<uint64_t>(stream_.tellg()); }
  uint64_t size() const { return size_; }
  uint64_t integer(unsigned bytes) {
    uint64_t result = 0;
    for (unsigned index = 0; index < bytes; ++index) {
      const auto value = stream_.get();
      require(value != EOF, "truncated EXR image");
      result |= uint64_t(static_cast<unsigned char>(value)) << (8 * index);
    }
    return result;
  }
  std::string terminated() {
    std::string result;
    for (;;) {
      require(position() < maxExrHeaderBytes, "EXR header exceeds its limit");
      const auto value = stream_.get();
      require(value != EOF, "truncated EXR header");
      if (value == 0) return result;
      require(result.size() < 255, "EXR header name exceeds its limit");
      result.push_back(static_cast<char>(value));
    }
  }
  void seek(uint64_t position) {
    require(position <= size_, "EXR offset exceeds image size");
    stream_.seekg(static_cast<std::streamoff>(position));
    require(bool(stream_), "invalid EXR offset");
  }
private:
  std::ifstream stream_;
  uint64_t size_;
};

void validateExr(const fs::path& path, uint32_t width, uint32_t height) {
  ExrInput input(path);
  require(input.integer(4) == 20000630 && input.integer(4) == 2, "unsupported EXR magic, version or flags");
  bool channels = false, window = false, compression = false, order = false;
  std::set<std::string> attributes;
  for (;;) {
    const auto name = input.terminated();
    if (name.empty()) break;
    require(attributes.insert(name).second, "duplicate EXR attribute");
    const auto type = input.terminated();
    const auto size = input.integer(4);
    const auto begin = input.position();
    require(size <= maxExrHeaderBytes && begin <= maxExrHeaderBytes - size, "EXR header exceeds its limit");
    const auto end = begin + size;
    if (name == "channels") {
      require(type == "chlist", "invalid EXR channels attribute");
      std::map<std::string, uint64_t> expected{{"B", 2}, {"G", 2}, {"InstanceID", 0},
                                               {"R", 2}, {"SemanticID", 0}, {"Z", 2}};
      for (;;) {
        const auto channel = input.terminated();
        if (channel.empty()) break;
        const auto found = expected.find(channel);
        require(found != expected.end() && input.integer(4) == found->second, "invalid EXR channel name or pixel type");
        expected.erase(found);
        require(input.integer(1) <= 1 && input.integer(3) == 0 &&
                input.integer(4) == 1 && input.integer(4) == 1, "unsupported EXR channel sampling");
        require(input.position() < end, "invalid EXR channel list length");
      }
      require(expected.empty(), "missing EXR dataset channel");
      channels = true;
    } else if (name == "dataWindow" || name == "displayWindow") {
      require(type == "box2i" && size == 16 && input.integer(4) == 0 && input.integer(4) == 0 &&
              input.integer(4) == width - 1 && input.integer(4) == height - 1, "EXR dimensions do not match calibration");
      if (name == "dataWindow") window = true;
    } else if (name == "compression") {
      require(type == "compression" && size == 1 && input.integer(1) == 0, "unsupported EXR compression");
      compression = true;
    } else if (name == "lineOrder") {
      require(type == "lineOrder" && size == 1 && input.integer(1) == 0, "unsupported EXR scanline order");
      order = true;
    } else {
      input.seek(end);
    }
    require(input.position() == end, "EXR attribute length mismatch");
  }
  require(channels && window && compression && order, "missing required EXR attribute");
  const auto table = input.position();
  const uint64_t rowBytes = uint64_t(width) * 6 * 4;
  const uint64_t firstRow = table + uint64_t(height) * 8;
  require(input.size() == firstRow + uint64_t(height) * (8 + rowBytes), "EXR scanline payload size mismatch");
  for (uint32_t row = 0; row < height; ++row) {
    input.seek(table + uint64_t(row) * 8);
    const auto offset = input.integer(8);
    require(offset == firstRow + uint64_t(row) * (8 + rowBytes), "EXR scanline offset mismatch");
    input.seek(offset);
    require(input.integer(4) == row && input.integer(4) == rowBytes, "EXR scanline index or size mismatch");
  }
}

std::pair<uint32_t, uint32_t> cameraDimensions(const Json& frame) {
  const auto& intrinsics = field(frame, "intrinsics");
  const auto width = natural(field(intrinsics, "width"), "camera width", 8192);
  const auto height = natural(field(intrinsics, "height"), "camera height", 8192);
  require(width > 0 && height > 0 && width * height <= 8'388'608, "invalid camera dimensions");
  require(number(field(intrinsics, "fx"), "fx") > 0 && number(field(intrinsics, "fy"), "fy") > 0,
          "camera focal lengths must be positive");
  (void)number(field(intrinsics, "cx"), "cx");
  (void)number(field(intrinsics, "cy"), "cy");
  const auto near = number(field(intrinsics, "nearMeters"), "nearMeters");
  require(near > 0 && number(field(intrinsics, "farMeters"), "farMeters") > near, "invalid camera depth range");
  const auto& pose = field(frame, "worldFromCamera");
  require(pose.is_array() && pose.size() == 4, "invalid camera world pose");
  for (size_t c = 0; c < 4; ++c) {
    require(pose[c].is_array() && pose[c].size() == 4, "invalid camera world pose column");
    for (size_t r = 0; r < 4; ++r) {
      const auto value = number(pose[c][r], "worldFromCamera");
      if (r == 3) require(value == (c == 3 ? 1.0 : 0.0), "camera world pose must be affine");
    }
  }
  for (size_t a = 0; a < 3; ++a) {
    for (size_t b = 0; b < 3; ++b) {
      double dot = 0;
      for (size_t r = 0; r < 3; ++r) dot += pose[a][r].get<double>() * pose[b][r].get<double>();
      require(std::abs(dot - (a == b ? 1.0 : 0.0)) <= 1e-6, "camera world pose must be rigid");
    }
  }
  const auto component = [&](size_t c, size_t r) { return pose[c][r].get<double>(); };
  const auto determinant = component(0, 0) * (component(1, 1) * component(2, 2) - component(2, 1) * component(1, 2)) -
      component(1, 0) * (component(0, 1) * component(2, 2) - component(2, 1) * component(0, 2)) +
      component(2, 0) * (component(0, 1) * component(1, 2) - component(1, 1) * component(0, 2));
  require(std::abs(determinant - 1.0) <= 1e-6, "camera world pose must be right-handed");
  return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

bool readLine(std::ifstream& input, std::string& line) {
  line.clear();
  std::array<char, 64 * 1024> block;
  for (;;) {
    input.getline(block.data(), static_cast<std::streamsize>(block.size()));
    const auto readCount = input.gcount();
    require(!input.bad(), "cannot read tick chunk");
    if (input.eof()) {
      require(line.empty() && readCount == 0, "truncated tick chunk (missing final newline)");
      return false;
    }
    const bool complete = !input.fail();
    require(readCount > 0, "cannot read tick chunk");
    // getline counts a consumed newline without copying it to the buffer.
    const auto size = static_cast<size_t>(readCount - (complete ? 1 : 0));
    require(size <= maxLineBytes - line.size(), "tick row exceeds its byte limit");
    line.append(block.data(), size);
    if (complete) return true;
    input.clear();
  }
}

} // namespace

RoboticsDatasetSummary validateRoboticsDatasetFiles(const fs::path& directory, Json manifest,
                                                     const RoboticsDatasetObserver& observe) {
  const auto root = directoryPath(directory);
  schema(manifest, "container.robotics-dataset");
  (void)string(field(manifest, "sourceNamespace"), "sourceNamespace");
  (void)string(field(manifest, "robotId"), "robotId");
  require(field(manifest, "coordinateConvention") == "ROS REP-103", "unsupported coordinate convention");
  const auto frequency = natural(field(manifest, "ticksPerSecond"), "ticksPerSecond", 1'000'000);
  const auto ticks = natural(field(manifest, "tickCount"), "tickCount", 8192);
  const auto perChunk = natural(field(manifest, "ticksPerChunk"), "ticksPerChunk", 1024);
  const auto chunks = natural(field(manifest, "chunkCount"), "chunkCount", 8192);
  const auto cameras = natural(field(manifest, "cameraFrameCount"), "cameraFrameCount", 1024);
  const auto imuCount = natural(field(manifest, "imuSampleCount"), "imuSampleCount", ticks);
  const auto lidarCount = natural(field(manifest, "lidarScanCount"), "lidarScanCount", ticks);
  require(frequency > 0 && ticks > 0 && perChunk > 0 && cameras > 0 && cameras <= ticks &&
          chunks == (ticks + perChunk - 1) / perChunk, "invalid tick or chunk counts");
  require(field(manifest, "labels") == "labels.json" && field(manifest, "chunkPattern") == "chunk-%08d.jsonl",
          "unsupported dataset file pattern");
  const auto& chunkDescriptors = field(manifest, "chunks");
  require(chunkDescriptors.is_array() && chunkDescriptors.size() == chunks, "chunk descriptor count mismatch");
  validateSource(field(manifest, "sceneSource"));
  const auto reportDigest = digest(field(manifest, "simulationReportSha256"));
  const auto& sensors = field(manifest, "sensors");
  const auto& camera = field(sensors, "camera");
  schema(camera, "container.sensor-camera-dataset");
  require(natural(field(camera, "frameCount"), "camera frameCount") == cameras &&
          sameSource(field(camera, "sceneSource"), field(manifest, "sceneSource")) &&
          digest(field(camera, "simulationReportSha256")) == reportDigest,
          "camera source provenance or frame count mismatch");
  require(field(camera, "labels") == "labels.json" && field(camera, "frameImagePattern") == "frame-%08d.exr" &&
          field(camera, "frameMetadataPattern") == "frame-%08d.json" && field(camera, "colorSpace") == "linear-sRGB" &&
          field(camera, "depthUnits") == "metres" && field(camera, "surfacePolicy") == "opaque-and-alpha-mask",
          "unsupported camera dataset conventions");
  const auto imuSchedule = schedule(field(sensors, "imu"), "samplesPerSecondMilliHz", static_cast<uint32_t>(frequency));
  const auto lidarSchedule = schedule(field(sensors, "lidar"), "scansPerSecondMilliHz", static_cast<uint32_t>(frequency));
  require(imuSchedule.has_value() || imuCount == 0, "IMU samples lack calibration");
  require(lidarSchedule.has_value() || lidarCount == 0, "LiDAR scans lack calibration");
  const auto labelPath = filePath(root, "labels.json");
  verifyFingerprint(labelPath, field(manifest, "labelFingerprint"));
  auto labels = readJson(labelPath);
  validateLabels(labels, manifest);

  std::set<std::string> expected{"dataset.json", "labels.json"};
  for (uint64_t chunk = 0; chunk < chunks; ++chunk) {
    const auto name = std::format("chunk-{:08}.jsonl", chunk);
    expected.insert(name);
    verifyFingerprint(filePath(root, name), chunkDescriptors[chunk]);
  }
  uint64_t seenCamera = 0, seenImu = 0, seenLidar = 0;
  std::optional<Json> intrinsics;
  for (uint64_t chunk = 0; chunk < chunks; ++chunk) {
    const auto name = std::format("chunk-{:08}.jsonl", chunk);
    std::ifstream input(filePath(root, name), std::ios::binary);
    require(bool(input), "cannot open tick chunk");
    std::string line;
    const auto end = std::min(ticks, (chunk + 1) * perChunk);
    for (uint64_t tick = chunk * perChunk; tick < end; ++tick) {
      require(readLine(input, line) && !line.empty(), "missing tick row in " + name);
      const auto row = parse(line, name + " tick " + std::to_string(tick));
      timestamp(row, tick, static_cast<uint32_t>(frequency));
      timestamp(field(row, "base"), tick, static_cast<uint32_t>(frequency));
      validateBase(field(row, "base"));
      const auto& imu = field(row, "imu");
      const auto& lidar = field(row, "lidar");
      require((!imu.is_null()) == (imuSchedule && imuSchedule->due(tick)), "IMU sample schedule mismatch");
      require((!lidar.is_null()) == (lidarSchedule && lidarSchedule->due(tick)), "LiDAR scan schedule mismatch");
      if (!imu.is_null()) {
        timestamp(imu, tick, static_cast<uint32_t>(frequency));
        validateImu(imu);
        ++seenImu;
      }
      if (!lidar.is_null()) {
        timestamp(lidar, tick, static_cast<uint32_t>(frequency));
        validateLidar(lidar, field(sensors, "lidar"));
        ++seenLidar;
      }
      const auto& capture = field(row, "camera");
      if (!capture.is_null()) {
        require(seenCamera < cameras, "too many camera frames");
        const auto& frame = field(capture, "frame");
        schema(frame, "container.sensor-camera-frame");
        timestamp(frame, tick, static_cast<uint32_t>(frequency));
        require(natural(field(frame, "sampleIndex"), "sampleIndex") == seenCamera, "camera sample index mismatch");
        const auto imageName = string(field(capture, "image"), "image");
        require(imageName == std::format("frame-{:08}.exr", seenCamera), "camera image name or index mismatch");
        const auto image = filePath(root, imageName);
        const auto [width, height] = cameraDimensions(frame);
        if (intrinsics) require(*intrinsics == field(frame, "intrinsics"), "camera calibration changed during dataset");
        else intrinsics = field(frame, "intrinsics");
        verifyFingerprint(image, capture);
        validateExr(image, width, height);
        expected.insert(imageName);
        ++seenCamera;
      }
      if (observe) observe(row);
    }
    require(!readLine(input, line), "extra tick row in " + name);
  }
  require(seenCamera == cameras && seenImu == imuCount && seenLidar == lidarCount, "sensor sample count mismatch");
  for (const auto& entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    require(expected.contains(name), "unexpected dataset file " + name);
    (void)filePath(root, name);
  }
  return {std::move(manifest), std::move(labels)};
}

RoboticsDatasetSummary readRoboticsDataset(const fs::path& directory, const RoboticsDatasetObserver& observe) {
  const auto root = directoryPath(directory);
  return validateRoboticsDatasetFiles(root, readJson(filePath(root, "dataset.json")), observe);
}

} // namespace container::robotics
