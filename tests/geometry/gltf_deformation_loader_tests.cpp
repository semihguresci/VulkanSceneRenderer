#include <Container/geometry/GltfModelLoader.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace gltf = container::geometry::gltf;
using Json = nlohmann::json;

std::string base64(std::span<const unsigned char> bytes) {
  constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((bytes.size()+2)/3*4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t value = uint32_t(bytes[i]) << 16 |
        (i+1 < bytes.size() ? uint32_t(bytes[i+1]) << 8 : 0) |
        (i+2 < bytes.size() ? uint32_t(bytes[i+2]) : 0);
    output += alphabet[(value >> 18) & 63];
    output += alphabet[(value >> 12) & 63];
    output += i+1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
    output += i+2 < bytes.size() ? alphabet[value & 63] : '=';
  }
  return output;
}

class DeformationAsset {
 public:
  DeformationAsset() {
    static std::atomic<unsigned> sequence{};
    directory_ = std::filesystem::temp_directory_path() /
        ("container_deformation_loader_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(sequence++));
    std::filesystem::create_directories(directory_);
    document = {{"asset",{{"version","2.0"}}}, {"accessors",Json::array()},
        {"bufferViews",Json::array()}, {"meshes",Json::array()},
        {"nodes",Json::array({Json{{"mesh",0}}})},
        {"scenes",Json::array({Json{{"nodes",Json::array({0})}}})}, {"scene",0}};
  }
  ~DeformationAsset() {
    std::error_code ignored;
    for (const auto& file : files_) std::filesystem::remove(file,ignored);
    std::filesystem::remove(directory_,ignored);
  }

  int floats(const char* type, size_t components, std::span<const float> values,
              bool positionBounds = false) {
    align();
    const size_t offset = data.size();
    for (float value : values) appendWord(std::bit_cast<uint32_t>(value));
    const int index = accessor(type,5126,components,values.size(),offset,false);
    if (positionBounds) {
      std::vector<float> minimum(components,std::numeric_limits<float>::max());
      std::vector<float> maximum(components,std::numeric_limits<float>::lowest());
      for (size_t i = 0; i < values.size(); ++i) {
        minimum[i%components] = std::min(minimum[i%components],values[i]);
        maximum[i%components] = std::max(maximum[i%components],values[i]);
      }
      document["accessors"][index]["min"] = minimum;
      document["accessors"][index]["max"] = maximum;
    }
    return index;
  }
  int bytes(const char* type, size_t components, std::span<const unsigned char> values,
             bool normalized = false) {
    align();
    const size_t offset = data.size();
    data.insert(data.end(),values.begin(),values.end());
    return accessor(type,5121,components,values.size(),offset,normalized);
  }
  Json triangle(float baseX = 0, size_t vertexCount = 3, size_t influenceSets = 0) {
    EXPECT_EQ(vertexCount%3,0u);
    std::vector<float> positions, normals;
    positions.reserve(vertexCount*3); normals.reserve(vertexCount*3);
    constexpr std::array<std::array<float,3>,3> triangle{{{0,0,0},{1,0,0},{0,1,0}}};
    for (size_t i = 0; i < vertexCount; ++i) {
      auto value = triangle[i%3]; value[0] += baseX;
      positions.insert(positions.end(),value.begin(),value.end());
      normals.insert(normals.end(),{0,0,1});
    }
    Json attributes{{"POSITION",floats("VEC3",3,positions,true)},
                    {"NORMAL",floats("VEC3",3,normals)}};
    for (size_t set = 0; set < influenceSets; ++set) {
      std::vector<unsigned char> joints(vertexCount*4,0), weights(vertexCount*4,0);
      for (size_t i = 0; i < vertexCount; ++i) weights[i*4] = 255;
      attributes["JOINTS_"+std::to_string(set)] = bytes("VEC4",4,joints);
      attributes["WEIGHTS_"+std::to_string(set)] = bytes("VEC4",4,weights,true);
    }
    return {{"attributes",std::move(attributes)},{"mode",4}};
  }
  int positionDelta(size_t vertexCount, float x = 1) {
    std::vector<float> values(vertexCount*3,0);
    for (size_t i = 0; i < vertexCount; ++i) values[i*3] = x;
    return floats("VEC3",3,values);
  }
  void addSkin(bool inverseBind = true) {
    document["nodes"][0]["skin"] = 0;
    document["nodes"].push_back({{"name","Joint"},{"translation",{0,2,0}}});
    document["scenes"][0]["nodes"].push_back(1);
    Json skin{{"joints",{1}},{"skeleton",1}};
    if (inverseBind) {
      const std::array<float,16> matrix{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,-2,0,1};
      skin["inverseBindMatrices"] = floats("MAT4",16,matrix);
    }
    document["skins"] = Json::array({std::move(skin)});
  }
  void combined() {
    auto primitive = triangle(0,3,1);
    primitive["targets"] = Json::array({Json{{"POSITION",positionDelta(3)}}});
    document["meshes"].push_back({{"primitives",Json::array({primitive})},{"weights",{0.75}}});
    document["nodes"][0]["weights"] = {-0.25};
    addSkin();
  }
  std::filesystem::path save(const char* name, bool binary = false) {
    auto json = document;
    json["buffers"] = Json::array({Json{{"byteLength",data.size()}}});
    if (!binary) json["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64(data);
    const auto path = directory_ / name;
    if (std::find(files_.begin(),files_.end(),path) == files_.end()) files_.push_back(path);
    std::ofstream file(path,std::ios::binary);
    if (!binary) file << json.dump();
    else {
      auto text = json.dump(); while (text.size()%4) text += ' ';
      auto binaryData = data; while (binaryData.size()%4) binaryData.push_back(0);
      auto word = [&](uint32_t value) {
        for (int i = 0; i < 4; ++i) file.put(static_cast<char>(value >> (i*8)));
      };
      word(0x46546c67); word(2);
      word(static_cast<uint32_t>(12+8+text.size()+8+binaryData.size()));
      word(static_cast<uint32_t>(text.size())); word(0x4e4f534a);
      file.write(text.data(),static_cast<std::streamsize>(text.size()));
      word(static_cast<uint32_t>(binaryData.size())); word(0x004e4942);
      file.write(reinterpret_cast<const char*>(binaryData.data()),static_cast<std::streamsize>(binaryData.size()));
    }
    file.close();
    return path;
  }

  Json document;
  std::vector<unsigned char> data;

 private:
  void align() { while (data.size()%4) data.push_back(0); }
  void appendWord(uint32_t value) {
    for (int i = 0; i < 4; ++i) data.push_back(static_cast<unsigned char>(value >> (i*8)));
  }
  int accessor(const char* type, int componentType, size_t components,
                size_t valueCount, size_t offset, bool normalized) {
    const int view = static_cast<int>(document["bufferViews"].size());
    document["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",data.size()-offset}});
    Json value{{"bufferView",view},{"componentType",componentType},{"count",valueCount/components},{"type",type}};
    if (normalized) value["normalized"] = true;
    document["accessors"].push_back(std::move(value));
    return static_cast<int>(document["accessors"].size()-1);
  }
  std::filesystem::path directory_;
  std::vector<std::filesystem::path> files_;
};

void expectRejected(const std::filesystem::path& path, std::string_view detail) {
  try {
    (void)gltf::LoadModelWithSource(path.string(),{false});
    FAIL() << "Malformed source unexpectedly returned a prepared render payload";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find(path.string()),std::string::npos) << message;
    EXPECT_NE(message.find(detail),std::string::npos) << message;
  }
}

TEST(GltfDeformationLoader, StaticSourceAvoidsOptionalDecodedDeformationData) {
  DeformationAsset asset;
  asset.document["meshes"].push_back({{"primitives",Json::array({asset.triangle()})}});
  for (bool binary : {false,true}) {
    const auto path = asset.save(binary ? "static.glb" : "static.gltf",binary);
    const auto loaded = gltf::LoadModelWithSource(path.string(),{false});
    EXPECT_FALSE(loaded.deformation);
    ASSERT_EQ(loaded.model.vertices().size(),3u);
    EXPECT_FLOAT_EQ(loaded.model.vertices()[1].position.x,1);
    EXPECT_EQ(loaded.model.indices(),(std::vector<uint32_t>{0,1,2}));
  }
}

TEST(GltfDeformationLoader, SelfContainedJsonAndGlbRetainMorphSkinAndAuthoredDefaults) {
  DeformationAsset asset; asset.combined();
  for (bool binary : {false,true}) {
    SCOPED_TRACE(binary ? "GLB" : "embedded glTF");
    const auto path = asset.save(binary ? "combined.glb" : "combined.gltf",binary);
    const auto loaded = gltf::LoadModelWithSource(path.string(),{false});
    ASSERT_TRUE(loaded.deformation);
    const auto& data = *loaded.deformation;
    ASSERT_EQ(data.primitives.size(),1u); ASSERT_EQ(data.skins.size(),1u); ASSERT_EQ(data.nodes.size(),2u);
    const auto& primitive = data.primitives[0];
    EXPECT_EQ(primitive.sourceMesh,0u); EXPECT_EQ(primitive.sourcePrimitive,0u);
    EXPECT_TRUE(primitive.hasSkinning()); EXPECT_TRUE(primitive.hasMorphing());
    EXPECT_EQ(primitive.meshDefaultWeights,(std::vector<float>{0.75f}));
    EXPECT_EQ(data.nodes[0].defaultMorphWeights,(std::vector<float>{-0.25f}));
    EXPECT_EQ(loaded.gltfModel.meshes[0].weights,(std::vector<double>{0.75}));
    EXPECT_EQ(loaded.gltfModel.nodes[0].weights,(std::vector<double>{-0.25}));
    EXPECT_EQ(data.skins[0].joints,(std::vector<uint32_t>{1}));
    ASSERT_EQ(data.skins[0].inverseBindMatrices.size(),1u);
    EXPECT_FLOAT_EQ(data.skins[0].inverseBindMatrices[0][3][1],-2);
    ASSERT_EQ(primitive.influences.size(),3u);
    ASSERT_EQ(primitive.influences[0].values.size(),4u);
    EXPECT_FLOAT_EQ(primitive.influences[0].values[0].weight,1);
    const auto posed = gltf::EvaluatePrimitive(primitive,data.nodes[0].defaultMorphWeights);
    EXPECT_FLOAT_EQ(posed[0].position.x,-0.25f);
    EXPECT_FLOAT_EQ(posed[1].position.x,0.75f);
    EXPECT_FLOAT_EQ(primitive.bindVertices[0].position.x,0);
  }
}

TEST(GltfDeformationLoader, MeshMajorPrimitiveOrderingMatchesRetainedBindVertices) {
  DeformationAsset asset;
  auto first = asset.triangle(0), second = asset.triangle(10), third = asset.triangle(20);
  first["targets"] = Json::array({Json{{"POSITION",asset.positionDelta(3,1)}}});
  second["targets"] = Json::array({Json{{"POSITION",asset.positionDelta(3,2)}}});
  asset.document["meshes"] = Json::array({Json{{"primitives",Json::array({first,second})},{"weights",{0.5}}},
                                        Json{{"primitives",Json::array({third})}}});
  asset.document["nodes"].push_back({{"mesh",1}});
  asset.document["scenes"][0]["nodes"].push_back(1);
  const auto loaded = gltf::LoadModelWithSource(asset.save("ordered.gltf").string(),{false});
  ASSERT_TRUE(loaded.deformation); ASSERT_EQ(loaded.deformation->primitives.size(),3u);
  for (size_t i = 0; i < 3; ++i) {
    const auto& retained = loaded.deformation->primitives[i];
    ASSERT_EQ(retained.bindVertices.size(),3u);
    EXPECT_FLOAT_EQ(retained.bindVertices[0].position.x,static_cast<float>(i*10));
    EXPECT_FLOAT_EQ(loaded.model.meshes()[i].vertices()[0].position.x,retained.bindVertices[0].position.x);
    EXPECT_EQ(retained.sourceMesh,i < 2 ? 0u : 1u);
    EXPECT_EQ(retained.sourcePrimitive,i == 1 ? 1u : 0u);
  }
  EXPECT_FLOAT_EQ(loaded.deformation->primitives[1].morphTargets[0].positions[0].x,2);
  EXPECT_FALSE(loaded.deformation->primitives[2].hasMorphing());
  EXPECT_EQ(loaded.deformation->nodes[0].defaultMorphWeights,(std::vector<float>{0.5f}));
}

TEST(GltfDeformationLoader, PreparedSourceRoundTripsPortableJsonAndGlbWithoutLosingDeformation) {
  DeformationAsset asset; asset.combined();
  const auto loaded = gltf::LoadModelWithSource(asset.save("original.gltf").string(),{false});
  tinygltf::TinyGLTF writer;
  for (bool binary : {false,true}) {
    const auto path = asset.save(binary ? "rewritten.glb" : "rewritten.gltf",binary);
    ASSERT_TRUE(writer.WriteGltfSceneToFile(&loaded.gltfModel,path.string(),true,true,true,binary));
    const auto reloaded = gltf::LoadModelWithSource(path.string(),{false});
    ASSERT_TRUE(reloaded.deformation); ASSERT_EQ(reloaded.deformation->primitives.size(),1u);
    EXPECT_EQ(reloaded.deformation->primitives[0].meshDefaultWeights,(std::vector<float>{0.75f}));
    EXPECT_EQ(reloaded.deformation->nodes[0].defaultMorphWeights,(std::vector<float>{-0.25f}));
    ASSERT_EQ(reloaded.deformation->skins.size(),1u);
    EXPECT_FLOAT_EQ(reloaded.deformation->skins[0].inverseBindMatrices[0][3][1],-2);
    EXPECT_EQ(reloaded.model.indices(),loaded.model.indices());
    EXPECT_EQ(reloaded.deformation->primitives[0].sourceMesh,0u);
    EXPECT_EQ(reloaded.deformation->primitives[0].sourcePrimitive,0u);
  }
}

TEST(GltfDeformationLoader, MissingInverseBindAccessorUsesIdentityWithoutChangingSource) {
  DeformationAsset asset;
  asset.document["meshes"].push_back({{"primitives",Json::array({asset.triangle(0,3,1)})}});
  asset.addSkin(false);
  const auto loaded = gltf::LoadModelWithSource(asset.save("default-bind.gltf").string(),{false});
  ASSERT_TRUE(loaded.deformation); ASSERT_EQ(loaded.deformation->skins.size(),1u);
  EXPECT_EQ(loaded.deformation->skins[0].inverseBindMatrices[0],glm::mat4(1));
  EXPECT_EQ(loaded.gltfModel.skins[0].inverseBindMatrices,-1);
}

TEST(GltfDeformationLoader, RejectsInvalidSkinBindingsThroughPublicCpuLoad) {
  for (const char* mutation : {"node skin","joint index","duplicate joints","skeleton","missing mesh","missing weights"}) {
    SCOPED_TRACE(mutation); DeformationAsset asset; asset.combined();
    if (std::string_view(mutation) == "node skin") asset.document["nodes"][0]["skin"] = 99;
    else if (std::string_view(mutation) == "joint index") asset.document["skins"][0]["joints"] = {99};
    else if (std::string_view(mutation) == "duplicate joints") asset.document["skins"][0]["joints"] = {1,1};
    else if (std::string_view(mutation) == "skeleton") asset.document["skins"][0]["skeleton"] = 99;
    else if (std::string_view(mutation) == "missing mesh") asset.document["nodes"][0].erase("mesh");
    else asset.document["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
    expectRejected(asset.save("invalid-skin.gltf"),"deformation");
  }
}

TEST(GltfDeformationLoader, RejectsInvalidMorphCountsAndDefaultsBeforeReturningPayload) {
  for (const char* mutation : {"mesh defaults","node defaults","target count","primitive counts","nonfinite delta"}) {
    SCOPED_TRACE(mutation); DeformationAsset asset; asset.combined();
    auto& primitive = asset.document["meshes"][0]["primitives"][0];
    if (std::string_view(mutation) == "mesh defaults") asset.document["meshes"][0]["weights"] = {0.5,0.5};
    else if (std::string_view(mutation) == "node defaults") asset.document["nodes"][0]["weights"] = {0.5,0.5};
    else if (std::string_view(mutation) == "target count") {
      const int index = primitive["targets"][0]["POSITION"].get<int>();
      asset.document["accessors"][index]["count"] = 2;
    } else if (std::string_view(mutation) == "primitive counts") {
      auto extra = primitive; extra.erase("targets");
      asset.document["meshes"][0]["primitives"].push_back(extra);
    } else {
      const std::array<float,9> invalid{std::numeric_limits<float>::quiet_NaN(),0,0, 0,0,0, 0,0,0};
      primitive["targets"][0]["POSITION"] = asset.floats("VEC3",3,invalid);
    }
    expectRejected(asset.save("invalid-morph.gltf"),"deformation");
  }
}

TEST(GltfDeformationLoader, RejectsNonAffineInverseBindAndInvalidInfluenceAccessors) {
  for (const char* mutation : {"inverse bind","joint type","weight count","joint outside skin"}) {
    SCOPED_TRACE(mutation); DeformationAsset asset; asset.combined();
    const auto& attributes = asset.document["meshes"][0]["primitives"][0]["attributes"];
    if (std::string_view(mutation) == "inverse bind") {
      const std::array<float,16> invalid{1,0,0,1, 0,1,0,0, 0,0,1,0, 0,0,0,1};
      asset.document["skins"][0]["inverseBindMatrices"] = asset.floats("MAT4",16,invalid);
    } else if (std::string_view(mutation) == "joint type")
      asset.document["accessors"][attributes["JOINTS_0"].get<int>()]["componentType"] = 5120;
    else if (std::string_view(mutation) == "weight count")
      asset.document["accessors"][attributes["WEIGHTS_0"].get<int>()]["count"] = 2;
    else {
      const std::array<unsigned char,12> invalid{1,0,0,0, 1,0,0,0, 1,0,0,0};
      asset.document["meshes"][0]["primitives"][0]["attributes"]["JOINTS_0"] = asset.bytes("VEC4",4,invalid);
    }
    const auto path = asset.save("invalid-accessor.gltf");
    expectRejected(path,"deformation");
    EXPECT_THROW(gltf::LoadModelFromFile(path.string()),std::runtime_error);
  }
}

TEST(GltfDeformationLoader, BoundedLoadAccountsForDecodedMorphExpansionBeyondEncodedGeometry) {
  DeformationAsset asset;
  auto primitive = asset.triangle(0,3072);
  const int delta = asset.positionDelta(3072,0.25f);
  asset.document["meshes"].push_back({{"primitives",Json::array({primitive})}});
  constexpr uintmax_t budget = 1024*1024;
  // Both files retain the exact same buffers, including the delta accessor.
  // A target list can reference one encoded array many times while creating
  // independently decoded target arrays. Ordinary geometry fits this budget.
  const auto staticPath = asset.save("budget-static.gltf");
  ASSERT_LT(std::filesystem::file_size(staticPath)+asset.data.size(),budget);
  const auto ordinary = gltf::LoadModelWithSource(staticPath.string(),{false,budget});
  EXPECT_FALSE(ordinary.deformation); ASSERT_EQ(ordinary.model.vertices().size(),3072u);
  auto& mesh = asset.document["meshes"][0];
  mesh["primitives"][0]["targets"] = Json::array({Json{{"POSITION",delta}}});
  mesh["weights"] = {0};
  const auto singleTarget = gltf::LoadModelWithSource(asset.save("budget-one-target.gltf").string(),{false,budget});
  ASSERT_TRUE(singleTarget.deformation);
  ASSERT_EQ(singleTarget.deformation->primitives[0].morphTargets.size(),1u);
  mesh["primitives"][0]["targets"] = Json::array();
  mesh["weights"] = std::vector<float>(64,0);
  for (int target = 0; target < 64; ++target)
    mesh["primitives"][0]["targets"].push_back({{"POSITION",delta}});
  const auto path = asset.save("budget-morph.gltf");
  ASSERT_LT(std::filesystem::file_size(path)+asset.data.size(),budget);
  const auto unlimited = gltf::LoadModelWithSource(path.string(),{false});
  ASSERT_TRUE(unlimited.deformation);
  ASSERT_EQ(unlimited.deformation->primitives[0].morphTargets.size(),64u);
  EXPECT_EQ(unlimited.deformation->primitives[0].morphTargets[63].positions.size(),3072u);
  try {
    (void)gltf::LoadModelWithSource(path.string(),{false,budget});
    FAIL() << "Decoded morph arrays were omitted from the bounded-loader budget";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("input-memory budget"),std::string::npos) << error.what();
  }
}

TEST(GltfDeformationLoader, BoundedLoadAccountsForAllDecodedInfluenceSets) {
  DeformationAsset asset;
  asset.document["meshes"].push_back({{"primitives",Json::array({asset.triangle(0,3072,8)})}});
  constexpr uintmax_t budget = 1536*1024;
  const auto staticPath = asset.save("budget-unused-joints.gltf");
  ASSERT_LT(std::filesystem::file_size(staticPath)+asset.data.size(),budget);
  const auto ordinary = gltf::LoadModelWithSource(staticPath.string(),{false,budget});
  EXPECT_FALSE(ordinary.deformation);
  asset.addSkin(false);
  auto& attributes = asset.document["meshes"][0]["primitives"][0]["attributes"];
  const auto allAttributes = attributes;
  for (int set = 1; set < 8; ++set) {
    attributes.erase("JOINTS_"+std::to_string(set));
    attributes.erase("WEIGHTS_"+std::to_string(set));
  }
  // A one-set skin with the same encoded buffers fits, proving that rejection
  // below is caused by the expanded set population rather than bind retention
  // or the fixed per-vertex/per-skin overhead alone.
  const auto singleSet = gltf::LoadModelWithSource(asset.save("budget-one-set.gltf").string(),{false,budget});
  ASSERT_TRUE(singleSet.deformation);
  ASSERT_EQ(singleSet.deformation->primitives[0].influences[0].values.size(),4u);
  attributes = allAttributes;
  const auto path = asset.save("budget-influences.gltf");
  ASSERT_LT(std::filesystem::file_size(path)+asset.data.size(),budget);
  const auto unlimited = gltf::LoadModelWithSource(path.string(),{false});
  ASSERT_TRUE(unlimited.deformation);
  const auto& vertex = unlimited.deformation->primitives[0].influences[0];
  ASSERT_EQ(vertex.values.size(),32u);
  for (size_t set = 0; set < 8; ++set) EXPECT_FLOAT_EQ(vertex.values[set*4].weight,0.125f);
  try {
    (void)gltf::LoadModelWithSource(path.string(),{false,budget});
    FAIL() << "Decoded influence arrays were omitted from the bounded-loader budget";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("input-memory budget"),std::string::npos) << error.what();
  }
}
} // namespace
