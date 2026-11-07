#include <Container/geometry/GltfSubtreeClone.h>
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

TEST(GltfSubtreeClone, SharesResourcesAndPreservesRetainedIdentitiesAndInputs) {
  const auto source = fixture(), before = source;
  const auto plan = PlanGltfSubtreeClone(source, source, 1);
  EXPECT_EQ(source, before);
  EXPECT_EQ(plan.nodes, (std::vector<int32_t>{-1,3,-1}));
  EXPECT_EQ(plan.root, 3); EXPECT_EQ(plan.parent, 0);
  EXPECT_EQ(plan.affectedScenes, std::vector<size_t>{0});
  EXPECT_EQ(plan.authoredSource.nodes[0].children, (std::vector<int>{1,2,3}));
  EXPECT_EQ(plan.authoredSource.nodes[1], source.nodes[1]);
  EXPECT_EQ(plan.authoredSource.nodes[2], source.nodes[2]);
  EXPECT_EQ(plan.authoredSource.nodes[3].mesh, 0);
  EXPECT_EQ(plan.authoredSource.buffers, source.buffers);
  EXPECT_EQ(plan.authoredSource.accessors, source.accessors);
  EXPECT_EQ(plan.authoredSource.meshes, source.meshes);
  EXPECT_EQ(plan.authoredSource.materials, source.materials);
  EXPECT_EQ(plan.authoredSource.textures, source.textures);
  EXPECT_EQ(plan.authoredSource.images, source.images);
  EXPECT_EQ(plan.authoredSource.scenes, source.scenes);
  EXPECT_EQ(plan.authoredSource, plan.evaluatedSource);
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneCatalog(plan.authoredSource));
}

TEST(GltfSubtreeClone, KeepsAuthoredRestDistinctFromCurrentPoseAndClonesOnlyTypedNodeTracks) {
  auto authored = fixture(); animation(authored);
  auto live = authored;
  ApplyAnimationPose(live, SampleAnimation(ParseAnimations(authored).clips[0], .5));
  const auto oldLive = live, oldAuthored = authored;
  const auto plan = PlanGltfSubtreeClone(live, authored, 1, {.rootName = "Independent copy"});
  EXPECT_EQ(live, oldLive); EXPECT_EQ(authored, oldAuthored);
  EXPECT_EQ(plan.originalChannelCounts, std::vector<size_t>{4});
  EXPECT_EQ(plan.clonedChannels, 3u);
  const auto& animation = plan.authoredSource.animations[0];
  ASSERT_EQ(animation.channels.size(), 7u);
  EXPECT_EQ(animation.samplers, authored.animations[0].samplers);
  for (size_t c = 0; c < 4; ++c) {
    EXPECT_EQ(animation.channels[c], authored.animations[0].channels[c]);
    EXPECT_EQ(animation.channels[c].target_extensions, authored.animations[0].channels[c].target_extensions);
    EXPECT_EQ(animation.channels[c].target_extensions_json_string,
              authored.animations[0].channels[c].target_extensions_json_string);
  }
  EXPECT_EQ(animation.channels[4].target_node, plan.root);
  EXPECT_EQ(pointerText(animation.channels[5]), "/nodes/3/scale/1");
  EXPECT_EQ(pointerText(animation.channels[6]), "/nodes/3/weights/0");
  EXPECT_EQ(animation.channels[5].target_extensions.at("KHR_animation_pointer").Get("extras"),
            authored.animations[0].channels[1].target_extensions.at("KHR_animation_pointer").Get("extras"));
  EXPECT_TRUE(animation.channels[5].target_extensions_json_string.empty());
  EXPECT_EQ(plan.authoredSource.nodes[3].weights, authored.nodes[1].weights);
  EXPECT_EQ(plan.evaluatedSource.nodes[3].weights, live.nodes[1].weights);
  EXPECT_EQ(plan.authoredSource.nodes[3].translation, authored.nodes[1].translation);
  EXPECT_EQ(plan.evaluatedSource.nodes[3].translation, live.nodes[1].translation);
  EXPECT_EQ(plan.authoredSource.nodes[3].name, "Independent copy");
  auto sampled = plan.evaluatedSource;
  const auto library = ParseAnimations(plan.authoredSource);
  ASSERT_EQ(library.clips[0].tracks.size(), 7u);
  ApplyAnimationPose(sampled, SampleAnimation(library.clips[0], 1));
  EXPECT_EQ(sampled.nodes[1].translation, sampled.nodes[3].translation);
  EXPECT_EQ(sampled.nodes[1].weights, sampled.nodes[3].weights);
  EXPECT_EQ(sampled.nodes[1].scale, sampled.nodes[3].scale);
  EXPECT_EQ(plan.authoredSource.accessors, authored.accessors);
  EXPECT_EQ(plan.authoredSource.buffers, authored.buffers);
}

TEST(GltfSubtreeClone, ClonesExternalSkinAncestorsWithoutTheirVisualAttachmentsAndPreservesWorldPoints) {
  auto authored = fixture(); skin(authored);
  authored.nodes[0].scale = {-2,3,.5};
  tinygltf::Light light; light.type = "point"; authored.lights.push_back(light);
  tinygltf::Camera camera; camera.type = "perspective"; camera.perspective.yfov = .7; camera.perspective.znear = .1;
  authored.cameras.push_back(camera);
  authored.nodes[2].light = 0; authored.nodes[2].camera = 0;
  declare(authored, "KHR_lights_punctual");
  authored.extensions["KHR_lights_punctual"] = Value(Object{{"lights", Value(Array{
      Value(Object{{"type", Value(std::string("point"))}})})}});
  authored.nodes[2].extensions["KHR_lights_punctual"] = Value(Object{{"light", Value(0)}});
  auto live = authored; live.nodes[3].translation = {0,7,0};
  const auto plan = PlanGltfSubtreeClone(live, authored, 1);
  ASSERT_EQ(plan.clonedNodes, 4u); ASSERT_EQ(plan.clonedSkins, 1u);
  EXPECT_EQ(plan.root, 5); EXPECT_EQ(plan.parent, 0);
  EXPECT_EQ(plan.contextRoots, std::vector<int32_t>{4});
  EXPECT_EQ(plan.authoredSource.nodes[0].children, (std::vector<int>{1,2,5}));
  EXPECT_EQ(plan.authoredSource.nodes[4].children, std::vector<int>{6});
  EXPECT_EQ(plan.authoredSource.nodes[6].children, std::vector<int>{7});
  EXPECT_EQ(plan.authoredSource.nodes[6].mesh, -1);
  EXPECT_EQ(plan.authoredSource.nodes[6].camera, -1);
  EXPECT_EQ(plan.authoredSource.nodes[6].light, -1);
  EXPECT_FALSE(plan.authoredSource.nodes[6].extensions.contains("KHR_lights_punctual"));
  EXPECT_EQ(plan.authoredSource.nodes[5].skin, 1);
  EXPECT_EQ(plan.authoredSource.skins[1].joints, std::vector<int>{7});
  EXPECT_EQ(plan.authoredSource.skins[1].skeleton, 6);
  EXPECT_EQ(plan.authoredSource.skins[1].inverseBindMatrices, authored.skins[0].inverseBindMatrices);
  EXPECT_EQ(plan.authoredSource.skins[1].extras, authored.skins[0].extras);
  EXPECT_EQ(plan.authoredSource.scenes[0].nodes, (std::vector<int>{0,4}));
  const auto authoredWorld = worlds(plan.authoredSource), liveWorld = worlds(plan.evaluatedSource);
  EXPECT_EQ(authoredWorld[1], authoredWorld[5]); EXPECT_EQ(authoredWorld[3], authoredWorld[7]);
  EXPECT_EQ(liveWorld[1], liveWorld[5]); EXPECT_EQ(liveWorld[3], liveWorld[7]);
  // One weight=1 and identity inverse bind: final world = jointWorld * bind
  // position, independently of the duplicate mesh's hierarchy placement.
  const auto point = glm::dvec4(1,2,3,1);
  EXPECT_EQ(authoredWorld[7] * point, glm::dvec4(-3,18,1.5,1));
  EXPECT_EQ(liveWorld[7] * point, glm::dvec4(-3,27,1.5,1));
  EXPECT_EQ(plan.authoredSource.skins[0], authored.skins[0]);
  EXPECT_EQ(plan.authoredSource.cameras, authored.cameras);
  EXPECT_EQ(plan.authoredSource.lights, authored.lights);
}

TEST(GltfSubtreeClone, ContextAnimationTracksAreClonedButTheirMorphWeightsAreNot) {
  auto source = fixture(); animation(source); skin(source);
  const auto input = source.animations[0].samplers[0].input;
  channel(source, source.animations[0], input,
      floats(source, {0,4,0, 0,7,0}, TINYGLTF_TYPE_VEC3, 2), 3, "translation");
  pointer(source, source.animations[0], input,
      floats(source, {.1f,.9f}, TINYGLTF_TYPE_SCALAR, 2), "/nodes/2/weights/0");
  const auto plan = PlanGltfSubtreeClone(source, source, 1);
  EXPECT_EQ(plan.clonedChannels, 4u); // selected T/scale/weights plus external joint T.
  const auto library = ParseAnimations(plan.authoredSource);
  ASSERT_EQ(library.clips[0].tracks.size(), 10u);
  EXPECT_TRUE(plan.authoredSource.nodes[size_t(plan.nodes[2])].weights.empty());
  EXPECT_EQ(plan.authoredSource.nodes[size_t(plan.nodes[2])].mesh, -1);
  auto evaluated = plan.evaluatedSource;
  ApplyAnimationPose(evaluated, SampleAnimation(library.clips[0], 1));
  EXPECT_EQ(evaluated.nodes[3].translation, evaluated.nodes[size_t(plan.nodes[3])].translation);
}

TEST(GltfSubtreeClone, PreservesInstancingAccessorIdentitiesExactUnsignedMetadataAndLocalFlags) {
  auto source = fixture();
  const auto translations = floats(source, {0,0,0, 2,0,0}, TINYGLTF_TYPE_VEC3, 2);
  const std::array<uint32_t,2> ids{16777217u,16777219u};
  std::vector<unsigned char> encoded(sizeof(ids)); std::memcpy(encoded.data(), ids.data(), encoded.size());
  const auto custom = bytes(source, encoded, TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, 2);
  declare(source, "EXT_mesh_gpu_instancing");
  source.nodes[1].extensions["EXT_mesh_gpu_instancing"] = Value(Object{{"attributes", Value(Object{
      {"TRANSLATION", Value(translations)}, {"_ID", Value(custom)}})}, {"extras", Value(77)}});
  for (const auto* name : {"KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability"}) declare(source, name);
  source.nodes[1].extensions["KHR_node_visibility"] = Value(Object{{"visible", Value(false)}});
  source.nodes[1].extensions["KHR_node_selectability"] = Value(Object{{"selectable", Value(false)}});
  source.nodes[1].extensions["KHR_node_hoverability"] = Value(Object{});
  const auto plan = PlanGltfSubtreeClone(source, source, 1);
  EXPECT_EQ(plan.authoredSource.nodes[3].extensions, source.nodes[1].extensions);
  EXPECT_EQ(plan.authoredSource.accessors.size(), source.accessors.size());
  const auto instances = ParseMeshGpuInstancing(plan.authoredSource);
  ASSERT_EQ(instances.nodes.size(), 2u);
  EXPECT_EQ(instances.summary.instances, 4u);
  CheckedAccessor accessor(plan.authoredSource, custom, "exact clone metadata");
  uint32_t value{}; accessor.readUnsigned(0, std::span(&value, 1)); EXPECT_EQ(value, ids[0]);
  accessor.readUnsigned(1, std::span(&value, 1)); EXPECT_EQ(value, ids[1]);
  const auto flags = ParseGltfNodeFlags(plan.authoredSource);
  EXPECT_EQ(flags.nodes[1], flags.nodes[3]);
  EXPECT_FALSE(flags.nodes[3].visible); EXPECT_FALSE(flags.nodes[3].selectable);
  EXPECT_TRUE(flags.nodes[3].hoverable);
  EXPECT_TRUE(flags.nodes[3].hasExtension(GltfNodeFlag::Hoverability));
}

TEST(GltfSubtreeClone, SharedParentSceneMembershipAndParentlessSelectedSceneAreDistinct) {
  auto authored = fixture(); skin(authored);
  authored.scenes.push_back(authored.scenes[0]); authored.defaultScene = 1;
  auto live = authored; live.defaultScene = -1;
  const auto child = PlanGltfSubtreeClone(live, authored, 1);
  EXPECT_EQ(child.destinationScene, 0u);
  EXPECT_EQ(child.affectedScenes, (std::vector<size_t>{0,1}));
  EXPECT_EQ(child.authoredSource.scenes[0].nodes, (std::vector<int>{0,4}));
  EXPECT_EQ(child.authoredSource.scenes[1].nodes, (std::vector<int>{0,4}));
  EXPECT_EQ(child.authoredSource.defaultScene, 1); EXPECT_EQ(child.evaluatedSource.defaultScene, -1);
  const auto root = PlanGltfSubtreeClone(live, authored, 0, {.destinationScene = 1});
  EXPECT_EQ(root.affectedScenes, std::vector<size_t>{1});
  EXPECT_EQ(root.authoredSource.scenes[0].nodes, std::vector<int>{0});
  EXPECT_EQ(root.authoredSource.scenes[1].nodes, (std::vector<int>{0,4}));
  EXPECT_TRUE(root.contextRoots.empty());
  EXPECT_EQ(root.authoredSource.defaultScene, 1); EXPECT_EQ(root.evaluatedSource.defaultScene, -1);
}

TEST(GltfSubtreeClone, SynthesizesImplicitSceneWithoutChangingOptionalDefaults) {
  auto source = fixture(); source.scenes.clear(); source.defaultScene = -1;
  const auto plan = PlanGltfSubtreeClone(source, source, 0);
  ASSERT_EQ(plan.authoredSource.scenes.size(), 1u);
  EXPECT_EQ(plan.authoredSource.scenes[0].nodes, (std::vector<int>{0,3}));
  EXPECT_EQ(plan.authoredSource.defaultScene, -1); EXPECT_EQ(plan.evaluatedSource.defaultScene, -1);
  const auto catalog = container::scene::BuildGltfSceneCatalog(plan.authoredSource);
  EXPECT_EQ(catalog.entries[0].sourceRoots, (std::vector<int32_t>{0,3}));
}

TEST(GltfSubtreeClone, RetainsArchiveOnlyManualResourcesAndSupportsRepeatedClones) {
  const auto live = fixture(); auto archive = live;
  auto manualImage = archive.images[0]; manualImage.name = "manual"; manualImage.image[0] = 99;
  archive.images.push_back(manualImage);
  auto manualTexture = archive.textures[0]; manualTexture.source = 1; archive.textures.push_back(manualTexture);
  const auto first = PlanGltfSubtreeClone(live, archive, 1);
  EXPECT_EQ(first.authoredSource.textures.size(), 2u); EXPECT_EQ(first.evaluatedSource.textures.size(), 2u);
  EXPECT_EQ(first.evaluatedSource.images[1].image, manualImage.image);
  const auto second = PlanGltfSubtreeClone(first.evaluatedSource, first.authoredSource, first.root);
  EXPECT_EQ(second.root, 4); EXPECT_EQ(second.parent, 0);
  EXPECT_EQ(second.authoredSource.nodes[0].children, (std::vector<int>{1,2,3,4}));
  EXPECT_EQ(second.authoredSource.meshes.size(), 1u);
  EXPECT_EQ(second.authoredSource.textures, archive.textures);
  EXPECT_EQ(second.authoredSource.buffers, archive.buffers);
}

TEST(GltfSubtreeClone, RepeatedAnimatedCloneKeepsEveryPreviousChannelAndRestIdentity) {
  auto source = fixture(); animation(source);
  const auto first = PlanGltfSubtreeClone(source, source, 1);
  const auto second = PlanGltfSubtreeClone(first.evaluatedSource, first.authoredSource, first.root);
  EXPECT_EQ(second.root, 4);
  EXPECT_EQ(second.originalChannelCounts, std::vector<size_t>{7});
  const auto& before = first.authoredSource.animations[0].channels;
  const auto& after = second.authoredSource.animations[0].channels;
  ASSERT_EQ(after.size(), 10u);
  for (size_t c = 0; c < before.size(); ++c) {
    EXPECT_EQ(after[c], before[c]);
    EXPECT_EQ(after[c].target_extensions, before[c].target_extensions);
    EXPECT_EQ(after[c].target_extensions_json_string, before[c].target_extensions_json_string);
  }
  EXPECT_EQ(pointerText(after[8]), "/nodes/4/scale/1");
  EXPECT_EQ(pointerText(after[9]), "/nodes/4/weights/0");
  auto evaluated = second.evaluatedSource;
  ApplyAnimationPose(evaluated, SampleAnimation(ParseAnimations(second.authoredSource).clips[0], 1));
  EXPECT_EQ(evaluated.nodes[1].weights, evaluated.nodes[3].weights);
  EXPECT_EQ(evaluated.nodes[1].weights, evaluated.nodes[4].weights);
  EXPECT_EQ(second.authoredSource.nodes[4].weights, source.nodes[1].weights);
  EXPECT_EQ(second.authoredSource.accessors, source.accessors);
}

TEST(GltfSubtreeClone, CallerRestrictionsRejectVisibleAndExternalDependencyClosuresWithoutMutation) {
  auto source = fixture(); skin(source); const auto before = source;
  for (const auto node : {0,1,2,3,9999}) {
    SCOPED_TRACE(node); const std::array<int32_t,1> blocked{node};
    EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {blocked}), std::runtime_error);
    EXPECT_EQ(source, before);
  }
  auto plain = fixture(); const std::array<int32_t,1> unrelated{2};
  EXPECT_NO_THROW((void)PlanGltfSubtreeClone(plain, plain, 1, {}, {unrelated}));
}

TEST(GltfSubtreeClone, RejectsUnknownClosureReferencesBadTransformsAndInvalidScenesTransactionally) {
  const auto source = fixture();
  for (int invalid = 0; invalid < 8; ++invalid) {
    SCOPED_TRACE(invalid); auto bad = source;
    if (invalid == 0) { declare(bad, "OMI_physics_body"); bad.nodes[1].extensions["OMI_physics_body"] = Value(Object{{"body", Value(0)}}); }
    if (invalid == 1) { bad.nodes[1].matrix = {1,0,0,.1, 0,1,0,0, 0,0,1,0, 0,0,0,1}; bad.nodes[1].translation.clear(); }
    if (invalid == 2) bad.nodes[1].translation = {1,2};
    if (invalid == 3) bad.nodes[1].translation = {std::numeric_limits<double>::infinity(),0,0};
    if (invalid == 4) bad.nodes[0].children.push_back(1);
    if (invalid == 5) bad.nodes[1].mesh = 99999;
    if (invalid == 6) bad.nodes[1].rotation = {0,0,0,0};
    if (invalid == 7) { bad.nodes[0].matrix = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,.2,1}; bad.nodes[0].translation.clear();
      bad.nodes[0].matrix[3] = .1; }
    const auto before = bad;
    EXPECT_THROW(PlanGltfSubtreeClone(bad, bad, 1), std::exception);
    if (invalid == 3) {
      // TinyGLTF's approximate double equality subtracts infinities, producing
      // NaN even for an untouched value. Compare that payload by exact bits,
      // then compare every other field with finite placeholders in test copies.
      EXPECT_EQ(std::bit_cast<uint64_t>(bad.nodes[1].translation[0]),
                std::bit_cast<uint64_t>(before.nodes[1].translation[0]));
      auto finiteAfter = bad, finiteBefore = before;
      finiteAfter.nodes[1].translation[0] = finiteBefore.nodes[1].translation[0] = 0;
      EXPECT_EQ(finiteAfter, finiteBefore);
    } else EXPECT_EQ(bad, before);
  }
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, -1), std::runtime_error);
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {.destinationScene = 9}), std::runtime_error);
  auto emptyScene = source; emptyScene.scenes.emplace_back();
  EXPECT_THROW(PlanGltfSubtreeClone(emptyScene, emptyScene, 1, {.destinationScene = 1}), std::runtime_error);
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {.rootName = "bad\nlabel"}), std::invalid_argument);
}

TEST(GltfSubtreeClone, RejectsChangedSourceBindingsAndNonresidentArchiveResources) {
  const auto archive = fixture();
  for (int invalid = 0; invalid < 4; ++invalid) {
    SCOPED_TRACE(invalid); auto live = archive;
    if (invalid == 0) live.nodes[1].mesh = -1;
    if (invalid == 1) live.meshes[0].primitives[0].attributes["POSITION"] = -1;
    if (invalid == 2) live.textures[0].source = 99;
    if (invalid == 3) live.buffers[0].data[0] ^= 1;
    EXPECT_THROW(PlanGltfSubtreeClone(live, archive, 1), std::runtime_error);
  }
  auto nonresident = archive; nonresident.images[0].image.clear(); nonresident.images[0].uri = "not-loaded.png";
  EXPECT_THROW(PlanGltfSubtreeClone(nonresident, nonresident, 1), std::runtime_error);
}

TEST(GltfSubtreeClone, EnforcesOutputPeakAndDecodedBudgetsBeforeReturningAnyCandidate) {
  auto source = fixture(); animation(source);
  const auto plan = PlanGltfSubtreeClone(source, source, 1);
  GltfMergeLimits limits;
  limits.maxNodes = source.nodes.size();
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {}, limits), std::runtime_error);
  limits = {}; limits.maxPlacementNodes = 0;
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {}, limits), std::runtime_error);
  limits = {}; limits.maxClonedChannels = 2;
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {}, limits), std::runtime_error);
  limits = {}; limits.maxResidentBytes = plan.peakResidentBytes;
  EXPECT_NO_THROW((void)PlanGltfSubtreeClone(source, source, 1, {}, {}, limits));
  --limits.maxResidentBytes;
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {}, limits), std::runtime_error);
  limits = {}; limits.maxMetadataBytes = plan.metadataBytes;
  EXPECT_NO_THROW((void)PlanGltfSubtreeClone(source, source, 1, {}, {}, limits));
  --limits.maxMetadataBytes;
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {}, limits), std::runtime_error);
  limits = {}; limits.maxAnimationDecodedBytes = plan.animationDecodedBytes;
  EXPECT_NO_THROW((void)PlanGltfSubtreeClone(source, source, 1, {}, {}, limits));
  --limits.maxAnimationDecodedBytes;
  EXPECT_THROW(PlanGltfSubtreeClone(source, source, 1, {}, {}, limits), std::runtime_error);
}

TEST(GltfSubtreeClone, CountsRetainedParentEdgeAndAppendedSkinReferencesAtExactLimits) {
  const auto plain = fixture();
  GltfMergeLimits limits; limits.maxReferences = 2; // Two original child edges.
  EXPECT_THROW(PlanGltfSubtreeClone(plain, plain, 1, {}, {}, limits), std::runtime_error);
  limits.maxReferences = 3;
  EXPECT_NO_THROW((void)PlanGltfSubtreeClone(plain, plain, 1, {}, {}, limits));

  auto skinned = fixture(); skin(skinned);
  // Original: three edges + (one joint + skeleton/IBM). Appended: two
  // transform-context edges + three skin refs + retained-parent sibling edge.
  limits.maxReferences = 11;
  EXPECT_THROW(PlanGltfSubtreeClone(skinned, skinned, 1, {}, {}, limits), std::runtime_error);
  limits.maxReferences = 12;
  EXPECT_NO_THROW((void)PlanGltfSubtreeClone(skinned, skinned, 1, {}, {}, limits));
}

} // namespace
