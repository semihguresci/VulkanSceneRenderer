#include "Container/renderer/temporal/TemporalCapture.h"
#include <gtest/gtest.h>

namespace {
using container::temporal::CaptureSequence;
using nlohmann::json;
TEST(TemporalCapture, InterpolatesNamedObjectAndCameraAtSubmittedScriptFrame) {
  const auto fixture = json::parse(
      R"({"schemaVersion":1,"frames":9,"sampleFrames":[1,5,9],"objectName":"Moving cube",
    "camera":[{"frame":1,"position":[0,0,4],"target":[0,0,0]},{"frame":9,"position":[2,0,4],"target":[1,0,0]}],
    "objectTranslation":[{"frame":1,"translation":[0,0,0]},{"frame":9,"translation":[4,0,0]}],
    "events":[{"frame":5,"reset":true,"taa":false,"resize":[1280,720]}]})");
  CaptureSequence sequence(fixture);
  const auto middle = sequence.sample(5);
  ASSERT_TRUE(middle.camera);
  EXPECT_EQ(middle.camera->position, glm::vec3(1, 0, 4));
  EXPECT_EQ(middle.camera->target, glm::vec3(0.5f, 0, 0));
  ASSERT_TRUE(middle.objectTranslation);
  EXPECT_EQ(*middle.objectTranslation, glm::vec3(2, 0, 0));
  EXPECT_EQ(middle.objectName, "Moving cube");
  EXPECT_TRUE(middle.event.reset);
  ASSERT_TRUE(middle.event.taa);
  EXPECT_FALSE(*middle.event.taa);
  EXPECT_FALSE(sequence.sample(6).event.reset);
  EXPECT_TRUE(sequence.captures(5));
  EXPECT_FALSE(sequence.captures(6));
}
TEST(TemporalCapture, RejectsInvalidTracksAndEventsBeforeGpuExecution) {
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"rayShadows":"bad"}]})")),
      std::invalid_argument);
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"rayShadowSamples":33}]})")),
      std::invalid_argument);
  CaptureSequence ray(json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"rayShadows":"soft","rayShadowSamples":8,"rayShadowDenoise":false}]})"));
  EXPECT_EQ(ray.sample(1).event.rayShadows, "soft");
  EXPECT_EQ(ray.sample(1).event.rayShadowSamples, 8u);
  EXPECT_EQ(ray.sample(1).event.rayShadowDenoise, false);
  EXPECT_THROW(
      CaptureSequence(json::parse(R"({"schemaVersion":1,"frames":0})")),
      std::invalid_argument);
  EXPECT_THROW(CaptureSequence(json::parse(
                   R"({"schemaVersion":1,"frames":5,"sampleFrames":[6]})")),
               std::invalid_argument);
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":5,"events":[{"frame":2},{"frame":2}]})")),
      std::invalid_argument);
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":5,"events":[{"frame":1,"resize":[0,360]}]})")),
      std::invalid_argument);
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":5,"events":[{"frame":1,"technique":"bad"}]})")),
      std::invalid_argument);
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":5,"camera":[{"frame":1,"position":[0,0,0],"target":[0,0,0]}]})")),
      std::invalid_argument);
}
TEST(TemporalCapture, ClampsOutsideTracksAndPreservesExactResetFrame) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":10,
    "bimTranslation":[{"frame":3,"translation":[1,2,3]},{"frame":6,"translation":[4,5,6]}],
    "events":[{"frame":4,"skip":true},{"frame":7,"reset":true}]})"));
  EXPECT_EQ(*sequence.sample(1).bimTranslation, glm::vec3(1, 2, 3));
  EXPECT_EQ(*sequence.sample(10).bimTranslation, glm::vec3(4, 5, 6));
  EXPECT_TRUE(sequence.sample(4).event.skip);
  EXPECT_FALSE(sequence.sample(5).event.skip);
  EXPECT_TRUE(sequence.sample(7).event.reset);
  EXPECT_FALSE(sequence.sample(8).event.reset);
}

TEST(TemporalCapture, AreaLightingChangesApplyOnlyAtTheirNamedFrame) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":10,
    "events":[{"frame":4,"areaLighting":"sampled","areaLightSamples":64},
              {"frame":7,"areaLighting":"ltc"}]})"));
  EXPECT_EQ(sequence.sample(4).event.areaLighting, "sampled");
  EXPECT_EQ(sequence.sample(4).event.areaLightSamples, 64u);
  EXPECT_TRUE(sequence.sample(5).event.areaLighting.empty());
  EXPECT_FALSE(sequence.sample(5).event.areaLightSamples);
  EXPECT_EQ(sequence.sample(7).event.areaLighting, "ltc");
  EXPECT_THROW(CaptureSequence(json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"areaLighting":"invalid"}]})")),
      std::invalid_argument);
  EXPECT_THROW(CaptureSequence(json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"areaLightSamples":16}]})")),
      std::invalid_argument);
}
TEST(TemporalCapture, AcceptsExactAreaLightSampleCountsInNumericForms) {
  const auto fixture = json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
  for (const uint32_t count : {9u, 25u, 64u}) {
    // JSON Schema numeric enum values also accept an integral float such as
    // 9.0. Preserve that equivalence while checking the exact value.
    for (const auto &encoded :
         json::array({count, static_cast<double>(count)})) {
      SCOPED_TRACE(encoded.dump());
      auto input = fixture;
      input["events"][0]["areaLightSamples"] = encoded;
      CaptureSequence sequence(input);
      EXPECT_EQ(sequence.sample(1).event.areaLightSamples, count);
      EXPECT_FALSE(sequence.sample(2).event.areaLightSamples);
    }
  }
}
TEST(TemporalCapture, AcceptsSupportedAreaLightingModesAndOmission) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":4,
    "events":[{"frame":1,"areaLighting":"ltc"},
              {"frame":2,"areaLighting":"sampled"},
              {"frame":3,"areaLightSamples":25}]})"));
  EXPECT_EQ(sequence.sample(1).event.areaLighting, "ltc");
  EXPECT_EQ(sequence.sample(2).event.areaLighting, "sampled");
  EXPECT_TRUE(sequence.sample(3).event.areaLighting.empty());
  EXPECT_EQ(sequence.sample(3).event.areaLightSamples, 25u);
  EXPECT_TRUE(sequence.sample(4).event.areaLighting.empty());
}
TEST(TemporalCapture, RejectsEmptyUnsupportedAndNonStringAreaLightingModes) {
  const auto fixture = json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
  for (const char *encoded : {"\"\"", "\"invalid\"", "\"LTC\"", "\"sampled \"",
                              "0", "1.0", "true", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto input = fixture;
    input["events"][0]["areaLighting"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}
TEST(TemporalCapture, RejectsAreaLightSampleValuesBeforeNarrowing) {
  const auto fixture = json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
  for (const char *encoded : {"9.5", "25.9", "64.1", "4294967305",
                              "-4294967287", "-9", "0", "16", "65",
                              "\"9\"", "true", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto input = fixture;
    input["events"][0]["areaLightSamples"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}
TEST(TemporalCapture, AreaLightEventsAreFiniteAndApplyOnce) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"areaLightPosition":[0.3,1.8,0.2]}]})"));
  ASSERT_TRUE(sequence.sample(4).event.areaLightPosition);
  EXPECT_EQ(*sequence.sample(4).event.areaLightPosition,
            glm::vec3(.3f, 1.8f, .2f));
  EXPECT_FALSE(sequence.sample(5).event.areaLightPosition);
  EXPECT_THROW(CaptureSequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"areaLightPosition":[1,2]}]})")),
               std::invalid_argument);
  EXPECT_THROW(CaptureSequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"areaLightPosition":[1e39,2,3]}]})")),
               std::invalid_argument);
}
TEST(TemporalCapture, GuiReloadUsesAnExactEventAndRejectsConflictingReloads) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"guiReload":"models/hello-wall.ifcx"}]})"));
  EXPECT_EQ(sequence.sample(4).event.guiReload, "models/hello-wall.ifcx");
  EXPECT_TRUE(sequence.sample(4).event.reload.empty());
  EXPECT_TRUE(sequence.sample(5).event.guiReload.empty());
  EXPECT_THROW(CaptureSequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"reload":"a.ifc","guiReload":"b.ifcx"}]})")),
               std::invalid_argument);
  CaptureSequence sample(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"guiSampleModel":"IFC5 / Hello Wall / hello-wall"}]})"));
  EXPECT_EQ(sample.sample(4).event.guiSampleModel,
            "IFC5 / Hello Wall / hello-wall");
  EXPECT_TRUE(sample.sample(5).event.guiSampleModel.empty());
  EXPECT_THROW(CaptureSequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":4,"guiSampleModel":"sample","guiReload":"b.ifcx"}]})")),
               std::invalid_argument);
}
} // namespace
