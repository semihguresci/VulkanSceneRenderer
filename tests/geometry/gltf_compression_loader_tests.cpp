#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfAccessor.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#if defined(CONTAINER_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
namespace gltf = container::geometry::gltf;
using Json = nlohmann::json;
constexpr std::array<float, 9> kPositions{0,0,0, 1,0,0, 0,1,0};

class Fixture {
 public:
  Fixture() {
    static std::atomic<unsigned> sequence{};
    root = std::filesystem::temp_directory_path() / ("container-compression-loader-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directory(root);
  }
  ~Fixture() {
    std::error_code ignored;
    for (const auto* name : {"asset.gltf", "asset.glb", "encoded.bin", "fallback.bin"})
      std::filesystem::remove(root / name, ignored);
    std::filesystem::remove(root, ignored);
  }
  Json asset(const char* extension, bool required = true) {
    std::vector<unsigned char> encoded{0};
#if defined(CONTAINER_HAS_MESHOPT)
    meshopt_encodeVertexVersion(std::string_view(extension) == "EXT_meshopt_compression" ? 0 : 1);
    encoded.resize(meshopt_encodeVertexBufferBound(3, 12));
    const size_t length = meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), kPositions.data(), 3, 12);
    if (!length) throw std::runtime_error("fixture encoder failed");
    encoded.resize(length);
#endif
    std::ofstream file(root / "encoded.bin", std::ios::binary);
    file.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    std::ofstream fallback(root / "fallback.bin", std::ios::binary);
    fallback.write(reinterpret_cast<const char*>(kPositions.data()), sizeof(kPositions));
    auto value = Json::parse(R"({
      "asset":{"version":"2.0"},
      "buffers":[{"uri":"encoded.bin","byteLength":1},{"byteLength":36}],
      "bufferViews":[{"buffer":1,"byteLength":36,"byteStride":12}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,
        "type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],
      "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
    })");
    value["extensionsUsed"] = {extension};
    value["buffers"][0]["byteLength"] = encoded.size();
    value["bufferViews"][0]["extensions"][extension] = {
      {"buffer", 0}, {"byteLength", encoded.size()}, {"byteStride", 12},
      {"count", 3}, {"mode", "ATTRIBUTES"}};
    if (required) value["extensionsRequired"] = {extension};
    return value;
  }
  std::filesystem::path save(Json value, bool binary = false) {
    std::vector<unsigned char> payload;
    if (binary) {
      std::ifstream input(root / "encoded.bin", std::ios::binary);
      payload.assign(std::istreambuf_iterator<char>(input), {});
      value["buffers"][0].erase("uri");
    }
    auto text = value.dump();
    const auto path = root / (binary ? "asset.glb" : "asset.gltf");
    std::ofstream file(path, std::ios::binary);
    auto word = [&](uint32_t number) {
      for (unsigned shift = 0; shift < 32; shift += 8)
        file.put(static_cast<char>(number >> shift));
    };
    if (binary) {
      while (text.size() % 4) text += ' ';
      while (payload.size() % 4) payload.push_back(0);
      word(0x46546c67); word(2); word(static_cast<uint32_t>(28 + text.size() + payload.size()));
      word(static_cast<uint32_t>(text.size())); word(0x4e4f534a);
      file << text;
      word(static_cast<uint32_t>(payload.size())); word(0x004e4942);
      file.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    } else file << text;
    return path;
  }
  std::filesystem::path root;
};

void expectPositions(const tinygltf::Model& source) {
  gltf::CheckedAccessor positions(source, 0, "decoded POSITION",
                                  {gltf::AccessorUsage::VertexAttribute});
  for (size_t i = 0; i < 3; ++i) {
    std::array<float, 3> actual{};
    positions.readFloats(i, actual);
    for (size_t axis = 0; axis < 3; ++axis) EXPECT_FLOAT_EQ(actual[axis], kPositions[i * 3 + axis]);
  }
}
} // namespace

TEST(GltfCompressionLoader, RequiredPlaceholderUsesDetachedAsciiAndGlbParsing) {
  for (const auto* name : {"EXT_meshopt_compression", "KHR_meshopt_compression"}) {
    SCOPED_TRACE(name);
    for (bool binary : {false, true}) {
      SCOPED_TRACE(binary);
      Fixture fixture;
      const auto path = fixture.save(fixture.asset(name), binary);
      if (!gltf::HasMeshoptDecoder()) {
        EXPECT_THROW((void)gltf::LoadSourceModel(path.string()), std::runtime_error);
        continue;
      }
      const auto before = std::filesystem::file_size(path);
      const auto loaded = gltf::LoadModelWithSource(path.string(), {false, 1024 * 1024});
      ASSERT_EQ(loaded.model.vertices().size(), 3u);
      expectPositions(loaded.gltfModel);
      EXPECT_TRUE(loaded.gltfModel.buffers[1].data.empty());
      EXPECT_TRUE(loaded.gltfModel.buffers[1].uri.empty());
      EXPECT_TRUE(loaded.gltfModel.extensionsRequired.empty());
      EXPECT_TRUE(loaded.gltfModel.extensionsUsed.empty());
      EXPECT_EQ(std::filesystem::file_size(path), before);
      for (const auto& entry : std::filesystem::directory_iterator(fixture.root))
        EXPECT_EQ(entry.path().filename().string().find("__container_meshopt_"), std::string::npos);
    }
  }
}

TEST(GltfCompressionLoader, OptionalTaggedFallbackIsUsableWithDecodersDisabled) {
  for (const auto* name : {"EXT_meshopt_compression", "KHR_meshopt_compression"}) {
    Fixture fixture;
    auto value = fixture.asset(name, false);
    value["buffers"][1]["uri"] = "fallback.bin";
    value["buffers"][1]["extensions"][name] = {{"fallback", true}};
    const auto loaded = gltf::LoadModelWithSource(fixture.save(value).string(), {false, 1024 * 1024});
    ASSERT_EQ(loaded.model.vertices().size(), 3u);
    expectPositions(loaded.gltfModel);
  }
}

TEST(GltfCompressionLoader, MissingOptionalFallbackAndHugeDeclarationsFailBeforeResourceIo) {
  Fixture fixture;
  auto optional = fixture.asset("EXT_meshopt_compression", false);
  EXPECT_THROW((void)gltf::LoadSourceModel(fixture.save(optional).string()), std::runtime_error);
  auto huge = fixture.asset("EXT_meshopt_compression");
  huge["buffers"][0]["uri"] = "must-not-be-read.bin";
  huge["buffers"][1]["byteLength"] = uint64_t(1) << 34;
  const auto hugePath = fixture.save(huge);
  try {
    (void)gltf::LoadSourceModel(hugePath.string(), {false, 8192});
    FAIL() << "Expected preflight rejection";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string_view(error.what()).find(hugePath.string()), std::string_view::npos);
    if (gltf::HasMeshoptDecoder())
      EXPECT_NE(std::string_view(error.what()).find("input-memory budget"), std::string_view::npos);
  }
  if (gltf::HasMeshoptDecoder()) {
    auto bounded = fixture.asset("EXT_meshopt_compression");
    gltf::LoadOptions options;
    options.decodeImages = false;
    options.compression.maxDecodedBytes = 35;
    const auto path = fixture.save(bounded);
    try {
      (void)gltf::LoadSourceModel(path.string(), options);
      FAIL() << "Expected decoded budget rejection";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string_view(error.what()).find(path.string()), std::string_view::npos);
      EXPECT_NE(std::string_view(error.what()).find("budget"), std::string_view::npos);
    }
  }
}
