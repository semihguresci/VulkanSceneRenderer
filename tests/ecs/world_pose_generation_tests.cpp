#include <Container/ecs/World.h>
#include <gtest/gtest.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace {
using container::ecs::World;
struct ThrowOnCopy {
  int value{17};
  ThrowOnCopy()=default;
  ThrowOnCopy(const ThrowOnCopy&) {throw std::runtime_error("Opaque context was copied");}
};
struct UnknownComponent {int value{1};};
void populate(World& world) {
  const auto e=world.registry().create();world.registry().emplace<container::ecs::MeshComponent>(e,3u);
  world.registry().emplace<container::ecs::MaterialComponent>(e,7u);world.registry().emplace<container::ecs::RenderableTag>(e);
  world.registry().emplace<container::ecs::SceneNodeRef>(e,11u);world.registry().emplace<container::ecs::TransformComponent>(e);
  container::gpu::CameraData camera;camera.cameraWorldPosition={1,2,3,1};(void)world.setActiveCamera(camera,.1f,100);
  container::gpu::PointLightData light;light.colorIntensity={.5f,.25f,1,8};(void)world.createPointLight(light);
}
} // namespace

TEST(WorldPoseGeneration, DetachedEngineCopyPreservesEntitiesButNeverCopiesOpaqueOrLargeContext) {
  World world;populate(world);auto& text=world.registry().ctx().emplace<std::string>(1024*1024,'a');
  auto& opaque=world.registry().ctx().emplace<ThrowOnCopy>();const auto* bytes=text.data();
  auto copy=world.cloneStateWithoutContext();ASSERT_TRUE(copy);EXPECT_EQ(copy->entityCount(),world.entityCount());
  EXPECT_EQ(copy->renderableCount(),1u);EXPECT_EQ(copy->pointLightCount(),1u);ASSERT_NE(copy->activeCamera(),nullptr);
  EXPECT_EQ(copy->activeCamera()->data.cameraWorldPosition,glm::vec4(1,2,3,1));
  for(const auto entity:world.registry().view<container::ecs::SceneNodeRef>()) {
    ASSERT_TRUE(copy->registry().valid(entity));EXPECT_EQ(copy->registry().get<container::ecs::SceneNodeRef>(entity).nodeIndex,11u);
  }
  EXPECT_FALSE(copy->registry().ctx().contains<std::string>());EXPECT_FALSE(copy->registry().ctx().contains<ThrowOnCopy>());
  EXPECT_EQ(world.registry().ctx().find<ThrowOnCopy>(),&opaque);EXPECT_EQ(text.data(),bytes);EXPECT_EQ(text.size(),1024*1024u);
  EXPECT_THROW((void)world.cloneState(),std::runtime_error); // Legacy full-context semantics remain strict.
}

TEST(WorldPoseGeneration, AllocationFreeExchangeKeepsEachWorldsOriginalContextAndExchangesCameraAndComponents) {
  World live,staged;populate(live);populate(staged);
  auto& a=live.registry().ctx().emplace<std::string>(1024*1024,'a');auto& b=staged.registry().ctx().emplace<std::string>(512*1024,'b');
  auto& opaqueA=live.registry().ctx().emplace<ThrowOnCopy>();auto& opaqueB=staged.registry().ctx().emplace<ThrowOnCopy>();
  opaqueB.value=29;const auto* bytesA=a.data();const auto* bytesB=b.data();
  container::gpu::CameraData camera;camera.cameraWorldPosition={5,6,7,1};(void)staged.setActiveCamera(camera,.2f,200);
  size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);live.swapStatePreservingContext(staged);attempts=guard.allocations();}
  EXPECT_EQ(attempts,0u);EXPECT_EQ(live.registry().ctx().find<std::string>(),&a);EXPECT_EQ(staged.registry().ctx().find<std::string>(),&b);
  EXPECT_EQ(a.data(),bytesA);EXPECT_EQ(b.data(),bytesB);EXPECT_EQ(live.registry().ctx().find<ThrowOnCopy>(),&opaqueA);
  EXPECT_EQ(staged.registry().ctx().find<ThrowOnCopy>(),&opaqueB);EXPECT_EQ(opaqueA.value,17);EXPECT_EQ(opaqueB.value,29);
  ASSERT_NE(live.activeCamera(),nullptr);ASSERT_NE(staged.activeCamera(),nullptr);
  EXPECT_EQ(live.activeCamera()->data.cameraWorldPosition,glm::vec4(5,6,7,1));
  EXPECT_EQ(staged.activeCamera()->data.cameraWorldPosition,glm::vec4(1,2,3,1));
  {material_publication_test::AllocationGuard guard(true);live.swapStatePreservingContext(staged);attempts=guard.allocations();}
  EXPECT_EQ(attempts,0u);EXPECT_EQ(live.activeCamera()->data.cameraWorldPosition,glm::vec4(1,2,3,1));
  EXPECT_EQ(live.registry().ctx().find<ThrowOnCopy>(),&opaqueA);
}

TEST(WorldPoseGeneration, UnknownNonemptyComponentsRejectWithoutChangingTheLiveWorldOrContext) {
  World world;populate(world);world.registry().ctx().emplace<ThrowOnCopy>();const auto count=world.entityCount();
  const auto e=world.registry().create();world.registry().emplace<UnknownComponent>(e);
  EXPECT_THROW((void)world.cloneStateWithoutContext(),std::runtime_error);EXPECT_EQ(world.entityCount(),count+1);
  EXPECT_TRUE(world.registry().valid(e));EXPECT_EQ(world.registry().get<UnknownComponent>(e).value,1);
  EXPECT_TRUE(world.registry().ctx().contains<ThrowOnCopy>());
}

TEST(WorldPoseGeneration, EveryDetachedCopyAllocationFailurePreservesLiveComponentsAndOpaqueContext) {
  World world;populate(world);auto& context=world.registry().ctx().emplace<ThrowOnCopy>();
  world.registry().ctx().emplace<std::string>(1024*1024,'x');size_t allocations{};
  {material_publication_test::AllocationGuard guard(false);auto prepared=world.cloneStateWithoutContext();allocations=guard.allocations();}
  ASSERT_GT(allocations,0u);const auto count=world.entityCount();
  for(size_t cut=0;cut<allocations;++cut) {
    bool denied{};std::unique_ptr<World> prepared;
    {material_publication_test::AllocationGuard guard(false,cut);try {prepared=world.cloneStateWithoutContext();}catch(const std::bad_alloc&) {denied=true;}}
    EXPECT_TRUE(denied);EXPECT_FALSE(prepared);EXPECT_EQ(world.entityCount(),count);
    EXPECT_EQ(world.registry().ctx().find<ThrowOnCopy>(),&context);EXPECT_EQ(world.renderableCount(),1u);EXPECT_EQ(world.pointLightCount(),1u);
    ASSERT_NE(world.activeCamera(),nullptr);EXPECT_EQ(world.activeCamera()->data.cameraWorldPosition,glm::vec4(1,2,3,1));
  }
}
