#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <nlohmann/json.hpp>

#include "Container/app/AppConfig.h"
#include "Container/utility/Platform.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef CONTAINER_APP_EXECUTABLE
#define CONTAINER_APP_EXECUTABLE ""
#endif

#ifndef CONTAINER_TEST_RESULTS_DIR
#define CONTAINER_TEST_RESULTS_DIR "test_results"
#endif

namespace {

using Json = nlohmann::json;

std::optional<std::string> envString(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

bool envFlag(const char* name) {
  const auto value = envString(name);
  return value && (*value == "1" || *value == "true" || *value == "TRUE");
}

std::filesystem::path repositoryRoot() {
  std::filesystem::path candidate =
      std::filesystem::absolute(__FILE__).parent_path().parent_path();
  if (std::filesystem::exists(candidate / "tests" / "fixtures" /
                              "rendering")) {
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

Json readJson(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("unable to open " + path.string());
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return Json::parse(contents.str());
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

std::filesystem::path resolveArtifactPath(std::string fixturePath,
                                          std::string_view platform) {
  fixturePath = replaceAll(std::move(fixturePath), "{platform}", platform);
  constexpr std::string_view prefix = "test_results/";
  if (fixturePath.starts_with(prefix)) {
    return std::filesystem::path(CONTAINER_TEST_RESULTS_DIR) /
           fixturePath.substr(prefix.size());
  }
  return repositoryRoot() / fixturePath;
}

std::filesystem::path visualRegressionResultPath(std::string_view platform,
                                                 std::string_view filename) {
  return std::filesystem::path(CONTAINER_TEST_RESULTS_DIR) /
         "visual-regression" / "manifest" / std::filesystem::path(platform) /
         filename;
}

void writeJsonFile(const std::filesystem::path& path, const Json& value) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    throw std::runtime_error("unable to write " + path.string());
  }
  file << value.dump(2) << '\n';
}

std::filesystem::path sceneAssetPath(std::string_view asset,
                                     const std::filesystem::path& executable) {
  if (asset == container::app::kDefaultSceneModelToken) {
    return std::filesystem::path(std::string(asset));
  }

  const std::filesystem::path requested{std::string(asset)};
  if (requested.is_absolute()) {
    return requested;
  }

  const std::array roots = {
      repositoryRoot(),
      std::filesystem::current_path(),
      executable.parent_path(),
  };
  for (const auto& root : roots) {
    const std::filesystem::path candidate = root / requested;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  return executable.parent_path() / requested;
}

std::string asCliPath(const std::filesystem::path& path) {
  return container::util::pathToUtf8(path);
}

std::string asManifestPath(const std::filesystem::path& path) {
  std::string value = container::util::pathToUtf8(path.lexically_normal());
  std::replace(value.begin(), value.end(), '\\', '/');
  return value;
}

void appendVec3(std::vector<std::string>& args, std::string_view flag,
                const Json& values) {
  args.emplace_back(flag);
  for (size_t i = 0; i < 3; ++i) {
    args.emplace_back(std::to_string(values.at(i).get<double>()));
  }
}

int runProcess(const std::filesystem::path& executable,
               const std::vector<std::string>& args) {
  std::vector<std::string> storage;
  storage.reserve(args.size() + 1);
  storage.push_back(asCliPath(executable));
  storage.insert(storage.end(), args.begin(), args.end());

  std::vector<const char*> argv;
  argv.reserve(storage.size() + 1);
  for (const std::string& value : storage) {
    argv.push_back(value.c_str());
  }
  argv.push_back(nullptr);

#ifdef _WIN32
  return _spawnv(_P_WAIT, storage.front().c_str(), argv.data());
#else
  const pid_t pid = fork();
  if (pid == 0) {
    execv(storage.front().c_str(), const_cast<char* const*>(argv.data()));
    _exit(127);
  }
  if (pid < 0) {
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return status;
#endif
}

struct Image {
  int width{0};
  int height{0};
  std::vector<unsigned char> rgba;
};

Image loadImage(const std::filesystem::path& path) {
  int width = 0;
  int height = 0;
  int channels = 0;
  stbi_uc* pixels = stbi_load(asCliPath(path).c_str(), &width, &height,
                              &channels, STBI_rgb_alpha);
  if (pixels == nullptr) {
    throw std::runtime_error("failed to load image " + path.string());
  }
  Image image;
  image.width = width;
  image.height = height;
  image.rgba.assign(pixels, pixels + static_cast<size_t>(width) *
                                      static_cast<size_t>(height) * 4u);
  stbi_image_free(pixels);
  return image;
}

double srgbToLinear(unsigned char value) {
  const double channel = static_cast<double>(value) / 255.0;
  if (channel <= 0.04045) {
    return channel / 12.92;
  }
  return std::pow((channel + 0.055) / 1.055, 2.4);
}

struct Metrics {
  double meanAbsoluteError{0.0};
  double p95AbsoluteError{0.0};
  double differentPixelFraction{0.0};
};

Json metricsToJson(const Metrics& metrics) {
  return Json{
      {"meanAbsoluteError", metrics.meanAbsoluteError},
      {"p95AbsoluteError", metrics.p95AbsoluteError},
      {"differentPixelFraction", metrics.differentPixelFraction},
  };
}

bool copyCandidateToGolden(const std::filesystem::path& candidatePath,
                           const std::filesystem::path& goldenPath,
                           bool overwrite) {
  if (!std::filesystem::exists(candidatePath)) {
    return false;
  }
  if (std::filesystem::exists(goldenPath) && !overwrite) {
    return false;
  }

  std::filesystem::create_directories(goldenPath.parent_path());
  std::filesystem::copy_file(
      candidatePath, goldenPath,
      overwrite ? std::filesystem::copy_options::overwrite_existing
                : std::filesystem::copy_options::none);
  return true;
}

std::array<unsigned char, 3> lerpHeatmapColor(
    const std::array<unsigned char, 3>& a,
    const std::array<unsigned char, 3>& b,
    double t) {
  t = std::clamp(t, 0.0, 1.0);
  return {
      static_cast<unsigned char>(std::round(
          static_cast<double>(a[0]) +
          (static_cast<double>(b[0]) - static_cast<double>(a[0])) * t)),
      static_cast<unsigned char>(std::round(
          static_cast<double>(a[1]) +
          (static_cast<double>(b[1]) - static_cast<double>(a[1])) * t)),
      static_cast<unsigned char>(std::round(
          static_cast<double>(a[2]) +
          (static_cast<double>(b[2]) - static_cast<double>(a[2])) * t)),
  };
}

std::array<unsigned char, 3> heatmapColor(double linearDiff) {
  constexpr double kHeatmapScale = 20.0;
  const double value = std::clamp(linearDiff * kHeatmapScale, 0.0, 1.0);

  struct Stop {
    double value;
    std::array<unsigned char, 3> color;
  };

  constexpr std::array<Stop, 6> kStops = {{
      {0.00, {0, 0, 0}},
      {0.12, {0, 42, 255}},
      {0.35, {0, 220, 255}},
      {0.60, {0, 255, 96}},
      {0.82, {255, 232, 0}},
      {1.00, {255, 0, 0}},
  }};

  for (size_t i = 1; i < kStops.size(); ++i) {
    if (value <= kStops[i].value) {
      const double t = (value - kStops[i - 1].value) /
                       (kStops[i].value - kStops[i - 1].value);
      return lerpHeatmapColor(kStops[i - 1].color, kStops[i].color, t);
    }
  }

  return kStops.back().color;
}

Metrics compareImages(const Image& candidate, const Image& golden,
                      const std::filesystem::path& diffPath) {
  if (candidate.width != golden.width || candidate.height != golden.height) {
    throw std::runtime_error("image dimensions differ");
  }

  const size_t pixelCount =
      static_cast<size_t>(candidate.width) * static_cast<size_t>(candidate.height);
  std::vector<double> channelDiffs;
  channelDiffs.reserve(pixelCount * 3u);
  std::vector<unsigned char> diff(pixelCount * 4u, 255);
  size_t differentPixels = 0;
  double sum = 0.0;

  for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
    double maxPixelDiff = 0.0;
    for (size_t channel = 0; channel < 3; ++channel) {
      const size_t offset = pixel * 4u + channel;
      const double delta = std::abs(srgbToLinear(candidate.rgba[offset]) -
                                    srgbToLinear(golden.rgba[offset]));
      channelDiffs.push_back(delta);
      sum += delta;
      maxPixelDiff = std::max(maxPixelDiff, delta);
    }
    const std::array<unsigned char, 3> heatColor = heatmapColor(maxPixelDiff);
    const size_t offset = pixel * 4u;
    diff[offset] = heatColor[0];
    diff[offset + 1u] = heatColor[1];
    diff[offset + 2u] = heatColor[2];
    if (maxPixelDiff > (1.0 / 255.0)) {
      ++differentPixels;
    }
  }

  std::ranges::sort(channelDiffs);
  const size_t p95Index = std::min(
      channelDiffs.size() - 1u,
      static_cast<size_t>(std::ceil(channelDiffs.size() * 0.95)) - 1u);

  std::filesystem::create_directories(diffPath.parent_path());
  if (stbi_write_png(asCliPath(diffPath).c_str(), candidate.width,
                     candidate.height, 4, diff.data(), candidate.width * 4) ==
      0) {
    throw std::runtime_error("failed to write diff image " + diffPath.string());
  }

  Metrics metrics;
  metrics.meanAbsoluteError = sum / static_cast<double>(channelDiffs.size());
  metrics.p95AbsoluteError = channelDiffs[p95Index];
  metrics.differentPixelFraction =
      static_cast<double>(differentPixels) / static_cast<double>(pixelCount);
  return metrics;
}

struct PixelRegion {
  int x{0};
  int y{0};
  int width{1};
  int height{1};
};

PixelRegion clampRegion(PixelRegion region, const Image& image) {
  region.x = std::clamp(region.x, 0, std::max(image.width - 1, 0));
  region.y = std::clamp(region.y, 0, std::max(image.height - 1, 0));
  region.width = std::clamp(region.width, 1, image.width - region.x);
  region.height = std::clamp(region.height, 1, image.height - region.y);
  return region;
}

PixelRegion probeRegion(const Json& probe, const Image& image,
                        std::string_view key) {
  const std::string pixelKey(key);
  const std::string uvKey = pixelKey + "Uv";

  if (probe.contains(uvKey)) {
    const Json& region = probe.at(uvKey);
    return clampRegion(
        PixelRegion{
            static_cast<int>(std::round(region.at(0).get<double>() *
                                        static_cast<double>(image.width))),
            static_cast<int>(std::round(region.at(1).get<double>() *
                                        static_cast<double>(image.height))),
            static_cast<int>(std::round(region.at(2).get<double>() *
                                        static_cast<double>(image.width))),
            static_cast<int>(std::round(region.at(3).get<double>() *
                                        static_cast<double>(image.height))),
        },
        image);
  }

  const Json& region = probe.at(pixelKey);
  return clampRegion(
      PixelRegion{
          region.at(0).get<int>(),
          region.at(1).get<int>(),
          region.at(2).get<int>(),
          region.at(3).get<int>(),
      },
      image);
}

struct RegionStats {
  double meanLinearLuminance{0.0};
  double stdDevLinearLuminance{0.0};
};

double linearLuminanceAt(const Image& image, int x, int y) {
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
       static_cast<size_t>(x)) *
      4u;
  return 0.2126 * srgbToLinear(image.rgba[offset]) +
         0.7152 * srgbToLinear(image.rgba[offset + 1u]) +
         0.0722 * srgbToLinear(image.rgba[offset + 2u]);
}

RegionStats luminanceStats(const Image& image, const PixelRegion& region) {
  double sum = 0.0;
  double sumSq = 0.0;
  size_t count = 0;
  for (int y = region.y; y < region.y + region.height; ++y) {
    for (int x = region.x; x < region.x + region.width; ++x) {
      const double value = linearLuminanceAt(image, x, y);
      sum += value;
      sumSq += value * value;
      ++count;
    }
  }

  const double mean = sum / static_cast<double>(count);
  const double variance =
      std::max(0.0, sumSq / static_cast<double>(count) - mean * mean);
  return {mean, std::sqrt(variance)};
}

double dominantColorFraction(const Image& image, const PixelRegion& region,
                             const Json& targetColor,
                             double maxDistance) {
  const std::array<double, 3> target = {
      targetColor.at(0).get<double>(),
      targetColor.at(1).get<double>(),
      targetColor.at(2).get<double>(),
  };

  size_t matchingPixels = 0;
  size_t totalPixels = 0;
  for (int y = region.y; y < region.y + region.height; ++y) {
    for (int x = region.x; x < region.x + region.width; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
           static_cast<size_t>(x)) *
          4u;
      const double r = static_cast<double>(image.rgba[offset]) / 255.0;
      const double g = static_cast<double>(image.rgba[offset + 1u]) / 255.0;
      const double b = static_cast<double>(image.rgba[offset + 2u]) / 255.0;
      const double dr = r - target[0];
      const double dg = g - target[1];
      const double db = b - target[2];
      if (std::sqrt(dr * dr + dg * dg + db * db) <= maxDistance) {
        ++matchingPixels;
      }
      ++totalPixels;
    }
  }

  return static_cast<double>(matchingPixels) /
         static_cast<double>(totalPixels);
}

Json evaluateImageProbes(std::string_view sceneId, const Json& probes,
                         const Image& image) {
  Json results = Json::array();
  for (const Json& probe : probes) {
    const std::string id = probe.at("id").get<std::string>();
    const std::string kind = probe.at("kind").get<std::string>();
    Json result{{"id", id}, {"kind", kind}, {"passed", true}};

    if (kind == "candidate_exists") {
      results.push_back(std::move(result));
      continue;
    }

    if (kind == "region_luminance_stddev") {
      const PixelRegion region = probeRegion(probe, image, "region");
      const RegionStats stats = luminanceStats(image, region);
      const double minimumStdDev = probe.at("minimumStdDev").get<double>();
      const bool passed = stats.stdDevLinearLuminance >= minimumStdDev;
      result["actualStdDev"] = stats.stdDevLinearLuminance;
      result["minimumStdDev"] = minimumStdDev;
      result["passed"] = passed;
      EXPECT_TRUE(passed) << sceneId << "/" << id
                          << " expected luminance stddev >= "
                          << minimumStdDev << ", got "
                          << stats.stdDevLinearLuminance;
      results.push_back(std::move(result));
      continue;
    }

    if (kind == "relative_region_luminance") {
      const PixelRegion litRegion = probeRegion(probe, image, "litRegion");
      const PixelRegion shadowRegion =
          probeRegion(probe, image, "shadowRegion");
      const RegionStats litStats = luminanceStats(image, litRegion);
      const RegionStats shadowStats = luminanceStats(image, shadowRegion);
      const double maximumRatio =
          probe.at("maximumShadowToLitRatio").get<double>();
      const double ratio =
          litStats.meanLinearLuminance > 1.0e-9
              ? shadowStats.meanLinearLuminance /
                    litStats.meanLinearLuminance
              : std::numeric_limits<double>::infinity();
      const bool passed = ratio <= maximumRatio;
      result["actualLitMeanLinearLuminance"] = litStats.meanLinearLuminance;
      result["actualShadowMeanLinearLuminance"] =
          shadowStats.meanLinearLuminance;
      result["actualShadowToLitRatio"] = ratio;
      result["maximumShadowToLitRatio"] = maximumRatio;
      result["passed"] = passed;
      EXPECT_TRUE(passed) << sceneId << "/" << id
                          << " expected shadow/lit luminance ratio <= "
                          << maximumRatio << ", got " << ratio;
      results.push_back(std::move(result));
      continue;
    }

    if (kind == "shadow_softness_gradient") {
      const PixelRegion umbraRegion = probeRegion(probe, image, "umbraRegion");
      const PixelRegion penumbraRegion =
          probeRegion(probe, image, "penumbraRegion");
      const PixelRegion litRegion = probeRegion(probe, image, "litRegion");
      const RegionStats umbraStats = luminanceStats(image, umbraRegion);
      const RegionStats penumbraStats = luminanceStats(image, penumbraRegion);
      const RegionStats litStats = luminanceStats(image, litRegion);
      const double minimumUmbraToPenumbraDelta =
          probe.at("minimumUmbraToPenumbraDelta").get<double>();
      const double minimumPenumbraToLitDelta =
          probe.at("minimumPenumbraToLitDelta").get<double>();
      const double umbraToPenumbraDelta =
          penumbraStats.meanLinearLuminance - umbraStats.meanLinearLuminance;
      const double penumbraToLitDelta =
          litStats.meanLinearLuminance - penumbraStats.meanLinearLuminance;
      const bool passed =
          umbraToPenumbraDelta >= minimumUmbraToPenumbraDelta &&
          penumbraToLitDelta >= minimumPenumbraToLitDelta;
      result["actualUmbraMeanLinearLuminance"] =
          umbraStats.meanLinearLuminance;
      result["actualPenumbraMeanLinearLuminance"] =
          penumbraStats.meanLinearLuminance;
      result["actualLitMeanLinearLuminance"] = litStats.meanLinearLuminance;
      result["actualUmbraToPenumbraDelta"] = umbraToPenumbraDelta;
      result["actualPenumbraToLitDelta"] = penumbraToLitDelta;
      result["minimumUmbraToPenumbraDelta"] = minimumUmbraToPenumbraDelta;
      result["minimumPenumbraToLitDelta"] = minimumPenumbraToLitDelta;
      result["passed"] = passed;
      EXPECT_TRUE(passed) << sceneId << "/" << id
                          << " expected soft shadow luminance ordering with "
                          << "umbra->penumbra delta >= "
                          << minimumUmbraToPenumbraDelta
                          << " and penumbra->lit delta >= "
                          << minimumPenumbraToLitDelta << ", got "
                          << umbraToPenumbraDelta << " and "
                          << penumbraToLitDelta;
      results.push_back(std::move(result));
      continue;
    }

    if (kind == "dominant_color_fraction") {
      const PixelRegion region = probeRegion(probe, image, "region");
      const double fraction = dominantColorFraction(
          image, region, probe.at("targetColor"),
          probe.at("maxDistance").get<double>());
      const double minimumFraction = probe.at("minimumFraction").get<double>();
      const bool passed = fraction >= minimumFraction;
      result["actualFraction"] = fraction;
      result["minimumFraction"] = minimumFraction;
      result["passed"] = passed;
      EXPECT_TRUE(passed) << sceneId << "/" << id
                          << " expected color fraction >= "
                          << minimumFraction << ", got " << fraction;
      results.push_back(std::move(result));
      continue;
    }

    result["passed"] = false;
    result["unsupported"] = true;
    ADD_FAILURE() << sceneId << "/" << id
                  << " unsupported visual regression probe kind " << kind;
    results.push_back(std::move(result));
  }

  return results;
}

}  // namespace

TEST(VisualRegressionGpu, WritesHeatmapDiffImages) {
  Image golden;
  golden.width = 4;
  golden.height = 1;
  golden.rgba.assign(4u * 4u, 255);

  for (size_t pixel = 0; pixel < 4u; ++pixel) {
    const size_t offset = pixel * 4u;
    golden.rgba[offset] = 0;
    golden.rgba[offset + 1u] = 0;
    golden.rgba[offset + 2u] = 0;
  }

  Image candidate = golden;
  candidate.rgba[4u] = 8;
  candidate.rgba[8u] = 32;
  candidate.rgba[12u] = 255;

  const std::filesystem::path diffPath =
      std::filesystem::temp_directory_path() /
      "container_visual_regression_tests" / "synthetic_heatmap_diff.png";

  const Metrics metrics = compareImages(candidate, golden, diffPath);
  EXPECT_GT(metrics.meanAbsoluteError, 0.0);
  ASSERT_TRUE(std::filesystem::exists(diffPath));

  const Image heatmap = loadImage(diffPath);
  ASSERT_EQ(heatmap.width, 4);
  ASSERT_EQ(heatmap.height, 1);

  auto pixel = [&](size_t index, size_t channel) {
    return heatmap.rgba[index * 4u + channel];
  };

  EXPECT_EQ(pixel(0, 0), 0);
  EXPECT_EQ(pixel(0, 1), 0);
  EXPECT_EQ(pixel(0, 2), 0);

  EXPECT_GT(pixel(1, 2), pixel(1, 0));
  EXPECT_GT(pixel(1, 2), 80);

  EXPECT_GT(pixel(2, 1), 40);
  EXPECT_GT(pixel(2, 2), 180);

  EXPECT_GT(pixel(3, 0), 220);
  EXPECT_LT(pixel(3, 1), 40);
  EXPECT_LT(pixel(3, 2), 40);
}

TEST(VisualRegressionGpu, EvaluatesRelativeRegionLuminanceProbes) {
  Image image;
  image.width = 4;
  image.height = 2;
  image.rgba.assign(4u * 2u * 4u, 255);

  auto setPixel = [&](int x, int y, unsigned char value) {
    const size_t offset =
        (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
         static_cast<size_t>(x)) *
        4u;
    image.rgba[offset] = value;
    image.rgba[offset + 1u] = value;
    image.rgba[offset + 2u] = value;
  };

  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      setPixel(x, y, x < 2 ? 220 : 80);
    }
  }

  const Json probes = Json::array({
      {
          {"id", "cornell_blocker_shadow_ratio"},
          {"kind", "relative_region_luminance"},
          {"litRegion", Json::array({0, 0, 2, 2})},
          {"shadowRegion", Json::array({2, 0, 2, 2})},
          {"maximumShadowToLitRatio", 0.35},
      },
  });

  const Json results =
      evaluateImageProbes("synthetic_cornell_box", probes, image);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.at(0).value("skipped", false));
  EXPECT_TRUE(results.at(0).at("passed").get<bool>());
  EXPECT_LT(results.at(0).at("actualShadowToLitRatio").get<double>(), 0.35);
}

TEST(VisualRegressionGpu, EvaluatesRegionLuminanceStdDevProbes) {
  Image image;
  image.width = 4;
  image.height = 4;
  image.rgba.assign(4u * 4u * 4u, 255);

  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const unsigned char value = ((x + y) % 2 == 0) ? 230 : 40;
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
           static_cast<size_t>(x)) *
          4u;
      image.rgba[offset] = value;
      image.rgba[offset + 1u] = value;
      image.rgba[offset + 2u] = value;
    }
  }

  const Json probes = Json::array({
      {
          {"id", "cascade_texel_density_debug_has_measurable_structure"},
          {"kind", "region_luminance_stddev"},
          {"region", Json::array({0, 0, 4, 4})},
          {"minimumStdDev", 0.1},
      },
  });

  const Json results =
      evaluateImageProbes("synthetic_shadow_cascade_stability", probes, image);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.at(0).value("skipped", false));
  EXPECT_TRUE(results.at(0).at("passed").get<bool>());
  EXPECT_GT(results.at(0).at("actualStdDev").get<double>(), 0.1);
}

TEST(VisualRegressionGpu, EvaluatesShadowSoftnessGradientProbes) {
  Image image;
  image.width = 6;
  image.height = 2;
  image.rgba.assign(6u * 2u * 4u, 255);

  auto setPixel = [&](int x, int y, unsigned char value) {
    const size_t offset =
        (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
         static_cast<size_t>(x)) *
        4u;
    image.rgba[offset] = value;
    image.rgba[offset + 1u] = value;
    image.rgba[offset + 2u] = value;
  };

  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const unsigned char value = x < 2 ? 50 : (x < 4 ? 135 : 225);
      setPixel(x, y, value);
    }
  }

  const Json probes = Json::array({
      {
          {"id", "cornell_blocker_shadow_has_penumbra"},
          {"kind", "shadow_softness_gradient"},
          {"umbraRegion", Json::array({0, 0, 2, 2})},
          {"penumbraRegion", Json::array({2, 0, 2, 2})},
          {"litRegion", Json::array({4, 0, 2, 2})},
          {"minimumUmbraToPenumbraDelta", 0.10},
          {"minimumPenumbraToLitDelta", 0.20},
      },
  });

  const Json results =
      evaluateImageProbes("synthetic_cornell_box", probes, image);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.at(0).value("skipped", false));
  EXPECT_TRUE(results.at(0).at("passed").get<bool>());
  EXPECT_GT(results.at(0).at("actualUmbraToPenumbraDelta").get<double>(),
            0.10);
  EXPECT_GT(results.at(0).at("actualPenumbraToLitDelta").get<double>(), 0.20);
}

TEST(VisualRegressionGpu, ReportsUnsupportedProbeKindsAsFailures) {
  Image image;
  image.width = 2;
  image.height = 2;
  image.rgba.assign(2u * 2u * 4u, 255);

  const Json probes = Json::array({
      {
          {"id", "stability_probe_not_implemented"},
          {"kind", "camera_stability_shadow_ratio"},
      },
  });

  Json results;
  EXPECT_NONFATAL_FAILURE(
      results = evaluateImageProbes("unsupported_probe_scene", probes, image),
      "unsupported visual regression probe kind");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results.at(0).value("skipped", false));
  EXPECT_FALSE(results.at(0).at("passed").get<bool>());
}

TEST(VisualRegressionGpu, ForwardMsaaPreservesOpaqueSurfaces) {
  if (!envFlag("CONTAINER_RUN_GPU_VISUAL_REGRESSION")) {
    GTEST_SKIP() << "set CONTAINER_RUN_GPU_VISUAL_REGRESSION=1 to run GPU captures";
  }
  const std::filesystem::path executable = CONTAINER_APP_EXECUTABLE;
  ASSERT_FALSE(executable.empty());
  ASSERT_TRUE(std::filesystem::exists(executable));
  const auto model = sceneAssetPath(
      "models/validation/cornell_box_yellow_area_light.gltf", executable);
  ASSERT_TRUE(std::filesystem::exists(model));
  const auto resultDir = std::filesystem::absolute(
      std::filesystem::path(CONTAINER_TEST_RESULTS_DIR) /
      "visual-regression" / "forward-msaa");
  std::filesystem::create_directories(resultDir);

  std::array<Image, 2> captures;
  for (size_t i = 0; i < captures.size(); ++i) {
    const std::string samples = i == 0u ? "1" : "4";
    const auto screenshot = resultDir / (samples + "x.png");
    // Remove any earlier result so a failed capture cannot reuse a stale image.
    std::filesystem::remove(screenshot);
    const std::vector<std::string> args{
        "--model", asCliPath(model), "--render-technique", "forward-raster",
        "--msaa", samples, "--width", "640", "--height", "360",
        "--hidden", "--no-ui", "--validation", "--screenshot",
        asCliPath(screenshot), "--warmup-frames", "8", "--capture-frame", "9",
        "--fixed-dt", "0.016666667", "--camera-position", "0", "1.05", "4.1",
        "--camera-target", "0", "0.95", "0", "--camera-fov", "36",
        "--environment-intensity", "0", "--directional-intensity", "0",
        "--exposure", "0.25", "--display-mode", "lit", "--no-bloom"};
    ASSERT_EQ(runProcess(executable, args), 0) << samples << "x MSAA";
    ASSERT_TRUE(std::filesystem::exists(screenshot));
    captures[i] = loadImage(screenshot);
    ASSERT_EQ(captures[i].width, 640);
    ASSERT_EQ(captures[i].height, 360);
  }

  // The center covers sloped opaque surfaces. Testing single-sample shading
  // against MAX-resolved MSAA depth previously made most of this region black.
  const PixelRegion opaqueRegion{192, 96, 256, 200};
  const double reference =
      luminanceStats(captures[0], opaqueRegion).meanLinearLuminance;
  const double multisampled =
      luminanceStats(captures[1], opaqueRegion).meanLinearLuminance;
  ASSERT_GT(reference, 0.05);
  EXPECT_GT(multisampled, 0.8 * reference);
  EXPECT_LT(multisampled, 1.2 * reference);
}

TEST(VisualRegressionGpu, CapturesAndComparesFixtureScenes) {
  if (!envFlag("CONTAINER_RUN_GPU_VISUAL_REGRESSION")) {
    GTEST_SKIP() << "set CONTAINER_RUN_GPU_VISUAL_REGRESSION=1 to run GPU "
                    "visual regression captures";
  }

  const auto platform = envString("CONTAINER_VISUAL_REGRESSION_PLATFORM");
  if (!platform) {
    GTEST_SKIP() << "set CONTAINER_VISUAL_REGRESSION_PLATFORM to a fixture "
                    "bucket such as windows-nvidia";
  }

  const std::filesystem::path appExecutable = CONTAINER_APP_EXECUTABLE;
  ASSERT_FALSE(appExecutable.empty());
  ASSERT_TRUE(std::filesystem::exists(appExecutable)) << appExecutable.string();

  const Json fixtures = readJson(repositoryRoot() / "tests" / "fixtures" /
                                 "rendering" /
                                 "realistic_visual_regression.fixtures.json");
  const Json& comparison = fixtures.at("comparison");
  const auto& buckets = comparison.at("platformBuckets");
  ASSERT_TRUE(std::ranges::any_of(buckets, [&](const Json& bucket) {
    return bucket.get<std::string>() == *platform;
  })) << "unknown visual regression platform bucket: " << *platform;

  const bool includePlanned =
      envFlag("CONTAINER_VISUAL_REGRESSION_INCLUDE_PLANNED");
  const bool allowMissingGoldens =
      envFlag("CONTAINER_VISUAL_REGRESSION_ALLOW_MISSING_GOLDENS");
  const bool promoteMissingGoldens =
      envFlag("CONTAINER_VISUAL_REGRESSION_PROMOTE_GOLDENS");
  const bool overwriteGoldens =
      envFlag("CONTAINER_VISUAL_REGRESSION_OVERWRITE_GOLDENS");
  const auto sceneFilter = envString("CONTAINER_VISUAL_REGRESSION_SCENE");
  const Json& capture = fixtures.at("capture");
  const auto resolution = capture.at("resolution");
  const uint32_t captureFrame = capture.at("captureFrame").get<uint32_t>();
  const uint32_t warmupFrames = capture.at("warmupFrames").get<uint32_t>();
  const double fixedDt = capture.at("fixedTimestepSeconds").get<double>();

  Json manifest{
      {"schemaVersion", 1},
      {"platform", *platform},
      {"fixture",
       asManifestPath(repositoryRoot() / "tests" / "fixtures" / "rendering" /
                      "realistic_visual_regression.fixtures.json")},
      {"appExecutable", asManifestPath(appExecutable)},
      {"capture", capture},
      {"comparison", comparison},
      {"options",
       {
           {"includePlanned", includePlanned},
           {"allowMissingGoldens", allowMissingGoldens},
           {"promoteMissingGoldens", promoteMissingGoldens},
           {"overwriteGoldens", overwriteGoldens},
           {"sceneFilter", sceneFilter.value_or("")},
       }},
      {"scenes", Json::array()},
  };

  size_t capturedScenes = 0;
  for (const Json& scene : fixtures.at("scenes")) {
    const std::string sceneId = scene.at("id").get<std::string>();
    const std::string status = scene.at("status").get<std::string>();
    if (sceneFilter && sceneId != *sceneFilter) {
      manifest["scenes"].push_back({
          {"id", sceneId},
          {"status", status},
          {"skipped", true},
          {"skipReason", "scene filter"},
      });
      continue;
    }

    if (status != "active" && !includePlanned) {
      manifest["scenes"].push_back({
          {"id", sceneId},
          {"status", status},
          {"skipped", true},
          {"skipReason", "planned scene excluded"},
      });
      continue;
    }

    const std::string asset = scene.at("asset").get<std::string>();
    const std::filesystem::path modelPath = sceneAssetPath(asset, appExecutable);

    const auto& screenshots = scene.at("screenshots");
    const std::filesystem::path candidatePath = resolveArtifactPath(
        screenshots.at("candidate").get<std::string>(), *platform);
    const std::filesystem::path goldenPath = resolveArtifactPath(
        screenshots.at("golden").get<std::string>(), *platform);
    const std::filesystem::path diffPath = resolveArtifactPath(
        screenshots.at("diff").get<std::string>(), *platform);

    Json sceneResult{
        {"id", sceneId},
        {"status", status},
        {"asset", asset},
        {"candidate", asManifestPath(candidatePath)},
        {"golden", asManifestPath(goldenPath)},
        {"diff", asManifestPath(diffPath)},
        {"captured", false},
        {"compared", false},
        {"promotedGolden", false},
        {"skipped", false},
    };

    if (asset != container::app::kDefaultSceneModelToken &&
        !std::filesystem::exists(modelPath)) {
      sceneResult["skipped"] = true;
      sceneResult["skipReason"] = "asset missing";
      if (status == "active") {
        ADD_FAILURE() << "active visual fixture asset is missing for "
                      << sceneId << ": " << modelPath.string();
      }
      manifest["scenes"].push_back(std::move(sceneResult));
      continue;
    }

    std::filesystem::create_directories(candidatePath.parent_path());
    std::vector<std::string> args;
    args.emplace_back("--model");
    args.emplace_back(asset == container::app::kDefaultSceneModelToken
                          ? asset
                          : asCliPath(modelPath));
    args.emplace_back("--width");
    args.emplace_back(std::to_string(resolution.at(0).get<uint32_t>()));
    args.emplace_back("--height");
    args.emplace_back(std::to_string(resolution.at(1).get<uint32_t>()));
    args.emplace_back("--visual-regression-capture");
    args.emplace_back(asCliPath(candidatePath));
    args.emplace_back("--warmup-frames");
    args.emplace_back(std::to_string(warmupFrames));
    args.emplace_back("--capture-frame");
    args.emplace_back(std::to_string(captureFrame));
    args.emplace_back("--fixed-dt");
    args.emplace_back(std::to_string(fixedDt));
    args.emplace_back("--no-ui");
    args.emplace_back("--hidden");
    args.emplace_back("--no-bloom");
    args.emplace_back("--display-mode");
    args.emplace_back(scene.at("renderMode").get<std::string>());
    appendVec3(args, "--camera-position", scene.at("camera").at("position"));
    appendVec3(args, "--camera-target", scene.at("camera").at("target"));
    args.emplace_back("--camera-fov");
    args.emplace_back(
        std::to_string(scene.at("camera").at("verticalFovDegrees").get<double>()));
    args.emplace_back("--exposure");
    args.emplace_back(std::to_string(scene.at("lighting").at("exposure").get<double>()));
    args.emplace_back("--environment-intensity");
    args.emplace_back(std::to_string(
        scene.at("lighting").at("environmentIntensity").get<double>()));
    if (scene.at("lighting").contains("directionalLight") &&
        scene.at("lighting").at("directionalLight").contains("illuminanceLux")) {
      const Json& directionalLight =
          scene.at("lighting").at("directionalLight");
      args.emplace_back("--directional-intensity");
      args.emplace_back(std::to_string(
          directionalLight.at("illuminanceLux").get<double>() / 10000.0));
      if (directionalLight.contains("direction")) {
        appendVec3(args, "--directional-direction",
                   directionalLight.at("direction"));
      }
      if (directionalLight.contains("color")) {
        appendVec3(args, "--directional-color", directionalLight.at("color"));
      }
    }

    const int exitCode = runProcess(appExecutable, args);
    sceneResult["processExitCode"] = exitCode;
    if (exitCode != 0) {
      ADD_FAILURE() << "GPU capture process failed for " << sceneId
                    << " with exit code " << exitCode;
      manifest["scenes"].push_back(std::move(sceneResult));
      continue;
    }
    if (!std::filesystem::exists(candidatePath)) {
      ADD_FAILURE() << "candidate was not written for " << sceneId;
      manifest["scenes"].push_back(std::move(sceneResult));
      continue;
    }
    sceneResult["captured"] = true;
    ++capturedScenes;

    std::optional<Image> candidateImage;
    try {
      candidateImage = loadImage(candidatePath);
      sceneResult["probes"] =
          evaluateImageProbes(sceneId, scene.at("probes"), *candidateImage);
    } catch (const std::exception& e) {
      sceneResult["probeError"] = e.what();
      ADD_FAILURE() << "probe evaluation failed for " << sceneId << ": "
                    << e.what();
      manifest["scenes"].push_back(std::move(sceneResult));
      continue;
    }

    if ((promoteMissingGoldens || overwriteGoldens) && status != "active") {
      sceneResult["goldenPromotionBlocked"] = true;
      ADD_FAILURE() << "refusing to promote golden for non-active fixture "
                    << sceneId
                    << "; planned validation captures are diagnostic-only "
                       "until they are made active with a reference-backed "
                       "golden";
    } else if (promoteMissingGoldens || overwriteGoldens) {
      sceneResult["promotedGolden"] =
          copyCandidateToGolden(candidatePath, goldenPath, overwriteGoldens);
    }

    if (!std::filesystem::exists(goldenPath)) {
      sceneResult["goldenMissing"] = true;
      if (!allowMissingGoldens) {
        ADD_FAILURE() << "missing golden image for " << sceneId << ": "
                      << goldenPath.string();
      }
      manifest["scenes"].push_back(std::move(sceneResult));
      continue;
    }

    Metrics metrics{};
    try {
      metrics = compareImages(*candidateImage, loadImage(goldenPath), diffPath);
    } catch (const std::exception& e) {
      sceneResult["compareError"] = e.what();
      ADD_FAILURE() << "comparison failed for " << sceneId << ": " << e.what();
      manifest["scenes"].push_back(std::move(sceneResult));
      continue;
    }
    sceneResult["compared"] = true;
    sceneResult["metrics"] = metricsToJson(metrics);
    const bool passed =
        metrics.meanAbsoluteError <=
            comparison.at("maxMeanAbsoluteError").get<double>() &&
        metrics.p95AbsoluteError <=
            comparison.at("maxP95AbsoluteError").get<double>() &&
        metrics.differentPixelFraction <=
            comparison.at("maxDifferentPixelFraction").get<double>();
    sceneResult["passed"] = passed;
    EXPECT_TRUE(passed)
        << sceneId << " metrics: " << metricsToJson(metrics).dump();
    manifest["scenes"].push_back(std::move(sceneResult));
  }

  manifest["capturedSceneCount"] = capturedScenes;
  writeJsonFile(visualRegressionResultPath(*platform, "latest.json"),
                manifest);

  EXPECT_GT(capturedScenes, 0u)
      << "no active visual regression scenes were captured";
}
