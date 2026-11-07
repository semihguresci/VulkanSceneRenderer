#include <Container/utility/GltfSourceMaterialExport.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>
#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/geometry/GltfMaterialExport.h>
#include <Container/geometry/GltfSceneExport.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace container::material;
using namespace container::geometry::gltf;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
constexpr const char* sg="KHR_materials_pbrSpecularGlossiness";
V array(std::initializer_list<double> values) {A result;for(const auto value:values) result.emplace_back(value);return V(std::move(result));}
V info(int index) {return V(O{{"index",V(index)},{"extras",V(O{{"opaqueIndex",V(991)}})}});}
struct Fixture {
 std::vector<int32_t> mapping;
 std::vector<uint32_t> aColor,aData,eColor{10,11,12},eData{20,21,22};
 std::vector<std::string> declarations;
 tinygltf::Material authored,evaluated;
 explicit Fixture(bool identity=false) {
  mapping=identity?std::vector<int32_t>{0,1,2}:std::vector<int32_t>{2,0,1,-1};
  aColor=identity?eColor:std::vector<uint32_t>{12,10,11,UINT32_MAX};
  aData=identity?eData:std::vector<uint32_t>{22,20,21,UINT32_MAX};
  declarations={sg,"KHR_texture_transform"};
  const auto make=[&](bool archive) {
   tinygltf::Material m;const auto index=[&](int i) {return archive&&!identity?std::array{1,2,0}[size_t(i)]:i;};
   m.name="Retained SG";m.pbrMetallicRoughness.baseColorTexture.index=index(1);
   m.pbrMetallicRoughness.metallicRoughnessTexture.index=index(2);m.normalTexture.index=index(2);
   m.occlusionTexture.index=index(0);m.emissiveTexture.index=index(0);
   m.pbrMetallicRoughness.baseColorFactor={.125,.25,.5,.75};m.pbrMetallicRoughness.metallicFactor=.17;m.pbrMetallicRoughness.roughnessFactor=.29;
   m.extensions[sg]=V(O{{"diffuseFactor",array({.3,.4,.5,.6})},{"specularFactor",array({1,.766,.336})},
       {"glossinessFactor",V(.8)},{"diffuseTexture",info(index(0))},{"specularGlossinessTexture",info(index(1))},
       {"extras",V(O{{"opaqueMaterialIndex",V(993)}})}});
   // SG excludes the physical lighting extensions. Dormant typed MR bindings
   // above and both legacy/fallback maps below still require complete remap.
   size_t ordinal{};
   for(auto* map:{&m.values,&m.additionalValues}) for(const auto name:KnownLegacyMaterialTextureFields())
    (*map)[std::string(name)].json_double_value={{"index",double(index(int(ordinal++%3)))},{"texCoord",1},{"offsetX",.125},{"rotation",-.25}};
   m.values["baseColorTexture"].json_double_value={{"index",double(index(1))}};
   m.additionalValues["normalTexture"].json_double_value={{"index",double(index(2))},{"scale",.5}};
   m.extras=V(O{{"opaqueTextureIndex",V(909)},{"nested",V(A{V("keep"),V(O{{"index",V(907)}})})},{"negativeZero",V(-0.)}});
   m.extensions_json_string=archive?"authored raw extensions":"evaluated raw extensions";
   m.extras_json_string=archive?"authored raw extras":"evaluated raw extras";
   return m;
  };
  authored=make(true);evaluated=make(false);
  evaluated.extensions.at(sg).Get<O>()["diffuseFactor"]=array({.5,.3,.25,.75});
  evaluated.extensions.at(sg).Get<O>()["glossinessFactor"]=V(.35);
 }
 GltfMaterialConversionContext archive() const {return {aColor.size(),declarations,aColor,aData,30};}
 GltfMaterialConversionContext live() const {return {eColor.size(),declarations,eColor,eData,30};}
 Material runtime() const {return ConvertGltfMaterial(evaluated,live()).value;}
 tinygltf::Material save(const GltfSourceMaterialExportLimits& limits={}) const {
  return ExportSourceBackedGltfMaterial(runtime(),authored,evaluated,archive(),live(),mapping,limits);
 }
};
struct Directory {
 std::filesystem::path parent=std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal(),path;
 Directory() {static std::atomic<uint64_t> sequence{};const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
  for(int i=0;i<32;++i) {path=parent/("container-sg-export-"+std::to_string(stamp)+"-"+std::to_string(sequence++));if(std::filesystem::create_directory(path)) return;}
  throw std::runtime_error("Cannot create source-material export test directory");}
 ~Directory() {if(path.parent_path()==parent && path.filename().string().starts_with("container-sg-export-")) {std::error_code error;std::filesystem::remove_all(path,error);}}
};
std::string utf8(const std::filesystem::path& path) {const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
tinygltf::Model packed(const Fixture& f,const tinygltf::Material& material) {
 tinygltf::Model result;result.asset.version="2.0";result.materials={material};result.extensionsUsed=f.declarations;
 std::vector<unsigned char> png;std::string mime;
 if(!tinygltf::DecodeDataURI(&png,mime,"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aP1sAAAAASUVORK5CYII=",0,false)) throw std::runtime_error("Invalid test PNG");
 result.buffers.emplace_back();result.buffers[0].data=png;while(result.buffers[0].data.size()%4) result.buffers[0].data.push_back(0);
 tinygltf::BufferView view;view.buffer=0;view.byteLength=png.size();result.bufferViews.push_back(view);
 result.images.emplace_back();result.images[0].bufferView=0;result.images[0].mimeType="image/png";
 result.textures.resize(f.aColor.size());for(size_t i=0;i<result.textures.size();++i) {result.textures[i].source=0;result.textures[i].name="Source "+std::to_string(i);}
 return result;
}
}

TEST(GltfSourceMaterialExport, PackedSgRetainsCurrentFactorsAndRemapsAllDormantAndFallbackReferences) {
 Fixture f;const auto beforeA=f.authored,beforeE=f.evaluated;const auto result=f.save();
 EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result,f.archive()).value,f.runtime()));
 EXPECT_EQ(result.extensions.at(sg).Get("diffuseFactor"),f.evaluated.extensions.at(sg).Get("diffuseFactor"));
 EXPECT_EQ(result.extensions.at(sg).Get("specularFactor"),f.evaluated.extensions.at(sg).Get("specularFactor"));
 EXPECT_EQ(result.extensions.at(sg).Get("glossinessFactor"),V(.35));
 EXPECT_EQ(result.pbrMetallicRoughness.baseColorFactor,f.evaluated.pbrMetallicRoughness.baseColorFactor);
 EXPECT_EQ(result.pbrMetallicRoughness.baseColorTexture.index,2);EXPECT_EQ(result.pbrMetallicRoughness.metallicRoughnessTexture.index,0);
 EXPECT_EQ(result.normalTexture.index,0);EXPECT_EQ(result.values.at("baseColorTexture").json_double_value.at("index"),2);
 for(const auto* map:{&result.values,&result.additionalValues}) {
  const auto& expected=map==&result.values?f.authored.values:f.authored.additionalValues;
  for(const auto name:KnownLegacyMaterialTextureFields()) EXPECT_EQ(map->at(std::string(name)).json_double_value.at("index"),expected.at(std::string(name)).json_double_value.at("index"));
 }
 EXPECT_EQ(result.extras,f.evaluated.extras);EXPECT_EQ(result.extras_json_string,"evaluated raw extras");EXPECT_TRUE(result.extensions_json_string.empty());
 EXPECT_TRUE(GltfMaterialRecordsEqual(f.authored,beforeA));EXPECT_TRUE(GltfMaterialRecordsEqual(f.evaluated,beforeE));
}

TEST(GltfSourceMaterialExport, ClearedPackedSgWithNewDiffuseBindingExportsWithoutBakingApproximation) {
 Fixture f;for(auto* record:{&f.authored,&f.evaluated}) {
  record->extensions.at(sg).Get<O>().erase("specularGlossinessTexture");record->values.erase("roughnessTexture");record->additionalValues.erase("roughnessTexture");
 }
 f.authored.extensions.at(sg).Get<O>()["diffuseTexture"]=info(0);f.evaluated.extensions.at(sg).Get<O>()["diffuseTexture"]=info(2);
 const auto result=f.save();EXPECT_FALSE(result.extensions.at(sg).Has("specularGlossinessTexture"));
 EXPECT_EQ(result.extensions.at(sg).Get("diffuseTexture").Get("index").Get<int>(),0);
 EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result,f.archive()).value,f.runtime()));
 EXPECT_THROW((void)exportMaterial(f.runtime(),{}),std::runtime_error);
}

TEST(GltfSourceMaterialExport, MixedSgWithoutPackedTextureKeepsStandaloneLegacyRoughnessData) {
 Fixture f;for(auto* record:{&f.authored,&f.evaluated}) record->extensions.at(sg).Get<O>().erase("specularGlossinessTexture");
 const auto runtime=f.runtime();EXPECT_EQ(runtime.specularColorTextureIndex,UINT32_MAX);
 EXPECT_FALSE(GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot::Roughness,runtime));
 const auto result=f.save();EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result,f.archive()).value,runtime));
 EXPECT_EQ(result.values.at("roughnessTexture").json_double_value.at("index"),f.authored.values.at("roughnessTexture").json_double_value.at("index"));
}

TEST(GltfSourceMaterialExport, ActualAsciiAndGlbWriterReloadPreservesSgSourceFactorsAndKnownReferences) {
 Fixture f;Directory directory;const auto exported=f.save();
 for(const auto extension:{".gltf",".glb"}) {
  SCOPED_TRACE(extension);const auto path=directory.path/(std::string("source-backed")+extension);const auto model=packed(f,exported);
  writeExportModel(model,path);tinygltf::TinyGLTF loader;tinygltf::Model loaded;std::string error,warning;
  const bool success=std::string_view(extension)==".glb"?loader.LoadBinaryFromFile(&loaded,&error,&warning,utf8(path)):loader.LoadASCIIFromFile(&loaded,&error,&warning,utf8(path));
  ASSERT_TRUE(success)<<error<<warning;ASSERT_EQ(loaded.materials.size(),1u);const auto& result=loaded.materials[0];
  EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result,f.archive()).value,f.runtime()));
  EXPECT_EQ(result.extensions.at(sg).Get("diffuseFactor"),f.evaluated.extensions.at(sg).Get("diffuseFactor"));
  EXPECT_EQ(result.extensions.at(sg).Get("specularFactor"),f.evaluated.extensions.at(sg).Get("specularFactor"));
  EXPECT_EQ(result.pbrMetallicRoughness.metallicRoughnessTexture.index,0);EXPECT_EQ(result.extras,f.evaluated.extras);
  for(const auto name:KnownLegacyMaterialTextureFields()) for(const auto* map:{&result.values,&result.additionalValues})
   EXPECT_EQ(map->at(std::string(name)).json_double_value.at("index"),(&result.values==map?f.authored.values:f.authored.additionalValues).at(std::string(name)).json_double_value.at("index"));
 }
}

TEST(GltfSourceMaterialExport, RuntimeDriftAndWrongArchiveRolesOrAuthoredBindingsReject) {
 Fixture f;auto changed=f.runtime();changed.roughnessFactor=std::nextafter(changed.roughnessFactor,1.f);
 EXPECT_THROW((void)ExportSourceBackedGltfMaterial(changed,f.authored,f.evaluated,f.archive(),f.live(),f.mapping),std::runtime_error);
 changed=f.runtime();changed.specularColorTextureIndex=21;
 EXPECT_THROW((void)ExportSourceBackedGltfMaterial(changed,f.authored,f.evaluated,f.archive(),f.live(),f.mapping),std::runtime_error);
 f.aColor[2]=29;EXPECT_THROW((void)f.save(),std::runtime_error);f.aColor[2]=11;
 f.authored.extensions.at(sg).Get<O>()["diffuseTexture"]=info(0);EXPECT_THROW((void)f.save(),std::runtime_error);
}

TEST(GltfSourceMaterialExport, MissingAmbiguousAndOutOfRangeSourceMapsReject) {
 Fixture f;for(const auto mapping:std::array<std::vector<int32_t>,4>{{{2,0,0,-1},{2,0,-1,-1},{2,0,3,-1},{2,0,1}}}) {
  EXPECT_THROW((void)ExportSourceBackedGltfMaterial(f.runtime(),f.authored,f.evaluated,f.archive(),f.live(),mapping),std::runtime_error);
 }
}

TEST(GltfSourceMaterialExport, OpaqueExtrasSurviveRemapButUnknownNormativeReferencesReject) {
 Fixture f;f.evaluated.extras.Get<O>()["unknownReferenceShapedAnnotation"]=V(O{{"index",V(0)}});EXPECT_NO_THROW((void)f.save());
 f.evaluated.extensions["VENDOR_texture_reference"]=V(O{{"index",V(0)}});f.declarations.push_back("VENDOR_texture_reference");
 EXPECT_THROW((void)f.save(),std::runtime_error);
 Fixture identity(true);identity.evaluated.extensions["VENDOR_texture_reference"]=V(O{{"index",V(0)}});identity.declarations.push_back("VENDOR_texture_reference");
 identity.evaluated.values["roughnessTexture"].json_double_value["vendorIndex"]=991;
 identity.evaluated.values["roughnessTexture"].json_double_value["index"]=-0.;
 const auto result=identity.save();EXPECT_EQ(result.extensions.at("VENDOR_texture_reference"),identity.evaluated.extensions.at("VENDOR_texture_reference"));
 EXPECT_EQ(result.values.at("roughnessTexture").json_double_value.at("vendorIndex"),991);
 EXPECT_TRUE(std::signbit(result.values.at("roughnessTexture").json_double_value.at("index")));
}

TEST(GltfSourceMaterialExport, FiniteBudgetsAndUnbackedSgRejectWithoutSourceMutation) {
 Fixture f;const auto beforeA=f.authored,beforeE=f.evaluated;
 for(const uint64_t limit:{uint64_t(0),uint64_t(1),UINT64_MAX}) {GltfSourceMaterialExportLimits limits;limits.maxOwnedBytes=limit;EXPECT_THROW((void)f.save(limits),std::runtime_error);}
 auto runtime=f.runtime();runtime.specularGlossinessWorkflow=false;
 EXPECT_THROW((void)ExportSourceBackedGltfMaterial(runtime,f.authored,f.evaluated,f.archive(),f.live(),f.mapping),std::runtime_error);
 EXPECT_TRUE(GltfMaterialRecordsEqual(f.authored,beforeA));EXPECT_TRUE(GltfMaterialRecordsEqual(f.evaluated,beforeE));
 EXPECT_THROW((void)exportMaterial(f.runtime(),{}),std::runtime_error);
}
