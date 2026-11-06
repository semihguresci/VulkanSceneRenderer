#include <Container/geometry/GltfMergeRuntimeData.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::geometry::gltf {
namespace {
constexpr std::string_view kInstancing="EXT_mesh_gpu_instancing";
[[noreturn]] void fail(std::string_view where,std::string_view message) {
  throw std::runtime_error("glTF merge runtime data "+std::string(where)+": "+std::string(message));
}
size_t added(size_t a,size_t b,std::string_view label) {
  if(b>SIZE_MAX-a) fail(label,"count overflow");return a+b;
}
uint64_t multiplied(uint64_t a,uint64_t b,std::string_view label) {
  if(a && b>UINT64_MAX/a) fail(label,"byte count overflow");return a*b;
}
uint64_t summed(uint64_t a,uint64_t b,std::string_view label) {
  if(b>UINT64_MAX-a) fail(label,"byte count overflow");return a+b;
}
struct Budget {
  const GltfMergeRuntimeDataLimits& limits;
  uint64_t bytes{},workspaceBytes{};
  InstancingSummary instances;
  void storage(uint64_t count,uint64_t width) {
    if(count>(std::min<uint64_t>(limits.maxOutputBytes,SIZE_MAX)-bytes)/width)
      fail("limits","owned output exceeds its byte budget");
    bytes+=count*width;
  }
  void workspace(uint64_t bytes) {
    if(bytes>std::min<uint64_t>(limits.maxWorkspaceBytes,SIZE_MAX)) fail("limits","temporary workspace exceeds its byte budget");
    workspaceBytes=std::max(workspaceBytes,bytes);
  }
};
template<glm::length_t N> bool finite(const glm::vec<N,float,glm::defaultp>& v) {
  for(glm::length_t i=0;i<N;++i) if(!std::isfinite(v[i])) return false;return true;
}
bool finiteAffine(const glm::mat4& value) {
  for(int c=0;c<4;++c) if(!finite(value[c])) return false;
  return std::abs(value[0][3])<=1e-5f && std::abs(value[1][3])<=1e-5f &&
      std::abs(value[2][3])<=1e-5f && std::abs(value[3][3]-1)<=1e-5f;
}
void weightValues(std::span<const float> values,Budget& budget) {
  budget.storage(values.size(),sizeof(float));
  for(const auto value:values) if(!std::isfinite(value)) fail("weights","non-finite retained default");
}
bool deformed(const tinygltf::Model& source) {
  if(!source.skins.empty()) return true;
  for(const auto& mesh:source.meshes) for(const auto& primitive:mesh.primitives)
    if(!primitive.targets.empty()) return true;
  return false;
}
size_t primitiveCount(const tinygltf::Model& source,const GltfMergeRuntimeDataLimits& limits) {
  if(source.meshes.size()>limits.maxPrimitives || source.nodes.size()>limits.maxNodes || source.skins.size()>limits.maxSkins)
    fail("source tables","count exceeds its limit");
  size_t count{};
  for(const auto& mesh:source.meshes) {
    count=added(count,mesh.primitives.size(),"primitives");
    if(count>limits.maxPrimitives) fail("primitives","count exceeds its limit");
    for(const auto& primitive:mesh.primitives)
      if(primitive.targets.size()!=mesh.primitives.front().targets.size()) fail("morphs","inconsistent target counts within mesh");
  }
  return count;
}
size_t morphCount(const tinygltf::Model& source,int mesh) {
  if(mesh<0) return 0;
  if(size_t(mesh)>=source.meshes.size()) fail("node","invalid mesh identity");
  const auto& primitives=source.meshes[size_t(mesh)].primitives;
  return primitives.empty()?0:primitives.front().targets.size();
}
uint32_t sourceNormalTexCoord(const tinygltf::Model& source,const tinygltf::Primitive& primitive) {
  if(primitive.material==-1) return 0;
  if(primitive.material<0 || size_t(primitive.material)>=source.materials.size()) fail("normal UV","invalid default material identity");
  const auto& texture=source.materials[size_t(primitive.material)].normalTexture;
  if(texture.index==-1) return 0;
  if(texture.index<0 || size_t(texture.index)>=source.textures.size() || texture.texCoord<0) fail("normal UV","invalid normal texture identity or selector");
  int selected=texture.texCoord;
  if(const auto transform=texture.extensions.find("KHR_texture_transform");transform!=texture.extensions.end()) {
    if(!transform->second.IsObject()) fail("normal UV","texture transform must be an object");
    if(transform->second.Has("texCoord")) {
      const auto& override=transform->second.Get("texCoord");
      if(!override.IsInt()) fail("normal UV","texture transform selector must be an integer");
      selected=override.Get<int>();
    }
  }
  if(selected<0 || selected>1) fail("normal UV","effective normal texture selector must be UV0 or UV1");
  return uint32_t(selected);
}
int mapped(int value,std::span<const int32_t> map,std::string_view label) {
  if(value<0 || size_t(value)>=map.size() || map[size_t(value)]<0) fail(label,"missing mapped identity");
  return map[size_t(value)];
}
void appendMap(std::span<const int32_t> map,size_t count,size_t offset,size_t finalCount,std::string_view label) {
  if(map.size()!=count || added(offset,count,label)>finalCount) fail(label,"map size disagrees with source or destination");
  for(size_t i=0;i<count;++i) if(map[i]<0 || size_t(map[i])!=offset+i) fail(label,"canonical append identity changed");
}
void attachment(const tinygltf::Node& target,int mesh,int skin,bool context) {
  if(target.mesh!=mesh || target.skin!=skin) fail("plan node","mesh/skin attachment disagrees with its source mapping");
  if(context && (!target.weights.empty() || target.extensions.contains(std::string(kInstancing))))
    fail("plan context","context-only node retains weights or instancing");
}
void primitiveAttachments(const tinygltf::Primitive& source,const tinygltf::Primitive& target,
    const std::vector<int32_t>* accessors) {
  const auto index=[&](int original) {return accessors?mapped(original,*accessors,"primitive accessor"):original;};
  if(source.attributes.size()!=target.attributes.size() || source.targets.size()!=target.targets.size())
    fail("plan primitive","attribute or target layout changed");
  const auto compare=[&](const auto& a,const auto& b) {
    if(a.size()!=b.size()) fail("plan primitive","attribute layout changed");
    for(const auto& [name,value]:a) {
      const auto found=b.find(name);
      if(found==b.end() || found->second!=index(value)) fail("plan primitive","attribute accessor mapping changed");
    }
  };
  compare(source.attributes,target.attributes);
  for(size_t t=0;t<source.targets.size();++t) compare(source.targets[t],target.targets[t]);
}
void skinAttachment(const tinygltf::Skin& source,const tinygltf::Skin& target,
    const std::vector<int32_t>* nodes,const std::vector<int32_t>* accessors) {
  const auto node=[&](int id) {return nodes?mapped(id,*nodes,"skin node"):id;};
  if(source.joints.size()!=target.joints.size() || target.skeleton!=(source.skeleton<0?-1:node(source.skeleton)))
    fail("plan skin","joint/skeleton layout changed");
  for(size_t j=0;j<source.joints.size();++j)
    if(target.joints[j]!=node(source.joints[j])) fail("plan skin","joint identity mapping changed");
  const auto ibm=source.inverseBindMatrices<0?-1:(accessors?mapped(source.inverseBindMatrices,*accessors,"inverse binds"):source.inverseBindMatrices);
  if(target.inverseBindMatrices!=ibm) fail("plan skin","inverse-bind accessor mapping changed");
}
void validatePlan(const tinygltf::Model& base,const tinygltf::Model& incoming,const GltfMergePlan& plan,
    const GltfMergeRuntimeDataLimits& limits) {
  const auto& out=plan.model;const auto& m=plan.imported;const auto& p=plan.placement;
  if(out.nodes.size()>limits.maxNodes || out.nodes.size()>INT32_MAX || out.skins.size()>limits.maxSkins)
    fail("plan tables","count exceeds its limit");
  if(out.meshes.size()!=added(base.meshes.size(),incoming.meshes.size(),"meshes")) fail("plan meshes","unexpected geometry copies");
  appendMap(m.meshes,incoming.meshes.size(),base.meshes.size(),out.meshes.size(),"mesh map");
  appendMap(m.nodes,incoming.nodes.size(),base.nodes.size(),out.nodes.size(),"node map");
  appendMap(m.skins,incoming.skins.size(),base.skins.size(),out.skins.size(),"skin map");
  if(m.accessors.size()!=incoming.accessors.size()) fail("accessor map","size differs from incoming source");
  int32_t previous=-1;
  for(size_t a=0;a<m.accessors.size();++a) {
    const auto destination=m.accessors[a];
    if(destination<0 || size_t(destination)<base.accessors.size() || size_t(destination)>=out.accessors.size() || destination<=previous)
      fail("accessor map","invalid, colliding or reordered append identity");
    const auto& expected=incoming.accessors[a];const auto& actual=out.accessors[size_t(destination)];
    if(expected.count!=actual.count || expected.type!=actual.type || expected.componentType!=actual.componentType || expected.normalized!=actual.normalized)
      fail("accessor map","mapped accessor shape changed");
    previous=destination;
  }
  if(p.nodes.size()!=incoming.nodes.size() || p.contextOnly.size()!=incoming.nodes.size() || p.skins.size()!=incoming.skins.size() ||
      m.primitives.size()!=incoming.meshes.size()) fail("placement maps","size differs from incoming source");
  const auto canonical=added(base.nodes.size(),incoming.nodes.size(),"canonical nodes");
  if(plan.parentNode<0 || size_t(plan.parentNode)!=canonical || canonical>=out.nodes.size()) fail("placement parent","invalid parent identity");
  attachment(out.nodes[canonical],-1,-1,true);
  size_t nextNode=canonical+1,nextSkin=added(base.skins.size(),incoming.skins.size(),"canonical skins");
  for(size_t n=0;n<p.nodes.size();++n) {
    if(p.nodes[n]==-1) {if(p.contextOnly[n]) fail("placement nodes","unplaced node marked context-only");continue;}
    if(p.nodes[n]<0 || size_t(p.nodes[n])!=nextNode++) fail("placement nodes","invalid, colliding or reordered clone identity");
  }
  for(const auto skin:p.skins) if(skin!=-1) {
    if(skin<0 || size_t(skin)!=nextSkin++) fail("placement skins","invalid, colliding or reordered clone identity");
  }
  if(nextNode!=out.nodes.size() || nextSkin!=out.skins.size()) fail("plan tables","unmapped node or skin suffix");
  for(size_t mesh=0;mesh<base.meshes.size();++mesh) {
    if(out.meshes[mesh].primitives.size()!=base.meshes[mesh].primitives.size()) fail("base mesh","primitive count changed");
    for(size_t primitive=0;primitive<base.meshes[mesh].primitives.size();++primitive)
      primitiveAttachments(base.meshes[mesh].primitives[primitive],out.meshes[mesh].primitives[primitive],nullptr);
  }
  for(size_t mesh=0;mesh<incoming.meshes.size();++mesh) {
    const auto& source=incoming.meshes[mesh];const auto& target=out.meshes[size_t(m.meshes[mesh])];
    if(target.primitives.size()!=source.primitives.size() || m.primitives[mesh].size()!=source.primitives.size()) fail("incoming mesh","primitive count changed");
    for(size_t primitive=0;primitive<source.primitives.size();++primitive) {
      if(m.primitives[mesh][primitive].mesh!=m.meshes[mesh] || m.primitives[mesh][primitive].primitive<0 ||
          size_t(m.primitives[mesh][primitive].primitive)!=primitive) fail("primitive map","source definition identity changed");
      primitiveAttachments(source.primitives[primitive],target.primitives[primitive],&m.accessors);
    }
  }
  for(size_t n=0;n<base.nodes.size();++n) attachment(out.nodes[n],base.nodes[n].mesh,base.nodes[n].skin,false);
  for(size_t n=0;n<incoming.nodes.size();++n) {
    const auto& source=incoming.nodes[n];
    const int mesh=source.mesh<0?-1:mapped(source.mesh,m.meshes,"node mesh");
    attachment(out.nodes[size_t(m.nodes[n])],mesh,source.skin<0?-1:mapped(source.skin,m.skins,"node skin"),false);
    if(p.nodes[n]>=0) attachment(out.nodes[size_t(p.nodes[n])],p.contextOnly[n]?-1:mesh,
        p.contextOnly[n] || source.skin<0?-1:mapped(source.skin,p.skins,"placed skin"),p.contextOnly[n]);
  }
  for(size_t s=0;s<base.skins.size();++s) skinAttachment(base.skins[s],out.skins[s],nullptr,nullptr);
  for(size_t s=0;s<incoming.skins.size();++s) {
    skinAttachment(incoming.skins[s],out.skins[size_t(m.skins[s])],&m.nodes,&m.accessors);
    if(p.skins[s]>=0) skinAttachment(incoming.skins[s],out.skins[size_t(p.skins[s])],&p.nodes,&m.accessors);
  }
}
void vertices(std::span<const Vertex> values,const tinygltf::Model& source,const tinygltf::Primitive& definition,Budget& budget) {
  const auto found=definition.attributes.find("POSITION");
  if(found==definition.attributes.end() || found->second<0 || size_t(found->second)>=source.accessors.size() ||
      values.empty() || values.size()!=source.accessors[size_t(found->second)].count) fail("bind vertices","source vertex count differs");
  budget.storage(values.size(),sizeof(Vertex));
  for(const auto& v:values)
    if(!finite(v.position) || !finite(v.color) || !finite(v.texCoord) || !finite(v.texCoord1) ||
        !finite(v.normal) || !finite(v.tangent) || !std::isfinite(v.colorAlpha)) fail("bind vertices","non-finite retained attribute");
}
void validateInput(const GltfMergeRuntimeDataInput& input,size_t primitiveCount,bool needed,Budget& budget) {
  const auto& source=input.source;const auto* data=input.deformation;
  if(!input.normalTextureTexCoords.empty() && input.normalTextureTexCoords.size()!=primitiveCount)
    fail("normal UV","source-flat policy table size differs");
  for(const auto selector:input.normalTextureTexCoords) if(selector>1) fail("normal UV","policy requires UV0 or UV1");
  if(data && (data->primitives.size()!=primitiveCount || data->nodes.size()!=source.nodes.size() || data->skins.size()!=source.skins.size()))
    fail("retained deformation","source table sizes differ");
  if(data) for(size_t p=0;p<data->primitives.size();++p) {
    const auto selector=data->primitives[p].normalTextureTexCoord;
    if(selector>1 || (!input.normalTextureTexCoords.empty() && selector!=input.normalTextureTexCoords[p]))
      fail("normal UV","retained frame policy is unsupported or differs from supplied policy");
  }
  if(needed && !data && (deformed(source) || input.sourceBindPrimitives.size()!=primitiveCount))
    fail("retained deformation","missing deformed data or source-ordered static bind meshes");
  if(!needed) return;
  // One small source-mesh table prevents rechecking every shared owner's full
  // vertex stream. It is bounded independently of the payload copied below.
  budget.workspace(multiplied(source.meshes.size(),sizeof(size_t),"mesh validation"));
  std::vector<size_t> jointLimits(source.meshes.size(),SIZE_MAX);
  for(size_t s=0;s<source.skins.size();++s) {
    const auto& skin=data->skins[s];const auto& definition=source.skins[s];
    if(skin.joints.empty() || skin.joints.size()!=definition.joints.size() || skin.joints.size()!=skin.inverseBindMatrices.size() || skin.skeleton!=definition.skeleton)
      fail("retained skin","joint/matrix/skeleton layout differs");
    if(skin.skeleton<-1 || (skin.skeleton>=0 && size_t(skin.skeleton)>=source.nodes.size())) fail("retained skin","invalid skeleton");
    for(size_t j=0;j<skin.joints.size();++j)
      if(skin.joints[j]>=source.nodes.size() || definition.joints[j]<0 || skin.joints[j]!=uint32_t(definition.joints[j]) || !finiteAffine(skin.inverseBindMatrices[j]))
        fail("retained skin","invalid joint identity or inverse bind matrix");
  }
  for(size_t n=0;n<source.nodes.size();++n) {
    const auto& definition=source.nodes[n];
    if(definition.mesh<-1 || definition.skin<-1 || (definition.skin>=0 && size_t(definition.skin)>=source.skins.size()) ||
        (definition.mesh<0 && definition.skin>=0)) fail("source node","invalid mesh/skin attachment");
    const auto count=morphCount(source,definition.mesh);
    if(data) {
      const auto& node=data->nodes[n];
      if(node.sourceMesh!=definition.mesh || node.skin!=definition.skin || node.defaultMorphWeights.size()!=count)
        fail("retained node","attachment or default morph layout differs");
      for(const auto weight:node.defaultMorphWeights) if(!std::isfinite(weight)) fail("retained node","non-finite authored default");
    } else if(count) fail("static node","morphing requires retained authored data");
    if(definition.skin>=0) jointLimits[size_t(definition.mesh)]=std::min(jointLimits[size_t(definition.mesh)],data->skins[size_t(definition.skin)].joints.size());
  }
  size_t flat{};uint64_t decoderMaps{};
  for(size_t m=0;m<source.meshes.size();++m) for(size_t p=0;p<source.meshes[m].primitives.size();++p,++flat) {
    const auto& definition=source.meshes[m].primitives[p];budget.storage(1,sizeof(DeformationPrimitive));
    if(!data) {
      if(input.normalTextureTexCoords.empty()) (void)sourceNormalTexCoord(source,definition);
      const auto count=input.sourceBindPrimitives[flat].vertices().size();
      vertices(input.sourceBindPrimitives[flat].vertices(),source,definition,budget);
      uint64_t joints{},weights{};
      for(const auto& [semantic,index]:definition.attributes) {
        (void)index;
        joints+=semantic.starts_with("JOINTS_");weights+=semantic.starts_with("WEIGHTS_");
      }
      if(joints) {
        budget.storage(count,sizeof(VertexSkinInfluences));
        budget.storage(count,multiplied(joints,4*sizeof(JointInfluence),"dormant influences"));
      }
      // LoadDeformationData uses two ordered maps while decoding one primitive.
      // Charge conservative 128-byte nodes, including two sentinel nodes. Its
      // CheckedAccessor views do not allocate count-sized scratch arrays.
      decoderMaps=std::max(decoderMaps,multiplied(summed(summed(joints,weights,"decoder maps"),2,"decoder maps"),128,"decoder maps"));
      continue;
    }
    const auto& primitive=data->primitives[flat];
    if(primitive.sourceMesh!=m || primitive.sourcePrimitive!=p || primitive.morphTargets.size()!=definition.targets.size() ||
        primitive.meshDefaultWeights.size()!=definition.targets.size())
      fail("retained primitive","source identity or target/default layout differs");
    vertices(primitive.bindVertices,source,definition,budget);
    if(primitive.authoredBaseNormal!=definition.attributes.contains("NORMAL") || primitive.authoredBaseTangent!=definition.attributes.contains("TANGENT"))
      fail("retained primitive","authored base-frame provenance differs");
    if(primitive.influences.empty()!=!definition.attributes.contains("JOINTS_0") ||
        (!primitive.influences.empty() && primitive.influences.size()!=primitive.bindVertices.size()) ||
        (jointLimits[m]!=SIZE_MAX && primitive.influences.empty())) fail("retained influences","source vertex/influence layout differs");
    budget.storage(primitive.influences.size(),sizeof(VertexSkinInfluences));
    for(const auto& vertex:primitive.influences) {
      budget.storage(vertex.values.size(),sizeof(JointInfluence));double sum{};
      for(const auto& influence:vertex.values) {
        if(!std::isfinite(influence.weight) || influence.weight<0 || influence.weight>1 || influence.joint>=jointLimits[m])
          fail("retained influences","non-finite weight or out-of-range skin joint");
        sum+=influence.weight;
      }
      if(!std::isfinite(sum) || std::abs(sum-1)>1e-4) fail("retained influences","weights must remain normalized");
    }
    budget.storage(primitive.morphTargets.size(),sizeof(MorphTarget));
    for(size_t t=0;t<primitive.morphTargets.size();++t) {
      const auto& target=primitive.morphTargets[t];
      for(const auto& [name,values]:{std::pair{"POSITION",&target.positions},std::pair{"NORMAL",&target.normals},std::pair{"TANGENT",&target.tangents}}) {
        if(values->empty()!=!definition.targets[t].contains(name) || (!values->empty() && values->size()!=primitive.bindVertices.size()))
          fail("retained morph target","delta/source layout differs");
        budget.storage(values->size(),sizeof(glm::vec3));
        for(const auto& value:*values) if(!finite(value)) fail("retained morph target","non-finite delta");
      }
    }
    weightValues(primitive.meshDefaultWeights,budget);
  }
  if(!data) {
    auto temporary=summed(sizeof(DeformationData),multiplied(primitiveCount,sizeof(DeformationPrimitive),"static decoder"),"static decoder");
    temporary=summed(temporary,multiplied(source.nodes.size(),sizeof(DeformationNode),"static decoder"),"static decoder");
    budget.workspace(summed(temporary,decoderMaps,"static decoder"));
  }
}
void chargeSkin(const DeformationSkin& skin,Budget& budget) {
  budget.storage(1,sizeof(DeformationSkin));budget.storage(skin.joints.size(),sizeof(uint32_t));
  budget.storage(skin.inverseBindMatrices.size(),sizeof(glm::mat4));
}
const tinygltf::Value::Object* instanceAttributes(const tinygltf::Node& node) {
  const auto found=node.extensions.find(std::string(kInstancing));if(found==node.extensions.end()) return nullptr;
  if(!found->second.IsObject() || !found->second.Has("attributes") || !found->second.Get("attributes").IsObject())
    fail("instancing","malformed source attributes");
  return &found->second.Get("attributes").Get<tinygltf::Value::Object>();
}
void validateInstancing(const GltfMergeRuntimeDataInput& input) {
  const auto* data=input.instancing;size_t slot{};
  for(size_t n=0;n<input.source.nodes.size();++n) {
    const auto* attributes=instanceAttributes(input.source.nodes[n]);if(!attributes) continue;
    if(!data || slot>=data->nodes.size()) fail("instancing","missing retained source node");
    const auto& node=data->nodes[slot++];
    if(node.sourceNode!=n || input.source.nodes[n].mesh<0 || node.sourceMesh!=uint32_t(input.source.nodes[n].mesh) ||
        node.sourceMesh>=input.source.meshes.size() || node.primitiveCount!=input.source.meshes[node.sourceMesh].primitives.size() ||
        !node.primitiveCount || node.localMatrices.empty() || node.attributes.empty() || node.attributes.size()!=attributes->size())
      fail("instancing","source node/mesh/primitive/attribute layout differs");
    std::string_view previous;
    for(const auto& attribute:node.attributes) {
      const auto found=attributes->find(attribute.semantic);
      if(attribute.semantic.empty() || (!previous.empty() && std::string_view(attribute.semantic)<=previous) || found==attributes->end() || !found->second.IsInt() ||
          found->second.Get<int>()!=attribute.sourceAccessor || attribute.sourceAccessor<0 || size_t(attribute.sourceAccessor)>=input.source.accessors.size() ||
          input.source.accessors[size_t(attribute.sourceAccessor)].count!=node.localMatrices.size())
        fail("instancing","invalid, missing or reordered accessor identity");
      previous=attribute.semantic;
    }
  }
  if(data && slot!=data->nodes.size()) fail("instancing","extra, duplicated or unsorted source nodes");
}
void chargeInstance(const NodeInstancing& node,uint32_t destination,const tinygltf::Model& output,
    const std::vector<int32_t>* accessors,Budget& budget) {
  if(destination>=output.nodes.size() || output.nodes[destination].mesh<0) fail("plan instancing","invalid destination node");
  const auto* attributes=instanceAttributes(output.nodes[destination]);
  if(!attributes || attributes->size()!=node.attributes.size()) fail("plan instancing","missing remapped attributes");
  for(const auto& attribute:node.attributes) {
    const auto found=attributes->find(attribute.semantic);
    const auto index=accessors?mapped(attribute.sourceAccessor,*accessors,"instance accessor"):attribute.sourceAccessor;
    if(found==attributes->end() || !found->second.IsInt() || found->second.Get<int>()!=index || index<0 || size_t(index)>=output.accessors.size())
      fail("plan instancing","accessor mapping differs");
  }
  auto& summary=budget.instances;const auto count=node.localMatrices.size();
  if(count>budget.limits.maxInstances-summary.instances || count>(budget.limits.maxExpandedPrimitives-summary.expandedPrimitives)/node.primitiveCount)
    fail("instancing","aggregate instance/expanded primitive limit exceeded");
  const auto start=budget.bytes;budget.storage(1,sizeof(NodeInstancing));budget.storage(count,sizeof(glm::mat4));
  budget.storage(node.attributes.size(),sizeof(InstanceAttributeReference));
  for(const auto& attribute:node.attributes) budget.storage(attribute.semantic.size(),1);
  for(const auto& matrix:node.localMatrices) if(!finiteAffine(matrix)) fail("instancing","non-finite or non-affine local matrix");
  ++summary.instancedNodes;summary.instances+=count;summary.expandedPrimitives+=count*node.primitiveCount;
  summary.decodedBytes+=budget.bytes-start;
}
std::optional<DeformationData> decodeStatic(const GltfMergeRuntimeDataInput& input,bool needed) {
  if(!needed || input.deformation) return {};
  // No skins or morph targets exist on this side. The decoder cannot capture
  // evaluated morph defaults, and reuses the caller's existing bind frames.
  auto result=LoadDeformationData(input.source,input.sourceBindPrimitives);
  size_t flat{};
  for(const auto& mesh:input.source.meshes) for(const auto& primitive:mesh.primitives) {
    result.primitives[flat].normalTextureTexCoord=input.normalTextureTexCoords.empty()
        ?sourceNormalTexCoord(input.source,primitive):input.normalTextureTexCoords[flat];
    ++flat;
  }
  return std::optional<DeformationData>(std::move(result));
}
} // namespace

GltfMergeRuntimeData ComposeGltfMergeRuntimeData(const GltfMergeRuntimeDataInput& base,
    const GltfMergeRuntimeDataInput& incoming,const GltfMergePlan& plan,const GltfMergeRuntimeDataLimits& limits) {
  if(!limits.maxNodes || !limits.maxPrimitives || !limits.maxSkins || !limits.maxInstances || !limits.maxExpandedPrimitives || !limits.maxOutputBytes || !limits.maxWorkspaceBytes)
    fail("limits","all limits must be finite and positive");
  const auto baseCount=primitiveCount(base.source,limits),incomingCount=primitiveCount(incoming.source,limits);
  if(added(baseCount,incomingCount,"combined primitives")>limits.maxPrimitives) fail("primitives","combined count exceeds limit");
  validatePlan(base.source,incoming.source,plan,limits);
  const bool needed=deformed(base.source) || deformed(incoming.source);Budget budget{limits};
  validateInput(base,baseCount,needed,budget);validateInput(incoming,incomingCount,needed,budget);
  validateInstancing(base);validateInstancing(incoming);
  if(needed) {
    budget.storage(plan.model.nodes.size(),sizeof(DeformationNode));
    const auto chargeNodes=[&](const GltfMergeRuntimeDataInput& input,bool placed) {
      for(size_t n=0;n<input.source.nodes.size();++n) {
        if(placed && (plan.placement.nodes[n]<0 || plan.placement.contextOnly[n])) continue;
        if(input.deformation) weightValues(input.deformation->nodes[n].defaultMorphWeights,budget);
      }
    };
    chargeNodes(base,false);chargeNodes(incoming,false);chargeNodes(incoming,true);
    if(base.deformation) for(const auto& skin:base.deformation->skins) chargeSkin(skin,budget);
    if(incoming.deformation) for(size_t s=0;s<incoming.deformation->skins.size();++s) {
      chargeSkin(incoming.deformation->skins[s],budget);
      if(plan.placement.skins[s]>=0) chargeSkin(incoming.deformation->skins[s],budget);
    }
  }
  if(base.instancing) for(const auto& node:base.instancing->nodes) chargeInstance(node,node.sourceNode,plan.model,nullptr,budget);
  if(incoming.instancing) for(const auto& node:incoming.instancing->nodes) {
    chargeInstance(node,uint32_t(plan.imported.nodes[node.sourceNode]),plan.model,&plan.imported.accessors,budget);
    if(plan.placement.nodes[node.sourceNode]>=0 && !plan.placement.contextOnly[node.sourceNode])
      chargeInstance(node,uint32_t(plan.placement.nodes[node.sourceNode]),plan.model,&plan.imported.accessors,budget);
  }
  size_t finalInstanceNodes{};
  for(const auto& node:plan.model.nodes) if(instanceAttributes(node)) ++finalInstanceNodes;
  if(finalInstanceNodes!=budget.instances.instancedNodes) fail("plan instancing","unmapped destination extension node");
  // No decoder or payload copy occurs before all allocation-bound preflight.
  // Decoder temporaries are private, bounded and validated before retained
  // payloads are copied. Their primitive allocations are moved into output.
  auto staticBase=decodeStatic(base,needed),staticIncoming=decodeStatic(incoming,needed);
  GltfMergeRuntimeData result;result.outputBytes=budget.bytes;result.workspaceBytes=budget.workspaceBytes;
  if(needed) {
    auto& out=result.deformation.emplace();out.primitives.reserve(baseCount+incomingCount);
    const auto appendPrimitives=[&](const GltfMergeRuntimeDataInput& input,std::optional<DeformationData>& decoded,bool imported) {
      size_t flat{};
      for(size_t m=0;m<input.source.meshes.size();++m) for(size_t p=0;p<input.source.meshes[m].primitives.size();++p,++flat) {
        DeformationPrimitive value;
        if(decoded) value=std::move(decoded->primitives[flat]);else value=input.deformation->primitives[flat];
        if(imported) value.sourceMesh=size_t(plan.imported.meshes[m]);out.primitives.push_back(std::move(value));
      }
    };
    appendPrimitives(base,staticBase,false);appendPrimitives(incoming,staticIncoming,true);
    out.skins.resize(plan.model.skins.size());
    const auto copySkin=[&](const DeformationSkin& input,size_t destination,const std::vector<int32_t>* nodes) {
      auto value=input;
      if(nodes) {for(auto& joint:value.joints) joint=uint32_t(mapped(int(joint),*nodes,"retained joint"));
        if(value.skeleton>=0) value.skeleton=mapped(value.skeleton,*nodes,"retained skeleton");}
      out.skins[destination]=std::move(value);
    };
    if(base.deformation) for(size_t s=0;s<base.deformation->skins.size();++s) copySkin(base.deformation->skins[s],s,nullptr);
    if(incoming.deformation) for(size_t s=0;s<incoming.deformation->skins.size();++s) {
      copySkin(incoming.deformation->skins[s],size_t(plan.imported.skins[s]),&plan.imported.nodes);
      if(plan.placement.skins[s]>=0) copySkin(incoming.deformation->skins[s],size_t(plan.placement.skins[s]),&plan.placement.nodes);
    }
    out.nodes.resize(plan.model.nodes.size());
    for(size_t n=0;n<out.nodes.size();++n) {out.nodes[n].sourceMesh=plan.model.nodes[n].mesh;out.nodes[n].skin=plan.model.nodes[n].skin;}
    const auto copyNode=[&](const GltfMergeRuntimeDataInput& input,size_t source,size_t destination) {
      if(input.deformation) out.nodes[destination].defaultMorphWeights=input.deformation->nodes[source].defaultMorphWeights;
    };
    for(size_t n=0;n<base.source.nodes.size();++n) copyNode(base,n,n);
    for(size_t n=0;n<incoming.source.nodes.size();++n) {
      copyNode(incoming,n,size_t(plan.imported.nodes[n]));
      if(plan.placement.nodes[n]>=0 && !plan.placement.contextOnly[n]) copyNode(incoming,n,size_t(plan.placement.nodes[n]));
    }
  }
  auto& instances=result.instancing;instances.summary=budget.instances;instances.nodes.reserve(size_t(budget.instances.instancedNodes));
  if(base.instancing) instances.nodes.insert(instances.nodes.end(),base.instancing->nodes.begin(),base.instancing->nodes.end());
  if(incoming.instancing) for(const auto& node:incoming.instancing->nodes) {
    auto value=node;value.sourceNode=uint32_t(plan.imported.nodes[node.sourceNode]);value.sourceMesh=uint32_t(plan.imported.meshes[node.sourceMesh]);
    for(auto& attribute:value.attributes) attribute.sourceAccessor=mapped(attribute.sourceAccessor,plan.imported.accessors,"retained instance accessor");
    instances.nodes.push_back(value);
    if(plan.placement.nodes[node.sourceNode]>=0 && !plan.placement.contextOnly[node.sourceNode]) {
      value.sourceNode=uint32_t(plan.placement.nodes[node.sourceNode]);instances.nodes.push_back(std::move(value));
    }
  }
  std::ranges::sort(instances.nodes,{},&NodeInstancing::sourceNode);
  return result;
}
} // namespace container::geometry::gltf
