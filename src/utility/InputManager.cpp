#include <Container/utility/Camera.h>
#include <Container/utility/InputManager.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

namespace container::window {

void InputManager::setWindow(GLFWwindow* window) {
  window_ = window;
  if (window_) {
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
}

bool InputManager::update(float deltaTime) {
  if (!camera_) {
    endFrame();
    return false;
  }

  const bool hadMouseDelta =
      (pendingMouseDeltaX_ != 0.0) || (pendingMouseDeltaY_ != 0.0);

  applyMouseInput();
  applyKeyboardInput(deltaTime);

  const bool moved = !pressedKeys_.empty();
  endFrame();
  return moved || hadMouseDelta;
}

InputFrameSnapshot InputManager::frameSnapshot() const {
  double framebufferScaleX = 1.0;
  double framebufferScaleY = 1.0;
  if (window_) {
    int windowWidth = 0;
    int windowHeight = 0;
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetWindowSize(window_, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
    if (windowWidth > 0 && windowHeight > 0 && framebufferWidth > 0 &&
        framebufferHeight > 0) {
      framebufferScaleX = static_cast<double>(framebufferWidth) / windowWidth;
      framebufferScaleY = static_cast<double>(framebufferHeight) / windowHeight;
    }
  }

  auto snapshot = InputFrameSnapshot{
      .cursorX = lastMouseX_,
      .cursorY = lastMouseY_,
      .framebufferCursorX = lastMouseX_ * framebufferScaleX,
      .framebufferCursorY = lastMouseY_ * framebufferScaleY,
      .cursorDeltaX = frameMouseDeltaX_,
      .cursorDeltaY = frameMouseDeltaY_,
      .scrollDeltaX = scrollDeltaX_,
      .scrollDeltaY = scrollDeltaY_,
      .focused = focused_,
      .lookModeActive = lookModeActive_,
      .pressedKeys = pressedKeys_,
      .pressedMouseButtons = pressedMouseButtons_,
      .keysPressedThisFrame = keysPressedThisFrame_,
      .keysReleasedThisFrame = keysReleasedThisFrame_,
      .mouseButtonsPressedThisFrame = mouseButtonsPressedThisFrame_,
      .mouseButtonsReleasedThisFrame = mouseButtonsReleasedThisFrame_,
      .leftButtonPressId = leftButtonPressId_,
      .leftButtonMotions = leftButtonMotions_,
  };
  for (auto& motion : snapshot.leftButtonMotions) {
    motion.pressX *= framebufferScaleX;
    motion.pressY *= framebufferScaleY;
  }
  return snapshot;
}

void InputManager::endFrame() {
  keysPressedThisFrame_.clear();
  keysReleasedThisFrame_.clear();
  mouseButtonsPressedThisFrame_.clear();
  mouseButtonsReleasedThisFrame_.clear();
  leftButtonMotions_.clear();
  frameMouseDeltaX_ = 0.0;
  frameMouseDeltaY_ = 0.0;
  scrollDeltaX_ = 0.0;
  scrollDeltaY_ = 0.0;
}

void InputManager::enqueueMouseDelta(double xpos, double ypos) {
  if (firstCursorEvent_) {
    lastMouseX_ = xpos;
    lastMouseY_ = ypos;
    firstCursorEvent_ = false;
    if (lookModeActive_) {
      firstMouseUpdate_ = false;
    }
    return;
  }

  const double xoffset = xpos - lastMouseX_;
  const double yoffset = lastMouseY_ - ypos;
  lastMouseX_ = xpos;
  lastMouseY_ = ypos;

  frameMouseDeltaX_ += xoffset;
  frameMouseDeltaY_ += yoffset;
  if (pressedMouseButtons_.contains(GLFW_MOUSE_BUTTON_LEFT)) {
    auto& motion = leftButtonMotion();
    motion.deltaX += xoffset;
    motion.deltaY += yoffset;
  }

  if (!lookModeActive_) return;

  if (firstMouseUpdate_) {
    firstMouseUpdate_ = false;
    return;
  }

  pendingMouseDeltaX_ += xoffset;
  pendingMouseDeltaY_ += yoffset;
}

LeftButtonMotion& InputManager::leftButtonMotion() {
  if (leftButtonMotions_.empty() || leftButtonMotions_.back().pressId != leftButtonPressId_)
    leftButtonMotions_.push_back({.pressId = leftButtonPressId_});
  return leftButtonMotions_.back();
}

void InputManager::handleMouseButton(int button, int action) {
  if (action == GLFW_PRESS) {
    const bool alreadyDown = pressedMouseButtons_.contains(button);
    pressedMouseButtons_.insert(button);
    if (!alreadyDown) {
      mouseButtonsPressedThisFrame_.insert(button);
      if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (firstCursorEvent_ && window_) {
          glfwGetCursorPos(window_, &lastMouseX_, &lastMouseY_);
          firstCursorEvent_ = false;
        }
        ++leftButtonPressId_;
        auto& motion = leftButtonMotion();
        motion.pressX = lastMouseX_;
        motion.pressY = lastMouseY_;
        motion.pressed = true;
      }
    }
  } else if (action == GLFW_RELEASE) {
    const bool wasDown = pressedMouseButtons_.erase(button) > 0u;
    if (wasDown) {
      mouseButtonsReleasedThisFrame_.insert(button);
      if (button == GLFW_MOUSE_BUTTON_LEFT) leftButtonMotion().released = true;
    }
  }
}

void InputManager::handleKey(int key, int action) {
  if (action == GLFW_PRESS) {
    const bool alreadyDown = pressedKeys_.contains(key);
    pressedKeys_.insert(key);
    if (!alreadyDown) {
      keysPressedThisFrame_.insert(key);
    }
  } else if (action == GLFW_REPEAT) {
    pressedKeys_.insert(key);
  } else if (action == GLFW_RELEASE) {
    const bool wasDown = pressedKeys_.erase(key) > 0u;
    if (wasDown) {
      keysReleasedThisFrame_.insert(key);
    }
  }
}

void InputManager::handleScroll(double xoffset, double yoffset) {
  scrollDeltaX_ += xoffset;
  scrollDeltaY_ += yoffset;
}

void InputManager::handleWindowFocus(bool focused) {
  focused_ = focused;
  if (focused) return;

  pressedKeys_.clear();
  pressedMouseButtons_.clear();
  keysPressedThisFrame_.clear();
  keysReleasedThisFrame_.clear();
  mouseButtonsPressedThisFrame_.clear();
  mouseButtonsReleasedThisFrame_.clear();
  leftButtonMotions_.clear();
  frameMouseDeltaX_ = 0.0;
  frameMouseDeltaY_ = 0.0;
  scrollDeltaX_ = 0.0;
  scrollDeltaY_ = 0.0;
  pendingMouseDeltaX_ = 0.0;
  pendingMouseDeltaY_ = 0.0;
  setLookMode(false);
}

void InputManager::applyMouseInput() {
  if (!camera_) return;

  if (!lookModeActive_) {
    pendingMouseDeltaX_ = 0.0;
    pendingMouseDeltaY_ = 0.0;
    return;
  }

  if (pendingMouseDeltaX_ == 0.0 && pendingMouseDeltaY_ == 0.0) return;

  camera_->addYawPitch(
      static_cast<float>(pendingMouseDeltaX_) * mouseSensitivity_,
      static_cast<float>(pendingMouseDeltaY_) * mouseSensitivity_);

  pendingMouseDeltaX_ = 0.0;
  pendingMouseDeltaY_ = 0.0;
}

void InputManager::applyKeyboardInput(float deltaTime) {
  if (!camera_) return;
  if (pressedKeys_.empty()) return;

  const glm::vec3 front = camera_->frontVector();
  const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
  const glm::vec3 cameraUp = camera_->upVector(front);

  glm::vec3 right = camera_->rightVector(front, cameraUp);
  right.y = 0.0f;
  if (glm::dot(right, right) > 0.000001f) {
    right = glm::normalize(right);
  } else {
    right = glm::vec3(1.0f, 0.0f, 0.0f);
  }

  glm::vec3 forward = front;
  forward.y = 0.0f;
  if (glm::dot(forward, forward) > 0.000001f) {
    forward = glm::normalize(forward);
  } else {
    forward = glm::normalize(glm::cross(right, worldUp));
  }

  float velocity = moveSpeed_ * deltaTime;
  if (pressedKeys_.count(GLFW_KEY_LEFT_SHIFT) ||
      pressedKeys_.count(GLFW_KEY_RIGHT_SHIFT)) {
    velocity *= 2.5f;
  }

  if (pressedKeys_.count(GLFW_KEY_W)) camera_->move(forward, velocity);
  if (pressedKeys_.count(GLFW_KEY_S)) camera_->move(-forward, velocity);
  if (pressedKeys_.count(GLFW_KEY_A)) camera_->move(-right, velocity);
  if (pressedKeys_.count(GLFW_KEY_D)) camera_->move(right, velocity);
  if (pressedKeys_.count(GLFW_KEY_SPACE) || pressedKeys_.count(GLFW_KEY_E)) {
    camera_->move(worldUp, velocity);
  }
  if (pressedKeys_.count(GLFW_KEY_LEFT_CONTROL) ||
      pressedKeys_.count(GLFW_KEY_RIGHT_CONTROL) ||
      pressedKeys_.count(GLFW_KEY_Q)) {
    camera_->move(-worldUp, velocity);
  }
}

void InputManager::setLookMode(bool enabled) {
  lookModeActive_ = enabled;
  pendingMouseDeltaX_ = 0.0;
  pendingMouseDeltaY_ = 0.0;
  firstMouseUpdate_ = true;

  if (!window_) return;

  glfwSetInputMode(window_, GLFW_CURSOR,
                   enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION,
                     enabled ? GLFW_TRUE : GLFW_FALSE);
  }
}

}  // namespace container::window
