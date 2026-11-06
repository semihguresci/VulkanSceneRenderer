#include <Container/geometry/GltfNodeExportCompaction.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfSceneCatalog.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

namespace {
using namespace container::geometry::gltf;
using Value = tinygltf::Value;
using Object = Value::Object;
using Array = Value::Array;

int floats(tinygltf::Model& model, std::initializer_list<float> values, int type, size_t count) {
  tinygltf::Buffer buffer; buffer.data.resize(values.size()*sizeof(float));
  std::memcpy(buffer.data.data(),values.begin(),buffer.data.size());
  buffer.extras = Value(Object{{"index",Value(709)}});
  tinygltf::BufferView view; view.buffer = int(model.buffers.size()); view.byteLength = buffer.data.size();
  tinygltf::Accessor accessor; accessor.sparse = {}; accessor.bufferView = int(model.bufferViews.size());
  accessor.type = type; accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT; accessor.count = count;
  if (type == TINYGLTF_TYPE_SCALAR && values.size()) {
    const auto extrema = std::minmax_element(values.begin(), values.end());
    accessor.minValues = {double(*extrema.first)}; accessor.maxValues = {double(*extrema.second)};
  }
  const auto result = int(model.accessors.size());
  model.buffers.push_back(std::move(buffer)); model.bufferViews.push_back(view); model.accessors.push_back(accessor);
  return result;
}
void declare(tinygltf::Model& model, const std::string& name) {
  if (std::ranges::find(model.extensionsUsed,name) == model.extensionsUsed.end()) model.extensionsUsed.push_back(name);
}
tinygltf::Model fixture() {
  tinygltf::Model model; model.asset.version = "2.0";
  model.asset.generator = "immutable compaction fixture";
  const auto positions = floats(model,{0,0,0, 1,0,0, 0,1,0},TINYGLTF_TYPE_VEC3,3);
  model.images.resize(1); model.images[0].as_is = true; model.images[0].mimeType = "image/png";
  model.images[0].image = {17,23,41,255}; model.images[0].extras = Value(Object{{"node",Value(4)}});
  model.samplers.resize(1); model.samplers[0].extras = Value(Object{{"index",Value(4)}});
  model.textures.resize(1); model.textures[0].source = 0; model.textures[0].sampler = 0;
  model.textures[0].extras = Value(Object{{"nodes",Value(Array{Value(4)})}});
  model.materials.resize(1); model.materials[0].pbrMetallicRoughness.baseColorTexture.index = 0;
  model.materials[0].extras = Value(Object{{"fakeNode",Value(Object{{"index",Value(4)}})}});
  tinygltf::Primitive primitive; primitive.mode = TINYGLTF_MODE_TRIANGLES;
  primitive.attributes["POSITION"] = positions; primitive.material = 0;
  model.meshes.resize(1); model.meshes[0].primitives.push_back(primitive);
  declare(model,"KHR_materials_variants");
  model.extensions["KHR_materials_variants"] = Value(Object{{"variants",Value(Array{
      Value(Object{{"name",Value(std::string("Alternate"))},{"extras",Value(Object{{"node",Value(3)}})}})})}});
  model.meshes[0].primitives[0].extensions["KHR_materials_variants"] = Value(Object{{"mappings",Value(Array{
      Value(Object{{"material",Value(0)},{"variants",Value(Array{Value(0)})},
          {"extras",Value(Object{{"node",Value(3)}})}})})}});
  model.nodes.resize(6);
  model.nodes[0].name = "Shared root"; model.nodes[0].children = {1,3}; model.nodes[0].translation = {2,3,4};
  model.nodes[1].name = "Retired branch"; model.nodes[1].children = {2}; model.nodes[1].mesh = 0;
  model.nodes[2].name = "Retired child"; model.nodes[2].mesh = 0;
  model.nodes[3].name = "Survivor"; model.nodes[3].mesh = 0; model.nodes[3].translation = {1,2,3};
  model.nodes[3].extras = Value(Object{{"node",Value(3)}, {"nodes",Value(Array{Value(1),Value(3)})}});
  model.nodes[4].name = "Other scene root"; model.nodes[4].children = {5}; model.nodes[4].scale = {2,3,4};
  model.nodes[5].name = "Other scene survivor"; model.nodes[5].mesh = 0;
  model.scenes.resize(3); model.scenes[0].nodes = {0}; model.scenes[1].nodes = {4,0};
  model.scenes[0].name = "A"; model.scenes[1].name = "Shared B"; model.scenes[2].name = "Empty";
  model.scenes[1].extras = Value(Object{{"node",Value(4)}});
  model.defaultScene = 1; model.extras = Value(Object{{"node",Value(4)}});
  return model;
}
void channel(tinygltf::Model& model, tinygltf::Animation& animation, int node,
             std::string path, std::initializer_list<float> values, int type, std::string pointer = {}) {
  tinygltf::AnimationSampler sampler;
  sampler.input = floats(model,{0,1},TINYGLTF_TYPE_SCALAR,2);
  sampler.output = floats(model,values,type,2); sampler.interpolation = "LINEAR";
  tinygltf::AnimationChannel target; target.sampler = int(animation.samplers.size());
  target.target_node = node; target.target_path = std::move(path);
  target.extras = Value(Object{{"node",Value(3)}});
  target.target_extras = Value(Object{{"node",Value(3)}});
  if (!pointer.empty()) {
    declare(model,"KHR_animation_pointer");
    target.target_extensions["KHR_animation_pointer"] = Value(Object{{"pointer",Value(std::move(pointer))},
        {"extras",Value(Object{{"node",Value(3)}})}});
    target.target_extensions_json_string = "unchanged original pointer metadata";
  }
  animation.samplers.push_back(sampler); animation.channels.push_back(target);
}
void animate(tinygltf::Model& model) {
  tinygltf::Animation clip; clip.name = "Retained metadata clip";
  clip.extras = Value(Object{{"node",Value(3)}});
  channel(model,clip,1,"translation",{0,0,0, 1,2,3},TINYGLTF_TYPE_VEC3);
  channel(model,clip,3,"translation",{1,2,3, 4,5,6},TINYGLTF_TYPE_VEC3);
  channel(model,clip,-1,"pointer",{2,3},TINYGLTF_TYPE_SCALAR,"/nodes/3/scale/1");
  channel(model,clip,-1,"pointer",{0,1},TINYGLTF_TYPE_SCALAR,"/nodes/2/translation/0");
  channel(model,clip,-1,"pointer",{.2f,.8f},TINYGLTF_TYPE_SCALAR,
      "/materials/0/pbrMetallicRoughness/roughnessFactor");
  model.animations.push_back(clip);
}
std::string pointerText(const tinygltf::AnimationChannel& channel) {
  return channel.target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>();
}
void expectResources(const tinygltf::Model& a, const tinygltf::Model& b) {
  EXPECT_EQ(a.buffers,b.buffers); EXPECT_EQ(a.bufferViews,b.bufferViews); EXPECT_EQ(a.accessors,b.accessors);
  EXPECT_EQ(a.images,b.images); EXPECT_EQ(a.samplers,b.samplers); EXPECT_EQ(a.textures,b.textures);
  EXPECT_EQ(a.materials,b.materials); EXPECT_EQ(a.meshes,b.meshes); EXPECT_EQ(a.cameras,b.cameras);
  EXPECT_EQ(a.lights,b.lights); EXPECT_EQ(a.extensions,b.extensions);
  EXPECT_EQ(a.extensionsUsed,b.extensionsUsed); EXPECT_EQ(a.extensionsRequired,b.extensionsRequired);
}

TEST(GltfNodeExportCompaction,ProjectsAllScenesInSurvivorOrderAndNeverRewritesOpaqueMetadata) {
  const auto source = fixture(); const auto before = source;
  const auto plan = CompactGltfExportNodes(source,std::array<int32_t,2>{2,1});
  EXPECT_EQ(source,before); EXPECT_EQ(plan.oldToNewNodes,(std::vector<int32_t>{0,-1,-1,1,2,3}));
  EXPECT_EQ(plan.newToOldNodes,(std::vector<int32_t>{0,3,4,5})); EXPECT_EQ(plan.retiredNodes,2u);
  ASSERT_EQ(plan.model.nodes.size(),4u); EXPECT_EQ(plan.model.nodes[0].children,(std::vector<int>{1}));
  EXPECT_EQ(plan.model.nodes[2].children,(std::vector<int>{3}));
  EXPECT_EQ(plan.model.nodes[1].translation,source.nodes[3].translation);
  EXPECT_EQ(plan.model.nodes[1].extras,source.nodes[3].extras);
  EXPECT_EQ(plan.model.scenes[0].nodes,(std::vector<int>{0}));
  EXPECT_EQ(plan.model.scenes[1].nodes,(std::vector<int>{2,0}));
  EXPECT_TRUE(plan.model.scenes[2].nodes.empty()); EXPECT_EQ(plan.model.scenes[1].extras,source.scenes[1].extras);
  EXPECT_EQ(plan.model.defaultScene,1); EXPECT_EQ(plan.model.extras,source.extras); expectResources(source,plan.model);
  EXPECT_EQ(container::scene::BuildGltfSceneCatalog(plan.model).entries.size(),3u);
}

TEST(GltfNodeExportCompaction,DropsOnlyRetiredChannelsAndRetargetsNodePointersWithoutMovingSamplers) {
  auto source = fixture(); animate(source); const auto before = source;
  const auto plan = CompactGltfExportNodes(source,std::array<int32_t,2>{1,2});
  EXPECT_EQ(source,before); EXPECT_EQ(plan.droppedChannels,2u); expectResources(source,plan.model);
  ASSERT_EQ(plan.model.animations.size(),1u); const auto& clip = plan.model.animations[0];
  EXPECT_EQ(clip.samplers,source.animations[0].samplers); EXPECT_EQ(clip.name,source.animations[0].name);
  EXPECT_EQ(clip.extras,source.animations[0].extras); ASSERT_EQ(clip.channels.size(),3u);
  EXPECT_EQ(clip.channels[0].target_node,1); EXPECT_EQ(clip.channels[0].sampler,1);
  EXPECT_EQ(pointerText(clip.channels[1]),"/nodes/1/scale/1"); EXPECT_EQ(clip.channels[1].sampler,2);
  EXPECT_TRUE(clip.channels[1].target_extensions_json_string.empty());
  EXPECT_EQ(clip.channels[1].extras,source.animations[0].channels[2].extras);
  EXPECT_EQ(clip.channels[1].target_extras,source.animations[0].channels[2].target_extras);
  EXPECT_EQ(clip.channels[1].target_extensions.at("KHR_animation_pointer").Get("extras"),
      source.animations[0].channels[2].target_extensions.at("KHR_animation_pointer").Get("extras"));
  EXPECT_EQ(clip.channels[2].target_extensions,source.animations[0].channels[4].target_extensions);
  EXPECT_EQ(clip.channels[2].target_extensions_json_string,source.animations[0].channels[4].target_extensions_json_string);
  const auto library = ParseAnimations(plan.model); ASSERT_TRUE(library.diagnostics.empty());
  const auto pose = SampleAnimation(library.clips[0],1);
  auto posed = plan.model; ApplyAnimationPose(posed,pose);
  EXPECT_EQ(posed.nodes[1].translation,(std::vector<double>{4,5,6}));
  EXPECT_EQ(posed.nodes[1].scale,(std::vector<double>{1,3,1}));
  EXPECT_NEAR(posed.materials[0].pbrMetallicRoughness.roughnessFactor,.8,1e-6);
}

TEST(GltfNodeExportCompaction,RetiresAnEntireClipTargetSetButKeepsItsMetadataAndUnusedSamplerResources) {
  auto source = fixture(); tinygltf::Animation clip; clip.name = "Now empty";
  clip.extras = Value(Object{{"keep",Value(true)}});
  channel(source,clip,2,"scale",{1,1,1, 2,2,2},TINYGLTF_TYPE_VEC3); source.animations.push_back(clip);
  const auto plan = CompactGltfExportNodes(source,std::array<int32_t,2>{1,2});
  ASSERT_EQ(plan.model.animations.size(),1u); EXPECT_TRUE(plan.model.animations[0].channels.empty());
  EXPECT_EQ(plan.model.animations[0].samplers,clip.samplers); EXPECT_EQ(plan.model.animations[0].extras,clip.extras);
  EXPECT_EQ(plan.model.accessors,source.accessors); EXPECT_EQ(plan.model.buffers,source.buffers);
}

TEST(GltfNodeExportCompaction,RemapsSurvivingSkinsAndDropsOnlyAffectedSkinsWithoutSurvivingOwners) {
  auto source = fixture(); source.skins.resize(3);
  source.skins[0].joints = {2}; source.skins[0].skeleton = 1; source.nodes[1].skin = 0;
  source.skins[1].joints = {5}; source.skins[1].skeleton = 4; source.nodes[3].skin = 1;
  source.skins[1].extras = Value(Object{{"node",Value(5)}});
  source.skins[2].joints = {0}; // Unused, unaffected skin must keep its definition.
  const auto plan = CompactGltfExportNodes(source,std::array<int32_t,2>{1,2});
  EXPECT_EQ(plan.oldToNewSkins,(std::vector<int32_t>{-1,0,1})); EXPECT_EQ(plan.droppedSkins,1u);
  ASSERT_EQ(plan.model.skins.size(),2u); EXPECT_EQ(plan.model.skins[0].joints,(std::vector<int>{3}));
  EXPECT_EQ(plan.model.skins[0].skeleton,2); EXPECT_EQ(plan.model.nodes[1].skin,0);
  EXPECT_EQ(plan.model.skins[0].extras,source.skins[1].extras); EXPECT_EQ(plan.model.skins[1].joints,(std::vector<int>{0}));
  expectResources(source,plan.model);
}

TEST(GltfNodeExportCompaction,RejectsDeletedJointsOrSkeletonForAnySurvivingOwner) {
  auto source = fixture(); source.skins.resize(1); source.skins[0].joints = {2}; source.nodes[3].skin = 0;
  auto before = source;
  EXPECT_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,2>{1,2}),std::runtime_error);
  EXPECT_EQ(source,before);
  source.skins[0].joints = {5}; source.skins[0].skeleton = 1; before = source;
  EXPECT_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,2>{1,2}),std::runtime_error);
  EXPECT_EQ(source,before);
}

TEST(GltfNodeExportCompaction,PreservesCameraLightInstancingAndAuthoredFlagsWithoutRenumberingResources) {
  auto source = fixture(); source.cameras.resize(1); source.cameras[0].type = "perspective";
  source.cameras[0].perspective.yfov = .7; source.cameras[0].perspective.znear = .1;
  source.lights.resize(1); source.lights[0].type = "point"; source.lights[0].intensity = 5;
  source.nodes[3].camera = 0; source.nodes[3].light = 0;
  declare(source,"KHR_lights_punctual"); declare(source,"EXT_mesh_gpu_instancing"); declare(source,"KHR_node_visibility");
  source.nodes[3].extensions["KHR_lights_punctual"] = Value(Object{{"light",Value(0)},{"extras",Value(Object{{"node",Value(3)}})}});
  source.nodes[3].extensions["KHR_node_visibility"] = Value(Object{{"visible",Value(true)}, {"extras",Value(Object{{"node",Value(3)}})}});
  const auto instance = floats(source,{0,0,0, 2,0,0},TINYGLTF_TYPE_VEC3,2);
  source.nodes[3].extensions["EXT_mesh_gpu_instancing"] = Value(Object{{"attributes",Value(Object{{"TRANSLATION",Value(instance)}})}});
  const auto plan = CompactGltfExportNodes(source,std::array<int32_t,2>{1,2});
  EXPECT_EQ(plan.model.nodes[1].camera,0); EXPECT_EQ(plan.model.nodes[1].light,0);
  EXPECT_EQ(plan.model.nodes[1].extensions,source.nodes[3].extensions); expectResources(source,plan.model);
}

TEST(GltfNodeExportCompaction,NoRetirementIsExactIncludingPointerMetadataAndMissingDefaultScene) {
  auto source = fixture(); animate(source); source.defaultScene = -1;
  const auto plan = CompactGltfExportNodes(source,{});
  EXPECT_EQ(plan.model,source); EXPECT_EQ(plan.model.animations[0].channels[2].target_extensions_json_string,
      source.animations[0].channels[2].target_extensions_json_string);
  EXPECT_EQ(plan.oldToNewNodes,(std::vector<int32_t>{0,1,2,3,4,5}));
  EXPECT_EQ(plan.newToOldNodes,plan.oldToNewNodes);
}

TEST(GltfNodeExportCompaction,CanRetireEveryNodeAndRetainNamedEmptyScenesOrAnImplicitScene) {
  auto source = fixture();
  const std::array<int32_t,6> all{0,1,2,3,4,5};
  auto plan = CompactGltfExportNodes(source,all);
  EXPECT_TRUE(plan.model.nodes.empty()); ASSERT_EQ(plan.model.scenes.size(),3u);
  for (size_t s=0;s<3;++s) { EXPECT_TRUE(plan.model.scenes[s].nodes.empty()); EXPECT_EQ(plan.model.scenes[s].name,source.scenes[s].name); }
  EXPECT_EQ(plan.model.defaultScene,1); expectResources(source,plan.model);
  source.scenes.clear(); source.defaultScene = -1;
  plan = CompactGltfExportNodes(source,all);
  EXPECT_TRUE(plan.model.nodes.empty()); EXPECT_TRUE(plan.model.scenes.empty()); EXPECT_EQ(plan.model.defaultScene,-1);
}

TEST(GltfNodeExportCompaction,RejectsMalformedRetirementAndNonClosedSubtreesWithoutMutation) {
  const auto source = fixture(); const auto before = source;
  for (const auto& nodes : {std::vector<int32_t>{1},std::vector<int32_t>{1,1},
      std::vector<int32_t>{-1},std::vector<int32_t>{6},std::vector<int32_t>{0,1,2}})
    EXPECT_THROW((void)CompactGltfExportNodes(source,nodes),std::runtime_error);
  EXPECT_EQ(source,before);
}

TEST(GltfNodeExportCompaction,RejectsUnknownReferenceSchemasButNeverInspectsExtrasAsReferences) {
  auto source = fixture(); source.nodes[3].extras = Value(Object{{"extensions",Value(Object{
      {"VENDOR_arbitrary",Value(Object{{"nodes",Value(Array{Value(1)})}})}})}});
  EXPECT_NO_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,2>{1,2}));
  declare(source,"VENDOR_node_reference");
  source.nodes[3].extensions["VENDOR_node_reference"] = Value(Object{{"node",Value(1)}});
  const auto before = source;
  EXPECT_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,2>{1,2}),std::runtime_error);
  EXPECT_EQ(source,before);
}

TEST(GltfNodeExportCompaction,RejectsUnsupportedNodePointersEvenWhenTheirTargetWouldBeRetired) {
  auto source = fixture(); tinygltf::Animation clip;
  channel(source,clip,-1,"pointer",{0,1},TINYGLTF_TYPE_SCALAR,"/nodes/1/mesh");
  source.animations.push_back(clip); const auto before = source;
  EXPECT_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,2>{1,2}),std::runtime_error);
  EXPECT_EQ(source,before);
}

TEST(GltfNodeExportCompaction,RejectsCountPayloadMetadataAndDecodeBudgetsBeforeCopyingOutput) {
  auto source = fixture(); animate(source); const auto before = source;
  const auto reject = [&](GltfMergeLimits limits) {
    EXPECT_THROW((void)CompactGltfExportNodes(source,std::array<int32_t,2>{1,2},limits),std::runtime_error);
    EXPECT_EQ(source,before);
  };
  GltfMergeLimits limits; limits.maxNodes = 5; reject(limits);
  limits = {}; limits.maxPrimitives = 0; reject(limits);
  limits = {}; limits.maxRootReferences = 2; reject(limits);
  limits = {}; limits.maxReferences = 1; reject(limits);
  limits = {}; limits.maxResidentBytes = 1; reject(limits);
  limits = {}; limits.maxMetadataBytes = 1; reject(limits);
  limits = {}; limits.maxAccessorDecodedBytes = 1; reject(limits);
  limits = {}; limits.maxAnimationDecodedBytes = 1; reject(limits);
  limits = {}; limits.maxAnimationChannels = 1; reject(limits);
}

TEST(GltfNodeExportCompaction,RejectsInvalidForestJointAndNumericMetadataBeforePublication) {
  auto source = fixture(); source.nodes[3].children = {0};
  EXPECT_THROW((void)CompactGltfExportNodes(source,{}),std::exception);
  source = fixture(); source.skins.resize(1); source.skins[0].joints = {5,5};
  EXPECT_THROW((void)CompactGltfExportNodes(source,{}),std::runtime_error);
  source = fixture(); source.skins.resize(1); source.skins[0].joints = {6};
  EXPECT_THROW((void)CompactGltfExportNodes(source,{}),std::runtime_error);
  source = fixture(); source.nodes[3].translation = {std::numeric_limits<double>::infinity(),0,0};
  EXPECT_THROW((void)CompactGltfExportNodes(source,{}),std::runtime_error);
  source = fixture(); source.nodes[3].lods = {1};
  EXPECT_THROW((void)CompactGltfExportNodes(source,{}),std::runtime_error);
}

TEST(GltfNodeExportCompaction,RepeatedCompactionUsesTheSuppliedIndexSpaceAndPreservesSharedResources) {
  const auto source = fixture();
  const auto first = CompactGltfExportNodes(source,std::array<int32_t,2>{1,2});
  const auto second = CompactGltfExportNodes(first.model,std::array<int32_t,2>{2,3});
  EXPECT_EQ(second.oldToNewNodes,(std::vector<int32_t>{0,1,-1,-1}));
  EXPECT_EQ(second.newToOldNodes,(std::vector<int32_t>{0,1}));
  ASSERT_EQ(second.model.nodes.size(),2u); EXPECT_EQ(second.model.nodes[1].name,"Survivor");
  EXPECT_EQ(second.model.nodes[0].children,(std::vector<int>{1}));
  EXPECT_EQ(second.model.scenes[1].nodes,(std::vector<int>{0}));
  EXPECT_EQ(second.model.defaultScene,source.defaultScene); expectResources(source,second.model);
}
} // namespace
