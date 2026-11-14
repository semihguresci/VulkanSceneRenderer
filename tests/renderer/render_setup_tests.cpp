#include "Container/renderer/scene/RenderSetup.h"

#include "Container/utility/AtomicFile.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/ModelImportCancellation.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {

class RenderSetupTest : public testing::Test {
protected:
  void SetUp() override {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    directory = std::filesystem::temp_directory_path() / ("container-render-setup-" + suffix);
    std::filesystem::create_directory(directory);
    model = directory / "scene.gltf";
    sidecar = directory / "review.render-setups.json";
    container::util::writeFileAtomically(model, "abc");
  }

  void TearDown() override {
    std::error_code error;
    for (auto file : {sidecar, model, directory / "copy.gltf", directory / "light.HDR"}) {
      std::filesystem::remove(file, error);
    }
    std::filesystem::remove(directory, error);
  }

  RenderSetupDocument Document() const {
    RenderSetupEntry first{.name = "Morning", .cameraSourceNode = 4, .materialVariant = 1,
                           .visibility = {{12, false}, {7, true}}, .hdrPath = directory / "light.HDR"};
    first.grade.toneMapper = ToneMapper::AgX;
    first.grade.contrast = 1.2f;
    first.grade.saturation = 0.0f;
    first.grade.shadowTone = {1.0f, 0.5f, 0.0f};
    first.grade.highlightTone = {0.0f, 0.5f, 1.0f};
    return {FingerprintRenderSetupSource(model, 0), {std::move(first), {.name = "Base materials"}}};
  }

  std::string ReadSidecarText() const {
    std::ifstream input(sidecar, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  std::filesystem::path directory;
  std::filesystem::path model;
  std::filesystem::path sidecar;
};

TEST_F(RenderSetupTest, FingerprintsExactPrimaryBytesAndTracksSceneIndex) {
  const auto source = FingerprintRenderSetupSource(model, 0);
  EXPECT_EQ(source.digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(source.byteSize, 3u);
  const std::array<unsigned char, 3> importedBytes{'a', 'b', 'c'};
  const auto imported = container::util::FingerprintContent(importedBytes);
  EXPECT_EQ(source.digest, imported.digest);
  EXPECT_EQ(source.byteSize, imported.byteSize);
  const auto copy = directory / "copy.gltf";
  std::filesystem::copy_file(model, copy);
  EXPECT_EQ(source, FingerprintRenderSetupSource(copy, 0));
  EXPECT_THROW(ValidateRenderSetupSource(source, FingerprintRenderSetupSource(copy, 1)),
               std::invalid_argument);
  container::util::writeFileAtomically(copy, "abd");
  EXPECT_THROW(ValidateRenderSetupSource(source, FingerprintRenderSetupSource(copy, 0)),
               std::invalid_argument);
  container::util::writeFileAtomically(copy, std::string(1'000'000, 'a'));
  EXPECT_EQ(FingerprintRenderSetupSource(copy, 0).digest,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  container::util::writeFileAtomically(copy, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
  EXPECT_EQ(FingerprintRenderSetupSource(copy, 0).digest,
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_F(RenderSetupTest, FingerprintHonorsLoaderCancellation) {
  std::stop_source stop;
  stop.request_stop();
  const std::array<unsigned char, 1> bytes{0};
  EXPECT_THROW((void)container::util::FingerprintContent(bytes, stop.get_token()),
               container::util::ModelImportCancelled);
  EXPECT_THROW((void)container::util::FingerprintFileContent(model, stop.get_token()),
               container::util::ModelImportCancelled);
}

TEST_F(RenderSetupTest, RoundTripsTwoSetupsAndPortableSidecarRelativePath) {
  const auto document = Document();
  SaveRenderSetupDocument(sidecar, document);
  const auto wire = nlohmann::json::parse(ReadSidecarText());
  EXPECT_EQ(wire.at("version"), 1);
  EXPECT_EQ(wire.at("setups")[0].at("hdr"), "light.HDR");
  EXPECT_TRUE(wire.at("setups")[1].at("hdr").is_null());
  const auto restored = LoadRenderSetupDocument(sidecar);
  EXPECT_EQ(restored.source, document.source);
  ASSERT_EQ(restored.setups.size(), 2u);
  EXPECT_EQ(restored.setups[0].name, "Morning");
  EXPECT_EQ(restored.setups[0].cameraSourceNode, 4u);
  EXPECT_EQ(restored.setups[0].materialVariant, 1u);
  EXPECT_EQ(restored.setups[0].visibility, document.setups[0].visibility);
  EXPECT_EQ(restored.setups[0].hdrPath, document.setups[0].hdrPath);
  EXPECT_EQ(restored.setups[0].grade.toneMapper, ToneMapper::AgX);
  EXPECT_FLOAT_EQ(restored.setups[0].grade.contrast, 1.2f);
  EXPECT_FLOAT_EQ(restored.setups[0].grade.saturation, 0.0f);
  EXPECT_EQ(restored.setups[0].grade.shadowTone, document.setups[0].grade.shadowTone);
  EXPECT_EQ(restored.setups[0].grade.highlightTone, document.setups[0].grade.highlightTone);
  EXPECT_FALSE(restored.setups[1].cameraSourceNode);
  EXPECT_FALSE(restored.setups[1].materialVariant);
  EXPECT_TRUE(restored.setups[1].visibility.empty());
  EXPECT_TRUE(restored.setups[1].hdrPath.empty());
}

TEST_F(RenderSetupTest, RejectsStaleReferencesAndMissingResourcesBeforeUse) {
  auto document = Document();
  const std::array active{RenderSetupNode{7, false}, RenderSetupNode{4, true}, RenderSetupNode{12, false}};
  auto validate = [&](std::span<const RenderSetupNode> nodes, size_t variants) {
    ValidateRenderSetupEntry(document.setups[0], document.source, document.source, nodes, variants);
  };
  EXPECT_THROW(validate(active, 2), std::invalid_argument);
  container::util::writeFileAtomically(document.setups[0].hdrPath, "fixture");
  EXPECT_NO_THROW(validate(active, 2));
  EXPECT_THROW(validate(active, 1), std::invalid_argument);
  EXPECT_THROW(validate(std::span(active).first(2), 2), std::invalid_argument);
  const std::array ambiguous{active[0], active[1], active[2], active[2]};
  EXPECT_THROW(validate(ambiguous, 2), std::invalid_argument);
  document.setups[0].cameraSourceNode = 7;
  EXPECT_THROW(validate(active, 2), std::invalid_argument);
}

TEST_F(RenderSetupTest, MalformedDocumentsDoNotReplaceTheLastSafeSave) {
  auto document = Document();
  SaveRenderSetupDocument(sidecar, document);
  const auto original = ReadSidecarText();
  document.setups[1].name = document.setups[0].name;
  EXPECT_THROW(SaveRenderSetupDocument(sidecar, document), std::invalid_argument);
  EXPECT_EQ(ReadSidecarText(), original);
  document = Document();
  document.setups[0].grade.saturation = std::numeric_limits<float>::infinity();
  EXPECT_THROW(SaveRenderSetupDocument(sidecar, document), std::invalid_argument);
  EXPECT_EQ(ReadSidecarText(), original);
  document = Document();
  document.setups[0].visibility.push_back(document.setups[0].visibility.back());
  EXPECT_THROW(SaveRenderSetupDocument(sidecar, document), std::invalid_argument);
  EXPECT_EQ(ReadSidecarText(), original);
}

TEST_F(RenderSetupTest, RejectsDuplicateKeysDeepNestingAndUnsupportedVersions) {
  const auto document = Document();
  SaveRenderSetupDocument(sidecar, document);
  const auto original = ReadSidecarText();
  container::util::writeFileAtomically(sidecar, "{\"version\":1," + original.substr(1));
  EXPECT_THROW((void)LoadRenderSetupDocument(sidecar), std::invalid_argument);
  container::util::writeFileAtomically(sidecar, "[[[[[[[0]]]]]]]");
  EXPECT_THROW((void)LoadRenderSetupDocument(sidecar), std::invalid_argument);
  auto wire = nlohmann::json::parse(original);
  wire["version"] = 2;
  container::util::writeFileAtomically(sidecar, wire.dump());
  EXPECT_THROW((void)LoadRenderSetupDocument(sidecar), std::invalid_argument);
  wire["version"] = 1;
  wire["setups"][0]["visibility"][0]["node"] = -1;
  container::util::writeFileAtomically(sidecar, wire.dump());
  EXPECT_THROW((void)LoadRenderSetupDocument(sidecar), std::invalid_argument);
  wire["setups"][0]["visibility"][0]["node"] = 0;
  wire["setups"][0]["hdr"] = std::string("bad\0path.hdr", 12);
  container::util::writeFileAtomically(sidecar, wire.dump());
  EXPECT_THROW((void)LoadRenderSetupDocument(sidecar), std::invalid_argument);
  SaveRenderSetupDocument(sidecar, document);
  EXPECT_EQ(LoadRenderSetupDocument(sidecar).setups.size(), 2u);
}

} // namespace
} // namespace container::renderer
