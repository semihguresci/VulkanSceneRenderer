#include <Container/renderer/lighting/LightEditReceipt.h>
#include <gtest/gtest.h>
#include "../scene/material_publication_allocations.h"

#include <limits>
#include <cmath>
#include <vector>

namespace {
using namespace container::renderer;
uint32_t field(LightEditField f) {return LightEditFieldBit(f);}
struct Fixture {
  LightEditTarget target{1,2,3,0,4,5,{EditableLightType::Point,EditableLightSource::Imported,0}};
  EditableLightEntity base,displayed;
  std::optional<EditableLightEntity> current;
  Fixture() {
    base.id=target.id;base.type=target.id.type;base.source=target.id.source;base.label="Source point";
    base.position={3,2,1};base.color={.25f,.5f,1};base.intensity=7;base.range=10;
    displayed=base;
  }
  LightEditPatch intensity(float value) const {LightEditPatch p;p.fields=field(LightEditField::Intensity);p.values.intensity=value;return p;}
  PreparedLightEditPlan edit(float value,const LightEditCommand* original=nullptr,const LightEditLimits& limits={}) const {
    return PlanLightEdit(target,displayed,base,current,intensity(value),original,limits);
  }
  void present(float value) {current=base;current->intensity=value;displayed=*current;}
  LightEditCommand publish(PreparedLightEditPlan& p) {
    const auto command=p.command();const auto after=p.overrideAfter();const auto selected=p.selectedAfter();
    EXPECT_TRUE(LightEditPlanMatches(p,target,displayed,base,current));EXPECT_TRUE(PublishLightEditPlan(p));
    current=after;displayed=selected;++target.revision;return command;
  }
  PreparedLightEditPlan replay(const LightEditCommand& c,bool applied) const {return RebaseLightEdit(target,displayed,base,current,c,applied);}
};

TEST(LightEditReceipt, FirstImportedOverrideOwnsPresenceAndRemovesAtTodaysSource) {
  Fixture f;auto p=f.edit(20);EXPECT_TRUE(p.changed());const auto c=f.publish(p);
  EXPECT_TRUE(c.published());EXPECT_TRUE(c.applied());EXPECT_TRUE(c.netChanged());ASSERT_TRUE(f.current);EXPECT_EQ(f.current->intensity,20);
  f.base.intensity=11;f.base.position={8,9,10};auto undo=f.replay(c,false);EXPECT_FALSE(undo.overrideAfter());
  EXPECT_EQ(undo.selectedAfter().intensity,11);EXPECT_EQ(undo.selectedAfter().position,f.base.position);f.publish(undo);
  EXPECT_FALSE(c.applied());EXPECT_FALSE(f.current);
  auto redo=f.replay(c,true);ASSERT_TRUE(redo.overrideAfter());EXPECT_EQ(redo.overrideAfter()->intensity,20);
  EXPECT_EQ(redo.overrideAfter()->position,f.base.position);f.publish(redo);EXPECT_TRUE(c.applied());
}
TEST(LightEditReceipt, PresentOverrideReplayPreservesCurrentUnownedFields) {
  Fixture f;f.present(5);auto p=f.edit(20);const auto c=f.publish(p);f.current->color={1,.125f,.25f};f.displayed=*f.current;
  auto undo=f.replay(c,false);ASSERT_TRUE(undo.overrideAfter());EXPECT_EQ(undo.overrideAfter()->intensity,5);
  EXPECT_EQ(undo.overrideAfter()->color,f.current->color);f.publish(undo);
  auto redo=f.replay(c,true);EXPECT_EQ(redo.overrideAfter()->intensity,20);EXPECT_EQ(redo.overrideAfter()->color,f.current->color);
}
TEST(LightEditReceipt, CannotRemoveIntroducedOverrideChangedByAnotherOwner) {
  Fixture f;auto p=f.edit(20);const auto c=f.publish(p);f.current->position.x=std::nextafter(f.current->position.x,100.f);f.displayed=*f.current;
  EXPECT_THROW((void)f.replay(c,false),std::invalid_argument);EXPECT_TRUE(c.applied());EXPECT_EQ(f.current->intensity,20);
}
TEST(LightEditReceipt, OwnedEndpointAndPresenceAreBothExact) {
  Fixture f;f.present(5);auto p=f.edit(20);const auto c=f.publish(p);
  f.current->intensity=std::nextafter(20.f,30.f);f.displayed=*f.current;EXPECT_THROW((void)f.replay(c,false),std::invalid_argument);
  f.current.reset();f.displayed=f.base;EXPECT_THROW((void)f.replay(c,false),std::invalid_argument);
}
TEST(LightEditReceipt, FlattenedGestureKeepsFirstBeforeAndLatestAfterWithoutChangingOldReceipt) {
  Fixture f;auto p=f.edit(9);const auto first=f.publish(p);auto next=f.edit(17,&first);const auto latest=f.publish(next);
  EXPECT_EQ(first.target().incarnation,latest.target().incarnation);EXPECT_EQ(first.ownedBytes(),latest.ownedBytes());
  EXPECT_THROW((void)f.replay(first,false),std::invalid_argument);
  auto undo=f.replay(latest,false);EXPECT_FALSE(undo.overrideAfter());f.publish(undo);
  auto redo=f.replay(latest,true);ASSERT_TRUE(redo.overrideAfter());EXPECT_EQ(redo.overrideAfter()->intensity,17);
}
TEST(LightEditReceipt, ChangedLastRealizedPreviewCannotBeExtended) {
  Fixture f;auto p=f.edit(9);const auto c=f.publish(p);f.current->color.r=.875f;f.displayed=*f.current;
  EXPECT_THROW((void)f.edit(17,&c),std::invalid_argument);EXPECT_TRUE(c.applied());
}
TEST(LightEditReceipt, PresentGestureReturningToOriginalBeforeHasNoNetHistoryChange) {
  Fixture f;f.present(5);auto p=f.edit(9);const auto first=f.publish(p);auto next=f.edit(5,&first);const auto c=next.command();
  EXPECT_TRUE(next.changed());EXPECT_FALSE(c.netChanged());ASSERT_TRUE(next.overrideAfter());f.publish(next);
  EXPECT_EQ(f.current->intensity,5);
}
TEST(LightEditReceipt, IntroducedGestureReturningToTodaysExactBaseCollapsesToAbsence) {
  Fixture f;auto p=f.edit(9);const auto first=f.publish(p);auto next=f.edit(7,&first);const auto c=next.command();
  EXPECT_TRUE(next.changed());EXPECT_FALSE(next.overrideAfter());EXPECT_FALSE(c.netChanged());f.publish(next);EXPECT_FALSE(f.current);
}
TEST(LightEditReceipt, ReturningToOldScalarWhileSourceAdvancedStillRequiresOverrideAndHistory) {
  Fixture f;auto p=f.edit(9);const auto first=f.publish(p);f.base.intensity=11;
  auto next=f.edit(7,&first);ASSERT_TRUE(next.overrideAfter());EXPECT_TRUE(next.command().netChanged());EXPECT_EQ(next.overrideAfter()->intensity,7);
}
TEST(LightEditReceipt, NoopDoesNotMaterializeAnAbsentOverride) {
  Fixture f;auto p=f.edit(7);EXPECT_FALSE(p.changed());EXPECT_FALSE(p.overrideAfter());EXPECT_FALSE(p.command().netChanged());EXPECT_FALSE(p.command().published());
}
TEST(LightEditReceipt, SceneZeroIsValidButReplacementOwnerBirthAndFamilyReject) {
  Fixture f;auto p=f.edit(20);const auto c=f.publish(p);
  for(int change=0;change<7;++change) {auto target=f.target;
    if(change==0) ++target.ownerIdentity;if(change==1) ++target.assetIdentity;if(change==2) ++target.graphIdentity;
    if(change==3) ++target.scene;if(change==4) ++target.incarnation;if(change==5) target.id.source=EditableLightSource::Manual;
    if(change==6) target.id.type=EditableLightType::Spot;
    EXPECT_THROW((void)RebaseLightEdit(target,f.displayed,f.base,f.current,c,false),std::invalid_argument);
  }
}
TEST(LightEditReceipt, OriginalIncarnationCanResolveAnotherDisplayOrdinal) {
  Fixture f;auto p=f.edit(20);const auto c=f.publish(p);f.target.id.index=7;f.displayed.id=f.base.id=f.current->id=f.target.id;
  auto undo=f.replay(c,false);EXPECT_FALSE(undo.overrideAfter());
  EXPECT_TRUE(SameLightEditIdentity(c.target(),f.target));EXPECT_FALSE(c.target()==f.target);
}
TEST(LightEditReceipt, WrongSideAndAnotherCompletedReplayInvalidatePreparedPhase) {
  Fixture f;auto p=f.edit(20);const auto c=f.publish(p);EXPECT_THROW((void)f.replay(c,true),std::invalid_argument);
  auto stale=f.replay(c,false);auto accepted=f.replay(c,false);f.publish(accepted);
  EXPECT_FALSE(LightEditPlanMatches(stale,f.target,f.displayed,f.base,f.current));EXPECT_FALSE(PublishLightEditPlan(stale));
}
TEST(LightEditReceipt, ExactImmediateSourceAndOverrideProofRejectsChangedInputsWithoutAllocating) {
  Fixture f;auto p=f.edit(20);auto altered=f.base;altered.range=std::nextafter(altered.range,100.f);auto target=f.target;++target.revision;
  bool ready{},source{},revision{},done{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=LightEditPlanMatches(p,f.target,f.displayed,f.base,f.current);
    source=LightEditPlanMatches(p,f.target,f.displayed,altered,f.current);revision=LightEditPlanMatches(p,target,f.displayed,f.base,f.current);
    done=PublishLightEditPlan(p);attempts=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_FALSE(source);EXPECT_FALSE(revision);EXPECT_TRUE(done);EXPECT_EQ(attempts,0u);
}
TEST(LightEditReceipt, CurrentPreviewGuardAllowsSourceClockAdvanceButRejectsLegacyOverrideMutation) {
  Fixture f;auto p=f.edit(20);const auto c=f.publish(p);auto values=LightEditValuesFrom(*f.current);auto changed=values;changed.position.x=17;
  bool ready{},stale{},wrong{};size_t attempts{};auto old=f.target;++old.incarnation;
  {material_publication_test::AllocationGuard deny(true);ready=LightEditCommandPreviewMatches(c,f.target,values);
    stale=LightEditCommandPreviewMatches(c,f.target,changed);wrong=LightEditCommandPreviewMatches(c,old,values);attempts=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_FALSE(stale);EXPECT_FALSE(wrong);EXPECT_EQ(attempts,0u);
}
TEST(LightEditReceipt, DirectionalGeneratedAndManualRowsUseSameSelectedOverrideProtocol) {
  for(const auto source:{EditableLightSource::Generated,EditableLightSource::Imported,EditableLightSource::Manual}) {
    Fixture f;f.target.id={EditableLightType::Directional,source,0};f.base.id=f.target.id;f.base.type=EditableLightType::Directional;f.base.source=source;
    f.base.position={5,6,7};f.displayed=f.base;if(source==EditableLightSource::Manual) f.current=f.base;
    LightEditPatch patch;patch.fields=field(LightEditField::Direction)|field(LightEditField::Intensity);patch.values.direction={2,0,0};patch.values.intensity=12;
    auto p=PlanLightEdit(f.target,f.displayed,f.base,f.current,patch);EXPECT_EQ(p.overrideAfter()->direction,glm::vec3(1,0,0));
    const auto c=f.publish(p);auto undo=f.replay(c,false);EXPECT_EQ(bool(undo.overrideAfter()),source==EditableLightSource::Manual);
  }
}
TEST(LightEditReceipt, SpotConesAreOneCanonicalCoupledField) {
  Fixture f;f.target.id.type=EditableLightType::Spot;f.base.id=f.target.id;f.base.type=EditableLightType::Spot;f.displayed=f.base;
  LightEditPatch patch;patch.fields=field(LightEditField::SpotCone);patch.values.innerConeDegrees=70;patch.values.outerConeDegrees=20;
  auto p=PlanLightEdit(f.target,f.displayed,f.base,f.current,patch);ASSERT_TRUE(p.overrideAfter());
  EXPECT_NEAR(p.overrideAfter()->innerConeDegrees,70,2e-5);EXPECT_NEAR(p.overrideAfter()->outerConeDegrees,70,2e-5);
  EXPECT_EQ(p.command().ownedFields(),field(LightEditField::SpotCone));
}
TEST(LightEditReceipt, AreaBasisNormalizationAndDimensionsRemainSelectedAndBounded) {
  Fixture f;f.target.id.type=EditableLightType::Area;f.base.id=f.target.id;f.base.type=EditableLightType::Area;f.displayed=f.base;
  LightEditPatch patch;patch.fields=field(LightEditField::AreaBasis)|field(LightEditField::AreaSize);patch.values.direction={0,-2,0};patch.values.tangent={2,1,0};patch.values.areaHalfSize={2,3};
  auto p=PlanLightEdit(f.target,f.displayed,f.base,f.current,patch);const auto& e=*p.overrideAfter();
  EXPECT_EQ(e.direction,glm::vec3(0,-1,0));EXPECT_NEAR(glm::dot(e.direction,e.tangent),0,1e-6);EXPECT_EQ(e.areaHalfSize,glm::vec2(2,3));
  EXPECT_TRUE((p.command().ownedFields()&field(LightEditField::Direction))!=0);
}
TEST(LightEditReceipt, CanonicalSpotEndpointsRestoreOriginalCosineBitsWithoutDegreeRoundtrip) {
  Fixture f;f.target.id.type=EditableLightType::Spot;
  container::gpu::PointLightData point;point.positionRadius={3,2,1,10};point.colorIntensity={.25f,.5f,1,7};
  point.directionInnerCos={0,0,-1,std::nextafter(std::cos(.2f),0.f)};
  point.coneOuterCosType={std::nextafter(std::cos(.6f),0.f),container::gpu::kLightTypeSpot,3,1};
  f.base=editablePointLight(EditableLightSource::Imported,0,point,false);f.current=f.base;f.displayed=f.base;
  LightEditCanonicalPayload raw;raw.kind=LightEditCanonicalPayload::Kind::Point;raw.point=point;
  LightEditPatch patch;patch.fields=field(LightEditField::SpotCone);patch.values.innerConeDegrees=0;patch.values.outerConeDegrees=45;
  auto p=PlanLightEdit(f.target,f.displayed,f.base,f.current,patch,nullptr,{},&raw);const auto after=p.canonicalAfter();const auto command=f.publish(p);
  EXPECT_EQ(after.point.directionInnerCos.w,1);EXPECT_EQ(after.point.coneOuterCosType.z,3);EXPECT_EQ(after.point.coneOuterCosType.w,1);
  auto undo=RebaseLightEdit(f.target,f.displayed,f.base,f.current,command,false,{},&after);
  EXPECT_EQ(std::bit_cast<uint32_t>(undo.canonicalAfter().point.directionInnerCos.w),std::bit_cast<uint32_t>(point.directionInnerCos.w));
  EXPECT_EQ(std::bit_cast<uint32_t>(undo.canonicalAfter().point.coneOuterCosType.x),std::bit_cast<uint32_t>(point.coneOuterCosType.x));
}
TEST(LightEditReceipt, UnsupportedTypeFieldsNonfiniteAndStorageLimitsRejectBeforePublication) {
  Fixture f;for(const auto selected:{LightEditField::Direction,LightEditField::SpotCone,LightEditField::AreaSize}) {
    auto patch=f.intensity(9);patch.fields=field(selected);EXPECT_THROW((void)PlanLightEdit(f.target,f.displayed,f.base,f.current,patch),std::invalid_argument);
  }
  auto patch=f.intensity(std::numeric_limits<float>::quiet_NaN());EXPECT_THROW((void)PlanLightEdit(f.target,f.displayed,f.base,f.current,patch),std::invalid_argument);
  LightEditLimits limits;limits.maxCommandBytes=1;EXPECT_THROW((void)f.edit(9,nullptr,limits),std::invalid_argument);
  limits={};limits.maxLabelBytes=1;EXPECT_THROW((void)f.edit(9,nullptr,limits),std::invalid_argument);
  EXPECT_FALSE(f.current);EXPECT_EQ(f.displayed.intensity,7);
}
TEST(LightEditReceipt, EveryDeniedSelectedPreparationLeavesOldReceiptAndOverrideUntouched) {
  Fixture f;auto initial=f.edit(9);const auto command=f.publish(initial);size_t count{};
  {material_publication_test::AllocationGuard measure(false);auto p=f.edit(17,&command);measure.disable();count=measure.allocations();}
  ASSERT_GT(count,0u);ASSERT_LT(count,1000u);
  for(size_t allowance=0;allowance<count;++allowance) {
    bool denied{};{material_publication_test::AllocationGuard fail(false,allowance);try {(void)f.edit(17,&command);}catch(const std::bad_alloc&){denied=true;}}
    EXPECT_TRUE(denied);EXPECT_TRUE(command.applied());EXPECT_EQ(f.current->intensity,9);
    EXPECT_TRUE(LightEditCommandPreviewMatches(command,f.target,LightEditValuesFrom(*f.current)));
  }
}
} // namespace
