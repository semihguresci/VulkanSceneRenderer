#include "Container/robotics/RoboticsScene.h"
#include "Container/robotics/TwinIngest.h"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using namespace container::robotics;
using enum TwinIngestCode;
using enum TwinAssetReference;

using Fingerprint = container::util::ContentFingerprint;
constexpr std::string_view door = "bim/5:plant/guid/8:DoorGuid";
constexpr std::string_view sharedFirst = "bim/5:plant/occurrence/first";
constexpr std::string_view sharedSecond = "bim/5:plant/occurrence/second";
constexpr std::string_view proxy = "bim/5:plant/source/9:proxy-row";

std::vector<Fingerprint> fingerprints() {
  return {{std::string(64, 'a'), 150}, {std::string(64, 'b'), 70}};
}

RoboticsScene scene() {
  return {.sourceNamespace = "plant", .frame = {.metersPerRendererUnit = 0.5, .importScale = 2.0},
          .entities = {{.id = std::string(door), .guid = "DoorGuid"},
                       {.id = std::string(sharedFirst), .guid = "SharedGuid"},
                       {.id = std::string(sharedSecond), .guid = "SharedGuid"},
                       {.id = std::string(proxy)}}};
}

TwinIngestMessage message(uint64_t observed, uint64_t received, TwinPayload payload,
                          std::string_view entity = "robot/base", std::string_view source = "sim",
                          TwinAssetReference reference = EntityId, std::string_view clock = "sim-clock") {
  return {std::string(clock), received, reference,
          {observed, std::string(entity), std::string(source), std::move(payload)}};
}

TEST(TwinIngest, PreservesOrderedBimSourceEvidenceAndRequiresMetricUnits) {
  auto model = scene();
  TwinIngest ingest(model, fingerprints(), "sim-clock");
  const auto recording = ingest.recording();
  EXPECT_EQ(recording.sourceNamespace, "plant");
  EXPECT_EQ(recording.sceneSource.layers, fingerprints());
  EXPECT_DOUBLE_EQ(recording.sceneSource.metersPerUnscaledRendererUnit, 1);
  EXPECT_EQ(recording.clockId, "sim-clock");
  EXPECT_TRUE(recording.events.empty());
  EXPECT_TRUE(ingest.latest(0).tracks.empty());
  auto invalidFingerprints = fingerprints();
  invalidFingerprints[0].digest.front() = 'z';
  EXPECT_THROW(TwinIngest(model, invalidFingerprints, "sim-clock"), std::invalid_argument);
  EXPECT_THROW(TwinIngest(model, fingerprints(), "bad\nclock"), std::invalid_argument);
  model.entities.push_back(model.entities.front());
  EXPECT_THROW(TwinIngest(model, fingerprints(), "sim-clock"), std::invalid_argument);
  model.entities.pop_back();
  model.frame.metersPerRendererUnit.reset();
  EXPECT_THROW(TwinIngest(model, fingerprints(), "sim-clock"), std::invalid_argument);
}

TEST(TwinIngest, KeepsFreshOutOfOrderHistoryAndReturnsTheActualLatestObservation) {
  TwinIngest ingest(scene(), fingerprints(), "sim-clock", 25);
  const auto start = message(100, 100, TwinRobotPose{{1, 0, 0}, 0});
  const auto newest = message(120, 120, TwinRobotPose{{3, 0, 0}, 0});
  const auto delayed = message(110, 125, TwinRobotPose{{2, 0, 0}, 0});
  EXPECT_EQ(ingest.ingest(start).code, Accepted);
  EXPECT_EQ(ingest.ingest(newest).code, Accepted);
  const auto receipt = ingest.ingest(delayed);
  EXPECT_EQ(receipt.code, AcceptedOutOfOrder);
  EXPECT_EQ(receipt.entityId, "robot/base");
  EXPECT_FALSE(receipt.ifcGuid);
  EXPECT_FALSE(receipt.diagnostic.empty());
  auto retry = delayed;
  retry.receivedTimestampNanoseconds = 126;
  EXPECT_EQ(ingest.ingest(retry).code, Duplicate);
  retry.event.source = "different provenance";
  EXPECT_EQ(ingest.ingest(retry).code, Conflict);
  const auto recording = ingest.recording();
  EXPECT_EQ(recording.events, (std::vector{start.event, delayed.event, newest.event}));

  const auto earlier = ingest.latest(115);
  ASSERT_EQ(earlier.tracks.size(), 1u);
  EXPECT_EQ(earlier.timestampNanoseconds, 115u);
  EXPECT_EQ(earlier.tracks[0].event, delayed.event);
  EXPECT_TRUE(earlier.tracks[0].observedPath.empty());
  EXPECT_FALSE(earlier.tracks[0].distanceToPlanMeters);
  const auto fresh = ingest.latest(145);
  ASSERT_EQ(fresh.tracks.size(), 1u);
  EXPECT_EQ(fresh.tracks[0].event, newest.event);
  EXPECT_FALSE(fresh.tracks[0].stale);
  EXPECT_TRUE(ingest.latest(146).tracks.at(0).stale);
  EXPECT_TRUE(ingest.latest(99).tracks.empty());
}

TEST(TwinIngest, RejectsIncompatibleTimeAndAdvancesOnlyMatchingMonotonicReceipts) {
  TwinIngest ingest(scene(), fingerprints(), "sim-clock", 20);
  const TwinRobotHealth health{"ready", 75};
  const auto wrongClock = message(200, 200, health, "robot/base", "external", EntityId, "wall-clock");
  EXPECT_EQ(ingest.ingest(wrongClock).code, ClockMismatch);
  EXPECT_EQ(ingest.ingest(message(100, 100, health)).code, Accepted);
  EXPECT_EQ(ingest.ingest(message(121, 120, health)).code, FutureObservation);
  EXPECT_EQ(ingest.ingest(message(119, 119, health)).code, ReceiptTimeRegression);
  EXPECT_EQ(ingest.ingest(message(99, 120, health)).code, StaleObservation);
  EXPECT_EQ(ingest.ingest(message(100, 120, health)).code, Duplicate);
  EXPECT_EQ(ingest.ingest(message(100, 121, health)).code, StaleObservation);
  EXPECT_EQ(ingest.ingest(message(121, 121, TwinRobotHealth{"ready", -1})).code, InvalidEvent);
  EXPECT_EQ(ingest.ingest(message(120, 120, health)).code, ReceiptTimeRegression);
  EXPECT_EQ(ingest.recording().events.size(), 1u);
}

TEST(TwinIngest, AcceptsContactAndClearsItWithTheNextFullPose) {
  TwinIngest ingest(scene(), fingerprints(), "sim-clock", 15);
  const TwinRobotContact contact{std::string(door), {2, 3, 4}, glm::dvec3(0, -1, 0)};
  const auto measured = message(100, 100, TwinRobotPose{{1, 0, 1}, 0, contact});
  EXPECT_EQ(ingest.ingest(measured).code, Accepted);
  EXPECT_EQ(ingest.ingest(measured).code, Duplicate);
  const auto fresh = ingest.latest(115);
  ASSERT_EQ(fresh.tracks.size(), 1u);
  EXPECT_FALSE(fresh.tracks[0].stale);
  EXPECT_EQ(fresh.tracks[0].event, measured.event);
  const auto expired = ingest.latest(116);
  EXPECT_TRUE(expired.tracks.at(0).stale);
  EXPECT_EQ(expired.tracks.at(0).event, measured.event);

  const auto cleared = message(121, 121, TwinRobotPose{{1.1, 0, 1}, 0});
  EXPECT_EQ(ingest.ingest(cleared).code, Accepted);
  auto conflicting = cleared;
  std::get<TwinRobotPose>(conflicting.event.payload).contact = contact;
  EXPECT_EQ(ingest.ingest(conflicting).code, Conflict);
  auto invalid = message(122, 122, TwinRobotPose{{1, 0, 1}, 0, contact});
  std::get<TwinRobotPose>(invalid.event.payload).contact->normal = glm::dvec3(2, 0, 0);
  EXPECT_EQ(ingest.ingest(invalid).code, InvalidEvent);

  const auto latest = ingest.latest(122);
  ASSERT_EQ(latest.tracks.size(), 1u);
  EXPECT_EQ(latest.tracks[0].event, cleared.event);
  EXPECT_FALSE(std::get<TwinRobotPose>(latest.tracks[0].event.payload).contact);
  const auto canonical = parseTwinRecording(serializeTwinRecording(ingest.recording()));
  EXPECT_EQ(canonical.events, (std::vector{measured.event, cleared.event}));
}

TEST(TwinIngest, ResolvesCanonicalEntityAndUniqueIfcGuidWithoutChangingGeometry) {
  const auto model = scene();
  TwinIngest ingest(model, fingerprints(), "sim-clock");
  const auto accepted = ingest.ingest(message(0, 0, TwinAssetState{"closed"}, door));
  EXPECT_EQ(accepted.code, Accepted);
  EXPECT_EQ(accepted.entityId, door);
  EXPECT_EQ(accepted.ifcGuid, "DoorGuid");
  const auto alias = ingest.ingest(message(0, 1, TwinAssetState{"closed"}, "DoorGuid", "sim", IfcGuid));
  EXPECT_EQ(alias.code, Duplicate);
  EXPECT_EQ(alias.entityId, door);
  EXPECT_EQ(alias.ifcGuid, "DoorGuid");
  EXPECT_EQ(ingest.ingest(message(1, 1, TwinAssetState{"closed"}, "doorGuid", "sim", IfcGuid)).code,
            UnknownAsset);
  EXPECT_EQ(ingest.ingest(message(1, 1, TwinAssetState{"closed"}, "DoorGuid")).code, UnknownAsset);
  EXPECT_EQ(ingest.ingest(message(1, 1, TwinAssetState{"closed"}, "SharedGuid", "sim", IfcGuid)).code,
            AmbiguousAsset);
  for (const auto id : {sharedFirst, sharedSecond}) {
    const auto occurrence = ingest.ingest(message(1, 1, TwinAssetState{"inspected"}, id));
    EXPECT_EQ(occurrence.code, Accepted);
    EXPECT_EQ(occurrence.entityId, id);
    EXPECT_EQ(occurrence.ifcGuid, "SharedGuid");
  }
  const auto noGuid = ingest.ingest(message(2, 2, TwinAssetState{"operating"}, proxy));
  EXPECT_EQ(noGuid.code, Accepted);
  EXPECT_EQ(noGuid.entityId, proxy);
  EXPECT_FALSE(noGuid.ifcGuid);
  EXPECT_EQ(ingest.ingest(message(2, 2, TwinRobotHealth{"healthy", {}}, "DoorGuid", "sim", IfcGuid)).code,
            InvalidEvent);
  EXPECT_EQ(ingest.recording().events.front().entityId, door);
  EXPECT_EQ(ingest.recording().events.size(), 4u);
  EXPECT_EQ(model.entities.front().guid, "DoorGuid");
}

TEST(TwinIngest, SeparatesConcurrentTaskTracksAndKeepsFreshnessOnUnsignedTimeExtremes) {
  constexpr uint64_t last = std::numeric_limits<uint64_t>::max();
  TwinIngest ingest(scene(), fingerprints(), "sim-clock", 10);
  EXPECT_EQ(ingest.ingest(message(last - 1, last - 1, TwinTaskStatus{"b", "started", 0.0})).code, Accepted);
  EXPECT_EQ(ingest.ingest(message(last - 1, last - 1, TwinTaskStatus{"a", "started", 0.2})).code, Accepted);
  EXPECT_EQ(ingest.ingest(message(last - 3, last, TwinRobotHealth{"ready", {}})).code, Accepted);
  EXPECT_EQ(ingest.ingest(message(last - 2, last, TwinTaskStatus{"a", "queued", {}})).code, AcceptedOutOfOrder);
  EXPECT_EQ(ingest.ingest(message(last - 1, last, TwinTaskStatus{"c", "invalid", 1.2})).code, InvalidEvent);
  EXPECT_EQ(ingest.ingest(message(last - 1, last, TwinTaskStatus{"b", "started", 0.0})).code, Duplicate);
  EXPECT_EQ(ingest.ingest(message(0, last, TwinTaskStatus{"b", "started", 0.0})).code, StaleObservation);
  const auto current = ingest.latest(last);
  ASSERT_EQ(current.tracks.size(), 3u);
  EXPECT_EQ(std::get<TwinTaskStatus>(current.tracks[1].event.payload).taskId, "a");
  EXPECT_EQ(std::get<TwinTaskStatus>(current.tracks[1].event.payload).state, "started");
  EXPECT_EQ(std::get<TwinTaskStatus>(current.tracks[2].event.payload).taskId, "b");
  EXPECT_FALSE(current.tracks[0].stale);
  const auto recording = ingest.recording();
  EXPECT_EQ(std::get<TwinTaskStatus>(recording.events[2].payload).taskId, "a");
  EXPECT_EQ(std::get<TwinTaskStatus>(recording.events[3].payload).taskId, "b");
  EXPECT_EQ(TwinReplay(recording).sample(last).tracks.size(), current.tracks.size());
}

TEST(TwinIngest, EnforcesSharedRecordingLimitsWithoutLosingAcceptedHistory) {
  TwinIngest plans(scene(), fingerprints(), "sim-clock");
  const TwinPlannedPath path{std::vector<glm::dvec3>(4096, glm::dvec3(0))};
  for (uint64_t time = 0; time < 64; ++time)
    ASSERT_EQ(plans.ingest(message(time, time, path)).code, Accepted);
  EXPECT_EQ(plans.ingest(message(64, 64, path)).code, LimitExceeded);
  EXPECT_EQ(plans.ingest(message(63, 64, path)).code, Duplicate);
  EXPECT_EQ(plans.recording().events.size(), 64u);

  TwinIngest observations(scene(), fingerprints(), "sim-clock");
  const TwinRobotHealth health{"ready", 50};
  for (uint64_t time = 0; time < 65'536; ++time)
    ASSERT_EQ(observations.ingest(message(time, time, health)).code, Accepted);
  EXPECT_EQ(observations.ingest(message(65'536, 65'536, health)).code, LimitExceeded);
  EXPECT_EQ(observations.ingest(message(65'535, 65'536, health)).code, Duplicate);
  EXPECT_EQ(observations.recording().events.size(), 65'536u);
  EXPECT_EQ(observations.latest(65'536).tracks.at(0).event.timestampNanoseconds, 65'535u);
}

TEST(TwinIngest, RetainsIndependentAlarmTracksThroughClearAndConflict) {
  TwinIngest ingest(scene(), fingerprints(), "sim-clock", 5);
  const TwinOperatorAlarm doorAlarm{"door", "warning", "Obstructed", true, {1, 2, 3}};
  const TwinOperatorAlarm motor{"motor", "critical", "Stalled", true, {4, 5, 6}};
  auto doorClear = doorAlarm;
  doorClear.active = false;
  EXPECT_EQ(ingest.ingest(message(10, 10, motor)).code, Accepted);
  EXPECT_EQ(ingest.ingest(message(10, 10, doorAlarm)).code, Accepted);
  const auto clear = message(13, 13, doorClear);
  EXPECT_EQ(ingest.ingest(clear).code, Accepted);
  EXPECT_EQ(ingest.ingest(clear).code, Duplicate);
  EXPECT_EQ(ingest.ingest(message(13, 13, doorAlarm)).code, Conflict);
  const auto latest = ingest.latest(16);
  ASSERT_EQ(latest.tracks.size(), 2u);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(latest.tracks[0].event.payload), doorClear);
  EXPECT_FALSE(latest.tracks[0].stale);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(latest.tracks[1].event.payload), motor);
  EXPECT_TRUE(latest.tracks[1].stale);
  const auto recorded = ingest.recording();
  ASSERT_EQ(recorded.events.size(), 3u);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(recorded.events[0].payload), doorAlarm);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(recorded.events[1].payload), motor);
  EXPECT_EQ(std::get<TwinOperatorAlarm>(recorded.events[2].payload), doorClear);
}

TEST(TwinIngest, CountsOnlyAcceptedOccupancyCellsAgainstItsOwnBudget) {
  TwinIngest ingest(scene(), fingerprints(), "sim-clock");
  const TwinOccupancyGrid grid{.resolutionMeters = 1, .width = 128, .height = 128,
                               .cells = std::string(128 * 128, '.')};
  auto malformed = grid;
  malformed.cells[0] = 'x';
  EXPECT_EQ(ingest.ingest(message(0, 0, malformed, "floor/map")).code, InvalidEvent);
  for (uint64_t time = 0; time < 64; ++time)
    ASSERT_EQ(ingest.ingest(message(time, time, grid, "floor/map")).code, Accepted);
  EXPECT_EQ(ingest.ingest(message(64, 64, grid, "floor/map")).code, LimitExceeded);
  EXPECT_EQ(ingest.ingest(message(63, 64, grid, "floor/map")).code, Duplicate);
  EXPECT_EQ(ingest.ingest(message(64, 64, TwinPlannedPath{{glm::dvec3(0)}})).code, Accepted);
  EXPECT_EQ(ingest.recording().events.size(), 65u);
  const auto latest = ingest.latest(64);
  EXPECT_EQ(latest.tracks[0].event.timestampNanoseconds, 63u);
  EXPECT_EQ(std::get<TwinOccupancyGrid>(latest.tracks[0].event.payload), grid);
}

} // namespace
