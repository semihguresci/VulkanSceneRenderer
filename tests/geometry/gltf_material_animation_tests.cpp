#include <Container/geometry/GltfAnimation.h>

#include <gtest/gtest.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace container::geometry::gltf;
using Value = tinygltf::Value;
using Object = Value::Object;
using Property = GltfMaterialProperty;

int floats(tinygltf::Model& model, std::initializer_list<float> values, int type, size_t count) {
  tinygltf::Buffer buffer;
  buffer.data.resize(values.size() * sizeof(float));
  std::memcpy(buffer.data.data(), values.begin(), buffer.data.size());
  tinygltf::BufferView view;
  view.buffer = int(model.buffers.size());
  view.byteLength = buffer.data.size();
  tinygltf::Accessor accessor;
  accessor.sparse = {};
  accessor.bufferView = int(model.bufferViews.size());
  accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.type = type;
  accessor.count = count;
  if (type == TINYGLTF_TYPE_SCALAR && values.size()) {
    const auto [low, high] = std::minmax_element(values.begin(), values.end());
    accessor.minValues = {*low};
    accessor.maxValues = {*high};
  }
  const auto result = int(model.accessors.size());
  model.buffers.push_back(std::move(buffer));
  model.bufferViews.push_back(view);
  model.accessors.push_back(accessor);
  return result;
}

void channel(tinygltf::Animation& clip, int input, int output, int node, std::string path) {
  tinygltf::AnimationSampler sampler;
  sampler.input = input;
  sampler.output = output;
  sampler.interpolation = "LINEAR";
  sampler.extras = Value(Object{{"samplerIdentity", Value(17)}});
  sampler.extras_json_string = "sampler raw metadata";
  tinygltf::AnimationChannel target;
  target.sampler = int(clip.samplers.size());
  target.target_node = node;
  target.target_path = std::move(path);
  target.extras = Value(Object{{"channelIdentity", Value(19)}});
  target.target_extras_json_string = "target raw metadata";
  clip.samplers.push_back(std::move(sampler));
  clip.channels.push_back(std::move(target));
}

void pointer(tinygltf::Animation& clip, int input, int output, std::string path) {
  channel(clip, input, output, -1, "pointer");
  clip.channels.back().target_extensions["KHR_animation_pointer"] =
      Value(Object{{"pointer", Value(std::move(path))}});
  clip.channels.back().target_extensions_json_string = "pointer raw metadata";
}

GltfMaterialPatch number(Property property, double value) {
  GltfMaterialPatch patch;
  patch.values.push_back({property, value});
  return patch;
}

double curve(float first, float last, double time) {
  // Independent two-key FLOAT source oracle, with keys at 0 and 2 seconds.
  const auto t = time / 2.;
  return (1. - t) * double(first) + t * double(last);
}

void controlsEqual(const AnimationPlayer& actual, const AnimationPlayer& expected) {
  EXPECT_EQ(actual.selectedClip(), expected.selectedClip());
  EXPECT_EQ(std::bit_cast<uint64_t>(actual.time()), std::bit_cast<uint64_t>(expected.time()));
  EXPECT_EQ(std::bit_cast<uint64_t>(actual.speed()), std::bit_cast<uint64_t>(expected.speed()));
  EXPECT_EQ(actual.playing(), expected.playing());
  EXPECT_EQ(actual.loop(), expected.loop());
}

void appendDeclarations(tinygltf::Model& model, const GltfMaterialAuthoringPlan& plan) {
  for (const auto& name : plan.extensionsToDeclare)
    if (std::ranges::find(model.extensionsUsed, name) == model.extensionsUsed.end())
      model.extensionsUsed.push_back(name);
}

struct Fixture {
  tinygltf::Model authored;
  Fixture() {
    authored.asset.version = "2.0";
    authored.extensionsUsed = {"KHR_animation_pointer", "KHR_lights_punctual", "VENDOR_material"};
    authored.materials.resize(2);
    auto& material = authored.materials[0];
    material.name = "Authored panel";
    material.pbrMetallicRoughness.baseColorFactor = {.1, .2, .3, 1};
    material.pbrMetallicRoughness.metallicFactor = .125;
    material.pbrMetallicRoughness.roughnessFactor = .75;
    material.normalTexture.index = 0;
    material.extras = Value(Object{{"opaqueSourceIdentity", Value(23)}});
    material.extensions["VENDOR_material"] = Value(Object{{"opaqueIndex", Value(37)}});
    material.extras_json_string = "material raw extras";
    material.extensions_json_string = "material raw extensions";
    authored.materials[1].name = "Other material";
    authored.materials[1].pbrMetallicRoughness.metallicFactor = .6;
    authored.materials[1].pbrMetallicRoughness.roughnessFactor = .8;
    authored.images.resize(2);
    authored.samplers.resize(2);
    authored.textures.resize(2);
    for (int i = 0; i < 2; ++i) {
      authored.images[size_t(i)].image = {static_cast<unsigned char>(i), 20, 30, 255};
      authored.images[size_t(i)].name = "Image " + std::to_string(i);
      authored.textures[size_t(i)].source = i;
      authored.textures[size_t(i)].sampler = i;
    }
    authored.nodes.resize(2);
    authored.nodes[0].translation = {1, 2, 3};
    authored.nodes[1].matrix = {1, 0, 0, 0, .25, 1, 0, 0, 0, 0, 1, 0, 4, 5, 6, 1};
    tinygltf::Camera camera;
    camera.type = "perspective";
    camera.perspective.yfov = .7;
    camera.perspective.znear = .1;
    authored.cameras.push_back(camera);
    tinygltf::Light light;
    light.type = "point";
    light.color = {1, 1, 1};
    light.intensity = 3;
    authored.lights.push_back(light);
    const auto input = floats(authored, {0, 2}, TINYGLTF_TYPE_SCALAR, 2);
    tinygltf::Animation clip;
    clip.name = "Material and unrelated typed tracks";
    clip.extras = Value(Object{{"clipIdentity", Value(29)}});
    clip.extras_json_string = "clip raw extras";
    pointer(clip, input, floats(authored, {.2f, .8f}, TINYGLTF_TYPE_SCALAR, 2),
        "/materials/0/pbrMetallicRoughness/metallicFactor");
    channel(clip, input, floats(authored, {1, 2, 3, 5, 2, 3}, TINYGLTF_TYPE_VEC3, 2), 0, "translation");
    pointer(clip, input, floats(authored, {3, 11}, TINYGLTF_TYPE_SCALAR, 2),
        "/extensions/KHR_lights_punctual/lights/0/intensity");
    pointer(clip, input, floats(authored, {.7f, .9f}, TINYGLTF_TYPE_SCALAR, 2),
        "/cameras/0/perspective/yfov");
    // Unsupported raw channels are retained diagnostics, never silently dropped
    // from the source or promoted to a new supported material property.
    pointer(clip, input, floats(authored, {0, 1}, TINYGLTF_TYPE_SCALAR, 2), "/materials/0/futureFactor");
    authored.animations.push_back(std::move(clip));
    tinygltf::Animation other;
    other.name = "Other clip";
    pointer(other, input, floats(authored, {.1f, .5f}, TINYGLTF_TYPE_SCALAR, 2),
        "/materials/0/pbrMetallicRoughness/metallicFactor");
    pointer(other, input, floats(authored, {.9f, .1f}, TINYGLTF_TYPE_SCALAR, 2),
        "/materials/1/pbrMetallicRoughness/roughnessFactor");
    authored.animations.push_back(std::move(other));
  }
  AnimationPlayer running(tinygltf::Model& evaluated, double time = .5) const {
    AnimationPlayer player(authored);
    player.selectClip(evaluated, 0);
    player.seek(evaluated, time);
    player.setLoop(false);
    player.setSpeed(-2.5);
    player.play();
    return player;
  }
};

void publish(tinygltf::Model& authored, tinygltf::Model& evaluated, AnimationPlayer& player,
             const GltfMaterialAuthoringPlan& plan) {
  auto next = player.withMaterialEdits(evaluated, plan);
  auto sampled = next.evaluateMaterialRestEdit(size_t(plan.destinationMaterial), plan.evaluatedAfter);
  if (plan.kind == GltfMaterialPlanKind::Edit) {
    authored.materials[size_t(plan.destinationMaterial)] = plan.authoredAfter;
    evaluated.materials[size_t(plan.destinationMaterial)] = std::move(sampled);
  } else {
    authored.materials.push_back(plan.authoredAfter);
    evaluated.materials.push_back(std::move(sampled));
  }
  appendDeclarations(authored, plan);
  appendDeclarations(evaluated, plan);
  player = std::move(next);
}

TEST(GltfMaterialAnimation, PreparedRestEditPreservesControlsClipsDiagnosticsAndUnrelatedSource) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto before = evaluated;
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, .25));
  const auto next = player.withMaterialEdits(evaluated, plan);
  controlsEqual(next, player);
  EXPECT_EQ(next.library().clips, player.library().clips);
  ASSERT_EQ(next.library().diagnostics.size(), 1u);
  EXPECT_EQ(next.library().diagnostics[0].message, player.library().diagnostics[0].message);
  EXPECT_EQ(next.library().diagnostics[0].clip, 0u);
  EXPECT_EQ(next.library().diagnostics[0].channel, 4u);
  EXPECT_EQ(next.library().restNodes, player.library().restNodes);
  EXPECT_EQ(next.library().restLights, player.library().restLights);
  EXPECT_EQ(next.library().restCameras, player.library().restCameras);
  EXPECT_EQ(next.library().restMaterials[1], player.library().restMaterials[1]);
  EXPECT_EQ(next.library().restMaterials[0].pbrMetallicRoughness.metallicFactor, .125);
  EXPECT_EQ(next.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor, .25);
  const auto sampled = next.evaluateMaterialRestEdit(0, plan.evaluatedAfter);
  EXPECT_EQ(sampled.pbrMetallicRoughness.metallicFactor, curve(.2f, .8f, .5));
  EXPECT_EQ(sampled.pbrMetallicRoughness.roughnessFactor, .25);
  EXPECT_EQ(sampled.normalTexture.index, 0);
  EXPECT_EQ(sampled.extensions, before.materials[0].extensions);
  EXPECT_EQ(sampled.extensions_json_string, "material raw extensions");
  EXPECT_EQ(evaluated, before);
  for (size_t i = 0; i < before.buffers.size(); ++i) EXPECT_EQ(evaluated.buffers[i].data, before.buffers[i].data);
  EXPECT_EQ(player.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor, .75);
}

TEST(GltfMaterialAnimation, AbsoluteTrackOverridesEditedRestOnlyAtCurrentClock) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto controls = player;
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Metallic, .9));
  publish(fixture.authored, evaluated, player, plan);
  controlsEqual(player, controls);
  EXPECT_EQ(fixture.authored.materials[0].pbrMetallicRoughness.metallicFactor, .9);
  EXPECT_EQ(player.library().restMaterials[0].pbrMetallicRoughness.metallicFactor, .9);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, curve(.2f, .8f, .5));
  player.seek(evaluated, 1.5);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, curve(.2f, .8f, 1.5));
  player.clearClip(evaluated);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, .9);
}

TEST(GltfMaterialAnimation, RepeatedUndoRedoRebasesCurrentClockWithoutRewindingOtherTypedTracks) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto command = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, .25));
  publish(fixture.authored, evaluated, player, command);
  player.seek(evaluated, 1.5);
  const auto beforeUndo = evaluated;
  const auto undoControls = player;
  auto undo = RebaseGltfMaterialEdit(command, GltfMaterialEditSide::Before,
      player.library().restMaterials[0], evaluated.materials[0], GltfMaterialSourceView(fixture.authored), GltfMaterialSourceView(evaluated));
  publish(fixture.authored, evaluated, player, undo);
  controlsEqual(player, undoControls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.roughnessFactor, .75);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, curve(.2f, .8f, 1.5));
  EXPECT_EQ(evaluated.nodes, beforeUndo.nodes);
  EXPECT_EQ(evaluated.lights, beforeUndo.lights);
  EXPECT_EQ(evaluated.cameras, beforeUndo.cameras);
  EXPECT_EQ(evaluated.materials[1], beforeUndo.materials[1]);
  player.seek(evaluated, .25);
  const auto redoControls = player;
  const auto beforeRedo = evaluated;
  auto redo = RebaseGltfMaterialEdit(undo, GltfMaterialEditSide::After,
      player.library().restMaterials[0], evaluated.materials[0], GltfMaterialSourceView(fixture.authored), GltfMaterialSourceView(evaluated));
  publish(fixture.authored, evaluated, player, redo);
  controlsEqual(player, redoControls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.roughnessFactor, .25);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, curve(.2f, .8f, .25));
  EXPECT_EQ(evaluated.nodes, beforeRedo.nodes);
  EXPECT_EQ(evaluated.lights, beforeRedo.lights);
  EXPECT_EQ(evaluated.cameras, beforeRedo.cameras);
  const auto secondUndo = RebaseGltfMaterialEdit(redo, GltfMaterialEditSide::Before,
      player.library().restMaterials[0], evaluated.materials[0], GltfMaterialSourceView(fixture.authored), GltfMaterialSourceView(evaluated));
  EXPECT_EQ(secondUndo.authoredAfter.pbrMetallicRoughness.roughnessFactor, .75);
}

TEST(GltfMaterialAnimation, ReplayAfterChangingClipPreservesSelectedClipAndOtherMaterialPose) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto command = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Metallic, .95));
  publish(fixture.authored, evaluated, player, command);
  player.selectClip(evaluated, 1);
  player.seek(evaluated, 1.25);
  const auto before = evaluated;
  const auto controls = player;
  const auto undo = RebaseGltfMaterialEdit(command, GltfMaterialEditSide::Before,
      player.library().restMaterials[0], evaluated.materials[0], GltfMaterialSourceView(fixture.authored), GltfMaterialSourceView(evaluated));
  publish(fixture.authored, evaluated, player, undo);
  controlsEqual(player, controls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, curve(.1f, .5f, 1.25));
  EXPECT_EQ(evaluated.materials[1], before.materials[1]);
  EXPECT_EQ(evaluated.materials[1].pbrMetallicRoughness.roughnessFactor, curve(.9f, .1f, 1.25));
  EXPECT_EQ(evaluated.nodes, before.nodes);
  EXPECT_EQ(evaluated.lights, before.lights);
  EXPECT_EQ(evaluated.cameras, before.cameras);
}

TEST(GltfMaterialAnimation, ReplayAfterClearingClipUsesPreparedTrackedRestInsteadOfCreationTimeSample) {
  Fixture fixture;auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  GltfMaterialPatch patch;patch.values={{Property::Metallic,.95},{Property::Roughness,.25}};
  const auto command=PlanGltfMaterialEdit(fixture.authored,evaluated,0,patch);
  publish(fixture.authored,evaluated,player,command);
  player.seek(evaluated,1.5);player.clearClip(evaluated);
  evaluated.materials[0].emissiveFactor={.6,.4,.2}; // An independent untracked evaluated field.
  const auto controls=player;const auto before=evaluated;
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,player.library().restMaterials[0],evaluated.materials[0],
      GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated));
  // The receipt intentionally retains its original evaluated endpoint. The
  // detached sampler must resolve this tracked leaf against today's rest/clip.
  ASSERT_EQ(undo.evaluatedAfter.pbrMetallicRoughness.metallicFactor,curve(.2f,.8f,.5));
  publish(fixture.authored,evaluated,player,undo);
  controlsEqual(player,controls);
  EXPECT_EQ(fixture.authored.materials[0].pbrMetallicRoughness.metallicFactor,.125);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,.125);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.roughnessFactor,.75);
  EXPECT_EQ(evaluated.materials[0].emissiveFactor,before.materials[0].emissiveFactor);
  EXPECT_EQ(evaluated.nodes,before.nodes);EXPECT_EQ(evaluated.lights,before.lights);EXPECT_EQ(evaluated.cameras,before.cameras);
  EXPECT_EQ(evaluated.animations,before.animations);EXPECT_EQ(player.library().clips,controls.library().clips);
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,player.library().restMaterials[0],evaluated.materials[0],
      GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated));
  publish(fixture.authored,evaluated,player,redo);
  controlsEqual(player,controls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,.95);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.roughnessFactor,.25);
  EXPECT_EQ(evaluated.materials[0].emissiveFactor,before.materials[0].emissiveFactor);
  player.selectClip(evaluated,0);player.seek(evaluated,1.75);
  const auto activeControls=player;const auto activeBefore=evaluated;
  const auto activeUndo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,player.library().restMaterials[0],evaluated.materials[0],
      GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated));
  publish(fixture.authored,evaluated,player,activeUndo);
  controlsEqual(player,activeControls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,curve(.2f,.8f,1.75));
  EXPECT_EQ(evaluated.nodes,activeBefore.nodes);EXPECT_EQ(evaluated.lights,activeBefore.lights);EXPECT_EQ(evaluated.cameras,activeBefore.cameras);
}

TEST(GltfMaterialAnimation, SelectedClipMissingEditedTargetUsesRestAndPreservesFrozenUntrackedFieldsAndBindings) {
  Fixture fixture;const auto input=fixture.authored.animations[0].samplers[0].input;
  tinygltf::Animation unrelated;unrelated.name="Other material only";
  pointer(unrelated,input,floats(fixture.authored,{.3f,.7f},TINYGLTF_TYPE_SCALAR,2),"/materials/1/pbrMetallicRoughness/roughnessFactor");
  fixture.authored.animations.push_back(std::move(unrelated));
  auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  const auto command=PlanGltfMaterialEdit(fixture.authored,evaluated,0,number(Property::Metallic,.95));
  publish(fixture.authored,evaluated,player,command);player.selectClip(evaluated,2);player.seek(evaluated,1.25);
  evaluated.materials[0].pbrMetallicRoughness.roughnessFactor=std::nextafter(.31,1.);
  evaluated.materials[0].normalTexture.index=1;
  evaluated.materials[0].extras=Value(Object{{"frozen",Value(737)}});
  const auto controls=player;const auto before=evaluated;
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,player.library().restMaterials[0],evaluated.materials[0],
      GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated));
  publish(fixture.authored,evaluated,player,undo);controlsEqual(player,controls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,.125);
  EXPECT_EQ(std::bit_cast<uint64_t>(evaluated.materials[0].pbrMetallicRoughness.roughnessFactor),
      std::bit_cast<uint64_t>(before.materials[0].pbrMetallicRoughness.roughnessFactor));
  EXPECT_EQ(evaluated.materials[0].normalTexture.index,1);
  EXPECT_EQ(evaluated.materials[0].extras,before.materials[0].extras);
  EXPECT_EQ(evaluated.materials[1],before.materials[1]);
  EXPECT_EQ(evaluated.materials[1].pbrMetallicRoughness.roughnessFactor,curve(.3f,.7f,1.25));
  const auto redo=RebaseGltfMaterialEdit(undo,GltfMaterialEditSide::After,player.library().restMaterials[0],evaluated.materials[0],
      GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated));
  publish(fixture.authored,evaluated,player,redo);controlsEqual(player,controls);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,.95);
  EXPECT_EQ(evaluated.materials[0].normalTexture.index,1);EXPECT_EQ(evaluated.materials[0].extras,before.materials[0].extras);
  EXPECT_EQ(evaluated.materials[1],before.materials[1]);
  EXPECT_EQ(fixture.authored.materials[0].normalTexture.index,0);
}

TEST(GltfMaterialAnimation, RestReplayPreservesUntrackedComponentsTextureIdentityAndOptionalRawLeaves) {
  Fixture fixture;auto& material=fixture.authored.materials[0];
  fixture.authored.extensionsUsed.insert(fixture.authored.extensionsUsed.end(),{"KHR_materials_anisotropy","KHR_texture_transform"});
  material.extensions["KHR_materials_anisotropy"]=Value(Object{}); // Omitted strength is zero.
  material.normalTexture.extensions["KHR_texture_transform"]=Value(Object{{"rotation",Value(0)},
      {"extras",Value(Object{{"raw",Value(-0.)}})}});
  material.normalTexture.extensions_json_string="authored transform cache";
  const auto input=fixture.authored.animations[0].samplers[0].input;
  pointer(fixture.authored.animations[0],input,floats(fixture.authored,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2),
      "/materials/0/pbrMetallicRoughness/baseColorFactor/1");
  pointer(fixture.authored.animations[0],input,floats(fixture.authored,{0,1},TINYGLTF_TYPE_SCALAR,2),
      "/materials/0/extensions/KHR_materials_anisotropy/anisotropyStrength");
  pointer(fixture.authored.animations[0],input,floats(fixture.authored,{0,.5f},TINYGLTF_TYPE_SCALAR,2),
      "/materials/0/normalTexture/extensions/KHR_texture_transform/rotation");
  pointer(fixture.authored.animations[0],input,floats(fixture.authored,{.1f,.9f},TINYGLTF_TYPE_SCALAR,2),
      "/materials/0/normalTexture/extensions/KHR_texture_transform/offset/0");
  auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  const auto command=PlanGltfMaterialEdit(fixture.authored,evaluated,0,number(Property::Anisotropy,.8));
  publish(fixture.authored,evaluated,player,command);player.clearClip(evaluated);
  auto& current=evaluated.materials[0];current.pbrMetallicRoughness.baseColorFactor={.85,.99,.65,.75};
  current.normalTexture.index=1;
  auto& transform=current.normalTexture.extensions.at("KHR_texture_transform").Get<Object>();
  transform["rotation"]=Value(.9);transform["offset"]=Value(Value::Array{Value(.75),Value(.88)});
  const auto controls=player;const auto before=evaluated;
  const auto undo=RebaseGltfMaterialEdit(command,GltfMaterialEditSide::Before,player.library().restMaterials[0],current,
      GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated));
  publish(fixture.authored,evaluated,player,undo);controlsEqual(player,controls);
  const auto& restored=evaluated.materials[0];
  EXPECT_EQ(restored.pbrMetallicRoughness.baseColorFactor,(std::vector<double>{.85,.2,.65,.75}));
  EXPECT_FALSE(restored.extensions.at("KHR_materials_anisotropy").Has("anisotropyStrength"));
  EXPECT_EQ(restored.normalTexture.index,1);
  const auto& restoredTransform=restored.normalTexture.extensions.at("KHR_texture_transform");
  EXPECT_TRUE(restoredTransform.Get("rotation").IsInt());EXPECT_EQ(restoredTransform.Get("rotation").Get<int>(),0);
  EXPECT_EQ(restoredTransform.Get("offset").Get(0).GetNumberAsDouble(),0.);
  EXPECT_EQ(restoredTransform.Get("offset").Get(1).GetNumberAsDouble(),.88);
  EXPECT_TRUE(std::signbit(restoredTransform.Get("extras").Get("raw").Get<double>()));
  EXPECT_TRUE(restored.normalTexture.extensions_json_string.empty());
  EXPECT_EQ(evaluated.nodes,before.nodes);EXPECT_EQ(evaluated.lights,before.lights);EXPECT_EQ(evaluated.cameras,before.cameras);
  EXPECT_EQ(evaluated.animations,before.animations);
}

TEST(GltfMaterialAnimation, RestSamplingBoundsAllClipTraversalEvenWhenNoClipIsSelected) {
  Fixture fixture;auto evaluated=fixture.authored;auto player=fixture.running(evaluated);player.clearClip(evaluated);
  const auto controls=player;const auto before=evaluated;
  GltfMaterialAnimationEditLimits limits;limits.maxChannels=player.library().clips[0].tracks.size();
  EXPECT_THROW(player.evaluateMaterialRestEdit(0,evaluated.materials[0],limits),std::invalid_argument);
  limits={};limits.maxClips=1;
  EXPECT_THROW(player.evaluateMaterialRestEdit(0,evaluated.materials[0],limits),std::invalid_argument);
  limits={};limits.maxOwnedBytes=1;
  EXPECT_THROW(player.evaluateMaterialRestEdit(0,evaluated.materials[0],limits),std::invalid_argument);
  controlsEqual(player,controls);EXPECT_EQ(evaluated,before);
}

TEST(GltfMaterialAnimation, DuplicateKeepsAuthoredRestAndFrozenEvaluatedAppearanceWithoutCopyingTracks) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto controls = player;
  const auto oldChannels = evaluated.animations[0].channels.size();
  const auto plan = PlanGltfMaterialDuplicate(fixture.authored, evaluated, 0);
  ASSERT_EQ(plan.destinationMaterial, 2);
  publish(fixture.authored, evaluated, player, plan);
  controlsEqual(player, controls);
  EXPECT_EQ(player.library().restMaterials[2].pbrMetallicRoughness.metallicFactor, .125);
  EXPECT_EQ(evaluated.materials[2].pbrMetallicRoughness.metallicFactor, curve(.2f, .8f, .5));
  EXPECT_EQ(evaluated.animations[0].channels.size(), oldChannels);
  EXPECT_EQ(player.library().clips[0].tracks.size(), 4u);
  const auto frozen = plan.evaluatedAfter;
  const auto frozenMetallicBits = std::bit_cast<uint64_t>(frozen.pbrMetallicRoughness.metallicFactor);
  EXPECT_EQ(std::bit_cast<uint64_t>(evaluated.materials[2].pbrMetallicRoughness.metallicFactor), frozenMetallicBits);
  player.seek(evaluated, 1.75);
  EXPECT_EQ(evaluated.materials[2], frozen);
  EXPECT_EQ(std::bit_cast<uint64_t>(evaluated.materials[2].pbrMetallicRoughness.metallicFactor), frozenMetallicBits);
  EXPECT_EQ(player.evaluateMaterialRestEdit(2, frozen), frozen);
  const auto dirty = player.clearClip(evaluated);
  // Stop restores tracked source IDs only. The new untracked duplicate keeps
  // its evaluated creation pose, independently of its authored .125 rest.
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor, .125);
  EXPECT_EQ(evaluated.materials[2], frozen);
  EXPECT_EQ(std::bit_cast<uint64_t>(evaluated.materials[2].pbrMetallicRoughness.metallicFactor), frozenMetallicBits);
  EXPECT_EQ(std::ranges::find(dirty.materials, size_t(2)), dirty.materials.end());
}

TEST(GltfMaterialAnimation, ExplicitPairAppendUsesIndependentTextureSpacesAndPreservesResourceBytes) {
  Fixture fixture;
  fixture.authored.textures.resize(5); // Archive-only texture suffix.
  auto evaluated = fixture.authored;
  evaluated.textures.resize(2);
  auto player = fixture.running(evaluated);
  tinygltf::Material authored, current;
  authored.pbrMetallicRoughness.metallicFactor = .15;
  current.pbrMetallicRoughness.metallicFactor = .65;
  authored.normalTexture.index = 4;
  current.normalTexture.index = 1;
  const auto before = evaluated;
  const auto controls = player;
  const auto plan = PlanGltfMaterialAppend(fixture.authored, evaluated, authored, current);
  const auto context = GltfMaterialAuthoringContext(plan);
  EXPECT_EQ(context.authoredTextureCount, 5u);
  EXPECT_EQ(context.evaluatedTextureCount, 2u);
  publish(fixture.authored, evaluated, player, plan);
  controlsEqual(player, controls);
  EXPECT_EQ(player.library().restMaterials[2].normalTexture.index, 4);
  EXPECT_EQ(evaluated.materials[2].normalTexture.index, 1);
  EXPECT_EQ(player.evaluateMaterialRestEdit(2, evaluated.materials[2]).pbrMetallicRoughness.metallicFactor, .65);
  EXPECT_EQ(evaluated.textures, before.textures);
  EXPECT_EQ(evaluated.images, before.images);
  EXPECT_EQ(evaluated.samplers, before.samplers);
  EXPECT_EQ(evaluated.accessors, before.accessors);
  EXPECT_EQ(evaluated.animations, before.animations);
  for (size_t i = 0; i < before.buffers.size(); ++i) EXPECT_EQ(evaluated.buffers[i].data, before.buffers[i].data);
}

TEST(GltfMaterialAnimation, ZeroSourceMaterialAppendWorksWithoutDummyIdentityTables) {
  tinygltf::Model authored, evaluated;
  AnimationPlayer player(authored);
  player.setSpeed(-0.);
  player.setLoop(false);
  tinygltf::Material a, e;
  a.pbrMetallicRoughness.roughnessFactor = .2;
  e.pbrMetallicRoughness.roughnessFactor = .7;
  const auto plan = PlanGltfMaterialAppend(authored, evaluated, a, e);
  auto next = player.withMaterialEdits(evaluated, plan);
  controlsEqual(next, player);
  ASSERT_EQ(next.library().restMaterials.size(), 1u);
  EXPECT_EQ(next.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor, .2);
  EXPECT_EQ(next.evaluateMaterialRestEdit(0, plan.evaluatedAfter).pbrMetallicRoughness.roughnessFactor, .7);
  EXPECT_TRUE(evaluated.materials.empty());
}

TEST(GltfMaterialAnimation, SamplesExtensionAndVectorComponentTracksWithoutPromotingTexturedZeroOrBlack) {
  Fixture fixture;
  auto& material = fixture.authored.materials[0];
  fixture.authored.extensionsUsed.insert(fixture.authored.extensionsUsed.end(),
      {"KHR_materials_anisotropy", "KHR_materials_diffuse_transmission", "KHR_texture_transform"});
  const auto transform = Value(Object{{"rotation", Value(.25)}});
  const auto textureExtensions = Value(Object{{"KHR_texture_transform", transform}});
  const auto texture = Value(Object{{"index", Value(0)}, {"texCoord", Value(1)}, {"extensions", textureExtensions}});
  material.extensions["KHR_materials_anisotropy"] = Value(Object{
      {"anisotropyStrength", Value(0.)}, {"anisotropyTexture", texture}});
  material.extensions["KHR_materials_diffuse_transmission"] = Value(Object{
      {"diffuseTransmissionFactor", Value(0.)}, {"diffuseTransmissionTexture", texture},
      {"diffuseTransmissionColorTexture", texture},
      {"diffuseTransmissionColorFactor", Value(Value::Array{Value(0.), Value(0.), Value(0.)})}});
  const auto input = fixture.authored.animations[0].samplers[0].input;
  const auto strength = floats(fixture.authored, {0, 1}, TINYGLTF_TYPE_SCALAR, 2);
  const auto green = floats(fixture.authored, {.2f, .8f}, TINYGLTF_TYPE_SCALAR, 2);
  pointer(fixture.authored.animations[0], input, strength,
      "/materials/0/extensions/KHR_materials_anisotropy/anisotropyStrength");
  pointer(fixture.authored.animations[0], input, green,
      "/materials/0/pbrMetallicRoughness/baseColorFactor/1");
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  GltfMaterialPatch patch;
  patch.values = {{Property::Anisotropy, 0.}, {Property::DiffuseTransmission, 0.},
      {Property::BaseColor, std::array<double, 4>{.9, .8, .7, 1.}}};
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, patch);
  const auto before = evaluated;
  const auto next = player.withMaterialEdits(evaluated, plan);
  const auto sampled = next.evaluateMaterialRestEdit(0, plan.evaluatedAfter);
  EXPECT_EQ(next.library().restMaterials[0].extensions.at("KHR_materials_anisotropy").Get("anisotropyStrength").GetNumberAsDouble(), 0.);
  EXPECT_EQ(sampled.extensions.at("KHR_materials_anisotropy").Get("anisotropyStrength").GetNumberAsDouble(), .25);
  const auto& diffuse = sampled.extensions.at("KHR_materials_diffuse_transmission");
  EXPECT_EQ(diffuse.Get("diffuseTransmissionFactor").GetNumberAsDouble(), 0.);
  for (size_t i = 0; i < 3; ++i) EXPECT_EQ(diffuse.Get("diffuseTransmissionColorFactor").Get(int(i)).GetNumberAsDouble(), 0.);
  EXPECT_EQ(diffuse.Get("diffuseTransmissionTexture"), texture);
  EXPECT_EQ(diffuse.Get("diffuseTransmissionColorTexture"), texture);
  EXPECT_EQ(sampled.pbrMetallicRoughness.baseColorFactor,
      (std::vector<double>{.9, curve(.2f, .8f, .5), .7, 1.}));
  auto absentParent = plan.evaluatedAfter;
  absentParent.extensions.erase("KHR_materials_anisotropy");
  EXPECT_THROW(next.evaluateMaterialRestEdit(0, absentParent), std::invalid_argument);
  auto invalidVector = plan.evaluatedAfter;
  invalidVector.pbrMetallicRoughness.baseColorFactor.resize(3);
  EXPECT_THROW(next.evaluateMaterialRestEdit(0, invalidVector), std::invalid_argument);
  EXPECT_EQ(evaluated, before);
  controlsEqual(next, player);
}

TEST(GltfMaterialAnimation, ReceiptRejectsForgedFieldsBeforeIndexingOrCopyingCallerRecords) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, .3));
  EXPECT_THROW(player.withMaterialEdits(evaluated, {}), std::invalid_argument);
  const std::vector<std::function<void(GltfMaterialAuthoringPlan&)>> changes{
      [](auto& p) { p.kind = GltfMaterialPlanKind::Append; },
      [](auto& p) { p.sourceMaterial = INT32_MAX; },
      [](auto& p) { p.destinationMaterial = INT32_MAX; },
      [](auto& p) { ++p.originalMaterialCount; },
      [](auto& p) { p.changed = !p.changed; },
      [](auto& p) { p.touchedProperties.clear(); },
      [](auto& p) { p.animationBindings[0].channel = 777; },
      [](auto& p) { p.extensionsToDeclare.push_back("FORGED"); },
      [](auto& p) { p.authoredBefore.normalTexture.index = 1; },
      [](auto& p) { p.authoredAfter.extras_json_string += " modified"; },
      [](auto& p) { p.evaluatedBefore.pbrMetallicRoughness.metallicFactor = std::nextafter(p.evaluatedBefore.pbrMetallicRoughness.metallicFactor, 1.); },
      [](auto& p) { p.evaluatedAfter.extensions["VENDOR_material"].template Get<Object>()["opaqueIndex"] = Value(38); }};
  const auto before = evaluated;
  const auto controls = player;
  for (size_t i = 0; i < changes.size(); ++i) {
    SCOPED_TRACE(i);
    auto forged = plan;
    changes[i](forged);
    EXPECT_THROW(player.withMaterialEdits(evaluated, forged), std::invalid_argument);
    controlsEqual(player, controls);
    EXPECT_EQ(evaluated, before);
  }
}

TEST(GltfMaterialAnimation, StaleEvaluatedRestAndIndependentResourceContextsRejectWithoutMutation) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, .3));
  auto staleEvaluated = evaluated;
  player.seek(staleEvaluated, 1.5);
  EXPECT_THROW(player.withMaterialEdits(staleEvaluated, plan), std::invalid_argument);
  auto differentArchive = fixture.authored;
  differentArchive.materials[0].pbrMetallicRoughness.roughnessFactor = std::nextafter(.75, 1.);
  const auto wrongRest = PlanGltfMaterialEdit(differentArchive, evaluated, 0, number(Property::Roughness, .3));
  EXPECT_THROW(player.withMaterialEdits(evaluated, wrongRest), std::invalid_argument);
  auto changedTextures = evaluated;
  changedTextures.textures.emplace_back();
  EXPECT_THROW(player.withMaterialEdits(changedTextures, plan), std::invalid_argument);
  auto changedNodes = evaluated;
  changedNodes.nodes.emplace_back();
  EXPECT_THROW(player.withMaterialEdits(changedNodes, plan), std::invalid_argument);
  const auto changedPlayer = player.withMaterialEdits(evaluated, plan);
  EXPECT_THROW(changedPlayer.withMaterialEdits(evaluated, plan), std::invalid_argument);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.roughnessFactor, .75);
}

TEST(GltfMaterialAnimation, ChangedSourceCurveBitsInterpolationAndTargetsReject) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, .3));
  auto changed = evaluated;
  const auto output = changed.animations[0].samplers[0].output;
  const auto view = changed.accessors[size_t(output)].bufferView;
  auto& data = changed.buffers[size_t(changed.bufferViews[size_t(view)].buffer)].data;
  const auto oneUlp = std::nextafter(.2f, 1.f);
  std::memcpy(data.data(), &oneUlp, sizeof(oneUlp));
  EXPECT_THROW(player.withMaterialEdits(changed, plan), std::invalid_argument);
  changed = evaluated;
  changed.animations[0].samplers[0].interpolation = "STEP";
  EXPECT_THROW(player.withMaterialEdits(changed, plan), std::invalid_argument);
  changed = evaluated;
  changed.animations[0].channels[1].target_node = 1; // Matrix target is also invalid.
  EXPECT_THROW(player.withMaterialEdits(changed, plan), std::invalid_argument);
  changed = evaluated;
  changed.animations[0].channels.back().target_extensions["KHR_animation_pointer"] =
      Value(Object{{"pointer", Value(std::string("/materials/0/anotherFutureProperty"))}});
  EXPECT_THROW(player.withMaterialEdits(changed, plan), std::invalid_argument);
}

TEST(GltfMaterialAnimation, OneUlpAndSignedZeroRemainExactInAuthoredRestAndUntrackedSampling) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  AnimationPlayer player(fixture.authored);
  const auto adjacent = std::nextafter(.75, 1.);
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, adjacent));
  auto next = player.withMaterialEdits(evaluated, plan);
  EXPECT_EQ(std::bit_cast<uint64_t>(next.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor),
      std::bit_cast<uint64_t>(adjacent));
  EXPECT_EQ(std::bit_cast<uint64_t>(next.evaluateMaterialRestEdit(0, plan.evaluatedAfter).pbrMetallicRoughness.roughnessFactor),
      std::bit_cast<uint64_t>(adjacent));
  const auto zero = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Metallic, -0.));
  next = player.withMaterialEdits(evaluated, zero);
  EXPECT_TRUE(std::signbit(next.library().restMaterials[0].pbrMetallicRoughness.metallicFactor));
  EXPECT_TRUE(std::signbit(next.evaluateMaterialRestEdit(0, zero.evaluatedAfter).pbrMetallicRoughness.metallicFactor));
  EXPECT_FALSE(std::signbit(player.library().restMaterials[0].pbrMetallicRoughness.metallicFactor));
}

TEST(GltfMaterialAnimation, CopyLimitsBoundWorkBeforePublicationWithoutChargingUnusedImagePayloads) {
  Fixture fixture;
  fixture.authored.images.emplace_back();
  fixture.authored.images.back().image.resize(8 * 1024 * 1024, 73);
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto before = evaluated.materials;
  const auto controls = player;
  const auto plan = PlanGltfMaterialEdit(fixture.authored, evaluated, 0, number(Property::Roughness, .3));
  GltfMaterialAnimationEditLimits bounded;
  bounded.maxOwnedBytes = 1024 * 1024; // Much smaller than the unused image.
  EXPECT_NO_THROW(player.withMaterialEdits(evaluated, plan, bounded));
  const std::vector<std::function<void(GltfMaterialAnimationEditLimits&)>> limits{
      [](auto& l) { l.maxOwnedBytes = 0; },
      [](auto& l) { l.maxOwnedBytes = UINT64_MAX; },
      [](auto& l) { l.maxOwnedBytes = 1; },
      [](auto& l) { l.maxDecodedBytes = 1; },
      [](auto& l) { l.maxMetadataValues = 1; },
      [](auto& l) { l.maxChannels = 1; },
      [](auto& l) { l.maxClips = 1; },
      [](auto& l) { l.maxMetadataDepth = 0; },
      [](auto& l) { l.authoring.maxOutputBytes = 1; }};
  for (size_t i = 0; i < limits.size(); ++i) {
    SCOPED_TRACE(i);
    GltfMaterialAnimationEditLimits invalid;
    limits[i](invalid);
    EXPECT_THROW(player.withMaterialEdits(evaluated, plan, invalid), std::invalid_argument);
    EXPECT_EQ(evaluated.materials, before);
    controlsEqual(player, controls);
  }
}

TEST(GltfMaterialAnimation, DetachedMaterialSamplerRejectsInvalidIdentityAndBudgetAtomically) {
  Fixture fixture;
  auto evaluated = fixture.authored;
  auto player = fixture.running(evaluated);
  const auto before = evaluated;
  const auto controls = player;
  EXPECT_THROW(player.evaluateMaterialRestEdit(2, evaluated.materials[0]), std::invalid_argument);
  GltfMaterialAnimationEditLimits limits;
  limits.maxOwnedBytes = 1;
  EXPECT_THROW(player.evaluateMaterialRestEdit(0, evaluated.materials[0], limits), std::invalid_argument);
  limits = {};
  limits.maxChannels = 1;
  EXPECT_THROW(player.evaluateMaterialRestEdit(0, evaluated.materials[0], limits), std::invalid_argument);
  EXPECT_EQ(evaluated, before);
  controlsEqual(player, controls);
}

TEST(GltfMaterialAnimation, DetachedTailRemovalPreservesCurrentPrefixRestsClockAndEverySourceByte) {
  Fixture fixture;
  auto evaluated=fixture.authored;
  auto player=fixture.running(evaluated);
  const auto duplicate=PlanGltfMaterialDuplicate(fixture.authored,evaluated,0);
  publish(fixture.authored,evaluated,player,duplicate);
  const auto prefixEdit=PlanGltfMaterialEdit(fixture.authored,evaluated,0,number(Property::Roughness,.27));
  publish(fixture.authored,evaluated,player,prefixEdit);
  player.seek(evaluated,1.25);
  player.setLoop(true);
  player.setSpeed(-1.75);
  const auto controls=player;
  const auto before=evaluated;
  const auto* materialData=evaluated.materials.data();
  const auto* imageData=evaluated.images[0].image.data();
  const auto next=player.withoutLastMaterialRest(evaluated,duplicate.authoredAfter);
  ASSERT_EQ(next.library().restMaterials.size(),2u);
  ASSERT_EQ(player.library().restMaterials.size(),3u);
  controlsEqual(next,controls);
  EXPECT_EQ(next.library().clips,player.library().clips);
  ASSERT_EQ(next.library().diagnostics.size(),player.library().diagnostics.size());
  for(size_t i=0;i<next.library().diagnostics.size();++i) {
    EXPECT_EQ(next.library().diagnostics[i].clip,player.library().diagnostics[i].clip);
    EXPECT_EQ(next.library().diagnostics[i].channel,player.library().diagnostics[i].channel);
    EXPECT_EQ(next.library().diagnostics[i].message,player.library().diagnostics[i].message);
  }
  for(size_t i=0;i<2;++i) EXPECT_TRUE(GltfMaterialRecordsEqual(next.library().restMaterials[i],player.library().restMaterials[i]));
  EXPECT_EQ(next.library().restMaterials[0].pbrMetallicRoughness.roughnessFactor,.27);
  EXPECT_EQ(next.library().restMaterials[0].pbrMetallicRoughness.metallicFactor,.125);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,curve(.2f,.8f,1.25));
  EXPECT_EQ(next.library().restNodes,player.library().restNodes);
  EXPECT_EQ(next.library().restLights,player.library().restLights);
  EXPECT_EQ(next.library().restCameras,player.library().restCameras);
  EXPECT_EQ(evaluated,before);
  EXPECT_EQ(evaluated.materials.data(),materialData);
  EXPECT_EQ(evaluated.images[0].image.data(),imageData);
  for(size_t i=0;i<before.buffers.size();++i) EXPECT_EQ(evaluated.buffers[i].data,before.buffers[i].data);
  // A resource owner would detach this same source tail separately. A fresh
  // trusted append can then restore it without copying tracks or old controls.
  auto detachedAuthored=fixture.authored;
  auto detachedEvaluated=evaluated;
  detachedAuthored.materials.pop_back();
  detachedEvaluated.materials.pop_back();
  const auto redo=PlanGltfMaterialAppend(detachedAuthored,detachedEvaluated,duplicate.authoredAfter,duplicate.evaluatedAfter);
  const auto restored=next.withMaterialEdits(detachedEvaluated,redo);
  controlsEqual(restored,controls);
  EXPECT_EQ(restored.library().clips,player.library().clips);
  for(size_t i=0;i<3;++i) EXPECT_TRUE(GltfMaterialRecordsEqual(restored.library().restMaterials[i],player.library().restMaterials[i]));
}

TEST(GltfMaterialAnimation, LastUntrackedRestCanBeRemovedToEmptyWithoutSelectingOrResettingPlayback) {
  tinygltf::Model source;
  source.materials.resize(1);
  source.materials[0].pbrMetallicRoughness.metallicFactor=-0.;
  AnimationPlayer player(source);
  player.setSpeed(-0.);
  player.setLoop(false);
  const auto before=source;
  const auto next=player.withoutLastMaterialRest(source,source.materials.back());
  EXPECT_TRUE(next.library().restMaterials.empty());
  controlsEqual(next,player);
  EXPECT_TRUE(std::signbit(next.speed()));
  EXPECT_EQ(source,before);
  EXPECT_THROW(next.withoutLastMaterialRest(source,source.materials.back()),std::invalid_argument);
  tinygltf::Model empty;
  AnimationPlayer emptyPlayer(empty);
  EXPECT_THROW(emptyPlayer.withoutLastMaterialRest(empty,source.materials.back()),std::invalid_argument);
}

TEST(GltfMaterialAnimation, TailRestWitnessUsesExactBitsTypesRawCachesAndIndependentTextureSpace) {
  Fixture fixture;
  fixture.authored.textures.resize(5);
  auto evaluated=fixture.authored;
  evaluated.textures.resize(2);
  auto player=fixture.running(evaluated);
  tinygltf::Material authored,visible;
  authored.pbrMetallicRoughness.roughnessFactor=.3;
  authored.pbrMetallicRoughness.metallicFactor=-0.;
  authored.normalTexture.index=4;
  authored.extras=Value(Object{{"identity",Value(17)}});
  authored.extras_json_string="trusted raw extras";
  authored.extensions_json_string="trusted raw extensions";
  authored.lods={0};
  visible=authored;
  visible.normalTexture.index=1;
  const auto append=PlanGltfMaterialAppend(fixture.authored,evaluated,authored,visible);
  publish(fixture.authored,evaluated,player,append);
  EXPECT_NO_THROW(player.withoutLastMaterialRest(evaluated,authored));
  const std::vector<std::function<void(tinygltf::Material&)>> mutations{
      [](auto& m){m.pbrMetallicRoughness.roughnessFactor=std::nextafter(.3,1.);},
      [](auto& m){m.pbrMetallicRoughness.metallicFactor=0.;},
      [](auto& m){m.extras.template Get<Object>()["identity"]=Value(17.);},
      [](auto& m){m.extras_json_string+=" stale";},
      [](auto& m){m.extensions_json_string+=" stale";},
      [](auto& m){m.normalTexture.index=1;},
      [](auto& m){m.lods.clear();}};
  for(size_t i=0;i<mutations.size();++i) {
    SCOPED_TRACE(i);
    auto stale=authored;
    mutations[i](stale);
    EXPECT_THROW(player.withoutLastMaterialRest(evaluated,stale),std::invalid_argument);
  }
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials.back(),authored));
  EXPECT_EQ(evaluated.materials.back().normalTexture.index,1);
}

TEST(GltfMaterialAnimation, TailRemovalRejectsSupportedUnsupportedWholeAndEscapedRawMaterialTargets) {
  const std::array<std::string,6> paths{
      "/materials/2/pbrMetallicRoughness/roughnessFactor",
      "/materials/2/futureFactor",
      "/materials/2",
      "/materials/99/future~1factor",
      "/materials/99",
      "/materials"};
  for(const auto& path:paths) {
    SCOPED_TRACE(path);
    Fixture fixture;
    fixture.authored.materials.emplace_back();
    auto& clip=fixture.authored.animations[0];
    pointer(clip,clip.samplers[0].input,clip.samplers[0].output,path);
    auto source=fixture.authored;
    AnimationPlayer player(source);
    const auto before=source;
    EXPECT_THROW(player.withoutLastMaterialRest(source,source.materials.back()),std::invalid_argument);
    EXPECT_EQ(source,before);
    EXPECT_EQ(player.library().restMaterials.size(),3u);
  }
}

TEST(GltfMaterialAnimation, RawUnsupportedTailTargetCannotHideBehindUnchangedDecodedDiagnostics) {
  Fixture fixture;
  auto evaluated=fixture.authored;
  auto player=fixture.running(evaluated);
  const auto duplicate=PlanGltfMaterialDuplicate(fixture.authored,evaluated,0);
  publish(fixture.authored,evaluated,player,duplicate);
  const auto originalDiagnostic=player.library().diagnostics[0].message;
  evaluated.animations[0].channels[4].target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"]=
      Value(std::string("/materials/2/futureFactor"));
  AnimationPlayer reparsed(evaluated);
  EXPECT_EQ(reparsed.library().clips,player.library().clips);
  EXPECT_EQ(reparsed.library().diagnostics[0].message,originalDiagnostic);
  EXPECT_THROW(player.withoutLastMaterialRest(evaluated,duplicate.authoredAfter),std::invalid_argument);
  // Current opaque raw metadata is not a historical snapshot comparison. It
  // remains valid and unchanged when every current target stays in the prefix.
  evaluated.animations[0].channels[4].target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"]=
      Value(std::string("/materials/0/futureFactor"));
  evaluated.animations[0].extras_json_string="current opaque bytes";
  const auto before=evaluated;
  EXPECT_NO_THROW(player.withoutLastMaterialRest(evaluated,duplicate.authoredAfter));
  EXPECT_EQ(evaluated.animations[0].extras_json_string,"current opaque bytes");
  EXPECT_EQ(evaluated,before);
}

TEST(GltfMaterialAnimation, TailRemovalRejectsChangedCurvesAndSourceOrDecodedIdentityShapesAtomically) {
  Fixture fixture;
  auto evaluated=fixture.authored;
  auto player=fixture.running(evaluated);
  const auto duplicate=PlanGltfMaterialDuplicate(fixture.authored,evaluated,0);
  publish(fixture.authored,evaluated,player,duplicate);
  const auto controls=player;
  const std::vector<std::function<void(tinygltf::Model&)>> mutations{
      [](auto& m){m.materials.pop_back();},
      [](auto& m){m.nodes.emplace_back();},
      [](auto& m){m.cameras.emplace_back();},
      [](auto& m){m.lights.emplace_back();},
      [](auto& m){m.animations.pop_back();},
      [](auto& m){m.animations[0].samplers[0].interpolation="STEP";},
      [](auto& m){m.animations[0].name+=" changed";},
      [](auto& m){
        const auto output=m.animations[0].samplers[0].output;
        const auto view=m.accessors[size_t(output)].bufferView;
        auto& bytes=m.buffers[size_t(m.bufferViews[size_t(view)].buffer)].data;
        const auto changed=std::nextafter(.2f,1.f);
        std::memcpy(bytes.data(),&changed,sizeof(changed));
      }};
  for(size_t i=0;i<mutations.size();++i) {
    SCOPED_TRACE(i);
    auto stale=evaluated;
    mutations[i](stale);
    const auto before=stale;
    EXPECT_THROW(player.withoutLastMaterialRest(stale,duplicate.authoredAfter),std::invalid_argument);
    EXPECT_EQ(stale,before);
    controlsEqual(player,controls);
  }
  auto forgedPlayer=player;
  auto& library=const_cast<AnimationLibrary&>(forgedPlayer.library());
  library.clips[0].tracks[0].target.index=99;
  EXPECT_THROW(forgedPlayer.withoutLastMaterialRest(evaluated,duplicate.authoredAfter),std::invalid_argument);
  EXPECT_EQ(player.library().clips,controls.library().clips);
}

TEST(GltfMaterialAnimation, TailRemovalBoundsCopiesDecodedWorkAndMetadataWithoutCopyingUnusedImages) {
  Fixture fixture;
  fixture.authored.images.emplace_back();
  fixture.authored.images.back().image.resize(8*1024*1024,73);
  auto evaluated=fixture.authored;
  auto player=fixture.running(evaluated);
  const auto duplicate=PlanGltfMaterialDuplicate(fixture.authored,evaluated,0);
  publish(fixture.authored,evaluated,player,duplicate);
  const auto controls=player;
  const auto* images=evaluated.images.back().image.data();
  GltfMaterialAnimationEditLimits bounded;
  bounded.maxOwnedBytes=1024*1024;
  EXPECT_NO_THROW(player.withoutLastMaterialRest(evaluated,duplicate.authoredAfter,bounded));
  const std::vector<std::function<void(GltfMaterialAnimationEditLimits&)>> mutations{
      [](auto& l){l.maxOwnedBytes=0;},
      [](auto& l){l.maxOwnedBytes=UINT64_MAX;},
      [](auto& l){l.maxOwnedBytes=1;},
      [](auto& l){l.maxDecodedBytes=1;},
      [](auto& l){l.maxDecodedBytes=UINT64_MAX;},
      [](auto& l){l.maxMetadataValues=1;},
      [](auto& l){l.maxMetadataDepth=0;},
      [](auto& l){l.maxChannels=1;},
      [](auto& l){l.maxClips=1;},
      [](auto& l){l.authoring.maxOutputBytes=1;}};
  for(size_t i=0;i<mutations.size();++i) {
    SCOPED_TRACE(i);
    GltfMaterialAnimationEditLimits limits;
    mutations[i](limits);
    EXPECT_THROW(player.withoutLastMaterialRest(evaluated,duplicate.authoredAfter,limits),std::invalid_argument);
    controlsEqual(player,controls);
  }
  auto excessive=evaluated;
  excessive.animations[0].channels[4].target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"]=
      Value(std::string("/materials/0/")+std::string(20000,'/'));
  EXPECT_THROW(player.withoutLastMaterialRest(excessive,duplicate.authoredAfter,bounded),std::invalid_argument);
  EXPECT_EQ(evaluated.images.back().image.data(),images);
  EXPECT_EQ(evaluated.images.back().image.size(),8u*1024*1024);
  EXPECT_EQ(player.library().restMaterials.size(),3u);
}

TEST(GltfMaterialAnimation, AppendCannotMaterializeAnUnsupportedRawPointerAtItsDestinationIdentity) {
  const std::array<std::string,3> paths{
      "/materials/2/future~1factor", "/materials/2", "/materials/99/future~0factor"};
  for(const auto& path:paths) {
    SCOPED_TRACE(path);
    Fixture fixture;
    fixture.authored.animations[0].channels[4].target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"]=Value(path);
    auto evaluated=fixture.authored;
    AnimationPlayer player(evaluated); // Unsupported targets are diagnostics.
    tinygltf::Material authored,visible;
    authored.name="Prospective untracked material";
    visible=authored;
    const auto plan=PlanGltfMaterialAppend(fixture.authored,evaluated,authored,visible);
    ASSERT_EQ(plan.destinationMaterial,2);
    const auto before=evaluated;
    const auto controls=player;
    EXPECT_THROW(player.withMaterialEdits(evaluated,plan),std::invalid_argument);
    EXPECT_EQ(evaluated,before);
    EXPECT_EQ(player.library().restMaterials.size(),2u);
    controlsEqual(player,controls);
  }
}

TEST(GltfMaterialAnimation, MaterialCopyPreflightRejectsInactiveBackingStorageInRawAndRetainedMetadata) {
  Fixture fixture;
  auto evaluated=fixture.authored;
  auto player=fixture.running(evaluated);
  const auto duplicate=PlanGltfMaterialDuplicate(fixture.authored,evaluated,0);
  publish(fixture.authored,evaluated,player,duplicate);
  auto malformedPlayer=player;
  auto& prefix=const_cast<AnimationLibrary&>(malformedPlayer.library()).restMaterials[0];
  prefix.extras.Get<Value::Array>().emplace_back(17); // Object tag with hidden array.
  EXPECT_THROW(malformedPlayer.withoutLastMaterialRest(evaluated,duplicate.authoredAfter),std::invalid_argument);
  malformedPlayer=player;
  auto& node=const_cast<AnimationLibrary&>(malformedPlayer.library()).restNodes[0];
  node.extras.Get<Value::Array>().emplace_back(19); // Null tag with hidden array.
  EXPECT_THROW(malformedPlayer.withoutLastMaterialRest(evaluated,duplicate.authoredAfter),std::invalid_argument);
  auto malformedSource=evaluated;
  malformedSource.animations[0].extras.Get<Value::Array>().emplace_back(23);
  EXPECT_THROW(player.withoutLastMaterialRest(malformedSource,duplicate.authoredAfter),std::invalid_argument);
  EXPECT_EQ(player.library().restMaterials.size(),3u);
  EXPECT_EQ(evaluated.animations[0].extras.Get<Object>().at("clipIdentity").Get<int>(),29);
}
GltfMaterialAuthoringPlan publishIndependentTexture(Fixture& fixture,tinygltf::Model& evaluated,AnimationPlayer& player) {
  auto futureAuthored=fixture.authored,futureEvaluated=evaluated;
  futureAuthored.textures.resize(3);futureEvaluated.textures.resize(5);
  GltfMaterialTextureEdit edit;edit.slot=GltfMaterialTextureSlot::BaseColor;edit.operation=GltfMaterialTextureOperation::Bind;
  edit.binding=GltfMaterialTextureSourcePair{2,4};GltfMaterialPatch patch;patch.textures.push_back(edit);
  const auto plan=PlanGltfMaterialTextureEdit(GltfMaterialSourceView(fixture.authored),GltfMaterialSourceView(evaluated),
      GltfMaterialSourceView(futureAuthored),GltfMaterialSourceView(futureEvaluated),0,
      fixture.authored.materials[0],evaluated.materials[0],patch);
  publish(fixture.authored,evaluated,player,plan);
  fixture.authored.textures.resize(3);evaluated.textures.resize(5);
  return plan;
}
void addTextureComponentTrack(Fixture& fixture) {
  fixture.authored.extensionsUsed.push_back("KHR_texture_transform");
  auto& texture=fixture.authored.materials[0].pbrMetallicRoughness.baseColorTexture;texture.index=0;
  texture.extensions["KHR_texture_transform"]=Value(Object{
      {"offset",Value(Value::Array{Value(.25),Value(-.5)})},{"vendor",Value(71)}});
  texture.extensions_json_string="authored transform cache";
  const auto input=fixture.authored.animations[0].samplers[0].input;
  pointer(fixture.authored.animations[0],input,floats(fixture.authored,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2),
      "/materials/0/pbrMetallicRoughness/baseColorTexture/extensions/KHR_texture_transform/offset/0");
}

TEST(GltfMaterialAnimation, SelectSwitchClearAndFreeRestoreRetainIndependentEvaluatedTextureDomain) {
  Fixture fixture;auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  const auto plan=publishIndependentTexture(fixture,evaluated,player);
  auto& current=evaluated.materials[0];current.pbrMetallicRoughness.roughnessFactor=.3125;
  current.extras.Get<Object>()["currentEvaluatedMetadata"]=Value(97);current.extras_json_string="current evaluated extras";
  const auto opaque=current.extras;
  const auto retained=[&] {
    EXPECT_EQ(current.pbrMetallicRoughness.baseColorTexture.index,4);
    EXPECT_EQ(current.pbrMetallicRoughness.roughnessFactor,.3125);
    EXPECT_EQ(current.extras,opaque);EXPECT_EQ(current.extras_json_string,"current evaluated extras");
    EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],plan.authoredAfter));
    EXPECT_EQ(player.library().restMaterials[0].pbrMetallicRoughness.baseColorTexture.index,2);
  };
  retained();player.selectClip(evaluated,0);retained();
  EXPECT_EQ(current.pbrMetallicRoughness.metallicFactor,curve(.2f,.8f,0));
  player.seek(evaluated,1.25);retained();
  player.selectClip(evaluated,1);retained();
  EXPECT_EQ(current.pbrMetallicRoughness.metallicFactor,curve(.1f,.5f,0));
  player.clearClip(evaluated);retained();EXPECT_EQ(current.pbrMetallicRoughness.metallicFactor,.125);
  current.pbrMetallicRoughness.metallicFactor=.9;
  const auto dirty=RestoreAnimationRestPose(evaluated,player.library());retained();
  EXPECT_EQ(current.pbrMetallicRoughness.metallicFactor,.125);
  EXPECT_NE(std::ranges::find(dirty.materials,size_t(0)),dirty.materials.end());
}

TEST(GltfMaterialAnimation, MaterialTextureComponentRestsPreserveEvaluatedSiblingsAndReconcileRawCachesOnce) {
  Fixture fixture;addTextureComponentTrack(fixture);auto evaluated=fixture.authored;
  auto& initial=evaluated.materials[0].pbrMetallicRoughness.baseColorTexture;initial.index=1;
  auto& initialTransform=initial.extensions.at("KHR_texture_transform").Get<Object>();
  initialTransform["offset"].Get<Value::Array>()[1]=Value(.75);initialTransform["vendor"]=Value(73);
  initial.extras=Value(Object{{"evaluatedTextureIdentity",Value(99)}});initial.extensions_json_string="evaluated transform cache";
  auto player=fixture.running(evaluated);const auto plan=publishIndependentTexture(fixture,evaluated,player);
  auto& texture=evaluated.materials[0].pbrMetallicRoughness.baseColorTexture;
  const auto component=[&](size_t index) {return texture.extensions.at("KHR_texture_transform").Get("offset").Get(int(index)).GetNumberAsDouble();};
  const auto retained=[&] {
    EXPECT_EQ(texture.index,4);EXPECT_EQ(component(1),1.25);
    EXPECT_EQ(texture.extensions.at("KHR_texture_transform").Get("vendor").Get<int>(),73);
    EXPECT_EQ(texture.extras,Value(Object{{"evaluatedTextureIdentity",Value(99)}}));
  };
  texture.extensions.at("KHR_texture_transform").Get<Object>()["offset"].Get<Value::Array>()[1]=Value(1.25);
  texture.extensions_json_string="valid current evaluated cache";
  player.selectClip(evaluated,0);retained();EXPECT_EQ(component(0),curve(.2f,.8f,0));EXPECT_TRUE(texture.extensions_json_string.empty());
  texture.extensions_json_string="valid cache before seek";
  player.seek(evaluated,1.5);retained();EXPECT_EQ(component(0),curve(.2f,.8f,1.5));EXPECT_TRUE(texture.extensions_json_string.empty());
  player.selectClip(evaluated,1);retained();EXPECT_EQ(component(0),.25);EXPECT_TRUE(texture.extensions_json_string.empty());
  texture.extensions_json_string="already resting evaluated cache";
  player.clearClip(evaluated);retained();EXPECT_EQ(component(0),.25);EXPECT_EQ(texture.extensions_json_string,"already resting evaluated cache");
  const auto& rest=plan.authoredAfter.pbrMetallicRoughness.baseColorTexture;
  texture.extensions=rest.extensions;
  texture.extensions.at("KHR_texture_transform").Get<Object>()["offset"].Get<Value::Array>()[0]=Value(.9);
  texture.extensions_json_string="cache before matching authored transform";
  RestoreAnimationRestPose(evaluated,player.library());
  EXPECT_EQ(texture.index,4);EXPECT_EQ(texture.extensions,rest.extensions);
  EXPECT_EQ(texture.extensions_json_string,"authored transform cache");
  EXPECT_EQ(texture.extras,Value(Object{{"evaluatedTextureIdentity",Value(99)}}));
}

TEST(GltfMaterialAnimation, MissingEvaluatedTextureTargetRollsBackSelectionClearSeekAndFreeRestore) {
  Fixture fixture;addTextureComponentTrack(fixture);auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  const auto plan=publishIndependentTexture(fixture,evaluated,player);
  evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.extensions.erase("KHR_texture_transform");
  const auto before=evaluated;const auto controls=player;
  const auto exact=[&] {
    controlsEqual(player,controls);EXPECT_EQ(evaluated,before);
    for(size_t i=0;i<before.materials.size();++i) EXPECT_TRUE(GltfMaterialRecordsEqual(evaluated.materials[i],before.materials[i]));
    EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.index,4);
  };
  EXPECT_THROW(player.selectClip(evaluated,1),std::invalid_argument);exact();
  EXPECT_THROW(player.clearClip(evaluated),std::invalid_argument);exact();
  EXPECT_THROW(player.seek(evaluated,1.5),std::invalid_argument);exact();
  EXPECT_THROW(RestoreAnimationRestPose(evaluated,player.library()),std::invalid_argument);exact();
  evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.extensions=plan.evaluatedAfter.pbrMetallicRoughness.baseColorTexture.extensions;
  EXPECT_NO_THROW(player.selectClip(evaluated,1));EXPECT_EQ(player.selectedClip(),1u);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.index,4);
}
TEST(GltfMaterialAnimation, OmittedVectorComponentsRestoreDefaultsWithoutRetypingIndependentEvaluatedSiblings) {
  struct Schema {const char* extension;const char* leaf;size_t width,element;double value;bool texture;};
  const std::array schemas{
      Schema{"KHR_materials_specular","specularColorFactor",3,1,1.,false},
      Schema{"KHR_materials_sheen","sheenColorFactor",3,1,0.,false},
      Schema{"KHR_texture_transform","offset",2,0,0.,true},
      Schema{"KHR_texture_transform","scale",2,0,1.,true}};
  for(const auto& schema:schemas) {
    SCOPED_TRACE(schema.leaf);Fixture fixture;fixture.authored.extensionsUsed.push_back(schema.extension);
    if(schema.texture) {
      auto& texture=fixture.authored.materials[0].pbrMetallicRoughness.baseColorTexture;texture.index=0;
      texture.extensions[schema.extension]=Value(Object{{"vendor",Value(19)}});
    } else fixture.authored.materials[0].extensions[schema.extension]=Value(Object{{"vendor",Value(19)}});
    const std::string parent=schema.texture?"pbrMetallicRoughness/baseColorTexture/extensions/":"extensions/";
    const auto input=fixture.authored.animations[0].samplers[0].input;
    pointer(fixture.authored.animations[0],input,floats(fixture.authored,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2),
        "/materials/0/"+parent+schema.extension+"/"+schema.leaf+"/"+std::to_string(schema.element));
    auto evaluated=fixture.authored;
    const auto object=[&]() -> Object& {
      auto& material=evaluated.materials[0];
      return schema.texture?material.pbrMetallicRoughness.baseColorTexture.extensions.at(schema.extension).Get<Object>():
          material.extensions.at(schema.extension).Get<Object>();
    };
    object()["vendor"]=Value(29);
    auto player=fixture.running(evaluated);const auto plan=publishIndependentTexture(fixture,evaluated,player);
    const size_t sibling=schema.element?0:1;
    for(size_t variant=0;variant<3;++variant) {
      SCOPED_TRACE(variant);Value independent=variant==0?Value(schema.value==0?.75:.25):
          variant==1?Value(int(schema.value)):Value(-0.);
      Value::Array values(schema.width,Value(schema.value));values[schema.element]=Value(.6);values[sibling]=independent;
      object()[schema.leaf]=Value(std::move(values));
      const auto exactSibling=[&] {
        ASSERT_TRUE(object().contains(schema.leaf));const auto& value=object().at(schema.leaf).Get(int(sibling));
        EXPECT_EQ(value.Type(),independent.Type());
        EXPECT_EQ(std::bit_cast<uint64_t>(value.GetNumberAsDouble()),std::bit_cast<uint64_t>(independent.GetNumberAsDouble()));
        EXPECT_EQ(object().at("vendor").Get<int>(),29);
        EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.index,4);
        EXPECT_EQ(player.library().restMaterials[0].pbrMetallicRoughness.baseColorTexture.index,2);
      };
      player.selectClip(evaluated,0);exactSibling();player.seek(evaluated,1.5);exactSibling();
      player.selectClip(evaluated,1);exactSibling();player.clearClip(evaluated);exactSibling();
      RestoreAnimationRestPose(evaluated,player.library());exactSibling();
      const auto detached=player.evaluateMaterialRestEdit(0,evaluated.materials[0]);
      EXPECT_TRUE(GltfMaterialRecordsEqual(detached,evaluated.materials[0]));
      EXPECT_EQ(object().at(schema.leaf).Get(int(schema.element)).GetNumberAsDouble(),schema.value);
    }
    // Without a distinct sibling value/type/bit pattern, a vector created by
    // sampling returns to the authored omission rather than explicit defaults.
    object()[schema.leaf]=Value(Value::Array(schema.width,Value(schema.value)));
    RestoreAnimationRestPose(evaluated,player.library());EXPECT_FALSE(object().contains(schema.leaf));
    EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.baseColorTexture.index,4);
    EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[0],plan.authoredAfter));
  }
}
} // namespace

namespace {
Fixture unusedMiddleMaterialFixture() {
  Fixture fixture;
  tinygltf::Material unused;
  unused.name="Unused middle material";
  unused.normalTexture.index=4;
  unused.extras=Value(Object{{"negativeZero",Value(-0.)},{"integer",Value(16777217)}});
  unused.extras_json_string="retained middle raw extras";
  fixture.authored.materials.insert(fixture.authored.materials.begin()+1,unused);
  fixture.authored.textures.resize(5);
  for(size_t i=2;i<5;++i) {fixture.authored.textures[i].source=0;fixture.authored.textures[i].sampler=0;}
  fixture.authored.animations[1].channels[1].target_extensions["KHR_animation_pointer"]=
      Value(Object{{"pointer",Value("/materials/2/pbrMetallicRoughness/roughnessFactor")}});
  return fixture;
}

TEST(GltfMaterialAnimation, UntrackedMiddleRestRetiresAndRestoresWithoutRenumberingOrRewindingControls) {
  const auto fixture=unusedMiddleMaterialFixture();
  auto evaluated=fixture.authored;evaluated.textures.resize(2);evaluated.materials[1].normalTexture.index=1;
  auto player=fixture.running(evaluated,.75);const auto controls=player;
  const auto before=evaluated;const auto originalRest=fixture.authored.materials[1];
  const tinygltf::Material tombstone;
  auto retired=player.withUntrackedMaterialRest(evaluated,GltfMaterialSourceView(fixture.authored),1,originalRest,tombstone);
  controlsEqual(retired,controls);controlsEqual(player,controls);
  ASSERT_EQ(retired.library().restMaterials.size(),3u);
  EXPECT_TRUE(GltfMaterialRecordsEqual(retired.library().restMaterials[0],player.library().restMaterials[0]));
  EXPECT_TRUE(GltfMaterialRecordsEqual(retired.library().restMaterials[1],tombstone));
  EXPECT_TRUE(GltfMaterialRecordsEqual(retired.library().restMaterials[2],player.library().restMaterials[2]));
  EXPECT_EQ(retired.library().clips,player.library().clips);
  ASSERT_EQ(retired.library().diagnostics.size(),player.library().diagnostics.size());
  for(size_t i=0;i<player.library().diagnostics.size();++i) {
    EXPECT_EQ(retired.library().diagnostics[i].clip,player.library().diagnostics[i].clip);
    EXPECT_EQ(retired.library().diagnostics[i].channel,player.library().diagnostics[i].channel);
    EXPECT_EQ(retired.library().diagnostics[i].message,player.library().diagnostics[i].message);
  }
  EXPECT_EQ(evaluated,before);
  evaluated.materials[1]=tombstone;
  retired.seek(evaluated,1.75);
  const auto current=retired;
  auto restored=retired.withUntrackedMaterialRest(evaluated,GltfMaterialSourceView(fixture.authored),1,tombstone,originalRest);
  controlsEqual(restored,current);
  EXPECT_TRUE(GltfMaterialRecordsEqual(restored.library().restMaterials[1],originalRest));
  EXPECT_EQ(restored.library().restMaterials[1].normalTexture.index,4);
  EXPECT_EQ(restored.evaluateMaterialRestEdit(1,before.materials[1]).normalTexture.index,1);
  EXPECT_EQ(evaluated.materials[0].pbrMetallicRoughness.metallicFactor,curve(.2f,.8f,1.75));
  EXPECT_EQ(evaluated.animations,before.animations);
}

TEST(GltfMaterialAnimation, UntrackedRetirementRejectsTargetsFromInactiveClipsAndRawUnsupportedProperties) {
  auto fixture=unusedMiddleMaterialFixture();auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  const auto before=evaluated;const auto controls=player;const tinygltf::Material tombstone;
  for(const size_t target:{size_t(0),size_t(2)})
    EXPECT_THROW(player.withUntrackedMaterialRest(evaluated,GltfMaterialSourceView(fixture.authored),target,
        fixture.authored.materials[target],tombstone),std::invalid_argument);
  fixture.authored.animations[0].channels[4].target_extensions["KHR_animation_pointer"]=
      Value(Object{{"pointer",Value("/materials/1/futureFactor/~0opaque/~1channel")}});
  auto unsupported=fixture.authored;AnimationPlayer unsupportedPlayer(fixture.authored);
  EXPECT_THROW(unsupportedPlayer.withUntrackedMaterialRest(unsupported,GltfMaterialSourceView(fixture.authored),1,
      fixture.authored.materials[1],tombstone),std::invalid_argument);
  auto whole=evaluated;
  whole.animations[0].channels[4].target_extensions["KHR_animation_pointer"]=Value(Object{{"pointer",Value("/materials")}});
  EXPECT_THROW(player.withUntrackedMaterialRest(whole,GltfMaterialSourceView(fixture.authored),1,
      fixture.authored.materials[1],tombstone),std::invalid_argument);
  controlsEqual(player,controls);EXPECT_EQ(evaluated,before);
}

TEST(GltfMaterialAnimation, UntrackedRestRequiresExactWitnessAndIndependentArchiveDomain) {
  const auto fixture=unusedMiddleMaterialFixture();auto evaluated=fixture.authored;evaluated.textures.resize(2);
  evaluated.materials[1].normalTexture.index=1;AnimationPlayer player(fixture.authored);
  const auto before=evaluated;const auto controls=player;const auto source=GltfMaterialSourceView(fixture.authored);
  const tinygltf::Material tombstone;
  for(size_t variant=0;variant<3;++variant) {
    auto stale=fixture.authored.materials[1];
    if(variant==0) stale.extras.Get<Object>()["negativeZero"]=Value(0.);
    if(variant==1) stale.extras.Get<Object>()["integer"]=Value(16777217.);
    if(variant==2) stale.extras_json_string+=" changed";
    EXPECT_THROW(player.withUntrackedMaterialRest(evaluated,source,1,stale,tombstone),std::invalid_argument);
  }
  auto retired=player.withUntrackedMaterialRest(evaluated,source,1,fixture.authored.materials[1],tombstone);
  auto wrong=source;wrong.textureCount=2;
  EXPECT_THROW(retired.withUntrackedMaterialRest(evaluated,wrong,1,tombstone,fixture.authored.materials[1]),std::invalid_argument);
  EXPECT_NO_THROW(retired.withUntrackedMaterialRest(evaluated,source,1,tombstone,fixture.authored.materials[1]));
  wrong=source;wrong.materialCount=2;
  EXPECT_THROW(player.withUntrackedMaterialRest(evaluated,wrong,1,fixture.authored.materials[1],tombstone),std::invalid_argument);
  EXPECT_THROW(player.withUntrackedMaterialRest(evaluated,source,3,fixture.authored.materials[1],tombstone),std::invalid_argument);
  controlsEqual(player,controls);EXPECT_EQ(evaluated,before);
}

TEST(GltfMaterialAnimation, UntrackedRestBoundsCoexistingLibraryValidationAndRawWorkWithoutCopyingImagePayloads) {
  auto fixture=unusedMiddleMaterialFixture();fixture.authored.images[0].image.assign(4*1024*1024,83);
  auto evaluated=fixture.authored;AnimationPlayer player(fixture.authored);
  const auto* image=evaluated.images[0].image.data();const auto* geometry=evaluated.buffers[0].data.data();
  const auto source=GltfMaterialSourceView(fixture.authored);const tinygltf::Material tombstone;
  GltfMaterialAnimationEditLimits bounded;bounded.maxOwnedBytes=1024*1024;
  EXPECT_NO_THROW(player.withUntrackedMaterialRest(evaluated,source,1,fixture.authored.materials[1],tombstone,bounded));
  for(size_t variant=0;variant<7;++variant) {
    auto limits=bounded;
    if(variant==0) limits.maxOwnedBytes=0;
    if(variant==1) limits.maxOwnedBytes=UINT64_MAX;
    if(variant==2) limits.maxOwnedBytes=1;
    if(variant==3) limits.maxMetadataValues=1;
    if(variant==4) limits.maxChannels=1;
    if(variant==5) limits.maxClips=1;
    if(variant==6) limits.maxDecodedBytes=1;
    EXPECT_THROW(player.withUntrackedMaterialRest(evaluated,source,1,fixture.authored.materials[1],tombstone,limits),std::invalid_argument);
  }
  EXPECT_EQ(evaluated.images[0].image.data(),image);EXPECT_EQ(evaluated.buffers[0].data.data(),geometry);
  EXPECT_TRUE(GltfMaterialRecordsEqual(player.library().restMaterials[1],fixture.authored.materials[1]));
}

TEST(GltfMaterialAnimation, UntrackedRestRejectsChangedCurvesAndSourceShapesBeforeReplacingAnyRest) {
  const auto fixture=unusedMiddleMaterialFixture();auto evaluated=fixture.authored;auto player=fixture.running(evaluated);
  const auto before=evaluated;const auto controls=player;const auto source=GltfMaterialSourceView(fixture.authored);const tinygltf::Material tombstone;
  auto changed=evaluated;float replacement=.4f;std::memcpy(changed.buffers[1].data.data(),&replacement,sizeof(replacement));
  EXPECT_THROW(player.withUntrackedMaterialRest(changed,source,1,fixture.authored.materials[1],tombstone),std::invalid_argument);
  changed=evaluated;changed.materials.pop_back();
  EXPECT_THROW(player.withUntrackedMaterialRest(changed,source,1,fixture.authored.materials[1],tombstone),std::invalid_argument);
  controlsEqual(player,controls);EXPECT_EQ(evaluated,before);
}
} // namespace

namespace {
tinygltf::Model smallCurveSource() {
  tinygltf::Model model; model.materials.emplace_back();
  tinygltf::Animation clip; clip.name = "One material curve";
  const auto input = floats(model, {0, 2}, TINYGLTF_TYPE_SCALAR, 2);
  const auto output = floats(model, {.2f, .8f}, TINYGLTF_TYPE_SCALAR, 2);
  pointer(clip, input, output, "/materials/0/pbrMetallicRoughness/metallicFactor");
  model.animations.push_back(clip); return model;
}
bool deniedCurveMatch(const MaterialAnimationCurveWitness& witness, const tinygltf::Model& source,
    const AnimationPlayer& player, size_t& allocations) {
  bool result{};
  { material_publication_test::AllocationGuard deny(true); result = witness.matches(source, player); allocations = deny.allocations(); }
  return result;
}

TEST(GltfMaterialAnimation, CurveWitnessMatchesLiveAndProspectiveCurvesWithoutPinningRestCountsOrControls) {
  Fixture f; auto current = f.authored; auto live = f.running(current);
  const auto plan = PlanGltfMaterialDuplicate(f.authored, current, 0);
  auto prospective = live.withMaterialEdits(current, plan);
  auto witness = prospective.prepareMaterialEditCurveWitness(current);
  ASSERT_TRUE(witness); EXPECT_GT(witness.ownedBytes(), 0u);
  EXPECT_EQ(prospective.library().restMaterials.size(), live.library().restMaterials.size() + 1);
  size_t allocations{};
  EXPECT_TRUE(deniedCurveMatch(witness, current, live, allocations)); EXPECT_EQ(allocations, 0u);
  EXPECT_TRUE(deniedCurveMatch(witness, current, prospective, allocations)); EXPECT_EQ(allocations, 0u);
  live.setSpeed(4); live.pause(); live.setLoop(true);
  EXPECT_TRUE(deniedCurveMatch(witness, current, live, allocations)); EXPECT_EQ(allocations, 0u);
  auto copy = witness; EXPECT_TRUE(copy.matches(current, prospective));
  EXPECT_FALSE(MaterialAnimationCurveWitness{}.matches(current, live)); EXPECT_EQ(MaterialAnimationCurveWitness{}.ownedBytes(), 0u);
}

TEST(GltfMaterialAnimation, CurveWitnessRejectsRawClipChannelSamplerAccessorViewAndBufferMetadataUnderDenial) {
  const auto source = smallCurveSource(); AnimationPlayer player(source);
  const auto witness = player.prepareMaterialEditCurveWitness(source);
  const std::array<std::function<void(tinygltf::Model&)>, 12> mutate{
      [](auto& m) { m.animations[0].extras_json_string += " stale"; },
      [](auto& m) { m.animations[0].samplers[0].extras.Get<Object>()["samplerIdentity"] = Value(17.); },
      [](auto& m) { m.animations[0].samplers[0].interpolation = "STEP"; },
      [](auto& m) { m.animations[0].channels[0].target_extensions_json_string += " stale"; },
      [](auto& m) { m.animations[0].channels[0].target_extensions["KHR_animation_pointer"] =
          Value(Object{{"pointer", Value(std::string("/materials/0/pbrMetallicRoughness/roughnessFactor"))}}); },
      [](auto& m) { m.accessors[0].minValues[0] = -0.; },
      [](auto& m) { m.accessors[1].extras_json_string = "new accessor raw metadata"; },
      [](auto& m) { m.accessors[1].byteOffset = 4; },
      [](auto& m) { m.bufferViews[1].byteStride = 8; },
      [](auto& m) { m.bufferViews[0].extensions["VENDOR_layout"] = Value(4); },
      [](auto& m) { m.buffers[1].extras = Value(Object{{"exactType", Value(1)}}); },
      [](auto& m) { m.extensionsRequired.push_back("KHR_accessor_float64"); }};
  for (const auto& change : mutate) {
    auto stale = source; change(stale); size_t allocations{};
    EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  }
}

TEST(GltfMaterialAnimation, CurveWitnessRejectsSelectedEncodedBitsAndSameClockReplacedDecodedLibrary) {
  auto source = smallCurveSource(); AnimationPlayer player(source);
  const auto witness = player.prepareMaterialEditCurveWitness(source);
  auto encoded = source; const auto oneUlp = std::nextafter(.2f, 1.f);
  std::memcpy(encoded.buffers[1].data.data(), &oneUlp, sizeof(oneUlp));
  size_t allocations{};
  EXPECT_FALSE(deniedCurveMatch(witness, encoded, player, allocations)); EXPECT_EQ(allocations, 0u);
  EXPECT_THROW(player.prepareMaterialEditCurveWitness(encoded), std::invalid_argument);
  AnimationPlayer replacement(encoded); controlsEqual(replacement, player);
  player = std::move(replacement);
  EXPECT_FALSE(deniedCurveMatch(witness, source, player, allocations)); EXPECT_EQ(allocations, 0u);
  EXPECT_THROW(player.prepareMaterialEditCurveWitness(source), std::invalid_argument);
}

TEST(GltfMaterialAnimation, CurveWitnessCapturesStridedElementsWithoutOwningGeometryOrPadding) {
  auto source = smallCurveSource();
  source.buffers.resize(1); source.buffers[0].data.assign(4 * 1024 * 1024, 71);
  source.bufferViews.resize(2);
  for (auto& view : source.bufferViews) { view.buffer = 0; view.byteOffset = 0; view.byteLength = source.buffers[0].data.size(); view.byteStride = 64; }
  source.accessors[0].bufferView = 0; source.accessors[0].byteOffset = 64;
  source.accessors[1].bufferView = 1; source.accessors[1].byteOffset = 68;
  const std::array<float, 4> values{0, 2, .2f, .8f};
  for (size_t i = 0; i < 2; ++i) {
    std::memcpy(source.buffers[0].data.data() + 64 + i * 64, &values[i], sizeof(float));
    std::memcpy(source.buffers[0].data.data() + 68 + i * 64, &values[2 + i], sizeof(float));
  }
  AnimationPlayer player(source); GltfMaterialAnimationEditLimits limits; limits.maxOwnedBytes = 128 * 1024;
  const auto* bytes = source.buffers[0].data.data();
  const auto witness = player.prepareMaterialEditCurveWitness(source, limits);
  EXPECT_LT(witness.ownedBytes(), 128 * 1024u); EXPECT_EQ(source.buffers[0].data.data(), bytes);
  source.buffers[0].data[80] ^= 3; source.buffers[0].data[3 * 1024 * 1024] ^= 1;
  size_t allocations{};
  EXPECT_TRUE(deniedCurveMatch(witness, source, player, allocations)); EXPECT_EQ(allocations, 0u);
  source.buffers[0].data[68] ^= 1;
  EXPECT_FALSE(deniedCurveMatch(witness, source, player, allocations)); EXPECT_EQ(allocations, 0u);
}

TEST(GltfMaterialAnimation, CurveWitnessCapturesSparseIndicesValuesAndZeroBaseWithoutInactiveNumericReads) {
  auto source = smallCurveSource();
  tinygltf::Buffer indices; indices.data = {0, 1}; source.buffers.push_back(indices);
  tinygltf::BufferView indexView; indexView.buffer = 2; indexView.byteLength = 2; source.bufferViews.push_back(indexView);
  auto& output = source.accessors[1]; output.bufferView = -1; output.byteOffset = 0; output.sparse = {};
  output.sparse.isSparse = true; output.sparse.count = 2; output.sparse.indices.bufferView = 2;
  output.sparse.indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  output.sparse.values.bufferView = 1;
  source.accessors[0].sparse.indices.extras = Value(Object{{"inactiveMetadata", Value(-0.)}});
  AnimationPlayer player(source); const auto witness = player.prepareMaterialEditCurveWitness(source);
  size_t allocations{}; EXPECT_TRUE(deniedCurveMatch(witness, source, player, allocations)); EXPECT_EQ(allocations, 0u);
  auto stale = source; stale.buffers[2].data[0] = 1;
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  stale = source; stale.buffers[1].data[0] ^= 1;
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  stale = source; stale.accessors[1].bufferView = 1;
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  stale = source; stale.accessors[0].sparse.indices.extras.Get<Object>()["inactiveMetadata"] = Value(0.);
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
}

TEST(GltfMaterialAnimation, CurveWitnessPreservesTargetSchemaAndDiagnosticBeforeAccessorSemantics) {
  auto source = smallCurveSource(); source.cameras.push_back(tinygltf::Camera{});
  source.cameras[0].type = "perspective"; source.cameras[0].perspective.yfov = .7; source.cameras[0].perspective.znear = .1;
  source.cameras[0].orthographic.xmag = 3; source.cameras[0].orthographic.ymag = 2;
  source.cameras[0].orthographic.znear = 0; source.cameras[0].orthographic.zfar = 20;
  pointer(source.animations[0], 0, 1, "/cameras/0/orthographic/xmag");
  // The unsupported camera-type target is diagnosed before accessor lookup.
  source.animations[0].samplers.back().input = source.animations[0].samplers.back().output = -987;
  AnimationPlayer player(source); ASSERT_EQ(player.library().diagnostics.size(), 1u);
  const auto witness = player.prepareMaterialEditCurveWitness(source); size_t allocations{};
  EXPECT_TRUE(deniedCurveMatch(witness, source, player, allocations)); EXPECT_EQ(allocations, 0u);
  auto stale = source; stale.cameras[0].type = "orthographic";
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  stale = source; stale.animations[0].channels.back().target_extensions["KHR_animation_pointer"] =
      Value(Object{{"pointer", Value(std::string("/cameras/0/perspective/yfov"))}});
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  // Supported malformed data throws rather than becoming a diagnostic.
  auto invalid = smallCurveSource(); const float nan = std::numeric_limits<float>::quiet_NaN();
  std::memcpy(invalid.buffers[1].data.data(), &nan, sizeof(nan));
  AnimationPlayer valid(smallCurveSource());
  EXPECT_THROW(valid.prepareMaterialEditCurveWitness(invalid), std::invalid_argument);
}

TEST(GltfMaterialAnimation, CurveWitnessRejectsNodeMatrixAndMorphShapeChangesWithoutCopyingMeshGeometry) {
  tinygltf::Model source; source.nodes.emplace_back(); source.nodes[0].mesh = 0;
  source.nodes[0].weights = {.2, .8}; source.meshes.emplace_back(); source.meshes[0].weights = {.2, .8};
  source.meshes[0].primitives.resize(2);
  for (auto& primitive : source.meshes[0].primitives) primitive.targets.resize(2);
  const int input = floats(source, {0, 2}, TINYGLTF_TYPE_SCALAR, 2);
  const int output = floats(source, {.2f, .8f, .4f, .6f}, TINYGLTF_TYPE_SCALAR, 4);
  tinygltf::Animation clip; channel(clip, input, output, 0, "weights"); source.animations.push_back(clip);
  AnimationPlayer player(source); const auto witness = player.prepareMaterialEditCurveWitness(source); size_t allocations{};
  auto stale = source; stale.meshes[0].primitives[1].targets.resize(1);
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  stale = source; stale.meshes[0].weights.push_back(.5);
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  source.meshes[0].primitives[0].attributes["POSITION"] = 999; // unrelated mesh geometry is not a curve dependency
  EXPECT_TRUE(deniedCurveMatch(witness, source, player, allocations)); EXPECT_EQ(allocations, 0u);
  auto trs = smallCurveSource(); trs.nodes.emplace_back();
  channel(trs.animations[0], 0, floats(trs, {1, 2, 3, 4, 5, 6}, TINYGLTF_TYPE_VEC3, 2), 0, "translation");
  AnimationPlayer trsPlayer(trs); const auto trsWitness = trsPlayer.prepareMaterialEditCurveWitness(trs);
  trs.nodes[0].matrix.assign(16, 0);
  EXPECT_FALSE(deniedCurveMatch(trsWitness, trs, trsPlayer, allocations)); EXPECT_EQ(allocations, 0u);
}

TEST(GltfMaterialAnimation, CurveWitnessRejectsSelectedOwnerTypesMetadataDepthAndDiagnosticLibraryChanges) {
  Fixture f; AnimationPlayer player(f.authored); const auto witness = player.prepareMaterialEditCurveWitness(f.authored);
  auto stale = f.authored; stale.lights[0].type = "spot"; size_t allocations{};
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  stale = f.authored; Value nested(1);
  for (int i = 0; i < 80; ++i) nested = Value(Value::Array{nested});
  stale.materials[0].extras = std::move(nested);
  EXPECT_FALSE(deniedCurveMatch(witness, stale, player, allocations)); EXPECT_EQ(allocations, 0u);
  auto alternate = f.authored; alternate.animations[0].channels.pop_back(); AnimationPlayer changed(alternate);
  EXPECT_FALSE(deniedCurveMatch(witness, f.authored, changed, allocations)); EXPECT_EQ(allocations, 0u);
}

TEST(GltfMaterialAnimation, CurveWitnessBoundsCoexistenceAndNeverCopiesUnrelatedImageOrGeometryBuffers) {
  auto source = smallCurveSource(); AnimationPlayer player(source);
  const auto baseline = player.prepareMaterialEditCurveWitness(source);
  source.images.emplace_back(); source.images[0].image.resize(2 * 1024 * 1024, 81);
  source.buffers.emplace_back(); source.buffers.back().data.resize(2 * 1024 * 1024, 82);
  const auto witness = player.prepareMaterialEditCurveWitness(source);
  EXPECT_EQ(witness.ownedBytes(), baseline.ownedBytes());
  for (size_t variant = 0; variant < 7; ++variant) {
    GltfMaterialAnimationEditLimits limits;
    if (variant == 0) limits.maxOwnedBytes = 0;
    if (variant == 1) limits.maxOwnedBytes = 1;
    if (variant == 2) limits.maxMetadataValues = 1;
    if (variant == 3) limits.maxMetadataDepth = 1;
    if (variant == 4) limits.maxChannels = 0;
    if (variant == 5) limits.maxClips = 0;
    if (variant == 6) limits.maxDecodedBytes = 1;
    EXPECT_THROW(player.prepareMaterialEditCurveWitness(source, limits), std::invalid_argument);
  }
}

TEST(GltfMaterialAnimation, EveryDeniedCurveWitnessAllocationPreservesSourcePlayerAndExistingProof) {
  auto source = smallCurveSource(); AnimationPlayer player(source); const auto before = source;
  const auto old = player.prepareMaterialEditCurveWitness(source); size_t allocations{};
  {
    material_publication_test::AllocationGuard count(false);
    auto witness = player.prepareMaterialEditCurveWitness(source); count.disable(); allocations = count.allocations();
  }
  ASSERT_GT(allocations, 0u); ASSERT_LT(allocations, 2000u);
  for (size_t allowance = 0; allowance < allocations; ++allowance) {
    bool denied{};
    {
      material_publication_test::AllocationGuard guard(false, allowance);
      try { (void)player.prepareMaterialEditCurveWitness(source); } catch (const std::bad_alloc&) { denied = true; }
    }
    EXPECT_TRUE(denied); EXPECT_TRUE(old.matches(source, player));
  }
  EXPECT_EQ(source, before); EXPECT_FALSE(player.selectedClip()); EXPECT_DOUBLE_EQ(player.time(), 0);
}
} // namespace
