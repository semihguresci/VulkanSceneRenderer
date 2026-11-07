#pragma once

#include <Container/geometry/GltfCameraAuthoring.h>
#include <Container/utility/CameraProjectionTarget.h>
#include <Container/utility/CameraViewPoseTarget.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace container::ui {
struct GltfAnimationClipChoice { std::string name; double duration{}; };
struct GltfAnimationCameraChoice { std::string name; size_t sourceNode{}; };
struct CameraProjectionSnapshot {
  container::scene::CameraProjectionTarget target;
  container::geometry::gltf::GltfCameraProjection type{};
  // GltfCameraProperty order. Zero aspect/far mean the authored omission;
  // viewport-derived aspect and culling fallbacks never enter this snapshot.
  std::array<double,8> values{};
  uint32_t sharedDefinitionUses{1};
  std::array<bool,8> animatedProperties{};
  std::array<std::string,8> propertyBlockReasons;
  std::string typeSwitchBlockReason;
};
struct CameraProjectionDraft {
  container::scene::CameraProjectionTarget target;
  container::geometry::gltf::GltfCameraProjection type{};
  std::array<double,8> values{};
  std::array<bool,8> edited{};
  bool typeEdited{};
};
struct CameraProjectionEditRequest {
  container::scene::CameraProjectionTarget target;
  container::geometry::gltf::GltfCameraProjectionPatch patch;
};
struct CameraViewPoseSnapshot {
  container::scene::CameraViewPoseTarget target;
  uint64_t viewRevision{};
  bool viewportActive{};
  std::string destinationName;
  std::string blockReason;
};
struct CameraViewPoseRequest {
  container::scene::CameraViewPoseTarget target;
  uint64_t viewRevision{};
};
struct GltfAnimationControls {
  std::vector<GltfAnimationClipChoice> clips;
  std::vector<GltfAnimationCameraChoice> cameras;
  int selectedClip{-1};
  int cameraNode{-1};
  double time{};
  double speed{1};
  bool playing{};
  bool loop{true};
  std::string error;
  std::vector<std::string> diagnostics;
  std::optional<int> requestedClip;
  std::optional<int> requestedCameraNode;
  std::optional<double> requestedTime;
  std::optional<double> requestedSpeed;
  std::optional<bool> requestedPlaying;
  std::optional<bool> requestedLoop;
  bool interactivityAvailable{};
  bool interactivityPlaying{};
  bool requestedInteractivityReset{};
  std::optional<bool> requestedInteractivityPlaying;
  std::optional<std::string> requestedInteractivityEvent;
  std::vector<std::string> interactivityEvents;
  std::string interactivityDiagnostic;
  size_t selectedInteractivityEvent{};
  double interactivityTime{};
  // Selected-record UI snapshots and weak intent only: no model, scene manager,
  // GPU resource or historical camera record is retained by these controls.
  std::optional<CameraProjectionSnapshot> cameraProjection;
  CameraProjectionDraft cameraProjectionDraft;
  std::optional<CameraProjectionEditRequest> requestedCameraProjection;
  std::string cameraProjectionError;
  // Independent weak destination: the frontend retains it while the active
  // view switches to free navigation. No source record or view matrix is owned.
  std::optional<CameraViewPoseSnapshot> cameraViewPose;
  std::optional<CameraViewPoseRequest> requestedCameraViewPose;
  std::string cameraViewPoseError;
};
// Finite selected snapshot admission. Invalid input or allocation failure leaves
// all previous controls intact. None clears the authored-camera editor. Dirty
// fields survive unrelated evaluated/graph updates of the same camera/lens;
// actual asset/definition/lens-revision changes reset the draft and queued edit.
[[nodiscard]] bool SetCameraProjectionSnapshot(GltfAnimationControls&,
    const std::optional<CameraProjectionSnapshot>&);
[[nodiscard]] bool SetCameraProjectionDraft(GltfAnimationControls&,const CameraProjectionDraft&) noexcept;
// One pending explicit Apply action. Duplicate properties, stale targets,
// blocked/invalid fields and concurrent camera/timeline requests reject without
// replacing a pending request. Backend preparation must revalidate the target.
[[nodiscard]] bool QueueCameraProjectionEdit(GltfAnimationControls&,
    const container::scene::CameraProjectionTarget&,
    const container::geometry::gltf::GltfCameraProjectionPatch&);
[[nodiscard]] bool QueueCameraProjectionDraft(GltfAnimationControls&);
// Finite, selected-only preview. Refresh never retargets a queued request;
// None clears the destination, pending pose request and its separate error.
// Invalid input/allocation failure preserves all previous controls.
[[nodiscard]] bool SetCameraViewPoseSnapshot(GltfAnimationControls&,
    const std::optional<CameraViewPoseSnapshot>&);
// One explicit write intent, valid only in free viewport mode against the
// exact current destination and unjittered-view revision. Backend preparation
// must independently prove the source node, view and publication context.
[[nodiscard]] bool QueueCameraViewPose(GltfAnimationControls&,
    const container::scene::CameraViewPoseTarget&,uint64_t viewRevision) noexcept;
// Draws inside the existing animation controls; also usable by headless widget
// tests without a platform window or a renderer.
void DrawCameraProjectionControls(GltfAnimationControls&);
void DrawCameraViewPoseControls(GltfAnimationControls&);
void DrawGltfAnimationControls(GltfAnimationControls& controls);
} // namespace container::ui
