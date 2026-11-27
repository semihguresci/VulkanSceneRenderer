#include "Container/robotics/CollisionSensorCamera.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {

using namespace container::robotics;

SensorCameraSettings testCamera() {
  return {.width = 2, .height = 2, .fx = 2, .fy = 2, .cx = .5, .cy = 1,
          .nearMeters = .5, .farMeters = 4};
}

void expectVector(glm::dvec3 actual, glm::dvec3 expected) {
  EXPECT_NEAR(actual.x, expected.x, 1e-12);
  EXPECT_NEAR(actual.y, expected.y, 1e-12);
  EXPECT_NEAR(actual.z, expected.z, 1e-12);
}

TEST(CollisionSensorCamera, UsesPixelCentersAndReturnsAxialDepthAtOffAxisHits) {
  const auto settings = testCamera();
  const std::array directions{glm::dvec3(0, .25, -1), glm::dvec3(.5, .25, -1),
                              glm::dvec3(0, -.25, -1), glm::dvec3(.5, -.25, -1)};
  const std::array entities{"wall", "glass", "uncolored", "wall"};
  std::size_t index = 0;
  const auto image = captureCollisionSensorCamera(settings, {},
      [&](glm::dvec3 origin, glm::dvec3 displacement) -> std::optional<LidarHit> {
        const auto direction = directions.at(index);
        expectVector(origin, direction * .5);
        expectVector(displacement, direction * 3.5);
        return LidarHit{(-2.0 - origin.z) / displacement.z, entities.at(index++)};
      }, [](std::string_view entity) -> std::optional<std::array<uint8_t, 3>> {
        if (entity == "wall") return std::array<uint8_t, 3>{11, 22, 33};
        if (entity == "glass") return std::array<uint8_t, 3>{44, 55, 66};
        return std::nullopt;
      });
  EXPECT_EQ(index, 4u);
  EXPECT_EQ(image.depth32fc1, (std::vector<float>{2, 2, 2, 2}));
  EXPECT_EQ(image.rgb8, (std::vector<uint8_t>{11, 22, 33, 44, 55, 66, 180, 180, 180, 11, 22, 33}));
}

TEST(CollisionSensorCamera, FollowsMountedCameraAxesAndTranslationInRep103) {
  auto settings = testCamera();
  settings.width = settings.height = 1;
  settings.cx = 0;
  settings.nearMeters = 1;
  settings.farMeters = 3;
  settings.baseFromCamera.position = {.2, -.1, .4};
  const SensorCameraPose base{{10, 20, 30}, glm::angleAxis(std::numbers::pi / 2, glm::dvec3(0, 0, 1))};
  const auto frame = makeSensorCameraFrame(settings, SensorSchedule(60, 1000), 0, base);
  const auto image = captureCollisionSensorCamera(settings, frame,
      [](glm::dvec3 origin, glm::dvec3 displacement) -> std::optional<LidarHit> {
        expectVector(origin, {10.35, 21.2, 30.65});
        expectVector(displacement, {.5, 2, .5});
        return LidarHit{.25, "wall"};
      });
  EXPECT_EQ(image.depth32fc1, (std::vector<float>{1.5f}));
}

TEST(CollisionSensorCamera, ClipsToAxialPlanesAndLeavesMissesBlackAndNaN) {
  auto settings = testCamera();
  settings.width = 4;
  settings.height = 1;
  settings.nearMeters = 1;
  settings.farMeters = 3;
  const std::array planeDepths{.5, 1.0, 3.0, 3.5};
  std::size_t index = 0;
  const auto image = captureCollisionSensorCamera(settings, {},
      [&](glm::dvec3 origin, glm::dvec3 displacement) -> std::optional<LidarHit> {
        EXPECT_EQ(origin.z, -1.0);
        EXPECT_EQ(origin.z + displacement.z, -3.0);
        const auto fraction = (-planeDepths.at(index++) - origin.z) / displacement.z;
        if (fraction < 0 || fraction > 1) return std::nullopt;
        return LidarHit{fraction, "uncolored"};
      });
  ASSERT_EQ(image.depth32fc1.size(), 4u);
  EXPECT_TRUE(std::isnan(image.depth32fc1[0]));
  EXPECT_EQ(image.depth32fc1[1], 1.0f);
  EXPECT_EQ(image.depth32fc1[2], 3.0f);
  EXPECT_TRUE(std::isnan(image.depth32fc1[3]));
  EXPECT_EQ(image.rgb8, (std::vector<uint8_t>{0, 0, 0, 180, 180, 180, 180, 180, 180, 0, 0, 0}));
}

TEST(CollisionSensorCamera, RejectsUnsupportedCalibrationAndMalformedHitFractions) {
  auto settings = testCamera();
  settings.width = 0;
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);
  settings.width = settings.height = 256;
  EXPECT_NO_THROW(validateCollisionSensorCameraSettings(settings));
  settings.height = 257;
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);

  settings = testCamera();
  settings.distortion = SensorCameraDistortion{};
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);
  settings = testCamera();
  settings.depthDropoutProbability = .1;
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);
  settings = testCamera();
  settings.rgbNoiseStdDevLinear = .1;
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);
  settings = testCamera();
  settings.appearance = SensorAppearanceSettings{};
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);

  settings = testCamera();
  settings.farMeters = settings.nearMeters;
  EXPECT_THROW(validateCollisionSensorCameraSettings(settings), std::invalid_argument);
  settings = testCamera();
  EXPECT_THROW((void)captureCollisionSensorCamera(settings, {}, {}), std::invalid_argument);
  const LidarCast miss = [](glm::dvec3, glm::dvec3) { return std::nullopt; };
  SensorCameraFrame invalidFrame;
  invalidFrame.worldFromCamera[0][0] = 2;
  EXPECT_THROW((void)captureCollisionSensorCamera(settings, invalidFrame, miss), std::invalid_argument);
  for (const auto fraction : {-.01, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
    const LidarCast invalidHit = [fraction](glm::dvec3, glm::dvec3) { return LidarHit{fraction, "wall"}; };
    EXPECT_THROW((void)captureCollisionSensorCamera(settings, {}, invalidHit), std::invalid_argument);
  }
}

} // namespace
