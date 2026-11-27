#include "Container/robotics/UrdfCollisionBindings.h"

#include <gtest/gtest.h>

#include <array>
#include <numbers>
#include <stdexcept>
#include <type_traits>

namespace {

using namespace container::robotics;
using namespace container::physics;

static_assert(!std::is_copy_assignable_v<UrdfRuntime>);
static_assert(!std::is_move_constructible_v<UrdfRuntime>);

TEST(UrdfCollisionBindings, NativeCylinderAndMovingForearmAreInTheExistingJoltWorld) {
  const auto fixture = std::filesystem::path(CONTAINER_ROBOTICS_FIXTURES) / "two-link-arm.urdf";
  UrdfRuntime robot(loadUrdf(fixture));
  PhysicsWorld world({.gravity = {0.f, 0.f, -9.81f}, .workerThreads = 1});
  const auto unrelated = world.createBox({.1f, .1f, .1f},
      {.motionType = PhysicsMotionType::Static, .position = {4, 0, 0}});
  {
    UrdfCollisionBindings bindings(world, robot);
    ASSERT_EQ(bindings.bodies().size(), 3u);
    EXPECT_EQ(world.bodyCount(), 4u);
    PhysicsBodyId forearm = kInvalidPhysicsBodyId;
    PhysicsBodyId pedestal = kInvalidPhysicsBodyId;
    for (const auto& binding : bindings.bodies()) {
      const auto& link = robot.model().links[binding.link];
      if (link.name == "forearm") forearm = binding.body;
      if (link.name == "base_link") pedestal = binding.body;
    }
    ASSERT_NE(forearm, kInvalidPhysicsBodyId);
    ASSERT_NE(pedestal, kInvalidPhysicsBodyId);

    const auto top = world.rayCast({-.14, 0, 1}, {0, 0, -1});
    ASSERT_TRUE(top);
    EXPECT_EQ(top->body, pedestal);
    EXPECT_NEAR(top->position.z, .12, 1e-5);

    const auto oldHit = world.rayCast({.05, .625, 1}, {0, 0, -1});
    ASSERT_TRUE(oldHit);
    EXPECT_EQ(oldHit->body, forearm);
    EXPECT_NEAR(oldHit->position.z, .29, 1e-5);

    constexpr auto rightAngle = std::numbers::pi / 2;
    const std::array values{UrdfJointValue{"shoulder", rightAngle},
                            UrdfJointValue{"elbow", rightAngle}};
    robot.setPositions(values);
    bindings.synchronize();
    EXPECT_FALSE(world.rayCast({.05, .625, 1}, {0, 0, -1}));

    const auto moved = world.rayCast({-.5, -.25, 1}, {0, 0, -1});
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved->body, forearm);
    EXPECT_NEAR(moved->position.z, .70, 1e-5);
  }

  EXPECT_EQ(world.bodyCount(), 1u);
  EXPECT_TRUE(world.contains(unrelated));
}

TEST(UrdfCollisionBindings, UnsupportedMovingMeshDoesNotPartiallyAttachPrimitives) {
  const auto model = parseUrdf(R"(
    <robot name="mesh_test">
      <link name="base">
        <collision><geometry><sphere radius="0.2"/></geometry></collision>
        <collision><geometry><mesh filename="package://demo/part.stl"/></geometry></collision>
      </link>
    </robot>)");
  UrdfRuntime robot(model);
  PhysicsWorld world({.workerThreads = 1});
  const auto existing = world.createSphere(.1f, {.motionType = PhysicsMotionType::Static});
  EXPECT_THROW(UrdfCollisionBindings(world, robot), std::invalid_argument);
  EXPECT_EQ(world.bodyCount(), 1u);
  EXPECT_TRUE(world.contains(existing));
}

} // namespace
