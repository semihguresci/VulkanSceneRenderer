#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/lighting/ImportedSceneLightEdits.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/renderer/platform/VulkanContext.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SceneNodeAuthoringPolicy.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/geometry/GltfSceneExport.h"
#include "Container/geometry/GltfNodeExportCompaction.h"
#include "Container/renderer/scene/SceneLightExport.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsElementBindings.h"
#include "Container/physics/PhysicsScene.h"
#endif

#include <stdexcept>
#include <exception>
#include <utility>

namespace container::renderer {

void RendererFrontend::captureImportedLightEdits() {
  if(!subs_.sceneManager || !subs_.lightingManager) return;
  if(!gltfImportedLightEdits_) gltfImportedLightEdits_=std::make_shared<ImportedSceneLightEdits>();
  gltfImportedLightEdits_->capture(subs_.sceneManager->publishedAuthoredSceneLights(),
      subs_.lightingManager->snapshotSceneLightEdits());
}

void RendererFrontend::selectGltfScene(size_t index) {
  finishEditGestures();
  if (pendingSceneRetirement_) publishSceneEdit();
  if(!subs_.sceneManager) throw std::runtime_error("No primary scene is loaded");
  if(index==subs_.sceneManager->selectedScene()) return;
  const auto activation=subs_.sceneManager->planSceneSelection(sceneGraph_,index);
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire scene readers before switching scenes");
  const auto previous=subs_.sceneManager->selectedScene();
  if(subs_.lightingManager) {
    gltfSceneLightEdits_[previous]=std::make_shared<SceneLightEdits>(subs_.lightingManager->snapshotSceneLightEdits());
    if(!gltfImportedLightEdits_) gltfImportedLightEdits_=std::make_shared<ImportedSceneLightEdits>();
    gltfImportedLightEdits_->capture(subs_.sceneManager->publishedAuthoredSceneLights(),
        *gltfSceneLightEdits_[previous]);
  }
  subs_.sceneManager->applySceneSelection(sceneGraph_,activation);
  if(subs_.lightingManager) {
    const auto found=gltfSceneLightEdits_.find(index);
    const auto edits=found==gltfSceneLightEdits_.end()?SceneLightEdits{}:*found->second;
    subs_.lightingManager->restoreSceneLightEdits(gltfImportedLightEdits_->merge(
        subs_.sceneManager->evaluateAuthoredSceneLights(sceneGraph_,index),edits));
  }
  if(gltfAnimation_) gltfAnimation_->pause();
  pauseGltfInteractivity();
  gltfAnimationCameraNode_.reset(); gltfAnimationCamera_.reset();
  pendingGltfAnimationDeltaSeconds_=0;
  if(sceneEdits_) sceneEdits_->clear();
  captureSceneHistory_.clear();
  captureObjectBase_.reset(); captureObjectNode_=container::scene::SceneGraph::kInvalidNode;
  transformDragSession_.active=false;
  clearSelectedMeshNode(); clearHoveredMeshNode();
  selectionNavigationAnchor_={};
#if CONTAINER_HAS_JOLT
  if(physicsScene_) physicsScene_->setPlaying(false);
#endif
  pendingPhysicsActions_.clear(); pendingPhysicsDeltaSeconds_=0;
  physicsJointFirstNode_.reset(); physicsJointFirstBody_=0; physicsJointFirstGraph_=0;
  physicsJointFirstIsWorld_=false; physicsSelectedJoint_=0;
  if(subs_.guiManager) {
    auto& animation=subs_.guiManager->gltfAnimationControls();
    animation.requestedClip.reset(); animation.requestedCameraNode.reset();
    animation.requestedTime.reset(); animation.requestedPlaying.reset();
    animation.requestedSpeed.reset(); animation.requestedLoop.reset();
    animation.playing=false; animation.cameraNode=-1;
    (void)subs_.guiManager->consumeEditHistoryRequest();
    subs_.guiManager->setStatusMessage("Selected glTF scene "+std::to_string(index));
  }
  refreshGltfCameraChoices();
  updateGltfDeformation();
  publishSceneEdit();
  resetCameraForActiveScene();
  if(subs_.temporalManager) subs_.temporalManager->reset("glTF scene changed");
}

void RendererFrontend::updateGltfSceneState() {
  if(!subs_.sceneManager) return;
  if(gltfSceneGraphIdentity_!=sceneGraph_.identity()) {
    gltfSceneGraphIdentity_=sceneGraph_.identity();
    gltfSceneLightEdits_.clear(); gltfImportedLightEdits_.reset();
    if(subs_.guiManager) {
      auto& state=subs_.guiManager->gltfSceneControls(); state={};
      for(const auto& entry:subs_.sceneManager->sceneCatalog().entries) state.names.push_back(entry.name);
    }
  }
  if(const auto requested=std::exchange(captureGltfScene_,{})) selectGltfScene(size_t(*requested));
  if(subs_.guiManager) subs_.guiManager->gltfSceneControls().selected=int(subs_.sceneManager->selectedScene());
}

void RendererFrontend::processGltfSceneRequest() {
  if(!subs_.guiManager || !subs_.sceneManager) return;
  auto& state=subs_.guiManager->gltfSceneControls();
  if(const auto requested=std::exchange(state.requested,{})) {
    try { selectGltfScene(size_t(*requested)); state.error.clear(); }
    catch(const std::exception& error) { state.error=error.what(); }
  }
  state.selected=int(subs_.sceneManager->selectedScene());
}

bool RendererFrontend::selectMaterialVariant(std::optional<size_t> variant,bool recordHistory) {
  finishEditGestures();
  if (pendingSceneRetirement_) publishSceneEdit();
  if (!subs_.sceneManager) throw std::runtime_error("No primary scene is loaded");
  auto edit=std::make_shared<container::scene::MaterialVariantGraphEdit>(subs_.sceneManager->planMaterialVariant(sceneGraph_,variant));
  if (!edit->changes()) return false;
  ensureSceneEditHistory();
  if (!subs_.sceneManager->applyMaterialVariant(sceneGraph_,*edit)) return false;
  if (recordHistory) {
    sceneEdits_->finishGesture();
    sceneEdits_->recordApplied({"Material variant",
      [this,edit] { return subs_.sceneManager && subs_.sceneManager->applyMaterialVariant(sceneGraph_,*edit,false); },
      [this,edit] { return subs_.sceneManager && subs_.sceneManager->applyMaterialVariant(sceneGraph_,*edit,true); }});
  } else sceneEditPublicationPending_=true;
  return true;
}

void RendererFrontend::updateMaterialVariantState() {
  if (!subs_.sceneManager) return;
  if (materialVariantGraphIdentity_!=sceneGraph_.identity()) {
    materialVariantGraphIdentity_=sceneGraph_.identity();
    if (subs_.guiManager) {
      auto& state=subs_.guiManager->materialVariantControls(); state={};
      const auto names=subs_.sceneManager->materialVariants().names(); state.names.assign(names.begin(),names.end());
    }
    if (svc_.config.gltfMaterialVariant>=0)
      selectMaterialVariant(size_t(svc_.config.gltfMaterialVariant),false);
  }
  if (const auto requested=std::exchange(captureMaterialVariant_,{}))
    selectMaterialVariant(*requested<0?std::nullopt:std::optional<size_t>(size_t(*requested)));
  if (!captureSceneHistory_.empty()) {
    finishEditGestures();
    ensureSceneEditHistory(); sceneEdits_->finishGesture();
    const auto action=std::exchange(captureSceneHistory_,{});
    if (!(action=="undo"?sceneEdits_->undo():sceneEdits_->redo()))
      throw std::runtime_error("Capture material variant history action could not be applied");
  }
  if (subs_.guiManager) {
    const auto selected=subs_.sceneManager->selectedMaterialVariant();
    subs_.guiManager->materialVariantControls().selected=selected?int(*selected):-1;
  }
  if (sceneEditPublicationPending_) publishSceneEdit();
}

void RendererFrontend::processMaterialVariantRequest() {
  if (!subs_.guiManager || !subs_.sceneManager) return;
  auto& state=subs_.guiManager->materialVariantControls();
  if (const auto request=std::exchange(state.requested,{})) {
    try {
      selectMaterialVariant(*request<0?std::nullopt:std::optional<size_t>(size_t(*request)));
      state.error.clear();
    } catch (const std::exception& error) { state.error=error.what(); }
  }
  const auto selected=subs_.sceneManager->selectedMaterialVariant(); state.selected=selected?int(*selected):-1;
}

void RendererFrontend::saveSceneModel(const std::filesystem::path& outputPath) {
  finishEditGestures();
  if (!subs_.sceneManager) throw std::runtime_error("No primary scene is loaded");
  auto snapshot = subs_.sceneManager->buildSceneExport(sceneGraph_);
  if (subs_.lightingManager) {
    std::vector<int32_t> sourceNodes;
    sourceNodes.reserve(sceneGraph_.nodeCount());
    for (uint32_t i = 0; i < sceneGraph_.nodeCount(); ++i)
      sourceNodes.push_back(sceneGraph_.getNode(i)->sourceNodeIndex);
    sourceNodes.resize(snapshot.nodes.size(), -1);
    std::vector<std::vector<EditableLightEntity>> sceneLights(snapshot.scenes.size());
    std::vector<container::scene::AuthoredSceneLights> sceneBindings(snapshot.scenes.size());
    std::vector<container::scene::AuthoredSceneLights> visibleSceneBindings(snapshot.scenes.size());
    const auto active=subs_.sceneManager->selectedScene();
    const auto current=subs_.lightingManager->snapshotSceneLightEdits();
    auto imported=gltfImportedLightEdits_?*gltfImportedLightEdits_:ImportedSceneLightEdits{};
    imported.capture(subs_.sceneManager->publishedAuthoredSceneLights(),current);
    const auto* root=sceneGraph_.getNode(sceneState_.rootNode);
    const auto outerTransform=root?root->worldTransform:glm::mat4(1);
    for(size_t index=0;index<sceneLights.size();++index) {
      sceneBindings[index]=subs_.sceneManager->evaluateAuthoredSceneLights(sceneGraph_,index,true);
      visibleSceneBindings[index]=subs_.sceneManager->evaluateAuthoredSceneLights(sceneGraph_,index);
      const auto cached=gltfSceneLightEdits_.find(index);
      const auto edits=index==active?current:cached==gltfSceneLightEdits_.end()?SceneLightEdits{}:*cached->second;
      sceneLights[index]=evaluateEditableSceneLightsForExport(sceneBindings[index],
          visibleSceneBindings[index],edits,imported,outerTransform);
    }
    exportEditableSceneLightsForScenes(snapshot,sceneLights,
        &subs_.sceneManager->animationSource(),sourceNodes,subs_.sceneManager->importScale(),
        sceneBindings,visibleSceneBindings);
  }
  const auto retired = subs_.sceneManager->retiredSceneExportNodes(sceneGraph_);
  if (!retired.empty())
    snapshot = container::geometry::gltf::CompactGltfExportNodes(snapshot, retired).model;
  subs_.sceneManager->compactRetiredMaterialExport(snapshot);
  container::geometry::gltf::writeExportModel(snapshot, outputPath);
}

void RendererFrontend::ensureSceneEditHistory() {
  if (!sceneEdits_) {
    sceneEdits_ = std::make_unique<container::util::SceneEditHistory>(
        sceneGraph_,
        container::util::SceneEditHistory::Callbacks{
            .beforeTransform =
                [this](uint32_t node, const glm::mat4 &local) {
                  validateSceneNodeTransform(node, local);
                },
            .beforeReparent =
                [this](uint32_t node, std::optional<uint32_t> parent,
                       const glm::mat4 &local) {
                  validateSceneNodeReparent(node, parent, local);
                },
            .onChanged = [this] { sceneEditPublicationPending_ = true; },
            .prepareReparent = [this](uint32_t node,
                const container::util::SceneEditHistory::Placement& before,
                const container::util::SceneEditHistory::Placement& after) {
              return prepareSceneReparent(node,before,after);
            }});
  }
  sceneEdits_->sync();
}

void RendererFrontend::processSceneNodeAuthoringRequest() {
  if (!subs_.guiManager) return;
  const auto request = subs_.guiManager->consumeSceneNodeAuthoringRequest();
  if (!request) return;
  try {
    if (authorSceneNode(*request))
      subs_.guiManager->setStatusMessage(
          request->action == container::ui::SceneNodeAuthoringAction::Rename
              ? "Renamed scene node"
              : request->action == container::ui::SceneNodeAuthoringAction::CreateEmpty
                  ? "Added empty scene node"
                  : request->action == container::ui::SceneNodeAuthoringAction::DuplicateSubtree
                      ? "Duplicated scene subtree" : "Deleted scene subtree");
  } catch (const std::exception& error) {
    subs_.guiManager->setStatusMessage("Cannot edit scene node: " + std::string(error.what()));
  }
}

bool RendererFrontend::authorSceneNode(const container::ui::SceneNodeAuthoringRequest& request) {
  using container::scene::SceneGraph;
  using container::scene::SceneManager;
  using container::ui::SceneNodeAuthoringAction;
  if (request.graphIdentity != sceneGraph_.identity())
    throw std::invalid_argument("The requested node belongs to a replaced scene");
  if (request.action != SceneNodeAuthoringAction::Rename &&
      request.action != SceneNodeAuthoringAction::CreateEmpty &&
      request.action != SceneNodeAuthoringAction::DuplicateSubtree &&
      request.action != SceneNodeAuthoringAction::DeleteSubtree)
    throw std::invalid_argument("Unknown scene node authoring action");
  finishEditGestures();
  if (request.action == SceneNodeAuthoringAction::DeleteSubtree)
    return retireSceneNode(request.node);
  if (request.action == SceneNodeAuthoringAction::DuplicateSubtree)
    return duplicateSceneNode(request.node,request.name.empty()
        ? std::nullopt : std::optional<std::string>(request.name));
  container::scene::ValidateSceneNodeName(request.name);
  const auto purpose = request.action == SceneNodeAuthoringAction::Rename
      ? container::scene::SceneNodeAuthoringPurpose::Rename
      : container::scene::SceneNodeAuthoringPurpose::CreateEmptyChild;
  const auto target = container::scene::ResolveSceneNodeAuthoringTarget(
      sceneGraph_, request.node, purpose);
  ensureSceneEditHistory();
  transformDragSession_.active = false;
  sceneEdits_->finishGesture();
  if (request.action == SceneNodeAuthoringAction::Rename)
    return sceneEdits_->setName(target, request.name);
  if (!subs_.sceneManager)
    throw std::invalid_argument("Empty node creation requires an initialized scene");
#if CONTAINER_HAS_JOLT
  if (physicsElements_ && physicsElements_->ownsNode(sceneGraph_, target))
    throw std::invalid_argument("Physics product internals cannot receive editor children");
#endif
  const auto first = static_cast<uint32_t>(sceneGraph_.nodeCount());
  auto candidate = sceneGraph_.makeEmptyNodeCandidate(target, request.name);
  auto membership = subs_.sceneManager->prepareEditorSceneRootRegistration(
      sceneGraph_, candidate, first);
  auto append = sceneGraph_.prepareAppend(std::move(candidate));
  struct Addition {
    uint32_t first, end;
    std::optional<std::vector<uint32_t>> undoRoots;
    SceneManager::EditorSceneRootMembership membership;
    std::optional<SceneGraph::DetachedTail> detached;
  };
  const auto state = std::make_shared<Addition>(Addition{first, first + 1,
      sceneGraph_.activeRoots(), membership.membership(), {}});
  const auto before = sceneEditSelection();
  auto after = before;
  after.mesh = first;
  after.bim = std::numeric_limits<uint32_t>::max();
  after.light = {};
  after.sectionEditable = false;
  container::util::EditHistory::Command command{
      "Add empty node",
      [this, state, before] {
        if (sceneGraph_.nodeCount() != state->end || !subs_.sceneManager)
          return false;
#if CONTAINER_HAS_JOLT
        if (physicsScene_ && physicsScene_->hasBody(state->first))
          throw std::invalid_argument("Detach physics bodies before undoing empty node creation");
#endif
        auto tail = sceneGraph_.prepareDetachTail(state->first, state->undoRoots);
        if (!tail) return false;
        auto roots = subs_.sceneManager->prepareEditorSceneRootChange(
            sceneGraph_, state->membership, false);
        if (!subs_.sceneManager->canPublishEditorSceneRootChange(roots) ||
            !sceneGraph_.publishTailChange(tail)) return false;
        if (!subs_.sceneManager->publishEditorSceneRootChange(roots)) std::terminate();
        state->detached = tail.takeDetachedTail();
        restoreSceneEditSelection(before);
        return true;
      },
      [this, state, after] {
        if (!state->detached || !subs_.sceneManager) return false;
        auto tail = sceneGraph_.prepareRestoreTail(*state->detached);
        if (!tail) return false;
        auto roots = subs_.sceneManager->prepareEditorSceneRootChange(
            sceneGraph_, state->membership, true);
        if (!subs_.sceneManager->canPublishEditorSceneRootChange(roots) ||
            !sceneGraph_.publishTailChange(tail)) return false;
        if (!subs_.sceneManager->publishEditorSceneRootChange(roots)) std::terminate();
        restoreSceneEditSelection(after);
        return true;
      }};
  if (!append || !subs_.sceneManager->canPublishEditorSceneRootChange(membership) ||
      !sceneGraph_.publishAppend(append)) return false;
  if (!subs_.sceneManager->publishEditorSceneRootChange(membership)) std::terminate();
  restoreSceneEditSelection(after);
  sceneEdits_->recordApplied(std::move(command));
  return true;
}

uint32_t RendererFrontend::addScenePrimitive(ScenePrimitiveKind kind,std::string name,
    std::optional<uint32_t> parent) {
  finishEditGestures();
  using container::scene::SceneGraph;
  using container::scene::SceneManager;
  if(!subs_.sceneController || !subs_.sceneManager || !sceneGraph_.getNode(sceneState_.rootNode) ||
      subs_.sceneManager->sceneCatalog().entries.empty())
    throw std::invalid_argument("Primitive creation requires an initialized primary scene");
  if(!name.empty()) container::scene::ValidateSceneNodeName(name);
  const auto target=container::scene::ResolveSceneNodeAuthoringTarget(sceneGraph_,
      parent.value_or(sceneState_.rootNode),container::scene::SceneNodeAuthoringPurpose::CreateEmptyChild);
#if CONTAINER_HAS_JOLT
  if(physicsElements_ && physicsElements_->ownsNode(sceneGraph_,target))
    throw std::invalid_argument("Physics product internals cannot receive editor primitives");
#endif
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire readers before primitive creation");
  ensureSceneEditHistory(); sceneEdits_->finishGesture();
  const auto before=sceneEditSelection();
  const auto first=static_cast<uint32_t>(sceneGraph_.nodeCount());
  const auto primitive=subs_.sceneController->appendScenePrimitiveGeometry(kind);
  if(primitive==SceneGraph::kInvalidNode) return primitive;
  container::scene::SceneNode leaf;
  leaf.parent=target; leaf.renderable=true; leaf.primitiveIndex=primitive;
  leaf.materialIndex=subs_.sceneManager->defaultMaterialIndex();
  leaf.name=name.empty()?std::string(scenePrimitiveKindLabel(kind)):std::move(name);
  auto candidate=sceneGraph_.makeAppendCandidate({std::move(leaf)});
  if(sceneGraph_.activeRoots() && !candidate.isNodeActive(first)) {
    auto roots=*sceneGraph_.activeRoots(); roots.push_back(first);
    candidate.setActiveRoots(roots);
  }
  auto membership=subs_.sceneManager->prepareEditorPrimitiveSceneRootRegistration(sceneGraph_,candidate,first);
  auto append=sceneGraph_.prepareAppend(std::move(candidate));
  struct Addition {
    uint32_t first,end;
    std::optional<std::vector<uint32_t>> undoRoots;
    SceneManager::EditorSceneRootMembership membership;
    std::optional<SceneGraph::DetachedTail> detached;
  };
  const auto state=std::make_shared<Addition>(Addition{
      first,first+1,sceneGraph_.activeRoots(),membership.membership(),{}});
  auto after=before; after.mesh=first; after.bim=SceneGraph::kInvalidNode;
  after.light={}; after.sectionEditable=false;
  container::util::EditHistory::Command command{
      "Add primitive",
      [this,state,before] {
        if(!subs_.sceneManager || sceneGraph_.nodeCount()!=state->end) return false;
#if CONTAINER_HAS_JOLT
        if(physicsScene_ && physicsScene_->hasBody(state->first))
          throw std::invalid_argument("Detach physics bodies before undoing primitive creation");
#endif
        auto tail=sceneGraph_.prepareDetachTail(state->first,state->undoRoots);
        if(!tail) return false;
        auto roots=subs_.sceneManager->prepareEditorSceneRootChange(sceneGraph_,state->membership,false);
        if(!subs_.sceneManager->canPublishEditorSceneRootChange(roots) || !sceneGraph_.publishTailChange(tail))
          return false;
        if(!subs_.sceneManager->publishEditorSceneRootChange(roots)) std::terminate();
        state->detached=tail.takeDetachedTail();
        restoreSceneEditSelection(before);
        return true;
      },
      [this,state,after] {
        if(!subs_.sceneManager || !state->detached) return false;
        auto tail=sceneGraph_.prepareRestoreTail(*state->detached);
        if(!tail) return false;
        auto roots=subs_.sceneManager->prepareEditorSceneRootChange(sceneGraph_,state->membership,true);
        if(!subs_.sceneManager->canPublishEditorSceneRootChange(roots) || !sceneGraph_.publishTailChange(tail))
          return false;
        if(!subs_.sceneManager->publishEditorSceneRootChange(roots)) std::terminate();
        restoreSceneEditSelection(after);
        return true;
      }};
  if(!append || !subs_.sceneManager->canPublishEditorSceneRootChange(membership) ||
      !sceneGraph_.publishAppend(append)) return SceneGraph::kInvalidNode;
  if(!subs_.sceneManager->publishEditorSceneRootChange(membership)) std::terminate();
  restoreSceneEditSelection(after);
  // Commit history before the ordinary geometry/provider refresh, so a refresh
  // failure retains a reversible graph/membership edit.
  sceneEdits_->recordApplied(std::move(command));
  subs_.sceneController->createGeometryBuffers();
  syncSceneStateFromController();
  restoreSceneEditSelection(after);
  selectionNavigationAnchor_={}; clearHoveredMeshNode();
  selectedDrawCommands_.clear(); selectedBimDrawCommands_.clear();
  refreshSceneObjectData(); syncSceneProviders();
  if(subs_.guiManager) subs_.guiManager->setStatusMessage("Added "+std::string(scenePrimitiveKindLabel(kind)));
  return first;
}

bool RendererFrontend::reparentSceneNode(uint32_t node,std::optional<uint32_t> parent) {
  finishEditGestures();
  if(sceneEditPublicationPending_ || pendingSceneRetirement_) publishSceneEdit();
  ensureSceneEditHistory(); sceneEdits_->finishGesture();
  return sceneEdits_->reparent(node,parent);
}

container::util::SceneEditHistory::ReparentActions RendererFrontend::prepareSceneReparent(
    uint32_t node,const container::util::SceneEditHistory::Placement& before,
    const container::util::SceneEditHistory::Placement& after) {
  using container::util::SceneEditHistory;
  using container::scene::SceneManager;
  if(!subs_.sceneManager || subs_.sceneManager->sceneCatalog().entries.empty()) return {};
  auto preview=sceneGraph_.preparePlacement(node,after.parent,after.local,after.siblingIndex,
      sceneGraph_.activeRoots());
  if(!preview) throw std::invalid_argument("Cannot stage the requested scene node placement");
  struct Change {
    uint32_t node;
    SceneEditHistory::Placement before,after;
    SceneManager::ReparentSceneRootMembership membership;
  };
  const auto state=std::make_shared<Change>(Change{node,before,after,
      subs_.sceneManager->planReparentSceneRoots(sceneGraph_,*preview.candidateGraph(),node)});
  const auto apply=[this,state](bool redo) {
    if(!subs_.sceneManager) return false;
    const auto& placement=redo?state->after:state->before;
    auto graph=sceneGraph_.preparePlacement(state->node,placement.parent,placement.local,
        placement.siblingIndex,state->membership.activeRoots(redo));
    if(!graph) return false;
    auto roots=subs_.sceneManager->prepareReparentSceneRootChange(sceneGraph_,state->membership,redo);
    if(!subs_.sceneManager->canPublishReparentSceneRootChange(roots) ||
        !sceneGraph_.publishPlacement(graph)) return false;
    if(!subs_.sceneManager->publishReparentSceneRootChange(roots)) std::terminate();
    return true;
  };
  return {[apply] { return apply(false); },[apply] { return apply(true); }};
}

void RendererFrontend::validateSceneNodeReparent(uint32_t node,
                                                 std::optional<uint32_t> parent,
                                                 const glm::mat4 &local) {
  if (node == sceneState_.rootNode)
    throw std::invalid_argument("The scene root cannot be reparented");
  if(subs_.sceneManager && subs_.sceneManager->sceneCatalog().entries.size()>1) {
    if(subs_.sceneManager->isAuthoredSceneRoot(sceneGraph_,node))
      throw std::invalid_argument("Shared glTF scene roots retain their global parent; edit their transform or children");
    if(parent && *parent!=sceneState_.rootNode && !sceneGraph_.isNodeActive(*parent))
      throw std::invalid_argument("Choose a parent in the active glTF scene");
  }
#if CONTAINER_HAS_JOLT
  if (physicsElements_ &&
      (physicsElements_->ownsNode(sceneGraph_, node) ||
       (parent && physicsElements_->ownsNode(sceneGraph_, *parent))))
    throw std::invalid_argument(
        "Physics product entries keep their internal parent");
  if (physicsScene_)
    physicsScene_->validateTransform(
        sceneGraph_, node, local,
        parent.value_or(container::scene::SceneGraph::kInvalidNode));
#else
  (void)parent;
  (void)local;
#endif
  pausePhysicsForTransformEdit(node);
}

void RendererFrontend::publishSceneEdit() {
  // Edits and playback run before recording a frame, after graphics readers
  // retire. Probe work can still be using scene buffers asynchronously.
  if (subs_.ddgiManager)
    subs_.ddgiManager->synchronizeAsync();
  publishSceneRetirementRefresh();
  captureImportedLightEdits();
  if(subs_.sceneManager) subs_.sceneManager->reconcileActiveSceneRoots(sceneGraph_);
  if (subs_.sceneManager && subs_.lightingManager) {
    // Snapshot the old ordinal table before visibility can remove a binding.
    // Restore overrides by stable source identity into the fresh visible table.
    const auto edits = subs_.lightingManager->snapshotSceneLightEdits();
    subs_.sceneManager->refreshActiveScene(sceneGraph_);
    subs_.lightingManager->restoreSceneLightEdits(gltfImportedLightEdits_->merge(
        subs_.sceneManager->publishedAuthoredSceneLights(), edits));
  } else if (subs_.sceneManager) {
    subs_.sceneManager->refreshActiveScene(sceneGraph_);
  }
  selectionNavigationAnchor_ = {};
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();
  selectedBimNativeCurveDrawCommands_.clear();
  if (!sceneGraph_.getNode(sceneState_.selectedMeshNode) ||
      !sceneGraph_.isNodeEffectivelySelectable(sceneState_.selectedMeshNode))
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  if (subs_.lightingManager) {
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
    subs_.lightingManager->updateLightingData();
  }
  refreshSceneObjectData();
  syncSceneProviders();
  if (subs_.temporalManager)
    subs_.temporalManager->reset("scene edit or history playback");
  sceneEditPublicationPending_ = false;
}

RendererFrontend::SceneEditSelection
RendererFrontend::sceneEditSelection() const {
  return {sceneState_.rootNode, sceneState_.selectedMeshNode,
          selectedBimObjectIndex_,
          subs_.lightingManager
              ? subs_.lightingManager->selectedEditableLightId()
              : EditableLightId{},
          subs_.guiManager &&
              subs_.guiManager->sectionPlaneState().visualPlaneEditable};
}

void RendererFrontend::restoreSceneEditSelection(
    const SceneEditSelection &selection) {
  sceneState_.rootNode = selection.root;
  sceneState_.selectedMeshNode =
      sceneGraph_.getNode(selection.mesh)
          ? selection.mesh
          : container::scene::SceneGraph::kInvalidNode;
  selectedBimObjectIndex_ = selection.bim;
  if (subs_.lightingManager)
    subs_.lightingManager->selectEditableLight(selection.light);
  if (subs_.guiManager)
    subs_.guiManager->setSectionPlaneVisualEditable(selection.sectionEditable);
}


void RendererFrontend::applyEditHistoryRequest() {
  if (!subs_.guiManager)
    return;
  ensureSceneEditHistory();
  const auto request = subs_.guiManager->consumeEditHistoryRequest();
  if (request == container::ui::GuiManager::EditHistoryRequest::None)
    return;
  finishEditGestures();
  // Complete the gesture before replay. New input starts a new history step.
  transformDragSession_.active = false;
  sceneEdits_->finishGesture();
  try {
    const bool undo =
        request == container::ui::GuiManager::EditHistoryRequest::Undo;
    const auto label =
        std::string(undo ? sceneEdits_->undoLabel() : sceneEdits_->redoLabel());
    const bool applied = undo ? sceneEdits_->undo() : sceneEdits_->redo();
    subs_.guiManager->setStatusMessage(
        applied ? std::string(undo ? "Undid " : "Redid ") + label
                : "History action could not be applied to the current scene");
  } catch (const std::exception &error) {
    subs_.guiManager->setStatusMessage("Cannot replay scene edit: " +
                                       std::string(error.what()));
  }
}

} // namespace container::renderer
