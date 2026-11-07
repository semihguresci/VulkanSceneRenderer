#include <Container/utility/GraphCloneProjection.h>
#include <Container/utility/SceneNodeAuthoringPolicy.h>
#include <Container/utility/SourcePrimitiveMapping.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <climits>
#include <iterator>
#include <stdexcept>
#include <string>

namespace container::scene {
namespace {
using Role=geometry::gltf::GltfCloneNodeRole;
constexpr auto invalid=SceneGraph::kInvalidNode;
constexpr uint8_t absent=3;
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("Graph clone projection: ")+message); }
uint64_t add(uint64_t a,uint64_t b) { if(b>UINT64_MAX-a) fail("budget overflow");return a+b; }
uint64_t mul(uint64_t a,uint64_t b) { if(a && b>UINT64_MAX/a) fail("budget overflow");return a*b; }
void charge(uint64_t& bytes,uint64_t count,uint64_t limit,const char* message) {
  bytes=add(bytes,count);if(bytes>limit) fail(message);
}
bool finite(const glm::mat4& value) {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(value[c][r])) return false;
  return true;
}
bool structural(const SceneNode& node) { return node.editorSceneRoot || node.interactionRootBoundary; }
}

GraphCloneProjectionPlan BuildGraphCloneProjection(
    const GraphCloneProjectionInput& input,const GraphCloneProjectionLimits& limits) {
  const auto& base=input.graph;const auto& source=input.source;
  const auto count=base.nodeCount();
  if(!limits.maxWorkspaceBytes || !limits.maxOutputBytes || !limits.maxReferences ||
      count>=invalid || count>limits.graph.hierarchy.maxGraphNodes || source.nodes.size()>count ||
      input.runtimePrimitiveCount>=invalid || input.runtimeMaterialCount>=invalid)
    fail("input count/storage bounds are invalid");
  if(input.sceneRoots.empty() || input.sceneRoots.size()!=std::max(size_t{1},source.scenes.size()) ||
      input.sceneRoots.size()>limits.maxScenes || input.selectedScene>=input.sceneRoots.size())
    fail("scene root catalog is incomplete");
  if(base.activeRoots() && *base.activeRoots()!=input.sceneRoots[input.selectedScene])
    fail("selected scene roots do not match the live active view");
  if(source.meshes.size()>limits.maxReferences || source.skins.size()>limits.maxReferences)
    fail("source definition tables exceed their bound");
  if(input.root>=count || base.isNodeRetired(input.root) || !base.isNodeInActiveHierarchy(input.root))
    fail("visual root is missing, retired or outside the active hierarchy");
  if(input.rootName) ValidateSceneNodeName(*input.rootName);
  uint64_t edges{},rootReferences{},baseBytes=sizeof(SceneGraph),sourceReferences{};
  for(uint32_t id=0;id<count;++id) {
    const auto& node=*base.getNode(id);
    charge(edges,node.children.size(),limits.maxReferences,"graph edges exceed their bound");
    baseBytes=add(baseBytes,add(sizeof(SceneNode),add(node.name.size(),mul(node.children.size(),sizeof(uint32_t)))));
    if(!finite(node.localTransform) || !finite(node.worldTransform)) fail("nonfinite retained transforms");
    if(node.materialIndex>=std::min(input.runtimeMaterialCount,limits.graph.hierarchy.runtimeMaterialCount))
      fail("runtime material index is invalid");
    if(node.renderable && node.primitiveIndex>=std::min(input.runtimePrimitiveCount,limits.graph.hierarchy.primitiveRangeCount))
      fail("runtime primitive index is invalid");
  }
  for(const auto& roots:input.sceneRoots)
    charge(rootReferences,roots.size(),limits.maxSceneRootReferences,"scene root references exceed their bound");
  for(const auto& skin:source.skins)
    charge(sourceReferences,add(skin.joints.size(),1),limits.maxReferences,"skin references exceed their bound");
  uint64_t primitiveCount{};
  for(const auto& mesh:source.meshes)
    charge(primitiveCount,mesh.primitives.size(),limits.maxReferences,"source primitive table exceeds its bound");
  GraphCloneProjectionPlan result;
  result.workspaceBytes=add(mul(count,112),add(mul(source.nodes.size(),16),add(mul(input.sceneRoots.size(),96),
      add(mul(rootReferences,24),add(mul(sourceReferences,8),add(mul(primitiveCount,8),mul(source.meshes.size(),8)))))));
  if(result.workspaceBytes>limits.maxWorkspaceBytes) fail("workspace budget exceeded before allocation");
  // The output owns one full graph and a projection/mapping/scene table. Reserve
  // a worst-case second full graph worth of appended payload before copying it.
  result.outputBytes=add(mul(baseBytes,3),add(result.workspaceBytes,
      add(mul(rootReferences,16),input.rootName?mul(input.rootName->size(),3):0)));
  if(result.outputBytes>limits.maxOutputBytes) fail("output budget exceeded before allocation");
  std::vector<uint32_t> incoming(count),sourceGraph(source.nodes.size(),invalid),meshBases;
  meshBases.reserve(source.meshes.size());
  uint32_t flat{};
  for(const auto& mesh:source.meshes) {meshBases.push_back(flat);flat+=uint32_t(mesh.primitives.size());}
  std::vector<uint32_t> primitiveIds;
  if(input.sourcePrimitiveToRuntime.empty()) {
    if(primitiveCount>input.runtimePrimitiveCount) fail("source primitive prefix exceeds runtime geometry");
    primitiveIds.reserve(primitiveCount);
    for(uint32_t p=0;p<primitiveCount;++p) primitiveIds.push_back(p);
  } else {
    ValidateSourcePrimitiveIds(input.sourcePrimitiveToRuntime,primitiveCount,input.runtimePrimitiveCount);
    primitiveIds.assign(input.sourcePrimitiveToRuntime.begin(),input.sourcePrimitiveToRuntime.end());
  }
  for(uint32_t id=0;id<count;++id) {
    const auto& node=*base.getNode(id);
    if(node.parent!=invalid && node.parent>=count) fail("parent index is invalid");
    for(const auto child:node.children) {
      if(child>=count || base.getNode(child)->parent!=id || ++incoming[child]!=1) fail("graph forest is inconsistent");
    }
    if(node.sourceNodeIndex!=-1) {
      if(node.sourceNodeIndex<0 || size_t(node.sourceNodeIndex)>=sourceGraph.size() ||
          sourceGraph[size_t(node.sourceNodeIndex)]!=invalid) fail("source node identity is invalid or repeated");
      sourceGraph[size_t(node.sourceNodeIndex)]=id;
    }
    if(node.sourceMeshIndex < -1 || node.sourcePrimitiveIndex < -1 || node.sourceInstanceIndex < -1)
      fail("negative source primitive identity is invalid");
    if(node.meshOwnerGraphNode!=invalid && (node.meshOwnerGraphNode>=count ||
        (!node.retired && base.isNodeRetired(node.meshOwnerGraphNode)))) fail("mesh owner is invalid or retired");
    if(node.sourceMeshIndex>=0) {
      const auto mesh=size_t(node.sourceMeshIndex);
      if(mesh>=source.meshes.size() || node.sourcePrimitiveIndex<0 ||
          size_t(node.sourcePrimitiveIndex)>=source.meshes[mesh].primitives.size() ||
          primitiveIds[meshBases[mesh]+uint32_t(node.sourcePrimitiveIndex)]!=node.primitiveIndex)
        fail("source/runtime primitive identity is inconsistent");
    }
  }
  if(std::ranges::find(sourceGraph,invalid)!=sourceGraph.end()) fail("source group mapping is incomplete");
  std::vector<uint8_t> rootMark(count),visited(count);
  for(const auto root:base.rootNodes()) {
    if(root>=count || rootMark[root]++ || incoming[root] || base.getNode(root)->parent!=invalid) fail("global roots are invalid");
  }
  for(uint32_t id=0;id<count;++id)
    if((incoming[id]==0)!=(rootMark[id]!=0)) fail("global forest omits a root");
  std::vector<uint32_t> preorder,begin(count),end(count);preorder.reserve(count);
  auto stack=base.rootNodes();size_t visitedCount{};
  while(!stack.empty()) {
    const auto id=stack.back();stack.pop_back();if(visited[id]++) fail("graph has a cycle or repeated edge");
    begin[id]=uint32_t(preorder.size());end[id]=begin[id]+1;preorder.push_back(id);
    ++visitedCount;const auto& children=base.getNode(id)->children;stack.insert(stack.end(),children.begin(),children.end());
  }
  if(visitedCount!=count) fail("global forest contains a cycle");
  for(auto it=preorder.rbegin();it!=preorder.rend();++it)
    if(const auto parent=base.getNode(*it)->parent;parent!=invalid) end[parent]=std::max(end[parent],end[*it]);
  std::vector<uint32_t> sceneStamp(count,invalid);
  for(uint32_t scene=0;scene<input.sceneRoots.size();++scene) for(const auto root:input.sceneRoots[scene]) {
    if(root>=count || sceneStamp[root]==scene) fail("scene root index is invalid or repeated");
    sceneStamp[root]=scene;
  }

  std::vector<uint8_t> role(count,absent),bindings(count),chainDone(count);
  stack={input.root};
  while(!stack.empty()) {
    const auto id=stack.back();stack.pop_back();const auto& node=*base.getNode(id);
    if(node.retired) continue;
    if(structural(node)) fail("visual subtree includes a structural wrapper");
    role[id]=uint8_t(Role::Visual);
    stack.insert(stack.end(),node.children.begin(),node.children.end());
  }
  const auto dependency=[&](uint32_t id,Role wanted) {
    if(id>=count || base.isNodeRetired(id)) fail("required dependency is missing or retired");
    if(structural(*base.getNode(id))) fail("required dependency is a protected structural wrapper");
    if(role[id]==uint8_t(Role::Visual)) return;
    role[id]=std::min(role[id],uint8_t(wanted));
    for(auto current=id;current!=invalid;) {
      const auto& node=*base.getNode(current);
      if(structural(node) || role[current]==uint8_t(Role::Visual)) break;
      if(node.retired) fail("required ancestor is retired");
      role[current]=std::min(role[current],uint8_t(Role::JointTransformOnly));
      if(chainDone[current]) break;
      chainDone[current]=1;current=node.parent;
    }
  };
  for(uint32_t id=0;id<count;++id) if(role[id]==uint8_t(Role::Visual)) {
    const auto& node=*base.getNode(id);
    if(!node.renderable) continue;
    ++result.clonedRenderables;
    if(node.sourceMeshIndex<0) continue;
    const auto owner=node.meshOwnerGraphNode;
    if(owner==invalid || base.getNode(owner)->sourceNodeIndex<0) fail("imported primitive has no source mesh owner");
    const auto sourceOwner=size_t(base.getNode(owner)->sourceNodeIndex);
    if(source.nodes[sourceOwner].mesh!=node.sourceMeshIndex) fail("source mesh owner differs from the primitive binding");
    bindings[owner]=1;dependency(owner,Role::MeshBindingOnly);
  }
  for(uint32_t id=0;id<count;++id) if(bindings[id]) {
    const auto sourceId=size_t(base.getNode(id)->sourceNodeIndex);
    const auto skin=source.nodes[sourceId].skin;
    if(skin==-1) continue;
    if(skin<0 || size_t(skin)>=source.skins.size()) fail("mesh owner skin is invalid");
    const auto& value=source.skins[size_t(skin)];
    for(const auto joint:value.joints) {
      if(joint<0 || size_t(joint)>=sourceGraph.size()) fail("skin joint is invalid");
      dependency(sourceGraph[size_t(joint)],Role::JointTransformOnly);
    }
    if(value.skeleton!=-1) {
      if(value.skeleton<0 || size_t(value.skeleton)>=sourceGraph.size()) fail("skin skeleton is invalid");
      dependency(sourceGraph[size_t(value.skeleton)],Role::JointTransformOnly);
    }
  }
  result.firstNode=uint32_t(count);result.oldToNewGraph.assign(count,invalid);
  for(uint32_t id=0;id<count;++id) if(role[id]!=absent) {
    const auto next=add(count,result.newToOldGraph.size());
    if(next>=invalid || next>=limits.graph.hierarchy.maxGraphNodes) fail("appended graph exceeds its node bound");
    result.oldToNewGraph[id]=uint32_t(next);result.newToOldGraph.push_back(id);
  }
  result.root=result.oldToNewGraph[input.root];
  uint64_t retainedRenderables{};
  for(uint32_t id=0;id<count;++id) retainedRenderables+=base.getNode(id)->renderable;
  if(add(retainedRenderables,result.clonedRenderables)>limits.graph.hierarchy.maxRenderableObjects ||
      add(base.renderableNodes().size(),result.clonedRenderables)>limits.graph.maxActiveRenderableObjects)
    fail("appended draw count exceeds its authoring/active bound");
  std::vector<SceneNode> tail;tail.reserve(result.newToOldGraph.size());
  for(const auto old:result.newToOldGraph) {
    auto node=*base.getNode(old);node.children.clear();
    if(old!=input.root && node.parent!=invalid && result.oldToNewGraph[node.parent]!=invalid)
      node.parent=result.oldToNewGraph[node.parent];
    if(role[old]!=uint8_t(Role::Visual)) {
      node.renderable=false;node.primitiveIndex=invalid;node.meshOwnerGraphNode=invalid;
      node.sourceMeshIndex=node.sourcePrimitiveIndex=node.sourceInstanceIndex=-1;
    } else if(node.meshOwnerGraphNode!=invalid && result.oldToNewGraph[node.meshOwnerGraphNode]!=invalid)
      node.meshOwnerGraphNode=result.oldToNewGraph[node.meshOwnerGraphNode];
    if(old==input.root && input.rootName) node.name=*input.rootName;
    if(node.sourceInstanceIndex>=0 && !finite(glm::inverse(node.worldTransform))) fail("copied instance world is singular");
    tail.push_back(std::move(node));
  }
  // Assemble new-parent children in the original sibling order, including
  // collapsed dependency forests, without touching retained parent lists.
  for(size_t n=0;n<tail.size();++n) {
    const auto old=result.newToOldGraph[n];
    for(const auto child:base.getNode(old)->children) {
      const auto mapped=result.oldToNewGraph[child];
      if(mapped!=invalid && tail[mapped-count].parent==count+n) tail[n].children.push_back(mapped);
    }
    if((tail[n].parent==invalid || tail[n].parent<count) && role[old]!=uint8_t(Role::Visual))
      result.dependencyRoots.push_back(uint32_t(count+n));
  }
  std::vector<uint32_t> sourceOrdinal(count,invalid);
  // Source projection uses actual graph preorder, including graph-only
  // intermediates, while each source receives one identity in the new tail.
  std::vector<std::pair<uint32_t,int32_t>> pending;
  for(auto it=result.newToOldGraph.rbegin();it!=result.newToOldGraph.rend();++it) {
    const auto n=result.oldToNewGraph[*it];
    if(tail[n-count].parent==invalid || tail[n-count].parent<count) pending.push_back({n,-1});
  }
  while(!pending.empty()) {
    const auto [id,parentSource]=pending.back();pending.pop_back();
    auto& node=tail[id-count];const auto old=result.newToOldGraph[id-count];
    auto projectedParent=parentSource;
    if(node.sourceNodeIndex>=0) {
      const auto original=node.sourceNodeIndex;
      if(add(source.nodes.size(),result.sourceProjection.nodes.size())>=INT_MAX) fail("source clone tail exceeds integer range");
      sourceOrdinal[old]=uint32_t(result.sourceProjection.nodes.size());
      result.sourceProjection.nodes.push_back({original,parentSource,Role(role[old]),bool(bindings[old]),
          node.name,node.visible,node.selectable,node.hoverable});
      node.sourceNodeIndex=int32_t(source.nodes.size()+sourceOrdinal[old]);projectedParent=original;
    }
    for(auto child=node.children.rbegin();child!=node.children.rend();++child)
      pending.push_back({*child,projectedParent});
  }
  // Parent-active scenes inherit the new sibling naturally. A context/root
  // duplication is selected-scene-only and receives one explicit runtime root.
  const auto parent=base.getNode(input.root)->parent;
  std::vector<uint8_t> ancestors(count);
  for(auto current=parent;current!=invalid;current=base.getNode(current)->parent) ancestors[current]=1;
  for(size_t scene=0;scene<input.sceneRoots.size();++scene) {
    bool inherits=false;
    for(const auto root:input.sceneRoots[scene]) if(ancestors[root] && !base.isNodeRetired(root)) {inherits=true;break;}
    if(inherits) result.sourceProjection.affectedScenes.push_back(scene);
  }
  if(std::ranges::find(result.sourceProjection.affectedScenes,input.selectedScene)==result.sourceProjection.affectedScenes.end())
    result.sourceProjection.affectedScenes.push_back(input.selectedScene);
  std::ranges::sort(result.sourceProjection.affectedScenes);
  result.sourceProjection.destinationScene=input.selectedScene;
  if(mul(result.dependencyRoots.size(),result.sourceProjection.affectedScenes.size())>limits.maxReferences)
    fail("scene dependency coverage work exceeds its bound");
  result.addedSceneRoots.resize(input.sceneRoots.size());
  uint64_t addedReferences{};
  const auto addRoot=[&](size_t scene,uint32_t root) {
    const auto nextReferences=add(addedReferences,1),nextBytes=add(result.outputBytes,16);
    if(add(rootReferences,nextReferences)>limits.maxSceneRootReferences) fail("appended scene roots exceed their bound");
    if(nextBytes>limits.maxOutputBytes) fail("appended root storage exceeds its output bound");
    result.addedSceneRoots[scene].push_back(root);addedReferences=nextReferences;result.outputBytes=nextBytes;
  };
  for(const auto scene:result.sourceProjection.affectedScenes) {
    std::vector<std::pair<uint32_t,uint32_t>> intervals;intervals.reserve(input.sceneRoots[scene].size());
    for(const auto root:input.sceneRoots[scene]) if(!base.isNodeRetired(root)) intervals.push_back({begin[root],end[root]});
    std::ranges::sort(intervals);size_t merged{};
    for(const auto interval:intervals) {
      if(merged && interval.first<intervals[merged-1].second)
        intervals[merged-1].second=std::max(intervals[merged-1].second,interval.second);
      else intervals[merged++]=interval;
    }
    intervals.resize(merged);
    const auto covered=[&](uint32_t node) {
      if(node==invalid) return false;
      const auto it=std::upper_bound(intervals.begin(),intervals.end(),begin[node],
          [](uint32_t value,const auto& interval) {return value<interval.first;});
      return it!=intervals.begin() && begin[node]<std::prev(it)->second;
    };
    bool inherits=false;
    for(const auto root:input.sceneRoots[scene]) if(ancestors[root] && !base.isNodeRetired(root)) {inherits=true;break;}
    if(!inherits) addRoot(scene,result.root);
    for(const auto dependency:result.dependencyRoots) if(!covered(tail[dependency-count].parent)) {
      addRoot(scene,dependency);
    }
    if(add(rootReferences,addedReferences)>limits.maxSceneRootReferences) fail("appended scene roots exceed their bound");
  }
  if(add(rootReferences,addedReferences)>limits.maxSceneRootReferences) fail("appended scene roots exceed their bound");
  result.sceneRoots.assign(input.sceneRoots.begin(),input.sceneRoots.end());
  for(const auto scene:result.sourceProjection.affectedScenes) {
    const auto& additions=result.addedSceneRoots[scene];
    result.sceneRoots[scene].insert(result.sceneRoots[scene].end(),additions.begin(),additions.end());
  }
  result.graph=base.makeAppendCandidate(std::move(tail));
  if(base.activeRoots()) result.graph.setActiveRoots(result.sceneRoots[input.selectedScene]);
  result.ownership=geometry::gltf::GltfProjectedCloneOwnershipProof(
      source,result.sourceProjection,limits.maxOutputBytes-result.outputBytes);
  result.outputBytes=add(result.outputBytes,result.ownership.retainedBytes());
  return result;
}
} // namespace container::scene
