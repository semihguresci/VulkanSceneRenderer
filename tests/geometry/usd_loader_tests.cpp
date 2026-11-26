#include "Container/geometry/UsdLoader.h"
#include "Container/utility/ModelImportCancellation.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

constexpr const char *kTriangleUsd = R"usd(
#usda 1.0

def Xform "Root"
{
    matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (2, 3, 4, 1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
    color3f[] primvars:displayColor = [(0.25, 0.5, 0.75)]
    float[] primvars:displayOpacity = [0.5]

    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    }
}
)usd";

constexpr const char *kZUpTriangleUsd = R"usd(
#usda 1.0
(
    upAxis = "Z"
)

def Xform "Root"
{
    matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (2, 3, 4, 1))
    uniform token[] xformOpOrder = ["xformOp:transform"]

    def Mesh "Tri"
    {
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 0, 1)]
    }
}
)usd";

constexpr const char *kCentimeterStageUsd = R"usd(
#usda 1.0
(
    metersPerUnit = 0.01
)

def Xform "Root"
{
    matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (100, 0, 0, 1))
    uniform token[] xformOpOrder = ["xformOp:transform"]

    def Mesh "Tri"
    {
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (50, 0, 0), (0, 25, 0)]
    }
}
)usd";

constexpr const char *kTinyUsdDisplayOpacityUsd = R"usd(
#usda 1.0

def Mesh "Tri"
{
    color3f[] primvars:displayColor = [(0.1, 0.2, 0.3)]
    float[] primvars:displayOpacity = [0.35]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)usd";

constexpr const char *kMetadataTriangleUsd = R"usd(
#usda 1.0

def Xform "Wall" (
    customData = {
        string globalId = "wall-guid"
        string ifcClass = "IfcWall"
    }
)
{
    def Mesh "Body"
    {
        rel material:binding = </_materials/WallPaint>
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}

def "_materials"
{
    def Material "WallPaint"
    {
        token outputs:surface.connect = </_materials/WallPaint/preview/Preview.outputs:surface>

        def Scope "preview"
        {
            def Shader "Preview"
            {
                uniform token info:id = "UsdPreviewSurface"
                float3 inputs:diffuseColor = (0.2, 0.4, 0.6)
                float inputs:roughness = 0.5
                float inputs:opacity = 1
                token outputs:surface
            }
        }
    }
}
)usd";

constexpr const char *kPointsUsd = R"usd(
#usda 1.0

def Xform "Survey" (
    customData = {
        string globalId = "points-guid"
    }
)
{
    color3f[] primvars:displayColor = [(0.4, 0.6, 0.8)]
    float[] primvars:displayOpacity = [0.75]

    def Points "Markers"
    {
        float[] widths = [0.2]
        point3f[] points = [(0, 0, 0), (1, 0, 0)]
    }
}
)usd";

constexpr const char *kBasisCurvesUsd = R"usd(
#usda 1.0

def Xform "Runs" (
    customData = {
        string globalId = "curve-guid"
    }
)
{
    color3f[] primvars:displayColor = [(0.85, 0.45, 0.2)]

    def BasisCurves "Centerlines"
    {
        int[] curveVertexCounts = [3, 2]
        float[] widths = [0.1]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (2, 0, 0), (2, 1, 0)]
    }
}
)usd";

constexpr const char *kColoredPointsUsd = R"usd(
#usda 1.0

def Points "Markers"
{
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0)]
    float[] widths = [0.2]
    point3f[] points = [(0, 0, 0), (1, 0, 0)]
}
)usd";

constexpr const char *kColoredBasisCurvesUsd = R"usd(
#usda 1.0

def BasisCurves "Centerlines"
{
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]
    int[] curveVertexCounts = [3]
    float[] widths = [0.1]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]
}
)usd";

constexpr const char *kTinyUsdPointsUsd = R"usd(
#usda 1.0
(
    upAxis = "Z"
)

def Xform "Survey" (
    customData = {
        string globalId = "tiny-points-guid"
        string ifcClass = "IfcAnnotation"
    }
)
{
    matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (2, 3, 4, 1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
    color3f[] primvars:displayColor = [(0.25, 0.5, 0.75)]
    float[] primvars:displayOpacity = [0.6]

    def Points "Markers"
    {
        float[] widths = [0.2]
        point3f[] points = [(0, 0, 0), (1, 0, 0)]
    }
}
)usd";

constexpr const char *kTinyUsdPointsMaterialUsd = R"usd(
#usda 1.0

def Xform "Survey"
{
    def Points "Markers"
    {
        rel material:binding = </_materials/PointMat>
        float[] widths = [0.15]
        point3f[] points = [(0, 0, 0), (0, 1, 0)]
    }
}

def Mesh "Anchor"
{
    rel material:binding = </_materials/PointMat>
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}

def "_materials"
{
    def Material "PointMat"
    {
        token outputs:surface.connect = </_materials/PointMat/preview/Preview.outputs:surface>

        def Scope "preview"
        {
            def Shader "Preview"
            {
                uniform token info:id = "UsdPreviewSurface"
                float3 inputs:diffuseColor = (0.7, 0.2, 0.1)
                float inputs:roughness = 0.4
                float inputs:opacity = 0.9
                token outputs:surface
            }
        }
    }
}
)usd";

void appendLe16(std::vector<uint8_t> &bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void appendLe32(std::vector<uint8_t> &bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

std::filesystem::path writeStoredUsdz(std::string_view name,
                                      std::string_view layerText) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_usd_loader_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / (std::string(name) + ".usdz");

  constexpr std::string_view kEntryName = "root.usda";
  std::vector<uint8_t> bytes;
  const uint32_t localHeaderOffset = 0u;
  appendLe32(bytes, 0x04034b50u);
  appendLe16(bytes, 20u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe32(bytes, 0u);
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe16(bytes, static_cast<uint16_t>(kEntryName.size()));
  appendLe16(bytes, 0u);
  bytes.insert(bytes.end(), kEntryName.begin(), kEntryName.end());
  bytes.insert(bytes.end(), layerText.begin(), layerText.end());

  const uint32_t centralDirectoryOffset = static_cast<uint32_t>(bytes.size());
  appendLe32(bytes, 0x02014b50u);
  appendLe16(bytes, 20u);
  appendLe16(bytes, 20u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe32(bytes, 0u);
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe16(bytes, static_cast<uint16_t>(kEntryName.size()));
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe32(bytes, 0u);
  appendLe32(bytes, localHeaderOffset);
  bytes.insert(bytes.end(), kEntryName.begin(), kEntryName.end());

  const uint32_t centralDirectorySize =
      static_cast<uint32_t>(bytes.size()) - centralDirectoryOffset;
  appendLe32(bytes, 0x06054b50u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 1u);
  appendLe16(bytes, 1u);
  appendLe32(bytes, centralDirectorySize);
  appendLe32(bytes, centralDirectoryOffset);
  appendLe16(bytes, 0u);

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return path;
}

std::filesystem::path writeUsdText(std::string_view name,
                                   std::string_view layerText) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_usd_loader_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / (std::string(name) + ".usda");
  std::ofstream out(path, std::ios::binary);
  out.write(layerText.data(), static_cast<std::streamsize>(layerText.size()));
  return path;
}

std::filesystem::path openUsdSampleRoot() {
  return std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
         "OpenUSD-Sample-Assets";
}

void expectRenderableUsdSample(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "OpenUSD sample is not available: " << path.string();
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
}

const container::geometry::dotbim::Element *
findElementBySourceId(const container::geometry::dotbim::Model &model,
                      std::string_view sourceId) {
  for (const container::geometry::dotbim::Element &element : model.elements) {
    if (element.sourceId == sourceId) {
      return &element;
    }
  }
  return nullptr;
}

void expectAllVertexColorsNear(
    const container::geometry::dotbim::Model &model, float red, float green,
    float blue) {
  ASSERT_FALSE(model.vertices.empty());
  for (const container::geometry::Vertex &vertex : model.vertices) {
    EXPECT_NEAR(vertex.color.r, red, 1.0e-6f);
    EXPECT_NEAR(vertex.color.g, green, 1.0e-6f);
    EXPECT_NEAR(vertex.color.b, blue, 1.0e-6f);
  }
}

void expectVertexColorNear(const container::geometry::Vertex &vertex, float red,
                           float green, float blue) {
  EXPECT_NEAR(vertex.color.r, red, 1.0e-6f);
  EXPECT_NEAR(vertex.color.g, green, 1.0e-6f);
  EXPECT_NEAR(vertex.color.b, blue, 1.0e-6f);
}

} // namespace

TEST(UsdLoader, CancelledImportStopsBeforeReadingOrParsing) {
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto stop = cancellation.get_token();
  EXPECT_THROW((void)container::geometry::usd::LoadFromFile("missing.usd", 1.0f, stop),
               container::util::ModelImportCancelled);
  EXPECT_THROW((void)container::geometry::usd::LoadFromText("invalid source", 1.0f, stop),
               container::util::ModelImportCancelled);
}

TEST(UsdLoader, ParsesUsdMeshTransformColorAndPolygonTriangulation) {
  const auto model = container::geometry::usd::LoadFromText(kTriangleUsd, 2.0f);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.indices.size(), 6u);
  EXPECT_EQ(model.vertices.size(), 6u);
  EXPECT_NEAR(model.elements[0].transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].y, 6.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].z, 8.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.75f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.5f, 1.0e-6f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
}

TEST(UsdLoader, PreservesAuthoredMeshMetadata) {
  constexpr const char *kUsd = R"usd(
#usda 1.0

def Xform "Building"
{
    string storeyName = "Level 01"
    string storeyId = "storey-guid"
    string discipline = "Architecture"
    string phase = "New construction"

    def Mesh "Wall"
    {
        string globalId = "wall-guid"
        string type = "IfcWall"
        string displayName = "North Wall"
        string objectType = "Basic Wall"
        string materialName = "Concrete"
        string materialCategory = "Structural"
        string fireRating = "2h"
        bool loadBearing = true
        token status = "Existing"
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  const auto &element = model.elements[0];
  EXPECT_EQ(element.guid, "wall-guid");
  EXPECT_EQ(element.type, "IfcWall");
  EXPECT_EQ(element.displayName, "North Wall");
  EXPECT_EQ(element.objectType, "Basic Wall");
  EXPECT_EQ(element.storeyName, "Level 01");
  EXPECT_EQ(element.storeyId, "storey-guid");
  EXPECT_EQ(element.materialName, "Concrete");
  EXPECT_EQ(element.materialCategory, "Structural");
  EXPECT_EQ(element.discipline, "Architecture");
  EXPECT_EQ(element.phase, "New construction");
  EXPECT_EQ(element.fireRating, "2h");
  EXPECT_EQ(element.loadBearing, "true");
  EXPECT_EQ(element.status, "Existing");
  EXPECT_EQ(element.sourceId, "/Building/Wall");
}

TEST(UsdLoader, ConvertsZUpStageToRendererAxes) {
  const auto model =
      container::geometry::usd::LoadFromText(kZUpTriangleUsd, 2.0f);

  ASSERT_EQ(model.elements.size(), 1u);
  const glm::mat4 &transform = model.elements[0].transform;
  EXPECT_NEAR(transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].z, -6.0f, 1.0e-6f);
}

TEST(UsdLoader, AppliesMetersPerUnitBeforeImportScale) {
  const auto model =
      container::geometry::usd::LoadFromText(kCentimeterStageUsd, 2.0f);

  ASSERT_EQ(model.elements.size(), 1u);
  const glm::mat4 &transform = model.elements[0].transform;
  EXPECT_TRUE(model.unitMetadata.hasSourceUnits);
  EXPECT_EQ(model.unitMetadata.sourceUnits, "centimeters");
  EXPECT_TRUE(model.unitMetadata.hasMetersPerUnit);
  EXPECT_NEAR(model.unitMetadata.metersPerUnit, 0.01f, 1.0e-9f);
  EXPECT_TRUE(model.unitMetadata.hasImportScale);
  EXPECT_NEAR(model.unitMetadata.importScale, 2.0f, 1.0e-6f);
  EXPECT_TRUE(model.unitMetadata.hasEffectiveImportScale);
  EXPECT_NEAR(model.unitMetadata.effectiveImportScale, 0.02f, 1.0e-9f);
  EXPECT_NEAR(transform[0].x, 0.02f, 1.0e-6f);
  EXPECT_NEAR(transform[1].y, 0.02f, 1.0e-6f);
  EXPECT_NEAR(transform[2].z, 0.02f, 1.0e-6f);
  EXPECT_NEAR(transform[3].x, 2.0f, 1.0e-6f);
}

TEST(UsdLoader, SkipsInvisibleMeshes) {
  constexpr const char *kUsd = R"usd(
#usda 1.0
def Xform "Hidden"
{
    token visibility = "invisible"
    def Mesh "Tri"
    {
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(model.indices.empty());
  EXPECT_TRUE(model.elements.empty());
}

TEST(UsdLoader, ParsesDoubleSidedFlag) {
  constexpr const char *kUsd = R"usd(
#usda 1.0
def Mesh "Tri"
{
    uniform bool doubleSided = false
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_FALSE(model.elements[0].doubleSided);
}

TEST(UsdLoader, DefaultsDoubleSidedToUsdSchemaFallback) {
  const auto model = container::geometry::usd::LoadFromText(kTriangleUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_FALSE(model.elements[0].doubleSided);
}

TEST(UsdLoader, LoadsStoredUsdzRootLayer) {
  const std::filesystem::path path =
      writeStoredUsdz("stored_triangle", kTriangleUsd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.indices.size(), 6u);
}

TEST(UsdLoader, ParsesAuthoredVertexAttributes) {
  constexpr const char *kUsd = R"usd(
#usda 1.0
def Mesh "Tri"
{
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)]
    texCoord2f[] primvars:st = [(0.125, 0.25), (0.875, 0.25), (0.125, 0.75)]
    texCoord2f[] primvars:st1 = [(0.2, 0.3), (0.4, 0.5), (0.6, 0.7)]
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_GE(model.vertices.size(), 3u);
  EXPECT_NEAR(model.vertices[0].normal.x, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].normal.y, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].normal.z, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord.x, 0.125f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord.y, 0.25f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord1.x, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord1.y, 0.3f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].color.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].color.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].color.b, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 1.0f, 1.0e-6f);
}

TEST(UsdLoader, InheritsUsdTextMetadataFromAncestorPrim) {
  const auto model =
      container::geometry::usd::LoadFromText(kMetadataTriangleUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].guid, "wall-guid");
  EXPECT_EQ(model.elements[0].type, "IfcWall");
}

TEST(UsdLoader, LoadsTextUsdPointsAsPlaceholderGeometry) {
  const auto model = container::geometry::usd::LoadFromText(kPointsUsd);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.nativePointRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_FALSE(model.vertices.empty());
  ASSERT_FALSE(model.indices.empty());
  EXPECT_LT(model.meshRanges[0].indexCount,
            static_cast<uint32_t>(model.indices.size()));
  EXPECT_EQ(model.nativePointRanges[0].indexCount, 2u);
  EXPECT_GT(model.meshRanges[0].boundsRadius, 0.1f);
  EXPECT_EQ(model.elements[0].guid, "points-guid");
  EXPECT_EQ(model.elements[0].type, "UsdGeomPoints");
  EXPECT_EQ(model.elements[0].geometryKind,
            container::geometry::dotbim::GeometryKind::Points);
  EXPECT_EQ(model.elements[0].sourceId, "/Survey/Markers");
  EXPECT_NEAR(model.elements[0].color.r, 0.4f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.6f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.8f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.75f, 1.0e-6f);
  expectAllVertexColorsNear(model, 0.4f, 0.6f, 0.8f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
}

TEST(UsdLoader, LoadsTextUsdBasisCurvesAsPlaceholderGeometry) {
  const auto model = container::geometry::usd::LoadFromText(kBasisCurvesUsd);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.nativeCurveRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_FALSE(model.vertices.empty());
  ASSERT_FALSE(model.indices.empty());
  EXPECT_LT(model.meshRanges[0].indexCount,
            static_cast<uint32_t>(model.indices.size()));
  EXPECT_EQ(model.nativeCurveRanges[0].indexCount, 6u);
  EXPECT_GT(model.meshRanges[0].boundsRadius, 0.5f);
  EXPECT_EQ(model.elements[0].guid, "curve-guid");
  EXPECT_EQ(model.elements[0].type, "UsdGeomBasisCurves");
  EXPECT_EQ(model.elements[0].geometryKind,
            container::geometry::dotbim::GeometryKind::Curves);
  EXPECT_EQ(model.elements[0].sourceId, "/Runs/Centerlines");
  EXPECT_NEAR(model.elements[0].color.r, 0.85f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.45f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.2f, 1.0e-6f);
  expectAllVertexColorsNear(model, 0.85f, 0.45f, 0.2f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
}

TEST(UsdLoader, AppliesUsdPointDisplayColorsToPlaceholderGeometry) {
  const auto model = container::geometry::usd::LoadFromText(kColoredPointsUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.nativePointRanges.size(), 1u);
  EXPECT_EQ(model.nativePointRanges[0].indexCount, 2u);
  ASSERT_GE(model.vertices.size(), 48u);
  expectVertexColorNear(model.vertices[0], 1.0f, 0.0f, 0.0f);
  expectVertexColorNear(model.vertices[23], 1.0f, 0.0f, 0.0f);
  expectVertexColorNear(model.vertices[24], 0.0f, 1.0f, 0.0f);
  expectVertexColorNear(model.vertices[47], 0.0f, 1.0f, 0.0f);
}

TEST(UsdLoader, AppliesUsdCurveDisplayColorsToPlaceholderGeometry) {
  const auto model =
      container::geometry::usd::LoadFromText(kColoredBasisCurvesUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.nativeCurveRanges.size(), 1u);
  EXPECT_EQ(model.nativeCurveRanges[0].indexCount, 4u);
  ASSERT_GE(model.vertices.size(), 72u);
  expectVertexColorNear(model.vertices[0], 0.5f, 0.5f, 0.0f);
  expectVertexColorNear(model.vertices[35], 0.5f, 0.5f, 0.0f);
  expectVertexColorNear(model.vertices[36], 0.0f, 0.5f, 0.5f);
  expectVertexColorNear(model.vertices[71], 0.0f, 0.5f, 0.5f);
}

TEST(UsdLoader, LoadsDisplayOpacityWhenTinyUsdIsAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("display_opacity", kTinyUsdDisplayOpacityUsd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.1f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.3f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.35f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdConvertsZUpStageToRendererAxesWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("z_up_stage_axes", kZUpTriangleUsd);

  const auto model = container::geometry::usd::LoadFromFile(path, 2.0f);
  ASSERT_EQ(model.elements.size(), 1u);
  const glm::mat4 &transform = model.elements[0].transform;
  EXPECT_NEAR(transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].z, -6.0f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdAppliesMetersPerUnitWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("tiny_centimeter_stage", kCentimeterStageUsd);

  const auto model = container::geometry::usd::LoadFromFile(path, 2.0f);
  ASSERT_EQ(model.elements.size(), 1u);
  const glm::mat4 &transform = model.elements[0].transform;
  EXPECT_NEAR(transform[0].x, 0.02f, 1.0e-6f);
  EXPECT_NEAR(transform[1].y, 0.02f, 1.0e-6f);
  EXPECT_NEAR(transform[2].z, 0.02f, 1.0e-6f);
  EXPECT_NEAR(transform[3].x, 2.0f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdLoadsGeomPointsAsPlaceholderGeometryWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("tiny_points_placeholder", kTinyUsdPointsUsd);

  const auto model = container::geometry::usd::LoadFromFile(path, 2.0f);
  const container::geometry::dotbim::Element *points =
      findElementBySourceId(model, "/Survey/Markers");
  ASSERT_NE(points, nullptr);
  ASSERT_FALSE(model.nativePointRanges.empty());
  ASSERT_LT(points->meshId, model.meshRanges.size());
  ASSERT_FALSE(model.vertices.empty());
  ASSERT_FALSE(model.indices.empty());
  EXPECT_GT(model.meshRanges[points->meshId].boundsRadius, 0.1f);
  EXPECT_EQ(points->guid, "tiny-points-guid");
  EXPECT_EQ(points->type, "IfcAnnotation");
  EXPECT_EQ(points->geometryKind,
            container::geometry::dotbim::GeometryKind::Points);
  EXPECT_EQ(points->displayName, "Markers");
  EXPECT_NEAR(points->color.r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(points->color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(points->color.b, 0.75f, 1.0e-6f);
  EXPECT_NEAR(points->color.a, 0.6f, 1.0e-6f);
  EXPECT_NEAR(points->transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(points->transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(points->transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_NEAR(points->transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(points->transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(points->transform[3].z, -6.0f, 1.0e-6f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdLoadsGeomBasisCurvesAsPlaceholderGeometryWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("tiny_basis_curves_placeholder", kBasisCurvesUsd);

  const auto model = container::geometry::usd::LoadFromFile(path, 2.0f);
  const container::geometry::dotbim::Element *curves =
      findElementBySourceId(model, "/Runs/Centerlines");
  ASSERT_NE(curves, nullptr);
  ASSERT_FALSE(model.nativeCurveRanges.empty());
  ASSERT_LT(curves->meshId, model.meshRanges.size());
  ASSERT_FALSE(model.vertices.empty());
  ASSERT_FALSE(model.indices.empty());
  EXPECT_GT(model.meshRanges[curves->meshId].boundsRadius, 0.5f);
  EXPECT_EQ(curves->guid, "curve-guid");
  EXPECT_EQ(curves->type, "UsdGeomBasisCurves");
  EXPECT_EQ(curves->geometryKind,
            container::geometry::dotbim::GeometryKind::Curves);
  EXPECT_EQ(curves->displayName, "Centerlines");
  EXPECT_NEAR(curves->color.r, 0.85f, 1.0e-6f);
  EXPECT_NEAR(curves->color.g, 0.45f, 1.0e-6f);
  EXPECT_NEAR(curves->color.b, 0.2f, 1.0e-6f);
  EXPECT_NEAR(curves->transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(curves->transform[1].y, 2.0f, 1.0e-6f);
  EXPECT_NEAR(curves->transform[2].z, 2.0f, 1.0e-6f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdPreservesGeomPointsMaterialBindingWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("tiny_points_material", kTinyUsdPointsMaterialUsd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  const container::geometry::dotbim::Element *points =
      findElementBySourceId(model, "/Survey/Markers");
  ASSERT_NE(points, nullptr);
  if (points->materialIndex == std::numeric_limits<uint32_t>::max()) {
    GTEST_SKIP() << "TinyUSDZ did not expose GeomPoints material binding";
  }
  ASSERT_LT(points->materialIndex, model.materials.size());
  EXPECT_NEAR(points->color.r, 0.7f, 1.0e-6f);
  EXPECT_NEAR(points->color.g, 0.2f, 1.0e-6f);
  EXPECT_NEAR(points->color.b, 0.1f, 1.0e-6f);
  EXPECT_NEAR(points->color.a, 0.9f, 1.0e-6f);
  const auto &material = model.materials[points->materialIndex].pbr;
  EXPECT_NEAR(material.baseColor.r, 0.7f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.g, 0.2f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.b, 0.1f, 1.0e-6f);
  EXPECT_NEAR(material.opacityFactor, 0.9f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdDefaultsDoubleSidedToUsdSchemaFallbackWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("default_double_sided", kTinyUsdDisplayOpacityUsd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_FALSE(model.elements[0].doubleSided);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsBoundPreviewSurfaceMaterialsWhenTinyUsdIsAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "cube-previewsurface.usda";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "TinyUSDZ PreviewSurface sample is not available";
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_FALSE(model.materials.empty());
  ASSERT_NE(model.elements[0].materialIndex,
            std::numeric_limits<uint32_t>::max());
  ASSERT_LT(model.elements[0].materialIndex, model.materials.size());
  const auto &material = model.materials[model.elements[0].materialIndex].pbr;
  EXPECT_NEAR(material.baseColor.r, 0.12825416f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.g, 0.8000001f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.b, 0.21895278f, 1.0e-6f);
  EXPECT_NEAR(material.metallicFactor, 0.0f, 1.0e-6f);
  EXPECT_NEAR(material.roughnessFactor, 0.4f, 1.0e-6f);
  EXPECT_NEAR(material.opacityFactor, 1.0f, 1.0e-6f);
  EXPECT_EQ(material.alphaMode, container::material::AlphaMode::Opaque);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdInheritsMetadataAndPreservesMaterialWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path samplePath =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "cube-previewsurface.usda";
  if (!std::filesystem::exists(samplePath)) {
    GTEST_SKIP() << "TinyUSDZ PreviewSurface sample is not available";
  }

  std::ifstream sample(samplePath, std::ios::binary);
  ASSERT_TRUE(sample);
  std::string usd((std::istreambuf_iterator<char>(sample)),
                  std::istreambuf_iterator<char>());

  const std::string xformNeedle = "def Xform \"Cube\"";
  const std::string xformReplacement = R"usd(def Xform "Cube" (
    customData = {
        string globalId = "wall-guid"
        string ifcClass = "IfcWall"
    }
)
{)usd";
  const size_t xformPos = usd.find(xformNeedle);
  ASSERT_NE(xformPos, std::string::npos);
  const size_t xformBracePos = usd.find('{', xformPos);
  ASSERT_NE(xformBracePos, std::string::npos);
  usd.replace(xformPos, xformBracePos - xformPos + 1u, xformReplacement);

  const std::string colorNeedle =
      "float3 inputs:diffuseColor = (0.12825416, 0.8000001, 0.21895278)";
  const size_t colorPos = usd.find(colorNeedle);
  ASSERT_NE(colorPos, std::string::npos);
  usd.replace(colorPos, colorNeedle.size(),
              "float3 inputs:diffuseColor = (0.2, 0.4, 0.6)");

  const std::filesystem::path path =
      writeUsdText("tiny_metadata_inheritance", usd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].guid, "wall-guid");
  EXPECT_EQ(model.elements[0].type, "IfcWall");
  ASSERT_NE(model.elements[0].materialIndex,
            std::numeric_limits<uint32_t>::max());
  ASSERT_LT(model.elements[0].materialIndex, model.materials.size());
  const auto &material = model.materials[model.elements[0].materialIndex].pbr;
  EXPECT_NEAR(material.baseColor.r, 0.2f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.g, 0.4f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.b, 0.6f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, MapsPreviewSurfaceSpecularWorkflowWhenTinyUsdIsAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path samplePath =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "cube-previewsurface.usda";
  if (!std::filesystem::exists(samplePath)) {
    GTEST_SKIP() << "TinyUSDZ PreviewSurface sample is not available";
  }

  std::ifstream sample(samplePath, std::ios::binary);
  ASSERT_TRUE(sample);
  std::string usd((std::istreambuf_iterator<char>(sample)),
                  std::istreambuf_iterator<char>());

  const size_t metallicPos = usd.find("float inputs:metallic = 0");
  ASSERT_NE(metallicPos, std::string::npos);
  usd.replace(metallicPos, std::string("float inputs:metallic = 0").size(),
              "float inputs:metallic = 1");

  const size_t roughnessPos = usd.find("float inputs:roughness = 0.4");
  ASSERT_NE(roughnessPos, std::string::npos);
  usd.replace(roughnessPos, std::string("float inputs:roughness = 0.4").size(),
              "float inputs:roughness = 0.25");

  const size_t specularPos = usd.find("float inputs:specular = 0.5");
  ASSERT_NE(specularPos, std::string::npos);
  usd.replace(specularPos, std::string("float inputs:specular = 0.5").size(),
              "int inputs:useSpecularWorkflow = 1\n"
              "                color3f inputs:specularColor = (0.2, 0.4, 0.6)\n"
              "                float inputs:specular = 0.5");

  const std::filesystem::path path =
      writeUsdText("preview_surface_specular_workflow", usd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_NE(model.elements[0].materialIndex,
            std::numeric_limits<uint32_t>::max());
  ASSERT_LT(model.elements[0].materialIndex, model.materials.size());

  const auto &material = model.materials[model.elements[0].materialIndex].pbr;
  EXPECT_FALSE(material.specularGlossinessWorkflow);
  EXPECT_NEAR(material.metallicFactor, 0.0f, 1.0e-6f);
  EXPECT_NEAR(material.roughnessFactor, 0.25f, 1.0e-6f);
  EXPECT_NEAR(material.specularColorFactor.r, 0.2f, 1.0e-6f);
  EXPECT_NEAR(material.specularColorFactor.g, 0.4f, 1.0e-6f);
  EXPECT_NEAR(material.specularColorFactor.b, 0.6f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsTinyUsdTextureAssetsWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "texture-cat-plane.usdz";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "TinyUSDZ textured USDZ sample is not available";
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_FALSE(model.materials.empty());

  bool foundBaseColorTexture = false;
  for (const auto &material : model.materials) {
    const auto &texture = material.texturePaths.baseColor;
    if (texture.empty()) {
      continue;
    }

    foundBaseColorTexture = true;
    EXPECT_FALSE(texture.name.empty());
    EXPECT_LE(texture.samplerIndex,
              container::gpu::kMaterialSamplerDescriptorCapacity - 1u);
    EXPECT_FALSE(texture.encodedBytes.empty());
  }
  EXPECT_TRUE(foundBaseColorTexture);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsExternalTinyUsdTexturePathsWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "texture-cat-plane.usda";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "TinyUSDZ textured USDA sample is not available";
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_FALSE(model.materials.empty());

  bool foundBaseColorTexture = false;
  for (const auto &material : model.materials) {
    const auto &texture = material.texturePaths.baseColor;
    if (texture.empty()) {
      continue;
    }

    foundBaseColorTexture = true;
    EXPECT_FALSE(texture.name.empty());
    ASSERT_FALSE(texture.path.empty());
    EXPECT_TRUE(std::filesystem::exists(texture.path));
  }
  EXPECT_TRUE(foundBaseColorTexture);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, PreservesTinyUsdScalarTextureOutputChannelsWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "texture-channel-001.usda";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "TinyUSDZ texture-channel sample is not available";
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_FALSE(model.materials.empty());

  bool foundScalarChannelMaterial = false;
  for (const auto &material : model.materials) {
    if (material.texturePaths.metalness.empty() ||
        material.texturePaths.roughness.empty()) {
      continue;
    }

    foundScalarChannelMaterial = true;
    EXPECT_EQ(material.pbr.metalnessTextureTransform.channel, 1u);
    EXPECT_EQ(material.pbr.roughnessTextureTransform.channel, 2u);
    EXPECT_FALSE(material.texturePaths.metalness.path.empty());
    EXPECT_FALSE(material.texturePaths.roughness.path.empty());
    EXPECT_TRUE(std::filesystem::exists(material.texturePaths.metalness.path));
    EXPECT_TRUE(std::filesystem::exists(material.texturePaths.roughness.path));
  }
  EXPECT_TRUE(foundScalarChannelMaterial);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, RejectsBinaryUsdcPayloads) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_usd_loader_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / "binary.usd";
  {
    std::ofstream out(path, std::ios::binary);
    out.write("PXR-USDC", 8);
  }

  EXPECT_THROW((void)container::geometry::usd::LoadFromFile(path),
               std::runtime_error);
}

TEST(UsdLoader, LoadsOpenUsdKitchenSetSampleWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  expectRenderableUsdSample(openUsdSampleRoot() / "Kitchen_set" /
                            "Kitchen_set.usd");
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsOpenUsdKitchenSetInstancedSampleWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  expectRenderableUsdSample(openUsdSampleRoot() / "Kitchen_set" /
                            "Kitchen_set_instanced.usd");
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsOpenUsdPointInstancedMedCitySampleWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  expectRenderableUsdSample(openUsdSampleRoot() / "PointInstancedMedCity" /
                            "PointInstancedMedCity.usd");
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}
