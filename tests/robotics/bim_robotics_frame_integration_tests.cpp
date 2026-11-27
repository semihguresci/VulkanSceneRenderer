#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/BimCollisionWorld.h"
#include "Container/robotics/RaycastLidar.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/robotics/UrdfRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <numbers>
#include <string_view>

namespace {

using namespace container::robotics;
constexpr double pi = std::numbers::pi;
constexpr uint32_t tickRate = 12;
constexpr std::string_view xWallGuid = "0000000000000000000027";
constexpr std::string_view yWallGuid = "0000000000000000000037";

struct Witness {
  uint64_t tick;
  glm::dvec3 sensor;
  glm::dvec3 hit;
  double range;
  std::string_view wallGuid;
};

// Analytic REP-103 witnesses from the authored joint origins and velocity limits.
// With the root yaw applied, the shoulder pivot is (.20, .60, 1.25) m.
// Shoulder angle advances 1.5 rad/s toward 1.5; elbow advances 2 rad/s
// toward .25. The forearm visual tip adds .45 m along the link's +X.
// Intersect the mounted +X unit ray with the first IFC wall it reaches,
// at REP-103 X=4 m or Y=3 m. Each hit lies within the finite wall face.
const std::array<Witness, 6> witnesses{{
    {1, {1.216666986344, .747906702778, 1.423267582868},
        {4., 1.097646792743, 2.189577404006}, 2.908004734764, xWallGuid},
    {4, {1.077884576705, 1.102380408596, 1.458223023836},
        {4., 2.698739339953, 2.673670368628}, 3.544634344011, xWallGuid},
    {6, {.926302369059, 1.303954738358, 1.458223023836},
        {2.746881702029, 3., 2.366482479328}, 2.648775920244, yWallGuid},
    {9, {.617990607842, 1.521059855120, 1.458223023836},
        {1.324747964965, 3., 2.056554714523}, 1.744927140618, yWallGuid},
    {11, {.377122437230, 1.595838866076, 1.458223023836},
         {.655620004621, 3., 1.980766048833}, 1.523903080264, yWallGuid},
    {13, {.251584523910, 1.610151692495, 1.458223023836},
         {.350145400241, 3., 1.966831330187}, 1.483264970768, yWallGuid},
}};

void expectPoint(const glm::dvec3& actual, const glm::dvec3& expected, double tolerance) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual[axis], expected[axis], tolerance) << "axis " << axis;
}

TEST(BimRoboticsFrames, ScaledNativeIfcAndArticulatedLidarAgreeInMetricRep103) {
  const auto fixtures = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES);
  const auto native = container::geometry::ifc::LoadFromFile(fixtures / "lidar-corner.ifc", 2.5f);
  const auto scene = buildRoboticsScene(native, {.sourceNamespace = "frame-golden"});
  ASSERT_TRUE(scene.frame.sourceMetersPerUnit && scene.frame.metersPerRendererUnit &&
              scene.frame.roboticsFromRenderer);
  EXPECT_DOUBLE_EQ(*scene.frame.sourceMetersPerUnit, .001);
  EXPECT_NEAR(*scene.frame.metersPerRendererUnit, .4, 1e-7);
  const auto xWall = std::ranges::find(scene.entities, xWallGuid, &SemanticEntity::guid);
  const auto yWall = std::ranges::find(scene.entities, yWallGuid, &SemanticEntity::guid);
  ASSERT_NE(xWall, scene.entities.end());
  ASSERT_NE(yWall, scene.entities.end());
  ASSERT_TRUE(xWall->rendererWorldFromLocal && xWall->roboticsWorldFromLocal);

  // IFC product #27's origin is (-625, 4125, 0) in authored millimetres.
  // Render axes and import scale produce (-1.5625, 0, -10.3125) renderer units.
  expectPoint(glm::dvec3((*xWall->rendererWorldFromLocal)[3]), {-1.5625, 0., -10.3125}, 1e-6);
  expectPoint(glm::dvec3((*xWall->roboticsWorldFromLocal)[3]), {4.125, .625, 0.}, 1e-10);
  expectPoint(glm::dvec3(*scene.frame.roboticsFromRenderer * glm::dvec4(0., 2.5, -10., 1.)),
              {4., 0., 1.}, 1e-6);

  BimCollisionWorld world(cookBimCollisionGeometry(native, scene), {}, 1);
  const auto cast = world.lidarRaycasts();
  UrdfRuntime arm(loadUrdf(fixtures / "two-link-arm.urdf"),
                  {{.4, .7, 1.}, glm::angleAxis(-pi / 2, glm::dvec3(0, 0, 1))}, tickRate);
  const auto forearm = std::ranges::find(arm.model().links, std::string_view("forearm"), &UrdfLink::name);
  ASSERT_NE(forearm, arm.model().links.end());
  const auto socket = std::ranges::find(forearm->visuals, std::string_view("tip_marker"), &UrdfVisual::name);
  ASSERT_NE(socket, forearm->visuals.end());
  const auto forearmIndex = static_cast<size_t>(forearm - arm.model().links.begin());
  const auto socketIndex = static_cast<size_t>(socket - forearm->visuals.begin());
  const std::array commands{UrdfJointValue{"shoulder", 1.5}, UrdfJointValue{"elbow", .25}};
  arm.setTargets(commands);

  const RaycastLidar lidar({.azimuthSamples = 1, .azimuthStart = 0., .maxRangeMeters = 8.,
      .baseFromSensor = {.position = {0., .10, .03},
                         .rotation = glm::angleAxis(.10, glm::dvec3(0, 0, 1))}});
  // 12 Hz / 5 Hz is fractional. A whole-tick rounding drift would change
  // the joint angles and return geometry at the listed sample ticks.
  const LidarSchedule schedule(tickRate, 5000, 1);
  EXPECT_FALSE(schedule.due(arm.clock().tick()));
  size_t seen = 0;
  for (uint64_t frame = 1; frame <= witnesses.back().tick; ++frame) {
    arm.step();
    const auto tick = arm.clock().tick();
    ASSERT_EQ(tick, frame);
    if (!schedule.due(tick)) continue;
    ASSERT_LT(seen, witnesses.size());
    const auto& witness = witnesses[seen++];
    SCOPED_TRACE(tick);
    const auto worldFromSocket = arm.worldFromVisual(forearmIndex, socketIndex);
    const auto scan = lidar.scan({glm::dvec3(worldFromSocket[3]), glm::quat_cast(glm::dmat3(worldFromSocket))},
                                 schedule, tick, cast);
    EXPECT_EQ(scan.tick, witness.tick);
    EXPECT_DOUBLE_EQ(scan.seconds, static_cast<double>(witness.tick) / tickRate);
    expectPoint(scan.worldFromSensor.position, witness.sensor, 1e-10);
    ASSERT_EQ(scan.returns.size(), 1u);
    const auto& hit = scan.returns.front();
    ASSERT_EQ(hit.status, LidarReturnStatus::Valid);
    ASSERT_TRUE(hit.rangeMeters);
    EXPECT_EQ(hit.entityId, witness.wallGuid == xWallGuid ? xWall->id : yWall->id);
    EXPECT_NEAR(*hit.rangeMeters, witness.range, 1e-5);
    const auto forward = scan.worldFromSensor.rotation * glm::dvec3(1, 0, 0);
    expectPoint(scan.worldFromSensor.position + forward * *hit.rangeMeters, witness.hit, 1e-5);
  }
  EXPECT_EQ(seen, witnesses.size());
}

} // namespace
