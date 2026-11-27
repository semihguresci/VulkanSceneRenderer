#include "DatasetSimulationReport.h"

#include "Container/robotics/SensorSchedule.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <streambuf>
#include <string_view>
#include <utility>
#include <vector>

namespace container::robotics {
namespace {

using Json = nlohmann::json;
using enum DatasetReportStream;

// Match the producer's v1 history and LiDAR limits. Allow one complete scan
// with 65,536 objects and their three fields in the observation DOM.
constexpr uint64_t maxTicks = 8192;
constexpr uint64_t maxRaysPerScan = 65'536;
constexpr uint64_t maxLidarReturns = 1'000'000;
constexpr size_t maxDepth = 32;
constexpr size_t maxTokenBytes = 64 * 1024;

[[noreturn]] void invalid(std::string_view field, std::string_view problem) {
  throw std::invalid_argument("BIM simulation report " + std::string(field) + ": " +
                              std::string(problem));
}

std::string field(std::string_view object, std::string_view key) {
  return std::string(object) + "." + std::string(key);
}

// The JSON lexer allocates tokens before calling SAX callbacks. Limit the
// encoded token at the stream boundary, including discarded metadata.
class TokenLimitedInput final : public std::streambuf {
public:
  explicit TokenLimitedInput(std::streambuf& input) : input_(input) {}

protected:
  int_type underflow() override {
    const auto next = input_.sbumpc();
    if (traits_type::eq_int_type(next, traits_type::eof())) return traits_type::eof();
    byte_ = traits_type::to_char_type(next);
    if (quoted_) {
      if (!escaped_ && byte_ == '"') {
        quoted_ = false;
        tokenBytes_ = 0;
      } else {
        escaped_ = !escaped_ && byte_ == '\\';
        countByte();
      }
    } else if (byte_ == '"') {
      quoted_ = true;
      tokenBytes_ = 0;
    } else if (std::string_view{"{}[]:, \r\n\t"}.find(byte_) != std::string_view::npos) {
      tokenBytes_ = 0;
    } else {
      countByte();
    }
    setg(&byte_, &byte_, &byte_ + 1);
    return next;
  }

private:
  void countByte() {
    if (++tokenBytes_ > maxTokenBytes) invalid("JSON", "individual token exceeds the 64 KiB limit");
  }

  std::streambuf& input_;
  size_t tokenBytes_{};
  char byte_{};
  bool quoted_{};
  bool escaped_{};
};

struct DomBudget {
  size_t nodes;
  size_t stringBytes;

  void consume(std::string_view key, const Json& value, std::string_view context) {
    const auto bytes = key.size() + (value.is_string() ? value.get_ref<const std::string&>().size() : 0);
    if (nodes == 0 || bytes > stringBytes) invalid(context, "exceeds the JSON subtree limit");
    --nodes;
    stringBytes -= bytes;
  }
};

const Json& required(const Json& object, std::string_view key, std::string_view context) {
  if (!object.is_object()) invalid(context, "must be an object");
  const auto value = object.find(key);
  if (value == object.end()) invalid(field(context, key), "is required");
  return *value;
}

uint64_t natural(const Json& value, std::string_view context,
                 uint64_t maximum = std::numeric_limits<uint64_t>::max()) {
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<int64_t>() < 0) ||
      value.get<uint64_t>() > maximum)
    invalid(context, "must be a nonnegative integer within the report limit");
  return value.get<uint64_t>();
}

double number(const Json& value, std::string_view context) {
  if (!value.is_number()) invalid(context, "must be a finite number");
  const double result = value.get<double>();
  if (!std::isfinite(result)) invalid(context, "must be a finite number");
  return result;
}

const std::string& nonemptyString(const Json& value, std::string_view context) {
  if (!value.is_string() || value.get_ref<const std::string&>().empty())
    invalid(context, "must be a nonempty string");
  return value.get_ref<const std::string&>();
}

void vector(const Json& value, size_t count, std::string_view context) {
  if (!value.is_array() || value.size() != count)
    invalid(context, "has the wrong number of components");
  for (const auto& component : value) number(component, context);
}

void drivePose(const Json& value, std::string_view context) {
  vector(required(value, "position", context), 3, field(context, "position"));
  number(required(value, "yaw", context), field(context, "yaw"));
}

void lidarPose(const Json& value, std::string_view context) {
  vector(required(value, "xyz", context), 3, field(context, "xyz"));
  const auto quaternionField = field(context, "quaternionXyzw");
  const auto& rotation = required(value, "quaternionXyzw", context);
  vector(rotation, 4, quaternionField);
  const double norm = std::hypot(std::hypot(rotation[0].get<double>(), rotation[1].get<double>()),
                                 std::hypot(rotation[2].get<double>(), rotation[3].get<double>()));
  if (std::abs(norm - 1.0) > 1e-6) invalid(quaternionField, "must be a unit quaternion");
}

void wheels(const Json& value, std::string_view context, bool counts = false) {
  for (const std::string_view side : {"left", "right"}) {
    const auto& wheel = required(value, side, context);
    const auto component = field(context, side);
    if (counts) {
      if (!wheel.is_number_integer() ||
          (wheel.is_number_unsigned() && wheel.get<uint64_t>() > uint64_t(std::numeric_limits<int64_t>::max())))
        invalid(component, "must be a signed 64-bit encoder count");
    } else {
      number(wheel, component);
    }
  }
}

void validateBase(const Json& row, std::string_view context) {
  drivePose(required(row, "base", context), field(context, "base"));
  if (const auto odometry = row.find("odometry"); odometry != row.end())
    drivePose(*odometry, field(context, "odometry"));
  if (const auto commanded = row.find("commanded"); commanded != row.end() && !commanded->is_null())
    drivePose(*commanded, field(context, "commanded"));
  for (const std::string_view key : {"wheelAngles", "wheelRates", "encoderAngles", "targetWheelRates",
                                    "groundWheelRates"})
    if (const auto value = row.find(key); value != row.end()) wheels(*value, field(context, key));
  if (const auto value = row.find("odometryCovariance"); value != row.end())
    vector(*value, 9, field(context, "odometryCovariance"));
  for (const std::string_view key : {"obstruction", "blockingEntityId"})
    if (const auto value = row.find(key); value != row.end() && !value->is_string())
      invalid(field(context, key), "must be a string");
}

void validateImu(const Json& row, std::string_view context) {
  for (const std::string_view key : {"specificForce", "angularVelocity"})
    vector(required(row, key, context), 3, field(context, key));
  wheels(required(row, "encoderCounts", context), field(context, "encoderCounts"), true);
  wheels(required(row, "encoderRadians", context), field(context, "encoderRadians"));
  drivePose(required(row, "odometry", context), field(context, "odometry"));
}

struct LidarReturns {
  std::optional<size_t> raysPerScan;
  uint64_t total{};
  double minimum{std::numeric_limits<double>::infinity()};
  double maximum{-std::numeric_limits<double>::infinity()};

  void validate(const Json& row, std::string_view context) {
    lidarPose(required(row, "worldFromSensor", context), field(context, "worldFromSensor"));
    const auto returnsField = field(context, "returns");
    const auto& returns = required(row, "returns", context);
    if (!returns.is_array() || returns.empty() || returns.size() > maxRaysPerScan)
      invalid(returnsField, "must contain 1..65536 LiDAR returns");
    if (raysPerScan && returns.size() != *raysPerScan)
      invalid(returnsField, "ray count changed between scans");
    raysPerScan = returns.size();
    total += returns.size();
    if (total > maxLidarReturns) invalid(returnsField, "report exceeds one million LiDAR returns");

    for (size_t i = 0; i < returns.size(); ++i) {
      const auto& hit = returns[i];
      const auto hitField = returnsField + "[" + std::to_string(i) + "]";
      const auto& status = nonemptyString(required(hit, "status", hitField), field(hitField, "status"));
      const auto& entity = required(hit, "entityId", hitField);
      if (!entity.is_null()) nonemptyString(entity, field(hitField, "entityId"));
      const auto& range = required(hit, "rangeMeters", hitField);
      const auto rangeField = field(hitField, "rangeMeters");
      if (status == "valid") {
        const auto distance = number(range, rangeField);
        if (distance < 0) invalid(rangeField, "must be nonnegative");
        minimum = std::min(minimum, distance);
        maximum = std::max(maximum, distance);
      } else {
        if (status != "no-hit" && status != "below-minimum" && status != "dropped" &&
            status != "noise-out-of-range")
          invalid(field(hitField, "status"), "unknown LiDAR return status");
        if (!range.is_null()) invalid(rangeField, "must be null for an invalid return");
        if (status == "no-hit" && !entity.is_null())
          invalid(field(hitField, "entityId"), "must be null for a miss");
      }
    }
  }
};

struct ScheduledRows {
  bool sawArray{};
  std::vector<uint64_t> ticks;

  void verify(const Json& calibration, std::string_view context, std::string_view rateKey,
              uint32_t ticksPerSecond, uint64_t finalTick) const {
    if (calibration.is_null()) return;
    if (!sawArray) invalid(context, "is missing its observation array");
    const auto rateField = field(context, rateKey);
    const auto rate = natural(required(calibration, rateKey, context), rateField, uint64_t(ticksPerSecond) * 1000);
    if (rate == 0) invalid(rateField, "must be positive");
    const auto first = natural(required(calibration, "firstTick", context), field(context, "firstTick"));
    const SensorSchedule schedule(ticksPerSecond, static_cast<uint32_t>(rate), first);
    if (ticks.size() != schedule.samplesThrough(finalTick))
      invalid(context, "observation count does not match the authored schedule and final base tick");
    for (const auto tick : ticks)
      if (tick > finalTick || !schedule.due(tick))
        invalid(context, "observation tick " + std::to_string(tick) + " is outside the authored schedule");
  }
};

enum class Role { Root, Sensor, Rows, Observation, Value, Skip };

struct Frame {
  Role role{Role::Skip};
  bool object{};
  Json* destination{};
  DatasetReportStream stream{Base};
  DomBudget* budget{};
  std::string key;
};

class ReportSax {
public:
  explicit ReportSax(const DatasetReportObserver& observe, bool baseHistoryOnly)
      : observe_(observe), baseHistoryOnly_(baseHistoryOnly) {}

  bool null() { return scalar(nullptr); }
  bool boolean(bool value) { return scalar(value); }
  bool number_integer(Json::number_integer_t value) { return scalar(value); }
  bool number_unsigned(Json::number_unsigned_t value) { return scalar(value); }
  bool number_float(Json::number_float_t value, const Json::string_t&) {
    if (!std::isfinite(value)) invalid("JSON", "number must be finite");
    return scalar(value);
  }
  bool string(Json::string_t& value) {
    if (!frames_.empty() && frames_.back().role == Role::Skip) return true;
    return scalar(std::move(value));
  }
  bool binary(Json::binary_t&) { invalid("JSON", "binary values are unsupported"); }
  bool start_object(size_t) { return start(true); }
  bool start_array(size_t) { return start(false); }
  bool key(Json::string_t& value) {
    if (frames_.back().role != Role::Skip) frames_.back().key = std::move(value);
    return true;
  }
  bool end_object() { return end(); }
  bool end_array() { return end(); }
  bool parse_error(size_t byte, const std::string&, const Json::exception&) {
    invalid("JSON", "malformed input at byte " + std::to_string(byte));
  }

  DatasetSimulationReport finish() {
    if (required(root_, "schema", "root") != "container.bim-robotics-simulation")
      invalid("schema", "must be container.bim-robotics-simulation");
    if (natural(required(root_, "version", "root"), "version", 1) != 1)
      invalid("version", "must be 1");
    if (required(root_, "coordinateConvention", "root") != "ROS REP-103")
      invalid("coordinateConvention", "must be ROS REP-103");
    const auto frequency = natural(required(root_, "ticksPerSecond", "root"), "ticksPerSecond",
                                   baseHistoryOnly_ ? 1'000'000 : 10'000);
    if (frequency == 0) invalid("ticksPerSecond", "must be positive");
    result_.ticksPerSecond = static_cast<uint32_t>(frequency);
    if (result_.tickCount == 0) invalid("history", "must contain the initial base tick");
    if (baseHistoryOnly_) return std::move(result_);
    result_.sourceNamespace = nonemptyString(required(root_, "sourceNamespace", "root"), "sourceNamespace");
    result_.robotId = nonemptyString(required(root_, "robotId", "root"), "robotId");
    const auto finalTick = result_.tickCount - 1;
    imu_.verify(result_.imuCalibration, "driveSensors", "samplesPerSecondMilliHz", result_.ticksPerSecond, finalTick);
    lidar_.verify(result_.lidarCalibration, "lidar", "scansPerSecondMilliHz", result_.ticksPerSecond, finalTick);
    validateImuCalibration();
    validateLidarCalibration();
    result_.imuSampleCount = imu_.ticks.size();
    result_.lidarScanCount = lidar_.ticks.size();
    return std::move(result_);
  }

private:
  static std::string_view rowName(DatasetReportStream stream) {
    switch (stream) {
    case Base: return "history";
    case Imu: return "driveSensors.samples";
    case Lidar: return "lidar.scans";
    }
    throw std::logic_error("Unknown simulation report stream");
  }

  Frame insert(Frame& parent, std::string key, Json value) {
    parent.budget->consume(key, value, parent.budget == &metadataBudget_ ? "metadata" : rowName(parent.stream));
    Json* inserted;
    if (parent.object) {
      auto [position, fresh] = parent.destination->get_ref<Json::object_t&>().try_emplace(key, std::move(value));
      if (!fresh) invalid(key, "duplicate object key");
      inserted = &position->second;
    } else {
      inserted = &parent.destination->emplace_back(std::move(value));
    }
    return {Role::Value, inserted->is_object(), inserted, parent.stream, parent.budget};
  }

  Frame rootValue(std::string key, Json value) {
    if (baseHistoryOnly_ && key != "history" && key != "schema" && key != "version" &&
        key != "coordinateConvention" && key != "ticksPerSecond")
      return {};
    if (key != "history" && key != "driveSensors" && key != "lidar" && key != "sceneSource" &&
        key != "schema" && key != "version" && key != "coordinateConvention" &&
        key != "sourceNamespace" && key != "robotId" && key != "ticksPerSecond")
      return {};
    if (!rootKeys_.insert(key).second) invalid(key, "duplicate root key");
    if (key == "history") {
      if (!value.is_array()) invalid(key, "must be an array");
      return {Role::Rows, false, nullptr, Base};
    }
    if (key == "driveSensors" || key == "lidar") {
      if (!value.is_object()) invalid(key, "must be an object");
      const auto stream = key == "lidar" ? Lidar : Imu;
      auto& calibration = stream == Lidar ? result_.lidarCalibration : result_.imuCalibration;
      calibration = std::move(value);
      return {Role::Sensor, true, &calibration, stream, &metadataBudget_};
    }
    if (key == "sceneSource") {
      if (!value.is_object()) invalid(key, "must be an object");
      result_.sceneSource = std::move(value);
      return {Role::Value, true, &result_.sceneSource, Base, &metadataBudget_};
    }
    if (value.is_structured()) invalid(key, "must be a scalar");
    Frame root{Role::Value, true, &root_, Base, &metadataBudget_};
    return insert(root, std::move(key), std::move(value));
  }

  Frame child(Json value) {
    auto& parent = frames_.back();
    auto key = std::exchange(parent.key, {});
    switch (parent.role) {
    case Role::Root: return rootValue(std::move(key), std::move(value));
    case Role::Sensor:
      if (const auto stream = parent.stream; key == (stream == Lidar ? "scans" : "samples")) {
        auto& rows = stream == Lidar ? lidar_ : imu_;
        if (rows.sawArray) invalid(rowName(stream), "duplicate observation array");
        if (!value.is_array()) invalid(rowName(stream), "must be an array");
        rows.sawArray = true;
        return {Role::Rows, false, nullptr, stream};
      }
      return insert(parent, std::move(key), std::move(value));
    case Role::Rows: {
      const auto stream = parent.stream;
      const auto count = stream == Base ? result_.tickCount : (stream == Lidar ? lidar_.ticks.size() : imu_.ticks.size());
      if (count == maxTicks) invalid(rowName(stream), "exceeds 8192 observations");
      if (!value.is_object()) invalid(rowName(stream), "each observation must be an object");
      row_ = std::move(value);
      rowBudget_ = {400'000, 64 * 1024 * 1024};
      return {Role::Observation, true, &row_, stream, &rowBudget_};
    }
    case Role::Observation:
      if (baseHistoryOnly_ && key != "tick" && key != "seconds" && key != "base") return {};
      return insert(parent, std::move(key), std::move(value));
    case Role::Value: return insert(parent, std::move(key), std::move(value));
    case Role::Skip: return {};
    }
    throw std::logic_error("Unknown simulation report parser state");
  }

  bool scalar(Json value) {
    if (frames_.empty()) invalid("root", "must be an object");
    child(std::move(value));
    return true;
  }

  bool start(bool object) {
    if (frames_.size() == maxDepth) invalid("JSON", "nesting exceeds 32 levels");
    if (frames_.empty()) {
      if (!object) invalid("root", "must be an object");
      frames_.push_back({Role::Root, true});
    } else if (frames_.back().role == Role::Skip) {
      frames_.emplace_back();
    } else {
      frames_.push_back(child(object ? Json::object() : Json::array()));
    }
    return true;
  }

  bool end() {
    const auto role = frames_.back().role;
    const auto stream = frames_.back().stream;
    frames_.pop_back();
    if (role != Role::Observation) return true;
    const auto index = stream == Base ? result_.tickCount : (stream == Lidar ? lidar_.ticks.size() : imu_.ticks.size());
    const auto context = std::string(rowName(stream)) + "[" + std::to_string(index) + "]";
    const auto tick = natural(required(row_, "tick", context), field(context, "tick"), maxTicks - 1);
    if (number(required(row_, "seconds", context), field(context, "seconds")) < 0)
      invalid(field(context, "seconds"), "must be nonnegative");
    if (stream == Base) {
      if (tick != result_.tickCount) invalid(field(context, "tick"), "base ticks must be contiguous from zero");
      if (baseHistoryOnly_)
        drivePose(required(row_, "base", context), field(context, "base"));
      else
        validateBase(row_, context);
      ++result_.tickCount;
    } else {
      auto& rows = stream == Lidar ? lidar_ : imu_;
      if (!rows.ticks.empty() && rows.ticks.back() >= tick)
        invalid(field(context, "tick"), "observation ticks must be strictly increasing");
      if (stream == Lidar) lidarReturns_.validate(row_, context);
      else validateImu(row_, context);
      rows.ticks.push_back(tick);
    }
    if (observe_) observe_(stream, row_);
    row_ = nullptr;
    return true;
  }

  void validateImuCalibration() const {
    const auto& calibration = result_.imuCalibration;
    if (calibration.is_null()) return;
    if (const auto frame = calibration.find("imuFrame"); frame != calibration.end() && *frame != "base")
      invalid("driveSensors.imuFrame", "must be base");
    for (const std::string_view key : {"accelerometer", "gyroscope"}) {
      const auto axis = calibration.find(key);
      if (axis == calibration.end()) continue;
      const auto context = field("driveSensors", key);
      for (const std::string_view setting : {"bias", "noiseStdDev", "driftPerSecond"})
        vector(required(*axis, setting, context), 3, field(context, setting));
    }
    if (const auto encoders = calibration.find("encoders"); encoders != calibration.end()) {
      constexpr std::string_view context = "driveSensors.encoders";
      const auto countsField = field(context, "countsPerRevolution");
      if (natural(required(*encoders, "countsPerRevolution", context), countsField,
                  std::numeric_limits<uint32_t>::max()) == 0)
        invalid(countsField, "must be positive");
      for (const std::string_view key : {"biasRadians", "noiseStdDevRadians", "driftRadiansPerSecond"})
        wheels(required(*encoders, key, context), field(context, key));
    }
  }

  void validateLidarCalibration() const {
    const auto& calibration = result_.lidarCalibration;
    if (calibration.is_null()) return;
    const auto rays = natural(required(calibration, "raysPerScan", "lidar"), "lidar.raysPerScan", maxRaysPerScan);
    const auto azimuths = natural(required(calibration, "azimuthSamples", "lidar"), "lidar.azimuthSamples", maxRaysPerScan);
    const auto& elevations = required(calibration, "elevationAngles", "lidar");
    if (rays == 0 || azimuths == 0 || !elevations.is_array() || elevations.empty() ||
        elevations.size() > maxRaysPerScan / azimuths || elevations.size() * azimuths != rays)
      invalid("lidar.raysPerScan", "must match the organized azimuth and elevation counts");
    if (lidarReturns_.raysPerScan && *lidarReturns_.raysPerScan != rays)
      invalid("lidar.raysPerScan", "does not match the observed ray count");
    const auto minimum = number(required(calibration, "minRangeMeters", "lidar"), "lidar.minRangeMeters");
    const auto maximum = number(required(calibration, "maxRangeMeters", "lidar"), "lidar.maxRangeMeters");
    if (minimum < 0 || maximum <= minimum)
      invalid("lidar", "calibration requires 0 <= minRangeMeters < maxRangeMeters");
    if (lidarReturns_.minimum < minimum || lidarReturns_.maximum > maximum)
      invalid("lidar.scans", "valid ranges must lie inside the authored calibration interval");
    for (const auto& elevation : elevations)
      if (std::abs(number(elevation, "lidar.elevationAngles")) > std::numbers::pi / 2)
        invalid("lidar.elevationAngles", "angles must lie in [-pi/2, pi/2]");
    if (const auto azimuth = calibration.find("azimuthStart"); azimuth != calibration.end())
      number(*azimuth, "lidar.azimuthStart");
    if (const auto azimuth = calibration.find("azimuthSpan"); azimuth != calibration.end()) {
      const auto span = number(*azimuth, "lidar.azimuthSpan");
      if (span < 0 || span > 2 * std::numbers::pi || (span == 0 && azimuths != 1))
        invalid("lidar.azimuthSpan", "must lie in [0, 2*pi], with zero span only for one azimuth");
    }
    lidarPose(required(calibration, "baseFromSensor", "lidar"), "lidar.baseFromSensor");
  }

  const DatasetReportObserver& observe_;
  const bool baseHistoryOnly_;
  DatasetSimulationReport result_;
  Json root_ = Json::object();
  std::set<std::string, std::less<>> rootKeys_;
  std::vector<Frame> frames_;
  Json row_;
  DomBudget metadataBudget_{100'000, 4 * 1024 * 1024};
  DomBudget rowBudget_{};
  ScheduledRows imu_;
  ScheduledRows lidar_;
  LidarReturns lidarReturns_;
};

DatasetSimulationReport scanReport(std::istream& raw, const DatasetReportObserver& observe,
                                   bool baseHistoryOnly) {
  TokenLimitedInput buffer(*raw.rdbuf());
  std::istream input(&buffer);
  ReportSax parser(observe, baseHistoryOnly);
  if (!Json::sax_parse(input, &parser)) invalid("JSON", "report could not be parsed");
  return parser.finish();
}

} // namespace

DatasetSimulationReport scanDatasetSimulationReport(const std::filesystem::path& path,
                                                     const DatasetReportObserver& observe) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Could not open the BIM simulation report");
  return scanDatasetSimulationReport(file, observe);
}

DatasetSimulationReport scanDatasetSimulationReport(std::istream& input,
                                                     const DatasetReportObserver& observe) {
  return scanReport(input, observe, false);
}

uint32_t scanSimulationBaseHistory(std::istream& input,
                                   const std::function<void(const nlohmann::json&)>& observe) {
  return scanReport(input, [&](DatasetReportStream, const Json& row) {
    if (observe) observe(row);
  }, true).ticksPerSecond;
}

} // namespace container::robotics
