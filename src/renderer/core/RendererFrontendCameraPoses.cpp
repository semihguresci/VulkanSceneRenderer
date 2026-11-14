#include <Container/renderer/core/RendererFrontend.h>
#include "RendererScenePublicationProof.h"
#include <Container/renderer/core/RenderTechnique.h>
#include <Container/renderer/culling/GpuCullManager.h>
#include <Container/renderer/effects/OpenVdbVolumeRenderer.h>
#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/platform/VulkanContextInitializer.h>
#include <Container/renderer/raytracing/RayShadowManager.h>
#include <Container/renderer/resources/FrameResourceManager.h>
#include <Container/renderer/resources/FrameResourceRegistry.h>
#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/renderer/scene/GltfDeformationScene.h>
#include <Container/renderer/scene/MeshSceneProviderBindings.h>
#include <Container/renderer/scene/PbrAuxiliaryPass.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/scene/SceneProviderSynchronizer.h>
#include <Container/renderer/shadow/ShadowCullManager.h>
#include <Container/renderer/shadow/ShadowManager.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/Platform.h>
#if CONTAINER_HAS_JOLT
#include <Container/physics/PhysicsScene.h>
#include <Container/physics/PhysicsElementBindings.h>
#endif

#include <Container/renderer/scene/CameraController.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/utility/GltfAnimationControls.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {
using namespace container::geometry::gltf;
using Target=container::scene::CameraViewPoseTarget;
using Manager=container::scene::SceneManager;
float poseAspect(const container::gpu::SwapChainManager& swapchain) {
  const auto extent=swapchain.extent();
  if(!extent.width || !extent.height) throw std::invalid_argument("Writing a camera pose requires a nonzero viewport");
  return float(extent.width)/float(extent.height);
}
bool samePoseMatrix(const glm::mat4& a,const glm::mat4& b) noexcept {
  return std::memcmp(&a,&b,sizeof(a))==0;
}
bool samePoseDestination(const Target& a,const Target& b) noexcept {
  // A preview refresh may advance graph/camera publication counters. It may
  // never substitute a new node lifetime, source namespace or definition.
  auto x=a,y=b;x.camera.graphRevision=y.camera.graphRevision=0;
  x.camera.revision=y.camera.revision=0;return x==y;
}
std::string poseDisplayName(std::string_view name) {
  size_t end=std::min<size_t>(name.size(),4000);
  while(end<name.size() && end && (uint8_t(name[end])&0xc0)==0x80) --end;
  std::string result(name.substr(0,end));if(end<name.size()) result+="...";return result;
}
nlohmann::json poseRecord(const tinygltf::Node& node) {
  return {{"name",poseDisplayName(node.name)},{"nameTruncated",node.name.size()>4000},{"camera",node.camera},{"matrix",node.matrix},
      {"translation",node.translation},{"rotation",node.rotation},{"scale",node.scale}};
}
uint64_t poseShadowDrawRevision(std::span<const DrawCommand> draws) noexcept {
  uint64_t hash=1469598103934665603ull;
  const auto mix=[&](uint32_t v) {hash^=v;hash*=1099511628211ull;};
  mix(uint32_t(draws.size()));for(const auto& draw:draws) {
    mix(draw.indexCount);mix(draw.instanceCount);mix(draw.firstIndex);mix(draw.objectIndex);
  }return hash;
}
container::scene::SceneProviderBounds poseProviderBounds(const SceneController::PreparedGeneration& generation) {
  container::scene::SceneProviderBounds result;uint64_t work{};
  const auto vertices=generation.geometryVertices();const auto indices=generation.geometryIndices();
  const auto objects=generation.objectData();
  const auto include=[&](std::span<const DrawCommand> draws) {
    for(const auto& draw:draws) {
      if(uint64_t(draw.firstIndex)+draw.indexCount>indices.size()) continue;
      for(uint32_t instance=0;instance<draw.instanceCount;++instance) {
        const auto object=uint64_t(draw.objectIndex)+instance;
        if(object>=objects.size()) throw std::invalid_argument("Camera pose draw has no object");
        for(uint32_t i=0;i<draw.indexCount;++i) {
          if(++work>512ull*1024*1024) throw std::length_error("Camera pose provider bounds exceed their work limit");
          const auto vertex=indices[size_t(draw.firstIndex)+i];
          if(vertex>=vertices.size()) throw std::invalid_argument("Camera pose draw has no vertex");
          const auto p=glm::vec3(objects[size_t(object)].model*glm::vec4(vertices[vertex].position,1));
          if(!result.valid) {result.min=p;result.max=p;result.valid=true;}
          else {result.min=glm::min(result.min,p);result.max=glm::max(result.max,p);}
        }
      }
    }
  };
  include(generation.opaqueDrawCommands());include(generation.transparentDrawCommands());return result;
}
}

struct RendererFrontend::CameraViewPoseHistoryBudget {
  static constexpr uint64_t limit=64ull*1024*1024;
  uint64_t reserved{};
};
struct RendererFrontend::CameraViewPoseHistory {
  Manager::CameraViewPoseCommand command;
  uint64_t graph{},reservation{};
  std::shared_ptr<CameraViewPoseHistoryBudget> budget;
  ~CameraViewPoseHistory() {if(budget) budget->reserved-=reservation;}
};
struct RendererFrontend::CameraViewPoseViewState {
  const CameraController* controller{};
  glm::mat4 view{1};
  VkExtent2D extent{};
  std::optional<size_t> selectedCamera;
};
struct RendererFrontend::CameraViewPosePublication {
  Manager::PreparedCameraViewPoseEdit source;
};

glm::mat4 RendererFrontend::cameraViewportUnjitteredView() const {
  if(!subs_.cameraController || !subs_.cameraController->camera())
    throw std::invalid_argument("The viewport navigation camera is unavailable");
  return subs_.cameraController->camera()->viewMatrix();
}
uint64_t RendererFrontend::refreshCameraViewPoseViewRevision() {
  const auto view=cameraViewportUnjitteredView();const auto extent=svc_.swapChainManager.extent();
  if(!cameraViewPoseViewState_ || cameraViewPoseViewState_->controller!=subs_.cameraController.get() ||
      !samePoseMatrix(cameraViewPoseViewState_->view,view) ||
      cameraViewPoseViewState_->extent.width!=extent.width || cameraViewPoseViewState_->extent.height!=extent.height ||
      cameraViewPoseViewState_->selectedCamera!=gltfAnimationCameraNode_) {
    if(cameraViewPoseViewRevision_==UINT64_MAX) throw std::overflow_error("Viewport pose revision exhausted");
    if(!cameraViewPoseViewState_) cameraViewPoseViewState_=std::make_shared<CameraViewPoseViewState>();
    auto& state=*cameraViewPoseViewState_;state.controller=subs_.cameraController.get();state.view=view;
    state.extent=extent;state.selectedCamera=gltfAnimationCameraNode_;++cameraViewPoseViewRevision_;
  }
  return cameraViewPoseViewRevision_;
}
void RendererFrontend::updateCameraViewPoseDestination() {
  if(!subs_.sceneManager || !gltfAnimationCameraNode_) return;
  const auto node=cameraGraphNode(*gltfAnimationCameraNode_);
  cameraViewPoseDestination_=subs_.sceneManager->cameraViewPoseTarget(sceneGraph_,node);
}
void RendererFrontend::refreshCameraViewPoseInspector() {
  if(!subs_.guiManager) return;auto& controls=subs_.guiManager->gltfAnimationControls();
  const auto clear=[&] {
    auto error=std::move(controls.cameraViewPoseError);
    (void)container::ui::SetCameraViewPoseSnapshot(controls,std::nullopt);
    controls.cameraViewPoseError.swap(error);
  };
  try {
    if(!subs_.sceneManager || !cameraViewPoseDestination_) {clear();return;}
    const auto current=subs_.sceneManager->cameraViewPoseTarget(sceneGraph_,cameraViewPoseDestination_->camera.graphNode);
    if(!current || !samePoseDestination(*current,*cameraViewPoseDestination_)) {
      cameraViewPoseDestination_.reset();clear();return;
    }
    cameraViewPoseDestination_=*current;
    container::ui::CameraViewPoseSnapshot snapshot;snapshot.target=*current;
    snapshot.viewRevision=refreshCameraViewPoseViewRevision();snapshot.viewportActive=!gltfAnimationCameraNode_;
    const auto source=current->camera.sourceNode;
    if (const auto* node = sceneGraph_.getNode(current->camera.graphNode))
      snapshot.destinationName = poseDisplayName(node->name);
    if(snapshot.destinationName.empty()) snapshot.destinationName="Camera node "+std::to_string(source);
    if(!snapshot.viewportActive) snapshot.blockReason="Choose Viewport in Camera, then navigate to the pose to write.";
    else if(materialGesture_ || lightEditGestureActive() || transformDragSession_.active || sceneEditPublicationPending_ || pendingSceneRetirement_)
      snapshot.blockReason="Finish the current scene edit before writing a camera pose.";
    if(!container::ui::SetCameraViewPoseSnapshot(controls,snapshot))
      throw std::invalid_argument("Camera pose destination metadata exceeds the editor limits");
  } catch(const std::exception& error) {clear();controls.cameraViewPoseError=error.what();}
}
bool RendererFrontend::authorCameraViewPoseSource(int32_t source,std::optional<uint64_t> expected) {
  if(!subs_.sceneManager || source<0) throw std::invalid_argument("A camera pose needs an active source node");
  if(!gltfAnimation_ || gltfAnimationGraphIdentity_!=sceneGraph_.identity()) resetGltfAnimation();
  auto& manager=*subs_.sceneManager;
  if(expected && *expected!=manager.cameraRevision()) throw std::invalid_argument("The authored camera revision is stale");
  const auto target=manager.cameraViewPoseTarget(sceneGraph_,cameraGraphNode(size_t(source)));
  if(!target) throw std::invalid_argument("The camera pose destination has no current source/node lifetime");
  return authorCameraViewPose(*target,refreshCameraViewPoseViewRevision());
}
bool RendererFrontend::authorCameraViewPose(const Target& target,uint64_t expectedViewRevision) {
  if(subs_.sceneManager && (!gltfAnimation_ || gltfAnimationGraphIdentity_!=sceneGraph_.identity())) resetGltfAnimation();
  if(!subs_.sceneManager || !gltfAnimation_ || gltfAnimationCameraNode_)
    throw std::invalid_argument("Choose the free Viewport before writing its pose to an authored camera");
  if(subs_.sceneManager->isLinkedNodeReadOnly(sceneGraph_,target.camera.graphNode))
    throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
  if(!expectedViewRevision || refreshCameraViewPoseViewRevision()!=expectedViewRevision)
    throw std::invalid_argument("The viewport pose changed before the camera write");
  if(materialGesture_ || lightEditGestureActive() || transformDragSession_.active || sceneEditPublicationPending_ || pendingSceneRetirement_)
    throw std::invalid_argument("Finish the current scene edit before writing a camera pose");
  if(cameraViewPosePublications_==UINT64_MAX) throw std::overflow_error("Camera pose publication counter exhausted");
  auto& manager=*subs_.sceneManager;const auto view=cameraViewportUnjitteredView();
  CameraViewPosePublication publication;
  publication.source=manager.prepareCameraViewPoseEdit(target,view,sceneGraph_,gltfAnimation_.get(),poseAspect(svc_.swapChainManager));
  // A real authored-rest edit may leave every sampled pose field unchanged.
  // Keep the manager's owned-endpoint decision for history/publication.
  if(!publication.source.changed()) {
    cameraViewPoseDestination_=manager.cameraViewPoseTarget(sceneGraph_,target.camera.graphNode);
    return true;
  }
  ensureSceneEditHistory();
  if(!cameraViewPoseHistoryBudget_) cameraViewPoseHistoryBudget_=std::make_shared<CameraViewPoseHistoryBudget>();
  const auto budget=cameraViewPoseHistoryBudget_;const auto command=publication.source.command();
  constexpr uint64_t overhead=sizeof(CameraViewPoseHistory)+1024;
  if(!command || command.ownedBytes()>CameraViewPoseHistoryBudget::limit ||
      budget->reserved>CameraViewPoseHistoryBudget::limit-command.ownedBytes() ||
      overhead>CameraViewPoseHistoryBudget::limit-command.ownedBytes()-budget->reserved)
    throw std::length_error("Camera pose history exceeds its selected-record storage budget");
  auto state=std::make_shared<CameraViewPoseHistory>();state->command=command;state->graph=sceneGraph_.identity();
  state->reservation=command.ownedBytes()+overhead;state->budget=budget;budget->reserved+=state->reservation;
  container::util::EditHistory::Command history{"Write viewport camera default",
      [this,state] {return replayCameraViewPose(*state,GltfCameraEditSide::Before);},
      [this,state] {return replayCameraViewPose(*state,GltfCameraEditSide::After);},true};
  if(refreshCameraViewPoseViewRevision()!=expectedViewRevision || !publishCameraViewPose(publication,&view)) return false;
  sceneEdits_->finishGesture();sceneEdits_->recordAppliedPublished(std::move(history));
  cameraViewPoseDestination_=manager.cameraViewPoseTarget(sceneGraph_,target.camera.graphNode);
  return true;
}
bool RendererFrontend::replayCameraViewPose(const CameraViewPoseHistory& state,GltfCameraEditSide side) {
  if(!subs_.sceneManager || !gltfAnimation_ || state.graph!=sceneGraph_.identity() ||
      cameraViewPosePublications_==UINT64_MAX) return false;
  if(subs_.sceneManager->isLinkedNodeReadOnly(sceneGraph_,state.command.target().camera.graphNode)) return false;
  if(materialGesture_ || lightEditGestureActive() || transformDragSession_.active || sceneEditPublicationPending_ || pendingSceneRetirement_)
    throw std::invalid_argument("Finish the current scene edit before replaying a camera pose");
  CameraViewPosePublication publication;
  publication.source=subs_.sceneManager->prepareCameraViewPoseReplay(state.command,side,sceneGraph_,gltfAnimation_.get(),
      poseAspect(svc_.swapChainManager));
  return publishCameraViewPose(publication,nullptr);
}
void RendererFrontend::processCameraViewPoseRequests() {
  if(!subs_.guiManager) return;auto& controls=subs_.guiManager->gltfAnimationControls();
  auto request=std::exchange(controls.requestedCameraViewPose,{});if(!request) return;
  try {
    if(controls.requestedCameraProjection || controls.requestedCameraNode || controls.requestedClip ||
        controls.requestedTime || controls.requestedSpeed || controls.requestedLoop || controls.requestedPlaying)
      throw std::invalid_argument("Apply camera selection or playback changes before writing its pose");
    if(!authorCameraViewPose(request->target,request->viewRevision)) throw std::runtime_error("The camera pose could not be published");
    controls.cameraViewPoseError.clear();refreshCameraViewPoseInspector();
  } catch(const std::exception& error) {controls.cameraViewPoseError=error.what();}
}

bool RendererFrontend::publishCameraViewPose(CameraViewPosePublication& publication,const glm::mat4* desiredView) {
  if(!subs_.sceneManager || !subs_.sceneController || !subs_.lightingManager || !subs_.frameResourceManager ||
      !subs_.frameRuntimeResourceRegistry || !subs_.sceneProviderSynchronizer || !subs_.sceneProviderRegistry || !gltfAnimation_)
    throw std::runtime_error("Writing camera poses requires an initialized scene renderer");
  auto& manager=*subs_.sceneManager;auto& controller=*subs_.sceneController;auto& lightManager=*subs_.lightingManager;
  auto& pose=publication.source;if(!pose) return false;if(!pose.changed()) return true;
  if(cameraViewPosePublications_==UINT64_MAX || manager.generationEpoch()==UINT64_MAX)
    throw std::overflow_error("Camera pose publication generation exhausted");
  const auto imageCount=uint32_t(svc_.swapChainManager.imageCount());
  if(!imageCount || imageCount>64 || imageCount!=buffers_.cameras.size() || imageCount!=buffers_.objects.size() ||
      imageCount!=buffers_.objectCapacities.size() || imageCount!=subs_.frameResourceManager->frameCount())
    throw std::runtime_error("Camera pose frame resources are not ready");
  const auto& graph=pose.candidateGraph();const auto target=pose.command().target();
  if(graph.nodeCount()>262144) throw std::length_error("Camera pose graph exceeds its publication limit");
#if CONTAINER_HAS_JOLT
  // A physical/provider body owns its world transform separately. A graph-only
  // pose write cannot atomically author that second format or its body state.
  std::vector<uint32_t> descendants{target.camera.graphNode};uint64_t physicsWork{};
  for(size_t i=0;i<descendants.size();++i) {
    if(++physicsWork>524288 || descendants[i]>=graph.nodeCount())
      throw std::length_error("Camera pose physics ownership walk exceeds its limit");
    const auto id=descendants[i];const auto* entry=graph.getNode(id);
    if((physicsScene_ && physicsScene_->hasBody(id)) ||
        (physicsElements_ && physicsElements_->ownsNode(sceneGraph_,id)))
      throw std::invalid_argument("Detach physics/provider bindings in this camera subtree before writing its pose");
    if(entry->children.size()>524288-descendants.size()) throw std::length_error("Camera pose subtree exceeds its edge limit");
    descendants.insert(descendants.end(),entry->children.begin(),entry->children.end());
  }
#endif
  const auto graphIdentity=sceneGraph_.identity(),graphRevision=sceneGraph_.revision();
  const auto epoch=manager.generationEpoch(),geometry=manager.geometryRevision(),material=manager.materialRevision();
  const auto aspect=poseAspect(svc_.swapChainManager);
  if(desiredView && (gltfAnimationCameraNode_ || !samePoseMatrix(*desiredView,cameraViewportUnjitteredView())))
    throw std::invalid_argument("The free viewport changed before writing its camera pose");
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire camera pose readers");
  detail::ScenePublicationExternalProof external(subs_.bimManager.get(),*subs_.sceneProviderRegistry,
      manager.materialCount(),false,std::nullopt,&manager);
  constexpr uint64_t cpuLimit=2ull*1024*1024*1024;
  uint64_t cpu=pose.ownedBytes();
  const auto charge=[&](uint64_t bytes) {if(bytes>cpuLimit-cpu) throw std::length_error("Camera pose staging exceeds its aggregate CPU budget");cpu+=bytes;};
  if(cpu>cpuLimit) throw std::length_error("Camera pose preparation exceeds its aggregate CPU budget");
  charge(external.ownedBytes());
  const bool showDiag=subs_.guiManager && subs_.guiManager->showNormalDiagCube();
  SceneController::GraphPoseGenerationLimits controllerLimits;
  controllerLimits.maxCpuBytes=std::min(controllerLimits.maxCpuBytes,cpuLimit-cpu);
  auto nextController=controller.prepareGraphPoseGeneration(graph,imageCount,showDiag,graphRevision,epoch,controllerLimits);
  charge(nextController.ownedBytes());
  auto oldLightEdits=lightManager.snapshotSceneLightEdits();
  auto nextImported=std::make_shared<ImportedSceneLightEdits>(gltfImportedLightEdits_?*gltfImportedLightEdits_:ImportedSceneLightEdits{});
  nextImported->capture(manager.publishedAuthoredSceneLights(),oldLightEdits);
  const auto edits=nextImported->merge(pose.candidateLights(),oldLightEdits);
  LightingGraphPoseGenerationLimits lightLimits;lightLimits.maxPreparedCpuBytes=std::min(lightLimits.maxPreparedCpuBytes,cpuLimit-cpu);
  auto nextLighting=lightManager.prepareGraphPoseGeneration(manager,graph,nextController.world(),sceneState_.rootNode,
      pose.candidateLights(),pose.candidateBounds(),edits,imageCount,lightLimits);
  charge(nextLighting.ownedBytes());
  auto sceneLightEdits=gltfSceneLightEdits_;
  sceneLightEdits[manager.selectedScene()]=std::make_shared<SceneLightEdits>(edits);
  auto sceneBindings=manager.prepareSceneBindings();
  if(subs_.bimManager && subs_.bimManager->hasScene()) {
    sceneBindings.prepareDescriptorFrames(buffers_.cameras,nextController.objectBuffers(),
        subs_.bimManager->objectBuffers());
  } else sceneBindings.prepareDescriptorFrames(buffers_.cameras,nextController.objectBuffers());

  std::unique_ptr<GltfDeformationPass> deformation;
  const auto* deformed=nextController.deformationScene();const auto topology=deformed?deformed->topologyRevision():0;
  const auto vertexCount=nextController.gpuBindVertices().size();
  if(deformed && !deformed->gpuInstances().empty()) {
    deformation=std::make_unique<GltfDeformationPass>(svc_.ctx.deviceWrapper,svc_.allocationManager,svc_.pipelineManager);
    deformation->createResources(container::util::executableDirectory(),imageCount);
    deformation->setScene(nextController.gpuBindVertices(),deformed->gpuInstances());
    for(uint32_t image=0;image<imageCount;++image) deformation->uploadFrame(image,deformed->gpuPoses());
  }
  std::vector<VkImageView> anisotropy,diffuse;
  if(subs_.pbrAuxiliaryPass && subs_.pbrAuxiliaryPass->ready()) for(uint32_t i=0;i<imageCount;++i) {
    anisotropy.push_back(subs_.pbrAuxiliaryPass->anisotropyView(i));
    diffuse.push_back(subs_.pbrAuxiliaryPass->diffuseTransmissionView(i));
  }
  auto auxiliaryBindings=subs_.frameResourceManager->prepareAuxiliaryBindings(anisotropy,diffuse);
  std::optional<OpenVdbVolumeRenderer::PreparedSceneBindings> volumeBindings;
  if(subs_.openVdbVolumeRenderer) volumeBindings=subs_.openVdbVolumeRenderer->prepareSceneBindings(
      nextLighting.lightingBuffers(),manager.sceneClipBuffers(),nextLighting.pointLightBuffers(),nextLighting.areaLightBuffers());
  std::optional<GpuCullManager::PreparedGeneration> culling;
  const auto cullDraws=nextController.opaqueSingleSidedDrawCommands();
  if(subs_.gpuCullManager) culling=subs_.gpuCullManager->prepareGeneration(nextController.objectBuffers(),
      nextController.objectBufferCapacities(),buffers_.cameras,sizeof(container::gpu::CameraData),cullDraws,nextController.objectDataRevision());
  std::optional<ShadowCullManager::PreparedGeneration> shadowCulling;
  if(subs_.shadowCullManager && subs_.shadowManager) {
    std::vector<VkBuffer> ubos;for(const auto& buffer:subs_.shadowManager->shadowCullUbos()) ubos.push_back(buffer.buffer);
    shadowCulling=subs_.shadowCullManager->prepareGeneration(nextController.objectBuffers(),nextController.objectBufferCapacities(),
        ubos,sizeof(container::gpu::ShadowCullData),cullDraws,poseShadowDrawRevision(cullDraws));
  }
  container::scene::MeshSceneProviderAssetCache assetCache;
  const auto& mesh=assetCache.get({.primitiveRanges=manager.primitiveRanges(),.materialCount=manager.materialCount(),
      .instanceCount=nextController.objectData().size(),.bounds=poseProviderBounds(nextController),
      .materialFactsAt=[&manager](uint32_t row) {const auto facts=manager.materialRenderProperties(row);
        return container::scene::MeshSceneProviderMaterialFacts{facts.transparent,facts.doubleSided};},
      .bindings=MakeMeshSceneProviderBindingView(nextController.objectData(),nextController.opaqueDrawCommands(),
          nextController.transparentDrawCommands(),uint64_t(nextController.geometryIndices().size()))},
      {.geometry=geometry,.materials=material,.instances=nextController.objectDataRevision()},subs_.sceneManager.get());
  MeshSceneProviderSyncInput nextMesh{.available=mesh.primitiveCount>0 || mesh.bounds.valid,.primitiveCount=mesh.primitiveCount,
      .materialCount=mesh.materialCount,.instanceCount=mesh.instanceCount,.triangleBatches=mesh.triangleBatches,.bounds=mesh.bounds,
      .geometryRevision=nextController.geometryRevision(),.materialRevision=material,.instanceRevision=nextController.objectDataRevision(),
      .displayName=activePrimaryModelPath_.empty()?"Primary mesh scene":activePrimaryModelPath_,.triangleBatchesAuthoritative=mesh.triangleBatchesAuthoritative};
  auto providerInput=buildSceneProviderSyncInput(&nextMesh);SceneProviderSynchronizer providers;
  container::scene::SceneProviderRegistry registry;providers.sync(registry,providerInput);
  std::unique_ptr<RayShadowManager> ray;
  if(subs_.rayShadowManager) {
    ray=subs_.rayShadowManager->createDetachedGeneration();
    const bool gi=subs_.ddgiManager && subs_.ddgiManager->wantsScene() && pushConstants_.bindless.semanticColorMode==0 &&
        (!graph.renderableNodes().empty() || (subs_.bimManager && subs_.bimManager->hasScene()));
    ray->requestSurfaceScene(gi,subs_.ddgiManager?subs_.ddgiManager->settings().surfaceBudgetBytes:svc_.config.ddgi.surfaceBudgetBytes);
    std::vector<DrawCommand> meshDraws(nextController.opaqueDrawCommands().begin(),nextController.opaqueDrawCommands().end());
    if(gi) meshDraws.insert(meshDraws.end(),nextController.transparentDrawCommands().begin(),nextController.transparentDrawCommands().end());
    std::vector<RaySceneProviderSource> sources;const bool wanted=ray->supported() && (gi || ray->settings().mode!=RayShadowMode::Raster);
    const auto vertices=deformed?nextController.geometryVertices():std::span<const container::geometry::Vertex>(manager.vertices());
    const auto indices=deformed?nextController.geometryIndices():std::span<const uint32_t>(manager.indices());
    if(providerInput.mesh.available && wanted) sources.push_back({{"primary-mesh-scene"},nextController.geometryRevision(),
        vertices,indices,nextController.objectData(),meshDraws});
    std::vector<DrawCommand> bimDraws;
    if(subs_.bimManager && subs_.bimManager->hasScene() && wanted) {
      auto filter=currentBimDrawFilter();filter.drawBudgetEnabled=false;filter.drawBudgetMaxObjects=0;
      const auto add=[&](const auto& draws) {for(const auto& draw:draws)
        if(subs_.bimManager->objectMatchesFilter(draw.objectIndex,filter) && bimObjectVisibleByLayer(draw.objectIndex)) bimDraws.push_back(draw);};
      add(subs_.bimManager->opaqueDrawCommands());if(gi) add(subs_.bimManager->transparentDrawCommands());
      sources.push_back({{"auxiliary-bim-scene"},subs_.bimManager->geometryRevision(),subs_.bimManager->vertices(),
          subs_.bimManager->indices(),subs_.bimManager->objectData(),bimDraws});
    }
    bool compatible=true;for(const auto& source:sources) for(const auto& draw:source.draws)
      for(uint32_t instance=0;instance<draw.instanceCount;++instance) {
        const auto object=uint64_t(draw.objectIndex)+instance;
        if(object>=source.objects.size() || !manager.rayGeometryCompatible(source.objects[size_t(object)].objectInfo.x)) compatible=false;
      }
    for(uint32_t image=0;image<imageCount;++image) {
      ray->update(image,buffers_.cameraData,*subs_.frameResourceManager->frame(image),sceneBindings.descriptorSet(image),
          nextLighting.lightDescriptorSet(image),sources,nextLighting.lightingData(),nextLighting.pointLightsSsbo(),nextLighting.areaLightsSsbo(),
          pushConstants_.bindless.sectionPlaneEnabled,pushConstants_.bindless.sectionPlane,manager.sceneClipState(),
          [&manager](uint32_t row) {return manager.temporalMaterialRevision(row);},compatible);
      svc_.allocationManager.executeStagedCommands([&](VkCommandBuffer command) {ray->recordBuild(command,image);});
      nextLighting.updateRayShadowDescriptors(image,ray->readSettings(image),ray->readSettingsSize(),ray->visibilityView());
    }
  }
  std::optional<DdgiManager::PreparedSceneHistoryReset> ddgi;
  std::optional<DdgiManager::PreparedLightSnapshotCapacity> ddgiLights;
  if(subs_.ddgiManager) {ddgi=subs_.ddgiManager->prepareSceneHistoryReset();
    ddgiLights=subs_.ddgiManager->prepareLightSnapshotCapacity(nextLighting.pointLightsSsbo().size(),nextLighting.areaLightsSsbo().size());}
  auto runtime=*subs_.frameRuntimeResourceRegistry;
  const auto bind=[&](RenderTechniqueId technique,uint32_t image) {
    runtime.bindDescriptorSet(technique,"scene-descriptor-set",image,{sceneBindings.descriptorSet(image)});
    runtime.bindDescriptorSet(technique,"light-descriptor-set",image,{nextLighting.lightDescriptorSet(image)});
    runtime.bindDescriptorSet(technique,"tiled-lighting-descriptor-set",image,{nextLighting.tiledDescriptorSet(image)});
    if(subs_.bimManager && subs_.bimManager->hasScene()) runtime.bindDescriptorSet(technique,"bim-scene-descriptor-set",image,{sceneBindings.auxiliaryDescriptorSet(image)});
    runtime.bindBuffer(technique,"scene-object-buffer",image,{nextController.objectBuffers()[image].buffer,
        sizeof(container::gpu::ObjectData)*nextController.objectBufferCapacities()[image],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  };
  for(uint32_t image=0;image<imageCount;++image) {bind(RenderTechniqueId::DeferredRaster,image);bind(RenderTechniqueId::ForwardRaster,image);
    if(subs_.activeTechnique && subs_.activeTechnique->id()!=RenderTechniqueId::DeferredRaster && subs_.activeTechnique->id()!=RenderTechniqueId::ForwardRaster) bind(subs_.activeTechnique->id(),image);}
  std::vector<container::gpu::AllocatedBuffer> objects(nextController.objectBuffers().begin(),nextController.objectBuffers().end());
  std::vector<size_t> capacities(nextController.objectBufferCapacities().begin(),nextController.objectBufferCapacities().end());
  std::vector<bool> shadowReady(imageCount,shadowCulling.has_value());std::string reason="authored camera pose edit or history";
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS) throw std::runtime_error("Failed to retire staged camera pose readers");
  const auto currentView=desiredView?cameraViewportUnjitteredView():glm::mat4(1);
  if(sceneGraph_.identity()!=graphIdentity || sceneGraph_.revision()!=graphRevision ||
      (desiredView && (gltfAnimationCameraNode_ || !samePoseMatrix(*desiredView,currentView))) ||
      !external.matches(*subs_.sceneProviderRegistry) ||
      !manager.canExchangeCameraViewPoseEdit(pose,sceneGraph_,gltfAnimation_.get(),aspect,desiredView?&currentView:nullptr) ||
      !manager.canExchangeSceneBindings(sceneBindings) || !controller.canExchangeGeneration(nextController,epoch) ||
      !lightManager.canExchangeGeneration(nextLighting) || (culling && !subs_.gpuCullManager->canExchangeGeneration(*culling)) ||
      (shadowCulling && !subs_.shadowCullManager->canExchangeGeneration(*shadowCulling)) ||
      (ray && !subs_.rayShadowManager->canExchangeGeneration(*ray)) || !subs_.frameResourceManager->canPublishAuxiliaryBindings(auxiliaryBindings) ||
      (volumeBindings && !subs_.openVdbVolumeRenderer->canPublishSceneBindings(*volumeBindings)) ||
      (ddgi && !subs_.ddgiManager->canPublishSceneHistoryReset(*ddgi)) ||
      (ddgiLights && !subs_.ddgiManager->canPublishLightSnapshotCapacity(*ddgiLights)))
    throw std::runtime_error("Scene, source or viewport changed while preparing the camera pose");
  if(!manager.exchangeCameraViewPoseEdit(pose,sceneGraph_,gltfAnimation_.get(),aspect,desiredView?&currentView:nullptr)) return false;
  controller.exchangeGeneration(nextController,sceneGraph_.revision(),geometry,epoch+1);
  if(!lightManager.exchangeGeneration(nextLighting)) std::terminate();
  manager.exchangeSceneBindings(sceneBindings);
  if(manager.generationEpoch()!=epoch+1) std::terminate();
  if(culling && !subs_.gpuCullManager->exchangeGeneration(*culling)) std::terminate();
  if(shadowCulling && !subs_.shadowCullManager->exchangeGeneration(*shadowCulling)) std::terminate();
  if(ray && !subs_.rayShadowManager->exchangeGeneration(*ray)) std::terminate();
  if(ddgiLights && !subs_.ddgiManager->publishLightSnapshotCapacity(*ddgiLights)) std::terminate();
  if(ddgi && !subs_.ddgiManager->publishSceneHistoryReset(*ddgi)) std::terminate();
  if(volumeBindings && !subs_.openVdbVolumeRenderer->publishSceneBindings(*volumeBindings)) std::terminate();
  if(!subs_.frameResourceManager->publishAuxiliaryBindings(auxiliaryBindings)) std::terminate();
  subs_.gltfDeformationPass.swap(deformation);subs_.sceneProviderSynchronizer->exchangeState(providers,*subs_.sceneProviderRegistry,registry);
  subs_.frameRuntimeResourceRegistry->swapState(runtime);std::swap(meshProviderAssetCache_,assetCache);
  gltfImportedLightEdits_.swap(nextImported);gltfSceneLightEdits_.swap(sceneLightEdits);
  buffers_.objects.swap(objects);buffers_.objectCapacities.swap(capacities);buffers_.shadowObjectDescriptorReady.swap(shadowReady);
  syncSceneStateFromController();gltfAnimationCamera_.reset();cameraProjectionInspectorCache_.reset();
  clearHoveredMeshNode();selectionNavigationAnchor_={};selectedDrawCommands_.clear();selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();selectedBimNativeCurveDrawCommands_.clear();
  gltfDeformationGraphIdentity_=subs_.gltfDeformationPass?sceneGraph_.identity():0;
  gltfDeformationSourceRevision_=geometry;gltfDeformationTopologyRevision_=topology;gltfDeformationVertexCount_=vertexCount;
  gltfDeformationImageCount_=imageCount;gltfDeformationSubmittedEpoch_.reset();gltfDeformationHistoryReset_=true;
  gltfDeformationMaxPositionDelta_=0;ddgiSceneChanges_.clear();ddgiTrackedSceneRevision_=0;++cameraViewPosePublications_;
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(reason));
  for(auto& object:objects) if(object.buffer && !controller.ownsObjectBuffer(object.buffer) &&
      std::ranges::none_of(nextController.objectBuffers(),[&](const auto& owned) {return owned.buffer==object.buffer;}))
    svc_.allocationManager.memoryManager()->destroyBuffer(object);
  return true;
}

void RendererFrontend::appendCameraViewPoseTelemetry(nlohmann::json& json) const {
  auto& result=json["gltfCameraPoses"];
  result={{"publications",cameraViewPosePublications_},{"viewRevision",cameraViewPoseViewRevision_},
      {"viewportActive",!gltfAnimationCameraNode_},{"destinationSourceNode",cameraViewPoseDestination_?cameraViewPoseDestination_->camera.sourceNode:-1},
      {"nodes",nlohmann::json::array()},{"truncated",false}};
  if(!subs_.sceneManager) return;
  const auto authored=subs_.sceneManager->authoredCameraSourceView();const auto& evaluated=subs_.sceneManager->animationSource();
  const auto* rest=gltfAnimation_?&gltfAnimation_->library().restNodes:nullptr;
  if(authored.nodes.size()>1'000'000 || evaluated.nodes.size()>1'000'000 || sceneGraph_.nodeCount()>262144) {result["truncated"]=true;return;}
  for(uint32_t graph=0;graph<sceneGraph_.nodeCount();++graph) {
    const auto* node=sceneGraph_.getNode(graph);if(!node || node->sourceNodeIndex<0) continue;
    const auto source=size_t(node->sourceNodeIndex);
    if(source>=authored.nodes.size() || source>=evaluated.nodes.size() || authored.nodes[source].camera<0) continue;
    const auto bounded=[](const tinygltf::Node& n) {return n.matrix.size()<=16 && n.translation.size()<=3 && n.rotation.size()<=4 && n.scale.size()<=3;};
    if(!bounded(authored.nodes[source]) || !bounded(evaluated.nodes[source]) ||
        (rest && source<rest->size() && !bounded((*rest)[source]))) {result["truncated"]=true;continue;}
    if(result["nodes"].size()>=256) {result["truncated"]=true;break;}
    nlohmann::json row={{"graphNode",graph},{"sourceNode",source},{"nodeIncarnation",sceneGraph_.nodeIncarnation(graph)},
        {"authored",poseRecord(authored.nodes[source])},{"evaluated",poseRecord(evaluated.nodes[source])}};
    if(rest && source<rest->size()) row["rest"]=poseRecord((*rest)[source]);
    result["nodes"].push_back(std::move(row));
  }
}

} // namespace container::renderer
