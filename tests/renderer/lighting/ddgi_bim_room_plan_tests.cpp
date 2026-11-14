#include "Container/renderer/lighting/DdgiBimRoomPlan.h"
#include "Container/renderer/lighting/DdgiVolumeSet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

namespace container::renderer {
namespace {

using Status = DdgiBimRoomCoverage::Status;

DdgiBimRoom Room(std::string identity, std::string storey,
                 glm::vec3 minimum, glm::vec3 maximum) {
  return {.identity = identity, .label = identity, .storey = storey,
          .bounds = {minimum, maximum, true}};
}

const DdgiBimRoomCoverage& FindRoom(const DdgiBimRoomPlan& plan, std::string_view name) {
  return *std::ranges::find(plan.rooms, name, &DdgiBimRoomCoverage::identity);
}

void ExpectFullBlendCoverage(const DdgiBimRoomPlan& plan, const DdgiBimRoomCoverage& room) {
  ASSERT_TRUE(room.volumeIndex.has_value());
  const DdgiVolumeSet volumes(plan.settings);
  const auto& desc = volumes.volume(*room.volumeIndex).description();
  EXPECT_LT(desc.spacing.x, plan.settings.volume.spacing.x);
  for (size_t corner = 0; corner < 8; ++corner) {
    auto point = room.bounds.minimum;
    for (int axis = 0; axis < 3; ++axis) {
      if (corner & (size_t{1} << axis)) point[axis] = room.bounds.maximum[axis];
    }
    EXPECT_NEAR(volumes.blend(point).weights[*room.volumeIndex], 1.f, 1e-5f);
  }
}

TEST(DdgiBimRoomPlan, MergesCompactRoomsButSeparatesStoreysAndIsDeterministic) {
  DdgiSettings base;
  base.volume.spacing = glm::vec3(1.5f);
  base.volumeBlendCells = 1.25f;
  const auto before = base;
  auto rooms = std::array{
      Room("a", "01", {-1.5f, 0.f, -1.f}, {-.1f, 1.5f, 1.f}),
      Room("b", "01", {.05f, 0.f, -1.f}, {1.5f, 1.5f, 1.f}),
      Room("c", "02", {-1.5f, 3.f, -1.f}, {-.1f, 4.5f, 1.f}),
  };
  const auto plan = planDdgiBimRooms(base, rooms);
  EXPECT_EQ(base, before);
  ASSERT_EQ(plan.volumes.size(), 2u);
  ASSERT_EQ(plan.rooms.size(), 3u);
  ASSERT_EQ(plan.volumes[0].roomIndices, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(plan.volumes[1].roomIndices, (std::vector<size_t>{2}));
  EXPECT_EQ(plan.settings.volume, base.volume);
  EXPECT_EQ(plan.settings.resources, base.resources);
  EXPECT_EQ(plan.settings.payloadBudgetBytes, base.payloadBudgetBytes);
  EXPECT_EQ(plan.baseProbeCount, DdgiVolumeSet(base).probeCount());
  EXPECT_EQ(plan.totalProbeCount, DdgiVolumeSet(plan.settings).probeCount());
  EXPECT_EQ(plan.payloadBytes, planDdgiVolumeResources(plan.settings).payloadBytes);
  EXPECT_EQ(plan.framesPerProbeCycle, (plan.totalProbeCount + 127u) / 128u);
  for (const auto& room : plan.rooms) {
    EXPECT_EQ(room.status, Status::CoveredPlanned);
    EXPECT_TRUE(room.reason.empty());
    ExpectFullBlendCoverage(plan, room);
  }
  std::ranges::reverse(rooms);
  const auto reversed = planDdgiBimRooms(base, rooms);
  EXPECT_EQ(reversed.settings, plan.settings);
  ASSERT_EQ(reversed.volumes.size(), plan.volumes.size());
  EXPECT_EQ(reversed.volumes[0].roomIndices, plan.volumes[0].roomIndices);
  EXPECT_EQ(reversed.volumes[1].roomIndices, plan.volumes[1].roomIndices);
}

TEST(DdgiBimRoomPlan, PreservesManualVolumesAndRequiresFullBlendCoverage) {
  DdgiSettings base;
  base.volume.spacing = glm::vec3(1.5f);
  base.additionalVolumes = {
      {{-1.f, -1.f, -1.f}, {.25f, .25f, .25f}, {9, 9, 9}},
      {{20.f, 20.f, 20.f}, {.25f, .25f, .25f}, {4, 4, 4}},
      {{50.f, 50.f, 50.f}, {.25f, .25f, .25f}, {4, 4, 4}},
  };
  const std::array rooms{
      Room("inside", "01", {-.7f, -.7f, -.7f}, {.7f, .7f, .7f}),
      Room("edge", "01", {-.9f, -.7f, -.7f}, {.7f, .7f, .7f}),
  };
  const auto plan = planDdgiBimRooms(base, rooms);
  EXPECT_EQ(plan.settings, base);
  EXPECT_TRUE(plan.volumes.empty());
  const auto& inside = FindRoom(plan, "inside");
  EXPECT_EQ(inside.status, Status::CoveredExisting);
  EXPECT_EQ(inside.volumeIndex, 1u);
  ExpectFullBlendCoverage(plan, inside);
  const auto& edge = FindRoom(plan, "edge");
  EXPECT_EQ(edge.status, Status::Uncovered);
  EXPECT_EQ(edge.reason, "All four DDGI volume slots are occupied");
}

TEST(DdgiBimRoomPlan, KeepsStableStoreyIdentityAndPreservesManualWhenAppending) {
  DdgiSettings base;
  base.volume.spacing = glm::vec3(2.f);
  base.additionalVolumes.push_back({{20.f, 20.f, 20.f}, {.25f, .25f, .25f}, {4, 4, 4}});
  auto first = Room("a", "Ground", {0.f, 0.f, 0.f}, {2.f, 1.f, 1.f});
  auto second = Room("b", "Ground", {2.1f, 0.f, 0.f}, {4.1f, 1.f, 1.f});
  first.storeyIdentity = "wing:first";
  second.storeyIdentity = "wing:second";
  const auto plan = planDdgiBimRooms(base, std::array{first, second});
  ASSERT_EQ(plan.volumes.size(), 2u);
  EXPECT_EQ(plan.settings.additionalVolumes[0], base.additionalVolumes[0]);
  EXPECT_EQ(plan.volumes[0].volumeIndex, 2u);
  EXPECT_EQ(plan.volumes[1].volumeIndex, 3u);
  EXPECT_EQ(FindRoom(plan, "b").storeyIdentity, "wing:second");
}

TEST(DdgiBimRoomPlan, DistinguishesMissingAndAmbiguousRoomBounds) {
  const DdgiSettings base;
  auto missing = Room("missing", "01", {}, {});
  missing.bounds.valid = false;
  missing.reason = "Room has only curves";
  auto ambiguous = Room("ambiguous", "01", {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
  ambiguous.ambiguous = true;
  ambiguous.reason = "Room has two conflicting relationships";
  const auto flat = Room("flat", "01", {0.f, 0.f, 0.f}, {1.f, 0.f, 1.f});
  const auto duplicateA = Room("duplicate", "01", {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f});
  const auto duplicateB = Room("duplicate", "02", {0.f, 3.f, 0.f}, {1.f, 4.f, 1.f});
  const auto plan = planDdgiBimRooms(base, std::array{missing, ambiguous, flat, duplicateA, duplicateB});
  EXPECT_EQ(plan.settings, base);
  EXPECT_TRUE(plan.volumes.empty());
  EXPECT_EQ(FindRoom(plan, "missing").reason, missing.reason);
  EXPECT_EQ(FindRoom(plan, "missing").status, Status::MissingGeometry);
  EXPECT_EQ(FindRoom(plan, "flat").status, Status::MissingGeometry);
  EXPECT_EQ(FindRoom(plan, "ambiguous").reason, ambiguous.reason);
  EXPECT_EQ(FindRoom(plan, "ambiguous").status, Status::Ambiguous);
  EXPECT_EQ(std::ranges::count(plan.rooms, Status::Ambiguous, &DdgiBimRoomCoverage::status), 3);
  EXPECT_EQ(ddgiBimRoomCoverageName(Status::Ambiguous), "ambiguous");
}

TEST(DdgiBimRoomPlan, ReportsProbePayloadAndQualityLimitsWithoutChangingBase) {
  DdgiSettings base;
  const std::array rooms{Room("wide", "01", {0.f, 0.f, 0.f}, {10.f, 3.f, 10.f})};
  auto options = DdgiDetailVolumePlacementOptions{.maxDetailProbes = 32, .maxTotalProbes = 512};
  const auto probes = planDdgiBimRooms(base, rooms, options);
  EXPECT_TRUE(probes.volumes.empty());
  EXPECT_EQ(probes.settings, base);
  EXPECT_NE(probes.rooms[0].reason.find("probe budget"), std::string::npos);
  const auto coarse = planDdgiBimRooms(base, rooms, {.maxDetailProbes = 80});
  EXPECT_TRUE(coarse.volumes.empty());
  EXPECT_NE(coarse.rooms[0].reason.find("more finely"), std::string::npos);
  base.payloadBudgetBytes = planDdgiVolumeResources(base).payloadBytes;
  const auto bytes = planDdgiBimRooms(base, rooms);
  EXPECT_TRUE(bytes.volumes.empty());
  EXPECT_EQ(bytes.settings, base);
  EXPECT_EQ(bytes.payloadBytes, base.payloadBudgetBytes);
  EXPECT_NE(bytes.rooms[0].reason.find("payload budget"), std::string::npos);
}

TEST(DdgiBimRoomPlan, SharesRayLimitedUpdatesAndSupportsCancellation) {
  DdgiSettings base;
  base.volume.spacing = glm::vec3(1.5f);
  base.paused = true;
  base.cameraFollow = true;
  base.resources.updates = {.maxProbesPerFrame = 100, .raysPerProbe = 32, .maxRaysPerFrame = 64};
  const std::array rooms{Room("one", "01", {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f})};
  const auto plan = planDdgiBimRooms(base, rooms, {.maxTotalProbes = 1024});
  ASSERT_EQ(plan.volumes.size(), 1u);
  EXPECT_TRUE(plan.settings.paused);
  EXPECT_TRUE(plan.settings.cameraFollow);
  EXPECT_EQ(plan.settings.resources, base.resources);
  EXPECT_LE(plan.totalProbeCount, 1024u);
  EXPECT_EQ(plan.framesPerProbeCycle, (plan.totalProbeCount + 1u) / 2u);
  base.resources.updates.maxRaysPerFrame = 0;
  EXPECT_EQ(planDdgiBimRooms(base, rooms).framesPerProbeCycle, 0u);
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = planDdgiBimRooms(base, rooms, {}, stop.get_token());
  EXPECT_TRUE(cancelled.cancelled);
  EXPECT_EQ(cancelled.settings, base);
  EXPECT_TRUE(cancelled.rooms.empty());
  EXPECT_TRUE(cancelled.volumes.empty());
}

} // namespace
} // namespace container::renderer
