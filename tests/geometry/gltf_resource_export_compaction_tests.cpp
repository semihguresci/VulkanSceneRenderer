#include <Container/geometry/GltfResourceExportCompaction.h>
#include <Container/geometry/GltfMaterialTextureAuthoring.h>
#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/geometry/GltfSceneExport.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
constexpr std::array<uint8_t,7> imageMask{1,0,1,0,1,0,1};
constexpr std::array<uint8_t,6> textureMask{1,0,1,0,1,1};
constexpr std::array<uint8_t,5> samplerMask{1,0,1,0,1};
GltfResourceExportMasks masks() {return {imageMask,textureMask,samplerMask};}
V info(int id) {return V(O{{"index",V(id)},{"extras",V(O{{"index",V(91)}})}});}
void declare(tinygltf::Model& model,const std::string& name) {
  if(std::ranges::find(model.extensionsUsed,name)==model.extensionsUsed.end()) model.extensionsUsed.push_back(name);
}
tinygltf::Parameter legacy(double id) {tinygltf::Parameter result;result.json_double_value["index"]=id;result.json_double_value["texCoord"]=1;return result;}
void pointer(tinygltf::Model& model,std::string path) {
  declare(model,"KHR_animation_pointer");if(model.animations.empty()) model.animations.resize(1);
  tinygltf::AnimationChannel channel;channel.target_node=-1;channel.target_path="pointer";
  channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::move(path))}});
  model.animations[0].channels.push_back(std::move(channel));
}
tinygltf::Model fixture() {
  tinygltf::Model model;model.asset.version="2.0";model.images.resize(7);model.textures.resize(6);model.samplers.resize(5);model.materials.resize(1);
  for(size_t i=0;i<model.images.size();++i) {
    auto& image=model.images[i];image.name="Image "+std::to_string(i);image.image={uint8_t(i),23,41,255};
    image.width=1;image.height=1;image.component=4;image.bits=8;image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image.extras=V(O{{"index",V(91)}});
  }
  for(size_t i=0;i<model.samplers.size();++i) {model.samplers[i].name="Sampler "+std::to_string(i);model.samplers[i].wrapS=i%2?TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE:TINYGLTF_TEXTURE_WRAP_REPEAT;}
  for(size_t i=0;i<model.textures.size();++i) {model.textures[i].name="Texture "+std::to_string(i);model.textures[i].source=0;model.textures[i].sampler=0;}
  model.textures[0].sampler=4;model.textures[2].source=2;model.textures[4].source=4;model.textures[4].sampler=2;model.textures[5].source=6;
  model.materials[0].name="Survivor";model.materials[0].normalTexture.index=4;
  model.materials[0].pbrMetallicRoughness.baseColorTexture.index=5;
  model.buffers.resize(1);model.buffers[0].data={0,1,2,3,4,5,6,7};
  model.bufferViews.resize(1);model.bufferViews[0].buffer=0;model.bufferViews[0].byteLength=8;
  model.accessors.resize(1);model.accessors[0].sparse={};model.accessors[0].bufferView=0;
  model.accessors[0].componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;model.accessors[0].type=TINYGLTF_TYPE_SCALAR;model.accessors[0].count=8;
  model.extras=V(O{{"images",V(A{V(1),V(3)})},{"extensions",V(O{{"VENDOR_annotation",V(17)}})}});
  return model;
}
std::vector<int32_t> ids(std::span<const int32_t> values) {return {values.begin(),values.end()};}
struct Directory {
  std::filesystem::path parent=std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal(),path;
  Directory() {
    static std::atomic<uint64_t> sequence{};const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    for(int i=0;i<32;++i) {path=parent/("container-resource-compact-"+std::to_string(stamp)+"-"+std::to_string(sequence++));if(std::filesystem::create_directory(path)) return;}
    throw std::runtime_error("Cannot create resource compaction test directory");
  }
  ~Directory() {if(path.parent_path()==parent && path.filename().string().starts_with("container-resource-compact-")) {std::error_code ignored;std::filesystem::remove_all(path,ignored);}}
};
std::string utf8(const std::filesystem::path& path) {const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
}

TEST(GltfResourceExportCompaction, ArbitraryMiddleHolesPreserveOrderAndBinaryAllocations) {
  auto model=fixture();const auto before=model;const auto* bytes=model.buffers[0].data.data();const auto* views=model.bufferViews.data();const auto* accessors=model.accessors.data();
  std::array<const void*,4> pixels{};for(size_t i=0;i<pixels.size();++i) pixels[i]=model.images[i*2].image.data();
  const auto plan=PlanGltfResourceExportCompaction(model,masks());EXPECT_EQ(model,before);
  EXPECT_EQ(ids(plan.oldToNewImages()),(std::vector<int32_t>{0,-1,1,-1,2,-1,3}));
  EXPECT_EQ(ids(plan.oldToNewTextures()),(std::vector<int32_t>{0,-1,1,-1,2,3}));
  EXPECT_EQ(ids(plan.oldToNewSamplers()),(std::vector<int32_t>{0,-1,1,-1,2}));
  EXPECT_EQ(ids(plan.newToOldImages()),(std::vector<int32_t>{0,2,4,6}));EXPECT_EQ(plan.stats().retiredTextures,2u);
  ApplyGltfResourceExportCompaction(model,plan);
  ASSERT_EQ(model.images.size(),4u);ASSERT_EQ(model.textures.size(),4u);ASSERT_EQ(model.samplers.size(),3u);
  for(size_t i=0;i<4;++i) {EXPECT_EQ(model.images[i].name,"Image "+std::to_string(i*2));EXPECT_EQ(model.images[i].image.data(),pixels[i]);EXPECT_EQ(model.images[i],before.images[i*2]);}
  EXPECT_EQ(model.textures[1].name,"Texture 2");EXPECT_EQ(model.textures[1].source,1);EXPECT_EQ(model.textures[2].sampler,1);
  EXPECT_EQ(model.materials[0].normalTexture.index,2);EXPECT_EQ(model.materials[0].pbrMetallicRoughness.baseColorTexture.index,3);
  EXPECT_EQ(model.buffers,before.buffers);EXPECT_EQ(model.bufferViews,before.bufferViews);EXPECT_EQ(model.accessors,before.accessors);
  EXPECT_EQ(model.buffers[0].data.data(),bytes);EXPECT_EQ(model.bufferViews.data(),views);EXPECT_EQ(model.accessors.data(),accessors);EXPECT_EQ(model.extras,before.extras);
}

TEST(GltfResourceExportCompaction, EveryRawTypedExtensionAndShadowedLegacyBindingRemaps) {
  auto model=fixture();auto& material=model.materials[0];material.pbrMetallicRoughness.metallicRoughnessTexture.index=2;
  material.occlusionTexture.index=5;material.emissiveTexture.index=4;material.values["baseColorTexture"]=legacy(2);material.additionalValues["baseColorTexture"]=legacy(4);
  for(const auto field:KnownLegacyMaterialTextureFields()) {material.values[std::string(field)]=legacy(5);material.additionalValues[std::string(field)]=legacy(2);}
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) if(!slot.extension.empty()) {
    declare(model,std::string(slot.extension));auto& extension=material.extensions[std::string(slot.extension)];if(!extension.IsObject()) extension=V(O{});
    extension.Get<O>()[std::string(slot.path.substr(slot.path.rfind('/')+1))]=info(4);
  }
  declare(model,"KHR_materials_pbrSpecularGlossiness");material.extensions["KHR_materials_pbrSpecularGlossiness"]=V(O{{"diffuseTexture",info(5)},{"specularGlossinessTexture",info(2)}});
  material.extensions_json_string="stale raw extension cache";const auto plan=PlanGltfResourceExportCompaction(model,masks());ApplyGltfResourceExportCompaction(model,plan);
  const auto& output=model.materials[0];EXPECT_EQ(output.pbrMetallicRoughness.baseColorTexture.index,3);EXPECT_EQ(output.pbrMetallicRoughness.metallicRoughnessTexture.index,1);
  EXPECT_EQ(output.normalTexture.index,2);EXPECT_EQ(output.occlusionTexture.index,3);EXPECT_EQ(output.emissiveTexture.index,2);
  EXPECT_EQ(output.values.at("baseColorTexture").TextureIndex(),1);EXPECT_EQ(output.additionalValues.at("baseColorTexture").TextureIndex(),2);
  for(const auto field:KnownLegacyMaterialTextureFields()) {EXPECT_EQ(output.values.at(std::string(field)).TextureIndex(),3);EXPECT_EQ(output.additionalValues.at(std::string(field)).TextureIndex(),1);}
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) if(!slot.extension.empty()) {
    const auto& binding=output.extensions.at(std::string(slot.extension)).Get(std::string(slot.path.substr(slot.path.rfind('/')+1)));
    EXPECT_EQ(binding.Get("index").Get<int>(),2);EXPECT_EQ(binding.Get("extras").Get("index").Get<int>(),91);
  }
  EXPECT_EQ(output.extensions.at("KHR_materials_pbrSpecularGlossiness").Get("diffuseTexture").Get("index").Get<int>(),3);
  EXPECT_EQ(output.extensions.at("KHR_materials_pbrSpecularGlossiness").Get("specularGlossinessTexture").Get("index").Get<int>(),1);
  EXPECT_TRUE(output.extensions_json_string.empty());
}

TEST(GltfResourceExportCompaction, EveryCodecAlternativeAndSamplerRemapsIndependently) {
  auto model=fixture();auto& texture=model.textures[0];int image=0;
  for(const auto* name:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds","EXT_texture_avif"}) {
    declare(model,name);texture.extensions[name]=V(O{{"source",V(image)},{"extras",V(O{{"source",V(99)}})}});image+=2;
  }
  texture.extensions_json_string="cached source IDs";const auto plan=PlanGltfResourceExportCompaction(model,masks());ApplyGltfResourceExportCompaction(model,plan);
  EXPECT_EQ(model.textures[0].source,0);EXPECT_EQ(model.textures[0].sampler,2);image=0;
  for(const auto* name:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds","EXT_texture_avif"}) {
    EXPECT_EQ(model.textures[0].extensions.at(name).Get("source").Get<int>(),image++);
    EXPECT_EQ(model.textures[0].extensions.at(name).Get("extras").Get("source").Get<int>(),99);
  }
  EXPECT_TRUE(model.textures[0].extensions_json_string.empty());
}

TEST(GltfResourceExportCompaction, RemovedTargetsRejectEvenWhenTheirBindingIsShadowedOrUnusedForRendering) {
  for(int kind=0;kind<5;++kind) {
    SCOPED_TRACE(kind);auto model=fixture();
    if(kind==0) model.materials[0].values["baseColorTexture"]=legacy(1);
    if(kind==1) model.materials[0].additionalValues["normalTexture"]=legacy(3);
    if(kind==2) model.textures[0].source=1;
    if(kind==3) model.textures[0].sampler=3;
    if(kind==4) {declare(model,"EXT_texture_avif");model.textures[0].extensions["EXT_texture_avif"]=V(O{{"source",V(5)}});}
    const auto before=model;EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,masks()),std::invalid_argument);EXPECT_EQ(model,before);
  }
}

TEST(GltfResourceExportCompaction, RetiredOwnersLoseNormativeFieldsAndChannelsBeforeReferenceValidation) {
  auto model=fixture();model.materials.resize(2);model.materials[1].normalTexture.index=91;model.materials[1].extensions["VENDOR_material"]=V(17);
  model.materials[1].name="Retired";model.materials[1].extras=V(O{{"texture",V(91)}});model.nodes.resize(2);
  model.nodes[1].extensions["VENDOR_node"]=V(19);model.nodes[1].name="Retired node";model.nodes[1].extras=V(O{{"texture",V(91)}});
  pointer(model,"/materials/1/VENDOR_unproved/property");pointer(model,"/nodes/1/VENDOR_unproved/property");
  pointer(model,"/materials/0/pbrMetallicRoughness/roughnessFactor");pointer(model,"/nodes/0/translation");
  const std::array<uint8_t,2> owners{1,0};auto selected=masks();selected.materials=owners;selected.nodes=owners;
  const auto plan=PlanGltfResourceExportCompaction(model,selected);ApplyGltfResourceExportCompaction(model,plan);
  EXPECT_EQ(model.materials[1].normalTexture.index,-1);EXPECT_TRUE(model.materials[1].extensions.empty());EXPECT_EQ(model.materials[1].name,"Retired");
  EXPECT_EQ(model.materials[1].extras.Get("texture").Get<int>(),91);EXPECT_TRUE(model.nodes[1].extensions.empty());
  ASSERT_EQ(model.animations[0].channels.size(),2u);EXPECT_EQ(model.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>(),"/materials/0/pbrMetallicRoughness/roughnessFactor");
}

TEST(GltfResourceExportCompaction, ActiveUnknownNormativeSchemasAndDirectResourceAnimationFailClosed) {
  for(const auto* path:{"/images/0/name","/textures/0/source","/samplers/0/wrapS","/materials/0/roughnessFactor"}) {
    auto model=fixture();pointer(model,path);const auto before=model;EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,masks()),std::invalid_argument);EXPECT_EQ(model,before);
  }
  for(int kind=0;kind<4;++kind) {
    auto model=fixture();if(kind==0) model.extensions["VENDOR_root"]=V(O{});if(kind==1) model.materials[0].extensions["VENDOR_material"]=V(O{});
    if(kind==2) model.images[0].extensions["VENDOR_image"]=V(O{});if(kind==3) model.samplers[0].extensions["VENDOR_sampler"]=V(O{});
    EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,masks()),std::invalid_argument);
  }
}

TEST(GltfResourceExportCompaction, MalformedMasksAndMetadataAndFiniteBudgetsRejectBeforeChanges) {
  auto model=fixture();const auto before=model;auto selected=masks();const std::array<uint8_t,2> shortMask{1,0};selected.images=shortMask;
  EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,selected),std::runtime_error);
  auto bad=imageMask;bad[1]=2;selected=masks();selected.images=bad;EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,selected),std::runtime_error);
  for(int kind=0;kind<4;++kind) {
    GltfResourceExportCompactionLimits limits;if(kind==0) limits.maxOwnedBytes=64;if(kind==1) limits.maxTableEntries=3;
    if(kind==2) limits.maxPayloadBytes=1;if(kind==3) limits.maxTraversalSteps=1;
    EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,masks(),limits),std::runtime_error);
  }
  EXPECT_EQ(model,before);model.materials[0].values["normalTexture"]=legacy(1.5);
  EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,masks()),std::invalid_argument);
}

TEST(GltfResourceExportCompaction, ExactPlanRejectsForeignReferenceMetadataStorageAndShapeChanges) {
  for(int kind=0;kind<6;++kind) {
    SCOPED_TRACE(kind);auto model=fixture();const auto plan=PlanGltfResourceExportCompaction(model,masks());
    if(kind==0) {auto other=model;EXPECT_THROW(ApplyGltfResourceExportCompaction(other,plan),std::runtime_error);continue;}
    if(kind==1) model.materials[0].normalTexture.index=2;
    if(kind==2) model.images[0].uri="changed.png";
    if(kind==3) model.images[0].image.reserve(model.images[0].image.capacity()+16);
    if(kind==4) model.textures.emplace_back();
    if(kind==5) model.materials[0].extensions_json_string="new raw cache";
    const auto before=model;EXPECT_THROW(ApplyGltfResourceExportCompaction(model,plan),std::runtime_error);EXPECT_EQ(model,before);
  }
}

TEST(GltfResourceExportCompaction, ExtensionIndicesRequireExactIntWhileLegacyAcceptsCheckedIntegralDouble) {
  for(const double number:{2.0,2.5,double(std::numeric_limits<int>::max())+1,std::numeric_limits<double>::infinity()}) for(int kind=0;kind<2;++kind) {
    auto model=fixture();
    if(kind==0) {declare(model,"KHR_materials_specular");model.materials[0].extensions["KHR_materials_specular"]=V(O{{"specularTexture",V(O{{"index",V(number)}})}});}
    else {declare(model,"EXT_texture_webp");model.textures[0].extensions["EXT_texture_webp"]=V(O{{"source",V(number)}});}
    const auto before=model;EXPECT_ANY_THROW((void)PlanGltfResourceExportCompaction(model,masks()));
    // tinygltf equality treats infinity numerically; preserve the exact typed
    // extension in addition to the untouched model for finite malformed rows.
    if(std::isfinite(number)) EXPECT_EQ(model,before);
  }
  auto model=fixture();model.materials[0].values["normalTexture"]=legacy(2.0);
  const auto plan=PlanGltfResourceExportCompaction(model,masks());ApplyGltfResourceExportCompaction(model,plan);EXPECT_EQ(model.materials[0].values.at("normalTexture").TextureIndex(),1);
  model=fixture();model.materials[0].values["normalTexture"]=legacy(double(std::numeric_limits<int>::max())+1);
  EXPECT_THROW((void)PlanGltfResourceExportCompaction(model,masks()),std::invalid_argument);
}

TEST(GltfResourceExportCompaction, EmptyAndFullyRetiredTablesHaveExplicitDenseResults) {
  tinygltf::Model empty;const auto noResources=PlanGltfResourceExportCompaction(empty);ApplyGltfResourceExportCompaction(empty,noResources);EXPECT_TRUE(empty.images.empty());
  auto model=fixture();model.materials.clear();const std::array<uint8_t,7> images{};const std::array<uint8_t,6> textures{};const std::array<uint8_t,5> samplers{};
  const auto plan=PlanGltfResourceExportCompaction(model,{images,textures,samplers});ApplyGltfResourceExportCompaction(model,plan);
  EXPECT_TRUE(model.images.empty());EXPECT_TRUE(model.textures.empty());EXPECT_TRUE(model.samplers.empty());EXPECT_EQ(model.buffers[0].data.size(),8u);
}

TEST(GltfResourceExportCompaction, EveryDeniedApplyAllocationPrecedesAllInputEdits) {
  size_t allocations{};{
    auto model=fixture();const auto plan=PlanGltfResourceExportCompaction(model,masks());material_publication_test::AllocationGuard guard(false);
    ApplyGltfResourceExportCompaction(model,plan);guard.disable();allocations=guard.allocations();
  }
  ASSERT_GT(allocations,0u);ASSERT_LT(allocations,4096u);
  for(size_t allowance=0;allowance<=allocations;++allowance) {
    SCOPED_TRACE(allowance);auto model=fixture();const auto before=model;const auto plan=PlanGltfResourceExportCompaction(model,masks());bool denied=false;
    material_publication_test::AllocationGuard guard(false,allowance);try {ApplyGltfResourceExportCompaction(model,plan);} catch(const std::bad_alloc&) {denied=true;}guard.disable();
    if(allowance<allocations) {EXPECT_TRUE(denied);EXPECT_EQ(model,before);}else {EXPECT_FALSE(denied);EXPECT_EQ(model.images.size(),4u);EXPECT_EQ(model.materials[0].normalTexture.index,2);}
  }
}

TEST(GltfResourceExportCompaction, ExportFiltersRetiredExternalImagesBeforeUriIoForBothFormats) {
  using container::geometry::Model;using container::scene::SceneGraph;Directory directory;SceneGraph graph;Model geometry;tinygltf::Model source;source.materials.resize(1);
  source.images.resize(3);source.images[0].uri="missing-retired-first.png";source.images[2].uri="missing-retired-last.png";
  std::string mime;ASSERT_TRUE(tinygltf::DecodeDataURI(&source.images[1].image,mime,"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aP1sAAAAASUVORK5CYII=",0,false));
  source.images[1].mimeType="image/png";source.images[1].as_is=true;source.textures.resize(3);source.samplers.resize(3);
  for(int i=0;i<3;++i) {source.textures[size_t(i)].source=i;source.textures[size_t(i)].sampler=i;}
  source.materials[0].pbrMetallicRoughness.baseColorTexture.index=1;const auto before=source;const std::array<uint8_t,3> active{0,1,0};
  SceneExportSnapshot snapshot{graph,geometry,&source,directory.path/"source.gltf"};snapshot.sourceResourceMasks={active,active,active};
  for(const auto* suffix:{".gltf",".glb"}) {
    const auto destination=directory.path/(std::string("surviving")+suffix);writeSceneExport(snapshot,destination);
    tinygltf::TinyGLTF loader;tinygltf::Model loaded;std::string error,warning;
    const auto okay=std::string_view(suffix)==".glb"?loader.LoadBinaryFromFile(&loaded,&error,&warning,utf8(destination)):loader.LoadASCIIFromFile(&loaded,&error,&warning,utf8(destination));
    ASSERT_TRUE(okay)<<error<<warning;ASSERT_EQ(loaded.images.size(),1u);ASSERT_EQ(loaded.textures.size(),1u);ASSERT_EQ(loaded.samplers.size(),1u);
    EXPECT_EQ(loaded.textures[0].source,0);EXPECT_EQ(loaded.textures[0].sampler,0);EXPECT_EQ(loaded.materials[0].pbrMetallicRoughness.baseColorTexture.index,0);
  }
  EXPECT_EQ(source,before);
}

TEST(GltfResourceExportCompaction, NoResourceHolesPreserveExistingUnknownSchemaExportBehavior) {
  using container::geometry::Model;using container::scene::SceneGraph;SceneGraph graph;Model geometry;tinygltf::Model source;
  source.extensions["VENDOR_preserved"]=V(O{{"index",V(19)}});source.materials.resize(1);source.materials[0].extensions["VENDOR_material"]=V(O{{"index",V(29)}});
  const auto output=buildExportModel({graph,geometry,&source});EXPECT_EQ(output.extensions,source.extensions);EXPECT_EQ(output.materials[0].extensions,source.materials[0].extensions);
}

TEST(GltfResourceExportCompaction, ExportDropsRetiredMaterialChannelsBeforeAnimationParsingAndImagePacking) {
  using container::geometry::Model;using container::scene::SceneGraph;SceneGraph graph;Model geometry;tinygltf::Model source;
  source.materials.resize(2);source.materials[1].normalTexture.index=99;source.materials[1].extensions["VENDOR_retired"]=V(O{{"texture",V(99)}});
  source.materials[1].extras=V(O{{"texture",V(99)}});source.images.resize(1);source.images[0].uri="never-open-this-retired-image.png";
  pointer(source,"/materials/1/VENDOR_unproved/texture");const auto before=source;
  const std::array<uint8_t,1> inactive{0};const std::array<uint8_t,2> materials{1,0};
  SceneExportSnapshot snapshot{graph,geometry,&source};snapshot.sourceResourceMasks.images=inactive;snapshot.sourceResourceMasks.materials=materials;
  const auto output=buildExportModel(snapshot);EXPECT_TRUE(output.images.empty());ASSERT_EQ(output.materials.size(),2u);
  EXPECT_EQ(output.materials[1].normalTexture.index,-1);EXPECT_TRUE(output.materials[1].extensions.empty());EXPECT_EQ(output.materials[1].extras,source.materials[1].extras);
  ASSERT_EQ(output.animations.size(),1u);EXPECT_TRUE(output.animations[0].channels.empty());EXPECT_EQ(source,before);
}
