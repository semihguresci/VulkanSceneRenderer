#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/RoboticsSceneManifest.h"

#include <gtest/gtest.h>
#include <glm/matrix.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace {

using namespace container::robotics;
namespace bim = container::geometry::dotbim;

std::string semanticIfcScene(bool millimetres) {
  const int unit = millimetres ? 1000 : 1;
  return std::format(R"ifc(ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,{},.METRE.);
#2=IFCCARTESIANPOINT(({}.,{}.,{}.));
#3=IFCDIRECTION((0.,0.,1.));
#4=IFCDIRECTION((0.,1.,0.));
#5=IFCDIRECTION((1.,0.,0.));
#6=IFCAXIS2PLACEMENT3D(#2,#3,#4);
#7=IFCLOCALPLACEMENT($,#6);
#8=IFCCARTESIANPOINT(({}.,0.,0.));
#9=IFCAXIS2PLACEMENT3D(#8,#3,#5);
#10=IFCLOCALPLACEMENT(#7,#9);
#20=IFCBUILDINGSTOREY('storey-guid',$,'Upper floor',$,$,#7,$,$,.ELEMENT.,$);
#21=IFCSPACE('room-guid',$,'Room 2',$,$,#10,$,$,.ELEMENT.,.INTERNAL.,$);
#22=IFCWALL('wall-guid',$,'Wall',$,$,#10,$,$);
#23=IFCOPENINGELEMENT('opening-guid',$,'Opening',$,$,#10,$,$);
#24=IFCDOOR('door-guid',$,'Door',$,$,#10,$,$,2.,1.);
#25=IFCSLAB('slab-guid',$,'Slab',$,$,$,$,$,.FLOOR.);
#30=IFCRELAGGREGATES('aggregates-guid',$,$,$,#20,(#21));
#31=IFCRELCONTAINEDINSPATIALSTRUCTURE('contains-guid',$,$,$,(#22,#24,#25),#21);
#32=IFCRELVOIDSELEMENT('voids-guid',$,$,$,#22,#23);
#33=IFCRELFILLSELEMENT('fills-guid',$,$,$,#23,#24);
ENDSEC;
END-ISO-10303-21;
)ifc", millimetres ? ".MILLI." : "$", unit, 2 * unit, 3 * unit, unit);
}

const SemanticEntity* bySource(const RoboticsScene& scene, std::string_view sourceId) {
  const auto found = std::ranges::find(scene.entities, sourceId, &SemanticEntity::sourceId);
  return found == scene.entities.end() ? nullptr : &*found;
}

bool hasRelationship(const RoboticsScene& scene, std::string_view from, std::string_view to,
                     SemanticRelationshipKind kind) {
  return std::ranges::any_of(scene.relationships, [=](const auto& edge) {
    return edge.from == from && edge.to == to && edge.kind == kind;
  });
}

TEST(BimRoboticsScene, Rep103PreservesHandednessAndAppliesMetricScaleOnce) {
  const glm::dmat4 toRos = rendererToRosTransform(0.001);
  EXPECT_EQ(toRos * glm::dvec4(0, 0, -1000, 0), glm::dvec4(1, 0, 0, 0));
  EXPECT_EQ(toRos * glm::dvec4(-1000, 0, 0, 0), glm::dvec4(0, 1, 0, 0));
  EXPECT_EQ(toRos * glm::dvec4(0, 1000, 0, 0), glm::dvec4(0, 0, 1, 0));
  EXPECT_GT(glm::determinant(toRos), 0.0);
  EXPECT_THROW((void)rendererToRosTransform(0), std::invalid_argument);
  EXPECT_THROW((void)rendererToRosTransform(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(BimRoboticsScene, NativeIfcUnitsAndNestedPlacementIncludeGeometrylessSemantics) {
  const auto metresModel = container::geometry::ifc::LoadFromStep(semanticIfcScene(false));
  const auto millimetresModel = container::geometry::ifc::LoadFromStep(semanticIfcScene(true), 2.0f);
  const auto metres = buildRoboticsScene(metresModel, {.sourceNamespace = "building"});
  const auto millimetres = buildRoboticsScene(millimetresModel, {.sourceNamespace = "building"});
  ASSERT_EQ(metres.entities.size(), 6u);
  ASSERT_EQ(millimetres.entities.size(), metres.entities.size());
  ASSERT_TRUE(millimetres.frame.metersPerRendererUnit);
  EXPECT_NEAR(*millimetres.frame.metersPerRendererUnit, 0.5, 1e-7);
  for (const std::string_view id : {"#20", "#21", "#22", "#23", "#24"}) {
    const auto* origin = bySource(metres, id);
    const auto* scaled = bySource(millimetres, id);
    ASSERT_TRUE(origin && scaled) << id;
    ASSERT_TRUE(origin->roboticsWorldFromLocal && scaled->roboticsWorldFromLocal) << id;
    EXPECT_EQ(origin->id, scaled->id);
    const glm::dvec4 expected = id == "#20" ? glm::dvec4(2, -1, 3, 1) : glm::dvec4(3, -1, 3, 1);
    for (glm::length_t axis = 0; axis < 4; ++axis) {
      EXPECT_NEAR((*origin->roboticsWorldFromLocal)[3][axis], expected[axis], 1e-11);
      EXPECT_NEAR((*scaled->roboticsWorldFromLocal)[3][axis], expected[axis], 1e-11);
    }
    EXPECT_TRUE(scaled->representations.empty());
  }
  const auto& storey = *bySource(metres, "#20");
  const auto& room = *bySource(metres, "#21");
  const auto& wall = *bySource(metres, "#22");
  const auto& opening = *bySource(metres, "#23");
  const auto& door = *bySource(metres, "#24");
  const auto* slab = bySource(metres, "#25");
  ASSERT_TRUE(slab);
  EXPECT_EQ(storey.kind, SemanticKind::Storey);
  EXPECT_EQ(room.kind, SemanticKind::Space);
  EXPECT_EQ(wall.kind, SemanticKind::Wall);
  EXPECT_EQ(opening.kind, SemanticKind::Opening);
  EXPECT_EQ(door.kind, SemanticKind::Door);
  EXPECT_EQ(slab->kind, SemanticKind::Slab);
  EXPECT_FALSE(slab->roboticsWorldFromLocal);
  EXPECT_EQ(wall.storeyIds, std::vector{storey.id});
  EXPECT_TRUE(hasRelationship(metres, wall.id, opening.id, SemanticRelationshipKind::Voids));
  EXPECT_TRUE(hasRelationship(metres, opening.id, door.id, SemanticRelationshipKind::Fills));
}

TEST(BimRoboticsScene, ManifestRoundTripsMirroredInstancesAndUnknownUnitEvidence) {
  glm::dmat4 mirrored(1);
  mirrored[0][0] = -2;
  mirrored[3] = {4, 2, 7, 1};
  auto model = bim::Model{};
  model.semanticProducts = {
      {.sourceId = "/building/A", .guid = "shared-guid", .type = "IFCWALLSTANDARDCASE",
       .name = "East wall", .transform = mirrored, .hasPlacement = true, .isComposedOccurrence = true},
      {.sourceId = "/building/B", .guid = "shared-guid", .type = "IFCWALLSTANDARDCASE",
       .name = "West wall", .transform = glm::dmat4(1), .hasPlacement = true, .isComposedOccurrence = true}};
  const auto known = buildRoboticsScene(model, {.sourceNamespace = "site", .metersPerRendererUnit = 0.5});
  ASSERT_EQ(known.entities.size(), 2u);
  const auto& first = *bySource(known, "/building/A");
  const auto& second = *bySource(known, "/building/B");
  EXPECT_NE(first.id, second.id);
  EXPECT_EQ(first.guid, second.guid);
  EXPECT_EQ(first.kind, SemanticKind::Wall);
  ASSERT_TRUE(first.roboticsWorldFromLocal);
  EXPECT_LT(glm::determinant(*first.roboticsWorldFromLocal), 0.0);
  EXPECT_EQ((*first.roboticsWorldFromLocal)[3], glm::dvec4(-3.5, -2, 1, 1));
  auto oneOccurrence = model;
  oneOccurrence.semanticProducts.pop_back();
  const auto single = buildRoboticsScene(oneOccurrence, {.sourceNamespace = "site", .metersPerRendererUnit = 0.5});
  ASSERT_EQ(single.entities.size(), 1u);
  EXPECT_EQ(single.entities.front().id, first.id);
  const std::string manifest = serializeRoboticsScene(known);
  EXPECT_EQ(serializeRoboticsScene(parseRoboticsScene(manifest)), manifest);
  std::ranges::reverse(model.semanticProducts);
  EXPECT_EQ(serializeRoboticsScene(buildRoboticsScene(model, {.sourceNamespace = "site", .metersPerRendererUnit = 0.5})),
            manifest);

  const auto unknown = buildRoboticsScene(model, {.sourceNamespace = "site"});
  EXPECT_FALSE(unknown.frame.metersPerRendererUnit);
  ASSERT_TRUE(bySource(unknown, "/building/A"));
  EXPECT_FALSE(bySource(unknown, "/building/A")->roboticsWorldFromLocal);
  EXPECT_FALSE(unknown.diagnostics.empty());
  const std::string unknownManifest = serializeRoboticsScene(unknown);
  EXPECT_EQ(serializeRoboticsScene(parseRoboticsScene(unknownManifest)), unknownManifest);

  auto invalid = nlohmann::json::parse(manifest);
  invalid["entities"][0]["roboticsWorldFromLocal"][3][0] = 12345.0;
  EXPECT_THROW((void)parseRoboticsScene(invalid.dump()), std::invalid_argument);
  invalid = nlohmann::json::parse(manifest);
  invalid["entities"][0]["rendererWorldFromLocal"][0] = {0, 0, 0, 0};
  EXPECT_THROW((void)parseRoboticsScene(invalid.dump()), std::invalid_argument);
  invalid = nlohmann::json::parse(manifest);
  invalid["relationships"].push_back({{"kind", "child"}, {"from", first.id}, {"to", "missing"}});
  EXPECT_THROW((void)parseRoboticsScene(invalid.dump()), std::invalid_argument);
  invalid.erase("relationships");
  invalid["version"] = 2;
  EXPECT_THROW((void)parseRoboticsScene(invalid.dump()), std::invalid_argument);
}

TEST(BimRoboticsScene, AShapeWithoutAnIfcProductDoesNotFabricateAPlacement) {
  const auto model = container::geometry::ifc::LoadFromStep(R"ifc(ISO-10303-21;
DATA;
#1=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#2=IFCTRIANGULATEDFACESET(#1,$,.F.,((1,2,3)),$);
ENDSEC;
END-ISO-10303-21;
)ifc");
  ASSERT_FALSE(model.elements.empty());
  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "geometry-only"});
  EXPECT_TRUE(scene.entities.empty());
  EXPECT_TRUE(std::ranges::any_of(scene.diagnostics, [](const auto& diagnostic) {
    return diagnostic.sourceId == "#2";
  }));
}

}  // namespace
