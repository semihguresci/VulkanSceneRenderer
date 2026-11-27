#include "Container/robotics/TwinTelemetry.h"

#include <gtest/gtest.h>

#include <algorithm>
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

} // namespace
