#pragma once

#include "Container/utility/Camera.h"
#include "Container/utility/GuiManager.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace container::gpu {
class AllocationManager;
class SwapChainManager;
class VulkanDevice;
struct AllocatedBuffer;
struct CameraData;
} // namespace container::gpu

namespace container::ecs {
class World;
} // namespace container::ecs

namespace container::scene {
class SceneGraph;
class SceneManager;
} // namespace container::scene

namespace container::window {
class InputManager;
} // namespace container::window

namespace container::renderer {

class SceneController;

// Manages the application camera: creation, reset, update, and transform
// helpers for both camera and scene-graph nodes.
class CameraController {
public:
  struct NavigationPivot {
    glm::vec3 center{0.0f};
    float radius{1.0f};
    bool valid{false};
  };

  struct SceneViewBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid{false};
  };

  CameraController(std::shared_ptr<container::gpu::VulkanDevice> device,
                   container::gpu::AllocationManager &allocationManager,
                   container::gpu::SwapChainManager &swapChainManager,
                   container::scene::SceneGraph &sceneGraph,
                   container::scene::SceneManager *sceneManager,
                   SceneController *sceneController,
                   container::ecs::World &world,
                   container::window::InputManager &inputManager);

  ~CameraController() = default;
  CameraController(const CameraController &) = delete;
  CameraController &operator=(const CameraController &) = delete;

  // ---- Lifecycle ----------------------------------------------------------

  // Creates PerspectiveCamera and wires it to the input manager.
  void createCamera();

  // Positions the camera relative to the current scene bounds.
  void resetCameraForScene();
  void resetCameraForBounds(const SceneViewBounds &bounds);

  // Viewport navigation helpers used by render-space interactions.
  void frameNodeOrScene(uint32_t nodeIndex);
  void orbit(uint32_t nodeIndex, float deltaX, float deltaY,
             float sensitivityScale = 1.0f);
  void pan(uint32_t nodeIndex, float deltaX, float deltaY,
           float speedScale = 1.0f);
  void moveInViewPlane(uint32_t nodeIndex, float deltaRight, float deltaUp,
                       float speedScale = 1.0f);
  void dolly(uint32_t nodeIndex, float wheelSteps, float speedScale = 1.0f);
  void adjustMoveSpeed(float wheelSteps);
  [[nodiscard]] float moveSpeed() const;
  [[nodiscard]] bool isOrthographic() const;
  void setOrthographic(uint32_t nodeIndex, bool enabled);
  void toggleProjectionMode(uint32_t nodeIndex);
  void setViewPreset(uint32_t nodeIndex,
                     container::ui::CameraViewPreset preset);
  void setBimElevationView(
      uint32_t nodeIndex,
      const container::ui::BimElevationViewRequest &request);
  void updateViewAnimation(float deltaTime);
  void setSelectionPivotOverride(NavigationPivot pivot);
  void clearSelectionPivotOverride();

  // ---- Per-frame ----------------------------------------------------------

  // Updates cameraData and uploads to cameraBuffer.
  void
  updateCameraBuffer(container::gpu::CameraData &outCameraData,
                     const container::gpu::AllocatedBuffer &cameraBuffer) const;

  // ---- Transform helpers --------------------------------------------------

  container::ui::TransformControls cameraTransformControls() const;
  container::ui::TransformControls
  nodeTransformControls(uint32_t nodeIndex) const;

  void
  applyCameraTransform(const container::ui::TransformControls &controls,
                       container::gpu::CameraData &outCameraData,
                       const container::gpu::AllocatedBuffer &cameraBuffer);

  void applyNodeTransform(uint32_t nodeIndex, uint32_t rootNode,
                          const container::ui::TransformControls &controls);

  void selectMeshNode(uint32_t nodeIndex, uint32_t &selectedMeshNode) const;

  // ---- Accessors ----------------------------------------------------------

  container::scene::BaseCamera *camera() const { return camera_.get(); }

private:
  static constexpr float kDefaultYaw = -135.0f;
  static constexpr float kDefaultPitch = -35.0f;
  static constexpr float kDefaultMoveSpeed = 1.0f;
  static constexpr float kDefaultNearPlane = 0.05f;
  static constexpr float kDefaultFarPlane = 500.0f;

  struct ViewPivot {
    glm::vec3 center{0.0f};
    float radius{1.0f};
    bool valid{false};
  };

  [[nodiscard]] ViewPivot resolveViewPivot(uint32_t nodeIndex) const;
  [[nodiscard]] float viewPlaneNavigationSpeed(uint32_t nodeIndex,
                                               float speedScale) const;
  void lookAt(const glm::vec3& target);
  void setCameraNearFar(float nearPlane, float farPlane);
  void syncOrthographicViewHeight(const ViewPivot &pivot);
  void cancelViewAnimation();

  struct ViewAnimation {
    bool active{false};
    glm::vec3 startPosition{0.0f};
    glm::vec3 targetPosition{0.0f};
    float startYawDegrees{0.0f};
    float startPitchDegrees{0.0f};
    float targetYawDegrees{0.0f};
    float targetPitchDegrees{0.0f};
    float elapsedSeconds{0.0f};
    float durationSeconds{0.22f};
  };

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager &allocationManager_;
  container::gpu::SwapChainManager &swapChainManager_;
  container::scene::SceneGraph &sceneGraph_;
  container::scene::SceneManager *sceneManager_{nullptr};
  SceneController *sceneController_{nullptr};
  container::ecs::World &world_;
  container::window::InputManager &inputManager_;

  std::unique_ptr<container::scene::BaseCamera> camera_;
  float perspectiveFieldOfViewDegrees_{60.0f};
  ViewAnimation viewAnimation_{};
  std::optional<NavigationPivot> selectionPivotOverride_{};
  mutable uint32_t cachedNodePivotIndex_{std::numeric_limits<uint32_t>::max()};
  mutable uint64_t
      cachedNodePivotSceneRevision_{std::numeric_limits<uint64_t>::max()};
  mutable uint64_t
      cachedNodePivotObjectRevision_{std::numeric_limits<uint64_t>::max()};
  mutable ViewPivot cachedNodePivot_{};
};

} // namespace container::renderer
