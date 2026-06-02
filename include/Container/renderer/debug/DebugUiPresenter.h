#pragma once

#include "Container/renderer/core/TechniqueDebugModel.h"

namespace container::renderer {

class RenderGraph;

} // namespace container::renderer

namespace container::ui {
class GuiManager;
}

namespace container::renderer {

// Keeps renderer debug UI synchronization out of RendererFrontend's frame
// submission path. The presenter translates neutral debug models into GUI
// toggle rows and applies edited toggle state back to the graph.
class DebugUiPresenter {
 public:
  static void publishRenderGraphDebugModel(
      container::ui::GuiManager& guiManager,
      const RenderGraphDebugModel& debugModel);

  static void publishTechniqueDebugModel(
      container::ui::GuiManager& guiManager,
      const TechniqueDebugModel& debugModel);

  static void publishRenderPasses(container::ui::GuiManager& guiManager,
                                  const RenderGraph& graph);

  // Returns true when protected/dependent toggles were corrected.
  static bool applyRenderPassToggles(container::ui::GuiManager& guiManager,
                                     RenderGraph& graph);
};

}  // namespace container::renderer
