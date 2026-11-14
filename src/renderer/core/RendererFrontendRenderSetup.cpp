#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/geometry/GltfAnimationRuntime.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/pathtracing/ReferencePathTracePass.h"
#include "Container/renderer/platform/VulkanContext.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <stdexcept>

namespace container::renderer {
namespace {

using container::scene::SceneGraph;
using container::scene::SceneManager;
using container::ui::RenderSetupAction;

struct SetupBindings {
  std::vector<RenderSetupNode> nodes;
  std::vector<uint32_t> graphNodes;
};

SetupBindings activeBindings(const SceneManager& scene, const SceneGraph& graph) {
  const auto& source = scene.animationSource();
  SetupBindings bindings;
  bindings.graphNodes.resize(scene.renderSetupSourceNodeCount(), SceneGraph::kInvalidNode);
  for (uint32_t id = 0; id < graph.nodeCount(); ++id) {
    const auto& node = *graph.getNode(id);
    if (node.sourceNodeIndex < 0 || size_t(node.sourceNodeIndex) >= bindings.graphNodes.size() ||
        size_t(node.sourceNodeIndex) >= source.nodes.size() || !graph.isNodeActive(id)) continue;
    const auto index = static_cast<uint32_t>(node.sourceNodeIndex);
    const auto camera = source.nodes[index].camera;
    bindings.nodes.push_back({index, camera >= 0 && size_t(camera) < source.cameras.size()});
    bindings.graphNodes[index] = id;
  }
  return bindings;
}

nlohmann::json gradeJson(const DisplayGrade& grade) {
  constexpr std::array names{"aces", "reinhard", "clip", "filmic", "uncharted", "agx", "neutral"};
  const auto mode = static_cast<size_t>(grade.toneMapper);
  return {{"toneMap", mode < names.size() ? names[mode] : "unknown"}, {"saturation", grade.saturation},
          {"contrast", grade.contrast}, {"temperatureKelvin", grade.temperatureKelvin}, {"tintDuv", grade.tintDuv},
          {"vibrance", grade.vibrance}, {"shadowTone", grade.shadowTone}, {"highlightTone", grade.highlightTone}};
}

} // namespace

struct RendererFrontend::RenderSetupPreview {
  std::string name;
  std::optional<size_t> camera, variant;
  container::ui::TransformControls viewport;
  std::optional<container::scene::PerspectiveCamera> perspective;
  std::optional<container::scene::OrthographicCamera> orthographic;
  DisplayGrade grade;
  DisplayGrade previewGrade;
  std::vector<container::scene::MaterialVariantGraphEdit::Assignment> materials;
  std::optional<container::geometry::Model> geometry;
  std::unique_ptr<EnvironmentManager::PreviewMaps> environment;

  void restoreViewport(CameraController& controller, container::gpu::CameraData& data) const {
    controller.setOrthographic(SceneGraph::kInvalidNode, orthographic.has_value());
    if (perspective) *static_cast<container::scene::PerspectiveCamera*>(controller.camera()) = *perspective;
    if (orthographic) *static_cast<container::scene::OrthographicCamera*>(controller.camera()) = *orthographic;
    controller.applyCameraTransform(viewport, data, {});
  }
};

DisplayGrade RendererFrontend::effectiveDisplayGrade() const {
  if (renderSetupPreview_) return renderSetupPreview_->previewGrade;
  return subs_.guiManager ? subs_.guiManager->displayGrade() : captureDisplayGrade_.value_or(svc_.config.displayGrade);
}

void RendererFrontend::refreshRenderSetupView(bool environmentChanged) {
  if (environmentChanged) {
    auto& environment = *subs_.environmentManager;
    if (subs_.ddgiManager) {
      subs_.ddgiManager->resetSceneHistory();
      subs_.ddgiManager->replaceEnvironment(environment.prefilteredView(), environment.envSampler());
    }
    if (subs_.referencePathTracePass) {
      subs_.referencePathTracePass->invalidateHistory();
      ++referenceTimingGeneration_;
    }
    updateFrameDescriptorSets();
  }
  refreshCameraData();
  publishSceneEdit();
}

void RendererFrontend::activateRenderSetup(const RenderSetupEntry& entry) {
  auto& scene = *subs_.sceneManager;
  const auto activeSource = scene.renderSetupSource();
  if (!activeSource) throw std::runtime_error("Open the primary glTF or GLB before previewing a setup");
  if (transformDragSession_.active || materialGesture_ || lightEditGestureActive() ||
      sceneEditPublicationPending_ || pendingSceneRetirement_)
    throw std::runtime_error("Finish the current scene edit before previewing a render setup");
  auto& environment = *subs_.environmentManager;
  if (environment.environmentLoading()) throw std::runtime_error("Wait for the current environment to finish loading");
  const auto bindings = activeBindings(scene, sceneGraph_);
  ValidateRenderSetupEntry(entry, renderSetups_.source, *activeSource, bindings.nodes,
      std::min(scene.renderSetupSourceVariantCount(), scene.materialVariants().names().size()));
  const std::optional<size_t> camera = entry.cameraSourceNode;
  if (camera) {
    (void)cameraGraphNode(*camera);
    const auto extent = svc_.swapChainManager.extent();
    if (!extent.width || !extent.height || !container::geometry::gltf::EvaluateAnimatedCamera(
        scene.animationSource(), sceneGraph_, *camera, float(extent.width) / float(extent.height)))
      throw std::invalid_argument("The render setup camera cannot supply a valid view");
  }
  std::vector<int8_t> visibility(sceneGraph_.nodeCount(), -1);
  for (const auto& entryFlag : entry.visibility) visibility[bindings.graphNodes[entryFlag.sourceNode]] = entryFlag.visible;
  auto variant = scene.planMaterialVariant(sceneGraph_, entry.materialVariant);
  auto base = renderSetupPreview_;
  if (!base) {
    base = std::make_shared<RenderSetupPreview>();
    base->camera = gltfAnimationCameraNode_;
    base->variant = scene.selectedMaterialVariant();
    base->viewport = subs_.cameraController->cameraTransformControls();
    const auto* viewportCamera = subs_.cameraController->camera();
    if (const auto* perspective = dynamic_cast<const container::scene::PerspectiveCamera*>(viewportCamera))
      base->perspective = *perspective;
    if (const auto* orthographic = dynamic_cast<const container::scene::OrthographicCamera*>(viewportCamera))
      base->orthographic = *orthographic;
    base->grade = effectiveDisplayGrade();
    base->materials = variant.assignments;
  }
  std::string nextName = entry.name;
  auto preparedEnvironment = entry.hdrPath.empty() ? nullptr : environment.prepareHdrPreview(entry.hdrPath);
  const bool restoreEnvironment = entry.hdrPath.empty() && bool(base->environment);
  const bool environmentChanged = bool(preparedEnvironment) || restoreEnvironment;
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
    throw std::runtime_error("Could not retire readers before switching render setup");
  if (variant.changes() && !scene.applyMaterialVariant(sceneGraph_, variant, true, &base->geometry))
    throw std::runtime_error("The render setup material bindings are stale");

  std::unique_ptr<EnvironmentManager::PreviewMaps> retired;
  if (preparedEnvironment) {
    environment.exchangePreviewMaps(*preparedEnvironment);
    if (!base->environment) base->environment = std::move(preparedEnvironment);
  } else if (restoreEnvironment) {
    retired = std::move(base->environment);
    environment.exchangePreviewMaps(*retired);
  }
  environment.setPreviewActive(true);
  sceneGraph_.setPreviewVisibility(std::move(visibility));
  if (!camera) base->restoreViewport(*subs_.cameraController, buffers_.cameraData);
  (void)setGltfCameraNode(camera);
  base->previewGrade = entry.grade;
  base->name.swap(nextName);
  renderSetupPreview_ = std::move(base);
  refreshRenderSetupView(environmentChanged);
}

void RendererFrontend::deactivateRenderSetup() {
  if (!renderSetupPreview_) return;
  auto base = renderSetupPreview_;
  auto& scene = *subs_.sceneManager;
  auto& environment = *subs_.environmentManager;
  auto camera = base->camera;
  if (camera) {
    const auto extent = svc_.swapChainManager.extent();
    try {
      (void)cameraGraphNode(*camera);
      if (!extent.width || !extent.height || !container::geometry::gltf::EvaluateAnimatedCamera(
          scene.animationSource(), sceneGraph_, *camera, float(extent.width) / float(extent.height))) camera.reset();
    } catch (const std::invalid_argument&) { camera.reset(); }
  }
  auto variant = scene.planMaterialVariant(sceneGraph_, base->variant);
  for (const auto& original : base->materials) {
    const auto found = std::ranges::lower_bound(variant.assignments, original.node, {},
        &container::scene::MaterialVariantGraphEdit::Assignment::node);
    if (found != variant.assignments.end() && found->node == original.node) found->after = original.before;
  }
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if (vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device()) != VK_SUCCESS)
    throw std::runtime_error("Could not retire readers before restoring the base view");
  if (variant.changes() && !scene.applyMaterialVariant(sceneGraph_, variant))
    throw std::runtime_error("The base material bindings are stale");
  sceneGraph_.setPreviewVisibility({});
  if (base->environment) environment.exchangePreviewMaps(*base->environment);
  environment.setPreviewActive(false);
  base->restoreViewport(*subs_.cameraController, buffers_.cameraData);
  (void)setGltfCameraNode(camera);
  if (subs_.guiManager) subs_.guiManager->setDisplayGrade(base->grade);
  else captureDisplayGrade_ = base->grade;
  renderSetupPreview_.reset();
  refreshRenderSetupView(bool(base->environment));
}

const container::geometry::Model* RendererFrontend::renderSetupExportGraph(SceneGraph& graph) const {
  if (!renderSetupPreview_) return nullptr;
  graph = sceneGraph_;
  graph.setPreviewVisibility({});
  for (const auto& assignment : renderSetupPreview_->materials)
    graph.setMaterialIndex(assignment.node, assignment.before);
  return renderSetupPreview_->geometry ? &*renderSetupPreview_->geometry : nullptr;
}

void RendererFrontend::clearRenderSetupDocument() {
  renderSetups_ = {};
  renderSetupPath_.clear();
  renderSetupStatus_.clear();
  renderSetupDiagnostic_.clear();
}

void RendererFrontend::handleRenderSetup(const container::ui::RenderSetupRequest& request) {
  if (request.action == RenderSetupAction::Deactivate) {
    deactivateRenderSetup();
    return;
  }
  auto* scene = subs_.sceneManager.get();
  const auto source = scene ? scene->renderSetupSource() : std::nullopt;
  if (!source) throw std::runtime_error("Render setups require an ordinary primary glTF or GLB scene");
  if (renderSetupPreview_ && request.action != RenderSetupAction::Activate && request.action != RenderSetupAction::Save)
    throw std::runtime_error("Deactivate the render setup before editing or opening its sidecar");
  if (request.action == RenderSetupAction::Open || request.action == RenderSetupAction::Save) {
    auto path = std::filesystem::absolute(container::util::pathFromUtf8(request.path)).lexically_normal();
    if (request.action == RenderSetupAction::Open) {
      auto document = LoadRenderSetupDocument(path);
      ValidateRenderSetupSource(document.source, *source);
      renderSetups_ = std::move(document);
    } else {
      auto extension = container::util::pathToUtf8(path.extension());
      std::ranges::transform(extension, extension.begin(), [](unsigned char value) { return char(std::tolower(value)); });
      if (extension != ".json") throw std::invalid_argument("Choose a .json path for the render setup sidecar");
      ValidateRenderSetupSource(renderSetups_.source, *source);
      scene->validateSceneExportDestination(path);
      const auto samePath = [&](const std::filesystem::path& input) {
        if (input.empty()) return false;
        std::error_code error;
        return std::filesystem::absolute(input).lexically_normal() == path || std::filesystem::equivalent(input, path, error);
      };
      if (samePath(scene->loadedPrimarySourcePath()) ||
          (subs_.bimManager && samePath(subs_.bimManager->loadedSourcePath())))
        throw std::runtime_error("The sidecar cannot overwrite a loaded model");
      if (std::any_of(renderSetups_.setups.begin(), renderSetups_.setups.end(), [&](const auto& setup) {
            return samePath(setup.hdrPath);
          }) || samePath(subs_.environmentManager->activeHdrPath()))
        throw std::runtime_error("The sidecar cannot overwrite an HDR environment");
      SaveRenderSetupDocument(path, renderSetups_);
    }
    renderSetupPath_ = std::move(path);
    return;
  }
  const auto existing = std::ranges::find(renderSetups_.setups, request.name, &RenderSetupEntry::name);
  const bool found = existing != renderSetups_.setups.end();
  if (request.action == RenderSetupAction::Activate) {
    if (!found) throw std::invalid_argument("No render setup has that name");
    activateRenderSetup(*existing);
    return;
  }
  if (found == (request.action == RenderSetupAction::Capture))
    throw std::invalid_argument(found ? "A render setup already has that name" : "No render setup has that name");
  auto document = renderSetups_;
  if (document.source.digest.empty()) document.source = *source;
  ValidateRenderSetupSource(document.source, *source);
  const size_t index = existing - renderSetups_.setups.begin();
  switch (request.action) {
  case RenderSetupAction::Capture:
  case RenderSetupAction::Update: {
    if (subs_.environmentManager->environmentLoading()) throw std::runtime_error("Wait for the environment to finish loading");
    RenderSetupEntry entry;
    entry.name = request.name;
    const auto indexForSidecar = [](std::optional<size_t> value) -> std::optional<uint32_t> {
      if (!value) return std::nullopt;
      if (*value > UINT32_MAX) throw std::invalid_argument("Render setup reference is outside the source index range");
      return static_cast<uint32_t>(*value);
    };
    entry.cameraSourceNode = indexForSidecar(gltfAnimationCameraNode_);
    entry.materialVariant = indexForSidecar(scene->selectedMaterialVariant());
    const auto& path = subs_.environmentManager->activeHdrPath();
    if (!path.empty()) entry.hdrPath = std::filesystem::absolute(path).lexically_normal();
    entry.grade = effectiveDisplayGrade();
    const auto bindings = activeBindings(*scene, sceneGraph_);
    entry.visibility.reserve(bindings.nodes.size());
    for (const auto& node : bindings.nodes) entry.visibility.push_back({node.sourceNode,
        sceneGraph_.getNode(bindings.graphNodes[node.sourceNode])->visible});
    ValidateRenderSetupEntry(entry, document.source, *source, bindings.nodes,
        std::min(scene->renderSetupSourceVariantCount(), scene->materialVariants().names().size()));
    if (found) document.setups[index] = std::move(entry);
    else document.setups.push_back(std::move(entry));
    break;
  }
  case RenderSetupAction::Rename: document.setups[index].name = request.newName; break;
  case RenderSetupAction::Delete: document.setups.erase(document.setups.begin() + index); break;
  default: throw std::invalid_argument("Invalid render setup action");
  }
  ValidateRenderSetupDocument(document);
  renderSetups_ = std::move(document);
}

void RendererFrontend::publishRenderSetupUi() {
  if (!subs_.guiManager) return;
  container::ui::RenderSetupUiState state;
  state.available = subs_.sceneManager && subs_.sceneManager->renderSetupSource().has_value();
  if (renderSetupPreview_) state.activeName = renderSetupPreview_->name;
  state.status = renderSetupDiagnostic_.empty() ? renderSetupStatus_ : renderSetupDiagnostic_;
  state.path = container::util::pathToUtf8(renderSetupPath_);
  for (const auto& setup : renderSetups_.setups) state.names.push_back(setup.name);
  subs_.guiManager->setRenderSetupState(std::move(state));
}

void RendererFrontend::processRenderSetupUi() {
  if (!subs_.guiManager) return;
  if (const auto request = subs_.guiManager->consumeRenderSetupRequest()) {
    try {
      handleRenderSetup(*request);
      renderSetupDiagnostic_.clear();
      renderSetupStatus_ = "Render setups updated";
    } catch (const std::exception& error) {
      renderSetupDiagnostic_ = error.what();
    }
  }
}

void RendererFrontend::applyCaptureRenderSetup(const container::temporal::CaptureRenderSetup& event) {
  using Action = container::temporal::CaptureRenderSetup::Action;
  RenderSetupAction action;
  switch (event.action) {
  case Action::Capture: action = RenderSetupAction::Capture; break;
  case Action::Activate: action = RenderSetupAction::Activate; break;
  case Action::Deactivate: action = RenderSetupAction::Deactivate; break;
  case Action::Load: action = RenderSetupAction::Open; break;
  case Action::Save: action = RenderSetupAction::Save; break;
  default: throw std::invalid_argument("Invalid render setup capture action");
  }
  try {
    handleRenderSetup({action, event.name, {}, event.path});
    renderSetupDiagnostic_.clear();
  } catch (const std::exception& error) {
    if (!event.expectFailure) throw;
    renderSetupDiagnostic_ = error.what();
    return;
  }
  if (event.expectFailure) throw std::runtime_error("The invalid render setup unexpectedly activated");
}

void RendererFrontend::appendRenderSetupTelemetry(nlohmann::json& json) const {
  auto names = nlohmann::json::array();
  for (const auto& setup : renderSetups_.setups) names.push_back(setup.name);
  json["renderSetup"] = {{"active", renderSetupPreview_ ? nlohmann::json(renderSetupPreview_->name) : nlohmann::json(nullptr)},
      {"names", std::move(names)}, {"diagnostic", renderSetupDiagnostic_}, {"displayGrade", gradeJson(effectiveDisplayGrade())}};
}

} // namespace container::renderer
