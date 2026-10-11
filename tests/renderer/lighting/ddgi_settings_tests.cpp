#include "Container/renderer/lighting/DdgiSettings.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace container::renderer;

template <typename Function>
void expectInvalidContaining(Function function, const char *message) {
  try {
    function();
    FAIL() << "Expected invalid_argument containing: " << message;
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find(message), std::string::npos)
        << error.what();
  }
}

TEST(DdgiSettingsTests, DefaultIsOptionalBoundedAndIncludesLocalFloor) {
  const DdgiSettings settings;
  EXPECT_FALSE(settings.enabled);
  EXPECT_FALSE(settings.paused);
  EXPECT_EQ(settings.debugView, DdgiDebugView::Off);
  EXPECT_NO_THROW(validateDdgiSettings(settings));
  const DdgiProbeVolume volume(settings.volume);
  EXPECT_EQ(volume.probeCount(), 245u);
  EXPECT_FLOAT_EQ(volume.minimum().x, -1.5f);
  EXPECT_FLOAT_EQ(volume.minimum().y, -.1f);
  EXPECT_FLOAT_EQ(volume.minimum().z, -1.5f);
  EXPECT_FLOAT_EQ(volume.maximum().x, 1.5f);
  EXPECT_FLOAT_EQ(volume.maximum().y, 1.9f);
  EXPECT_FLOAT_EQ(volume.maximum().z, 1.5f);
  EXPECT_TRUE(volume.contains({0.f, 0.f, 0.f}));
  EXPECT_EQ(settings.resources.updates.maxProbesPerFrame, 128u);
  EXPECT_EQ(settings.resources.updates.raysPerProbe, 128u);
  EXPECT_EQ(settings.resources.updates.maxRaysPerFrame, 16384u);
  const auto plan = planDdgiResources(volume, settings.resources);
  EXPECT_LE(plan.payloadBytes, settings.payloadBudgetBytes);
  EXPECT_EQ(settings.surfaceBudgetBytes, 128ull * 1024 * 1024);
}

TEST(DdgiSettingsTests, PauseRetainsTheConfiguredResourceCapacity) {
  DdgiSettings settings;
  const auto before =
      planDdgiResources(DdgiProbeVolume(settings.volume), settings.resources);
  settings.paused = true;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
  const auto after =
      planDdgiResources(DdgiProbeVolume(settings.volume), settings.resources);
  EXPECT_EQ(after.rayScratchBytes, before.rayScratchBytes);
  EXPECT_EQ(after.updateCapacity.rayCount, before.updateCapacity.rayCount);
}

TEST(DdgiSettingsTests, DiagnosticsRejectInvalidEnumsAndNonpositiveRadius) {
  DdgiSettings settings;
  settings.debugView = static_cast<DdgiDebugView>(5u);
  expectInvalidContaining([&] { validateDdgiSettings(settings); }, "debugView");
  settings.debugView = DdgiDebugView::Classification;
  for (const float radius : {0.f, -1.f, std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN()}) {
    settings.debugProbeRadius = radius;
    expectInvalidContaining([&] { validateDdgiSettings(settings); },
                            "debugProbeRadius");
  }
  settings.debugProbeRadius = .1f;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
}

TEST(DdgiSettingsTests, RejectsEveryNonfiniteScalarEvenWhenDisabled) {
  constexpr std::array<float DdgiSettings::*, 6> fields{
      &DdgiSettings::maxDistance,    &DdgiSettings::hysteresis,
      &DdgiSettings::normalBias,     &DdgiSettings::viewBias,
      &DdgiSettings::visibilityBias, &DdgiSettings::intensity};
  for (const auto field : fields)
    for (const float value : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity()}) {
      DdgiSettings settings;
      settings.*field = value;
      EXPECT_THROW(validateDdgiSettings(settings), std::invalid_argument);
      settings.enabled = true;
      EXPECT_THROW(validateDdgiSettings(settings), std::invalid_argument);
    }
}

TEST(DdgiSettingsTests, TraceDistanceRequiresPositiveFiniteBound) {
  for (const float value :
       {0.f, -1.f,
        std::nextafter(kMaxDdgiTraceDistance,
                       std::numeric_limits<float>::infinity())}) {
    DdgiSettings settings;
    settings.maxDistance = value;
    expectInvalidContaining([&] { validateDdgiSettings(settings); },
                            "maxDistance");
  }
  DdgiSettings settings;
  settings.maxDistance = kMaxDdgiTraceDistance;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
  settings.maxDistance = std::numeric_limits<float>::min();
  settings.normalBias = settings.viewBias = settings.visibilityBias = 0.f;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
}

TEST(DdgiSettingsTests, HysteresisAndIntensityAcceptEndpointsAndRejectExcess) {
  DdgiSettings settings;
  settings.hysteresis = 0.f;
  settings.intensity = 0.f;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
  settings.hysteresis = .99f;
  settings.intensity = kMaxDdgiIntensity;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
  for (const float value : {-1.f, std::nextafter(.99f, 1.f)}) {
    settings = {};
    settings.hysteresis = value;
    expectInvalidContaining([&] { validateDdgiSettings(settings); },
                            "hysteresis");
  }
  for (const float value :
       {-1.f, std::nextafter(kMaxDdgiIntensity,
                             std::numeric_limits<float>::infinity())}) {
    settings = {};
    settings.intensity = value;
    expectInvalidContaining([&] { validateDdgiSettings(settings); },
                            "intensity");
  }
}

TEST(DdgiSettingsTests, EachWorldSpaceBiasMustFitTheTraceDistance) {
  constexpr std::array<float DdgiSettings::*, 3> fields{
      &DdgiSettings::normalBias, &DdgiSettings::viewBias,
      &DdgiSettings::visibilityBias};
  for (const auto field : fields) {
    DdgiSettings settings;
    settings.*field = 0.f;
    EXPECT_NO_THROW(validateDdgiSettings(settings));
    settings.*field = settings.maxDistance;
    EXPECT_NO_THROW(validateDdgiSettings(settings));
    for (const float value :
         {-1.f, std::nextafter(settings.maxDistance,
                               std::numeric_limits<float>::infinity())}) {
      settings.*field = value;
      EXPECT_THROW(validateDdgiSettings(settings), std::invalid_argument);
    }
  }
}

TEST(DdgiSettingsTests, PerProbeRayLimitsRejectZeroAndLargeLoops) {
  for (const uint32_t value :
       {0u, kMaxDdgiRaysPerProbe + 1u, std::numeric_limits<uint32_t>::max()}) {
    DdgiSettings settings;
    settings.resources.updates.raysPerProbe = value;
    expectInvalidContaining([&] { validateDdgiSettings(settings); },
                            "raysPerProbe");
  }
  for (const uint32_t value : {1u, kMaxDdgiRaysPerProbe}) {
    DdgiSettings settings;
    settings.resources.updates.raysPerProbe = value;
    EXPECT_NO_THROW(validateDdgiSettings(settings));
  }
}

TEST(DdgiSettingsTests,
     ZeroFrameBudgetsPauseWithoutRejectingValidConfiguration) {
  for (const auto budget :
       {DdgiUpdateBudget{0, 128, 16384}, DdgiUpdateBudget{128, 128, 0},
        DdgiUpdateBudget{128, 128, 127}}) {
    DdgiSettings settings;
    settings.enabled = true;
    settings.resources.updates = budget;
    EXPECT_NO_THROW(validateDdgiSettings(settings));
    const auto batch =
        planDdgiUpdates(DdgiProbeVolume(settings.volume), budget);
    EXPECT_EQ(batch.probeCount, 0u);
    EXPECT_EQ(batch.rayCount, 0u);
  }
}

TEST(DdgiSettingsTests, LatticeContractRejectsInvalidAndOverflowingInputs) {
  for (int axis = 0; axis < 3; ++axis) {
    DdgiSettings settings;
    settings.volume.counts[axis] = 0;
    expectInvalidContaining([&] { validateDdgiSettings(settings); }, "counts");
    settings = {};
    settings.volume.spacing[axis] = 0.f;
    expectInvalidContaining([&] { validateDdgiSettings(settings); }, "spacing");
    settings = {};
    settings.volume.origin[axis] = std::numeric_limits<float>::infinity();
    expectInvalidContaining([&] { validateDdgiSettings(settings); }, "origin");
  }
  DdgiSettings settings;
  settings.volume.counts = {65536, 65536, 1};
  expectInvalidContaining([&] { validateDdgiSettings(settings); }, "uint32");
  settings = {};
  settings.volume.origin.x = 1e20f;
  expectInvalidContaining([&] { validateDdgiSettings(settings); }, "rebase");
}

TEST(DdgiSettingsTests, ConfiguredPayloadBudgetRejectsOversizedPlansExactly) {
  DdgiSettings settings;
  const DdgiProbeVolume volume(settings.volume);
  const auto plan = planDdgiResources(volume, settings.resources);
  settings.payloadBudgetBytes = plan.payloadBytes;
  EXPECT_NO_THROW(validateDdgiSettings(settings));
  settings.payloadBudgetBytes = plan.payloadBytes - 1;
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "maxPayloadBytes");
  settings = {};
  settings.volume.counts = {65535, 65535, 1};
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "maxPayloadBytes");
}

TEST(DdgiSettingsTests,
     ResourceSettingsRejectUnsupportedTilesAndPayloadOverflow) {
  DdgiSettings settings;
  settings.resources.irradianceInteriorTexels = 1;
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "irradiance interior");
  settings = {};
  settings.resources.visibilityInteriorTexels = 0;
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "visibility interior");
  settings = {};
  settings.volume.counts = {2, 2, 1};
  settings.resources.irradianceInteriorTexels = 1000000000;
  settings.payloadBudgetBytes = std::numeric_limits<uint64_t>::max();
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "atlas payload overflow");
}

TEST(DdgiSettingsTests, ZeroMemoryBudgetsAreInvalidEvenWhenDisabled) {
  DdgiSettings settings;
  settings.payloadBudgetBytes = 0;
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "payloadBudgetBytes");
  settings = {};
  settings.surfaceBudgetBytes = 0;
  expectInvalidContaining([&] { validateDdgiSettings(settings); },
                          "surfaceBudgetBytes");
}

} // namespace
