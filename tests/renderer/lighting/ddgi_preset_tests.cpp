#include "Container/renderer/lighting/DdgiPreset.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "Container/utility/Platform.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace container::renderer;
using Json = nlohmann::json;

DdgiPreset variedPreset() {
  DdgiPreset preset{"fixture scene"};
  auto &settings = preset.settings;
  settings.enabled = true;
  settings.paused = true;
  settings.volume = {{-7.f, 2.f, -.5f}, {.25f, .5f, 1.f}, {4, 6, 8}};
  settings.additionalVolumes = {
      {{-10.f, -2.f, -10.f}, {2.f, 2.f, 2.f}, {9, 9, 9}},
      {{-3.f, 2.f, 1.f}, {.1f, .2f, .3f}, {5, 6, 7}},
      {{0.f, 0.f, 0.f}, {.5f, .5f, .5f}, {3, 3, 3}}};
  settings.volumeBlendCells = 2.5f;
  settings.thinGlassTransmission = true;
  settings.dielectricTransmission = true;
  settings.cameraFollow = true;
  settings.asyncCompute = true;
  settings.resources.irradianceInteriorTexels = 8;
  settings.resources.visibilityInteriorTexels = 16;
  settings.resources.updates = {37, 256, 4096};
  settings.maxDistance = 123.75f;
  settings.hysteresis = .875f;
  settings.normalBias = .125f;
  settings.viewBias = .0625f;
  settings.visibilityBias = .25f;
  settings.intensity = 2.25f;
  settings.debugView = DdgiDebugView::Relocation;
  settings.debugProbeRadius = .25f;
  settings.payloadBudgetBytes = 32ull * 1024 * 1024;
  settings.surfaceBudgetBytes = 96ull * 1024 * 1024;
  return preset;
}

class PresetDirectory {
public:
  explicit PresetDirectory(const std::filesystem::path &parent =
                               std::filesystem::temp_directory_path()) {
    static std::atomic<uint64_t> serial{0};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = parent / ("container-ddgi-preset-test-" + std::to_string(tick) +
                     "-" + std::to_string(serial.fetch_add(1)));
    if (!std::filesystem::create_directory(path))
      throw std::runtime_error("cannot create unique preset test directory");
  }
  ~PresetDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

class CurrentDirectory {
public:
  explicit CurrentDirectory(const std::filesystem::path &path)
      : previous_(std::filesystem::current_path()) {
    std::filesystem::current_path(path);
  }
  ~CurrentDirectory() {
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
  }

private:
  std::filesystem::path previous_;
};

#ifdef _WIN32
// Readers that omit FILE_SHARE_DELETE temporarily prevent atomic replacement
// on Windows. The helper owns this native handle independently of writer locks.
class RenameBlockingReader {
public:
  explicit RenameBlockingReader(const std::filesystem::path &path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error(
          "cannot open replacement-blocking preset reader");
  }
  ~RenameBlockingReader() { release(); }
  void release() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};
#endif

void writeText(const std::filesystem::path &path, std::string_view text) {
  std::ofstream file(path, std::ios::binary);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  ASSERT_TRUE(file.good());
}

TEST(DdgiPresetTests, RoundTripsEverySettingAndAllFourVolumes) {
  const auto preset = variedPreset();
  const auto encoded = encodeDdgiPreset(preset);
  EXPECT_LT(encoded.size(), kMaxDdgiPresetBytes);
  EXPECT_EQ(decodeDdgiPreset(encoded), preset);
  EXPECT_EQ(encodeDdgiPreset(decodeDdgiPreset(encoded)), encoded);
  EXPECT_EQ(decodeDdgiPreset(encodeDdgiPreset({"default scene"})),
            (DdgiPreset{"default scene"}));
}

TEST(DdgiPresetTests, MigratesVersionOneWithoutEnablingNewTransportOrScheduling) {
  auto preset = variedPreset();
  preset.settings.dielectricTransmission = false;
  preset.settings.cameraFollow = false;
  preset.settings.asyncCompute = false;
  auto document = Json::parse(encodeDdgiPreset(preset));
  document["version"] = 1;
  for (const auto key : {"dielectricTransmission", "cameraFollow", "asyncCompute"})
    document["settings"].erase(key);
  EXPECT_EQ(decodeDdgiPreset(document.dump()), preset);
  EXPECT_EQ(Json::parse(encodeDdgiPreset(decodeDdgiPreset(document.dump())))["version"], 2);
  document["settings"]["asyncCompute"] = true;
  EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument);
}

TEST(DdgiPresetTests, VersionTwoRequiresBooleanExtendedControls) {
  const auto baseline = Json::parse(encodeDdgiPreset(variedPreset()));
  for (const auto key : {"dielectricTransmission", "cameraFollow", "asyncCompute"}) {
    auto document = baseline;
    document["settings"].erase(key);
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument);
    document = baseline;
    document["settings"][key] = 1;
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument);
  }
}

TEST(DdgiPresetTests, PreservesExactUnsigned64BitBudgets) {
  auto preset = variedPreset();
  preset.settings.payloadBudgetBytes = std::numeric_limits<uint64_t>::max();
  preset.settings.surfaceBudgetBytes = std::numeric_limits<uint64_t>::max() - 1;
  EXPECT_EQ(decodeDdgiPreset(encodeDdgiPreset(preset)), preset);
}

TEST(DdgiPresetTests, RejectsUnsupportedAndNonintegerVersions) {
  auto document = Json::parse(encodeDdgiPreset({"scene"}));
  for (const Json version : {Json(0), Json(3), Json(-1), Json(1.0), Json("1"),
                             Json(true), Json(nullptr)}) {
    document["version"] = version;
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()),
                 std::invalid_argument);
  }
}

TEST(DdgiPresetTests, RejectsMissingAndUnknownFieldsAtEachSchemaLevel) {
  const auto original = Json::parse(encodeDdgiPreset({"scene"}));
  for (const auto pointer :
       {"", "/settings", "/settings/volume", "/settings/resources",
        "/settings/resources/updates"}) {
    auto document = original;
    auto &object = document[Json::json_pointer(pointer)];
    object["unknown"] = 1;
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()),
                 std::invalid_argument);
    document = original;
    auto &required = document[Json::json_pointer(pointer)];
    required.erase(required.begin());
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()),
                 std::invalid_argument);
  }
}

TEST(DdgiPresetTests, RejectsDuplicateRootAndNestedKeys) {
  auto encoded = encodeDdgiPreset({"scene"});
  encoded.insert(encoded.find('{') + 1, "\"version\":1,");
  EXPECT_THROW((void)decodeDdgiPreset(encoded), std::invalid_argument);
  encoded = encodeDdgiPreset({"scene"});
  const auto enabled = encoded.find("\"enabled\"");
  ASSERT_NE(enabled, std::string::npos);
  encoded.insert(enabled, "\"enabled\":true,");
  EXPECT_THROW((void)decodeDdgiPreset(encoded), std::invalid_argument);
  // Unicode escapes must compare as decoded keys, not their source spelling.
  encoded = encodeDdgiPreset({"scene"});
  encoded.insert(encoded.find('{') + 1, "\"vers\\u0069on\":1,");
  EXPECT_THROW((void)decodeDdgiPreset(encoded), std::invalid_argument);
}

TEST(DdgiPresetTests, RejectsWrongTypesFractionsAndIntegerOverflow) {
  const auto original = Json::parse(encodeDdgiPreset({"scene"}));
  for (const auto [field, value] : std::vector<std::pair<std::string, Json>>{
           {"/sceneIdentity", 1},
           {"/settings/enabled", 1},
           {"/settings/paused", "false"},
           {"/settings/thinGlassTransmission", 1},
           {"/settings/intensity", true},
           {"/settings/maxDistance", "20"},
           {"/settings/volume/counts/0", 3.5},
           {"/settings/volume/counts/0", -1},
           {"/settings/volume/counts/0", uint64_t(1) << 32},
           {"/settings/resources/updates/raysPerProbe", 128.0},
           {"/settings/payloadBudgetBytes", 1.5},
           {"/settings/surfaceBudgetBytes", -1},
           {"/settings/debugView", 1.0}}) {
    auto document = original;
    document[Json::json_pointer(field)] = value;
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument)
        << field;
  }
  auto encoded = encodeDdgiPreset({"scene"});
  const auto field = encoded.find("\"payloadBudgetBytes\"");
  const auto start = encoded.find(':', field) + 1;
  const auto end = encoded.find(',', start);
  encoded.replace(start, end - start, "18446744073709551616");
  EXPECT_THROW((void)decodeDdgiPreset(encoded), std::invalid_argument);
}

TEST(DdgiPresetTests, RejectsWrongVectorSizesAndExcessVolumes) {
  const auto original = Json::parse(encodeDdgiPreset({"scene"}));
  for (const auto field : {"origin", "spacing", "counts"})
    for (const Json vector : {Json::array(), Json::array({1, 2}),
                              Json::array({1, 2, 3, 4}), Json(1)}) {
      auto document = original;
      document["settings"]["volume"][field] = vector;
      EXPECT_THROW((void)decodeDdgiPreset(document.dump()),
                   std::invalid_argument);
    }
  auto document = original;
  document["settings"]["additionalVolumes"] = Json::array();
  for (int i = 0; i < 4; ++i)
    document["settings"]["additionalVolumes"].push_back(
        document["settings"]["volume"]);
  EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument);
  document["settings"]["additionalVolumes"] = "not an array";
  EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument);
}

TEST(DdgiPresetTests, RejectsNonfiniteOverflowAndInvalidSettings) {
  const auto original = Json::parse(encodeDdgiPreset({"scene"}));
  for (const auto [field, value] : std::vector<std::pair<std::string, Json>>{
           {"/settings/volume/origin/0", 1e39},
           {"/settings/volume/spacing/0", 0},
           {"/settings/volume/counts/0", 0},
           {"/settings/resources/updates/raysPerProbe", 0},
           {"/settings/volumeBlendCells", 8.1},
           {"/settings/hysteresis", 1},
           {"/settings/normalBias", 21},
           {"/settings/payloadBudgetBytes", 1},
           {"/settings/debugView", 5}}) {
    auto document = original;
    document[Json::json_pointer(field)] = value;
    EXPECT_THROW((void)decodeDdgiPreset(document.dump()), std::invalid_argument)
        << field;
  }
  auto encoded = encodeDdgiPreset({"scene"});
  const auto field = encoded.find("\"maxDistance\"");
  const auto start = encoded.find(':', field) + 1;
  const auto end = encoded.find(',', start);
  encoded.replace(start, end - start, "1e400");
  EXPECT_THROW((void)decodeDdgiPreset(encoded), std::invalid_argument);
  auto preset = variedPreset();
  preset.settings.intensity = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)encodeDdgiPreset(preset), std::invalid_argument);
}

TEST(DdgiPresetTests, BoundsInputSizeNestingAndIdentity) {
  EXPECT_THROW((void)decodeDdgiPreset(""), std::invalid_argument);
  EXPECT_THROW(
      (void)decodeDdgiPreset(std::string(kMaxDdgiPresetBytes + 1, ' ')),
      std::invalid_argument);
  EXPECT_THROW(
      (void)decodeDdgiPreset(std::string(17, '[') + "0" + std::string(17, ']')),
      std::invalid_argument);
  EXPECT_THROW((void)decodeDdgiPreset("{} trailing"), std::invalid_argument);
  EXPECT_THROW((void)encodeDdgiPreset({""}), std::invalid_argument);
  EXPECT_THROW((void)encodeDdgiPreset(
                   {std::string(kMaxDdgiSceneIdentityBytes + 1, 'a')}),
               std::invalid_argument);
  EXPECT_THROW((void)encodeDdgiPreset({std::string("a\0b", 3)}),
               std::invalid_argument);
  const DdgiPreset boundary{std::string(kMaxDdgiSceneIdentityBytes, 'a')};
  EXPECT_EQ(decodeDdgiPreset(encodeDdgiPreset(boundary)), boundary);
  const DdgiPreset quoted{"{{[[\"\\\" braces inside a string ]]}}"};
  EXPECT_EQ(decodeDdgiPreset(encodeDdgiPreset(quoted)), quoted);
}

TEST(DdgiPresetTests, SaveLoadAndReplaceAnExistingPreset) {
  PresetDirectory directory;
  const auto path = directory.path / "nested" / "scene.json";
  saveDdgiPreset(path, {"scene"});
  EXPECT_EQ(loadDdgiPreset(path), (DdgiPreset{"scene"}));
  const auto changed = variedPreset();
  saveDdgiPreset(path, changed);
  EXPECT_EQ(loadDdgiPreset(path), changed);
  EXPECT_EQ(
      std::distance(std::filesystem::directory_iterator(path.parent_path()),
                    std::filesystem::directory_iterator()),
      1);
}

TEST(DdgiPresetTests, InvalidSaveKeepsPreviouslySavedContents) {
  PresetDirectory directory;
  const auto path = directory.path / "scene.json";
  const auto original = variedPreset();
  saveDdgiPreset(path, original);
  auto invalid = original;
  invalid.settings.resources.updates.raysPerProbe = 0;
  EXPECT_THROW(saveDdgiPreset(path, invalid), std::invalid_argument);
  EXPECT_EQ(loadDdgiPreset(path), original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),
                          std::filesystem::directory_iterator()),
            1);
}

TEST(DdgiPresetTests,
     ReplacementFailurePreservesDestinationAndCleansTemporary) {
  PresetDirectory directory;
  const auto destination = directory.path / "scene.json";
  std::filesystem::create_directory(destination);
  writeText(destination / "keep.txt", "existing content");
  EXPECT_THROW(saveDdgiPreset(destination, {"scene"}), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_regular_file(destination / "keep.txt"));
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),
                          std::filesystem::directory_iterator()),
            1);
}

TEST(DdgiPresetTests, FailedLoadDoesNotModifyAnExistingConfiguration) {
  PresetDirectory directory;
  const auto path = directory.path / "scene.json";
  const auto original = variedPreset();
  auto current = original.settings;
  writeText(path, "{\"version\":2}");
  EXPECT_THROW(current = loadDdgiPreset(path).settings, std::invalid_argument);
  EXPECT_EQ(current, original.settings);
  writeText(path, std::string(kMaxDdgiPresetBytes + 1, ' '));
  EXPECT_THROW((void)loadDdgiPreset(path), std::invalid_argument);
  EXPECT_THROW((void)loadDdgiPreset(directory.path / "missing.json"),
               std::runtime_error);
}

TEST(DdgiPresetTests,
     ConcurrentSaveLeavesOneCompletePresetAndNoTemporaryFiles) {
  PresetDirectory directory;
  const auto path = directory.path / "scene.json";
  const DdgiPreset first{"first scene"};
  const auto second = variedPreset();
  std::atomic<unsigned> failures{0};
  std::mutex errorsMutex;
  std::string errors;
  std::vector<std::thread> writers;
  for (unsigned index = 0; index < 8; ++index)
    writers.emplace_back([&, index] {
      try {
        saveDdgiPreset(path, index % 2 ? first : second);
      } catch (const std::exception &error) {
        ++failures;
        const std::lock_guard lock(errorsMutex);
        errors += std::string(error.what()) + '\n';
      } catch (...) {
        ++failures;
        const std::lock_guard lock(errorsMutex);
        errors += "unknown write failure\n";
      }
    });
  for (auto &writer : writers)
    writer.join();
  EXPECT_EQ(failures.load(), 0u) << errors;
  const auto final = loadDdgiPreset(path);
  EXPECT_TRUE(final == first || final == second);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),
                          std::filesystem::directory_iterator()),
            1);
}

#ifdef _WIN32
TEST(DdgiPresetTests, WindowsTemporaryReaderLockRetriesThenReplacesPreset) {
  PresetDirectory directory;
  const auto path = directory.path / "scene.json";
  saveDdgiPreset(path, {"original scene"});
  const auto replacement = variedPreset();
  RenameBlockingReader reader(path);
  std::jthread releaseReader([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    reader.release();
  });
  EXPECT_NO_THROW(saveDdgiPreset(path, replacement));
  releaseReader.join();
  EXPECT_EQ(loadDdgiPreset(path), replacement);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),
                          std::filesystem::directory_iterator()),
            1);
}

TEST(DdgiPresetTests,
     WindowsPersistentReaderLockFailsBoundedlyAndPreservesOriginal) {
  PresetDirectory directory;
  const auto path = directory.path / "scene.json";
  const DdgiPreset original{"original scene"};
  saveDdgiPreset(path, original);
  RenameBlockingReader reader(path);
  auto attempt = std::async(std::launch::async, [&] {
    try {
      saveDdgiPreset(path, variedPreset());
      return false;
    } catch (const std::runtime_error &) {
      return true;
    }
  });
  const auto completed = attempt.wait_for(std::chrono::seconds(2));
  // Release even if the bound assertion fails, so an accidentally unbounded
  // retry loop can finish and the test can report its failure without hanging.
  reader.release();
  EXPECT_EQ(completed, std::future_status::ready);
  EXPECT_TRUE(attempt.get())
      << "persistent reader lock unexpectedly allowed save";
  EXPECT_EQ(loadDdgiPreset(path), original);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),
                          std::filesystem::directory_iterator()),
            1);
}
#endif

TEST(DdgiPresetTests,
     SceneIdentityNormalizesPathsAndSeparatesScaleAndProviders) {
  PresetDirectory directory;
  const auto primary = directory.path / "model.gltf";
  const auto auxiliary = directory.path / "site.ifc";
  writeText(primary, "fixture");
  const auto identity = ddgiSceneIdentity(primary);
  EXPECT_EQ(identity, ddgiSceneIdentity(directory.path / "." / "model.gltf"));
  {
    // The fixture can be on C: while CTest runs on F:. There is no relative
    // path across different Windows drive roots.
    const CurrentDirectory sameDrive(directory.path);
    EXPECT_EQ(identity, ddgiSceneIdentity(std::filesystem::relative(primary)));
  }
  EXPECT_EQ(identity, ddgiSceneIdentity(primary, {}, 1.f, 100.f));
  EXPECT_NE(identity, ddgiSceneIdentity(primary, {}, .001f));
  EXPECT_NE(identity, ddgiSceneIdentity(primary, auxiliary));
  EXPECT_NE(ddgiSceneIdentity({}, auxiliary, 1.f, 1.f),
            ddgiSceneIdentity({}, auxiliary, 1.f, .001f));
  EXPECT_NE(ddgiSceneIdentity(primary, auxiliary),
            ddgiSceneIdentity(auxiliary, primary));
  EXPECT_THROW((void)ddgiSceneIdentity({}, {}), std::invalid_argument);
  EXPECT_THROW((void)ddgiSceneIdentity(primary, {}, 0.f),
               std::invalid_argument);
  EXPECT_THROW((void)ddgiSceneIdentity(primary, {},
                                       std::numeric_limits<float>::infinity()),
               std::invalid_argument);
}

TEST(DdgiPresetTests, ScenePresetFilenamesStayInsideTheSelectedDirectory) {
  PresetDirectory directory;
  const auto path =
      ddgiScenePresetPath("../unsafe\\name/../../scene", directory.path);
  EXPECT_EQ(path.parent_path(), directory.path);
  EXPECT_EQ(path.extension(), ".json");
  EXPECT_EQ(path.filename().string().size(), 27u);
  EXPECT_EQ(path,
            ddgiScenePresetPath("../unsafe\\name/../../scene", directory.path));
  EXPECT_NE(path, ddgiScenePresetPath("another scene", directory.path));
  EXPECT_THROW((void)ddgiScenePresetPath(""), std::invalid_argument);
  EXPECT_THROW((void)ddgiScenePresetPath("scene", {}), std::invalid_argument);
}

TEST(DdgiPresetTests, UnicodeModelIdentityAndPresetPathRoundTrip) {
  PresetDirectory directory;
  const auto model =
      directory.path / std::filesystem::path(u8"bâtiment-模型.gltf");
  const auto path =
      directory.path / std::filesystem::path(u8"réglages-模型.json");
  writeText(model, "fixture");
  const DdgiPreset preset{ddgiSceneIdentity(model), variedPreset().settings};
  saveDdgiPreset(path, preset);
  EXPECT_EQ(loadDdgiPreset(path), preset);
  EXPECT_EQ(decodeDdgiPreset(encodeDdgiPreset(preset)), preset);
}

TEST(DdgiPresetTests,
     ExecutableRelativeAssetsKeepIdentityAcrossWorkingDirectories) {
  PresetDirectory assets(container::util::executableDirectory());
  PresetDirectory working;
  const auto primary = assets.path / "model.gltf";
  const auto auxiliary = assets.path / "site.ifc";
  writeText(primary, "fixture");
  writeText(auxiliary, "fixture");
  const auto expected = ddgiSceneIdentity(primary, auxiliary, .001f, 100.f);
  const CurrentDirectory changed(working.path);
  EXPECT_EQ(ddgiSceneIdentity(assets.path.filename() / "model.gltf",
                              assets.path.filename() / "site.ifc", .001f,
                              100.f),
            expected);
}

TEST(DdgiPresetTests,
     DefaultStorageIsAbsoluteAndIndependentOfWorkingDirectory) {
  const auto directory = ddgiDefaultPresetDirectory();
  const auto preset = ddgiScenePresetPath("stable scene");
  EXPECT_TRUE(directory.is_absolute());
  EXPECT_EQ(preset.parent_path(), directory);
  EXPECT_EQ(directory.filename(), "ddgi");
  PresetDirectory working;
  const CurrentDirectory changed(working.path);
  EXPECT_EQ(ddgiDefaultPresetDirectory(), directory);
  EXPECT_EQ(ddgiScenePresetPath("stable scene"), preset);
}

} // namespace
