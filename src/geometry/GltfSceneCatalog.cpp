#include <Container/geometry/GltfSceneCatalog.h>

#include <algorithm>
#include <climits>
#include <set>
#include <stdexcept>
#include <utility>

namespace container::scene {
namespace {
[[noreturn]] void fail(const char* message) {
  throw std::invalid_argument(std::string("glTF scene selection: ")+message);
}
} // namespace

GltfSceneCatalog BuildGltfSceneCatalog(const tinygltf::Model& source,
    const GltfSceneCatalogLimits& limits) {
  const auto count=std::max(size_t{1},source.scenes.size());
  if(!limits.maxScenes || count>limits.maxScenes || source.nodes.size()>INT_MAX)
    fail("scene/node table exceeds its count limit");
  if((source.defaultScene>=0 && size_t(source.defaultScene)>=source.scenes.size()) || source.defaultScene < -1)
    fail("default scene index is invalid");
  std::vector<bool> parent(source.nodes.size());
  for(const auto& node:source.nodes) for(const auto child:node.children) {
    if(child<0 || size_t(child)>=source.nodes.size()) fail("child index is invalid");
    if(parent[size_t(child)]) fail("node hierarchy has multiple parent references");
    parent[size_t(child)]=true;
  }
  std::vector<int32_t> roots;
  for(size_t i=0;i<parent.size();++i) if(!parent[i]) roots.push_back(int32_t(i));
  // With at most one incoming edge, visiting the global roots once proves
  // that all components are strict trees, including unused source nodes.
  std::vector<int32_t> stack=roots;
  size_t visited=0;
  while(!stack.empty()) {
    const auto id=stack.back();stack.pop_back();
    ++visited;
    const auto& children=source.nodes[size_t(id)].children;
    stack.insert(stack.end(),children.begin(),children.end());
  }
  if(visited!=source.nodes.size()) fail("node hierarchy contains a cycle");
  GltfSceneCatalog result;
  result.authoredScenes=!source.scenes.empty(); result.sourceNodeCount=source.nodes.size();
  result.defaultScene=source.defaultScene>=0 ? size_t(source.defaultScene) : 0;
  uint64_t rootCount=0;
  const auto append=[&](std::string name,const auto& sourceRoots) {
    if(rootCount>limits.maxRootReferences || sourceRoots.size()>limits.maxRootReferences-rootCount)
      fail("scene roots exceed the aggregate reference limit");
    rootCount+=sourceRoots.size();
    std::set<int32_t> unique;
    for(const auto id:sourceRoots) {
      if(id<0 || size_t(id)>=source.nodes.size()) fail("scene root index is invalid");
      if(parent[size_t(id)]) fail("scene root is also a node child");
      if(!unique.insert(id).second) fail("scene root is repeated");
    }
    result.entries.push_back({std::move(name),{sourceRoots.begin(),sourceRoots.end()}});
  };
  if(source.scenes.empty()) append("Scene",roots);
  else for(size_t i=0;i<source.scenes.size();++i) {
    const auto& scene=source.scenes[i];
    append(scene.name.empty()?"Scene "+std::to_string(i):scene.name,scene.nodes);
  }
  return result;
}

} // namespace container::scene
