#include <Container/geometry/GltfMaterialAuthoring.h>

#include <gtest/gtest.h>

// One translation unit owns the shared allocation controls in this executable.
#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using P=GltfMaterialProperty;
using E=GltfMaterialExtension;
using V=tinygltf::Value;
using O=V::Object;
using A=V::Array;

tinygltf::Model basic() {
  tinygltf::Model model;model.materials.emplace_back();model.textures.resize(23);
  model.materials[0].name="Source";
  model.materials[0].extras=V(O{{"author",V(std::string("keep"))},{"rawIndex",V(909)}});
  model.materials[0].extensions["VENDOR_optional"]=V(O{{"opaqueIndex",V(313)}});
  model.extensionsUsed={"VENDOR_optional","KHR_texture_transform"};model.extensionsRequired={"VENDOR_optional"};
  return model;
}
void enable(tinygltf::Model& model,E extension,O body={}) {
  const auto name=std::string(GltfMaterialExtensionName(extension));model.materials[0].extensions[name]=V(std::move(body));
  if(std::ranges::find(model.extensionsUsed,name)==model.extensionsUsed.end()) model.extensionsUsed.push_back(name);
}
GltfMaterialPatch number(P property,double value) {return {{{property,value}}};}
GltfMaterialPatch rgb(P property,std::array<double,3> value) {return {{{property,value}}};}
double scalar(const tinygltf::Material& material,E extension,const char* key) {
  return material.extensions.at(std::string(GltfMaterialExtensionName(extension))).Get(key).GetNumberAsDouble();
}
V tex(int index) {
  return V(O{{"index",V(index)},{"texCoord",V(1)},{"extras",V(O{{"opaqueIndex",V(701)}})},
      {"extensions",V(O{{"KHR_texture_transform",V(O{{"offset",V(A{V(.25),V(-.5)})},
          {"scale",V(A{V(-2.),V(.75)})},{"rotation",V(.125)},{"texCoord",V(0)},
          {"extras",V(O{{"unknownTransformIndex",V(801)}})}})}})}});
}
void pointer(tinygltf::Model& source,std::string path) {
  if(source.animations.empty()) source.animations.emplace_back();
  const int buffer=int(source.buffers.size()),view=int(source.bufferViews.size()),accessor=int(source.accessors.size());
  tinygltf::Buffer data;const std::array<float,4> payload{0.f,1.f,.1f,.9f};data.data.resize(sizeof(payload));
  std::memcpy(data.data.data(),payload.data(),sizeof(payload));source.buffers.push_back(std::move(data));
  for(size_t offset:{size_t(0),size_t(8)}) {tinygltf::BufferView v;v.buffer=buffer;v.byteOffset=offset;v.byteLength=8;source.bufferViews.push_back(v);}
  tinygltf::Accessor input;input.bufferView=view;input.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  input.type=TINYGLTF_TYPE_SCALAR;input.count=2;input.minValues={0.};input.maxValues={1.};
  auto output=input;output.bufferView=view+1;output.minValues={double(.1f)};output.maxValues={double(.9f)};
  source.accessors.push_back(input);source.accessors.push_back(output);
  tinygltf::AnimationSampler sampler;sampler.input=accessor;sampler.output=accessor+1;sampler.interpolation="LINEAR";
  tinygltf::AnimationChannel channel;channel.target_node=-1;channel.target_path="pointer";channel.sampler=int(source.animations[0].samplers.size());
  source.animations[0].samplers.push_back(sampler);
  channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::move(path))}});
  source.animations[0].channels.push_back(channel);
  source.extensionsUsed.push_back("KHR_animation_pointer");
}

template<class Predicate> void matchesWithoutAllocations(Predicate&& predicate,bool expected) {
  bool matched{};size_t attempts{};
  {
    material_publication_test::AllocationGuard deny(true);
    matched=predicate();attempts=deny.allocations();
  }
  EXPECT_EQ(matched,expected);EXPECT_EQ(attempts,0u);
}
tinygltf::Model physical() {
  auto model=basic();
  for(size_t e=1;e<size_t(E::SpecularGlossiness);++e) enable(model,E(e));
  return model;
}
} // namespace

TEST(GltfMaterialAuthoring, TypedRegistryIsCompleteStableAndUnique) {
  const auto registry=GltfMaterialPropertyRegistry();ASSERT_EQ(registry.size(),32u);
  std::set<std::string_view> paths;
  for(size_t i=0;i<registry.size();++i) {
    EXPECT_EQ(size_t(registry[i].property),i);EXPECT_TRUE(paths.insert(registry[i].path).second);
    EXPECT_TRUE(registry[i].components==1 || registry[i].components==3 || registry[i].components==4);
  }
  EXPECT_EQ(registry[size_t(P::DiffuseTransmissionColor)].path,"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor");
  EXPECT_THROW(GltfMaterialExtensionName(E::Count),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, CoreFactorsAlphaSidednessNameAndMetadataRemainIndependent) {
  auto authored=basic(),evaluated=authored;
  authored.materials[0].pbrMetallicRoughness.baseColorFactor={.1,.2,.3,.4};
  evaluated.materials[0].pbrMetallicRoughness.baseColorFactor={.5,.6,.7,.8};
  authored.materials[0].pbrMetallicRoughness.metallicFactor=.2;
  evaluated.materials[0].pbrMetallicRoughness.metallicFactor=.8;
  const auto originalA=authored,originalE=evaluated;
  GltfMaterialPatch patch;patch.values={{P::Roughness,.25},{P::AlphaCutoff,2.5},{P::Emissive,std::array{.2,.3,.4}}};
  patch.alphaMode=GltfMaterialAlphaMode::Mask;patch.doubleSided=true;patch.name="Edited material";
  const auto plan=PlanGltfMaterialEdit(authored,evaluated,0,patch);
  EXPECT_EQ(plan.kind,GltfMaterialPlanKind::Edit);EXPECT_EQ(plan.sourceMaterial,0);EXPECT_EQ(plan.destinationMaterial,0);EXPECT_TRUE(plan.changed);
  EXPECT_EQ(plan.authoredBefore,originalA.materials[0]);EXPECT_EQ(plan.evaluatedBefore,originalE.materials[0]);
  EXPECT_EQ(plan.authoredAfter.pbrMetallicRoughness.baseColorFactor,(std::vector<double>{.1,.2,.3,.4}));
  EXPECT_EQ(plan.evaluatedAfter.pbrMetallicRoughness.baseColorFactor,(std::vector<double>{.5,.6,.7,.8}));
  EXPECT_DOUBLE_EQ(plan.authoredAfter.pbrMetallicRoughness.metallicFactor,.2);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.8);
  for(const auto* m:{&plan.authoredAfter,&plan.evaluatedAfter}) {
    EXPECT_DOUBLE_EQ(m->pbrMetallicRoughness.roughnessFactor,.25);EXPECT_DOUBLE_EQ(m->alphaCutoff,2.5);
    EXPECT_EQ(m->alphaMode,"MASK");EXPECT_TRUE(m->doubleSided);EXPECT_EQ(m->name,"Edited material");
    EXPECT_EQ(m->extras,originalA.materials[0].extras);EXPECT_EQ(m->extensions.at("VENDOR_optional"),originalA.materials[0].extensions.at("VENDOR_optional"));
  }
  EXPECT_EQ(authored,originalA);EXPECT_EQ(evaluated,originalE);EXPECT_TRUE(plan.extensionsToDeclare.empty());
}

TEST(GltfMaterialAuthoring, EveryPhysicalFactorGroupUsesRawValidatedValuesWithoutNormalization) {
  auto model=physical();model.materials[0].normalTexture.index=0;model.materials[0].occlusionTexture.index=1;
  model.materials[0].extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatNormalTexture"]=tex(2);
  GltfMaterialPatch patch;
  patch.values={{P::BaseColor,std::array{.0,.25,.5,1.}}, {P::Metallic,.0}, {P::Roughness,1.}, {P::Emissive,std::array{.1,.2,.3}},
      {P::NormalScale,-2.}, {P::OcclusionStrength,.5}, {P::EmissiveStrength,3.}, {P::Ior,0.}, {P::Dispersion,2.},
      {P::Transmission,.4}, {P::Thickness,.7}, {P::AttenuationDistance,12.}, {P::AttenuationColor,std::array{.1,.2,.3}},
      {P::Clearcoat,.5}, {P::ClearcoatRoughness,.6}, {P::ClearcoatNormalScale,-3.}, {P::SheenColor,std::array{.2,.3,.4}},
      {P::SheenRoughness,.7}, {P::Specular,.8}, {P::SpecularColor,std::array{.4,.5,.6}}, {P::Iridescence,.9},
      {P::IridescenceIor,1.}, {P::IridescenceThicknessMinimum,800.}, {P::IridescenceThicknessMaximum,20.},
      {P::Anisotropy,1.}, {P::AnisotropyRotation,-17.}, {P::DiffuseTransmission,.0}, {P::DiffuseTransmissionColor,std::array{.0,.0,.0}}};
  const auto before=model;const auto plan=PlanGltfMaterialEdit(model,model,0,patch);
  EXPECT_EQ(model,before);EXPECT_EQ(plan.touchedProperties.size(),patch.values.size());
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::Ior,"ior"),0.);
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::Dispersion,"dispersion"),2.);
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::Iridescence,"iridescenceThicknessMinimum"),800.);
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::Iridescence,"iridescenceThicknessMaximum"),20.);
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::DiffuseTransmission,"diffuseTransmissionFactor"),0.);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.normalTexture.scale,-2.);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatNormalTexture").Get("scale").GetNumberAsDouble(),-3.);
}

TEST(GltfMaterialAuthoring, SpecularGlossinessFactorsAndEmissiveStrengthPreserveWorkflow) {
  auto model=basic();enable(model,E::SpecularGlossiness,{{"diffuseTexture",tex(1)},{"specularGlossinessTexture",tex(2)}});
  GltfMaterialPatch patch;
  patch.values={{P::SpecularGlossinessDiffuse,std::array{.1,.2,.3,.4}}, {P::SpecularGlossinessSpecular,std::array{.0,.5,1.}},
      {P::Glossiness,.25}, {P::EmissiveStrength,2.}};
  const auto plan=PlanGltfMaterialEdit(model,model,0,patch);
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::SpecularGlossiness,"glossinessFactor"),.25);
  EXPECT_EQ(plan.authoredAfter.extensions.at("KHR_materials_pbrSpecularGlossiness").Get("diffuseTexture"),tex(1));
  EXPECT_EQ(plan.extensionsToDeclare,(std::vector<std::string>{"KHR_materials_emissive_strength"}));
}

TEST(GltfMaterialAuthoring, ResetsRepresentOmittedInfinityAndKeepExplicitZeroAndBlack) {
  auto model=physical();
  model.materials[0].extensions["KHR_materials_volume"].Get<O>()["attenuationDistance"]=V(5.);
  model.materials[0].extensions["KHR_materials_anisotropy"].Get<O>()["anisotropyTexture"]=tex(0);
  model.materials[0].extensions["KHR_materials_diffuse_transmission"].Get<O>()["diffuseTransmissionColorTexture"]=tex(1);
  GltfMaterialPatch patch;patch.values={{P::AttenuationDistance,std::monostate{}},{P::Anisotropy,0.},
      {P::DiffuseTransmission,0.},{P::DiffuseTransmissionColor,std::array{0.,0.,0.}}};
  const auto plan=PlanGltfMaterialEdit(model,model,0,patch);
  EXPECT_FALSE(plan.authoredAfter.extensions.at("KHR_materials_volume").Has("attenuationDistance"));
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::Anisotropy,"anisotropyStrength"),0.);
  EXPECT_EQ(plan.authoredAfter.extensions.at("KHR_materials_diffuse_transmission").Get("diffuseTransmissionColorFactor"),V(A{V(0.),V(0.),V(0.)}));
  auto empty=basic();const auto reset=PlanGltfMaterialEdit(empty,empty,0,{{{P::AttenuationDistance,std::monostate{}}}});
  EXPECT_FALSE(reset.changed);EXPECT_FALSE(reset.authoredAfter.extensions.contains("KHR_materials_volume"));
}

TEST(GltfMaterialAuthoring, PreservesTwentyThreeDistinctTextureProvenanceRecordsAndOpaqueMetadata) {
  auto model=physical();auto& material=model.materials[0];
  int index=0;
  for(auto* info:{static_cast<tinygltf::TextureInfo*>(&material.pbrMetallicRoughness.baseColorTexture),
      &material.pbrMetallicRoughness.metallicRoughnessTexture,&material.emissiveTexture}) {
    info->index=index++;info->texCoord=1;info->extras=V(O{{"index",V(700+index)}});
  }
  material.normalTexture.index=index++;material.occlusionTexture.index=index++;
  const std::pair<E,const char*> fields[]={{E::Specular,"specularTexture"},{E::Specular,"specularColorTexture"},
      {E::Transmission,"transmissionTexture"},{E::Volume,"thicknessTexture"},{E::Clearcoat,"clearcoatTexture"},
      {E::Clearcoat,"clearcoatRoughnessTexture"},{E::Clearcoat,"clearcoatNormalTexture"},{E::Sheen,"sheenColorTexture"},
      {E::Sheen,"sheenRoughnessTexture"},{E::Iridescence,"iridescenceTexture"},{E::Iridescence,"iridescenceThicknessTexture"},
      {E::Anisotropy,"anisotropyTexture"},{E::DiffuseTransmission,"diffuseTransmissionTexture"},{E::DiffuseTransmission,"diffuseTransmissionColorTexture"}};
  for(const auto& [extension,key]:fields) material.extensions[std::string(GltfMaterialExtensionName(extension))].Get<O>()[key]=tex(index++);
  // Nineteen standard physical texture infos plus four opaque vendor bindings
  // use all23 source texture identities. No runtime slot identity is inferred.
  for(int extra=0;extra<4;++extra) material.extensions["VENDOR_optional"].Get<O>()["texture"+std::to_string(extra)]=tex(index++);
  ASSERT_EQ(index,23);const auto before=model;
  const auto plan=PlanGltfMaterialEdit(model,model,0,number(P::Roughness,.33));
  auto expected=before.materials[0];expected.pbrMetallicRoughness.roughnessFactor=.33;
  EXPECT_EQ(plan.authoredAfter,expected);EXPECT_EQ(plan.evaluatedAfter,expected);EXPECT_EQ(model,before);
}

TEST(GltfMaterialAuthoring, DoesNotReplaceOldRestWithAnAnimatedEvaluatedValue) {
  auto authored=basic();enable(authored,E::Anisotropy,{{"anisotropyStrength",V(.1)}});
  pointer(authored,"/materials/0/extensions/KHR_materials_anisotropy/anisotropyStrength");
  authored.materials[0].pbrMetallicRoughness.metallicFactor=.2;auto evaluated=authored;
  evaluated.materials[0].extensions["KHR_materials_anisotropy"].Get<O>()["anisotropyStrength"]=V(.9);
  evaluated.materials[0].pbrMetallicRoughness.metallicFactor=.8;
  const auto a=authored,e=evaluated;const auto plan=PlanGltfMaterialEdit(authored,evaluated,0,number(P::Roughness,.3));
  EXPECT_DOUBLE_EQ(scalar(plan.authoredAfter,E::Anisotropy,"anisotropyStrength"),.1);
  EXPECT_DOUBLE_EQ(scalar(plan.evaluatedAfter,E::Anisotropy,"anisotropyStrength"),.9);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.pbrMetallicRoughness.metallicFactor,.2);
  EXPECT_DOUBLE_EQ(plan.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.8);
  ASSERT_EQ(plan.animationBindings.size(),1u);EXPECT_EQ(plan.animationBindings[0].property,P::Anisotropy);
  EXPECT_EQ(authored,a);EXPECT_EQ(evaluated,e);
  const auto touched=PlanGltfMaterialEdit(authored,evaluated,0,number(P::Anisotropy,.4));
  EXPECT_DOUBLE_EQ(scalar(touched.authoredAfter,E::Anisotropy,"anisotropyStrength"),.4);
  EXPECT_DOUBLE_EQ(scalar(touched.evaluatedAfter,E::Anisotropy,"anisotropyStrength"),.4);
  EXPECT_EQ(authored.animations,a.animations);EXPECT_EQ(evaluated.animations,e.animations);
}

TEST(GltfMaterialAuthoring, AnimatedExtensionRemovalRejectsNestedTextureTransformsAndSecondSideTargets) {
  auto authored=basic();enable(authored,E::Clearcoat,{{"clearcoatTexture",tex(0)}});
  pointer(authored,"/materials/0/extensions/KHR_materials_clearcoat/clearcoatTexture/extensions/KHR_texture_transform/offset/1");
  auto evaluated=authored;GltfMaterialPatch off;off.extensions={{E::Clearcoat,false}};
  const auto before=authored;
  EXPECT_THROW(PlanGltfMaterialEdit(authored,evaluated,0,off),std::invalid_argument);EXPECT_EQ(authored,before);
  authored.animations.clear();EXPECT_THROW(PlanGltfMaterialEdit(authored,evaluated,0,off),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, DuplicateSharesResourcesAndKeepsDistinctRestWithoutCopyingTracks) {
  auto a=basic();a.materials[0].pbrMetallicRoughness.baseColorTexture.index=7;
  pointer(a,"/materials/0/pbrMetallicRoughness/roughnessFactor");auto e=a;
  a.materials[0].pbrMetallicRoughness.roughnessFactor=.2;e.materials[0].pbrMetallicRoughness.roughnessFactor=.8;
  const auto oldA=a,oldE=e;GltfMaterialPatch patch;patch.name="Source copy";
  const auto plan=PlanGltfMaterialDuplicate(a,e,0,patch);
  EXPECT_EQ(plan.kind,GltfMaterialPlanKind::Duplicate);EXPECT_EQ(plan.sourceMaterial,0);EXPECT_EQ(plan.destinationMaterial,1);
  EXPECT_EQ(plan.originalMaterialCount,1u);EXPECT_EQ(plan.authoredAfter.pbrMetallicRoughness.baseColorTexture.index,7);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.pbrMetallicRoughness.roughnessFactor,.2);EXPECT_DOUBLE_EQ(plan.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.8);
  ASSERT_EQ(plan.animationBindings.size(),1u);EXPECT_EQ(plan.animationBindings[0].clip,0u);EXPECT_EQ(plan.animationBindings[0].channel,0u);
  EXPECT_EQ(a,oldA);EXPECT_EQ(e,oldE);EXPECT_EQ(a.animations[0].channels.size(),1u);
}

TEST(GltfMaterialAuthoring, ExplicitPairAppendSupportsProceduralAndImplicitMaterialization) {
  tinygltf::Model authored,evaluated;tinygltf::Material a,e;a.pbrMetallicRoughness.metallicFactor=.2;e.pbrMetallicRoughness.metallicFactor=.8;
  GltfMaterialPatch patch;patch.values={{P::DiffuseTransmission,.4}};
  const auto plan=PlanGltfMaterialAppend(authored,evaluated,a,e,patch);
  EXPECT_EQ(plan.kind,GltfMaterialPlanKind::Append);EXPECT_FALSE(plan.sourceMaterial);EXPECT_EQ(plan.destinationMaterial,0);
  EXPECT_DOUBLE_EQ(plan.authoredAfter.pbrMetallicRoughness.metallicFactor,.2);EXPECT_DOUBLE_EQ(plan.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.8);
  EXPECT_EQ(plan.extensionsToDeclare,(std::vector<std::string>{"KHR_materials_diffuse_transmission"}));
  EXPECT_TRUE(authored.materials.empty());EXPECT_TRUE(evaluated.materials.empty());
  EXPECT_THROW(PlanGltfMaterialEdit(authored,evaluated,-1,{}),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, ExtensionDeclarationsOnlyAppendAndNoOpPreservesRawCaches) {
  auto model=basic();enable(model,E::Clearcoat,{{"clearcoatFactor",V(.5)}});
  model.materials[0].extensions_json_string="opaque retained cache";
  const auto original=model;
  const auto noOp=PlanGltfMaterialEdit(model,model,0,number(P::Clearcoat,.5));
  EXPECT_FALSE(noOp.changed);
  EXPECT_EQ(noOp.authoredAfter.extensions_json_string,"opaque retained cache");
  EXPECT_EQ(noOp.evaluatedAfter.extensions_json_string,"opaque retained cache");
  const auto untouched=PlanGltfMaterialDuplicate(model,model,0);
  EXPECT_EQ(untouched.authoredAfter,original.materials[0]);
  EXPECT_EQ(untouched.authoredAfter.extensions_json_string,"opaque retained cache");
  GltfMaterialPatch patch;patch.extensions={{E::Clearcoat,false},{E::Anisotropy,true}};
  const auto plan=PlanGltfMaterialEdit(model,model,0,patch);
  EXPECT_EQ(plan.extensionsToDeclare,(std::vector<std::string>{"KHR_materials_anisotropy"}));
  EXPECT_EQ(model,original);EXPECT_EQ(model.extensionsRequired,original.extensionsRequired);
  EXPECT_FALSE(plan.authoredAfter.extensions.contains("KHR_materials_clearcoat"));
  EXPECT_TRUE(plan.authoredAfter.extensions.contains("KHR_materials_anisotropy"));
}

TEST(GltfMaterialAuthoring, RejectsNonfiniteShapeRangesInvalidEnumsAndConflictingPatches) {
  auto model=basic();
  for(const double value:{-1.,1.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),std::numeric_limits<double>::max()}) {
    SCOPED_TRACE(value);EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::Metallic,value)),std::invalid_argument);
  }
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,rgb(P::BaseColor,{1.,1.,1.})),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::Ior,.99)),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::AttenuationDistance,0.)),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::AttenuationDistance,std::numeric_limits<double>::infinity())),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::Count,0.)),std::invalid_argument);
  GltfMaterialPatch patch;patch.values={{P::Roughness,.3},{P::Roughness,std::monostate{}}};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch),std::invalid_argument);
  patch={};patch.extensions={{E::Clearcoat,false}};patch.values={{P::Clearcoat,.3}};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch),std::invalid_argument);
  patch={};patch.extensions={{E::Count,true}};EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch),std::invalid_argument);
  patch={};patch.alphaMode=GltfMaterialAlphaMode(127);EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, RejectsMalformedRawLeavesTextureBindingsAndWorkflowExclusionsBeforeMutation) {
  auto good=basic();enable(good,E::Anisotropy);
  const std::vector<V> malformed={V(std::string("bad")),V(true),V(A{V(.2)}),V(std::numeric_limits<double>::infinity())};
  for(const auto& value:malformed) {
    auto model=good;model.materials[0].extensions["KHR_materials_anisotropy"].Get<O>()["anisotropyStrength"]=value;
    EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  }
  auto model=good;model.materials[0].pbrMetallicRoughness.baseColorFactor={1.,1.,1.};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  model=good;model.materials[0].normalTexture.index=99;EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  model=good;model.materials[0].normalTexture.index=0;model.materials[0].normalTexture.texCoord=2;
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  model=good;model.materials[0].extensions["KHR_materials_anisotropy"].Get<O>()["anisotropyTexture"]=V(O{{"index",V(.5)}});
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  GltfMaterialPatch unlit;unlit.extensions={{E::Unlit,true}};
  EXPECT_THROW(PlanGltfMaterialEdit(good,good,0,unlit),std::invalid_argument);
  GltfMaterialPatch sg;sg.extensions={{E::SpecularGlossiness,true}};
  EXPECT_THROW(PlanGltfMaterialEdit(good,good,0,sg),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, TextureScaleEditsNeverManufactureMissingTextureInfos) {
  auto model=basic();
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::NormalScale,.5)),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::OcclusionStrength,.5)),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,number(P::ClearcoatNormalScale,.5)),std::invalid_argument);
  enable(model,E::Clearcoat);GltfMaterialPatch patch;patch.extensions={{E::Clearcoat,true}};patch.values={{P::ClearcoatNormalScale,-1.}};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, BoundsCopiesAndOpaqueMetadataBeforePublishingAnyPlan) {
  auto model=basic();const auto before=model;GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=1;
  EXPECT_THROW(PlanGltfMaterialDuplicate(model,model,0,{},limits),std::invalid_argument);EXPECT_EQ(model,before);
  limits={};limits.maxPatchOperations=1;GltfMaterialPatch patch;patch.values={{P::Roughness,.5},{P::Metallic,.5}};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch,limits),std::invalid_argument);
  V nested(O{});for(int i=0;i<10;++i) nested=V(O{{"child",nested}});
  model.materials[0].extras=std::move(nested);limits={};limits.maxMetadataDepth=4;
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{},limits),std::invalid_argument);
  auto evaluated=basic();evaluated.materials.emplace_back();
  EXPECT_THROW(PlanGltfMaterialAppend(basic(),evaluated,{},{}),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, OneUlpAndSignedZeroEditsUseExactAuthoredRepresentation) {
  auto model=basic();model.materials[0].pbrMetallicRoughness.roughnessFactor=.5;
  const double adjacent=std::nextafter(.5,1.);
  const auto core=PlanGltfMaterialEdit(model,model,0,number(P::Roughness,adjacent));
  EXPECT_TRUE(core.changed);
  EXPECT_EQ(std::bit_cast<uint64_t>(core.authoredAfter.pbrMetallicRoughness.roughnessFactor),std::bit_cast<uint64_t>(adjacent));
  enable(model,E::Clearcoat,{{"clearcoatFactor",V(.5)}});
  const auto extension=PlanGltfMaterialEdit(model,model,0,number(P::Clearcoat,adjacent));
  EXPECT_TRUE(extension.changed);
  EXPECT_EQ(std::bit_cast<uint64_t>(scalar(extension.authoredAfter,E::Clearcoat,"clearcoatFactor")),std::bit_cast<uint64_t>(adjacent));
  model.materials[0].pbrMetallicRoughness.metallicFactor=0.;
  const auto zero=PlanGltfMaterialEdit(model,model,0,number(P::Metallic,-0.));
  EXPECT_TRUE(zero.changed);EXPECT_TRUE(std::signbit(zero.authoredAfter.pbrMetallicRoughness.metallicFactor));
  const auto negative=PlanGltfMaterialEdit(model,model,0,number(P::AnisotropyRotation,-0.));
  EXPECT_TRUE(negative.changed);EXPECT_TRUE(std::signbit(scalar(negative.authoredAfter,E::Anisotropy,"anisotropyRotation")));
}

TEST(GltfMaterialAuthoring, WorkflowSwitchRequiresExplicitCompatibleExtensionRemoval) {
  auto model=physical();model.materials[0].doubleSided=true;model.materials[0].alphaMode="BLEND";
  GltfMaterialPatch patch;
  for(size_t e=1;e<size_t(E::SpecularGlossiness);++e) patch.extensions.push_back({E(e),false});
  patch.extensions.push_back({E::Unlit,true});patch.alphaMode=GltfMaterialAlphaMode::Opaque;patch.doubleSided=false;
  const auto before=model;const auto plan=PlanGltfMaterialEdit(model,model,0,patch);
  EXPECT_EQ(plan.authoredAfter.alphaMode,"OPAQUE");EXPECT_FALSE(plan.authoredAfter.doubleSided);
  EXPECT_TRUE(plan.authoredAfter.extensions.contains("KHR_materials_unlit"));
  EXPECT_EQ(plan.authoredAfter.extensions.size(),2u);EXPECT_EQ(model,before);
  EXPECT_EQ(plan.extensionsToDeclare,(std::vector<std::string>{"KHR_materials_unlit"}));
}

TEST(GltfMaterialAuthoring, IndependentResourceSuffixesAndDeclarationsDoNotNormalizeBindings) {
  auto authored=basic(),evaluated=authored;authored.textures.emplace_back();
  authored.extensionsUsed.push_back("ARCHIVE_extra");
  authored.materials[0].normalTexture.index=22;evaluated.materials[0].normalTexture.index=21;
  const auto a=authored,e=evaluated;
  const auto plan=PlanGltfMaterialEdit(authored,evaluated,0,number(P::NormalScale,-.5));
  EXPECT_EQ(plan.authoredAfter.normalTexture.index,22);EXPECT_EQ(plan.evaluatedAfter.normalTexture.index,21);
  EXPECT_EQ(authored,a);EXPECT_EQ(evaluated,e);EXPECT_TRUE(plan.extensionsToDeclare.empty());
}

TEST(GltfMaterialAuthoring, WholeExtensionPointersAlsoBlockRemovalAndBindingsStaySourceIndexed) {
  auto model=basic();enable(model,E::Clearcoat);pointer(model,"/materials/0/extensions/KHR_materials_clearcoat");
  GltfMaterialPatch patch;patch.extensions={{E::Clearcoat,false}};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,patch),std::invalid_argument);
  model.materials.emplace_back();model.materials[1].extensions["KHR_materials_clearcoat"]=V(O{});
  model.animations[0].channels.clear();
  pointer(model,"/materials/1/extensions/KHR_materials_clearcoat/clearcoatFactor");
  const auto plan=PlanGltfMaterialEdit(model,model,0,patch);
  EXPECT_TRUE(plan.animationBindings.empty());EXPECT_FALSE(plan.authoredAfter.extensions.contains("KHR_materials_clearcoat"));
}

TEST(GltfMaterialAuthoring, MalformedRawColorsAlphaAndTextureTransformsReject) {
  auto model=physical();
  model.materials[0].extensions["KHR_materials_sheen"].Get<O>()["sheenColorFactor"]=V(A{V(.1),V(.2)});
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  model=basic();model.materials[0].alphaMode="DITHER";
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  model=basic();model.materials[0].normalTexture.index=0;
  model.materials[0].normalTexture.extensions["KHR_texture_transform"]=V(O{{"scale",V(A{V(1.)})}});
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,{}),std::invalid_argument);
  model=basic();GltfMaterialPatch duplicate;duplicate.extensions={{E::Unlit,true},{E::Unlit,false}};
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,duplicate),std::invalid_argument);
  GltfMaterialAuthoringLimits limits;limits.maxPatchOperations=1;GltfMaterialPatch flags;
  flags.alphaMode=GltfMaterialAlphaMode::Blend;flags.doubleSided=false;
  EXPECT_THROW(PlanGltfMaterialEdit(model,model,0,flags,limits),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, IndependentDuplicateCanRemoveSourceAnimatedExtensionWithoutNewTargets) {
  auto source=basic();enable(source,E::Clearcoat,{{"clearcoatFactor",V(.5)}});
  pointer(source,"/materials/0/extensions/KHR_materials_clearcoat/clearcoatFactor");const auto before=source;
  GltfMaterialPatch patch;patch.extensions={{E::Clearcoat,false}};
  EXPECT_THROW(PlanGltfMaterialEdit(source,source,0,patch),std::invalid_argument);
  const auto duplicate=PlanGltfMaterialDuplicate(source,source,0,patch);
  EXPECT_EQ(duplicate.destinationMaterial,1);EXPECT_FALSE(duplicate.authoredAfter.extensions.contains("KHR_materials_clearcoat"));
  ASSERT_EQ(duplicate.animationBindings.size(),1u);EXPECT_EQ(duplicate.animationBindings[0].property,P::Clearcoat);
  EXPECT_EQ(source,before);EXPECT_EQ(source.animations[0].channels.size(),1u);
  EXPECT_EQ(source.animations[0].channels[0].target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>(),
      "/materials/0/extensions/KHR_materials_clearcoat/clearcoatFactor");
}

TEST(GltfMaterialAuthoring, PairAppendReportsOpaqueAndNestedTextureDeclarationsWithoutCopyingSourceLists) {
  tinygltf::Model authored,evaluated;authored.textures.resize(1);evaluated.textures.resize(1);
  authored.extensionsUsed={"UNRELATED_source_a"};evaluated.extensionsUsed={"UNRELATED_source_b"};
  authored.extensionsRequired={"UNRELATED_source_a"};evaluated.extensionsRequired={"UNRELATED_source_b"};
  tinygltf::Material material;material.normalTexture.index=0;
  material.normalTexture.extensions["KHR_texture_transform"]=V(O{{"rotation",V(.25)}});
  material.normalTexture.extensions["VENDOR_texture"]=V(O{{"opaqueIndex",V(41)}});
  material.extensions["VENDOR_material"]=V(O{{"opaqueIndex",V(42)}});
  material.pbrMetallicRoughness.extensions["VENDOR_pbr"]=V(O{{"opaqueIndex",V(43)}});
  const auto a=authored,e=evaluated;
  const auto plan=PlanGltfMaterialAppend(authored,evaluated,material,material);
  EXPECT_EQ(plan.extensionsToDeclare,(std::vector<std::string>{"KHR_texture_transform","VENDOR_material","VENDOR_pbr","VENDOR_texture"}));
  EXPECT_EQ(plan.authoredAfter,material);EXPECT_EQ(plan.evaluatedAfter,material);
  EXPECT_EQ(authored,a);EXPECT_EQ(evaluated,e);
}

TEST(GltfMaterialAuthoring, NonowningViewsMatchModelPlanningAndExposeIndependentTrustedContexts) {
  auto a=basic(),e=a;a.textures.emplace_back();a.extensionsUsed.push_back("ARCHIVE_extra");
  const auto av=GltfMaterialSourceView(a),ev=GltfMaterialSourceView(e);
  EXPECT_EQ(av.extensionsUsed.data(),a.extensionsUsed.data());EXPECT_EQ(av.animations.data(),a.animations.data());
  const auto direct=PlanGltfMaterialEdit(a,e,0,number(P::Metallic,.25));
  const auto view=PlanGltfMaterialEdit(av,ev,0,a.materials[0],e.materials[0],number(P::Metallic,.25));
  EXPECT_EQ(view.authoredAfter,direct.authoredAfter);EXPECT_EQ(view.evaluatedAfter,direct.evaluatedAfter);
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(view,av,ev));
  const auto context=GltfMaterialAuthoringContext(view);
  EXPECT_EQ(context.materialCount,1u);EXPECT_EQ(context.authoredTextureCount,24u);EXPECT_EQ(context.evaluatedTextureCount,23u);
  EXPECT_EQ(std::ranges::find(context.authoredDeclarations,"ARCHIVE_extra"),context.authoredDeclarations.end());
  const auto duplicate=PlanGltfMaterialDuplicate(av,ev,0,a.materials[0],e.materials[0]);
  EXPECT_EQ(duplicate.destinationMaterial,1);EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(duplicate,av,ev));
  EXPECT_THROW(PlanGltfMaterialEdit(av,ev,1,a.materials[0],e.materials[0],{}),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, ImmutableReceiptRejectsDefaultSwappedAndEveryPublicIdentityOrMaskMutation) {
  auto source=basic();pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  const auto av=GltfMaterialSourceView(source);
  const auto original=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const auto copy=original;EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(copy,av,av));
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan({},av,av),std::invalid_argument);
  const std::vector<std::function<void(GltfMaterialAuthoringPlan&)>> mutations{
    [](auto& p){p.kind=GltfMaterialPlanKind::Duplicate;},[](auto& p){p.sourceMaterial.reset();},
    [](auto& p){p.destinationMaterial=1;},[](auto& p){++p.originalMaterialCount;},
    [](auto& p){p.touchedProperties.clear();},[](auto& p){p.touchedProperties.push_back(P::Metallic);},
    [](auto& p){p.touchedExtensions.push_back(E::Unlit);},[](auto& p){p.animationBindings[0].property=P::Metallic;},
    [](auto& p){p.animationBindings[0].channel=2;},[](auto& p){p.animationBindings[0].component=0;},
    [](auto& p){p.animationBindings[0].path="emissiveFactor";},[](auto& p){p.extensionsToDeclare.push_back("FORGED");},
    [](auto& p){p.changed=!p.changed;},[](auto& p){++p.peakOwnedBytes;}};
  for(size_t i=0;i<mutations.size();++i) {
    SCOPED_TRACE(i);auto forged=original;mutations[i](forged);
    EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(forged,av,av),std::invalid_argument);
    EXPECT_THROW(GltfMaterialAuthoringContext(forged),std::invalid_argument);
  }
  auto swapped=original;auto other=PlanGltfMaterialEdit(source,source,0,number(P::Metallic,.2));
  std::swap(swapped.authoredAfter,other.authoredAfter);
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(swapped,av,av),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, ReceiptRejectsBitChangesAcrossAllFourRecordsAndUntouchedMetadataTexturesCaches) {
  auto source=basic();source.materials[0].pbrMetallicRoughness.metallicFactor=0.;
  const auto plan=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.5));
  const auto view=GltfMaterialSourceView(source);
  for(int record=0;record<4;++record) {
    auto forged=plan;
    auto* material=record==0?&forged.authoredBefore:record==1?&forged.authoredAfter:
        record==2?&forged.evaluatedBefore:&forged.evaluatedAfter;
    material->pbrMetallicRoughness.roughnessFactor=std::nextafter(material->pbrMetallicRoughness.roughnessFactor,0.);
    EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(forged,view,view),std::invalid_argument);
  }
  const std::vector<std::function<void(tinygltf::Material&)>> mutations{
    [](auto& m){m.pbrMetallicRoughness.metallicFactor=-0.;},
    [](auto& m){m.normalTexture.index=0;},[](auto& m){m.extras.Get<O>()["rawIndex"]=V(910);},
    [](auto& m){m.extensions["VENDOR_optional"].Get<O>()["opaqueIndex"]=V(314);},
    [](auto& m){m.extensions_json_string="forged cache";},[](auto& m){m.extras_json_string="forged extras";},
    [](auto& m){m.pbrMetallicRoughness.extensions_json_string="forged pbr";},[](auto& m){m.lods.push_back(1);}};
  for(const auto& mutation:mutations) {auto forged=plan;mutation(forged.evaluatedAfter);
    EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(forged,view,view),std::invalid_argument);}
}

TEST(GltfMaterialAuthoring, ReceiptContextRejectsStaleCountsBindingsAndRemovedRelevantDeclarationsOnly) {
  auto source=basic();enable(source,E::Clearcoat);pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  const auto plan=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const auto original=source;
  auto changed=source;changed.textures.emplace_back();
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(plan,GltfMaterialSourceView(source),GltfMaterialSourceView(changed)),std::invalid_argument);
  changed=source;changed.materials.emplace_back();
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(plan,GltfMaterialSourceView(changed),GltfMaterialSourceView(source)),std::invalid_argument);
  changed=source;changed.animations[0].channels[0].target_extensions["KHR_animation_pointer"].Get<O>()["pointer"]=V(std::string("/materials/0/pbrMetallicRoughness/metallicFactor"));
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(plan,GltfMaterialSourceView(source),GltfMaterialSourceView(changed)),std::invalid_argument);
  changed=source;std::erase(changed.extensionsUsed,"KHR_materials_clearcoat");
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(plan,GltfMaterialSourceView(changed),GltfMaterialSourceView(source)),std::invalid_argument);
  changed=source;changed.extensionsUsed.push_back("LATER_global");changed.extensionsUsed.push_back("KHR_materials_anisotropy");
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(plan,GltfMaterialSourceView(changed),GltfMaterialSourceView(changed)));
  EXPECT_EQ(source,original);
}

TEST(GltfMaterialAuthoring, RebasePreservesCurrentClockUntouchedLeavesAndUsesOriginalCommandAcrossRepeatedReplay) {
  auto a=basic(),e=a;a.materials[0].pbrMetallicRoughness.roughnessFactor=.2;
  e.materials[0].pbrMetallicRoughness.roughnessFactor=.8;
  const auto command=PlanGltfMaterialEdit(a,e,0,number(P::Roughness,.3));
  auto currentA=command.authoredAfter,currentE=command.evaluatedAfter;
  currentE.pbrMetallicRoughness.metallicFactor=.9;currentE.emissiveFactor={.1,.2,.3};
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,currentA,currentE,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_DOUBLE_EQ(undo.authoredAfter.pbrMetallicRoughness.roughnessFactor,.2);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.8);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.9);
  EXPECT_EQ(undo.evaluatedAfter.emissiveFactor,currentE.emissiveFactor);
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(undo,GltfMaterialSourceView(a),GltfMaterialSourceView(e)));
  currentE=undo.evaluatedAfter;currentE.pbrMetallicRoughness.metallicFactor=.45;
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,currentE,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_DOUBLE_EQ(redo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.3);
  EXPECT_DOUBLE_EQ(redo.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.45);
  EXPECT_EQ(command.authoredBefore,a.materials[0]);EXPECT_EQ(command.evaluatedBefore,e.materials[0]);
  EXPECT_THROW(RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredBefore,currentE,GltfMaterialSourceView(a),GltfMaterialSourceView(e)),std::invalid_argument);
  EXPECT_THROW(RebaseGltfMaterialEdit(command,GltfMaterialEditSide(127),currentA,currentE,GltfMaterialSourceView(a),GltfMaterialSourceView(e)),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, RebaseNumericExtensionLeafPreservesCurrentOtherLeavesAndInvalidatesOnlyChangedCache) {
  auto a=basic();enable(a,E::Clearcoat,{{"clearcoatFactor",V(.1)},{"clearcoatRoughnessFactor",V(.2)},{"vendor",V(O{{"keep",V(77)}})}});
  a.materials[0].extensions_json_string="trusted original cache";auto e=a;
  const auto command=PlanGltfMaterialEdit(a,e,0,number(P::Clearcoat,.4));
  auto current=command.evaluatedAfter;current.extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatRoughnessFactor"]=V(.9);
  current.extensions_json_string="current cache";
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,current,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_DOUBLE_EQ(scalar(undo.evaluatedAfter,E::Clearcoat,"clearcoatFactor"),.1);
  EXPECT_DOUBLE_EQ(scalar(undo.evaluatedAfter,E::Clearcoat,"clearcoatRoughnessFactor"),.9);
  EXPECT_EQ(undo.evaluatedAfter.extensions.at("KHR_materials_clearcoat").Get("vendor"),V(O{{"keep",V(77)}}));
  EXPECT_TRUE(undo.evaluatedAfter.extensions_json_string.empty());
  EXPECT_EQ(undo.authoredAfter.extensions_json_string,"trusted original cache");
}

TEST(GltfMaterialAuthoring, RebaseRestoresExactRemovedExtensionMetadataTexturesRawTypesAndCaches) {
  auto a=basic();enable(a,E::Clearcoat,{{"clearcoatFactor",V(0)},{"clearcoatNormalTexture",tex(2)},{"vendor",V(O{{"opaque",V(-0.)}})}});
  a.materials[0].extensions_json_string="exact original extensions";auto e=a;
  e.materials[0].extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatFactor"]=V(.7);
  GltfMaterialPatch patch;patch.extensions={{E::Clearcoat,false}};
  const auto command=PlanGltfMaterialEdit(a,e,0,patch);
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_TRUE(undo.authoredAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").IsInt());
  EXPECT_EQ(undo.authoredAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatNormalTexture"),tex(2));
  EXPECT_EQ(undo.authoredAfter.extensions_json_string,"exact original extensions");
  EXPECT_EQ(undo.evaluatedAfter.extensions_json_string,"exact original extensions");
  EXPECT_DOUBLE_EQ(scalar(undo.evaluatedAfter,E::Clearcoat,"clearcoatFactor"),.7);
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,GltfMaterialSourceView(a),GltfMaterialSourceView(e));
  EXPECT_FALSE(redo.authoredAfter.extensions.contains("KHR_materials_clearcoat"));
  EXPECT_FALSE(redo.evaluatedAfter.extensions.contains("KHR_materials_clearcoat"));
}

TEST(GltfMaterialAuthoring, RebaseRemovesNumericCreatedContainerAndRetainsAppendedGlobalDeclaration) {
  auto a=basic();const auto command=PlanGltfMaterialEdit(a,a,0,number(P::Anisotropy,0.));
  auto published=a;published.materials[0]=command.authoredAfter;
  published.extensionsUsed.insert(published.extensionsUsed.end(),command.extensionsToDeclare.begin(),command.extensionsToDeclare.end());
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,published.materials[0],command.evaluatedAfter,GltfMaterialSourceView(published),GltfMaterialSourceView(published));
  EXPECT_FALSE(undo.authoredAfter.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_FALSE(undo.evaluatedAfter.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_TRUE(undo.extensionsToDeclare.empty());
  EXPECT_NE(std::ranges::find(published.extensionsUsed,"KHR_materials_anisotropy"),published.extensionsUsed.end());
  const auto duplicate=PlanGltfMaterialDuplicate(a,a,0);
  EXPECT_THROW(RebaseGltfMaterialEdit(duplicate,GltfMaterialEditSide::Before,duplicate.authoredAfter,duplicate.evaluatedAfter,GltfMaterialSourceView(a),GltfMaterialSourceView(a)),std::invalid_argument);
  const auto append=PlanGltfMaterialAppend(a,a,{},{});
  EXPECT_THROW(RebaseGltfMaterialEdit(append,GltfMaterialEditSide::Before,append.authoredAfter,append.evaluatedAfter,GltfMaterialSourceView(a),GltfMaterialSourceView(a)),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, ReceiptAndRebaseChargeStorageBeforeCopiesAndRespectReducedLimits) {
  auto source=basic();source.materials[0].extras=V(O{{"large",V(std::string(4096,'x'))}});
  const auto command=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=command.peakOwnedBytes-1;
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(command,GltfMaterialSourceView(source),GltfMaterialSourceView(source),limits),std::invalid_argument);
  EXPECT_THROW(PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3),limits),std::invalid_argument);
  limits={};limits.maxOutputBytes=command.peakOwnedBytes;
  EXPECT_THROW(RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,command.evaluatedAfter,GltfMaterialSourceView(source),GltfMaterialSourceView(source),limits),std::invalid_argument);
  limits={};limits.maxMetadataDepth=1;
  auto forged=command;forged.authoredAfter.extras.Get<O>()["large"]=V(std::string(10000,'y'));
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(forged,GltfMaterialSourceView(source),GltfMaterialSourceView(source),limits),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, ImmediateBeforeValidationAcceptsUndoDirectionAndRejectsStaleBits) {
  auto source=basic();source.materials[0].pbrMetallicRoughness.metallicFactor=0.;
  const auto command=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const auto view=GltfMaterialSourceView(source);
  EXPECT_NO_THROW(ValidateGltfMaterialEditBefore(command,source.materials[0],source.materials[0],view,view));
  auto stale=source.materials[0];stale.pbrMetallicRoughness.metallicFactor=-0.;
  EXPECT_THROW(ValidateGltfMaterialEditBefore(command,stale,source.materials[0],view,view),std::invalid_argument);
  stale=source.materials[0];stale.pbrMetallicRoughness.roughnessFactor=std::nextafter(1.,0.);
  EXPECT_THROW(ValidateGltfMaterialEditBefore(command,source.materials[0],stale,view,view),std::invalid_argument);
  auto evaluated=command.evaluatedAfter;evaluated.emissiveFactor={.1,.2,.3};
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,evaluated,view,view);
  EXPECT_NO_THROW(ValidateGltfMaterialEditBefore(undo,command.authoredAfter,evaluated,view,view));
  auto forged=undo;forged.authoredBefore.name="forged";
  EXPECT_THROW(ValidateGltfMaterialEditBefore(forged,forged.authoredBefore,evaluated,view,view),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, RebaseKeepsOneUlpAndSignedZeroCommandValuesAndRejectsOneUlpStaleRest) {
  auto source=basic();enable(source,E::Clearcoat,{{"clearcoatFactor",V(.5)}});
  source.materials[0].pbrMetallicRoughness.metallicFactor=0.;
  const auto adjacent=std::nextafter(.5,1.);
  GltfMaterialPatch patch;patch.values={{P::Clearcoat,adjacent},{P::Metallic,-0.}};
  patch.alphaMode=GltfMaterialAlphaMode::Mask;patch.name="Renamed";patch.doubleSided=true;
  const auto command=PlanGltfMaterialEdit(source,source,0,patch);
  auto current=command.evaluatedAfter;current.pbrMetallicRoughness.roughnessFactor=.9;
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,command.authoredAfter,current,
      GltfMaterialSourceView(source),GltfMaterialSourceView(source));
  EXPECT_FALSE(std::signbit(undo.evaluatedAfter.pbrMetallicRoughness.metallicFactor));
  EXPECT_EQ(std::bit_cast<uint64_t>(scalar(undo.evaluatedAfter,E::Clearcoat,"clearcoatFactor")),std::bit_cast<uint64_t>(.5));
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.9);
  EXPECT_EQ(undo.evaluatedAfter.alphaMode,"OPAQUE");EXPECT_EQ(undo.evaluatedAfter.name,"Source");EXPECT_FALSE(undo.evaluatedAfter.doubleSided);
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,
      GltfMaterialSourceView(source),GltfMaterialSourceView(source));
  EXPECT_TRUE(std::signbit(redo.evaluatedAfter.pbrMetallicRoughness.metallicFactor));
  EXPECT_EQ(std::bit_cast<uint64_t>(scalar(redo.evaluatedAfter,E::Clearcoat,"clearcoatFactor")),std::bit_cast<uint64_t>(adjacent));
  EXPECT_EQ(redo.evaluatedAfter.alphaMode,"MASK");EXPECT_EQ(redo.evaluatedAfter.name,"Renamed");EXPECT_TRUE(redo.evaluatedAfter.doubleSided);
  auto stale=command.authoredAfter;stale.extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatFactor"]=V(.5);
  EXPECT_THROW(RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,stale,current,
      GltfMaterialSourceView(source),GltfMaterialSourceView(source)),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, CoalesceDragBackKeepsActualClearedCacheAndReplaysBothExactSides) {
  auto source=basic();enable(source,E::Clearcoat,{{"clearcoatFactor",V(.5)}});
  source.materials[0].extensions_json_string="retained original JSON";
  const auto view=GltfMaterialSourceView(source);
  const auto first=PlanGltfMaterialEdit(source,source,0,number(P::Clearcoat,.8));
  const auto next=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,first.evaluatedAfter,number(P::Clearcoat,.5));
  const auto rebuilt=PlanGltfMaterialEdit(source,source,0,number(P::Clearcoat,.5));
  ASSERT_FALSE(rebuilt.changed);ASSERT_EQ(rebuilt.authoredAfter.extensions_json_string,"retained original JSON");
  ASSERT_TRUE(next.authoredAfter.extensions_json_string.empty());
  const auto cumulative=CoalesceGltfMaterialEdit(first,next,view,view);
  EXPECT_TRUE(cumulative.changed);
  EXPECT_EQ(cumulative.authoredBefore.extensions_json_string,"retained original JSON");
  EXPECT_TRUE(cumulative.authoredAfter.extensions_json_string.empty());
  EXPECT_TRUE(cumulative.evaluatedAfter.extensions_json_string.empty());
  EXPECT_DOUBLE_EQ(scalar(cumulative.authoredAfter,E::Clearcoat,"clearcoatFactor"),.5);
  EXPECT_NO_THROW(ValidateGltfMaterialEditBefore(next,first.authoredAfter,first.evaluatedAfter,view,view));
  // The cumulative command is for history, never an immediate next publication.
  EXPECT_THROW(ValidateGltfMaterialEditBefore(cumulative,next.authoredAfter,next.evaluatedAfter,view,view),std::invalid_argument);
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,next.authoredAfter,next.evaluatedAfter,view,view);
  EXPECT_EQ(undo.authoredAfter.extensions_json_string,"retained original JSON");
  EXPECT_EQ(undo.evaluatedAfter.extensions_json_string,"retained original JSON");
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,view,view);
  EXPECT_TRUE(redo.authoredAfter.extensions_json_string.empty());EXPECT_TRUE(redo.evaluatedAfter.extensions_json_string.empty());
  EXPECT_EQ(first.authoredBefore.extensions_json_string,"retained original JSON");
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(cumulative,view,view));
}

TEST(GltfMaterialAuthoring, CoalesceKeepsResetPresenceRawTypesSignedZeroAndOneUlpValues) {
  auto source=basic();enable(source,E::Clearcoat,{{"clearcoatFactor",V(0)}});
  source.materials[0].pbrMetallicRoughness.metallicFactor=0.;
  source.materials[0].pbrMetallicRoughness.roughnessFactor=.5;
  source.materials[0].extensions_json_string="original integer and omitted leaf";
  const auto view=GltfMaterialSourceView(source);const auto adjacent=std::nextafter(.5,1.);
  GltfMaterialPatch preview;preview.values={{P::Clearcoat,.3},{P::ClearcoatRoughness,.2},{P::Metallic,-0.},{P::Roughness,adjacent}};
  const auto first=PlanGltfMaterialEdit(source,source,0,preview);
  GltfMaterialPatch reset;reset.values={{P::Clearcoat,0.},{P::ClearcoatRoughness,std::monostate{}}};
  const auto next=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,first.evaluatedAfter,reset);
  const auto cumulative=CoalesceGltfMaterialEdit(first,next,view,view);
  ASSERT_TRUE(cumulative.authoredBefore.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").IsInt());
  ASSERT_TRUE(cumulative.authoredAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").IsReal());
  EXPECT_FALSE(cumulative.authoredAfter.extensions.at("KHR_materials_clearcoat").Has("clearcoatRoughnessFactor"));
  EXPECT_TRUE(std::signbit(cumulative.authoredAfter.pbrMetallicRoughness.metallicFactor));
  EXPECT_EQ(std::bit_cast<uint64_t>(cumulative.authoredAfter.pbrMetallicRoughness.roughnessFactor),std::bit_cast<uint64_t>(adjacent));
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,next.authoredAfter,next.evaluatedAfter,view,view);
  EXPECT_TRUE(undo.evaluatedAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").IsInt());
  EXPECT_FALSE(undo.evaluatedAfter.extensions.at("KHR_materials_clearcoat").Has("clearcoatRoughnessFactor"));
  EXPECT_FALSE(std::signbit(undo.evaluatedAfter.pbrMetallicRoughness.metallicFactor));
  EXPECT_EQ(std::bit_cast<uint64_t>(undo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor),std::bit_cast<uint64_t>(.5));
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,view,view);
  EXPECT_TRUE(redo.evaluatedAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").IsReal());
  EXPECT_FALSE(redo.evaluatedAfter.extensions.at("KHR_materials_clearcoat").Has("clearcoatRoughnessFactor"));
  EXPECT_TRUE(std::signbit(redo.evaluatedAfter.pbrMetallicRoughness.metallicFactor));
  EXPECT_EQ(std::bit_cast<uint64_t>(redo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor),std::bit_cast<uint64_t>(adjacent));
}

TEST(GltfMaterialAuthoring, CoalesceCreatedThenResetContainerRetainsBoundaryAndAppendOnlyDeclaration) {
  auto source=basic();const auto first=PlanGltfMaterialEdit(source,source,0,number(P::Anisotropy,.4));
  source.extensionsUsed.insert(source.extensionsUsed.end(),first.extensionsToDeclare.begin(),first.extensionsToDeclare.end());
  const auto view=GltfMaterialSourceView(source);
  GltfMaterialPatch reset;reset.values={{P::Anisotropy,std::monostate{}}};
  const auto next=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,first.evaluatedAfter,reset);
  const auto cumulative=CoalesceGltfMaterialEdit(first,next,view,view);
  ASSERT_TRUE(cumulative.authoredAfter.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_FALSE(cumulative.authoredAfter.extensions.at("KHR_materials_anisotropy").Has("anisotropyStrength"));
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,next.authoredAfter,next.evaluatedAfter,view,view);
  EXPECT_FALSE(undo.authoredAfter.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_FALSE(undo.evaluatedAfter.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_TRUE(undo.extensionsToDeclare.empty());
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,view,view);
  ASSERT_TRUE(redo.evaluatedAfter.extensions.contains("KHR_materials_anisotropy"));
  EXPECT_TRUE(redo.evaluatedAfter.extensions.at("KHR_materials_anisotropy").Get<O>().empty());
  EXPECT_NE(std::ranges::find(source.extensionsUsed,"KHR_materials_anisotropy"),source.extensionsUsed.end());
}

TEST(GltfMaterialAuthoring, CoalesceIntermediateToggleOwnsWholeObjectAcrossFurtherPreviewsAndMetadata) {
  auto authored=basic();enable(authored,E::Clearcoat,{{"clearcoatFactor",V(0)},{"clearcoatNormalTexture",tex(2)},
      {"vendor",V(O{{"raw",V(-0.)}})}});
  authored.materials[0].extensions_json_string="original object and textures";auto evaluated=authored;
  evaluated.materials[0].extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatFactor"]=V(.7);
  const auto av=GltfMaterialSourceView(authored),ev=GltfMaterialSourceView(evaluated);
  GltfMaterialPatch off;off.extensions={{E::Clearcoat,false}};
  const auto first=PlanGltfMaterialEdit(authored,evaluated,0,off);
  GltfMaterialPatch on;on.extensions={{E::Clearcoat,true}};on.name="Paint";on.alphaMode=GltfMaterialAlphaMode::Mask;on.doubleSided=true;
  const auto next=PlanGltfMaterialEdit(av,ev,0,first.authoredAfter,first.evaluatedAfter,on);
  const auto pair=CoalesceGltfMaterialEdit(first,next,av,ev);
  ASSERT_TRUE(pair.authoredBefore.extensions.contains("KHR_materials_clearcoat"));
  ASSERT_TRUE(pair.authoredAfter.extensions.contains("KHR_materials_clearcoat"));
  EXPECT_TRUE(pair.touchedProperties.empty());
  const auto third=PlanGltfMaterialEdit(av,ev,0,next.authoredAfter,next.evaluatedAfter,number(P::Clearcoat,.25));
  const auto cumulative=CoalesceGltfMaterialEdit(pair,third,av,ev);
  auto current=third.evaluatedAfter;current.pbrMetallicRoughness.roughnessFactor=.91;
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,third.authoredAfter,current,av,ev);
  const auto& restored=undo.evaluatedAfter.extensions.at("KHR_materials_clearcoat");
  EXPECT_EQ(restored.Get("clearcoatNormalTexture"),tex(2));
  EXPECT_TRUE(std::signbit(restored.Get("vendor").Get("raw").Get<double>()));
  EXPECT_DOUBLE_EQ(restored.Get("clearcoatFactor").Get<double>(),.7);
  EXPECT_TRUE(undo.authoredAfter.extensions.at("KHR_materials_clearcoat").Get("clearcoatFactor").IsInt());
  EXPECT_EQ(undo.authoredAfter.extensions_json_string,"original object and textures");
  EXPECT_EQ(undo.evaluatedAfter.name,"Source");EXPECT_EQ(undo.evaluatedAfter.alphaMode,"OPAQUE");EXPECT_FALSE(undo.evaluatedAfter.doubleSided);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.91);
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,av,ev);
  const auto& emptyDerived=redo.evaluatedAfter.extensions.at("KHR_materials_clearcoat");
  EXPECT_FALSE(emptyDerived.Has("clearcoatNormalTexture"));EXPECT_FALSE(emptyDerived.Has("vendor"));
  EXPECT_DOUBLE_EQ(emptyDerived.Get("clearcoatFactor").Get<double>(),.25);
  EXPECT_EQ(redo.evaluatedAfter.name,"Paint");EXPECT_EQ(redo.evaluatedAfter.alphaMode,"MASK");EXPECT_TRUE(redo.evaluatedAfter.doubleSided);
  EXPECT_DOUBLE_EQ(redo.evaluatedAfter.pbrMetallicRoughness.roughnessFactor,.91);
}

TEST(GltfMaterialAuthoring, CoalesceAllowsAdvancingUnrelatedEvaluatedLeavesAndKeepsTheirCurrentReplayValues) {
  auto source=basic();enable(source,E::Clearcoat,{{"clearcoatFactor",V(.1)},{"clearcoatRoughnessFactor",V(.2)}});
  pointer(source,"/materials/0/pbrMetallicRoughness/metallicFactor");
  auto initialE=source.materials[0];initialE.pbrMetallicRoughness.metallicFactor=.25;
  initialE.extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatFactor"]=V(.7);
  const auto view=GltfMaterialSourceView(source);
  const auto first=PlanGltfMaterialEdit(view,view,0,source.materials[0],initialE,number(P::Clearcoat,.3));
  auto advanced=first.evaluatedAfter;advanced.pbrMetallicRoughness.metallicFactor=.9;
  advanced.extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatRoughnessFactor"]=V(.8);
  const auto next=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,advanced,number(P::Clearcoat,.4));
  const auto cumulative=CoalesceGltfMaterialEdit(first,next,view,view);
  EXPECT_DOUBLE_EQ(cumulative.evaluatedBefore.pbrMetallicRoughness.metallicFactor,.25);
  EXPECT_DOUBLE_EQ(cumulative.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.9);
  EXPECT_EQ(cumulative.animationBindings.size(),1u);
  auto now=next.evaluatedAfter;now.pbrMetallicRoughness.metallicFactor=.45;
  now.extensions["KHR_materials_clearcoat"].Get<O>()["clearcoatRoughnessFactor"]=V(.95);
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,next.authoredAfter,now,view,view);
  EXPECT_DOUBLE_EQ(scalar(undo.authoredAfter,E::Clearcoat,"clearcoatFactor"),.1);
  EXPECT_DOUBLE_EQ(scalar(undo.evaluatedAfter,E::Clearcoat,"clearcoatFactor"),.7);
  EXPECT_DOUBLE_EQ(scalar(undo.evaluatedAfter,E::Clearcoat,"clearcoatRoughnessFactor"),.95);
  EXPECT_DOUBLE_EQ(undo.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.45);
  auto later=undo.evaluatedAfter;later.pbrMetallicRoughness.metallicFactor=.6;
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,later,view,view);
  EXPECT_DOUBLE_EQ(scalar(redo.evaluatedAfter,E::Clearcoat,"clearcoatFactor"),.4);
  EXPECT_DOUBLE_EQ(scalar(redo.evaluatedAfter,E::Clearcoat,"clearcoatRoughnessFactor"),.95);
  EXPECT_DOUBLE_EQ(redo.evaluatedAfter.pbrMetallicRoughness.metallicFactor,.6);
}

TEST(GltfMaterialAuthoring, CoalesceUnchangedFinalPreviewRetainsFalseAndMetadataOwnership) {
  auto source=basic();source.materials[0].doubleSided=true;source.materials[0].alphaMode="MASK";
  const auto view=GltfMaterialSourceView(source);
  GltfMaterialPatch patch;patch.name="Paint";patch.alphaMode=GltfMaterialAlphaMode::Opaque;patch.doubleSided=false;
  const auto first=PlanGltfMaterialEdit(source,source,0,patch);
  const auto unchanged=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,first.evaluatedAfter,number(P::Roughness,1.));
  ASSERT_FALSE(unchanged.changed);
  const auto cumulative=CoalesceGltfMaterialEdit(first,unchanged,view,view);
  ASSERT_TRUE(cumulative.changed);
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,unchanged.authoredAfter,unchanged.evaluatedAfter,view,view);
  EXPECT_EQ(undo.evaluatedAfter.name,"Source");EXPECT_EQ(undo.evaluatedAfter.alphaMode,"MASK");EXPECT_TRUE(undo.evaluatedAfter.doubleSided);
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,undo.authoredAfter,undo.evaluatedAfter,view,view);
  EXPECT_EQ(redo.evaluatedAfter.name,"Paint");EXPECT_EQ(redo.evaluatedAfter.alphaMode,"OPAQUE");EXPECT_FALSE(redo.evaluatedAfter.doubleSided);
}

TEST(GltfMaterialAuthoring, CoalesceRejectsForgedStaleChainsDifferentIdentitiesAndInverseCommands) {
  auto source=basic();source.materials.push_back(source.materials[0]);const auto view=GltfMaterialSourceView(source);
  const auto first=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const auto next=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,first.evaluatedAfter,number(P::Roughness,.4));
  EXPECT_THROW(CoalesceGltfMaterialEdit({},next,view,view),std::invalid_argument);
  auto forged=first;forged.authoredAfter.extras.Get<O>()["rawIndex"]=V(910);
  EXPECT_THROW(CoalesceGltfMaterialEdit(forged,next,view,view),std::invalid_argument);
  forged=next;forged.evaluatedAfter.pbrMetallicRoughness.roughnessFactor=std::nextafter(.4,1.);
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,forged,view,view),std::invalid_argument);
  const auto zero=PlanGltfMaterialEdit(source,source,0,number(P::Metallic,0.));
  const auto matchingFirst=PlanGltfMaterialEdit(view,view,0,zero.authoredAfter,zero.evaluatedAfter,number(P::Roughness,.3));
  for(const auto& mutate:std::vector<std::function<void(tinygltf::Material&)>>{
      [](auto& m){m.pbrMetallicRoughness.roughnessFactor=std::nextafter(.3,1.);},
      [](auto& m){m.extensions_json_string="unrelated cache mutation";},
      [](auto& m){m.pbrMetallicRoughness.metallicFactor=-0.;}}) {
    auto stale=matchingFirst.authoredAfter;mutate(stale);
    const auto broken=PlanGltfMaterialEdit(view,view,0,stale,matchingFirst.evaluatedAfter,number(P::Roughness,.4));
    EXPECT_THROW(CoalesceGltfMaterialEdit(matchingFirst,broken,view,view),std::invalid_argument);
  }
  const auto other=PlanGltfMaterialEdit(source,source,1,number(P::Roughness,.4));
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,other,view,view),std::invalid_argument);
  const auto inverse=RebaseGltfMaterialEdit(first,GltfMaterialEditSide::Before,first.authoredAfter,first.evaluatedAfter,view,view);
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,inverse,view,view),std::invalid_argument);
  EXPECT_THROW(CoalesceGltfMaterialEdit(inverse,next,view,view),std::invalid_argument);
  const auto duplicate=PlanGltfMaterialDuplicate(source,source,0);
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,duplicate,view,view),std::invalid_argument);
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(first,view,view));
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(next,view,view));
}

TEST(GltfMaterialAuthoring, CoalescePinsSeparateTextureSpacesDeclarationsAndOriginalAnimationBindings) {
  auto a=basic();enable(a,E::Clearcoat);pointer(a,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  auto e=a;a.textures.emplace_back();const auto av=GltfMaterialSourceView(a),ev=GltfMaterialSourceView(e);
  const auto first=PlanGltfMaterialEdit(a,e,0,number(P::Roughness,.3));
  const auto next=PlanGltfMaterialEdit(av,ev,0,first.authoredAfter,first.evaluatedAfter,number(P::Roughness,.4));
  const auto cumulative=CoalesceGltfMaterialEdit(first,next,av,ev);
  EXPECT_EQ(GltfMaterialAuthoringContext(cumulative).authoredTextureCount,24u);
  EXPECT_EQ(GltfMaterialAuthoringContext(cumulative).evaluatedTextureCount,23u);
  auto wrong=ev;++wrong.textureCount;
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,av,wrong),std::invalid_argument);
  wrong=av;++wrong.materialCount;
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,wrong,ev),std::invalid_argument);
  auto declarations=a.extensionsUsed;std::erase(declarations,"KHR_materials_clearcoat");wrong=av;wrong.extensionsUsed=declarations;
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,wrong,ev),std::invalid_argument);
  auto changed=a.animations;changed[0].channels[0].target_extensions["KHR_animation_pointer"].Get<O>()["pointer"]=V(std::string("/materials/0/pbrMetallicRoughness/metallicFactor"));
  auto changedA=av,changedE=ev;changedA.animations=changed;changedE.animations=changed;
  const auto changedNext=PlanGltfMaterialEdit(changedA,changedE,0,first.authoredAfter,first.evaluatedAfter,number(P::Roughness,.4));
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,changedNext,changedA,changedE),std::invalid_argument);
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,changedA,changedE),std::invalid_argument);
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(cumulative,av,ev));
}

TEST(GltfMaterialAuthoring, CoalesceReservesCoexistingInputsAndScratchAndHasFlatRepeatedPreviewCost) {
  auto source=basic();source.materials[0].extras.Get<O>()["large"]=V(std::string(4096,'x'));
  const auto view=GltfMaterialSourceView(source);
  const auto first=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const auto next=PlanGltfMaterialEdit(view,view,0,first.authoredAfter,first.evaluatedAfter,number(P::Metallic,.4));
  const auto cumulative=CoalesceGltfMaterialEdit(first,next,view,view);
  GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=cumulative.peakOwnedBytes-1;
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,view,view,limits),std::invalid_argument);
  limits={};limits.maxOutputBytes=std::max(first.peakOwnedBytes,next.peakOwnedBytes);
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,view,view,limits),std::invalid_argument);
  limits={};limits.maxPatchOperations=1;
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,view,view,limits),std::invalid_argument);
  limits={};limits.maxOutputBytes=0;
  EXPECT_THROW(CoalesceGltfMaterialEdit(first,next,view,view,limits),std::invalid_argument);
  auto pending=first;uint64_t stableCost{};
  for(size_t frame=1;frame<=64;++frame) {
    const auto step=PlanGltfMaterialEdit(view,view,0,pending.authoredAfter,pending.evaluatedAfter,number(P::Roughness,double(frame)/128.));
    auto replacement=CoalesceGltfMaterialEdit(pending,step,view,view);
    if(frame==2) stableCost=replacement.peakOwnedBytes;
    if(frame>2) EXPECT_EQ(replacement.peakOwnedBytes,stableCost);
    pending=std::move(replacement);
  }
  EXPECT_DOUBLE_EQ(pending.authoredBefore.pbrMetallicRoughness.roughnessFactor,1.);
  EXPECT_DOUBLE_EQ(pending.authoredAfter.pbrMetallicRoughness.roughnessFactor,.5);
  EXPECT_EQ(pending.authoredAfter.extras.Get("large").Get<std::string>(),std::string(4096,'x'));
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(pending,view,view));
  const auto undo=RebaseGltfMaterialEdit(pending,GltfMaterialEditSide::Before,pending.authoredAfter,pending.evaluatedAfter,view,view);
  EXPECT_DOUBLE_EQ(undo.authoredAfter.pbrMetallicRoughness.roughnessFactor,1.);
}

TEST(GltfMaterialAuthoring, RecordStoragePreflightsOpaqueAndLegacyPayloadAtExactByteBoundary) {
  auto record=basic().materials[0];
  record.extensions_json_string=std::string(2048,'x');
  record.normalTexture.extras=V(O{{"binary",V(std::vector<unsigned char>(4096,77))}});
  record.values["legacy"].number_array={.1,.2,.3};
  const auto bytes=GltfMaterialRecordStorageBytes(record);
  EXPECT_GT(bytes,sizeof(record)+2048u+4096u);
  GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=bytes;
  EXPECT_EQ(GltfMaterialRecordStorageBytes(record,limits),bytes);
  limits.maxOutputBytes=bytes-1;
  EXPECT_THROW(GltfMaterialRecordStorageBytes(record,limits),std::invalid_argument);
  EXPECT_EQ(record.extensions_json_string.size(),2048u);
  EXPECT_EQ(record.normalTexture.extras.Get("binary").Get<std::vector<unsigned char>>().size(),4096u);
}

TEST(GltfMaterialAuthoring, RecordStorageRejectsUnboundedDepthValuesAndNonfiniteLegacyPayload) {
  auto record=basic().materials[0];
  GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=UINT64_MAX;
  EXPECT_THROW(GltfMaterialRecordStorageBytes(record,limits),std::invalid_argument);
  limits={};limits.maxMetadataValues=1;
  EXPECT_THROW(GltfMaterialRecordStorageBytes(record,limits),std::invalid_argument);
  limits={};limits.maxMetadataDepth=1;
  record.extras=V(O{{"nested",V(O{{"leaf",V(1)}})}});
  EXPECT_THROW(GltfMaterialRecordStorageBytes(record,limits),std::invalid_argument);
  limits={};record.values["legacy"].json_double_value["value"]=std::numeric_limits<double>::infinity();
  EXPECT_THROW(GltfMaterialRecordStorageBytes(record,limits),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, ExactRecordComparisonRetainsNumericBitsTypesCachesAndNestedTextureMetadata) {
  const auto record=basic().materials[0];auto changed=record;
  EXPECT_TRUE(GltfMaterialRecordsEqual(record,changed));
  changed.pbrMetallicRoughness.roughnessFactor=std::nextafter(record.pbrMetallicRoughness.roughnessFactor,0.);
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
  changed=record;changed.emissiveFactor[0]=-0.;
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
  changed=record;changed.extras.Get<O>()["rawIndex"]=V(909.);
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
  changed=record;changed.normalTexture.extensions_json_string="retained raw cache";
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
  changed=record;changed.normalTexture.extensions["VENDOR_texture"]=V(O{{"index",V(3)}});
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
  changed=record;changed.values["legacy"].bool_value=true;
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
  changed=record;changed.extras.Get<std::string>()="hidden inactive payload";
  EXPECT_FALSE(GltfMaterialRecordsEqual(record,changed));
}

TEST(GltfMaterialAuthoring, RecordStorageRejectsHiddenInactiveContainerCopies) {
  auto record=basic().materials[0];record.extras=V(0.);
  record.extras.Get<A>().emplace_back(std::string(4096,'x'));
  EXPECT_THROW((void)GltfMaterialRecordStorageBytes(record),std::invalid_argument);
  record.extras=V(O{});record.extras.Get<std::string>()=std::string(4096,'y');
  EXPECT_THROW((void)GltfMaterialRecordStorageBytes(record),std::invalid_argument);
  record.extras=V(A{});record.extras.Get<std::vector<unsigned char>>().resize(4096);
  EXPECT_THROW((void)GltfMaterialRecordStorageBytes(record),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, MakeUniqueFreezesVisibleRestAndRetainsExactOriginAndAnimationProvenance) {
  auto authored=basic();pointer(authored,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  auto evaluated=authored;evaluated.materials[0].pbrMetallicRoughness.roughnessFactor=.23;
  evaluated.materials[0].extras.Get<O>()["rawIndex"]=V(910);
  const auto clips=authored.animations;const auto* bytes=evaluated.buffers[0].data.data();
  const auto frozen=PlanGltfMaterialMakeUnique(authored,evaluated,0);
  EXPECT_EQ(frozen.kind,GltfMaterialPlanKind::MakeUnique);EXPECT_EQ(frozen.sourceMaterial,0);
  EXPECT_EQ(frozen.destinationMaterial,1);ASSERT_TRUE(frozen.changed);
  EXPECT_TRUE(GltfMaterialRecordsEqual(frozen.authoredBefore,authored.materials[0]));
  EXPECT_TRUE(GltfMaterialRecordsEqual(frozen.evaluatedBefore,evaluated.materials[0]));
  EXPECT_TRUE(GltfMaterialRecordsEqual(frozen.authoredAfter,evaluated.materials[0]));
  EXPECT_TRUE(GltfMaterialRecordsEqual(frozen.evaluatedAfter,evaluated.materials[0]));
  ASSERT_EQ(frozen.animationBindings.size(),1u);EXPECT_EQ(frozen.animationBindings[0].clip,0u);
  EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(frozen,GltfMaterialSourceView(authored),GltfMaterialSourceView(evaluated)));
  EXPECT_EQ(authored.animations,clips);EXPECT_EQ(evaluated.buffers[0].data.data(),bytes);
  auto forged=frozen;forged.sourceMaterial.reset();
  EXPECT_THROW(ValidateGltfMaterialAuthoringPlan(forged,GltfMaterialSourceView(authored),GltfMaterialSourceView(evaluated)),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, MakeUniqueAppliesPatchAfterFreezeAndRefusesEditOnlyHistoryOperations) {
  auto authored=basic(),evaluated=authored;evaluated.materials[0].pbrMetallicRoughness.roughnessFactor=.23;
  GltfMaterialPatch patch=number(P::Metallic,.1);patch.name="Unique";
  const auto frozen=PlanGltfMaterialMakeUnique(authored,evaluated,0,patch);
  EXPECT_DOUBLE_EQ(frozen.authoredAfter.pbrMetallicRoughness.roughnessFactor,.23);
  EXPECT_DOUBLE_EQ(frozen.authoredAfter.pbrMetallicRoughness.metallicFactor,.1);
  EXPECT_TRUE(GltfMaterialRecordsEqual(frozen.authoredAfter,frozen.evaluatedAfter));
  EXPECT_EQ(frozen.authoredAfter.name,"Unique");
  EXPECT_THROW(RebaseGltfMaterialEdit(frozen,GltfMaterialEditSide::Before,frozen.authoredAfter,frozen.evaluatedAfter,
      GltfMaterialSourceView(authored),GltfMaterialSourceView(evaluated)),std::invalid_argument);
  GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=frozen.peakOwnedBytes-1;
  EXPECT_THROW((void)PlanGltfMaterialMakeUnique(authored,evaluated,0,patch,limits),std::invalid_argument);
}

TEST(GltfMaterialAuthoring, PublicationMatchesAnimatedEditMakeUniqueAndRebasedHistoryWithoutAllocating) {
  auto source=basic();pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  pointer(source,"/materials/0/extensions/FUTURE_property/value/0");
  const auto view=GltfMaterialSourceView(source);
  const auto edit=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const auto unique=PlanGltfMaterialMakeUnique(source,source,0,number(P::Metallic,.2));
  const auto duplicate=PlanGltfMaterialDuplicate(source,source,0);
  const auto procedural=PlanGltfMaterialAppend(source,source,{},{});
  const auto next=PlanGltfMaterialEdit(view,view,0,edit.authoredAfter,edit.evaluatedAfter,number(P::Roughness,.4));
  const auto cumulative=CoalesceGltfMaterialEdit(edit,next,view,view);
  const auto undo=RebaseGltfMaterialEdit(cumulative,GltfMaterialEditSide::Before,next.authoredAfter,next.evaluatedAfter,view,view);
  for(const auto* plan:{&edit,&unique,&duplicate,&procedural,&cumulative,&undo}) {
    EXPECT_NO_THROW(ValidateGltfMaterialAuthoringPlan(*plan,view,view));
    matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(*plan,view,view);},true);
  }
  matchesWithoutAllocations([&] {return GltfMaterialEditBeforeMatches(edit,source.materials[0],source.materials[0],view,view);},true);
  matchesWithoutAllocations([&] {return GltfMaterialEditBeforeMatches(undo,next.authoredAfter,next.evaluatedAfter,view,view);},true);
  matchesWithoutAllocations([&] {return GltfMaterialEditBeforeMatches(unique,source.materials[0],source.materials[0],view,view);},false);
}

TEST(GltfMaterialAuthoring, PublicationMatchersRejectTamperedReceiptCurrentBitsAndFiniteLimitFailuresWithoutAllocating) {
  auto source=basic();pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  const auto view=GltfMaterialSourceView(source);const auto original=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));
  const std::array<std::function<void(GltfMaterialAuthoringPlan&)>,8> mutations{
      [](auto& p) {p.sourceMaterial.reset();}, [](auto& p) {p.destinationMaterial=1;},
      [](auto& p) {p.authoredBefore.pbrMetallicRoughness.metallicFactor=-0.;},
      [](auto& p) {p.evaluatedAfter.extensions_json_string="tampered raw metadata";},
      [](auto& p) {p.authoredAfter.extras.Get<O>()["rawIndex"]=V(909.);},
      [](auto& p) {p.animationBindings[0].path="other binding";},
      [](auto& p) {p.touchedProperties.push_back(P::Metallic);}, [](auto& p) {++p.peakOwnedBytes;}};
  for(const auto& mutate:mutations) {
    auto tampered=original;mutate(tampered);
    matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(tampered,view,view);},false);
  }
  const GltfMaterialAuthoringPlan empty;
  matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(empty,view,view);},false);
  auto stale=source.materials[0];stale.pbrMetallicRoughness.roughnessFactor=std::nextafter(1.,0.);
  matchesWithoutAllocations([&] {return GltfMaterialEditBeforeMatches(original,stale,source.materials[0],view,view);},false);
  stale=source.materials[0];stale.normalTexture.extensions_json_string="stale nested raw cache";
  matchesWithoutAllocations([&] {return GltfMaterialEditBeforeMatches(original,source.materials[0],stale,view,view);},false);
  auto hidden=original;hidden.authoredAfter.extras=V(0);hidden.authoredAfter.extras.Get<A>().emplace_back(1);
  matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(hidden,view,view);},false);
  const std::array<std::function<void(GltfMaterialAuthoringLimits&)>,6> tighten{
      [&](auto& l) {l.maxOutputBytes=original.peakOwnedBytes-1;}, [](auto& l) {l.maxOutputBytes=UINT64_MAX;},
      [](auto& l) {l.maxMetadataValues=1;}, [](auto& l) {l.maxMetadataDepth=257;},
      [](auto& l) {l.maxAnimationChannels=0;}, [](auto& l) {l.maxPatchOperations=0;}};
  for(const auto& change:tighten) {
    GltfMaterialAuthoringLimits limits;change(limits);
    matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(original,view,view,limits);},false);
  }
}

TEST(GltfMaterialAuthoring, PublicationBindingMatchRejectsStaleMalformedAndMissingRawTargetsWithoutAllocating) {
  auto source=basic();enable(source,E::Clearcoat);source.materials.emplace_back();
  pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  pointer(source,"/materials/1/pbrMetallicRoughness/metallicFactor");
  const auto plan=PlanGltfMaterialEdit(source,source,0,number(P::Roughness,.3));const auto view=GltfMaterialSourceView(source);
  const std::array<std::function<void(tinygltf::Model&)>,7> mutations{
      [](auto& m) {m.animations[0].channels[0].target_extensions["KHR_animation_pointer"].Get<O>()["pointer"]=V(std::string("/materials/0/FUTURE_property/value"));},
      [](auto& m) {m.animations[0].channels[0].target_extensions["KHR_animation_pointer"].Get<O>()["pointer"]=V(std::string("/materials/999/pbrMetallicRoughness/roughnessFactor"));},
      [](auto& m) {m.animations[0].channels[0].target_extensions["KHR_animation_pointer"].Get<O>()["pointer"]=V(12);},
      [](auto& m) {m.animations[0].channels[0].target_extensions["KHR_animation_pointer"]=V(12);},
      [](auto& m) {m.animations[0].channels[0].target_extensions.clear();},
      [](auto& m) {m.animations[0].channels.push_back(m.animations[0].channels[0]);},
      [](auto& m) {std::erase(m.extensionsUsed,"KHR_materials_clearcoat");}};
  for(const auto& mutate:mutations) {
    auto changed=source;mutate(changed);const auto changedView=GltfMaterialSourceView(changed);
    matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(plan,view,changedView);},false);
  }
  auto unrelated=source;unrelated.animations[0].channels[1].target_extensions["KHR_animation_pointer"].Get<O>()["pointer"]=
      V(std::string("/materials/1/pbrMetallicRoughness/roughnessFactor"));
  unrelated.extensionsUsed.push_back("LATER_global");const auto unrelatedView=GltfMaterialSourceView(unrelated);
  matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(plan,unrelatedView,unrelatedView);},true);
}

TEST(GltfMaterialAuthoring, PublicationResourceTransitionAndOriginalReplayKeepBothDomainsAndBeforeRecordsWithoutAllocating) {
  auto source=basic();pointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
  const auto before=GltfMaterialSourceView(source);auto afterA=before,afterE=before;
  afterA.textureCount=24;afterE.textureCount=25;
  const auto plan=PlanGltfMaterialTextureEdit(before,before,afterA,afterE,0,source.materials[0],source.materials[0],number(P::Roughness,.3));
  EXPECT_NO_THROW(ValidateGltfMaterialEditTransition(plan,source.materials[0],source.materials[0],before,before,afterA,afterE));
  matchesWithoutAllocations([&] {return GltfMaterialEditTransitionMatches(plan,source.materials[0],source.materials[0],before,before,afterA,afterE);},true);
  matchesWithoutAllocations([&] {return GltfMaterialAuthoringPlanMatches(plan,before,before);},false);
  matchesWithoutAllocations([&] {return GltfMaterialEditBeforeMatches(plan,source.materials[0],source.materials[0],before,before);},false);
  auto wrong=afterA;++wrong.textureCount;
  matchesWithoutAllocations([&] {return GltfMaterialEditTransitionMatches(plan,source.materials[0],source.materials[0],before,before,wrong,afterE);},false);
  matchesWithoutAllocations([&] {return GltfMaterialEditTransitionMatches(plan,plan.authoredAfter,source.materials[0],before,before,afterA,afterE);},false);
  const auto undo=RebaseGltfMaterialTextureEdit(plan,GltfMaterialEditSide::Before,plan.authoredAfter,plan.evaluatedAfter,
      afterA,afterE,before,before);
  EXPECT_NO_THROW(ValidateGltfMaterialEditTransition(undo,plan.authoredAfter,plan.evaluatedAfter,afterA,afterE,before,before));
  matchesWithoutAllocations([&] {return GltfMaterialEditTransitionMatches(undo,plan.authoredAfter,plan.evaluatedAfter,
      afterA,afterE,before,before);},true);
  auto forged=undo;forged.evaluatedAfter.extras_json_string="replayed metadata tamper";
  matchesWithoutAllocations([&] {return GltfMaterialEditTransitionMatches(forged,plan.authoredAfter,plan.evaluatedAfter,
      afterA,afterE,before,before);},false);
}
