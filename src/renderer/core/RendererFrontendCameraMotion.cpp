#include "Container/renderer/core/RendererFrontend.h"

#include "Container/geometry/GltfAnimationRuntime.h"
#include "Container/geometry/GltfCameraMotion.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace container::renderer {
namespace {
using namespace container::geometry::gltf;
using container::ui::CameraMotionAction;

bool capturesView(CameraMotionAction action) {
  return action == CameraMotionAction::CreateClip || action == CameraMotionAction::CaptureKey;
}

}  // namespace

struct RendererFrontend::CameraMotionHistory {
  container::scene::CameraViewPoseTarget target;
  size_t clip{};
  std::optional<size_t> beforeSelection;
  double beforeTime{}, afterTime{};
  std::optional<GltfCameraMotionClip> before, after;
  std::optional<tinygltf::Node> matrixRest, trsRest;
};

void RendererFrontend::refreshCameraMotionInspector() {
  if (!subs_.guiManager) return;
  auto& controls = subs_.guiManager->gltfAnimationControls();
  if (!controls.cameraViewPose || !subs_.sceneManager || !gltfAnimation_) {
    (void)container::ui::SetCameraMotionSnapshot(controls, std::nullopt);
    return;
  }
  const auto& pose = *controls.cameraViewPose;
  container::ui::CameraMotionSnapshot snapshot;
  snapshot.target = pose.target;
  snapshot.viewRevision = pose.viewRevision;
  snapshot.destinationName = pose.destinationName;
  snapshot.viewportActive = pose.viewportActive;
  try {
    if (auto selected = gltfAnimation_->selectedClip()) {
      auto clip = ReadGltfCameraMotion(subs_.sceneManager->animationSource(), *selected);
      if (clip && clip->sourceNode == static_cast<uint32_t>(pose.target.camera.sourceNode)) {
        snapshot.clip = static_cast<int>(*selected);
        snapshot.keyTimes.reserve(clip->keys.size());
        for (const auto& key : clip->keys) snapshot.keyTimes.push_back(key.time);
      }
    }
    if (subs_.sceneManager->isLinkedNodeReadOnly(sceneGraph_, pose.target.camera.graphNode))
      snapshot.blockReason = "Make the linked camera editable before authoring motion.";
    if (!container::ui::SetCameraMotionSnapshot(controls, std::move(snapshot)))
      throw std::runtime_error("Camera motion controls could not display these keys");
  } catch (const std::exception& error) {
    (void)container::ui::SetCameraMotionSnapshot(controls, std::nullopt);
    controls.cameraMotionError = error.what();
  }
}

void RendererFrontend::processCameraMotionRequests() {
  if (!subs_.guiManager) return;
  auto& controls = subs_.guiManager->gltfAnimationControls();
  auto request = std::exchange(controls.requestedCameraMotion, {});
  if (!request) return;
  try {
    if (controls.requestedCameraNode || controls.requestedClip || controls.requestedTime ||
        controls.requestedSpeed || controls.requestedLoop || controls.requestedPlaying)
      throw std::invalid_argument("Apply camera selection or playback changes before editing motion");
    if (request->action != CameraMotionAction::CreateClip &&
        gltfAnimation_ && gltfAnimation_->selectedClip() != size_t(request->clip))
      throw std::invalid_argument("The selected camera motion clip changed before the edit");
    authorCameraMotion(*request);
    controls.cameraMotionError.clear();
  } catch (const std::exception& error) {
    controls.cameraMotionError = error.what();
  }
}

void RendererFrontend::applyCaptureCameraMotion(const container::temporal::CaptureCameraMotion& event) {
  if (!subs_.sceneManager) throw std::runtime_error("Camera motion requires an active glTF scene");
  if (!gltfAnimation_ || gltfAnimationGraphIdentity_ != sceneGraph_.identity()) resetGltfAnimation();
  const auto graphNode = cameraGraphNode(static_cast<size_t>(event.sourceNode));
  const auto target = subs_.sceneManager->cameraViewPoseTarget(sceneGraph_, graphNode);
  if (!target) throw std::invalid_argument("Camera motion requires an active authored camera");
  using Operation = container::temporal::CaptureCameraMotion::Operation;
  CameraMotionAction action;
  switch (event.operation) {
  case Operation::Create: action = CameraMotionAction::CreateClip; break;
  case Operation::Capture: action = CameraMotionAction::CaptureKey; break;
  case Operation::Move: action = CameraMotionAction::MoveKey; break;
  case Operation::Delete: action = CameraMotionAction::DeleteKey; break;
  default: throw std::invalid_argument("Unknown camera motion operation");
  }
  const uint64_t viewRevision = capturesView(action) ? refreshCameraViewPoseViewRevision() : 0;
  authorCameraMotion({action, *target, viewRevision, event.clip, event.time, event.newTime});
}

void RendererFrontend::authorCameraMotion(const container::ui::CameraMotionRequest& request) {
  if (renderSetupPreview_) throw std::invalid_argument("Deactivate the render setup before editing camera motion");
  if (!subs_.sceneManager || !gltfAnimation_ || gltfAnimationGraphIdentity_ != sceneGraph_.identity())
    throw std::invalid_argument("The camera motion scene is unavailable");
  auto& manager = *subs_.sceneManager;
  const auto current = manager.cameraViewPoseTarget(sceneGraph_, request.target.camera.graphNode);
  if (!current || *current != request.target || manager.isLinkedNodeReadOnly(sceneGraph_, current->camera.graphNode))
    throw std::invalid_argument("The camera motion destination changed or is read-only");
  if (gltfAnimation_->playing() || materialGesture_ || lightEditGestureActive() || transformDragSession_.active ||
      sceneEditPublicationPending_ || pendingSceneRetirement_)
    throw std::invalid_argument("Pause animation and finish the current scene edit before editing camera motion");

  const auto source = static_cast<uint32_t>(current->camera.sourceNode);
  const auto& model = manager.animationSource();
  const bool creating = request.action == CameraMotionAction::CreateClip;
  if (!creating && request.clip < 0) throw std::invalid_argument("Select a camera motion clip");
  auto history = std::make_shared<CameraMotionHistory>();
  history->target = *current;
  history->clip = creating ? model.animations.size() : static_cast<size_t>(request.clip);
  history->beforeSelection = gltfAnimation_->selectedClip();
  history->beforeTime = gltfAnimation_->time();
  history->before = ReadGltfCameraMotion(model, history->clip);
  if (!creating && (!history->before || history->before->sourceNode != source))
    throw std::invalid_argument("The clip is not a motion clip for this camera");

  std::optional<GltfCameraMotionPose> pose;
  if (capturesView(request.action)) {
    if (gltfAnimationCameraNode_ || request.viewRevision != refreshCameraViewPoseViewRevision())
      throw std::invalid_argument("Use the current free viewport to capture a camera key");
    const auto extent = svc_.swapChainManager.extent();
    if (!extent.width || !extent.height) throw std::invalid_argument("Camera motion requires a nonzero viewport");
    const auto destinationClip = creating ? std::nullopt : std::optional(history->clip);
    pose = manager.captureCameraMotionPose(*current, sceneGraph_, cameraViewportUnjitteredView(),
        float(extent.width) / float(extent.height), *gltfAnimation_, destinationClip,
        creating ? 0.0f : static_cast<float>(request.time));
  }
  std::string label;
  switch (request.action) {
  case CameraMotionAction::CreateClip:
    label = "Create camera motion clip";
    history->after = GltfCameraMotionClip{source, "Camera motion " + std::to_string(history->clip + 1), {{0.0f, *pose}}};
    history->matrixRest = manager.cameraMotionMatrixRest(source);
    if (history->matrixRest)
      history->trsRest = CameraPoseAsTrs(EvaluateAnimatedNodeLocalTransform(*history->matrixRest), *history->matrixRest);
    break;
  case CameraMotionAction::CaptureKey:
    label = "Capture camera key";
    history->after = EditGltfCameraMotionKey(*history->before, GltfCameraMotionKeyAction::Capture, request.time, {}, pose);
    history->afterTime = static_cast<float>(request.time);
    break;
  case CameraMotionAction::MoveKey:
    label = "Move camera key";
    history->after = EditGltfCameraMotionKey(*history->before, GltfCameraMotionKeyAction::Move, request.time, request.newTime);
    history->afterTime = static_cast<float>(request.newTime);
    break;
  case CameraMotionAction::DeleteKey:
    label = "Delete camera key";
    history->after = EditGltfCameraMotionKey(*history->before, GltfCameraMotionKeyAction::Delete, request.time);
    history->afterTime = history->beforeTime;
    break;
  default:
    throw std::invalid_argument("Unknown camera motion edit");
  }
  if (history->before == history->after) return;

  ensureSceneEditHistory();
  container::util::EditHistory::Command command{std::move(label),
      [this, history] { return replayCameraMotion(*history, false); },
      [this, history] { return replayCameraMotion(*history, true); }, true};
  if (!replayCameraMotion(*history, true)) throw std::runtime_error("The camera motion clip could not be published");
  sceneEdits_->recordAppliedPublished(std::move(command));
}

bool RendererFrontend::replayCameraMotion(const CameraMotionHistory& history, bool apply) {
  if (!subs_.sceneManager || !gltfAnimation_ || sceneGraph_.identity() != history.target.camera.graphIdentity)
    return false;
  if (materialGesture_ || lightEditGestureActive() || transformDragSession_.active ||
      sceneEditPublicationPending_ || pendingSceneRetirement_)
    throw std::invalid_argument("Finish the current scene edit before replaying camera motion");
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  const auto& rest = apply ? history.trsRest : history.matrixRest;
  const auto dirty = subs_.sceneManager->publishCameraMotionEdit(gltfAnimation_, history.target, sceneGraph_, history.clip,
      apply ? std::optional<size_t>(history.clip) : history.beforeSelection,
      apply ? history.afterTime : history.beforeTime, apply ? history.before : history.after,
      apply ? history.after : history.before, rest ? &*rest : nullptr);
  if (!dirty.empty()) publishGltfAnimationChanges(dirty);
  if (subs_.temporalManager) subs_.temporalManager->reset("glTF camera motion edit");
  if (subs_.guiManager) {
    auto& controls = subs_.guiManager->gltfAnimationControls();
    const auto& clips = gltfAnimation_->library().clips;
    controls.clips.clear();
    controls.clips.reserve(clips.size());
    for (size_t i = 0; i < clips.size(); ++i)
      controls.clips.push_back({clips[i].name.empty() ? "Clip " + std::to_string(i + 1) : clips[i].name, clips[i].duration});
    controls.selectedClip = gltfAnimation_->selectedClip() ? static_cast<int>(*gltfAnimation_->selectedClip()) : -1;
    controls.time = gltfAnimation_->time();
    controls.playing = gltfAnimation_->playing();
  }
  return true;
}

void RendererFrontend::appendCameraMotionTelemetry(nlohmann::json& animation) const {
  auto& motions = animation["cameraMotion"] = nlohmann::json::array();
  const auto& source = subs_.sceneManager->animationSource();
  for (size_t clip = 0; clip < source.animations.size(); ++clip) {
    std::optional<GltfCameraMotionClip> motion;
    try {
      motion = ReadGltfCameraMotion(source, clip);
    } catch (const std::invalid_argument&) {
      // A foreign or malformed editor marker must not prevent ordinary captures.
      continue;
    }
    if (!motion) continue;
    auto times = nlohmann::json::array();
    for (const auto& key : motion->keys) times.push_back(key.time);
    motions.push_back({{"clip", clip}, {"sourceNode", motion->sourceNode}, {"keyTimes", std::move(times)}});
  }
}

}  // namespace container::renderer
