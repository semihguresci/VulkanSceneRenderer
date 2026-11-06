#include <Container/geometry/GltfMaterialExportCompaction.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/geometry/GltfAnimation.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
void declare(tinygltf::Model& model,const std::string& name) {
  if(std::ranges::find(model.extensionsUsed,name)==model.extensionsUsed.end()) model.extensionsUsed.push_back(name);
}
int floats(tinygltf::Model& model,std::initializer_list<float> values,int type,size_t count) {
  if(model.buffers.empty()) model.buffers.emplace_back();
  auto& bytes=model.buffers[0].data;while(bytes.size()%4) bytes.push_back(0);
  tinygltf::BufferView view;view.buffer=0;view.byteOffset=bytes.size();view.byteLength=values.size()*sizeof(float);
  const auto offset=bytes.size();bytes.resize(offset+view.byteLength);std::memcpy(bytes.data()+offset,values.begin(),view.byteLength);
  tinygltf::Accessor accessor;accessor.sparse={};accessor.bufferView=int(model.bufferViews.size());
  accessor.type=type;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;accessor.count=count;
  if(type==TINYGLTF_TYPE_SCALAR) {const auto mm=std::minmax_element(values.begin(),values.end());accessor.minValues={*mm.first};accessor.maxValues={*mm.second};}
  const auto id=int(model.accessors.size());model.bufferViews.push_back(view);model.accessors.push_back(accessor);return id;
}
void pointer(tinygltf::Model& model,std::string path) {
  declare(model,"KHR_animation_pointer");
  tinygltf::Animation animation;tinygltf::AnimationSampler sampler;
  sampler.input=floats(model,{0,1},TINYGLTF_TYPE_SCALAR,2);sampler.output=floats(model,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2);
  animation.samplers.push_back(sampler);tinygltf::AnimationChannel channel;
  channel.sampler=0;channel.target_node=-1;channel.target_path="pointer";
  channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::move(path))},{"extras",V(O{{"material",V(1)}})}});
  channel.target_extras=V(O{{"material",V(1)}});channel.target_extensions_json_string="original pointer cache";
  animation.channels.push_back(channel);model.animations.push_back(animation);
}
std::string text(const tinygltf::Model& model) {
  return model.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>();
}
void variants(tinygltf::Model& model,int material) {
  declare(model,"KHR_materials_variants");
  model.extensions["KHR_materials_variants"]=V(O{{"variants",V(A{V(O{{"name",V(std::string("Variant"))}})})}});
  model.meshes[0].primitives[0].extensions["KHR_materials_variants"]=V(O{{"mappings",V(A{
      V(O{{"material",V(material)},{"variants",V(A{V(0)})},{"extras",V(O{{"material",V(1)}})}})})}});
  model.meshes[0].primitives[0].extensions_json_string="original variant cache";
}
tinygltf::Model fixture() {
  tinygltf::Model model;model.asset.version="2.0";model.materials.resize(5);
  for(size_t m=0;m<5;++m) {model.materials[m].name="Row "+std::to_string(m);model.materials[m].extras=V(O{{"material",V(1)}});}
  const auto positions=floats(model,{0,0,0,1,0,0,0,1,0},TINYGLTF_TYPE_VEC3,3);
  tinygltf::Primitive primitive;primitive.attributes["POSITION"]=positions;primitive.material=4;
  model.meshes.resize(1);model.meshes[0].primitives.push_back(primitive);variants(model,2);
  model.nodes.resize(1);model.nodes[0].mesh=0;model.nodes[0].translation={1,2,3};
  model.scenes.resize(1);model.scenes[0].nodes={0};model.defaultScene=0;
  declare(model,"MSFT_lod");model.materials[0].lods={2,4};
  model.materials[0].extensions["MSFT_lod"]=V(O{{"ids",V(A{V(2),V(4)})},{"extras",V(O{{"index",V(1)}})}});
  model.materials[0].extensions_json_string="original material cache";
  model.materials[1].lods={1,3}; // Removed row's outgoing references vanish.
  pointer(model,"/materials/4/extensions/FUTURE_property/value~1suffix/~0");
  model.extras=V(O{{"materials",V(A{V(1),V(3)})},{"extensions",V(O{{"VENDOR_annotation",V(1)}})}});
  return model;
}
std::vector<int32_t> vector(std::span<const int32_t> values) {return {values.begin(),values.end()};}
struct Directory {
  std::filesystem::path parent=std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal(),path;
  Directory() {
    static std::atomic<uint64_t> sequence{};const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    for(int i=0;i<32;++i) {path=parent/("container-material-compact-"+std::to_string(stamp)+"-"+std::to_string(sequence++));if(std::filesystem::create_directory(path)) return;}
    throw std::runtime_error("Cannot create material compaction test directory");
  }
  ~Directory() {if(path.parent_path()==parent && path.filename().string().starts_with("container-material-compact-")) {std::error_code error;std::filesystem::remove_all(path,error);}}
};
std::string utf8(const std::filesystem::path& path) {const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
}

TEST(GltfMaterialExportCompaction, ArbitraryMiddleRowsRemapEveryKnownMaterialReferenceAndPreservePayloadAllocations) {
  auto model=fixture();
  model.images.resize(1);model.images[0].image={17,23,41,255};model.images[0].as_is=true;model.images[0].mimeType="image/png";
  model.textures.resize(1);model.textures[0].source=0;model.materials[4].normalTexture.index=0;
  const auto before=model;const auto* buffer=model.buffers[0].data.data();const auto* image=model.images[0].image.data();
  const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,2>{3,1});
  EXPECT_EQ(model,before);EXPECT_EQ(vector(plan.oldToNewMaterials()),(std::vector<int32_t>{0,-1,1,-1,2}));
  EXPECT_EQ(vector(plan.newToOldMaterials()),(std::vector<int32_t>{0,2,4}));EXPECT_EQ(plan.stats().retiredMaterials,2u);
  ApplyGltfMaterialExportCompaction(model,plan);const auto& output=model;
  ASSERT_EQ(output.materials.size(),3u);EXPECT_EQ(output.materials[1].name,"Row 2");EXPECT_EQ(output.materials[2].name,"Row 4");
  EXPECT_EQ(output.materials[0].lods,(std::vector<int>{1,2}));
  EXPECT_EQ(output.materials[0].extensions.at("MSFT_lod").Get("ids"),V(A{V(1),V(2)}));
  EXPECT_EQ(output.meshes[0].primitives[0].material,2);
  EXPECT_EQ(output.meshes[0].primitives[0].extensions.at("KHR_materials_variants").Get("mappings").Get<A>()[0].Get("material").Get<int>(),1);
  EXPECT_EQ(text(output),"/materials/2/extensions/FUTURE_property/value~1suffix/~0");
  EXPECT_TRUE(output.materials[0].extensions_json_string.empty());EXPECT_TRUE(output.meshes[0].primitives[0].extensions_json_string.empty());
  EXPECT_TRUE(output.animations[0].channels[0].target_extensions_json_string.empty());
  EXPECT_EQ(output.buffers,before.buffers);EXPECT_EQ(output.bufferViews,before.bufferViews);EXPECT_EQ(output.accessors,before.accessors);
  EXPECT_EQ(output.images,before.images);EXPECT_EQ(output.textures,before.textures);EXPECT_EQ(output.nodes,before.nodes);
  EXPECT_EQ(output.scenes,before.scenes);EXPECT_EQ(output.extras,before.extras);EXPECT_EQ(output.extensions,before.extensions);
  EXPECT_EQ(output.buffers[0].data.data(),buffer);EXPECT_EQ(output.images[0].image.data(),image);
  EXPECT_EQ(output.materials[2].normalTexture.index,0);EXPECT_EQ(output.materials[2].extras,before.materials[4].extras);
}

TEST(GltfMaterialExportCompaction, EveryIncomingReferenceToRetiredRowsBlocksWithoutChangingInput) {
  for(const int kind:{0,1,2,3,4}) {
    SCOPED_TRACE(kind);auto model=fixture();
    if(kind==0) model.meshes[0].primitives[0].material=1;
    if(kind==1) model.meshes[0].primitives[0].extensions.at("KHR_materials_variants").Get<O>().at("mappings").Get<A>()[0].Get<O>().at("material")=V(1);
    if(kind==2) model.materials[0].lods.push_back(1);
    if(kind==3) model.materials[0].extensions.at("MSFT_lod").Get<O>().at("ids").Get<A>().push_back(V(1));
    if(kind==4) model.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get<O>().at("pointer")=V(std::string("/materials/1/unknown/property"));
    const auto before=model;EXPECT_THROW((void)PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1}),std::runtime_error);EXPECT_EQ(model,before);
  }
}

TEST(GltfMaterialExportCompaction, ChangedReferencesShapesForeignModelsAndUnknownMetadataRejectBeforeAnyMutation) {
  for(const int kind:{0,1,2,3}) {
    SCOPED_TRACE(kind);auto model=fixture();const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,2>{1,3});
    if(kind==0) model.meshes[0].primitives[0].material=2;
    if(kind==1) model.meshes[0].primitives.push_back(model.meshes[0].primitives[0]);
    if(kind==2) model.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get<O>().at("pointer")=V(std::string("/materials/2/newSuffix"));
    if(kind==3) {declare(model,"VENDOR_unknown");model.materials[0].extensions["VENDOR_unknown"]=V(O{{"material",V(1)}});}
    const auto before=model;EXPECT_THROW((void)ApplyGltfMaterialExportCompaction(model,plan),std::runtime_error);EXPECT_EQ(model,before);
  }
  auto model=fixture();const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1});auto foreign=model;
  EXPECT_THROW((void)ApplyGltfMaterialExportCompaction(foreign,plan),std::runtime_error);EXPECT_EQ(foreign,model);
  EXPECT_THROW((void)ApplyGltfMaterialExportCompaction(model,{}),std::runtime_error);
}

TEST(GltfMaterialExportCompaction, UnknownNormativeDomainsFailClosedWhileExtrasRemainOpaque) {
  for(const int domain:{0,1,2,3,4,5}) {
    auto model=fixture();declare(model,"VENDOR_reference");
    const V body(O{{"material",V(1)}});
    if(domain==0) model.extensions["VENDOR_reference"]=body;
    if(domain==1) model.materials[0].extensions["VENDOR_reference"]=body;
    if(domain==2) model.meshes[0].primitives[0].extensions["VENDOR_reference"]=body;
    if(domain==3) model.nodes[0].extensions["VENDOR_reference"]=body;
    if(domain==4) {declare(model,"KHR_materials_clearcoat");model.materials[0].extensions["KHR_materials_clearcoat"]=V(O{{"clearcoatTexture",V(O{{"extensions",V(O{{"VENDOR_reference",body}})}})}});}
    if(domain==5) {std::erase(model.extensionsUsed,"VENDOR_reference");model.meshes[0].primitives[0].extensions["MSFT_lod"]=V(O{{"ids",V(A{V(1)})}});}
    EXPECT_THROW((void)PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1}),std::runtime_error);
  }
  auto model=fixture();model.materials[0].extras=V(O{{"extensions",V(O{{"VENDOR_reference",V(O{{"material",V(1)}})}})}});
  const auto before=model.materials[0].extras;const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1});
  ApplyGltfMaterialExportCompaction(model,plan);const auto& output=model;EXPECT_EQ(output.materials[0].extras,before);
}

TEST(GltfMaterialExportCompaction, RegisteredImageAlternativesRemainUntouchedAndTheirReferencesAreBounded) {
  auto model=fixture();model.images.resize(2);
  for(auto& image:model.images) {image.image={1,2,3,4};image.as_is=true;}
  model.textures.resize(1);model.textures[0].source=0;declare(model,"EXT_texture_avif");
  model.textures[0].extensions["EXT_texture_avif"]=V(O{{"source",V(1)},{"extras",V(O{{"material",V(1)}})}});
  const auto expected=model.textures;const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1});
  ApplyGltfMaterialExportCompaction(model,plan);const auto& output=model;EXPECT_EQ(output.textures,expected);
  auto invalid=output;invalid.textures[0].extensions.at("EXT_texture_avif").Get<O>().at("source")=V(2);
  EXPECT_THROW((void)PlanGltfMaterialExportCompaction(invalid,{}),std::runtime_error);
}

TEST(GltfMaterialExportCompaction, MalformedPointersRawReferencesAndRetirementSetsReject) {
  for(const auto* path:{"/materials/01/value","/materials/-1/value","/materials/5/value","/materials/4/value~2","/materials","/meshes/0/primitives/0/material"}) {
    auto model=fixture();model.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get<O>().at("pointer")=V(std::string(path));
    EXPECT_THROW((void)PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1}),std::runtime_error);
  }
  for(const auto ids:std::array<std::vector<int32_t>,4>{{{1,1},{-1},{5},{INT32_MAX}}}) {
    const auto model=fixture();EXPECT_THROW((void)PlanGltfMaterialExportCompaction(model,ids),std::runtime_error);
  }
  auto model=fixture();model.materials[0].extensions.at("MSFT_lod").Get<O>().at("ids").Get<A>()[0]=V(2.0);
  EXPECT_THROW((void)PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1}),std::runtime_error);
}

TEST(GltfMaterialExportCompaction, FiniteCountMetadataWorkPayloadAndProofBudgetsPrecedeOutputMutation) {
  const auto model=fixture();
  for(const int kind:{0,1,2,3,4,5,6,7,8,9,10,11}) {
    GltfMaterialExportCompactionLimits limits;
    if(kind==0) limits.maxOwnedBytes=1;if(kind==1) limits.maxMetadataBytes=1;
    if(kind==2) limits.maxReferences=1;if(kind==3) limits.maxTableEntries=1;
    if(kind==4) limits.maxTraversalSteps=1;if(kind==5) limits.maxMetadataValues=1;
    if(kind==6) limits.maxResidentBytes=1;if(kind==7) limits.maxAccessorDecodedBytes=1;
    if(kind==8) limits.maxAnimationDecodedBytes=1;if(kind==9) limits.maxMetadataDepth=1;
    if(kind==10) limits.maxOwnedBytes=UINT64_MAX;if(kind==11) limits.maxTraversalSteps=0;
    EXPECT_THROW((void)PlanGltfMaterialExportCompaction(model,std::array<int32_t,1>{1},limits),std::runtime_error);
  }
  auto nonfinite=model;nonfinite.materials[0].extras=V(std::numeric_limits<double>::infinity());
  EXPECT_THROW((void)PlanGltfMaterialExportCompaction(nonfinite,std::array<int32_t,1>{1}),std::runtime_error);
  auto hidden=model;hidden.extras=V(0.);hidden.extras.Get<A>().push_back(V(std::string(10000,'x')));
  EXPECT_THROW((void)PlanGltfMaterialExportCompaction(hidden,std::array<int32_t,1>{1}),std::runtime_error);
}

TEST(GltfMaterialExportCompaction, EmptyRetirementPreservesExactStorageAndAllUnusedRowsCanRetire) {
  auto model=fixture();const auto before=model;const auto* payload=model.buffers[0].data.data();
  const auto plan=PlanGltfMaterialExportCompaction(model,{});ApplyGltfMaterialExportCompaction(model,plan);const auto& output=model;
  EXPECT_EQ(output,before);EXPECT_EQ(output.buffers[0].data.data(),payload);
  tinygltf::Model unused;unused.asset.version="2.0";unused.materials.resize(3);unused.materials[1].lods={1,2};
  const auto all=PlanGltfMaterialExportCompaction(unused,std::array<int32_t,3>{2,0,1});
  ApplyGltfMaterialExportCompaction(unused,all);EXPECT_TRUE(unused.materials.empty());
}

TEST(GltfMaterialExportCompaction, ActualAsciiAndGlbSaveReloadRetainRemappedReferencesAndBinaryGeometry) {
  Directory directory;
  for(const auto suffix:{".gltf",".glb"}) {
    SCOPED_TRACE(suffix);auto model=fixture();
    std::vector<unsigned char> png;std::string mime;
    ASSERT_TRUE(tinygltf::DecodeDataURI(&png,mime,"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aP1sAAAAASUVORK5CYII=",0,false));
    auto& bytes=model.buffers[0].data;tinygltf::BufferView imageView;imageView.buffer=0;imageView.byteOffset=bytes.size();imageView.byteLength=png.size();
    bytes.insert(bytes.end(),png.begin(),png.end());while(bytes.size()%4) bytes.push_back(0);
    model.images.resize(1);model.images[0].bufferView=int(model.bufferViews.size());model.images[0].mimeType="image/png";model.bufferViews.push_back(imageView);
    model.textures.resize(1);model.textures[0].source=0;model.materials[4].pbrMetallicRoughness.baseColorTexture.index=0;
    // Use a standard animatable suffix for semantic reload validation as well.
    model.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get<O>().at("pointer")=V(std::string("/materials/4/pbrMetallicRoughness/roughnessFactor"));
    const auto before=model;const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,2>{1,3});
    ApplyGltfMaterialExportCompaction(model,plan);const auto& output=model;const auto destination=directory.path/(std::string("compacted")+suffix);
    writeExportModel(output,destination);tinygltf::TinyGLTF loader;tinygltf::Model loaded;std::string error,warning;
    const bool okay=std::string_view(suffix)==".glb"?loader.LoadBinaryFromFile(&loaded,&error,&warning,utf8(destination)):loader.LoadASCIIFromFile(&loaded,&error,&warning,utf8(destination));
    ASSERT_TRUE(okay)<<error<<warning;ASSERT_EQ(loaded.materials.size(),3u);EXPECT_EQ(loaded.materials[2].name,"Row 4");
    EXPECT_EQ(loaded.meshes[0].primitives[0].material,2);EXPECT_EQ(loaded.materials[0].lods,(std::vector<int>{1,2}));
    EXPECT_EQ(text(loaded),"/materials/2/pbrMetallicRoughness/roughnessFactor");
    EXPECT_EQ(loaded.buffers[0].data,before.buffers[0].data);EXPECT_EQ(loaded.accessors,before.accessors);EXPECT_EQ(loaded.extras,before.extras);
    ASSERT_EQ(loaded.images.size(),1u);EXPECT_EQ(loaded.images[0].mimeType,"image/png");EXPECT_EQ(loaded.materials[2].pbrMetallicRoughness.baseColorTexture.index,0);
    const auto library=ParseAnimations(loaded);EXPECT_TRUE(library.diagnostics.empty());ASSERT_EQ(library.clips.size(),1u);
  }
}

TEST(GltfMaterialExportCompaction, SourceToRuntimeProjectionThenCompactionNeverUsesSourceIdsAsRuntimeIds) {
  using container::geometry::Model;using container::scene::SceneGraph;
  const auto geometry=Model::MakeCube();SceneGraph graph;
  const auto group=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Group");
  const auto leaf=graph.createNode(glm::mat4(1),0,true,0,"Cube");graph.setParent(leaf,group);graph.updateWorldTransforms();
  auto source=buildExportModel({graph,geometry});pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  variants(source,0);std::vector<tinygltf::Material> runtime(3);runtime[2]=source.materials[0];runtime[2].name="Source zero at runtime two";
  graph.setMaterialIndex(leaf,2);const std::array<int32_t,2> nodes{0,1};const std::array<int32_t,1> mapping{2};
  auto staged=buildExportModel({graph,geometry,&source,{},nodes,runtime,mapping});
  ASSERT_EQ(staged.meshes[0].primitives[0].material,2);ASSERT_EQ(text(staged),"/materials/2/pbrMetallicRoughness/roughnessFactor");
  const auto plan=PlanGltfMaterialExportCompaction(staged,std::array<int32_t,1>{1});
  ApplyGltfMaterialExportCompaction(staged,plan);const auto& output=staged;
  EXPECT_EQ(output.materials[1].name,"Source zero at runtime two");EXPECT_EQ(output.meshes[0].primitives[0].material,1);
  EXPECT_EQ(text(output),"/materials/1/pbrMetallicRoughness/roughnessFactor");
  EXPECT_EQ(output.meshes[0].primitives[0].extensions.at("KHR_materials_variants").Get("mappings").Get<A>()[0].Get("material").Get<int>(),1);
}

TEST(GltfMaterialExportCompaction, DenialAtEveryAllocationBoundaryLeavesInputExactUntilNoThrowCompaction) {
  size_t allocations{};
  {
    auto model=fixture();const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,2>{1,3});
    material_publication_test::AllocationGuard guard(false);
    ApplyGltfMaterialExportCompaction(model,plan);guard.disable();allocations=guard.allocations();
    ASSERT_EQ(model.materials.size(),3u);EXPECT_EQ(model.meshes[0].primitives[0].material,2);
  }
  // This includes all checking and replacement preparation. Every denied
  // allocation must precede ALL source edits. The final allowed boundary
  // executes the complete no-throw integer/string/material-assignment phase.
  ASSERT_GT(allocations,0u);ASSERT_LT(allocations,4096u);
  for(size_t allowance=0;allowance<=allocations;++allowance) {
    SCOPED_TRACE(allowance);auto model=fixture();const auto before=model;
    const auto plan=PlanGltfMaterialExportCompaction(model,std::array<int32_t,2>{1,3});bool denied{};
    material_publication_test::AllocationGuard guard(false,allowance);
    try {ApplyGltfMaterialExportCompaction(model,plan);} catch(const std::bad_alloc&) {denied=true;}
    guard.disable();
    if(allowance<allocations) {EXPECT_TRUE(denied);EXPECT_EQ(model,before);}
    else {EXPECT_FALSE(denied);EXPECT_EQ(model.materials.size(),3u);EXPECT_EQ(model.meshes[0].primitives[0].material,2);}
  }
}
