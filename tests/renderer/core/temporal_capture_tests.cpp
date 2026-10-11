#include "Container/renderer/temporal/TemporalCapture.h"
#include <gtest/gtest.h>

#include <bit>
#include <cstdint>

namespace {
using container::temporal::CaptureSequence;
using nlohmann::json;

void expectSameVectorBits(const glm::vec3 &actual, const glm::vec3 &expected) {
  for (int axis = 0; axis < 3; ++axis) {
    SCOPED_TRACE(axis);
    EXPECT_EQ(std::bit_cast<uint32_t>(actual[axis]),
              std::bit_cast<uint32_t>(expected[axis]));
  }
}

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

TEST(TemporalCapture, ConstantNonzeroTracksPreserveBitsAcrossSixtyFrames) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":60,
    "camera":[{"frame":1,"position":[2.15,-0.35,4.05],"target":[0.05,0.65,-0.15]},
              {"frame":60,"position":[2.15,-0.35,4.05],"target":[0.05,0.65,-0.15]}],
    "objectTranslation":[{"frame":1,"translation":[0.05,-0.35,2.15]},
                         {"frame":60,"translation":[0.05,-0.35,2.15]}],
    "bimTranslation":[{"frame":1,"translation":[-0.15,1.8,0.2]},
                      {"frame":60,"translation":[-0.15,1.8,0.2]}]})"));
  const glm::vec3 position(2.15f, -0.35f, 4.05f);
  const glm::vec3 target(0.05f, 0.65f, -0.15f);
  const glm::vec3 object(0.05f, -0.35f, 2.15f);
  const glm::vec3 bim(-0.15f, 1.8f, 0.2f);
  for (uint32_t frame = 1; frame <= 60; ++frame) {
    SCOPED_TRACE(frame);
    const auto sample = sequence.sample(frame);
    ASSERT_TRUE(sample.camera);
    ASSERT_TRUE(sample.objectTranslation);
    ASSERT_TRUE(sample.bimTranslation);
    expectSameVectorBits(sample.camera->position, position);
    expectSameVectorBits(sample.camera->target, target);
    expectSameVectorBits(*sample.objectTranslation, object);
    expectSameVectorBits(*sample.bimTranslation, bim);
  }
}

TEST(TemporalCapture, MovingTracksKeepStationaryComponentsAndExactKeys) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":60,
    "camera":[{"frame":1,"position":[0.35,2.15,4.05],"target":[0.05,0.35,0.15]},
              {"frame":31,"position":[1.35,2.15,4.05],"target":[0.05,0.35,0.15]},
              {"frame":60,"position":[2.35,2.15,4.05],"target":[0.05,0.35,0.15]}],
    "objectTranslation":[{"frame":1,"translation":[0.05,0.35,2.15]},
                         {"frame":31,"translation":[0.15,0.35,2.15]},
                         {"frame":60,"translation":[0.25,0.35,2.15]}]})"));
  const glm::vec3 target(0.05f, 0.35f, 0.15f);
  for (uint32_t frame = 1; frame <= 60; ++frame) {
    SCOPED_TRACE(frame);
    const auto sample = sequence.sample(frame);
    ASSERT_TRUE(sample.camera);
    ASSERT_TRUE(sample.objectTranslation);
    expectSameVectorBits(sample.camera->target, target);
    EXPECT_EQ(std::bit_cast<uint32_t>(sample.camera->position.y),
              std::bit_cast<uint32_t>(2.15f));
    EXPECT_EQ(std::bit_cast<uint32_t>(sample.camera->position.z),
              std::bit_cast<uint32_t>(4.05f));
    EXPECT_EQ(std::bit_cast<uint32_t>(sample.objectTranslation->y),
              std::bit_cast<uint32_t>(0.35f));
    EXPECT_EQ(std::bit_cast<uint32_t>(sample.objectTranslation->z),
              std::bit_cast<uint32_t>(2.15f));
  }
  expectSameVectorBits(sequence.sample(1).camera->position,
                       glm::vec3(0.35f, 2.15f, 4.05f));
  expectSameVectorBits(sequence.sample(31).camera->position,
                       glm::vec3(1.35f, 2.15f, 4.05f));
  expectSameVectorBits(sequence.sample(60).camera->position,
                       glm::vec3(2.35f, 2.15f, 4.05f));
  expectSameVectorBits(*sequence.sample(31).objectTranslation,
                       glm::vec3(0.15f, 0.35f, 2.15f));
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
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"areaLighting":"invalid"}]})")),
      std::invalid_argument);
  EXPECT_THROW(
      CaptureSequence(json::parse(
          R"({"schemaVersion":1,"frames":2,"events":[{"frame":1,"areaLightSamples":16}]})")),
      std::invalid_argument);
}
TEST(TemporalCapture, AcceptsExactAreaLightSampleCountsInNumericForms) {
  const auto fixture =
      json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
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
  const auto fixture =
      json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
  for (const char *encoded : {"\"\"", "\"invalid\"", "\"LTC\"", "\"sampled \"",
                              "0", "1.0", "true", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto input = fixture;
    input["events"][0]["areaLighting"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}
TEST(TemporalCapture, RejectsAreaLightSampleValuesBeforeNarrowing) {
  const auto fixture =
      json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
  for (const char *encoded :
       {"9.5", "25.9", "64.1", "4294967305", "-4294967287", "-9", "0", "16",
        "65", "\"9\"", "true", "null", "[]", "{}"}) {
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
TEST(TemporalCapture, DdgiFlagsApplyOnceIncludingFalse) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":4,
    "events":[{"frame":1,"ddgiPaused":true,"ddgiEnabled":false},
              {"frame":3,"ddgiPaused":false,"ddgiEnabled":true}]})"));
  EXPECT_EQ(sequence.sample(1).event.ddgiPaused, true);
  EXPECT_EQ(sequence.sample(1).event.ddgiEnabled, false);
  EXPECT_FALSE(sequence.sample(2).event.ddgiPaused.has_value());
  EXPECT_FALSE(sequence.sample(2).event.ddgiEnabled.has_value());
  EXPECT_EQ(sequence.sample(3).event.ddgiPaused, false);
  EXPECT_EQ(sequence.sample(3).event.ddgiEnabled, true);
}
TEST(TemporalCapture, DdgiSceneFitIsAnExactEventWithoutImplicitEnable) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":4,
    "events":[{"frame":2,"ddgiFitProbes":1024}]})"));
  EXPECT_FALSE(sequence.sample(1).event.ddgiFitProbes);
  EXPECT_EQ(sequence.sample(2).event.ddgiFitProbes, 1024u);
  EXPECT_FALSE(sequence.sample(2).event.ddgiEnabled.has_value());
  EXPECT_FALSE(sequence.sample(3).event.ddgiFitProbes);
}

TEST(TemporalCapture, RejectsNonIntegerOrInsufficientDdgiFitBudgets) {
  for (const char *encoded : {"26", "0", "-1", "27.5", "4294967296", "true",
                              "\"1024\"", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto input = json::parse(
        R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
    input["events"][0]["ddgiFitProbes"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

TEST(TemporalCapture, AcceptsExactIntegerDdgiFitBudgetsBeforeNarrowing) {
  for (const uint32_t budget : {27u, 1024u, 4294967295u}) {
    for (const auto &encoded : json::array({budget, double(budget)})) {
      auto input = json::parse(
          R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
      input["events"][0]["ddgiFitProbes"] = encoded;
      CaptureSequence sequence(input);
      EXPECT_EQ(sequence.sample(1).event.ddgiFitProbes, budget);
      EXPECT_FALSE(sequence.sample(2).event.ddgiFitProbes);
    }
  }
}

TEST(TemporalCapture, RejectsNonBooleanDdgiFlags) {
  for (const char *name : {"ddgiPaused", "ddgiEnabled"}) {
    for (const char *encoded : {"0", "1", "1.0", "\"true\"", "null", "[]", "{}"}) {
      SCOPED_TRACE(name);
      SCOPED_TRACE(encoded);
      auto input = json::parse(
          R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
      input["events"][0][name] = json::parse(encoded);
      EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
    }
  }
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

TEST(TemporalCapture, GuiUnloadIsAnExactBooleanEventAndFalseDoesNotUnload) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":3,"guiUnload":true},{"frame":6,"guiUnload":false}]})"));
  EXPECT_FALSE(sequence.sample(2).event.guiUnload);
  EXPECT_TRUE(sequence.sample(3).event.guiUnload);
  EXPECT_FALSE(sequence.sample(4).event.guiUnload);
  EXPECT_FALSE(sequence.sample(6).event.guiUnload);
  for (const auto &encoded : {"0", "1", "\"true\"", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto invalid = json::parse(R"({"schemaVersion":1,"frames":8,"events":[{"frame":3}]})");
    invalid["events"][0]["guiUnload"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{invalid}, std::invalid_argument);
  }
}

TEST(TemporalCapture, GuiUnloadRejectsEveryConflictingLoadRoute) {
  for (const auto *route : {"reload", "guiReload", "guiSampleModel"}) {
    SCOPED_TRACE(route);
    auto conflict = json::parse(R"({"schemaVersion":1,"frames":8,
      "events":[{"frame":3,"guiUnload":true}]})");
    conflict["events"][0][route] = "model.ifcx";
    EXPECT_THROW(CaptureSequence{conflict}, std::invalid_argument);
    conflict["events"][0]["guiUnload"] = false;
    EXPECT_NO_THROW(CaptureSequence{conflict});
  }
}

TEST(TemporalCapture, GuiModelActionsSettleByDefaultAndCanRemainAsynchronousOnce) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":2,"guiReload":"model.gltf"},
               {"frame":4,"guiReload":"large.ifcx","guiWaitForLoad":false},
               {"frame":6,"guiUnload":true}]})"));
  EXPECT_TRUE(sequence.sample(2).event.guiWaitForLoad);
  EXPECT_FALSE(sequence.sample(4).event.guiWaitForLoad);
  EXPECT_TRUE(sequence.sample(5).event.guiWaitForLoad);
  EXPECT_TRUE(sequence.sample(6).event.guiWaitForLoad);
  for (const char *action : {"guiReload", "guiSampleModel", "guiUnload"}) {
    auto input = json::parse(R"({"schemaVersion":1,"frames":8,
      "events":[{"frame":3,"guiWaitForLoad":false}]})");
    input["events"][0][action] = std::string(action) == "guiUnload"
                                    ? json(true)
                                    : json("model");
    EXPECT_NO_THROW(CaptureSequence{input});
  }
}

TEST(TemporalCapture, RejectsNonBooleanGuiWaitForLoad) {
  for (const char *encoded : {"0", "1", "1.0", "\"true\"", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto input = json::parse(R"({"schemaVersion":1,"frames":8,
      "events":[{"frame":3,"guiReload":"model.gltf"}]})");
    input["events"][0]["guiWaitForLoad"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

TEST(TemporalCapture, AsynchronousCaptureRequiresActualGuiModelAction) {
  for (const char *extra : {"{}", R"({"reload":"model.gltf"})",
                           R"({"guiUnload":false})", R"({"guiReload":""})",
                           R"({"guiSampleModel":""})"}) {
    auto input = json::parse(R"({"schemaVersion":1,"frames":8,
      "events":[{"frame":3,"guiWaitForLoad":false}]})");
    input["events"][0].update(json::parse(extra));
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
    input["events"][0]["guiWaitForLoad"] = true;
    EXPECT_NO_THROW(CaptureSequence{input});
  }
}

TEST(TemporalCapture, BimSelectionIsAnExactEventWithAValidUint32Index) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":8,
    "events":[{"frame":3,"bimSelectedObject":0},
               {"frame":6,"bimSelectedObject":4294967295}]})"));
  EXPECT_FALSE(sequence.sample(2).event.bimSelectedObject.has_value());
  ASSERT_TRUE(sequence.sample(3).event.bimSelectedObject.has_value());
  EXPECT_EQ(*sequence.sample(3).event.bimSelectedObject, 0u);
  EXPECT_FALSE(sequence.sample(4).event.bimSelectedObject.has_value());
  ASSERT_TRUE(sequence.sample(6).event.bimSelectedObject.has_value());
  EXPECT_EQ(*sequence.sample(6).event.bimSelectedObject, 4294967295u);
  for (const auto &encoded : {"-1", "1.5", "4294967296", "true", "\"0\"", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    auto invalid = json::parse(R"({"schemaVersion":1,"frames":8,"events":[{"frame":3}]})");
    invalid["events"][0]["bimSelectedObject"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{invalid}, std::invalid_argument);
  }
}
} // namespace
