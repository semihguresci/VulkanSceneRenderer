#include "Container/renderer/temporal/TemporalCapture.h"
#include <gtest/gtest.h>

#include <bit>
#include <array>
#include <cstdint>
#include <limits>
#include <cmath>

namespace {
using container::temporal::CaptureSequence;
using nlohmann::json;

TEST(TemporalCapture, ReferenceRendererSwitchIsOneShotAndRejectsUnknownNames) {
  const json input{{"schemaVersion",1},{"frames",3},{"events",json::array({{{"frame",2},{"technique","path-tracing"}}})}};
  CaptureSequence sequence(input);
  EXPECT_TRUE(sequence.sample(1).event.technique.empty());
  EXPECT_EQ(sequence.sample(2).event.technique,"path-tracing");
  EXPECT_TRUE(sequence.sample(3).event.technique.empty());
  auto invalid=input;invalid["events"][0]["technique"]="missing-renderer";
  EXPECT_THROW(CaptureSequence{invalid},std::invalid_argument);
}

TEST(TemporalCapture, ReferenceDisplaySwitchIsOneShotAndRejectsInvalidModes) {
  using container::renderer::ReferencePathDisplayMode;
  auto input = json::parse(R"({"schemaVersion":1,"frames":4,"events":[
    {"frame":2,"referenceDisplay":"standard-error"},{"frame":4,"referenceDisplay":"beauty"}]})");
  const CaptureSequence sequence(input);
  EXPECT_FALSE(sequence.sample(1).event.referenceDisplay);
  EXPECT_EQ(sequence.sample(2).event.referenceDisplay, ReferencePathDisplayMode::StandardError);
  EXPECT_FALSE(sequence.sample(3).event.referenceDisplay);
  EXPECT_EQ(sequence.sample(4).event.referenceDisplay, ReferencePathDisplayMode::Beauty);

  for (const auto& invalid : {json("variance"), json(1), json(nullptr)}) {
    input["events"][0]["referenceDisplay"] = invalid;
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

TEST(TemporalCapture, BimFloorPlanReplacesOverlayStateOnItsNamedFrames) {
  using Elevation = container::temporal::CaptureBimFloorPlan::Elevation;
  const CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":5,"events":[
    {"frame":2,"bimFloorPlan":{"enabled":true,"elevation":"source-elevation",
      "depthTest":false,"color":[1,0.25,0],"opacity":1,"lineWidth":8}},
    {"frame":4,"technique":"path-tracing","bimFloorPlan":{"enabled":false,
      "elevation":"projected-ground","depthTest":true,"color":[0,0,1],"opacity":0.05,"lineWidth":1}}
  ]})"));
  EXPECT_FALSE(sequence.sample(1).event.bimFloorPlan);
  const auto first = sequence.sample(2).event.bimFloorPlan;
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->enabled);
  EXPECT_FALSE(first->depthTest);
  EXPECT_EQ(first->elevation, Elevation::SourceElevation);
  EXPECT_EQ(first->color, glm::vec3(1.0f, 0.25f, 0.0f));
  EXPECT_FLOAT_EQ(first->opacity, 1.0f);
  EXPECT_FLOAT_EQ(first->lineWidth, 8.0f);
  EXPECT_FALSE(sequence.sample(3).event.bimFloorPlan);
  const auto last = sequence.sample(4).event.bimFloorPlan;
  ASSERT_TRUE(last);
  EXPECT_FALSE(last->enabled);
  EXPECT_TRUE(last->depthTest);
  EXPECT_EQ(last->elevation, Elevation::ProjectedGround);
  EXPECT_EQ(last->color, glm::vec3(0.0f, 0.0f, 1.0f));
  EXPECT_FLOAT_EQ(last->opacity, 0.05f);
  EXPECT_FLOAT_EQ(last->lineWidth, 1.0f);
  EXPECT_FALSE(sequence.sample(5).event.bimFloorPlan);
}

TEST(TemporalCapture, RejectsInvalidBimFloorPlanFieldsAndBounds) {
  auto input = json::parse(R"({"schemaVersion":1,"frames":1,"events":[
    {"frame":1,"bimFloorPlan":{"enabled":true,"elevation":"source-elevation",
      "depthTest":false,"color":[1,0.25,0],"opacity":0.5,"lineWidth":2}}
  ]})");
  auto &floorPlan = input["events"][0]["bimFloorPlan"];
  const auto valid = floorPlan;
  for (const auto &[field, encoded] : std::array{
           std::pair{"enabled", "1"}, std::pair{"depthTest", "null"},
           std::pair{"elevation", R"("ground")"}, std::pair{"elevation", "0"},
           std::pair{"color", "[1,0]"}, std::pair{"color", "[1,0,false]"},
           std::pair{"color", "[1,0,-0.01]"}, std::pair{"color", "[1.01,0,0]"},
           std::pair{"opacity", "0.049999999"}, std::pair{"opacity", "1.1"},
           std::pair{"lineWidth", "0.9"}, std::pair{"lineWidth", "8.1"}}) {
    SCOPED_TRACE(field);
    floorPlan = valid;
    floorPlan[field] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
  floorPlan = valid;
  floorPlan.erase("color");
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  floorPlan["colour"] = valid["color"];
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  floorPlan = valid;
  floorPlan["opacity"] = std::numeric_limits<double>::infinity();
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
}

TEST(TemporalCapture, FsrModesSelectIndependentSceneGrids) {
  using container::renderer::UpscaleBackend;
  using container::renderer::UpscaleMode;
  using container::renderer::UpscaleSelection;
  using container::renderer::fsrRenderExtent;
  auto input = json::parse(R"({"schemaVersion":1,"frames":4,"events":[
    {"frame":2,"fsr":"balanced"},{"frame":4,"fsr":"native"}]})");
  const CaptureSequence sequence(input);
  EXPECT_FALSE(sequence.sample(1).event.upscaling);
  EXPECT_EQ(sequence.sample(2).event.upscaling, (UpscaleSelection{UpscaleBackend::Fsr, UpscaleMode::Balanced}));
  EXPECT_FALSE(sequence.sample(3).event.upscaling);
  EXPECT_EQ(sequence.sample(4).event.upscaling, (UpscaleSelection{UpscaleBackend::Fsr, UpscaleMode::Native}));
  const auto scaled = fsrRenderExtent(UpscaleMode::Balanced, {1281, 721});
  EXPECT_EQ(scaled.width, 753u);
  EXPECT_EQ(scaled.height, 424u);
  EXPECT_EQ(fsrRenderExtent(UpscaleMode::Native, {1281, 721}).height, 721u);
  input["events"][0]["fsr"] = "unknown";
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
}

TEST(TemporalCapture, DlssSelectionRejectsTwoUpscalersInOneEvent) {
  using namespace container::renderer;
  auto input = json::parse(R"({"schemaVersion":1,"frames":4,"events":[
    {"frame":1,"dlss":"quality"},{"frame":2,"fsr":"balanced"},{"frame":4,"dlss":"native"}]})");
  const CaptureSequence sequence(input);
  EXPECT_EQ(sequence.sample(1).event.upscaling, (UpscaleSelection{UpscaleBackend::Dlss, UpscaleMode::Quality}));
  EXPECT_EQ(sequence.sample(2).event.upscaling, (UpscaleSelection{UpscaleBackend::Fsr, UpscaleMode::Balanced}));
  EXPECT_FALSE(sequence.sample(3).event.upscaling);
  EXPECT_EQ(sequence.sample(4).event.upscaling, (UpscaleSelection{UpscaleBackend::Dlss, UpscaleMode::Native}));
  input["events"][0]["dlss"] = 2;
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  input["events"][0]["dlss"] = "performance";
  input["events"][0]["fsr"] = "native";
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
}

TEST(TemporalCapture, ReferenceDlssIsAnIndependentExclusiveDisplayBackend) {
  using container::renderer::UpscaleMode;
  auto input = json::parse(R"({"schemaVersion":1,"frames":4,"events":[
    {"frame":1,"referenceDlss":"quality"},{"frame":3,"referenceDlss":"off"}]})");
  const CaptureSequence sequence(input);
  EXPECT_EQ(sequence.sample(1).event.referenceDlss, UpscaleMode::Quality);
  EXPECT_FALSE(sequence.sample(1).event.upscaling);
  EXPECT_FALSE(sequence.sample(2).event.referenceDlss);
  EXPECT_EQ(sequence.sample(3).event.referenceDlss, UpscaleMode::Native);
  for (const auto* backend : {"referenceOidn", "referenceOptix"}) {
    auto conflict = input;
    conflict["events"][0][backend] = std::string_view(backend) == "referenceOidn" ? json("off") : json(false);
    EXPECT_THROW(CaptureSequence{conflict}, std::invalid_argument);
  }
  input["events"][0]["referenceDlss"] = "native";
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
}

json jointCapture() {
  return json::parse(R"({"schemaVersion":1,"frames":4,"events":[{"frame":2,
    "physicsJointCreate":{"type":"hinge","bodyA":{"node":0},"bodyB":{"bimObject":1},
      "anchor":[0,3,0],"axis":[0,0,2],"limits":[-0.5,0.5],
      "motor":{"mode":"velocity","targetVelocity":1,"maxForceOrTorque":100}}}]})");
}

TEST(TemporalCapture, BodySettingsPreserveMassCollisionAndVelocityFieldsForOneEvent) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":3,"events":[
    {"frame":2,"physicsAttach":"dynamic","physicsNode":0,"physicsSettings":{
      "mass":2.5,"friction":0.2,"restitution":0.8,"gravityFactor":0,"useCCD":true,
      "linearVelocity":[500,0,-1],"angularVelocity":[0,2,0]}}]})"));
  EXPECT_FALSE(sequence.sample(1).event.physicsSettings);
  const auto settings = sequence.sample(2).event.physicsSettings;
  ASSERT_TRUE(settings);
  EXPECT_FLOAT_EQ(settings->mass, 2.5f);
  EXPECT_FLOAT_EQ(settings->friction, .2f);
  EXPECT_FLOAT_EQ(settings->restitution, .8f);
  EXPECT_FLOAT_EQ(settings->gravityFactor, 0);
  EXPECT_TRUE(settings->useCCD);
  EXPECT_EQ(settings->linearVelocity, glm::vec3(500, 0, -1));
  EXPECT_EQ(settings->angularVelocity, glm::vec3(0, 2, 0));
  EXPECT_FALSE(sequence.sample(3).event.physicsSettings);
}

TEST(TemporalCapture, BodySettingsRequireAttachmentAndRejectUnknownFields) {
  for (const auto &attachment : {json(nullptr), json(""), json("detach")}) {
    json event{{"frame", 1}, {"physicsSettings", json::object()}};
    if (!attachment.is_null()) event["physicsAttach"] = attachment;
    EXPECT_THROW(CaptureSequence(json{{"schemaVersion", 1}, {"frames", 1},
                                      {"events", json::array({event})}}), std::invalid_argument);
  }
  for (const auto &invalid : {json(nullptr), json::array(), json{{"motionType", "dynamic"}},
          json{{"position", {0, 1, 0}}}, json{{"useCCD", 1}}, json{{"gravityFactor", true}},
          json{{"mass", 0}}, json{{"mass", 1e-40}}, json{{"mass", -1}},
          json{{"mass", 1e39}}, json{{"friction", -1}}, json{{"restitution", 1.1}},
          json{{"linearVelocity", {0, 1}}}, json{{"angularVelocity", {0, false, 1}}}}) {
    const json input{{"schemaVersion", 1}, {"frames", 1}, {"events", json::array({
        {{"frame", 1}, {"physicsAttach", "dynamic"}, {"physicsSettings", invalid}}})}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument) << invalid.dump();
  }
}

TEST(TemporalCapture, BodySettingsDefaultsAndNegativeGravityRemainExplicit) {
  const auto parsed = CaptureSequence(json::parse(R"({"schemaVersion":1,"frames":1,"events":[
    {"frame":1,"physicsAttach":"static","physicsSettings":{"gravityFactor":-2}}]})"))
      .sample(1).event.physicsSettings;
  ASSERT_TRUE(parsed);
  EXPECT_FLOAT_EQ(parsed->mass, 1);
  EXPECT_FLOAT_EQ(parsed->gravityFactor, -2);
  EXPECT_FALSE(parsed->useCCD);
  EXPECT_EQ(parsed->linearVelocity, glm::vec3(0));
}

TEST(TemporalCapture, JointCreationPreservesTypedSettingsAndNamedFrame) {
  using namespace container::physics;
  auto input = jointCapture();
  input["events"][0]["physicsAction"] = "play";
  CaptureSequence sequence(input);
  EXPECT_FALSE(sequence.sample(1).event.physicsJointCreate);
  const auto created = sequence.sample(2).event.physicsJointCreate;
  ASSERT_TRUE(created);
  EXPECT_EQ(created->bodyA.node, 0u);
  EXPECT_EQ(created->bodyB.bimObject, 1u);
  EXPECT_EQ(created->settings.type, PhysicsJointType::Hinge);
  EXPECT_EQ(created->settings.anchor, glm::dvec3(0, 3, 0));
  EXPECT_EQ(created->settings.axis, glm::vec3(0, 0, 1));
  EXPECT_NEAR(glm::dot(created->settings.axis, created->settings.normal), 0, 1e-6);
  EXPECT_TRUE(created->settings.limitsEnabled);
  EXPECT_FLOAT_EQ(created->settings.lowerLimit, -.5f);
  EXPECT_EQ(created->settings.motor.mode, PhysicsJointMotorMode::Velocity);
  EXPECT_FLOAT_EQ(created->settings.motor.targetVelocity, 1);
  EXPECT_FALSE(created->settings.collideConnected);
  EXPECT_FALSE(sequence.sample(3).event.physicsJointCreate);
}

TEST(TemporalCapture, JointWorldAnchorRetainsDoublePrecisionAndBodySelectors) {
  auto input = jointCapture();
  auto &joint = input["events"][0]["physicsJointCreate"];
  joint["bodyA"] = {{"world", true}};
  joint["bodyB"] = {{"node", 4294967295u}};
  joint["anchor"] = {10000000.25, 3, 0};
  const auto parsed = CaptureSequence(input).sample(2).event.physicsJointCreate;
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed->bodyA.world);
  EXPECT_FALSE(parsed->bodyA.node);
  EXPECT_EQ(parsed->bodyB.node, 4294967295u);
  EXPECT_DOUBLE_EQ(parsed->settings.anchor.x, 10000000.25);
}

TEST(TemporalCapture, JointMotorUpdatesAndRetirementAreExactEvents) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":5,"events":[
    {"frame":1,"physicsJointMotorVelocity":0},{"frame":3,"physicsJointAction":"remove"},
    {"frame":5,"physicsJointAction":"clear"}]})"));
  ASSERT_TRUE(sequence.sample(1).event.physicsJointMotorVelocity);
  EXPECT_FLOAT_EQ(*sequence.sample(1).event.physicsJointMotorVelocity, 0);
  EXPECT_FALSE(sequence.sample(2).event.physicsJointMotorVelocity);
  EXPECT_EQ(sequence.sample(3).event.physicsJointAction, "remove");
  EXPECT_TRUE(sequence.sample(4).event.physicsJointAction.empty());
  EXPECT_EQ(sequence.sample(5).event.physicsJointAction, "clear");
}

TEST(TemporalCapture, JointAxesNormalizeWithoutOverflowAndStayPerpendicular) {
  for (const auto &axis : {json::array({1e30, 0, 0}), json::array({0, 1e-20, 0}),
                          json::array({1, 2, 3})}) {
    auto input = jointCapture();
    input["events"][0]["physicsJointCreate"]["axis"] = axis;
    const auto settings = CaptureSequence(input).sample(2).event.physicsJointCreate->settings;
    EXPECT_NEAR(glm::length(settings.axis), 1, 1e-6);
    EXPECT_NEAR(glm::length(settings.normal), 1, 1e-6);
    EXPECT_NEAR(glm::dot(settings.axis, settings.normal), 0, 1e-6);
  }
}

TEST(TemporalCapture, RejectsMalformedAndAmbiguousJointEndpoints) {
  for (const auto &endpoint : {json::object(), json::array(), json(nullptr),
          json{{"world", false}}, json{{"world", 1}}, json{{"node", -1}},
          json{{"node", true}}, json{{"node", 1.5}}, json{{"node", 4294967296ull}},
          json{{"node", 0}, {"bimObject", 1}}, json{{"unknown", 1}}}) {
    auto input = jointCapture();
    input["events"][0]["physicsJointCreate"]["bodyA"] = endpoint;
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument) << endpoint.dump();
  }
  for (const auto &bodyB : {json{{"world", true}}, json{{"node", 0}}}) {
    auto input = jointCapture();
    input["events"][0]["physicsJointCreate"]["bodyB"] = bodyB;
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

TEST(TemporalCapture, RejectsInvalidJointVectorsLimitsAndTypes) {
  for (const auto &patch : {json{{"type", "spring"}}, json{{"type", true}},
          json{{"anchor", {0, 1}}}, json{{"anchor", {true, 0, 0}}},
          json{{"axis", {0, 0, 0}}}, json{{"axis", {1e39, 0, 0}}},
          json{{"limits", {-1, -0.1}}}, json{{"limits", {0.1, 1}}},
          json{{"limits", {-4, 4}}}, json{{"limits", {false, 1}}},
          json{{"collideConnected", 1}}, json{{"unknown", 1}}}) {
    auto input = jointCapture();
    input["events"][0]["physicsJointCreate"].update(patch);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument) << patch.dump();
  }
  for (const char *required : {"type", "bodyA", "bodyB", "anchor"}) {
    auto input = jointCapture();
    input["events"][0]["physicsJointCreate"].erase(required);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

TEST(TemporalCapture, RejectsInvalidJointMotorsAndConflictingCommands) {
  for (const auto &motor : {json(nullptr), json::object(), json{{"mode", true}},
          json{{"mode", "unknown"}}, json{{"mode", "velocity"}, {"targetVelocity", true}},
          json{{"mode", "velocity"}, {"maxForceOrTorque", -1}},
          json{{"mode", "position"}, {"frequencyHz", 0}},
          json{{"mode", "velocity"}, {"dampingRatio", -1}},
          json{{"mode", "velocity"}, {"targetPosition", 1e39}}}) {
    auto input = jointCapture();
    input["events"][0]["physicsJointCreate"]["motor"] = motor;
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument) << motor.dump();
  }
  for (const auto &conflict : {json{{"physicsJointAction", "remove"}},
          json{{"physicsJointAction", "clear"}}, json{{"physicsJointMotorVelocity", 0}},
          json{{"physicsAction", "reset"}}, json{{"physicsAction", "clear"}},
          json{{"physicsAction", "detach"}}}) {
    auto input = jointCapture();
    input["events"][0].update(conflict);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

TEST(TemporalCapture, FixedAndSliderJointSettingsHaveExplicitDefaults) {
  using namespace container::physics;
  for (const char *type : {"fixed", "slider"}) {
    auto input = jointCapture();
    auto &joint = input["events"][0]["physicsJointCreate"];
    joint["type"] = type;
    joint.erase("axis"); joint.erase("limits"); joint.erase("motor");
    const auto parsed = CaptureSequence(input).sample(2).event.physicsJointCreate;
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->settings.type, std::string(type) == "fixed" ? PhysicsJointType::Fixed : PhysicsJointType::Slider);
    EXPECT_EQ(parsed->settings.axis, glm::vec3(0, 1, 0));
    EXPECT_FALSE(parsed->settings.limitsEnabled);
    EXPECT_EQ(parsed->settings.motor.mode, PhysicsJointMotorMode::Off);
  }
}

TEST(TemporalCapture, JointLimitsRejectZeroWidthForHingeAndSlider) {
  for (const char *type : {"hinge", "slider"}) {
    auto input = jointCapture();
    auto &joint = input["events"][0]["physicsJointCreate"];
    joint["type"] = type;
    joint["limits"] = {0, 0};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
    joint.erase("limits");
    EXPECT_NO_THROW(CaptureSequence{input});
  }
}

TEST(TemporalCapture, PhysicsAttachmentAndPlaybackApplyOnlyAtNamedFrame) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":5,
    "events":[{"frame":2,"physicsAttach":"dynamic","physicsAction":"play"},
              {"frame":4,"physicsAction":"reset"}]})"));
  EXPECT_TRUE(sequence.sample(1).event.physicsAttach.empty());
  EXPECT_EQ(sequence.sample(2).event.physicsAttach, "dynamic");
  EXPECT_EQ(sequence.sample(2).event.physicsAction, "play");
  EXPECT_TRUE(sequence.sample(3).event.physicsAttach.empty());
  EXPECT_TRUE(sequence.sample(3).event.physicsAction.empty());
  EXPECT_EQ(sequence.sample(4).event.physicsAction, "reset");
  EXPECT_TRUE(sequence.sample(5).event.physicsAction.empty());
}

TEST(TemporalCapture, AcceptsEveryPhysicsCommandAndEmptySentinel) {
  for (const char *attach : {"", "static", "kinematic", "dynamic"}) {
    for (const char *action : {"", "play", "pause", "step"}) {
      auto input = json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
      input["events"][0]["physicsAttach"] = attach;
      input["events"][0]["physicsAction"] = action;
      CaptureSequence sequence(input);
      EXPECT_EQ(sequence.sample(1).event.physicsAttach, attach);
      EXPECT_EQ(sequence.sample(1).event.physicsAction, action);
      EXPECT_TRUE(sequence.sample(2).event.physicsAttach.empty());
      EXPECT_TRUE(sequence.sample(2).event.physicsAction.empty());
    }
  }
  for (const char *action : {"reset", "clear", "detach", "demo"}) {
    auto input = json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
    input["events"][0]["physicsAction"] = action;
    EXPECT_NO_THROW(CaptureSequence{input});
  }
}

TEST(TemporalCapture, RejectsUnsupportedOrNonStringPhysicsCommands) {
  for (const char *field : {"physicsAttach", "physicsAction"}) {
    for (const char *encoded : {"0", "1.0", "true", "false", "null", "[]", "{}",
                                "\"Dynamic\"", "\"dynamic \"", "\"unsupported\""}) {
      SCOPED_TRACE(field);
      SCOPED_TRACE(encoded);
      auto input = json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
      input["events"][0][field] = json::parse(encoded);
      EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
    }
  }
}

TEST(TemporalCapture, RejectsAttachmentAndConflictingPhysicsRetirement) {
  for (const char *attach : {"static", "kinematic", "dynamic"}) {
    for (const char *action : {"reset", "clear", "detach", "demo"}) {
      auto input = json::parse(R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
      input["events"][0]["physicsAttach"] = attach;
      input["events"][0]["physicsAction"] = action;
      EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
    }
  }
}

TEST(TemporalCapture, PhysicsNodeAcceptsZeroAndUint32BoundaryWithoutNarrowing) {
  for (uint32_t node : {0u, 1u, 4294967295u}) {
    for (const auto &encoded : json::array({node, double(node)})) {
      auto input = json::parse(R"({"schemaVersion":1,"frames":2,
        "events":[{"frame":1,"physicsAttach":"static"}]})");
      input["events"][0]["physicsNode"] = encoded;
      CaptureSequence sequence(input);
      ASSERT_TRUE(sequence.sample(1).event.physicsNode.has_value());
      EXPECT_EQ(*sequence.sample(1).event.physicsNode, node);
      EXPECT_FALSE(sequence.sample(2).event.physicsNode.has_value());
      input["events"][0].erase("physicsAttach");
      input["events"][0]["physicsAction"] = "detach";
      EXPECT_NO_THROW(CaptureSequence{input});
    }
  }
}

TEST(TemporalCapture, RejectsInvalidOrUnusedPhysicsNodeTargets) {
  for (const char *encoded : {"-1", "1.5", "4294967296", "true", "null", "\"0\"", "[]", "{}"}) {
    auto input = json::parse(R"({"schemaVersion":1,"frames":2,
      "events":[{"frame":1,"physicsAttach":"static"}]})");
    input["events"][0]["physicsNode"] = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
  for (const char *action : {"", "play", "pause", "step", "reset", "clear"}) {
    auto input = json::parse(R"({"schemaVersion":1,"frames":2,
      "events":[{"frame":1,"physicsNode":0}]})");
    input["events"][0]["physicsAction"] = action;
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
}

void expectSameVectorBits(const glm::vec3 &actual, const glm::vec3 &expected) {
  for (int axis = 0; axis < 3; ++axis) {
    SCOPED_TRACE(axis);
    EXPECT_EQ(std::bit_cast<uint32_t>(actual[axis]),
              std::bit_cast<uint32_t>(expected[axis]));
  }
}

TEST(TemporalCapture, ObjectScaleInterpolatesSignedMultipliersWithTranslation) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":9,
    "objectNode":3,"objectName":"Scaled source parent",
    "objectScale":[{"frame":1,"scale":[-1,1,2]},{"frame":9,"scale":[-3,5,6]}],
    "objectTranslation":[{"frame":1,"translation":[0,0,0]},{"frame":9,"translation":[8,4,-4]}]})"));
  const auto middle = sequence.sample(5);
  ASSERT_TRUE(middle.objectScale);
  ASSERT_TRUE(middle.objectTranslation);
  EXPECT_EQ(*middle.objectScale, glm::vec3(-2, 3, 4));
  EXPECT_EQ(*middle.objectTranslation, glm::vec3(4, 2, -2));
  EXPECT_EQ(middle.objectNode, 3u);
  EXPECT_EQ(middle.objectName, "Scaled source parent");
  EXPECT_EQ(*sequence.sample(1).objectScale, glm::vec3(-1, 1, 2));
  EXPECT_EQ(*sequence.sample(9).objectScale, glm::vec3(-3, 5, 6));
}

TEST(TemporalCapture, ObjectScaleDefaultsAndEndpointHoldsPreserveAuthoredAxes) {
  CaptureSequence absent(json{{"schemaVersion", 1}, {"frames", 9}});
  EXPECT_FALSE(absent.sample(5).objectScale);
  EXPECT_FALSE(absent.sample(5).objectTranslation);
  CaptureSequence empty(json{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", json::array()}});
  EXPECT_FALSE(empty.sample(5).objectScale);
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":9,
    "objectScale":[{"frame":3,"scale":[-0.05,1.005,2.15]},
                   {"frame":7,"scale":[-0.05,1.005,2.15]}]})"));
  for (uint32_t frame = 1; frame <= 9; ++frame) {
    const auto sample = sequence.sample(frame);
    ASSERT_TRUE(sample.objectScale);
    expectSameVectorBits(*sample.objectScale, glm::vec3(-.05f, 1.005f, 2.15f));
    EXPECT_FALSE(sample.objectTranslation);
  }
}

TEST(TemporalCapture, ObjectScaleRejectsZeroSignChangesAndMalformedTypedKeys) {
  const auto key = [](json scale) { return json{{"frame", 1}, {"scale", std::move(scale)}}; };
  for (const auto &invalid : {json(nullptr), json(true), json(1), json("1"), json::object(),
          json::array({1, 1}), json::array({1, 1, 1, 1}), json::array({1, false, 1}),
          json::array({1, "2", 1}), json::array({1, 0, 1}), json::array({1, -0.0, 1}),
          json::array({1, 1e-100, 1}), json::array({1, 1e39, 1}),
          json::array({1, std::numeric_limits<double>::infinity(), 1}),
          json::array({1, std::numeric_limits<double>::quiet_NaN(), 1})}) {
    const json input{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", json::array({key(invalid)})}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument) << invalid.dump();
  }
  for (const auto &invalidTrack : {json(nullptr), json::object(), json(1), json("scale")}) {
    const json input{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", invalidTrack}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
  for (const auto &invalidFrame : {json(true), json("1"), json(-1), json(0), json(1.5), json(10),
                                  json(std::numeric_limits<double>::infinity())}) {
    auto invalidKey = key({1, 1, 1});
    invalidKey["frame"] = invalidFrame;
    const json input{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", json::array({invalidKey})}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
  for (const auto &invalidKey : {json::object(), json::array({1, 1, 1}),
          json{{"frame", 1}}, json{{"scale", {1, 1, 1}}},
          json{{"frame", 1}, {"scale", {1, 1, 1}}, {"unknown", 1}}}) {
    const json input{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", json::array({invalidKey})}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
  for (int axis = 0; axis < 3; ++axis) {
    auto first = key({1, -2, 3});
    auto second = first;
    second["frame"] = 9;
    second["scale"][axis] = -second["scale"][axis].get<float>();
    const json input{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", json::array({first, second})}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument) << axis;
  }
  for (uint32_t frame : {1u, 2u}) {
    auto first = key({1, 1, 1});
    first["frame"] = 2;
    auto second = key({2, 2, 2});
    second["frame"] = frame;
    const json input{{"schemaVersion", 1}, {"frames", 9}, {"objectScale", json::array({first, second})}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
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

TEST(TemporalCapture, SelectsTelemetryFramesIndependentlyOfScreenshots) {
  CaptureSequence sequence(json::parse(
      R"({"schemaVersion":1,"frames":6,"sampleFrames":[6],"telemetryFrames":[5,2,6]})"));
  EXPECT_TRUE(sequence.hasTelemetryFrames());
  EXPECT_TRUE(sequence.capturesTelemetry(2));
  EXPECT_TRUE(sequence.capturesTelemetry(5));
  EXPECT_TRUE(sequence.capturesTelemetry(6));
  EXPECT_FALSE(sequence.capturesTelemetry(3));
  EXPECT_FALSE(sequence.captures(5));
  EXPECT_TRUE(sequence.captures(6));

  CaptureSequence timingOnly(json::parse(
      R"({"schemaVersion":1,"frames":6,"sampleFrames":[],"telemetryFrames":[6]})"));
  EXPECT_TRUE(timingOnly.capturesTelemetry(6));
  EXPECT_FALSE(timingOnly.captures(6));
}

TEST(TemporalCapture, RejectsDuplicateAndOutOfBoundsTelemetryFrames) {
  for (auto frames : {json::array({3, 3}), json::array({0}), json::array({7}),
                      json::array({1.5}), json::array({4294967297ULL}),
                      json::array({-4294967295LL})}) {
    const json input{{"schemaVersion", 1}, {"frames", 6}, {"telemetryFrames", frames}};
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
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

TEST(TemporalCapture, DdgiIntensityAppliesOnceIncludingZero) {
  const CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":6,
    "events":[{"frame":1,"ddgiIntensity":1},
              {"frame":3,"ddgiIntensity":0,"ddgiPaused":true},
              {"frame":5,"ddgiIntensity":0.125},
              {"frame":6,"ddgiIntensity":10000}]})"));
  EXPECT_EQ(sequence.sample(1).event.ddgiIntensity, 1.f);
  EXPECT_FALSE(sequence.sample(1).event.ddgiPaused.has_value());
  EXPECT_FALSE(sequence.sample(2).event.ddgiIntensity.has_value());
  EXPECT_EQ(sequence.sample(3).event.ddgiIntensity, 0.f);
  EXPECT_EQ(sequence.sample(3).event.ddgiPaused, true);
  EXPECT_FALSE(sequence.sample(3).event.ddgiEnabled.has_value());
  EXPECT_FALSE(sequence.sample(4).event.ddgiIntensity.has_value());
  EXPECT_EQ(sequence.sample(5).event.ddgiIntensity, .125f);
  EXPECT_EQ(sequence.sample(6).event.ddgiIntensity,
            container::renderer::kMaxDdgiIntensity);
}

TEST(TemporalCapture, RejectsInvalidDdgiIntensityBeforeNarrowing) {
  auto input = json::parse(
      R"({"schemaVersion":1,"frames":2,"events":[{"frame":1}]})");
  auto &intensity = input["events"][0]["ddgiIntensity"];
  for (const char *encoded : {"-0.00001", "10000.0001", "1e39", "\"1\"", "true",
                              "false", "null", "[]", "{}"}) {
    SCOPED_TRACE(encoded);
    intensity = json::parse(encoded);
    EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
  }
  intensity = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(CaptureSequence{input}, std::invalid_argument);
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

TEST(TemporalCapture, MaterialVariantAndHistoryEventsApplyOnlyAtTheirNamedFrame) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":5,"events":[
    {"frame":2,"materialVariant":0},{"frame":3,"sceneHistory":"undo"},
    {"frame":4,"sceneHistory":"redo"},{"frame":5,"materialVariant":-1}]})"));
  EXPECT_FALSE(sequence.sample(1).event.materialVariant);
  EXPECT_EQ(sequence.sample(2).event.materialVariant,0);
  EXPECT_EQ(sequence.sample(3).event.sceneHistory,"undo");
  EXPECT_EQ(sequence.sample(4).event.sceneHistory,"redo");
  EXPECT_EQ(sequence.sample(5).event.materialVariant,-1);
  EXPECT_TRUE(sequence.sample(1).event.sceneHistory.empty());
  for(const auto& event:std::vector<json>{
      {{"frame",1},{"materialVariant",-2}},{{"frame",1},{"materialVariant",1.5}},
      {{"frame",1},{"materialVariant",std::numeric_limits<uint64_t>::max()}},
      {{"frame",1},{"sceneHistory","reset"}},{{"frame",1},{"sceneHistory",42}},
      {{"frame",1},{"materialVariant",0},{"sceneHistory","undo"}}}) {
    const json invalid{{"schemaVersion",1},{"frames",2},{"events",json::array({event})}};
    EXPECT_THROW((void)CaptureSequence(invalid),std::invalid_argument);
  }
}

TEST(TemporalCapture, GltfSceneSwitchIsAValidatedExactEvent) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":4,
    "events":[{"frame":2,"gltfScene":0},{"frame":4,"gltfScene":2147483647,"expectGltfSceneRejection":true}]})"));
  EXPECT_FALSE(sequence.sample(1).event.gltfScene);
  EXPECT_EQ(sequence.sample(2).event.gltfScene,0);
  EXPECT_FALSE(sequence.sample(2).event.expectGltfSceneRejection);
  EXPECT_FALSE(sequence.sample(3).event.gltfScene);
  EXPECT_EQ(sequence.sample(4).event.gltfScene,2147483647);
  EXPECT_TRUE(sequence.sample(4).event.expectGltfSceneRejection);
  for(const auto& value:std::vector<json>{-1,1.5,true,nullptr,"0",uint64_t(2147483648)}) {
    const json invalid{{"schemaVersion",1},{"frames",2},
      {"events",json::array({{{"frame",1},{"gltfScene",value}}})}};
    EXPECT_THROW((void)CaptureSequence(invalid),std::invalid_argument);
  }
  for (const auto& event : std::vector<json>{
           {{"expectGltfSceneRejection", true}},
           {{"gltfScene", 0}, {"expectGltfSceneRejection", "true"}}}) {
    auto invalid = event;
    invalid["frame"] = 1;
    EXPECT_THROW((void)CaptureSequence({{"schemaVersion", 1}, {"frames", 1},
        {"events", json::array({invalid})}}), std::invalid_argument);
  }
}

TEST(TemporalCapture, NodeFlagsAreStrictGroupedExactEvents) {
  CaptureSequence sequence(json::parse(R"({"schemaVersion":1,"frames":5,"events":[
    {"frame":2,"nodeFlags":{"sourceNode":0,"visible":false,"selectable":true}},
    {"frame":4,"nodeFlags":{"sourceNode":2147483647,"hoverable":false}}]})"));
  EXPECT_FALSE(sequence.sample(1).event.nodeFlags);
  ASSERT_TRUE(sequence.sample(2).event.nodeFlags);
  const auto flags = *sequence.sample(2).event.nodeFlags;
  EXPECT_EQ(flags.sourceNode, 0);
  EXPECT_EQ(flags.visible, false);
  EXPECT_EQ(flags.selectable, true);
  EXPECT_FALSE(flags.hoverable.has_value());
  EXPECT_FALSE(sequence.sample(3).event.nodeFlags);
  EXPECT_EQ(sequence.sample(4).event.nodeFlags->hoverable, false);
  for (const auto& flags : std::vector<json>{nullptr, false, 0, "flags", json::object(),
       {{"sourceNode",0}}, {{"sourceNode",-1},{"visible",true}},
       {{"sourceNode",1.5},{"visible",true}}, {{"sourceNode",true},{"visible",true}},
       {{"sourceNode",uint64_t(2147483648)},{"visible",true}},
       {{"sourceNode",0},{"visible",0}}, {{"sourceNode",0},{"hoverable",nullptr}},
       {{"sourceNode",0},{"selectable","true"}}, {{"sourceNode",0},{"other",true}}}) {
    const json invalid{{"schemaVersion",1},{"frames",2},
        {"events",json::array({{{"frame",1},{"nodeFlags",flags}}})}};
    EXPECT_THROW((void)CaptureSequence(invalid), std::invalid_argument);
  }
}

TEST(TemporalCapture, NodeFlagEditsAndOtherHistoryActionsUseSeparateEvents) {
  for (const auto& action : std::vector<json>{{{"sceneHistory","undo"}},
       {{"materialVariant",0}}, {{"gltfScene",0}}}) {
    json event{{"frame",1},{"nodeFlags",{{"sourceNode",0},{"visible",false}}}};
    event.update(action);
    const json invalid{{"schemaVersion",1},{"frames",2},{"events",json::array({event})}};
    EXPECT_THROW((void)CaptureSequence(invalid), std::invalid_argument);
  }
}

namespace {
json mergeCapture(json request = json{{"path", "models/incoming.glb"}}) {
  return json{{"schemaVersion", 1}, {"frames", 4},
      {"events", json::array({{{"frame", 2}, {"merge", std::move(request)}}})}};
}
constexpr std::array<double, 16> mergeIdentity{
    1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
} // namespace

TEST(TemporalCapture, MergeRequestHasExplicitDefaultsAndAppliesOnlyAtItsNamedFrame) {
  const auto input = mergeCapture();
  const auto before = input;
  CaptureSequence sequence(input);
  EXPECT_EQ(input, before);
  EXPECT_FALSE(sequence.sample(1).event.merge);
  const auto event = sequence.sample(2).event;
  ASSERT_TRUE(event.merge);
  EXPECT_EQ(event.merge->path, "models/incoming.glb");
  EXPECT_EQ(event.merge->parentName, "Imported scene");
  EXPECT_FALSE(event.merge->incomingScene);
  EXPECT_FALSE(event.merge->placementMatrix);
  EXPECT_TRUE(event.guiWaitForLoad);
  EXPECT_FALSE(sequence.sample(3).event.merge);
  EXPECT_FALSE(sequence.sample(4).event.merge);
}

TEST(TemporalCapture, MergeRequestPreservesAuthoredDoubleColumnMajorPlacementAndNames) {
  auto matrix = mergeIdentity;
  matrix[0] = -2; matrix[1] = .2; matrix[5] = 3; matrix[10] = 4;
  matrix[12] = 5; matrix[13] = 6; matrix[14] = 7;
  const auto parsed = CaptureSequence(mergeCapture({{"path", "with spaces/B.gltf"},
      {"parentName", "Placed B"}, {"incomingScene", std::numeric_limits<int>::max()},
      {"placementMatrix", matrix}})).sample(2).event.merge;
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->path, "with spaces/B.gltf");
  EXPECT_EQ(parsed->parentName, "Placed B");
  EXPECT_EQ(parsed->incomingScene, std::numeric_limits<int>::max());
  ASSERT_TRUE(parsed->placementMatrix);
  EXPECT_EQ(*parsed->placementMatrix, matrix);
  EXPECT_DOUBLE_EQ((*parsed->placementMatrix)[1], .2);
  EXPECT_NE((*parsed->placementMatrix)[1], double(float(.2)));
  EXPECT_EQ((*parsed->placementMatrix)[12], 5);
}

TEST(TemporalCapture, MergeRequestUsesGuiSettlePolicyAndAllowsInertRoutesAndRenderSettings) {
  auto input = mergeCapture();
  input["events"][0].update(json{{"guiWaitForLoad", false}, {"reload", ""}, {"guiReload", ""},
      {"guiSampleModel", ""}, {"guiUnload", false}, {"physicsAttach", ""}, {"physicsAction", ""},
      {"physicsJointAction", ""}, {"taa", false}, {"exposure", .35}, {"samples", 4}, {"resize", {640, 360}}});
  CaptureSequence sequence(input);
  EXPECT_TRUE(sequence.sample(2).event.merge);
  EXPECT_FALSE(sequence.sample(2).event.guiWaitForLoad);
  EXPECT_FALSE(sequence.sample(3).event.merge);
  EXPECT_TRUE(sequence.sample(3).event.guiWaitForLoad);
  input["events"][0].erase("merge");
  EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument);
}

TEST(TemporalCapture, MergeRequestRejectsUnknownFieldsMissingPathsAndWrongStringTypes) {
  for (const auto& request : std::vector<json>{nullptr, false, 1, "incoming.glb", json::array(), json::object(),
      {{"path", ""}}, {{"path", nullptr}}, {{"path", true}}, {{"path", 12}},
      {{"path", json::array()}}, {{"path", json::object()}},
      {{"path", "B.glb"}, {"parentName", ""}}, {{"path", "B.glb"}, {"parentName", false}},
      {{"path", "B.glb"}, {"parentName", nullptr}}, {{"path", "B.glb"}, {"unknown", 0}},
      {{"path", "B.glb"}, {"destinationScene", 0}}, {{"path", "B.glb"}, {"importScale", 2}}}) {
    EXPECT_THROW((void)CaptureSequence(mergeCapture(request)), std::invalid_argument) << request.dump();
  }
}

TEST(TemporalCapture, MergeSceneSelectorRejectsTruncationSignedWrapAndNonIntegerTypes) {
  for (const auto& scene : std::vector<json>{-1, 1.0, 1.5, true, "0", nullptr, json::array(),
      json::object(), uint64_t(2147483648), std::numeric_limits<uint64_t>::max()}) {
    EXPECT_THROW((void)CaptureSequence(mergeCapture({{"path", "B.glb"}, {"incomingScene", scene}})),
                 std::invalid_argument) << scene.dump();
  }
  const auto parsed = CaptureSequence(mergeCapture({{"path", "B.glb"}, {"incomingScene", 0}})).sample(2).event.merge;
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->incomingScene);
  EXPECT_EQ(*parsed->incomingScene, 0);
}

TEST(TemporalCapture, MergePlacementRequiresExactlySixteenFiniteNumericBinary32Values) {
  for (const auto& matrix : std::vector<json>{nullptr, true, "identity", json::object(),
      json::array(), json::array({1, 0, 0, 1}), std::vector<double>(15, 0.), std::vector<double>(17, 0.)}) {
    EXPECT_THROW((void)CaptureSequence(mergeCapture({{"path", "B.glb"}, {"placementMatrix", matrix}})),
                 std::invalid_argument);
  }
  for (const auto& invalid : std::vector<json>{true, nullptr, "0", json::array(), json::object(),
      1e39, -1e39, std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    json matrix = mergeIdentity;
    matrix[12] = invalid;
    EXPECT_THROW((void)CaptureSequence(mergeCapture({{"path", "B.glb"}, {"placementMatrix", matrix}})),
                 std::invalid_argument);
  }
  auto maximumTranslation = mergeIdentity;
  maximumTranslation[12] = std::numeric_limits<float>::max();
  EXPECT_NO_THROW((void)CaptureSequence(mergeCapture({{"path", "B.glb"}, {"placementMatrix", maximumTranslation}})));
}

TEST(TemporalCapture, MergePlacementRejectsNonAffineSingularRoundedAndOverflowingBases) {
  for (const size_t component : {3u, 7u, 11u, 15u}) {
    auto matrix = mergeIdentity;
    matrix[component] = .5;
    EXPECT_THROW((void)CaptureSequence(mergeCapture({{"path", "B.glb"}, {"placementMatrix", matrix}})),
                 std::invalid_argument);
  }
  auto singular = mergeIdentity; singular[0] = 0;
  auto unstable = mergeIdentity; unstable[0] = unstable[5] = unstable[10] = 1e-5;
  auto roundedSingular = mergeIdentity;
  roundedSingular[0] = roundedSingular[1] = roundedSingular[4] = 1;
  roundedSingular[5] = 1+1e-8;
  auto overflowing = mergeIdentity; overflowing[0] = overflowing[5] = overflowing[10] = 1e20;
  for (const auto& matrix : {singular, unstable, roundedSingular, overflowing})
    EXPECT_THROW((void)CaptureSequence(mergeCapture({{"path", "B.glb"}, {"placementMatrix", matrix}})),
                 std::invalid_argument);
}

TEST(TemporalCapture, MergeAndOtherModelSceneHistoryOrPhysicsMutationsRequireSeparateEvents) {
  std::vector<json> conflicts{
      {{"reload", "A.gltf"}}, {{"guiReload", "A.gltf"}}, {{"guiSampleModel", "sample A"}}, {{"guiUnload", true}},
      {{"gltfScene", 0}}, {{"materialVariant", -1}}, {{"nodeFlags", {{"sourceNode", 0}, {"visible", false}}}},
      {{"sceneHistory", "undo"}}, {{"sceneHistory", "redo"}}, {{"objectVisible", false}}, {{"bimHiddenObject", 0}},
      {{"physicsAttach", "dynamic"}}, {{"physicsJointAction", "clear"}}, {{"physicsJointAction", "remove"}},
      {{"physicsJointMotorVelocity", 0}},
      {{"physicsJointCreate", jointCapture()["events"][0]["physicsJointCreate"]}}};
  for (const char* action : {"play", "pause", "step", "reset", "clear", "detach"})
    conflicts.push_back(json{{"physicsAction", action}});
  for (const auto& conflict : conflicts) {
    auto input = mergeCapture();
    input["events"][0].update(conflict);
    EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument) << conflict.dump();
  }
}

namespace {
json nodeAuthoringCapture(json request = json{{"action", "rename"}, {"graphNode", 0}, {"name", "Renamed node"}}) {
  return json{{"schemaVersion", 1}, {"frames", 4},
      {"events", json::array({{{"frame", 2}, {"nodeAuthoring", std::move(request)}}})}};
}
} // namespace

TEST(TemporalCapture, NodeAuthoringUsesTypedSelectorsAtExactlyItsNamedFrame) {
  auto input = nodeAuthoringCapture();
  const auto original = input;
  CaptureSequence sequence(input);
  EXPECT_EQ(input, original);
  EXPECT_FALSE(sequence.sample(1).event.nodeAuthoring);
  const auto request = sequence.sample(2).event.nodeAuthoring;
  ASSERT_TRUE(request);
  EXPECT_EQ(request->action, "rename"); EXPECT_EQ(request->graphNode, 0u);
  EXPECT_FALSE(request->sourceNode); EXPECT_EQ(request->name, "Renamed node");
  EXPECT_FALSE(sequence.sample(3).event.nodeAuthoring);
  EXPECT_FALSE(sequence.sample(4).event.nodeAuthoring);
  const auto created = CaptureSequence(nodeAuthoringCapture({{"action", "create-empty"},
      {"sourceNode", std::numeric_limits<int>::max()}, {"name", "  Empty \xE7\xA9\xBA  "}})).sample(2).event.nodeAuthoring;
  ASSERT_TRUE(created); EXPECT_EQ(created->action, "create-empty");
  EXPECT_EQ(created->sourceNode, std::numeric_limits<int>::max()); EXPECT_FALSE(created->graphNode);
  EXPECT_EQ(created->name, "  Empty \xE7\xA9\xBA  ");
  const auto maximumGraph = CaptureSequence(nodeAuthoringCapture({{"action", "rename"},
      {"graphNode", std::numeric_limits<uint32_t>::max()-1u}, {"name", "Last representable node"}}))
      .sample(2).event.nodeAuthoring;
  ASSERT_TRUE(maximumGraph); EXPECT_EQ(maximumGraph->graphNode, std::numeric_limits<uint32_t>::max()-1u);
}

TEST(TemporalCapture, NodeAuthoringRejectsUnknownActionsFieldsMissingNamesAndAmbiguousTargets) {
  for (const auto& request : std::vector<json>{nullptr, false, 0, "rename", json::array(), json::object(),
      {{"graphNode", 0}, {"name", "A"}}, {{"action", "rename"}, {"name", "A"}},
      {{"action", "rename"}, {"graphNode", 0}}, {{"action", "rename"}, {"graphNode", 0}, {"name", nullptr}},
      {{"action", false}, {"graphNode", 0}, {"name", "A"}},
      {{"action", "delete"}, {"graphNode", 0}, {"name", "A"}},
      {{"action", "duplicate"}, {"graphNode", 0}, {"name", "A"}},
      {{"action", "create-empty"}, {"graphNode", 0}, {"sourceNode", 0}, {"name", "A"}},
      {{"action", "rename"}, {"graphNode", 0}, {"name", "A"}, {"parent", 1}},
      {{"action", "rename"}, {"graphNode", 0}, {"name", "A"}, {"graphIdentity", 7}}}) {
    EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture(request)), std::invalid_argument);
  }
}

TEST(TemporalCapture, MakeEditableUsesNodeIdentityWithoutAnAuthoredName) {
  const auto input = nodeAuthoringCapture({{"action", "make-editable"}, {"sourceNode", 3}});
  const CaptureSequence sequence(input);
  ASSERT_TRUE(sequence.sample(2).event.nodeAuthoring);
  EXPECT_EQ(sequence.sample(2).event.nodeAuthoring->action, "make-editable");
  EXPECT_EQ(sequence.sample(2).event.nodeAuthoring->sourceNode, 3);
  EXPECT_FALSE(sequence.sample(3).event.nodeAuthoring);
  EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture(
      {{"action", "make-editable"}, {"graphNode", 1}, {"name", "Unused"}})), std::invalid_argument);
}

TEST(TemporalCapture, DeleteSubtreeUsesExistingTypedSelectorsWithoutANameAtOnlyItsEventFrame) {
  for (const char* selector : {"graphNode", "sourceNode"}) {
    auto input = nodeAuthoringCapture({{"action", "delete-subtree"}, {selector, 0}});
    const auto before = input;
    CaptureSequence sequence(input);
    EXPECT_EQ(input, before);
    EXPECT_FALSE(sequence.sample(1).event.nodeAuthoring);
    const auto request = sequence.sample(2).event.nodeAuthoring;
    ASSERT_TRUE(request);
    EXPECT_EQ(request->action, "delete-subtree"); EXPECT_TRUE(request->name.empty());
    EXPECT_EQ(request->graphNode.has_value(), selector == std::string("graphNode"));
    EXPECT_EQ(request->sourceNode.has_value(), selector == std::string("sourceNode"));
    EXPECT_FALSE(sequence.sample(3).event.nodeAuthoring);
    EXPECT_FALSE(sequence.sample(4).event.nodeAuthoring);
  }
  const auto graph = CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"},
      {"graphNode", std::numeric_limits<uint32_t>::max()-1u}})).sample(2).event.nodeAuthoring;
  ASSERT_TRUE(graph); EXPECT_EQ(graph->graphNode, std::numeric_limits<uint32_t>::max()-1u);
  const auto source = CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"},
      {"sourceNode", std::numeric_limits<int>::max()}})).sample(2).event.nodeAuthoring;
  ASSERT_TRUE(source); EXPECT_EQ(source->sourceNode, std::numeric_limits<int>::max());
}

TEST(TemporalCapture, DeleteSubtreeRejectsAnySuppliedNameUnknownFieldsAndAmbiguousSelectors) {
  for (const auto& name : std::vector<json>{"", "Unused", nullptr, false, 0, json::array(), json::object()})
    EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"},
        {"graphNode", 0}, {"name", name}})), std::invalid_argument);
  for (const auto* field : {"recursive", "children", "parent", "retired", "graphIdentity"})
    EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"},
        {"sourceNode", 0}, {field, true}})), std::invalid_argument);
  EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"}})), std::invalid_argument);
  EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"},
      {"graphNode", 0}, {"sourceNode", 0}})), std::invalid_argument);
  for (const char* selector : {"graphNode", "sourceNode"})
    for (const auto& index : std::vector<json>{-1, 0.0, true, "0", nullptr,
        std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint32_t>::max()})
      EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "delete-subtree"},
          {selector, index}})), std::invalid_argument);
}

TEST(TemporalCapture, NodeAuthoringRejectsFractionBooleanSignedWrapAndOverflowBeforeNarrowing) {
  for (const char* selector : {"graphNode", "sourceNode"}) {
    std::vector<json> invalid{-1, 0.0, 1.0, 1.5, true, false, "0", nullptr, json::array(), json::object(),
        std::numeric_limits<uint64_t>::max(), std::numeric_limits<double>::infinity()};
    invalid.push_back(selector == std::string("graphNode") ? json(std::numeric_limits<uint32_t>::max()) : json(uint64_t(2147483648)));
    for (const auto& index : invalid) {
      json request{{"action", "rename"}, {selector, index}, {"name", "A"}};
      EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture(request)), std::invalid_argument);
    }
    const auto zero = CaptureSequence(nodeAuthoringCapture({{"action", "rename"}, {selector, 0}, {"name", "Zero"}}))
        .sample(2).event.nodeAuthoring;
    ASSERT_TRUE(zero); EXPECT_TRUE(zero->graphNode.has_value() != zero->sourceNode.has_value());
  }
}

TEST(TemporalCapture, NodeAuthoringUsesSharedUtf8ByteBudgetAndNameValidation) {
  const std::string exact = std::string(4092, 'a')+"\xF0\x9F\x92\xA1";
  const auto valid = CaptureSequence(nodeAuthoringCapture({{"action", "rename"}, {"sourceNode", 0}, {"name", exact}}))
      .sample(2).event.nodeAuthoring;
  ASSERT_TRUE(valid); EXPECT_EQ(valid->name, exact);
  for (const auto& name : std::vector<std::string>{"", "  ", "\xC2\xA0", "Line\nbreak",
      std::string("embedded\0nul", 12), "\x80", "\xC0\xAF", "\xED\xA0\x80", exact+"a"})
    EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "create-empty"}, {"graphNode", 0}, {"name", name}})),
                 std::invalid_argument);
  for (const auto& name : std::vector<json>{false, 1, json::array(), json::object()})
    EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture({{"action", "rename"}, {"graphNode", 0}, {"name", name}})),
                 std::invalid_argument);
}

TEST(TemporalCapture, NodeAuthoringAndOtherModelSceneHistoryOrPhysicsMutationsRequireSeparateEvents) {
  std::vector<json> conflicts{
      {{"merge", {{"path", "B.glb"}}}}, {{"reload", "A.gltf"}}, {{"guiReload", "A.gltf"}},
      {{"guiSampleModel", "Sample A"}}, {{"guiUnload", true}}, {{"gltfScene", 0}}, {{"materialVariant", -1}},
      {{"nodeFlags", {{"sourceNode", 0}, {"visible", false}}}}, {{"sceneHistory", "undo"}}, {{"sceneHistory", "redo"}},
      {{"objectVisible", false}}, {{"bimHiddenObject", 0}}, {{"physicsAttach", "dynamic"}},
      {{"physicsAttach", "static"}, {"physicsNode", 0}, {"physicsSettings", {{"mass", 2}}}},
      {{"physicsJointAction", "clear"}}, {{"physicsJointAction", "remove"}}, {{"physicsJointMotorVelocity", 0}},
      {{"physicsJointCreate", jointCapture()["events"][0]["physicsJointCreate"]}}};
  for (const char* action : {"play", "pause", "step", "reset", "clear", "detach"})
    conflicts.push_back(json{{"physicsAction", action}});
  for (const char* action : {"rename", "create-empty", "delete-subtree", "duplicate-subtree"}) {
    for (const auto& conflict : conflicts) {
      json request{{"action", action}, {"graphNode", 0}};
      if (action != std::string("delete-subtree")) request["name"] = "A";
      auto input = nodeAuthoringCapture(std::move(request));
      input["events"][0].update(conflict);
      EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument) << conflict.dump();
    }
  }
}

TEST(TemporalCapture, NodeAuthoringAllowsRenderSettingsAndInertFieldsWithoutModelWorkerSemantics) {
  auto input = nodeAuthoringCapture();
  input["events"][0].update(json{{"reload", ""}, {"guiReload", ""}, {"guiSampleModel", ""}, {"guiUnload", false},
      {"physicsAttach", ""}, {"physicsAction", ""}, {"physicsJointAction", ""},
      {"taa", false}, {"exposure", .35}, {"samples", 4}, {"resize", {640, 360}}});
  const auto event = CaptureSequence(input).sample(2).event;
  ASSERT_TRUE(event.nodeAuthoring); EXPECT_EQ(event.nodeAuthoring->graphNode, 0u);
  EXPECT_EQ(event.samples, 4u); EXPECT_EQ(event.taa, false); EXPECT_TRUE(event.guiWaitForLoad);
  input["events"][0]["guiWaitForLoad"] = false;
  EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument);
}

TEST(TemporalCapture, NodeAuthoringFlagsHistoryAndMergeCanUseDistinctEvents) {
  const json input{{"schemaVersion", 1}, {"frames", 5}, {"events", json::array({
      {{"frame", 1}, {"nodeAuthoring", {{"action", "create-empty"}, {"graphNode", 0}, {"name", "Empty"}}}},
      {{"frame", 2}, {"nodeAuthoring", {{"action", "rename"}, {"sourceNode", 0}, {"name", "Renamed"}}}},
      {{"frame", 3}, {"nodeFlags", {{"sourceNode", 0}, {"visible", false}}}},
      {{"frame", 4}, {"sceneHistory", "undo"}}, {{"frame", 5}, {"merge", {{"path", "B.glb"}}}}})}};
  CaptureSequence sequence(input);
  EXPECT_EQ(sequence.sample(1).event.nodeAuthoring->action, "create-empty");
  EXPECT_EQ(sequence.sample(2).event.nodeAuthoring->action, "rename");
  EXPECT_FALSE(sequence.sample(3).event.nodeAuthoring);
  EXPECT_TRUE(sequence.sample(3).event.nodeFlags); EXPECT_EQ(sequence.sample(4).event.sceneHistory, "undo");
  EXPECT_TRUE(sequence.sample(5).event.merge);
}

TEST(TemporalCapture, ViewportPointerDispatchesOnlyAtTheRequestedFrame) {
  using Action = container::temporal::CaptureViewportPointer::Action;
  auto input = json::parse(R"({"schemaVersion":1,"frames":4,"events":[
    {"frame":2,"viewportPointer":{"action":"hover","pixel":[320,180]}},
    {"frame":4,"viewportPointer":{"action":"select","pixel":[1,0]},"depthProbe":[1,0]}]})");
  const CaptureSequence sequence(input);
  EXPECT_TRUE(sequence.hasViewportPointer());
  EXPECT_FALSE(sequence.sample(1).event.viewportPointer);
  const auto hover = sequence.sample(2).event.viewportPointer;
  ASSERT_TRUE(hover);
  EXPECT_EQ(hover->action, Action::Hover);
  EXPECT_EQ(hover->pixel, glm::uvec2(320, 180));
  EXPECT_FALSE(sequence.sample(3).event.viewportPointer);
  const auto click = sequence.sample(4).event;
  ASSERT_TRUE(click.viewportPointer);
  EXPECT_EQ(click.viewportPointer->action, Action::Select);
  ASSERT_TRUE(click.depthProbe);
  EXPECT_EQ(click.viewportPointer->pixel, *click.depthProbe);

  for (const auto& malformed : json::parse(R"([
    {"action":"drag","pixel":[0,0]}, {"action":"select","pixel":[-1,0]},
    {"action":"hover","pixel":[0,16385]}, {"action":"hover","pixel":[0.5,0]},
    {"action":"select"}, {"action":"hover","pixel":[0,0],"extra":true}])")) {
    input["events"][0]["viewportPointer"] = malformed;
    EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument) << malformed.dump();
  }
}

TEST(TemporalCapture, DeleteHistoryAndMergeRemainValidAsSeparateEvents) {
  const json input{{"schemaVersion", 1}, {"frames", 4}, {"events", json::array({
      {{"frame", 1}, {"nodeAuthoring", {{"action", "delete-subtree"}, {"sourceNode", 0}}}},
      {{"frame", 2}, {"sceneHistory", "undo"}}, {{"frame", 3}, {"sceneHistory", "redo"}},
      {{"frame", 4}, {"merge", {{"path", "B.glb"}}}}})}};
  const CaptureSequence sequence(input);
  ASSERT_TRUE(sequence.sample(1).event.nodeAuthoring);
  EXPECT_EQ(sequence.sample(1).event.nodeAuthoring->action, "delete-subtree");
  EXPECT_TRUE(sequence.sample(1).event.nodeAuthoring->name.empty());
  EXPECT_EQ(sequence.sample(2).event.sceneHistory, "undo");
  EXPECT_EQ(sequence.sample(3).event.sceneHistory, "redo");
  EXPECT_TRUE(sequence.sample(4).event.merge);
}

TEST(TemporalCapture, AnimationPlayingIsStrictBooleanAndAnExactFrameEvent) {
  const json input{{"schemaVersion", 1}, {"frames", 4}, {"events", json::array({
      {{"frame", 2}, {"animationPlaying", true}, {"exposure", .35}, {"taa", false}},
      {{"frame", 3}, {"animationPlaying", false}}})}};
  const CaptureSequence sequence(input);
  EXPECT_FALSE(sequence.sample(1).event.animationPlaying);
  EXPECT_EQ(sequence.sample(2).event.animationPlaying, std::optional<bool>{true});
  EXPECT_EQ(sequence.sample(2).event.exposure, .35f);
  EXPECT_EQ(sequence.sample(3).event.animationPlaying, std::optional<bool>{false});
  EXPECT_FALSE(sequence.sample(4).event.animationPlaying);
  for (const auto& value : std::vector<json>{nullptr, 0, 1, "true", json::array(), json::object()}) {
    auto invalid = input; invalid["events"][0]["animationPlaying"] = value;
    EXPECT_THROW((void)CaptureSequence(invalid), std::invalid_argument);
  }
}

TEST(TemporalCapture, AnimationPlayingConflictsWithSceneMutationsEvenWhenPausing) {
  std::vector<json> conflicts{
      {{"nodeAuthoring", {{"action", "delete-subtree"}, {"sourceNode", 0}}}},
      {{"nodeAuthoring", {{"action", "rename"}, {"sourceNode", 0}, {"name", "A"}}}},
      {{"merge", {{"path", "B.glb"}}}}, {{"reload", "A.gltf"}}, {{"guiReload", "A.gltf"}},
      {{"guiSampleModel", "Sample A"}}, {{"guiUnload", true}}, {{"gltfScene", 0}}, {{"materialVariant", -1}},
      {{"nodeFlags", {{"sourceNode", 0}, {"visible", false}}}}, {{"sceneHistory", "undo"}}, {{"sceneHistory", "redo"}},
      {{"objectVisible", false}}, {{"bimHiddenObject", 0}}, {{"physicsAttach", "dynamic"}},
      {{"physicsJointAction", "clear"}}, {{"physicsJointMotorVelocity", 0}},
      {{"physicsJointCreate", jointCapture()["events"][0]["physicsJointCreate"]}}};
  for (const char* action : {"play", "pause", "step", "reset", "clear", "detach"}) conflicts.push_back(json{{"physicsAction", action}});
  for (const bool playing : {true, false}) for (const auto& conflict : conflicts) {
    json event{{"frame", 1}, {"animationPlaying", playing}}; event.update(conflict);
    const json input{{"schemaVersion", 1}, {"frames", 1}, {"events", json::array({event})}};
    EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument) << conflict.dump();
  }
  const json distinct{{"schemaVersion", 1}, {"frames", 3}, {"events", json::array({
      {{"frame", 1}, {"nodeAuthoring", {{"action", "delete-subtree"}, {"sourceNode", 0}}}},
      {{"frame", 2}, {"animationPlaying", true}}, {{"frame", 3}, {"sceneHistory", "undo"}}})}};
  EXPECT_NO_THROW((void)CaptureSequence(distinct));
}

namespace {
json typedEditCapture(json event) {
  event["frame"] = 2;
  return json{{"schemaVersion", 1}, {"frames", 4}, {"events", json::array({std::move(event)})}};
}
json affineCaptureMatrix() {
  return json::array({-2., 0., 0., 0., .25, 3., 0., 0., 0., 0., 4., 0., 5., -6., 7., 1.});
}
json reparentCaptureEvent() {
  return json{{"nodeReparent", {{"node", {{"sourceNode", 0}}}, {"parent", {{"graphNode", 7}}}}}};
}
json transformCaptureEvent() {
  return json{{"nodeTransform", {{"node", {{"graphNode", 0}}}, {"matrix", affineCaptureMatrix()}}}};
}
json primitiveCaptureEvent() {
  return json{{"addPrimitive", {{"kind", "quad"}, {"name", "Procedural witness"}, {"parent", {{"sourceNode", 0}}}}}};
}
} // namespace

TEST(TemporalCapture, DuplicateSubtreeAllowsOmittedOrExplicitValidNameAtOnlyItsEventFrame) {
  for (const char* selector : {"sourceNode", "graphNode"}) {
    auto input = nodeAuthoringCapture({{"action", "duplicate-subtree"}, {selector, 0}});
    const auto original = input;
    const CaptureSequence sequence(input);
    ASSERT_TRUE(sequence.sample(2).event.nodeAuthoring);
    EXPECT_EQ(sequence.sample(2).event.nodeAuthoring->action, "duplicate-subtree");
    EXPECT_TRUE(sequence.sample(2).event.nodeAuthoring->name.empty());
    EXPECT_FALSE(sequence.sample(1).event.nodeAuthoring);
    EXPECT_FALSE(sequence.sample(3).event.nodeAuthoring);
    EXPECT_EQ(input, original);
    input["events"][0]["nodeAuthoring"]["name"] = "  Copy \xE7\xA9\xBA  ";
    EXPECT_EQ(CaptureSequence(input).sample(2).event.nodeAuthoring->name, "  Copy \xE7\xA9\xBA  ");
  }
}

TEST(TemporalCapture, DuplicateSubtreeRejectsInvalidSuppliedNamesAndUnknownOrAmbiguousSelectors) {
  for (const auto& name : std::vector<json>{nullptr, false, 0, "", " \t ", "line\nfeed", std::string(4097, 'A'),
      std::string("\xc0\x80", 2), json::array(), json::object()}) {
    const auto input = nodeAuthoringCapture({{"action", "duplicate-subtree"}, {"sourceNode", 0}, {"name", name}});
    EXPECT_THROW((void)CaptureSequence(input), std::invalid_argument);
  }
  for (const auto& request : std::vector<json>{{{"action", "duplicate-subtree"}},
      {{"action", "duplicate-subtree"}, {"sourceNode", 0}, {"graphNode", 1}},
      {{"action", "duplicate-subtree"}, {"sourceNode", 0}, {"parent", 1}}}) {
    EXPECT_THROW((void)CaptureSequence(nodeAuthoringCapture(request)), std::invalid_argument);
  }
}

TEST(TemporalCapture, ReparentCarriesIndependentTypedNodeAndParentReferencesForExactlyOneFrame) {
  auto event = reparentCaptureEvent();
  event["nodeReparent"]["node"]["sourceNode"] = std::numeric_limits<int>::max();
  event["nodeReparent"]["parent"]["graphNode"] = std::numeric_limits<uint32_t>::max()-1u;
  const auto input = typedEditCapture(event); const auto before = input;
  const CaptureSequence sequence(input);
  const auto request = sequence.sample(2).event.nodeReparent;
  ASSERT_TRUE(request);
  EXPECT_EQ(request->node.sourceNode, std::numeric_limits<int>::max());
  EXPECT_FALSE(request->node.graphNode);
  EXPECT_EQ(request->parent.graphNode, std::numeric_limits<uint32_t>::max()-1u);
  EXPECT_FALSE(request->parent.sourceNode);
  EXPECT_FALSE(sequence.sample(1).event.nodeReparent);
  EXPECT_FALSE(sequence.sample(3).event.nodeReparent);
  EXPECT_EQ(input, before);
}

TEST(TemporalCapture, NodeTransformRetainsColumnMajorShearMirroringAndTranslationAtItsEventOnly) {
  const auto input = typedEditCapture(transformCaptureEvent());
  const CaptureSequence sequence(input);
  const auto request = sequence.sample(2).event.nodeTransform;
  ASSERT_TRUE(request);
  EXPECT_EQ(request->node.graphNode, 0u);
  EXPECT_EQ(request->matrix[0], glm::vec4(-2, 0, 0, 0));
  EXPECT_EQ(request->matrix[1], glm::vec4(.25f, 3, 0, 0));
  EXPECT_EQ(request->matrix[2], glm::vec4(0, 0, 4, 0));
  EXPECT_EQ(request->matrix[3], glm::vec4(5, -6, 7, 1));
  EXPECT_FALSE(sequence.sample(1).event.nodeTransform);
  EXPECT_FALSE(sequence.sample(3).event.nodeTransform);
  auto singular = transformCaptureEvent(); singular["nodeTransform"]["matrix"][0] = 0;
  EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture(singular)));
}

TEST(TemporalCapture, AllNestedNodeReferencesRejectWrongTypesUnknownFieldsAmbiguityAndNarrowing) {
  const std::vector<std::pair<json, std::string>> routes{
      {reparentCaptureEvent(), "/nodeReparent/node"}, {reparentCaptureEvent(), "/nodeReparent/parent"},
      {transformCaptureEvent(), "/nodeTransform/node"}, {primitiveCaptureEvent(), "/addPrimitive/parent"}};
  std::vector<json> invalid{nullptr, false, 0, "node", json::array(), json::object(),
      {{"graphNode", 0}, {"sourceNode", 0}}, {{"graphNode", 0}, {"name", "A"}}, {{"node", 0}}};
  for (const char* selector : {"graphNode", "sourceNode"})
    for (const auto& value : std::vector<json>{nullptr, false, "0", 0.5, 0.0, -1, std::numeric_limits<uint64_t>::max()})
      invalid.push_back(json{{selector, value}});
  invalid.push_back(json{{"graphNode", std::numeric_limits<uint32_t>::max()}});
  invalid.push_back(json{{"sourceNode", uint64_t(std::numeric_limits<int>::max())+1}});
  for (const auto& [event, pointer] : routes) for (const auto& reference : invalid) {
    auto changed = event; changed[json::json_pointer(pointer)] = reference;
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(changed)), std::invalid_argument) << pointer;
  }
}

TEST(TemporalCapture, NewNodeCommandObjectsRequireTheirExactFieldsAndFixedKeepWorldReparentPolicy) {
  for (const auto& event : std::vector<json>{{{"nodeReparent", nullptr}}, {{"nodeTransform", false}}, {{"addPrimitive", 4}},
      {{"nodeReparent", {{"node", {{"graphNode", 0}}}}}},
      {{"nodeReparent", {{"parent", {{"graphNode", 0}}}}}},
      {{"nodeTransform", {{"node", {{"sourceNode", 0}}}}}},
      {{"nodeTransform", {{"matrix", affineCaptureMatrix()}}}}}) {
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument);
  }
  auto reparent = reparentCaptureEvent(); reparent["nodeReparent"]["keepWorld"] = false;
  EXPECT_THROW((void)CaptureSequence(typedEditCapture(reparent)), std::invalid_argument);
  auto transform = transformCaptureEvent(); transform["nodeTransform"]["translation"] = {1, 2, 3};
  EXPECT_THROW((void)CaptureSequence(typedEditCapture(transform)), std::invalid_argument);
  auto primitive = primitiveCaptureEvent(); primitive["addPrimitive"]["materialIndex"] = 0;
  EXPECT_THROW((void)CaptureSequence(typedEditCapture(primitive)), std::invalid_argument);
}

TEST(TemporalCapture, NodeTransformRejectsMalformedNonfiniteUnrepresentableAndProjectiveMatrices) {
  std::vector<json> invalid{nullptr, false, "identity", json::object(), json::array(),
      json::array({1, 0, 0}), json::array({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0})};
  for (const auto& value : std::vector<json>{nullptr, false, "1", json::object(), json::array(),
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
      double(std::numeric_limits<float>::max())*2}) {
    auto matrix = affineCaptureMatrix(); matrix[0] = value; invalid.push_back(std::move(matrix));
  }
  for (const auto index : {3, 7, 11, 15}) {
    auto matrix = affineCaptureMatrix(); matrix[index] = index == 15 ? .5 : 1e-300;
    invalid.push_back(std::move(matrix));
  }
  for (const auto& matrix : invalid) {
    auto event = transformCaptureEvent(); event["nodeTransform"]["matrix"] = matrix;
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument);
  }
}

TEST(TemporalCapture, AddPrimitiveAcceptsAllProductionKindsOptionalParentAndExactValidNames) {
  for (const char* kind : {"quad", "plane", "cube", "sphere", "triangular-prism", "pyramid", "torus"}) {
    auto event = primitiveCaptureEvent(); event["addPrimitive"]["kind"] = kind;
    event["addPrimitive"]["name"] = "  Runtime \xE7\xA9\xBA  ";
    const CaptureSequence sequence(typedEditCapture(event));
    const auto request = sequence.sample(2).event.addPrimitive;
    ASSERT_TRUE(request);
    EXPECT_EQ(request->kind, kind);
    EXPECT_EQ(request->name, "  Runtime \xE7\xA9\xBA  ");
    ASSERT_TRUE(request->parent);
    EXPECT_EQ(request->parent->sourceNode, 0);
    EXPECT_FALSE(sequence.sample(1).event.addPrimitive);
    EXPECT_FALSE(sequence.sample(3).event.addPrimitive);
    event["addPrimitive"].erase("parent");
    EXPECT_FALSE(CaptureSequence(typedEditCapture(event)).sample(2).event.addPrimitive->parent);
  }
}

TEST(TemporalCapture, AddPrimitiveRejectsMissingUnknownKindsAndInvalidNames) {
  for (const auto& request : std::vector<json>{json::object(), {{"kind", "quad"}}, {{"name", "A"}},
      {{"kind", false}, {"name", "A"}}, {{"kind", "Quad"}, {"name", "A"}}, {{"kind", "triangle"}, {"name", "A"}},
      {{"kind", "quad"}, {"name", nullptr}}, {{"kind", "quad"}, {"name", ""}}, {{"kind", "quad"}, {"name", "   "}},
      {{"kind", "quad"}, {"name", "line\nfeed"}}, {{"kind", "quad"}, {"name", std::string(4097, 'A')}}}) {
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"addPrimitive", request}})), std::invalid_argument);
  }
}

TEST(TemporalCapture, AnimationClipTimeAndPlayingMayShareOneExactFrameWithDoublePrecisionTime) {
  const double time = .123456789012345;
  const auto input = typedEditCapture({{"animationClip", 0}, {"animationTime", time}, {"animationPlaying", false}, {"taa", false}});
  const CaptureSequence sequence(input);
  EXPECT_EQ(sequence.sample(2).event.animationClip, 0);
  EXPECT_EQ(sequence.sample(2).event.animationTime, time);
  EXPECT_EQ(sequence.sample(2).event.animationPlaying, false);
  EXPECT_FALSE(sequence.sample(1).event.animationClip);
  EXPECT_FALSE(sequence.sample(1).event.animationTime);
  EXPECT_FALSE(sequence.sample(3).event.animationPlaying);
  EXPECT_FALSE(sequence.sample(3).event.animationTime);
  EXPECT_EQ(CaptureSequence(typedEditCapture({{"animationClip", -1}, {"animationTime", 0}})).sample(2).event.animationClip, -1);
  EXPECT_EQ(CaptureSequence(typedEditCapture({{"animationClip", std::numeric_limits<int>::max()}})).sample(2).event.animationClip,
      std::numeric_limits<int>::max());
}

TEST(TemporalCapture, AnimationClipAndTimeRejectTypeRangeAndNonfiniteValuesWithoutNarrowing) {
  for (const auto& value : std::vector<json>{nullptr, false, "0", 0.0, .5, -2, std::numeric_limits<uint64_t>::max(),
      uint64_t(std::numeric_limits<int>::max())+1}) {
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"animationClip", value}})), std::invalid_argument);
  }
  for (const auto& value : std::vector<json>{nullptr, false, "0", -.1, json::array(), json::object(),
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"animationTime", value}})), std::invalid_argument);
  }
  EXPECT_EQ(CaptureSequence(typedEditCapture({{"animationTime", std::numeric_limits<double>::max()}})).sample(2).event.animationTime,
      std::numeric_limits<double>::max());
}

TEST(TemporalCapture, EachNewNodeEditAndGroupedAnimationControlsConflictWithOtherMutationGroups) {
  const std::vector<json> commands{reparentCaptureEvent(), transformCaptureEvent(), primitiveCaptureEvent(),
      {{"animationClip", 0}, {"animationTime", 0}, {"animationPlaying", false}}};
  std::vector<json> conflicts{{{"nodeAuthoring", {{"action", "duplicate-subtree"}, {"sourceNode", 0}}}},
      {{"merge", {{"path", "B.glb"}}}}, {{"reload", "A.glb"}}, {{"guiReload", "A.glb"}}, {{"guiSampleModel", "A"}},
      {{"guiUnload", true}}, {{"gltfScene", 0}}, {{"materialVariant", -1}}, {{"nodeFlags", {{"sourceNode", 0}, {"visible", false}}}},
      {{"sceneHistory", "undo"}}, {{"objectVisible", false}}, {{"bimHiddenObject", 0}}, {{"physicsAttach", "dynamic"}},
      {{"physicsJointAction", "clear"}}, {{"physicsJointMotorVelocity", 0}},
      {{"physicsJointCreate", jointCapture()["events"][0]["physicsJointCreate"]}}};
  for (const char* action : {"play", "pause", "step", "reset", "clear", "detach"}) conflicts.push_back({{"physicsAction", action}});
  for (size_t index = 0; index < commands.size(); ++index) {
    for (const auto& conflict : conflicts) {
      auto event = commands[index]; event.update(conflict);
      EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument) << event.dump();
    }
    for (size_t other = index+1; other < commands.size(); ++other) {
      auto event = commands[index]; event.update(commands[other]);
      EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument);
    }
    auto event = commands[index]; event.update(json{{"taa", false}, {"exposure", .35}, {"samples", 4}, {"resize", {640, 360}},
        {"reload", ""}, {"guiReload", ""}, {"guiSampleModel", ""}, {"guiUnload", false}, {"physicsAction", ""}});
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture(event)));
  }
  const std::vector<json> controls{json{{"animationClip", -1}}, json{{"animationTime", 0}}, json{{"animationPlaying", false}}};
  for (const auto& control : controls) {
    auto event = reparentCaptureEvent(); event.update(control);
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument);
  }
}

namespace {
using MaterialProperty = container::geometry::gltf::GltfMaterialProperty;
using MaterialPhase = container::material::MaterialEditGesturePhase;
json materialPatch(std::string property = "pbrMetallicRoughness/roughnessFactor", json value = .25) {
  return json{{"sourceMaterial", 0}, {"values", json::array({{{"property", std::move(property)}, {"value", std::move(value)}}})}};
}
json materialCapture(json patch) { return typedEditCapture({{"materialEdit", std::move(patch)}}); }
} // namespace

TEST(TemporalCapture, MaterialEditPreservesTypedZeroBlackResetFalseAndUtf8ForOnlyItsFrame) {
  auto patch = materialPatch("pbrMetallicRoughness/metallicFactor", 0.);
  patch["values"].push_back({{"property", "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor"}, {"value", {0., 0., 0.}}});
  patch["values"].push_back({{"property", "pbrMetallicRoughness/baseColorFactor"}, {"value", {1., 0., .5, 0.}}});
  patch["values"].push_back({{"property", "extensions/KHR_materials_volume/attenuationDistance"}, {"value", nullptr}});
  patch["extensions"] = json::array({{{"extension", "KHR_materials_clearcoat"}, {"enabled", true}},
      {{"extension", "KHR_materials_anisotropy"}, {"enabled", false}}});
  patch["alphaMode"] = "OPAQUE";
  patch["doubleSided"] = false;
  patch["name"] = "  Paint \xE7\xA9\xBA  ";
  const auto input = materialCapture(patch), before = input;
  const CaptureSequence sequence(input);
  const auto request = sequence.sample(2).event.materialEdit;
  ASSERT_TRUE(request);
  EXPECT_EQ(request->sourceMaterial, 0);
  EXPECT_EQ(request->phase, MaterialPhase::Commit);
  EXPECT_FALSE(request->gestureId);
  ASSERT_EQ(request->patch.values.size(), 4u);
  EXPECT_EQ(request->patch.values[0].property, MaterialProperty::Metallic);
  EXPECT_EQ(std::get<double>(request->patch.values[0].value), 0.);
  EXPECT_EQ((std::get<std::array<double, 3>>(request->patch.values[1].value)), (std::array<double, 3>{0, 0, 0}));
  EXPECT_EQ((std::get<std::array<double, 4>>(request->patch.values[2].value)), (std::array<double, 4>{1, 0, .5, 0}));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(request->patch.values[3].value));
  EXPECT_EQ(request->patch.alphaMode, container::geometry::gltf::GltfMaterialAlphaMode::Opaque);
  ASSERT_TRUE(request->patch.doubleSided.has_value());
  EXPECT_FALSE(*request->patch.doubleSided);
  EXPECT_EQ(request->patch.name, "  Paint \xE7\xA9\xBA  ");
  ASSERT_EQ(request->patch.extensions.size(), 2u);
  EXPECT_TRUE(request->patch.extensions[0].enabled);
  EXPECT_FALSE(request->patch.extensions[1].enabled);
  EXPECT_FALSE(sequence.sample(1).event.materialEdit);
  EXPECT_FALSE(sequence.sample(3).event.materialEdit);
  EXPECT_EQ(input, before);
}

TEST(TemporalCapture, MaterialRegistryAcceptsEveryCanonicalFactorAndResetWithoutInventingAliases) {
  using namespace container::geometry::gltf;
  auto patch = json{{"sourceMaterial", std::numeric_limits<int32_t>::max()}, {"values", json::array()}};
  for (const auto& entry : GltfMaterialPropertyRegistry()) {
    json value = .5;
    if (entry.components == 3) value = json::array({0., .25, 1.});
    if (entry.components == 4) value = json::array({0., .25, 1., .5});
    if (entry.property == MaterialProperty::Ior) value = 0.;
    if (entry.property == MaterialProperty::IridescenceIor) value = 1.3;
    patch["values"].push_back({{"property", std::string(entry.path)}, {"value", value}});
  }
  const auto request = CaptureSequence(materialCapture(patch)).sample(2).event.materialEdit;
  ASSERT_TRUE(request);
  ASSERT_EQ(request->patch.values.size(), size_t(MaterialProperty::Count));
  EXPECT_EQ(request->sourceMaterial, std::numeric_limits<int32_t>::max());
  for (size_t i = 0; i < request->patch.values.size(); ++i) EXPECT_EQ(request->patch.values[i].property, MaterialProperty(i));
  for (auto& value : patch["values"]) value["value"] = nullptr;
  const auto resets = CaptureSequence(materialCapture(patch)).sample(2).event.materialEdit;
  for (const auto& value : resets->patch.values) EXPECT_TRUE(std::holds_alternative<std::monostate>(value.value));
  EXPECT_THROW((void)CaptureSequence(materialCapture(materialPatch("roughness", .2))), std::invalid_argument);
  EXPECT_THROW((void)CaptureSequence(materialCapture(materialPatch("/materials/0/pbrMetallicRoughness/roughnessFactor", .2))), std::invalid_argument);
}

TEST(TemporalCapture, MaterialGesturePhasesRetainExactPortableIdsAndOptionalFinalPatch) {
  constexpr uint64_t maximum = (uint64_t(1) << 53) - 1;
  for (const auto& [name, phase] : std::array{
      std::pair{"begin", MaterialPhase::Begin}, std::pair{"update", MaterialPhase::Update}, std::pair{"end", MaterialPhase::End}}) {
    auto patch = materialPatch(); patch["gestureId"] = maximum; patch["gesturePhase"] = name;
    const auto request = CaptureSequence(materialCapture(patch)).sample(2).event.materialEdit;
    ASSERT_TRUE(request); EXPECT_EQ(request->gestureId, maximum); EXPECT_EQ(request->phase, phase);
  }
  for (const auto* phase : {"end", "cancel"}) {
    const auto request = CaptureSequence(materialCapture({{"sourceMaterial", 0}, {"gestureId", 1}, {"gesturePhase", phase}})).sample(2).event.materialEdit;
    EXPECT_EQ(request->gestureId, 1u);
    EXPECT_TRUE(request->patch.values.empty());
  }
  auto commit = materialPatch(); commit["gesturePhase"] = "commit";
  EXPECT_EQ(CaptureSequence(materialCapture(commit)).sample(2).event.materialEdit->phase, MaterialPhase::Commit);
}

TEST(TemporalCapture, MaterialIdentityAndGestureIdsRejectWrongTypesFractionsBooleansAndOverflow) {
  for (const auto& value : std::vector<json>{nullptr, false, "0", -1, .5, 0.0, uint64_t(INT32_MAX)+1,
      std::numeric_limits<uint64_t>::max(), json::array(), json::object()}) {
    auto patch = materialPatch(); patch["sourceMaterial"] = value;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
  for (const auto& value : std::vector<json>{nullptr, false, "1", -1, 0, 1.5, 1.0, uint64_t(1)<<53, std::numeric_limits<uint64_t>::max()}) {
    auto patch = materialPatch(); patch["gesturePhase"] = "begin"; patch["gestureId"] = value;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
  auto missing = materialPatch(); missing.erase("sourceMaterial");
  EXPECT_THROW((void)CaptureSequence(materialCapture(missing)), std::invalid_argument);
}

TEST(TemporalCapture, MaterialGestureAndPatchPresenceRulesRejectAmbiguousRequests) {
  for (const auto* phase : {"begin", "update", "end", "cancel"}) {
    auto patch = materialPatch(); patch["gesturePhase"] = phase;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
  for (const auto* phase : {"begin", "update"}) {
    EXPECT_THROW((void)CaptureSequence(materialCapture({{"sourceMaterial", 0}, {"gestureId", 1}, {"gesturePhase", phase}})), std::invalid_argument);
  }
  auto patch = materialPatch(); patch["gestureId"] = 1;
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch["gesturePhase"] = "commit";
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch["gesturePhase"] = "cancel";
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch["values"] = json::array();
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  for (const auto& phase : std::vector<json>{nullptr, false, 0, "drag", std::string(1000, 'A')}) {
    auto request = materialPatch(); request["gesturePhase"] = phase;
    EXPECT_THROW((void)CaptureSequence(materialCapture(request)), std::invalid_argument);
  }
  EXPECT_THROW((void)CaptureSequence(materialCapture({{"sourceMaterial", 0}})), std::invalid_argument);
  EXPECT_THROW((void)CaptureSequence(materialCapture({{"sourceMaterial", 0}, {"values", json::array()}})), std::invalid_argument);
  EXPECT_NO_THROW((void)CaptureSequence(materialCapture({{"sourceMaterial", 0}, {"doubleSided", false}})));
}

TEST(TemporalCapture, MaterialPatchRejectsUnknownFieldsDuplicateOperationsAndWrongTypedShapes) {
  for (const auto& value : std::vector<json>{nullptr, false, 0, "material", json::array()})
    EXPECT_THROW((void)CaptureSequence(materialCapture(value)), std::invalid_argument);
  auto patch = materialPatch(); patch["texture"] = 0;
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch(); patch["values"].push_back(patch["values"][0]);
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch(); patch["values"][0]["runtimeMaterial"] = 0;
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  for (const auto& input : std::vector<json>{nullptr, false, "value", json::object(), json::array({1, 2})}) {
    patch = materialPatch(); patch["values"] = input;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
  for (const auto& input : std::vector<json>{false, "0", json::array({0}), json::object()})
    EXPECT_THROW((void)CaptureSequence(materialCapture(materialPatch("pbrMetallicRoughness/metallicFactor", input))), std::invalid_argument);
  for (const auto& input : std::vector<json>{0., json::array({0, 0}), json::array({0, 0, 0, 0}), json::array({0, false, 0})})
    EXPECT_THROW((void)CaptureSequence(materialCapture(materialPatch("emissiveFactor", input))), std::invalid_argument);
}

TEST(TemporalCapture, MaterialExtensionBooleansUseOnlyKnownNamesAndRejectDuplicatesAndConflicts) {
  using namespace container::geometry::gltf;
  json extensions = json::array();
  for (size_t i = 0; i < size_t(GltfMaterialExtension::Count); ++i)
    extensions.push_back({{"extension", std::string(GltfMaterialExtensionName(GltfMaterialExtension(i)))}, {"enabled", false}});
  const auto request = CaptureSequence(materialCapture({{"sourceMaterial", 0}, {"extensions", extensions}})).sample(2).event.materialEdit;
  ASSERT_EQ(request->patch.extensions.size(), size_t(GltfMaterialExtension::Count));
  for (const auto& extension : request->patch.extensions) EXPECT_FALSE(extension.enabled);
  for (const auto& extension : std::vector<json>{{{"extension", "VENDOR_material"}, {"enabled", true}},
      {{"extension", "clearcoat"}, {"enabled", true}}, {{"extension", "KHR_materials_clearcoat"}, {"enabled", 1}},
      {{"extension", "KHR_materials_clearcoat"}}, {{"extension", "KHR_materials_clearcoat"}, {"enabled", false}, {"unknown", 0}}}) {
    EXPECT_THROW((void)CaptureSequence(materialCapture({{"sourceMaterial", 0}, {"extensions", json::array({extension})}})), std::invalid_argument);
  }
  auto patch = materialPatch(); patch["extensions"] = json::array({extensions[0], extensions[0]});
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch("extensions/KHR_materials_clearcoat/clearcoatFactor", nullptr);
  patch["extensions"] = json::array({{{"extension", "KHR_materials_clearcoat"}, {"enabled", false}}});
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
}

TEST(TemporalCapture, MaterialNumericRangesPreserveSignedZeroAndAllowNormativeNonunitFactors) {
  const std::vector<std::pair<std::string, double>> accepted{
      {"normalTexture/scale", -2}, {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale", -3},
      {"extensions/KHR_materials_anisotropy/anisotropyRotation", -1000}, {"alphaCutoff", 2},
      {"extensions/KHR_materials_dispersion/dispersion", 15}, {"extensions/KHR_materials_emissive_strength/emissiveStrength", 100},
      {"extensions/KHR_materials_ior/ior", 0}, {"extensions/KHR_materials_iridescence/iridescenceIor", 1}};
  for (const auto& [property, value] : accepted) EXPECT_NO_THROW((void)CaptureSequence(materialCapture(materialPatch(property, value))));
  const auto zero = CaptureSequence(materialCapture(materialPatch("pbrMetallicRoughness/roughnessFactor", -0.))).sample(2).event.materialEdit;
  EXPECT_TRUE(std::signbit(std::get<double>(zero->patch.values[0].value)));
  const std::vector<std::pair<std::string, double>> rejected{
      {"pbrMetallicRoughness/metallicFactor", -.01}, {"pbrMetallicRoughness/roughnessFactor", 1.01},
      {"alphaCutoff", -.01}, {"extensions/KHR_materials_ior/ior", .5},
      {"extensions/KHR_materials_iridescence/iridescenceIor", .99},
      {"extensions/KHR_materials_volume/attenuationDistance", 0}, {"extensions/KHR_materials_volume/attenuationDistance", 1e-100},
      {"normalTexture/scale", 1e39}, {"pbrMetallicRoughness/roughnessFactor", std::numeric_limits<double>::quiet_NaN()},
      {"pbrMetallicRoughness/roughnessFactor", std::numeric_limits<double>::infinity()}};
  for (const auto& [property, value] : rejected) EXPECT_THROW((void)CaptureSequence(materialCapture(materialPatch(property, value))), std::invalid_argument);
}

TEST(TemporalCapture, MaterialMetadataRejectsInvalidLabelsModesAndNonbooleanSidedness) {
  for (const auto& name : std::vector<json>{nullptr, false, 0, "", " \t ", "line\nfeed", std::string(4097, 'A'), std::string("\xc0\x80", 2)}) {
    auto patch = materialPatch(); patch["name"] = name;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
  for (const auto& mode : std::vector<json>{nullptr, false, 0, "opaque", "DITHER"}) {
    auto patch = materialPatch(); patch["alphaMode"] = mode;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
  for (const auto& sided : std::vector<json>{nullptr, 0, 1, "false"}) {
    auto patch = materialPatch(); patch["doubleSided"] = sided;
    EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  }
}

TEST(TemporalCapture, MaterialEditOperationAndStringBudgetsRejectBeforeOwnedRequestGrowth) {
  auto patch = materialPatch();
  for (int i = 0; i < 40; ++i) patch["values"].push_back(patch["values"][0]);
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch(); patch["extensions"] = json::array();
  for (int i = 0; i < 14; ++i) patch["extensions"].push_back({{"extension", "KHR_materials_unlit"}, {"enabled", false}});
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch(std::string(257, 'A'), .2);
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch(); patch[std::string(10000, 'A')] = false;
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
  patch = materialPatch(); patch["extensions"] = json::array({{{"extension", std::string(65, 'A')}, {"enabled", false}}});
  EXPECT_THROW((void)CaptureSequence(materialCapture(patch)), std::invalid_argument);
}

TEST(TemporalCapture, MaterialEditConflictsWithOtherMutationGroupsButAllowsIndependentRenderSettings) {
  std::vector<json> conflicts{reparentCaptureEvent(), transformCaptureEvent(), primitiveCaptureEvent(),
      {{"nodeAuthoring", {{"action", "rename"}, {"sourceNode", 0}, {"name", "New"}}}},
      {{"animationPlaying", false}}, {{"animationClip", 0}}, {{"animationTime", 0}},
      {{"merge", {{"path", "B.glb"}}}}, {{"reload", "A.glb"}}, {{"guiReload", "A.glb"}}, {{"guiSampleModel", "A"}},
      {{"guiUnload", true}}, {{"gltfScene", 0}}, {{"materialVariant", -1}}, {{"nodeFlags", {{"sourceNode", 0}, {"visible", false}}}},
      {{"sceneHistory", "undo"}}, {{"objectVisible", false}}, {{"bimHiddenObject", 0}}, {{"physicsNode", 0}},
      {{"physicsAttach", "dynamic"}}, {{"physicsAction", "pause"}}, {{"physicsJointAction", "clear"}}, {{"physicsJointMotorVelocity", 0}},
      {{"physicsJointCreate", jointCapture()["events"][0]["physicsJointCreate"]}}};
  for (const auto& conflict : conflicts) {
    auto event = conflict; event["materialEdit"] = materialPatch();
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument) << event.dump();
  }
  auto event = json{{"materialEdit", materialPatch()}, {"taa", false}, {"exposure", .35}, {"samples", 4},
      {"resize", {640, 360}}, {"reload", ""}, {"guiReload", ""}, {"guiSampleModel", ""}, {"guiUnload", false}, {"physicsAction", ""}};
  EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture(event)));
  event["guiWaitForLoad"] = false;
  EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)), std::invalid_argument);
  const json separate{{"schemaVersion", 1}, {"frames", 3}, {"events", json::array({
      {{"frame", 1}, {"animationClip", 0}, {"animationTime", .5}, {"animationPlaying", true}},
      {{"frame", 2}, {"materialEdit", materialPatch()}}, {{"frame", 3}, {"sceneHistory", "undo"}}})}};
  EXPECT_NO_THROW((void)CaptureSequence(separate));
}

namespace {
json materialBindingPatch(const char* operation="assign") {
  return {{"operation",operation},{"graphNode",7},{"expectedRuntimeMaterial",11},{"sourceMaterial",2},{"runtimeMaterial",17}};
}
json materialBindingCapture(json request) {return typedEditCapture({{"materialBinding",std::move(request)}});}
} // namespace

TEST(TemporalCapture, MaterialBindingPreservesExplicitSparseIdentitiesAtOnlyItsNamedFrame) {
  using Operation=container::material::MaterialBindingEditOperation;
  const auto input=materialBindingCapture(materialBindingPatch()),before=input;
  const CaptureSequence sequence(input);
  EXPECT_FALSE(sequence.sample(1).event.materialBinding);
  const auto request=sequence.sample(2).event.materialBinding;ASSERT_TRUE(request);
  EXPECT_EQ(request->operation,Operation::Assign);EXPECT_EQ(request->graphNode,7u);
  EXPECT_EQ(request->expectedRuntimeMaterial,11u);EXPECT_EQ(request->sourceMaterial,2);
  EXPECT_EQ(request->runtimeMaterial,17u);EXPECT_FALSE(request->name);
  EXPECT_FALSE(sequence.sample(2).event.materialEdit);EXPECT_FALSE(sequence.sample(3).event.materialBinding);
  EXPECT_EQ(input,before);
}

TEST(TemporalCapture, MaterialBindingAcceptsZeroAndRuntimeOnlyPairsWithoutGuessingSourceIdentity) {
  auto patch=materialBindingPatch();patch["graphNode"]=0;patch["expectedRuntimeMaterial"]=0;
  patch["sourceMaterial"]=-1;patch["runtimeMaterial"]=23;
  const auto runtime=CaptureSequence(materialBindingCapture(patch)).sample(2).event.materialBinding;
  ASSERT_TRUE(runtime);EXPECT_EQ(runtime->graphNode,0u);EXPECT_EQ(runtime->expectedRuntimeMaterial,0u);
  EXPECT_EQ(runtime->sourceMaterial,-1);EXPECT_EQ(runtime->runtimeMaterial,23u);
  patch["sourceMaterial"]=0;patch["runtimeMaterial"]=0;
  const auto zero=CaptureSequence(materialBindingCapture(patch)).sample(2).event.materialBinding;
  ASSERT_TRUE(zero);EXPECT_EQ(zero->sourceMaterial,0);EXPECT_EQ(zero->runtimeMaterial,0u);
}

TEST(TemporalCapture, MaterialBindingIntegerBoundsRemainExactWithoutUintSentinelOrIntOverflow) {
  for(const bool unsignedInput:{false,true}) {
    auto patch=materialBindingPatch();
    const json maximum=unsignedInput?json(uint64_t(UINT32_MAX)-1):json(int64_t(UINT32_MAX)-1);
    for(const auto* field:{"graphNode","expectedRuntimeMaterial","runtimeMaterial"}) patch[field]=maximum;
    patch["sourceMaterial"]=unsignedInput?json(uint64_t(INT32_MAX)):json(int64_t(INT32_MAX));
    const auto request=CaptureSequence(materialBindingCapture(patch)).sample(2).event.materialBinding;
    ASSERT_TRUE(request);EXPECT_EQ(request->graphNode,UINT32_MAX-1);EXPECT_EQ(request->expectedRuntimeMaterial,UINT32_MAX-1);
    EXPECT_EQ(request->runtimeMaterial,UINT32_MAX-1);EXPECT_EQ(request->sourceMaterial,INT32_MAX);
  }
  for(const auto* field:{"graphNode","expectedRuntimeMaterial","runtimeMaterial","sourceMaterial"}) {
    for(const auto& invalid:std::vector<json>{nullptr,false,true,"0",.5,0.0,1.0,json::array(),json::object(),
        std::numeric_limits<uint64_t>::max(),std::numeric_limits<int64_t>::min()}) {
      auto patch=materialBindingPatch();patch[field]=invalid;
      EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument)<<field<<" "<<invalid.dump();
    }
  }
  for(const auto* field:{"graphNode","expectedRuntimeMaterial","runtimeMaterial"})
    for(const auto& invalid:std::vector<json>{-1,int64_t(UINT32_MAX),uint64_t(UINT32_MAX),uint64_t(UINT32_MAX)+1}) {
      auto patch=materialBindingPatch();patch[field]=invalid;
      EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument)<<field;
    }
  for(const auto& invalid:std::vector<json>{-2,int64_t(INT32_MAX)+1,uint64_t(INT32_MAX)+1}) {
    auto patch=materialBindingPatch();patch["sourceMaterial"]=invalid;
    EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
  }
}

TEST(TemporalCapture, MaterialBindingRequiresEveryExplicitFieldAndRejectsUnknownShapesAndOperations) {
  for(const auto* field:{"operation","graphNode","expectedRuntimeMaterial","sourceMaterial","runtimeMaterial"}) {
    auto patch=materialBindingPatch();patch.erase(field);
    EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument)<<field;
  }
  for(const auto& invalid:std::vector<json>{nullptr,false,0,"binding",json::array(),json::object()})
    EXPECT_THROW((void)CaptureSequence(materialBindingCapture(invalid)),std::invalid_argument);
  for(const auto& operation:std::vector<json>{nullptr,false,1,"","Assign","make-unique","duplicate",std::string(10000,'A')}) {
    auto patch=materialBindingPatch();patch["operation"]=operation;
    EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
  }
  for(const auto* extra:{"selectedGraphNode","texture","gestureId","gesturePhase","sourceNode"}) {
    auto patch=materialBindingPatch();patch[extra]=0;
    EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
  }
  auto patch=materialBindingPatch();patch[std::string(10000,'A')]=0;
  EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
  patch=materialBindingPatch();patch["name"]="Renaming is not assignment";
  EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
}

TEST(TemporalCapture, MaterialMakeUniquePreservesOptionalUtf8NameAndCurrentPairWithoutCreatingAnIdentity) {
  using Operation=container::material::MaterialBindingEditOperation;
  auto patch=materialBindingPatch("makeUnique");patch["runtimeMaterial"]=11;
  auto request=CaptureSequence(materialBindingCapture(patch)).sample(2).event.materialBinding;
  ASSERT_TRUE(request);EXPECT_EQ(request->operation,Operation::MakeUnique);EXPECT_EQ(request->sourceMaterial,2);
  EXPECT_EQ(request->runtimeMaterial,request->expectedRuntimeMaterial);EXPECT_FALSE(request->name);
  patch["name"]="  Copper \xE7\xA9\xBA  ";
  request=CaptureSequence(materialBindingCapture(patch)).sample(2).event.materialBinding;
  ASSERT_TRUE(request);EXPECT_EQ(request->name,"  Copper \xE7\xA9\xBA  ");
  patch["sourceMaterial"]=-1;
  EXPECT_EQ(CaptureSequence(materialBindingCapture(patch)).sample(2).event.materialBinding->sourceMaterial,-1);
}

TEST(TemporalCapture, MaterialMakeUniqueRejectsBadNamesAndBoundsBytesBeforeOwningTheName) {
  for(const auto& name:std::vector<json>{nullptr,false,0,""," \t ","line\nfeed",std::string(4097,'A'),
      std::string("\xc0\x80",2),std::string("Nul\0name",8),std::string(2049,'\xc2')}) {
    auto patch=materialBindingPatch("makeUnique");patch["runtimeMaterial"]=11;patch["name"]=name;
    EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
  }
  std::string multibyte;for(size_t i=0;i<2049;++i) multibyte+="\xc2\xa0";
  auto patch=materialBindingPatch("makeUnique");patch["runtimeMaterial"]=11;patch["name"]=multibyte+"x";
  EXPECT_THROW((void)CaptureSequence(materialBindingCapture(patch)),std::invalid_argument);
}

TEST(TemporalCapture, MaterialBindingMutationsAreIsolatedButRenderSettingsAndSeparateHistoryEventsRemainValid) {
  std::vector<json> conflicts{reparentCaptureEvent(),transformCaptureEvent(),primitiveCaptureEvent(),
      {{"materialEdit",materialPatch()}},{{"nodeAuthoring",{{"action","rename"},{"sourceNode",0},{"name","New"}}}},
      {{"animationPlaying",false}},{{"animationClip",-1}},{{"animationTime",0}},{{"merge",{{"path","B.glb"}}}},
      {{"reload","A.glb"}},{{"guiReload","A.glb"}},{{"guiSampleModel","A"}},{{"guiUnload",true}},{{"gltfScene",0}},
      {{"materialVariant",-1}},{{"nodeFlags",{{"sourceNode",0},{"visible",false}}}},{{"sceneHistory","undo"}},
      {{"objectVisible",false}},{{"bimHiddenObject",0}},{{"physicsNode",0}},{{"physicsAttach","dynamic"}},
      {{"physicsAction","pause"}},{{"physicsJointAction","clear"}},{{"physicsJointMotorVelocity",0}},
      {{"physicsJointCreate",jointCapture()["events"][0]["physicsJointCreate"]}},
      {{"physicsAttach","static"},{"physicsSettings",json::object()}}};
  for(const auto& conflict:conflicts) {
    auto event=conflict;event["materialBinding"]=materialBindingPatch();
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument)<<event.dump();
  }
  auto event=json{{"materialBinding",materialBindingPatch()},{"taa",false},{"exposure",.35},{"samples",4},
      {"resize",{640,360}},{"reload",""},{"guiReload",""},{"guiSampleModel",""},{"guiUnload",false},
      {"physicsAttach",""},{"physicsAction",""},{"physicsJointAction",""}};
  EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture(event)));
  const json separate{{"schemaVersion",1},{"frames",3},{"events",json::array({
      {{"frame",1},{"materialBinding",materialBindingPatch()}},{{"frame",2},{"sceneHistory","undo"}},
      {{"frame",3},{"sceneHistory","redo"}}})}};
  EXPECT_NO_THROW((void)CaptureSequence(separate));
}

namespace {
json texturePatch(const char* operation="clear",const char* slot="baseColor") {
  return {{"sourceMaterial",2},{"slot",slot},{"operation",operation}};
}
json textureCapture(json request) {return typedEditCapture({{"materialTextureEdit",std::move(request)}});}
} // namespace

TEST(TemporalCapture, TextureEditKeepsTypedPathSamplerAndResetLeavesAtOnlyItsNamedFrame) {
  using Operation=container::material::MaterialTextureEditOperation;
  auto patch=texturePatch("importFile","normal");patch["path"]="C:/Textures/normal.png";patch["expectedResourceRevision"]=uint64_t(19);
  patch["sampler"]={{"minFilter",9728},{"magFilter",9729},{"wrapS",33071},{"wrapT",33648}};
  patch["mapping"]={{"texCoord",1},{"offset",{-.25,.5}},{"scale",nullptr},{"rotation",-0.0}};
  const auto input=textureCapture(patch),before=input;const CaptureSequence sequence(input);
  EXPECT_FALSE(sequence.sample(1).event.materialTextureEdit);
  const auto actual=sequence.sample(2).event.materialTextureEdit;ASSERT_TRUE(actual);
  EXPECT_EQ(actual->operation,Operation::ImportFile);EXPECT_EQ(actual->slot,container::geometry::gltf::GltfMaterialTextureSlot::Normal);
  EXPECT_EQ(actual->sourceMaterial,2);EXPECT_EQ(actual->path,"C:/Textures/normal.png");EXPECT_EQ(actual->expectedResourceRevision,19u);
  ASSERT_TRUE(actual->sampler);EXPECT_EQ(actual->sampler->minFilter,9728);EXPECT_EQ(actual->sampler->magFilter,9729);
  EXPECT_EQ(actual->sampler->wrapS,33071);EXPECT_EQ(actual->sampler->wrapT,33648);
  ASSERT_TRUE(actual->mapping.texCoord);EXPECT_EQ(std::get<uint32_t>(*actual->mapping.texCoord),1u);
  ASSERT_TRUE(actual->mapping.offset);EXPECT_EQ((std::get<std::array<double,2>>(*actual->mapping.offset)),(std::array<double,2>{-.25,.5}));
  ASSERT_TRUE(actual->mapping.scale);EXPECT_TRUE(std::holds_alternative<std::monostate>(*actual->mapping.scale));
  ASSERT_TRUE(actual->mapping.rotation);EXPECT_TRUE(std::signbit(std::get<double>(*actual->mapping.rotation)));
  EXPECT_FALSE(actual->mapping.channel);EXPECT_FALSE(sequence.sample(3).event.materialTextureEdit);EXPECT_EQ(input,before);
}

TEST(TemporalCapture, TextureEditRecognizesEveryRegisteredSlotWithoutGuessingAnEvaluatedOrRuntimeIndex) {
  using Operation=container::material::MaterialTextureEditOperation;
  for(size_t i=0;i<container::material::kMaterialTextureSlotNames.size();++i) {
    auto patch=texturePatch("bindExisting");patch["slot"]=std::string(container::material::kMaterialTextureSlotNames[i]);patch["sourceTexture"]=8;
    const auto actual=CaptureSequence(textureCapture(patch)).sample(2).event.materialTextureEdit;ASSERT_TRUE(actual);
    EXPECT_EQ(size_t(actual->slot),i);EXPECT_EQ(actual->sourceTexture,8u);EXPECT_EQ(actual->operation,Operation::BindExisting);
    EXPECT_FALSE(actual->path);EXPECT_FALSE(actual->expectedResourceRevision);
  }
}

TEST(TemporalCapture, TextureEditsAcceptAllExplicitSupportedSamplerPoliciesAndDefaultOmission) {
  for(int min:{-1,9728,9729,9984,9985,9986,9987}) for(int mag:{-1,9728,9729})
    for(int u:{10497,33071,33648}) for(int v:{10497,33071,33648}) {
      auto patch=texturePatch("editSampler");patch["sampler"]={{"minFilter",min},{"magFilter",mag},{"wrapS",u},{"wrapT",v}};
      const auto actual=CaptureSequence(textureCapture(patch)).sample(2).event.materialTextureEdit;ASSERT_TRUE(actual);ASSERT_TRUE(actual->sampler);
      EXPECT_EQ(actual->sampler->minFilter,min);EXPECT_EQ(actual->sampler->magFilter,mag);EXPECT_EQ(actual->sampler->wrapS,u);EXPECT_EQ(actual->sampler->wrapT,v);
    }
  auto patch=texturePatch("editSampler");patch["sampler"]=json::object();
  const auto actual=CaptureSequence(textureCapture(patch)).sample(2).event.materialTextureEdit;ASSERT_TRUE(actual);ASSERT_TRUE(actual->sampler);
  EXPECT_EQ(actual->sampler->minFilter,-1);EXPECT_EQ(actual->sampler->magFilter,-1);EXPECT_EQ(actual->sampler->wrapS,10497);
}

TEST(TemporalCapture, TextureEditIdentitiesRetainIntegerBoundsAndNeverAcceptFloatingOrNegativeIds) {
  auto patch=texturePatch("bindExisting");patch["sourceMaterial"]=uint64_t(INT32_MAX);patch["sourceTexture"]=uint64_t(INT32_MAX);
  patch["expectedResourceRevision"]=UINT64_MAX;
  const auto actual=CaptureSequence(textureCapture(patch)).sample(2).event.materialTextureEdit;ASSERT_TRUE(actual);
  EXPECT_EQ(actual->sourceMaterial,INT32_MAX);EXPECT_EQ(actual->sourceTexture,uint32_t(INT32_MAX));EXPECT_EQ(actual->expectedResourceRevision,UINT64_MAX);
  for(const auto* field:{"sourceMaterial","sourceTexture"}) for(const json invalid:{json(-1),json(1.0),json(true),json("0"),json(uint64_t(INT32_MAX)+1)}) {
    auto bad=patch;bad[field]=invalid;EXPECT_THROW((void)CaptureSequence(textureCapture(bad)),std::invalid_argument)<<bad.dump();
  }
  for(const json invalid:{json(0),json(-1),json(1.0),json(true),json("19")}) {
    auto bad=patch;bad["expectedResourceRevision"]=invalid;EXPECT_THROW((void)CaptureSequence(textureCapture(bad)),std::invalid_argument);
  }
}

TEST(TemporalCapture, TextureOperationPayloadsAreExclusiveAndUnknownFieldsCannotCarryOpaqueHandles) {
  std::vector<json> invalid;
  for(const auto* missing:{"sourceMaterial","slot","operation"}) {auto bad=texturePatch();bad.erase(missing);invalid.push_back(std::move(bad));}
  for(const auto* operation:{"importFile","bindExisting","editMapping","editSampler"}) invalid.push_back(texturePatch(operation));
  auto bad=texturePatch();bad["path"]="image.png";invalid.push_back(bad);
  bad=texturePatch("importFile");bad["path"]="image.png";bad["sourceTexture"]=0;invalid.push_back(bad);
  bad=texturePatch("bindExisting");bad["sourceTexture"]=0;bad["path"]="image.png";invalid.push_back(bad);
  bad=texturePatch("editMapping");bad["mapping"]={{"texCoord",0}};bad["sampler"]=json::object();invalid.push_back(bad);
  bad=texturePatch("editSampler");bad["sampler"]=json::object();bad["mapping"]={{"rotation",0}};invalid.push_back(bad);
  for(const auto* field:{"runtimeTexture","imageView","ownerCookie","gestureId","unexpected"}) {bad=texturePatch();bad[field]=1;invalid.push_back(bad);}
  for(const json slot:{json("base-color"),json("unknown"),json(0),json(true)}) {bad=texturePatch();bad["slot"]=slot;invalid.push_back(bad);}
  bad=texturePatch("paint");invalid.push_back(bad);
  for(const auto& request:invalid) EXPECT_THROW((void)CaptureSequence(textureCapture(request)),std::invalid_argument)<<request.dump();
}

TEST(TemporalCapture, TexturePathByteCapsAndEmbeddedNullRejectBeforeOwnedRequestGrowth) {
  auto patch=texturePatch("importFile");patch["path"]=std::string(65536,'x');
  EXPECT_NO_THROW((void)CaptureSequence(textureCapture(patch)));
  for(const json path:{json(""),json(std::string(65537,'x')),json(std::string("a\0b",3)),json(false),json::array()}) {
    patch["path"]=path;EXPECT_THROW((void)CaptureSequence(textureCapture(patch)),std::invalid_argument);
  }
  std::string multibyte;for(size_t i=0;i<32769;++i) multibyte+="\xc2\xa0";
  patch["path"]=multibyte;EXPECT_THROW((void)CaptureSequence(textureCapture(patch)),std::invalid_argument);
  patch=texturePatch();patch[std::string(33,'x')]=0;EXPECT_THROW((void)CaptureSequence(textureCapture(patch)),std::invalid_argument);
}

TEST(TemporalCapture, TextureMappingLeavesDistinguishResetFromExactZeroAndRejectMalformedVectors) {
  auto patch=texturePatch("editMapping","height");patch["mapping"]={{"texCoord",nullptr},{"channel",0},{"offset",nullptr},{"scale",{0.,1.}},{"rotation",0.}};
  const auto actual=CaptureSequence(textureCapture(patch)).sample(2).event.materialTextureEdit;ASSERT_TRUE(actual);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(*actual->mapping.texCoord));EXPECT_EQ(std::get<uint32_t>(*actual->mapping.channel),0u);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(*actual->mapping.offset));
  EXPECT_EQ((std::get<std::array<double,2>>(*actual->mapping.scale)),(std::array<double,2>{0.,1.}));
  for(const auto& invalid:std::vector<json>{json::object(),{{"texCoord",2}},{{"texCoord",1.0}},{{"channel",5}},
      {{"offset",{0}}},{{"offset",{0,0,0}}},{{"scale",false}},{{"rotation",std::numeric_limits<double>::infinity()}},
      {{"rotation",double(std::numeric_limits<float>::max())*2}},{{"offset",{0,std::numeric_limits<double>::quiet_NaN()}}},{{"unknown",0}}}) {
    patch["mapping"]=invalid;EXPECT_THROW((void)CaptureSequence(textureCapture(patch)),std::invalid_argument);
  }
}

TEST(TemporalCapture, TextureSamplerRejectsUnsupportedEnumsShapesAndUnknownProperties) {
  auto patch=texturePatch("editSampler");
  for(const auto& sampler:std::vector<json>{nullptr,false,json::array(),{{"minFilter",0}},{{"magFilter",9987}},
      {{"wrapS",0}},{{"wrapT",-1}},{{"minFilter",9728.0}},{{"magFilter",true}},{{"anisotropy",8}}}) {
    patch["sampler"]=sampler;EXPECT_THROW((void)CaptureSequence(textureCapture(patch)),std::invalid_argument);
  }
}

TEST(TemporalCapture, TextureMutationsAreIsolatedWhileRenderSettingsAndSeparateHistoryEventsRemainValid) {
  const std::vector<json> conflicts{reparentCaptureEvent(),transformCaptureEvent(),primitiveCaptureEvent(),
      {{"materialEdit",materialPatch()}},{{"materialBinding",materialBindingPatch()}},{{"nodeAuthoring",{{"action","rename"},{"sourceNode",0},{"name","New"}}}},
      {{"animationPlaying",false}},{{"animationClip",-1}},{{"animationTime",0}},{{"merge",{{"path","B.glb"}}}},
      {{"reload","A.glb"}},{{"guiReload","A.glb"}},{{"guiSampleModel","A"}},{{"guiUnload",true}},{{"gltfScene",0}},
      {{"materialVariant",-1}},{{"nodeFlags",{{"sourceNode",0},{"visible",false}}}},{{"sceneHistory","undo"}},
      {{"objectVisible",false}},{{"bimHiddenObject",0}},{{"physicsNode",0}},{{"physicsAttach","dynamic"}},
      {{"physicsAction","pause"}},{{"physicsJointAction","clear"}},{{"physicsJointMotorVelocity",0}}};
  for(auto event:conflicts) {
    event["materialTextureEdit"]=texturePatch();EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument)<<event.dump();
  }
  json event{{"materialTextureEdit",texturePatch()},{"taa",false},{"exposure",.35},{"samples",4},{"resize",{640,360}},
      {"reload",""},{"guiReload",""},{"guiSampleModel",""},{"guiUnload",false},{"physicsAttach",""},{"physicsAction",""},{"physicsJointAction",""}};
  EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture(event)));
  json sequence{{"schemaVersion",1},{"frames",3},{"events",json::array({
      {{"frame",1},{"materialTextureEdit",texturePatch()}},{{"frame",2},{"sceneHistory","undo"}},{{"frame",3},{"sceneHistory","redo"}}})}};
  EXPECT_NO_THROW((void)CaptureSequence(sequence));
}

TEST(TemporalCapture, ExpectedTextureFailureIsAnEventFlagAndDefaultsToFatal) {
  auto input=textureCapture(texturePatch());
  EXPECT_FALSE(CaptureSequence(input).sample(2).event.expectMaterialTextureFailure);
  for(const bool expected:{false,true}) {
    input["events"][0]["expectMaterialTextureFailure"]=expected;
    const auto actual=CaptureSequence(input).sample(2).event;
    ASSERT_TRUE(actual.materialTextureEdit);
    EXPECT_EQ(actual.expectMaterialTextureFailure,expected);
    EXPECT_FALSE(CaptureSequence(input).sample(1).event.expectMaterialTextureFailure);
    EXPECT_FALSE(CaptureSequence(input).sample(3).event.expectMaterialTextureFailure);
  }
  auto nested=texturePatch();nested["expectMaterialTextureFailure"]=true;
  EXPECT_THROW((void)CaptureSequence(textureCapture(nested)),std::invalid_argument);
}

TEST(TemporalCapture, ExpectedTextureFailureRequiresATextureActionAndStrictBoolean) {
  for(const bool expected:{false,true}) {
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"expectMaterialTextureFailure",expected}})),std::invalid_argument);
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"materialEdit",materialPatch()},{"expectMaterialTextureFailure",expected}})),std::invalid_argument);
  }
  for(const json invalid:{json(nullptr),json(0),json(1),json("true"),json::array(),json::object()}) {
    auto input=textureCapture(texturePatch());input["events"][0]["expectMaterialTextureFailure"]=invalid;
    EXPECT_THROW((void)CaptureSequence(input),std::invalid_argument)<<invalid.dump();
  }
}

namespace {
json retirementPatch() {return {{"runtimeMaterial",3},{"sourceMaterial",1}};}
json retirementCapture(json request) {return typedEditCapture({{"materialRetirement",std::move(request)}});}
}

TEST(TemporalCapture, MaterialRetirementKeepsExplicitIdentitiesAndIsOneShot) {
  auto input=retirementCapture(retirementPatch());const auto before=input;
  const CaptureSequence sequence(input);const auto event=sequence.sample(2).event;
  ASSERT_TRUE(event.materialRetirement);
  EXPECT_EQ(event.materialRetirement->runtimeMaterial,3u);
  EXPECT_EQ(event.materialRetirement->sourceMaterial,1);
  EXPECT_FALSE(event.materialRetirement->expectedAuthoredRevision);
  EXPECT_FALSE(event.expectMaterialRetirementFailure);
  EXPECT_FALSE(sequence.sample(1).event.materialRetirement);
  EXPECT_FALSE(sequence.sample(3).event.materialRetirement);
  EXPECT_EQ(input,before);
  auto runtimeOnly=retirementPatch();runtimeOnly["sourceMaterial"]=-1;runtimeOnly["runtimeMaterial"]=0;
  runtimeOnly["expectedAuthoredRevision"]=9007199254740991ull;
  const auto parsed=CaptureSequence(retirementCapture(runtimeOnly)).sample(2).event.materialRetirement;
  ASSERT_TRUE(parsed);EXPECT_EQ(parsed->sourceMaterial,-1);EXPECT_EQ(parsed->runtimeMaterial,0u);
  EXPECT_EQ(parsed->expectedAuthoredRevision,9007199254740991ull);
}

TEST(TemporalCapture, MaterialRetirementRejectsMalformedOrAmbiguousIdentitiesBeforeExecution) {
  for(const char* field:{"runtimeMaterial","sourceMaterial"}) {
    auto request=retirementPatch();request.erase(field);
    EXPECT_THROW((void)CaptureSequence(retirementCapture(request)),std::invalid_argument);
  }
  for(const json invalid:{json(nullptr),json(true),json(1.5),json("1"),json::array(),json::object()}) {
    for(const char* field:{"runtimeMaterial","sourceMaterial","expectedAuthoredRevision"}) {
      auto request=retirementPatch();request[field]=invalid;
      EXPECT_THROW((void)CaptureSequence(retirementCapture(request)),std::invalid_argument)<<field<<invalid.dump();
    }
  }
  for(const json invalid:{json(-1),json(4294967295ull),json(UINT64_MAX)}) {
    auto request=retirementPatch();request["runtimeMaterial"]=invalid;
    EXPECT_THROW((void)CaptureSequence(retirementCapture(request)),std::invalid_argument);
  }
  for(const json invalid:{json(-2),json(2147483648ull),json(UINT64_MAX)}) {
    auto request=retirementPatch();request["sourceMaterial"]=invalid;
    EXPECT_THROW((void)CaptureSequence(retirementCapture(request)),std::invalid_argument);
  }
  for(const json invalid:{json(0),json(-1),json(9007199254740992ull),json(UINT64_MAX)}) {
    auto request=retirementPatch();request["expectedAuthoredRevision"]=invalid;
    EXPECT_THROW((void)CaptureSequence(retirementCapture(request)),std::invalid_argument);
  }
  for(const char* field:{"operation","retired","graphNode","expectedMaterial","expectMaterialRetirementFailure"}) {
    auto request=retirementPatch();request[field]=0;
    EXPECT_THROW((void)CaptureSequence(retirementCapture(request)),std::invalid_argument);
  }
}

TEST(TemporalCapture, MaterialRetirementRequiresSeparateSceneHistoryMaterialAndTimelineEvents) {
  const std::vector<json> conflicts{
    {{"materialEdit",materialPatch()}},{{"materialBinding",materialBindingPatch()}},{{"materialTextureEdit",texturePatch()}},
    {{"sceneHistory","undo"}},{{"animationTime",.75}},{{"animationPlaying",false}},{{"animationClip",0}},
    {{"gltfScene",0}},{{"materialVariant",-1}},{{"nodeFlags",{{"sourceNode",0},{"visible",false}}}},
    {{"nodeAuthoring",{{"action","rename"},{"sourceNode",0},{"name","renamed"}}}},
    {{"reload","other.gltf"}},{{"guiReload","other.gltf"}},{{"guiSampleModel","other"}},{{"guiUnload",true}},
    {{"merge",{{"path","other.gltf"}}}},{{"objectVisible",false}},{{"bimHiddenObject",0}},
    {{"physicsAttach","static"}},{{"physicsAction","pause"}},{{"physicsNode",0}},{{"physicsSettings",json::object()}}};
  for(auto event:conflicts) {
    event["materialRetirement"]=retirementPatch();
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument)<<event.dump();
  }
  auto event=json{{"materialRetirement",retirementPatch()},{"taa",false},{"exposure",.35},{"samples",4},{"resize",{640,360}}};
  EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture(event)));
  json sequence{{"schemaVersion",1},{"frames",3},{"events",json::array({
      {{"frame",1},{"materialRetirement",retirementPatch()}},{{"frame",2},{"sceneHistory","undo"}},{{"frame",3},{"sceneHistory","redo"}}})}};
  EXPECT_NO_THROW((void)CaptureSequence(sequence));
}

TEST(TemporalCapture, ExpectedMaterialRetirementFailureRequiresTheActionAndStrictBoolean) {
  for(const bool expected:{false,true}) {
    auto input=retirementCapture(retirementPatch());input["events"][0]["expectMaterialRetirementFailure"]=expected;
    const auto event=CaptureSequence(input).sample(2).event;ASSERT_TRUE(event.materialRetirement);
    EXPECT_EQ(event.expectMaterialRetirementFailure,expected);
    EXPECT_FALSE(CaptureSequence(input).sample(1).event.expectMaterialRetirementFailure);
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"expectMaterialRetirementFailure",expected}})),std::invalid_argument);
  }
  for(const json invalid:{json(nullptr),json(0),json(1),json("true"),json::array(),json::object()}) {
    auto input=retirementCapture(retirementPatch());input["events"][0]["expectMaterialRetirementFailure"]=invalid;
    EXPECT_THROW((void)CaptureSequence(input),std::invalid_argument);
  }
}

namespace {
json resourceRetirementPatch() {return {{"domain","archive"},{"kind","texture"},{"row",2}};}
json resourceRetirementCapture(json request) {return typedEditCapture({{"resourceRetirement",std::move(request)}});}
}

TEST(TemporalCapture, ResourceRetirementUsesExplicitDomainKindRowAndOneShotIntent) {
  auto input=resourceRetirementCapture(resourceRetirementPatch());const auto before=input;
  const auto event=CaptureSequence(input).sample(2).event;ASSERT_TRUE(event.resourceRetirement);
  EXPECT_EQ(event.resourceRetirement->domain,container::ui::ResourceCatalogDomain::Archive);
  EXPECT_EQ(event.resourceRetirement->kind,container::ui::ResourceCatalogKind::Texture);
  EXPECT_EQ(event.resourceRetirement->row,2u);EXPECT_FALSE(event.resourceRetirement->expectedResourceRevision);
  EXPECT_FALSE(CaptureSequence(input).sample(1).event.resourceRetirement);EXPECT_EQ(input,before);
  auto runtime=resourceRetirementPatch();runtime["domain"]="runtime";runtime["kind"]="runtimeTexture";runtime["row"]=0;
  runtime["expectedResourceRevision"]=9007199254740991ull;runtime["expectedIncarnation"]=1;
  const auto parsed=CaptureSequence(resourceRetirementCapture(runtime)).sample(2).event.resourceRetirement;
  ASSERT_TRUE(parsed);EXPECT_EQ(parsed->expectedResourceRevision,9007199254740991ull);EXPECT_EQ(parsed->expectedIncarnation,1u);
}

TEST(TemporalCapture, ResourceRetirementRejectsInvalidIdentitiesUnknownFieldsAndDomainKindPairs) {
  for(const char* field:{"domain","kind","row"}) {
    auto request=resourceRetirementPatch();request.erase(field);
    EXPECT_THROW((void)CaptureSequence(resourceRetirementCapture(request)),std::invalid_argument);
  }
  for(const json invalid:{json(nullptr),json(true),json(1.5),json("1"),json::array(),json::object(),json(-1),json(UINT64_MAX)}) {
    for(const char* field:{"row","expectedResourceRevision","expectedIncarnation"}) {
      auto request=resourceRetirementPatch();request[field]=invalid;
      EXPECT_THROW((void)CaptureSequence(resourceRetirementCapture(request)),std::invalid_argument)<<field<<invalid;
    }
  }
  for(const char* domain:{"archive","evaluated","runtime","foreign"}) for(const char* kind:{"image","texture","sampler","runtimeTexture","foreign"}) {
    auto request=resourceRetirementPatch();request["domain"]=domain;request["kind"]=kind;
    const bool valid=std::string(domain)!="foreign" && std::string(kind)!="foreign" &&
        ((std::string(domain)=="runtime")== (std::string(kind)=="runtimeTexture"));
    if(valid) {EXPECT_NO_THROW((void)CaptureSequence(resourceRetirementCapture(request)));}
    else {EXPECT_THROW((void)CaptureSequence(resourceRetirementCapture(request)),std::invalid_argument);}
  }
  for(const char* field:{"asset","operation","expectedAuthoredRevision","sourceMaterial","retired"}) {
    auto request=resourceRetirementPatch();request[field]=0;
    EXPECT_THROW((void)CaptureSequence(resourceRetirementCapture(request)),std::invalid_argument);
  }
  for(const char* field:{"expectedResourceRevision","expectedIncarnation"}) for(const json invalid:{json(0),json(9007199254740992ull)}) {
    auto request=resourceRetirementPatch();request[field]=invalid;
    EXPECT_THROW((void)CaptureSequence(resourceRetirementCapture(request)),std::invalid_argument);
  }
  auto request=resourceRetirementPatch();request["row"]=1000000;
  EXPECT_THROW((void)CaptureSequence(resourceRetirementCapture(request)),std::invalid_argument);
}

TEST(TemporalCapture, ResourceRetirementRejectsOtherMutationsButAllowsDisplayControls) {
  for(auto event:std::vector<json>{{{"materialRetirement",retirementPatch()}},{{"materialEdit",materialPatch()}},
      {{"materialBinding",materialBindingPatch()}},{{"materialTextureEdit",texturePatch()}},{{"sceneHistory","undo"}},
      {{"animationPlaying",false}},{{"animationClip",0}},{{"animationTime",0}},{{"gltfScene",0}},{{"guiUnload",true}},
      {{"reload","other.gltf"}},{{"merge",{{"path","other.gltf"}}}},{{"objectVisible",false}},{{"physicsNode",0}}}) {
    event["resourceRetirement"]=resourceRetirementPatch();
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument)<<event;
  }
  EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"resourceRetirement",resourceRetirementPatch()},
      {"taa",false},{"exposure",.35},{"samples",4},{"resize",{640,360}}})));
}

TEST(TemporalCapture, ExpectedResourceFailureRequiresActionAndBoolean) {
  for(const bool expected:{false,true}) {
    auto input=resourceRetirementCapture(resourceRetirementPatch());input["events"][0]["expectResourceRetirementFailure"]=expected;
    EXPECT_EQ(CaptureSequence(input).sample(2).event.expectResourceRetirementFailure,expected);
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"expectResourceRetirementFailure",expected}})),std::invalid_argument);
  }
  for(const json invalid:{json(nullptr),json(0),json(1),json("true"),json::array(),json::object()}) {
    auto input=resourceRetirementCapture(resourceRetirementPatch());input["events"][0]["expectResourceRetirementFailure"]=invalid;
    EXPECT_THROW((void)CaptureSequence(input),std::invalid_argument);
  }
}

namespace {
json cameraProjectionPatch() {
  return {{"sourceNode",2},{"values",json::array({{{"property","perspective/yfov"},{"value",.7}},
      {{"property","perspective/aspectRatio"},{"value",nullptr}},{{"property","perspective/zfar"},{"value",nullptr}}})}};
}
}

TEST(TemporalCapture, CameraProjectionUsesCanonicalFieldsRadiansAndExplicitOmission) {
  using namespace container::geometry::gltf;
  auto request=cameraProjectionPatch();request["expectedCameraRevision"]=9007199254740991ull;
  const auto event=CaptureSequence(typedEditCapture({{"cameraProjection",request}})).sample(2).event;
  ASSERT_TRUE(event.cameraProjection);EXPECT_EQ(event.cameraProjection->sourceNode,2);
  EXPECT_EQ(event.cameraProjection->expectedCameraRevision,9007199254740991ull);
  const auto& values=event.cameraProjection->patch.values;ASSERT_EQ(values.size(),3u);
  EXPECT_EQ(values[0].property,GltfCameraProperty::PerspectiveYfov);EXPECT_DOUBLE_EQ(std::get<double>(values[0].value),.7);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(values[1].value));EXPECT_TRUE(std::holds_alternative<std::monostate>(values[2].value));
}

TEST(TemporalCapture, CameraProjectionOrthographicZeroNearAndTypeSwitchStayExplicit) {
  const json request={{"sourceNode",INT32_MAX},{"type","orthographic"},{"values",json::array({
      {{"property","orthographic/xmag"},{"value",4.}},{{"property","orthographic/ymag"},{"value",2.}},
      {{"property","orthographic/znear"},{"value",0.}},{{"property","orthographic/zfar"},{"value",100.}}})}};
  const auto action=CaptureSequence(typedEditCapture({{"cameraProjection",request}})).sample(2).event.cameraProjection;
  ASSERT_TRUE(action);EXPECT_EQ(action->patch.type,container::geometry::gltf::GltfCameraProjection::Orthographic);
  EXPECT_EQ(action->sourceNode,INT32_MAX);EXPECT_DOUBLE_EQ(std::get<double>(action->patch.values[2].value),0.);
}

TEST(TemporalCapture, CameraSelectionSupportsIndexUniqueNodeNameAndViewportWithoutAnEdit) {
  for(const json selector:{json{{"sourceNode",0}},json{{"nodeName","Parented camera"}},json{{"viewport",true}}}) {
    const auto event=CaptureSequence(typedEditCapture({{"cameraSelection",selector}})).sample(2).event;
    ASSERT_TRUE(event.cameraSelection);EXPECT_FALSE(event.cameraProjection);
    EXPECT_EQ(event.cameraSelection->viewport,selector.contains("viewport"));
    EXPECT_EQ(event.cameraSelection->nodeName.has_value(),selector.contains("nodeName"));
    EXPECT_EQ(event.cameraSelection->sourceNode.has_value(),selector.contains("sourceNode"));
  }
}

TEST(TemporalCapture, CameraSelectionRejectsAmbiguousMalformedAndOversizedSelectors) {
  for(const json selector:{json(nullptr),json(true),json::array(),json::object(),json{{"viewport",false}},json{{"viewport",1}},
      json{{"sourceNode",-1}},json{{"sourceNode",1.}},json{{"sourceNode",true}},json{{"sourceNode",UINT64_MAX}},
      json{{"nodeName",""}},json{{"nodeName",std::string(4097,'x')}},json{{"nodeName","bad\nname"}},
      json{{"sourceNode",0},{"viewport",true}},json{{"sourceNode",0},{"nodeName","Camera"}},json{{"cameraName","Camera"}}})
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraSelection",selector}})),std::invalid_argument)<<selector;
}

TEST(TemporalCapture, CameraProjectionRejectsInvalidPropertiesTypesDuplicateLeavesAndRequiredOmissions) {
  for(const json edit:{json{{"property","perspective/yfov"},{"value",nullptr}},json{{"property","orthographic/xmag"},{"value",nullptr}},
      json{{"property","perspective/yfov/0"},{"value",.8}},json{{"property","unknown"},{"value",1}},
      json{{"property",true},{"value",1}},json{{"property","perspective/yfov"}},json{{"property","perspective/yfov"},{"value",.8},{"extra",0}}}) {
    auto request=cameraProjectionPatch();request["values"]=json::array({edit});
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument)<<request;
  }
  auto request=cameraProjectionPatch();request["values"].push_back(request["values"][0]);
  EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument);
  request=cameraProjectionPatch();request["type"]="orthographic";
  EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument);
  for(const json type:{json(true),json(nullptr),json(0),json("Perspective"),json("invalid")}) {
    request=cameraProjectionPatch();request["type"]=type;
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument);
  }
}

TEST(TemporalCapture, CameraProjectionRejectsNonfiniteAndOutOfDomainNumericValues) {
  for(const char* property:{"perspective/yfov","perspective/aspectRatio","perspective/znear","perspective/zfar","orthographic/xmag","orthographic/ymag","orthographic/zfar"})
    for(const json value:{json(0),json(-1),json(true),json("1"),json::array(),json::object(),json(std::numeric_limits<double>::infinity()),json(std::numeric_limits<double>::quiet_NaN()),json(std::numeric_limits<double>::max())}) {
      auto request=cameraProjectionPatch();request["values"]=json::array({{{"property",property},{"value",value}}});
      EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument)<<property;
    }
  auto request=cameraProjectionPatch();request["values"][0]["value"]=3.14159265358979323846;
  EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument);
}

TEST(TemporalCapture, CameraProjectionRequiresBoundedExactIdentityAndNonemptyKnownPatch) {
  for(const char* field:{"sourceNode","expectedCameraRevision"}) for(const json bad:{json(true),json(nullptr),json(1.),json(-1),json(UINT64_MAX)}) {
    auto request=cameraProjectionPatch();request[field]=bad;
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument)<<field;
  }
  for(const json request:{json::object(),json{{"sourceNode",0}},json{{"sourceNode",0},{"values",json::array()}},
      json{{"sourceNode",0},{"values",json::object()}},json{{"sourceNode",0},{"unexpected",0}},
      json{{"sourceNode",0},{"type","perspective"},{"expectedCameraRevision",0}}})
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraProjection",request}})),std::invalid_argument)<<request;
}

TEST(TemporalCapture, CameraActionsRequireSeparateMutationEventsAndAllowDisplayChanges) {
  for(const char* action:{"cameraProjection","cameraSelection","cameraPose"}) {
    const json request=std::string_view(action)=="cameraProjection"?cameraProjectionPatch():
        std::string_view(action)=="cameraPose"?json{{"sourceNode",0},{"fromViewport",true}}:json{{"sourceNode",0}};
    for(json event:std::vector<json>{{{"cameraProjection",cameraProjectionPatch()},{"cameraSelection",{{"viewport",true}}}},
        {{"materialEdit",materialPatch()}},{{"materialBinding",materialBindingPatch()}},{{"resourceRetirement",resourceRetirementPatch()}},
        {{"sceneHistory","undo"}},{{"animationTime",.5}},{{"animationClip",0}},{{"animationPlaying",false}},{{"gltfScene",0}},
        {{"guiUnload",true}},{{"merge",{{"path","other.gltf"}}}},{{"objectVisible",false}},{{"physicsNode",0}}}) {
      event[action]=request;
      EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument)<<event;
    }
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{action,request},{"taa",false},{"exposure",.35},{"resize",{640,360}},{"ddgiIntensity",0}})));
  }
}

TEST(TemporalCapture, ExpectedCameraFailuresRequireMatchingActionAndStrictBoolean) {
  for(const auto& [field,action,request]:{
      std::tuple{"expectCameraProjectionFailure","cameraProjection",cameraProjectionPatch()},
      std::tuple{"expectCameraSelectionFailure","cameraSelection",json{{"sourceNode",0}}},
      std::tuple{"expectCameraPoseFailure","cameraPose",json{{"sourceNode",0},{"fromViewport",true}}}}) {
    for(const bool expected:{false,true}) {
      EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{field,expected},{action,request}})));
      EXPECT_THROW((void)CaptureSequence(typedEditCapture({{field,expected}})),std::invalid_argument);
    }
    for(const json bad:{json(nullptr),json(0),json(1),json("true"),json::array(),json::object()})
      EXPECT_THROW((void)CaptureSequence(typedEditCapture({{field,bad},{action,request}})),std::invalid_argument);
  }
}

TEST(TemporalCapture, CameraPoseParsesCurrentViewportIntentWithoutInjectedTransformOrLens) {
  CaptureSequence sequence(typedEditCapture({{"cameraPose",{{"sourceNode",4},{"fromViewport",true},{"expectedCameraRevision",23}}}}));
  const auto event=sequence.sample(2).event;ASSERT_TRUE(event.cameraPose);
  EXPECT_EQ(event.cameraPose->sourceNode,4);EXPECT_EQ(event.cameraPose->expectedCameraRevision,23u);
  EXPECT_FALSE(event.expectCameraPoseFailure);EXPECT_FALSE(event.cameraProjection);EXPECT_FALSE(event.cameraSelection);
}

TEST(TemporalCapture, CameraPoseRejectsMalformedOrInjectedSourceViewData) {
  for(const json request:{json(),json(true),json::array(),json::object(),json{{"sourceNode",0}},
      json{{"fromViewport",true}},json{{"sourceNode",0},{"fromViewport",false}},
      json{{"sourceNode",0},{"fromViewport",1}},json{{"sourceNode",0},{"fromViewport","true"}}})
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraPose",request}})),std::invalid_argument);
  for(const char* field:{"matrix","view","pose","position","camera","lens","values"}) {
    json request{{"sourceNode",0},{"fromViewport",true}};request[field]=json::array();
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraPose",request}})),std::invalid_argument)<<field;
  }
}

TEST(TemporalCapture, CameraPoseRejectsInexactIdentitiesAndOtherCameraActions) {
  for(const char* field:{"sourceNode","expectedCameraRevision"})
    for(const json value:{json(true),json(nullptr),json(1.),json(-1),json(UINT64_MAX),json("1")}) {
      json request{{"sourceNode",0},{"fromViewport",true}};request[field]=value;
      EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"cameraPose",request}})),std::invalid_argument);
    }
  for(const auto& other:std::vector<json>{{{"cameraSelection",{{"viewport",true}}}},{{"cameraProjection",cameraProjectionPatch()}}}) {
    auto event=other;event["cameraPose"]={{"sourceNode",0},{"fromViewport",true}};
    EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument);
  }
}

namespace {
json lightTarget(const char* type="point") {return {{"type",type},{"source","imported"},{"nodeName","Selected light"}};}
json lightCommit() {return {{"target",lightTarget()},{"phase","commit"},{"values",{{"intensity",7.},{"position",{1.,2.,3.}}}}};}
}
TEST(TemporalCapture, LightNumericCommitKeepsTypedIdentityAndFiniteValues) {
  auto action=lightCommit();action["expectedLightRevision"]=9007199254740991ull;
  const auto event=CaptureSequence(typedEditCapture({{"lightEdit",action}})).sample(2).event;
  ASSERT_TRUE(event.lightEdit);EXPECT_EQ(event.lightEdit->target.nodeName,"Selected light");
  EXPECT_EQ(event.lightEdit->target.type,container::renderer::EditableLightType::Point);
  EXPECT_EQ(event.lightEdit->target.source,container::renderer::EditableLightSource::Imported);
  EXPECT_EQ(event.lightEdit->expectedLightRevision,9007199254740991ull);EXPECT_EQ(event.lightEdit->gesture,0u);
  ASSERT_TRUE(event.lightEdit->values);EXPECT_EQ(event.lightEdit->values->values.position,glm::vec3(1,2,3));
  EXPECT_EQ(event.lightEdit->values->values.intensity,7);EXPECT_FALSE(event.lightEdit->gizmo);
}
TEST(TemporalCapture, LightSelectorsUseSeparateSourceDomainsAndAddTypes) {
  for(const char* type:{"point","spot","area","directional"}) {
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"lightSelection",{{"type",type},{"source","imported"},{"sourceNode",0}}}})));
    for(const char* source:{"manual","generated"})
      EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"lightSelection",{{"type",type},{"source",source},{"index",4294967294ull}}}})));
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"lightAdd",{{"type",type},{"expectedLightRevision",1}}}})));
  }
}
TEST(TemporalCapture, LightSelectorsRejectAmbiguousUnsupportedAndForeignFields) {
  for(const json target:{json::object(),json{{"type","Point"},{"source","imported"},{"sourceNode",0}},
      json{{"type","point"},{"source","Imported"},{"sourceNode",0}},json{{"type","point"},{"source","imported"},{"index",0}},
      json{{"type","area"},{"source","manual"},{"sourceNode",0}},json{{"type","area"},{"source","generated"},{"nodeName","Name"}},
      json{{"type","spot"},{"source","imported"},{"sourceNode",0},{"nodeName","Name"}},
      json{{"type","spot"},{"source","imported"},{"sourceNode",0},{"camera",0}}})
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightSelection",target}})),std::invalid_argument)<<target;
  for(const json name:{json(""),json(std::string(4097,'x')),json("bad\nname"),json(true),json(7)}) {
    auto target=lightTarget();target["nodeName"]=name;EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightSelection",target}})),std::invalid_argument);
  }
}
TEST(TemporalCapture, LightIdentitiesRejectBooleansFloatsOverflowAndMissingRevisions) {
  for(const json value:{json(true),json(nullptr),json(1.),json(-1),json(UINT64_MAX),json("1")}) {
    auto edit=lightCommit();edit["expectedLightRevision"]=value;
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightSelection",{{"type","point"},{"source","imported"},{"sourceNode",value}}}})),std::invalid_argument);
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightSelection",{{"type","point"},{"source","manual"},{"index",value}}}})),std::invalid_argument);
  }
  auto edit=lightCommit();edit["expectedLightRevision"]=0;EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
}
TEST(TemporalCapture, LightGesturePhasesHaveExplicitPayloadAndIdentityRules) {
  for(const char* phase:{"begin","update","end","cancel"}) {
    auto edit=lightCommit();edit["phase"]=phase;edit["gesture"]=9007199254740991ull;
    if(std::string_view(phase)=="end" || std::string_view(phase)=="cancel") edit.erase("values");
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})));
    edit.erase("gesture");EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
  }
  for(const json phase:{json("Begin"),json("preview"),json(1),json(true),json(nullptr)}) {
    auto edit=lightCommit();edit["phase"]=phase;EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
  }
  auto edit=lightCommit();edit["gesture"]=1;EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
  for(const char* phase:{"end","cancel"}) {edit=lightCommit();edit["phase"]=phase;edit["gesture"]=1;EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);}
}
TEST(TemporalCapture, LightGizmoUsesCumulativeAmountAndTypedDefaults) {
  for(const char* tool:{"translate","rotate","scale"}) {
    auto edit=lightCommit();edit.erase("values");edit["gizmo"]={{"tool",tool},{"amount",{1.,2.,3.}}};
    const auto parsed=CaptureSequence(typedEditCapture({{"lightEdit",edit}})).sample(2).event.lightEdit;
    ASSERT_TRUE(parsed);ASSERT_TRUE(parsed->gizmo);EXPECT_FALSE(parsed->values);EXPECT_EQ(parsed->gizmo->amount,glm::vec3(1,2,3));
    EXPECT_EQ(parsed->gizmo->space,container::temporal::CaptureLightGizmo::Space::World);
    EXPECT_EQ(parsed->gizmo->axis,container::temporal::CaptureLightGizmo::Axis::Free);EXPECT_FALSE(parsed->gizmo->snap);
    edit["gizmo"]["space"]="local";edit["gizmo"]["axis"]="z";edit["gizmo"]["snap"]=true;
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})));
  }
}
TEST(TemporalCapture, LightGizmoRejectsUnknownKeysMalformedVectorsAndBooleanOptions) {
  for(const json gizmo:{json::object(),json{{"tool","Translate"},{"amount",{1,2,3}}},
      json{{"tool","translate"},{"amount",{1,2}}},json{{"tool","translate"},{"amount",{1,true,3}}},
      json{{"tool","rotate"},{"amount",{1,2,3}},{"axis","xy"}},json{{"tool","rotate"},{"amount",{1,2,3}},{"space","view"}},
      json{{"tool","scale"},{"amount",{1,2,3}},{"snap",1}},json{{"tool","scale"},{"amount",{1,2,3}},{"matrix",json::array()}}}) {
    auto edit=lightCommit();edit.erase("values");edit["gizmo"]=gizmo;
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument)<<gizmo;
  }
  auto edit=lightCommit();edit["gizmo"]={{"tool","translate"},{"amount",{1,2,3}}};
  EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
}
TEST(TemporalCapture, LightAreaFullDimensionsAndRightHandedBasisStayExplicit) {
  json edit{{"target",lightTarget("area")},{"phase","commit"}};
  edit["values"]["areaSize"]={4.,6.};edit["values"]["areaShape"]="disk";
  edit["values"]["areaBasis"]={{"tangent",{1.,0.,0.}},{"bitangent",{0.,0.,1.}}};
  const auto parsed=CaptureSequence(typedEditCapture({{"lightEdit",edit}})).sample(2).event.lightEdit;
  ASSERT_TRUE(parsed);ASSERT_TRUE(parsed->values);const auto& patch=*parsed->values;
  EXPECT_EQ(patch.values.areaHalfSize,glm::vec2(2,3));EXPECT_EQ(patch.values.direction,glm::vec3(0,-1,0));
  EXPECT_EQ(patch.values.areaShape,container::gpu::kAreaLightTypeDisk);
  EXPECT_NE(patch.fields&container::renderer::LightEditFieldBit(container::renderer::LightEditField::Direction),0u);
  edit["values"]["direction"]={0.,0.,-1.};const auto explicitDirection=CaptureSequence(typedEditCapture({{"lightEdit",edit}})).sample(2).event.lightEdit;
  EXPECT_EQ(explicitDirection->values->values.direction,glm::vec3(0,0,-1));
}
TEST(TemporalCapture, LightValuesRejectNonfiniteWrongTypesAndUnsupportedProperties) {
  for(const json value:{json(true),json(nullptr),json("1"),json::array(),json(std::numeric_limits<double>::infinity()),json(std::numeric_limits<double>::quiet_NaN()),json(std::numeric_limits<double>::max())}) {
    auto edit=lightCommit();edit["values"]={{"intensity",value}};EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
  }
  for(const json values:{json::object(),json{{"unknown",1}},json{{"color",{1,-1,0}}},json{{"range",-1}},json{{"intensity",-1}},json{{"direction",{0,0,0}}},
      json{{"spotCone",{30,20}}},json{{"areaSize",{2,0}}},json{{"areaShape","triangle"}}}) {
    auto edit=lightCommit();edit["values"]=values;EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument)<<values;
  }
  for(const auto& [type,key,value]:{std::tuple{"point","direction",json{0,0,-1}},std::tuple{"directional","position",json{1,2,3}},
      std::tuple{"area","spotCone",json{10,20}},std::tuple{"spot","areaSize",json{2,3}}}) {
    auto edit=lightCommit();edit["target"]=lightTarget(type);edit["values"]={{key,value}};
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",edit}})),std::invalid_argument);
  }
}
TEST(TemporalCapture, LightActionsAreIsolatedFromAllSceneHistoryTimelineAndLegacyLightMutations) {
  for(const auto& [action,request]:{std::tuple{"lightEdit",lightCommit()},std::tuple{"lightSelection",lightTarget()},std::tuple{"lightAdd",json{{"type","area"}}}}) {
    for(json event:std::vector<json>{{{"sceneHistory","undo"}},{{"animationTime",.5}},{{"animationPlaying",false}},{{"gltfScene",0}},
        {{"cameraPose",{{"sourceNode",0},{"fromViewport",true}}}},{{"areaLightPosition",{1,2,3}}},{{"guiUnload",false}},{{"objectVisible",false}},
        {{"materialEdit",materialPatch()}},{{"lightEdit",lightCommit()},{"lightAdd",{{"type","point"}}}},{{"unknown",false}}}) {
      event[action]=request;EXPECT_THROW((void)CaptureSequence(typedEditCapture(event)),std::invalid_argument)<<event;
    }
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{action,request},{"taa",false},{"exposure",.35},{"resize",{640,360}}})));
  }
}
TEST(TemporalCapture, ExpectedLightFailureRequiresExactlyOneTypedActionAndBoolean) {
  for(const bool flag:{false,true}) {
    EXPECT_NO_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",lightCommit()},{"expectLightEditFailure",flag}})));
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"expectLightEditFailure",flag}})),std::invalid_argument);
  }
  for(const json bad:{json(nullptr),json(1),json("true"),json::array()})
    EXPECT_THROW((void)CaptureSequence(typedEditCapture({{"lightEdit",lightCommit()},{"expectLightEditFailure",bad}})),std::invalid_argument);
}
