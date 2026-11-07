#include <Container/geometry/GltfMaterialAuthoring.h>
#include <Container/geometry/GltfMaterialExport.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/utility/GltfMaterialConversion.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace container::geometry::gltf;
using S=GltfMaterialTextureSlot;
using Op=GltfMaterialTextureOperation;
using V=tinygltf::Value;
using O=V::Object;
using A=V::Array;
constexpr const char* transform="KHR_texture_transform";
constexpr const char* sg="KHR_materials_pbrSpecularGlossiness";

tinygltf::Model basic(size_t textures=24) {
 tinygltf::Model result;result.asset.version="2.0";result.materials.emplace_back();result.textures.resize(textures);
 result.materials[0].name="Original";result.materials[0].extras=V(O{{"opaqueIndex",V(919)}});
 result.materials[0].extensions["VENDOR_opaque"]=V(O{{"index",V(777)}});
 result.extensionsUsed={transform,"VENDOR_opaque"};
 for(size_t extension=0;extension<size_t(GltfMaterialExtension::Count);++extension)
  result.extensionsUsed.emplace_back(GltfMaterialExtensionName(GltfMaterialExtension(extension)));
 return result;
}
GltfMaterialTextureEdit bind(S slot,int authored=1,int evaluated=2) {
 GltfMaterialTextureEdit result;result.slot=slot;result.operation=Op::Bind;
 result.binding=GltfMaterialTextureSourcePair{authored,evaluated};return result;
}
GltfMaterialTextureEdit mapping(S slot) {GltfMaterialTextureEdit result;result.slot=slot;result.operation=Op::Mapping;return result;}
GltfMaterialTextureEdit clear(S slot) {GltfMaterialTextureEdit result;result.slot=slot;result.operation=Op::Clear;return result;}
GltfMaterialPatch patch(GltfMaterialTextureEdit edit) {GltfMaterialPatch result;result.textures.push_back(std::move(edit));return result;}
void adopt(tinygltf::Model& model,const tinygltf::Material& material,const GltfMaterialAuthoringPlan& plan) {
 model.materials[0]=material;
 for(const auto& name:plan.extensionsToDeclare) if(std::ranges::find(model.extensionsUsed,name)==model.extensionsUsed.end()) model.extensionsUsed.push_back(name);
}
V info(int index) {return V(O{{"index",V(index)},{"extras",V(O{{"vendorIndex",V(404)}})}});}
void sharedSg(tinygltf::Model& model) {
 model.materials[0].extensions[sg]=V(O{{"specularGlossinessTexture",info(0)}});
}
void target(tinygltf::Model& model,const std::string& path) {
 model.animations.emplace_back();
 const int buffer=int(model.buffers.size()),view=int(model.bufferViews.size()),accessor=int(model.accessors.size());
 const std::array<float,4> values{0.f,1.f,.25f,.75f};tinygltf::Buffer data;data.data.resize(sizeof(values));
 std::memcpy(data.data.data(),values.data(),sizeof(values));model.buffers.push_back(std::move(data));
 for(size_t offset:{size_t(0),size_t(8)}) {tinygltf::BufferView item;item.buffer=buffer;item.byteOffset=offset;item.byteLength=8;model.bufferViews.push_back(item);}
 tinygltf::Accessor time;time.bufferView=view;time.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;time.type=TINYGLTF_TYPE_SCALAR;
 time.count=2;time.minValues={0.};time.maxValues={1.};auto value=time;value.bufferView=view+1;value.minValues={.25};value.maxValues={.75};
 model.accessors.push_back(time);model.accessors.push_back(value);tinygltf::AnimationSampler sampler;
 sampler.input=accessor;sampler.output=accessor+1;sampler.interpolation="LINEAR";model.animations[0].samplers.push_back(sampler);
 tinygltf::AnimationChannel channel;channel.target_path="pointer";channel.target_node=-1;channel.sampler=0;
 channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V("/materials/0/"+path)}});
 model.animations[0].channels.push_back(std::move(channel));model.extensionsUsed.push_back("KHR_animation_pointer");
}
}

TEST(GltfMaterialTextureAuthoring, RegistryHasAllStableSlotsRolesAndUniquePaths) {
 const auto registry=GltfMaterialTextureSlotRegistry();ASSERT_EQ(registry.size(),23u);
 std::set<std::string_view> paths;size_t colorRoles{};
 for(size_t i=0;i<registry.size();++i) {
  EXPECT_EQ(size_t(registry[i].slot),i);EXPECT_FALSE(registry[i].label.empty());EXPECT_TRUE(paths.insert(registry[i].path).second);
  colorRoles+=registry[i].srgb;EXPECT_EQ(&GltfMaterialTextureSlotDescription(S(i)),&registry[i]);
 }
 EXPECT_EQ(colorRoles,5u);EXPECT_EQ(registry[size_t(S::DiffuseTransmission)].channel,3u);
 EXPECT_THROW((void)GltfMaterialTextureSlotDescription(S::Count),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, EverySlotBindsIndependentAuthoredEvaluatedSourcesAndClears) {
 for(size_t i=0;i<size_t(S::Count);++i) {
  SCOPED_TRACE(i);const auto slot=S(i);auto a=basic(),e=a;
  auto edit=bind(slot,3,7);edit.mapping.texCoord=GltfMaterialTextureUIntValue{1u};
  edit.mapping.offset=GltfMaterialTextureVectorValue{std::array{.125,-.25}};
  const auto plan=PlanGltfMaterialEdit(a,e,0,patch(edit));
  EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,slot).index,3);
  const auto evaluated=ReadGltfMaterialTextureBinding(plan.evaluatedAfter,slot);
  EXPECT_EQ(evaluated.index,7);EXPECT_EQ(evaluated.texCoord,1u);EXPECT_EQ(evaluated.offset,(std::array{.125,-.25}));
  EXPECT_EQ(plan.authoredAfter.extras,a.materials[0].extras);
  EXPECT_EQ(plan.evaluatedAfter.extensions.at("VENDOR_opaque"),e.materials[0].extensions.at("VENDOR_opaque"));
  adopt(a,plan.authoredAfter,plan);adopt(e,plan.evaluatedAfter,plan);
  const auto removed=PlanGltfMaterialEdit(a,e,0,patch(clear(slot)));
  EXPECT_EQ(ReadGltfMaterialTextureBinding(removed.authoredAfter,slot).index,-1);
  EXPECT_EQ(ReadGltfMaterialTextureBinding(removed.evaluatedAfter,slot).index,-1);
  EXPECT_EQ(a.textures.size(),24u);EXPECT_EQ(e.textures.size(),24u);
 }
}

TEST(GltfMaterialTextureAuthoring, CoreBindingAndClearRemoveOnlySelectedRawMirrorsAndReplayExactAliases) {
 constexpr std::array coreSlots{S::BaseColor,S::Normal,S::Occlusion,S::Emissive,S::MetallicRoughness};
 constexpr std::array names{"baseColorTexture","normalTexture","occlusionTexture","emissiveTexture","metallicRoughnessTexture"};
 for(size_t selected=0;selected<coreSlots.size();++selected) for(bool clearing:{false,true}) {
  SCOPED_TRACE(names[selected]);
  SCOPED_TRACE(clearing);auto a=basic(),e=a;
  for(auto* model:{&a,&e}) {
   auto& material=model->materials[0];
   material.pbrMetallicRoughness.baseColorTexture.index=material.normalTexture.index=material.occlusionTexture.index=
    material.emissiveTexture.index=material.pbrMetallicRoughness.metallicRoughnessTexture.index=0;
   for(const auto* name:names) {
    auto& first=material.values[name];first.json_double_value={{"index",0},{"vendorSignedZero",-0.}};
    first.string_value="exact retained mirror payload";
    auto& second=material.additionalValues[name];second.json_double_value={{"index",3},{"texCoord",1},{"vendor",711}};
    second.number_array={.25,-0.,.75};
   }
   material.additionalValues["VENDOR_texture"].string_value="unrelated texture metadata";
  }
  const auto command=PlanGltfMaterialEdit(a,e,0,patch(clearing?clear(coreSlots[selected]):bind(coreSlots[selected],1,2)));
  for(const auto* material:{&command.authoredAfter,&command.evaluatedAfter}) {
   EXPECT_FALSE(material->values.contains(names[selected]));EXPECT_FALSE(material->additionalValues.contains(names[selected]));
   auto expected=a.materials[0];
   expected.values.erase(names[selected]);expected.additionalValues.erase(names[selected]);
   auto actual=*material;
   CopyGltfMaterialTextureEditLeaves(actual,expected,coreSlots[selected],TextureBinding|TextureContainer);
   EXPECT_TRUE(GltfMaterialRecordsEqual(actual,expected));
  }
  EXPECT_EQ(ReadGltfMaterialTextureBinding(command.authoredAfter,coreSlots[selected]).index,clearing?-1:1);
  EXPECT_EQ(ReadGltfMaterialTextureBinding(command.evaluatedAfter,coreSlots[selected]).index,clearing?-1:2);
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,
   GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_TRUE(GltfMaterialRecordsEqual(undo.authoredAfter,a.materials[0]));
  EXPECT_TRUE(GltfMaterialRecordsEqual(undo.evaluatedAfter,e.materials[0]));
  const auto redo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,
   GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_TRUE(GltfMaterialRecordsEqual(redo.authoredAfter,command.authoredAfter));
  EXPECT_TRUE(GltfMaterialRecordsEqual(redo.evaluatedAfter,command.evaluatedAfter));
 }
}

TEST(GltfMaterialTextureAuthoring, CoreMirrorDoesNotAddFallbackPrecedenceOrMappingOwnership) {
 auto a=basic();a.materials[0].values["normalTexture"].json_double_value={{"index",0},{"texCoord",1}};
 EXPECT_EQ(ReadGltfMaterialTextureBinding(a.materials[0],S::Normal).index,-1);
 auto edit=mapping(S::Normal);edit.mapping.rotation=GltfMaterialTextureScalarValue{.4};
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 a.materials[0].normalTexture.index=0;
 const auto command=PlanGltfMaterialEdit(a,a,0,patch(edit));
 EXPECT_EQ(command.authoredAfter.values,a.materials[0].values);
 auto current=command.evaluatedAfter;current.values["normalTexture"].json_double_value["vendor"]=42;
 const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,current,
  GltfMaterialSourceView(a),GltfMaterialSourceView(a));
 EXPECT_EQ(undo.evaluatedAfter.values,current.values);
}

TEST(GltfMaterialTextureAuthoring, SpecularGlossinessDiffuseEditPreservesIndependentCoreMirrors) {
 auto a=basic();a.materials[0].extensions[sg]=V(O{{"diffuseTexture",info(0)}});
 a.materials[0].pbrMetallicRoughness.baseColorTexture.index=3;
 a.materials[0].values["baseColorTexture"].json_double_value={{"index",3},{"vendor",41}};
 a.materials[0].additionalValues["baseColorTexture"].json_double_value={{"index",4}};
 for(bool clearing:{false,true}) {
  const auto command=PlanGltfMaterialEdit(a,a,0,patch(clearing?clear(S::BaseColor):bind(S::BaseColor,1,2)));
  EXPECT_EQ(command.authoredAfter.values,a.materials[0].values);EXPECT_EQ(command.authoredAfter.additionalValues,a.materials[0].additionalValues);
  EXPECT_EQ(command.authoredAfter.pbrMetallicRoughness.baseColorTexture.index,3);
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,
   GltfMaterialSourceView(a),GltfMaterialSourceView(a));
  EXPECT_TRUE(GltfMaterialRecordsEqual(undo.authoredAfter,a.materials[0]));
 }
}

TEST(GltfMaterialTextureAuthoring, ShadowedCoreMirrorsMustBeValidBeforeAnExplicitEdit) {
 constexpr std::array names{"baseColorTexture","normalTexture","occlusionTexture","emissiveTexture","metallicRoughnessTexture"};
 for(const auto* name:names) for(bool additional:{false,true}) {
  SCOPED_TRACE(name);
  SCOPED_TRACE(additional);auto a=basic();a.materials[0].normalTexture.index=0;
  auto& parameter=(additional?a.materials[0].additionalValues:a.materials[0].values)[name];
  parameter.json_double_value={{"index",999}};
  EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(bind(S::Normal,1,2))),std::invalid_argument);
  parameter.json_double_value["index"]=.5;
  EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(bind(S::Normal,1,2))),std::invalid_argument);
 }
}

TEST(GltfMaterialTextureAuthoring, BindingPreservesUnknownTextureMetadataAndIndependentFactors) {
 auto a=basic(),e=a;
 a.materials[0].normalTexture.index=e.materials[0].normalTexture.index=0;
 a.materials[0].normalTexture.scale=.2;e.materials[0].normalTexture.scale=.8;
 for(auto* model:{&a,&e}) {
  auto& normal=model->materials[0].normalTexture;normal.extras=V(O{{"opaque",V(501)}});
  normal.extensions["VENDOR_texture"]=V(O{{"index",V(502)}});normal.extensions_json_string="retained raw texture cache";
  model->materials[0].extensions_json_string="retained material cache";
 }
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(bind(S::Normal,4,9)));
 EXPECT_DOUBLE_EQ(plan.authoredAfter.normalTexture.scale,.2);EXPECT_DOUBLE_EQ(plan.evaluatedAfter.normalTexture.scale,.8);
 EXPECT_EQ(plan.authoredAfter.normalTexture.extras,a.materials[0].normalTexture.extras);
 EXPECT_EQ(plan.authoredAfter.normalTexture.extensions,a.materials[0].normalTexture.extensions);
 EXPECT_EQ(plan.authoredAfter.normalTexture.extensions_json_string,"retained raw texture cache");
 EXPECT_EQ(plan.authoredAfter.extensions_json_string,"retained material cache");
}

TEST(GltfMaterialTextureAuthoring, ClearAndUndoRetainCoreNormalScaleAndCurrentUnownedEvaluation) {
 auto a=basic(),e=a;a.materials[0].normalTexture.index=e.materials[0].normalTexture.index=0;
 a.materials[0].normalTexture.scale=.25;e.materials[0].normalTexture.scale=.5;
 const auto command=PlanGltfMaterialEdit(a,e,0,patch(clear(S::Normal)));
 EXPECT_DOUBLE_EQ(command.authoredAfter.normalTexture.scale,.25);EXPECT_DOUBLE_EQ(command.evaluatedAfter.normalTexture.scale,.5);
 auto current=command.evaluatedAfter;current.normalTexture.scale=.9;current.pbrMetallicRoughness.roughnessFactor=.37;
 const auto replay=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,current,
  GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 EXPECT_EQ(replay.evaluatedAfter.normalTexture.index,0);EXPECT_DOUBLE_EQ(replay.evaluatedAfter.normalTexture.scale,.9);
 EXPECT_DOUBLE_EQ(replay.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.37);
}

TEST(GltfMaterialTextureAuthoring, TexCoordEditOwnsBothSelectorsAndUndoRestoresExactOverrideAndCache) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  auto& texture=model->materials[0].pbrMetallicRoughness.baseColorTexture;texture.index=0;texture.texCoord=1;
  texture.extensions[transform]=V(O{{"texCoord",V(0)},{"offset",V(A{V(.25),V(-.5)})},{"vendor",V(3)}});
  texture.extensions_json_string="exact raw transform cache";
 }
 auto edit=mapping(S::BaseColor);edit.mapping.texCoord=GltfMaterialTextureUIntValue{1u};
 const auto command=PlanGltfMaterialEdit(a,e,0,patch(edit));
 const auto binding=ReadGltfMaterialTextureBinding(command.authoredAfter,S::BaseColor);
 EXPECT_EQ(binding.texCoord,1u);EXPECT_EQ(binding.offset,(std::array{.25,-.5}));
 EXPECT_FALSE(command.authoredAfter.pbrMetallicRoughness.baseColorTexture.extensions.at(transform).Has("texCoord"));
 const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,
  GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 EXPECT_TRUE(GltfMaterialRecordsEqual(undo.authoredAfter,a.materials[0]));
 EXPECT_TRUE(GltfMaterialRecordsEqual(undo.evaluatedAfter,e.materials[0]));
}

TEST(GltfMaterialTextureAuthoring, MappingResetPreservesUnknownLeavesAndTransformContainer) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  auto& value=model->materials[0].emissiveTexture;value.index=0;
  value.extensions[transform]=V(O{{"rotation",V(.4)},{"vendor",V(O{{"index",V(17)}})}});
 }
 auto edit=mapping(S::Emissive);edit.mapping.rotation=GltfMaterialTextureScalarValue{};
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(edit));
 const auto& value=plan.authoredAfter.emissiveTexture.extensions.at(transform);
 EXPECT_FALSE(value.Has("rotation"));EXPECT_TRUE(value.Has("vendor"));
 EXPECT_DOUBLE_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,S::Emissive).rotation,0.);
}

TEST(GltfMaterialTextureAuthoring, LegacyPrecedenceBindingAndClearCannotRevealOldFallback) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  auto& m=model->materials[0];m.values["heightTexture"].json_double_value={{"index",0},{"channel",2},{"texCoord",1},{"vendor",123}};
  m.additionalValues["heightTexture"].json_double_value={{"index",1}};
  m.values["displacementTexture"].json_double_value={{"index",2}};
  m.additionalValues["VENDOR_data"].string_value="preserved";
 }
 EXPECT_EQ(ReadGltfMaterialTextureBinding(a.materials[0],S::Height).index,0);
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(bind(S::Height,5,6)));
 EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,S::Height).channel,2u);
 EXPECT_EQ(plan.authoredAfter.values.at("heightTexture").json_double_value.at("vendor"),123.);
 EXPECT_FALSE(plan.authoredAfter.additionalValues.contains("heightTexture"));EXPECT_FALSE(plan.authoredAfter.values.contains("displacementTexture"));
 adopt(a,plan.authoredAfter,plan);adopt(e,plan.evaluatedAfter,plan);
 const auto removed=PlanGltfMaterialEdit(a,e,0,patch(clear(S::Height)));
 EXPECT_EQ(ReadGltfMaterialTextureBinding(removed.authoredAfter,S::Height).index,-1);
 EXPECT_EQ(removed.authoredAfter.additionalValues.at("VENDOR_data").string_value,"preserved");
}

TEST(GltfMaterialTextureAuthoring, ExistingStandardBindingStillValidatesIgnoredLegacyFallback) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  model->materials[0].extensions["KHR_materials_specular"]=V(O{{"specularTexture",info(0)}});
  model->materials[0].values["specularTexture"].json_double_value={{"index",999}};
 }
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,e,0,patch(bind(S::Specular,1,2))),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, StandardBindRemovesKnownLegacyFallbacksButNotFactorOrOpaqueKeys) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  auto& m=model->materials[0];m.extensions["KHR_materials_transmission"]=V(O{{"transmissionTexture",info(0)},{"transmissionFactor",V(.42)}});
  m.values["refractionTexture"].json_double_value={{"index",1}};
  m.additionalValues["transmissionTexture"].json_double_value={{"index",2}};
  m.values["refraction"].has_number_value=true;m.values["refraction"].number_value=.3;
 }
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(bind(S::Transmission,4,5)));
 EXPECT_FALSE(plan.authoredAfter.values.contains("refractionTexture"));EXPECT_FALSE(plan.authoredAfter.additionalValues.contains("transmissionTexture"));
 EXPECT_DOUBLE_EQ(plan.authoredAfter.extensions.at("KHR_materials_transmission").Get("transmissionFactor").GetNumberAsDouble(),.42);
 EXPECT_DOUBLE_EQ(plan.authoredAfter.values.at("refraction").number_value,.3);
 EXPECT_EQ(plan.authoredAfter.extensions.at("KHR_materials_transmission").Get("transmissionTexture").Get("extras"),info(0).Get("extras"));
}

TEST(GltfMaterialTextureAuthoring, SpecularGlossinessCouplesOneSourceFieldAcrossBothRoles) {
 auto a=basic(),e=a;sharedSg(a);sharedSg(e);
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(bind(S::Roughness,4,7)));
 for(const auto slot:{S::Roughness,S::SpecularColor}) {
  EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,slot).index,4);
  EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.evaluatedAfter,slot).index,7);
  EXPECT_NE(std::ranges::find(plan.touchedTextures,slot),plan.touchedTextures.end());
 }
 GltfMaterialPatch bad;bad.textures={bind(S::Roughness,1,2),bind(S::SpecularColor,3,4)};
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,e,0,bad),std::invalid_argument);
 bad.textures[1]=bind(S::SpecularColor,1,2);
 EXPECT_NO_THROW((void)PlanGltfMaterialEdit(a,e,0,bad));
}

TEST(GltfMaterialTextureAuthoring, ClearingSharedSpecularGlossinessCannotRevealLegacyRoughness) {
 auto a=basic(),e=a;sharedSg(a);sharedSg(e);
 for(auto* model:{&a,&e}) model->materials[0].values["roughnessTexture"].json_double_value={{"index",1}};
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(clear(S::SpecularColor)));
 EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,S::Roughness).index,-1);
 EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,S::SpecularColor).index,-1);
 EXPECT_FALSE(plan.authoredAfter.values.contains("roughnessTexture"));
}

TEST(GltfMaterialTextureAuthoring, ClearingAbsentSharedFieldDoesNotEraseIndependentLegacyRoughness) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  model->materials[0].extensions[sg]=V(O{});
  model->materials[0].values["roughnessTexture"].json_double_value={{"index",1}};
 }
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(clear(S::SpecularColor)));
 EXPECT_EQ(ReadGltfMaterialTextureBinding(plan.authoredAfter,S::Roughness).index,1);
 EXPECT_FALSE(plan.changed);
}

TEST(GltfMaterialTextureAuthoring, CoalescedMappingReplaysOnlyOwnedLeavesAtCurrentEvaluation) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) {
  auto& normal=model->materials[0].normalTexture;normal.index=0;
  normal.extensions[transform]=V(O{{"offset",V(A{V(0.),V(0.)})},{"rotation",V(.1)}});
 }
 auto edit=mapping(S::Normal);edit.mapping.offset=GltfMaterialTextureVectorValue{std::array{1.,2.}};
 const auto first=PlanGltfMaterialEdit(a,e,0,patch(edit));adopt(a,first.authoredAfter,first);adopt(e,first.evaluatedAfter,first);
 edit.mapping.offset=GltfMaterialTextureVectorValue{std::array{3.,4.}};
 const auto last=PlanGltfMaterialEdit(a,e,0,patch(edit));
 const auto command=CoalesceGltfMaterialEdit(first,last,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 auto current=last.evaluatedAfter;current.normalTexture.scale=.7;
 current.normalTexture.extensions.at(transform).Get<O>()["rotation"]=V(.9);
 const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,last.authoredAfter,current,
  GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 const auto restored=ReadGltfMaterialTextureBinding(undo.evaluatedAfter,S::Normal);
 EXPECT_EQ(restored.offset,(std::array{0.,0.}));EXPECT_DOUBLE_EQ(restored.rotation,.9);EXPECT_DOUBLE_EQ(undo.evaluatedAfter.normalTexture.scale,.7);
}

TEST(GltfMaterialTextureAuthoring, CoalescedClearThenBindRetainsContainerMetadataOwnership) {
 auto a=basic(),e=a;
 for(auto* model:{&a,&e}) model->materials[0].extensions["KHR_materials_specular"]=V(O{{"specularTexture",info(0)}});
 const auto originalA=a.materials[0],originalE=e.materials[0];
 const auto first=PlanGltfMaterialEdit(a,e,0,patch(clear(S::Specular)));adopt(a,first.authoredAfter,first);adopt(e,first.evaluatedAfter,first);
 const auto last=PlanGltfMaterialEdit(a,e,0,patch(bind(S::Specular,2,3)));
 const auto command=CoalesceGltfMaterialEdit(first,last,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,last.authoredAfter,last.evaluatedAfter,
  GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 EXPECT_TRUE(GltfMaterialRecordsEqual(undo.authoredAfter,originalA));EXPECT_TRUE(GltfMaterialRecordsEqual(undo.evaluatedAfter,originalE));
}

TEST(GltfMaterialTextureAuthoring, ClearCannotOrphanTextureAnimationTargets) {
 auto a=basic();a.materials[0].normalTexture.index=0;target(a,"normalTexture/scale");auto e=a;
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,e,0,patch(clear(S::Normal))),std::invalid_argument);
 EXPECT_NO_THROW((void)PlanGltfMaterialEdit(a,e,0,patch(bind(S::Normal,1,2))));
}

TEST(GltfMaterialTextureAuthoring, TransformResetRetainsAnimatedTransformObject) {
 auto a=basic();a.materials[0].normalTexture.index=0;
 a.materials[0].normalTexture.extensions[transform]=V(O{{"rotation",V(.4)}});
 target(a,"normalTexture/extensions/KHR_texture_transform/rotation");auto e=a;
 auto edit=mapping(S::Normal);edit.mapping.rotation=GltfMaterialTextureScalarValue{};
 const auto plan=PlanGltfMaterialEdit(a,e,0,patch(edit));
 ASSERT_TRUE(plan.authoredAfter.normalTexture.extensions.contains(transform));
 EXPECT_TRUE(plan.authoredAfter.normalTexture.extensions.at(transform).Get<O>().empty());
 EXPECT_EQ(plan.animationBindings.size(),1u);
}

TEST(GltfMaterialTextureAuthoring, FourDomainTransitionValidatesBeforeAndAfterIndependently) {
 auto a=basic(1),e=basic(2);a.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
 e.materials[0].pbrMetallicRoughness.baseColorTexture.index=1;
 auto futureA=a,futureE=e;futureA.textures.resize(2);futureE.textures.resize(4);
 const auto command=PlanGltfMaterialTextureEdit(GltfMaterialSourceView(a),GltfMaterialSourceView(e),
  GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE),0,a.materials[0],e.materials[0],patch(bind(S::BaseColor,1,3)));
 const auto witness=GltfMaterialAuthoringContext(command);
 EXPECT_TRUE(witness.resourceTransition);EXPECT_EQ(witness.authoredTextureCount,1u);EXPECT_EQ(witness.evaluatedTextureCount,2u);
 EXPECT_EQ(witness.afterAuthoredTextureCount,2u);EXPECT_EQ(witness.afterEvaluatedTextureCount,4u);
 EXPECT_NO_THROW(ValidateGltfMaterialEditTransition(command,a.materials[0],e.materials[0],GltfMaterialSourceView(a),GltfMaterialSourceView(e),
  GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE)));
 EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(command,GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE)),std::invalid_argument);
 EXPECT_THROW(ValidateGltfMaterialEditTransition(command,a.materials[0],e.materials[0],GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE),
  GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE)),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, ResourceTransitionRejectsBadBeforeEvenWhenItFitsProspectiveAfter) {
 auto a=basic(1),e=a;a.materials[0].emissiveTexture.index=e.materials[0].emissiveTexture.index=1;
 auto future=a;future.textures.resize(3);
 EXPECT_THROW((void)PlanGltfMaterialTextureEdit(GltfMaterialSourceView(a),GltfMaterialSourceView(e),
  GltfMaterialSourceView(future),GltfMaterialSourceView(future),0,a.materials[0],e.materials[0],patch(bind(S::Emissive,2,2))),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, ResourceTransitionRejectsAfterThatOnlyFitsTheOtherDomain) {
 auto a=basic(1),e=a,futureA=basic(2),futureE=basic(4);
 EXPECT_THROW((void)PlanGltfMaterialTextureEdit(GltfMaterialSourceView(a),GltfMaterialSourceView(e),
  GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE),0,a.materials[0],e.materials[0],patch(bind(S::BaseColor,3,3))),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, TransitionUndoShrinksDomainsAndRedoUsesFreshCurrentEvaluation) {
 auto a=basic(1),e=a;a.materials[0].pbrMetallicRoughness.baseColorTexture.index=e.materials[0].pbrMetallicRoughness.baseColorTexture.index=0;
 auto futureA=basic(2),futureE=basic(3);
 const auto command=PlanGltfMaterialTextureEdit(GltfMaterialSourceView(a),GltfMaterialSourceView(e),GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE),
  0,a.materials[0],e.materials[0],patch(bind(S::BaseColor,1,2)));
 auto current=command.evaluatedAfter;current.pbrMetallicRoughness.roughnessFactor=.17;
 const auto undo=RebaseGltfMaterialTextureEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,current,
  GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE),GltfMaterialSourceView(a),GltfMaterialSourceView(e));
 EXPECT_EQ(undo.evaluatedAfter.pbrMetallicRoughness.baseColorTexture.index,0);EXPECT_DOUBLE_EQ(undo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.17);
 EXPECT_NO_THROW(ValidateGltfMaterialEditTransition(undo,command.authoredAfter,current,GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE),
  GltfMaterialSourceView(a),GltfMaterialSourceView(e)));
 auto redoCurrent=undo.evaluatedAfter;redoCurrent.pbrMetallicRoughness.roughnessFactor=.83;
 const auto redo=RebaseGltfMaterialTextureEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,redoCurrent,
  GltfMaterialSourceView(a),GltfMaterialSourceView(e),GltfMaterialSourceView(futureA),GltfMaterialSourceView(futureE));
 EXPECT_EQ(redo.evaluatedAfter.pbrMetallicRoughness.baseColorTexture.index,2);EXPECT_DOUBLE_EQ(redo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.83);
}

TEST(GltfMaterialTextureAuthoring, TransitionReplayCanRetainOuterProvedAdditionalResourcePrefix) {
 auto before=basic(1),after=basic(2);before.materials[0].normalTexture.index=0;
 const auto command=PlanGltfMaterialTextureEdit(GltfMaterialSourceView(before),GltfMaterialSourceView(before),
  GltfMaterialSourceView(after),GltfMaterialSourceView(after),0,before.materials[0],before.materials[0],patch(bind(S::Normal,1,1)));
 auto currentDomain=basic(5),futureDomain=basic(5);
 const auto undo=RebaseGltfMaterialTextureEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,
  GltfMaterialSourceView(currentDomain),GltfMaterialSourceView(currentDomain),GltfMaterialSourceView(futureDomain),GltfMaterialSourceView(futureDomain));
 EXPECT_EQ(undo.evaluatedAfter.normalTexture.index,0);
 const auto witness=GltfMaterialAuthoringContext(undo);EXPECT_EQ(witness.authoredTextureCount,5u);EXPECT_EQ(witness.afterAuthoredTextureCount,5u);
}

TEST(GltfMaterialTextureAuthoring, PublicReceiptAndTransitionContextForgeryReject) {
 auto before=basic(1),after=basic(2);
 const auto command=PlanGltfMaterialTextureEdit(GltfMaterialSourceView(before),GltfMaterialSourceView(before),
  GltfMaterialSourceView(after),GltfMaterialSourceView(after),0,before.materials[0],before.materials[0],patch(bind(S::Normal,1,1)));
 auto altered=command;altered.touchedTextures.clear();
 EXPECT_THROW((void)GltfMaterialAuthoringContext(altered),std::invalid_argument);
 altered=command;altered.authoredAfter.normalTexture.index=0;
 EXPECT_THROW(ValidateGltfMaterialEditTransition(altered,before.materials[0],before.materials[0],GltfMaterialSourceView(before),GltfMaterialSourceView(before),
  GltfMaterialSourceView(after),GltfMaterialSourceView(after)),std::invalid_argument);
 auto stale=before.materials[0];stale.name="changed";
 EXPECT_THROW(ValidateGltfMaterialEditTransition(command,stale,before.materials[0],GltfMaterialSourceView(before),GltfMaterialSourceView(before),
  GltfMaterialSourceView(after),GltfMaterialSourceView(after)),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, BadShapesDuplicateSlotsMissingBindingsAndMappingsReject) {
 auto a=basic();
 GltfMaterialPatch duplicate;duplicate.textures={bind(S::Normal),bind(S::Normal)};
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,duplicate),std::invalid_argument);
 auto edit=bind(S::Normal);edit.binding.reset();EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 edit=clear(S::Normal);edit.binding=GltfMaterialTextureSourcePair{1,2};EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 edit=mapping(S::Normal);edit.mapping.texCoord=GltfMaterialTextureUIntValue{1u};EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 edit=bind(S::Normal);edit.slot=S::Count;EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 edit=bind(S::Normal);edit.operation=Op(255);EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, MappingRangesFixedChannelsAndFiniteWorkBudgetsReject) {
 auto a=basic();
 auto edit=bind(S::Normal);edit.mapping.texCoord=GltfMaterialTextureUIntValue{2u};EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 edit=bind(S::Normal);edit.mapping.channel=GltfMaterialTextureUIntValue{2u};EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 edit=bind(S::Height);edit.mapping.channel=GltfMaterialTextureUIntValue{2u};EXPECT_NO_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)));
 edit=bind(S::BaseColor);edit.mapping.offset=GltfMaterialTextureVectorValue{std::array{0.,std::numeric_limits<double>::infinity()}};
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(edit)),std::invalid_argument);
 GltfMaterialAuthoringLimits limits;limits.maxPatchOperations=1;
 GltfMaterialPatch two;two.textures={bind(S::Normal),bind(S::BaseColor)};
 EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,two,limits),std::invalid_argument);
 limits={};limits.maxOutputBytes=128;EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(bind(S::Normal)),limits),std::invalid_argument);
 limits={};limits.maxAnimationChannels=UINT64_MAX;EXPECT_THROW((void)PlanGltfMaterialEdit(a,a,0,patch(bind(S::Normal)),limits),std::invalid_argument);
}

TEST(GltfMaterialTextureAuthoring, SignedZeroMappingBitsAndMetadataSurviveUndo) {
 auto a=basic();a.materials[0].normalTexture.index=0;
 auto edit=mapping(S::Normal);edit.mapping.offset=GltfMaterialTextureVectorValue{std::array{-0.,.25}};
 const auto command=PlanGltfMaterialEdit(a,a,0,patch(edit));
 EXPECT_EQ(std::bit_cast<uint64_t>(ReadGltfMaterialTextureBinding(command.authoredAfter,S::Normal).offset[0]),std::bit_cast<uint64_t>(-0.));
 const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,
  GltfMaterialSourceView(a),GltfMaterialSourceView(a));
 EXPECT_TRUE(GltfMaterialRecordsEqual(undo.evaluatedAfter,a.materials[0]));
}

namespace {
struct TextureExportDirectory {
 std::filesystem::path parent=std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
 std::filesystem::path path;
 TextureExportDirectory() {
  static std::atomic<uint64_t> serial{};
  const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
  for(size_t attempt=0;attempt<32;++attempt) {
   path=parent/("container-texture-export-"+std::to_string(stamp)+"-"+std::to_string(serial++));
   if(std::filesystem::create_directory(path)) return;
  }
  throw std::runtime_error("Cannot create isolated texture export test directory");
 }
 ~TextureExportDirectory() {
  // The only recursively removed path is this directly created temporary child.
  if(path.parent_path()==parent && path.filename().string().starts_with("container-texture-export-")) {
   std::error_code error;std::filesystem::remove_all(path,error);
  }
 }
};
std::string pathUtf8(const std::filesystem::path& path) {
 const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};
}
std::string fileBytes(const std::filesystem::path& path) {
 std::ifstream input(path,std::ios::binary);if(!input) throw std::runtime_error("Cannot read texture export test output");
 return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void scalar(tinygltf::ParameterMap& map,const char* name,double value) {
 auto& parameter=map[name];parameter.has_number_value=true;parameter.number_value=value;
}
tinygltf::Model packedTextureModel() {
 auto result=basic(23);GltfMaterialPatch edits;
 for(size_t i=0;i<size_t(S::Count);++i) {
  auto edit=bind(S(i),int(i),int(i));edit.mapping.texCoord=GltfMaterialTextureUIntValue{1u};
  edit.mapping.offset=GltfMaterialTextureVectorValue{std::array{.125,-.25}};
  edit.mapping.scale=GltfMaterialTextureVectorValue{std::array{.5,1.5}};
  edit.mapping.rotation=GltfMaterialTextureScalarValue{.125};edits.textures.push_back(edit);
 }
 const auto plan=PlanGltfMaterialEdit(result,result,0,edits);adopt(result,plan.authoredAfter,plan);
 auto& material=result.materials[0];material.pbrMetallicRoughness.baseColorFactor={.25,.5,.75,.8};
 material.pbrMetallicRoughness.roughnessFactor=0;material.pbrMetallicRoughness.metallicFactor=0;
 // A stale raw standard factor must never shadow a current typed factor edit.
 scalar(material.values,"roughnessFactor",.91);
 scalar(material.values,"opacity",.5);scalar(material.additionalValues,"heightScale",.025);
 scalar(material.values,"heightOffset",-.2);scalar(material.values,"specular",.2);scalar(material.additionalValues,"refraction",.3);
 material.additionalValues["heightTexture"]=std::move(material.values.at("heightTexture"));material.values.erase("heightTexture");
 material.values["displacementTexture"].json_double_value={{"index",1},{"vendorFallback",901}};
 auto& height=material.additionalValues.at("heightTexture").json_double_value;
 height["vendorIndex"]=987;height["negativeZero"]=-0.;
 material.extras=V(O{{"opaqueIndex",V(919)},{"nested",V(A{V("kept"),V(O{{"value",V(1.25)}})})}});
 std::vector<unsigned char> png;std::string mime;
 if(!tinygltf::DecodeDataURI(&png,mime,"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aP1sAAAAASUVORK5CYII=",0,false))
  throw std::runtime_error("Cannot decode inline test PNG");
 result.buffers.emplace_back();result.buffers[0].data=png;
 while(result.buffers[0].data.size()%4) result.buffers[0].data.push_back(0);
 tinygltf::BufferView view;view.buffer=0;view.byteLength=png.size();result.bufferViews.push_back(view);
 for(size_t i=0;i<2;++i) {tinygltf::Image image;image.bufferView=0;image.mimeType="image/png";result.images.push_back(image);}
 result.samplers.emplace_back();for(size_t i=0;i<result.textures.size();++i) {result.textures[i].source=int(i%2);result.textures[i].sampler=0;}
 return result;
}
struct TextureRoles {
 std::array<uint32_t,23> color{},data{};
 TextureRoles() {color.fill(0);data.fill(1);}
 container::material::ConvertedGltfMaterial convert(const tinygltf::Material& material,const tinygltf::Model& model) const {
  return container::material::ConvertGltfMaterial(material,{model.textures.size(),model.extensionsUsed,color,data,2});
 }
};
tinygltf::Model reloadTextureModel(const std::filesystem::path& path) {
 tinygltf::TinyGLTF reader;tinygltf::Model result;std::string errors,warnings;
 const bool loaded=path.extension()==".glb"?reader.LoadBinaryFromFile(&result,&errors,&warnings,pathUtf8(path)):
  reader.LoadASCIIFromFile(&result,&errors,&warnings,pathUtf8(path));
 if(!loaded) throw std::runtime_error("Cannot reload actual project texture export: "+errors+warnings);return result;
}
uint32_t little32(const std::string& bytes,size_t offset) {
 if(offset>bytes.size() || bytes.size()-offset<4) throw std::runtime_error("Truncated test GLB");uint32_t result{};
 for(size_t i=0;i<4;++i) result|=uint32_t(static_cast<unsigned char>(bytes[offset+i]))<<(8*i);return result;
}
}

TEST(GltfMaterialTextureAuthoring, ActualAsciiAndGlbWritersRoundTripAll23SourceBindingsWithoutGpuInverse) {
 TextureExportDirectory directory;TextureRoles roles;
 for(const auto suffix:{".gltf",".glb"}) {
  SCOPED_TRACE(suffix);auto model=packedTextureModel();const auto original=model.materials[0];
  auto converted=roles.convert(original,model);converted.value.specularFactor=.6f;converted.value.transmissionFactor=.45f;
  const std::array<int32_t,2> deliberatelyUnresolvable{-1,-1};
  model.materials[0]=exportMaterial(converted.value,deliberatelyUnresolvable,&original,&converted.runtimeBindings);
  appendMaterialExtensionDeclarations(model);const auto path=directory.path/(std::string("all-slots")+suffix);
  ASSERT_NO_THROW(writeExportModel(model,path));const auto loaded=reloadTextureModel(path);ASSERT_EQ(loaded.materials.size(),1u);
  const auto& record=loaded.materials[0];const auto roundTrip=roles.convert(record,loaded);
  EXPECT_TRUE(container::material::RuntimeMaterialsEqual(roundTrip.value,converted.value));
  for(size_t i=0;i<size_t(S::Count);++i) {SCOPED_TRACE(i);EXPECT_EQ(roundTrip.sourceTextures[i],int(i));EXPECT_EQ(ReadGltfMaterialTextureBinding(record,S(i)).index,int(i));}
  EXPECT_EQ(record.extras,original.extras);EXPECT_EQ(record.extensions.at("VENDOR_opaque"),original.extensions.at("VENDOR_opaque"));
  EXPECT_FALSE(record.values.contains("heightTexture"));ASSERT_TRUE(record.additionalValues.contains("heightTexture"));
  const auto& metadata=record.additionalValues.at("heightTexture").json_double_value;
  EXPECT_DOUBLE_EQ(metadata.at("vendorIndex"),987);EXPECT_TRUE(std::signbit(metadata.at("negativeZero")));
  EXPECT_EQ(record.values.at("displacementTexture").json_double_value,original.values.at("displacementTexture").json_double_value);
  EXPECT_FLOAT_EQ(roundTrip.value.roughnessFactor,0);EXPECT_FLOAT_EQ(roundTrip.value.metallicFactor,0);
  EXPECT_FLOAT_EQ(roundTrip.value.specularFactor,.6f);EXPECT_FLOAT_EQ(roundTrip.value.transmissionFactor,.45f);
  EXPECT_FLOAT_EQ(roundTrip.value.opacityFactor,.5f);EXPECT_FLOAT_EQ(roundTrip.value.baseColor.a,.8f);
  EXPECT_FLOAT_EQ(roundTrip.value.baseColor.a*roundTrip.value.opacityFactor,.4f);
  EXPECT_FLOAT_EQ(roundTrip.value.heightScale,.025f);EXPECT_FLOAT_EQ(roundTrip.value.heightOffset,-.2f);
  EXPECT_DOUBLE_EQ(record.values.at("specular").number_value,.2);EXPECT_DOUBLE_EQ(record.additionalValues.at("refraction").number_value,.3);
 }
}

TEST(GltfMaterialTextureAuthoring, AppendResourcesRemapsEveryKnownLegacyAliasAtBothSourceLocations) {
 tinygltf::Model destination,source;destination.textures.resize(3);source.textures.resize(2);source.materials.emplace_back();
 auto& record=source.materials[0];size_t ordinal{};
 for(const auto name:KnownLegacyMaterialTextureFields()) {
  const double index=double(ordinal++%2);record.values[std::string(name)].json_double_value={{"index",index},{"vendor",71}};
  record.additionalValues[std::string(name)].json_double_value={{"index",1-index},{"vendor",72}};
 }
 record.values["VENDOR_texture"].json_double_value={{"index",999}};record.normalTexture.index=1;
 record.extensions["KHR_materials_specular"]=V(O{{"specularTexture",V(O{{"index",V(0)}})}});
 const auto materials=appendExportResources(destination,source,std::filesystem::path("source.gltf"));
 ASSERT_EQ(materials.size(),1u);EXPECT_EQ(destination.textures.size(),5u);ordinal=0;
 for(const auto name:KnownLegacyMaterialTextureFields()) {
  SCOPED_TRACE(name);const auto key=std::string(name);const double index=double(ordinal++%2);
  EXPECT_DOUBLE_EQ(materials[0].values.at(key).json_double_value.at("index"),3+index);
  EXPECT_DOUBLE_EQ(materials[0].additionalValues.at(key).json_double_value.at("index"),4-index);
  EXPECT_DOUBLE_EQ(materials[0].values.at(key).json_double_value.at("vendor"),71);
 }
 EXPECT_DOUBLE_EQ(materials[0].values.at("VENDOR_texture").json_double_value.at("index"),999);
 EXPECT_EQ(materials[0].normalTexture.index,4);EXPECT_EQ(materials[0].extensions.at("KHR_materials_specular").Get("specularTexture").Get("index").Get<int>(),3);
 EXPECT_EQ(source.textures.size(),2u);EXPECT_EQ(source.materials[0].normalTexture.index,1);
}

TEST(GltfMaterialTextureAuthoring, InvalidLegacyAppendRejectsBeforeDestinationResourceMutation) {
 tinygltf::Model destination,source;destination.textures.resize(3);source.textures.resize(2);source.materials.emplace_back();
 source.materials[0].additionalValues["heightTexture"].json_double_value={{"index",2}};
 EXPECT_THROW((void)appendExportResources(destination,source,std::filesystem::path("source.gltf")),std::runtime_error);
 EXPECT_EQ(destination.textures.size(),3u);EXPECT_TRUE(destination.buffers.empty());
}

TEST(GltfMaterialTextureAuthoring, CustomLegacyExportRequiresExactSourceAndRuntimeBindingProof) {
 TextureRoles roles;auto model=packedTextureModel();const auto& source=model.materials[0];const auto converted=roles.convert(source,model);
 const std::array<int32_t,2> noMapping{-1,-1};
 EXPECT_THROW((void)exportMaterial(converted.value,noMapping,&source),std::runtime_error);
 auto invalid=converted.runtimeBindings;invalid.runtimeIndices[size_t(S::Height)]=0;
 EXPECT_THROW((void)exportMaterial(converted.value,noMapping,&source,&invalid),std::runtime_error);
 auto missing=source;missing.additionalValues.erase("heightTexture");missing.values.erase("displacementTexture");
 EXPECT_THROW((void)exportMaterial(converted.value,noMapping,&missing,&converted.runtimeBindings),std::runtime_error);
 const auto exported=exportMaterial(converted.value,noMapping,&source,&converted.runtimeBindings);
 EXPECT_EQ(ReadGltfMaterialTextureBinding(exported,S::Height).index,int(S::Height));
}

TEST(GltfMaterialTextureAuthoring, LegacyPhysicalScalarChannelsRemainUnshadowedThroughActualWriter) {
 TextureExportDirectory directory;TextureRoles roles;auto model=packedTextureModel();auto& source=model.materials[0];
 source.extensions.at("KHR_materials_specular").Get<O>().erase("specularTexture");
 source.extensions.at("KHR_materials_specular").Get<O>().erase("specularColorTexture");
 source.extensions.at("KHR_materials_transmission").Get<O>().erase("transmissionTexture");
 source.values["specularTexture"].json_double_value={{"index",7},{"texCoord",1},{"channel",2},{"offsetX",.125},{"vendor",123}};
 source.additionalValues["refractionTexture"].json_double_value={{"index",10},{"texCoord",1},{"channel",1},{"rotation",.25}};
 const auto before=source;const auto converted=roles.convert(source,model);const std::array<int32_t,2> noMapping{-1,-1};
 model.materials[0]=exportMaterial(converted.value,noMapping,&before,&converted.runtimeBindings);
 const auto path=directory.path/"legacy-physical.glb";writeExportModel(model,path);const auto loaded=reloadTextureModel(path);
 const auto& record=loaded.materials[0];EXPECT_FALSE(record.extensions.at("KHR_materials_specular").Has("specularTexture"));
 EXPECT_FALSE(record.extensions.at("KHR_materials_specular").Has("specularColorTexture"));
 EXPECT_FALSE(record.extensions.at("KHR_materials_transmission").Has("transmissionTexture"));
 EXPECT_EQ(ReadGltfMaterialTextureBinding(record,S::Specular).channel,2u);EXPECT_EQ(ReadGltfMaterialTextureBinding(record,S::Transmission).channel,1u);
 EXPECT_DOUBLE_EQ(record.values.at("specularTexture").json_double_value.at("vendor"),123);
 EXPECT_TRUE(container::material::RuntimeMaterialsEqual(roles.convert(record,loaded).value,converted.value));
}

TEST(GltfMaterialTextureAuthoring, GlbCompatibilityProjectionPreservesEntireBinaryChunkStreamExactly) {
 TextureExportDirectory directory;auto model=packedTextureModel();tinygltf::TinyGLTF writer;const auto original=directory.path/"staged.glb";
 ASSERT_TRUE(writer.WriteGltfSceneToFile(&model,pathUtf8(original),false,false,true,true));const auto before=fileBytes(original);
 const auto replacement=StageLegacyMaterialSerialization(model,original,true);ASSERT_TRUE(replacement);const auto after=fileBytes(*replacement);
 EXPECT_EQ(little32(after,0),0x46546c67u);EXPECT_EQ(little32(after,4),2u);EXPECT_EQ(little32(after,8),after.size());
 EXPECT_EQ(little32(after,12)%4,0u);EXPECT_EQ(little32(after,16),0x4e4f534au);
 EXPECT_EQ(before.substr(20+little32(before,12)),after.substr(20+little32(after,12)));
 EXPECT_EQ(fileBytes(original),before);EXPECT_GT(after.size(),before.size());
}

TEST(GltfMaterialTextureAuthoring, LegacyWriterFiniteBudgetsAndBadReferencesKeepPublishedDestinationExact) {
 TextureExportDirectory directory;const auto path=directory.path/"existing.glb";{std::ofstream output(path,std::ios::binary);output<<"existing user destination";}
 const auto original=fileBytes(path);auto model=packedTextureModel();
 auto invalid=model;invalid.materials[0].additionalValues["heightTexture"].json_double_value["index"]=23;
 EXPECT_THROW(writeExportModel(invalid,path),std::runtime_error);EXPECT_EQ(fileBytes(path),original);
 for(size_t option=0;option<6;++option) {
  SCOPED_TRACE(option);GltfLegacyMaterialSerializationLimits limits;
  if(option==0) limits.maxParameterValues=1;if(option==1) limits.maxParameterBytes=1;if(option==2) limits.maxJsonBytes=1;
  if(option==3) limits.maxOwnedBytes=1;if(option==4) limits.maxJsonDepth=1;if(option==5) limits.maxJsonValues=1;
  EXPECT_THROW(writeExportModel(model,path,limits),std::runtime_error);EXPECT_EQ(fileBytes(path),original);
 }
 EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path),std::filesystem::directory_iterator()),1);
}

TEST(GltfMaterialTextureAuthoring, LegacySerializationRejectsAmbiguousShapesNonfiniteMetadataAndNonfiniteCapacities) {
 auto model=packedTextureModel();const auto original=model.materials[0];
 model.materials[0].additionalValues["heightTexture"].has_number_value=true;
 EXPECT_THROW((void)ValidateLegacyMaterialSerializationInput(model.materials,23),std::runtime_error);
 model.materials[0]=original;model.materials[0].additionalValues["heightTexture"].json_double_value["vendor"]=std::numeric_limits<double>::infinity();
 EXPECT_THROW((void)ValidateLegacyMaterialSerializationInput(model.materials,23),std::runtime_error);
 model.materials[0]=original;model.materials[0].additionalValues["heightTexture"].json_double_value[std::string("\xC0\x80",2)]=1;
 EXPECT_THROW((void)ValidateLegacyMaterialSerializationInput(model.materials,23),std::runtime_error);
 GltfLegacyMaterialSerializationLimits limits;limits.maxOwnedBytes=UINT64_MAX;
 EXPECT_THROW((void)ValidateLegacyMaterialSerializationInput(std::span<const tinygltf::Material>(&original,1),23,limits),std::runtime_error);
}

TEST(GltfMaterialTextureAuthoring, LegacyPreflightRejectsByteReservationsBeforeExportCopyAndBeforeKeyTraversal) {
 auto model=packedTextureModel();GltfLegacyMaterialSerializationLimits limits;limits.maxOwnedBytes=128*1024;
 EXPECT_THROW((void)ValidateLegacyMaterialSerializationInput(model.materials,23,limits),std::runtime_error);
 limits={};limits.maxJsonBytes=1;
 EXPECT_THROW((void)ValidateLegacyMaterialSerializationInput(model.materials,23,limits),std::runtime_error);
 tinygltf::Material selected;selected.values["heightTexture"].json_double_value={{"index",0},{std::string("\x01\xC0\x80",3),1}};
 limits={};limits.maxParameterBytes=sizeof(tinygltf::Parameter)+std::string_view("heightTexture").size();
 try {(void)ValidateLegacyMaterialSerializationInput(std::span<const tinygltf::Material>(&selected,1),1,limits);FAIL()<<"Expected finite key-byte reservation to reject";}
 catch(const std::runtime_error& error) {EXPECT_NE(std::string(error.what()).find("metadata exceeds its capacity"),std::string::npos);}
}

TEST(GltfMaterialTextureAuthoring, LegacyProjectionBoundsNewPbrAndTextureObjectDepth) {
 TextureExportDirectory directory;
 for(size_t location=0;location<3;++location) {
  SCOPED_TRACE(location);tinygltf::Model model;model.asset.version="2.0";model.materials.emplace_back();model.textures.emplace_back();
  model.textures[0].name="Depth fixture texture"; // TinyGLTF otherwise writes a null empty texture.
  auto& record=model.materials[0];auto& map=location==2?record.additionalValues:record.values;
  map["heightTexture"].json_double_value={{"index",0}};
  if(location==1) record.pbrMetallicRoughness.metallicFactor=.5;
  GltfLegacyMaterialSerializationLimits limits;limits.maxJsonDepth=location==2?3:4;
  const auto path=directory.path/("depth-"+std::to_string(location)+".gltf");
  EXPECT_THROW(writeExportModel(model,path,limits),std::runtime_error);EXPECT_FALSE(std::filesystem::exists(path));
  limits.maxJsonDepth=location==2?4:5;EXPECT_NO_THROW(writeExportModel(model,path,limits));
  const auto loaded=reloadTextureModel(path);ASSERT_EQ(loaded.materials.size(),1u);
  EXPECT_EQ(ReadGltfMaterialTextureBinding(loaded.materials[0],S::Height).index,0);
 }
}
