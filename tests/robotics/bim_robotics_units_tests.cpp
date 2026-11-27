#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <string_view>

namespace {

namespace bim = container::geometry::dotbim;
using namespace container::robotics;

bim::Model importProject(std::string_view declarations, std::string_view assignedUnits) {
  const auto step = std::format(R"ifc(ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.);
{}
#4=IFCUNITASSIGNMENT(({}));
#5=IFCPROJECT('project-guid',$,'Unit fixture',$,$,$,$,$,#4);
#10=IFCCARTESIANPOINT((4.,10.,3.));
#13=IFCAXIS2PLACEMENT3D(#10,$,$);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.F.,((1,2,3)),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCPRODUCTDEFINITIONSHAPE($,$,(#30));
#40=IFCWALL('wall-guid',$,'Wall',$,$,#14,#31,$,$);
ENDSEC;
END-ISO-10303-21;
)ifc", declarations, assignedUnits);
  return container::geometry::ifc::LoadFromStep(step);
}

const SemanticEntity* findWall(const RoboticsScene& scene) {
  const auto wall = std::ranges::find(scene.entities, std::string_view("#40"),
                                      &SemanticEntity::sourceId);
  return wall == scene.entities.end() ? nullptr : &*wall;
}

void expectPlacementInMetres(const bim::Model& model, double metresPerSourceUnit) {
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements.front().transform[3], glm::vec4(4, 3, -10, 1));

  ASSERT_TRUE(model.semanticUnitContext);
  ASSERT_TRUE(model.semanticUnitContext->sourceMetersPerUnit);
  ASSERT_TRUE(model.semanticUnitContext->metersPerImportedUnit);
  EXPECT_DOUBLE_EQ(*model.semanticUnitContext->sourceMetersPerUnit, metresPerSourceUnit);
  EXPECT_DOUBLE_EQ(*model.semanticUnitContext->metersPerImportedUnit, metresPerSourceUnit);

  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "unit-fixture"});
  ASSERT_TRUE(scene.frame.metersPerRendererUnit);
  EXPECT_DOUBLE_EQ(*scene.frame.metersPerRendererUnit, metresPerSourceUnit);
  const auto* wall = findWall(scene);
  ASSERT_NE(wall, nullptr);
  ASSERT_TRUE(wall->roboticsWorldFromLocal);
  ASSERT_EQ(wall->representations.size(), 1u);
  ASSERT_TRUE(wall->representations.front().roboticsWorldFromLocal);
  EXPECT_EQ(*wall->roboticsWorldFromLocal, *wall->representations.front().roboticsWorldFromLocal);
  const auto& position = (*wall->roboticsWorldFromLocal)[3];
  EXPECT_NEAR(position.x, 10 * metresPerSourceUnit, 1e-13);
  EXPECT_NEAR(position.y, -4 * metresPerSourceUnit, 1e-13);
  EXPECT_NEAR(position.z, 3 * metresPerSourceUnit, 1e-13);
}

TEST(BimRoboticsUnits, AssignedSiLengthOverridesEarlierUnassignedMetre) {
  const auto model = importProject("#2=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);", "#2");
  expectPlacementInMetres(model, 0.001);
}

TEST(BimRoboticsUnits, ConversionBasedProjectLengthUsesItsAuthoredFactor) {
  const auto model = importProject(R"ifc(
#2=IFCMEASUREWITHUNIT(IFCLENGTHMEASURE(0.3048),#1);
#3=IFCCONVERSIONBASEDUNIT($,.LENGTHUNIT.,'foot',#2);
)ifc", "#3");
  expectPlacementInMetres(model, 0.3048);
}

TEST(BimRoboticsUnits, UnsupportedOrAmbiguousAssignmentCannotInheritBaseMetre) {
  constexpr std::string_view declarations = R"ifc(
#2=IFCMEASUREWITHUNIT(IFCLENGTHMEASURE(0.3048),#1);
#3=IFCCONVERSIONBASEDUNITWITHOFFSET($,.LENGTHUNIT.,'unsupported',#2,1.);
)ifc";
  for (const std::string_view assignment : {"#3", "#1,#3"}) {
    SCOPED_TRACE(assignment);
    const auto model = importProject(declarations, assignment);
    ASSERT_EQ(model.elements.size(), 1u);
    EXPECT_EQ(model.elements.front().transform[3], glm::vec4(4, 3, -10, 1));
    ASSERT_TRUE(model.unitMetadata.hasMetersPerUnit);
    EXPECT_FLOAT_EQ(model.unitMetadata.metersPerUnit, 1.f);
    ASSERT_TRUE(model.semanticUnitContext);
    EXPECT_FALSE(model.semanticUnitContext->sourceMetersPerUnit);
    EXPECT_FALSE(model.semanticUnitContext->metersPerImportedUnit);

    const auto scene = buildRoboticsScene(model, {.sourceNamespace = "unit-fixture"});
    EXPECT_FALSE(scene.frame.metersPerRendererUnit);
    EXPECT_FALSE(scene.frame.sourceMetersPerUnit);
    const auto* wall = findWall(scene);
    ASSERT_NE(wall, nullptr);
    EXPECT_TRUE(wall->rendererWorldFromLocal);
    EXPECT_FALSE(wall->roboticsWorldFromLocal);
    ASSERT_EQ(wall->representations.size(), 1u);
    EXPECT_FALSE(wall->representations.front().roboticsWorldFromLocal);
  }
}

} // namespace
