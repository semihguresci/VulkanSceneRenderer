#include <Container/renderer/core/RendererFrontend.h>
#include "RendererLightCachePublication.h"
#include "RendererScenePublicationProof.h"

#include <Container/renderer/effects/OpenVdbVolumeRenderer.h>
#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/platform/VulkanContextInitializer.h>
#include <Container/renderer/resources/FrameResourceManager.h>
#include <Container/renderer/resources/FrameResourceRegistry.h>
#include <Container/renderer/scene/CameraController.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/renderer/temporal/TemporalCapture.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>
#include <Container/utility/TransformScale.h>

#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {
std::string lightMessage(std::string_view text) {
  constexpr size_t limit=500;
  if(text.size()<=limit) return std::string(text);
  size_t end=limit;while(end && (static_cast<unsigned char>(text[end])&0xc0)==0x80) --end;
  return std::string(text.substr(0,end))+"...";
}
std::string lightLabel(uint32_t fields) {
  if(fields==LightEditFieldBit(LightEditField::Intensity)) return "Light intensity";
  if(fields==LightEditFieldBit(LightEditField::Color)) return "Light color";
  if(fields==LightEditFieldBit(LightEditField::Position)) return "Move light";
  if(fields==LightEditFieldBit(LightEditField::Range)) return "Light range";
  if(fields==LightEditFieldBit(LightEditField::SpotCone)) return "Spot light beam";
  if(fields==LightEditFieldBit(LightEditField::AreaSize)) return "Area light size";
  if(fields==LightEditFieldBit(LightEditField::AreaShape)) return "Area light shape";
  return "Edit light";
}
uint64_t lightRuntimeBytes(const FrameResourceRegistry& registry) {
  if(registry.size()>4096 || registry.bindingCount()>4096) throw std::length_error("Light frame registry exceeds its staging limit");
  uint64_t bytes=sizeof(registry)+registry.size()*sizeof(FrameResourceDesc)+registry.bindingCount()*sizeof(FrameResourceBinding);
  const auto name=[&](std::string_view value) {if(value.size()>256) throw std::length_error("Light frame resource name exceeds its limit");bytes+=value.size()+1;};
  for(const auto& row:registry.resourceDescriptors()) name(row.key.name);
  for(const auto& row:registry.resourceBindings()) name(row.key.name);
  return bytes;
}
bool lightRuntimeSame(const FrameResourceRegistry& a,const FrameResourceRegistry& b) noexcept {
  if(a.size()!=b.size() || a.bindingCount()!=b.bindingCount()) return false;
  for(size_t i=0;i<a.size();++i) {
    const auto& x=a.resourceDescriptors()[i];const auto& y=b.resourceDescriptors()[i];
    if(!(x.key==y.key) || x.kind!=y.kind || x.lifetime!=y.lifetime || x.image.format!=y.image.format ||
        x.image.extent.width!=y.image.extent.width || x.image.extent.height!=y.image.extent.height || x.image.extent.depth!=y.image.extent.depth ||
        x.image.usage!=y.image.usage || x.image.samples!=y.image.samples || x.buffer.size!=y.buffer.size ||
        x.buffer.usage!=y.buffer.usage || x.framebuffer.attachmentCount!=y.framebuffer.attachmentCount) return false;
  }
  for(size_t i=0;i<a.bindingCount();++i) {
    const auto& x=a.resourceBindings()[i];const auto& y=b.resourceBindings()[i];
    if(!(x.handle==y.handle) || !(x.key==y.key) || x.frameIndex!=y.frameIndex || x.kind!=y.kind ||
        x.image.image!=y.image.image || x.image.view!=y.image.view || x.image.format!=y.image.format ||
        x.image.extent.width!=y.image.extent.width || x.image.extent.height!=y.image.extent.height || x.image.extent.depth!=y.image.extent.depth ||
        x.image.usage!=y.image.usage || x.image.samples!=y.image.samples || x.buffer.buffer!=y.buffer.buffer ||
        x.buffer.size!=y.buffer.size || x.buffer.usage!=y.buffer.usage || x.framebuffer.framebuffer!=y.framebuffer.framebuffer ||
        x.framebuffer.renderPass!=y.framebuffer.renderPass || x.framebuffer.extent.width!=y.framebuffer.extent.width ||
        x.framebuffer.extent.height!=y.framebuffer.extent.height || x.framebuffer.attachmentCount!=y.framebuffer.attachmentCount ||
        x.descriptor.descriptorSet!=y.descriptor.descriptorSet || x.sampler.sampler!=y.sampler.sampler) return false;
  }
  return true;
}
} // namespace

struct RendererFrontend::LightEditHistoryBudget {
  static constexpr uint64_t limit=64ull*1024*1024;
  uint64_t reserved{};
};
struct RendererFrontend::LightEditHistory {
  LightEditCommand command;
  std::shared_ptr<LightEditHistoryBudget> budget;
  uint64_t reservation{};
  ~LightEditHistory() {if(budget) budget->reserved-=reservation;}
};
struct RendererFrontend::PendingLightGesture {
  uint64_t id{};
  LightEditTarget target;
  EditableLightEntity start;
  bool gizmo{};
  bool cancelRequested{};
  std::shared_ptr<LightEditHistory> history;
  container::util::EditHistory::Command journal;
};
struct RendererFrontend::LightEditPublication {LightingManager::PreparedLightEdit prepared;};
struct RendererFrontend::PendingCaptureLight {
  std::optional<container::temporal::CaptureLightEdit> edit;
  std::optional<container::temporal::CaptureLightTarget> selection;
  std::optional<container::temporal::CaptureLightAdd> add;
  bool expectFailure{};
};

bool RendererFrontend::lightEditGestureActive() const noexcept {return bool(lightEditGesture_);}
void RendererFrontend::finishLightEditInput() {
  const bool queued=subs_.guiManager && (subs_.guiManager->lightEditGestureActive() || subs_.guiManager->hasPendingLightEditRequests());
  if(queued) {
    subs_.guiManager->finishLightEditGesture();processLightEditRequests();
    if(!lightEditError_.empty()) throw std::runtime_error(lightEditError_);
  }
  finishLightEditGesture();
}
void RendererFrontend::finishEditGestures() {
  finishLightEditInput();finishMaterialGesture();
}
void RendererFrontend::reportLightEditError(std::string_view message) {lightEditError_=lightMessage(message);}
void RendererFrontend::selectLightForEditing(EditableLightId id) {
  if(!subs_.lightingManager) return;
  finishEditGestures();
  subs_.lightingManager->selectEditableLight(id);transformDragSession_.active=false;
  sceneState_.selectedMeshNode=container::scene::SceneGraph::kInvalidNode;selectedBimObjectIndex_=UINT32_MAX;
  selectionNavigationAnchor_={};clearHoveredMeshNode();selectedDrawCommands_.clear();selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();selectedBimNativeCurveDrawCommands_.clear();
  if(subs_.guiManager) {subs_.guiManager->setSectionPlaneVisualEditable(false);subs_.guiManager->setStatusMessage("Selected editable light");}
  refreshLightEditInspector();
}

void RendererFrontend::refreshLightEditInspector() {
  if(!subs_.guiManager) return;
  LightEditSnapshot snapshot;
  try {
    if(subs_.lightingManager) snapshot=subs_.lightingManager->lightEditSnapshot(subs_.lightingManager->selectedEditableLightId());
    else snapshot.reason="Lighting is not ready";
    snapshot.light.label=lightMessage(snapshot.light.label);snapshot.reason=lightMessage(snapshot.reason);
    snapshot.error=lightMessage(lightEditError_);
    subs_.guiManager->setLightEditSnapshot(std::move(snapshot));
  } catch(const std::exception& error) {
    snapshot={};snapshot.reason=lightMessage(error.what());snapshot.error=lightMessage(lightEditError_);
    subs_.guiManager->setLightEditSnapshot(std::move(snapshot));
  }
}

bool RendererFrontend::publishLightEdit(LightEditPublication& publication) {
  auto& edit=publication.prepared;
  if(!edit) return false;if(!edit.changed()) return true;
  if(!subs_.lightingManager || !subs_.sceneManager || !subs_.frameRuntimeResourceRegistry || !subs_.sceneProviderRegistry)
    throw std::runtime_error("Lighting edits require initialized frame resources");
  auto& manager=*subs_.lightingManager;auto& scene=*subs_.sceneManager;
  const uint32_t images=uint32_t(svc_.swapChainManager.imageCount());
  if(!images || images>64 || images!=edit.lightingBuffers().size() || lightEditPublications_==UINT64_MAX)
    throw std::length_error("Light frame count or publication generation is invalid");
  constexpr uint64_t maxCpuBytes=128ull*1024*1024;
  const auto runtimeBytes=lightRuntimeBytes(*subs_.frameRuntimeResourceRegistry);
  if(edit.ownedBytes()>maxCpuBytes || 2*runtimeBytes+1024*1024>maxCpuBytes-edit.ownedBytes())
    throw std::length_error("Lighting publication exceeds its CPU staging budget");
  uint64_t cpuBytes=edit.ownedBytes()+2*runtimeBytes+1024*1024;
  const auto charge=[&](uint64_t bytes) {
    if(bytes>maxCpuBytes-cpuBytes) throw std::length_error("Light consumers exceed the aggregate CPU staging budget");
    cpuBytes+=bytes;
  };
  const auto externalBytes=detail::ScenePublicationExternalProof::preparationBytes(
      subs_.bimManager.get(),*subs_.sceneProviderRegistry,scene.materialCount(),&scene);
  charge(externalBytes);
  if(subs_.ddgiManager) charge(subs_.ddgiManager->lightEditPreparationCpuBytes());
  const auto graph=sceneGraph_.identity(),revision=sceneGraph_.revision();
  const auto epoch=scene.generationEpoch(),geometry=scene.geometryRevision(),materials=scene.materialRevision();
  const auto selectedScene=scene.selectedScene();
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Cannot retire lighting readers");
  detail::ScenePublicationExternalProof external(subs_.bimManager.get(),*subs_.sceneProviderRegistry,
      scene.materialCount(),false,std::nullopt,&scene);
  if(external.ownedBytes()>externalBytes) throw std::length_error("The external light reader proof grew beyond admission");
  detail::LightCachePublication caches(gltfImportedLightEdits_,gltfSceneLightEdits_,
      detail::LightCachePublication::SourceView{scene.authoredPointLightBindings(),scene.authoredDirectionalLightBindings(),scene.authoredAreaLightBindings()},
      edit.beforeEdits(),edit.edits(),edit.target().id,selectedScene);
  FrameResourceRegistry before=*subs_.frameRuntimeResourceRegistry,runtime=before;
  const auto bind=[&](RenderTechniqueId technique,uint32_t image) {
    runtime.bindDescriptorSet(technique,"light-descriptor-set",image,{edit.lightDescriptorSet(image)});
    runtime.bindDescriptorSet(technique,"tiled-lighting-descriptor-set",image,{edit.tiledDescriptorSet()});
  };
  for(uint32_t image=0;image<images;++image) {
    bind(RenderTechniqueId::DeferredRaster,image);bind(RenderTechniqueId::ForwardRaster,image);
    if(subs_.activeTechnique && subs_.activeTechnique->id()!=RenderTechniqueId::DeferredRaster &&
        subs_.activeTechnique->id()!=RenderTechniqueId::ForwardRaster) bind(subs_.activeTechnique->id(),image);
  }
  std::optional<OpenVdbVolumeRenderer::PreparedSceneBindings> volume;
  if(subs_.openVdbVolumeRenderer) volume=subs_.openVdbVolumeRenderer->prepareSceneBindings(
      edit.lightingBuffers(),scene.sceneClipBuffer(),edit.pointLightBuffer(),edit.areaLightBuffer());
  std::optional<DdgiManager::PreparedSceneHistoryReset> ddgi;
  std::optional<DdgiManager::PreparedLightSnapshotCapacity> capacity;
  if(subs_.ddgiManager) {
    ddgi=subs_.ddgiManager->prepareSceneHistoryReset();
    capacity=subs_.ddgiManager->prepareLightSnapshotCapacity(edit.pointLightsSsbo().size(),edit.areaLightsSsbo().size());
  }
  std::string reason="light edit or history";
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Cannot retire staged lighting readers");
  if(sceneGraph_.identity()!=graph || sceneGraph_.revision()!=revision || scene.generationEpoch()!=epoch ||
      scene.geometryRevision()!=geometry || scene.materialRevision()!=materials || scene.selectedScene()!=selectedScene ||
      !external.matches(*subs_.sceneProviderRegistry) || !caches.matches() ||
      !lightRuntimeSame(before,*subs_.frameRuntimeResourceRegistry) || !manager.canExchangeLightEdit(edit) ||
      (volume && !subs_.openVdbVolumeRenderer->canPublishSceneBindings(*volume)) ||
      (ddgi && !subs_.ddgiManager->canPublishSceneHistoryReset(*ddgi)) ||
      (capacity && !subs_.ddgiManager->canPublishLightSnapshotCapacity(*capacity)))
    throw std::runtime_error("The light or scene changed while preparing this edit");
  if(!manager.exchangeLightEdit(edit)) return false;
  if(!caches.exchange()) std::terminate();
  if(capacity && !subs_.ddgiManager->publishLightSnapshotCapacity(*capacity)) std::terminate();
  if(ddgi && !subs_.ddgiManager->publishSceneHistoryReset(*ddgi)) std::terminate();
  if(volume && !subs_.openVdbVolumeRenderer->publishSceneBindings(*volume)) std::terminate();
  subs_.frameRuntimeResourceRegistry->swapState(runtime);
  ++lightEditPublications_;
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(reason));
  // Ray/DDGI update receives the current light set for each retired image
  // before recording that image. Geometry and AS generations stay unchanged.
  return true;
}

bool RendererFrontend::replayLightEdit(LightEditHistory& state,bool applied) {
  if(!subs_.lightingManager || lightEditGesture_ || lightEditHistoryReplays_==UINT64_MAX) return false;
  LightEditPublication publication;
  publication.prepared=subs_.lightingManager->prepareLightEditReplay(state.command,applied);
  if(!publishLightEdit(publication)) return false;
  ++lightEditHistoryReplays_;lightEditError_.clear();return true;
}

bool RendererFrontend::authorLightEditRequest(const LightEditRequest& request) {
  if(!subs_.lightingManager) throw std::runtime_error("Lighting is not ready");
  if(request.phase==LightEditPhase::End || request.phase==LightEditPhase::Cancel) {
    if(!request.gesture || (request.phase==LightEditPhase::Cancel && request.patch.fields))
      throw std::invalid_argument("The light gesture terminal intent is invalid");
    if(!lightEditGesture_) {
      if(request.gesture==lastFinishedLightGesture_) return true;
      throw std::invalid_argument("The light gesture is no longer active");
    }
    if(request.gesture!=lightEditGesture_->id || !SameLightEditIdentity(request.target,lightEditGesture_->target))
      throw std::invalid_argument("The light gesture target changed");
    if(request.patch.fields && request.phase==LightEditPhase::End) {
      auto update=request;update.phase=LightEditPhase::Update;
      if(!authorLightEditRequest(update)) return false;
    }
    finishLightEditGesture(request.phase==LightEditPhase::Cancel);return true;
  }
  if(request.phase!=LightEditPhase::Commit && request.phase!=LightEditPhase::Begin && request.phase!=LightEditPhase::Update)
    throw std::invalid_argument("Unknown light edit phase");
  if(!request.patch.fields || (request.patch.fields&~kAllLightEditFields) ||
      (request.phase==LightEditPhase::Commit ? request.gesture!=0 : request.gesture==0))
    throw std::invalid_argument("The light edit intent is invalid");
  if(lightEditGesture_ && lightEditGesture_->cancelRequested) finishLightEditGesture(true);
  finishMaterialGesture();
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  ensureSceneEditHistory();
  const auto snapshot=subs_.lightingManager->lightEditSnapshot(request.target.id);
  if(!snapshot.available || snapshot.target!=request.target) throw std::invalid_argument("The light changed; select it again");
  const bool standalone=request.phase==LightEditPhase::Commit;
  if((standalone || request.phase==LightEditPhase::Begin) && lightEditGesture_ &&
      (standalone || lightEditGesture_->id!=request.gesture)) finishLightEditGesture();
  if(request.phase==LightEditPhase::Update && (!lightEditGesture_ || lightEditGesture_->id!=request.gesture))
    throw std::invalid_argument("Begin the light gesture before updating it");
  const auto reserve=[&]() {
    if(lightEditHistoryCommits_==UINT64_MAX) throw std::overflow_error("Light history generation exhausted");
    if(!lightEditHistoryBudget_) lightEditHistoryBudget_=std::make_shared<LightEditHistoryBudget>();
    constexpr uint64_t amount=64*1024+sizeof(LightEditHistory)+1024;
    if(lightEditHistoryBudget_->reserved>LightEditHistoryBudget::limit-amount)
      throw std::length_error("Light edit history exceeds its selected-record budget");
    auto state=std::make_shared<LightEditHistory>();state->budget=lightEditHistoryBudget_;state->reservation=amount;
    state->budget->reserved+=amount;return state;
  };
  const bool createdGesture=!lightEditGesture_;
  if(createdGesture) {
    auto gesture=std::make_shared<PendingLightGesture>();gesture->id=request.gesture;gesture->target=request.target;
    gesture->start=snapshot.light;gesture->history=reserve();
    const auto state=gesture->history;
    gesture->journal={lightLabel(request.patch.fields),[this,state] {return replayLightEdit(*state,false);},
        [this,state] {return replayLightEdit(*state,true);},true};
    lightEditGesture_=std::move(gesture);
  }
  if(!lightEditGesture_->history) {
    auto state=reserve();
    container::util::EditHistory::Command journal{lightLabel(request.patch.fields),[this,state] {return replayLightEdit(*state,false);},
        [this,state] {return replayLightEdit(*state,true);},true};
    lightEditGesture_->history=std::move(state);lightEditGesture_->journal=std::move(journal);
  }
  if(!SameLightEditIdentity(request.target,lightEditGesture_->target)) throw std::invalid_argument("The selected light gesture changed");
  auto& state=*lightEditGesture_->history;
  try {
    LightEditPublication publication;
    publication.prepared=subs_.lightingManager->prepareLightEdit(request.target,request.patch,state.command?&state.command:nullptr);
    if(publication.prepared.changed()) {
      const auto command=publication.prepared.command();
      if(!command || command.ownedBytes()>64*1024) throw std::length_error("Light command exceeds its selected-record budget");
      if(!publishLightEdit(publication)) {if(createdGesture) lightEditGesture_.reset();return false;}
      state.command=command;
    }
  } catch(...) {
    if(createdGesture) lightEditGesture_.reset();
    throw;
  }
  lightEditGesture_->target.revision=subs_.lightingManager->lightEditRevision();
  lightEditError_.clear();
  if(standalone) finishLightEditGesture();
  return true;
}

void RendererFrontend::finishLightEditGesture(bool cancel) {
  if(!lightEditGesture_) return;
  const auto gesture=lightEditGesture_;
  // A failed cancellation remains a cancellation on every subsequent retry,
  // including selection/save/history barriers after the widget has deactivated.
  gesture->cancelRequested=gesture->cancelRequested || cancel;
  cancel=gesture->cancelRequested;
  if(gesture->history && gesture->history->command) {
    auto& state=*gesture->history;
    if(cancel) {
      LightEditPublication publication;
      publication.prepared=subs_.lightingManager->prepareLightEditReplay(state.command,false);
      if(!publishLightEdit(publication)) throw std::runtime_error("Cannot cancel the light edit in the current scene");
    } else {
      if(!subs_.lightingManager->lightEditCommandMatches(state.command))
        throw std::invalid_argument("The light changed before its gesture could finish");
      if(state.command.netChanged()) {
        if(lightEditHistoryCommits_==UINT64_MAX) throw std::overflow_error("Light history generation exhausted");
        sceneEdits_->finishGesture();sceneEdits_->recordAppliedPublished(std::move(gesture->journal));
        ++lightEditHistoryCommits_;
      }
    }
  }
  lastFinishedLightGesture_=gesture->id;
  if(gesture->gizmo) transformDragSession_.active=false;
  lightEditGesture_.reset();lightEditError_.clear();
}

void RendererFrontend::processLightEditRequests() {
  if(!subs_.guiManager || !subs_.lightingManager) return;
  try {
    auto requests=subs_.guiManager->consumeLightEditRequests();
    const auto selection=std::exchange(pendingLightSelection_,{});
    const auto add=subs_.guiManager->consumeLightAddRequest();
    const auto capturedRevision=subs_.lightingManager->lightEditRevision();auto ownedRevision=capturedRevision;
    for(auto request:requests) {
      // One widget batch may contain Update+End or End+Begin. Upgrade only
      // intents captured from this exact batch's original snapshot and only
      // across publications proved to belong to this drain.
      if(request.target.revision==capturedRevision && ownedRevision==subs_.lightingManager->lightEditRevision()) {
        const auto current=subs_.lightingManager->lightEditSnapshot(request.target.id);
        if(current.available && SameLightEditIdentity(current.target,request.target)) request.target=current.target;
      }
      if(!authorLightEditRequest(request)) throw std::runtime_error("The light edit was not published");
      ownedRevision=subs_.lightingManager->lightEditRevision();
    }
    if(selection) selectLightForEditing(*selection);
    if(add) {
      if(!authorManualLightAdd(*add)) throw std::runtime_error("The manual light could not be added");
    }
  } catch(const std::exception& error) {lightEditError_=lightMessage(error.what());}
  refreshLightEditInspector();
}

bool RendererFrontend::authorManualLightAdd(EditableLightType type) {
  finishLightEditGesture();finishMaterialGesture();
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  if(!subs_.lightingManager) throw std::runtime_error("Lighting is not ready");
  if(lightEditHistoryCommits_==UINT64_MAX) throw std::overflow_error("Light history generation exhausted");
  ensureSceneEditHistory();LightEditPublication publication;
  publication.prepared=subs_.lightingManager->prepareLightAdd(type);
  if(!publication.prepared.changed()) return true;
  if(!lightEditHistoryBudget_) lightEditHistoryBudget_=std::make_shared<LightEditHistoryBudget>();
  const auto command=publication.prepared.command();
  const auto amount=command.ownedBytes()+sizeof(LightEditHistory)+1024;
  if(!command || amount>LightEditHistoryBudget::limit || lightEditHistoryBudget_->reserved>LightEditHistoryBudget::limit-amount)
    throw std::length_error("Manual light history exceeds its budget");
  auto state=std::make_shared<LightEditHistory>();state->command=command;state->budget=lightEditHistoryBudget_;
  state->reservation=amount;state->budget->reserved+=amount;
  container::util::EditHistory::Command history{std::string("Add ")+editableLightTypeLabel(type)+" light",
      [this,state] {return replayLightEdit(*state,false);},[this,state] {return replayLightEdit(*state,true);},true};
  if(!publishLightEdit(publication)) return false;
  sceneEdits_->finishGesture();sceneEdits_->recordAppliedPublished(std::move(history));++lightEditHistoryCommits_;
  sceneState_.selectedMeshNode=container::scene::SceneGraph::kInvalidNode;selectedBimObjectIndex_=UINT32_MAX;
  selectionNavigationAnchor_={};clearHoveredMeshNode();selectedDrawCommands_.clear();selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();selectedBimNativeCurveDrawCommands_.clear();
  if(subs_.guiManager) subs_.guiManager->setSectionPlaneVisualEditable(false);
  lightEditError_.clear();return true;
}

void RendererFrontend::beginLightGizmoGesture(container::ui::ViewportTool,container::ui::TransformSpace,
    container::ui::TransformAxis,bool) {
  if(!subs_.lightingManager || sceneState_.selectedMeshNode!=container::scene::SceneGraph::kInvalidNode) return;
  if(!subs_.lightingManager->selectedEditableLight()) return;
  if(subs_.guiManager) {subs_.guiManager->finishLightEditGesture();processLightEditRequests();}
  finishLightEditGesture();finishMaterialGesture();
  if(nextLightGizmoGesture_==UINT64_MAX) throw std::overflow_error("Light gizmo identity exhausted");
  // The shared authoring route owns allocation/history reservation on the
  // first nonzero delta. Keep only the immutable starting visible row here.
  auto gesture=std::make_shared<PendingLightGesture>();gesture->id=nextLightGizmoGesture_++;
  const auto snapshot=subs_.lightingManager->lightEditSnapshot(subs_.lightingManager->selectedEditableLightId());
  if(!snapshot.available) throw std::invalid_argument("This light is no longer editable");
  gesture->target=snapshot.target;gesture->start=snapshot.light;gesture->gizmo=true;
  lightEditGesture_=std::move(gesture);
}
void RendererFrontend::endLightGizmoGesture() {
  if(!lightEditGesture_ || !lightEditGesture_->gizmo) return;
  finishLightEditGesture();transformDragSession_.active=false;
}
void RendererFrontend::cancelLightGizmoGesture(RenderSurfaceInteractionController::TransformGestureCancelReason) {
  if(!lightEditGesture_ || !lightEditGesture_->gizmo) return;
  finishLightEditGesture(true);transformDragSession_.active=false;
}

bool RendererFrontend::applyLightGizmoControls(container::ui::ViewportTool tool,container::ui::TransformAxis axis,
    const container::ui::TransformControls& controls) {
  if(!lightEditGesture_ || !lightEditGesture_->gizmo) return false;
  auto edited=lightEditGesture_->start;const auto& start=transformDragSession_.startControls;
  switch(tool) {
  case container::ui::ViewportTool::Translate:
    if(edited.type==EditableLightType::Directional) return false;
    edited.position+=controls.position-start.position;break;
  case container::ui::ViewportTool::Rotate: {
    if(edited.type==EditableLightType::Point) return false;
    const auto delta=controls.rotationDegrees-start.rotationDegrees;
    const auto rotate=[&](const glm::vec3& around,float degrees) {
      if(std::abs(degrees)<=1e-4f) return;
      const auto matrix=glm::mat3(glm::rotate(glm::mat4(1),glm::radians(degrees),glm::normalize(around)));
      edited.direction=matrix*edited.direction;
      if(edited.type==EditableLightType::Area) {edited.tangent=matrix*edited.tangent;edited.bitangent=matrix*edited.bitangent;}
    };
    switch(axis) {
    case container::ui::TransformAxis::X:rotate(transformDragSession_.axisX,delta.x);break;
    case container::ui::TransformAxis::Y:rotate(transformDragSession_.axisY,delta.y);break;
    case container::ui::TransformAxis::Z:rotate(transformDragSession_.axisZ,delta.z);break;
    case container::ui::TransformAxis::Free:rotate(transformDragSession_.axisY,delta.y);rotate(transformDragSession_.axisX,delta.x);break;
    }break;
  }
  case container::ui::ViewportTool::Scale: {
    if(edited.type==EditableLightType::Directional) return false;
    const auto average=[](glm::vec3 v) {return std::max((v.x+v.y+v.z)*.33333334f,.001f);};
    const float factor=std::clamp(average(controls.scale)/average(start.scale),.05f,20.f);
    edited.range=std::max(edited.range*factor,.001f);
    if(edited.type==EditableLightType::Area) edited.areaHalfSize=glm::max(edited.areaHalfSize*factor,glm::vec2(.001f));
    break;
  }
  case container::ui::ViewportTool::Select:return false;
  }
  auto patch=LightEditPatchFrom(lightEditGesture_->start,edited);
  if(!patch.fields && (!lightEditGesture_->history || !lightEditGesture_->history->command)) return false;
  // Send the original endpoint too when a drag returns to its start. Skipping
  // a zero start-difference would leave the previous nonzero preview live.
  if(tool==container::ui::ViewportTool::Translate) patch.fields=LightEditFieldBit(LightEditField::Position);
  else if(tool==container::ui::ViewportTool::Rotate) patch.fields=LightEditFieldBit(LightEditField::Direction)|
      (edited.type==EditableLightType::Area?LightEditFieldBit(LightEditField::AreaBasis):0);
  else if(tool==container::ui::ViewportTool::Scale) patch.fields=LightEditFieldBit(LightEditField::Range)|
      (edited.type==EditableLightType::Area?LightEditFieldBit(LightEditField::AreaSize):0);
  const auto snapshot=subs_.lightingManager->lightEditSnapshot(lightEditGesture_->target.id);
  auto request=LightEditRequest{snapshot.target,lightEditGesture_->history && lightEditGesture_->history->command?
      LightEditPhase::Update:LightEditPhase::Begin,lightEditGesture_->id,patch};
  return authorLightEditRequest(request);
}

void RendererFrontend::queueCaptureLightEvent(const container::temporal::CaptureEvent& event) {
  if(!event.lightEdit && !event.lightSelection && !event.lightAdd) return;
  if(pendingCaptureLight_) throw std::invalid_argument("The previous capture light action has not been evaluated");
  auto pending=std::make_shared<PendingCaptureLight>();
  pending->edit=event.lightEdit;pending->selection=event.lightSelection;pending->add=event.lightAdd;
  pending->expectFailure=event.expectLightEditFailure;pendingCaptureLight_=std::move(pending);
}

EditableLightId RendererFrontend::resolveCaptureLightTarget(const container::temporal::CaptureLightTarget& target) const {
  if(!subs_.lightingManager || !subs_.sceneManager) throw std::invalid_argument("No light scene is available");
  const auto& lights=subs_.lightingManager->editableLights();
  if(lights.size()>16384) throw std::length_error("Capture light selection exceeds its bounded search");
  const detail::LightCachePublication::SourceView authored{subs_.sceneManager->authoredPointLightBindings(),
      subs_.sceneManager->authoredDirectionalLightBindings(),subs_.sceneManager->authoredAreaLightBindings()};
  const auto& nodes=subs_.sceneManager->animationSource().nodes;
  std::optional<EditableLightId> found;
  for(const auto& light:lights) {
    if(light.type!=target.type || light.source!=target.source) continue;
    bool matches=false;
    if(target.source!=EditableLightSource::Imported) matches=target.index && light.id.index==*target.index;
    else {
      const container::scene::AuthoredLightBinding* binding=nullptr;
      if(light.type==EditableLightType::Area && light.id.index<authored.areaBindings.size()) binding=&authored.areaBindings[light.id.index];
      else if(light.type==EditableLightType::Directional && light.id.index<authored.directionalBindings.size()) binding=&authored.directionalBindings[light.id.index];
      else if((light.type==EditableLightType::Point || light.type==EditableLightType::Spot) && light.id.index<authored.pointBindings.size()) binding=&authored.pointBindings[light.id.index];
      if(binding) matches=target.sourceNode ? binding->sourceNode==*target.sourceNode :
          binding->sourceNode>=0 && size_t(binding->sourceNode)<nodes.size() && nodes[size_t(binding->sourceNode)].name==target.nodeName;
    }
    if(matches) {if(found) throw std::invalid_argument("The capture light target is ambiguous");found=light.id;}
  }
  if(!found) throw std::invalid_argument("The capture light target is not available");
  return *found;
}

void RendererFrontend::processCaptureLightEvent() {
  const auto pending=std::exchange(pendingCaptureLight_,{});
  if(!pending) return;
  bool accepted=false;
  try {
    if(!subs_.lightingManager) throw std::invalid_argument("Lighting is not ready");
    const auto requireRevision=[&](std::optional<uint64_t> expected) {
      if(expected && *expected!=subs_.lightingManager->lightEditRevision())
        throw std::invalid_argument("The capture light revision is stale");
    };
    if(pending->selection) {selectLightForEditing(resolveCaptureLightTarget(*pending->selection));accepted=true;}
    else if(pending->add) {
      requireRevision(pending->add->expectedLightRevision);finishLightEditInput();
      accepted=authorManualLightAdd(pending->add->type);
    } else if(pending->edit) {
      const auto& intent=*pending->edit;requireRevision(intent.expectedLightRevision);
      const auto id=resolveCaptureLightTarget(intent.target);
      const auto snapshot=subs_.lightingManager->lightEditSnapshot(id);
      if(!snapshot.available) throw std::invalid_argument("The capture light is no longer editable");
      const bool terminal=intent.phase==LightEditPhase::End || intent.phase==LightEditPhase::Cancel;
      if(terminal || !intent.gizmo) {
        if(intent.phase==LightEditPhase::Begin || intent.phase==LightEditPhase::Commit) selectLightForEditing(id);
        const auto current=subs_.lightingManager->lightEditSnapshot(id);
        accepted=authorLightEditRequest({current.target,intent.phase,intent.gesture,intent.values.value_or(LightEditPatch{})});
      } else {
        using CaptureGizmo=container::temporal::CaptureLightGizmo;
        const auto& gizmo=*intent.gizmo;
        const auto tool=gizmo.tool==CaptureGizmo::Tool::Translate?container::ui::ViewportTool::Translate:
            gizmo.tool==CaptureGizmo::Tool::Rotate?container::ui::ViewportTool::Rotate:container::ui::ViewportTool::Scale;
        const auto space=gizmo.space==CaptureGizmo::Space::Local?container::ui::TransformSpace::Local:container::ui::TransformSpace::World;
        const auto axis=gizmo.axis==CaptureGizmo::Axis::X?container::ui::TransformAxis::X:
            gizmo.axis==CaptureGizmo::Axis::Y?container::ui::TransformAxis::Y:
            gizmo.axis==CaptureGizmo::Axis::Z?container::ui::TransformAxis::Z:container::ui::TransformAxis::Free;
        if((tool==container::ui::ViewportTool::Rotate && snapshot.light.type==EditableLightType::Point) ||
            (tool!=container::ui::ViewportTool::Rotate && snapshot.light.type==EditableLightType::Directional))
          throw std::invalid_argument("This light does not support the requested gizmo tool");
        const bool starting=intent.phase==LightEditPhase::Begin || intent.phase==LightEditPhase::Commit;
        if(starting) {
          selectLightForEditing(id);
          interactionController_.setTool(tool);interactionController_.setTransformSpace(space);
          interactionController_.setTransformAxis(axis);interactionController_.setTransformSnapEnabled(gizmo.snap);
          beginLightGizmoGesture(tool,space,axis,gizmo.snap);
          if(!lightEditGesture_) throw std::invalid_argument("The light gizmo could not begin");
          if(intent.phase!=LightEditPhase::Commit) lightEditGesture_->id=intent.gesture;
          const auto visible=buildTransformGizmoState();
          transformDragSession_={.active=true,.nodeIndex=UINT32_MAX,.tool=tool,.space=space,.axis=axis,.snapEnabled=gizmo.snap,
              .startControls={.position=snapshot.light.position,.rotationDegrees=glm::vec3(0),.scale=glm::vec3(std::max(snapshot.light.range,1.f))},
              .origin=visible.origin,.gizmoScale=std::max(visible.scale,.0001f),.axisX=visible.axisX,.axisY=visible.axisY,.axisZ=visible.axisZ};
        } else if(!lightEditGesture_ || !lightEditGesture_->gizmo || lightEditGesture_->id!=intent.gesture ||
            !SameLightEditIdentity(lightEditGesture_->target,snapshot.target) || transformDragSession_.tool!=tool ||
            transformDragSession_.space!=space || transformDragSession_.axis!=axis || transformDragSession_.snapEnabled!=gizmo.snap)
          throw std::invalid_argument("The capture gizmo gesture or target changed");
        auto controls=transformDragSession_.startControls;
        glm::vec3 amount=gizmo.amount;
        if(axis!=container::ui::TransformAxis::Free) {
          const int component=axis==container::ui::TransformAxis::X?0:axis==container::ui::TransformAxis::Y?1:2;
          for(int i=0;i<3;++i) if(i!=component) amount[i]=0;
        }
        if(tool==container::ui::ViewportTool::Translate) {
          controls.position+=space==container::ui::TransformSpace::Local ?
              transformDragSession_.axisX*amount.x+transformDragSession_.axisY*amount.y+transformDragSession_.axisZ*amount.z:amount;
          if(gizmo.snap) for(int i=0;i<3;++i) controls.position[i]=transformDragSession_.startControls.position[i]+
              std::round((controls.position[i]-transformDragSession_.startControls.position[i])/.25f)*.25f;
        } else if(tool==container::ui::ViewportTool::Rotate) {
          if(gizmo.snap) for(int i=0;i<3;++i) amount[i]=std::round(amount[i]/15.f)*15.f;
          controls.rotationDegrees+=amount;
        } else {
          const auto factors=glm::clamp(glm::vec3(1)+amount,glm::vec3(.25f),glm::vec3(4));
          for(int i=0;i<3;++i) if(amount[i]!=0) controls.scale[i]=container::ui::clampSignedScale(controls.scale[i]*factors[i]);
          if(gizmo.snap) controls.scale=container::ui::snapSignedScale(transformDragSession_.startControls.scale,controls.scale,.1f);
        }
        (void)applyLightGizmoControls(tool,axis,controls);accepted=true;
        if(intent.phase==LightEditPhase::Commit) finishLightEditGesture();
      }
    }
    if(!accepted) throw std::runtime_error("The capture light action could not be published");
    lightEditError_.clear();
  } catch(const std::exception& error) {
    reportLightEditError(error.what());
    if(!pending->expectFailure) throw;
    return;
  }
  if(pending->expectFailure) throw std::runtime_error("The capture light action unexpectedly succeeded");
}

void RendererFrontend::appendLightEditTelemetry(nlohmann::json& json) const {
  auto& out=json["lightEdits"];
  out={{"publications",lightEditPublications_},{"commits",lightEditHistoryCommits_},{"replays",lightEditHistoryReplays_},
      {"gesture",lightEditGesture_?lightEditGesture_->id:0},{"error",lightEditError_}};
  if(!subs_.lightingManager) return;
  out["revision"]=subs_.lightingManager->lightEditRevision();out["lights"]=nlohmann::json::array();
  const auto& lights=subs_.lightingManager->editableLights();
  if(lights.size()>16384) {out["truncated"]=true;return;}
  for(const auto& light:lights) {
    const auto values=LightEditValuesFrom(light);
    out["lights"].push_back({{"type",uint32_t(light.type)},{"source",uint32_t(light.source)},{"index",light.id.index},
        {"position",{values.position.x,values.position.y,values.position.z}},
        {"direction",{values.direction.x,values.direction.y,values.direction.z}},
        {"color",{values.color.x,values.color.y,values.color.z}},{"intensity",values.intensity},{"range",values.range},
        {"cones",{values.innerConeDegrees,values.outerConeDegrees}},
        {"halfSize",{values.areaHalfSize.x,values.areaHalfSize.y}},{"shape",values.areaShape},
        {"tangent",{values.tangent.x,values.tangent.y,values.tangent.z}},
        {"bitangent",{values.bitangent.x,values.bitangent.y,values.bitangent.z}},
        {"selected",light.id==subs_.lightingManager->selectedEditableLightId()}});
    if(light.source==EditableLightSource::Imported && subs_.sceneManager) {
      const auto bindings=light.type==EditableLightType::Directional?subs_.sceneManager->authoredDirectionalLightBindings():
          light.type==EditableLightType::Area?subs_.sceneManager->authoredAreaLightBindings():subs_.sceneManager->authoredPointLightBindings();
      if(light.id.index<bindings.size()) {
        const auto& binding=bindings[light.id.index];auto& row=out["lights"].back();
        row["sourceNode"]=binding.sourceNode;
        row["extension"]=binding.extension.size()<=256?binding.extension:"[binding exceeds display limit]";
      }
    }
  }
  out["truncated"]=false;
}
} // namespace container::renderer
