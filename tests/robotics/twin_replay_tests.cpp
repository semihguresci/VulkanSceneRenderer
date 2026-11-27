#include "Container/robotics/TwinTelemetry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {

using namespace container::robotics;
constexpr double pi = std::numbers::pi;

TwinRecording recording(std::vector<TwinEvent> events, uint64_t freshness = 20) {
  return {.sourceNamespace = "north wing", .sceneSource = {{{std::string(64, 'a'), 2'441}}, .001},
          .clockId = "fixed-step", .staleAfterNanoseconds = freshness, .events = std::move(events)};
}

TwinEvent pose(uint64_t time, glm::dvec3 position, double yaw = 0,
               std::string id = "robot/base", std::string source = "simulator") {
  return {time, std::move(id), std::move(source), TwinRobotPose{position, yaw}};
}

TwinSensorFrustum inspectionFrustum() {
  return {.width = 8, .height = 6, .fx = 4, .fy = 3, .cx = 2, .cy = 1.5, .nearMeters = 2, .farMeters = 4,
          .worldFromCamera = glm::dmat4(glm::dvec4(0, -1, 0, 0), glm::dvec4(0, 0, 1, 0),
                                      glm::dvec4(-1, 0, 0, 0), glm::dvec4(10, 20, 30, 1))};
}

template<class Payload>
const TwinTrackSample& track(const TwinReplaySample& sample, std::string_view entity) {
  const auto value = std::ranges::find_if(sample.tracks, [entity](const auto& candidate) {
    return candidate.event.entityId == entity && std::holds_alternative<Payload>(candidate.event.payload);
  });
  if (value == sample.tracks.end()) throw std::logic_error("Test expected a sampled twin track");
  return *value;
}

TEST(TwinReplay, OrdersOutOfOrderObservationsByTimestampThenIdentityAndKind) {
  auto log = recording({pose(41, {2, 3, 4}),
                        {10, "plant/valve", "control", TwinAssetState{"closed"}},
                        {10, "plant/valve", "sensor-gateway", TwinSensorStatus{"online"}},
                        pose(10, {1, 2, 3})});
  const TwinReplay replay(log);
  EXPECT_EQ(replay.firstTimestamp(), 10u);
  EXPECT_EQ(replay.lastTimestamp(), 41u);
  const auto& events = replay.recording().events;
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(events[0].entityId, "plant/valve");
  EXPECT_EQ(events[0].timestampNanoseconds, 10u);
  EXPECT_TRUE(std::holds_alternative<TwinSensorStatus>(events[0].payload));
  EXPECT_TRUE(std::holds_alternative<TwinAssetState>(events[1].payload));
  EXPECT_EQ(events[2].entityId, "robot/base");
  EXPECT_EQ(events[2].timestampNanoseconds, 10u);
  EXPECT_EQ(events[3].timestampNanoseconds, 41u);
  EXPECT_TRUE(replay.sample(9).tracks.empty());
  const auto initial = replay.sample(10);
  ASSERT_EQ(initial.tracks.size(), 3u);
  EXPECT_EQ(track<TwinAssetState>(initial, "plant/valve").event.source, "control");
  EXPECT_EQ(std::get<TwinSensorStatus>(track<TwinSensorStatus>(initial, "plant/valve").event.payload).state,
            "online");

  log.events.push_back(pose(10, {1, 2, 3}, 0, "robot/base", "external"));
  EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);
}

TEST(TwinReplay, HoldsEachTaskIndependentlyAlongsideRobotHealth) {
  auto log = recording({
      {10, "robot/base", "dispatcher", TwinTaskStatus{"unload", "waiting", std::nullopt}},
      {20, "robot/base", "dispatcher", TwinTaskStatus{"load", "finished", 1.0}},
      {10, "robot/base", "monitor", TwinRobotHealth{"charging", 0.0}},
      {10, "robot/base", "dispatcher", TwinTaskStatus{"load", "running", 0.0}},
      {35, "robot/base", "monitor", TwinRobotHealth{"ready", 100.0}},
  }, 12);
  const TwinReplay replay(log);
  const auto& ordered = replay.recording().events;
  EXPECT_TRUE(std::holds_alternative<TwinRobotHealth>(ordered[0].payload));
  EXPECT_EQ(std::get<TwinTaskStatus>(ordered[1].payload).taskId, "load");
  EXPECT_EQ(std::get<TwinTaskStatus>(ordered[2].payload).taskId, "unload");

  const auto initial = replay.sample(10);
  ASSERT_EQ(initial.tracks.size(), 3u);
  EXPECT_EQ(std::get<TwinRobotHealth>(initial.tracks[0].event.payload).batteryPercent, 0.0);
  EXPECT_EQ(std::get<TwinTaskStatus>(initial.tracks[1].event.payload).state, "running");
  EXPECT_FALSE(std::get<TwinTaskStatus>(initial.tracks[2].event.payload).progress.has_value());

  const auto later = replay.sample(25);
  ASSERT_EQ(later.tracks.size(), 3u);
  EXPECT_TRUE(later.tracks[0].stale);
  EXPECT_EQ(std::get<TwinTaskStatus>(later.tracks[1].event.payload),
            (TwinTaskStatus{"load", "finished", 1.0}));
  EXPECT_FALSE(later.tracks[1].stale);
  EXPECT_EQ(std::get<TwinTaskStatus>(later.tracks[2].event.payload),
            (TwinTaskStatus{"unload", "waiting", std::nullopt}));
  EXPECT_TRUE(later.tracks[2].stale);
  EXPECT_EQ(later.tracks[2].event.timestampNanoseconds, 10u);
  const auto recharged = replay.sample(35);
  EXPECT_EQ(std::get<TwinRobotHealth>(recharged.tracks[0].event.payload).batteryPercent, 100.0);

  log.events.push_back({10, "robot/base", "other source", TwinTaskStatus{"load", "blocked", 0.0}});
  EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);
}

TEST(TwinReplay, InterpolatesShortFreshGapsByTheShortestYawArcAndKeepsSampleProvenance) {
  const TwinReplay replay(recording({pose(100, {1, 2, 3}, 170 * pi / 180, "robot/base", "simulator"),
                                     pose(110, {3, 6, 9}, -170 * pi / 180, "robot/base", "logger"),
                                     pose(210, {7, 8, 9})}));
  const auto midpoint = replay.sample(105);
  const auto& interpolated = track<TwinRobotPose>(midpoint, "robot/base");
  const auto& reading = std::get<TwinRobotPose>(interpolated.event.payload);
  EXPECT_EQ(midpoint.timestampNanoseconds, 105u);
  EXPECT_EQ(reading.position, glm::dvec3(2, 4, 6));
  EXPECT_NEAR(std::abs(reading.yaw), pi, 1e-12);
  EXPECT_FALSE(interpolated.stale);
  EXPECT_EQ(interpolated.event.timestampNanoseconds, 100u);
  EXPECT_EQ(interpolated.event.source, "simulator");
  EXPECT_EQ(interpolated.observedPath, (std::vector<glm::dvec3>{{1, 2, 3}}));

  const auto held = replay.sample(130);
  const auto& latest = track<TwinRobotPose>(held, "robot/base");
  EXPECT_FALSE(latest.stale);
  EXPECT_EQ(latest.event.timestampNanoseconds, 110u);
  EXPECT_EQ(std::get<TwinRobotPose>(latest.event.payload).position, glm::dvec3(3, 6, 9));
  EXPECT_EQ(latest.observedPath, (std::vector<glm::dvec3>{{1, 2, 3}, {3, 6, 9}}));
  EXPECT_TRUE(track<TwinRobotPose>(replay.sample(131), "robot/base").stale);
  EXPECT_TRUE(track<TwinRobotPose>(replay.sample(231), "robot/base").stale);
  EXPECT_FALSE(track<TwinRobotPose>(replay.sample(210), "robot/base").stale);
  const auto scrubbedBack = replay.sample(105);
  const auto& repeated = track<TwinRobotPose>(scrubbedBack, "robot/base");
  EXPECT_EQ(std::get<TwinRobotPose>(repeated.event.payload).position, reading.position);
  EXPECT_EQ(repeated.observedPath, interpolated.observedPath);
}

TEST(TwinReplay, HandlesFreshnessZeroAndUnsignedTimestampExtremes) {
  constexpr auto end = std::numeric_limits<uint64_t>::max();
  const auto log = recording({pose(end, {4, 0, 0}), pose(end - 4, {0, 0, 0})}, 4);
  const TwinReplay replay(log);
  EXPECT_TRUE(replay.sample(end - 5).tracks.empty());
  const auto midpoint = replay.sample(end - 2);
  EXPECT_EQ(std::get<TwinRobotPose>(track<TwinRobotPose>(midpoint, "robot/base").event.payload).position,
            glm::dvec3(2, 0, 0));
  auto freshOnly = log;
  freshOnly.staleAfterNanoseconds = 0;
  const TwinReplay strict(std::move(freshOnly));
  EXPECT_FALSE(track<TwinRobotPose>(strict.sample(end - 4), "robot/base").stale);
  const auto held = strict.sample(end - 2);
  EXPECT_TRUE(track<TwinRobotPose>(held, "robot/base").stale);
  EXPECT_EQ(std::get<TwinRobotPose>(track<TwinRobotPose>(held, "robot/base").event.payload).position,
            glm::dvec3(0));
  EXPECT_FALSE(track<TwinRobotPose>(strict.sample(end), "robot/base").stale);
}

TEST(TwinReplay, HoldsMeasuredContactThroughPoseInterpolationUntilClearedOrStale) {
  const TwinRobotContact first{"bim/wall", {3, 4, 5}, glm::dvec3(-1, 0, 0)};
  const TwinRobotContact second{"bim/door", {6, 7, 8}};
  const TwinReplay replay(recording({pose(100, {0, 0, 0}),
      {110, "robot/base", "simulator", TwinRobotPose{{2, 0, 0}, 0, first}},
      {120, "robot/base", "simulator", TwinRobotPose{{4, 0, 0}, 0, second}},
      pose(130, {6, 0, 0}),
      {160, "robot/base", "simulator", TwinRobotPose{{7, 0, 0}, 0, first}}}));

  const auto before = replay.sample(105);
  const auto& approaching = std::get<TwinRobotPose>(track<TwinRobotPose>(before, "robot/base").event.payload);
  EXPECT_EQ(approaching.position, glm::dvec3(1, 0, 0));
  EXPECT_FALSE(approaching.contact);

  const auto between = replay.sample(115);
  const auto& held = track<TwinRobotPose>(between, "robot/base");
  const auto& measured = std::get<TwinRobotPose>(held.event.payload);
  EXPECT_FALSE(held.stale);
  EXPECT_EQ(held.event.timestampNanoseconds, 110u);
  EXPECT_EQ(measured.position, glm::dvec3(3, 0, 0));
  EXPECT_EQ(measured.contact, std::optional{first});

  const auto later = replay.sample(120);
  EXPECT_EQ(std::get<TwinRobotPose>(track<TwinRobotPose>(later, "robot/base").event.payload).contact,
            std::optional{second});
  const auto cleared = replay.sample(130);
  EXPECT_FALSE(std::get<TwinRobotPose>(track<TwinRobotPose>(cleared, "robot/base").event.payload).contact);
  EXPECT_FALSE(track<TwinRobotPose>(replay.sample(180), "robot/base").stale);
  const auto expired = replay.sample(181);
  const auto& stale = track<TwinRobotPose>(expired, "robot/base");
  EXPECT_TRUE(stale.stale);
  EXPECT_EQ(std::get<TwinRobotPose>(stale.event.payload).contact, std::optional{first});
}

TEST(TwinRecording, RoundTripsContactAndKeepsTheHighestRequiredVersion) {
  auto log = recording({pose(5, {1, 2, 3}, .25)});
  const auto checkVersion = [](const TwinRecording& input, std::string_view version) {
    const auto json = serializeTwinRecording(input);
    EXPECT_NE(json.find(version), std::string::npos);
    const auto decoded = parseTwinRecording(json);
    EXPECT_EQ(decoded.events, input.events);
    EXPECT_EQ(serializeTwinRecording(decoded), json);
    return json;
  };
  EXPECT_EQ(checkVersion(log, R"("version":1)").find(R"("contact")"), std::string::npos);
  log.events.push_back({6, "robot/base", "simulator", TwinRobotHealth{"active", 75}});
  checkVersion(log, R"("version":2)");
  log.events.push_back({7, "robot/base", "simulator", TwinOperatorAlarm{"halt", "warning", "Contact", true}});
  log.events.push_back({8, "robot/base", "simulator", TwinRobotHealth{"active", 74}});
  checkVersion(log, R"("version":3)");

  const TwinRobotContact contact{"bim/wall", {3, 4, 5}, glm::dvec3(0, 0, 1.00000005)};
  log.events.push_back({9, "robot/base", "simulator", TwinRobotPose{{2, 3, 4}, .25, contact}});
  log.events.push_back({10, "robot/base", "simulator", TwinRobotPose{{2, 3, 4}, .25,
      TwinRobotContact{"bim/door", {3, 4, 5}}}});
  log.events.push_back(pose(11, {3, 4, 5}));
  auto json = checkVersion(log, R"("version":4)");
  EXPECT_NE(json.find(R"("otherEntityId":"bim/wall")"), std::string::npos);
  EXPECT_NE(json.find(R"("positionMeters":[3.0,4.0,5.0])"), std::string::npos);
  const auto version = json.find(R"("version":4)");
  ASSERT_NE(version, std::string::npos);
  json[version + std::string_view(R"("version":)").size()] = '3';
  EXPECT_THROW((void)parseTwinRecording(json), std::invalid_argument);
}

TEST(TwinRecording, RejectsMalformedContactObjectsAndInvalidMetrics) {
  const TwinRobotContact valid{"bim/wall", {3, 4, 5}, glm::dvec3(-1, 0, 0)};
  const auto log = recording({{17, "robot/base", "simulator", TwinRobotPose{{}, 0, valid}}});
  const auto canonical = serializeTwinRecording(log);
  const auto rejectJson = [&](std::string_view key, std::string_view replacement) {
    auto invalidJson = canonical;
    const auto field = invalidJson.find(key);
    ASSERT_NE(field, std::string::npos);
    invalidJson.replace(field, key.size(), replacement);
    EXPECT_THROW((void)parseTwinRecording(invalidJson), std::invalid_argument);
  };
  rejectJson(R"("otherEntityId")", R"("unknown")");
  rejectJson(R"("positionMeters":[3.0,4.0,5.0])", R"("positionMeters":[3.0,4.0])");
  rejectJson(R"("normal":[-1.0,0.0,0.0])", R"("normal":null)");

  const auto rejectContact = [&](const TwinRobotContact& contact) {
    auto malformed = log;
    std::get<TwinRobotPose>(malformed.events[0].payload).contact = contact;
    EXPECT_THROW(canonicalizeTwinRecording(malformed), std::invalid_argument);
  };
  rejectContact({"", valid.position, valid.normal});
  rejectContact({std::string(1025, 'a'), valid.position, valid.normal});
  rejectContact({"wall\n", valid.position, valid.normal});
  rejectContact({valid.otherEntityId, {1e9, 0, 0}, valid.normal});
  rejectContact({valid.otherEntityId, {0, std::numeric_limits<double>::infinity(), 0}, valid.normal});
  rejectContact({valid.otherEntityId, valid.position, glm::dvec3(0)});
  rejectContact({valid.otherEntityId, valid.position, glm::dvec3(2, 0, 0)});
  rejectContact({valid.otherEntityId, valid.position, glm::dvec3(std::numeric_limits<double>::quiet_NaN(), 0, 0)});
}

TEST(TwinReplay, ComparesTheCurrentPositionAgainstOnlyAFreshMatchingPlan) {
  const TwinReplay replay(recording({
      pose(10, {2, 3, 4}), {10, "robot/base", "planner", TwinPlannedPath{{{0, 0, 0}, {4, 0, 0}, {4, 0, 0}}}},
      {10, "robot/other", "planner", TwinPlannedPath{{{2, 3, 4}}}},
      pose(25, {10, 8, 0}), {31, "robot/base", "planner", TwinPlannedPath{}}, pose(31, {10, 8, 0}),
      {42, "robot/base", "planner", TwinPlannedPath{{{2, 3, 9}}}}, pose(42, {2, 3, 4}),
      {55, "robot/base", "planner", TwinPlannedPath{{{2, 3, 4}}}},
  }, 10));
  EXPECT_EQ(track<TwinRobotPose>(replay.sample(10), "robot/base").distanceToPlanMeters, 5.0);
  EXPECT_FALSE(track<TwinRobotPose>(replay.sample(25), "robot/base").distanceToPlanMeters);
  EXPECT_FALSE(track<TwinRobotPose>(replay.sample(31), "robot/base").distanceToPlanMeters);
  EXPECT_EQ(track<TwinRobotPose>(replay.sample(42), "robot/base").distanceToPlanMeters, 5.0);
  EXPECT_FALSE(track<TwinRobotPose>(replay.sample(55), "robot/base").distanceToPlanMeters);
}

TEST(TwinReplay, KeepsTheLast2048ObservedPointsInTimestampOrder) {
  auto log = recording({pose(0, {0, 0, 0})});
  for (uint64_t tick = 1; tick <= 2'050; ++tick) log.events.push_back(pose(tick, {double(tick), 0, 0}));
  const TwinReplay replay(std::move(log));
  const auto latest = replay.sample(2'050);
  const auto& trail = track<TwinRobotPose>(latest, "robot/base").observedPath;
  ASSERT_EQ(trail.size(), 2'048u);
  EXPECT_EQ(trail.front().x, 3);
  EXPECT_EQ(trail.back().x, 2'050);
  const auto earlier = replay.sample(1);
  const auto& shortTrail = track<TwinRobotPose>(earlier, "robot/base").observedPath;
  EXPECT_EQ(shortTrail, (std::vector<glm::dvec3>{{0, 0, 0}, {1, 0, 0}}));
}

TEST(TwinReplay, RejectsMissingProvenanceAndInvalidMetricObservations) {
  const auto valid = recording({pose(5, {0, 1, 2})});
  const auto reject = [&](auto change) {
    auto malformed = valid;
    change(malformed);
    EXPECT_THROW(canonicalizeTwinRecording(malformed), std::invalid_argument);
  };
  reject([](auto& log) { log.sourceNamespace.clear(); });
  reject([](auto& log) { log.clockId = "   "; });
  reject([](auto& log) { log.sceneSource.layers.clear(); });
  reject([](auto& log) { log.sceneSource.layers[0].digest[0] = 'A'; });
  reject([](auto& log) { log.sceneSource.layers[0].byteSize = 0; });
  reject([](auto& log) { log.sceneSource.metersPerUnscaledRendererUnit = -1; });
  reject([](auto& log) { log.sceneSource.metersPerUnscaledRendererUnit = std::numeric_limits<double>::infinity(); });
  reject([](auto& log) { log.events.clear(); });
  reject([](auto& log) { log.events[0].entityId.resize(1025, 'a'); });
  reject([](auto& log) { log.events[0].source = "packet\nwithout provenance"; });
  reject([](auto& log) { log.events[0].payload = TwinSensorStatus{""}; });
  reject([](auto& log) { log.events[0].payload = TwinAssetState{std::string(257, 'x')}; });
  reject([](auto& log) { log.events[0].payload = TwinRobotPose{{0, 0, 1e9}, 0}; });
  reject([](auto& log) { log.events[0].payload = TwinRobotPose{{0, 0, 0}, std::numeric_limits<double>::quiet_NaN()}; });
  reject([](auto& log) { log.events[0].payload = TwinPlannedPath{{{0, -std::numeric_limits<double>::infinity(), 0}}}; });
  reject([](auto& log) { log.events[0].payload = TwinPlannedPath{std::vector<glm::dvec3>(4'097, glm::dvec3(0))}; });
}

TEST(TwinReplay, ValidatesHealthAndTaskIdentifiersStatesAndNumericRanges) {
  constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
  constexpr auto inf = std::numeric_limits<double>::infinity();
  for (const auto& event : std::vector<TwinPayload>{
           TwinRobotHealth{"ready", -1.0}, TwinRobotHealth{"ready", 100.01}, TwinRobotHealth{"ready", nan},
           TwinRobotHealth{"", std::nullopt}, TwinTaskStatus{"", "running", 0.5},
           TwinTaskStatus{"task\ncontrol", "running", 0.5}, TwinTaskStatus{"task", "", 0.5},
           TwinTaskStatus{"task", "running", -0.01}, TwinTaskStatus{"task", "running", 1.01},
           TwinTaskStatus{"task", "running", inf},
       }) {
    auto input = recording({{1, "robot/base", "observer", event}});
    EXPECT_THROW(canonicalizeTwinRecording(input), std::invalid_argument);
  }
  auto valid = recording({{1, "robot/base", "observer", TwinRobotHealth{"ready", std::nullopt}},
                          {1, "robot/base", "observer", TwinTaskStatus{"task", "running", 0.5}}});
  EXPECT_NO_THROW(canonicalizeTwinRecording(valid));
}

TEST(TwinReplay, BoundsTheTapeAndAggregatePlanGeometry) {
  auto log = recording({pose(1, {0, 0, 0})});
  log.events.resize(65'537);
  EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);

  log = recording({pose(1, {0, 0, 0})});
  const auto layer = log.sceneSource.layers.front();
  log.sceneSource.layers.resize(257, layer);
  EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);

  log = recording({pose(1, {0, 0, 0})});
  for (uint64_t tick = 0; tick < 64; ++tick)
    log.events.push_back({tick, "robot/base", "planner", TwinPlannedPath{std::vector<glm::dvec3>(4'096, glm::dvec3(0))}});
  EXPECT_NO_THROW(canonicalizeTwinRecording(log));
  log.events.push_back({64, "robot/base", "planner", TwinPlannedPath{{glm::dvec3(0)}}});
  EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);
}

TEST(TwinReplay, ProjectsUncenteredSensorImageEdgesIntoRep103) {
  const auto camera = inspectionFrustum();
  const std::array<glm::dvec3, 8> expected{{
      {12, 21, 31}, {12, 17, 31}, {12, 17, 27}, {12, 21, 27},
      {14, 22, 32}, {14, 14, 32}, {14, 14, 24}, {14, 22, 24},
  }};
  EXPECT_EQ(twinFrustumCorners(camera), expected);
  const TwinReplay replay(recording({{5, "robot/rgbd", "camera", camera}}));
  EXPECT_TRUE(replay.sample(4).tracks.empty());
  const auto sample = replay.sample(10);
  EXPECT_EQ(std::get<TwinSensorFrustum>(track<TwinSensorFrustum>(sample, "robot/rgbd").event.payload), camera);
}

TEST(TwinReplay, RejectsInvalidSensorIntrinsicsAndNonRigidOrUnboundedCameraTransforms) {
  const auto reject = [](auto change) {
    auto invalid = inspectionFrustum();
    change(invalid);
    EXPECT_THROW((void)twinFrustumCorners(invalid), std::invalid_argument);
    auto log = recording({{1, "camera", "source", invalid}});
    EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);
  };
  reject([](auto& camera) { camera.width = 0; });
  reject([](auto& camera) { camera.width = camera.height = 8192; });
  reject([](auto& camera) { camera.fx = 0; });
  reject([](auto& camera) { camera.fy = std::numeric_limits<double>::infinity(); });
  reject([](auto& camera) { camera.cx = std::numeric_limits<double>::quiet_NaN(); });
  reject([](auto& camera) { camera.farMeters = camera.nearMeters; });
  reject([](auto& camera) { camera.nearMeters = -1; });
  reject([](auto& camera) { camera.worldFromCamera[0] *= 2.0; });
  reject([](auto& camera) { camera.worldFromCamera[0] *= -1.0; });
  reject([](auto& camera) { camera.worldFromCamera[1][3] = .01; });
  reject([](auto& camera) { camera.worldFromCamera[3][0] = std::numeric_limits<double>::infinity(); });
  reject([](auto& camera) { camera.worldFromCamera[3][0] = 100'000'000; });
  reject([](auto& camera) { camera.farMeters = 500'000'000; });
}

TEST(TwinReplay, RetainsIndependentAlarmsAndTheirClearEvents) {
  const auto doorActive = TwinOperatorAlarm{"door", "warning", "Door obstructed", true, {3, 2, 1}};
  auto doorClear = doorActive;
  doorClear.active = false;
  const auto motor = TwinOperatorAlarm{"motor", "critical", "Motor fault", true, {4, 2, 1}};
  const auto wifi = TwinOperatorAlarm{"wifi", "info", "Radio reconnecting", true, {5, 2, 1}};
  auto log = recording({{13, "robot/base", "controller", wifi},
                        {10, "robot/base", "controller", motor},
                        {10, "robot/base", "controller", doorActive},
                        {13, "robot/base", "controller", doorClear}}, 10);
  const TwinReplay replay(log);
  const auto beforeClear = replay.sample(12);
  ASSERT_EQ(beforeClear.tracks.size(), 2u);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(beforeClear.tracks[0].event.payload), doorActive);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(beforeClear.tracks[1].event.payload), motor);
  const auto afterClear = replay.sample(22);
  ASSERT_EQ(afterClear.tracks.size(), 3u);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(afterClear.tracks[0].event.payload), doorClear);
  EXPECT_FALSE(afterClear.tracks[0].stale);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(afterClear.tracks[1].event.payload), motor);
  EXPECT_TRUE(afterClear.tracks[1].stale);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(afterClear.tracks[2].event.payload), wifi);
  EXPECT_FALSE(afterClear.tracks[2].stale);

  log.events.push_back({13, "robot/base", "other source", doorActive});
  EXPECT_THROW(canonicalizeTwinRecording(log), std::invalid_argument);
  const auto reject = [&](auto change) {
    auto invalid = motor;
    change(invalid);
    auto bad = recording({{1, "robot/base", "source", invalid}});
    EXPECT_THROW(canonicalizeTwinRecording(bad), std::invalid_argument);
  };
  reject([](auto& alarm) { alarm.alarmId.clear(); });
  reject([](auto& alarm) { alarm.severity = "urgent"; });
  reject([](auto& alarm) { alarm.message = "unexpected\nnewline"; });
  reject([](auto& alarm) { alarm.position.y = std::numeric_limits<double>::quiet_NaN(); });
}

TEST(TwinReplay, KeepsOccupancyGridSnapshotsAndValidatesCellsAndBudgets) {
  const TwinOccupancyGrid grid{.origin = {10, 5, 2}, .yaw = pi / 2, .resolutionMeters = .25,
                               .width = 3, .height = 2, .cells = ".#??#."};
  auto changed = grid;
  changed.cells = "###...";
  auto onTape = recording({{4, "level/map", "sim", grid}, {6, "level/map", "sim", changed}});
  const TwinReplay replay(onTape);
  const auto held = replay.sample(5);
  EXPECT_EQ(std::get<TwinOccupancyGrid>(track<TwinOccupancyGrid>(held, "level/map").event.payload), grid);
  const auto stale = replay.sample(27);
  EXPECT_EQ(std::get<TwinOccupancyGrid>(track<TwinOccupancyGrid>(stale, "level/map").event.payload), changed);
  EXPECT_TRUE(track<TwinOccupancyGrid>(stale, "level/map").stale);

  const auto reject = [&](auto change) {
    auto invalid = grid;
    change(invalid);
    auto bad = recording({{1, "map", "source", invalid}});
    EXPECT_THROW(canonicalizeTwinRecording(bad), std::invalid_argument);
  };
  reject([](auto& map) { map.width = 0; });
  reject([](auto& map) { map.height = 129; });
  reject([](auto& map) { map.cells[0] = 'x'; });
  reject([](auto& map) { map.cells.pop_back(); });
  reject([](auto& map) { map.resolutionMeters = 0; });
  reject([](auto& map) { map.resolutionMeters = std::numeric_limits<double>::infinity(); });
  reject([](auto& map) { map.yaw = std::numeric_limits<double>::quiet_NaN(); });
  reject([](auto& map) { map.origin.x = 100'000'001; });
  reject([](auto& map) { map.origin.y = 100'000'000; });

  const TwinOccupancyGrid full{.resolutionMeters = 1, .width = 128, .height = 128, .cells = std::string(128 * 128, '?')};
  onTape.events.clear();
  for (uint64_t time = 0; time < 64; ++time)
    onTape.events.push_back({time, "level/map", "sim", full});
  EXPECT_NO_THROW(canonicalizeTwinRecording(onTape));
  onTape.events.push_back({64, "level/map", "sim", grid});
  EXPECT_THROW(canonicalizeTwinRecording(onTape), std::invalid_argument);
}

} // namespace
