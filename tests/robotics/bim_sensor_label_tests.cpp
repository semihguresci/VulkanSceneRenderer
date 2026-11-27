#include "Container/geometry/DotBimLoader.h"
#include "Container/robotics/BimSensorLabels.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace container::robotics;
namespace bim = container::geometry::dotbim;

TEST(BimSensorLabels, RepeatedGuidsAndRepresentationsFollowOccurrenceAcrossDrawOrder) {
  bim::Model model;
  model.semanticProducts = {
      {.sourceId = "/floor/a", .guid = "shared-wall-guid", .type = "IfcWall",
       .isComposedOccurrence = true},
      {.sourceId = "/floor/b", .guid = "shared-wall-guid", .type = "ifcWall",
       .isComposedOccurrence = true},
      {.sourceId = "/floor/door", .guid = "door-guid", .type = "IfcDoor"},
  };
  model.elements = {
      {.meshId = 6, .guid = "shared-wall-guid", .sourceId = "/defined/body",
       .semanticSourceId = "/floor/b"},
      {.meshId = 7, .guid = "door-guid", .sourceId = "/defined/door-body",
       .semanticSourceId = "/floor/door"},
      {.meshId = 6, .guid = "shared-wall-guid", .sourceId = "/defined/body",
       .semanticSourceId = "/floor/a"},
      {.meshId = 8, .geometryKind = bim::GeometryKind::Curves, .guid = "shared-wall-guid",
       .sourceId = "/defined/trim", .semanticSourceId = "/floor/a"},
  };
  auto scene = buildRoboticsScene(model, {.sourceNamespace = "test-bim", .metersPerRendererUnit = 0.001});
  const auto labels = buildBimSensorLabels(model, scene);
  ASSERT_EQ(labels.elements.size(), 4u);
  EXPECT_NE(labels.elements[0].instanceId, 0u);
  EXPECT_NE(labels.elements[0].instanceId, labels.elements[2].instanceId);
  EXPECT_EQ(labels.elements[0].semanticId, labels.elements[2].semanticId);
  EXPECT_EQ(labels.elements[2], labels.elements[3]);
  EXPECT_NE(labels.elements[1].semanticId, labels.elements[2].semanticId);
  ASSERT_EQ(labels.classes.size(), 3u);
  EXPECT_EQ(labels.classes.at(labels.elements[2].semanticId - 1).ifcClass, "IFCWALL");
  const auto& wall = labels.instances.at(labels.elements[2].instanceId - 1);
  EXPECT_EQ(wall.guid, "shared-wall-guid");
  EXPECT_EQ(wall.occurrenceId, "/floor/a");
  EXPECT_EQ(wall.representationIds, (std::vector<std::string>{"/defined/body", "/defined/trim"}));

  std::ranges::reverse(model.elements);
  std::ranges::reverse(scene.entities);
  for (auto& entity : scene.entities)
    std::ranges::reverse(entity.representations);
  const auto reordered = buildBimSensorLabels(model, scene);
  EXPECT_TRUE(std::ranges::equal(labels.elements, reordered.elements | std::views::reverse));
  const auto manifest = serializeBimSensorLabelManifest(labels);
  EXPECT_EQ(manifest, serializeBimSensorLabelManifest(reordered));
  const auto parsed = nlohmann::json::parse(manifest);
  EXPECT_EQ(parsed.at("schema"), "container.bim-sensor-labels");
  EXPECT_EQ(parsed.at("backgroundId"), 0);
  EXPECT_DOUBLE_EQ(parsed.at("metersPerRendererUnit").get<double>(), 0.001);
  EXPECT_FALSE(parsed.contains("elements"));
}

TEST(BimSensorLabels, UnresolvedRowsRemainBackgroundAndStandaloneRowsKeepIdentity) {
  bim::Model model;
  model.elements = {
      {.meshId = 1, .guid = "legacy-render-guid", .sourceId = "/standalone", .isRepresentationOnly = true},
      {.meshId = 7, .sourceId = "/standalone", .isRepresentationOnly = true},
      {.meshId = 2, .sourceId = "/conflict", .isRepresentationOnly = true},
      {.meshId = 3, .sourceId = "/conflict", .isRepresentationOnly = true},
      {.meshId = 4, .sourceId = "/ambiguous"},
      {.meshId = 5, .sourceId = "/unmatched"},
  };
  model.elements[3].transform[3].x = 1.0f;
  RoboticsScene scene;
  scene.sourceNamespace = "ambiguous-scene";
  scene.entities = {
      {.id = "first", .sourceId = "/first", .guid = "shared", .ifcClass = "IfcWall",
       .representations = {{.sourceId = "/ambiguous", .meshId = 4}}},
      {.id = "second", .sourceId = "/second", .guid = "shared", .ifcClass = "IfcWall",
       .representations = {{.sourceId = "/ambiguous", .meshId = 4}}},
  };
  const auto labels = buildBimSensorLabels(model, scene);
  ASSERT_EQ(labels.elements.size(), 6u);
  EXPECT_NE(labels.elements[0].instanceId, 0u);
  EXPECT_EQ(labels.elements[0].semanticId, 1u);
  EXPECT_EQ(labels.elements[0], labels.elements[1]);
  EXPECT_EQ(labels.instances.at(labels.elements[0].instanceId - 1).guid, "");
  for (size_t i = 2; i < labels.elements.size(); ++i)
    EXPECT_EQ(labels.elements[i], BimSensorLabel{});
  for (const auto code : {"ambiguous-element", "unmatched-element", "ambiguous-standalone-representation"})
    EXPECT_NE(std::ranges::find(labels.diagnostics, std::string_view(code), &RoboticsDiagnostic::code),
              labels.diagnostics.end());
  std::ranges::reverse(scene.entities);
  EXPECT_EQ(serializeBimSensorLabelManifest(labels),
            serializeBimSensorLabelManifest(buildBimSensorLabels(model, scene)));

  scene.entities.back().id = scene.entities.front().id;
  EXPECT_THROW((void)buildBimSensorLabels(model, scene), std::invalid_argument);
}

} // namespace
