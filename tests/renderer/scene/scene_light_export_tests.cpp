#include <Container/renderer/scene/SceneLightExport.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/GltfSceneInstancing.h>
#include <Container/geometry/GltfModelLoader.h>
#include <Container/geometry/GltfSceneExport.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfInteractivity.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::renderer;
using Value = tinygltf::Value;
using Object = Value::Object;

tinygltf::Model scene(size_t nodes) {
  tinygltf::Model model;
  model.asset.version = "2.0";
  model.nodes.resize(nodes);
  model.scenes.emplace_back();
  model.defaultScene = 0;
  for (size_t i = 0; i < nodes; ++i) model.scenes[0].nodes.push_back(static_cast<int>(i));
  return model;
}

EditableLightEntity entity(EditableLightType type, EditableLightSource source, uint32_t index) {
  EditableLightEntity light;
  light.type = type;
  light.source = source;
  light.id = {type, source, index};
  light.label = "Current light";
  light.position = {7, 8, 9};
  light.direction = {0, 0, -1};
  light.color = {0.2f, 0.4f, 0.6f};
  light.intensity = 17;
  light.range = 25;
  light.innerConeDegrees = 10;
  light.outerConeDegrees = 30;
  light.tangent = {1, 0, 0};
  light.bitangent = {0, 1, 0};
  return light;
}

void lightBehavior(tinygltf::Model& model, const std::string& property) {
  using Array = Value::Array;
  using namespace container::geometry::gltf;
  const Value start(Object{{"declaration",Value(0)},
      {"flows",Value(Object{{"out",Value(Object{{"node",Value(1)}})}})}});
  const Value write(Object{{"declaration",Value(1)},{"configuration",Value(Object{
      {"pointer",Value(Object{{"value",Value(Array{Value(
          std::string("/extensions/KHR_lights_punctual/lights/0/") + property)})}})},
      {"type",Value(Object{{"value",Value(Array{Value(0)})}})}})},
      {"values",Value(Object{{"value",Value(Object{{"type",Value(0)},{"value",Value(Array{Value(.75)})}})}})}});
  model.extensions[kGltfInteractivityExtension] = Value(Object{{"graphs",Value(Array{Value(Object{
      {"types",Value(Array{Value(Object{{"signature",Value(std::string("float"))}})})},
      {"declarations",Value(Array{Value(Object{{"op",Value(std::string("event/onStart"))}}),
          Value(Object{{"op",Value(std::string("pointer/set"))}})})},
      {"nodes",Value(Array{start,write})}})})}});
  model.extensionsUsed = {"KHR_lights_punctual",kGltfInteractivityExtension};
}

glm::mat4 matrix(const tinygltf::Node& node) {
  glm::mat4 result(1);
  if (node.matrix.size() == 16)
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) result[c][r] = float(node.matrix[c * 4 + r]);
  else {
    if (node.translation.size() == 3)
      result = glm::translate(result, glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
    if (node.rotation.size() == 4)
      result *= glm::mat4_cast(glm::quat(float(node.rotation[3]), float(node.rotation[0]),
                                        float(node.rotation[1]), float(node.rotation[2])));
    if (node.scale.size() == 3)
      result = glm::scale(result, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
  }
  return result;
}

int firstDirectional(const tinygltf::Model& model) {
  const auto& roots = model.scenes[model.defaultScene].nodes;
  std::vector<int> pending(roots.rbegin(), roots.rend());
  while (!pending.empty()) {
    const auto index = pending.back();
    pending.pop_back();
    const auto& node = model.nodes[index];
    if (node.light >= 0 && model.lights[node.light].type == "directional")
      return node.light;
    pending.insert(pending.end(), node.children.rbegin(), node.children.rend());
  }
  return -1;
}

int floatAccessor(tinygltf::Model& model, int type, std::vector<float> values) {
  if (model.buffers.empty()) model.buffers.emplace_back();
  auto& bytes = model.buffers[0].data;
  while (bytes.size()%4) bytes.push_back(0);
  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = bytes.size();
  for (float value : values) {
    const auto bits = std::bit_cast<uint32_t>(value);
    for (unsigned int byte=0;byte<4;++byte) bytes.push_back(static_cast<unsigned char>(bits>>(byte*8)));
  }
  view.byteLength = bytes.size()-view.byteOffset;
  tinygltf::Accessor accessor;
  accessor.bufferView = static_cast<int>(model.bufferViews.size());
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.type = type;
  accessor.count = values.size()/(type==TINYGLTF_TYPE_VEC3?3:type==TINYGLTF_TYPE_VEC4?4:1);
  if (type==TINYGLTF_TYPE_SCALAR) {
    accessor.minValues = {*std::min_element(values.begin(),values.end())};
    accessor.maxValues = {*std::max_element(values.begin(),values.end())};
  }
  model.bufferViews.push_back(view);
  model.accessors.push_back(accessor);
  return static_cast<int>(model.accessors.size()-1);
}

tinygltf::AnimationChannel lightChannel(int sampler, const char* property) {
  tinygltf::AnimationChannel channel;
  channel.sampler = sampler;
  channel.target_path = "pointer";
  channel.target_extensions["KHR_animation_pointer"] = Value(Object{{"pointer",
      Value(std::string("/extensions/KHR_lights_punctual/lights/0/")+property)}});
  return channel;
}
}  // namespace

TEST(SceneLightExport, MapsOriginalMixedLightOrderAfterHierarchyReordering) {
  auto source = scene(5);
  for (const auto* type : {"spot", "point", "directional", "directional", "point"}) {
    tinygltf::Light definition;
    definition.type = type;
    definition.intensity = 2;
    source.lights.push_back(definition);
  }
  for (int i = 0; i < 5; ++i) source.nodes[i].light = i;
  source.lights[0].name = "Original spot name";
  source.lights[0].extras = Value(Object{{"retain", Value(true)}});
  auto exported = source;
  exported.scenes[0].nodes = {4, 3, 2, 1, 0};
  const std::vector<int32_t> identities{0, 1, 2, 3, 4};
  std::array lights{
      entity(EditableLightType::Spot, EditableLightSource::Imported, 0),
      entity(EditableLightType::Point, EditableLightSource::Imported, 1),
      entity(EditableLightType::Directional, EditableLightSource::Imported, 0)};
  exportEditableSceneLights(exported, lights, &source, identities);
  ASSERT_EQ(exported.nodes.size(), 8u);
  EXPECT_EQ(exported.nodes[0].children, std::vector<int>{5});
  EXPECT_EQ(exported.nodes[1].children, std::vector<int>{6});
  EXPECT_EQ(exported.nodes[2].children, std::vector<int>{7});
  EXPECT_EQ(exported.scenes[0].nodes.front(), 2);
  EXPECT_EQ(exported.nodes[3].light, 3);
  EXPECT_EQ(exported.nodes[4].light, 4);
  EXPECT_DOUBLE_EQ(exported.lights[3].intensity, 2);
  EXPECT_DOUBLE_EQ(exported.lights[4].intensity, 2);
  EXPECT_EQ(exported.lights[5].name, "Original spot name");
  EXPECT_TRUE(exported.lights[5].extras.Get("retain").Get<bool>());
  EXPECT_EQ(exported.lights[5].type, "spot");
  EXPECT_EQ(exported.lights[6].type, "point");
  EXPECT_EQ(exported.lights[7].type, "directional");
  EXPECT_DOUBLE_EQ(exported.lights[5].range, 25);
  EXPECT_DOUBLE_EQ(exported.lights[6].range, 25);
  EXPECT_DOUBLE_EQ(exported.lights[7].range, 0);
  EXPECT_NEAR(exported.lights[5].spot.outerConeAngle, glm::radians(30.0), 1e-7);
}

TEST(SceneLightExport, MovesLightWithoutMovingMeshOnSharedSourceNode) {
  auto source = scene(1);
  source.lights.emplace_back();
  source.lights[0].type = "point";
  source.nodes[0].light = 0;
  source.nodes[0].mesh = 3;
  source.nodes[0].translation = {2, 3, 4};
  auto exported = source;
  const auto original = exported.nodes[0];
  const std::array lights{entity(EditableLightType::Point, EditableLightSource::Imported, 0)};
  const std::vector<int32_t> identities{0};
  exportEditableSceneLights(exported, lights, &source, identities);
  EXPECT_EQ(exported.nodes[0].mesh, original.mesh);
  EXPECT_EQ(exported.nodes[0].translation, original.translation);
  EXPECT_EQ(exported.nodes[0].light, -1);
  ASSERT_EQ(exported.nodes[0].children.size(), 1u);
  const auto world = matrix(exported.nodes[0]) * matrix(exported.nodes[1]);
  EXPECT_NEAR(world[3].x, lights[0].position.x, 1e-6);
  EXPECT_NEAR(world[3].y, lights[0].position.y, 1e-6);
  EXPECT_NEAR(world[3].z, lights[0].position.z, 1e-6);
}

TEST(SceneLightExport, ShearedImportedCarrierRemainsExactlyAffineThroughBothSaveFormats) {
  // These nonuniform, sheared ancestors reproduce an exported camera/light
  // hierarchy whose old general float inverse produced homogeneous w=1-2^-24.
  auto source=scene(3);
  source.scenes[0].nodes={0};
  source.nodes[0].children={1};
  source.nodes[1].children={2};
  source.nodes[0].matrix={
      .9969385862350464,.4648800790309906,0,0,
      -.19909487664699554,.9002006649971008,0,0,
      .002442850498482585,.08940935134887695,1.2000000476837158,0,
      .30000001192092896,.20000000298023224,6,1};
  source.nodes[1].matrix={
      1.2278543710708618,-.32231810688972473,.03656519576907158,0,
      .14704687893390656,.7368596792221069,.044321492314338684,0,
      -.022283820435404778,-.09804901480674744,1.1030513048171997,0,
      .14163751900196075,-.2953169047832489,-1.2417634032146907e-8,1};
  source.nodes[2].translation={.25,.15000000596046448,-1};
  source.nodes[2].name="Retained light owner";
  source.nodes[2].extras=Value(Object{{"nodeIdentity",Value(1012)}});
  source.nodes[2].light=0;
  source.lights.emplace_back();
  source.lights[0].type="point";
  source.lights[0].name="Retained point definition";
  source.lights[0].extras=Value(Object{{"annotation",Value(std::string("retain"))}});
  const auto parentWorld=matrix(source.nodes[0])*matrix(source.nodes[1])*matrix(source.nodes[2]);
  const auto current=entity(EditableLightType::Point,EditableLightSource::Imported,0);
  const auto expectedWorld=glm::translate(glm::mat4(1),current.position);
  const auto oldLocal=glm::inverse(parentWorld)*expectedWorld;
  ASSERT_NE(oldLocal[3][3],1.0f); // The regression must exercise the old failure.

  auto exported=source;
  const std::vector<int32_t> identities{0,1,2};
  exportEditableSceneLights(exported,std::array{current},&source,identities);
  ASSERT_EQ(exported.nodes.size(),4u);
  ASSERT_EQ(exported.lights.size(),2u);
  const auto check=[&](const tinygltf::Model& model) {
    ASSERT_EQ(model.nodes.size(),4u);
    ASSERT_EQ(model.nodes[3].matrix.size(),16u);
    EXPECT_DOUBLE_EQ(model.nodes[3].matrix[3],0);
    EXPECT_DOUBLE_EQ(model.nodes[3].matrix[7],0);
    EXPECT_DOUBLE_EQ(model.nodes[3].matrix[11],0);
    EXPECT_DOUBLE_EQ(model.nodes[3].matrix[15],1);
    EXPECT_EQ(model.nodes[0],source.nodes[0]);
    EXPECT_EQ(model.nodes[1],source.nodes[1]);
    EXPECT_EQ(model.nodes[2].translation,source.nodes[2].translation);
    EXPECT_EQ(model.nodes[2].name,source.nodes[2].name);
    EXPECT_EQ(model.nodes[2].extras,source.nodes[2].extras);
    EXPECT_EQ(model.nodes[2].light,-1);
    EXPECT_EQ(model.nodes[2].children,(std::vector<int>{3}));
    EXPECT_EQ(model.nodes[3].light,1);
    EXPECT_EQ(model.lights[0],source.lights[0]);
    EXPECT_EQ(model.lights[1].name,source.lights[0].name);
    EXPECT_EQ(model.lights[1].extras,source.lights[0].extras);
    const auto actual=parentWorld*matrix(model.nodes[3]);
    for (int c=0;c<4;++c) for(int r=0;r<4;++r)
      EXPECT_NEAR(actual[c][r],expectedWorld[c][r],2e-6);
    EXPECT_NO_THROW((void)container::scene::BuildGltfSceneHierarchy(model,{},{}));
  };
  check(exported);
  auto oldExport=exported;
  oldExport.nodes[3].matrix[15]=oldLocal[3][3];
  EXPECT_THROW((void)container::scene::BuildGltfSceneHierarchy(oldExport,{},{}),std::invalid_argument);

  const auto stamp=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  for (const auto* extension : {".gltf",".glb"}) {
    SCOPED_TRACE(extension);
    const auto path=std::filesystem::temp_directory_path()/("container-affine-light-"+stamp+extension);
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {std::error_code ignored;std::filesystem::remove(path,ignored);}
    } cleanup{path};
    container::geometry::gltf::writeExportModel(exported,path);
    check(container::geometry::gltf::LoadSourceModel(path.string()));
  }
}

TEST(SceneLightExport, SmallInvertibleParentUsesRepresentableAffineInverse) {
  auto source=scene(1);
  source.nodes[0].scale={1e-20,2e-20,3e-20};
  source.nodes[0].light=0;
  source.lights.emplace_back();source.lights[0].type="point";
  auto current=entity(EditableLightType::Point,EditableLightSource::Imported,0);
  current.position={0,0,0};
  const auto parentWorld=matrix(source.nodes[0]);
  const auto oldInverse=glm::inverse(parentWorld);
  bool oldFinite=true;
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) oldFinite&=std::isfinite(oldInverse[c][r]);
  ASSERT_FALSE(oldFinite);
  auto exported=source;
  exportEditableSceneLights(exported,std::array{current},&source,std::array<int32_t,1>{0});
  ASSERT_EQ(exported.nodes.size(),2u);
  const auto world=parentWorld*matrix(exported.nodes[1]);
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) EXPECT_NEAR(world[c][r],c==r?1:0,1e-6);
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneHierarchy(exported,{},{}));
}

TEST(SceneLightExport, InvalidAffineParentIsRejectedWithoutChangingSnapshot) {
  for (const int invalid : {0,1,2,3,4,5}) {
    SCOPED_TRACE(invalid);
    auto model=scene(1);
    model.nodes[0].light=0;
    model.lights.emplace_back();model.lights[0].type="point";
    model.nodes[0].matrix={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    if(invalid==0) model.nodes[0].matrix[15]=std::nextafter(1.0f,0.0f);
    if(invalid==1) model.nodes[0].matrix[3]=.125;
    if(invalid==2) model.nodes[0].matrix[0]=0;
    if(invalid==3) model.nodes[0].matrix[0]=std::numeric_limits<float>::denorm_min();
    if(invalid==4) model.nodes[0].matrix[0]=std::numeric_limits<float>::infinity();
    if(invalid==5) model.nodes[0].matrix[0]=std::bit_cast<double>(uint64_t{0x7ff8000000001234});
    const auto before=model;
    EXPECT_THROW(exportEditableSceneLights(model,std::array{
        entity(EditableLightType::Point,EditableLightSource::Imported,0)}),std::runtime_error);
    ASSERT_EQ(model.nodes.size(),before.nodes.size());
    ASSERT_EQ(model.nodes[0].matrix.size(),before.nodes[0].matrix.size());
    for(size_t i=0;i<before.nodes[0].matrix.size();++i)
      EXPECT_EQ(std::bit_cast<uint64_t>(model.nodes[0].matrix[i]),
                std::bit_cast<uint64_t>(before.nodes[0].matrix[i]));
    // TinyGLTF's epsilon-based equality rejects even an unchanged Inf/NaN.
    // Prove its exact payload above before normalizing only that comparison cell.
    auto actualComparison=model;
    auto beforeComparison=before;
    if(invalid>=4) actualComparison.nodes[0].matrix[0]=beforeComparison.nodes[0].matrix[0]=0;
    EXPECT_EQ(actualComparison,beforeComparison);
  }
}

TEST(SceneLightExport, FiniteLocalCannotPublishAnUnrepresentableFloatWorld) {
  for(const bool overflow : {true,false}) {
    SCOPED_TRACE(overflow);
    auto source=scene(1);
    source.nodes[0].light=0;
    source.lights.emplace_back();source.lights[0].type="point";
    source.nodes[0].matrix=overflow ? std::vector<double>{
        1e38,1e-38,0,0,1e38,2e-38,0,0,0,0,1,0,0,0,0,1} : std::vector<double>{
        1e6,0,1,0,0,1,0,0,1000000.4375,0,2,0,0,0,0,1};
    const auto parent=matrix(source.nodes[0]);
    const auto inverse=glm::inverse(glm::dmat3(parent));
    glm::mat4 previousLocal(1);
    for(int c=0;c<3;++c) for(int r=0;r<3;++r) {
      previousLocal[c][r]=float(inverse[c][r]);
      ASSERT_TRUE(std::isfinite(previousLocal[c][r]));
    }
    const auto actual=parent*previousLocal;
    bool actualFinite=true;
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) actualFinite&=std::isfinite(actual[c][r]);
    if(overflow) EXPECT_FALSE(actualFinite);
    else {
      ASSERT_TRUE(actualFinite);
      EXPECT_GT(std::abs(actual[2][0]),1e-3f); // Finite cancellation changes the light basis.
    }
    const auto before=source;
    auto current=entity(EditableLightType::Point,EditableLightSource::Imported,0);
    current.position={0,0,0};
    EXPECT_THROW(exportEditableSceneLights(source,std::array{current}),std::runtime_error);
    EXPECT_EQ(source,before);
  }
  // A small world-space edit beneath an ordinary translated parent still has
  // normal float cancellation. Its position error fits the publication budget.
  auto source=scene(1);
  source.nodes[0].light=0;
  source.nodes[0].translation={1000,0,0};
  source.lights.emplace_back();source.lights[0].type="point";
  auto current=entity(EditableLightType::Point,EditableLightSource::Imported,0);
  current.position={.1f,0,0};
  const auto parent=matrix(source.nodes[0]);
  exportEditableSceneLights(source,std::array{current});
  ASSERT_EQ(source.nodes.size(),2u);
  const auto world=parent*matrix(source.nodes[1]);
  EXPECT_GT(std::abs(world[3].x-current.position.x),64*std::numeric_limits<float>::epsilon());
  EXPECT_NEAR(world[3].x,current.position.x,1e-4);
  EXPECT_EQ(world[3].y,0);
  EXPECT_EQ(world[3].z,0);
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneHierarchy(source,{},{}));
}

TEST(SceneLightExport, ImportedSunRetainsAnimatedAncestorsAndActiveTraversalPriority) {
  auto source = scene(3);
  source.scenes[0].nodes = {0, 2};
  source.nodes[0].children = {1};
  source.nodes[1].light = 0;
  source.nodes[2].light = 1;
  source.lights.resize(2);
  source.lights[0].type = source.lights[1].type = "directional";
  auto exported = source;
  exported.scenes[0].nodes = {2, 0};
  const std::array lights{entity(EditableLightType::Directional, EditableLightSource::Imported, 0)};
  const std::vector<int32_t> identities{0, 1, 2};
  exportEditableSceneLights(exported, lights, &source, identities);
  EXPECT_EQ(firstDirectional(exported), 2);
  ASSERT_EQ(exported.nodes[1].children, std::vector<int>{3});
  // A later sampled ancestor rotation must move the sun instead of leaving a
  // frozen world-space light behind. Its independent editor correction stays.
  const auto before = matrix(exported.nodes[0]) * matrix(exported.nodes[1]) * matrix(exported.nodes[3]);
  const auto q = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
  exported.nodes[0].rotation = {q.x, q.y, q.z, q.w};
  const auto after = matrix(exported.nodes[0]) * matrix(exported.nodes[1]) * matrix(exported.nodes[3]);
  const auto expected = glm::mat4_cast(q) * before;
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) EXPECT_NEAR(after[c][r], expected[c][r], 1e-6);
  EXPECT_GT(glm::distance(glm::vec3(after[2]), glm::vec3(before[2])), 1.0f);
}

TEST(SceneLightExport, SharedImportedLightPointerAnimatesEveryEditedBinding) {
  auto source = scene(2);
  source.lights.emplace_back();
  source.lights[0].type = "point";
  source.nodes[0].light = source.nodes[1].light = 0;
  tinygltf::AnimationChannel channel;
  channel.sampler = 3;
  channel.target_path = "pointer";
  channel.target_extensions["KHR_animation_pointer"] = Value(Object{
      {"pointer", Value(std::string("/extensions/KHR_lights_punctual/lights/0/intensity"))},
      {"retained", Value(true)}});
  source.animations.emplace_back();
  source.animations[0].channels.push_back(channel);
  auto exported = source;
  const std::array lights{entity(EditableLightType::Point, EditableLightSource::Imported, 0),
                          entity(EditableLightType::Point, EditableLightSource::Imported, 1)};
  const std::vector<int32_t> identities{0, 1};
  exportEditableSceneLights(exported, lights, &source, identities);
  ASSERT_EQ(exported.animations[0].channels.size(), 3u);
  for(int i=0;i<3;++i) {
    const auto& actual = exported.animations[0].channels[i];
    EXPECT_EQ(actual.sampler, 3);
    EXPECT_EQ(actual.target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>(),
        "/extensions/KHR_lights_punctual/lights/" + std::to_string(i) + "/intensity");
    EXPECT_TRUE(actual.target_extensions.at("KHR_animation_pointer").Get("retained").Get<bool>());
  }
  EXPECT_EQ(source.animations[0].channels.size(), 1u);
}

TEST(SceneLightExport, SampledSharedChannelsPreserveRangeAndRadianceUnits) {
  using container::geometry::gltf::AnimationPlayer;
  auto source = scene(2);
  source.lights.emplace_back();
  source.lights[0].type = "point";
  source.lights[0].range = 1;
  source.nodes[0].light = source.nodes[1].light = 0;
  const int times = floatAccessor(source,TINYGLTF_TYPE_SCALAR,{0,2});
  const int scalarValues = floatAccessor(source,TINYGLTF_TYPE_SCALAR,{1,3});
  const int colors = floatAccessor(source,TINYGLTF_TYPE_VEC3,{1,.2f,.4f, .5f,.6f,1});
  source.animations.emplace_back();
  auto& animation = source.animations[0];
  tinygltf::AnimationSampler scalar;
  scalar.input=times; scalar.output=scalarValues;
  tinygltf::AnimationSampler color=scalar; color.output=colors;
  animation.samplers={scalar,color};
  animation.channels={lightChannel(0,"range"),lightChannel(0,"intensity"),lightChannel(1,"color")};
  auto exported=source;
  auto a=entity(EditableLightType::Point,EditableLightSource::Imported,0);
  auto b=entity(EditableLightType::Point,EditableLightSource::Imported,1);
  a.color={2,1,1}; b.color={4,1,1};
  exportEditableSceneLights(exported,std::array{a,b},&source,std::array<int32_t,2>{0,1},2);
  const auto rest=exported.lights;
  EXPECT_EQ(exported.animations[0].samplers[0],source.animations[0].samplers[0]);
  EXPECT_EQ(exported.animations[0].samplers[1],source.animations[0].samplers[1]);
  for(size_t channel=3;channel<exported.animations[0].channels.size();++channel) {
    const auto sampler=exported.animations[0].channels[channel].sampler;
    EXPECT_GE(sampler,2);
    EXPECT_EQ(exported.accessors[exported.animations[0].samplers[sampler].output].componentType,
              TINYGLTF_COMPONENT_TYPE_FLOAT);
  }
  AnimationPlayer player(exported);
  player.selectClip(exported,0); player.seek(exported,1);
  EXPECT_DOUBLE_EQ(exported.lights[0].range,2);
  EXPECT_DOUBLE_EQ(exported.lights[0].intensity,2);
  for(size_t clone=1;clone<3;++clone) {
    const double factor=clone==1?2:4;
    EXPECT_DOUBLE_EQ(exported.lights[clone].range,4);
    EXPECT_DOUBLE_EQ(exported.lights[clone].intensity,2*factor);
    for(size_t c=0;c<3;++c)
      EXPECT_NEAR(exported.lights[clone].color[c]*exported.lights[clone].intensity,
                  exported.lights[0].color[c]*exported.lights[0].intensity,1e-6);
  }
  player.clearClip(exported);
  EXPECT_EQ(exported.lights,rest);
  EXPECT_EQ(source.animations[0].samplers.size(),2u);
  EXPECT_DOUBLE_EQ(source.lights[0].range,1);
}

TEST(SceneLightExport, CubicRangeScaleIncludesDerivativesWithoutChangingSharedSampler) {
  using container::geometry::gltf::AnimationPlayer;
  auto source=scene(2);
  source.lights.emplace_back(); source.lights[0].type="point"; source.lights[0].range=1;
  source.nodes[0].light=source.nodes[1].light=0;
  const int times=floatAccessor(source,TINYGLTF_TYPE_SCALAR,{0,2});
  const int values=floatAccessor(source,TINYGLTF_TYPE_SCALAR,{0,1,2, 0,3,0});
  source.animations.emplace_back();
  tinygltf::AnimationSampler sampler;
  sampler.input=times; sampler.output=values; sampler.interpolation="CUBICSPLINE";
  source.animations[0].samplers={sampler};
  source.animations[0].channels={lightChannel(0,"range"),lightChannel(0,"intensity")};
  auto exported=source;
  const std::array lights{entity(EditableLightType::Point,EditableLightSource::Imported,0),
                          entity(EditableLightType::Point,EditableLightSource::Imported,1)};
  exportEditableSceneLights(exported,lights,&source,std::array<int32_t,2>{0,1},2);
  EXPECT_EQ(exported.animations[0].samplers[0],sampler);
  AnimationPlayer player(exported);
  player.selectClip(exported,0); player.seek(exported,1);
  EXPECT_DOUBLE_EQ(exported.lights[0].range,2.5);
  EXPECT_DOUBLE_EQ(exported.lights[0].intensity,2.5);
  EXPECT_DOUBLE_EQ(exported.lights[1].range,5);
  EXPECT_DOUBLE_EQ(exported.lights[2].range,5);
  EXPECT_DOUBLE_EQ(exported.lights[1].intensity,2.5);
  EXPECT_DOUBLE_EQ(exported.lights[2].intensity,2.5);
}

TEST(SceneLightExport, ActiveSunPrecedesAnInactiveAnimatedAncestorBinding) {
  using container::geometry::gltf::AnimationPlayer;
  auto source=scene(2);
  source.lights.resize(2);
  source.lights[0].type=source.lights[1].type="directional";
  source.lights[1].name="Preserved ancestor sun";
  source.nodes[0].light=0; source.nodes[1].light=1;
  const int times=floatAccessor(source,TINYGLTF_TYPE_SCALAR,{0,2});
  const auto q=glm::angleAxis(glm::radians(90.f),glm::vec3(0,1,0));
  const int rotations=floatAccessor(source,TINYGLTF_TYPE_VEC4,{0,0,0,1,q.x,q.y,q.z,q.w});
  source.animations.emplace_back();
  tinygltf::AnimationSampler sampler; sampler.input=times; sampler.output=rotations;
  source.animations[0].samplers={sampler};
  tinygltf::AnimationChannel channel; channel.sampler=0; channel.target_node=1; channel.target_path="rotation";
  source.animations[0].channels={channel};
  auto exported=source;
  // Reparenting the active sun beneath a source sun must not switch the active
  // binding just because pre-order traversal observes the new ancestor first.
  exported.scenes[0].nodes={1}; exported.nodes[1].children={0};
  const auto current=entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  exportEditableSceneLights(exported,std::array{current},&source,std::array<int32_t,2>{0,1});
  ASSERT_EQ(exported.nodes.size(),4u);
  EXPECT_EQ(firstDirectional(exported),2);
  EXPECT_EQ(exported.nodes[1].light,-1);
  EXPECT_EQ(exported.nodes[3].light,1);
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{0,3}));
  EXPECT_EQ(exported.lights[1],source.lights[1]);
  AnimationPlayer player(exported); player.selectClip(exported,0); player.seek(exported,2);
  const auto parent=matrix(exported.nodes[1]);
  const auto active=parent*matrix(exported.nodes[0])*matrix(exported.nodes[2]);
  const auto preserved=parent*matrix(exported.nodes[3]);
  const auto expected=glm::mat4_cast(q);
  for(int c=0;c<3;++c) for(int r=0;r<3;++r) {
    EXPECT_NEAR(active[c][r],expected[c][r],1e-6);
    EXPECT_NEAR(preserved[c][r],expected[c][r],1e-6);
  }
  EXPECT_EQ(firstDirectional(exported),2);
}

TEST(SceneLightExport, ManualSunOverrideDoesNotInheritImportedLightPointer) {
  auto source = scene(1);
  source.nodes[0].light = 0;
  source.lights.emplace_back();
  source.lights[0].type = "directional";
  tinygltf::AnimationChannel channel;
  channel.target_path = "pointer";
  channel.target_extensions["KHR_animation_pointer"] = Value(Object{
      {"pointer", Value(std::string("/extensions/KHR_lights_punctual/lights/0/color/1"))}});
  source.animations.emplace_back();
  source.animations[0].channels.push_back(channel);
  auto exported = source;
  const std::array lights{entity(EditableLightType::Directional, EditableLightSource::Manual, 0)};
  const std::vector<int32_t> identities{0};
  exportEditableSceneLights(exported, lights, &source, identities);
  EXPECT_EQ(exported.animations, source.animations);
  EXPECT_EQ(firstDirectional(exported), 1);
}

TEST(SceneLightExport, ManualSunReplacesActiveImportedBindingAndPreservesAdditionalSourceSuns) {
  auto source = scene(2);
  for (int i = 0; i < 2; ++i) {
    tinygltf::Light light;
    light.type = "directional";
    light.name = i == 0 ? "Original active sun" : "Untouched additional sun";
    light.intensity = i == 0 ? 3 : 9;
    source.lights.push_back(light);
    source.nodes[i].light = i;
  }
  source.nodes[0].name = "Source sun and object transform";
  source.nodes[0].translation = {2, 3, 4};
  auto exported = source;
  // A sibling reorder must not make the preserved additional sun active when
  // the current manually overridden directional binding is reloaded.
  exported.scenes[0].nodes = {1, 0};
  auto current = entity(EditableLightType::Directional, EditableLightSource::Manual, 0);
  current.label = "Manual Directional";
  current.direction = glm::normalize(glm::vec3(1, -2, -3));
  const std::array lights{current};
  const std::vector<int32_t> identities{0, 1};
  exportEditableSceneLights(exported, lights, &source, identities);
  ASSERT_EQ(exported.nodes.size(), 3u);
  ASSERT_EQ(exported.lights.size(), 3u);
  EXPECT_EQ(exported.scenes[0].nodes, (std::vector<int>{2, 1, 0}));
  EXPECT_EQ(exported.nodes[0].light, -1);
  EXPECT_EQ(exported.nodes[0].translation, source.nodes[0].translation);
  EXPECT_EQ(exported.nodes[0].name, source.nodes[0].name);
  EXPECT_EQ(exported.nodes[1], source.nodes[1]);
  EXPECT_EQ(exported.lights[0], source.lights[0]);
  EXPECT_EQ(exported.lights[1], source.lights[1]);
  EXPECT_EQ(firstDirectional(exported), 2);
  EXPECT_EQ(exported.lights[2].name, "Manual Directional");
  EXPECT_DOUBLE_EQ(exported.lights[2].intensity, current.intensity);
  const auto direction = -glm::vec3(matrix(exported.nodes[2])[2]);
  EXPECT_NEAR(direction.x, current.direction.x, 1e-6);
  EXPECT_NEAR(direction.y, current.direction.y, 1e-6);
  EXPECT_NEAR(direction.z, current.direction.z, 1e-6);

  const auto path = std::filesystem::temp_directory_path() /
      ("container-manual-sun-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".glb");
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
  container::geometry::gltf::writeExportModel(exported, path);
  const auto reloaded = container::geometry::gltf::LoadSourceModel(path.string());
  const auto active = firstDirectional(reloaded);
  ASSERT_GE(active, 0);
  EXPECT_DOUBLE_EQ(reloaded.lights[active].intensity, current.intensity);
  EXPECT_EQ(reloaded.lights[active].name, "Manual Directional");
  EXPECT_EQ(reloaded.nodes[1].light, 1);
  EXPECT_EQ(reloaded.lights[1].name, "Untouched additional sun");
  EXPECT_DOUBLE_EQ(reloaded.lights[1].intensity, 9);
}

TEST(SceneLightExport, PreservesAllSupportedAreaDialectsAndMirroredAuthoredBasis) {
  for (const auto* dialect : {"KHR_lights_area", "EXT_lights_area", "WEBGI_lights_area", "WEBGI_area_light"}) {
    SCOPED_TRACE(dialect);
    auto source = scene(1);
    Value::Array definitions;
    definitions.emplace_back(Object{{"shape", Value(std::string("rectangle"))},
        {"width", Value(1.0)}, {"height", Value(2.0)}, {"authorNote", Value(std::string("retain"))}});
    source.extensions[dialect] = Value(Object{{"lights", Value(std::move(definitions))}});
    source.nodes[0].extensions[dialect] = Value(Object{{"light", Value(0)}});
    auto exported = source;
    auto current = entity(EditableLightType::Area, EditableLightSource::Imported, 0);
    current.tangent = {-1, 0, 0};
    current.bitangent = {0.1f, 1, 0};
    current.areaHalfSize = {2, 3};
    const std::array lights{current};
    const std::vector<int32_t> identities{0};
    exportEditableSceneLights(exported, lights, &source, identities);
    ASSERT_EQ(exported.lights.size(), 1u);
    const auto& definition = exported.lights[0];
    EXPECT_TRUE(definition.extensions.contains("KHR_lights_area"));
    EXPECT_EQ(definition.extras.Get("authorNote").Get<std::string>(), "retain");
    EXPECT_DOUBLE_EQ(definition.extensions.at("KHR_lights_area").Get("width").GetNumberAsDouble(), 4);
    EXPECT_DOUBLE_EQ(definition.extensions.at("KHR_lights_area").Get("height").GetNumberAsDouble(), 6);
    EXPECT_DOUBLE_EQ(definition.range, 25);
    EXPECT_FALSE(exported.nodes[0].extensions.contains(dialect));
    EXPECT_NEAR(matrix(exported.nodes[1])[0].x, -1, 1e-6);
    EXPECT_GT(matrix(exported.nodes[1])[1].x, 0);
  }
}

TEST(SceneLightExport, InvalidLaterLightLeavesDetachedSnapshotUnchanged) {
  auto model = scene(0);
  const auto original = model;
  auto first = entity(EditableLightType::Point, EditableLightSource::Manual, 0);
  auto second = entity(EditableLightType::Spot, EditableLightSource::Manual, 1);
  second.color.x = std::numeric_limits<float>::quiet_NaN();
  const std::array lights{first, second};
  EXPECT_THROW(exportEditableSceneLights(model, lights), std::runtime_error);
  EXPECT_EQ(model, original);
  second.color.x = 1;
  second.outerConeDegrees = 100;
  EXPECT_THROW(exportEditableSceneLights(model, std::array{second}), std::runtime_error);
  EXPECT_EQ(model, original);
}

TEST(SceneLightExport, ReloadsPortableManualLightsAndSkipsGeneratedDiagnostics) {
  auto model = scene(0);
  const std::array lights{
      entity(EditableLightType::Point, EditableLightSource::Manual, 0),
      entity(EditableLightType::Spot, EditableLightSource::Manual, 1),
      entity(EditableLightType::Directional, EditableLightSource::Generated, 0)};
  exportEditableSceneLights(model, lights);
  ASSERT_EQ(model.lights.size(), 2u);
  const auto path = std::filesystem::temp_directory_path() /
      ("container-light-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".glb");
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
  container::geometry::gltf::writeExportModel(model, path);
  const auto reloaded = container::geometry::gltf::LoadSourceModel(path.string());
  ASSERT_EQ(reloaded.lights.size(), 2u);
  EXPECT_EQ(reloaded.lights[0].type, "point");
  EXPECT_EQ(reloaded.lights[1].type, "spot");
  EXPECT_DOUBLE_EQ(reloaded.lights[0].range, 25);
  EXPECT_DOUBLE_EQ(reloaded.lights[1].intensity, 17);
  EXPECT_EQ(reloaded.scenes[0].nodes.size(), 2u);
}

TEST(SceneLightExport, BatchPreservesEverySceneAndIndependentBindingsSharingOneDefinition) {
  auto source = scene(3);
  source.scenes.resize(2); source.scenes[0].nodes = {0}; source.scenes[1].nodes = {1,2};
  source.defaultScene = 1;
  source.lights.resize(2); source.lights[0].type = "point"; source.lights[1].type = "spot";
  source.lights[0].intensity = 2; source.lights[1].intensity = 3;
  source.nodes[0].light = source.nodes[1].light = 0; source.nodes[2].light = 1;
  auto first = entity(EditableLightType::Point,EditableLightSource::Imported,0); first.intensity = 7;
  auto second = first; second.intensity = 8;
  auto third = entity(EditableLightType::Spot,EditableLightSource::Imported,1); third.intensity = 9;
  const auto manual = entity(EditableLightType::Point,EditableLightSource::Manual,0);
  const auto generated = entity(EditableLightType::Point,EditableLightSource::Generated,0);
  const std::vector<std::vector<EditableLightEntity>> lights{{first},{second,third,manual,generated}};
  auto exported = source;
  exportEditableSceneLightsForScenes(exported,lights);
  EXPECT_EQ(exported.defaultScene,1); ASSERT_EQ(exported.scenes.size(),2u);
  ASSERT_EQ(exported.lights.size(),6u); ASSERT_EQ(exported.nodes.size(),7u);
  EXPECT_DOUBLE_EQ(exported.lights[0].intensity,2); EXPECT_DOUBLE_EQ(exported.lights[1].intensity,3);
  EXPECT_DOUBLE_EQ(exported.lights[2].intensity,7); EXPECT_DOUBLE_EQ(exported.lights[3].intensity,8);
  EXPECT_DOUBLE_EQ(exported.lights[4].intensity,9);
  EXPECT_EQ(exported.nodes[0].children,(std::vector<int>{3}));
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{4}));
  EXPECT_EQ(exported.nodes[2].children,(std::vector<int>{5}));
  EXPECT_EQ(exported.scenes[0].nodes,(std::vector<int>{0}));
  EXPECT_EQ(exported.scenes[1].nodes,(std::vector<int>{1,2,6}));
}

TEST(SceneLightExport, BatchDeduplicatesSharedBindingsWithoutRenumberingLocalOrdinals) {
  auto source = scene(2);
  source.scenes.resize(2); source.scenes[0].nodes = {0}; source.scenes[1].nodes = {1,0};
  source.lights.resize(2); source.lights[0].type = "point"; source.lights[1].type = "spot";
  source.nodes[0].light = 0; source.nodes[1].light = 1;
  auto shared = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  auto sharedInSecondScene = shared; sharedInSecondScene.id.index = 1;
  sharedInSecondScene.label = "A different scene-local ordinal label";
  const auto spot = entity(EditableLightType::Spot,EditableLightSource::Imported,0);
  const std::vector<std::vector<EditableLightEntity>> lights{{shared},{sharedInSecondScene,spot}};
  auto exported = source;
  const std::vector<int32_t> identities{0,1};
  exportEditableSceneLightsForScenes(exported,lights,&source,identities);
  ASSERT_EQ(exported.lights.size(),4u); ASSERT_EQ(exported.nodes.size(),4u);
  EXPECT_EQ(exported.nodes[0].children,(std::vector<int>{2}));
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{3}));
  EXPECT_EQ(exported.lights[2].type,"point"); EXPECT_EQ(exported.lights[3].type,"spot");
  EXPECT_EQ(exported.scenes[1].nodes,(std::vector<int>{1,0}));
}

TEST(SceneLightExport, BatchDeduplicatesAllSharedLightTypesAndRejectsConflictsTransactionally) {
  for (const auto type : {EditableLightType::Point,EditableLightType::Spot,
                          EditableLightType::Area,EditableLightType::Directional}) {
    SCOPED_TRACE(static_cast<uint32_t>(type));
    auto source = scene(1); source.scenes.resize(2); source.scenes[1].nodes = {0};
    source.nodes[0].light = 0; source.lights.resize(1);
    source.lights[0].type = type == EditableLightType::Directional ? "directional" :
                            type == EditableLightType::Spot ? "spot" : "point";
    if (type == EditableLightType::Area)
      source.lights[0].extensions["KHR_lights_area"] = Value(Object{{"shape",Value(std::string("rectangle"))}});
    const auto shared = entity(type,EditableLightSource::Imported,0);
    std::vector<std::vector<EditableLightEntity>> lights{{shared},{shared}};
    auto exported = source;
    exportEditableSceneLightsForScenes(exported,lights);
    EXPECT_EQ(exported.lights.size(),2u); EXPECT_EQ(exported.nodes.size(),2u);
    EXPECT_EQ(exported.nodes[0].children,(std::vector<int>{1}));
    lights[1][0].intensity += 1;
    exported = source;
    EXPECT_THROW(exportEditableSceneLightsForScenes(exported,lights),std::runtime_error);
    EXPECT_EQ(exported,source);
  }
}

TEST(SceneLightExport, BatchPreservesSharedSunPriorityAndIgnoresSceneDependentGizmoPositions) {
  auto source = scene(3); source.scenes.resize(2);
  source.scenes[0].nodes = {0,1}; source.scenes[1].nodes = {2,0};
  source.lights.resize(3);
  for (size_t i = 0; i < source.lights.size(); ++i) {
    source.lights[i].type = "directional"; source.nodes[i].light = static_cast<int>(i);
  }
  // The shared binding is first in original traversal in both scenes, while
  // edited exported root ordering initially prefers another sun in scene 1.
  source.scenes[1].nodes = {0,2};
  auto exported = source; exported.scenes[1].nodes = {2,0};
  const auto shared = entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  auto otherAnchor = shared; otherAnchor.position = {100,200,300};
  const std::vector<std::vector<EditableLightEntity>> lights{{shared},{otherAnchor}};
  const std::vector<int32_t> identities{0,1,2};
  exportEditableSceneLightsForScenes(exported,lights,&source,identities);
  ASSERT_EQ(exported.lights.size(),4u);
  EXPECT_EQ(firstDirectional(exported),3);
  exported.defaultScene = 1; EXPECT_EQ(firstDirectional(exported),3);
  EXPECT_EQ(exported.scenes[1].nodes,(std::vector<int>{0,2}));
}

TEST(SceneLightExport, BatchRejectsManualSunReplacingASharedBindingWithoutLosingOtherScenesSun) {
  auto source = scene(1); source.scenes.resize(2); source.scenes[1].nodes = {0};
  source.nodes[0].light = 0; source.lights.resize(1); source.lights[0].type = "directional";
  const auto manual = entity(EditableLightType::Directional,EditableLightSource::Manual,0);
  const std::vector<std::vector<EditableLightEntity>> lights{{manual},{}};
  auto exported = source;
  EXPECT_THROW(exportEditableSceneLightsForScenes(exported,lights),std::runtime_error);
  EXPECT_EQ(exported,source);
}

TEST(SceneLightExport, BatchLateInvalidLightLeavesTheCompleteSnapshotUnchanged) {
  auto model = scene(0); model.scenes.resize(2); model.defaultScene = 1;
  const auto point = entity(EditableLightType::Point,EditableLightSource::Manual,0);
  auto invalidSpot = entity(EditableLightType::Spot,EditableLightSource::Manual,0);
  invalidSpot.outerConeDegrees = 100;
  const std::vector<std::vector<EditableLightEntity>> lights{{point},{invalidSpot}};
  const auto original = model;
  EXPECT_THROW(exportEditableSceneLightsForScenes(model,lights),std::runtime_error);
  EXPECT_EQ(model,original);
}

TEST(SceneLightExport, BatchRetainsEmptyUnvisitedScenesAndValidatesTheSceneTable) {
  auto source = scene(2); source.scenes.resize(2);
  source.scenes[0].nodes = {0}; source.scenes[1].nodes = {1};
  source.lights.resize(1); source.lights[0].type = "point";
  source.nodes[0].light = source.nodes[1].light = 0;
  const auto manual = entity(EditableLightType::Point,EditableLightSource::Manual,0);
  const std::vector<std::vector<EditableLightEntity>> lights{{},{manual}};
  auto exported = source;
  exportEditableSceneLightsForScenes(exported,lights);
  EXPECT_EQ(exported.nodes[0],source.nodes[0]); EXPECT_EQ(exported.nodes[1],source.nodes[1]);
  EXPECT_EQ(exported.scenes[0],source.scenes[0]);
  EXPECT_EQ(exported.scenes[1].nodes,(std::vector<int>{1,2}));
  const auto original = exported;
  const std::vector<std::vector<EditableLightEntity>> wrongSize{{manual}};
  EXPECT_THROW(exportEditableSceneLightsForScenes(exported,wrongSize),std::runtime_error);
  EXPECT_EQ(exported,original);
}

TEST(SceneLightExport, FreshEvaluationTransformsAuthoredPoseOnceAndPreservesRangeAndAreaSize) {
  container::scene::AuthoredSceneLights authored;
  auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  point.position = {1,2,3}; point.range = 17;
  auto spot = entity(EditableLightType::Spot,EditableLightSource::Imported,1);
  spot.position = {-2,4,6}; spot.direction = glm::normalize(glm::vec3(1,-1,-1)); spot.range = 23;
  authored.points = {pointLightDataFromEditable(point),pointLightDataFromEditable(spot)};
  auto area = entity(EditableLightType::Area,EditableLightSource::Imported,0);
  area.position = {3,5,7}; area.direction = {0,0,-1};
  area.tangent = {1,0,0}; area.bitangent = {0,-1,0};
  area.areaHalfSize = {2,3}; area.range = 29;
  authored.areas = {areaLightDataFromEditable(area)};
  authored.directionals = {{{0,0,-1,0},{0.2f,0.4f,0.6f,7}},{{1,0,0,0},{1,1,1,100}}};
  const auto outer = glm::translate(glm::mat4(1),glm::vec3(10,20,30))*
      glm::rotate(glm::mat4(1),glm::radians(90.0f),glm::vec3(0,1,0))*
      glm::scale(glm::mat4(1),glm::vec3(2,3,4));
  const auto lights = evaluateEditableSceneLights(authored,SceneLightEdits{},outer);
  ASSERT_EQ(lights.size(),4u);
  EXPECT_EQ(lights[0].type,EditableLightType::Directional);
  EXPECT_NEAR(glm::length(lights[0].direction-glm::normalize(glm::vec3(outer*glm::vec4(0,0,-1,0)))),0,1e-6);
  EXPECT_FLOAT_EQ(lights[0].intensity,7);
  EXPECT_NEAR(glm::length(lights[1].position-glm::vec3(outer*glm::vec4(point.position,1))),0,1e-5);
  EXPECT_FLOAT_EQ(lights[1].range,17);
  EXPECT_NEAR(glm::length(lights[2].position-glm::vec3(outer*glm::vec4(spot.position,1))),0,1e-5);
  EXPECT_NEAR(glm::length(lights[2].direction-glm::normalize(glm::vec3(outer*glm::vec4(spot.direction,0)))),0,1e-6);
  EXPECT_FLOAT_EQ(lights[2].range,23);
  EXPECT_NEAR(lights[2].innerConeDegrees,spot.innerConeDegrees,1e-4);
  EXPECT_NEAR(lights[2].outerConeDegrees,spot.outerConeDegrees,1e-4);
  EXPECT_NEAR(glm::length(lights[3].position-glm::vec3(outer*glm::vec4(area.position,1))),0,1e-5);
  EXPECT_NEAR(glm::length(lights[3].tangent-glm::normalize(glm::vec3(outer*glm::vec4(glm::vec3(authored.areas[0].tangentHalfSize),0)))),0,1e-6);
  EXPECT_NEAR(glm::length(lights[3].bitangent-glm::normalize(glm::vec3(outer*glm::vec4(glm::vec3(authored.areas[0].bitangentHalfSize),0)))),0,1e-6);
  EXPECT_EQ(lights[3].areaHalfSize,glm::vec2(2,3)); EXPECT_FLOAT_EQ(lights[3].range,29);
  EXPECT_EQ(authored.points[0].positionRadius,glm::vec4(1,2,3,17));
}

TEST(SceneLightExport, FreshEvaluationAppliesOnlyExplicitWorldSpaceImportedOverrides) {
  container::scene::AuthoredSceneLights authored;
  authored.points = {pointLightDataFromEditable(entity(EditableLightType::Point,EditableLightSource::Imported,0))};
  authored.areas = {areaLightDataFromEditable(entity(EditableLightType::Area,EditableLightSource::Imported,0))};
  authored.directionals = {{{0,0,-1,0},{1,1,1,2}}};
  SceneLightEdits edits;
  auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  point.position = {70,80,90}; point.range = 41; point.intensity = 43;
  edits.importedPointOverrides = {pointLightDataFromEditable(point)};
  auto area = entity(EditableLightType::Area,EditableLightSource::Imported,0);
  area.position = {100,110,120}; area.areaHalfSize = {5,6};
  edits.importedAreaOverrides = {areaLightDataFromEditable(area)};
  auto sun = entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  sun.direction = {1,0,0}; sun.intensity = 47; edits.directionalOverride = sun;
  const auto lights = evaluateEditableSceneLights(authored,edits,
      glm::translate(glm::mat4(1),glm::vec3(1000,2000,3000)));
  ASSERT_EQ(lights.size(),3u);
  EXPECT_EQ(lights[0].direction,glm::vec3(1,0,0)); EXPECT_FLOAT_EQ(lights[0].intensity,47);
  EXPECT_EQ(lights[1].position,point.position); EXPECT_FLOAT_EQ(lights[1].range,41);
  EXPECT_FLOAT_EQ(lights[1].intensity,43);
  EXPECT_EQ(lights[2].position,area.position); EXPECT_EQ(lights[2].areaHalfSize,area.areaHalfSize);
}

TEST(SceneLightExport, FreshEvaluationPreservesManualIdsAndSunPrecedenceWithoutGeneratedContent) {
  container::scene::AuthoredSceneLights authored;
  authored.directionals = {{{0,0,-1,0},{1,1,1,2}}};
  SceneLightEdits edits;
  auto sun = entity(EditableLightType::Directional,EditableLightSource::Manual,0);
  sun.direction = {0,-1,0}; sun.intensity = 53; edits.directionalOverride = sun;
  auto spot = entity(EditableLightType::Spot,EditableLightSource::Manual,0);
  auto point = entity(EditableLightType::Point,EditableLightSource::Manual,1);
  auto area = entity(EditableLightType::Area,EditableLightSource::Manual,0);
  edits.manualPointLights = {pointLightDataFromEditable(spot),pointLightDataFromEditable(point)};
  edits.manualAreaLights = {areaLightDataFromEditable(area)};
  edits.generatedPointOverrides = {pointLightDataFromEditable(point)};
  edits.generatedAreaOverrides = {areaLightDataFromEditable(area)};
  const auto lights = evaluateEditableSceneLights(authored,edits,
      glm::translate(glm::mat4(1),glm::vec3(1000,2000,3000)));
  ASSERT_EQ(lights.size(),4u);
  EXPECT_EQ(lights[0].source,EditableLightSource::Manual);
  EXPECT_EQ(lights[0].direction,sun.direction); EXPECT_FLOAT_EQ(lights[0].intensity,53);
  EXPECT_EQ(lights[1].type,EditableLightType::Spot); EXPECT_EQ(lights[1].id.index,0u);
  EXPECT_EQ(lights[2].type,EditableLightType::Point); EXPECT_EQ(lights[2].id.index,1u);
  EXPECT_EQ(lights[3].type,EditableLightType::Area); EXPECT_EQ(lights[3].id.index,0u);
  for (const auto& light : lights) EXPECT_EQ(light.source,EditableLightSource::Manual);
  EXPECT_EQ(lights[1].position,spot.position); EXPECT_EQ(lights[3].position,area.position);
}

TEST(SceneLightExport, FreshEvaluationOmitsGeneratedSunAndStaleImportedOverridesWithoutSourceBindings) {
  SceneLightEdits edits;
  edits.directionalOverride = entity(EditableLightType::Directional,EditableLightSource::Generated,0);
  edits.importedPointOverrides = {pointLightDataFromEditable(entity(EditableLightType::Point,EditableLightSource::Imported,0))};
  edits.importedAreaOverrides = {areaLightDataFromEditable(entity(EditableLightType::Area,EditableLightSource::Imported,0))};
  const container::scene::AuthoredSceneLights authored;
  EXPECT_TRUE(evaluateEditableSceneLights(authored,edits,glm::mat4(1)).empty());
  edits.directionalOverride = entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  EXPECT_TRUE(evaluateEditableSceneLights(authored,edits,glm::mat4(1)).empty());
  edits.directionalOverride = entity(EditableLightType::Directional,EditableLightSource::Manual,0);
  const auto manual = evaluateEditableSceneLights(authored,edits,glm::mat4(1));
  ASSERT_EQ(manual.size(),1u); EXPECT_EQ(manual[0].source,EditableLightSource::Manual);
}

TEST(SceneLightExport, FreshEvaluationReadsCurrentPoseWhileRetainingExplicitEditsAndManualLights) {
  container::scene::AuthoredSceneLights authored;
  const auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  authored.points = {pointLightDataFromEditable(point),pointLightDataFromEditable(point)};
  SceneLightEdits edits;
  edits.importedPointOverrides.resize(2);
  auto explicitEdit = point; explicitEdit.position = {200,300,400};
  edits.importedPointOverrides[1] = pointLightDataFromEditable(explicitEdit);
  const auto manual = entity(EditableLightType::Point,EditableLightSource::Manual,0);
  edits.manualPointLights = {pointLightDataFromEditable(manual)};
  const auto old = evaluateEditableSceneLights(authored,edits,glm::mat4(1));
  authored.points[0].positionRadius = {11,13,17,19}; authored.points[0].colorIntensity.w = 23;
  authored.points[1].positionRadius = {100,200,300,400};
  const auto current = evaluateEditableSceneLights(authored,edits,
      glm::translate(glm::mat4(1),glm::vec3(10,20,30)));
  ASSERT_EQ(current.size(),3u); ASSERT_EQ(old.size(),3u);
  EXPECT_NE(current[0].position,old[0].position);
  EXPECT_EQ(current[0].position,glm::vec3(21,33,47)); EXPECT_FLOAT_EQ(current[0].range,19);
  EXPECT_FLOAT_EQ(current[0].intensity,23);
  EXPECT_EQ(current[1].position,explicitEdit.position);
  EXPECT_EQ(current[2].position,manual.position);
}

TEST(SceneLightExport, FreshEvaluationRetainsAuthoredContentBeyondRendererLightCaps) {
  container::scene::AuthoredSceneLights authored;
  authored.points.resize(size_t(container::gpu::kMaxClusteredLights)+1,
      pointLightDataFromEditable(entity(EditableLightType::Point,EditableLightSource::Imported,0)));
  authored.areas.resize(size_t(container::gpu::kMaxAreaLights)+1,
      areaLightDataFromEditable(entity(EditableLightType::Area,EditableLightSource::Imported,0)));
  const auto lights = evaluateEditableSceneLights(authored,SceneLightEdits{},glm::mat4(1));
  ASSERT_EQ(lights.size(),authored.points.size()+authored.areas.size());
  EXPECT_EQ(lights[authored.points.size()-1].id.index,container::gpu::kMaxClusteredLights);
  EXPECT_EQ(lights.back().id.index,container::gpu::kMaxAreaLights);
}

TEST(SceneLightExport, FreshEvaluationAuthorsScaledRangesForPreviouslyUnvisitedScenes) {
  auto source = scene(2); source.scenes.resize(2);
  source.scenes[0].nodes = {0}; source.scenes[1].nodes = {1};
  source.lights.resize(1); source.lights[0].type = "point"; source.lights[0].range = 5;
  source.nodes[0].light = source.nodes[1].light = 0;
  source.nodes[0].translation = {2,0,0}; source.nodes[1].translation = {20,0,0};
  container::scene::AuthoredSceneLights first, unvisited;
  auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  point.position = {2,0,0}; point.range = 10;
  first.points = {pointLightDataFromEditable(point)};
  point.position = {20,0,0}; unvisited.points = {pointLightDataFromEditable(point)};
  const std::vector<std::vector<EditableLightEntity>> lights{
      evaluateEditableSceneLights(first,SceneLightEdits{},glm::mat4(1)),
      evaluateEditableSceneLights(unvisited,SceneLightEdits{},glm::mat4(1))};
  auto exported = source;
  exportEditableSceneLightsForScenes(exported,lights,nullptr,{},2);
  ASSERT_EQ(exported.lights.size(),3u);
  EXPECT_DOUBLE_EQ(exported.lights[0].range,5);
  EXPECT_DOUBLE_EQ(exported.lights[1].range,10); EXPECT_DOUBLE_EQ(exported.lights[2].range,10);
  const auto secondWorld = matrix(exported.nodes[1])*matrix(exported.nodes[exported.nodes[1].children[0]]);
  EXPECT_EQ(glm::vec3(secondWorld[3]),glm::vec3(20,0,0));
}

TEST(SceneLightExport, GlobalImportedEditsFollowSourceBindingsAcrossSceneLocalOrdinals) {
  container::scene::AuthoredSceneLights first;
  first.pointBindings = {{10,{}},{20,{}}};
  first.areaBindings = {{30,"KHR_lights_area"},{40,"EXT_lights_area"}};
  auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  point.position = {101,102,103};
  auto area = entity(EditableLightType::Area,EditableLightSource::Imported,1);
  area.position = {201,202,203};
  SceneLightEdits edits;
  edits.importedPointOverrides = {pointLightDataFromEditable(point),std::nullopt};
  edits.importedAreaOverrides = {std::nullopt,areaLightDataFromEditable(area)};
  ImportedSceneLightEdits global; global.capture(first,edits);
  container::scene::AuthoredSceneLights second;
  second.pointBindings = {{20,{}},{50,{}},{10,{}}};
  second.areaBindings = {{40,"EXT_lights_area"},{30,"KHR_lights_area"},{60,"KHR_lights_area"}};
  const auto merged = global.merge(second,SceneLightEdits{});
  ASSERT_EQ(merged.importedPointOverrides.size(),3u);
  EXPECT_FALSE(merged.importedPointOverrides[0]); EXPECT_FALSE(merged.importedPointOverrides[1]);
  ASSERT_TRUE(merged.importedPointOverrides[2]);
  EXPECT_EQ(glm::vec3(merged.importedPointOverrides[2]->positionRadius),point.position);
  ASSERT_EQ(merged.importedAreaOverrides.size(),3u); ASSERT_TRUE(merged.importedAreaOverrides[0]);
  EXPECT_EQ(glm::vec3(merged.importedAreaOverrides[0]->positionRange),area.position);
  EXPECT_FALSE(merged.importedAreaOverrides[1]); EXPECT_FALSE(merged.importedAreaOverrides[2]);
}

TEST(SceneLightExport, EditableLightSelectionFollowsOnlyTheSameVisibleBinding) {
  using Binding = container::scene::AuthoredLightBinding;
  const EditableLightId point{EditableLightType::Point, EditableLightSource::Imported, 1};
  const std::array<Binding, 2> before{{{10, {}}, {20, {}}}};
  const auto selectedPoint = selectedImportedLightBinding(point, before);
  ASSERT_TRUE(selectedPoint);
  EXPECT_EQ(selectedPoint->sourceNode, 20);
  const std::array<Binding, 2> reordered{{{20, {}}, {10, {}}}};
  EXPECT_EQ(remapImportedLightSelection(point, &*selectedPoint, reordered),
            (EditableLightId{EditableLightType::Point, EditableLightSource::Imported, 0}));
  EXPECT_FALSE(isValidEditableLightId(remapImportedLightSelection(point, &*selectedPoint,
      std::span<const Binding>(before).first(1))));

  const EditableLightId area{EditableLightType::Area, EditableLightSource::Imported, 0};
  const Binding selectedArea{30, "KHR_lights_area"};
  const std::array<Binding, 1> otherDialect{{{30, "EXT_lights_area"}}};
  EXPECT_FALSE(isValidEditableLightId(remapImportedLightSelection(area, &selectedArea, otherDialect)));

  const EditableLightId sun{EditableLightType::Directional, EditableLightSource::Imported, 0};
  const auto selectedSun = selectedImportedLightBinding(sun, before);
  ASSERT_TRUE(selectedSun);
  EXPECT_EQ(remapImportedLightSelection(sun, &*selectedSun, before), sun);
  EXPECT_FALSE(isValidEditableLightId(remapImportedLightSelection(sun, &*selectedSun, reordered)));

  const EditableLightId manual{EditableLightType::Directional, EditableLightSource::Manual, 0};
  EXPECT_FALSE(selectedImportedLightBinding(manual, before));
  EXPECT_EQ(remapImportedLightSelection(manual, nullptr, reordered), manual);
}

TEST(SceneLightExport, GlobalImportedBindingIdentityIncludesSourceNodeAndExactAreaDialect) {
  container::scene::AuthoredSceneLights source;
  source.pointBindings = {{10,{}}};
  source.areaBindings = {{10,"KHR_lights_area"},{10,"EXT_lights_area"},{20,"KHR_lights_area"}};
  const auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  auto area = entity(EditableLightType::Area,EditableLightSource::Imported,0);
  SceneLightEdits edits;
  edits.importedPointOverrides = {pointLightDataFromEditable(point)};
  for (const float intensity : {11.f,22.f,33.f}) {
    area.intensity = intensity; edits.importedAreaOverrides.push_back(areaLightDataFromEditable(area));
  }
  ImportedSceneLightEdits global; global.capture(source,edits);
  container::scene::AuthoredSceneLights reordered;
  reordered.pointBindings = {{20,{}},{10,{}}};
  reordered.areaBindings = {{20,"KHR_lights_area"},{10,"EXT_lights_area"},{10,"KHR_lights_area"}};
  const auto merged = global.merge(reordered,SceneLightEdits{});
  ASSERT_EQ(merged.importedPointOverrides.size(),2u);
  EXPECT_FALSE(merged.importedPointOverrides[0]); ASSERT_TRUE(merged.importedPointOverrides[1]);
  ASSERT_EQ(merged.importedAreaOverrides.size(),3u);
  for (const auto& light : merged.importedAreaOverrides) ASSERT_TRUE(light);
  EXPECT_FLOAT_EQ(merged.importedAreaOverrides[0]->colorIntensity.w,33);
  EXPECT_FLOAT_EQ(merged.importedAreaOverrides[1]->colorIntensity.w,22);
  EXPECT_FLOAT_EQ(merged.importedAreaOverrides[2]->colorIntensity.w,11);
}

TEST(SceneLightExport, LatestGlobalImportedEditSupersedesStaleSceneCacheAndRetainsLocalManualContent) {
  container::scene::AuthoredSceneLights source;
  source.pointBindings = {{10,{}}}; source.areaBindings = {{20,"KHR_lights_area"}};
  auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  auto area = entity(EditableLightType::Area,EditableLightSource::Imported,0);
  SceneLightEdits cached;
  cached.importedPointOverrides = {pointLightDataFromEditable(point)};
  cached.importedAreaOverrides = {areaLightDataFromEditable(area)};
  const auto manual = entity(EditableLightType::Spot,EditableLightSource::Manual,0);
  cached.manualPointLights = {pointLightDataFromEditable(manual)};
  ImportedSceneLightEdits global; global.capture(source,cached);
  auto secondScene = global.merge(source,SceneLightEdits{});
  secondScene.importedPointOverrides[0]->positionRadius.x = 123;
  secondScene.importedAreaOverrides[0]->colorIntensity.w = 456;
  global.capture(source,secondScene);
  const auto restored = global.merge(source,cached);
  ASSERT_TRUE(restored.importedPointOverrides[0]); ASSERT_TRUE(restored.importedAreaOverrides[0]);
  EXPECT_FLOAT_EQ(restored.importedPointOverrides[0]->positionRadius.x,123);
  EXPECT_FLOAT_EQ(restored.importedAreaOverrides[0]->colorIntensity.w,456);
  ASSERT_EQ(restored.manualPointLights.size(),1u);
  EXPECT_EQ(restored.manualPointLights[0].positionRadius,cached.manualPointLights[0].positionRadius);
  EXPECT_EQ(restored.manualPointLights[0].coneOuterCosType,cached.manualPointLights[0].coneOuterCosType);
  EXPECT_NE(restored.importedPointOverrides[0]->positionRadius,cached.importedPointOverrides[0]->positionRadius);
}

TEST(SceneLightExport, ClearingGlobalImportedOverridesRevealsFreshSourceAnimationInsteadOfStaleCachedValues) {
  container::scene::AuthoredSceneLights source;
  source.pointBindings = {{10,{}}}; source.areaBindings = {{20,"KHR_lights_area"}};
  const auto point = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  const auto area = entity(EditableLightType::Area,EditableLightSource::Imported,0);
  source.points = {pointLightDataFromEditable(point)}; source.areas = {areaLightDataFromEditable(area)};
  SceneLightEdits stale;
  stale.importedPointOverrides = {source.points[0]}; stale.importedAreaOverrides = {source.areas[0]};
  ImportedSceneLightEdits global; global.capture(source,stale);
  global.capture(source,SceneLightEdits{});
  source.points[0].positionRadius = {100,200,300,400}; source.points[0].colorIntensity.w = 500;
  source.areas[0].positionRange = {600,700,800,900};
  const auto restored = global.merge(source,stale);
  ASSERT_EQ(restored.importedPointOverrides.size(),1u); EXPECT_FALSE(restored.importedPointOverrides[0]);
  ASSERT_EQ(restored.importedAreaOverrides.size(),1u); EXPECT_FALSE(restored.importedAreaOverrides[0]);
  const auto evaluated = evaluateEditableSceneLights(source,restored,glm::mat4(1));
  ASSERT_EQ(evaluated.size(),2u);
  EXPECT_EQ(evaluated[0].position,glm::vec3(100,200,300)); EXPECT_FLOAT_EQ(evaluated[0].intensity,500);
  EXPECT_EQ(evaluated[1].position,glm::vec3(600,700,800));
}

TEST(SceneLightExport, LocalManualSunRetainsGlobalImportedSunForOtherScenesAndDoesNotFreezeUnrelatedBindings) {
  container::scene::AuthoredSceneLights source;
  source.directionalBindings = {{10,{}}}; source.directionals = {{{0,0,-1,0},{1,1,1,2}}};
  auto importedSun = entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  importedSun.intensity = 11;
  SceneLightEdits imported; imported.directionalOverride = importedSun;
  ImportedSceneLightEdits global; global.capture(source,imported);
  auto manualSun = entity(EditableLightType::Directional,EditableLightSource::Manual,0);
  manualSun.intensity = 22;
  SceneLightEdits local; local.directionalOverride = manualSun;
  global.capture(source,local);
  const auto localRestored = global.merge(source,local);
  ASSERT_TRUE(localRestored.directionalOverride);
  EXPECT_EQ(localRestored.directionalOverride->source,EditableLightSource::Manual);
  EXPECT_FLOAT_EQ(localRestored.directionalOverride->intensity,22);
  auto other = global.merge(source,SceneLightEdits{});
  ASSERT_TRUE(other.directionalOverride);
  EXPECT_EQ(other.directionalOverride->source,EditableLightSource::Imported);
  EXPECT_FLOAT_EQ(other.directionalOverride->intensity,11);
  other.directionalOverride->intensity = 33; global.capture(source,other);
  const auto stillManual = evaluateEditableSceneLights(source,global.merge(source,local),glm::mat4(1));
  ASSERT_EQ(stillManual.size(),1u); EXPECT_FLOAT_EQ(stillManual[0].intensity,22);
  auto unrelated = source; unrelated.directionalBindings = {{99,{}}};
  const auto unrelatedState = global.merge(unrelated,imported);
  EXPECT_FALSE(unrelatedState.directionalOverride);
  const auto fresh = evaluateEditableSceneLights(unrelated,unrelatedState,glm::mat4(1));
  ASSERT_EQ(fresh.size(),1u); EXPECT_FLOAT_EQ(fresh[0].intensity,2);
}

TEST(SceneLightExport, CaptureBeforeManualSunReplacementRetainsLastImportedEditAndGeneratedLightingStaysLocal) {
  container::scene::AuthoredSceneLights source;
  source.directionalBindings = {{10,{}}}; source.directionals = {{{0,0,-1,0},{1,1,1,2}}};
  auto importedSun = entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  importedSun.intensity = 79;
  SceneLightEdits beforeManual; beforeManual.directionalOverride = importedSun;
  ImportedSceneLightEdits global;
  global.capture(source,beforeManual); // The UI hook captures before replacing the active sun.
  SceneLightEdits afterManual;
  afterManual.directionalOverride = entity(EditableLightType::Directional,EditableLightSource::Manual,0);
  global.capture(source,afterManual);
  SceneLightEdits generated;
  generated.directionalOverride = entity(EditableLightType::Directional,EditableLightSource::Generated,0);
  generated.generatedPointOverrides = {pointLightDataFromEditable(entity(EditableLightType::Point,EditableLightSource::Generated,0))};
  const container::scene::AuthoredSceneLights noSource;
  global.capture(noSource,generated);
  const auto sharedScene = evaluateEditableSceneLights(source,global.merge(source,SceneLightEdits{}),glm::mat4(1));
  ASSERT_EQ(sharedScene.size(),1u);
  EXPECT_EQ(sharedScene[0].source,EditableLightSource::Imported); EXPECT_FLOAT_EQ(sharedScene[0].intensity,79);
  EXPECT_TRUE(evaluateEditableSceneLights(noSource,global.merge(noSource,generated),glm::mat4(1)).empty());
  EXPECT_EQ(generated.generatedPointOverrides.size(),1u);
}

TEST(SceneLightExport, BindingAwareOverlayDoesNotAliasFilteredFirstPointOrAreaLight) {
  auto source=scene(4); source.lights.resize(2);
  source.lights[0].type=source.lights[1].type="point";
  source.nodes[0].light=0; source.nodes[1].light=1;
  source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
  source.nodes[2].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
  source.extensions["KHR_lights_area"]=Value(Object{{"lights",Value(Value::Array{
      Value(Object{{"shape",Value(std::string("rectangle"))}}),
      Value(Object{{"shape",Value(std::string("rectangle"))}})})}});
  source.nodes[2].extensions["KHR_lights_area"]=Value(Object{{"light",Value(0)}});
  source.nodes[3].extensions["KHR_lights_area"]=Value(Object{{"light",Value(1)}});
  container::scene::AuthoredSceneLights visible;
  visible.pointBindings={{1,{}}}; visible.areaBindings={{3,"KHR_lights_area"}};
  visible.points={pointLightDataFromEditable(entity(EditableLightType::Point,EditableLightSource::Imported,0))};
  visible.areas={areaLightDataFromEditable(entity(EditableLightType::Area,EditableLightSource::Imported,0))};
  const std::vector<container::scene::AuthoredSceneLights> tables{visible};
  const std::vector<std::vector<EditableLightEntity>> lights{
      evaluateEditableSceneLights(visible,SceneLightEdits{},glm::mat4(1))};
  auto exported=source;
  exportEditableSceneLightsForScenes(exported,lights,nullptr,{},1,tables,tables);
  EXPECT_EQ(exported.nodes[0],source.nodes[0]); EXPECT_EQ(exported.nodes[2],source.nodes[2]);
  EXPECT_EQ(exported.nodes[1].light,-1); ASSERT_EQ(exported.nodes[1].children.size(),1u);
  EXPECT_FALSE(exported.nodes[3].extensions.contains("KHR_lights_area"));
  ASSERT_EQ(exported.nodes[3].children.size(),1u);
  EXPECT_EQ(exported.nodes[1].children[0],4); EXPECT_EQ(exported.nodes[3].children[0],5);
}

TEST(SceneLightExport, FullBindingEvaluationRetainsHiddenOverridesAndScaledRanges) {
  auto source=scene(2); source.lights.resize(1); source.lights[0].type="point"; source.lights[0].range=5;
  source.nodes[0].light=source.nodes[1].light=0;
  source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
  container::scene::AuthoredSceneLights full,visible;
  auto point=entity(EditableLightType::Point,EditableLightSource::Imported,0); point.range=10;
  full.points={pointLightDataFromEditable(point),pointLightDataFromEditable(point)};
  full.pointBindings={{0,{}},{1,{}}};
  visible.points={full.points[1]}; visible.pointBindings={{1,{}}};
  ImportedSceneLightEdits global;
  point.position={70,80,90}; point.range=20; point.intensity=100;
  global.points[{0,{}}]=pointLightDataFromEditable(point);
  const std::vector<container::scene::AuthoredSceneLights> fullTables{full},visibleTables{visible};
  const std::vector<std::vector<EditableLightEntity>> lights{
      evaluateEditableSceneLightsForExport(full,visible,SceneLightEdits{},global,glm::mat4(1))};
  ASSERT_EQ(lights[0].size(),2u); EXPECT_EQ(lights[0][0].position,point.position);
  auto exported=source;
  exportEditableSceneLightsForScenes(exported,lights,nullptr,{},2,fullTables,visibleTables);
  ASSERT_EQ(exported.lights.size(),3u);
  EXPECT_DOUBLE_EQ(exported.lights[1].range,20); EXPECT_DOUBLE_EQ(exported.lights[1].intensity,100);
  EXPECT_DOUBLE_EQ(exported.lights[2].range,10);
  EXPECT_FALSE(exported.nodes[0].extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
  EXPECT_EQ(exported.nodes[0].children,(std::vector<int>{2}));
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{3}));
}

TEST(SceneLightExport, FullSunEvaluationExportsHiddenEditsAndNonzeroFirstVisibleDirectionalId) {
  auto source=scene(2); source.lights.resize(2);
  source.lights[0].type=source.lights[1].type="directional";
  source.nodes[0].light=0; source.nodes[1].light=1;
  source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
  container::scene::AuthoredSceneLights full,visible;
  full.directionals={{{0,0,-1,0},{1,1,1,2}},{{0,-1,0,0},{1,1,1,3}}};
  full.directionalBindings={{0,{}},{1,{}}};
  visible.directionals={full.directionals[1]}; visible.directionalBindings={{1,{}}};
  ImportedSceneLightEdits global;
  auto sun=entity(EditableLightType::Directional,EditableLightSource::Imported,0);
  sun.intensity=31; global.directionals[{0,{}}]=sun;
  sun.intensity=41; global.directionals[{1,{}}]=sun;
  const std::vector<container::scene::AuthoredSceneLights> fullTables{full},visibleTables{visible};
  const std::vector<std::vector<EditableLightEntity>> lights{
      evaluateEditableSceneLightsForExport(full,visible,SceneLightEdits{},global,glm::mat4(1))};
  ASSERT_EQ(lights[0].size(),2u); EXPECT_EQ(lights[0][0].id.index,0u); EXPECT_EQ(lights[0][1].id.index,1u);
  auto exported=source;
  exportEditableSceneLightsForScenes(exported,lights,nullptr,{},1,fullTables,visibleTables);
  ASSERT_EQ(exported.lights.size(),4u);
  EXPECT_DOUBLE_EQ(exported.lights[2].intensity,31); EXPECT_DOUBLE_EQ(exported.lights[3].intensity,41);
  EXPECT_EQ(firstDirectional(exported),3);
  EXPECT_EQ(exported.nodes[0].children,(std::vector<int>{2}));
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{3}));
  EXPECT_FALSE(exported.nodes[0].extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
}

TEST(SceneLightExport, ManualSunReplacesOnlyFirstVisibleBindingAndRetainsHiddenSunEdits) {
  for (const bool allHidden : {false,true}) {
    SCOPED_TRACE(allHidden);
    auto source=scene(2); source.lights.resize(2);
    source.lights[0].type=source.lights[1].type="directional";
    source.nodes[0].light=0; source.nodes[1].light=1;
    source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
    if(allHidden) source.nodes[1].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
    container::scene::AuthoredSceneLights full,visible;
    full.directionals={{{0,0,-1,0},{1,1,1,2}},{{0,-1,0,0},{1,1,1,3}}};
    full.directionalBindings={{0,{}},{1,{}}};
    if(!allHidden) { visible.directionals={full.directionals[1]}; visible.directionalBindings={{1,{}}}; }
    ImportedSceneLightEdits global;
    auto hiddenSun=entity(EditableLightType::Directional,EditableLightSource::Imported,0);
    hiddenSun.intensity=31; global.directionals[{0,{}}]=hiddenSun;
    SceneLightEdits edits;
    auto manual=entity(EditableLightType::Directional,EditableLightSource::Manual,0);
    manual.intensity=100; edits.directionalOverride=manual;
    const std::vector<container::scene::AuthoredSceneLights> fullTables{full},visibleTables{visible};
    const std::vector<std::vector<EditableLightEntity>> lights{
        evaluateEditableSceneLightsForExport(full,visible,edits,global,glm::mat4(1))};
    ASSERT_EQ(lights[0].size(),allHidden?3u:2u);
    auto exported=source;
    exportEditableSceneLightsForScenes(exported,lights,nullptr,{},1,fullTables,visibleTables);
    EXPECT_EQ(exported.nodes[0].children,(std::vector<int>{2}));
    EXPECT_DOUBLE_EQ(exported.lights[2].intensity,31);
    EXPECT_DOUBLE_EQ(exported.lights[size_t(firstDirectional(exported))].intensity,100);
    EXPECT_EQ(exported.nodes[1].children.size(),allHidden?1u:0u);
    EXPECT_FALSE(exported.nodes[0].extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
  }
}

TEST(SceneLightExport, BindingAwareOverlayResolvesRuntimeReparentedSourceNodesOutsideOriginalSceneAdjacency) {
  auto source=scene(3); source.scenes.resize(2);
  source.scenes[0].nodes={0}; source.scenes[1].nodes={1}; source.nodes[0].children={2};
  source.lights.resize(1); source.lights[0].type="point"; source.nodes[2].light=0;
  auto exported=source; exported.nodes[0].children.clear(); exported.nodes[1].children={2};
  container::scene::AuthoredSceneLights second;
  second.pointBindings={{2,{}}};
  second.points={pointLightDataFromEditable(entity(EditableLightType::Point,EditableLightSource::Imported,0))};
  const std::vector<container::scene::AuthoredSceneLights> tables{{},second};
  const std::vector<std::vector<EditableLightEntity>> lights{{},
      evaluateEditableSceneLightsForExport(second,second,SceneLightEdits{},ImportedSceneLightEdits{},glm::mat4(1))};
  const std::vector<int32_t> identities{0,1,2};
  exportEditableSceneLightsForScenes(exported,lights,&source,identities,1,tables,tables);
  EXPECT_TRUE(exported.nodes[0].children.empty());
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{2}));
  EXPECT_EQ(exported.nodes[2].light,-1); EXPECT_EQ(exported.nodes[2].children,(std::vector<int>{3}));
}

TEST(SceneLightExport, BindingAwareSharedSunDeduplicationIgnoresSceneLocalIndicesAndConflictsRemainAtomic) {
  auto source=scene(2); source.scenes.resize(2);
  source.scenes[0].nodes={1}; source.scenes[1].nodes={0,1};
  source.lights.resize(2); source.lights[0].type=source.lights[1].type="directional";
  source.nodes[0].light=0; source.nodes[1].light=1;
  source.nodes[0].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(false)}});
  container::scene::AuthoredSceneLights first,second;
  first.directionals={{{0,0,-1,0},{1,1,1,41}}}; first.directionalBindings={{1,{}}};
  second.directionals={{{0,-1,0,0},{1,1,1,31}},first.directionals[0]};
  second.directionalBindings={{0,{}},{1,{}}};
  const std::vector<container::scene::AuthoredSceneLights> full{first,second},visible{first,first};
  std::vector<std::vector<EditableLightEntity>> lights{
      evaluateEditableSceneLightsForExport(first,first,SceneLightEdits{},ImportedSceneLightEdits{},glm::mat4(1)),
      evaluateEditableSceneLightsForExport(second,first,SceneLightEdits{},ImportedSceneLightEdits{},glm::mat4(1))};
  auto exported=source;
  exportEditableSceneLightsForScenes(exported,lights,nullptr,{},1,full,visible);
  ASSERT_EQ(exported.lights.size(),4u);
  EXPECT_EQ(exported.nodes[1].children,(std::vector<int>{2}));
  exported.defaultScene=1; EXPECT_EQ(firstDirectional(exported),2);
  lights[1][1].intensity+=1;
  exported=source;
  EXPECT_THROW(exportEditableSceneLightsForScenes(exported,lights,nullptr,{},1,full,visible),std::runtime_error);
  EXPECT_EQ(exported,source);
}

TEST(SceneLightExport, TenThousandDeepLightAndSunExportRetainsWorldPoseAndTraversalPriority) {
  constexpr int depth=10'000;
  for (const bool stableBindings : {false,true}) {
    SCOPED_TRACE(stableBindings);
    auto source=scene(depth); source.scenes[0].nodes={0};
    for (int i=0;i<depth;++i) {
      source.nodes[size_t(i)].translation={0,0,1};
      if(i+1<depth) source.nodes[size_t(i)].children={i+1};
    }
    source.lights.resize(2); source.lights[0].type="directional"; source.lights[1].type="point";
    source.nodes.back().light=0; source.nodes[depth/2].light=1;
    const auto point=entity(EditableLightType::Point,EditableLightSource::Imported,0);
    const auto sun=entity(EditableLightType::Directional,EditableLightSource::Imported,0);
    auto exported=source;
    if (stableBindings) {
      container::scene::AuthoredSceneLights table;
      table.pointBindings={{depth/2,{}}}; table.directionalBindings={{depth-1,{}}};
      const std::vector<container::scene::AuthoredSceneLights> tables{table};
      const std::vector<std::vector<EditableLightEntity>> lights{{point,sun}};
      exportEditableSceneLightsForScenes(exported,lights,nullptr,{},1,tables,tables);
    } else {
      const std::array lights{point,sun};
      exportEditableSceneLights(exported,lights);
    }
    ASSERT_EQ(exported.nodes.size(),size_t(depth+2)); ASSERT_EQ(exported.lights.size(),4u);
    EXPECT_EQ(firstDirectional(exported),3);
    EXPECT_EQ(exported.nodes.back().light,3);
    EXPECT_EQ(exported.nodes[depth-1].children,(std::vector<int>{depth+1}));
    EXPECT_EQ(exported.nodes[depth/2].children,(std::vector<int>{depth/2+1,depth}));
    glm::mat4 world(1);
    for(int i=0;i<depth;++i) {
      world*=matrix(exported.nodes[size_t(i)]);
      if(i+1<depth) EXPECT_EQ(exported.nodes[size_t(i)].children.front(),i+1);
    }
    const auto sunWorld=world*matrix(exported.nodes.back());
    EXPECT_EQ(glm::vec3(sunWorld[3]),sun.position);
    EXPECT_NEAR(glm::length(-glm::vec3(sunWorld[2])-sun.direction),0,1e-6);
    glm::mat4 pointWorld(1);
    for(int i=0;i<=depth/2;++i) pointWorld*=matrix(exported.nodes[size_t(i)]);
    pointWorld*=matrix(exported.nodes[size_t(depth)]);
    EXPECT_EQ(glm::vec3(pointWorld[3]),point.position);
  }
}

TEST(SceneLightExport, IterativeLegacySourceTraversalRejectsCyclesAndDuplicateReferencesBeforeMutation) {
  for (const bool cycle : {false,true}) {
    SCOPED_TRACE(cycle);
    auto exported=scene(3); exported.lights.resize(1); exported.lights[0].type="point";
    exported.nodes[2].light=0;
    auto source=exported;
    if (cycle) {
      source.scenes[0].nodes={0}; source.nodes[0].children={1}; source.nodes[1].children={0};
    } else {
      source.scenes[0].nodes={0,1,2}; source.nodes[0].children={1};
    }
    const auto original=exported;
    const std::array lights{entity(EditableLightType::Point,EditableLightSource::Imported,0)};
    const std::vector<int32_t> identities{0,1,2};
    EXPECT_THROW(exportEditableSceneLights(exported,lights,&source,identities),std::runtime_error);
    EXPECT_EQ(exported,original);
  }
}

TEST(SceneLightExport, IterativeWorldTraversalRejectsLateSceneCyclesAndDuplicateChildrenAtomically) {
  for (const bool cycle : {false,true}) {
    SCOPED_TRACE(cycle);
    auto model=scene(3); model.scenes.resize(2);
    model.scenes[0].nodes={0}; model.scenes[1].nodes={1};
    model.nodes[1].children=cycle?std::vector<int>{2}:std::vector<int>{2,2};
    if(cycle) model.nodes[2].children={1};
    const auto original=model;
    const auto manual=entity(EditableLightType::Point,EditableLightSource::Manual,0);
    const std::vector<std::vector<EditableLightEntity>> lights{{manual},{manual}};
    const std::vector<container::scene::AuthoredSceneLights> tables(2);
    EXPECT_THROW(exportEditableSceneLightsForScenes(model,lights,nullptr,{},1,tables,tables),std::runtime_error);
    EXPECT_EQ(model,original);
  }
}

TEST(SceneLightExport, BehaviorWritesTheExportedSharedLightDefinitionAfterOverlay) {
  using namespace container::geometry::gltf;
  auto model = scene(2);
  model.lights.resize(1);
  model.lights[0].type = "point";
  model.lights[0].intensity = 10;
  model.nodes[0].light = model.nodes[1].light = 0;
  lightBehavior(model,"intensity");
  const auto source = model;
  const std::array identities{int32_t{0},int32_t{1}};
  const std::array lights{entity(EditableLightType::Point,EditableLightSource::Imported,0),
                         entity(EditableLightType::Point,EditableLightSource::Imported,1)};
  exportEditableSceneLights(model,lights,&source,identities);
  ASSERT_EQ(model.nodes.size(),4u);
  ASSERT_GE(model.nodes[2].light,0);
  EXPECT_EQ(model.nodes[2].light,model.nodes[3].light);
  GltfInteractivity behavior(model);
  const auto dirty = behavior.advance(model,ParseAnimations(model),0);
  EXPECT_EQ(dirty.lights,std::vector<size_t>{size_t(model.nodes[2].light)});
  EXPECT_DOUBLE_EQ(model.lights[size_t(model.nodes[2].light)].intensity,.75);
  EXPECT_DOUBLE_EQ(source.lights[0].intensity,10);
}

TEST(SceneLightExport, BehaviorRejectsDivergentSharedDefinitionsAtomically) {
  auto model = scene(2);
  model.lights.resize(1);
  model.lights[0].type = "point";
  model.nodes[0].light = model.nodes[1].light = 0;
  lightBehavior(model,"intensity");
  const auto original = model;
  const std::array identities{int32_t{0},int32_t{1}};
  auto first = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  auto second = entity(EditableLightType::Point,EditableLightSource::Imported,1);
  second.intensity = first.intensity * 2;
  const std::array lights{first,second};
  EXPECT_THROW(exportEditableSceneLights(model,lights,&original,identities),std::runtime_error);
  EXPECT_EQ(model,original);
}

TEST(SceneLightExport, BehaviorValueSpaceChecksOnlyTheConsumedLightProperty) {
  using namespace container::geometry::gltf;
  for (const std::string property : {"intensity","range"}) {
    auto model = scene(1);
    model.lights.resize(1);
    model.lights[0].type = "point";
    model.nodes[0].light = 0;
    lightBehavior(model,property);
    const auto original = model;
    const std::array identities{int32_t{0}};
    const std::array lights{entity(EditableLightType::Point,EditableLightSource::Imported,0)};
    if (property == "range") {
      EXPECT_THROW(exportEditableSceneLights(model,lights,&original,identities,2),std::runtime_error);
      EXPECT_EQ(model,original);
    } else {
      ASSERT_NO_THROW(exportEditableSceneLights(model,lights,&original,identities,2));
      GltfInteractivity behavior(model);
      (void)behavior.advance(model,ParseAnimations(model),0);
      EXPECT_DOUBLE_EQ(model.lights[size_t(model.nodes.back().light)].intensity,.75);
    }
  }
  auto model = scene(1);
  model.lights.resize(1);
  model.lights[0].type = "point";
  model.nodes[0].light = 0;
  lightBehavior(model,"intensity");
  const auto original = model;
  const std::array identities{int32_t{0}};
  auto light = entity(EditableLightType::Point,EditableLightSource::Imported,0);
  light.color = {3,2,1};
  const std::array lights{light};
  EXPECT_THROW(exportEditableSceneLights(model,lights,&original,identities),std::runtime_error);
  EXPECT_EQ(model,original);
}
