#include <Container/geometry/GltfTextureResourceReferences.h>
#include <Container/geometry/GltfMaterialTextureAuthoring.h>

#include <gtest/gtest.h>
#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using Value=tinygltf::Value;
using Object=Value::Object;
using Kind=GltfTextureResourceReferenceKind;
Value info(int index) {return Value(Object{{"index",Value(index)}});}
Value source(int index) {return Value(Object{{"source",Value(index)}});}
std::vector<uint32_t> ids(std::span<const uint32_t> values) {return {values.begin(),values.end()};}
tinygltf::Parameter parameter(double index) {
  tinygltf::Parameter result;result.json_double_value["index"]=index;return result;
}
tinygltf::Model model() {
  tinygltf::Model result;result.asset.version="2.0";result.images.resize(5);result.samplers.resize(2);
  result.textures.resize(8);for(auto& t:result.textures) {t.source=0;t.sampler=0;}
  result.materials.resize(2);return result;
}
void declare(tinygltf::Model& source,std::string name) {
  if(std::ranges::find(source.extensionsUsed,name)==source.extensionsUsed.end()) source.extensionsUsed.push_back(std::move(name));
}
void pointer(tinygltf::Model& model,std::string path) {
  declare(model,"KHR_animation_pointer");if(model.animations.empty()) model.animations.resize(1);
  tinygltf::AnimationChannel channel;channel.target_path="pointer";channel.target_node=-1;
  channel.target_extensions["KHR_animation_pointer"]=Value(Object{{"pointer",Value(std::move(path))}});
  model.animations[0].channels.push_back(std::move(channel));
}

TEST(GltfTextureResourceReferences,RawCoreExtensionsAndShadowedLegacyIndicesAllRemainDependencies) {
  auto sourceModel=model();auto& m=sourceModel.materials[0];
  m.pbrMetallicRoughness.baseColorTexture.index=0;m.pbrMetallicRoughness.metallicRoughnessTexture.index=1;
  m.normalTexture.index=2;m.occlusionTexture.index=3;m.emissiveTexture.index=4;
  m.values["baseColorTexture"]=parameter(5);m.additionalValues["baseColorTexture"]=parameter(6);
  m.values["roughnessTexture"]=parameter(7);
  declare(sourceModel,"KHR_materials_pbrSpecularGlossiness");
  m.extensions["KHR_materials_pbrSpecularGlossiness"]=Value(Object{{"diffuseTexture",info(1)},{"specularGlossinessTexture",info(2)}});
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);
  EXPECT_TRUE(proof.complete());EXPECT_EQ(ids(proof.materialTextures(0)),(std::vector<uint32_t>{0,1,2,3,4,5,6,7}));
  EXPECT_TRUE(proof.materialTextures(1).empty());
  auto detached=ExtractGltfMaterialRawTextureReferences(m,8);
  EXPECT_EQ(ids(detached.textures()),ids(proof.materialTextures(0)));
  EXPECT_TRUE(GltfMaterialRawTextureReferencesMatch(m,8,{},detached));
  // Same normalized dependencies, different raw location: exact witness rejects.
  std::swap(m.values["baseColorTexture"],m.additionalValues["baseColorTexture"]);
  EXPECT_FALSE(GltfTextureResourceReferencesMatch(sourceModel,{},proof));
  EXPECT_FALSE(GltfMaterialRawTextureReferencesMatch(m,8,{},detached));
}

TEST(GltfTextureResourceReferences,EveryRegisteredTypedTextureFieldIsExtractedWithoutWorkflowSelection) {
  auto sourceModel=model();auto& m=sourceModel.materials[0];
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) if(!slot.extension.empty()) {
    declare(sourceModel,std::string(slot.extension));const std::string name(slot.extension);
    if(!m.extensions.contains(name)) m.extensions[name]=Value(Object{});
    const std::string field(slot.path.substr(slot.path.rfind('/')+1));
    m.extensions[name].Get<Object>()[field]=info(int(uint32_t(slot.slot)%8));
  }
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) if(!slot.extension.empty()) {
    const auto refs=proof.materialTextures(0);EXPECT_NE(std::ranges::find(refs,uint32_t(slot.slot)%8),refs.end());
  }
  EXPECT_TRUE(proof.complete());
}

TEST(GltfTextureResourceReferences,AllAuthoredCoreAndCodecAlternativesAndSamplerAreIndependent) {
  auto sourceModel=model();auto& texture=sourceModel.textures[0];texture.source=0;texture.sampler=1;
  int index=1;
  for(const auto* name:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds","EXT_texture_avif"}) {
    declare(sourceModel,name);texture.extensions[name]=source(index++);
  }
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);
  EXPECT_EQ(ids(proof.textureImages(0)),(std::vector<uint32_t>{0,1,2,3,4}));
  EXPECT_EQ(ids(proof.textureSamplers(0)),(std::vector<uint32_t>{1}));
  const auto detached=ExtractGltfTextureRawResourceReferences(texture,5,2);
  EXPECT_TRUE(detached.complete());EXPECT_EQ(ids(detached.images()),ids(proof.textureImages(0)));
  EXPECT_EQ(ids(detached.samplers()),ids(proof.textureSamplers(0)));
  texture.extensions["EXT_texture_webp"]=source(0);
  EXPECT_FALSE(GltfTextureRawResourceReferencesMatch(texture,5,2,{},{},detached));
  // A codec not selected for rendering still prevents its source disappearing.
  texture.extensions["EXT_texture_webp"]=source(5);
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel),std::invalid_argument);
}

TEST(GltfTextureResourceReferences,RetiredOwnersAreOmittedAndActiveReferencesToRetiredTargetsReject) {
  auto sourceModel=model();sourceModel.materials[0].normalTexture.index=2;
  sourceModel.materials[1].normalTexture.index=99;sourceModel.materials[1].extensions["VENDOR_unknown"]=Value(19);
  declare(sourceModel,"VENDOR_unknown");sourceModel.textures[7].source=99;
  const std::array<uint8_t,2> materialMask{1,0};const std::array<uint8_t,8> textureMask{1,1,1,1,1,1,1,0};
  GltfTextureResourceMasks masks;masks.materials=materialMask;masks.textures=textureMask;
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel,masks);
  EXPECT_TRUE(proof.complete());EXPECT_FALSE(proof.materialActive(1));EXPECT_FALSE(proof.textureActive(7));
  EXPECT_TRUE(proof.materialTextures(1).empty());EXPECT_TRUE(proof.textureImages(7).empty());
  auto changed=textureMask;changed[2]=0;masks.textures=changed;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,masks),std::invalid_argument);
  EXPECT_FALSE(GltfTextureResourceReferencesMatch(sourceModel,masks,proof));
  masks.textures=textureMask;const std::array<uint8_t,5> imageMask{0,1,1,1,1};masks.images=imageMask;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,masks),std::invalid_argument);
  const std::array<uint8_t,2> badMask{1,2};masks.materials=badMask;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,masks),std::invalid_argument);
}

TEST(GltfTextureResourceReferences,OpaqueNormativeSchemasAreExplicitIncompleteAndExactlyWitnessed) {
  auto sourceModel=model();sourceModel.materials[0].normalTexture.index=3;
  sourceModel.extensions["VENDOR_root"]=Value(Value::Array{Value(19)});
  sourceModel.materials[0].extensions["VENDOR_material"]=Value(Object{{"texture",Value(6)}});
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel),std::invalid_argument);
  GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  EXPECT_FALSE(proof.complete());ASSERT_EQ(proof.incompleteReasons().size(),2u);
  EXPECT_EQ(ids(proof.materialTextures(0)),(std::vector<uint32_t>{3}));
  EXPECT_TRUE(GltfTextureResourceReferencesMatch(sourceModel,{},proof));
  const auto selected=ExtractGltfMaterialRawTextureReferences(sourceModel.materials[0],8,{},limits);
  EXPECT_FALSE(selected.complete());EXPECT_EQ(ids(selected.textures()),(std::vector<uint32_t>{3}));
  sourceModel.materials[0].extensions["VENDOR_material"]=Value(Object{{"texture",Value(5)}});
  EXPECT_FALSE(GltfTextureResourceReferencesMatch(sourceModel,{},proof));
  EXPECT_FALSE(GltfMaterialRawTextureReferencesMatch(sourceModel.materials[0],8,{},selected));
  limits.maxIncompleteReasons=1;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::length_error);
}

TEST(GltfTextureResourceReferences,ExtrasAreAnnotationsWhileNestedNormativeExtensionsFailClosed) {
  auto sourceModel=model();sourceModel.extras=Value(Object{{"extensions",Value(Object{{"VENDOR_annotation",Value(7)}})},{"index",Value(99)}});
  EXPECT_TRUE(ExtractGltfTextureResourceReferences(sourceModel).complete());
  declare(sourceModel,"KHR_materials_specular");
  auto binding=info(0);binding.Get<Object>()["extensions"]=Value(Object{{"VENDOR_nested",Value(Object{{"image",Value(4)}})}});
  sourceModel.materials[0].extensions["KHR_materials_specular"]=Value(Object{{"specularTexture",std::move(binding)}});
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel),std::invalid_argument);
  GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  EXPECT_FALSE(proof.complete());EXPECT_EQ(ids(proof.materialTextures(0)),(std::vector<uint32_t>{0}));
  EXPECT_TRUE(GltfTextureResourceReferencesMatch(sourceModel,{},proof));
}

TEST(GltfTextureResourceReferences,RawAnimationTargetsAndUnsupportedIndexMutationRemainConservative) {
  auto sourceModel=model();pointer(sourceModel,"/images/3/name");pointer(sourceModel,"/textures/4/name");
  pointer(sourceModel,"/samplers/1/wrapS");pointer(sourceModel,"/materials/0/roughnessFactor");
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel),std::invalid_argument);
  GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  ASSERT_EQ(proof.incompleteReasons().size(),4u);
  for(const auto& reason:proof.incompleteReasons()) EXPECT_EQ(reason.kind,GltfTextureResourceIncompleteKind::UnknownAnimationPointer);
  const std::vector<GltfTextureResourceReference> actual(proof.sourceReferences().begin(),proof.sourceReferences().end());
  EXPECT_EQ(actual,(std::vector<GltfTextureResourceReference>{{Kind::Image,3},{Kind::Texture,4},{Kind::Sampler,1}}));
  pointer(sourceModel,"/textures/4/source");
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel),std::invalid_argument);
  auto incomplete=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  EXPECT_FALSE(incomplete.complete());EXPECT_EQ(incomplete.incompleteReasons()[4].kind,GltfTextureResourceIncompleteKind::AnimatedResourceIndex);
  pointer(sourceModel,"/extensions/VENDOR_state/texture");incomplete=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  ASSERT_EQ(incomplete.incompleteReasons().size(),6u);
  pointer(sourceModel,"/textures/4/extensions/VENDOR_future/property");incomplete=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  ASSERT_EQ(incomplete.incompleteReasons().size(),7u);
  pointer(sourceModel,"/materials/0/extensions/KHR_materials_specular/specularTexture/extensions/VENDOR_future/value");
  incomplete=ExtractGltfTextureResourceReferences(sourceModel,{},limits);ASSERT_EQ(incomplete.incompleteReasons().size(),8u);
  pointer(sourceModel,"/images/99/name");
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::invalid_argument);
}

TEST(GltfTextureResourceReferences,SupportedNumericPointerGrammarIncludesComponentsAndNestedTextureTransforms) {
  auto sourceModel=model();sourceModel.nodes.resize(1);sourceModel.cameras.resize(2);sourceModel.lights.resize(1);
  sourceModel.cameras[0].type="perspective";sourceModel.cameras[0].perspective.zfar=100;
  sourceModel.cameras[0].perspective.aspectRatio=1;sourceModel.cameras[1].type="orthographic";
  sourceModel.lights[0].type="spot";sourceModel.lights[0].range=10;
  for(const auto* path:{"/materials/0/pbrMetallicRoughness/roughnessFactor",
      "/materials/0/pbrMetallicRoughness/baseColorFactor/3","/materials/0/emissiveFactor/2",
      "/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor/1",
      "/materials/0/extensions/KHR_materials_pbrSpecularGlossiness/specularFactor/0",
      "/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/offset/1",
      "/materials/0/extensions/KHR_materials_specular/specularColorTexture/extensions/KHR_texture_transform/scale",
      "/materials/0/extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture/extensions/KHR_texture_transform/rotation",
      "/nodes/0/translation/2","/nodes/0/rotation","/nodes/0/scale",
      "/cameras/0/perspective/yfov","/cameras/1/orthographic/xmag",
      "/extensions/KHR_lights_punctual/lights/0/color/2","/extensions/KHR_lights_punctual/lights/0/spot/innerConeAngle"})
    pointer(sourceModel,path);
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);
  EXPECT_TRUE(proof.complete());EXPECT_TRUE(proof.sourceReferences().empty());
  bool matches{};size_t allocations{};
  {material_publication_test::AllocationGuard deny(true);
    matches=GltfTextureResourceReferencesMatch(sourceModel,{},proof);allocations=deny.allocations();}
  EXPECT_TRUE(matches);EXPECT_EQ(allocations,0u);
}

TEST(GltfTextureResourceReferences,UnknownPointerSuffixesNeverInheritKnownOwnerCompleteness) {
  for(const auto* path:{"/materials/0/roughnessFactor","/materials/0/pbrMetallicRoughness/futureBinding",
      "/materials/0/extensions/KHR_materials_specular/futureTexture",
      "/materials/0/normalTexture/extensions/KHR_texture_transform/texCoord",
      "/materials/0/normalTexture/extensions/KHR_texture_transform/futureIndex",
      "/materials/0/extras/texture","/nodes/0/translation/extra","/nodes/0/extensions/KHR_node_visibility/visible",
      "/cameras/0/perspective/texture","/extensions/KHR_lights_punctual/lights/0/cookie",
      "/extensions/KHR_lights_area/lights/0/intensity","/images/0/name","/textures/0/name","/samplers/0/wrapS"}) {
    SCOPED_TRACE(path);auto sourceModel=model();sourceModel.nodes.resize(1);sourceModel.cameras.resize(1);sourceModel.lights.resize(1);
    sourceModel.cameras[0].type="perspective";pointer(sourceModel,path);
    EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel),std::invalid_argument);
    GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
    const auto proof=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
    ASSERT_FALSE(proof.complete());ASSERT_EQ(proof.incompleteReasons().size(),1u);
    EXPECT_EQ(proof.incompleteReasons()[0].kind,GltfTextureResourceIncompleteKind::UnknownAnimationPointer);
    bool matches{};size_t allocations{};
    {material_publication_test::AllocationGuard deny(true);
      matches=GltfTextureResourceReferencesMatch(sourceModel,{},proof);allocations=deny.allocations();}
    EXPECT_TRUE(matches);EXPECT_EQ(allocations,0u);
  }
}

TEST(GltfTextureResourceReferences,PointerOwnerBoundsPhasesAndVectorComponentsAreValidated) {
  GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  for(const auto* path:{"/materials/99/emissiveFactor","/materials/00/emissiveFactor","/materials/-1/emissiveFactor",
      "/nodes/1/translation","/nodes/bad/translation","/cameras/1/perspective/yfov",
      "/extensions/KHR_lights_punctual/lights/1/color","/materials/0/emissiveFactor/3",
      "/materials/0/pbrMetallicRoughness/roughnessFactor/0","/nodes/0/rotation/0","/nodes/0/translation/3",
      "/materials/0/normalTexture/extensions/KHR_texture_transform/offset/2"}) {
    SCOPED_TRACE(path);auto sourceModel=model();sourceModel.nodes.resize(1);sourceModel.cameras.resize(1);sourceModel.lights.resize(1);
    pointer(sourceModel,path);EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::invalid_argument);
  }
  auto sourceModel=model();pointer(sourceModel,"/materials/0/emissiveFactor");
  const std::array<uint8_t,2> materialMask{0,1};GltfTextureResourceMasks masks;masks.materials=materialMask;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,masks,limits),std::invalid_argument);
}

TEST(GltfTextureResourceReferences,SharedNonResourcePointerClassifierUsesTheSameGrammarWithoutAllocation) {
  auto source=model();source.nodes.resize(1);source.cameras.resize(1);source.cameras[0].type="perspective";
  const std::string longUnknown="/materials/0/"+std::string(4096,'x');
  bool material{},transform{},node{},camera{},unknown{},resource{},bounds{},escaped{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);
    material=IsSupportedGltfNonResourceAnimationPointer(source,"/materials/0/pbrMetallicRoughness/roughnessFactor");
    transform=IsSupportedGltfNonResourceAnimationPointer(source,"/materials/0/normalTexture/extensions/KHR_texture_transform/offset/1");
    node=IsSupportedGltfNonResourceAnimationPointer(source,"/nodes/0/translation/2");
    camera=IsSupportedGltfNonResourceAnimationPointer(source,"/cameras/0/perspective/yfov");
    unknown=IsSupportedGltfNonResourceAnimationPointer(source,longUnknown);
    resource=IsSupportedGltfNonResourceAnimationPointer(source,"/textures/0/name");
    bounds=IsSupportedGltfNonResourceAnimationPointer(source,"/nodes/0/rotation/0");
    escaped=IsSupportedGltfNonResourceAnimationPointer(source,"/materials/0/emissiveFactor~2");attempts=deny.allocations();}
  EXPECT_TRUE(material);EXPECT_TRUE(transform);EXPECT_TRUE(node);EXPECT_TRUE(camera);
  EXPECT_FALSE(unknown);EXPECT_FALSE(resource);EXPECT_FALSE(bounds);EXPECT_FALSE(escaped);EXPECT_EQ(attempts,0u);
}

TEST(GltfTextureResourceReferences,MalformedReferencesNeverBecomePermittedOpaqueSchemas) {
  auto sourceModel=model();GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  sourceModel.materials[0].values["roughnessTexture"]=parameter(1.5);
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::invalid_argument);
  sourceModel.materials[0].values.clear();sourceModel.materials[0].normalTexture.index=-2;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::invalid_argument);
  sourceModel.materials[0].normalTexture.index=-1;
  declare(sourceModel,"KHR_texture_basisu");sourceModel.textures[0].extensions["KHR_texture_basisu"]=Value(Object{{"source",Value(1.0)}});
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::invalid_argument);
  sourceModel.textures[0].extensions.clear();sourceModel.materials[0].values["VENDOR_binding"]=parameter(1);
  EXPECT_FALSE(ExtractGltfTextureResourceReferences(sourceModel,{},limits).complete());
}

TEST(GltfTextureResourceReferences,MetadataBudgetsBoundDepthWorkStorageAndDoNotCopyImagePayloads) {
  auto sourceModel=model();sourceModel.images[0].image.resize(1024*1024,17);sourceModel.buffers.resize(1);sourceModel.buffers[0].data.resize(1024*1024,42);
  sourceModel.images[0].uri=std::string(1024*1024,'x');
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);
  EXPECT_LT(proof.stats().ownedBytes,64u*1024);
  sourceModel.images[0].image[0]=19;sourceModel.buffers[0].data[0]=99;
  EXPECT_TRUE(GltfTextureResourceReferencesMatch(sourceModel,{},proof)); // Payload freshness is an outer owner responsibility.
  GltfTextureResourceReferenceLimits limits;limits.maxOwnedBytes=1;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::length_error);
  limits={};limits.maxTraversalSteps=1;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::length_error);
  limits={};limits.maxMetadataDepth=2;
  sourceModel.extras=Value(Object{{"a",Value(Object{{"b",Value(Object{{"c",Value(1)}})}})}});
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::length_error);
  limits={};limits.maxMetadataValues=1;
  EXPECT_THROW((void)ExtractGltfTextureResourceReferences(sourceModel,{},limits),std::length_error);
}

TEST(GltfTextureResourceReferences,SelectedEndpointStorageDoesNotRetainWholeCatalogMasksOrRanges) {
  tinygltf::Material material;material.normalTexture.index=7;
  const auto small=ExtractGltfMaterialRawTextureReferences(material,8);
  const auto large=ExtractGltfMaterialRawTextureReferences(material,1'000'000);
  EXPECT_EQ(small.stats().ownedBytes,large.stats().ownedBytes);
  tinygltf::Texture texture;texture.source=7;texture.sampler=3;
  const auto selected=ExtractGltfTextureRawResourceReferences(texture,1'000'000,1'000'000);
  EXPECT_LT(selected.stats().ownedBytes,4096u);
  const auto current=ExtractGltfTextureRawResourceReferences(texture,8,4);
  const std::array<uint8_t,8> activeImages{1,1,1,1,1,1,1,0};
  EXPECT_FALSE(GltfTextureRawResourceReferencesMatch(texture,8,4,activeImages,{},current));
  EXPECT_THROW((void)ExtractGltfTextureRawResourceReferences(texture,8,4,activeImages),std::invalid_argument);
}

TEST(GltfTextureResourceReferences,RechecksAreAllocationFreeForValidStaleAndOpaqueWitnesses) {
  auto sourceModel=model();sourceModel.materials[0].normalTexture.index=1;
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);
  const auto material=ExtractGltfMaterialRawTextureReferences(sourceModel.materials[0],8);
  const auto texture=ExtractGltfTextureRawResourceReferences(sourceModel.textures[0],5,2);
  bool valid{},materialValid{},textureValid{},stale{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);
    valid=GltfTextureResourceReferencesMatch(sourceModel,{},proof);
    materialValid=GltfMaterialRawTextureReferencesMatch(sourceModel.materials[0],8,{},material);
    textureValid=GltfTextureRawResourceReferencesMatch(sourceModel.textures[0],5,2,{},{},texture);
    sourceModel.materials[0].normalTexture.index=2;stale=GltfTextureResourceReferencesMatch(sourceModel,{},proof);attempts=deny.allocations();}
  EXPECT_TRUE(valid);EXPECT_TRUE(materialValid);EXPECT_TRUE(textureValid);EXPECT_FALSE(stale);EXPECT_EQ(attempts,0u);
  sourceModel.extensions["VENDOR_unknown"]=Value(Object{{"index",Value(7)}});
  GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  const auto opaque=ExtractGltfTextureResourceReferences(sourceModel,{},limits);
  {material_publication_test::AllocationGuard deny(true);valid=GltfTextureResourceReferencesMatch(sourceModel,{},opaque);attempts=deny.allocations();}
  EXPECT_TRUE(valid);EXPECT_EQ(attempts,0u);
}

TEST(GltfTextureResourceReferences,EveryPreparationAllocationFailureLeavesSourceAndEarlierProofUntouched) {
  auto sourceModel=model();sourceModel.materials[0].normalTexture.index=3;
  sourceModel.materials[0].extras=Value(Object{{"keep",Value(std::string(100,'x'))}});
  const auto proof=ExtractGltfTextureResourceReferences(sourceModel);size_t calls{};
  {material_publication_test::AllocationGuard count(false);auto prepared=ExtractGltfTextureResourceReferences(sourceModel);calls=count.allocations();}
  ASSERT_GT(calls,0u);ASSERT_LT(calls,100u);
  for(size_t allowance=0;allowance<calls;++allowance) {
    bool failed{};{material_publication_test::AllocationGuard deny(false,allowance);try {(void)ExtractGltfTextureResourceReferences(sourceModel);} catch(const std::bad_alloc&) {failed=true;}}
    EXPECT_TRUE(failed);EXPECT_TRUE(GltfTextureResourceReferencesMatch(sourceModel,{},proof));
    EXPECT_EQ(sourceModel.materials[0].normalTexture.index,3);
  }
}
} // namespace
