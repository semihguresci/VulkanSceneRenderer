#include "Container/asl/PoseReplay.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#include <limits>

namespace {
using nlohmann::json;
using container::asl::PoseReplay;
json document() {
  json points = json::array(), mask = json::array();
  for (size_t i = 0; i < 553; ++i) { points.push_back(nullptr); mask.push_back(false); }
  points[0] = {.2, .3, -.4}; mask[0] = true;
  return {{"schema", "asl-pose-replay-v1"}, {"schema_version", 1}, {"landmark_count", 553}, {"frame_count", 3},
      {"coordinate_space", {{"x", "normalized_image"}, {"y", "normalized_image"},
        {"z", "MediaPipe_model_relative"}, {"metric_depth", false}, {"display", "image_xy_plane_z_retained"}}},
      {"groups", {{{"name", "left_hand"}, {"offset", 0}, {"count", 21}},
                   {{"name", "right_hand"}, {"offset", 21}, {"count", 21}},
                   {{"name", "pose"}, {"offset", 42}, {"count", 33}},
                   {{"name", "face"}, {"offset", 75}, {"count", 478}}}},
      {"source", {{"dataset", "test"}, {"sample_id", "hello-1"}, {"label", "hello"}, {"signer_id", "signer:1"},
                   {"split", "test"}, {"parent_sample_ids", json::array()}, {"input_type", "native_recording"},
                   {"manifest", "test.jsonl"}}},
      {"timing", {{"estimated", false}, {"kind", "native_camera_timestamps"}}},
      {"frames", {{{"timestamp_ms", 500}, {"points", points}, {"valid", mask}},
                   {{"timestamp_ms", 540}, {"points", points}, {"valid", mask}},
                   {{"timestamp_ms", 640}, {"points", points}, {"valid", mask}}}}};
}

TEST(AslPoseReplay, PreservesSourceAndMissingObservations) {
  PoseReplay replay(document());
  ASSERT_EQ(replay.frames().size(), 3);
  EXPECT_EQ(replay.sampleId(), "hello-1");
  EXPECT_EQ(replay.label(), "hello");
  EXPECT_EQ(replay.frames()[0].timestampMs, 500);
  EXPECT_TRUE(replay.frames()[0].points[0].valid);
  EXPECT_FALSE(replay.frames()[0].points[1].valid);
  EXPECT_FLOAT_EQ(replay.frames()[0].points[0].xyz.z, -.4f);
}
TEST(AslPoseReplay, HoldsGapsLoopsAndClampsWithoutInterpolation) {
  PoseReplay replay(document());
  EXPECT_EQ(replay.frameIndexAt(0), 0);
  EXPECT_EQ(replay.frameIndexAt(39), 0);
  EXPECT_EQ(replay.frameIndexAt(40), 1);
  EXPECT_EQ(replay.frameIndexAt(139), 1);
  EXPECT_EQ(replay.frameIndexAt(140), 2);
  EXPECT_EQ(replay.frameIndexAt(239), 2);
  EXPECT_EQ(replay.frameIndexAt(240), 0);
  EXPECT_EQ(replay.frameIndexAt(100000, false), 2);
  EXPECT_DOUBLE_EQ(replay.durationMs(), 240);
  EXPECT_THROW(replay.frameAt(-1), std::invalid_argument);
  EXPECT_THROW(replay.frameAt(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}
TEST(AslPoseReplay, ConvertsImageYToWorldUpWithoutInventingDepth) {
  const auto p = container::asl::imagePlanePosition({.25f, .75f, 100.f});
  EXPECT_FLOAT_EQ(p.x, -.5f); EXPECT_FLOAT_EQ(p.y, -.5f); EXPECT_FLOAT_EQ(p.z, 0);
}
TEST(AslPoseReplay, SamplingUsesRelativeTimeEvenForLargeSourceEpochs) {
  auto doc = document();
  doc["frames"][0]["timestamp_ms"] = 9007199254740989ull;
  doc["frames"][1]["timestamp_ms"] = 9007199254740990ull;
  doc["frames"][2]["timestamp_ms"] = 9007199254740991ull;
  const PoseReplay replay(doc);
  EXPECT_EQ(replay.frameIndexAt(.9), 0);
  EXPECT_EQ(replay.frameIndexAt(1.0), 1);
  EXPECT_EQ(replay.frameIndexAt(1.9), 1);
  EXPECT_EQ(replay.frameIndexAt(2.0), 2);
}
TEST(AslPoseReplay, RejectsFalseMaskClaimsAndNonfiniteOrHugeCoordinates) {
  auto doc = document(); doc["frames"][0]["valid"][1] = true;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); doc["frames"][0]["points"][1] = {0,0,0};
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  for (const auto bad : {10001., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    doc = document(); doc["frames"][0]["points"][0][0] = bad;
    EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  }
  doc = document(); doc["frames"][0]["valid"][0] = 1;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
}
TEST(AslPoseReplay, RejectsMalformedTimesAndShapes) {
  for (const json time : {json(500), json(-1), json(1.2), json(true), json(9007199254740992ull)}) {
    auto doc = document(); doc["frames"][1]["timestamp_ms"] = time;
    EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  }
  auto doc = document(); doc["frames"][0]["points"].erase(0);
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); doc["frame_count"] = 4;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
}
TEST(AslPoseReplay, RejectsWrongCoordinateSchemaOrMissingProvenance) {
  auto doc = document(); doc["coordinate_space"]["metric_depth"] = true;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); doc["groups"][2]["offset"] = 21;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); doc["source"]["sample_id"] = "";
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); doc["source"]["parent_sample_ids"] = {"one", "one"};
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); doc["source"]["split"] = "whatever";
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
}
TEST(AslPoseReplay, ClockSharesDeterministicCaptureAndPauseSemantics) {
  container::asl::ReplayClock clock;
  clock.advance(.04); EXPECT_DOUBLE_EQ(clock.elapsedMs(), 40);
  clock.togglePause(); clock.advance(.1); EXPECT_DOUBLE_EQ(clock.elapsedMs(), 40);
  clock.restart(); EXPECT_DOUBLE_EQ(clock.elapsedMs(), 0);
  clock.togglePause(); clock.setSpeed(2); clock.advance(.02); EXPECT_DOUBLE_EQ(clock.elapsedMs(), 40);
  clock.seek(200); EXPECT_DOUBLE_EQ(clock.elapsedMs(), 200);
  EXPECT_THROW(clock.advance(-1), std::invalid_argument);
  EXPECT_THROW(clock.setSpeed(17), std::invalid_argument);
  EXPECT_THROW(clock.seek(-1), std::invalid_argument);
}
TEST(AslPoseReplay, RejectsEmptyAndExcessiveFrames) {
  auto doc = document(); doc["frames"] = json::array(); doc["frame_count"] = 0;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
  doc = document(); const auto frame = doc["frames"][0];
  doc["frames"] = json::array();
  for (size_t i = 0; i < 2049; ++i) doc["frames"].push_back(frame);
  doc["frame_count"] = 2049;
  EXPECT_THROW(PoseReplay{doc}, std::invalid_argument);
}
TEST(AslPoseReplay, LoadsBoundedJsonAndRejectsOversizeOrDeepFiles) {
  const auto name = "container-asl-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
  const auto path = std::filesystem::temp_directory_path() / name;
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
  { std::ofstream file(path); file << document().dump(); }
  EXPECT_EQ(PoseReplay::load(path).frames().size(), 3);
  { std::ofstream file(path, std::ios::trunc); file << ""; }
  EXPECT_THROW((void)PoseReplay::load(path), std::invalid_argument);
  { std::ofstream file(path, std::ios::trunc); file.seekp(container::asl::kMaxReplayBytes); file << ' '; }
  EXPECT_THROW((void)PoseReplay::load(path), std::invalid_argument);
  { std::ofstream file(path, std::ios::trunc); file << std::string(35, '[') << "0" << std::string(35, ']'); }
  EXPECT_THROW((void)PoseReplay::load(path), std::invalid_argument);
}

json articulatedReplayDocument(double aspect) {
  using namespace container::asl;
  HandCalibration calibration;
  calibration.valid = true;
  calibration.handedness = Handedness::Right;
  calibration.palmOffsets = {glm::dvec3(.55, .4, 0), {.3, 1, 0}, {.1, 1.1, 0}, {-.12, 1.05, 0}, {-.3, 1, 0}};
  for (size_t digit = 0; digit < 5; ++digit) calibration.boneLengths[digit] = {.4, .25, .2};
  auto doc = document();
  doc["frames"] = json::array();
  doc["frame_count"] = 4;
  constexpr std::array<uint64_t, 4> timestamps{500, 540, 900, 940};
  for (size_t frame = 0; frame < timestamps.size(); ++frame) {
    HandPose pose;
    pose.wrist = {.7, -.6, .02}; pose.scale = .1;
    pose.angles[5] = .1 + frame * .02;
    const auto source = forwardHand(calibration, pose);
    auto points = json::array(), mask = json::array();
    for (size_t i = 0; i < 553; ++i) { points.push_back(nullptr); mask.push_back(false); }
    for (size_t joint = 0; joint < 21; ++joint) {
      points[21 + joint] = {source[joint].x / aspect, -source[joint].y, -source[joint].z / aspect};
      mask[21 + joint] = true;
    }
    // Missing fingertip is inferred for the model but must remain invisible.
    if (frame == 1) { points[29] = nullptr; mask[29] = false; }
    // An absent wrist makes this whole hand a reported raw fallback.
    if (frame == 3) { points[21] = nullptr; mask[21] = false; }
    points[42] = {.3, .4, -.1}; mask[42] = true;
    points[75] = {.2, .1, -.3}; mask[75] = true;
    doc["frames"].push_back({{"timestamp_ms", timestamps[frame]}, {"points", points}, {"valid", mask}});
  }
  return doc;
}

TEST(AslPoseReplay, HandFitsPreserveRawBodyFaceMasksAndSourceTimestamps) {
  using namespace container::asl;
  const PoseReplay replay(articulatedReplayDocument(2.0));
  const HandPoseReplay fitted(replay, 2.0);
  ASSERT_EQ(fitted.frames().size(), 4);
  ASSERT_TRUE(fitted.fits()[0][1].valid) << fitted.fits()[0][1].diagnostics.message;
  ASSERT_TRUE(fitted.fits()[1][1].valid);
  ASSERT_TRUE(fitted.fits()[2][1].valid);
  EXPECT_TRUE(fitted.fits()[2][1].diagnostics.temporalReset);
  EXPECT_FALSE(fitted.fits()[3][1].valid);
  for (size_t frame = 0; frame < 4; ++frame) {
    EXPECT_EQ(fitted.frames()[frame].timestampMs, replay.frames()[frame].timestampMs);
    for (size_t joint = 0; joint < 553; ++joint) {
      EXPECT_EQ(fitted.frames()[frame].points[joint].valid, replay.frames()[frame].points[joint].valid);
      if (joint < 21 || joint >= 42 || frame == 3)
        EXPECT_EQ(fitted.frames()[frame].points[joint].xyz, replay.frames()[frame].points[joint].xyz);
    }
  }
  EXPECT_FALSE(fitted.frames()[1].points[29].valid);
  EXPECT_TRUE(fitted.fits()[1][1].inferred[8]);
  // Source storage is not mutated by fitting, playback or reporting.
  const auto original = articulatedReplayDocument(2.0);
  EXPECT_FLOAT_EQ(replay.frames()[0].points[22].xyz.x, original["frames"][0]["points"][22][0].get<float>());
}

TEST(AslPoseReplay, DerivedHandReportSeparatesFitSpaceAndObservationSpace) {
  using namespace container::asl;
  const PoseReplay replay(articulatedReplayDocument(2.0));
  const HandPoseReplay fitted(replay, 2.0);
  const auto report = fitted.fitDocument();
  EXPECT_EQ(report["schema"], "asl-hand-kinematics-fit-v1");
  EXPECT_EQ(report["frame_count"], 4);
  EXPECT_FALSE(report["coordinate_space"]["metric_depth"].get<bool>());
  EXPECT_EQ(report["coordinate_space"]["aspect_ratio"], 2.0);
  EXPECT_EQ(report["angle_names"].size(), 21);
  const auto &fit = report["frames"][0]["hands"]["right"];
  EXPECT_NEAR(fit["positions"][0][0].get<double>(), fit["wrist"][0].get<double>() / 2.0, 1e-12);
  EXPECT_NEAR(fit["positions"][0][1].get<double>(), -fit["wrist"][1].get<double>(), 1e-12);
  const auto &partial = report["frames"][1]["hands"]["right"];
  EXPECT_FALSE(partial["observed"][8].get<bool>());
  EXPECT_FALSE(partial["constrained"][8].get<bool>());
  EXPECT_TRUE(partial["inferred"][8].get<bool>());
  const auto &invalid = report["frames"][3]["hands"]["right"];
  EXPECT_FALSE(invalid["valid"].get<bool>());
  EXPECT_TRUE(invalid["positions"].is_null());
  EXPECT_TRUE(invalid["angles_radians"].is_null());
  EXPECT_EQ(report["summary"]["hand_fit_count"], 3);
  EXPECT_EQ(report["summary"]["raw_fallback_hand_count"], 5);
  EXPECT_LT(report["summary"]["max_bone_length_error"].get<double>(), 1e-12);
  EXPECT_EQ(report.dump(), HandPoseReplay(replay, 2.0).fitDocument().dump());
}

TEST(AslPoseReplay, HandAspectBoundsAndMissingCalibrationAreExplicit) {
  using namespace container::asl;
  const PoseReplay replay(document());
  for (const auto aspect : {0.0, .124, 8.1, std::numeric_limits<double>::quiet_NaN()})
    EXPECT_THROW((void)HandPoseReplay(replay, aspect), std::invalid_argument);
  const HandPoseReplay fitted(replay);
  const auto report = fitted.fitDocument();
  EXPECT_FALSE(fitted.calibrations()[0].valid);
  EXPECT_EQ(report["summary"]["hand_fit_count"], 0);
  EXPECT_EQ(report["summary"]["raw_fallback_hand_count"], 6);
  EXPECT_EQ(report["coordinate_space"]["aspect_assumption"], "square_normalized_assumed");
}

TEST(AslPoseReplay, ReacquiringMissingWristReportsTemporalHistoryReset) {
  using namespace container::asl;
  auto doc = articulatedReplayDocument(1.0);
  doc["frames"][2]["points"][21] = nullptr;
  doc["frames"][2]["valid"][21] = false;
  doc["frames"][3]["points"][21] = doc["frames"][0]["points"][21];
  doc["frames"][3]["valid"][21] = true;
  const HandPoseReplay fitted{PoseReplay(doc)};
  ASSERT_FALSE(fitted.fits()[2][1].valid);
  ASSERT_TRUE(fitted.fits()[3][1].valid);
  EXPECT_TRUE(fitted.fits()[3][1].diagnostics.temporalReset);
}
} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
