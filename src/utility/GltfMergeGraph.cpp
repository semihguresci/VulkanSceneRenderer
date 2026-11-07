#include <Container/utility/GltfMergeGraph.h>

#include <Container/geometry/GltfSceneCatalog.h>
#include <Container/utility/SourcePrimitiveMapping.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <glm/gtc/matrix_inverse.hpp>

namespace container::scene {
namespace {
constexpr auto invalid = SceneGraph::kInvalidNode;

[[noreturn]] void fail(const char* message) {
  throw std::invalid_argument(std::string("glTF merged graph: ") + message);
}

bool finite(const glm::mat4& value) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(value[column][row])) return false;
  return true;
}

// Validate before traversing old ancestry. In particular,
// legacy mutable node access must not turn a bad base into an unbounded walk.
void validateBaseForest(const SceneGraph& graph) {
  std::vector<uint8_t> incoming(graph.nodeCount(), 0), roots(graph.nodeCount(), 0);
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    const auto& node = *graph.getNode(id);
    if (node.parent != invalid && (node.parent >= graph.nodeCount() || node.parent == id))
      fail("retained graph has an invalid parent");
    for (const auto child : node.children) {
      if (child >= graph.nodeCount() || graph.getNode(child)->parent != id || incoming[child]++)
        fail("retained graph has an invalid or repeated child");
    }
  }
  std::vector<uint32_t> work;
  work.reserve(graph.nodeCount());
  for (const auto root : graph.rootNodes()) {
    if (root >= graph.nodeCount() || roots[root]++ || graph.getNode(root)->parent != invalid)
      fail("retained graph has an invalid root list");
    work.push_back(root);
  }
  for (uint32_t id = 0; id < graph.nodeCount(); ++id)
    if (incoming[id] != (graph.getNode(id)->parent != invalid) ||
        roots[id] != (graph.getNode(id)->parent == invalid))
      fail("retained graph has inconsistent topology");
  size_t visited = 0;
  for (size_t next = 0; next < work.size(); ++next) {
    const auto& children = graph.getNode(work[next])->children;
    work.insert(work.end(), children.begin(), children.end());
    ++visited;
  }
  if (visited != graph.nodeCount()) fail("retained graph contains a cycle");
  if (graph.activeRoots())
    for (const auto root : *graph.activeRoots())
      if (root >= graph.nodeCount()) fail("retained active root is outside the graph");
}
} // namespace

GltfMergeGraphCandidate BuildGltfMergeGraphCandidate(
    const SceneGraph& base, const geometry::gltf::GltfMergePlan& plan,
    size_t retainedSourceNodeCount,
    std::span<const uint32_t> primitiveMaterialIndices,
    std::span<const uint32_t> sourcePrimitiveToRuntime,
    const geometry::gltf::GltfInstancingData& mergedInstancing,
    const glm::mat4& importTransform, const GltfMergeGraphLimits& limits) {
  const auto& source = plan.model;
  const auto graphLimit = std::min<uint64_t>(limits.hierarchy.maxGraphNodes, invalid);
  if (!graphLimit || !limits.hierarchy.maxRenderableObjects || !limits.maxActiveRenderableObjects ||
      base.nodeCount() >= graphLimit || retainedSourceNodeCount > source.nodes.size())
    fail("invalid or exhausted graph limits");
  validateBaseForest(base);
  const auto catalog = BuildGltfSceneCatalog(source);
  if (plan.parentNode < 0 || size_t(plan.parentNode) < retainedSourceNodeCount ||
      size_t(plan.parentNode) >= source.nodes.size() || plan.destinationScene < 0 ||
      size_t(plan.destinationScene) >= catalog.entries.size())
    fail("merge placement or destination identity is invalid");
  const auto parentSource = size_t(plan.parentNode);
  if (source.nodes[parentSource].mesh >= 0 ||
      std::ranges::find(catalog.entries[size_t(plan.destinationScene)].sourceRoots, plan.parentNode) ==
          catalog.entries[size_t(plan.destinationScene)].sourceRoots.end())
    fail("named placement must be a meshless source scene root");
  if (plan.imported.nodes.size() != parentSource - retainedSourceNodeCount ||
      plan.placement.nodes.size() != plan.imported.nodes.size())
    fail("merge node maps do not cover the canonical incoming table");
  size_t placed = 0;
  for (size_t i = 0; i < plan.imported.nodes.size(); ++i) {
    if (plan.imported.nodes[i] != int32_t(retainedSourceNodeCount + i))
      fail("canonical incoming node identities are not append-only");
    const auto id = plan.placement.nodes[i];
    if (id == -1) continue;
    if (id < 0 || size_t(id) != parentSource + 1 + placed++)
      fail("placed node identities are not append-only");
  }
  if (source.nodes.size() != parentSource + 1 + placed)
    fail("merge node maps do not cover every appended source node");
  std::vector<uint32_t> sourceParents(source.nodes.size(), invalid);
  for (size_t parent = 0; parent < source.nodes.size(); ++parent)
    for (const auto child : source.nodes[parent].children) {
      sourceParents[size_t(child)] = uint32_t(parent);
      if ((parent < retainedSourceNodeCount) != (size_t(child) < retainedSourceNodeCount))
        fail("new source topology must not adopt retained nodes or parents");
    }
  for (size_t id = retainedSourceNodeCount; id < parentSource; ++id)
    if (sourceParents[id] != invalid && sourceParents[id] >= parentSource)
      fail("canonical incoming nodes must remain outside the placed closure");
  for (size_t id = parentSource + 1; id < source.nodes.size(); ++id)
    if (sourceParents[id] == invalid || sourceParents[id] < parentSource)
      fail("every placed source node must descend from the named parent");

  size_t primitiveCount = 0;
  std::vector<size_t> primitiveBases;
  primitiveBases.reserve(source.meshes.size());
  for (const auto& mesh : source.meshes) {
    primitiveBases.push_back(primitiveCount);
    if (mesh.primitives.size() > size_t(std::numeric_limits<int32_t>::max()) - primitiveCount)
      fail("source primitive count exceeds its identity range");
    primitiveCount += mesh.primitives.size();
  }
  if (primitiveMaterialIndices.size() != primitiveCount)
    fail("material IDs must cover the complete source primitive table");
  ValidateSourcePrimitiveIds(sourcePrimitiveToRuntime, primitiveCount, limits.hierarchy.primitiveRangeCount);
  for (const auto material : primitiveMaterialIndices)
    if (material >= std::min<uint64_t>(limits.hierarchy.runtimeMaterialCount, invalid))
      fail("source primitive material ID is outside the runtime table");

  GltfMergeGraphCandidate result;
  result.sourceNodeToGraph.assign(source.nodes.size(), invalid);
  uint64_t retainedRenderables = 0;
  for (uint32_t id = 0; id < base.nodeCount(); ++id) {
    const auto& node = *base.getNode(id);
    retainedRenderables += node.renderable;
    if (node.sourceMeshIndex != -1 || node.sourcePrimitiveIndex != -1) {
      if (node.sourceMeshIndex < 0 || size_t(node.sourceMeshIndex) >= source.meshes.size() ||
          node.sourcePrimitiveIndex < 0 ||
          size_t(node.sourcePrimitiveIndex) >= source.meshes[size_t(node.sourceMeshIndex)].primitives.size())
        fail("retained source primitive identity is invalid");
      const auto primitive = primitiveBases[size_t(node.sourceMeshIndex)] + size_t(node.sourcePrimitiveIndex);
      if (sourcePrimitiveToRuntime[primitive] != node.primitiveIndex)
        fail("retained source primitive disagrees with the runtime mapping");
    }
    if (node.editorSceneRoot) {
      if (result.editorRoot != invalid || node.renderable || node.sourceNodeIndex != -1)
        fail("retained editor root is ambiguous, renderable or source-bound");
      result.editorRoot = id;
    }
    if (node.sourceNodeIndex == -1) continue;
    if (node.sourceNodeIndex < 0 || size_t(node.sourceNodeIndex) >= retainedSourceNodeCount ||
        result.sourceNodeToGraph[size_t(node.sourceNodeIndex)] != invalid)
      fail("retained source group identity is invalid or duplicated");
    result.sourceNodeToGraph[size_t(node.sourceNodeIndex)] = id;
  }
  if (!base.nodeCount() && retainedSourceNodeCount) fail("empty graph cannot retain source nodes");
  if (base.nodeCount() && result.editorRoot == invalid) fail("retained graph has no editor root");
  if (std::ranges::find(std::span(result.sourceNodeToGraph).first(retainedSourceNodeCount), invalid) !=
      std::span(result.sourceNodeToGraph).first(retainedSourceNodeCount).end())
    fail("retained graph is missing a source group");

  std::vector<uint32_t> activeRoots = base.activeRoots() ? *base.activeRoots() : base.rootNodes();
  if (result.editorRoot != invalid) {
    // PreparedAppend permits this exact refinement for context-only editor
    // roots. Use original children, never the newly appended import wrapper.
    if (std::ranges::find(activeRoots, result.editorRoot) != activeRoots.end()) {
      std::vector<uint32_t> refined;
      std::vector<uint8_t> seen(base.nodeCount(), 0);
      const auto append = [&](uint32_t id) {
        if (!seen[id]) { seen[id] = 1; refined.push_back(id); }
      };
      for (const auto id : activeRoots) {
        const auto& node = *base.getNode(id);
        if (node.editorSceneRoot && !node.renderable && node.sourceNodeIndex == -1)
          for (const auto child : node.children) append(child);
        else append(id);
      }
      activeRoots = std::move(refined);
    }
    std::vector<uint8_t> activeRoot(base.nodeCount(), 0);
    for (const auto root : activeRoots) activeRoot[root] = 1;
    for (auto parent = result.editorRoot; parent != invalid; parent = base.getNode(parent)->parent)
      if (activeRoot[parent]) fail("active retained ancestor would activate canonical incoming content");
  }

  // This generates source groups and synthetic instance/primitive children
  // offline. Its old import wrapper and every retained generated node are
  // deliberately discarded; live edited nodes remain the authoritative prefix.
  auto generated = BuildGltfSceneHierarchy(source, mergedInstancing, primitiveMaterialIndices,
      importTransform, limits.hierarchy, GltfHierarchyScope::CompleteAsset, sourcePrimitiveToRuntime);
  std::vector<uint8_t> take(generated.nodeCount(), 0);
  std::vector<uint32_t> generatedSource(source.nodes.size(), invalid);
  size_t appendedNodes = 1 + size_t(base.nodeCount() == 0);
  uint64_t appendedRenderables = 0;
  for (uint32_t id = 0; id < generated.nodeCount(); ++id) {
    const auto& node = *generated.getNode(id);
    if (node.sourceNodeIndex >= 0) {
      const auto sourceId = size_t(node.sourceNodeIndex);
      if (sourceId >= source.nodes.size() || generatedSource[sourceId] != invalid)
        fail("generated source group identity is invalid or duplicated");
      generatedSource[sourceId] = id;
      take[id] = sourceId >= retainedSourceNodeCount;
    } else if (node.meshOwnerGraphNode != invalid) {
      const auto* owner = generated.getNode(node.meshOwnerGraphNode);
      if (!owner || owner->sourceNodeIndex < 0) fail("generated primitive has no source owner");
      take[id] = size_t(owner->sourceNodeIndex) >= retainedSourceNodeCount;
    }
    if (take[id]) { ++appendedNodes; appendedRenderables += node.renderable; }
  }
  if (std::ranges::find(generatedSource, invalid) != generatedSource.end())
    fail("complete generated hierarchy is missing a source group");
  if (appendedNodes > graphLimit - base.nodeCount() ||
      retainedRenderables > limits.hierarchy.maxRenderableObjects ||
      appendedRenderables > limits.hierarchy.maxRenderableObjects - retainedRenderables)
    fail("appended authoring graph exceeds its aggregate expansion budget");
  uint64_t placedRenderables = 0;
  std::vector<uint32_t> work{generatedSource[parentSource]};
  while (!work.empty()) {
    const auto id = work.back(); work.pop_back();
    if (!take[id]) fail("placed hierarchy references a retained generated node");
    const auto& node = *generated.getNode(id);
    placedRenderables += node.renderable;
    work.insert(work.end(), node.children.begin(), node.children.end());
  }
  const auto activeCount = base.renderableNodes().size();
  if (activeCount > limits.maxActiveRenderableObjects ||
      placedRenderables > limits.maxActiveRenderableObjects - activeCount)
    fail("selected placement exceeds the active object budget");

  const auto editorWorld = result.editorRoot == invalid ? glm::mat4(1)
                                                       : base.getNode(result.editorRoot)->worldTransform;
  const auto importWorld = editorWorld * importTransform;
  if (!finite(importWorld)) fail("import wrapper world transform exceeds the finite runtime range");
  std::vector<glm::mat4> appendedWorlds(generated.nodeCount(), glm::mat4(1));
  for (uint32_t id = 0; id < generated.nodeCount(); ++id) {
    if (!take[id]) continue;
    const auto& node = *generated.getNode(id);
    if (node.parent != invalid && take[node.parent] && node.parent >= id)
      fail("generated hierarchy is not in parent-before-child order");
    const auto parentWorld = node.parent != invalid && take[node.parent]
        ? appendedWorlds[node.parent] : importWorld;
    appendedWorlds[id] = parentWorld * node.localTransform;
    if (!finite(appendedWorlds[id])) fail("appended world transform exceeds the finite runtime range");
    if (node.sourceInstanceIndex >= 0 && !finite(glm::inverse(appendedWorlds[id])))
      fail("editor-composed instance world is singular and requires geometry baking");
  }

  // Plan all final IDs/topology before cloning the retained graph once. Bulk
  // assembly avoids per-node global-root erasure, child growth and ancestry
  // walks for both very wide and very deep appended source forests.
  const auto first = uint32_t(base.nodeCount());
  std::vector<SceneNode> tail;
  tail.reserve(appendedNodes);
  if (!base.nodeCount()) {
    result.editorRoot = first;
    SceneNode editor; editor.name = "Scene Root"; editor.editorSceneRoot = true;
    tail.push_back(std::move(editor));
  }
  result.importRoot = first + uint32_t(tail.size());
  SceneNode wrapper; wrapper.name = "Import Scale"; wrapper.interactionRootBoundary = true;
  wrapper.localTransform = importTransform; wrapper.worldTransform = importWorld;
  wrapper.parent = result.editorRoot;
  tail.push_back(std::move(wrapper));
  if (!base.nodeCount()) tail[0].children.push_back(result.importRoot);
  std::vector<uint32_t> remap(generated.nodeCount(), invalid);
  uint32_t next = first + uint32_t(tail.size());
  for (uint32_t id = 0; id < generated.nodeCount(); ++id)
    if (take[id]) remap[id] = next++;
  tail[result.importRoot - first].children.reserve(appendedNodes);
  for (uint32_t id = 0; id < generated.nodeCount(); ++id) {
    if (!take[id]) continue;
    const auto& original = *generated.getNode(id);
    auto payload = original;
    payload.parent = original.parent != invalid && take[original.parent]
        ? remap[original.parent] : result.importRoot;
    payload.children.clear(); payload.children.reserve(original.children.size());
    for (const auto child : original.children) {
      if (!take[child]) fail("new source node would adopt retained generated children");
      payload.children.push_back(remap[child]);
    }
    if (payload.parent == result.importRoot)
      tail[result.importRoot - first].children.push_back(remap[id]);
    payload.meshOwnerGraphNode = invalid;
    if (original.meshOwnerGraphNode != invalid) {
      if (!take[original.meshOwnerGraphNode]) fail("new primitive would adopt a retained owner");
      payload.meshOwnerGraphNode = remap[original.meshOwnerGraphNode];
    }
    payload.worldTransform = appendedWorlds[id];
    tail.push_back(std::move(payload));
    if (original.sourceNodeIndex >= 0)
      result.sourceNodeToGraph[size_t(original.sourceNodeIndex)] = remap[id];
  }
  result.graph = base.makeAppendCandidate(std::move(tail));
  result.placementRoot = result.sourceNodeToGraph[parentSource];
  activeRoots.push_back(result.placementRoot);
  result.graph.setActiveRoots(activeRoots);
  result.sceneRoots.reserve(catalog.entries.size());
  for (const auto& scene : catalog.entries) {
    auto& roots = result.sceneRoots.emplace_back();
    roots.reserve(scene.sourceRoots.size());
    for (const auto sourceRoot : scene.sourceRoots) roots.push_back(result.sourceNodeToGraph[size_t(sourceRoot)]);
  }
  for (const auto id : generated.rootNodes()) {
    const auto& node = *generated.getNode(id);
    if (take[id] && node.sourceNodeIndex != plan.parentNode) result.canonicalRoots.push_back(remap[id]);
    // A generated import wrapper is the only other possible global root.
    if (!take[id] && node.sourceNodeIndex == -1)
      for (const auto child : node.children)
        if (take[child] && generated.getNode(child)->sourceNodeIndex != plan.parentNode)
          result.canonicalRoots.push_back(remap[child]);
  }
  return result;
}
} // namespace container::scene
