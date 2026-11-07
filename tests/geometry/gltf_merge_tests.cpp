#include <Container/geometry/GltfMerge.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfDeformation.h>
#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/geometry/GltfSessionMerge.h>
#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/utility/GltfSceneSelection.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

namespace {
using namespace container::geometry::gltf;
using Value=tinygltf::Value;
using Object=Value::Object;
using Array=Value::Array;

void declare(tinygltf::Model& model,const std::string& name) {
  if(std::ranges::find(model.extensionsUsed,name)==model.extensionsUsed.end()) model.extensionsUsed.push_back(name);
}
int floats(tinygltf::Model& model,std::initializer_list<float> values,int type,size_t count) {
  tinygltf::Buffer buffer;buffer.data.resize(values.size()*sizeof(float));
  std::memcpy(buffer.data.data(),values.begin(),buffer.data.size());
  const int id=int(model.accessors.size());
  tinygltf::BufferView view;view.buffer=int(model.buffers.size());view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor;accessor.sparse={};accessor.bufferView=int(model.bufferViews.size());
  accessor.count=count;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;accessor.type=type;
  model.buffers.push_back(std::move(buffer));model.bufferViews.push_back(std::move(view));model.accessors.push_back(std::move(accessor));return id;
}
tinygltf::Model fixture(unsigned char texel=17) {
  tinygltf::Model model;
  const auto positions=floats(model,{0,0,0,1,0,0,0,1,0},TINYGLTF_TYPE_VEC3,3);
  tinygltf::Image image;image.name="same image";image.uri="same.png";image.mimeType="image/png";image.as_is=true;image.image={texel,2,3,4};
  image.extras=Value(Object{{"imageId",Value(int(texel))}});model.images.push_back(image);
  tinygltf::Sampler sampler;sampler.extras=Value(Object{{"samplerId",Value(int(texel))}});model.samplers.push_back(sampler);
  tinygltf::Texture texture;texture.source=0;texture.sampler=0;texture.extras=Value(Object{{"textureId",Value(int(texel))}});model.textures.push_back(texture);
  tinygltf::Material material;material.name="same material";material.pbrMetallicRoughness.baseColorTexture.index=0;
  material.extras=Value(Object{{"fooTexture",Value(Object{{"index",Value(982)}})}});model.materials.push_back(material);
  tinygltf::Primitive primitive;primitive.mode=TINYGLTF_MODE_TRIANGLES;primitive.attributes["POSITION"]=positions;primitive.material=0;
  tinygltf::Mesh mesh;mesh.primitives.push_back(primitive);model.meshes.push_back(mesh);
  tinygltf::Node node;node.name="same node";node.mesh=0;model.nodes.push_back(node);
  tinygltf::Scene scene;scene.name="same scene";scene.nodes={0};model.scenes.push_back(scene);model.defaultScene=0;
  return model;
}
void coreAnimation(tinygltf::Model& model,int node=0,std::string path="translation",int output=-1) {
  const auto input=floats(model,{0,1},TINYGLTF_TYPE_SCALAR,2);
  model.accessors[size_t(input)].minValues={0};model.accessors[size_t(input)].maxValues={1};
  if(output<0) output=floats(model,{4,0,0,6,0,0},TINYGLTF_TYPE_VEC3,2);
  tinygltf::AnimationSampler sampler;sampler.input=input;sampler.output=output;
  tinygltf::AnimationChannel channel;channel.sampler=0;channel.target_node=node;channel.target_path=path;
  channel.extras=Value(Object{{"index",Value(744)}});
  tinygltf::Animation animation;animation.name="same clip";animation.samplers.push_back(sampler);animation.channels.push_back(channel);model.animations.push_back(animation);
}
void pointerChannel(tinygltf::Model& model,tinygltf::Animation& animation,const std::string& pointer,int output) {
  declare(model,"KHR_animation_pointer");
  tinygltf::AnimationSampler sampler;sampler.input=animation.samplers[0].input;sampler.output=output;
  tinygltf::AnimationChannel channel;channel.sampler=int(animation.samplers.size());channel.target_path="pointer";
  channel.target_extensions["KHR_animation_pointer"]=Value(Object{{"pointer",Value(pointer)},
    {"extras",Value(Object{{"index",Value(199)}})}});
  animation.samplers.push_back(sampler);animation.channels.push_back(channel);
}
std::string pointer(const tinygltf::AnimationChannel& channel) {
  return channel.target_extensions.at("KHR_animation_pointer").Get("pointer").Get<std::string>();
}
void variants(tinygltf::Model& model,int metadata) {
  declare(model,"KHR_materials_variants");
  model.extensions["KHR_materials_variants"]=Value(Object{{"variants",Value(Array{Value(Object{{"name",Value(std::string("same"))},{"extras",Value(metadata)}})})}});
  model.meshes[0].primitives[0].extensions["KHR_materials_variants"]=Value(Object{{"mappings",Value(Array{
    Value(Object{{"material",Value(0)},{"variants",Value(Array{Value(0)})},{"extras",Value(Object{{"index",Value(metadata)}})}})})}});
}
void sun(tinygltf::Model& model) {
  declare(model,"KHR_lights_punctual");tinygltf::Light light;light.type="directional";light.color={1,1,1};
  light.extras=Value(Object{{"source",Value(811)}});model.lights.push_back(light);model.nodes[0].light=0;
  model.nodes[0].extensions["KHR_lights_punctual"]=Value(Object{{"light",Value(0)},{"extras",Value(991)}});
  model.extensions["KHR_lights_punctual"]=Value(Object{{"lights",Value(Array{Value(Object{{"type",Value(std::string("directional"))}})})}});
}
void skin(tinygltf::Model& model) {
  tinygltf::Node joint;joint.name="joint";joint.translation={0,2,0};model.nodes.push_back(joint);
  tinygltf::Node context;context.name="outside ancestor";context.children={1};context.translation={10,0,0};context.mesh=0;model.nodes.push_back(context);
  tinygltf::Skin skin;skin.joints={1};skin.skeleton=2;
  skin.inverseBindMatrices=floats(model,{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},TINYGLTF_TYPE_MAT4,1);
  model.skins.push_back(skin);model.nodes[0].skin=0;
  tinygltf::Buffer jointBuffer;jointBuffer.data.resize(12);
  tinygltf::BufferView jointView;jointView.buffer=int(model.buffers.size());jointView.byteLength=12;jointView.byteStride=4;
  tinygltf::Accessor jointAccessor;jointAccessor.sparse={};jointAccessor.bufferView=int(model.bufferViews.size());jointAccessor.count=3;
  jointAccessor.type=TINYGLTF_TYPE_VEC4;jointAccessor.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
  model.buffers.push_back(jointBuffer);model.bufferViews.push_back(jointView);
  model.meshes[0].primitives[0].attributes["JOINTS_0"]=int(model.accessors.size());model.accessors.push_back(jointAccessor);
  model.meshes[0].primitives[0].attributes["WEIGHTS_0"]=floats(model,{1,0,0,0,1,0,0,0,1,0,0,0},TINYGLTF_TYPE_VEC4,3);
  tinygltf::Scene joints;joints.name="joint scene";joints.nodes={2};model.scenes.push_back(joints);
}

std::vector<glm::mat4> translationMatrixWorlds(const tinygltf::Model& model) {
  std::vector<int> parents(model.nodes.size(),-1);
  for(size_t n=0;n<model.nodes.size();++n) for(const auto child:model.nodes[n].children) parents[size_t(child)]=int(n);
  std::vector<glm::mat4> worlds(model.nodes.size(),glm::mat4(1));
  for(size_t n=0;n<model.nodes.size();++n) for(int ancestor=int(n);ancestor>=0;ancestor=parents[size_t(ancestor)]) {
    const auto& node=model.nodes[size_t(ancestor)];glm::mat4 local(1);
    if(!node.matrix.empty()) for(int c=0;c<4;++c) for(int r=0;r<4;++r) local[c][r]=float(node.matrix[size_t(c*4+r)]);
    else if(!node.translation.empty()) local=glm::translate(local,glm::vec3(node.translation[0],node.translation[1],node.translation[2]));
    worlds[n]=local*worlds[n];
  }
  return worlds;
}

TEST(GltfMerge, RemapsIndependentResourcesAndPreservesBothInputs) {
  const auto base=fixture(11),incoming=fixture(22);const auto beforeBase=base,beforeIncoming=incoming;
  const auto plan=PlanGltfMerge(base,incoming);
  EXPECT_EQ(base,beforeBase);EXPECT_EQ(incoming,beforeIncoming);
  EXPECT_EQ(plan.imported.buffers,(std::vector<int32_t>{1}));
  EXPECT_EQ(plan.imported.materials,(std::vector<int32_t>{1}));
  EXPECT_EQ(plan.model.bufferViews[1].buffer,1);EXPECT_EQ(plan.model.accessors[1].bufferView,1);
  EXPECT_EQ(plan.model.meshes[1].primitives[0].attributes.at("POSITION"),1);
  EXPECT_EQ(plan.model.meshes[1].primitives[0].material,1);
  EXPECT_EQ(plan.model.materials[1].pbrMetallicRoughness.baseColorTexture.index,1);
  EXPECT_EQ(plan.model.textures[1].source,1);EXPECT_EQ(plan.model.textures[1].sampler,1);
  auto retainedImage=incoming.images[0];retainedImage.uri.clear();
  EXPECT_EQ(plan.model.images[1],retainedImage);EXPECT_EQ(plan.model.samplers[1],incoming.samplers[0]);
  EXPECT_EQ(plan.model.materials[1].extras,incoming.materials[0].extras);
  EXPECT_EQ(plan.model.buffers[1].data,incoming.buffers[0].data);
  EXPECT_EQ(plan.model.meshes.size(),2u);EXPECT_EQ(plan.model.nodes[size_t(plan.placement.nodes[0])].mesh,1);
  EXPECT_EQ(plan.model.nodes[0],base.nodes[0]);
}

TEST(GltfMerge, EqualPixelsDoNotCollapseAuthoredTextureImageOrSamplerIdentities) {
  auto base=fixture(),incoming=fixture();incoming.images[0].extras=Value(44);incoming.textures[0].extras=Value(55);incoming.samplers[0].extras=Value(66);
  const auto plan=PlanGltfMerge(base,incoming);
  ASSERT_EQ(plan.model.images.size(),2u);ASSERT_EQ(plan.model.textures.size(),2u);ASSERT_EQ(plan.model.samplers.size(),2u);
  EXPECT_EQ(plan.model.images[1].extras,Value(44));EXPECT_EQ(plan.model.textures[1].extras,Value(55));EXPECT_EQ(plan.model.samplers[1].extras,Value(66));
  EXPECT_EQ(plan.model.materials[1].pbrMetallicRoughness.baseColorTexture.index,1);
}

TEST(GltfMerge, PreservesImplicitMaterialInsteadOfBindingMaterialZero) {
  auto source=fixture();source.meshes[0].primitives[0].material=-1;
  const auto plan=PlanGltfMerge(fixture(),source);EXPECT_EQ(plan.model.meshes[1].primitives[0].material,-1);
}

TEST(GltfMerge, PreservesSharedCanonicalRootsEmptyScenesAndSceneMetadata) {
  auto incoming=fixture();incoming.scenes[0].extras=Value(Object{{"index",Value(81)}});
  auto shared=incoming.scenes[0];shared.name="shared";incoming.scenes.push_back(shared);
  tinygltf::Scene empty;empty.name="empty";empty.extras=Value(std::string("retain"));incoming.scenes.push_back(empty);
  const auto plan=PlanGltfMerge(fixture(),incoming);
  ASSERT_EQ(plan.model.scenes.size(),4u);
  EXPECT_EQ(plan.model.scenes[1].nodes,(std::vector<int>{1}));EXPECT_EQ(plan.model.scenes[2].nodes,plan.model.scenes[1].nodes);
  EXPECT_EQ(plan.model.scenes[1].extras,incoming.scenes[0].extras);EXPECT_TRUE(plan.model.scenes[3].nodes.empty());
  EXPECT_EQ(plan.model.scenes[3].extras,empty.extras);EXPECT_EQ(plan.model.scenes[0].nodes,(std::vector<int>{0,plan.parentNode}));
  EXPECT_EQ(plan.model.nodes[1].children,incoming.nodes[0].children);
  EXPECT_EQ(plan.model.nodes[size_t(plan.parentNode)].children,(std::vector<int>{plan.placement.nodes[0]}));
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneCatalog(plan.model));
}

TEST(GltfMerge, EmptyIncomingSceneCreatesOnlyNamedEmptyPlacement) {
  auto incoming=fixture();tinygltf::Scene empty;empty.name="empty";incoming.scenes.push_back(empty);
  GltfMergeOptions options;options.incomingScene=1;options.parentName="Empty import";
  const auto plan=PlanGltfMerge(fixture(),incoming,options);
  EXPECT_EQ(plan.summary.placementNodes,0u);EXPECT_TRUE(plan.model.nodes[size_t(plan.parentNode)].children.empty());
  EXPECT_EQ(plan.model.nodes[size_t(plan.parentNode)].name,"Empty import");EXPECT_EQ(plan.placement.nodes[0],-1);
}

TEST(GltfMerge, PreservesPlacementMatrixWithoutBakingGeometry) {
  GltfMergeOptions options;options.parentName="Placed";options.placementMatrix=std::array<double,16>{-2,0,0,0,0,3,0,0,0,0,4,0,5,6,7,1};
  auto incoming=fixture();incoming.nodes[0].translation={1,2,3};const auto plan=PlanGltfMerge(fixture(),incoming,options);
  EXPECT_EQ(plan.model.nodes[size_t(plan.parentNode)].matrix,(std::vector<double>(options.placementMatrix->begin(),options.placementMatrix->end())));
  EXPECT_EQ(plan.model.nodes[size_t(plan.placement.nodes[0])].translation,incoming.nodes[0].translation);
  EXPECT_EQ(plan.model.buffers[1].data,incoming.buffers[0].data);
}

TEST(GltfMerge, ClonesJointAncestorClosureWithoutLeakingVisualAttachments) {
  auto incoming=fixture();skin(incoming);coreAnimation(incoming,1);
  const auto before=incoming;const auto plan=PlanGltfMerge(fixture(),incoming);
  ASSERT_EQ(plan.model.skins.size(),2u);EXPECT_EQ(incoming,before);
  EXPECT_EQ(plan.model.skins[0].joints,(std::vector<int>{plan.imported.nodes[1]}));
  const auto& placedSkin=plan.model.skins[size_t(plan.placement.skins[0])];
  EXPECT_EQ(placedSkin.joints,(std::vector<int>{plan.placement.nodes[1]}));EXPECT_EQ(placedSkin.skeleton,plan.placement.nodes[2]);
  EXPECT_EQ(placedSkin.inverseBindMatrices,plan.imported.accessors[1]);
  const auto& ancestor=plan.model.nodes[size_t(plan.placement.nodes[2])];
  EXPECT_EQ(ancestor.mesh,-1);EXPECT_EQ(ancestor.translation,incoming.nodes[2].translation);EXPECT_EQ(ancestor.children,(std::vector<int>{plan.placement.nodes[1]}));
  EXPECT_TRUE(plan.placement.contextOnly[1]);EXPECT_TRUE(plan.placement.contextOnly[2]);EXPECT_FALSE(plan.placement.contextOnly[0]);
  EXPECT_EQ(plan.model.nodes[size_t(plan.placement.nodes[0])].skin,plan.placement.skins[0]);
  EXPECT_EQ(plan.model.meshes.size(),2u);ASSERT_EQ(plan.model.animations[0].channels.size(),2u);
  EXPECT_EQ(plan.model.animations[0].channels[1].target_node,plan.placement.nodes[1]);
  const auto jointSource=incoming.meshes[0].primitives[0].attributes.at("JOINTS_0");
  const auto& jointAccessor=plan.model.accessors[size_t(plan.imported.accessors[size_t(jointSource)])];
  const auto& jointView=plan.model.bufferViews[size_t(jointAccessor.bufferView)];
  EXPECT_EQ(plan.model.buffers[size_t(jointView.buffer)].data,(std::vector<unsigned char>(12,0)));
}

TEST(GltfMerge, DuplicatesNodeAndComponentPointerChannelsButSharesResourceTracks) {
  auto incoming=fixture();coreAnimation(incoming);sun(incoming);
  tinygltf::Camera camera;camera.type="perspective";camera.perspective.yfov=.8;camera.perspective.znear=.1;incoming.cameras.push_back(camera);incoming.nodes[0].camera=0;
  const auto scalar=floats(incoming,{.2f,.8f},TINYGLTF_TYPE_SCALAR,2);
  auto& animation=incoming.animations[0];
  pointerChannel(incoming,animation,"/nodes/0/scale/1",scalar);
  pointerChannel(incoming,animation,"/materials/0/pbrMetallicRoughness/roughnessFactor",scalar);
  pointerChannel(incoming,animation,"/cameras/0/perspective/yfov",scalar);
  pointerChannel(incoming,animation,"/extensions/KHR_lights_punctual/lights/0/intensity",scalar);
  auto base=fixture();sun(base);tinygltf::Camera baseCamera=camera;base.cameras.push_back(baseCamera);
  const auto plan=PlanGltfMerge(base,incoming);const auto& channels=plan.model.animations[0].channels;
  ASSERT_EQ(channels.size(),7u);EXPECT_EQ(channels[0].target_node,1);
  EXPECT_EQ(pointer(channels[1]),"/nodes/1/scale/1");
  EXPECT_EQ(pointer(channels[2]),"/materials/1/pbrMetallicRoughness/roughnessFactor");
  EXPECT_EQ(pointer(channels[3]),"/cameras/1/perspective/yfov");
  EXPECT_EQ(pointer(channels[4]),"/extensions/KHR_lights_punctual/lights/1/intensity");
  EXPECT_EQ(channels[5].target_node,plan.placement.nodes[0]);EXPECT_EQ(pointer(channels[6]),"/nodes/"+std::to_string(plan.placement.nodes[0])+"/scale/1");
  EXPECT_EQ(channels[6].sampler,1);EXPECT_EQ(channels[6].target_extensions.at("KHR_animation_pointer").Get("extras"),Value(Object{{"index",Value(199)}}));
  const auto library=ParseAnimations(plan.model);const auto pose=SampleAnimation(library.clips[0],.5);
  EXPECT_EQ(pose.values[0].value[0],5);EXPECT_EQ(pose.values[5].value[0],5);
}

TEST(GltfMerge, ExternalJointClosureMovesSkinnedWorldPointOnlyByPlacement) {
  auto incoming=fixture();skin(incoming);incoming.nodes[0].translation={3,0,0};
  GltfMergeOptions options;options.placementMatrix=std::array<double,16>{-2,0,0,0,0,3,0,0,0,0,4,0,5,6,7,1};
  const auto plan=PlanGltfMerge(fixture(),incoming,options);
  const auto before=translationMatrixWorlds(incoming),after=translationMatrixWorlds(plan.model);
  DeformationSkin source;source.joints={1};source.inverseBindMatrices={glm::mat4(1)};source.skeleton=2;
  const auto beforePalette=ComputeSkinPalette(source,before[0],before);
  const auto initial=before[0]*beforePalette[0]*glm::vec4(1,0,0,1);
  EXPECT_FLOAT_EQ(initial.x,11);EXPECT_FLOAT_EQ(initial.y,2);
  DeformationSkin placed=source;placed.joints={uint32_t(plan.placement.nodes[1])};placed.skeleton=plan.placement.nodes[2];
  const auto owner=size_t(plan.placement.nodes[0]);const auto afterPalette=ComputeSkinPalette(placed,after[owner],after);
  const auto final=after[owner]*afterPalette[0]*glm::vec4(1,0,0,1);
  // Independent arithmetic: (-2*11+5, 3*2+6, 4*0+7).
  EXPECT_NEAR(final.x,-17,1e-5);EXPECT_NEAR(final.y,12,1e-5);EXPECT_NEAR(final.z,7,1e-5);
}

TEST(GltfMerge, RemapsVariantIdsAndMappingsWithoutDeduplicatingNamesOrMetadata) {
  auto base=fixture(),incoming=fixture();variants(base,41);variants(incoming,42);
  const auto plan=PlanGltfMerge(base,incoming);const auto library=ParseMaterialVariants(plan.model);
  ASSERT_EQ(library.names().size(),2u);EXPECT_EQ(library.names()[0],library.names()[1]);
  const auto& mapping=plan.model.meshes[1].primitives[0].extensions.at("KHR_materials_variants").Get("mappings").Get<Array>()[0];
  EXPECT_EQ(mapping.Get("material").Get<int>(),1);EXPECT_EQ(mapping.Get("variants").Get<Array>()[0].Get<int>(),1);
  EXPECT_EQ(mapping.Get("extras"),Value(Object{{"index",Value(42)}}));
  EXPECT_EQ(plan.model.extensions.at("KHR_materials_variants").Get("variants").Get<Array>()[1].Get("extras"),Value(42));
}

TEST(GltfMerge, RemapsInstancingAndAllAcceptedAreaDialectReferences) {
  for(const auto* name:{"KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"}) {
    auto base=fixture(),incoming=fixture();declare(base,name);declare(incoming,name);
    const Value definition(Object{{"type",Value(std::string("rect"))},{"width",Value(2.0)},{"extras",Value(Object{{"index",Value(819)}})}});
    base.extensions[name]=Value(Object{{"lights",Value(Array{definition})}});incoming.extensions[name]=base.extensions[name];
    incoming.nodes[0].extensions[name]=Value(Object{{"light",Value(0)},{"extras",Value(73)}});
    declare(incoming,"EXT_mesh_gpu_instancing");incoming.nodes[0].extensions["EXT_mesh_gpu_instancing"]=Value(Object{{"attributes",Value(Object{{"TRANSLATION",Value(0)}})}});
    const auto plan=PlanGltfMerge(base,incoming);const auto& node=plan.model.nodes[1];
    EXPECT_EQ(node.extensions.at(name).Get("light").Get<int>(),1);EXPECT_EQ(node.extensions.at(name).Get("extras"),Value(73));
    EXPECT_EQ(node.extensions.at("EXT_mesh_gpu_instancing").Get("attributes").Get("TRANSLATION").Get<int>(),1);
    EXPECT_EQ(plan.model.extensions.at(name).Get("lights").Get<Array>()[1],definition);
  }
}

TEST(GltfMerge, RemapsKnownExtensionTexturesButNeverExtrasLookalikes) {
  auto incoming=fixture();declare(incoming,"KHR_materials_anisotropy");declare(incoming,"KHR_texture_transform");
  auto& material=incoming.materials[0];material.extensions["KHR_materials_anisotropy"]=Value(Object{
    {"anisotropyStrength",Value(0.0)},{"anisotropyTexture",Value(Object{{"index",Value(0)},{"texCoord",Value(7)},
      {"extensions",Value(Object{{"KHR_texture_transform",Value(Object{{"texCoord",Value(1)}})}})}})},
    {"extras",Value(Object{{"fooTexture",Value(Object{{"index",Value(901)}})}})}});
  material.pbrMetallicRoughness.baseColorTexture.texCoord=7;
  material.pbrMetallicRoughness.baseColorTexture.extensions["KHR_texture_transform"]=Value(Object{{"texCoord",Value(1)}});
  const auto plan=PlanGltfMerge(fixture(),incoming);
  const auto& ext=plan.model.materials[1].extensions.at("KHR_materials_anisotropy");
  EXPECT_EQ(ext.Get("anisotropyStrength"),Value(0.0));EXPECT_EQ(ext.Get("anisotropyTexture").Get("index"),Value(1));
  EXPECT_EQ(ext.Get("anisotropyTexture").Get("texCoord"),Value(7));EXPECT_EQ(ext.Get("extras"),material.extensions.at("KHR_materials_anisotropy").Get("extras"));
  EXPECT_EQ(plan.model.materials[1].pbrMetallicRoughness.baseColorTexture.texCoord,7);
  incoming.materials[0].pbrMetallicRoughness.baseColorTexture.extensions["KHR_texture_transform"].Get<Object>()["texCoord"]=Value(2);
  EXPECT_THROW((void)PlanGltfMerge(fixture(),incoming),std::runtime_error);
}

TEST(GltfMerge, RemapsTypedLegacyTexturesAndRejectsUnknownLegacyReferences) {
  auto incoming=fixture();
  for(const auto* key:{"roughnessTexture","metalnessTexture","specularTexture","heightTexture","displacementTexture","opacityTexture","refractionTexture","transmissionTexture"}) {
    tinygltf::Parameter parameter;parameter.json_double_value={{"index",0},{"texCoord",1}};incoming.materials[0].additionalValues[key]=parameter;
  }
  const auto plan=PlanGltfMerge(fixture(),incoming);
  for(const auto& [key,value]:plan.model.materials[1].additionalValues) {EXPECT_EQ(value.TextureIndex(),1)<<key;EXPECT_EQ(value.TextureTexCoord(),1);}
  incoming.materials[0].values["mysteryTexture"].json_double_value["index"]=0;
  EXPECT_THROW((void)PlanGltfMerge(fixture(),incoming),std::runtime_error);
}

TEST(GltfMerge, RemapsAlternateImageSourcesWithoutChangingMetadata) {
  for(const auto* name:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds"}) {
    auto incoming=fixture();declare(incoming,name);incoming.textures[0].extensions[name]=Value(Object{{"source",Value(0)},{"extras",Value(Object{{"source",Value(91)}})}});
    const auto plan=PlanGltfMerge(fixture(),incoming);
    EXPECT_EQ(plan.model.textures[1].extensions.at(name).Get("source"),Value(1));
    EXPECT_EQ(plan.model.textures[1].extensions.at(name).Get("extras"),Value(Object{{"source",Value(91)}}));
  }
}

TEST(GltfMerge, RemapsEverySupportedExtensionTextureFieldByExactSchemaLocation) {
  const std::array fields{
    std::pair{"KHR_materials_specular","specularTexture"},std::pair{"KHR_materials_specular","specularColorTexture"},
    std::pair{"KHR_materials_transmission","transmissionTexture"},std::pair{"KHR_materials_volume","thicknessTexture"},
    std::pair{"KHR_materials_clearcoat","clearcoatTexture"},std::pair{"KHR_materials_clearcoat","clearcoatRoughnessTexture"},
    std::pair{"KHR_materials_clearcoat","clearcoatNormalTexture"},std::pair{"KHR_materials_sheen","sheenColorTexture"},
    std::pair{"KHR_materials_sheen","sheenRoughnessTexture"},std::pair{"KHR_materials_iridescence","iridescenceTexture"},
    std::pair{"KHR_materials_iridescence","iridescenceThicknessTexture"},std::pair{"KHR_materials_anisotropy","anisotropyTexture"},
    std::pair{"KHR_materials_diffuse_transmission","diffuseTransmissionTexture"},std::pair{"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture"},
    std::pair{"KHR_materials_pbrSpecularGlossiness","diffuseTexture"},std::pair{"KHR_materials_pbrSpecularGlossiness","specularGlossinessTexture"}};
  for(const auto& [extension,key]:fields) {
    SCOPED_TRACE(std::string(extension)+"/"+key);auto incoming=fixture();declare(incoming,extension);
    incoming.materials[0].extensions[extension]=Value(Object{{key,Value(Object{{"index",Value(0)},{"extras",Value(Object{{"index",Value(616)}})}})}});
    const auto first=PlanGltfMerge(fixture(),incoming);const auto second=PlanGltfMerge(first.model,incoming);
    const auto& value=second.model.materials[2].extensions.at(extension).Get(key);
    EXPECT_EQ(value.Get("index"),Value(2));EXPECT_EQ(value.Get("extras"),Value(Object{{"index",Value(616)}}));
  }
}

TEST(GltfMerge, RetargetsNestedTextureTransformPointersAcrossRepeatedMerges) {
  auto incoming=fixture();declare(incoming,"KHR_materials_anisotropy");declare(incoming,"KHR_texture_transform");
  incoming.materials[0].extensions["KHR_materials_anisotropy"]=Value(Object{{"anisotropyTexture",Value(Object{{"index",Value(0)},
    {"extensions",Value(Object{{"KHR_texture_transform",Value(Object{{"offset",Value(Array{Value(0.0),Value(0.0)})}})}})}})}});
  coreAnimation(incoming);const auto scalar=floats(incoming,{0,.5f},TINYGLTF_TYPE_SCALAR,2);
  const std::string suffix="/extensions/KHR_materials_anisotropy/anisotropyTexture/extensions/KHR_texture_transform/offset/0";
  pointerChannel(incoming,incoming.animations[0],"/materials/0"+suffix,scalar);
  const auto first=PlanGltfMerge(fixture(),incoming);const auto second=PlanGltfMerge(first.model,incoming);
  ASSERT_EQ(second.model.animations.size(),2u);ASSERT_EQ(second.model.animations[1].channels.size(),3u);
  EXPECT_EQ(pointer(second.model.animations[0].channels[1]),"/materials/1"+suffix);
  EXPECT_EQ(pointer(second.model.animations[1].channels[1]),"/materials/2"+suffix);
  EXPECT_TRUE(ParseAnimations(second.model).diagnostics.empty());
}

TEST(GltfMerge, RetainsAreaMetadataInsideTypedPunctualDefinitionsAcrossRepeatedMerges) {
  auto incoming=fixture();sun(incoming);declare(incoming,"KHR_lights_area");
  const Value area(Object{{"type",Value(std::string("rect"))},{"width",Value(3.0)},{"extras",Value(Object{{"index",Value(72)}})}});
  incoming.lights[0].extensions["KHR_lights_area"]=area;
  incoming.extensions["KHR_lights_punctual"].Get<Object>().at("lights").Get<Array>()[0].Get<Object>()["extensions"]=Value(Object{{"KHR_lights_area",area}});
  const auto first=PlanGltfMerge(fixture(),incoming);const auto second=PlanGltfMerge(first.model,incoming);
  EXPECT_EQ(second.model.lights[1].extensions.at("KHR_lights_area"),area);
  EXPECT_EQ(second.model.extensions.at("KHR_lights_punctual").Get("lights").Get<Array>()[1].Get("extensions").Get("KHR_lights_area"),area);
}

TEST(GltfMerge, PreservesSparseZeroBaseAndResidentBytesWithoutDensifying) {
  auto incoming=fixture();tinygltf::Buffer buffer;buffer.data.resize(16);buffer.data[0]=2;const std::array<float,3> xyz{8,9,10};std::memcpy(buffer.data.data()+4,xyz.data(),12);
  incoming.buffers.push_back(buffer);tinygltf::BufferView indices;indices.buffer=1;indices.byteLength=1;incoming.bufferViews.push_back(indices);
  tinygltf::BufferView values;values.buffer=1;values.byteOffset=4;values.byteLength=12;incoming.bufferViews.push_back(values);
  tinygltf::Accessor a;a.sparse={};a.count=3;a.type=TINYGLTF_TYPE_VEC3;a.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;a.sparse.isSparse=true;a.sparse.count=1;
  a.sparse.indices.bufferView=1;a.sparse.indices.componentType=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;a.sparse.values.bufferView=2;
  incoming.accessors.push_back(a);incoming.meshes[0].primitives[0].attributes["POSITION"]=1;
  const auto plan=PlanGltfMerge(fixture(),incoming);const auto& mapped=plan.model.accessors[2];
  EXPECT_EQ(mapped.bufferView,-1);EXPECT_EQ(mapped.sparse.indices.bufferView,2);EXPECT_EQ(mapped.sparse.values.bufferView,3);
  EXPECT_EQ(plan.model.buffers[2].data,buffer.data);std::array<double,3> point{};CheckedAccessor accessor(plan.model,2,"test");accessor.readDoubles(2,point);
  EXPECT_EQ(point,(std::array<double,3>{8,9,10}));
}

TEST(GltfMerge, ResidentDecodedImagesCannotResolveAgainstTheWrongOrigin) {
  auto base=fixture(21),incoming=fixture(22);
  for(auto* image:{&base.images[0],&incoming.images[0]}) {image->as_is=false;image->width=1;image->height=1;image->component=4;image->bits=8;image->pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;}
  const auto beforeBase=base,beforeIncoming=incoming;const auto plan=PlanGltfMerge(base,incoming);
  EXPECT_TRUE(plan.model.images[0].uri.empty());EXPECT_TRUE(plan.model.images[1].uri.empty());
  EXPECT_EQ(plan.model.images[0].image,base.images[0].image);EXPECT_EQ(plan.model.images[1].image,incoming.images[0].image);
  EXPECT_EQ(base,beforeBase);EXPECT_EQ(incoming,beforeIncoming);
}

TEST(GltfMerge, PreservesMorphTargetsDefaultsAndPlacedWeightTracks) {
  auto incoming=fixture();const auto delta=floats(incoming,{0,0,1,0,0,2,0,0,3},TINYGLTF_TYPE_VEC3,3);
  incoming.meshes[0].primitives[0].targets={{{"POSITION",delta}}};incoming.meshes[0].weights={.1};incoming.nodes[0].weights={.2};
  const auto weights=floats(incoming,{.25f,.75f},TINYGLTF_TYPE_SCALAR,2);coreAnimation(incoming,0,"weights",weights);
  const auto plan=PlanGltfMerge(fixture(),incoming);
  EXPECT_EQ(plan.model.meshes[1].weights,incoming.meshes[0].weights);
  EXPECT_EQ(plan.model.meshes[1].primitives[0].targets[0].at("POSITION"),plan.imported.accessors[size_t(delta)]);
  EXPECT_EQ(plan.model.nodes[size_t(plan.placement.nodes[0])].weights,incoming.nodes[0].weights);
  const auto library=ParseAnimations(plan.model);ASSERT_EQ(library.clips[0].tracks.size(),2u);
  const auto pose=SampleAnimation(library.clips[0],.5);EXPECT_EQ(pose.values[0].value[0],.5);EXPECT_EQ(pose.values[1].value[0],.5);
  EXPECT_EQ(plan.model.buffers[size_t(plan.imported.buffers[1])].data,incoming.buffers[1].data);
}

TEST(GltfMerge, DoesNotCloneWeightTracksOntoTransformOnlyJointContext) {
  auto incoming=fixture();skin(incoming);const auto delta=floats(incoming,{0,0,0,0,0,0,0,0,0},TINYGLTF_TYPE_VEC3,3);
  incoming.meshes[0].primitives[0].targets={{{"POSITION",delta}}};incoming.meshes[0].weights={0};
  const auto weights=floats(incoming,{0,1},TINYGLTF_TYPE_SCALAR,2);coreAnimation(incoming,2,"weights",weights);
  const auto plan=PlanGltfMerge(fixture(),incoming);
  ASSERT_EQ(plan.model.animations[0].channels.size(),1u);EXPECT_TRUE(plan.placement.contextOnly[2]);
  EXPECT_EQ(plan.model.nodes[size_t(plan.placement.nodes[2])].mesh,-1);
  EXPECT_TRUE(ParseAnimations(plan.model).diagnostics.empty());
}

TEST(GltfMerge, KeepsFloat64AnimationRepresentationAndSamplePrecision) {
  auto incoming=fixture();declare(incoming,"KHR_accessor_float64");incoming.extensionsRequired.push_back("KHR_accessor_float64");
  const std::array<double,6> translations{1.000000000000001,0,0,1.000000000000003,0,0};
  tinygltf::Buffer buffer;buffer.data.resize(sizeof(translations));std::memcpy(buffer.data.data(),translations.data(),sizeof(translations));
  tinygltf::BufferView view;view.buffer=int(incoming.buffers.size());view.byteLength=buffer.data.size();
  tinygltf::Accessor accessor;accessor.sparse={};accessor.bufferView=int(incoming.bufferViews.size());accessor.componentType=TINYGLTF_COMPONENT_TYPE_DOUBLE;accessor.type=TINYGLTF_TYPE_VEC3;accessor.count=2;
  const int output=int(incoming.accessors.size());incoming.buffers.push_back(buffer);incoming.bufferViews.push_back(view);incoming.accessors.push_back(accessor);coreAnimation(incoming,0,"translation",output);
  const auto plan=PlanGltfMerge(fixture(),incoming);
  EXPECT_EQ(plan.model.accessors[size_t(plan.imported.accessors[size_t(output)])].componentType,TINYGLTF_COMPONENT_TYPE_DOUBLE);
  EXPECT_EQ(plan.model.buffers[size_t(plan.imported.buffers[1])].data,buffer.data);
  const auto library=ParseAnimations(plan.model);const auto pose=SampleAnimation(library.clips[0],.5);
  EXPECT_DOUBLE_EQ(pose.values[0].value[0],(translations[0]+translations[3])*.5);
  EXPECT_DOUBLE_EQ(pose.values[1].value[0],pose.values[0].value[0]);EXPECT_NE(pose.values[0].value[0],double(float(pose.values[0].value[0])));
}

TEST(GltfMerge, ChargesReusedAccessorExpansionForEveryAnimationChannelAndClone) {
  auto incoming=fixture();coreAnimation(incoming);GltfMergeLimits limits;
  // Each translation track expands 2 scalar times plus 2 VEC3 values = 64B.
  limits.maxAnimationDecodedBytes=100;
  EXPECT_THROW((void)PlanGltfMerge(fixture(),incoming,{},limits),std::runtime_error);
  limits.maxAnimationDecodedBytes=128;const auto plan=PlanGltfMerge(fixture(),incoming,{},limits);
  EXPECT_EQ(plan.summary.animationDecodedBytes,128u);
}

TEST(GltfMerge, RepeatedMergesRetainPreviousIdsAndAllSceneEntries) {
  const auto source=fixture();const auto first=PlanGltfMerge(source,source);const auto second=PlanGltfMerge(first.model,source);
  EXPECT_EQ(second.imported.nodes[0],int(first.model.nodes.size()));EXPECT_EQ(second.model.scenes.size(),3u);
  for(size_t i=0;i<first.model.nodes.size();++i) EXPECT_EQ(second.model.nodes[i],first.model.nodes[i]);
  for(size_t i=0;i<first.model.materials.size();++i) EXPECT_EQ(second.model.materials[i],first.model.materials[i]);
  EXPECT_NO_THROW((void)container::scene::BuildGltfSceneCatalog(second.model));
}

TEST(GltfMerge, RejectsPlacementThatBecomesSingularAfterBinary32Conversion) {
  GltfMergeOptions options;
  options.placementMatrix=std::array<double,16>{1,1,0,0,1,1+1e-8,0,0,0,0,1,0,0,0,0,1};
  // Authored determinant is about1e-8; the two runtime basis columns coincide.
  EXPECT_THROW((void)PlanGltfMerge(fixture(),fixture(),options),std::runtime_error);
}

TEST(GltfMerge, RejectsFinitePlacementWithBinary32DeterminantOverflow) {
  GltfMergeOptions options;
  options.placementMatrix=std::array<double,16>{1e20,0,0,0,0,1e20,0,0,0,0,1e20,0,0,0,0,1};
  // Every uploaded coefficient is finite, but the float determinant overflows.
  EXPECT_THROW((void)PlanGltfMerge(fixture(),fixture(),options),std::runtime_error);
}

TEST(GltfMerge, PreservesFiniteMirroredAffinePlacementThroughSharedValidation) {
  GltfMergeOptions options;
  options.placementMatrix=std::array<double,16>{-2,.25,0,0,0,3,0,0,0,0,4,0,5,6,7,1};
  const auto plan=PlanGltfMerge(fixture(),fixture(),options);
  EXPECT_EQ(plan.model.nodes[size_t(plan.parentNode)].matrix,
      std::vector<double>(options.placementMatrix->begin(),options.placementMatrix->end()));
}

TEST(GltfMerge, SynthesizesCatalogForSourceWithoutAuthoredScenes) {
  auto base=fixture(),incoming=fixture();base.scenes.clear();base.defaultScene=-1;incoming.scenes.clear();incoming.defaultScene=-1;
  const auto plan=PlanGltfMerge(base,incoming);EXPECT_EQ(plan.model.scenes.size(),2u);EXPECT_EQ(plan.imported.scenes,(std::vector<int32_t>{1}));EXPECT_EQ(plan.model.defaultScene,0);
}

TEST(GltfMerge, CountsImplicitSceneObjectsBeforePublishingAnEmptyMerge) {
  const tinygltf::Model empty;const auto plan=PlanGltfMerge(empty,empty);
  // Two source assets, two generated scenes, one parent and asset provenance.
  EXPECT_EQ(plan.summary.objects,6u);GltfMergeLimits limits;limits.maxTotalObjects=5;
  EXPECT_THROW((void)PlanGltfMerge(empty,empty,{},limits),std::runtime_error);
}

TEST(GltfMerge, RemapsIndependentXmpPacketsAndKeepsInvalidReferencesIsolated) {
  using Json = nlohmann::json;
  auto base = fixture(), incoming = fixture();
  coreAnimation(incoming);
  const auto retained = [](tinygltf::Model& model, const Json& packets) {
    declare(model, kGltfXmpExtension);
    model.extensions.emplace(kGltfXmpExtension, Value(Object{}));
    model.extensions_json_string = Json{{kGltfXmpExtension, {{"packets", packets}}}}.dump();
  };
  const auto attach = [](auto& record, int index) {
    record.extensions.emplace(kGltfXmpExtension, Value(Object{{"packet", Value(index)}}));
  };
  const auto destinationPackets = Json::parse(R"([{"@context":{},"label":"destination"},null])");
  Json incomingPackets = Json::array();
  for (const auto* label : {"asset", "scene", "node", "mesh", "material", "image", "animation"}) {
    incomingPackets.push_back({{"@context", Json::object()}, {"label", label},
        {"integer", uint64_t{18446744073709551614ull}}, {"empty", Json::array()}});
  }
  retained(base, destinationPackets);
  retained(incoming, incomingPackets);
  attach(base.asset, 0);
  attach(base.nodes[0], 4); // Within the combined table, outside its own source table.
  attach(base.materials[0], 1);
  attach(incoming.asset, 0);
  attach(incoming.scenes[0], 1);
  attach(incoming.nodes[0], 2);
  attach(incoming.meshes[0], 3);
  attach(incoming.materials[0], 4);
  attach(incoming.images[0], 5);
  attach(incoming.animations[0], 6);

  const auto plan = PlanGltfSessionSources(base, base, incoming, {}, {});
  const auto& model = plan.merge.model;
  const auto package = ReadGltfXmpPackage(model);
  ASSERT_TRUE(package);
  auto expected = destinationPackets;
  for (const auto& packet : incomingPackets) expected.push_back(packet);
  EXPECT_EQ(package->at("packets"), expected);
  EXPECT_EQ(ResolveGltfXmpPacket(*package, model.asset.extensions).packet, &package->at("packets").at(0));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.nodes[0].extensions).message.contains("outside"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, plan.evaluatedSource.nodes[0].extensions).message.contains("outside"));
  EXPECT_TRUE(ResolveGltfXmpPacket(*package, model.materials[0].extensions).message.contains("@context"));
  const std::array attachments{&model.nodes[size_t(plan.merge.parentNode)].extensions,
      &model.scenes[size_t(plan.merge.incomingScene)].extensions, &model.nodes[1].extensions,
      &model.meshes[1].extensions, &model.materials[1].extensions, &model.images[1].extensions,
      &model.animations[0].extensions};
  for (size_t i = 0; i < attachments.size(); ++i) {
    const auto packet = ResolveGltfXmpPacket(*package, *attachments[i]);
    ASSERT_NE(packet.packet, nullptr);
    EXPECT_EQ(*packet.packet, incomingPackets[i]);
  }
  EXPECT_EQ(model.nodes[size_t(plan.merge.placement.nodes[0])].extensions, model.nodes[1].extensions);
  EXPECT_EQ(plan.merge.incomingAsset.extensions, model.nodes[size_t(plan.merge.parentNode)].extensions);
  EXPECT_TRUE(ResolveGltfXmpPacket(*ReadGltfXmpPackage(base), base.nodes[0].extensions).message.contains("outside"));
}

TEST(GltfMerge, RejectsUnknownExtensionsTargetsAndRootConflictsWithoutInputMutation) {
  const auto base=fixture();auto incoming=fixture();const auto before=base;
  declare(incoming,"VENDOR_references");incoming.nodes[0].extensions["VENDOR_references"]=Value(Object{{"index",Value(0)}});
  EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);EXPECT_EQ(base,before);
  incoming=fixture();incoming.extras=Value(Object{{"different",Value(2)}});auto withExtras=base;withExtras.extras=Value(Object{{"different",Value(1)}});
  EXPECT_THROW((void)PlanGltfMerge(withExtras,incoming),std::runtime_error);
  incoming=fixture();coreAnimation(incoming);const auto output=floats(incoming,{0,1},TINYGLTF_TYPE_SCALAR,2);
  pointerChannel(incoming,incoming.animations[0],"/nodes/0/extensions/VENDOR_references/value",output);
  EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
}

TEST(GltfMerge, PreservesOpaqueOptionalDestinationMetadataAtStableIndices) {
  auto base=fixture();
  const auto incoming=fixture(33);
  const std::string name="VENDOR_destination_metadata";
  const Value metadata(Object{{"source",Value(9001)},{"label",Value(std::string("opaque"))}});
  declare(base,name);
  base.extensions[name]=metadata;
  base.meshes[0].primitives[0].extensions[name]=metadata;
  base.textures[0].extensions[name]=metadata;
  base.asset.version="2.1";
  base.asset.minVersion="2.0";

  const auto merged=PlanGltfMerge(base,incoming);
  EXPECT_EQ(merged.model.asset.version,"2.1");
  EXPECT_EQ(merged.model.extensions.at(name),metadata);
  EXPECT_EQ(merged.model.meshes[0].primitives[0].extensions.at(name),metadata);
  EXPECT_EQ(merged.model.textures[0].extensions.at(name),metadata);
  EXPECT_EQ(merged.model.textures[0].source,0);
  EXPECT_EQ(merged.model.textures[1].source,1);
  EXPECT_EQ(std::ranges::count(merged.model.extensionsUsed,name),1);

  base.extensionsRequired.push_back(name);
  EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
  base.extensionsRequired.clear();
  base.textures[0].source=-1;
  EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
}

TEST(GltfMerge, RejectsMalformedReferencesHierarchyAndNonresidentInputs) {
  const auto base=fixture();auto incoming=fixture();incoming.meshes[0].primitives[0].material=19;
  EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
  incoming=fixture();incoming.nodes[0].children={0};EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
  incoming=fixture();incoming.scenes[0].nodes={0,0};EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
  incoming=fixture();incoming.images[0].image.clear();EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
  incoming=fixture();incoming.bufferViews[0].byteOffset=SIZE_MAX;EXPECT_THROW((void)PlanGltfMerge(base,incoming),std::runtime_error);
}

TEST(GltfMerge, EnforcesCombinedPayloadMetadataNodeRootAndCloneBudgets) {
  const auto source=fixture();GltfMergeLimits limits;
  limits.maxNodes=3;EXPECT_THROW((void)PlanGltfMerge(source,source,{},limits),std::runtime_error);
  limits={};limits.maxResidentBytes=120;EXPECT_THROW((void)PlanGltfMerge(source,source,{},limits),std::runtime_error);
  limits={};limits.maxMetadataBytes=200;EXPECT_THROW((void)PlanGltfMerge(source,source,{},limits),std::runtime_error);
  limits={};limits.maxRootReferences=2;EXPECT_THROW((void)PlanGltfMerge(source,source,{},limits),std::runtime_error);
  auto animated=source;coreAnimation(animated);limits={};limits.maxClonedChannels=0;EXPECT_THROW((void)PlanGltfMerge(source,animated,{},limits),std::runtime_error);
  auto huge=source;huge.accessors[0].count=SIZE_MAX;limits={};EXPECT_THROW((void)PlanGltfMerge(source,huge,{},limits),std::runtime_error);
}

TEST(GltfMerge, RejectsInvalidPlacementAndSceneSelection) {
  const auto source=fixture();GltfMergeOptions options;options.incomingScene=99;EXPECT_THROW((void)PlanGltfMerge(source,source,options),std::runtime_error);
  options={};options.placementMatrix=std::array<double,16>{};EXPECT_THROW((void)PlanGltfMerge(source,source,options),std::runtime_error);
  options.placementMatrix=std::array<double,16>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};(*options.placementMatrix)[12]=std::numeric_limits<double>::infinity();
  EXPECT_THROW((void)PlanGltfMerge(source,source,options),std::runtime_error);
}
} // namespace
