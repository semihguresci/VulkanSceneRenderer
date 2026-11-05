#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfModelLoader.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
using namespace container::geometry::gltf;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
template<class T> void append(Bytes& bytes,T value) {
  using Bits = std::conditional_t<sizeof(T) == 8,uint64_t,
    std::conditional_t<sizeof(T) == 4,uint32_t,std::conditional_t<sizeof(T) == 2,uint16_t,uint8_t>>>;
  const auto bits = std::bit_cast<Bits>(value);
  for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(static_cast<unsigned char>(bits >> (i*8)));
}
template<class T> Bytes encoded(std::initializer_list<T> values) {
  Bytes result; for (T value : values) append(result,value); return result;
}
tinygltf::Model oneAccessor(int component,int type,size_t count,const Bytes& bytes) {
  tinygltf::Model model; model.buffers.resize(1); model.buffers[0].data = bytes;
  tinygltf::BufferView view; view.buffer = 0; view.byteLength = bytes.size(); model.bufferViews.push_back(view);
  tinygltf::Accessor accessor; accessor.bufferView = 0; accessor.componentType = component; accessor.type = type; accessor.count = count;
  model.accessors.push_back(accessor); return model;
}
void sparse(tinygltf::Model& model,const Bytes& indices,const Bytes& values,int indexType,size_t count) {
  auto add = [&](const Bytes& data) {
    auto& bytes = model.buffers[0].data;
    while (bytes.size()%8) bytes.push_back(0);
    tinygltf::BufferView view; view.buffer = 0; view.byteOffset = bytes.size(); view.byteLength = data.size();
    bytes.insert(bytes.end(),data.begin(),data.end()); model.bufferViews.push_back(view); return int(model.bufferViews.size()-1);
  };
  auto& accessor = model.accessors[0]; accessor.sparse.isSparse = true; accessor.sparse.count = int(count);
  accessor.sparse.indices.bufferView = add(indices); accessor.sparse.indices.componentType = indexType;
  accessor.sparse.indices.byteOffset = 0;
  accessor.sparse.values.bufferView = add(values); accessor.sparse.values.byteOffset = 0;
}
TEST(GltfAccessor, SparseOverridesHonorOffsetsAndNormalizeAfterRawReplacement) {
  auto model = oneAccessor(5122,TINYGLTF_TYPE_VEC2,3,encoded<int16_t>({-32768,32767, 0,0, 16384,-16384}));
  model.accessors[0].normalized = true;
  sparse(model,Bytes{99,1},encoded<int16_t>({77,77,32767,-32768}),5121,1);
  model.accessors[0].sparse.indices.byteOffset = 1;
  model.accessors[0].sparse.values.byteOffset = 4;
  const auto original = model.buffers[0].data;
  CheckedAccessor reader(model,0,"fixture"); std::array<double,2> value{};
  reader.readDoubles(0,value); EXPECT_EQ(value,(std::array<double,2>{-1,1}));
  reader.readDoubles(1,value); EXPECT_EQ(value,(std::array<double,2>{1,-1}));
  reader.readDoubles(2,value); EXPECT_NEAR(value[0],16384.0/32767,1e-14);
  EXPECT_EQ(model.buffers[0].data,original); EXPECT_EQ(model.accessors[0].componentType,5122);
}
TEST(GltfAccessor, SparseOnlyUsesZeroBaseAndExactUnsignedIndices) {
  auto model = oneAccessor(5125,TINYGLTF_TYPE_SCALAR,3,{}); model.accessors[0].bufferView = -1;
  sparse(model,encoded<uint16_t>({1,2}),encoded<uint32_t>({16777217,4294967294u}),5123,2);
  CheckedAccessor reader(model,0,"indices",{AccessorUsage::Indices}); std::array<uint32_t,1> value{};
  reader.readUnsigned(0,value); EXPECT_EQ(value[0],0u);
  reader.readUnsigned(1,value); EXPECT_EQ(value[0],16777217u);
  reader.readUnsigned(2,value); EXPECT_EQ(value[0],4294967294u);
  EXPECT_THROW(reader.readUnsigned(3,value),std::runtime_error);
  std::array<uint32_t,2> wrong{}; EXPECT_THROW(reader.readUnsigned(0,wrong),std::runtime_error);
}
TEST(GltfAccessor, BufferlessNonsparseAccessorsInitializeAllComponentsToZero) {
  for (const auto [component,type] : {std::pair{5125,TINYGLTF_TYPE_SCALAR},
       std::pair{5126,TINYGLTF_TYPE_VEC3},std::pair{5121,TINYGLTF_TYPE_MAT3}}) {
    auto model = oneAccessor(component,type,3,{});
    model.accessors[0].bufferView = -1;
    model.buffers.clear(); model.bufferViews.clear();
    CheckedAccessor reader(model,0,"zero base");
    std::array<float,16> values{}; values.fill(9);
    reader.readFloats(2,std::span(values).first(reader.components()));
    for (size_t i = 0; i < reader.components(); ++i) EXPECT_EQ(values[i],0);
    if (component == 5125) {
      std::array<uint32_t,1> index{99}; reader.readUnsigned(1,index); EXPECT_EQ(index[0],0u);
    }
    model.accessors[0].byteOffset = 4;
    EXPECT_THROW(CheckedAccessor(model,0,"invalid zero base"),std::runtime_error);
  }
}
TEST(GltfAccessor, PaddedMatricesUseColumnOffsetsAndAllowMissingFinalPadding) {
  for (const auto [type,component,rows] : {std::tuple{TINYGLTF_TYPE_MAT2,5121,2},
       std::tuple{TINYGLTF_TYPE_MAT3,5121,3},std::tuple{TINYGLTF_TYPE_MAT3,5123,3}}) {
    SCOPED_TRACE(std::to_string(type)+" / "+std::to_string(component));
    Bytes bytes; const size_t width = component == 5121 ? 1 : 2;
    const size_t columnStride = (size_t(rows)*width+3)/4*4;
    for (int matrix = 0; matrix < 2; ++matrix)
      for (int column = 0; column < rows; ++column) {
        for (int row = 0; row < rows; ++row) {
          const auto value = uint16_t(matrix*20+column*rows+row+1);
          if (width == 1) append(bytes,uint8_t(value)); else append(bytes,value);
        }
        if (matrix != 1 || column != rows-1)
          bytes.insert(bytes.end(),columnStride-size_t(rows)*width,0xee);
      }
    auto model = oneAccessor(component,type,2,bytes); CheckedAccessor reader(model,0,"matrix");
    std::array<double,16> values{}; reader.readDoubles(1,std::span(values).first(size_t(rows)*rows));
    for (int i = 0; i < rows*rows; ++i) EXPECT_EQ(values[size_t(i)],21+i);
    model.bufferViews[0].byteLength--; EXPECT_THROW(CheckedAccessor(model,0,"matrix"),std::runtime_error);
  }
}
TEST(GltfAccessor, SparseMatrixIndicesNeedOnlyTheirOwnComponentAlignment) {
  auto model = oneAccessor(5121,TINYGLTF_TYPE_MAT2,2,{}); model.accessors[0].bufferView = -1;
  sparse(model,Bytes{0xaa,1},Bytes{1,2,0xee,0xee,3,4},5121,1);
  model.accessors[0].sparse.indices.byteOffset = 1;
  CheckedAccessor reader(model,0,"matrix"); std::array<double,4> value{};
  reader.readDoubles(1,value); EXPECT_EQ(value,(std::array<double,4>{1,2,3,4}));
}
TEST(GltfAccessor, RejectsMalformedSparseLayoutsBeforeReadingOrAllocatingOutputs) {
  const auto valid = [] {
    auto model = oneAccessor(5126,TINYGLTF_TYPE_SCALAR,3,{}); model.accessors[0].bufferView = -1;
    sparse(model,Bytes{0,2},encoded<float>({1,2}),5121,2); return model;
  };
  auto model = valid(); model.buffers[0].data[model.bufferViews[1].byteOffset+1] = 0;
  EXPECT_THROW(CheckedAccessor(model,0,"duplicate"),std::runtime_error);
  model = valid(); model.buffers[0].data[model.bufferViews[1].byteOffset+1] = 3;
  EXPECT_THROW(CheckedAccessor(model,0,"range"),std::runtime_error);
  model = valid(); model.bufferViews[2].byteStride = 4; EXPECT_THROW(CheckedAccessor(model,0,"stride"),std::runtime_error);
  model = valid(); model.accessors[0].sparse.values.byteOffset = std::numeric_limits<size_t>::max(); EXPECT_THROW(CheckedAccessor(model,0,"offset"),std::runtime_error);
  model = valid(); model.accessors[0].sparse.count = 4; EXPECT_THROW(CheckedAccessor(model,0,"count"),std::runtime_error);
  model = valid(); model.accessors[0].bufferView = -2; EXPECT_THROW(CheckedAccessor(model,0,"view"),std::runtime_error);
  model = valid(); model.accessors[0].sparse.indices.componentType = 5126; EXPECT_THROW(CheckedAccessor(model,0,"index type"),std::runtime_error);
}
TEST(GltfAccessor, DenseOverflowAndVertexAlignmentHaveCheckedRanges) {
  auto model = oneAccessor(5126,TINYGLTF_TYPE_VEC3,1,encoded<float>({1,2,3}));
  model.accessors[0].count = std::numeric_limits<size_t>::max();
  EXPECT_THROW(CheckedAccessor(model,0,"overflow"),std::runtime_error);
  model.accessors[0].count = 1; model.accessors[0].byteOffset = std::numeric_limits<size_t>::max();
  EXPECT_THROW(CheckedAccessor(model,0,"offset"),std::runtime_error);
  model = oneAccessor(5121,TINYGLTF_TYPE_VEC3,2,Bytes{1,2,3,4,5,6});
  EXPECT_THROW(CheckedAccessor(model,0,"vertex",{AccessorUsage::VertexAttribute}),std::runtime_error);
  model.buffers[0].data = Bytes{1,2,3,99,4,5,6,99}; model.bufferViews[0].byteLength = 8; model.bufferViews[0].byteStride = 4;
  CheckedAccessor reader(model,0,"vertex",{AccessorUsage::VertexAttribute}); std::array<float,3> value{};
  reader.readFloats(1,value); EXPECT_EQ(value,(std::array<float,3>{4,5,6}));
  model.bufferViews[0].byteStride = 5; EXPECT_THROW(CheckedAccessor(model,0,"stride"),std::runtime_error);
}
TEST(GltfAccessor, Float64NeedsBothGatesAndConvertsOnlyRepresentableFiniteValues) {
  auto model = oneAccessor(5130,TINYGLTF_TYPE_SCALAR,1,encoded<double>({1.0000000001}));
  EXPECT_THROW(CheckedAccessor(model,0,"double",{AccessorUsage::Generic,true}),std::runtime_error);
  model.extensionsRequired = {"KHR_accessor_float64"};
  EXPECT_THROW(CheckedAccessor(model,0,"double"),std::runtime_error);
  CheckedAccessor reader(model,0,"double",{AccessorUsage::Generic,true}); std::array<double,1> precise{};
  reader.readDoubles(0,precise); EXPECT_DOUBLE_EQ(precise[0],1.0000000001);
  std::array<float,1> rounded{}; reader.readFloats(0,rounded); EXPECT_FLOAT_EQ(rounded[0],1);
  for (double value : {std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),
      double(std::numeric_limits<float>::max())*2,std::numeric_limits<double>::min()}) {
    model.buffers[0].data = encoded<double>({value});
    CheckedAccessor invalid(model,0,"double",{AccessorUsage::Generic,true,true});
    EXPECT_THROW(invalid.readFloats(0,rounded),std::runtime_error);
  }
  model.accessors[0].normalized = true;
  EXPECT_THROW(CheckedAccessor(model,0,"double",{AccessorUsage::Generic,true}),std::runtime_error);
}
TEST(GltfAccessor, FloatFrameRepairExceptionIsExplicitAndDoesNotAffectOtherConsumers) {
  auto model = oneAccessor(5126,TINYGLTF_TYPE_SCALAR,1,encoded<float>({std::numeric_limits<float>::quiet_NaN()}));
  CheckedAccessor strict(model,0,"strict"); std::array<float,1> value{};
  EXPECT_THROW(strict.readFloats(0,value),std::runtime_error);
  CheckedAccessor repair(model,0,"frame",{AccessorUsage::Generic,false,true});
  EXPECT_NO_THROW(repair.readFloats(0,value)); EXPECT_TRUE(std::isnan(value[0]));
}

struct FileFixture {
  Json json{{"asset",{{"version","2.0"}}},{"accessors",Json::array()},{"bufferViews",Json::array()},
    {"meshes",Json::array({{{"primitives",Json::array({{{"attributes",Json::object()},{"mode",4}}})}}})},
    {"nodes",Json::array({{{"mesh",0}}})},{"scenes",Json::array({{{"nodes",{0}}}})},{"scene",0}};
  Bytes bytes;
  std::filesystem::path directory;
  FileFixture() {
    static std::atomic<uint64_t> counter{0};
    directory = std::filesystem::temp_directory_path()/
      ("container_accessor_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"_"+std::to_string(counter++));
    std::filesystem::create_directories(directory);
  }
  ~FileFixture() {
    std::error_code ignored;
    for (const char* name : {"asset.gltf","asset.bin"}) std::filesystem::remove(directory/name,ignored);
    std::filesystem::remove(directory,ignored);
  }
  Json& primitive() { return json["meshes"][0]["primitives"][0]; }
  int view(const Bytes& data,size_t stride = 0) {
    while (bytes.size()%8) bytes.push_back(0);
    Json view{{"buffer",0},{"byteOffset",bytes.size()},{"byteLength",data.size()}};
    if (stride) view["byteStride"] = stride;
    bytes.insert(bytes.end(),data.begin(),data.end()); json["bufferViews"].push_back(view);
    return int(json["bufferViews"].size()-1);
  }
  int attribute(const char* semantic,int component,const char* type,size_t count,const Bytes& data,bool normalized = false,size_t stride = 0) {
    const int index = int(json["accessors"].size());
    Json accessor{{"bufferView",view(data,stride)},{"componentType",component},{"type",type},{"count",count}};
    if (normalized) accessor["normalized"] = true;
    if (std::string_view(semantic) == "POSITION") { accessor["min"] = {0,0,0}; accessor["max"] = {1,1,0}; }
    json["accessors"].push_back(accessor); primitive()["attributes"][semantic] = index; return index;
  }
  void positions() { attribute("POSITION",5126,"VEC3",3,encoded<float>({0,0,0,1,0,0,0,1,0})); }
  std::string save() {
    json["buffers"] = Json::array({{{"uri","asset.bin"},{"byteLength",bytes.size()}}});
    std::ofstream binary(directory/"asset.bin",std::ios::binary); binary.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); binary.close();
    std::ofstream text(directory/"asset.gltf"); text << json.dump(2); text.close(); return (directory/"asset.gltf").string();
  }
};
TEST(GltfAccessorLoader, SparseGeometryAndColorsPreserveAuthoredSourceAndAlpha) {
  for (bool zeroBase : {false,true}) {
    FileFixture fixture; fixture.positions();
    const int color = fixture.attribute("COLOR_0",5121,"VEC4",3,Bytes{255,0,0,0, 0,255,0,128, 0,0,255,255},true);
    auto& position = fixture.json["accessors"][0];
    position["max"] = {2,3,0};
    if (zeroBase) position.erase("bufferView");
    position["sparse"] = {{"count",2},{"indices",{{"bufferView",fixture.view(Bytes{9,1,2})},{"byteOffset",1},{"componentType",5121}}},
      {"values",{{"bufferView",fixture.view(encoded<float>({99,99,99,2,0,0,0,3,0}))},{"byteOffset",12}}}};
    const auto original = fixture.bytes;
    const auto loaded = LoadModelWithSource(fixture.save());
    ASSERT_EQ(loaded.model.vertices().size(),3u); EXPECT_FALSE(loaded.deformation);
    EXPECT_EQ(loaded.model.vertices()[1].position,glm::vec3(2,0,0));
    EXPECT_EQ(loaded.model.vertices()[2].position,glm::vec3(0,3,0));
    EXPECT_FLOAT_EQ(loaded.model.vertices()[0].colorAlpha,0);
    EXPECT_NEAR(loaded.model.vertices()[1].colorAlpha,128.f/255,1e-7f);
    EXPECT_FLOAT_EQ(loaded.model.vertices()[2].colorAlpha,1);
    EXPECT_EQ(loaded.gltfModel.buffers[0].data,original);
    EXPECT_EQ(loaded.gltfModel.accessors[0].sparse.indices.byteOffset,1u);
    EXPECT_EQ(loaded.gltfModel.accessors[0].bufferView,zeroBase ? -1 : 0);
    EXPECT_EQ(loaded.gltfModel.accessors[size_t(color)].componentType,5121);
  }
}
TEST(GltfAccessorLoader, QuantizedPositionsAndUvKeepAuthoredDequantizationTransforms) {
  FileFixture fixture;
  fixture.json["extensionsRequired"] = {"KHR_mesh_quantization"};
  fixture.json["extensionsUsed"] = {"KHR_mesh_quantization"};
  fixture.attribute("POSITION",5122,"VEC3",3,encoded<int16_t>({0,0,0,99, 32767,0,0,99, 0,32767,0,99}),true,8);
  fixture.json["accessors"][0]["max"] = {32767,32767,0};
  fixture.attribute("NORMAL",5120,"VEC3",3,Bytes{0,0,127,99, 0,0,127,99, 0,0,127,99},true,4);
  fixture.attribute("TEXCOORD_0",5122,"VEC2",3,encoded<int16_t>({-2,3,4,5,6,7}));
  fixture.json["nodes"][0]["scale"] = {2,3,4};
  fixture.json["nodes"][0]["translation"] = {10,20,30};
  const auto loaded = LoadModelWithSource(fixture.save());
  EXPECT_EQ(loaded.model.vertices()[1].position,glm::vec3(1,0,0));
  EXPECT_EQ(loaded.model.vertices()[2].texCoord,glm::vec2(6,7));
  EXPECT_EQ(loaded.gltfModel.nodes[0].scale,(std::vector<double>{2,3,4}));
  EXPECT_EQ(loaded.gltfModel.nodes[0].translation,(std::vector<double>{10,20,30}));
  fixture.json.erase("extensionsRequired"); EXPECT_THROW(LoadModelWithSource(fixture.save()),std::runtime_error);
}
TEST(GltfAccessorLoader, Float64GeometryKeepsSourceAndRejectsRuntimeRangeLoss) {
  FileFixture fixture; fixture.json["extensionsRequired"] = {"KHR_accessor_float64"};
  fixture.json["extensionsUsed"] = {"KHR_accessor_float64"};
  fixture.attribute("POSITION",5130,"VEC3",3,encoded<double>({0,0,0,1.0000000001,0,0,0,1,0}));
  fixture.json["accessors"][0]["max"] = {1.0000000001,1,0};
  const auto original = fixture.bytes;
  const auto loaded = LoadModelWithSource(fixture.save());
  EXPECT_FLOAT_EQ(loaded.model.vertices()[1].position.x,1);
  EXPECT_EQ(loaded.gltfModel.buffers[0].data,original); EXPECT_EQ(loaded.gltfModel.accessors[0].componentType,5130);
  for (double invalid : {double(std::numeric_limits<float>::max())*2,std::numeric_limits<double>::min(),std::numeric_limits<double>::infinity()}) {
    fixture.bytes = encoded<double>({0,0,0,invalid,0,0,0,1,0});
    EXPECT_THROW(LoadModelWithSource(fixture.save()),std::runtime_error);
  }
  fixture.bytes = original; fixture.json.erase("extensionsRequired");
  EXPECT_THROW(LoadModelWithSource(fixture.save()),std::runtime_error);
}
TEST(GltfAccessorLoader, QuantizedMorphAndDoubleSkinDataReachDeformationWithoutSourceRewriting) {
  FileFixture fixture; fixture.positions();
  fixture.json["extensionsRequired"] = {"KHR_mesh_quantization","KHR_accessor_float64"};
  fixture.json["extensionsUsed"] = {"KHR_mesh_quantization","KHR_accessor_float64"};
  fixture.attribute("JOINTS_0",5121,"VEC4",3,Bytes(12,0));
  fixture.attribute("WEIGHTS_0",5130,"VEC4",3,encoded<double>({.25,.75,0,0, .25,.75,0,0, .25,.75,0,0}));
  const int morph = int(fixture.json["accessors"].size());
  fixture.json["accessors"].push_back({{"bufferView",fixture.view(encoded<int16_t>({0,0,0,99,2,-3,4,99,0,0,0,99}),8)},
    {"componentType",5122},{"type","VEC3"},{"count",3},{"min",{0,-3,0}},{"max",{2,0,4}}});
  fixture.primitive()["targets"] = Json::array({{{"POSITION",morph}}});
  fixture.json["meshes"][0]["weights"] = {.5};
  const int inverse = int(fixture.json["accessors"].size());
  fixture.json["accessors"].push_back({{"bufferView",fixture.view(encoded<double>({1,0,0,0, 0,1,0,0, 0,0,1,0, 2,0,0,1}))},
    {"componentType",5130},{"type","MAT4"},{"count",1}});
  fixture.json["nodes"][0]["skin"] = 0;
  fixture.json["nodes"].push_back({{"translation",{3,0,0}}});
  fixture.json["scenes"][0]["nodes"] = {0,1};
  fixture.json["skins"] = Json::array({{{"joints",{1}},{"inverseBindMatrices",inverse}}});
  const auto original = fixture.bytes;
  const auto loaded = LoadModelWithSource(fixture.save()); ASSERT_TRUE(loaded.deformation);
  const auto& data = *loaded.deformation;
  ASSERT_EQ(data.primitives.size(),1u); EXPECT_EQ(data.primitives[0].morphTargets[0].positions[1],glm::vec3(2,-3,4));
  EXPECT_FLOAT_EQ(data.primitives[0].influences[0].values[0].weight,.25f);
  EXPECT_FLOAT_EQ(data.primitives[0].influences[0].values[1].weight,.75f);
  EXPECT_FLOAT_EQ(data.skins[0].inverseBindMatrices[0][3].x,2);
  glm::mat4 joint(1); joint[3].x = 3;
  const std::array<glm::mat4,2> world{glm::mat4(1),joint};
  const auto palette = ComputeSkinPalette(data.skins[0],glm::mat4(1),world);
  const auto vertices = EvaluatePrimitive(data.primitives[0],{},palette);
  EXPECT_EQ(vertices[1].position,glm::vec3(7,-1.5f,2));
  EXPECT_EQ(loaded.gltfModel.buffers[0].data,original);
  EXPECT_EQ(loaded.gltfModel.accessors[size_t(inverse)].componentType,5130);
  fixture.json["accessors"][size_t(morph)]["componentType"] = 5123;
  EXPECT_THROW(LoadModelWithSource(fixture.save()),std::runtime_error);
  fixture.json["accessors"][size_t(morph)]["componentType"] = 5122;
  fixture.primitive()["targets"] = Json::array({{{"TEXCOORD_0",morph}}});
  try { (void)LoadModelWithSource(fixture.save()); FAIL() << "Unsupported UV morph was ignored"; }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("unsupported morph target semantic"),std::string::npos); }
}
TEST(GltfAccessorLoader, SparseExpansionIsChargedToConfiguredInputBudget) {
  FileFixture fixture; fixture.positions();
  auto& position = fixture.json["accessors"][0]; position.erase("bufferView"); position["count"] = 999999;
  position["max"] = {1,2,3};
  position["sparse"] = {{"count",1},{"indices",{{"bufferView",fixture.view(Bytes{0})},{"componentType",5121}}},
    {"values",{{"bufferView",fixture.view(encoded<float>({1,2,3}))}}}};
  LoadOptions options; options.maxInputBytes = 1024*1024;
  try { (void)LoadModelWithSource(fixture.save(),options); FAIL() << "Sparse geometry expansion bypassed budget"; }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("budget"),std::string::npos); }
}
TEST(GltfAccessorLoader, SparseAnimationExpansionCountsIndependentDecodedTrackStorage) {
  FileFixture fixture; fixture.positions();
  constexpr size_t keys = 4096;
  Bytes times; for (size_t i = 0; i < keys; ++i) append(times,float(i));
  fixture.json["accessors"].push_back({{"bufferView",fixture.view(times)},{"componentType",5126},
    {"type","SCALAR"},{"count",keys},{"min",{0}},{"max",{keys-1}}});
  fixture.json["accessors"].push_back({{"componentType",5126},{"type","SCALAR"},{"count",keys},
    {"sparse",{{"count",1},{"indices",{{"bufferView",fixture.view(encoded<uint16_t>({keys-1}))},{"componentType",5123}}},
      {"values",{{"bufferView",fixture.view(encoded<float>({1}))}}}}}});
  fixture.json["materials"] = Json::array({Json::object()}); fixture.primitive()["material"] = 0;
  fixture.json["extensionsUsed"] = {"KHR_animation_pointer"};
  fixture.json["animations"] = Json::array({{{"samplers",Json::array({{{"input",1},{"output",2},{"interpolation","LINEAR"}}})},
    {"channels",Json::array({{{"sampler",0},{"target",{{"path","pointer"},
      {"extensions",{{"KHR_animation_pointer",{{"pointer","/materials/0/alphaCutoff"}}}}}}}}})}}});
  const auto loaded = LoadModelWithSource(fixture.save());
  // Verify the retained animation source through the independent accessor
  // contract; live animation evaluation belongs to its own integration suite.
  CheckedAccessor values(loaded.gltfModel,2,"sparse animation output");
  std::array<double,1> value{};values.readDoubles(keys-1,value);EXPECT_EQ(value[0],1);
  values.readDoubles(0,value);EXPECT_EQ(value[0],0);
  LoadOptions options; options.maxInputBytes = 64*1024;
  try { (void)LoadModelWithSource(fixture.save(),options); FAIL() << "Sparse track expansion bypassed budget"; }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("budget"),std::string::npos); }
}
TEST(GltfAccessorLoader, BufferlessNonsparseAttributesMorphsAndAnimationUseZeroDefaults) {
  FileFixture fixture; fixture.positions();
  const auto zeroAccessor = [&](const char* type,size_t count) {
    const int index = int(fixture.json["accessors"].size());
    fixture.json["accessors"].push_back(Json{{"componentType",5126},{"type",type},{"count",count}});
    return index;
  };
  fixture.primitive()["attributes"]["TEXCOORD_0"] = zeroAccessor("VEC2",3);
  fixture.primitive()["attributes"]["COLOR_0"] = zeroAccessor("VEC4",3);
  fixture.primitive()["targets"] = Json::array({Json{{"POSITION",zeroAccessor("VEC3",3)}}});
  fixture.json["nodes"][0]["weights"] = {.75};
  const int input = int(fixture.json["accessors"].size());
  fixture.json["accessors"].push_back(Json{{"bufferView",fixture.view(encoded<float>({0,1}))},
    {"componentType",5126},{"type","SCALAR"},{"count",2},{"min",{0}},{"max",{1}}});
  const int output = zeroAccessor("VEC3",2);
  const Json sampler{{"input",input},{"output",output}};
  const Json channel{{"sampler",0},{"target",{{"node",0},{"path","translation"}}}};
  fixture.json["animations"] = Json::array({Json{{"samplers",Json::array({sampler})},
    {"channels",Json::array({channel})}}});
  auto loaded = LoadModelWithSource(fixture.save());
  ASSERT_TRUE(loaded.deformation);
  const auto& primitive = loaded.deformation->primitives.front();
  const std::array<float,1> weights{.75f};
  const auto evaluated = EvaluatePrimitive(primitive,weights,{});
  for (size_t i = 0; i < evaluated.size(); ++i) {
    EXPECT_EQ(evaluated[i].position,loaded.model.vertices()[i].position);
    EXPECT_EQ(evaluated[i].texCoord,glm::vec2(0));
    EXPECT_EQ(evaluated[i].color,glm::vec3(0));
    EXPECT_FLOAT_EQ(evaluated[i].colorAlpha,0);
  }
  CheckedAccessor values(loaded.gltfModel,output,"bufferless animation output");
  for(size_t i=0;i<values.count();++i) {
    std::array<double,3> value{1,2,3};values.readDoubles(i,value);
    EXPECT_EQ(value,(std::array<double,3>{0,0,0}));
  }
  EXPECT_TRUE(loaded.gltfModel.nodes[0].translation.empty());
  EXPECT_EQ(loaded.gltfModel.accessors[size_t(output)].bufferView,-1);
  EXPECT_FALSE(loaded.gltfModel.accessors[size_t(output)].sparse.isSparse);
}
} // namespace
