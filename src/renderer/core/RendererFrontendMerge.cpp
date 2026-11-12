#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/geometry/GltfMerge.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/effects/OpenVdbVolumeRenderer.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/ImportedSceneLightEdits.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/GltfDeformationPass.h"
#include "Container/renderer/scene/GltfDeformationScene.h"
#include "Container/renderer/scene/PbrAuxiliaryPass.h"
#include "Container/renderer/scene/MeshSceneProviderBindings.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneNodeAuthoringPolicy.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsScene.h"
#include "Container/physics/PhysicsElementBindings.h"
#endif

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <nlohmann/json.hpp>

namespace container::renderer {

struct RendererFrontend::SceneMergeBudget {
  static constexpr uint64_t limit=4ull*1024*1024*1024;
  uint64_t reserved{};
};

// Retain source/material/texture ownership, not viewport-dependent dynamic
// resources. Every replay stages against the current camera and frame table.
struct RendererFrontend::SceneMergeHistory {
  explicit SceneMergeHistory(container::scene::SceneManager::PreparedMergeGeneration value)
      : assets(std::move(value)) {}
  ~SceneMergeHistory() { if(budget) budget->reserved-=reservation; }
  std::shared_ptr<SceneMergeBudget> budget;
  uint64_t reservation{};
  container::scene::SceneManager::PreparedMergeGeneration assets;
  uint64_t graphIdentity{};
  uint32_t first{}, end{};
  bool merged{false};
  bool importedPublication{true};
  std::optional<std::vector<uint32_t>> undoRoots;
  std::optional<container::scene::SceneGraph::DetachedTail> detached;
  std::vector<container::scene::SceneNodeLocalPose> graphPose;
  std::unique_ptr<container::geometry::gltf::AnimationPlayer> animation;
  std::optional<size_t> cameraNode;
  SceneLightEdits lightEdits;
  std::shared_ptr<ImportedSceneLightEdits> imported;
  std::map<size_t,std::shared_ptr<SceneLightEdits>> sceneLightEdits;
  SceneEditSelection before{}, after{};
};

namespace {
uint64_t addMergeBytes(uint64_t a,uint64_t b) {
  if(b>UINT64_MAX-a) throw std::length_error("Scene merge history size overflow");
  return a+b;
}
uint64_t graphHistoryBytes(const container::scene::SceneGraph& graph) {
  uint64_t bytes=sizeof(graph);
  for(uint32_t i=0;i<graph.nodeCount();++i) {
    const auto* node=graph.getNode(i);
    bytes=addMergeBytes(bytes,sizeof(*node)+node->name.capacity()+1);
    bytes=addMergeBytes(bytes,uint64_t(node->children.capacity())*sizeof(uint32_t)+64);
  }
  return bytes;
}
uint64_t decodedClipBytes(const container::geometry::gltf::AnimationPlayer* player) {
  if(!player) return 0;
  uint64_t bytes=sizeof(*player);
  for(const auto& clip:player->library().clips) {
    bytes=addMergeBytes(bytes,sizeof(clip)+clip.name.capacity()+1);
    for(const auto& track:clip.tracks) {
      bytes=addMergeBytes(bytes,sizeof(track)+track.target.property.capacity()+1);
      bytes=addMergeBytes(bytes,uint64_t(track.times.capacity()+track.values.capacity())*sizeof(double));
    }
  }
  return bytes;
}
uint64_t shadowDrawRevision(std::span<const DrawCommand> draws) {
  uint64_t hash=1469598103934665603ull;
  const auto mix=[&](uint32_t value) { hash^=value; hash*=1099511628211ull; };
  mix(static_cast<uint32_t>(draws.size()));
  for(const auto& draw:draws) {
    mix(draw.indexCount); mix(draw.instanceCount);
    mix(draw.firstIndex); mix(draw.objectIndex);
  }
  return hash;
}

container::scene::SceneProviderBounds mergeBounds(const SceneController::PreparedGeneration& controller) {
  container::scene::SceneProviderBounds result;
  const auto vertices=controller.geometryVertices();
  const auto indices=controller.geometryIndices();
  const auto objects=controller.objectData();
  const auto include=[&](std::span<const DrawCommand> draws) {
    for(const auto& draw:draws) {
      if(uint64_t(draw.firstIndex)+draw.indexCount>indices.size()) continue; // diagnostic tail
      for(uint32_t instance=0;instance<draw.instanceCount;++instance) {
        const auto object=uint64_t(draw.objectIndex)+instance;
        if(object>=objects.size()) throw std::runtime_error("Merge draw references an absent object");
        for(uint32_t i=0;i<draw.indexCount;++i) {
          const auto vertex=indices[size_t(draw.firstIndex)+i];
          if(vertex>=vertices.size()) throw std::runtime_error("Merge draw references an absent vertex");
          const auto p=glm::vec3(objects[size_t(object)].model*glm::vec4(vertices[vertex].position,1.0f));
          if(!result.valid) { result.min=p; result.max=p; result.valid=true; }
          else { result.min=glm::min(result.min,p); result.max=glm::max(result.max,p); }
        }
      }
    }
  };
  include(controller.opaqueDrawCommands()); include(controller.transparentDrawCommands());
  return result;
}
} // namespace

bool RendererFrontend::mergeSceneModel(const std::string& path,
    const container::geometry::gltf::GltfMergeOptions& options,
    const container::geometry::GltfLoadResult* prepared) {
  finishEditGestures();
  if(!subs_.sceneManager || !subs_.sceneController || !subs_.lightingManager)
    throw std::runtime_error("Scene merge requires an initialized primary renderer");
  ensureSceneEditHistory();
  sceneEdits_->finishGesture();
  // Retire shared scene readers before cloning mapped state or staging uploads.
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire scene readers before merge");
  std::optional<container::geometry::GltfLoadResult> local;
  if (!prepared) {
    local=container::scene::SceneManager::prepareGltfModel(path,512ull*1024*1024,
        svc_.config.experimentalGltfLinkedAssets, false, svc_.config.experimentalGltfMaterials, false, true);
    prepared=&*local;
  }
  if(!mergeBudget_) mergeBudget_=std::make_shared<SceneMergeBudget>();
  const auto beforeBytes=subs_.sceneManager->mergeHistoryResidentBytes();
  const auto activeBytes=addMergeBytes(addMergeBytes(beforeBytes,beforeBytes),
      addMergeBytes(graphHistoryBytes(sceneGraph_),decodedClipBytes(gltfAnimation_.get())));
  const auto usedBytes=addMergeBytes(mergeBudget_->reserved,activeBytes);
  if(usedBytes>=SceneMergeBudget::limit)
    throw std::length_error("Scene merge history CPU payload budget is exhausted");
  container::scene::SceneMergeGenerationLimits limits;
  limits.maxPreparedCpuBytes=std::min(limits.maxPreparedCpuBytes,SceneMergeBudget::limit-usedBytes);
  auto state=std::make_shared<SceneMergeHistory>(subs_.sceneManager->prepareMergeGeneration(
      *prepared,container::util::pathFromUtf8(path),options,sceneGraph_,limits));
  const auto& source=state->assets.manager().animationSource();
  state->animation=gltfAnimation_
      ? std::make_unique<container::geometry::gltf::AnimationPlayer>(gltfAnimation_->withExtendedSource(source))
      : std::make_unique<container::geometry::gltf::AnimationPlayer>(source);
  return commitSceneAddition(std::move(state), "Merge "+options.parentName,
      beforeBytes,usedBytes,std::nullopt,true);
}

bool RendererFrontend::duplicateSceneNode(uint32_t node,std::optional<std::string> name) {
  finishEditGestures();
  using container::scene::SceneGraph;
  if(!subs_.sceneManager || !subs_.sceneController || !subs_.lightingManager)
    throw std::invalid_argument("Subtree duplication requires an initialized primary scene");
  const auto root=container::scene::ResolveSceneNodeAuthoringTarget(sceneGraph_,node,
      container::scene::SceneNodeAuthoringPurpose::DuplicateSubtree);
  const auto closure=container::scene::CollectSceneNodeAuthoringSubtree(sceneGraph_,root);
  for (const auto id : closure.nodes) requireEditableSceneNode(id);
  if(closure.containsProtectedNodes)
    throw std::invalid_argument("A protected structural node cannot be duplicated");
  if(!name) {
    name=sceneGraph_.getNode(root)->name+" copy";
    try { container::scene::ValidateSceneNodeName(*name); }
    catch(const std::invalid_argument&) { name="Node copy"; }
  } else container::scene::ValidateSceneNodeName(*name);
#if CONTAINER_HAS_JOLT
  std::vector<uint8_t> constrained(sceneGraph_.nodeCount(),0);
  for(uint32_t id=0;id<sceneGraph_.nodeCount();++id)
    if((physicsScene_ && physicsScene_->graphIdentity()==sceneGraph_.identity() && physicsScene_->hasBody(id)) ||
        (physicsElements_ && physicsElements_->ownsNode(sceneGraph_,id))) constrained[id]=1;
  if(physicsScene_ && physicsScene_->graphIdentity()==sceneGraph_.identity()) {
    if(physicsScene_->jointCount()>131072)
      throw std::length_error("Subtree duplication joint metadata budget exceeded");
    for(const auto& joint:physicsScene_->joints()) {
      if(joint.nodeA && *joint.nodeA<constrained.size()) constrained[*joint.nodeA]=1;
      if(joint.nodeB<constrained.size()) constrained[joint.nodeB]=1;
    }
  }
  for(const auto id:closure.nodes) if(constrained[id])
    throw std::invalid_argument("Detach physics bodies, joints and product bindings before duplicating their subtree");
#endif
  ensureSceneEditHistory(); sceneEdits_->finishGesture();
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire scene readers before duplication");
  if(pendingSceneRetirement_) publishSceneEdit();
  if(!mergeBudget_) mergeBudget_=std::make_shared<SceneMergeBudget>();
  const auto beforeBytes=subs_.sceneManager->mergeHistoryResidentBytes();
  const auto activeBytes=addMergeBytes(addMergeBytes(beforeBytes,beforeBytes),
      addMergeBytes(graphHistoryBytes(sceneGraph_),decodedClipBytes(gltfAnimation_.get())));
  const auto usedBytes=addMergeBytes(mergeBudget_->reserved,activeBytes);
  if(usedBytes>=SceneMergeBudget::limit)
    throw std::length_error("Scene addition history CPU payload budget is exhausted");
  container::scene::SceneMergeGenerationLimits limits;
  limits.maxPreparedCpuBytes=std::min(limits.maxPreparedCpuBytes,SceneMergeBudget::limit-usedBytes);
  auto state=std::make_shared<SceneMergeHistory>(subs_.sceneManager->prepareSubtreeCloneGeneration(
      sceneGraph_,root,name,limits));
#if CONTAINER_HAS_JOLT
  // The required skin/mesh contexts can include graph-only ancestors outside
  // the visual subtree. Their actual original graph IDs are explicit proof
  // data; source labels and source indices cannot identify physics ownership.
  for(const auto original:state->assets.cloneOriginalGraphNodes())
    if(original>=constrained.size() || constrained[original])
      throw std::invalid_argument("A duplication dependency has a physics or product binding");
#endif
  const auto& source=subs_.sceneManager->animationSource();
  state->animation=gltfAnimation_
      ? std::make_unique<container::geometry::gltf::AnimationPlayer>(gltfAnimation_->withClonedSource(
          source,state->assets.projectedClonePlan(),state->assets.cloneOwnership()))
      : std::make_unique<container::geometry::gltf::AnimationPlayer>(state->assets.manager().animationSource());
  const auto selected=state->assets.cloneRoot();
  return commitSceneAddition(std::move(state),"Duplicate subtree",beforeBytes,usedBytes,selected,false);
}

bool RendererFrontend::commitSceneAddition(std::shared_ptr<SceneMergeHistory> state,
    std::string label,uint64_t beforeBytes,uint64_t usedBytes,
    std::optional<uint32_t> selection,bool imported) {
  state->importedPublication=imported;
  state->graphIdentity=sceneGraph_.identity();
  state->first=static_cast<uint32_t>(sceneGraph_.nodeCount());
  state->end=static_cast<uint32_t>(state->assets.graph().nodeCount());
  state->undoRoots=sceneGraph_.activeRoots();
  state->before=sceneEditSelection(); state->after=state->before;
  if(selection) {
    state->after.mesh=*selection;
    state->after.bim=container::scene::SceneGraph::kInvalidNode;
    state->after.light={}; state->after.sectionEditable=false;
  }
  state->cameraNode=gltfAnimationCameraNode_;
  state->lightEdits=subs_.lightingManager->snapshotSceneLightEdits();
  state->imported=std::make_shared<ImportedSceneLightEdits>(
      gltfImportedLightEdits_?*gltfImportedLightEdits_:ImportedSceneLightEdits{});
  state->imported->capture(subs_.sceneManager->publishedAuthoredSceneLights(),state->lightEdits);
  if(!imported) {
    // Copy current per-binding light edits only for actual visual source
    // owners. Nonvisual mesh/joint contexts must not acquire visual lights.
    const auto& plan=state->assets.projectedClonePlan();
    const auto copyBindings=[&](auto& table) {
      std::remove_reference_t<decltype(table)> additions;
      for(const auto& [binding,value]:table) {
        const auto old=binding.sourceNode;
        if(old<0 || size_t(old)>=plan.clone.nodes.size() || plan.clone.nodes[size_t(old)]<0 ||
            plan.roles[size_t(old)]!=container::geometry::gltf::GltfCloneNodeRole::Visual) continue;
        auto target=binding; target.sourceNode=plan.clone.nodes[size_t(old)];
        additions.emplace(std::move(target),value);
      }
      table.merge(additions);
    };
    copyBindings(state->imported->points); copyBindings(state->imported->areas);
    copyBindings(state->imported->directionals);
  }
  state->sceneLightEdits=gltfSceneLightEdits_;
  const auto generationBytes=std::max(beforeBytes,state->assets.manager().mergeHistoryResidentBytes());
  auto reservation=addMergeBytes(generationBytes,generationBytes);
  reservation=addMergeBytes(reservation,graphHistoryBytes(state->assets.graph()));
  reservation=addMergeBytes(reservation,uint64_t(state->end)*sizeof(container::scene::SceneNodeLocalPose));
  reservation=addMergeBytes(reservation,std::max(decodedClipBytes(gltfAnimation_.get()),decodedClipBytes(state->animation.get())));
  if(reservation>SceneMergeBudget::limit-usedBytes)
    throw std::length_error("Scene merge would exceed the retained history CPU payload budget");
  state->budget=mergeBudget_; state->reservation=reservation;
  mergeBudget_->reserved+=reservation;
  std::erase_if(mergeReceipts_,[](const auto& receipt) { return receipt.expired(); });
  mergeReceipts_.push_back(state);
  // All closures/labels allocate before the first live publication.
  container::util::EditHistory::Command command{
      std::move(label),
      [this,state] { return replaySceneMerge(*state,false); },
      [this,state] { return replaySceneMerge(*state,true); },true};
  auto append=sceneGraph_.prepareAppend(state->assets.graph());
  if(!publishSceneMerge(*state,state->assets.graph(),&append,nullptr,true))
    return false;
  sceneEdits_->recordAppliedPublished(std::move(command));
  if(imported) lastCommittedMerge_=state;
  state->assets.releasePreparationData();
  return true;
}

bool RendererFrontend::replaySceneMerge(SceneMergeHistory& state,bool redo) {
  if(sceneGraph_.identity()!=state.graphIdentity || state.merged==redo ||
      sceneGraph_.nodeCount()!=(redo?state.first:state.end) ||
      state.graphPose.size()!=(redo?state.end:state.first)) return false;
#if CONTAINER_HAS_JOLT
  if(!redo && physicsScene_)
    for(uint32_t node=state.first;node<state.end;++node)
      if(physicsScene_->hasBody(node))
        throw std::invalid_argument("Detach physics bodies before undoing scene import");
  // A source/player snapshot cannot rewind a physical body using only a
  // visual transform. Refuse replay if an unrelated live body has moved.
  if(physicsScene_ && physicsScene_->graphIdentity()==sceneGraph_.identity())
    for(uint32_t node=0;node<std::min(sceneGraph_.nodeCount(),state.graphPose.size());++node)
      if(physicsScene_->hasBody(node) && sceneGraph_.getNode(node)->localTransform!=state.graphPose[node].localTransform)
        throw std::invalid_argument("Restore moved physics bodies before replaying scene addition history");
#endif
  auto tail=redo
      ? state.detached ? sceneGraph_.prepareRestoreTail(*state.detached,state.graphPose)
                       : container::scene::SceneGraph::PreparedTailChange{}
      : sceneGraph_.prepareDetachTail(state.first,state.undoRoots,state.graphPose);
  if(!tail) return false;
  auto candidate=*tail.candidateGraph();
  const bool published=publishSceneMerge(state,candidate,nullptr,&tail,redo);
  state.assets.releaseResourceDependencyPreparation();
  return published;
}

bool RendererFrontend::publishSceneMerge(SceneMergeHistory& state,
    container::scene::SceneGraph& graph,
    container::scene::SceneGraph::PreparedAppend* append,
    container::scene::SceneGraph::PreparedTailChange* tail,bool redo) {
  auto& manager=state.assets.manager();
  const auto behavior = [](const tinygltf::Model& source) -> const tinygltf::Value* {
    const auto found = source.extensions.find(container::geometry::gltf::kGltfInteractivityExtension);
    return found == source.extensions.end() ? nullptr : &found->second;
  };
  const auto* previousGraph = behavior(subs_.sceneManager->animationSource());
  const auto* nextGraph = behavior(manager.animationSource());
  const bool replaceBehavior = (previousGraph == nullptr) != (nextGraph == nullptr) ||
      (previousGraph && nextGraph && *previousGraph != *nextGraph);
  std::unique_ptr<container::geometry::gltf::GltfInteractivity> nextBehavior;
  if (replaceBehavior && nextGraph)
    nextBehavior = std::make_unique<container::geometry::gltf::GltfInteractivity>(manager.animationSource());
  std::vector<container::scene::SceneNodeLocalPose> oldPose;
  oldPose.reserve(sceneGraph_.nodeCount());
  for(uint32_t node=0;node<sceneGraph_.nodeCount();++node)
    oldPose.push_back({node,sceneGraph_.getNode(node)->localTransform});
  const auto imageCount=static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  const auto expectedRevision=sceneGraph_.revision();
  const auto expectedEpoch=subs_.sceneManager->generationEpoch();
  const auto futureGeometry=subs_.sceneManager->geometryRevision()+1;
  const auto futureMaterial=subs_.sceneManager->materialRevision()+1;
  if(expectedEpoch==UINT64_MAX || futureGeometry==0 || futureMaterial==0)
    throw std::overflow_error("Scene generation counters exhausted");
  if(imageCount==0 || imageCount!=buffers_.cameras.size() ||
      imageCount!=subs_.frameResourceManager->frameCount())
    throw std::runtime_error("Scene merge frame resources are not ready");
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire scene generation readers");
  manager.refreshActiveScene(graph);
  state.assets.prepareDescriptorFrames(imageCount);
  const auto dependencyUsed=addMergeBytes(mergeBudget_?mergeBudget_->reserved:0,subs_.sceneManager->mergeHistoryResidentBytes());
  if(dependencyUsed>=SceneMergeBudget::limit)
    throw std::length_error("Scene replay resource dependency preparation exceeds its CPU budget");
  state.assets.prepareResourceDependencies(*subs_.sceneManager,state.animation.get(),
      std::min<uint64_t>(512ull*1024*1024,SceneMergeBudget::limit-dependencyUsed));
  auto controller=subs_.sceneController->prepareGeneration(graph,manager,imageCount,
      subs_.guiManager && subs_.guiManager->showNormalDiagCube(),expectedRevision,expectedEpoch);
  auto oldLightEdits=subs_.lightingManager->snapshotSceneLightEdits();
  auto oldImported=std::make_shared<ImportedSceneLightEdits>(
      gltfImportedLightEdits_?*gltfImportedLightEdits_:ImportedSceneLightEdits{});
  oldImported->capture(subs_.sceneManager->publishedAuthoredSceneLights(),oldLightEdits);
  auto nextImported=std::make_shared<ImportedSceneLightEdits>(
      state.imported?*state.imported:ImportedSceneLightEdits{});
  auto lighting=subs_.lightingManager->prepareGeneration(manager,graph,controller.world(),
      sceneState_.rootNode,nextImported->merge(manager.publishedAuthoredSceneLights(),state.lightEdits),imageCount);
  manager.updateDescriptorSets(buffers_.cameras,controller.objectBuffers());
  if(subs_.bimManager && subs_.bimManager->hasScene())
    manager.updateAuxiliaryDescriptorSets(buffers_.cameras,subs_.bimManager->objectAllocatedBuffer());

  std::unique_ptr<GltfDeformationPass> deformation;
  const auto* deformed=controller.deformationScene();
  const auto topology=deformed?deformed->topologyRevision():0;
  const auto vertexCount=controller.gpuBindVertices().size();
  if(deformed && !deformed->gpuInstances().empty()) {
    deformation=std::make_unique<GltfDeformationPass>(svc_.ctx.deviceWrapper,svc_.allocationManager,svc_.pipelineManager);
    deformation->createResources(container::util::executableDirectory(),imageCount);
    deformation->setScene(controller.gpuBindVertices(),deformed->gpuInstances());
    for(uint32_t i=0;i<imageCount;++i) deformation->uploadFrame(i,deformed->gpuPoses());
  }
  std::unique_ptr<PbrAuxiliaryPass> auxiliary;
  auto* auxiliaryPass=subs_.pbrAuxiliaryPass.get();
  if(manager.hasExtendedPbrMaterials() && (!auxiliaryPass || !auxiliaryPass->ready())) {
    std::vector<VkImageView> depths; depths.reserve(imageCount);
    for(uint32_t i=0;i<imageCount;++i) {
      const auto* frame=subs_.frameResourceManager->frame(i);
      depths.push_back(msaaSampleCount_==VK_SAMPLE_COUNT_1_BIT?frame->depthStencil.view:frame->depthStencilMsaa.view);
    }
    auxiliary=std::make_unique<PbrAuxiliaryPass>(svc_.ctx.deviceWrapper,svc_.allocationManager,svc_.pipelineManager);
    auxiliary->createResources(container::util::executableDirectory(),manager.descriptorSetLayout(),
        resources_.gBufferFormats.depthStencil,msaaSampleCount_,depths,svc_.swapChainManager.extent());
    auxiliaryPass=auxiliary.get();
  }
  std::vector<VkImageView> anisotropy,diffuse;
  if(auxiliaryPass && auxiliaryPass->ready())
    for(uint32_t i=0;i<imageCount;++i) {
      anisotropy.push_back(auxiliaryPass->anisotropyView(i));
      diffuse.push_back(auxiliaryPass->diffuseTransmissionView(i));
    }
  auto auxiliaryBindings=subs_.frameResourceManager->prepareAuxiliaryBindings(anisotropy,diffuse);
  std::optional<OpenVdbVolumeRenderer::PreparedSceneBindings> volumeBindings;
  if(subs_.openVdbVolumeRenderer)
    volumeBindings=subs_.openVdbVolumeRenderer->prepareSceneBindings(lighting.lightingBuffers(),
        manager.sceneClipBuffer(),lighting.pointLightBuffer(),lighting.areaLightBuffer());

  std::optional<GpuCullManager::PreparedGeneration> culling;
  const auto cullDraws=controller.opaqueSingleSidedDrawCommands();
  if(subs_.gpuCullManager)
    culling=subs_.gpuCullManager->prepareGeneration(controller.objectBuffers(),
        controller.objectBufferCapacities(),buffers_.cameras,sizeof(container::gpu::CameraData),
        cullDraws,controller.objectDataRevision());
  std::optional<ShadowCullManager::PreparedGeneration> shadowCulling;
  if(subs_.shadowCullManager && subs_.shadowManager) {
    std::vector<VkBuffer> ubos;
    for(const auto& buffer:subs_.shadowManager->shadowCullUbos()) ubos.push_back(buffer.buffer);
    shadowCulling=subs_.shadowCullManager->prepareGeneration(controller.objectBuffers(),
        controller.objectBufferCapacities(),ubos,sizeof(container::gpu::ShadowCullData),
        cullDraws,shadowDrawRevision(cullDraws));
  }

  container::scene::MeshSceneProviderAssetCache assetCache;
  const auto bounds=mergeBounds(controller);
  const auto& meshAsset=assetCache.get({.primitiveRanges=manager.primitiveRanges(),
      .materialCount=manager.materialCount(),.instanceCount=controller.objectData().size(),.bounds=bounds,
      .materialFactsAt=[&manager](uint32_t material) {
        const auto facts=manager.materialRenderProperties(material);
        return container::scene::MeshSceneProviderMaterialFacts{facts.transparent,facts.doubleSided};
      },.bindings=MakeMeshSceneProviderBindingView(controller.objectData(),controller.opaqueDrawCommands(),
          controller.transparentDrawCommands(),uint64_t(controller.geometryIndices().size()))},
      {.geometry=futureGeometry,.materials=futureMaterial,.instances=controller.objectDataRevision()},subs_.sceneManager.get());
  MeshSceneProviderSyncInput preparedMesh{.available=meshAsset.primitiveCount>0 || meshAsset.bounds.valid,
      .primitiveCount=meshAsset.primitiveCount,.materialCount=meshAsset.materialCount,
      .instanceCount=meshAsset.instanceCount,.triangleBatches=meshAsset.triangleBatches,
      .bounds=meshAsset.bounds,.geometryRevision=controller.geometryRevision(),
      .materialRevision=futureMaterial,.instanceRevision=controller.objectDataRevision(),
      .displayName=activePrimaryModelPath_.empty()?"Primary mesh scene":activePrimaryModelPath_,
      .triangleBatchesAuthoritative=meshAsset.triangleBatchesAuthoritative};
  auto providerInput=buildSceneProviderSyncInput(&preparedMesh);
  SceneProviderSynchronizer providers;
  container::scene::SceneProviderRegistry registry;
  providers.sync(registry,providerInput);

  std::unique_ptr<RayShadowManager> ray;
  if(subs_.rayShadowManager) {
    ray=subs_.rayShadowManager->createDetachedGeneration();
    const bool giCompatible=pushConstants_.bindless.semanticColorMode==0;
    const bool gi=subs_.ddgiManager && subs_.ddgiManager->wantsScene() && giCompatible &&
        (!graph.renderableNodes().empty() || (subs_.bimManager && subs_.bimManager->hasScene()));
    ray->requestSurfaceScene(gi,subs_.ddgiManager?subs_.ddgiManager->settings().surfaceBudgetBytes:svc_.config.ddgi.surfaceBudgetBytes);
    std::vector<DrawCommand> meshDraws(controller.opaqueDrawCommands().begin(),controller.opaqueDrawCommands().end());
    if(gi) meshDraws.insert(meshDraws.end(),controller.transparentDrawCommands().begin(),controller.transparentDrawCommands().end());
    std::vector<RaySceneProviderSource> sources;
    // Static live views use SceneManager storage; its addresses survive the
    // asset exchange. Match those exact extraction keys before publication.
    const auto rayVertices=deformed?controller.geometryVertices():std::span<const container::geometry::Vertex>(manager.vertices());
    const auto rayIndices=deformed?controller.geometryIndices():std::span<const uint32_t>(manager.indices());
    const bool wantsRayScene=ray->supported() && (gi || ray->settings().mode!=RayShadowMode::Raster);
    if(providerInput.mesh.available && wantsRayScene)
      sources.push_back({{"primary-mesh-scene"},controller.geometryRevision(),rayVertices,
          rayIndices,controller.objectData(),meshDraws});
    std::vector<DrawCommand> bimDraws;
    if(subs_.bimManager && subs_.bimManager->hasScene() && wantsRayScene) {
      auto filter=currentBimDrawFilter(); filter.drawBudgetEnabled=false; filter.drawBudgetMaxObjects=0;
      const auto add=[&](const auto& draws) {
        for(const auto& draw:draws)
          if(subs_.bimManager->objectMatchesFilter(draw.objectIndex,filter) && bimObjectVisibleByLayer(draw.objectIndex))
            bimDraws.push_back(draw);
      };
      add(subs_.bimManager->opaqueDrawCommands());
      if(gi) add(subs_.bimManager->transparentDrawCommands());
      sources.push_back({{"auxiliary-bim-scene"},subs_.bimManager->geometryRevision(),subs_.bimManager->vertices(),
          subs_.bimManager->indices(),subs_.bimManager->objectData(),bimDraws});
    }
    bool compatible=true;
    for(const auto& source:sources) for(const auto& draw:source.draws)
      for(uint32_t instance=0;instance<draw.instanceCount;++instance) {
        const auto object=uint64_t(draw.objectIndex)+instance;
        if(object>=source.objects.size() || !manager.rayGeometryCompatible(source.objects[size_t(object)].objectInfo.x)) compatible=false;
      }
    const auto& clip=manager.sceneClipState();
    for(uint32_t i=0;i<imageCount;++i) {
      ray->update(i,buffers_.cameraData,*subs_.frameResourceManager->frame(i),manager.descriptorSet(i),
          lighting.lightDescriptorSet(i),sources,lighting.lightingData(),lighting.pointLightsSsbo(),lighting.areaLightsSsbo(),
          pushConstants_.bindless.sectionPlaneEnabled,pushConstants_.bindless.sectionPlane,clip,
          [&manager](uint32_t material) { return manager.temporalMaterialRevision(material); },compatible);
      svc_.allocationManager.executeStagedCommands([&](VkCommandBuffer command) { ray->recordBuild(command,i); });
      lighting.updateRayShadowDescriptors(i,ray->readSettings(i),ray->readSettingsSize(),ray->visibilityView());
    }
  }

  std::optional<DdgiManager::PreparedSceneHistoryReset> ddgiReset;
  std::optional<DdgiManager::PreparedLightSnapshotCapacity> ddgiLights;
  if(subs_.ddgiManager) {
    ddgiReset=subs_.ddgiManager->prepareSceneHistoryReset();
    ddgiLights=subs_.ddgiManager->prepareLightSnapshotCapacity(lighting.pointLightsSsbo().size(),lighting.areaLightsSsbo().size());
  }
  auto runtime=*subs_.frameRuntimeResourceRegistry;
  const auto bind=[&](RenderTechniqueId technique,uint32_t image) {
    runtime.bindDescriptorSet(technique,"scene-descriptor-set",image,{manager.descriptorSet(image)});
    runtime.bindDescriptorSet(technique,"light-descriptor-set",image,{lighting.lightDescriptorSet(image)});
    runtime.bindDescriptorSet(technique,"tiled-lighting-descriptor-set",image,{lighting.tiledDescriptorSet()});
    if(subs_.bimManager && subs_.bimManager->hasScene())
      runtime.bindDescriptorSet(technique,"bim-scene-descriptor-set",image,{manager.auxiliaryDescriptorSet(image)});
    runtime.bindBuffer(technique,"scene-object-buffer",image,
        {controller.objectBuffers()[image].buffer,sizeof(container::gpu::ObjectData)*controller.objectBufferCapacities()[image],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  };
  for(uint32_t i=0;i<imageCount;++i) {
    bind(RenderTechniqueId::DeferredRaster,i); bind(RenderTechniqueId::ForwardRaster,i);
    if(subs_.activeTechnique && subs_.activeTechnique->id()!=RenderTechniqueId::DeferredRaster &&
        subs_.activeTechnique->id()!=RenderTechniqueId::ForwardRaster) bind(subs_.activeTechnique->id(),i);
  }
  std::vector<container::gpu::AllocatedBuffer> objects(controller.objectBuffers().begin(),controller.objectBuffers().end());
  std::vector<size_t> capacities(controller.objectBufferCapacities().begin(),controller.objectBufferCapacities().end());
  std::vector<bool> shadowReady(imageCount,shadowCulling.has_value());
  std::string temporalReason=state.importedPublication
      ? (redo?"scene merge":"scene merge undo")
      : (redo?"scene subtree duplicate":"scene subtree duplicate undo");
  container::ui::GuiManager::MaterialVariantControls sceneControls,variantControls;
  container::ui::GltfAnimationControls animationControls;
  if(subs_.guiManager) {
    for(const auto& entry:manager.sceneCatalog().entries) sceneControls.names.push_back(entry.name);
    sceneControls.selected=static_cast<int>(manager.selectedScene());
    const auto names=manager.materialVariants().names();
    variantControls.names.assign(names.begin(),names.end());
    const auto selected=manager.selectedMaterialVariant();
    variantControls.selected=selected?static_cast<int>(*selected):-1;
    if(state.animation) {
      const auto& player=*state.animation;
      for(size_t i=0;i<player.library().clips.size();++i) {
        const auto& clip=player.library().clips[i];
        animationControls.clips.push_back({clip.name.empty()?"Clip "+std::to_string(i+1):clip.name,clip.duration});
      }
      for(const auto& diagnostic:player.library().diagnostics)
        animationControls.diagnostics.push_back("Clip "+std::to_string(diagnostic.clip+1)+", channel "+
            std::to_string(diagnostic.channel+1)+": "+diagnostic.message);
      animationControls.selectedClip=player.selectedClip()?static_cast<int>(*player.selectedClip()):-1;
      animationControls.time=player.time(); animationControls.speed=player.speed();
      animationControls.playing=player.playing(); animationControls.loop=player.loop();
    }
    setGltfCameraChoices(graph, manager.animationSource(), animationControls);
    animationControls.cameraNode=state.cameraNode?static_cast<int>(*state.cameraNode):-1;
  }
  // Progress callbacks can submit loading frames. Retire them before the
  // proof audit and keep every operation below allocation-free.
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire loading frames before scene publication");
  if(sceneGraph_.identity()!=state.graphIdentity || sceneGraph_.revision()!=expectedRevision ||
      subs_.sceneManager->generationEpoch()!=expectedEpoch ||
      (append && !state.assets.readyForExchange(*subs_.sceneManager,sceneGraph_,state.animation.get())) ||
      !state.assets.resourceDependenciesReadyForExchange(*subs_.sceneManager,state.animation.get()) ||
      !subs_.sceneController->canExchangeGeneration(controller,expectedEpoch) ||
      !subs_.lightingManager->canExchangeGeneration(lighting) ||
      (culling && !subs_.gpuCullManager->canExchangeGeneration(*culling)) ||
      (shadowCulling && !subs_.shadowCullManager->canExchangeGeneration(*shadowCulling)) ||
      (ray && !subs_.rayShadowManager->canExchangeGeneration(*ray)) ||
      !subs_.frameResourceManager->canPublishAuxiliaryBindings(auxiliaryBindings) ||
      (volumeBindings && !subs_.openVdbVolumeRenderer->canPublishSceneBindings(*volumeBindings)) ||
      (ddgiReset && !subs_.ddgiManager->canPublishSceneHistoryReset(*ddgiReset)) ||
      (ddgiLights && !subs_.ddgiManager->canPublishLightSnapshotCapacity(*ddgiLights)))
    throw std::runtime_error("Scene changed while preparing merge publication");
  // The graph is the only publication that can reject a stale topology proof.
  if(append ? !sceneGraph_.publishAppend(*append) : !sceneGraph_.publishTailChange(*tail)) return false;
  subs_.sceneManager->exchangeMergeGeneration(state.assets,state.animation.get());
  subs_.sceneController->exchangeGeneration(controller,sceneGraph_.revision(),futureGeometry,expectedEpoch+1);
  const bool lightsPublished=subs_.lightingManager->exchangeGeneration(lighting); assert(lightsPublished);
  if(culling) { const bool ok=subs_.gpuCullManager->exchangeGeneration(*culling); assert(ok); }
  if(shadowCulling) { const bool ok=subs_.shadowCullManager->exchangeGeneration(*shadowCulling); assert(ok); }
  if(ray) { const bool ok=subs_.rayShadowManager->exchangeGeneration(*ray); assert(ok); }
  if(ddgiLights) { const bool ok=subs_.ddgiManager->publishLightSnapshotCapacity(*ddgiLights); assert(ok); }
  if(ddgiReset) { const bool ok=subs_.ddgiManager->publishSceneHistoryReset(*ddgiReset); assert(ok); }
  if(volumeBindings) { const bool ok=subs_.openVdbVolumeRenderer->publishSceneBindings(*volumeBindings); assert(ok); }
  { const bool ok=subs_.frameResourceManager->publishAuxiliaryBindings(auxiliaryBindings); assert(ok); }
  if(auxiliary) subs_.pbrAuxiliaryPass.swap(auxiliary);
  subs_.gltfDeformationPass.swap(deformation);
  subs_.sceneProviderSynchronizer->exchangeState(providers,*subs_.sceneProviderRegistry,registry);
  subs_.frameRuntimeResourceRegistry->swapState(runtime);
  static_assert(std::is_nothrow_swappable_v<container::scene::MeshSceneProviderAssetCache>);
  std::swap(meshProviderAssetCache_,assetCache);
  gltfAnimation_.swap(state.animation);
  if (replaceBehavior) {
    gltfInteractivity_.swap(nextBehavior);
    gltfInteractivityError_.clear();
    gltfInteractivityRequest_ = {};
  }
  gltfInteractivityHoverChain_.clear();
  state.graphPose.swap(oldPose);
  std::swap(gltfAnimationCameraNode_,state.cameraNode);
  gltfAnimationCamera_.reset();
  gltfAnimationGraphIdentity_=sceneGraph_.identity(); pendingGltfAnimationDeltaSeconds_=0;
  static_assert(std::is_nothrow_swappable_v<SceneLightEdits>);
  std::swap(state.lightEdits,oldLightEdits);
  gltfImportedLightEdits_.swap(nextImported);
  state.imported.swap(oldImported);
  gltfSceneLightEdits_.swap(state.sceneLightEdits);
  if(subs_.guiManager) {
    static_assert(std::is_nothrow_swappable_v<decltype(sceneControls)>);
    static_assert(std::is_nothrow_swappable_v<decltype(animationControls)>);
    std::swap(subs_.guiManager->gltfSceneControls(),sceneControls);
    std::swap(subs_.guiManager->materialVariantControls(),variantControls);
    std::swap(subs_.guiManager->gltfAnimationControls(),animationControls);
  }
  gltfSceneGraphIdentity_=sceneGraph_.identity(); materialVariantGraphIdentity_=sceneGraph_.identity();
  buffers_.objects.swap(objects); buffers_.objectCapacities.swap(capacities);
  buffers_.shadowObjectDescriptorReady.swap(shadowReady);
  syncSceneStateFromController(); sceneState_.indexType=subs_.sceneManager->indexType();
  restoreSceneEditSelection(redo?state.after:state.before);
  selectionNavigationAnchor_={}; clearHoveredMeshNode();
  selectedDrawCommands_.clear(); selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear(); selectedBimNativeCurveDrawCommands_.clear();
  transformDragSession_.active=false; captureObjectBase_.reset();
  sceneEditPublicationPending_=false;
  gltfDeformationGraphIdentity_=subs_.gltfDeformationPass?sceneGraph_.identity():0;
  gltfDeformationSourceRevision_=futureGeometry; gltfDeformationTopologyRevision_=topology;
  gltfDeformationVertexCount_=vertexCount; gltfDeformationImageCount_=imageCount;
  gltfDeformationSubmittedEpoch_.reset(); gltfDeformationHistoryReset_=true;
  gltfDeformationMaxPositionDelta_=0;
  ddgiSceneChanges_.clear(); ddgiTrackedSceneRevision_=0;
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(temporalReason));
  if(!redo && tail) state.detached=tail->takeDetachedTail();
  state.merged=redo;
  if(state.importedPublication) ++mergeRequest_.publications;
  // Initial frontend buffers have been copied into controller-owned snapshots.
  // Inactive scene sets are rebound before replay; they never submit these old
  // aliases. Controller-owned aliases are released by the retired controller.
  for(auto& object:objects)
    if(object.buffer && !subs_.sceneController->ownsObjectBuffer(object.buffer) &&
        std::ranges::none_of(controller.objectBuffers(),[&](const auto& owned) { return owned.buffer==object.buffer; }))
      svc_.allocationManager.memoryManager()->destroyBuffer(object);
  return true;
}

void RendererFrontend::appendGltfMergeTelemetry(nlohmann::json& json) const {
  const char* outcome="idle";
  switch(mergeRequest_.outcome) {
    case MergeRequestOutcome::Idle: break;
    case MergeRequestOutcome::Pending: outcome="pending"; break;
    case MergeRequestOutcome::Committed: outcome="committed"; break;
    case MergeRequestOutcome::Failed: outcome="failed"; break;
  }
  auto& view=json["gltfMerge"];
  view={{"requestId",mergeRequest_.requestId},{"outcome",outcome},{"error",mergeRequest_.error},
      {"parentName",mergeRequest_.parentName},{"sourceReadCount",mergeRequest_.sourceReadCount},
      {"generationPublications",mergeRequest_.publications},
      {"commitGeneration",subs_.sceneManager?subs_.sceneManager->generationEpoch():0},
      {"descriptorGeneration",subs_.sceneManager?subs_.sceneManager->generationEpoch():0},
      {"retainedGenerationCount",std::ranges::count_if(mergeReceipts_,[](const auto& value) { return !value.expired(); })},
      {"retainedHistoryLogicalCpuBytes",mergeBudget_?mergeBudget_->reserved:0},
      {"historyLogicalCpuBudgetBytes",SceneMergeBudget::limit},
      {"retainedNodes",nlohmann::json::array()},{"authoringNodes",nlohmann::json::array()},
      {"objectBuffers",nlohmann::json::array()}};
  view["graphIdentity"] = sceneGraph_.identity();
  view["graphNodeCount"] = sceneGraph_.nodeCount();
  view["activeRoots"] = sceneGraph_.activeRoots()
      ? nlohmann::json(*sceneGraph_.activeRoots()) : nlohmann::json(nullptr);
  if(const auto* memory=svc_.allocationManager.memoryManager()) {
    VmaTotalStatistics statistics{};
    vmaCalculateStatistics(memory->allocator(),&statistics);
    view["vmaAllocationCount"]=statistics.total.statistics.allocationCount;
    view["vmaAllocationBytes"]=statistics.total.statistics.allocationBytes;
  }
  if(subs_.sceneManager) {
    const auto& source=subs_.sceneManager->animationSource();
    view["sourceTables"]={{"nodes",source.nodes.size()},{"meshes",source.meshes.size()},
        {"materials",source.materials.size()},{"images",source.images.size()},
        {"textures",source.textures.size()},{"samplers",source.samplers.size()},
        {"buffers",source.buffers.size()},{"scenes",source.scenes.size()}};
    view["importScale"]=subs_.sceneManager->importScale();
  }
  const auto last=lastCommittedMerge_.lock();
  const auto retainedCount=last?std::min<size_t>(last->first,sceneGraph_.nodeCount()):sceneGraph_.nodeCount();
  view["retainedNodesTruncated"]=retainedCount>4096;
  view["authoringNodesTruncated"]=sceneGraph_.nodeCount()>4096;
  for(uint32_t i=0;i<std::min<size_t>(sceneGraph_.nodeCount(),4096);++i) {
    const auto* node=sceneGraph_.getNode(i);
    auto columns=nlohmann::json::array();
    auto localColumns=nlohmann::json::array();
    for(size_t c=0;c<4;++c) columns.push_back({node->worldTransform[c][0],node->worldTransform[c][1],node->worldTransform[c][2],node->worldTransform[c][3]});
    for(size_t c=0;c<4;++c) localColumns.push_back({node->localTransform[c][0],node->localTransform[c][1],node->localTransform[c][2],node->localTransform[c][3]});
    nlohmann::json row={{"graphNode",i},{"sourceNode",node->sourceNodeIndex},
        {"name",node->name},{"parentGraphNode",node->parent},
        {"meshOwnerGraphNode",node->meshOwnerGraphNode},
        {"renderable",node->renderable},{"retired",node->retired},{"active",sceneGraph_.isNodeActive(i)},
        {"primitiveIndex",node->primitiveIndex},{"materialIndex",node->materialIndex},
        {"sourceMesh",node->sourceMeshIndex},{"sourcePrimitive",node->sourcePrimitiveIndex},
        {"sourceInstance",node->sourceInstanceIndex},{"localColumns",std::move(localColumns)},
        {"worldColumns",std::move(columns)}};
    view["authoringNodes"].push_back(row);
    if(i<retainedCount) view["retainedNodes"].push_back(std::move(row));
  }
  for(const auto& buffer:buffers_.objects)
    view["objectBuffers"].push_back({{"handle",uint64_t(reinterpret_cast<uintptr_t>(buffer.buffer))},
        {"controllerOwned",subs_.sceneController && subs_.sceneController->ownsObjectBuffer(buffer.buffer)}});
}
} // namespace container::renderer
