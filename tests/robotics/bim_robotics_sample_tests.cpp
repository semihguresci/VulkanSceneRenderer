#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/robotics/RoboticsSceneManifest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using container::robotics::RoboticsScene;
using container::robotics::RoboticsSceneOptions;
using container::robotics::SemanticEntity;
using container::robotics::SemanticKind;
using container::robotics::SemanticRelationshipKind;

std::optional<std::filesystem::path> teklaSample(std::string_view extension) {
  const char* root = std::getenv("CONTAINER_IFC_SAMPLE_ROOT");
  if (!root)
    return std::nullopt;
  return std::filesystem::path(root) / "Tekla House" /
      ("TeklaHouse." + std::string(extension));
}

const SemanticEntity* bySource(const RoboticsScene& scene, std::string_view source) {
  const auto found = std::ranges::find(scene.entities, source, &SemanticEntity::sourceId);
  return found == scene.entities.end() ? nullptr : &*found;
}

const SemanticEntity* bySourceSuffix(const RoboticsScene& scene, std::string_view source) {
  const SemanticEntity* result = nullptr;
  for (const auto& entity : scene.entities) {
    if (!entity.sourceId.ends_with(source))
      continue;
    if (result)
      return nullptr;
    result = &entity;
  }
  return result;
}

bool hasRelationship(const RoboticsScene& scene, const SemanticEntity& parent,
                     const SemanticEntity& child, SemanticRelationshipKind kind) {
  return std::ranges::any_of(scene.relationships, [&](const auto& relationship) {
    return relationship.from == parent.id && relationship.to == child.id &&
           relationship.kind == kind;
  });
}

void checkTeklaClasses(const RoboticsScene& scene) {
  EXPECT_EQ(std::ranges::count(scene.entities, SemanticKind::Storey, &SemanticEntity::kind), 1);
  EXPECT_EQ(std::ranges::count(scene.entities, SemanticKind::Wall, &SemanticEntity::kind), 69);
  EXPECT_EQ(std::ranges::count(scene.entities, SemanticKind::Slab, &SemanticEntity::kind), 35);
  // These facts are absent in both source exports. They must not be inferred
  // from an opening-shaped void or from the building/storey hierarchy.
  for (const auto kind : {SemanticKind::Space, SemanticKind::Door, SemanticKind::Opening})
    EXPECT_EQ(std::ranges::count(scene.entities, kind, &SemanticEntity::kind), 0);
}

void checkManifestReload(const RoboticsScene& scene) {
  const auto manifest = container::robotics::serializeRoboticsScene(scene);
  EXPECT_EQ(container::robotics::serializeRoboticsScene(
                container::robotics::parseRoboticsScene(manifest)), manifest);

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
      ("container-tekla-robotics-" + std::to_string(nonce) + ".json");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } cleanup{path};
  container::robotics::writeRoboticsSceneManifest(path, scene);
  EXPECT_EQ(container::robotics::serializeRoboticsScene(
                container::robotics::readRoboticsSceneManifest(path)), manifest);
}

TEST(BimRoboticsSamples, TeklaIfcPreservesSourceGuidPlacementAndManifest) {
  const auto sample = teklaSample("ifc");
  if (!sample)
    GTEST_SKIP() << "Set CONTAINER_IFC_SAMPLE_ROOT to the buildingSMART examples directory";
  ASSERT_TRUE(std::filesystem::is_regular_file(*sample)) << *sample;
  const RoboticsSceneOptions options{.sourceNamespace = "buildingSMART/TeklaHouse.ifc"};
  const auto load = [&] {
    return container::robotics::buildRoboticsScene(
        container::geometry::ifc::LoadFromFile(*sample), options);
  };
  const auto scene = load();
  checkTeklaClasses(scene);
  ASSERT_TRUE(scene.frame.metersPerRendererUnit);
  // The renderer still uses its historical float millimetre import scale.
  constexpr double rendererScale = static_cast<double>(0.001f);
  EXPECT_DOUBLE_EQ(*scene.frame.metersPerRendererUnit, 0.001 / rendererScale);
  ASSERT_TRUE(scene.frame.sourceMetersPerUnit);
  EXPECT_NEAR(*scene.frame.sourceMetersPerUnit, .001, 1e-9);

  const auto* wall = bySource(scene, "#1561");
  ASSERT_NE(wall, nullptr);
  EXPECT_EQ(wall->guid, "1GQIdN0001sZ4pD3amDpOs");
  EXPECT_EQ(wall->kind, SemanticKind::Wall);
  EXPECT_EQ(wall->name, "PANEL");
  ASSERT_TRUE(wall->rendererWorldFromLocal);
  ASSERT_TRUE(wall->roboticsWorldFromLocal);
  // Independent STEP oracle: #1557=(-3946.6006077131,10224.6131834722,
  // 12000) mm, #1560 is its local placement; all ancestor placements are
  // identity. REP-103 gives (source Y,-source X,source Z), in metres.
  const auto& renderer = *wall->rendererWorldFromLocal;
  const auto& robotics = *wall->roboticsWorldFromLocal;
  EXPECT_NEAR(renderer[3].x, -3946.6006077131 * rendererScale, 1e-9);
  EXPECT_NEAR(renderer[3].y, 12000.0 * rendererScale, 1e-9);
  EXPECT_NEAR(renderer[3].z, -10224.6131834722 * rendererScale, 1e-9);
  EXPECT_NEAR(robotics[3].x, 10.2246131834722, 1e-9);
  EXPECT_NEAR(robotics[3].y, 3.9466006077131, 1e-9);
  EXPECT_NEAR(robotics[3].z, 12.0, 1e-9);
  EXPECT_FALSE(wall->representations.empty());
  const auto* assembly = bySource(scene, "#1208");
  const auto* storey = bySource(scene, "#42");
  ASSERT_NE(assembly, nullptr);
  ASSERT_NE(storey, nullptr);
  // #187032 is the explicit IFCRELAGGREGATES from #1208 to #1561.
  EXPECT_TRUE(hasRelationship(scene, *assembly, *wall, SemanticRelationshipKind::Aggregates));
  EXPECT_NE(std::ranges::find(wall->storeyIds, storey->id), wall->storeyIds.end());
  checkManifestReload(scene);
  EXPECT_EQ(container::robotics::serializeRoboticsScene(load()),
            container::robotics::serializeRoboticsScene(scene));
}

TEST(BimRoboticsSamples, TeklaIfcxPreservesAuthoredHierarchyWithoutInventingUnitsOrGuid) {
  const auto sample = teklaSample("ifcx");
  if (!sample)
    GTEST_SKIP() << "Set CONTAINER_IFC_SAMPLE_ROOT to the buildingSMART examples directory";
  ASSERT_TRUE(std::filesystem::is_regular_file(*sample)) << *sample;
  const RoboticsSceneOptions unknownOptions{.sourceNamespace = "buildingSMART/TeklaHouse.ifcx"};
  const auto unknownScene = [&] {
    const auto model = container::geometry::ifcx::LoadFromFile(*sample);
    // Independent JSON audit: 26,268 data rows, 11,946 class-authored
    // products in one tree and 11,945 product-child edges. The only attribute
    // keys are class, diffuseColor, xformop and mesh: no GlobalId or units.
    EXPECT_EQ(model.semanticProducts.size(), 11946u);
    EXPECT_EQ(model.semanticRelationships.size(), 11945u);
    EXPECT_FALSE(model.unitMetadata.hasSourceUnits);
    EXPECT_FALSE(model.unitMetadata.hasMetersPerUnit);
    EXPECT_TRUE(std::ranges::all_of(model.semanticProducts,
        [](const auto& product) { return product.guid.empty(); }));
    EXPECT_TRUE(std::ranges::all_of(model.semanticRelationships,
        [](const auto& relationship) {
          return relationship.kind == container::geometry::dotbim::SemanticRelationshipKind::Child;
        }));
    return container::robotics::buildRoboticsScene(model, unknownOptions);
  }();
  EXPECT_EQ(unknownScene.entities.size(), 11946u);
  EXPECT_EQ(unknownScene.relationships.size(), 11945u);
  EXPECT_FALSE(unknownScene.frame.sourceMetersPerUnit);
  EXPECT_FALSE(unknownScene.frame.metersPerRendererUnit);
  EXPECT_FALSE(unknownScene.frame.roboticsFromRenderer);
  EXPECT_TRUE(std::ranges::all_of(unknownScene.entities, [](const auto& entity) {
    return entity.guid.empty() && !entity.roboticsWorldFromLocal;
  }));
  EXPECT_FALSE(unknownScene.diagnostics.empty());
  EXPECT_TRUE(std::ranges::any_of(unknownScene.diagnostics, [](const auto& diagnostic) {
    return diagnostic.code == "missing-metric-units";
  }));
  EXPECT_TRUE(std::ranges::any_of(unknownScene.diagnostics, [](const auto& diagnostic) {
    return diagnostic.code == "missing-guid";
  }));
  checkTeklaClasses(unknownScene);

  const auto loadMetric = [&](float importScale) {
    // This is an explicit test override for this independently audited metre
    // export, never a claim that IFCX authored a unit declaration.
    const RoboticsSceneOptions options{.sourceNamespace = unknownOptions.sourceNamespace,
                                      .metersPerRendererUnit = 1.0 / importScale};
    return container::robotics::buildRoboticsScene(
        container::geometry::ifcx::LoadFromFile(*sample, importScale), options);
  };
  const auto metric = loadMetric(1.0f);
  const auto scaled = loadMetric(2.0f);
  EXPECT_FALSE(metric.frame.sourceMetersPerUnit);
  ASSERT_TRUE(metric.frame.metersPerRendererUnit);
  EXPECT_DOUBLE_EQ(*metric.frame.metersPerRendererUnit, 1.0);
  ASSERT_TRUE(scaled.frame.metersPerRendererUnit);
  EXPECT_DOUBLE_EQ(*scaled.frame.metersPerRendererUnit, .5);

  constexpr auto wallPath = "506544e0-0001-d54a-3133-343838323739";
  const auto* wall = bySourceSuffix(metric, wallPath);
  ASSERT_NE(wall, nullptr);
  ASSERT_TRUE(wall->roboticsWorldFromLocal);
  const auto& pose = *wall->roboticsWorldFromLocal;
  // Independently read from this wall's USD matrix translation, not from a
  // second importer: source (-.4900035867111969,-.189996760831853,12).
  EXPECT_NEAR(pose[3].x, -.189996760831853, 1e-12);
  EXPECT_NEAR(pose[3].y, .4900035867111969, 1e-12);
  EXPECT_NEAR(pose[3].z, 12.0, 1e-12);
  EXPECT_TRUE(wall->guid.empty());
  EXPECT_EQ(wall->storeyIds.size(), 1u);
  const auto* scaledWall = bySource(scaled, wall->sourceId);
  ASSERT_NE(scaledWall, nullptr);
  EXPECT_EQ(scaledWall->id, wall->id);
  ASSERT_TRUE(scaledWall->roboticsWorldFromLocal);
  for (glm::length_t column = 0; column < 4; ++column)
    for (glm::length_t row = 0; row < 4; ++row)
      EXPECT_NEAR((*scaledWall->roboticsWorldFromLocal)[column][row], pose[column][row], 1e-12);

  const auto* project = bySourceSuffix(metric, "e83dc660-55d6-41c1-9bb5-0201b140c897");
  const auto* site = bySourceSuffix(metric, "f154d0ce-9821-4e79-bf71-ab28427b3bec");
  const auto* building = bySourceSuffix(metric, "6a2e45fe-3860-42e8-853a-efc15ed348b5");
  const auto* storey = bySourceSuffix(metric, "1b21a592-467c-4618-8617-451be2c11e55");
  ASSERT_NE(project, nullptr);
  ASSERT_NE(site, nullptr);
  ASSERT_NE(building, nullptr);
  ASSERT_NE(storey, nullptr);
  EXPECT_TRUE(hasRelationship(metric, *project, *site, SemanticRelationshipKind::Child));
  EXPECT_TRUE(hasRelationship(metric, *site, *building, SemanticRelationshipKind::Child));
  EXPECT_TRUE(hasRelationship(metric, *building, *storey, SemanticRelationshipKind::Child));
  EXPECT_NE(std::ranges::find(wall->storeyIds, storey->id), wall->storeyIds.end());
  checkManifestReload(metric);
  EXPECT_EQ(container::robotics::serializeRoboticsScene(loadMetric(1.0f)),
            container::robotics::serializeRoboticsScene(metric));
}

} // namespace
