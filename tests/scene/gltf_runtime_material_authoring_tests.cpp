#include <Container/utility/GltfRuntimeMaterialAuthoring.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace container::material;
using Value=tinygltf::Value;using Object=Value::Object;
constexpr size_t count=size_t(GltfMaterialTextureSlot::Count);
constexpr uint32_t absent=UINT32_MAX;
Value info(int source) {return Value(Object{{"index",Value(source)}});}
Value array(std::initializer_list<double> values) {Value::Array out;for(const auto value:values) out.emplace_back(value);return Value(std::move(out));}
tinygltf::Parameter texture(int source) {tinygltf::Parameter p;p.json_double_value["index"]=source;return p;}
struct Fixture {
  std::vector<std::string> declarations;
  std::vector<uint32_t> color,data;
  tinygltf::Material source;
  Fixture(size_t textures=23):color(textures),data(textures) {
    for(size_t i=0;i<textures;++i) {color[i]=uint32_t(100+i);data[i]=uint32_t(200+i);}
    source.name="Retained runtime origin";
    source.pbrMetallicRoughness.metallicFactor=.25;
    source.pbrMetallicRoughness.roughnessFactor=.75;
    source.extras=Value(Object{{"opaqueIndex",Value(16777217)},{"nested",Value(Object{{"textureLike",info(977)}})}});
    source.extensions["VENDOR_opaque"]=Value(Object{{"materialIndexLike",Value(373)}});
    source.extensions_json_string="opaque raw source cache";source.extras_json_string="opaque extras cache";
    declarations={"VENDOR_opaque"};
  }
  GltfMaterialConversionContext context() const {return {color.size(),declarations,color,data,1000};}
  void extension(const char* name,Object fields) {source.extensions[name]=Value(std::move(fields));declarations.emplace_back(name);}
  void allSlots() {
    source.alphaMode="MASK";source.pbrMetallicRoughness.baseColorTexture.index=0;source.normalTexture.index=1;
    source.occlusionTexture.index=2;source.emissiveTexture.index=3;source.pbrMetallicRoughness.metallicRoughnessTexture.index=4;
    source.values["roughnessTexture"]=texture(5);source.values["metalnessTexture"]=texture(6);source.values["heightTexture"]=texture(8);source.values["opacityTexture"]=texture(9);
    extension("KHR_materials_specular",{{"specularTexture",info(7)},{"specularColorTexture",info(11)}});
    extension("KHR_materials_transmission",{{"transmissionTexture",info(10)}});
    extension("KHR_materials_clearcoat",{{"clearcoatTexture",info(12)},{"clearcoatRoughnessTexture",info(13)},{"clearcoatNormalTexture",info(14)}});
    extension("KHR_materials_volume",{{"thicknessTexture",info(15)}});
    extension("KHR_materials_sheen",{{"sheenColorTexture",info(16)},{"sheenRoughnessTexture",info(17)}});
    extension("KHR_materials_iridescence",{{"iridescenceTexture",info(18)},{"iridescenceThicknessTexture",info(19)}});
    extension("KHR_materials_anisotropy",{{"anisotropyTexture",info(20)}});
    extension("KHR_materials_diffuse_transmission",{{"diffuseTransmissionTexture",info(21)},{"diffuseTransmissionColorTexture",info(22)}});
  }
};
RuntimeMaterialAuthoringResult materialize(const Material& value,const RuntimeMaterialSourceTextures& ids={},
    GltfMaterialConversionContext context={},const tinygltf::Material* retained=nullptr,
    const RuntimeMaterialAuthoringLimits& limits={}) {
  return MaterializeRuntimeMaterial(value,ids,ids,{context,retained},{context,retained},limits);
}
void expectRoundTrip(const RuntimeMaterialAuthoringResult& result,const Material& input,GltfMaterialConversionContext context) {
  ASSERT_TRUE(result);EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result.authored(),context).value,input));
  EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result.evaluated(),context).value,input));
}

TEST(GltfRuntimeMaterialAuthoring, DefaultsRetainRuntimeMetallicZeroInsteadOfGltfDefaultOne) {
  Material value;
  auto result=materialize(value);expectRoundTrip(result,value,{});
  EXPECT_EQ(result.authored().pbrMetallicRoughness.metallicFactor,0.);
  EXPECT_EQ(result.authored().pbrMetallicRoughness.roughnessFactor,1.);
  EXPECT_GT(result.ownedBytes(),sizeof(result));
  EXPECT_TRUE(std::ranges::find(result.extensionsToDeclare(),"KHR_materials_ior")!=result.extensionsToDeclare().end());
}

TEST(GltfRuntimeMaterialAuthoring, ActualSolidDiagnosticImplicitAndMaterialXValuesRoundTrip) {
  for(int kind=0;kind<4;++kind) {
    Material value;value.baseColor={.21f,.5f,.82f,.7f};
    value.roughnessFactor=kind==0?.82f:kind==1?.5f:1.f;
    value.alphaMode=kind==0?AlphaMode::Mask:kind==1?AlphaMode::Blend:AlphaMode::Opaque;
    value.doubleSided=kind%2;
    if(kind==3) {value.opacityFactor=.6f;value.alphaMode=AlphaMode::Blend;value.specularFactor=.25f;value.heightScale=-.02f;value.transmissionFactor=.4f;}
    auto result=materialize(value);expectRoundTrip(result,value,{});
    EXPECT_EQ(ConvertGltfMaterial(result.authored(),{}).value.opacityFactor,value.opacityFactor);
    EXPECT_EQ(result.authored().alphaCutoff,double(value.alphaCutoff));
  }
}

TEST(GltfRuntimeMaterialAuthoring, All23SourceSlotsRemainLiteralAndNeverReverseResourceAliases) {
  Fixture fixture;fixture.allSlots();
  fixture.data[5]=fixture.data[6]=77;fixture.color[0]=fixture.color[3]=55;
  const auto input=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,23);
  for(size_t i=0;i<count;++i) EXPECT_EQ(ids[i],int32_t(i));
  const auto result=materialize(input,ids,fixture.context());expectRoundTrip(result,input,fixture.context());
  EXPECT_EQ(ReadGltfMaterialSourceTextures(result.authored(),23),ids);
  EXPECT_EQ(result.authored().pbrMetallicRoughness.baseColorTexture.index,0);
  EXPECT_EQ(result.authored().emissiveTexture.index,3);
  EXPECT_EQ(result.authored().values.at("roughnessTexture").json_double_value.at("index"),5.);
  EXPECT_EQ(result.authored().values.at("metalnessTexture").json_double_value.at("index"),6.);
}

TEST(GltfRuntimeMaterialAuthoring, AuthoredAndEvaluatedSourceDomainsCanUseIndependentIdsAndCounts) {
  Fixture authored,evaluated(27);authored.allSlots();
  const auto value=ConvertGltfMaterial(authored.source,authored.context()).value;
  const auto authoredIds=ReadGltfMaterialSourceTextures(authored.source,23);RuntimeMaterialSourceTextures evaluatedIds;
  std::fill(evaluated.color.begin(),evaluated.color.end(),absent);std::fill(evaluated.data.begin(),evaluated.data.end(),absent);
  for(size_t i=0;i<count;++i) {const auto id=26-i;evaluatedIds[i]=int32_t(id);evaluated.color[id]=authored.color[i];evaluated.data[id]=authored.data[i];}
  auto result=MaterializeRuntimeMaterial(value,authoredIds,evaluatedIds,{authored.context()},{evaluated.context()});
  EXPECT_EQ(ReadGltfMaterialSourceTextures(result.authored(),23),authoredIds);
  EXPECT_EQ(ReadGltfMaterialSourceTextures(result.evaluated(),27),evaluatedIds);
  EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result.authored(),authored.context()).value,value));
  EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result.evaluated(),evaluated.context()).value,value));
}

TEST(GltfRuntimeMaterialAuthoring, OneSourceTextureSupportsIndependentColorAndDataRoles) {
  Fixture fixture(1);fixture.source.pbrMetallicRoughness.baseColorTexture.index=0;fixture.source.normalTexture.index=0;
  const auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  RuntimeMaterialSourceTextures ids;ids[0]=ids[1]=0;
  auto result=materialize(value,ids,fixture.context());expectRoundTrip(result,value,fixture.context());
  const auto converted=ConvertGltfMaterial(result.authored(),fixture.context());
  EXPECT_EQ(converted.value.baseColorTextureIndex,100u);EXPECT_EQ(converted.value.normalTextureIndex,200u);
  EXPECT_EQ(converted.sourceTextures[0],0);EXPECT_EQ(converted.sourceTextures[1],0);
}

TEST(GltfRuntimeMaterialAuthoring, ArchiveOnlyRetainedTemplatePreservesMetadataAcrossIndependentLiveIds) {
  Fixture authored(23),evaluated(1);authored.source.pbrMetallicRoughness.baseColorTexture.index=22;
  const auto value=ConvertGltfMaterial(authored.source,authored.context()).value;
  const auto authoredIds=ReadGltfMaterialSourceTextures(authored.source,23);
  RuntimeMaterialSourceTextures evaluatedIds;evaluatedIds[0]=0;evaluated.color[0]=authored.color[22];
  const auto original=authored.source;
  const auto result=MaterializeRuntimeMaterial(value,authoredIds,evaluatedIds,{authored.context(),&authored.source},{evaluated.context()});
  EXPECT_EQ(result.authored().pbrMetallicRoughness.baseColorTexture.index,22);
  EXPECT_EQ(result.evaluated().pbrMetallicRoughness.baseColorTexture.index,0);
  EXPECT_EQ(result.authored().extras,authored.source.extras);EXPECT_EQ(result.evaluated().extras,authored.source.extras);
  EXPECT_EQ(result.evaluated().extensions.at("VENDOR_opaque"),authored.source.extensions.at("VENDOR_opaque"));
  EXPECT_TRUE(RuntimeMaterialsEqual(ConvertGltfMaterial(result.evaluated(),evaluated.context()).value,value));
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(authored.source,original));
}

TEST(GltfRuntimeMaterialAuthoring, ExactRetainedRuntimeOriginKeepsOpaqueRecordsAndRawCaches) {
  Fixture fixture;fixture.allSlots();
  fixture.source.normalTexture.extras=Value(Object{{"sourceTextureIndexLike",Value(677)}});
  const auto input=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,23);
  const auto original=fixture.source;
  auto result=materialize(input,ids,fixture.context(),&fixture.source);
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(result.authored(),fixture.source));
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(result.evaluated(),fixture.source));
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(fixture.source,original));
}

TEST(GltfRuntimeMaterialAuthoring, CanonicalNumericUpdateRetainsUnknownAndNestedTextureMetadata) {
  Fixture fixture(1);fixture.source.normalTexture.index=0;
  fixture.source.normalTexture.extras=Value(Object{{"retained",Value("normal origin")}});
  fixture.source.normalTexture.extensions["KHR_texture_transform"]=Value(Object{{"offset",array({.2,.4})},{"vendorAngle",Value(37)}});
  fixture.declarations.emplace_back("KHR_texture_transform");
  auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;value.roughnessFactor=.5f;
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,1);
  auto result=materialize(value,ids,fixture.context(),&fixture.source);expectRoundTrip(result,value,fixture.context());
  EXPECT_EQ(result.authored().extras,fixture.source.extras);
  EXPECT_EQ(result.authored().extensions.at("VENDOR_opaque"),fixture.source.extensions.at("VENDOR_opaque"));
  EXPECT_EQ(result.authored().normalTexture.extras,fixture.source.normalTexture.extras);
  EXPECT_EQ(result.authored().normalTexture.extensions.at("KHR_texture_transform").Get("vendorAngle").Get<int>(),37);
  EXPECT_TRUE(result.authored().extensions_json_string.empty());
}

TEST(GltfRuntimeMaterialAuthoring, ZeroBlackNegativeZeroAndIndependentLegacyFactorsAreNotPromoted) {
  Material value;value.baseColor={0.f,-0.f,0.f,1.f};value.emissiveColor=glm::vec3(0);
  value.metallicFactor=-0.f;value.roughnessFactor=0.f;value.specularFactor=0.f;value.specularColorFactor=glm::vec3(0);
  value.transmissionFactor=-0.f;value.opacityFactor=.4f;value.alphaMode=AlphaMode::Blend;value.heightScale=-.2f;value.heightOffset=-0.f;
  value.clearcoatFactor=0.f;value.diffuseTransmissionFactor=0.f;value.diffuseTransmissionColorFactor=glm::vec3(0);
  auto result=materialize(value);expectRoundTrip(result,value,{});
  EXPECT_TRUE(std::signbit(result.authored().pbrMetallicRoughness.metallicFactor));
  EXPECT_TRUE(std::signbit(ConvertGltfMaterial(result.authored(),{}).value.baseColor.y));
  EXPECT_EQ(result.authored().pbrMetallicRoughness.baseColorFactor[3],1.);
  EXPECT_EQ(result.authored().values.at("opacity").number_value,double(.4f));
}

TEST(GltfRuntimeMaterialAuthoring, TypedLegacyTextureTransformsRetainEveryNumericComponentAndChannel) {
  Fixture fixture(1);fixture.source.alphaMode="MASK";
  for(const auto* name:{"roughnessTexture","metalnessTexture","heightTexture","opacityTexture"}) fixture.source.values[name]=texture(0);
  auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  for(auto* transform:{&value.roughnessTextureTransform,&value.metalnessTextureTransform,&value.heightTextureTransform,&value.opacityTextureTransform})
    *transform=TextureTransform{{-.25f,.75f},{-2.f,.5f},-.3f,1,2};
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,1);
  auto result=materialize(value,ids,fixture.context());expectRoundTrip(result,value,fixture.context());
  const auto& parameter=result.authored().values.at("heightTexture").json_double_value;
  EXPECT_EQ(parameter.at("offsetX"),double(-.25f));EXPECT_EQ(parameter.at("offsetY"),double(.75f));
  EXPECT_EQ(parameter.at("scaleX"),-2.);EXPECT_EQ(parameter.at("scaleY"),.5);EXPECT_EQ(parameter.at("rotation"),double(-.3f));
  EXPECT_EQ(parameter.at("texCoord"),1.);EXPECT_EQ(parameter.at("channel"),2.);
}

TEST(GltfRuntimeMaterialAuthoring, StandardTextureTransformsRetainSignsAndDoNotChangeSourceIdentity) {
  Fixture fixture(1);fixture.source.normalTexture.index=0;
  auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  value.normalTextureTransform={{-.0f,.75f},{-2.f,.5f},-.3f,1,4};value.normalTextureScale=-2.f;
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,1);auto result=materialize(value,ids,fixture.context());
  expectRoundTrip(result,value,fixture.context());EXPECT_EQ(result.authored().normalTexture.index,0);
  EXPECT_EQ(result.authored().normalTexture.texCoord,1);EXPECT_EQ(result.authored().normalTexture.scale,-2.);
  EXPECT_TRUE(std::signbit(ConvertGltfMaterial(result.authored(),fixture.context()).value.normalTextureTransform.offset.x));
}

TEST(GltfRuntimeMaterialAuthoring, EveryActiveSlotRetainsItsIndependentTransform) {
  Fixture fixture;fixture.allSlots();auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  const std::array<TextureTransform Material::*,count> slots{
    &Material::baseColorTextureTransform,&Material::normalTextureTransform,&Material::occlusionTextureTransform,&Material::emissiveTextureTransform,
    &Material::metallicRoughnessTextureTransform,&Material::roughnessTextureTransform,&Material::metalnessTextureTransform,&Material::specularTextureTransform,
    &Material::heightTextureTransform,&Material::opacityTextureTransform,&Material::transmissionTextureTransform,&Material::specularColorTextureTransform,
    &Material::clearcoatTextureTransform,&Material::clearcoatRoughnessTextureTransform,&Material::clearcoatNormalTextureTransform,&Material::thicknessTextureTransform,
    &Material::sheenColorTextureTransform,&Material::sheenRoughnessTextureTransform,&Material::iridescenceTextureTransform,&Material::iridescenceThicknessTextureTransform,
    &Material::anisotropyTextureTransform,&Material::diffuseTransmissionTextureTransform,&Material::diffuseTransmissionColorTextureTransform};
  for(size_t i=0;i<count;++i) value.*slots[i]=TextureTransform{{float(i)*.125f,-.25f},{-.5f,float(i+1)*.25f},float(i)*-.0625f,uint32_t(i%2),i==21?3u:4u};
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,23);const auto result=materialize(value,ids,fixture.context());
  const auto converted=ConvertGltfMaterial(result.authored(),fixture.context()).value;
  for(size_t i=0;i<count;++i) {
    SCOPED_TRACE(i);const auto& expected=value.*slots[i];const auto& actual=converted.*slots[i];
    EXPECT_EQ(actual.offset,expected.offset);EXPECT_EQ(actual.scale,expected.scale);EXPECT_EQ(actual.rotation,expected.rotation);
    EXPECT_EQ(actual.texCoord,expected.texCoord);EXPECT_EQ(actual.channel,expected.channel);
  }
}

TEST(GltfRuntimeMaterialAuthoring, RetainedSpecularGlossinessNeedsNoInverseApproximation) {
  Fixture fixture(2);fixture.extension("KHR_materials_pbrSpecularGlossiness",{{"diffuseFactor",array({.3,.4,.5,.6})},
      {"specularFactor",array({1,.766,.336})},{"glossinessFactor",Value(.9)},{"diffuseTexture",info(0)},{"specularGlossinessTexture",info(1)}});
  const auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,2);
  auto result=materialize(value,ids,fixture.context(),&fixture.source);expectRoundTrip(result,value,fixture.context());
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(result.authored(),fixture.source));
  EXPECT_EQ(ids[5],1);EXPECT_EQ(ids[11],1);
  EXPECT_THROW((void)materialize(value,ids,fixture.context()),std::invalid_argument);
  auto changed=value;changed.baseColor.x=std::nextafter(changed.baseColor.x,0.f);
  EXPECT_THROW((void)materialize(changed,ids,fixture.context(),&fixture.source),std::invalid_argument);
  changed=value;changed.specularGlossinessDiffuse.x=std::nextafter(changed.specularGlossinessDiffuse.x,0.f);
  EXPECT_THROW((void)materialize(changed,ids,fixture.context(),&fixture.source),std::invalid_argument);
  changed=value;changed.specularGlossinessSpecular.y=std::nextafter(changed.specularGlossinessSpecular.y,0.f);
  EXPECT_THROW((void)materialize(changed,ids,fixture.context(),&fixture.source),std::invalid_argument);
  changed=value;changed.roughnessFactor=.45f;
  auto glossiness=materialize(changed,ids,fixture.context(),&fixture.source);
  expectRoundTrip(glossiness,changed,fixture.context());
}

TEST(GltfRuntimeMaterialAuthoring, SpecularGlossinessPackedColorAliasesValidateWithOrWithoutIndependentDataRole) {
  for(const bool dataPresent:{false,true}) {
    SCOPED_TRACE(dataPresent);Fixture fixture(2);
    if(!dataPresent) fixture.data.assign(2,absent);
    fixture.extension("KHR_materials_pbrSpecularGlossiness",{{"diffuseFactor",array({.3,.4,.5,.6})},
        {"specularFactor",array({1,.766,.336})},{"glossinessFactor",Value(.9)},
        {"diffuseTexture",info(0)},{"specularGlossinessTexture",info(1)}});
    const auto converted=ConvertGltfMaterial(fixture.source,fixture.context());
    const auto ids=ReadGltfMaterialSourceTextures(fixture.source,2);
    EXPECT_EQ(ids[5],1);EXPECT_EQ(ids[11],1);EXPECT_EQ(converted.sourceTextures[5],1);EXPECT_EQ(converted.sourceTextures[11],1);
    EXPECT_EQ(converted.value.roughnessTextureIndex,101u);EXPECT_EQ(converted.value.specularColorTextureIndex,101u);
    const auto result=materialize(converted.value,ids,fixture.context(),&fixture.source);
    expectRoundTrip(result,converted.value,fixture.context());
    EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(result.authored(),fixture.source));
    auto wrongRole=converted.value;wrongRole.roughnessTextureIndex=201;
    EXPECT_THROW((void)materialize(wrongRole,ids,fixture.context(),&fixture.source),std::invalid_argument);
  }
}

TEST(GltfRuntimeMaterialAuthoring, SpecularGlossinessWithoutPackedTextureRetainsIndependentLegacyDataOrigin) {
  Fixture fixture(1);fixture.extension("KHR_materials_pbrSpecularGlossiness",{});
  fixture.source.values["roughnessTexture"]=texture(0);fixture.source.values["roughnessTexture"].json_double_value["channel"]=2;
  fixture.color[0]=absent;
  const auto converted=ConvertGltfMaterial(fixture.source,fixture.context());
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,1);
  EXPECT_EQ(ids[5],0);EXPECT_EQ(ids[11],-1);EXPECT_EQ(converted.value.roughnessTextureIndex,200u);
  EXPECT_EQ(converted.value.specularColorTextureIndex,absent);
  EXPECT_FALSE(GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot::Roughness,converted.value));
  const auto result=materialize(converted.value,ids,fixture.context(),&fixture.source);
  expectRoundTrip(result,converted.value,fixture.context());
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(result.authored(),fixture.source));
}

TEST(GltfRuntimeMaterialAuthoring, UnlitDefaultsRemainUnlitAndCannotGainExcludedLightingExtensions) {
  Material value;value.unlit=true;value.baseColor={.3f,.4f,.5f,1.f};
  auto result=materialize(value);expectRoundTrip(result,value,{});
  EXPECT_TRUE(result.authored().extensions.contains("KHR_materials_unlit"));
  EXPECT_FALSE(result.authored().extensions.contains("KHR_materials_ior"));
  EXPECT_FALSE(result.authored().extensions.contains("KHR_materials_clearcoat"));
  value.clearcoatFactor=.5f;EXPECT_THROW((void)materialize(value),std::invalid_argument);
}

TEST(GltfRuntimeMaterialAuthoring, UnsupportedExactFieldsRejectWithNamedDiagnosticsAndNoMutation) {
  const auto named=[](const std::function<void()>& action,const char* field) {
    try {action();FAIL()<<"Expected exact representation rejection";}
    catch(const std::invalid_argument& error) {EXPECT_NE(std::string(error.what()).find(field),std::string::npos);}
  };
  Material dormant;dormant.baseColorTextureTransform.offset.x=.25f;
  named([&]{(void)materialize(dormant);},"baseColorTexture");
  Fixture fixture(1);fixture.source.pbrMetallicRoughness.baseColorTexture.index=0;
  auto channel=ConvertGltfMaterial(fixture.source,fixture.context()).value;channel.baseColorTextureTransform.channel=2;
  const auto ids=ReadGltfMaterialSourceTextures(fixture.source,1);
  named([&]{(void)materialize(channel,ids,fixture.context());},"baseColorTexture");
  Material coverage;coverage.opacityFactor=.5f;coverage.alphaMode=AlphaMode::Opaque;
  named([&]{(void)materialize(coverage);},"alphaMode");
  EXPECT_EQ(channel.baseColorTextureTransform.channel,2u);EXPECT_EQ(dormant.baseColorTextureTransform.offset.x,.25f);
}

TEST(GltfRuntimeMaterialAuthoring, BadSourceIdsRolesAndResidentWitnessesRejectWithoutGuessing) {
  Fixture fixture(1);fixture.source.pbrMetallicRoughness.baseColorTexture.index=0;
  const auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;
  auto ids=ReadGltfMaterialSourceTextures(fixture.source,1);
  auto bad=ids;bad[0]=-1;EXPECT_THROW((void)materialize(value,bad,fixture.context()),std::invalid_argument);
  bad=ids;bad[0]=1;EXPECT_THROW((void)materialize(value,bad,fixture.context()),std::invalid_argument);
  bad=ids;bad[0]=-2;EXPECT_THROW((void)materialize(value,bad,fixture.context()),std::invalid_argument);
  fixture.color[0]=200;EXPECT_THROW((void)materialize(value,ids,fixture.context()),std::invalid_argument);
  fixture.color[0]=absent;EXPECT_THROW((void)materialize(value,ids,fixture.context()),std::invalid_argument);
  fixture.color[0]=100;auto context=fixture.context();context.dataResources={};
  EXPECT_THROW((void)materialize(value,ids,context),std::invalid_argument);
  context=fixture.context();context.runtimeResourceCount=100;EXPECT_THROW((void)materialize(value,ids,context),std::invalid_argument);
}

TEST(GltfRuntimeMaterialAuthoring, SourceReaderValidatesFallbacksAndPreservesPhysicalPrecedence) {
  Fixture fixture(2);fixture.source.values["specularTexture"]=texture(1);
  fixture.extension("KHR_materials_specular",{{"specularTexture",info(0)}});
  EXPECT_EQ(ReadGltfMaterialSourceTextures(fixture.source,2)[7],0);
  fixture.source.values["specularTexture"].json_double_value["index"]=2;
  EXPECT_THROW((void)ReadGltfMaterialSourceTextures(fixture.source,2),std::invalid_argument);
  fixture.source.values["specularTexture"].json_double_value["index"]=.5;
  EXPECT_THROW((void)ReadGltfMaterialSourceTextures(fixture.source,2),std::invalid_argument);
  fixture.source.values.erase("specularTexture");fixture.source.normalTexture.index=-2;
  EXPECT_THROW((void)ReadGltfMaterialSourceTextures(fixture.source,2),std::invalid_argument);
}

TEST(GltfRuntimeMaterialAuthoring, LimitsRejectBeforeSelectedPayloadCopiesAndInputsStayExact) {
  Fixture fixture(0);const auto value=ConvertGltfMaterial(fixture.source,fixture.context()).value;const auto original=fixture.source;
  for(const uint64_t limit:std::array<uint64_t,3>{0,1,UINT64_MAX}) {
    RuntimeMaterialAuthoringLimits limits;limits.maxOutputBytes=limit;
    EXPECT_THROW((void)materialize(value,{},fixture.context(),&fixture.source,limits),std::invalid_argument);
  }
  RuntimeMaterialAuthoringLimits limits;limits.conversion.validation.maxMetadataValues=1;
  EXPECT_THROW((void)materialize(value,{},fixture.context(),&fixture.source,limits),std::invalid_argument);
  limits={};limits.conversion.validation.maxMetadataDepth=1;
  EXPECT_THROW((void)materialize(value,{},fixture.context(),&fixture.source,limits),std::invalid_argument);
  limits={};limits.conversion.validation.maxOutputBytes=1;
  EXPECT_THROW((void)materialize(value,{},fixture.context(),&fixture.source,limits),std::invalid_argument);
  EXPECT_TRUE(container::geometry::gltf::GltfMaterialRecordsEqual(fixture.source,original));
}

TEST(GltfRuntimeMaterialAuthoring, NonfiniteAndOutOfRangeRuntimeFactorsRejectRatherThanClamp) {
  const std::vector<std::function<void(Material&)>> changes{
    [](auto& m){m.baseColor.x=1.01f;},[](auto& m){m.roughnessFactor=-.1f;},[](auto& m){m.emissiveColor.x=2.f;},
    [](auto& m){m.ior=.5f;},[](auto& m){m.attenuationDistance=0;},[](auto& m){m.attenuationDistance=-std::numeric_limits<float>::infinity();},
    [](auto& m){m.opacityFactor=std::numeric_limits<float>::quiet_NaN();},[](auto& m){m.normalTextureTransform.offset.x=std::numeric_limits<float>::infinity();},
    [](auto& m){m.diffuseTransmissionTextureTransform.texCoord=2;},[](auto& m){m.heightTextureTransform.channel=5;},
    [](auto& m){m.alphaMode=AlphaMode(17);}};
  for(size_t i=0;i<changes.size();++i) {SCOPED_TRACE(i);Material value;changes[i](value);EXPECT_THROW((void)materialize(value),std::invalid_argument);}
  Material valid;valid.attenuationDistance=std::numeric_limits<float>::infinity();EXPECT_NO_THROW((void)materialize(valid));
}

TEST(GltfRuntimeMaterialAuthoring, InvalidConversionCapsRejectWithoutRetainedRecordsOrCanonicalOutput) {
  const std::vector<std::function<void(RuntimeMaterialAuthoringLimits&)>> invalid{
    [](auto& l){l.conversion.maxSourceTextures=0;},[](auto& l){l.conversion.maxSourceTextures=UINT64_MAX;},
    [](auto& l){l.conversion.maxRuntimeResources=0;},[](auto& l){l.conversion.maxRuntimeResources=UINT64_MAX;},
    [](auto& l){l.conversion.validation.maxOutputBytes=0;},[](auto& l){l.conversion.validation.maxOutputBytes=1;},
    [](auto& l){l.conversion.validation.maxOutputBytes=UINT64_MAX;},
    [](auto& l){l.conversion.validation.maxMetadataValues=0;},[](auto& l){l.conversion.validation.maxMetadataValues=UINT64_MAX;},
    [](auto& l){l.conversion.validation.maxMetadataDepth=0;},[](auto& l){l.conversion.validation.maxMetadataDepth=257;},
    [](auto& l){l.conversion.validation.maxAnimationChannels=0;},[](auto& l){l.conversion.validation.maxAnimationChannels=UINT64_MAX;},
    [](auto& l){l.conversion.validation.maxPatchOperations=0;},[](auto& l){l.conversion.validation.maxPatchOperations=UINT64_MAX;}};
  const Material original;
  for(size_t i=0;i<invalid.size();++i) {
    SCOPED_TRACE(i);RuntimeMaterialAuthoringLimits limits;invalid[i](limits);
    EXPECT_THROW((void)materialize(original,{}, {},nullptr,limits),std::invalid_argument);
    const tinygltf::Material source;
    EXPECT_THROW((void)ReadGltfMaterialSourceTextures(source,0,limits.conversion),std::invalid_argument);
    EXPECT_TRUE(RuntimeMaterialsEqual(original,Material{}));
  }
}

TEST(GltfRuntimeMaterialAuthoring, EqualityIsBitExactForEveryRuntimeFieldAndIgnoresNoBinding) {
  const Material before;
  std::vector<std::function<void(Material&)>> changes{
    [](auto& m){m.baseColor.x=-0.f;},[](auto& m){m.emissiveColor.y=-0.f;},[](auto& m){m.metallicFactor=-0.f;},
    [](auto& m){m.roughnessFactor=.5f;},[](auto& m){m.alphaCutoff=.3f;},[](auto& m){m.normalTextureScale=-1.f;},
    [](auto& m){m.occlusionStrength=0.f;},[](auto& m){m.opacityFactor=0.f;},[](auto& m){m.specularFactor=0.f;},
    [](auto& m){m.heightScale=-0.f;},[](auto& m){m.heightOffset=0.f;},[](auto& m){m.transmissionFactor=-0.f;},
    [](auto& m){m.emissiveStrength=0.f;},[](auto& m){m.specularColorFactor.z=.5f;},[](auto& m){m.ior=2.f;},
    [](auto& m){m.dispersion=-0.f;},[](auto& m){m.clearcoatFactor=-0.f;},[](auto& m){m.clearcoatRoughnessFactor=-0.f;},
    [](auto& m){m.clearcoatNormalTextureScale=0.f;},[](auto& m){m.thicknessFactor=-0.f;},[](auto& m){m.attenuationColor.x=.5f;},
    [](auto& m){m.attenuationDistance=2.f;},[](auto& m){m.sheenColorFactor.x=-0.f;},[](auto& m){m.sheenRoughnessFactor=-0.f;},
    [](auto& m){m.iridescenceFactor=-0.f;},[](auto& m){m.iridescenceIor=1.5f;},[](auto& m){m.iridescenceThicknessMinimum=1.f;},
    [](auto& m){m.iridescenceThicknessMaximum=2.f;},[](auto& m){m.anisotropyStrength=-0.f;},[](auto& m){m.anisotropyRotation=-0.f;},
    [](auto& m){m.diffuseTransmissionFactor=-0.f;},[](auto& m){m.diffuseTransmissionColorFactor.z=0.f;},
    [](auto& m){m.alphaMode=AlphaMode::Blend;},[](auto& m){m.doubleSided=true;},[](auto& m){m.specularGlossinessWorkflow=true;},[](auto& m){m.unlit=true;}};
  for(const auto& change:changes) {auto after=before;change(after);EXPECT_FALSE(RuntimeMaterialsEqual(before,after));}
  for (int component = 0; component < 3; ++component) {
    auto diffuse = before, specular = before;
    diffuse.specularGlossinessDiffuse[component] = .5f;
    specular.specularGlossinessSpecular[component] = .5f;
    EXPECT_FALSE(RuntimeMaterialsEqual(before, diffuse));
    EXPECT_FALSE(RuntimeMaterialsEqual(before, specular));
  }
  auto a=before,b=before;a.baseColor.x=b.baseColor.x=std::numeric_limits<float>::quiet_NaN();EXPECT_TRUE(RuntimeMaterialsEqual(a,b));
  for(auto* value:{&a,&b}) value->baseColor.x=1.f;
  a.baseColorTextureIndex=0;EXPECT_FALSE(RuntimeMaterialsEqual(a,b));b.baseColorTextureIndex=0;EXPECT_TRUE(RuntimeMaterialsEqual(a,b));
  a.diffuseTransmissionColorTextureTransform.offset.y=-0.f;EXPECT_FALSE(RuntimeMaterialsEqual(a,b));
}

TEST(GltfRuntimeMaterialAuthoring, EqualityChecksAll23ResourceMembersIndependently) {
  const std::array<uint32_t Material::*,count> slots{
    &Material::baseColorTextureIndex,&Material::normalTextureIndex,&Material::occlusionTextureIndex,&Material::emissiveTextureIndex,
    &Material::metallicRoughnessTextureIndex,&Material::roughnessTextureIndex,&Material::metalnessTextureIndex,&Material::specularTextureIndex,
    &Material::heightTextureIndex,&Material::opacityTextureIndex,&Material::transmissionTextureIndex,&Material::specularColorTextureIndex,
    &Material::clearcoatTextureIndex,&Material::clearcoatRoughnessTextureIndex,&Material::clearcoatNormalTextureIndex,&Material::thicknessTextureIndex,
    &Material::sheenColorTextureIndex,&Material::sheenRoughnessTextureIndex,&Material::iridescenceTextureIndex,&Material::iridescenceThicknessTextureIndex,
    &Material::anisotropyTextureIndex,&Material::diffuseTransmissionTextureIndex,&Material::diffuseTransmissionColorTextureIndex};
  const Material original;
  for(size_t i=0;i<count;++i) {SCOPED_TRACE(i);auto changed=original;changed.*slots[i]=uint32_t(i);EXPECT_FALSE(RuntimeMaterialsEqual(original,changed));}
}
} // namespace
