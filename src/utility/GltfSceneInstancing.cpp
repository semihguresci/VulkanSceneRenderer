#include <Container/utility/GltfSceneInstancing.h>
#include <Container/geometry/GltfNodeFlags.h>
#include <Container/utility/SourcePrimitiveMapping.h>

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <string>

namespace container::scene {
namespace {

[[noreturn]] void fail(const GltfMeshAttachment& input, const char* detail) {
  throw std::invalid_argument("glTF mesh attachment owner[" +
      std::to_string(input.ownerNode) + "] source node[" +
      std::to_string(input.sourceNodeIndex) + "]: " + detail);
}

bool finiteAffine(const glm::mat4& matrix) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(matrix[column][row])) return false;
  return matrix[0][3] == 0.0f && matrix[1][3] == 0.0f &&
      matrix[2][3] == 0.0f && matrix[3][3] == 1.0f;
}

glm::mat4 ownerWorldFromLocals(const SceneGraph& graph,
                             const GltfMeshAttachment& input) {
  // Graph population need not have updated cached world transforms yet.
  // Validate in the same root-to-child multiplication order as SceneGraph.
  std::vector<uint32_t> ancestors;
  uint32_t current = input.ownerNode;
  while (current != SceneGraph::kInvalidNode) {
    const auto* node = graph.getNode(current);
    if (!node || ancestors.size() >= graph.nodeCount())
      fail(input, "owner ancestry is invalid or cyclic");
    if (!finiteAffine(node->localTransform))
      fail(input, "owner ancestry must contain finite affine transforms");
    ancestors.push_back(current);
    current = node->parent;
  }
  glm::mat4 world(1.0f);
  for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
    world *= graph.getNode(*it)->localTransform;
    if (!finiteAffine(world)) fail(input, "owner world transform exceeds finite affine range");
  }
  return world;
}

struct PlannedPrimitive {
  glm::mat4 localTransform{1.0f};
  uint32_t primitive{}, material{};
  int32_t sourcePrimitive{-1};
  std::string name;
};

// The complete hierarchy builder already has a validated parent-composed
// owner world. Public standalone attachment still recomposes local ancestry,
// since its caller need not have refreshed the graph's cached world matrices.
std::vector<AttachedGltfPrimitive> attachGltfMeshPrimitives(
    SceneGraph& graph, const GltfMeshAttachment& input,
    const GltfMeshAttachmentLimits& limits, const glm::mat4* validatedOwnerWorld) {
  const auto* owner = graph.getNode(input.ownerNode);
  if (!owner || owner->renderable) fail(input, "owner must be an existing nonrenderable group");
  if (input.sourceNodeIndex < -1 || input.sourceMeshIndex < -1)
    fail(input, "source identities must be nonnegative or unmapped (-1)");
  if (owner->sourceNodeIndex != input.sourceNodeIndex)
    fail(input, "owner source node identity differs from attachment");
  const uint64_t primitiveCount = input.materialIndices.size();
  if (!primitiveCount || primitiveCount > uint64_t(std::numeric_limits<int32_t>::max()))
    fail(input, "mesh must contain a representable positive primitive count");
  const uint64_t primitiveBound = std::min<uint64_t>(limits.primitiveRangeCount,
      SceneGraph::kInvalidNode);
  if (!input.runtimePrimitiveIndices.empty())
    ValidateSourcePrimitiveIds(input.runtimePrimitiveIndices, size_t(primitiveCount), primitiveBound);
  else if (input.primitiveBase > primitiveBound ||
      primitiveCount > primitiveBound - input.primitiveBase)
    fail(input, "primitive range exceeds the geometry table or uint32 IDs");
  const uint64_t materialBound = std::min<uint64_t>(limits.runtimeMaterialCount,
      SceneGraph::kInvalidNode);
  for (const auto material : input.materialIndices)
    if (material >= materialBound) fail(input, "runtime material ID exceeds its table");

  uint64_t instanceCount = 1;
  if (const auto* instances = input.instancing) {
    if (input.sourceNodeIndex < 0 || input.sourceMeshIndex < 0 ||
        instances->sourceNode != uint32_t(input.sourceNodeIndex) ||
        instances->sourceMesh != uint32_t(input.sourceMeshIndex) ||
        instances->primitiveCount != primitiveCount)
      fail(input, "instancing source identity or primitive count differs from attachment");
    instanceCount = instances->localMatrices.size();
    if (!instanceCount || instanceCount > uint64_t(std::numeric_limits<int32_t>::max()))
      fail(input, "instance count is zero or exceeds uint32 source identity");
  }
  // Primitive and instance counts above are at most uint32, so their product
  // fits uint64. Check every aggregate bound before staging dense output.
  const uint64_t added = primitiveCount * instanceCount;
  const uint64_t graphBound = std::min<uint64_t>(limits.maxGraphNodes,
      SceneGraph::kInvalidNode);
  if (graph.nodeCount() > graphBound || added > graphBound - graph.nodeCount())
    fail(input, "expanded graph exceeds its node limit or uint32 IDs");
  const uint64_t renderables = graph.renderableNodes().size();
  if (renderables > limits.maxRenderableObjects ||
      added > limits.maxRenderableObjects - renderables)
    fail(input, "expanded mesh exceeds the aggregate renderable object limit");
  if (added > std::numeric_limits<size_t>::max() ||
      owner->children.size() > std::numeric_limits<size_t>::max() - size_t(added))
    fail(input, "expanded mesh exceeds addressable storage");

  const auto ownerWorld = validatedOwnerWorld ? *validatedOwnerWorld
                                             : ownerWorldFromLocals(graph, input);
  if (!finiteAffine(ownerWorld)) fail(input, "owner world transform exceeds finite affine range");
  if (input.instancing)
    for (const auto& local : input.instancing->localMatrices)
      if (!finiteAffine(local) || !finiteAffine(ownerWorld * local))
        fail(input, "instance local/world transform exceeds finite affine range");

  std::vector<PlannedPrimitive> planned;
  std::vector<AttachedGltfPrimitive> result;
  planned.reserve(size_t(added));
  result.reserve(size_t(added));
  const auto firstNode = uint32_t(graph.nodeCount());
  for (uint32_t instance = 0; uint64_t(instance) < instanceCount; ++instance) {
    for (uint32_t primitive = 0; uint64_t(primitive) < primitiveCount; ++primitive) {
      const uint32_t runtimePrimitive = input.runtimePrimitiveIndices.empty()
          ? input.primitiveBase + primitive : input.runtimePrimitiveIndices[primitive];
      auto name = "Primitive " + std::to_string(runtimePrimitive);
      std::optional<geometry::gltf::SourceInstanceIdentity> identity;
      if (input.instancing) {
        identity = input.instancing->identity(instance, primitive);
        name = "Node " + std::to_string(input.sourceNodeIndex) + " " + name +
            " Instance " + std::to_string(instance);
      }
      planned.push_back({input.instancing ? input.instancing->localMatrices[instance] : glm::mat4(1.0f),
          runtimePrimitive, input.materialIndices[primitive],
          input.sourceMeshIndex >= 0 ? int32_t(primitive) : -1, std::move(name)});
      result.push_back({firstNode + uint32_t(result.size()), identity});
    }
  }
  // Avoid repeated exact-capacity allocations in setParent for a large batch.
  // All names, identities and validation are prepared before topology changes.
  graph.getNode(input.ownerNode)->children.reserve(owner->children.size() + size_t(added));
  for (auto& child : planned) {
    const auto nodeIndex = graph.createNode(child.localTransform, child.material,
        true, child.primitive, std::move(child.name));
    if (!graph.setParent(nodeIndex, input.ownerNode))
      throw std::logic_error("validated glTF mesh attachment could not parent a new primitive");
    auto& node = *graph.getNode(nodeIndex);
    node.meshOwnerGraphNode = input.ownerNode;
    node.sourceMeshIndex = input.sourceMeshIndex;
    node.sourcePrimitiveIndex = child.sourcePrimitive;
    if (result[nodeIndex-firstNode].sourceInstance)
      node.sourceInstanceIndex = int32_t(result[nodeIndex-firstNode].sourceInstance->sourceInstance);
  }
  return result;
}

} // namespace

std::vector<AttachedGltfPrimitive> AttachGltfMeshPrimitives(
    SceneGraph& graph, const GltfMeshAttachment& input,
    const GltfMeshAttachmentLimits& limits) {
  return attachGltfMeshPrimitives(graph, input, limits, nullptr);
}

SceneGraph BuildGltfSceneHierarchy(const tinygltf::Model& source,
    const geometry::gltf::GltfInstancingData& instancing,
    std::span<const uint32_t> primitiveMaterialIndices,
    const glm::mat4& importTransform, const GltfMeshAttachmentLimits& limits,
    GltfHierarchyScope scope, std::span<const uint32_t> sourcePrimitiveToRuntime) {
  const auto reject = [](const char* message) { throw std::invalid_argument(std::string("glTF scene hierarchy: ") + message); };
  if (!finiteAffine(importTransform)) reject("import transform must be finite affine");
  const auto nodeFlags = geometry::gltf::ParseGltfNodeFlags(source);
  std::vector<uint32_t> bases(source.meshes.size());
  uint64_t primitiveCount = 0;
  for (size_t mesh=0;mesh<source.meshes.size();++mesh) {
    if (primitiveCount >= SceneGraph::kInvalidNode) reject("primitive identity exceeds uint32 range");
    bases[mesh] = uint32_t(primitiveCount);
    primitiveCount += source.meshes[mesh].primitives.size();
  }
  if (primitiveCount > primitiveMaterialIndices.size()) reject("mesh/material primitive table counts disagree");
  const auto mappingCount = source.meshes.empty() ? primitiveMaterialIndices.size() : size_t(primitiveCount);
  if (!sourcePrimitiveToRuntime.empty())
    ValidateSourcePrimitiveIds(sourcePrimitiveToRuntime, mappingCount, limits.primitiveRangeCount);
  SceneGraph graph;
  if (primitiveMaterialIndices.empty() && source.nodes.empty()) return graph;
  std::optional<uint32_t> importRoot;
  if (source.nodes.empty() || importTransform != glm::mat4(1.0f))
    importRoot = graph.createNode(importTransform,0,false,SceneGraph::kInvalidNode,"Import Scale");
  if (importRoot) {
    graph.getNode(*importRoot)->worldTransform=importTransform;
    graph.getNode(*importRoot)->interactionRootBoundary=true;
  }
  if (source.nodes.empty()) {
    if (!source.meshes.empty()) {
      for (size_t mesh=0;mesh<source.meshes.size();++mesh)
        (void)attachGltfMeshPrimitives(graph,{*importRoot,-1,int32_t(mesh),bases[mesh],
            primitiveMaterialIndices.subspan(bases[mesh],source.meshes[mesh].primitives.size()),nullptr,
            sourcePrimitiveToRuntime.empty() ? std::span<const uint32_t>{} :
                sourcePrimitiveToRuntime.subspan(bases[mesh],source.meshes[mesh].primitives.size())},limits,&importTransform);
    } else
      (void)attachGltfMeshPrimitives(graph,{*importRoot,-1,-1,0,primitiveMaterialIndices,nullptr,sourcePrimitiveToRuntime},limits,&importTransform);
    graph.updateWorldTransforms();
    return graph;
  }
  const auto localTransform = [&](const tinygltf::Node& node) {
    glm::mat4 result(1);
    if (!node.matrix.empty()) {
      if (node.matrix.size()!=16 || !node.translation.empty() || !node.rotation.empty() || !node.scale.empty()) reject("node matrix/TRS schema is invalid");
      for (int c=0;c<4;++c) for(int r=0;r<4;++r) result[c][r]=float(node.matrix[size_t(c*4+r)]);
    } else {
      if ((!node.translation.empty() && node.translation.size()!=3) ||
          (!node.rotation.empty() && node.rotation.size()!=4) ||
          (!node.scale.empty() && node.scale.size()!=3)) reject("node TRS size is invalid");
      if (!node.translation.empty()) result=glm::translate(result,glm::vec3(node.translation[0],node.translation[1],node.translation[2]));
      if (!node.rotation.empty()) {
        const glm::quat rotation{float(node.rotation[3]),float(node.rotation[0]),float(node.rotation[1]),float(node.rotation[2])};
        const auto length=glm::length(rotation);
        if (!std::isfinite(length) || length==0) reject("node quaternion is invalid");
        result *= glm::mat4_cast(rotation/length);
      }
      if (!node.scale.empty()) result=glm::scale(result,glm::vec3(node.scale[0],node.scale[1],node.scale[2]));
    }
    if (!finiteAffine(result)) reject("node transform exceeds finite affine range");
    return result;
  };
  std::vector<int> roots;
  if (!source.scenes.empty() && scope==GltfHierarchyScope::ActiveScene) {
    const auto scene=source.defaultScene>=0 ? size_t(source.defaultScene) : 0;
    if (scene>=source.scenes.size()) reject("default scene index is invalid");
    roots=source.scenes[scene].nodes;
  } else {
    std::vector<bool> child(source.nodes.size());
    for (const auto& node:source.nodes) for(int id:node.children) {
      if (id<0 || size_t(id)>=source.nodes.size()) reject("child index is invalid");
      if(child[size_t(id)]) reject("source node has multiple parent references");
      child[size_t(id)]=true;
    }
    for(size_t i=0;i<child.size();++i) if(!child[i]) roots.push_back(int(i));
    if (roots.empty()) reject("node hierarchy has no root");
  }
  struct Pending { int source; std::optional<uint32_t> parent; };
  std::vector<Pending> stack;
  for(auto it=roots.rbegin();it!=roots.rend();++it) stack.push_back({*it,importRoot});
  std::vector<bool> visited(source.nodes.size());
  while (!stack.empty()) {
    const auto pending=stack.back(); stack.pop_back();
    if(pending.source<0 || size_t(pending.source)>=source.nodes.size()) reject("active node index is invalid");
    if(visited[size_t(pending.source)]) reject("active hierarchy contains a cycle or repeated node");
    visited[size_t(pending.source)]=true;
    if(graph.nodeCount()>=std::min<uint64_t>(limits.maxGraphNodes,SceneGraph::kInvalidNode)) reject("graph node budget exceeded");
    const auto& node=source.nodes[size_t(pending.source)];
    const auto local=localTransform(node);
    const auto parentWorld=pending.parent ? graph.getNode(*pending.parent)->worldTransform : glm::mat4(1);
    const auto world=parentWorld*local;
    if(!finiteAffine(world)) reject("node world transform exceeds finite affine range");
    const auto id=graph.createNode(local,0,false,SceneGraph::kInvalidNode,node.name.empty()?"glTF Node "+std::to_string(pending.source):node.name);
    graph.getNode(id)->sourceNodeIndex=pending.source;
    graph.setParent(id,pending.parent);
    graph.getNode(id)->worldTransform=world;
    const auto& flags=nodeFlags.nodes[size_t(pending.source)];
    graph.setVisible(id,flags.visible);
    graph.setSelectable(id,flags.selectable);
    graph.setHoverable(id,flags.hoverable);
    if(node.mesh>=0) {
      if(size_t(node.mesh)>=source.meshes.size()) reject("mesh index is invalid");
      const auto base=bases[size_t(node.mesh)];
      const auto count=source.meshes[size_t(node.mesh)].primitives.size();
      const auto* instances=geometry::gltf::FindNodeInstancing(instancing,uint32_t(pending.source));
      if(instances) for(uint32_t i=0;i<instances->localMatrices.size();++i) {
        const auto transform=geometry::gltf::ComposeInstanceWorld(*instances,i,world);
        // Vulkan TLAS transforms must be invertible. Refuse this documented
        // partial case during detached preflight, before retiring live assets.
        const auto inverse=glm::inverse(transform);
        for(int c=0;c<4;++c) for(int r=0;r<4;++r)
          if(!std::isfinite(inverse[c][r])) reject("singular instancing transforms require geometry baking before Vulkan raster/ray publication");
      }
      (void)attachGltfMeshPrimitives(graph,{id,pending.source,node.mesh,base,
          primitiveMaterialIndices.subspan(base,count),instances,sourcePrimitiveToRuntime.empty()
              ? std::span<const uint32_t>{} : sourcePrimitiveToRuntime.subspan(base,count)},limits,&world);
    }
    for(auto it=node.children.rbegin();it!=node.children.rend();++it) stack.push_back({*it,id});
  }
  if(scope==GltfHierarchyScope::CompleteAsset &&
      std::ranges::any_of(visited,[](bool value) { return !value; }))
    reject("complete source hierarchy contains an unreachable cycle");
  graph.updateWorldTransforms();
  return graph;
}

} // namespace container::scene
