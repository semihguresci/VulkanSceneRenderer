#include <Container/utility/SceneManager.h>

#include <Container/utility/SourceMaterialMapping.h>
#include <Container/utility/SourcePrimitiveMapping.h>
#include <Container/utility/TextureImageOwnership.h>
#include <Container/utility/TextureSamplingPlan.h>
#include <Container/utility/VulkanDevice.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace container::scene {
namespace {
using namespace container::geometry;
using namespace container::geometry::gltf;

[[noreturn]] void mergeFailure(const char* reason) {
  throw std::runtime_error(std::string("glTF merge generation: ") + reason);
}
template<class Prepare> void mergeResourcePreflight(Prepare&& prepare) {
  try {prepare();}
  catch(const std::invalid_argument& error) {mergeFailure(error.what());}
  catch(const std::length_error& error) {mergeFailure(error.what());}
}
uint64_t checkedAdd(uint64_t a,uint64_t b) {
  if(b>UINT64_MAX-a) mergeFailure("byte/count arithmetic overflow");
  return a+b;
}
uint64_t checkedBytes(uint64_t count,uint64_t width) {
  if(width && count>UINT64_MAX/width) mergeFailure("byte/count arithmetic overflow");
  return count*width;
}
struct Budget {
  uint64_t limit{},bytes{};
  void add(uint64_t value) {
    bytes=checkedAdd(bytes,value);
    if(bytes>limit) mergeFailure("aggregate detached CPU storage exceeds its budget");
  }
};
void metadataStorage(Budget& budget,const tinygltf::Value& value,uint32_t depth,uint32_t maxDepth) {
  if(depth>maxDepth) mergeFailure("retained export metadata exceeds the nesting budget");
  budget.add(sizeof(tinygltf::Value));
  if(value.IsString()) budget.add(value.Get<std::string>().size());
  else if(value.IsBinary()) budget.add(value.Get<std::vector<unsigned char>>().size());
  else if(value.IsArray())
    for(const auto& child:value.Get<tinygltf::Value::Array>()) metadataStorage(budget,child,depth+1,maxDepth);
  else if(value.IsObject())
    for(const auto& [key,child]:value.Get<tinygltf::Value::Object>()) {
      budget.add(checkedAdd(128,key.size()));metadataStorage(budget,child,depth+1,maxDepth);
    }
}
template<class Property> void propertyStorage(Budget& budget,const Property& property,uint32_t maxDepth) {
  budget.add(property.extras_json_string.size());budget.add(property.extensions_json_string.size());
  metadataStorage(budget,property.extras,0,maxDepth);
  for(const auto& [key,value]:property.extensions) {
    budget.add(checkedAdd(128,key.size()));metadataStorage(budget,value,0,maxDepth);
  }
}
void materialStorage(Budget& budget,const tinygltf::Material& material,uint32_t maxDepth) {
  budget.add(sizeof(material));budget.add(material.name.size());budget.add(material.alphaMode.size());
  budget.add(checkedBytes(checkedAdd(material.emissiveFactor.size(),material.pbrMetallicRoughness.baseColorFactor.size()),sizeof(double)));
  budget.add(checkedBytes(material.lods.size(),sizeof(int)));
  propertyStorage(budget,material,maxDepth);
  propertyStorage(budget,material.pbrMetallicRoughness,maxDepth);
  propertyStorage(budget,material.pbrMetallicRoughness.baseColorTexture,maxDepth);
  propertyStorage(budget,material.pbrMetallicRoughness.metallicRoughnessTexture,maxDepth);
  propertyStorage(budget,material.normalTexture,maxDepth);propertyStorage(budget,material.occlusionTexture,maxDepth);
  propertyStorage(budget,material.emissiveTexture,maxDepth);
  for(const auto* map:{&material.values,&material.additionalValues})
    for(const auto& [key,value]:*map) {
      budget.add(128);budget.add(key.size());budget.add(value.string_value.size());
      budget.add(checkedBytes(value.number_array.size(),sizeof(double)));
      for(const auto& [name,number]:value.json_double_value) budget.add(checkedAdd(128,name.size()));
    }
}
uint64_t residentBytes(const tinygltf::Model& model) {
  uint64_t bytes{};
  for(const auto& buffer:model.buffers) bytes=checkedAdd(bytes,buffer.data.size());
  for(const auto& image:model.images) bytes=checkedAdd(bytes,image.image.size());
  return bytes;
}
uint64_t streamBytes(const Model& model) {
  return checkedAdd(checkedBytes(model.vertices().size(),sizeof(Vertex)),
                    checkedBytes(model.indices().size(),sizeof(uint32_t)));
}
uint64_t sourceBindBytes(const Model& model,std::span<const uint32_t> map) {
  uint64_t bytes=checkedBytes(map.size(),sizeof(Mesh));
  for(const auto runtime:map) {
    if(runtime>=model.meshes().size()) mergeFailure("source bind primitive identity is invalid");
    const auto& mesh=model.meshes()[runtime];
    bytes=checkedAdd(bytes,checkedBytes(mesh.vertices().size(),sizeof(Vertex)));
    bytes=checkedAdd(bytes,checkedBytes(mesh.indices().size(),sizeof(uint32_t)));
  }
  return bytes;
}
uint64_t graphWorkspaceBytes(const SceneGraph& base,const tinygltf::Model& source,
    const GltfInstancingData& instancing,const GltfMergeGraphLimits& limits) {
  uint64_t retained=checkedBytes(base.nodeCount(),sizeof(SceneNode)+8*sizeof(uint32_t));
  for(uint32_t id=0;id<base.nodeCount();++id) {
    const auto& node=*base.getNode(id);
    retained=checkedAdd(retained,node.name.size());
    retained=checkedAdd(retained,checkedBytes(node.children.size(),sizeof(uint32_t)));
  }
  uint64_t generated=sizeof(SceneNode)+64,nodes=1,renderables{};
  for(size_t id=0;id<source.nodes.size();++id) {
    const auto& node=source.nodes[id];
    nodes=checkedAdd(nodes,1);
    generated=checkedAdd(generated,sizeof(SceneNode)+96+node.name.size());
    generated=checkedAdd(generated,checkedBytes(node.children.size(),sizeof(uint32_t)));
    if(node.mesh<0) continue;
    if(size_t(node.mesh)>=source.meshes.size()) mergeFailure("source node mesh is invalid");
    const auto* instances=FindNodeInstancing(instancing,static_cast<uint32_t>(id));
    const auto count=checkedBytes(source.meshes[size_t(node.mesh)].primitives.size(),
        instances?instances->localMatrices.size():1);
    nodes=checkedAdd(nodes,count);renderables=checkedAdd(renderables,count);
    generated=checkedAdd(generated,checkedBytes(count,sizeof(SceneNode)+96+sizeof(uint32_t)));
  }
  if(nodes>limits.hierarchy.maxGraphNodes || renderables>limits.hierarchy.maxRenderableObjects)
    mergeFailure("complete generated hierarchy exceeds its table budget");
  // Generated hierarchy, append tail and detached candidate can overlap in
  // preparation. Reserve their names/topology plus linear validation/world maps.
  return checkedAdd(retained,checkedAdd(checkedBytes(generated,3),
      checkedBytes(nodes,sizeof(glm::mat4)+12*sizeof(uint32_t))));
}
std::vector<Mesh> sourceBind(const Model& model,std::span<const uint32_t> map) {
  std::vector<Mesh> result; result.reserve(map.size());
  for(const auto runtime:map) result.push_back(model.meshes()[runtime]);
  return result;
}
std::vector<uint32_t> refinedRoots(std::span<const uint32_t> roots,
                                 const SceneGraph& retained) {
  std::vector<uint32_t> result;
  std::vector<uint8_t> seen(retained.nodeCount(),0);
  const auto append=[&](uint32_t node) {
    if(node>=seen.size()) mergeFailure("retained scene root identity is invalid");
    if(!seen[node]) { seen[node]=1; result.push_back(node); }
  };
  for(const auto root:roots) {
    const auto* node=retained.getNode(root);
    // A retired procedural tail can remain in the old membership table for
    // redo. It is absent from this branch and must not alias a new merge ID.
    if(!node) continue;
    if(node->editorSceneRoot && !node->renderable && node->sourceNodeIndex==-1)
      for(const auto child:node->children) append(child);
    else append(root);
  }
  return result;
}
void storageBudget(uint64_t count,uint64_t width,uint64_t limit,
                   const VkPhysicalDeviceLimits& deviceLimits) {
  const auto bytes=checkedBytes(std::max<uint64_t>(1,count),width);
  if(!limit || bytes>limit || bytes>deviceLimits.maxStorageBufferRange)
    mergeFailure("material/texture metadata storage exceeds its CPU or device budget");
}

// Logical payload accounting deliberately counts embedded property objects a
// second time. This leaves room for tree/map bookkeeping without depending on
// one STL's allocation layout. It does not copy JSON or expand any accessors.
constexpr uint32_t kHistoryMetadataDepth=64;
template<class T> void retainedVector(Budget& budget,const std::vector<T>& values) {
  budget.add(checkedBytes(values.size(),sizeof(T)));
}
void retainedText(Budget& budget,const std::string& value) {
  budget.add(checkedAdd(value.size(),1));
}
template<class T> void retainedSourceObject(Budget& budget,const T& value) {
  budget.add(sizeof(T));
  propertyStorage(budget,value,kHistoryMetadataDepth);
#define RETAINED_TEXT(member) if constexpr(requires { std::string_view(value.member); }) retainedText(budget,value.member)
  RETAINED_TEXT(name); RETAINED_TEXT(uri); RETAINED_TEXT(mimeType);
  RETAINED_TEXT(type); RETAINED_TEXT(alphaMode); RETAINED_TEXT(version);
  RETAINED_TEXT(minVersion); RETAINED_TEXT(generator); RETAINED_TEXT(copyright);
  RETAINED_TEXT(interpolation); RETAINED_TEXT(target_path); RETAINED_TEXT(distanceModel);
#undef RETAINED_TEXT
#define RETAINED_VECTOR(member) if constexpr(requires { value.member; }) retainedVector(budget,value.member)
  RETAINED_VECTOR(children); RETAINED_VECTOR(lods); RETAINED_VECTOR(rotation);
  RETAINED_VECTOR(translation); RETAINED_VECTOR(matrix); RETAINED_VECTOR(weights);
  RETAINED_VECTOR(joints); RETAINED_VECTOR(minValues); RETAINED_VECTOR(maxValues);
  RETAINED_VECTOR(emissiveFactor); RETAINED_VECTOR(baseColorFactor); RETAINED_VECTOR(color);
  RETAINED_VECTOR(image); RETAINED_VECTOR(data); RETAINED_VECTOR(audioEmitters);
#undef RETAINED_VECTOR
  if constexpr(requires { value.scale; })
    if constexpr(!std::is_arithmetic_v<std::remove_cvref_t<decltype(value.scale)>>)
      retainedVector(budget,value.scale);
  if constexpr(requires { value.pbrMetallicRoughness; }) {
    retainedSourceObject(budget,value.pbrMetallicRoughness);
    retainedSourceObject(budget,value.normalTexture);
    retainedSourceObject(budget,value.occlusionTexture);
    retainedSourceObject(budget,value.emissiveTexture);
    for(const auto* map:{&value.values,&value.additionalValues})
      for(const auto& [name,parameter]:*map) {
        budget.add(128+sizeof(parameter)); retainedText(budget,name);
        retainedText(budget,parameter.string_value); retainedVector(budget,parameter.number_array);
        for(const auto& [key,number]:parameter.json_double_value) {
          budget.add(128+sizeof(number)); retainedText(budget,key);
        }
      }
  }
  if constexpr(requires { value.baseColorTexture; }) {
    retainedSourceObject(budget,value.baseColorTexture);
    retainedSourceObject(budget,value.metallicRoughnessTexture);
  }
  if constexpr(requires { value.spot; }) retainedSourceObject(budget,value.spot);
  if constexpr(requires { value.positional; }) retainedSourceObject(budget,value.positional);
  if constexpr(requires { value.perspective; }) {
    retainedSourceObject(budget,value.perspective); retainedSourceObject(budget,value.orthographic);
  }
  const auto references=[&](const auto& attributes) {
    for(const auto& [name,index]:attributes) {
      budget.add(128+sizeof(index)); retainedText(budget,name);
    }
  };
  if constexpr(requires { value.attributes; }) references(value.attributes);
  if constexpr(requires { value.targets; }) {
    retainedVector(budget,value.targets);
    for(const auto& target:value.targets) references(target);
  }
#define RETAINED_OBJECTS(member) if constexpr(requires { value.member; }) \
    for(const auto& child:value.member) retainedSourceObject(budget,child)
  RETAINED_OBJECTS(primitives); RETAINED_OBJECTS(samplers); RETAINED_OBJECTS(channels);
#undef RETAINED_OBJECTS
  if constexpr(requires { value.target_extras; }) {
    metadataStorage(budget,value.target_extras,0,kHistoryMetadataDepth);
    retainedText(budget,value.target_extras_json_string);
    retainedText(budget,value.target_extensions_json_string);
    for(const auto& [name,extension]:value.target_extensions) {
      budget.add(128); retainedText(budget,name);
      metadataStorage(budget,extension,0,kHistoryMetadataDepth);
    }
  }
  if constexpr(requires { value.sparse; }) {
    retainedSourceObject(budget,value.sparse);
    retainedSourceObject(budget,value.sparse.indices);
    retainedSourceObject(budget,value.sparse.values);
  }
}
void retainedSourceModel(Budget& budget,const tinygltf::Model& model) {
  retainedSourceObject(budget,model);
  retainedSourceObject(budget,model.asset);
  for(const auto* names:{&model.extensionsUsed,&model.extensionsRequired}) {
    retainedVector(budget,*names);
    for(const auto& name:*names) retainedText(budget,name);
  }
#define RETAINED_TABLE(member) for(const auto& value:model.member) retainedSourceObject(budget,value)
  RETAINED_TABLE(buffers); RETAINED_TABLE(bufferViews); RETAINED_TABLE(accessors);
  RETAINED_TABLE(images); RETAINED_TABLE(samplers); RETAINED_TABLE(textures);
  RETAINED_TABLE(materials); RETAINED_TABLE(meshes); RETAINED_TABLE(nodes); RETAINED_TABLE(skins);
  RETAINED_TABLE(cameras); RETAINED_TABLE(lights); RETAINED_TABLE(animations); RETAINED_TABLE(scenes);
  RETAINED_TABLE(audioEmitters); RETAINED_TABLE(audioSources);
#undef RETAINED_TABLE
  for(const auto& scene:model.scenes) retainedVector(budget,scene.nodes);
}
void retainedGeometry(Budget& budget,const Model& model) {
  budget.add(sizeof(Model));
  retainedVector(budget,model.meshes());
  retainedVector(budget,model.vertices()); retainedVector(budget,model.indices());
  retainedVector(budget,model.primitiveRanges());
  for(const auto& mesh:model.meshes()) {
    retainedVector(budget,mesh.vertices()); retainedVector(budget,mesh.indices());
  }
}
void retainedDeformation(Budget& budget,const DeformationData& data) {
  budget.add(sizeof(data));
  retainedVector(budget,data.primitives); retainedVector(budget,data.skins); retainedVector(budget,data.nodes);
  for(const auto& primitive:data.primitives) {
    retainedVector(budget,primitive.bindVertices); retainedVector(budget,primitive.influences);
    retainedVector(budget,primitive.morphTargets); retainedVector(budget,primitive.meshDefaultWeights);
    for(const auto& influences:primitive.influences) retainedVector(budget,influences.values);
    for(const auto& target:primitive.morphTargets) {
      retainedVector(budget,target.positions); retainedVector(budget,target.normals);
      retainedVector(budget,target.tangents);
    }
  }
  for(const auto& skin:data.skins) {
    retainedVector(budget,skin.joints); retainedVector(budget,skin.inverseBindMatrices);
  }
  for(const auto& node:data.nodes) retainedVector(budget,node.defaultMorphWeights);
}

uint64_t cloneDeformationGrowth(const DeformationData& data,const tinygltf::Model& source,
    const GltfProjectedSubtreeClonePlan& projected,const GltfMergeRuntimeDataLimits& limits) {
  const auto& plan=projected.clone;const auto& output=plan.authoredSource;
  if(data.nodes.size()!=source.nodes.size() || data.skins.size()!=source.skins.size() ||
      output.nodes.size()>limits.maxNodes || output.skins.size()>limits.maxSkins ||
      data.primitives.size()>limits.maxPrimitives) mergeFailure("retained deformation identity/counts differ");
  uint64_t primitiveCount{};
  for(const auto& mesh:source.meshes) primitiveCount=checkedAdd(primitiveCount,mesh.primitives.size());
  if(data.primitives.size()!=primitiveCount) mergeFailure("retained deformation primitives are not source ordered");
  Budget bytes{limits.maxOutputBytes};retainedDeformation(bytes,data);const auto original=bytes.bytes;
  for(size_t n=0;n<plan.nodes.size();++n) if(plan.nodes[n]>=0) {
    const auto& node=data.nodes[n];
    if(node.sourceMesh!=source.nodes[n].mesh || node.skin!=source.nodes[n].skin)
      mergeFailure("retained deformation node binding differs from the source");
    bytes.add(sizeof(DeformationNode));
    if(projected.retainMeshBindings[n]) bytes.add(checkedBytes(node.defaultMorphWeights.size(),sizeof(float)));
  }
  for(size_t s=0;s<plan.skins.size();++s) if(plan.skins[s]>=0) {
    const auto& skin=data.skins[s];const auto& authored=source.skins[s];
    if(skin.joints.size()!=authored.joints.size() || skin.inverseBindMatrices.size()!=skin.joints.size() ||
        skin.skeleton!=authored.skeleton) mergeFailure("retained skin layout differs from source");
    for(size_t j=0;j<skin.joints.size();++j)
      if(authored.joints[j]<0 || skin.joints[j]!=uint32_t(authored.joints[j]) ||
          skin.joints[j]>=plan.nodes.size() || plan.nodes[skin.joints[j]]<0)
        mergeFailure("retained skin joint lacks a cloned source identity");
    if(skin.skeleton>=0 && (size_t(skin.skeleton)>=plan.nodes.size() || plan.nodes[size_t(skin.skeleton)]<0))
      mergeFailure("retained skeleton lacks a cloned source identity");
    bytes.add(sizeof(DeformationSkin));bytes.add(checkedBytes(skin.joints.size(),sizeof(uint32_t)));
    bytes.add(checkedBytes(skin.inverseBindMatrices.size(),sizeof(glm::mat4)));
  }
  return bytes.bytes-original;
}
DeformationData cloneDeformation(const DeformationData& retained,const GltfProjectedSubtreeClonePlan& projected) {
  const auto& plan=projected.clone;DeformationData output;output.primitives=retained.primitives;
  output.nodes.reserve(plan.authoredSource.nodes.size());output.skins.reserve(plan.authoredSource.skins.size());
  output.nodes.assign(retained.nodes.begin(),retained.nodes.end());output.skins.assign(retained.skins.begin(),retained.skins.end());
  output.nodes.resize(plan.authoredSource.nodes.size());output.skins.resize(plan.authoredSource.skins.size());
  for(size_t n=0;n<plan.nodes.size();++n) if(plan.nodes[n]>=0) {
    auto node=retained.nodes[n];const auto& source=plan.authoredSource.nodes[size_t(plan.nodes[n])];
    node.sourceMesh=source.mesh;node.skin=source.skin;
    if(!projected.retainMeshBindings[n]) node.defaultMorphWeights.clear();
    output.nodes[size_t(plan.nodes[n])]=std::move(node);
  }
  for(size_t s=0;s<plan.skins.size();++s) if(plan.skins[s]>=0) {
    auto skin=retained.skins[s];for(auto& joint:skin.joints) joint=uint32_t(plan.nodes[joint]);
    if(skin.skeleton>=0) skin.skeleton=plan.nodes[size_t(skin.skeleton)];
    output.skins[size_t(plan.skins[s])]=std::move(skin);
  }
  return output;
}
uint64_t cloneInstancingGrowth(const GltfInstancingData& data,const tinygltf::Model& source,
    const GltfProjectedSubtreeClonePlan& projected,const GltfMergeRuntimeDataLimits& limits) {
  Budget bytes{limits.maxOutputBytes};retainedVector(bytes,data.nodes);
  uint64_t instances{},expanded{},growth{};uint32_t previous{};bool first=true;
  for(const auto& node:data.nodes) {
    if(node.sourceNode>=source.nodes.size() || source.nodes[node.sourceNode].mesh<0 ||
        node.sourceMesh!=uint32_t(source.nodes[node.sourceNode].mesh) || node.sourceMesh>=source.meshes.size() ||
        node.primitiveCount!=source.meshes[node.sourceMesh].primitives.size() || !node.primitiveCount ||
        node.localMatrices.empty() || (!first && node.sourceNode<=previous))
      mergeFailure("retained instancing identities are invalid or unsorted");
    first=false;previous=node.sourceNode;
    const auto found=source.nodes[node.sourceNode].extensions.find("EXT_mesh_gpu_instancing");
    if(found==source.nodes[node.sourceNode].extensions.end() || !found->second.IsObject() ||
        !found->second.Has("attributes") || !found->second.Get("attributes").IsObject())
      mergeFailure("retained instancing has no source attribute map");
    const auto& attributes=found->second.Get("attributes").Get<tinygltf::Value::Object>();
    if(attributes.size()!=node.attributes.size()) mergeFailure("retained instance attribute table differs");
    uint64_t payload=sizeof(NodeInstancing)+checkedBytes(node.localMatrices.size(),sizeof(glm::mat4));
    payload=checkedAdd(payload,checkedBytes(node.attributes.size(),sizeof(InstanceAttributeReference)));
    for(const auto& attribute:node.attributes) {
      const auto value=attributes.find(attribute.semantic);
      if(value==attributes.end() || !value->second.IsInt() || value->second.Get<int>()!=attribute.sourceAccessor ||
          attribute.sourceAccessor<0 || size_t(attribute.sourceAccessor)>=source.accessors.size())
        mergeFailure("retained instance accessor identity differs");
      payload=checkedAdd(payload,attribute.semantic.size());
    }
    bytes.add(payload);instances=checkedAdd(instances,node.localMatrices.size());
    expanded=checkedAdd(expanded,checkedBytes(node.localMatrices.size(),node.primitiveCount));
    if(projected.clone.nodes[node.sourceNode]>=0 && projected.retainMeshBindings[node.sourceNode]) {
      bytes.add(payload);growth=checkedAdd(growth,payload);instances=checkedAdd(instances,node.localMatrices.size());
      expanded=checkedAdd(expanded,checkedBytes(node.localMatrices.size(),node.primitiveCount));
    }
  }
  for(size_t n=0;n<source.nodes.size();++n) if(source.nodes[n].extensions.contains("EXT_mesh_gpu_instancing") &&
      !FindNodeInstancing(data,uint32_t(n))) mergeFailure("source instance data is missing");
  if(instances>limits.maxInstances || expanded>limits.maxExpandedPrimitives)
    mergeFailure("cloned instance count/expansion exceeds its limit");
  return growth;
}
GltfInstancingData cloneInstancing(const GltfInstancingData& retained,const GltfProjectedSubtreeClonePlan& projected) {
  GltfInstancingData output;
  size_t added{};
  for(const auto& node:retained.nodes)
    added+=projected.clone.nodes[node.sourceNode]>=0 && projected.retainMeshBindings[node.sourceNode];
  output.nodes.reserve(retained.nodes.size()+added);
  output.nodes.assign(retained.nodes.begin(),retained.nodes.end());
  for(const auto& original:retained.nodes)
    if(projected.clone.nodes[original.sourceNode]>=0 && projected.retainMeshBindings[original.sourceNode]) {
      auto node=original;node.sourceNode=uint32_t(projected.clone.nodes[original.sourceNode]);
      output.nodes.push_back(std::move(node));
    }
  std::sort(output.nodes.begin()+retained.nodes.size(),output.nodes.end(),
      [](const auto& a,const auto& b) {return a.sourceNode<b.sourceNode;});
  output.summary={};
  for(const auto& node:output.nodes) {
    ++output.summary.instancedNodes;
    output.summary.instances=checkedAdd(output.summary.instances,node.localMatrices.size());
    output.summary.expandedPrimitives=checkedAdd(output.summary.expandedPrimitives,
        checkedBytes(node.localMatrices.size(),node.primitiveCount));
    auto bytes=checkedAdd(sizeof(NodeInstancing),checkedBytes(node.localMatrices.size(),sizeof(glm::mat4)));
    bytes=checkedAdd(bytes,checkedBytes(node.attributes.size(),sizeof(InstanceAttributeReference)));
    for(const auto& attribute:node.attributes) bytes=checkedAdd(bytes,attribute.semantic.size());
    output.summary.decodedBytes=checkedAdd(output.summary.decodedBytes,bytes);
  }
  return output;
}
} // namespace

struct SceneManager::PreparedMergeGeneration::ResourceDependencyPreparation {
  const SceneManager* live{};
  std::shared_ptr<const uint8_t> liveIdentity,targetIdentity;
  TextureResourceLifecycleGenerationPtr before,targetBefore,published;
  std::vector<MaterialResourceDependencyStatePtr> beforeDependencies,targetBeforeDependencies,dependencies;
  const AnimationPlayer* targetPlayer{};
  uint64_t epoch{},geometry{},material{},resource{},targetEpoch{},targetGeometry{},targetMaterial{},targetResource{};
  uint64_t cameraRevision{},targetCameraRevision{};
  uint64_t cameraEvaluationRevision{},targetCameraEvaluationRevision{};
  std::shared_ptr<const CameraProjectionNamespace> cameraNamespace,targetCameraNamespace;
  uint64_t materialCatalogRevision{},targetMaterialCatalogRevision{},bytes{};
  bool shared{},sourceMutation{},consumed{};
  PreparedTextureResourceLifecycle mutation;
};

SceneManager::PreparedMergeGeneration::PreparedMergeGeneration(
    PreparedMergeGeneration&&) noexcept=default;
SceneManager::PreparedMergeGeneration& SceneManager::PreparedMergeGeneration::operator=(
    PreparedMergeGeneration&&) noexcept=default;
SceneManager::PreparedMergeGeneration::~PreparedMergeGeneration()=default;

void SceneManager::PreparedMergeGeneration::releasePreparationData() noexcept {
  graph_.reset();
  sources_.reset();
  cloneSources_.reset();
  cloneOwnership_={};
  std::vector<uint32_t>{}.swap(cloneOriginalGraphNodes_);
  releaseResourceDependencyPreparation();
}

uint64_t SceneManager::PreparedMergeGeneration::resourceDependencyPreparationBytes() const noexcept {
  return resourceDependencies_?resourceDependencies_->bytes:0;
}
void SceneManager::PreparedMergeGeneration::releaseResourceDependencyPreparation() noexcept {
  if(resourceDependencies_ && resourceDependencies_->consumed) resourceDependencies_.reset();
}
void SceneManager::PreparedMergeGeneration::prepareResourceDependencies(const SceneManager& live,
    const AnimationPlayer* targetPlayer,uint64_t maxCpuBytes) try {
  if(owner_!=&live || ownerIdentity_!=live.sceneRootOwnerIdentity_ || !manager_ || !maxCpuBytes || maxCpuBytes==UINT64_MAX)
    mergeFailure("resource dependency preparation has a foreign owner or invalid budget");
  live.refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,maxCpuBytes);
  auto& target=*manager_;
  if(!live.textureResourceLifecycle_ || !target.textureResourceLifecycle_ || !live.materialResourceDependenciesValid())
    mergeFailure("live merge resource dependency authority is stale");
  Budget budget{maxCpuBytes};budget.add(sizeof(ResourceDependencyPreparation)+256);
  const auto coexistence=resourceDependencies_?resourceDependencies_->bytes:0;budget.add(coexistence);
  budget.add(checkedBytes(live.materialResourceDependencies_.size()+target.materialResourceDependencies_.size(),sizeof(MaterialResourceDependencyStatePtr)));
  auto staged=std::make_unique<ResourceDependencyPreparation>();
  staged->live=&live;staged->liveIdentity=live.sceneRootOwnerIdentity_;staged->targetIdentity=target.sceneRootOwnerIdentity_;
  staged->before=live.textureResourceLifecycle_;staged->targetBefore=target.textureResourceLifecycle_;
  staged->beforeDependencies=live.materialResourceDependencies_;staged->targetBeforeDependencies=target.materialResourceDependencies_;
  staged->targetPlayer=targetPlayer;staged->shared=staged->targetBefore->sharesAuthority(staged->before);
  if(staged->shared) {
    if(!staged->before->matches(target.textureResourceLifecycleView()))
      mergeFailure("shared clone resource payloads changed outside its history receipt");
    // Clone history changes the graph namespace, not resource incarnation
    // authority. Resource-tail replay must happen through its own receipt.
    for(const auto domain:{container::util::ResourceDomainKind::Archive,container::util::ResourceDomainKind::Evaluated,container::util::ResourceDomainKind::Runtime})
      for(const auto kind:{container::util::ResourceKind::Image,container::util::ResourceKind::Texture,container::util::ResourceKind::Sampler,container::util::ResourceKind::RuntimeTexture}) {
        const auto count=staged->before->rowCount(domain,kind);
        if(count!=staged->targetBefore->rowCount(domain,kind)) mergeFailure("shared clone resource rows require their original tail replay");
        for(uint32_t row=0;row<count;++row)
          if(staged->before->resource({domain,kind,row})!=staged->targetBefore->resource({domain,kind,row}))
            mergeFailure("shared clone resource incarnation belongs to another branch");
      }
  } else if(!target.textureResourceLifecycle_->matches(target.textureResourceLifecycleView()) || !target.materialResourceDependenciesValid())
    mergeFailure("independent inactive merge resource authority is stale");
  staged->epoch=live.generationEpoch_;staged->geometry=live.geometryRevision_;staged->material=live.materialRevision_;staged->resource=live.materialResourceRevision_;
  staged->cameraRevision=live.cameraProjectionRevision_;staged->cameraNamespace=live.cameraProjectionNamespace_;
  staged->targetCameraRevision=target.cameraProjectionRevision_;staged->targetCameraNamespace=target.cameraProjectionNamespace_;
  staged->cameraEvaluationRevision=live.cameraEvaluationRevision_;staged->targetCameraEvaluationRevision=target.cameraEvaluationRevision_;
  staged->targetEpoch=target.generationEpoch_;staged->targetGeometry=target.geometryRevision_;staged->targetMaterial=target.materialRevision_;staged->targetResource=target.materialResourceRevision_;
  staged->materialCatalogRevision=live.materialLifecycle_?live.materialLifecycle_->revision():0;
  staged->targetMaterialCatalogRevision=target.materialLifecycle_?target.materialLifecycle_->revision():0;
  // Independent catalogs retain their own valid controls while inactive. An
  // unchanged shared clone can likewise preserve its exact generation pointer.
  if(target.materialResourceDependenciesValid(targetPlayer) &&
      target.materialResourceDependencyMapMatches(target.materialResourceDependencies_,target.textureResourceLifecycle_,targetPlayer)) {
    staged->bytes=budget.bytes-coexistence;resourceDependencies_=std::move(staged);return;
  }
  TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=budget.limit-budget.bytes;
  limits.retirementPolicy=container::util::ResourceRetirementPolicy::InvalidatePreparations;
  staged->published=staged->shared?staged->before:staged->targetBefore;
  staged->mutation=ReconcileTextureResourceLifecycle(staged->published,target.textureResourceLifecycleView(),limits);
  const auto sourceBytes=staged->mutation.ownedBytes();budget.add(sourceBytes);
  const auto& outgoing=staged->shared?staged->beforeDependencies:staged->targetBeforeDependencies;
  staged->dependencies=target.stageCurrentMaterialResourceDependencies(staged->mutation,budget.limit-budget.bytes,outgoing,targetPlayer);
  budget.add(SceneManager::materialResourceDependencyMapBytes(staged->dependencies,outgoing));
  staged->mutation.seal();
  if(staged->mutation.ownedBytes()>sourceBytes) budget.add(staged->mutation.ownedBytes()-sourceBytes);
  staged->sourceMutation=true;staged->bytes=budget.bytes-coexistence;
  if(!CanExchangeTextureResourceLifecycle(staged->mutation,staged->published,target.textureResourceLifecycleView()) ||
      !target.materialResourceDependencyMapMatches(staged->dependencies,staged->mutation.next(),targetPlayer,true) ||
      !target.materialResourceDependencyMapWillBeActive(staged->dependencies,staged->mutation))
    mergeFailure("prospective merge resource dependencies became stale");
  resourceDependencies_=std::move(staged);
}
catch(const std::invalid_argument& error) {mergeFailure(error.what());}
catch(const std::length_error& error) {mergeFailure(error.what());}

bool SceneManager::PreparedMergeGeneration::resourceDependenciesReadyForExchange(const SceneManager& live,const AnimationPlayer* targetPlayer) const noexcept {
  if(!resourceDependencies_ || resourceDependencies_->consumed || owner_!=&live || ownerIdentity_!=live.sceneRootOwnerIdentity_ || !manager_) return false;
  const auto& s=*resourceDependencies_;const auto& target=*manager_;
  if(s.targetPlayer!=targetPlayer || s.live!=&live || s.liveIdentity!=live.sceneRootOwnerIdentity_ || s.targetIdentity!=target.sceneRootOwnerIdentity_ ||
      s.before!=live.textureResourceLifecycle_ || s.targetBefore!=target.textureResourceLifecycle_ ||
      s.beforeDependencies!=live.materialResourceDependencies_ || s.targetBeforeDependencies!=target.materialResourceDependencies_ ||
      s.epoch!=live.generationEpoch_ || s.geometry!=live.geometryRevision_ || s.material!=live.materialRevision_ || s.resource!=live.materialResourceRevision_ ||
      s.cameraRevision!=live.cameraProjectionRevision_ || s.targetCameraRevision!=target.cameraProjectionRevision_ ||
      s.cameraEvaluationRevision!=live.cameraEvaluationRevision_ || s.targetCameraEvaluationRevision!=target.cameraEvaluationRevision_ ||
      s.cameraNamespace!=live.cameraProjectionNamespace_ || s.targetCameraNamespace!=target.cameraProjectionNamespace_ ||
      live.cameraProjectionRevision_==UINT64_MAX || target.cameraProjectionRevision_==UINT64_MAX ||
      live.cameraEvaluationRevision_==UINT64_MAX || target.cameraEvaluationRevision_==UINT64_MAX ||
      s.targetEpoch!=target.generationEpoch_ || s.targetGeometry!=target.geometryRevision_ || s.targetMaterial!=target.materialRevision_ || s.targetResource!=target.materialResourceRevision_ ||
      s.materialCatalogRevision!=(live.materialLifecycle_?live.materialLifecycle_->revision():0) ||
      s.targetMaterialCatalogRevision!=(target.materialLifecycle_?target.materialLifecycle_->revision():0) ||
      !live.materialResourceDependenciesValid() || !s.before->matches(live.textureResourceLifecycleView())) return false;
  if(!s.sourceMutation) return target.materialResourceDependenciesValid(targetPlayer) &&
      target.materialResourceDependencyMapMatches(target.materialResourceDependencies_,target.textureResourceLifecycle_,targetPlayer);
  if(!CanExchangeTextureResourceLifecycle(s.mutation,s.published,target.textureResourceLifecycleView()) ||
      !target.materialResourceDependencyMapMatches(s.dependencies,s.mutation.next(),targetPlayer,true) ||
      !target.materialResourceDependencyMapWillBeActive(s.dependencies,s.mutation)) return false;
  return true;
}

uint64_t SceneManager::mergeHistoryResidentBytes() const {
  if(materialEntryOwners_.size()>materialManager_.materialCount())
    mergeFailure("retained material ownership table exceeds its runtime identity range");
  if(runtimeMaterialProvenance_.size()>materialManager_.materialCount() || archiveTextureResources_.color.size()!=exportSource_.textures.size() ||
      archiveTextureResources_.data.size()!=exportSource_.textures.size() || archiveTextureToEvaluated_.size()!=exportSource_.textures.size() ||
      archiveTextureIdentities_.size()!=exportSource_.textures.size())
    mergeFailure("retained material resource provenance tables disagree");
  Budget bytes{UINT64_MAX};
  bytes.add(sizeof(*this));
  bytes.add(linkedAssetStateBytes());
  retainedSourceModel(bytes,gltfModel_); retainedSourceModel(bytes,exportSource_);
  retainedSourceModel(bytes,modelExchangeScratch_);
  retainedGeometry(bytes,model_);
  if(variantOriginalModel_) retainedGeometry(bytes,*variantOriginalModel_);
  if(deformation_) retainedDeformation(bytes,*deformation_);
  if(variantOriginalDeformation_) retainedDeformation(bytes,*variantOriginalDeformation_);
  retainedVector(bytes,gltfInstancing_.nodes);
  for(const auto& node:gltfInstancing_.nodes) {
    retainedVector(bytes,node.localMatrices); retainedVector(bytes,node.attributes);
    for(const auto& attribute:node.attributes) retainedText(bytes,attribute.semantic);
  }
  retainedVector(bytes,sceneCatalog_.entries);
  for(const auto& entry:sceneCatalog_.entries) {
    retainedText(bytes,entry.name); retainedVector(bytes,entry.sourceRoots);
  }
  for(const auto* table:{&additionalSceneRoots_,&sceneRootOrder_}) {
    retainedVector(bytes,*table);
    for(const auto& roots:*table) retainedVector(bytes,roots);
  }
  // Parsed variants retain both a numeric lookup and exact source extension
  // objects. Count their duplicated metadata through its authoritative source.
  bytes.add(checkedBytes(materialVariants_.names().size(),sizeof(std::string)));
  for(const auto& name:materialVariants_.names()) retainedText(bytes,name);
  bytes.add(checkedBytes(materialVariants_.primitives().size(),
      sizeof(MaterialVariantPrimitive)+sizeof(std::optional<tinygltf::Value>)));
  for(const auto& primitive:materialVariants_.primitives()) retainedVector(bytes,primitive.mappings);
  bytes.add(checkedBytes(checkedAdd(gltfModel_.meshes.size(),1),sizeof(size_t)));
  if(const auto root=gltfModel_.extensions.find("KHR_materials_variants");root!=gltfModel_.extensions.end())
    metadataStorage(bytes,root->second,0,kHistoryMetadataDepth);
  for(const auto& mesh:gltfModel_.meshes) for(const auto& primitive:mesh.primitives)
    if(const auto extension=primitive.extensions.find("KHR_materials_variants");extension!=primitive.extensions.end())
      metadataStorage(bytes,extension->second,0,kHistoryMetadataDepth);
  bytes.add(checkedBytes(materialManager_.materialCount(),sizeof(container::material::Material)));
  retainedVector(bytes,materialLifecycleHandles_);
  if(materialLifecycle_) bytes.add(materialLifecycle_->ownedBytes());
  bytes.add(cameraProjectionNamespaceBytes());
  bytes.add(retainedOpacityMicromapBytes());
  bytes.add(materialResourceDependencyBytes());
  if(textureManager_.textureCount()>UINT32_MAX)
    mergeFailure("retained texture table exceeds its integer range");
  bytes.add(checkedBytes(textureManager_.textureCount(),container::material::TextureManager::rowStorageBytes()+64));
  for(uint32_t i=0;i<textureManager_.textureCount();++i) {
    const auto* active=textureManager_.getTexture(i);
    if(!active) continue; // Stable dead rows retain metadata, not payload/cache strings.
    const auto& texture=*active;
    retainedText(bytes,texture.name); retainedText(bytes,texture.contentKey);
    // The two lookup maps own key copies, nodes and bucket entries. Duplicate
    // content aliases may overcount, but never add shared GPU image payloads.
    bytes.add(256+4*sizeof(void*));
    retainedText(bytes,texture.name); retainedText(bytes,texture.contentKey);
  }
  retainedVector(bytes,exportMaterialSources_);
  for(const auto& material:exportMaterialSources_)
    if(material) materialStorage(bytes,*material,kHistoryMetadataDepth);
  retainedVector(bytes,exportMaterialTextureBindings_);
  bytes.add(checkedBytes(sourcePath_.native().size(),sizeof(std::filesystem::path::value_type)));
#define RETAINED_MEMBER(member) retainedVector(bytes,member)
  RETAINED_MEMBER(sourcePrimitiveToRuntime_); RETAINED_MEMBER(runtimePrimitiveToSource_);
  RETAINED_MEMBER(sourceMaterialToRuntime_); RETAINED_MEMBER(sourcePrimitiveImplicitMaterials_);
  RETAINED_MEMBER(materialEntryOwners_);
  RETAINED_MEMBER(runtimeMaterialProvenance_);RETAINED_MEMBER(archiveTextureResources_.color);RETAINED_MEMBER(archiveTextureResources_.data);
  RETAINED_MEMBER(archiveTextureToEvaluated_);RETAINED_MEMBER(archiveTextureIdentities_);RETAINED_MEMBER(materialTextureSourceRequests_);
  RETAINED_MEMBER(archiveResourcePhases_.images);RETAINED_MEMBER(archiveResourcePhases_.textures);RETAINED_MEMBER(archiveResourcePhases_.samplers);
  RETAINED_MEMBER(gltfResourcePhases_.images);RETAINED_MEMBER(gltfResourcePhases_.textures);RETAINED_MEMBER(gltfResourcePhases_.samplers);
  RETAINED_MEMBER(gltfTextureResources_.color); RETAINED_MEMBER(gltfTextureResources_.data);
  RETAINED_MEMBER(variantNormalTexCoords_); RETAINED_MEMBER(exportTextureIndices_);
  RETAINED_MEMBER(vertices_); RETAINED_MEMBER(indices_); RETAINED_MEMBER(gpuMaterials_);
  RETAINED_MEMBER(gpuMaterialTemporalRevisions_); RETAINED_MEMBER(textureMetadata_);
  RETAINED_MEMBER(textureSamplingPlan_.rows); RETAINED_MEMBER(textureSamplingPlan_.images);
  RETAINED_MEMBER(textureSamplingPlan_.metadata); RETAINED_MEMBER(textureSamplingPlan_.samplerSlots.keys);
  if(textureSamplingFallback_) {
    bytes.add(sizeof(TextureSamplingFallback)+128); // Shared payload/control, conservatively retained by this generation.
    retainedText(bytes,textureSamplingFallback_->resource.name);
    retainedText(bytes,textureSamplingFallback_->resource.contentKey);
  }
  RETAINED_MEMBER(authoredPointLights_); RETAINED_MEMBER(authoredDirectionalLights_); RETAINED_MEMBER(authoredAreaLights_);
  RETAINED_MEMBER(authoredPointBindings_); RETAINED_MEMBER(authoredDirectionalBindings_); RETAINED_MEMBER(authoredAreaBindings_);
  RETAINED_MEMBER(materialSamplers_); RETAINED_MEMBER(materialSamplerSlots_.keys);
  RETAINED_MEMBER(descriptorSets_); RETAINED_MEMBER(auxiliaryDescriptorSets_);
  if(textureImageOwnership_) bytes.add(textureImageOwnership_->ownedBytes());
  if(textureResourceLifecycle_) bytes.add(textureResourceLifecycle_->ownedBytes());
#undef RETAINED_MEMBER
  for(const auto& request:materialTextureSourceRequests_) {
    retainedText(bytes,request.key);if(request.encoded) bytes.add(request.encoded->size());
  }
  for(const auto* bindings:{&authoredPointBindings_,&authoredDirectionalBindings_,&authoredAreaBindings_})
    for(const auto& binding:*bindings) retainedText(bytes,binding.extension);
  return bytes.bytes;
}

bool SceneManager::PreparedMergeGeneration::readyForExchange(
    const SceneManager& live,const SceneGraph& graph,const AnimationPlayer* targetPlayer) const noexcept {
  return owner_==&live && ownerIdentity_==live.sceneRootOwnerIdentity_ && manager_ && graph_ &&
      (sources_ || (cloneSources_ && cloneOwnership_)) &&
      live.generationEpoch_==expectedGenerationEpoch_ &&
      live.geometryRevision_==expectedGeometryRevision_ &&
      live.materialRevision_==expectedMaterialRevision_ &&
      live.materialResourceRevision_==expectedMaterialResourceRevision_ &&
      live.sceneRootMembershipRevision_==expectedSceneRootMembershipRevision_ && live.selectedScene_==expectedSelectedScene_ &&
      graph.identity()==expectedGraphIdentity_ && graph.revision()==expectedGraphRevision_ &&
      live.generationEpoch_<UINT64_MAX && live.geometryRevision_<UINT64_MAX &&
      live.materialRevision_<UINT64_MAX && live.materialResourceRevision_<UINT64_MAX &&
      live.allocationManager_ && manager_->allocationManager_==live.allocationManager_ &&
      expectedTextureImageOwnership_.lock()==live.textureImageOwnership_ &&
      expectedTextureResourceLifecycle_.lock()==live.textureResourceLifecycle_ &&
      live.textureResourceLifecycle_ && manager_->textureResourceLifecycle_ &&
      live.textureResourceLifecycle_->matches(live.textureResourceLifecycleView()) &&
      resourceDependenciesReadyForExchange(live,targetPlayer) &&
      expectedTextureSamplingRevision_==live.textureSamplingPlan_.textureRevision &&
      expectedTextureSamplingRows_==live.textureSamplingPlan_.rows.size() &&
      live.textureSamplingStateValidForResources() && manager_->textureSamplingStateValid() &&
      container::material::TextureImageOwnershipMatches(live.textureImageOwnership_,live.textureManager_,*live.allocationManager_) &&
      container::material::TextureImageOwnershipMatches(manager_->textureImageOwnership_,manager_->textureManager_,*live.allocationManager_);
}

SceneManager::PreparedMergeGeneration SceneManager::prepareMergeGeneration(
    const GltfLoadResult& incoming,const std::filesystem::path& incomingPath,
    const GltfMergeOptions& options,const SceneGraph& baseGraph,
    const SceneMergeGenerationLimits& limits) const {
  if(!allocationManager_ || !pipelineManager_ || !deviceWrapper_ ||
      descriptorSetLayout_==VK_NULL_HANDLE || materialSamplers_.empty())
    mergeFailure("live manager is not initialized");
  if(!textureSamplingStateValidForResources() ||
      !container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    mergeFailure("live texture sampling or image ownership is stale");
  if(incomingPath.empty()) mergeFailure("incoming source path is empty");
  if(generationEpoch_==UINT64_MAX || geometryRevision_==UINT64_MAX || materialRevision_==UINT64_MAX || materialResourceRevision_==UINT64_MAX)
    mergeFailure("live generation revision is exhausted");
  if(!limits.maxPreparedCpuBytes || !limits.maxPreparedTextureBytes)
    mergeFailure("detached storage limits must be positive");
  mergeResourcePreflight([&] {
    refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes);
  });
  ValidateSourceMaterialMapping(sourceMaterialToRuntime_,gltfModel_.materials.size(),
                               materialManager_.materialCount());
  if(materialEntryOwners_.size()>materialManager_.materialCount())
    mergeFailure("live material ownership table exceeds its runtime identity range");
  validateGltfTextureResourceTable(gltfTextureResources_,gltfModel_.textures.size(),textureManager_.textureCount());
  if(!materialManager_.getMaterial(defaultMaterialIndex_))
    mergeFailure("live implicit default material is missing");

  PreparedMergeGeneration result;
  result.owner_=this;
  result.ownerIdentity_=sceneRootOwnerIdentity_;
  result.expectedGenerationEpoch_=generationEpoch_;
  result.expectedGeometryRevision_=geometryRevision_;
  result.expectedMaterialRevision_=materialRevision_;
  result.expectedMaterialResourceRevision_=materialResourceRevision_;
  result.expectedTextureImageOwnership_=textureImageOwnership_;
  result.expectedTextureResourceLifecycle_=textureResourceLifecycle_;
  result.expectedTextureSamplingRevision_=textureSamplingPlan_.textureRevision;
  result.expectedTextureSamplingRows_=textureSamplingPlan_.rows.size();
  result.expectedGraphIdentity_=baseGraph.identity();
  result.expectedGraphRevision_=baseGraph.revision();
  result.expectedSceneRootMembershipRevision_=sceneRootMembershipRevision_;result.expectedSelectedScene_=selectedScene_;
  auto sourceLimits=limits.source;
  sourceLimits.maxResidentBytes=std::min(sourceLimits.maxResidentBytes,limits.maxPreparedCpuBytes);
  sourceLimits.maxMetadataBytes=std::min(sourceLimits.maxMetadataBytes,limits.maxPreparedCpuBytes);
  result.sources_=std::make_unique<GltfSessionMergePlan>(
      PlanGltfSessionSources(gltfModel_,exportSource_,incoming.gltfModel,options,sourceLimits));
  const auto& plan=result.sources_->merge;
  if(plan.destinationScene<0 || size_t(plan.destinationScene)!=selectedScene_)
    mergeFailure("merge destination must be the currently selected scene");
  Budget cpu{limits.maxPreparedCpuBytes};
  cpu.add(sizeof(SceneManager)+sizeof(PreparedMergeGeneration)+1024);
  cpu.add(result.sources_->peakResidentBytes);
  cpu.add(result.sources_->metadataBytes);
  if(plan.model.textures.size()!=result.sources_->evaluatedSource.textures.size() ||
      plan.imported.textures.size()!=incoming.gltfModel.textures.size() ||
      plan.model.textures.size()!=checkedAdd(exportSource_.textures.size(),incoming.gltfModel.textures.size()) ||
      gltfModel_.textures.size()>exportSource_.textures.size() ||
      exportSource_.textures.size()>plan.model.textures.size())
    mergeFailure("merged source texture identity tables are inconsistent");
  // Candidate roles cover the COMPLETE live merged source, including unused
  // archive-only suffix holes. Incoming upload roles and map-validation scratch
  // coexist with it; charge all before their first allocation.
  cpu.add(checkedBytes(plan.model.textures.size(),2*sizeof(uint32_t)));
  cpu.add(checkedBytes(incoming.gltfModel.textures.size(),2*sizeof(uint32_t)+sizeof(int32_t)));
  for(const auto destination:plan.imported.textures)
    if(destination<0 || size_t(destination)<exportSource_.textures.size() ||
        size_t(destination)>=plan.model.textures.size()) mergeFailure("incoming texture translation is out of range");
  {
    auto destinations=plan.imported.textures;
    std::sort(destinations.begin(),destinations.end());
    if(std::adjacent_find(destinations.begin(),destinations.end())!=destinations.end())
      mergeFailure("incoming texture translation aliases another source binding");
  }
  // The immutable source plan stays in the receipt for animation and graph
  // staging. The candidate owns independent live and authored model copies.
  cpu.add(checkedBytes(residentBytes(plan.model),2));
  cpu.add(checkedBytes(result.sources_->metadataBytes,2));
  for(const auto& material:exportMaterialSources_)
    if(material) materialStorage(cpu,*material,limits.source.maxMetadataDepth);
  cpu.add(checkedBytes(exportMaterialTextureBindings_.size(),sizeof(decltype(exportMaterialTextureBindings_)::value_type)));
  // The dry-run table copy includes dead stable rows, even though those rows
  // own no payload strings or cache entries.
  cpu.add(checkedBytes(textureManager_.textureCount(),container::material::TextureManager::rowStorageBytes()+64));
  for(uint32_t i=0;i<textureManager_.textureCount();++i) {
    const auto* active=textureManager_.getTexture(i);
    if(!active) continue;
    const auto& texture=*active;
    // Candidate and dry-run table include independently owned key strings.
    cpu.add(checkedBytes(texture.name.size()+texture.contentKey.size()+128,6));
  }
  auto geometryLimits=limits.geometry;
  geometryLimits.maxOutputBytes=std::min(geometryLimits.maxOutputBytes,cpu.limit-cpu.bytes);
  auto geometry=ComposeGltfMergeGeometry(
      {model_,sourcePrimitiveToRuntime_,runtimePrimitiveToSource_},incoming.model,plan,geometryLimits);
  cpu.add(geometry.outputBytes);
  cpu.add(checkedBytes(geometry.sourcePrimitiveToRuntime.size(),sizeof(uint32_t))); // Retained tangent UV policy for every source primitive.
  cpu.add(streamBytes(geometry.runtime)); // SceneManager's shader upload copies.
  std::vector<Mesh> retainedBind;
  if(deformation_ || incoming.deformation) {
    cpu.add(sourceBindBytes(model_,sourcePrimitiveToRuntime_));
    retainedBind=sourceBind(model_,sourcePrimitiveToRuntime_);
  }
  auto runtimeLimits=limits.runtime;
  const auto runtimeRemaining=cpu.limit-cpu.bytes;
  runtimeLimits.maxWorkspaceBytes=std::min(runtimeLimits.maxWorkspaceBytes,runtimeRemaining/2);
  runtimeLimits.maxOutputBytes=std::min(runtimeLimits.maxOutputBytes,
      runtimeRemaining-runtimeLimits.maxWorkspaceBytes);
  auto runtime=ComposeGltfMergeRuntimeData(
      {gltfModel_,deformationData(),&gltfInstancing_,retainedBind,variantNormalTexCoords_},
      {incoming.gltfModel,incoming.deformation?&*incoming.deformation:nullptr,
       &incoming.instancing,incoming.model.meshes()},plan,runtimeLimits);
  cpu.add(runtime.outputBytes); cpu.add(runtime.workspaceBytes);
  cpu.add(graphWorkspaceBytes(baseGraph,plan.model,runtime.instancing,limits.graph));
  cpu.add(checkedBytes(incoming.gltfModel.textures.size(),
      2*sizeof(container::material::PreparedGltfTextureBinding)+
      2*sizeof(container::material::PreparedGltfTextureImage)+128));
  auto preparedTextures=container::material::PrepareGltfTextures(incoming.gltfModel,
      allocationManager_->textureDecodeOptions(false),
      std::min(limits.maxPreparedTextureBytes,cpu.limit-cpu.bytes), config_.experimentalGltfMaterials);
  for(const auto& image:preparedTextures.images)
    for(const auto& mip:image.image.mips) cpu.add(mip.bytes.size());

  bool needsIncomingDefault=false;
  for(const auto& mesh:incoming.gltfModel.meshes) for(const auto& primitive:mesh.primitives)
    needsIncomingDefault=needsIncomingDefault || primitive.material==-1;
  const auto materialCount=checkedAdd(checkedAdd(materialManager_.materialCount(),
      incoming.gltfModel.materials.size()),needsIncomingDefault?1:0);
  if(materialCount>INT32_MAX) mergeFailure("runtime material identities exceed their integer range");
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(),&properties);
  storageBudget(materialCount,sizeof(container::gpu::GpuMaterial),limits.maxMaterialBufferBytes,properties.limits);
  // Dry-run the exact content/sampler aliases before uploading. Counting every
  // role as new would spuriously reject legal deduplicated texture bindings.
  size_t prospectiveTextureCount{};
  uint64_t newTextureBytes{};
  {
    auto preview=textureManager_;
    (void)container::material::UploadPreparedGltfTextures(preparedTextures,preview,
        [&](const auto&,const container::material::TextureImageData& image) {
          if(image.mips.empty()) mergeFailure("prepared texture has no mip data");
          auto width=image.mips.front().width,height=image.mips.front().height;
          size_t level{};
          do {
            newTextureBytes=checkedAdd(newTextureBytes,
                container::material::TextureMipByteSize(image.format,width,height));
            if(!limits.maxNewTextureBytes || newTextureBytes>limits.maxNewTextureBytes)
              mergeFailure("new texture images exceed their aggregate GPU data budget");
            ++level;
            if(image.authoredMipChain && level>=image.mips.size()) break;
            if(width==1 && height==1) break;
            width=std::max(width/2,1u);height=std::max(height/2,1u);
          } while(true);
          return container::material::TextureResource{};
        });
    prospectiveTextureCount=preview.textureCount();
    if(prospectiveTextureCount>textureDescriptorCapacity_)
      mergeFailure("merged texture table exceeds the device descriptor capacity");
    // Policy-only planning accepts this dry run's not-yet-uploaded images,
    // while preserving default policy for inherited dead numeric slots.
    (void)container::material::PlanTextureSamplerSlots(preview,materialSamplerDescriptorCapacity_);
  }
  storageBudget(prospectiveTextureCount,sizeof(container::gpu::GpuTextureMetadata),
                limits.maxTextureMetadataBufferBytes,properties.limits);
  cpu.add(checkedBytes(materialCount,sizeof(container::material::Material)+
      2*sizeof(container::gpu::GpuMaterial)+sizeof(uint64_t)));
  cpu.add(materialLifecyclePreparationBytes(size_t(materialCount)));
  cpu.add(checkedBytes(materialCount,sizeof(decltype(materialEntryOwners_)::value_type)));
  cpu.add(checkedBytes(materialCount,sizeof(RuntimeMaterialProvenance)));
  cpu.add(checkedBytes(plan.model.textures.size(),3*sizeof(uint32_t)+sizeof(std::shared_ptr<const uint8_t>)+64));
  cpu.add(checkedBytes(prospectiveTextureCount,container::material::TextureManager::rowStorageBytes()+64+
      2*sizeof(container::gpu::GpuTextureMetadata)));
  cpu.add(container::material::TextureSamplingPlanStorageBytes(prospectiveTextureCount));
  cpu.add(container::material::TextureImageOwnershipStorageBytes(prospectiveTextureCount));

  result.manager_=std::make_unique<SceneManager>(*allocationManager_,*pipelineManager_,deviceWrapper_,config_);
  auto& candidate=*result.manager_;
  candidate.iesProfileCache_=iesProfileCache_;
  candidate.resetTextureAllocationsOnReset_=false;
  candidate.ownsDescriptorPool_=true;
  candidate.descriptorSetLayout_=descriptorSetLayout_; // Borrow the stable pipeline ABI.
  candidate.textureDescriptorCapacity_=textureDescriptorCapacity_;
  candidate.materialSamplerDescriptorCapacity_=materialSamplerDescriptorCapacity_;
  candidate.textureManager_=textureManager_;
  candidate.textureSamplingFallback_=textureSamplingFallback_;
  candidate.materialManager_=materialManager_;
  candidate.materialEntryOwners_.reserve(size_t(materialCount));
  candidate.materialEntryOwners_.assign(materialEntryOwners_.begin(),materialEntryOwners_.end());
  candidate.materialEntryOwners_.resize(size_t(materialCount));
  candidate.materialTailVacancy_=materialTailVacancy_;
  candidate.materialResourceOwnerIdentity_=materialResourceOwnerIdentity_;
  candidate.materialResourceVacancy_=materialResourceVacancy_;
  candidate.runtimeMaterialProvenance_=runtimeMaterialProvenance_;candidate.runtimeMaterialProvenance_.resize(size_t(materialCount));
  candidate.archiveTextureResources_=archiveTextureResources_;
  candidate.archiveTextureResources_.color.resize(plan.model.textures.size(),UINT32_MAX);
  candidate.archiveTextureResources_.data.resize(plan.model.textures.size(),UINT32_MAX);
  candidate.archiveTextureToEvaluated_=archiveTextureToEvaluated_;candidate.archiveTextureToEvaluated_.resize(plan.model.textures.size(),-1);
  candidate.archiveTextureIdentities_=archiveTextureIdentities_;candidate.archiveTextureIdentities_.resize(plan.model.textures.size());
  candidate.materialTextureSourceRequests_=materialTextureSourceRequests_;
  candidate.textureImageOwnership_=textureImageOwnership_;
  candidate.defaultMaterialIndex_=defaultMaterialIndex_;
  candidate.diagnosticMaterialIndex_=diagnosticMaterialIndex_;
  candidate.materialBaseColor_=materialBaseColor_;
  candidate.sceneClipState_=sceneClipState_;
  candidate.sourcePath_=sourcePath_; // Secondary URI resolution never uses this path.
  candidate.exportSource_=plan.model;
  candidate.gltfModel_=result.sources_->evaluatedSource;
  candidate.linkedComposition_=linkedComposition_;
  candidate.linkedAssetState_=prepareLinkedMergeOwnership(incoming,plan);
  cpu.add(candidate.linkedAssetStateBytes());
  const auto inheritPhase=[&](auto& destination,const auto& original,size_t count) {
    if(original.empty()) return;
    cpu.add(checkedBytes(std::max(count,original.size()),sizeof(uint8_t)));
    destination=original;destination.resize(count,1);
  };
  inheritPhase(candidate.archiveResourcePhases_.images,archiveResourcePhases_.images,candidate.exportSource_.images.size());
  inheritPhase(candidate.archiveResourcePhases_.textures,archiveResourcePhases_.textures,candidate.exportSource_.textures.size());
  inheritPhase(candidate.archiveResourcePhases_.samplers,archiveResourcePhases_.samplers,candidate.exportSource_.samplers.size());
  inheritPhase(candidate.gltfResourcePhases_.images,gltfResourcePhases_.images,candidate.gltfModel_.images.size());
  inheritPhase(candidate.gltfResourcePhases_.textures,gltfResourcePhases_.textures,candidate.gltfModel_.textures.size());
  inheritPhase(candidate.gltfResourcePhases_.samplers,gltfResourcePhases_.samplers,candidate.gltfModel_.samplers.size());
  const auto promotedPhase=[&](auto& destination,const auto& archive,size_t oldEvaluated,size_t oldArchive,size_t count) {
    if(archive.empty()) return;
    for(size_t row=oldEvaluated;row<oldArchive;++row) if(archive[row]==0) {
      if(destination.empty()) {cpu.add(count);destination.assign(count,1);}
      destination[row]=0;
    }
  };
  promotedPhase(candidate.gltfResourcePhases_.images,candidate.archiveResourcePhases_.images,gltfModel_.images.size(),exportSource_.images.size(),candidate.gltfModel_.images.size());
  promotedPhase(candidate.gltfResourcePhases_.textures,candidate.archiveResourcePhases_.textures,gltfModel_.textures.size(),exportSource_.textures.size(),candidate.gltfModel_.textures.size());
  promotedPhase(candidate.gltfResourcePhases_.samplers,candidate.archiveResourcePhases_.samplers,gltfModel_.samplers.size(),exportSource_.samplers.size(),candidate.gltfModel_.samplers.size());
  candidate.gltfTextureResources_.color.assign(candidate.gltfModel_.textures.size(),UINT32_MAX);
  candidate.gltfTextureResources_.data.assign(candidate.gltfModel_.textures.size(),UINT32_MAX);
  std::copy(gltfTextureResources_.color.begin(),gltfTextureResources_.color.end(),candidate.gltfTextureResources_.color.begin());
  std::copy(gltfTextureResources_.data.begin(),gltfTextureResources_.data.end(),candidate.gltfTextureResources_.data.begin());
  // SessionSources promotes the archive-only resource suffix into its shared
  // source index space. Carry explicit forward roles into that proved prefix.
  for(size_t texture=gltfModel_.textures.size();texture<exportSource_.textures.size();++texture) {
    candidate.archiveTextureToEvaluated_[texture]=candidate.sourceResourceActive({container::util::ResourceDomainKind::Archive,
        container::util::ResourceKind::Texture,uint32_t(texture)})?int32_t(texture):-1;
    candidate.gltfTextureResources_.color[texture]=archiveTextureResources_.color[texture];
    candidate.gltfTextureResources_.data[texture]=archiveTextureResources_.data[texture];
    if(!candidate.archiveTextureIdentities_[texture]) candidate.archiveTextureIdentities_[texture]=std::make_shared<const uint8_t>(0);
  }
  candidate.model_=std::move(geometry.runtime);
  candidate.sourcePrimitiveToRuntime_=std::move(geometry.sourcePrimitiveToRuntime);
  candidate.runtimePrimitiveToSource_=std::move(geometry.runtimePrimitiveToSource);
  candidate.deformation_=std::move(runtime.deformation);
  candidate.gltfInstancing_=std::move(runtime.instancing);
  candidate.materialVariants_=ParseMaterialVariants(candidate.gltfModel_);
  candidate.selectedMaterialVariant_=selectedMaterialVariant_;
  candidate.sourceMaterialToRuntime_=sourceMaterialToRuntime_;
  candidate.sourceMaterialToRuntime_.resize(candidate.gltfModel_.materials.size(),UINT32_MAX);
  candidate.sourcePrimitiveImplicitMaterials_=sourcePrimitiveImplicitMaterials_;
  if(candidate.sourcePrimitiveImplicitMaterials_.empty())
    candidate.sourcePrimitiveImplicitMaterials_.assign(sourcePrimitiveToRuntime_.size(),defaultMaterialIndex_);
  if(candidate.sourcePrimitiveImplicitMaterials_.size()!=sourcePrimitiveToRuntime_.size())
    mergeFailure("retained primitive implicit material table is incomplete");
  for(const auto fallback:candidate.sourcePrimitiveImplicitMaterials_)
    if(!candidate.materialManager_.getMaterial(fallback))
      mergeFailure("retained primitive implicit material is missing");
  uint32_t incomingDefault=defaultMaterialIndex_;
  if(needsIncomingDefault) {
    container::material::Material implicit;
    implicit.metallicFactor=1.0f; // glTF's implicit material defaults.
    incomingDefault=candidate.materialManager_.createMaterial(implicit);
    const auto slots=snapshotMaterialTextureBindings(implicit).runtimeIndices;
    if(std::ranges::any_of(slots,[](uint32_t texture){return texture!=UINT32_MAX;}))
      mergeFailure("implicit material has an unexpected texture binding");
    candidate.runtimeMaterialProvenance_[incomingDefault]={true,{}};
  }
  candidate.sourcePrimitiveImplicitMaterials_.resize(candidate.sourcePrimitiveToRuntime_.size(),incomingDefault);
  candidate.exportMaterialSources_=exportMaterialSources_;
  candidate.exportMaterialTextureBindings_=exportMaterialTextureBindings_;
  candidate.exportTextureIndices_=exportTextureIndices_;

  const auto firstMaterial=candidate.materialManager_.materialCount();
  const auto incomingMaterials=MakeContiguousSourceMaterialMapping(firstMaterial,
      incoming.gltfModel.materials.size(),materialCount);
  for(size_t i=0;i<incomingMaterials.size();++i) {
    const auto source=plan.imported.materials.at(i);
    if(source<0 || size_t(source)>=candidate.sourceMaterialToRuntime_.size())
      mergeFailure("incoming material translation is invalid");
    candidate.sourceMaterialToRuntime_[size_t(source)]=incomingMaterials[i];
  }
  ValidateSourceMaterialMapping(candidate.sourceMaterialToRuntime_,candidate.gltfModel_.materials.size(),materialCount);
  std::vector<uint32_t> materials;
  materials.reserve(candidate.sourcePrimitiveToRuntime_.size());
  size_t sourceFlat{};
  for(const auto& mesh:candidate.gltfModel_.meshes) for(const auto& primitive:mesh.primitives)
    materials.push_back(ResolveSourceMaterialIndex(primitive.material,candidate.sourceMaterialToRuntime_,
        candidate.gltfModel_.materials.size(),materialCount,
        candidate.sourcePrimitiveImplicitMaterials_.at(sourceFlat++)));
  auto graphLimits=limits.graph;
  graphLimits.maxActiveRenderableObjects=std::min<uint64_t>(graphLimits.maxActiveRenderableObjects,config_.maxSceneObjects);
  graphLimits.hierarchy.primitiveRangeCount=candidate.model_.meshes().size();
  graphLimits.hierarchy.runtimeMaterialCount=materialCount;
  const auto scale=glm::scale(glm::mat4(1),glm::vec3(importScale()));
  auto assembled=BuildGltfMergeGraphCandidate(baseGraph,plan,gltfModel_.nodes.size(),
      materials,candidate.sourcePrimitiveToRuntime_,candidate.gltfInstancing_,scale,graphLimits);
  candidate.sceneCatalog_=BuildGltfSceneCatalog(candidate.gltfModel_);
  candidate.selectedScene_=size_t(plan.destinationScene);
  // Selection and graph placement express the active view. Preserve each
  // source table's exact optional default, including -1 and an archive default
  // differing from the currently selected live scene.
  candidate.additionalSceneRoots_.resize(candidate.sceneCatalog_.entries.size());
  candidate.sceneRootOrder_.resize(candidate.sceneCatalog_.entries.size());
  for(size_t scene=0;scene<sceneCatalog_.entries.size();++scene) {
    candidate.additionalSceneRoots_[scene]=refinedRoots(additionalSceneRoots_.at(scene),baseGraph);
    candidate.sceneRootOrder_[scene]=refinedRoots(
        planSceneSelection(baseGraph,scene,limits.graph.hierarchy.maxRenderableObjects).runtimeRoots,baseGraph);
  }
  candidate.sceneRootOrder_[candidate.selectedScene_]=*assembled.graph.activeRoots();
  candidate.sceneRootsDirty_=false;
  candidate.mergedAssets_=true;
  if(candidate.materialVariants_.hasVariants()) {
    cpu.add(geometry.outputBytes);cpu.add(runtime.outputBytes);
    candidate.variantOriginalModel_=candidate.model_;
    candidate.variantOriginalDeformation_=candidate.deformation_;
  }
  candidate.vertices_=candidate.model_.vertices();
  candidate.indices_=candidate.model_.indices();
  candidate.indexType_=indexType_;
  candidate.refreshActiveScene(assembled.graph);

  // All uploads are isolated from the active texture manager. The batch rolls
  // back its own images on any subsequent material/graph/buffer failure.
  auto batch=allocationManager_->beginTextureAllocationBatch();
  auto textureResources=container::material::UploadPreparedGltfTextures(
      preparedTextures,candidate.textureManager_,[&](const auto& name,const auto& image) {
        return allocationManager_->createTextureFromImageData(name,image,materialQueueFamilies());
      });
  validateGltfTextureResourceTable(textureResources,incoming.gltfModel.textures.size(),candidate.textureManager_.textureCount());
  for(size_t texture=0;texture<plan.imported.textures.size();++texture) {
    const auto destination=size_t(plan.imported.textures[texture]);
    candidate.gltfTextureResources_.color[destination]=textureResources.color[texture];
    candidate.gltfTextureResources_.data[destination]=textureResources.data[texture];
    candidate.archiveTextureResources_.color[destination]=textureResources.color[texture];
    candidate.archiveTextureResources_.data[destination]=textureResources.data[texture];
    candidate.archiveTextureToEvaluated_[destination]=int32_t(destination);
    candidate.archiveTextureIdentities_[destination]=std::make_shared<const uint8_t>(0);
  }
  validateGltfTextureResourceTable(candidate.gltfTextureResources_,candidate.gltfModel_.textures.size(),candidate.textureManager_.textureCount());
  auto ignoredDefault=candidate.defaultMaterialIndex_;
  candidate.materialXBridge_.loadMaterialsForGltfWithTextureRoles(incoming.gltfModel,
      textureResources,candidate.materialManager_,ignoredDefault,config_.experimentalGltfMaterials);
  if(candidate.materialManager_.materialCount()!=materialCount)
    mergeFailure("incoming material import did not append one record per source definition");
  candidate.exportMaterialSources_.resize(materialCount);
  candidate.exportMaterialTextureBindings_.resize(materialCount);
  for(size_t i=0;i<incomingMaterials.size();++i) {
    const auto source=plan.imported.materials.at(i);
    const auto target=incomingMaterials[i];
    // Already remapped against the one authoritative merged resource archive.
    // Calling rememberExportMaterials here would duplicate all source tables.
    candidate.exportMaterialSources_[target]=plan.model.materials[size_t(source)];
    candidate.exportMaterialTextureBindings_[target]=snapshotMaterialTextureBindings(
        *candidate.materialManager_.getMaterial(target));
    candidate.rememberRuntimeMaterialProvenance(target,*candidate.exportMaterialSources_[target],candidate.gltfModel_.materials[size_t(source)]);
  }
  candidate.exportTextureIndices_.resize(candidate.textureManager_.textureCount(),-1);
  for(const auto* role:{&textureResources.color,&textureResources.data})
    for(size_t i=0;i<role->size();++i) {
      const auto resource=(*role)[i];
      if(resource==UINT32_MAX) continue;
      if(resource>=candidate.exportTextureIndices_.size() || i>=plan.imported.textures.size())
        mergeFailure("incoming texture resource translation is invalid");
      // Slot-specific provenance retains equal-pixel source texture metadata.
      // The fallback identity is used only for subsequent changed bindings.
      if(candidate.exportTextureIndices_[resource]<0)
        candidate.exportTextureIndices_[resource]=plan.imported.textures[i];
    }
  candidate.variantNormalTexCoords_.reserve(candidate.sourcePrimitiveToRuntime_.size());
  size_t flat{};
  for(const auto& mesh:candidate.gltfModel_.meshes) for(const auto& primitive:mesh.primitives) {
    const auto material=candidate.resolvePrimitiveMaterialIndex(flat,primitive.material);
    uint32_t uv{};
    if(candidate.deformation_) uv=candidate.deformation_->primitives.at(flat).normalTextureTexCoord;
    else if(flat<variantNormalTexCoords_.size()) uv=variantNormalTexCoords_[flat];
    else {
      const auto* value=candidate.materialManager_.getMaterial(material);
      if(value->normalTextureIndex!=UINT32_MAX) uv=value->normalTextureTransform.texCoord;
    }
    if(uv>1) mergeFailure("generated frame normal texture UV must be zero or one");
    candidate.variantNormalTexCoords_.push_back(uv); ++flat;
  }
  candidate.createSampler();
  auto retiredMaterials=materialRetirementMask();
  retiredMaterials.resize(candidate.materialManager_.materialCount(),0);
  candidate.resetMaterialLifecycle(retiredMaterials);
  cpu.add(checkedBytes(candidate.gltfModel_.cameras.size(),128)+256);
  candidate.resetCameraProjectionNamespace();
  candidate.uploadMaterialBuffer();
  // Descriptor validation borrows only committed allocator capabilities.
  // Keep the aggregate rollback owner while acquiring every candidate image
  // pin. Inherited aliases reuse their original pins; the aggregate is released
  // outside publication after all new batch members have independent owners.
  auto incomingImages=batch.commit();
  const std::array inheritedImages{textureImageOwnership_};
  candidate.textureImageOwnership_=container::material::PrepareTextureImageOwnership(
      candidate.textureManager_,*allocationManager_,inheritedImages,limits.maxPreparedCpuBytes);
  incomingImages.reset();
  candidate.uploadTextureMetadataBuffer(cpu.limit-cpu.bytes);
  cpu.add(candidate.textureResourceLifecycle_->ownedBytes());
  cpu.add(candidate.materialResourceDependencyBytes());
  candidate.createSceneClipStateBuffer();
  if(descriptorSets_.size()>UINT32_MAX/2) mergeFailure("frame descriptor count exceeds its integer range");
  candidate.allocateDescriptorSets(static_cast<uint32_t>(descriptorSets_.size()));
  candidate.geometryRevision_=geometryRevision_+1;
  candidate.materialRevision_=materialRevision_+1;
  candidate.materialResourceRevision_=materialResourceRevision_+1;
  candidate.generationEpoch_=generationEpoch_+1;
  result.graph_=std::make_unique<SceneGraph>(std::move(assembled.graph));
  result.prepareResourceDependencies(*this,nullptr,cpu.limit-cpu.bytes);
  cpu.add(result.resourceDependencyPreparationBytes());
  if(!result.readyForExchange(*this,baseGraph))
    mergeFailure("live source changed during detached preparation");
  return result;
}

SceneManager::PreparedMergeGeneration SceneManager::prepareSubtreeCloneGeneration(
    const SceneGraph& baseGraph,uint32_t rootGraphId,std::optional<std::string> name,
    const SceneMergeGenerationLimits& limits) const {
  if(!allocationManager_ || !pipelineManager_ || !deviceWrapper_ ||
      descriptorSetLayout_==VK_NULL_HANDLE || materialSamplers_.empty())
    mergeFailure("live manager is not initialized for subtree duplication");
  if(!textureSamplingStateValidForResources() ||
      !container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    mergeFailure("live texture sampling or image ownership is stale for subtree duplication");
  if(generationEpoch_==UINT64_MAX || geometryRevision_==UINT64_MAX || materialRevision_==UINT64_MAX || materialResourceRevision_==UINT64_MAX)
    mergeFailure("live generation revision is exhausted");
  if(!limits.maxPreparedCpuBytes || selectedScene_>=sceneCatalog_.entries.size() ||
      sceneCatalog_.entries.size()>limits.source.maxScenes ||
      additionalSceneRoots_.size()!=sceneCatalog_.entries.size())
    mergeFailure("clone preparation budget or scene membership is invalid");
  mergeResourcePreflight([&] {
    refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes);
  });
  ValidateSourceMaterialMapping(sourceMaterialToRuntime_,gltfModel_.materials.size(),materialManager_.materialCount());
  if(materialEntryOwners_.size()>materialManager_.materialCount())
    mergeFailure("live material ownership table exceeds its runtime identity range");
  validateGltfTextureResourceTable(gltfTextureResources_,gltfModel_.textures.size(),textureManager_.textureCount());
  uint64_t primitiveCount{};
  for(const auto& mesh:gltfModel_.meshes) primitiveCount=checkedAdd(primitiveCount,mesh.primitives.size());
  if(sourcePrimitiveToRuntime_.size()!=primitiveCount || runtimePrimitiveToSource_.size()!=model_.meshes().size())
    mergeFailure("source/runtime primitive maps are incomplete");
  for(size_t source=0;source<sourcePrimitiveToRuntime_.size();++source) {
    const auto runtime=sourcePrimitiveToRuntime_[source];
    if(runtime>=runtimePrimitiveToSource_.size() || runtimePrimitiveToSource_[runtime]!=int64_t(source))
      mergeFailure("source/runtime primitive maps disagree");
  }
  for(size_t runtime=0;runtime<runtimePrimitiveToSource_.size();++runtime) {
    const auto source=runtimePrimitiveToSource_[runtime];
    if(source < -1 || (source>=0 && (size_t(source)>=sourcePrimitiveToRuntime_.size() ||
        sourcePrimitiveToRuntime_[size_t(source)]!=runtime))) mergeFailure("runtime/source primitive map is invalid");
  }
  if(!materialManager_.getMaterial(defaultMaterialIndex_)) mergeFailure("live default material is unavailable");
  if(model_.meshes().size()>limits.geometry.maxPrimitives || model_.vertices().size()>limits.geometry.maxVertices ||
      model_.indices().size()>limits.geometry.maxIndices) mergeFailure("retained clone geometry exceeds its table budget");
  Budget geometryBytes{limits.geometry.maxOutputBytes};retainedGeometry(geometryBytes,model_);

  PreparedMergeGeneration result;result.owner_=this;result.ownerIdentity_=sceneRootOwnerIdentity_;
  result.expectedGenerationEpoch_=generationEpoch_;result.expectedGeometryRevision_=geometryRevision_;
  result.expectedMaterialRevision_=materialRevision_;result.expectedGraphIdentity_=baseGraph.identity();
  result.expectedMaterialResourceRevision_=materialResourceRevision_;
  result.expectedTextureImageOwnership_=textureImageOwnership_;
  result.expectedTextureResourceLifecycle_=textureResourceLifecycle_;
  result.expectedTextureSamplingRevision_=textureSamplingPlan_.textureRevision;
  result.expectedTextureSamplingRows_=textureSamplingPlan_.rows.size();
  result.expectedGraphRevision_=baseGraph.revision();
  result.expectedSceneRootMembershipRevision_=sceneRootMembershipRevision_;result.expectedSelectedScene_=selectedScene_;
  Budget cpu{limits.maxPreparedCpuBytes};
  // Reserve the complete retained asset shell before its first copy. This
  // includes current variant frames, old authored defaults and every export
  // provenance cache; source-plan growth is charged separately below.
  cpu.add(checkedBytes(mergeHistoryResidentBytes(),2));
  uint64_t rootReferences{};
  for(size_t scene=0;scene<sceneCatalog_.entries.size();++scene) {
    rootReferences=checkedAdd(rootReferences,sceneCatalog_.entries[scene].sourceRoots.size());
    rootReferences=checkedAdd(rootReferences,additionalSceneRoots_[scene].size());
  }
  if(!sceneCatalog_.authoredScenes && !sceneCatalog_.sourceNodeCount)
    rootReferences=checkedAdd(rootReferences,baseGraph.rootNodes().size());
  if(rootReferences>limits.source.maxRootReferences) mergeFailure("retained clone scene roots exceed their budget");
  cpu.add(checkedBytes(rootReferences,16));cpu.add(checkedBytes(sceneCatalog_.entries.size(),96));
  cpu.add(checkedBytes(baseGraph.nodeCount(),32)); // Scene selection maps/temporary traversal.
  std::vector<std::vector<uint32_t>> roots;roots.reserve(sceneCatalog_.entries.size());
  for(size_t scene=0;scene<sceneCatalog_.entries.size();++scene)
    roots.push_back(planSceneSelection(baseGraph,scene,limits.graph.hierarchy.maxRenderableObjects).runtimeRoots);

  GraphCloneProjectionLimits graphLimits;graphLimits.graph=limits.graph;
  graphLimits.graph.maxActiveRenderableObjects=std::min<uint64_t>(limits.graph.maxActiveRenderableObjects,config_.maxSceneObjects);
  graphLimits.graph.hierarchy.primitiveRangeCount=model_.meshes().size();
  graphLimits.graph.hierarchy.runtimeMaterialCount=materialManager_.materialCount();
  graphLimits.maxScenes=limits.source.maxScenes;graphLimits.maxReferences=limits.source.maxReferences;
  graphLimits.maxSceneRootReferences=limits.source.maxRootReferences;
  graphLimits.maxWorkspaceBytes=std::min(graphLimits.maxWorkspaceBytes,(cpu.limit-cpu.bytes)/2);
  graphLimits.maxOutputBytes=std::min(graphLimits.maxOutputBytes,(cpu.limit-cpu.bytes)/2);
  auto graph=BuildGraphCloneProjection({baseGraph,gltfModel_,rootGraphId,roots,selectedScene_,
      model_.meshes().size(),materialManager_.materialCount(),sourcePrimitiveToRuntime_,std::move(name)},graphLimits);
  cpu.add(graph.outputBytes);cpu.add(graph.workspaceBytes);
  auto sourceLimits=limits.source;const auto remaining=cpu.limit-cpu.bytes;
  sourceLimits.maxResidentBytes=std::min(sourceLimits.maxResidentBytes,remaining/2);
  sourceLimits.maxMetadataBytes=std::min(sourceLimits.maxMetadataBytes,remaining-remaining/2);
  result.cloneSources_=std::make_unique<GltfProjectedSubtreeClonePlan>(
      PlanGltfProjectedSubtreeClone(gltfModel_,exportSource_,graph.sourceProjection,{},sourceLimits));
  const auto& projected=*result.cloneSources_;const auto& plan=projected.clone;
  if(plan.authoredSource.textures.size()!=exportSource_.textures.size() ||
      plan.evaluatedSource.textures.size()!=gltfModel_.textures.size())
    mergeFailure("subtree clone changed source texture identities");
  graph.ownership.validate(gltfModel_,projected);
  cpu.add(plan.peakResidentBytes);cpu.add(plan.metadataBytes);
  Budget oldSources{UINT64_MAX},newSources{UINT64_MAX};
  retainedSourceModel(oldSources,gltfModel_);retainedSourceModel(oldSources,exportSource_);
  retainedSourceModel(newSources,plan.authoredSource);retainedSourceModel(newSources,plan.evaluatedSource);
  if(newSources.bytes>oldSources.bytes) cpu.add(newSources.bytes-oldSources.bytes);
  if(deformation_) cpu.add(cloneDeformationGrowth(*deformation_,gltfModel_,projected,limits.runtime));
  else if(plan.clonedSkins) mergeFailure("cloned skin has no retained deformation definitions");
  if(variantOriginalDeformation_)
    cpu.add(cloneDeformationGrowth(*variantOriginalDeformation_,gltfModel_,projected,limits.runtime));
  cpu.add(cloneInstancingGrowth(gltfInstancing_,gltfModel_,projected,limits.runtime));
  // New scene catalog/root tables and source-directed light associations may
  // coexist with all old vectors while refreshing the detached generation.
  cpu.add(checkedBytes(plan.clonedNodes,8*(sizeof(AuthoredLightBinding)+sizeof(container::gpu::PointLightData)+
      sizeof(container::gpu::AreaLightData)+sizeof(AuthoredDirectionalLight)+64)));
  cpu.add(checkedBytes(checkedAdd(plan.authoredSource.nodes.size(),baseGraph.nodeCount()),16));
  cpu.add(checkedBytes(plan.authoredSource.scenes.size(),sizeof(GltfSceneEntry)));
  for(const auto& scene:plan.authoredSource.scenes) {
    cpu.add(scene.name.size());cpu.add(checkedBytes(scene.nodes.size(),sizeof(int32_t)));
  }

  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(),&properties);
  storageBudget(materialManager_.materialCount(),sizeof(container::gpu::GpuMaterial),limits.maxMaterialBufferBytes,properties.limits);
  storageBudget(textureManager_.textureCount(),sizeof(container::gpu::GpuTextureMetadata),limits.maxTextureMetadataBufferBytes,properties.limits);
  if(textureManager_.textureCount()>textureDescriptorCapacity_ || descriptorSets_.empty() || descriptorSets_.size()>64)
    mergeFailure("retained texture/frame descriptor capacity is invalid");
  cpu.add(checkedBytes(materialManager_.materialCount(),2*sizeof(container::gpu::GpuMaterial)+sizeof(uint64_t)));
  cpu.add(checkedBytes(materialManager_.materialCount(),sizeof(container::util::ResourceHandle)));
  cpu.add(checkedBytes(materialManager_.materialCount(),sizeof(decltype(materialEntryOwners_)::value_type)));
  cpu.add(checkedBytes(textureManager_.textureCount(),2*sizeof(container::gpu::GpuTextureMetadata)));
  cpu.add(container::material::TextureSamplingPlanStorageBytes(textureManager_.textureCount()));
  cpu.add(container::material::TextureImageOwnershipStorageBytes(textureManager_.textureCount()));
  result.manager_=std::make_unique<SceneManager>(*allocationManager_,*pipelineManager_,deviceWrapper_,config_);
  auto& candidate=*result.manager_;
  candidate.iesProfileCache_=iesProfileCache_;
  candidate.resetTextureAllocationsOnReset_=false;candidate.ownsDescriptorPool_=true;
  candidate.descriptorSetLayout_=descriptorSetLayout_;
  candidate.textureDescriptorCapacity_=textureDescriptorCapacity_;
  candidate.materialSamplerDescriptorCapacity_=materialSamplerDescriptorCapacity_;
  candidate.textureManager_=textureManager_;candidate.materialManager_=materialManager_;
  candidate.textureSamplingFallback_=textureSamplingFallback_;
  candidate.textureResourceLifecycle_=textureResourceLifecycle_;
  cpu.add(checkedBytes(materialResourceDependencies_.size(),sizeof(MaterialResourceDependencyStatePtr)));
  candidate.materialResourceDependencies_=materialResourceDependencies_;
  // Clone preserves the exact material table and incarnations. Its new node
  // namespace does not create a second material identity domain. Linear history
  // reverses later material actions before restoring this clone payload.
  candidate.materialLifecycle_=materialLifecycle_;candidate.materialLifecycleDomain_=materialLifecycleDomain_;
  candidate.materialLifecycleHandles_=materialLifecycleHandles_;candidate.materialLifecycleNextSlot_=materialLifecycleNextSlot_;
  candidate.materialEntryOwners_.reserve(materialManager_.materialCount());
  candidate.materialEntryOwners_.assign(materialEntryOwners_.begin(),materialEntryOwners_.end());
  candidate.materialEntryOwners_.resize(materialManager_.materialCount());
  candidate.materialTailVacancy_=materialTailVacancy_;
  candidate.textureImageOwnership_=textureImageOwnership_;candidate.model_=model_;
  candidate.sourcePrimitiveToRuntime_=sourcePrimitiveToRuntime_;candidate.runtimePrimitiveToSource_=runtimePrimitiveToSource_;
  candidate.gltfModel_=plan.evaluatedSource;candidate.exportSource_=plan.authoredSource;candidate.sourcePath_=sourcePath_;
  candidate.linkedComposition_=linkedComposition_;
  candidate.linkedAssetState_=linkedAssetState_;
  candidate.inheritCameraProjectionNamespace(*this);cpu.add(candidate.cameraProjectionNamespaceBytes());
  candidate.archiveResourcePhases_=archiveResourcePhases_;candidate.gltfResourcePhases_=gltfResourcePhases_;
  candidate.gltfTextureResources_=gltfTextureResources_;
  candidate.materialResourceOwnerIdentity_=materialResourceOwnerIdentity_;
  candidate.materialResourceVacancy_=materialResourceVacancy_;
  candidate.runtimeMaterialProvenance_=runtimeMaterialProvenance_;
  candidate.archiveTextureResources_=archiveTextureResources_;candidate.archiveTextureToEvaluated_=archiveTextureToEvaluated_;
  candidate.archiveTextureIdentities_=archiveTextureIdentities_;candidate.materialTextureSourceRequests_=materialTextureSourceRequests_;
  candidate.gltfInstancing_=cloneInstancing(gltfInstancing_,projected);
  if(deformation_) candidate.deformation_=cloneDeformation(*deformation_,projected);
  candidate.materialVariants_=materialVariants_;candidate.selectedMaterialVariant_=selectedMaterialVariant_;
  candidate.variantOriginalModel_=variantOriginalModel_;
  if(variantOriginalDeformation_) candidate.variantOriginalDeformation_=cloneDeformation(*variantOriginalDeformation_,projected);
  candidate.variantNormalTexCoords_=variantNormalTexCoords_;
  candidate.sourceMaterialToRuntime_=sourceMaterialToRuntime_;
  candidate.sourcePrimitiveImplicitMaterials_=sourcePrimitiveImplicitMaterials_;
  candidate.exportMaterialSources_=exportMaterialSources_;
  candidate.exportMaterialTextureBindings_=exportMaterialTextureBindings_;
  candidate.exportTextureIndices_=exportTextureIndices_;
  candidate.vertices_=vertices_;candidate.indices_=indices_;candidate.indexType_=indexType_;
  candidate.materialBaseColor_=materialBaseColor_;candidate.defaultMaterialIndex_=defaultMaterialIndex_;
  candidate.diagnosticMaterialIndex_=diagnosticMaterialIndex_;candidate.sceneClipState_=sceneClipState_;
  candidate.sceneCatalog_=BuildGltfSceneCatalog(candidate.gltfModel_,
      {.maxScenes=limits.source.maxScenes,.maxRootReferences=limits.source.maxRootReferences});
  candidate.selectedScene_=selectedScene_;candidate.additionalSceneRoots_=additionalSceneRoots_;
  for(size_t scene=0;scene<candidate.additionalSceneRoots_.size();++scene) {
    auto& additions=candidate.additionalSceneRoots_[scene];
    std::erase_if(additions,[&](uint32_t node) {return !baseGraph.getNode(node);});
    additions.insert(additions.end(),graph.addedSceneRoots[scene].begin(),graph.addedSceneRoots[scene].end());
  }
  candidate.sceneRootOrder_=graph.sceneRoots;candidate.sceneRootsDirty_=false;candidate.mergedAssets_=true;
  for(size_t scene=0;scene<graph.sceneRoots.size();++scene)
    if(candidate.planSceneSelection(graph.graph,scene,limits.graph.hierarchy.maxRenderableObjects).runtimeRoots!=graph.sceneRoots[scene])
      mergeFailure("projected runtime scene membership differs from candidate catalog");
  candidate.refreshActiveScene(graph.graph);
  // No texture allocation batch, image preparation, geometry decoder or file
  // access belongs to this path. All image handles retain their existing lease.
  candidate.createSampler();candidate.uploadMaterialBuffer();candidate.uploadTextureMetadataBuffer();
  candidate.createSceneClipStateBuffer();candidate.allocateDescriptorSets(uint32_t(descriptorSets_.size()));
  candidate.geometryRevision_=geometryRevision_+1;candidate.materialRevision_=materialRevision_+1;
  candidate.materialResourceRevision_=materialResourceRevision_+1;
  candidate.generationEpoch_=generationEpoch_+1;
  result.cloneRoot_=graph.root;result.cloneOwnership_=std::move(graph.ownership);
  result.cloneOriginalGraphNodes_=std::move(graph.newToOldGraph);
  result.graph_=std::make_unique<SceneGraph>(std::move(graph.graph));
  result.prepareResourceDependencies(*this,nullptr,cpu.limit-cpu.bytes);
  cpu.add(result.resourceDependencyPreparationBytes());
  if(!result.readyForExchange(*this,baseGraph)) mergeFailure("live source changed during detached clone preparation");
  return result;
}

void SceneManager::PreparedMergeGeneration::prepareDescriptorFrames(uint32_t imageCount) {
  if(!manager_ || imageCount==0 || imageCount>64)
    throw std::invalid_argument("Invalid inactive scene descriptor frame count");
  if(manager_->descriptorSets_.size()!=imageCount ||
      manager_->auxiliaryDescriptorSets_.size()!=imageCount)
    manager_->allocateDescriptorSets(imageCount);
}

void SceneManager::exchangeMergeGeneration(PreparedMergeGeneration& prepared,const AnimationPlayer* targetPlayer) noexcept {
  assert(prepared.owner_==this && prepared.ownerIdentity_==sceneRootOwnerIdentity_ && prepared.manager_);
  auto& other=*prepared.manager_;
  assert(other.allocationManager_==allocationManager_ &&
         other.deviceWrapper_==deviceWrapper_ && other.descriptorSetLayout_==descriptorSetLayout_);
  assert(generationEpoch_<UINT64_MAX && geometryRevision_<UINT64_MAX && materialRevision_<UINT64_MAX);
  assert(materialResourceRevision_<UINT64_MAX);
  if(!prepared.resourceDependenciesReadyForExchange(*this,targetPlayer)) return;
  auto& dependencies=*prepared.resourceDependencies_;
  if(dependencies.sourceMutation) {
    const bool sourcePublished=ExchangeTextureResourceLifecycle(dependencies.mutation,dependencies.published,other.textureResourceLifecycleView());
    assert(sourcePublished);if(!sourcePublished) return;
    other.textureResourceLifecycle_.swap(dependencies.published);
    other.materialResourceDependencies_.swap(dependencies.dependencies);
  }
  dependencies.consumed=true;
  const auto nextEpoch=generationEpoch_+1;
  const auto nextGeometry=geometryRevision_+1;
  const auto nextMaterial=materialRevision_+1;
  const auto nextResource=materialResourceRevision_+1;
  const auto nextCamera=cameraProjectionRevision_+1,nextOtherCamera=other.cameraProjectionRevision_+1;
  const auto nextEvaluatedCamera=cameraEvaluationRevision_+1,nextOtherEvaluatedCamera=other.cameraEvaluationRevision_+1;
  const bool sharedMaterialLifecycle=materialLifecycle_ && materialLifecycle_==other.materialLifecycle_;
  const auto lifecycleHighWater=std::max(materialLifecycleNextSlot_,other.materialLifecycleNextSlot_);
  const auto swapSourceModel=[&](tinygltf::Model& left,tinygltf::Model& right) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<tinygltf::Model>);
    // All records, including future TinyGLTF fields, travel through move
    // assignments. The scratch's map sentinels were allocated by the manager's
    // fallible constructor, never inside the no-allocation publication.
    modelExchangeScratch_=std::move(left);
    left=std::move(right);
    right=std::move(modelExchangeScratch_);
    modelExchangeScratch_.asset.version.clear();
  };
  using std::swap;
#define EXCHANGE_ASSET(field) static_assert(std::is_nothrow_swappable_v<decltype(field)>); swap(field,other.field)
  EXCHANGE_ASSET(textureManager_); EXCHANGE_ASSET(materialManager_);
  EXCHANGE_ASSET(materialLifecycle_); EXCHANGE_ASSET(materialLifecycleDomain_);
  EXCHANGE_ASSET(materialLifecycleHandles_); EXCHANGE_ASSET(materialLifecycleNextSlot_);
  EXCHANGE_ASSET(materialResourceDependencies_);
  EXCHANGE_ASSET(materialEntryOwners_); EXCHANGE_ASSET(materialTailVacancy_);
  EXCHANGE_ASSET(cameraProjectionNamespace_);
  EXCHANGE_ASSET(model_); EXCHANGE_ASSET(sourcePrimitiveToRuntime_); EXCHANGE_ASSET(runtimePrimitiveToSource_);
  EXCHANGE_ASSET(opacityMicromapState_); EXCHANGE_ASSET(opacityMicromapGeometryRevision_);
  EXCHANGE_ASSET(linkedComposition_); EXCHANGE_ASSET(linkedSceneBaseline_); EXCHANGE_ASSET(linkedAssetState_);
  swapSourceModel(gltfModel_,other.gltfModel_); EXCHANGE_ASSET(gltfInstancing_);
  EXCHANGE_ASSET(gltfTextureResources_);
  EXCHANGE_ASSET(archiveResourcePhases_);EXCHANGE_ASSET(gltfResourcePhases_);
  EXCHANGE_ASSET(materialResourceOwnerIdentity_);EXCHANGE_ASSET(runtimeMaterialProvenance_);EXCHANGE_ASSET(archiveTextureResources_);
  EXCHANGE_ASSET(materialResourceVacancy_);
  EXCHANGE_ASSET(archiveTextureToEvaluated_);EXCHANGE_ASSET(archiveTextureIdentities_);EXCHANGE_ASSET(materialTextureSourceRequests_);
  EXCHANGE_ASSET(sceneCatalog_); EXCHANGE_ASSET(selectedScene_); EXCHANGE_ASSET(additionalSceneRoots_);
  EXCHANGE_ASSET(sceneRootOrder_); EXCHANGE_ASSET(sceneRootsDirty_); EXCHANGE_ASSET(mergedAssets_);
  EXCHANGE_ASSET(deformation_); EXCHANGE_ASSET(materialVariants_); EXCHANGE_ASSET(selectedMaterialVariant_);
  EXCHANGE_ASSET(variantOriginalModel_); EXCHANGE_ASSET(variantOriginalDeformation_); EXCHANGE_ASSET(variantNormalTexCoords_);
  swapSourceModel(exportSource_,other.exportSource_); EXCHANGE_ASSET(sourcePath_);
  EXCHANGE_ASSET(exportMaterialSources_); EXCHANGE_ASSET(exportMaterialTextureBindings_); EXCHANGE_ASSET(exportTextureIndices_);
  EXCHANGE_ASSET(vertices_); EXCHANGE_ASSET(indices_); EXCHANGE_ASSET(geometryRevision_); EXCHANGE_ASSET(materialRevision_);
  EXCHANGE_ASSET(modelBounds_); EXCHANGE_ASSET(authoredPointLights_); EXCHANGE_ASSET(authoredDirectionalLights_);
  EXCHANGE_ASSET(iesProfileAddresses_);
  EXCHANGE_ASSET(authoredAreaLights_); EXCHANGE_ASSET(authoredPointBindings_); EXCHANGE_ASSET(authoredDirectionalBindings_);
  EXCHANGE_ASSET(authoredAreaBindings_); EXCHANGE_ASSET(indexType_); EXCHANGE_ASSET(materialBaseColor_);
  EXCHANGE_ASSET(defaultMaterialIndex_); EXCHANGE_ASSET(diagnosticMaterialIndex_); EXCHANGE_ASSET(sourceMaterialToRuntime_);
  EXCHANGE_ASSET(sourcePrimitiveImplicitMaterials_);
  EXCHANGE_ASSET(gpuMaterials_); EXCHANGE_ASSET(hasExtendedPbrMaterials_); EXCHANGE_ASSET(gpuMaterialTemporalRevisions_);
  EXCHANGE_ASSET(materialBuffer_); EXCHANGE_ASSET(materialBufferCapacity_);
  EXCHANGE_ASSET(textureMetadata_); EXCHANGE_ASSET(textureMetadataBuffer_); EXCHANGE_ASSET(textureMetadataBufferCapacity_);
  EXCHANGE_ASSET(textureSamplingFallback_); EXCHANGE_ASSET(textureSamplingPlan_);
  EXCHANGE_ASSET(sceneClipState_); EXCHANGE_ASSET(sceneClipStateBuffer_);
  EXCHANGE_ASSET(baseColorSampler_); EXCHANGE_ASSET(materialSamplers_); EXCHANGE_ASSET(materialSamplerSlots_);
  EXCHANGE_ASSET(descriptorPool_); EXCHANGE_ASSET(ownsDescriptorPool_);
  EXCHANGE_ASSET(descriptorSets_); EXCHANGE_ASSET(auxiliaryDescriptorSets_); EXCHANGE_ASSET(textureImageOwnership_);
  EXCHANGE_ASSET(textureResourceLifecycle_);
#undef EXCHANGE_ASSET
  if(sharedMaterialLifecycle)
    materialLifecycleNextSlot_=other.materialLifecycleNextSlot_=lifecycleHighWater;
  // A restored snapshot carries its original geometry and map together. Preserve
  // eligibility only if the map still matched that geometry before publication.
  if(opacityMicromapGeometryRevision_==geometryRevision_)
    opacityMicromapGeometryRevision_=nextGeometry;
  else
    opacityMicromapGeometryRevision_.reset();
  // Object epochs describe publication, not the historical payload's old age.
  generationEpoch_=nextEpoch; geometryRevision_=nextGeometry; materialRevision_=nextMaterial;
  materialResourceRevision_=nextResource;
  cameraProjectionRevision_=nextCamera;other.cameraProjectionRevision_=nextOtherCamera;
  cameraEvaluationRevision_=nextEvaluatedCamera;other.cameraEvaluationRevision_=nextOtherEvaluatedCamera;
  if(authoredMaterialRevision_<UINT64_MAX) ++authoredMaterialRevision_;
}

} // namespace container::scene
