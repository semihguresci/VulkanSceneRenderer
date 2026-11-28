#pragma once

#include <Container/renderer/core/RenderTechnique.h>
#include <Container/utility/GuiManager.h>

#include <string_view>

namespace container::renderer {

enum class BimFloorPlanViewportRoute {
  SceneColor,
  Overview,
  DiagnosticGrayscale,
  ExcludedByDisplayMode,
  PresentationOverlay,
};

[[nodiscard]] constexpr bool bimFloorPlanUsesPresentationOverlay(
    RenderTechniqueId technique, ui::GBufferViewMode mode, bool depthTest) {
  return isReferenceRayTechnique(technique) ||
         ((technique == RenderTechniqueId::ForwardRaster || technique == RenderTechniqueId::DeferredRaster) &&
          !depthTest && (mode == ui::GBufferViewMode::Lit || mode == ui::GBufferViewMode::Overview));
}

[[nodiscard]] constexpr BimFloorPlanViewportRoute bimFloorPlanViewportRoute(
    RenderTechniqueId technique, ui::GBufferViewMode mode, bool depthTest) {
  using Route = BimFloorPlanViewportRoute;
  using Mode = ui::GBufferViewMode;
  if (isReferenceRayTechnique(technique))
    return Route::PresentationOverlay;
  switch (mode) {
  case Mode::Lit:
    return bimFloorPlanUsesPresentationOverlay(technique, mode, depthTest)
        ? Route::PresentationOverlay : Route::SceneColor;
  case Mode::SurfaceNormals:
  case Mode::ObjectSpaceNormals:
    return Route::SceneColor;
  case Mode::Overview:
    return Route::Overview;
  case Mode::ShadowCascades:
  case Mode::TileLightHeatMap:
    return Route::DiagnosticGrayscale;
  default:
    return Route::ExcludedByDisplayMode;
  }
}

[[nodiscard]] constexpr std::string_view bimFloorPlanViewportRouteName(
    BimFloorPlanViewportRoute route) {
  using Route = BimFloorPlanViewportRoute;
  switch (route) {
  case Route::SceneColor: return "scene-color";
  case Route::Overview: return "overview";
  case Route::DiagnosticGrayscale: return "diagnostic-grayscale";
  case Route::PresentationOverlay: return "presentation-overlay";
  case Route::ExcludedByDisplayMode: return "excluded-by-display-mode";
  }
  return "excluded-by-display-mode";
}

} // namespace container::renderer
