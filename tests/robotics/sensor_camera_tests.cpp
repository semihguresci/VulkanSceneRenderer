#include "Container/robotics/SensorCamera.h"

#include <gtest/gtest.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using namespace container::robotics;
constexpr double pi = std::numbers::pi;

SensorCameraSettings calibration() {
  return {.width = 640, .height = 480, .fx = 400, .fy = 350, .cx = 240.5, .cy = 170.5,
          .nearMeters = .1, .farMeters = 50};
}

void expectVector(glm::dvec3 actual, glm::dvec3 expected) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual[axis], expected[axis], 1e-12);
}

glm::dvec3 ndc(const glm::dmat4& projection, glm::dvec3 position) {
  const auto clip = projection * glm::dvec4(position, 1);
  return glm::dvec3(clip) / clip.w;
}

glm::dvec2 pixel(const SensorCameraSettings& settings, glm::dvec3 position) {
  return {(1 + position.x) * settings.width / 2, (1 - position.y) * settings.height / 2};
}

std::string spec(std::string_view extra = {}) {
  std::string result = R"({"schema":"container.sensor-camera","version":1,
    "intrinsics":{"width":640,"height":480,"fx":400,"fy":350,"cx":240.5,"cy":170.5,
    "nearMeters":0.1,"farMeters":50})";
  result += extra;
  result += '}';
  return result;
}

std::string trajectory(bool badTick = false, bool badSeconds = false) {
  std::ostringstream json;
  json << std::setprecision(std::numeric_limits<double>::max_digits10);
  json << R"({"schema":"container.bim-robotics-simulation","version":1,
    "coordinateConvention":"ROS REP-103","ticksPerSecond":12,
    "lidar":{"scans":[{"returns":[{},{}]}]},"history":[)";
  for (uint32_t tick = 0; tick <= 10; ++tick) {
    if (tick != 0) json << ',';
    json << R"({"tick":)" << (badTick && tick == 2 ? 3 : tick)
         << R"(,"seconds":)" << (badSeconds && tick == 2 ? .5 : double(tick) / 12)
         << R"(,"base":{"position":[)" << tick << ',' << -static_cast<int>(tick) << ",1],\"yaw\":"
         << (tick == 4 ? pi / 2 : 0.0) << "},\"odometry\":{\"position\":[90,90,90],\"yaw\":0}}";
  }
  json << "]}";
  return json.str();
}

template<class Action>
void expectInvalid(Action action, std::string_view expectedKey) {
  try {
    action();
    ADD_FAILURE() << "Expected invalid_argument for " << expectedKey;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string_view(error.what()).find(expectedKey), std::string_view::npos) << error.what();
  }
}

TEST(SensorCamera, AsymmetricProjectionMatchesPixelEdgesAndMetricReverseZ) {
  const auto settings = calibration();
  constexpr double metresPerRendererUnit = .25;
  const auto projection = sensorCameraProjection(settings, metresPerRendererUnit);
  const auto center = pixel(settings, ndc(projection, {0, 0, -8}));
  EXPECT_NEAR(center.x, 240.5, 1e-12);
  EXPECT_NEAR(center.y, 170.5, 1e-12);
  const auto sample = ndc(projection, {2.5, -2, -8});
  const auto projected = pixel(settings, sample);
  EXPECT_NEAR(projected.x, 365.5, 1e-12);
  EXPECT_NEAR(projected.y, 258.0, 1e-12);
  const double expectedDepth = settings.nearMeters * (settings.farMeters / 2 - 1) /
                               (settings.farMeters - settings.nearMeters);
  EXPECT_NEAR(sample.z, expectedDepth, 1e-12);
  EXPECT_NEAR(ndc(projection, {0, 0, -settings.nearMeters / metresPerRendererUnit}).z, 1.0, 1e-12);
  EXPECT_NEAR(ndc(projection, {0, 0, -settings.farMeters / metresPerRendererUnit}).z, 0.0, 1e-12);
  expectVector(sample, ndc(sensorCameraProjection(settings), {.625, -.5, -2}));

  // Vulkan's positive-height framebuffer viewport uses the opposite Y
  // convention. The flip includes the asymmetric projection's Y offset.
  auto flipped = projection;
  for (glm::length_t col = 0; col < 4; ++col) flipped[col][1] *= -1;
  const auto vulkan = ndc(flipped, {2.5, -2, -8});
  EXPECT_NEAR((1 + vulkan.y) * settings.height / 2, projected.y, 1e-12);
  EXPECT_NEAR(vulkan.z, sample.z, 1e-12);
}

TEST(SensorCamera, LatchesDueTickAndComposesDefaultCameraAxesRelativeToRobot) {
  auto settings = calibration();
  settings.baseFromCamera.position = {2, 3, 4};
  const SensorCameraPose base{{10, 20, 30}, 12.0 * glm::angleAxis(pi / 2, glm::dvec3(0, 0, 1))};
  const SensorSchedule schedule(60, 7'000, 2);
  expectInvalid([&] { (void)makeSensorCameraFrame(settings, schedule, 10, base); }, "frame.tick");
  const auto frame = makeSensorCameraFrame(settings, schedule, 11, base);
  EXPECT_EQ(frame.tick, 11u);
  EXPECT_DOUBLE_EQ(frame.seconds, 11.0 / 60);
  EXPECT_EQ(frame.sampleIndex, 1u);
  expectVector(glm::dvec3(frame.worldFromCamera[3]), {7, 22, 34});
  expectVector(glm::dvec3(frame.worldFromCamera[0]), {1, 0, 0});
  expectVector(glm::dvec3(frame.worldFromCamera[1]), {0, 0, 1});
  expectVector(glm::dvec3(frame.worldFromCamera[2]), {0, -1, 0});
  EXPECT_NO_THROW(validateSensorCameraFrame(frame));
}

TEST(SensorCamera, StaticSpecUsesOneFrameAndPreservesMountAndSensorErrorFields) {
  const auto defaultPlayback = parseSensorCameraPlayback(spec());
  ASSERT_EQ(defaultPlayback.frames.size(), 1u);
  EXPECT_EQ(defaultPlayback.frames.front().tick, 0u);
  EXPECT_DOUBLE_EQ(defaultPlayback.frames.front().seconds, 0.0);
  expectVector(glm::dvec3(-defaultPlayback.frames.front().worldFromCamera[2]), {1, 0, 0});

  const auto playback = parseSensorCameraPlayback(spec(R"(,
    "timing":{"firstTick":5},
    "baseFromCamera":{"xyz":[1,0,2],"rpy":[0,0,1.5707963267948966]},
    "worldFromBase":{"xyz":[3,4,5],"rpy":[0,0,1.5707963267948966]},
    "distortion":{"k1":0.1,"p2":-0.005,"k3":0.02},
    "depthNoise":{"stdDevMeters":0.025,"seed":18446744073709551615})"));
  ASSERT_EQ(playback.frames.size(), 1u);
  EXPECT_EQ(playback.frames.front().tick, 5u);
  EXPECT_EQ(playback.frames.front().sampleIndex, 0u);
  EXPECT_DOUBLE_EQ(playback.frames.front().seconds, 5.0 / 60);
  const auto& pose = playback.frames.front().worldFromCamera;
  expectVector(glm::dvec3(pose[3]), {3, 5, 7});
  expectVector(glm::dvec3(-pose[2]), {-1, 0, 0});
  expectVector(glm::dvec3(pose[1]), {0, 0, 1});
  ASSERT_TRUE(playback.settings.distortion);
  EXPECT_DOUBLE_EQ(playback.settings.distortion->k1, .1);
  EXPECT_DOUBLE_EQ(playback.settings.distortion->k2, 0);
  EXPECT_DOUBLE_EQ(playback.settings.distortion->p2, -.005);
  EXPECT_DOUBLE_EQ(playback.settings.distortion->k3, .02);
  EXPECT_DOUBLE_EQ(playback.settings.depthNoiseStdDevMeters, .025);
  EXPECT_EQ(playback.settings.depthNoiseSeed, std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(sensorCameraProjection(playback.settings), sensorCameraProjection(defaultPlayback.settings));
}

TEST(SensorCamera, PlaybackInheritsTrajectoryFrequencyAndUsesCommittedGroundTruth) {
  const auto report = trajectory();
  const auto playback = parseSensorCameraPlayback(spec(R"(,
      "timing":{"framesPerSecondMilliHz":5000,"firstTick":1,"lastTick":8},
      "baseFromCamera":{"xyz":[1,0,2]})"), std::string_view(report));
  EXPECT_EQ(playback.schedule.ticksPerSecond(), 12u);
  ASSERT_EQ(playback.frames.size(), 3u);
  EXPECT_EQ(playback.frames[0].tick, 1u);
  EXPECT_EQ(playback.frames[1].tick, 4u);
  EXPECT_EQ(playback.frames[2].tick, 6u);
  for (size_t sample = 0; sample < playback.frames.size(); ++sample) {
    EXPECT_EQ(playback.frames[sample].sampleIndex, sample);
    EXPECT_DOUBLE_EQ(playback.frames[sample].seconds, double(playback.frames[sample].tick) / 12);
  }
  expectVector(glm::dvec3(playback.frames[0].worldFromCamera[3]), {2, -1, 3});
  expectVector(glm::dvec3(playback.frames[1].worldFromCamera[3]), {4, -3, 3});
  expectVector(glm::dvec3(-playback.frames[1].worldFromCamera[2]), {0, 1, 0});
  expectVector(glm::dvec3(playback.frames[2].worldFromCamera[3]), {7, -6, 3});
  const auto throughEnd = parseSensorCameraPlayback(spec(R"(,"timing":{"framesPerSecondMilliHz":12000})"),
                                                     std::string_view(report));
  ASSERT_EQ(throughEnd.frames.size(), 11u);
  EXPECT_EQ(throughEnd.frames.back().tick, 10u);
}

TEST(SensorCamera, RejectsInvalidCalibrationRangesNonRigidPosesAndUnboundedPlayback) {
  auto settings = calibration();
  settings.width = 8192;
  settings.height = 8192;
  expectInvalid([&] { validateSensorCameraSettings(settings); }, "intrinsics.width * height");
  settings = calibration();
  settings.fx = 0.0;
  expectInvalid([&] { (void)sensorCameraProjection(settings); }, "intrinsics.fx");
  settings = calibration();
  settings.farMeters = settings.nearMeters;
  expectInvalid([&] { validateSensorCameraSettings(settings); }, "intrinsics.farMeters");
  expectInvalid([&] { (void)sensorCameraProjection(calibration(), 0); }, "metersPerRendererUnit");
  expectInvalid([&] {
    (void)sensorCameraProjection(calibration(), std::numeric_limits<double>::min());
  }, "metersPerRendererUnit");

  SensorCameraFrame frame;
  frame.worldFromCamera[0][0] = -1;
  expectInvalid([&] { validateSensorCameraFrame(frame); }, "frame.worldFromCamera");
  frame.worldFromCamera[0][0] = 2;
  expectInvalid([&] { validateSensorCameraFrame(frame); }, "frame.worldFromCamera");
  const SensorCameraPose zeroQuaternion{{0, 0, 0}, {0, 0, 0, 0}};
  expectInvalid([&] {
    (void)makeSensorCameraFrame(calibration(), SensorSchedule(60, 1000), 0, zeroQuaternion);
  }, "worldFromBase.rotation");

  expectInvalid([&] { (void)parseSensorCameraPlayback(spec(R"(,"depthNoise":{"stdDevMeters":-1})")); },
                "depthNoise.stdDevMeters");
  expectInvalid([&] { (void)parseSensorCameraPlayback(spec(R"(,"baseFromCamera":{"xyz":[0,1]})")); },
                "baseFromCamera.xyz");
  expectInvalid([&] { (void)parseSensorCameraPlayback(spec(R"(,"distortion":{"radial":3})")); },
                "distortion.radial");
  expectInvalid([&] { (void)parseSensorCameraPlayback(spec(R"(,"timing":{"framesPerSecondMilliHz":60001})")); },
                "timing.framesPerSecondMilliHz");
  expectInvalid([&] {
    (void)parseSensorCameraPlayback(spec(R"(,"timing":{"framesPerSecondMilliHz":60000,"lastTick":1024})"));
  }, "timing.lastTick");
}

TEST(SensorCamera, RejectsGapsMismatchedTimesAndIncompatibleTrajectoryTiming) {
  const auto good = trajectory();
  const auto missingTick = trajectory(true);
  const auto badSeconds = trajectory(false, true);
  expectInvalid([&] { (void)parseSensorCameraPlayback(spec(), std::string_view(missingTick)); },
                "trajectory.history[2].tick");
  expectInvalid([&] { (void)parseSensorCameraPlayback(spec(), std::string_view(badSeconds)); },
                "trajectory.history[2].seconds");
  expectInvalid([&] {
    (void)parseSensorCameraPlayback(spec(R"(,"timing":{"ticksPerSecond":60})"), std::string_view(good));
  }, "timing.ticksPerSecond");
  expectInvalid([&] {
    (void)parseSensorCameraPlayback(spec(R"(,"timing":{"lastTick":11})"), std::string_view(good));
  }, "timing.lastTick");
  expectInvalid([&] {
    (void)parseSensorCameraPlayback(spec(R"(,"worldFromBase":{"xyz":[1,2,3]})"), std::string_view(good));
  }, "worldFromBase");
}

} // namespace
