#include <Container/utility/GltfMaterialConversion.h>
#include <Container/utility/MaterialXIntegration.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace container::material;
using Value = tinygltf::Value;
using Object = Value::Object;
constexpr auto invalid = std::numeric_limits<uint32_t>::max();
constexpr auto count = size_t(GltfMaterialTextureSlot::Count);
Value array(std::initializer_list<double> numbers) {
  Value::Array result;
  for (const auto value : numbers) result.emplace_back(value);
  return Value(std::move(result));
}
Value info(int index) { return Value(Object{{"index", Value(index)}}); }
tinygltf::Parameter texture(int index) {
  tinygltf::Parameter result;
  result.json_double_value["index"] = index;
  return result;
}
tinygltf::Parameter number(double value) {
  tinygltf::Parameter result;
  result.has_number_value = true;
  result.number_value = value;
  return result;
}
struct Fixture {
  tinygltf::Material material;
  std::vector<std::string> declarations;
  std::vector<uint32_t> color, data;
  Fixture(size_t textures = 23) : color(textures), data(textures) {
    for (size_t i = 0; i < textures; ++i) { color[i] = uint32_t(1000 + i); data[i] = uint32_t(2000 + i); }
    material.name = "One detached material";
    material.extras = Value(Object{{"identity", Value(73)}});
    material.extensions["VENDOR_opaque"] = Value(Object{{"textureIndexIsOpaque", Value(901)}});
    material.extensions_json_string = "source extension cache";
    material.extras_json_string = "source extras cache";
    material.pbrMetallicRoughness.metallicFactor = .4;
    material.pbrMetallicRoughness.roughnessFactor = .7;
    declarations = {"VENDOR_opaque"};
  }
  GltfMaterialConversionContext context() const { return {color.size(), declarations, color, data, 3000}; }
  void extension(const std::string& name, Object object) {
    material.extensions[name] = Value(std::move(object));
    declarations.push_back(name);
  }
  void allSlots() {
    material.alphaMode = "MASK";
    material.pbrMetallicRoughness.baseColorTexture.index = 0;
    material.normalTexture.index = 1;
    material.occlusionTexture.index = 2;
    material.emissiveTexture.index = 3;
    material.pbrMetallicRoughness.metallicRoughnessTexture.index = 4;
    material.values["roughnessTexture"] = texture(5);
    material.values["metalnessTexture"] = texture(6);
    material.values["heightTexture"] = texture(8);
    material.values["opacityTexture"] = texture(9);
    extension("KHR_materials_specular", {{"specularTexture", info(7)}, {"specularColorTexture", info(11)}});
    extension("KHR_materials_transmission", {{"transmissionTexture", info(10)}});
    extension("KHR_materials_clearcoat", {{"clearcoatTexture", info(12)}, {"clearcoatRoughnessTexture", info(13)}, {"clearcoatNormalTexture", info(14)}});
    extension("KHR_materials_volume", {{"thicknessTexture", info(15)}});
    extension("KHR_materials_sheen", {{"sheenColorTexture", info(16)}, {"sheenRoughnessTexture", info(17)}});
    extension("KHR_materials_iridescence", {{"iridescenceTexture", info(18)}, {"iridescenceThicknessTexture", info(19)}});
    extension("KHR_materials_anisotropy", {{"anisotropyTexture", info(20)}});
    extension("KHR_materials_diffuse_transmission", {{"diffuseTransmissionTexture", info(21)}, {"diffuseTransmissionColorTexture", info(22)}});
  }
};

TEST(GltfMaterialConversion, StrictDefaultsAndExplicitLegacyPolicyAreIndependent) {
  tinygltf::Material source;
  const auto strict = ConvertGltfMaterial(source, {});
  EXPECT_EQ(strict.value.metallicFactor, 1.f);
  EXPECT_EQ(strict.value.roughnessFactor, 1.f);
  EXPECT_EQ(strict.value.baseColor, glm::vec4(1));
  EXPECT_EQ(strict.value.emissiveColor, glm::vec3(0));
  EXPECT_EQ(strict.value.specularFactor, 1.f);
  EXPECT_EQ(strict.value.ior, 1.5f);
  EXPECT_EQ(strict.value.diffuseTransmissionColorFactor, glm::vec3(1));
  EXPECT_TRUE(std::isinf(strict.value.attenuationDistance));
  for (const auto index : strict.sourceTextures) EXPECT_EQ(index, -1);
  for (const auto resource : strict.runtimeBindings.runtimeIndices) EXPECT_EQ(resource, invalid);
  EXPECT_EQ(ConvertGltfMaterial(source, {}, GltfMaterialConversionPolicy::LegacyImportCompatibility).value.metallicFactor, 0.f);
  source.values["metallicFactor"] = number(1.);
  EXPECT_EQ(ConvertGltfMaterial(source, {}, GltfMaterialConversionPolicy::LegacyImportCompatibility).value.metallicFactor, 1.f);
  source.pbrMetallicRoughness.metallicFactor = 0.;
  EXPECT_EQ(ConvertGltfMaterial(source, {}).value.metallicFactor, 0.f);
}

TEST(GltfMaterialConversion, ExperimentalSourceLayerSurvivesAnOrdinaryMaterialEdit) {
  Fixture fixture(3);
  fixture.extension("KHR_materials_retroreflection", {
      {"retroreflectionFactor",Value(.65)}, {"retroreflectionTexture",info(0)}});
  fixture.extension("KHR_materials_scatter", {
      {"scatterStrengthFactor",Value(.5)}, {"scatterStrengthTexture",info(1)},
      {"multiscatterColorTexture",info(2)}, {"multiscatterColorFactor",array({.2,.5,.75})}});
  auto enabled=fixture.context();
  enabled.enableExperimentalMaterials=true;
  const auto original=ConvertGltfMaterial(fixture.material,enabled).value;
  EXPECT_EQ(original.experimental.textureResources(),(std::array<uint32_t,3>{2000,2001,1002}));
  EXPECT_FLOAT_EQ(original.experimental.retroreflection,.65f);
  EXPECT_EQ(original.experimental.scatterColor,glm::vec3(.2f,.5f,.75f));
  fixture.material.pbrMetallicRoughness.roughnessFactor=.25;
  const auto updated=ConvertGltfMaterial(fixture.material,enabled).value;
  EXPECT_FLOAT_EQ(updated.roughnessFactor,.25f);
  EXPECT_FLOAT_EQ(updated.experimental.scatterStrength,original.experimental.scatterStrength);
  EXPECT_EQ(updated.experimental.textureResources(),original.experimental.textureResources());
  const auto disabled=ConvertGltfMaterial(fixture.material,fixture.context()).value;
  EXPECT_FLOAT_EQ(disabled.experimental.retroreflection,0.0f);
  EXPECT_EQ(disabled.experimental.textureResources(),(std::array<uint32_t,3>{invalid,invalid,invalid}));
}

TEST(GltfMaterialConversion, All23BindingsRetainLiteralSourceIdsAndCorrectColorDataRoles) {
  Fixture fixture;
  fixture.allSlots();
  const auto before = fixture.material;
  const auto converted = ConvertGltfMaterial(fixture.material, fixture.context());
  const auto& value = converted.value;
  std::array<int32_t, count> expectedSources;
  std::iota(expectedSources.begin(), expectedSources.end(), 0);
  EXPECT_EQ(converted.sourceTextures, expectedSources);
  for (size_t i = 0; i < count; ++i) {
    const bool color = i == 0 || i == 3 || i == 11 || i == 16 || i == 22;
    EXPECT_EQ(converted.runtimeBindings.runtimeIndices[i], uint32_t((color ? 1000 : 2000) + i));
  }
  EXPECT_EQ(value.baseColorTextureIndex, 1000u);
  EXPECT_EQ(value.normalTextureIndex, 2001u);
  EXPECT_EQ(value.occlusionTextureIndex, 2002u);
  EXPECT_EQ(value.emissiveTextureIndex, 1003u);
  EXPECT_EQ(value.metallicRoughnessTextureIndex, 2004u);
  EXPECT_EQ(value.roughnessTextureIndex, 2005u);
  EXPECT_EQ(value.metalnessTextureIndex, 2006u);
  EXPECT_EQ(value.specularTextureIndex, 2007u);
  EXPECT_EQ(value.heightTextureIndex, 2008u);
  EXPECT_EQ(value.opacityTextureIndex, 2009u);
  EXPECT_EQ(value.transmissionTextureIndex, 2010u);
  EXPECT_EQ(value.specularColorTextureIndex, 1011u);
  EXPECT_EQ(value.clearcoatTextureIndex, 2012u);
  EXPECT_EQ(value.clearcoatRoughnessTextureIndex, 2013u);
  EXPECT_EQ(value.clearcoatNormalTextureIndex, 2014u);
  EXPECT_EQ(value.thicknessTextureIndex, 2015u);
  EXPECT_EQ(value.sheenColorTextureIndex, 1016u);
  EXPECT_EQ(value.sheenRoughnessTextureIndex, 2017u);
  EXPECT_EQ(value.iridescenceTextureIndex, 2018u);
  EXPECT_EQ(value.iridescenceThicknessTextureIndex, 2019u);
  EXPECT_EQ(value.anisotropyTextureIndex, 2020u);
  EXPECT_EQ(value.diffuseTransmissionTextureIndex, 2021u);
  EXPECT_EQ(value.diffuseTransmissionColorTextureIndex, 1022u);
  EXPECT_EQ(value.diffuseTransmissionTextureTransform.channel, 3u);
  EXPECT_EQ(fixture.material, before);
  EXPECT_EQ(fixture.material.extensions_json_string, "source extension cache");
}

TEST(GltfMaterialConversion, SharedGpuDataHandlesNeverAliasDistinctSourceColorViewsOrProvenance) {
  Fixture fixture(2);
  fixture.data = {77, 77};
  fixture.color = {21, 39};
  fixture.material.pbrMetallicRoughness.baseColorTexture.index = 1;
  fixture.material.emissiveTexture.index = 0;
  fixture.extension("KHR_materials_specular", {{"specularColorTexture", info(1)}});
  fixture.extension("KHR_materials_diffuse_transmission", {{"diffuseTransmissionTexture", info(0)}, {"diffuseTransmissionColorTexture", info(1)}});
  const auto converted = ConvertGltfMaterial(fixture.material, fixture.context());
  EXPECT_EQ(converted.value.baseColorTextureIndex, 39u);
  EXPECT_EQ(converted.value.emissiveTextureIndex, 21u);
  EXPECT_EQ(converted.value.specularColorTextureIndex, 39u);
  EXPECT_EQ(converted.value.diffuseTransmissionTextureIndex, 77u);
  EXPECT_EQ(converted.value.diffuseTransmissionColorTextureIndex, 39u);
  EXPECT_EQ(converted.sourceTextures[0], 1);
  EXPECT_EQ(converted.sourceTextures[3], 0);
  EXPECT_EQ(converted.sourceTextures[21], 0);
  EXPECT_EQ(converted.sourceTextures[22], 1);
}

TEST(GltfMaterialConversion, TexturedZeroAndBlackRemainZeroIncludingExtensionDefaults) {
  Fixture fixture;
  fixture.allSlots();
  fixture.material.pbrMetallicRoughness.metallicFactor = 0.;
  fixture.material.pbrMetallicRoughness.roughnessFactor = -0.;
  fixture.material.extensions["KHR_materials_specular"].Get<Object>()["specularFactor"] = Value(0.);
  fixture.material.extensions["KHR_materials_specular"].Get<Object>()["specularColorFactor"] = array({0, 0, 0});
  const auto value = ConvertGltfMaterial(fixture.material, fixture.context()).value;
  EXPECT_EQ(value.metallicFactor, 0.f);
  EXPECT_EQ(value.roughnessFactor, 0.f);
  EXPECT_TRUE(std::signbit(value.roughnessFactor));
  EXPECT_EQ(value.specularFactor, 0.f);
  EXPECT_EQ(value.specularColorFactor, glm::vec3(0));
  EXPECT_EQ(value.transmissionFactor, 0.f);
  EXPECT_EQ(value.clearcoatFactor, 0.f);
  EXPECT_EQ(value.clearcoatRoughnessFactor, 0.f);
  EXPECT_EQ(value.sheenColorFactor, glm::vec3(0));
  EXPECT_EQ(value.sheenRoughnessFactor, 0.f);
  EXPECT_EQ(value.iridescenceFactor, 0.f);
  EXPECT_EQ(value.anisotropyStrength, 0.f);
  EXPECT_EQ(value.diffuseTransmissionFactor, 0.f);
}

TEST(GltfMaterialConversion, RemovedExtensionsResetFactorsFlagsBindingsAndTransformDefaults) {
  Fixture fixture(1);
  fixture.material.alphaMode = "BLEND";
  fixture.material.doubleSided = true;
  fixture.extension("KHR_materials_anisotropy", {{"anisotropyStrength", Value(.8)}, {"anisotropyTexture", info(0)}});
  fixture.extension("KHR_materials_clearcoat", {{"clearcoatFactor", Value(.7)}, {"clearcoatTexture", info(0)}});
  const auto before = ConvertGltfMaterial(fixture.material, fixture.context());
  EXPECT_EQ(before.value.anisotropyStrength, .8f);
  fixture.material.extensions.erase("KHR_materials_anisotropy");
  fixture.material.extensions.erase("KHR_materials_clearcoat");
  fixture.material.alphaMode = "OPAQUE";
  fixture.material.doubleSided = false;
  fixture.extension("KHR_materials_unlit", {});
  const auto after = ConvertGltfMaterial(fixture.material, fixture.context());
  EXPECT_EQ(after.value.anisotropyStrength, 0.f);
  EXPECT_EQ(after.value.clearcoatFactor, 0.f);
  EXPECT_EQ(after.value.anisotropyTextureIndex, invalid);
  EXPECT_EQ(after.value.clearcoatTextureIndex, invalid);
  EXPECT_EQ(after.value.anisotropyTextureTransform.offset, glm::vec2(0));
  EXPECT_EQ(after.value.anisotropyTextureTransform.scale, glm::vec2(1));
  EXPECT_EQ(after.value.alphaMode, AlphaMode::Opaque);
  EXPECT_FALSE(after.value.doubleSided);
  EXPECT_TRUE(after.value.unlit);
  EXPECT_FALSE(after.value.specularGlossinessWorkflow);
}

TEST(GltfMaterialConversion, CoreAndExtensionTransformsUseEffectiveUvOverrideAndIndependentChannels) {
  Fixture fixture(2);
  fixture.declarations.push_back("KHR_texture_transform");
  fixture.material.normalTexture.index = 0;
  fixture.material.normalTexture.texCoord = 0;
  fixture.material.normalTexture.scale = -2.;
  const auto transform = Value(Object{{"offset", array({.2, -.4})}, {"scale", array({-2, .5})}, {"rotation", Value(.25)}, {"texCoord", Value(1)}});
  fixture.material.normalTexture.extensions["KHR_texture_transform"] = transform;
  auto textureInfo = info(1);
  textureInfo.Get<Object>()["texCoord"] = Value(1);
  textureInfo.Get<Object>()["extensions"] = Value(Object{{"KHR_texture_transform", Value(Object{{"texCoord", Value(0)}, {"rotation", Value(-.7)}})}});
  fixture.extension("KHR_materials_diffuse_transmission", {{"diffuseTransmissionTexture", textureInfo}, {"diffuseTransmissionColorTexture", info(0)}});
  const auto converted = ConvertGltfMaterial(fixture.material, fixture.context()).value;
  EXPECT_EQ(converted.normalTextureTransform.texCoord, 1u);
  EXPECT_EQ(converted.normalTextureTransform.offset, glm::vec2(.2f, -.4f));
  EXPECT_EQ(converted.normalTextureTransform.scale, glm::vec2(-2, .5));
  EXPECT_EQ(converted.normalTextureTransform.rotation, .25f);
  EXPECT_EQ(converted.normalTextureScale, -2.f);
  EXPECT_EQ(converted.diffuseTransmissionTextureTransform.texCoord, 0u);
  EXPECT_EQ(converted.diffuseTransmissionTextureTransform.channel, 3u);
  EXPECT_EQ(converted.diffuseTransmissionTextureTransform.rotation, -.7f);
  EXPECT_EQ(converted.diffuseTransmissionColorTextureTransform.channel, 4u);
}

TEST(GltfMaterialConversion, SpecularGlossinessSharesColorRgbAndLinearAlphaAndResetsToMr) {
  Fixture fixture(2);
  fixture.extension("KHR_materials_pbrSpecularGlossiness", {{"diffuseFactor", array({0, 0, 0, .6})},
      {"specularFactor", array({1, .766, .336})}, {"glossinessFactor", Value(.9)},
      {"diffuseTexture", info(0)}, {"specularGlossinessTexture", info(1)}});
  const auto sg = ConvertGltfMaterial(fixture.material, fixture.context());
  EXPECT_TRUE(sg.value.specularGlossinessWorkflow);
  EXPECT_EQ(sg.value.specularGlossinessDiffuse, glm::vec3(0));
  EXPECT_EQ(sg.value.specularGlossinessSpecular, glm::vec3(1, .766f, .336f));
  EXPECT_FLOAT_EQ(sg.value.baseColor.a, .6f);
  EXPECT_EQ(sg.value.baseColorTextureIndex, 1000u);
  EXPECT_EQ(sg.value.roughnessTextureIndex, 1001u);
  EXPECT_EQ(sg.value.specularColorTextureIndex, 1001u);
  EXPECT_EQ(sg.value.metallicRoughnessTextureIndex, invalid);
  EXPECT_EQ(sg.value.metalnessTextureIndex, invalid);
  EXPECT_EQ(sg.value.roughnessFactor, .9f); // Shader consumes glossiness in this workflow.
  EXPECT_NEAR(sg.value.baseColor.r, 1.f, 1e-5f);
  EXPECT_NEAR(sg.value.baseColor.g, .766f, 1e-5f);
  EXPECT_NEAR(sg.value.baseColor.b, .336f, 1e-5f);
  EXPECT_EQ(sg.sourceTextures[0], 0);
  EXPECT_EQ(sg.sourceTextures[5], 1);
  EXPECT_EQ(sg.sourceTextures[11], 1);
  fixture.material.extensions.erase("KHR_materials_pbrSpecularGlossiness");
  fixture.material.pbrMetallicRoughness.metallicRoughnessTexture.index = 0;
  const auto mr = ConvertGltfMaterial(fixture.material, fixture.context());
  EXPECT_FALSE(mr.value.specularGlossinessWorkflow);
  EXPECT_EQ(mr.value.specularGlossinessDiffuse, glm::vec3(1));
  EXPECT_EQ(mr.value.specularGlossinessSpecular, glm::vec3(1));
  EXPECT_EQ(mr.value.metallicRoughnessTextureIndex, 2000u);
  EXPECT_EQ(mr.value.roughnessTextureIndex, invalid);
  EXPECT_EQ(mr.value.specularColorTextureIndex, invalid);
  EXPECT_EQ(mr.value.metallicFactor, .4f);
  EXPECT_EQ(mr.value.roughnessFactor, .7f);
}

TEST(GltfMaterialConversion, SpecularGlossinessPackedColorNeedsNoSeparateDataUpload) {
  Fixture fixture(2);fixture.data.assign(2,invalid);
  fixture.extension("KHR_materials_pbrSpecularGlossiness",{{"diffuseTexture",info(0)},{"specularGlossinessTexture",info(1)}});
  const auto converted=ConvertGltfMaterial(fixture.material,fixture.context());
  EXPECT_EQ(converted.value.specularGlossinessDiffuse, glm::vec3(1));
  EXPECT_EQ(converted.value.specularGlossinessSpecular, glm::vec3(1));
  EXPECT_EQ(converted.value.roughnessTextureIndex,1001u);EXPECT_EQ(converted.value.specularColorTextureIndex,1001u);
  EXPECT_EQ(converted.sourceTextures[size_t(GltfMaterialTextureSlot::Roughness)],1);
  EXPECT_EQ(converted.sourceTextures[size_t(GltfMaterialTextureSlot::SpecularColor)],1);
  EXPECT_EQ(converted.runtimeBindings.runtimeIndices[size_t(GltfMaterialTextureSlot::Roughness)],1001u);
  EXPECT_EQ(converted.runtimeBindings.runtimeIndices[size_t(GltfMaterialTextureSlot::SpecularColor)],1001u);
  // Both transforms retain their shared source mapping; SG's shader chooses
  // alpha for glossiness and RGB for specular color from this Color resource.
  EXPECT_EQ(converted.value.roughnessTextureTransform.channel,4u);
  EXPECT_EQ(converted.value.specularColorTextureTransform.channel,4u);
  EXPECT_TRUE(GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot::Roughness,true));
  EXPECT_TRUE(GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot::Roughness,converted.value));
  EXPECT_FALSE(GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot::Roughness,false));
  fixture.color[1]=invalid;
  EXPECT_THROW((void)ConvertGltfMaterial(fixture.material,fixture.context()),std::invalid_argument);
}

TEST(GltfMaterialConversion, SharedSourceUsesColorForSgAndDataForIndependentMrRoughness) {
  Fixture fixture(2);
  fixture.extension("KHR_materials_pbrSpecularGlossiness",{{"specularGlossinessTexture",info(1)}});
  const auto sg=ConvertGltfMaterial(fixture.material,fixture.context());
  auto mrSource=fixture.material;mrSource.extensions.erase("KHR_materials_pbrSpecularGlossiness");mrSource.values["roughnessTexture"]=texture(1);
  const auto mr=ConvertGltfMaterial(mrSource,fixture.context());
  EXPECT_EQ(sg.value.roughnessTextureIndex,1001u);EXPECT_EQ(sg.value.specularColorTextureIndex,1001u);
  EXPECT_EQ(mr.value.roughnessTextureIndex,2001u);EXPECT_EQ(mr.value.specularColorTextureIndex,invalid);
  EXPECT_EQ(sg.sourceTextures[size_t(GltfMaterialTextureSlot::Roughness)],mr.sourceTextures[size_t(GltfMaterialTextureSlot::Roughness)]);
}

TEST(GltfMaterialConversion, SpecularGlossinessWithoutPackedTextureKeepsIndependentLegacyRoughnessData) {
  Fixture fixture(1);fixture.extension("KHR_materials_pbrSpecularGlossiness",{});
  fixture.material.values["roughnessTexture"]=texture(0);
  fixture.material.values["roughnessTexture"].json_double_value["channel"]=2;
  const auto converted=ConvertGltfMaterial(fixture.material,fixture.context());
  EXPECT_TRUE(converted.value.specularGlossinessWorkflow);EXPECT_EQ(converted.value.roughnessTextureIndex,2000u);
  EXPECT_EQ(converted.value.specularColorTextureIndex,invalid);EXPECT_EQ(converted.value.roughnessTextureTransform.channel,2u);
  EXPECT_EQ(converted.sourceTextures[size_t(GltfMaterialTextureSlot::Roughness)],0);
  EXPECT_EQ(converted.sourceTextures[size_t(GltfMaterialTextureSlot::SpecularColor)],-1);
  EXPECT_FALSE(GltfMaterialTextureUsesColorResource(GltfMaterialTextureSlot::Roughness,converted.value));
  fixture.color[0]=invalid;EXPECT_NO_THROW((void)ConvertGltfMaterial(fixture.material,fixture.context()));
  fixture.data[0]=invalid;EXPECT_THROW((void)ConvertGltfMaterial(fixture.material,fixture.context()),std::invalid_argument);
}

TEST(GltfMaterialConversion, TypedLegacyOpacityFactorsAndCustomChannelsHaveExplicitSemantics) {
  Fixture fixture(1);
  fixture.material.values["opacity"] = number(.8);
  fixture.material.additionalValues["opacity"] = number(.25);
  fixture.material.values["heightScale"] = number(-.2);
  fixture.material.values["heightOffset"] = number(.1);
  auto legacyTexture = texture(0);
  legacyTexture.json_double_value["texCoord"] = 1;
  legacyTexture.json_double_value["channel"] = 2;
  fixture.material.values["opacityTexture"] = legacyTexture;
  const auto value = ConvertGltfMaterial(fixture.material, fixture.context()).value;
  EXPECT_EQ(value.opacityFactor, .25f);
  EXPECT_EQ(value.alphaMode, AlphaMode::Blend);
  EXPECT_EQ(value.heightScale, -.2f);
  EXPECT_EQ(value.heightOffset, .1f);
  EXPECT_EQ(value.opacityTextureIndex, 2000u);
  EXPECT_EQ(value.opacityTextureTransform.texCoord, 1u);
  EXPECT_EQ(value.opacityTextureTransform.channel, 2u);
  fixture.material.alphaMode = "MASK";
  EXPECT_EQ(ConvertGltfMaterial(fixture.material, fixture.context()).value.alphaMode, AlphaMode::Mask);
  fixture.material.values["opacityTexture"].json_double_value["channel"] = 2.5;
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, fixture.context()), std::invalid_argument);
}

TEST(GltfMaterialConversion, PhysicalTransmissionPreservesCoverageModeInsteadOfEnablingAlphaBlend) {
  for (const auto* mode : {"OPAQUE", "MASK", "BLEND"}) {
    Fixture fixture(1);
    fixture.material.alphaMode = mode;
    fixture.extension("KHR_materials_transmission", {{"transmissionFactor", Value(.6)}, {"transmissionTexture", info(0)}});
    const auto value = ConvertGltfMaterial(fixture.material, fixture.context()).value;
    EXPECT_EQ(value.alphaMode, std::string_view(mode) == "MASK" ? AlphaMode::Mask : std::string_view(mode) == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque);
    EXPECT_EQ(value.transmissionFactor, .6f);
  }
}

TEST(GltfMaterialConversion, ExplicitPhysicalFactorsWinOverContradictoryLegacyFallbacks) {
  Fixture fixture(1);
  fixture.material.values["specular"] = number(.2);
  fixture.material.additionalValues["specular"] = number(.3);
  fixture.material.values["refraction"] = number(.1);
  fixture.extension("KHR_materials_specular", {{"specularFactor", Value(.8)}});
  fixture.extension("KHR_materials_transmission", {{"transmissionFactor", Value(.6)}});
  const auto physical = ConvertGltfMaterial(fixture.material, fixture.context()).value;
  EXPECT_EQ(physical.specularFactor, .8f);
  EXPECT_EQ(physical.transmissionFactor, .6f);
  fixture.material.extensions.erase("KHR_materials_specular");
  fixture.material.extensions.erase("KHR_materials_transmission");
  const auto legacy = ConvertGltfMaterial(fixture.material, fixture.context()).value;
  EXPECT_EQ(legacy.specularFactor, .3f);
  EXPECT_EQ(legacy.transmissionFactor, .1f);
}

TEST(GltfMaterialConversion, MissingRolesBadRuntimeReferencesAndTableShapesReject) {
  Fixture fixture(1);
  fixture.material.normalTexture.index = 0;
  fixture.data[0] = invalid;
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, fixture.context()), std::invalid_argument);
  fixture.data[0] = 3000;
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, fixture.context()), std::invalid_argument);
  fixture.data[0] = 19;
  fixture.material.pbrMetallicRoughness.baseColorTexture.index = 0;
  fixture.color[0] = invalid;
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, fixture.context()), std::invalid_argument);
  fixture.color[0] = 13;
  auto context = fixture.context();
  context.dataResources = {};
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, context), std::invalid_argument);
  context = fixture.context();
  context.runtimeResourceCount = 0;
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, context), std::invalid_argument);
}

TEST(GltfMaterialConversion, MalformedAndUnsupportedNumericTextureInputsRejectWithoutNarrowing) {
  Fixture fixture(1);
  const auto original = fixture.material;
  const std::vector<std::function<void(Fixture&)>> corruptions{
      [](auto& f) { f.material.pbrMetallicRoughness.roughnessFactor = std::numeric_limits<double>::quiet_NaN(); },
      [](auto& f) { f.material.normalTexture.index = 1; },
      [](auto& f) { f.material.normalTexture.index = 0; f.material.normalTexture.texCoord = 2; },
      [](auto& f) { f.material.alphaMode = "DITHER"; },
      [](auto& f) { f.material.values["heightScale"] = number(std::numeric_limits<double>::max()); },
      [](auto& f) { f.material.values["opacity"] = number(-.1); },
      [](auto& f) { f.material.values["heightTexture"] = texture(0); f.material.values["heightTexture"].json_double_value["index"] = .5; },
      [](auto& f) { f.extension("KHR_materials_transmission", {{"transmissionTexture", Value(Object{{"index", Value(0.)}})}}); },
      [](auto& f) { f.extension("KHR_materials_transmission", {{"transmissionTexture", Value(Object{{"index", Value(0)}, {"channel", Value(2)}})}}); }};
  for (size_t i = 0; i < corruptions.size(); ++i) {
    SCOPED_TRACE(i);
    auto bad = fixture;
    corruptions[i](bad);
    EXPECT_THROW(ConvertGltfMaterial(bad.material, bad.context()), std::invalid_argument);
  }
  EXPECT_EQ(fixture.material, original);
}

TEST(GltfMaterialConversion, MetadataAndResourceBudgetsRejectBeforeUnboundedScratch) {
  Fixture fixture(1);
  fixture.material.normalTexture.index = 0;
  const std::vector<std::function<void(GltfMaterialConversionLimits&)>> changes{
      [](auto& l) { l.maxSourceTextures = 0; }, [](auto& l) { l.maxRuntimeResources = UINT64_MAX; },
      [](auto& l) { l.maxRuntimeResources = 1; }, [](auto& l) { l.validation.maxOutputBytes = 1; },
      [](auto& l) { l.validation.maxOutputBytes = UINT64_MAX; }, [](auto& l) { l.validation.maxMetadataValues = 1; },
      [](auto& l) { l.validation.maxMetadataDepth = 0; }};
  for (size_t i = 0; i < changes.size(); ++i) {
    SCOPED_TRACE(i);
    GltfMaterialConversionLimits limits;
    changes[i](limits);
    EXPECT_THROW(ConvertGltfMaterial(fixture.material, fixture.context(), GltfMaterialConversionPolicy::StrictGltf, limits), std::invalid_argument);
  }
  Value metadata(1);
  for (int depth = 0; depth < 12; ++depth) metadata = Value(Object{{"nested", std::move(metadata)}});
  fixture.material.extras = metadata;
  GltfMaterialConversionLimits bounded;
  bounded.validation.maxMetadataDepth = 4;
  EXPECT_THROW(ConvertGltfMaterial(fixture.material, fixture.context(), GltfMaterialConversionPolicy::StrictGltf, bounded), std::invalid_argument);
}

TEST(GltfMaterialConversion, ValidCoreAndSgImportsMatchExistingNumericAndRoleContracts) {
  for (const bool sg : {false, true}) {
    Fixture fixture(2);
    fixture.material.values["metallicFactor"] = number(.4);
    fixture.material.doubleSided = true;
    fixture.material.pbrMetallicRoughness.baseColorFactor = {.2, .4, .6, .8};
    fixture.material.pbrMetallicRoughness.baseColorTexture.index = 0;
    fixture.material.normalTexture.index = 1;
    if (sg) fixture.extension("KHR_materials_pbrSpecularGlossiness", {{"diffuseFactor", array({0, 0, 0, .6})},
        {"specularFactor", array({1, .766, .336})}, {"glossinessFactor", Value(.9)}, {"diffuseTexture", info(0)}, {"specularGlossinessTexture", info(1)}});
    tinygltf::Model model;
    model.materials = {fixture.material};
    model.textures.resize(2);
    model.images.resize(2);
    model.textures[0].source = 0;
    model.textures[1].source = 1;
    model.extensionsUsed = fixture.declarations;
    SlangMaterialXBridge bridge;
    MaterialManager manager;
    uint32_t first = invalid;
    bridge.loadMaterialsForGltfWithTextureRoles(model, {fixture.color, fixture.data}, manager, first);
    ASSERT_EQ(manager.materialCount(), 1u);
    const auto converted = ConvertGltfMaterial(fixture.material, fixture.context());
    const auto& old = *manager.getMaterial(first);
    EXPECT_NEAR(converted.value.baseColor.r, old.baseColor.r, 1e-6f);
    EXPECT_NEAR(converted.value.baseColor.g, old.baseColor.g, 1e-6f);
    EXPECT_NEAR(converted.value.baseColor.b, old.baseColor.b, 1e-6f);
    EXPECT_EQ(converted.value.baseColor.a, old.baseColor.a);
    EXPECT_NEAR(converted.value.metallicFactor, old.metallicFactor, 1e-6f);
    EXPECT_EQ(converted.value.roughnessFactor, old.roughnessFactor);
    EXPECT_EQ(converted.value.specularGlossinessDiffuse, old.specularGlossinessDiffuse);
    EXPECT_EQ(converted.value.specularGlossinessSpecular, old.specularGlossinessSpecular);
    EXPECT_EQ(converted.value.specularGlossinessWorkflow, old.specularGlossinessWorkflow);
    EXPECT_EQ(converted.value.doubleSided, old.doubleSided);
    EXPECT_EQ(converted.runtimeBindings.runtimeIndices, container::geometry::gltf::snapshotMaterialTextureBindings(old).runtimeIndices);
  }
}
} // namespace
