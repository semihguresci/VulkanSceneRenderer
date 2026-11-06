#include <Container/geometry/GltfMergeRuntimeData.h>
#include <Container/geometry/GltfAccessor.h>

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>

namespace {
using namespace container::geometry::gltf;
using container::geometry::Mesh;
using container::geometry::Vertex;
using Value=tinygltf::Value;
using Object=Value::Object;
constexpr const char* kInstances="EXT_mesh_gpu_instancing";

struct Fixture {
  tinygltf::Model source;
  std::vector<Mesh> bind;
  Fixture() {
    const auto positions=accessor<float>({0,0,0,1,0,0,0,1,0},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT);
    tinygltf::Primitive primitive;primitive.mode=TINYGLTF_MODE_TRIANGLES;primitive.attributes["POSITION"]=positions;
    tinygltf::Mesh mesh;mesh.primitives.push_back(primitive);source.meshes.push_back(mesh);
    tinygltf::Node node;node.mesh=0;node.name="owner";source.nodes.push_back(node);
    tinygltf::Scene scene;scene.nodes={0};source.scenes.push_back(scene);source.defaultScene=0;
    std::vector<Vertex> vertices(3);
    for(size_t i=0;i<vertices.size();++i) {
      vertices[i].position={i==1?1.f:0.f,i==2?1.f:0.f,0};
      vertices[i].normal={0,0,1};vertices[i].tangent={-1,0,0,-1};
      vertices[i].texCoord={float(i)*.25f,.4f};vertices[i].texCoord1={.8f,float(i)*.3f};
      vertices[i].color={.2f,.4f,.6f};vertices[i].colorAlpha=.7f;
    }
    bind.emplace_back(std::move(vertices),std::vector<uint32_t>{0,1,2});
  }
  template<class T> int accessor(std::initializer_list<T> values,int type,int component) {
    tinygltf::Buffer buffer;buffer.data.resize(values.size()*sizeof(T));
    std::memcpy(buffer.data.data(),values.begin(),buffer.data.size());
    tinygltf::BufferView view;view.buffer=int(source.buffers.size());view.byteLength=buffer.data.size();
    tinygltf::Accessor value;value.sparse={};value.bufferView=int(source.bufferViews.size());
    value.type=type;value.componentType=component;value.count=values.size()/size_t(tinygltf::GetNumComponentsInType(type));
    source.buffers.push_back(std::move(buffer));source.bufferViews.push_back(view);source.accessors.push_back(value);
    return int(source.accessors.size()-1);
  }
  void morph(float meshDefault=.15f,float nodeDefault=.25f) {
    const auto delta=accessor<float>({2,0,0,2,0,0,2,0,0},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT);
    source.meshes[0].primitives[0].targets.push_back({{"POSITION",delta}});
    source.meshes[0].weights={meshDefault};source.nodes[0].weights={nodeDefault};
  }
  void animation() {
    const auto time=accessor<float>({0,1},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT);
    source.accessors[size_t(time)].minValues={0};source.accessors[size_t(time)].maxValues={1};
    const auto values=accessor<float>({0,.9f},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT);
    tinygltf::AnimationSampler sampler;sampler.input=time;sampler.output=values;
    tinygltf::AnimationChannel channel;channel.sampler=0;channel.target_node=0;channel.target_path="weights";
    tinygltf::Animation clip;clip.samplers.push_back(sampler);clip.channels.push_back(channel);source.animations.push_back(clip);
  }
  void skinWithExternalContext() {
    source.meshes[0].primitives[0].attributes["JOINTS_0"]=accessor<uint8_t>({0,0,0,0,0,0,0,0,0,0,0,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    source.meshes[0].primitives[0].attributes["WEIGHTS_0"]=accessor<float>({1,0,0,0,1,0,0,0,1,0,0,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_FLOAT);
    tinygltf::Node joint;joint.name="external joint";joint.translation={0,2,0};source.nodes.push_back(joint);
    tinygltf::Node ancestor;ancestor.name="context geometry";ancestor.mesh=0;ancestor.children={1};ancestor.translation={10,0,0};
    if(!source.meshes[0].primitives[0].targets.empty()) ancestor.weights={.65};
    source.nodes.push_back(ancestor);
    tinygltf::Skin skin;skin.joints={1};skin.skeleton=2;source.skins.push_back(skin);source.nodes[0].skin=0;
    tinygltf::Scene other;other.name="joint scene";other.nodes={2};source.scenes.push_back(other);
  }
  void dormantInfluences() {
    auto& primitive=source.meshes[0].primitives[0];
    primitive.attributes["JOINTS_0"]=accessor<uint16_t>({65535,257,17,0,65535,257,17,0,65535,257,17,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
    const auto first=accessor<uint8_t>({255,255,0,0,255,255,0,0,255,255,0,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE);
    source.accessors[size_t(first)].normalized=true;primitive.attributes["WEIGHTS_0"]=first;
    primitive.attributes["JOINTS_1"]=accessor<uint16_t>({50001,31,29,0,50001,31,29,0,50001,31,29,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
    const auto second=accessor<uint16_t>({65535,0,0,0,65535,0,0,0,65535,0,0,0},TINYGLTF_TYPE_VEC4,TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
    source.accessors[size_t(second)].normalized=true;primitive.attributes["WEIGHTS_1"]=second;
  }
  void instances(size_t node=0) {
    if(std::ranges::find(source.extensionsUsed,kInstances)==source.extensionsUsed.end()) source.extensionsUsed.push_back(kInstances);
    const auto transforms=accessor<float>({2,0,0,-3,1,0},TINYGLTF_TYPE_VEC3,TINYGLTF_COMPONENT_TYPE_FLOAT);
    const auto ids=accessor<uint32_t>({16'777'217u,0xffffffffu},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
    source.nodes[node].extensions[kInstances]=Value(Object{{"attributes",Value(Object{{"TRANSLATION",Value(transforms)},{"_ID",Value(ids)}})}});
  }
  void sharedOwner(bool skinned=false) {
    auto node=source.nodes[0];node.name="shared owner";node.translation={4,0,0};
    if(!skinned) node.skin=-1;
    if(!source.meshes[0].primitives[0].targets.empty()) node.weights={.45};
    source.nodes.push_back(node);source.scenes[0].nodes.push_back(int(source.nodes.size()-1));
  }
  void normalTexture(int texCoord,std::optional<int> override={}) {
    tinygltf::Image image;image.width=1;image.height=1;image.component=4;image.bits=8;
    image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;image.image={128,128,255,255};source.images.push_back(image);
    tinygltf::Texture texture;texture.source=0;source.textures.push_back(texture);
    tinygltf::Material material;material.normalTexture.index=0;material.normalTexture.texCoord=texCoord;
    if(override) {
      source.extensionsUsed.push_back("KHR_texture_transform");
      material.normalTexture.extensions["KHR_texture_transform"]=Value(Object{{"texCoord",Value(*override)}});
    }
    source.materials.push_back(material);source.meshes[0].primitives[0].material=0;
    source.meshes[0].primitives[0].attributes["TEXCOORD_0"]=accessor<float>({0,.4f,.25f,.4f,.5f,.4f},TINYGLTF_TYPE_VEC2,TINYGLTF_COMPONENT_TYPE_FLOAT);
    source.meshes[0].primitives[0].attributes["TEXCOORD_1"]=accessor<float>({.8f,0,.8f,.3f,.8f,.6f},TINYGLTF_TYPE_VEC2,TINYGLTF_COMPONENT_TYPE_FLOAT);
  }
  DeformationData deformation() const {return LoadDeformationData(source,bind);}
  GltfInstancingData instancing() const {return ParseMeshGpuInstancing(source);}
};

void sameVertex(const Vertex& a,const Vertex& b) {
  EXPECT_EQ(a.position,b.position);EXPECT_EQ(a.normal,b.normal);EXPECT_EQ(a.tangent,b.tangent);
  EXPECT_EQ(a.color,b.color);EXPECT_EQ(a.colorAlpha,b.colorAlpha);EXPECT_EQ(a.texCoord,b.texCoord);EXPECT_EQ(a.texCoord1,b.texCoord1);
}
void samePrimitive(const DeformationPrimitive& a,const DeformationPrimitive& b,bool identity=true) {
  if(identity) {EXPECT_EQ(a.sourceMesh,b.sourceMesh);EXPECT_EQ(a.sourcePrimitive,b.sourcePrimitive);}
  ASSERT_EQ(a.bindVertices.size(),b.bindVertices.size());
  for(size_t v=0;v<a.bindVertices.size();++v) sameVertex(a.bindVertices[v],b.bindVertices[v]);
  EXPECT_EQ(a.meshDefaultWeights,b.meshDefaultWeights);EXPECT_EQ(a.normalTextureTexCoord,b.normalTextureTexCoord);
  EXPECT_EQ(a.authoredBaseNormal,b.authoredBaseNormal);EXPECT_EQ(a.authoredBaseTangent,b.authoredBaseTangent);
  ASSERT_EQ(a.morphTargets.size(),b.morphTargets.size());
  for(size_t t=0;t<a.morphTargets.size();++t) {
    EXPECT_EQ(a.morphTargets[t].positions,b.morphTargets[t].positions);EXPECT_EQ(a.morphTargets[t].normals,b.morphTargets[t].normals);
    EXPECT_EQ(a.morphTargets[t].tangents,b.morphTargets[t].tangents);
  }
  ASSERT_EQ(a.influences.size(),b.influences.size());
  for(size_t v=0;v<a.influences.size();++v) {
    ASSERT_EQ(a.influences[v].values.size(),b.influences[v].values.size());
    for(size_t i=0;i<a.influences[v].values.size();++i) {
      EXPECT_EQ(a.influences[v].values[i].joint,b.influences[v].values[i].joint);
      EXPECT_EQ(a.influences[v].values[i].weight,b.influences[v].values[i].weight);
    }
  }
}

TEST(GltfMergeRuntimeData, StaticSourcesNeedNoBindCopiesAndKeepInstancingSorted) {
  Fixture a,b;a.instances();b.instances();const auto ai=a.instancing(),bi=b.instancing();
  const auto plan=PlanGltfMerge(a.source,b.source);
  const auto data=ComposeGltfMergeRuntimeData({a.source,nullptr,&ai},{b.source,nullptr,&bi},plan);
  EXPECT_FALSE(data.deformation);ASSERT_EQ(data.instancing.nodes.size(),3u);
  EXPECT_EQ(data.instancing.summary.instances,6u);EXPECT_EQ(data.instancing.summary.expandedPrimitives,6u);
  EXPECT_GT(data.instancing.summary.decodedBytes,0u);EXPECT_EQ(data.outputBytes,data.instancing.summary.decodedBytes);
  EXPECT_EQ(data.instancing.nodes[0].sourceNode,0u);
  EXPECT_EQ(data.instancing.nodes[1].sourceNode,uint32_t(plan.imported.nodes[0]));
  EXPECT_EQ(data.instancing.nodes[2].sourceNode,uint32_t(plan.placement.nodes[0]));
  EXPECT_EQ(data.instancing.nodes[2].localMatrices,bi.nodes[0].localMatrices);
}

TEST(GltfMergeRuntimeData, PreservesAnimatedAuthoredDefaultsAndVariantFramesAtNonrestPose) {
  Fixture a,b;a.morph();a.animation();auto retained=a.deformation();retained.primitives[0].normalTextureTexCoord=1;
  retained.primitives[0].bindVertices[0].normal={0,1,0};retained.primitives[0].bindVertices[0].tangent={0,0,-1,1};
  a.source.nodes[0].weights={.81};a.source.meshes[0].weights={.73};
  const auto beforeA=a.source,beforeB=b.source;const auto beforeData=retained;
  const auto plan=PlanGltfMerge(a.source,b.source);
  const auto result=ComposeGltfMergeRuntimeData({a.source,&retained},{b.source,nullptr,nullptr,b.bind},plan);
  ASSERT_TRUE(result.deformation);const auto& data=*result.deformation;
  EXPECT_EQ(data.nodes[0].defaultMorphWeights,(std::vector<float>{.25f}));
  EXPECT_EQ(data.primitives[0].meshDefaultWeights,(std::vector<float>{.15f}));
  samePrimitive(data.primitives[0],beforeData.primitives[0]);
  EXPECT_EQ(data.nodes[size_t(plan.parentNode)].sourceMesh,-1);EXPECT_EQ(data.nodes[size_t(plan.parentNode)].skin,-1);
  EXPECT_TRUE(data.nodes[size_t(plan.parentNode)].defaultMorphWeights.empty());
  EXPECT_EQ(a.source,beforeA);EXPECT_EQ(b.source,beforeB);samePrimitive(retained.primitives[0],beforeData.primitives[0]);
}

TEST(GltfMergeRuntimeData, RemapsExternalJointClosureAndStripsContextMorphAndInstancing) {
  Fixture a,b;b.morph();b.skinWithExternalContext();b.instances(2);
  const auto deformation=b.deformation();const auto instances=b.instancing();const auto plan=PlanGltfMerge(a.source,b.source);
  const auto result=ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&deformation,&instances},plan);
  ASSERT_TRUE(result.deformation);const auto& data=*result.deformation;ASSERT_EQ(data.skins.size(),2u);
  EXPECT_EQ(data.skins[size_t(plan.imported.skins[0])].joints,(std::vector<uint32_t>{uint32_t(plan.imported.nodes[1])}));
  EXPECT_EQ(data.skins[size_t(plan.placement.skins[0])].joints,(std::vector<uint32_t>{uint32_t(plan.placement.nodes[1])}));
  EXPECT_EQ(data.skins[size_t(plan.placement.skins[0])].skeleton,plan.placement.nodes[2]);
  EXPECT_EQ(data.skins[1].inverseBindMatrices,deformation.skins[0].inverseBindMatrices);
  for(const size_t source:{size_t(1),size_t(2)}) {
    const auto& context=data.nodes[size_t(plan.placement.nodes[source])];
    EXPECT_EQ(context.sourceMesh,-1);EXPECT_EQ(context.skin,-1);EXPECT_TRUE(context.defaultMorphWeights.empty());
  }
  EXPECT_EQ(data.nodes[size_t(plan.imported.nodes[2])].defaultMorphWeights,(std::vector<float>{.65f}));
  EXPECT_EQ(data.nodes[size_t(plan.placement.nodes[0])].skin,plan.placement.skins[0]);
  ASSERT_EQ(result.instancing.nodes.size(),1u);EXPECT_EQ(result.instancing.nodes[0].sourceNode,uint32_t(plan.imported.nodes[2]));
  EXPECT_EQ(FindNodeInstancing(result.instancing,uint32_t(plan.placement.nodes[2])),nullptr);
}

TEST(GltfMergeRuntimeData, SharedIncomingMeshAppendsOneSourceDefinitionForManyOwners) {
  Fixture a,b;a.morph();b.morph();b.skinWithExternalContext();b.sharedOwner(true);
  const auto ad=a.deformation(),bd=b.deformation();const auto plan=PlanGltfMerge(a.source,b.source);
  const auto result=ComposeGltfMergeRuntimeData({a.source,&ad},{b.source,&bd},plan);
  ASSERT_TRUE(result.deformation);ASSERT_EQ(result.deformation->primitives.size(),2u);
  EXPECT_EQ(result.deformation->primitives[0].sourceMesh,0u);EXPECT_EQ(result.deformation->primitives[1].sourceMesh,1u);
  samePrimitive(result.deformation->primitives[1],bd.primitives[0],false);
  EXPECT_EQ(result.deformation->nodes[size_t(plan.placement.nodes[0])].sourceMesh,1);
  EXPECT_EQ(result.deformation->nodes[size_t(plan.placement.nodes[3])].sourceMesh,1);
  EXPECT_EQ(result.deformation->nodes[size_t(plan.placement.nodes[3])].defaultMorphWeights,(std::vector<float>{.45f}));
}

TEST(GltfMergeRuntimeData, RetainedSkinPaletteMovesWorldPointOnlyByPlacement) {
  Fixture a,b;b.skinWithExternalContext();const auto bd=b.deformation();
  GltfMergeOptions options;options.placementMatrix=std::array<double,16>{-2,0,0,0,0,3,0,0,0,0,4,0,5,6,7,1};
  const auto plan=PlanGltfMerge(a.source,b.source,options);
  const auto result=ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&bd},plan);
  ASSERT_TRUE(result.deformation);
  glm::mat4 parent(1);for(int c=0;c<4;++c) for(int r=0;r<4;++r) parent[c][r]=float((*options.placementMatrix)[size_t(c*4+r)]);
  const auto owner=size_t(plan.placement.nodes[0]);
  std::vector<glm::mat4> worlds(plan.model.nodes.size(),glm::mat4(1));
  worlds[owner]=parent;
  worlds[size_t(plan.placement.nodes[2])]=parent*glm::translate(glm::mat4(1),glm::vec3(10,0,0));
  worlds[size_t(plan.placement.nodes[1])]=worlds[size_t(plan.placement.nodes[2])]*glm::translate(glm::mat4(1),glm::vec3(0,2,0));
  const auto palette=ComputeSkinPalette(result.deformation->skins[size_t(plan.placement.skins[0])],worlds[owner],worlds);
  const auto evaluated=EvaluatePrimitive(result.deformation->primitives[1],{},palette);
  const auto position=worlds[owner]*glm::vec4(evaluated[1].position,1);
  // Bind point (1,0,0) -> original external joint (11,2,0), then placement.
  EXPECT_NEAR(position.x,-17,1e-5);EXPECT_NEAR(position.y,12,1e-5);EXPECT_NEAR(position.z,7,1e-5);
}

TEST(GltfMergeRuntimeData, CustomInstanceIdsUseMappedAccessorsWithoutFloatConversion) {
  Fixture a,b;b.instances();const auto bi=b.instancing();const auto sourceBefore=b.source;
  const auto plan=PlanGltfMerge(a.source,b.source);
  const auto result=ComposeGltfMergeRuntimeData({a.source},{b.source,nullptr,&bi},plan);
  ASSERT_EQ(result.instancing.nodes.size(),2u);
  for(const auto& node:result.instancing.nodes) {
    EXPECT_EQ(node.sourceMesh,1u);EXPECT_EQ(node.localMatrices,bi.nodes[0].localMatrices);
    ASSERT_EQ(node.attributes.size(),2u);EXPECT_EQ(node.attributes[1].semantic,"_ID");
    EXPECT_EQ(node.attributes[1].sourceAccessor,plan.imported.accessors[size_t(bi.nodes[0].attributes[1].sourceAccessor)]);
    CheckedAccessor reader(plan.model,node.attributes[1].sourceAccessor,"merged custom identity");
    std::array<uint32_t,1> id{};reader.readUnsigned(0,id);EXPECT_EQ(id[0],16'777'217u);
    reader.readUnsigned(1,id);EXPECT_EQ(id[0],0xffffffffu);
  }
  EXPECT_EQ(b.source,sourceBefore);
}

TEST(GltfMergeRuntimeData, RepeatedMergeRetainsAllPriorSourceDefaultsAndPlacements) {
  Fixture a,b,c;a.morph();b.morph();b.instances();c.morph(.3f,.4f);
  const auto ad=a.deformation(),bd=b.deformation(),cd=c.deformation();const auto bi=b.instancing();
  const auto abPlan=PlanGltfMerge(a.source,b.source);
  const auto ab=ComposeGltfMergeRuntimeData({a.source,&ad},{b.source,&bd,&bi},abPlan);
  auto abLive=abPlan.model;abLive.nodes[0].weights={.94};abLive.nodes[size_t(abPlan.placement.nodes[0])].weights={.84};
  const auto abcPlan=PlanGltfMerge(abLive,c.source);
  const auto abc=ComposeGltfMergeRuntimeData({abLive,&*ab.deformation,&ab.instancing},{c.source,&cd},abcPlan);
  ASSERT_TRUE(abc.deformation);ASSERT_EQ(abc.deformation->primitives.size(),3u);
  for(size_t p=0;p<ab.deformation->primitives.size();++p) samePrimitive(abc.deformation->primitives[p],ab.deformation->primitives[p]);
  for(size_t n=0;n<ab.deformation->nodes.size();++n) {
    EXPECT_EQ(abc.deformation->nodes[n].defaultMorphWeights,ab.deformation->nodes[n].defaultMorphWeights);
    EXPECT_EQ(abc.deformation->nodes[n].sourceMesh,ab.deformation->nodes[n].sourceMesh);
  }
  EXPECT_EQ(abc.deformation->nodes[size_t(abcPlan.placement.nodes[0])].defaultMorphWeights,(std::vector<float>{.4f}));
  ASSERT_EQ(abc.instancing.nodes.size(),ab.instancing.nodes.size());
  EXPECT_EQ(abc.instancing.nodes[0].sourceNode,ab.instancing.nodes[0].sourceNode);
  EXPECT_EQ(abc.instancing.nodes[1].localMatrices,ab.instancing.nodes[1].localMatrices);
}

TEST(GltfMergeRuntimeData, DormantNormalizedJointSetsRemainExactAndSelfComposable) {
  Fixture a,b,c;a.dormantInfluences();b.morph();const auto bd=b.deformation();
  const auto beforeA=a.source;const auto beforeVertex=a.bind[0].vertices()[0];
  const auto abPlan=PlanGltfMerge(a.source,b.source);
  const auto ab=ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&bd},abPlan);
  ASSERT_TRUE(ab.deformation);ASSERT_EQ(ab.deformation->primitives.size(),2u);EXPECT_GT(ab.workspaceBytes,0u);
  const auto& dormant=ab.deformation->primitives[0];ASSERT_EQ(dormant.influences.size(),3u);
  for(const auto& vertex:dormant.influences) {
    ASSERT_EQ(vertex.values.size(),8u);EXPECT_EQ(vertex.values[0].joint,65535u);EXPECT_EQ(vertex.values[1].joint,257u);
    EXPECT_EQ(vertex.values[4].joint,50001u);EXPECT_EQ(vertex.values[5].joint,31u);
    // Both integer formats decode to 1,1,0,0 + 1,0,0,0, then normalize
    // across ALL sets: each of the three contributing weights becomes 1/3.
    EXPECT_FLOAT_EQ(vertex.values[0].weight,1.f/3);EXPECT_FLOAT_EQ(vertex.values[1].weight,1.f/3);
    EXPECT_FLOAT_EQ(vertex.values[4].weight,1.f/3);EXPECT_FLOAT_EQ(vertex.values[2].weight,0);
  }
  EXPECT_EQ(ab.deformation->nodes[0].skin,-1); // Dormant attributes do not attach a skin.
  const auto abcPlan=PlanGltfMerge(abPlan.model,c.source);
  const auto abc=ComposeGltfMergeRuntimeData({abPlan.model,&*ab.deformation,&ab.instancing},{c.source,nullptr,nullptr,c.bind},abcPlan);
  ASSERT_TRUE(abc.deformation);ASSERT_EQ(abc.deformation->primitives.size(),3u);
  samePrimitive(abc.deformation->primitives[0],dormant);
  EXPECT_EQ(a.source,beforeA);sameVertex(a.bind[0].vertices()[0],beforeVertex);
}

TEST(GltfMergeRuntimeData, DormantInfluenceLayoutErrorsDoNotCopyOrMutateRetainedData) {
  Fixture a,b;a.dormantInfluences();b.morph();const auto bd=b.deformation();const auto beforeData=bd;
  const auto plan=PlanGltfMerge(a.source,b.source);
  for(int malformed=0;malformed<3;++malformed) {
    auto bad=a;auto& primitive=bad.source.meshes[0].primitives[0];
    if(malformed==0) primitive.attributes.erase("WEIGHTS_1");
    else if(malformed==1) bad.source.accessors[size_t(primitive.attributes.at("WEIGHTS_0"))].normalized=false;
    else bad.source.accessors[size_t(primitive.attributes.at("JOINTS_0"))].count=2;
    auto badPlan=plan;badPlan.model.meshes[0].primitives[0].attributes=primitive.attributes;
    const auto sourceBefore=bad.source;const auto vertexBefore=bad.bind[0].vertices()[0];
    EXPECT_THROW((void)ComposeGltfMergeRuntimeData({bad.source,nullptr,nullptr,bad.bind},{b.source,&bd},badPlan),std::runtime_error)<<malformed;
    EXPECT_EQ(bad.source,sourceBefore);sameVertex(bad.bind[0].vertices()[0],vertexBefore);samePrimitive(bd.primitives[0],beforeData.primitives[0]);
  }
}

TEST(GltfMergeRuntimeData, StaticDecoderChecksOutputAndWorkspaceBoundsBeforePayloadDecode) {
  Fixture a,b;a.dormantInfluences();b.morph();const auto bd=b.deformation();const auto plan=PlanGltfMerge(a.source,b.source);
  const GltfMergeRuntimeDataInput base{a.source,nullptr,nullptr,a.bind},incoming{b.source,&bd};
  const auto valid=ComposeGltfMergeRuntimeData(base,incoming,plan);ASSERT_GT(valid.workspaceBytes,1u);
  GltfMergeRuntimeDataLimits limits;limits.maxWorkspaceBytes=valid.workspaceBytes;
  EXPECT_NO_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits));
  --limits.maxWorkspaceBytes;EXPECT_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits),std::runtime_error);
  auto invalid=a;invalid.source.accessors[size_t(invalid.source.meshes[0].primitives[0].attributes.at("WEIGHTS_0"))].componentType=999;
  const auto expectBudget=[&](const GltfMergeRuntimeDataLimits& cap,const char* expected) {
    try {
      (void)ComposeGltfMergeRuntimeData({invalid.source,nullptr,nullptr,invalid.bind},incoming,plan,cap);
      FAIL()<<"Expected a predecode budget rejection";
    } catch(const std::runtime_error& error) {EXPECT_NE(std::string(error.what()).find(expected),std::string::npos)<<error.what();}
  };
  limits={};limits.maxOutputBytes=1;expectBudget(limits,"owned output");
  limits={};limits.maxWorkspaceBytes=1;expectBudget(limits,"temporary workspace");
}

TEST(GltfMergeRuntimeData, StaticFallbackKeepsBindFramesAndSourceOrderWithoutRuntimeGaps) {
  Fixture a,b;b.morph();const auto bd=b.deformation();
  const auto sourcePosition=a.source.meshes[0].primitives[0].attributes.at("POSITION");
  // A procedural runtime gap never enters this source-ordered input span.
  std::vector<Mesh> runtime={a.bind[0],Mesh(a.bind[0].vertices(),{0,1,2}),b.bind[0]};
  const auto plan=PlanGltfMerge(a.source,b.source);
  const auto data=ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,std::span(runtime).first(1)},{b.source,&bd},plan);
  ASSERT_TRUE(data.deformation);ASSERT_EQ(data.deformation->primitives.size(),2u);
  EXPECT_EQ(sourcePosition,a.source.meshes[0].primitives[0].attributes.at("POSITION"));
  sameVertex(data.deformation->primitives[0].bindVertices[0],runtime[0].vertices()[0]);
  EXPECT_FALSE(data.deformation->primitives[0].authoredBaseNormal);EXPECT_FALSE(data.deformation->primitives[0].authoredBaseTangent);
  EXPECT_EQ(data.deformation->primitives[1].sourceMesh,1u);
}

TEST(GltfMergeRuntimeData, StaticNormalUvRetainsDefaultTransformOverrideAndCurrentVariantPolicy) {
  for(int policy=0;policy<3;++policy) {
    Fixture a,b;b.morph();const auto bd=b.deformation();
    if(policy==0) a.normalTexture(1);
    else if(policy==1) a.normalTexture(7,1); // Only the effective selector needs UV0/UV1.
    else a.normalTexture(0); // Active variant differs from raw source default.
    const std::array<uint32_t,1> selected{1};
    const auto sourceBefore=a.source;const auto vertexBefore=a.bind[0].vertices()[0];
    const auto plan=PlanGltfMerge(a.source,b.source);
    const GltfMergeRuntimeDataInput base{a.source,nullptr,nullptr,a.bind,policy==2?std::span<const uint32_t>(selected):std::span<const uint32_t>{}};
    const auto result=ComposeGltfMergeRuntimeData(base,{b.source,&bd},plan);
    ASSERT_TRUE(result.deformation);ASSERT_EQ(result.deformation->primitives.size(),2u);
    EXPECT_EQ(result.deformation->primitives[0].normalTextureTexCoord,1u)<<policy;
    sameVertex(result.deformation->primitives[0].bindVertices[0],vertexBefore);
    EXPECT_EQ(a.source,sourceBefore);sameVertex(a.bind[0].vertices()[0],vertexBefore);
  }
}

TEST(GltfMergeRuntimeData, NormalUvPolicyValidatesFullSourceTableAndRetainedConsistency) {
  Fixture a,b;a.morph();auto ad=a.deformation();ad.primitives[0].normalTextureTexCoord=1;
  const auto plan=PlanGltfMerge(a.source,b.source);
  const std::array<uint32_t,1> selected{1},different{0},unsupported{2};const std::array<uint32_t,2> wrongSize{1,1};
  const GltfMergeRuntimeDataInput incoming{b.source,nullptr,nullptr,b.bind};
  EXPECT_NO_THROW((void)ComposeGltfMergeRuntimeData({a.source,&ad,nullptr,{},selected},incoming,plan));
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,&ad,nullptr,{},different},incoming,plan),std::runtime_error);
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,&ad,nullptr,{},unsupported},incoming,plan),std::runtime_error);
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,&ad,nullptr,{},wrongSize},incoming,plan),std::runtime_error);
  ad.primitives[0].normalTextureTexCoord=2;
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,&ad},incoming,plan),std::runtime_error);
  // Validate an explicitly supplied static policy even if no deformation is needed.
  Fixture c,d;const auto staticPlan=PlanGltfMerge(c.source,d.source);
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({c.source,nullptr,nullptr,{},unsupported},{d.source},staticPlan),std::runtime_error);
}

TEST(GltfMergeRuntimeData, ExplicitAccessorMapsAllowArchiveOnlyResourceSuffixes) {
  Fixture a,b;b.instances();auto archive=a;
  (void)archive.accessor<float>({27},TINYGLTF_TYPE_SCALAR,TINYGLTF_COMPONENT_TYPE_FLOAT);
  const auto bi=b.instancing();const auto plan=PlanGltfMerge(archive.source,b.source);
  const auto result=ComposeGltfMergeRuntimeData({a.source},{b.source,nullptr,&bi},plan);
  EXPECT_GT(plan.imported.accessors[0],int32_t(a.source.accessors.size()));
  EXPECT_EQ(result.instancing.nodes[0].attributes[0].sourceAccessor,
      plan.imported.accessors[size_t(bi.nodes[0].attributes[0].sourceAccessor)]);
}

TEST(GltfMergeRuntimeData, MissingDynamicOrStaticBindDataRejectsBeforePublication) {
  Fixture a,b;b.morph();const auto bd=b.deformation();const auto plan=PlanGltfMerge(a.source,b.source);
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source},{b.source,&bd},plan),std::runtime_error);
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source},plan),std::runtime_error);
  auto bad=bd;bad.primitives[0].sourceMesh=7;
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&bad},plan),std::runtime_error);
  bad=bd;bad.nodes[0].defaultMorphWeights.clear();
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&bad},plan),std::runtime_error);
  const std::vector<Mesh> wrong{Mesh(std::vector<Vertex>(2),{0,1,1})};
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,wrong},{b.source,&bd},plan),std::runtime_error);
}

TEST(GltfMergeRuntimeData, CorruptedMergeMapsAndAttachmentsRejectWithoutInputMutation) {
  Fixture a,b;b.morph();b.skinWithExternalContext();const auto bd=b.deformation();
  const auto plan=PlanGltfMerge(a.source,b.source),before=plan;const auto sourceBefore=b.source;
  for(int corruption=0;corruption<8;++corruption) {
    auto bad=plan;
    switch(corruption) {
      case 0:bad.imported.meshes[0]=0;break;
      case 1:bad.imported.nodes[0]=0;break;
      case 2:bad.placement.nodes[1]=bad.placement.nodes[0];break;
      case 3:bad.placement.skins[0]=0;break;
      case 4:bad.imported.accessors[0]=0;break;
      case 5:bad.imported.primitives[0][0].primitive=1;break;
      case 6:bad.model.nodes[size_t(bad.placement.nodes[0])].skin=-1;break;
      case 7:bad.model.skins[1].joints[0]=0;break;
    }
    EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&bd},bad),std::runtime_error)<<corruption;
  }
  EXPECT_EQ(plan.model,before.model);EXPECT_EQ(b.source,sourceBefore);
  EXPECT_EQ(bd.nodes[0].defaultMorphWeights,(std::vector<float>{.25f}));
}

TEST(GltfMergeRuntimeData, AggregateOutputAndInstanceLimitsIncludePlacementCopies) {
  Fixture a,b;b.morph();b.instances();const auto bd=b.deformation();const auto bi=b.instancing();
  const auto plan=PlanGltfMerge(a.source,b.source);
  const GltfMergeRuntimeDataInput base{a.source,nullptr,nullptr,a.bind},incoming{b.source,&bd,&bi};
  const auto valid=ComposeGltfMergeRuntimeData(base,incoming,plan);ASSERT_GT(valid.outputBytes,1u);
  GltfMergeRuntimeDataLimits limits;limits.maxOutputBytes=valid.outputBytes;
  EXPECT_NO_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits));
  --limits.maxOutputBytes;EXPECT_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits),std::runtime_error);
  limits={};limits.maxInstances=3;EXPECT_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits),std::runtime_error);
  limits={};limits.maxExpandedPrimitives=3;EXPECT_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits),std::runtime_error);
  limits={};limits.maxPrimitives=1;EXPECT_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits),std::runtime_error);
  limits={};limits.maxNodes=2;EXPECT_THROW((void)ComposeGltfMergeRuntimeData(base,incoming,plan,limits),std::runtime_error);
}

TEST(GltfMergeRuntimeData, NonfiniteRetainedDefaultsFramesSkinAndInstancesReject) {
  Fixture a,b;b.morph();b.skinWithExternalContext();b.instances(2);
  const auto bd=b.deformation();const auto bi=b.instancing();const auto plan=PlanGltfMerge(a.source,b.source);
  for(int corruption=0;corruption<5;++corruption) {
    auto data=bd;auto instances=bi;const auto nan=std::numeric_limits<float>::quiet_NaN();
    switch(corruption) {
      case 0:data.nodes[0].defaultMorphWeights[0]=nan;break;
      case 1:data.primitives[0].bindVertices[0].tangent.x=nan;break;
      case 2:data.skins[0].inverseBindMatrices[0][0][0]=nan;break;
      case 3:data.primitives[0].morphTargets[0].positions[0].x=nan;break;
      case 4:instances.nodes[0].localMatrices[0][0][0]=nan;break;
    }
    EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source,nullptr,nullptr,a.bind},{b.source,&data,&instances},plan),std::runtime_error)<<corruption;
  }
}

TEST(GltfMergeRuntimeData, MissingDuplicateAndWrongAccessorInstancingRowsReject) {
  Fixture a,b;b.instances();const auto bi=b.instancing();const auto plan=PlanGltfMerge(a.source,b.source);
  EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source},{b.source},plan),std::runtime_error);
  for(int corruption=0;corruption<3;++corruption) {
    auto bad=bi;
    if(corruption==0) bad.nodes.push_back(bad.nodes[0]);
    else if(corruption==1) bad.nodes[0].attributes[0].sourceAccessor=0;
    else bad.nodes[0].sourceMesh=1;
    EXPECT_THROW((void)ComposeGltfMergeRuntimeData({a.source},{b.source,nullptr,&bad},plan),std::runtime_error)<<corruption;
  }
}
} // namespace
