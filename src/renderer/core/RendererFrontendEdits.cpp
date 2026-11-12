#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/bim/BimManager.h"
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
#include "Container/utility/SourceMaterialMapping.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/geometry/GltfInteractivity.h"
#include "Container/geometry/GltfLinkedAssets.h"
#include "Container/geometry/GltfSceneExport.h"
#include "Container/geometry/GltfNodeExportCompaction.h"
#include "Container/renderer/scene/SceneLightExport.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsElementBindings.h"
#include "Container/physics/PhysicsScene.h"
#endif

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace container::renderer {
namespace {
using namespace container::geometry::gltf;

struct LinkedExportPlan {
  std::vector<GltfLinkedHybridCarrier> carriers;
  std::vector<uint8_t> linkedGraphNodes, prunedGraphNodes, omittedAnimations;
  std::vector<size_t> linkedContexts;
  std::vector<int32_t> sourceMaterialToExport, animatedLinkedMaterials;
  GltfExportResourceIndexMaps resources;
};

LinkedExportPlan planLinkedExport(const container::scene::SceneManager& manager,
    const container::scene::SceneGraph& graph, const GltfLinkedCompositionInfo& composition) {
  LinkedExportPlan plan;
  plan.linkedGraphNodes.resize(graph.nodeCount());
  plan.prunedGraphNodes.resize(graph.nodeCount());
  std::vector<int32_t> carriers(composition.primaryNodes, -1);
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    const auto source = graph.getNode(id)->sourceNodeIndex;
    const bool linked = manager.isLinkedNodeReadOnly(graph, id);
    const bool retired = graph.isNodeRetired(id);
    plan.linkedGraphNodes[id] = linked;
    plan.prunedGraphNodes[id] = linked || retired;
    if (source < 0 || retired) continue;
    const auto index = size_t(source);
    if (!manager.isLinkedSourceCarrier(index) && !linked) continue;
    if (index >= composition.nodePlacements.size())
      throw std::runtime_error("Linked save cannot retain references from a merged manifest. Use portable Save As.");
    if (index >= carriers.size() || linked) continue;
    if (carriers[index] >= 0)
      throw std::runtime_error("A linked placement has more than one export node.");
    carriers[index] = static_cast<int32_t>(id);
  }
  std::vector<bool> retained(composition.placementRecords.size());
  std::vector<bool> contexts(composition.sourceContexts.size());
  for (size_t i = 0; i < composition.placementRecords.size(); ++i) {
    const auto& placement = composition.placementRecords[i];
    retained[i] = manager.isLinkedSourceCarrier(size_t(placement.carrierNode));
    if (placement.parentPlacement < 0) {
      if (!retained[i]) continue;
      const auto output = carriers.at(size_t(placement.carrierNode));
      if (output < 0) throw std::runtime_error("A retained linked placement has no export node.");
      plan.carriers.push_back({placement.carrierNode, output});
    } else if (size_t(placement.parentPlacement) >= i || retained[i] != retained[size_t(placement.parentPlacement)]) {
      throw std::runtime_error("Linked save requires the complete nested placement family.");
    }
    if (retained[i]) contexts.at(placement.sourceContext) = true;
  }

  const auto& source = manager.animationSource();
  if (source.extensions.contains(kGltfInteractivityExtension))
    throw std::runtime_error("Linked save after detachment requires a scene without interactivity. Use portable Save As.");
  if (!plan.carriers.empty() && manager.selectedMaterialVariant())
    throw std::runtime_error("Linked save requires the default material variant. Use portable Save As for the selected variant.");
  plan.omittedAnimations.resize(source.animations.size());
  for (size_t i = 0; i < contexts.size(); ++i) {
    if (!contexts[i]) continue;
    plan.linkedContexts.push_back(i);
    const auto range = composition.sourceContexts[i].animations;
    const auto first = size_t(range.first);
    if (first > plan.omittedAnimations.size() || range.count > plan.omittedAnimations.size() - first)
      throw std::runtime_error("Linked animation ownership is outside the export clip table.");
    std::fill_n(plan.omittedAnimations.begin() + first, range.count, uint8_t{1});
  }
  plan.sourceMaterialToExport = container::scene::ExportSourceMaterialMapping(
      manager.sourceMaterialRuntimeMapping(), source.materials.size(), manager.materialCount());
  for (size_t i = 0; i < source.animations.size(); ++i) {
    if (!plan.omittedAnimations[i]) continue;
    for (const auto& channel : source.animations[i].channels) {
      const auto pointer = channel.target_extensions.find("KHR_animation_pointer");
      if (pointer == channel.target_extensions.end() || !pointer->second.Has("pointer")) continue;
      const auto& path = pointer->second.Get("pointer");
      if (!path.IsString() || !path.Get<std::string>().starts_with("/materials/")) continue;
      const auto target = ResolveAnimationPointer(source, path.Get<std::string>());
      if (target && target->kind == AnimationTargetKind::Material)
        plan.animatedLinkedMaterials.push_back(plan.sourceMaterialToExport.at(target->index));
    }
  }
  std::ranges::sort(plan.animatedLinkedMaterials);
  plan.animatedLinkedMaterials.erase(std::unique(plan.animatedLinkedMaterials.begin(),
      plan.animatedLinkedMaterials.end()), plan.animatedLinkedMaterials.end());
  return plan;
}

void omitLinkedLightOverlays(const container::scene::SceneManager& manager, const SceneLightEdits& edits,
    const container::scene::AuthoredSceneLights& full, container::scene::AuthoredSceneLights& visible,
    std::vector<EditableLightEntity>& lights) {
  const auto linked = [&](const auto& binding) {
    return binding.sourceNode >= 0 && manager.isLinkedSourceNodeReadOnly(size_t(binding.sourceNode));
  };
  std::erase_if(lights, [&](const auto& light) {
    if (light.source != EditableLightSource::Imported) return false;
    const auto& bindings = light.type == EditableLightType::Area ? full.areaBindings
        : light.type == EditableLightType::Directional ? full.directionalBindings : full.pointBindings;
    if (!linked(bindings.at(light.id.index))) return false;
    if (manager.importScale() != 1.0f &&
        (light.type == EditableLightType::Area ||
         (light.type != EditableLightType::Directional && light.range > 0.0f)))
      throw std::runtime_error("Linked save cannot preserve scaled light ranges or areas. Use portable Save As.");
    return true;
  });
  // An exported manual sun becomes the first scene root. Its original
  // linked sun will be restored by the URI and needs no inline replacement.
  if (edits.directionalOverride && edits.directionalOverride->source == EditableLightSource::Manual &&
      !visible.directionalBindings.empty() && linked(visible.directionalBindings.front())) {
    visible.directionalBindings.clear();
    visible.directionals.clear();
  }
}

} // namespace

void RendererFrontend::captureImportedLightEdits() {
  if(!subs_.sceneManager || !subs_.lightingManager) return;
  if(!gltfImportedLightEdits_) gltfImportedLightEdits_=std::make_shared<ImportedSceneLightEdits>();
  gltfImportedLightEdits_->capture(subs_.sceneManager->publishedAuthoredSceneLights(),
      subs_.lightingManager->snapshotSceneLightEdits());
}

void RendererFrontend::selectGltfScene(size_t index, bool expectRejection) {
  gltfSceneSelectionError_.clear();
  if (!subs_.sceneManager) throw std::runtime_error("No primary scene is loaded");
  if (index == subs_.sceneManager->selectedScene() &&
      index < subs_.sceneManager->sceneCatalog().entries.size()) {
    if (expectRejection) throw std::runtime_error("Capture requested rejection of the current glTF scene");
    return;
  }
  container::scene::GltfSceneActivation activation;
  try {
    activation = subs_.sceneManager->planSceneSelection(sceneGraph_, index);
  } catch (const std::invalid_argument& error) {
    if (!expectRejection) throw;
    gltfSceneSelectionError_ = error.what();
    return;
  }
  if (expectRejection) throw std::runtime_error("Capture glTF scene selection unexpectedly passed preflight");

  // Rejected requests leave live previews, the journal and the active view intact.
  finishEditGestures();
  if (sceneEditPublicationPending_ || pendingSceneRetirement_) publishSceneEdit();
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
        subs_.sceneManager->publishedAuthoredSceneLights(),edits));
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
    subs_.guiManager->resetSceneNodeAuthoring();
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
  // The old editable light rows belong to the previous scene. Rebuild them
  // from the new binding table without capturing the old rows a second time.
  refreshSceneEditView();
  resetCameraForActiveScene();
  if(subs_.temporalManager) subs_.temporalManager->reset("glTF scene changed");
}

void RendererFrontend::updateGltfSceneState() {
  if(!subs_.sceneManager) return;
  if (subs_.guiManager) subs_.guiManager->setLinkedSceneSaveAvailable(subs_.sceneManager->hasLinkedScene());
  if(gltfSceneGraphIdentity_!=sceneGraph_.identity()) {
    gltfSceneGraphIdentity_=sceneGraph_.identity();
    gltfSceneLightEdits_.clear(); gltfImportedLightEdits_.reset();
    gltfSceneSelectionError_.clear();
    if(subs_.guiManager) {
      auto& state=subs_.guiManager->gltfSceneControls(); state={};
      for(const auto& entry:subs_.sceneManager->sceneCatalog().entries) state.names.push_back(entry.name);
    }
  }
  if (const auto requested = std::exchange(captureGltfScene_, {})) {
    selectGltfScene(requested->index, requested->expectRejection);
    if (subs_.guiManager) subs_.guiManager->gltfSceneControls().error = gltfSceneSelectionError_;
  }
  if(subs_.guiManager) subs_.guiManager->gltfSceneControls().selected=int(subs_.sceneManager->selectedScene());
}

void RendererFrontend::processGltfSceneRequest() {
  if(!subs_.guiManager || !subs_.sceneManager) return;
  auto& state=subs_.guiManager->gltfSceneControls();
  if(const auto requested=std::exchange(state.requested,{})) {
    try { selectGltfScene(size_t(*requested)); }
    catch(const std::exception& error) { gltfSceneSelectionError_=error.what(); }
    state.error = gltfSceneSelectionError_;
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

void RendererFrontend::saveSceneModel(const std::filesystem::path& outputPath, bool keepLinks) {
  finishEditGestures();
  if (!subs_.sceneManager) throw std::runtime_error("No primary scene is loaded");
  const auto& manager = *subs_.sceneManager;
  if (keepLinks && gltfAnimation_ && gltfAnimation_->selectedClip())
    throw std::runtime_error("Linked save requires the authored rest pose. Use portable Save As for the active animation.");
  const auto* composition = keepLinks ? manager.linkedPrimaryComposition() : nullptr;
  if (keepLinks && !composition)
    throw std::runtime_error("The primary scene has no retained linked asset manifest");
  if (subs_.bimManager && !subs_.bimManager->loadedSourcePath().empty()) {
    const auto& source = subs_.bimManager->loadedSourcePath();
    std::error_code error;
    const auto target = std::filesystem::weakly_canonical(outputPath, error);
    if (outputPath.lexically_normal() == source || (!error && target == source) ||
        std::filesystem::equivalent(outputPath, source, error))
      throw std::runtime_error("Scene export cannot replace the loaded auxiliary model");
  }
  manager.validateSceneExportDestination(outputPath);
  if (composition) {
    const auto hasEdits = [](const SceneLightEdits& edits) {
      const auto anyOverride = [](const auto& values) {
        return std::ranges::any_of(values, [](const auto& value) { return value.has_value(); });
      };
      return edits.directionalOverride || !edits.manualPointLights.empty() || !edits.manualAreaLights.empty() ||
          anyOverride(edits.importedPointOverrides) || anyOverride(edits.importedAreaOverrides);
    };
    auto imported = gltfImportedLightEdits_ ? *gltfImportedLightEdits_ : ImportedSceneLightEdits{};
    const auto active = subs_.sceneManager->selectedScene();
    const auto current = subs_.lightingManager ? subs_.lightingManager->snapshotSceneLightEdits() : SceneLightEdits{};
    imported.capture(subs_.sceneManager->publishedAuthoredSceneLights(), current);
    const auto anyImported = [](const auto& overrides) {
      return std::ranges::any_of(overrides, [](const auto& entry) { return entry.second.has_value(); });
    };
    const bool hasLightEdits = hasEdits(current) || anyImported(imported.points) ||
        anyImported(imported.areas) || anyImported(imported.directionals) ||
        std::ranges::any_of(gltfSceneLightEdits_, [&](const auto& entry) {
          return entry.first != active && hasEdits(*entry.second);
        });
    if (!hasLightEdits && manager.trySaveLinkedSceneManifest(sceneGraph_, outputPath)) return;
  }
  std::optional<LinkedExportPlan> linked;
  GltfLinkedPortableExportOptions projection;
  if (composition) {
    linked = planLinkedExport(manager, sceneGraph_, *composition);
    projection = {linked->linkedGraphNodes, linked->omittedAnimations, &linked->resources};
  }
  auto snapshot = manager.buildSceneExport(sceneGraph_, linked ? &projection : nullptr);
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
      sceneLights[index]=evaluateEditableSceneLightsForExport(sceneBindings[index],edits,imported,outerTransform);
      if (linked) omitLinkedLightOverlays(manager, edits, sceneBindings[index], visibleSceneBindings[index], sceneLights[index]);
    }
    exportEditableSceneLightsForScenes(snapshot,sceneLights,
        &subs_.sceneManager->animationSource(),sourceNodes,subs_.sceneManager->importScale(),
        sceneBindings,visibleSceneBindings);
  }
  if (linked) {
    const auto retiredMaterials = manager.retiredMaterialIndices();
    writeLinkedHybridScene(std::move(snapshot), {*composition, linked->carriers, linked->prunedGraphNodes,
        linked->linkedContexts, linked->sourceMaterialToExport, retiredMaterials,
        linked->animatedLinkedMaterials, linked->resources}, outputPath);
    return;
  }
  const auto retired = manager.retiredSceneExportNodes(sceneGraph_);
  if (!retired.empty())
    snapshot = container::geometry::gltf::CompactGltfExportNodes(snapshot, retired).model;
  manager.compactRetiredMaterialExport(snapshot);
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
            },
            .beforeNodeMetadata = [this](uint32_t node) {
              requireEditableSceneNode(node);
              if (!subs_.sceneManager || !subs_.guiManager) return;
              const auto sourceNode = sceneGraph_.getNode(node)->sourceNodeIndex;
              const auto& nodes = subs_.sceneManager->animationSource().nodes;
              gltfCameraChoicesDirty_ |= sourceNode >= 0 && size_t(sourceNode) < nodes.size() &&
                  nodes[size_t(sourceNode)].camera >= 0;
            },
            .prepareNodeFlags = [this](uint32_t node,
                const container::util::SceneEditHistory::NodeFlagChanges& changes) {
              using Actions = container::util::SceneEditHistory::NodeFlagActions;
              const auto sourceNode = sceneGraph_.getNode(node)->sourceNodeIndex;
              GltfNodeFlagEdit edit;
              if (subs_.sceneManager && sourceNode >= 0) {
                requireEditableSceneNode(node);
                edit = subs_.sceneManager->prepareGltfNodeFlagEdit(size_t(sourceNode), changes);
              }
              std::shared_ptr<std::optional<VisibilityLightSelection>> displaced;
              if (changes[0] && subs_.sceneManager && subs_.lightingManager)
                displaced = std::make_shared<std::optional<VisibilityLightSelection>>();
              if (!edit.before.authoredExtensions && !displaced) return Actions{};
              const auto apply = [this, node, sourceNode, displaced](const GltfNodeFlags& flags, bool visible) {
                auto selected = displaced && !visible ? selectedLightUnder(node) : std::nullopt;
                if (flags.authoredExtensions)
                  ApplyGltfNodeFlagEdit(subs_.sceneManager->animationSource().nodes.at(size_t(sourceNode)), flags);
                if (!displaced) return;
                if (!visible) {
                  *displaced = std::move(selected);
                } else if (*displaced) {
                  auto restore = std::exchange(*displaced, std::nullopt);
                  if (sceneEditSelectionVacant()) pendingVisibilityLightSelection_ = std::move(restore);
                }
              };
              const bool visible = changes[0].value_or(true);
              return Actions{
                  [apply, before = edit.before, visible] { apply(before, !visible); },
                  [apply, after = edit.after, visible] { apply(after, visible); }};
            },
            .meshSelection = [this] {
              return container::util::SceneEditHistory::MeshSelection{
                  sceneState_.selectedMeshNode, selectedBimObjectIndex_ != container::scene::SceneGraph::kInvalidNode ||
                      (subs_.lightingManager && isValidEditableLightId(subs_.lightingManager->selectedEditableLightId())) ||
                      (subs_.guiManager && subs_.guiManager->sectionPlaneState().visualPlaneEditable)};
            },
            .setMeshSelection = [this](uint32_t node) noexcept { sceneState_.selectedMeshNode = node; }});
  }
  sceneEdits_->sync();
}

void RendererFrontend::requireEditableSceneNode(uint32_t node) const {
  if (subs_.sceneManager && subs_.sceneManager->isLinkedNodeReadOnly(sceneGraph_, node))
    throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
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
                      ? "Duplicated scene subtree"
                      : request->action == container::ui::SceneNodeAuthoringAction::MakeEditable
                          ? "Made linked source editable" : "Deleted scene subtree");
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
  if (!request.nodeIncarnation || request.nodeIncarnation != sceneGraph_.nodeIncarnation(request.node) ||
      sceneGraph_.isNodeRetired(request.node))
    throw std::invalid_argument("The requested scene node is no longer available. Select it and try again.");
  if (request.action == SceneNodeAuthoringAction::MakeEditable) {
    if (!subs_.sceneManager || !subs_.sceneManager->canMakeLinkedAssetEditable(sceneGraph_, request.node))
      throw std::invalid_argument("Choose a linked asset placement or its contents");
    finishEditGestures();
    ensureSceneEditHistory();
    if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
    if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
      throw std::runtime_error("Failed to retire readers before making the linked asset editable");
    if (subs_.sceneManager->makeLinkedAssetEditable(sceneGraph_, request.node) == 0) return false;
    sceneEdits_->clear();
    return true;
  }
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
  requireEditableSceneNode(target);
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
  requireEditableSceneNode(target);
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
  const auto primitive=subs_.sceneController->resolveScenePrimitiveGeometry(kind);
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
  subs_.sceneController->refreshPrimitiveGeometryBuffers();
  syncSceneStateFromController();
  restoreSceneEditSelection(after);
  selectionNavigationAnchor_={}; clearHoveredMeshNode();
  selectedDrawCommands_.clear(); selectedBimDrawCommands_.clear();
  refreshSceneObjectData(); syncSceneProviders();
  if(subs_.guiManager) subs_.guiManager->setStatusMessage("Added "+std::string(scenePrimitiveKindLabel(kind)));
  return first;
}

bool RendererFrontend::reparentSceneNode(uint32_t node,std::optional<uint32_t> parent) {
  requireEditableSceneNode(node);
  if (parent) requireEditableSceneNode(*parent);
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
  requireEditableSceneNode(node);
  if (parent) requireEditableSceneNode(*parent);
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
  const auto& pendingLight = pendingVisibilityLightSelection_;
  publishSceneRetirementRefresh();
  captureImportedLightEdits();
  if(subs_.sceneManager) subs_.sceneManager->reconcileActiveSceneRoots(sceneGraph_);
  if (subs_.sceneManager && subs_.lightingManager) {
    // Snapshot the old ordinal table before visibility can remove a binding.
    // Restore overrides and selection into the fresh visible table.
    const bool mayRestoreLight = pendingLight && sceneEditSelectionVacant();
    const auto selected = subs_.lightingManager->selectedEditableLightId();
    const auto binding = selectedImportedLightBinding(selected, authoredLightBindings(*subs_.sceneManager, selected.type));
    const auto edits = subs_.lightingManager->snapshotSceneLightEdits();
    subs_.sceneManager->refreshActiveScene(sceneGraph_);
    subs_.lightingManager->restoreSceneLightEdits(gltfImportedLightEdits_->merge(
        subs_.sceneManager->publishedAuthoredSceneLights(), edits));
    auto nextSelection = remapImportedLightSelection(selected, binding ? &*binding : nullptr,
        authoredLightBindings(*subs_.sceneManager, selected.type));
    // A visibility command can restore a light only after its binding returns
    // to the visible table, and only while there is no replacement selection.
    if (mayRestoreLight && pendingLight->graphIdentity == sceneGraph_.identity() &&
        pendingLight->assetIdentity == subs_.sceneManager->materialAssetIdentity() &&
        pendingLight->scene == subs_.sceneManager->selectedScene() &&
        pendingLight->nodeIncarnation == sceneGraph_.nodeIncarnation(pendingLight->node) &&
        !sceneGraph_.isNodeRetired(pendingLight->node)) {
      nextSelection = remapImportedLightSelection(pendingLight->light, pendingLight->binding.get(),
          authoredLightBindings(*subs_.sceneManager, pendingLight->light.type));
    }
    subs_.lightingManager->selectEditableLight(nextSelection);
  } else if (subs_.sceneManager) {
    subs_.sceneManager->refreshActiveScene(sceneGraph_);
  }
  refreshSceneEditView();
}

void RendererFrontend::refreshSceneEditView() {
  if (gltfCameraChoicesDirty_) refreshGltfCameraChoices();
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
  pendingVisibilityLightSelection_.reset();
  sceneEditPublicationPending_ = false;
}

bool RendererFrontend::sceneEditSelectionVacant() const {
  constexpr auto none = container::scene::SceneGraph::kInvalidNode;
  return sceneState_.selectedMeshNode == none && selectedBimObjectIndex_ == none &&
      (!subs_.lightingManager || !isValidEditableLightId(subs_.lightingManager->selectedEditableLightId())) &&
      (!subs_.guiManager || !subs_.guiManager->sectionPlaneState().visualPlaneEditable);
}

std::optional<RendererFrontend::VisibilityLightSelection> RendererFrontend::selectedLightUnder(uint32_t ancestor) const {
  if (!subs_.sceneManager || !subs_.lightingManager) return {};
  const auto light = subs_.lightingManager->selectedEditableLightId();
  auto binding = selectedImportedLightBinding(light, authoredLightBindings(*subs_.sceneManager, light.type));
  if (!binding) return {};
  for (uint32_t node = 0; node < sceneGraph_.nodeCount(); ++node) {
    if (sceneGraph_.getNode(node)->sourceNodeIndex != binding->sourceNode ||
        !sceneGraph_.isNodeEffectivelyVisible(node)) continue;
    auto parent = node;
    for (size_t steps = 0; steps < sceneGraph_.nodeCount(); ++steps) {
      if (parent == ancestor) {
        return VisibilityLightSelection{light,
            std::make_shared<const container::scene::AuthoredLightBinding>(std::move(*binding)),
            node, sceneGraph_.nodeIncarnation(node), sceneGraph_.identity(),
            subs_.sceneManager->materialAssetIdentity(), subs_.sceneManager->selectedScene()};
      }
      const auto* current = sceneGraph_.getNode(parent);
      if (!current) break;
      parent = current->parent;
    }
  }
  return {};
}

RendererFrontend::SceneEditSelection
RendererFrontend::sceneEditSelection() const {
  const auto light = subs_.lightingManager ? subs_.lightingManager->selectedEditableLightId() : EditableLightId{};
  auto binding = subs_.sceneManager ?
      selectedImportedLightBinding(light, authoredLightBindings(*subs_.sceneManager, light.type)) : std::nullopt;
  return {sceneState_.rootNode, sceneState_.selectedMeshNode, selectedBimObjectIndex_, light,
          subs_.guiManager && subs_.guiManager->sectionPlaneState().visualPlaneEditable,
          binding ? std::make_shared<const container::scene::AuthoredLightBinding>(std::move(*binding)) : nullptr};
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
    subs_.lightingManager->selectEditableLight(subs_.sceneManager ?
        remapImportedLightSelection(selection.light, selection.lightBinding.get(),
            authoredLightBindings(*subs_.sceneManager, selection.light.type)) : selection.light);
  if (subs_.guiManager)
    subs_.guiManager->setSectionPlaneVisualEditable(selection.sectionEditable);
}


void RendererFrontend::applyEditHistoryRequest() {
  if (!subs_.guiManager)
    return;
  ensureSceneEditHistory();
  const auto request = subs_.guiManager->consumeEditHistoryRequest();
  if (request == container::ui::GuiManager::EditHistoryRequest::None ||
      transformDragSession_.active || sceneEdits_->transformGestureActive())
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
