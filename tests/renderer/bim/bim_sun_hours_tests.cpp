#include "Container/renderer/bim/BimSunHours.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace container::renderer {
namespace {

BimSunHoursSettings Grid() {
  BimSunHoursSettings settings;
  settings.solar.month = 3;
  settings.solar.day = 20;
  settings.minX = -1.5;
  settings.minZ = -1.5;
  settings.maxX = 7.5;
  settings.maxZ = 7.5;
  settings.columns = 3;
  settings.rows = 3;
  settings.startMinute = 530;
  settings.endMinute = 550;
  settings.stepMinutes = 20;
  return settings;
}

std::array<BimSunTriangle, 2> Cover(double minX, double maxX, double minZ, double maxZ,
                                  double y) {
  const glm::dvec3 a{minX, y, minZ};
  const glm::dvec3 b{maxX, y, minZ};
  const glm::dvec3 c{maxX, y, maxZ};
  const glm::dvec3 d{minX, y, maxZ};
  return {BimSunTriangle{{a, b, c}}, BimSunTriangle{{a, d, c}}};
}

TEST(BimSunHours, MapsCellCentersInRowMajorWorldCoordinates) {
  auto settings = Grid();
  settings.elevation = 2.0;
  EXPECT_EQ(BimSunHoursCellCenter(settings, 0, 0), (glm::dvec3{0, 2, 0}));
  EXPECT_EQ(BimSunHoursCellCenter(settings, 2, 1), (glm::dvec3{6, 2, 3}));
  EXPECT_EQ(BimSunHoursCellCenter(settings, 7), (glm::dvec3{3, 2, 6}));
  EXPECT_THROW((void)BimSunHoursCellCenter(settings, 9), std::out_of_range);
}

TEST(BimSunHours, WeightsMidpointsAndTheFinalPartialInterval) {
  auto settings = Grid();
  settings.startMinute = 523;
  settings.endMinute = 570;
  settings.stepMinutes = 20;
  const auto result = RunBimSunHours(settings, {});
  ASSERT_EQ(result.hours.size(), 9u);
  EXPECT_EQ(result.settings.startMinute, 523u);
  EXPECT_EQ(result.settings.endMinute, 570u);
  EXPECT_EQ(result.settings.stepMinutes, 20u);
  EXPECT_EQ(result.sampleCount, 3u);
  EXPECT_EQ(result.daylightSampleCount, 3u);
  EXPECT_DOUBLE_EQ(result.daylightHours, 47.0 / 60.0);
  for (const double hours : result.hours) {
    EXPECT_DOUBLE_EQ(hours, 47.0 / 60.0);
  }
}

TEST(BimSunHours, UsesMidpointAboveHorizonToWeightStraddlingIntervals) {
  auto settings = Grid();
  settings.startMinute = 350;
  settings.endMinute = 377;
  settings.stepMinutes = 10;
  // The equatorial sunrise is about 06:07 local at longitude zero today.
  // Midpoints at 05:55 and 06:05 are dark. Only the last interval contributes.
  const auto result = RunBimSunHours(settings, {});
  EXPECT_EQ(result.sampleCount, 3u);
  EXPECT_EQ(result.daylightSampleCount, 1u);
  EXPECT_DOUBLE_EQ(result.daylightHours, 7.0 / 60.0);
  EXPECT_DOUBLE_EQ(result.hours[0], 7.0 / 60.0);
}

TEST(BimSunHours, ExcludesNightIncludingTheLastHalfMinuteOfTheDay) {
  auto settings = Grid();
  settings.solar.year = 2099;
  settings.solar.month = 12;
  settings.solar.day = 31;
  settings.startMinute = 1418;
  settings.endMinute = 1440;
  settings.stepMinutes = 21;
  const auto result = RunBimSunHours(settings, {});
  EXPECT_EQ(result.sampleCount, 2u);
  EXPECT_EQ(result.daylightSampleCount, 0u);
  EXPECT_DOUBLE_EQ(result.daylightHours, 0.0);
  EXPECT_DOUBLE_EQ(result.hours[0], 0.0);
}

TEST(BimSunHours, BlocksTwoSidedCoverAndIgnoresTheReceiverSurface) {
  const auto settings = Grid();
  const auto underCover = RunBimSunHours(settings, Cover(-100, 100, -100, 100, 2));
  const auto onReceiver = RunBimSunHours(settings, Cover(-100, 100, -100, 100, 0));
  for (size_t i = 0; i < underCover.hours.size(); ++i) {
    EXPECT_DOUBLE_EQ(underCover.hours[i], 0.0);
    EXPECT_DOUBLE_EQ(onReceiver.hours[i], underCover.daylightHours);
  }
}

TEST(BimSunHours, RotatesKnownMorningShadowWithModelNorth) {
  auto settings = Grid();
  // Rotating the morning sun by +90° moves the same cover's shadow north,
  // into the next row.
  const auto geometry = Cover(2.5, 4.5, 2.5, 5.5, 1.0);
  const auto east = RunBimSunHours(settings, geometry);
  settings.northDegrees = 90.0;
  const auto south = RunBimSunHours(settings, geometry);
  ASSERT_EQ(east.hours.size(), 9u);
  ASSERT_EQ(south.hours.size(), 9u);
  EXPECT_DOUBLE_EQ(east.hours[4], 0.0); // Grid center catches the east blocker.
  EXPECT_DOUBLE_EQ(east.hours[7], east.daylightHours);
  EXPECT_DOUBLE_EQ(south.hours[4], south.daylightHours);
  EXPECT_DOUBLE_EQ(south.hours[7], 0.0);
}

TEST(BimSunHours, RecognizesCloseCoverOnADistantSiteAndCountsInvalidTriangles) {
  auto settings = Grid();
  constexpr double shift = 1e9;
  settings.minX += shift;
  settings.maxX += shift;
  settings.minZ -= shift;
  settings.maxZ -= shift;
  settings.elevation = shift;
  const auto cover = Cover(shift - 100, shift + 100, -shift - 100, -shift + 100, shift + 1e-6);
  std::vector<BimSunTriangle> geometry(cover.begin(), cover.end());
  geometry.resize(12, cover[0]);
  geometry.push_back({});
  geometry.push_back(cover[0]);
  geometry.back().vertices[0].y = std::numeric_limits<double>::infinity();
  const auto result = RunBimSunHours(settings, geometry);
  EXPECT_EQ(result.skippedTriangles, 2u);
  ASSERT_EQ(result.hours.size(), 9u);
  for (const double hours : result.hours) {
    EXPECT_DOUBLE_EQ(hours, 0.0);
  }
}

TEST(BimSunHours, RejectsInvalidSettingsAndBoundsWorkload) {
  auto settings = Grid();
  settings.solar.day = 32;
  EXPECT_THROW(ValidateBimSunHoursSettings(settings), std::invalid_argument);
  settings = Grid();
  settings.maxZ = settings.minZ;
  EXPECT_THROW(ValidateBimSunHoursSettings(settings), std::invalid_argument);
  settings = Grid();
  settings.startMinute = 1430;
  settings.endMinute = 25;
  EXPECT_THROW(ValidateBimSunHoursSettings(settings), std::invalid_argument);
  settings = Grid();
  settings.columns = 129;
  EXPECT_THROW(ValidateBimSunHoursSettings(settings), std::length_error);
  settings = Grid();
  settings.columns = 128;
  settings.rows = 128;
  settings.startMinute = 0;
  settings.endMinute = 1440;
  settings.stepMinutes = 1;
  EXPECT_THROW(ValidateBimSunHoursSettings(settings), std::length_error);
  const std::vector<BimSunTriangle> excessiveGeometry(kMaxBimSunHoursTriangles + 1);
  EXPECT_THROW((void)RunBimSunHours(Grid(), excessiveGeometry), std::length_error);
}

TEST(BimSunHours, DiscardsCancelledResults) {
  const auto settings = Grid();
  std::stop_source stop;
  stop.request_stop();
  const auto result = RunBimSunHours(settings, Cover(-100, 100, -100, 100, 2), stop.get_token());
  EXPECT_TRUE(result.cancelled);
  EXPECT_TRUE(result.hours.empty());
  EXPECT_EQ(result.sampleCount, 0u);
  EXPECT_EQ(result.settings.startMinute, settings.startMinute);
}

} // namespace
} // namespace container::renderer
