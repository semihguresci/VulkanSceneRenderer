#include "Container/renderer/deferred/DeferredRasterTechnique.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/TechniqueDebugModel.h"
#include "Container/renderer/debug/DebugUiPresenter.h"
#include "Container/renderer/forward/ForwardRasterTechnique.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneGraph.h"
#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

// These tests execute the actual widgets without a platform or GPU backend.
class SceneControlsWidgetsTests : public ::testing::Test {
 protected:
  void SetUp() override {
    previousContext_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previousContext_);
  }

  void frame(const std::function<void()>& widget) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(630.0f, 470.0f));
    ImGui::Begin("Scene controls test", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    widget();
    ImGui::End();
    ImGui::Render();
  }

  ImGuiTable* tableWithColumns(int columns) {
    for (int index = 0; index < context_->Tables.GetMapSize(); ++index) {
      auto* table = context_->Tables.TryGetMapData(index);
      if (table && table->ColumnsCount == columns) return table;
    }
    return nullptr;
  }

  ImGuiWindow* childContaining(std::string_view name) {
    for (auto* window : context_->Windows) {
      if ((window->Flags & ImGuiWindowFlags_ChildWindow) &&
          std::string_view(window->Name).find(name) != std::string_view::npos)
        return window;
    }
    return nullptr;
  }

  void click(const std::function<void()>& widget, ImVec2 position) {
    ImGui::GetIO().AddMousePosEvent(position.x, position.y);
    frame(widget);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(widget);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(widget);
  }

  ImGuiContext* context_{};
  ImGuiContext* previousContext_{};
};

}  // namespace

TEST(GuiPanelLayoutTests, RestoredOffscreenWindowFitsCurrentWorkArea) {
  const auto bounds = container::ui::constrainGuiPanelToWorkArea(
      {0.0f, 0.0f}, {1280.0f, 720.0f},
      {{2673.0f, 537.0f}, {442.0f, 1580.0f}});
  EXPECT_EQ(bounds.size, glm::vec2(442.0f, 704.0f));
  EXPECT_EQ(bounds.position, glm::vec2(830.0f, 8.0f));
}

TEST(GuiPanelLayoutTests, ValidUserPositionAndSizeArePreserved) {
  const container::ui::GuiPanelBounds original{{420.0f, 130.0f},
                                               {460.0f, 370.0f}};
  const auto bounds = container::ui::constrainGuiPanelToWorkArea(
      {200.0f, 100.0f}, {1000.0f, 700.0f}, original);
  EXPECT_EQ(bounds.position, original.position);
  EXPECT_EQ(bounds.size, original.size);
}

TEST(GuiPanelLayoutTests, SmallViewportAndNonfiniteSavedBoundsStayFinite) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const auto bounds = container::ui::constrainGuiPanelToWorkArea(
      {40.0f, 90.0f}, {100.0f, 80.0f}, {{nan, nan}, {nan, nan}});
  EXPECT_EQ(bounds.position, glm::vec2(48.0f, 98.0f));
  EXPECT_EQ(bounds.size, glm::vec2(84.0f, 64.0f));
  const auto tiny = container::ui::constrainGuiPanelToWorkArea(
      {40.0f, 90.0f}, {1.0f, 1.0f}, {{-20.0f, -90.0f}, {500.0f, 700.0f}});
  EXPECT_EQ(tiny.position, glm::vec2(40.0f, 90.0f));
  EXPECT_EQ(tiny.size, glm::vec2(1.0f));
}

TEST_F(SceneControlsWidgetsTests, HierarchyFinalNodeIsReachableAndSelectable) {
  container::scene::SceneGraph scene;
  constexpr uint32_t count = 300;
  for (uint32_t index = 0; index < count; ++index)
    scene.createNode(glm::mat4(1.0f), 0, false,
                     container::scene::SceneGraph::kInvalidNode,
                     "Hierarchy node " + std::to_string(index));
  container::ui::GuiManager gui;
  uint32_t selected = container::scene::SceneGraph::kInvalidNode;
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawSceneHierarchy(scene, container::scene::SceneGraph::kInvalidNode,
        selected, [&](uint32_t node) { selected = node; }, [](uint32_t) {},
        [](uint32_t, bool) {}, [](uint32_t, std::optional<uint32_t>) {});
  };
  frame(draw);
  frame(draw);
  auto* table = tableWithColumns(4);
  ASSERT_NE(table, nullptr);
  ASSERT_NE(table->InnerWindow, nullptr);
  EXPECT_LE(table->InnerWindow->Size.y, 300.0f);
  EXPECT_GT(table->InnerWindow->ScrollMax.y, 3000.0f);
  EXPECT_GT(table->InnerWindow->ScrollMax.x, 0.0f);
  EXPECT_EQ(table->FreezeRowsRequest, 1);
  ImGui::SetScrollY(table->InnerWindow, table->InnerWindow->ScrollMax.y);
  frame(draw);
  frame(draw);
  EXPECT_NEAR(table->InnerWindow->Scroll.y,
              table->InnerWindow->ScrollMax.y, 1.0f);
  const float finalRowY = (table->RowPosY1 + table->RowPosY2) * 0.5f;
  ASSERT_LT(finalRowY, table->InnerWindow->InnerRect.Max.y);
  ASSERT_GT(finalRowY, table->InnerWindow->InnerRect.Min.y);
  click(draw, ImVec2(table->Columns[0].WorkMinX + 40.0f, finalRowY));
  EXPECT_EQ(selected, count - 1);
}

TEST_F(SceneControlsWidgetsTests, LargeModelCatalogScrollsToLastEntry) {
  std::vector<container::ui::SampleModelOption> models;
  for (size_t index = 0; index < 300; ++index)
    models.push_back({"Model " + std::to_string(index),
                      "models/" + std::to_string(index) + ".glb"});
  std::string filter;
  std::optional<size_t> chosen;
  const auto draw = [&] {
    if (auto selection = container::ui::drawSampleModelList(models, filter, -1))
      chosen = selection;
  };
  frame(draw);
  frame(draw);
  auto* list = childContaining("SampleModelList");
  ASSERT_NE(list, nullptr);
  EXPECT_LE(list->Size.y, 240.0f);
  EXPECT_GT(list->ScrollMax.y, 3000.0f);
  ImGui::SetScrollY(list, list->ScrollMax.y);
  frame(draw);
  frame(draw);
  const float y = list->DC.CursorMaxPos.y - ImGui::GetTextLineHeight() * 0.5f;
  ASSERT_LT(y, list->InnerRect.Max.y);
  ASSERT_GT(y, list->InnerRect.Min.y);
  click(draw, ImVec2(list->InnerRect.Min.x + 40.0f, y));
  ASSERT_TRUE(chosen);
  EXPECT_EQ(*chosen, models.size() - 1);
}

TEST_F(SceneControlsWidgetsTests, ModelPathFilterReturnsOriginalCatalogIndex) {
  const std::vector<container::ui::SampleModelOption> models{
      {"Same label", "models/first.glb"},
      {"Same label", "models/second.ifc"},
      {"Same label", "models/final.usda"}};
  std::string filter = "FINAL.USDA";
  std::optional<size_t> chosen;
  const auto draw = [&] {
    if (auto selection = container::ui::drawSampleModelList(models, filter, -1))
      chosen = selection;
  };
  frame(draw);
  frame(draw);
  auto* list = childContaining("SampleModelList");
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->ScrollMax.y, 0.0f);
  click(draw, ImVec2(list->InnerRect.Min.x + 40.0f,
                      list->DC.CursorMaxPos.y - ImGui::GetTextLineHeight() * 0.5f));
  ASSERT_TRUE(chosen);
  EXPECT_EQ(*chosen, 2u);
}

TEST_F(SceneControlsWidgetsTests, BimBrowserFinalElementCanBeSelectedAndFocused) {
  std::vector<container::renderer::BimElementMetadata> elements(300);
  for (size_t index = 0; index < elements.size(); ++index) {
    auto& element = elements[index];
    element.objectIndex = static_cast<uint32_t>(index * 2 + 10);
    element.guid = "Bim element " + std::to_string(index);
    element.displayName = "Element name " + std::to_string(index);
    element.type = "IfcWall";
    element.storeyName = "Upper floor";
    element.materialName = "Concrete";
  }
  uint32_t selected = std::numeric_limits<uint32_t>::max();
  uint32_t focused = std::numeric_limits<uint32_t>::max();
  container::ui::BimInspectionState inspection;
  inspection.hasScene = true;
  inspection.elements = elements;
  inspection.selectObject = [&](uint32_t index) { selected = index; };
  inspection.focusObject = [&](uint32_t index) { focused = index; };
  container::ui::GuiManager gui;
  const auto draw = [&] {
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    gui.drawBimElementBrowser(inspection);
  };
  frame(draw);
  frame(draw);
  auto* table = tableWithColumns(7);
  ASSERT_NE(table, nullptr);
  EXPECT_LE(table->InnerWindow->Size.y, 300.0f);
  EXPECT_GT(table->InnerWindow->ScrollMax.y, 3000.0f);
  EXPECT_GT(table->InnerWindow->ScrollMax.x, 0.0f);
  EXPECT_EQ(table->FreezeRowsRequest, 1);
  EXPECT_EQ(table->FreezeColumnsRequest, 2);
  ImGui::SetScrollY(table->InnerWindow, table->InnerWindow->ScrollMax.y);
  frame(draw);
  frame(draw);
  const float y = (table->RowPosY1 + table->RowPosY2) * 0.5f;
  ASSERT_GT(y, table->InnerWindow->InnerRect.Min.y);
  ASSERT_LT(y, table->InnerWindow->InnerRect.Max.y);
  click(draw, ImVec2(table->Columns[0].WorkMinX + 40.0f, y));
  EXPECT_EQ(selected, elements.back().objectIndex);
  click(draw, ImVec2(table->Columns[1].WorkMinX + 20.0f, y));
  EXPECT_EQ(focused, elements.back().objectIndex);
}

TEST(ModelRequestTests, UnloadIsExplicitDeferredAndConsumedOnlyOnce) {
  container::ui::GuiManager gui;
  gui.queueModelLoadRequest("models/model.ifc", 10.0f, "BIM model");
  gui.queueModelUnloadRequest();
  const auto request = gui.consumeModelLoadRequest();
  ASSERT_TRUE(request);
  EXPECT_TRUE(request->unload);
  EXPECT_TRUE(request->path.empty());
  EXPECT_FALSE(gui.consumeModelLoadRequest());
  gui.queueModelLoadRequest("models/model.glb", 1.0f, "glTF model");
  const auto load = gui.consumeModelLoadRequest();
  ASSERT_TRUE(load);
  EXPECT_FALSE(load->unload);
  EXPECT_EQ(load->path, "models/model.glb");
}

TEST(ModelRequestTests, InspectionResetClearsOldVisibilityAndPreservesRendering) {
  container::ui::GuiManager gui;
  auto& filter = const_cast<container::ui::BimFilterState&>(gui.bimFilterState());
  filter.isolateSelection = filter.hideSelection = filter.storeyFilterEnabled = true;
  filter.storey = "Old model storey";
  auto& timeline = const_cast<container::ui::BimPhaseTimelineUiState&>(
      gui.bimPhaseTimelineUiState());
  timeline.enabled = true;
  timeline.activePhaseIndex = 12;
  container::ui::SectionPlaneState section;
  section.enabled = true;
  gui.setSectionPlaneState(section);
  container::renderer::DdgiSettings ddgi;
  ddgi.enabled = true;
  ddgi.intensity = 3.0f;
  ASSERT_TRUE(gui.trySetDdgiSettings(ddgi));
  gui.setGBufferViewMode(container::ui::GBufferViewMode::Normals);
  gui.resetModelInspectionState();
  EXPECT_FALSE(gui.bimFilterState().isolateSelection);
  EXPECT_FALSE(gui.bimFilterState().hideSelection);
  EXPECT_FALSE(gui.bimFilterState().storeyFilterEnabled);
  EXPECT_TRUE(gui.bimFilterState().storey.empty());
  EXPECT_FALSE(gui.bimPhaseTimelineUiState().enabled);
  EXPECT_EQ(gui.bimPhaseTimelineUiState().activePhaseIndex, 0u);
  EXPECT_FALSE(gui.sectionPlaneState().enabled);
  EXPECT_EQ(gui.ddgiSettings(), ddgi);
  EXPECT_EQ(gui.gBufferViewMode(), container::ui::GBufferViewMode::Normals);
}

TEST(DdgiGuiControlsTests, PauseRetainsConfiguredBudgetsAcrossFrameSync) {
  container::ui::GuiManager guiManager;
  container::renderer::DdgiSettings settings;
  settings.enabled = true;
  settings.resources.updates.maxProbesPerFrame = 37;
  settings.resources.updates.maxRaysPerFrame = 4096;
  guiManager.setDdgiState(settings, {.supported = true, .active = true});
  auto paused = guiManager.ddgiSettings();
  paused.paused = true;
  ASSERT_TRUE(guiManager.trySetDdgiSettings(paused));
  guiManager.setDdgiState(paused, {.supported = true, .active = true});
  EXPECT_TRUE(guiManager.ddgiSettings().paused);
  EXPECT_EQ(guiManager.ddgiSettings().resources.updates.maxProbesPerFrame, 37u);
  EXPECT_EQ(guiManager.ddgiSettings().resources.updates.maxRaysPerFrame, 4096u);
  auto resumed = guiManager.ddgiSettings();
  resumed.paused = false;
  ASSERT_TRUE(guiManager.trySetDdgiSettings(resumed));
  EXPECT_EQ(guiManager.ddgiSettings().resources.updates.maxProbesPerFrame, 37u);
  EXPECT_EQ(guiManager.ddgiSettings().resources.updates.maxRaysPerFrame, 4096u);
}

TEST(DdgiGuiControlsTests, DetailVolumesAreCenteredAndBoundedWithoutEnablingGi) {
  container::ui::GuiManager gui;
  const auto primary = gui.ddgiSettings().volume;
  ASSERT_TRUE(gui.addDdgiDetailVolume());
  const auto detail = gui.ddgiSettings().additionalVolumes.front();
  EXPECT_EQ(detail.spacing, primary.spacing * .5f);
  EXPECT_EQ(detail.origin, primary.origin + primary.spacing *
      glm::vec3(primary.counts - glm::uvec3(1)) * .25f);
  EXPECT_FALSE(gui.ddgiSettings().enabled);
  ASSERT_TRUE(gui.addDdgiDetailVolume());
  ASSERT_TRUE(gui.addDdgiDetailVolume());
  const auto accepted = gui.ddgiSettings();
  EXPECT_FALSE(gui.addDdgiDetailVolume());
  EXPECT_EQ(gui.ddgiSettings(), accepted);
  EXPECT_FALSE(gui.removeDdgiDetailVolume(0));
  EXPECT_FALSE(gui.removeDdgiDetailVolume(4));
  ASSERT_TRUE(gui.removeDdgiDetailVolume(2));
  EXPECT_EQ(gui.ddgiSettings().additionalVolumes.size(), 2);
  EXPECT_EQ(gui.ddgiSettings().volume, primary);
}

TEST(DdgiGuiControlsTests, InvalidDetailVolumePreservesTheAcceptedSet) {
  container::ui::GuiManager gui;
  ASSERT_TRUE(gui.addDdgiDetailVolume());
  const auto accepted = gui.ddgiSettings();
  auto invalid = accepted;
  invalid.additionalVolumes[0].spacing.x = 0;
  EXPECT_FALSE(gui.trySetDdgiSettings(invalid));
  EXPECT_EQ(gui.ddgiSettings(), accepted);
  EXPECT_FALSE(gui.selectDdgiVolume(2));
  EXPECT_TRUE(gui.selectDdgiVolume(1));
}

TEST(DdgiGuiControlsTests, PresetRequestsAreOneShotAndDoNotMutateLighting) {
  container::ui::GuiManager gui;
  const auto accepted = gui.ddgiSettings();
  using Action = container::ui::GuiManager::DdgiPresetAction;
  EXPECT_FALSE(gui.consumeDdgiPresetRequest());
  gui.requestDdgiPreset(Action::Save);
  EXPECT_EQ(gui.consumeDdgiPresetRequest(), Action::Save);
  EXPECT_FALSE(gui.consumeDdgiPresetRequest());
  gui.requestDdgiPreset(Action::Load);
  EXPECT_EQ(gui.consumeDdgiPresetRequest(), Action::Load);
  EXPECT_EQ(gui.ddgiSettings(), accepted);
}

TEST(DdgiGuiControlsTests, DetailFitRequestRetainsLightingAndConsumesOnlyOnce) {
  container::ui::GuiManager gui;
  container::renderer::DdgiSettings settings;
  gui.setDdgiState(settings, {.supported = true});
  EXPECT_FALSE(gui.requestDdgiDetailFit(26));
  EXPECT_FALSE(gui.consumeDdgiDetailFitRequest());
  ASSERT_TRUE(gui.requestDdgiDetailFit(512));
  EXPECT_EQ(gui.consumeDdgiDetailFitRequest(), 512u);
  EXPECT_FALSE(gui.consumeDdgiDetailFitRequest());
  EXPECT_EQ(gui.ddgiSettings(), settings);
  for (int index = 0; index < 3; ++index) ASSERT_TRUE(gui.addDdgiDetailVolume());
  EXPECT_FALSE(gui.requestDdgiDetailFit(512));
  EXPECT_FALSE(gui.consumeDdgiDetailFitRequest());
}

TEST(DdgiGuiControlsTests, ExtendedTransportAndSchedulingSurviveFrameSynchronization) {
  container::ui::GuiManager gui;
  container::renderer::DdgiSettings settings;
  settings.dielectricTransmission = settings.cameraFollow = settings.asyncCompute = true;
  ASSERT_TRUE(gui.trySetDdgiSettings(settings));
  gui.setDdgiState(settings, {.supported = true});
  EXPECT_EQ(gui.ddgiSettings(), settings);
  EXPECT_FALSE(gui.ddgiSettings().enabled);
}

TEST(DdgiGuiControlsTests, RejectedNewDetailCannotReplaceThePrimaryDraft) {
  container::ui::GuiManager gui;
  const auto accepted = gui.ddgiSettings();
  ASSERT_TRUE(gui.addDdgiDetailVolume());
  gui.rejectDdgiSettings(accepted, "owner refused detail allocation");
  gui.setDdgiState(accepted, {});
  EXPECT_EQ(gui.ddgiSettings(), accepted);
  ASSERT_TRUE(gui.addDdgiDetailVolume());
  EXPECT_EQ(gui.ddgiSettings().volume, accepted.volume);
  EXPECT_EQ(gui.ddgiSettings().additionalVolumes[0].spacing, accepted.volume.spacing * .5f);
}

TEST(DdgiGuiControlsTests, InvalidVolumeEditPreservesWorkingLighting) {
  container::ui::GuiManager guiManager;
  container::renderer::DdgiSettings settings;
  settings.enabled = true;
  settings.intensity = 2.f;
  guiManager.setDdgiState(settings, {.supported = true, .active = true});
  auto edit = settings;
  edit.volume.spacing.y = 0.f;
  EXPECT_FALSE(guiManager.trySetDdgiSettings(edit));
  EXPECT_TRUE(guiManager.ddgiSettings().enabled);
  EXPECT_EQ(guiManager.ddgiSettings().intensity, 2.f);
  EXPECT_EQ(guiManager.ddgiSettings().volume.spacing.y,
            settings.volume.spacing.y);
  EXPECT_FALSE(guiManager.ddgiControlError().empty());
  edit.volume.spacing.y = .25f;
  ASSERT_TRUE(guiManager.trySetDdgiSettings(edit));
  EXPECT_EQ(guiManager.ddgiSettings().volume.spacing.y, .25f);
  EXPECT_TRUE(guiManager.ddgiControlError().empty());
}

TEST(DdgiGuiControlsTests, RejectsNonfiniteImmediateEditAndInvalidRayQuality) {
  container::ui::GuiManager guiManager;
  container::renderer::DdgiSettings settings;
  guiManager.setDdgiState(settings, {});
  auto edit = settings;
  edit.intensity = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(guiManager.trySetDdgiSettings(edit));
  EXPECT_EQ(guiManager.ddgiSettings().intensity, settings.intensity);
  edit = settings;
  edit.resources.updates.raysPerProbe = 0;
  EXPECT_FALSE(guiManager.trySetDdgiSettings(edit));
  EXPECT_EQ(guiManager.ddgiSettings().resources.updates.raysPerProbe,
            settings.resources.updates.raysPerProbe);
}

TEST(DdgiGuiControlsTests,
     ReportsResourceBudgetRejectionWithoutPublishingEdit) {
  container::ui::GuiManager guiManager;
  container::renderer::DdgiSettings settings;
  guiManager.setDdgiState(settings, {.supported = true});
  auto edit = settings;
  edit.volume.counts = {1000, 1000, 1000};
  EXPECT_FALSE(guiManager.trySetDdgiSettings(edit));
  EXPECT_EQ(guiManager.ddgiSettings().volume.counts.x,
            settings.volume.counts.x);
  EXPECT_FALSE(guiManager.ddgiControlError().empty());
}

TEST(DdgiGuiControlsTests, OwnerRejectionRestoresTheAcceptedControlState) {
  container::ui::GuiManager guiManager;
  container::renderer::DdgiSettings accepted;
  accepted.enabled = true;
  guiManager.setDdgiState(accepted, {.supported = true, .active = true});
  auto candidate = accepted;
  candidate.volume.origin.x = -2.f;
  candidate.resources.updates.raysPerProbe = 256;
  ASSERT_TRUE(guiManager.trySetDdgiSettings(candidate));
  guiManager.rejectDdgiSettings(accepted, "test owner rejected volume");
  EXPECT_EQ(guiManager.ddgiSettings(), accepted);
  EXPECT_EQ(guiManager.ddgiControlError(), "test owner rejected volume");
  guiManager.setDdgiState(accepted, {.supported = true, .active = true});
  EXPECT_EQ(guiManager.ddgiSettings(), accepted);
  EXPECT_EQ(guiManager.ddgiControlError(), "test owner rejected volume");
}

TEST(DdgiGuiControlsTests, SceneFitRequestIsOneShotAndDoesNotEnableOrEditGi) {
  container::ui::GuiManager gui;
  container::renderer::DdgiSettings settings;
  settings.paused = true;
  settings.intensity = 2.f;
  gui.setDdgiState(settings, {.supported = true});
  EXPECT_FALSE(gui.requestDdgiSceneFit(26));
  EXPECT_FALSE(gui.consumeDdgiSceneFitRequest());
  EXPECT_FALSE(gui.ddgiControlError().empty());
  ASSERT_TRUE(gui.requestDdgiSceneFit(1024));
  EXPECT_EQ(gui.ddgiSettings(), settings);
  EXPECT_EQ(gui.consumeDdgiSceneFitRequest(), 1024u);
  EXPECT_FALSE(gui.consumeDdgiSceneFitRequest());
  EXPECT_TRUE(gui.ddgiControlError().empty());
}

TEST(DdgiGuiControlsTests, AcceptedSceneFitPersistsAndFailureKeepsWorkingVolume) {
  container::ui::GuiManager gui;
  container::renderer::DdgiSettings settings;
  settings.enabled = true;
  settings.volume.origin = {-5, -3, -8};
  settings.volume.spacing = {1, 1, 1};
  gui.acceptDdgiSceneFit(settings, "Fitted scene");
  gui.setDdgiState(settings, {.supported = true, .active = true});
  EXPECT_EQ(gui.ddgiSettings(), settings);
  ASSERT_TRUE(gui.requestDdgiSceneFit(2048));
  EXPECT_EQ(gui.consumeDdgiSceneFitRequest(), 2048u);
  gui.rejectDdgiSettings(settings, "No visible triangle geometry to fit");
  EXPECT_EQ(gui.ddgiSettings(), settings);
  gui.setDdgiState(settings, {.supported = true, .active = true});
  EXPECT_EQ(gui.ddgiSettings(), settings);
  EXPECT_EQ(gui.ddgiControlError(), "No visible triangle geometry to fit");
}

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

  const size_t drawFrame = rendererFrontend.find(
      "bool RendererFrontend::drawFrame(bool &framebufferResized)");
  ASSERT_NE(drawFrame, std::string::npos);
  const size_t drawFrameEnd = rendererFrontend.find("\n}", drawFrame);
  ASSERT_NE(drawFrameEnd, std::string::npos);
  const std::string drawFrameBody =
      rendererFrontend.substr(drawFrame, drawFrameEnd - drawFrame);
  const size_t processRequest =
      drawFrameBody.find("processPendingGuiModelLoadRequest();");
  const size_t acquireImage = drawFrameBody.find("vkAcquireNextImageKHR(");
  const size_t drawControls = drawFrameBody.find("presentSceneControls();");
  ASSERT_NE(processRequest, std::string::npos);
  ASSERT_NE(acquireImage, std::string::npos);
  ASSERT_NE(drawControls, std::string::npos);
  // Loading views own their acquire/submit/present cycle. Commit must precede
  // the regular frame's acquire as well as the scene controls' ImGui frame.
  EXPECT_LT(processRequest, acquireImage);
  EXPECT_LT(acquireImage, drawControls);
  EXPECT_EQ(drawFrameBody.find("processPendingGuiModelLoadRequest();",
                              processRequest + 1),
            std::string::npos);

  const size_t presentSceneControls =
      rendererFrontend.find("void RendererFrontend::presentSceneControls()");
  ASSERT_NE(presentSceneControls, std::string::npos);
  const size_t sceneControlsEnd =
      rendererFrontend.find("\n}", presentSceneControls);
  ASSERT_NE(sceneControlsEnd, std::string::npos);
  const std::string sceneControlsBody = rendererFrontend.substr(
      presentSceneControls, sceneControlsEnd - presentSceneControls);
  EXPECT_FALSE(contains(sceneControlsBody, "processPendingGuiModelLoadRequest"));
  EXPECT_FALSE(contains(sceneControlsBody, "reloadSceneModel("));
  const size_t startFrame =
      sceneControlsBody.find("subs_.guiManager->startFrame();");
  ASSERT_NE(startFrame, std::string::npos);
  const size_t publishLighting =
      sceneControlsBody.find("subs_.guiManager->setLightingSettings(");
  const size_t publishBloom =
      sceneControlsBody.find("subs_.guiManager->setBloomSettings(");
  ASSERT_NE(publishLighting, std::string::npos);
  ASSERT_NE(publishBloom, std::string::npos);
  EXPECT_LT(publishLighting, startFrame);
  EXPECT_LT(publishBloom, startFrame);
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
