#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/utility/SceneMaterialVariants.h>
#include <Container/utility/SceneEditHistory.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

namespace {
using namespace container::geometry::gltf;
using Json=nlohmann::json;
constexpr const char* kExtension="KHR_materials_variants";
static_assert(std::is_nothrow_move_constructible_v<MaterialVariantLibrary>);
static_assert(std::is_nothrow_move_assignable_v<MaterialVariantLibrary>);
static_assert(std::is_nothrow_swappable_v<MaterialVariantLibrary>);
tinygltf::Value value(const Json& item) {
  if (item.is_object()) {
    tinygltf::Value::Object output;
    for (const auto& [key,child]:item.items()) output[key]=value(child);
    return tinygltf::Value(std::move(output));
  }
  if (item.is_array()) {
    tinygltf::Value::Array output;
    for (const auto& child:item) output.push_back(value(child));
    return tinygltf::Value(std::move(output));
  }
  if (item.is_boolean()) return tinygltf::Value(item.get<bool>());
  if (item.is_number_integer()) return tinygltf::Value(item.get<int>());
  if (item.is_number()) return tinygltf::Value(item.get<double>());
  if (item.is_string()) return tinygltf::Value(item.get<std::string>());
  return {};
}
tinygltf::Model fixture() {
  tinygltf::Model source; source.asset.version="2.0";
  source.extensionsUsed={kExtension};
  source.extensions[kExtension]=value(Json{{"variants",{{{"name","Warm"},{"extras",{{"sku","red"}}}},{{"name","Cool"}}}},
      {"extras",{{"root",true}}},{"extensions",{{"VENDOR_root",{{"keep",3}}}}}});
  source.materials.resize(4);
  for(size_t m=0;m<4;++m) {
    source.materials[m].name="material"+std::to_string(m);
    source.materials[m].pbrMetallicRoughness.baseColorFactor={double(m)/4,0.5,0.25,1};
  }
  source.meshes.resize(2); source.meshes[0].primitives.resize(2); source.meshes[1].primitives.resize(1);
  auto& first=source.meshes[0].primitives[0]; first.material=0;
  first.extensions[kExtension]=value(Json{{"mappings",{
      {{"material",1},{"variants",{0}},{"name","mapping name"},{"extras",{{"keep","mapping"}}},{"extensions",{{"VENDOR_mapping",{{"keep",true}}}}}},
      {{"material",2},{"variants",{1}}}}},{"extras",{{"keep","primitive"}}},{"extensions",{{"VENDOR_primitive",{{"keep",true}}}}}});
  source.meshes[0].primitives[1].material=-1;
  source.meshes[0].primitives[1].extensions[kExtension]=value(Json{{"mappings",{{{"material",1},{"variants",{1}}}}}});
  source.meshes[1].primitives[0].material=2;
  // Proper geometry allows the same generated fixture to roundtrip glTF/GLB.
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
  tinygltf::Buffer buffer; buffer.data.resize(sizeof(positions));
  std::memcpy(buffer.data.data(),positions.data(),sizeof(positions)); source.buffers.push_back(buffer);
  tinygltf::BufferView view; view.buffer=0; view.byteLength=sizeof(positions); view.target=34962; source.bufferViews.push_back(view);
  tinygltf::Accessor accessor; accessor.bufferView=0; accessor.componentType=5126; accessor.type=TINYGLTF_TYPE_VEC3;
  accessor.count=3; accessor.minValues={0,0,0}; accessor.maxValues={1,1,0}; source.accessors.push_back(accessor);
  for(auto& mesh:source.meshes) for(auto& primitive:mesh.primitives) {
    primitive.mode=4; primitive.attributes={{"POSITION",0}};
  }
  tinygltf::Node node; node.mesh=0; source.nodes.push_back(node); node.mesh=1; source.nodes.push_back(node);
  tinygltf::Scene scene; scene.nodes={0,1}; source.scenes.push_back(scene); source.defaultScene=0;
  return source;
}
std::vector<int32_t> ids(const MaterialVariantSelectionPlan& plan) {
  std::vector<int32_t> result; for(const auto& assignment:plan.assignments) result.push_back(assignment.runtimeMaterial); return result;
}
bool exactMetadata(const tinygltf::Value& first,const tinygltf::Value& second) {
  if(first.Type()!=second.Type()) return false;
  if(first.IsInt()) return first.Get<int>()==second.Get<int>();
  if(first.IsNumber()) return std::bit_cast<uint64_t>(first.Get<double>())==std::bit_cast<uint64_t>(second.Get<double>());
  if(first.IsBool()) return first.Get<bool>()==second.Get<bool>();
  if(first.IsString()) return first.Get<std::string>()==second.Get<std::string>();
  if(first.IsBinary()) return first.Get<std::vector<unsigned char>>()==second.Get<std::vector<unsigned char>>();
  if(first.IsArray()) {
    const auto& a=first.Get<tinygltf::Value::Array>();const auto& b=second.Get<tinygltf::Value::Array>();
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i) if(!exactMetadata(a[i],b[i])) return false;
  } else if(first.IsObject()) {
    const auto& a=first.Get<tinygltf::Value::Object>();const auto& b=second.Get<tinygltf::Value::Object>();
    if(a.size()!=b.size()) return false;
    auto other=b.begin();
    for(const auto& [key,item]:a) {
      if(key!=other->first || !exactMetadata(item,other->second)) return false;
      ++other;
    }
  }
  return true;
}
void replaceMappings(tinygltf::Model& source,const Json& mappings) {
  source.meshes[0].primitives[0].extensions[kExtension]=value(Json{{"mappings",mappings}});
}
class Directory {
public:
  Directory() {
    static std::atomic<uint64_t> sequence{};
    path=std::filesystem::temp_directory_path()/ ("container-variants-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(sequence++));
    std::filesystem::create_directory(path);
  }
  ~Directory() {
    std::error_code ignored;
    for(const auto* name:{"variants.gltf","variants.glb"}) std::filesystem::remove(path/name,ignored);
    std::filesystem::remove(path,ignored);
  }
  std::filesystem::path path;
};

TEST(GltfMaterialVariants, ResolvesCompleteSelectionsAndAuthoredDefaultsWithoutCloningMaterials) {
  const auto source=fixture(); const auto library=ParseMaterialVariants(source);
  ASSERT_EQ(library.names().size(),2u); EXPECT_EQ(library.names()[0],"Warm"); EXPECT_EQ(library.materialCount(),4u);
  EXPECT_EQ(library.primitive(0,0).defaultMaterial,0); EXPECT_EQ(library.primitive(0,1).defaultMaterial,-1);
  const std::array<int32_t,4> mapping{10,20,30,40};
  EXPECT_EQ(ids(planMaterialVariantSelection(library,0,mapping,99)),(std::vector<int32_t>{20,99,30}));
  EXPECT_EQ(ids(planMaterialVariantSelection(library,1,mapping,99)),(std::vector<int32_t>{30,20,30}));
  EXPECT_EQ(ids(planMaterialVariantSelection(library,std::nullopt,mapping,99)),(std::vector<int32_t>{10,99,30}));
  for(int i=0;i<20;++i) {
    (void)planMaterialVariantSelection(library,size_t(i%2),mapping,99);
    EXPECT_EQ(ids(planMaterialVariantSelection(library,std::nullopt,mapping,99)),(std::vector<int32_t>{10,99,30}));
  }
  EXPECT_EQ(source.meshes[0].primitives[0].material,0); EXPECT_EQ(source.materials.size(),4u);
  EXPECT_EQ(source.materials[1].pbrMetallicRoughness.baseColorFactor,(std::vector<double>{0.25,0.5,0.25,1}));
  const auto identity=planMaterialVariantSelection(library,1);
  EXPECT_EQ(ids(identity),(std::vector<int32_t>{2,1,2}));
  EXPECT_EQ(identity.assignments[2].sourceMesh,1u); EXPECT_EQ(identity.assignments[2].sourcePrimitive,0u);
}

TEST(GltfMaterialVariants, OneMappingCanSelectMultipleVariantsAndNamesHaveStableIndexIdentity) {
  auto source=fixture();
  source.extensions[kExtension]=value(Json{{"variants",{{{"name",""}},{{"name",""}},{{"name","Warm"}}}}});
  replaceMappings(source,Json::array({{{"material",3},{"variants",{2,0}}}}));
  const auto library=ParseMaterialVariants(source);
  EXPECT_EQ(library.names()[0],library.names()[1]);
  EXPECT_EQ(ids(planMaterialVariantSelection(library,0)),(std::vector<int32_t>{3,0,2}));
  EXPECT_EQ(ids(planMaterialVariantSelection(library,1)),(std::vector<int32_t>{0,1,2}));
  EXPECT_EQ(ids(planMaterialVariantSelection(library,2)),(std::vector<int32_t>{3,0,2}));
}

TEST(GltfMaterialVariants, NoExtensionSupportsDefaultOnlyAndChecksEmptyLibraryLookup) {
  auto source=fixture(); source.extensions.clear(); source.extensionsUsed.clear();
  for(auto& mesh:source.meshes) for(auto& primitive:mesh.primitives) primitive.extensions.clear();
  const auto library=ParseMaterialVariants(source);
  EXPECT_FALSE(library.hasVariants()); EXPECT_TRUE(library.names().empty());
  EXPECT_EQ(ids(planMaterialVariantSelection(library,std::nullopt)),(std::vector<int32_t>{0,0,2}));
  EXPECT_THROW((void)planMaterialVariantSelection(library,0),std::runtime_error);
  EXPECT_THROW((void)MaterialVariantLibrary{}.primitive(0,0),std::runtime_error);
  EXPECT_THROW((void)library.primitive(2,0),std::runtime_error);
  EXPECT_THROW((void)library.primitive(0,2),std::runtime_error);
}

TEST(GltfMaterialVariants, RejectsInvalidRootTablesNamesAndDeclarations) {
  for(const auto& root:std::vector<Json>{nullptr,Json::array(),Json::object(),{{"variants",Json::array()}},
      {{"variants",{42}}},{{"variants",{{{"name",42}}}}},{{"variants",{{{"extra","missing name"}}}}}}) {
    auto source=fixture(); source.extensions[kExtension]=value(root);
    EXPECT_THROW((void)ParseMaterialVariants(source),std::runtime_error)<<root.dump();
  }
  auto source=fixture(); source.extensions.clear();
  EXPECT_THROW((void)ParseMaterialVariants(source),std::runtime_error);
  source=fixture(); source.extensionsUsed.clear();
  EXPECT_THROW((void)ParseMaterialVariants(source),std::runtime_error);
}

TEST(GltfMaterialVariants, RejectsInvalidMappingTablesIndicesAndDuplicateVariantAssignmentsWithContext) {
  for(const auto& mappings:std::vector<Json>{nullptr,Json::object(),Json::array(),Json::array({42}),
      Json::array({{{"variants",{0}}}}),Json::array({{{"material",1}}}),
      Json::array({{{"material",-1},{"variants",{0}}}}),Json::array({{{"material",4},{"variants",{0}}}}),
      Json::array({{{"material",1.5},{"variants",{0}}}}),Json::array({{{"material",1},{"variants",Json::array()}}}),
      Json::array({{{"material",1},{"variants",{0}},{"name",42}}}),
      Json::array({{{"material",1},{"variants",{0}},{"extensions",Json::array()}}}),
      Json::array({{{"material",1},{"variants",{-1}}}}),Json::array({{{"material",1},{"variants",{2}}}}),
      Json::array({{{"material",1},{"variants",{0,0}}}}),
      Json::array({{{"material",1},{"variants",{0}}},{{"material",1},{"variants",{0}}}})}) {
    auto source=fixture(); replaceMappings(source,mappings);
    try { (void)ParseMaterialVariants(source); FAIL()<<mappings.dump(); }
    catch(const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("mesh 0 primitive 0"),std::string::npos); }
  }
  auto source=fixture(); source.meshes[1].primitives[0].material=4;
  EXPECT_THROW((void)ParseMaterialVariants(source),std::runtime_error);
  source.meshes[1].primitives[0].material=-2;
  EXPECT_THROW((void)ParseMaterialVariants(source),std::runtime_error);
}

TEST(GltfMaterialVariants, InvalidSelectionOrMissingAlternateMaterialFailsBeforeAnyLiveAssignment) {
  const auto source=fixture(); const auto library=ParseMaterialVariants(source);
  const auto before=planMaterialVariantSelection(library,std::nullopt);
  EXPECT_THROW((void)planMaterialVariantSelection(library,2),std::runtime_error);
  EXPECT_THROW((void)planMaterialVariantSelection(library,std::nullopt,{},-1),std::runtime_error);
  EXPECT_THROW((void)planMaterialVariantSelection(library,0,std::array<int32_t,3>{0,1,2}),std::runtime_error);
  // An initially undrawn alternate material must still be preloaded/translated.
  EXPECT_THROW((void)planMaterialVariantSelection(library,std::nullopt,std::array<int32_t,4>{0,1,2,-1}),std::runtime_error);
  EXPECT_EQ(ids(planMaterialVariantSelection(library,std::nullopt)),ids(before));
  EXPECT_EQ(source.meshes[0].primitives[0].material,0);
}

TEST(GltfMaterialVariants, ExportRemapsAllAlternateMaterialsAndPreservesNestedMetadataAndCurrentAppearance) {
  const auto source=fixture(); const auto library=ParseMaterialVariants(source);
  auto output=source; output.materials.resize(8); output.meshes[0].primitives[0].material=6;
  const std::array<int32_t,4> mapping{4,5,6,7};
  const std::array primitives{MaterialVariantExportPrimitive{0,0,0,0},MaterialVariantExportPrimitive{0,1,0,1},MaterialVariantExportPrimitive{1,0,1,0}};
  CopyMaterialVariantMetadataForExport(output,library,mapping,primitives);
  EXPECT_EQ(output.meshes[0].primitives[0].material,6);
  const auto& root=output.extensions.at(kExtension);
  EXPECT_TRUE(root.Get("extras").Get("root").Get<bool>());
  EXPECT_EQ(root.Get("variants").Get(0).Get("extras").Get("sku").Get<std::string>(),"red");
  EXPECT_EQ(root.Get("extensions").Get("VENDOR_root").Get("keep").Get<int>(),3);
  const auto& extension=output.meshes[0].primitives[0].extensions.at(kExtension);
  const auto& first=extension.Get("mappings").Get(0);
  EXPECT_EQ(first.Get("material").Get<int>(),5); EXPECT_EQ(first.Get("name").Get<std::string>(),"mapping name");
  EXPECT_EQ(first.Get("extras").Get("keep").Get<std::string>(),"mapping");
  EXPECT_TRUE(first.Get("extensions").Get("VENDOR_mapping").Get("keep").Get<bool>());
  EXPECT_EQ(extension.Get("extras").Get("keep").Get<std::string>(),"primitive");
  EXPECT_TRUE(extension.Get("extensions").Get("VENDOR_primitive").Get("keep").Get<bool>());
  EXPECT_EQ(source.meshes[0].primitives[0].extensions.at(kExtension).Get("mappings").Get(0).Get("material").Get<int>(),1);
  EXPECT_EQ(output.buffers[0].data,source.buffers[0].data);
  const auto reloaded=ParseMaterialVariants(output);
  EXPECT_EQ(ids(planMaterialVariantSelection(reloaded,std::nullopt)),(std::vector<int32_t>{6,0,2}));
  EXPECT_EQ(ids(planMaterialVariantSelection(reloaded,0)),(std::vector<int32_t>{5,0,2}));
}

TEST(GltfMaterialVariants, ExportMapsInstancedPrimitivesAndRejectsInvalidPlansWithoutPartialMetadataWrites) {
  const auto source=fixture(); const auto library=ParseMaterialVariants(source);
  auto output=source; output.extensions.erase(kExtension); output.meshes[1].primitives[0].extensions.clear();
  const auto valid=MaterialVariantExportPrimitive{0,0,1,0};
  const std::array invalid{valid,MaterialVariantExportPrimitive{0,0,99,0}};
  EXPECT_THROW(CopyMaterialVariantMetadataForExport(output,library,{},invalid),std::runtime_error);
  EXPECT_FALSE(output.extensions.contains(kExtension)); EXPECT_TRUE(output.meshes[1].primitives[0].extensions.empty());
  EXPECT_THROW(CopyMaterialVariantMetadataForExport(output,library,{},std::array{valid,valid}),std::runtime_error);
  EXPECT_THROW(CopyMaterialVariantMetadataForExport(output,library,std::array<int32_t,4>{0,7,2,3},std::array{valid}),std::runtime_error);
  EXPECT_FALSE(output.extensions.contains(kExtension));
  const std::array instances{MaterialVariantExportPrimitive{0,0,0,0},valid};
  CopyMaterialVariantMetadataForExport(output,library,{},instances);
  EXPECT_TRUE(output.extensions.contains(kExtension)); EXPECT_TRUE(output.meshes[1].primitives[0].extensions.contains(kExtension));
}

TEST(GltfMaterialVariants, ExportPreservesRequiredVariantDeclaration) {
  auto source=fixture(); source.extensionsRequired={kExtension};
  const auto library=ParseMaterialVariants(source);
  auto output=source; output.extensionsRequired.clear(); output.extensionsUsed.clear(); output.extensions.clear();
  CopyMaterialVariantMetadataForExport(output,library,{},std::array{MaterialVariantExportPrimitive{0,0,0,0}});
  EXPECT_EQ(output.extensionsRequired,(std::vector<std::string>{kExtension}));
  EXPECT_EQ(output.extensionsUsed,(std::vector<std::string>{kExtension}));
}

TEST(GltfMaterialVariants, MaterialCountGrowthPreservesSelectionWithNoncontiguousRuntimeAndExportIds) {
  const auto source=fixture();const auto original=ParseMaterialVariants(source);
  const auto grown=original.withExtendedMaterialCount(4,6);
  ASSERT_TRUE(grown);EXPECT_EQ(original.materialCount(),4u);EXPECT_EQ(grown->materialCount(),6u);
  const std::array<int32_t,6> mapping{8,3,9,5,0,7};
  EXPECT_EQ(ids(planMaterialVariantSelection(*grown,0,mapping,42)),(std::vector<int32_t>{3,42,9}));
  EXPECT_EQ(ids(planMaterialVariantSelection(*grown,1,mapping,42)),(std::vector<int32_t>{9,3,9}));
  EXPECT_EQ(ids(planMaterialVariantSelection(*grown,std::nullopt,mapping,42)),(std::vector<int32_t>{8,42,9}));
  EXPECT_THROW((void)planMaterialVariantSelection(*grown,0,std::array<int32_t,4>{8,3,9,5}),std::runtime_error);
  auto output=source;output.materials.resize(10);output.meshes[0].primitives[0].material=7;
  CopyMaterialVariantMetadataForExport(output,*grown,mapping,std::array{
      MaterialVariantExportPrimitive{0,0,0,0},MaterialVariantExportPrimitive{0,1,0,1},MaterialVariantExportPrimitive{1,0,1,0}});
  EXPECT_EQ(output.meshes[0].primitives[0].material,7);
  EXPECT_EQ(output.meshes[0].primitives[0].extensions.at(kExtension).Get("mappings").Get(0).Get("material").Get<int>(),3);
  EXPECT_EQ(ids(planMaterialVariantSelection(ParseMaterialVariants(output),0)),(std::vector<int32_t>{3,0,2}));
  EXPECT_EQ(source.materials.size(),4u);EXPECT_EQ(output.buffers[0].data,source.buffers[0].data);
  EXPECT_EQ(output.animations.size(),source.animations.size());EXPECT_EQ(grown->names().size(),2u);
}

TEST(GltfMaterialVariants, MaterialCountGrowthRetainsExactOpaqueMetadataRequiredFlagAndMappingOrder) {
  auto source=fixture();source.extensionsRequired={kExtension};
  auto& root=source.extensions.at(kExtension).Get<tinygltf::Value::Object>();
  root["opaque"]=tinygltf::Value(tinygltf::Value::Object{
      {"integer",tinygltf::Value(16'777'217)},{"real",tinygltf::Value(std::nextafter(.2,1.))},
      {"negativeZero",tinygltf::Value(-0.)},{"binary",tinygltf::Value(std::vector<unsigned char>{0,255,17})}});
  root["variants"].Get<tinygltf::Value::Array>()[1].Get<tinygltf::Value::Object>()["name"]=tinygltf::Value(std::string(""));
  auto& primitive=source.meshes[0].primitives[0].extensions.at(kExtension).Get<tinygltf::Value::Object>();
  primitive["opaque"]=tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(4),tinygltf::Value(4.),tinygltf::Value(-0.)});
  const auto original=ParseMaterialVariants(source);auto grown=original.withExtendedMaterialCount(4,5);
  ASSERT_TRUE(grown);auto again=grown->withExtendedMaterialCount(5,7);ASSERT_TRUE(again);
  auto output=source;output.materials.resize(7);output.extensions.clear();output.extensionsRequired.clear();
  output.extensions["VENDOR_unrelated"]=tinygltf::Value(16'777'219);
  CopyMaterialVariantMetadataForExport(output,*again,{},std::array{
      MaterialVariantExportPrimitive{0,0,0,0},MaterialVariantExportPrimitive{0,1,0,1},MaterialVariantExportPrimitive{1,0,1,0}});
  EXPECT_TRUE(exactMetadata(output.extensions.at(kExtension),source.extensions.at(kExtension)));
  for(size_t m=0;m<source.meshes.size();++m) for(size_t p=0;p<source.meshes[m].primitives.size();++p)
    if(source.meshes[m].primitives[p].extensions.contains(kExtension))
      EXPECT_TRUE(exactMetadata(output.meshes[m].primitives[p].extensions.at(kExtension),source.meshes[m].primitives[p].extensions.at(kExtension)));
  EXPECT_EQ(output.extensionsRequired,(std::vector<std::string>{kExtension}));
  EXPECT_EQ(output.extensions.at("VENDOR_unrelated").Get<int>(),16'777'219);
  EXPECT_EQ(again->names()[1],"");EXPECT_EQ(again->primitive(0,0).mappings,original.primitive(0,0).mappings);
  EXPECT_EQ(again->primitive(0,1).defaultMaterial,-1);EXPECT_EQ(source.materials.size(),4u);
}

TEST(GltfMaterialVariants, EqualMaterialCountDoesNotCloneAndEmptyLibrariesCanGrowWithoutVariants) {
  auto library=ParseMaterialVariants(fixture());
  EXPECT_FALSE(library.withExtendedMaterialCount(4,4));
  MaterialVariantLibrary empty;
  EXPECT_FALSE(empty.withExtendedMaterialCount(0,0));
  auto grown=empty.withExtendedMaterialCount(0,2);ASSERT_TRUE(grown);
  EXPECT_EQ(grown->materialCount(),2u);EXPECT_FALSE(grown->hasVariants());EXPECT_TRUE(grown->names().empty());
  EXPECT_TRUE(planMaterialVariantSelection(*grown,std::nullopt,std::array<int32_t,2>{9,3}).assignments.empty());
  tinygltf::Model source;source.meshes.resize(2);source.meshes[1].primitives.resize(1);
  source.meshes[1].primitives[0].material=-1;
  const auto implicit=ParseMaterialVariants(source);auto extended=implicit.withExtendedMaterialCount(0,1);
  ASSERT_TRUE(extended);EXPECT_EQ(ids(planMaterialVariantSelection(*extended,std::nullopt,std::array<int32_t,1>{31},99)),(std::vector<int32_t>{99}));
  EXPECT_FALSE(extended->hasVariants());EXPECT_EQ(implicit.materialCount(),0u);
}

TEST(GltfMaterialVariants, MaterialCountGrowthRejectsStaleContextShrinkAndIdentityOverflow) {
  const auto library=ParseMaterialVariants(fixture());
  EXPECT_THROW((void)library.withExtendedMaterialCount(3,5),std::runtime_error);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,3),std::runtime_error);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,0),std::runtime_error);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,size_t(INT32_MAX)+1),std::runtime_error);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,SIZE_MAX),std::runtime_error);
  MaterialVariantGrowthLimits limits;limits.maxMaterials=5;
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,6,limits),std::runtime_error);
  EXPECT_EQ(library.materialCount(),4u);EXPECT_EQ(ids(planMaterialVariantSelection(library,1)),(std::vector<int32_t>{2,1,2}));
}

TEST(GltfMaterialVariants, MaterialCountGrowthBoundsCopiedStorageMetadataAndTraversalBeforeReturningCandidate) {
  const auto library=ParseMaterialVariants(fixture());
  const std::array members{&MaterialVariantGrowthLimits::maxMaterials,&MaterialVariantGrowthLimits::maxMeshes,
      &MaterialVariantGrowthLimits::maxPrimitives,&MaterialVariantGrowthLimits::maxVariants,
      &MaterialVariantGrowthLimits::maxMappings,&MaterialVariantGrowthLimits::maxOwnedBytes,
      &MaterialVariantGrowthLimits::maxMetadataValues,&MaterialVariantGrowthLimits::maxTraversalSteps};
  for(const auto member:members) {
    MaterialVariantGrowthLimits limits;limits.*member=1;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5,limits),std::runtime_error);
    limits=MaterialVariantGrowthLimits{};limits.*member=0;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5,limits),std::runtime_error);
    limits=MaterialVariantGrowthLimits{};limits.*member=UINT64_MAX;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5,limits),std::runtime_error);
  }
  for(const auto depth:{0u,1u,129u}) {
    MaterialVariantGrowthLimits limits;limits.maxMetadataDepth=depth;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5,limits),std::runtime_error);
  }
  auto source=fixture();tinygltf::Value nested(5);
  for(size_t i=0;i<12;++i) nested=tinygltf::Value(tinygltf::Value::Array{std::move(nested)});
  source.extensions.at(kExtension).Get<tinygltf::Value::Object>()["opaque"]=std::move(nested);
  const auto deep=ParseMaterialVariants(source);MaterialVariantGrowthLimits limits;limits.maxMetadataDepth=8;
  EXPECT_THROW((void)deep.withExtendedMaterialCount(4,5,limits),std::runtime_error);
  EXPECT_TRUE(deep.withExtendedMaterialCount(4,5));
  EXPECT_EQ(library.materialCount(),4u);EXPECT_EQ(deep.materialCount(),4u);
}

TEST(GltfMaterialVariants, MaterialCountGrowthRefusesMalformedDerivedTablesAndReferencesIntoNewTail) {
  {
    auto library=ParseMaterialVariants(fixture());
    const_cast<MaterialVariantPrimitive&>(library.primitive(0,0)).defaultMaterial=4;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5),std::runtime_error);
  }
  {
    auto library=ParseMaterialVariants(fixture());
    const_cast<MaterialVariantPrimitive&>(library.primitive(0,0)).sourceMesh=1;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5),std::runtime_error);
  }
  for(const auto mappings:std::vector<std::vector<std::pair<size_t,int32_t>>>{
      {{0,4},{1,2}},{{0,1},{0,2}},{{1,2},{0,1}},{{0,2},{1,2}},{{0,1}},{{0,-1},{1,2}}}) {
    auto library=ParseMaterialVariants(fixture());
    const_cast<MaterialVariantPrimitive&>(library.primitive(0,0)).mappings=mappings;
    EXPECT_THROW((void)library.withExtendedMaterialCount(4,5),std::runtime_error);
  }
  auto library=ParseMaterialVariants(fixture());
  const_cast<std::string&>(library.names()[0])="changed after parsing";
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,5),std::runtime_error);
}

TEST(GltfMaterialVariants, MaterialCountGrowthRejectsNonfiniteOpaqueMetadataBeforeCopy) {
  auto source=fixture();
  source.extensions.at(kExtension).Get<tinygltf::Value::Object>()["opaque"]=tinygltf::Value(std::numeric_limits<double>::infinity());
  const auto library=ParseMaterialVariants(source);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,5),std::runtime_error);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,4),std::runtime_error);
  EXPECT_EQ(library.materialCount(),4u);
}

TEST(GltfMaterialVariants, MaterialCountGrowthRejectsInactiveOpaqueValueCopyPayload) {
  auto source=fixture();tinygltf::Value malformed(7);
  malformed.Get<tinygltf::Value::Array>().push_back(tinygltf::Value(std::string(1024,'x')));
  source.extensions.at(kExtension).Get<tinygltf::Value::Object>()["opaque"]=std::move(malformed);
  const auto library=ParseMaterialVariants(source);
  EXPECT_THROW((void)library.withExtendedMaterialCount(4,5),std::runtime_error);
  EXPECT_EQ(library.materialCount(),4u);
}

TEST(GltfMaterialVariants, LastMaterialRemovalPreservesSelectionsAndSparseExportMappings) {
  auto source=fixture();source.extensionsRequired={kExtension};
  const auto original=ParseMaterialVariants(source);const auto removed=original.withRemovedLastMaterial(4);
  EXPECT_EQ(original.materialCount(),4u);EXPECT_EQ(removed.materialCount(),3u);
  const std::array<int32_t,3> mapping{7,4,9};
  EXPECT_EQ(ids(planMaterialVariantSelection(removed,0,mapping,42)),(std::vector<int32_t>{4,42,9}));
  EXPECT_EQ(ids(planMaterialVariantSelection(removed,1,mapping,42)),(std::vector<int32_t>{9,4,9}));
  EXPECT_EQ(ids(planMaterialVariantSelection(removed,std::nullopt,mapping,42)),(std::vector<int32_t>{7,42,9}));
  EXPECT_THROW((void)planMaterialVariantSelection(removed,0,std::array<int32_t,4>{7,4,9,2}),std::runtime_error);
  auto output=source;output.materials.resize(10);output.meshes[0].primitives[0].material=6;
  CopyMaterialVariantMetadataForExport(output,removed,mapping,std::array{
      MaterialVariantExportPrimitive{0,0,0,0},MaterialVariantExportPrimitive{0,1,0,1},MaterialVariantExportPrimitive{1,0,1,0}});
  EXPECT_EQ(output.meshes[0].primitives[0].material,6);
  EXPECT_EQ(output.meshes[0].primitives[0].extensions.at(kExtension).Get("mappings").Get(0).Get("material").Get<int>(),4);
  EXPECT_EQ(ids(planMaterialVariantSelection(ParseMaterialVariants(output),1)),(std::vector<int32_t>{9,4,2}));
  EXPECT_EQ(output.extensionsRequired,source.extensionsRequired);EXPECT_EQ(output.buffers[0].data,source.buffers[0].data);
  EXPECT_EQ(removed.names().size(),original.names().size());EXPECT_EQ(removed.primitive(0,0).mappings,original.primitive(0,0).mappings);
  EXPECT_THROW((void)removed.withRemovedLastMaterial(3),std::runtime_error); // Material 2 is still used.
  EXPECT_TRUE(removed.withExtendedMaterialCount(3,4));
  EXPECT_THROW((void)original.withExtendedMaterialCount(4,3),std::runtime_error);
}

TEST(GltfMaterialVariants, LastMaterialRemovalKeepsExactOpaqueMetadataAndRequiredRoot) {
  auto source=fixture();source.extensionsRequired={kExtension};
  source.extensions.at(kExtension).Get<tinygltf::Value::Object>()["opaque"]=tinygltf::Value(tinygltf::Value::Object{
      {"tailLikeInteger",tinygltf::Value(3)},{"largeInteger",tinygltf::Value(16'777'217)},
      {"negativeZero",tinygltf::Value(-0.)},{"adjacentDouble",tinygltf::Value(std::nextafter(.2,1.))},
      {"typedArray",tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(1),tinygltf::Value(1.)})}});
  source.meshes[0].primitives[0].extensions.at(kExtension).Get<tinygltf::Value::Object>()["opaque"]=
      tinygltf::Value(std::vector<unsigned char>{0,255,17});
  const auto original=ParseMaterialVariants(source);const auto removed=original.withRemovedLastMaterial(4);
  auto output=source;output.materials.resize(3);output.extensionsRequired.clear();output.extensions.clear();
  CopyMaterialVariantMetadataForExport(output,removed,{},std::array{
      MaterialVariantExportPrimitive{0,0,0,0},MaterialVariantExportPrimitive{0,1,0,1},MaterialVariantExportPrimitive{1,0,1,0}});
  EXPECT_TRUE(exactMetadata(output.extensions.at(kExtension),source.extensions.at(kExtension)));
  EXPECT_TRUE(exactMetadata(output.meshes[0].primitives[0].extensions.at(kExtension),source.meshes[0].primitives[0].extensions.at(kExtension)));
  EXPECT_EQ(output.extensionsRequired,(std::vector<std::string>{kExtension}));
  EXPECT_EQ(original.materialCount(),4u);EXPECT_EQ(removed.materialCount(),3u);
  EXPECT_EQ(output.extensions.at(kExtension).Get("opaque").Get("tailLikeInteger").Get<int>(),3); // Opaque values are not guessed references.
}

TEST(GltfMaterialVariants, LastMaterialRemovalHandlesSingleMaterialImplicitDefaultsAndEmptyTables) {
  tinygltf::Model source;source.materials.resize(1);source.meshes.resize(1);source.meshes[0].primitives.resize(1);
  source.meshes[0].primitives[0].material=-1;
  const auto original=ParseMaterialVariants(source);const auto removed=original.withRemovedLastMaterial(1);
  EXPECT_EQ(removed.materialCount(),0u);EXPECT_FALSE(removed.hasVariants());EXPECT_TRUE(removed.names().empty());
  EXPECT_EQ(ids(planMaterialVariantSelection(removed,std::nullopt,{},73)),(std::vector<int32_t>{73}));
  EXPECT_EQ(original.materialCount(),1u);EXPECT_THROW((void)removed.withRemovedLastMaterial(0),std::runtime_error);
  EXPECT_THROW((void)MaterialVariantLibrary{}.withRemovedLastMaterial(0),std::runtime_error);
  source.meshes[0].primitives[0].material=0;
  EXPECT_THROW((void)ParseMaterialVariants(source).withRemovedLastMaterial(1),std::runtime_error);
  source.meshes.clear();
  EXPECT_EQ(ParseMaterialVariants(source).withRemovedLastMaterial(1).materialCount(),0u);
}

TEST(GltfMaterialVariants, LastMaterialRemovalAllowsNamesOnlyVariantRootWithZeroRemainingMaterials) {
  auto source=fixture();source.materials.resize(1);source.extensionsRequired={kExtension};
  for(auto& mesh:source.meshes) for(auto& primitive:mesh.primitives) {primitive.material=-1;primitive.extensions.clear();}
  const auto original=ParseMaterialVariants(source);const auto removed=original.withRemovedLastMaterial(1);
  EXPECT_TRUE(removed.hasVariants());EXPECT_EQ(removed.names().size(),2u);EXPECT_EQ(removed.materialCount(),0u);
  EXPECT_EQ(ids(planMaterialVariantSelection(removed,1,{},67)),(std::vector<int32_t>{67,67,67}));
  auto output=source;output.materials.clear();output.extensionsRequired.clear();
  CopyMaterialVariantMetadataForExport(output,removed,{},std::array{MaterialVariantExportPrimitive{0,0,0,0}});
  EXPECT_TRUE(exactMetadata(output.extensions.at(kExtension),source.extensions.at(kExtension)));
  EXPECT_EQ(output.extensionsRequired,(std::vector<std::string>{kExtension}));
  EXPECT_EQ(ParseMaterialVariants(output).materialCount(),0u);
}

TEST(GltfMaterialVariants, LastMaterialRemovalRejectsDefaultAndMappedTailReferencesBeforeCopy) {
  auto source=fixture();source.meshes[1].primitives[0].material=3;
  auto library=ParseMaterialVariants(source);EXPECT_THROW((void)library.withRemovedLastMaterial(4),std::runtime_error);
  EXPECT_EQ(library.materialCount(),4u);EXPECT_EQ(library.primitive(1,0).defaultMaterial,3);
  source=fixture();replaceMappings(source,Json::array({{{"material",3},{"variants",{0}}}}));
  library=ParseMaterialVariants(source);EXPECT_THROW((void)library.withRemovedLastMaterial(4),std::runtime_error);
  EXPECT_EQ(ids(planMaterialVariantSelection(library,0)),(std::vector<int32_t>{3,0,2}));
  source=fixture();library=ParseMaterialVariants(source);
  const_cast<MaterialVariantPrimitive&>(library.primitive(0,0)).mappings[0].second=3;
  EXPECT_THROW((void)library.withRemovedLastMaterial(4),std::runtime_error);
  EXPECT_EQ(library.materialCount(),4u);
}

TEST(GltfMaterialVariants, LastMaterialRemovalRejectsStaleContextAndBoundedPreflightFailures) {
  const auto library=ParseMaterialVariants(fixture());
  for(const auto expected:{size_t(0),size_t(3),size_t(5),SIZE_MAX})
    EXPECT_THROW((void)library.withRemovedLastMaterial(expected),std::runtime_error);
  const std::array members{&MaterialVariantGrowthLimits::maxMaterials,&MaterialVariantGrowthLimits::maxMeshes,
      &MaterialVariantGrowthLimits::maxPrimitives,&MaterialVariantGrowthLimits::maxVariants,
      &MaterialVariantGrowthLimits::maxMappings,&MaterialVariantGrowthLimits::maxOwnedBytes,
      &MaterialVariantGrowthLimits::maxMetadataValues,&MaterialVariantGrowthLimits::maxTraversalSteps};
  for(const auto member:members) {
    MaterialVariantGrowthLimits limits;limits.*member=1;
    EXPECT_THROW((void)library.withRemovedLastMaterial(4,limits),std::runtime_error);
    limits=MaterialVariantGrowthLimits{};limits.*member=0;
    EXPECT_THROW((void)library.withRemovedLastMaterial(4,limits),std::runtime_error);
    limits=MaterialVariantGrowthLimits{};limits.*member=UINT64_MAX;
    EXPECT_THROW((void)library.withRemovedLastMaterial(4,limits),std::runtime_error);
  }
  MaterialVariantGrowthLimits limits;limits.maxMaterials=3; // The current four-entry context must also fit.
  EXPECT_THROW((void)library.withRemovedLastMaterial(4,limits),std::runtime_error);
  limits={};limits.maxMetadataDepth=1;
  EXPECT_THROW((void)library.withRemovedLastMaterial(4,limits),std::runtime_error);
  EXPECT_EQ(library.materialCount(),4u);EXPECT_EQ(ids(planMaterialVariantSelection(library,0)),(std::vector<int32_t>{1,0,2}));
}

TEST(GltfMaterialVariants, PortableAsciiAndBinaryReloadRetainVariantsSavedDefaultsAndResourceBytes) {
  const auto source=fixture(); const auto library=ParseMaterialVariants(source);
  auto output=source; output.meshes[0].primitives[0].material=2;
  CopyMaterialVariantMetadataForExport(output,library,{},std::array{
      MaterialVariantExportPrimitive{0,0,0,0},MaterialVariantExportPrimitive{0,1,0,1},MaterialVariantExportPrimitive{1,0,1,0}});
  Directory directory;
  for(bool binary:{false,true}) {
    const auto path=directory.path/(binary?"variants.glb":"variants.gltf");
    tinygltf::TinyGLTF writer; ASSERT_TRUE(writer.WriteGltfSceneToFile(&output,path.string(),true,true,true,binary));
    tinygltf::Model reloaded; std::string error,warning; tinygltf::TinyGLTF reader;
    ASSERT_TRUE(binary?reader.LoadBinaryFromFile(&reloaded,&error,&warning,path.string()):
        reader.LoadASCIIFromFile(&reloaded,&error,&warning,path.string()))<<error;
    const auto roundtrip=ParseMaterialVariants(reloaded);
    ASSERT_EQ(roundtrip.names().size(),2u); EXPECT_EQ(roundtrip.names()[1],"Cool");
    EXPECT_EQ(ids(planMaterialVariantSelection(roundtrip,std::nullopt)),(std::vector<int32_t>{2,0,2}));
    EXPECT_EQ(ids(planMaterialVariantSelection(roundtrip,0)),(std::vector<int32_t>{1,0,2}));
    EXPECT_EQ(reloaded.buffers[0].data,source.buffers[0].data);
    EXPECT_EQ(reloaded.extensions.at(kExtension).Get("variants").Get(0).Get("extras").Get("sku").Get<std::string>(),"red");
  }
}

TEST(GltfMaterialVariants, SceneHistoryRestoresExactAssignmentsSelectionAndAllSharedInstancesAfterReparent) {
  const auto library=ParseMaterialVariants(fixture());
  container::scene::SceneGraph graph;
  const auto root=graph.createNode(glm::mat4(1),0);
  const auto first=graph.createNode(glm::mat4(1),77,true,0);
  const auto instance=graph.createNode(glm::mat4(1),88,true,0);
  const auto procedural=graph.createNode(glm::mat4(1),99,true,0);
  for (const auto node:{first,instance}) {
    graph.getNode(node)->sourceMeshIndex=0; graph.getNode(node)->sourcePrimitiveIndex=0;
    graph.setParent(node,root);
  }
  std::optional<size_t> selected;
  size_t publications=0;
  container::util::SceneEditHistory history(graph,{{},{},[&]{++publications;}});
  const auto plan=planMaterialVariantSelection(library,0,std::array<int32_t,4>{10,20,30,40});
  const auto edit=container::scene::PlanMaterialVariantGraphEdit(graph,plan,selected);
  const auto revision=graph.revision();
  ASSERT_TRUE(container::scene::ApplyMaterialVariantGraphEdit(graph,edit,true,selected));
  ASSERT_TRUE(history.recordApplied({"Material variant",
      [&]{return container::scene::ApplyMaterialVariantGraphEdit(graph,edit,false,selected);},
      [&]{return container::scene::ApplyMaterialVariantGraphEdit(graph,edit,true,selected);}}));
  EXPECT_GT(graph.revision(),revision); EXPECT_EQ(selected,0u);
  EXPECT_EQ(graph.getNode(first)->materialIndex,20u); EXPECT_EQ(graph.getNode(instance)->materialIndex,20u);
  EXPECT_EQ(graph.getNode(procedural)->materialIndex,99u);
  ASSERT_TRUE(history.reparent(instance,std::nullopt));
  ASSERT_TRUE(history.undo()); ASSERT_TRUE(history.undo());
  EXPECT_FALSE(selected); EXPECT_EQ(graph.getNode(first)->materialIndex,77u); EXPECT_EQ(graph.getNode(instance)->materialIndex,88u);
  ASSERT_TRUE(history.redo()); ASSERT_TRUE(history.redo());
  EXPECT_EQ(selected,0u); EXPECT_EQ(graph.getNode(instance)->materialIndex,20u);
  EXPECT_EQ(graph.getNode(procedural)->materialIndex,99u); EXPECT_EQ(publications,6u);
  const auto reset=container::scene::PlanMaterialVariantGraphEdit(graph,planMaterialVariantSelection(library,std::nullopt),selected);
  ASSERT_TRUE(container::scene::ApplyMaterialVariantGraphEdit(graph,reset,true,selected));
  EXPECT_FALSE(selected); EXPECT_EQ(graph.getNode(first)->materialIndex,0u);
}

TEST(GltfMaterialVariants, ScenePlanAndPlaybackFailWithoutPartialMutationAndGraphReplacementClearsHistory) {
  container::scene::SceneGraph graph;
  const auto first=graph.createNode(glm::mat4(1),7,true,0);
  graph.getNode(first)->sourceMeshIndex=0; graph.getNode(first)->sourcePrimitiveIndex=0;
  const auto library=ParseMaterialVariants(fixture());
  auto plan=planMaterialVariantSelection(library,0);
  std::optional<size_t> selected;
  const auto edit=container::scene::PlanMaterialVariantGraphEdit(graph,plan,selected);
  auto invalid=edit; invalid.assignments.push_back({999,0,1});
  EXPECT_FALSE(container::scene::ApplyMaterialVariantGraphEdit(graph,invalid,true,selected));
  EXPECT_EQ(graph.getNode(first)->materialIndex,7u); EXPECT_FALSE(selected);
  plan.assignments.clear();
  EXPECT_THROW((void)container::scene::PlanMaterialVariantGraphEdit(graph,plan,selected),std::runtime_error);
  container::util::SceneEditHistory history(graph);
  ASSERT_TRUE(container::scene::ApplyMaterialVariantGraphEdit(graph,edit,true,selected));
  history.recordApplied({"Variant",[&]{return container::scene::ApplyMaterialVariantGraphEdit(graph,edit,false,selected);},
      [&]{return container::scene::ApplyMaterialVariantGraphEdit(graph,edit,true,selected);}});
  graph=container::scene::SceneGraph{};
  EXPECT_FALSE(container::scene::ApplyMaterialVariantGraphEdit(graph,edit,false,selected));
  EXPECT_FALSE(history.canUndo());
}
} // namespace
