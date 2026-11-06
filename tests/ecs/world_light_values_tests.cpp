#include <Container/ecs/World.h>
#include <gtest/gtest.h>
#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

namespace {
using namespace container::ecs;
using Point=container::gpu::PointLightData;
using Guard=material_publication_test::AllocationGuard;
Point point(float marker) {Point p;p.positionRadius={marker,2,3,8};p.colorIntensity={.25f,.5f,1,marker};return p;}
bool equal(const Point& a,const Point& b) {return std::memcmp(&a,&b,sizeof(Point))==0;}
struct Opaque {
  int value{71};Opaque()=default;Opaque(const Opaque&) {throw std::runtime_error("Opaque context copied");}
};
struct Unknown {int value{37};};
struct Signals {int updates{};void changed(entt::registry&,entt::entity){++updates;}};
void unchanged(const World& world,entt::entity entity,const Point& expected) {
  ASSERT_TRUE(world.registry().valid(entity));ASSERT_TRUE((world.registry().all_of<LightComponent,LightTag>(entity)));
  EXPECT_TRUE(equal(world.registry().get<LightComponent>(entity).data,expected));
}
bool publish(World& world,World::PreparedLightValues& stage) {
  bool ready{},done{};size_t attempts{};
  {Guard deny(true);ready=world.canExchangeLightValues(stage);done=world.exchangeLightValues(stage);attempts=deny.allocations();}
  EXPECT_EQ(attempts,0u);return ready && done;
}
static_assert(!std::is_copy_constructible_v<World::PreparedLightValues>);
static_assert(std::is_nothrow_move_constructible_v<World::PreparedLightValues>);
static_assert(noexcept(std::declval<World&>().exchangeLightValues(std::declval<World::PreparedLightValues&>())));
}

TEST(WorldLightValues, CanonicalRowsMapToDistinctEntitiesIncludingDuplicatePayloadsWithoutWorldOrContextCopy) {
  World world;const auto a=point(1),b=point(2);const auto first=world.createPointLight(a),second=world.createPointLight(b),third=world.createPointLight(a);
  world.registry().emplace<Unknown>(first);auto& opaque=world.registry().ctx().emplace<Opaque>();
  auto& text=world.registry().ctx().emplace<std::string>(16*1024*1024,'x');const auto* textBytes=text.data();
  Signals signals;world.registry().on_update<LightComponent>().connect<&Signals::changed>(signals);
  const auto mesh=world.registry().create();world.registry().emplace<MeshComponent>(mesh,7u);world.registry().emplace<RenderableTag>(mesh);
  (void)world.setActiveCamera({});const auto* camera=world.activeCamera();const auto entities=world.entityCount();
  const std::array before{b,a,a},after{point(10),point(11),point(12)};
  auto prepared=world.prepareLightValues(before,after);ASSERT_TRUE(prepared);EXPECT_GT(prepared.ownedBytes(),0u);
  unchanged(world,first,a);unchanged(world,second,b);unchanged(world,third,a);
  EXPECT_TRUE(publish(world,prepared));EXPECT_FALSE(prepared);
  const auto low=entt::to_integral(first)<entt::to_integral(third)?first:third;
  const auto high=low==first?third:first;
  unchanged(world,low,after[1]);unchanged(world,high,after[2]);unchanged(world,second,after[0]);
  EXPECT_EQ(world.entityCount(),entities);EXPECT_EQ(world.activeCamera(),camera);
  EXPECT_EQ(world.registry().ctx().find<Opaque>(),&opaque);EXPECT_EQ(text.data(),textBytes);
  EXPECT_EQ(signals.updates,0);
  EXPECT_EQ(world.registry().get<Unknown>(first).value,37);EXPECT_EQ(world.registry().get<MeshComponent>(mesh).primitiveIndex,7u);
}

TEST(WorldLightValues, EmptyDirectionalOrAreaMirrorIsValidAndDoesNotInspectUnrelatedOpaqueComponents) {
  World world;const auto entity=world.registry().create();world.registry().emplace<Unknown>(entity);
  auto& opaque=world.registry().ctx().emplace<Opaque>();auto prepared=world.prepareLightValues({},{});
  EXPECT_TRUE(publish(world,prepared));EXPECT_EQ(world.entityCount(),1u);EXPECT_EQ(world.registry().ctx().find<Opaque>(),&opaque);
  EXPECT_EQ(world.registry().get<Unknown>(entity).value,37);
}

TEST(WorldLightValues, WrongCountsPayloadsNonfiniteAndIncompleteEntitiesRejectWithoutMutation) {
  World world;const auto old=point(1);const auto entity=world.createPointLight(old);const std::array before{old},after{point(2)};
  EXPECT_THROW((void)world.prepareLightValues(before,{}),std::invalid_argument);
  EXPECT_THROW((void)world.prepareLightValues(std::array{point(3)},after),std::invalid_argument);
  auto invalid=after;invalid[0].directionInnerCos.x=std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)world.prepareLightValues(before,invalid),std::invalid_argument);unchanged(world,entity,old);
  world.registry().remove<LightTag>(entity);
  EXPECT_THROW((void)world.prepareLightValues(before,after),std::invalid_argument);
  world.registry().emplace<LightTag>(entity);world.registry().remove<LightComponent>(entity);
  EXPECT_THROW((void)world.prepareLightValues(before,after),std::invalid_argument);
}

TEST(WorldLightValues, ExactPointTypeValueTagAndEntityMembershipStalenessRejectsWithoutAllocating) {
  for(int change=0;change<4;++change) {
    World world;const auto old=point(1);const auto entity=world.createPointLight(old);
    auto prepared=world.prepareLightValues(std::array{old},std::array{point(2)});
    if(change==0) world.registry().get<LightComponent>(entity).data.colorIntensity.w=9;
    if(change==1) world.registry().get<LightComponent>(entity).data.coneOuterCosType.y=1;
    if(change==2) world.registry().remove<LightTag>(entity);
    if(change==3) {world.registry().destroy(entity);(void)world.createPointLight(old);}
    bool ready{},done{};size_t attempts{};
    {Guard deny(true);ready=world.canExchangeLightValues(prepared);done=world.exchangeLightValues(prepared);attempts=deny.allocations();}
    EXPECT_FALSE(ready);EXPECT_FALSE(done);EXPECT_EQ(attempts,0u);EXPECT_TRUE(prepared);
  }
}

TEST(WorldLightValues, OriginalStageNeverRevivesAfterAnOwnedValueRoundTrip) {
  World world;const auto a=point(1),b=point(2);const auto entity=world.createPointLight(a);
  auto original=world.prepareLightValues(std::array{a},std::array{point(9)});
  auto first=world.prepareLightValues(std::array{a},std::array{b});ASSERT_TRUE(publish(world,first));
  auto second=world.prepareLightValues(std::array{b},std::array{a});ASSERT_TRUE(publish(world,second));unchanged(world,entity,a);
  EXPECT_FALSE(world.canExchangeLightValues(original));EXPECT_FALSE(world.exchangeLightValues(original));
}

TEST(WorldLightValues, ForeignClonesEqualStateSwapAndOwnerAddressReuseCannotAuthorizeOldStages) {
  World world;const auto p=point(1);(void)world.createPointLight(p);auto copy=world.cloneStateWithoutContext();
  auto token=world.prepareLightValues(std::array{p},std::array{point(2)});
  EXPECT_FALSE(copy->canExchangeLightValues(token));world.swapStatePreservingContext(*copy);
  EXPECT_FALSE(world.canExchangeLightValues(token));
  alignas(World) std::array<std::byte,sizeof(World)> storage;auto* first=std::construct_at(reinterpret_cast<World*>(storage.data()));
  (void)first->createPointLight(p);auto old=first->prepareLightValues(std::array{p},std::array{point(3)});std::destroy_at(first);
  auto* replacement=std::construct_at(reinterpret_cast<World*>(storage.data()));(void)replacement->createPointLight(p);
  EXPECT_FALSE(replacement->canExchangeLightValues(old));std::destroy_at(replacement);
}

TEST(WorldLightValues, AdmissionBudgetsRejectBeforePublicationAndAllowTheExactDeclaredPeakCharge) {
  World world;const std::array before{point(1),point(2)},after{point(3),point(4)};world.replacePointLights(before);
  auto stage=world.prepareLightValues(before,after);World::LightValueLimits limits;limits.maxOwnedBytes=stage.ownedBytes();
  EXPECT_NO_THROW((void)world.prepareLightValues(before,after,limits));--limits.maxOwnedBytes;
  EXPECT_THROW((void)world.prepareLightValues(before,after,limits),std::invalid_argument);
  limits={};limits.maxLights=1;EXPECT_THROW((void)world.prepareLightValues(before,after,limits),std::invalid_argument);
  limits={};limits.maxWork=1;EXPECT_THROW((void)world.prepareLightValues(before,after,limits),std::invalid_argument);
  limits={};limits.maxOwnedBytes=UINT64_MAX;EXPECT_THROW((void)world.prepareLightValues(before,after,limits),std::invalid_argument);
  EXPECT_TRUE(world.canExchangeLightValues(stage));
}

TEST(WorldLightValues, EveryPreparationAllocationFailurePreservesLiveValuesOpaqueContextAndAnEarlierStage) {
  World world;const std::array before{point(1),point(2)},after{point(3),point(4)};world.replacePointLights(before);
  auto& opaque=world.registry().ctx().emplace<Opaque>();auto earlier=world.prepareLightValues(before,after);size_t count{};
  {Guard measure(false);auto token=world.prepareLightValues(before,after);count=measure.allocations();}
  ASSERT_GT(count,0u);
  for(size_t cut=0;cut<count;++cut) {
    bool denied{};World::PreparedLightValues token;
    {Guard fail(false,cut);try {token=world.prepareLightValues(before,after);}catch(const std::bad_alloc&) {denied=true;}}
    EXPECT_TRUE(denied);EXPECT_FALSE(token);EXPECT_TRUE(world.canExchangeLightValues(earlier));EXPECT_EQ(world.registry().ctx().find<Opaque>(),&opaque);
  }
}

TEST(WorldLightValues, StructuralAdmissionAndCloneWitnessExcludeContextAndDetectEveryEnginePayload) {
  World world;const auto entity=world.createPointLight(point(1));auto& text=world.registry().ctx().emplace<std::string>(16*1024*1024,'c');
  auto& opaque=world.registry().ctx().emplace<Opaque>();(void)world.setActiveCamera({});
  uint64_t bytes{};size_t attempts{};
  {Guard deny(true);bytes=world.stateStorageBytes(64ull*1024*1024,64ull*1024*1024);attempts=deny.allocations();}
  EXPECT_EQ(attempts,0u);EXPECT_LT(bytes,text.size());auto before=world.cloneStateWithoutContext();bool same{};
  {Guard deny(true);same=world.stateValuesMatch(*before);attempts=deny.allocations();}EXPECT_TRUE(same);EXPECT_EQ(attempts,0u);
  world.registry().get<LightComponent>(entity).data.positionRadius.x=17;EXPECT_FALSE(world.stateValuesMatch(*before));
  world.registry().get<LightComponent>(entity).data=point(1);EXPECT_TRUE(world.stateValuesMatch(*before));
  const auto camera=world.registry().view<CameraComponent>().front();world.registry().get<CameraComponent>(camera).nearPlane=-0.f;
  EXPECT_FALSE(world.stateValuesMatch(*before));EXPECT_EQ(world.registry().ctx().find<Opaque>(),&opaque);
}

TEST(WorldLightValues, StructuralAdmissionRejectsUnknownPoolsNonfiniteValuesAndFiniteBudgetExhaustion) {
  World world;const auto entity=world.createPointLight(point(1));
  EXPECT_THROW((void)world.stateStorageBytes(1,1000),std::invalid_argument);
  EXPECT_THROW((void)world.stateStorageBytes(64ull*1024*1024,1),std::invalid_argument);
  EXPECT_THROW((void)world.stateStorageBytes(UINT64_MAX,10000),std::invalid_argument);
  world.registry().get<LightComponent>(entity).data.positionRadius.x=std::numeric_limits<float>::infinity();
  EXPECT_THROW((void)world.stateStorageBytes(64ull*1024*1024,10000),std::invalid_argument);
  world.registry().get<LightComponent>(entity).data=point(1);world.registry().emplace<Unknown>(entity);
  EXPECT_THROW((void)world.stateStorageBytes(64ull*1024*1024,10000),std::invalid_argument);
}
