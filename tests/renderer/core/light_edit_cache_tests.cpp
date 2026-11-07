#include "../../../src/renderer/core/RendererLightCachePublication.h"
#include "../scene/material_publication_allocations.h"
#include <gtest/gtest.h>

namespace {
using namespace container::renderer;
using detail::LightCachePublication;
using material_publication_test::AllocationGuard;
using Source=container::scene::AuthoredSceneLights;
using Binding=container::scene::AuthoredLightBinding;
EditableLightId point() {return {EditableLightType::Point,EditableLightSource::Imported,0};}
Source source() {Source value;value.pointBindings={{7,{}}};return value;}
SceneLightEdits edits(float intensity) {
  SceneLightEdits value;value.importedPointOverrides.resize(1);value.importedPointOverrides[0].emplace();
  value.importedPointOverrides[0]->colorIntensity.w=intensity;return value;
}

TEST(LightEditCache, MissingGlobalUsesPreallocatedNodeAndOnlyActiveMirrorIsInvalidated) {
  std::shared_ptr<ImportedSceneLightEdits> global;LightCachePublication::Mirrors mirrors;
  auto active=std::make_shared<SceneLightEdits>(),other=std::make_shared<SceneLightEdits>();mirrors[0]=active;mirrors[1]=other;
  const auto before=edits(2),after=edits(9);const auto src=source();
  LightCachePublication prepared(global,mirrors,src,before,after,point(),0);
  ASSERT_FALSE(global);ASSERT_EQ(mirrors[0],active);
  AllocationGuard deny(true);const bool ready=prepared.matches(),published=prepared.exchange(),again=prepared.exchange();
  const auto allocations=deny.allocations();deny.disable();
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_FALSE(again);EXPECT_EQ(allocations,0u);
  ASSERT_TRUE(global);ASSERT_TRUE(global->points.at(src.pointBindings[0]));
  EXPECT_EQ(global->points.at(src.pointBindings[0])->colorIntensity.w,9);
  EXPECT_FALSE(mirrors.contains(0));EXPECT_EQ(mirrors.at(1),other);
}

TEST(LightEditCache, UnrelatedBindingChangesSurviveWhileSelectedStaleCellRejects) {
  auto global=std::make_shared<ImportedSceneLightEdits>();const auto src=source();const Binding unrelated{9,{}};
  global->points[src.pointBindings[0]]=edits(2).importedPointOverrides[0];global->points[unrelated]=edits(4).importedPointOverrides[0];
  LightCachePublication::Mirrors mirrors;const auto before=edits(2),after=edits(9);
  LightCachePublication prepared(global,mirrors,src,before,after,point(),0);
  global->points.at(unrelated)->colorIntensity.w=13;
  AllocationGuard deny(true);const bool published=prepared.exchange();const auto allocations=deny.allocations();deny.disable();
  EXPECT_TRUE(published);EXPECT_EQ(allocations,0u);EXPECT_EQ(global->points.at(unrelated)->colorIntensity.w,13);
  LightCachePublication stale(global,mirrors,src,after,before,point(),0);
  global->points.at(src.pointBindings[0])->positionRadius.x=-0.f;
  AllocationGuard denyAgain(true);const bool ready=stale.matches(),rejected=stale.exchange();
  const auto attempts=denyAgain.allocations();denyAgain.disable();
  EXPECT_FALSE(ready);EXPECT_FALSE(rejected);EXPECT_EQ(attempts,0u);
  EXPECT_EQ(std::bit_cast<uint32_t>(global->points.at(src.pointBindings[0])->positionRadius.x),0x80000000u);
}

TEST(LightEditCache, ReplacementGlobalOrMirrorRejectsWithoutRetargeting) {
  auto global=std::make_shared<ImportedSceneLightEdits>();const auto src=source();LightCachePublication::Mirrors mirrors;
  mirrors[0]=std::make_shared<SceneLightEdits>();const auto before=edits(2),after=edits(9);
  LightCachePublication changedMirror(global,mirrors,src,before,after,point(),0);
  mirrors[0]=std::make_shared<SceneLightEdits>();EXPECT_FALSE(changedMirror.exchange());
  LightCachePublication changedGlobal(global,mirrors,src,before,after,point(),0);
  global=std::make_shared<ImportedSceneLightEdits>();EXPECT_FALSE(changedGlobal.exchange());EXPECT_TRUE(global->points.empty());
}

TEST(LightEditCache, ClearingOverridePublishesAbsenceRatherThanAnOldSample) {
  auto global=std::make_shared<ImportedSceneLightEdits>();const auto src=source();LightCachePublication::Mirrors mirrors;
  const auto before=edits(8);SceneLightEdits after;after.importedPointOverrides.resize(1);
  global->points[src.pointBindings[0]]=before.importedPointOverrides[0];
  LightCachePublication prepared(global,mirrors,src,before,after,point(),0);ASSERT_TRUE(prepared.exchange());
  ASSERT_TRUE(global->points.contains(src.pointBindings[0]));EXPECT_FALSE(global->points.at(src.pointBindings[0]));
  EXPECT_FALSE(global->merge(src,after).importedPointOverrides[0]);
}

TEST(LightEditCache, ManualSunMasksAndPreservesTheOriginalImportedBindingOnly) {
  Source src;src.directionalBindings={{4,{}}};SceneLightEdits before,after;
  before.directionalOverride.emplace();before.directionalOverride->source=EditableLightSource::Imported;
  before.directionalOverride->id={EditableLightType::Directional,EditableLightSource::Imported,0};
  before.directionalOverride->intensity=17;after.directionalOverride.emplace();
  after.directionalOverride->source=EditableLightSource::Manual;
  auto global=std::make_shared<ImportedSceneLightEdits>();const Binding other{99,{}};global->directionals[other]=std::nullopt;
  LightCachePublication::Mirrors mirrors;
  LightCachePublication prepared(global,mirrors,src,before,after,{EditableLightType::Directional,EditableLightSource::Manual,0},0);
  AllocationGuard deny(true);const bool published=prepared.exchange();const auto attempts=deny.allocations();deny.disable();
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);ASSERT_TRUE(global->directionals.at(src.directionalBindings[0]));
  EXPECT_EQ(global->directionals.at(src.directionalBindings[0])->intensity,17);EXPECT_FALSE(global->directionals.at(other));
}

TEST(LightEditCache, LimitsAndEveryPreparationAllocationCutpointPreserveLiveCells) {
  const auto before=edits(2),after=edits(9);auto src=source();auto global=std::make_shared<ImportedSceneLightEdits>();
  LightCachePublication::Mirrors mirrors;src.pointBindings[0].extension=std::string(257,'x');
  EXPECT_THROW((void)LightCachePublication(global,mirrors,src,before,after,point(),0),std::length_error);
  src=source();EXPECT_THROW((void)LightCachePublication(global,mirrors,src,before,after,point(),0,1),std::length_error);
  size_t count{};{AllocationGuard measured(false);LightCachePublication prepared(global,mirrors,src,before,after,point(),0);count=measured.allocations();}
  ASSERT_GT(count,0u);
  for(size_t cutoff=0;cutoff<count;++cutoff) {
    bool failed{};
    {AllocationGuard denied(false,cutoff);try {LightCachePublication prepared(global,mirrors,src,before,after,point(),0);}
      catch(const std::bad_alloc&) {failed=true;}}
    EXPECT_TRUE(failed)<<cutoff;EXPECT_TRUE(global->points.empty());EXPECT_TRUE(mirrors.empty());
  }
}

TEST(LightEditCache, ExistingDirectionalLabelIsBoundedBeforeAnySelectedValueCopy) {
  Source src;src.directionalBindings={{4,{}}};SceneLightEdits before,after;
  after.directionalOverride.emplace();after.directionalOverride->source=EditableLightSource::Imported;
  auto global=std::make_shared<ImportedSceneLightEdits>();
  global->directionals[src.directionalBindings[0]].emplace();
  auto& prior=*global->directionals.at(src.directionalBindings[0]);prior.label=std::string(4097,'x');
  LightCachePublication::Mirrors mirrors;auto active=std::make_shared<SceneLightEdits>();mirrors[0]=active;
  for(const auto origin:{EditableLightSource::Imported,EditableLightSource::Manual}) {
    EXPECT_THROW((void)LightCachePublication(global,mirrors,src,before,after,
        {EditableLightType::Directional,origin,0},0),std::length_error);
    EXPECT_EQ(prior.label.size(),4097u);EXPECT_EQ(mirrors.at(0),active);
  }
  prior.label.resize(4096);after.directionalOverride->label=std::string(4096,'y');
  const auto budget=sizeof(LightCachePublication)+16384+3*after.directionalOverride->label.size();
  EXPECT_THROW((void)LightCachePublication(global,mirrors,src,before,after,
      {EditableLightType::Directional,EditableLightSource::Imported,0},0,budget),std::length_error);
  EXPECT_EQ(prior.label,std::string(4096,'x'));EXPECT_EQ(mirrors.at(0),active);
}
} // namespace
