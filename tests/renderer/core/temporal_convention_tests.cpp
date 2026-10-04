#include "Container/common/CommonMath.h"
#include "Container/renderer/temporal/TemporalConventions.h"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <utility>

namespace {

namespace temporal = container::temporal;
constexpr float kTolerance = 1e-6f;

void expectUv(glm::vec2 actual, float u, float v) {
  EXPECT_NEAR(actual.x, u, kTolerance);
  EXPECT_NEAR(actual.y, v, kTolerance);
}

glm::mat4 perspective(float aspect = 1.0f) {
  return container::math::perspectiveRH_ReverseZ(glm::radians(90.0f), aspect,
                                                 1.0f, 11.0f);
}

TEST(TemporalConventions, NegativeHeightViewportMapsNamedCorners) {
  // Independent viewport geometry: +Y is the top, +X is the right.
  expectUv(temporal::sceneNdcToUv({-1.0f, 1.0f}), 0.0f, 0.0f);
  expectUv(temporal::sceneNdcToUv({1.0f, 1.0f}), 1.0f, 0.0f);
  expectUv(temporal::sceneNdcToUv({-1.0f, -1.0f}), 0.0f, 1.0f);
  expectUv(temporal::sceneNdcToUv({1.0f, -1.0f}), 1.0f, 1.0f);
  expectUv(temporal::sceneNdcToUv({0.0f, 0.0f}), 0.5f, 0.5f);
  expectUv(temporal::sceneUvToNdc({0.0f, 0.0f}), -1.0f, 1.0f);
  expectUv(temporal::sceneUvToNdc({1.0f, 1.0f}), 1.0f, -1.0f);
  expectUv(temporal::sceneUvToNdc({0.75f, 0.25f}), 0.5f, 0.5f);
}

TEST(TemporalConventions, PerspectiveUsesReverseZAndCameraForwardMinusZ) {
  // With near=1, far=11, a point at distance 2 has depth 9/20.
  for (const auto [distance, expectedDepth] :
       std::array<std::pair<float, float>, 3>{
           {{1.0f, 1.0f}, {2.0f, 0.45f}, {11.0f, 0.0f}}}) {
    SCOPED_TRACE(distance);
    const auto point = temporal::projectSceneClip(
        perspective() * glm::vec4(0.0f, 0.0f, -distance, 1.0f));
    ASSERT_TRUE(point.valid);
    expectUv(point.uv, 0.5f, 0.5f);
    EXPECT_NEAR(point.reverseZDepth, expectedDepth, kTolerance);
  }
  EXPECT_FALSE(temporal::projectSceneClip(perspective() *
                                          glm::vec4(0.0f, 0.0f, 2.0f, 1.0f))
                   .valid);
}

TEST(TemporalConventions, OrthographicProjectionUsesTheSameUvAndDepthContract) {
  const auto projection =
      container::math::orthoRH_ReverseZ(-2.0f, 6.0f, -4.0f, 4.0f, 1.0f, 11.0f);
  const auto topLeft = temporal::projectSceneClip(
      projection * glm::vec4(-2.0f, 4.0f, -1.0f, 1.0f));
  const auto center = temporal::projectSceneClip(
      projection * glm::vec4(2.0f, 0.0f, -6.0f, 1.0f));
  ASSERT_TRUE(topLeft.valid);
  ASSERT_TRUE(center.valid);
  expectUv(topLeft.uv, 0.0f, 0.0f);
  EXPECT_NEAR(topLeft.reverseZDepth, 1.0f, kTolerance);
  expectUv(center.uv, 0.5f, 0.5f);
  EXPECT_NEAR(center.reverseZDepth, 0.5f, kTolerance);
}

TEST(TemporalConventions, AspectChangeHasAnalyticHorizontalMapping) {
  // 90-degree vertical FOV: at z=-2, x=y=1 is half a vertical half-frustum.
  const glm::vec4 position(1.0f, 1.0f, -2.0f, 1.0f);
  const auto previousClip = perspective(1.0f) * position;
  const auto currentClip = perspective(2.0f) * position;
  const auto previous = temporal::projectSceneClip(previousClip);
  const auto current = temporal::projectSceneClip(currentClip);
  ASSERT_TRUE(previous.valid);
  ASSERT_TRUE(current.valid);
  expectUv(previous.uv, 0.75f, 0.25f);
  expectUv(current.uv, 0.625f, 0.25f);
  // The temporal owner resets on projection/aspect changes, even though both
  // positions can individually be projected.
  EXPECT_FALSE(
      temporal::motionFromUnjitteredClips(currentClip, previousClip, false)
          .valid);
}

TEST(TemporalConventions,
     ObjectMotionReprojectsRightAndDownToPreviousLocation) {
  const auto projection = perspective();
  const auto currentClip = projection * glm::vec4(0.0f, -1.0f, -2.0f, 1.0f);
  const auto previousClip = projection * glm::vec4(-1.0f, 1.0f, -2.0f, 1.0f);
  const auto motion =
      temporal::motionFromUnjitteredClips(currentClip, previousClip, true);
  ASSERT_TRUE(motion.valid);
  expectUv(motion.uv, -0.25f, -0.5f);
  const auto address = temporal::reprojectToPreviousGrid(
      {0.5f, 0.75f}, motion, {0.0f, 0.0f}, {0.0f, 0.0f});
  ASSERT_TRUE(address.valid);
  expectUv(address.uv, 0.25f, 0.25f);
  EXPECT_NEAR(motion.previousReverseZDepth, 0.45f, kTolerance);
}

TEST(TemporalConventions, CameraTranslationHasTheOppositeApparentMotion) {
  const auto projection = perspective();
  const glm::vec4 worldPoint(1.0f, 1.0f, -2.0f, 1.0f);
  const auto previousClip = projection * worldPoint;
  // Move the camera right by one world unit, then up by one world unit.
  const auto cameraRight = container::math::lookAt(
      {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
  const auto cameraUp = container::math::lookAt(
      {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
  const auto horizontal = temporal::motionFromUnjitteredClips(
      projection * cameraRight * worldPoint, previousClip, true);
  const auto vertical = temporal::motionFromUnjitteredClips(
      projection * cameraUp * worldPoint, previousClip, true);
  ASSERT_TRUE(horizontal.valid);
  ASSERT_TRUE(vertical.valid);
  expectUv(horizontal.uv, 0.25f, 0.0f);
  expectUv(vertical.uv, 0.0f, -0.25f);
}

TEST(TemporalConventions, PixelJitterMovesRightAndDownWithoutChangingDepth) {
  const auto jitter = temporal::makeJitter({0.25f, 0.5f}, {800u, 400u});
  ASSERT_TRUE(jitter.valid);
  expectUv(jitter.uv, 0.0003125f, 0.00125f);
  expectUv(jitter.ndc, 0.000625f, -0.0025f);
  glm::vec4 clip(0.0f, 0.0f, 0.9f, 2.0f);
  clip.x += jitter.ndc.x * clip.w;
  clip.y += jitter.ndc.y * clip.w;
  const auto projected = temporal::projectSceneClip(clip);
  ASSERT_TRUE(projected.valid);
  expectUv(projected.uv, 0.5003125f, 0.50125f);
  EXPECT_NEAR(projected.reverseZDepth, 0.45f, kTolerance);
}

TEST(TemporalConventions, StaticSurfaceSeparatesResolvedColorAndRawDepthGrids) {
  const glm::vec4 clip(0.0f, 0.0f, 0.9f, 2.0f);
  const auto currentJitter =
      temporal::makeJitter({0.25f, -0.25f}, {800u, 400u});
  const auto previousJitter =
      temporal::makeJitter({-0.25f, 0.25f}, {800u, 400u});
  ASSERT_TRUE(currentJitter.valid);
  ASSERT_TRUE(previousJitter.valid);
  const auto motion = temporal::motionFromUnjitteredClips(clip, clip, true);
  ASSERT_TRUE(motion.valid);
  expectUv(motion.uv, 0.0f, 0.0f);
  const glm::vec2 currentRasterUv(0.5003125f, 0.499375f);
  const auto color = temporal::reprojectToPreviousGrid(
      currentRasterUv, motion, currentJitter.uv, {0.0f, 0.0f});
  const auto depth = temporal::reprojectToPreviousGrid(
      currentRasterUv, motion, currentJitter.uv, previousJitter.uv);
  ASSERT_TRUE(color.valid);
  ASSERT_TRUE(depth.valid);
  expectUv(color.uv, 0.5f, 0.5f);
  expectUv(depth.uv, 0.4996875f, 0.500625f);
}

TEST(TemporalConventions, MovingSurfaceAppliesJitterCorrectionExactlyOnce) {
  const auto motion = temporal::motionFromUnjitteredClips(
      {0.0f, -1.0f, 0.9f, 2.0f}, {-1.0f, 1.0f, 0.9f, 2.0f}, true);
  const auto currentJitter = temporal::makeJitter({0.25f, 0.5f}, {800u, 400u});
  const auto previousJitter =
      temporal::makeJitter({-0.25f, -0.5f}, {800u, 400u});
  const glm::vec2 currentRasterUv(0.5003125f, 0.75125f);
  const auto color = temporal::reprojectToPreviousGrid(
      currentRasterUv, motion, currentJitter.uv, {0.0f, 0.0f});
  const auto depth = temporal::reprojectToPreviousGrid(
      currentRasterUv, motion, currentJitter.uv, previousJitter.uv);
  ASSERT_TRUE(color.valid);
  ASSERT_TRUE(depth.valid);
  expectUv(color.uv, 0.25f, 0.25f);
  expectUv(depth.uv, 0.2496875f, 0.24875f);
}

TEST(TemporalConventions, HomogeneousScaleDoesNotChangeMotionOrDepth) {
  const auto motion = temporal::motionFromUnjitteredClips(
      {0.0f, 0.0f, 0.5f, 1.0f}, {0.0f, 0.0f, 5.0f, 10.0f}, true);
  ASSERT_TRUE(motion.valid);
  expectUv(motion.uv, 0.0f, 0.0f);
  EXPECT_FLOAT_EQ(motion.previousReverseZDepth, 0.5f);
}

TEST(TemporalConventions,
     OffScreenProjectionIsValidButItsHistoryAddressIsRejected) {
  const auto projected = temporal::projectSceneClip({4.0f, 0.0f, 0.5f, 1.0f});
  ASSERT_TRUE(projected.valid);
  expectUv(projected.uv, 2.5f, 0.5f);
  const auto motion = temporal::motionFromUnjitteredClips(
      {0.0f, 0.0f, 0.5f, 1.0f}, {4.0f, 0.0f, 0.5f, 1.0f}, true);
  ASSERT_TRUE(motion.valid);
  expectUv(motion.uv, 2.0f, 0.0f); // Signed velocity is never saturated.
  const auto address = temporal::reprojectToPreviousGrid(
      {0.5f, 0.5f}, motion, {0.0f, 0.0f}, {0.0f, 0.0f});
  EXPECT_FALSE(address.valid);
  expectUv(address.uv, 0.0f, 0.0f);
}

TEST(TemporalConventions, ImageBoundsAreLowerInclusiveUpperExclusive) {
  EXPECT_TRUE(temporal::insideImage({0.0f, 0.0f}));
  EXPECT_TRUE(temporal::insideImage({0.999f, 0.999f}));
  EXPECT_FALSE(temporal::insideImage({1.0f, 0.5f}));
  EXPECT_FALSE(temporal::insideImage({0.5f, 1.0f}));
  EXPECT_FALSE(temporal::insideImage({-0.001f, 0.5f}));
  EXPECT_FALSE(temporal::insideImage({0.5f, -0.001f}));
}

TEST(TemporalConventions, InvalidClipsYieldFiniteInvalidResults) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const float tiny = std::numeric_limits<float>::min();
  const float huge = std::numeric_limits<float>::max();
  const std::array<glm::vec4, 9> invalidClips{{{0.0f, 0.0f, 0.0f, 0.0f},
                                               {0.0f, 0.0f, 0.5f, -1.0f},
                                               {nan, 0.0f, 0.5f, 1.0f},
                                               {0.0f, infinity, 0.5f, 1.0f},
                                               {0.0f, 0.0f, nan, 1.0f},
                                               {0.0f, 0.0f, 0.5f, infinity},
                                               {0.0f, 0.0f, -0.01f, 1.0f},
                                               {0.0f, 0.0f, 1.01f, 1.0f},
                                               {huge, 0.0f, 0.0f, tiny}}};
  for (std::size_t i = 0; i < invalidClips.size(); ++i) {
    SCOPED_TRACE(i);
    const auto point = temporal::projectSceneClip(invalidClips[i]);
    EXPECT_FALSE(point.valid);
    expectUv(point.uv, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(point.reverseZDepth, 0.0f);
    for (const bool invalidateCurrent : {false, true}) {
      const glm::vec4 validClip(0.0f, 0.0f, 0.5f, 1.0f);
      const auto motion = temporal::motionFromUnjitteredClips(
          invalidateCurrent ? invalidClips[i] : validClip,
          invalidateCurrent ? validClip : invalidClips[i], true);
      EXPECT_FALSE(motion.valid);
      expectUv(motion.uv, 0.0f, 0.0f);
      EXPECT_FLOAT_EQ(motion.previousReverseZDepth, 0.0f);
    }
  }
}

TEST(TemporalConventions,
     InvalidSurfaceHistoryDoesNotMasqueradeAsStaticMotion) {
  const glm::vec4 clip(0.0f, 0.0f, 0.5f, 1.0f);
  const auto motion = temporal::motionFromUnjitteredClips(clip, clip, false);
  EXPECT_FALSE(motion.valid);
  expectUv(motion.uv, 0.0f, 0.0f);
  EXPECT_FALSE(temporal::reprojectToPreviousGrid({0.5f, 0.5f}, motion,
                                                 {0.0f, 0.0f}, {0.0f, 0.0f})
                   .valid);
}

TEST(TemporalConventions, InvalidJitterAndAddressesCannotProduceNanHistory) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float huge = std::numeric_limits<float>::max();
  for (const auto extent : {glm::uvec2(0u, 400u), glm::uvec2(800u, 0u)}) {
    const auto jitter = temporal::makeJitter({0.25f, 0.5f}, extent);
    EXPECT_FALSE(jitter.valid);
    expectUv(jitter.uv, 0.0f, 0.0f);
    expectUv(jitter.ndc, 0.0f, 0.0f);
  }
  EXPECT_FALSE(temporal::makeJitter({nan, 0.0f}, {800u, 400u}).valid);
  EXPECT_FALSE(temporal::makeJitter({huge, 0.0f}, {1u, 1u}).valid);
  const temporal::MotionVector motion{{0.0f, 0.0f}, 0.5f, true};
  for (const auto address :
       {temporal::reprojectToPreviousGrid({nan, 0.5f}, motion, {}, {}),
        temporal::reprojectToPreviousGrid({0.5f, 0.5f}, motion, {nan, 0.0f},
                                          {}),
        temporal::reprojectToPreviousGrid({0.5f, 0.5f}, motion, {},
                                          {0.0f, nan}),
        temporal::reprojectToPreviousGrid(
            {0.5f, 0.5f}, {{huge, huge}, 0.5f, true}, {}, {huge, huge})}) {
    EXPECT_FALSE(address.valid);
    expectUv(address.uv, 0.0f, 0.0f);
  }
}

TEST(TemporalConventions,
     PreExposureConvertsOldStorageToCurrentRadianceDomain) {
  // Radiance 4 stored with old pre-exposure 2 is 8; new pre-exposure 0.5 is 2.
  const auto scale = temporal::historyExposureScale(0.5f, 2.0f);
  ASSERT_TRUE(scale.valid);
  EXPECT_FLOAT_EQ(8.0f * scale.value, 2.0f);
  EXPECT_FLOAT_EQ(temporal::historyExposureScale(1.0f, 1.0f).value, 1.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  const float huge = std::numeric_limits<float>::max();
  const float tiny = std::numeric_limits<float>::min();
  for (const auto [current, previous] :
       std::array<std::pair<float, float>, 7>{{{0.0f, 1.0f},
                                               {-1.0f, 1.0f},
                                               {1.0f, 0.0f},
                                               {nan, 1.0f},
                                               {1.0f, infinity},
                                               {huge, tiny},
                                               {tiny, huge}}}) {
    const auto invalid = temporal::historyExposureScale(current, previous);
    EXPECT_FALSE(invalid.valid);
    EXPECT_FLOAT_EQ(invalid.value, 0.0f);
  }
}

} // namespace
