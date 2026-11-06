#include "Container/geometry/OpenVdbPointAttributesJson.h"
#include "Container/geometry/OpenVdbPoints.h"
#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <nlohmann/json.hpp>
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointGroup.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPointAttributesJsonAvailability, DisabledBeforeOptionsAndAllFileAccess) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  for(int mode:{0,1,2,3}) {
    try {
      vdb::ProcessingOptions p;p.maxInputBytes=0;
      if(mode==0)(void)vdb::ApplyPointAttributeEditsFile("missing","missing","missing/out",{},p);
      if(mode==1)(void)vdb::ExportPointAttributeSchema("missing","missing/out",{},p);
      if(mode==2)vdb::ExportPointAttribute("missing","","missing/out",{},p);
      if(mode==3)vdb::ExportPointGroup("missing","","missing/out",{},p);
      FAIL();
    } catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using Json=nlohmann::json;
class OpenVdbPointAttributesJson:public ::testing::Test {
 protected:
  void SetUp()override {
    static std::atomic_uint64_t serial{0};directory=std::filesystem::temp_directory_path()/("container-point-json-tests-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory);p.gridName="edited";p.voxelSize=1;
    const std::array<vdb::PointRecord,3> records{{{{32,0,0},{3,0,0},3},{{-16,0,0},{1,0,0},1},{{.1,0,0},{2,0,0},2}}};
    auto initial=p;initial.gridName="points";(void)vdb::WritePoints(records,path("source.vdb"),initial);
    openvdb::io::File input(path("source.vdb").string());input.open(false);
    auto grid=openvdb::gridPtrCast<openvdb::points::PointDataGrid>(input.readGrid("points"));input.close();
    grid->tree().setActiveState({0,0,0},false);grid->insertMeta("artist",openvdb::StringMetadata("unchanged"));
    {openvdb::io::File output(path("source.vdb").string());output.write({grid});}
  }
  void TearDown()override {std::error_code ignored;for(const auto& entry:std::filesystem::directory_iterator(directory))std::filesystem::remove(entry.path(),ignored);std::filesystem::remove(directory,ignored);}
  std::filesystem::path path(const std::string& name)const{return directory/name;}
  static std::string bytes(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
  void text(const std::string& name,const std::string& value){std::ofstream stream(path(name),std::ios::binary);stream.write(value.data(),value.size());}
  Json read(const std::string& name){std::ifstream stream(path(name));return Json::parse(stream);}
  void apply(const std::string& value,const std::string& output="edited.vdb",const vdb::PointAttributeOptions& o={}) {
    text("edits.json",value);(void)vdb::ApplyPointAttributeEditsFile(path("source.vdb"),path("edits.json"),path(output),{},p,o);
  }
  void reject(const std::string& value,const vdb::PointAttributeOptions& o={}) {
    const auto output="failure-"+std::to_string(serial++)+".vdb";text("edits.json",value);
    EXPECT_THROW((void)vdb::ApplyPointAttributeEditsFile(path("source.vdb"),path("edits.json"),path(output),{},p,o),std::exception)<<value;
    EXPECT_FALSE(std::filesystem::exists(path(output)));
  }
  std::filesystem::path directory;vdb::ProcessingOptions p;int serial=0;
};
TEST_F(OpenVdbPointAttributesJson, AllEightTypesExactIntegerUtf8NulAndUnorderedMembersRoundTrip) {
  const auto source=bytes(path("source.vdb"));
  const std::string document=R"({"groups":[{"values":[true,false,true],"name":"selected","op":"add"}],"attributes":[
    {"values":[false,true,false],"type":"bool","name":"flag","op":"add"},
    {"op":"add","name":"i32","type":"int32","values":[-2147483648,0,2147483647]},
    {"op":"add","name":"i64","type":"int64","values":[-9223372036854775808,9007199254740993,9223372036854775807]},
    {"op":"add","name":"f32","type":"float","values":[1.25]},
    {"op":"add","name":"f64","type":"double","values":[1e-300,-1e300,-0.0]},
    {"op":"add","name":"v32","type":"vec3f","values":[[1,2,3]]},
    {"op":"add","name":"v64","type":"vec3d","values":[[1e-200,-1e200,3]]},
    {"op":"add","name":"label","type":"string","values":["blue","\u84dd","a\u0000b"]}],"version":1})";
  apply(document);const auto edits=bytes(path("edits.json"));
  vdb::ExportPointAttribute(path("edited.vdb"),"i64",path("integers.json"));
  const auto integers=read("integers.json");EXPECT_EQ(integers["type"],"int64");
  EXPECT_EQ(integers["values"][0].get<int64_t>(),INT64_MIN);EXPECT_EQ(integers["values"][1].get<int64_t>(),9007199254740993ll);
  EXPECT_EQ(integers["values"][2].get<int64_t>(),INT64_MAX);
  EXPECT_NE(bytes(path("integers.json")).find("9007199254740993"),std::string::npos);
  for(const auto* name:{"flag","i32","f32","f64","v32","v64","label"})vdb::ExportPointAttribute(path("edited.vdb"),name,path(std::string(name)+".json"));
  EXPECT_TRUE(read("flag.json")["values"][0].is_boolean());EXPECT_EQ(read("i32.json")["values"][0].get<int32_t>(),INT32_MIN);
  EXPECT_EQ(read("f32.json")["values"],Json::array({1.25,1.25,1.25}));
  EXPECT_TRUE(std::signbit(read("f64.json")["values"][2].get<double>()));
  EXPECT_EQ(read("label.json")["values"][1].get<std::string>(),std::string("\xe8\x93\x9d"));
  EXPECT_EQ(read("label.json")["values"][2].get<std::string>(),std::string("a\0b",3));
  EXPECT_EQ(read("v64.json")["values"][0],Json::array({1e-200,-1e200,3.0}));
  vdb::ExportPointGroup(path("edited.vdb"),"selected",path("group.json"));
  const auto group=read("group.json");EXPECT_EQ(group["values"],Json::array({true,false,true}));EXPECT_TRUE(group["values"][0].is_boolean());
  const auto schema=vdb::ExportPointAttributeSchema(path("edited.vdb"),path("schema.json"));
  EXPECT_EQ(schema.pointCount,3u);const auto json=read("schema.json");EXPECT_EQ(json["point_count"],3u);EXPECT_EQ(json["grid_name"],"edited");
  EXPECT_EQ(json["traversal"],"leaf_origin_xyz_then_local_index");EXPECT_EQ(group["traversal"],json["traversal"]);
  EXPECT_EQ(bytes(path("source.vdb")),source);EXPECT_EQ(bytes(path("edits.json")),edits);
}
TEST_F(OpenVdbPointAttributesJson, NativeOrderInactivePointsBroadcastAndSequentialEdits) {
  apply(R"({"version":1,"attributes":[{"op":"add","name":"temporary","type":"int64","values":[10,20,30]},
    {"op":"rename","name":"temporary","new_name":"id"},{"op":"set","name":"pscale","type":"double","values":[2.5]}],
    "groups":[{"op":"add","name":"temporary","values":[true]},{"op":"rename","name":"temporary","new_name":"all"},
    {"op":"set","name":"all","values":[false,true,false]},{"op":"add","name":"drop","values":[false]},{"op":"drop","name":"drop"}]})");
  const auto values=vdb::ReadPointAttribute(path("edited.vdb"),"id");EXPECT_EQ(std::get<std::vector<int64_t>>(values.values),(std::vector<int64_t>{10,20,30}));
  EXPECT_EQ(vdb::ReadPointGroup(path("edited.vdb"),"all"),(std::vector<uint8_t>{0,1,0}));
  EXPECT_EQ(std::get<std::vector<double>>(vdb::ReadPointAttribute(path("edited.vdb"),"pscale").values),(std::vector<double>{2.5,2.5,2.5}));
  vdb::ExportPointAttribute(path("source.vdb"),"pscale",path("native-order.json"));EXPECT_EQ(read("native-order.json")["values"],Json::array({1.0,2.0,3.0}));
  text("drop.json",R"({"version":1,"attributes":[{"op":"drop","name":"id"}]})");
  (void)vdb::ApplyPointAttributeEditsFile(path("edited.vdb"),path("drop.json"),path("dropped.vdb"),{},p);
  EXPECT_THROW((void)vdb::ReadPointAttribute(path("dropped.vdb"),"id"),std::invalid_argument);
}
TEST_F(OpenVdbPointAttributesJson, StrictRootDuplicateUnknownAndOperationSchemasReject) {
  for(const auto* value:{"[]","{}","null",R"({"version":2})",R"({"version":1.0})",R"({"version":true})",
    R"({"version":1,"version":1})",R"({"version":1,"unknown":0})",R"({"version":1,"attributes":{}})",
    R"({"version":1,"attributes":[{"op":"drop","name":"v","name":"pscale"}]})",
    R"({"version":1,"attributes":[{"op":"add","name":"x","type":"double","values":[1],"typo":0}]})",
    R"({"version":1,"attributes":[{"op":"rename","name":"v"}]})",
    R"({"version":1,"attributes":[{"op":"drop","name":"v","values":[]}]})",
    R"({"version":1,"attributes":[{"op":"set","name":"pscale","values":[1]}]})",
    R"({"version":1,"groups":[{"op":"add","name":"g","type":"bool","values":[true]}]})",
    R"({"version":1,"groups":[{"op":"add","name":"g","values":[1]}]})",
    R"({"version":1,"attributes":[{"op":"add","name":"x","type":"float64","values":[1]}]})",
    R"({"version":1,"attributes":[{"op":"add","name":"x","type":"vec3d","values":[[1,2]]}]})",
    R"({"version":1,"attributes":[{"op":"add","name":"x","type":"vec3d","values":[[1,2,3,4]]}]})",
    R"({"version":1,"attributes":[{"op":"add","name":"x","type":"double","values":[{}]}]})",
    R"({"version":1,"attributes":[{"op":"add","name":"x","type":"double","values":[null]}]})",
    R"({"version":1,"groups":[{"op":"set","name":"g","values":[[true]]}]})",
    R"({"version":1} trailing)"})reject(value);
}
TEST_F(OpenVdbPointAttributesJson, ExactIntegerTypeAndNativeFloatingRangeGuards) {
  for(const auto& [type,number]:{std::pair{"int64","9223372036854775808"},std::pair{"int64","-9223372036854775809"},
    std::pair{"int64","1.0"},std::pair{"int64","1e0"},std::pair{"int32","2147483648"},std::pair{"int32","-2147483649"},
    std::pair{"float","1e40"},std::pair{"float","1e-46"},std::pair{"double","1e400"},std::pair{"double","1e-999"}})
    reject(std::string("{\"version\":1,\"attributes\":[{\"op\":\"add\",\"name\":\"x\",\"type\":\"")+type+"\",\"values\":["+number+"]}]}");
  reject("{\"version\":1,\"attributes\":[{\"op\":\"add\",\"name\":\"x\",\"type\":\"double\",\"values\":[0."+std::string(130,'0')+"1]}]}");
  apply(R"({"version":1,"attributes":[{"op":"add","name":"tiny","type":"double","values":[4.9406564584124654e-324]}]})");
  EXPECT_EQ(std::get<std::vector<double>>(vdb::ReadPointAttribute(path("edited.vdb"),"tiny").values)[0],std::numeric_limits<double>::denorm_min());
}
TEST_F(OpenVdbPointAttributesJson, IntegerToFloatRoundsDirectlyWithoutDoubleRounding) {
  apply(R"({"version":1,"attributes":[{"op":"add","name":"scalar","type":"float","values":[1152921573326323713]},
    {"op":"add","name":"vector","type":"vec3f","values":[[1152921573326323713,-1152921573326323713,1152921573326323712]]}]})");
  const auto scalar=std::get<std::vector<float>>(vdb::ReadPointAttribute(path("edited.vdb"),"scalar").values);
  for(const auto value:scalar) EXPECT_EQ(std::bit_cast<uint32_t>(value),0x5d800001u);
  const auto vectors=std::get<std::vector<std::array<float,3>>>(vdb::ReadPointAttribute(path("edited.vdb"),"vector").values);
  for(const auto& value:vectors) {
    EXPECT_EQ(std::bit_cast<uint32_t>(value[0]),0x5d800001u);
    EXPECT_EQ(std::bit_cast<uint32_t>(value[1]),0xdd800001u);
    EXPECT_EQ(std::bit_cast<uint32_t>(value[2]),0x5d800000u);
  }
}
TEST_F(OpenVdbPointAttributesJson, PositionProtectionAndTransactionFailureDoNotPublish) {
  reject(R"({"version":1,"attributes":[{"op":"add","name":"valid","type":"double","values":[1]},{"op":"set","name":"P","type":"vec3f","values":[[0,0,0]]}]})");
  reject(R"({"version":1,"attributes":[{"op":"rename","name":"v","new_name":"P"}]})");
  reject(R"({"version":1,"attributes":[{"op":"drop","name":"P"}]})");
  reject(R"({"version":1,"attributes":[{"op":"add","name":"x","type":"double","values":[1,2]}]})");
  reject(R"({"version":1,"attributes":[{"op":"add","name":"a\u0000b","type":"double","values":[1]}]})");
}
TEST_F(OpenVdbPointAttributesJson, StringPointEditAndSharedParserNativeBudgetsAreEnforced) {
  auto limits=vdb::PointAttributeOptions{};limits.maxPoints=2;
  reject(R"({"version":1,"attributes":[{"op":"add","name":"x","type":"double","values":[1,2,3]}]})",limits);
  limits={};limits.maxEdits=1;reject(R"({"version":1,"groups":[{"op":"add","name":"a","values":[true]},{"op":"drop","name":"a"}]})",limits);
  limits={};limits.maxStringBytes=3;reject(R"({"version":1,"attributes":[{"op":"add","name":"label","type":"string","values":["abcd"]}]})",limits);
  const auto before=bytes(path("source.vdb"));const std::string document="{\"version\":1,\"attributes\":[{\"op\":\"add\",\"name\":\"x\",\"type\":\"double\",\"values\":["+std::string(3*1024*1024,' ') +"1]}]}";
  text("large.json",document);auto small=p;small.maxInputBytes=1024*1024;
  EXPECT_THROW((void)vdb::ApplyPointAttributeEditsFile(path("source.vdb"),path("large.json"),path("encoded.vdb"),{},small),std::invalid_argument);
  // Native no-edit copy fits this budget alone; retained encoded JSON reserve
  // reduces the same budget rather than giving the native call a second cap.
  small.maxInputBytes=16*1024*1024;(void)vdb::WritePointAttributeEdits(path("source.vdb"),path("native-copy.vdb"),{}, {},{},small);
  const std::string padded="{\"version\":1}"+std::string(4*1024*1024,' ');text("padded.json",padded);
  EXPECT_THROW((void)vdb::ApplyPointAttributeEditsFile(path("source.vdb"),path("padded.json"),path("combined.vdb"),{},small),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("encoded.vdb")));EXPECT_FALSE(std::filesystem::exists(path("combined.vdb")));EXPECT_EQ(bytes(path("source.vdb")),before);
}
TEST_F(OpenVdbPointAttributesJson, InvalidUtf8RejectsWithoutReplacingNativeBytesOrLeavingPartialJson) {
  const std::string invalid("\xc0\xaf",2);
  reject(std::string("{\"version\":1,\"attributes\":[{\"op\":\"add\",\"name\":\"label\",\"type\":\"string\",\"values\":[\"")+invalid+"\"]}]}");
  const std::array<vdb::PointAttributeEdit,1> edits{{{vdb::PointEditOperation::Add,"label",{},std::vector<std::string>{invalid}}}};
  (void)vdb::WritePointAttributeEdits(path("source.vdb"),path("raw.vdb"),edits,{}, {},p);
  const auto original=bytes(path("raw.vdb"));
  try{vdb::ExportPointAttribute(path("raw.vdb"),"label",path("invalid.json"));FAIL();}
  catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("invalid UTF-8"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("invalid.json")));EXPECT_EQ(bytes(path("raw.vdb")),original);
}
TEST_F(OpenVdbPointAttributesJson, OutputProtectionAndEncodedExportLimitCleanUp) {
  text("protected.json","preserve");text("edits.json",R"({"version":1})");
  EXPECT_THROW((void)vdb::ExportPointAttributeSchema(path("source.vdb"),path("protected.json")),std::invalid_argument);
  EXPECT_THROW(vdb::ExportPointAttribute(path("source.vdb"),"v",path("protected.json")),std::invalid_argument);
  EXPECT_THROW(vdb::ExportPointGroup(path("source.vdb"),"missing",path("protected.json")),std::invalid_argument);
  EXPECT_THROW((void)vdb::ApplyPointAttributeEditsFile(path("source.vdb"),path("edits.json"),path("source.vdb")),std::invalid_argument);
  EXPECT_THROW((void)vdb::ExportPointAttributeSchema(path("source.vdb"),path("missing/out.json")),std::invalid_argument);
  EXPECT_EQ(bytes(path("protected.json")),"preserve");
  auto small=p;small.maxInputBytes=1;
  EXPECT_THROW(vdb::ExportPointAttribute(path("source.vdb"),"v",path("too-small.json"),{},small),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("too-small.json")));
}
TEST_F(OpenVdbPointAttributesJson, EscapedOutputIsBoundedWhileStreamingAndOwnedPartialIsRemoved) {
  // Many duplicate strings in a single leaf keep native dictionary storage
  // small while JSON escaping produces a much larger encoded document.
  std::vector<vdb::PointRecord> points(100);for(auto& point:points)point.radius=1;
  auto generation=p;generation.gridName="points";(void)vdb::WritePoints(points,path("many.vdb"),generation);
  const std::string controls(100000,'\x01');
  const std::array<vdb::PointAttributeEdit,1> edits{{{vdb::PointEditOperation::Add,"label",{},std::vector<std::string>{controls}}}};
  (void)vdb::WritePointAttributeEdits(path("many.vdb"),path("controls.vdb"),edits,{}, {},p);
  auto strings=vdb::PointAttributeOptions{};strings.maxStringBytes=100*controls.size();
  auto small=p;small.maxInputBytes=45*1024*1024;
  // This separate read proves the failure is the encoded writer cap, rather
  // than a native source/decoded-payload rejection.
  EXPECT_NO_THROW((void)vdb::ReadPointAttribute(path("controls.vdb"),"label",{},small,strings));
  try{vdb::ExportPointAttribute(path("controls.vdb"),"label",path("escaped.json"),{},small,strings);FAIL();}
  catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("encoded byte budget"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("escaped.json")));
}
#endif
} // namespace
