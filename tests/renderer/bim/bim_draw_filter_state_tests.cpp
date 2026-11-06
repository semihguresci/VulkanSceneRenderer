#include "Container/renderer/bim/BimDrawFilterState.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

using container::renderer::BimDisciplinePreset;
using container::renderer::BimDrawFilter;
using container::renderer::BimDrawFilterState;
using container::renderer::BimDrawFilterStateInputs;
using container::renderer::BimElementMetadata;
using container::renderer::DrawCommand;

[[nodiscard]] BimDrawFilterStateInputs
makeInputs(uint64_t revision, const std::vector<BimElementMetadata> &metadata,
           const std::vector<DrawCommand> &opaqueSingleSided,
           std::span<const std::string> phaseOrder = {}) {
  return BimDrawFilterStateInputs{
      .revision = revision,
      .objectCount = metadata.size(),
      .metadata = {metadata.data(), metadata.size()},
      .phaseOrder = phaseOrder,
      .opaqueSingleSidedDrawCommands = &opaqueSingleSided,
  };
}

TEST(BimDrawFilterStateTests, FiltersAndMergesInstancedSurfaceCommands) {
  std::vector<BimElementMetadata> metadata(3u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "Wall";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "Wall";
  metadata[2].objectIndex = 2u;
  metadata[2].type = "Door";

  const std::vector<DrawCommand> opaqueSingleSided{
      DrawCommand{.objectIndex = 0u,
                  .firstIndex = 12u,
                  .indexCount = 6u,
                  .instanceCount = 3u},
  };

  BimDrawFilter filter{};
  filter.typeFilterEnabled = true;
  filter.type = "Wall";

  BimDrawFilterState state;
  const auto inputs = makeInputs(7u, metadata, opaqueSingleSided);
  const auto &lists = state.filteredDrawLists(filter, inputs);

  ASSERT_EQ(lists.opaqueSingleSidedDrawCommands.size(), 1u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].objectIndex, 0u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].firstIndex, 12u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].indexCount, 6u);
  EXPECT_EQ(lists.opaqueSingleSidedDrawCommands[0].instanceCount, 2u);
  EXPECT_TRUE(state.objectMatchesFilter(1u, filter, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, filter, inputs));
}

TEST(BimDrawFilterStateTests, SelectionFiltersUseProductIdentity) {
  std::vector<BimElementMetadata> metadata(3u);
  metadata[0].objectIndex = 0u;
  metadata[0].guid = "shared-guid";
  metadata[1].objectIndex = 1u;
  metadata[1].guid = "shared-guid";
  metadata[2].objectIndex = 2u;
  metadata[2].guid = "other-guid";
  const std::vector<DrawCommand> opaqueSingleSided;

  BimDrawFilter isolate{};
  isolate.isolateSelection = true;
  isolate.selectedObjectIndex = 0u;

  BimDrawFilterState state;
  const auto inputs = makeInputs(1u, metadata, opaqueSingleSided);
  EXPECT_TRUE(state.objectMatchesFilter(0u, isolate, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(1u, isolate, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, isolate, inputs));

  BimDrawFilter hide{};
  hide.hideSelection = true;
  hide.selectedObjectIndex = 0u;
  EXPECT_FALSE(state.objectMatchesFilter(0u, hide, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(1u, hide, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(2u, hide, inputs));
}

TEST(BimDrawFilterStateTests, RoomVisibilitySplitsAndRestoresInstancedDraws) {
  const std::vector<BimElementMetadata> metadata{
      {.objectIndex = 0u, .type = "IfcWall"},
      {.objectIndex = 1u, .type = "IfcSpace"},
      {.objectIndex = 2u, .type = "IfcSpaceHeater"},
  };
  const std::vector<DrawCommand> draws{
      {.objectIndex = 0u, .firstIndex = 12u, .indexCount = 6u, .instanceCount = 3u},
  };
  const auto inputs = makeInputs(1u, metadata, draws);
  BimDrawFilterState state;
  BimDrawFilter filter{.hideRoomVolumes = true};

  const auto &hidden = state.filteredDrawLists(filter, inputs).opaqueSingleSidedDrawCommands;
  ASSERT_EQ(hidden.size(), 2u);
  EXPECT_EQ(hidden[0].objectIndex, 0u);
  EXPECT_EQ(hidden[0].instanceCount, 1u);
  EXPECT_EQ(hidden[1].objectIndex, 2u);
  EXPECT_EQ(hidden[1].instanceCount, 1u);
  EXPECT_FALSE(state.objectMatchesFilter(1u, filter, inputs));

  filter.hideRoomVolumes = false;
  const auto &shown = state.filteredDrawLists(filter, inputs).opaqueSingleSidedDrawCommands;
  ASSERT_EQ(shown.size(), 1u);
  EXPECT_EQ(shown[0].instanceCount, 3u);
  EXPECT_TRUE(state.objectMatchesFilter(1u, filter, inputs));
}

TEST(BimDrawFilterStateTests, CachedListsRefreshOnlyWhenRevisionChanges) {
  std::vector<BimElementMetadata> metadata(2u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "Wall";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "Wall";
  std::vector<DrawCommand> opaqueSingleSided{
      DrawCommand{.objectIndex = 0u,
                  .firstIndex = 4u,
                  .indexCount = 3u,
                  .instanceCount = 1u},
  };

  BimDrawFilter filter{};
  filter.typeFilterEnabled = true;
  filter.type = "Wall";

  BimDrawFilterState state;
  auto inputs = makeInputs(1u, metadata, opaqueSingleSided);
  size_t filteredCount = state.filteredDrawLists(filter, inputs)
                             .opaqueSingleSidedDrawCommands.size();
  ASSERT_EQ(filteredCount, 1u);

  opaqueSingleSided.push_back(DrawCommand{.objectIndex = 1u,
                                          .firstIndex = 8u,
                                          .indexCount = 3u,
                                          .instanceCount = 1u});
  filteredCount = state.filteredDrawLists(filter, inputs)
                      .opaqueSingleSidedDrawCommands.size();
  EXPECT_EQ(filteredCount, 1u);

  inputs.revision = 2u;
  filteredCount = state.filteredDrawLists(filter, inputs)
                      .opaqueSingleSidedDrawCommands.size();
  EXPECT_EQ(filteredCount, 2u);
}

TEST(BimDrawFilterStateTests, PhaseTimelineHidesFutureAndDemolishedElements) {
  std::vector<BimElementMetadata> metadata(4u);
  metadata[0].objectIndex = 0u;
  metadata[0].phase = "Existing";
  metadata[0].status = "Existing";
  metadata[1].objectIndex = 1u;
  metadata[1].phase = "New";
  metadata[1].status = "New";
  metadata[2].objectIndex = 2u;
  metadata[2].phase = "Future";
  metadata[2].status = "New";
  metadata[3].objectIndex = 3u;
  metadata[3].phase = "Existing";
  metadata[3].status = "Demolished";
  const std::vector<DrawCommand> opaqueSingleSided;
  const std::array<std::string, 3u> phases{"Existing", "New", "Future"};

  BimDrawFilter filter{};
  filter.phaseTimelineEnabled = true;
  filter.phaseTimelineActiveIndex = 1u;
  filter.phaseTimelineShowExisting = true;
  filter.phaseTimelineShowNew = true;
  filter.phaseTimelineShowDemolished = false;
  filter.phaseTimelineGhostFuture = false;

  BimDrawFilterState state;
  const auto inputs = makeInputs(3u, metadata, opaqueSingleSided, phases);

  EXPECT_TRUE(state.objectMatchesFilter(0u, filter, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(1u, filter, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, filter, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(3u, filter, inputs));
}

TEST(BimDrawFilterStateTests, ArchitecturePresetHidesMepClasses) {
  std::vector<BimElementMetadata> metadata(3u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "IfcWall";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "IfcDuctSegment";
  metadata[2].objectIndex = 2u;
  metadata[2].type = "IfcPipeSegment";
  const std::vector<DrawCommand> opaqueSingleSided;

  BimDrawFilter filter{};
  filter.disciplinePreset = BimDisciplinePreset::Architecture;

  BimDrawFilterState state;
  const auto inputs = makeInputs(4u, metadata, opaqueSingleSided);

  EXPECT_TRUE(state.objectMatchesFilter(0u, filter, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(1u, filter, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, filter, inputs));
}

TEST(BimDrawFilterStateTests, MepXrayPresetShowsMepServiceClasses) {
  std::vector<BimElementMetadata> metadata(5u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "IfcWall";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "IfcPipeSegment";
  metadata[2].objectIndex = 2u;
  metadata[2].type = "IfcDuctSegment";
  metadata[3].objectIndex = 3u;
  metadata[3].type = "IfcCableCarrierSegment";
  metadata[4].objectIndex = 4u;
  metadata[4].type = "IfcServiceTerminal";
  const std::vector<DrawCommand> opaqueSingleSided;

  BimDrawFilter filter{};
  filter.disciplinePreset = BimDisciplinePreset::MepXray;

  BimDrawFilterState state;
  const auto inputs = makeInputs(5u, metadata, opaqueSingleSided);

  EXPECT_FALSE(state.objectMatchesFilter(0u, filter, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(1u, filter, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(2u, filter, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(3u, filter, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(4u, filter, inputs));
}

TEST(BimDrawFilterStateTests, DisciplinePresetsIgnoreMepWordsOutsideTypeFields) {
  std::vector<BimElementMetadata> metadata(5u);
  metadata[0].objectIndex = 0u;
  metadata[0].type = "IfcWall";
  metadata[0].objectType = "Basic Wall";
  metadata[0].displayName = "Conductive service wall";
  metadata[0].materialName = "conductive service coating";
  metadata[0].discipline = "Architecture";
  metadata[1].objectIndex = 1u;
  metadata[1].type = "IfcDuctSegment";
  metadata[2].objectIndex = 2u;
  metadata[2].type = "IfcPipeSegment";
  metadata[3].objectIndex = 3u;
  metadata[3].type = "IfcCableCarrierSegment";
  metadata[4].objectIndex = 4u;
  metadata[4].type = "IfcServiceTerminal";
  const std::vector<DrawCommand> opaqueSingleSided;

  BimDrawFilterState state;
  const auto inputs = makeInputs(6u, metadata, opaqueSingleSided);

  BimDrawFilter architecture{};
  architecture.disciplinePreset = BimDisciplinePreset::Architecture;
  EXPECT_TRUE(state.objectMatchesFilter(0u, architecture, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(1u, architecture, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(2u, architecture, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(3u, architecture, inputs));
  EXPECT_FALSE(state.objectMatchesFilter(4u, architecture, inputs));

  BimDrawFilter mepXray{};
  mepXray.disciplinePreset = BimDisciplinePreset::MepXray;
  EXPECT_FALSE(state.objectMatchesFilter(0u, mepXray, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(1u, mepXray, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(2u, mepXray, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(3u, mepXray, inputs));
  EXPECT_TRUE(state.objectMatchesFilter(4u, mepXray, inputs));
}

} // namespace
