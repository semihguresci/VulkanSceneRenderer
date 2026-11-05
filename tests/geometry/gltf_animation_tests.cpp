#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/utility/Material.h>
#include <Container/utility/SceneGraph.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace container::geometry::gltf;

int floatAccessor(tinygltf::Model& model, int type,
                  std::initializer_list<float> data) {
  if (model.buffers.empty()) model.buffers.emplace_back();
  auto& bytes = model.buffers[0].data;
  while (bytes.size() % 4) bytes.push_back(0);
  tinygltf::BufferView view;
  view.buffer = 0; view.byteOffset = bytes.size(); view.byteLength = data.size()*4;
  for (float value : data) {
    const auto bits = std::bit_cast<uint32_t>(value);
    for (size_t i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(bits >> (8*i)));
  }
  tinygltf::Accessor accessor;
  accessor.bufferView = static_cast<int>(model.bufferViews.size());
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.type = type;
  accessor.count = data.size()/static_cast<size_t>(tinygltf::GetNumComponentsInType(type));
  if (type == TINYGLTF_TYPE_SCALAR && data.size() != 0) {
    const auto [minimum, maximum] = std::minmax_element(data.begin(), data.end());
    accessor.minValues = {*minimum}; accessor.maxValues = {*maximum};
  }
  model.bufferViews.push_back(view);
  model.accessors.push_back(accessor);
  return static_cast<int>(model.accessors.size()-1);
}
int doubleAccessor(tinygltf::Model& model,int type,std::initializer_list<double> data) {
  if (model.buffers.empty()) model.buffers.emplace_back();
  auto& bytes = model.buffers[0].data; while (bytes.size()%8) bytes.push_back(0);
  tinygltf::BufferView view; view.buffer = 0; view.byteOffset = bytes.size(); view.byteLength = data.size()*8;
  for (double value : data) {
    const auto bits = std::bit_cast<uint64_t>(value);
    for (size_t i = 0; i < 8; ++i) bytes.push_back(static_cast<unsigned char>(bits >> (i*8)));
  }
  tinygltf::Accessor accessor; accessor.bufferView = int(model.bufferViews.size()); accessor.componentType = 5130;
  accessor.type = type; accessor.count = data.size()/size_t(tinygltf::GetNumComponentsInType(type));
  if (type == TINYGLTF_TYPE_SCALAR) { accessor.minValues = {*std::min_element(data.begin(),data.end())}; accessor.maxValues = {*std::max_element(data.begin(),data.end())}; }
  model.bufferViews.push_back(view); model.accessors.push_back(accessor); return int(model.accessors.size()-1);
}
tinygltf::Model translationModel(const std::string& interpolation = "LINEAR") {
  tinygltf::Model model;
  model.nodes.resize(1);
  tinygltf::Animation animation;
  animation.name = "fixture";
  tinygltf::AnimationSampler sampler;
  sampler.input = floatAccessor(model, TINYGLTF_TYPE_SCALAR, {1, 3});
  sampler.output = floatAccessor(model, TINYGLTF_TYPE_VEC3, {2,4,6, 6,8,10});
  sampler.interpolation = interpolation;
  animation.samplers.push_back(sampler);
  tinygltf::AnimationChannel channel;
  channel.sampler = 0; channel.target_node = 0; channel.target_path = "translation";
  animation.channels.push_back(channel);
  model.animations.push_back(animation);
  return model;
}
void setPointer(tinygltf::AnimationChannel& channel, const std::string& pointer) {
  channel.target_node = -1; channel.target_path = "pointer";
  channel.target_extensions["KHR_animation_pointer"] = tinygltf::Value(
    tinygltf::Value::Object{{"pointer", tinygltf::Value(pointer)}});
}
void addPointer(tinygltf::Model& model, const std::string& pointer, int type,
                std::initializer_list<float> values) {
  if (model.animations.empty()) model.animations.emplace_back();
  auto& animation = model.animations[0];
  tinygltf::AnimationSampler sampler;
  sampler.input = floatAccessor(model, TINYGLTF_TYPE_SCALAR, {0, 2});
  sampler.output = floatAccessor(model, type, values);
  tinygltf::AnimationChannel channel;
  channel.sampler = static_cast<int>(animation.samplers.size());
  setPointer(channel, pointer);
  animation.samplers.push_back(sampler);
  animation.channels.push_back(channel);
}
const std::vector<double>& onlyValue(const AnimationPose& pose) {
  return pose.values.at(0).value;
}
tinygltf::Value uvTransform(tinygltf::Value::Object properties = {}) {
  return tinygltf::Value(std::move(properties));
}
void expectValueEqual(const tinygltf::Value& actual, const tinygltf::Value& expected) {
  ASSERT_EQ(actual.Type(),expected.Type());
  if (actual.IsNumber()) EXPECT_DOUBLE_EQ(actual.GetNumberAsDouble(),expected.GetNumberAsDouble());
  else if (actual.IsString()) EXPECT_EQ(actual.Get<std::string>(),expected.Get<std::string>());
  else if (actual.IsBool()) EXPECT_EQ(actual.Get<bool>(),expected.Get<bool>());
  else if (actual.IsArray()) {
    ASSERT_EQ(actual.ArrayLen(),expected.ArrayLen());
    for (size_t i = 0; i < actual.ArrayLen(); ++i) expectValueEqual(actual.Get(i),expected.Get(i));
  } else if (actual.IsObject()) {
    ASSERT_EQ(actual.Keys(),expected.Keys());
    for (const auto& key : actual.Keys()) { SCOPED_TRACE(key); expectValueEqual(actual.Get(key),expected.Get(key)); }
  }
}
void expectMaterialDataEqual(const tinygltf::Material& actual, const tinygltf::Material& expected) {
  // Compare exact Value types/optional objects without linking TinyGLTF's file
  // loader implementation into this CPU-only animation target.
  expectValueEqual(tinygltf::Value(actual.extensions),tinygltf::Value(expected.extensions));
  expectValueEqual(actual.extras,expected.extras);
  const auto texture = [&](const auto& a, const auto& e) {
    EXPECT_EQ(a.index,e.index); EXPECT_EQ(a.texCoord,e.texCoord);
    expectValueEqual(tinygltf::Value(a.extensions),tinygltf::Value(e.extensions)); expectValueEqual(a.extras,e.extras);
    EXPECT_EQ(a.extras_json_string,e.extras_json_string); EXPECT_EQ(a.extensions_json_string,e.extensions_json_string);
  };
  texture(actual.pbrMetallicRoughness.baseColorTexture,expected.pbrMetallicRoughness.baseColorTexture);
  texture(actual.pbrMetallicRoughness.metallicRoughnessTexture,expected.pbrMetallicRoughness.metallicRoughnessTexture);
  texture(actual.normalTexture,expected.normalTexture); texture(actual.occlusionTexture,expected.occlusionTexture);
  texture(actual.emissiveTexture,expected.emissiveTexture);
  EXPECT_EQ(actual.pbrMetallicRoughness.baseColorFactor,expected.pbrMetallicRoughness.baseColorFactor);
  EXPECT_DOUBLE_EQ(actual.pbrMetallicRoughness.metallicFactor,expected.pbrMetallicRoughness.metallicFactor);
  EXPECT_DOUBLE_EQ(actual.pbrMetallicRoughness.roughnessFactor,expected.pbrMetallicRoughness.roughnessFactor);
}
void setTextureTransform(tinygltf::Material& material, const std::string& path,
                         const tinygltf::Value& transform) {
  const tinygltf::Value extras(tinygltf::Value::Object{{"authored",tinygltf::Value(std::string("preserve"))}});
  const tinygltf::ExtensionMap extensions{{"KHR_texture_transform",transform},{"VENDOR_metadata",extras}};
  const auto typed = [&](auto& info) { info.index = 0; info.texCoord = 1; info.extensions = extensions; info.extras = extras; };
  if (path == "pbrMetallicRoughness/baseColorTexture") typed(material.pbrMetallicRoughness.baseColorTexture);
  else if (path == "pbrMetallicRoughness/metallicRoughnessTexture") typed(material.pbrMetallicRoughness.metallicRoughnessTexture);
  else if (path == "normalTexture") typed(material.normalTexture);
  else if (path == "occlusionTexture") typed(material.occlusionTexture);
  else if (path == "emissiveTexture") typed(material.emissiveTexture);
  else {
    const auto slash = path.find('/',11);
    const auto extensionName = path.substr(11,slash-11), textureName = path.substr(slash+1);
    auto& extension = material.extensions[extensionName];
    if (!extension.IsObject()) extension = tinygltf::Value(tinygltf::Value::Object{});
    extension.Get<tinygltf::Value::Object>()[textureName] = tinygltf::Value(tinygltf::Value::Object{
      {"index",tinygltf::Value(0)}, {"texCoord",tinygltf::Value(1)}, {"extras",extras},
      {"extensions",tinygltf::Value(tinygltf::Value::Object(extensions.begin(),extensions.end()))}});
  }
}
struct TextureSlot {
  const char* path;
  container::material::TextureTransform container::material::Material::* transform;
  uint32_t container::material::Material::* handle;
};
const TextureSlot textureSlots[]{
  {"pbrMetallicRoughness/baseColorTexture",&container::material::Material::baseColorTextureTransform,&container::material::Material::baseColorTextureIndex},
  {"pbrMetallicRoughness/metallicRoughnessTexture",&container::material::Material::metallicRoughnessTextureTransform,&container::material::Material::metallicRoughnessTextureIndex},
  {"normalTexture",&container::material::Material::normalTextureTransform,&container::material::Material::normalTextureIndex},
  {"occlusionTexture",&container::material::Material::occlusionTextureTransform,&container::material::Material::occlusionTextureIndex},
  {"emissiveTexture",&container::material::Material::emissiveTextureTransform,&container::material::Material::emissiveTextureIndex},
  {"extensions/KHR_materials_specular/specularTexture",&container::material::Material::specularTextureTransform,&container::material::Material::specularTextureIndex},
  {"extensions/KHR_materials_specular/specularColorTexture",&container::material::Material::specularColorTextureTransform,&container::material::Material::specularColorTextureIndex},
  {"extensions/KHR_materials_transmission/transmissionTexture",&container::material::Material::transmissionTextureTransform,&container::material::Material::transmissionTextureIndex},
  {"extensions/KHR_materials_clearcoat/clearcoatTexture",&container::material::Material::clearcoatTextureTransform,&container::material::Material::clearcoatTextureIndex},
  {"extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture",&container::material::Material::clearcoatRoughnessTextureTransform,&container::material::Material::clearcoatRoughnessTextureIndex},
  {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture",&container::material::Material::clearcoatNormalTextureTransform,&container::material::Material::clearcoatNormalTextureIndex},
  {"extensions/KHR_materials_volume/thicknessTexture",&container::material::Material::thicknessTextureTransform,&container::material::Material::thicknessTextureIndex},
  {"extensions/KHR_materials_sheen/sheenColorTexture",&container::material::Material::sheenColorTextureTransform,&container::material::Material::sheenColorTextureIndex},
  {"extensions/KHR_materials_sheen/sheenRoughnessTexture",&container::material::Material::sheenRoughnessTextureTransform,&container::material::Material::sheenRoughnessTextureIndex},
  {"extensions/KHR_materials_iridescence/iridescenceTexture",&container::material::Material::iridescenceTextureTransform,&container::material::Material::iridescenceTextureIndex},
  {"extensions/KHR_materials_iridescence/iridescenceThicknessTexture",&container::material::Material::iridescenceThicknessTextureTransform,&container::material::Material::iridescenceThicknessTextureIndex},
  {"extensions/KHR_materials_anisotropy/anisotropyTexture",&container::material::Material::anisotropyTextureTransform,&container::material::Material::anisotropyTextureIndex},
  {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",&container::material::Material::diffuseTransmissionTextureTransform,&container::material::Material::diffuseTransmissionTextureIndex},
  {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",&container::material::Material::diffuseTransmissionColorTextureTransform,&container::material::Material::diffuseTransmissionColorTextureIndex},
  {"extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",&container::material::Material::baseColorTextureTransform,&container::material::Material::baseColorTextureIndex},
  {"extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture",&container::material::Material::roughnessTextureTransform,&container::material::Material::roughnessTextureIndex},
};

TEST(GltfAnimation, DeclaredFloat64PreservesTimeKeyAndInterpolationPrecision) {
  auto model = translationModel(); model.extensionsRequired = {"KHR_accessor_float64"};
  model.animations[0].samplers[0].input = doubleAccessor(model,TINYGLTF_TYPE_SCALAR,{.1,.3});
  model.animations[0].samplers[0].output = doubleAccessor(model,TINYGLTF_TYPE_VEC3,
    {1.0000000001,0,0, 1.0000000003,0,0});
  const auto library = ParseAnimations(model);
  EXPECT_EQ(library.clips[0].tracks[0].times,(std::vector<double>{.1,.3}));
  EXPECT_DOUBLE_EQ(library.clips[0].tracks[0].values[0],1.0000000001);
  const auto pose = SampleAnimation(library.clips[0],.2);
  EXPECT_NEAR(pose.values[0].value[0],1.0000000002,1e-15);
  ApplyAnimationPose(model,pose); EXPECT_NEAR(model.nodes[0].translation[0],1.0000000002,1e-15);
  model.extensionsRequired.clear(); EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
}
TEST(GltfAnimation, UnsafeFloat64PropertiesRejectBeforeRuntimePublication) {
  for (double value : {1e100,1e-300}) {
    auto model = translationModel(); model.extensionsRequired = {"KHR_accessor_float64"};
    model.animations[0].samplers[0].output = doubleAccessor(model,TINYGLTF_TYPE_VEC3,{value,0,0,value,0,0});
    const auto library = ParseAnimations(model); const auto pose = SampleAnimation(library.clips[0],2);
    EXPECT_DOUBLE_EQ(pose.values[0].value[0],value); // Offline sampling keeps authored binary64.
    EXPECT_THROW(ApplyAnimationPose(model,pose),std::invalid_argument);
    EXPECT_TRUE(model.nodes[0].translation.empty());
  }
}
tinygltf::Model lateUnsafeModel(double unsafe = 1e100) {
  auto model = translationModel(); model.extensionsRequired = {"KHR_accessor_float64"};
  model.nodes[0].translation = {9,8,7}; model.materials.resize(1);
  addPointer(model,"/materials/0/alphaCutoff",TINYGLTF_TYPE_SCALAR,{.25f,.5f});
  auto& sampler = model.animations[0].samplers.back(); sampler.interpolation = "STEP";
  sampler.output = doubleAccessor(model,TINYGLTF_TYPE_SCALAR,{.25,unsafe});
  return model;
}
TEST(GltfAnimation, EntirePoseRangeValidationPrecedesSafeFirstChannelWrites) {
  for (double unsafe : {1e100,1e-300}) {
    auto model = lateUnsafeModel(unsafe); const auto library = ParseAnimations(model);
    const auto pose = SampleAnimation(library.clips[0],3);
    ASSERT_EQ(pose.values.size(),2u); EXPECT_EQ(pose.values[0].value,(std::vector<double>{6,8,10}));
    const auto bytes = model.buffers[0].data; const auto* address = model.buffers[0].data.data();
    EXPECT_THROW(ApplyAnimationPose(model,pose),std::invalid_argument);
    EXPECT_EQ(model.nodes[0].translation,(std::vector<double>{9,8,7}));
    EXPECT_DOUBLE_EQ(model.materials[0].alphaCutoff,.5);
    EXPECT_EQ(model.buffers[0].data,bytes); EXPECT_EQ(model.buffers[0].data.data(),address);
  }
}
TEST(GltfAnimation, FailedSeekAndAdvanceRetainLastSourceTimeAndPlayingState) {
  for (bool loop : {false,true}) {
    auto model = lateUnsafeModel(); AnimationPlayer player(model);
    player.selectClip(model,0); player.seek(model,1.5); player.setLoop(loop); player.play();
    const auto translation = model.nodes[0].translation;
    EXPECT_THROW(player.seek(model,3),std::invalid_argument);
    EXPECT_EQ(model.nodes[0].translation,translation); EXPECT_DOUBLE_EQ(model.materials[0].alphaCutoff,.25);
    EXPECT_DOUBLE_EQ(player.time(),1.5); EXPECT_EQ(player.selectedClip(),0u); EXPECT_TRUE(player.playing());
    // Non-looping advance would stop at the invalid endpoint. Looping advance
    // reaches the second STEP key without wrapping. Neither state may commit.
    EXPECT_THROW(player.advance(model,loop ? .5 : 100),std::invalid_argument);
    EXPECT_EQ(model.nodes[0].translation,translation); EXPECT_DOUBLE_EQ(model.materials[0].alphaCutoff,.25);
    EXPECT_DOUBLE_EQ(player.time(),1.5); EXPECT_EQ(player.selectedClip(),0u); EXPECT_TRUE(player.playing());
  }
}
TEST(GltfAnimation, FailedClipSelectionPreservesPreviousPoseAndSuccessfulSelectionRestoresRest) {
  auto model = lateUnsafeModel();
  auto unsafeClip = model.animations[0];
  unsafeClip.samplers[1].output = doubleAccessor(model,TINYGLTF_TYPE_SCALAR,{1e100,1e100});
  auto safeClip = model.animations[0]; safeClip.channels.resize(1); safeClip.channels[0].target_path = "scale";
  model.animations.push_back(unsafeClip); model.animations.push_back(safeClip);
  AnimationPlayer player(model); player.selectClip(model,0); player.seek(model,1.5); player.play();
  const auto translation = model.nodes[0].translation;
  EXPECT_THROW(player.selectClip(model,1),std::invalid_argument);
  EXPECT_EQ(model.nodes[0].translation,translation); EXPECT_DOUBLE_EQ(model.materials[0].alphaCutoff,.25);
  EXPECT_DOUBLE_EQ(player.time(),1.5); EXPECT_EQ(player.selectedClip(),0u); EXPECT_TRUE(player.playing());
  const auto dirty = player.selectClip(model,2);
  EXPECT_EQ(dirty.nodes,(std::vector<size_t>{0})); EXPECT_EQ(dirty.materials,(std::vector<size_t>{0}));
  EXPECT_EQ(model.nodes[0].translation,(std::vector<double>{9,8,7}));
  EXPECT_EQ(model.nodes[0].scale,(std::vector<double>{2,4,6})); EXPECT_DOUBLE_EQ(model.materials[0].alphaCutoff,.5);
  EXPECT_EQ(player.selectedClip(),2u); EXPECT_DOUBLE_EQ(player.time(),0);
  player.seek(model,1.5);
  const auto scale = model.nodes[0].scale;
  model.nodes.emplace_back(); // Structure mismatch must not partially clear/rest.
  EXPECT_THROW(player.clearClip(model),std::invalid_argument);
  EXPECT_EQ(model.nodes[0].scale,scale); EXPECT_EQ(player.selectedClip(),2u);
  EXPECT_DOUBLE_EQ(player.time(),1.5); EXPECT_TRUE(player.playing());
  model.nodes.pop_back(); player.clearClip(model);
  EXPECT_TRUE(model.nodes[0].scale.empty()); EXPECT_FALSE(player.selectedClip());
  EXPECT_DOUBLE_EQ(player.time(),0); EXPECT_FALSE(player.playing());
}
TEST(GltfAnimation, LaterStructuralFailureRollsBackOnlyTouchedSourceObjects) {
  auto model = translationModel(); model.materials.resize(1);
  model.materials[0].extensions["KHR_materials_ior"] = uvTransform();
  addPointer(model,"/materials/0/extensions/KHR_materials_ior/ior",TINYGLTF_TYPE_SCALAR,{1.25f,1.75f});
  const auto library = ParseAnimations(model); const auto pose = SampleAnimation(library.clips[0],2);
  model.materials[0].extensions.clear(); // The target was valid when parsed.
  model.nodes[0].translation = {9,8,7}; model.nodes.push_back(tinygltf::Node{});
  model.nodes[1].name = "Unrelated editor metadata";
  EXPECT_THROW(ApplyAnimationPose(model,pose),std::invalid_argument);
  EXPECT_EQ(model.nodes[0].translation,(std::vector<double>{9,8,7}));
  EXPECT_TRUE(model.materials[0].extensions.empty()); EXPECT_EQ(model.nodes[1].name,"Unrelated editor metadata");
}
TEST(GltfAnimation, WideFloat64CubicDerivativesRemainAuthoredAndOnlySamplesAreRangeChecked) {
  for (double interval : {1e-40,2.0}) {
    auto model = translationModel("CUBICSPLINE"); model.extensionsRequired = {"KHR_accessor_float64"};
    model.animations[0].samplers[0].input = doubleAccessor(model,TINYGLTF_TYPE_SCALAR,{0,interval});
    model.animations[0].samplers[0].output = doubleAccessor(model,TINYGLTF_TYPE_VEC3,
      {0,0,0, 0,0,0, 1e40,0,0, 0,0,0, 1,0,0, 0,0,0});
    const auto library = ParseAnimations(model); EXPECT_DOUBLE_EQ(library.clips[0].tracks[0].values[6],1e40);
    const auto pose = SampleAnimation(library.clips[0],interval/2);
    if (interval < 1) {
      EXPECT_NEAR(pose.values[0].value[0],.625,1e-14); EXPECT_NO_THROW(ApplyAnimationPose(model,pose));
    } else EXPECT_THROW(ApplyAnimationPose(model,pose),std::invalid_argument);
  }
}

TEST(GltfAnimationRuntime, TextureMotionSamplesAllShaderSlotsWithoutChangingBindingsOrMetadata) {
  for (const auto& slot : textureSlots) {
    SCOPED_TRACE(slot.path);
    tinygltf::Model model; model.materials.resize(1);
    const auto transform = uvTransform({{"texCoord",tinygltf::Value(1)},
      {"extras",tinygltf::Value(std::string("source metadata"))}});
    setTextureTransform(model.materials[0],slot.path,transform);
    const auto rest = model.materials[0];
    const std::string prefix = "/materials/0/" + std::string(slot.path) + "/extensions/KHR_texture_transform/";
    addPointer(model,prefix+"offset",TINYGLTF_TYPE_VEC2,{0,.25f, .5f,.75f});
    addPointer(model,prefix+"scale",TINYGLTF_TYPE_VEC2,{1,2, 3,4});
    addPointer(model,prefix+"rotation",TINYGLTF_TYPE_SCALAR,{0,1});
    AnimationPlayer player(model);
    ASSERT_TRUE(player.library().diagnostics.empty());
    player.selectClip(model,0); const auto dirty = player.seek(model,1);
    EXPECT_EQ(dirty.materials,(std::vector<size_t>{0}));
    container::material::Material runtime;
    auto& actual = runtime.*slot.transform;
    actual.texCoord = 1; actual.channel = 3;
    for (const auto& other : textureSlots) runtime.*other.handle = 101;
    UpdateAnimatedMaterialProperties(model.materials[0],runtime);
    EXPECT_EQ(actual.offset,glm::vec2(.25f,.5f));
    EXPECT_EQ(actual.scale,glm::vec2(2,3)); EXPECT_FLOAT_EQ(actual.rotation,.5f);
    EXPECT_EQ(actual.texCoord,1u); EXPECT_EQ(actual.channel,3u);
    for (const auto& other : textureSlots) EXPECT_EQ(runtime.*other.handle,101u);
    auto expected = rest;
    auto expectedTransform = transform;
    auto& expectedObject = expectedTransform.Get<tinygltf::Value::Object>();
    expectedObject["offset"] = tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(.25),tinygltf::Value(.5)});
    expectedObject["scale"] = tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(2.0),tinygltf::Value(3.0)});
    expectedObject["rotation"] = tinygltf::Value(.5);
    setTextureTransform(expected,slot.path,expectedTransform);
    expectMaterialDataEqual(model.materials[0],expected);
    player.clearClip(model); expectMaterialDataEqual(model.materials[0],rest);
    UpdateAnimatedMaterialProperties(model.materials[0],runtime);
    EXPECT_EQ(actual.offset,glm::vec2(0)); EXPECT_EQ(actual.scale,glm::vec2(1));
    EXPECT_FLOAT_EQ(actual.rotation,0); EXPECT_EQ(actual.texCoord,1u); EXPECT_EQ(actual.channel,3u);
  }
}
TEST(GltfAnimation, TextureElementChannelsPreserveDefaultsAndExistingSibling) {
  tinygltf::Model model; model.materials.resize(1);
  setTextureTransform(model.materials[0],"normalTexture",uvTransform({
    {"offset",tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(.125),tinygltf::Value(.25)})}}));
  const std::string prefix = "/materials/0/normalTexture/extensions/KHR_texture_transform/";
  addPointer(model,prefix+"offset/1",TINYGLTF_TYPE_SCALAR,{0,1});
  addPointer(model,prefix+"scale/0",TINYGLTF_TYPE_SCALAR,{2,4});
  auto library = ParseAnimations(model);
  ApplyAnimationPose(model,SampleAnimation(library.clips[0],1));
  const auto& value = model.materials[0].normalTexture.extensions.at("KHR_texture_transform");
  EXPECT_DOUBLE_EQ(value.Get("offset").Get(size_t(0)).GetNumberAsDouble(),.125);
  EXPECT_DOUBLE_EQ(value.Get("offset").Get(size_t(1)).GetNumberAsDouble(),.5);
  EXPECT_DOUBLE_EQ(value.Get("scale").Get(size_t(0)).GetNumberAsDouble(),3);
  EXPECT_DOUBLE_EQ(value.Get("scale").Get(size_t(1)).GetNumberAsDouble(),1);
  addPointer(model,prefix+"scale/1",TINYGLTF_TYPE_SCALAR,{3,5});
  EXPECT_NO_THROW(ParseAnimations(model));
  addPointer(model,prefix+"scale",TINYGLTF_TYPE_VEC2,{1,1,2,2});
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
}
TEST(GltfAnimation, TextureCubicDerivativesAndStepRotationUseAuthoredValues) {
  tinygltf::Model model; model.materials.resize(1);
  setTextureTransform(model.materials[0],"emissiveTexture",uvTransform());
  const std::string prefix = "/materials/0/emissiveTexture/extensions/KHR_texture_transform/";
  addPointer(model,prefix+"scale",TINYGLTF_TYPE_VEC2,{
    0,0, 1,2, 4,-2, 0,0, 3,6, 0,0});
  model.animations[0].samplers[0].interpolation = "CUBICSPLINE";
  addPointer(model,prefix+"rotation",TINYGLTF_TYPE_SCALAR,{-.5f,2});
  model.animations[0].samplers[1].interpolation = "STEP";
  const auto library = ParseAnimations(model);
  EXPECT_EQ(library.clips[0].tracks[0].values,(std::vector<double>{0,0,1,2,4,-2,0,0,3,6,0,0}));
  const auto pose = SampleAnimation(library.clips[0],1);
  EXPECT_EQ(pose.values[0].value,(std::vector<double>{3,3.5}));
  EXPECT_EQ(pose.values[1].value,(std::vector<double>{-.5}));
  const auto nearStart = SampleAnimation(library.clips[0],1e-6).values[0].value;
  EXPECT_NEAR((nearStart[0]-1)/1e-6,4,1e-5);
  EXPECT_NEAR((nearStart[1]-2)/1e-6,-2,1e-5);
}
TEST(GltfAnimation, TextureParentsAndNumericLeavesAreValidatedWithoutSynthesizingObjects) {
  tinygltf::Model model; model.materials.resize(1);
  const std::string pointer = "/materials/0/extensions/KHR_materials_clearcoat/clearcoatNormalTexture/extensions/KHR_texture_transform/offset";
  addPointer(model,pointer,TINYGLTF_TYPE_VEC2,{0,0,1,1});
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  model.materials[0].extensions["KHR_materials_clearcoat"] = uvTransform();
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  setTextureTransform(model.materials[0],"extensions/KHR_materials_clearcoat/clearcoatNormalTexture",uvTransform());
  EXPECT_NO_THROW(ParseAnimations(model));
  auto& texture = model.materials[0].extensions.at("KHR_materials_clearcoat").Get<tinygltf::Value::Object>().at("clearcoatNormalTexture");
  texture.Get<tinygltf::Value::Object>().erase("index");
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  for (const auto& malformed : {tinygltf::Value(1),tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(0.0)}),
         tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(0.0),tinygltf::Value(std::string("bad"))})}) {
    setTextureTransform(model.materials[0],"extensions/KHR_materials_clearcoat/clearcoatNormalTexture",uvTransform({{"offset",malformed}}));
    EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  }
  setTextureTransform(model.materials[0],"normalTexture",uvTransform());
  model.materials[0].normalTexture.extensions.erase("KHR_texture_transform");
  setPointer(model.animations[0].channels[0],"/materials/0/normalTexture/extensions/KHR_texture_transform/offset");
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
}
TEST(GltfAnimation, TextureDiscreteAndShaderUnrepresentedTargetsRemainDiagnostics) {
  tinygltf::Model model; model.materials.resize(1);
  for (const char* path : {"pbrMetallicRoughness/baseColorTexture/index",
      "normalTexture/texCoord", "normalTexture/extensions/KHR_texture_transform/texCoord",
      "extensions/KHR_materials_anisotropy/anisotropyTexture/index", "extras/customFloat"})
    addPointer(model,"/materials/0/"+std::string(path),TINYGLTF_TYPE_SCALAR,{0,1});
  const auto library = ParseAnimations(model);
  EXPECT_EQ(library.diagnostics.size(),5u); EXPECT_TRUE(library.clips[0].tracks.empty());
}
TEST(GltfAnimationRuntime, ExtendedPbrFactorsRetainStrictZerosMetadataAndAuthoredRest) {
  tinygltf::Model model; model.materials.resize(1);
  using Value = tinygltf::Value;
  using Object = Value::Object;
  const Value metadata(Object{{"author",Value(std::string("retain"))}});
  model.materials[0].extensions["KHR_materials_anisotropy"] = Value(Object{{"extras",metadata}});
  model.materials[0].extensions["KHR_materials_diffuse_transmission"] = Value(Object{{"VENDOR_property",metadata}});
  for (const auto* path : {"extensions/KHR_materials_anisotropy/anisotropyTexture",
                          "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
                          "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture"})
    setTextureTransform(model.materials[0],path,uvTransform());
  const auto rest = model.materials[0];
  addPointer(model,"/materials/0/extensions/KHR_materials_anisotropy/anisotropyStrength",TINYGLTF_TYPE_SCALAR,{0,1});
  addPointer(model,"/materials/0/extensions/KHR_materials_anisotropy/anisotropyRotation",TINYGLTF_TYPE_SCALAR,{-2,2});
  addPointer(model,"/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",TINYGLTF_TYPE_SCALAR,{0,1});
  addPointer(model,"/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",TINYGLTF_TYPE_VEC3,
      {0,0,0, .5f,1,.25f});
  AnimationPlayer player(model);
  ASSERT_TRUE(player.library().diagnostics.empty());
  ASSERT_EQ(player.library().clips[0].tracks.size(),4u);
  player.selectClip(model,0); player.seek(model,0);
  container::material::Material runtime;
  runtime.anisotropyTextureIndex = runtime.diffuseTransmissionTextureIndex = runtime.diffuseTransmissionColorTextureIndex = 97;
  runtime.diffuseTransmissionTextureTransform.channel = 3;
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_FLOAT_EQ(runtime.anisotropyStrength,0);
  EXPECT_FLOAT_EQ(runtime.diffuseTransmissionFactor,0);
  EXPECT_EQ(runtime.diffuseTransmissionColorFactor,glm::vec3(0));
  player.seek(model,1);
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_FLOAT_EQ(runtime.anisotropyStrength,.5f); EXPECT_FLOAT_EQ(runtime.anisotropyRotation,0);
  EXPECT_FLOAT_EQ(runtime.diffuseTransmissionFactor,.5f);
  EXPECT_EQ(runtime.diffuseTransmissionColorFactor,glm::vec3(.25f,.5f,.125f));
  EXPECT_EQ(runtime.anisotropyTextureIndex,97u); EXPECT_EQ(runtime.diffuseTransmissionTextureIndex,97u);
  EXPECT_EQ(runtime.diffuseTransmissionColorTextureIndex,97u);
  EXPECT_EQ(runtime.diffuseTransmissionTextureTransform.channel,3u);
  expectValueEqual(model.materials[0].extensions.at("KHR_materials_anisotropy").Get("extras"),metadata);
  expectValueEqual(model.materials[0].extensions.at("KHR_materials_diffuse_transmission").Get("VENDOR_property"),metadata);
  player.clearClip(model);
  expectMaterialDataEqual(model.materials[0],rest);
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_FLOAT_EQ(runtime.anisotropyStrength,0); EXPECT_FLOAT_EQ(runtime.anisotropyRotation,0);
  EXPECT_FLOAT_EQ(runtime.diffuseTransmissionFactor,0);
  EXPECT_EQ(runtime.diffuseTransmissionColorFactor,glm::vec3(1));
}

TEST(GltfAnimation, ExtendedPbrColorComponentsUseWhiteDefaultsAndRestoreAcrossClips) {
  tinygltf::Model model; model.materials.resize(1);
  model.materials[0].extensions["KHR_materials_anisotropy"] = uvTransform();
  model.materials[0].extensions["KHR_materials_diffuse_transmission"] = uvTransform();
  const auto rest = model.materials[0];
  addPointer(model,"/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor/1",
      TINYGLTF_TYPE_SCALAR,{0,0});
  addPointer(model,"/materials/0/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",
      TINYGLTF_TYPE_SCALAR,{0,1});
  auto colorClip = model.animations[0]; model.animations.clear();
  addPointer(model,"/materials/0/extensions/KHR_materials_anisotropy/anisotropyRotation",TINYGLTF_TYPE_SCALAR,{0,2});
  model.animations.insert(model.animations.begin(),colorClip);
  AnimationPlayer player(model);
  player.selectClip(model,0); player.seek(model,1);
  container::material::Material runtime;
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_EQ(runtime.diffuseTransmissionColorFactor,glm::vec3(1,0,1));
  EXPECT_FLOAT_EQ(runtime.diffuseTransmissionFactor,.5f);
  player.selectClip(model,1); player.seek(model,1);
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_EQ(runtime.diffuseTransmissionColorFactor,glm::vec3(1));
  EXPECT_FLOAT_EQ(runtime.diffuseTransmissionFactor,0);
  EXPECT_FLOAT_EQ(runtime.anisotropyRotation,1);
  player.clearClip(model); expectMaterialDataEqual(model.materials[0],rest);
}

TEST(GltfAnimation, ExtendedPbrPointersValidateShapeParentsAndAtomicPublication) {
  const std::string prefix = "/materials/0/extensions/KHR_materials_diffuse_transmission/";
  tinygltf::Model model; model.materials.resize(1);
  addPointer(model,prefix+"diffuseTransmissionColorFactor",TINYGLTF_TYPE_VEC3,{0,0,0,1,1,1});
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  model.materials[0].extensions["KHR_materials_diffuse_transmission"] = uvTransform();
  EXPECT_NO_THROW(ParseAnimations(model));
  model.materials[0].extensions["KHR_materials_diffuse_transmission"].Get<tinygltf::Value::Object>()["diffuseTransmissionColorFactor"] =
      tinygltf::Value(tinygltf::Value::Array{tinygltf::Value(1.0),tinygltf::Value(1.0)});
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  model.materials[0].extensions["KHR_materials_diffuse_transmission"] = uvTransform();
  model.accessors[model.animations[0].samplers[0].output].type = TINYGLTF_TYPE_VEC2;
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  model.accessors[model.animations[0].samplers[0].output].type = TINYGLTF_TYPE_VEC3;
  addPointer(model,prefix+"diffuseTransmissionColorFactor/0",TINYGLTF_TYPE_SCALAR,{0,1});
  EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
  model.animations[0].channels.pop_back(); model.animations[0].samplers.pop_back();
  model.materials[0].extensions["KHR_materials_anisotropy"] = uvTransform();
  addPointer(model,"/materials/0/extensions/KHR_materials_anisotropy/anisotropyRotation",TINYGLTF_TYPE_SCALAR,{0,1});
  model.extensionsRequired = {"KHR_accessor_float64"};
  model.animations[0].samplers.back().output = doubleAccessor(model,TINYGLTF_TYPE_SCALAR,{0,1e100});
  const auto rest = model.materials[0];
  const auto library = ParseAnimations(model);
  EXPECT_THROW(ApplyAnimationPose(model,SampleAnimation(library.clips[0],2)),std::invalid_argument);
  expectMaterialDataEqual(model.materials[0],rest);
}

TEST(GltfAnimation, ExtendedPbrBoundsValidateKeysAndSampledCubicValuesBeforeMutation) {
  for (const auto* property : {"extensions/KHR_materials_anisotropy/anisotropyStrength",
                              "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",
                              "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor/2"}) {
    SCOPED_TRACE(property);
    tinygltf::Model model; model.materials.resize(1);
    model.materials[0].extensions["KHR_materials_anisotropy"] = uvTransform();
    model.materials[0].extensions["KHR_materials_diffuse_transmission"] = uvTransform();
    addPointer(model,"/materials/0/"+std::string(property),TINYGLTF_TYPE_SCALAR,{0,2});
    EXPECT_THROW(ParseAnimations(model),std::invalid_argument);
    model.animations[0].samplers[0].output = floatAccessor(model,TINYGLTF_TYPE_SCALAR,{
        0,.5f,4, -4,.5f,0});
    model.animations[0].samplers[0].interpolation = "CUBICSPLINE";
    const auto library = ParseAnimations(model);
    auto pose = SampleAnimation(library.clips[0],1);
    ASSERT_GT(pose.values[0].value[0],1);
    // A valid preceding channel must not be published if the later material
    // sample overshoots its valid range.
    pose.values.insert(pose.values.begin(),AnimatedValue{{AnimationTargetKind::Material,0,"alphaCutoff",1,{}},{.25}});
    const auto rest = model.materials[0];
    EXPECT_THROW(ApplyAnimationPose(model,pose),std::invalid_argument);
    expectMaterialDataEqual(model.materials[0],rest);
  }
}

TEST(GltfAnimationRuntime, PackedMetallicRoughnessTransformPreservesEachShaderChannel) {
  tinygltf::Material source;
  setTextureTransform(source,"pbrMetallicRoughness/metallicRoughnessTexture",uvTransform({{"rotation",tinygltf::Value(.75)}}));
  container::material::Material runtime;
  runtime.roughnessTextureTransform.channel = 1; runtime.metalnessTextureTransform.channel = 2;
  runtime.roughnessTextureTransform.texCoord = 1; runtime.metalnessTextureTransform.texCoord = 0;
  UpdateAnimatedMaterialProperties(source,runtime);
  EXPECT_FLOAT_EQ(runtime.metallicRoughnessTextureTransform.rotation,.75f);
  EXPECT_FLOAT_EQ(runtime.roughnessTextureTransform.rotation,.75f);
  EXPECT_FLOAT_EQ(runtime.metalnessTextureTransform.rotation,.75f);
  EXPECT_EQ(runtime.roughnessTextureTransform.channel,1u); EXPECT_EQ(runtime.metalnessTextureTransform.channel,2u);
  EXPECT_EQ(runtime.roughnessTextureTransform.texCoord,1u); EXPECT_EQ(runtime.metalnessTextureTransform.texCoord,0u);
}
TEST(GltfAnimationRuntime, ClearcoatNormalScaleAndLegacyFactorsReachShaderFieldsAndRestore) {
  tinygltf::Model model; model.materials.resize(1);
  setTextureTransform(model.materials[0],"extensions/KHR_materials_clearcoat/clearcoatNormalTexture",uvTransform());
  model.materials[0].extensions["KHR_materials_pbrSpecularGlossiness"] = uvTransform();
  const auto rest = model.materials[0];
  addPointer(model,"/materials/0/extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale",TINYGLTF_TYPE_SCALAR,{0,4});
  addPointer(model,"/materials/0/extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor",TINYGLTF_TYPE_VEC4,{.2f,.4f,.6f,.8f, .6f,.8f,1,1});
  addPointer(model,"/materials/0/extensions/KHR_materials_pbrSpecularGlossiness/specularFactor",TINYGLTF_TYPE_VEC3,{0,0,0, .08f,.08f,.08f});
  addPointer(model,"/materials/0/extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor",TINYGLTF_TYPE_SCALAR,{.25f,.75f});
  AnimationPlayer player(model); player.selectClip(model,0); player.seek(model,1);
  container::material::Material runtime; runtime.specularGlossinessWorkflow = true;
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_FLOAT_EQ(runtime.clearcoatNormalTextureScale,2); EXPECT_NEAR(runtime.metallicFactor,0,1e-6f);
  EXPECT_NEAR(runtime.baseColor.r,.4f,1e-6f); EXPECT_NEAR(runtime.baseColor.g,.6f,1e-6f);
  EXPECT_NEAR(runtime.baseColor.b,.8f,1e-6f); EXPECT_NEAR(runtime.baseColor.a,.9f,1e-6f);
  EXPECT_NEAR(runtime.specularFactor,1,1e-6f); EXPECT_EQ(runtime.specularColorFactor,glm::vec3(1));
  EXPECT_FLOAT_EQ(runtime.roughnessFactor,.5f); EXPECT_TRUE(runtime.specularGlossinessWorkflow);
  player.clearClip(model); expectMaterialDataEqual(model.materials[0],rest);
  UpdateAnimatedMaterialProperties(model.materials[0],runtime);
  EXPECT_FLOAT_EQ(runtime.clearcoatNormalTextureScale,1); EXPECT_FLOAT_EQ(runtime.roughnessFactor,1);
}

TEST(GltfAnimation, LinearTranslationClampsBeforeAndAfterKeys) {
  auto model = translationModel();
  const auto library = ParseAnimations(model);
  const auto& clip = library.clips[0];
  EXPECT_EQ(clip.name, "fixture"); EXPECT_DOUBLE_EQ(clip.duration, 3);
  EXPECT_EQ(onlyValue(SampleAnimation(clip, 0)), (std::vector<double>{2,4,6}));
  EXPECT_EQ(onlyValue(SampleAnimation(clip, 2)), (std::vector<double>{4,6,8}));
  EXPECT_EQ(onlyValue(SampleAnimation(clip, 99)), (std::vector<double>{6,8,10}));
  auto dirty = ApplyAnimationPose(model, SampleAnimation(clip, 2));
  EXPECT_EQ(dirty.nodes, (std::vector<size_t>{0}));
  EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{4,6,8}));
  EXPECT_TRUE(ApplyAnimationPose(model, SampleAnimation(clip, 2)).empty());
}
TEST(GltfAnimation, StepUsesLeftValueAndExactBoundaryUsesNewValue) {
  const auto library = ParseAnimations(translationModel("STEP"));
  EXPECT_EQ(onlyValue(SampleAnimation(library.clips[0], 2.999)), (std::vector<double>{2,4,6}));
  EXPECT_EQ(onlyValue(SampleAnimation(library.clips[0], 3)), (std::vector<double>{6,8,10}));
}
TEST(GltfAnimation, CubicTangentsAreDerivativesScaledByInterval) {
  auto model = translationModel("CUBICSPLINE");
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_VEC3,
    {0,0,0, 0,0,0, 4,0,0, // key 0, outgoing x derivative = 4/second
     0,0,0, 0,0,0, 0,0,0});
  const auto library = ParseAnimations(model);
  const auto& clip = library.clips[0];
  EXPECT_NEAR(onlyValue(SampleAnimation(clip, 2))[0], 1.0, 1e-12);
  const double epsilon = 1e-5;
  EXPECT_NEAR(onlyValue(SampleAnimation(clip, 1+epsilon))[0]/epsilon, 4.0, 1e-4);
}
TEST(GltfAnimation, LinearQuaternionTakesShortestArcAndPreservesAuthoredValues) {
  auto model = translationModel();
  auto& channel = model.animations[0].channels[0]; channel.target_path = "rotation";
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_VEC4,
    {0,0,0,1, 0,0,-1,0});
  const auto library = ParseAnimations(model);
  const auto& clip = library.clips[0];
  const auto pose = SampleAnimation(clip, 2);
  EXPECT_NEAR(onlyValue(pose)[2], -std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(onlyValue(pose)[3], std::sqrt(0.5), 1e-12);
  EXPECT_DOUBLE_EQ(clip.tracks[0].values[6], -1);
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_VEC4,
    {0,0,0,1, 0,0,0,-1});
  const auto opposite = ParseAnimations(model);
  EXPECT_NEAR(onlyValue(SampleAnimation(opposite.clips[0], 2))[3], 1, 1e-12);
  EXPECT_DOUBLE_EQ(opposite.clips[0].tracks[0].values[7], -1);
}
TEST(GltfAnimation, CubicQuaternionNormalizesOnlyResultNotTangents) {
  auto model = translationModel("CUBICSPLINE");
  model.animations[0].channels[0].target_path = "rotation";
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_VEC4,
    {0,0,0,0, 0,0,0,1, 0,0,4,0,
     0,0,0,0, 0,0,0,1, 0,0,0,0});
  const auto library = ParseAnimations(model);
  const auto value = onlyValue(SampleAnimation(library.clips[0], 2));
  EXPECT_NEAR(value[2], std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(value[3], std::sqrt(0.5), 1e-12);
  EXPECT_DOUBLE_EQ(library.clips[0].tracks[0].values[10], 4);
}
TEST(GltfAnimation, MorphWeightsArePackedPerFrameAndApplyToNode) {
  auto model = translationModel(); model.meshes.resize(1);
  model.meshes[0].primitives.resize(1); model.meshes[0].primitives[0].targets.resize(2);
  model.meshes[0].weights = {0.1,0.2}; model.nodes[0].mesh = 0;
  model.animations[0].channels[0].target_path = "weights";
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_SCALAR, {0,1, 1,0});
  const auto library = ParseAnimations(model);
  const auto dirty = ApplyAnimationPose(model, SampleAnimation(library.clips[0], 2));
  EXPECT_TRUE(dirty.nodes.empty()); EXPECT_EQ(dirty.weights, (std::vector<size_t>{0}));
  EXPECT_EQ(model.nodes[0].weights, (std::vector<double>{0.5,0.5}));
  RestoreAnimationRestPose(model, library); EXPECT_TRUE(model.nodes[0].weights.empty());
  EXPECT_EQ(model.meshes[0].weights, (std::vector<double>{0.1,0.2}));
}
TEST(GltfAnimation, CubicMorphWeightsHaveGroupedTangentsValuesAndDurationScale) {
  auto model = translationModel("CUBICSPLINE"); model.meshes.resize(1);
  model.meshes[0].primitives.resize(1); model.meshes[0].primitives[0].targets.resize(2);
  model.nodes[0].mesh = 0; model.animations[0].channels[0].target_path = "weights";
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_SCALAR,
    {0,0, 0,1, 4,0, 0,0, 0,1, 0,0});
  const auto library = ParseAnimations(model);
  EXPECT_EQ(onlyValue(SampleAnimation(library.clips[0], 2)), (std::vector<double>{1,1}));
}
TEST(GltfAnimation, SparseOutputOverlaysZeroBaseAndRejectsDuplicateIndices) {
  auto model = translationModel();
  const int output = model.animations[0].samplers[0].output;
  const int sparseValues = floatAccessor(model, TINYGLTF_TYPE_VEC3, {8,4,2});
  tinygltf::BufferView indices;
  indices.buffer = 0; indices.byteOffset = model.buffers[0].data.size(); indices.byteLength = 1;
  model.buffers[0].data.push_back(1); model.bufferViews.push_back(indices);
  auto& accessor = model.accessors[output]; accessor.bufferView = -1;
  accessor.sparse.isSparse = true; accessor.sparse.count = 1;
  accessor.sparse.indices.bufferView = static_cast<int>(model.bufferViews.size()-1);
  accessor.sparse.indices.byteOffset = 0;
  accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  accessor.sparse.values.bufferView = model.accessors[sparseValues].bufferView;
  accessor.sparse.values.byteOffset = 0;
  const auto library = ParseAnimations(model);
  EXPECT_EQ(onlyValue(SampleAnimation(library.clips[0], 2)), (std::vector<double>{4,2,1}));
  model.buffers[0].data.back() = 2;
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
}
TEST(GltfAnimation, DenseSparseOverridesAndStrictlyIncreasingIndices) {
  auto model = translationModel();
  const int sparseValues = floatAccessor(model, TINYGLTF_TYPE_VEC3, {8,4,2, 10,6,4});
  tinygltf::BufferView indices;
  indices.buffer = 0; indices.byteOffset = model.buffers[0].data.size(); indices.byteLength = 2;
  model.buffers[0].data.insert(model.buffers[0].data.end(), {0,1}); model.bufferViews.push_back(indices);
  auto& accessor = model.accessors[model.animations[0].samplers[0].output];
  accessor.sparse.isSparse = true; accessor.sparse.count = 2;
  accessor.sparse.indices.bufferView = static_cast<int>(model.bufferViews.size()-1);
  accessor.sparse.indices.byteOffset = 0;
  accessor.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  accessor.sparse.values.bufferView = model.accessors[sparseValues].bufferView;
  accessor.sparse.values.byteOffset = 0;
  const auto library = ParseAnimations(model);
  EXPECT_EQ(onlyValue(SampleAnimation(library.clips[0], 2)), (std::vector<double>{9,5,3}));
  model.buffers[0].data.back() = 0; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
}
TEST(GltfAnimation, NormalizedIntegerQuaternionIsDecodedBeforeInterpolation) {
  auto model = translationModel(); model.animations[0].channels[0].target_path = "rotation";
  auto& accessor = model.accessors[model.animations[0].samplers[0].output];
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_BYTE; accessor.normalized = true;
  accessor.type = TINYGLTF_TYPE_VEC4; accessor.count = 2;
  auto& view = model.bufferViews[accessor.bufferView]; view.byteLength = 8;
  auto& bytes = model.buffers[0].data;
  const std::vector<unsigned char> values{0,0,0,127, 0,0,127,0};
  std::copy(values.begin(), values.end(), bytes.begin()+view.byteOffset);
  const auto library = ParseAnimations(model);
  EXPECT_NEAR(onlyValue(SampleAnimation(library.clips[0], 2))[2], std::sqrt(0.5), 1e-12);
}
TEST(GltfAnimation, RejectsMalformedAccessorsAndTimelines) {
  auto model = translationModel();
  model.animations[0].samplers[0].input = 999; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.accessors[1].count = 3; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.bufferViews[1].byteLength = 20; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.bufferViews[1].byteStride = 8; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.bufferViews[1].byteStride = 14; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.accessors[1].byteOffset = std::numeric_limits<size_t>::max(); EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.animations[0].samplers[0].input = floatAccessor(model, TINYGLTF_TYPE_SCALAR, {1,1}); EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.animations[0].samplers[0].input = floatAccessor(model, TINYGLTF_TYPE_SCALAR, {-1,3}); EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.accessors[0].minValues.clear(); EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.buffers[0].data.resize(1); EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.nodes[0].matrix.resize(16); EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.accessors[1].bufferView = -2; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.animations[0].channels[0].target_node = -2; EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
}
TEST(GltfAnimation, FloatInputMinMaxUsesAccessorPrecision) {
  auto model = translationModel();
  const int input = floatAccessor(model, TINYGLTF_TYPE_SCALAR, {0.1f,0.3f});
  model.accessors[input].minValues = {0.1}; model.accessors[input].maxValues = {0.3};
  model.animations[0].samplers[0].input = input;
  EXPECT_NO_THROW(ParseAnimations(model));
}
TEST(GltfAnimation, InterleavedStrideReadsOnlyAuthoredElements) {
  auto model = translationModel();
  const int output = floatAccessor(model, TINYGLTF_TYPE_VEC4, {2,4,6,999, 6,8,10,999});
  auto& accessor = model.accessors[output]; accessor.type = TINYGLTF_TYPE_VEC3;
  model.bufferViews[accessor.bufferView].byteStride = 16;
  model.animations[0].samplers[0].output = output;
  const auto library = ParseAnimations(model);
  EXPECT_EQ(onlyValue(SampleAnimation(library.clips[0], 2)), (std::vector<double>{4,6,8}));
}
TEST(GltfAnimation, RejectsNonfiniteValuesAndZeroQuaternion) {
  auto model = translationModel();
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_VEC3,
    {0,0,0, std::numeric_limits<float>::infinity(),0,0});
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model.animations[0].channels[0].target_path = "rotation";
  model.animations[0].samplers[0].output = floatAccessor(model, TINYGLTF_TYPE_VEC4, {0,0,0,0, 0,0,0,1});
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  const auto library = ParseAnimations(translationModel());
  EXPECT_THROW(SampleAnimation(library.clips[0], std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}
TEST(GltfAnimation, PointerMaterialsLightsAndCamerasProduceTypedSourceUpdates) {
  tinygltf::Model model; model.materials.resize(2); model.lights.resize(2); model.cameras.resize(2);
  model.lights[1].type = "spot"; model.lights[1].range = 10;
  model.cameras[1].type = "perspective"; model.cameras[1].perspective.yfov = 0.8;
  addPointer(model, "/materials/1/pbrMetallicRoughness/baseColorFactor", TINYGLTF_TYPE_VEC4, {1,0,0,1, 0,0,1,0.5});
  addPointer(model, "/materials/1/pbrMetallicRoughness/roughnessFactor", TINYGLTF_TYPE_SCALAR, {0.2f,0.8f});
  addPointer(model, "/extensions/KHR_lights_punctual/lights/1/intensity", TINYGLTF_TYPE_SCALAR, {2,10});
  addPointer(model, "/extensions/KHR_lights_punctual/lights/1/color", TINYGLTF_TYPE_VEC3, {1,0,0, 0,1,0});
  addPointer(model, "/extensions/KHR_lights_punctual/lights/1/spot/outerConeAngle", TINYGLTF_TYPE_SCALAR, {0.2f,0.6f});
  addPointer(model, "/cameras/1/perspective/yfov", TINYGLTF_TYPE_SCALAR, {0.5f,1.5f});
  const auto library = ParseAnimations(model);
  const auto dirty = ApplyAnimationPose(model, SampleAnimation(library.clips[0], 1));
  EXPECT_EQ(dirty.materials, (std::vector<size_t>{1})); EXPECT_EQ(dirty.lights, (std::vector<size_t>{1}));
  EXPECT_EQ(dirty.cameras, (std::vector<size_t>{1})); EXPECT_TRUE(dirty.nodes.empty());
  EXPECT_EQ(model.materials[1].pbrMetallicRoughness.baseColorFactor, (std::vector<double>{0.5,0,0.5,0.75}));
  EXPECT_NEAR(model.materials[1].pbrMetallicRoughness.roughnessFactor, 0.5, 1e-7);
  EXPECT_DOUBLE_EQ(model.lights[1].intensity, 6); EXPECT_EQ(model.lights[1].color, (std::vector<double>{0.5,0.5,0}));
  EXPECT_NEAR(model.lights[1].spot.outerConeAngle, 0.4, 1e-7); EXPECT_DOUBLE_EQ(model.cameras[1].perspective.yfov, 1);
  RestoreAnimationRestPose(model, library);
  EXPECT_DOUBLE_EQ(model.lights[1].intensity, 1); EXPECT_EQ(model.materials[1].pbrMetallicRoughness.baseColorFactor, (std::vector<double>{1,1,1,1}));
  EXPECT_DOUBLE_EQ(model.cameras[1].perspective.yfov, 0.8);
}
TEST(GltfAnimation, PointerExtensionDefaultsAndArrayElementsPreserveOtherElements) {
  tinygltf::Model model; model.materials.resize(1);
  model.materials[0].extensions["KHR_materials_specular"] = tinygltf::Value(tinygltf::Value::Object{});
  addPointer(model, "/materials/0/extensions/KHR_materials_specular/specularColorFactor/1", TINYGLTF_TYPE_SCALAR, {0.2f,0.4f});
  const auto library = ParseAnimations(model);
  ApplyAnimationPose(model, SampleAnimation(library.clips[0], 1));
  const auto& array = model.materials[0].extensions.at("KHR_materials_specular").Get("specularColorFactor");
  EXPECT_DOUBLE_EQ(array.Get(0).GetNumberAsDouble(), 1);
  EXPECT_NEAR(array.Get(1).GetNumberAsDouble(), 0.3, 1e-7);
  EXPECT_DOUBLE_EQ(array.Get(2).GetNumberAsDouble(), 1);
  RestoreAnimationRestPose(model, library);
  EXPECT_FALSE(model.materials[0].extensions.at("KHR_materials_specular").Has("specularColorFactor"));
}
TEST(GltfAnimation, PointerNodeComponentsUseDefaultsAndCanonicalOverlapDetection) {
  tinygltf::Model model; model.nodes.resize(1);
  addPointer(model, "/nodes/0/scale/1", TINYGLTF_TYPE_SCALAR, {2,4});
  addPointer(model, "/nodes/0/scale/2", TINYGLTF_TYPE_SCALAR, {3,5});
  const auto library = ParseAnimations(model);
  ApplyAnimationPose(model, SampleAnimation(library.clips[0], 1));
  EXPECT_EQ(model.nodes[0].scale, (std::vector<double>{1,3,4}));
  addPointer(model, "/nodes/0/scale", TINYGLTF_TYPE_VEC3, {1,1,1, 2,2,2});
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model = translationModel(); model.animations[0].channels.push_back(model.animations[0].channels[0]);
  setPointer(model.animations[0].channels.back(), "/nodes/0/translation");
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
}
TEST(GltfAnimation, UnknownPointersAreDiagnosedAndMalformedKnownPointersRejected) {
  tinygltf::Model model; model.materials.resize(1); model.cameras.resize(1);
  addPointer(model, "/materials/0/name", TINYGLTF_TYPE_SCALAR, {0,1});
  const auto unknown = ParseAnimations(model);
  EXPECT_TRUE(unknown.clips[0].tracks.empty()); ASSERT_EQ(unknown.diagnostics.size(), 1);
  EXPECT_EQ(unknown.diagnostics[0].channel, 0);
  setPointer(model.animations[0].channels[0], "/materials/0/emissiveFactor/3");
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  setPointer(model.animations[0].channels[0], "/materials/01/alphaCutoff");
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  setPointer(model.animations[0].channels[0], "/materials/0/extensions/KHR_materials_ior/ior");
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  model.cameras[0].type = "perspective";
  setPointer(model.animations[0].channels[0], "/cameras/0/perspective/zfar");
  EXPECT_THROW(ParseAnimations(model), std::invalid_argument);
  setPointer(model.animations[0].channels[0], "/materials/0/pbrMetallicRoughness~1baseColorFactor");
  EXPECT_EQ(ParseAnimations(model).diagnostics.size(), 1);
}
TEST(GltfAnimation, PlayerSelectionRestoresPreviousClipAndSeekWorksWhenPaused) {
  auto model = translationModel();
  auto second = model.animations[0]; second.channels[0].target_path = "scale";
  model.animations.push_back(second);
  AnimationPlayer player(model); player.selectClip(model, 0);
  EXPECT_FALSE(player.playing()); EXPECT_DOUBLE_EQ(player.time(), 0);
  player.seek(model, 2); EXPECT_EQ(model.nodes[0].translation, (std::vector<double>{4,6,8}));
  player.selectClip(model, 1); EXPECT_TRUE(model.nodes[0].translation.empty());
  EXPECT_EQ(model.nodes[0].scale, (std::vector<double>{2,4,6}));
  player.clearClip(model); EXPECT_TRUE(model.nodes[0].scale.empty()); EXPECT_FALSE(player.selectedClip());
}
TEST(GltfAnimation, PlayerHandlesLoopPauseSpeedAndClampedPlayback) {
  auto model = translationModel(); AnimationPlayer player(model); player.selectClip(model,0);
  EXPECT_TRUE(player.advance(model,1).empty()); EXPECT_DOUBLE_EQ(player.time(),0);
  player.play(); player.setSpeed(2); player.advance(model,2); EXPECT_DOUBLE_EQ(player.time(),1);
  player.pause(); player.advance(model,1); EXPECT_DOUBLE_EQ(player.time(),1);
  player.play(); player.setSpeed(-1); player.advance(model,2); EXPECT_DOUBLE_EQ(player.time(),2);
  player.setLoop(false); player.advance(model,100); EXPECT_DOUBLE_EQ(player.time(),0); EXPECT_FALSE(player.playing());
  player.setSpeed(1); player.play(); player.advance(model,100); EXPECT_DOUBLE_EQ(player.time(),3); EXPECT_FALSE(player.playing());
  player.seek(model,1); EXPECT_DOUBLE_EQ(player.time(),1);
}
TEST(GltfAnimation, PlayerLargeFiniteWrapsAndInvalidInputsRemainFinite) {
  auto model = translationModel(); AnimationPlayer player(model); player.selectClip(model,0); player.play();
  player.setSpeed(std::numeric_limits<double>::max());
  player.advance(model,std::numeric_limits<double>::max());
  EXPECT_TRUE(std::isfinite(player.time())); EXPECT_GE(player.time(),0); EXPECT_LT(player.time(),3);
  player.setSpeed(-std::numeric_limits<double>::max());
  player.advance(model,std::numeric_limits<double>::max());
  EXPECT_TRUE(std::isfinite(player.time())); EXPECT_GE(player.time(),0); EXPECT_LT(player.time(),3);
  EXPECT_THROW(player.setSpeed(std::numeric_limits<double>::infinity()), std::invalid_argument);
  EXPECT_THROW(player.seek(model,std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  EXPECT_THROW(player.advance(model,-1), std::invalid_argument);
  EXPECT_THROW(player.advance(model,std::numeric_limits<double>::infinity()), std::invalid_argument);
}
TEST(GltfAnimationRuntime, SourceAssociationsUpdateGroupsAndRetainEditorChildren) {
  tinygltf::Model source; source.nodes.resize(2);
  source.nodes[0].translation = {1,2,3}; source.nodes[1].translation = {9,0,0};
  container::scene::SceneGraph graph;
  const auto editor = graph.createNode(glm::translate(glm::mat4(1),glm::vec3(50,0,0)),0);
  const auto group = graph.createNode(glm::mat4(1),0);
  graph.getNode(group)->sourceNodeIndex = 0;
  const auto primitive = graph.createNode(glm::translate(glm::mat4(1),glm::vec3(7,0,0)),0);
  graph.setParent(primitive,group);
  AnimationDirtySet dirty; dirty.nodes = {0};
  EXPECT_TRUE(ApplyAnimatedNodeTransforms(source,dirty,graph));
  EXPECT_FLOAT_EQ(graph.getNode(editor)->localTransform[3].x,50);
  EXPECT_FLOAT_EQ(graph.getNode(group)->localTransform[3].y,2);
  EXPECT_FLOAT_EQ(graph.getNode(primitive)->localTransform[3].x,7);
  EXPECT_FLOAT_EQ(graph.getNode(primitive)->worldTransform[3].x,8);
  EXPECT_FALSE(ApplyAnimatedNodeTransforms(source,dirty,graph));
  graph = container::scene::SceneGraph{};
  const auto replacement = graph.createNode(glm::mat4(1),0);
  graph.getNode(replacement)->sourceNodeIndex = 0;
  EXPECT_TRUE(ApplyAnimatedNodeTransforms(source,dirty,graph));
  EXPECT_FLOAT_EQ(graph.getNode(replacement)->localTransform[3].z,3);
}
TEST(GltfAnimationRuntime, NumericMaterialsPreserveUploadedTextureHandlesAndTransforms) {
  tinygltf::Material source;
  source.pbrMetallicRoughness.baseColorFactor = {0.1,0.2,0.3,0.4};
  source.pbrMetallicRoughness.roughnessFactor = 0.25;
  source.extensions["KHR_materials_transmission"] = tinygltf::Value(tinygltf::Value::Object{
    {"transmissionFactor",tinygltf::Value(0.75)}});
  source.extensions["KHR_materials_emissive_strength"] = tinygltf::Value(tinygltf::Value::Object{
    {"emissiveStrength",tinygltf::Value(2.5)}});
  container::material::Material runtime;
  runtime.baseColorTextureIndex = 42; runtime.normalTextureIndex = 19;
  runtime.baseColorTextureTransform.offset = {0.4f,0.6f};
  runtime.alphaMode = container::material::AlphaMode::Blend;
  UpdateAnimatedMaterialProperties(source,runtime);
  EXPECT_NEAR(runtime.baseColor.a,0.4f,1e-7f);
  EXPECT_FLOAT_EQ(runtime.roughnessFactor,0.25f);
  EXPECT_FLOAT_EQ(runtime.transmissionFactor,0.75f); EXPECT_FLOAT_EQ(runtime.emissiveStrength,2.5f);
  EXPECT_EQ(runtime.baseColorTextureIndex,42u); EXPECT_EQ(runtime.normalTextureIndex,19u);
  EXPECT_EQ(runtime.baseColorTextureTransform.offset,glm::vec2(0.4f,0.6f));
  EXPECT_EQ(runtime.alphaMode,container::material::AlphaMode::Blend);
}
tinygltf::Model cameraSource() {
  tinygltf::Model model; model.nodes.resize(1); model.nodes[0].camera = 0;
  model.cameras.resize(1); model.cameras[0].type = "perspective";
  auto& camera = model.cameras[0].perspective;
  camera.yfov = 3.14159265358979323846/2; camera.aspectRatio = 2;
  camera.znear = 1; camera.zfar = 9;
  return model;
}
float projectedDepth(const glm::mat4& matrix,float distance) {
  const glm::vec4 clip = matrix*glm::vec4(0,0,-distance,1);
  return clip.z/clip.w;
}
TEST(GltfAnimationRuntime, AuthoredCameraUsesRollWorldTransformAndFiniteReverseZ) {
  const auto source = cameraSource(); container::scene::SceneGraph graph;
  const auto transform = glm::translate(glm::mat4(1),glm::vec3(2,3,4))*glm::rotate(glm::mat4(1),glm::radians(90.0f),glm::vec3(0,0,1));
  const auto binding = graph.createNode(transform,0); graph.getNode(binding)->sourceNodeIndex = 0;
  graph.updateWorldTransforms(); const auto state = EvaluateAnimatedCamera(source,graph,0,1.25f);
  ASSERT_TRUE(state); EXPECT_FALSE(state->orthographic);
  EXPECT_EQ(state->data.cameraWorldPosition,glm::vec4(2,3,4,1));
  EXPECT_EQ(state->data.cameraForward,glm::vec4(0,0,-1,0));
  EXPECT_NEAR(projectedDepth(state->projection,1),1,1e-6f);
  EXPECT_NEAR(projectedDepth(state->projection,9),0,1e-6f);
  EXPECT_NEAR(state->projection[0][0],0.5f,1e-6f);
  EXPECT_EQ(state->shadowProjection,state->projection);
  EXPECT_GT(state->projection[1][1],0); // Y remains unflipped.
  const glm::vec3 up = glm::vec3(transform[1]);
  const auto clip = state->data.viewProj*glm::vec4(glm::vec3(2,3,4)+glm::vec3(0,0,-2)+up,1);
  EXPECT_NEAR(clip.y/clip.w,0.5f,1e-6f);
}
TEST(GltfAnimationRuntime, InfinitePerspectiveRetainsSourceAndFiniteCullSpan) {
  auto source = cameraSource(); source.cameras[0].perspective.zfar = 0;
  source.cameras[0].perspective.aspectRatio = 0;
  container::scene::SceneGraph graph; const auto binding = graph.createNode(glm::mat4(1),0);
  graph.getNode(binding)->sourceNodeIndex = 0; graph.updateWorldTransforms();
  const auto state = EvaluateAnimatedCamera(source,graph,0,2);
  ASSERT_TRUE(state); EXPECT_NEAR(projectedDepth(state->projection,1),1,1e-6f);
  EXPECT_NEAR(projectedDepth(state->projection,1e9f),1e-9f,1e-15f);
  EXPECT_TRUE(std::isfinite(state->farPlane)); EXPECT_FLOAT_EQ(state->farPlane,1e6f);
  EXPECT_DOUBLE_EQ(source.cameras[0].perspective.zfar,0);
  EXPECT_NEAR(projectedDepth(state->shadowProjection,state->nearPlane),1,1e-6f);
  EXPECT_NEAR(projectedDepth(state->shadowProjection,state->farPlane),0,1e-6f);
  EXPECT_NE(state->shadowProjection[2][2],state->projection[2][2]);
  const auto shadowInverse = glm::inverse(state->shadowProjection*state->view);
  for (float x : {-1.f,1.f}) for (float y : {-1.f,1.f}) for (float z : {0.f,1.f}) {
    const auto corner = shadowInverse*glm::vec4(x,y,z,1);
    EXPECT_GT(corner.w,0);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(std::isfinite(corner[i]/corner.w));
  }
}
TEST(GltfAnimationRuntime, AuthoredCascadeFrustumCornersFollowMovedAndRolledCamera) {
  const auto source = cameraSource(); container::scene::SceneGraph graph;
  const auto roll = glm::rotate(glm::mat4(1),glm::radians(90.0f),glm::vec3(0,0,1));
  const auto binding = graph.createNode(glm::translate(glm::mat4(1),glm::vec3(2,3,4))*roll,0);
  graph.getNode(binding)->sourceNodeIndex = 0; graph.updateWorldTransforms();
  auto camera = EvaluateAnimatedCamera(source,graph,0,1);
  ASSERT_TRUE(camera);
  auto reconstruct = [](const AnimatedCameraState& state,float depth) {
    const auto homogeneous = glm::inverse(state.shadowProjection*state.view)*glm::vec4(1,1,depth,1);
    return glm::vec3(homogeneous)/homogeneous.w;
  };
  const auto near = reconstruct(*camera,1), far = reconstruct(*camera,0);
  EXPECT_NEAR(near.x,1,1e-5f); EXPECT_NEAR(near.y,5,1e-5f); EXPECT_NEAR(near.z,3,1e-5f);
  EXPECT_NEAR(far.x,-7,1e-4f); EXPECT_NEAR(far.y,21,1e-4f); EXPECT_NEAR(far.z,-5,1e-4f);
  const auto middle = near+(far-near)*0.5f;
  EXPECT_NEAR(middle.x,-3,1e-4f); EXPECT_NEAR(middle.y,13,1e-4f); EXPECT_NEAR(middle.z,-1,1e-4f);
  graph.setLocalTransform(binding,glm::translate(glm::mat4(1),glm::vec3(102,23,11))*roll);
  graph.updateWorldTransforms(); camera = EvaluateAnimatedCamera(source,graph,0,1);
  ASSERT_TRUE(camera); const auto movedNear = reconstruct(*camera,1);
  EXPECT_NEAR(movedNear.x-near.x,100,1e-4f); EXPECT_NEAR(movedNear.y-near.y,20,1e-4f); EXPECT_NEAR(movedNear.z-near.z,7,1e-4f);
}
TEST(GltfAnimationRuntime, OrthographicCameraUsesAuthoredMagnitudesAndReverseZ) {
  auto source = cameraSource(); auto& camera = source.cameras[0]; camera.type = "orthographic";
  camera.orthographic.xmag = 2; camera.orthographic.ymag = 4;
  camera.orthographic.znear = 0; camera.orthographic.zfar = 10;
  container::scene::SceneGraph graph; const auto binding = graph.createNode(glm::mat4(1),0);
  graph.getNode(binding)->sourceNodeIndex = 0; graph.updateWorldTransforms();
  const auto state = EvaluateAnimatedCamera(source,graph,0,99);
  ASSERT_TRUE(state); EXPECT_TRUE(state->orthographic);
  EXPECT_NEAR(projectedDepth(state->projection,0),1,1e-6f);
  EXPECT_NEAR(projectedDepth(state->projection,10),0,1e-6f);
  EXPECT_FLOAT_EQ(state->projection[0][0],0.5f); EXPECT_FLOAT_EQ(state->projection[1][1],0.25f);
  camera.orthographic.ymag = 0; EXPECT_THROW(EvaluateAnimatedCamera(source,graph,0,1),std::invalid_argument);
  graph = container::scene::SceneGraph{}; EXPECT_FALSE(EvaluateAnimatedCamera(source,graph,0,1));
}

} // namespace
