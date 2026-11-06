#include <Container/app/AppConfig.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/renderer/bim/BimManager.h>
#include <Container/renderer/core/RendererFrontend.h>
#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/platform/VulkanContext.h>
#include <Container/renderer/scene/CameraController.h>
#include <Container/renderer/scene/GltfDeformationPass.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/scene/ScenePrimitives.h>
#include <Container/renderer/temporal/TemporalCapture.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/Platform.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {
using Graph = container::scene::SceneGraph;
using Tool = container::ui::ViewportTool;
using Space = container::ui::TransformSpace;
using Axis = container::ui::TransformAxis;

bool finiteAffine(const glm::mat4& value) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(value[column][row])) return false;
  return value[0].w == 0 && value[1].w == 0 && value[2].w == 0 &&
         value[3].w == 1;
}

glm::dmat3 invertBasis(const glm::dmat3& value) {
  const double determinant = glm::determinant(value);
  if (!std::isfinite(determinant) || determinant == 0)
    throw std::invalid_argument("The parent transform is singular");
  return glm::inverse(value);
}
}  // namespace

struct RendererFrontend::NodeGesture {
  struct Endpoints {
    NodePlacement first, last;
  };
  uint32_t node{};
  uint64_t graphIdentity{}, incarnation{}, widget{};
  std::shared_ptr<Endpoints> values;
  bool gizmo{}, cancelRequested{};
  container::util::EditHistory::Command command;
};

std::optional<RendererFrontend::NodePlacement> RendererFrontend::nodePlacement(
    uint32_t node) const {
  const auto* entry = sceneGraph_.getNode(node);
  const auto sibling = sceneGraph_.siblingIndex(node);
  if (!entry || !sibling) return std::nullopt;
  return NodePlacement{entry->localTransform, entry->parent, *sibling,
                       entry->visible};
}

bool RendererFrontend::nodeHasActivePoseTrack(uint32_t node) const {
  const auto* entry = sceneGraph_.getNode(node);
  if (!entry || entry->sourceNodeIndex < 0 || !gltfAnimation_ ||
      !gltfAnimation_->selectedClip())
    return false;
  const auto& clip =
      gltfAnimation_->library().clips[*gltfAnimation_->selectedClip()];
  using Kind = container::geometry::gltf::AnimationTargetKind;
  return std::ranges::any_of(clip.tracks, [&](const auto& track) {
    return track.target.index == static_cast<size_t>(entry->sourceNodeIndex) &&
           (track.target.kind == Kind::NodeTranslation ||
            track.target.kind == Kind::NodeRotation ||
            track.target.kind == Kind::NodeScale);
  });
}

RendererFrontend::NodeEditSelection RendererFrontend::nodeEditSelection()
    const {
  return {sceneState_.selectedMeshNode, selectedBimObjectIndex_,
          subs_.lightingManager
              ? subs_.lightingManager->selectedEditableLightId()
              : EditableLightId{},
          subs_.guiManager &&
              subs_.guiManager->sectionPlaneState().visualPlaneEditable};
}

void RendererFrontend::restoreNodeEditSelection(
    const NodeEditSelection& selection) {
  sceneState_.selectedMeshNode = sceneGraph_.getNode(selection.mesh)
                                     ? selection.mesh
                                     : Graph::kInvalidNode;
  selectedBimObjectIndex_ =
      subs_.bimManager && subs_.bimManager->metadataForObject(selection.bim)
          ? selection.bim
          : UINT32_MAX;
  if (subs_.lightingManager)
    subs_.lightingManager->selectEditableLight(selection.light);
  if (subs_.guiManager)
    subs_.guiManager->setSectionPlaneVisualEditable(selection.sectionEditable);
  selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();
  selectedBimNativeCurveDrawCommands_.clear();
}

struct RendererFrontend::PrimitiveCreation {
  uint64_t graphIdentity, parentIncarnation;
  uint32_t oldRoot, root, node, primitive;
  size_t rootSibling{}, sibling{};
  std::optional<container::scene::SceneNode> createdRoot;
  container::scene::SceneNode leaf;
  container::geometry::Mesh mesh;
  NodeEditSelection before, after;
};

bool RendererFrontend::recordPrimitiveCreation(ScenePrimitiveKind kind) {
  if (!subs_.sceneController || !subs_.sceneManager) return false;
  ensureSceneEditHistory();
  finishLightEditInput();
  finishNodeGesture();
  if (sceneGraph_.renderableNodes().size() >= svc_.config.maxSceneObjects)
    throw std::length_error("Reached maximum scene object capacity");

  auto edit = std::make_shared<PrimitiveCreation>();
  edit->graphIdentity = sceneGraph_.identity();
  edit->oldRoot = sceneState_.rootNode;
  edit->root = edit->oldRoot;
  edit->before = nodeEditSelection();
  const auto material = subs_.sceneManager->defaultMaterialIndex();
  const auto count = subs_.sceneManager->model().meshes().size();
  if (count >= Graph::kInvalidNode) throw std::length_error("Scene primitive indices exhausted");
  edit->primitive = static_cast<uint32_t>(count);
  edit->mesh = makeScenePrimitive(kind, material == UINT32_MAX ? -1 : static_cast<int32_t>(material));

  Graph candidate(sceneGraph_);
  if (edit->root == Graph::kInvalidNode) {
    edit->root = candidate.createNode(glm::mat4(1), material);
    edit->createdRoot = *candidate.getNode(edit->root);
    edit->rootSibling = *candidate.siblingIndex(edit->root);
  }
  edit->parentIncarnation = candidate.nodeIncarnation(edit->root);
  if (!edit->parentIncarnation) return false;
  const std::string name(scenePrimitiveKindLabel(kind));
  edit->node = candidate.createNode(glm::mat4(1), material, true, edit->primitive, name);
  if (!candidate.setParent(edit->node, edit->root)) return false;
  edit->leaf = *candidate.getNode(edit->node);
  edit->sibling = *candidate.siblingIndex(edit->node);
  edit->after = {edit->node, UINT32_MAX, {}, false};
  container::util::EditHistory::Command command{
      "Add " + name,
      [this, edit] { return replayPrimitiveCreation(*edit, false); },
      [this, edit] { return replayPrimitiveCreation(*edit, true); },
      true};
  publishPrimitiveGeometry(candidate, edit->root, edit->primitive, &edit->mesh, edit->after);
  editHistory_.record(std::move(command));
  return true;
}

bool RendererFrontend::replayPrimitiveCreation(const PrimitiveCreation& edit, bool append) {
  if (edit.graphIdentity != sceneGraph_.identity() ||
      sceneState_.rootNode != (append ? edit.oldRoot : edit.root) ||
      (!edit.createdRoot && sceneGraph_.nodeIncarnation(edit.root) != edit.parentIncarnation))
    return false;
  const auto meshCount = subs_.sceneManager->model().meshes().size();
  if (meshCount != size_t(edit.primitive) + (append ? 0u : 1u)) return false;
  Graph candidate(sceneGraph_);
  if (append) {
    if (candidate.renderableNodes().size() >= svc_.config.maxSceneObjects)
      return false;
    if (edit.createdRoot && !candidate.restoreLastLeaf(edit.root, *edit.createdRoot, edit.rootSibling))
      return false;
    if (!candidate.restoreLastLeaf(edit.node, edit.leaf, edit.sibling))
      return false;
  } else {
    const auto* leaf = candidate.getNode(edit.node);
    if (!leaf || leaf->primitiveIndex != edit.primitive || leaf->parent != edit.root ||
        candidate.siblingIndex(edit.node) != edit.sibling ||
        !candidate.removeLastLeaf(edit.node, edit.leaf.incarnation))
      return false;
    if (edit.createdRoot && !candidate.removeLastLeaf(edit.root, edit.parentIncarnation))
      return false;
  }
  publishPrimitiveGeometry(candidate, append ? edit.root : edit.oldRoot, edit.primitive,
                           append ? &edit.mesh : nullptr, append ? edit.after : edit.before);
  return true;
}

void RendererFrontend::publishPrimitiveGeometry(
    Graph& candidate, uint32_t root, uint32_t primitive,
    const container::geometry::Mesh* append, const NodeEditSelection& selection) {
  candidate.updateWorldTransforms();
  if (append) {
    const auto& transform = candidate.getNode(selection.mesh)->worldTransform;
    if (!finiteAffine(transform) || glm::determinant(glm::dmat3(transform)) == 0)
      throw std::invalid_argument("The primitive must have a finite, nonsingular world transform");
  }
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  auto source = subs_.sceneManager->prepareRuntimeMeshChange(primitive, append);
  auto geometry = subs_.sceneController->prepareGeometryEdit(
      candidate, source.model, source.bounds,
      subs_.guiManager && subs_.guiManager->showNormalDiagCube());

  std::unique_ptr<GltfDeformationPass> deformationPass;
  const auto* deformation = geometry.deformation();
  const auto vertices = geometry.gpuBindVertices();
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
      throw std::runtime_error("Cannot retire probe history for the geometry edit");
  }
  std::string resetReason = "procedural primitive changed";

  // Frame recording has not begun. Retired resources stay owned by the
  // prepared generation until the renderer has adopted the complete edit.
  sceneGraph_.exchangeNodeEdit(candidate);
  subs_.sceneManager->exchangeRuntimeMeshChange(source);
  subs_.sceneController->exchangeGeometryEdit(geometry);
  subs_.gltfDeformationPass.swap(deformationPass);
  gltfDeformationImageCount_ = imageCount;
  gltfDeformationSourceIdentity_ = subs_.sceneManager->animationSourceRevision();
  gltfDeformationSourceRevision_ = subs_.sceneManager->geometryRevision();
  gltfDeformationTopologyRevision_ = deformation ? deformation->topologyRevision() : 0;
  gltfDeformationVertexCount_ = vertices.size();
  gltfDeformationSubmittedEpoch_.reset();
  if (ddgi && !subs_.ddgiManager->publishSceneHistoryReset(*ddgi))
    std::terminate();
  if (subs_.temporalManager) subs_.temporalManager->reset(std::move(resetReason));
  sceneState_.rootNode = root;
  sceneState_.diagCubeObjectIndex = subs_.sceneController->diagCubeObjectIndex();
  syncSceneStateFromController();
  selectionNavigationAnchor_ = {};
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  invalidateDepthVisibilityFrames();
  restoreNodeEditSelection(selection);
}

bool RendererFrontend::publishNodePlacement(
    uint32_t node, uint64_t graphIdentity, uint64_t incarnation,
    NodeEditKind kind, const NodePlacement& expected,
    const NodePlacement& desired, const NodeEditSelection* selection) {
  if (graphIdentity != sceneGraph_.identity() ||
      incarnation != sceneGraph_.nodeIncarnation(node) ||
      !subs_.sceneController || !subs_.sceneManager)
    return false;
  const auto current = nodePlacement(node);
  if (!current) return false;
  switch (kind) {
    case NodeEditKind::Transform:
      if (current->local != expected.local ||
          current->parent != expected.parent)
        return false;
      break;
    case NodeEditKind::Visibility:
      if (current->visible != expected.visible) return false;
      break;
    case NodeEditKind::Reparent:
      if (current->parent != expected.parent ||
          current->sibling != expected.sibling ||
          current->local != expected.local)
        return false;
      break;
  }
  if (kind != NodeEditKind::Visibility && nodeHasActivePoseTrack(node))
    throw std::invalid_argument(
        "Clear the active glTF pose clip before changing this node's "
        "transform");
  if (kind != NodeEditKind::Visibility && !finiteAffine(desired.local))
    throw std::invalid_argument("The node transform must be finite and affine");

  Graph candidate(sceneGraph_);
  if (kind == NodeEditKind::Reparent) {
    const auto parent = desired.parent == Graph::kInvalidNode
                            ? std::nullopt
                            : std::optional(desired.parent);
    if (!candidate.setParentAt(node, parent, desired.sibling)) return false;
  }
  if (kind == NodeEditKind::Visibility)
    candidate.setVisible(node, desired.visible);
  else
    candidate.setLocalTransform(node, desired.local);
  candidate.updateWorldTransforms();
  // An edit can affect descendants even when its own matrix is finite.
  if (kind != NodeEditKind::Visibility) {
    std::vector<uint32_t> pending{node};
    while (!pending.empty()) {
      const auto index = pending.back();
      pending.pop_back();
      const auto& entry = *candidate.getNode(index);
      if (!finiteAffine(entry.worldTransform))
        throw std::invalid_argument(
            "The edited subtree has a nonfinite world transform");
      if (entry.renderable &&
          glm::determinant(glm::dmat3(entry.worldTransform)) == 0)
        throw std::invalid_argument(
            "The edited subtree has a singular renderable transform");
      pending.insert(pending.end(), entry.children.begin(),
                     entry.children.end());
    }
  }

  auto objects = subs_.sceneController->prepareObjectData(
      candidate, subs_.guiManager && subs_.guiManager->showNormalDiagCube());
  std::optional<container::scene::AuthoredSceneLights> lights;
  if (kind != NodeEditKind::Visibility)
    lights = subs_.sceneManager->evaluateAuthoredSceneLights(
        &candidate, sceneState_.rootNode);
  std::optional<DdgiManager::PreparedSceneHistoryReset> ddgi;
  if (subs_.ddgiManager && subs_.ddgiManager->supported()) {
    subs_.ddgiManager->synchronizeAsync();
    ddgi = subs_.ddgiManager->prepareSceneHistoryReset();
    if (!subs_.ddgiManager->canPublishSceneHistoryReset(*ddgi))
      throw std::runtime_error("Cannot retire probe history for the node edit");
  }
  std::string resetReason = "scene node edit or history";

  // The renderer thread owns the graph. Frame recording has yet to begin,
  // and GPU object buffers are filled by the regular retired-image path.
  sceneGraph_.exchangeNodeEdit(candidate);
  subs_.sceneController->exchangeObjectData(objects);
  if (lights) subs_.sceneManager->exchangeAuthoredSceneLights(*lights);
  if (ddgi && !subs_.ddgiManager->publishSceneHistoryReset(*ddgi))
    std::terminate();
  if (subs_.temporalManager)
    subs_.temporalManager->reset(std::move(resetReason));
  sceneState_.diagCubeObjectIndex =
      subs_.sceneController->diagCubeObjectIndex();
  selectionNavigationAnchor_ = {};
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  invalidateDepthVisibilityFrames();
  if (selection) restoreNodeEditSelection(*selection);
  return true;
}

bool RendererFrontend::recordNodeEdit(uint32_t node, NodeEditKind kind,
                                      const NodePlacement& before,
                                      const NodePlacement& after,
                                      std::string label) {
  ensureSceneEditHistory();
  finishLightEditInput();
  finishNodeGesture();
  const auto graph = sceneGraph_.identity();
  const auto birth = sceneGraph_.nodeIncarnation(node);
  if (before == after || !birth) return false;
  const auto selection = nodeEditSelection();
  return editHistory_.execute(
      {std::move(label),
       [=, this] {
         return publishNodePlacement(node, graph, birth, kind, after, before,
                                     &selection);
       },
       [=, this] {
         return publishNodePlacement(node, graph, birth, kind, before, after,
                                     &selection);
       },
       true});
}

bool RendererFrontend::editNodeVisibility(uint32_t node, bool visible) {
  const auto before = nodePlacement(node);
  if (!before) return false;
  auto after = *before;
  after.visible = visible;
  return recordNodeEdit(node, NodeEditKind::Visibility, *before, after,
                        "Node visibility");
}

bool RendererFrontend::reparentNode(uint32_t node,
                                    std::optional<uint32_t> parent) {
  if (node == sceneState_.rootNode) return false;
  const auto before = nodePlacement(node);
  if (!before || before->parent == parent.value_or(Graph::kInvalidNode))
    return false;
  const auto local = sceneGraph_.worldPreservingLocalTransform(node, parent);
  if (!local) return false;
  auto after = *before;
  after.local = *local;
  after.parent = parent.value_or(Graph::kInvalidNode);
  after.sibling = parent ? sceneGraph_.getNode(*parent)->children.size()
                         : sceneGraph_.rootNodes().size();
  return recordNodeEdit(node, NodeEditKind::Reparent, *before, after,
                        "Reparent node");
}

void RendererFrontend::beginNodeGesture(uint32_t node, uint64_t widget,
                                        bool gizmo) {
  ensureSceneEditHistory();
  finishLightEditInput();
  finishNodeGesture();
  const auto before = nodePlacement(node);
  if (!before)
    throw std::invalid_argument("The transform node is no longer available");
  if (nodeHasActivePoseTrack(node))
    throw std::invalid_argument(
        "Clear the active glTF pose clip before changing this node's "
        "transform");
  auto gesture = std::make_shared<NodeGesture>();
  gesture->node = node;
  gesture->graphIdentity = sceneGraph_.identity();
  gesture->incarnation = sceneGraph_.nodeIncarnation(node);
  gesture->widget = widget;
  gesture->gizmo = gizmo;
  // The closures are prepared before any published preview. Capturing their
  // own gesture would create a cycle, so store playback state separately.
  gesture->values = std::make_shared<NodeGesture::Endpoints>(
      NodeGesture::Endpoints{*before, *before});
  const auto values = gesture->values;
  const auto graph = gesture->graphIdentity;
  const auto birth = gesture->incarnation;
  const auto selection = nodeEditSelection();
  gesture->command = container::util::EditHistory::Command{
      "Transform node",
      [=, this] {
        return publishNodePlacement(node, graph, birth, NodeEditKind::Transform,
                                    values->last, values->first, &selection);
      },
      [=, this] {
        return publishNodePlacement(node, graph, birth, NodeEditKind::Transform,
                                    values->first, values->last, &selection);
      },
      true};
  nodeGesture_ = std::move(gesture);
}

bool RendererFrontend::previewNodePose(const glm::mat4& local) {
  if (!nodeGesture_) return false;
  auto& gesture = *nodeGesture_;
  if (gesture.cancelRequested) {
    finishNodeGesture(true);
    return false;
  }
  auto& last = gesture.values->last;
  auto next = last;
  next.local = local;
  if (next == last) return false;
  if (!publishNodePlacement(gesture.node, gesture.graphIdentity,
                            gesture.incarnation, NodeEditKind::Transform, last,
                            next))
    throw std::invalid_argument(
        "The transform target changed during this gesture");
  last = next;
  return true;
}

void RendererFrontend::finishNodeGesture(bool cancel) {
  if (!nodeGesture_) return;
  auto& gesture = *nodeGesture_;
  gesture.cancelRequested |= cancel;
  const auto& [first, last] = *gesture.values;
  if (last != first) {
    if (gesture.cancelRequested) {
      if (!gesture.command.undo())
        throw std::invalid_argument(
            "The transform target changed before cancellation");
    } else {
      const auto current = nodePlacement(gesture.node);
      if (!current || current->local != last.local ||
          current->parent != last.parent)
        throw std::invalid_argument(
            "The transform target changed before release");
      editHistory_.record(std::move(gesture.command));
    }
  }
  if (gesture.gizmo) transformDragSession_.active = false;
  nodeGesture_.reset();
}

bool RendererFrontend::nodeGizmoActive() const noexcept {
  return nodeGesture_ && nodeGesture_->gizmo;
}

void RendererFrontend::applyNodeGizmoControls(
    Tool tool, Axis axis, const container::ui::TransformControls& controls) {
  if (!nodeGizmoActive()) return;
  const auto& gesture = *nodeGesture_;
  const auto& drag = transformDragSession_;
  const auto& first = gesture.values->first;
  auto local = first.local;
  const auto* parent = sceneGraph_.getNode(first.parent);
  const glm::dmat3 basis =
      parent ? glm::dmat3(parent->worldTransform) : glm::dmat3(1);
  if (tool == Tool::Translate) {
    const auto delta = controls.position - drag.startControls.position;
    if (delta != glm::vec3(0))
      local[3] +=
          glm::vec4(glm::vec3(invertBasis(basis) * glm::dvec3(delta)), 0);
  } else if (tool == Tool::Rotate) {
    const auto delta =
        controls.rotationDegrees - drag.startControls.rotationDegrees;
    if (delta != glm::vec3(0)) {
      glm::dmat4 rotation(1);
      const auto rotate = [&](float degrees, glm::vec3 direction) {
        if (degrees != 0)
          rotation = glm::rotate(glm::dmat4(1), glm::radians(double(degrees)),
                                 glm::dvec3(direction)) *
                     rotation;
      };
      if (axis == Axis::Free || axis == Axis::Y) rotate(delta.y, drag.axisY);
      if (axis == Axis::Free || axis == Axis::X) rotate(delta.x, drag.axisX);
      if (axis == Axis::Z) rotate(delta.z, drag.axisZ);
      const glm::mat3 next(invertBasis(basis) * glm::dmat3(rotation) * basis *
                           glm::dmat3(local));
      for (int column = 0; column < 3; ++column)
        local[column] = glm::vec4(next[column], 0);
    }
  } else if (tool == Tool::Scale &&
             controls.scale != drag.startControls.scale) {
    const auto factor = controls.scale / drag.startControls.scale;
    if (drag.space == Space::Local) {
      for (int column = 0; column < 3; ++column)
        local[column] *= factor[column];
    } else {
      const glm::dmat3 scale(factor.x, 0, 0, 0, factor.y, 0, 0, 0, factor.z);
      const glm::mat3 next(invertBasis(basis) * scale * basis *
                           glm::dmat3(local));
      for (int column = 0; column < 3; ++column)
        local[column] = glm::vec4(next[column], 0);
    }
  }
  (void)previewNodePose(local);
}

void RendererFrontend::processNodeEditRequests() {
  if (!subs_.guiManager) return;
  try {
    if (subs_.guiManager->consumeNodeTransformCancel()) {
      if (nodeGesture_ && !nodeGesture_->gizmo) finishNodeGesture(true);
      pendingNodeEdits_.clear();
      return;
    }
    auto requests = std::exchange(pendingNodeEdits_, {});
    for (const auto& edit : requests) {
      if (edit.kind == NodeEditKind::Visibility) {
        (void)editNodeVisibility(edit.node, edit.visible);
      } else if (edit.kind == NodeEditKind::Reparent) {
        if (!reparentNode(edit.node, edit.parent))
          subs_.guiManager->setStatusMessage(
              "Cannot reparent the selected node");
      } else if (const auto local =
                     subs_.cameraController->previewNodeTransform(
                         edit.node, edit.controls)) {
        if (edit.widget) {
          if (!nodeGesture_ || nodeGesture_->gizmo ||
              nodeGesture_->node != edit.node ||
              nodeGesture_->widget != *edit.widget)
            beginNodeGesture(edit.node, *edit.widget, false);
          (void)previewNodePose(*local);
        } else if (const auto before = nodePlacement(edit.node)) {
          auto after = *before;
          after.local = *local;
          (void)recordNodeEdit(edit.node, NodeEditKind::Transform, *before,
                               after, "Transform node");
        }
      }
    }
    if (nodeGesture_ && !nodeGesture_->gizmo &&
        (!subs_.guiManager->nodeTransformGestureActive() ||
         subs_.guiManager->nodeTransformWidget() != nodeGesture_->widget))
      finishNodeGesture();
  } catch (const std::exception& error) {
    subs_.guiManager->setStatusMessage(error.what());
  }
}

void RendererFrontend::processCaptureNodeEdit() {
  const auto edit = std::exchange(captureNodeEdit_, {});
  if (!edit) return;
  ensureSceneEditHistory();
  const auto resolve = [this](uint32_t source) {
    for (uint32_t node = 0; node < sceneGraph_.nodeCount(); ++node) {
      const auto& entry = *sceneGraph_.getNode(node);
      if (entry.sourceNodeIndex >= 0 &&
          uint32_t(entry.sourceNodeIndex) == source)
        return node;
    }
    throw std::invalid_argument(
        "Capture node is outside the active glTF scene");
  };
  const auto node = edit->graphNode ? *edit->graphNode : resolve(edit->sourceNode);
  if (!sceneGraph_.getNode(node))
    throw std::invalid_argument("Capture graph node is outside the active scene");
  captureNodeTelemetry_ = node;
  using Operation = container::temporal::CaptureNodeEdit::Operation;
  switch (edit->operation) {
    case Operation::Select:
      finishLightEditInput();
      finishNodeGesture();
      if (subs_.lightingManager) subs_.lightingManager->selectEditableLight({});
      sceneState_.selectedMeshNode = node;
      return;
    case Operation::Transform: {
      const auto before = nodePlacement(node);
      auto after = *before;
      after.local = edit->matrix;
      if (!recordNodeEdit(node, NodeEditKind::Transform, *before, after,
                          "Transform node"))
        throw std::invalid_argument("Capture transform could not be applied");
      return;
    }
    case Operation::Visibility:
      if (!editNodeVisibility(node, edit->visible))
        throw std::invalid_argument("Capture visibility could not be applied");
      return;
    case Operation::Reparent:
      if (!reparentNode(node, edit->parent
                                  ? std::optional(resolve(*edit->parent))
                                  : std::nullopt))
        throw std::invalid_argument("Capture reparent could not be applied");
      return;
    case Operation::Begin:
      if (subs_.lightingManager) subs_.lightingManager->selectEditableLight({});
      sceneState_.selectedMeshNode = node;
      using CaptureGizmo = container::temporal::CaptureLightGizmo;
      interactionController_.setTool(
          edit->tool == CaptureGizmo::Tool::Translate ? Tool::Translate
          : edit->tool == CaptureGizmo::Tool::Rotate  ? Tool::Rotate
                                                      : Tool::Scale);
      interactionController_.setTransformSpace(
          edit->space == CaptureGizmo::Space::Local ? Space::Local
                                                    : Space::World);
      interactionController_.setTransformAxis(
          edit->axis == CaptureGizmo::Axis::X   ? Axis::X
          : edit->axis == CaptureGizmo::Axis::Y ? Axis::Y
          : edit->axis == CaptureGizmo::Axis::Z ? Axis::Z
                                                : Axis::Free);
      interactionController_.setTransformSnapEnabled(edit->snap);
      beginNodeGesture(node, 0, true);
      break;
    case Operation::Update:
    case Operation::End:
    case Operation::Cancel:
      if (!nodeGizmoActive() || nodeGesture_->node != node)
        throw std::invalid_argument("Capture node gizmo is not active");
      if (edit->operation != Operation::Update) {
        finishNodeGesture(edit->operation == Operation::Cancel);
        return;
      }
      break;
  }
  const auto& viewport = interactionController_.state();
  transformSelectedNodeByDrag(
      viewport.tool, viewport.transformSpace, viewport.transformAxis,
      viewport.transformSnapEnabled, edit->delta.x, edit->delta.y);
}

void RendererFrontend::appendNodeEditTelemetry(nlohmann::json& output) const {
  if (!captureNodeTelemetry_) return;
  const auto node = *captureNodeTelemetry_;
  const auto* entry = sceneGraph_.getNode(node);
  if (!entry) return;
  const auto matrix = [](const glm::mat4& value) {
    auto result = nlohmann::json::array();
    for (const auto column : {0, 1, 2, 3})
      for (const auto row : {0, 1, 2, 3}) result.push_back(value[column][row]);
    return result;
  };
  output["nodeEdit"] = {
      {"graphNode", node},
      {"sourceNode", entry->sourceNodeIndex},
      {"local", matrix(entry->localTransform)},
      {"world", matrix(entry->worldTransform)},
      {"parent", entry->parent},
      {"sibling", *sceneGraph_.siblingIndex(node)},
      {"visible", entry->visible},
      {"effectiveVisible", sceneGraph_.isNodeEffectivelyVisible(node)},
      {"selected", sceneState_.selectedMeshNode == node},
      {"selectedGraphNode", sceneState_.selectedMeshNode},
      {"gizmoActive", nodeGizmoActive()}};
}
}  // namespace container::renderer
