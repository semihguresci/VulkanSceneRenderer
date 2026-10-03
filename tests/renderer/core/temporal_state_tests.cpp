#include "Container/common/CommonMath.h"
#include "Container/renderer/temporal/TemporalMaterialRevision.h"
#include "Container/renderer/temporal/TemporalState.h"

#include <gtest/gtest.h>

namespace {
using container::gpu::CameraData;
using container::gpu::ObjectData;
using container::temporal::Settings;
using container::temporal::State;

CameraData camera(float x = 0.0f) {
  CameraData value{};
  value.cameraWorldPosition.x = x;
  value.viewProj = container::math::perspectiveRH_ReverseZ(glm::radians(60.0f),
                                                           1.0f, 0.1f, 100.0f) *
                   glm::translate(glm::mat4(1), glm::vec3(-x, 0, 0));
  value.inverseViewProj = glm::inverse(value.viewProj);
  return value;
}
Settings enabled() {
  Settings settings;
  settings.enabled = true;
  return settings;
}

TEST(TemporalState, PendingPreparationDoesNotConsumeRenderedFrameOrJitter) {
  State state;
  auto first = camera();
  state.prepareCamera(first, {640, 360}, enabled());
  const auto jitter = first.jitterUv;
  auto skipped = camera(2);
  state.prepareCamera(skipped, {640, 360}, enabled());
  EXPECT_EQ(state.frameId(), 0u);
  EXPECT_EQ(skipped.jitterUv, jitter);
  EXPECT_EQ(skipped.temporalInfo.x, 0u);
  state.commit();
  auto next = camera(3);
  state.prepareCamera(next, {640, 360}, enabled());
  EXPECT_EQ(state.frameId(), 1u);
  EXPECT_EQ(next.previousViewProj, skipped.unjitteredViewProj);
  EXPECT_EQ(glm::vec2(next.jitterUv.z, next.jitterUv.w),
            glm::vec2(skipped.jitterUv));
}

TEST(TemporalState, PreviousObjectIsLastSubmissionAndIndependentOfDrawOrder) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {640, 360}, enabled());
  ObjectData a, b;
  a.model[3].x = 1;
  b.model[3].x = 9;
  state.prepareObject(a, {1, 101}, 7);
  state.prepareObject(b, {1, 202}, 8);
  EXPECT_EQ(a.temporalInfo.y, 0u);
  const auto token = a.temporalInfo.x;
  state.commit();
  view = camera();
  state.prepareCamera(view, {640, 360}, enabled());
  b.model[3].x = 11;
  a.model[3].x = 3;
  state.prepareObject(b, {1, 202}, 8);
  state.prepareObject(a, {1, 101}, 7);
  EXPECT_EQ(a.previousModel[3].x, 1);
  EXPECT_EQ(b.previousModel[3].x, 9);
  EXPECT_EQ(a.temporalInfo.x, token);
  EXPECT_EQ(a.temporalInfo.y, 1u);
  // A second pending simulation update must still see the submitted transform.
  a.model[3].x = 4;
  state.prepareObject(a, {1, 101}, 7);
  EXPECT_EQ(a.previousModel[3].x, 1);
}

TEST(TemporalState, ProviderAndLifetimeIdentityPreventBorrowedHistory) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  ObjectData object;
  object.model[3].x = 5;
  state.prepareObject(object, {1, 10}, 0);
  const auto token = object.temporalInfo.x;
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  ObjectData replacement;
  state.prepareObject(replacement, {2, 10}, 0);
  EXPECT_EQ(replacement.temporalInfo.y, 0u);
  EXPECT_NE(replacement.temporalInfo.x, token);
  state.prepareObject(replacement, {1, 11}, 0);
  EXPECT_EQ(replacement.temporalInfo.y, 0u);
  EXPECT_EQ(replacement.previousModel, replacement.model);
}

TEST(TemporalState, MaterialOrTopologyRevisionInvalidatesOnlyAffectedObject) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  ObjectData a, b;
  state.prepareObject(a, {1, 10}, 5);
  state.prepareObject(b, {2, 10}, 5);
  const auto oldToken = a.temporalInfo.x;
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.prepareObject(a, {1, 10}, 6);
  state.prepareObject(b, {2, 10}, 5);
  EXPECT_EQ(a.temporalInfo.y, 0u);
  EXPECT_NE(a.temporalInfo.x, oldToken);
  EXPECT_EQ(b.temporalInfo.y, 1u);
}

TEST(TemporalState, DeletedThenReappearingIdentityIsNewSurface) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  ObjectData object;
  state.prepareObject(object, {1, 10}, 5);
  const auto old = object.temporalInfo.x;
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.prepareObject(object, {1, 10}, 5);
  EXPECT_EQ(object.temporalInfo.y, 0u);
  EXPECT_NE(object.temporalInfo.x, old);
}

TEST(TemporalState, CullingDoesNotRemoveSnapshotIfObjectStillExists) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  ObjectData object;
  object.model[3].x = 2;
  state.prepareObject(object, {1, 10}, 5);
  state.commit();
  // Snapshot preparation covers the object list, independent of visible draws.
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  object.model[3].x = 3;
  state.prepareObject(object, {1, 10}, 5);
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  object.model[3].x = 4;
  state.prepareObject(object, {1, 10}, 5);
  EXPECT_EQ(object.previousModel[3].x, 3);
  EXPECT_EQ(object.temporalInfo.y, 1u);
}

TEST(TemporalState, JitterIsBoundedDeterministicAndPhysicalMotionIsZero) {
  State state;
  for (uint32_t frame = 0; frame < 64; ++frame) {
    auto view = camera();
    state.prepareCamera(view, {1920, 1080}, enabled());
    const auto jitter = container::temporal::jitterPixels(frame, 0);
    EXPECT_LE(std::abs(jitter.x), 0.5f);
    EXPECT_LE(std::abs(jitter.y), 0.5f);
    EXPECT_EQ(jitter, container::temporal::jitterPixels(frame + 32, 0));
    if (frame != 0) {
      const glm::vec4 position(0.25f, 0.1f, -2, 1);
      const auto motion = container::temporal::motionFromUnjitteredClips(
          view.unjitteredViewProj * position, view.previousViewProj * position,
          true);
      ASSERT_TRUE(motion.valid);
      EXPECT_EQ(motion.uv, glm::vec2(0));
    }
    state.commit();
  }
}

TEST(TemporalState, DisabledRouteHasExactZeroJitterAndOriginalProjection) {
  State state;
  auto view = camera();
  const auto original = view.viewProj;
  state.prepareCamera(view, {64, 32}, Settings{});
  EXPECT_EQ(view.viewProj, original);
  EXPECT_EQ(view.jitterUv, glm::vec4(0));
  EXPECT_EQ(view.temporalInfo.w, 0u);
}

TEST(TemporalState, EverySeededJitterCycleHasZeroMean) {
  for (uint32_t seed : {0u, 1u, 17u, 1000u}) {
    glm::vec2 sum{};
    for (uint32_t frame = 0; frame < 32; ++frame)
      sum += container::temporal::jitterPixels(frame, seed);
    EXPECT_NEAR(sum.x, 0.0f, 1e-5f);
    EXPECT_NEAR(sum.y, 0.0f, 1e-5f);
  }
}

TEST(TemporalState, ExtentProjectionSeedAndCameraCutResetHistory) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.commit();
  auto checkReset = [&](glm::uvec2 extent, Settings settings, float x,
                        glm::mat4 projection) {
    const auto epoch = state.epoch();
    view = camera(x);
    state.prepareCamera(view, extent, settings, projection);
    EXPECT_GT(state.epoch(), epoch);
    EXPECT_EQ(view.temporalInfo.x, 0u);
    state.commit();
  };
  checkReset({128, 64}, enabled(), 0, glm::mat4(1));
  checkReset({128, 64}, enabled(), 0, glm::mat4(2));
  auto settings = enabled();
  settings.jitterSeed = 13;
  checkReset({128, 64}, settings, 0, glm::mat4(2));
  checkReset({128, 64}, settings, 20, glm::mat4(2));
}

TEST(TemporalState, SettingsRejectIncompatibleMsaaAndNonfiniteWeights) {
  EXPECT_THROW(container::temporal::validateSettings(enabled(), 4),
               std::invalid_argument);
  auto settings = enabled();
  settings.historyWeight = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(container::temporal::validateSettings(settings, 1),
               std::invalid_argument);
  settings = enabled();
  settings.historyWeight = 1;
  EXPECT_THROW(container::temporal::validateSettings(settings, 1),
               std::invalid_argument);
  settings = enabled();
  settings.depthAbsoluteTolerance = -1;
  EXPECT_THROW(container::temporal::validateSettings(settings, 1),
               std::invalid_argument);
  settings = enabled();
  settings.varianceGamma = 0;
  EXPECT_THROW(container::temporal::validateSettings(settings, 1),
               std::invalid_argument);
  EXPECT_NO_THROW(container::temporal::validateSettings(Settings{}, 4));
}

TEST(TemporalState, DiscardedPreparationDoesNotCommitAnUnsubmittedObject) {
  State state;
  auto view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  ObjectData a, b;
  state.prepareObject(a, {1, 1}, 0);
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.prepareObject(a, {1, 1}, 0);
  state.prepareObject(b, {1, 2}, 0);
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.prepareObject(a, {1, 1}, 0);
  state.commit();
  view = camera();
  state.prepareCamera(view, {64, 64}, enabled());
  state.prepareObject(b, {1, 2}, 0);
  EXPECT_EQ(b.temporalInfo.y, 0u);
}

TEST(TemporalState, MaterialPaddingDoesNotCauseUnrelatedHistoryRejection) {
  container::gpu::GpuMaterial a{}, b = a;
  auto *bytes = reinterpret_cast<unsigned char *>(&b);
  for (size_t index =
           offsetof(container::gpu::GpuMaterial, iridescenceThicknessMaximum) +
           sizeof(float);
       index < offsetof(container::gpu::GpuMaterial, specularColorFactor);
       ++index)
    bytes[index] = 0xa5;
  EXPECT_EQ(container::temporal::materialRevision(a),
            container::temporal::materialRevision(b));
  b.baseColorTextureTransform.row0.x = 2;
  EXPECT_NE(container::temporal::materialRevision(a),
            container::temporal::materialRevision(b));
}
} // namespace
