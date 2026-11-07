#include <Container/utility/GltfSceneSelection.h>

#include <algorithm>
#include <climits>
#include <set>
#include <stdexcept>

namespace container::scene {
namespace {
[[noreturn]] void fail(const char* message) {
  throw std::invalid_argument(std::string("glTF scene selection: ")+message);
}
}


GltfSceneActivation PlanGltfSceneActivation(const GltfSceneCatalog& catalog,
    const SceneGraph& graph,size_t scene,uint64_t maxRenderableObjects,
    std::span<const uint32_t> additionalRoots) {
  if(scene>=catalog.entries.size()) fail("requested scene index is invalid");
  if(graph.nodeCount()>=SceneGraph::kInvalidNode) fail("runtime node table exceeds its index limit");
  std::vector<uint32_t> mapping(catalog.sourceNodeCount,SceneGraph::kInvalidNode);
  for(uint32_t i=0;i<graph.nodeCount();++i) {
    const auto source=graph.getNode(i)->sourceNodeIndex;
    if(source == -1) continue;
    if(source < -1) fail("runtime source node identity is invalid");
    if(size_t(source)>=mapping.size()) fail("runtime source node identity is invalid");
    if(mapping[size_t(source)]!=SceneGraph::kInvalidNode) fail("runtime source node identity is ambiguous");
    mapping[size_t(source)]=i;
  }
  GltfSceneActivation result;result.scene=scene;
  if(!catalog.authoredScenes && !catalog.sourceNodeCount) result.runtimeRoots=graph.rootNodes();
  else for(const auto source:catalog.entries[scene].sourceRoots) {
    if(source<0 || size_t(source)>=mapping.size() || mapping[size_t(source)]==SceneGraph::kInvalidNode)
      fail("selected source root is missing from the authoring graph");
    const auto runtime = mapping[size_t(source)];
    if (!graph.isNodeRetired(runtime)) result.runtimeRoots.push_back(runtime);
  }
  for(const auto id:additionalRoots) {
    // Undoing an appended primitive temporarily retires its tail node. The
    // same identity is restored by redo; it must not activate another scene.
    if(!graph.getNode(id) || graph.isNodeRetired(id)) continue;
    if(std::find(result.runtimeRoots.begin(),result.runtimeRoots.end(),id)==result.runtimeRoots.end())
      result.runtimeRoots.push_back(id);
  }
  std::erase_if(result.runtimeRoots, [&](uint32_t id) { return graph.isNodeRetired(id); });
  // Reparenting within one scene may put an old root below another selected
  // root. The view is their union; export must list only its topmost entries.
  std::vector<bool> selectedRoot(graph.nodeCount(),false);
  for(const auto id:result.runtimeRoots) selectedRoot[id]=true;
  std::erase_if(result.runtimeRoots,[&](uint32_t id) {
    auto parent=graph.getNode(id)->parent;
    while(parent!=SceneGraph::kInvalidNode) {
      if(parent>=graph.nodeCount()) fail("runtime parent index is invalid");
      if(selectedRoot[parent]) return true;
      parent=graph.getNode(parent)->parent;
    }
    return false;
  });
  std::vector<uint8_t> visited(graph.nodeCount(),0);
  auto stack=result.runtimeRoots;
  while(!stack.empty()) {
    const auto id=stack.back();stack.pop_back();
    if(id>=graph.nodeCount()) fail("runtime child index is invalid");
    if(visited[id]) continue;
    visited[id]=1;
    const auto& node=*graph.getNode(id);
    if (node.retired) continue;
    if(node.renderable && ++result.renderableCount>maxRenderableObjects)
      fail("selected scene exceeds the renderable object budget");
    stack.insert(stack.end(),node.children.begin(),node.children.end());
  }
  return result;
}

} // namespace container::scene
