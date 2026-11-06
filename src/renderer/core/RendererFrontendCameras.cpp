#include <Container/renderer/core/RendererFrontend.h>

#include <Container/geometry/GltfCameraAuthoring.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/utility/GltfAnimationControls.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {
using namespace container::geometry::gltf;
using Target=container::scene::CameraProjectionTarget;
constexpr uint32_t maxCameraGraphNodes=1'000'000;
std::array<double,8> cameraValues(const tinygltf::Camera& camera) noexcept {
  return {camera.perspective.yfov,camera.perspective.aspectRatio,camera.perspective.znear,camera.perspective.zfar,
      camera.orthographic.xmag,camera.orthographic.ymag,camera.orthographic.znear,camera.orthographic.zfar};
}
bool sameDefinition(const Target& a,const Target& b) noexcept {
  auto x=a,y=b;x.graphRevision=y.graphRevision=0;return x==y;
}
nlohmann::json cameraRecord(const tinygltf::Camera& camera) {
  return {{"name",camera.name},{"type",camera.type},{"values",cameraValues(camera)}};
}
float cameraAspect(const container::gpu::SwapChainManager& swapchain) {
  const auto size=swapchain.extent();
  if(!size.width || !size.height) throw std::invalid_argument("Camera editing requires a nonzero viewport");
  return float(size.width)/float(size.height);
}
} // namespace

struct RendererFrontend::CameraProjectionHistoryBudget {
  static constexpr uint64_t limit=64ull*1024*1024;
  uint64_t reserved{};
};
struct RendererFrontend::CameraProjectionHistory {
  container::scene::SceneManager::CameraProjectionCommand command;
  uint64_t graph{},reservation{};
  std::shared_ptr<CameraProjectionHistoryBudget> budget;
  ~CameraProjectionHistory() {if(budget) budget->reserved-=reservation;}
};
struct RendererFrontend::CameraProjectionInspectorCache {
  container::ui::CameraProjectionSnapshot snapshot;
  size_t authoredNodes{},evaluatedNodes{},authoredCameras{},evaluatedCameras{};
  const tinygltf::Animation* authoredAnimations{};
  const tinygltf::Animation* evaluatedAnimations{};
  uint64_t animationShape{};
};

uint32_t RendererFrontend::cameraGraphNode(size_t sourceNode) const {
  if(!subs_.sceneManager || sceneGraph_.nodeCount()>maxCameraGraphNodes)
    throw std::invalid_argument("The authored camera graph is unavailable or exceeds its traversal limit");
  const auto& source=subs_.sceneManager->animationSource();
  if(sourceNode>=source.nodes.size() || source.nodes[sourceNode].camera<0 ||
      size_t(source.nodes[sourceNode].camera)>=source.cameras.size())
    throw std::invalid_argument("The selected source node has no authored camera");
  uint32_t selected=container::scene::SceneGraph::kInvalidNode;
  for(uint32_t id=0;id<sceneGraph_.nodeCount();++id) {
    const auto* node=sceneGraph_.getNode(id);
    if(!node || !sceneGraph_.isNodeActive(id) || node->sourceNodeIndex<0 || size_t(node->sourceNodeIndex)!=sourceNode) continue;
    if(selected!=container::scene::SceneGraph::kInvalidNode)
      throw std::invalid_argument("The authored camera source node has ambiguous active graph bindings");
    selected=id;
  }
  if(selected==container::scene::SceneGraph::kInvalidNode) throw std::invalid_argument("The authored camera node is outside the active scene");
  return selected;
}
size_t RendererFrontend::cameraNodeByName(std::string_view name) const {
  if(!subs_.sceneManager || name.empty() || name.size()>4096 || sceneGraph_.nodeCount()>maxCameraGraphNodes)
    throw std::invalid_argument("The authored camera name or graph is invalid");
  const auto& source=subs_.sceneManager->animationSource();std::optional<size_t> selected;
  for(uint32_t id=0;id<sceneGraph_.nodeCount();++id) {
    const auto* node=sceneGraph_.getNode(id);
    if(!node || !sceneGraph_.isNodeActive(id) || node->sourceNodeIndex<0 || size_t(node->sourceNodeIndex)>=source.nodes.size()) continue;
    const auto index=size_t(node->sourceNodeIndex);const auto& row=source.nodes[index];
    if(row.name!=name || row.camera<0 || size_t(row.camera)>=source.cameras.size()) continue;
    if(selected && *selected!=index) throw std::invalid_argument("The authored camera node name is ambiguous; use its source index");
    selected=index;
  }
  if(!selected) throw std::invalid_argument("No active authored camera node has that exact name");
  (void)cameraGraphNode(*selected);return *selected;
}
bool RendererFrontend::setGltfCameraNode(std::optional<size_t> sourceNode) {
  // Capture actions may run before the first draw after loading a scene. Bind
  // its current rest owner now so the lazy animation update cannot erase an
  // accepted frame-one selection afterwards.
  if(!gltfAnimation_ || gltfAnimationGraphIdentity_!=sceneGraph_.identity()) resetGltfAnimation();
  if(sourceNode) {
    (void)cameraGraphNode(*sourceNode);
    if(!EvaluateAnimatedCamera(subs_.sceneManager->animationSource(),sceneGraph_,*sourceNode,cameraAspect(svc_.swapChainManager)))
      throw std::invalid_argument("The selected authored camera cannot supply a valid view");
  }
  if(sourceNode==gltfAnimationCameraNode_) {
    updateCameraViewPoseDestination();
    if(subs_.guiManager) subs_.guiManager->gltfAnimationControls().cameraProjectionError.clear();
    return true;
  }
  std::string reason="authored camera selection";
  gltfAnimationCameraNode_=sourceNode;gltfAnimationCamera_.reset();cameraProjectionInspectorCache_.reset();
  updateCameraViewPoseDestination();
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(reason));
  if(subs_.guiManager) {
    auto& controls=subs_.guiManager->gltfAnimationControls();controls.cameraNode=sourceNode?int(*sourceNode):-1;
    controls.cameraProjectionError.clear();
  }
  return true;
}

bool RendererFrontend::authorCameraProjectionSource(int32_t sourceNode,const GltfCameraProjectionPatch& patch,
    std::optional<uint64_t> expectedRevision) {
  if(sourceNode<0 || !subs_.sceneManager) throw std::invalid_argument("Camera editing requires an active authored camera node");
  if(!gltfAnimation_ || gltfAnimationGraphIdentity_!=sceneGraph_.identity()) resetGltfAnimation();
  auto& manager=*subs_.sceneManager;
  if(expectedRevision && *expectedRevision!=manager.cameraRevision()) throw std::invalid_argument("The authored camera revision is stale");
  const auto target=manager.cameraProjectionTarget(sceneGraph_,cameraGraphNode(size_t(sourceNode)));
  if(!target) throw std::invalid_argument("The source camera has no current authored/evaluated correspondence");
  return authorCameraProjection(*target,patch);
}
bool RendererFrontend::authorCameraProjection(const Target& target,const GltfCameraProjectionPatch& patch) {
  if(subs_.sceneManager && (!gltfAnimation_ || gltfAnimationGraphIdentity_!=sceneGraph_.identity())) resetGltfAnimation();
  if(!subs_.sceneManager || !gltfAnimation_) throw std::invalid_argument("Camera editing requires the current authored scene and animation rest owner");
  if(lightEditGestureActive()) throw std::invalid_argument("Finish the light gesture before applying a camera edit");
  if(materialGesture_) throw std::invalid_argument("Finish the material gesture before applying a camera edit");
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  if(cameraProjectionPublications_==UINT64_MAX) throw std::overflow_error("Camera publication counter is exhausted");
  ensureSceneEditHistory();auto& manager=*subs_.sceneManager;
  const auto aspect=cameraAspect(svc_.swapChainManager);
  auto prepared=manager.prepareCameraProjectionEdit(target,patch,sceneGraph_,gltfAnimation_.get(),aspect);
  if(!prepared.changed()) return true;
  if(!cameraProjectionHistoryBudget_) cameraProjectionHistoryBudget_=std::make_shared<CameraProjectionHistoryBudget>();
  const auto budget=cameraProjectionHistoryBudget_;
  const auto command=prepared.command();
  if(!command || command.ownedBytes()>CameraProjectionHistoryBudget::limit ||
      budget->reserved>CameraProjectionHistoryBudget::limit-command.ownedBytes() ||
      1024+sizeof(CameraProjectionHistory)>CameraProjectionHistoryBudget::limit-command.ownedBytes()-budget->reserved)
    throw std::length_error("Camera edit history exceeds its selected-record storage budget");
  auto state=std::make_shared<CameraProjectionHistory>();state->command=command;state->graph=sceneGraph_.identity();
  state->reservation=command.ownedBytes()+1024+sizeof(CameraProjectionHistory);state->budget=budget;budget->reserved+=state->reservation;
  container::util::EditHistory::Command history{"Edit authored camera",
      [this,state] {return replayCameraProjection(*state,GltfCameraEditSide::Before);},
      [this,state] {return replayCameraProjection(*state,GltfCameraEditSide::After);},true};
  std::string reason="authored camera projection edit";
  if(!manager.canExchangeCameraProjectionEdit(prepared,sceneGraph_,gltfAnimation_.get(),aspect) ||
      !manager.exchangeCameraProjectionEdit(prepared,sceneGraph_,gltfAnimation_.get(),aspect)) return false;
  ++cameraProjectionPublications_;gltfAnimationCamera_.reset();cameraProjectionInspectorCache_.reset();
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(reason));
  sceneEdits_->finishGesture();sceneEdits_->recordAppliedPublished(std::move(history));return true;
}
bool RendererFrontend::replayCameraProjection(const CameraProjectionHistory& state,GltfCameraEditSide side) {
  if(!subs_.sceneManager || !gltfAnimation_ || state.graph!=sceneGraph_.identity() || cameraProjectionPublications_==UINT64_MAX) return false;
  auto& manager=*subs_.sceneManager;const auto aspect=cameraAspect(svc_.swapChainManager);
  auto prepared=manager.prepareCameraProjectionReplay(state.command,side,sceneGraph_,gltfAnimation_.get(),aspect);
  std::string reason="authored camera projection history";
  if(!manager.canExchangeCameraProjectionEdit(prepared,sceneGraph_,gltfAnimation_.get(),aspect) ||
      !manager.exchangeCameraProjectionEdit(prepared,sceneGraph_,gltfAnimation_.get(),aspect)) return false;
  ++cameraProjectionPublications_;gltfAnimationCamera_.reset();cameraProjectionInspectorCache_.reset();
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(reason));return true;
}

void RendererFrontend::refreshCameraProjectionInspector() {
  if(!subs_.guiManager) return;
  auto& controls=subs_.guiManager->gltfAnimationControls();
  if(!subs_.sceneManager || !gltfAnimationCameraNode_) {
    // An empty refresh is not another camera selection. Keep a rejection
    // visible until an accepted user action or scene reset clears it.
    auto error=std::move(controls.cameraProjectionError);
    (void)container::ui::SetCameraProjectionSnapshot(controls,std::nullopt);
    controls.cameraProjectionError.swap(error);cameraProjectionInspectorCache_.reset();return;
  }
  try {
    auto& manager=*subs_.sceneManager;
    const auto authored=manager.authoredCameraSourceView(),evaluated=GltfCameraSourceView(manager.animationSource());
    uint32_t graphNode=container::scene::SceneGraph::kInvalidNode;
    std::optional<Target> target;
    if(cameraProjectionInspectorCache_) {
      const auto& old=cameraProjectionInspectorCache_->snapshot.target;
      const auto* node=sceneGraph_.getNode(old.graphNode);
      if(old.graphIdentity==sceneGraph_.identity() && node && node->sourceNodeIndex>=0 &&
          size_t(node->sourceNodeIndex)==*gltfAnimationCameraNode_ && sceneGraph_.isNodeActive(old.graphNode)) {
        graphNode=old.graphNode;
        const auto sourceNode=size_t(node->sourceNodeIndex);
        if(old.asset==manager.cameraAssetIdentity() && old.revision==manager.cameraRevision() &&
            sourceNode<authored.nodes.size() && sourceNode<evaluated.nodes.size() &&
            authored.nodes[sourceNode].camera==old.authoredCamera && evaluated.nodes[sourceNode].camera==old.evaluatedCamera) {
          // Retarget only weak preview intent after unrelated graph motion.
          // Apply always reconstructs the complete current backend proof.
          target=old;target->graphRevision=sceneGraph_.revision();
        }
      }
    }
    if(graphNode==container::scene::SceneGraph::kInvalidNode) graphNode=cameraGraphNode(*gltfAnimationCameraNode_);
    if(!target) target=manager.cameraProjectionTarget(sceneGraph_,graphNode);
    if(!target) throw std::invalid_argument("The authored camera definition is unavailable");
    // The preview is only a bounded availability hint. Current source, rest and
    // graph proofs are always prepared again when Apply is consumed.
    uint64_t shape=1469598103934665603ull;
    for(const auto source:{authored,evaluated}) {
      if(source.animations.size()>65536) throw std::length_error("Camera preview exceeds its clip traversal budget");
      for(const auto& clip:source.animations) {shape^=clip.channels.size();shape*=1099511628211ull;}
    }
    auto cache=cameraProjectionInspectorCache_;
    if(!cache || !sameDefinition(cache->snapshot.target,*target) || cache->authoredNodes!=authored.nodes.size() ||
        cache->evaluatedNodes!=evaluated.nodes.size() || cache->authoredCameras!=authored.cameras.size() ||
        cache->evaluatedCameras!=evaluated.cameras.size() || cache->authoredAnimations!=authored.animations.data() ||
        cache->evaluatedAnimations!=evaluated.animations.data() || cache->animationShape!=shape) {
      const auto plan=PlanGltfCameraProjectionEdit(authored,evaluated,target->authoredCamera,target->evaluatedCamera,{});
      auto next=std::make_shared<CameraProjectionInspectorCache>();next->snapshot.target=*target;
      next->snapshot.sharedDefinitionUses=uint32_t(plan.authoredNodes.size());
      for(const auto* bindings:{&plan.authoredAnimationBindings,&plan.evaluatedAnimationBindings}) for(const auto& binding:*bindings)
        next->snapshot.propertyBlockReasons[size_t(binding.property)]="This lens field is animated; edit its animation instead.";
      if(!plan.authoredAnimationBindings.empty() || !plan.evaluatedAnimationBindings.empty())
        next->snapshot.typeSwitchBlockReason="The camera has animated lens fields.";
      next->authoredNodes=authored.nodes.size();next->evaluatedNodes=evaluated.nodes.size();
      next->authoredCameras=authored.cameras.size();next->evaluatedCameras=evaluated.cameras.size();
      next->authoredAnimations=authored.animations.data();next->evaluatedAnimations=evaluated.animations.data();next->animationShape=shape;
      cameraProjectionInspectorCache_=next;cache=std::move(next);
    }
    auto snapshot=cache->snapshot;snapshot.target=*target;
    const auto& record=manager.authoredCamera(target->authoredCamera);snapshot.values=cameraValues(record);
    snapshot.type=record.type=="orthographic"?GltfCameraProjection::Orthographic:GltfCameraProjection::Perspective;
    if(!container::ui::SetCameraProjectionSnapshot(controls,snapshot)) throw std::invalid_argument("The camera inspector snapshot is invalid");
  } catch(const std::exception& error) {
    (void)container::ui::SetCameraProjectionSnapshot(controls,std::nullopt);cameraProjectionInspectorCache_.reset();
    controls.cameraProjectionError=error.what();
  }
}
void RendererFrontend::processCameraProjectionRequests() {
  if(!subs_.guiManager) return;auto& controls=subs_.guiManager->gltfAnimationControls();
  auto request=std::exchange(controls.requestedCameraProjection,{});if(!request) return;
  try {
    if(controls.requestedCameraNode || controls.requestedClip || controls.requestedTime || controls.requestedSpeed ||
        controls.requestedLoop || controls.requestedPlaying || !gltfAnimationCameraNode_ ||
        *gltfAnimationCameraNode_!=size_t(request->target.sourceNode))
      throw std::invalid_argument("Apply the camera selection or playback action before editing its lens");
    if(!authorCameraProjection(request->target,request->patch)) throw std::runtime_error("The camera edit could not be published");
    controls.cameraProjectionError.clear();refreshCameraProjectionInspector();
    if(controls.cameraProjection) {
      container::ui::CameraProjectionDraft draft;draft.target=controls.cameraProjection->target;
      draft.type=controls.cameraProjection->type;draft.values=controls.cameraProjection->values;
      (void)container::ui::SetCameraProjectionDraft(controls,draft);
    }
  } catch(const std::exception& error) {controls.cameraProjectionError=error.what();}
}

void RendererFrontend::appendCameraProjectionTelemetry(nlohmann::json& json) const {
  auto& value=json["gltfCameraAuthoring"];
  value={{"publications",cameraProjectionPublications_},{"asset",0},{"revision",0},{"cameras",nlohmann::json::array()},
      {"authored",nlohmann::json::array()},{"evaluated",nlohmann::json::array()},{"rests",nlohmann::json::array()},
      {"truncated",false},{"selectedSourceNode",gltfAnimationCameraNode_?int64_t(*gltfAnimationCameraNode_):-1}};
  if(!subs_.sceneManager) return;
  const auto& manager=*subs_.sceneManager;value["asset"]=manager.cameraAssetIdentity();value["revision"]=manager.cameraRevision();
  const auto authored=manager.authoredCameraSourceView(),evaluated=GltfCameraSourceView(manager.animationSource());
  value["authoredCameraCount"]=authored.cameras.size();value["evaluatedCameraCount"]=evaluated.cameras.size();
  value["restCameraCount"]=gltfAnimation_?gltfAnimation_->library().restCameras.size():0;
  if(authored.cameras.size()>256 || evaluated.cameras.size()>256 ||
      (gltfAnimation_ && gltfAnimation_->library().restCameras.size()>256) || sceneGraph_.nodeCount()>maxCameraGraphNodes) {value["truncated"]=true;return;}
  for(size_t i=0;i<authored.cameras.size();++i) value["authored"].push_back(cameraRecord(authored.cameras[i]));
  for(size_t i=0;i<evaluated.cameras.size();++i) value["evaluated"].push_back(cameraRecord(evaluated.cameras[i]));
  if(gltfAnimation_) for(const auto& camera:gltfAnimation_->library().restCameras) value["rests"].push_back(cameraRecord(camera));
  uint64_t targetWork=32'000'000;
  const uint64_t graphCount=sceneGraph_.nodeCount();
  const uint64_t perTargetWork=std::max<uint64_t>(1,std::min<uint64_t>(16'000'000,graphCount*graphCount+graphCount));
  for(uint32_t id=0;!evaluated.cameras.empty() && id<sceneGraph_.nodeCount();++id) {
    const auto* node=sceneGraph_.getNode(id);
    if(!node || node->sourceNodeIndex<0 || size_t(node->sourceNodeIndex)>=evaluated.nodes.size()) continue;
    const auto& sourceNode=evaluated.nodes[size_t(node->sourceNodeIndex)];
    if(sourceNode.camera<0 || size_t(sourceNode.camera)>=evaluated.cameras.size()) continue;
    if(!sceneGraph_.isNodeActive(id)) continue;
    if(value["cameras"].size()>=1024) {value["truncated"]=true;break;}
    if(perTargetWork>targetWork) {value["truncated"]=true;break;}targetWork-=perTargetWork;
    const auto target=manager.cameraProjectionTarget(sceneGraph_,id);if(!target) continue;
    value["cameras"].push_back({{"graphNode",id},{"sourceNode",target->sourceNode},{"authoredCamera",target->authoredCamera},
        {"evaluatedCamera",target->evaluatedCamera},{"sourceNamespace",target->sourceNamespace},{"incarnation",target->incarnation},
        {"graphIdentity",target->graphIdentity},{"graphRevision",target->graphRevision}});
  }
  if(gltfAnimationCamera_) {
    const auto matrix=[](const glm::mat4& m) {auto result=nlohmann::json::array();
      for(int c=0;c<4;++c) for(int r=0;r<4;++r) result.push_back(m[c][r]);return result;};
    value["view"]=matrix(gltfAnimationCamera_->view);value["projection"]=matrix(gltfAnimationCamera_->projection);
    value["shadowProjection"]=matrix(gltfAnimationCamera_->shadowProjection);
  }
}
} // namespace container::renderer
