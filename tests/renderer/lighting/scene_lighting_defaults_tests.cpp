#include "Container/app/SceneLightingDefaults.h"

#include <gtest/gtest.h>

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
