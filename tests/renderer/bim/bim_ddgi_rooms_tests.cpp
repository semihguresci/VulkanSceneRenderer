#include "Container/renderer/bim/BimDdgiRooms.h"

#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/utility/ModelImportCancellation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace container::renderer {
namespace {

const DdgiBimRoom &roomByIdentity(const BimDdgiRoomExtraction &result,
                                 std::string_view identity) {
  const auto found = std::ranges::find(result.rooms, identity, &DdgiBimRoom::identity);
  if (found == result.rooms.end()) throw std::logic_error("Fixture room not found");
  return *found;
}

TEST(BimDdgiRooms, ExtractsMappedIfcSpaceAndUnrepresentedStoreySpace) {
  constexpr auto step = R"ifc(
ISO-10303-21; DATA;
#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);
#10=IFCCARTESIANPOINT((0.,0.,0.));
#11=IFCDIRECTION((0.,0.,1.));
#12=IFCDIRECTION((1.,0.,0.));
#13=IFCAXIS2PLACEMENT3D(#10,#11,#12);
#14=IFCLOCALPLACEMENT($,#13);
#20=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1000.,0.,0.),(0.,2000.,0.),(0.,0.,3000.)));
#21=IFCTRIANGULATEDFACESET(#20,$,.T.,((1,3,2),(1,2,4),(1,4,3),(2,3,4)),$);
#30=IFCSHAPEREPRESENTATION($,'Body','Tessellation',(#21));
#31=IFCREPRESENTATIONMAP(#13,#30);
#40=IFCDIRECTION((0.,1.,0.));
#41=IFCDIRECTION((-1.,0.,0.));
#42=IFCCARTESIANPOINT((6000.,0.,4000.));
#44=IFCCARTESIANTRANSFORMATIONOPERATOR3D(#40,#41,#42,2.,#11);
#50=IFCMAPPEDITEM(#31,#44);
#51=IFCSHAPEREPRESENTATION($,'Body','MappedRepresentation',(#50));
#52=IFCPRODUCTDEFINITIONSHAPE($,$,(#51));
#60=IFCSPACE('mapped-guid',$,'Mapped room',$,$,#14,#52,$,$);
#61=IFCSPACE('missing-guid',$,'Missing room',$,$,#14,$,$,$);
#70=IFCBUILDINGSTOREY('upper-guid',$,'Second floor',$,$,#14,$,$,4000.);
#71=IFCBUILDINGSTOREY('lower-guid',$,'Ground floor',$,$,#14,$,$,0.);
#72=IFCRELAGGREGATES('relation-a',$,$,$,#70,(#60));
#73=IFCRELAGGREGATES('relation-b',$,$,$,#71,(#61));
ENDSEC; END-ISO-10303-21;
)ifc";
  const auto imported = geometry::ifc::LoadFromStep(step, 0.5f);
  ASSERT_EQ(imported.elements.size(), 1u);
  ASSERT_EQ(imported.meshRanges.size(), 1u);
  ASSERT_EQ(imported.spacesWithoutDrawableGeometry.size(), 1u);
  EXPECT_EQ(imported.spacesWithoutDrawableGeometry.front().sourceId, "#61");
  EXPECT_FLOAT_EQ(imported.unitMetadata.metersPerUnit, 0.001f);
  EXPECT_FLOAT_EQ(imported.unitMetadata.effectiveImportScale, 0.0005f);
  const auto &element = imported.elements.front();
  const auto &range = imported.meshRanges.front();
  const std::array metadata{BimElementMetadata{.objectIndex = 0, .guid = element.guid,
      .type = element.type, .displayName = element.displayName, .storeyName = element.storeyName,
      .storeyId = element.storeyId, .sourceId = element.sourceId}};
  // Mimic a live placement update after import; cached imported bounds cannot
  // account for the added current-world translation.
  std::array objects{gpu::ObjectData{.model = element.transform}};
  objects.front().model[3] += glm::vec4(1, 2, 3, 0);
  const std::array draws{DrawCommand{.firstIndex = range.firstIndex, .indexCount = range.indexCount}};
  const auto result = ExtractBimDdgiRooms({.metadata = metadata, .vertices = imported.vertices,
      .indices = imported.indices, .objects = objects, .opaqueDraws = draws,
      .missingSpaces = imported.spacesWithoutDrawableGeometry});
  ASSERT_EQ(result.rooms.size(), 2u);
  const auto &mapped = roomByIdentity(result, "mapped-guid");
  EXPECT_EQ(mapped.storey, "Second floor");
  EXPECT_EQ(mapped.storeyIdentity, "upper-guid");
  ASSERT_TRUE(mapped.bounds.valid);
  EXPECT_NEAR(mapped.bounds.minimum.x, 2, 1e-5f);
  EXPECT_NEAR(mapped.bounds.minimum.y, 4, 1e-5f);
  EXPECT_NEAR(mapped.bounds.minimum.z, 2, 1e-5f);
  EXPECT_NEAR(mapped.bounds.maximum.x, 4, 1e-5f);
  EXPECT_NEAR(mapped.bounds.maximum.y, 7, 1e-5f);
  EXPECT_NEAR(mapped.bounds.maximum.z, 3, 1e-5f);
  const auto &missing = roomByIdentity(result, "missing-guid");
  EXPECT_EQ(missing.storey, "Ground floor");
  EXPECT_FALSE(missing.bounds.valid);
  EXPECT_NE(missing.reason.find("representation"), std::string::npos);
  EXPECT_EQ(result.missingRooms, 1u);
  EXPECT_EQ(result.ambiguousRooms, 0u);
}

TEST(BimDdgiRooms, UnionsIndexedFragmentsAndReportsAmbiguousAndCurveOnlyRooms) {
  const std::array vertices{geometry::Vertex{.position = {1, 0, 0}},
      geometry::Vertex{.position = {0, 2, 0}}, geometry::Vertex{.position = {0, 0, 3}},
      geometry::Vertex{.position = {500, 500, 500}}};
  const std::array indices{0u, 1u, 2u};
  std::array<gpu::ObjectData, 5> objects;
  objects[1].model[3].x = 4;
  const std::array metadata{
      BimElementMetadata{.objectIndex = 0, .guid = "joined", .type = "IfcSpace"},
      BimElementMetadata{.objectIndex = 1, .guid = "joined", .type = "IfcSpace"},
      BimElementMetadata{.objectIndex = 2, .guid = "conflict", .type = "Room", .storeyId = "a"},
      BimElementMetadata{.objectIndex = 3, .guid = "conflict", .type = "Room", .storeyId = "b"},
      BimElementMetadata{.objectIndex = 4, .guid = "curve", .type = "Space",
                         .geometryKind = BimGeometryKind::Curves}};
  const std::array opaque{DrawCommand{.objectIndex = 0, .indexCount = 3}};
  const std::array transparent{DrawCommand{.objectIndex = 1, .indexCount = 3, .instanceCount = 4}};
  const std::array relationNodes{BimRelationshipNode{.guid = "storey-2", .label = "Second floor",
                                                    .ifcClass = "IfcBuildingStorey"},
                                   BimRelationshipNode{.objectIndex = 1, .ifcClass = "IfcSpace"}};
  const std::array edges{BimRelationshipEdge{.from = 0, .to = 1, .kind = BimRelationshipKind::SpatialParent}};
  const BimDdgiRoomSource source{.metadata = metadata, .vertices = vertices, .indices = indices,
      .objects = objects, .opaqueDraws = opaque, .transparentDraws = transparent,
      .relationshipNodes = relationNodes, .relationships = edges};
  const auto result = ExtractBimDdgiRooms(source);
  ASSERT_EQ(result.rooms.size(), 3u);
  const auto &joined = roomByIdentity(result, "joined");
  EXPECT_EQ(joined.storey, "Second floor");
  EXPECT_EQ(joined.storeyIdentity, "storey-2");
  EXPECT_EQ(joined.bounds.minimum, (glm::vec3{0, 0, 0}));
  EXPECT_EQ(joined.bounds.maximum, (glm::vec3{5, 2, 3}));
  const auto &conflicting = roomByIdentity(result, "conflict");
  EXPECT_TRUE(conflicting.ambiguous);
  EXPECT_NE(conflicting.reason.find("storey"), std::string::npos);
  const auto &curve = roomByIdentity(result, "curve");
  EXPECT_FALSE(curve.bounds.valid);
  EXPECT_NE(curve.reason.find("curve"), std::string::npos);
  EXPECT_EQ(result.ambiguousRooms, 1u);
  EXPECT_EQ(result.missingRooms, 1u);
  std::stop_source cancelled;
  cancelled.request_stop();
  EXPECT_THROW((void)ExtractBimDdgiRooms(source, cancelled.get_token()),
               container::util::ModelImportCancelled);
}

} // namespace
} // namespace container::renderer
