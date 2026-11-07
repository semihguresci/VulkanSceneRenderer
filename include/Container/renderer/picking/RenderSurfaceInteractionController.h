#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>

#include "Container/utility/GuiManager.h"

namespace container::window {
class InputManager;
struct InputFrameSnapshot;
}  // namespace container::window

namespace container::renderer {

class CameraController;
struct DebugRenderState;

// Owns render-space input routing. It decides when raw input belongs to ImGui,
// an active viewport gesture, camera navigation, or debug shortcuts.
class RenderSurfaceInteractionController {
 public:
  enum class TransformGestureCancelReason { Escape, FocusLost, SelectionLost };
  struct TransformGestureCallbacks {
    std::function<void(container::ui::ViewportTool, container::ui::TransformSpace,
                       container::ui::TransformAxis, bool)> begin{};
    std::function<void()> end{};
    std::function<void(TransformGestureCancelReason)> cancel{};
  };
  struct Context {
    container::window::InputManager& inputManager;
    CameraController& cameraController;
    DebugRenderState& debugState;
    float deltaTime{0.0f};
    bool guiCapturingMouse{false};
    bool guiCapturingKeyboard{false};
    uint32_t selectedMeshNode{std::numeric_limits<uint32_t>::max()};
    bool hasSelection{false};
    std::function<void(double, double)> selectAtCursor{};
    std::function<void(double, double)> hoverAtCursor{};
    std::function<void()> clearHover{};
    std::function<void()> clearSelection{};
    std::function<std::optional<container::ui::TransformAxis>(double, double)>
        pickTransformGizmoAxisAtCursor{};
    std::function<void(container::ui::ViewportTool,
                       container::ui::TransformSpace,
                       container::ui::TransformAxis,
                       bool,
                       double,
                       double)> transformSelectedByDrag{};
    std::function<void(std::string)> setStatusMessage{};
    std::function<void()> onCullingUnfrozen{};
    TransformGestureCallbacks transformGestureCallbacks{};
  };

  void process(Context context);
  [[nodiscard]] const container::ui::ViewportInteractionState& state() const {
    return state_;
  }
  void setTool(container::ui::ViewportTool tool);
  void setTransformSpace(container::ui::TransformSpace transformSpace);
  void setTransformAxis(container::ui::TransformAxis transformAxis);
  void setNavigationStyle(container::ui::ViewportNavigationStyle style);
  void setTransformSnapEnabled(bool enabled);

  // The same lifecycle path is usable with detached input snapshots, without
  // constructing navigation/GPU services. Callbacks run before ownership is
  // changed; a failed publication leaves the active gesture available.
  void beginTransformGesture(container::ui::ViewportTool tool,
      container::ui::TransformSpace space, container::ui::TransformAxis axis,
      bool snapEnabled, const TransformGestureCallbacks&, int pointerButton = 0);
  void processTransformGestureLifecycle(const container::window::InputFrameSnapshot&,
      const TransformGestureCallbacks&, bool hasSelection = true);

 private:
  void handleDebugShortcuts(const container::window::InputFrameSnapshot& input,
                            Context& context, bool shortcutsAllowed);
  void handleToolShortcuts(const container::window::InputFrameSnapshot& input,
                           Context& context, bool shortcutsAllowed);
  void handleLookOwnership(const container::window::InputFrameSnapshot& input,
                           Context& context);
  void handleViewportActions(
      const container::window::InputFrameSnapshot& input, Context& context,
      bool viewportInputAllowed);

  void setGesture(container::ui::ViewportGesture gesture);

  container::ui::ViewportInteractionState state_{};
  int activePointerButton_{-1};
  bool transformConfigurationChanged_{false};
};

}  // namespace container::renderer
