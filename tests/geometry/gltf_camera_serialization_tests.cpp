#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/geometry/GltfSceneExport.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using Json=nlohmann::json;
using V=tinygltf::Value;
using O=V::Object;
struct Directory {
  std::filesystem::path root=std::filesystem::temp_directory_path()/("container-camera-serialization-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() {std::filesystem::create_directories(root);}
  ~Directory() {
    std::error_code ignored;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    const auto target=std::filesystem::weakly_canonical(root);
    if(target.parent_path()==parent && target.filename().string().starts_with("container-camera-serialization-")) std::filesystem::remove_all(target,ignored);
  }
};
std::string pathString(const std::filesystem::path& path) {const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
std::vector<unsigned char> bytes(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary|std::ios::ate);const auto size=input.tellg();
  if(!input || size<0 || size>64*1024*1024) throw std::runtime_error("Invalid camera serialization fixture size");
  std::vector<unsigned char> result(static_cast<size_t>(size));input.seekg(0);
  if(size && !input.read(reinterpret_cast<char*>(result.data()),size)) throw std::runtime_error("Cannot read camera serialization fixture");return result;
}
uint32_t word(const std::vector<unsigned char>& data,size_t offset) {
  if(offset>data.size() || data.size()-offset<4) throw std::runtime_error("Truncated GLB fixture header");
  return uint32_t(data[offset])|(uint32_t(data[offset+1])<<8)|(uint32_t(data[offset+2])<<16)|(uint32_t(data[offset+3])<<24);
}
Json json(const std::filesystem::path& path) {
  const auto data=bytes(path);
  if(path.extension()==".glb" || (data.size()>=4 && word(data,0)==0x46546c67)) {
    if(data.size()<20 || word(data,0)!=0x46546c67 || word(data,4)!=2 || word(data,8)!=data.size() ||
        word(data,16)!=0x4e4f534a || word(data,12)%4 || word(data,12)>data.size()-20)
      throw std::runtime_error("Malformed GLB fixture");
    return Json::parse(data.begin()+20,data.begin()+20+word(data,12));
  }
  return Json::parse(data.begin(),data.end());
}
std::vector<unsigned char> tail(const std::filesystem::path& path) {
  const auto data=bytes(path);const auto offset=size_t(20)+word(data,12);
  if(offset>data.size()) throw std::runtime_error("Invalid GLB fixture tail");return {data.begin()+offset,data.end()};
}
tinygltf::Model model(bool legacy=false) {
  tinygltf::Model result;result.asset.version="2.0";result.cameras.resize(3);
  auto& omitted=result.cameras[0];omitted.name="Infinite authored lens";omitted.type="perspective";
  omitted.perspective.yfov=.55;omitted.perspective.znear=.2;omitted.perspective.zfar=0;omitted.perspective.aspectRatio=0;
  omitted.extras=V(O{{"zfar",V(0.)},{"perspective",V(O{{"zfar",V(0.)}})}});
  omitted.perspective.extras=V(O{{"zfar",V(0.)},{"aspectRatio",V(0.)},{"note",V(std::string("retain camera metadata"))}});
  omitted.perspective.extensions["VENDOR_optics"]=V(O{{"zfar",V(0.)},{"positive",V(77.)}});
  result.extensionsUsed.push_back("VENDOR_optics");result.cameras[1]=omitted;
  result.cameras[1].name="Finite authored lens";result.cameras[1].perspective.zfar=150.;result.cameras[1].perspective.aspectRatio=1.75;
  auto& ortho=result.cameras[2];ortho.name="Orthographic lens";ortho.type="orthographic";
  ortho.orthographic.xmag=4;ortho.orthographic.ymag=2;ortho.orthographic.znear=0;ortho.orthographic.zfar=40;
  ortho.orthographic.extras=V(O{{"zfar",V(0.)}});
  result.nodes.resize(3);for(int row=0;row<3;++row) result.nodes[size_t(row)].camera=row;
  result.scenes.resize(1);result.scenes[0].nodes={0,1,2};result.defaultScene=0;
  result.extras=V(O{{"cameras",V(V::Array{V(O{{"perspective",V(O{{"zfar",V(0.)}})}})})}});
  result.buffers.resize(1);result.buffers[0].data={17,29,43,61,79,101,127,149,173,191,211,233,251,3,7,13};
  if(legacy) {result.materials.emplace_back();auto& opacity=result.materials[0].additionalValues["opacity"];opacity.has_number_value=true;opacity.number_value=.5;}
  return result;
}
void checkCameraJson(const Json& output,bool legacy) {
  ASSERT_EQ(output.at("cameras").size(),3u);const auto& omitted=output["cameras"][0]["perspective"];
  EXPECT_FALSE(omitted.contains("zfar"));EXPECT_FALSE(omitted.contains("aspectRatio"));
  EXPECT_DOUBLE_EQ(omitted.at("yfov").get<double>(),.55);EXPECT_DOUBLE_EQ(omitted.at("znear").get<double>(),.2);
  EXPECT_EQ(omitted.at("extras").at("zfar"),0.);EXPECT_EQ(omitted.at("extras").at("aspectRatio"),0.);
  EXPECT_EQ(omitted.at("extras").at("note"),"retain camera metadata");
  EXPECT_EQ(omitted.at("extensions").at("VENDOR_optics").at("zfar"),0.);
  EXPECT_EQ(output["cameras"][0]["extras"]["perspective"]["zfar"],0.);
  EXPECT_DOUBLE_EQ(output["cameras"][1]["perspective"]["zfar"].get<double>(),150.);
  EXPECT_DOUBLE_EQ(output["cameras"][1]["perspective"]["aspectRatio"].get<double>(),1.75);
  EXPECT_DOUBLE_EQ(output["cameras"][2]["orthographic"]["zfar"].get<double>(),40.);
  ASSERT_TRUE(output["cameras"][2]["orthographic"].contains("znear"));
  EXPECT_DOUBLE_EQ(output["cameras"][2]["orthographic"].at("znear").get<double>(),0.);
  EXPECT_DOUBLE_EQ(output["cameras"][2]["orthographic"]["xmag"].get<double>(),4.);
  EXPECT_DOUBLE_EQ(output["cameras"][2]["orthographic"]["ymag"].get<double>(),2.);
  EXPECT_EQ(output["cameras"][2]["orthographic"]["extras"]["zfar"],0.);
  EXPECT_EQ(output["extras"]["cameras"][0]["perspective"]["zfar"],0.);
  if(legacy) EXPECT_DOUBLE_EQ(output.at("materials")[0].at("opacity").get<double>(),.5);
}
}

TEST(GltfCameraSerialization, SharedSceneWriterOmitsInfiniteFarInActualAsciiAndGlbJsonAndPreservesFiniteLenses) {
  Directory directory;
  for(bool legacy:{false,true}) for(const auto* extension:{".gltf",".glb"}) {
    SCOPED_TRACE(legacy);
    SCOPED_TRACE(extension);auto source=model(legacy);const auto path=directory.root/(std::string(legacy?"legacy":"camera-only")+extension);
    ASSERT_NO_THROW(writeExportModel(source,path));checkCameraJson(json(path),legacy);
    tinygltf::TinyGLTF loader;tinygltf::Model loaded;std::string errors,warnings;
    const bool success=path.extension()==".glb"?loader.LoadBinaryFromFile(&loaded,&errors,&warnings,pathString(path)):
        loader.LoadASCIIFromFile(&loaded,&errors,&warnings,pathString(path));
    ASSERT_TRUE(success)<<errors;ASSERT_EQ(loaded.cameras.size(),3u);
    EXPECT_DOUBLE_EQ(loaded.cameras[0].perspective.zfar,0);EXPECT_DOUBLE_EQ(loaded.cameras[0].perspective.aspectRatio,0);
    EXPECT_DOUBLE_EQ(loaded.cameras[1].perspective.zfar,150);EXPECT_DOUBLE_EQ(loaded.cameras[2].orthographic.zfar,40);
    ASSERT_EQ(loaded.buffers.size(),1u);EXPECT_EQ(loaded.buffers[0].data,source.buffers[0].data);
    EXPECT_DOUBLE_EQ(source.cameras[0].perspective.zfar,0);EXPECT_TRUE(source.cameras[0].perspective.extras.Has("zfar"));
  }
}

TEST(GltfCameraSerialization, CameraOnlyRewritePreservesCompleteGlbBinaryTailAndCanonicalContainerLengths) {
  Directory directory;auto source=model();const auto path=directory.root/"staged.glb";tinygltf::TinyGLTF writer;
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&source,pathString(path),false,false,true,true));
  // Retain a future/unknown aligned chunk as well as the actual BIN payload.
  auto raw=bytes(path);const auto appendWord=[&](uint32_t value) {for(unsigned shift=0;shift<32;shift+=8) raw.push_back(static_cast<unsigned char>(value>>shift));};
  appendWord(8);appendWord(0x1234abcd);const std::array<unsigned char,8> opaque{91,17,0,255,39,0,73,141};raw.insert(raw.end(),opaque.begin(),opaque.end());
  const auto length=uint32_t(raw.size());for(unsigned shift=0;shift<32;shift+=8) raw[8+shift/8]=static_cast<unsigned char>(length>>shift);
  {std::ofstream output(path,std::ios::binary|std::ios::trunc);output.write(reinterpret_cast<const char*>(raw.data()),std::streamsize(raw.size()));ASSERT_TRUE(output);}
  ASSERT_TRUE(json(path)["cameras"][0]["perspective"].contains("zfar"));const auto before=tail(path);
  const auto replacement=StageLegacyMaterialSerialization(source,path,true);ASSERT_TRUE(replacement);
  checkCameraJson(json(*replacement),false);EXPECT_EQ(tail(*replacement),before);
  const auto complete=bytes(*replacement);EXPECT_EQ(word(complete,8),complete.size());EXPECT_EQ(word(complete,12)%4,0u);
}

TEST(GltfCameraSerialization, FiniteFarNeedsNoRewriteAndWrongStagedFarCannotBeSilentlyDiscarded) {
  Directory directory;auto finite=model();finite.cameras[0].perspective.zfar=90.;
  EXPECT_FALSE(ValidateSceneSerializationCompatibilityInput(finite));
  const auto path=directory.root/"positive.gltf";tinygltf::TinyGLTF writer;
  ASSERT_TRUE(writer.WriteGltfSceneToFile(&finite,pathString(path),false,false,true,false));
  EXPECT_FALSE(StageLegacyMaterialSerialization(finite,path,false));EXPECT_DOUBLE_EQ(json(path)["cameras"][0]["perspective"]["zfar"].get<double>(),90.);
  finite.cameras[0].perspective.zfar=0;
  EXPECT_THROW((void)StageLegacyMaterialSerialization(finite,path,false),std::runtime_error);
  EXPECT_DOUBLE_EQ(json(path)["cameras"][0]["perspective"]["zfar"].get<double>(),90.);
}

TEST(GltfCameraSerialization, CameraLimitsAndInvalidFarPreservePublishedDestinationBeforeCompatibilityRewrite) {
  Directory directory;const auto path=directory.root/"published.gltf";{std::ofstream file(path,std::ios::binary);file<<"previous published destination";}
  const auto original=bytes(path);auto source=model();
  for(int option=0;option<5;++option) {
    SCOPED_TRACE(option);GltfLegacyMaterialSerializationLimits limits;
    if(option==0) limits.maxCameras=2;if(option==1) limits.maxCameras=0;
    if(option==2) limits.maxCameras=UINT64_MAX;if(option==3) limits.maxJsonBytes=1;if(option==4) limits.maxOwnedBytes=4096;
    EXPECT_THROW(writeExportModel(source,path,limits),std::runtime_error);EXPECT_EQ(bytes(path),original);
  }
  for(const double far:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
    source.cameras[0].perspective.zfar=far;EXPECT_THROW(writeExportModel(source,path),std::runtime_error);EXPECT_EQ(bytes(path),original);
  }
}
