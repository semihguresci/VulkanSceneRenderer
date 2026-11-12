// InputManager.h
#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

struct GLFWwindow;

namespace container::scene {
class BaseCamera;
}

namespace container::window {

struct LeftButtonMotion {
  uint64_t pressId{};
  double pressX{}, pressY{};
  double deltaX{}, deltaY{};
  bool pressed{}, released{};
};

struct InputFrameSnapshot {
  double cursorX{0.0};
  double cursorY{0.0};
  double framebufferCursorX{0.0};
  double framebufferCursorY{0.0};
  double cursorDeltaX{0.0};
  double cursorDeltaY{0.0};
  double scrollDeltaX{0.0};
  double scrollDeltaY{0.0};
  bool focused{true};
  bool lookModeActive{false};

  std::unordered_set<int> pressedKeys{};
  std::unordered_set<int> pressedMouseButtons{};
  std::unordered_set<int> keysPressedThisFrame{};
  std::unordered_set<int> keysReleasedThisFrame{};
  std::unordered_set<int> mouseButtonsPressedThisFrame{};
  std::unordered_set<int> mouseButtonsReleasedThisFrame{};
  uint64_t leftButtonPressId{};
  // Press positions are in framebuffer coordinates. Deltas retain the same
  // window-coordinate units and Y convention as cursorDeltaX/cursorDeltaY.
  std::vector<LeftButtonMotion> leftButtonMotions;

  [[nodiscard]] bool keyDown(int key) const {
    return pressedKeys.contains(key);
  }

  [[nodiscard]] bool keyPressed(int key) const {
    return keysPressedThisFrame.contains(key);
  }

  [[nodiscard]] bool keyReleased(int key) const {
    return keysReleasedThisFrame.contains(key);
  }

  [[nodiscard]] bool mouseButtonDown(int button) const {
    return pressedMouseButtons.contains(button);
  }

  [[nodiscard]] bool mouseButtonPressed(int button) const {
    return mouseButtonsPressedThisFrame.contains(button);
  }

  [[nodiscard]] bool mouseButtonReleased(int button) const {
    return mouseButtonsReleasedThisFrame.contains(button);
  }
};

class InputManager {
 public:
  InputManager() = default;

  void setMoveSpeed(float speed) { moveSpeed_ = speed; }
  [[nodiscard]] float moveSpeed() const { return moveSpeed_; }
  void setMouseSensitivity(float sensitivity) {
    mouseSensitivity_ = sensitivity;
  }
  void setCamera(container::scene::BaseCamera* camera) { camera_ = camera; }
  void setWindow(GLFWwindow* window);

  [[nodiscard]] bool isLooking() const { return lookModeActive_; }
  void setLookMode(bool enabled);

  bool update(float deltaTime);
  [[nodiscard]] InputFrameSnapshot frameSnapshot() const;
  void endFrame();

  void enqueueMouseDelta(double xpos, double ypos);
  void handleMouseButton(int button, int action);
  void handleKey(int key, int action);
  void handleScroll(double xoffset, double yoffset);
  void handleWindowFocus(bool focused);

 private:
  void applyMouseInput();
  void applyKeyboardInput(float deltaTime);
  LeftButtonMotion& leftButtonMotion();

  container::scene::BaseCamera* camera_{nullptr};
  GLFWwindow* window_{nullptr};
  std::unordered_set<int> pressedKeys_{};
  std::unordered_set<int> pressedMouseButtons_{};
  std::unordered_set<int> keysPressedThisFrame_{};
  std::unordered_set<int> keysReleasedThisFrame_{};
  std::unordered_set<int> mouseButtonsPressedThisFrame_{};
  std::unordered_set<int> mouseButtonsReleasedThisFrame_{};
  uint64_t leftButtonPressId_{};
  std::vector<LeftButtonMotion> leftButtonMotions_;

  double lastMouseX_{0.0};
  double lastMouseY_{0.0};
  bool firstMouseUpdate_{true};
  bool firstCursorEvent_{true};
  bool lookModeActive_{false};
  bool focused_{true};
  double frameMouseDeltaX_{0.0};
  double frameMouseDeltaY_{0.0};
  double scrollDeltaX_{0.0};
  double scrollDeltaY_{0.0};
  double pendingMouseDeltaX_{0.0};
  double pendingMouseDeltaY_{0.0};

  float moveSpeed_{3.5f};
  float mouseSensitivity_{0.10f};
};

}  // namespace container::window
