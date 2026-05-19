#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "Container/app/AppConfig.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;

std::filesystem::path repositoryRoot() {
  std::filesystem::path candidate =
      std::filesystem::absolute(__FILE__).parent_path().parent_path();
  if (std::filesystem::exists(candidate / "tests" / "fixtures" / "rendering")) {
    return candidate;
  }

  candidate = std::filesystem::current_path();
  while (!candidate.empty()) {
    if (std::filesystem::exists(candidate / "tests" / "fixtures" /
                                "rendering")) {
      return candidate;
    }
    candidate = candidate.parent_path();
  }

  return std::filesystem::absolute(__FILE__).parent_path().parent_path();
}

Json readJsonFixture(const std::filesystem::path &relativePath) {
  const std::filesystem::path path = repositoryRoot() / relativePath;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Unable to open " << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return Json::parse(contents.str());
}

std::string readTextFixture(const std::filesystem::path &relativePath) {
  const std::filesystem::path path = repositoryRoot() / relativePath;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Unable to open " << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

bool isSafeRelativePath(std::string_view value) {
  const std::filesystem::path path{std::string(value)};
  if (path.is_absolute()) {
    return false;
  }
  return value.find("..") == std::string_view::npos &&
         value.find('\\') == std::string_view::npos;
}

bool isLowerSnakeId(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

bool hasRequiredObjectFields(const Json &object,
                             std::initializer_list<std::string_view> fields) {
  return std::all_of(fields.begin(), fields.end(), [&](std::string_view field) {
    return object.contains(std::string(field));
  });
}

const Json *findSceneById(const Json &scenes, std::string_view id) {
  const auto scene = std::ranges::find_if(scenes, [&](const Json &candidate) {
    return candidate.contains("id") &&
           candidate.at("id").get<std::string>() == id;
  });
  return scene == scenes.end() ? nullptr : &*scene;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool isGpuHarnessRenderMode(std::string_view mode) {
  const std::set<std::string_view> modes{
      "final-lit",
      "albedo",
      "base-color",
      "normals",
      "material",
      "depth",
      "emissive",
      "transparency",
      "revealage",
      "overview",
      "surface-normals",
      "object-space-normals",
      "shadow-cascades",
      "tile-light-heat-map",
      "tile-light-heatmap",
      "shadow-texel-density",
  };
  return modes.contains(mode);
}

bool isGpuHarnessProbeKind(std::string_view kind) {
  const std::set<std::string_view> kinds{
      "candidate_exists",
      "region_luminance_stddev",
      "relative_region_luminance",
      "shadow_softness_gradient",
      "dominant_color_fraction",
  };
  return kinds.contains(kind);
}

std::string replaceAll(std::string value, std::string_view from,
                       std::string_view to) {
  size_t pos = 0;
  while ((pos = value.find(from, pos)) != std::string::npos) {
    value.replace(pos, from.size(), to);
    pos += to.size();
  }
  return value;
}

double pointLightAttenuation(double distanceMeters, double rangeMeters) {
  const double distanceSq = distanceMeters * distanceMeters;
  double smoothCutoff = 1.0;
  if (std::isfinite(rangeMeters) && rangeMeters > 0.0) {
    const double normalizedSq = distanceSq / (rangeMeters * rangeMeters);
    const double smoothFactor = std::clamp(1.0 - normalizedSq, 0.0, 1.0);
    smoothCutoff = smoothFactor * smoothFactor;
  }
  return smoothCutoff / std::max(distanceSq, 0.01);
}

double snapToTexel(double value, double texelSize) {
  return std::round(value / texelSize) * texelSize;
}

} // namespace

TEST(RealisticRenderingValidation, FixtureSchemaAndMetadataArePresent) {
  const Json schema = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.schema.json");
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");

  ASSERT_EQ(schema.at("properties").at("schemaVersion").at("const").get<int>(),
            1);
  ASSERT_EQ(fixtures.at("schemaVersion").get<int>(), 1);
  EXPECT_EQ(fixtures.at("screenshotRoot").get<std::string>(),
            "tests/visual-regression");

  EXPECT_TRUE(hasRequiredObjectFields(schema, {
                                                  "$schema",
                                                  "$id",
                                                  "required",
                                                  "properties",
                                                  "$defs",
                                              }));
  EXPECT_TRUE(hasRequiredObjectFields(fixtures, {
                                                    "capture",
                                                    "comparison",
                                                    "cpuChecks",
                                                    "scenes",
                                                }));
}

TEST(RealisticRenderingValidation, ScreenshotScenesFollowStableContract) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json &capture = fixtures.at("capture");
  const Json &comparison = fixtures.at("comparison");
  const Json &scenes = fixtures.at("scenes");

  ASSERT_TRUE(capture.at("resolution").is_array());
  ASSERT_EQ(capture.at("resolution").size(), 2u);
  EXPECT_EQ(capture.at("format").get<std::string>(), "png");
  EXPECT_GE(capture.at("warmupFrames").get<int>(), 2);
  EXPECT_GT(capture.at("captureFrame").get<int>(),
            capture.at("warmupFrames").get<int>());

  EXPECT_GT(comparison.at("maxMeanAbsoluteError").get<double>(), 0.0);
  EXPECT_GE(comparison.at("maxP95AbsoluteError").get<double>(),
            comparison.at("maxMeanAbsoluteError").get<double>());
  EXPECT_GT(comparison.at("maxDifferentPixelFraction").get<double>(), 0.0);
  EXPECT_LE(comparison.at("maxDifferentPixelFraction").get<double>(), 1.0);
  ASSERT_FALSE(comparison.at("platformBuckets").empty());

  ASSERT_GE(scenes.size(), 3u);
  std::set<std::string> sceneIds;
  std::set<std::string> categories;
  size_t activeSceneCount = 0;
  for (const Json &scene : scenes) {
    ASSERT_TRUE(hasRequiredObjectFields(scene, {
                                                   "id",
                                                   "category",
                                                   "status",
                                                   "asset",
                                                   "renderMode",
                                                   "camera",
                                                   "lighting",
                                                   "probes",
                                                   "screenshots",
                                               }));

    const std::string id = scene.at("id").get<std::string>();
    const std::string status = scene.at("status").get<std::string>();
    EXPECT_TRUE(status == "active" || status == "planned") << status;
    if (status == "active") {
      ++activeSceneCount;
    }
    EXPECT_TRUE(sceneIds.insert(id).second) << "duplicate scene id: " << id;
    EXPECT_TRUE(isLowerSnakeId(id)) << id;
    EXPECT_EQ(id.find('-'), std::string::npos)
        << "scene ids should use underscores, not dashes";
    categories.insert(scene.at("category").get<std::string>());

    EXPECT_TRUE(isSafeRelativePath(scene.at("asset").get<std::string>()));
    EXPECT_FALSE(scene.at("probes").empty());
    EXPECT_GT(scene.at("camera").at("verticalFovDegrees").get<double>(), 0.0);
    EXPECT_GT(scene.at("lighting").at("exposure").get<double>(), 0.0);
    EXPECT_GE(scene.at("lighting").at("environmentIntensity").get<double>(),
              0.0);

    const Json &screenshots = scene.at("screenshots");
    for (const std::string field : {"golden", "candidate", "diff"}) {
      const std::string path = screenshots.at(field).get<std::string>();
      EXPECT_TRUE(isSafeRelativePath(path)) << path;
      EXPECT_NE(path.find(id), std::string::npos) << path;
      EXPECT_NE(path.find(".png"), std::string::npos) << path;
    }
    EXPECT_NE(screenshots.at("golden").get<std::string>().find(
                  fixtures.at("screenshotRoot").get<std::string>()),
              std::string::npos);
    EXPECT_NE(screenshots.at("candidate")
                  .get<std::string>()
                  .find("test_results/visual-regression"),
              std::string::npos);
    EXPECT_NE(screenshots.at("diff").get<std::string>().find(
                  "test_results/visual-regression"),
              std::string::npos);
  }

  EXPECT_TRUE(categories.contains("material"));
  EXPECT_TRUE(categories.contains("exposure"));
  EXPECT_TRUE(categories.contains("lighting"));
  EXPECT_TRUE(categories.contains("shadow"));
  EXPECT_GE(activeSceneCount, 1u)
      << "GPU visual harness should always have at least one active fixture";
}

TEST(RealisticRenderingValidation,
     ActiveVisualFixtureGoldensExistForAdvertisedPlatforms) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json &platforms = fixtures.at("comparison").at("platformBuckets");

  ASSERT_FALSE(platforms.empty());
  for (const Json &scene : fixtures.at("scenes")) {
    if (scene.at("status").get<std::string>() != "active") {
      continue;
    }

    const std::string sceneId = scene.at("id").get<std::string>();
    const std::string goldenTemplate =
        scene.at("screenshots").at("golden").get<std::string>();
    for (const Json &platformJson : platforms) {
      const std::string platform = platformJson.get<std::string>();
      const std::string goldenPath =
          replaceAll(goldenTemplate, "{platform}", platform);
      ASSERT_TRUE(isSafeRelativePath(goldenPath)) << goldenPath;
      EXPECT_TRUE(std::filesystem::exists(repositoryRoot() / goldenPath))
          << "Missing active golden for scene " << sceneId
          << " on platform bucket " << platform << ": " << goldenPath;
    }
  }
}

TEST(RealisticRenderingValidation,
     ActiveVisualFixturesUseTraceableAssetsForGoldenReferences) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");

  for (const Json &scene : fixtures.at("scenes")) {
    if (scene.at("status").get<std::string>() != "active") {
      continue;
    }

    const std::string sceneId = scene.at("id").get<std::string>();
    const std::string asset = scene.at("asset").get<std::string>();
    const bool isExternalSampleModel =
        asset.rfind("models/glTF-Sample-Models/2.0/", 0) == 0;
    const bool isExplicitEngineDiagnostic =
        asset == "__default_test_scene__" &&
        sceneId.rfind("diagnostic_", 0) == 0;
    const bool isPromotedLocalShadowFixture =
        sceneId == "cornell_box_local_light_occlusion" &&
        asset == "models/validation/cornell_box_local_light.gltf";
    EXPECT_TRUE(isExternalSampleModel || isExplicitEngineDiagnostic ||
                isPromotedLocalShadowFixture)
        << "Active visual regression fixture " << sceneId
        << " must use a traceable external sample asset, a diagnostic_* "
           "engine diagnostic scene, or a promoted local shadow fixture with "
           "numeric probe coverage and a checked-in golden.";
  }
}

TEST(RealisticRenderingValidation,
     PlannedValidationFixtureGoldensAreNotPromoted) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json &platforms = fixtures.at("comparison").at("platformBuckets");

  for (const Json &scene : fixtures.at("scenes")) {
    if (scene.at("status").get<std::string>() != "planned") {
      continue;
    }

    const std::string asset = scene.at("asset").get<std::string>();
    if (asset.rfind("models/validation/", 0) != 0) {
      continue;
    }

    const std::string sceneId = scene.at("id").get<std::string>();
    const std::string goldenTemplate =
        scene.at("screenshots").at("golden").get<std::string>();
    for (const Json &platformJson : platforms) {
      const std::string platform = platformJson.get<std::string>();
      const std::string goldenPath =
          replaceAll(goldenTemplate, "{platform}", platform);
      ASSERT_TRUE(isSafeRelativePath(goldenPath)) << goldenPath;
      EXPECT_FALSE(std::filesystem::exists(repositoryRoot() / goldenPath))
          << "Planned local validation fixture " << sceneId
          << " must not carry a promoted renderer-generated golden for "
          << platform << ": " << goldenPath;
    }
  }
}

TEST(RealisticRenderingValidation,
     CornellBoxFixtureRemainsPlannedUntilReferenceBacked) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json *scene =
      findSceneById(fixtures.at("scenes"), "cornell_box_local_light_occlusion");
  ASSERT_NE(scene, nullptr)
      << "Cornell box must be available as a local-light occlusion fixture";

  EXPECT_EQ(scene->at("status").get<std::string>(), "active")
      << "The Cornell local-light fixture is active after numeric probe and "
         "golden-image promotion.";
  EXPECT_EQ(scene->at("category").get<std::string>(), "shadow");
  EXPECT_EQ(scene->at("renderMode").get<std::string>(), "final-lit");

  const std::string asset = scene->at("asset").get<std::string>();
  EXPECT_EQ(asset, "models/validation/cornell_box_local_light.gltf");
  EXPECT_TRUE(std::filesystem::exists(repositoryRoot() / asset))
      << "Cornell box fixture asset is missing";

  const Json &lighting = scene->at("lighting");
  EXPECT_EQ(lighting.at("environmentIntensity").get<double>(), 0.0);
  ASSERT_TRUE(lighting.contains("directionalLight"));
  EXPECT_EQ(lighting.at("directionalLight")
                .at("illuminanceLux")
                .get<double>(),
            0.0);
  ASSERT_TRUE(lighting.contains("authoredLightsFromAsset"));
  EXPECT_TRUE(lighting.at("authoredLightsFromAsset").get<bool>());

  bool foundOcclusionProbe = false;
  bool foundSoftnessProbe = false;
  for (const Json &probe : scene->at("probes")) {
    const std::string kind = probe.at("kind").get<std::string>();
    if (kind == "relative_region_luminance") {
      foundOcclusionProbe = true;
      EXPECT_TRUE(probe.contains("litRegionUv"));
      EXPECT_TRUE(probe.contains("shadowRegionUv"));
      EXPECT_LE(probe.at("maximumShadowToLitRatio").get<double>(), 0.75);
      continue;
    }
    if (kind != "shadow_softness_gradient") {
      continue;
    }
    foundSoftnessProbe = true;
    EXPECT_TRUE(probe.contains("umbraRegionUv"));
    EXPECT_TRUE(probe.contains("penumbraRegionUv"));
    EXPECT_TRUE(probe.contains("litRegionUv"));
    EXPECT_GT(probe.at("minimumUmbraToPenumbraDelta").get<double>(), 0.0);
    EXPECT_GT(probe.at("minimumPenumbraToLitDelta").get<double>(), 0.0);
  }
  EXPECT_TRUE(foundOcclusionProbe)
      << "Cornell box must compare lit and occluded regions";
  EXPECT_TRUE(foundSoftnessProbe)
      << "Cornell box must verify a soft local-light penumbra";
}

TEST(RealisticRenderingValidation,
     YellowAreaPurpleDirectionalFixtureUsesDedicatedAssetAndNumericProbes) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json *scene = findSceneById(
      fixtures.at("scenes"), "cornell_yellow_area_purple_directional_shadow");
  ASSERT_NE(scene, nullptr);

  EXPECT_EQ(scene->at("status").get<std::string>(), "planned");
  EXPECT_EQ(scene->at("category").get<std::string>(), "shadow");
  EXPECT_EQ(scene->at("renderMode").get<std::string>(), "final-lit");

  const std::string asset = scene->at("asset").get<std::string>();
  EXPECT_EQ(asset, "models/validation/cornell_box_yellow_area_light.gltf");
  EXPECT_TRUE(std::filesystem::exists(repositoryRoot() / asset))
      << "Yellow Cornell fixture asset is missing";

  const Json &lighting = scene->at("lighting");
  EXPECT_EQ(lighting.at("environmentIntensity").get<double>(), 0.0);
  EXPECT_TRUE(lighting.at("authoredLightsFromAsset").get<bool>());
  ASSERT_TRUE(lighting.contains("directionalLight"));
  const Json &directional = lighting.at("directionalLight");
  EXPECT_NEAR(directional.at("illuminanceLux").get<double>(), 18000.0, 1e-6);
  ASSERT_EQ(directional.at("direction").size(), 3u);
  EXPECT_NEAR(directional.at("direction").at(0).get<double>(), -0.36, 1e-6);
  EXPECT_NEAR(directional.at("direction").at(1).get<double>(), -0.86, 1e-6);
  EXPECT_NEAR(directional.at("direction").at(2).get<double>(), -0.36, 1e-6);
  ASSERT_EQ(directional.at("color").size(), 3u);
  EXPECT_NEAR(directional.at("color").at(0).get<double>(), 0.62, 1e-6);
  EXPECT_NEAR(directional.at("color").at(1).get<double>(), 0.35, 1e-6);
  EXPECT_NEAR(directional.at("color").at(2).get<double>(), 1.0, 1e-6);

  const auto targetMatches = [](const Json &color, double r, double g,
                                double b) {
    return color.size() == 3u &&
           std::abs(color.at(0).get<double>() - r) <= 1e-6 &&
           std::abs(color.at(1).get<double>() - g) <= 1e-6 &&
           std::abs(color.at(2).get<double>() - b) <= 1e-6;
  };

  bool foundCandidateExistsProbe = false;
  bool foundYellowProbe = false;
  bool foundPurpleProbe = false;
  size_t dominantColorProbeCount = 0;
  for (const Json &probe : scene->at("probes")) {
    const std::string kind = probe.at("kind").get<std::string>();
    if (kind == "candidate_exists") {
      foundCandidateExistsProbe = true;
      continue;
    }
    if (kind != "dominant_color_fraction") {
      continue;
    }

    ++dominantColorProbeCount;
    ASSERT_TRUE(probe.contains("targetColor"));
    const Json &targetColor = probe.at("targetColor");
    foundYellowProbe =
        foundYellowProbe || targetMatches(targetColor, 1.0, 0.86, 0.10);
    foundPurpleProbe =
        foundPurpleProbe || targetMatches(targetColor, 0.62, 0.35, 1.0);
  }

  EXPECT_FALSE(foundCandidateExistsProbe)
      << "Mixed yellow-area/purple-directional fixture needs numeric probes";
  EXPECT_GE(dominantColorProbeCount, 2u);
  EXPECT_TRUE(foundYellowProbe);
  EXPECT_TRUE(foundPurpleProbe);
}

TEST(RealisticRenderingValidation,
     ShadowCascadeStabilityFixtureUsesDirectionalNumericCoverage) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json *scene =
      findSceneById(fixtures.at("scenes"), "shadow_cascade_stability");
  ASSERT_NE(scene, nullptr);

  EXPECT_EQ(scene->at("status").get<std::string>(), "planned");
  EXPECT_EQ(scene->at("category").get<std::string>(), "shadow");
  EXPECT_EQ(scene->at("renderMode").get<std::string>(),
            "shadow-texel-density");
  ASSERT_TRUE(scene->at("lighting").contains("directionalLight"));
  EXPECT_GT(scene->at("lighting")
                .at("directionalLight")
                .at("illuminanceLux")
                .get<double>(),
            0.0);

  bool foundCandidateExistsProbe = false;
  bool foundTexelDensityStdDevProbe = false;
  for (const Json &probe : scene->at("probes")) {
    const std::string kind = probe.at("kind").get<std::string>();
    if (kind == "candidate_exists") {
      foundCandidateExistsProbe = true;
      continue;
    }
    if (kind == "region_luminance_stddev") {
      foundTexelDensityStdDevProbe = true;
      EXPECT_TRUE(probe.contains("regionUv"));
      EXPECT_GT(probe.at("minimumStdDev").get<double>(), 0.0);
    }
  }

  EXPECT_FALSE(foundCandidateExistsProbe)
      << "Cascade stability needs numeric debug-image coverage";
  EXPECT_TRUE(foundTexelDensityStdDevProbe);
}

TEST(RealisticRenderingValidation,
     ShadowCorrectnessFixturesCoverOpenAndClosedWorlds) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");

  const Json *openScene =
      findSceneById(fixtures.at("scenes"), "open_shadow_wall_blocker");
  const Json *closedScene =
      findSceneById(fixtures.at("scenes"), "closed_shadow_room_blocker");

  ASSERT_NE(openScene, nullptr);
  ASSERT_NE(closedScene, nullptr);

  for (const Json *scene : {openScene, closedScene}) {
    EXPECT_EQ(scene->at("category").get<std::string>(), "shadow");
    EXPECT_EQ(scene->at("renderMode").get<std::string>(), "final-lit");
    ASSERT_TRUE(scene->contains("probes"));

    const std::string asset = scene->at("asset").get<std::string>();
    EXPECT_TRUE(std::filesystem::exists(repositoryRoot() / asset))
        << "Shadow correctness fixture asset is missing: " << asset;

    bool foundRatioProbe = false;
    for (const Json &probe : scene->at("probes")) {
      const std::string kind = probe.at("kind").get<std::string>();
      if (kind == "relative_region_luminance") {
        foundRatioProbe = true;
        EXPECT_TRUE(probe.contains("litRegionUv"));
        EXPECT_TRUE(probe.contains("shadowRegionUv"));
        EXPECT_LE(probe.at("maximumShadowToLitRatio").get<double>(), 0.75);
      }
      EXPECT_TRUE(isGpuHarnessProbeKind(kind))
          << scene->at("id").get<std::string>()
          << " declares unsupported GPU probe kind " << kind;
    }
    EXPECT_TRUE(foundRatioProbe);
  }
}

TEST(RealisticRenderingValidation,
     ShadowFixturesOnlyDeclareGpuHarnessExecutableChecks) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");

  for (const Json &scene : fixtures.at("scenes")) {
    if (scene.at("category").get<std::string>() != "shadow") {
      continue;
    }

    const std::string sceneId = scene.at("id").get<std::string>();
    const std::string renderMode = scene.at("renderMode").get<std::string>();
    EXPECT_TRUE(isGpuHarnessRenderMode(renderMode))
        << sceneId << " uses unsupported GPU capture render mode "
        << renderMode;
    EXPECT_FALSE(scene.contains("motionPath"))
        << sceneId << " declares camera motion, but the GPU harness captures "
        << "only scene.camera";
    EXPECT_FALSE(scene.at("lighting").contains("shadow"))
        << sceneId << " declares shadow settings that the GPU capture CLI "
        << "does not apply";

    for (const Json &probe : scene.at("probes")) {
      const std::string kind = probe.at("kind").get<std::string>();
      EXPECT_TRUE(isGpuHarnessProbeKind(kind))
          << sceneId << " declares unsupported GPU probe kind " << kind;
    }
  }
}

TEST(RealisticRenderingValidation,
     CornellBoxAssetDeclaresExactlyOneSoftAuthoredAreaLight) {
  const Json cornell =
      readJsonFixture("models/validation/cornell_box_local_light.gltf");

  ASSERT_TRUE(cornell.contains("extensionsUsed"));
  EXPECT_TRUE(std::ranges::any_of(cornell.at("extensionsUsed"),
                                  [](const Json &extension) {
                                    return extension.get<std::string>() ==
                                           "KHR_lights_punctual";
                                  }));

  const Json &lights =
      cornell.at("extensions").at("KHR_lights_punctual").at("lights");
  ASSERT_EQ(lights.size(), 1u);
  EXPECT_EQ(lights.at(0).at("type").get<std::string>(), "point");
  EXPECT_NEAR(lights.at(0).at("intensity").get<double>(), 300.0, 1e-6);
  EXPECT_GT(lights.at(0).at("range").get<double>(), 0.0);
  ASSERT_TRUE(lights.at(0).contains("extras"));
  const Json &area =
      lights.at(0).at("extras").at("areaLight");
  EXPECT_EQ(area.at("shape").get<std::string>(), "rect");
  EXPECT_NEAR(area.at("width").get<double>(), 0.8, 1e-6);
  EXPECT_NEAR(area.at("height").get<double>(), 0.8, 1e-6);
  EXPECT_NEAR(area.at("range").get<double>(), 3.2, 1e-6);

  size_t punctualLightNodeCount = 0;
  for (const Json &node : cornell.at("nodes")) {
    const auto extension = node.find("extensions");
    if (extension == node.end()) {
      continue;
    }
    const auto punctual = extension->find("KHR_lights_punctual");
    if (punctual == extension->end()) {
      continue;
    }
    ++punctualLightNodeCount;
    EXPECT_EQ(punctual->at("light").get<int>(), 0);
    ASSERT_TRUE(node.contains("rotation"));
    EXPECT_NEAR(node.at("rotation").at(0).get<double>(), -0.70710678, 1e-6);
    EXPECT_NEAR(node.at("rotation").at(3).get<double>(), 0.70710678, 1e-6);
  }
  EXPECT_EQ(punctualLightNodeCount, 1u);
}

TEST(RealisticRenderingValidation,
     CornellYellowAreaAssetDeclaresYellowSoftAuthoredAreaLight) {
  const Json cornell =
      readJsonFixture("models/validation/cornell_box_yellow_area_light.gltf");

  EXPECT_EQ(cornell.at("asset").at("generator").get<std::string>(),
            "Container Cornell box yellow area validation fixture");
  const Json &lights =
      cornell.at("extensions").at("KHR_lights_punctual").at("lights");
  ASSERT_EQ(lights.size(), 1u);
  const Json &light = lights.at(0);
  EXPECT_EQ(light.at("name").get<std::string>(),
            "Cornell ceiling yellow soft area light");
  EXPECT_EQ(light.at("type").get<std::string>(), "point");
  ASSERT_EQ(light.at("color").size(), 3u);
  EXPECT_NEAR(light.at("color").at(0).get<double>(), 1.0, 1e-6);
  EXPECT_NEAR(light.at("color").at(1).get<double>(), 0.86, 1e-6);
  EXPECT_NEAR(light.at("color").at(2).get<double>(), 0.10, 1e-6);
  EXPECT_NEAR(light.at("intensity").get<double>(), 300.0, 1e-6);
  EXPECT_NEAR(light.at("range").get<double>(), 3.2, 1e-6);

  ASSERT_TRUE(light.contains("extras"));
  const Json &area = light.at("extras").at("areaLight");
  EXPECT_EQ(area.at("shape").get<std::string>(), "rect");
  EXPECT_NEAR(area.at("width").get<double>(), 0.8, 1e-6);
  EXPECT_NEAR(area.at("height").get<double>(), 0.8, 1e-6);
  EXPECT_NEAR(area.at("range").get<double>(), 3.2, 1e-6);

  size_t punctualLightNodeCount = 0;
  for (const Json &node : cornell.at("nodes")) {
    const auto extension = node.find("extensions");
    if (extension == node.end()) {
      continue;
    }
    const auto punctual = extension->find("KHR_lights_punctual");
    if (punctual == extension->end()) {
      continue;
    }

    ++punctualLightNodeCount;
    EXPECT_EQ(node.at("name").get<std::string>(),
              "Ceiling yellow soft area light");
    EXPECT_EQ(punctual->at("light").get<int>(), 0);
    ASSERT_TRUE(node.contains("rotation"));
    EXPECT_NEAR(node.at("rotation").at(0).get<double>(), -0.70710678, 1e-6);
    EXPECT_NEAR(node.at("rotation").at(3).get<double>(), 0.70710678, 1e-6);
  }
  EXPECT_EQ(punctualLightNodeCount, 1u);
  EXPECT_EQ(cornell.at("scenes").at(0).at("name").get<std::string>(),
            "Cornell box yellow area purple directional shadow");
}

TEST(RealisticRenderingValidation,
     DefaultCornellStartupSuppressesGlobalFillLights) {
  const auto config = container::app::DefaultAppConfig();
  EXPECT_EQ(config.modelPath, container::app::kDefaultModelRelativePath);
  EXPECT_TRUE(container::app::IsDefaultAuthoredLocalLightScene(config.modelPath));
  EXPECT_TRUE(container::app::IsDefaultAuthoredLocalLightScene(
      "models\\validation\\cornell_box_local_light.gltf"));
  EXPECT_TRUE(container::app::IsDefaultAuthoredLocalLightScene(
      (repositoryRoot() / "models/validation/cornell_box_local_light.gltf")
          .string()));
  EXPECT_FALSE(container::app::IsDefaultAuthoredLocalLightScene(
      "models/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf"));
  EXPECT_EQ(container::app::kDefaultAuthoredLocalLightEnvironmentIntensity, 0.0f);
  EXPECT_EQ(container::app::kDefaultAuthoredLocalLightDirectionalIntensity, 0.0f);
  EXPECT_FALSE(config.hasBloomEnabledOverride);
  EXPECT_TRUE(config.bloomEnabled);

  const std::string rendererFrontend =
      readTextFixture("src/renderer/core/RendererFrontend.cpp");
  EXPECT_TRUE(contains(rendererFrontend, "IsDefaultAuthoredLocalLightScene"));
  EXPECT_TRUE(contains(rendererFrontend, "authoredPointLights().empty()"));
  EXPECT_TRUE(contains(rendererFrontend, "authoredAreaLights().empty()"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "kDefaultAuthoredLocalLightEnvironmentIntensity"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "kDefaultAuthoredLocalLightDirectionalIntensity"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "kDefaultAuthoredLocalLightBloomEnabled"));
}

TEST(RealisticRenderingValidation, FixtureCpuChecksMatchRenderingMath) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json &cpuChecks = fixtures.at("cpuChecks");

  for (const Json &sample : cpuChecks.at("manualExposure")) {
    const double luminance = sample.at("inputLuminance").get<double>();
    const double exposure = sample.at("exposure").get<double>();
    const double expected = sample.at("expectedLuminance").get<double>();
    EXPECT_NEAR(luminance * exposure, expected, 1e-12);
  }

  const Json &attenuation = cpuChecks.at("pointLightAttenuation");
  const double nearAttenuation =
      pointLightAttenuation(attenuation.at("nearDistanceMeters").get<double>(),
                            attenuation.at("rangeMeters").get<double>());
  const double farAttenuation =
      pointLightAttenuation(attenuation.at("farDistanceMeters").get<double>(),
                            attenuation.at("rangeMeters").get<double>());
  ASSERT_GT(farAttenuation, 0.0);
  EXPECT_NEAR(nearAttenuation / farAttenuation,
              attenuation.at("expectedNearToFarRatio").get<double>(), 1e-3);

  const Json &shadow = cpuChecks.at("shadowTexelSnap");
  const double texelSize =
      (shadow.at("cascadeWorldRadius").get<double>() * 2.0) /
      shadow.at("shadowMapSize").get<double>();
  EXPECT_NEAR(texelSize, shadow.at("expectedTexelSize").get<double>(), 1e-12);

  const Json &center = shadow.at("unsnappedLightSpaceCenter");
  const double snappedX = snapToTexel(center.at(0).get<double>(), texelSize);
  const double snappedY = snapToTexel(center.at(1).get<double>(), texelSize);
  const double maxError =
      shadow.at("maxSnapErrorTexels").get<double>() * texelSize;
  EXPECT_LE(std::abs(snappedX - center.at(0).get<double>()), maxError);
  EXPECT_LE(std::abs(snappedY - center.at(1).get<double>()), maxError);

  for (const Json &material : cpuChecks.at("materialExpectations")) {
    ASSERT_TRUE(hasRequiredObjectFields(material, {
                                                      "name",
                                                      "baseColor",
                                                      "metallic",
                                                      "roughness",
                                                      "expectedClass",
                                                  }));
    EXPECT_EQ(material.at("baseColor").size(), 3u);
    for (const Json &channel : material.at("baseColor")) {
      EXPECT_GE(channel.get<double>(), 0.0);
      EXPECT_LE(channel.get<double>(), 1.0);
    }
    EXPECT_GE(material.at("metallic").get<double>(), 0.0);
    EXPECT_LE(material.at("metallic").get<double>(), 1.0);
    EXPECT_GE(material.at("roughness").get<double>(), 0.0);
    EXPECT_LE(material.at("roughness").get<double>(), 1.0);
  }
}
