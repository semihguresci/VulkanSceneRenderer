#include "Container/renderer/scene/SceneController.h"
#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/MaterialBindingAuthoring.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::ObjectData;

namespace {
using PoseLimits=SceneController::GraphPoseGenerationLimits;
struct PoseBudget {
  const PoseLimits& limits;uint64_t cpu{},gpu{},work{};
  void bytes(uint64_t n,uint64_t width=1) {
    if(width && n>(limits.maxCpuBytes-cpu)/width) throw std::length_error("Graph pose generation exceeds its CPU storage limit");
    cpu+=n*width;
  }
  void visit(uint64_t n=1) {
    if(n>limits.maxWork-work) throw std::length_error("Graph pose generation exceeds its work limit");work+=n;
  }
  void deviceBytes(uint64_t n,uint64_t width=1) {
    if(width && n>(limits.maxGpuBytes-gpu)/width) throw std::length_error("Graph pose generation exceeds its GPU payload limit");
    gpu+=n*width;
  }
};
bool poseMatrixFinite(const glm::mat4& value) noexcept {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(!std::isfinite(value[c][r])) return false;return true;
}
bool poseMatrixSame(const glm::mat4& a,const glm::mat4& b) noexcept {
  for(int c=0;c<4;++c) for(int r=0;r<4;++r)
    if(std::bit_cast<uint32_t>(a[c][r])!=std::bit_cast<uint32_t>(b[c][r])) return false;return true;
}
bool poseNodeStructureSame(const container::scene::SceneNode& a,const container::scene::SceneNode& b) noexcept {
  return a.parent==b.parent && a.materialIndex==b.materialIndex && a.primitiveIndex==b.primitiveIndex &&
      a.sourceNodeIndex==b.sourceNodeIndex && a.editorSceneRoot==b.editorSceneRoot && a.meshOwnerGraphNode==b.meshOwnerGraphNode &&
      a.renderable==b.renderable && a.visible==b.visible && a.name==b.name && a.children==b.children &&
      a.sourceMeshIndex==b.sourceMeshIndex && a.sourcePrimitiveIndex==b.sourcePrimitiveIndex && a.sourceInstanceIndex==b.sourceInstanceIndex &&
      a.selectable==b.selectable && a.hoverable==b.hoverable && a.interactionRootBoundary==b.interactionRootBoundary && a.retired==b.retired;
}
struct PoseGraphWitness {
  uint64_t identity{},revision{},hierarchy{};
  std::vector<container::scene::SceneNode> nodes;
  std::vector<uint64_t> incarnations;
  std::vector<uint32_t> roots;
  std::optional<std::vector<uint32_t>> active;
  void capture(const container::scene::SceneGraph& graph) {
    identity=graph.identity();revision=graph.revision();hierarchy=graph.hierarchyRevision();roots=graph.rootNodes();active=graph.activeRoots();
    nodes.reserve(graph.nodeCount());incarnations.reserve(graph.nodeCount());
    for(uint32_t i=0;i<graph.nodeCount();++i) {nodes.push_back(*graph.getNode(i));incarnations.push_back(graph.nodeIncarnation(i));}
  }
  bool matches(const container::scene::SceneGraph& graph,bool original) const noexcept {
    if((original && (graph.identity()!=identity || graph.revision()!=revision || graph.hierarchyRevision()!=hierarchy)) ||
        graph.nodeCount()!=nodes.size() || graph.rootNodes()!=roots || graph.activeRoots()!=active) return false;
    for(uint32_t i=0;i<nodes.size();++i) {
      const auto* row=graph.getNode(i);
      if(!row || graph.nodeIncarnation(i)!=incarnations[i] || !poseNodeStructureSame(nodes[i],*row) ||
          !poseMatrixSame(nodes[i].localTransform,row->localTransform) || !poseMatrixSame(nodes[i].worldTransform,row->worldTransform)) return false;
    }
    return true;
  }
};
struct PoseWeightWitness {
  struct Node {int mesh{},skin{};std::vector<double> weights;};
  std::vector<Node> nodes;
  std::vector<std::vector<double>> meshes;
  void capture(const tinygltf::Model& source) {
    nodes.reserve(source.nodes.size());meshes.reserve(source.meshes.size());
    for(const auto& node:source.nodes) nodes.push_back({node.mesh,node.skin,node.weights});
    for(const auto& mesh:source.meshes) meshes.push_back(mesh.weights);
  }
  bool matches(const tinygltf::Model& source) const noexcept {
    if(nodes.size()!=source.nodes.size() || meshes.size()!=source.meshes.size()) return false;
    const auto same=[](const std::vector<double>& a,const std::vector<double>& b) noexcept {
      return a.size()==b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(double))==0);
    };
    for(size_t i=0;i<nodes.size();++i) if(nodes[i].mesh!=source.nodes[i].mesh || nodes[i].skin!=source.nodes[i].skin || !same(nodes[i].weights,source.nodes[i].weights)) return false;
    for(size_t i=0;i<meshes.size();++i) if(!same(meshes[i],source.meshes[i].weights)) return false;
    return true;
  }
};
void poseGraphPreflight(const container::scene::SceneGraph& live,const container::scene::SceneGraph& candidate,PoseBudget& budget) {
  const auto count=live.nodeCount();
  if(count>budget.limits.maxNodes || count>=UINT32_MAX || candidate.nodeCount()!=count ||
      candidate.rootNodes()!=live.rootNodes() || candidate.activeRoots()!=live.activeRoots() ||
      candidate.hierarchyRevision()!=live.hierarchyRevision()) throw std::invalid_argument("Graph pose generation cannot change hierarchy or active roots");
  budget.bytes(count,sizeof(container::scene::SceneNode)*4+512);budget.bytes(live.rootNodes().size(),32);
  if(live.activeRoots()) budget.bytes(live.activeRoots()->size(),32);
  uint64_t edges=0,objects=0;
  for(uint32_t i=0;i<count;++i) {
    budget.visit();const auto& before=*live.getNode(i);const auto& after=*candidate.getNode(i);
    if(before.children.size()>budget.limits.maxEdges-edges) throw std::length_error("Graph pose generation exceeds its edge limit");
    edges+=before.children.size();budget.visit(before.children.size()+before.name.size());budget.bytes(before.name.size(),4);budget.bytes(before.children.size(),32);
    if(!live.nodeIncarnation(i) || live.nodeIncarnation(i)!=candidate.nodeIncarnation(i) || !poseNodeStructureSame(before,after) ||
        !poseMatrixFinite(before.localTransform) || !poseMatrixFinite(before.worldTransform) ||
        !poseMatrixFinite(after.localTransform) || !poseMatrixFinite(after.worldTransform))
      throw std::invalid_argument("Graph pose generation accepts only finite transforms of the original node lifetimes");
    for(const auto* graph:{&live,&candidate}) {
      const auto& node=*graph->getNode(i);
      if(node.parent!=container::scene::SceneGraph::kInvalidNode && node.parent>=count) throw std::invalid_argument("Graph pose generation has an invalid parent");
      const auto world=(node.parent==container::scene::SceneGraph::kInvalidNode?glm::mat4(1):graph->getNode(node.parent)->worldTransform)*node.localTransform;
      if(!poseMatrixSame(world,node.worldTransform)) throw std::invalid_argument("Graph pose generation requires current exact graph worlds");
    }
    if(before.renderable) ++objects;
    for(const auto child:before.children) if(child>=count || live.getNode(child)->parent!=i)
      throw std::invalid_argument("Graph pose generation has an inconsistent forest");
  }
  if(objects>budget.limits.maxObjects) throw std::length_error("Graph pose generation exceeds its object limit");
  // Bounded forest validation before any graph copy or derived-state upload.
  std::vector<uint8_t> seen(count);std::vector<uint32_t> stack;stack.reserve(count);
  for(const auto root:live.rootNodes()) {
    if(root>=count || live.getNode(root)->parent!=container::scene::SceneGraph::kInvalidNode) throw std::invalid_argument("Graph pose generation has an invalid root");
    stack.push_back(root);
  }
  size_t visited=0;
  while(!stack.empty()) {
    const auto node=stack.back();stack.pop_back();budget.visit();
    if(seen[node]) throw std::invalid_argument("Graph pose generation has a cyclic or shared forest");seen[node]=1;++visited;
    for(const auto child:live.getNode(node)->children) stack.push_back(child);
  }
  if(visited!=count) throw std::invalid_argument("Graph pose generation has detached or missing forest nodes");
}
void poseSourcePreflight(const container::scene::SceneManager& manager,const container::scene::SceneGraph& graph,
    const container::ecs::World& world,uint32_t images,bool diagnostic,PoseBudget& budget) {
  const auto& source=manager.animationSource();const auto& meshes=manager.model().meshes();const auto* data=manager.deformationData();
  if(source.nodes.size()>budget.limits.maxNodes || source.meshes.size()>budget.limits.maxObjects || meshes.size()>budget.limits.maxObjects)
    throw std::length_error("Graph pose source tables exceed their finite capacities");
  budget.bytes(source.nodes.size(),sizeof(PoseWeightWitness::Node)+128);budget.bytes(source.meshes.size(),sizeof(std::vector<double>)+64);
  for(const auto& node:source.nodes) {budget.bytes(node.weights.size(),sizeof(double)*2);budget.visit(node.weights.size()+1);}
  for(const auto& mesh:source.meshes) {budget.bytes(mesh.weights.size(),sizeof(double)*2);budget.visit(mesh.weights.size()+1);}
  uint64_t vertices=manager.vertices().size(),indices=manager.indices().size(),joints=0,objects=diagnostic?1:0;
  if(vertices>budget.limits.maxVertices || indices>budget.limits.maxIndices) throw std::length_error("Graph pose source geometry exceeds its finite capacities");
  if(data) {
    if(data->primitives.size()>meshes.size() || data->nodes.size()>budget.limits.maxNodes || data->skins.size()>budget.limits.maxNodes ||
        (!data->primitives.empty() && (manager.runtimePrimitiveToSource().size()!=meshes.size() ||
          manager.sourcePrimitiveToRuntime().size()!=data->primitives.size())))
      throw std::invalid_argument("Graph pose deformation source mappings are incomplete");
    budget.bytes(data->primitives.size(),1024);budget.bytes(data->nodes.size(),256);budget.bytes(data->skins.size(),256);
    for(size_t definition=0;definition<data->primitives.size();++definition) {
      const auto& primitive=data->primitives[definition];
      budget.bytes(primitive.bindVertices.size(),sizeof(container::geometry::Vertex)*4);budget.visit(primitive.bindVertices.size());
      budget.bytes(primitive.influences.size(),sizeof(container::geometry::gltf::VertexSkinInfluences)*4);uint64_t maxInfluences=0;
      for(const auto& vertex:primitive.influences) {budget.bytes(vertex.values.size(),sizeof(container::geometry::gltf::JointInfluence)*4);budget.visit(vertex.values.size()+1);maxInfluences=std::max<uint64_t>(maxInfluences,vertex.values.size());}
      budget.bytes(primitive.morphTargets.size(),256);budget.bytes(primitive.meshDefaultWeights.size(),sizeof(float)*4);
      for(const auto& target:primitive.morphTargets) for(const auto* values:{&target.positions,&target.normals,&target.tangents}) {
        budget.bytes(values->size(),sizeof(glm::vec3)*4);budget.visit(values->size());
      }
      // Missing frames can expand to one independent vertex per triangle
      // corner, including every target and influence. Bound this before the
      // existing frame preparer allocates any corner/Mikk scratch.
      if(primitive.hasMorphing() && (!primitive.authoredBaseNormal || !primitive.authoredBaseTangent)) {
        const auto runtime=manager.sourcePrimitiveToRuntime()[definition];
        if(runtime>=meshes.size()) throw std::invalid_argument("Graph pose missing-frame primitive has no exact runtime source");
        const uint64_t corners=meshes[runtime].indices().size();
        budget.bytes(corners,1024);
        for(const auto& target:primitive.morphTargets) { (void)target;budget.bytes(corners,sizeof(glm::vec3)*12);budget.visit(corners);}
        if(maxInfluences && corners>budget.limits.maxWork/maxInfluences) throw std::length_error("Graph pose corner influence work exceeds its limit");
        budget.bytes(corners*maxInfluences,sizeof(container::geometry::gltf::JointInfluence)*4);budget.visit(corners*maxInfluences);
      }
    }
    for(const auto& skin:data->skins) {joints=std::max<uint64_t>(joints,skin.joints.size());budget.bytes(skin.joints.size(),sizeof(uint32_t)*4);budget.bytes(skin.inverseBindMatrices.size(),sizeof(glm::mat4)*4);budget.visit(skin.joints.size()+skin.inverseBindMatrices.size());}
    for(const auto& node:data->nodes) {budget.bytes(node.defaultMorphWeights.size(),sizeof(float)*4);budget.visit(node.defaultMorphWeights.size()+1);}
  }
  for(uint32_t i=0;i<graph.nodeCount();++i) {
    const auto& node=*graph.getNode(i);budget.visit();if(!node.renderable) continue;++objects;
    if(node.primitiveIndex>=meshes.size() || node.materialIndex>=manager.materialCount()) throw std::invalid_argument("Graph pose object has an invalid primitive/material binding");
    if(!data || data->primitives.empty()) continue;
    const auto definition=manager.runtimePrimitiveToSource()[node.primitiveIndex];if(definition<0) continue;
    if(size_t(definition)>=data->primitives.size()) throw std::invalid_argument("Graph pose deformation definition is invalid");
    const auto& primitive=data->primitives[size_t(definition)];if(!primitive.hasMorphing() && !primitive.hasSkinning()) continue;
    const auto count=primitive.hasMorphing() && (!primitive.authoredBaseNormal || !primitive.authoredBaseTangent)?
        meshes[node.primitiveIndex].indices().size():primitive.bindVertices.size();
    const auto indexCount=meshes[node.primitiveIndex].indices().size();
    if(count>budget.limits.maxVertices-vertices || indexCount>budget.limits.maxIndices-indices) throw std::length_error("Graph pose expanded deformation exceeds its geometry capacities");
    vertices+=count;indices+=indexCount;budget.bytes(joints,sizeof(glm::mat4)*6);budget.bytes(primitive.morphTargets.size(),sizeof(float)*8);
    budget.visit(count+indexCount+joints);
  }
  if(objects>budget.limits.maxObjects) throw std::length_error("Graph pose generation exceeds its object capacity");
  budget.bytes(vertices,sizeof(container::geometry::Vertex)*12);budget.bytes(indices,sizeof(uint32_t)*8);budget.bytes(graph.nodeCount(),256);
  budget.bytes(objects,sizeof(ObjectData)*4+sizeof(DrawCommand)*12+512);
  budget.bytes(images,sizeof(container::gpu::AllocatedBuffer)+128);
  const auto& registry=world.registry();
  for(auto&& [id,pool]:registry.storage()) {
    (void)id;if(!pool.size()) continue;const auto& type=pool.type();
    if(type!=entt::type_id<container::ecs::TransformComponent>() && type!=entt::type_id<container::ecs::MeshComponent>() &&
        type!=entt::type_id<container::ecs::MaterialComponent>() && type!=entt::type_id<container::ecs::LightComponent>() &&
        type!=entt::type_id<container::ecs::CameraComponent>() && type!=entt::type_id<container::ecs::RenderableTag>() &&
        type!=entt::type_id<container::ecs::LightTag>() && type!=entt::type_id<container::ecs::CameraTag>() && type!=entt::type_id<container::ecs::SceneNodeRef>())
      throw std::invalid_argument("Cannot stage an unknown ECS component pool");
    budget.bytes(pool.capacity(),1024);budget.bytes(pool.extent(),sizeof(entt::entity)*2);budget.visit(pool.size());
  }
  if(const auto* entities=registry.storage<entt::entity>()) {
    budget.bytes(entities->capacity(),1024);budget.bytes(entities->extent(),sizeof(entt::entity)*2);budget.visit(entities->size());
    for(auto&& [entity]:entities->each()) {
      if(uint64_t(entt::to_entity(entity))>budget.limits.maxObjects+budget.limits.maxNodes)
        throw std::length_error("Graph pose ECS sparse entity address exceeds its finite capacity");
    }
  }
  budget.deviceBytes(vertices+64,sizeof(container::geometry::Vertex));budget.deviceBytes(indices+128,sizeof(uint32_t));
  budget.deviceBytes(std::max<uint64_t>(1,objects),uint64_t(images)*sizeof(ObjectData));
  budget.bytes(budget.gpu); // Coexisting synchronous upload staging payloads.
}
bool poseWorldSame(const container::ecs::World& a,const container::ecs::World& b) noexcept {
  const auto& left=a.registry();const auto& right=b.registry();
  if(a.entityCount()!=b.entityCount()) return false;
  if(const auto* entities=left.storage<entt::entity>()) for(auto&& [entity]:entities->each()) if(!right.valid(entity)) return false;
  const auto compare=[&]<class T>() {
    const auto* x=left.storage<T>();const auto* y=right.storage<T>();
    if((x?x->size():0)!=(y?y->size():0)) return false;
    if(x) for(const auto entity:left.view<const T>()) {
      if constexpr(std::is_empty_v<T>) {
        if(!right.all_of<T>(entity)) return false;
      } else {
      const auto* other=right.try_get<T>(entity);if(!other) return false;
      if constexpr(!std::is_empty_v<T>) {
        const auto& value=left.get<T>(entity);
        if constexpr(std::is_same_v<T,container::ecs::CameraComponent>) {
          static_assert(sizeof(container::gpu::CameraData)==6*sizeof(glm::mat4)+4*sizeof(glm::vec4)+sizeof(glm::uvec4));
          if(std::memcmp(&value.data,&other->data,sizeof(value.data)) ||
              std::bit_cast<uint32_t>(value.nearPlane)!=std::bit_cast<uint32_t>(other->nearPlane) ||
              std::bit_cast<uint32_t>(value.farPlane)!=std::bit_cast<uint32_t>(other->farPlane)) return false;
        } else if constexpr(std::is_same_v<T,container::ecs::TransformComponent>) {
          if(!poseMatrixSame(value.localTransform,other->localTransform) || !poseMatrixSame(value.worldTransform,other->worldTransform)) return false;
        } else if constexpr(std::is_same_v<T,container::ecs::LightComponent>) {
          static_assert(sizeof(container::gpu::PointLightData)==4*sizeof(glm::vec4));
          if(std::memcmp(&value.data,&other->data,sizeof(value.data))) return false;
        } else if constexpr(std::is_same_v<T,container::ecs::MeshComponent>) {if(value.primitiveIndex!=other->primitiveIndex) return false;}
        else if constexpr(std::is_same_v<T,container::ecs::MaterialComponent>) {if(value.materialIndex!=other->materialIndex) return false;}
        else if constexpr(std::is_same_v<T,container::ecs::SceneNodeRef>) {if(value.nodeIndex!=other->nodeIndex) return false;}
      }
      if constexpr(std::is_same_v<T,container::ecs::CameraComponent>)
        if((&left.get<T>(entity)==a.activeCamera())!=(other==b.activeCamera())) return false;
      }
    }
    return true;
  };
  for(auto&& [id,pool]:right.storage()) {
    const auto* old=left.storage(id);if(pool.size() && (!old || old->type()!=pool.type() || old->size()!=pool.size())) return false;
  }
  return compare.template operator()<container::ecs::TransformComponent>() && compare.template operator()<container::ecs::MeshComponent>() &&
      compare.template operator()<container::ecs::MaterialComponent>() && compare.template operator()<container::ecs::LightComponent>() &&
      compare.template operator()<container::ecs::CameraComponent>() && compare.template operator()<container::ecs::RenderableTag>() &&
      compare.template operator()<container::ecs::LightTag>() && compare.template operator()<container::ecs::CameraTag>() &&
      compare.template operator()<container::ecs::SceneNodeRef>();
}
} // namespace

struct SceneController::OwnedGenerationBuffers {
  container::gpu::AllocationManager& allocations;
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::AllocatedBuffer vertices{}, indices{};
  std::vector<container::gpu::AllocatedBuffer> objects;
  std::vector<size_t> capacities;

  OwnedGenerationBuffers(container::gpu::AllocationManager& owner,
                         std::shared_ptr<container::gpu::VulkanDevice> wrapper)
      : allocations(owner), device(std::move(wrapper)) {}
  ~OwnedGenerationBuffers() {
    // Caller retires readers before published generation destruction, including
    // history eviction. Unpublished uploads finish synchronously. Do not infer
    // safe retirement from an unchecked/failed device-idle call here.
    for (auto& object:objects) allocations.destroyBuffer(object);
    allocations.destroyBuffer(indices); allocations.destroyBuffer(vertices);
  }
  static std::unique_ptr<OwnedGenerationBuffers> geometry(
      container::gpu::AllocationManager& allocations,
      const std::shared_ptr<container::gpu::VulkanDevice>& device,
      std::span<const container::geometry::Vertex> vertices,
      std::span<const uint32_t> indices) {
    auto result=std::make_unique<OwnedGenerationBuffers>(allocations,device);
    result->vertices=allocations.uploadBuffer(std::as_bytes(vertices),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    result->indices=allocations.uploadBuffer(std::as_bytes(indices),VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    return result;
  }
  void createObjects(uint32_t count,std::span<const ObjectData> data) {
    const auto capacity=std::max<size_t>(1,data.size());
    if (capacity>std::numeric_limits<VkDeviceSize>::max()/sizeof(ObjectData))
      throw std::runtime_error("Staged object buffer exceeds its byte range");
    objects.resize(count); capacities.assign(count,capacity);
    for (auto& object:objects) {
      object=allocations.createBuffer(sizeof(ObjectData)*capacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          VMA_MEMORY_USAGE_AUTO,VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT);
      if (!data.empty()) SceneController::writeToBuffer(allocations,object,data.data(),data.size_bytes());
    }
  }
};

struct SceneController::PreparedGeneration::State {
  const SceneController* owner{};
  uint64_t expectedGraphRevision{},expectedSceneEpoch{};
  uint64_t expectedControllerGeometry{},expectedControllerObjects{},expectedControllerUpload{};
  bool graphPose{},poseConsumed{};
  uint64_t sourceGeometry{},sourceMaterial{},sourceEpoch{},sourceAsset{},cpuBytes{};
  const container::geometry::gltf::DeformationData* sourceDeformation{};
  PoseGraphWitness beforePose,nextPose;
  PoseWeightWitness weights;
  std::unique_ptr<container::ecs::World> beforeWorld;
  std::unique_ptr<container::ecs::World> world_{std::make_unique<container::ecs::World>()};
  std::unique_ptr<OwnedGenerationBuffers> ownedGenerationBuffers_;
  std::unique_ptr<OwnedGenerationBuffers> ownedMaterialObjectBuffers_;
  // The first live generation may still use shared arenas/frontend buffers.
  // Prepare an owned undo snapshot without touching those live aliases.
  std::unique_ptr<OwnedGenerationBuffers> initialBuffers;
  std::unordered_map<VkBuffer,uint64_t> initialUploadRevisions;
  std::unique_ptr<GltfDeformationScene> deformationScene_;
  std::vector<container::geometry::Vertex> gpuBindVertices_;
  std::vector<uint32_t> gpuBindIndices_;
  size_t generationSceneVertexCount_{},generationSceneIndexCount_{};
  uint64_t deformationSourceGeometryRevision_{std::numeric_limits<uint64_t>::max()},geometryRevision_{};
  bool deformationTopologyPending_{true};
  std::vector<ObjectData> objectData_;
  std::vector<DrawCommand> opaqueDrawCommands_,transparentDrawCommands_;
  std::vector<DrawCommand> opaqueSingleSidedDrawCommands_,opaqueWindingFlippedDrawCommands_,opaqueDoubleSidedDrawCommands_;
  std::vector<DrawCommand> transparentSingleSidedDrawCommands_,transparentWindingFlippedDrawCommands_,transparentDoubleSidedDrawCommands_;
  std::vector<PrimitiveBounds> primitiveBounds_;
  std::vector<uint32_t> objectNodeIndices_;
  std::unordered_map<VkBuffer,uint64_t> objectBufferUploadRevisions_;
  uint64_t cachedSceneGraphRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t cachedGeometryRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t cachedMaterialRevision_{std::numeric_limits<uint64_t>::max()};
  uint64_t objectDataRevision_{},objectBufferUploadRevision_{};
  bool cachedShowDiagCube_{},objectDataCacheValid_{};
  container::gpu::BufferSlice vertexSlice_{},indexSlice_{},diagCubeVertexSlice_{},diagCubeIndexSlice_{};
  uint32_t diagCubeIndexCount_{},diagCubeObjectIndex_{std::numeric_limits<uint32_t>::max()};
};

struct SceneController::PreparedMaterialRouting::State {
  const SceneController* owner{};
  uint64_t graph{},candidateGraph{},sceneEpoch{},sourceGeometry{},sourceMaterial{},geometry{},objects{},upload{};
  container::scene::MaterialBindingCommand bindings;
  container::scene::MaterialBindingSide bindingTarget{container::scene::MaterialBindingSide::After};
  bool bindingAware{},consumed{};
  size_t ecsRenderableCount{};
  struct EcsBinding {
    entt::entity entity{};
    container::ecs::MaterialComponent* component{};
    const container::ecs::SceneNodeRef* nodeRef{};
    const container::ecs::MeshComponent* mesh{};
    uint32_t node{},primitive{},before{},after{};
  };
  std::vector<EcsBinding> ecsBindings;
  [[nodiscard]] bool ecsMatches(const container::ecs::World& world) const noexcept {
    if(world.renderableCount()!=ecsRenderableCount) return false;
    const auto& registry=world.registry();
    for(const auto& row:ecsBindings) {
      const auto* component=registry.try_get<container::ecs::MaterialComponent>(row.entity);
      const auto* node=registry.try_get<container::ecs::SceneNodeRef>(row.entity);
      const auto* mesh=registry.try_get<container::ecs::MeshComponent>(row.entity);
      if(!registry.valid(row.entity) || !registry.all_of<container::ecs::RenderableTag>(row.entity) ||
          component!=row.component || node!=row.nodeRef || mesh!=row.mesh ||
          !component || !node || !mesh || component->materialIndex!=row.before ||
          node->nodeIndex!=row.node || mesh->primitiveIndex!=row.primitive) return false;
    }
    return true;
  }
  std::unique_ptr<OwnedGenerationBuffers> buffers,initial;
  bool adoptBase{};
  std::unordered_map<VkBuffer,uint64_t> initialUploads,objectBufferUploadRevisions_;
  std::vector<ObjectData> objectData_;
  std::vector<uint32_t> objectNodeIndices_;
  std::vector<DrawCommand> opaqueDrawCommands_,transparentDrawCommands_;
  std::vector<DrawCommand> opaqueSingleSidedDrawCommands_,opaqueWindingFlippedDrawCommands_,opaqueDoubleSidedDrawCommands_;
  std::vector<DrawCommand> transparentSingleSidedDrawCommands_,transparentWindingFlippedDrawCommands_,transparentDoubleSidedDrawCommands_;
  uint64_t cachedSceneGraphRevision_{},cachedGeometryRevision_{},cachedMaterialRevision_{};
  bool cachedShowDiagCube_{},objectDataCacheValid_{true};
  uint32_t diagCubeObjectIndex_{UINT32_MAX};
};
SceneController::PreparedMaterialRouting::PreparedMaterialRouting() noexcept=default;
SceneController::PreparedMaterialRouting::~PreparedMaterialRouting()=default;
SceneController::PreparedMaterialRouting::PreparedMaterialRouting(PreparedMaterialRouting&&) noexcept=default;
SceneController::PreparedMaterialRouting& SceneController::PreparedMaterialRouting::operator=(PreparedMaterialRouting&&) noexcept=default;
bool SceneController::PreparedMaterialRouting::ready() const noexcept {return state_ && !state_->consumed && state_->owner && state_->buffers;}
std::span<const container::gpu::AllocatedBuffer> SceneController::PreparedMaterialRouting::objectBuffers() const noexcept {
  return state_ && state_->buffers?std::span<const container::gpu::AllocatedBuffer>(state_->buffers->objects):std::span<const container::gpu::AllocatedBuffer>{};
}
std::span<const size_t> SceneController::PreparedMaterialRouting::objectBufferCapacities() const noexcept {
  return state_ && state_->buffers?std::span<const size_t>(state_->buffers->capacities):std::span<const size_t>{};
}
#define ROUTING_SPAN(type,name) std::span<const type> SceneController::PreparedMaterialRouting::name() const noexcept { \
  return state_?std::span<const type>(state_->name##_):std::span<const type>{}; }
ROUTING_SPAN(ObjectData,objectData)
ROUTING_SPAN(uint32_t,objectNodeIndices)
ROUTING_SPAN(DrawCommand,opaqueDrawCommands)
ROUTING_SPAN(DrawCommand,transparentDrawCommands)
ROUTING_SPAN(DrawCommand,opaqueSingleSidedDrawCommands)
ROUTING_SPAN(DrawCommand,opaqueWindingFlippedDrawCommands)
ROUTING_SPAN(DrawCommand,opaqueDoubleSidedDrawCommands)
ROUTING_SPAN(DrawCommand,transparentSingleSidedDrawCommands)
ROUTING_SPAN(DrawCommand,transparentWindingFlippedDrawCommands)
ROUTING_SPAN(DrawCommand,transparentDoubleSidedDrawCommands)
#undef ROUTING_SPAN
uint64_t SceneController::PreparedMaterialRouting::objectDataRevision() const noexcept {
  return state_ && state_->owner?state_->owner->objectDataRevision_+1:0;
}
uint64_t SceneController::PreparedMaterialRouting::expectedGraphRevision() const noexcept {return state_?state_->graph:0;}
uint64_t SceneController::PreparedMaterialRouting::candidateGraphRevision() const noexcept {return state_?state_->candidateGraph:0;}

static_assert(std::is_nothrow_swappable_v<std::vector<container::geometry::Vertex>>);
static_assert(std::is_nothrow_swappable_v<std::unordered_map<VkBuffer,uint64_t>>);
static_assert(std::is_nothrow_swappable_v<std::unique_ptr<GltfDeformationScene>>);

SceneController::PreparedGeneration::PreparedGeneration() noexcept=default;
SceneController::PreparedGeneration::~PreparedGeneration()=default;
SceneController::PreparedGeneration::PreparedGeneration(PreparedGeneration&&) noexcept=default;
SceneController::PreparedGeneration& SceneController::PreparedGeneration::operator=(PreparedGeneration&&) noexcept=default;
bool SceneController::PreparedGeneration::ready() const noexcept { return state_ && !state_->poseConsumed && state_->owner && state_->ownedGenerationBuffers_; }
uint64_t SceneController::PreparedGeneration::expectedGraphRevision() const noexcept { return state_?state_->expectedGraphRevision:0; }
uint64_t SceneController::PreparedGeneration::expectedSceneEpoch() const noexcept { return state_?state_->expectedSceneEpoch:0; }
container::gpu::BufferSlice SceneController::PreparedGeneration::vertexSlice() const noexcept { return state_?state_->vertexSlice_:container::gpu::BufferSlice{}; }
container::gpu::BufferSlice SceneController::PreparedGeneration::indexSlice() const noexcept { return state_?state_->indexSlice_:container::gpu::BufferSlice{}; }
container::gpu::BufferSlice SceneController::PreparedGeneration::diagCubeVertexSlice() const noexcept { return state_?state_->diagCubeVertexSlice_:container::gpu::BufferSlice{}; }
container::gpu::BufferSlice SceneController::PreparedGeneration::diagCubeIndexSlice() const noexcept { return state_?state_->diagCubeIndexSlice_:container::gpu::BufferSlice{}; }
uint32_t SceneController::PreparedGeneration::diagCubeIndexCount() const noexcept { return state_?state_->diagCubeIndexCount_:0; }
std::span<const container::gpu::AllocatedBuffer> SceneController::PreparedGeneration::objectBuffers() const noexcept {
  const auto* owner=state_?(state_->ownedMaterialObjectBuffers_?state_->ownedMaterialObjectBuffers_.get():state_->ownedGenerationBuffers_.get()):nullptr;
  return owner?std::span<const container::gpu::AllocatedBuffer>(owner->objects):std::span<const container::gpu::AllocatedBuffer>{};
}
std::span<const size_t> SceneController::PreparedGeneration::objectBufferCapacities() const noexcept {
  const auto* owner=state_?(state_->ownedMaterialObjectBuffers_?state_->ownedMaterialObjectBuffers_.get():state_->ownedGenerationBuffers_.get()):nullptr;
  return owner?std::span<const size_t>(owner->capacities):std::span<const size_t>{};
}
const GltfDeformationScene* SceneController::PreparedGeneration::deformationScene() const noexcept { return state_?state_->deformationScene_.get():nullptr; }
std::span<const ObjectData> SceneController::PreparedGeneration::objectData() const noexcept {
  return state_?std::span<const ObjectData>(state_->objectData_):std::span<const ObjectData>{};
}
std::span<const uint32_t> SceneController::PreparedGeneration::objectNodeIndices() const noexcept {
  return state_?std::span<const uint32_t>(state_->objectNodeIndices_):std::span<const uint32_t>{};
}
std::span<const DrawCommand> SceneController::PreparedGeneration::opaqueDrawCommands() const noexcept {
  return state_?std::span<const DrawCommand>(state_->opaqueDrawCommands_):std::span<const DrawCommand>{};
}
std::span<const DrawCommand> SceneController::PreparedGeneration::opaqueSingleSidedDrawCommands() const noexcept {
  return state_?std::span<const DrawCommand>(state_->opaqueSingleSidedDrawCommands_):std::span<const DrawCommand>{};
}
std::span<const DrawCommand> SceneController::PreparedGeneration::opaqueWindingFlippedDrawCommands() const noexcept {
  return state_?std::span<const DrawCommand>(state_->opaqueWindingFlippedDrawCommands_):std::span<const DrawCommand>{};
}
std::span<const DrawCommand> SceneController::PreparedGeneration::opaqueDoubleSidedDrawCommands() const noexcept {
  return state_?std::span<const DrawCommand>(state_->opaqueDoubleSidedDrawCommands_):std::span<const DrawCommand>{};
}
std::span<const DrawCommand> SceneController::PreparedGeneration::transparentDrawCommands() const noexcept {
  return state_?std::span<const DrawCommand>(state_->transparentDrawCommands_):std::span<const DrawCommand>{};
}
std::span<const container::geometry::Vertex> SceneController::PreparedGeneration::geometryVertices() const noexcept {
  return state_?(state_->deformationScene_?state_->deformationScene_->currentVertices():std::span<const container::geometry::Vertex>(state_->gpuBindVertices_).first(state_->generationSceneVertexCount_)):std::span<const container::geometry::Vertex>{};
}
std::span<const uint32_t> SceneController::PreparedGeneration::geometryIndices() const noexcept {
  return state_?(state_->deformationScene_?state_->deformationScene_->indices():std::span<const uint32_t>(state_->gpuBindIndices_).first(state_->generationSceneIndexCount_)):std::span<const uint32_t>{};
}
std::span<const container::geometry::Vertex> SceneController::PreparedGeneration::gpuBindVertices() const noexcept {
  return state_?std::span<const container::geometry::Vertex>(state_->gpuBindVertices_):std::span<const container::geometry::Vertex>{};
}
uint64_t SceneController::PreparedGeneration::geometryRevision() const noexcept { return state_ && state_->owner?state_->owner->geometryRevision_+1:0; }
uint64_t SceneController::PreparedGeneration::objectDataRevision() const noexcept { return state_ && state_->owner?state_->owner->objectDataRevision_+1:0; }
uint64_t SceneController::PreparedGeneration::ownedBytes() const noexcept {return state_?state_->cpuBytes:0;}
container::ecs::World& SceneController::PreparedGeneration::world() {
  if (!state_) throw std::logic_error("Empty prepared scene generation has no World");
  return *state_->world_;
}
const container::ecs::World& SceneController::PreparedGeneration::world() const {
  if (!state_) throw std::logic_error("Empty prepared scene generation has no World");
  return *state_->world_;
}

namespace {

bool transformFlipsWinding(const glm::mat4& transform) {
  const glm::vec3 x(transform[0]);
  const glm::vec3 y(transform[1]);
  const glm::vec3 z(transform[2]);
  const float determinant =
      x.x * (y.y * z.z - y.z * z.y) -
      y.x * (x.y * z.z - x.z * z.y) +
      z.x * (x.y * y.z - x.z * y.y);
  return determinant < 0.0f;
}

void expandBounds(glm::vec3 &boundsMin, glm::vec3 &boundsMax,
                  const glm::vec3 &point) {
  boundsMin = glm::min(boundsMin, point);
  boundsMax = glm::max(boundsMax, point);
}

bool intersectRaySphere(const glm::vec3& origin, const glm::vec3& direction,
                        const glm::vec3& center, float radius,
                        float& outDistance) {
  if (radius <= 0.0f) {
    return false;
  }

  const glm::vec3 toCenter = center - origin;
  const float projected = glm::dot(toCenter, direction);
  const float centerDistance2 = glm::dot(toCenter, toCenter);
  const float radius2 = radius * radius;
  const float closestDistance2 =
      centerDistance2 - projected * projected;
  if (closestDistance2 > radius2) {
    return false;
  }

  const float halfChord =
      std::sqrt(std::max(0.0f, radius2 - closestDistance2));
  const float t0 = projected - halfChord;
  const float t1 = projected + halfChord;
  if (t1 < 0.0f) {
    return false;
  }

  outDistance = t0 >= 0.0f ? t0 : 0.0f;
  return true;
}

enum class TriangleCullMode {
  None,
  Back,
  Front,
};

struct PickRay {
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
  bool valid{false};
};

PickRay makePickRay(const container::gpu::CameraData& cameraData,
                    VkExtent2D viewportExtent,
                    double cursorX,
                    double cursorY) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u) {
    return {};
  }

  const float ndcX =
      static_cast<float>((cursorX / static_cast<double>(viewportExtent.width)) *
                             2.0 -
                         1.0);
  const float ndcY =
      static_cast<float>(1.0 -
                         (cursorY / static_cast<double>(viewportExtent.height)) *
                             2.0);

  const glm::vec4 nearClip{ndcX, ndcY, 1.0f, 1.0f};
  const glm::vec4 farClip{ndcX, ndcY, 0.0f, 1.0f};
  glm::vec4 nearWorld = cameraData.inverseViewProj * nearClip;
  glm::vec4 farWorld = cameraData.inverseViewProj * farClip;
  if (nearWorld.w == 0.0f) {
    return {};
  }
  nearWorld /= nearWorld.w;

  PickRay ray{};
  ray.origin = glm::vec3(nearWorld);
  // An authored infinite reverse-Z camera maps depth zero to a direction
  // (homogeneous w=0), rather than a finite world point.
  ray.direction = farWorld.w == 0.0f ? glm::vec3(farWorld)
      : glm::vec3(farWorld / farWorld.w) - ray.origin;
  const double rayLength = std::hypot(static_cast<double>(ray.direction.x),
      static_cast<double>(ray.direction.y), static_cast<double>(ray.direction.z));
  if (!std::isfinite(rayLength) || rayLength <= 1e-12) {
    return {};
  }
  for (int i = 0; i < 3; ++i) ray.direction[i] = static_cast<float>(ray.direction[i]/rayLength);
  ray.valid = true;
  return ray;
}

bool intersectRayTriangle(const glm::vec3& origin,
                          const glm::vec3& direction,
                          const glm::vec3& v0,
                          const glm::vec3& v1,
                          const glm::vec3& v2,
                          TriangleCullMode cullMode,
                          float& outDistance) {
  constexpr float kEpsilon = 0.0000001f;
  const glm::vec3 edge1 = v1 - v0;
  const glm::vec3 edge2 = v2 - v0;
  const glm::vec3 pvec = glm::cross(direction, edge2);
  const float determinant = glm::dot(edge1, pvec);
  if ((cullMode == TriangleCullMode::Back && determinant <= kEpsilon) ||
      (cullMode == TriangleCullMode::Front && determinant >= -kEpsilon) ||
      (cullMode == TriangleCullMode::None &&
       std::abs(determinant) <= kEpsilon)) {
    return false;
  }

  const float inverseDeterminant = 1.0f / determinant;
  const glm::vec3 tvec = origin - v0;
  const float u = glm::dot(tvec, pvec) * inverseDeterminant;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const glm::vec3 qvec = glm::cross(tvec, edge1);
  const float v = glm::dot(direction, qvec) * inverseDeterminant;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float distance = glm::dot(edge2, qvec) * inverseDeterminant;
  if (distance < 0.0f) {
    return false;
  }

  outDistance = distance;
  return true;
}

float projectDepth(const container::gpu::CameraData& cameraData,
                   const glm::vec3& worldPosition) {
  const glm::vec4 clip =
      cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (clip.w == 0.0f) {
    return 0.0f;
  }
  return std::clamp(clip.z / clip.w, 0.0f, 1.0f);
}

bool sectionPlaneClips(const glm::vec3& worldPosition, bool enabled,
                       const glm::vec4& plane) {
  return enabled && glm::dot(glm::vec3(plane), worldPosition) + plane.w < 0.0f;
}

}  // namespace

SceneController::SceneController(
    std::shared_ptr<container::gpu::VulkanDevice>  device,
    container::gpu::AllocationManager&             allocationManager,
    container::gpu::PipelineManager&             pipelineManager,
    container::scene::SceneGraph&                     sceneGraph,
    container::scene::SceneManager&                   sceneManager,
    container::ui::GuiManager*                        guiManager,
    const container::app::AppConfig&                           config)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , pipelineManager_(pipelineManager)
    , sceneGraph_(sceneGraph)
    , sceneManager_(sceneManager)
    , guiManager_(guiManager)
    , config_(config)
    , world_(std::make_unique<container::ecs::World>()) {
}

SceneController::~SceneController() = default;

container::ecs::World& SceneController::world() { return *world_; }
const container::ecs::World& SceneController::world() const { return *world_; }

std::span<const container::gpu::AllocatedBuffer> SceneController::objectBuffers() const noexcept {
  const auto* owner=ownedMaterialObjectBuffers_?ownedMaterialObjectBuffers_.get():ownedGenerationBuffers_.get();
  return owner?std::span<const container::gpu::AllocatedBuffer>(owner->objects):std::span<const container::gpu::AllocatedBuffer>{};
}
std::span<const size_t> SceneController::objectBufferCapacities() const noexcept {
  const auto* owner=ownedMaterialObjectBuffers_?ownedMaterialObjectBuffers_.get():ownedGenerationBuffers_.get();
  return owner?std::span<const size_t>(owner->capacities):std::span<const size_t>{};
}
bool SceneController::ownsObjectBuffer(VkBuffer buffer) const noexcept {
  if (buffer==VK_NULL_HANDLE) return false;
  return std::ranges::any_of(objectBuffers(),[&](const auto& owned) { return owned.buffer==buffer; });
}

void SceneController::setOwnedGeometrySlices(size_t sceneIndexCount) noexcept {
  vertexSlice_={ownedGenerationBuffers_->vertices.buffer,0,gpuBindVertices_.size()*sizeof(container::geometry::Vertex)};
  indexSlice_={ownedGenerationBuffers_->indices.buffer,0,gpuBindIndices_.size()*sizeof(uint32_t)};
  diagCubeVertexSlice_=vertexSlice_;
  diagCubeIndexSlice_={indexSlice_.buffer,sceneIndexCount*sizeof(uint32_t),diagCubeIndexCount_*sizeof(uint32_t)};
}

void SceneController::swapGenerationState(PreparedGeneration::State& state) noexcept {
  if(state.graphPose) world_->swapStatePreservingContext(*state.world_);
  else world_->swapState(*state.world_);
  using std::swap;
#define SCENE_EXCHANGE(member) swap(member,state.member)
  SCENE_EXCHANGE(ownedGenerationBuffers_); SCENE_EXCHANGE(deformationScene_);
  SCENE_EXCHANGE(ownedMaterialObjectBuffers_);
  SCENE_EXCHANGE(gpuBindVertices_); SCENE_EXCHANGE(gpuBindIndices_);
  SCENE_EXCHANGE(generationSceneVertexCount_); SCENE_EXCHANGE(generationSceneIndexCount_);
  SCENE_EXCHANGE(deformationSourceGeometryRevision_); SCENE_EXCHANGE(geometryRevision_); SCENE_EXCHANGE(deformationTopologyPending_);
  SCENE_EXCHANGE(objectData_); SCENE_EXCHANGE(opaqueDrawCommands_); SCENE_EXCHANGE(transparentDrawCommands_);
  SCENE_EXCHANGE(opaqueSingleSidedDrawCommands_); SCENE_EXCHANGE(opaqueWindingFlippedDrawCommands_); SCENE_EXCHANGE(opaqueDoubleSidedDrawCommands_);
  SCENE_EXCHANGE(transparentSingleSidedDrawCommands_); SCENE_EXCHANGE(transparentWindingFlippedDrawCommands_); SCENE_EXCHANGE(transparentDoubleSidedDrawCommands_);
  SCENE_EXCHANGE(primitiveBounds_); SCENE_EXCHANGE(objectNodeIndices_); SCENE_EXCHANGE(objectBufferUploadRevisions_);
  SCENE_EXCHANGE(cachedSceneGraphRevision_); SCENE_EXCHANGE(cachedGeometryRevision_);
  SCENE_EXCHANGE(cachedMaterialRevision_);
  SCENE_EXCHANGE(objectDataRevision_); SCENE_EXCHANGE(objectBufferUploadRevision_);
  SCENE_EXCHANGE(cachedShowDiagCube_); SCENE_EXCHANGE(objectDataCacheValid_);
  SCENE_EXCHANGE(vertexSlice_); SCENE_EXCHANGE(indexSlice_); SCENE_EXCHANGE(diagCubeVertexSlice_); SCENE_EXCHANGE(diagCubeIndexSlice_);
  SCENE_EXCHANGE(diagCubeIndexCount_); SCENE_EXCHANGE(diagCubeObjectIndex_);
#undef SCENE_EXCHANGE
}

SceneController::PreparedGeneration SceneController::prepareGeneration(
    container::scene::SceneGraph& detachedGraph,container::scene::SceneManager& detachedScene,
    uint32_t imageCount,bool showDiagCube,uint64_t expectedGraphRevision,uint64_t expectedSceneEpoch) const {
  if (&detachedGraph==&sceneGraph_ || &detachedScene==&sceneManager_)
    throw std::invalid_argument("Scene generation preparation requires detached graph and manager");
  return prepareGenerationImpl(detachedGraph,detachedScene,imageCount,showDiagCube,expectedGraphRevision,expectedSceneEpoch,false);
}
SceneController::PreparedGeneration SceneController::prepareGraphPoseGeneration(
    const container::scene::SceneGraph& candidate,uint32_t images,bool showDiag,uint64_t graphRevision,uint64_t epoch) const {
  return prepareGraphPoseGeneration(candidate,images,showDiag,graphRevision,epoch,GraphPoseGenerationLimits{});
}
SceneController::PreparedGeneration SceneController::prepareGraphPoseGeneration(
    const container::scene::SceneGraph& candidate,uint32_t images,bool showDiag,uint64_t graphRevision,uint64_t epoch,
    const GraphPoseGenerationLimits& limits) const {
  if(&candidate==&sceneGraph_ || graphRevision!=sceneGraph_.revision() || graphRevision==UINT64_MAX || epoch==UINT64_MAX ||
      !images || images>limits.maxImages || !limits.maxNodes || !limits.maxEdges || !limits.maxObjects ||
      !limits.maxVertices || !limits.maxIndices || !limits.maxImages || !limits.maxCpuBytes || !limits.maxGpuBytes || !limits.maxWork ||
      limits.maxNodes>=UINT32_MAX || limits.maxObjects>=UINT32_MAX || limits.maxVertices>=UINT32_MAX || limits.maxIndices>=UINT32_MAX ||
      limits.maxCpuBytes==UINT64_MAX || limits.maxGpuBytes==UINT64_MAX || limits.maxWork==UINT64_MAX)
    throw std::invalid_argument("Graph pose generation requires an exact detached candidate and finite limits");
  PoseBudget budget{limits};budget.bytes(sizeof(PreparedGeneration::State)+4096);
  poseGraphPreflight(sceneGraph_,candidate,budget);poseSourcePreflight(sceneManager_,candidate,*world_,images,showDiag,budget);
  if(!ownedGenerationBuffers_) {
    budget.deviceBytes(gpuBindVertices_.size(),sizeof(container::geometry::Vertex));budget.deviceBytes(gpuBindIndices_.size(),sizeof(uint32_t));
    if(!ownedMaterialObjectBuffers_) budget.deviceBytes(std::max<size_t>(1,objectData_.size()),uint64_t(images)*sizeof(ObjectData));
    budget.bytes(gpuBindVertices_.size(),sizeof(container::geometry::Vertex));budget.bytes(gpuBindIndices_.size(),sizeof(uint32_t));
  }
  const auto geometry=sceneManager_.geometryRevision(),material=sceneManager_.materialRevision(),sourceEpoch=sceneManager_.generationEpoch();
  const auto asset=sceneManager_.materialAssetIdentity();const auto* deformation=sceneManager_.deformationData();
  PoseGraphWitness before;before.capture(sceneGraph_);PoseWeightWitness weights;weights.capture(std::as_const(sceneManager_).animationSource());
  auto beforeWorld=world_->cloneStateWithoutContext();
  container::scene::SceneGraph detached(candidate);
  auto result=prepareGenerationImpl(detached,sceneManager_,images,showDiag,graphRevision,epoch,true);
  auto& state=*result.state_;state.beforePose=std::move(before);state.nextPose.capture(detached);state.weights=std::move(weights);
  state.beforeWorld=std::move(beforeWorld);state.sourceGeometry=geometry;state.sourceMaterial=material;state.sourceEpoch=sourceEpoch;
  state.sourceAsset=asset;state.sourceDeformation=deformation;state.cpuBytes=budget.cpu;
  // Re-evaluating detached caches must not reinterpret an inconsistent input
  // world or change any raw caller-owned candidate payload.
  if(!state.nextPose.matches(candidate,false) || !canExchangeGeneration(result,epoch))
    throw std::invalid_argument("Graph pose generation became stale during preparation");
  return result;
}
SceneController::PreparedGeneration SceneController::prepareGenerationImpl(
    container::scene::SceneGraph& detachedGraph,container::scene::SceneManager& detachedScene,
    uint32_t imageCount,bool showDiagCube,uint64_t expectedGraphRevision,uint64_t expectedSceneEpoch,bool preserveContext) const {
  if (!imageCount || imageCount>65536 || !device_ || !allocationManager_.memoryManager())
    throw std::invalid_argument("Scene generation preparation requires initialized resources and image count");
  uint64_t activeObjects=showDiagCube?1:0;
  for (const auto node:detachedGraph.renderableNodes())
    if (detachedGraph.isNodeEffectivelyVisible(node)) ++activeObjects;
  if (activeObjects>config_.maxSceneObjects || activeObjects>UINT32_MAX)
    throw std::runtime_error("Staged scene exceeds maximum object count");
  if (geometryRevision_==UINT64_MAX || objectDataRevision_==UINT64_MAX || objectBufferUploadRevision_==UINT64_MAX)
    throw std::overflow_error("Scene generation revision exhausted");
  PreparedGeneration result; result.state_=std::make_unique<PreparedGeneration::State>();
  auto& state=*result.state_; state.owner=this;
  state.graphPose=preserveContext;
  state.expectedGraphRevision=expectedGraphRevision; state.expectedSceneEpoch=expectedSceneEpoch;
  state.expectedControllerGeometry=geometryRevision_; state.expectedControllerObjects=objectDataRevision_;
  state.expectedControllerUpload=objectBufferUploadRevision_;
  SceneController detached(device_,allocationManager_,pipelineManager_,detachedGraph,detachedScene,nullptr,config_);
  detached.world_=preserveContext?world_->cloneStateWithoutContext():world_->cloneState();
  detached.updateDeformation(true);
  detached.ownedGenerationBuffers_=OwnedGenerationBuffers::geometry(allocationManager_,device_,detached.gpuBindVertices_,detached.gpuBindIndices_);
  detached.diagCubeIndexCount_=static_cast<uint32_t>(makeScenePrimitiveCube().indices().size());
  detached.setOwnedGeometrySlices(detached.geometryIndices().size());
  detached.deformationTopologyPending_=false;
  detached.syncObjectDataFromSceneGraph(showDiagCube);
  detached.ownedGenerationBuffers_->createObjects(imageCount,detached.objectData_);
  for (const auto& object:detached.ownedGenerationBuffers_->objects)
    detached.objectBufferUploadRevisions_.emplace(object.buffer,detached.objectBufferUploadRevision_);
  detached.swapGenerationState(state);
  if (!ownedGenerationBuffers_) {
    state.initialBuffers=OwnedGenerationBuffers::geometry(allocationManager_,device_,gpuBindVertices_,gpuBindIndices_);
    if(!ownedMaterialObjectBuffers_) {
      state.initialBuffers->createObjects(imageCount,objectData_);
      for (const auto& object:state.initialBuffers->objects)
        state.initialUploadRevisions.emplace(object.buffer,objectBufferUploadRevision_);
    }
  }
  return result;
}

bool SceneController::canExchangeGeneration(const PreparedGeneration& prepared,uint64_t currentSceneEpoch) const noexcept {
  return prepared.ready() && prepared.state_->owner==this &&
      prepared.state_->expectedGraphRevision==sceneGraph_.revision() && prepared.state_->expectedSceneEpoch==currentSceneEpoch &&
      prepared.state_->expectedControllerGeometry==geometryRevision_ && prepared.state_->expectedControllerObjects==objectDataRevision_ &&
      prepared.state_->expectedControllerUpload==objectBufferUploadRevision_ &&
      geometryRevision_!=UINT64_MAX && objectDataRevision_!=UINT64_MAX && objectBufferUploadRevision_!=UINT64_MAX &&
      (!prepared.state_->graphPose || (prepared.state_->beforePose.matches(sceneGraph_,true) && prepared.state_->beforeWorld &&
          poseWorldSame(*prepared.state_->beforeWorld,*world_) && prepared.state_->sourceGeometry==sceneManager_.geometryRevision() &&
          prepared.state_->sourceMaterial==sceneManager_.materialRevision() && prepared.state_->sourceEpoch==sceneManager_.generationEpoch() &&
          prepared.state_->sourceAsset==sceneManager_.materialAssetIdentity() && prepared.state_->sourceDeformation==sceneManager_.deformationData() &&
          prepared.state_->weights.matches(std::as_const(sceneManager_).animationSource())));
}

void SceneController::exchangeGeneration(PreparedGeneration& prepared,uint64_t publishedGraphRevision,
    uint64_t publishedSceneGeometryRevision,uint64_t publishedSceneEpoch) noexcept {
  // The frontend validates the outer scene/graph transaction before its first
  // publication. This exchange has no fallible work or retirement side effect.
  if (!prepared.ready() || prepared.state_->owner!=this) return;
  auto& state=*prepared.state_;
  if(state.graphPose && (state.expectedControllerGeometry!=geometryRevision_ || state.expectedControllerObjects!=objectDataRevision_ ||
      state.expectedControllerUpload!=objectBufferUploadRevision_ || geometryRevision_==UINT64_MAX || objectDataRevision_==UINT64_MAX || objectBufferUploadRevision_==UINT64_MAX ||
      publishedGraphRevision!=sceneGraph_.revision() || publishedGraphRevision!=state.expectedGraphRevision+1 ||
      publishedSceneGeometryRevision!=state.sourceGeometry || publishedSceneEpoch!=state.expectedSceneEpoch+1 ||
      !state.nextPose.matches(sceneGraph_,false) || !state.beforeWorld || !poseWorldSame(*state.beforeWorld,*world_) ||
      state.sourceGeometry!=sceneManager_.geometryRevision() || state.sourceMaterial!=sceneManager_.materialRevision() ||
      state.sourceEpoch!=sceneManager_.generationEpoch() || state.sourceAsset!=sceneManager_.materialAssetIdentity() ||
      state.sourceDeformation!=sceneManager_.deformationData() || !state.weights.matches(std::as_const(sceneManager_).animationSource()))) return;
  const auto nextGeometry=geometryRevision_+1,nextObjects=objectDataRevision_+1,nextUpload=objectBufferUploadRevision_+1;
  if (!ownedGenerationBuffers_ && state.initialBuffers) {
    const auto sceneIndices=diagCubeIndexSlice_.buffer==indexSlice_.buffer && diagCubeIndexSlice_.offset>=indexSlice_.offset
        ?size_t((diagCubeIndexSlice_.offset-indexSlice_.offset)/sizeof(uint32_t)):gpuBindIndices_.size();
    ownedGenerationBuffers_=std::move(state.initialBuffers);
    setOwnedGeometrySlices(sceneIndices);
    if(!ownedMaterialObjectBuffers_) objectBufferUploadRevisions_.swap(state.initialUploadRevisions);
  }
  swapGenerationState(state);
  geometryRevision_=nextGeometry; objectDataRevision_=nextObjects; objectBufferUploadRevision_=nextUpload;
  deformationSourceGeometryRevision_=publishedSceneGeometryRevision;
  cachedSceneGraphRevision_=publishedGraphRevision; cachedGeometryRevision_=geometryRevision_;
  cachedMaterialRevision_=sceneManager_.materialRevision();
  for (auto& [buffer,revision]:objectBufferUploadRevisions_) { (void)buffer; revision=objectBufferUploadRevision_; }
  state.expectedGraphRevision=publishedGraphRevision; state.expectedSceneEpoch=publishedSceneEpoch;
  state.expectedControllerGeometry=geometryRevision_; state.expectedControllerObjects=objectDataRevision_;
  state.expectedControllerUpload=objectBufferUploadRevision_;
  if(state.graphPose) state.poseConsumed=true;
}

bool SceneController::materialRoutingCacheCurrent(bool showDiagCube) const noexcept {
  return objectDataCacheValid_ && cachedSceneGraphRevision_==sceneGraph_.revision() &&
      cachedGeometryRevision_==geometryRevision_ && cachedMaterialRevision_==sceneManager_.materialRevision() &&
      cachedShowDiagCube_==showDiagCube && objectNodeIndices_.size()==objectData_.size();
}

static container::scene::MaterialBindingContext routingBindingContext(const container::scene::SceneManager& manager) noexcept {
  if(manager.materialCount()>UINT32_MAX || manager.primitiveRanges().size()>=UINT32_MAX) return {};
  return {manager.materialAssetIdentity(),manager.sourceMaterialRuntimeMapping(),uint32_t(manager.materialCount()),
      uint32_t(manager.primitiveRanges().size())};
}

SceneController::PreparedMaterialRouting SceneController::prepareMaterialRouting(
    std::span<const container::scene::MaterialRenderProperties> materials,uint32_t images,bool showDiag,
    uint64_t epoch) const {
  return prepareMaterialRouting(materials,images,showDiag,epoch,MaterialRoutingLimits{});
}
SceneController::PreparedMaterialRouting SceneController::prepareMaterialRouting(
    std::span<const container::scene::MaterialRenderProperties> materials,uint32_t images,bool showDiag,
    uint64_t epoch,const MaterialRoutingLimits& limits) const {
  return prepareMaterialRoutingImpl(materials,nullptr,images,showDiag,epoch,limits);
}
SceneController::PreparedMaterialRouting SceneController::prepareMaterialRouting(
    std::span<const container::scene::MaterialRenderProperties> materials,
    const container::scene::PreparedMaterialBindingChange& bindings,uint32_t images,bool showDiag,uint64_t epoch) const {
  return prepareMaterialRouting(materials,bindings,images,showDiag,epoch,MaterialRoutingLimits{});
}
SceneController::PreparedMaterialRouting SceneController::prepareMaterialRouting(
    std::span<const container::scene::MaterialRenderProperties> materials,
    const container::scene::PreparedMaterialBindingChange& bindings,uint32_t images,bool showDiag,uint64_t epoch,
    const MaterialRoutingLimits& limits) const {
  return prepareMaterialRoutingImpl(materials,&bindings,images,showDiag,epoch,limits);
}
SceneController::PreparedMaterialRouting SceneController::prepareMaterialRoutingImpl(
    std::span<const container::scene::MaterialRenderProperties> materials,
    const container::scene::PreparedMaterialBindingChange* bindings,uint32_t images,bool showDiag,
    uint64_t epoch,const MaterialRoutingLimits& limits) const {
  const auto fail=[](const char* reason) {throw std::runtime_error(std::string("Material routing: ")+reason);};
  if(!images || images>limits.maxImages || !device_ || !allocationManager_.memoryManager() ||
      epoch!=sceneManager_.generationEpoch()) fail("invalid frame/scene context");
  // Validate the source view. The detached candidate may deliberately add or
  // remove its diagnostic tail; that does not mutate today's cached view.
  if(!materialRoutingCacheCurrent(cachedShowDiagCube_)) fail("current object cache is stale");
  if(bindings && !container::scene::CanPublishMaterialBindings(sceneGraph_,*bindings,routingBindingContext(sceneManager_)))
    fail("material binding proof is stale or belongs to another graph/asset");
  const auto prospectiveCount=bindings?bindings->targetContext().runtimeMaterialCount:sceneManager_.materialCount();
  const auto assignments=bindings?bindings->assignments():std::span<const container::scene::MaterialBindingAssignment>{};
  if(materials.size()!=prospectiveCount || materials.size()>limits.maxMaterials)
    fail("runtime material identity table differs");
  for(size_t i=0;i<materials.size();++i)
    if(materials[i].gpuMaterialIndex!=i || !std::isfinite(materials[i].heightScale)) fail("invalid runtime material facts");
  if(objectDataRevision_==UINT64_MAX || objectBufferUploadRevision_==UINT64_MAX) fail("object revision exhausted");
  const bool oldDiag=diagCubeObjectIndex_!=UINT32_MAX;
  if(oldDiag && (objectData_.empty() || diagCubeObjectIndex_!=objectData_.size()-1)) fail("diagnostic tail is malformed");
  const size_t rows=objectData_.size()-size_t(oldDiag);
  const bool nextDiag=showDiag && diagCubeVertexSlice_.buffer!=VK_NULL_HANDLE;
  const size_t count=rows+size_t(nextDiag);
  if(count>UINT32_MAX || count>limits.maxObjects || count>config_.maxSceneObjects) fail("object count exceeds its budget");
  const auto multiply=[&](uint64_t a,uint64_t b) {if(a && b>UINT64_MAX/a) fail("byte range overflow");return a*b;};
  const auto add=[&](uint64_t a,uint64_t b) {if(b>UINT64_MAX-a) fail("byte range overflow");return a+b;};
  // All eight draw vectors reserve the full count. Include lookup scratch,
  // upload maps and an optional owned legacy object snapshot before copies.
  const auto bound=std::max(count,objectData_.size());
  auto cpu=add(sizeof(PreparedMaterialRouting::State),multiply(bound,sizeof(ObjectData)+sizeof(uint32_t)+sizeof(uint8_t)+9*sizeof(DrawCommand)));
  cpu=add(cpu,multiply(images,2*(sizeof(container::gpu::AllocatedBuffer)+sizeof(size_t))+512));
  if(bindings) {
    cpu=add(cpu,bindings->command().ownedBytes());
    cpu=add(cpu,multiply(rows,sizeof(PreparedMaterialRouting::State::EcsBinding)+sizeof(uint32_t)));
  }
  const bool legacy=!ownedMaterialObjectBuffers_ && !ownedGenerationBuffers_;
  auto gpu=multiply(multiply(images,std::max<size_t>(1,count)),sizeof(ObjectData));
  if(legacy) gpu=add(gpu,multiply(multiply(images,std::max<size_t>(1,objectData_.size())),sizeof(ObjectData)));
  if(cpu>limits.maxCpuBytes || gpu>limits.maxGpuBytes) fail("detached storage budget exceeded");
  std::vector<uint32_t> expectedEcsNodes;
  if(bindings) expectedEcsNodes.reserve(rows);
  for(size_t i=0;i<rows;++i) {
    const auto& object=objectData_[i];const auto node=objectNodeIndices_[i];
    if(object.objectInfo.x>=sceneManager_.materialCount() || object.temporalInfo.w>=sceneManager_.primitiveRanges().size() ||
        node>=sceneGraph_.nodeCount() || sceneGraph_.isNodeRetired(node)) fail("object source identity is invalid");
    if(bindings && sceneGraph_.getNode(node)->materialIndex!=object.objectInfo.x)
      fail("object material cache disagrees with the binding source");
    if(bindings) {
      const auto found=std::ranges::lower_bound(assignments,node,{},&container::scene::MaterialBindingAssignment::graphNode);
      if(found!=assignments.end() && found->graphNode==node) expectedEcsNodes.push_back(node);
    }
  }
  PreparedMaterialRouting result;result.state_=std::make_unique<PreparedMaterialRouting::State>();
  auto& state=*result.state_;state.owner=this;state.graph=sceneGraph_.revision();state.sceneEpoch=epoch;
  state.candidateGraph=bindings?bindings->candidateGraphRevision():state.graph;
  if(bindings) {
    state.bindingAware=true;state.bindings=bindings->command();state.bindingTarget=bindings->targetSide();state.ecsRenderableCount=rows;
    state.ecsBindings.reserve(rows);
    const auto& registry=std::as_const(*world_).registry();
    const auto view=registry.view<const container::ecs::MaterialComponent,const container::ecs::SceneNodeRef,
        const container::ecs::MeshComponent,const container::ecs::RenderableTag>();
    size_t entities{};
    for(const auto entity:view) {
      ++entities;
      const auto& node=view.get<const container::ecs::SceneNodeRef>(entity);
      const auto found=std::ranges::lower_bound(assignments,node.nodeIndex,{},&container::scene::MaterialBindingAssignment::graphNode);
      if(found==assignments.end() || found->graphNode!=node.nodeIndex) continue;
      const auto& material=view.get<const container::ecs::MaterialComponent>(entity);
      const auto& mesh=view.get<const container::ecs::MeshComponent>(entity);
      if(material.materialIndex!=found->beforeRuntimeMaterial ||
          mesh.primitiveIndex!=sceneGraph_.getNode(node.nodeIndex)->primitiveIndex)
        fail("ECS material/primitive cache disagrees with the binding source");
      state.ecsBindings.push_back({entity,const_cast<container::ecs::MaterialComponent*>(&material),&node,&mesh,
          node.nodeIndex,mesh.primitiveIndex,found->beforeRuntimeMaterial,found->afterRuntimeMaterial});
    }
    if(entities!=rows) fail("ECS renderable cache differs from the object table");
    // Duplicate entity references are not a valid one-to-one source mirror.
    std::ranges::sort(state.ecsBindings,{},&PreparedMaterialRouting::State::EcsBinding::node);
    std::ranges::sort(expectedEcsNodes);
    if(state.ecsBindings.size()!=expectedEcsNodes.size()) fail("selected object bindings have missing/extra ECS witnesses");
    for(size_t i=0;i<expectedEcsNodes.size();++i)
      if(state.ecsBindings[i].node!=expectedEcsNodes[i]) fail("selected object binding differs from its ECS witness");
    for(size_t i=1;i<state.ecsBindings.size();++i)
      if(state.ecsBindings[i-1].node==state.ecsBindings[i].node) fail("ECS contains duplicate selected graph bindings");
  }
  state.sourceGeometry=sceneManager_.geometryRevision();state.sourceMaterial=sceneManager_.materialRevision();
  state.geometry=geometryRevision_;state.objects=objectDataRevision_;state.upload=objectBufferUploadRevision_;
  state.objectData_.reserve(count);state.objectData_.assign(objectData_.begin(),objectData_.begin()+rows);
  state.objectNodeIndices_.reserve(count);state.objectNodeIndices_.assign(objectNodeIndices_.begin(),objectNodeIndices_.begin()+rows);
  std::vector<DrawCommand> draws(rows);std::vector<bool> seen(rows);
  const auto collect=[&](const auto& commands) {for(const auto& draw:commands) {
    if(draw.instanceCount!=1 || draw.objectIndex>=rows || seen[draw.objectIndex]) fail("draw/object table is not one-to-one");
    seen[draw.objectIndex]=true;draws[draw.objectIndex]=draw;
  }};
  collect(opaqueDrawCommands_);collect(transparentDrawCommands_);
  if(std::ranges::find(seen,false)!=seen.end()) fail("object has no draw binding");
  for(auto* commands:{&state.opaqueDrawCommands_,&state.transparentDrawCommands_,
      &state.opaqueSingleSidedDrawCommands_,&state.opaqueWindingFlippedDrawCommands_,&state.opaqueDoubleSidedDrawCommands_,
      &state.transparentSingleSidedDrawCommands_,&state.transparentWindingFlippedDrawCommands_,&state.transparentDoubleSidedDrawCommands_})
    commands->reserve(rows);
  for(size_t i=0;i<rows;++i) {
    auto& object=state.objectData_[i];
    if(bindings) {
      const auto found=std::ranges::lower_bound(assignments,state.objectNodeIndices_[i],{},&container::scene::MaterialBindingAssignment::graphNode);
      if(found!=assignments.end() && found->graphNode==state.objectNodeIndices_[i]) object.objectInfo.x=found->afterRuntimeMaterial;
    }
    if(object.objectInfo.x>=materials.size()) fail("target object material exceeds the prospective runtime table");
    const auto& properties=materials[object.objectInfo.x];
    const auto primitive=object.temporalInfo.w;
    const auto* deformation=deformationScene_?deformationScene_->drawRange(state.objectNodeIndices_[i]):nullptr;
    const bool doubleSided=properties.doubleSided || sceneManager_.primitiveRanges()[primitive].disableBackfaceCulling ||
        (deformation && deformation->orientation==GltfDeformationOrientation::MixedOrDegenerate);
    object.objectInfo.y&=~container::gpu::kObjectFlagDoubleSided;
    if(doubleSided) object.objectInfo.y|=container::gpu::kObjectFlagDoubleSided;
    const bool flipped=transformFlipsWinding(object.model)!=
        (deformation && deformation->orientation==GltfDeformationOrientation::Reversed);
    if(deformation || primitive<primitiveBounds_.size()) {
      const auto bounds=deformation?PrimitiveBounds{deformation->bounds.center,deformation->bounds.radius,deformation->bounds.valid}:primitiveBounds_[primitive];
      if(bounds.valid) {
        const auto scale=container::math::conservativeSphereScale(object.model);
        object.boundingSphere=glm::vec4(glm::vec3(object.model*glm::vec4(bounds.center,1)),
            bounds.radius*scale+std::abs(properties.heightScale)*scale);
        for(int axis=0;axis<4;++axis) if(!std::isfinite(object.boundingSphere[axis])) fail("material bounds are not finite");
      }
    }
    auto& all=properties.transparent?state.transparentDrawCommands_:state.opaqueDrawCommands_;all.push_back(draws[i]);
    auto& sided=properties.transparent
        ?(doubleSided?state.transparentDoubleSidedDrawCommands_:flipped?state.transparentWindingFlippedDrawCommands_:state.transparentSingleSidedDrawCommands_)
        :(doubleSided?state.opaqueDoubleSidedDrawCommands_:flipped?state.opaqueWindingFlippedDrawCommands_:state.opaqueSingleSidedDrawCommands_);
    sided.push_back(draws[i]);
  }
  if(nextDiag) {
    ObjectData object{};
    if(oldDiag) object=objectData_.back();
    else {
      const auto& bounds=sceneManager_.modelBounds();
      const auto center=bounds.valid?bounds.center+glm::vec3(bounds.radius*1.75f,0,0):glm::vec3(0);
      const auto scale=bounds.valid?std::max(.25f,bounds.radius*.45f):.5f;
      object.model=glm::translate(glm::mat4(1),center)*glm::scale(glm::mat4(1),glm::vec3(scale));
      const auto normal=glm::transpose(glm::inverse(glm::mat3(object.model)));
      object.normalMatrix0=glm::vec4(normal[0],0);object.normalMatrix1=glm::vec4(normal[1],0);object.normalMatrix2=glm::vec4(normal[2],0);
      object.objectInfo.x=sceneManager_.diagnosticMaterialIndex();object.temporalInfo.z=object.temporalInfo.w=UINT32_MAX;
    }
    if(object.objectInfo.x>=materials.size()) fail("diagnostic material exceeds the prospective runtime table");
    state.diagCubeObjectIndex_=uint32_t(rows);state.objectData_.push_back(object);state.objectNodeIndices_.push_back(UINT32_MAX);
  }
  state.cachedSceneGraphRevision_=state.candidateGraph;state.cachedGeometryRevision_=geometryRevision_;
  state.cachedMaterialRevision_=sceneManager_.materialRevision();state.cachedShowDiagCube_=showDiag;
  state.buffers=std::make_unique<OwnedGenerationBuffers>(allocationManager_,device_);
  state.buffers->createObjects(images,state.objectData_);
  for(const auto& object:state.buffers->objects) state.objectBufferUploadRevisions_.emplace(object.buffer,objectBufferUploadRevision_+1);
  if(!ownedMaterialObjectBuffers_) {
    state.initial=std::make_unique<OwnedGenerationBuffers>(allocationManager_,device_);
    state.adoptBase=bool(ownedGenerationBuffers_);
    if(!state.adoptBase) {
      state.initial->createObjects(images,objectData_);
      for(const auto& object:state.initial->objects) state.initialUploads.emplace(object.buffer,objectBufferUploadRevision_);
    }
  }
  return result;
}
bool SceneController::canExchangeMaterialRouting(const PreparedMaterialRouting& prepared,uint64_t epoch) const noexcept {
  if(!prepared.ready()) return false;const auto& state=*prepared.state_;
  if(state.bindingAware && (!container::scene::MatchesMaterialBindings(sceneGraph_,state.bindings,routingBindingContext(sceneManager_),
      state.bindingTarget==container::scene::MaterialBindingSide::After?container::scene::MaterialBindingSide::Before:container::scene::MaterialBindingSide::After) ||
      !world_ || !state.ecsMatches(*world_))) return false;
  return state.owner==this && state.graph==sceneGraph_.revision() && state.sceneEpoch==epoch &&
      epoch==sceneManager_.generationEpoch() && objectDataCacheValid_ &&
      cachedSceneGraphRevision_==state.graph && cachedGeometryRevision_==geometryRevision_ &&
      cachedMaterialRevision_==state.sourceMaterial &&
      state.sourceGeometry==sceneManager_.geometryRevision() && state.sourceMaterial==sceneManager_.materialRevision() &&
      state.geometry==geometryRevision_ && state.objects==objectDataRevision_ && state.upload==objectBufferUploadRevision_ &&
      objectDataRevision_<UINT64_MAX && objectBufferUploadRevision_<UINT64_MAX;
}
void SceneController::exchangeMaterialRouting(PreparedMaterialRouting& prepared,uint64_t materialRevision,uint64_t sceneEpoch) noexcept {
  if(!prepared.ready() || prepared.state_->owner!=this) return;auto& state=*prepared.state_;
  if(state.bindingAware && (!container::scene::MatchesMaterialBindings(sceneGraph_,state.bindings,routingBindingContext(sceneManager_),state.bindingTarget) ||
      !world_ || !state.ecsMatches(*world_))) return;
  if(materialRevision!=sceneManager_.materialRevision() || sceneEpoch!=sceneManager_.generationEpoch() ||
      state.sourceGeometry!=sceneManager_.geometryRevision() || !objectDataCacheValid_ ||
      cachedSceneGraphRevision_!=state.graph || cachedGeometryRevision_!=geometryRevision_ ||
      cachedMaterialRevision_!=state.sourceMaterial ||
      state.candidateGraph!=sceneGraph_.revision() || state.geometry!=geometryRevision_ || state.objects!=objectDataRevision_ ||
      state.upload!=objectBufferUploadRevision_ || objectDataRevision_==UINT64_MAX || objectBufferUploadRevision_==UINT64_MAX) return;
  // Existing component pointers/entities were checked as one batch above.
  // ID writes keep the live World/registry and every entity identity stable.
  for(const auto& row:state.ecsBindings) row.component->materialIndex=row.after;
  if(!ownedMaterialObjectBuffers_ && state.initial) {
    if(state.adoptBase) {
      state.initial->objects.swap(ownedGenerationBuffers_->objects);state.initial->capacities.swap(ownedGenerationBuffers_->capacities);
    } else objectBufferUploadRevisions_.swap(state.initialUploads);
    ownedMaterialObjectBuffers_=std::move(state.initial);
  }
  ownedMaterialObjectBuffers_.swap(state.buffers);
  using std::swap;
#define MATERIAL_EXCHANGE(name) static_assert(std::is_nothrow_swappable_v<decltype(name)>);swap(name,state.name)
  MATERIAL_EXCHANGE(objectData_);MATERIAL_EXCHANGE(objectNodeIndices_);MATERIAL_EXCHANGE(objectBufferUploadRevisions_);
  MATERIAL_EXCHANGE(opaqueDrawCommands_);MATERIAL_EXCHANGE(transparentDrawCommands_);
  MATERIAL_EXCHANGE(opaqueSingleSidedDrawCommands_);MATERIAL_EXCHANGE(opaqueWindingFlippedDrawCommands_);MATERIAL_EXCHANGE(opaqueDoubleSidedDrawCommands_);
  MATERIAL_EXCHANGE(transparentSingleSidedDrawCommands_);MATERIAL_EXCHANGE(transparentWindingFlippedDrawCommands_);MATERIAL_EXCHANGE(transparentDoubleSidedDrawCommands_);
  MATERIAL_EXCHANGE(cachedSceneGraphRevision_);MATERIAL_EXCHANGE(cachedGeometryRevision_);MATERIAL_EXCHANGE(cachedMaterialRevision_);
  MATERIAL_EXCHANGE(cachedShowDiagCube_);MATERIAL_EXCHANGE(objectDataCacheValid_);MATERIAL_EXCHANGE(diagCubeObjectIndex_);
#undef MATERIAL_EXCHANGE
  ++objectDataRevision_;++objectBufferUploadRevision_;cachedMaterialRevision_=materialRevision;
  for(auto& [buffer,revision]:objectBufferUploadRevisions_) { (void)buffer;revision=objectBufferUploadRevision_; }
  state.graph=sceneGraph_.revision();state.sceneEpoch=sceneEpoch;state.sourceMaterial=materialRevision;
  state.sourceGeometry=sceneManager_.geometryRevision();state.geometry=geometryRevision_;
  state.objects=objectDataRevision_;state.upload=objectBufferUploadRevision_;
  if(state.bindingAware) state.consumed=true;
}
void SceneController::acknowledgeMaterialRevision(uint64_t revision) noexcept {
  if(revision==sceneManager_.materialRevision() && objectDataCacheValid_ &&
      cachedSceneGraphRevision_==sceneGraph_.revision() && cachedGeometryRevision_==geometryRevision_)
    cachedMaterialRevision_=revision;
}

std::span<const container::geometry::Vertex> SceneController::geometryVertices() const {
  return deformationScene_ ? deformationScene_->currentVertices()
      : std::span<const container::geometry::Vertex>(sceneManager_.vertices());
}

std::span<const uint32_t> SceneController::geometryIndices() const {
  return deformationScene_ ? deformationScene_->indices()
      : std::span<const uint32_t>(sceneManager_.indices());
}

void SceneController::rebuildDeformationBindStreams() {
  const auto sceneVertices = deformationScene_ ? deformationScene_->bindVertices()
      : std::span<const container::geometry::Vertex>(sceneManager_.vertices());
  const auto sceneIndices = geometryIndices();
  const auto cube = makeScenePrimitiveCube();
  constexpr auto maximum = std::numeric_limits<uint32_t>::max();
  if (sceneVertices.size() > maximum - cube.vertices().size() ||
      sceneIndices.size() > maximum - cube.indices().size())
    throw std::runtime_error("Expanded scene and diagnostic geometry exceed 32-bit indexing");
  gpuBindVertices_.assign(sceneVertices.begin(),sceneVertices.end());
  generationSceneVertexCount_=sceneVertices.size(); generationSceneIndexCount_=sceneIndices.size();
  const uint32_t diagnosticVertexBase = static_cast<uint32_t>(gpuBindVertices_.size());
  gpuBindVertices_.insert(gpuBindVertices_.end(),cube.vertices().begin(),cube.vertices().end());
  gpuBindIndices_.assign(sceneIndices.begin(),sceneIndices.end());
  for (uint32_t index : cube.indices()) gpuBindIndices_.push_back(diagnosticVertexBase+index);
}

GltfDeformationSceneUpdate SceneController::updateDeformation(bool resetHistory) {
  const uint64_t sourceRevision = sceneManager_.geometryRevision();
  const bool sourceChanged = sourceRevision != deformationSourceGeometryRevision_;
  if (sourceChanged) {
    if (const auto* data = sceneManager_.deformationData()) {
      if (!deformationScene_) deformationScene_ = std::make_unique<GltfDeformationScene>();
      deformationScene_->configure(*data,sceneManager_.model(),
                                   sceneManager_.sourcePrimitiveToRuntime());
    } else deformationScene_.reset();
    deformationTopologyPending_ = true;
    rebuildPrimitiveBoundsCache();
  }
  GltfDeformationSceneUpdate result;
  if (deformationScene_) {
    sceneGraph_.updateWorldTransforms();
    std::vector<uint32_t> owners(sceneGraph_.nodeCount(),container::scene::SceneGraph::kInvalidNode);
    for (uint32_t index = 0; index < sceneGraph_.nodeCount(); ++index)
      if (const auto* node = sceneGraph_.getNode(index)) owners[index] = node->meshOwnerGraphNode;
    result = deformationScene_->update(sceneGraph_,owners,&sceneManager_.animationSource(),resetHistory);
  }
  if (sourceChanged || result.topologyChanged) {
    deformationTopologyPending_ = true;
    rebuildDeformationBindStreams();
  }
  if (sourceChanged || result.geometryChanged || result.topologyChanged) {
    ++geometryRevision_;
    invalidateObjectDataCache();
  }
  deformationSourceGeometryRevision_ = sourceRevision;
  result.geometryChanged |= sourceChanged;
  result.topologyChanged = deformationTopologyPending_;
  result.historyReset |= resetHistory || sourceChanged;
  return result;
}

void SceneController::commitDeformationFrame() {
  if (deformationScene_) deformationScene_->commitSubmitted();
}

void SceneController::rebuildPrimitiveBoundsCache() {
  const auto& primitiveRanges = sceneManager_.primitiveRanges();
  const auto& verts = sceneManager_.vertices();
  const auto& indices = sceneManager_.indices();

  primitiveBounds_.assign(primitiveRanges.size(), {});
  for (size_t primitiveIndex = 0; primitiveIndex < primitiveRanges.size();
       ++primitiveIndex) {
    const auto& primitive = primitiveRanges[primitiveIndex];
    const size_t begin = primitive.firstIndex;
    if (begin >= indices.size() || primitive.indexCount == 0u) {
      continue;
    }
    const size_t end =
        std::min(indices.size(), begin + static_cast<size_t>(primitive.indexCount));

    bool hasVertex = false;
    glm::vec3 localMin(std::numeric_limits<float>::max());
    glm::vec3 localMax(std::numeric_limits<float>::lowest());
    for (size_t i = begin; i < end; ++i) {
      const uint32_t vertexIndex = indices[i];
      if (vertexIndex >= verts.size()) {
        continue;
      }
      const glm::vec3& pos = verts[vertexIndex].position;
      localMin = glm::min(localMin, pos);
      localMax = glm::max(localMax, pos);
      hasVertex = true;
    }
    if (!hasVertex) {
      continue;
    }

    const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
    float localRadius = 0.0f;
    for (size_t i = begin; i < end; ++i) {
      const uint32_t vertexIndex = indices[i];
      if (vertexIndex >= verts.size()) {
        continue;
      }
      localRadius = std::max(
          localRadius, glm::length(verts[vertexIndex].position - localCenter));
    }
    primitiveBounds_[primitiveIndex] = {localCenter, localRadius, true};
  }
}

void SceneController::invalidateObjectDataCache() {
  objectDataCacheValid_ = false;
  cachedSceneGraphRevision_ = std::numeric_limits<uint64_t>::max();
  cachedGeometryRevision_ = std::numeric_limits<uint64_t>::max();
  cachedMaterialRevision_ = std::numeric_limits<uint64_t>::max();
  objectBufferUploadRevisions_.clear();
}

void SceneController::refreshObjectDataCache(bool showDiagCube) {
  updateDeformation();
  const uint64_t sceneGraphRevision = sceneGraph_.revision();
  if (objectDataCacheValid_ &&
      cachedSceneGraphRevision_ == sceneGraphRevision &&
      cachedGeometryRevision_ == geometryRevision_ &&
      cachedMaterialRevision_ == sceneManager_.materialRevision() &&
      cachedShowDiagCube_ == showDiagCube) {
    return;
  }

  syncObjectDataFromSceneGraph(showDiagCube);
}

// ---------------------------------------------------------------------------
// Static buffer helpers
// ---------------------------------------------------------------------------

void SceneController::writeToBuffer(
    container::gpu::AllocationManager&      allocationManager,
    const container::gpu::AllocatedBuffer&  buffer,
    const void*                              data,
    size_t                                   size) {
  void* mapped = buffer.allocation_info.pMappedData;
  bool  mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map buffer for writing");
    }
    mappedHere = true;
  }

  std::memcpy(mapped, data, size);
  if (vmaFlushAllocation(allocationManager.memoryManager()->allocator(),
                         buffer.allocation, 0,
                         static_cast<VkDeviceSize>(size)) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation);
    }
    throw std::runtime_error("failed to flush buffer after writing");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                   buffer.allocation);
  }
}

bool SceneController::ensureObjectBufferCapacity(
    container::gpu::AllocationManager&  allocationManager,
    container::gpu::AllocatedBuffer&    objectBuffer,
    size_t&                              objectBufferCapacity,
    size_t                               requiredObjectCount,
    VkBufferUsageFlags                   additionalUsage) {
  const size_t required = std::max<size_t>(1, requiredObjectCount);
  if (objectBuffer.buffer != VK_NULL_HANDLE &&
      objectBufferCapacity >= required) {
    return false;
  }

  if (objectBuffer.buffer != VK_NULL_HANDLE) {
    allocationManager.destroyBuffer(objectBuffer);
    objectBufferCapacity = 0;
  }

  objectBuffer = allocationManager.createBuffer(
      sizeof(ObjectData) * required, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|additionalUsage,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  objectBufferCapacity = required;
  return true;
}

// ---------------------------------------------------------------------------
// Geometry buffers
// ---------------------------------------------------------------------------

void SceneController::createGeometryBuffers() {
  updateDeformation();
  const auto sceneIndices = geometryIndices();
  const container::geometry::Mesh cube = makeScenePrimitiveCube();
  const VkDeviceSize diagIndexByteOffset =
      static_cast<VkDeviceSize>(sceneIndices.size()) * sizeof(uint32_t);

  if (ownedGenerationBuffers_) {
    auto replacement=OwnedGenerationBuffers::geometry(allocationManager_,device_,gpuBindVertices_,gpuBindIndices_);
    // Existing frontend aliases still identify these owned per-image buffers.
    // A topology update replaces geometry only, after retiring its readers.
    replacement->objects.swap(ownedGenerationBuffers_->objects);
    replacement->capacities.swap(ownedGenerationBuffers_->capacities);
    ownedGenerationBuffers_=std::move(replacement);
    vertexSlice_={ownedGenerationBuffers_->vertices.buffer,0,gpuBindVertices_.size()*sizeof(container::geometry::Vertex)};
    indexSlice_={ownedGenerationBuffers_->indices.buffer,0,gpuBindIndices_.size()*sizeof(uint32_t)};
  } else {
    vertexSlice_ = allocationManager_.uploadVertices(
        std::span<const container::geometry::Vertex>(gpuBindVertices_));
    indexSlice_ = allocationManager_.uploadIndices(
        std::span<const uint32_t>(gpuBindIndices_));
  }

  diagCubeVertexSlice_ = vertexSlice_;
  diagCubeIndexSlice_  = indexSlice_;
  diagCubeIndexSlice_.offset = indexSlice_.offset + diagIndexByteOffset;
  diagCubeIndexSlice_.size =
      static_cast<VkDeviceSize>(cube.indices().size()) * sizeof(uint32_t);
  diagCubeIndexCount_ = static_cast<uint32_t>(cube.indices().size());
  deformationTopologyPending_ = false;
  rebuildPrimitiveBoundsCache();
  invalidateObjectDataCache();
}

void SceneController::createSceneBuffers(
    const container::gpu::AllocatedBuffer& cameraBuffer,
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity) {
  const VkBuffer previousObjectBuffer = objectBuffer.buffer;
  const bool bufferRecreated =
      ensureObjectBufferCapacity(allocationManager_, objectBuffer,
                                 objectBufferCapacity,
                                 sceneGraph_.renderableNodes().size());
  if (bufferRecreated) {
    objectBufferUploadRevisions_.erase(previousObjectBuffer);
    objectBufferUploadRevisions_.erase(objectBuffer.buffer);
  }
  refreshObjectDataCache(false);
  // Upload is deferred to updateObjectBuffer; descriptor sets updated by
  // caller.
}

// ---------------------------------------------------------------------------
// syncObjectDataFromSceneGraph
// ---------------------------------------------------------------------------

void SceneController::syncObjectDataFromSceneGraph(bool showDiagCube) {
  sceneGraph_.updateWorldTransforms();
  updateDeformation();

  // Sync ECS registry from the scene graph so views are up-to-date.
  world_->syncFromSceneGraph(sceneGraph_);

  objectData_.clear();
  objectNodeIndices_.clear();
  opaqueDrawCommands_.clear();
  transparentDrawCommands_.clear();
  opaqueSingleSidedDrawCommands_.clear();
  opaqueWindingFlippedDrawCommands_.clear();
  opaqueDoubleSidedDrawCommands_.clear();
  transparentSingleSidedDrawCommands_.clear();
  transparentWindingFlippedDrawCommands_.clear();
  transparentDoubleSidedDrawCommands_.clear();
  const uint32_t renderableCount = world_->renderableCount();
  objectData_.reserve(renderableCount);
  objectNodeIndices_.reserve(renderableCount);
  opaqueDrawCommands_.reserve(renderableCount);
  transparentDrawCommands_.reserve(renderableCount);
  opaqueSingleSidedDrawCommands_.reserve(renderableCount);
  opaqueWindingFlippedDrawCommands_.reserve(renderableCount);
  opaqueDoubleSidedDrawCommands_.reserve(renderableCount);
  transparentSingleSidedDrawCommands_.reserve(renderableCount);
  transparentWindingFlippedDrawCommands_.reserve(renderableCount);
  transparentDoubleSidedDrawCommands_.reserve(renderableCount);

  world_->forEachRenderableWithNode(
      [this](const container::ecs::TransformComponent& transform,
             const container::ecs::MeshComponent&      mesh,
             const container::ecs::MaterialComponent&   material,
             const container::ecs::SceneNodeRef&        nodeRef) {
        if (mesh.primitiveIndex == std::numeric_limits<uint32_t>::max() ||
            mesh.primitiveIndex >= sceneManager_.primitiveRanges().size()) {
          return;
        }

        const auto& primitive =
            sceneManager_.primitiveRanges()[mesh.primitiveIndex];
        const auto* deformation = deformationScene_ ? deformationScene_->drawRange(nodeRef.nodeIndex) : nullptr;
        const auto materialProperties =
            sceneManager_.materialRenderProperties(material.materialIndex);
        const uint32_t materialIndex = materialProperties.gpuMaterialIndex;

        ObjectData object{};
        object.model = transform.worldTransform;
        object.objectInfo.x = materialIndex;
        object.temporalInfo.z = nodeRef.nodeIndex;
        object.temporalInfo.w = mesh.primitiveIndex;
        {
          const glm::mat3 model3  = glm::mat3(transform.worldTransform);
          const glm::mat3 normal3 = glm::transpose(glm::inverse(model3));
          object.normalMatrix0 = glm::vec4(normal3[0], 0.0f);
          object.normalMatrix1 = glm::vec4(normal3[1], 0.0f);
          object.normalMatrix2 = glm::vec4(normal3[2], 0.0f);
        }
        const bool materialDoubleSided = materialProperties.doubleSided;
        const bool materialTransparent = materialProperties.transparent;
        const bool rasterDoubleSided =
            materialDoubleSided || primitive.disableBackfaceCulling ||
            (deformation && deformation->orientation == GltfDeformationOrientation::MixedOrDegenerate);
        const bool windingFlipped =
            transformFlipsWinding(transform.worldTransform) !=
            (deformation && deformation->orientation == GltfDeformationOrientation::Reversed);
        object.objectInfo.y =
            rasterDoubleSided ? container::gpu::kObjectFlagDoubleSided : 0u;
        if (deformation && deformation->orientation == GltfDeformationOrientation::Reversed)
          object.objectInfo.y |= container::gpu::kObjectFlagDeformationWindingFlipped;

        if (deformation || mesh.primitiveIndex < primitiveBounds_.size()) {
          const PrimitiveBounds bounds = deformation
            ? PrimitiveBounds{deformation->bounds.center,deformation->bounds.radius,deformation->bounds.valid}
            : primitiveBounds_[mesh.primitiveIndex];
          if (bounds.valid) {
            const glm::vec3 worldCenter = glm::vec3(
                transform.worldTransform * glm::vec4(bounds.center, 1.0f));
            const float scaleMax = container::math::conservativeSphereScale(transform.worldTransform);
            const float heightInflation =
                std::abs(materialProperties.heightScale) * scaleMax;
            object.boundingSphere =
                glm::vec4(worldCenter, bounds.radius * scaleMax + heightInflation);
          }
        }

        const uint32_t objectIndex =
            static_cast<uint32_t>(objectData_.size());
        objectData_.push_back(object);
        objectNodeIndices_.push_back(nodeRef.nodeIndex);

        DrawCommand drawCommand{};
        drawCommand.objectIndex = objectIndex;
        drawCommand.firstIndex  = deformation ? deformation->firstIndex : primitive.firstIndex;
        drawCommand.indexCount  = deformation ? deformation->indexCount : primitive.indexCount;

        if (materialTransparent) {
          transparentDrawCommands_.push_back(drawCommand);
          if (rasterDoubleSided) {
            transparentDoubleSidedDrawCommands_.push_back(drawCommand);
          } else if (windingFlipped) {
            transparentWindingFlippedDrawCommands_.push_back(drawCommand);
          } else {
            transparentSingleSidedDrawCommands_.push_back(drawCommand);
          }
        } else {
          opaqueDrawCommands_.push_back(drawCommand);
          if (rasterDoubleSided) {
            opaqueDoubleSidedDrawCommands_.push_back(drawCommand);
          } else if (windingFlipped) {
            opaqueWindingFlippedDrawCommands_.push_back(drawCommand);
          } else {
            opaqueSingleSidedDrawCommands_.push_back(drawCommand);
          }
        }
      });

  // Diagnostic cube: appended last so its objectIndex is always known.
  diagCubeObjectIndex_ = std::numeric_limits<uint32_t>::max();
  if (showDiagCube && diagCubeVertexSlice_.buffer != VK_NULL_HANDLE) {
    ObjectData cubeObject{};
    glm::vec3  diagCenter{0.0f};
    float      diagScale = 0.5f;
    const auto& bounds = sceneManager_.modelBounds();
    if (bounds.valid) {
      diagCenter = bounds.center +
                   glm::vec3(bounds.radius * 1.75f, 0.0f, 0.0f);
      diagScale = std::max(0.25f, bounds.radius * 0.45f);
    }
    const glm::mat4 cubeModel =
        glm::translate(glm::mat4(1.0f), diagCenter) *
        glm::scale(glm::mat4(1.0f), glm::vec3(diagScale));
    cubeObject.model = cubeModel;
    const glm::mat3 cubeNormal =
        glm::transpose(glm::inverse(glm::mat3(cubeModel)));
    cubeObject.normalMatrix0 = glm::vec4(cubeNormal[0], 0.0f);
    cubeObject.normalMatrix1 = glm::vec4(cubeNormal[1], 0.0f);
    cubeObject.normalMatrix2 = glm::vec4(cubeNormal[2], 0.0f);
    cubeObject.objectInfo.x = sceneManager_.diagnosticMaterialIndex();
    cubeObject.temporalInfo.z = container::scene::SceneGraph::kInvalidNode;
    cubeObject.temporalInfo.w = std::numeric_limits<uint32_t>::max();
    diagCubeObjectIndex_ = static_cast<uint32_t>(objectData_.size());
    objectData_.push_back(cubeObject);
    objectNodeIndices_.push_back(container::scene::SceneGraph::kInvalidNode);
  }

  cachedSceneGraphRevision_ = sceneGraph_.revision();
  cachedGeometryRevision_ = geometryRevision_;
  cachedMaterialRevision_ = sceneManager_.materialRevision();
  cachedShowDiagCube_ = showDiagCube;
  objectDataCacheValid_ = true;
  ++objectDataRevision_;
  ++objectBufferUploadRevision_;
}

// ---------------------------------------------------------------------------
// updateObjectBuffer
// ---------------------------------------------------------------------------

bool SceneController::updateObjectBuffer(
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity,
    const container::gpu::AllocatedBuffer& cameraBuffer,
    bool deferUploadToGpu) {
  lastObjectUploadBytes_=0;
  const VkBuffer previousObjectBuffer = objectBuffer.buffer;
  const bool showDiagCube =
      guiManager_ && guiManager_->showNormalDiagCube() &&
      diagCubeVertexSlice_.buffer != VK_NULL_HANDLE;
  refreshObjectDataCache(showDiagCube);

  bool bufferRecreated=false;
  auto* objectOwner=ownedMaterialObjectBuffers_?ownedMaterialObjectBuffers_.get():ownedGenerationBuffers_.get();
  if (objectOwner && ownsObjectBuffer(objectBuffer.buffer)) {
    const auto found=std::ranges::find(objectOwner->objects,objectBuffer.buffer,
        &container::gpu::AllocatedBuffer::buffer);
    const auto index=size_t(found-objectOwner->objects.begin());
    try {
      bufferRecreated=ensureObjectBufferCapacity(allocationManager_,*found,
          objectOwner->capacities[index],objectData_.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    } catch (...) {
      objectBuffer=*found; objectBufferCapacity=objectOwner->capacities[index];
      throw;
    }
    objectBuffer=*found; objectBufferCapacity=objectOwner->capacities[index];
  } else {
    bufferRecreated = ensureObjectBufferCapacity(
        allocationManager_, objectBuffer, objectBufferCapacity, objectData_.size());
  }
  if (bufferRecreated) {
    objectBufferUploadRevisions_.erase(previousObjectBuffer);
    objectBufferUploadRevisions_.erase(objectBuffer.buffer);
  }
  if (objectBuffer.buffer == VK_NULL_HANDLE || objectData_.empty())
    return bufferRecreated;
  if(deferUploadToGpu) {
    // Compute publishes the entire record. A later CPU user must perform a
    // full upload even if the authoring revision has remained unchanged.
    objectBufferUploadRevisions_.erase(objectBuffer.buffer);
    return bufferRecreated;
  }

  const auto uploadRecord =
      objectBufferUploadRevisions_.find(objectBuffer.buffer);
  const uint64_t uploadedRevision =
      uploadRecord == objectBufferUploadRevisions_.end()
          ? std::numeric_limits<uint64_t>::max()
          : uploadRecord->second;
  if (uploadedRevision != objectBufferUploadRevision_) {
    writeToBuffer(allocationManager_, objectBuffer, objectData_.data(),
                  sizeof(ObjectData) * objectData_.size());
    lastObjectUploadBytes_=sizeof(ObjectData)*objectData_.size();
    objectBufferUploadRevisions_[objectBuffer.buffer] =
        objectBufferUploadRevision_;
  }
  return bufferRecreated;
}

uint32_t SceneController::pickRenderableNode(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableNodeHit(cameraData, viewportExtent, cursorX, cursorY,
                               sectionPlaneEnabled, sectionPlane)
      .nodeIndex;
}

SceneNodePickHit SceneController::pickRenderableNodeHit(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableNodeHitForDraws(cameraData, viewportExtent, cursorX,
                                       cursorY, true, true,
                                       sectionPlaneEnabled, sectionPlane);
}

SceneNodePickHit SceneController::pickTransparentRenderableNodeHit(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  return pickRenderableNodeHitForDraws(cameraData, viewportExtent, cursorX,
                                       cursorY, false, true,
                                       sectionPlaneEnabled, sectionPlane);
}

SceneNodePickHit SceneController::pickRenderableNodeHitForDraws(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool includeOpaque,
    bool includeTransparent,
    bool sectionPlaneEnabled,
    glm::vec4 sectionPlane) const {
  if (objectData_.empty()) {
    return {};
  }

  const PickRay ray = makePickRay(cameraData, viewportExtent, cursorX, cursorY);
  if (!ray.valid) {
    return {};
  }

  const auto vertices = geometryVertices();
  const auto indices = geometryIndices();
  SceneNodePickHit nearest{};
  auto testDrawCommands = [&](const std::vector<DrawCommand>& commands,
                              TriangleCullMode cullMode) {
    for (const DrawCommand& command : commands) {
      if (command.firstIndex >= indices.size() || command.indexCount < 3u) {
        continue;
      }
      const size_t firstIndex = command.firstIndex;
      const size_t indexCount =
          std::min(static_cast<size_t>(command.indexCount),
                   indices.size() - firstIndex);
      const size_t endIndex = firstIndex + indexCount;
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);

      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        if (objectIndex >= objectData_.size() ||
            objectIndex >= objectNodeIndices_.size()) {
          continue;
        }
        const uint32_t nodeIndex = objectNodeIndices_[objectIndex];
        if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
          continue;
        }

        const glm::vec4 sphere = objectData_[objectIndex].boundingSphere;
        float sphereDistance = 0.0f;
        if (sphere.w > 0.0f &&
            (!intersectRaySphere(ray.origin, ray.direction, glm::vec3(sphere),
                                 sphere.w, sphereDistance) ||
             sphereDistance > nearest.distance)) {
          continue;
        }

        const glm::mat4& model = objectData_[objectIndex].model;
        for (size_t index = firstIndex; index + 2u < endIndex; index += 3u) {
          const uint32_t i0 = indices[index];
          const uint32_t i1 = indices[index + 1u];
          const uint32_t i2 = indices[index + 2u];
          if (i0 >= vertices.size() || i1 >= vertices.size() ||
              i2 >= vertices.size()) {
            continue;
          }

          const glm::vec3 v0 =
              glm::vec3(model * glm::vec4(vertices[i0].position, 1.0f));
          const glm::vec3 v1 =
              glm::vec3(model * glm::vec4(vertices[i1].position, 1.0f));
          const glm::vec3 v2 =
              glm::vec3(model * glm::vec4(vertices[i2].position, 1.0f));
          float hitDistance = 0.0f;
          if (intersectRayTriangle(ray.origin, ray.direction, v0, v1, v2,
                                   cullMode, hitDistance) &&
              hitDistance < nearest.distance) {
            const glm::vec3 hitPosition =
                ray.origin + ray.direction * hitDistance;
            if (sectionPlaneClips(hitPosition, sectionPlaneEnabled,
                                  sectionPlane)) {
              continue;
            }
            nearest.nodeIndex = nodeIndex;
            nearest.distance = hitDistance;
            nearest.depth = projectDepth(cameraData, hitPosition);
            nearest.worldPosition = hitPosition;
            nearest.hasWorldPosition = true;
            nearest.hit = true;
          }
        }
      }
    }
  };

  const bool hasSplitDrawCommands =
      !opaqueSingleSidedDrawCommands_.empty() ||
      !transparentSingleSidedDrawCommands_.empty() ||
      !opaqueWindingFlippedDrawCommands_.empty() ||
      !transparentWindingFlippedDrawCommands_.empty() ||
      !opaqueDoubleSidedDrawCommands_.empty() ||
      !transparentDoubleSidedDrawCommands_.empty();
  if (hasSplitDrawCommands) {
    if (includeOpaque) {
      testDrawCommands(opaqueSingleSidedDrawCommands_, TriangleCullMode::Back);
    }
    if (includeTransparent) {
      testDrawCommands(transparentSingleSidedDrawCommands_,
                       TriangleCullMode::Back);
    }
    if (includeOpaque) {
      testDrawCommands(opaqueWindingFlippedDrawCommands_,
                       TriangleCullMode::Front);
    }
    if (includeTransparent) {
      testDrawCommands(transparentWindingFlippedDrawCommands_,
                       TriangleCullMode::Front);
    }
    if (includeOpaque) {
      testDrawCommands(opaqueDoubleSidedDrawCommands_, TriangleCullMode::None);
    }
    if (includeTransparent) {
      testDrawCommands(transparentDoubleSidedDrawCommands_,
                       TriangleCullMode::None);
    }
  } else {
    if (includeOpaque) {
      testDrawCommands(opaqueDrawCommands_, TriangleCullMode::None);
    }
    if (includeTransparent) {
      testDrawCommands(transparentDrawCommands_, TriangleCullMode::None);
    }
  }

  return nearest;
}

uint32_t SceneController::nodeIndexForObject(uint32_t objectIndex) const {
  if (objectIndex >= objectNodeIndices_.size()) {
    return container::scene::SceneGraph::kInvalidNode;
  }
  return objectNodeIndices_[objectIndex];
}

SceneNodeWorldBounds SceneController::nodeWorldBounds(uint32_t nodeIndex) const {
  if (nodeIndex == container::scene::SceneGraph::kInvalidNode ||
      objectData_.empty()) {
    return {};
  }

  std::unordered_set<uint32_t> nodeSubtree;
  std::vector<uint32_t> stack{nodeIndex};
  while (!stack.empty()) {
    const uint32_t current = stack.back();
    stack.pop_back();
    const auto* currentNode = sceneGraph_.getNode(current);
    if (!currentNode || currentNode->retired) continue;
    if (!nodeSubtree.insert(current).second) {
      continue;
    }
    if (const auto *node = currentNode) {
      for (const uint32_t child : node->children) {
        stack.push_back(child);
      }
    }
  }
  if (nodeSubtree.empty()) {
    return {};
  }

  glm::vec3 boundsMin{std::numeric_limits<float>::max()};
  glm::vec3 boundsMax{std::numeric_limits<float>::lowest()};
  bool hasBounds = false;
  for (uint32_t objectIndex = 0u;
       objectIndex < objectData_.size() && objectIndex < objectNodeIndices_.size();
       ++objectIndex) {
    if (!nodeSubtree.contains(objectNodeIndices_[objectIndex]) ||
        !sceneGraph_.isNodeActive(objectNodeIndices_[objectIndex])) {
      continue;
    }

    const glm::vec4 sphere = objectData_[objectIndex].boundingSphere;
    const glm::vec3 center{sphere};
    const float radius = std::max(sphere.w, 0.0f);
    if (radius > 0.0f && std::isfinite(radius)) {
      expandBounds(boundsMin, boundsMax, center - glm::vec3(radius));
      expandBounds(boundsMin, boundsMax, center + glm::vec3(radius));
      hasBounds = true;
      continue;
    }

    const glm::vec3 origin{objectData_[objectIndex].model[3]};
    expandBounds(boundsMin, boundsMax, origin);
    hasBounds = true;
  }

  if (!hasBounds) {
    return {};
  }

  SceneNodeWorldBounds bounds{};
  bounds.valid = true;
  bounds.min = boundsMin;
  bounds.max = boundsMax;
  bounds.center = (boundsMin + boundsMax) * 0.5f;
  bounds.size = boundsMax - boundsMin;
  bounds.radius = std::max(0.25f, glm::length(bounds.size) * 0.5f);
  return bounds;
}

void SceneController::collectDrawCommandsForNode(
    uint32_t nodeIndex, std::vector<DrawCommand>& outCommands) const {
  outCommands.clear();
  if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
    return;
  }

  auto appendMatching = [&](const std::vector<DrawCommand>& commands) {
    for (const DrawCommand& command : commands) {
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);
      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        if (objectIndex < objectNodeIndices_.size() &&
            objectNodeIndices_[objectIndex] == nodeIndex) {
          DrawCommand selected = command;
          selected.objectIndex = objectIndex;
          selected.instanceCount = 1u;
          outCommands.push_back(selected);
        }
      }
    }
  };

  appendMatching(opaqueDrawCommands_);
  appendMatching(transparentDrawCommands_);
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

void SceneController::buildSceneGraph(uint32_t& outRootNode,
                                      uint32_t& outSelectedMeshNode,
                                      uint32_t& outCubeNode) {
  sceneManager_.populateSceneGraph(sceneGraph_);
  outRootNode = container::scene::SceneGraph::kInvalidNode;

  const uint32_t existingNodeCount =
      static_cast<uint32_t>(sceneGraph_.nodeCount());
  if (existingNodeCount > 0) {
    outRootNode = sceneGraph_.createNode(
        glm::mat4(1.0f), sceneManager_.defaultMaterialIndex(), false,
        container::scene::SceneGraph::kInvalidNode, "Scene Root");
    sceneGraph_.getNode(outRootNode)->editorSceneRoot = true;
    for (uint32_t i = 0; i < existingNodeCount; ++i) {
      const auto* node = sceneGraph_.getNode(i);
      if (node != nullptr &&
          node->parent == container::scene::SceneGraph::kInvalidNode) {
        sceneGraph_.setParent(i, outRootNode);
      }
    }
    sceneGraph_.updateWorldTransforms();
  }

  const auto& renderable = sceneGraph_.renderableNodes();
  if (renderable.empty()) {
    outSelectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  } else if (std::find(renderable.begin(), renderable.end(),
                       outSelectedMeshNode) == renderable.end()) {
    outSelectedMeshNode = renderable.front();
  }
  outCubeNode = container::scene::SceneGraph::kInvalidNode;
  invalidateObjectDataCache();
}

void SceneController::addSceneObject(
    const glm::mat4&                         transform,
    uint32_t                                 rootNode,
    container::gpu::AllocatedBuffer&        objectBuffer,
    size_t&                                  objectBufferCapacity,
    const container::gpu::AllocatedBuffer&  cameraBuffer) {
  if (sceneGraph_.renderableNodes().size() >= config_.maxSceneObjects) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Reached maximum scene object capacity");
    }
    return;
  }
  const uint32_t node = sceneGraph_.createNode(
      transform, sceneManager_.defaultMaterialIndex(), true);
  sceneGraph_.setParent(node, rootNode);
  sceneGraph_.updateWorldTransforms();
  sceneManager_.registerActiveSceneNode(sceneGraph_,node);
  updateObjectBuffer(objectBuffer, objectBufferCapacity, cameraBuffer);
  if (guiManager_) {
    guiManager_->setStatusMessage("Added object to scene");
  }
}

uint32_t SceneController::appendScenePrimitiveGeometry(ScenePrimitiveKind kind) {
  if (sceneGraph_.renderableNodes().size() >= config_.maxSceneObjects) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Reached maximum scene object capacity");
    }
    return container::scene::SceneGraph::kInvalidNode;
  }

  const int32_t materialIndex =
      sceneManager_.defaultMaterialIndex() ==
              std::numeric_limits<uint32_t>::max()
          ? -1
          : static_cast<int32_t>(sceneManager_.defaultMaterialIndex());
  const uint32_t primitiveIndex =
      sceneManager_.appendRuntimeMesh(makeScenePrimitive(kind, materialIndex));
  if (primitiveIndex == std::numeric_limits<uint32_t>::max()) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Failed to add primitive");
    }
    return container::scene::SceneGraph::kInvalidNode;
  }
  invalidateObjectDataCache();
  return primitiveIndex;
}

uint32_t SceneController::addScenePrimitive(ScenePrimitiveKind kind,
                                            const glm::mat4& transform,
                                            uint32_t& rootNode) {
  const auto primitiveIndex=appendScenePrimitiveGeometry(kind);
  if(primitiveIndex==container::scene::SceneGraph::kInvalidNode) return primitiveIndex;
  if (rootNode == container::scene::SceneGraph::kInvalidNode) {
    rootNode = sceneGraph_.createNode(glm::mat4(1.0f),
                                      sceneManager_.defaultMaterialIndex(),
                                      false);
    sceneGraph_.getNode(rootNode)->editorSceneRoot = true;
  }

  const uint32_t node = sceneGraph_.createNode(
      transform, sceneManager_.defaultMaterialIndex(), true, primitiveIndex,
      std::string(scenePrimitiveKindLabel(kind)));
  sceneGraph_.setParent(node, rootNode);
  sceneGraph_.updateWorldTransforms();
  sceneManager_.registerActiveSceneNode(sceneGraph_,node);
  createGeometryBuffers();
  invalidateObjectDataCache();

  if (guiManager_) {
    guiManager_->setStatusMessage("Added " +
                                  std::string(scenePrimitiveKindLabel(kind)));
  }
  return node;
}

bool SceneController::reloadSceneModel(
    const std::string&                      path,
    float                                   importScale,
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity,
    const container::gpu::AllocatedBuffer& cameraBuffer,
    VkIndexType&                            outIndexType,
    uint32_t&                               outRootNode,
    uint32_t&                               outSelectedMeshNode,
    uint32_t&                               outCubeNode,
    container::geometry::GltfLoadResult*      prepared) {
  vkDeviceWaitIdle(device_->device());
  const std::array<container::gpu::AllocatedBuffer, 1> cameraBuffers = {
      cameraBuffer};
  const uint64_t previousGeometryRevision = sceneManager_.geometryRevision();
  const bool result =
      sceneManager_.reloadModel(path, importScale, cameraBuffers, objectBuffer,
                               prepared);
  if (!result && sceneManager_.geometryRevision() == previousGeometryRevision) {
    if (guiManager_)
      guiManager_->setStatusMessage("Failed to load model: " + path);
    return false;
  }
  outIndexType = sceneManager_.indexType();
  buildSceneGraph(outRootNode, outSelectedMeshNode, outCubeNode);
  ensureObjectBufferCapacity(allocationManager_, objectBuffer,
                             objectBufferCapacity,
                             sceneGraph_.renderableNodes().size());
  // The device-idle wait above retired all previous geometry readers. Reuse
  // the arenas from offset zero rather than growing them on every model load.
  allocationManager_.resetGeometryAllocations();
  createGeometryBuffers();
  if (guiManager_) {
    guiManager_->setStatusMessage(
        result ? "Loaded model: " + path : "Failed to load model: " + path);
  }
  return result;
}

}  // namespace container::renderer
