#include <Container/renderer/core/RendererFrontend.h>

#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/lighting/ImportedSceneLightEdits.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/platform/VulkanContext.h>
#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/renderer/scene/GltfDeformationScene.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/Platform.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <stdexcept>

namespace container::renderer {

void RendererFrontend::selectGltfScene(uint32_t index) {
  if (!subs_.sceneManager || !subs_.sceneController ||
      index >= subs_.sceneManager->animationSource().scenes.size())
    throw std::invalid_argument("Requested glTF scene index is unavailable");
  const auto current = sceneGraph_.activeScene().value_or(0);
  if (current == index) return;

  ensureSceneEditHistory();
  finishLightEditInput();
  finishNodeGesture();
  container::scene::SceneGraph candidate(sceneGraph_);
  candidate.setActiveScene(index);
  if (candidate.renderableNodes().size() > svc_.config.maxSceneObjects)
    throw std::length_error("The selected glTF scene exceeds the object limit");

  auto& manager = *subs_.sceneManager;
  const auto bounds = manager.evaluateSceneBounds(candidate);
  auto lights = manager.evaluateGltfSceneLights(candidate, sceneState_.rootNode, index);
  auto nextLocalEdits = gltfSceneLightEdits_;
  auto nextImportedEdits = std::make_shared<ImportedSceneLightEdits>();
  SceneLightEdits nextLightState;
  if (subs_.lightingManager) {
    if (gltfImportedLightEdits_) *nextImportedEdits = *gltfImportedLightEdits_;
    const auto saved = std::make_shared<const SceneLightEdits>(subs_.lightingManager->snapshotSceneLightEdits());
    nextImportedEdits->capture(manager.publishedAuthoredSceneLights(), *saved);
    nextLocalEdits[current] = saved;
    const auto local = nextLocalEdits.find(index);
    nextLightState = nextImportedEdits->merge(lights,
        local == nextLocalEdits.end() ? SceneLightEdits{} : *local->second);
  }

  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
    throw std::runtime_error("Failed to retire GPU scene readers");
  const bool showDiagnostics = subs_.guiManager && subs_.guiManager->showNormalDiagCube();
  const bool hasDeformation = manager.deformationData() != nullptr;
  auto& controller = *subs_.sceneController;
  auto objects = hasDeformation ? controller.prepareGeometryEdit(candidate, manager.model(), bounds, showDiagnostics)
                                : controller.prepareObjectData(candidate, showDiagnostics);
  std::unique_ptr<GltfDeformationPass> deformationPass;
  const auto* deformation = objects.deformation();
  const auto vertices = objects.gpuBindVertices();
  const auto imageCount = static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  if (deformation && !deformation->gpuInstances().empty()) {
    deformationPass = std::make_unique<GltfDeformationPass>(
        svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
    deformationPass->createResources(container::util::executableDirectory(), imageCount);
    deformationPass->setScene(vertices, deformation->gpuInstances());
  }

  std::optional<DdgiManager::PreparedSceneHistoryReset> ddgi;
  if (subs_.ddgiManager && subs_.ddgiManager->supported()) {
    ddgi = subs_.ddgiManager->prepareSceneHistoryReset();
    if (!subs_.ddgiManager->canPublishSceneHistoryReset(*ddgi))
      throw std::runtime_error("Cannot retire probe history for the scene switch");
  }
  if (subs_.lightingManager) subs_.lightingManager->restoreSceneLightEdits(nextLightState);

  sceneGraph_.exchangeNodeEdit(candidate);
  manager.publishSceneSelection(index, bounds, lights);
  gltfSceneLightEdits_.swap(nextLocalEdits);
  gltfImportedLightEdits_.swap(nextImportedEdits);
  if (hasDeformation) {
    controller.exchangeGeometryEdit(objects);
    subs_.gltfDeformationPass.swap(deformationPass);
    gltfDeformationImageCount_ = imageCount;
    gltfDeformationSourceIdentity_ = manager.animationSourceRevision();
    gltfDeformationSourceRevision_ = manager.geometryRevision();
    gltfDeformationTopologyRevision_ = deformation ? deformation->topologyRevision() : 0;
    gltfDeformationVertexCount_ = vertices.size();
    gltfDeformationSubmittedEpoch_.reset();
  } else {
    controller.exchangeObjectData(objects);
  }
  if (ddgi && !subs_.ddgiManager->publishSceneHistoryReset(*ddgi)) std::terminate();

  if (gltfAnimation_) gltfAnimation_->pause();
  gltfAnimationCameraNode_.reset();
  gltfAnimationCamera_.reset();
  pendingGltfAnimationDeltaSeconds_ = 0;
  captureObjectBase_.reset();
  captureObjectNode_ = container::scene::SceneGraph::kInvalidNode;
  captureNodeTelemetry_.reset();
  editHistory_.clear();
  transformDragSession_.active = false;
  selectionNavigationAnchor_ = {};
  clearSelectedMeshNode();
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  invalidateDepthVisibilityFrames();
  sceneState_.diagCubeObjectIndex = controller.diagCubeObjectIndex();
  syncSceneStateFromController();
  syncSceneProviders();
  if (subs_.lightingManager) subs_.lightingManager->updateLightingData();
  resetCameraForActiveScene();
  refreshGltfCameraChoices();
  refreshLightEditInspector();
  if (subs_.guiManager) {
    auto& animation = subs_.guiManager->gltfAnimationControls();
    animation.requestedCameraNode.reset();
    animation.requestedPlaying.reset();
    animation.playing = false;
    animation.cameraNode = -1;
    subs_.guiManager->setGltfSceneIndex(index);
    subs_.guiManager->setStatusMessage("Selected glTF scene " + std::to_string(index));
  }
}

void RendererFrontend::processGltfSceneRequest() {
  if (!subs_.guiManager) return;
  const auto requested = subs_.guiManager->consumeGltfSceneRequest();
  if (!requested) return;
  try {
    selectGltfScene(*requested);
  } catch (const std::exception& error) {
    subs_.guiManager->setStatusMessage("Cannot switch glTF scene: " + std::string(error.what()));
  }
}

} // namespace container::renderer
