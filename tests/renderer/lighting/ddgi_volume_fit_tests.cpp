#include "Container/renderer/lighting/DdgiVolumeFit.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace container::renderer;

template <typename Function>
void expectInvalidContaining(Function function, const char *message) {
  try {
    (void)function();
    FAIL() << "Expected invalid_argument containing: " << message;
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find(message), std::string::npos)
        << error.what();
  }
}

uint64_t plannedPayload(const DdgiSettings &settings) {
  const DdgiResourceLimits limits{std::numeric_limits<uint32_t>::max(),
                                  std::numeric_limits<uint64_t>::max(),
                                  settings.payloadBudgetBytes};
  return planDdgiVolumeResources(settings, limits)
      .payloadBytes;
}

void expectCoverage(const DdgiVolumeFitResult &fit, glm::vec3 minimum,
                    glm::vec3 maximum, uint32_t cap) {
  const auto &settings = fit.settings;
  const DdgiProbeVolume volume(settings.volume);
  const float cell = settings.volume.spacing.x;
  EXPECT_TRUE(std::isfinite(cell));
  EXPECT_GE(cell, 1e-3f);
  EXPECT_FLOAT_EQ(settings.volume.spacing.y, cell);
  EXPECT_FLOAT_EQ(settings.volume.spacing.z, cell);
  double diagonalSquared = 0;
  for (int axis = 0; axis < 3; ++axis) {
    EXPECT_GE(settings.volume.counts[axis], 3u);
    EXPECT_FLOAT_EQ(fit.boundsMin[axis], volume.minimum()[axis]);
    EXPECT_FLOAT_EQ(fit.boundsMax[axis], volume.maximum()[axis]);
    // Flat-axis centering can move by half of the conservative float grid;
    // nonzero axes retain a full cell beyond both original endpoints.
    const double margin = minimum[axis] == maximum[axis] ? .5 * cell : cell;
    EXPECT_GE(double(minimum[axis]) - fit.boundsMin[axis], margin);
    EXPECT_GE(double(fit.boundsMax[axis]) - maximum[axis], margin);
    const double length = double(fit.boundsMax[axis]) - fit.boundsMin[axis];
    diagonalSquared += length * length;
  }
  for (uint32_t corner = 0; corner < 8; ++corner) {
    glm::vec3 position;
    for (int axis = 0; axis < 3; ++axis)
      position[axis] = (corner & (1u << axis)) ? maximum[axis] : minimum[axis];
    EXPECT_TRUE(volume.contains(position));
    EXPECT_TRUE(volume.interpolate(position).has_value());
  }
  EXPECT_EQ(fit.probeCount, volume.probeCount());
  EXPECT_LE(fit.probeCount, cap);
  EXPECT_EQ(fit.payloadBytes, plannedPayload(settings));
  EXPECT_LE(fit.payloadBytes, settings.payloadBudgetBytes);
  const double diagonal = std::sqrt(diagonalSquared);
  EXPECT_GE(double(settings.maxDistance), diagonal);
  EXPECT_LT(double(std::nextafter(settings.maxDistance, 0.f)), diagonal);
  EXPECT_LE(settings.maxDistance, kMaxDdgiTraceDistance);
  EXPECT_FLOAT_EQ(settings.normalBias, .06f * cell);
  EXPECT_FLOAT_EQ(settings.viewBias, .02f * cell);
  EXPECT_FLOAT_EQ(settings.visibilityBias, .1f * cell);
  EXPECT_FLOAT_EQ(settings.debugProbeRadius, .12f * cell);
  EXPECT_NO_THROW(validateDdgiSettings(settings));
}

TEST(DdgiVolumeFitTests, FitsPaddedSceneAndReportsExactResourcePayload) {
  const DdgiSettings settings;
  const glm::vec3 minimum{-2.f, 0.f, -3.f}, maximum{2.f, 5.f, 5.f};
  const auto fit = fitDdgiVolume(settings, minimum, maximum);
  expectCoverage(fit, minimum, maximum, 2048);
  EXPECT_FALSE(fit.settings.enabled);
  EXPECT_GT(fit.probeCount, 1000u);
}

TEST(DdgiVolumeFitTests, SceneFitPreservesAdditionalVolumesAndSharesPayloadCap) {
  DdgiSettings settings;
  settings.resources.updates={16,64,1024};
  settings.additionalVolumes.push_back({{-.25f,0,-.25f},{.25f,.25f,.25f},{5,5,5}});
  const auto unrestricted=fitDdgiVolume(settings,{0,0,0},{1,1,1},{2048});
  EXPECT_EQ(unrestricted.settings.additionalVolumes,settings.additionalVolumes);
  EXPECT_GT(unrestricted.payloadBytes,planDdgiResources(
      DdgiProbeVolume(unrestricted.settings.volume),settings.resources).payloadBytes);
  auto restricted=settings;
  restricted.payloadBudgetBytes=unrestricted.payloadBytes/2;
  const auto fit=fitDdgiVolume(restricted,{0,0,0},{1,1,1},{2048});
  expectCoverage(fit,{0,0,0},{1,1,1},2048);
  EXPECT_EQ(fit.settings.additionalVolumes,settings.additionalVolumes);
  EXPECT_LT(fit.probeCount,unrestricted.probeCount);
  EXPECT_LE(fit.payloadBytes,restricted.payloadBudgetBytes);
}

TEST(DdgiVolumeFitTests, AdditionalVolumeCannotHideAnImpossibleCombinedBudget) {
  DdgiSettings settings;
  settings.additionalVolumes.push_back({{0,0,0},{1,1,1},{10,10,10}});
  settings.payloadBudgetBytes=500000;
  EXPECT_THROW(fitDdgiVolume(settings,{0,0,0},{1,1,1}),std::invalid_argument);
}

TEST(DdgiVolumeFitTests, CapChoosesTheFinestPaddedCubeTopology) {
  const auto fit = fitDdgiVolume({}, glm::vec3(0.f), glm::vec3(1.f), {125});
  expectCoverage(fit, glm::vec3(0.f), glm::vec3(1.f), 125);
  EXPECT_EQ(fit.settings.volume.counts, glm::uvec3(5u));
  EXPECT_EQ(fit.probeCount, 125u);
  // Five nodes cover the one-unit extent plus two outside cells, requiring
  // h >= 1/2. Any finer spacing would need at least 6^3 probes.
  EXPECT_FLOAT_EQ(fit.settings.volume.spacing.x, .5f);
  EXPECT_EQ(fit.boundsMin, glm::vec3(-.5f));
  EXPECT_EQ(fit.boundsMax, glm::vec3(1.5f));
}

TEST(DdgiVolumeFitTests,
     CoarserCountTransitionCanRestoreSupportedTraceDistance) {
  const glm::vec3 minimum{0.f};
  const glm::vec3 maximum{600077.125f, 486221.15625f, 176561.4375f};
  const auto fit = fitDdgiVolume({}, minimum, maximum, {1283});
  expectCoverage(fit, minimum, maximum, 1283);
  // The finest count/payload fit has 15x12x7 nodes at spacing 54025, whose
  // padded diagonal exceeds 1e6. The first useful topology transition drops
  // X to 14 nodes and brings the diagonal below the supported trace limit.
  EXPECT_FLOAT_EQ(fit.settings.volume.spacing.x, 54552.5f);
  EXPECT_EQ(fit.settings.volume.counts, glm::uvec3(14u, 12u, 7u));
  EXPECT_EQ(fit.probeCount, 1176u);
  EXPECT_LT(fit.settings.maxDistance, 985000.f);
}

TEST(DdgiVolumeFitTests, FitScalesTransportAndKeepsEquivalentTopology) {
  const glm::vec3 minimum{-2.f, -1.f, -3.f}, maximum{2.f, 4.f, 5.f};
  const auto small = fitDdgiVolume({}, minimum, maximum);
  const auto large = fitDdgiVolume({}, minimum * 1000.f, maximum * 1000.f);
  expectCoverage(large, minimum * 1000.f, maximum * 1000.f, 2048);
  EXPECT_EQ(small.settings.volume.counts, large.settings.volume.counts);
  for (const auto field :
       {&DdgiSettings::maxDistance, &DdgiSettings::normalBias,
        &DdgiSettings::viewBias, &DdgiSettings::visibilityBias,
        &DdgiSettings::debugProbeRadius})
    EXPECT_NEAR(large.settings.*field, 1000.f * (small.settings.*field),
                1e-5f * (large.settings.*field));
  EXPECT_NEAR(large.settings.volume.spacing.x,
              small.settings.volume.spacing.x * 1000.f,
              large.settings.volume.spacing.x * 1e-5f);
}

TEST(DdgiVolumeFitTests, HandlesLongFlatAndPointBoundsWithoutCollapsedAxes) {
  const std::array<std::pair<glm::vec3, glm::vec3>, 3> cases{{
      {{-500.f, .25f, .25f}, {500.f, .25f, .25f}},
      {{-10.f, 0.f, -4.f}, {10.f, 0.f, 4.f}},
      {{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}},
  }};
  for (const auto &[minimum, maximum] : cases) {
    const auto fit = fitDdgiVolume({}, minimum, maximum, {1024});
    expectCoverage(fit, minimum, maximum, 1024);
    for (int axis = 0; axis < 3; ++axis)
      if (minimum[axis] == maximum[axis])
        EXPECT_EQ(fit.settings.volume.counts[axis], 3u);
  }
  const auto point = fitDdgiVolume({}, glm::vec3(0.f), glm::vec3(0.f), {27});
  EXPECT_EQ(point.probeCount, 27u);
  EXPECT_LE(point.settings.volume.spacing.x,
            std::nextafter(1e-3f, std::numeric_limits<float>::infinity()));
}

TEST(DdgiVolumeFitTests, VerySmallSceneUsesPracticalCellFloor) {
  const glm::vec3 minimum{0.f}, maximum{1e-7f, 0.f, 0.f};
  const auto fit = fitDdgiVolume({}, minimum, maximum);
  expectCoverage(fit, minimum, maximum, 2048);
  EXPECT_EQ(fit.settings.volume.counts, glm::uvec3(4u, 3u, 3u));
  EXPECT_LT(fit.settings.volume.spacing.x, .00101f);
}

TEST(DdgiVolumeFitTests, LargeBimCoordinatesKeepEveryAxisNodeDistinct) {
  const glm::vec3 minimum{100000000.f, -100000000.f, 100000000.f};
  const glm::vec3 maximum = minimum + glm::vec3(16.f, 32.f, 8.f);
  const auto fit = fitDdgiVolume({}, minimum, maximum, {512});
  expectCoverage(fit, minimum, maximum, 512);
  EXPECT_GE(fit.settings.volume.spacing.x, 8.f);
  const DdgiProbeVolume volume(fit.settings.volume);
  for (int axis = 0; axis < 3; ++axis) {
    glm::uvec3 coordinate{0u};
    float previous = volume.minimum()[axis];
    for (uint32_t i = 1; i < fit.settings.volume.counts[axis]; ++i) {
      coordinate[axis] = i;
      const glm::vec3 position =
          volume.probePosition(volume.linearIndex(coordinate));
      EXPECT_GT(position[axis], previous);
      const auto sample = volume.interpolate(position);
      ASSERT_TRUE(sample.has_value());
      EXPECT_EQ(sample->probeIndices[0], volume.linearIndex(coordinate));
      EXPECT_FLOAT_EQ(sample->weights[0], 1.f);
      previous = position[axis];
    }
  }
}

TEST(DdgiVolumeFitTests,
     FlatAxisAtFloatExponentBoundaryStillHasInteriorCoverage) {
  const float value = std::nextafter(131072.f, 0.f);
  const glm::vec3 point{value};
  const auto fit = fitDdgiVolume({}, point, point, {27});
  expectCoverage(fit, point, point, 27);
  EXPECT_EQ(fit.probeCount, 27u);
  EXPECT_GE(fit.settings.volume.spacing.x, .015625f);
}

TEST(DdgiVolumeFitTests,
     PayloadBudgetCoarsensTheFitInsteadOfRejectingOldVolume) {
  DdgiSettings settings;
  const auto unrestricted =
      fitDdgiVolume(settings, glm::vec3(0.f), glm::vec3(1.f));
  settings.payloadBudgetBytes = 600000;
  EXPECT_THROW(validateDdgiSettings(settings), std::invalid_argument);
  const auto limited = fitDdgiVolume(settings, glm::vec3(0.f), glm::vec3(1.f));
  expectCoverage(limited, glm::vec3(0.f), glm::vec3(1.f), 2048);
  EXPECT_EQ(limited.settings.volume.counts, glm::uvec3(4u));
  EXPECT_LT(limited.probeCount, unrestricted.probeCount);
  EXPECT_GT(limited.settings.volume.spacing.x,
            unrestricted.settings.volume.spacing.x);
  EXPECT_EQ(limited.settings.payloadBudgetBytes, 600000u);
}

TEST(DdgiVolumeFitTests,
     PreservesLiveControlsResourcesAndBudgetsWithoutMutation) {
  DdgiSettings current;
  current.enabled = true;
  current.paused = true;
  current.hysteresis = .8f;
  current.intensity = 2.5f;
  current.debugView = DdgiDebugView::Relocation;
  current.resources.irradianceInteriorTexels = 8;
  current.resources.visibilityInteriorTexels = 12;
  current.resources.updates = {17, 64, 1088};
  current.payloadBudgetBytes = 8ull * 1024 * 1024;
  current.surfaceBudgetBytes = 9ull * 1024 * 1024;
  const DdgiSettings before = current;
  const glm::vec3 minimum{-4.f, -2.f, -1.f}, maximum{2.f, 1.f, 5.f};
  const auto fit = fitDdgiVolume(current, minimum, maximum, {256});
  expectCoverage(fit, minimum, maximum, 256);
  DdgiSettings expected = before;
  expected.volume = fit.settings.volume;
  expected.maxDistance = fit.settings.maxDistance;
  expected.normalBias = fit.settings.normalBias;
  expected.viewBias = fit.settings.viewBias;
  expected.visibilityBias = fit.settings.visibilityBias;
  expected.debugProbeRadius = fit.settings.debugProbeRadius;
  EXPECT_EQ(fit.settings, expected);
  EXPECT_EQ(current, before);
}

TEST(DdgiVolumeFitTests, RecomputedLatticeAndScaleDoNotBlockAValidFit) {
  DdgiSettings settings;
  settings.volume.spacing = glm::vec3(0.f);
  settings.maxDistance = std::numeric_limits<float>::quiet_NaN();
  settings.normalBias = -1.f;
  settings.viewBias = std::numeric_limits<float>::infinity();
  settings.visibilityBias = -1.f;
  settings.debugProbeRadius = 0.f;
  EXPECT_NO_THROW(
      (void)fitDdgiVolume(settings, glm::vec3(-1.f), glm::vec3(1.f)));
}

TEST(DdgiVolumeFitTests, RejectsNonfiniteAndReversedBoundsOnEveryAxis) {
  for (int axis = 0; axis < 3; ++axis) {
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()}) {
      glm::vec3 minimum{0.f}, maximum{1.f};
      minimum[axis] = invalid;
      expectInvalidContaining(
          [&] { (void)fitDdgiVolume({}, minimum, maximum); }, "bounds");
      minimum = glm::vec3(0.f);
      maximum[axis] = invalid;
      expectInvalidContaining(
          [&] { (void)fitDdgiVolume({}, minimum, maximum); }, "bounds");
    }
    glm::vec3 minimum{0.f}, maximum{1.f};
    minimum[axis] = 2.f;
    expectInvalidContaining([&] { (void)fitDdgiVolume({}, minimum, maximum); },
                            "ordered");
  }
}

TEST(DdgiVolumeFitTests, RejectsCapsThatCannotSupplyOutsidePadding) {
  for (const uint32_t cap : {0u, 1u, 26u})
    expectInvalidContaining(
        [&] { (void)fitDdgiVolume({}, glm::vec3(0.f), glm::vec3(0.f), {cap}); },
        "at least 27");
  expectInvalidContaining(
      [&] { (void)fitDdgiVolume({}, glm::vec3(0.f), glm::vec3(1.f), {63}); },
      "at least 64");
  EXPECT_NO_THROW(
      (void)fitDdgiVolume({}, glm::vec3(0.f), glm::vec3(1.f), {64}));
}

TEST(DdgiVolumeFitTests, FailureLeavesSettingsIntactAndReportsTheBudgetCause) {
  DdgiSettings settings;
  settings.payloadBudgetBytes = 1024;
  const DdgiSettings before = settings;
  expectInvalidContaining(
      [&] { (void)fitDdgiVolume(settings, glm::vec3(-1.f), glm::vec3(1.f)); },
      "payload");
  EXPECT_EQ(settings, before);
  settings = {};
  settings.resources.irradianceInteriorTexels = 1;
  expectInvalidContaining(
      [&] { (void)fitDdgiVolume(settings, glm::vec3(-1.f), glm::vec3(1.f)); },
      "interior texels");
  settings = {};
  settings.hysteresis = 1.f;
  expectInvalidContaining(
      [&] { (void)fitDdgiVolume(settings, glm::vec3(-1.f), glm::vec3(1.f)); },
      "hysteresis");
}

TEST(DdgiVolumeFitTests, PausedOrZeroUpdateBudgetRetainsAValidPlannedVolume) {
  DdgiSettings settings;
  settings.paused = true;
  settings.resources.updates.maxRaysPerFrame = 0;
  const auto fit = fitDdgiVolume(settings, glm::vec3(-1.f), glm::vec3(1.f));
  expectCoverage(fit, glm::vec3(-1.f), glm::vec3(1.f), 2048);
  const auto updates = planDdgiUpdates(DdgiProbeVolume(fit.settings.volume),
                                       fit.settings.resources.updates);
  EXPECT_EQ(updates.probeCount, 0u);
  EXPECT_TRUE(fit.settings.paused);
}

TEST(DdgiVolumeFitTests,
     UnsupportedDistanceAndCoordinatePrecisionHaveDiagnostics) {
  expectInvalidContaining(
      [&] { (void)fitDdgiVolume({}, glm::vec3(0.f), {1000000.f, 0.f, 0.f}); },
      "smaller bounds");
  expectInvalidContaining(
      [&] {
        (void)fitDdgiVolume({}, glm::vec3(0.f), {400000.f, 0.f, 0.f}, {36});
      },
      "increase maxProbes");
  expectInvalidContaining(
      [&] {
        (void)fitDdgiVolume({}, glm::vec3(std::numeric_limits<float>::max()),
                            glm::vec3(std::numeric_limits<float>::max()));
      },
      "rebase");
  expectInvalidContaining(
      [&] {
        (void)fitDdgiVolume({}, glm::vec3(-std::numeric_limits<float>::max()),
                            glm::vec3(std::numeric_limits<float>::max()));
      },
      "smaller bounds");
}

} // namespace
