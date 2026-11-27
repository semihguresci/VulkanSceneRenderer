#include "Container/robotics/SensorCamera.h"
#include "Container/utility/ContentFingerprint.h"
#include "DatasetSimulationReport.h"

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace container::robotics {
namespace {

using Json = nlohmann::json;
constexpr size_t maxSpecBytes = 64 * 1024;
constexpr size_t maxTrajectoryBytes = 128 * 1024 * 1024;
constexpr uint32_t maxImageExtent = 8192;
constexpr uint64_t maxImagePixels = 8'388'608;
constexpr uint64_t maxPlaybackTick = 8191;
constexpr uint64_t maxPlaybackFrames = 1024;

[[noreturn]] void invalid(std::string_view key, std::string_view message) {
  throw std::invalid_argument("Sensor camera " + std::string(key) + " " + std::string(message));
}

void requireFinite(double value, std::string_view key) {
  if (!std::isfinite(value)) invalid(key, "must be finite");
}

bool fitsFloat(double value) {
  return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

bool positiveFloat(double value) {
  return fitsFloat(value) && value > 0 && static_cast<float>(value) > 0;
}

void requireFinite(glm::dvec3 value, std::string_view key) {
  for (glm::length_t i = 0; i < 3; ++i) requireFinite(value[i], key);
}

glm::dquat normalizedRotation(glm::dquat value, std::string_view key) {
  requireFinite({value.x, value.y, value.z}, key);
  requireFinite(value.w, key);
  const double maxComponent = std::max({std::abs(value.w), std::abs(value.x),
                                        std::abs(value.y), std::abs(value.z)});
  if (maxComponent == 0.0) invalid(key, "must be a nonzero quaternion");
  return glm::normalize(value / maxComponent);
}

glm::dquat fromRpy(glm::dvec3 rpy) {
  constexpr double turn = 2 * std::numbers::pi;
  return glm::angleAxis(std::remainder(rpy.z, turn), glm::dvec3(0, 0, 1)) *
         glm::angleAxis(std::remainder(rpy.y, turn), glm::dvec3(0, 1, 0)) *
         glm::angleAxis(std::remainder(rpy.x, turn), glm::dvec3(1, 0, 0));
}

std::string field(std::string_view context, std::string_view key) {
  return std::string(context) + "." + std::string(key);
}

void checkKeys(const Json& value, std::string_view context, std::initializer_list<std::string_view> keys) {
  if (!value.is_object()) invalid(context, "must be an object");
  for (auto entry = value.begin(); entry != value.end(); ++entry)
    if (std::ranges::find(keys, std::string_view(entry.key())) == keys.end())
      invalid(field(context, entry.key()), "is an unknown key");
}

const Json& required(const Json& value, std::string_view context, const char* key) {
  if (!value.is_object()) invalid(context, "must be an object");
  const auto found = value.find(key);
  if (found == value.end()) invalid(field(context, key), "is required");
  return *found;
}

double number(const Json& value, std::string_view key) {
  if (!value.is_number()) invalid(key, "must be a number");
  const double parsed = value.get<double>();
  requireFinite(parsed, key);
  return parsed;
}

double number(const Json& value, std::string_view context, const char* key) {
  return number(required(value, context, key), field(context, key));
}

double numberOr(const Json& value, std::string_view context, const char* key, double fallback = 0.0) {
  const auto found = value.find(key);
  return found == value.end() ? fallback : number(*found, field(context, key));
}

uint64_t natural(const Json& value, std::string_view key, uint64_t maximum) {
  uint64_t parsed{};
  if (value.is_number_unsigned()) {
    parsed = value.get<uint64_t>();
  } else if (value.is_number_integer() && value.get<int64_t>() >= 0) {
    parsed = static_cast<uint64_t>(value.get<int64_t>());
  } else {
    invalid(key, "must be a nonnegative integer");
  }
  if (parsed > maximum) invalid(key, "exceeds " + std::to_string(maximum));
  return parsed;
}

uint64_t naturalOr(const Json& value, std::string_view context, const char* key,
                   uint64_t fallback, uint64_t maximum) {
  const auto found = value.find(key);
  return found == value.end() ? fallback : natural(*found, field(context, key), maximum);
}

glm::dvec3 vector(const Json& value, std::string_view key) {
  if (!value.is_array() || value.size() != 3) invalid(key, "must be a three-number array");
  return {number(value[0], key), number(value[1], key), number(value[2], key)};
}

SensorCameraPose pose(const Json& value, std::string_view context, SensorCameraPose fallback = {}) {
  checkKeys(value, context, {"xyz", "rpy"});
  if (const auto xyz = value.find("xyz"); xyz != value.end())
    fallback.position = vector(*xyz, field(context, "xyz"));
  if (const auto rpy = value.find("rpy"); rpy != value.end())
    fallback.rotation = fromRpy(vector(*rpy, field(context, "rpy"))) * fallback.rotation;
  return fallback;
}

void requireSchema(const Json& value, std::string_view context, std::string_view schema) {
  const auto& authored = required(value, context, "schema");
  if (!authored.is_string() || authored.get_ref<const std::string&>() != schema)
    invalid(field(context, "schema"), "must be " + std::string(schema));
  if (natural(required(value, context, "version"), field(context, "version"), 1) != 1)
    invalid(field(context, "version"), "must be 1");
}

Json parseJson(std::string_view input, std::string_view context, size_t limit) {
  if (input.size() > limit) invalid(context, "exceeds the JSON byte limit");
  try {
    if (context == "trajectory") {
      // Simulation reports may contain large sensor arrays. Parse the full
      // JSON syntax, but only retain the top-level fields needed for playback.
      return Json::parse(input, [](int depth, Json::parse_event_t event, const Json& value) {
        return event != Json::parse_event_t::key || depth != 1 || value == "schema" ||
               value == "version" || value == "coordinateConvention" || value == "ticksPerSecond" ||
               value == "history";
      });
    }
    return Json::parse(input);
  } catch (const Json::exception& error) {
    invalid(context, error.what());
  }
}

struct Trajectory {
  uint32_t ticksPerSecond{};
  std::vector<SensorCameraPose> bases;
};

SensorCameraPose readTrajectoryPose(const Json& sample, std::string_view context) {
  const auto& base = required(sample, context, "base");
  const auto baseContext = field(context, "base");
  const auto position = vector(required(base, baseContext, "position"), field(baseContext, "position"));
  const double yaw = number(base, baseContext, "yaw");
  return {position, fromRpy({0, 0, yaw})};
}

std::string trajectoryRow(size_t tick) {
  return "trajectory.history[" + std::to_string(tick) + "]";
}

void validateTrajectoryTimestamp(double stamp, size_t tick, uint32_t frequency) {
  const double expected = static_cast<double>(tick) / frequency;
  if (std::abs(stamp - expected) > 1e-12 * std::max(1.0, expected))
    invalid(field(trajectoryRow(tick), "seconds"), "does not match tick / ticksPerSecond");
}

Trajectory readTrajectory(std::string_view input) {
  const auto json = parseJson(input, "trajectory", maxTrajectoryBytes);
  requireSchema(json, "trajectory", "container.bim-robotics-simulation");
  if (required(json, "trajectory", "coordinateConvention") != "ROS REP-103")
    invalid("trajectory.coordinateConvention", "must be ROS REP-103");
  const auto frequency = static_cast<uint32_t>(natural(required(json, "trajectory", "ticksPerSecond"),
                                                       "trajectory.ticksPerSecond", 1'000'000));
  if (frequency == 0) invalid("trajectory.ticksPerSecond", "must be positive");
  const auto& history = required(json, "trajectory", "history");
  if (!history.is_array() || history.empty() || history.size() > maxPlaybackTick + 1)
    invalid("trajectory.history", "must contain 1..8192 committed ticks");

  Trajectory result{.ticksPerSecond = frequency};
  result.bases.reserve(history.size());
  for (size_t tick = 0; tick < history.size(); ++tick) {
    const auto context = trajectoryRow(tick);
    const auto& sample = history[tick];
    if (natural(required(sample, context, "tick"), field(context, "tick"), maxPlaybackTick) != tick)
      invalid(field(context, "tick"), "must be contiguous from zero");
    validateTrajectoryTimestamp(number(sample, context, "seconds"), tick, frequency);
    result.bases.push_back(readTrajectoryPose(sample, context));
  }
  return result;
}

SensorCameraSettings readSettings(const Json& spec) {
  const auto& intrinsics = required(spec, "spec", "intrinsics");
  checkKeys(intrinsics, "intrinsics", {"width", "height", "fx", "fy", "cx", "cy", "nearMeters", "farMeters"});
  SensorCameraSettings result{
      .width = static_cast<uint32_t>(natural(required(intrinsics, "intrinsics", "width"),
                                             "intrinsics.width", maxImageExtent)),
      .height = static_cast<uint32_t>(natural(required(intrinsics, "intrinsics", "height"),
                                              "intrinsics.height", maxImageExtent)),
      .fx = number(intrinsics, "intrinsics", "fx"), .fy = number(intrinsics, "intrinsics", "fy"),
      .cx = number(intrinsics, "intrinsics", "cx"), .cy = number(intrinsics, "intrinsics", "cy"),
      .nearMeters = number(intrinsics, "intrinsics", "nearMeters"),
      .farMeters = number(intrinsics, "intrinsics", "farMeters")};
  if (const auto mount = spec.find("baseFromCamera"); mount != spec.end())
    result.baseFromCamera = pose(*mount, "baseFromCamera", result.baseFromCamera);
  if (const auto distortion = spec.find("distortion"); distortion != spec.end()) {
    checkKeys(*distortion, "distortion", {"k1", "k2", "p1", "p2", "k3"});
    result.distortion = SensorCameraDistortion{
        numberOr(*distortion, "distortion", "k1"), numberOr(*distortion, "distortion", "k2"),
        numberOr(*distortion, "distortion", "p1"), numberOr(*distortion, "distortion", "p2"),
        numberOr(*distortion, "distortion", "k3")};
  }
  if (const auto noise = spec.find("depthNoise"); noise != spec.end()) {
    checkKeys(*noise, "depthNoise", {"stdDevMeters", "seed"});
    result.depthNoiseStdDevMeters = numberOr(*noise, "depthNoise", "stdDevMeters");
    result.depthNoiseSeed = naturalOr(*noise, "depthNoise", "seed", 0, std::numeric_limits<uint64_t>::max());
  }
  return result;
}

std::string readText(const std::filesystem::path& path, std::string_view context, size_t limit) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) invalid(context, "file could not be opened");
  const auto length = file.tellg();
  if (length < 0 || length > static_cast<std::streamoff>(limit))
    invalid(context, "file size is unreadable or exceeds the JSON byte limit");
  std::string result(static_cast<size_t>(length), '\0');
  file.seekg(0);
  if (!file.read(result.data(), static_cast<std::streamsize>(result.size())))
    invalid(context, "file could not be read");
  return result;
}

} // namespace

void validateSensorCameraSettings(const SensorCameraSettings& settings) {
  if (settings.width == 0 || settings.width > maxImageExtent)
    invalid("intrinsics.width", "must be in [1, 8192]");
  if (settings.height == 0 || settings.height > maxImageExtent)
    invalid("intrinsics.height", "must be in [1, 8192]");
  if (uint64_t(settings.width) * settings.height > maxImagePixels)
    invalid("intrinsics.width * height", "exceeds the 8388608 pixel limit");
  if (!positiveFloat(2.0 * (settings.fx / settings.width)))
    invalid("intrinsics.fx", "must produce a positive finite projection scale");
  if (!positiveFloat(2.0 * (settings.fy / settings.height)))
    invalid("intrinsics.fy", "must produce a positive finite projection scale");
  if (!fitsFloat(2.0 * (settings.cx / settings.width) - 1.0))
    invalid("intrinsics.cx", "must produce a finite projection offset");
  if (!fitsFloat(2.0 * (settings.cy / settings.height) - 1.0))
    invalid("intrinsics.cy", "must produce a finite projection offset");
  if (!positiveFloat(settings.nearMeters)) invalid("intrinsics.nearMeters", "must be positive and fit float depth");
  if (!positiveFloat(settings.farMeters) || static_cast<float>(settings.farMeters) <= static_cast<float>(settings.nearMeters))
    invalid("intrinsics.farMeters", "must exceed nearMeters and fit float depth");
  requireFinite(settings.baseFromCamera.position, "baseFromCamera.xyz");
  (void)normalizedRotation(settings.baseFromCamera.rotation, "baseFromCamera.rotation");
  if (const auto& distortion = settings.distortion) {
    requireFinite(distortion->k1, "distortion.k1");
    requireFinite(distortion->k2, "distortion.k2");
    requireFinite(distortion->p1, "distortion.p1");
    requireFinite(distortion->p2, "distortion.p2");
    requireFinite(distortion->k3, "distortion.k3");
  }
  requireFinite(settings.depthNoiseStdDevMeters, "depthNoise.stdDevMeters");
  if (settings.depthNoiseStdDevMeters < 0) invalid("depthNoise.stdDevMeters", "must be nonnegative");
}

void validateSensorCameraFrame(const SensorCameraFrame& frame) {
  requireFinite(frame.seconds, "frame.seconds");
  if (frame.seconds < 0.0) invalid("frame.seconds", "must be nonnegative");
  const auto& world = frame.worldFromCamera;
  for (glm::length_t i = 0; i < 4; ++i) {
    requireFinite(glm::dvec3(world[i]), "frame.worldFromCamera");
    requireFinite(world[i][3], "frame.worldFromCamera");
  }
  constexpr double tolerance = 1e-8;
  if (std::abs(world[0][3]) > tolerance || std::abs(world[1][3]) > tolerance ||
      std::abs(world[2][3]) > tolerance || std::abs(world[3][3] - 1.0) > tolerance)
    invalid("frame.worldFromCamera", "must be affine");
  const glm::dvec3 x(world[0]), y(world[1]), z(world[2]);
  if (std::abs(glm::dot(x, x) - 1.0) > tolerance || std::abs(glm::dot(y, y) - 1.0) > tolerance ||
      std::abs(glm::dot(z, z) - 1.0) > tolerance || std::abs(glm::dot(x, y)) > tolerance ||
      std::abs(glm::dot(y, z)) > tolerance || std::abs(glm::dot(z, x)) > tolerance ||
      std::abs(glm::dot(glm::cross(x, y), z) - 1.0) > tolerance)
    invalid("frame.worldFromCamera", "must be a right-handed rigid pose");
}

glm::dmat4 sensorCameraProjection(const SensorCameraSettings& settings, double metersPerRendererUnit) {
  validateSensorCameraSettings(settings);
  requireFinite(metersPerRendererUnit, "metersPerRendererUnit");
  if (metersPerRendererUnit <= 0.0) invalid("metersPerRendererUnit", "must be positive");
  const double nearPlane = settings.nearMeters / metersPerRendererUnit;
  const double farPlane = settings.farMeters / metersPerRendererUnit;
  const double a = settings.nearMeters / (settings.farMeters - settings.nearMeters);
  const double b = nearPlane * (1.0 + a);
  if (!positiveFloat(nearPlane) || !positiveFloat(farPlane) ||
      static_cast<float>(nearPlane) >= static_cast<float>(farPlane) || !positiveFloat(a) || !positiveFloat(b))
    invalid("intrinsics.nearMeters/farMeters at metersPerRendererUnit", "cannot form a finite renderer projection");

  glm::dmat4 projection(0.0);
  projection[0][0] = 2.0 * (settings.fx / settings.width);
  projection[1][1] = 2.0 * (settings.fy / settings.height);
  projection[2][0] = 1.0 - 2.0 * (settings.cx / settings.width);
  projection[2][1] = 2.0 * (settings.cy / settings.height) - 1.0;
  projection[2][2] = a;
  projection[2][3] = -1.0;
  projection[3][2] = b;
  return projection;
}

SensorCameraFrame makeSensorCameraFrame(const SensorCameraSettings& settings, const SensorSchedule& schedule,
                                        uint64_t tick, const SensorCameraPose& worldFromBase) {
  validateSensorCameraSettings(settings);
  if (!schedule.due(tick)) invalid("frame.tick", "does not belong to the camera schedule");
  requireFinite(worldFromBase.position, "worldFromBase.xyz");
  const auto base = normalizedRotation(worldFromBase.rotation, "worldFromBase.rotation");
  const auto mount = normalizedRotation(settings.baseFromCamera.rotation, "baseFromCamera.rotation");
  auto world = glm::mat4_cast(glm::normalize(base * mount));
  world[3] = glm::dvec4(worldFromBase.position + base * settings.baseFromCamera.position, 1.0);
  SensorCameraFrame result{tick, schedule.seconds(tick), schedule.sampleIndex(tick), world};
  validateSensorCameraFrame(result);
  return result;
}

namespace {

Json readSpec(std::string_view specJson) {
  auto spec = parseJson(specJson, "spec", maxSpecBytes);
  checkKeys(spec, "spec", {"schema", "version", "intrinsics", "timing", "baseFromCamera", "worldFromBase",
                            "distortion", "depthNoise"});
  requireSchema(spec, "spec", "container.sensor-camera");
  return spec;
}

SensorCameraPlayback makePlayback(const Json& spec, const Trajectory* trajectory) {
  auto settings = readSettings(spec);
  // Validate the optical clipping configuration even when no renderer is present.
  (void)sensorCameraProjection(settings);
  SensorCameraPose staticBase;
  const auto authoredBase = spec.find("worldFromBase");
  if (trajectory) {
    if (authoredBase != spec.end()) invalid("worldFromBase", "cannot override a supplied trajectory");
  } else if (authoredBase != spec.end()) {
    staticBase = pose(*authoredBase, "worldFromBase");
  }

  const auto timing = spec.value("timing", Json::object());
  checkKeys(timing, "timing", {"ticksPerSecond", "framesPerSecondMilliHz", "firstTick", "lastTick"});
  const auto frequency = static_cast<uint32_t>(naturalOr(timing, "timing", "ticksPerSecond",
      trajectory ? trajectory->ticksPerSecond : 60, 1'000'000));
  if (frequency == 0) invalid("timing.ticksPerSecond", "must be positive");
  if (trajectory && frequency != trajectory->ticksPerSecond)
    invalid("timing.ticksPerSecond", "does not match trajectory.ticksPerSecond");
  const auto maximumRate = uint64_t(frequency) * 1000;
  const auto rate = static_cast<uint32_t>(naturalOr(timing, "timing", "framesPerSecondMilliHz",
      std::min(uint64_t{10'000}, maximumRate), maximumRate));
  if (rate == 0) invalid("timing.framesPerSecondMilliHz", "must be positive");
  const auto firstTick = naturalOr(timing, "timing", "firstTick", 0, maxPlaybackTick);
  const auto lastTick = naturalOr(timing, "timing", "lastTick",
                               trajectory ? trajectory->bases.size() - 1 : firstTick, maxPlaybackTick);
  if (lastTick < firstTick) invalid("timing.lastTick", "precedes firstTick");
  if (trajectory && lastTick >= trajectory->bases.size())
    invalid("timing.lastTick", "exceeds the committed trajectory history");
  const SensorSchedule schedule(frequency, rate, firstTick);
  const auto frameCount = schedule.samplesThrough(lastTick);
  if (frameCount > maxPlaybackFrames) invalid("timing.lastTick", "would produce more than 1024 camera frames");
  SensorCameraPlayback playback{.settings = settings, .schedule = schedule};
  playback.frames.reserve(static_cast<size_t>(frameCount));
  for (auto tick = firstTick; tick <= lastTick; ++tick) {
    if (!schedule.due(tick)) continue;
    const auto& base = trajectory ? trajectory->bases[static_cast<size_t>(tick)] : staticBase;
    playback.frames.push_back(makeSensorCameraFrame(settings, schedule, tick, base));
  }
  return playback;
}

} // namespace

SensorCameraPlayback parseSensorCameraPlayback(std::string_view specJson, std::optional<std::string_view> trajectoryJson) {
  const auto spec = readSpec(specJson);
  if (!trajectoryJson) return makePlayback(spec, nullptr);
  const auto trajectory = readTrajectory(*trajectoryJson);
  return makePlayback(spec, &trajectory);
}

SensorCameraPlayback loadSensorCameraPlayback(const std::filesystem::path& specPath,
    const std::optional<std::filesystem::path>& trajectoryPath) {
  const auto spec = readSpec(readText(specPath, "spec", maxSpecBytes));
  if (!trajectoryPath) return makePlayback(spec, nullptr);
  Trajectory trajectory;
  std::vector<double> stamps;
  util::ContentFingerprint digest;
  try {
    digest = util::FingerprintFileWhileReading(*trajectoryPath, [&](std::istream& input) {
      trajectory.ticksPerSecond = scanSimulationBaseHistory(input, [&](const Json& row) {
        const auto context = trajectoryRow(trajectory.bases.size());
        stamps.push_back(number(row, context, "seconds"));
        trajectory.bases.push_back(readTrajectoryPose(row, context));
      });
    });
  } catch (const std::invalid_argument& error) {
    invalid("trajectory", error.what());
  }
  for (size_t tick = 0; tick < stamps.size(); ++tick)
    validateTrajectoryTimestamp(stamps[tick], tick, trajectory.ticksPerSecond);
  auto playback = makePlayback(spec, &trajectory);
  playback.simulationReportSha256 = std::move(digest.digest);
  return playback;
}

} // namespace container::robotics
