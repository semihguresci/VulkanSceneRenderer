#include "Container/robotics/UrdfModel.h"
#include "Container/robotics/UrdfRuntime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {

using namespace container::robotics;
using namespace std::chrono_literals;
constexpr double pi = std::numbers::pi;

UrdfModel armFixture() {
  return loadUrdf(std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "two-link-arm.urdf");
}

size_t linkIndex(const UrdfModel& model, std::string_view name) {
  const auto found = std::ranges::find(model.links, name, &UrdfLink::name);
  if (found == model.links.end()) throw std::runtime_error("Missing expected URDF link");
  return static_cast<size_t>(found - model.links.begin());
}

size_t jointIndex(const UrdfModel& model, std::string_view name) {
  const auto found = std::ranges::find(model.joints, name, &UrdfJoint::name);
  if (found == model.joints.end()) throw std::runtime_error("Missing expected URDF joint");
  return static_cast<size_t>(found - model.joints.begin());
}

void expectVector(glm::dvec3 actual, glm::dvec3 expected, double tolerance = 1e-12) {
  for (glm::length_t axis = 0; axis < 3; ++axis)
    EXPECT_NEAR(actual[axis], expected[axis], tolerance);
}

void expectJoint(const UrdfJointState& actual, const UrdfJointState& expected) {
  EXPECT_NEAR(actual.position, expected.position, 1e-12);
  EXPECT_NEAR(actual.velocity, expected.velocity, 1e-12);
  EXPECT_NEAR(actual.target, expected.target, 1e-12);
}

TEST(UrdfRuntime, NativeArmPreservesAuthoredSocketAndJointFrames) {
  UrdfRuntime runtime(armFixture());
  const auto& model = runtime.model();
  EXPECT_EQ(model.name, "two_link_arm");
  ASSERT_EQ(model.links.size(), 3u);
  ASSERT_EQ(model.joints.size(), 2u);
  const auto base = linkIndex(model, "base_link");
  const auto upper = linkIndex(model, "upper_arm");
  const auto forearm = linkIndex(model, "forearm");
  const auto shoulder = jointIndex(model, "shoulder");
  const auto elbow = jointIndex(model, "elbow");
  EXPECT_EQ(model.rootLink, base);
  EXPECT_EQ(model.jointOrder, (std::vector{shoulder, elbow}));
  EXPECT_EQ(model.joints[shoulder].parentLink, base);
  EXPECT_EQ(model.joints[shoulder].childLink, upper);
  EXPECT_EQ(model.joints[elbow].parentLink, upper);
  EXPECT_EQ(model.joints[elbow].childLink, forearm);
  ASSERT_TRUE(model.joints[shoulder].limits && model.joints[elbow].limits);
  EXPECT_EQ(model.joints[shoulder].type, UrdfJointType::Revolute);
  EXPECT_EQ(model.joints[elbow].type, UrdfJointType::Revolute);
  ASSERT_TRUE(model.joints[shoulder].limits->lower && model.joints[shoulder].limits->upper);
  ASSERT_TRUE(model.joints[elbow].limits->lower && model.joints[elbow].limits->upper);
  EXPECT_DOUBLE_EQ(*model.joints[shoulder].limits->lower, -pi / 2);
  EXPECT_DOUBLE_EQ(*model.joints[shoulder].limits->upper, pi / 2);
  EXPECT_DOUBLE_EQ(model.joints[shoulder].limits->velocity, 1.5);
  EXPECT_DOUBLE_EQ(model.joints[shoulder].limits->effort, 8.0);
  EXPECT_DOUBLE_EQ(*model.joints[elbow].limits->lower, -1.2);
  EXPECT_DOUBLE_EQ(*model.joints[elbow].limits->upper, 1.8);
  EXPECT_DOUBLE_EQ(model.joints[elbow].limits->velocity, 2.0);
  EXPECT_DOUBLE_EQ(model.joints[elbow].limits->effort, 3.0);

  ASSERT_EQ(model.links[base].collisions.size(), 1u);
  const auto* pedestal = std::get_if<UrdfCylinder>(&model.links[base].collisions[0].geometry);
  ASSERT_NE(pedestal, nullptr);
  EXPECT_DOUBLE_EQ(pedestal->radius, .16);
  EXPECT_DOUBLE_EQ(pedestal->length, .12);
  ASSERT_EQ(model.links[forearm].visuals.size(), 2u);
  ASSERT_EQ(model.links[forearm].collisions.size(), 1u);
  ASSERT_TRUE(model.links[forearm].inertial);
  EXPECT_EQ(model.links[forearm].visuals[1].name, "tip_marker");
  EXPECT_EQ(model.links[forearm].collisions[0].name, "forearm_collision");
  const auto* marker = std::get_if<UrdfSphere>(&model.links[forearm].visuals[1].geometry);
  const auto* box = std::get_if<UrdfBox>(&model.links[forearm].collisions[0].geometry);
  ASSERT_TRUE(marker && box);
  EXPECT_DOUBLE_EQ(marker->radius, .035);
  EXPECT_EQ(box->size, glm::dvec3(.45, .08, .08));
  const auto& inertial = *model.links[forearm].inertial;
  EXPECT_DOUBLE_EQ(inertial.mass, 1.2);
  EXPECT_DOUBLE_EQ(inertial.inertia[0][1], -.0003);
  EXPECT_DOUBLE_EQ(inertial.inertia[1][0], -.0003);
  EXPECT_DOUBLE_EQ(inertial.inertia[0][2], .0002);
  EXPECT_DOUBLE_EQ(inertial.inertia[2][1], .0001);

  expectVector(glm::dvec3(runtime.worldFromLinks()[upper][3]), {.10, -.20, .25});
  const auto zeroForearm = runtime.worldFromLinks()[forearm];
  expectVector(glm::dvec3(zeroForearm[3]), {.05, .40, .25});
  expectVector(glm::dvec3(zeroForearm[0]), {0, 1, 0});
  expectVector(glm::dvec3(zeroForearm[1]), {0, 0, 1});
  expectVector(glm::dvec3(zeroForearm[2]), {1, 0, 0});
  expectVector(glm::dvec3(runtime.worldFromVisual(forearm, 1)[3]), {.05, .85, .25});
  expectVector(glm::dvec3(runtime.worldFromCollision(forearm, 0)[3]), {.05, .625, .25});
  const auto zeroInertial = runtime.worldFromInertial(forearm);
  ASSERT_TRUE(zeroInertial);
  expectVector(glm::dvec3((*zeroInertial)[3]), {.06, .625, .245});

  const std::array rotated{UrdfJointValue{"shoulder", pi / 2}, UrdfJointValue{"elbow", pi / 2}};
  runtime.setPositions(rotated);
  expectVector(glm::dvec3(runtime.worldFromLinks()[upper][3]), {.10, -.20, .25});
  expectVector(glm::dvec3(runtime.worldFromLinks()[forearm][3]), {-.50, -.25, .25});
  expectVector(glm::dvec3(runtime.worldFromVisual(forearm, 1)[3]), {-.50, -.25, .70});
  expectVector(glm::dvec3(runtime.worldFromCollision(forearm, 0)[3]), {-.50, -.25, .475});
}

TEST(UrdfRuntime, InvalidBatchesAreAtomicAndFixedStepsRespectJointVelocities) {
  UrdfRuntime runtime(armFixture());
  const auto shoulder = jointIndex(runtime.model(), "shoulder");
  const auto elbow = jointIndex(runtime.model(), "elbow");
  const std::array targets{UrdfJointValue{"shoulder", 1.2}, UrdfJointValue{"elbow", -1.0}};
  runtime.setTargets(targets);
  const std::array invalid{UrdfJointValue{"shoulder", .2}, UrdfJointValue{"elbow", 2.0}};
  EXPECT_THROW(runtime.setTargets(invalid), std::invalid_argument);
  expectJoint(runtime.joints()[shoulder], {.target = 1.2});
  expectJoint(runtime.joints()[elbow], {.target = -1.0});
  const std::array invalidDirect{UrdfJointValue{"elbow", .1},
                                 UrdfJointValue{"shoulder", std::numeric_limits<double>::infinity()}};
  const auto before = runtime.worldFromLinks()[linkIndex(runtime.model(), "forearm")];
  EXPECT_THROW(runtime.setPositions(invalidDirect), std::invalid_argument);
  EXPECT_EQ(runtime.worldFromLinks()[linkIndex(runtime.model(), "forearm")], before);
  expectJoint(runtime.joints()[elbow], {.target = -1.0});

  runtime.step();
  expectJoint(runtime.joints()[shoulder], {.position = .025, .velocity = 1.5, .target = 1.2});
  expectJoint(runtime.joints()[elbow], {.position = -2.0 / 60.0, .velocity = -2.0, .target = -1.0});
  const std::array direct{UrdfJointValue{"shoulder", .6}};
  runtime.setPositions(direct);
  expectJoint(runtime.joints()[shoulder], {.position = .6, .target = .6});
  expectJoint(runtime.joints()[elbow], {.position = -2.0 / 60.0, .velocity = -2.0, .target = -1.0});
  runtime.step();
  expectJoint(runtime.joints()[shoulder], {.position = .6, .target = .6});
}

struct TickWitness {
  uint64_t tick;
  std::array<UrdfJointState, 2> joints;
  glm::dmat4 tipFrame;
};

std::vector<TickWitness> replayArm(UrdfRuntime& runtime, uint32_t framesPerSecond) {
  const auto forearm = linkIndex(runtime.model(), "forearm");
  runtime.setPaused(false);
  std::vector<TickWitness> result;
  const std::array firstTargets{UrdfJointValue{"shoulder", 1.2}, UrdfJointValue{"elbow", .8}};
  const std::array nextTargets{UrdfJointValue{"shoulder", -.7}, UrdfJointValue{"elbow", -.8}};
  for (const auto& targets : {firstTargets, nextTargets}) {
    runtime.setTargets(targets);
    auto elapsed = 0ns;
    uint64_t previousTick = runtime.clock().tick();
    constexpr int64_t nanosPerSecond = 1'000'000'000;
    for (uint32_t frame = 1; frame <= framesPerSecond; ++frame) {
      // A ceiling-aligned deadline makes every 60 Hz tick visible at both cadences.
      const auto target = std::chrono::nanoseconds(
          (nanosPerSecond * frame + framesPerSecond - 1) / framesPerSecond);
      runtime.advance(target - elapsed);
      elapsed = target;
      const auto tick = runtime.clock().tick();
      if (tick == previousTick) continue;
      EXPECT_EQ(tick, previousTick + 1);
      result.push_back({tick, {runtime.joints()[0], runtime.joints()[1]},
                        runtime.worldFromVisual(forearm, 1)});
      previousTick = tick;
    }
  }
  return result;
}

TEST(UrdfRuntime, ResetRestoresAuthoredStateAndReplaysAtTwoFrameCadences) {
  const auto root = urdfPoseFromRpy({10, -2, 3}, {0, 0, pi / 2});
  UrdfRuntime runtime(armFixture(), root);
  const auto forearm = linkIndex(runtime.model(), "forearm");
  EXPECT_NEAR(runtime.worldFromVisual(forearm, 1)[3].x, 9.15, 1e-12);
  const auto at60 = replayArm(runtime, 60);
  ASSERT_EQ(at60.size(), 120u);
  EXPECT_EQ(runtime.clock().tick(), 120u);
  runtime.setWorldFromRoot(urdfPoseFromRpy({2, 4, 8}, {0, 0, 0}));
  runtime.reset();
  EXPECT_TRUE(runtime.clock().paused());
  EXPECT_EQ(runtime.clock().tick(), 0u);
  EXPECT_EQ(runtime.advance(1s), 0u);
  expectVector(glm::dvec3(runtime.worldFromLinks()[runtime.model().rootLink][3]), {10, -2, 3});
  expectJoint(runtime.joints()[0], {});
  expectJoint(runtime.joints()[1], {});

  const auto at120 = replayArm(runtime, 120);
  ASSERT_EQ(at120.size(), at60.size());
  EXPECT_DOUBLE_EQ(runtime.clock().seconds(), 2.0);
  for (size_t i = 0; i < at60.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(at120[i].tick, at60[i].tick);
    for (size_t joint = 0; joint < 2; ++joint)
      expectJoint(at120[i].joints[joint], at60[i].joints[joint]);
    EXPECT_EQ(at120[i].tipFrame, at60[i].tipFrame);
  }
}

TEST(UrdfRuntime, PositiveOnlySliderStartsAtItsLowerLimitAndUsesJointAxis) {
  UrdfRuntime runtime(parseUrdf(R"urdf(
<robot name="positive_slider">
  <link name="base"/><link name="slider"/>
  <joint name="slide" type="prismatic">
    <parent link="base"/><child link="slider"/>
    <origin xyz="0.10 0.20 0.30" rpy="0 0 1.5707963267948966"/>
    <axis xyz="2 0 0"/><limit lower="0.15" upper="0.55" effort="10" velocity="0.4"/>
  </joint>
</robot>)urdf"));
  const auto slider = linkIndex(runtime.model(), "slider");
  expectJoint(runtime.joints()[0], {.position = .15, .target = .15});
  expectVector(glm::dvec3(runtime.worldFromLinks()[slider][3]), {.10, .35, .30});
  const std::array target{UrdfJointValue{"slide", .55}};
  runtime.setTargets(target);
  runtime.step();
  expectJoint(runtime.joints()[0], {.position = .15 + .4 / 60, .velocity = .4, .target = .55});
  runtime.setPositions(target);
  expectVector(glm::dvec3(runtime.worldFromLinks()[slider][3]), {.10, .75, .30});
  runtime.reset();
  expectJoint(runtime.joints()[0], {.position = .15, .target = .15});
  expectVector(glm::dvec3(runtime.worldFromLinks()[slider][3]), {.10, .35, .30});
}

TEST(UrdfRuntime, ContinuousCoordinatesRemainUnwrappedWhileFkUsesAngularEquivalence) {
  UrdfRuntime runtime(parseUrdf(R"urdf(
<robot name="continuous_spindle">
  <link name="base"/><link name="spindle"/>
  <joint name="spindle_joint" type="continuous">
    <parent link="base"/><child link="spindle"/>
    <axis xyz="0 0 1"/><limit effort="2" velocity="3"/>
  </joint>
</robot>)urdf"));
  const auto spindle = linkIndex(runtime.model(), "spindle");
  constexpr double position = 8 * pi + pi / 2;
  const std::array measured{UrdfJointValue{"spindle_joint", position}};
  runtime.setPositions(measured);
  expectJoint(runtime.joints()[0], {.position = position, .target = position});
  expectVector(glm::dvec3(runtime.worldFromLinks()[spindle][0]), {0, 1, 0});
  expectVector(glm::dvec3(runtime.worldFromLinks()[spindle][1]), {-1, 0, 0});
  const std::array target{UrdfJointValue{"spindle_joint", 0}};
  runtime.setTargets(target);
  runtime.step();
  expectJoint(runtime.joints()[0], {.position = position - .05, .velocity = -3, .target = 0});
  EXPECT_GT(runtime.joints()[0].position, 8 * pi);
}

TEST(UrdfRuntime, PreservesMeshUriSignedScaleAndNamedMaterialsWithoutResolvingResources) {
  const auto model = parseUrdf(R"urdf(
<robot name="visual_metadata">
  <material name="paint"><color rgba="0.1 0.2 0.3 0.75"/></material>
  <material name="wrap"><texture filename="package://arm_description/textures/wrap.png"/></material>
  <link name="base">
    <visual name="fin"><geometry><mesh filename="package://arm_description/meshes/fin.gltf" scale="-1 2 0.5"/></geometry><material name="paint"/></visual>
    <visual name="cap"><geometry><sphere radius="0.03"/></geometry><material name="wrap"/></visual>
  </link>
</robot>)urdf");
  ASSERT_EQ(model.links.size(), 1u);
  const auto& visuals = model.links.front().visuals;
  ASSERT_EQ(visuals.size(), 2u);
  const auto* fin = std::get_if<UrdfMesh>(&visuals[0].geometry);
  ASSERT_NE(fin, nullptr);
  EXPECT_EQ(fin->uri, "package://arm_description/meshes/fin.gltf");
  EXPECT_EQ(fin->scale, glm::dvec3(-1, 2, .5));
  ASSERT_TRUE(visuals[0].material && visuals[0].material->rgba);
  EXPECT_EQ(visuals[0].material->name, "paint");
  EXPECT_EQ(*visuals[0].material->rgba, glm::dvec4(.1, .2, .3, .75));
  ASSERT_TRUE(visuals[1].material);
  EXPECT_EQ(visuals[1].material->textureUri, "package://arm_description/textures/wrap.png");
}

void expectInvalidUrdf(std::string_view xml, std::string_view context) {
  try {
    (void)parseUrdf(xml);
    ADD_FAILURE() << "Accepted invalid URDF for " << context;
  } catch (const std::invalid_argument& error) {
    const std::string_view message(error.what());
    EXPECT_TRUE(message.starts_with("URDF line ")) << message;
    EXPECT_NE(message.find(context), std::string_view::npos) << message;
  }
}

TEST(UrdfRuntime, InvalidTreeFiniteValuesShapesAndInertiasHaveContext) {
  expectInvalidUrdf(R"urdf(<robot name="bad_tree"><link name="base"/>
    <joint name="broken" type="fixed"><parent link="base"/><child link="missing"/></joint>
  </robot>)urdf", "unknown child link 'missing'");
  expectInvalidUrdf(R"urdf(<robot name="ambiguous_parent">
    <link name="left"/><link name="right"/><link name="tip"/>
    <joint name="from_left" type="fixed"><parent link="left"/><child link="tip"/></joint>
    <joint name="from_right" type="fixed"><parent link="right"/><child link="tip"/></joint>
  </robot>)urdf", "link 'tip' has multiple parent joints");
  expectInvalidUrdf(R"urdf(<robot name="bad_origin"><link name="base">
    <visual><origin xyz="0 inf 0"/><geometry><box size="1 1 1"/></geometry></visual>
  </link></robot>)urdf", "xyz");
  expectInvalidUrdf(R"urdf(<robot name="bad_shape"><link name="base">
    <collision><geometry><sphere radius="-0.1"/></geometry></collision>
  </link></robot>)urdf", "radius");
  expectInvalidUrdf(R"urdf(<robot name="bad_tensor"><link name="base">
    <inertial><mass value="1"/><inertia ixx="1" ixy="0" ixz="0" iyy="1" iyz="0" izz="3"/></inertial>
  </link></robot>)urdf", "inertia");
}

} // namespace
