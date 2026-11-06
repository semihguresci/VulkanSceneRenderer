#include <Container/utility/SceneManager.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace container::scene {
namespace {
using namespace container::geometry::gltf;
[[noreturn]] void poseFailure(const char* message) { throw std::invalid_argument(message); }
bool exactPoseMatrix(const glm::mat4& a,const glm::mat4& b) noexcept { return std::memcmp(&a,&b,sizeof(a))==0; }
struct PoseBudget {
  uint64_t available;
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>available/width) poseFailure("Camera pose preparation exceeds its CPU budget");
    available-=count*width;
  }
};
struct PoseAncestor {
  uint32_t node{},parent{SceneGraph::kInvalidNode};
  uint64_t incarnation{};
  glm::mat4 local{1},world{1};
};
constexpr std::array<std::string_view,4> poseAreaExtensions{
    "KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"};
const tinygltf::Value* poseExtension(const tinygltf::ExtensionMap& extensions,std::string_view name) noexcept {
  const auto found=std::find_if(extensions.begin(),extensions.end(),[&](const auto& item){return item.first==name;});
  return found==extensions.end()?nullptr:&found->second;
}
struct PoseLightSourceRow { size_t source{},extensions{}; uint32_t graphNode{}; int light{-1}; uint8_t areaMask{}; };
struct PoseLightRow { size_t index{}; tinygltf::Light record; };
struct PoseAreaNodeRow { size_t source{},extension{}; tinygltf::Value record; };
struct PoseLightSourceProof {
  size_t nodes{},lights{};
  std::vector<PoseLightSourceRow> sourceRows;
  std::vector<PoseLightRow> lightRows;
  std::vector<PoseAreaNodeRow> areaRows;
  std::array<std::optional<tinygltf::Value>,4> areaDefinitions;
};
void validatePoseLimits(const CameraViewPoseLimits& limits) {
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX || !limits.maxGraphNodes ||
      limits.maxGraphNodes>1'000'000 || !limits.maxGraphWork || limits.maxGraphWork==UINT64_MAX ||
      !limits.maxDerivedWork || limits.maxDerivedWork==UINT64_MAX ||
      !limits.placement.maxPayloadBytes || limits.placement.maxPayloadBytes==UINT64_MAX ||
      !limits.placement.maxNodes || !limits.placement.maxEdges || limits.placement.maxEdges==UINT64_MAX ||
      !limits.authoring.maxOwnedBytes || limits.authoring.maxOwnedBytes==UINT64_MAX ||
      !limits.animation.maxOwnedBytes || limits.animation.maxOwnedBytes==UINT64_MAX)
    poseFailure("Camera pose requires finite preparation and graph capacities");
}
void preflightPoseDerivedWork(const SceneGraph& graph,const tinygltf::Model& source,
    std::span<const container::geometry::PrimitiveRange> primitives,size_t indexCount,uint64_t maxWork) {
  uint64_t remaining=maxWork;
  const auto charge=[&](uint64_t count,uint64_t width=1) {
    if(width && count>remaining/width) poseFailure("Camera pose derived bounds/lights exceed their work budget");
    remaining-=count*width;
  };
  charge(graph.nodeCount(),16);charge(graph.rootNodes().size(),8);charge(graph.renderableNodes().size(),4);
  for(uint32_t id=0;id<graph.nodeCount();++id) {
    const auto* node=graph.getNode(id);if(!node) poseFailure("Camera pose derived state has an unavailable graph row");
    charge(node->children.size(),8);
    if(node->sourceNodeIndex>=0) {
      if(size_t(node->sourceNodeIndex)>=source.nodes.size()) poseFailure("Camera pose derived state has an unavailable source owner");
      // Known area-property lookups scan borrowed extension keys. Account for
      // their work too, without visiting/copying arbitrary vendor metadata.
      charge(source.nodes[size_t(node->sourceNodeIndex)].extensions.size(),16);
    }
  }
  const auto ancestry=[&](uint32_t id) {
    for(size_t depth=0;depth<graph.nodeCount();++depth) {
      const auto* node=graph.getNode(id);if(!node) poseFailure("Camera pose derived state has an unavailable ancestor");
      charge(4);
      if(node->parent==SceneGraph::kInvalidNode) return;
      id=node->parent;
    }
    poseFailure("Camera pose derived state has a cyclic parent chain");
  };
  if(graph.activeRoots()) {
    charge(graph.activeRoots()->size(),8);
    for(const auto root:*graph.activeRoots()) ancestry(root);
  }
  for(const auto id:graph.renderableNodes()) {
    const auto* node=graph.getNode(id);
    if(!node || node->primitiveIndex>=primitives.size()) poseFailure("Camera pose bounds reference an unavailable primitive");
    const auto& primitive=primitives[node->primitiveIndex];
    const uint64_t first=primitive.firstIndex,count=primitive.indexCount;
    if(first>indexCount || count>uint64_t(indexCount)-first || first+count>UINT32_MAX)
      poseFailure("Camera pose bounds primitive index range is invalid or wraps uint32");
    // Both bounds passes walk visibility ancestry and every index/callback.
    // Four units per index/ancestor conservatively covers those repeated walks.
    charge(count,4);ancestry(id);
  }
}
// Count/validate the actual parent chain without SceneGraph's lazy membership
// cache. The final guard performs the same bounded, allocation-free walk.
bool poseChain(const SceneGraph& graph,uint32_t selected,const CameraViewPoseLimits& limits,
    size_t& depth,size_t& sibling) noexcept {
  if(graph.nodeCount()>limits.maxGraphNodes || selected>=graph.nodeCount()) return false;
  uint64_t work{}; depth=0; sibling=0; bool active=!graph.activeRoots();
  auto current=selected;
  for(size_t count=0;count<graph.nodeCount();++count) {
    const auto* node=graph.getNode(current);
    if(!node || node->retired || !graph.nodeIncarnation(current) || ++work>limits.maxGraphWork) return false;
    ++depth;
    if(graph.activeRoots()) {
      if(graph.activeRoots()->size()>limits.maxGraphWork-work) return false;
      work+=graph.activeRoots()->size();
      if(std::ranges::find(*graph.activeRoots(),current)!=graph.activeRoots()->end()) active=true;
    }
    const std::vector<uint32_t>* siblings=&graph.rootNodes();
    if(node->parent!=SceneGraph::kInvalidNode) {
      const auto* parent=graph.getNode(node->parent); if(!parent || parent->retired) return false;
      siblings=&parent->children;
    }
    if(siblings->size()>limits.maxGraphWork-work) return false; work+=siblings->size();
    const auto found=std::ranges::find(*siblings,current);
    if(found==siblings->end() || std::find(std::next(found),siblings->end(),current)!=siblings->end()) return false;
    if(current==selected) sibling=size_t(found-siblings->begin());
    if(node->parent==SceneGraph::kInvalidNode) return active;
    current=node->parent;
  }
  return false;
}
bool uniquePoseBinding(const SceneGraph& graph,uint32_t selected,uint64_t maxWork) noexcept {
  const auto* node=graph.getNode(selected); if(!node || node->sourceNodeIndex<0 || graph.nodeCount()>maxWork) return false;
  for(uint32_t row=0;row<graph.nodeCount();++row)
    if(row!=selected && graph.getNode(row)->sourceNodeIndex==node->sourceNodeIndex) return false;
  return true;
}
GltfCameraPoseGraphState capturePoseGraph(const SceneGraph& graph,const CameraViewPoseTarget& target,
    float aspect,const CameraViewPoseLimits& limits,PoseBudget& budget,std::vector<PoseAncestor>& ancestors) {
  size_t depth{},sibling{};
  if(!poseChain(graph,target.camera.graphNode,limits,depth,sibling) ||
      !uniquePoseBinding(graph,target.camera.graphNode,limits.maxGraphWork))
    poseFailure("Camera pose requires one current active source-node binding and a valid parent chain");
  budget.add(depth,sizeof(PoseAncestor)); ancestors.reserve(depth);
  auto current=target.camera.graphNode;
  for(size_t i=0;i<depth;++i) {
    const auto& node=*graph.getNode(current);
    ancestors.push_back({current,node.parent,graph.nodeIncarnation(current),node.localTransform,node.worldTransform});
    current=node.parent;
  }
  std::reverse(ancestors.begin(),ancestors.end());
  glm::mat4 world(1),parentWorld(1);
  for(const auto& node:ancestors) {
    parentWorld=world; world=world*node.local;
    if(!exactPoseMatrix(world,node.world)) poseFailure("Camera pose ancestor world cache is not current");
  }
  const auto& node=*graph.getNode(target.camera.graphNode);
  GltfCameraPoseGraphState result;
  result.identity=graph.identity(); result.revision=graph.revision(); result.nodeIncarnation=target.nodeIncarnation;
  result.parentIncarnation=node.parent==SceneGraph::kInvalidNode?0:graph.nodeIncarnation(node.parent);
  result.nodeCount=uint32_t(graph.nodeCount()); result.node=target.camera.graphNode; result.parent=node.parent;
  result.siblingIndex=sibling; result.parentWorld=parentWorld; result.local=node.localTransform;
  result.world=world; result.viewportAspect=aspect;
  return result;
}
bool currentPoseGraph(const SceneGraph& graph,const CameraViewPoseTarget& target,const CameraViewPoseLimits& limits,
    std::span<const PoseAncestor> ancestors,const GltfCameraPoseGraphState& expected) noexcept {
  size_t depth{},sibling{};
  if(!poseChain(graph,target.camera.graphNode,limits,depth,sibling) || depth!=ancestors.size() ||
      sibling!=expected.siblingIndex || !uniquePoseBinding(graph,target.camera.graphNode,limits.maxGraphWork) ||
      graph.identity()!=expected.identity || graph.revision()!=expected.revision || graph.nodeCount()!=expected.nodeCount ||
      graph.nodeIncarnation(target.camera.graphNode)!=expected.nodeIncarnation) return false;
  glm::mat4 world(1),parentWorld(1);
  for(const auto& row:ancestors) {
    const auto* node=graph.getNode(row.node);
    if(!node || node->retired || node->parent!=row.parent || graph.nodeIncarnation(row.node)!=row.incarnation ||
        !exactPoseMatrix(node->localTransform,row.local) || !exactPoseMatrix(node->worldTransform,row.world)) return false;
    parentWorld=world; world=world*node->localTransform;
    if(!exactPoseMatrix(world,row.world)) return false;
  }
  const auto* node=graph.getNode(target.camera.graphNode);
  return node && node->parent==expected.parent &&
      (node->parent==SceneGraph::kInvalidNode?0:graph.nodeIncarnation(node->parent))==expected.parentIncarnation &&
      exactPoseMatrix(parentWorld,expected.parentWorld) && exactPoseMatrix(world,expected.world) &&
      exactPoseMatrix(node->localTransform,expected.local);
}
void exchangeNodePose(tinygltf::Node& live,tinygltf::Node& after) noexcept {
  live.matrix.swap(after.matrix); live.translation.swap(after.translation);
  live.rotation.swap(after.rotation); live.scale.swap(after.scale);
}
bool samePoseHistoryTarget(const CameraViewPoseTarget& current,const CameraViewPoseTarget& original) noexcept {
  const auto& a=current.camera; const auto& b=original.camera;
  return current.nodeIncarnation==original.nodeIncarnation && a.asset==b.asset && a.sourceNamespace==b.sourceNamespace &&
      a.graphIdentity==b.graphIdentity && a.graphNode==b.graphNode && a.sourceNode==b.sourceNode &&
      a.authoredCamera==b.authoredCamera && a.evaluatedCamera==b.evaluatedCamera && a.incarnation==b.incarnation;
}
bool rawPoseRowsMatch(const SceneGraph& graph,std::span<const SceneNode> rows,std::span<const uint64_t> births,
    const std::vector<uint32_t>& roots,const std::vector<uint32_t>& renderables,
    const std::optional<std::vector<uint32_t>>& active) noexcept {
  if(graph.nodeCount()!=rows.size() || rows.size()!=births.size() || graph.rootNodes()!=roots ||
      graph.renderableNodes()!=renderables || graph.activeRoots()!=active) return false;
  for(uint32_t row=0;row<rows.size();++row) {
    const auto* current=graph.getNode(row);
    if(!current || *current!=rows[row] || graph.nodeIncarnation(row)!=births[row] ||
        !exactPoseMatrix(current->localTransform,rows[row].localTransform) ||
        !exactPoseMatrix(current->worldTransform,rows[row].worldTransform)) return false;
  }
  return true;
}
void capturePoseLightSources(const tinygltf::Model& source,const SceneGraph& graph,const CameraViewPoseLimits& limits,
    PoseBudget& budget,PoseLightSourceProof& proof) {
  proof.nodes=source.nodes.size(); proof.lights=source.lights.size();
  size_t rows{},areas{},potentialLights{};
  for(uint32_t id=0;id<graph.nodeCount();++id) {
    const auto* node=graph.getNode(id); if(!node || node->sourceNodeIndex<0) continue;
    if(size_t(node->sourceNodeIndex)>=source.nodes.size()) poseFailure("Camera pose graph references an unavailable source light owner");
    const auto& owner=source.nodes[size_t(node->sourceNodeIndex)]; ++rows;
    if(owner.light>=0 && size_t(owner.light)<source.lights.size()) ++potentialLights;
    for(const auto name:poseAreaExtensions) if(poseExtension(owner.extensions,name)) {++areas; ++potentialLights;}
  }
  budget.add(rows,sizeof(PoseLightSourceRow)+sizeof(size_t)); budget.add(areas,sizeof(PoseAreaNodeRow));
  // Bound evaluated arrays/bindings and temporary scene-membership traversal
  // before the existing light evaluator allocates. Only actual possible light
  // bindings contribute, rather than every unrelated source node times five.
  budget.add(potentialLights,2*(sizeof(container::gpu::PointLightData)+sizeof(container::gpu::AreaLightData)+
      sizeof(AuthoredDirectionalLight)+sizeof(AuthoredLightBinding)+128));
  budget.add(graph.nodeCount(),128); budget.add(source.nodes.size(),16);
  proof.sourceRows.reserve(rows); proof.areaRows.reserve(areas);
  std::vector<size_t> lightIds; lightIds.reserve(rows);
  for(uint32_t id=0;id<graph.nodeCount();++id) {
    const auto* node=graph.getNode(id); if(!node || node->sourceNodeIndex<0) continue;
    const auto index=size_t(node->sourceNodeIndex); const auto& owner=source.nodes[index];
    PoseLightSourceRow snapshot{index,owner.extensions.size(),id,owner.light,0};
    if(owner.light>=0 && size_t(owner.light)<source.lights.size()) lightIds.push_back(size_t(owner.light));
    for(size_t key=0;key<poseAreaExtensions.size();++key) if(const auto* value=poseExtension(owner.extensions,poseAreaExtensions[key])) {
      snapshot.areaMask|=uint8_t(1u<<key); budget.add(GltfAuthoringValueStorageBytes(*value,limits.authoring));
      proof.areaRows.emplace_back(); auto& area=proof.areaRows.back(); area.source=index; area.extension=key; area.record=*value;
    }
    proof.sourceRows.push_back(snapshot);
  }
  std::sort(lightIds.begin(),lightIds.end()); lightIds.erase(std::unique(lightIds.begin(),lightIds.end()),lightIds.end());
  budget.add(lightIds.size(),sizeof(PoseLightRow)); proof.lightRows.reserve(lightIds.size());
  for(const auto index:lightIds) {
    budget.add(GltfCameraPoseLightStorageBytes(source.lights[index],limits.authoring));
    proof.lightRows.emplace_back(); proof.lightRows.back().index=index; proof.lightRows.back().record=source.lights[index];
  }
  // Only the four supported area-light tables can affect this evaluator. Their
  // bounded metadata is preparation-only; no binary/model copy or history pin.
  for(size_t key=0;key<poseAreaExtensions.size();++key) if(const auto* value=poseExtension(source.extensions,poseAreaExtensions[key])) {
    budget.add(GltfAuthoringValueStorageBytes(*value,limits.authoring)); proof.areaDefinitions[key]=*value;
  }
}
bool poseLightSourcesMatch(const PoseLightSourceProof& proof,const tinygltf::Model& source,const SceneGraph& graph,
    const CameraViewPoseLimits& limits) noexcept {
  if(source.nodes.size()!=proof.nodes || source.lights.size()!=proof.lights) return false;
  size_t sourceBindings{};
  for(uint32_t id=0;id<graph.nodeCount();++id) if(graph.getNode(id)->sourceNodeIndex>=0) ++sourceBindings;
  if(sourceBindings!=proof.sourceRows.size()) return false;
  for(const auto& row:proof.sourceRows) {
    const auto* node=graph.getNode(row.graphNode);
    if(!node || node->sourceNodeIndex<0 || size_t(node->sourceNodeIndex)!=row.source ||
        row.source>=source.nodes.size() || source.nodes[row.source].light!=row.light ||
        source.nodes[row.source].extensions.size()!=row.extensions) return false;
    uint8_t mask{};
    for(size_t key=0;key<poseAreaExtensions.size();++key)
      if(poseExtension(source.nodes[row.source].extensions,poseAreaExtensions[key])) mask|=uint8_t(1u<<key);
    if(mask!=row.areaMask) return false;
  }
  for(const auto& row:proof.areaRows) {
    if(row.source>=source.nodes.size() || row.extension>=poseAreaExtensions.size()) return false;
    const auto* value=poseExtension(source.nodes[row.source].extensions,poseAreaExtensions[row.extension]);
    if(!value || !GltfCameraPoseValuesMatch(*value,row.record,limits.authoring)) return false;
  }
  for(const auto& row:proof.lightRows)
    if(row.index>=source.lights.size() || !GltfCameraPoseLightRecordsMatch(source.lights[row.index],row.record,limits.authoring)) return false;
  for(size_t key=0;key<poseAreaExtensions.size();++key) {
    const auto* value=poseExtension(source.extensions,poseAreaExtensions[key]);
    if(bool(value)!=bool(proof.areaDefinitions[key]) ||
        (value && !GltfCameraPoseValuesMatch(*value,*proof.areaDefinitions[key],limits.authoring))) return false;
  }
  return true;
}
} // namespace

struct SceneManager::CameraViewPoseCommand::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  CameraViewPoseTarget target;
  GltfCameraPosePlan original;
  uint64_t bytes{};
  bool published{},applied{};
};
struct SceneManager::PreparedCameraViewPoseEdit::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  CameraViewPoseTarget target;
  CameraViewPoseLimits limits;
  CameraViewPoseCommand command;
  GltfCameraPosePlan plan;
  PreparedCameraNodeRestEdit rest;
  SceneGraph::PreparedPlacement placement;
  std::vector<PoseAncestor> ancestors;
  std::vector<SceneNode> beforeRows;
  std::vector<uint64_t> beforeBirths;
  std::vector<uint32_t> beforeRoots,beforeRenderables;
  std::optional<std::vector<uint32_t>> beforeActiveRoots;
  const AnimationPlayer* player{};
  AnimatedCameraState view;
  AuthoredSceneLights lights;
  ModelBounds bounds;
  PoseLightSourceProof lightSources;
  float aspect{};
  uint64_t bytes{},geometry{},generation{},evaluation{},catalog{},membership{};
  size_t scene{};
  GltfCameraEditSide side{GltfCameraEditSide::After};
  bool replay{},expectedApplied{},consumed{};
};
SceneManager::CameraViewPoseCommand::operator bool() const noexcept { return bool(state_); }
bool SceneManager::CameraViewPoseCommand::published() const noexcept { return state_ && state_->published; }
bool SceneManager::CameraViewPoseCommand::applied() const noexcept { return state_ && state_->applied; }
uint64_t SceneManager::CameraViewPoseCommand::ownedBytes() const noexcept { return state_?state_->bytes:0; }
CameraViewPoseTarget SceneManager::CameraViewPoseCommand::target() const noexcept { return state_?state_->target:CameraViewPoseTarget{}; }
SceneManager::PreparedCameraViewPoseEdit::PreparedCameraViewPoseEdit() noexcept = default;
SceneManager::PreparedCameraViewPoseEdit::~PreparedCameraViewPoseEdit() = default;
SceneManager::PreparedCameraViewPoseEdit::PreparedCameraViewPoseEdit(PreparedCameraViewPoseEdit&&) noexcept = default;
SceneManager::PreparedCameraViewPoseEdit& SceneManager::PreparedCameraViewPoseEdit::operator=(PreparedCameraViewPoseEdit&&) noexcept = default;
SceneManager::PreparedCameraViewPoseEdit::operator bool() const noexcept { return state_ && !state_->consumed; }
bool SceneManager::PreparedCameraViewPoseEdit::changed() const noexcept { return state_ && state_->plan.changed; }
uint64_t SceneManager::PreparedCameraViewPoseEdit::ownedBytes() const noexcept { return state_?state_->bytes:0; }
SceneManager::CameraViewPoseCommand SceneManager::PreparedCameraViewPoseEdit::command() const noexcept { return state_?state_->command:CameraViewPoseCommand{}; }
const SceneGraph& SceneManager::PreparedCameraViewPoseEdit::candidateGraph() const {
  if(!*this || !state_->placement.candidateGraph()) poseFailure("Camera pose preparation has no candidate graph");
  return *state_->placement.candidateGraph();
}
const AnimatedCameraState& SceneManager::PreparedCameraViewPoseEdit::candidateView() const {
  if(!*this) poseFailure("Camera pose preparation is empty or consumed"); return state_->view;
}
const AuthoredSceneLights& SceneManager::PreparedCameraViewPoseEdit::candidateLights() const {
  if(!*this) poseFailure("Camera pose preparation is empty or consumed"); return state_->lights;
}
const ModelBounds& SceneManager::PreparedCameraViewPoseEdit::candidateBounds() const {
  if(!*this) poseFailure("Camera pose preparation is empty or consumed"); return state_->bounds;
}
std::optional<CameraViewPoseTarget> SceneManager::cameraViewPoseTarget(const SceneGraph& graph,uint32_t node) const noexcept {
  const auto camera=cameraProjectionTarget(graph,node); const auto incarnation=graph.nodeIncarnation(node);
  if(!camera || !incarnation) return {}; return CameraViewPoseTarget{*camera,incarnation};
}
SceneManager::PreparedCameraViewPoseEdit SceneManager::prepareCameraViewPoseEdit(const CameraViewPoseTarget& target,
    const glm::mat4& desired,const SceneGraph& graph,const AnimationPlayer* player,float aspect,const CameraViewPoseLimits& limits) const {
  validatePoseLimits(limits); const auto current=cameraViewPoseTarget(graph,target.camera.graphNode);
  if(!current || *current!=target) poseFailure("The selected camera node changed before writing its viewport pose");
  PoseBudget budget{limits.maxPreparedCpuBytes}; std::vector<PoseAncestor> ancestors;
  const auto context=capturePoseGraph(graph,target,aspect,limits,budget,ancestors);
  auto authoring=limits.authoring; authoring.maxOwnedBytes=std::min(authoring.maxOwnedBytes,budget.available/3);
  auto plan=PlanGltfCameraViewPose(GltfCameraPoseView(exportSource_),GltfCameraPoseView(gltfModel_),
      target.camera.sourceNode,target.camera.sourceNode,context,desired,authoring);
  // This temporary chain belongs to planner scratch, not to the later token.
  // Release it before the combined preparation builds its own exact witness.
  std::vector<PoseAncestor>().swap(ancestors);
  return prepareCameraViewPoseImpl(target,std::move(plan),nullptr,GltfCameraEditSide::After,graph,player,aspect,limits);
}
SceneManager::PreparedCameraViewPoseEdit SceneManager::prepareCameraViewPoseReplay(const CameraViewPoseCommand& command,
    GltfCameraEditSide side,const SceneGraph& graph,const AnimationPlayer* player,float aspect,const CameraViewPoseLimits& limits) const {
  validatePoseLimits(limits);
  if(!command || !command.state_->published || (side!=GltfCameraEditSide::Before && side!=GltfCameraEditSide::After))
    poseFailure("Camera pose replay requires its original published command");
  const auto& c=*command.state_; const auto current=cameraViewPoseTarget(graph,c.target.camera.graphNode);
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || !current || !samePoseHistoryTarget(*current,c.target) ||
      c.applied!=(side==GltfCameraEditSide::Before))
    poseFailure("Camera pose history belongs to a stale, foreign or wrong-side node");
  PoseBudget budget{limits.maxPreparedCpuBytes}; budget.add(command.ownedBytes()); std::vector<PoseAncestor> ancestors;
  const auto context=capturePoseGraph(graph,*current,aspect,limits,budget,ancestors);
  auto authoring=limits.authoring; authoring.maxOwnedBytes=std::min(authoring.maxOwnedBytes,budget.available/2);
  auto plan=RebaseGltfCameraViewPose(c.original,side,GltfCameraPoseView(exportSource_),GltfCameraPoseView(gltfModel_),context,authoring);
  std::vector<PoseAncestor>().swap(ancestors);
  return prepareCameraViewPoseImpl(*current,std::move(plan),&command,side,graph,player,aspect,limits);
}
SceneManager::PreparedCameraViewPoseEdit SceneManager::prepareCameraViewPoseImpl(const CameraViewPoseTarget& target,
    GltfCameraPosePlan&& plan,const CameraViewPoseCommand* original,GltfCameraEditSide side,const SceneGraph& graph,
    const AnimationPlayer* player,float aspect,const CameraViewPoseLimits& limits) const try {
  if(!player || !std::isfinite(aspect) || aspect<=0 || cameraProjectionRevision_==UINT64_MAX || cameraEvaluationRevision_==UINT64_MAX)
    poseFailure("Camera pose requires a current rest owner and valid viewport");
  const auto current=cameraViewPoseTarget(graph,target.camera.graphNode);
  if(!current || *current!=target) poseFailure("Camera pose source correspondence changed during preparation");
  PoseBudget budget{limits.maxPreparedCpuBytes}; budget.add(sizeof(PreparedCameraViewPoseEdit::State)+256);
  budget.add(plan.peakOwnedBytes);
  if(original) budget.add(original->ownedBytes());
  else { budget.add(sizeof(CameraViewPoseCommand::State)+256); budget.add(plan.peakOwnedBytes); }
  PreparedCameraViewPoseEdit result; result.state_=std::make_unique<PreparedCameraViewPoseEdit::State>(); auto& s=*result.state_;
  s.owner=this; s.ownerIdentity=sceneRootOwnerIdentity_; s.target=target; s.limits=limits; s.player=player;
  s.aspect=aspect; s.side=side; s.replay=original!=nullptr;
  s.geometry=geometryRevision_; s.generation=generationEpoch_; s.evaluation=cameraEvaluationRevision_;
  s.catalog=sceneRootCatalogIdentity_; s.membership=sceneRootMembershipRevision_; s.scene=selectedScene_;
  const auto context=capturePoseGraph(graph,target,aspect,limits,budget,s.ancestors);
  if(!GltfCameraPoseEditMatches(plan,GltfCameraPoseView(exportSource_),GltfCameraPoseView(gltfModel_),context))
    poseFailure("Camera pose source, graph or parent changed during preparation");
  s.plan=std::move(plan);
  if(original) { s.command=*original; s.expectedApplied=s.command.state_->applied; }
  else {
    s.command.state_=std::make_shared<CameraViewPoseCommand::State>(); auto& c=*s.command.state_;
    c.owner=this; c.ownerIdentity=sceneRootOwnerIdentity_; c.target=target; c.original=s.plan;
    c.bytes=sizeof(CameraViewPoseCommand::State)+256+c.original.peakOwnedBytes;
  }
  // The graph helper admits its complete detached graph/traversal footprint
  // before copying. Reserve that explicit upper bound in the combined budget;
  // it is preparation storage and never enters the selected history command.
  auto placement=limits.placement; placement.maxNodes=std::min(placement.maxNodes,limits.maxGraphNodes);
  budget.add(placement.maxPayloadBytes);
  // Publication swaps a whole candidate graph. The legacy placement receipt
  // promises revision-based matching, so additionally retain exact raw rows to
  // reject even untracked getNode() writes to unrelated nodes. This complete
  // before witness is preparation-only and never retained by history.
  budget.add(placement.maxPayloadBytes);
  s.placement=graph.preparePlacement(target.camera.graphNode,
      context.parent==SceneGraph::kInvalidNode?std::optional<uint32_t>{}:std::optional<uint32_t>{context.parent},
      s.plan.localAfter,context.siblingIndex,graph.activeRoots(),placement);
  const auto* candidate=s.placement.candidateGraph();
  if(!candidate) poseFailure("Camera pose candidate exceeds graph capacity or changes an invalid placement");
  s.beforeRows.reserve(graph.nodeCount());s.beforeBirths.reserve(graph.nodeCount());
  for(uint32_t row=0;row<graph.nodeCount();++row) {
    s.beforeRows.push_back(*graph.getNode(row));s.beforeBirths.push_back(graph.nodeIncarnation(row));
  }
  s.beforeRoots=graph.rootNodes();s.beforeRenderables=graph.renderableNodes();s.beforeActiveRoots=graph.activeRoots();
  const auto* node=candidate->getNode(target.camera.graphNode);
  if(!node || !exactPoseMatrix(node->localTransform,s.plan.localAfter) || !exactPoseMatrix(node->worldTransform,s.plan.worldAfter))
    poseFailure("Camera pose candidate graph differs from the proved float endpoint");
  const auto view=EvaluateAnimatedCamera(gltfModel_.cameras[size_t(target.camera.evaluatedCamera)],node->worldTransform,aspect);
  if(!view || !exactPoseMatrix(view->view,s.plan.viewAfter)) poseFailure("Camera pose candidate has no exact current render view");
  s.view=*view;
  preflightPoseDerivedWork(*candidate,gltfModel_,model_.primitiveRanges(),indices_.size(),limits.maxDerivedWork);
  capturePoseLightSources(gltfModel_,*candidate,limits,budget,s.lightSources);
  s.lights=evaluateAuthoredSceneLights(*candidate,selectedScene_);
  s.bounds=calculateModelBounds(candidate);
  auto animation=limits.animation; animation.authoring=limits.authoring;
  animation.maxOwnedBytes=std::min(animation.maxOwnedBytes,budget.available);
  s.rest=player->prepareCameraNodeRestEdit(s.plan,GltfCameraPoseView(exportSource_),gltfModel_,animation);
  budget.add(s.rest.ownedBytes()); s.bytes=limits.maxPreparedCpuBytes-budget.available;
  const auto* desired=s.replay?nullptr:&s.plan.desiredView;
  if(!canExchangeCameraViewPoseEdit(result,graph,player,aspect,desired))
    poseFailure("Camera pose source changed during detached preparation");
  return result;
} catch(const std::length_error& error) { throw std::invalid_argument(error.what()); }

bool SceneManager::canExchangeCameraViewPoseEdit(const PreparedCameraViewPoseEdit& token,const SceneGraph& graph,
    const AnimationPlayer* player,float aspect,const glm::mat4* currentView) const noexcept {
  if(!token) return false; const auto& s=*token.state_;
  if(s.owner!=this || s.ownerIdentity!=sceneRootOwnerIdentity_ || s.player!=player || !player ||
      std::bit_cast<uint32_t>(s.aspect)!=std::bit_cast<uint32_t>(aspect) ||
      (!s.replay && (!currentView || !exactPoseMatrix(*currentView,s.plan.desiredView))) ||
      cameraProjectionRevision_==UINT64_MAX || cameraEvaluationRevision_==UINT64_MAX ||
      graph.identity()!=s.plan.graphBefore.identity || graph.revision()!=s.plan.graphBefore.revision ||
      graph.nodeCount()!=s.plan.graphBefore.nodeCount ||
      s.geometry!=geometryRevision_ || s.generation!=generationEpoch_ || s.evaluation!=cameraEvaluationRevision_ ||
      s.catalog!=sceneRootCatalogIdentity_ || s.membership!=sceneRootMembershipRevision_ || s.scene!=selectedScene_ ||
      !rawPoseRowsMatch(graph,s.beforeRows,s.beforeBirths,s.beforeRoots,s.beforeRenderables,s.beforeActiveRoots) ||
      !poseLightSourcesMatch(s.lightSources,gltfModel_,graph,s.limits)) return false;
  const auto current=cameraViewPoseTarget(graph,s.target.camera.graphNode);
  if(!current || *current!=s.target || !s.command || s.command.state_->owner!=this ||
      s.command.state_->ownerIdentity!=sceneRootOwnerIdentity_ || !samePoseHistoryTarget(s.target,s.command.state_->target) ||
      (s.replay?(!s.command.state_->published || s.command.state_->applied!=s.expectedApplied):s.command.state_->published) ||
      !currentPoseGraph(graph,s.target,s.limits,s.ancestors,s.plan.graphBefore) || !graph.canPublishPlacement(s.placement)) return false;
  return GltfCameraPoseEditMatches(s.plan,GltfCameraPoseView(exportSource_),GltfCameraPoseView(gltfModel_),s.plan.graphBefore) &&
      player->cameraNodeRestEditMatches(s.rest,GltfCameraPoseView(exportSource_),gltfModel_);
}
bool SceneManager::exchangeCameraViewPoseEdit(PreparedCameraViewPoseEdit& token,SceneGraph& graph,
    AnimationPlayer* player,float aspect,const glm::mat4* currentView) noexcept {
  if(!canExchangeCameraViewPoseEdit(token,graph,player,aspect,currentView)) return false;
  auto& s=*token.state_;
  if(s.plan.changed) {
    if(!player->exchangeCameraNodeRestEdit(s.rest,GltfCameraPoseView(exportSource_),gltfModel_)) return false;
    exchangeNodePose(exportSource_.nodes[size_t(s.plan.authoredNode)],s.plan.authoredAfter);
    exchangeNodePose(gltfModel_.nodes[size_t(s.plan.evaluatedNode)],s.plan.evaluatedAfter);
    // The exact placement was rechecked before any source/rest swap; those
    // swaps cannot alter this single-threaded graph authority or allocate.
    if(!graph.publishPlacement(s.placement)) std::terminate();
    authoredPointLights_.swap(s.lights.points); authoredDirectionalLights_.swap(s.lights.directionals);
    authoredAreaLights_.swap(s.lights.areas); authoredPointBindings_.swap(s.lights.pointBindings);
    authoredDirectionalBindings_.swap(s.lights.directionalBindings); authoredAreaBindings_.swap(s.lights.areaBindings);
    std::swap(modelBounds_,s.bounds);
    ++cameraProjectionRevision_; ++cameraEvaluationRevision_;
  }
  s.command.state_->published=true; s.command.state_->applied=s.side==GltfCameraEditSide::After; s.consumed=true;
  return true;
}
} // namespace container::scene
