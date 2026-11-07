#include "Container/asl/PoseReplay.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <nlohmann/json.hpp>

namespace container::asl {
namespace {
using Json = nlohmann::json;
void require(bool condition, const char *message) {
  if (!condition) throw std::invalid_argument(message);
}
bool text(const Json &value, bool nullable = false) {
  if (nullable && value.is_null()) return true;
  if (!value.is_string()) return false;
  const auto &str = value.get_ref<const std::string &>();
  return !str.empty() && str.size() <= 16384 &&
      std::ranges::none_of(str, [](unsigned char c) { return c < 32; });
}
uint64_t integer(const Json &value) {
  require(value.is_number_integer(), "Replay timestamps and counts must be integers");
  if (value.is_number_unsigned()) {
    const auto result = value.get<uint64_t>();
    require(result <= 9007199254740991ull, "Replay integer exceeds exact JSON range");
    return result;
  }
  const auto result = value.get<int64_t>();
  require(result >= 0 && result <= 9007199254740991ll, "Replay integer outside supported range");
  return static_cast<uint64_t>(result);
}
bool exactString(const Json &object, const char *key, std::string_view value) {
  return object.contains(key) && object.at(key).is_string() &&
      object.at(key).get_ref<const std::string &>() == value;
}
} // namespace

glm::vec3 imagePlanePosition(const glm::vec3 &point) {
  // Image +Y down -> the renderer's world +Y up. This is observation-space
  // conversion only; projection and Vulkan's negative viewport stay unchanged.
  return {2.0f * (point.x - 0.5f), 2.0f * (0.5f - point.y), 0.0f};
}

PoseReplay PoseReplay::load(const std::filesystem::path &path) {
  const auto bytes = std::filesystem::file_size(path);
  require(bytes > 0 && bytes <= kMaxReplayBytes, "Replay file must be nonempty and at most 64 MiB");
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("Cannot open ASL pose replay");
  std::string contents(static_cast<size_t>(bytes), '\0');
  stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  require(stream.gcount() == static_cast<std::streamsize>(contents.size()) && stream.peek() == EOF,
          "Replay file changed during reading");
  return PoseReplay(Json::parse(contents, [](int depth, Json::parse_event_t, Json &) {
    require(depth <= 32, "Replay JSON nesting exceeds 32 levels");
    return true;
  }));
}

PoseReplay::PoseReplay(const Json &doc) {
  require(doc.is_object(), "Replay root must be an object");
  require(exactString(doc, "schema", "asl-pose-replay-v1") && doc.at("schema_version") == 1,
          "Unsupported ASL pose replay schema");
  require(integer(doc.at("landmark_count")) == kLandmarkCount, "Replay must have 553 landmarks");
  const auto &space = doc.at("coordinate_space");
  require(space.is_object() && exactString(space, "x", "normalized_image") &&
          exactString(space, "y", "normalized_image") && exactString(space, "z", "MediaPipe_model_relative") &&
          exactString(space, "display", "image_xy_plane_z_retained") &&
          space.at("metric_depth").is_boolean() && !space.at("metric_depth").get<bool>(),
          "Only explicitly nonmetric image XY replay is supported");
  const auto &groups = doc.at("groups");
  require(groups.is_array() && groups.size() == 4, "Replay must declare four canonical groups");
  const std::array<const char *, 4> names{"left_hand", "right_hand", "pose", "face"};
  const std::array<size_t, 4> offsets{0, 21, 42, 75}, counts{21, 21, 33, 478};
  for (size_t i = 0; i < 4; ++i)
    require(exactString(groups[i], "name", names[i]) && integer(groups[i].at("offset")) == offsets[i] &&
            integer(groups[i].at("count")) == counts[i], "Replay group offsets do not match canonical layout");
  const auto &source = doc.at("source");
  require(source.is_object() && text(source.at("sample_id")) && text(source.at("dataset")) &&
          text(source.at("signer_id"), true) && text(source.at("label"), true) &&
          text(source.at("manifest")) && text(source.at("input_type")), "Replay source provenance is incomplete");
  const auto &split = source.at("split");
  require(split == "train" || split == "validation" || split == "test" || split == "unassigned",
          "Unsupported source split");
  const auto &parents = source.at("parent_sample_ids");
  require(parents.is_array() && parents.size() <= 32, "Replay parent IDs are invalid");
  std::vector<std::string> parentIds;
  for (const auto &parent : parents) {
    require(text(parent), "Invalid parent sample ID");
    const auto id = parent.get<std::string>();
    require(std::ranges::find(parentIds, id) == parentIds.end(), "Duplicate parent sample ID");
    parentIds.push_back(id);
  }
  require(doc.at("timing").is_object() && doc.at("timing").dump().size() <= 16384,
          "Source timing metadata is required and bounded to 16 KiB");
  sampleId_ = source.at("sample_id").get<std::string>();
  label_ = source.at("label").is_null() ? sampleId_ : source.at("label").get<std::string>();
  const auto &frames = doc.at("frames");
  require(frames.is_array() && !frames.empty() && frames.size() <= kMaxFrames &&
          integer(doc.at("frame_count")) == frames.size(), "Replay frame count is invalid or exceeds 2048");
  frames_.reserve(frames.size());
  for (const auto &frame : frames) {
    PoseFrame parsed;
    parsed.timestampMs = integer(frame.at("timestamp_ms"));
    require(frames_.empty() || parsed.timestampMs > frames_.back().timestampMs,
            "Replay timestamps must increase strictly");
    const auto &points = frame.at("points");
    const auto &valid = frame.at("valid");
    require(points.is_array() && points.size() == kLandmarkCount && valid.is_array() && valid.size() == kLandmarkCount,
            "Replay point and mask arrays must have 553 entries");
    for (size_t i = 0; i < kLandmarkCount; ++i) {
      require(valid[i].is_boolean(), "Replay validity masks must be boolean");
      parsed.points[i].valid = valid[i].get<bool>();
      if (!parsed.points[i].valid) {
        require(points[i].is_null(), "Invalid replay observations must be null");
        continue;
      }
      require(points[i].is_array() && points[i].size() == 3, "Valid replay points must have three coordinates");
      for (size_t c = 0; c < 3; ++c) {
        require(points[i][c].is_number(), "Replay coordinates must be numbers");
        const double value = points[i][c].get<double>();
        require(std::isfinite(value) && std::abs(value) <= 10000.0, "Replay coordinate outside finite display bound");
        parsed.points[i].xyz[static_cast<glm::length_t>(c)] = static_cast<float>(value);
      }
    }
    frames_.push_back(std::move(parsed));
  }
}

double PoseReplay::durationMs() const {
  // The final observation is held for the last source interval before looping.
  // This display-only end hold does not create or change source timestamps.
  const auto tail = frames_.size() > 1 ? frames_.back().timestampMs - frames_[frames_.size()-2].timestampMs : 33ull;
  return static_cast<double>(frames_.back().timestampMs - frames_.front().timestampMs + tail);
}
size_t PoseReplay::frameIndexAt(double elapsedMs, bool loop) const {
  require(std::isfinite(elapsedMs) && elapsedMs >= 0, "Replay time must be finite and nonnegative");
  const double relative = loop ? std::fmod(elapsedMs, durationMs()) : elapsedMs;
  const auto origin = frames_.front().timestampMs;
  const auto it = std::upper_bound(frames_.begin(), frames_.end(), relative,
      [origin](double at, const PoseFrame &frame) {
        return at < static_cast<double>(frame.timestampMs - origin);
      });
  return it == frames_.begin() ? 0 : static_cast<size_t>(std::distance(frames_.begin(), it) - 1);
}
const PoseFrame &PoseReplay::frameAt(double elapsedMs, bool loop) const {
  return frames_[frameIndexAt(elapsedMs, loop)];
}

HandFitOptions replayHandFitOptions() {
  HandFitOptions options;
  options.maxAngularVelocityRadPerSecond = 12.0;
  return options;
}

HandPoseReplay::HandPoseReplay(const PoseReplay &replay, double imageAspectRatio,
                             HandFitOptions options)
    : aspectRatio_(imageAspectRatio), options_(std::move(options)), frames_(replay.frames()) {
  require(std::isfinite(aspectRatio_) && aspectRatio_ >= .125 && aspectRatio_ <= 8,
          "Hand image aspect ratio must be in [0.125,8]");
  std::array<std::vector<HandObservation>, 2> observations;
  for (auto &hand : observations) hand.reserve(frames_.size());
  for (const auto &frame : replay.frames()) {
    for (size_t hand = 0; hand < 2; ++hand) {
      HandObservation observation;
      observation.timestampMs = static_cast<int64_t>(frame.timestampMs);
      for (size_t joint = 0; joint < HandLandmarkCount; ++joint) {
        const auto &point = frame.points[hand * HandLandmarkCount + joint];
        observation.observed[joint] = point.valid;
        if (point.valid) {
          // MediaPipe hand-relative Z follows image-width units; Y follows
          // image-height units. Rotate Y/Z together into a RH +Y-up fit space.
          observation.positions[joint] = {aspectRatio_ * point.xyz.x, -point.xyz.y,
                                          -aspectRatio_ * point.xyz.z};
        }
      }
      observations[hand].push_back(std::move(observation));
    }
  }
  calibrations_[0] = calibrateHand(observations[0], Handedness::Left);
  calibrations_[1] = calibrateHand(observations[1], Handedness::Right);
  fits_.resize(frames_.size());
  for (size_t frame = 0; frame < frames_.size(); ++frame) {
    for (size_t hand = 0; hand < 2; ++hand) {
      // Retain an invalid prior receipt as well: the core resets its history
      // and explicitly reports a reacquisition after lost tracking.
      const auto *previous = frame > 0 ? &fits_[frame - 1][hand] : nullptr;
      auto &fit = fits_[frame][hand];
      fit = fitHand(calibrations_[hand], observations[hand][frame], options_, previous);
      // Raw observations remain authoritative. A failed fit is an explicitly
      // reported raw fallback, and an inferred joint never becomes observed.
      for (size_t joint = 0; joint < HandLandmarkCount; ++joint) {
        require(fit.observed[joint] == observations[hand][frame].observed[joint],
                "Hand fitter changed the observation mask");
        if (!fit.valid || !fit.observed[joint]) continue;
        const auto &position = fit.positions[joint];
        require(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z),
                "Hand fitter produced a nonfinite position");
        frames_[frame].points[hand * HandLandmarkCount + joint].xyz = {
            position.x / aspectRatio_, -position.y, -position.z / aspectRatio_};
      }
    }
  }
}

Json HandPoseReplay::fitDocument() const {
  const auto vec = [](const glm::dvec3 &value) { return Json::array({value.x, value.y, value.z}); };
  Json calibrations = Json::object();
  constexpr std::array names{"left", "right"};
  for (size_t hand = 0; hand < 2; ++hand) {
    const auto &calibration = calibrations_[hand];
    Json offsets = Json::array();
    for (const auto &offset : calibration.palmOffsets) offsets.push_back(vec(offset));
    calibrations[names[hand]] = {
        {"valid", calibration.valid}, {"handedness", names[hand]}, {"palm_offsets", offsets},
        {"bone_lengths", calibration.boneLengths}, {"palm_frame_count", calibration.palmFrameCount},
        {"bone_observation_counts", calibration.boneObservationCounts}, {"message", calibration.message}};
  }
  Json limits = Json::array();
  for (const auto &limit : options_.limits) limits.push_back({limit.minimum, limit.maximum});
  Json document = {
      {"schema", "asl-hand-kinematics-fit-v1"}, {"schema_version", 1}, {"frame_count", frames_.size()},
      {"method", "experimental-hand-fk-ik-v2"},
      {"coordinate_space", {{"input", "normalized_image_xy_mediapipe_relative_z"},
          {"solver", "x=aspect*x,y=-y,z=-aspect*z"},
          {"output", "normalized_image_xy_mediapipe_relative_z"},
          {"pose", "solver"}, {"aspect_ratio", aspectRatio_},
          {"aspect_assumption", aspectRatio_ == 1.0 ? "square_normalized_assumed" : "user_supplied_width_over_height"},
          {"metric_depth", false}}},
      {"options", {{"max_iterations", options_.maxIterations},
          {"max_angular_velocity_rad_per_second", options_.maxAngularVelocityRadPerSecond},
          {"max_temporal_gap_ms", options_.maxTemporalGapMs},
          {"estimate_scale_from_palm", options_.estimateScaleFromPalm},
          {"minimum_scale", options_.minimumScale}, {"maximum_scale", options_.maximumScale},
          {"convergence_tolerance", options_.convergenceTolerance}, {"damping", options_.damping}}},
      {"angle_names", handAngleNames()}, {"angle_limits_radians", limits},
      {"calibration", std::move(calibrations)}, {"frames", Json::array()}};
  size_t validFits = 0, missingFits = 0, velocityLimited = 0, temporalResets = 0, constrainedCount = 0;
  double maxResidual = 0, maxBoneLengthError = 0;
  for (size_t frame = 0; frame < frames_.size(); ++frame) {
    Json hands = Json::object();
    for (size_t hand = 0; hand < 2; ++hand) {
      const auto &fit = fits_[frame][hand];
      const auto &d = fit.diagnostics;
      Json positions = nullptr, wrist = nullptr, basis = nullptr, scale = nullptr, angles = nullptr;
      std::array<bool, HandLandmarkCount> constrained{}, inferred{};
      if (fit.valid) {
        ++validFits;
        positions = Json::array();
        for (size_t joint = 0; joint < HandLandmarkCount; ++joint) {
          const auto &point = fit.positions[joint];
          positions.push_back(Json::array({point.x / aspectRatio_, -point.y, -point.z / aspectRatio_}));
          constrained[joint] = fit.observed[joint];
          inferred[joint] = fit.inferred[joint];
          constrainedCount += constrained[joint] ? 1 : 0;
        }
        wrist = vec(fit.pose.wrist);
        basis = Json::array({vec(fit.pose.basis[0]), vec(fit.pose.basis[1]), vec(fit.pose.basis[2])});
        scale = fit.pose.scale;
        angles = fit.pose.angles;
        maxResidual = std::max(maxResidual, d.maxResidual);
        maxBoneLengthError = std::max(maxBoneLengthError, d.maxBoneLengthError);
      } else {
        ++missingFits;
      }
      velocityLimited += d.velocityLimited ? 1 : 0;
      temporalResets += d.temporalReset ? 1 : 0;
      hands[names[hand]] = {{"valid", fit.valid}, {"positions", positions},
          {"observed", fit.observed}, {"inferred", inferred}, {"constrained", constrained},
          {"wrist", wrist}, {"basis", basis}, {"scale", scale}, {"angles_radians", angles},
          {"diagnostics", {{"observed_count", d.observedCount},
              {"observed_articulated_count", d.observedArticulatedCount}, {"iterations", d.iterations},
              {"angles_at_limit", d.anglesAtLimit}, {"rms_residual", d.rmsResidual},
              {"max_residual", d.maxResidual}, {"max_bone_length_error", d.maxBoneLengthError},
              {"objective", d.objective}, {"velocity_limited", d.velocityLimited},
              {"temporal_reset", d.temporalReset}, {"scale_clamped", d.scaleClamped},
              {"message", d.message}}}};
    }
    document["frames"].push_back({{"timestamp_ms", frames_[frame].timestampMs}, {"hands", std::move(hands)}});
  }
  document["summary"] = {{"frame_count", frames_.size()}, {"hand_fit_count", validFits},
      {"raw_fallback_hand_count", missingFits}, {"constrained_observation_count", constrainedCount},
      {"velocity_limited_hand_count", velocityLimited}, {"temporal_reset_hand_count", temporalResets},
      {"max_residual", maxResidual}, {"max_bone_length_error", maxBoneLengthError}};
  return document;
}

void ReplayClock::advance(double seconds) {
  require(std::isfinite(seconds) && seconds >= 0 && seconds <= 60, "Replay timestep must be finite and in [0,60] seconds");
  if (!paused_) elapsedMs_ += seconds * speed_ * 1000;
}
void ReplayClock::seek(double milliseconds) {
  require(std::isfinite(milliseconds) && milliseconds >= 0 && milliseconds <= 1e12, "Replay seek outside display range");
  elapsedMs_ = milliseconds;
}
void ReplayClock::setSpeed(double speed) {
  require(std::isfinite(speed) && speed >= 0 && speed <= 16, "Replay speed must be in [0,16]");
  speed_ = speed;
}
} // namespace container::asl
