#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfAccessor.h>
#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
namespace gltf = container::geometry::gltf;
using Json = nlohmann::json;
constexpr const char* kExtension = "EXT_mesh_gpu_instancing";
struct Fixture {
  tinygltf::Model source;
  Fixture() {
    source.extensionsUsed = {kExtension}; source.extensionsRequired = {kExtension};
    source.meshes.resize(1); source.meshes[0].primitives.resize(2);
    source.nodes.resize(2); source.nodes[0].children = {1}; source.nodes[1].mesh = 0;
    source.nodes[1].extensions[kExtension] = tinygltf::Value(tinygltf::Value::Object{
        {"attributes", tinygltf::Value(tinygltf::Value::Object{})}});
    source.buffers.emplace_back();
  }
  template<class T> int add(const char* semantic, int type, int component, std::vector<T> values,
                            bool normalized = false, size_t stride = 0, size_t prefix = 0) {
    const size_t components = tinygltf::GetNumComponentsInType(type);
    const size_t count = values.size() / components;
    auto& data = source.buffers[0].data;
    while (data.size()%8) data.push_back(0);
    tinygltf::BufferView view; view.buffer = 0; view.byteOffset = data.size(); view.byteStride = stride;
    const size_t element = sizeof(T) * components;
    const size_t step = stride ? stride : element;
    const size_t extent = prefix + (count-1) * step + element;
    data.resize(data.size() + extent);
    for (size_t i = 0; i < count; ++i)
      std::memcpy(data.data() + view.byteOffset + prefix + i * step, values.data() + i * components, element);
    view.byteLength = extent;
    source.bufferViews.push_back(view);
    tinygltf::Accessor accessor; accessor.bufferView = int(source.bufferViews.size()-1);
    accessor.byteOffset = prefix; accessor.count = count; accessor.type = type;
    accessor.componentType = component; accessor.normalized = normalized;
    source.accessors.push_back(accessor);
    const auto index = int(source.accessors.size()-1);
    auto attributes = source.nodes[1].extensions[kExtension].Get("attributes").Get<tinygltf::Value::Object>();
    attributes[semantic] = tinygltf::Value(index);
    source.nodes[1].extensions[kExtension] = tinygltf::Value(tinygltf::Value::Object{{"attributes",tinygltf::Value(attributes)}});
    return index;
  }
  int translations() { return add<float>("TRANSLATION",TINYGLTF_TYPE_VEC3,5126,{2,1,0, -1,0,0},false,16,4); }
  Json metadata() const {
    Json json{{"extensionsUsed",source.extensionsUsed},{"extensionsRequired",source.extensionsRequired},
              {"nodes",Json::array()},{"meshes",{{{"primitives",Json::array({Json::object(),Json::object()})}}}},
              {"accessors",Json::array()}};
    for (const auto& node : source.nodes) {
      Json value = Json::object();
      if (node.mesh >= 0) value["mesh"] = node.mesh;
      if (const auto found = node.extensions.find(kExtension); found != node.extensions.end()) {
        Json attrs = Json::object();
        for (const auto& [semantic,index] : found->second.Get("attributes").Get<tinygltf::Value::Object>()) attrs[semantic] = index.Get<int>();
        value["extensions"][kExtension]["attributes"] = std::move(attrs);
      }
      json["nodes"].push_back(std::move(value));
    }
    for (const auto& a : source.accessors) {
      const auto type = a.type == TINYGLTF_TYPE_SCALAR ? "SCALAR" : a.type == TINYGLTF_TYPE_VEC3 ? "VEC3" :
          a.type == TINYGLTF_TYPE_VEC4 ? "VEC4" : "VEC2";
      json["accessors"].push_back({{"type",type},{"componentType",a.componentType},{"count",a.count},{"normalized",a.normalized}});
    }
    return json;
  }
};
template<class F> void fails(F function, std::string_view detail) {
  try { function(); FAIL() << "Expected rejection"; }
  catch (const std::exception& e) { EXPECT_NE(std::string(e.what()).find(detail),std::string::npos) << e.what(); }
}
void near(const glm::vec4& value,const glm::vec4& expected) {
  for (int i=0;i<4;++i) EXPECT_NEAR(value[i],expected[i],1e-5);
}
} // namespace

TEST(GltfInstancing, TranslationDefaultsShareOneMatrixPerInstanceAcrossPrimitives) {
  Fixture f; const auto index = f.translations();
  const auto before = f.source.buffers[0].data;
  const auto data = gltf::ParseMeshGpuInstancing(f.source);
  ASSERT_EQ(data.nodes.size(),1u); ASSERT_EQ(data.nodes[0].localMatrices.size(),2u);
  EXPECT_EQ(data.summary.instances,2u); EXPECT_EQ(data.summary.expandedPrimitives,4u);
  EXPECT_EQ(data.summary.instancedNodes,1u); EXPECT_GT(data.summary.decodedBytes,128u);
  EXPECT_EQ(data.nodes[0].attributes[0].sourceAccessor,index);
  near(data.nodes[0].localMatrices[0]*glm::vec4(1,2,3,1),{3,3,3,1});
  near(data.nodes[0].localMatrices[1]*glm::vec4(1,2,3,1),{0,2,3,1});
  EXPECT_EQ(data.nodes[0].identity(1,1),(gltf::SourceInstanceIdentity{1,0,1,1}));
  EXPECT_EQ(gltf::FindNodeInstancing(data,0),nullptr);
  EXPECT_EQ(gltf::FindNodeInstancing(data,1),&data.nodes[0]);
  EXPECT_EQ(f.source.buffers[0].data,before); EXPECT_EQ(f.source.meshes[0].primitives.size(),2u);
  const auto summary = gltf::ValidateMeshGpuInstancingJson(f.metadata());
  EXPECT_EQ(summary.instances,data.summary.instances); EXPECT_EQ(summary.expandedPrimitives,data.summary.expandedPrimitives);
  EXPECT_EQ(summary.decodedBytes,data.summary.decodedBytes);
}
TEST(GltfInstancing, CustomOnlyAttributesEstablishIdentityInstancesWithoutFloatIdConversion) {
  Fixture f;
  const auto accessor = f.add<uint32_t>("_ID",TINYGLTF_TYPE_SCALAR,5125,{16'777'217u,0xffffffffu});
  const auto data = gltf::ParseMeshGpuInstancing(f.source);
  for (const auto& matrix : data.nodes[0].localMatrices) near(matrix*glm::vec4(1,2,3,1),{1,2,3,1});
  EXPECT_EQ(data.nodes[0].attributes[0].sourceAccessor,accessor);
  gltf::CheckedAccessor ids(f.source,accessor,"original IDs"); std::array<uint32_t,1> raw{};
  ids.readUnsigned(0,raw); EXPECT_EQ(raw[0],16'777'217u);
  ids.readUnsigned(1,raw); EXPECT_EQ(raw[0],0xffffffffu);
}
TEST(GltfInstancing, NormalizedSignedRotationPreservesPrecisionAndQuaternionSign) {
  for (bool wide : {false,true}) {
    Fixture f;
    if (wide) f.add<int16_t>("ROTATION",TINYGLTF_TYPE_VEC4,5122,{0,0,23170,23170, 0,0,0,-32768},true);
    else f.add<int8_t>("ROTATION",TINYGLTF_TYPE_VEC4,5120,{0,0,90,90, 0,0,0,-128},true);
    const auto data = gltf::ParseMeshGpuInstancing(f.source);
    near(data.nodes[0].localMatrices[0]*glm::vec4(1,0,0,1),{0,1,0,1});
    near(data.nodes[0].localMatrices[1]*glm::vec4(1,0,0,1),{1,0,0,1});
  }
}
TEST(GltfInstancing, NestedMirroredWorldCompositionMatchesIndependentRepeatedNodeArithmetic) {
  Fixture f; f.translations();
  f.add<float>("ROTATION",TINYGLTF_TYPE_VEC4,5126,{0,0,.707106781f,.707106781f, 0,0,0,1});
  f.add<float>("SCALE",TINYGLTF_TYPE_VEC3,5126,{1,-1,1, 1,1,1});
  const auto data = gltf::ParseMeshGpuInstancing(f.source);
  const auto root = glm::translate(glm::mat4(1),glm::vec3(5,0,0)) *
      glm::rotate(glm::mat4(1),glm::radians(90.f),glm::vec3(0,0,1)) * glm::scale(glm::mat4(1),glm::vec3(-2,3,1));
  const auto nodeWorld = root * glm::translate(glm::mat4(1),glm::vec3(1,2,0));
  const auto first = gltf::ComposeInstanceWorld(data.nodes[0],0,nodeWorld);
  const auto second = gltf::ComposeInstanceWorld(data.nodes[0],1,nodeWorld);
  // Instance S/R/T first, then child translation, then parent S/R/T.
  near(first*glm::vec4(1,2,0,1),{-7,-10,0,1});
  near(second*glm::vec4(1,2,0,1),{-7,-2,0,1});
  EXPECT_GT(glm::determinant(glm::mat3(first)),0.f); EXPECT_LT(glm::determinant(glm::mat3(second)),0.f);
}
TEST(GltfInstancing, SparseAndBufferlessAttributesUseCheckedAccessorValues) {
  Fixture f; const auto index = f.translations();
  auto& a = f.source.accessors[index]; a.bufferView = -1; a.byteOffset = 0;
  a.sparse.isSparse = true; a.sparse.count = 1;
  auto& bytes = f.source.buffers[0].data;
  while (bytes.size()%8) bytes.push_back(0);
  tinygltf::BufferView iv; iv.buffer = 0; iv.byteOffset = bytes.size(); iv.byteLength = 1; bytes.push_back(1);
  f.source.bufferViews.push_back(iv); a.sparse.indices.bufferView = int(f.source.bufferViews.size()-1); a.sparse.indices.componentType = 5121; a.sparse.indices.byteOffset = 0;
  while (bytes.size()%8) bytes.push_back(0);
  tinygltf::BufferView vv; vv.buffer = 0; vv.byteOffset = bytes.size(); vv.byteLength = 12;
  const std::array<float,3> replacement{2,3,4}; bytes.resize(bytes.size()+12); std::memcpy(bytes.data()+vv.byteOffset,replacement.data(),12);
  f.source.bufferViews.push_back(vv); a.sparse.values.bufferView = int(f.source.bufferViews.size()-1); a.sparse.values.byteOffset = 0;
  const auto data = gltf::ParseMeshGpuInstancing(f.source);
  near(data.nodes[0].localMatrices[0]*glm::vec4(0,0,0,1),{0,0,0,1});
  near(data.nodes[0].localMatrices[1]*glm::vec4(0,0,0,1),{2,3,4,1});
}
TEST(GltfInstancing, InvalidCountsShapesNormalizationAndQuaternionValuesHaveSourceContext) {
  Fixture f; const auto translation = f.translations();
  const auto custom = f.add<uint8_t>("_ID",TINYGLTF_TYPE_SCALAR,5121,{0,1});
  f.source.accessors[custom].count = 3;
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source);},"node[1]._ID");
  f.source.accessors[custom].count = 2; f.source.accessors[translation].componentType = 5122;
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source);},"FLOAT VEC3");
  f.source.accessors[translation].componentType = 5126; f.source.accessors[translation].type = TINYGLTF_TYPE_VEC2;
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source);},"node[1].TRANSLATION");
  for (const auto q : {0.f,2.f}) {
    Fixture bad; bad.add<float>("ROTATION",TINYGLTF_TYPE_VEC4,5126,{0,0,0,q});
    fails([&]{(void)gltf::ParseMeshGpuInstancing(bad.source);},"node[1].ROTATION instance[0]");
  }
  Fixture bad; bad.add<uint8_t>("ROTATION",TINYGLTF_TYPE_VEC4,5121,{0,0,0,255},true);
  fails([&]{(void)gltf::ParseMeshGpuInstancing(bad.source);},"signed BYTE/SHORT");
}
TEST(GltfInstancing, NonfiniteBuiltinAndCustomDataFailWhileZeroScaleRemainsValid) {
  for (const auto* semantic : {"TRANSLATION","SCALE","_VECTOR"}) {
    Fixture bad; bad.add<float>(semantic,TINYGLTF_TYPE_VEC3,5126,{std::numeric_limits<float>::infinity(),0,0});
    fails([&]{(void)gltf::ParseMeshGpuInstancing(bad.source);},"node[1]."+std::string(semantic));
  }
  Fixture f; f.add<float>("SCALE",TINYGLTF_TYPE_VEC3,5126,{-1,0,2});
  const auto data = gltf::ParseMeshGpuInstancing(f.source);
  near(data.nodes[0].localMatrices[0]*glm::vec4(1,2,3,1),{-1,0,6,1});
}
TEST(GltfInstancing, JsonGateRejectsNarrowingAliasesAndMalformedAttributesBeforeBufferReads) {
  Fixture f; f.translations();
  for (const auto& index : {Json(uint64_t{4294967296}),Json(-1),Json(.5),Json("0")}) {
    auto json = f.metadata(); json["nodes"][1]["extensions"][kExtension]["attributes"]["TRANSLATION"] = index;
    fails([&]{(void)gltf::ValidateMeshGpuInstancingJson(json);},"node[1].TRANSLATION");
  }
  auto json = f.metadata(); json["nodes"][1].erase("mesh");
  fails([&]{(void)gltf::ValidateMeshGpuInstancingJson(json);},"node[1]");
  json = f.metadata(); json["nodes"][1]["extensions"][kExtension]["attributes"] = Json::object();
  fails([&]{(void)gltf::ValidateMeshGpuInstancingJson(json);},"positive instance count");
  json = f.metadata(); json["nodes"][1]["extensions"][kExtension]["attributes"]["POSITION"] = 0;
  fails([&]{(void)gltf::ValidateMeshGpuInstancingJson(json);},"underscore");
  f.source.extensionsUsed.clear();
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source);},"extensionsUsed");
}
TEST(GltfInstancing, AggregateInstanceExpansionAndByteBudgetsAreCheckedBeforeDenseOutput) {
  Fixture f; const auto index = f.translations(); f.source.nodes.push_back(f.source.nodes[1]);
  gltf::InstancingLimits limits; limits.maxInstances = 3;
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source,limits);},"aggregate instance count");
  limits = {}; limits.maxExpandedPrimitives = 7;
  fails([&]{(void)gltf::ValidateMeshGpuInstancingJson(f.metadata(),limits);},"expanded primitive count");
  limits = {}; limits.maxDecodedBytes = 64;
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source,limits);},"byte budget");
  f.source.accessors[index].count = SIZE_MAX;
  fails([&]{(void)gltf::ParseMeshGpuInstancing(f.source);},"instance limit");
  limits = {}; limits.maxInstances = 0;
  EXPECT_THROW((void)gltf::ParseMeshGpuInstancing(f.source,limits),std::runtime_error);
}
TEST(GltfInstancing, WorldAndIdentityHelpersRejectInvalidIndicesNonAffineAndOverflow) {
  Fixture f; f.add<float>("SCALE",TINYGLTF_TYPE_VEC3,5126,{2,2,2});
  auto data = gltf::ParseMeshGpuInstancing(f.source);
  EXPECT_THROW((void)data.nodes[0].identity(1,0),std::runtime_error);
  EXPECT_THROW((void)data.nodes[0].identity(0,2),std::runtime_error);
  EXPECT_THROW((void)gltf::ComposeInstanceWorld(data.nodes[0],1,glm::mat4(1)),std::runtime_error);
  auto matrix = glm::mat4(1); matrix[0][3] = .5f;
  EXPECT_THROW((void)gltf::ComposeInstanceWorld(data.nodes[0],0,matrix),std::runtime_error);
  matrix = glm::scale(glm::mat4(1),glm::vec3(std::numeric_limits<float>::max()));
  EXPECT_THROW((void)gltf::ComposeInstanceWorld(data.nodes[0],0,matrix),std::runtime_error);
}
