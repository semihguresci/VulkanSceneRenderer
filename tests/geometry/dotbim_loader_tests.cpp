#include "Container/geometry/DotBimLoader.h"
#include "Container/utility/ModelImportCancellation.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

TEST(DotBimLoader, CancelledImportDoesNotReadOrParse) {
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto stop = cancellation.get_token();
  EXPECT_THROW((void)container::geometry::dotbim::LoadFromJson("invalid source", 1.0f, stop),
               container::util::ModelImportCancelled);
  EXPECT_THROW((void)container::geometry::dotbim::LoadFromFile("", 1.0f, stop),
               container::util::ModelImportCancelled);
}

TEST(DotBimLoader, ParsesMeshElementTransformAndColor) {
  constexpr const char *kDotBimJson = R"json(
{
  "schema_version": "1.1.0",
  "meshes": [
    {
      "mesh_id": 7,
      "coordinates": [0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 3.0, 0.0],
      "indices": [0, 1, 2]
    }
  ],
  "elements": [
    {
      "mesh_id": 7,
      "guid": "element-1",
      "type": "Wall",
      "displayName": "North wall",
      "objectType": "Basic wall",
      "storeyName": "Level 01",
      "storeyId": "storey-1",
      "materialName": "Concrete",
      "materialCategory": "Structural",
      "discipline": "Architecture",
      "phase": "New construction",
      "fireRating": "2h",
      "loadBearing": true,
      "status": "Existing",
      "sourceId": "#42",
      "vector": {"x": 4.0, "y": 5.0, "z": 6.0},
      "rotation": {"qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0},
      "color": {"r": 10, "g": 20, "b": 30, "a": 255},
      "properties": [
        {
          "set": "Pset_WallCommon",
          "name": "AcousticRating",
          "value": "Rw40",
          "category": "pset"
        }
      ]
    }
  ],
  "info": {
    "georeference": {
      "sourceUpAxis": "Z",
      "crsName": "EPSG test CRS",
      "crsAuthority": "EPSG",
      "crsCode": "1234",
      "coordinateOffset": [1000.0, 2000.0, 12.5],
      "mapConversionName": "IfcMapConversion"
    }
  }
}
)json";

  const auto model =
      container::geometry::dotbim::LoadFromJson(kDotBimJson, 2.0f);

  ASSERT_EQ(model.vertices.size(), 3u);
  ASSERT_EQ(model.indices.size(), 3u);
  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);

  EXPECT_FALSE(model.unitMetadata.hasSourceUnits);
  EXPECT_FALSE(model.unitMetadata.hasMetersPerUnit);
  EXPECT_TRUE(model.unitMetadata.hasImportScale);
  EXPECT_NEAR(model.unitMetadata.importScale, 2.0f, 1.0e-6f);
  EXPECT_TRUE(model.unitMetadata.hasEffectiveImportScale);
  EXPECT_NEAR(model.unitMetadata.effectiveImportScale, 2.0f, 1.0e-6f);
  EXPECT_TRUE(model.georeferenceMetadata.hasSourceUpAxis);
  EXPECT_EQ(model.georeferenceMetadata.sourceUpAxis, "Z");
  EXPECT_TRUE(model.georeferenceMetadata.hasCoordinateOffset);
  EXPECT_NEAR(model.georeferenceMetadata.coordinateOffset.x, 1000.0, 1.0e-9);
  EXPECT_NEAR(model.georeferenceMetadata.coordinateOffset.y, 2000.0, 1.0e-9);
  EXPECT_NEAR(model.georeferenceMetadata.coordinateOffset.z, 12.5, 1.0e-9);
  EXPECT_EQ(model.georeferenceMetadata.crsAuthority, "EPSG");
  EXPECT_EQ(model.georeferenceMetadata.crsCode, "1234");

  EXPECT_EQ(model.meshRanges[0].meshId, 7u);
  EXPECT_EQ(model.meshRanges[0].firstIndex, 0u);
  EXPECT_EQ(model.meshRanges[0].indexCount, 3u);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.x, 1.0f, 1.0e-5f);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.y, 1.5f, 1.0e-5f);

  const auto &element = model.elements[0];
  EXPECT_EQ(element.meshId, 7u);
  EXPECT_EQ(element.guid, "element-1");
  EXPECT_EQ(element.type, "Wall");
  EXPECT_EQ(element.displayName, "North wall");
  EXPECT_EQ(element.objectType, "Basic wall");
  EXPECT_EQ(element.storeyName, "Level 01");
  EXPECT_EQ(element.storeyId, "storey-1");
  EXPECT_EQ(element.materialName, "Concrete");
  EXPECT_EQ(element.materialCategory, "Structural");
  EXPECT_EQ(element.discipline, "Architecture");
  EXPECT_EQ(element.phase, "New construction");
  EXPECT_EQ(element.fireRating, "2h");
  EXPECT_EQ(element.loadBearing, "true");
  EXPECT_EQ(element.status, "Existing");
  EXPECT_EQ(element.sourceId, "#42");
  ASSERT_EQ(element.properties.size(), 1u);
  EXPECT_EQ(element.properties[0].set, "Pset_WallCommon");
  EXPECT_EQ(element.properties[0].name, "AcousticRating");
  EXPECT_EQ(element.properties[0].value, "Rw40");
  EXPECT_EQ(element.properties[0].category, "pset");
  EXPECT_NEAR(element.transform[0].x, 2.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[1].z, -2.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[2].y, 2.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[3].x, 8.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[3].y, 12.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[3].z, -10.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.r, 10.0f / 255.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.g, 20.0f / 255.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.b, 30.0f / 255.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.a, 1.0f, 1.0e-5f);

  EXPECT_NEAR(model.vertices[0].normal.z, 1.0f, 1.0e-5f);
}

TEST(DotBimLoader, RejectsElementWithMissingMeshId) {
  constexpr const char *kDotBimJson = R"json(
{
  "schema_version": "1.1.0",
  "meshes": [
    {"mesh_id": 0, "coordinates": [0, 0, 0, 1, 0, 0, 0, 1, 0], "indices": [0, 1, 2]}
  ],
  "elements": [
    {"vector": {"x": 0, "y": 0, "z": 0}}
  ],
  "info": {}
}
)json";

  EXPECT_THROW((void)container::geometry::dotbim::LoadFromJson(kDotBimJson),
               std::runtime_error);
}

} // namespace
