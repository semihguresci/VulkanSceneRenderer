#include "Container/geometry/OpenVdbTransportReference.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>

namespace {
namespace vdb = container::geometry::vdb;
using V = vdb::TransportVector;
const std::array<vdb::TransportRay, 1> ray{{{{0, 0, -2}, {0, 0, 1}}}};
vdb::TransportReferenceOptions options(uint32_t samples = 100'000, uint32_t order = 0) {
  vdb::TransportReferenceOptions o;
  o.samplesPerRay = samples;
  o.maxScatteringEvents = order;
  o.rouletteStart = 65;
  return o;
}
double hg(double g) {
  return (1 - g * g) / (4 * std::numbers::pi * std::pow(1 + g * g - 2 * g, 1.5));
}
void statistical(const vdb::TransportRayEstimate &value, V expected, double slack = 1e-4) {
  for (int k = 0; k < 3; ++k)
    EXPECT_NEAR(value.radiance[k], expected[k], 6 * value.standardError[k] + slack);
}
vdb::TransportScene homogeneous(double extinction = 1) {
  vdb::TransportScene scene;
  vdb::TransportMedium medium;
  medium.extinction = extinction;
  medium.albedo = {0, 0, 0};
  scene.media.push_back(medium);
  return scene;
}

TEST(OpenVdbTransportReference, VacuumAndBlackTwoSidedGeometryAreExactWithoutOpenVdb) {
  vdb::TransportScene scene;
  scene.environment = {.25, .5, 2};
  auto result = vdb::TraceTransportReference(scene, ray, options(16));
  EXPECT_EQ(result.rays[0].radiance, scene.environment);
  EXPECT_EQ(result.rays[0].standardError, (V{}));
  EXPECT_EQ(result.paths, 16u);
  EXPECT_EQ(result.scatteringEvents, 0u);
  scene.occluders.push_back({{-2, -2, 0}, {2, -2, 0}, {0, 2, 0}});
  result = vdb::TraceTransportReference(scene, ray, options(16));
  EXPECT_EQ(result.rays[0].radiance, (V{}));
  std::swap(scene.occluders[0].a, scene.occluders[0].c);
  EXPECT_EQ(vdb::TraceTransportReference(scene, ray, options(16)).rays[0].radiance, (V{}));
}

TEST(OpenVdbTransportReference, HomogeneousAbsorptionMatchesBeerAndHasNoSpuriousScattering) {
  auto scene = homogeneous(.7);
  scene.environment = {1, .5, 2};
  const auto r = vdb::TraceTransportReference(scene, ray, options());
  const double t = std::exp(-1.4);
  statistical(r.rays[0], {t, .5 * t, 2 * t});
  EXPECT_EQ(r.scatteringEvents, 0u);
  EXPECT_EQ(r.truncatedPaths, 0u);
  EXPECT_GE(r.rays[0].radiance[0], 0);
  EXPECT_LE(r.rays[0].radiance[0], 1);
}

TEST(OpenVdbTransportReference, IndependentPureEmissionAndAbsorbingEmissionMatchAnalyticSlab) {
  auto scene = homogeneous(0);
  scene.media[0].emission = {2, 1, .25};
  scene.environment = {.5, .25, 1};
  auto r = vdb::TraceTransportReference(scene, ray, options());
  statistical(r.rays[0], {4.5, 2.25, 1.5});
  EXPECT_EQ(r.truncatedPaths, 0u);
  EXPECT_GT(r.nullCollisions, 0u);
  scene.media[0].extinction = .7;
  r = vdb::TraceTransportReference(scene, ray, options());
  const double t = std::exp(-1.4), integral = (1 - t) / .7;
  statistical(r.rays[0], {.5 * t + 2 * integral, .25 * t + integral, t + .25 * integral});
  auto high = options();
  high.emissionSamplingRate = 4;
  const auto independent = vdb::TraceTransportReference(scene, ray, high);
  statistical(independent.rays[0], {.5 * t + 2 * integral, .25 * t + integral, t + .25 * integral});
}

TEST(OpenVdbTransportReference, HeterogeneousTrilinearSlabAndOverlapsUseCombinedExtinction) {
  auto scene = homogeneous(1);
  scene.environment = {1, 1, 1};
  scene.media[0].dimensions = {1, 1, 2};
  scene.media[0].density = {0, 2};
  auto r = vdb::TraceTransportReference(scene, ray, options());
  const double t = std::exp(-2.0);
  statistical(r.rays[0], {t, t, t});
  scene = homogeneous(.5);
  scene.environment = {1, 1, 1};
  scene.media.push_back(scene.media[0]);
  const auto overlap = vdb::TraceTransportReference(scene, ray, options());
  auto one = homogeneous(1);
  one.environment = scene.environment;
  const auto merged = vdb::TraceTransportReference(one, ray, options());
  EXPECT_EQ(overlap.rays[0].radiance, merged.rays[0].radiance);
}

TEST(OpenVdbTransportReference, OverlappingRgbAlbedosAndDifferentPhasesMatchSingleScatterIntegral) {
  auto scene = homogeneous(.25);
  scene.media[0].albedo = {1, 0, 0};
  scene.media[0].anisotropy = .6;
  scene.media.push_back(scene.media[0]);
  scene.media[1].albedo = {0, 1, 0};
  scene.media[1].anisotropy = -.4;
  scene.lights.push_back({vdb::TransportLightType::Directional, {0, 0, 1}, {1, 1, 1}});
  auto o = options(200'000, 1);
  const auto r = vdb::TraceTransportReference(scene, ray, o);
  statistical(r.rays[0], {.5 * std::exp(-1) * hg(.6), .5 * std::exp(-1) * hg(-.4), 0}, 2e-4);
  EXPECT_GT(r.shadowProposals, 0u);
  EXPECT_NEAR(r.meanScatteringCosine, .1, .015);
  std::swap(scene.media[0], scene.media[1]);
  o.seed = 73;
  const auto swapped = vdb::TraceTransportReference(scene, ray, o);
  statistical(swapped.rays[0], {.5 * std::exp(-1) * hg(.6), .5 * std::exp(-1) * hg(-.4), 0}, 2e-4);
}

TEST(OpenVdbTransportReference, PositiveHenyeyGreensteinSamplesFollowCurrentReversePathTravel) {
  auto scene = homogeneous(.5);
  scene.media[0].albedo = {1, 1, 1};
  scene.media[0].anisotropy = .7;
  const auto r = vdb::TraceTransportReference(scene, ray, options(80'000, 1));
  EXPECT_GT(r.phaseSamples, 40'000u);
  EXPECT_NEAR(r.meanScatteringCosine, .7, .01);
  scene.media[0].anisotropy = -.7;
  const auto backwards = vdb::TraceTransportReference(scene, ray, options(80'000, 1));
  EXPECT_NEAR(backwards.meanScatteringCosine, -.7, .01);
  scene.media[0].anisotropy = 1e-10;
  const auto nearZero = vdb::TraceTransportReference(scene, ray, options(80'000, 1));
  EXPECT_NEAR(nearZero.meanScatteringCosine, 0, .01);
}

TEST(OpenVdbTransportReference,
     ConservativeMediumPreservesConstantEnvironmentAndRouletteExpectation) {
  auto scene = homogeneous(.25);
  scene.media[0].albedo = {1, 1, 1};
  scene.environment = {1, .5, .25};
  auto o = options(20'000, 64);
  const auto r = vdb::TraceTransportReference(scene, ray, o);
  EXPECT_EQ(r.truncatedPaths, 0u);
  EXPECT_EQ(r.rays[0].radiance, scene.environment);
  EXPECT_EQ(r.rays[0].standardError, (V{}));
  o.samplesPerRay = 80'000;
  o.rouletteStart = 1;
  const auto roulette = vdb::TraceTransportReference(scene, ray, o);
  statistical(roulette.rays[0], scene.environment);
  EXPECT_GT(roulette.rouletteTerminations, 0u);
}

TEST(OpenVdbTransportReference, ASecondSpatialScatterRoutesLightAroundAnOpaqueOccluder) {
  auto scene = homogeneous(.7);
  scene.media[0].boundsMin = {-2, -2, -2};
  scene.media[0].boundsMax = {2, 2, 2};
  scene.media[0].albedo = {.9, .9, .9};
  scene.lights.push_back({vdb::TransportLightType::Directional, {1, 0, 0}, {1, 1, 1}});
  scene.occluders = {{{0, -.5, -2}, {0, .5, -2}, {0, .5, 2}},
                     {{0, -.5, -2}, {0, .5, 2}, {0, -.5, 2}}};
  const std::array<vdb::TransportRay, 1> view{{{{-3, 0, 0}, {1, 0, 0}}}};
  const auto single = vdb::TraceTransportReference(scene, view, options(80'000, 1));
  EXPECT_EQ(single.rays[0].radiance, (V{}));
  const auto multiple = vdb::TraceTransportReference(scene, view, options(120'000, 3));
  EXPECT_GT(multiple.rays[0].radiance[0], 8 * multiple.rays[0].standardError[0]);
  EXPECT_GT(multiple.rays[0].radiance[0], 1e-4);
  EXPECT_GT(multiple.scatteringEvents, single.scatteringEvents);
  EXPECT_GT(multiple.truncatedPaths, 0u);
}

TEST(OpenVdbTransportReference, PointIntensityUsesInverseSquareAndSceneSymmetry) {
  auto scene = homogeneous(.15);
  scene.media[0].albedo = {.8, .6, .4};
  scene.lights.push_back({vdb::TransportLightType::Point, {0, 0, 3}, {2, 2, 2}});
  const auto a = vdb::TraceTransportReference(scene, ray, options(100'000, 1));
  scene.lights[0].value = {4, 4, 4};
  const auto doubled = vdb::TraceTransportReference(scene, ray, options(100'000, 1));
  for (int k = 0; k < 3; ++k)
    EXPECT_DOUBLE_EQ(doubled.rays[0].radiance[k], 2 * a.rays[0].radiance[k]);
  scene.lights[0].value = {2, 2, 2};
  scene.lights[0].vector = {0, 0, -3};
  const std::array<vdb::TransportRay, 1> reflected{{{{0, 0, 2}, {0, 0, -1}}}};
  EXPECT_EQ(vdb::TraceTransportReference(scene, reflected, options(100'000, 1)).rays[0].radiance,
            a.rays[0].radiance);
  scene.lights[0].vector = {0, 0, 100};
  const auto near = vdb::TraceTransportReference(scene, ray, options(40'000, 1));
  scene.lights[0].vector = {0, 0, 200};
  const auto far = vdb::TraceTransportReference(scene, ray, options(40'000, 1));
  // Scattering points lie in z=[-1,1], so exact per-point inverse-square
  // ratios are close to four without assuming a pointlike medium.
  EXPECT_NEAR(near.rays[0].radiance[0] / far.rays[0].radiance[0], 4, .05);
}

TEST(OpenVdbTransportReference, AdditionalSpatialOrdersConvergeSeparatelyFromSamplingError) {
  auto scene = homogeneous(.8);
  scene.media[0].albedo = {.85, .85, .85};
  scene.environment = {1, 1, 1};
  double previous = 0;
  vdb::TransportReferenceResult sixteen, sixtyFour;
  for (uint32_t order : {0u, 1u, 2u, 4u, 16u, 64u}) {
    const auto result = vdb::TraceTransportReference(scene, ray, options(40'000, order));
    EXPECT_GE(result.rays[0].radiance[0], previous - 1e-14);
    EXPECT_LE(result.rays[0].radiance[0], 1);
    previous = result.rays[0].radiance[0];
    if (order == 1)
      EXPECT_GT(result.truncatedPaths, 100u);
    if (order == 16)
      sixteen = result;
    if (order == 64)
      sixtyFour = result;
  }
  EXPECT_EQ(sixtyFour.truncatedPaths, 0u);
  EXPECT_NEAR(sixteen.rays[0].radiance[0], sixtyFour.rays[0].radiance[0],
              6 * std::hypot(sixteen.rays[0].standardError[0], sixtyFour.rays[0].standardError[0]));
}

TEST(OpenVdbTransportReference, IncreasingSamplesReducesMeasuredErrorAgainstAnalyticReference) {
  auto scene = homogeneous(.5);
  scene.environment = {1, 1, 1};
  double lowError = 0, highError = 0, lowVariance = 0, highVariance = 0;
  for (uint64_t seed = 1; seed <= 8; ++seed) {
    auto low = options(256), high = options(16'384);
    low.seed = high.seed = seed;
    const auto a = vdb::TraceTransportReference(scene, ray, low),
               b = vdb::TraceTransportReference(scene, ray, high);
    lowError += std::pow(a.rays[0].radiance[0] - std::exp(-1.0), 2);
    highError += std::pow(b.rays[0].radiance[0] - std::exp(-1.0), 2);
    lowVariance += std::pow(a.rays[0].standardError[0], 2);
    highVariance += std::pow(b.rays[0].standardError[0], 2);
  }
  EXPECT_LT(highError, lowError * .15);
  EXPECT_LT(highVariance, lowVariance * .025);
}

TEST(OpenVdbTransportReference, DenseVolumeAdapterCopiesItsPayloadAndRejectsNativeData) {
  container::geometry::OpenVdbVolume volume;
  volume.dimensions = {1, 1, 1};
  volume.boundsMin = {-1, -2, -3};
  volume.boundsMax = {1, 2, 3};
  volume.density = {.5f};
  auto medium = vdb::MakeTransportMedium(volume);
  EXPECT_EQ(medium.boundsMin, (V{-1, -2, -3}));
  EXPECT_EQ(medium.density, volume.density);
  volume.density[0] = 1;
  EXPECT_FLOAT_EQ(medium.density[0], .5f);
  volume.sparseDensity = {0};
  EXPECT_THROW((void)vdb::MakeTransportMedium(volume), std::invalid_argument);
}

TEST(OpenVdbTransportReference, InvalidInputsAndBudgetsThrowWithoutPublishingPartialResults) {
  auto scene = homogeneous();
  const auto fails = [&](const auto &s, const auto &o) {
    EXPECT_THROW((void)vdb::TraceTransportReference(s, ray, o), std::exception);
  };
  {
    auto o = options();
    o.samplesPerRay = 0;
    fails(scene, o);
  }
  {
    auto o = options();
    o.maxScatteringEvents = 65;
    fails(scene, o);
  }
  {
    auto o = options();
    o.maxWork = 1;
    fails(scene, o);
  }
  {
    auto o = options();
    o.maxInputBytes = 1;
    fails(scene, o);
  }
  {
    auto o = options();
    o.emissionSamplingRate = 0;
    fails(scene, o);
  }
  {
    auto s = scene;
    s.media[0].density = {-1};
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].density = {std::numeric_limits<float>::infinity()};
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].density.clear();
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].dimensions = {257, 1, 1};
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].albedo = {1.1, 0, 0};
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].anisotropy = 1;
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].boundsMin[0] = s.media[0].boundsMax[0];
    fails(s, options());
  }
  {
    auto s = scene;
    s.media.resize(9);
    fails(s, options());
  }
  {
    auto s = scene;
    s.occluders.push_back({});
    fails(s, options());
  }
  {
    auto s = scene;
    s.media[0].dimensions = {2, 1, 1};
    s.media[0].density = {1, 1};
    auto o = options();
    o.maxDensityValues = 1;
    fails(s, o);
  }
  std::array<vdb::TransportRay, 1> bad = ray;
  bad[0].direction = {0, 0, 0};
  EXPECT_THROW((void)vdb::TraceTransportReference(scene, bad, options()), std::invalid_argument);
  auto o = options(32, 2);
  o.maxWork = 100'000;
  const auto first = vdb::TraceTransportReference(scene, ray, o),
             second = vdb::TraceTransportReference(scene, ray, o);
  EXPECT_EQ(first.rays[0].radiance, second.rays[0].radiance);
  EXPECT_EQ(first.work, second.work);
}

TEST(OpenVdbTransportReference, RepresentableEmissionCannotSilentlyUnderflowItsProposalScore) {
  auto scene = homogeneous(0);
  scene.media[0].emission = {1e-320, 0, 0};
  auto o = options(1);
  o.emissionSamplingRate = 1e6;
  // The exact slab integral 2e-320 is representable. Dividing each source score
  // by a large proposal rate is not; this must fail rather than return black.
  EXPECT_THROW((void)vdb::TraceTransportReference(scene, ray, o), std::runtime_error);
}

TEST(OpenVdbTransportReference, ScaledMomentsPreserveRepresentableTinyRadianceUncertainty) {
  auto scene = homogeneous(1);
  scene.environment = {1e-200, 0, 0};
  const auto result = vdb::TraceTransportReference(scene, ray, options(4096));
  const auto &estimate = result.rays[0];
  EXPECT_GT(estimate.radiance[0], 0);
  EXPECT_GT(estimate.standardError[0], 0);
  const double observedProbability = estimate.radiance[0] / 1e-200;
  const double expectedError = std::sqrt(observedProbability * (1 - observedProbability) / 4095);
  EXPECT_NEAR(estimate.standardError[0] / 1e-200, expectedError, 1e-14);
}

TEST(OpenVdbTransportReference, OrderTruncationCountsSelectedScatteringRatherThanBlackAbsorbers) {
  auto scene = homogeneous(.9);
  scene.media.push_back(scene.media[0]);
  scene.media[1].extinction = .1;
  scene.media[1].albedo = {1, 1, 1};
  const auto result = vdb::TraceTransportReference(scene, ray, options());
  const double probability = .1 * (1 - std::exp(-2.0));
  EXPECT_NEAR(double(result.truncatedPaths) / result.paths, probability,
              6 * std::sqrt(probability * (1 - probability) / result.paths));
  EXPECT_EQ(result.scatteringEvents, 0u);
}
} // namespace
