#include <Container/geometry/GltfCapabilities.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfImageSources.h>
#include <Container/utility/TextureImageData.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
namespace gltf = container::geometry::gltf;
using gltf::CapabilitySeverity;
using gltf::ExtensionSupport;

TEST(GltfCapabilities, ActiveConsumerRestrictionsCannotPromoteUnsupportedExtensions) {
  gltf::AssetRequirements required{"2.0",{}, {"KHR_materials_unlit"},{"KHR_materials_unlit"}};
  EXPECT_TRUE(gltf::validateCapabilities(required,"source.gltf").supported());
  const std::array<std::string,1> unavailable{"KHR_materials_unlit"};
  const auto blocked=gltf::validateCapabilities(required,"source.gltf",unavailable);
  ASSERT_FALSE(blocked.supported());ASSERT_EQ(blocked.diagnostics.size(),1u);
  EXPECT_EQ(blocked.diagnostics[0].code,"unsupported_required_extension");
  required.extensionsRequired.clear();
  const auto fallback=gltf::validateCapabilities(required,"source.gltf",unavailable);
  ASSERT_TRUE(fallback.supported());ASSERT_EQ(fallback.diagnostics.size(),1u);
  EXPECT_EQ(fallback.diagnostics[0].severity,CapabilitySeverity::Warning);
  required.extensionsRequired={"VENDOR_unknown"};required.extensionsUsed=required.extensionsRequired;
  EXPECT_FALSE(gltf::validateCapabilities(required,"source.gltf",unavailable).supported());
}

class AssetFixture {
 public:
  AssetFixture() {
    static std::atomic<unsigned> sequence{};
    directory = std::filesystem::temp_directory_path() /
        ("container_gltf_capabilities_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(sequence++));
    std::filesystem::create_directories(directory);
  }
  ~AssetFixture() {
    std::error_code ignored;
    for (const char* name : {"asset.gltf", "asset.glb", "triangle.bin"})
      std::filesystem::remove(directory / name, ignored);
    std::filesystem::remove(directory, ignored);
  }
  std::filesystem::path save(const nlohmann::json& json, bool binary = false) {
    const auto path = directory / (binary ? "asset.glb" : "asset.gltf");
    std::ofstream file(path, std::ios::binary);
    std::string text = json.dump();
    if (binary) {
      while (text.size() % 4) text += ' ';
      auto word = [&](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
          file.put(static_cast<char>((value >> shift) & 0xff));
      };
      word(0x46546c67);
      word(2);
      word(static_cast<uint32_t>(20 + text.size()));
      word(static_cast<uint32_t>(text.size()));
      word(0x4e4f534a);
    }
    file << text;
    return path;
  }
  nlohmann::json triangle() {
    const std::array<float, 9> vertices{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::ofstream file(directory / "triangle.bin", std::ios::binary);
    file.write(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices));
    return nlohmann::json::parse(R"({
      "asset":{"version":"2.0"},
      "buffers":[{"uri":"triangle.bin","byteLength":36}],
      "bufferViews":[{"buffer":0,"byteLength":36}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
                    "min":[0,0,0],"max":[1,1,0]}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],
      "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
    })");
  }
  std::filesystem::path directory;
};

constexpr std::string_view kEmbeddedRedPng =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
    "iZk9HQAAAABJRU5ErkJggg==";

struct AlternateImageFixture {
  const char* extension;
  const char* mimeType;
  const char* data;
};

constexpr AlternateImageFixture kAlternateImages[]{
    // The unavailable KTX2 decoder must never interpret this opaque header
    // payload. The WebP and AVIF payloads are complete one-pixel red images.
    {"KHR_texture_basisu", "image/ktx2", "q0tUWCAyMLsNChoK"},
    {"EXT_texture_webp", "image/webp", "UklGRhwAAABXRUJQVlA4TA8AAAAvAAAAAAcQ/Y/+ByKi/wEA"},
    {"EXT_texture_avif", "image/avif",
     "AAAAIGZ0eXBhdmlmAAAAAGF2aWZtaWYxbWlhZk1BMUIAAADrbWV0YQAAAAAAAAAhaGRscgAA"
     "AAAAAAAAcGljdAAAAAAAAAAAAAAAAAAAAAAOcGl0bQAAAAAAAQAAAB5pbG9jAAAAAEQAAAEAAQAA"
     "AAEAAAETAAAAHgAAAChpaW5mAAAAAAABAAAAGmluZmUCAAAAAAEAAGF2MDFDb2xvcgAAAABq"
     "aXBycAAAAEtpcGNvAAAAFGlzcGUAAAAAAAAAAQAAAAEAAAAQcGl4aQAAAAADCAgIAAAADGF2"
     "MUOBAAwAAAAAE2NvbHJuY2x4AAEADQAGgAAAABdpcG1hAAAAAAAAAAEAAQQBAoMEAAAAJm1k"
     "YXQSAAoFGAAGBCAyExQAAwwwxAAAeUzU33xM1O1T0oA="},
};

nlohmann::json alternateTextureAsset(AssetFixture& fixture,
                                    const AlternateImageFixture& alternate) {
  auto json = fixture.triangle();
  json["extensionsUsed"] = {alternate.extension};
  json["images"] = {
      {{"uri", kEmbeddedRedPng}},
      {{"uri", "data:application/octet-stream;base64," + std::string(alternate.data)},
       {"mimeType", alternate.mimeType}}};
  json["textures"] = {{{"source", 0},
                        {"extensions", {{alternate.extension, {{"source", 1}}}}}}};
  json["materials"] = {{{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}}};
  json["meshes"][0]["primitives"][0]["material"] = 0;
  return json;
}

template <typename Load>
void expectLoadError(Load load, const std::filesystem::path& path,
                     std::string_view detail) {
  try {
    load();
    FAIL() << "Expected a capability rejection";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(path.string()), std::string::npos) << message;
    EXPECT_NE(message.find(detail), std::string::npos) << message;
  }
}

}  // namespace

TEST(GltfCapabilities, AcceptsCurrentVersionWithoutWarnings) {
  const auto validation = gltf::validateCapabilities({"2.0"}, "current.gltf");
  EXPECT_TRUE(validation.supported());
  EXPECT_TRUE(validation.diagnostics.empty());
}

TEST(GltfCapabilities, RejectsUnsupportedMajorVersions) {
  for (const char* version : {"0.0", "1.0", "3.0"}) {
    const auto validation = gltf::validateCapabilities({version}, "old.gltf");
    EXPECT_FALSE(validation.supported()) << version;
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "unsupported_version");
  }
}

TEST(GltfCapabilities, RejectsMalformedVersionsWithoutThrowing) {
  for (const char* version : {"", "2", ".0", "2.", "2.0.0", "v2.0", " 2.0",
                               "2.0 ", "+2.0", "2.-1", "2.0preview", "4294967296.0"}) {
    const auto validation = gltf::validateCapabilities({version}, "invalid.gltf");
    EXPECT_FALSE(validation.supported()) << version;
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "invalid_version");
  }
}

TEST(GltfCapabilities, WarnsForCompatibleForwardMinorWithOrWithoutMinimum) {
  for (const auto minimum : {std::optional<std::string>{},
                             std::optional<std::string>{"2.0"}}) {
    const auto validation = gltf::validateCapabilities({"2.7", minimum}, "future.gltf");
    EXPECT_TRUE(validation.supported());
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "newer_minor_version");
    EXPECT_EQ(validation.diagnostics[0].severity, CapabilitySeverity::Warning);
  }
  EXPECT_TRUE(gltf::validateCapabilities({"2.0", "2.0"}, "current.gltf").supported());
}

TEST(GltfCapabilities, RejectsUnsupportedAndMalformedMinimumVersions) {
  for (const char* minimum : {"1.0", "2.1", "3.0"}) {
    const auto validation = gltf::validateCapabilities({"2.7", minimum}, "minimum.gltf");
    EXPECT_FALSE(validation.supported()) << minimum;
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "unsupported_min_version");
  }
  for (const char* minimum : {"", "2", "2.0.0", "2.-1"}) {
    const auto validation = gltf::validateCapabilities({"2.0", minimum}, "minimum.gltf");
    EXPECT_FALSE(validation.supported()) << minimum;
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "invalid_min_version");
  }
  EXPECT_FALSE(gltf::validateCapabilities({"2.0", "2.1"}, "minimum.gltf").supported());
}

TEST(GltfCapabilities, RejectsUnknownRequiredEvenWhenOmittedFromUsed) {
  const auto validation = gltf::validateCapabilities(
      {"2.0", {}, {}, {"VENDOR_future", "VENDOR_future"}}, "required.gltf");
  EXPECT_FALSE(validation.supported());
  ASSERT_EQ(validation.diagnostics.size(), 1u);
  EXPECT_EQ(validation.diagnostics[0].code, "unsupported_required_extension");
  EXPECT_EQ(validation.diagnostics[0].assetPath, "required.gltf");
  EXPECT_NE(gltf::formatCapabilityDiagnostic(validation.diagnostics[0]).find("VENDOR_future"),
            std::string::npos);
}

TEST(GltfCapabilities, WarnsOnceForUnknownOptionalAndGivesRequiredPrecedence) {
  const auto optional = gltf::validateCapabilities(
      {"2.0", {}, {"VENDOR_future", "VENDOR_future"}, {}}, "optional.gltf");
  EXPECT_TRUE(optional.supported());
  ASSERT_EQ(optional.diagnostics.size(), 1u);
  EXPECT_EQ(optional.diagnostics[0].code, "unsupported_optional_extension");
  EXPECT_EQ(optional.diagnostics[0].severity, CapabilitySeverity::Warning);
  const auto required = gltf::validateCapabilities(
      {"2.0", {}, {"VENDOR_future"}, {"VENDOR_future"}}, "required.gltf");
  EXPECT_FALSE(required.supported());
  ASSERT_EQ(required.diagnostics.size(), 1u);
  EXPECT_EQ(required.diagnostics[0].severity, CapabilitySeverity::Error);
}

TEST(GltfCapabilities, RegistryMatchesImplementedMaterialsLightsAndUvTransforms) {
  gltf::AssetRequirements requirements{"2.0"};
  requirements.extensionsRequired = {
      "KHR_materials_unlit", "KHR_materials_specular", "KHR_materials_ior",
      "KHR_materials_dispersion", "KHR_materials_emissive_strength",
      "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_clearcoat",
      "KHR_materials_sheen", "KHR_texture_transform",
      "KHR_lights_punctual", "KHR_node_visibility"};
  const auto validation = gltf::validateCapabilities(requirements, "materials.gltf");
  EXPECT_TRUE(validation.supported());
  EXPECT_TRUE(validation.diagnostics.empty());
  for (const auto& name : requirements.extensionsRequired) {
    const auto* capability = gltf::extensionCapability(name);
    ASSERT_NE(capability, nullptr) << name;
    EXPECT_EQ(capability->support, ExtensionSupport::Supported) << name;
  }
}

TEST(GltfCapabilities, AcceptsImplementedApproximationsWithExplicitWarning) {
  for (const char* extension : {"KHR_materials_pbrSpecularGlossiness", "KHR_materials_iridescence", "KHR_lights_area",
                                "EXT_lights_area", "WEBGI_lights_area", "KHR_animation_pointer",
                                "KHR_mesh_quantization", "KHR_accessor_float64", "MSFT_texture_dds", "EXT_mesh_gpu_instancing"}) {
    const auto validation = gltf::validateCapabilities(
        {"2.0", {}, {extension}, {extension}}, "approximate.gltf");
    EXPECT_TRUE(validation.supported());
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "partial_extension_support");
    if (std::string_view(extension) == "KHR_materials_iridescence") {
      EXPECT_EQ(gltf::extensionCapability(extension)->support, ExtensionSupport::Partial);
      EXPECT_NE(validation.diagnostics[0].message.find("cosine-tint approximation"),
                std::string::npos);
    }
  }
}

TEST(GltfCapabilities, RejectsUnavailableDecodersAndUnimplementedRuntimeFeatures) {
  for (const char* extension : {"KHR_draco_mesh_compression", "EXT_meshopt_compression",
                                "KHR_meshopt_compression", "KHR_texture_basisu",
                                "EXT_texture_webp", "EXT_texture_avif"}) {
    const auto* capability = gltf::extensionCapability(extension);
    ASSERT_NE(capability, nullptr) << extension;
    if ((std::string_view(extension) == "KHR_draco_mesh_compression" && gltf::HasDracoDecoder()) ||
        (std::string_view(extension).find("meshopt_compression") != std::string_view::npos && gltf::HasMeshoptDecoder()))
      continue;
    if ((std::string_view(extension) == "KHR_texture_basisu" && container::material::HasKtxTextureCodec()) ||
        (std::string_view(extension) == "EXT_texture_webp" && container::material::HasWebpTextureCodec())) continue;
    EXPECT_EQ(capability->support, ExtensionSupport::Unsupported);
    EXPECT_FALSE(gltf::validateCapabilities(
        {"2.0", {}, {extension}, {extension}}, "unsupported.glb").supported());
    const auto optional = gltf::validateCapabilities(
        {"2.0", {}, {extension}, {}}, "fallback.gltf");
    EXPECT_TRUE(optional.supported());
    ASSERT_EQ(optional.diagnostics.size(), 1u);
    EXPECT_EQ(optional.diagnostics[0].code, "unsupported_optional_extension");
  }
  EXPECT_EQ(gltf::extensionCapability("VENDOR_unknown"), nullptr);
}

TEST(GltfCapabilities, CompressionRegistryReflectsConfiguredBackends) {
  for (const char* extension : {"KHR_draco_mesh_compression", "EXT_meshopt_compression",
                                 "KHR_meshopt_compression"}) {
    const bool available = std::string_view(extension) == "KHR_draco_mesh_compression"
        ? gltf::HasDracoDecoder() : gltf::HasMeshoptDecoder();
    const auto* capability = gltf::extensionCapability(extension);
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->support != ExtensionSupport::Unsupported, available);
    EXPECT_EQ(gltf::validateCapabilities(
        {"2.0", {}, {extension}, {extension}}, "compression.gltf").supported(), available);
  }
}

TEST(GltfCapabilitiesLoader, EveryGeometryEntryPointRejectsBeforeResourceIo) {
  AssetFixture fixture;
  auto json = fixture.triangle();
  json["buffers"][0]["uri"] = "missing.bin";
  json["extensionsRequired"] = {"VENDOR_unavailable"};
  const auto path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string()); }, path,
                  "unsupported_required_extension");
  expectLoadError([&] { (void)gltf::LoadModelFromFile(path.string()); }, path,
                  "VENDOR_unavailable");
  expectLoadError([&] { (void)container::geometry::Model::LoadFromGltf(path.string()); }, path,
                  "VENDOR_unavailable");
  expectLoadError([&] { (void)gltf::LoadSourceModel(path.string()); }, path,
                  "VENDOR_unavailable");
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string(), {false, 4096}); }, path,
                  "VENDOR_unavailable");
}

TEST(GltfCapabilitiesLoader, BinarySourceUsesTheSameGateAndAllowsMaterialOnlyAssets) {
  AssetFixture fixture;
  auto json = nlohmann::json::parse(R"({"asset":{"version":"2.0"},"materials":[{}]})");
  auto path = fixture.save(json, true);
  EXPECT_EQ(gltf::LoadSourceModel(path.string()).materials.size(), 1u);
  json["extensionsRequired"] = {"VENDOR_future"};
  path = fixture.save(json, true);
  expectLoadError([&] { (void)gltf::LoadSourceModel(path.string()); }, path,
                  "VENDOR_future");
}

TEST(GltfCapabilitiesLoader, ReturnsAssetScopedWarningsWhileImportingFallbackGeometry) {
  AssetFixture fixture;
  auto json = fixture.triangle();
  json["extensionsUsed"] = {"VENDOR_optional", "VENDOR_optional"};
  const auto path = fixture.save(json);
  const auto loaded = gltf::LoadModelWithSource(path.string());
  EXPECT_EQ(loaded.model.vertices().size(), 3u);
  ASSERT_EQ(loaded.capabilityDiagnostics.size(), 1u);
  EXPECT_EQ(loaded.capabilityDiagnostics[0].assetPath, path.string());
  EXPECT_EQ(loaded.capabilityDiagnostics[0].code, "unsupported_optional_extension");
  EXPECT_EQ(loaded.capabilityDiagnostics[0].severity, CapabilitySeverity::Warning);
}

TEST(GltfCapabilitiesLoader, ConsumerRejectsRequiredInstancingBeforeResourceIo) {
  AssetFixture fixture;
  auto json = fixture.triangle();
  constexpr auto extension = "EXT_mesh_gpu_instancing";
  json["nodes"][0]["extensions"][extension]["attributes"]["TRANSLATION"] = 0;
  json["extensionsUsed"] = {extension};
  json["extensionsRequired"] = {extension};

  const auto path = fixture.save(json);
  const auto editable = gltf::LoadModelWithSource(path.string());
  EXPECT_EQ(editable.instancing.nodes.size(), 1u);

  gltf::LoadOptions staticComposition;
  staticComposition.unavailableRuntimeExtensions = {extension};
  auto missingBuffer = json;
  missingBuffer["buffers"][0]["uri"] = "missing.bin";
  fixture.save(missingBuffer);
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string(), staticComposition); },
                  path, "Required extension 'EXT_mesh_gpu_instancing' is unavailable in the active scene consumer");

  json.erase("extensionsRequired");
  fixture.save(json);
  const auto optional = gltf::LoadModelWithSource(path.string(), staticComposition);
  EXPECT_EQ(optional.model.vertices().size(), 3u);
  ASSERT_EQ(optional.capabilityDiagnostics.size(), 1u);
  EXPECT_EQ(optional.capabilityDiagnostics[0].assetPath, path.string());
  EXPECT_EQ(optional.capabilityDiagnostics[0].code, "unsupported_optional_extension");
  EXPECT_NE(optional.capabilityDiagnostics[0].message.find(extension), std::string::npos);
}

TEST(GltfCapabilitiesLoader, OptionalAlternateImagesUseDecodedPngFallbacks) {
  for (const auto& alternate : kAlternateImages) {
    if (gltf::extensionCapability(alternate.extension)->support != ExtensionSupport::Unsupported) continue;
    SCOPED_TRACE(alternate.extension);
    for (bool binary : {false, true}) {
      AssetFixture fixture;
      const auto path = fixture.save(alternateTextureAsset(fixture, alternate), binary);
      // Default image decoding must succeed, not just the geometry-only
      // decodeImages=false path. Exercise finite input preflight as well.
      const auto loaded = gltf::LoadModelWithSource(path.string());
      const auto bounded = gltf::LoadModelWithSource(path.string(), {true, 65536});
      for (const auto* result : {&loaded, &bounded}) {
        EXPECT_EQ(result->model.vertices().size(), 3u);
        ASSERT_EQ(result->gltfModel.images.size(), 2u);
        const auto& png = result->gltfModel.images[0];
        EXPECT_EQ(png.width, 1);
        EXPECT_EQ(png.height, 1);
        EXPECT_EQ(png.component, 4);
        EXPECT_EQ(png.image, (std::vector<unsigned char>{255, 0, 0, 255}));
        EXPECT_TRUE(result->gltfModel.images[1].image.empty());
        ASSERT_EQ(result->gltfModel.textures.size(), 1u);
        EXPECT_EQ(result->gltfModel.textures[0].source, 0);
        ASSERT_EQ(result->capabilityDiagnostics.size(), 1u);
        EXPECT_EQ(result->capabilityDiagnostics[0].code, "unsupported_optional_extension");
        EXPECT_NE(result->capabilityDiagnostics[0].message.find(alternate.extension),
                  std::string::npos);
      }
    }
  }
}

TEST(GltfCapabilitiesLoader, RequiredAlternateImageExtensionsRejectBeforeResourceReads) {
  for (const auto& alternate : kAlternateImages) {
    if (gltf::extensionCapability(alternate.extension)->support != ExtensionSupport::Unsupported) continue;
    AssetFixture fixture;
    auto json = alternateTextureAsset(fixture, alternate);
    json["extensionsRequired"] = {alternate.extension};
    json["buffers"][0]["uri"] = "missing.bin";
    const auto path = fixture.save(json);
    expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string()); }, path,
                    "unsupported_required_extension");
    expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string(), {true, 65536}); }, path,
                    alternate.extension);
  }
}

TEST(GltfCapabilitiesLoader, AlternateReferencesNeverSuppressOtherCoreImageDecoding) {
  for (const auto& alternate : kAlternateImages) {
    AssetFixture fixture;
    auto json = alternateTextureAsset(fixture, alternate);
    json["textures"].push_back({{"source", 1}});
    const auto path = fixture.save(json);
    if (std::string_view(alternate.extension) == "EXT_texture_avif") {
      expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string()); }, path, "STB cannot decode");
    } else {
      // Typed formats retain exact source bytes for role/device-aware decoding.
      // They must remain resident even when a core reference shares the source.
      const auto loaded = gltf::LoadModelWithSource(path.string());
      ASSERT_EQ(loaded.gltfModel.images.size(), 2u);
      EXPECT_TRUE(loaded.gltfModel.images[1].as_is);
      EXPECT_FALSE(loaded.gltfModel.images[1].image.empty());
    }
  }
}

TEST(GltfCapabilitiesLoader, SharedPngUsesCoreFallbackOrSelectedBasisValidation) {
  AssetFixture fixture;
  auto json = alternateTextureAsset(fixture, kAlternateImages[0]);
  json["images"].erase(1);
  json["textures"][0]["extensions"]["KHR_texture_basisu"]["source"] = 0;
  const auto path = fixture.save(json);
  if (container::material::HasKtxTextureCodec()) {
    const auto loaded = gltf::LoadModelWithSource(path.string());
    ASSERT_TRUE(loaded.gltfModel.images[0].as_is);
    EXPECT_THROW((void)gltf::SelectTextureImageSource(loaded.gltfModel, 0), std::runtime_error);
    return;
  }
  const auto loaded = gltf::LoadModelWithSource(path.string());
  ASSERT_EQ(loaded.gltfModel.images.size(), 1u);
  EXPECT_EQ(loaded.gltfModel.images[0].image,
            (std::vector<unsigned char>{255, 0, 0, 255}));
}

TEST(GltfCapabilitiesLoader, MalformedCoreIndicesCannotAliasSkippedAlternateImages) {
  for (const auto &index : {nlohmann::json(uint64_t{4294967297}),
                            nlohmann::json(int64_t{-4294967295}),
                            nlohmann::json(2), nlohmann::json(-1),
                            nlohmann::json(0.5)}) {
    AssetFixture fixture;
    auto json = alternateTextureAsset(fixture, kAlternateImages[0]);
    json["textures"][0]["source"] = index;
    const auto path = fixture.save(json);
    expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string()); }, path,
                    "texture.source");
    expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string(), {true, 65536}); }, path,
                    "texture.source");
  }
}

TEST(GltfCapabilitiesLoader, UnreferencedOrdinaryImagesKeepDefaultDecodingBehavior) {
  AssetFixture fixture;
  auto json = alternateTextureAsset(fixture, kAlternateImages[0]);
  json["images"].push_back({{"uri", "data:application/octet-stream;base64,AQIDBA=="}});
  const auto path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string()); }, path,
                  "STB cannot decode");
  // The explicitly requested geometry-only mode continues to skip all image
  // decoding, not only the alternate extension image.
  const auto geometryOnly = gltf::LoadModelWithSource(path.string(), {false, 65536});
  EXPECT_EQ(geometryOnly.model.vertices().size(), 3u);
  for (const auto& image : geometryOnly.gltfModel.images) EXPECT_TRUE(image.image.empty());
}

TEST(GltfCapabilitiesLoader, EnforcesOwnVersionAndMinimumPolicyForJsonFiles) {
  AssetFixture fixture;
  auto json = fixture.triangle();
  json["asset"]["version"] = "2.1";
  json["asset"]["minVersion"] = "2.0";
  auto path = fixture.save(json);
  const auto loaded = gltf::LoadModelWithSource(path.string());
  ASSERT_EQ(loaded.capabilityDiagnostics.size(), 1u);
  EXPECT_EQ(loaded.capabilityDiagnostics[0].code, "newer_minor_version");
  json["asset"]["minVersion"] = "2.1";
  path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string()); }, path,
                  "unsupported_min_version");
  json["asset"]["minVersion"] = "broken";
  path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadSourceModel(path.string()); }, path,
                  "invalid_min_version");
  json["asset"] = {{"version", "3.0"}};
  path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelFromFile(path.string()); }, path,
                  "unsupported_version");
  json["asset"] = {{"version", "2.0.0"}};
  path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelFromFile(path.string()); }, path,
                  "invalid_version");
}

TEST(GltfCapabilitiesLoader, PreservesEncodedAndDeclaredGeometryBudgets) {
  AssetFixture fixture;
  auto json = fixture.triangle();
  auto path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string(), {false, 1}); }, path,
                  "input budget");
  json["buffers"][0]["byteLength"] = 100000;
  path = fixture.save(json);
  expectLoadError([&] { (void)gltf::LoadModelWithSource(path.string(), {false, 4096}); }, path,
                  "input-memory budget");
}

TEST(GltfCapabilities, TextureRegistryReflectsConfiguredBackendsAndVariants) {
  for (const auto& [extension, available] : std::array{
      std::pair{"KHR_texture_basisu", container::material::HasKtxTextureCodec()},
      std::pair{"EXT_texture_webp", container::material::HasWebpTextureCodec()}}) {
    const auto* capability = gltf::extensionCapability(extension);
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->support != ExtensionSupport::Unsupported, available);
    EXPECT_EQ(gltf::validateCapabilities({"2.0", {}, {extension}, {extension}}, "textures.gltf").supported(), available);
  }
  const auto variants = gltf::validateCapabilities({"2.0", {}, {"KHR_materials_variants"}, {"KHR_materials_variants"}}, "variants.gltf");
  EXPECT_TRUE(variants.supported());
  EXPECT_TRUE(variants.diagnostics.empty());
}

TEST(GltfCapabilities, NodeFlagRegistryDistinguishesVisibilityFromInteractionApproximation) {
  const auto* visibility = gltf::extensionCapability("KHR_node_visibility");
  ASSERT_NE(visibility, nullptr);
  EXPECT_EQ(visibility->support, ExtensionSupport::Supported);
  EXPECT_TRUE(gltf::validateCapabilities({"2.0", {}, {"KHR_node_visibility"},
      {"KHR_node_visibility"}}, "visible.gltf").diagnostics.empty());
  for (const char* extension : {"KHR_node_selectability", "KHR_node_hoverability"}) {
    const auto* capability = gltf::extensionCapability(extension);
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->support, ExtensionSupport::Partial);
    const auto validation = gltf::validateCapabilities(
        {"2.0", {}, {extension}, {extension}}, "interaction.gltf");
    EXPECT_TRUE(validation.supported());
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "partial_extension_support");
    EXPECT_NE(validation.diagnostics[0].message.find("ancestor routing"), std::string::npos);
    EXPECT_NE(validation.diagnostics[0].message.find("trace-through"), std::string::npos);
    EXPECT_NE(validation.diagnostics[0].message.find("behavior-graph"), std::string::npos);
  }
}

TEST(GltfCapabilities, ExtendedPbrRequiredSupportNamesItsRenderingApproximations) {
  for (const char* extension : {"KHR_materials_anisotropy",
                                 "KHR_materials_diffuse_transmission"}) {
    const auto* capability = gltf::extensionCapability(extension);
    ASSERT_NE(capability, nullptr);
    EXPECT_EQ(capability->support, ExtensionSupport::Partial);
    const auto validation = gltf::validateCapabilities(
        {"2.0", {}, {extension}, {extension}}, "extended-material.gltf");
    EXPECT_TRUE(validation.supported());
    ASSERT_EQ(validation.diagnostics.size(), 1u);
    EXPECT_EQ(validation.diagnostics[0].code, "partial_extension_support");
    EXPECT_EQ(validation.diagnostics[0].severity, CapabilitySeverity::Warning);
    EXPECT_NE(validation.diagnostics[0].message.find(extension), std::string::npos);
    EXPECT_NE(capability->description.find(
        std::string_view(extension) == "KHR_materials_anisotropy"
            ? "bent-normal" : "Release-candidate"), std::string_view::npos);
  }
}

TEST(GltfCapabilitiesLoader, RequiredExtendedPbrUsesTheSameJsonAndBinaryMetadataGate) {
  for (bool binary : {false, true}) {
    AssetFixture fixture;
    auto json = fixture.triangle();
    json["extensionsUsed"] = json["extensionsRequired"] = {
        "KHR_materials_anisotropy", "KHR_materials_diffuse_transmission"};
    json["materials"] = nlohmann::json::array({{
        {"extensions", {
            {"KHR_materials_anisotropy", {{"anisotropyStrength", 0.0}, {"anisotropyRotation", 0.65}}},
            {"KHR_materials_diffuse_transmission", {{"diffuseTransmissionFactor", 0.7},
                {"diffuseTransmissionColorFactor", {0.0, 0.2, 0.8}}}}
        }}
    }});
    json["meshes"][0]["primitives"][0]["material"] = 0;
    auto path = fixture.save(json, binary);
    const auto loaded = gltf::LoadModelWithSource(path.string());
    EXPECT_EQ(loaded.model.vertices().size(), 3u);
    ASSERT_EQ(loaded.capabilityDiagnostics.size(), 2u);
    for (const auto& diagnostic : loaded.capabilityDiagnostics)
      EXPECT_EQ(diagnostic.code, "partial_extension_support");
    const auto& extensions = loaded.gltfModel.materials.at(0).extensions;
    EXPECT_DOUBLE_EQ(extensions.at("KHR_materials_anisotropy").Get("anisotropyStrength").GetNumberAsDouble(), 0.0);
    EXPECT_DOUBLE_EQ(extensions.at("KHR_materials_diffuse_transmission").Get("diffuseTransmissionFactor").GetNumberAsDouble(), 0.7);
    json["buffers"][0]["uri"] = "missing-extended-material.bin";
    json["materials"][0]["extensions"]["KHR_materials_diffuse_transmission"]["diffuseTransmissionFactor"] = 2;
    path = fixture.save(json, binary);
    expectLoadError([&] { (void)gltf::LoadSourceModel(path.string()); }, path,
                    "diffuseTransmissionFactor");
  }
}

TEST(GltfCapabilitiesLoader, RequiredNodeFlagsUseTheSameJsonAndBinaryMetadataGate) {
  const std::array extensions{
      "KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability"};
  const std::array properties{"visible", "selectable", "hoverable"};
  for (bool binary : {false, true}) {
    AssetFixture fixture;
    auto json = fixture.triangle();
    json["extensionsUsed"] = json["extensionsRequired"] = extensions;
    for (size_t i = 0; i < extensions.size(); ++i)
      json["nodes"][0]["extensions"][extensions[i]][properties[i]] = false;
    auto path = fixture.save(json, binary);
    const auto loaded = gltf::LoadModelWithSource(path.string());
    EXPECT_EQ(loaded.model.vertices().size(), 3u);
    ASSERT_EQ(loaded.capabilityDiagnostics.size(), 2u);
    for (const auto& diagnostic : loaded.capabilityDiagnostics)
      EXPECT_EQ(diagnostic.code, "partial_extension_support");
    for (size_t i = 0; i < extensions.size(); ++i)
      EXPECT_FALSE(loaded.gltfModel.nodes[0].extensions.at(extensions[i]).Get(properties[i]).Get<bool>());

    json["buffers"][0]["uri"] = "missing-node-flags.bin";
    for (size_t i = 0; i < extensions.size(); ++i) {
      auto malformed = json;
      malformed["nodes"][0]["extensions"][extensions[i]][properties[i]] = 0;
      path = fixture.save(malformed, binary);
      expectLoadError([&] { (void)gltf::LoadSourceModel(path.string()); }, path,
          "glTF node flags node[0].extensions." + std::string(extensions[i]) + "." + properties[i]);
    }
    auto undeclared = json;
    undeclared.erase("extensionsRequired");
    undeclared.erase("extensionsUsed");
    path = fixture.save(undeclared, binary);
    expectLoadError([&] { (void)gltf::LoadSourceModel(path.string()); }, path,
                    "extension must be declared in extensionsUsed");
  }
}
