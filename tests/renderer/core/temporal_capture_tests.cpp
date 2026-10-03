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
} // namespace
