#include "Container/app/SceneLightingDefaults.h"
#include "Container/app/RendererLightingSettings.h"

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <set>

namespace {
using container::app::SceneLightingDefaults;
using container::app::SceneLightingValues;
constexpr SceneLightingValues cornell{0.0f, 0.0f, 0.0f, false, 24};
constexpr SceneLightingValues viewer{};
} // namespace

TEST(SceneLightingDefaultsTests, CornellBimCornellRestoresAutomaticLighting) {
  SceneLightingDefaults state;
  auto values = state.apply(viewer, cornell);
  EXPECT_FLOAT_EQ(values.directionalIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.bounceIntensity, 0.0f);
  EXPECT_FALSE(values.bloomEnabled);
  EXPECT_EQ(values.localShadowLayerBudget, 24u);
  values = state.apply(values, viewer);
  EXPECT_FLOAT_EQ(values.directionalIntensity, 2.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 1.0f);
  EXPECT_FLOAT_EQ(values.bounceIntensity, 1.0f);
  EXPECT_TRUE(values.bloomEnabled);
  EXPECT_EQ(values.localShadowLayerBudget, 8u);
  values = state.apply(values, cornell);
  EXPECT_FLOAT_EQ(values.directionalIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.bounceIntensity, 0.0f);
  EXPECT_FALSE(values.bloomEnabled);
}

TEST(SceneLightingDefaultsTests, ExplicitZeroAndBloomOverridesSurviveReloads) {
  SceneLightingDefaults state;
  SceneLightingValues values{0.0f, 0.0f, 1.0f, true};
  values = state.apply(values, cornell, {true, true, true});
  EXPECT_TRUE(values.bloomEnabled);
  values = state.apply(values, viewer, {true, true, true});
  EXPECT_FLOAT_EQ(values.directionalIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 0.0f);
  EXPECT_TRUE(values.bloomEnabled);
}

TEST(SceneLightingDefaultsTests, UserEditPreservesOnlyTheEditedField) {
  SceneLightingDefaults state;
  auto values = state.apply(viewer, cornell);
  values.environmentIntensity = 0.4f;
  values = state.apply(values, viewer);
  EXPECT_FLOAT_EQ(values.directionalIntensity, 2.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 0.4f);
  values = state.apply(values, cornell);
  EXPECT_FLOAT_EQ(values.directionalIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 0.4f);
}

TEST(SceneLightingDefaultsTests, EditMatchingNextPresetStillRemainsUserOwned) {
  SceneLightingDefaults state;
  auto values = state.apply(viewer, cornell);
  values.directionalIntensity = viewer.directionalIntensity;
  values = state.apply(values, viewer);
  values = state.apply(values, cornell);
  EXPECT_FLOAT_EQ(values.directionalIntensity, viewer.directionalIntensity);
}

TEST(SceneLightingDefaultsTests, UserCanKeepLightsOffAndEnableArtisticFill) {
  SceneLightingDefaults state;
  auto values = state.apply(viewer, viewer);
  values.directionalIntensity = 0.0f;
  values.environmentIntensity = 0.0f;
  values.bounceIntensity = 0.5f;
  values.bloomEnabled = false;
  values = state.apply(values, cornell);
  values = state.apply(values, viewer);
  EXPECT_FLOAT_EQ(values.directionalIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.environmentIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.bounceIntensity, 0.5f);
  EXPECT_FALSE(values.bloomEnabled);
}

TEST(SceneLightingDefaultsTests, ExplicitBounceIntensitySurvivesScenePresetChanges) {
  for (const float requested : {0.0f, 1.0f, 2.0f}) {
    SCOPED_TRACE(requested);
    SceneLightingDefaults state;
    auto values = viewer;
    values.bounceIntensity = requested;
    const container::app::SceneLightingOverrides overrides{.bounceIntensity = true};
    values = state.apply(values, cornell, overrides);
    EXPECT_FLOAT_EQ(values.bounceIntensity, requested);
    EXPECT_FLOAT_EQ(values.directionalIntensity, 0.0f);
    EXPECT_FLOAT_EQ(values.environmentIntensity, 0.0f);
    values = state.apply(values, viewer, overrides);
    EXPECT_FLOAT_EQ(values.bounceIntensity, requested);
    EXPECT_FLOAT_EQ(values.directionalIntensity, 2.0f);
    EXPECT_FLOAT_EQ(values.environmentIntensity, 1.0f);
    values = state.apply(values, cornell, overrides);
    EXPECT_FLOAT_EQ(values.bounceIntensity, requested);
  }
}

TEST(SceneLightingDefaultsTests, ExplicitZeroBounceRemainsOwnedWhenItMatchesCurrentAutomaticValue) {
  SceneLightingDefaults state;
  auto values = state.apply(viewer, cornell);
  ASSERT_FLOAT_EQ(values.bounceIntensity, 0.0f);
  values = state.apply(values, cornell, {.bounceIntensity = true});
  values = state.apply(values, viewer);
  EXPECT_FLOAT_EQ(values.bounceIntensity, 0.0f);
  EXPECT_FLOAT_EQ(values.directionalIntensity, viewer.directionalIntensity);
  EXPECT_FLOAT_EQ(values.environmentIntensity, viewer.environmentIntensity);
}

TEST(SceneLightingDefaultsTests, MixedProviderStartupUsesViewerIllumination) {
  SceneLightingDefaults state;
  const auto values = state.apply(viewer, viewer);
  EXPECT_GT(values.directionalIntensity, 0.0f);
  EXPECT_GT(values.environmentIntensity, 0.0f);
}

TEST(SceneLightingDefaultsTests, UserShadowBudgetSurvivesPresetChanges) {
  SceneLightingDefaults state;
  auto values = state.apply(viewer, cornell);
  values.localShadowLayerBudget = 12;
  values = state.apply(values, viewer);
  values = state.apply(values, cornell);
  EXPECT_EQ(values.localShadowLayerBudget, 12u);
}

TEST(RendererLightingSettings, SharedMetadataRetainsExistingNamesOptionsAndRanges) {
  using namespace container::app;
  ASSERT_EQ(kRendererLightingSettings.size(),4u);
  const std::array names{"environmentIntensity","directionalIntensity","bounceIntensity","environmentYaw"};
  const std::array options{"--environment-intensity","--directional-intensity","--bounce-intensity","--environment-yaw"};
  const std::array minimum{0.f,0.f,0.f,-180.f},maximum{16.f,16.f,2.f,180.f};
  std::set<std::string_view> unique;
  for(size_t i=0;i<4;++i) {
    const auto& value=kRendererLightingSettings[i];EXPECT_TRUE(unique.insert(value.name).second);
    EXPECT_EQ(value.name,names[i]);EXPECT_EQ(value.cliOption,options[i]);
    EXPECT_EQ(value.minimum,minimum[i]);EXPECT_EQ(value.maximum,maximum[i]);
    EXPECT_EQ(&rendererLightingSetting(value.setting),&value);
  }
}

TEST(RendererLightingSettings, ExplicitValidationChecksDoubleEndpointsBeforeNarrowing) {
  using namespace container::app;
  for(const auto& value:kRendererLightingSettings) {
    EXPECT_TRUE(validRendererLightingValue(value.setting,value.minimum));
    EXPECT_TRUE(validRendererLightingValue(value.setting,value.maximum));
    EXPECT_FALSE(validRendererLightingValue(value.setting,std::nextafter(double(value.minimum),-INFINITY)));
    EXPECT_FALSE(validRendererLightingValue(value.setting,std::nextafter(double(value.maximum),INFINITY)));
    for(const double invalid:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
      EXPECT_FALSE(validRendererLightingValue(value.setting,invalid));
  }
  EXPECT_THROW((void)parseRendererLightingValue(RendererLightingSetting::EnvironmentIntensity,"16.0000001"),std::invalid_argument);
  EXPECT_THROW((void)parseRendererLightingValue(RendererLightingSetting::BounceIntensity,"2.0000001"),std::invalid_argument);
  EXPECT_THROW((void)parseRendererLightingValue(RendererLightingSetting::EnvironmentYaw,"180.000001"),std::invalid_argument);
}

TEST(RendererLightingSettings, CliParsingPreservesFiniteBoundaryValuesAndRejectsInvalidWholeTokens) {
  using namespace container::app;
  EXPECT_FLOAT_EQ(parseRendererLightingValue(RendererLightingSetting::EnvironmentIntensity,"16"),16.f);
  EXPECT_FLOAT_EQ(parseRendererLightingValue(RendererLightingSetting::DirectionalIntensity,"+0.5"),.5f);
  EXPECT_FLOAT_EQ(parseRendererLightingValue(RendererLightingSetting::BounceIntensity,"2e0"),2.f);
  EXPECT_FLOAT_EQ(parseRendererLightingValue(RendererLightingSetting::EnvironmentYaw,"-180"),-180.f);
  EXPECT_FLOAT_EQ(parseRendererLightingValue(RendererLightingSetting::EnvironmentYaw,"180"),180.f);
  for(const auto* text:{"","nan","inf","-inf","0.5junk","1e5000","-0.00001","17"})
    EXPECT_THROW((void)parseRendererLightingValue(RendererLightingSetting::EnvironmentIntensity,text),std::invalid_argument)<<text;
  EXPECT_THROW((void)parseRendererLightingValue(RendererLightingSetting::BounceIntensity,"2.01"),std::invalid_argument);
}

TEST(RendererLightingSettings, ProgrammaticDefenseClampsFiniteAndNonfiniteValuesWithoutChangingValidValues) {
  using namespace container::app;
  for(const auto& value:kRendererLightingSettings) {
    EXPECT_FLOAT_EQ(clampRendererLightingValue(value.setting,value.minimum-1),value.minimum);
    EXPECT_FLOAT_EQ(clampRendererLightingValue(value.setting,value.maximum+1),value.maximum);
    const float middle=(value.minimum+value.maximum)*.5f;
    EXPECT_FLOAT_EQ(clampRendererLightingValue(value.setting,middle),middle);
    for(const float invalid:{std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
      EXPECT_FLOAT_EQ(clampRendererLightingValue(value.setting,invalid),0.f);
  }
}
