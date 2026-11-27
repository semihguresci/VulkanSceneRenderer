#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/robotics/BimCollisionGeometry.h"
#include "Container/robotics/RoboticsScene.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

namespace bim = container::geometry::dotbim;
using namespace container::robotics;

constexpr char wallWithOpening[] = R"ifc(ISO-10303-21;
DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINT((0.,0.,0.));
#21=IFCCARTESIANPOINT((1000.,0.,0.));
#22=IFCCARTESIANPOINT((1000.,100.,0.));
#23=IFCCARTESIANPOINT((0.,100.,0.));
#24=IFCPOLYLINE((#20,#21,#22,#23,#20));
#25=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#24);
#26=IFCEXTRUDEDAREASOLID(#25,#13,#11,1000.);
#27=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#26));
#28=IFCPRODUCTDEFINITIONSHAPE($,$,(#27));
#29=IFCWALL('wall-guid',$,$,$,$,#14,#28,$,$);
#30=IFCCARTESIANPOINT((250.,0.,250.));
#31=IFCAXIS2PLACEMENT3D(#30,#11,#12);
#32=IFCLOCALPLACEMENT(#14,#31);
#40=IFCCARTESIANPOINT((0.,0.,0.));
#41=IFCCARTESIANPOINT((500.,0.,0.));
#42=IFCCARTESIANPOINT((500.,100.,0.));
#43=IFCCARTESIANPOINT((0.,100.,0.));
#44=IFCPOLYLINE((#40,#41,#42,#43,#40));
#45=IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,#44);
#46=IFCEXTRUDEDAREASOLID(#45,#13,#11,500.);
#47=IFCSHAPEREPRESENTATION($,'Body','SweptSolid',(#46));
#48=IFCPRODUCTDEFINITIONSHAPE($,$,(#47));
#49=IFCOPENINGELEMENT('opening-guid',$,$,$,$,#32,#48,$);
#50=IFCRELVOIDSELEMENT('void-guid',$,$,$,#29,#49);
ENDSEC;
END-ISO-10303-21;
)ifc";

bool intersectsWall(const BimCollider& wall, double y, double z) {
  const glm::dvec3 start(-1, y, z), direction(1, 0, 0);
  const auto& mesh = *wall.mesh;
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    const auto a = wall.originMeters + glm::dvec3(mesh.vertices[mesh.indices[i]]);
    const auto ab = wall.originMeters + glm::dvec3(mesh.vertices[mesh.indices[i + 1]]) - a;
    const auto ac = wall.originMeters + glm::dvec3(mesh.vertices[mesh.indices[i + 2]]) - a;
    const auto edge = glm::cross(direction, ac);
    const double determinant = glm::dot(ab, edge);
    if (std::abs(determinant) < 1e-9)
      continue;
    const auto fromA = start - a;
    const double u = glm::dot(fromA, edge) / determinant;
    const auto cross = glm::cross(fromA, ab);
    const double v = glm::dot(direction, cross) / determinant;
    const double distance = glm::dot(ac, cross) / determinant;
    if (u >= 0 && v >= 0 && u + v <= 1 && distance >= 0 && distance <= 2)
      return true;
  }
  return false;
}

TEST(BimCollisionGeometry, PreservesNativeHostOpeningAndExcludesVirtualOpeningGeometry) {
  const auto model = container::geometry::ifc::LoadFromStep(wallWithOpening);
  const auto scene = buildRoboticsScene(model, {.sourceNamespace = "opening"});
  const auto collision = cookBimCollisionGeometry(model, scene);
  ASSERT_EQ(collision.colliders.size(), 1u);
  const auto& wall = collision.colliders.front();
  EXPECT_EQ(wall.kind, BimColliderKind::StaticMesh);
  EXPECT_FALSE(wall.supportCandidate);
  ASSERT_TRUE(wall.mesh);
  EXPECT_EQ(wall.mesh->indices.size(), model.meshRanges.front().indexCount);
  EXPECT_TRUE(intersectsWall(wall, -.12, .47));
  EXPECT_FALSE(intersectsWall(wall, -.52, .47));
}

bim::Model meshFixture() {
  bim::Model model;
  model.vertices.resize(4);
  model.vertices[1].position = {2, 0, 0};
  model.vertices[2].position = {0, 1, 0};
  model.vertices[3].position = {0, 0, 3};
  model.indices = {0, 1, 2, 0, 3, 1};
  model.meshRanges = {{.meshId = 7, .indexCount = 6}};
  return model;
}

SemanticEntity entity(std::string id, std::string type, glm::dmat4 world) {
  return {.id = id,
          .ifcClass = type,
          .kind = classifySemanticKind(type),
          .representations = {{.sourceId = id + "/mesh", .meshId = 7,
                               .roboticsWorldFromLocal = world}}};
}

TEST(BimCollisionGeometry, ReusesLocalMeshAcrossTranslationsAndPreservesMirroredWinding) {
  const auto model = meshFixture();
  glm::dmat4 placement(1);
  placement[0].x = -2;
  placement[1] = {1, 3, 0, 0};
  placement[3] = {1e9, -2e9, 6.5, 1};
  auto translated = placement;
  translated[3].z = 8.5;
  auto different = placement;
  different[1].x = 2;
  const RoboticsScene scene{
      .frame = {.metersPerRendererUnit = 1.0},
      .entities = {entity("slab", "IfcSlab", placement), entity("ramp", "IfcRampFlight", translated),
                   entity("stair", "IfcStairFlight", different)}};

  const auto collision = cookBimCollisionGeometry(model, scene);
  ASSERT_EQ(collision.colliders.size(), 3u);
  ASSERT_TRUE(collision.diagnostics.empty());
  const auto& ramp = collision.colliders[0];
  const auto& slab = collision.colliders[1];
  const auto& stair = collision.colliders[2];
  ASSERT_TRUE(ramp.mesh && slab.mesh && stair.mesh);
  EXPECT_EQ(ramp.mesh.get(), slab.mesh.get());
  EXPECT_NE(stair.mesh.get(), slab.mesh.get());
  EXPECT_TRUE(ramp.supportCandidate && slab.supportCandidate && stair.supportCandidate);
  EXPECT_EQ(ramp.originMeters - slab.originMeters, glm::dvec3(0, 0, 2));
  const auto& mesh = *slab.mesh;
  EXPECT_EQ(mesh.vertices.size(), 4u);
  const auto a = mesh.vertices[mesh.indices[0]], b = mesh.vertices[mesh.indices[1]],
             c = mesh.vertices[mesh.indices[2]];
  EXPECT_GT(glm::cross(b - a, c - a).z, 0.f);
  EXPECT_NEAR(slab.originMeters.z + a.z, 6.5, 1e-12);
}

TEST(BimCollisionGeometry, SeparatesDoorMeshesFromDynamicBoxes) {
  const auto model = meshFixture();
  const RoboticsScene scene{
      .frame = {.metersPerRendererUnit = 1.0},
      .entities = {entity("prop", "IfcFurniture", glm::dmat4(1)),
                   entity("door", "IfcDoor", glm::dmat4(1)),
                   entity("space", "IfcSpace", glm::dmat4(1))}};
  const auto collision = cookBimCollisionGeometry(model, scene);
  ASSERT_EQ(collision.colliders.size(), 2u);
  ASSERT_TRUE(collision.diagnostics.empty());
  EXPECT_EQ(collision.colliders[0].kind, BimColliderKind::DoorMesh);
  EXPECT_TRUE(collision.colliders[0].mesh);
  EXPECT_EQ(collision.colliders[1].kind, BimColliderKind::DynamicBox);
  EXPECT_FALSE(collision.colliders[1].mesh);
  EXPECT_EQ(collision.colliders[1].originMeters, glm::dvec3(1, .5, 1.5));
  EXPECT_EQ(collision.colliders[1].halfExtents, glm::vec3(1, .5, 1.5));
}

TEST(BimCollisionGeometry, DiagnosesInvalidMeshesAndUnknownUnitsAndEnforcesBudgets) {
  auto model = meshFixture();
  RoboticsScene scene{.frame = {.metersPerRendererUnit = 1.0},
                      .entities = {entity("wall", "IfcWall", glm::dmat4(1))}};
  EXPECT_THROW((void)cookBimCollisionGeometry(model, scene, {.maxTriangleReferences = 1}),
               std::length_error);
  model.indices[0] = 1000;
  const auto invalid = cookBimCollisionGeometry(model, scene);
  EXPECT_TRUE(invalid.colliders.empty());
  ASSERT_FALSE(invalid.diagnostics.empty());
  EXPECT_EQ(invalid.diagnostics.front().code, "invalid-collision-geometry");
  scene.frame.metersPerRendererUnit.reset();
  const auto unknown = cookBimCollisionGeometry(model, scene);
  EXPECT_TRUE(unknown.colliders.empty());
  ASSERT_EQ(unknown.diagnostics.size(), 1u);
  EXPECT_EQ(unknown.diagnostics.front().code, "metric-units-unavailable");
}

} // namespace
