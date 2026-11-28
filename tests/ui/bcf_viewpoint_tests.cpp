#include "Container/renderer/bim/BimCoordinationOverlay.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimMeasurementSnapping.h"
#include "Container/utility/BcfViewpoint.h"
#include "Container/utility/GuiManager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

#include <miniz/miniz.h>

namespace {

container::ui::ViewpointSnapshotState sampleSnapshot() {
  container::ui::ViewpointSnapshotState snapshot{};
  snapshot.label = "Door review";
  snapshot.camera.position = {1.0f, 2.0f, 3.0f};
  snapshot.camera.rotationDegrees = {-15.0f, 45.0f, 0.0f};
  snapshot.camera.scale = {1.0f, 1.0f, 1.0f};
  snapshot.selectedMeshNode = 12u;
  snapshot.selectedBimObjectIndex = 34u;
  snapshot.selectedBimGuid = "2n1$example";
  snapshot.selectedBimType = "IfcDoor";
  snapshot.selectedBimSourceId = "#42";
  snapshot.bimModelPath = "models/example.ifc";
  snapshot.bimFilter.typeFilterEnabled = true;
  snapshot.bimFilter.type = "IfcDoor";
  snapshot.bimFilter.storeyFilterEnabled = true;
  snapshot.bimFilter.storey = "Level 01";
  snapshot.bimFilter.statusFilterEnabled = true;
  snapshot.bimFilter.status = "Existing";
  snapshot.bimFilter.isolateSelection = true;
  snapshot.bimFilter.disciplinePreset =
      container::renderer::BimDisciplinePreset::MepXray;
  snapshot.phaseTimeline.enabled = true;
  snapshot.phaseTimeline.activePhaseIndex = 2u;
  snapshot.phaseTimeline.showExisting = true;
  snapshot.phaseTimeline.showNew = false;
  snapshot.phaseTimeline.showDemolished = true;
  snapshot.phaseTimeline.ghostFuture = true;
  return snapshot;
}

std::vector<std::string> archiveEntryNames(const std::filesystem::path& path) {
  std::vector<std::string> names;
  mz_zip_archive zip{};
  const std::string archivePath = path.string();
  if (!mz_zip_reader_init_file(&zip, archivePath.c_str(), 0)) {
    return names;
  }

  const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
  names.reserve(fileCount);
  for (mz_uint i = 0; i < fileCount; ++i) {
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_is_file_a_directory(&zip, i) ||
        !mz_zip_reader_file_stat(&zip, i, &stat)) {
      continue;
    }
    names.emplace_back(stat.m_filename);
  }
  mz_zip_reader_end(&zip);
  return names;
}

}  // namespace

TEST(BimMeasurementCapture, AppliesConfiguredSnapModeToSelectedBounds) {
  container::ui::BimInspectionState inspection{};
  inspection.hasSelection = true;
  inspection.hasSelectionBounds = true;
  inspection.selectedObjectIndex = 42u;
  inspection.modelPath = "model.ifc";
  inspection.type = "IfcWall";
  inspection.guid = "wall-guid";
  inspection.selectionBoundsMin = {0.0f, 10.0f, 20.0f};
  inspection.selectionBoundsMax = {4.0f, 16.0f, 28.0f};
  inspection.selectionBoundsCenter = {2.0f, 13.0f, 24.0f};
  inspection.selectionFloorElevation = 9.5f;

  container::ui::BimMeasurementSnapUiState snap{};
  snap.maxScreenDistancePixels = 12.0f;

  snap.mode = container::ui::BimMeasurementSnapMode::Off;
  auto captured =
      container::ui::CaptureBimMeasurementPointFromSelection(inspection, snap);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->snapKind, container::renderer::BimSnapKind::BoundsCenter);
  EXPECT_EQ(captured->center, inspection.selectionBoundsCenter);

  snap.mode = container::ui::BimMeasurementSnapMode::Vertex;
  captured =
      container::ui::CaptureBimMeasurementPointFromSelection(inspection, snap);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->snapKind, container::renderer::BimSnapKind::BoundsCorner);
  EXPECT_EQ(captured->center, glm::vec3(0.0f, 10.0f, 20.0f));

  snap.mode = container::ui::BimMeasurementSnapMode::Bounds;
  captured =
      container::ui::CaptureBimMeasurementPointFromSelection(inspection, snap);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->snapKind, container::renderer::BimSnapKind::BoundsCorner);
  EXPECT_EQ(captured->center, glm::vec3(0.0f, 10.0f, 20.0f));

  snap.mode = container::ui::BimMeasurementSnapMode::Edge;
  captured =
      container::ui::CaptureBimMeasurementPointFromSelection(inspection, snap);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->snapKind, container::renderer::BimSnapKind::EdgeMidpoint);
  EXPECT_EQ(captured->center, glm::vec3(2.0f, 10.0f, 20.0f));

  snap.mode = container::ui::BimMeasurementSnapMode::Face;
  captured =
      container::ui::CaptureBimMeasurementPointFromSelection(inspection, snap);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->snapKind, container::renderer::BimSnapKind::FaceCenter);
  EXPECT_EQ(captured->center, glm::vec3(0.0f, 13.0f, 24.0f));

  snap.mode = container::ui::BimMeasurementSnapMode::Floor;
  captured =
      container::ui::CaptureBimMeasurementPointFromSelection(inspection, snap);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->snapKind, container::renderer::BimSnapKind::FloorElevation);
  EXPECT_EQ(captured->center, glm::vec3(2.0f, 9.5f, 24.0f));
}

TEST(BcfViewpoint, RoundTripsContainerSnapshotFields) {
  auto snapshot = sampleSnapshot();
  snapshot.additionalBimSelections.push_back({"second-guid", "#456"});
  const std::string xml =
      container::ui::bcf::ExportVisualizationInfo(snapshot);

  EXPECT_NE(xml.find("<VisualizationInfo>"), std::string::npos);
  EXPECT_NE(xml.find("<PerspectiveCamera>"), std::string::npos);
  EXPECT_NE(xml.find("IfcGuid=\"2n1$example\""), std::string::npos);

  const auto imported = container::ui::bcf::ImportVisualizationInfo(xml);
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->label, snapshot.label);
  EXPECT_FLOAT_EQ(imported->camera.position.x, snapshot.camera.position.x);
  EXPECT_FLOAT_EQ(imported->camera.position.y, snapshot.camera.position.y);
  EXPECT_FLOAT_EQ(imported->camera.position.z, snapshot.camera.position.z);
  EXPECT_FLOAT_EQ(imported->camera.rotationDegrees.x,
                  snapshot.camera.rotationDegrees.x);
  EXPECT_FLOAT_EQ(imported->camera.rotationDegrees.y,
                  snapshot.camera.rotationDegrees.y);
  EXPECT_EQ(imported->selectedMeshNode, snapshot.selectedMeshNode);
  EXPECT_EQ(imported->selectedBimObjectIndex,
            snapshot.selectedBimObjectIndex);
  EXPECT_EQ(imported->selectedBimGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(imported->selectedBimType, snapshot.selectedBimType);
  EXPECT_EQ(imported->selectedBimSourceId, snapshot.selectedBimSourceId);
  ASSERT_EQ(imported->additionalBimSelections.size(), 1u);
  EXPECT_EQ(imported->additionalBimSelections.front().guid, "second-guid");
  EXPECT_EQ(imported->additionalBimSelections.front().sourceId, "#456");
  EXPECT_EQ(imported->bimModelPath, snapshot.bimModelPath);
  EXPECT_TRUE(imported->bimFilter.typeFilterEnabled);
  EXPECT_EQ(imported->bimFilter.type, "IfcDoor");
  EXPECT_TRUE(imported->bimFilter.storeyFilterEnabled);
  EXPECT_EQ(imported->bimFilter.storey, "Level 01");
  EXPECT_TRUE(imported->bimFilter.statusFilterEnabled);
  EXPECT_EQ(imported->bimFilter.status, "Existing");
  EXPECT_TRUE(imported->bimFilter.isolateSelection);
  EXPECT_EQ(imported->bimFilter.disciplinePreset,
            container::renderer::BimDisciplinePreset::MepXray);
  EXPECT_TRUE(imported->phaseTimeline.enabled);
  EXPECT_EQ(imported->phaseTimeline.activePhaseIndex, 2u);
  EXPECT_TRUE(imported->phaseTimeline.showExisting);
  EXPECT_FALSE(imported->phaseTimeline.showNew);
  EXPECT_TRUE(imported->phaseTimeline.showDemolished);
  EXPECT_TRUE(imported->phaseTimeline.ghostFuture);
}

TEST(BcfViewpoint, RoundTripsBimFilterDisciplinePresetValues) {
  using container::renderer::BimDisciplinePreset;

  const std::array presets{
      BimDisciplinePreset::None,
      BimDisciplinePreset::Architecture,
      BimDisciplinePreset::MepXray,
  };

  for (const BimDisciplinePreset preset : presets) {
    SCOPED_TRACE(static_cast<uint32_t>(preset));
    auto snapshot = sampleSnapshot();
    snapshot.bimFilter.disciplinePreset = preset;

    const auto imported = container::ui::bcf::ImportVisualizationInfo(
        container::ui::bcf::ExportVisualizationInfo(snapshot));

    ASSERT_TRUE(imported.has_value());
    EXPECT_EQ(imported->bimFilter.disciplinePreset, preset);
  }
}

TEST(BcfViewpoint, SavesAndLoadsVisualizationInfoFile) {
  const auto snapshot = sampleSnapshot();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_viewpoint.bcfv";

  ASSERT_TRUE(container::ui::bcf::SaveVisualizationInfo(snapshot, path));
  const auto imported = container::ui::bcf::LoadVisualizationInfo(path);
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->label, snapshot.label);
  EXPECT_EQ(imported->selectedBimGuid, snapshot.selectedBimGuid);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(BcfViewpoint, SavesAndLoadsStandaloneVisualizationInfoWithMarkup) {
  namespace bcf = container::ui::bcf;
  const auto snapshot = sampleSnapshot();
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "container_bcf_viewpoint_with_markup.bcfv";
  const bcf::BcfViewpointMarkup markup{
      .guid = "standalone-viewpoint-guid",
      .viewpointFile = "review.bcfv",
      .snapshotFile = "door-review.png",
      .pins = {{.guid = "located-pin-guid",
                .label = "Door & frame",
                .hasLocation = true,
                .location = {4.0f, -5.0f, 6.25f},
                .ifcGuid = snapshot.selectedBimGuid,
                .sourceId = snapshot.selectedBimSourceId},
               {.guid = "unlocated-pin-guid",
                .label = "Check hardware",
                .ifcGuid = "3n1$hardware",
                .sourceId = "#43"}},
      .lines = {{
          .guid = "clearance-line-guid",
          .start = {1.0f, 2.0f, 3.0f},
          .end = {4.0f, 5.0f, 6.0f},
          .color = {0.2f, 0.4f, 0.8f},
          .label = "Clearance < 3 m",
      }},
  };

  ASSERT_TRUE(bcf::SaveVisualizationInfo(snapshot, markup, path));
  const auto legacySnapshot = bcf::LoadVisualizationInfo(path);
  const auto loaded = bcf::LoadVisualizationInfoWithMarkup(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(legacySnapshot.has_value());
  EXPECT_EQ(legacySnapshot->selectedBimGuid, snapshot.selectedBimGuid);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->snapshot.label, snapshot.label);
  EXPECT_EQ(loaded->snapshot.camera.position, snapshot.camera.position);
  EXPECT_EQ(loaded->snapshot.selectedBimGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(loaded->markup.guid, markup.guid);
  EXPECT_EQ(loaded->markup.viewpointFile, markup.viewpointFile);
  EXPECT_EQ(loaded->markup.snapshotFile, markup.snapshotFile);
  ASSERT_EQ(loaded->markup.pins.size(), 2u);
  const bcf::BcfPin& pin = loaded->markup.pins.front();
  EXPECT_EQ(pin.guid, markup.pins.front().guid);
  EXPECT_EQ(pin.label, markup.pins.front().label);
  EXPECT_TRUE(pin.hasLocation);
  EXPECT_EQ(pin.location, markup.pins.front().location);
  EXPECT_EQ(pin.ifcGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(pin.sourceId, snapshot.selectedBimSourceId);
  EXPECT_EQ(loaded->markup.pins.back().guid, markup.pins.back().guid);
  EXPECT_EQ(loaded->markup.pins.back().label, markup.pins.back().label);
  EXPECT_FALSE(loaded->markup.pins.back().hasLocation);
  ASSERT_EQ(loaded->markup.lines.size(), 1u);
  const bcf::BcfMarkupLine& line = loaded->markup.lines.front();
  EXPECT_EQ(line.guid, markup.lines.front().guid);
  EXPECT_EQ(line.label, markup.lines.front().label);
  EXPECT_EQ(line.start, markup.lines.front().start);
  EXPECT_EQ(line.end, markup.lines.front().end);
  EXPECT_EQ(line.color, markup.lines.front().color);

  const auto issuePins =
      container::renderer::exportBcfPinsAsIssueOverlayMarkers(loaded->markup);
  ASSERT_EQ(issuePins.size(), 1u);
  EXPECT_EQ(issuePins.front().position, pin.location);
  EXPECT_EQ(issuePins.front().label, pin.label);
  EXPECT_EQ(issuePins.front().ifcGuid, pin.ifcGuid);
  EXPECT_EQ(issuePins.front().sourceId, pin.sourceId);
}

TEST(BcfViewpoint, RejectsStandaloneMarkupWithoutViewpoint) {
  namespace bcf = container::ui::bcf;
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "container_bcf_markup_without_viewpoint.bcfv";
  std::ofstream file(path, std::ios::binary);
  file << R"xml(<VisualizationInfo><ContainerMarkup Guid="orphaned"/>
</VisualizationInfo>)xml";
  file.close();
  ASSERT_TRUE(file);
  EXPECT_FALSE(bcf::LoadVisualizationInfoWithMarkup(path).has_value());

  std::error_code ec;
  std::filesystem::remove(path, ec);
  EXPECT_FALSE(bcf::LoadVisualizationInfoWithMarkup(path).has_value());
  EXPECT_FALSE(
      bcf::LoadVisualizationInfoWithMarkup(path.parent_path()).has_value());
}

TEST(BcfViewpoint, PreservesViewpointWhenMarkupCoordinatesAreOutOfRange) {
  namespace bcf = container::ui::bcf;
  const auto snapshot = sampleSnapshot();
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "container_bcf_out_of_range_markup.bcfv";
  std::string xml = bcf::ExportVisualizationInfo(snapshot);
  xml.insert(xml.find("</VisualizationInfo>"), R"xml(
  <ContainerMarkup>
    <Pin Label="Unplaced" IfcGuid="2n1$example" AuthoringToolId="#42">
      <Point><X>1e100</X><Y>0</Y><Z>0</Z></Point>
    </Pin>
    <MarkupLine Guid="invalid-endpoint">
      <Start><X>0</X><Y>0</Y><Z>0</Z></Start>
      <End><X>0</X><Y>1e100</Y><Z>0</Z></End>
    </MarkupLine>
  </ContainerMarkup>
)xml");
  std::ofstream file(path, std::ios::binary);
  file << xml;
  file.close();
  ASSERT_TRUE(file);
  const auto loaded = bcf::LoadVisualizationInfoWithMarkup(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->snapshot.camera.position, snapshot.camera.position);
  ASSERT_EQ(loaded->markup.pins.size(), 1u);
  const bcf::BcfPin& pin = loaded->markup.pins.front();
  EXPECT_EQ(pin.guid, bcf::StablePinGuid("2n1$example", "#42"));
  EXPECT_EQ(pin.label, "Unplaced");
  EXPECT_FALSE(pin.hasLocation);
  EXPECT_TRUE(loaded->markup.lines.empty());
}

TEST(BcfViewpoint, ImportsStandardPerspectiveCameraWithoutContainerSnapshot) {
  constexpr const char* kStandardBcf = R"xml(
<VisualizationInfo>
  <PerspectiveCamera>
    <CameraViewPoint>
      <X>10</X>
      <Y>2</Y>
      <Z>-5</Z>
    </CameraViewPoint>
    <CameraDirection>
      <X>0</X>
      <Y>0</Y>
      <Z>-1</Z>
    </CameraDirection>
    <CameraUpVector>
      <X>0</X>
      <Y>1</Y>
      <Z>0</Z>
    </CameraUpVector>
    <FieldOfView>60</FieldOfView>
  </PerspectiveCamera>
  <Components>
    <Selection>
      <Component IfcGuid="3abc" AuthoringToolId="#123"/>
    </Selection>
  </Components>
</VisualizationInfo>
)xml";

  const auto imported =
      container::ui::bcf::ImportVisualizationInfo(kStandardBcf);
  ASSERT_TRUE(imported.has_value());
  EXPECT_FLOAT_EQ(imported->camera.position.x, 10.0f);
  EXPECT_FLOAT_EQ(imported->camera.position.y, 2.0f);
  EXPECT_FLOAT_EQ(imported->camera.position.z, -5.0f);
  EXPECT_NEAR(imported->camera.rotationDegrees.x, 0.0f, 1.0e-5f);
  EXPECT_NEAR(imported->camera.rotationDegrees.y, 90.0f, 1.0e-5f);
  EXPECT_EQ(imported->selectedBimGuid, "3abc");
  EXPECT_EQ(imported->selectedBimSourceId, "#123");

  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "container_bcf_standard_perspective.bcfv";
  std::ofstream file(path, std::ios::binary);
  file << kStandardBcf;
  file.close();
  ASSERT_TRUE(file);
  const auto loaded = container::ui::bcf::LoadVisualizationInfoWithMarkup(path);
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->snapshot.camera.position, imported->camera.position);
  EXPECT_EQ(loaded->snapshot.selectedBimGuid, imported->selectedBimGuid);
  EXPECT_TRUE(loaded->markup.guid.empty());
  EXPECT_TRUE(loaded->markup.viewpointFile.empty());
  EXPECT_TRUE(loaded->markup.snapshotFile.empty());
  EXPECT_TRUE(loaded->markup.pins.empty());
  EXPECT_TRUE(loaded->markup.lines.empty());
}

TEST(BcfViewpoint, RoundTripsTopicCommentsAndViewpointPins) {
  container::ui::bcf::BcfMarkup markup{};
  markup.topic.guid = "topic-1";
  markup.topic.title = "Door clearance";
  markup.topic.status = "Open";
  markup.topic.priority = "High";
  markup.topic.labels = {"coordination", "door"};
  markup.topic.tags = {"egress", "clearance"};

  container::ui::bcf::BcfViewpointMarkup viewpoint{};
  viewpoint.guid = "viewpoint-1";
  viewpoint.viewpointFile = "viewpoint.bcfv";
  viewpoint.snapshotFile = "snapshot.png";
  viewpoint.pins.push_back({
      .label = "Door leaf",
      .hasLocation = true,
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = "2n1$example",
      .sourceId = "#42",
  });
  markup.viewpoints.push_back(viewpoint);

  container::ui::bcf::BcfViewpointMarkup secondViewpoint{};
  secondViewpoint.guid = "viewpoint-2";
  secondViewpoint.viewpointFile = "viewpoint-2.bcfv";
  secondViewpoint.pins.push_back({
      .label = "Frame",
      .hasLocation = true,
      .location = {2.0f, 3.0f, 4.0f},
      .ifcGuid = "3n1$frame",
      .sourceId = "#43",
  });
  markup.viewpoints.push_back(secondViewpoint);

  markup.comments.push_back({
      .guid = "comment-1",
      .date = "2026-05-03T12:00:00Z",
      .author = "Reviewer",
      .text = "Please verify the clear opening.",
      .viewpointGuid = "viewpoint-1",
  });
  markup.comments.push_back({
      .guid = "comment-2",
      .author = "Architect",
      .text = "Frame detail is the second viewpoint.",
      .viewpointGuid = "viewpoint-2",
  });

  const std::string xml = container::ui::bcf::ExportMarkup(markup);
  EXPECT_NE(xml.find("<Markup>"), std::string::npos);
  EXPECT_NE(xml.find("TopicStatus=\"Open\""), std::string::npos);
  EXPECT_NE(xml.find("IfcGuid=\"2n1$example\""), std::string::npos);
  EXPECT_NE(xml.find("<Label>coordination</Label>"), std::string::npos);
  EXPECT_NE(xml.find("<Tag>egress</Tag>"), std::string::npos);
  const std::string expectedPinGuid =
      container::ui::bcf::StablePinGuid("2n1$example", "#42", "topic-1");
  EXPECT_NE(xml.find("Guid=\"" + expectedPinGuid + "\""), std::string::npos);

  const auto imported = container::ui::bcf::ImportMarkup(xml);
  ASSERT_TRUE(imported.has_value());
  EXPECT_EQ(imported->topic.guid, "topic-1");
  EXPECT_EQ(imported->topic.title, "Door clearance");
  EXPECT_EQ(imported->topic.status, "Open");
  EXPECT_EQ(imported->topic.priority, "High");
  ASSERT_EQ(imported->topic.labels.size(), 2u);
  EXPECT_EQ(imported->topic.labels.front(), "coordination");
  ASSERT_EQ(imported->topic.tags.size(), 2u);
  EXPECT_EQ(imported->topic.tags.front(), "egress");

  ASSERT_EQ(imported->comments.size(), 2u);
  EXPECT_EQ(imported->comments.front().guid, "comment-1");
  EXPECT_EQ(imported->comments.front().author, "Reviewer");
  EXPECT_EQ(imported->comments.front().text,
            "Please verify the clear opening.");
  EXPECT_EQ(imported->comments.front().viewpointGuid, "viewpoint-1");
  EXPECT_EQ(imported->comments.back().viewpointGuid, "viewpoint-2");

  ASSERT_EQ(imported->viewpoints.size(), 2u);
  EXPECT_EQ(imported->viewpoints.front().guid, "viewpoint-1");
  ASSERT_EQ(imported->viewpoints.front().pins.size(), 1u);
  const container::ui::bcf::BcfPin& pin =
      imported->viewpoints.front().pins.front();
  EXPECT_EQ(pin.guid, expectedPinGuid);
  EXPECT_EQ(pin.label, "Door leaf");
  EXPECT_TRUE(pin.hasLocation);
  EXPECT_FLOAT_EQ(pin.location.x, 1.0f);
  EXPECT_FLOAT_EQ(pin.location.y, 2.0f);
  EXPECT_FLOAT_EQ(pin.location.z, 3.0f);
  EXPECT_EQ(pin.ifcGuid, "2n1$example");
  EXPECT_EQ(pin.sourceId, "#42");
  ASSERT_EQ(imported->viewpoints.back().pins.size(), 1u);
  EXPECT_EQ(imported->viewpoints.back().pins.front().guid,
            container::ui::bcf::StablePinGuid("3n1$frame", "#43", "topic-1"));
}

TEST(BcfViewpoint, VisualizationInfoPinsDoNotBreakSnapshotImport) {
  const auto snapshot = sampleSnapshot();
  container::ui::bcf::BcfViewpointMarkup markup{};
  markup.guid = "viewpoint-1";
  markup.pins.push_back({
      .guid = "pin-1",
      .label = "Selected door",
      .hasLocation = true,
      .location = {4.0f, 5.0f, 6.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });

  const std::string xml =
      container::ui::bcf::ExportVisualizationInfo(snapshot, markup);

  const auto importedSnapshot =
      container::ui::bcf::ImportVisualizationInfo(xml);
  ASSERT_TRUE(importedSnapshot.has_value());
  EXPECT_EQ(importedSnapshot->label, snapshot.label);
  EXPECT_EQ(importedSnapshot->selectedBimGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(importedSnapshot->selectedBimSourceId,
            snapshot.selectedBimSourceId);

  const auto importedMarkup =
      container::ui::bcf::ImportVisualizationInfoMarkup(xml);
  ASSERT_TRUE(importedMarkup.has_value());
  EXPECT_EQ(importedMarkup->guid, "viewpoint-1");
  ASSERT_EQ(importedMarkup->pins.size(), 1u);
  EXPECT_TRUE(importedMarkup->pins.front().hasLocation);
  EXPECT_EQ(importedMarkup->pins.front().ifcGuid, snapshot.selectedBimGuid);
  EXPECT_EQ(importedMarkup->pins.front().sourceId,
            snapshot.selectedBimSourceId);
  EXPECT_FLOAT_EQ(importedMarkup->pins.front().location.x, 4.0f);
}

TEST(BcfViewpoint, SavesAndLoadsUnpackedTopicFolder) {
  const auto snapshot = sampleSnapshot();
  auto secondSnapshot = snapshot;
  secondSnapshot.label = "Frame review";
  secondSnapshot.selectedBimGuid = "3n1$frame";
  secondSnapshot.selectedBimSourceId = "#43";
  secondSnapshot.camera.position = {7.0f, 8.0f, 9.0f};

  container::ui::bcf::BcfTopicFolder folder{};
  folder.markup.topic.guid = "topic-folder";
  folder.markup.topic.title = "Door and frame";
  folder.markup.topic.status = "Open";
  folder.markup.topic.labels = {"coordination"};

  container::ui::bcf::BcfTopicViewpoint first{};
  first.markup.guid = "viewpoint-a";
  first.markup.viewpointFile = "viewpoint-a.bcfv";
  first.markup.pins.push_back({
      .label = "Door",
      .hasLocation = true,
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });
  first.snapshot = snapshot;
  folder.viewpoints.push_back(first);

  container::ui::bcf::BcfTopicViewpoint second{};
  second.markup.guid = "viewpoint-b";
  second.markup.viewpointFile = "viewpoint-b.bcfv";
  second.markup.pins.push_back({
      .label = "Frame",
      .hasLocation = true,
      .location = {7.0f, 8.0f, 9.0f},
      .ifcGuid = secondSnapshot.selectedBimGuid,
      .sourceId = secondSnapshot.selectedBimSourceId,
  });
  second.snapshot = secondSnapshot;
  folder.viewpoints.push_back(second);

  folder.markup.comments.push_back({
      .guid = "comment-a",
      .author = "Reviewer",
      .text = "Door issue",
      .viewpointGuid = "viewpoint-a",
  });
  folder.markup.comments.push_back({
      .guid = "comment-b",
      .author = "Reviewer",
      .text = "Frame issue",
      .viewpointGuid = "viewpoint-b",
  });

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_topic_folder";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  ASSERT_TRUE(container::ui::bcf::SaveTopicFolder(folder, path));
  EXPECT_TRUE(std::filesystem::exists(path / "markup.bcf"));
  EXPECT_TRUE(std::filesystem::exists(path / "viewpoint-a.bcfv"));
  EXPECT_TRUE(std::filesystem::exists(path / "viewpoint-b.bcfv"));

  const auto loaded = container::ui::bcf::LoadTopicFolder(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->markup.topic.title, "Door and frame");
  ASSERT_EQ(loaded->markup.topic.labels.size(), 1u);
  EXPECT_EQ(loaded->markup.topic.labels.front(), "coordination");
  ASSERT_EQ(loaded->markup.comments.size(), 2u);
  EXPECT_EQ(loaded->markup.comments.front().viewpointGuid, "viewpoint-a");
  EXPECT_EQ(loaded->markup.comments.back().viewpointGuid, "viewpoint-b");

  ASSERT_EQ(loaded->viewpoints.size(), 2u);
  ASSERT_TRUE(loaded->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);
  ASSERT_TRUE(loaded->viewpoints.back().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.back().snapshot->selectedBimGuid,
            secondSnapshot.selectedBimGuid);
  ASSERT_EQ(loaded->viewpoints.front().markup.pins.size(), 1u);
  EXPECT_EQ(loaded->viewpoints.front().markup.pins.front().guid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "topic-folder"));

  std::filesystem::remove_all(path, ec);
}

TEST(BcfViewpoint, TopicFolderImportCompletesVisualizationPinGuidWithTopic) {
  const auto snapshot = sampleSnapshot();

  const std::filesystem::path firstPath =
      std::filesystem::temp_directory_path() / "container_bcf_topic_a";
  const std::filesystem::path secondPath =
      std::filesystem::temp_directory_path() / "container_bcf_topic_b";
  std::error_code ec;
  std::filesystem::remove_all(firstPath, ec);
  std::filesystem::remove_all(secondPath, ec);
  std::filesystem::create_directories(firstPath, ec);
  std::filesystem::create_directories(secondPath, ec);

  const auto writeTopic = [&snapshot](const std::filesystem::path& path,
                                      std::string_view topicGuid) {
    std::ofstream markup(path / "markup.bcf", std::ios::binary);
    markup << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<Markup>\n"
              "  <Topic Guid=\""
           << topicGuid
           << "\">\n"
              "    <Title>Imported topic</Title>\n"
              "  </Topic>\n"
              "  <Viewpoints>\n"
              "    <ViewPoint Guid=\"viewpoint-a\" Viewpoint=\"viewpoint.bcfv\"/>\n"
              "  </Viewpoints>\n"
              "</Markup>\n";

    std::ofstream viewpoint(path / "viewpoint.bcfv", std::ios::binary);
    viewpoint << "<VisualizationInfo>\n"
                 "  <PerspectiveCamera>\n"
                 "    <CameraViewPoint><X>1</X><Y>2</Y><Z>3</Z></CameraViewPoint>\n"
                 "    <CameraDirection><X>0</X><Y>0</Y><Z>-1</Z></CameraDirection>\n"
                 "    <CameraUpVector><X>0</X><Y>1</Y><Z>0</Z></CameraUpVector>\n"
                 "  </PerspectiveCamera>\n"
                 "  <ContainerMarkup Guid=\"viewpoint-a\">\n"
                 "    <Pin Label=\"Door pin\" IfcGuid=\""
              << snapshot.selectedBimGuid << "\" AuthoringToolId=\""
              << snapshot.selectedBimSourceId
              << "\">\n"
                 "      <Location><X>1</X><Y>2</Y><Z>3</Z></Location>\n"
                 "    </Pin>\n"
                 "  </ContainerMarkup>\n"
                 "</VisualizationInfo>\n";
  };

  writeTopic(firstPath, "topic-folder-a");
  writeTopic(secondPath, "topic-folder-b");

  const auto loadedFirst = container::ui::bcf::LoadTopicFolder(firstPath);
  const auto loadedSecond = container::ui::bcf::LoadTopicFolder(secondPath);
  ASSERT_TRUE(loadedFirst.has_value());
  ASSERT_TRUE(loadedSecond.has_value());
  ASSERT_EQ(loadedFirst->viewpoints.size(), 1u);
  ASSERT_EQ(loadedSecond->viewpoints.size(), 1u);
  ASSERT_EQ(loadedFirst->viewpoints.front().markup.pins.size(), 1u);
  ASSERT_EQ(loadedSecond->viewpoints.front().markup.pins.size(), 1u);

  const std::string firstGuid =
      loadedFirst->viewpoints.front().markup.pins.front().guid;
  const std::string secondGuid =
      loadedSecond->viewpoints.front().markup.pins.front().guid;
  EXPECT_EQ(firstGuid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "topic-folder-a"));
  EXPECT_EQ(secondGuid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "topic-folder-b"));
  EXPECT_NE(firstGuid, secondGuid);

  std::filesystem::remove_all(firstPath, ec);
  std::filesystem::remove_all(secondPath, ec);
}

TEST(BcfViewpoint, SavesAndLoadsBcfArchive) {
  const auto snapshot = sampleSnapshot();
  auto secondSnapshot = snapshot;
  secondSnapshot.label = "Window review";
  secondSnapshot.selectedBimGuid = "4n1$window";
  secondSnapshot.selectedBimSourceId = "#44";
  secondSnapshot.camera.position = {10.0f, 11.0f, 12.0f};

  container::ui::bcf::BcfTopicFolder archive{};
  archive.markup.topic.guid = "archive-topic";
  archive.markup.topic.title = "Archive review";
  archive.markup.topic.status = "Open";
  archive.markup.topic.priority = "Normal";
  archive.markup.topic.labels = {"coordination", "envelope"};
  archive.markup.topic.tags = {"door", "window"};

  container::ui::bcf::BcfTopicViewpoint first{};
  first.markup.guid = "archive-viewpoint-a";
  first.markup.viewpointFile = "viewpoint-a.bcfv";
  first.markup.pins.push_back({
      .label = "Door pin",
      .hasLocation = true,
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });
  first.snapshot = snapshot;
  archive.viewpoints.push_back(first);

  container::ui::bcf::BcfTopicViewpoint second{};
  second.markup.guid = "archive-viewpoint-b";
  second.markup.viewpointFile = "viewpoint-b.bcfv";
  second.markup.pins.push_back({
      .label = "Window pin",
      .hasLocation = true,
      .location = {4.0f, 5.0f, 6.0f},
      .ifcGuid = secondSnapshot.selectedBimGuid,
      .sourceId = secondSnapshot.selectedBimSourceId,
  });
  second.snapshot = secondSnapshot;
  archive.viewpoints.push_back(second);

  archive.markup.comments.push_back({
      .guid = "archive-comment-a",
      .author = "Reviewer",
      .text = "Door archive comment",
      .viewpointGuid = "archive-viewpoint-a",
  });
  archive.markup.comments.push_back({
      .guid = "archive-comment-b",
      .author = "Reviewer",
      .text = "Window archive comment",
      .viewpointGuid = "archive-viewpoint-b",
  });

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_topic.bcf";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(container::ui::bcf::SaveBcfArchive(archive, path));
  EXPECT_TRUE(std::filesystem::exists(path));

  const auto loaded = container::ui::bcf::LoadBcfArchive(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->markup.topic.guid, "archive-topic");
  EXPECT_EQ(loaded->markup.topic.title, "Archive review");
  EXPECT_EQ(loaded->markup.topic.status, "Open");
  EXPECT_EQ(loaded->markup.topic.priority, "Normal");
  ASSERT_EQ(loaded->markup.topic.labels.size(), 2u);
  EXPECT_EQ(loaded->markup.topic.labels.front(), "coordination");
  ASSERT_EQ(loaded->markup.topic.tags.size(), 2u);
  EXPECT_EQ(loaded->markup.topic.tags.back(), "window");

  ASSERT_EQ(loaded->markup.comments.size(), 2u);
  EXPECT_EQ(loaded->markup.comments.front().viewpointGuid,
            "archive-viewpoint-a");
  EXPECT_EQ(loaded->markup.comments.back().viewpointGuid,
            "archive-viewpoint-b");

  ASSERT_EQ(loaded->viewpoints.size(), 2u);
  ASSERT_TRUE(loaded->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);
  ASSERT_TRUE(loaded->viewpoints.back().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.back().snapshot->selectedBimGuid,
            secondSnapshot.selectedBimGuid);

  ASSERT_EQ(loaded->viewpoints.front().markup.pins.size(), 1u);
  EXPECT_EQ(loaded->viewpoints.front().markup.pins.front().guid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "archive-topic"));
  EXPECT_EQ(loaded->viewpoints.front().markup.pins.front().ifcGuid,
            snapshot.selectedBimGuid);
  ASSERT_EQ(loaded->viewpoints.back().markup.pins.size(), 1u);
  EXPECT_EQ(loaded->viewpoints.back().markup.pins.front().sourceId,
            secondSnapshot.selectedBimSourceId);

  std::filesystem::remove(path, ec);
}

TEST(BcfViewpoint, ArchiveImportCompletesVisualizationPinGuidWithTopic) {
  const auto snapshot = sampleSnapshot();

  const std::filesystem::path firstPath =
      std::filesystem::temp_directory_path() / "container_bcf_archive_a.bcf";
  const std::filesystem::path secondPath =
      std::filesystem::temp_directory_path() / "container_bcf_archive_b.bcf";
  std::error_code ec;
  std::filesystem::remove(firstPath, ec);
  std::filesystem::remove(secondPath, ec);

  const auto writeArchive = [&snapshot](const std::filesystem::path& path,
                                        std::string_view topicGuid) {
    const std::string markup =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Markup>\n"
        "  <Topic Guid=\"" +
        std::string(topicGuid) +
        "\">\n"
        "    <Title>Imported archive</Title>\n"
        "  </Topic>\n"
        "  <Viewpoints>\n"
        "    <ViewPoint Guid=\"archive-viewpoint\" Viewpoint=\"viewpoint.bcfv\"/>\n"
        "  </Viewpoints>\n"
        "</Markup>\n";
    const std::string viewpoint =
        "<VisualizationInfo>\n"
        "  <PerspectiveCamera>\n"
        "    <CameraViewPoint><X>1</X><Y>2</Y><Z>3</Z></CameraViewPoint>\n"
        "    <CameraDirection><X>0</X><Y>0</Y><Z>-1</Z></CameraDirection>\n"
        "    <CameraUpVector><X>0</X><Y>1</Y><Z>0</Z></CameraUpVector>\n"
        "  </PerspectiveCamera>\n"
        "  <ContainerMarkup Guid=\"archive-viewpoint\">\n"
        "    <Pin Label=\"Door pin\" IfcGuid=\"" +
        snapshot.selectedBimGuid + "\" AuthoringToolId=\"" +
        snapshot.selectedBimSourceId +
        "\">\n"
        "      <Location><X>1</X><Y>2</Y><Z>3</Z></Location>\n"
        "    </Pin>\n"
        "  </ContainerMarkup>\n"
        "</VisualizationInfo>\n";

    mz_zip_archive zip{};
    const std::string archivePath = path.string();
    if (!mz_zip_writer_init_file(&zip, archivePath.c_str(), 0)) {
      return false;
    }
    bool ok = mz_zip_writer_add_mem(&zip, "topic/markup.bcf", markup.data(),
                                    markup.size(), MZ_BEST_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "topic/viewpoint.bcfv",
                                    viewpoint.data(), viewpoint.size(),
                                    MZ_BEST_COMPRESSION) &&
              mz_zip_writer_finalize_archive(&zip);
    ok = mz_zip_writer_end(&zip) && ok;
    return ok;
  };

  ASSERT_TRUE(writeArchive(firstPath, "archive-topic-a"));
  ASSERT_TRUE(writeArchive(secondPath, "archive-topic-b"));

  const auto loadedFirst = container::ui::bcf::LoadBcfArchive(firstPath);
  const auto loadedSecond = container::ui::bcf::LoadBcfArchive(secondPath);
  ASSERT_TRUE(loadedFirst.has_value());
  ASSERT_TRUE(loadedSecond.has_value());
  ASSERT_EQ(loadedFirst->viewpoints.size(), 1u);
  ASSERT_EQ(loadedSecond->viewpoints.size(), 1u);
  ASSERT_EQ(loadedFirst->viewpoints.front().markup.pins.size(), 1u);
  ASSERT_EQ(loadedSecond->viewpoints.front().markup.pins.size(), 1u);

  const std::string firstGuid =
      loadedFirst->viewpoints.front().markup.pins.front().guid;
  const std::string secondGuid =
      loadedSecond->viewpoints.front().markup.pins.front().guid;
  EXPECT_EQ(firstGuid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "archive-topic-a"));
  EXPECT_EQ(secondGuid,
            container::ui::bcf::StablePinGuid(
                snapshot.selectedBimGuid, snapshot.selectedBimSourceId,
                "archive-topic-b"));
  EXPECT_NE(firstGuid, secondGuid);

  std::filesystem::remove(firstPath, ec);
  std::filesystem::remove(secondPath, ec);
}

TEST(BcfViewpoint, AppendsCommentToLoadedArchiveAndPreservesViewpoints) {
  const auto snapshot = sampleSnapshot();
  auto secondSnapshot = snapshot;
  secondSnapshot.label = "Frame review";
  secondSnapshot.selectedBimGuid = "3n1$frame";
  secondSnapshot.selectedBimSourceId = "#43";
  secondSnapshot.camera.position = {7.0f, 8.0f, 9.0f};

  container::ui::bcf::BcfTopicFolder archive{};
  archive.markup.topic.guid = "append-topic";
  archive.markup.topic.title = "Append comment review";
  archive.markup.topic.status = "Open";

  container::ui::bcf::BcfTopicViewpoint first{};
  first.markup.guid = "append-viewpoint-a";
  first.markup.viewpointFile = "viewpoint-a.bcfv";
  first.markup.pins.push_back({
      .label = "Door pin",
      .hasLocation = true,
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = snapshot.selectedBimGuid,
      .sourceId = snapshot.selectedBimSourceId,
  });
  first.snapshot = snapshot;
  archive.viewpoints.push_back(first);

  container::ui::bcf::BcfTopicViewpoint second{};
  second.markup.guid = "append-viewpoint-b";
  second.markup.viewpointFile = "viewpoint-b.bcfv";
  second.snapshot = secondSnapshot;
  archive.viewpoints.push_back(second);

  archive.markup.comments.push_back({
      .guid = "append-comment-a",
      .author = "Reviewer",
      .text = "Initial archive comment",
      .viewpointGuid = "append-viewpoint-a",
  });

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_append.bcfzip";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(container::ui::bcf::SaveBcfArchive(archive, path));

  auto loaded = container::ui::bcf::LoadBcfArchive(path);
  ASSERT_TRUE(loaded.has_value());
  loaded->markup.comments.push_back({
      .guid = "append-comment-b",
      .author = "Coordinator",
      .text = "Follow-up after loading archive",
      .viewpointGuid = "append-viewpoint-b",
  });
  ASSERT_TRUE(container::ui::bcf::SaveBcfArchive(*loaded, path));

  const auto reloaded = container::ui::bcf::LoadBcfArchive(path);
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_EQ(reloaded->markup.comments.size(), 2u);
  EXPECT_EQ(reloaded->markup.comments.back().text,
            "Follow-up after loading archive");
  EXPECT_EQ(reloaded->markup.comments.back().viewpointGuid,
            "append-viewpoint-b");
  ASSERT_EQ(reloaded->viewpoints.size(), 2u);
  ASSERT_TRUE(reloaded->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(reloaded->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);
  ASSERT_TRUE(reloaded->viewpoints.back().snapshot.has_value());
  EXPECT_EQ(reloaded->viewpoints.back().snapshot->selectedBimGuid,
            secondSnapshot.selectedBimGuid);
  ASSERT_EQ(reloaded->viewpoints.front().markup.pins.size(), 1u);
  EXPECT_EQ(reloaded->viewpoints.front().markup.pins.front().label,
            "Door pin");

  std::filesystem::remove(path, ec);
}

TEST(BcfViewpoint, SanitizesUnsafeDotDotTopicGuidInArchivePath) {
  const auto snapshot = sampleSnapshot();

  container::ui::bcf::BcfTopicFolder archive{};
  archive.markup.topic.guid = "..";
  archive.markup.topic.title = "Unsafe topic guid";

  container::ui::bcf::BcfTopicViewpoint viewpoint{};
  viewpoint.markup.guid = "safe-viewpoint";
  viewpoint.markup.viewpointFile = "viewpoint.bcfv";
  viewpoint.snapshot = snapshot;
  archive.viewpoints.push_back(viewpoint);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "container_bcf_dotdot.bcfzip";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  ASSERT_TRUE(container::ui::bcf::SaveBcfArchive(archive, path));
  const std::vector<std::string> entries = archiveEntryNames(path);
  ASSERT_FALSE(entries.empty());
  EXPECT_NE(std::find(entries.begin(), entries.end(), "topic/markup.bcf"),
            entries.end());
  EXPECT_NE(std::find(entries.begin(), entries.end(), "topic/viewpoint.bcfv"),
            entries.end());
  for (const std::string& entry : entries) {
    EXPECT_EQ(entry.find(".."), std::string::npos);
    EXPECT_FALSE(entry.starts_with("./"));
    EXPECT_FALSE(entry.starts_with("/"));
  }

  const auto loaded = container::ui::bcf::LoadBcfArchive(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->markup.topic.guid, "..");
  ASSERT_EQ(loaded->viewpoints.size(), 1u);
  ASSERT_TRUE(loaded->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loaded->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);

  std::filesystem::remove(path, ec);
}

TEST(BcfViewpoint, ExportsPinsAsIssueOverlayMarkers) {
  container::ui::bcf::BcfMarkup markup{};
  markup.topic.guid = "overlay-topic";

  container::ui::bcf::BcfViewpointMarkup first{};
  first.pins.push_back({
      .guid = "pin-a",
      .label = "Door issue",
      .hasLocation = true,
      .location = {1.0f, 2.0f, 3.0f},
      .ifcGuid = "2n1$example",
      .sourceId = "#42",
  });
  markup.viewpoints.push_back(first);

  container::ui::bcf::BcfViewpointMarkup second{};
  second.pins.push_back({
      .guid = "pin-b",
      .label = "Frame issue",
      .hasLocation = true,
      .location = {4.0f, 5.0f, 6.0f},
      .ifcGuid = "3n1$frame",
      .sourceId = "#43",
  });
  markup.viewpoints.push_back(second);

  const auto issuePins =
      container::renderer::exportBcfPinsAsIssueOverlayMarkers(markup);

  ASSERT_EQ(issuePins.size(), 2u);
  EXPECT_EQ(issuePins.front().label, "Door issue");
  EXPECT_EQ(issuePins.front().position, glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(issuePins.front().ifcGuid, "2n1$example");
  EXPECT_EQ(issuePins.front().sourceId, "#42");
  EXPECT_EQ(issuePins.back().label, "Frame issue");
  EXPECT_EQ(issuePins.back().ifcGuid, "3n1$frame");
  EXPECT_EQ(issuePins.back().sourceId, "#43");

  container::renderer::BimCoordinationOverlayBuildOptions options{};
  options.spacesEnabled = false;
  options.mepXrayEnabled = false;
  options.clashesEnabled = false;
  options.issuePinsEnabled = true;
  const auto overlay = container::renderer::buildBimCoordinationOverlay(
      {.issuePins = issuePins, .options = options});

  ASSERT_EQ(overlay.markers.size(), 2u);
  EXPECT_EQ(overlay.markers.front().kind,
            container::renderer::BimCoordinationOverlayKind::IssuePin);
  EXPECT_EQ(overlay.markers.front().label, "Door issue");
  EXPECT_EQ(overlay.markers.front().position, glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(overlay.markers.front().ifcGuid, "2n1$example");
  EXPECT_EQ(overlay.markers.front().sourceId, "#42");
  EXPECT_EQ(overlay.markers.back().ifcGuid, "3n1$frame");
  EXPECT_EQ(overlay.markers.back().sourceId, "#43");
}

TEST(BcfViewpoint, SkipsIssueOverlayPinsWithoutValidLocation) {
  constexpr const char* kMarkupWithMalformedPin = R"xml(
<Markup>
  <Topic Guid="malformed-topic">
    <Title>Malformed pin</Title>
  </Topic>
  <Viewpoints>
    <ViewPoint Guid="viewpoint-a">
      <ContainerMarkup>
        <Pin Guid="pin-without-location" Label="Missing location"
             IfcGuid="2n1$example" AuthoringToolId="#42"/>
      </ContainerMarkup>
    </ViewPoint>
  </Viewpoints>
</Markup>
)xml";

  const auto imported =
      container::ui::bcf::ImportMarkup(kMarkupWithMalformedPin);
  ASSERT_TRUE(imported.has_value());
  ASSERT_EQ(imported->viewpoints.size(), 1u);
  ASSERT_EQ(imported->viewpoints.front().pins.size(), 1u);
  EXPECT_FALSE(imported->viewpoints.front().pins.front().hasLocation);

  const auto issuePins =
      container::renderer::exportBcfPinsAsIssueOverlayMarkers(*imported);

  EXPECT_TRUE(issuePins.empty());

  container::ui::bcf::BcfMarkup manualMarkup{};
  container::ui::bcf::BcfViewpointMarkup viewpoint{};
  viewpoint.pins.push_back({
      .guid = "manual-default-pin",
      .label = "Manual missing location",
      .ifcGuid = "2n1$example",
      .sourceId = "#42",
  });
  manualMarkup.viewpoints.push_back(viewpoint);

  const auto manualIssuePins =
      container::renderer::exportBcfPinsAsIssueOverlayMarkers(manualMarkup);
  EXPECT_TRUE(manualIssuePins.empty());
}

TEST(BcfViewpoint, RoundTripsMarkupPolylineAndSnapshot) {
  const auto snapshot = sampleSnapshot();

  container::ui::bcf::BcfTopicFolder topic{};
  topic.markup.topic.guid = "polyline-topic";
  topic.markup.topic.title = "Polyline review";

  container::ui::bcf::BcfTopicViewpoint viewpoint{};
  viewpoint.markup.guid = "polyline-viewpoint";
  viewpoint.markup.viewpointFile = "polyline-viewpoint.bcfv";
  viewpoint.markup.lines.push_back({
      .guid = "line-a",
      .start = {1.0f, 2.0f, 3.0f},
      .end = {4.0f, 5.0f, 6.0f},
      .color = {0.2f, 0.4f, 0.8f},
      .label = "Clearance path A",
  });
  viewpoint.markup.lines.push_back({
      .guid = "line-b",
      .start = {4.0f, 5.0f, 6.0f},
      .end = {7.0f, 8.0f, 9.0f},
      .color = {0.8f, 0.4f, 0.2f},
      .label = "Clearance path B",
  });
  viewpoint.snapshot = snapshot;
  topic.viewpoints.push_back(viewpoint);

  const std::string visualizationXml =
      container::ui::bcf::ExportVisualizationInfo(snapshot, viewpoint.markup);
  EXPECT_NE(visualizationXml.find("<MarkupLine"), std::string::npos);
  const auto visualizationMarkup =
      container::ui::bcf::ImportVisualizationInfoMarkup(visualizationXml);
  ASSERT_TRUE(visualizationMarkup.has_value());
  ASSERT_EQ(visualizationMarkup->lines.size(), 2u);
  EXPECT_EQ(visualizationMarkup->lines.front().guid, "line-a");
  EXPECT_EQ(visualizationMarkup->lines.front().label, "Clearance path A");
  EXPECT_EQ(visualizationMarkup->lines.front().start,
            glm::vec3(1.0f, 2.0f, 3.0f));
  EXPECT_EQ(visualizationMarkup->lines.front().end,
            glm::vec3(4.0f, 5.0f, 6.0f));
  EXPECT_EQ(visualizationMarkup->lines.front().color,
            glm::vec3(0.2f, 0.4f, 0.8f));

  const std::filesystem::path folderPath =
      std::filesystem::temp_directory_path() / "container_bcf_polyline_folder";
  std::error_code ec;
  std::filesystem::remove_all(folderPath, ec);
  ASSERT_TRUE(container::ui::bcf::SaveTopicFolder(topic, folderPath));

  const auto loadedFolder = container::ui::bcf::LoadTopicFolder(folderPath);
  ASSERT_TRUE(loadedFolder.has_value());
  ASSERT_EQ(loadedFolder->viewpoints.size(), 1u);
  ASSERT_TRUE(loadedFolder->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loadedFolder->viewpoints.front().snapshot->selectedBimGuid,
            snapshot.selectedBimGuid);
  ASSERT_EQ(loadedFolder->viewpoints.front().markup.lines.size(), 2u);
  EXPECT_EQ(loadedFolder->viewpoints.front().markup.lines.back().guid,
            "line-b");

  const std::filesystem::path archivePath =
      std::filesystem::temp_directory_path() /
      "container_bcf_polyline.bcfzip";
  std::filesystem::remove(archivePath, ec);
  ASSERT_TRUE(container::ui::bcf::SaveBcfArchive(topic, archivePath));

  const auto loadedArchive = container::ui::bcf::LoadBcfArchive(archivePath);
  ASSERT_TRUE(loadedArchive.has_value());
  ASSERT_EQ(loadedArchive->viewpoints.size(), 1u);
  ASSERT_TRUE(loadedArchive->viewpoints.front().snapshot.has_value());
  EXPECT_EQ(loadedArchive->viewpoints.front().snapshot->label, snapshot.label);
  ASSERT_EQ(loadedArchive->viewpoints.front().markup.lines.size(), 2u);
  EXPECT_EQ(loadedArchive->viewpoints.front().markup.lines.back().label,
            "Clearance path B");
  EXPECT_EQ(loadedArchive->viewpoints.front().markup.lines.back().end,
            glm::vec3(7.0f, 8.0f, 9.0f));

  std::filesystem::remove_all(folderPath, ec);
  std::filesystem::remove(archivePath, ec);
}
