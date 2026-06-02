#include "Container/renderer/deferred/DeferredRasterTechnique.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/TechniqueDebugModel.h"
#include "Container/renderer/debug/DebugUiPresenter.h"
#include "Container/renderer/forward/ForwardRasterTechnique.h"
#include "Container/utility/GuiManager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class MinimalTechnique final : public container::renderer::RenderTechnique {
 public:
  [[nodiscard]] container::renderer::RenderTechniqueId id() const override {
    return container::renderer::RenderTechniqueId::ForwardRaster;
  }

  [[nodiscard]] std::string_view name() const override {
    return container::renderer::renderTechniqueName(id());
  }

  [[nodiscard]] std::string_view displayName() const override {
    return container::renderer::renderTechniqueDisplayName(id());
  }

  [[nodiscard]] container::renderer::RenderTechniqueAvailability availability(
      const container::renderer::RenderSystemContext&) const override {
    return container::renderer::RenderTechniqueAvailability::availableNow();
  }

  void buildFrameGraph(container::renderer::RenderSystemContext&) override {}
};

std::string readRepoTextFile(const std::filesystem::path& relativePath) {
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / relativePath;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool hasDisplayMode(const container::renderer::TechniqueDebugModel& model,
                    std::string_view id) {
  return std::ranges::any_of(model.displayModes, [id](const auto& mode) {
    return mode.id == id;
  });
}

}  // namespace

TEST(TechniqueDebugModelTests, DefaultTechniqueModelUsesTechniqueIdentity) {
  MinimalTechnique technique;

  const container::renderer::TechniqueDebugModel model =
      technique.debugModel();

  EXPECT_EQ(model.techniqueName, "forward-raster");
  EXPECT_EQ(model.displayName, "Forward rendering");
  EXPECT_TRUE(model.panels.empty());
  EXPECT_TRUE(model.displayModes.empty());
}

TEST(TechniqueDebugModelTests, DeferredRasterPublishesDebugPanels) {
  container::renderer::DeferredRasterTechnique technique;

  const container::renderer::TechniqueDebugModel model =
      technique.debugModel();

  ASSERT_EQ(model.techniqueName, "deferred-raster");
  ASSERT_FALSE(model.panels.empty());
  EXPECT_EQ(model.panels.front().id, "deferred-frame");
  EXPECT_FALSE(model.panels.front().controls.empty());
  EXPECT_TRUE(hasDisplayMode(model, "albedo"));
  EXPECT_TRUE(hasDisplayMode(model, "tile-light-heat-map"));
}

TEST(TechniqueDebugModelTests,
     ForwardRasterPublishesForwardCompatibleDisplayModes) {
  container::renderer::ForwardRasterTechnique technique;

  const container::renderer::TechniqueDebugModel model =
      technique.debugModel();

  EXPECT_EQ(model.techniqueName, "forward-raster");
  EXPECT_TRUE(hasDisplayMode(model, "lit"));
  EXPECT_TRUE(hasDisplayMode(model, "depth"));
  EXPECT_TRUE(hasDisplayMode(model, "overview"));
  EXPECT_TRUE(hasDisplayMode(model, "shadow-cascades"));
  EXPECT_TRUE(hasDisplayMode(model, "shadow-texel-density"));
  EXPECT_FALSE(hasDisplayMode(model, "albedo"));
  EXPECT_FALSE(hasDisplayMode(model, "normals"));
  EXPECT_FALSE(hasDisplayMode(model, "material"));
  EXPECT_FALSE(hasDisplayMode(model, "tile-light-heat-map"));
}

TEST(TechniqueDebugModelTests,
     DebugPresenterPublishesRenderGraphDebugModelRows) {
  container::ui::GuiManager guiManager;
  container::renderer::RenderGraphDebugModel debugModel{};
  debugModel.passes.push_back(
      {.passName = "Lighting",
       .enabled = false,
       .active = false,
       .locked = true,
       .autoDisabled = true,
       .skipReason = "Disabled",
       .dependencyNote = "inactive: disabled by test"});

  container::renderer::DebugUiPresenter::publishRenderGraphDebugModel(
      guiManager, debugModel);

  ASSERT_EQ(guiManager.renderPassToggles().size(), 1u);
  const container::ui::RenderPassToggle& toggle =
      guiManager.renderPassToggles().front();
  EXPECT_EQ(toggle.name, "Lighting");
  EXPECT_FALSE(toggle.enabled);
  EXPECT_TRUE(toggle.locked);
  EXPECT_TRUE(toggle.autoDisabled);
  EXPECT_EQ(toggle.dependencyNote, "inactive: disabled by test");
}

TEST(TechniqueDebugModelTests,
     DebugPresenterPublishesTechniqueDebugStateAndDisplayModes) {
  container::ui::GuiManager guiManager;
  container::renderer::TechniqueDebugModel debugModel{};
  debugModel.techniqueName = "forward-raster";
  debugModel.displayName = "Forward rendering";
  debugModel.displayModes.push_back(
      {.id = "lit", .label = "Lit", .value = 0u});
  debugModel.displayModes.push_back(
      {.id = "depth", .label = "Depth", .value = 4u});
  debugModel.panels.push_back(
      {.id = "forward-frame",
       .title = "Forward Frame",
       .controls = {{.id = "forward-lighting",
                     .label = "Forward lighting",
                     .kind =
                         container::renderer::TechniqueDebugControlKind::
                             Action}}});

  container::renderer::DebugUiPresenter::publishTechniqueDebugModel(
      guiManager, debugModel);

  const container::ui::GuiRenderTechniqueDebugState& state =
      guiManager.renderTechniqueDebugState();
  EXPECT_EQ(state.techniqueName, "forward-raster");
  EXPECT_EQ(state.displayName, "Forward rendering");
  ASSERT_EQ(state.displayModes.size(), 2u);
  EXPECT_EQ(state.displayModes.front().label, "Lit");
  ASSERT_EQ(state.panels.size(), 1u);
  EXPECT_EQ(state.panels.front().title, "Forward Frame");
  ASSERT_EQ(state.panels.front().controls.size(), 1u);
  EXPECT_EQ(state.panels.front().controls.front().label, "Forward lighting");
}

TEST(TechniqueDebugModelTests,
     GuiClampsUnsupportedDisplayModeWhenTechniqueChanges) {
  container::ui::GuiManager guiManager;
  ASSERT_EQ(guiManager.gBufferViewMode(),
            container::ui::GBufferViewMode::Overview);

  container::renderer::TechniqueDebugModel debugModel{};
  debugModel.techniqueName = "minimal";
  debugModel.displayName = "Minimal";
  debugModel.displayModes.push_back(
      {.id = "lit", .label = "Lit", .value = 0u});
  debugModel.displayModes.push_back(
      {.id = "depth", .label = "Depth", .value = 4u});

  container::renderer::DebugUiPresenter::publishTechniqueDebugModel(
      guiManager, debugModel);

  EXPECT_EQ(guiManager.gBufferViewMode(), container::ui::GBufferViewMode::Lit);
}

TEST(TechniqueDebugModelGuardrails, DebugModelsStayUiBackendNeutral) {
  const std::string debugModelHeader =
      readRepoTextFile("include/Container/renderer/core/TechniqueDebugModel.h");
  const std::string renderTechniqueHeader =
      readRepoTextFile("include/Container/renderer/core/RenderTechnique.h");

  for (const std::string& forbidden : {"imgui", "ImGui", "GuiManager", "Vk"}) {
    EXPECT_FALSE(contains(debugModelHeader, forbidden))
        << "Technique debug models should stay UI/backend neutral.";
  }
  EXPECT_TRUE(contains(renderTechniqueHeader, "debugModel()"));
  EXPECT_TRUE(contains(renderTechniqueHeader, "SceneProviderRegistry"));
}

TEST(TechniqueDebugModelGuardrails, GuiHeaderKeepsRendererDebugModelsNeutral) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiSource =
      readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string guiDebugState =
      readRepoTextFile("include/Container/utility/GuiDebugState.h");
  const std::string debugPresenterHeader = readRepoTextFile(
      "include/Container/renderer/debug/DebugUiPresenter.h");
  const std::string debugPresenter =
      readRepoTextFile("src/renderer/debug/DebugUiPresenter.cpp");

  EXPECT_TRUE(contains(guiHeader, "GuiDebugState.h"));
  EXPECT_TRUE(contains(guiHeader, "struct RendererTelemetryView"));
  EXPECT_TRUE(contains(guiHeader, "struct RenderEngineOption"));
  EXPECT_TRUE(contains(guiDebugState, "GuiRenderTechniqueDebugState"));
  EXPECT_TRUE(contains(guiHeader, "setRenderTechniqueDebugState"));
  EXPECT_TRUE(contains(guiHeader, "renderTechniqueDebugState"));
  EXPECT_TRUE(contains(guiHeader, "consumeRenderEngineChange"));
  EXPECT_TRUE(contains(guiHeader, "setRenderEngineOptions"));
  EXPECT_TRUE(contains(guiSource, "\"Rendering Engine\""));
  EXPECT_TRUE(contains(guiSource, "renderTechniqueDebugState_.displayModes"));
  EXPECT_TRUE(contains(guiSource, "!renderEngineOptions_.empty()"));
  EXPECT_TRUE(contains(guiSource, "std::ranges::any_of(renderEngineOptions_"));
  EXPECT_FALSE(contains(guiHeader, "RenderTechnique.h"));
  EXPECT_TRUE(contains(guiSource, "RenderTechnique.h"));
  EXPECT_FALSE(contains(guiHeader, "BimSemanticColorMode.h"));
  EXPECT_FALSE(contains(guiHeader, "RendererTelemetry.h"));
  EXPECT_FALSE(contains(guiHeader,
                        "container::renderer::RendererTelemetryView "
                        "rendererTelemetry_"));
  EXPECT_TRUE(contains(guiDebugState, "GuiRendererTelemetryView"));
  EXPECT_TRUE(contains(debugPresenterHeader, "TechniqueDebugModel.h"));
  EXPECT_TRUE(
      contains(debugPresenterHeader, "publishRenderGraphDebugModel"));
  EXPECT_TRUE(contains(debugPresenterHeader, "publishTechniqueDebugModel"));
  EXPECT_FALSE(contains(debugPresenterHeader, "RenderGraph.h"));
  EXPECT_FALSE(contains(debugPresenterHeader, "GuiManager.h"));
  EXPECT_TRUE(contains(debugPresenter, "graph.debugModel()"));
  EXPECT_TRUE(contains(debugPresenter, "GuiRenderTechniqueDebugState"));
}

TEST(TechniqueDebugModelGuardrails, RendererFrontendOwnsRuntimeTechniqueSwitch) {
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/core/FrameRecorder.h");

  EXPECT_TRUE(contains(frameRecorderHeader, "activeTechnique"));
  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "pendingRenderTechniqueChange_"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "applyPendingRenderTechniqueChange"));
  EXPECT_TRUE(contains(rendererFrontend, "initializeRenderTechnique"));
  EXPECT_TRUE(contains(rendererFrontend, "syncGuiRenderEngineOptions"));
  EXPECT_TRUE(contains(rendererFrontend, "publishTechniqueDebugModel"));
  EXPECT_TRUE(contains(rendererFrontend, "consumeRenderEngineChange"));
}

TEST(TechniqueDebugModelGuardrails,
     SceneModelLoadsAreDeferredOutsideTheActiveImGuiFrame) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiSource =
      readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(guiHeader, "struct ModelLoadRequest"));
  EXPECT_TRUE(contains(guiHeader, "consumeModelLoadRequest"));
  EXPECT_TRUE(contains(guiHeader, "pendingModelLoadRequest_"));
  EXPECT_TRUE(contains(guiSource, "queueModelLoadRequest("));
  EXPECT_TRUE(contains(guiSource, "pendingModelLoadRequest_ ="));
  EXPECT_FALSE(contains(guiSource, "reloadModel(modelPathInput_"));
  EXPECT_FALSE(contains(guiSource, "reloadDefault(importScale_"));
  EXPECT_TRUE(contains(rendererFrontendHeader,
                       "processPendingGuiModelLoadRequest"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "processPendingGuiModelLoadRequest();"));

  const size_t presentSceneControls =
      rendererFrontend.find("void RendererFrontend::presentSceneControls()");
  ASSERT_NE(presentSceneControls, std::string::npos);
  const size_t processRequest = rendererFrontend.find(
      "processPendingGuiModelLoadRequest();", presentSceneControls);
  const size_t startFrame =
      rendererFrontend.find("subs_.guiManager->startFrame();",
                            presentSceneControls);
  ASSERT_NE(processRequest, std::string::npos);
  ASSERT_NE(startFrame, std::string::npos);
  EXPECT_LT(processRequest, startFrame);
}

TEST(TechniqueDebugModelGuardrails,
     GuiFrameLifecycleCanRecoverWhenFramePreparationThrows) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiSource =
      readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(guiHeader, "void endFrame()"));
  EXPECT_TRUE(contains(guiHeader, "bool imguiFrameOpen_"));
  EXPECT_TRUE(contains(guiSource, "void GuiManager::endFrame()"));
  EXPECT_TRUE(contains(guiSource, "ImGui::EndFrame()"));
  EXPECT_TRUE(contains(guiSource, "imguiFrameOpen_ = true"));
  EXPECT_TRUE(contains(guiSource, "imguiFrameOpen_ = false"));
  EXPECT_TRUE(contains(rendererFrontend, "GuiFrameExceptionGuard"));
  EXPECT_TRUE(contains(rendererFrontend, "std::uncaught_exceptions()"));
  EXPECT_TRUE(contains(rendererFrontend, "gui_.endFrame();"));
}

TEST(TechniqueDebugModelGuardrails,
     BimModelLoadsResetCameraUsingAuxiliaryBounds) {
  const std::string cameraHeader =
      readRepoTextFile("include/Container/renderer/scene/CameraController.h");
  const std::string cameraSource =
      readRepoTextFile("src/renderer/scene/CameraController.cpp");
  const std::string rendererFrontendHeader =
      readRepoTextFile("include/Container/renderer/core/RendererFrontend.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(cameraHeader, "struct SceneViewBounds"));
  EXPECT_TRUE(contains(cameraHeader, "resetCameraForBounds"));
  EXPECT_TRUE(contains(cameraSource,
                       "void CameraController::resetCameraForBounds"));
  EXPECT_TRUE(contains(rendererFrontendHeader, "resetCameraForActiveScene"));
  EXPECT_TRUE(contains(rendererFrontend, "cameraSceneBoundsFromActiveContent"));
  EXPECT_TRUE(
      contains(rendererFrontend, "sceneProviderBoundsFromBim(*bimManager)"));

  const size_t refreshSceneState =
      rendererFrontend.find("auto refreshSceneState =");
  ASSERT_NE(refreshSceneState, std::string::npos);
  const size_t resetActiveScene =
      rendererFrontend.find("resetCameraForActiveScene();", refreshSceneState);
  ASSERT_NE(resetActiveScene, std::string::npos);
  const size_t updateCameraBuffer =
      rendererFrontend.find("updateCameraBuffer(imageIndex);",
                            resetActiveScene);
  ASSERT_NE(updateCameraBuffer, std::string::npos);
  EXPECT_LT(resetActiveScene, updateCameraBuffer);
}
