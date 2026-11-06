#include <Container/utility/SceneManager.h>
#include <Container/utility/SceneGraph.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace container::scene {
namespace {
using namespace container::geometry::gltf;
constexpr uint64_t maxCameraRows=1'000'000,maxCameraGraphWork=16'000'000;
[[noreturn]] void cameraFailure(const char* message) {throw std::invalid_argument(message);}
uint64_t cameraIdentity() {
  static std::atomic_uint64_t next{1};auto value=next.load(std::memory_order_relaxed);
  for(;;) {
    if(value==UINT64_MAX) throw std::overflow_error("Camera identity namespace exhausted");
    if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed)) return value;
  }
}
struct CameraBudget {
  uint64_t available;
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>available/width) cameraFailure("Camera projection preparation exceeds its CPU budget");
    available-=count*width;
  }
};
bool sameMatrix(const glm::mat4& a,const glm::mat4& b) noexcept {return std::memcmp(&a,&b,sizeof(a))==0;}
// Do not consult SceneGraph's lazily allocated membership cache in a final
// guard. The selected parent chain and exact active-root view are enough.
bool activeCameraNode(const SceneGraph& graph,uint32_t node,uint64_t maxNodes,uint64_t maxWork) noexcept {
  if(graph.nodeCount()>maxNodes || node>=graph.nodeCount()) return false;
  uint64_t work{};auto current=node;
  for(size_t depth=0;depth<graph.nodeCount();++depth) {
    const auto* value=graph.getNode(current);if(!value || value->retired || ++work>maxWork) return false;
    if(const auto& active=graph.activeRoots();active) {
      if(active->size()>maxWork-work) return false;work+=active->size();
      if(std::ranges::find(*active,current)!=active->end()) return true;
    }
    if(value->parent==SceneGraph::kInvalidNode) {
      if(graph.activeRoots()) return false;
      const auto& roots=graph.rootNodes();if(roots.size()>maxWork-work) return false;
      return std::ranges::find(roots,current)!=roots.end();
    }
    const auto* parent=graph.getNode(value->parent);if(!parent || parent->retired || parent->children.size()>maxWork-work) return false;
    work+=parent->children.size();
    if(std::ranges::find(parent->children,current)==parent->children.end()) return false;
    current=value->parent;
  }
  return false;
}
bool canonicalCameraNode(const SceneGraph& graph,uint32_t node,uint64_t maxWork) noexcept {
  const auto* selected=graph.getNode(node);if(!selected || node>maxWork) return false;
  for(uint32_t row=0;row<node;++row)
    if(graph.getNode(row)->sourceNodeIndex==selected->sourceNodeIndex) return false;
  return true;
}
void validateCameraLimits(const CameraProjectionLimits& limits) {
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX || !limits.maxGraphNodes ||
      limits.maxGraphNodes>maxCameraRows || !limits.maxGraphWork || limits.maxGraphWork==UINT64_MAX ||
      !limits.authoring.maxOwnedBytes || limits.authoring.maxOwnedBytes==UINT64_MAX ||
      !limits.animation.maxOwnedBytes || limits.animation.maxOwnedBytes==UINT64_MAX)
    cameraFailure("Camera projection requires finite preparation and graph capacities");
}
}

struct SceneManager::CameraProjectionIdentity {
  uint64_t incarnation{};
  int32_t authored{-1},evaluated{-1};
};
struct SceneManager::CameraProjectionNamespace {
  uint64_t identity{},bytes{};
  std::vector<std::shared_ptr<const CameraProjectionIdentity>> rows;
};
struct SceneManager::CameraProjectionCommand::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  std::shared_ptr<const CameraProjectionIdentity> identity;
  CameraProjectionTarget target;
  GltfCameraProjectionPlan original;
  uint64_t bytes{};
  bool published{},applied{};
};
struct SceneManager::PreparedCameraProjectionEdit::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  std::shared_ptr<const CameraProjectionIdentity> identity;
  CameraProjectionTarget target;
  CameraProjectionLimits limits;
  CameraProjectionCommand command;
  GltfCameraProjectionPlan plan;
  PreparedCameraRestEdit rest;
  const AnimationPlayer* player{};
  AnimatedCameraState view;
  glm::mat4 world{1},local{1};
  uint32_t parent{SceneGraph::kInvalidNode};
  float aspect{};
  uint64_t bytes{};
  GltfCameraEditSide side{GltfCameraEditSide::After};
  bool replay{},expectedApplied{},consumed{};
  std::array<tinygltf::Camera,2> scratch;
};

void SceneManager::resetCameraProjectionNamespace() {
  if(cameraProjectionRevision_==UINT64_MAX) throw std::overflow_error("Camera publication revision exhausted");
  if(gltfModel_.cameras.size()!=exportSource_.cameras.size() || gltfModel_.cameras.size()>maxCameraRows ||
      gltfModel_.nodes.size()!=exportSource_.nodes.size()) cameraFailure("Authored and evaluated camera identity tables disagree");
  for(size_t node=0;node<gltfModel_.nodes.size();++node) {
    const auto a=exportSource_.nodes[node].camera,e=gltfModel_.nodes[node].camera;
    if(a!=e || a< -1 || (a>=0 && size_t(a)>=gltfModel_.cameras.size()))
      cameraFailure("Loaded source nodes do not prove camera correspondence");
  }
  auto next=std::make_shared<CameraProjectionNamespace>();next->identity=cameraIdentity();
  next->rows.reserve(gltfModel_.cameras.size());
  for(size_t row=0;row<gltfModel_.cameras.size();++row) {
    auto identity=std::make_shared<CameraProjectionIdentity>();identity->incarnation=cameraIdentity();
    identity->authored=identity->evaluated=int32_t(row);next->rows.push_back(std::move(identity));
  }
  next->bytes=sizeof(CameraProjectionNamespace)+128+next->rows.size()*(sizeof(std::shared_ptr<const CameraProjectionIdentity>)+sizeof(CameraProjectionIdentity)+64);
  if(!cameraAssetIdentity_) cameraAssetIdentity_=cameraIdentity();
  cameraProjectionNamespace_=std::move(next);++cameraProjectionRevision_;
}
void SceneManager::inheritCameraProjectionNamespace(const SceneManager& before) {
  if(!before.cameraProjectionNamespace_ || gltfModel_.cameras.size()!=before.cameraProjectionNamespace_->rows.size() ||
      exportSource_.cameras.size()!=before.cameraProjectionNamespace_->rows.size())
    cameraFailure("A shared clone changed the camera definition namespace");
  if(!cameraAssetIdentity_) cameraAssetIdentity_=cameraIdentity();
  cameraProjectionNamespace_=before.cameraProjectionNamespace_;
}
uint64_t SceneManager::cameraProjectionNamespaceBytes() const noexcept {
  return cameraProjectionNamespace_?cameraProjectionNamespace_->bytes:0;
}
const tinygltf::Camera& SceneManager::authoredCamera(int32_t camera) const {
  if(camera<0 || size_t(camera)>=exportSource_.cameras.size()) cameraFailure("Authored camera identity is unavailable");
  return exportSource_.cameras[size_t(camera)];
}
std::optional<CameraProjectionTarget> SceneManager::cameraProjectionTarget(const SceneGraph& graph,uint32_t graphNode) const noexcept {
  if(!cameraProjectionNamespace_ || cameraProjectionNamespace_->rows.size()!=gltfModel_.cameras.size() ||
      cameraProjectionNamespace_->rows.size()!=exportSource_.cameras.size()) return {};
  const auto* node=graph.getNode(graphNode);
  if(!node || node->sourceNodeIndex<0 || size_t(node->sourceNodeIndex)>=gltfModel_.nodes.size() ||
      size_t(node->sourceNodeIndex)>=exportSource_.nodes.size()) return {};
  const int a=exportSource_.nodes[size_t(node->sourceNodeIndex)].camera,e=gltfModel_.nodes[size_t(node->sourceNodeIndex)].camera;
  if(a<0 || e<0 || size_t(e)>=cameraProjectionNamespace_->rows.size()) return {};
  const auto& identity=cameraProjectionNamespace_->rows[size_t(e)];
  if(!identity || identity->authored!=a || identity->evaluated!=e ||
      !activeCameraNode(graph,graphNode,maxCameraRows,maxCameraGraphWork)) return {};
  return CameraProjectionTarget{cameraAssetIdentity_,cameraProjectionNamespace_->identity,cameraProjectionRevision_,
      graph.identity(),graph.revision(),identity->incarnation,graphNode,node->sourceNodeIndex,a,e};
}

SceneManager::CameraProjectionCommand::operator bool() const noexcept {return bool(state_);}
bool SceneManager::CameraProjectionCommand::published() const noexcept {return state_ && state_->published;}
bool SceneManager::CameraProjectionCommand::applied() const noexcept {return state_ && state_->applied;}
uint64_t SceneManager::CameraProjectionCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
CameraProjectionTarget SceneManager::CameraProjectionCommand::target() const noexcept {return state_?state_->target:CameraProjectionTarget{};}
SceneManager::PreparedCameraProjectionEdit::PreparedCameraProjectionEdit() noexcept=default;
SceneManager::PreparedCameraProjectionEdit::~PreparedCameraProjectionEdit()=default;
SceneManager::PreparedCameraProjectionEdit::PreparedCameraProjectionEdit(PreparedCameraProjectionEdit&&) noexcept=default;
SceneManager::PreparedCameraProjectionEdit& SceneManager::PreparedCameraProjectionEdit::operator=(PreparedCameraProjectionEdit&&) noexcept=default;
SceneManager::PreparedCameraProjectionEdit::operator bool() const noexcept {return state_ && !state_->consumed;}
bool SceneManager::PreparedCameraProjectionEdit::changed() const noexcept {return state_ && state_->plan.changed;}
uint64_t SceneManager::PreparedCameraProjectionEdit::ownedBytes() const noexcept {return state_?state_->bytes:0;}
SceneManager::CameraProjectionCommand SceneManager::PreparedCameraProjectionEdit::command() const noexcept {return state_?state_->command:CameraProjectionCommand{};}
const AnimatedCameraState& SceneManager::PreparedCameraProjectionEdit::candidateView() const {
  if(!state_) cameraFailure("Camera projection preparation is empty");return state_->view;
}

SceneManager::PreparedCameraProjectionEdit SceneManager::prepareCameraProjectionEdit(const CameraProjectionTarget& target,
    const GltfCameraProjectionPatch& patch,const SceneGraph& graph,const AnimationPlayer* player,float aspect,
    const CameraProjectionLimits& limits) const {
  validateCameraLimits(limits);const auto current=cameraProjectionTarget(graph,target.graphNode);
  if(!current || *current!=target) cameraFailure("The selected camera changed before its projection edit");
  auto authoring=limits.authoring;authoring.maxOwnedBytes=std::min(authoring.maxOwnedBytes,limits.maxPreparedCpuBytes/3);
  auto plan=PlanGltfCameraProjectionEdit(authoredCameraSourceView(),GltfCameraSourceView(gltfModel_),
      target.authoredCamera,target.evaluatedCamera,patch,authoring);
  return prepareCameraProjectionImpl(target,std::move(plan),nullptr,GltfCameraEditSide::After,graph,player,aspect,limits);
}
SceneManager::PreparedCameraProjectionEdit SceneManager::prepareCameraProjectionReplay(const CameraProjectionCommand& command,
    GltfCameraEditSide side,const SceneGraph& graph,const AnimationPlayer* player,float aspect,const CameraProjectionLimits& limits) const {
  validateCameraLimits(limits);
  if(!command || !command.state_->published || (side!=GltfCameraEditSide::Before && side!=GltfCameraEditSide::After))
    cameraFailure("Camera projection replay requires its original published command");
  const auto& c=*command.state_;const auto current=cameraProjectionTarget(graph,c.target.graphNode);
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || !current ||
      current->asset!=c.target.asset || current->sourceNamespace!=c.target.sourceNamespace ||
      current->graphIdentity!=c.target.graphIdentity || current->sourceNode!=c.target.sourceNode ||
      current->authoredCamera!=c.target.authoredCamera || current->evaluatedCamera!=c.target.evaluatedCamera ||
      current->incarnation!=c.target.incarnation || c.applied!=(side==GltfCameraEditSide::Before))
    cameraFailure("Camera projection history belongs to a stale, foreign or wrong-side definition");
  if(command.ownedBytes()>=limits.maxPreparedCpuBytes) cameraFailure("Camera projection history exceeds its coexistence budget");
  auto authoring=limits.authoring;authoring.maxOwnedBytes=std::min(authoring.maxOwnedBytes,(limits.maxPreparedCpuBytes-command.ownedBytes())/2);
  auto plan=RebaseGltfCameraProjectionEdit(c.original,side,authoredCameraSourceView(),GltfCameraSourceView(gltfModel_),authoring);
  return prepareCameraProjectionImpl(*current,std::move(plan),&command,side,graph,player,aspect,limits);
}
SceneManager::PreparedCameraProjectionEdit SceneManager::prepareCameraProjectionImpl(const CameraProjectionTarget& target,
    GltfCameraProjectionPlan&& plan,const CameraProjectionCommand* original,GltfCameraEditSide side,const SceneGraph& graph,
    const AnimationPlayer* player,float aspect,const CameraProjectionLimits& limits) const try {
  if(!player || !std::isfinite(aspect) || aspect<=0 || cameraProjectionRevision_==UINT64_MAX || cameraEvaluationRevision_==UINT64_MAX ||
      !activeCameraNode(graph,target.graphNode,limits.maxGraphNodes,limits.maxGraphWork) ||
      !canonicalCameraNode(graph,target.graphNode,limits.maxGraphWork))
    cameraFailure("Camera projection requires a current rest owner, active node and valid viewport");
  const auto current=cameraProjectionTarget(graph,target.graphNode);
  if(!current || *current!=target || !GltfCameraProjectionEditMatches(plan,authoredCameraSourceView(),GltfCameraSourceView(gltfModel_)))
    cameraFailure("Camera projection source or correspondence changed during preparation");
  CameraBudget budget{limits.maxPreparedCpuBytes};budget.add(sizeof(PreparedCameraProjectionEdit::State)+256);
  budget.add(plan.peakOwnedBytes);
  if(original) budget.add(original->ownedBytes());else {budget.add(sizeof(CameraProjectionCommand::State)+256);budget.add(plan.peakOwnedBytes);}
  PreparedCameraProjectionEdit result;result.state_=std::make_unique<PreparedCameraProjectionEdit::State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.target=target;s.limits=limits;s.player=player;s.aspect=aspect;
  s.identity=cameraProjectionNamespace_->rows[size_t(target.evaluatedCamera)];s.side=side;s.replay=original!=nullptr;
  s.plan=std::move(plan);const auto* node=graph.getNode(target.graphNode);s.world=node->worldTransform;s.local=node->localTransform;s.parent=node->parent;
  if(original) {s.command=*original;s.expectedApplied=s.command.state_->applied;}
  else {
    s.command.state_=std::make_shared<CameraProjectionCommand::State>();auto& c=*s.command.state_;
    c.owner=this;c.ownerIdentity=sceneRootOwnerIdentity_;c.identity=s.identity;c.target=target;c.original=s.plan;
    c.bytes=sizeof(CameraProjectionCommand::State)+256+c.original.peakOwnedBytes;
  }
  const auto view=EvaluateAnimatedCamera(s.plan.evaluatedAfter,s.world,aspect);
  if(!view) cameraFailure("The selected camera has no renderable candidate projection");s.view=*view;
  auto animation=limits.animation;animation.authoring=limits.authoring;animation.maxOwnedBytes=std::min(animation.maxOwnedBytes,budget.available);
  s.rest=player->prepareCameraRestEdit(s.plan,authoredCameraSourceView(),gltfModel_,animation);budget.add(s.rest.ownedBytes());
  s.bytes=limits.maxPreparedCpuBytes-budget.available;
  if(!canExchangeCameraProjectionEdit(result,graph,player,aspect)) cameraFailure("Camera source changed during detached projection preparation");
  return result;
} catch(const std::length_error& error) {throw std::invalid_argument(error.what());}

bool SceneManager::canExchangeCameraProjectionEdit(const PreparedCameraProjectionEdit& token,const SceneGraph& graph,
    const AnimationPlayer* player,float aspect) const noexcept {
  if(!token) return false;const auto& s=*token.state_;
  if(s.owner!=this || s.ownerIdentity!=sceneRootOwnerIdentity_ || s.player!=player || !player ||
      std::bit_cast<uint32_t>(s.aspect)!=std::bit_cast<uint32_t>(aspect) || cameraProjectionRevision_==UINT64_MAX || cameraEvaluationRevision_==UINT64_MAX ||
      !activeCameraNode(graph,s.target.graphNode,s.limits.maxGraphNodes,s.limits.maxGraphWork) ||
      !canonicalCameraNode(graph,s.target.graphNode,s.limits.maxGraphWork)) return false;
  const auto current=cameraProjectionTarget(graph,s.target.graphNode);
  if(!current || *current!=s.target || !cameraProjectionNamespace_ ||
      cameraProjectionNamespace_->rows[size_t(s.target.evaluatedCamera)]!=s.identity || !s.command ||
      s.command.state_->identity!=s.identity || (s.replay?
        (!s.command.state_->published || s.command.state_->applied!=s.expectedApplied):s.command.state_->published)) return false;
  const auto* node=graph.getNode(s.target.graphNode);
  return node && node->parent==s.parent && sameMatrix(node->worldTransform,s.world) && sameMatrix(node->localTransform,s.local) &&
      GltfCameraProjectionEditMatches(s.plan,authoredCameraSourceView(),GltfCameraSourceView(gltfModel_)) &&
      player->cameraRestEditMatches(s.rest,authoredCameraSourceView(),gltfModel_);
}
bool SceneManager::exchangeCameraProjectionEdit(PreparedCameraProjectionEdit& token,const SceneGraph& graph,
    AnimationPlayer* player,float aspect) noexcept {
  if(!canExchangeCameraProjectionEdit(token,graph,player,aspect)) return false;auto& s=*token.state_;
  if(s.plan.changed) {
    if(!player->exchangeCameraRestEdit(s.rest,authoredCameraSourceView(),gltfModel_)) return false;
    static_assert(std::is_nothrow_move_assignable_v<tinygltf::Camera>);
    const auto exchange=[](tinygltf::Camera& live,tinygltf::Camera& after,tinygltf::Camera& scratch) noexcept {
      scratch=std::move(live);live=std::move(after);after=std::move(scratch);
    };
    exchange(exportSource_.cameras[size_t(s.target.authoredCamera)],s.plan.authoredAfter,s.scratch[0]);
    exchange(gltfModel_.cameras[size_t(s.target.evaluatedCamera)],s.plan.evaluatedAfter,s.scratch[1]);
    ++cameraProjectionRevision_;++cameraEvaluationRevision_;
  }
  s.command.state_->published=true;s.command.state_->applied=s.side==GltfCameraEditSide::After;s.consumed=true;
  return true;
}
} // namespace container::scene
