#include <Container/geometry/GltfProjectedSubtreeClone.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfNodeFlags.h>
#include <Container/geometry/GltfSceneCatalog.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string>

namespace {
using namespace container::geometry::gltf;
using Value = tinygltf::Value;
using Object = Value::Object;
using Array = Value::Array;

void declare(tinygltf::Model& model, const std::string& name) {
  if (std::ranges::find(model.extensionsUsed, name) == model.extensionsUsed.end())
    model.extensionsUsed.push_back(name);
}
int bytes(tinygltf::Model& model, std::vector<unsigned char> data,
          int component, int type, size_t count) {
  tinygltf::Buffer buffer; buffer.data = std::move(data);
  tinygltf::BufferView view; view.buffer = int(model.buffers.size()); view.byteLength = buffer.data.size();
  tinygltf::Accessor accessor; accessor.sparse = {}; accessor.bufferView = int(model.bufferViews.size());
  accessor.componentType = component; accessor.type = type; accessor.count = count;
  const auto result = int(model.accessors.size());
  model.buffers.push_back(std::move(buffer)); model.bufferViews.push_back(view); model.accessors.push_back(accessor);
  return result;
}
int floats(tinygltf::Model& model, std::initializer_list<float> values, int type, size_t count) {
  std::vector<unsigned char> data(values.size() * sizeof(float));
  std::memcpy(data.data(), values.begin(), data.size());
  const auto result = bytes(model, std::move(data), TINYGLTF_COMPONENT_TYPE_FLOAT, type, count);
  if (type == TINYGLTF_TYPE_SCALAR && values.size()) {
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    model.accessors[size_t(result)].minValues = {*minimum};
    model.accessors[size_t(result)].maxValues = {*maximum};
  }
  return result;
}
tinygltf::Model fixture() {
  tinygltf::Model model; model.asset.version = "2.0";
  const auto positions = floats(model, {0,0,0, 1,0,0, 0,1,0}, TINYGLTF_TYPE_VEC3, 3);
  tinygltf::Image image; image.name = "resident"; image.as_is = true; image.mimeType = "image/png";
  image.image = {11,22,33,44}; model.images.push_back(image);
  model.samplers.emplace_back();
  tinygltf::Texture texture; texture.source = 0; texture.sampler = 0; model.textures.push_back(texture);
  tinygltf::Material material; material.pbrMetallicRoughness.baseColorTexture.index = 0;
  model.materials.push_back(material);
  tinygltf::Primitive primitive; primitive.mode = TINYGLTF_MODE_TRIANGLES;
  primitive.attributes["POSITION"] = positions; primitive.material = 0;
  primitive.extras = Value(Object{{"index", Value(198)}});
  tinygltf::Mesh mesh; mesh.primitives.push_back(primitive); model.meshes.push_back(mesh);
  model.nodes.resize(3);
  model.nodes[0].name = "Root"; model.nodes[0].children = {1,2}; model.nodes[0].translation = {3,0,0};
  model.nodes[1].name = "Selected"; model.nodes[1].mesh = 0; model.nodes[1].translation = {1,0,0};
  model.nodes[2].name = "Sibling"; model.nodes[2].mesh = 0;
  tinygltf::Scene scene; scene.nodes = {0}; model.scenes.push_back(scene); model.defaultScene = 0;
  return model;
}
void morph(tinygltf::Model& model) {
  const auto delta = floats(model, {0,0,1, 0,0,1, 0,0,1}, TINYGLTF_TYPE_VEC3, 3);
  model.meshes[0].primitives[0].targets = {{{"POSITION", delta}}};
  model.meshes[0].weights = {.25}; model.nodes[1].weights = {.3};
}
void channel(tinygltf::Model& model, tinygltf::Animation& animation,
             int input, int output, int node, std::string path) {
  tinygltf::AnimationSampler sampler; sampler.input = input; sampler.output = output;
  sampler.interpolation = "LINEAR";
  tinygltf::AnimationChannel target; target.sampler = int(animation.samplers.size());
  target.target_node = node; target.target_path = std::move(path);
  target.extras = Value(Object{{"index", Value(17)}});
  animation.samplers.push_back(sampler); animation.channels.push_back(target);
}
void pointer(tinygltf::Model& model, tinygltf::Animation& animation,
             int input, int output, std::string text) {
  declare(model, "KHR_animation_pointer");
  channel(model, animation, input, output, -1, "pointer");
  animation.channels.back().target_extensions["KHR_animation_pointer"] = Value(Object{
      {"pointer", Value(std::move(text))}, {"extras", Value(Object{{"index", Value(119)}})}});
  animation.channels.back().target_extensions_json_string = "original target bytes";
}
void animation(tinygltf::Model& model) {
  morph(model);
  const auto input = floats(model, {0,1}, TINYGLTF_TYPE_SCALAR, 2);
  tinygltf::Animation clip; clip.name = "Shared clip";
  channel(model, clip, input, floats(model, {1,0,0, 4,0,0}, TINYGLTF_TYPE_VEC3, 2), 1, "translation");
  pointer(model, clip, input, floats(model, {1,3}, TINYGLTF_TYPE_SCALAR, 2), "/nodes/1/scale/1");
  pointer(model, clip, input, floats(model, {.2f,.8f}, TINYGLTF_TYPE_SCALAR, 2), "/nodes/1/weights/0");
  pointer(model, clip, input, floats(model, {.2f,.9f}, TINYGLTF_TYPE_SCALAR, 2),
      "/materials/0/pbrMetallicRoughness/roughnessFactor");
  model.animations.push_back(clip);
}
std::string pointerText(const tinygltf::AnimationChannel& channel) {
  return channel.target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>();
}
void skin(tinygltf::Model& model) {
  model.nodes[2].name = "External ancestor"; model.nodes[2].translation = {2,0,0};
  model.nodes[2].children = {3};
  tinygltf::Node joint; joint.name = "External joint"; joint.translation = {0,4,0}; model.nodes.push_back(joint);
  tinygltf::Skin value; value.joints = {3}; value.skeleton = 2;
  value.inverseBindMatrices = floats(model, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, TINYGLTF_TYPE_MAT4, 1);
  value.extras = Value(Object{{"identity", Value(51)}});
  model.skins.push_back(value); model.nodes[1].skin = 0;
  model.meshes[0].primitives[0].attributes["JOINTS_0"] = bytes(model,
      std::vector<unsigned char>(12), TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, TINYGLTF_TYPE_VEC4, 3);
  model.meshes[0].primitives[0].attributes["WEIGHTS_0"] = floats(model,
      {1,0,0,0, 1,0,0,0, 1,0,0,0}, TINYGLTF_TYPE_VEC4, 3);
}
std::vector<glm::dmat4> worlds(const tinygltf::Model& model) {
  std::vector<int> parent(model.nodes.size(), -1);
  for (size_t n = 0; n < model.nodes.size(); ++n)
    for (const auto child : model.nodes[n].children) parent[size_t(child)] = int(n);
  std::vector<glm::dmat4> result(model.nodes.size(), glm::dmat4(1));
  for (size_t n = 0; n < model.nodes.size(); ++n)
    for (auto ancestor = int(n); ancestor >= 0; ancestor = parent[size_t(ancestor)]) {
      const auto& node = model.nodes[size_t(ancestor)]; glm::dmat4 local(1);
      if (!node.matrix.empty())
        for (size_t c = 0; c < 4; ++c) for (size_t r = 0; r < 4; ++r) local[c][r] = node.matrix[c * 4 + r];
      else {
        if (!node.translation.empty()) local = glm::translate(local, glm::dvec3(node.translation[0], node.translation[1], node.translation[2]));
        if (!node.scale.empty()) local = glm::scale(local, glm::dvec3(node.scale[0], node.scale[1], node.scale[2]));
      }
      result[n] = local * result[n];
    }
  return result;
}

GltfSubtreeCloneProjection projection(std::initializer_list<GltfProjectedCloneNode> nodes) {
  return {nodes,{0},0};
}
void cameraAndLight(tinygltf::Model& source,int node) {
  tinygltf::Light light;light.type="point";source.lights.push_back(light);
  tinygltf::Camera camera;camera.type="perspective";
  camera.perspective.yfov=.7;camera.perspective.znear=.1;source.cameras.push_back(camera);
  declare(source,"KHR_lights_punctual");
  source.extensions["KHR_lights_punctual"]=Value(Object{{"lights",Value(Array{
      Value(Object{{"type",Value(std::string("point"))}})})}});
  source.nodes[size_t(node)].camera=0;source.nodes[size_t(node)].light=0;
  source.nodes[size_t(node)].extensions["KHR_lights_punctual"]=Value(Object{{"light",Value(0)}});
}
void sameResources(const tinygltf::Model& result,const tinygltf::Model& source) {
  EXPECT_EQ(result.meshes,source.meshes);EXPECT_EQ(result.materials,source.materials);
  EXPECT_EQ(result.accessors,source.accessors);EXPECT_EQ(result.bufferViews,source.bufferViews);
  EXPECT_EQ(result.buffers,source.buffers);EXPECT_EQ(result.images,source.images);
  EXPECT_EQ(result.textures,source.textures);EXPECT_EQ(result.samplers,source.samplers);
  EXPECT_EQ(result.cameras,source.cameras);EXPECT_EQ(result.lights,source.lights);
}

TEST(GltfProjectedSubtreeClone,PreservesOldTopologyAndAuthoredRestWhileCloningNodeTracksOnly) {
  auto authored=fixture();animation(authored);auto live=authored;
  ApplyAnimationPose(live,SampleAnimation(ParseAnimations(authored).clips[0],.5));
  const auto oldLive=live,oldAuthored=authored;
  const auto plan=PlanGltfProjectedSubtreeClone(live,authored,
      projection({{1,-1,GltfCloneNodeRole::Visual,true,"Actual edited label",false,true,false}}));
  EXPECT_EQ(live,oldLive);EXPECT_EQ(authored,oldAuthored);
  const auto& result=plan.clone;ASSERT_EQ(result.nodes,(std::vector<int32_t>{-1,3,-1}));
  EXPECT_EQ(result.clonedChannels,3u);EXPECT_EQ(result.originalChannelCounts,(std::vector<size_t>{4}));
  EXPECT_FALSE(result.contextOnly[1]);EXPECT_TRUE(plan.retainMeshBindings[1]);EXPECT_TRUE(plan.copyWeightTracks[1]);
  EXPECT_EQ(result.authoredSource.nodes[0],authored.nodes[0]);
  EXPECT_EQ(result.authoredSource.nodes[3].translation,authored.nodes[1].translation);
  EXPECT_EQ(result.evaluatedSource.nodes[3].translation,live.nodes[1].translation);
  EXPECT_EQ(result.authoredSource.nodes[3].weights,authored.nodes[1].weights);
  EXPECT_EQ(result.evaluatedSource.nodes[3].weights,live.nodes[1].weights);
  EXPECT_EQ(result.authoredSource.nodes[3].name,"Actual edited label");
  const auto& channels=result.authoredSource.animations[0].channels;ASSERT_EQ(channels.size(),7u);
  for(size_t i=0;i<4;++i) {
    EXPECT_EQ(channels[i],authored.animations[0].channels[i]);
    EXPECT_EQ(channels[i].target_extensions,authored.animations[0].channels[i].target_extensions);
    EXPECT_EQ(channels[i].target_extensions_json_string,authored.animations[0].channels[i].target_extensions_json_string);
  }
  EXPECT_EQ(channels[4].target_node,3);EXPECT_EQ(pointerText(channels[5]),"/nodes/3/scale/1");
  EXPECT_EQ(pointerText(channels[6]),"/nodes/3/weights/0");
  EXPECT_TRUE(channels[5].target_extensions_json_string.empty());
  EXPECT_EQ(channels[5].target_extensions.at("KHR_animation_pointer").Get("extras"),
      authored.animations[0].channels[1].target_extensions.at("KHR_animation_pointer").Get("extras"));
  EXPECT_EQ(result.authoredSource.animations[0].samplers,authored.animations[0].samplers);
  const auto library=ParseAnimations(result.authoredSource);EXPECT_TRUE(library.diagnostics.empty());
  auto sampled=result.evaluatedSource;ApplyAnimationPose(sampled,SampleAnimation(library.clips[0],1));
  EXPECT_EQ(sampled.nodes[3].translation,sampled.nodes[1].translation);
  EXPECT_EQ(sampled.nodes[3].weights,sampled.nodes[1].weights);
  EXPECT_EQ(sampled.nodes[3].scale,sampled.nodes[1].scale);
  EXPECT_EQ(result.authoredSource.scenes[0].nodes,(std::vector<int>{0,3}));
  sameResources(result.authoredSource,authored);
}

TEST(GltfProjectedSubtreeClone,BindingOnlyOwnerKeepsSkinAndWeightsWithoutCopyingExternalVisualAttachments) {
  auto source=fixture();animation(source);skin(source);cameraAndLight(source,1);
  const auto input=source.animations[0].samplers[0].input;
  channel(source,source.animations[0],input,floats(source,{0,4,0,0,7,0},TINYGLTF_TYPE_VEC3,2),3,"translation");
  pointer(source,source.animations[0],input,floats(source,{.1f,.9f},TINYGLTF_TYPE_SCALAR,2),"/nodes/2/weights/0");
  const auto plan=PlanGltfProjectedSubtreeClone(source,source,projection({
      {1,-1,GltfCloneNodeRole::MeshBindingOnly,true,"Binding owner"},
      {2,-1,GltfCloneNodeRole::JointTransformOnly,false,"Joint ancestor"},
      {3,2,GltfCloneNodeRole::JointTransformOnly,false,"Joint"}}));
  const auto& result=plan.clone;ASSERT_EQ(result.nodes,(std::vector<int32_t>{-1,4,5,6}));
  EXPECT_EQ(result.clonedSkins,1u);EXPECT_EQ(result.clonedChannels,4u);
  const auto& owner=result.authoredSource.nodes[4];EXPECT_EQ(owner.mesh,0);EXPECT_EQ(owner.skin,1);
  EXPECT_EQ(owner.weights,source.nodes[1].weights);EXPECT_EQ(owner.camera,-1);EXPECT_EQ(owner.light,-1);
  EXPECT_FALSE(owner.extensions.contains("KHR_lights_punctual"));
  EXPECT_FALSE(result.contextOnly[1]);EXPECT_TRUE(plan.copyWeightTracks[1]);
  EXPECT_TRUE(result.contextOnly[2]);EXPECT_FALSE(plan.copyWeightTracks[2]);
  EXPECT_EQ(result.authoredSource.nodes[5].mesh,-1);EXPECT_TRUE(result.authoredSource.nodes[5].weights.empty());
  EXPECT_EQ(result.authoredSource.nodes[5].children,(std::vector<int>{6}));
  EXPECT_EQ(result.authoredSource.skins[1].joints,(std::vector<int>{6}));
  EXPECT_EQ(result.authoredSource.skins[1].skeleton,5);
  EXPECT_EQ(result.authoredSource.skins[1].inverseBindMatrices,source.skins[0].inverseBindMatrices);
  EXPECT_EQ(result.authoredSource.skins[1].extras,source.skins[0].extras);
  EXPECT_TRUE(ParseAnimations(result.authoredSource).diagnostics.empty());
  EXPECT_EQ(result.authoredSource.nodes[0],source.nodes[0]);sameResources(result.authoredSource,source);
}

TEST(GltfProjectedSubtreeClone,VisualGroupWithoutCopiedDrawsKeepsCameraLightButDropsUnusedMeshAndWeightTracks) {
  auto source=fixture();animation(source);skin(source);cameraAndLight(source,1);
  const auto plan=PlanGltfProjectedSubtreeClone(source,source,
      projection({{1,-1,GltfCloneNodeRole::Visual,false,"Group with moved-out primitives"}}));
  const auto& result=plan.clone;ASSERT_EQ(result.authoredSource.nodes.size(),5u);
  const auto& node=result.authoredSource.nodes[4];EXPECT_EQ(node.mesh,-1);EXPECT_EQ(node.skin,-1);
  EXPECT_TRUE(node.weights.empty());EXPECT_EQ(node.camera,0);EXPECT_EQ(node.light,0);
  EXPECT_EQ(node.extensions.at("KHR_lights_punctual"),source.nodes[1].extensions.at("KHR_lights_punctual"));
  EXPECT_FALSE(result.contextOnly[1]);EXPECT_FALSE(plan.retainMeshBindings[1]);EXPECT_FALSE(plan.copyWeightTracks[1]);
  EXPECT_EQ(result.clonedSkins,0u);EXPECT_EQ(result.clonedChannels,2u);
  const auto& channels=result.authoredSource.animations[0].channels;ASSERT_EQ(channels.size(),6u);
  EXPECT_EQ(channels[4].target_node,4);EXPECT_EQ(pointerText(channels[5]),"/nodes/4/scale/1");
  EXPECT_TRUE(ParseAnimations(result.authoredSource).diagnostics.empty());
  EXPECT_EQ(result.authoredSource.nodes[1],source.nodes[1]);sameResources(result.authoredSource,source);
}

TEST(GltfProjectedSubtreeClone,GraphOnlyProjectionDoesNotInventSourceGrowthOrChangeRawClips) {
  auto source=fixture();animation(source);auto live=source;live.nodes[1].translation={9,0,0};
  const auto result=PlanGltfProjectedSubtreeClone(live,source,projection({}));
  EXPECT_EQ(result.clone.authoredSource,source);EXPECT_EQ(result.clone.evaluatedSource,live);
  EXPECT_EQ(result.clone.nodes,(std::vector<int32_t>{-1,-1,-1}));
  EXPECT_EQ(result.clone.clonedNodes,0u);EXPECT_EQ(result.clone.clonedChannels,0u);
  EXPECT_TRUE(result.clone.contextRoots.empty());EXPECT_EQ(result.clone.originalChannelCounts,(std::vector<size_t>{4}));
}

TEST(GltfProjectedSubtreeClone,ExplicitProjectedForestCanDifferFromTheCanonicalSourceParenting) {
  auto source=fixture();const auto plan=PlanGltfProjectedSubtreeClone(source,source,projection({
      {1,2,GltfCloneNodeRole::Visual,true,"Moved child"},
      {2,-1,GltfCloneNodeRole::Visual,true,"Current graph parent"}}));
  EXPECT_EQ(plan.clone.nodes,(std::vector<int32_t>{-1,3,4}));
  EXPECT_EQ(plan.clone.authoredSource.nodes[4].children,(std::vector<int>{3}));
  EXPECT_EQ(plan.clone.authoredSource.nodes[3].children,std::vector<int>{});
  EXPECT_EQ(plan.clone.authoredSource.scenes[0].nodes,(std::vector<int>{0,4}));
  EXPECT_EQ(plan.clone.authoredSource.nodes[0],source.nodes[0]);
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneCatalog(plan.clone.authoredSource));
}

TEST(GltfProjectedSubtreeClone,RegistersSharedRootsOnlyInAffectedScenesAndPreservesExactDefaultAndMetadata) {
  auto source=fixture();source.scenes[0].name="Shared A";source.scenes[0].extras=Value(Object{{"index",Value(73)}});
  tinygltf::Scene shared;shared.name="Shared B";shared.nodes={0};source.scenes.push_back(shared);
  source.scenes.emplace_back();source.scenes[2].name="Empty";source.defaultScene=-1;
  auto selected=projection({{1,-1,GltfCloneNodeRole::Visual,true,"Shared copy"}});
  selected.affectedScenes={0,1};selected.destinationScene=1;
  const auto plan=PlanGltfProjectedSubtreeClone(source,source,selected);
  EXPECT_EQ(plan.clone.authoredSource.defaultScene,-1);EXPECT_EQ(plan.clone.evaluatedSource.defaultScene,-1);
  EXPECT_EQ(plan.clone.authoredSource.scenes[0].nodes,(std::vector<int>{0,3}));
  EXPECT_EQ(plan.clone.authoredSource.scenes[1].nodes,(std::vector<int>{0,3}));
  EXPECT_EQ(plan.clone.authoredSource.scenes[2],source.scenes[2]);
  EXPECT_EQ(plan.clone.authoredSource.scenes[0].extras,source.scenes[0].extras);
  EXPECT_EQ(plan.clone.authoredSource.scenes[0].name,source.scenes[0].name);
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneCatalog(plan.clone.authoredSource));
}

TEST(GltfProjectedSubtreeClone,RetainsInstancingAccessorsAndUnsignedCustomDataOnlyForActualMeshBindings) {
  auto source=fixture();const auto translations=floats(source,{0,0,0,2,0,0},TINYGLTF_TYPE_VEC3,2);
  const std::array<uint32_t,2> ids{16777217u,16777219u};std::vector<unsigned char> encoded(sizeof(ids));
  std::memcpy(encoded.data(),ids.data(),encoded.size());
  const auto custom=bytes(source,encoded,TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,TINYGLTF_TYPE_SCALAR,2);
  declare(source,"EXT_mesh_gpu_instancing");source.nodes[1].extensions["EXT_mesh_gpu_instancing"]=Value(Object{
      {"attributes",Value(Object{{"TRANSLATION",Value(translations)},{"_ID",Value(custom)}})}});
  const auto binding=PlanGltfProjectedSubtreeClone(source,source,
      projection({{1,-1,GltfCloneNodeRole::MeshBindingOnly,true,"Instance owner"}}));
  EXPECT_EQ(binding.clone.authoredSource.nodes[3].extensions.at("EXT_mesh_gpu_instancing"),
      source.nodes[1].extensions.at("EXT_mesh_gpu_instancing"));
  const auto data=ParseMeshGpuInstancing(binding.clone.authoredSource);ASSERT_EQ(data.nodes.size(),2u);
  sameResources(binding.clone.authoredSource,source);
  const auto stripped=PlanGltfProjectedSubtreeClone(source,source,
      projection({{1,-1,GltfCloneNodeRole::Visual,false,"No actual instances copied"}}));
  EXPECT_FALSE(stripped.clone.authoredSource.nodes[3].extensions.contains("EXT_mesh_gpu_instancing"));
  EXPECT_EQ(stripped.clone.authoredSource.nodes[3].mesh,-1);
  EXPECT_EQ(ParseMeshGpuInstancing(stripped.clone.authoredSource).nodes.size(),1u);
}

TEST(GltfProjectedSubtreeClone,PreservesOpaqueMetadataAndAuthoredFlagObjectPresenceWhileApplyingLocalFlags) {
  auto source=fixture();declare(source,"KHR_node_visibility");declare(source,"KHR_node_selectability");
  source.nodes[1].extensions["KHR_node_visibility"]=Value(Object{{"visible",Value(true)},
      {"extras",Value(Object{{"index",Value(71)}})},{"vendorNote",Value(std::string("keep"))}});
  source.nodes[1].extensions["KHR_node_selectability"]=Value(Object{{"extras",Value(Object{{"index",Value(93)}})}});
  source.nodes[1].extras=Value(Object{{"fooTexture",Value(Object{{"index",Value(188)}})}});
  auto requested=projection({{1,-1,GltfCloneNodeRole::Visual,true,"Renamed",false,true,false}});
  const auto plan=PlanGltfProjectedSubtreeClone(source,source,requested);
  const auto& node=plan.clone.authoredSource.nodes[3];
  EXPECT_FALSE(node.extensions.at("KHR_node_visibility").Get("visible").Get<bool>());
  EXPECT_EQ(node.extensions.at("KHR_node_visibility").Get("extras"),source.nodes[1].extensions.at("KHR_node_visibility").Get("extras"));
  EXPECT_EQ(node.extensions.at("KHR_node_selectability"),source.nodes[1].extensions.at("KHR_node_selectability"));
  EXPECT_FALSE(node.extensions.at("KHR_node_hoverability").Get("hoverable").Get<bool>());
  EXPECT_EQ(node.extras,source.nodes[1].extras);EXPECT_EQ(source.nodes[1].name,"Selected");
  EXPECT_NE(std::ranges::find(plan.clone.authoredSource.extensionsUsed,"KHR_node_hoverability"),
      plan.clone.authoredSource.extensionsUsed.end());
}

TEST(GltfProjectedSubtreeClone,RejectsMalformedProjectionAndMissingSkinClosureWithoutInputMutation) {
  auto source=fixture();skin(source);const auto before=source;
  const auto expect=[&](GltfSubtreeCloneProjection p) {
    EXPECT_THROW((void)PlanGltfProjectedSubtreeClone(source,source,p),std::runtime_error);
    EXPECT_EQ(source,before);
  };
  expect(projection({{1,-1,GltfCloneNodeRole::Visual,true,"Missing joints"}}));
  expect(projection({{1,-1,GltfCloneNodeRole::Visual,false,"First"},{1,-1,GltfCloneNodeRole::Visual,false,"Duplicate"}}));
  expect(projection({{9,-1,GltfCloneNodeRole::Visual,false,"Invalid"}}));
  expect(projection({{1,2,GltfCloneNodeRole::Visual,false,"Missing parent"}}));
  expect(projection({{1,1,GltfCloneNodeRole::Visual,false,"Self"}}));
  expect(projection({{1,2,GltfCloneNodeRole::Visual,false,"Cycle"},{2,1,GltfCloneNodeRole::JointTransformOnly,false,"Cycle"}}));
  expect(projection({{1,-1,GltfCloneNodeRole::JointTransformOnly,true,"Bad role binding"}}));
  expect(projection({{1,-1,GltfCloneNodeRole::MeshBindingOnly,false,"Missing binding"}}));
  auto scenes=projection({});scenes.affectedScenes={0,0};expect(scenes);
  scenes.affectedScenes={};expect(scenes);
  const std::array<int32_t,1> restricted{1};
  EXPECT_THROW((void)PlanGltfProjectedSubtreeClone(source,source,
      projection({{1,-1,GltfCloneNodeRole::Visual,false,"Restricted"}}),{restricted}),std::runtime_error);
  EXPECT_EQ(source,before);
}

TEST(GltfProjectedSubtreeClone,BoundsInputsBothOutputCopiesChannelsAndProjectionBeforePublication) {
  auto source=fixture();animation(source);const auto before=source;
  const auto requested=projection({{1,-1,GltfCloneNodeRole::Visual,true,"Copy"}});
  const auto valid=PlanGltfProjectedSubtreeClone(source,source,requested);
  for(const auto kind:{0,1,2,3,4,5}) {
    GltfMergeLimits limits;
    if(kind==0) limits.maxResidentBytes=valid.clone.peakResidentBytes-1;
    if(kind==1) limits.maxMetadataBytes=valid.clone.metadataBytes-1;
    if(kind==2) limits.maxAnimationDecodedBytes=valid.clone.animationDecodedBytes-1;
    if(kind==3) limits.maxClonedChannels=2;
    if(kind==4) limits.maxPlacementNodes=0;
    if(kind==5) limits.maxRootReferences=1;
    EXPECT_THROW((void)PlanGltfProjectedSubtreeClone(source,source,requested,{},limits),std::runtime_error)<<kind;
    EXPECT_EQ(source,before);
  }
}

TEST(GltfProjectedSubtreeClone,RepeatedProjectionsAppendOnlyNewSourceIdsAndPreserveEarlierChannelPrefixes) {
  auto source=fixture();animation(source);
  const auto first=PlanGltfProjectedSubtreeClone(source,source,
      projection({{1,-1,GltfCloneNodeRole::Visual,true,"First copy"}}));
  const auto second=PlanGltfProjectedSubtreeClone(first.clone.evaluatedSource,first.clone.authoredSource,
      projection({{3,-1,GltfCloneNodeRole::Visual,true,"Second copy"}}));
  EXPECT_EQ(second.clone.nodes,(std::vector<int32_t>{-1,-1,-1,4}));
  EXPECT_EQ(second.clone.clonedChannels,3u);EXPECT_EQ(second.clone.authoredSource.animations[0].channels.size(),10u);
  for(size_t i=0;i<first.clone.authoredSource.animations[0].channels.size();++i)
    EXPECT_EQ(second.clone.authoredSource.animations[0].channels[i],first.clone.authoredSource.animations[0].channels[i]);
  EXPECT_EQ(second.clone.authoredSource.nodes[3],first.clone.authoredSource.nodes[3]);
  EXPECT_EQ(second.clone.authoredSource.scenes[0].nodes,(std::vector<int>{0,3,4}));
  auto sampled=second.clone.evaluatedSource;const auto library=ParseAnimations(second.clone.authoredSource);
  ApplyAnimationPose(sampled,SampleAnimation(library.clips[0],1));
  EXPECT_EQ(sampled.nodes[1].translation,sampled.nodes[4].translation);
  EXPECT_EQ(sampled.nodes[1].weights,sampled.nodes[4].weights);sameResources(second.clone.authoredSource,source);
}

TEST(GltfProjectedSubtreeClone,RejectsUnknownReferencesAndUnsupportedPointersInsteadOfGuessingTheirMeaning) {
  auto source=fixture();declare(source,"VENDOR_node_link");
  source.nodes[1].extensions["VENDOR_node_link"]=Value(Object{{"node",Value(2)}});
  const auto requested=projection({{1,-1,GltfCloneNodeRole::Visual,true,"Copy"}});
  EXPECT_THROW((void)PlanGltfProjectedSubtreeClone(source,source,requested),std::runtime_error);
  source=fixture();animation(source);const auto input=source.animations[0].samplers[0].input;
  pointer(source,source.animations[0],input,floats(source,{1,2},TINYGLTF_TYPE_SCALAR,2),"/nodes/1/extras/value");
  EXPECT_THROW((void)PlanGltfProjectedSubtreeClone(source,source,requested),std::runtime_error);
}
} // namespace

