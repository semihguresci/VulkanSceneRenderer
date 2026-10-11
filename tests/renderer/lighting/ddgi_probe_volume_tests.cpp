#include "Container/renderer/lighting/DdgiProbeVolume.h"

#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace {
using namespace container::renderer;

DdgiProbeVolume nonCubicVolume() {
  return DdgiProbeVolume({{-2.f, 10.f, -8.f}, {2.f, 4.f, 6.f}, {3, 2, 4}});
}

void expectVec3(glm::vec3 actual, glm::vec3 expected) {
  EXPECT_FLOAT_EQ(actual.x, expected.x);
  EXPECT_FLOAT_EQ(actual.y, expected.y);
  EXPECT_FLOAT_EQ(actual.z, expected.z);
}

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

TEST(DdgiProbeVolumeTests, InclusiveBoundsAndXFastIndexingAreWorldSpace) {
  const auto volume = nonCubicVolume();
  EXPECT_EQ(volume.probeCount(), 24u);
  expectVec3(volume.minimum(), {-2, 10, -8});
  expectVec3(volume.maximum(), {2, 14, 10});
  EXPECT_EQ(volume.linearIndex({0, 0, 0}), 0u);
  EXPECT_EQ(volume.linearIndex({2, 0, 0}), 2u);
  EXPECT_EQ(volume.linearIndex({0, 1, 0}), 3u);
  EXPECT_EQ(volume.linearIndex({0, 0, 1}), 6u);
  EXPECT_EQ(volume.linearIndex({2, 1, 2}), 17u);
  EXPECT_EQ(volume.linearIndex({2, 1, 3}), 23u);
  const auto coordinates = volume.coordinates(17);
  EXPECT_EQ(coordinates.x, 2u);
  EXPECT_EQ(coordinates.y, 1u);
  EXPECT_EQ(coordinates.z, 2u);
  expectVec3(volume.probePosition(17), {2, 14, 4});
  expectVec3(volume.probePosition(23), volume.maximum());
  for (uint32_t index = 0; index < volume.probeCount(); ++index)
    EXPECT_EQ(volume.linearIndex(volume.coordinates(index)), index);
}

TEST(DdgiProbeVolumeTests, RejectsCoordinatesAndLinearIndicesOutsideVolume) {
  const auto volume = nonCubicVolume();
  for (const glm::uvec3 invalid :
       {glm::uvec3(3, 0, 0),
        {0, 2, 0},
        {0, 0, 4},
        {std::numeric_limits<uint32_t>::max(), 0, 0}})
    EXPECT_THROW((void)volume.linearIndex(invalid), std::out_of_range);
  for (const uint32_t invalid : {24u, std::numeric_limits<uint32_t>::max()}) {
    EXPECT_THROW((void)volume.coordinates(invalid), std::out_of_range);
    EXPECT_THROW((void)volume.probePosition(invalid), std::out_of_range);
  }
}

TEST(DdgiProbeVolumeTests, TrilinearNeighborsAndWeightsMatchKnownNonCubicCell) {
  const auto sample = nonCubicVolume().interpolate({1, 11, 2.5f});
  ASSERT_TRUE(sample);
  EXPECT_FALSE(sample->clamped);
  const std::array<uint32_t, 8> expectedIndices{7, 8, 10, 11, 13, 14, 16, 17};
  const std::array<float, 8> expectedWeights{
      .09375f, .09375f, .03125f, .03125f, .28125f, .28125f, .09375f, .09375f};
  EXPECT_EQ(sample->probeIndices, expectedIndices);
  EXPECT_EQ(sample->weights, expectedWeights);
  EXPECT_FLOAT_EQ(
      std::accumulate(sample->weights.begin(), sample->weights.end(), 0.f),
      1.f);
}

TEST(DdgiProbeVolumeTests, CubeCenterProducesEightEqualContributions) {
  const DdgiProbeVolume volume({{0, 0, 0}, {2, 2, 2}, {2, 2, 2}});
  const auto sample = volume.interpolate({1, 1, 1});
  ASSERT_TRUE(sample);
  for (uint32_t corner = 0; corner < 8; ++corner) {
    EXPECT_EQ(sample->probeIndices[corner], corner);
    EXPECT_FLOAT_EQ(sample->weights[corner], .125f);
  }
}

TEST(DdgiProbeVolumeTests, ExactEndpointsAndInteriorProbeSelectOneProbe) {
  const auto volume = nonCubicVolume();
  for (const uint32_t index : {0u, 7u, 23u}) {
    const auto sample = volume.interpolate(volume.probePosition(index));
    ASSERT_TRUE(sample);
    float selectedWeight = 0;
    for (uint32_t corner = 0; corner < 8; ++corner) {
      if (sample->probeIndices[corner] == index)
        selectedWeight += sample->weights[corner];
      else
        EXPECT_FLOAT_EQ(sample->weights[corner], 0.f);
    }
    EXPECT_FLOAT_EQ(selectedWeight, 1.f);
  }
  // The mathematical endpoint is not exactly representable as float.
  const DdgiProbeVolume fractional(
      {{.1f, -.3f, .7f}, {.2f, .4f, .3f}, {4, 3, 2}});
  const auto endpoint = fractional.interpolate(fractional.maximum());
  ASSERT_TRUE(endpoint);
  EXPECT_EQ(endpoint->probeIndices[0], 23u);
  EXPECT_FLOAT_EQ(endpoint->weights[0], 1.f);
}

TEST(DdgiProbeVolumeTests, OutOfVolumeIsRejectedUnlessClampIsExplicit) {
  const auto volume = nonCubicVolume();
  for (const glm::vec3 outside :
       {glm::vec3(-2.01f, 10, -8), {2, 14.01f, 10}, {0, 12, 10.01f}}) {
    EXPECT_FALSE(volume.contains(outside));
    EXPECT_FALSE(volume.interpolate(outside));
    const auto sample = volume.interpolate(outside, DdgiOutsideVolume::Clamp);
    ASSERT_TRUE(sample);
    EXPECT_TRUE(sample->clamped);
    EXPECT_NEAR(
        std::accumulate(sample->weights.begin(), sample->weights.end(), 0.f),
        1.f, 1e-6f);
  }
  EXPECT_TRUE(volume.contains(volume.minimum()));
  EXPECT_TRUE(volume.contains(volume.maximum()));
  const auto corner =
      volume.interpolate({-100, 100, 100}, DdgiOutsideVolume::Clamp);
  ASSERT_TRUE(corner);
  EXPECT_EQ(corner->probeIndices[0], 21u);
  EXPECT_FLOAT_EQ(corner->weights[0], 1.f);
}

TEST(DdgiProbeVolumeTests,
     LargeOriginUsesActualRoundedProbeCentersForInterpolation) {
  const DdgiProbeVolume volume({{100000000.f, 0, 0}, {12, 1, 1}, {5, 1, 1}});
  // Non-power-of-two spacing rounds centers alternately above/below the ideal
  // double lattice. Each actual probe must retain all of its own contribution.
  for (uint32_t probe = 0; probe < volume.probeCount(); ++probe) {
    const auto sample = volume.interpolate(volume.probePosition(probe));
    ASSERT_TRUE(sample);
    float energy = 0;
    for (uint32_t corner = 0; corner < 8; ++corner)
      if (sample->probeIndices[corner] == probe)
        energy += sample->weights[corner];
      else
        EXPECT_FLOAT_EQ(sample->weights[corner], 0.f);
    EXPECT_FLOAT_EQ(energy, 1.f) << "probe=" << probe;
  }
  const auto midpoint = volume.interpolate({100000008.f, 0, 0});
  ASSERT_TRUE(midpoint);
  EXPECT_EQ(midpoint->probeIndices[0], 0u);
  EXPECT_EQ(midpoint->probeIndices[1], 1u);
  EXPECT_FLOAT_EQ(midpoint->weights[0], .5f);
  EXPECT_FLOAT_EQ(midpoint->weights[1], .5f);
}

TEST(DdgiProbeVolumeTests, SingletonAxesDoNotIntroduceDuplicateEnergy) {
  const DdgiProbeVolume volume({{3, 4, 5}, {2, 3, 4}, {1, 3, 1}});
  const auto sample = volume.interpolate({3, 5.5f, 5});
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->probeIndices,
            (std::array<uint32_t, 8>{0, 0, 1, 1, 0, 0, 1, 1}));
  EXPECT_EQ(sample->weights,
            (std::array<float, 8>{.5f, 0, .5f, 0, 0, 0, 0, 0}));
  EXPECT_FALSE(volume.interpolate({3.01f, 5.5f, 5}));
  const auto clamped =
      volume.interpolate({100, 5.5f, -100}, DdgiOutsideVolume::Clamp);
  ASSERT_TRUE(clamped);
  EXPECT_EQ(clamped->weights, sample->weights);
  const DdgiProbeVolume single({{3, 4, 5}, {2, 3, 4}, {1, 1, 1}});
  const auto one = single.interpolate({3, 4, 5});
  ASSERT_TRUE(one);
  EXPECT_FLOAT_EQ(one->weights[0], 1.f);
  EXPECT_FLOAT_EQ(
      std::accumulate(one->weights.begin(), one->weights.end(), 0.f), 1.f);
}

TEST(DdgiProbeVolumeTests, RejectsZeroCountsAndNonPositiveSpacingOnEveryAxis) {
  for (int axis = 0; axis < 3; ++axis) {
    DdgiProbeVolumeDesc desc;
    desc.counts[axis] = 0;
    expectInvalidContaining([&] { (void)DdgiProbeVolume(desc); }, "counts");
    for (const float spacing : {0.f, -1.f}) {
      desc = {};
      desc.spacing[axis] = spacing;
      expectInvalidContaining([&] { (void)DdgiProbeVolume(desc); }, "spacing");
    }
  }
}

TEST(DdgiProbeVolumeTests, RejectsNonfiniteDescriptionAndInterpolation) {
  const auto volume = nonCubicVolume();
  for (const float invalid : {std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()})
    for (int axis = 0; axis < 3; ++axis) {
      DdgiProbeVolumeDesc desc;
      desc.origin[axis] = invalid;
      expectInvalidContaining([&] { (void)DdgiProbeVolume(desc); }, "origin");
      desc = {};
      desc.spacing[axis] = invalid;
      expectInvalidContaining([&] { (void)DdgiProbeVolume(desc); }, "spacing");
      glm::vec3 position(0, 12, 0);
      position[axis] = invalid;
      EXPECT_FALSE(volume.contains(position));
      EXPECT_THROW((void)volume.interpolate(position), std::invalid_argument);
      EXPECT_THROW((void)volume.interpolate(position, DdgiOutsideVolume::Clamp),
                   std::invalid_argument);
    }
  EXPECT_THROW((void)volume.interpolate({0, 12, 0}, DdgiOutsideVolume(123)),
               std::invalid_argument);
}

TEST(DdgiProbeVolumeTests, RejectsIndexProductOverflowWithoutAllocatingProbes) {
  const uint32_t maximum = std::numeric_limits<uint32_t>::max();
  for (const glm::uvec3 counts : {glm::uvec3(65536, 65536, 1),
                                  {maximum, maximum, maximum},
                                  {maximum, 2, 1}}) {
    DdgiProbeVolumeDesc desc;
    desc.counts = counts;
    expectInvalidContaining([&] { (void)DdgiProbeVolume(desc); }, "uint32");
  }
  // Large but valid non-cubic input stays a constant-size CPU description.
  const DdgiProbeVolume valid({{0, 0, 0}, {1, 1, 1}, {65535, 65535, 1}});
  EXPECT_EQ(valid.probeCount(), 4294836225u);
  EXPECT_EQ(valid.linearIndex({65534, 65534, 0}), 4294836224u);
}

TEST(DdgiProbeVolumeTests, RejectsInfiniteAndCollapsedFloatWorldBounds) {
  const float maximum = std::numeric_limits<float>::max();
  expectInvalidContaining(
      [&] {
        (void)DdgiProbeVolume({{maximum, 0, 0}, {maximum, 1, 1}, {2, 1, 1}});
      },
      "bounds");
  expectInvalidContaining(
      [] { (void)DdgiProbeVolume({{1e20f, 0, 0}, {1, 1, 1}, {2, 1, 1}}); },
      "rebase");
  // A singleton axis has no distinct adjacent positions to require.
  EXPECT_NO_THROW(
      (void)DdgiProbeVolume({{maximum, 0, 0}, {1, 1, 1}, {1, 1, 1}}));
}

TEST(DdgiProbeUpdateTests, BoundedScheduleWrapsWithoutPartialRaySets) {
  const auto volume = nonCubicVolume();
  const auto batch = planDdgiUpdates(volume, {10, 8, 31}, 22);
  EXPECT_EQ(batch.volumeProbeCount, 24u);
  EXPECT_EQ(batch.probeCount, 3u);
  EXPECT_EQ(batch.rayCount, 24u);
  EXPECT_EQ(batch.nextProbe, 1u);
  std::array<DdgiProbeUpdate, 4> updates;
  updates[3].probeIndex = 999;
  writeDdgiProbeUpdates(batch, updates);
  EXPECT_EQ(updates[0].probeIndex, 22u);
  EXPECT_EQ(updates[1].probeIndex, 23u);
  EXPECT_EQ(updates[2].probeIndex, 0u);
  for (uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(updates[i].firstRay, i * 8);
    EXPECT_EQ(updates[i].rayCount, 8u);
    EXPECT_EQ(updates[i].reserved, 0u);
  }
  EXPECT_EQ(updates[3].probeIndex, 999u);
  const auto next = planDdgiUpdates(volume, {10, 8, 31}, batch.nextProbe);
  EXPECT_EQ(next.firstProbe, 1u);
  EXPECT_EQ(next.nextProbe, 4u);
}

TEST(DdgiProbeUpdateTests, EachBoundCapsWorkAndZeroBudgetPreservesCursor) {
  const auto volume = nonCubicVolume();
  EXPECT_EQ(planDdgiUpdates(volume, {100, 2, 1000}).probeCount, 24u);
  EXPECT_EQ(planDdgiUpdates(volume, {2, 2, 1000}).probeCount, 2u);
  EXPECT_EQ(planDdgiUpdates(volume, {100, 2, 7}).probeCount, 3u);
  for (const auto budget : {DdgiUpdateBudget{0, 8, 64}, {8, 8, 0}, {8, 8, 7}}) {
    const auto batch = planDdgiUpdates(volume, budget, 17);
    EXPECT_EQ(batch.probeCount, 0u);
    EXPECT_EQ(batch.rayCount, 0u);
    EXPECT_EQ(batch.nextProbe, 17u);
    EXPECT_NO_THROW(writeDdgiProbeUpdates(batch, {}));
  }
  EXPECT_THROW((void)planDdgiUpdates(volume, {10, 0, 10}),
               std::invalid_argument);
  EXPECT_THROW((void)planDdgiUpdates(volume, {}, 24), std::out_of_range);
}

TEST(DdgiProbeUpdateTests,
     MaximumRayBudgetDoesNotOverflowMultiplicationOrWrap) {
  const DdgiProbeVolume volume({{0, 0, 0}, {1, 1, 1}, {65535, 65535, 1}});
  const uint32_t maximum = std::numeric_limits<uint32_t>::max();
  const auto batch =
      planDdgiUpdates(volume, {maximum, 2, maximum}, 4294836224u);
  EXPECT_EQ(batch.probeCount, 2147483647u);
  EXPECT_EQ(batch.rayCount, 4294967294u);
  EXPECT_EQ(batch.nextProbe, 2147483646u);
  const auto one = planDdgiUpdates(volume, {maximum, maximum, maximum});
  EXPECT_EQ(one.probeCount, 1u);
  EXPECT_EQ(one.rayCount, maximum);
}

TEST(DdgiProbeUpdateTests, RejectsMalformedBatchAndShortSpanBeforeWriting) {
  auto batch = planDdgiUpdates(nonCubicVolume(), {3, 8, 24});
  std::array<DdgiProbeUpdate, 2> shortSpan;
  shortSpan[0].probeIndex = 777;
  EXPECT_THROW(writeDdgiProbeUpdates(batch, shortSpan), std::out_of_range);
  EXPECT_EQ(shortSpan[0].probeIndex, 777u);
  std::array<DdgiProbeUpdate, 3> full;
  for (int field = 0; field < 6; ++field) {
    batch = planDdgiUpdates(nonCubicVolume(), {3, 8, 24});
    switch (field) {
    case 0:
      batch.volumeProbeCount = 0;
      break;
    case 1:
      batch.firstProbe = 24;
      break;
    case 2:
      batch.probeCount = 25;
      break;
    case 3:
      batch.raysPerProbe = 0;
      break;
    case 4:
      batch.rayCount = 23;
      break;
    case 5:
      batch.nextProbe = 4;
      break;
    }
    full[0].probeIndex = 777;
    EXPECT_THROW(writeDdgiProbeUpdates(batch, full), std::invalid_argument);
    EXPECT_EQ(full[0].probeIndex, 777u);
  }
}

TEST(DdgiProbeResourceTests,
     PlansOctahedralBordersPaddingHistoryAndAllBuffers) {
  const auto plan = planDdgiResources(nonCubicVolume(), {6, 14, {5, 16, 63}});
  EXPECT_EQ(plan.irradiance.interiorTexels, 6u);
  EXPECT_EQ(plan.irradiance.tileTexels, 8u);
  EXPECT_EQ(plan.irradiance.tilesPerRow, 5u);
  EXPECT_EQ(plan.irradiance.tileRows, 5u);
  EXPECT_EQ(plan.irradiance.width, 40u);
  EXPECT_EQ(plan.irradiance.height, 40u);
  EXPECT_EQ(plan.irradiance.bytesPerHistory, 12800u);
  EXPECT_EQ(plan.visibility.tileTexels, 16u);
  EXPECT_EQ(plan.visibility.width, 80u);
  EXPECT_EQ(plan.visibility.height, 80u);
  EXPECT_EQ(plan.visibility.bytesPerHistory, 51200u);
  EXPECT_EQ(plan.updateCapacity.probeCount, 3u);
  EXPECT_EQ(plan.updateCapacity.rayCount, 48u);
  EXPECT_EQ(plan.probeStateBytes, 768u);
  EXPECT_EQ(plan.rayScratchBytes, 1536u);
  EXPECT_EQ(plan.updateRecordBytes, 48u);
  EXPECT_EQ(plan.probePositionBytes, 384u);
  EXPECT_EQ(plan.payloadBytes, 130736u);
}

TEST(DdgiProbeResourceTests, PlansNonSquareAtlasAndDisabledScratch) {
  const DdgiProbeVolume volume({{0, 0, 0}, {1, 1, 1}, {3, 1, 1}});
  DdgiResourceSettings settings;
  settings.updates.maxProbesPerFrame = 0;
  const auto plan = planDdgiResources(volume, settings);
  EXPECT_EQ(plan.irradiance.tilesPerRow, 2u);
  EXPECT_EQ(plan.irradiance.tileRows, 2u);
  EXPECT_EQ(plan.rayScratchBytes, 0u);
  EXPECT_EQ(plan.updateRecordBytes, 0u);
  EXPECT_EQ(plan.probeStateBytes, 96u);
  const DdgiProbeVolume six({{0, 0, 0}, {1, 1, 1}, {6, 1, 1}});
  const auto rectangle = planDdgiResources(six, settings);
  EXPECT_EQ(rectangle.irradiance.tilesPerRow, 3u);
  EXPECT_EQ(rectangle.irradiance.tileRows, 2u);
  EXPECT_EQ(rectangle.irradiance.width, 24u);
  EXPECT_EQ(rectangle.irradiance.height, 16u);
}

TEST(DdgiProbeResourceTests,
     EachDeviceAndPayloadLimitRejectsWithActionableResource) {
  const auto volume = nonCubicVolume();
  const auto plan = planDdgiResources(volume);
  DdgiResourceLimits limits;
  limits.maxImageDimension2D = 0;
  expectInvalidContaining([&] { (void)planDdgiResources(volume, {}, limits); },
                          "maxImageDimension2D");
  limits = {};
  limits.maxImageDimension2D = 79;
  expectInvalidContaining([&] { (void)planDdgiResources(volume, {}, limits); },
                          "visibility");
  limits = {};
  limits.maxBufferBytes = plan.rayScratchBytes - 1;
  expectInvalidContaining([&] { (void)planDdgiResources(volume, {}, limits); },
                          "ray scratch");
  limits = {};
  limits.maxPayloadBytes = plan.payloadBytes - 1;
  expectInvalidContaining([&] { (void)planDdgiResources(volume, {}, limits); },
                          "maxPayloadBytes");
  limits.maxPayloadBytes = plan.payloadBytes;
  EXPECT_NO_THROW((void)planDdgiResources(volume, {}, limits));
  DdgiResourceSettings disabled;
  disabled.updates.maxProbesPerFrame = 0;
  limits = {};
  limits.maxBufferBytes = plan.probeStateBytes - 1;
  expectInvalidContaining(
      [&] { (void)planDdgiResources(volume, disabled, limits); },
      "probe state");
}

TEST(DdgiProbeResourceTests,
     ExactImageLimitAcceptsAndRoundedPaddingIsBudgeted) {
  const DdgiProbeVolume volume({{0, 0, 0}, {1, 1, 1}, {3, 3, 1}});
  DdgiResourceSettings settings;
  settings.updates.maxProbesPerFrame = 0;
  DdgiResourceLimits limits;
  limits.maxImageDimension2D = 48;
  EXPECT_NO_THROW((void)planDdgiResources(volume, settings, limits));
  limits.maxImageDimension2D = 47;
  EXPECT_THROW((void)planDdgiResources(volume, settings, limits),
               std::invalid_argument);
  // Three probes consume four allocated tiles, in both histories.
  const DdgiProbeVolume padded({{0, 0, 0}, {1, 1, 1}, {3, 1, 1}});
  limits = {};
  limits.maxPayloadBytes = 3u * (8u * 8u + 16u * 16u) * 8u * 2u + 3u * 32u;
  expectInvalidContaining(
      [&] { (void)planDdgiResources(padded, settings, limits); },
      "maxPayloadBytes");
}

TEST(DdgiProbeResourceTests,
     RejectsInvalidTileSizeAndCheckedUint64PayloadOverflow) {
  const DdgiProbeVolume volume({{0, 0, 0}, {1, 1, 1}, {2, 2, 1}});
  DdgiResourceSettings settings;
  for (const uint32_t invalid :
       {0u, 1u, std::numeric_limits<uint32_t>::max() - 1,
        std::numeric_limits<uint32_t>::max()}) {
    settings = {};
    settings.irradianceInteriorTexels = invalid;
    expectInvalidContaining([&] { (void)planDdgiResources(volume, settings); },
                            "irradiance interior");
    settings = {};
    settings.visibilityInteriorTexels = invalid;
    expectInvalidContaining([&] { (void)planDdgiResources(volume, settings); },
                            "visibility interior");
  }
  DdgiResourceLimits unlimited{std::numeric_limits<uint32_t>::max(),
                               std::numeric_limits<uint64_t>::max(),
                               std::numeric_limits<uint64_t>::max()};
  settings = {};
  settings.irradianceInteriorTexels = 1000000000;
  expectInvalidContaining(
      [&] { (void)planDdgiResources(volume, settings, unlimited); },
      "atlas payload overflow");
  settings.irradianceInteriorTexels = settings.visibilityInteriorTexels =
      500000000;
  expectInvalidContaining(
      [&] { (void)planDdgiResources(volume, settings, unlimited); },
      "history atlas payload overflow");
}

TEST(DdgiProbeResourceTests,
     DefaultProbeStateDoesNotAdvertiseInitializedLighting) {
  const DdgiProbeState state;
  EXPECT_EQ(state.classification, DdgiProbeClassification::Uninitialized);
  EXPECT_EQ(state.validRayCount, 0u);
  EXPECT_EQ(state.flags, 0u);
  EXPECT_EQ(state.relocation, (std::array<float, 3>{0, 0, 0}));
  EXPECT_EQ(sizeof(state), 32u);
  EXPECT_EQ(sizeof(DdgiRaySample), 32u);
  EXPECT_EQ(sizeof(DdgiProbeUpdate), 16u);
}

} // namespace
