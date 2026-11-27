#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/RoboticsOpenUsd.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace bim = container::geometry::dotbim;
using namespace container::robotics;

class TemporaryStage {
public:
  TemporaryStage() {
    static std::atomic_uint64_t serial{};
    const auto time = std::chrono::steady_clock::now().time_since_epoch().count();
    do {
      directory_ = fs::temp_directory_path() / ("bim-usda-" + std::to_string(time) +
                   "-" + std::to_string(serial.fetch_add(1)));
    } while (!fs::create_directory(directory_));
    path_ = directory_ / "scene.usda";
  }

  ~TemporaryStage() {
    std::error_code error;
    fs::remove(path_, error);
    fs::remove(directory_, error);
  }

  const fs::path& path() const { return path_; }

  std::string read() const {
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read the exported USDA test stage");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

private:
  fs::path directory_;
  fs::path path_;
};

bool hasDiagnostic(const RoboticsOpenUsdResult& result, std::string_view code) {
  return std::ranges::find(result.diagnostics, code, &RoboticsDiagnostic::code) != result.diagnostics.end();
}

TEST(BimRoboticsUsd, WritesDeterministicNativeIfcWithSemanticsAndCollision) {
  const auto input = fs::path(CONTAINER_ROBOTICS_FIXTURES) / "lidar-corner.ifc";
  auto model = container::geometry::ifc::LoadFromFile(input, 2.5f);
  TemporaryStage output;
  const RoboticsSceneOptions options{.sourceNamespace = "corner"};
  const auto result = writeRoboticsOpenUsd(output.path(), model, options);
  EXPECT_GE(result.entityCount, 6u);
  EXPECT_EQ(result.meshCount, 3u);
  EXPECT_EQ(result.colliderCount, 3u);
  const auto stage = output.read();
  EXPECT_TRUE(stage.starts_with("#usda 1.0\n"));
  EXPECT_TRUE(stage.contains("metersPerUnit = 1"));
  EXPECT_TRUE(stage.contains("upAxis = \"Z\""));
  EXPECT_TRUE(stage.contains("ifc:guid = \"0000000000000000000027\""));
  EXPECT_TRUE(stage.contains("ifc:guid = \"0000000000000000000037\""));
  EXPECT_TRUE(stage.contains("SemanticsLabelsAPI:class"));
  EXPECT_TRUE(stage.contains("PhysicsMeshCollisionAPI"));
  EXPECT_TRUE(stage.contains("UsdPreviewSurface"));
  EXPECT_TRUE(stage.contains("ifc:contains"));

  std::ranges::reverse(model.semanticProducts);
  std::ranges::reverse(model.semanticRelationships);
  std::ranges::reverse(model.elements);
  (void)writeRoboticsOpenUsd(output.path(), model, options);
  EXPECT_EQ(output.read(), stage);
}

TEST(BimRoboticsUsd, IfcxSharedGuidKeepsBothOccurrencesAndRequiresMetricEvidence) {
  constexpr auto source = R"json({"data":[
    {"path":"root","children":{"First":"one","Second":"two"}},
    {"path":"one","inherits":["type"],"attributes":{"usd::xformop":{"transform":[
      [1,0,0,0],[0,1,0,0],[0,0,1,0],[3,0,0,1]]}}},
    {"path":"two","inherits":["type"],"attributes":{"usd::xformop":{"transform":[
      [-1,0,0,0],[0,2,0,0],[0,0,1,0],[5,0,0,1]]}}},
    {"path":"type","attributes":{"bsi::ifc::class":"IfcSlab",
      "bsi::ifc::globalId":"shared-guid"},"children":{"Body":"mesh"}},
    {"path":"mesh","attributes":{"usd::usdgeom::mesh":{
      "faceVertexIndices":[0,1,2],"points":[[0,0,0],[1,0,0],[0,1,0]]}}}
  ]})json";
  auto model = container::geometry::ifcx::LoadFromJson(source);
  TemporaryStage output;
  const RoboticsSceneOptions metric{.sourceNamespace = "shared", .metersPerRendererUnit = 0.5};
  const auto written = writeRoboticsOpenUsd(output.path(), model, metric);
  EXPECT_EQ(written.entityCount, 2u);
  EXPECT_EQ(written.meshCount, 2u);
  EXPECT_EQ(written.colliderCount, 2u);
  EXPECT_TRUE(hasDiagnostic(written, "shared-guid-occurrences"));
  const auto stage = output.read();
  EXPECT_TRUE(stage.contains("ifc:sourceId = \"root/one\""));
  EXPECT_TRUE(stage.contains("ifc:sourceId = \"root/two\""));
  EXPECT_TRUE(stage.contains("ifc:guid = \"shared-guid\""));

  const RoboticsSceneOptions unknown{.sourceNamespace = "shared"};
  EXPECT_THROW((void)writeRoboticsOpenUsd(output.path(), model, unknown), std::invalid_argument);
  EXPECT_EQ(output.read(), stage);
  model.importReport.completeness = container::geometry::ImportCompleteness::Failed;
  EXPECT_THROW((void)writeRoboticsOpenUsd(output.path(), model, metric), std::invalid_argument);
  EXPECT_EQ(output.read(), stage);
}

TEST(BimRoboticsUsd, UnambiguousPhysicalHierarchyRetainsOtherRelationshipTargets) {
  bim::Model model;
  for (std::string_view id : {"a", "b", "c", "d"}) {
    model.semanticProducts.push_back({.sourceId = std::string(id),
        .guid = "guid-" + std::string(id), .type = "IfcSpace", .hasPlacement = true});
  }
  using Kind = bim::SemanticRelationshipKind;
  model.semanticRelationships = {{"a", "b", Kind::Aggregates}, {"b", "a", Kind::Nests},
      {"a", "c", Kind::Contains}, {"b", "c", Kind::Contains},
      {"c", "d", Kind::Contains}, {"c", "a", Kind::SpaceBoundary}};

  TemporaryStage output;
  const auto result = writeRoboticsOpenUsd(output.path(), model,
      {.sourceNamespace = "graph", .metersPerRendererUnit = 1.0});
  EXPECT_EQ(result.entityCount, 4u);
  EXPECT_EQ(result.meshCount, 0u);
  EXPECT_TRUE(hasDiagnostic(result, "ambiguous-usd-parent"));
  EXPECT_TRUE(hasDiagnostic(result, "cyclic-usd-parent"));
  EXPECT_TRUE(hasDiagnostic(result, "no-visual-meshes"));
  const auto stage = output.read();
  EXPECT_TRUE(stage.contains("ifc:aggregates"));
  EXPECT_TRUE(stage.contains("ifc:nests"));
  EXPECT_TRUE(stage.contains("ifc:spaceBoundary"));
}

TEST(BimRoboticsUsd, InvalidIfcXDrawablePlacementDoesNotBecomeAnIdentityCollider) {
  constexpr auto source = R"json({"data":[
    {"path":"wall","attributes":{"bsi::ifc::class":"IfcWall",
       "bsi::ifc::globalId":"wall-guid"},"children":{"Body":"group"}},
    {"path":"group","attributes":{"usd::xformop":{"transform":[[1,0],[0,1]]}},
       "children":{"Leaf":"mesh"}},
    {"path":"mesh","attributes":{"usd::usdgeom::mesh":{
       "faceVertexIndices":[0,1,2],"points":[[0,0,0],[1,0,0],[0,1,0]]}}}
  ]})json";
  const auto model = container::geometry::ifcx::LoadFromJson(source);
  ASSERT_EQ(model.elements.size(), 1u);
  TemporaryStage output;
  const auto result = writeRoboticsOpenUsd(output.path(), model,
      {.sourceNamespace = "malformed", .metersPerRendererUnit = 1.0});
  EXPECT_EQ(result.entityCount, 1u);
  EXPECT_EQ(result.meshCount, 0u);
  EXPECT_EQ(result.colliderCount, 0u);
  EXPECT_TRUE(hasDiagnostic(result, "no-visual-meshes"));
  EXPECT_TRUE(hasDiagnostic(result, "metric-placement-unavailable"));
  EXPECT_TRUE(output.read().contains("ifc:guid = \"wall-guid\""));
}

TEST(BimRoboticsUsd, StandaloneVisualGeometryKeepsItsPlacementWithoutAnInventedProduct) {
  constexpr auto source = R"ifc(ISO-10303-21;
DATA;
#1=IFCCARTESIANPOINTLIST3D(((0.,0.,0.),(1.,0.,0.),(0.,1.,0.)));
#2=IFCTRIANGULATEDFACESET(#1,$,.F.,((1,2,3)),$);
ENDSEC;
END-ISO-10303-21;
)ifc";
  const auto model = container::geometry::ifc::LoadFromStep(source);
  TemporaryStage output;
  const auto result = writeRoboticsOpenUsd(output.path(), model,
      {.sourceNamespace = "standalone", .metersPerRendererUnit = 1.0});
  EXPECT_EQ(result.entityCount, 0u);
  EXPECT_EQ(result.meshCount, 1u);
  EXPECT_EQ(result.colliderCount, 0u);
  const auto stage = output.read();
  EXPECT_TRUE(stage.contains("\"Unclassified\""));
  EXPECT_FALSE(stage.contains("ifc:guid"));
}

} // namespace
