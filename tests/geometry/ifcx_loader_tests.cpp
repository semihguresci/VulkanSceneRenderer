#include "Container/geometry/IfcxLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

std::string readTextFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(file)),
                   std::istreambuf_iterator<char>());
  return text;
}

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool startsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool downloadedIfc5SampleLooksRenderable(const std::filesystem::path &path) {
  const std::string text = readTextFile(path);
  if (text.find("usd::usdgeom::mesh") != std::string::npos ||
      text.find("usd::usdgeom::basiscurves") != std::string::npos ||
      text.find("points::array") != std::string::npos ||
      text.find("points::base64") != std::string::npos ||
      text.find("pcd::base64") != std::string::npos) {
    return true;
  }

  const std::string stem = lowerAscii(path.stem().string());
  return stem.find("-add-") != std::string::npos ||
         startsWith(stem, "add-") ||
         lowerAscii(path.parent_path().filename().string()) == "advanced";
}

TEST(IfcxLoader, ParsesUsdMeshWithInheritedMetadataAndTransform) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "product",
      "children": { "Body": "mesh" },
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [2, 3, 4, 1]
          ]
        },
        "bsi::ifc::globalId": "wall-guid",
        "bsi::ifc::class": { "code": "IfcWall" },
        "bsi::ifc::name": "North Wall",
        "bsi::ifc::objectType": "Basic Wall",
        "bsi::ifc::storeyName": "Level 01",
        "bsi::ifc::storeyId": "storey-guid",
        "bsi::ifc::materialName": "Concrete",
        "bsi::ifc::materialCategory": "Structural",
        "bsi::ifc::discipline": "Architecture",
        "bsi::ifc::phase": "New construction",
        "bsi::ifc::Pset_WallCommon::FireRating": "2h",
        "bsi::ifc::Pset_WallCommon::LoadBearing": true,
        "bsi::ifc::Pset_WallCommon::AcousticRating": "Rw40",
        "bsi::ifc::classification::Uniclass2015": "Ss_25_10_30",
        "bsi::ifc::sourceUpAxis": "Z",
        "bsi::ifc::projectedCRS::name": "EPSG test CRS",
        "bsi::ifc::projectedCRS::epsg": 1234,
        "bsi::ifc::mapConversion::eastings": 1000.0,
        "bsi::ifc::mapConversion::northings": 2000.0,
        "bsi::ifc::mapConversion::orthogonalHeight": 12.5,
        "bsi::ifc::status": "Existing",
        "bsi::ifc::presentation::diffuseColor": [0.2, 0.4, 0.6],
        "bsi::ifc::presentation::opacity": 0.5
      }
    },
    {
      "path": "mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson, 2.0f);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.vertices.size(), 3u);
  EXPECT_EQ(model.indices.size(), 3u);
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
  EXPECT_EQ(model.georeferenceMetadata.crsName, "EPSG test CRS");
  EXPECT_EQ(model.georeferenceMetadata.crsAuthority, "EPSG");
  EXPECT_EQ(model.georeferenceMetadata.crsCode, "1234");
  EXPECT_NEAR(model.elements[0].transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].z, -6.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_EQ(model.elements[0].guid, "wall-guid");
  EXPECT_EQ(model.elements[0].type, "IfcWall");
  EXPECT_EQ(model.elements[0].displayName, "North Wall");
  EXPECT_EQ(model.elements[0].objectType, "Basic Wall");
  EXPECT_EQ(model.elements[0].storeyName, "Level 01");
  EXPECT_EQ(model.elements[0].storeyId, "storey-guid");
  EXPECT_EQ(model.elements[0].materialName, "Concrete");
  EXPECT_EQ(model.elements[0].materialCategory, "Structural");
  EXPECT_EQ(model.elements[0].discipline, "Architecture");
  EXPECT_EQ(model.elements[0].phase, "New construction");
  EXPECT_EQ(model.elements[0].fireRating, "2h");
  EXPECT_EQ(model.elements[0].loadBearing, "true");
  EXPECT_EQ(model.elements[0].status, "Existing");
  EXPECT_EQ(model.elements[0].sourceId, "product/mesh");
  const auto acousticIt = std::ranges::find_if(
      model.elements[0].properties, [](const auto &property) {
        return property.set == "Pset_WallCommon" &&
               property.name == "AcousticRating";
      });
  ASSERT_NE(acousticIt, model.elements[0].properties.end());
  EXPECT_EQ(acousticIt->value, "Rw40");
  EXPECT_EQ(acousticIt->category, "pset");
  const auto classificationIt = std::ranges::find_if(
      model.elements[0].properties, [](const auto &property) {
        return property.category == "classification" &&
               property.name == "Uniclass2015";
      });
  ASSERT_NE(classificationIt, model.elements[0].properties.end());
  EXPECT_EQ(classificationIt->value, "Ss_25_10_30");
  EXPECT_NEAR(model.elements[0].color.r, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.4f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.6f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.5f, 1.0e-6f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
}

TEST(IfcxLoader, LiftsLineMarkingSurfaceFeaturesOffHostSurface) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "course",
      "children": {
        "Body": "course-mesh",
        "road - line marking": "line-marking"
      },
      "attributes": {
        "bsi::ifc::class": { "code": "IfcCourse" }
      }
    },
    {
      "path": "course-mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2, 1, 0, 3],
          "points": [[0, 0, 0], [2, 0, 0], [2, 1, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "line-marking",
      "children": { "Body": "marking-mesh" },
      "attributes": {
        "bsi::ifc::class": { "code": "IfcSurfaceFeatureLINEMARKING" }
      }
    },
    {
      "path": "marking-mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2, 1, 0, 3],
          "points": [[0, 0, 0], [1, 0, 0], [1, 0.1, 0], [0, 0.1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 2u);
  const auto courseIt = std::ranges::find_if(model.elements, [](const auto &e) {
    return e.type == "IfcCourse";
  });
  const auto markingIt =
      std::ranges::find_if(model.elements, [](const auto &e) {
        return e.type == "IfcSurfaceFeatureLINEMARKING";
      });
  ASSERT_NE(courseIt, model.elements.end());
  ASSERT_NE(markingIt, model.elements.end());
  EXPECT_NEAR(courseIt->transform[3].y, 0.0f, 1.0e-6f);
  EXPECT_GT(markingIt->transform[3].y, courseIt->transform[3].y);
  EXPECT_NEAR(markingIt->transform[3].y, 0.002f, 1.0e-6f);
}

TEST(IfcxLoader, TriangulatesPolygonCountsAndSkipsInvisibleMeshes) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "quad",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexCounts": [4],
          "faceVertexIndices": [0, 1, 2, 3],
          "points": [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "hidden",
      "attributes": {
        "usd::usdgeom::visibility": { "visibility": "invisible" },
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [0, 1, 0], [0, 0, 1]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.indices.size(), 6u);
  EXPECT_EQ(model.vertices.size(), 6u);
}

TEST(IfcxLoader, AppliesInheritedMaterialPresentation) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "mat-gray",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.35, 0.45, 0.55],
        "bsi::ifc::presentation::opacity": 0.6
      }
    },
    {
      "path": "mesh",
      "inherits": { "material": "mat-gray" },
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.35f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.45f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.55f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.6f, 1.0e-6f);
}

TEST(IfcxLoader, InstancesInheritedTypeGeometry) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "window-type",
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [10, 0, 0, 1]
          ]
        }
      },
      "children": { "Body": "shared-mesh" }
    },
    {
      "path": "shared-mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "window-a",
      "inherits": { "windowType": "window-type" },
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [1, 0, 0, 1]
          ]
        }
      }
    },
    {
      "path": "window-b",
      "inherits": { "windowType": "window-type" },
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [3, 0, 0, 1]
          ]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 2u);
  std::vector<float> xTranslations{
      model.elements[0].transform[3].x,
      model.elements[1].transform[3].x,
  };
  std::ranges::sort(xTranslations);
  EXPECT_NEAR(xTranslations[0], 11.0f, 1.0e-6f);
  EXPECT_NEAR(xTranslations[1], 13.0f, 1.0e-6f);
}

TEST(IfcxLoader, InstancePresentationOverridesInheritedTypeDefaults) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "door-type",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [1.0, 0.0, 0.0]
      },
      "children": { "Body": "shared-mesh" }
    },
    {
      "path": "shared-mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "door-instance",
      "inherits": { "doorType": "door-type" },
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.0, 0.0, 1.0]
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 1.0f, 1.0e-6f);
}

TEST(IfcxLoader, MapsInheritedGltfMaterial) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "mat-pbr",
      "attributes": {
        "gltf::material": {
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.1, 0.2, 0.3, 0.4],
            "metallicFactor": 0.7,
            "roughnessFactor": 0.25,
            "baseColorTexture": "textures/base.png",
            "metallicRoughnessTexture": "textures/mr.png"
          },
          "normalTexture": {
            "texture": "textures/normal.png",
            "scale": 0.5
          },
          "occlusionTexture": {
            "texture": "textures/ao.png",
            "strength": 0.6
          },
          "emissiveTexture": "textures/emissive.png",
          "emissiveFactor": [0.8, 0.7, 0.6],
          "alphaMode": "Blend",
          "alphaCutoff": 0.35,
          "doubleSided": true
        }
      }
    },
    {
      "path": "mesh",
      "inherits": { "material": "mat-pbr" },
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.materials.size(), 1u);
  EXPECT_EQ(model.elements[0].materialIndex, 0u);
  EXPECT_NEAR(model.elements[0].color.r, 0.1f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.3f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.4f, 1.0e-6f);
  EXPECT_TRUE(model.elements[0].doubleSided);
  const auto &material = model.materials[0].pbr;
  EXPECT_NEAR(material.metallicFactor, 0.7f, 1.0e-6f);
  EXPECT_NEAR(material.roughnessFactor, 0.25f, 1.0e-6f);
  EXPECT_NEAR(material.normalTextureScale, 0.5f, 1.0e-6f);
  EXPECT_NEAR(material.occlusionStrength, 0.6f, 1.0e-6f);
  EXPECT_NEAR(material.emissiveColor.r, 0.8f, 1.0e-6f);
  EXPECT_NEAR(material.alphaCutoff, 0.35f, 1.0e-6f);
  EXPECT_EQ(material.alphaMode, container::material::AlphaMode::Blend);
  EXPECT_FALSE(model.materials[0].texturePaths.baseColor.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.metallicRoughness.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.normal.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.occlusion.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.emissive.empty());
}

TEST(IfcxLoader, PreservesPresentationOpacityOnGltfMaterials) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "mat-glass",
      "attributes": {
        "gltf::material": {
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.7, 0.8, 0.9, 1.0],
            "roughnessFactor": 0.05
          }
        },
        "bsi::ifc::presentation::opacity": 0.35
      }
    },
    {
      "path": "mesh",
      "inherits": { "material": "mat-glass" },
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.materials.size(), 2u);
  EXPECT_EQ(model.elements[0].materialIndex, 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.7f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.8f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.9f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.35f, 1.0e-6f);
  EXPECT_NEAR(model.materials[1].pbr.baseColor.a, 0.35f, 1.0e-6f);
  EXPECT_EQ(model.materials[1].pbr.alphaMode,
            container::material::AlphaMode::Blend);
}

TEST(IfcxLoader, LoadsBasisCurvesAndPointArrays) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "root",
      "children": {
        "Axis": "curve",
        "Survey": "points"
      }
    },
    {
      "path": "curve",
      "attributes": {
        "usd::usdgeom::basiscurves": {
          "curveVertexCounts": [2, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 1, 1]]
        }
      }
    },
    {
      "path": "points",
      "attributes": {
        "points::array": {
          "positions": [[0, 0, 0], [1, 1, 1]],
          "colors": [[1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  EXPECT_EQ(model.meshRanges.size(), 2u);
  ASSERT_EQ(model.nativeCurveRanges.size(), 1u);
  ASSERT_EQ(model.nativePointRanges.size(), 1u);
  EXPECT_EQ(model.nativeCurveRanges[0].indexCount, 4u);
  EXPECT_EQ(model.nativePointRanges[0].indexCount, 2u);
  ASSERT_EQ(model.elements.size(), 2u);
  EXPECT_TRUE(std::ranges::any_of(model.elements, [](const auto &element) {
    return element.geometryKind ==
           container::geometry::dotbim::GeometryKind::Curves;
  }));
  EXPECT_TRUE(std::ranges::any_of(model.elements, [](const auto &element) {
    return element.geometryKind ==
           container::geometry::dotbim::GeometryKind::Points;
  }));
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
}

TEST(IfcxLoader, NonMeshElementsInheritMetadataThroughReferences) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "curve-meta",
      "attributes": {
        "bsi::ifc::globalId": "curve-guid",
        "bsi::ifc::name": "Alignment Directrix",
        "bsi::ifc::objectType": "Reference Line",
        "sourceId": "curve-source"
      }
    },
    {
      "path": "point-meta",
      "attributes": {
        "bsi::ifc::globalId": "point-guid",
        "bsi::ifc::prop::Name": "Survey Points",
        "bsi::ifc::materialName": "Survey Paint",
        "sourceId": "point-source"
      }
    },
    {
      "path": "curve",
      "inherits": { "metadata": "curve-meta" },
      "attributes": {
        "usd::usdgeom::basiscurves": {
          "points": [[0, 0, 0], [1, 0, 0]]
        }
      }
    },
    {
      "path": "points",
      "inherits": { "metadata": "point-meta" },
      "attributes": {
        "points::array": {
          "positions": [[0, 0, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 2u);
  const auto curveIt = std::ranges::find_if(
      model.elements,
      [](const auto &element) { return element.guid == "curve-guid"; });
  const auto pointsIt = std::ranges::find_if(
      model.elements,
      [](const auto &element) { return element.guid == "point-guid"; });
  ASSERT_NE(curveIt, model.elements.end());
  ASSERT_NE(pointsIt, model.elements.end());
  EXPECT_EQ(curveIt->geometryKind,
            container::geometry::dotbim::GeometryKind::Curves);
  EXPECT_EQ(curveIt->displayName, "Alignment Directrix");
  EXPECT_EQ(curveIt->objectType, "Reference Line");
  EXPECT_EQ(curveIt->sourceId, "curve-source");
  EXPECT_EQ(pointsIt->geometryKind,
            container::geometry::dotbim::GeometryKind::Points);
  EXPECT_EQ(pointsIt->displayName, "Survey Points");
  EXPECT_EQ(pointsIt->materialName, "Survey Paint");
  EXPECT_EQ(pointsIt->sourceId, "point-source");
}

TEST(IfcxLoader, PointCloudColorsComposeWithElementMaterials) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "root",
      "children": {
        "Plain": "plain",
        "Colored": "colored"
      }
    },
    {
      "path": "plain",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.25, 0.5, 0.75],
        "points::array": {
          "positions": [[0, 0, 0]]
        }
      }
    },
    {
      "path": "colored",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.5, 0.5, 0.5],
        "points::array": {
          "positions": [[1, 0, 0], [2, 0, 0]],
          "colors": [[1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 2u);
  ASSERT_EQ(model.meshRanges.size(), 2u);
  const auto plainIt = std::ranges::find_if(
      model.elements,
      [](const auto &element) { return element.guid == "root/plain"; });
  const auto coloredIt = std::ranges::find_if(
      model.elements,
      [](const auto &element) { return element.guid == "root/colored"; });
  ASSERT_NE(plainIt, model.elements.end());
  ASSERT_NE(coloredIt, model.elements.end());

  const auto rangeForMesh = [&](uint32_t meshId) {
    return std::ranges::find_if(model.meshRanges,
                                [meshId](const auto &range) {
                                  return range.meshId == meshId;
                                });
  };
  const auto plainRange = rangeForMesh(plainIt->meshId);
  const auto coloredRange = rangeForMesh(coloredIt->meshId);
  ASSERT_NE(plainRange, model.meshRanges.end());
  ASSERT_NE(coloredRange, model.meshRanges.end());
  ASSERT_GE(plainRange->indexCount, 1u);
  ASSERT_GE(coloredRange->indexCount, 25u);

  EXPECT_NEAR(plainIt->color.r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(plainIt->color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(plainIt->color.b, 0.75f, 1.0e-6f);
  const glm::vec3 plainVertexColor =
      model.vertices[model.indices[plainRange->firstIndex]].color;
  EXPECT_NEAR(plainVertexColor.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(plainVertexColor.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(plainVertexColor.b, 1.0f, 1.0e-6f);

  EXPECT_NEAR(coloredIt->color.r, 0.5f, 1.0e-6f);
  EXPECT_NEAR(coloredIt->color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(coloredIt->color.b, 0.5f, 1.0e-6f);
  const glm::vec3 firstPointColor =
      model.vertices[model.indices[coloredRange->firstIndex]].color;
  const glm::vec3 secondPointColor =
      model.vertices[model.indices[coloredRange->firstIndex + 24u]].color;
  EXPECT_NEAR(firstPointColor.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(firstPointColor.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(firstPointColor.b, 0.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.r, 0.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.b, 0.0f, 1.0e-6f);
}

TEST(IfcxLoader, PointCloudArrayColorsStayAlignedByPointIndex) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "points",
      "attributes": {
        "points::array": {
          "positions": [[0, 0, 0], [1, 0, 0], [2, 0, 0]],
          "colors": [[null, 0, 0], [0, 1, 0], [2, -1, 0.5]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.nativePointRanges.size(), 1u);
  ASSERT_EQ(model.meshRanges[0].indexCount, 72u);
  ASSERT_EQ(model.nativePointRanges[0].indexCount, 3u);
  const auto pointColor = [&](uint32_t pointIndex) {
    const uint32_t indexOffset =
        model.meshRanges[0].firstIndex + pointIndex * 24u;
    return model.vertices[model.indices[indexOffset]].color;
  };

  const glm::vec3 firstPointColor = pointColor(0u);
  const glm::vec3 secondPointColor = pointColor(1u);
  const glm::vec3 thirdPointColor = pointColor(2u);
  EXPECT_NEAR(firstPointColor.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(firstPointColor.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(firstPointColor.b, 1.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.r, 0.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.b, 0.0f, 1.0e-6f);
  EXPECT_NEAR(thirdPointColor.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(thirdPointColor.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(thirdPointColor.b, 0.5f, 1.0e-6f);
}

TEST(IfcxLoader, PcdBase64UsesOrganizedWidthHeightPointCount) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "pcd",
      "attributes": {
        "pcd::base64": "IyAuUENEIHYuNwpWRVJTSU9OIC43CkZJRUxEUyB4IHkgeiByZ2IKU0laRSA0IDQgNCA0ClRZUEUgRiBGIEYgVQpDT1VOVCAxIDEgMSAxCldJRFRIIDIKSEVJR0hUIDIKREFUQSBhc2NpaQowIDAgMCAxNjcxMTY4MAoxIDAgMCA2NTI4MAowIDEgMCAyNTUKMSAxIDAgMTY3NzcyMTUK"
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.meshRanges[0].indexCount, 96u);
  const auto pointColor = [&](uint32_t pointIndex) {
    const uint32_t indexOffset =
        model.meshRanges[0].firstIndex + pointIndex * 24u;
    return model.vertices[model.indices[indexOffset]].color;
  };

  const glm::vec3 firstPointColor = pointColor(0u);
  const glm::vec3 secondPointColor = pointColor(1u);
  const glm::vec3 thirdPointColor = pointColor(2u);
  const glm::vec3 fourthPointColor = pointColor(3u);
  EXPECT_NEAR(firstPointColor.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(firstPointColor.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(firstPointColor.b, 0.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.r, 0.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(secondPointColor.b, 0.0f, 1.0e-6f);
  EXPECT_NEAR(thirdPointColor.r, 0.0f, 1.0e-6f);
  EXPECT_NEAR(thirdPointColor.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(thirdPointColor.b, 1.0f, 1.0e-6f);
  EXPECT_NEAR(fourthPointColor.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(fourthPointColor.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(fourthPointColor.b, 1.0f, 1.0e-6f);
}

TEST(IfcxLoader, DecodesCompressedPcdExtendedLzfReferences) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "points",
      "attributes": {
        "pcd::base64": "IyAuUENEIHYwLjcgLSBQb2ludCBDbG91ZCBEYXRhIGZpbGUgZm9ybWF0ClZFUlNJT04gMC43CkZJRUxEUyB4IHkgegpTSVpFIDQgNCA0ClRZUEUgRiBGIEYKQ09VTlQgMSAxIDEKV0lEVEggNQpIRUlHSFQgMQpQT0lOVFMgNQpEQVRBIGJpbmFyeV9jb21wcmVzc2VkCgUAAAA8AAAAAADgMgA="
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.nativePointRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.nativePointRanges[0].indexCount, 5u);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_EQ(model.elements[0].geometryKind,
            container::geometry::dotbim::GeometryKind::Points);
}

TEST(IfcxLoader, ComposesLocalIfcImports) {
  const std::filesystem::path tempDir =
      std::filesystem::temp_directory_path() / "container_ifcx_import_test";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  const std::filesystem::path layerDir = tempDir / "layers";
  std::filesystem::create_directories(layerDir);
  const std::filesystem::path base = layerDir / "base.ifcx";
  const std::filesystem::path overlay = tempDir / "overlay.ifcx";
  {
    std::ofstream file(base, std::ios::binary);
    file << R"json(
{
  "data": [
    {
      "path": "product",
      "children": { "Body": "mesh" },
      "attributes": {
        "gltf::material": {
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.9, 0.1, 0.2, 1.0],
            "baseColorTexture": "textures/base.png"
          }
        }
      }
    },
    {
      "path": "mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";
  }
  {
    std::ofstream file(overlay, std::ios::binary);
    file << R"json(
{
  "imports": [
    { "uri": "layers/base.ifcx" }
  ],
  "data": [
    {
      "path": "product",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.9, 0.1, 0.2]
      }
    }
  ]
}
)json";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(overlay);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.materials.size(), 1u);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_NEAR(model.elements[0].color.r, 0.9f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.1f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.2f, 1.0e-6f);
  EXPECT_EQ(model.materials[0].texturePaths.baseColor.path.lexically_normal(),
            (layerDir / "textures" / "base.png").lexically_normal());

  std::filesystem::remove_all(tempDir);
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5Sample) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Hello Wall" /
      "hello-wall.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5PbrLayer) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Domestic Hot Water" /
      "domestic-hot-water-add-pbr.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
  EXPECT_GE(model.materials.size(), 1u);
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5AdvancedLayer) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Hello Wall" /
      "advanced" / "3rd-window.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_GE(model.meshRanges.size(), 2u);
  EXPECT_GE(model.elements.size(), 2u);
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5PointCloudSample) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Point Cloud" /
      "point-cloud.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
  EXPECT_GE(model.elements.size(), 5u);
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5LargeCompressedPointCloud) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Point Cloud" /
      "S1-pointcloud.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  ASSERT_TRUE(downloadedIfc5SampleLooksRenderable(sample));
  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
  EXPECT_GE(model.elements.size(), 1u);
}

} // namespace
