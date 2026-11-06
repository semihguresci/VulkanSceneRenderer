#include <Container/utility/GuiManager.h>

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using namespace container::ui;
using Phase=container::material::MaterialEditGesturePhase;
using Value=tinygltf::Value;
MaterialInspectorTarget target() {return {11,23,5,3,7,9,2};}
tinygltf::Material material() {
  tinygltf::Material result;result.name="Paint";result.doubleSided=true;
  result.pbrMetallicRoughness.baseColorFactor={.2,.3,.4,1};result.pbrMetallicRoughness.metallicFactor=.4;
  return result;
}
MaterialInspectorSnapshot snapshot() {return BuildMaterialInspectorSnapshot(target(),material(),material(),4);}
MaterialInspectorSnapshot bindingSnapshot() {
  auto result=snapshot();result.bindingOperationsAvailable=true;result.makeUniqueAvailable=true;
  result.assignmentChoices={{2,9,"Paint"},{3,17,"Glass"},{-1,23,"Runtime default"}};return result;
}
MaterialInspectorSnapshot catalogSnapshot(bool withSurface=true) {
  auto result=withSurface?snapshot():MaterialInspectorSnapshot{};
  result.catalogChoices={{2,9,"Paint"},{3,17,"Unused middle material"},{-1,23,"Unused runtime material"}};
  result.catalogTarget={11,23,5,41,17,3};result.materialRetirementAvailable=true;return result;
}
ResourceCatalogSnapshot resourceSnapshot() {
  ResourceCatalogSnapshot result;result.available=true;result.assetIdentity=23;result.resourceRevision=19;
  result.choices={{{23,19,7,41,2,ResourceCatalogDomain::Archive,ResourceCatalogKind::Texture},"Texture 2: Unused paint"},
      {{23,19,9,43,5,ResourceCatalogDomain::Runtime,ResourceCatalogKind::RuntimeTexture},"Runtime texture 5: Unused image"}};
  result.selected=result.choices[0].target;result.retirementAvailable=true;return result;
}
MaterialBindingOperationRequest assignRequest() {
  return {target(),MaterialBindingOperation::Assign,3,17,{}};
}
MaterialBindingOperationRequest uniqueRequest() {
  return {target(),MaterialBindingOperation::MakeUnique,2,9,{}};
}
QueuedMaterialEdit numeric(Phase phase,uint64_t id,double value=.25) {
  QueuedMaterialEdit result;result.target=target();result.request.sourceMaterial=2;result.request.phase=phase;
  if(phase!=Phase::Commit) result.request.gestureId=id;
  if(phase!=Phase::Cancel) result.request.patch.values.push_back({GltfMaterialProperty::Metallic,value});
  return result;
}
MaterialInspectorSnapshot textureSnapshot() {
  auto result=bindingSnapshot();result.textureEditingAvailable=true;result.resourceRevision=19;
  result.textureChoices={{0,0,"Retained color","retained.png",true,true},{8,11,"Other image","other.png",true,true},
      {12,-1,"Archive data","data.png",false,true}};
  for(auto& slot:result.textureSlots) {
    slot.available=true;slot.mappingEditable=true;slot.samplerEditable=true;slot.bound=true;
    slot.authoredTexture=0;slot.evaluatedTexture=0;slot.file="retained.png";
  }
  return result;
}
MaterialInspectorSnapshot sharedSpecularGlossinessSnapshot() {
  auto source=material();source.extensions["KHR_materials_pbrSpecularGlossiness"]=Value(Value::Object{
      {"specularGlossinessTexture",Value(Value::Object{{"index",Value(0)}})}});
  auto result=BuildMaterialInspectorSnapshot(target(),source,source,2);
  result.textureEditingAvailable=true;result.resourceRevision=19;
  result.textureChoices={{8,11,"Color source","color.png",true,false},{12,-1,"Data source","data.png",false,true}};
  for(auto& slot:result.textureSlots) {slot.available=true;slot.mappingEditable=slot.bound;slot.samplerEditable=slot.bound;}
  return result;
}
QueuedMaterialTextureEdit textureRequest(container::material::MaterialTextureEditOperation operation=
    container::material::MaterialTextureEditOperation::Clear,GltfMaterialTextureSlot slot=GltfMaterialTextureSlot::BaseColor) {
  QueuedMaterialTextureEdit result;result.target=target();result.request.sourceMaterial=2;
  result.request.slot=slot;result.request.operation=operation;result.request.expectedResourceRevision=19;return result;
}

TEST(GuiMaterialTextures, FactoryReadsAllSlotMappingsWithoutAdvertisingUnprovedResourceCapabilities) {
  auto authored=material(),evaluated=material();
  authored.normalTexture.index=3;evaluated.normalTexture.index=9;
  authored.normalTexture.texCoord=1;authored.normalTexture.extensions["KHR_texture_transform"]=Value(Value::Object{
      {"offset",Value(Value::Array{Value(.25),Value(-.5)})},{"rotation",Value(.75)}});
  const auto state=BuildMaterialInspectorSnapshot(target(),authored,evaluated,2);
  ASSERT_EQ(state.textureSlots.size(),23u);EXPECT_FALSE(state.textureEditingAvailable);EXPECT_EQ(state.resourceRevision,0u);
  const auto& normal=state.textureSlots[size_t(GltfMaterialTextureSlot::Normal)];
  EXPECT_TRUE(normal.bound);EXPECT_FALSE(normal.available);EXPECT_FALSE(normal.srgb);
  EXPECT_EQ(normal.authoredTexture,3);EXPECT_EQ(normal.evaluatedTexture,9);EXPECT_EQ(normal.texCoord,1u);
  EXPECT_EQ(normal.offset,(std::array<double,2>{.25,-.5}));EXPECT_EQ(normal.rotation,.75);
  EXPECT_TRUE(state.textureSlots[size_t(GltfMaterialTextureSlot::BaseColor)].srgb);
}

TEST(GuiMaterialTextures, SpecularGlossinessSnapshotIdentifiesOneSharedColorSourceWithLinearAlpha) {
  auto authored=material(),evaluated=material();
  const auto extension=[](int diffuse,int gloss) {return Value(Value::Object{
      {"diffuseTexture",Value(Value::Object{{"index",Value(diffuse)}})},
      {"specularGlossinessTexture",Value(Value::Object{{"index",Value(gloss)}})}});};
  authored.extensions["KHR_materials_pbrSpecularGlossiness"]=extension(3,7);
  evaluated.extensions["KHR_materials_pbrSpecularGlossiness"]=extension(9,11);
  const auto state=BuildMaterialInspectorSnapshot(target(),authored,evaluated,2);
  const auto& color=state.textureSlots[size_t(GltfMaterialTextureSlot::SpecularColor)];
  const auto& roughness=state.textureSlots[size_t(GltfMaterialTextureSlot::Roughness)];
  EXPECT_EQ(state.textureSlots[0].authoredTexture,3);EXPECT_EQ(state.textureSlots[0].evaluatedTexture,9);
  EXPECT_EQ(color.authoredTexture,7);EXPECT_EQ(roughness.authoredTexture,7);
  EXPECT_EQ(color.evaluatedTexture,11);EXPECT_EQ(roughness.evaluatedTexture,11);
  EXPECT_TRUE(color.srgb);EXPECT_TRUE(roughness.srgb);
  EXPECT_EQ(color.coupledSlot,GltfMaterialTextureSlot::Roughness);
  EXPECT_EQ(roughness.coupledSlot,GltfMaterialTextureSlot::SpecularColor);
}

TEST(GuiMaterialTextures, MetallicRoughnessAndSeparateLegacyRoughnessKeepTheirDataRole) {
  auto source=material();tinygltf::Parameter roughness;roughness.json_double_value={{"index",4}};
  source.values["roughnessTexture"]=roughness;
  for(const bool sg:{false,true}) {
    if(sg) source.extensions["KHR_materials_pbrSpecularGlossiness"]=Value(Value::Object{});
    const auto state=BuildMaterialInspectorSnapshot(target(),source,source,2);
    const auto& slot=state.textureSlots[size_t(GltfMaterialTextureSlot::Roughness)];
    EXPECT_FALSE(slot.srgb);EXPECT_FALSE(slot.coupledSlot);EXPECT_TRUE(slot.bound);EXPECT_EQ(slot.authoredTexture,4);
  }
}

TEST(GuiMaterialTextures, SharedSpecularGlossinessSnapshotsRejectDivergentRolesIdentitiesMappingAndSampler) {
  const auto original=sharedSpecularGlossinessSnapshot();GuiManager gui;gui.setMaterialInspectorSnapshot(original);
  for(const auto& mutate:std::vector<std::function<void(MaterialInspectorTexture&)>>{
      [](auto& slot){slot.srgb=false;},[](auto& slot){slot.bound=false;},
      [](auto& slot){slot.authoredTexture=8;},[](auto& slot){slot.evaluatedTexture=11;},
      [](auto& slot){slot.offset[0]=.25;},[](auto& slot){slot.sampler.wrapS=33071;},
      [](auto& slot){slot.sourceField="roughnessTexture";}}) {
    auto damaged=original;mutate(damaged.textureSlots[size_t(GltfMaterialTextureSlot::Roughness)]);
    EXPECT_THROW(gui.setMaterialInspectorSnapshot(std::move(damaged)),std::invalid_argument);
    EXPECT_TRUE(gui.materialInspectorSnapshot().textureSlots[size_t(GltfMaterialTextureSlot::Roughness)].srgb);
  }
}

TEST(GuiMaterialTextures, SharedSpecularGlossinessPickerRequiresColorWhileOrdinaryRoughnessRequiresData) {
  using Operation=container::material::MaterialTextureEditOperation;
  GuiManager gui;gui.setMaterialInspectorSnapshot(sharedSpecularGlossinessSnapshot());
  for(const auto slot:{GltfMaterialTextureSlot::Roughness,GltfMaterialTextureSlot::SpecularColor}) {
    auto request=textureRequest(Operation::BindExisting,slot);request.request.sourceTexture=12;
    EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
    request.request.sourceTexture=8;gui.queueMaterialTextureEditRequest(request);
    const auto queued=gui.consumeMaterialTextureEditRequest();ASSERT_TRUE(queued);EXPECT_EQ(queued->request.sourceTexture,8u);
  }
  gui.setMaterialInspectorSnapshot(textureSnapshot());auto request=textureRequest(Operation::BindExisting,GltfMaterialTextureSlot::Roughness);
  request.request.sourceTexture=12;gui.queueMaterialTextureEditRequest(request);EXPECT_TRUE(gui.consumeMaterialTextureEditRequest());
}

TEST(GuiMaterialTextures, EveryRegisteredSlotUsesTheSameGuardedDiscreteQueueWithoutRuntimeHandles) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(textureSnapshot());
  for(const auto& info:GltfMaterialTextureSlotRegistry()) {
    auto request=textureRequest(container::material::MaterialTextureEditOperation::ImportFile,info.slot);
    request.request.path="texture.png";gui.queueMaterialTextureEditRequest(request);
    auto actual=gui.consumeMaterialTextureEditRequest();ASSERT_TRUE(actual);EXPECT_EQ(actual->target,target());
    EXPECT_EQ(actual->request.slot,info.slot);EXPECT_EQ(actual->request.path,"texture.png");
    EXPECT_FALSE(actual->request.sourceTexture);EXPECT_EQ(actual->request.expectedResourceRevision,19u);
  }
}

TEST(GuiMaterialTextures, StaleCatalogAndSelectionRejectWithoutReplacingAnExistingRequestOrFactorPreview) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(textureSnapshot());gui.queueMaterialEditRequest(numeric(Phase::Begin,51));
  auto stale=textureRequest();stale.request.expectedResourceRevision=18;
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(stale),std::invalid_argument);
  stale=textureRequest();++stale.target.assetIdentity;
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(stale),std::invalid_argument);
  const auto preview=gui.consumeMaterialEditRequests();ASSERT_EQ(preview.size(),1u);EXPECT_EQ(preview[0].request.phase,Phase::Begin);
  gui.queueMaterialTextureEditRequest(textureRequest());
  auto invalid=textureRequest();invalid.request.path="forbidden.png";
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(invalid),std::invalid_argument);
  const auto retained=gui.consumeMaterialTextureEditRequest();ASSERT_TRUE(retained);
  EXPECT_EQ(retained->request.operation,container::material::MaterialTextureEditOperation::Clear);
}

TEST(GuiMaterialTextures, ExistingTextureChoicesRetainAuthoredIdentityAndEnforceTheSelectedRole) {
  using Operation=container::material::MaterialTextureEditOperation;
  GuiManager gui;gui.setMaterialInspectorSnapshot(textureSnapshot());
  auto request=textureRequest(Operation::BindExisting);request.request.sourceTexture=8;
  gui.queueMaterialTextureEditRequest(request);auto actual=gui.consumeMaterialTextureEditRequest();ASSERT_TRUE(actual);
  EXPECT_EQ(actual->request.sourceTexture,8u); // Evaluated ID11 is not substituted.
  request.request.sourceTexture=12;EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  request.request.slot=GltfMaterialTextureSlot::Normal;gui.queueMaterialTextureEditRequest(request);
  actual=gui.consumeMaterialTextureEditRequest();ASSERT_TRUE(actual);EXPECT_EQ(actual->request.sourceTexture,12u);
  request.request.sourceTexture=99;EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
}

TEST(GuiMaterialTextures, MappingAndSamplerCapabilitiesAreIndependentAndMalformedPayloadsNeverQueue) {
  using Operation=container::material::MaterialTextureEditOperation;
  auto state=textureSnapshot();state.textureSlots[0].mappingEditable=false;state.textureSlots[0].samplerEditable=false;
  GuiManager gui;gui.setMaterialInspectorSnapshot(state);
  auto request=textureRequest(Operation::EditMapping);request.request.mapping.offset=std::array<double,2>{0,.5};
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  request=textureRequest(Operation::ImportFile);request.request.path="new.png";request.request.mapping.texCoord=1u;
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  request=textureRequest(Operation::EditSampler);request.request.sampler=container::material::MaterialTextureSamplerDescription{};
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  state=textureSnapshot();gui.setMaterialInspectorSnapshot(state);
  request.request.sampler->wrapS=0;EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  request=textureRequest(Operation::EditMapping);request.request.mapping.rotation=std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  request.request.mapping.rotation=0.;request.request.mapping.channel=0u;
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(request),std::invalid_argument);
  EXPECT_FALSE(gui.consumeMaterialTextureEditRequest());
}

TEST(GuiMaterialTextures, TextureActionAndBindingSelectionCannotRaceAndResetDropsOnlyQueuedState) {
  GuiManager gui;auto state=textureSnapshot();state.choices={{7,9,2,"Left"},{8,17,3,"Right"}};gui.setMaterialInspectorSnapshot(state);
  gui.queueMaterialTextureEditRequest(textureRequest());
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(assignRequest()),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialBindingSelectionRequest(8),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialEditRequest(numeric(Phase::Commit,0)),std::invalid_argument);
  gui.resetModelInspectionState();EXPECT_FALSE(gui.consumeMaterialTextureEditRequest());
  EXPECT_FALSE(gui.materialInspectorSnapshot().textureEditingAvailable);
}

TEST(GuiMaterialTextures, PendingTextureKeepsAnExistingGestureEndAndRetainsDelayedSelectionGuards) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(textureSnapshot());
  gui.queueMaterialEditRequest(numeric(Phase::Begin,71));gui.queueMaterialTextureEditRequest(textureRequest());
  EXPECT_NO_THROW(gui.queueMaterialEditRequest(numeric(Phase::End,71)));
  auto changed=textureSnapshot();++changed.target.assetIdentity;gui.setMaterialInspectorSnapshot(changed);
  const auto queued=gui.consumeMaterialTextureEditRequest();ASSERT_TRUE(queued);EXPECT_EQ(queued->target,target());
  const auto edits=gui.consumeMaterialEditRequests();ASSERT_EQ(edits.size(),2u);
  EXPECT_EQ(edits[0].request.phase,Phase::Begin);EXPECT_EQ(edits[1].request.phase,Phase::End);
}

TEST(GuiMaterialTextures, SnapshotRejectsAmbiguousCatalogsInvalidMappingsAndUnprovedCapabilities) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(textureSnapshot());auto state=textureSnapshot();
  state.textureChoices.push_back(state.textureChoices.front());
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(state),std::invalid_argument);
  state=textureSnapshot();state.textureSlots[0].scale[0]=std::numeric_limits<double>::infinity();
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(state),std::invalid_argument);
  state=textureSnapshot();state.resourceRevision=0;EXPECT_THROW(gui.setMaterialInspectorSnapshot(state),std::invalid_argument);
  state=textureSnapshot();state.textureChoices[0].file=std::string(65537,'x');
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(state),std::invalid_argument);
  EXPECT_EQ(gui.materialInspectorSnapshot().resourceRevision,19u);
}

TEST(GuiMaterialAuthoring, CompactFactoryReadsAllKnownDefaultsWithoutCopyingOpaquePayloads) {
  auto source=material();source.extras=Value(Value::Object{{"largeOpaque",Value(std::string(65536,'x'))}});
  auto result=BuildMaterialInspectorSnapshot(target(),source,source,17);
  ASSERT_TRUE(result.available);EXPECT_EQ(result.sharedUsers,17u);EXPECT_EQ(result.name,"Paint");
  EXPECT_EQ(result.numeric.size(),32u);EXPECT_EQ(result.extensions.size(),13u);
  for(const auto& info:GltfMaterialPropertyRegistry()) EXPECT_EQ(result.numeric[size_t(info.property)].components,info.components);
  EXPECT_EQ(result.numeric[size_t(GltfMaterialProperty::Metallic)].authored[0],.4);
  EXPECT_EQ(result.numeric[size_t(GltfMaterialProperty::Metallic)].defaults[0],1);
  EXPECT_EQ(result.numeric[size_t(GltfMaterialProperty::Ior)].defaults[0],1.5);
  EXPECT_EQ(result.numeric[size_t(GltfMaterialProperty::IridescenceIor)].defaults[0],1.3);
  EXPECT_TRUE(std::isinf(result.numeric[size_t(GltfMaterialProperty::AttenuationDistance)].authored[0]));
  EXPECT_FALSE(result.numeric[size_t(GltfMaterialProperty::AttenuationDistance)].present);
  EXPECT_EQ(result.numeric[size_t(GltfMaterialProperty::DiffuseTransmissionColor)].authored[0],1);
  EXPECT_EQ(result.numeric[size_t(GltfMaterialProperty::SheenColor)].authored[0],0);
  EXPECT_FALSE(result.extensions[size_t(GltfMaterialExtension::Transmission)]);
}

TEST(GuiMaterialAuthoring, FactorySeparatesAuthoredZeroFromAnimatedReadoutAndPresence) {
  auto authored=material(),evaluated=material();
  authored.extensions["KHR_materials_transmission"]=Value(Value::Object{{"transmissionFactor",Value(0.)}});
  evaluated.extensions["KHR_materials_transmission"]=Value(Value::Object{{"transmissionFactor",Value(.75)}});
  std::array<bool,size_t(GltfMaterialProperty::Count)> animated{};
  std::array<bool,size_t(GltfMaterialExtension::Count)> extensions{};
  animated[size_t(GltfMaterialProperty::Transmission)]=true;extensions[size_t(GltfMaterialExtension::Transmission)]=true;
  const auto result=BuildMaterialInspectorSnapshot(target(),authored,evaluated,2,{},animated,extensions);
  const auto& value=result.numeric[size_t(GltfMaterialProperty::Transmission)];
  EXPECT_TRUE(value.present);EXPECT_TRUE(value.animated);EXPECT_EQ(value.authored[0],0);EXPECT_EQ(value.evaluated[0],.75);
  EXPECT_TRUE(result.extensions[size_t(GltfMaterialExtension::Transmission)]);
  EXPECT_TRUE(result.animatedExtensions[size_t(GltfMaterialExtension::Transmission)]);
  EXPECT_EQ(authored.extensions.at("KHR_materials_transmission").Get("transmissionFactor").GetNumberAsDouble(),0);
}

TEST(GuiMaterialAuthoring, FactoryRejectsMalformedKnownValuesAndUnboundedChoiceMetadata) {
  auto bad=material();bad.pbrMetallicRoughness.baseColorFactor={1,0};
  EXPECT_THROW((void)BuildMaterialInspectorSnapshot(target(),bad,material(),1),std::invalid_argument);
  bad=material();bad.extensions["KHR_materials_sheen"]=Value(Value::Object{{"sheenColorFactor",Value(2.)}});
  EXPECT_THROW((void)BuildMaterialInspectorSnapshot(target(),bad,material(),1),std::invalid_argument);
  bad=material();bad.pbrMetallicRoughness.metallicFactor=std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW((void)BuildMaterialInspectorSnapshot(target(),bad,material(),1),std::invalid_argument);
  const std::array<MaterialBindingChoice,1> choices{{{7,9,2,std::string(257,'x')}}};
  EXPECT_THROW((void)BuildMaterialInspectorSnapshot(target(),material(),material(),1,choices),std::invalid_argument);
}

TEST(GuiMaterialAuthoring, OrderedQueueKeepsBeginUpdateEndTogetherAndExactZeroFalseAndReset) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(snapshot());
  gui.queueMaterialEditRequest(numeric(Phase::Begin,31,.4));
  gui.queueMaterialEditRequest(numeric(Phase::Update,31,0));
  gui.queueMaterialEditRequest(numeric(Phase::End,31,0));
  auto commit=numeric(Phase::Commit,0);commit.request.patch.values[0].value=std::monostate{};
  commit.request.patch.doubleSided=false;gui.queueMaterialEditRequest(commit);
  const auto requests=gui.consumeMaterialEditRequests();ASSERT_EQ(requests.size(),4u);
  EXPECT_EQ(requests[0].request.phase,Phase::Begin);EXPECT_EQ(requests[1].request.phase,Phase::Update);
  EXPECT_EQ(requests[2].request.phase,Phase::End);EXPECT_EQ(requests[0].request.gestureId,requests[2].request.gestureId);
  EXPECT_EQ(std::get<double>(requests[1].request.patch.values[0].value),0);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(requests[3].request.patch.values[0].value));
  ASSERT_TRUE(requests[3].request.patch.doubleSided);EXPECT_FALSE(*requests[3].request.patch.doubleSided);
  EXPECT_TRUE(gui.consumeMaterialEditRequests().empty());
  for(const auto& info:GltfMaterialPropertyRegistry()) {
    auto reset=numeric(Phase::Commit,0);reset.request.patch.values={{info.property,std::monostate{}}};
    gui.queueMaterialEditRequest(std::move(reset));
  }
  for(size_t i=0;i<size_t(GltfMaterialExtension::Count);++i) {
    auto feature=numeric(Phase::Commit,0);feature.request.patch.values.clear();
    feature.request.patch.extensions={{GltfMaterialExtension(i),false}};gui.queueMaterialEditRequest(std::move(feature));
  }
  const auto all=gui.consumeMaterialEditRequests();ASSERT_EQ(all.size(),45u);
  for(size_t i=32;i<all.size();++i) EXPECT_FALSE(all[i].request.patch.extensions[0].enabled);
}

TEST(GuiMaterialAuthoring, InvalidOrStaleRequestsNeverOverwriteAnEarlierPendingEdit) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(snapshot());gui.queueMaterialEditRequest(numeric(Phase::Commit,0));
  auto stale=numeric(Phase::Commit,0);++stale.target.authoredRevision;
  EXPECT_THROW(gui.queueMaterialEditRequest(stale),std::invalid_argument);
  stale=numeric(Phase::Commit,0);++stale.target.assetIdentity;EXPECT_THROW(gui.queueMaterialEditRequest(stale),std::invalid_argument);
  stale=numeric(Phase::Commit,0);stale.request.sourceMaterial=1;EXPECT_THROW(gui.queueMaterialEditRequest(stale),std::invalid_argument);
  auto invalid=numeric(Phase::Commit,0);invalid.request.patch.values[0].value=-.1;
  EXPECT_THROW(gui.queueMaterialEditRequest(invalid),std::invalid_argument);
  invalid=numeric(Phase::Commit,0);invalid.request.patch.values[0].value=std::array<double,3>{1,0,0};
  EXPECT_THROW(gui.queueMaterialEditRequest(invalid),std::invalid_argument);
  invalid=numeric(Phase::Cancel,7);invalid.request.patch.doubleSided=false;
  EXPECT_THROW(gui.queueMaterialEditRequest(invalid),std::invalid_argument);
  ASSERT_EQ(gui.consumeMaterialEditRequests().size(),1u);
}

TEST(GuiMaterialAuthoring, BindingChoicesRemainSeparateFromMaterialEditsAndResetClearsPendingState) {
  GuiManager gui;auto state=snapshot();state.choices={{7,9,2,"Paint"},{8,10,3,"Glass"}};
  gui.setMaterialInspectorSnapshot(state);gui.queueMaterialBindingSelectionRequest(8);
  EXPECT_THROW(gui.queueMaterialBindingSelectionRequest(400),std::invalid_argument);
  const auto choice=gui.consumeMaterialBindingSelectionRequest();ASSERT_TRUE(choice);
  EXPECT_EQ(choice->target,target());EXPECT_EQ(choice->bindingGraphNode,8u);EXPECT_TRUE(gui.consumeMaterialEditRequests().empty());
  gui.queueMaterialEditRequest(numeric(Phase::Commit,0));gui.queueMaterialBindingSelectionRequest(7);
  gui.resetModelInspectionState();EXPECT_FALSE(gui.materialInspectorSnapshot().available);
  EXPECT_TRUE(gui.consumeMaterialEditRequests().empty());EXPECT_FALSE(gui.consumeMaterialBindingSelectionRequest());
}

TEST(GuiMaterialAuthoring, BindingCatalogRejectsAmbiguousUnboundedAndUnprovedCapabilities) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(bindingSnapshot());
  auto bad=bindingSnapshot();bad.assignmentChoices[1].runtimeMaterial=9;
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.assignmentChoices[1].sourceMaterial=2;
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.assignmentChoices[1].sourceMaterial=-2;
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.assignmentChoices[1].label=std::string(257,'x');
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.assignmentChoices.resize(65537);
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.bindingOperationsError=std::string(4097,'x');
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.target.bindingGraphNode=UINT32_MAX;
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.available=false;
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  bad=bindingSnapshot();bad.bindingOperationsAvailable=false;
  EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  EXPECT_EQ(gui.materialInspectorSnapshot().target,target());
  EXPECT_TRUE(gui.materialInspectorSnapshot().bindingOperationsAvailable);
}

TEST(GuiMaterialAuthoring, AssignmentRequiresEveryTargetGuardAndTheExactAdvertisedIdentityPair) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(bindingSnapshot());
  for(size_t field=0;field<7;++field) {
    auto stale=assignRequest();auto& t=stale.target;
    switch(field) {
    case 0:++t.graphIdentity;break;case 1:++t.assetIdentity;break;case 2:++t.authoredRevision;break;
    case 3:++t.selectedGraphNode;break;case 4:++t.bindingGraphNode;break;case 5:++t.runtimeMaterial;break;
    default:++t.sourceMaterial;break;
    }
    EXPECT_THROW(gui.queueMaterialBindingOperationRequest(stale),std::invalid_argument);
  }
  auto bad=assignRequest();bad.sourceMaterial=2;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  bad=assignRequest();bad.runtimeMaterial=18;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  bad=assignRequest();bad.sourceMaterial=2;bad.runtimeMaterial=9;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  bad=assignRequest();bad.name="Unexpected rename";
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  bad=assignRequest();bad.operation=MaterialBindingOperation(99);
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  EXPECT_FALSE(gui.consumeMaterialBindingOperationRequest());
  gui.setMaterialInspectorSnapshot(snapshot());
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(assignRequest()),std::invalid_argument);
}

TEST(GuiMaterialAuthoring, OneShotBindingQueueDoesNotOverwriteOrForgeFactorGestureBoundaries) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(bindingSnapshot());
  gui.queueMaterialEditRequest(numeric(Phase::Begin,31,.4));
  gui.queueMaterialBindingOperationRequest(assignRequest());
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(uniqueRequest()),std::invalid_argument);
  const auto factors=gui.consumeMaterialEditRequests();ASSERT_EQ(factors.size(),1u);
  EXPECT_EQ(factors[0].request.phase,Phase::Begin);
  const auto request=gui.consumeMaterialBindingOperationRequest();ASSERT_TRUE(request);
  EXPECT_EQ(request->target,target());EXPECT_EQ(request->operation,MaterialBindingOperation::Assign);
  EXPECT_EQ(request->sourceMaterial,3);EXPECT_EQ(request->runtimeMaterial,17u);EXPECT_FALSE(request->name);
  EXPECT_FALSE(gui.consumeMaterialBindingOperationRequest());
  EXPECT_EQ(gui.materialInspectorSnapshot().target.runtimeMaterial,9u);
}

TEST(GuiMaterialAuthoring, RuntimeSurfaceCanAssignAnExplicitMaterialWithoutFactorEditorAvailability) {
  GuiManager gui;auto state=bindingSnapshot();state.available=false;state.makeUniqueAvailable=false;
  state.target.sourceMaterial=-1;state.target.runtimeMaterial=23;gui.setMaterialInspectorSnapshot(state);
  auto request=assignRequest();request.target=state.target;gui.queueMaterialBindingOperationRequest(request);
  const auto queued=gui.consumeMaterialBindingOperationRequest();ASSERT_TRUE(queued);
  EXPECT_EQ(queued->target.sourceMaterial,-1);EXPECT_EQ(queued->runtimeMaterial,17u);
  auto unique=uniqueRequest();unique.target=state.target;unique.sourceMaterial=-1;unique.runtimeMaterial=23;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(unique),std::invalid_argument);
  state.available=true;state.target=target();state.makeUniqueAvailable=true;gui.setMaterialInspectorSnapshot(state);
  request=assignRequest();request.sourceMaterial=-1;request.runtimeMaterial=23;
  gui.queueMaterialBindingOperationRequest(request);
  EXPECT_EQ(gui.consumeMaterialBindingOperationRequest()->sourceMaterial,-1);
}

TEST(GuiMaterialAuthoring, MakeUniqueRequiresExactSelectedSourceAndBoundsOptionalName) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(bindingSnapshot());
  auto bad=uniqueRequest();bad.sourceMaterial=3;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  bad=uniqueRequest();bad.runtimeMaterial=17;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  bad=uniqueRequest();bad.name=std::string(4097,'x');
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(bad),std::invalid_argument);
  auto request=uniqueRequest();request.name="Copper finish";gui.queueMaterialBindingOperationRequest(request);
  const auto queued=gui.consumeMaterialBindingOperationRequest();ASSERT_TRUE(queued);
  EXPECT_EQ(queued->operation,MaterialBindingOperation::MakeUnique);EXPECT_EQ(queued->sourceMaterial,2);
  EXPECT_EQ(queued->runtimeMaterial,9u);EXPECT_EQ(queued->name,"Copper finish");
  auto disabled=bindingSnapshot();disabled.makeUniqueAvailable=false;gui.setMaterialInspectorSnapshot(disabled);
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(uniqueRequest()),std::invalid_argument);
}

TEST(GuiMaterialAuthoring, RuntimeMakeUniqueRequiresExplicitCapabilityAndRetainsExactRuntimeGuard) {
  GuiManager gui;auto state=bindingSnapshot();state.available=false;state.makeUniqueAvailable=true;
  state.target.sourceMaterial=-1;state.target.runtimeMaterial=23;gui.setMaterialInspectorSnapshot(state);
  auto request=uniqueRequest();request.target=state.target;request.sourceMaterial=-1;request.runtimeMaterial=23;
  auto stale=request;stale.runtimeMaterial=9;
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(stale),std::invalid_argument);
  gui.queueMaterialBindingOperationRequest(request);const auto queued=gui.consumeMaterialBindingOperationRequest();ASSERT_TRUE(queued);
  EXPECT_EQ(queued->target,state.target);EXPECT_EQ(queued->sourceMaterial,-1);EXPECT_EQ(queued->runtimeMaterial,23u);
  EXPECT_TRUE(gui.consumeMaterialEditRequests().empty());
}

TEST(GuiMaterialAuthoring, SurfacePreferenceAndBindingActionCannotRaceAndResetDropsPendingAction) {
  GuiManager gui;auto state=bindingSnapshot();state.choices={{7,9,2,"Paint"},{8,17,3,"Glass"}};
  gui.setMaterialInspectorSnapshot(state);gui.queueMaterialBindingSelectionRequest(8);
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(assignRequest()),std::invalid_argument);
  const auto selection=gui.consumeMaterialBindingSelectionRequest();ASSERT_TRUE(selection);
  EXPECT_EQ(selection->bindingGraphNode,8u);EXPECT_FALSE(gui.consumeMaterialBindingOperationRequest());
  gui.queueMaterialBindingOperationRequest(assignRequest());
  EXPECT_THROW(gui.queueMaterialBindingSelectionRequest(8),std::invalid_argument);
  auto replacement=state;++replacement.target.graphIdentity;gui.setMaterialInspectorSnapshot(replacement);
  const auto original=gui.consumeMaterialBindingOperationRequest();ASSERT_TRUE(original);
  EXPECT_EQ(original->target.graphIdentity,11u); // Delayed actions retain their old guard.
  gui.setMaterialInspectorSnapshot(state);gui.queueMaterialBindingOperationRequest(uniqueRequest());
  gui.resetModelInspectionState();EXPECT_FALSE(gui.consumeMaterialBindingOperationRequest());
  EXPECT_FALSE(gui.materialInspectorSnapshot().bindingOperationsAvailable);
}

TEST(GuiMaterialRetirement, UnusedMiddleAndRuntimeRowsCanBeDeletedWithoutSelectingASurface) {
  GuiManager gui;auto state=catalogSnapshot(false);gui.setMaterialInspectorSnapshot(state);
  gui.queueMaterialRetirementRequest({state.catalogTarget});
  const auto request=gui.consumeMaterialRetirementRequest();ASSERT_TRUE(request);
  EXPECT_EQ(request->target.runtimeMaterial,17u);EXPECT_EQ(request->target.sourceMaterial,3);
  EXPECT_TRUE(gui.consumeMaterialEditRequests().empty());EXPECT_FALSE(gui.consumeMaterialBindingOperationRequest());
  state.catalogTarget.runtimeMaterial=23;state.catalogTarget.sourceMaterial=-1;gui.setMaterialInspectorSnapshot(state);
  gui.queueMaterialRetirementRequest({state.catalogTarget});
  EXPECT_EQ(gui.consumeMaterialRetirementRequest()->target.sourceMaterial,-1);
}

TEST(GuiMaterialRetirement, CatalogRejectsAmbiguousUnboundedAndUnprovedSnapshots) {
  GuiManager gui;gui.setMaterialInspectorSnapshot(catalogSnapshot());
  for(const auto& change:std::vector<std::function<void(MaterialInspectorSnapshot&)>>{
      [](auto& s){s.catalogChoices[1].runtimeMaterial=9;},[](auto& s){s.catalogChoices[1].sourceMaterial=2;},
      [](auto& s){s.catalogChoices[1].label=std::string(257,'x');},[](auto& s){s.catalogChoices.resize(65537);},
      [](auto& s){s.catalogTarget.runtimeMaterial=18;},[](auto& s){s.catalogTarget.assetIdentity=0;},
      [](auto& s){s.catalogTarget.sourceMaterial=-2;},[](auto& s){s.materialRetirementReason="Referenced";},
      [](auto& s){s.materialCatalogError=std::string(4097,'x');}}) {
    auto bad=catalogSnapshot();change(bad);EXPECT_THROW(gui.setMaterialInspectorSnapshot(bad),std::invalid_argument);
  }
  EXPECT_EQ(gui.materialInspectorSnapshot().catalogTarget.runtimeMaterial,17u);
}

TEST(GuiMaterialRetirement, EveryWeakGuardAndAdvertisedSourceRuntimePairIsRequired) {
  GuiManager gui;const auto state=catalogSnapshot();gui.setMaterialInspectorSnapshot(state);
  for(size_t field=0;field<6;++field) {
    auto target=state.catalogTarget;
    switch(field) {
    case 0:++target.graphIdentity;break;case 1:++target.assetIdentity;break;case 2:++target.authoredRevision;break;
    case 3:++target.materialRevision;break;case 4:++target.runtimeMaterial;break;default:++target.sourceMaterial;break;
    }
    EXPECT_THROW(gui.queueMaterialRetirementRequest({target}),std::invalid_argument);
  }
  EXPECT_THROW(gui.queueMaterialCatalogSelectionRequest({state.catalogTarget,2,17}),std::invalid_argument);
  auto blocked=state;blocked.materialRetirementAvailable=false;blocked.materialRetirementReason="A source primitive uses this material.";
  gui.setMaterialInspectorSnapshot(blocked);EXPECT_THROW(gui.queueMaterialRetirementRequest({blocked.catalogTarget}),std::invalid_argument);
  EXPECT_FALSE(gui.consumeMaterialRetirementRequest());
}

TEST(GuiMaterialRetirement, QueuedDeletionKeepsItsOldGuardAndDoesNotEraseFactorGesture) {
  GuiManager gui;auto state=catalogSnapshot();gui.setMaterialInspectorSnapshot(state);
  gui.queueMaterialEditRequest(numeric(Phase::Begin,71,.6));gui.queueMaterialRetirementRequest({state.catalogTarget});
  EXPECT_THROW(gui.queueMaterialRetirementRequest({state.catalogTarget}),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialCatalogSelectionRequest({state.catalogTarget,2,9}),std::invalid_argument);
  gui.queueMaterialEditRequest(numeric(Phase::End,71,.6));
  state.catalogChoices.erase(state.catalogChoices.begin()+1);state.catalogTarget={11,23,6,42,9,2};
  state.materialRetirementAvailable=false;state.materialRetirementReason="This material is in use.";
  gui.setMaterialInspectorSnapshot(state);
  const auto queued=gui.consumeMaterialRetirementRequest();ASSERT_TRUE(queued);
  EXPECT_EQ(queued->target.runtimeMaterial,17u);EXPECT_EQ(queued->target.authoredRevision,5u);
  const auto factors=gui.consumeMaterialEditRequests();ASSERT_EQ(factors.size(),2u);
  EXPECT_EQ(factors[0].request.phase,Phase::Begin);EXPECT_EQ(factors[1].request.phase,Phase::End);
  EXPECT_FALSE(gui.consumeMaterialRetirementRequest());
}

TEST(GuiMaterialRetirement, CatalogSelectionOnlyQueuesAnIntentAndResetDropsBothQueues) {
  GuiManager gui;const auto state=catalogSnapshot(false);gui.setMaterialInspectorSnapshot(state);
  gui.queueMaterialCatalogSelectionRequest({state.catalogTarget,2,9});
  EXPECT_EQ(gui.materialInspectorSnapshot().catalogTarget.runtimeMaterial,17u);
  EXPECT_THROW(gui.queueMaterialRetirementRequest({state.catalogTarget}),std::invalid_argument);
  const auto selection=gui.consumeMaterialCatalogSelectionRequest();ASSERT_TRUE(selection);EXPECT_EQ(selection->runtimeMaterial,9u);
  EXPECT_FALSE(gui.consumeMaterialRetirementRequest());gui.queueMaterialRetirementRequest({state.catalogTarget});
  gui.resetModelInspectionState();EXPECT_FALSE(gui.consumeMaterialRetirementRequest());EXPECT_TRUE(gui.materialInspectorSnapshot().catalogChoices.empty());
  gui.setMaterialInspectorSnapshot(state);gui.queueMaterialCatalogSelectionRequest({state.catalogTarget,2,9});
  gui.resetModelInspectionState();EXPECT_FALSE(gui.consumeMaterialCatalogSelectionRequest());
}

class MaterialWidgets : public ::testing::Test {
protected:
  void SetUp() override {
    previous=ImGui::GetCurrentContext();context=ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.DisplaySize={900,1700};io.DeltaTime=1.f/60;
    unsigned char* pixels{};int width{},height{};io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    gui.setMaterialInspectorSnapshot(snapshot());frame();frame();
  }
  void TearDown() override {ImGui::DestroyContext(context);ImGui::SetCurrentContext(previous);}
  void frame() {
    ImGui::NewFrame();ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({890,1690});
    ImGui::Begin("Material tests",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
    gui.drawMaterialInspector();ImGui::End();ImGui::Render();
    auto batch=gui.consumeMaterialEditRequests();for(auto& item:batch) requests.push_back(std::move(item));
    if(auto operation=gui.consumeMaterialBindingOperationRequest()) bindingRequests.push_back(std::move(*operation));
    if(auto operation=gui.consumeMaterialTextureEditRequest()) textureRequests.push_back(std::move(*operation));
    if(auto selection=gui.consumeMaterialCatalogSelectionRequest()) catalogSelections.push_back(std::move(*selection));
    if(auto retirement=gui.consumeMaterialRetirementRequest()) retirementRequests.push_back(std::move(*retirement));
  }
  ImGuiID id(const char* label,std::optional<GltfMaterialProperty> property={},bool numericComponent=false,int component=0) const {
    auto* window=ImGui::FindWindowByName("Material tests");auto seed=ImHashStr("MaterialInspector",0,window->ID);
    const auto& t=gui.materialInspectorSnapshot().target;
    const std::array<uint64_t,6> ids{t.graphIdentity,t.assetIdentity,t.selectedGraphNode,t.bindingGraphNode,t.runtimeMaterial,uint64_t(t.sourceMaterial)};
    const int targetId=int(ImHashData(ids.data(),sizeof(ids)));seed=ImHashData(&targetId,sizeof(targetId),seed);
    if(property) {const int p=int(*property);seed=ImHashData(&p,sizeof(p),seed);}
    if(!numericComponent) return ImHashStr(label,0,seed);
    // ImGui 1.91.8 DragScalarN scopes each component and submits DragScalar("").
    seed=ImHashStr("##Value",0,seed);seed=ImHashData(&component,sizeof(component),seed);return ImHashStr("",0,seed);
  }
  ImVec2 locate(ImGuiID item,float x=35.f,bool required=true) {
    for(float y=35;y<1600;y+=4) {
      ImGui::GetIO().AddMousePosEvent(x,y);frame();
      if(context->HoveredId==item) return {x,y};
    }
    if(required) ADD_FAILURE()<<"Could not find material widget "<<item;return {-100,-100};
  }
  void click(ImVec2 point) {
    auto& io=ImGui::GetIO();io.AddMousePosEvent(point.x,point.y);frame();
    io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
  }
  void key(ImGuiKey value) {auto& io=ImGui::GetIO();io.AddKeyEvent(value,true);frame();io.AddKeyEvent(value,false);frame();}
  void replace(const char* text) {
    auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);frame();io.AddInputCharactersUTF8(text);frame();
  }
  void startDrag(ImVec2 point) {
    auto& io=ImGui::GetIO();io.AddMousePosEvent(point.x,point.y);frame();io.AddMouseButtonEvent(0,true);frame();
    io.AddMousePosEvent(point.x+20,point.y);frame();io.AddMousePosEvent(point.x+30,point.y);frame();
  }
  void release() {ImGui::GetIO().AddMouseButtonEvent(0,false);frame();}
  void chooseAssignment(size_t row) {
    click(locate(id("Assign material")));ASSERT_FALSE(context->OpenPopupStack.empty());
    frame(); // Let the combo popup finish its automatic layout/scroll placement.
    auto* popup=context->OpenPopupStack.back().Window;ASSERT_NE(popup,nullptr);
    const auto& choice=gui.materialInspectorSnapshot().assignmentChoices.at(row);
    const int runtime=int(choice.runtimeMaterial);
    const auto seed=ImHashData(&runtime,sizeof(runtime),popup->ID);
    const auto wanted=ImHashStr(choice.label.c_str(),0,seed);
    const auto bounds=popup->InnerRect;const float x=bounds.Min.x+20;
    for(float y=bounds.Min.y+1;y<bounds.Max.y;y+=2) {
      ImGui::GetIO().AddMousePosEvent(x,y);frame();
      if(context->HoveredId==wanted) {click({x,y});return;}
    }
    FAIL()<<"Could not find assignment item "<<choice.label;
  }
  void chooseCatalog(size_t row) {
    click(locate(id("Material catalog")));ASSERT_FALSE(context->OpenPopupStack.empty());frame();
    auto* popup=context->OpenPopupStack.back().Window;ASSERT_NE(popup,nullptr);
    const auto& choice=gui.materialInspectorSnapshot().catalogChoices.at(row);const int runtime=int(choice.runtimeMaterial);
    const auto seed=ImHashData(&runtime,sizeof(runtime),popup->ID);const auto wanted=ImHashStr(choice.label.c_str(),0,seed);
    const auto bounds=popup->InnerRect;const float x=bounds.Min.x+20;
    for(float y=bounds.Min.y+1;y<bounds.Max.y;y+=2) {
      ImGui::GetIO().AddMousePosEvent(x,y);frame();if(context->HoveredId==wanted) {click({x,y});return;}
    }
    FAIL()<<"Could not find catalog item "<<choice.label;
  }
  ImGuiID textureScope(GltfMaterialTextureSlot slot) const {
    auto seed=id("Texture slots");const int index=int(slot);seed=ImHashData(&index,sizeof(index),seed);
    const auto& state=gui.materialInspectorSnapshot().textureSlots[size_t(slot)];
    const auto label=state.label.empty()?GltfMaterialTextureSlotDescription(slot).label:std::string_view(state.label);
    return ImHashStr(label.data(),label.size(),seed);
  }
  ImGuiID textureItem(const char* label,GltfMaterialTextureSlot slot=GltfMaterialTextureSlot::BaseColor,const char* subtree=nullptr) const {
    auto seed=textureScope(slot);if(subtree) seed=ImHashStr(subtree,0,seed);return ImHashStr(label,0,seed);
  }
  void openTexture(GltfMaterialTextureSlot slot=GltfMaterialTextureSlot::BaseColor) {
    gui.setMaterialInspectorSnapshot(textureSnapshot());frame();click(locate(id("Texture slots")));
    click(locate(textureScope(slot)));frame();
  }
  void chooseTexture(size_t row,GltfMaterialTextureSlot slot=GltfMaterialTextureSlot::BaseColor) {
    // Two open texture tree levels indent these controls past locate's x=35.
    click(locate(textureItem("Existing image",slot),75));ASSERT_FALSE(context->OpenPopupStack.empty());frame();
    auto* popup=context->OpenPopupStack.back().Window;ASSERT_NE(popup,nullptr);
    const auto& choice=gui.materialInspectorSnapshot().textureChoices.at(row);const int source=int(choice.sourceTexture);
    const auto seed=ImHashData(&source,sizeof(source),popup->ID);const auto wanted=ImHashStr(choice.label.c_str(),0,seed);
    const auto bounds=popup->InnerRect;const float x=bounds.Min.x+20;
    for(float y=bounds.Min.y+1;y<bounds.Max.y;y+=2) {
      ImGui::GetIO().AddMousePosEvent(x,y);frame();if(context->HoveredId==wanted) {click({x,y});return;}
    }
    FAIL()<<"Could not find texture item "<<choice.label;
  }
  GuiManager gui;ImGuiContext *previous{},*context{};std::vector<QueuedMaterialEdit> requests;
  std::vector<MaterialBindingOperationRequest> bindingRequests;
  std::vector<QueuedMaterialTextureEdit> textureRequests;
  std::vector<MaterialCatalogSelectionRequest> catalogSelections;
  std::vector<MaterialRetirementRequest> retirementRequests;
};

TEST(GuiResourceCatalog, SelectionAndDeleteWorkWithoutSurfaceOrMaterialSnapshot) {
  GuiManager gui;const auto state=resourceSnapshot();gui.setResourceCatalogSnapshot(state);
  gui.queueResourceCatalogSelectionRequest({state.choices[1].target});
  const auto selection=gui.consumeResourceCatalogSelectionRequest();ASSERT_TRUE(selection);
  EXPECT_EQ(selection->target,state.choices[1].target);EXPECT_FALSE(gui.consumeResourceCatalogSelectionRequest());
  EXPECT_FALSE(gui.consumeResourceRetirementRequest());
  gui.queueResourceRetirementRequest({*state.selected});const auto deletion=gui.consumeResourceRetirementRequest();
  ASSERT_TRUE(deletion);EXPECT_EQ(deletion->target,*state.selected);
  EXPECT_FALSE(gui.consumeResourceRetirementRequest());EXPECT_FALSE(gui.consumeMaterialRetirementRequest());
  EXPECT_FALSE(gui.materialInspectorSnapshot().available);
}

TEST(GuiResourceCatalog, EveryIdentityAndRevisionMustMatchCurrentSelectedRow) {
  GuiManager gui;const auto state=resourceSnapshot();gui.setResourceCatalogSnapshot(state);
  for(const auto member:{&ResourceCatalogTarget::assetIdentity,&ResourceCatalogTarget::resourceRevision,
      &ResourceCatalogTarget::domainIdentity,&ResourceCatalogTarget::incarnation}) {
    auto stale=*state.selected;++(stale.*member);
    EXPECT_THROW(gui.queueResourceRetirementRequest({stale}),std::invalid_argument);
    EXPECT_THROW(gui.queueResourceCatalogSelectionRequest({stale}),std::invalid_argument);
  }
  auto stale=*state.selected;stale.row++;
  EXPECT_THROW(gui.queueResourceRetirementRequest({stale}),std::invalid_argument);
  EXPECT_FALSE(gui.consumeResourceRetirementRequest());EXPECT_FALSE(gui.consumeResourceCatalogSelectionRequest());
  gui.queueResourceRetirementRequest({*state.selected});EXPECT_TRUE(gui.consumeResourceRetirementRequest());
}

TEST(GuiResourceCatalog, BlockedDeleteCannotBeForcedThroughQueue) {
  GuiManager gui;auto state=resourceSnapshot();state.retirementAvailable=false;state.retirementReason="A material uses this texture.";
  gui.setResourceCatalogSnapshot(state);
  EXPECT_THROW(gui.queueResourceRetirementRequest({*state.selected}),std::invalid_argument);
  EXPECT_FALSE(gui.consumeResourceRetirementRequest());
  gui.queueResourceCatalogSelectionRequest({state.choices[1].target});EXPECT_TRUE(gui.consumeResourceCatalogSelectionRequest());
}

TEST(GuiResourceCatalog, InvalidSnapshotLeavesPreviousCatalogAndQueuedActionUnchanged) {
  GuiManager gui;const auto state=resourceSnapshot();gui.setResourceCatalogSnapshot(state);
  gui.queueResourceRetirementRequest({*state.selected});
  for(int error=0;error<9;++error) {
    auto invalid=state;
    if(error==0) invalid.choices[0].target.incarnation=0;
    if(error==1) invalid.choices[0].target.row=1000000;
    if(error==2) invalid.choices[0].target.kind=ResourceCatalogKind::RuntimeTexture;
    if(error==3) invalid.choices.push_back(invalid.choices[0]);
    if(error==4) invalid.choices[0].target.resourceRevision++;
    if(error==5) invalid.choices[0].label=std::string(257,'x');
    if(error==6) invalid.selected.reset();
    if(error==7) invalid.available=false;
    if(error==8) invalid.retirementReason="Still in use";
    EXPECT_THROW(gui.setResourceCatalogSnapshot(std::move(invalid)),std::invalid_argument)<<error;
    EXPECT_EQ(gui.resourceCatalogSnapshot().selected,state.selected);
  }
  const auto deletion=gui.consumeResourceRetirementRequest();ASSERT_TRUE(deletion);EXPECT_EQ(deletion->target,*state.selected);
}

TEST(GuiResourceCatalog, PendingResourceActionsRejectMaterialTransactionsAndOtherResourceActions) {
  GuiManager gui;const auto state=resourceSnapshot();gui.setResourceCatalogSnapshot(state);gui.setMaterialInspectorSnapshot(textureSnapshot());
  gui.queueResourceRetirementRequest({*state.selected});
  EXPECT_THROW(gui.queueResourceCatalogSelectionRequest({state.choices[1].target}),std::invalid_argument);
  EXPECT_THROW(gui.queueResourceRetirementRequest({*state.selected}),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialTextureEditRequest(textureRequest()),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialBindingOperationRequest(assignRequest()),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialEditRequest(numeric(Phase::Commit,0)),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialEditRequest(numeric(Phase::End,11)),std::invalid_argument);
  EXPECT_THROW(gui.queueMaterialEditRequest(numeric(Phase::Cancel,11)),std::invalid_argument);
  auto end=numeric(Phase::End,11);end.request.patch={};EXPECT_NO_THROW(gui.queueMaterialEditRequest(end));
  const auto deactivation=gui.consumeMaterialEditRequests();ASSERT_EQ(deactivation.size(),1u);
  EXPECT_EQ(deactivation[0].request.phase,Phase::End);EXPECT_TRUE(deactivation[0].request.patch.values.empty());
  EXPECT_TRUE(gui.consumeResourceRetirementRequest());EXPECT_FALSE(gui.consumeMaterialTextureEditRequest());
  EXPECT_TRUE(gui.consumeMaterialEditRequests().empty());
  gui.queueMaterialTextureEditRequest(textureRequest());
  EXPECT_THROW(gui.queueResourceRetirementRequest({*state.selected}),std::invalid_argument);
  EXPECT_TRUE(gui.consumeMaterialTextureEditRequest());
}

TEST(GuiResourceCatalog, ResetClearsCatalogAndBothKindsOfPendingAction) {
  GuiManager gui;const auto state=resourceSnapshot();gui.setResourceCatalogSnapshot(state);
  gui.queueResourceRetirementRequest({*state.selected});gui.resetModelInspectionState();
  EXPECT_FALSE(gui.resourceCatalogSnapshot().available);EXPECT_FALSE(gui.consumeResourceRetirementRequest());
  gui.setResourceCatalogSnapshot(state);gui.queueResourceCatalogSelectionRequest({state.choices[1].target});
  gui.resetModelInspectionState();EXPECT_FALSE(gui.consumeResourceCatalogSelectionRequest());
}

TEST_F(MaterialWidgets, ResourceDeleteButtonWorksWithNoMaterialOrSurfaceSelection) {
  const auto state=resourceSnapshot();gui.setMaterialInspectorSnapshot({});gui.setResourceCatalogSnapshot(state);frame();
  auto* window=ImGui::FindWindowByName("Material tests");
  const auto seed=ImHashStr("ResourceCatalog",0,window->ID);const auto wanted=ImHashStr("Delete unused resource",0,seed);
  click(locate(wanted));frame();const auto request=gui.consumeResourceRetirementRequest();
  ASSERT_TRUE(request);EXPECT_EQ(request->target,*state.selected);EXPECT_TRUE(requests.empty());EXPECT_TRUE(bindingRequests.empty());
  EXPECT_TRUE(retirementRequests.empty());EXPECT_EQ(gui.resourceCatalogSnapshot().selected,state.selected);
}

TEST_F(MaterialWidgets, MaterialCatalogPickerQueuesSelectionWithoutAssigningOrDeleting) {
  auto state=catalogSnapshot(false);state.catalogTarget.runtimeMaterial=9;state.catalogTarget.sourceMaterial=2;
  state.materialRetirementAvailable=false;state.materialRetirementReason="A source primitive uses this material.";
  gui.setMaterialInspectorSnapshot(state);frame();chooseCatalog(1);frame();
  ASSERT_EQ(catalogSelections.size(),1u);EXPECT_EQ(catalogSelections[0].runtimeMaterial,17u);EXPECT_EQ(catalogSelections[0].sourceMaterial,3);
  EXPECT_TRUE(retirementRequests.empty());EXPECT_TRUE(bindingRequests.empty());EXPECT_TRUE(requests.empty());
  EXPECT_EQ(gui.materialInspectorSnapshot().catalogTarget.runtimeMaterial,9u);
  state.catalogTarget.runtimeMaterial=17;state.catalogTarget.sourceMaterial=3;state.materialRetirementAvailable=true;state.materialRetirementReason.clear();
  gui.setMaterialInspectorSnapshot(state);frame();click(locate(id("Delete unused material")));frame();
  ASSERT_EQ(retirementRequests.size(),1u);EXPECT_EQ(retirementRequests[0].target.runtimeMaterial,17u);
  EXPECT_EQ(retirementRequests[0].target.sourceMaterial,3);EXPECT_TRUE(bindingRequests.empty());EXPECT_TRUE(requests.empty());
}

TEST_F(MaterialWidgets, DeleteUnusedMaterialButtonHonorsBackendUnavailableReason) {
  auto state=catalogSnapshot(false);gui.setMaterialInspectorSnapshot(state);frame();
  const auto point=locate(id("Delete unused material"));ASSERT_GE(point.x,0);
  state.materialRetirementAvailable=false;state.materialRetirementReason="Retained BIM metadata uses this material.";
  gui.setMaterialInspectorSnapshot(state);frame();click(point);frame();
  EXPECT_TRUE(retirementRequests.empty());EXPECT_TRUE(bindingRequests.empty());EXPECT_TRUE(requests.empty());
}

TEST_F(MaterialWidgets, ScalarDragProducesOneOrderedGestureThroughRelease) {
  const auto position=locate(id("Metallic",GltfMaterialProperty::Metallic,true));startDrag(position);release();frame();
  ASSERT_GE(requests.size(),3u);EXPECT_EQ(requests.front().request.phase,Phase::Begin);EXPECT_EQ(requests.back().request.phase,Phase::End);
  EXPECT_EQ(std::count_if(requests.begin(),requests.end(),[](const auto& r){return r.request.phase==Phase::Begin;}),1);
  EXPECT_EQ(std::count_if(requests.begin(),requests.end(),[](const auto& r){return r.request.phase==Phase::End;}),1);
  for(const auto& request:requests) EXPECT_EQ(request.request.gestureId,requests.front().request.gestureId);
  EXPECT_GT(std::get<double>(requests.back().request.patch.values[0].value),.4);
}

TEST_F(MaterialWidgets, EscapeCancelsAnActualNumericDragWithoutAnEndOrCommit) {
  const auto position=locate(id("Metallic",GltfMaterialProperty::Metallic,true));startDrag(position);key(ImGuiKey_Escape);release();frame();
  ASSERT_GE(requests.size(),2u);EXPECT_EQ(requests.front().request.phase,Phase::Begin);EXPECT_EQ(requests.back().request.phase,Phase::Cancel);
  EXPECT_TRUE(requests.back().request.patch.values.empty());
  EXPECT_EQ(std::count_if(requests.begin(),requests.end(),[](const auto& r){return r.request.phase==Phase::End || r.request.phase==Phase::Commit;}),0);
}

TEST_F(MaterialWidgets, EvaluatedRefreshKeepsActiveDraftAndUsesFreshRevisionGuard) {
  const auto position=locate(id("Metallic",GltfMaterialProperty::Metallic,true));startDrag(position);
  auto updated=snapshot();updated.target.authoredRevision=8;
  updated.numeric[size_t(GltfMaterialProperty::Metallic)].evaluated[0]=.95;
  updated.numeric[size_t(GltfMaterialProperty::Metallic)].animated=true;gui.setMaterialInspectorSnapshot(updated);frame();
  ImGui::GetIO().AddMousePosEvent(position.x+40,position.y);frame();release();
  ASSERT_FALSE(requests.empty());EXPECT_EQ(requests.back().request.phase,Phase::End);
  EXPECT_EQ(requests.back().target.authoredRevision,8u);
  const auto value=std::get<double>(requests.back().request.patch.values[0].value);EXPECT_GT(value,.4);EXPECT_NE(value,.95);
  EXPECT_EQ(requests.front().request.gestureId,requests.back().request.gestureId);
}

TEST_F(MaterialWidgets, ChangingBindingCancelsTheOldGestureAndNeverTargetsTheNewSelection) {
  const auto position=locate(id("Metallic",GltfMaterialProperty::Metallic,true));startDrag(position);
  auto changed=snapshot();changed.target.bindingGraphNode=17;changed.target.sourceMaterial=12;gui.setMaterialInspectorSnapshot(changed);frame();release();frame();
  ASSERT_FALSE(requests.empty());EXPECT_EQ(requests.back().request.phase,Phase::Cancel);
  for(const auto& request:requests) {EXPECT_EQ(request.target.bindingGraphNode,7u);EXPECT_EQ(request.request.sourceMaterial,2);}
}

TEST_F(MaterialWidgets, VectorDragPreservesTheOtherAuthoredComponents) {
  const auto position=locate(id("Base color",GltfMaterialProperty::BaseColor,true));startDrag(position);release();
  ASSERT_FALSE(requests.empty());const auto& value=std::get<std::array<double,4>>(requests.back().request.patch.values[0].value);
  EXPECT_GT(value[0],.2);EXPECT_EQ(value[1],.3);EXPECT_EQ(value[2],.4);EXPECT_EQ(value[3],1);
}

TEST_F(MaterialWidgets, TextEntryCanSetScalarToExactZeroAndEnterEndsOnlyOnce) {
  const auto position=locate(id("Metallic",GltfMaterialProperty::Metallic,true));
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);click(position);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);frame();
  replace("0");key(ImGuiKey_Enter);frame();
  ASSERT_FALSE(requests.empty());EXPECT_EQ(requests.back().request.phase,Phase::End);
  EXPECT_EQ(std::get<double>(requests.back().request.patch.values[0].value),0);
  EXPECT_EQ(std::count_if(requests.begin(),requests.end(),[](const auto& r){return r.request.phase==Phase::End;}),1);
}

TEST_F(MaterialWidgets, ResetButtonQueuesSchemaDefaultInsteadOfPromotingZero) {
  auto source=material();source.extensions["KHR_materials_clearcoat"]=Value(Value::Object{{"clearcoatFactor",Value(.8)}});
  gui.setMaterialInspectorSnapshot(BuildMaterialInspectorSnapshot(target(),source,source,2));frame();
  const auto reset=id("Reset",GltfMaterialProperty::Clearcoat);
  ImVec2 point{-100,-100};
  for(float x:{850.f,820.f,790.f,760.f}) {
    point=locate(reset,x,false);if(point.x>=0) break;
  }
  ASSERT_GE(point.x,0);
  click(point);ASSERT_EQ(requests.size(),1u);EXPECT_EQ(requests[0].request.phase,Phase::Commit);
  EXPECT_EQ(requests[0].request.patch.values[0].property,GltfMaterialProperty::Clearcoat);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(requests[0].request.patch.values[0].value));
  EXPECT_EQ(gui.materialInspectorSnapshot().numeric[size_t(GltfMaterialProperty::Clearcoat)].defaults[0],0);
}

TEST_F(MaterialWidgets, CheckboxFalseAndNameEnterProduceSeparateCommits) {
  click(locate(id("Double-sided")));ASSERT_EQ(requests.size(),1u);
  ASSERT_TRUE(requests[0].request.patch.doubleSided);EXPECT_FALSE(*requests[0].request.patch.doubleSided);
  click(locate(id("Name")));replace("Copper finish");key(ImGuiKey_Enter);frame();
  ASSERT_EQ(requests.size(),2u);ASSERT_TRUE(requests[1].request.patch.name);EXPECT_EQ(*requests[1].request.patch.name,"Copper finish");
  EXPECT_EQ(requests[0].request.phase,Phase::Commit);EXPECT_EQ(requests[1].request.phase,Phase::Commit);
}

TEST_F(MaterialWidgets, MakeUniqueButtonQueuesOneGuardedActionWithoutChangingSnapshot) {
  gui.setMaterialInspectorSnapshot(bindingSnapshot());frame();
  click(locate(id("Make unique"),110));frame();
  ASSERT_EQ(bindingRequests.size(),1u);const auto& request=bindingRequests[0];
  EXPECT_EQ(request.target,target());EXPECT_EQ(request.operation,MaterialBindingOperation::MakeUnique);
  EXPECT_EQ(request.sourceMaterial,2);EXPECT_EQ(request.runtimeMaterial,9u);
  EXPECT_TRUE(requests.empty());EXPECT_EQ(gui.materialInspectorSnapshot().sharedUsers,4u);
}

TEST_F(MaterialWidgets, AssignPickerIsOnlyAPreferenceUntilAssignButtonIsClicked) {
  gui.setMaterialInspectorSnapshot(bindingSnapshot());frame();chooseAssignment(1);
  EXPECT_TRUE(bindingRequests.empty());EXPECT_TRUE(requests.empty());
  click(locate(id("Assign")));frame();ASSERT_EQ(bindingRequests.size(),1u);
  EXPECT_EQ(bindingRequests[0].operation,MaterialBindingOperation::Assign);
  EXPECT_EQ(bindingRequests[0].sourceMaterial,3);EXPECT_EQ(bindingRequests[0].runtimeMaterial,17u);
  EXPECT_EQ(gui.materialInspectorSnapshot().target.runtimeMaterial,9u);EXPECT_TRUE(requests.empty());
}

TEST_F(MaterialWidgets, RuntimeSurfaceStillDisplaysWorkingAssignControlsBeforeFactorUnavailableReturn) {
  auto state=bindingSnapshot();state.available=false;state.makeUniqueAvailable=false;
  state.target.sourceMaterial=-1;state.target.runtimeMaterial=23;gui.setMaterialInspectorSnapshot(state);frame();
  chooseAssignment(1);click(locate(id("Assign")));frame();ASSERT_EQ(bindingRequests.size(),1u);
  EXPECT_EQ(bindingRequests[0].target,state.target);EXPECT_EQ(bindingRequests[0].runtimeMaterial,17u);
  EXPECT_TRUE(requests.empty());
}

TEST_F(MaterialWidgets, DisabledMakeUniqueNeverQueuesForUnprovedRuntimeProvenance) {
  auto state=bindingSnapshot();state.available=false;state.makeUniqueAvailable=false;
  state.target.sourceMaterial=-1;state.target.runtimeMaterial=23;gui.setMaterialInspectorSnapshot(state);frame();
  // Disabled items do not become HoveredId, so locate the enabled neighbor and
  // click the actual second button's measured row rather than a synthetic API.
  chooseAssignment(1);const auto assign=locate(id("Assign"));
  click({110,assign.y});frame();EXPECT_TRUE(bindingRequests.empty());EXPECT_TRUE(requests.empty());
}

TEST_F(MaterialWidgets, ProvedRuntimeMakeUniqueButtonWorksBeforeFactorUnavailableReturn) {
  auto state=bindingSnapshot();state.available=false;state.makeUniqueAvailable=true;
  state.target.sourceMaterial=-1;state.target.runtimeMaterial=23;gui.setMaterialInspectorSnapshot(state);frame();
  click(locate(id("Make unique"),110));frame();
  ASSERT_EQ(bindingRequests.size(),1u);const auto& request=bindingRequests[0];
  EXPECT_EQ(request.target,state.target);EXPECT_EQ(request.sourceMaterial,-1);EXPECT_EQ(request.runtimeMaterial,23u);
  EXPECT_EQ(request.operation,MaterialBindingOperation::MakeUnique);EXPECT_TRUE(requests.empty());
}

TEST_F(MaterialWidgets, TextureReplaceButtonSubmitsTheTypedPathWithoutMutatingItsSnapshot) {
  openTexture();click(locate(textureItem("Image path"),70));replace("replacement.png");
  click(locate(textureItem("Replace from file"),70));frame();ASSERT_EQ(textureRequests.size(),1u);
  const auto& request=textureRequests[0];EXPECT_EQ(request.target,target());
  EXPECT_EQ(request.request.operation,container::material::MaterialTextureEditOperation::ImportFile);
  EXPECT_EQ(request.request.path,"replacement.png");EXPECT_EQ(request.request.expectedResourceRevision,19u);
  EXPECT_EQ(gui.materialInspectorSnapshot().textureSlots[0].file,"retained.png");
  EXPECT_TRUE(requests.empty());EXPECT_TRUE(bindingRequests.empty());
}

TEST_F(MaterialWidgets, ExistingTexturePickerDoesNotCommitUntilUseSelectedIsClicked) {
  openTexture();chooseTexture(1);EXPECT_TRUE(textureRequests.empty());
  click(locate(textureItem("Use selected texture"),75));frame();ASSERT_EQ(textureRequests.size(),1u);
  EXPECT_EQ(textureRequests[0].request.operation,container::material::MaterialTextureEditOperation::BindExisting);
  EXPECT_EQ(textureRequests[0].request.sourceTexture,8u);EXPECT_FALSE(textureRequests[0].request.path);
}

TEST_F(MaterialWidgets, ClearTextureButtonQueuesOneDiscreteOperationWithNoOtherPayload) {
  openTexture();ImVec2 point{-100,-100};for(float x:{180.f,210.f,240.f,270.f}) {
    point=locate(textureItem("Clear slot"),x,false);if(point.x>=0) break;
  }
  ASSERT_GE(point.x,0);click(point);frame();ASSERT_EQ(textureRequests.size(),1u);
  const auto& request=textureRequests[0].request;
  EXPECT_EQ(request.operation,container::material::MaterialTextureEditOperation::Clear);
  EXPECT_FALSE(request.path);EXPECT_FALSE(request.sourceTexture);EXPECT_FALSE(request.sampler);
  EXPECT_TRUE(container::material::MaterialTextureMappingEmpty(request.mapping));
}

TEST_F(MaterialWidgets, MappingResetButtonQueuesOmissionWithoutEditingFixedSchemaChannels) {
  openTexture();click(locate(textureItem("UV mapping"),75));
  ImVec2 point{-100,-100};for(float x:{160.f,190.f,220.f,250.f}) {
    point=locate(textureItem("Reset mapping",GltfMaterialTextureSlot::BaseColor,"UV mapping"),x,false);if(point.x>=0) break;
  }
  ASSERT_GE(point.x,0);click(point);frame();ASSERT_EQ(textureRequests.size(),1u);
  const auto& request=textureRequests[0].request;
  EXPECT_EQ(request.operation,container::material::MaterialTextureEditOperation::EditMapping);
  ASSERT_TRUE(request.mapping.texCoord);ASSERT_TRUE(request.mapping.offset);ASSERT_TRUE(request.mapping.scale);ASSERT_TRUE(request.mapping.rotation);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(*request.mapping.texCoord));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(*request.mapping.offset));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(*request.mapping.scale));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(*request.mapping.rotation));EXPECT_FALSE(request.mapping.channel);
  EXPECT_FALSE(request.sampler);EXPECT_FALSE(request.path);EXPECT_FALSE(request.sourceTexture);
}
} // namespace
