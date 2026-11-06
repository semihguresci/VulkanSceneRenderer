#include <Container/geometry/GltfSessionMerge.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfAnimation.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
using namespace container::geometry::gltf;
using Value = tinygltf::Value;
using Object = Value::Object;

int floats(tinygltf::Model& model, std::initializer_list<float> data, int type, size_t count) {
  tinygltf::Buffer buffer; buffer.data.resize(data.size()*sizeof(float));
  std::memcpy(buffer.data.data(),data.begin(),buffer.data.size());
  tinygltf::BufferView view; view.buffer=int(model.buffers.size()); view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor; accessor.sparse={}; accessor.bufferView=int(model.bufferViews.size());
  accessor.count=count; accessor.type=type; accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  const auto id=int(model.accessors.size());
  model.buffers.push_back(std::move(buffer)); model.bufferViews.push_back(view); model.accessors.push_back(accessor);
  return id;
}
void texture(tinygltf::Model& model, int identity) {
  tinygltf::Image image; image.name="Image "+std::to_string(identity); image.as_is=true;
  image.uri="relative "+std::to_string(identity)+".png"; image.mimeType="image/png";
  image.image={137,80,78,71,static_cast<unsigned char>(identity)};
  image.extras=Value(Object{{"identity",Value(identity)}});
  tinygltf::Sampler sampler; sampler.name="Sampler "+std::to_string(identity);
  sampler.wrapS=TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
  tinygltf::Texture tex; tex.source=int(model.images.size()); tex.sampler=int(model.samplers.size());
  tex.extras=Value(Object{{"identity",Value(identity)}});
  model.images.push_back(image); model.samplers.push_back(sampler); model.textures.push_back(tex);
}
tinygltf::Model fixture(int identity=11) {
  tinygltf::Model m;
  const auto position=floats(m,{0,0,0,1,0,0,0,1,0},TINYGLTF_TYPE_VEC3,3);
  texture(m,identity);
  tinygltf::Material material; material.name="Surface "+std::to_string(identity);
  material.pbrMetallicRoughness.baseColorTexture.index=0;
  m.materials.push_back(material);
  tinygltf::Primitive primitive; primitive.mode=TINYGLTF_MODE_TRIANGLES;
  primitive.attributes["POSITION"]=position; primitive.material=0;
  tinygltf::Mesh mesh; mesh.primitives.push_back(primitive); m.meshes.push_back(mesh);
  tinygltf::Node node; node.mesh=0; node.translation={double(identity),0,0}; m.nodes.push_back(node);
  tinygltf::Scene scene; scene.nodes={0}; m.scenes.push_back(scene); m.defaultScene=0;
  return m;
}
void animation(tinygltf::Model& model) {
  const auto times=floats(model,{0,1},TINYGLTF_TYPE_SCALAR,2);
  model.accessors[size_t(times)].minValues={0}; model.accessors[size_t(times)].maxValues={1};
  const auto output=floats(model,{3,4,5,7,8,9},TINYGLTF_TYPE_VEC3,2);
  tinygltf::AnimationSampler sampler; sampler.input=times; sampler.output=output;
  tinygltf::AnimationChannel channel; channel.sampler=0; channel.target_node=0; channel.target_path="translation";
  tinygltf::Animation clip; clip.name="Motion"; clip.samplers.push_back(sampler); clip.channels.push_back(channel);
  model.animations.push_back(clip);
}
void skinAndSparseMorph(tinygltf::Model& model) {
  tinygltf::Node joint; joint.translation={0,2,0}; model.nodes.push_back(joint);
  model.scenes[0].nodes.push_back(1);
  tinygltf::Skin skin; skin.joints={1}; skin.skeleton=1;
  skin.inverseBindMatrices=floats(model,{1,0,0,0,0,1,0,0,0,0,1,0,3,4,5,1},TINYGLTF_TYPE_MAT4,1);
  model.skins.push_back(skin); model.nodes[0].skin=0;
  const auto joints=floats(model,{0,0,0,0,0,0,0,0,0,0,0,0},TINYGLTF_TYPE_VEC4,3);
  // JOINTS is an unsigned, unnormalized accessor; replace its byte payload.
  auto& jointAccessor=model.accessors[size_t(joints)]; jointAccessor.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  auto& jointView=model.bufferViews[size_t(jointAccessor.bufferView)]; jointView.byteLength=12;
  model.buffers[size_t(jointView.buffer)].data.assign(12,0);
  auto& primitive=model.meshes[0].primitives[0]; primitive.attributes["JOINTS_0"]=joints;
  primitive.attributes["WEIGHTS_0"]=floats(model,{1,0,0,0,1,0,0,0,1,0,0,0},TINYGLTF_TYPE_VEC4,3);
  tinygltf::Buffer sparse; sparse.data.resize(16); sparse.data[0]=2;
  const std::array<float,3> delta{8,9,10}; std::memcpy(sparse.data.data()+4,delta.data(),12);
  const auto buffer=int(model.buffers.size()); model.buffers.push_back(sparse);
  tinygltf::BufferView indexView; indexView.buffer=buffer; indexView.byteLength=1;
  const auto index=int(model.bufferViews.size()); model.bufferViews.push_back(indexView);
  tinygltf::BufferView valueView; valueView.buffer=buffer; valueView.byteOffset=4; valueView.byteLength=12;
  const auto values=int(model.bufferViews.size()); model.bufferViews.push_back(valueView);
  tinygltf::Accessor accessor; accessor.sparse={}; accessor.count=3;
  accessor.type=TINYGLTF_TYPE_VEC3; accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
  accessor.sparse.isSparse=true; accessor.sparse.count=1;
  accessor.sparse.indices.bufferView=index; accessor.sparse.indices.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  accessor.sparse.values.bufferView=values;
  primitive.targets={{{"POSITION",int(model.accessors.size())}}}; model.accessors.push_back(accessor);
}

TEST(GltfSessionMerge, ManualResourceSuffixUsesOneCanonicalTextureIndexSpace) {
  const auto live=fixture(); auto archive=live; texture(archive,71);
  const auto incoming=fixture(22), beforeArchive=archive;
  const auto merged=PlanGltfSessionSources(live,archive,incoming);
  EXPECT_EQ(merged.merge.imported.textures,(std::vector<int32_t>{2}));
  for (const auto* model:{&merged.merge.model,&merged.evaluatedSource}) {
    ASSERT_EQ(model->textures.size(),3u);
    EXPECT_EQ(model->materials[0].pbrMetallicRoughness.baseColorTexture.index,0);
    EXPECT_EQ(model->materials[1].pbrMetallicRoughness.baseColorTexture.index,2);
    EXPECT_EQ(model->textures[1],archive.textures[1]);
    EXPECT_EQ(model->images[1].image,archive.images[1].image);
    EXPECT_EQ(model->samplers[1],archive.samplers[1]);
  }
  EXPECT_EQ(archive,beforeArchive); EXPECT_EQ(live,fixture()); EXPECT_EQ(incoming,fixture(22));
}

TEST(GltfSessionMerge, AuthoredDefaultsAndOldEvaluatedPoseRemainIndependent) {
  auto archive=fixture(); animation(archive);
  tinygltf::Light light; light.type="point"; light.intensity=3; archive.lights.push_back(light);
  archive.extensionsUsed={"KHR_lights_punctual"};
  archive.extensions["KHR_lights_punctual"]=Value(Object{{"lights",Value(Value::Array{
      Value(Object{{"type",Value(std::string("point"))},{"intensity",Value(3.0)}})})}});
  tinygltf::Camera camera; camera.type="perspective"; camera.perspective.yfov=.5; camera.perspective.znear=.1;
  archive.cameras.push_back(camera);
  auto live=archive; AnimationPlayer player(live); player.selectClip(live,0); player.seek(live,.5);
  live.materials[0].pbrMetallicRoughness.roughnessFactor=.2; live.lights[0].intensity=17;
  live.extensions["KHR_lights_punctual"].Get<Object>().at("lights").Get<Value::Array>()[0].Get<Object>()["intensity"]=Value(17.0);
  live.cameras[0].perspective.yfov=.8;
  const auto beforeLive=live;
  const auto merged=PlanGltfSessionSources(live,archive,fixture(22));
  EXPECT_EQ(merged.merge.model.nodes[0].translation,archive.nodes[0].translation);
  EXPECT_EQ(merged.evaluatedSource.nodes[0].translation,(std::vector<double>{5,6,7}));
  EXPECT_DOUBLE_EQ(merged.merge.model.materials[0].pbrMetallicRoughness.roughnessFactor,1);
  EXPECT_DOUBLE_EQ(merged.evaluatedSource.materials[0].pbrMetallicRoughness.roughnessFactor,.2);
  EXPECT_DOUBLE_EQ(merged.merge.model.lights[0].intensity,3); EXPECT_DOUBLE_EQ(merged.evaluatedSource.lights[0].intensity,17);
  EXPECT_DOUBLE_EQ(merged.merge.model.cameras[0].perspective.yfov,.5);
  EXPECT_DOUBLE_EQ(merged.evaluatedSource.cameras[0].perspective.yfov,.8);
  EXPECT_EQ(merged.evaluatedSource.nodes[size_t(merged.merge.imported.nodes[0])].translation,(std::vector<double>{22,0,0}));
  auto extended=player.withExtendedSource(merged.evaluatedSource); auto projected=merged.evaluatedSource;
  extended.clearClip(projected); EXPECT_EQ(projected.nodes[0].translation,archive.nodes[0].translation);
  EXPECT_EQ(live,beforeLive);
}

TEST(GltfSessionMerge, ResourceSuffixOffsetsPreserveSparseMorphInverseBindsAndAnimationAccessors) {
  const auto live=fixture(); auto archive=live;
  const auto manualAccessor=floats(archive,{91},TINYGLTF_TYPE_SCALAR,1);
  auto incoming=fixture(22); skinAndSparseMorph(incoming); animation(incoming);
  const auto merged=PlanGltfSessionSources(live,archive,incoming);
  const auto& maps=merged.merge.imported;
  EXPECT_EQ(maps.accessors[0],int32_t(archive.accessors.size()));
  EXPECT_EQ(maps.bufferViews[0],int32_t(archive.bufferViews.size()));
  EXPECT_EQ(maps.buffers[0],int32_t(archive.buffers.size()));
  for (const auto* model:{&merged.merge.model,&merged.evaluatedSource}) {
    std::array<float,1> manual{}; CheckedAccessor(*model,manualAccessor,"manual suffix").readFloats(0,manual);
    EXPECT_FLOAT_EQ(manual[0],91);
    const auto& primitive=model->meshes[size_t(maps.meshes[0])].primitives[0];
    std::array<float,3> delta{}; CheckedAccessor(*model,primitive.targets[0].at("POSITION"),"sparse morph").readFloats(2,delta);
    EXPECT_EQ(delta,(std::array<float,3>{8,9,10}));
    const auto& skin=model->skins[size_t(maps.skins[0])];
    std::array<float,16> ibm{}; CheckedAccessor(*model,skin.inverseBindMatrices,"inverse bind").readFloats(0,ibm);
    EXPECT_EQ(ibm[12],3); EXPECT_EQ(ibm[13],4); EXPECT_EQ(ibm[14],5); EXPECT_EQ(ibm[15],1);
    const auto clips=ParseAnimations(*model); ASSERT_EQ(clips.clips.size(),1u);
    const auto pose=SampleAnimation(clips.clips[0],.5); ASSERT_EQ(pose.values.size(),2u);
    EXPECT_EQ(pose.values[0].value,(std::vector<double>{5,6,7}));
    EXPECT_EQ(pose.values[1].value,pose.values[0].value);
  }
}

TEST(GltfSessionMerge, RepeatedMergePromotesNewManualSuffixWithoutShiftingExistingIds) {
  auto archive=fixture(); const auto live=archive; texture(archive,71);
  auto first=PlanGltfSessionSources(live,archive,fixture(22));
  first.evaluatedSource.nodes[0].translation={101,102,103};
  texture(first.merge.model,81);
  const auto second=PlanGltfSessionSources(first.evaluatedSource,first.merge.model,fixture(33));
  EXPECT_EQ(second.merge.imported.textures,(std::vector<int32_t>{4}));
  EXPECT_EQ(second.merge.model.materials[0].pbrMetallicRoughness.baseColorTexture.index,0);
  EXPECT_EQ(second.merge.model.materials[1].pbrMetallicRoughness.baseColorTexture.index,2);
  EXPECT_EQ(second.merge.model.materials[2].pbrMetallicRoughness.baseColorTexture.index,4);
  EXPECT_EQ(second.evaluatedSource.nodes[0].translation,(std::vector<double>{101,102,103}));
  EXPECT_EQ(second.merge.model.nodes[0].translation,archive.nodes[0].translation);
  EXPECT_EQ(second.merge.model.textures[3],first.merge.model.textures[3]);
}

TEST(GltfSessionMerge, ImageEncodingAndUriNormalizationDoNotChangeBindingIdentity) {
  auto live=fixture(); auto archive=live;
  live.images[0].as_is=false; live.images[0].width=live.images[0].height=1;
  live.images[0].component=4; live.images[0].bits=8; live.images[0].image={1,2,3,4};
  archive.images[0].uri="F:/original/source%20image.png";
  live.buffers[0].uri="source.bin"; archive.buffers[0].uri="F:/original/source.bin";
  const auto merged=PlanGltfSessionSources(live,archive,fixture(22));
  EXPECT_EQ(merged.evaluatedSource.images[0].image,archive.images[0].image);
  EXPECT_TRUE(merged.evaluatedSource.images[0].as_is);
  EXPECT_TRUE(merged.evaluatedSource.images[0].uri.empty());
  EXPECT_EQ(merged.evaluatedSource.textures[0].source,0);
}

TEST(GltfSessionMerge, LiveSelectedDefaultControlsOmittedDestinationButArchiveDefaultSurvives) {
  auto archive=fixture(); archive.scenes.push_back(archive.scenes[0]);
  auto live=archive; live.defaultScene=1;
  const auto selected=PlanGltfSessionSources(live,archive,fixture(22));
  EXPECT_EQ(selected.merge.destinationScene,1); EXPECT_EQ(selected.merge.model.defaultScene,0);
  EXPECT_EQ(selected.evaluatedSource.defaultScene,1);
  GltfMergeOptions options; options.destinationScene=0;
  const auto explicitScene=PlanGltfSessionSources(live,archive,fixture(22),options);
  EXPECT_EQ(explicitScene.merge.destinationScene,0); EXPECT_EQ(explicitScene.evaluatedSource.defaultScene,1);
}

TEST(GltfSessionMerge, AbsentLiveDefaultUsesEffectiveSceneZeroAndStaysAbsent) {
  auto archive=fixture(); archive.scenes.push_back(archive.scenes[0]); archive.defaultScene=1;
  auto live=archive; live.defaultScene=-1;
  const auto selected=PlanGltfSessionSources(live,archive,fixture(22));
  EXPECT_EQ(selected.merge.destinationScene,0);
  EXPECT_EQ(selected.merge.model.defaultScene,1);
  EXPECT_EQ(selected.evaluatedSource.defaultScene,-1);
  EXPECT_NE(std::ranges::find(selected.merge.model.scenes[0].nodes,selected.merge.parentNode),
            selected.merge.model.scenes[0].nodes.end());
  EXPECT_EQ(selected.merge.model.scenes[1].nodes,archive.scenes[1].nodes);
  GltfMergeOptions options; options.destinationScene=1;
  const auto explicitScene=PlanGltfSessionSources(live,archive,fixture(22),options);
  EXPECT_EQ(explicitScene.merge.destinationScene,1);
  EXPECT_EQ(explicitScene.evaluatedSource.defaultScene,-1);
  EXPECT_EQ(live.defaultScene,-1); EXPECT_EQ(archive.defaultScene,1);
}

TEST(GltfSessionMerge, EvaluatedDefaultLeavesKeepTextureBindingsAndMetadataExact) {
  auto archive=fixture(); archive.extensionsUsed={"KHR_materials_anisotropy","KHR_texture_transform"};
  archive.materials[0].extensions["KHR_materials_anisotropy"]=Value(Object{});
  archive.materials[0].pbrMetallicRoughness.baseColorTexture.extensions["KHR_texture_transform"]=Value(Object{});
  auto live=archive;
  live.materials[0].extensions["KHR_materials_anisotropy"].Get<Object>()["anisotropyStrength"]=Value(.6);
  auto& transform=live.materials[0].pbrMetallicRoughness.baseColorTexture.extensions["KHR_texture_transform"].Get<Object>();
  transform["rotation"]=Value(.4);
  const auto result=PlanGltfSessionSources(live,archive,fixture(22));
  EXPECT_DOUBLE_EQ(result.evaluatedSource.materials[0].extensions.at("KHR_materials_anisotropy").Get("anisotropyStrength").GetNumberAsDouble(),.6);
  EXPECT_FALSE(result.merge.model.materials[0].extensions.at("KHR_materials_anisotropy").Has("anisotropyStrength"));
  transform["texCoord"]=Value(1);
  EXPECT_THROW((void)PlanGltfSessionSources(live,archive,fixture(22)),std::runtime_error);
}

TEST(GltfSessionMerge, ProceduralVersionNormalizationKeepsManualResourcesAndRejectsAuthoredInvalidVersion) {
  tinygltf::Model archive; archive.asset.version.clear(); texture(archive,71);
  const tinygltf::Model live=archive;
  const auto result=PlanGltfSessionSources(live,archive,fixture(22));
  EXPECT_EQ(result.merge.model.asset.version,"2.0"); EXPECT_EQ(result.merge.imported.textures[0],1);
  EXPECT_TRUE(archive.asset.version.empty());
  auto invalid=fixture(); invalid.asset.version.clear();
  EXPECT_THROW((void)PlanGltfSessionSources(invalid,invalid,fixture(22)),std::runtime_error);
}

TEST(GltfSessionMerge, InvalidPrefixesAndNonresidentArchivesLeaveEveryInputUnchanged) {
  for (int change=0;change<10;++change) {
    auto live=fixture(); animation(live); auto archive=live; texture(archive,71);
    const auto incoming=fixture(22);
    switch(change) {
      case 0: live.textures[0].source=1; break;
      case 1: live.accessors[0].byteOffset=4; break;
      case 2: live.nodes[0].mesh=-1; break;
      case 3: live.buffers[0].data[0]^=1; break;
      case 4: archive.images[0].image.clear(); break;
      case 5: archive.buffers[0].data.clear(); break;
      case 6: live.animations[0].samplers[0].interpolation="STEP"; break;
      case 7: live.materials[0].pbrMetallicRoughness.baseColorTexture.index=1; break;
      case 8: live.images[0].extras=Value(Object{{"identity",Value(999)}}); break;
      case 9: live.images[0].image[0]^=1; break;
    }
    const auto beforeLive=live,beforeArchive=archive,beforeIncoming=incoming;
    EXPECT_THROW((void)PlanGltfSessionSources(live,archive,incoming),std::runtime_error) << change;
    EXPECT_EQ(live,beforeLive); EXPECT_EQ(archive,beforeArchive); EXPECT_EQ(incoming,beforeIncoming);
  }
}

TEST(GltfSessionMerge, BothOutputPayloadAndMetadataBudgetsAreReservedBeforeCopies) {
  const auto live=fixture(),archive=live,incoming=fixture(22);
  const auto plan=PlanGltfSessionSources(live,archive,incoming);
  GltfMergeLimits limits; limits.maxResidentBytes=plan.peakResidentBytes;
  limits.maxMetadataBytes=plan.metadataBytes;
  EXPECT_NO_THROW((void)PlanGltfSessionSources(live,archive,incoming,{},limits));
  --limits.maxResidentBytes;
  EXPECT_THROW((void)PlanGltfSessionSources(live,archive,incoming,{},limits),std::runtime_error);
  limits.maxResidentBytes=plan.peakResidentBytes; --limits.maxMetadataBytes;
  EXPECT_THROW((void)PlanGltfSessionSources(live,archive,incoming,{},limits),std::runtime_error);
}

TEST(GltfSessionMerge, SparsePrimitiveExtensionAndPointerTargetsAreIdentityBearing) {
  auto archive=fixture(); skinAndSparseMorph(archive); animation(archive);
  archive.extensionsUsed.push_back("KHR_animation_pointer");
  auto& channel=archive.animations[0].channels[0]; channel.target_node=-1; channel.target_path="pointer";
  channel.target_extensions["KHR_animation_pointer"]=Value(Object{
      {"pointer",Value(std::string("/materials/0/pbrMetallicRoughness/roughnessFactor"))}});
  archive.animations[0].samplers[0].output=floats(archive,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2);
  const auto morph=archive.meshes[0].primitives[0].targets[0].at("POSITION");
  for (int change=0;change<6;++change) {
    auto live=archive;
    if (change==0) live.accessors[size_t(morph)].sparse.values.byteOffset=4;
    else if (change==1) live.meshes[0].primitives[0].extensions["KHR_materials_variants"]=Value(Object{});
    else if (change==2) live.animations[0].channels[0].target_extensions["KHR_animation_pointer"].Get<Object>()["pointer"]=
        Value(std::string("/materials/0/alphaCutoff"));
    else if (change==3) live.animations[0].channels[0].target_extras=Value(Object{{"identity",Value(9)}});
    else if (change==4) live.animations[0].channels[0].target_extras_json_string="{\"identity\":9}";
    else live.animations[0].channels[0].target_extensions_json_string="{}";
    EXPECT_THROW((void)PlanGltfSessionSources(live,archive,fixture(22)),std::runtime_error) << change;
  }
}
} // namespace
