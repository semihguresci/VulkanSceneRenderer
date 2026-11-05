#include <Container/geometry/GltfImageSources.h>
#include <Container/geometry/GltfModelLoader.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#if defined(CONTAINER_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <tuple>

namespace {
namespace gltf = container::geometry::gltf;
using Json = nlohmann::json;
constexpr const char* kPng = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
constexpr const char* kWidePng = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABEAYAAABPhRjKAAAAEUlEQVR4nGNgZGxgOBzw/z8AChEDlIUHTUAAAAAASUVORK5CYII=";
constexpr std::array<unsigned char, 74> kWideBytes{137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,16,6,0,0,0,79,133,24,202,0,0,0,17,73,68,65,84,120,156,99,96,100,108,96,56,28,240,255,63,0,10,17,3,148,133,7,77,64,0,0,0,0,73,69,78,68,174,66,96,130};

Json asset() {
  return {{"asset", {{"version", "2.0"}}}, {"images", {{{"uri", kPng}}}}, {"textures", {{{"source", 0}}}}};
}
Json alternatives() {
  auto value = asset();
  value["images"].push_back({{"uri", "image.ktx2"}, {"mimeType", "image/ktx2"}});
  value["images"].push_back({{"uri", "image.webp"}, {"mimeType", "image/webp"}});
  value["textures"][0]["extensions"] = {{"KHR_texture_basisu", {{"source", 1}}}, {"EXT_texture_webp", {{"source", 2}}}};
  value["extensionsUsed"] = {"KHR_texture_basisu", "EXT_texture_webp"};
  return value;
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    static std::atomic<unsigned> sequence{};
    root = std::filesystem::temp_directory_path() / ("container-image-sources-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
    std::filesystem::create_directory(root);
  }
  ~Fixture() {
    std::error_code ignored;
    for (const auto* name : {"asset.gltf", "image.png", "encoded.bin"}) std::filesystem::remove(root / name, ignored);
    std::filesystem::remove(root, ignored);
  }
  std::filesystem::path save(const Json& value) const {
    const auto path = root / "asset.gltf";
    std::ofstream(path) << value.dump();
    return path;
  }
};
template<class F> void fails(F function, std::string_view detail) {
  try { function(); FAIL() << "Expected rejection"; }
  catch (const std::exception& error) { EXPECT_NE(std::string(error.what()).find(detail), std::string::npos) << error.what(); }
}
std::vector<unsigned char> embeddedBytes(const char* value) {
  tinygltf::Model model;
  tinygltf::TinyGLTF parser;
  // This helper copies container bytes only; it does not decode their pixels.
  parser.SetImageLoader([](tinygltf::Image* image, int, std::string*, std::string*, int, int,
      const unsigned char* bytes, int count, void*) { image->image.assign(bytes, bytes + count); return true; }, nullptr);
  std::string error, warning;
  const auto text = Json{{"asset", {{"version", "2.0"}}}, {"images", {{{"uri", value}}}}}.dump();
  if (!parser.LoadASCIIFromString(&model, &error, &warning, text.data(), unsigned(text.size()), "")) throw std::runtime_error(error);
  return model.images.at(0).image;
}
} // namespace

TEST(GltfImageSources, EnabledAlternatePreferenceAndDisabledCoreFallbackAreCentralized) {
  auto value = alternatives();
  for (const auto [capabilities, expected, kind] : {
      std::tuple{gltf::ImageSourceCapabilities{false,false}, 0, gltf::TextureImageSourceKind::Core},
      std::tuple{gltf::ImageSourceCapabilities{false,true}, 2, gltf::TextureImageSourceKind::Webp},
      std::tuple{gltf::ImageSourceCapabilities{true,true}, 1, gltf::TextureImageSourceKind::Basisu}}) {
    const auto plan = gltf::SelectTextureImageSources(value, capabilities);
    ASSERT_EQ(plan.textures.size(), 1u);
    EXPECT_EQ(plan.textures[0].imageIndex, expected);
    EXPECT_EQ(plan.textures[0].kind, kind);
    EXPECT_EQ(plan.textures[0].strictKhrBasisu(), kind == gltf::TextureImageSourceKind::Basisu);
    EXPECT_TRUE(plan.selectedImages[size_t(expected)]);
  }
}
TEST(GltfImageSources, InvalidAlternatesAndMissingFallbackFailEvenWhenCodecIsDisabled) {
  for (const auto& source : {Json(-1), Json(3), Json(.5), Json(uint64_t{4294967297}), Json("one")}) {
    auto value = alternatives();
    value["textures"][0]["extensions"]["KHR_texture_basisu"]["source"] = source;
    fails([&] { (void)gltf::SelectTextureImageSources(value, {false,false}); }, "texture[0].extensions.KHR_texture_basisu.source");
  }
  auto value = alternatives(); value["textures"][0].erase("source");
  fails([&] { (void)gltf::SelectTextureImageSources(value, {false,false}); }, "core fallback");
  fails([&] { (void)gltf::SelectTextureImageSources(value, {true,false}); }, "extensionsRequired");
  value["extensionsRequired"] = {"KHR_texture_basisu"};
  fails([&] { (void)gltf::SelectTextureImageSources(value, {false,true}); }, "required image decoder");
  EXPECT_EQ(gltf::SelectTextureImageSources(value, {true,false}).textures[0].imageIndex, 1);
}
TEST(GltfImageSources, DdsOverridePreferenceFallbackAndRequiredContracts) {
  auto value = alternatives();
  value["images"].push_back({{"uri", "image.dds"}, {"mimeType", "image/vnd-ms.dds"}});
  value["textures"][0]["extensions"]["MSFT_texture_dds"] = {{"source", 3}};
  value["extensionsUsed"].push_back("MSFT_texture_dds");
  for (const auto [caps, expected, kind] : {
      std::tuple{gltf::ImageSourceCapabilities{true,true,true}, 1, gltf::TextureImageSourceKind::Basisu},
      std::tuple{gltf::ImageSourceCapabilities{false,true,true}, 3, gltf::TextureImageSourceKind::Dds},
      std::tuple{gltf::ImageSourceCapabilities{false,true,false}, 2, gltf::TextureImageSourceKind::Webp},
      std::tuple{gltf::ImageSourceCapabilities{false,false,false}, 0, gltf::TextureImageSourceKind::Core}}) {
    const auto selected = gltf::SelectTextureImageSources(value, caps).textures.at(0);
    EXPECT_EQ(selected.imageIndex, expected); EXPECT_EQ(selected.kind, kind);
  }
  value["textures"][0]["extensions"].erase("KHR_texture_basisu");
  value["textures"][0]["extensions"].erase("EXT_texture_webp");
  value["textures"][0].erase("source");
  fails([&] { (void)gltf::SelectTextureImageSources(value); }, "extensionsRequired");
  value["extensionsRequired"] = {"MSFT_texture_dds"};
  EXPECT_EQ(gltf::SelectTextureImageSources(value).textures.at(0).extension(), "MSFT_texture_dds");
  fails([&] { (void)gltf::SelectTextureImageSources(value, {false,false,false}); }, "required image decoder");
  value["images"][3]["mimeType"] = "image/png";
  fails([&] { (void)gltf::SelectTextureImageSources(value); }, "MIME type");
  value["images"][3]["mimeType"] = "image/vnd-ms.dds";
  for (const auto& invalid : {Json(-1), Json(4), Json(.5), Json(uint64_t{4294967297})}) {
    value["textures"][0]["extensions"]["MSFT_texture_dds"]["source"] = invalid;
    fails([&] { (void)gltf::SelectTextureImageSources(value, {false,false,false}); }, "required image decoder");
    value["extensionsRequired"] = Json::array(); value["textures"][0]["source"] = 0;
    fails([&] { (void)gltf::SelectTextureImageSources(value, {false,false,false}); }, "MSFT_texture_dds.source");
    value["extensionsRequired"] = {"MSFT_texture_dds"};
  }
}
TEST(GltfImageSources, CoreReferencesAndSelectedSourcesCannotBeSuppressedAsAlternates) {
  auto value = alternatives();
  value["textures"].push_back({{"source", 1}});
  const auto plan = gltf::SelectTextureImageSources(value, {false,false});
  EXPECT_FALSE(plan.unsupportedAlternateOnly[1]);
  EXPECT_TRUE(plan.unsupportedAlternateOnly[2]);
  value["textures"][0]["extensions"]["KHR_texture_basisu"] = 1;
  fails([&] { (void)gltf::SelectTextureImageSources(value, {false,false}); }, "must specify an image source");
}
TEST(GltfImageSources, DdsOverrideLoadsRetainedBytesAndPreservesPartialMips) {
  Fixture fixture;
  std::vector<unsigned char> bytes(128);
  const auto word = [&](size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<unsigned char>(value >> (i * 8));
  };
  word(0, 0x20534444); word(4, 124); word(8, 0x81007); word(12, 4); word(16, 4);
  word(20, 8); word(28, 2); word(76, 32); word(80, 4); word(84, 0x31545844); word(108, 0x1000);
  for (uint16_t color : {uint16_t{0xf800}, uint16_t{0x07e0}}) {
    bytes.push_back(static_cast<unsigned char>(color)); bytes.push_back(static_cast<unsigned char>(color >> 8));
    bytes.insert(bytes.end(), 6, 0);
  }
  std::ofstream file(fixture.root / "encoded.bin", std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); file.close();
  auto value = asset(); value["images"].push_back({{"uri", "encoded.bin"}, {"mimeType", "image/vnd-ms.dds"}});
  value["textures"][0]["extensions"]["MSFT_texture_dds"] = {{"source", 1}};
  value["extensionsUsed"] = {"MSFT_texture_dds"};
  const auto loaded = gltf::LoadSourceModel(fixture.save(value).string());
  EXPECT_TRUE(loaded.images.at(1).as_is); EXPECT_EQ(loaded.images.at(1).image, bytes);
  const auto selection = gltf::SelectTextureImageSource(loaded, 0);
  ASSERT_EQ(selection.kind, gltf::TextureImageSourceKind::Dds);
  auto decoded = gltf::DecodeSelectedTextureImage(loaded, selection, gltf::TextureImageRole::Data);
  ASSERT_EQ(decoded.mips.size(), 2u); EXPECT_TRUE(decoded.authoredMipChain);
  EXPECT_EQ(decoded.mips[0].width, 4u); EXPECT_EQ(decoded.mips[1].width, 2u);
  EXPECT_EQ(decoded.format, VK_FORMAT_R8G8B8A8_UNORM);
  EXPECT_EQ(decoded.mips[0].bytes[0], std::byte{255}); EXPECT_EQ(decoded.mips[0].bytes[1], std::byte{0});
  EXPECT_EQ(decoded.mips[1].bytes[0], std::byte{0}); EXPECT_EQ(decoded.mips[1].bytes[1], std::byte{255});
  auto malformed = loaded;
  malformed.images[1].image = embeddedBytes(kPng);
  fails([&] { (void)gltf::DecodeSelectedTextureImage(malformed, selection, gltf::TextureImageRole::Data); }, "requires a DDS image");
}
TEST(GltfImageSources, LoaderDefaultDecodedAndOptInExactEncodedContractsRemainDistinct) {
  Fixture fixture;
  auto value = asset(); value["images"][0]["uri"] = kWidePng;
  const auto path = fixture.save(value);
  const auto legacy = gltf::LoadSourceModel(path.string());
  ASSERT_EQ(legacy.images.size(), 1u);
  EXPECT_FALSE(legacy.images[0].as_is); EXPECT_EQ(legacy.images[0].bits, 16);
  EXPECT_EQ(legacy.images[0].pixel_type, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
  ASSERT_EQ(legacy.images[0].image.size(), 8u);
  std::array<uint16_t,4> raw{}; std::memcpy(raw.data(), legacy.images[0].image.data(), 8);
  EXPECT_EQ(raw, (std::array<uint16_t,4>{257,32768,50000,65535}));
  gltf::LoadOptions options; options.retainEncodedImages = true;
  const auto encoded = gltf::LoadSourceModel(path.string(), options);
  EXPECT_TRUE(encoded.images[0].as_is); EXPECT_EQ(encoded.images[0].mimeType, "image/png");
  EXPECT_EQ(encoded.images[0].image, (std::vector<unsigned char>(kWideBytes.begin(), kWideBytes.end())));
  options.decodeImages = false;
  const auto noDecode = gltf::LoadSourceModel(path.string(), options);
  EXPECT_TRUE(noDecode.images[0].image.empty()); EXPECT_FALSE(noDecode.images[0].as_is);
}
TEST(GltfImageSources, LegacyAndRetainedSixteenBitSemanticDecodePreservePrecisionAndRole) {
  Fixture fixture; auto value = asset(); value["images"][0]["uri"] = kWidePng;
  const auto path = fixture.save(value);
  for (bool retain : {false,true}) {
    gltf::LoadOptions options; options.retainEncodedImages = retain;
    const auto source = gltf::LoadSourceModel(path.string(), options);
    const auto selected = gltf::SelectTextureImageSource(source, 0);
    auto data = gltf::DecodeSelectedTextureImage(source, selected, gltf::TextureImageRole::Data);
    EXPECT_EQ(data.format, VK_FORMAT_R16G16B16A16_UNORM);
    std::array<uint16_t,4> raw{}; ASSERT_EQ(data.mips.at(0).bytes.size(), 8u);
    std::memcpy(raw.data(), data.mips[0].bytes.data(), 8);
    EXPECT_EQ(raw, (std::array<uint16_t,4>{257,32768,50000,65535}));
    auto color = gltf::DecodeSelectedTextureImage(source, selected, gltf::TextureImageRole::Color);
    EXPECT_EQ(color.format, VK_FORMAT_R32G32B32A32_SFLOAT);
    std::array<float,4> linear{}; std::memcpy(linear.data(), color.mips.at(0).bytes.data(), 16);
    EXPECT_NEAR(linear[0], (257.f/65535.f)/12.92f, 1e-7);
    EXPECT_NEAR(linear[1], std::pow((32768.f/65535.f+.055f)/1.055f, 2.4f), 1e-7);
    EXPECT_EQ(linear[3], 1.f);
    container::material::TextureDecodeOptions profile; profile.allowedFormats = {VK_FORMAT_R8G8B8A8_UNORM};
    EXPECT_THROW((void)gltf::DecodeSelectedTextureImage(source, selected, gltf::TextureImageRole::Data, profile), std::runtime_error);
    profile = {}; profile.strictKhrBasisu = true;
    EXPECT_THROW((void)gltf::DecodeSelectedTextureImage(source, selected, gltf::TextureImageRole::Data, profile), std::runtime_error);
  }
}
TEST(GltfImageSources, ManualRetentionMimeDetectionIncludesHdrBmpAndGif) {
  for (const auto& [header,mime] : {std::pair{"#?RADIANCE\n", "image/vnd.radiance"},
                                  std::pair{"#?RGBE\n", "image/vnd.radiance"},
                                  std::pair{"BMhead", "image/bmp"},std::pair{"GIF89a", "image/gif"}}) {
    const auto bytes = std::span(reinterpret_cast<const unsigned char*>(header),std::strlen(header));
    EXPECT_EQ(gltf::EncodedTextureMimeType(bytes),mime);
  }
}
TEST(GltfImageSources, ManualUnknownMimeFormatsRetainPortablePixelsAndPrecision) {
  // Uncompressed top-left TGA; each pixel has distinct color and alpha.
  std::vector<std::byte> tga(18);
  tga[2] = std::byte{2}; tga[12] = std::byte{2}; tga[14] = std::byte{1};
  tga[16] = std::byte{32}; tga[17] = std::byte{0x28};
  for (unsigned char value : {53,29,17,149,127,103,91,255}) tga.push_back(std::byte{value});
  const auto archived = gltf::RetainTextureImageForExport(tga);
  EXPECT_FALSE(archived.as_is); EXPECT_TRUE(archived.mimeType.empty());
  EXPECT_EQ(archived.width, 2); EXPECT_EQ(archived.bits, 8); EXPECT_EQ(archived.component, 4);
  EXPECT_EQ(archived.image, (std::vector<unsigned char>{17,29,53,149,91,103,127,255}));
  // PSD stores 16-bit planar samples in big-endian order. This minimal raw PSD
  // avoids depending on an encoder to verify exact adjacent 16-bit values.
  std::vector<std::byte> psd;
  const auto byte = [&](unsigned value) { psd.push_back(static_cast<std::byte>(value)); };
  for (char c : std::string("8BPS")) byte(c);
  for (unsigned value : {0,1, 0,0,0,0,0,0, 0,4, 0,0,0,1, 0,0,0,2, 0,16, 0,3,
                         0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0}) byte(value);
  for (uint16_t value : {0x1234,0x1235,0x8001,0x8002,0xfedc,0xfedd,0xffff,0xffff}) {
    byte(value >> 8); byte(value & 255);
  }
  const auto wide = gltf::RetainTextureImageForExport(psd);
  EXPECT_FALSE(wide.as_is); EXPECT_EQ(wide.bits, 16);
  EXPECT_EQ(wide.pixel_type, TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
  std::array<uint16_t,8> words{}; ASSERT_EQ(wide.image.size(), sizeof(words));
  std::memcpy(words.data(), wide.image.data(), sizeof(words));
  EXPECT_EQ(words, (std::array<uint16_t,8>{0x1234,0x8001,0xfedc,0xffff,0x1235,0x8002,0xfedd,0xffff}));
  const auto png = embeddedBytes(kWidePng);
  const auto retained = gltf::RetainTextureImageForExport({reinterpret_cast<const std::byte*>(png.data()),png.size()});
  EXPECT_TRUE(retained.as_is); EXPECT_EQ(retained.mimeType, "image/png"); EXPECT_EQ(retained.image, png);
  container::material::TextureDecodeOptions limits; limits.maxEncodedBytes = 1;
  EXPECT_THROW((void)gltf::RetainTextureImageForExport(tga, limits), std::runtime_error);
}

TEST(GltfImageSources, NewContainerDataUriMimeTypesKeepEncodedBytesAndPlusSigns) {
  Fixture fixture;
  // Preserve the payload even when its URI MIME is outside the parser's
  // allowlist. Pixel/container validation remains the typed decoder's job.
  for (const auto* mime : {"image/ktx2", "image/ktx", "image/webp", "image/vnd-ms.dds", "image/vnd.radiance"}) {
    auto value = asset();
    const auto payload = std::string(kWidePng).substr(std::string(kWidePng).find(',') + 1);
    value["images"][0]["uri"] = std::string("data:") + mime + ";base64," + payload;
    gltf::LoadOptions options; options.retainEncodedImages = true;
    const auto loaded = gltf::LoadSourceModel(fixture.save(value).string(), options);
    EXPECT_TRUE(loaded.images[0].as_is);
    EXPECT_EQ(loaded.images[0].image, (std::vector<unsigned char>(kWideBytes.begin(), kWideBytes.end())));
    options.decodeImages = false;
    EXPECT_TRUE(gltf::LoadSourceModel(fixture.save(value).string(), options).images[0].image.empty());
  }
  auto plus = asset(); plus["images"][0]["uri"] = "data:image/ktx2;base64,+/8=";
  gltf::LoadOptions retained; retained.retainEncodedImages = true;
  EXPECT_EQ(gltf::LoadSourceModel(fixture.save(plus).string(), retained).images[0].image,
            (std::vector<unsigned char>{251,255}));
  auto invalid = asset(); invalid["images"][0]["uri"] = "data:image/ktx2,opaque";
  fails([&] { (void)gltf::LoadSourceModel(fixture.save(invalid).string()); }, "image[0]");
}
TEST(GltfImageSources, MissingSelectedExternalAndMalformedViewRejectWithImageContext) {
  Fixture fixture; auto value = asset(); value["images"][0]["uri"] = "missing.png";
  fails([&] { (void)gltf::LoadSourceModel(fixture.save(value).string()); }, "image[0]");
  for (const auto& index : {Json(-1),Json(1),Json(.5)}) {
    value = asset(); value["images"][0] = {{"bufferView", index}, {"mimeType", "image/png"}};
    value["buffers"] = {{{"byteLength", 4}, {"uri", "must-not-be-read.bin"}}};
    value["bufferViews"] = {{{"buffer", 0}, {"byteLength", 4}}};
    fails([&] { (void)gltf::LoadSourceModel(fixture.save(value).string()); }, "image[0]");
  }
  value["images"][0]["bufferView"] = 0; value["bufferViews"][0]["byteOffset"] = 3;
  fails([&] { (void)gltf::LoadSourceModel(fixture.save(value).string()); }, "image[0] view byte range");
}
TEST(GltfImageSources, EncodedCopiesAndDecodedPixelsObserveTheAggregateInputBudget) {
  Fixture fixture; const auto value = asset(); const auto path = fixture.save(value);
  const auto encoded = embeddedBytes(kPng);
  gltf::LoadOptions options;
  options.retainEncodedImages = true;
  options.maxInputBytes = std::filesystem::file_size(path) + encoded.size() * 2 - 1;
  fails([&] { (void)gltf::LoadSourceModel(path.string(), options); }, "aggregate");
  options.retainEncodedImages = false; options.maxInputBytes += 4;
  fails([&] { (void)gltf::LoadSourceModel(path.string(), options); }, "aggregate");
  options.maxInputBytes += 1;
  EXPECT_EQ(gltf::LoadSourceModel(path.string(), options).images[0].image.size(), 4u);
}
TEST(GltfImageSources, CompressedImageViewIsDeferredUntilMeshoptNormalization) {
#if defined(CONTAINER_HAS_MESHOPT)
  Fixture fixture;
  auto png = embeddedBytes(kPng); while (png.size()%4) png.push_back(0);
  meshopt_encodeVertexVersion(0);
  std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(png.size()/4,4));
  encoded.resize(meshopt_encodeVertexBuffer(encoded.data(),encoded.size(),png.data(),png.size()/4,4));
  std::ofstream file(fixture.root/"encoded.bin",std::ios::binary);
  file.write(reinterpret_cast<const char*>(encoded.data()),std::streamsize(encoded.size())); file.close();
  auto value = asset(); value["extensionsUsed"] = {"EXT_meshopt_compression"}; value["extensionsRequired"] = {"EXT_meshopt_compression"};
  value["buffers"] = {{{"byteLength",encoded.size()},{"uri","encoded.bin"}},{{"byteLength",png.size()}}};
  value["bufferViews"] = {{{"buffer",1},{"byteLength",png.size()},{"extensions",{{"EXT_meshopt_compression",{
      {"buffer",0},{"byteLength",encoded.size()},{"count",png.size()/4},{"byteStride",4},{"mode","ATTRIBUTES"}}}}}}};
  value["images"][0] = {{"bufferView",0},{"mimeType","image/png"}};
  const auto path = fixture.save(value);
  const auto loaded = gltf::LoadSourceModel(path.string());
  EXPECT_EQ(loaded.images[0].width,1); EXPECT_EQ(loaded.images[0].image,(std::vector<unsigned char>{255,0,0,255}));
  gltf::LoadOptions options; options.retainEncodedImages = true;
  const auto retained = gltf::LoadSourceModel(path.string(),options);
  EXPECT_TRUE(retained.images[0].as_is); EXPECT_EQ(retained.images[0].image,png);
  options.decodeImages = false;
  EXPECT_TRUE(gltf::LoadSourceModel(path.string(),options).images[0].image.empty());
#else
  auto value = asset(); value["extensionsUsed"] = {"EXT_meshopt_compression"};
  value["extensionsRequired"] = {"EXT_meshopt_compression"};
  Fixture fixture;
  EXPECT_THROW((void)gltf::LoadSourceModel(fixture.save(value).string()), std::runtime_error);
#endif
}

namespace {
struct TangentFixture {
  tinygltf::Model source;
  container::geometry::Model bind;
  TangentFixture(bool authored = false) {
    using namespace container::geometry;
    std::vector<Vertex> vertices(3);
    vertices[0].position = {0,0,0}; vertices[1].position = {1,0,0}; vertices[2].position = {0,1,0};
    vertices[0].texCoord = {0,0}; vertices[1].texCoord = {1,0}; vertices[2].texCoord = {0,1};
    vertices[0].texCoord1 = {1,0}; vertices[1].texCoord1 = {0,0}; vertices[2].texCoord1 = {1,1};
    for (auto& vertex : vertices) { vertex.normal = {0,0,1}; vertex.tangent = {0,1,0,-1}; vertex.color = {.2f,.3f,.4f}; vertex.colorAlpha = .6f; }
    auto procedural = vertices; for (auto& vertex : procedural) vertex.position.x += 5;
    bind = Model::FromMeshes({Mesh(vertices,{0,1,2},7,true),Mesh(procedural,{0,2,1},9,false)});
    tinygltf::Accessor position; position.count = 3; source.accessors.push_back(position);
    tinygltf::Primitive primitive; primitive.attributes = {{"POSITION",0},{"NORMAL",0}};
    if (authored) primitive.attributes["TANGENT"] = 0;
    tinygltf::Mesh mesh; mesh.primitives.push_back(primitive); source.meshes.push_back(mesh);
    // The variant caller has already resolved any texture-transform selector.
    // This original selector must not override the supplied per-primitive UV1.
    tinygltf::Material material; material.normalTexture.texCoord = 0; source.materials.push_back(material);
  }
};
void expectSameVertexExceptTangent(const container::geometry::Vertex& a,const container::geometry::Vertex& b) {
  EXPECT_EQ(a.position,b.position); EXPECT_EQ(a.normal,b.normal); EXPECT_EQ(a.color,b.color);
  EXPECT_EQ(a.colorAlpha,b.colorAlpha); EXPECT_EQ(a.texCoord,b.texCoord); EXPECT_EQ(a.texCoord1,b.texCoord1);
}
} // namespace
TEST(GltfBindTangents, VariantUvRegenerationUsesExistingMikkFramesAndPreservesAppendedMeshes) {
  TangentFixture fixture;
  const auto original = fixture.bind;
  const std::array<uint32_t,1> uv0{0},uv1{1};
  const auto first = gltf::RegenerateGltfBindTangents(fixture.source,fixture.bind,uv0);
  const auto second = gltf::RegenerateGltfBindTangents(fixture.source,fixture.bind,uv1);
  ASSERT_EQ(second.meshes().size(),2u);
  for (size_t i=0;i<3;++i) {
    EXPECT_NEAR(first.meshes()[0].vertices()[i].tangent.x,1.f,1e-6);
    EXPECT_NEAR(second.meshes()[0].vertices()[i].tangent.x,-1.f,1e-6);
    EXPECT_EQ(first.meshes()[0].vertices()[i].tangent.w,1.f);
    EXPECT_EQ(second.meshes()[0].vertices()[i].tangent.w,-1.f);
    expectSameVertexExceptTangent(second.meshes()[0].vertices()[i],original.meshes()[0].vertices()[i]);
    EXPECT_EQ(second.meshes()[1].vertices()[i].tangent,original.meshes()[1].vertices()[i].tangent);
    expectSameVertexExceptTangent(second.meshes()[1].vertices()[i],original.meshes()[1].vertices()[i]);
    EXPECT_EQ(fixture.bind.vertices()[i].tangent,original.vertices()[i].tangent);
  }
  EXPECT_EQ(second.meshes()[0].indices(),original.meshes()[0].indices());
  EXPECT_EQ(second.meshes()[1].indices(),original.meshes()[1].indices());
  EXPECT_EQ(second.meshes()[0].materialIndex(),7); EXPECT_TRUE(second.meshes()[0].disableBackfaceCulling());
  EXPECT_EQ(second.meshes()[1].materialIndex(),9); EXPECT_FALSE(second.meshes()[1].disableBackfaceCulling());
  EXPECT_EQ(fixture.source.materials[0].normalTexture.texCoord,0);
  EXPECT_EQ(fixture.source.meshes[0].primitives[0].attributes.count("TANGENT"),0u);
}
TEST(GltfBindTangents, AuthoredNormalAndTangentFramesRemainUnchanged) {
  TangentFixture fixture(true); const std::array<uint32_t,1> uv1{1};
  const auto result = gltf::RegenerateGltfBindTangents(fixture.source,fixture.bind,uv1);
  for (size_t i=0;i<fixture.bind.vertices().size();++i) {
    EXPECT_EQ(result.vertices()[i].tangent,fixture.bind.vertices()[i].tangent);
    expectSameVertexExceptTangent(result.vertices()[i],fixture.bind.vertices()[i]);
  }
}
TEST(GltfBindTangents, InvalidUvOrSourceTablesFailBeforeAnyBindChanges) {
  TangentFixture fixture; const std::array<uint32_t,1> invalid{2},valid{0};
  EXPECT_THROW((void)gltf::RegenerateGltfBindTangents(fixture.source,fixture.bind,{}),std::runtime_error);
  EXPECT_THROW((void)gltf::RegenerateGltfBindTangents(fixture.source,fixture.bind,invalid),std::runtime_error);
  fixture.source.accessors[0].count = 4;
  EXPECT_THROW((void)gltf::RegenerateGltfBindTangents(fixture.source,fixture.bind,valid),std::runtime_error);
  fixture.source.accessors[0].count = 3;
  auto invalidBind = container::geometry::Model::FromMeshes({container::geometry::Mesh(
      fixture.bind.meshes()[0].vertices(), {0,1,99})});
  EXPECT_THROW((void)gltf::RegenerateGltfBindTangents(fixture.source,invalidBind,valid),std::runtime_error);
  EXPECT_EQ(fixture.bind.vertices()[0].tangent,(glm::vec4{0,1,0,-1}));
}
