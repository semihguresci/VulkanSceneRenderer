#pragma once

#include <Container/renderer/core/RenderTechnique.h>
#include <Container/utility/GuiManager.h>

#include <algorithm>
#include <string_view>

namespace container::renderer {

enum class BimFloorPlanViewportRoute {
  SceneColor,
  Overview,
  DiagnosticGrayscale,
  ExcludedByDisplayMode,
  PresentationOverlay,
};

[[nodiscard]] constexpr bool bimFloorPlanUsesOverviewOverlay(
    RenderTechniqueId technique, ui::GBufferViewMode mode, bool depthTest) {
  return technique == RenderTechniqueId::ForwardRaster &&
         mode == ui::GBufferViewMode::Overview && !depthTest;
}

// Inner Lit tile of the Overview grid in post_process.slang.
inline constexpr float kOverviewLitTileLeft = 0.02f + 0.006f;
inline constexpr float kOverviewLitTileTop = 0.04f + 0.006f;
inline constexpr float kOverviewLitTileWidth = (0.98f - 0.02f) / 3.0f - 2.0f * 0.006f;
inline constexpr float kOverviewLitTileHeight = (0.96f - 0.04f) / 4.0f - 2.0f * 0.006f;

[[nodiscard]] constexpr float bimFloorPlanOverviewLineWidth(float lineWidth) {
  return std::max(1.0f, lineWidth * kOverviewLitTileHeight);
}

// Raster floor plans are in scene color before post_process.slang selects the
// display buffer. Reference ray techniques draw floor plans after post-processing.
[[nodiscard]] constexpr BimFloorPlanViewportRoute bimFloorPlanViewportRoute(
    RenderTechniqueId technique, ui::GBufferViewMode mode) {
  using Route = BimFloorPlanViewportRoute;
  using Mode = ui::GBufferViewMode;
  if (isReferenceRayTechnique(technique))
    return Route::PresentationOverlay;
  switch (mode) {
  case Mode::Lit:
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
