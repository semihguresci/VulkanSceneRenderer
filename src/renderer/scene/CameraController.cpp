#include "Container/renderer/scene/CameraController.h"
#include "Container/ecs/World.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

namespace container::renderer {

using container::gpu::CameraData;

namespace {

container::ui::TransformControls decomposeTransform(const glm::mat4 &matrix) {
  container::ui::TransformControls controls{};
  controls.position = glm::vec3(matrix[3]);

  glm::vec3 basisX = glm::vec3(matrix[0]);
  glm::vec3 basisY = glm::vec3(matrix[1]);
  glm::vec3 basisZ = glm::vec3(matrix[2]);
  controls.scale =
      glm::vec3(glm::length(basisX), glm::length(basisY), glm::length(basisZ));
  controls.scale = glm::max(controls.scale, glm::vec3(0.001f));

  glm::mat3 rotationMatrix{};
  rotationMatrix[0] = basisX / controls.scale.x;
  rotationMatrix[1] = basisY / controls.scale.y;
  rotationMatrix[2] = basisZ / controls.scale.z;
  if (glm::determinant(rotationMatrix) < 0.0f) {
    controls.scale.x *= -1.0f;
    rotationMatrix[0] *= -1.0f;
  }

  controls.rotationDegrees = glm::degrees(
      glm::eulerAngles(glm::normalize(glm::quat_cast(rotationMatrix))));
  return controls;
}

glm::mat4 composeTransform(const container::ui::TransformControls &controls) {
  const glm::vec3 safeScale = glm::max(controls.scale, glm::vec3(0.001f));
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), controls.position);
  transform *=
      glm::mat4_cast(glm::quat(glm::radians(controls.rotationDegrees)));
  transform = glm::scale(transform, safeScale);
  return transform;
}

float selectedNodeRadius(const container::scene::SceneManager *sceneManager) {
  if (sceneManager && sceneManager->modelBounds().valid) {
    return std::max(0.25f, sceneManager->modelBounds().radius * 0.10f);
  }
  return 1.0f;
}

CameraController::SceneViewBounds sceneViewBoundsFromSceneManager(
    const container::scene::SceneManager *sceneManager) {
  if (sceneManager == nullptr || !sceneManager->modelBounds().valid) {
    return {};
  }

  const auto &bounds = sceneManager->modelBounds();
  return {
      .min = bounds.min,
      .max = bounds.max,
      .valid = true,
  };
}

void copyCameraPose(container::scene::BaseCamera &target,
                    const container::scene::BaseCamera &source) {
  target.setPosition(source.position());
  target.setYawPitch(source.yawDegrees(), source.pitchDegrees());
  target.setScale(source.scale());
}

struct CameraAngles {
  float yawDegrees{0.0f};
  float pitchDegrees{0.0f};
};

CameraAngles cameraAnglesFromForward(glm::vec3 forward) {
  const float length = glm::length(forward);
  if (!std::isfinite(length) || length <= 0.0001f) {
    return {};
  }
  forward /= length;
  return {glm::degrees(std::atan2(-forward.z, forward.x)),
          glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)))};
}

float smoothStep(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float lerpAngleDegrees(float from, float to, float t) {
  const float delta = std::remainder(to - from, 360.0f);
  return from + delta * t;
}

} // namespace

CameraController::CameraController(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager &allocationManager,
    container::gpu::SwapChainManager &swapChainManager,
    container::scene::SceneGraph &sceneGraph,
    container::scene::SceneManager *sceneManager,
    container::renderer::SceneController *sceneController,
    container::ecs::World &world,
    container::window::InputManager &inputManager)
    : device_(std::move(device)), allocationManager_(allocationManager),
      swapChainManager_(swapChainManager), sceneGraph_(sceneGraph),
      sceneManager_(sceneManager), sceneController_(sceneController),
      world_(world), inputManager_(inputManager) {}

void CameraController::createCamera() {
  auto perspective = std::make_unique<container::scene::PerspectiveCamera>();
  camera_ = std::move(perspective);
  inputManager_.setCamera(camera_.get());
  inputManager_.setMouseSensitivity(0.10f);
  resetCameraForScene();
}

void CameraController::resetCameraForScene() {
  resetCameraForBounds(sceneViewBoundsFromSceneManager(sceneManager_));
}

void CameraController::resetCameraForBounds(const SceneViewBounds &bounds) {
  if (!camera_)
    return;
  cancelViewAnimation();
  camera_->setScale(glm::vec3(1.0f));
  inputManager_.setMoveSpeed(kDefaultMoveSpeed);

  float farPlane = kDefaultFarPlane;
  bool hasBounds = false;
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius = 1.0f;
  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
  glm::vec3 boundsSize{0.0f};

  if (bounds.valid) {
    boundsMin = glm::min(bounds.min, bounds.max);
    boundsMax = glm::max(bounds.min, bounds.max);
    boundsSize = boundsMax - boundsMin;
    boundsCenter = (boundsMin + boundsMax) * 0.5f;
    const float computedRadius = glm::length(boundsSize) * 0.5f;
    if (std::isfinite(computedRadius) && computedRadius > 0.0f) {
      hasBounds = true;
      boundsRadius = std::max(0.25f, computedRadius);
    }
  }

  if (hasBounds) {
    farPlane =
        std::max(farPlane, glm::length(boundsCenter) + boundsRadius * 4.0f);
  }
  setCameraNearFar(kDefaultNearPlane, farPlane);

  if (hasBounds) {
    // For hall-like scenes (e.g. Sponza), start inside the volume looking down
    // the longest horizontal axis. For roughly cubic scenes, fall back to the
    // default three-quarter overview.
    const bool xIsLong = boundsSize.x >= boundsSize.z;
    const float longExtent = xIsLong ? boundsSize.x : boundsSize.z;
    const float crossExtent = xIsLong ? boundsSize.z : boundsSize.x;
    const bool hallLike =
        longExtent > 1.25f * crossExtent && longExtent > 1.5f * boundsSize.y;

    if (hallLike) {
      // RH: front.x = cos(yaw)cos(pitch), front.z = -sin(yaw)cos(pitch).
      // Look +X => yaw = 0; -X => 180; -Z => 90; +Z => -90.
      const float yaw = xIsLong ? 180.0f : 90.0f;
      camera_->setYawPitch(yaw, 0.0f);
      const float eyeHeight = boundsMin.y + boundsSize.y * 0.32f;
      const float endInset = longExtent * 0.10f;
      glm::vec3 eye = boundsCenter;
      eye.y = eyeHeight;
      if (xIsLong) {
        eye.x = boundsMax.x - endInset;
      } else {
        eye.z = boundsMax.z - endInset;
      }
      camera_->setPosition(eye);
    } else {
      camera_->setYawPitch(kDefaultYaw, kDefaultPitch);
      const float distance = boundsRadius * 2.5f + kDefaultNearPlane;
      const glm::vec3 front = camera_->frontVector();
      camera_->setPosition(boundsCenter - front * distance);
    }
    inputManager_.setMoveSpeed(
        std::max(kDefaultMoveSpeed,
                 hallLike ? longExtent * 0.08f : boundsRadius * 0.5f));
  } else {
    camera_->setYawPitch(90.0f, 0.0f);
    camera_->setPosition(glm::vec3(0.0f, 0.0f, 3.0f));
  }

  syncOrthographicViewHeight(
      ViewPivot{boundsCenter, std::max(boundsRadius, 0.25f), hasBounds});
}

CameraController::ViewPivot
CameraController::resolveViewPivot(uint32_t nodeIndex) const {
  if (selectionPivotOverride_ && selectionPivotOverride_->valid) {
    return ViewPivot{selectionPivotOverride_->center,
                     std::max(selectionPivotOverride_->radius, 0.25f), true};
  }

  if (nodeIndex != container::scene::SceneGraph::kInvalidNode) {
    if (sceneController_) {
      const uint64_t sceneRevision = sceneGraph_.revision();
      const uint64_t objectRevision = sceneController_->objectDataRevision();
      if (cachedNodePivotIndex_ == nodeIndex &&
          cachedNodePivotSceneRevision_ == sceneRevision &&
          cachedNodePivotObjectRevision_ == objectRevision) {
        if (cachedNodePivot_.valid) {
          return cachedNodePivot_;
        }
      } else {
        const SceneNodeWorldBounds bounds =
            sceneController_->nodeWorldBounds(nodeIndex);
        cachedNodePivotIndex_ = nodeIndex;
        cachedNodePivotSceneRevision_ = sceneRevision;
        cachedNodePivotObjectRevision_ = objectRevision;
        cachedNodePivot_ =
            bounds.valid
                ? ViewPivot{bounds.center, std::max(bounds.radius, 0.25f),
                            true}
                : ViewPivot{};
        if (cachedNodePivot_.valid) {
          return cachedNodePivot_;
        }
      }
    }
    if (const auto *node = sceneGraph_.getNode(nodeIndex)) {
      return ViewPivot{glm::vec3(node->worldTransform[3]),
                       selectedNodeRadius(sceneManager_), true};
    }
  }

  if (sceneManager_ && sceneManager_->modelBounds().valid) {
    const auto &bounds = sceneManager_->modelBounds();
    return ViewPivot{bounds.center, std::max(bounds.radius, 0.25f), true};
  }

  if (camera_) {
    return ViewPivot{camera_->position() + camera_->frontVector() * 5.0f, 1.0f,
                     true};
  }

  return {};
}

float CameraController::viewPlaneNavigationSpeed(uint32_t nodeIndex,
                                                 float speedScale) const {
  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  float viewScale =
      pivot.valid ? glm::length(camera_->position() - pivot.center)
                  : std::max(inputManager_.moveSpeed(), kDefaultMoveSpeed);
  if (const auto *orthographic =
          dynamic_cast<const container::scene::OrthographicCamera *>(
              camera_.get())) {
    viewScale = orthographic->viewHeight();
  }
  return std::max(viewScale, 1.0f) * 0.0015f *
         std::max(speedScale, 0.01f);
}

void CameraController::lookAt(const glm::vec3 &target) {
  if (!camera_) {
    return;
  }

  glm::vec3 forward = target - camera_->position();
  const float length = glm::length(forward);
  if (length <= 0.0001f) {
    return;
  }
  forward /= length;

  const float pitchDegrees =
      glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  const float yawDegrees = glm::degrees(std::atan2(-forward.z, forward.x));
  camera_->setYawPitch(yawDegrees, pitchDegrees);
}

void CameraController::setCameraNearFar(float nearPlane, float farPlane) {
  if (auto *perspective =
          dynamic_cast<container::scene::PerspectiveCamera *>(camera_.get())) {
    perspective->setNearFar(nearPlane, farPlane);
  } else if (auto *orthographic =
                 dynamic_cast<container::scene::OrthographicCamera *>(
                     camera_.get())) {
    orthographic->setNearFar(nearPlane, farPlane);
  }
}

void CameraController::syncOrthographicViewHeight(const ViewPivot &pivot) {
  auto *orthographic =
      dynamic_cast<container::scene::OrthographicCamera *>(camera_.get());
  if (!orthographic) {
    return;
  }

  const float viewHeight =
      pivot.valid ? std::max(0.25f, pivot.radius * 2.4f) : 10.0f;
  orthographic->setViewHeight(viewHeight);
}

void CameraController::cancelViewAnimation() { viewAnimation_.active = false; }

void CameraController::frameNodeOrScene(uint32_t nodeIndex) {
  if (!camera_) {
    return;
  }
  cancelViewAnimation();

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  if (!pivot.valid) {
    resetCameraForScene();
    return;
  }

  const float distance = std::max(kDefaultNearPlane * 4.0f,
                                  pivot.radius * 2.5f + kDefaultNearPlane);
  camera_->setPosition(pivot.center - camera_->frontVector() * distance);
  lookAt(pivot.center);
  syncOrthographicViewHeight(pivot);
  inputManager_.setMoveSpeed(std::max(kDefaultMoveSpeed, pivot.radius * 0.5f));
}

void CameraController::orbit(uint32_t nodeIndex, float deltaX, float deltaY,
                             float sensitivityScale) {
  if (!camera_ || (deltaX == 0.0f && deltaY == 0.0f)) {
    return;
  }
  cancelViewAnimation();

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  if (!pivot.valid) {
    return;
  }

  const float distance =
      std::max(glm::length(camera_->position() - pivot.center),
               kDefaultNearPlane * 4.0f);
  const float sensitivity = 0.20f * std::max(sensitivityScale, 0.01f);
  camera_->setYawPitch(camera_->yawDegrees() + deltaX * sensitivity,
                       camera_->pitchDegrees() + deltaY * sensitivity);
  camera_->setPosition(pivot.center - camera_->frontVector() * distance);
  lookAt(pivot.center);
}

void CameraController::pan(uint32_t nodeIndex, float deltaX, float deltaY,
                           float speedScale) {
  if (!camera_ || (deltaX == 0.0f && deltaY == 0.0f)) {
    return;
  }
  cancelViewAnimation();

  const float speed = viewPlaneNavigationSpeed(nodeIndex, speedScale);
  const glm::vec3 front = camera_->frontVector();
  const glm::vec3 up = camera_->upVector(front);
  const glm::vec3 right = camera_->rightVector(front, up);
  camera_->move(-right, deltaX * speed);
  camera_->move(-up, deltaY * speed);
}

void CameraController::moveInViewPlane(uint32_t nodeIndex, float deltaRight,
                                       float deltaUp, float speedScale) {
  if (!camera_ || (deltaRight == 0.0f && deltaUp == 0.0f)) {
    return;
  }
  cancelViewAnimation();

  const float speed = viewPlaneNavigationSpeed(nodeIndex, speedScale);
  const glm::vec3 front = camera_->frontVector();
  const glm::vec3 up = camera_->upVector(front);
  const glm::vec3 right = camera_->rightVector(front, up);
  camera_->move(right, deltaRight * speed);
  camera_->move(up, deltaUp * speed);
}

void CameraController::dolly(uint32_t nodeIndex, float wheelSteps,
                             float speedScale) {
  if (!camera_ || wheelSteps == 0.0f) {
    return;
  }
  cancelViewAnimation();

  if (auto *orthographic =
          dynamic_cast<container::scene::OrthographicCamera *>(camera_.get())) {
    const float exponent = wheelSteps * std::max(speedScale, 0.01f);
    const float nextHeight =
        std::clamp(orthographic->viewHeight() * std::pow(0.85f, exponent),
                   0.001f, kDefaultFarPlane * 100.0f);
    orthographic->setViewHeight(nextHeight);
    return;
  }

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  float navigationScale = std::max(inputManager_.moveSpeed(),
                                   kDefaultMoveSpeed);
  if (pivot.valid) {
    navigationScale =
        std::max(navigationScale,
                 std::max(glm::length(camera_->position() - pivot.center),
                          pivot.radius));
  }
  const float distance =
      wheelSteps * navigationScale * std::max(speedScale, 0.01f) * 0.25f;
  camera_->move(camera_->frontVector(), distance);
}

void CameraController::adjustMoveSpeed(float wheelSteps) {
  if (wheelSteps == 0.0f) {
    return;
  }

  const float multiplier = std::pow(1.15f, wheelSteps);
  const float nextSpeed =
      std::clamp(inputManager_.moveSpeed() * multiplier, 0.001f, 100000.0f);
  inputManager_.setMoveSpeed(nextSpeed);
}

float CameraController::moveSpeed() const { return inputManager_.moveSpeed(); }

void CameraController::setSelectionPivotOverride(NavigationPivot pivot) {
  if (pivot.valid) {
    pivot.radius = std::max(pivot.radius, 0.25f);
    selectionPivotOverride_ = pivot;
  } else {
    clearSelectionPivotOverride();
  }
}

void CameraController::clearSelectionPivotOverride() {
  selectionPivotOverride_.reset();
}

bool CameraController::isOrthographic() const {
  return dynamic_cast<const container::scene::OrthographicCamera *>(
             camera_.get()) != nullptr;
}

void CameraController::setOrthographic(uint32_t nodeIndex, bool enabled) {
  if (!camera_ || enabled == isOrthographic()) {
    return;
  }

  float nearPlane = kDefaultNearPlane;
  float farPlane = kDefaultFarPlane;
  if (const auto *perspective =
          dynamic_cast<const container::scene::PerspectiveCamera *>(
              camera_.get())) {
    perspectiveFieldOfViewDegrees_ = perspective->fieldOfViewDegrees();
    nearPlane = perspective->nearPlane();
    farPlane = perspective->farPlane();
  } else if (const auto *orthographic =
                 dynamic_cast<const container::scene::OrthographicCamera *>(
                     camera_.get())) {
    nearPlane = orthographic->nearPlane();
    farPlane = orthographic->farPlane();
  }

  std::unique_ptr<container::scene::BaseCamera> replacement;
  if (enabled) {
    auto orthographic = std::make_unique<container::scene::OrthographicCamera>();
    orthographic->setNearFar(nearPlane, farPlane);
    copyCameraPose(*orthographic, *camera_);
    replacement = std::move(orthographic);
  } else {
    auto perspective = std::make_unique<container::scene::PerspectiveCamera>();
    perspective->setFieldOfView(perspectiveFieldOfViewDegrees_);
    perspective->setNearFar(nearPlane, farPlane);
    copyCameraPose(*perspective, *camera_);
    replacement = std::move(perspective);
  }

  camera_ = std::move(replacement);
  inputManager_.setCamera(camera_.get());
  syncOrthographicViewHeight(resolveViewPivot(nodeIndex));
}

void CameraController::toggleProjectionMode(uint32_t nodeIndex) {
  setOrthographic(nodeIndex, !isOrthographic());
}

void CameraController::setViewPreset(uint32_t nodeIndex,
                                     container::ui::CameraViewPreset preset) {
  if (!camera_) {
    return;
  }

  const ViewPivot pivot = resolveViewPivot(nodeIndex);
  if (!pivot.valid) {
    return;
  }

  glm::vec3 forward{0.0f, 0.0f, -1.0f};
  switch (preset) {
  case container::ui::CameraViewPreset::Front:
    forward = {0.0f, 0.0f, -1.0f};
    break;
  case container::ui::CameraViewPreset::Back:
    forward = {0.0f, 0.0f, 1.0f};
    break;
  case container::ui::CameraViewPreset::Right:
    forward = {-1.0f, 0.0f, 0.0f};
    break;
  case container::ui::CameraViewPreset::Left:
    forward = {1.0f, 0.0f, 0.0f};
    break;
  case container::ui::CameraViewPreset::Top:
    forward = {0.0f, -1.0f, 0.0f};
    break;
  case container::ui::CameraViewPreset::Bottom:
    forward = {0.0f, 1.0f, 0.0f};
    break;
  }

  float distance = glm::length(camera_->position() - pivot.center);
  if (!std::isfinite(distance) || distance <= kDefaultNearPlane * 4.0f) {
    distance = std::max(kDefaultNearPlane * 4.0f,
                        pivot.radius * 2.5f + kDefaultNearPlane);
  }

  const glm::vec3 targetForward = glm::normalize(forward);
  const CameraAngles angles = cameraAnglesFromForward(targetForward);
  viewAnimation_.active = true;
  viewAnimation_.startPosition = camera_->position();
  viewAnimation_.targetPosition = pivot.center - targetForward * distance;
  viewAnimation_.startYawDegrees = camera_->yawDegrees();
  viewAnimation_.startPitchDegrees = camera_->pitchDegrees();
  viewAnimation_.targetYawDegrees = angles.yawDegrees;
  viewAnimation_.targetPitchDegrees = angles.pitchDegrees;
  viewAnimation_.elapsedSeconds = 0.0f;
  viewAnimation_.durationSeconds = 0.22f;
  syncOrthographicViewHeight(pivot);
  inputManager_.setMoveSpeed(std::max(kDefaultMoveSpeed, pivot.radius * 0.5f));
}

void CameraController::setBimElevationView(
    uint32_t nodeIndex, const container::ui::BimElevationViewRequest &request) {
  container::ui::CameraViewPreset preset = request.preset;
  switch (preset) {
  case container::ui::CameraViewPreset::Front:
  case container::ui::CameraViewPreset::Back:
  case container::ui::CameraViewPreset::Right:
  case container::ui::CameraViewPreset::Left:
    break;
  case container::ui::CameraViewPreset::Top:
  case container::ui::CameraViewPreset::Bottom:
    preset = container::ui::CameraViewPreset::Front;
    break;
  }

  // BIM elevation drawings use orthographic side cameras, while the existing
  // preset path keeps orbit pivot, animation, and near/far behavior shared.
  if (request.forceOrthographic) {
    setOrthographic(nodeIndex, true);
  }
  setViewPreset(nodeIndex, preset);
}

void CameraController::updateViewAnimation(float deltaTime) {
  if (!camera_ || !viewAnimation_.active) {
    return;
  }

  viewAnimation_.elapsedSeconds += std::max(deltaTime, 0.0f);
  const float rawT = viewAnimation_.durationSeconds > 0.0001f
                         ? viewAnimation_.elapsedSeconds /
                               viewAnimation_.durationSeconds
                         : 1.0f;
  const float t = smoothStep(rawT);
  camera_->setPosition(glm::mix(viewAnimation_.startPosition,
                                viewAnimation_.targetPosition, t));
  camera_->setYawPitch(
      lerpAngleDegrees(viewAnimation_.startYawDegrees,
                       viewAnimation_.targetYawDegrees, t),
      viewAnimation_.startPitchDegrees +
          (viewAnimation_.targetPitchDegrees - viewAnimation_.startPitchDegrees) *
              t);
  if (rawT >= 1.0f) {
    camera_->setPosition(viewAnimation_.targetPosition);
    camera_->setYawPitch(viewAnimation_.targetYawDegrees,
                         viewAnimation_.targetPitchDegrees);
    viewAnimation_.active = false;
  }
}

void CameraController::updateCameraBuffer(
    container::gpu::CameraData &outCameraData,
    const container::gpu::AllocatedBuffer &cameraBuffer) const {
  if (!camera_)
    return;
  const auto extent = swapChainManager_.extent();
  const float aspect =
      static_cast<float>(extent.width) / static_cast<float>(extent.height);

  outCameraData.viewProj = camera_->viewProjection(aspect);
  outCameraData.inverseViewProj = glm::inverse(outCameraData.viewProj);
  outCameraData.cameraWorldPosition = glm::vec4(camera_->position(), 1.0f);
  outCameraData.cameraForward = glm::vec4(camera_->frontVector(), 0.0f);

  float nearPlane = kDefaultNearPlane;
  float farPlane = kDefaultFarPlane;
  if (const auto *perspective =
          dynamic_cast<const container::scene::PerspectiveCamera *>(
              camera_.get())) {
    nearPlane = perspective->nearPlane();
    farPlane = perspective->farPlane();
  } else if (const auto *orthographic =
                 dynamic_cast<const container::scene::OrthographicCamera *>(
                     camera_.get())) {
    nearPlane = orthographic->nearPlane();
    farPlane = orthographic->farPlane();
  }
  (void)world_.setActiveCamera(outCameraData, nearPlane, farPlane);

  if (cameraBuffer.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, cameraBuffer,
                                   &outCameraData,
                                   sizeof(container::gpu::CameraData));
  }
}

container::ui::TransformControls
CameraController::cameraTransformControls() const {
  container::ui::TransformControls controls{};
  if (!camera_)
    return controls;
  controls.position = camera_->position();
  controls.rotationDegrees = {camera_->pitchDegrees(), camera_->yawDegrees(),
                              0.0f};
  controls.scale = camera_->scale();
  return controls;
}

container::ui::TransformControls
CameraController::nodeTransformControls(uint32_t nodeIndex) const {
  if (const auto *node = sceneGraph_.getNode(nodeIndex)) {
    return decomposeTransform(node->localTransform);
  }
  return {};
}

void CameraController::applyCameraTransform(
    const container::ui::TransformControls &controls,
    container::gpu::CameraData &outCameraData,
    const container::gpu::AllocatedBuffer &cameraBuffer) {
  if (!camera_)
    return;
  cancelViewAnimation();
  camera_->setPosition(controls.position);
  camera_->setYawPitch(controls.rotationDegrees.y, controls.rotationDegrees.x);
  camera_->setScale(controls.scale);
  updateCameraBuffer(outCameraData, cameraBuffer);
}

void CameraController::applyNodeTransform(
    uint32_t nodeIndex, uint32_t rootNode,
    const container::ui::TransformControls &controls) {
  if (sceneGraph_.getNode(nodeIndex) == nullptr)
    return;
  sceneGraph_.setLocalTransform(nodeIndex, composeTransform(controls));
  sceneGraph_.updateWorldTransforms();
  // Caller is responsible for triggering updateLightingData when rootNode
  // changes.
  (void)rootNode;
}

void CameraController::selectMeshNode(uint32_t nodeIndex,
                                      uint32_t &selectedMeshNode) const {
  selectedMeshNode = sceneGraph_.getNode(nodeIndex) != nullptr
                         ? nodeIndex
                         : container::scene::SceneGraph::kInvalidNode;
}

} // namespace container::renderer
